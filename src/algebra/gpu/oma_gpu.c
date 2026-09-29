/* Omega mixed algebra MA-6: GPU realizations of Omega-X on GB10 (sm_121).
 * See oma_gpu.h and spec/mixed-algebra-ma6-gpu.md.
 *
 * Structure:
 *   1. kernel builder on Omega's one IR + encoder (omega_blackwell_codegen.h),
 *      including the four ops added there for MA-6 (LOP3_LUT, POPC,
 *      IADD3_R3, LDG_E_OFF);
 *   2. a scheduler that chooses every instruction's control word (stall
 *      counts from register dependencies, scoreboards for variable-latency
 *      ops) and passes it to the encoder through insn->control, the same
 *      field the existing kernels use. Nothing is patched after encoding;
 *   3. host packers + CPU models of each kernel's index arithmetic;
 *   4. a launcher that keeps one M16 native channel open, reuses the
 *      QMD/cbank/push-buffer recipe of omega_blackwell_submit.c and waits on
 *      the completion marker with m16_native_wait_marker, as the silicon
 *      tests do.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "algebra/gpu/oma_gpu.h"

#include "omega_blackwell_codegen.h"
#include "omega_blackwell_qmd.h"
#include "m16_native.h"
#include "nvrm.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* 1. builder                                                          */
/* ------------------------------------------------------------------ */

enum { C_FIX = 1, C_HMMA, C_VAR, C_CTRL };
/* scoreboards */
enum { SB_W = 0, SB_X = 1, SB_SETUP = 2, SB_POPC = 3, SB_ATOM = 4, SB_STORE = 5 };

#define SR_GLOBALTIMER_LO 0x52u /* same special register rx_resident_gpu.c uses */
#define G3_MAGIC 0x4B400000u    /* bits of 1.5 * 2^23 */

typedef struct {
    uint8_t cls;
    int8_t wb, rb;
    uint8_t xwait;   /* extra wait mask */
    uint8_t lat;     /* fixed latency of the result (0 = class default) */
    uint8_t fixed;   /* encoder ignores insn->control for this op */
    uint8_t fstall;  /* its built-in stall */
    uint8_t fwait;   /* its built-in wait mask */
    uint8_t drain;   /* wait for every outstanding scoreboard here */
} meta_t;

typedef struct {
    BlackwellIRProgram prog;
    meta_t meta[BW_MAX_IR_INSNS];
    int bad;
    int loop_top, loop_bra;
    int scratch;
} kb_t;

static BlackwellIRInsn ins(BlackwellIROpcode o) {
    BlackwellIRInsn n;
    memset(&n, 0, sizeof n);
    n.op = o;
    n.dst_vreg = n.src1_vreg = n.src2_vreg = n.src3_vreg = n.ureg = -1;
    return n;
}

static meta_t mfix(void) { meta_t m; memset(&m, 0, sizeof m); m.cls = C_FIX; m.wb = m.rb = -1; return m; }
static meta_t mvar(int wb, int rb) { meta_t m = mfix(); m.cls = C_VAR; m.wb = (int8_t)wb; m.rb = (int8_t)rb; return m; }

static int emit(kb_t *k, BlackwellIRInsn n, meta_t m) {
    int i = omega_bw_ir_append(&k->prog, &n);
    if (i < 0) { k->bad = 1; return -1; }
    k->meta[i] = m;
    return i;
}

static int V(kb_t *k) { int v = omega_bw_ir_alloc_vreg(&k->prog); if (v < 0) k->bad = 1; return v; }
static int V2(kb_t *k) { int v = omega_bw_ir_alloc_vreg64(&k->prog); if (v < 0) k->bad = 1; return v; }
static int V4(kb_t *k) { int v = omega_bw_ir_alloc_vreg128(&k->prog); if (v < 0) k->bad = 1; return v; }
static int UV2(kb_t *k) { int v = omega_bw_ir_alloc_uvreg64(&k->prog); if (v < 0) k->bad = 1; return v; }

static void movi(kb_t *k, int d, uint32_t imm) { BlackwellIRInsn n = ins(BW_IR_MOV_IMM); n.dst_vreg = d; n.imm = imm; emit(k, n, mfix()); }
static void movrz(kb_t *k, int d, int sub) { BlackwellIRInsn n = ins(BW_IR_MOV_RZ); n.dst_vreg = d; n.dst_subreg = (uint8_t)sub; emit(k, n, mfix()); }
static void movi_sub(kb_t *k, int d, int sub, uint32_t imm) { BlackwellIRInsn n = ins(BW_IR_MOV_IMM); n.dst_vreg = d; n.dst_subreg = (uint8_t)sub; n.imm = imm; emit(k, n, mfix()); }
static void s2r(kb_t *k, int d, uint32_t sr) { BlackwellIRInsn n = ins(BW_IR_S2R); n.dst_vreg = d; n.imm = sr; emit(k, n, mvar(SB_SETUP, -1)); }
static void ldc64(kb_t *k, int d, uint32_t off) { BlackwellIRInsn n = ins(BW_IR_LDC64); n.dst_vreg = d; n.imm = off; emit(k, n, mvar(SB_SETUP, -1)); }
static void ldcu64(kb_t *k, int ud, uint32_t off) { BlackwellIRInsn n = ins(BW_IR_LDCU64); n.dst_vreg = ud; n.imm = off; n.is_uniform = true; emit(k, n, mvar(SB_SETUP, -1)); }
/* d = a * imm + c (c = -1: RZ) */
static void imadi(kb_t *k, int d, int a, uint32_t imm, int c) { BlackwellIRInsn n = ins(BW_IR_IMAD); n.dst_vreg = d; n.src1_vreg = a; n.imm = imm; n.src3_vreg = c; emit(k, n, mfix()); }
static void imadr(kb_t *k, int d, int a, int b, int c) { BlackwellIRInsn n = ins(BW_IR_IMAD); n.dst_vreg = d; n.src1_vreg = a; n.src2_vreg = b; n.src3_vreg = c; emit(k, n, mfix()); }
/* d64 = idx * scale + base64 */
static void wide(kb_t *k, int d, int idx, uint32_t scale, int base) { BlackwellIRInsn n = ins(BW_IR_IMAD_WIDE); n.dst_vreg = d; n.src1_vreg = idx; n.imm = scale; n.src3_vreg = base; emit(k, n, mfix()); }
static void add3s(kb_t *k, int d, int a, int as, int b, int bs, int c) {
    BlackwellIRInsn n = ins(BW_IR_IADD3_R3); n.dst_vreg = d; n.src1_vreg = a; n.src1_subreg = (uint8_t)as;
    n.src2_vreg = b; n.src2_subreg = (uint8_t)bs; n.src3_vreg = c; emit(k, n, mfix());
}
static void add3(kb_t *k, int d, int a, int b, int c) { add3s(k, d, a, 0, b, 0, c); }
static void add2(kb_t *k, int d, int a, int b) { add3s(k, d, a, 0, b, 0, -1); }
static void shr(kb_t *k, int d, int a, uint32_t sh) { BlackwellIRInsn n = ins(BW_IR_SHF_R); n.dst_vreg = d; n.src1_vreg = a; n.imm = sh; emit(k, n, mfix()); }
static void andi(kb_t *k, int d, int a, uint32_t imm) { BlackwellIRInsn n = ins(BW_IR_LOP3_AND); n.dst_vreg = d; n.src1_vreg = a; n.imm = imm; emit(k, n, mfix()); }
static void lop3(kb_t *k, int d, int a, int b, int c, uint8_t lut) {
    BlackwellIRInsn n = ins(BW_IR_LOP3_LUT); n.dst_vreg = d; n.src1_vreg = a; n.src2_vreg = b; n.src3_vreg = c; n.imm = lut;
    emit(k, n, mfix());
}
static void popc(kb_t *k, int d, int a) {
    BlackwellIRInsn n = ins(BW_IR_POPC); n.dst_vreg = d; n.src1_vreg = a;
    meta_t m = mvar(SB_POPC, -1); m.lat = 15; /* scoreboard and a fixed-latency guard both */
    emit(k, n, m);
}
static void ldg(kb_t *k, int d, int dsub, int addr, int ud, int32_t off, int sb) {
    BlackwellIRInsn n = ins(off ? BW_IR_LDG_E_OFF : BW_IR_LDG_E); n.dst_vreg = d; n.dst_subreg = (uint8_t)dsub;
    n.src1_vreg = addr; n.ureg = ud; n.imm = (uint32_t)off;
    emit(k, n, mvar(sb, -1));
}
static void stg(kb_t *k, int addr, int data, int ud) { BlackwellIRInsn n = ins(BW_IR_STG_E); n.src1_vreg = addr; n.src2_vreg = data; n.ureg = ud; emit(k, n, mvar(-1, SB_STORE)); }
static void atom_add(kb_t *k, int d, int addr, int val, int ud) {
    BlackwellIRInsn n = ins(BW_IR_ATOMG_ADD_STRONG_SYS); n.dst_vreg = d; n.src1_vreg = addr; n.src2_vreg = val; n.ureg = ud;
    emit(k, n, mvar(SB_ATOM, SB_STORE));
}
/* ISETP.GE (immediate) keeps the encoder's built-in control 0x001fda00:
 * stall 13, waits on scoreboard 0. */
static void isetp_i(kb_t *k, int a, uint32_t imm) {
    BlackwellIRInsn n = ins(BW_IR_ISETP_GE); n.src1_vreg = a; n.imm = imm;
    meta_t m = mfix(); m.lat = 13; m.fixed = 1; m.fstall = 13; m.fwait = 0x1; emit(k, n, m);
}
static void isetp_u(kb_t *k, int a, int b) { BlackwellIRInsn n = ins(BW_IR_ISETP_GE_U32); n.src1_vreg = a; n.src2_vreg = b; meta_t m = mfix(); m.lat = 13; emit(k, n, m); }
static int bra(kb_t *k, int pred, int neg) {
    BlackwellIRInsn n = ins(BW_IR_BRA); n.predicate_p0 = pred != 0; n.predicate_not = neg != 0;
    meta_t m = mfix(); m.cls = C_CTRL; return emit(k, n, m);
}
/* EXIT keeps its built-in control 0x000fea00 (stall 5, no wait), so a MOV
 * into a scratch register just before it carries the drain wait. */
static void exit_(kb_t *k, int pred) {
    BlackwellIRInsn c = ins(BW_IR_MOV_RZ); c.dst_vreg = k->scratch;
    meta_t mc = mfix(); mc.drain = 1; emit(k, c, mc);
    BlackwellIRInsn n = ins(BW_IR_EXIT); n.predicate_p0 = pred != 0;
    meta_t m = mfix(); m.cls = C_CTRL; m.fixed = 1; m.fstall = 5; m.fwait = 0; emit(k, n, m);
}
static void selfbra(kb_t *k) {
    BlackwellIRInsn b = ins(BW_IR_BRA); /* self branch after EXIT, as the existing kernels emit */
    meta_t mb = mfix(); mb.cls = C_CTRL; emit(k, b, mb);
}
static void hmma(kb_t *k, int d, int a, int b, int c) {
    BlackwellIRInsn n = ins(BW_IR_HMMA_BF16); n.dst_vreg = d; n.src1_vreg = a; n.src2_vreg = b; n.src3_vreg = c;
    meta_t m = mfix(); m.cls = C_HMMA; emit(k, n, m);
}

/* ---- def/use in physical register ids: GPR 0..254, uniform 256+, P0 400 ---- */
#define REG_P0 400
#define REG_MAX 512

typedef struct { int d[8], nd, u[16], nu; } du_t;

static void addr_(int *arr, int *cnt, int base, int width) {
    for (int i = 0; i < width; i++) {
        int r = base + i;
        if (r == 255) continue; /* RZ */
        arr[(*cnt)++] = r;
    }
}

static int ph(const OmegaRegAlloc *ra, int v, int sub) { return ra->vreg_to_phys[v] + sub; }

static void defuse(const kb_t *k, int i, du_t *o) {
    const BlackwellIRInsn *n = &k->prog.insns[i];
    const OmegaRegAlloc *ra = &k->prog.regalloc;
    memset(o, 0, sizeof *o);
    int A = n->src1_vreg, B = n->src2_vreg, C = n->src3_vreg, D = n->dst_vreg;
    switch (n->op) {
    case BW_IR_MOV_RZ: case BW_IR_MOV_IMM: case BW_IR_S2R: case BW_IR_LDC:
        addr_(o->d, &o->nd, ph(ra, D, n->dst_subreg), 1); break;
    case BW_IR_LDC64:
        addr_(o->d, &o->nd, ph(ra, D, 0), 2); break;
    case BW_IR_LDCU64:
        addr_(o->d, &o->nd, 256 + ra->uvreg_to_phys[D], 2); break;
    case BW_IR_IMAD:
        addr_(o->d, &o->nd, ph(ra, D, n->dst_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, A, n->src1_subreg), 1);
        if (B >= 0) addr_(o->u, &o->nu, ph(ra, B, n->src2_subreg), 1);
        if (C >= 0) addr_(o->u, &o->nu, ph(ra, C, n->src3_subreg), 1);
        break;
    case BW_IR_IMAD_WIDE:
        addr_(o->d, &o->nd, ph(ra, D, 0), 2);
        addr_(o->u, &o->nu, ph(ra, A, n->src1_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, C, 0), 2);
        break;
    case BW_IR_ISETP_GE:
        addr_(o->u, &o->nu, ph(ra, A, n->src1_subreg), 1);
        o->d[o->nd++] = REG_P0; break;
    case BW_IR_ISETP_GE_U32:
        addr_(o->u, &o->nu, ph(ra, A, n->src1_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, B, n->src2_subreg), 1);
        o->d[o->nd++] = REG_P0; break;
    case BW_IR_LDG_E: case BW_IR_LDG_E_U16: case BW_IR_LDG_E_OFF:
        addr_(o->d, &o->nd, ph(ra, D, n->dst_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, A, 0), 2);
        addr_(o->u, &o->nu, 256 + ra->uvreg_to_phys[n->ureg], 2);
        break;
    case BW_IR_STG_E:
        addr_(o->u, &o->nu, ph(ra, A, 0), 2);
        addr_(o->u, &o->nu, ph(ra, B, n->src2_subreg), 1);
        addr_(o->u, &o->nu, 256 + ra->uvreg_to_phys[n->ureg], 2);
        break;
    case BW_IR_ATOMG_ADD_STRONG_SYS:
        if (D >= 0) addr_(o->d, &o->nd, ph(ra, D, 0), 1);
        addr_(o->u, &o->nu, ph(ra, A, 0), 2);
        addr_(o->u, &o->nu, ph(ra, B, n->src2_subreg), 1);
        addr_(o->u, &o->nu, 256 + ra->uvreg_to_phys[n->ureg], 2);
        break;
    case BW_IR_IADD3: case BW_IR_IADD3_R3: case BW_IR_LOP3_XOR: case BW_IR_LOP3_LUT:
        addr_(o->d, &o->nd, ph(ra, D, n->dst_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, A, n->src1_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, B, n->src2_subreg), 1);
        if (C >= 0) addr_(o->u, &o->nu, ph(ra, C, n->src3_subreg), 1);
        break;
    case BW_IR_HMMA_BF16: case BW_IR_HMMA_F16:
        addr_(o->d, &o->nd, ph(ra, D, 0), 4);
        addr_(o->u, &o->nu, ph(ra, A, 0), 4);
        addr_(o->u, &o->nu, ph(ra, B, 0), 2);
        if (C >= 0) addr_(o->u, &o->nu, ph(ra, C, 0), 4);
        break;
    case BW_IR_SHF_R: case BW_IR_LOP3_AND: case BW_IR_POPC:
        addr_(o->d, &o->nd, ph(ra, D, n->dst_subreg), 1);
        addr_(o->u, &o->nu, ph(ra, A, n->src1_subreg), 1);
        break;
    case BW_IR_BRA: case BW_IR_EXIT:
        if (n->predicate_p0) o->u[o->nu++] = REG_P0;
        break;
    default: break;
    }
}

static int intersects(const int *a, int na, const int *b, int nb) {
    for (int i = 0; i < na; i++) for (int j = 0; j < nb; j++) if (a[i] == b[j]) return 1;
    return 0;
}

/* ---- scheduler ---- */
typedef struct { kb_t *k; uint8_t *stall; du_t *du; int changed; } sched_t;

static int nsucc(const kb_t *k, int i, int *s) {
    int n = (int)k->prog.count, c = 0;
    const BlackwellIRInsn *x = &k->prog.insns[i];
    if (x->op == BW_IR_EXIT && !x->predicate_p0) return 0;
    if (i == k->loop_bra) s[c++] = k->loop_top;
    if (x->op == BW_IR_BRA && i != k->loop_bra && !x->predicate_p0) return 0; /* trailing self branch */
    if (i + 1 < n) s[c++] = i + 1;
    return c;
}

/* From producer p, walk every path; if a consumer is reached with fewer
 * issue cycles than the latency, add stall on the path just before it. */
static void walk(sched_t *S, int p, int lat, int war, int node, int acc, int *path, int depth) {
    if (depth > 64) return;
    const du_t *dp = &S->du[p], *dj = &S->du[node];
    int hit = war ? intersects(dj->d, dj->nd, dp->u, dp->nu)
                  : (intersects(dj->u, dj->nu, dp->d, dp->nd) || intersects(dj->d, dj->nd, dp->d, dp->nd));
    if (hit) {
        if (acc < lat) {
            int need = lat - acc;
            for (int q = depth - 1; q >= -1 && need > 0; q--) {
                int at = q >= 0 ? path[q] : p;
                if (S->k->meta[at].fixed) continue;
                int room = 15 - S->stall[at];
                if (room <= 0) continue;
                int add = room < need ? room : need;
                S->stall[at] = (uint8_t)(S->stall[at] + add);
                need -= add;
                S->changed = 1;
            }
            if (need > 0) S->k->bad = 1;
        }
        return;
    }
    int nacc = acc + S->stall[node];
    if (nacc >= lat) return;
    int s[2];
    int c = nsucc(S->k, node, s);
    path[depth] = node;
    for (int i = 0; i < c; i++) walk(S, p, lat, war, s[i], nacc, path, depth + 1);
}

static int latency(const kb_t *k, int i) {
    const meta_t *m = &k->meta[i];
    if (m->lat) return m->lat;
    if (m->cls == C_FIX) return 6;
    if (m->cls == C_HMMA) return 32;
    return 0;
}

static void schedule(kb_t *k, uint8_t *stall, uint8_t *wait) {
    int n = (int)k->prog.count;
    du_t *du = calloc((size_t)n, sizeof *du);
    if (!du) { k->bad = 1; return; }
    for (int i = 0; i < n; i++) {
        defuse(k, i, &du[i]);
        const meta_t *m = &k->meta[i];
        stall[i] = 1;
        if (m->cls == C_VAR) stall[i] = 2;
        if (m->cls == C_CTRL) stall[i] = 5;
        if (m->fixed) stall[i] = m->fstall;
        wait[i] = m->xwait;
    }
    sched_t S = { k, stall, du, 1 };
    int path[80];
    for (int pass = 0; pass < 200 && S.changed; pass++) {
        S.changed = 0;
        for (int p = 0; p < n; p++) {
            int lat = latency(k, p);
            if (!lat) continue;
            int s[2];
            int c = nsucc(k, p, s);
            for (int j = 0; j < c; j++) walk(&S, p, lat, 0, s[j], stall[p], path, 0);
            if (k->meta[p].cls == C_HMMA)
                for (int j = 0; j < c; j++) walk(&S, p, 16, 1, s[j], stall[p], path, 0);
        }
    }
    if (S.changed) k->bad = 1;
    /* scoreboards, program order; loop top, back branch and drain carriers wait for all */
    static uint8_t pw[REG_MAX], pr[REG_MAX];
    memset(pw, 0, sizeof pw);
    memset(pr, 0, sizeof pr);
    for (int i = 0; i < n; i++) {
        const meta_t *m = &k->meta[i];
        uint8_t w = wait[i];
        int drain = (i == k->loop_top) || (i == k->loop_bra) || m->drain;
        if (drain) for (int r = 0; r < REG_MAX; r++) w |= pw[r] | pr[r];
        for (int j = 0; j < du[i].nu; j++) w |= pw[du[i].u[j]];
        for (int j = 0; j < du[i].nd; j++) w |= pw[du[i].d[j]] | pr[du[i].d[j]];
        if (m->fixed) {
            if (w & (uint8_t)~m->fwait) { k->bad = 1; fprintf(stderr, "omg: insn %d needs wait 0x%x beyond its fixed control\n", i, w); }
            w = m->fwait;
        }
        wait[i] = w;
        if (w) for (int r = 0; r < REG_MAX; r++) { pw[r] &= (uint8_t)~w; pr[r] &= (uint8_t)~w; }
        if (m->cls == C_VAR) {
            if (m->wb >= 0) for (int j = 0; j < du[i].nd; j++) pw[du[i].d[j]] |= (uint8_t)(1u << m->wb);
            if (m->rb >= 0) for (int j = 0; j < du[i].nu; j++) pr[du[i].u[j]] |= (uint8_t)(1u << m->rb);
        }
    }
    free(du);
}

static int finish(kb_t *k, omg_kernel *out) {
    if (k->bad) return -1;
    int n = (int)k->prog.count;
    /* every vreg lives for the whole program: no reuse across the loop */
    for (int v = 0; v < k->prog.regalloc.num_vregs; v++) {
        k->prog.regalloc.intervals[v].first_def = 0;
        k->prog.regalloc.intervals[v].last_use = n - 1;
    }
    for (int v = 0; v < k->prog.regalloc.num_uvregs; v++) {
        k->prog.regalloc.uintervals[v].first_def = 0;
        k->prog.regalloc.uintervals[v].last_use = n - 1;
    }
    if (omega_bw_regalloc_solve(&k->prog) != 0) return -1;
    uint8_t stall[BW_MAX_IR_INSNS], wait[BW_MAX_IR_INSNS];
    schedule(k, stall, wait);
    if (k->bad) return -1;
    for (int i = 0; i < n; i++) {
        const meta_t *m = &k->meta[i];
        if (m->fixed) continue;
        uint32_t wb = (m->cls == C_VAR && m->wb >= 0) ? (uint32_t)m->wb : 7u;
        uint32_t rb = (m->cls == C_VAR && m->rb >= 0) ? (uint32_t)m->rb : 7u;
        /* control field: stall 9..12, write barrier 14..16, read barrier
         * 17..19, wait mask 20..25 (as in rx_resident_gpu.c seat_scoreboard) */
        k->prog.insns[i].control = ((uint32_t)stall[i] << 9) | (wb << 14) | (rb << 17) | ((uint32_t)wait[i] << 20);
    }
    size_t padded = ((size_t)n + 7) & ~(size_t)7;
    if (padded < 32) padded = 32;
    uint8_t *code = calloc(padded, 16);
    if (!code) return -1;
    size_t len = 0;
    if (omega_bw_encode_program(&k->prog, code, padded * 16, &len) != 0) { free(code); return -1; }
    out->code = code;
    out->code_bytes = len;
    out->insn_count = (size_t)n;
    uint32_t g = k->prog.regalloc.peak_gpr_usage + 2;
    out->gpr = g < 32 ? 32 : ((g + 7) & ~7u);
    sha256_hash(code, len, out->sha256);
    return 0;
}

void omg_kernel_free(omg_kernel *k) {
    if (k && k->code) { free(k->code); k->code = NULL; }
}

static uint32_t lg2(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }
static int pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* common prologue */
typedef struct { int ud, pw0, px, py, pts, pw1, tid, cta, t0, gtid, one; } pro_t;

static void prologue(kb_t *k, const omg_geom *g, pro_t *p, int need_w1) {
    p->ud = UV2(k);
    p->pw0 = V2(k); p->px = V2(k); p->py = V2(k); p->pts = V2(k);
    p->pw1 = need_w1 ? V2(k) : -1;
    p->tid = V(k); p->cta = V(k); p->t0 = V(k); p->gtid = V(k); p->one = V(k);
    k->scratch = V(k);
    ldcu64(k, p->ud, 0x358);
    ldc64(k, p->pw0, 0x380);
    ldc64(k, p->px, 0x388);
    ldc64(k, p->py, 0x390);
    ldc64(k, p->pts, 0x398);
    if (need_w1) ldc64(k, p->pw1, 0x3a0);
    s2r(k, p->tid, BW_SR_TID_X);
    s2r(k, p->cta, BW_SR_CTAID_X);
    s2r(k, p->t0, SR_GLOBALTIMER_LO);
    movi(k, p->one, 1);
    imadi(k, p->gtid, p->cta, g->cta, p->tid);
}

/* lane 0 of every warp writes (t0, t1) to ts[2*warp .. 2*warp+1] */
static void epilogue(kb_t *k, pro_t *p) {
    int t1 = V(k), lane = V(k), wv = V(k), i0 = V(k), i1 = V(k), a0 = V2(k), a1 = V2(k);
    BlackwellIRInsn n = ins(BW_IR_S2R); n.dst_vreg = t1; n.imm = SR_GLOBALTIMER_LO;
    meta_t m = mvar(SB_SETUP, -1); m.xwait = (uint8_t)(1u << SB_ATOM);
    emit(k, n, m);
    andi(k, lane, p->tid, 31);
    isetp_i(k, lane, 1);
    exit_(k, 1);
    shr(k, wv, p->gtid, 5);
    imadi(k, i0, wv, 2, -1);
    wide(k, a0, i0, 4, p->pts);
    stg(k, a0, p->t0, p->ud);
    add2(k, i1, i0, p->one);
    wide(k, a1, i1, 4, p->pts);
    stg(k, a1, t1, p->ud);
    exit_(k, 0);
    selfbra(k);
}

static void row_slice(kb_t *k, const omg_geom *g, int src, int row, int s, uint32_t rows) {
    if (rows == 1) movrz(k, row, 0); else andi(k, row, src, rows - 1);
    if (rows == 1) imadi(k, s, src, 1, -1); else shr(k, s, src, lg2(rows));
    (void)g;
}

/* G1 / G2A: unpack field, IMAD with widened x, subtract off * sum(x) */
static int build_simt_imad(const omg_geom *g, kb_t *k) {
    uint32_t bits = g->kind == OMG_G1_I8 ? 8 : 2, wpw = g->wpw, U = g->U, T = g->threads;
    uint32_t off = g->kind == OMG_G1_I8 ? 128u : 1u, mask = (1u << bits) - 1;
    pro_t p; prologue(k, g, &p, 0);
    int row = V(k), s = V(k), widx = V(k), xidx = V(k), wlim = V(k), wstep = V(k), xstep = V(k);
    int acc[4], sx[2];
    for (int i = 0; i < 4; i++) acc[i] = V(k);
    for (int i = 0; i < 2; i++) sx[i] = V(k);
    int aw = V2(k), ax = V2(k);
    int wv[4], xv[4][16], tmp[8];
    for (uint32_t u = 0; u < U; u++) { wv[u] = V(k); for (uint32_t b = 0; b < wpw; b++) xv[u][b] = V(k); }
    for (int i = 0; i < 8; i++) tmp[i] = V(k);
    if (k->bad) return -1;
    row_slice(k, g, p.gtid, row, s, g->m);
    imadi(k, widx, p.gtid, 1, -1);
    imadi(k, xidx, s, wpw, -1);
    movi(k, wlim, g->m * g->units_per_row);
    movi(k, wstep, U * T);
    movi(k, xstep, U * g->S * wpw);
    for (int i = 0; i < 4; i++) movrz(k, acc[i], 0);
    for (int i = 0; i < 2; i++) movrz(k, sx[i], 0);
    k->loop_top = (int)k->prog.count;
    wide(k, aw, widx, 4, p.pw0);
    wide(k, ax, xidx, 4, p.px);
    for (uint32_t u = 0; u < U; u++) ldg(k, wv[u], 0, aw, p.ud, (int32_t)(u * 4u * T), SB_W);
    for (uint32_t u = 0; u < U; u++)
        for (uint32_t b = 0; b < wpw; b++)
            ldg(k, xv[u][b], 0, ax, p.ud, (int32_t)((u * g->S * wpw + b) * 4u), SB_X);
    int t = 0, a = 0, si = 0;
    for (uint32_t u = 0; u < U; u++) {
        for (uint32_t b = 0; b < wpw; b++) {
            int f = tmp[t++ & 7];
            if (b == 0) andi(k, f, wv[u], mask);
            else if (b == wpw - 1) shr(k, f, wv[u], bits * b);
            else { shr(k, f, wv[u], bits * b); andi(k, f, f, mask); }
            imadr(k, acc[a], f, xv[u][b], acc[a]);
            a = (a + 1) & 3;
        }
        for (uint32_t b = 0; b + 1 < wpw; b += 2) { add3(k, sx[si], sx[si], xv[u][b], xv[u][b + 1]); si ^= 1; }
    }
    add2(k, widx, widx, wstep);
    add2(k, xidx, xidx, xstep);
    isetp_u(k, widx, wlim);
    int b = bra(k, 1, 1);
    k->loop_bra = b;
    k->prog.insns[b].imm = (uint32_t)(k->loop_top - b);
    /* tail */
    int y = V(k), sxt = V(k), ay = V2(k), old = V(k);
    if (k->bad) return -1;
    add3(k, y, acc[0], acc[1], acc[2]);
    add2(k, y, y, acc[3]);
    add2(k, sxt, sx[0], sx[1]);
    imadi(k, y, sxt, (uint32_t)(-(int32_t)off), y);
    wide(k, ay, row, 4, p.py);
    atom_add(k, old, ay, y, p.ud);
    epilogue(k, &p);
    return 0;
}

/* G2B: bit planes + POPC */
static int build_plane(const omg_geom *g, kb_t *k) {
    uint32_t U = g->U, T = g->threads;
    pro_t p; prologue(k, g, &p, 1);
    int row = V(k), s = V(k), widx = V(k), xidx = V(k), wlim = V(k), wstep = V(k), xstep = V(k);
    int A[8], B[8];
    for (int j = 0; j < 8; j++) { A[j] = V(k); B[j] = V(k); }
    int ap = V2(k), am = V2(k), ax = V2(k);
    int P[2], M[2], X[2][8], ta[2][8], tb[2][8];
    for (uint32_t u = 0; u < U; u++) {
        P[u] = V(k); M[u] = V(k);
        for (int j = 0; j < 8; j++) { X[u][j] = V(k); ta[u][j] = V(k); tb[u][j] = V(k); }
    }
    if (k->bad) return -1;
    row_slice(k, g, p.gtid, row, s, g->m);
    imadi(k, widx, p.gtid, 1, -1);
    imadi(k, xidx, s, 8, -1);
    movi(k, wlim, g->m * g->units_per_row);
    movi(k, wstep, U * T);
    movi(k, xstep, U * g->S * 8);
    for (int j = 0; j < 8; j++) { movrz(k, A[j], 0); movrz(k, B[j], 0); }
    k->loop_top = (int)k->prog.count;
    wide(k, ap, widx, 4, p.pw0);
    wide(k, am, widx, 4, p.pw1);
    wide(k, ax, xidx, 4, p.px);
    for (uint32_t u = 0; u < U; u++) {
        ldg(k, P[u], 0, ap, p.ud, (int32_t)(u * 4u * T), SB_W);
        ldg(k, M[u], 0, am, p.ud, (int32_t)(u * 4u * T), SB_W);
    }
    for (uint32_t u = 0; u < U; u++)
        for (int j = 0; j < 8; j++)
            ldg(k, X[u][j], 0, ax, p.ud, (int32_t)((u * g->S * 8 + (uint32_t)j) * 4u), SB_X);
    for (uint32_t u = 0; u < U; u++)
        for (int j = 0; j < 8; j++) {
            lop3(k, ta[u][j], P[u], X[u][j], -1, 0xC0);
            lop3(k, tb[u][j], M[u], X[u][j], -1, 0xC0);
        }
    for (uint32_t u = 0; u < U; u++)
        for (int j = 0; j < 8; j++) { popc(k, ta[u][j], ta[u][j]); popc(k, tb[u][j], tb[u][j]); }
    for (int j = 0; j < 8; j++) {
        add3(k, A[j], A[j], ta[0][j], ta[1][j]);
        add3(k, B[j], B[j], tb[0][j], tb[1][j]);
    }
    add2(k, widx, widx, wstep);
    add2(k, xidx, xidx, xstep);
    isetp_u(k, widx, wlim);
    int b = bra(k, 1, 1);
    k->loop_bra = b;
    k->prog.insns[b].imm = (uint32_t)(k->loop_top - b);
    int y = V(k), ay = V2(k), old = V(k);
    if (k->bad) return -1;
    movrz(k, y, 0);
    for (int j = 0; j < 8; j++) {
        uint32_t c = j == 7 ? (uint32_t)-128 : (1u << j);
        imadi(k, y, A[j], c, y);
        imadi(k, y, B[j], (uint32_t)0 - c, y);
    }
    wide(k, ay, row, 4, p.py);
    atom_add(k, old, ay, y, p.ud);
    epilogue(k, &p);
    return 0;
}

/* G3: HMMA.16816.F32.BF16 with magic-biased FP32 accumulators */
static int build_hmma(const omg_geom *g, kb_t *k) {
    uint32_t U = g->U, NW = g->warps, tiles = g->tiles;
    pro_t p; prologue(k, g, &p, 0);
    int lane = V(k), warp = V(k), tile = V(k), s = V(k), gq = V(k), tq = V(k);
    int aidx = V(k), xidx = V(k), astep = V(k), xstep = V(k), alim = V(k);
    int acc[2], Aq[2], Bp[2];
    for (uint32_t u = 0; u < U; u++) { acc[u] = V4(k); Aq[u] = V4(k); Bp[u] = V2(k); }
    int aa = V2(k), ax = V2(k);
    if (k->bad) return -1;
    andi(k, lane, p.tid, 31);
    shr(k, warp, p.gtid, 5);
    row_slice(k, g, warp, tile, s, tiles);
    shr(k, gq, lane, 2);
    andi(k, tq, lane, 3);
    imadi(k, aidx, warp, 128, lane);
    imadi(k, xidx, s, 8, tq);
    movi(k, astep, U * NW * 128u);
    movi(k, xstep, U * g->S * 8u);
    movi(k, alim, tiles * g->units_per_row * 128u);
    for (uint32_t u = 0; u < U; u++) for (int r = 0; r < 4; r++) movi_sub(k, acc[u], r, G3_MAGIC);
    k->loop_top = (int)k->prog.count;
    wide(k, aa, aidx, 4, p.pw0);
    wide(k, ax, xidx, 4, p.px);
    for (uint32_t u = 0; u < U; u++)
        for (int r = 0; r < 4; r++)
            ldg(k, Aq[u], r, aa, p.ud, (int32_t)((u * NW * 128u + (uint32_t)r * 32u) * 4u), SB_W);
    for (uint32_t u = 0; u < U; u++) {
        ldg(k, Bp[u], 0, ax, p.ud, (int32_t)((u * g->S * 8u) * 4u), SB_X);
        ldg(k, Bp[u], 1, ax, p.ud, (int32_t)((u * g->S * 8u + 4u) * 4u), SB_X);
    }
    for (uint32_t u = 0; u < U; u++) hmma(k, acc[u], Aq[u], Bp[u], acc[u]);
    add2(k, aidx, aidx, astep);
    add2(k, xidx, xidx, xstep);
    isetp_u(k, aidx, alim);
    int b = bra(k, 1, 1);
    k->loop_bra = b;
    k->prog.insns[b].imm = (uint32_t)(k->loop_top - b);
    int negm = V(k), v0 = V(k), v1 = V(k), r0 = V(k), r1 = V(k), ay0 = V2(k), ay1 = V2(k), o0 = V(k), o1 = V(k);
    if (k->bad) return -1;
    movi(k, negm, (uint32_t)0 - U * G3_MAGIC);
    add3s(k, v0, acc[0], 0, acc[1], 0, negm);
    add3s(k, v1, acc[0], 2, acc[1], 2, negm);
    isetp_i(k, tq, 1);
    exit_(k, 1);
    imadi(k, r0, tile, 16, gq);
    wide(k, ay0, r0, 4, p.py);
    atom_add(k, o0, ay0, v0, p.ud);
    imadi(k, r1, p.one, 8, r0);
    wide(k, ay1, r1, 4, p.py);
    atom_add(k, o1, ay1, v1, p.ud);
    epilogue(k, &p);
    return 0;
}

const char *omg_kind_name(omg_kind k) {
    switch (k) {
    case OMG_G1_I8: return "G1_int8_imad";
    case OMG_G2A_CRUMB: return "G2A_crumb_imad";
    case OMG_G2B_PLANE: return "G2B_bitplane_popc";
    case OMG_G3_BF16: return "G3_bf16_hmma";
    }
    return "?";
}

int omg_plan(omg_kind kind, uint32_t m, uint32_t n, omg_geom *g) {
    if (!g || !pow2(m) || !pow2(n) || m > 65536 || n < 1024 || n > (1u << 20)) return -1;
    memset(g, 0, sizeof *g);
    g->kind = kind; g->m = m; g->n = n; g->m_pad = m; g->tiles = m;
    const uint32_t TT = 65536, TW = 2048;
    switch (kind) {
    case OMG_G1_I8: g->wpw = 4; g->U = 4; break;
    case OMG_G2A_CRUMB: g->wpw = 16; g->U = 2; break;
    case OMG_G2B_PLANE: g->wpw = 32; g->U = 2; break;
    case OMG_G3_BF16: g->wpw = 16; g->U = 2; break;
    default: return -1;
    }
    g->units_per_row = n / g->wpw;
    if (g->units_per_row < g->U) return -1;
    if (kind == OMG_G3_BF16) {
        if (n > OMG_G3_MAX_N) return -1; /* outside the declared exact domain */
        g->tiles = m < 16 ? 1 : m / 16;
        g->m_pad = g->tiles * 16;
        uint32_t S = TW / g->tiles; if (S < 1) S = 1;
        if (S > g->units_per_row / g->U) S = g->units_per_row / g->U;
        g->S = S;
        g->warps = S * g->tiles;
        g->threads = 32 * g->warps;
        g->w0_bytes = (size_t)g->tiles * 16 * n * 2;
        g->x_bytes = (size_t)n * 2;
        if ((uint64_t)(g->U - 1) * g->warps * 512u + 384u >= (1u << 23)) return -1;
    } else {
        uint32_t S = TT / m; if (S < 1) S = 1;
        if (S > g->units_per_row / g->U) S = g->units_per_row / g->U;
        g->S = S;
        g->threads = S * m;
        g->warps = (g->threads + 31) / 32;
        if (kind == OMG_G1_I8) { g->w0_bytes = (size_t)m * n; g->x_bytes = (size_t)n * 4; }
        if (kind == OMG_G2A_CRUMB) { g->w0_bytes = (size_t)m * n / 4; g->x_bytes = (size_t)n * 4; }
        if (kind == OMG_G2B_PLANE) { g->w0_bytes = g->w1_bytes = (size_t)m * n / 8; g->x_bytes = (size_t)n; }
        if ((uint64_t)(g->U - 1) * 4u * g->threads >= (1u << 23)) return -1;
        uint32_t xw = kind == OMG_G2B_PLANE ? 8 : g->wpw;
        if ((uint64_t)((g->U - 1) * g->S * xw + xw) * 4u >= (1u << 23)) return -1;
    }
    g->cta = g->threads < 128 ? g->threads : 128;
    g->grid = g->threads / g->cta;
    g->y_bytes = (size_t)g->m_pad * 4;
    g->ts_bytes = (size_t)g->warps * 8;
    return 0;
}

int omg_build(const omg_geom *g, omg_kernel *out) {
    if (!g || !out) return -1;
    memset(out, 0, sizeof *out);
    kb_t *k = calloc(1, sizeof *k);
    if (!k) return -1;
    omega_bw_ir_init(&k->prog);
    k->loop_top = k->loop_bra = -1;
    int rc;
    switch (g->kind) {
    case OMG_G1_I8: case OMG_G2A_CRUMB: rc = build_simt_imad(g, k); break;
    case OMG_G2B_PLANE: rc = build_plane(g, k); break;
    case OMG_G3_BF16: rc = build_hmma(g, k); break;
    default: rc = -1;
    }
    if (rc == 0) rc = finish(k, out);
    free(k);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 3. packers and CPU models                                           */
/* ------------------------------------------------------------------ */

static uint16_t bf16_of_int(int v) {
    float f = (float)v;
    uint32_t b;
    memcpy(&b, &f, 4);
    return (uint16_t)(b >> 16); /* exact for |v| <= 256: low 16 bits are zero */
}
static int int_of_bf16(uint16_t h) {
    uint32_t b = (uint32_t)h << 16;
    float f;
    memcpy(&f, &b, 4);
    return (int)f;
}

int omg_pack_weights(const omg_geom *g, const int8_t *w, uint32_t *w0, uint32_t *w1) {
    if (!g || !w || !w0) return -1;
    uint32_t m = g->m, n = g->n;
    for (size_t i = 0; i < (size_t)m * n; i++) if (w[i] < -1 || w[i] > 1) return -1;
    switch (g->kind) {
    case OMG_G1_I8:
    case OMG_G2A_CRUMB: {
        uint32_t bits = g->kind == OMG_G1_I8 ? 8 : 2, wpw = g->wpw;
        int off = g->kind == OMG_G1_I8 ? 128 : 1;
        for (uint32_t kw = 0; kw < g->units_per_row; kw++)
            for (uint32_t r = 0; r < m; r++) {
                uint32_t word = 0;
                for (uint32_t b = 0; b < wpw; b++)
                    word |= (uint32_t)(w[(size_t)r * n + kw * wpw + b] + off) << (bits * b);
                w0[(size_t)kw * m + r] = word;
            }
        return 0;
    }
    case OMG_G2B_PLANE:
        if (!w1) return -1;
        for (uint32_t kw = 0; kw < g->units_per_row; kw++)
            for (uint32_t r = 0; r < m; r++) {
                uint32_t pw = 0, mw = 0;
                for (uint32_t b = 0; b < 32; b++) {
                    int v = w[(size_t)r * n + kw * 32 + b];
                    if (v == 1) pw |= 1u << b;
                    if (v == -1) mw |= 1u << b;
                }
                w0[(size_t)kw * m + r] = pw;
                w1[(size_t)kw * m + r] = mw;
            }
        return 0;
    case OMG_G3_BF16: {
        uint32_t C = g->units_per_row;
        for (uint32_t c = 0; c < C; c++)
            for (uint32_t tile = 0; tile < g->tiles; tile++)
                for (uint32_t r = 0; r < 4; r++)
                    for (uint32_t lane = 0; lane < 32; lane++) {
                        uint32_t gq = lane >> 2, tq = lane & 3;
                        uint32_t row = tile * 16 + gq + ((r & 1) ? 8 : 0);
                        uint32_t kk = c * 16 + tq * 2 + ((r & 2) ? 8 : 0);
                        uint32_t lo = 0, hi = 0;
                        if (row < m) {
                            lo = bf16_of_int(w[(size_t)row * n + kk]);
                            hi = bf16_of_int(w[(size_t)row * n + kk + 1]);
                        }
                        w0[(((size_t)c * g->tiles + tile) * 4 + r) * 32 + lane] = lo | (hi << 16);
                    }
        return 0;
    }
    }
    return -1;
}

int omg_pack_x(const omg_geom *g, const int8_t *x, uint32_t *xd) {
    if (!g || !x || !xd) return -1;
    uint32_t n = g->n;
    switch (g->kind) {
    case OMG_G1_I8: case OMG_G2A_CRUMB:
        for (uint32_t i = 0; i < n; i++) xd[i] = (uint32_t)(int32_t)x[i];
        return 0;
    case OMG_G2B_PLANE:
        for (uint32_t kw = 0; kw < n / 32; kw++) {
            uint32_t pl[8] = { 0 };
            for (uint32_t b = 0; b < 32; b++) {
                uint32_t v = (uint8_t)x[kw * 32 + b];
                for (int j = 0; j < 8; j++) pl[j] |= ((v >> j) & 1u) << b;
            }
            memcpy(&xd[kw * 8], pl, sizeof pl);
        }
        return 0;
    case OMG_G3_BF16:
        for (uint32_t i = 0; i < n; i += 2)
            xd[i / 2] = (uint32_t)bf16_of_int(x[i]) | ((uint32_t)bf16_of_int(x[i + 1]) << 16);
        return 0;
    }
    return -1;
}

static uint32_t popc32(uint32_t v) { return (uint32_t)__builtin_popcount(v); }

int omg_model(const omg_geom *g, const uint32_t *w0, const uint32_t *w1, const uint32_t *xd, int32_t *y) {
    if (!g || !w0 || !xd || !y) return -1;
    for (uint32_t i = 0; i < g->m_pad; i++) y[i] = 0;
    uint32_t T = g->threads, U = g->U;
    if (g->kind == OMG_G3_BF16) {
        uint32_t NW = g->warps, total = g->tiles * g->units_per_row * 128u;
        for (uint32_t warp = 0; warp < NW; warp++) {
            uint32_t tile = warp & (g->tiles - 1), s = warp >> lg2(g->tiles);
            for (uint32_t lane = 0; lane < 32; lane++) {
                uint32_t gq = lane >> 2, tq = lane & 3;
                if (tq) continue;
                /* rows gq and gq+8, column 0: sum over the lane group's view */
                int64_t v0 = 0, v1 = 0;
                for (uint32_t aidx = warp * 128 + lane, xidx = s * 8 + tq; aidx < total;
                     aidx += U * NW * 128, xidx += U * g->S * 8)
                    for (uint32_t u = 0; u < U; u++)
                        for (uint32_t l = 0; l < 4; l++) { /* the 4 lanes that hold row gq's k pairs */
                            uint32_t ln = gq * 4 + l;
                            uint32_t ai = aidx - lane + ln + u * NW * 128;
                            uint32_t xi = xidx - tq + l + u * g->S * 8;
                            for (uint32_t r = 0; r < 4; r++) {
                                uint32_t a = w0[ai + r * 32], xw = xd[xi + ((r & 2) ? 4 : 0)];
                                int prod = int_of_bf16((uint16_t)a) * int_of_bf16((uint16_t)xw) +
                                           int_of_bf16((uint16_t)(a >> 16)) * int_of_bf16((uint16_t)(xw >> 16));
                                if (r & 1) v1 += prod; else v0 += prod;
                            }
                        }
                y[tile * 16 + gq] += (int32_t)v0;
                y[tile * 16 + gq + 8] += (int32_t)v1;
            }
        }
        return 0;
    }
    uint32_t total = g->m * g->units_per_row;
    for (uint32_t t = 0; t < T; t++) {
        uint32_t row = t & (g->m - 1), s = t >> lg2(g->m);
        uint32_t acc = 0, sx = 0;
        uint32_t A[8] = { 0 }, B[8] = { 0 };
        for (uint32_t widx = t, i = 0; widx < total; widx += U * T, i++)
            for (uint32_t u = 0; u < U; u++) {
                uint32_t wi = widx + u * T;
                if (g->kind == OMG_G2B_PLANE) {
                    uint32_t xi = (s + (i * U + u) * g->S) * 8;
                    for (int j = 0; j < 8; j++) {
                        A[j] += popc32(w0[wi] & xd[xi + (uint32_t)j]);
                        B[j] += popc32(w1[wi] & xd[xi + (uint32_t)j]);
                    }
                } else {
                    uint32_t bits = g->kind == OMG_G1_I8 ? 8 : 2, wpw = g->wpw;
                    uint32_t xi = (s + (i * U + u) * g->S) * wpw;
                    for (uint32_t b = 0; b < wpw; b++) {
                        uint32_t f = (w0[wi] >> (bits * b)) & ((1u << bits) - 1);
                        acc += f * xd[xi + b];
                        sx += xd[xi + b];
                    }
                }
            }
        uint32_t v;
        if (g->kind == OMG_G2B_PLANE) {
            v = 0;
            for (int j = 0; j < 8; j++) {
                uint32_t c = j == 7 ? (uint32_t)-128 : (1u << j);
                v += A[j] * c - B[j] * c;
            }
        } else {
            v = acc - (g->kind == OMG_G1_I8 ? 128u : 1u) * sx;
        }
        y[row] = (int32_t)((uint32_t)y[row] + v);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 4. launcher                                                         */
/* ------------------------------------------------------------------ */

/* Same channel setup words as omega_blackwell_submit.c:12-16. */
static const uint32_t SETUP_WORDS[18] = {
    0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
    0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
    0x20012559, 0x00000000,
};

#define ARGS_WORDS 12

struct omg_session {
    M16NativeContext ctx;
    NvrmMem pb, code, cbank, qmd, marker, buf[5];
    size_t bufsz[5];
    uint32_t gpr;
    uint32_t seq;
    int ok;
};

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int omg_open(omg_session **out, const omg_kernel *k, size_t w0, size_t w1, size_t x, size_t y, size_t ts) {
    if (!out || !k || !k->code) return -1;
    omg_session *s = calloc(1, sizeof *s);
    if (!s) return -1;
    if (m16_native_open(&s->ctx) != 0) { free(s); return -1; }
    if (m16_native_create_channel(&s->ctx) != 0) goto fail;
    if (nvrm_alloc(&s->ctx.rm, 0x10000, &s->pb) != 0) goto fail;
    s->ctx.pb_mem = s->pb;
    size_t cb = (k->code_bytes + 0xfff) & ~(size_t)0xfff;
    if (nvrm_alloc(&s->ctx.rm, cb ? cb : 0x1000, &s->code) != 0) goto fail;
    if (nvrm_alloc(&s->ctx.rm, 0x1000, &s->cbank) != 0) goto fail;
    if (nvrm_alloc(&s->ctx.rm, 0x10000, &s->qmd) != 0) goto fail;
    if (nvrm_alloc(&s->ctx.rm, 0x1000, &s->marker) != 0) goto fail;
    size_t sz[5] = { w0, w1, x, y, ts };
    for (int i = 0; i < 5; i++) {
        size_t b = (sz[i] + 0xfff) & ~(size_t)0xfff;
        if (b < 0x1000) b = 0x1000;
        if (nvrm_alloc(&s->ctx.rm, b, &s->buf[i]) != 0) goto fail;
        s->bufsz[i] = b;
        memset(s->buf[i].cpu, 0, b);
    }
    memcpy(s->code.cpu, k->code, k->code_bytes);
    s->gpr = k->gpr;
    s->ok = 1;
    *out = s;
    return 0;
fail:
    m16_native_close(&s->ctx);
    free(s);
    return -1;
}

void *omg_buf(omg_session *s, int which) {
    if (!s || which < 0 || which > 4) return NULL;
    return s->buf[which].cpu;
}

static void dma_words(uint32_t *pb, size_t *len, uint64_t dst, const uint32_t *src, uint32_t nw) {
    size_t l = *len;
    pb[l++] = nvrm_mthd(1, 0x0188, 2);
    pb[l++] = (uint32_t)(dst >> 32);
    pb[l++] = (uint32_t)dst;
    pb[l++] = nvrm_mthd(1, 0x0180, 2);
    pb[l++] = nw * 4;
    pb[l++] = 0x00000001;
    pb[l++] = nvrm_mthd(1, 0x01b0, 1);
    pb[l++] = 0x00000041;
    pb[l++] = (nw << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[l], src, nw * 4);
    l += nw;
    *len = l;
}

static void inline_qmd(uint32_t *pb, size_t *len, uint64_t va, const uint32_t *q) {
    size_t l = *len;
    pb[l++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[l++] = (1u << 30) | (uint32_t)((va >> 40) & 0x1ff);
    pb[l++] = (uint32_t)(va >> 8);
    memcpy(&pb[l], q, 96 * 4);
    l += 96;
    *len = l;
}

int omg_launch(omg_session *s, uint32_t cta, uint32_t grid, uint32_t warps, uint64_t *host_ns, uint64_t *gpu_ns) {
    if (!s || !s->ok || !cta || !grid) return -1;
    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver_2d(cbank_data, s->cbank.va, cta, 1, grid, 1);
    uint32_t args[ARGS_WORDS] = { 0 };
    uint64_t va[5];
    for (int i = 0; i < 5; i++) va[i] = s->buf[i].va;
    /* 0x380 w0, 0x388 x, 0x390 y, 0x398 ts, 0x3a0 w1 */
    uint64_t order[5] = { va[0], va[2], va[3], va[4], va[1] };
    for (int i = 0; i < 5; i++) { args[2 * i] = (uint32_t)order[i]; args[2 * i + 1] = (uint32_t)(order[i] >> 32); }
    memcpy(s->cbank.cpu, cbank_data, sizeof cbank_data);
    memcpy((uint8_t *)s->cbank.cpu + 0x380, args, sizeof args);

    uint64_t qmd0_va = s->qmd.va, qmd1_va = s->qmd.va + 0x1000, sem_va = s->qmd.va + 0x2000,
             scratch_va = s->qmd.va + 0x4000;
    OmegaBlackwellQmdConfig cfg = {
        .code_va = s->code.va, .cbank_va = s->cbank.va, .scratch_va = scratch_va, .sem_va = sem_va,
        .qmd0_va = qmd0_va, .qmd1_va = qmd1_va, .num_elements = cta * grid, .threads_per_block = cta,
        .grid_width = grid, .threads_x = cta, .threads_y = 1, .grid_x = grid, .grid_y = 1,
        .gpr_count = s->gpr,
    };
    uint32_t q0[OMEGA_BW_QMD_WORDS], q1[OMEGA_BW_QMD_WORDS];
    omega_blackwell_build_qmd0(q0, qmd0_va, qmd1_va);
    omega_blackwell_build_qmd1(q1, &cfg);
    if (omega_blackwell_verify_qmd_invariants(q1) != 0) return -1;
    memcpy(s->qmd.cpu, q0, sizeof q0);
    memcpy((uint8_t *)s->qmd.cpu + 0x1000, q1, sizeof q1);
    volatile uint32_t *hsem = (volatile uint32_t *)((uint8_t *)s->qmd.cpu + 0x2000);
    volatile uint32_t *hmark = (volatile uint32_t *)s->marker.cpu;
    *hsem = 0;
    uint32_t payload = 0x4d000000u | (++s->seq & 0xffffffu);
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t pb[1024];
    size_t l = 0;
    memcpy(pb, SETUP_WORDS, sizeof SETUP_WORDS);
    l += 18;
    dma_words(pb, &l, s->cbank.va, cbank_data, 224);
    dma_words(pb, &l, s->cbank.va + 0x380, args, ARGS_WORDS);
    inline_qmd(pb, &l, qmd0_va, q0);
    uint32_t init = 5;
    dma_words(pb, &l, sem_va, &init, 1);
    inline_qmd(pb, &l, qmd1_va, q1);
    pb[l++] = nvrm_mthd(0, 0x005c, 5);
    pb[l++] = (uint32_t)s->marker.va;
    pb[l++] = (uint32_t)(s->marker.va >> 32);
    pb[l++] = payload;
    pb[l++] = 0;
    pb[l++] = 0x1 | (1u << 20); /* RELEASE | WFI */

    uint64_t t0 = now_ns();
    if (m16_native_submit_methods(&s->ctx, pb, l) != 0) { s->ok = 0; return -1; }
    /* The completion mechanism the silicon tests use. A kernel is never
     * abandoned early: the bound is 300 s, far above any MA-6 kernel. */
    if (m16_native_wait_marker(hmark, payload, 300000) != 0) {
        s->ok = 0;
        fprintf(stderr, "omg: launch %u did not complete in 300 s\n", s->seq);
        return -1;
    }
    uint64_t t1 = now_ns();
    nvrm_retire(&s->ctx.rm, s->ctx.rm.put);
    if (*hsem != 6) { fprintf(stderr, "omg: release semaphore %u, want 6\n", *hsem); return -1; }
    if (host_ns) *host_ns = t1 - t0;
    if (gpu_ns) {
        *gpu_ns = 0;
        if (warps) {
            volatile uint32_t *ts = (volatile uint32_t *)s->buf[4].cpu;
            uint32_t base = ts[0];
            int64_t lo = 0, hi = 0;
            for (uint32_t w = 0; w < warps; w++) {
                int64_t a = (int32_t)(ts[2 * w] - base), b = (int32_t)(ts[2 * w + 1] - base);
                if (a < lo) lo = a;
                if (b > hi) hi = b;
            }
            *gpu_ns = (uint64_t)(hi - lo);
        }
    }
    return 0;
}

int omg_close(omg_session *s) {
    if (!s) return -1;
    int rc = m16_native_close(&s->ctx);
    free(s);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 5. silicon self-test                   */
/* ------------------------------------------------------------------ */

static const uint8_t ST_LUTS[8] = { 0xC0, 0xFC, 0x3C, 0x96, 0xE8, 0x80, 0x0F, 0xCA };
#define ST_THREADS 256u
#define ST_OUT 16u

static uint32_t lut_eval(uint8_t lut, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t idx = (((a >> i) & 1u) << 2) | (((b >> i) & 1u) << 1) | ((c >> i) & 1u);
        r |= ((uint32_t)(lut >> idx) & 1u) << i;
    }
    return r;
}

static int build_selftest(omg_kernel *out) {
    kb_t *k = calloc(1, sizeof *k);
    if (!k) return -1;
    omega_bw_ir_init(&k->prog);
    k->loop_top = k->loop_bra = -1;
    omg_geom g;
    memset(&g, 0, sizeof g);
    g.cta = 128;
    pro_t p; prologue(k, &g, &p, 0);
    int ax = V2(k), a = V(k), b = V(k), c = V(k), a1 = V(k), o[ST_OUT], bi = V(k), ao = V2(k), ix = V(k);
    int lo = V(k), ay = V2(k), old = V(k), t1 = V(k), yi = V(k);
    for (unsigned i = 0; i < ST_OUT; i++) o[i] = V(k);
    wide(k, ax, p.gtid, 4, p.px);
    ldg(k, a, 0, ax, p.ud, 0, SB_X);
    ldg(k, b, 0, ax, p.ud, 1024, SB_X);
    ldg(k, c, 0, ax, p.ud, 2048, SB_X);
    ldg(k, a1, 0, ax, p.ud, 4, SB_X);
    for (int i = 0; i < 8; i++) lop3(k, o[i], a, b, c, ST_LUTS[i]);
    popc(k, o[8], a);
    popc(k, o[9], b);
    add3(k, o[10], a, b, c);
    imadi(k, o[11], a1, 1, -1);
    imadi(k, o[12], p.t0, 1, -1);
    s2r(k, t1, SR_GLOBALTIMER_LO);
    imadi(k, o[13], t1, 1, -1);
    lop3(k, o[14], a, b, -1, 0xC0); /* two-input form with RZ */
    imadi(k, o[15], p.gtid, 1, -1);
    imadi(k, bi, p.gtid, ST_OUT, -1);
    for (unsigned i = 0; i < ST_OUT; i++) {
        imadi(k, ix, p.one, i, bi);
        wide(k, ao, ix, 4, p.pw0);
        stg(k, ao, o[i], p.ud);
    }
    andi(k, lo, a, 0xff);
    andi(k, yi, p.gtid, 7);
    wide(k, ay, yi, 4, p.py);
    atom_add(k, old, ay, lo, p.ud);
    exit_(k, 0);
    selfbra(k);
    int rc = finish(k, out);
    free(k);
    return rc;
}

int omg_selftest(char *rep, size_t cap) {
    omg_kernel k;
    if (build_selftest(&k) != 0) return -1;
    omg_session *s = NULL;
    size_t used = 0;
    if (omg_open(&s, &k, ST_THREADS * ST_OUT * 4, 0, 4096 * 4, 64, 0) != 0) { omg_kernel_free(&k); return -1; }
    uint32_t *in = omg_buf(s, 2), *out = omg_buf(s, 0), *y = omg_buf(s, 3);
    uint32_t st = 0x9e3779b9u;
    uint32_t A[ST_THREADS + 1], Bv[ST_THREADS], Cv[ST_THREADS];
    for (unsigned i = 0; i < ST_THREADS + 1; i++) { st = st * 1664525u + 1013904223u; A[i] = st ^ (st >> 7); }
    for (unsigned i = 0; i < ST_THREADS; i++) { st = st * 1664525u + 1013904223u; Bv[i] = st; st = st * 1664525u + 1013904223u; Cv[i] = st >> 3; }
    A[0] = 0xffffffffu; A[1] = 0; Bv[0] = 0xffffffffu; Bv[1] = 0x80000001u;
    static uint32_t inh[768];
    for (unsigned i = 0; i < ST_THREADS; i++) { inh[i] = A[i]; inh[256 + i] = Bv[i]; inh[512 + i] = Cv[i]; }
    for (unsigned i = 0; i < 768; i++) in[i] = inh[i];
    for (unsigned i = 0; i < ST_THREADS * ST_OUT; i++) out[i] = 0xdeadbeefu;
    for (int i = 0; i < 8; i++) y[i] = 0;
    uint64_t hn = 0;
    int fails = 0;
    if (omg_launch(s, 128, ST_THREADS / 128, 0, &hn, NULL) != 0) { omg_close(s); omg_kernel_free(&k); return -1; }
    int bad[16] = { 0 };
    uint32_t tsd_max = 0;
    for (unsigned t = 0; t < ST_THREADS; t++) {
        const uint32_t *o = &out[t * ST_OUT];
        for (int i = 0; i < 8; i++) if (o[i] != lut_eval(ST_LUTS[i], A[t], Bv[t], Cv[t])) bad[i]++;
        if (o[8] != popc32(A[t])) bad[8]++;
        if (o[9] != popc32(Bv[t])) bad[9]++;
        if (o[10] != A[t] + Bv[t] + Cv[t]) bad[10]++;
        if (o[11] != inh[t + 1]) bad[11]++;
        uint32_t d = o[13] - o[12];
        if (d > 1000000u) bad[12]++;
        if (d > tsd_max) tsd_max = d;
        if (o[14] != (A[t] & Bv[t])) bad[14]++;
        if (o[15] != t) bad[15]++;
    }
    uint32_t yw[8] = { 0 };
    for (unsigned t = 0; t < ST_THREADS; t++) yw[t & 7] += A[t] & 0xff;
    for (int i = 0; i < 8; i++) if (y[i] != yw[i]) bad[13]++;
    static const char *names[16] = {
        "LOP3 LUT 0xC0 (a&b) reg Rc", "LOP3 LUT 0xFC (a|b)", "LOP3 LUT 0x3C (a^b)", "LOP3 LUT 0x96 (a^b^c)",
        "LOP3 LUT 0xE8 (majority)", "LOP3 LUT 0x80 (a&b&c)", "LOP3 LUT 0x0F (~a)", "LOP3 LUT 0xCA (a?b:c)",
        "POPC a", "POPC b", "IADD3 a+b+c (third operand)", "LDG immediate offset +4",
        "%globaltimer two reads ordered and < 1 ms apart", "ATOMG.ADD.STRONG.SYS 256 adds into 8 words",
        "LOP3 LUT 0xC0 with Rc = RZ", "thread id round trip" };
    for (int i = 0; i < 16; i++) {
        if (bad[i]) fails++;
        if (rep && used < cap)
            used += (size_t)snprintf(rep + used, cap - used, "%s %s (%d mismatches)\n", bad[i] ? "FAIL" : "PASS", names[i], bad[i]);
    }
    if (rep && used < cap)
        used += (size_t)snprintf(rep + used, cap - used, "INFO selftest host launch %llu ns, max in-thread timer delta %u ns\n",
                                 (unsigned long long)hn, tsd_max);
    omg_close(s);
    omg_kernel_free(&k);
    return fails;
}
