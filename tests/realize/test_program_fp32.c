/* test_program_fp32.c -- FP32 in the program IR (E1 gap row 12, spec/program-fp32.md).
 * Gate OMEGA_PROGRAM_FP32_PASS: type check refuses mixed types, conversions are explicit,
 * ids bind the types, evaluation equals an independent host-float evaluator, and the AArch64
 * realizer refuses FP32 programs. Physics-free; opens no device. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "omega_core.h"
#include "omega_program.h"
#include "omega_program_fp32.h"
#include "omega_numeric.h"

static int fails, total;
#define CHECK(c, msg) do { total++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* independent evaluator: host float arithmetic (built with -ffp-contract=off) */
static uint32_t host_ref(const OmegaProgram *p, uint32_t x) {
    float v = bf(x);
    for (uint16_t i = 0; i < p->body.step_count; ++i) {
        uint8_t op = p->body.steps[i].op;
        float k = bf((uint32_t)p->body.steps[i].imm);
        if (op == OP_ADD) v = v + k;
        else if (op == OP_SUB) v = v - k;
        else if (op == OP_MUL) v = v * k;
        else if (op == OP_DIV) v = v / k;
    }
    return fbits(v);
}

int main(void) {
    char why[200];
    OmegaProgram a, b, c;
    OmegaProgramStep add1[] = { { OP_ADD, fbits(1.0f) } };
    OmegaProgramStep mix[] = { { OP_ADD, fbits(1.0f) }, { OP_MUL, fbits(2.5f) }, { OP_DIV, fbits(3.0f) }, { OP_SUB, fbits(0.5f) } };

    /* 1. a valid FP32 program builds, checks, has an id */
    CHECK(omega_program_build_fp32(&a, "fp_add1", TYPE_FP32, TYPE_FP32, add1, 1) == 0, "build fp32 add");
    CHECK(omega_program_fp32_check(&a, why, sizeof why) == 0, "check fp32 add");
    CHECK(omega_program_build_fp32(&b, "fp_mix", TYPE_FP32, TYPE_FP32, mix, 4) == 0, "build fp32 mix");

    /* 2. id binds the type: same steps over u32 is refused, never aliases */
    CHECK(omega_program_build_fp32(&c, "u32_add1", TYPE_UNSIGNED_INT, TYPE_UNSIGNED_INT, add1, 1) != 0,
          "ADD on u32 refused without a conversion");
    OmegaProgramStep cv_fp[] = { { OP_CONVERT, TYPE_FP32 } };
    OmegaProgram u2f, f2u;
    CHECK(omega_program_build_convert(&u2f, "u2f", TYPE_UNSIGNED_INT, TYPE_FP32) == 0, "build u32->fp32");
    CHECK(omega_program_build_convert(&f2u, "f2u", TYPE_FP32, TYPE_UNSIGNED_INT) == 0, "build fp32->u32");
    CHECK(memcmp(u2f.program_id.bytes, f2u.program_id.bytes, OMEGA_ID_BYTES) != 0, "convert ids differ");
    CHECK(memcmp(a.program_id.bytes, b.program_id.bytes, OMEGA_ID_BYTES) != 0, "fp32 program ids differ by body");
    OmegaProgram a2;
    CHECK(omega_program_build_fp32(&a2, "renamed", TYPE_FP32, TYPE_FP32, add1, 1) == 0 &&
          memcmp(a.program_id.bytes, a2.program_id.bytes, OMEGA_ID_BYTES) == 0, "name is metadata, id stable");
    OmegaProgramStep add_other[] = { { OP_ADD, fbits(2.0f) } };
    OmegaProgram a3;
    CHECK(omega_program_build_fp32(&a3, "fp_add2", TYPE_FP32, TYPE_FP32, add_other, 1) == 0 &&
          memcmp(a.program_id.bytes, a3.program_id.bytes, OMEGA_ID_BYTES) != 0, "constant is part of the id");

    /* 3. type check refuses mixed types and non-FP32 ops */
    OmegaProgramStep bad_and[] = { { OP_AND, 0xFF } };
    CHECK(omega_program_build_fp32(&c, "and", TYPE_FP32, TYPE_FP32, bad_and, 1) != 0, "AND on FP32 refused");
    OmegaProgramStep mixed[] = { { OP_CONVERT, TYPE_UNSIGNED_INT }, { OP_ADD, 1 } };
    CHECK(omega_program_build_fp32(&c, "mixed", TYPE_FP32, TYPE_UNSIGNED_INT, mixed, 2) != 0, "ADD after CONVERT-to-u32 refused");
    CHECK(omega_program_build_fp32(&c, "wrongout", TYPE_FP32, TYPE_UNSIGNED_INT, add1, 1) != 0, "body type != contract output refused");
    CHECK(omega_program_build_fp32(&c, "noop", TYPE_FP32, TYPE_FP32, cv_fp, 1) != 0, "CONVERT to same type refused");
    OmegaProgramStep big[] = { { OP_ADD, 0x100000000ull } };
    CHECK(omega_program_build_fp32(&c, "big", TYPE_FP32, TYPE_FP32, big, 1) != 0, "constant wider than 32 bits refused");
    /* tampered body: stale id refused */
    OmegaProgram t = a; t.body.steps[0].imm = fbits(9.0f);
    CHECK(omega_program_fp32_check(&t, why, sizeof why) != 0, "tampered body refused");
    /* contract width 64 refused */
    t = a; t.contract.input_width = 64;
    CHECK(omega_program_fp32_check(&t, why, sizeof why) != 0, "width 64 refused");

    /* 4. composition: FP32 then FP32 ok; u32 meets FP32 only through convert */
    CHECK(omega_program_compose(&a, &b, &c, why, sizeof why) == 0, "compose fp32 . fp32");
    CHECK(c.body.step_count == 5 && omega_program_fp32_check(&c, why, sizeof why) == 0, "composition is a valid fp32 program");
    CHECK(omega_program_compose(&a, &f2u, &c, why, sizeof why) == 0 && omega_program_fp32_check(&c, why, sizeof why) == 0,
          "compose fp32 then convert to u32");
    CHECK(omega_program_compose(&f2u, &a, &c, why, sizeof why) != 0, "u32-out program cannot feed an FP32 program directly");
    CHECK(omega_program_compose(&u2f, &a, &c, why, sizeof why) == 0 && omega_program_fp32_check(&c, why, sizeof why) == 0,
          "u32 -> convert -> fp32 op composes");

    /* 5. evaluation equals the independent host evaluator and the library definitions */
    static const uint32_t pat[] = { 0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
                                    0x00000001u, 0x007fffffu, 0x00800000u, 0x7f7fffffu, 0x4b800000u, 0x33800000u, 0x3effffffu };
    uint32_t xs[4096], ys[4096];
    size_t n = 0;
    for (size_t i = 0; i < sizeof pat / sizeof pat[0]; ++i) xs[n++] = pat[i];
    uint64_t s = 0x9e3779b97f4a7c15ull;
    while (n < 4096) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; xs[n++] = (uint32_t)(s >> 16); }
    const OmegaProgram *progs[] = { &a, &b, &a3 };
    int eq = 1;
    for (int pi = 0; pi < 3; ++pi) {
        CHECK(omega_program_fp32_eval(progs[pi], xs, n, ys) == 0, "eval fp32");
        for (size_t i = 0; i < n; ++i) {
            uint32_t r = host_ref(progs[pi], xs[i]);
            int nanboth = isnan(bf(r)) && isnan(bf(ys[i]));
            if (r != ys[i] && !nanboth) { eq = 0; printf("  mismatch prog %d x=%08x host=%08x lib=%08x\n", pi, xs[i], r, ys[i]); break; }
        }
    }
    CHECK(eq, "fp32 eval == host float evaluator (14 patterns + 4082 random, 3 programs)");

    /* conversions equal the library reference definitions */
    CHECK(omega_program_fp32_eval(&u2f, xs, n, ys) == 0, "eval u2f");
    eq = 1;
    for (size_t i = 0; i < n; ++i) if (ys[i] != fbits(omega_ref_u2f(xs[i]))) { eq = 0; break; }
    CHECK(eq, "u32->fp32 == omega_ref_u2f");
    CHECK(omega_program_fp32_eval(&f2u, xs, n, ys) == 0, "eval f2u");
    eq = 1;
    for (size_t i = 0; i < n; ++i) if (ys[i] != omega_ref_f2u(bf(xs[i]))) { eq = 0; break; }
    CHECK(eq, "fp32->u32 == omega_ref_f2u");

    /* a u32 -> convert -> add -> u32 chain, explicit conversions only */
    OmegaProgramStep chain[] = { { OP_CONVERT, TYPE_FP32 }, { OP_ADD, fbits(1.5f) }, { OP_CONVERT, TYPE_UNSIGNED_INT } };
    OmegaProgram ch;
    CHECK(omega_program_build_fp32(&ch, "chain", TYPE_UNSIGNED_INT, TYPE_UNSIGNED_INT, chain, 3) == 0, "build explicit chain");
    uint32_t in3[] = { 0, 1, 41, 0xfffffff0u }, out3[4];
    CHECK(omega_program_fp32_eval(&ch, in3, 4, out3) == 0 && out3[0] == 1 && out3[1] == 2 && out3[2] == 42, "chain evaluates 0->1, 1->2, 41->42");

    /* 6. the AArch64 realizer refuses FP32 programs with a reason */
    char rwhy[200] = "";
    CHECK(omega_program_realize_ex(&a, rwhy, sizeof rwhy) != 0 && rwhy[0] != '\0', "AArch64 realizer refuses FP32 program");

    printf("GATE OMEGA_PROGRAM_FP32_PASS %s %d/%d\n", fails ? "FAIL" : "PASS", total - fails, total);
    return fails ? 1 : 0;
}
