/* Checks Omega's branch encoder against words produced by the chip compiler.
 * The compiler output is the reference. This program does not launch anything. */
#include "omega_blackwell_codegen.h"

#include <stdio.h>
#include <stdint.h>

typedef struct {
    int32_t delta;
    int pred; /* 0 always, 1 @P0, 2 @!P0 */
    uint32_t w0, w1, w2;
} BraOracle;

static const BraOracle ORACLE[] = {
    /* Resident worker, read back from the chip compiler. */
    { 102, 1, 0x00940947u, 0x00000004u, 0x03800000u },
    {  94, 2, 0x00748947u, 0x00000004u, 0x03800000u },
    {  85, 2, 0x00508947u, 0x00000004u, 0x03800000u },
    {  67, 1, 0x00080947u, 0x00000004u, 0x03800000u },
    {  63, 1, 0x00f80947u, 0x00000000u, 0x03800000u },
    {  54, 1, 0x00d40947u, 0x00000000u, 0x03800000u },
    {  50, 1, 0x00c40947u, 0x00000000u, 0x03800000u },
    {  47, 2, 0x00b88947u, 0x00000000u, 0x03800000u },
    {  41, 1, 0x00a00947u, 0x00000000u, 0x03800000u },
    {  30, 1, 0x00740947u, 0x00000000u, 0x03800000u },
    {  27, 2, 0x00688947u, 0x00000000u, 0x03800000u },
    {   6, 0, 0x00147947u, 0x00000000u, 0x03800000u },
    { -95, 2, 0x00808947u, 0xfffffff8u, 0x0383ffffu },
    {   0, 0, 0x00fc7947u, 0xfffffffcu, 0x0383ffffu },
    /* Separate distance ladder, including a long forward jump. */
    {   9, 1, 0x00200947u, 0x00000000u, 0x03800000u },
    {  65, 1, 0x00000947u, 0x00000004u, 0x03800000u },
    { 257, 1, 0x00000947u, 0x00000010u, 0x03800000u },
    {  -2, 1, 0x00f40947u, 0xfffffffcu, 0x0383ffffu },
};

static int encode_one(const BraOracle *o, uint32_t out[4]) {
    BlackwellIRProgram prog;
    omega_bw_ir_init(&prog);
    BlackwellIRInsn insn;
    insn = (BlackwellIRInsn){0};
    insn.op = BW_IR_BRA;
    insn.imm = (uint32_t)o->delta;
    insn.predicate_p0 = (o->pred != 0);
    insn.predicate_not = (o->pred == 2);
    if (omega_bw_ir_append(&prog, &insn) != 0) return -1;
    uint8_t buf[512];
    size_t len = 0;
    if (omega_bw_encode_program(&prog, buf, sizeof buf, &len) != 0) return -2;
    for (int i = 0; i < 4; i++) {
        out[i] = (uint32_t)buf[i * 4]
               | ((uint32_t)buf[i * 4 + 1] << 8)
               | ((uint32_t)buf[i * 4 + 2] << 16)
               | ((uint32_t)buf[i * 4 + 3] << 24);
    }
    return 0;
}

int main(void) {
    int fail = 0;
    int n = (int)(sizeof ORACLE / sizeof ORACLE[0]);
    for (int i = 0; i < n; i++) {
        uint32_t w[4];
        if (encode_one(&ORACLE[i], w) != 0) {
            printf("encode failed at %d\n", i);
            fail++;
            continue;
        }
        if (w[0] != ORACLE[i].w0 || w[1] != ORACLE[i].w1 || w[2] != ORACLE[i].w2) {
            printf("mismatch %d delta=%d pred=%d\n", i, ORACLE[i].delta, ORACLE[i].pred);
            fail++;
        }
    }
    /* The padding branch used after a finished kernel must keep its old bytes. */
    {
        uint32_t w[4];
        BraOracle spin = { 0, 0, 0x00fc7947u, 0xfffffffcu, 0x0383ffffu };
        if (encode_one(&spin, w) != 0 || w[3] != 0x000fc000u) {
            printf("self-branch padding changed\n");
            fail++;
        }
    }
    if (fail) {
        printf("FAIL %d\n", fail);
        return 1;
    }
    printf("PASS %d branch words match the chip compiler\n", n);
    return 0;
}
