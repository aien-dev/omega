/*
 * One persistent graphics seat. It is launched once, watches the same image
 * the processor uses, and leaves when it sees a shutdown notice. It does not
 * export a function for the processor to call.
 */
#include "rx_resident_gpu.h"

#include "omega_blackwell_codegen.h"
#include "omega_blackwell_qmd.h"
#include "omega_blackwell_submit.h"
#include "m16_native.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LDG_CTL 0x001ea800u
#define CRC_POLY 0x82F63B78u
/* Safety stop if shutdown is never posted. A real shutdown leaves sooner.
 * The launch wait is 15 seconds, so this only matters when shutdown is missed. */
#define GIVEUP 200000000u

typedef struct __attribute__((packed)) {
    uint64_t region;
    uint32_t seat_gen;      /* the only claim generation this seat takes */
    uint32_t epoch;
    uint32_t off_c2g;
    uint32_t off_g2c;
    uint32_t off_hb;
    uint32_t off_tbl;
    uint32_t giveup;
} SeatArgs;

typedef struct LaunchJob LaunchJob;

struct RxGpuSeat {
    M16NativeContext ctx;
    pthread_t thread;
    int thread_live;
    int opened;
    int killed;             /* channel destroyed under the running seat */
    volatile int abort;     /* stop waiting for the launch marker */
    volatile int marker_ok;
    volatile int sem_ok;
    char err[160];
    LaunchJob *job;
    volatile uint32_t *hb_watch;
    RxWorld *world;
    int image_bound;
};

/* The chip is leaving. The world keeps its objects: copy the image into
 * memory the world owns before the chip memory goes away. A chip reset or
 * exit must not take canonical identity with it. */
static int seat_return_image(RxGpuSeat *s) {
    if (!s->image_bound) return 0;
    RxWorld *w = s->world;
    uint64_t bytes = w->coherent_bytes;
    void *own = malloc((size_t)bytes);
    if (!own) return -1;
    if (rx_world_bind_coherent(w, own, bytes, 0) != RX_OK) {
        free(own);
        return -1;
    }
    s->image_bound = 0;
    return 0;
}

typedef struct {
    int base, addr, hb_ptr, c2g, g2c, slot, dst, win;
    int tid, one, zero, ones, give, hb, head, tail;
    int tmp, tmp2, msg, obj, gen, seq, f0, f1;
    int crc, poly, word, byte, nbit, wi;
    int fifteen, thirtytwo, off_c2g, off_g2c, off_hb, off_tbl, epoch;
    int off, slot_off, dst_off;
    int win_b, lim, sgen, clk;
    int uv;
} Regs;

static uint32_t crc_bitwise(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++)
            c = (c & 1u) ? (CRC_POLY ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static int crc_matches_sealer(void) {
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = RX_RING_CLAIM;
    d.object_id = 3;
    d.object_generation = 1;
    d.object_length = 64;
    d.payload_len = 24;
    d.arg_b = 9;
    d.payload[0] = 4;
    d.payload[3] = 1;
    d.payload[16] = 7;
    rx_world_seal_descriptor(&d);
    uint32_t want = d.checksum;
    uint8_t buf[0x3C + 64];
    memcpy(buf, &d, 0x3C);
    memcpy(buf + 0x3C, d.payload, 64);
    return crc_bitwise(buf, sizeof(buf)) == want;
}

static BlackwellIRInsn op(BlackwellIROpcode code) {
    BlackwellIRInsn n;
    memset(&n, 0, sizeof(n));
    n.op = code;
    n.dst_vreg = n.src1_vreg = n.src2_vreg = n.src3_vreg = n.ureg = -1;
    return n;
}

/* S2R special register %globaltimer, low 32 bits (ns). Kept here so the
 * hash-pinned codegen header stays untouched. */
#define RX_SR_GLOBALTIMER_LO 0x52

static int emit_overflow;

static int em(BlackwellIRProgram *p, BlackwellIRInsn n) {
    int idx = omega_bw_ir_append(p, &n);
    if (idx < 0) {
        emit_overflow = 1;
        return -1;
    }
    return idx;
}

static int bra(BlackwellIRProgram *p, int pred, int neg) {
    BlackwellIRInsn n = op(BW_IR_BRA);
    n.predicate_p0 = pred ? true : false;
    n.predicate_not = neg ? true : false;
    return em(p, n);
}

static void fix(BlackwellIRProgram *p, int at, int target) {
    if (at >= 0 && target >= 0) p->insns[at].imm = (uint32_t)(target - at);
}

static void movi(BlackwellIRProgram *p, int dst, uint32_t imm) {
    BlackwellIRInsn n = op(BW_IR_MOV_IMM);
    n.dst_vreg = dst;
    n.imm = imm;
    em(p, n);
}

static void add32(BlackwellIRProgram *p, int dst, int a, int b) {
    BlackwellIRInsn n = op(BW_IR_IADD3);
    n.dst_vreg = dst;
    n.src1_vreg = a;
    n.src2_vreg = b;
    em(p, n);
}

static void xor_rr(BlackwellIRProgram *p, int dst, int a, int b) {
    BlackwellIRInsn n = op(BW_IR_LOP3_XOR);
    n.dst_vreg = dst;
    n.src1_vreg = a;
    n.src2_vreg = b;
    em(p, n);
}

static void andi(BlackwellIRProgram *p, int dst, int a, uint32_t imm) {
    BlackwellIRInsn n = op(BW_IR_LOP3_AND);
    n.dst_vreg = dst;
    n.src1_vreg = a;
    n.imm = imm;
    em(p, n);
}

static void shr(BlackwellIRProgram *p, int dst, int a, uint32_t imm) {
    BlackwellIRInsn n = op(BW_IR_SHF_R);
    n.dst_vreg = dst;
    n.src1_vreg = a;
    n.imm = imm;
    em(p, n);
}

static void ge_rr(BlackwellIRProgram *p, int a, int b) {
    BlackwellIRInsn n = op(BW_IR_ISETP_GE_U32);
    n.src1_vreg = a;
    n.src2_vreg = b;
    em(p, n);
}

static void addk(BlackwellIRProgram *p, int dst, int base, int idx, uint32_t scale) {
    BlackwellIRInsn n = op(BW_IR_IMAD_WIDE);
    n.dst_vreg = dst;
    n.src1_vreg = idx;
    n.src3_vreg = base;
    n.imm = scale;
    em(p, n);
}

static void shl_n(BlackwellIRProgram *p, int dst, int src, int n) {
    add32(p, dst, src, src);
    for (int i = 1; i < n; i++) add32(p, dst, dst, dst);
}

/* Byte offset from the original region base. The wide add is only ever
 * immediate 1 onto that base. A second wide add onto a computed pointer
 * drops the high half of the address. */
static void at_off(BlackwellIRProgram *p, const Regs *r, int dst, int base_off, uint32_t extra) {
    if (extra == 0) {
        add32(p, r->off, base_off, r->zero);
    } else {
        movi(p, r->tmp2, extra);
        add32(p, r->off, base_off, r->tmp2);
    }
    addk(p, dst, r->base, r->off, 1u);
}

static void ring_slot(BlackwellIRProgram *p, Regs *r, int dest_off, int index, int ring_off) {
    andi(p, r->tmp, index, 1023u);
    shl_n(p, r->tmp, r->tmp, 7);
    movi(p, r->tmp2, 192u);
    add32(p, r->tmp, r->tmp, r->tmp2);
    add32(p, dest_off, r->tmp, ring_off);
}

/* System-scope strong load: it goes past the chip's own cache, so a value
 * the processor wrote is seen on the next poll. */
static void ld(BlackwellIRProgram *p, int dst, int addr, int uv) {
    BlackwellIRInsn n = op(BW_IR_LDG_STRONG_SYS);
    n.dst_vreg = dst;
    n.src1_vreg = addr;
    n.ureg = uv;
    n.control = LDG_CTL;
    em(p, n);
}

static void st(BlackwellIRProgram *p, int addr, int data, int uv) {
    BlackwellIRInsn n = op(BW_IR_STG_EF);
    n.src1_vreg = addr;
    n.src2_vreg = data;
    n.ureg = uv;
    em(p, n);
}

static void st_ef(BlackwellIRProgram *p, int addr, int data, int uv) {
    BlackwellIRInsn n = op(BW_IR_STG_EF);
    n.src1_vreg = addr;
    n.src2_vreg = data;
    n.ureg = uv;
    em(p, n);
}

static void emit_byte(BlackwellIRProgram *p, const Regs *r) {
    andi(p, r->byte, r->word, 0xffu);
    xor_rr(p, r->crc, r->crc, r->byte);
    movi(p, r->nbit, 8u);
    int bit = (int)p->count;
    andi(p, r->byte, r->crc, 1u);
    shr(p, r->crc, r->crc, 1u);
    ge_rr(p, r->byte, r->one);
    int skip = bra(p, 1, 1);
    xor_rr(p, r->crc, r->crc, r->poly);
    fix(p, skip, (int)p->count);
    add32(p, r->nbit, r->nbit, r->ones);
    ge_rr(p, r->nbit, r->one);
    fix(p, bra(p, 1, 0), bit);
    shr(p, r->word, r->word, 8u);
}

static int build_program(BlackwellIRProgram *prog, uint32_t *gpr_out) {
    emit_overflow = 0;
    omega_bw_ir_init(prog);
    Regs r;
    memset(&r, 0, sizeof(r));
    r.base = omega_bw_ir_alloc_vreg64(prog);
    r.addr = omega_bw_ir_alloc_vreg64(prog);
    r.hb_ptr = omega_bw_ir_alloc_vreg64(prog);
    r.c2g = omega_bw_ir_alloc_vreg64(prog);
    r.g2c = omega_bw_ir_alloc_vreg64(prog);
    r.dst = omega_bw_ir_alloc_vreg64(prog);
    r.uv = omega_bw_ir_alloc_uvreg64(prog);
#define A32(field) r.field = omega_bw_ir_alloc_vreg(prog)
    A32(tid); A32(one); A32(zero); A32(ones); A32(give); A32(hb);
    A32(head); A32(tail); A32(tmp); A32(tmp2); A32(msg); A32(obj); A32(gen);
    A32(seq); A32(f0); A32(f1); A32(crc); A32(poly); A32(word); A32(byte);
    A32(nbit); A32(wi); A32(fifteen); A32(thirtytwo);
    A32(off_c2g); A32(off_g2c); A32(off_hb); A32(off_tbl);
    A32(off); A32(slot_off); A32(dst_off); A32(win_b); A32(lim); A32(sgen); A32(clk);
#undef A32
    if (r.sgen < 0 || r.uv < 0) return -1;

    BlackwellIRInsn s2 = op(BW_IR_S2R);
    s2.dst_vreg = r.tid;
    s2.imm = BW_SR_TID_X;
    em(prog, s2);
    BlackwellIRInsn cmp = op(BW_IR_ISETP_GE);
    cmp.src1_vreg = r.tid;
    cmp.imm = 1;
    em(prog, cmp);
    int to_body = bra(prog, 0, 0);
    em(prog, op(BW_IR_NOP));
    fix(prog, to_body, (int)prog->count);

    BlackwellIRInsn du = op(BW_IR_LDCU64);
    du.dst_vreg = r.uv;
    du.imm = 0x358;
    du.is_uniform = true;
    em(prog, du);
    BlackwellIRInsn pb = op(BW_IR_LDC64);
    pb.dst_vreg = r.base;
    pb.imm = 0x380;
    em(prog, pb);
#define LDC32(reg, off) do { BlackwellIRInsn n = op(BW_IR_LDC); n.dst_vreg = (reg); n.imm = (off); em(prog, n); } while (0)
    LDC32(r.sgen, 0x388);
    LDC32(r.off_c2g, 0x390);
    LDC32(r.off_g2c, 0x394);
    LDC32(r.off_hb, 0x398);
    LDC32(r.off_tbl, 0x39c);
    LDC32(r.give, 0x3a0);
#undef LDC32
    movi(prog, r.one, 1u);
    movi(prog, r.zero, 0u);
    movi(prog, r.ones, 0xffffffffu);
    movi(prog, r.poly, CRC_POLY);
    movi(prog, r.fifteen, 15u);
    movi(prog, r.thirtytwo, 32u);
    /* The seat indexes the world's projected table (RxProjectedTable), which
     * holds RX_MAX_OBJECTS records, not the frozen ABI's 64. With 64 here the
     * chip refused every object id past 63 while the stand-in accepted it. */
    movi(prog, r.lim, RX_MAX_OBJECTS);
    /* The wide multiply-add drops a small offset (256 landed on the header).
     * Copy the region base, then add 256 onto the low half with a plain add. */
    {
        BlackwellIRInsn n = op(BW_IR_LDC64);
        n.dst_vreg = r.c2g;
        n.imm = 0x380;
        em(prog, n);
    }
    movi(prog, r.tmp2, 256u);
    add32(prog, r.c2g, r.c2g, r.tmp2);
    add32(prog, r.c2g, r.c2g, r.zero);
    addk(prog, r.g2c, r.base, r.off_g2c, 1u);

    addk(prog, r.hb_ptr, r.base, r.off_hb, 1u);
    int loop = (int)prog->count;
    em(prog, op(BW_IR_CCTL_IVALL));   /* drop any stale private copy before the check */
    ld(prog, r.tail, r.c2g, r.uv);
    add32(prog, r.tmp, r.tail, r.one);
    st_ef(prog, r.hb_ptr, r.tmp, r.uv);
    add32(prog, r.give, r.give, r.ones);
    /* A count that moves on every pass: the processor can see the seat is
     * alive, and that it has stopped. */
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_LIVE);
    st(prog, r.addr, r.give, r.uv);
    ge_rr(prog, r.give, r.one);
    int give_exit = bra(prog, 1, 1);

    at_off(prog, &r, r.addr, r.off_c2g, 64u);
    ld(prog, r.head, r.addr, r.uv);
    ge_rr(prog, r.head, r.tail);
    fix(prog, bra(prog, 1, 0), loop);

    movi(prog, r.tmp2, 9u);
    at_off(prog, &r, r.addr, r.off_hb, 4u);
    st(prog, r.addr, r.tmp2, r.uv);

    ring_slot(prog, &r, r.slot_off, r.head, r.off_c2g);
    at_off(prog, &r, r.addr, r.slot_off, 4u);
    ld(prog, r.word, r.addr, r.uv);
    shr(prog, r.msg, r.word, 16u);

    movi(prog, r.tmp2, RX_RING_SHUTDOWN);
    xor_rr(prog, r.tmp, r.msg, r.tmp2);
    ge_rr(prog, r.tmp, r.one);
    int not_shutdown = bra(prog, 1, 0);
    add32(prog, r.head, r.head, r.one);
    at_off(prog, &r, r.addr, r.off_c2g, 64u);
    st(prog, r.addr, r.head, r.uv);
    int shutdown_exit = bra(prog, 0, 0);

    fix(prog, not_shutdown, (int)prog->count);
    movi(prog, r.tmp2, RX_RING_CLAIM);
    xor_rr(prog, r.tmp, r.msg, r.tmp2);
    ge_rr(prog, r.tmp, r.one);
    int not_claim = bra(prog, 1, 0);

    /* R15: the chip's own clock at pickup, with the claim it belongs to. */
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_CLAIM);
    st(prog, r.addr, r.head, r.uv);
    {
        BlackwellIRInsn t = op(BW_IR_S2R);
        t.dst_vreg = r.clk;
        t.imm = RX_SR_GLOBALTIMER_LO;
        em(prog, t);
    }
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_T_PICK);
    st(prog, r.addr, r.clk, r.uv);

    /* Input object A: id and generation from the notice header. */
    at_off(prog, &r, r.addr, r.slot_off, 0x18u);
    ld(prog, r.obj, r.addr, r.uv);
    at_off(prog, &r, r.addr, r.slot_off, 0x1cu);
    ld(prog, r.gen, r.addr, r.uv);
    int stale[8];
    int ns = 0;
    /* A claim from another seat generation: posted before a seat was lost. */
    at_off(prog, &r, r.addr, r.slot_off, 0x14u);
    ld(prog, r.f0, r.addr, r.uv);
    xor_rr(prog, r.tmp2, r.f0, r.sgen);
    ge_rr(prog, r.tmp2, r.one);
    stale[ns++] = bra(prog, 1, 0);
    ge_rr(prog, r.obj, r.lim);                   /* id outside the table */
    stale[ns++] = bra(prog, 1, 0);
    shl_n(prog, r.tmp, r.obj, 5);
    add32(prog, r.tmp, r.tmp, r.off_tbl);
    movi(prog, r.tmp2, 64u);
    add32(prog, r.seq, r.tmp, r.tmp2);           /* seq = A's table entry */
    at_off(prog, &r, r.addr, r.seq, 4u);
    ld(prog, r.f0, r.addr, r.uv);
    xor_rr(prog, r.tmp2, r.f0, r.gen);
    ge_rr(prog, r.tmp2, r.one);                  /* generation moved */
    stale[ns++] = bra(prog, 1, 0);
    at_off(prog, &r, r.addr, r.seq, 8u);
    ld(prog, r.f0, r.addr, r.uv);
    xor_rr(prog, r.tmp2, r.f0, r.one);
    ge_rr(prog, r.tmp2, r.one);                  /* not active */
    stale[ns++] = bra(prog, 1, 0);
    at_off(prog, &r, r.addr, r.seq, 16u);
    ld(prog, r.f0, r.addr, r.uv);
    add32(prog, r.seq, r.f0, r.zero);            /* seq = A's window */

    /* Output object B: id and generation from the notice payload. */
    at_off(prog, &r, r.addr, r.slot_off, 0x58u);
    ld(prog, r.obj, r.addr, r.uv);
    at_off(prog, &r, r.addr, r.slot_off, 0x5cu);
    ld(prog, r.gen, r.addr, r.uv);
    ge_rr(prog, r.obj, r.lim);
    stale[ns++] = bra(prog, 1, 0);
    shl_n(prog, r.tmp, r.obj, 5);
    add32(prog, r.tmp, r.tmp, r.off_tbl);
    movi(prog, r.tmp2, 64u);
    add32(prog, r.win_b, r.tmp, r.tmp2);         /* win_b = B's table entry */
    at_off(prog, &r, r.addr, r.win_b, 4u);
    ld(prog, r.f0, r.addr, r.uv);
    xor_rr(prog, r.tmp2, r.f0, r.gen);
    ge_rr(prog, r.tmp2, r.one);
    stale[ns++] = bra(prog, 1, 0);
    at_off(prog, &r, r.addr, r.win_b, 8u);
    ld(prog, r.f0, r.addr, r.uv);
    xor_rr(prog, r.tmp2, r.f0, r.one);
    ge_rr(prog, r.tmp2, r.one);
    stale[ns++] = bra(prog, 1, 0);
    at_off(prog, &r, r.addr, r.win_b, 16u);
    ld(prog, r.f0, r.addr, r.uv);
    add32(prog, r.win_b, r.f0, r.zero);          /* win_b = B's window */

    /* The body: the qualified 32-bit integer add, A.field0 + A.field1. */
    at_off(prog, &r, r.addr, r.seq, 0u);
    ld(prog, r.f0, r.addr, r.uv);
    at_off(prog, &r, r.addr, r.seq, 8u);
    ld(prog, r.f1, r.addr, r.uv);
    add32(prog, r.f1, r.f0, r.f1);
    at_off(prog, &r, r.addr, r.win_b, 0u);
    st(prog, r.addr, r.f1, r.uv);
    at_off(prog, &r, r.addr, r.win_b, 4u);
    st(prog, r.addr, r.zero, r.uv);
    movi(prog, r.msg, 1u | (RX_RING_PUBLISH << 16));
    int skip_fault_msg = bra(prog, 0, 0);
    for (int i = 0; i < ns; i++) fix(prog, stale[i], (int)prog->count);
    movi(prog, r.msg, 1u | (RX_RING_FAULT << 16));
    fix(prog, skip_fault_msg, (int)prog->count);

    /* Fault-injection hold. The seat marks which claim it holds, then waits
     * while the processor keeps the hold word set: B's window may already
     * hold the sum and no result is posted. The safety stop still counts. */
    add32(prog, r.tmp, r.head, r.one);
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_HELD);
    st(prog, r.addr, r.tmp, r.uv);
    int hold = (int)prog->count;
    em(prog, op(BW_IR_CCTL_IVALL));
    add32(prog, r.give, r.give, r.ones);
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_LIVE);
    st(prog, r.addr, r.give, r.uv);
    ge_rr(prog, r.give, r.one);
    int hold_exit = bra(prog, 1, 1);
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_HOLD);
    ld(prog, r.tmp, r.addr, r.uv);
    ge_rr(prog, r.tmp, r.one);
    fix(prog, bra(prog, 1, 0), hold);

    at_off(prog, &r, r.g2c, r.off_g2c, 0u);
    ld(prog, r.tail, r.g2c, r.uv);
    ring_slot(prog, &r, r.dst_off, r.tail, r.off_g2c);
    movi(prog, r.wi, 0u);
    int wloop = (int)prog->count;
    ge_rr(prog, r.wi, r.thirtytwo);
    int wdone = bra(prog, 1, 0);
    add32(prog, r.nbit, r.wi, r.wi);
    add32(prog, r.nbit, r.nbit, r.nbit);
    add32(prog, r.off, r.slot_off, r.nbit);
    addk(prog, r.addr, r.base, r.off, 1u);
    ld(prog, r.word, r.addr, r.uv);
    add32(prog, r.tmp, r.word, r.zero);
    xor_rr(prog, r.tmp2, r.wi, r.one);
    ge_rr(prog, r.tmp2, r.one);
    int keep_word = bra(prog, 1, 0);
    add32(prog, r.tmp, r.msg, r.zero);
    fix(prog, keep_word, (int)prog->count);
    xor_rr(prog, r.tmp2, r.wi, r.fifteen);
    ge_rr(prog, r.tmp2, r.one);
    int keep_sum = bra(prog, 1, 0);
    add32(prog, r.tmp, r.zero, r.zero);
    fix(prog, keep_sum, (int)prog->count);
    add32(prog, r.off, r.dst_off, r.nbit);
    addk(prog, r.addr, r.base, r.off, 1u);
    st(prog, r.addr, r.tmp, r.uv);
    add32(prog, r.wi, r.wi, r.one);
    fix(prog, bra(prog, 0, 0), wloop);
    fix(prog, wdone, (int)prog->count);

    movi(prog, r.crc, 0xffffffffu);
    movi(prog, r.wi, 0u);
    int cloop = (int)prog->count;
    ge_rr(prog, r.wi, r.thirtytwo);
    int cdone = bra(prog, 1, 0);
    xor_rr(prog, r.tmp, r.wi, r.fifteen);
    ge_rr(prog, r.tmp, r.one);
    int do_word = bra(prog, 1, 0);
    int skip_word = bra(prog, 0, 0);
    fix(prog, do_word, (int)prog->count);
    add32(prog, r.nbit, r.wi, r.wi);
    add32(prog, r.nbit, r.nbit, r.nbit);
    add32(prog, r.off, r.dst_off, r.nbit);
    addk(prog, r.addr, r.base, r.off, 1u);
    ld(prog, r.word, r.addr, r.uv);
    emit_byte(prog, &r);
    emit_byte(prog, &r);
    emit_byte(prog, &r);
    emit_byte(prog, &r);
    fix(prog, skip_word, (int)prog->count);
    add32(prog, r.wi, r.wi, r.one);
    fix(prog, bra(prog, 0, 0), cloop);
    fix(prog, cdone, (int)prog->count);
    xor_rr(prog, r.crc, r.crc, r.ones);
    at_off(prog, &r, r.addr, r.dst_off, 0x3cu);
    st(prog, r.addr, r.crc, r.uv);
    /* R15: the chip's clock when the result and its notice are written,
     * before the release that makes them visible. */
    {
        BlackwellIRInsn t = op(BW_IR_S2R);
        t.dst_vreg = r.clk;
        t.imm = RX_SR_GLOBALTIMER_LO;
        em(prog, t);
    }
    at_off(prog, &r, r.addr, r.off_hb, RX_SEAT_HB_T_DONE);
    st(prog, r.addr, r.clk, r.uv);
    add32(prog, r.tail, r.tail, r.one);
    /* Release: the output window and the whole notice reach memory before
     * the processor can see the new tail. */
    em(prog, op(BW_IR_MEMBAR_SC_SYS));
    at_off(prog, &r, r.g2c, r.off_g2c, 0u);
    st(prog, r.g2c, r.tail, r.uv);

    fix(prog, not_claim, (int)prog->count);
    add32(prog, r.head, r.head, r.one);
    at_off(prog, &r, r.addr, r.off_c2g, 64u);
    st(prog, r.addr, r.head, r.uv);
    fix(prog, bra(prog, 0, 0), loop);

    int leave = (int)prog->count;
    fix(prog, give_exit, leave);
    fix(prog, hold_exit, leave);
    fix(prog, shutdown_exit, leave);
    em(prog, op(BW_IR_EXIT));
    em(prog, op(BW_IR_BRA));

    for (int v = 0; v < prog->regalloc.num_vregs; v++) {
        if (prog->regalloc.intervals[v].first_def < 0)
            prog->regalloc.intervals[v].first_def = 0;
        prog->regalloc.intervals[v].last_use = (int)prog->count - 1;
    }
    if (emit_overflow || omega_bw_regalloc_solve(prog) != 0) {
        fprintf(stderr, "seat build overflow %d instructions %zu vregs %d\n",
                emit_overflow, prog->count, prog->regalloc.num_vregs);
        return -1;
    }
    if (gpr_out) *gpr_out = prog->regalloc.peak_gpr_usage;
    return (int)prog->count;
}

/* Instruction classes by the low opcode bits of word 0. */
#define OPC(w0) ((w0) & 0xfffu)
#define OPC_S2R  0x919u
#define OPC_LDC  0xb82u
#define OPC_LDCU 0x7acu
#define OPC_LDG  0x981u
#define OPC_STG  0x986u
#define OPC_ISETP_I 0x80cu
#define OPC_ISETP_R 0x20cu
#define OPC_BRA  0x947u

/* Control field in word 3: stall 9..12, yield 13, write barrier 14..16,
 * read barrier 17..19, wait mask 20..25, reuse 26..29. */
#define CTL_STALL(n) ((uint32_t)(n) << 9)
#define CTL_YIELD    (1u << 13)
#define CTL_WB(b)    ((uint32_t)(b) << 14)
#define CTL_RB(b)    ((uint32_t)(b) << 17)
#define CTL_WAIT(m)  ((uint32_t)(m) << 20)
#define CTL_MASK     0x3ffffe00u

/* The seat runs one thread and polls. Speed does not matter here; a value
 * used before its load lands does. Every variable-latency read (constant
 * bank, global memory, special register) sets barrier 0. Every store holds
 * barrier 1 until its operands are read. Every instruction waits on both, and
 * fixed-latency work stalls long enough for the next instruction to read it. */
static void seat_scoreboard(uint8_t *code, size_t bytes) {
    for (size_t at = 0; at + 16 <= bytes; at += 16) {
        uint32_t w[4];
        memcpy(w, code + at, 16);
        uint32_t opc = OPC(w[0]);
        uint32_t ctl;
        if (opc == OPC_S2R || opc == OPC_LDC || opc == OPC_LDCU || opc == OPC_LDG)
            ctl = CTL_STALL(2) | CTL_WB(0) | CTL_RB(7) | CTL_WAIT(0x3);
        else if (opc == OPC_STG)
            ctl = CTL_STALL(2) | CTL_WB(7) | CTL_RB(1) | CTL_WAIT(0x3);
        else
            ctl = CTL_STALL(13) | CTL_WB(7) | CTL_RB(7) | CTL_WAIT(0x3);
        w[3] = (w[3] & ~CTL_MASK) | ctl;
        memcpy(code + at, w, 16);
    }
}

/* The seat must open with the lane check: only thread 0 continues. */
static int first_is_lane_check(const uint8_t *code, size_t n) {
    if (n < 48) return 0;
    uint32_t w0[3];
    for (int i = 0; i < 3; i++) memcpy(&w0[i], code + 16 * i, 4);
    int ok = OPC(w0[0]) == OPC_S2R &&
             (OPC(w0[1]) == OPC_ISETP_I || OPC(w0[1]) == OPC_ISETP_R) &&
             OPC(w0[2]) == OPC_BRA;
    if (!ok) fprintf(stderr, "seat: the program does not open with the lane check\n");
    return ok;
}

struct LaunchJob {
    RxGpuSeat *seat;
    uint8_t *code;
    size_t code_bytes;
    uint32_t gpr;
    SeatArgs args;
    int submit_rc;
    /* Launch memory, kept across a channel rebuild. */
    int have_mem;
    NvrmMem code_mem, cbank_mem, qmd_mem, marker_mem, large_pb;
};

/* The launch marker lands only when the seat leaves. A kill stops the wait. */
static int wait_marker_or_abort(RxGpuSeat *s, volatile uint32_t *marker, uint32_t want,
                                int timeout_ms) {
    for (int i = 0; i < timeout_ms * 20; i++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (*marker == want) return 0;
        if (s->abort) return -1;
        usleep(50);
    }
    return *marker == want ? 0 : -1;
}

static void *launch_main(void *arg) {
    LaunchJob *job = arg;
    RxGpuSeat *s = job->seat;
    M16NativeContext *ctx = &s->ctx;
    if (!job->have_mem) {
        if (nvrm_alloc(&ctx->rm, 0x4000, &job->code_mem) != 0 ||
            nvrm_alloc(&ctx->rm, 0x1000, &job->cbank_mem) != 0 ||
            nvrm_alloc(&ctx->rm, 0x10000, &job->qmd_mem) != 0 ||
            nvrm_alloc(&ctx->rm, 0x1000, &job->marker_mem) != 0 ||
            nvrm_alloc(&ctx->rm, 0x10000, &job->large_pb) != 0) {
            snprintf(s->err, sizeof s->err, "graphics memory was not allocated");
            job->submit_rc = -1;
            return NULL;
        }
        job->have_mem = 1;
    }
    NvrmMem code_mem = job->code_mem, cbank_mem = job->cbank_mem, qmd_mem = job->qmd_mem;
    NvrmMem marker_mem = job->marker_mem;
    ctx->pb_mem = job->large_pb;
    memcpy(code_mem.cpu, job->code, job->code_bytes);

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_mem.va);
    memcpy(cbank_mem.cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_mem.cpu + 0x380, &job->args, sizeof(job->args));

    uint64_t qmd0_va = qmd_mem.va;
    uint64_t qmd1_va = qmd_mem.va + 0x1000;
    uint64_t sem_va = qmd_mem.va + 0x2000;
    uint64_t scratch_va = qmd_mem.va + 0x4000;
    /* The chip keeps the top two registers of a thread's allocation for
     * itself. On GB10 a value placed in R46 of a 48-register launch read back
     * wrong. Ask for two more than the program uses, then round up. */
    uint32_t gpr = (job->gpr + 2u + 15u) & ~15u;
    if (gpr < 32u) gpr = 32u;
    OmegaBlackwellQmdConfig cfg = {
        .code_va = code_mem.va,
        .cbank_va = cbank_mem.va,
        .scratch_va = scratch_va,
        .sem_va = sem_va,
        .qmd0_va = qmd0_va,
        .qmd1_va = qmd1_va,
        .threads_x = 1,
        .grid_x = 1,
        .gpr_count = gpr
    };
    uint32_t qmd0[OMEGA_BW_QMD_WORDS];
    uint32_t qmd1[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(qmd0, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(qmd1, &cfg);
    if (omega_blackwell_verify_qmd_invariants(qmd1) != 0) {
        snprintf(s->err, sizeof s->err, "launch record was rejected");
        job->submit_rc = -1;
        return NULL;
    }
    memcpy(qmd_mem.cpu, qmd0, sizeof(qmd0));
    memcpy((uint8_t *)qmd_mem.cpu + 0x1000, qmd1, sizeof(qmd1));
    volatile uint32_t *hsem = (volatile uint32_t *)((uint8_t *)qmd_mem.cpu + 0x2000);
    volatile uint32_t *hmarker = (volatile uint32_t *)marker_mem.cpu;
    *hsem = 0;
    *hmarker = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    static const uint32_t setup[18] = {
        0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
        0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
        0x20012559, 0x00000000
    };
    uint32_t pb[1024];
    size_t n = 0;
    memcpy(&pb[n], setup, sizeof(setup));
    n += sizeof(setup) / 4;
    pb[n++] = nvrm_mthd(1, 0x0188, 2);
    pb[n++] = (uint32_t)(cbank_mem.va >> 32);
    pb[n++] = (uint32_t)cbank_mem.va;
    pb[n++] = nvrm_mthd(1, 0x0180, 2);
    pb[n++] = 0x00000380;
    pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1);
    pb[n++] = 0x00000041;
    pb[n++] = (224u << 16) | (1u << 13) | (0x01b4u >> 2) | (6u << 28);
    memcpy(&pb[n], cbank_data, 224 * 4);
    n += 224;
    uint32_t arg_words = (uint32_t)((sizeof(SeatArgs) + 3u) / 4u);
    pb[n++] = nvrm_mthd(1, 0x0188, 2);
    pb[n++] = (uint32_t)((cbank_mem.va + 0x380) >> 32);
    pb[n++] = (uint32_t)(cbank_mem.va + 0x380);
    pb[n++] = nvrm_mthd(1, 0x0180, 2);
    pb[n++] = arg_words * 4u;
    pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1);
    pb[n++] = 0x00000041;
    pb[n++] = (arg_words << 16) | (1u << 13) | (0x01b4u >> 2) | (6u << 28);
    memcpy(&pb[n], &job->args, sizeof(job->args));
    n += arg_words;
    pb[n++] = (98u << 16) | (1u << 13) | (0x0318u >> 2) | (2u << 28);
    pb[n++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ffu);
    pb[n++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[n], qmd0, 96 * 4);
    n += 96;
    pb[n++] = nvrm_mthd(1, 0x0188, 2);
    pb[n++] = (uint32_t)(sem_va >> 32);
    pb[n++] = (uint32_t)sem_va;
    pb[n++] = nvrm_mthd(1, 0x0180, 2);
    pb[n++] = 0x00000004;
    pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1);
    pb[n++] = 0x00000041;
    pb[n++] = (1u << 16) | (1u << 13) | (0x01b4u >> 2) | (6u << 28);
    pb[n++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb[n++] = (98u << 16) | (1u << 13) | (0x0318u >> 2) | (2u << 28);
    pb[n++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ffu);
    pb[n++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[n], qmd1, 96 * 4);
    n += 96;
    pb[n++] = nvrm_mthd(0, 0x005c, 5);
    pb[n++] = (uint32_t)marker_mem.va;
    pb[n++] = (uint32_t)(marker_mem.va >> 32);
    pb[n++] = OMEGA_BW_MARKER_COMPLETION_PAYLOAD;
    pb[n++] = 0;
    pb[n++] = 0x1u | (1u << 20);
    if (n > 1024) {
        snprintf(s->err, sizeof s->err, "launch stream does not fit");
        job->submit_rc = -1;
        return NULL;
    }
    job->submit_rc = m16_native_submit_methods(ctx, pb, n);
    if (job->submit_rc != 0) {
        snprintf(s->err, sizeof s->err, "the graphics seat was not submitted");
        return NULL;
    }
    int wait = wait_marker_or_abort(s, hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD, 60000);
    s->marker_ok = wait == 0 && *hmarker == OMEGA_BW_MARKER_COMPLETION_PAYLOAD;
    s->sem_ok = *hsem == OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE;
    if (!s->marker_ok && !s->abort)
        snprintf(s->err, sizeof s->err, "the graphics seat did not finish");
    return NULL;
}

/* Wait for the seat's first heartbeat in the image. */
static int wait_heartbeat(RxGpuSeat *s) {
    RxWorld *w = s->world;
    volatile uint32_t *hb = (volatile uint32_t *)(w->coherent + rx_world_off_heartbeat());
    s->hb_watch = hb;
    for (int i = 0; i < 2000; i++) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (*hb != 0) return 0;
        if (s->job->submit_rc != 0 && s->err[0]) break;
        usleep(1000);
    }
    if (!s->err[0]) snprintf(s->err, sizeof s->err, "the graphics seat did not show a heartbeat");
    fprintf(stderr, "seat: %s\n", s->err);
    return -1;
}

static uint32_t current_seat_gen(RxWorld *w) {
    pthread_mutex_lock(&w->mu);
    uint32_t g = w->seat_generation;
    pthread_mutex_unlock(&w->mu);
    return g;
}

int rx_gpu_seat_kill(RxGpuSeat *s) {
    if (!s || !s->opened || s->killed) return -1;
    /* Freeing the channel group preempts and tears down the running seat.
     * The image and launch memory stay allocated and mapped. */
    int rc = nvrm_channel_destroy(&s->ctx.rm);
    s->abort = 1;
    if (s->thread_live) {
        pthread_join(s->thread, NULL);
        s->thread_live = 0;
    }
    s->killed = 1;
    if (rc != 0) snprintf(s->err, sizeof s->err, "the graphics channel was not destroyed");
    return rc == 0 ? 0 : -1;
}

int rx_gpu_seat_relaunch(RxGpuSeat *s) {
    if (!s || !s->killed || !s->image_bound || !s->job) return -1;
    if (nvrm_channel(&s->ctx.rm) != 0) {
        snprintf(s->err, sizeof s->err, "the graphics channel was not rebuilt");
        return -1;
    }
    s->killed = 0;
    s->abort = 0;
    s->marker_ok = 0;
    s->sem_ok = 0;
    s->err[0] = 0;
    s->job->submit_rc = 0;
    s->job->args.seat_gen = current_seat_gen(s->world);
    memset(s->world->coherent + rx_world_off_heartbeat(), 0, 64);
    __asm__ volatile("dsb sy" ::: "memory");
    if (pthread_create(&s->thread, NULL, launch_main, s->job) != 0) return -1;
    s->thread_live = 1;
    return wait_heartbeat(s);
}

int rx_gpu_seat_begin(RxWorld *w, RxGpuSeat **out) {
    if (out) *out = NULL;
    if (!w || !w->coherent || !w->resident_enabled) return -1;
    if (!crc_matches_sealer()) return -1;

    BlackwellIRProgram prog;
    uint32_t gpr = 0;
    int insns = build_program(&prog, &gpr);
    fprintf(stderr, "seat build insns %d gpr %u\n", insns, gpr);
    if (insns < 8 || insns > BW_MAX_IR_INSNS) {
        fprintf(stderr, "seat: program was not built (%d)\n", insns);
        return -1;
    }
    size_t padded = ((size_t)insns + 7u) & ~7u;
    if (padded < 32) padded = 32;
    size_t bytes = padded * 16u;
    uint8_t *code = calloc(1, bytes);
    if (!code) return -1;
    size_t emitted = 0;
    if (omega_bw_encode_program(&prog, code, bytes, &emitted) != 0) {
        free(code);
        return -1;
    }
    seat_scoreboard(code, emitted);
    if (!first_is_lane_check(code, emitted)) {
        free(code);
        return -1;
    }

    RxGpuSeat *s = calloc(1, sizeof(*s));
    if (!s) { free(code); return -1; }
    if (m16_native_open(&s->ctx) != 0 || m16_native_create_channel(&s->ctx) != 0) {
        snprintf(s->err, sizeof s->err, "the graphics device did not open");
        m16_native_close(&s->ctx);
        free(code);
        free(s);
        return -1;
    }
    s->opened = 1;
    s->world = w;
    NvrmMem image;
    memset(&image, 0, sizeof(image));
    uint64_t bytes_img = w->coherent_bytes;
    uint64_t alloc = (bytes_img + 0xfffu) & ~0xfffu;
    if (alloc < 0x100000) alloc = 0x100000;
    if (nvrm_alloc_gpu_uncached(&s->ctx.rm, alloc, &image) != 0) {
        snprintf(s->err, sizeof s->err, "the shared image was not allocated");
        m16_native_close(&s->ctx);
        free(code);
        free(s);
        return -1;
    }
    if (rx_world_bind_coherent(w, image.cpu, bytes_img, 1) != RX_OK) {
        snprintf(s->err, sizeof s->err, "the world image was not bound");
        m16_native_close(&s->ctx);
        free(code);
        free(s);
        return -1;
    }
    s->image_bound = 1;
    memset(w->coherent + rx_world_off_heartbeat(), 0, 64);

    LaunchJob *job = calloc(1, sizeof(*job));
    if (!job) {
        seat_return_image(s);
        m16_native_close(&s->ctx);
        free(code);
        free(s);
        return -1;
    }
    job->seat = s;
    job->code = code;
    job->code_bytes = emitted;
    job->gpr = gpr;
    job->args.region = image.va;
    job->args.seat_gen = current_seat_gen(w);
    job->args.epoch = w->world_epoch;
    job->args.off_c2g = (uint32_t)rx_world_off_c2g();
    job->args.off_g2c = (uint32_t)rx_world_off_g2c();
    job->args.off_hb = (uint32_t)rx_world_off_heartbeat();
    job->args.off_tbl = (uint32_t)rx_world_off_object_table();
    job->args.giveup = GIVEUP;
    if (pthread_create(&s->thread, NULL, launch_main, job) != 0) {
        free(job);
        free(code);
        seat_return_image(s);
        m16_native_close(&s->ctx);
        free(s);
        return -1;
    }
    s->job = job;
    s->thread_live = 1;
    if (out) *out = s;
    if (wait_heartbeat(s) != 0) return -1;
    fprintf(stderr, "seat heartbeat %u instructions %d gpr %u\n", *s->hb_watch, insns, gpr);
    return 0;
}

int rx_gpu_seat_finish(RxGpuSeat *seat) {
    if (!seat) return -1;
    if (seat->thread_live) {
        pthread_join(seat->thread, NULL);
        seat->thread_live = 0;
    }
    /* A killed seat never reaches its marker; leaving is then only the image
     * going back to the world and the device closing. */
    int ok = seat->killed || (seat->marker_ok && seat->sem_ok);
    if (!ok && seat->err[0]) fprintf(stderr, "seat finish: %s\n", seat->err);
    if (seat_return_image(seat) != 0) ok = 0;
    if (seat->opened) m16_native_close(&seat->ctx);
    if (seat->job) {
        free(seat->job->code);
        free(seat->job);
    }
    free(seat);
    return ok ? 0 : -1;
}
