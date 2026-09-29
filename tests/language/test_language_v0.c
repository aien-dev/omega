/*
 * test_language_v0.c -- Omega surface language V0 (lane 3) tests.
 *
 * Locks the semantic laws of spec/omega-language-v0.md: literal spelling,
 * whitespace/comments and binding names never reach a SemanticId; widths do;
 * lowering is deterministic and maps onto the existing builders; everything
 * outside V0 fails closed without leaving objects behind.
 *
 * Usage: test_language_v0 [golden_file]          (default tests/language/golden/v0.txt)
 *        test_language_v0 --regen [golden_file]  (rewrite the golden file)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_core.h"
#include "omega_canonical.h"
#include "omega_program.h"
#include "omega_exec.h"
#include "visor.h"
#include "omega_lex.h"
#include "omega_parse.h"
#include "omega_lower.h"

static int g_pass = 0, g_total = 0;

#define CHECK(cond, ...) do { \
    g_total++; \
    if (cond) g_pass++; \
    else { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static const char *DEFAULT_GOLDEN = "tests/language/golden/v0.txt";

/* ---------- helpers ---------- */

static int eval(VisorSession *s, const char *line, OmegaLangResult *r, char *err) {
    return omega_language_eval_line(s, line, r, err, 256);
}

static void fresh(VisorSession *s) {
    visor_session_destroy(s);
    if (visor_session_init(s) != 0) { printf("FAIL: session init\n"); exit(2); }
}

static int ids_equal(const SemanticId *a, const SemanticId *b) {
    return memcmp(a->bytes, b->bytes, OMEGA_ID_BYTES) == 0;
}

/* Lower an expression line into a brand-new graph with no bindings. */
static int lower_fresh(const char *line, SemanticId *id, char *err) {
    OmegaAst ast;
    int rc = omega_language_parse_line(line, &ast, err, 256);
    if (rc) return rc;
    OmegaGraph *g = omega_graph_create();
    rc = omega_language_lower_to_graph(g, NULL, NULL, &ast, id, NULL, err, 256);
    omega_graph_destroy(g);
    return rc;
}

static void hex_bytes(const uint8_t *b, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hx[b[i] >> 4]; out[2 * i + 1] = hx[b[i] & 15]; }
    out[2 * n] = '\0';
}

/* ---------- golden vectors ---------- */

static const char *const GOLDEN_SRC[] = {
    "E\t7: u64",
    "E\t07: u64",
    "E\t0x07: u64",
    "E\t0b111: u64",
    "E\t(7: u64)",
    "E\t7: u32",
    "E\t255: u8",
    "E\t65535: u16",
    "E\t18446744073709551615: u64",
    "E\ttrue",
    "E\tfalse",
    "E\t(3: u64) + 4",
    "E\t(3: u64)+4",
    "E\t(3: u64) + /* c */ 4",
    "E\t(10: u64) - 3 * 2",
    "E\t(0xff: u64) & 0x0f | 0x30",
    "E\t(100: u32) / 7",
    "E\tlet a: u64 = 7",
    "E\tlet b: u64 = 7",
    "E\tlet c: u64 = (1: u64) + 2",
    "P\tfn transform(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }",
    "P\tfn transform(y: u64) -> u64 requires true ensures result>=1 { y*2+1 }",
    "P\tfn transform(x: u64) -> u64 requires true ensures result >= 1 { 2 * x + 1 }",
    "P\tfn mask(x: u64) -> u64 { x & 0xff }",
    NULL
};

/* Produce "encoding_hex\tid_hex" for one golden source. */
static int golden_compute(char kind, const char *src, char *enc_hex, size_t enc_cap, char *id_hex, char *err) {
    OmegaAst ast;
    int rc = omega_language_parse_line(src, &ast, err, 256);
    if (rc) return rc;
    if (kind == 'E') {
        OmegaGraph *g = omega_graph_create();
        SemanticId id;
        rc = omega_language_lower_to_graph(g, NULL, NULL, &ast, &id, NULL, err, 256);
        if (rc == 0) {
            const OmegaObject *o = omega_graph_find_object_const(g, &id);
            uint8_t buf[8192];
            size_t len = 0;
            if (!o || omega_canonical_encode(o, buf, sizeof buf, &len) != 0 || 2 * len + 1 > enc_cap) rc = -9;
            else { hex_bytes(buf, len, enc_hex); omega_hex_semantic_id(&id, id_hex); }
        }
        omega_graph_destroy(g);
        return rc;
    }
    OmegaProgram p;
    rc = omega_language_lower_program(&ast, &p, err, 256);
    if (rc) return rc;
    /* For programs the "encoding" is the realized code the existing builder emitted. */
    if (2 * p.realization.code_len + 1 > enc_cap) return -9;
    hex_bytes(p.realization.code_bytes, p.realization.code_len, enc_hex);
    omega_hex_semantic_id(&p.program_id, id_hex);
    return 0;
}

static int golden_regen(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); return 1; }
    fprintf(f, "# Omega language V0 golden vectors: kind<TAB>source<TAB>encoding_hex<TAB>semantic_id_hex\n");
    fprintf(f, "# E = expression/let RHS lowered into a fresh graph; encoding = omega_canonical_encode of the root object.\n");
    fprintf(f, "# P = fn lowered to OmegaProgram; encoding = realization code bytes; id = program_id.\n");
    static char enc[20000];
    for (int i = 0; GOLDEN_SRC[i]; i++) {
        char id_hex[65], err[256];
        const char *src = GOLDEN_SRC[i] + 2;
        if (golden_compute(GOLDEN_SRC[i][0], src, enc, sizeof enc, id_hex, err) != 0) {
            printf("regen failed on '%s': %s\n", src, err);
            fclose(f);
            return 1;
        }
        fprintf(f, "%c\t%s\t%s\t%s\n", GOLDEN_SRC[i][0], src, enc, id_hex);
    }
    fclose(f);
    printf("wrote %s\n", path);
    return 0;
}

static void test_golden(const char *path) {
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "golden file %s missing", path);
    if (!f) return;
    static char line[40000], enc[20000];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = '\0';
        char *kind = line, *src = strchr(kind, '\t');
        if (!src) { CHECK(0, "bad golden line"); continue; }
        *src++ = '\0';
        char *want_enc = strchr(src, '\t');
        if (!want_enc) { CHECK(0, "bad golden line"); continue; }
        *want_enc++ = '\0';
        char *want_id = strchr(want_enc, '\t');
        if (!want_id) { CHECK(0, "bad golden line"); continue; }
        *want_id++ = '\0';
        char id_hex[65], err[256];
        int rc = golden_compute(kind[0], src, enc, sizeof enc, id_hex, err);
        CHECK(rc == 0, "golden '%s' failed to lower: %s", src, err);
        if (rc) continue;
        CHECK(strcmp(enc, want_enc) == 0, "golden '%s' encoding changed", src);
        CHECK(strcmp(id_hex, want_id) == 0, "golden '%s' id changed: %s != %s", src, id_hex, want_id);
        n++;
    }
    fclose(f);
    CHECK(n >= 20, "golden file has only %d vectors", n);
}

/* ---------- laws ---------- */

static void test_literal_spellings(void) {
    const char *forms[] = { "7: u64", "07: u64", "0x07: u64", "0b111: u64", "(7: u64)", "0x7 : u64",
                            "/* seven */ 7: u64 // done", NULL };
    SemanticId base, id;
    char err[256];
    CHECK(lower_fresh(forms[0], &base, err) == 0, "7: u64 lowers: %s", err);
    for (int i = 1; forms[i]; i++) {
        CHECK(lower_fresh(forms[i], &id, err) == 0, "'%s' lowers: %s", forms[i], err);
        CHECK(ids_equal(&base, &id), "'%s' has the same id as 7: u64", forms[i]);
    }
    /* Same id as the direct builder call. */
    OmegaGraph *g = omega_graph_create();
    OmegaObject *t = omega_build_type_uint(g, 64);
    OmegaObject *v = omega_build_val_uint(g, &t->id, 64, 7);
    CHECK(ids_equal(&base, &v->id), "7: u64 == omega_build_val_uint(u64, 7)");
    omega_graph_destroy(g);
}

static void test_width_changes_id(void) {
    SemanticId a, b, c;
    char err[256];
    CHECK(lower_fresh("7: u64", &a, err) == 0, "u64");
    CHECK(lower_fresh("7: u32", &b, err) == 0, "u32");
    CHECK(lower_fresh("7: u8", &c, err) == 0, "u8");
    CHECK(!ids_equal(&a, &b), "u32 7 and u64 7 differ");
    CHECK(!ids_equal(&b, &c), "u8 7 and u32 7 differ");
}

static void test_whitespace_comments_and_names(void) {
    VisorSession s;
    memset(&s, 0, sizeof s);
    fresh(&s);
    OmegaLangResult r, r2;
    char err[256];
    CHECK(eval(&s, "let x: u64 = 3", &r, err) == 0, "let x: %s", err);
    CHECK(r.kind == OMEGA_LANG_BINDING && strcmp(r.name, "x") == 0 && strcmp(r.type_text, "u64") == 0,
          "let result shape");
    CHECK(eval(&s, "let y: u64 = 4", &r, err) == 0, "let y: %s", err);
    const char *forms[] = { "x + y", "x+y", "x + /* c */ y", "  x\t+   y  // trailing", "(x) + (y)", "(x + y)",
                            "(x + y : u64)", "x + y: u64", NULL };
    CHECK(eval(&s, forms[0], &r, err) == 0, "x + y: %s", err);
    CHECK(r.kind == OMEGA_LANG_EXPRESSION, "x + y is an expression");
    uint16_t count_after_first = s.graph->object_count;
    for (int i = 1; forms[i]; i++) {
        CHECK(eval(&s, forms[i], &r2, err) == 0, "'%s': %s", forms[i], err);
        CHECK(ids_equal(&r.id, &r2.id), "'%s' has the same apply id as 'x + y'", forms[i]);
    }
    CHECK(s.graph->object_count == count_after_first, "re-entering equal expressions adds no objects (dedupe)");

    /* The apply equals the one built by hand with the existing builders (OVERFLOW_WRAP). */
    OmegaGraph *g = omega_graph_create();
    OmegaObject *t = omega_build_type_uint(g, 64);
    SemanticId tid = t->id;
    SemanticId v3 = omega_build_val_uint(g, &tid, 64, 3)->id;
    SemanticId v4 = omega_build_val_uint(g, &tid, 64, 4)->id;
    SemanticId op = omega_build_op_binary(g, OP_ADD, OVERFLOW_WRAP, &tid)->id;
    SemanticId ap = omega_build_apply(g, &op, &v3, &v4)->id;
    CHECK(ids_equal(&r.id, &ap), "x + y == omega_build_apply(ADD/WRAP u64, 3, 4)");
    CHECK(ids_equal(&r.type_id, &tid), "expression type_id is the u64 TYPE id");
    omega_graph_destroy(g);

    /* Binding names never enter the id. */
    OmegaLangResult ra, rb;
    uint16_t before = s.graph->object_count;
    CHECK(eval(&s, "let a: u64 = 7", &ra, err) == 0, "let a");
    uint16_t mid = s.graph->object_count;
    CHECK(eval(&s, "let b: u64 = 7", &rb, err) == 0, "let b");
    CHECK(ids_equal(&ra.id, &rb.id), "let a = 7 and let b = 7 bind the same value id");
    CHECK(mid == before + 1 && s.graph->object_count == mid, "second identical let adds no object");
    const VisorBinding *vb = visor_binding_get(&s, "b");
    CHECK(vb && vb->kind == VISOR_BIND_OBJECT && ids_equal(&vb->id, &rb.id), "binding b set via visor");

    /* let with an expression binds the expression's id. */
    CHECK(eval(&s, "let z: u64 = x + y", &rb, err) == 0, "let z = x + y: %s", err);
    CHECK(ids_equal(&rb.id, &r.id), "let z = x + y binds the apply id");
    /* Contextual literal typing: x + 1 takes u64 from x. */
    CHECK(eval(&s, "x + 1", &r2, err) == 0, "x + 1: %s", err);
    SemanticId explicit_id;
    CHECK(eval(&s, "x + (1: u64)", &rb, err) == 0 && (explicit_id = rb.id, 1), "x + (1: u64)");
    CHECK(ids_equal(&r2.id, &explicit_id), "x + 1 == x + (1: u64)");
    /* Rebinding shadows. */
    CHECK(eval(&s, "let x: u64 = x * 2", &rb, err) == 0, "rebind x: %s", err);
    vb = visor_binding_get(&s, "x");
    CHECK(vb && ids_equal(&vb->id, &rb.id), "x now names the new apply");
    /* `_` resolves through session->last (set by the console). */
    visor_set_last(&s, visor_binding_get(&s, "y"));
    CHECK(eval(&s, "_ + x", &rb, err) == 0, "_ + x: %s", err);
    visor_session_destroy(&s);
}

static void test_determinism(void) {
    const char *lines[] = { "(3: u64) + 4 * 5", "(0xff: u32) & 0x0f | 1", "true", "(9: u16) / 3", NULL };
    for (int i = 0; lines[i]; i++) {
        SemanticId a, b;
        char err[256];
        CHECK(lower_fresh(lines[i], &a, err) == 0, "'%s' lowers: %s", lines[i], err);
        CHECK(lower_fresh(lines[i], &b, err) == 0, "'%s' lowers again", lines[i]);
        CHECK(ids_equal(&a, &b), "'%s' deterministic across fresh graphs", lines[i]);
    }
    /* Precedence: * binds tighter than +, & tighter than |. */
    SemanticId p1, p2;
    char err[256];
    CHECK(lower_fresh("(3: u64) + 4 * 5", &p1, err) == 0 && lower_fresh("(3: u64) + (4 * 5)", &p2, err) == 0,
          "precedence lowers");
    CHECK(ids_equal(&p1, &p2), "a + b * c == a + (b * c)");
    CHECK(lower_fresh("((3: u64) + 4) * 5", &p2, err) == 0 && !ids_equal(&p1, &p2), "(a + b) * c differs");
    CHECK(lower_fresh("(1: u64) | 2 & 3", &p1, err) == 0 && lower_fresh("(1: u64) | (2 & 3)", &p2, err) == 0 &&
          ids_equal(&p1, &p2), "a | b & c == a | (b & c)");
}

typedef struct { const char *line; int rc; } Reject;

static void test_fail_closed(void) {
    const Reject cases[] = {
        /* ambiguous width */
        { "7", -2 }, { "(7)", -2 }, { "1 + 2", -2 }, { "(1 + 2) * 3", -2 },
        /* literal range */
        { "256: u8", -2 }, { "65536: u16", -2 }, { "4294967296: u32", -2 },
        { "18446744073709551616: u64", -2 }, { "0x10000000000000000: u64", -2 },
        { "let q: u8 = 300", -2 },
        /* mixed widths / types */
        { "(1: u32) + (1: u64)", -2 }, { "let w: u32 = (1: u64)", -2 }, { "(true: u64)", -2 },
        { "true + false", -2 }, { "(1: u64) + true", -2 }, { "let t: bool = 1", -2 },
        /* not in V0 */
        { "let s: i64 = 1", -2 }, { "(1: i32)", -2 }, { "let f: f64 = 1", -2 },
        { "\"hello\"", -2 }, { "[1, 2]", -2 }, { "-1", -2 }, { "!true", -2 },
        { "(1: u64) == 1", -2 }, { "(1: u64) < 2", -2 }, { "(4: u64) / 0", -2 },
        { "effect", -2 }, { "mint(1)", -2 }, { "let cap: u64 = 1", -2 }, { "publish + 1", -2 },
        { "unknown_name + 1", -2 }, { "f(1)", -2 }, { "let u64: u64 = 1", -2 },
        /* syntax */
        { "let x u64 = 7", -1 }, { "let x = 7", -1 }, { "(1: u64) +", -1 }, { ")", -1 },
        { "(1: u64) (2: u64)", -1 }, { "7u64", -1 }, { "0x: u64", -1 }, { "0b2: u64", -1 },
        { "/* open", -1 }, { "(1: u64", -1 }, { "let _: u64 = 1", -1 }, { "x;", -1 },
        { NULL, 0 }
    };
    VisorSession s;
    memset(&s, 0, sizeof s);
    fresh(&s);
    OmegaLangResult r;
    char err[256];
    CHECK(eval(&s, "let seed: u64 = 1", &r, err) == 0, "seed binding");
    for (int i = 0; cases[i].line; i++) {
        uint16_t before = s.graph->object_count;
        size_t nb = s.bindings.count, np = s.program_count;
        err[0] = '\0';
        int rc = eval(&s, cases[i].line, &r, err);
        CHECK(rc == cases[i].rc, "'%s' -> %d (want %d): %s", cases[i].line, rc, cases[i].rc, err);
        CHECK(s.graph->object_count == before && s.bindings.count == nb && s.program_count == np,
              "'%s' leaves the session unchanged", cases[i].line);
        CHECK(strncmp(err, "column ", 7) == 0 && !strchr(err, '\n'), "'%s' has a one-line column message: %s",
              cases[i].line, err);
        CHECK(r.kind == OMEGA_LANG_NONE, "'%s' reports no result", cases[i].line);
    }
    /* Empty / comment-only lines are fine and do nothing. */
    CHECK(eval(&s, "   // nothing", &r, err) == 0 && r.kind == OMEGA_LANG_NONE, "comment-only line");
    visor_session_destroy(&s);
}

static void test_depth_and_capacity(void) {
    char line[600];
    char err[256];
    SemanticId id;
    /* 40 nested parens: beyond the depth limit (32). */
    size_t p = 0;
    for (int i = 0; i < 40; i++) line[p++] = '(';
    p += (size_t)sprintf(line + p, "7: u64");
    for (int i = 0; i < 40; i++) line[p++] = ')';
    line[p] = '\0';
    CHECK(lower_fresh(line, &id, err) == -3, "deep nesting fails closed with -3: %s", err);
    /* 20 nested parens is fine. */
    p = 0;
    for (int i = 0; i < 20; i++) line[p++] = '(';
    p += (size_t)sprintf(line + p, "7: u64");
    for (int i = 0; i < 20; i++) line[p++] = ')';
    line[p] = '\0';
    SemanticId plain;
    CHECK(lower_fresh(line, &id, err) == 0 && lower_fresh("7: u64", &plain, err) == 0 && ids_equal(&id, &plain),
          "20 redundant parens lower to the same id");
    /* Too many tokens. */
    p = (size_t)sprintf(line, "(1: u64)");
    for (int i = 0; i < 140; i++) p += (size_t)sprintf(line + p, "+1");
    CHECK(lower_fresh(line, &id, err) == -3, "over-long expression fails closed with -3: %s", err);

    /* Graph capacity: fill the graph, then a new expression must fail with -3 and roll back. */
    VisorSession s;
    memset(&s, 0, sizeof s);
    fresh(&s);
    OmegaLangResult r;
    for (int i = 0; s.graph->object_count < OMEGA_MAX_GRAPH_OBJECTS - 2; i++) {
        sprintf(line, "let v: u64 = %d", i);
        if (eval(&s, line, &r, err) != 0) break;
    }
    uint16_t before = s.graph->object_count;
    CHECK(before == OMEGA_MAX_GRAPH_OBJECTS - 2, "graph filled to %u", before);
    CHECK(eval(&s, "(100000: u64) + 100001", &r, err) == -3, "full graph -> -3: %s", err);
    CHECK(s.graph->object_count == before, "full graph rolled back to %u (now %u)", before, s.graph->object_count);
    CHECK(eval(&s, "v + v", &r, err) == 0, "two free slots still usable: %s", err);
    visor_session_destroy(&s);
}

static void test_programs(void) {
    VisorSession s;
    memset(&s, 0, sizeof s);
    fresh(&s);
    OmegaLangResult r, r2;
    char err[256];
    uint16_t objs = s.graph->object_count;
    const char *src = "fn transform(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }";
    CHECK(eval(&s, src, &r, err) == 0, "fn transform: %s", err);
    CHECK(r.kind == OMEGA_LANG_PROGRAM && r.program_index == 0 && strcmp(r.name, "transform") == 0,
          "fn result shape");
    CHECK(s.graph->object_count == objs, "fn does not touch the graph");
    const OmegaProgram *p = &s.programs[0];
    CHECK(s.program_count == 1 && ids_equal(&p->program_id, &r.id), "program stored via visor_program_add");
    CHECK(strcmp(p->name, "transform") == 0, "program named after fn");
    CHECK(strcmp(p->contract.precondition, "true") == 0 && strcmp(p->contract.postcondition, "result >= 1") == 0,
          "contract text: '%s' / '%s'", p->contract.precondition, p->contract.postcondition);
    SemanticId pre, post;
    omega_build_constraint_id(CONST_PRECONDITION, "true", &pre);
    omega_build_constraint_id(CONST_POSTCONDITION, "result >= 1", &post);
    CHECK(ids_equal(&pre, &p->contract.precondition_id) && ids_equal(&post, &p->contract.postcondition_id),
          "contract ids via omega_build_constraint_id");
    OmegaProgram copy = *p;
    omega_program_compute_id(&copy);
    CHECK(ids_equal(&copy.program_id, &p->program_id), "program_id is current (recomputed after rename)");
    const VisorBinding *vb = visor_binding_get(&s, "transform");
    CHECK(vb && vb->kind == VISOR_BIND_PROGRAM && vb->index == 0 && ids_equal(&vb->id, &r.id), "fn bound");
    CHECK(p->contract.input_type == TYPE_UNSIGNED_INT && p->contract.input_width == 64, "u64 contract");

    /* Same program from the builders by hand: build(MUL 2) then compose with build(ADD 1). */
    OmegaProgram a, b, c;
    omega_program_build_unary_op(&a, "transform", OP_MUL, 2);
    omega_program_build_unary_op(&b, "transform", OP_ADD, 1);
    omega_program_compose(&a, &b, &c, NULL, 0);
    CHECK(c.realization.code_len == p->realization.code_len &&
          memcmp(c.realization.code_bytes, p->realization.code_bytes, c.realization.code_len) == 0,
          "code equals build_unary_op(MUL 2) o build_unary_op(ADD 1)");

#if defined(__aarch64__)
    /* Checked through the EXISTING executor (not a private evaluator). */
    uint64_t out = 0;
    CHECK(omega_program_exec(p, 5, &out) == 0 && out == 11, "transform(5) == 11 via omega_program_exec (%llu)",
          (unsigned long long)out);
#else
    CHECK(1, "exec skipped off aarch64");
#endif

    /* Identity laws: param name, spacing and commutative spelling don't matter; fn name does. */
    const char *same[] = {
        "fn transform(y: u64) -> u64 requires true ensures result>=1 { y*2+1 }",
        "fn transform(x: u64) -> u64 requires true ensures result >= 1 { 2 * x + 1 }",
        "fn transform(x: u64) -> u64 requires /* pre */ true ensures result >= 0x1 { (x * 2) + 1 }",
        "fn transform(x: u64) -> u64 requires true ensures result >= 1 { 1 + x * 2 }",
        NULL
    };
    for (int i = 0; same[i]; i++) {
        CHECK(eval(&s, same[i], &r2, err) == 0, "'%s': %s", same[i], err);
        CHECK(ids_equal(&r.id, &r2.id), "'%s' has transform's program id", same[i]);
    }
    CHECK(eval(&s, "fn other(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }", &r2, err) == 0 &&
          !ids_equal(&r.id, &r2.id), "different fn name -> different program id (existing model hashes name)");
    CHECK(eval(&s, "fn transform(x: u64) -> u64 requires true ensures result >= 2 { x * 2 + 1 }", &r2, err) == 0 &&
          !ids_equal(&r.id, &r2.id), "different ensures -> different program id");
    CHECK(s.program_count == 3, "identical fns reuse one table entry (%zu entries)", s.program_count);
    /* The existing program_id does not hash the code: a different body with the same
     * name + contract + cost would reuse the id. Lowering refuses that collision. */
    {
        size_t np = s.program_count;
        int rc = eval(&s, "fn transform(x: u64) -> u64 requires true ensures result >= 1 { x * 3 + 1 }", &r2, err);
        CHECK(rc == -2 && strstr(err, "collision") && s.program_count == np,
              "different body under the same program id is refused: %d %s", rc, err);
    }
    CHECK(eval(&s, "fn m(x: u64) -> u64 { x & 0xff }", &r2, err) == 0, "fn without contract: %s", err);
    CHECK(strcmp(s.programs[r2.program_index].contract.precondition, "true") == 0 &&
          strcmp(s.programs[r2.program_index].contract.postcondition, "true") == 0, "omitted clauses are 'true'");
    CHECK(eval(&s, "fn s(x: u64) -> u64 requires x < 10 ensures result == x - 1 { x - 1 }", &r2, err) == 0 &&
          strcmp(s.programs[r2.program_index].contract.precondition, "x < 10") == 0 &&
          strcmp(s.programs[r2.program_index].contract.postcondition, "result == x - 1") == 0,
          "clause text canonical: %s", err);

    /* Failures: no program, no binding, no objects. */
    const Reject bad[] = {
        { "fn r(x: u64) -> u64 { r(x) }", -2 },                 /* call / recursion */
        { "fn r(x: u64) -> u64 { r + 1 }", -2 },                /* recursion by name */
        { "fn g(x: u64) -> u64 { x * x }", -2 },                /* not a constant step */
        { "fn g(x: u64) -> u64 { x * (2 + 1) }", -2 },          /* constant folding = evaluation */
        { "fn g(x: u64) -> u64 { x / 2 }", -2 },                /* not in program op set */
        { "fn g(x: u64) -> u64 { x + 4294967296 }", -2 },       /* imm wider than 32 bits */
        { "fn g(x: u64) -> u64 { x }", -2 },                    /* no step */
        { "fn g(x: u64) -> u64 { 5 + 1 }", -2 },                /* no parameter */
        { "fn g(x: u32) -> u32 { x + 1 }", -2 },                /* width fixed at 64 */
        { "fn g(x: u64) -> u64 { 1 - x }", -2 },                /* sub is not commutative */
        { "fn g(x: u64, y: u64) -> u64 { x + 1 }", -2 },        /* one param only */
        { "fn g(x: u64) -> u64 { x + seed }", -2 },             /* outside names */
        { "fn g(x: u64) -> u64 requires result > 0 { x + 1 }", -2 },  /* result in requires */
        { "fn g(x: u64) -> u64 requires z > 0 { x + 1 }", -2 },       /* unknown name in clause */
        { "fn g(x: u64) -> u64 ensures result >= 1111111111 + 2222222222 + 3333333333 + 4444444444 + 55 { x + 1 }", -2 },
        { "fn g(x: u64) -> u64 { x == 1 }", -2 },
        { "fn g(x: u64) -> u64 { x + (2: u32) }", -2 },         /* mixed width constant */
        { "fn g(x: u64) -> u64 { (2: u32) * x }", -2 },
        { "fn g(x: u64) -> u64 { (x: u32) + 1 }", -2 },
        { "transform + 1", -2 },                                /* program used as a value */
        { "fn g(x: u64) -> u64 x + 1", -1 },
        { "fn g(x: u64) { x + 1 }", -1 },
        { NULL, 0 }
    };
    for (int i = 0; bad[i].line; i++) {
        size_t np = s.program_count, nb = s.bindings.count;
        uint16_t no = s.graph->object_count;
        int rc = eval(&s, bad[i].line, &r2, err);
        CHECK(rc == bad[i].rc, "'%s' -> %d (want %d): %s", bad[i].line, rc, bad[i].rc, err);
        CHECK(s.program_count == np && s.bindings.count == nb && s.graph->object_count == no,
              "'%s' leaves the session unchanged", bad[i].line);
    }
    /* Program table full -> -3. */
    for (int i = 0; s.program_count < VISOR_MAX_PROGRAMS && i < 64; i++) {
        char fl[96];
        snprintf(fl, sizeof fl, "fn fill%d(x: u64) -> u64 { x + %d }", i, i);
        if (eval(&s, fl, &r2, err) != 0) break;
    }
    CHECK(s.program_count == VISOR_MAX_PROGRAMS, "program table filled");
    CHECK(eval(&s, "fn fill0(x: u64) -> u64 { x + 0 }", &r2, err) == 0 && r2.program_index >= 0,
          "re-entering an existing program still works when the table is full: %s", err);
    CHECK(eval(&s, "fn more(x: u64) -> u64 { x + 1 }", &r2, err) == -3, "program table full -> -3: %s", err);
    visor_session_destroy(&s);
}

int main(int argc, char **argv) {
    const char *golden = DEFAULT_GOLDEN;
    if (argc >= 2 && strcmp(argv[1], "--regen") == 0) {
        return golden_regen(argc >= 3 ? argv[2] : DEFAULT_GOLDEN);
    }
    if (argc >= 2) golden = argv[1];

    test_literal_spellings();
    test_width_changes_id();
    test_whitespace_comments_and_names();
    test_determinism();
    test_fail_closed();
    test_depth_and_capacity();
    test_programs();
    test_golden(golden);

    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
