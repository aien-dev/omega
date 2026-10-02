/* test_omega_program_ir.c: the canonical program IR and the id recomputation behind the
 * mandatory source/IR recheck (VC1 stage 6, SPEC 6 step 3). Each guard in
 * src/omega_program_ir.c carries a VC1I tag; the Makefile target test-program-ir breaks each tag
 * in a copy and requires the named check to FAIL. CPU only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_program.h"
#include "omega_program_ir.h"

static int g_total, g_failed;
static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    g_total++;
    if (!ok) g_failed++;
}
static OmegaProgram *newprog(int op, uint64_t imm) {
    OmegaProgram *p = calloc(1, sizeof *p);
    if (!p || omega_program_build_unary_op(p, "ir-test", op, imm) != 0) { fprintf(stderr, "setup failed\n"); exit(2); }
    return p;
}
/* fixed offsets of the layout in omega_program_ir.h */
enum { OFF_IN_TYPE = 24, OFF_STEPS = 24 + 1 + 2 + 1 + 2 + 32 + 32, OFF_FIRST_STEP = OFF_STEPS + 2 };

static void t_roundtrip(void) {
    static const int ops[5] = { OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR };
    static const uint64_t imms[4] = { 0, 1, 0x01234567ULL, 0xffffffffULL };
    int all = 1, same_id = 1, canon = 1, distinct = 1;
    uint8_t seen[20][32]; int ns = 0;
    for (int a = 0; a < 5; a++)
        for (int b = 0; b < 4; b++) {
            OmegaProgram *p = newprog(ops[a], imms[b]);
            uint8_t *ir = NULL; size_t n = 0;
            OmegaProgram *q = calloc(1, sizeof *q);
            if (omega_program_ir_encode(p, &ir, &n) != 0) { all = 0; goto next; }
            if (omega_program_ir_decode(ir, n, q) != 0) { all = 0; goto next; }
            if (memcmp(q->program_id.bytes, p->program_id.bytes, 32) != 0) same_id = 0;
            uint8_t id[32];
            if (omega_program_ir_recompute_id(ir, n, id) != 0 || memcmp(id, p->program_id.bytes, 32) != 0) same_id = 0;
            uint8_t *ir2 = NULL; size_t n2 = 0;
            if (omega_program_ir_encode(q, &ir2, &n2) != 0 || n2 != n || memcmp(ir, ir2, n) != 0) canon = 0;
            free(ir2);
            for (int k = 0; k < ns; k++) if (memcmp(seen[k], id, 32) == 0) distinct = 0;
            memcpy(seen[ns++], id, 32);
next:
            free(ir); free(q); free(p);
        }
    check("ir-roundtrips-every-op-and-immediate", all);
    check("ir-recomputes-the-program-id-exactly", same_id);
    check("ir-encoding-is-canonical-decode-then-encode-is-identical", canon);
    check("ir-different-programs-have-different-recomputed-ids", distinct);
}

static void t_refusals(void) {
    OmegaProgram *p = newprog(OP_ADD, 17);
    uint8_t *ir = NULL; size_t n = 0;
    if (omega_program_ir_encode(p, &ir, &n) != 0) { fprintf(stderr, "setup failed\n"); exit(2); }
    OmegaProgram *q = calloc(1, sizeof *q);
    uint8_t id[32];

    check("ir-has-the-documented-length-for-one-step", n == (size_t)OFF_FIRST_STEP + 9);
    check("ir-refuses-null-arguments", omega_program_ir_decode(NULL, n, q) != 0 && omega_program_ir_decode(ir, n, NULL) != 0 && omega_program_ir_recompute_id(NULL, 0, id) != 0);

    int all = 1;
    for (size_t k = 0; k < n; k++) {      /* an exact-size heap copy, so a read past the end is a sanitizer error, not a silent success */
        uint8_t *cut = malloc(k ? k : 1);
        if (!cut) { fprintf(stderr, "setup failed\n"); exit(2); }
        memcpy(cut, ir, k);
        if (omega_program_ir_decode(cut, k, q) == 0) all = 0;
        free(cut);
    }
    check("ir-refuses-truncation-at-every-length", all);

    uint8_t *t = malloc(n + 1); memcpy(t, ir, n); t[n] = 0;
    check("ir-refuses-trailing-bytes", omega_program_ir_decode(t, n + 1, q) != 0 && omega_program_ir_decode(ir, n, q) == 0);

    memcpy(t, ir, n); t[0] ^= 1;
    check("ir-refuses-wrong-tag", omega_program_ir_decode(t, n, q) != 0);

    /* 65 steps, all of them present: the limit refuses, not truncation */
    {
        size_t m = (size_t)OFF_STEPS + 2 + 65 * 9;
        uint8_t *big = calloc(1, m);
        memcpy(big, ir, OFF_STEPS);
        big[OFF_STEPS] = 0; big[OFF_STEPS + 1] = 65;
        for (int i = 0; i < 65; i++) big[OFF_FIRST_STEP + 9 * i] = OP_ADD;
        check("ir-refuses-more-steps-than-the-program-limit", omega_program_ir_decode(big, m, q) != 0);
        free(big);
    }
    /* far over the limit with all the data present: the decoder must stop before it writes past its output */
    {
        size_t m = (size_t)OFF_STEPS + 2 + 4000 * 9;
        uint8_t *big = calloc(1, m);
        memcpy(big, ir, OFF_STEPS);
        big[OFF_STEPS] = (uint8_t)(4000 >> 8); big[OFF_STEPS + 1] = (uint8_t)(4000 & 255);
        for (int i = 0; i < 4000; i++) big[OFF_FIRST_STEP + 9 * i] = OP_ADD;
        check("ir-refuses-a-huge-step-count-without-writing-past-its-output", omega_program_ir_decode(big, m, q) != 0);
        free(big);
    }

    /* an input type the program id does not accept: refused, never a made-up id */
    memcpy(t, ir, n); t[OFF_IN_TYPE] = 0xEE;
    check("ir-refuses-a-type-the-program-id-cannot-take", omega_program_ir_decode(t, n, q) != 0);

    /* injectivity: flipping any single bit either refuses or changes the id (one program, one byte string) */
    int inj = 1;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            memcpy(t, ir, n); t[i] ^= (uint8_t)(1u << b);
            if (omega_program_ir_recompute_id(t, n, id) == 0 && memcmp(id, p->program_id.bytes, 32) == 0) inj = 0;
        }
    check("ir-no-single-bit-change-keeps-the-same-id", inj);

    check("ir-encode-refuses-a-program-with-no-body", ({ OmegaProgram *e = calloc(1, sizeof *e); uint8_t *o = NULL; size_t l = 0; int r = omega_program_ir_encode(e, &o, &l); free(e); r != 0; }));
    free(t); free(q); free(ir); free(p);
}

int main(void) {
    t_roundtrip();
    t_refusals();
    printf("%d checks, %d failed\n", g_total, g_failed);
    if (g_failed) return 1;
    printf("test-program-ir: PASS\n");
    return 0;
}
