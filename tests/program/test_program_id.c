/* test_program_id.c -- program identity v2 (spec/program-identity.md).
 *
 * Gates (each prints "GATE <name> PASS|FAIL <passed>/<total>"):
 *   OMEGA_PROGRAM_ID_BODY_BOUND_PASS               the id changes when the body changes
 *   OMEGA_PROGRAM_ID_REPRESENTATION_INVARIANT_PASS the id ignores name, whitespace, parameter name,
 *                                                  operand side, node allocation order, cost, code
 *   OMEGA_PROGRAM_ID_MUTATION_SEPARATION_PASS      seeded differential: id equal iff body+contract
 *                                                  equal (hash-free oracle); single mutations separate
 *   OMEGA_PROGRAM_ID_LIBRARY_REGRESSION_PASS       library / discovery / refactor / synthesis /
 *                                                  realization-triple consumers
 *   OMEGA_PROGRAM_ID_VISOR_REGRESSION_PASS         Visor language lowering + session behaviour
 *
 * Physics-free, host-portable (no native execution: CI runs this on x86-64).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "omega_core.h"
#include "omega_canonical.h"
#include "omega_program.h"
#include "omega_library.h"
#include "omega_discovery.h"
#include "omega_synthesis.h"
#include "omega_machine.h"
#include "omega_realize.h"
#include "omega_realize_synth.h"
#include "sha256.h"
#include "visor.h"
#include "omega_parse.h"
#include "omega_lower.h"

enum { G_BODY, G_REPR, G_MUT, G_LIB, G_VISOR, G_COUNT };
static const char *const GATE_NAME[G_COUNT] = {
    "OMEGA_PROGRAM_ID_BODY_BOUND_PASS", "OMEGA_PROGRAM_ID_REPRESENTATION_INVARIANT_PASS",
    "OMEGA_PROGRAM_ID_MUTATION_SEPARATION_PASS", "OMEGA_PROGRAM_ID_LIBRARY_REGRESSION_PASS",
    "OMEGA_PROGRAM_ID_VISOR_REGRESSION_PASS"
};
static long g_pass[G_COUNT], g_total[G_COUNT];

#define CHECK(gate, cond, ...)                                                        \
    do {                                                                              \
        g_total[gate]++;                                                              \
        if (cond) g_pass[gate]++;                                                     \
        else {                                                                        \
            fprintf(stderr, "FAIL [%s] %s:%d ", GATE_NAME[gate], __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                             \
            fprintf(stderr, "\n");                                                    \
        }                                                                             \
    } while (0)

/* Bulk checks (differential) count every comparison but only print the first few failures. */
static long g_bulk_fail_printed;
#define BULK(gate, cond, ...)                                                          \
    do {                                                                               \
        g_total[gate]++;                                                               \
        if (cond) g_pass[gate]++;                                                      \
        else if (g_bulk_fail_printed++ < 10) {                                         \
            fprintf(stderr, "FAIL [%s] %s:%d ", GATE_NAME[gate], __FILE__, __LINE__);  \
            fprintf(stderr, __VA_ARGS__);                                              \
            fprintf(stderr, "\n");                                                     \
        }                                                                              \
    } while (0)

static int ids_equal(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, OMEGA_ID_BYTES) == 0;
}
static int id_zero(const SemanticId *a) {
    static const uint8_t z[OMEGA_ID_BYTES];
    return memcmp(a->bytes, z, OMEGA_ID_BYTES) == 0;
}

/* Replace the contract with explicit leaf clauses and recompute. */
static void set_contract(OmegaProgram *p, const char *pre, const char *post) {
    p->contract.post_leaf_count = 0;
    snprintf(p->contract.precondition, sizeof p->contract.precondition, "%s", pre);
    snprintf(p->contract.postcondition, sizeof p->contract.postcondition, "%s", post);
    omega_build_constraint_id(CONST_PRECONDITION, pre, &p->contract.precondition_id);
    omega_build_constraint_id(CONST_POSTCONDITION, post, &p->contract.postcondition_id);
    omega_program_compute_id(p);
}

/* Build a chain program from steps by composing unary programs (the library path). */
static int build_chain(OmegaProgram *out, const char *name, const uint8_t *ops, const uint64_t *imms, size_t n) {
    OmegaProgram acc, step, tmp;
    char err[160];
    if (n == 0 || omega_program_build_unary_op(&acc, name, (OpCode)ops[0], imms[0]) != 0) return -1;
    for (size_t i = 1; i < n; i++) {
        if (omega_program_build_unary_op(&step, name, (OpCode)ops[i], imms[i]) != 0) return -1;
        if (omega_program_compose(&acc, &step, &tmp, err, sizeof err) != 0) return -1;
        acc = tmp;
    }
    *out = acc;
    return 0;
}

static int lower_fn(const char *src, OmegaProgram *out) {
    OmegaAst ast;
    char err[256];
    if (omega_language_parse_line(src, &ast, err, sizeof err) != 0) return -1;
    return omega_language_lower_program(&ast, out, err, sizeof err);
}

static VisorSession g_s;
static int eval(const char *line, OmegaLangResult *r, char *err) {
    return omega_language_eval_line(&g_s, line, r, err, 256);
}
static void fresh(void) {
    visor_session_destroy(&g_s);
    if (visor_session_init(&g_s) != 0) { printf("FAIL: session init\n"); exit(2); }
}

/* ------------------------------------------------------------------ BODY_BOUND */
static void test_body_bound(void) {
    OmegaProgram a, b, c, d;
    /* 1. direct builder: x + 1 vs x + 2 */
    CHECK(G_BODY, omega_program_build_unary_op(&a, "f", OP_ADD, 1) == 0 &&
                  omega_program_build_unary_op(&b, "f", OP_ADD, 2) == 0, "build");
    CHECK(G_BODY, !ids_equal(&a.program_id, &b.program_id), "build_unary_op: f(x)=x+1 vs x+2 differ");
    /* 2. same name, same contract text, different body: the v1 collision */
    set_contract(&a, "true", "true");
    set_contract(&b, "true", "true");
    CHECK(G_BODY, !ids_equal(&a.program_id, &b.program_id), "same name + same contract, x+1 vs x+2 differ");
    CHECK(G_BODY, omega_program_build_unary_op(&c, "f", OP_SUB, 1) == 0, "build sub");
    set_contract(&c, "true", "true");
    CHECK(G_BODY, !ids_equal(&a.program_id, &c.program_id), "same contract, x+1 vs x-1 differ");
    /* equal cost low bytes (the v1 collision recipe): ADD and SUB have equal cost */
    CHECK(G_BODY, a.cost.insn_count == c.cost.insn_count && a.cost.latency_cycles == c.cost.latency_cycles,
          "precondition: equal cost");
    /* 3. Visor lowering path */
    CHECK(G_BODY, lower_fn("fn f(x: u64) -> u64 { x + 1 }", &a) == 0 &&
                  lower_fn("fn f(x: u64) -> u64 { x + 2 }", &b) == 0, "lower");
    CHECK(G_BODY, !ids_equal(&a.program_id, &b.program_id), "Visor lowering: fn f(x)=x+1 vs x+2 differ");
    CHECK(G_BODY, lower_fn("fn t(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }", &a) == 0 &&
                  lower_fn("fn t(x: u64) -> u64 requires true ensures result >= 1 { x * 3 + 1 }", &b) == 0,
          "lower 2");
    CHECK(G_BODY, !ids_equal(&a.program_id, &b.program_id), "Visor lowering: x*2+1 vs x*3+1 (same contract) differ");
    /* 4. composition: order matters (x*2+1 vs (x+1)*2) */
    uint8_t o1[2] = { OP_MUL, OP_ADD }, o2[2] = { OP_ADD, OP_MUL };
    uint64_t i1[2] = { 2, 1 }, i2[2] = { 1, 2 };
    CHECK(G_BODY, build_chain(&a, "g", o1, i1, 2) == 0 && build_chain(&b, "g", o2, i2, 2) == 0, "chains");
    set_contract(&a, "x >= 0", "same");
    set_contract(&b, "x >= 0", "same");
    CHECK(G_BODY, !ids_equal(&a.program_id, &b.program_id), "composition order separates");
    /* 5. the body root alone separates */
    SemanticId ra, rb;
    CHECK(G_BODY, omega_program_body_root_id(&a, &ra) == 0 && omega_program_body_root_id(&b, &rb) == 0 &&
                  !ids_equal(&ra, &rb), "body roots differ");
    /* 6. no body -> no identity (fail closed, never a shared id) */
    omega_program_init(&d, "opaque");
    d.contract = a.contract;
    memcpy(d.realization.code_bytes, a.realization.code_bytes, a.realization.code_len);
    d.realization.code_len = a.realization.code_len;
    memset(d.program_id.bytes, 0xAB, OMEGA_ID_BYTES);
    CHECK(G_BODY, omega_program_compute_id(&d) == -1 && id_zero(&d.program_id), "no body -> -1 and zero id");
    /* 7. constants wider than the realization can load are refused, not truncated */
    CHECK(G_BODY, omega_program_build_unary_op(&d, "wide", OP_ADD, 0x100000000ull) != 0, "wide imm refused");
    CHECK(G_BODY, omega_program_build_unary_op(&d, "edge", OP_ADD, 0xFFFFFFFFull) == 0, "32-bit imm accepted");
    /* 8. version boundary: v2 id != the v1 recipe for the same program */
    {
        OmegaProgram p;
        omega_program_build_unary_op(&p, "f", OP_ADD, 1);
        uint8_t buf[256];
        size_t pos = 0;
        buf[pos++] = 'P'; buf[pos++] = 'R'; buf[pos++] = 'O'; buf[pos++] = 'G';
        buf[pos++] = 1; buf[pos++] = 'f';
        buf[pos++] = (uint8_t)p.contract.input_type; buf[pos++] = (uint8_t)(p.contract.input_width & 0xFF);
        buf[pos++] = (uint8_t)p.contract.output_type; buf[pos++] = (uint8_t)(p.contract.output_width & 0xFF);
        memcpy(&buf[pos], p.contract.precondition_id.bytes, 32); pos += 32;
        memcpy(&buf[pos], p.contract.postcondition_id.bytes, 32); pos += 32;
        buf[pos++] = (uint8_t)(p.cost.insn_count & 0xFF); buf[pos++] = (uint8_t)(p.cost.latency_cycles & 0xFF);
        SemanticId v1;
        sha256_hash(buf, pos, v1.bytes);
        CHECK(G_BODY, !ids_equal(&v1, &p.program_id), "v2 id differs from the v1 recipe");
        CHECK(G_BODY, strcmp(OMEGA_PROGRAM_ID_DOMAIN, "omega.program.v2") == 0, "domain tag is omega.program.v2");
    }
}

/* ------------------------------------------------------- REPRESENTATION_INVARIANT */

/* Build the body graph by hand in a scrambled allocation order with filler objects. */
static int scrambled_root(const OmegaProgram *p, uint32_t seed, SemanticId *out) {
    OmegaGraph *g = omega_graph_create();
    if (!g) return -1;
    uint16_t w = p->contract.input_width;
    uint16_t n = p->body.step_count;
    SemanticId cid[OMEGA_PROGRAM_MAX_STEPS], oid[OMEGA_PROGRAM_MAX_STEPS];
    int rc = -1;
    /* filler first, so every real object lands at a different index */
    for (uint32_t k = 0; k < (seed % 5) + 1; k++) {
        OmegaObject *t8 = omega_build_type_uint(g, 8);
        if (!t8) goto out;
        SemanticId t8id = t8->id;
        if (!omega_build_val_uint(g, &t8id, 8, k)) goto out;
    }
    OmegaObject *t = omega_build_type_uint(g, w);
    if (!t) goto out;
    SemanticId tid = t->id;
    for (int i = (int)n - 1; i >= 0; i--) {   /* constants, reverse order */
        OmegaObject *c = omega_build_val_uint(g, &tid, w, p->body.steps[i].imm);
        if (!c) goto out;
        cid[i] = c->id;
    }
    for (uint16_t i = 0; i < n; i++) {        /* ops, with a bool type in between */
        if (i == 1 && !omega_build_type_bool(g)) goto out;
        OmegaObject *o = omega_build_op_binary(g, (OpCode)p->body.steps[i].op, OVERFLOW_WRAP, &tid);
        if (!o) goto out;
        oid[i] = o->id;
    }
    OmegaObject *param = omega_build_param(g, &tid, 0);
    if (!param) goto out;
    SemanticId cur = param->id;
    for (uint16_t i = 0; i < n; i++) {
        OmegaObject *a = omega_build_apply(g, &oid[i], &cur, &cid[i]);
        if (!a) goto out;
        cur = a->id;
    }
    *out = cur;
    rc = 0;
out:
    omega_graph_destroy(g);
    return rc;
}

/* Compose parts[lo, hi) with a seeded random bracketing. */
static uint64_t g_brng = 0x243F6A8885A308D3ull;
static int compose_bracketed(const OmegaProgram *parts, int lo, int hi, OmegaProgram *out) {
    if (hi - lo == 1) { *out = parts[lo]; return 0; }
    g_brng ^= g_brng >> 12; g_brng ^= g_brng << 25; g_brng ^= g_brng >> 27;
    int split = lo + 1 + (int)((g_brng * 0x2545F4914F6CDD1Dull) % (uint64_t)(hi - lo - 1));
    OmegaProgram *l = malloc(sizeof *l), *r = malloc(sizeof *r);
    char err[160];
    int rc = (l && r && compose_bracketed(parts, lo, split, l) == 0 && compose_bracketed(parts, split, hi, r) == 0 &&
              omega_program_compose(l, r, out, err, sizeof err) == 0) ? 0 : -1;
    free(l); free(r);
    return rc;
}

static void test_associativity(void) {
    static const uint8_t AOPS[5] = { OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR };
    uint64_t rng = 0x5851F42D4C957F2Dull;
    OmegaProgram *parts = malloc(8 * sizeof *parts);
    OmegaProgram *left = malloc(sizeof *left), *tmp = malloc(sizeof *tmp), *br = malloc(sizeof *br);
    long chains = 0, brackets = 0, swaps = 0;
    char err[160];
    for (int ch = 0; ch < 400; ch++) {
        rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
        int n = 3 + (int)(rng % 6);
        for (int i = 0; i < n; i++) {
            rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
            uint64_t v = rng * 0x2545F4914F6CDD1Dull;
            omega_program_build_unary_op(&parts[i], "p", (OpCode)AOPS[v % 5], (v >> 8) % 100);
            if ((v >> 40) % 4 == 0) {   /* some parts carry a user-stated leaf postcondition */
                char post[32];
                snprintf(post, sizeof post, "result >= %u", (unsigned)((v >> 48) % 7));
                set_contract(&parts[i], "x >= 0", post);
            }
        }
        *left = parts[0];
        for (int i = 1; i < n; i++) {
            omega_program_compose(left, &parts[i], tmp, err, sizeof err);
            *left = *tmp;
        }
        chains++;
        CHECK(G_REPR, left->contract.post_leaf_count == n, "chain %d flattens to %d leaves (%u)", ch, n,
              left->contract.post_leaf_count);
        for (int b = 0; b < 4; b++) {
            brackets++;
            BULK(G_REPR, compose_bracketed(parts, 0, n, br) == 0 && ids_equal(&br->program_id, &left->program_id) &&
                         br->contract.post_leaf_count == n,
                 "chain %d bracketing %d: id differs from the left fold", ch, b);
        }
        /* a;b != b;a when the bodies differ */
        if (parts[0].body.steps[0].op != parts[1].body.steps[0].op ||
            parts[0].body.steps[0].imm != parts[1].body.steps[0].imm) {
            OmegaProgram *ab = malloc(sizeof *ab), *ba = malloc(sizeof *ba);
            swaps++;
            BULK(G_REPR, omega_program_compose(&parts[0], &parts[1], ab, err, sizeof err) == 0 &&
                         omega_program_compose(&parts[1], &parts[0], ba, err, sizeof err) == 0 &&
                         !ids_equal(&ab->program_id, &ba->program_id), "chain %d: a;b == b;a", ch);
            free(ab); free(ba);
        }
    }
    /* a stale leaf list (postcondition replaced without clearing it) is treated as a leaf */
    {
        OmegaProgram *s = malloc(sizeof *s), *f = malloc(sizeof *f), *x1 = malloc(sizeof *x1), *x2 = malloc(sizeof *x2);
        *s = *left;
        omega_build_constraint_id(CONST_POSTCONDITION, "replaced", &s->contract.postcondition_id);  /* leaves kept */
        *f = *left;
        set_contract(f, "x >= 0", "replaced");
        CHECK(G_REPR, omega_program_compose(s, &parts[0], x1, err, sizeof err) == 0 &&
                      omega_program_compose(f, &parts[0], x2, err, sizeof err) == 0 &&
                      ids_equal(&x1->contract.postcondition_id, &x2->contract.postcondition_id) &&
                      x1->contract.post_leaf_count == 2, "stale leaf list is ignored");
        free(s); free(f); free(x1); free(x2);
    }
    printf("associativity: %ld chains, %ld random bracketings, %ld a;b vs b;a pairs\n", chains, brackets, swaps);
    free(parts); free(left); free(tmp); free(br);
}

static void test_representation(void) {
    OmegaProgram a, b, c;
    /* names are metadata */
    omega_program_build_unary_op(&a, "add_one", OP_ADD, 1);
    omega_program_build_unary_op(&b, "increment", OP_ADD, 1);
    CHECK(G_REPR, ids_equal(&a.program_id, &b.program_id), "different names, same body+contract -> same id");
    /* recompute is deterministic and name edits do not move it */
    c = a;
    snprintf(c.name, sizeof c.name, "renamed");
    omega_program_compute_id(&c);
    CHECK(G_REPR, ids_equal(&a.program_id, &c.program_id), "rename + recompute -> same id");
    /* cost and realization are metadata */
    c.cost.insn_count += 7; c.cost.latency_cycles = 99; c.cost.reg_pressure = 9; c.cost.memory_bytes = 64;
    c.realization.code_bytes[0] ^= 0xFF;
    omega_program_compute_id(&c);
    CHECK(G_REPR, ids_equal(&a.program_id, &c.program_id), "cost and code changes -> same id");
    /* pointer values: a graph pointer never enters the id */
    c.graph = (OmegaGraph *)(uintptr_t)0x1234;
    omega_program_compute_id(&c);
    c.graph = NULL;
    CHECK(G_REPR, ids_equal(&a.program_id, &c.program_id), "graph pointer -> same id");

    /* Visor source forms: whitespace, comments, parameter name, operand side, literal spelling */
    static const char *const same[] = {
        "fn f(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }",
        "fn f(x:u64)->u64 requires true ensures result>=1 {x*2+1}",
        "fn f(n: u64) -> u64 requires true ensures result >= 1 { n * 2 + 1 }",
        "fn f(x: u64) -> u64 requires true ensures result >= 1 { 1 + 2 * x }",
        "fn f(x: u64) -> u64 requires /* c */ true ensures result >= 0x1 { (x * 0b10) + 1 }",
        "fn   other_name(y: u64)   ->   u64 requires true ensures result >= 1 { (2 * y) + (1) }",
        NULL
    };
    OmegaProgram ref;
    CHECK(G_REPR, lower_fn(same[0], &ref) == 0, "lower ref");
    for (int i = 1; same[i]; i++) {
        OmegaProgram q;
        CHECK(G_REPR, lower_fn(same[i], &q) == 0 && ids_equal(&q.program_id, &ref.program_id),
              "'%s' has the reference id", same[i]);
    }
    /* Visor lowering == library composition when the fn states the same contract the
     * library program carries (the fn's ensures replaces the derived one). */
    uint8_t ops[2] = { OP_MUL, OP_ADD };
    uint64_t imms[2] = { 2, 1 };
    CHECK(G_REPR, build_chain(&c, "whatever", ops, imms, 2) == 0, "chain");
    set_contract(&c, "true", "result >= 1");
    CHECK(G_REPR, ids_equal(&c.program_id, &ref.program_id), "compose(mul2, add1) with ensures 'result >= 1' == Visor fn");
    /* composition associativity: the composed postcondition is the flattened leaf list,
     * so every bracketing of one chain has one id (no contract is held fixed here). */
    test_associativity();
    /* node allocation order: scrambled hand-built graph gives the same body root */
    for (uint32_t s = 0; s < 8; s++) {
        SemanticId canon, scr;
        CHECK(G_REPR, omega_program_body_root_id(&ref, &canon) == 0 && scrambled_root(&ref, s, &scr) == 0 &&
                      ids_equal(&canon, &scr), "scrambled allocation order (seed %u) -> same body root", s);
    }
    /* lift: the realization lifts back to exactly the body and the same id */
    {
        OmegaProgramBody lb;
        OmegaProgram lifted = ref;
        CHECK(G_REPR, omega_program_lift_body(ref.realization.code_bytes, ref.realization.code_len, &lb) == 0 &&
                      lb.step_count == ref.body.step_count &&
                      memcmp(lb.steps, ref.body.steps, lb.step_count * sizeof(OmegaProgramStep)) == 0,
              "lift(realization) == body");
        lifted.body = lb;
        omega_program_compute_id(&lifted);
        CHECK(G_REPR, ids_equal(&lifted.program_id, &ref.program_id), "lifted body -> same id");
        uint8_t code[AARCH64_MAX_CODE_BYTES];
        size_t len = 0;
        CHECK(G_REPR, omega_program_emit_body(&ref.body, code, &len, sizeof code) == 0 &&
                      len == ref.realization.code_len && memcmp(code, ref.realization.code_bytes, len) == 0,
              "emit(body) == builder realization");
        /* not-in-template code does not lift */
        CHECK(G_REPR, omega_program_lift_body(ref.realization.code_bytes + 4, ref.realization.code_len - 4, &lb) != 0,
              "misaligned slice does not lift");
    }
}

/* -------------------------------------------------------- MUTATION_SEPARATION */

static uint64_t g_rng;
static uint64_t rnd(void) {   /* xorshift64*, seeded */
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

static const uint8_t OPS[5] = { OP_ADD, OP_SUB, OP_MUL, OP_AND, OP_OR };
static const char *const PRE[3] = { "x >= 0", "true", "x < 10" };
static const char *const POST[3] = { "true", "result >= 1", "result == x" };

typedef struct {
    OmegaProgram p;
    uint8_t pre, post;
} Gen;

static void gen_program(Gen *g, int small) {
    OmegaProgram *p = &g->p;
    omega_program_init(p, "gen");
    size_t n = 1 + rnd() % (small ? 3 : 8);
    p->body.has_body = true;
    p->body.step_count = (uint16_t)n;
    for (size_t i = 0; i < n; i++) {
        p->body.steps[i].op = OPS[rnd() % 5];
        p->body.steps[i].imm = small ? rnd() % 3 : (rnd() & 0xFFFFFFFFull);
    }
    uint16_t w = (rnd() % 4 == 0) ? 32 : 64;
    p->contract.input_type = p->contract.output_type = TYPE_UNSIGNED_INT;
    p->contract.input_width = p->contract.output_width = w;
    g->pre = (uint8_t)(rnd() % 3);
    g->post = (uint8_t)(rnd() % 3);
    snprintf(p->name, sizeof p->name, "n%llu", (unsigned long long)(rnd() % 1000));
    set_contract(p, PRE[g->pre], POST[g->post]);
}

/* Hash-free oracle: structural equality of the canonical body and the contract. */
static int oracle_equal(const Gen *a, const Gen *b) {
    const OmegaProgram *x = &a->p, *y = &b->p;
    if (x->body.step_count != y->body.step_count) return 0;
    for (uint16_t i = 0; i < x->body.step_count; i++)
        if (x->body.steps[i].op != y->body.steps[i].op || x->body.steps[i].imm != y->body.steps[i].imm) return 0;
    return x->contract.input_type == y->contract.input_type && x->contract.input_width == y->contract.input_width &&
           x->contract.output_type == y->contract.output_type && x->contract.output_width == y->contract.output_width &&
           strcmp(x->contract.precondition, y->contract.precondition) == 0 &&
           strcmp(x->contract.postcondition, y->contract.postcondition) == 0;
}

#define DIFF_N 3000
#define MUT_N 2000
static Gen g_gen[DIFF_N];

static void test_mutation(void) {
    /* Differential: a small space so equal programs actually recur. */
    g_rng = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < DIFF_N; i++) gen_program(&g_gen[i], 1);
    long eq_pairs = 0, pairs = 0;
    for (int i = 0; i < DIFF_N; i++)
        for (int j = i + 1; j < DIFF_N; j++) {
            int o = oracle_equal(&g_gen[i], &g_gen[j]);
            int h = ids_equal(&g_gen[i].p.program_id, &g_gen[j].p.program_id);
            pairs++;
            eq_pairs += o;
            BULK(G_MUT, o == h, "pair %d,%d oracle=%d id_equal=%d", i, j, o, h);
        }
    CHECK(G_MUT, eq_pairs > 100, "differential exercised equal programs (%ld equal of %ld pairs)", eq_pairs, pairs);
    printf("differential: %d programs, %ld pairs, %ld structurally equal\n", DIFF_N, pairs, eq_pairs);

    /* Single mutations always separate (wide space). */
    g_rng = 0xD1B54A32D192ED03ull;
    long mut = 0;
    for (int i = 0; i < MUT_N; i++) {
        Gen base;
        gen_program(&base, 0);
        for (int kind = 0; kind < 8; kind++) {
            Gen m = base;
            OmegaProgram *p = &m.p;
            uint16_t k = (uint16_t)(rnd() % p->body.step_count);
            switch (kind) {
            case 0: { size_t cur = 0;
                      while (cur < 5 && OPS[cur] != p->body.steps[k].op) cur++;
                      p->body.steps[k].op = OPS[(cur + 1 + rnd() % 4) % 5]; } break;
            case 1: p->body.steps[k].imm ^= (1ull << (rnd() % 32)); break;
            case 2: p->contract.input_width = p->contract.output_width = (p->contract.input_width == 64) ? 32 : 64;
                    /* keep the constants representable at 32 bits (they are, < 2^32) */ break;
            case 3: m.pre = (uint8_t)((m.pre + 1 + rnd() % 2) % 3); break;
            case 4: m.post = (uint8_t)((m.post + 1 + rnd() % 2) % 3); break;
            case 5: if (p->body.step_count < OMEGA_PROGRAM_MAX_STEPS) {   /* append a step */
                        p->body.steps[p->body.step_count].op = OPS[rnd() % 5];
                        p->body.steps[p->body.step_count].imm = rnd() & 0xFFFF;
                        p->body.step_count++;
                    } break;
            case 6: if (p->body.step_count > 1) p->body.step_count--; else p->body.steps[0].imm += 1; break;
            case 7: p->contract.output_width = (p->contract.output_width == 64) ? 32 : 64; break;
            }
            set_contract(p, PRE[m.pre], POST[m.post]);
            if (oracle_equal(&m, &base)) continue;   /* a no-op mutation (cannot happen; guarded) */
            mut++;
            BULK(G_MUT, !ids_equal(&m.p.program_id, &base.p.program_id), "mutation kind %d on program %d did not separate", kind, i);
        }
    }
    printf("mutations: %ld single mutations over %d programs\n", mut, MUT_N);
}

/* -------------------------------------------------------- LIBRARY_REGRESSION */
static void test_library(void) {
    OmegaLibrary *lib = calloc(1, sizeof *lib);
    if (!lib) { printf("FAIL: oom\n"); exit(2); }
    omega_library_init(lib);
    VerifyReport rep;
    OmegaProgram a, b, c, d, comp;
    char err[160];
    omega_program_build_unary_op(&a, "add7", OP_ADD, 7);
    omega_program_build_unary_op(&b, "plus_seven", OP_ADD, 7);
    omega_program_build_unary_op(&c, "add7", OP_ADD, 8);
    a.is_verified = b.is_verified = c.is_verified = true;
    CHECK(G_LIB, omega_library_insert(lib, &a, NULL, 0, NULL) == 0, "insert add7");
    CHECK(G_LIB, omega_library_insert(lib, &b, NULL, 0, NULL) != 0 && lib->count == 1,
          "same body + contract under another name is a duplicate");
    CHECK(G_LIB, omega_library_insert(lib, &c, NULL, 0, NULL) == 0 && lib->count == 2,
          "different body under the same name is admitted");
    CHECK(G_LIB, omega_library_find_by_id(lib, &a.program_id) && omega_library_find_by_id(lib, &c.program_id) &&
                 omega_library_find_by_id(lib, &a.program_id) != omega_library_find_by_id(lib, &c.program_id),
          "find_by_id separates the two bodies");
    omega_program_init(&d, "opaque");
    d.contract = a.contract; d.is_verified = d.is_realized = true;
    omega_program_compute_id(&d);
    CHECK(G_LIB, omega_library_insert(lib, &d, NULL, 0, NULL) != 0 && lib->count == 2, "no-body program refused");
    /* dependency DAG */
    CHECK(G_LIB, omega_program_compose(&a, &c, &comp, err, sizeof err) == 0, "compose");
    comp.is_verified = true;
    SemanticId deps[2] = { a.program_id, c.program_id };
    CHECK(G_LIB, omega_library_insert(lib, &comp, deps, 2, NULL) == 0, "insert composite with deps");
    SemanticId cyc[1] = { comp.program_id };
    CHECK(G_LIB, omega_library_has_cycle(lib, &a.program_id, cyc, 1), "cycle detected");
    omega_library_destroy(lib);

    /* discovery: abstraction lifts to the shared 2x+1 body and is admissible */
    OmegaCorpus *corpus = calloc(1, sizeof *corpus);
    OmegaDiscoveryResult *res = calloc(1, sizeof *res);
    if (!corpus || !res) { printf("FAIL: oom\n"); exit(2); }
    omega_corpus_init(corpus);
    CHECK(G_LIB, omega_corpus_populate_benchmark(corpus) == 0 && corpus->count == 4, "corpus");
    for (size_t i = 0; i < corpus->count; i++)
        for (size_t j = i + 1; j < corpus->count; j++)
            CHECK(G_LIB, !ids_equal(&corpus->programs[i].program_id, &corpus->programs[j].program_id),
                  "corpus programs %zu,%zu distinct", i, j);
    CHECK(G_LIB, omega_discover_abstractions(corpus, res) == 0 && res->best_candidate_index >= 0, "discover");
    if (res->best_candidate_index >= 0) {
        const OmegaAbstractionCandidate *cand = &res->candidates[res->best_candidate_index];
        const OmegaProgramBody *bb = &cand->abstraction.body;
        CHECK(G_LIB, cand->is_verified && bb->has_body && bb->step_count == 2 &&
                     bb->steps[0].op == OP_MUL && bb->steps[0].imm == 2 &&
                     bb->steps[1].op == OP_ADD && bb->steps[1].imm == 1, "abstraction lifted to mul2;add1");
        CHECK(G_LIB, !id_zero(&cand->abstraction.program_id), "abstraction has an identity");
        OmegaProgram ref;
        CHECK(G_LIB, omega_refactor_program(&corpus->programs[0], &cand->abstraction, cand->slice_offset_insns,
                                            cand->slice_len_insns, &ref) == 0 &&
                     ids_equal(&ref.program_id, &corpus->programs[0].program_id) &&
                     ref.cost.insn_count != corpus->programs[0].cost.insn_count,
              "refactoring preserves meaning -> same id, different cost");
        OmegaLibrary *l2 = calloc(1, sizeof *l2);
        omega_library_init(l2);
        uint8_t rh[32] = { 1 };
        CHECK(G_LIB, omega_discovery_admit_to_library(l2, cand, rh) == 0 && l2->count == 1, "abstraction admitted");
        omega_library_destroy(l2);
        free(l2);
    }
    /* every candidate id is either zero-with-unverified or unique */
    for (size_t i = 0; i < res->candidate_count; i++) {
        const OmegaAbstractionCandidate *ci = &res->candidates[i];
        CHECK(G_LIB, ci->is_verified ? !id_zero(&ci->abstraction.program_id) : 1, "candidate %zu identity", i);
        for (size_t j = i + 1; j < res->candidate_count; j++)
            if (!id_zero(&ci->abstraction.program_id))
                CHECK(G_LIB, !ids_equal(&ci->abstraction.program_id, &res->candidates[j].abstraction.program_id),
                      "candidates %zu,%zu distinct", i, j);
    }
    omega_corpus_destroy(corpus);
    free(corpus);
    free(res);

    /* realization triple id follows the program id */
    {
        OmegaProgram p1, p2;
        omega_program_build_unary_op(&p1, "p", OP_MUL, 3);
        omega_program_build_unary_op(&p2, "p", OP_MUL, 4);
        OmegaMachineGraph *mg = calloc(1, sizeof *mg);
        omega_machine_build_dgx_spark(mg);
        SemanticId t1, t2;
        omega_realize_compute_triple_id(&p1.program_id, &mg->machine_id, &p1.realization, &t1);
        omega_realize_compute_triple_id(&p2.program_id, &mg->machine_id, &p1.realization, &t2);
        CHECK(G_LIB, !ids_equal(&t1, &t2), "triple id separates bodies");
        free(mg);
    }
    /* verification still accepts builder programs */
    omega_program_build_unary_op(&a, "v", OP_OR, 0x10);
    CHECK(G_LIB, omega_program_verify(&a, &rep) == 0 && a.is_verified, "verify builder program");
    /* composition beyond the body capacity is refused */
    {
        OmegaProgram big;
        uint8_t ops[OMEGA_PROGRAM_MAX_STEPS];
        uint64_t imms[OMEGA_PROGRAM_MAX_STEPS];
        for (int i = 0; i < OMEGA_PROGRAM_MAX_STEPS; i++) { ops[i] = OP_ADD; imms[i] = 1; }
        CHECK(G_LIB, build_chain(&big, "big", ops, imms, OMEGA_PROGRAM_MAX_STEPS) == 0 &&
                     big.body.step_count == OMEGA_PROGRAM_MAX_STEPS && !id_zero(&big.program_id), "64-step body");
        OmegaProgram one, out;
        omega_program_build_unary_op(&one, "one", OP_ADD, 1);
        CHECK(G_LIB, omega_program_compose(&big, &one, &out, err, sizeof err) != 0, "65 steps refused: %s", err);
    }
}

/* ----------------------------------------------------------- VISOR_REGRESSION */
static void test_visor(void) {
    OmegaLangResult r1, r2, r3;
    char err[256];
    fresh();
    CHECK(G_VISOR, eval("fn t(x: u64) -> u64 { x + 1 }", &r1, err) == 0, "fn t x+1: %s", err);
    fresh();   /* `clear`: a new session */
    CHECK(G_VISOR, eval("fn t(x: u64) -> u64 { x + 2 }", &r2, err) == 0, "fn t x+2: %s", err);
    CHECK(G_VISOR, !ids_equal(&r1.id, &r2.id), "after clear: fn t x+1 vs x+2 -> distinct ids");
    /* redefinition in one session: distinct id, program added, name rebound, no collision */
    size_t np = g_s.program_count;
    CHECK(G_VISOR, eval("fn t(x: u64) -> u64 { x + 3 }", &r3, err) == 0 && !strstr(err, "collision"),
          "redefine t: %s", err);
    const VisorBinding *b = visor_binding_get(&g_s, "t");
    CHECK(G_VISOR, !ids_equal(&r2.id, &r3.id) && g_s.program_count == np + 1 && b && ids_equal(&b->id, &r3.id),
          "redefinition: distinct id, program added, t rebound");
    /* identical redefinition reuses the entry */
    np = g_s.program_count;
    CHECK(G_VISOR, eval("fn t(y: u64) -> u64 { y+3 }", &r1, err) == 0 && ids_equal(&r1.id, &r3.id) &&
                   g_s.program_count == np, "identical body (other spelling) reuses the entry");
    /* another name, same program -> same id, same entry */
    CHECK(G_VISOR, eval("fn u(x: u64) -> u64 { x + 3 }", &r1, err) == 0 && ids_equal(&r1.id, &r3.id) &&
                   r1.program_index == r3.program_index && g_s.program_count == np, "alias reuses the entry");
    /* the stored program's id is current and its realization is the emitted body */
    const OmegaProgram *p = &g_s.programs[r3.program_index];
    OmegaProgram copy = *p;
    omega_program_compute_id(&copy);
    CHECK(G_VISOR, ids_equal(&copy.program_id, &p->program_id), "stored id is current");
    uint8_t code[AARCH64_MAX_CODE_BYTES];
    size_t len = 0;
    CHECK(G_VISOR, omega_program_emit_body(&p->body, code, &len, sizeof code) == 0 &&
                   len == p->realization.code_len && memcmp(code, p->realization.code_bytes, len) == 0,
          "stored realization == emit(body)");
    /* contract still separates in Visor */
    CHECK(G_VISOR, eval("fn t(x: u64) -> u64 ensures result >= 3 { x + 3 }", &r1, err) == 0 &&
                   !ids_equal(&r1.id, &r3.id), "ensures separates");
    visor_session_destroy(&g_s);
}

int main(void) {
    test_body_bound();
    test_representation();
    test_mutation();
    test_library();
    test_visor();
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
