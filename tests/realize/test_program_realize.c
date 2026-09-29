/* test_program_realize.c -- the realization compiles the actual Omega program
 * (spec/program-realization.md).
 *
 * Gates (each prints "GATE <name> PASS|FAIL <passed>/<total>"):
 *   OMEGA_PROGRAM_REALIZE_PASS                  omega_program_realize exists, compiles the canonical
 *                                               body, binds the program id, and fails closed (with a
 *                                               reason) on every unsupported semantic
 *   OMEGA_REALIZATION_PROGRAM_DRIVEN_PASS       the machine-aware synthesizer's code is derived from
 *                                               the program: different programs -> different code
 *                                               and ids; sequential code lifts back to the body;
 *                                               the old fixed-3x-2 synthesizer fails this gate
 *   OMEGA_REALIZATION_DIFFERENTIAL_PASS         native execution of the realized bytes == the
 *                                               independent semantic evaluator, boundary + seeded
 *                                               random inputs, named + seeded random pipelines,
 *                                               u8/u16/u32/u64, both canonical machines
 *   OMEGA_REALIZATION_TRIPLE_BIND_PASS          SEMANTIC_ID + MACHINE_ID + code -> REALIZATION_ID;
 *                                               rebinding, code and machine tampering all fail
 *
 * Physics-free. Executes generated code natively: AArch64 hosts only (on any other host every
 * execution check fails; it never passes silently).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "omega_core.h"
#include "omega_canonical.h"
#include "omega_program.h"
#include "omega_machine.h"
#include "omega_realize.h"
#include "omega_realize_synth.h"
#include "omega_exec.h"
#include "aarch64_encoder.h"
#include "aarch64_decoder.h"

enum { G_REAL, G_DRIVEN, G_DIFF, G_TRIPLE, G_COUNT };
static const char *const GATE_NAME[G_COUNT] = {
    "OMEGA_PROGRAM_REALIZE_PASS", "OMEGA_REALIZATION_PROGRAM_DRIVEN_PASS",
    "OMEGA_REALIZATION_DIFFERENTIAL_PASS", "OMEGA_REALIZATION_TRIPLE_BIND_PASS"
};
static long g_pass[G_COUNT], g_total[G_COUNT];
static long g_fail_printed;

#define CHECK(gate, cond, ...)                                                          \
    do {                                                                                \
        g_total[gate]++;                                                                \
        if (cond) g_pass[gate]++;                                                       \
        else if (g_fail_printed++ < 40) {                                               \
            fprintf(stderr, "FAIL [%s] %s:%d ", GATE_NAME[gate], __FILE__, __LINE__);   \
            fprintf(stderr, __VA_ARGS__);                                               \
            fprintf(stderr, "\n");                                                      \
        }                                                                               \
    } while (0)

static int ids_equal(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, OMEGA_ID_BYTES) == 0;
}

static uint64_t g_rng = 0x0123456789ABCDEFull;   /* fixed seed: reproducible */
static uint64_t rnd(void) {
    uint64_t x = g_rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    g_rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint64_t wmask(uint16_t w) { return w >= 64 ? ~0ull : ((1ull << w) - 1ull); }

/* A program built directly from a body (any width, any 64-bit constant): the canonical
 * structures only, no builder template. */
static int make_prog(OmegaProgram *p, const char *name, uint16_t w, const uint8_t *ops, const uint64_t *imms,
                     size_t n) {
    omega_program_init(p, name);
    p->contract.input_type = p->contract.output_type = TYPE_UNSIGNED_INT;
    p->contract.input_width = p->contract.output_width = w;
    snprintf(p->contract.precondition, sizeof p->contract.precondition, "true");
    snprintf(p->contract.postcondition, sizeof p->contract.postcondition, "true");
    omega_build_constraint_id(CONST_PRECONDITION, "true", &p->contract.precondition_id);
    omega_build_constraint_id(CONST_POSTCONDITION, "true", &p->contract.postcondition_id);
    p->body.has_body = true;
    p->body.step_count = (uint16_t)n;
    for (size_t i = 0; i < n; ++i) { p->body.steps[i].op = ops[i]; p->body.steps[i].imm = imms[i]; }
    return omega_program_compute_id(p);
}

static int compose2(OmegaProgram *out, OpCode o1, uint64_t i1, OpCode o2, uint64_t i2) {
    OmegaProgram a, b;
    char err[160];
    if (omega_program_build_unary_op(&a, "a", o1, i1) != 0 || omega_program_build_unary_op(&b, "b", o2, i2) != 0)
        return -1;
    return omega_program_compose(&a, &b, out, err, sizeof err);
}

/* ---- the named programs (built through the real builders) ------------------ */

#define NAMED_MAX 24
static OmegaProgram g_named[NAMED_MAX];
static const char *g_named_label[NAMED_MAX];
static size_t g_named_n;

static void add_named(const char *label, int rc, const OmegaProgram *p) {
    CHECK(G_REAL, rc == 0, "build %s", label);
    if (rc == 0 && g_named_n < NAMED_MAX) { g_named[g_named_n] = *p; g_named_label[g_named_n++] = label; }
}

static void build_named(void) {
    OmegaProgram p;
    add_named("x+1", omega_program_build_unary_op(&p, "f", OP_ADD, 1), &p);
    add_named("x+11", omega_program_build_unary_op(&p, "f", OP_ADD, 11), &p);
    add_named("x-2", omega_program_build_unary_op(&p, "f", OP_SUB, 2), &p);
    add_named("x*3", omega_program_build_unary_op(&p, "f", OP_MUL, 3), &p);
    add_named("(x*3)-2", compose2(&p, OP_MUL, 3, OP_SUB, 2), &p);
    add_named("(x+7)*5", compose2(&p, OP_ADD, 7, OP_MUL, 5), &p);
    add_named("x&0xFF00FF", omega_program_build_unary_op(&p, "f", OP_AND, 0xFF00FF), &p);
    add_named("x|0x80000001", omega_program_build_unary_op(&p, "f", OP_OR, 0x80000001u), &p);
    add_named("x&0xFFFF", omega_program_build_unary_op(&p, "f", OP_AND, 0xFFFF), &p);
    add_named("x|0x10", omega_program_build_unary_op(&p, "f", OP_OR, 0x10), &p);
    {   /* wide constants: movz + movk up to lsl 48 */
        uint8_t o[] = { OP_ADD };             uint64_t i[] = { 0x123456789ABCDEF0ull };
        add_named("x+0x123456789ABCDEF0", make_prog(&p, "w", 64, o, i, 1), &p);
        uint8_t o2[] = { OP_MUL, OP_AND, OP_OR }; uint64_t i2[] = { 0x100000001ull, 0xFFFF0000FFFF0000ull, 0x0001000000000000ull };
        add_named("((x*0x100000001)&0xFFFF0000FFFF0000)|1<<48", make_prog(&p, "w", 64, o2, i2, 3), &p);
    }
    {   /* declared widths below 64 (wrap mod 2^w) */
        uint8_t o[] = { OP_ADD };              uint64_t i[] = { 1 };
        add_named("u8 x+1", make_prog(&p, "n", 8, o, i, 1), &p);
        uint8_t o2[] = { OP_SUB };             uint64_t i2[] = { 2 };
        add_named("u16 x-2", make_prog(&p, "n", 16, o2, i2, 1), &p);
        uint8_t o3[] = { OP_MUL, OP_SUB };     uint64_t i3[] = { 3, 2 };
        add_named("u32 (x*3)-2", make_prog(&p, "n", 32, o3, i3, 2), &p);
        uint8_t o4[] = { OP_ADD, OP_MUL };     uint64_t i4[] = { 0xFFFFFFFFull, 0xFFFFFFFFull };
        add_named("u32 (x+max)*max", make_prog(&p, "n", 32, o4, i4, 2), &p);
    }
    {   /* identity (empty chain) and the 64-step boundary */
        add_named("u64 x (no steps)", make_prog(&p, "id", 64, NULL, NULL, 0), &p);
        uint8_t o[64]; uint64_t i[64];
        for (int k = 0; k < 64; ++k) { o[k] = (uint8_t)(k % 2 ? OP_MUL : OP_ADD); i[k] = 0xFEDCBA9876543210ull ^ (uint64_t)k; }
        add_named("64-step wide pipeline", make_prog(&p, "long", 64, o, i, 64), &p);
    }
}

/* ---- evaluator sanity (hand-derived wraparound facts, not the oracle) ------ */

static void test_evaluator_sanity(void) {
    OmegaProgram p;
    uint64_t xs[4], ys[4];
    omega_program_build_unary_op(&p, "f", OP_ADD, 1);
    xs[0] = 0; xs[1] = ~0ull;
    CHECK(G_DIFF, omega_program_eval(&p, xs, 2, ys) == 0 && ys[0] == 1 && ys[1] == 0, "eval x+1 at 0 and wrap at max");
    omega_program_build_unary_op(&p, "f", OP_SUB, 2);
    xs[0] = 0; xs[1] = 1;
    CHECK(G_DIFF, omega_program_eval(&p, xs, 2, ys) == 0 && ys[0] == ~0ull - 1 && ys[1] == ~0ull, "eval x-2 wraps below 0");
    uint8_t o[] = { OP_ADD }; uint64_t i[] = { 1 };
    make_prog(&p, "n", 8, o, i, 1);
    xs[0] = 255; xs[1] = 256;
    CHECK(G_DIFF, omega_program_eval(&p, xs, 1, ys) == 0 && ys[0] == 0, "eval u8 255+1 == 0");
    CHECK(G_DIFF, omega_program_eval(&p, xs + 1, 1, ys) != 0, "eval refuses an input outside u8");
}

/* ---- OMEGA_PROGRAM_REALIZE_PASS ------------------------------------------ */

/* The retired fusion rule of omega_program_compose, kept here only to prove the compiled
 * composite is byte-identical to it for template parts. */
static int legacy_splice(const OmegaProgram *a, const OmegaProgram *b, uint8_t *out, size_t *len) {
    size_t al = a->realization.code_len - 4;
    memcpy(out, a->realization.code_bytes, al);
    memcpy(out + al, b->realization.code_bytes, b->realization.code_len);
    *len = al + b->realization.code_len;
    return 0;
}

static void expect_refused(OmegaProgram *p, const char *what, const char *needle) {
    char why[192] = "";
    OmegaProgram c = *p;
    int rc = omega_program_realize_ex(&c, why, sizeof why);
    CHECK(G_REAL, rc != 0 && !c.is_realized && why[0] && (!needle || strstr(why, needle)),
          "%s must fail closed with a reason (rc=%d why=%s)", what, rc, why);
    RealizationSynthesisResult res;
    rc = omega_synthesize_for_dgx_spark(p, &res);
    CHECK(G_REAL, rc == -2 && !res.solved && res.realization.code_len == 0 && res.why[0],
          "%s: synthesizer fails closed too (rc=%d)", what, rc);
    printf("FAIL_CLOSED %-34s %s\n", what, why);
}

static void test_realize(void) {
    for (size_t k = 0; k < g_named_n; ++k) {
        OmegaProgram p = g_named[k];
        p.is_realized = false;
        memset(&p.realization, 0, sizeof p.realization);
        char why[192] = "";
        CHECK(G_REAL, omega_program_realize_ex(&p, why, sizeof why) == 0 && p.is_realized && !p.is_verified,
              "realize %s (%s)", g_named_label[k], why);
        CHECK(G_REAL, ids_equal(&p.realization.semantic_id, &p.program_id), "%s realization binds program id",
              g_named_label[k]);
        RealizationObject r = p.realization;
        omega_compute_realization_id(&r);
        CHECK(G_REAL, ids_equal(&r.realization_id, &p.realization.realization_id) && !p.realization.has_machine_id,
              "%s OMG_R0 id recomputes", g_named_label[k]);
        VerifyReport rep;
        CHECK(G_REAL, omega_verify_v0_structural(NULL, &p.realization, &rep) == 0, "%s V0", g_named_label[k]);
        uint8_t code[AARCH64_MAX_CODE_BYTES];
        size_t len = 0;
        CHECK(G_REAL, omega_program_emit_schedule(&p.body, p.contract.input_width, OMEGA_SCHED_SEQUENTIAL, code, &len,
                                                  sizeof code) == 0 &&
                      len == p.realization.code_len && memcmp(code, p.realization.code_bytes, len) == 0,
              "%s realize == SEQUENTIAL schedule of its body", g_named_label[k]);
        /* the builder path now goes through omega_program_realize: same bytes */
        if (g_named[k].is_realized)
            CHECK(G_REAL, g_named[k].realization.code_len == p.realization.code_len &&
                          memcmp(g_named[k].realization.code_bytes, p.realization.code_bytes, len) == 0 &&
                          ids_equal(&g_named[k].realization.realization_id, &p.realization.realization_id),
                  "%s builder realization == omega_program_realize", g_named_label[k]);
        /* realize output executes as the program means */
        uint64_t xs[OMEGA_REALIZE_DIFF_INPUTS], ys[OMEGA_REALIZE_DIFF_INPUTS];
        size_t n = omega_realize_differential_inputs(&p, xs, OMEGA_REALIZE_DIFF_INPUTS);
        int erc = omega_program_eval(&p, xs, n, ys);
        CHECK(G_REAL, n == OMEGA_REALIZE_DIFF_INPUTS && erc == 0, "%s eval", g_named_label[k]);
        for (size_t i = 0; erc == 0 && i < n; ++i) {
            uint64_t got = 0;
            int rc = omega_program_exec(&p, xs[i], &got);
            CHECK(G_REAL, rc == 0 && got == ys[i], "%s realize exec x=0x%llx got 0x%llx want 0x%llx (rc=%d)",
                  g_named_label[k], (unsigned long long)xs[i], (unsigned long long)got, (unsigned long long)ys[i], rc);
        }
    }

    /* compose now compiles the composite body: byte-identical to the retired splice */
    OmegaProgram a, b, c;
    char err[160];
    for (int t = 0; t < 50; ++t) {
        static const OpCode ops[] = { OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR };
        omega_program_build_unary_op(&a, "a", ops[rnd() % 5], rnd() & 0xFFFFFFFFull);
        omega_program_build_unary_op(&b, "b", ops[rnd() % 5], rnd() & (t % 2 ? 0xFFFF : 0xFFFFFFFFull));
        uint8_t sp[AARCH64_MAX_CODE_BYTES];
        size_t sl = 0;
        legacy_splice(&a, &b, sp, &sl);
        CHECK(G_REAL, omega_program_compose(&a, &b, &c, err, sizeof err) == 0 && c.is_realized &&
                      c.realization.code_len == sl && memcmp(c.realization.code_bytes, sp, sl) == 0,
              "compose realization == legacy splice bytes (t=%d)", t);
    }

    /* fail closed, each with a reason */
    OmegaProgram p;
    omega_program_init(&p, "empty");
    expect_refused(&p, "no body", "no semantic body");
    uint8_t o1[] = { OP_DIV }; uint64_t i1[] = { 3 };
    make_prog(&p, "div", 64, o1, i1, 1);   /* no id: DIV is outside the body op set */
    p.program_id.bytes[0] = 1;
    expect_refused(&p, "DIV step", "DIV");
    uint8_t o2[] = { OP_NOT }; uint64_t i2[] = { 0 };
    make_prog(&p, "not", 64, o2, i2, 1);
    expect_refused(&p, "NOT step", "not in V0 body op set");
    uint8_t o3[] = { OP_ADD }; uint64_t i3[] = { 1 };
    make_prog(&p, "s", 64, o3, i3, 1);
    p.contract.input_type = p.contract.output_type = TYPE_SIGNED_INT;
    omega_program_compute_id(&p);
    expect_refused(&p, "signed i64", "unsigned integers only");
    make_prog(&p, "bv", 64, o3, i3, 1);
    p.contract.input_type = p.contract.output_type = TYPE_BITVECTOR;
    omega_program_compute_id(&p);
    expect_refused(&p, "bitvector", "unsigned integers only");
    make_prog(&p, "w12", 12, o3, i3, 1);
    expect_refused(&p, "width u12", "width 12");
    make_prog(&p, "wmix", 64, o3, i3, 1);
    p.contract.output_width = 32;
    omega_program_compute_id(&p);
    expect_refused(&p, "u64 -> u32", "never converts");
    uint64_t i4[] = { 256 };
    make_prog(&p, "u8wide", 8, o3, i4, 1);
    expect_refused(&p, "u8 constant 256", "does not fit");
    make_prog(&p, "stale", 64, o3, i3, 1);
    p.body.steps[0].imm = 2;   /* body changed after the id was computed */
    expect_refused(&p, "stale program id", "stale");
    make_prog(&p, "over", 64, o3, i3, 1);
    p.body.step_count = OMEGA_PROGRAM_MAX_STEPS + 1;
    expect_refused(&p, "65 steps", "64 steps");
    make_prog(&p, "noid", 64, o3, i3, 1);
    memset(&p.program_id, 0, sizeof p.program_id);
    expect_refused(&p, "zero program id", "stale");
}

/* ---- DIFFERENTIAL + PROGRAM_DRIVEN over named and random pipelines --------- */

static OmegaMachineGraph g_spark, g_qemu;

#define POOL_MAX 600
static RealizationSynthesisResult g_res_s[POOL_MAX], g_res_q[POOL_MAX];
static OmegaProgram g_pool[POOL_MAX];
static size_t g_pool_n;

static void diff_one(const OmegaProgram *p, const char *label, RealizationSynthesisResult *rs,
                     RealizationSynthesisResult *rq) {
    int a = omega_synthesize_for_dgx_spark(p, rs);
    int b = omega_synthesize_for_qemu_virt(p, rq);
    CHECK(G_DIFF, a == 0 && rs->solved && rs->inputs_checked == OMEGA_REALIZE_DIFF_INPUTS,
          "%s spark synth rc=%d %s", label, a, rs->why);
    CHECK(G_DIFF, b == 0 && rq->solved && rq->inputs_checked == OMEGA_REALIZE_DIFF_INPUTS,
          "%s qemu synth rc=%d %s", label, b, rq->why);
    CHECK(G_DRIVEN, rs->schedule == OMEGA_SCHED_PRELOAD && rq->schedule == OMEGA_SCHED_SEQUENTIAL,
          "%s schedule chosen from machine", label);
    /* own loop: boundary + extra random inputs in the declared domain */
    uint16_t w = p->contract.input_width;
    uint64_t xs[OMEGA_REALIZE_DIFF_INPUTS + 32], ys[OMEGA_REALIZE_DIFF_INPUTS + 32];
    size_t n = omega_realize_differential_inputs(p, xs, OMEGA_REALIZE_DIFF_INPUTS);
    for (int i = 0; i < 32; ++i) xs[n++] = rnd() & wmask(w);
    if (omega_program_eval(p, xs, n, ys) != 0) { CHECK(G_DIFF, 0, "%s eval refused", label); return; }
    for (size_t i = 0; i < n; ++i) {
        uint64_t gs = 0, gq = 0;
        int e1 = omega_exec_native_f3(&rs->realization, xs[i], 0, 0, &gs);
        int e2 = omega_exec_native_f3(&rq->realization, xs[i], 0, 0, &gq);
        CHECK(G_DIFF, e1 == 0 && gs == ys[i], "%s spark x=0x%llx native=0x%llx semantic=0x%llx", label,
              (unsigned long long)xs[i], (unsigned long long)gs, (unsigned long long)ys[i]);
        CHECK(G_DIFF, e2 == 0 && gq == ys[i], "%s qemu x=0x%llx native=0x%llx semantic=0x%llx", label,
              (unsigned long long)xs[i], (unsigned long long)gq, (unsigned long long)ys[i]);
    }
    /* sequential code of a u64 program lifts back to exactly its body */
    OmegaProgramBody lifted;
    if (w == 64)
        CHECK(G_DRIVEN, omega_program_lift_body(rq->realization.code_bytes, rq->realization.code_len, &lifted) == 0 &&
                        lifted.step_count == p->body.step_count &&
                        memcmp(lifted.steps, p->body.steps, p->body.step_count * sizeof(OmegaProgramStep)) == 0,
              "%s sequential code lifts back to the program body", label);
}

static void random_body(OmegaProgram *p, int t) {
    static const uint16_t widths[] = { 8, 16, 32, 64 };
    static const uint8_t ops[] = { OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR };
    uint16_t w = widths[rnd() % 4];
    size_t n = 1 + (size_t)(rnd() % (t % 10 == 0 ? 64 : 8));
    uint8_t o[64];
    uint64_t im[64];
    for (size_t i = 0; i < n; ++i) {
        o[i] = ops[rnd() % 5];
        uint64_t v = rnd();
        switch (rnd() % 4) {          /* constant size classes: 16, 32, 48, 64 bits */
            case 0: v &= 0xFFFF; break;
            case 1: v &= 0xFFFFFFFFull; break;
            case 2: v &= 0xFFFFFFFFFFFFull; break;
            default: break;
        }
        im[i] = v & wmask(w);
    }
    make_prog(p, "rand", w, o, im, n);
}

static void test_differential_and_driven(void) {
    omega_machine_build_dgx_spark(&g_spark);
    omega_machine_build_qemu_virt(&g_qemu);
    for (size_t k = 0; k < g_named_n && g_pool_n < POOL_MAX; ++k, ++g_pool_n) {
        g_pool[g_pool_n] = g_named[k];
        diff_one(&g_pool[g_pool_n], g_named_label[k], &g_res_s[g_pool_n], &g_res_q[g_pool_n]);
    }
    char label[48];
    for (int t = 0; g_pool_n < POOL_MAX; ++t, ++g_pool_n) {
        random_body(&g_pool[g_pool_n], t);
        snprintf(label, sizeof label, "random#%d", t);
        diff_one(&g_pool[g_pool_n], label, &g_res_s[g_pool_n], &g_res_q[g_pool_n]);
    }

    /* PROGRAM_DRIVEN: distinct programs -> distinct code and distinct RealizationIds (per
     * machine and across machines); identical program -> identical realization. */
    long dup_prog = 0;
    for (size_t i = 0; i < g_pool_n; ++i)
        for (size_t j = i + 1; j < g_pool_n; ++j) {
            if (ids_equal(&g_pool[i].program_id, &g_pool[j].program_id)) { dup_prog++; continue; }
            CHECK(G_DRIVEN, !ids_equal(&g_res_s[i].realization_id, &g_res_s[j].realization_id) &&
                            !ids_equal(&g_res_q[i].realization_id, &g_res_q[j].realization_id) &&
                            !ids_equal(&g_res_s[i].realization_id, &g_res_q[j].realization_id),
                  "programs %zu/%zu share a RealizationId", i, j);
            bool same_body = g_pool[i].contract.input_width == g_pool[j].contract.input_width &&
                             g_pool[i].body.step_count == g_pool[j].body.step_count &&
                             memcmp(g_pool[i].body.steps, g_pool[j].body.steps,
                                    g_pool[i].body.step_count * sizeof(OmegaProgramStep)) == 0;
            bool same_code = g_res_q[i].realization.code_len == g_res_q[j].realization.code_len &&
                             memcmp(g_res_q[i].realization.code_bytes, g_res_q[j].realization.code_bytes,
                                    g_res_q[i].realization.code_len) == 0;
            CHECK(G_DRIVEN, same_body || !same_code, "programs %zu/%zu with different bodies got identical code", i, j);
        }
    printf("INFO pool=%zu programs, %ld duplicate-program pairs skipped\n", g_pool_n, dup_prog);
    for (size_t i = 0; i < 20; ++i) {
        RealizationSynthesisResult again;
        omega_synthesize_for_dgx_spark(&g_pool[i], &again);
        CHECK(G_DRIVEN, ids_equal(&again.realization_id, &g_res_s[i].realization_id) &&
                        again.realization.code_len == g_res_s[i].realization.code_len,
              "deterministic realization %zu", i);
        CHECK(G_DRIVEN, !ids_equal(&g_res_s[i].realization_id, &g_res_q[i].realization_id),
              "same program, different machine -> different RealizationId %zu", i);
    }

    /* The M14 3x-2 program keeps its historical bytes on both machines (goldens unchanged). */
    static const uint8_t spark_3x2[20] = { 0x61,0x00,0x80,0xd2, 0x42,0x00,0x80,0xd2, 0x00,0x7c,0x01,0x9b,
                                          0x00,0x00,0x02,0xcb, 0xc0,0x03,0x5f,0xd6 };
    static const uint8_t qemu_3x2[20] = { 0x61,0x00,0x80,0xd2, 0x00,0x7c,0x01,0x9b, 0x41,0x00,0x80,0xd2,
                                         0x00,0x00,0x01,0xcb, 0xc0,0x03,0x5f,0xd6 };
    CHECK(G_DRIVEN, g_res_s[4].realization.code_len == 20 && memcmp(g_res_s[4].realization.code_bytes, spark_3x2, 20) == 0,
          "(x*3)-2 spark bytes == historical preload schedule");
    CHECK(G_DRIVEN, g_res_q[4].realization.code_len == 20 && memcmp(g_res_q[4].realization.code_bytes, qemu_3x2, 20) == 0,
          "(x*3)-2 qemu bytes == historical sequential schedule");
}

/* Sanity demo: the pre-fix synthesizer (fixed f(x)=3x-2 whatever the program), reproduced
 * here only. PROGRAM_DRIVEN must reject it; it must not pass verification for x+1. */
static void legacy_fixed_synth(const OmegaProgram *p, const OmegaMachineGraph *mg, RealizationObject *r) {
    memset(r, 0, sizeof *r);
    size_t pos = 0;
    aarch64_emit_movz(r->code_bytes, &pos, sizeof r->code_bytes, true, REG_X1, 3, 0);
    aarch64_emit_movz(r->code_bytes, &pos, sizeof r->code_bytes, true, REG_X2, 2, 0);
    aarch64_emit_mul_reg(r->code_bytes, &pos, sizeof r->code_bytes, true, REG_X0, REG_X0, REG_X1);
    aarch64_emit_sub_reg(r->code_bytes, &pos, sizeof r->code_bytes, true, REG_X0, REG_X0, REG_X2);
    aarch64_emit_ret(r->code_bytes, &pos, sizeof r->code_bytes);
    r->code_len = pos;
    r->semantic_id = p->program_id;
    r->machine_id = mg->machine_id;
    r->has_machine_id = true;
    r->target_profile = mg->target_profile;
    omega_realize_compute_triple_id(&p->program_id, &mg->machine_id, r, &r->realization_id);
    r->has_id = true;
}

static void test_legacy_sanity(void) {
    size_t rejected = 0, same_code_pairs = 0, pairs = 0;
    RealizationObject r0, r;
    legacy_fixed_synth(&g_named[0], &g_spark, &r0);
    for (size_t k = 0; k < g_named_n; ++k) {
        legacy_fixed_synth(&g_named[k], &g_spark, &r);
        char why[192];
        if (omega_realization_verify_program(&g_named[k], &g_spark, &r, NULL, why, sizeof why) != 0) rejected++;
        if (k) { pairs++; if (r.code_len == r0.code_len && memcmp(r.code_bytes, r0.code_bytes, r.code_len) == 0) same_code_pairs++; }
    }
    /* Only (x*3)-2 itself is legitimately 3x-2; every other program must be rejected. */
    CHECK(G_DRIVEN, rejected == g_named_n - 1, "legacy fixed synth rejected for %zu of %zu programs", rejected, g_named_n);
    CHECK(G_DRIVEN, same_code_pairs == pairs, "legacy emits identical code for every program (not program-driven)");
    printf("SANITY legacy_fixed_3x2_synth PROGRAM_DRIVEN FAIL (expected): identical code for %zu/%zu other programs, "
           "verification rejects %zu/%zu\n", same_code_pairs, pairs, rejected, g_named_n);
}

/* ---- TRIPLE_BIND + hostile ------------------------------------------------- */

static void test_triple(void) {
    char why[192];
    for (size_t a = 0; a < g_named_n; ++a) {
        const OmegaProgram *A = &g_named[a];
        const RealizationObject *RA = &g_res_s[a].realization;
        SemanticId t;
        omega_realize_compute_triple_id(&A->program_id, &g_spark.machine_id, RA, &t);
        CHECK(G_TRIPLE, ids_equal(&t, &RA->realization_id) && ids_equal(&RA->semantic_id, &A->program_id) &&
                        ids_equal(&RA->machine_id, &g_spark.machine_id), "%s triple recomputes", g_named_label[a]);
        CHECK(G_TRIPLE, omega_realization_verify_program(A, &g_spark, RA, NULL, why, sizeof why) == 0,
              "%s honest realization verifies (%s)", g_named_label[a], why);
        /* each component changes the id */
        RealizationObject m = *RA;
        m.code_bytes[0] ^= 0x20;
        SemanticId t2, t3, t4;
        omega_realize_compute_triple_id(&A->program_id, &g_spark.machine_id, &m, &t2);
        omega_realize_compute_triple_id(&A->program_id, &g_qemu.machine_id, RA, &t3);
        omega_realize_compute_triple_id(&g_named[(a + 1) % g_named_n].program_id, &g_spark.machine_id, RA, &t4);
        CHECK(G_TRIPLE, !ids_equal(&t, &t2) && !ids_equal(&t, &t3) && !ids_equal(&t, &t4),
              "%s code / machine / semantic each change the id", g_named_label[a]);

        /* altered code bytes, id left stale -> fails */
        CHECK(G_TRIPLE, omega_realization_verify_program(A, &g_spark, &m, NULL, why, sizeof why) != 0,
              "%s altered code (stale id) accepted", g_named_label[a]);
        /* altered code, triple recomputed -> differential fails. Flip the first constant's
         * low bit (MOVZ imm16 bit 0 = instruction bit 5). */
        m = *RA;
        m.code_bytes[0] ^= 0x20;
        omega_realize_compute_triple_id(&m.semantic_id, &m.machine_id, &m, &m.realization_id);
        int rc = omega_realization_verify_program(A, &g_spark, &m, NULL, why, sizeof why);
        if (A->body.step_count > 0)
            CHECK(G_TRIPLE, rc != 0 && strstr(why, "MISMATCH"), "%s altered code (re-hashed) accepted: %s",
                  g_named_label[a], why);
        /* altered machine id */
        m = *RA;
        m.machine_id = g_qemu.machine_id;
        CHECK(G_TRIPLE, omega_realization_verify_program(A, &g_spark, &m, NULL, why, sizeof why) != 0,
              "%s machine id swapped (stale triple) accepted", g_named_label[a]);
        omega_realize_compute_triple_id(&m.semantic_id, &m.machine_id, &m, &m.realization_id);
        CHECK(G_TRIPLE, omega_realization_verify_program(A, &g_spark, &m, NULL, why, sizeof why) != 0,
              "%s realization re-bound to qemu accepted for spark", g_named_label[a]);
        /* forged machine graph: content changed, id kept */
        OmegaMachineGraph forged = g_spark;
        forged.pipeline.issue_width = 8;
        CHECK(G_TRIPLE, omega_realization_verify_program(A, &forged, RA, NULL, why, sizeof why) != 0,
              "%s forged machine graph accepted", g_named_label[a]);

        for (size_t b = 0; b < g_named_n; ++b) {
            if (b == a) continue;
            const OmegaProgram *B = &g_named[b];
            /* A's realization verified as B: semantic id mismatch */
            CHECK(G_TRIPLE, omega_realization_verify_program(B, &g_spark, RA, NULL, why, sizeof why) != 0,
                  "A=%s realization accepted for B=%s", g_named_label[a], g_named_label[b]);
            /* the discriminating rebinding: A's code stamped with B's id + consistent triple */
            RealizationObject rb = *RA;
            rb.semantic_id = B->program_id;
            omega_realize_compute_triple_id(&rb.semantic_id, &rb.machine_id, &rb, &rb.realization_id);
            rc = omega_realization_verify_program(B, &g_spark, &rb, NULL, why, sizeof why);
            CHECK(G_TRIPLE, rc != 0, "A=%s code rebound to B=%s (consistent triple) accepted", g_named_label[a],
                  g_named_label[b]);
            /* and it is rejected by the semantic differential or the width gate, not luck */
            CHECK(G_TRIPLE, rc != 0 && (strstr(why, "MISMATCH") || strstr(why, "V0")),
                  "rebinding %s->%s rejected for the wrong reason: %s", g_named_label[a], g_named_label[b], why);
        }
    }
}

int main(void) {
#if !defined(__aarch64__)
    fprintf(stderr, "NOTE: not an AArch64 host: native execution checks will FAIL (never skipped)\n");
#endif
    build_named();
    test_evaluator_sanity();
    test_realize();
    test_differential_and_driven();
    test_legacy_sanity();
    test_triple();
    int ok = 1;
    for (int i = 0; i < G_COUNT; i++) {
        int pass = g_pass[i] == g_total[i] && g_total[i] > 0;
        if (!pass) ok = 0;
        printf("GATE %s %s %ld/%ld\n", GATE_NAME[i], pass ? "PASS" : "FAIL", g_pass[i], g_total[i]);
    }
    long p = 0, t = 0;
    for (int i = 0; i < G_COUNT; i++) { p += g_pass[i]; t += g_total[i]; }
    printf("%s %ld/%ld\n", ok ? "PASS" : "FAIL", p, t);
    return ok ? 0 : 1;
}
