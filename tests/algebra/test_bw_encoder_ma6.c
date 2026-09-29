/* Host checks for the four sm_121 encoder ops added for MA-6
 * (src/omega_blackwell_codegen.{h,c}: BW_IR_LOP3_LUT, BW_IR_POPC,
 * BW_IR_IADD3_R3, BW_IR_LDG_E_OFF).
 *
 * 1. The existing encoder fixtures still pass (earlier ops' bytes unchanged).
 * 2. Each new op, at its neutral setting, reproduces the words of the
 *    existing verified op it generalises (LOP3_LUT 0x3c with RZ == LOP3_XOR,
 *    IADD3_R3 with RZ == IADD3 words 0-2, LDG_E_OFF with 0 == LDG_E), and the
 *    extra field lands only in its documented bits.
 * 3. POPC field layout; out-of-range LDG offsets are refused.
 * Host checks show the fields are where the layout says; the silicon
 * self-test (tests/algebra/ma6_gpu.c --selftest) shows the chip agrees.
 */
#include "omega_blackwell_codegen.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (c) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static BlackwellIRInsn I(BlackwellIROpcode o, int d, int a, int b, int c) {
    BlackwellIRInsn n;
    memset(&n, 0, sizeof n);
    n.op = o; n.dst_vreg = d; n.src1_vreg = a; n.src2_vreg = b; n.src3_vreg = c; n.ureg = -1;
    return n;
}

int main(void) {
    CHECK(omega_blackwell_verify_codegen_fixtures() == 0, "existing codegen fixtures unchanged");

    BlackwellIRProgram p;
    omega_bw_ir_init(&p);
    int a = omega_bw_ir_alloc_vreg(&p), b = omega_bw_ir_alloc_vreg(&p), c = omega_bw_ir_alloc_vreg(&p);
    int d = omega_bw_ir_alloc_vreg(&p), e = omega_bw_ir_alloc_vreg(&p);
    int ad = omega_bw_ir_alloc_vreg64(&p), ud = omega_bw_ir_alloc_uvreg64(&p);
    BlackwellIRInsn x;
    x = I(BW_IR_MOV_IMM, a, -1, -1, -1); omega_bw_ir_append(&p, &x);
    x = I(BW_IR_MOV_IMM, b, -1, -1, -1); omega_bw_ir_append(&p, &x);
    x = I(BW_IR_MOV_IMM, c, -1, -1, -1); omega_bw_ir_append(&p, &x);
    x = I(BW_IR_LDCU64, ud, -1, -1, -1); x.is_uniform = true; x.imm = 0x358; omega_bw_ir_append(&p, &x);
    x = I(BW_IR_LDC64, ad, -1, -1, -1); x.imm = 0x380; omega_bw_ir_append(&p, &x);          /* 4 */
    x = I(BW_IR_LOP3_XOR, d, a, b, -1); omega_bw_ir_append(&p, &x);                        /* 5 */
    x = I(BW_IR_LOP3_LUT, d, a, b, -1); x.imm = 0x3c; omega_bw_ir_append(&p, &x);          /* 6 */
    x = I(BW_IR_LOP3_LUT, d, a, b, c); x.imm = 0x96; omega_bw_ir_append(&p, &x);           /* 7 */
    x = I(BW_IR_IADD3, e, a, b, -1); omega_bw_ir_append(&p, &x);                           /* 8 */
    x = I(BW_IR_IADD3_R3, e, a, b, -1); omega_bw_ir_append(&p, &x);                        /* 9 */
    x = I(BW_IR_IADD3_R3, e, a, b, c); omega_bw_ir_append(&p, &x);                         /* 10 */
    x = I(BW_IR_LDG_E, d, ad, -1, -1); x.ureg = ud; omega_bw_ir_append(&p, &x);            /* 11 */
    x = I(BW_IR_LDG_E_OFF, d, ad, -1, -1); x.ureg = ud; x.imm = 0; omega_bw_ir_append(&p, &x);          /* 12 */
    x = I(BW_IR_LDG_E_OFF, d, ad, -1, -1); x.ureg = ud; x.imm = 0x123454; omega_bw_ir_append(&p, &x);   /* 13 */
    x = I(BW_IR_LDG_E_OFF, d, ad, -1, -1); x.ureg = ud; x.imm = (uint32_t)-8; omega_bw_ir_append(&p, &x); /* 14 */
    x = I(BW_IR_POPC, e, a, -1, -1); omega_bw_ir_append(&p, &x);                           /* 15 */
    x = I(BW_IR_EXIT, -1, -1, -1, -1); omega_bw_ir_append(&p, &x);
    for (int v = 0; v < p.regalloc.num_vregs; v++) { p.regalloc.intervals[v].first_def = 0; p.regalloc.intervals[v].last_use = (int)p.count - 1; }
    for (int v = 0; v < p.regalloc.num_uvregs; v++) { p.regalloc.uintervals[v].first_def = 0; p.regalloc.uintervals[v].last_use = (int)p.count - 1; }
    CHECK(omega_bw_regalloc_solve(&p) == 0, "regalloc");
    uint8_t code[64 * 16];
    size_t len = 0;
    CHECK(omega_bw_encode_program(&p, code, sizeof code, &len) == 0, "encode program with the new ops");
    uint32_t w[17][4];
    for (int i = 0; i < 17; i++) memcpy(w[i], code + 16 * i, 16);
    uint32_t pa = (uint32_t)p.regalloc.vreg_to_phys[a], pc = (uint32_t)p.regalloc.vreg_to_phys[c];
    uint32_t pe = (uint32_t)p.regalloc.vreg_to_phys[e];

    CHECK(memcmp(w[5], w[6], 12) == 0 && w[6][3] == 0x001fca00u, "LOP3_LUT 0x3c, Rc=RZ == LOP3_XOR words 0-2");
    CHECK(w[7][0] == w[5][0] && w[7][1] == w[5][1] && w[7][2] == (0x078e0000u | (0x96u << 8) | pc),
          "LOP3_LUT: LUT in w2 bits 8-15, Rc in w2 bits 0-7, nothing else moves");
    CHECK(memcmp(w[8], w[9], 12) == 0, "IADD3_R3 with Rc=RZ == IADD3 words 0-2");
    CHECK(w[10][0] == w[8][0] && w[10][1] == w[8][1] && w[10][2] == (0x07ffe000u | pc), "IADD3_R3: Rc in w2 bits 0-7 only");
    CHECK(memcmp(w[11], w[12], 16) == 0, "LDG_E_OFF with offset 0 == LDG_E (all 4 words)");
    CHECK(w[13][0] == w[11][0] && w[13][2] == w[11][2] && (w[13][1] & 0xffu) == (w[11][1] & 0xffu) &&
          (w[13][1] >> 8) == 0x123454u, "LDG_E_OFF: offset in bits 40-63, UR byte kept");
    CHECK((w[14][1] >> 8) == 0xfffff8u, "LDG_E_OFF: negative offset is 24-bit two's complement");
    CHECK(w[15][0] == (0x7309u | (pe << 16)) && w[15][1] == pa && w[15][2] == 0 && w[15][3] == 0x000e2200u,
          "POPC Rd, Rb: 0x309, Rd bits 16-23, source bits 32-39, default control sets scoreboard 0");

    BlackwellIRProgram q;
    omega_bw_ir_init(&q);
    int qd = omega_bw_ir_alloc_vreg(&q), qa = omega_bw_ir_alloc_vreg64(&q), qu = omega_bw_ir_alloc_uvreg64(&q);
    x = I(BW_IR_LDG_E_OFF, qd, qa, -1, -1); x.ureg = qu; x.imm = 1u << 23; omega_bw_ir_append(&q, &x);
    for (int v = 0; v < q.regalloc.num_vregs; v++) { q.regalloc.intervals[v].first_def = 0; q.regalloc.intervals[v].last_use = 0; }
    q.regalloc.uintervals[0].first_def = 0; q.regalloc.uintervals[0].last_use = 0;
    omega_bw_regalloc_solve(&q);
    CHECK(omega_bw_encode_program(&q, code, sizeof code, &len) != 0, "LDG_E_OFF refuses offset 2^23");

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
