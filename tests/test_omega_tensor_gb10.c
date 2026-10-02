/*
 * M20 OMEGA_TENSOR GB10 parity gate (docs/tensor/M20_OMEGA_TENSOR.md).
 *
 *   test_omega_tensor_gb10            host self checks only; opens no device
 *   test_omega_tensor_gb10 --chip     host self checks, then the parity gate
 *                                     and the mutant sweep on the GB10. Needs
 *                                     GB10_CHIP_RUN=1 in the environment; run
 *                                     it through tests/run_tensor_chip.sh
 *                                     (lock, pins, receipt), never directly.
 *
 * Parity: every case runs through the CPU table and the GB10 table from the
 * same seeded inputs; both must return OK and give the same value id
 * (omega_tensor_value_id: dtype + shape + values, any NaN = canonical NaN,
 * -0 != +0). Cases: general matmul (1x1x1, 129x3x257, odd K across the
 * 32-lane tile edges, broadcast batches, a transposed view operand, special
 * values), reductions with n across warp/tile edges (31, 32, 33, 1023, 1024,
 * 1025, 32769) for SUM/MAX/MIN/MEAN, multi-row reductions along both axes,
 * elementwise binary/unary/fma/cast over a grid of specials (NaN, -0, inf,
 * subnormals) plus random bit patterns, broadcast elementwise and one call
 * longer than a launch chunk. The six CMP_* mask compare ops (CR-3) are NOT
 * parity cases: the GB10 table has no compare entry, so a host self check
 * requires it to refuse each with OMEGA_TENSOR_ERR_REALIZATION (nothing made,
 * no device call), with a counterexample table that does set compare.
 *
 * Mutants (in-process realization tables over a base table: the CPU table in
 * host mode, the GB10 table in chip mode): WRONG_REDUCE_ORDER (rows reduced
 * reversed), FFMA_MATMUL (matmul products pairwise contracted with FFMA),
 * SWAPPED_OPERAND (a and b exchanged). Each must give a different value id on
 * at least one case where both sides returned OK; a mutant that is not caught
 * fails the test. No libm, no CUDA.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_numeric.h"
#include "omega_numeric_reduce.h"
#include "omega_tensor.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- inputs ---------------------------------------------------------------- */
static uint64_t g_rng;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

static const uint32_t SPECIALS[] = {
    0x00000000U, 0x80000000U, 0x7f800000U, 0xff800000U, 0x7fc00000U, 0x7fa00001U, 0xffc00123U,
    0x00000001U, 0x80000001U, 0x007fffffU, 0x00800000U, 0x7f7fffffU, 0xff7fffffU, 0x3f800000U,
    0xbf800000U, 0x3f000000U, 0x4b800000U, 0x33800000U, 0x3effffffU, 0x7f000000U,
};
#define NSPECIAL (sizeof(SPECIALS) / sizeof(SPECIALS[0]))

/* any FP32: 1/4 specials, 1/4 small-range normals, 1/2 raw bit patterns */
static float rand_f32(void) {
    uint64_t r = rnd();
    switch (r & 3u) {
    case 0: return omega_bits_to_float(SPECIALS[(r >> 8) % NSPECIAL]);
    case 1: return omega_bits_to_float((uint32_t)((r >> 32) & 0x807fffffU) | ((uint32_t)(120 + ((r >> 8) % 16)) << 23));
    default: return omega_bits_to_float((uint32_t)(r >> 16));
    }
}
/* finite values over a moderate exponent range (sums keep their bits) */
static float rand_mod(void) {
    uint64_t r = rnd();
    return omega_bits_to_float((uint32_t)((r >> 32) & 0x807fffffU) | ((uint32_t)(110 + ((r >> 8) % 30)) << 23));
}

/* ---- cases ----------------------------------------------------------------- */
typedef enum { K_MM, K_MM_T, K_RED, K_BIN, K_UN, K_FMA, K_CAST, K_BCAST_ADD, K_BCAST_FMA, K_BIG_ADD } Kind;
enum { MUT_ORDER = 1u, MUT_FFMA = 2u, MUT_SWAP = 4u };

typedef struct {
    char     name[64];
    Kind     kind;
    int      op;        /* reduce / binary op, cast kind                     */
    int      dist;      /* 0 rand_mod, 1 rand_f32                            */
    uint32_t ra, rb, axis;
    uint64_t sa[4], sb[4];
    unsigned mut;       /* mutants that run this case                        */
} Case;

#define MAX_CASES 160
static Case g_cases[MAX_CASES];
static size_t g_ncases;

static Case *add_case(const char *name, Kind k, unsigned mut) {
    if (g_ncases >= MAX_CASES) { printf("FAIL too many cases\n"); exit(2); }
    Case *c = &g_cases[g_ncases++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->kind = k;
    c->mut = mut;
    return c;
}
static void mm_case(const char *name, uint32_t ra, const uint64_t *sa, uint32_t rb, const uint64_t *sb,
                    int dist, unsigned mut) {
    Case *c = add_case(name, K_MM, mut);
    c->ra = ra; c->rb = rb; c->dist = dist;
    memcpy(c->sa, sa, ra * sizeof(uint64_t));
    memcpy(c->sb, sb, rb * sizeof(uint64_t));
}

static const char *RED_NAME[OMEGA_TR_COUNT] = { "SUM", "MAX", "MIN", "MEAN" };
/* Sized to the 20 parity binary ops (everything before OMEGA_TB_CMP_EQ). The CMP_*
 * ops are not parity cases (the GB10 table has no compare entry), so no name
 * is ever looked up for them and no entry stays NULL. */
static const char *BIN_NAME[OMEGA_TB_CMP_EQ] = {
    "ADD", "SUB", "MUL", "DIV", "MIN", "MAX", "SEL_GE", "SEL_LT", "SEL_LE", "SEL_GT", "SEL_EQ", "SEL_NE",
    "SEL_NUM", "SEL_NAN", "SEL_LTU", "SEL_LEU", "SEL_GTU", "SEL_GEU", "SEL_EQU", "SEL_NEU",
};

static void build_cases(void) {
    char nm[64];
    /* general matmul */
    mm_case("mm_1x1x1", 2, (uint64_t[]){ 1, 1 }, 2, (uint64_t[]){ 1, 1 }, 0, 0);
    mm_case("mm_129x3x257", 2, (uint64_t[]){ 129, 3 }, 2, (uint64_t[]){ 3, 257 }, 0, 0);
    static const uint64_t KS[] = { 31, 32, 33, 63, 64, 65, 1023, 1024, 1025 };
    for (size_t i = 0; i < sizeof(KS) / sizeof(KS[0]); i++) {
        snprintf(nm, sizeof(nm), "mm_2x%llux3", (unsigned long long)KS[i]);
        unsigned mut = (KS[i] == 33 || KS[i] == 65) ? (MUT_ORDER | MUT_FFMA) : 0;
        mm_case(nm, 2, (uint64_t[]){ 2, KS[i] }, 2, (uint64_t[]){ KS[i], 3 }, 0, mut);
    }
    mm_case("mm_bcast_3x1x4x5_2x5x6", 4, (uint64_t[]){ 3, 1, 4, 5 }, 3, (uint64_t[]){ 2, 5, 6 }, 0, MUT_FFMA);
    mm_case("mm_bcast_2x3x33_33x4", 3, (uint64_t[]){ 2, 3, 33 }, 2, (uint64_t[]){ 33, 4 }, 0, MUT_FFMA);
    mm_case("mm_specials_3x33x2", 2, (uint64_t[]){ 3, 33 }, 2, (uint64_t[]){ 33, 2 }, 1, 0);
    Case *t = add_case("mm_viewT_5x7x3", K_MM_T, 0);
    t->ra = 2; t->sa[0] = 7; t->sa[1] = 5; t->rb = 2; t->sb[0] = 7; t->sb[1] = 3;
    /* reductions with n across warp/tile edges */
    static const uint64_t NS[] = { 31, 32, 33, 1023, 1024, 1025, 32769 };
    for (int op = 0; op < OMEGA_TR_COUNT; op++) {
        for (size_t i = 0; i < sizeof(NS) / sizeof(NS[0]); i++) {
            snprintf(nm, sizeof(nm), "red_%s_n%llu", RED_NAME[op], (unsigned long long)NS[i]);
            unsigned mut = (op == OMEGA_TR_SUM && (NS[i] == 33 || NS[i] == 1025)) ? MUT_ORDER : 0;
            Case *c = add_case(nm, K_RED, mut);
            c->op = op; c->ra = 1; c->sa[0] = NS[i];
        }
        for (int s = 0; s < 2; s++) {
            snprintf(nm, sizeof(nm), "red_%s_specials_n%d", RED_NAME[op], s ? 1025 : 33);
            Case *c = add_case(nm, K_RED, 0);
            c->op = op; c->dist = 1; c->ra = 1; c->sa[0] = s ? 1025 : 33;
        }
        snprintf(nm, sizeof(nm), "red_%s_rows_3x1025_axis1", RED_NAME[op]);
        Case *c = add_case(nm, K_RED, 0);
        c->op = op; c->ra = 2; c->sa[0] = 3; c->sa[1] = 1025; c->axis = 1;
        snprintf(nm, sizeof(nm), "red_%s_cols_1025x3_axis0", RED_NAME[op]);
        c = add_case(nm, K_RED, 0);
        c->op = op; c->ra = 2; c->sa[0] = 1025; c->sa[1] = 3; c->axis = 0;
    }
    /* elementwise */
    /* ops before OMEGA_TB_CMP_EQ only: the 20 cases of the original gate. The
     * CMP_* ops are refused by the GB10 table, checked in host_checks. */
    for (int op = 0; op < OMEGA_TB_CMP_EQ; op++) {
        snprintf(nm, sizeof(nm), "bin_%s", BIN_NAME[op]);
        unsigned mut = (op == OMEGA_TB_SUB || op == OMEGA_TB_DIV || op == OMEGA_TB_SEL_LT) ? MUT_SWAP : 0;
        Case *c = add_case(nm, K_BIN, mut);
        c->op = op;
    }
    add_case("un_SQRT", K_UN, 0);
    add_case("fma_FFMA_V", K_FMA, 0);
    static const char *CAST_NAME[4] = { "cast_F32_TO_F16", "cast_F32_TO_BF16", "cast_F16_TO_F32", "cast_BF16_TO_F32" };
    for (int k = 0; k < 4; k++) add_case(CAST_NAME[k], K_CAST, 0)->op = k;
    add_case("bcast_add_4x1x3_5x1", K_BCAST_ADD, MUT_SWAP);
    add_case("bcast_fma_2x1_1x3_3", K_BCAST_FMA, 0);
    add_case("big_add_70001", K_BIG_ADD, 0);
}

/* specials grid (a, b over all pairs) then random bit patterns */
#define ELEM_N (NSPECIAL * NSPECIAL + 600u)
static void elem_inputs(float *a, float *b, float *c) {
    size_t i = 0;
    for (size_t p = 0; p < NSPECIAL; p++)
        for (size_t q = 0; q < NSPECIAL; q++, i++) {
            a[i] = omega_bits_to_float(SPECIALS[p]);
            b[i] = omega_bits_to_float(SPECIALS[q]);
            if (c) c[i] = omega_bits_to_float(SPECIALS[(p + q) % NSPECIAL]);
        }
    for (; i < ELEM_N; i++) { a[i] = rand_f32(); b[i] = rand_f32(); if (c) c[i] = rand_f32(); }
}

static uint64_t nelem(uint32_t r, const uint64_t *s) { uint64_t n = 1; for (uint32_t i = 0; i < r; i++) n *= s[i]; return n; }

static int mk(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, int dist, OmegaTensor *out) {
    uint64_t n = nelem(rank, shape);
    float *d = malloc((size_t)n * sizeof(float));
    if (!d) return OMEGA_TENSOR_ERR_CAPACITY;
    for (uint64_t i = 0; i < n; i++) d[i] = dist ? rand_f32() : rand_mod();
    int rc = omega_tensor_from_f32(ctx, rank, shape, d, out);
    free(d);
    return rc;
}

static int run_body(OmegaTensorCtx *ctx, const Case *c, OmegaTensor *out) {
    OmegaTensor a = { 0, 0 }, b = { 0, 0 }, x = { 0, 0 };
    int rc;
    switch (c->kind) {
    case K_MM:
        rc = mk(ctx, c->ra, c->sa, c->dist, &a);
        if (!rc) rc = mk(ctx, c->rb, c->sb, c->dist, &b);
        return rc ? rc : omega_tensor_matmul(ctx, a, b, out);
    case K_MM_T:
        rc = mk(ctx, c->ra, c->sa, 0, &x);
        if (!rc) rc = omega_tensor_transpose(ctx, x, &a);
        if (!rc) rc = mk(ctx, c->rb, c->sb, 0, &b);
        return rc ? rc : omega_tensor_matmul(ctx, a, b, out);
    case K_RED:
        rc = mk(ctx, c->ra, c->sa, c->dist, &a);
        return rc ? rc : omega_tensor_reduce(ctx, (OmegaTensorReduceOp)c->op, a, c->axis, false, out);
    case K_BIN: case K_UN: case K_FMA: {
        float *va = malloc(ELEM_N * sizeof(float)), *vb = malloc(ELEM_N * sizeof(float)),
              *vc = malloc(ELEM_N * sizeof(float));
        OmegaTensor tc = { 0, 0 };
        uint64_t s = ELEM_N;
        rc = (va && vb && vc) ? OMEGA_TENSOR_OK : OMEGA_TENSOR_ERR_CAPACITY;
        if (!rc) elem_inputs(va, vb, vc);
        if (!rc) rc = omega_tensor_from_f32(ctx, 1, &s, va, &a);
        if (!rc) rc = omega_tensor_from_f32(ctx, 1, &s, vb, &b);
        if (!rc) rc = omega_tensor_from_f32(ctx, 1, &s, vc, &tc);
        free(va); free(vb); free(vc);
        if (rc) return rc;
        if (c->kind == K_BIN) return omega_tensor_binary(ctx, (OmegaTensorBinaryOp)c->op, a, b, out);
        if (c->kind == K_UN) return omega_tensor_unary(ctx, OMEGA_TU_SQRT, a, out);
        return omega_tensor_fma(ctx, a, b, tc, out);
    }
    case K_CAST: {
        uint64_t s = ELEM_N;
        if (c->op < 2) {
            float *va = malloc(ELEM_N * sizeof(float)), *vb = malloc(ELEM_N * sizeof(float));
            rc = (va && vb) ? OMEGA_TENSOR_OK : OMEGA_TENSOR_ERR_CAPACITY;
            if (!rc) elem_inputs(va, vb, NULL);
            if (!rc) rc = omega_tensor_from_f32(ctx, 1, &s, va, &a);
            free(va); free(vb);
            return rc ? rc : omega_tensor_cast(ctx, a, c->op == 0 ? OMEGA_DT_F16 : OMEGA_DT_BF16, out);
        }
        static const uint16_t SP16[] = { 0x0000, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0x0001, 0x8001,
                                         0x03ff, 0x0400, 0x7bff, 0x3c00, 0x7f80, 0xff80, 0x7fc0, 0x7f81,
                                         0x0080, 0x007f };
        uint16_t *h = malloc(ELEM_N * sizeof(uint16_t));
        if (!h) return OMEGA_TENSOR_ERR_CAPACITY;
        for (size_t i = 0; i < ELEM_N; i++)
            h[i] = i < sizeof(SP16) / sizeof(SP16[0]) ? SP16[i] : (uint16_t)(rnd() >> 24);
        OmegaDType dt = c->op == 2 ? OMEGA_DT_F16 : OMEGA_DT_BF16;
        rc = omega_tensor_from_data(ctx, dt, 1, &s, h, &a);
        free(h);
        return rc ? rc : omega_tensor_cast(ctx, a, OMEGA_DT_F32, out);
    }
    case K_BCAST_ADD:
        rc = mk(ctx, 3, (uint64_t[]){ 4, 1, 3 }, 1, &a);
        if (!rc) rc = mk(ctx, 2, (uint64_t[]){ 5, 1 }, 1, &b);
        return rc ? rc : omega_tensor_binary(ctx, OMEGA_TB_SUB, a, b, out);
    case K_BCAST_FMA: {
        OmegaTensor tc = { 0, 0 };
        rc = mk(ctx, 2, (uint64_t[]){ 2, 1 }, 1, &a);
        if (!rc) rc = mk(ctx, 2, (uint64_t[]){ 1, 3 }, 1, &b);
        if (!rc) rc = mk(ctx, 1, (uint64_t[]){ 3 }, 1, &tc);
        return rc ? rc : omega_tensor_fma(ctx, a, b, tc, out);
    }
    case K_BIG_ADD:
        rc = mk(ctx, 1, (uint64_t[]){ 70001 }, 1, &a);
        if (!rc) rc = mk(ctx, 1, (uint64_t[]){ 70001 }, 1, &b);
        return rc ? rc : omega_tensor_binary(ctx, OMEGA_TB_ADD, a, b, out);
    }
    return OMEGA_TENSOR_ERR_BAD_ARGS;
}

static uint64_t seed_of(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x100000001b3ULL; }
    return h ? h : 1;
}

/* Runs one case through real; status and value id (zeroed unless OK).
 * *e1 gets the underlying E1 code of the last numeric failure (0 if none),
 * read before the context is destroyed. Diagnostics only. */
typedef struct { uint8_t *bytes; size_t n, esz; } RawOut;   /* output elements, diagnostics only */

static size_t dtype_size(OmegaDType d) { return d == OMEGA_DT_F32 ? 4u : 2u; }

static int run_case(const OmegaTensorRealization *real, const Case *c, uint8_t id[32], int *e1, RawOut *raw) {
    OmegaTensorCtx *ctx;
    memset(id, 0, 32);
    *e1 = 0;
    if (raw) { raw->bytes = NULL; raw->n = 0; raw->esz = 0; }
    int rc = omega_tensor_ctx_create(64, real, &ctx);
    if (rc) return rc;
    g_rng = seed_of(c->name);
    OmegaTensor out = { 0, 0 };
    rc = run_body(ctx, c, &out);
    if (!rc) rc = omega_tensor_value_id(ctx, out, id);
    if (!rc && raw) {   /* copy the output elements out; a failure here only loses the diagnostics */
        OmegaTensorInfo inf;
        if (omega_tensor_info(ctx, out, &inf) == OMEGA_TENSOR_OK && inf.elements > 0) {
            size_t esz = dtype_size(inf.dtype), bytes = (size_t)inf.elements * esz;
            uint8_t *b = malloc(bytes);
            if (b && omega_tensor_read(ctx, out, b, bytes) == OMEGA_TENSOR_OK) {
                raw->bytes = b; raw->n = (size_t)inf.elements; raw->esz = esz;
            } else {
                free(b);
            }
        }
    }
    *e1 = omega_tensor_last_numeric_error(ctx);
    omega_tensor_ctx_destroy(ctx);
    return rc;
}

static void hex8(const uint8_t id[32], char s[17]) {
    for (int i = 0; i < 8; i++) snprintf(s + 2 * i, 3, "%02x", id[i]);
}

/* ---- DIFFER diagnostics (print only; no check depends on them) -------------
 * The E1 executors pre-fill every output buffer with the byte 0x55 before
 * submission (src/omega_numeric_gb10.c:129, src/omega_numeric_reduce_gb10.c:341,
 * src/omega_numeric_divsqrt_gb10.c:1077). An output element still holding that
 * pattern was most likely read before the chip wrote it. */
static uint32_t elem_bits(const RawOut *o, size_t i) {
    if (o->esz == 4) { uint32_t v; memcpy(&v, o->bytes + i * 4, 4); return v; }
    uint16_t h; memcpy(&h, o->bytes + i * 2, 2); return h;
}

/* TENSOR_GB10_FILL: elements of the output still holding the pre-fill pattern.
 * Printed for every DIFFER case, and for any other case where the count is > 0. */
static void fill_line(const char *name, const char *tag, const RawOut *o, bool always) {
    uint32_t pat = o->esz == 4 ? 0x55555555u : 0x5555u;
    size_t n = 0;
    for (size_t i = 0; i < o->n; i++) n += elem_bits(o, i) == pat;
    if (always || n > 0)
        printf("TENSOR_GB10_FILL %s count=%zu of %zu pattern=0x%0*x tag=%s\n", name, n, o->n,
               (int)(o->esz * 2), (unsigned)pat, tag);
}

static void diff_detail(const char *name, const char *tag, const RawOut *a, const RawOut *b) {
    if (!a->bytes || !b->bytes || a->n != b->n || a->esz != b->esz) {
        printf("TENSOR_GB10_FIRST_DIFF %s unavailable (cpu n=%zu esz=%zu, gb10 n=%zu esz=%zu) tag=%s\n", name,
               a->n, a->esz, b->n, b->esz, tag);
        if (b->bytes) fill_line(name, tag, b, true);
        return;
    }
    size_t first = a->n, last = 0, count = 0;
    for (size_t i = 0; i < a->n; i++)
        if (elem_bits(a, i) != elem_bits(b, i)) {
            if (first == a->n) first = i;
            last = i;
            count++;
        }
    if (count == 0) {   /* ids differ but bits agree: NaN payloads, which the value id canonicalizes */
        printf("TENSOR_GB10_FIRST_DIFF %s none: 0 differing elements of %zu tag=%s\n", name, a->n, tag);
    } else {
        int w = (int)(a->esz * 2);
        printf("TENSOR_GB10_FIRST_DIFF %s idx=%zu of %zu cpu_bits=0x%0*x gb10_bits=0x%0*x differing=%zu last_idx=%zu tag=%s\n",
               name, first, a->n, w, (unsigned)elem_bits(a, first), w, (unsigned)elem_bits(b, first), count, last, tag);
    }
    fill_line(name, tag, b, true);
}

typedef struct { size_t cases, both_ok, differ, not_ok; } Cmp;

/* Reference CPU vs test table over the cases selected by mask (0 = all). */
static Cmp compare(const OmegaTensorRealization *test, unsigned mask, const char *tag, bool verbose) {
    Cmp r = { 0, 0, 0, 0 };
    for (size_t i = 0; i < g_ncases; i++) {
        const Case *c = &g_cases[i];
        if (mask && !(c->mut & mask)) continue;
        uint8_t ia[32], ib[32];
        int ea, eb;
        RawOut oa, ob;
        int ra = run_case(omega_tensor_cpu_realization(), c, ia, &ea, &oa);
        fprintf(stderr, "GB10_CASE_BEGIN %s %s\n", tag, c->name);   /* ties stderr call lines to the case */
        int rb = run_case(test, c, ib, &eb, &ob);
        r.cases++;
        bool ok = ra == OMEGA_TENSOR_OK && rb == OMEGA_TENSOR_OK;
        bool eq = ok && memcmp(ia, ib, 32) == 0;
        if (ok) r.both_ok++; else r.not_ok++;
        if (ok && !eq) r.differ++;
        if (verbose || !eq) {
            char ha[17], hb[17];
            hex8(ia, ha); hex8(ib, hb);
            printf("%s_CASE %s cpu=%s rc=%d test=%s rc=%d %s", tag, c->name, ha, ra, hb, rb,
                   eq ? "EQUAL" : ok ? "DIFFER" : "NOT_OK");
            if (!ok) printf(" cpu_e1=%d test_e1=%d", ea, eb);   /* diagnostics only */
            printf("\n");
        }
        if (ok && !eq) diff_detail(c->name, tag, &oa, &ob);
        else if (ob.bytes) fill_line(c->name, tag, &ob, false);
        free(oa.bytes);
        free(ob.bytes);
    }
    return r;
}

/* ---- mutant realizations over a base table ---------------------------------- */
static const OmegaTensorRealization *g_base;

static int base_rows(OmegaTensorReduceOp op, const float *x, size_t rows, size_t n, float *out) {
    if (g_base->reduce_rows) return g_base->reduce_rows(op, x, rows, n, out);
    for (size_t r = 0; r < rows; r++) {
        int rc = g_base->reduce(op, x + r * n, n, &out[r]);
        if (rc) return rc;
    }
    return 0;
}
static int pass_elem(OmegaNumericOp op, const float *a, const float *b, const float *c, float *out, size_t n) {
    return g_base->elementwise(op, a, b, c, out, n);
}
static int pass_reduce(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    return base_rows(op, x, 1, n, out);
}

/* WRONG_REDUCE_ORDER: every row reduced back to front. */
static int m_order_rows(OmegaTensorReduceOp op, const float *x, size_t rows, size_t n, float *out) {
    float *rev = malloc(rows * n * sizeof(float));
    if (!rev) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    for (size_t r = 0; r < rows; r++)
        for (size_t i = 0; i < n; i++) rev[r * n + i] = x[r * n + n - 1 - i];
    int rc = base_rows(op, rev, rows, n, out);
    free(rev);
    return rc;
}
static int m_order_reduce(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    return m_order_rows(op, x, 1, n, out);
}

/* FFMA_MATMUL: remembers the operands of the last FMUL; a SUM over exactly
 * those products is replaced by r[k] = FFMA(a[2k], b[2k], a[2k+1]*b[2k+1])
 * (contracted, one rounding less) and the tree over r. */
static float *st_a, *st_b;
static size_t st_n;
static int m_ffma_elem(OmegaNumericOp op, const float *a, const float *b, const float *c, float *out, size_t n) {
    st_n = 0;
    if (op == OMEGA_NOP_FMUL && b) {
        float *na = realloc(st_a, n * sizeof(float)), *nb = realloc(st_b, n * sizeof(float));
        if (na) st_a = na;
        if (nb) st_b = nb;
        if (na && nb) { memcpy(st_a, a, n * sizeof(float)); memcpy(st_b, b, n * sizeof(float)); st_n = n; }
    }
    return g_base->elementwise(op, a, b, c, out, n);
}
static int m_ffma_rows(OmegaTensorReduceOp op, const float *x, size_t rows, size_t n, float *out) {
    if (op != OMEGA_TR_SUM || st_n != rows * n || n < 2) { st_n = 0; return base_rows(op, x, rows, n, out); }
    st_n = 0;
    size_t h = n / 2, m = (n + 1) / 2;
    float *ae = malloc(rows * h * sizeof(float)), *be = malloc(rows * h * sizeof(float));
    float *ao = malloc(rows * h * sizeof(float)), *bo = malloc(rows * h * sizeof(float));
    float *q = malloc(rows * h * sizeof(float)), *f = malloc(rows * h * sizeof(float));
    float *rr = malloc(rows * m * sizeof(float));
    int rc = (ae && be && ao && bo && q && f && rr) ? 0 : OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!rc) {
        for (size_t r = 0; r < rows; r++)
            for (size_t k = 0; k < h; k++) {
                ae[r * h + k] = st_a[r * n + 2 * k];     be[r * h + k] = st_b[r * n + 2 * k];
                ao[r * h + k] = st_a[r * n + 2 * k + 1]; bo[r * h + k] = st_b[r * n + 2 * k + 1];
            }
        rc = g_base->elementwise(OMEGA_NOP_FMUL, ao, bo, NULL, q, rows * h);
        if (!rc) rc = g_base->elementwise(OMEGA_NOP_FFMA_V, ae, be, q, f, rows * h);
    }
    if (!rc) {
        for (size_t r = 0; r < rows; r++) {
            for (size_t k = 0; k < h; k++) rr[r * m + k] = f[r * h + k];
            if (m > h) rr[r * m + h] = x[r * n + n - 1];
        }
        rc = base_rows(OMEGA_TR_SUM, rr, rows, m, out);
    }
    free(ae); free(be); free(ao); free(bo); free(q); free(f); free(rr);
    return rc;
}
static int m_ffma_reduce(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    return m_ffma_rows(op, x, 1, n, out);
}

/* SWAPPED_OPERAND: a and b exchanged in every two-operand call. */
static int m_swap_elem(OmegaNumericOp op, const float *a, const float *b, const float *c, float *out, size_t n) {
    return b ? g_base->elementwise(op, b, a, c, out, n) : g_base->elementwise(op, a, b, c, out, n);
}

typedef struct { const char *name; unsigned mask; OmegaTensorRealization table; } Mutant;
static const Mutant MUTANTS[] = {
    { "WRONG_REDUCE_ORDER", MUT_ORDER,
      { .name = "MUTANT_WRONG_REDUCE_ORDER", .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
        .elementwise = pass_elem, .reduce = m_order_reduce, .reduce_rows = m_order_rows } },
    { "FFMA_MATMUL", MUT_FFMA,
      { .name = "MUTANT_FFMA_MATMUL", .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
        .elementwise = m_ffma_elem, .reduce = m_ffma_reduce, .reduce_rows = m_ffma_rows } },
    { "SWAPPED_OPERAND", MUT_SWAP,
      { .name = "MUTANT_SWAPPED_OPERAND", .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
        .elementwise = m_swap_elem, .reduce = pass_reduce, .reduce_rows = base_rows } },
};
#define NMUTANTS (sizeof(MUTANTS) / sizeof(MUTANTS[0]))

/* Every mutant over base must differ on >= 1 case where both sides were OK. */
static size_t run_mutants(const OmegaTensorRealization *base, const char *tag) {
    g_base = base;
    size_t caught = 0;
    for (size_t i = 0; i < NMUTANTS; i++) {
        char t[96];
        snprintf(t, sizeof(t), "%s_MUTANT_%s", tag, MUTANTS[i].name);
        Cmp r = compare(&MUTANTS[i].table, MUTANTS[i].mask, t, false);
        bool ok = r.differ > 0;
        caught += ok;
        printf("%s: %s (%zu of %zu cases differ, %zu not OK)\n", t, ok ? "caught" : "NOT CAUGHT",
               r.differ, r.cases, r.not_ok);
        CHECK(ok, "mutant %s not caught over %s", MUTANTS[i].name, base->name);
        CHECK(r.not_ok == 0, "mutant %s: %zu cases did not run OK on both sides", MUTANTS[i].name, r.not_ok);
    }
    free(st_a); free(st_b); st_a = st_b = NULL; st_n = 0;
    return caught;
}

/* ---- host self checks (no device) ------------------------------------------- */
/* Ops the semantic layer submits (omega_tensor.c), listed independently. */
static const OmegaNumericOp USED_OPS[] = {
    OMEGA_NOP_SQRT, OMEGA_NOP_FFMA_V, OMEGA_NOP_FMUL, OMEGA_NOP_FADD, OMEGA_NOP_FSUB, OMEGA_NOP_DIV,
    OMEGA_NOP_FMNMX_MIN, OMEGA_NOP_FMNMX_MAX, OMEGA_NOP_FSETP_SEL, OMEGA_NOP_FSETP_LT_SEL,
    OMEGA_NOP_FSETP_LE_SEL, OMEGA_NOP_FSETP_GT_SEL, OMEGA_NOP_FSETP_EQ_SEL, OMEGA_NOP_FSETP_NE_SEL,
    OMEGA_NOP_FSETP_NUM_SEL, OMEGA_NOP_FSETP_NAN_SEL, OMEGA_NOP_FSETP_LTU_SEL, OMEGA_NOP_FSETP_LEU_SEL,
    OMEGA_NOP_FSETP_GTU_SEL, OMEGA_NOP_FSETP_GEU_SEL, OMEGA_NOP_FSETP_EQU_SEL, OMEGA_NOP_FSETP_NEU_SEL,
    OMEGA_NOP_F32_TO_F16, OMEGA_NOP_F32_TO_BF16, OMEGA_NOP_F16_TO_F32, OMEGA_NOP_BF16_TO_F32,
};
#define NUSED (sizeof(USED_OPS) / sizeof(USED_OPS[0]))

static bool is_used(OmegaNumericOp op) {
    for (size_t i = 0; i < NUSED; i++) if (USED_OPS[i] == op) return true;
    return false;
}

/* The map is right iff every used op names its own encoded E1 op and passes
 * the pre-submission check, and every other op is unmapped. Returns the
 * number of problems. */
static int check_map(const char *(*name_of)(OmegaNumericOp), bool verbose) {
    static float a[32], b[32], c[32], o[32];
    for (int i = 0; i < 32; i++) { a[i] = 1.0f; b[i] = 2.0f; c[i] = 0.5f; }
    int bad = 0;
    for (int op = 0; op < OMEGA_NOP_COUNT; op++) {
        const char *nm = name_of((OmegaNumericOp)op);
        if (!is_used((OmegaNumericOp)op)) {
            if (nm) { bad++; if (verbose) printf("  map: op %d not used by the tensor layer but mapped to %s\n", op, nm); }
            continue;
        }
        const OmegaNumericOpInfo *info = nm ? omega_numeric_op_find(nm) : NULL;
        if (!info || info->op != (OmegaNumericOp)op || !info->gb10_encoded) {
            bad++;
            if (verbose) printf("  map: used op %d -> %s is not its own GB10-encoded op\n", op, nm ? nm : "(none)");
            continue;
        }
        char err[256];
        int rc = omega_numeric_submit_check(nm, a, b, c, o, 32, err, sizeof(err));
        if (rc != OMEGA_NUMERIC_OK) { bad++; if (verbose) printf("  map: %s refused: %s\n", nm, err); }
    }
    return bad;
}
static const char *mut_map_wrong_op(OmegaNumericOp op) {
    return op == OMEGA_NOP_FSUB ? "FADD" : omega_tensor_gb10_op_name(op);
}
static const char *mut_map_exp(OmegaNumericOp op) {
    return op == OMEGA_NOP_EXP ? "EXP" : omega_tensor_gb10_op_name(op);
}
static const char *mut_map_uniform_ffma(OmegaNumericOp op) {
    return op == OMEGA_NOP_FFMA_V ? "FFMA" : omega_tensor_gb10_op_name(op);
}

/* CR-3 mask compare ops. The GB10 table has no compare entry, so the semantic
 * layer must refuse each CMP_* op with OMEGA_TENSOR_ERR_REALIZATION before it
 * gathers anything or calls the table: no tensor or storage slot is made, the
 * output handle stays zero and no numeric error is recorded (so no device was
 * reached). Returns the number of problems. */
static const OmegaTensorBinaryOp CMP_OPS[] = {
    OMEGA_TB_CMP_EQ, OMEGA_TB_CMP_NE, OMEGA_TB_CMP_LT, OMEGA_TB_CMP_LE, OMEGA_TB_CMP_GT, OMEGA_TB_CMP_GE,
};
#define NCMP_OPS (sizeof(CMP_OPS) / sizeof(CMP_OPS[0]))

static int check_cmp_refused(const OmegaTensorRealization *real, bool verbose) {
    static const uint64_t shape[1] = { 4 };
    static const float da[4] = { 1.0f, 2.0f, 3.0f, 4.0f }, db[4] = { 1.0f, 5.0f, 3.0f, 0.0f };
    OmegaTensorCtx *ctx = NULL;
    OmegaTensor a = { 0, 0 }, b = { 0, 0 };
    if (omega_tensor_ctx_create(8, real, &ctx) != OMEGA_TENSOR_OK) {
        if (verbose) printf("  cmp: context create refused\n");
        return 1;
    }
    if (omega_tensor_from_f32(ctx, 1, shape, da, &a) != OMEGA_TENSOR_OK ||
        omega_tensor_from_f32(ctx, 1, shape, db, &b) != OMEGA_TENSOR_OK) {
        if (verbose) printf("  cmp: input tensors not made\n");
        omega_tensor_ctx_destroy(ctx);
        return 1;
    }
    uint32_t t0 = 0, s0 = 0;
    omega_tensor_live_counts(ctx, &t0, &s0);
    int bad = 0;
    for (size_t i = 0; i < NCMP_OPS; i++) {
        OmegaTensor out = { 0, 0 };
        int rc = omega_tensor_binary(ctx, CMP_OPS[i], a, b, &out);
        uint32_t t1 = 0, s1 = 0;
        omega_tensor_live_counts(ctx, &t1, &s1);
        if (rc != OMEGA_TENSOR_ERR_REALIZATION) {
            bad++;
            if (verbose) printf("  cmp: op %d returned %d, expected %d\n", (int)CMP_OPS[i], rc, OMEGA_TENSOR_ERR_REALIZATION);
        }
        if (t1 != t0 || s1 != s0 || out.slot != 0 || out.generation != 0) {
            bad++;
            if (verbose) printf("  cmp: op %d made a tensor or storage slot\n", (int)CMP_OPS[i]);
        }
        if (omega_tensor_last_numeric_error(ctx) != 0) {
            bad++;
            if (verbose) printf("  cmp: op %d reached a numeric call\n", (int)CMP_OPS[i]);
        }
    }
    omega_tensor_ctx_destroy(ctx);
    return bad;
}

/* counterexample table: the GB10 table WITH a compare entry (a stub that fills +0.0) */
static int cmp_stub(OmegaNumericOp sel_op, const float *a, const float *b, float *out, size_t n) {
    (void)sel_op; (void)a; (void)b;
    for (size_t i = 0; i < n; i++) out[i] = 0.0f;
    return OMEGA_NUMERIC_OK;
}

static void host_checks(void) {
    const OmegaTensorRealization *g = omega_tensor_gb10_realization();
    CHECK(g && g->name && g->elementwise && g->reduce && g->reduce_rows && g->reduce_order,
          "GB10 table incomplete");
    CHECK(g && strcmp(g->reduce_order, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) == 0 &&
          strcmp(g->reduce_order, OMEGA_REDUCE_DECLARED_ORDER) == 0, "GB10 table order string");
    OmegaTensorCtx *ctx = NULL;
    CHECK(omega_tensor_ctx_create(8, g, &ctx) == OMEGA_TENSOR_OK, "ctx_create refused the GB10 table");
    omega_tensor_ctx_destroy(ctx);
    /* counterexample: the same table with another order string is refused */
    OmegaTensorRealization wrong = *g;
    wrong.reduce_order = "SEQUENTIAL_LEFT_TO_RIGHT";
    ctx = NULL;
    CHECK(omega_tensor_ctx_create(8, &wrong, &ctx) == OMEGA_TENSOR_ERR_REALIZATION && !ctx,
          "GB10 table with a wrong order string accepted");

    /* op map, and its counterexamples */
    CHECK(check_map(omega_tensor_gb10_op_name, true) == 0, "GB10 op map");
    CHECK(check_map(mut_map_wrong_op, false) > 0, "map check missed FSUB->FADD");
    CHECK(check_map(mut_map_exp, false) > 0, "map check missed EXP (no GB10 kernel)");
    CHECK(check_map(mut_map_uniform_ffma, false) > 0, "map check missed FFMA_V->FFMA (uniform c)");

    /* refusals before any device is opened: ops without a GB10 kernel (or not
     * used by the tensor layer) and sizes past OMEGA_REDUCE_GB10_MAX_N */
    static const OmegaNumericOp REFUSED[] = { OMEGA_NOP_EXP, OMEGA_NOP_LOG, OMEGA_NOP_FFMA, OMEGA_NOP_MUFU_RCP,
                                              OMEGA_NOP_LDS_STS, OMEGA_NOP_SHFL_DOWN, OMEGA_NOP_REDUCE_SUM };
    float a[4] = { 1.0f, 2.0f, 3.0f, 4.0f }, b[4] = { 1.0f, 1.0f, 1.0f, 1.0f }, o[4];
    for (size_t i = 0; i < sizeof(REFUSED) / sizeof(REFUSED[0]); i++) {
        int rc = g->elementwise(REFUSED[i], a, b, b, o, 4);
        CHECK(rc == OMEGA_NUMERIC_ERR_NOT_ENCODED, "GB10 table did not refuse op %d (rc %d)", REFUSED[i], rc);
    }
    CHECK(g->elementwise((OmegaNumericOp)OMEGA_NOP_COUNT, a, b, b, o, 4) == OMEGA_NUMERIC_ERR_NOT_ENCODED,
          "GB10 table did not refuse an out-of-range op");
    float r1;
    CHECK(g->reduce(OMEGA_TR_SUM, NULL, OMEGA_REDUCE_GB10_MAX_N + 1, &r1) == OMEGA_NUMERIC_ERR_OPERANDS,
          "reduce past OMEGA_REDUCE_GB10_MAX_N not refused as OPERANDS");
    CHECK(g->reduce_rows(OMEGA_TR_SUM, NULL, 1, OMEGA_REDUCE_GB10_MAX_N + 1, &r1) == OMEGA_NUMERIC_ERR_OPERANDS,
          "reduce_rows past OMEGA_REDUCE_GB10_MAX_N not refused as OPERANDS");
    CHECK(g->reduce((OmegaTensorReduceOp)OMEGA_TR_COUNT, a, 4, &r1) == OMEGA_NUMERIC_ERR_BAD_ARGS,
          "unknown reduce op not refused");

    /* CR-3 compare: no compare entry, each CMP_* op refused with ERR_REALIZATION,
     * nothing made, no device call; and the counterexample: the same table with
     * a compare entry set must fail this very check */
    CHECK(g->compare == NULL, "GB10 table has a compare entry (CMP_* must be refused)");
    CHECK(check_cmp_refused(g, true) == 0, "GB10 table does not refuse the CMP_* ops cleanly");
    OmegaTensorRealization with_cmp = *g;
    with_cmp.compare = cmp_stub;
    CHECK(check_cmp_refused(&with_cmp, false) > 0, "CMP refusal check missed a table with a compare entry");

    /* the harness itself, on the CPU: CPU vs CPU equal on every case, and
     * every mutant (over the CPU table) caught */
    Cmp self = compare(omega_tensor_cpu_realization(), 0, "HOST_SELF", false);
    CHECK(self.cases == g_ncases && self.both_ok == g_ncases && self.differ == 0,
          "harness CPU vs CPU: %zu cases, %zu OK, %zu differ", self.cases, self.both_ok, self.differ);
    size_t caught = run_mutants(omega_tensor_cpu_realization(), "HOST");
    printf("HOST_HARNESS_MUTANTS: %s (%zu of %zu caught over the CPU table)\n",
           caught == NMUTANTS ? "PASS" : "FAIL", caught, NMUTANTS);
}

int main(int argc, char **argv) {
    bool chip = argc == 2 && strcmp(argv[1], "--chip") == 0;
    if (argc > 1 && !chip) { printf("usage: %s [--chip]\n", argv[0]); return 2; }
    if (!omega_numeric_fpenv_ok()) { printf("FPCR not RNE/no-FTZ: refusing to run\n"); return 2; }
    if (chip) {
        const char *e = getenv("GB10_CHIP_RUN");
        if (!e || strcmp(e, "1") != 0) {
            printf("--chip needs GB10_CHIP_RUN=1 (run through tests/run_tensor_chip.sh)\n");
            return 2;
        }
    }
    build_cases();
    printf("cases: %zu\n", g_ncases);
    host_checks();
    int host_fail = g_fail;
    printf("TENSOR_GB10_HOST: %s (%d checks, %d failed)\n", host_fail ? "FAIL" : "PASS", g_pass + g_fail, host_fail);
    if (!chip) {
        printf("M20 Tensor GB10 host self checks: %s (no device opened)\n", g_fail ? "FAIL" : "PASS");
        return g_fail ? 1 : 0;
    }

    const OmegaTensorRealization *g = omega_tensor_gb10_realization();
    Cmp p = compare(g, 0, "TENSOR_GB10", true);
    bool parity = p.cases == g_ncases && p.both_ok == g_ncases && p.differ == 0;
    CHECK(parity, "GB10 parity: %zu cases, %zu OK on both, %zu differ", p.cases, p.both_ok, p.differ);
    printf("TENSOR_GB10_PARITY: %s (%zu of %zu cases bit-exact value ids)\n", parity ? "PASS" : "FAIL",
           p.both_ok - p.differ, p.cases);
    size_t caught = run_mutants(g, "TENSOR_GB10");
    printf("TENSOR_GB10_MUTANTS: %s (%zu of %zu caught on chip)\n", caught == NMUTANTS ? "PASS" : "FAIL",
           caught, NMUTANTS);
    printf("%d checks, %d failed\n", g_pass + g_fail, g_fail);
    printf("M20 Tensor GB10 Verdict: %s\n", g_fail == 0 && parity && caught == NMUTANTS ? "PASS" : "FAIL");
    return g_fail ? 1 : 0;
}
