/*
 * M20 OMEGA_TENSOR CPU tests (docs/tensor/M20_OMEGA_TENSOR.md).
 *
 * The expected values come from an independent naive reference written in
 * this file straight from the definitions: logical row-major arrays, index
 * arithmetic on shapes only (never on the tensor's strides), the E1 scalar
 * reference functions (omega_ref_*, omega_ieee_div/sqrt, omega_ref_ffma_int),
 * and its own copy of the frozen reduction tree. Every comparison is bit
 * exact (any NaN equals any NaN: NaN payload is not semantic).
 * No GPU, no device, no libm.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_numeric.h"
#include "omega_tensor.h"
#include "omega_tensor_reduce_seam.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

static const uint32_t SPECIALS[] = {
    0x00000000U, 0x80000000U, 0x7f800000U, 0xff800000U, 0x7fc00000U, 0x7fa00001U, 0xffc00123U,
    0x00000001U, 0x80000001U, 0x007fffffU, 0x00800000U, 0x7f7fffffU, 0xff7fffffU, 0x3f800000U,
    0xbf800000U, 0x3f000000U, 0x4b800000U, 0x33800000U, 0x3effffffU, 0x7f000000U,
};
#define NSPECIAL (sizeof(SPECIALS) / sizeof(SPECIALS[0]))

/* Random FP32: 1/4 specials, 1/4 small-range normals, 1/2 raw bit patterns. */
static float rand_f32(void) {
    uint64_t r = rnd();
    switch (r & 3u) {
    case 0: return omega_bits_to_float(SPECIALS[(r >> 8) % NSPECIAL]);
    case 1: return omega_bits_to_float((uint32_t)((r >> 32) & 0x807fffffU) | ((uint32_t)(120 + ((r >> 8) % 16)) << 23));
    default: return omega_bits_to_float((uint32_t)(r >> 16));
    }
}
/* Finite moderate values (for matmul / sums so results are not all NaN). */
static float rand_mod(void) {
    uint64_t r = rnd();
    if ((r & 63u) == 0) return omega_bits_to_float(SPECIALS[(r >> 8) % NSPECIAL]);
    return omega_bits_to_float((uint32_t)((r >> 32) & 0x807fffffU) | ((uint32_t)(110 + ((r >> 8) % 30)) << 23));
}
static void fill(float *x, size_t n, float (*g)(void)) { for (size_t i = 0; i < n; i++) x[i] = g(); }

static bool same(float a, float b) { return omega_numeric_bits_equal(a, b); }
static size_t mismatches(const float *a, const float *b, size_t n) {
    size_t m = 0;
    for (size_t i = 0; i < n; i++) m += !same(a[i], b[i]);
    return m;
}
static uint64_t prod(uint32_t r, const uint64_t *s) { uint64_t n = 1; for (uint32_t i = 0; i < r; i++) n *= s[i]; return n; }

/* ---- independent reference ------------------------------------------------ */

typedef float (*Bin)(float, float);
static int g_selop;
static float r_add(float a, float b) { return omega_ref_fadd(a, b); }
static float r_sub(float a, float b) { return omega_ref_fsub(a, b); }
static float r_mul(float a, float b) { return omega_ref_fmul(a, b); }
static float r_div(float a, float b) { return omega_ieee_div(a, b); }
static float r_min(float a, float b) { return omega_ref_fmin(a, b); }
static float r_max(float a, float b) { return omega_ref_fmax(a, b); }
static float r_sel(float a, float b) { return omega_ref_fsetp_pred(g_selop, a, b) == 1 ? a : b; }

typedef struct { OmegaTensorBinaryOp op; Bin f; int selop; const char *name; } BinCase;
static const BinCase BIN_CASES[] = {
    {OMEGA_TB_ADD, r_add, 0, "ADD"}, {OMEGA_TB_SUB, r_sub, 0, "SUB"}, {OMEGA_TB_MUL, r_mul, 0, "MUL"},
    {OMEGA_TB_DIV, r_div, 0, "DIV"}, {OMEGA_TB_MIN, r_min, 0, "MIN"}, {OMEGA_TB_MAX, r_max, 0, "MAX"},
    {OMEGA_TB_SEL_GE, r_sel, OMEGA_NOP_FSETP_SEL, "SEL_GE"}, {OMEGA_TB_SEL_LT, r_sel, OMEGA_NOP_FSETP_LT_SEL, "SEL_LT"},
    {OMEGA_TB_SEL_LE, r_sel, OMEGA_NOP_FSETP_LE_SEL, "SEL_LE"}, {OMEGA_TB_SEL_GT, r_sel, OMEGA_NOP_FSETP_GT_SEL, "SEL_GT"},
    {OMEGA_TB_SEL_EQ, r_sel, OMEGA_NOP_FSETP_EQ_SEL, "SEL_EQ"}, {OMEGA_TB_SEL_NE, r_sel, OMEGA_NOP_FSETP_NE_SEL, "SEL_NE"},
    {OMEGA_TB_SEL_NUM, r_sel, OMEGA_NOP_FSETP_NUM_SEL, "SEL_NUM"}, {OMEGA_TB_SEL_NAN, r_sel, OMEGA_NOP_FSETP_NAN_SEL, "SEL_NAN"},
    {OMEGA_TB_SEL_LTU, r_sel, OMEGA_NOP_FSETP_LTU_SEL, "SEL_LTU"}, {OMEGA_TB_SEL_LEU, r_sel, OMEGA_NOP_FSETP_LEU_SEL, "SEL_LEU"},
    {OMEGA_TB_SEL_GTU, r_sel, OMEGA_NOP_FSETP_GTU_SEL, "SEL_GTU"}, {OMEGA_TB_SEL_GEU, r_sel, OMEGA_NOP_FSETP_GEU_SEL, "SEL_GEU"},
    {OMEGA_TB_SEL_EQU, r_sel, OMEGA_NOP_FSETP_EQU_SEL, "SEL_EQU"}, {OMEGA_TB_SEL_NEU, r_sel, OMEGA_NOP_FSETP_NEU_SEL, "SEL_NEU"},
};
#define NBIN (sizeof(BIN_CASES) / sizeof(BIN_CASES[0]))

/* Index of logical element `flat` of out shape (ro, so) inside an operand of
 * shape (ra, sa) under numpy broadcasting (dense row-major operand). */
static uint64_t bcast_src(uint64_t flat, uint32_t ro, const uint64_t *so, uint32_t ra, const uint64_t *sa) {
    uint64_t idx[8];
    for (uint32_t d = ro; d-- > 0;) { idx[d] = flat % so[d]; flat /= so[d]; }
    uint64_t src = 0;
    for (uint32_t d = 0; d < ra; d++) {
        uint64_t i = idx[ro - ra + d];
        src = src * sa[d] + (sa[d] == 1 ? 0 : i);
    }
    return src;
}

static float ref_tree(OmegaTensorReduceOp op, const float *x, size_t n) {
    float pad = omega_bits_to_float(op == OMEGA_TR_SUM || op == OMEGA_TR_MEAN ? 0x80000000U : 0x7fc00000U);
    float *lvl = malloc(n * sizeof(float));
    memcpy(lvl, x, n * sizeof(float));
    size_t len = n;
    int levels = 0;
    while (levels == 0 || len > 1) {
        size_t out = 0;
        for (size_t base = 0; base < len; base += 32) {
            float v[32];
            for (int l = 0; l < 32; l++) v[l] = base + (size_t)l < len ? lvl[base + (size_t)l] : pad;
            static const int deltas[5] = {16, 8, 4, 2, 1};
            for (int k = 0; k < 5; k++)
                for (int i = 0; i < deltas[k]; i++) {
                    float a = v[i], b = v[i + deltas[k]];
                    v[i] = op == OMEGA_TR_MAX ? omega_ref_fmax(a, b)
                         : op == OMEGA_TR_MIN ? omega_ref_fmin(a, b) : omega_ref_fadd(a, b);
                }
            lvl[out++] = v[0];
        }
        len = out;
        levels++;
    }
    float r = lvl[0];
    free(lvl);
    if (op == OMEGA_TR_MEAN) r = omega_ieee_div(r, (float)n);
    return r;
}

/* Reference matmul on dense row-major [M,K] x [K,N]. */
static void ref_matmul(const float *a, const float *b, float *o, size_t M, size_t K, size_t N) {
    float *p = malloc(K * sizeof(float));
    for (size_t i = 0; i < M; i++)
        for (size_t j = 0; j < N; j++) {
            for (size_t k = 0; k < K; k++) p[k] = omega_ref_fmul(a[i * K + k], b[k * N + j]);
            o[i * N + j] = ref_tree(OMEGA_TR_SUM, p, K);
        }
    free(p);
}

/* ---- helpers -------------------------------------------------------------- */

static OmegaTensorCtx *g;
static OmegaTensor mk(uint32_t rank, const uint64_t *shape, const float *data) {
    OmegaTensor t;
    int rc = omega_tensor_from_f32(g, rank, shape, data, &t);
    if (rc) { printf("FATAL create rc=%d\n", rc); exit(2); }
    return t;
}
static float *rd(OmegaTensor t) {
    OmegaTensorInfo in;
    if (omega_tensor_info(g, t, &in)) return NULL;
    float *o = malloc((size_t)in.elements * sizeof(float));
    if (omega_tensor_read_f32(g, t, o, (size_t)in.elements)) { free(o); return NULL; }
    return o;
}
static void vid(OmegaTensor t, uint8_t id[32]) { if (omega_tensor_value_id(g, t, id)) memset(id, 0xee, 32); }

/* ---- 1. shape / type errors ------------------------------------------------ */
static void test_shape_type(void) {
    float d[64] = {0};
    uint64_t s9[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1}, s0[2] = {2, 0}, big[2] = {1u << 20, 1u << 11};
    OmegaTensor t;
    CHECK(omega_tensor_from_f32(g, 9, s9, d, &t) == OMEGA_TENSOR_ERR_RANK, "rank 9 refused");
    CHECK(omega_tensor_from_f32(g, 2, s0, d, &t) == OMEGA_TENSOR_ERR_SHAPE, "zero dim refused");
    CHECK(omega_tensor_from_f32(g, 2, big, d, &t) == OMEGA_TENSOR_ERR_CAPACITY, "2^31 elements refused");
    CHECK(omega_tensor_from_data(g, (OmegaDType)9, 1, s9, d, &t) == OMEGA_TENSOR_ERR_DTYPE, "unknown dtype");
    uint64_t s23[2] = {2, 3}, s34[2] = {3, 4}, s4[1] = {4}, s32[2] = {3, 2};
    OmegaTensor a = mk(2, s23, d), b = mk(2, s34, d), c = mk(1, s4, d), e = mk(2, s32, d), o;
    uint16_t h[6] = {0};
    OmegaTensor f16;
    CHECK(omega_tensor_from_data(g, OMEGA_DT_F16, 2, s23, h, &f16) == 0, "f16 create");
    CHECK(omega_tensor_binary(g, OMEGA_TB_ADD, a, f16, &o) == OMEGA_TENSOR_ERR_DTYPE, "f16 arithmetic refused");
    CHECK(omega_tensor_reduce(g, OMEGA_TR_SUM, f16, 0, false, &o) == OMEGA_TENSOR_ERR_DTYPE, "f16 reduce refused");
    CHECK(omega_tensor_matmul(g, f16, e, &o) == OMEGA_TENSOR_ERR_DTYPE, "f16 matmul refused");
    CHECK(omega_tensor_cast(g, f16, OMEGA_DT_BF16, &o) == OMEGA_TENSOR_ERR_DTYPE, "f16->bf16 refused");
    CHECK(omega_tensor_binary(g, OMEGA_TB_ADD, a, b, &o) == OMEGA_TENSOR_ERR_SHAPE, "[2,3]+[3,4] refused");
    CHECK(omega_tensor_matmul(g, a, a, &o) == OMEGA_TENSOR_ERR_SHAPE, "matmul K mismatch");
    CHECK(omega_tensor_matmul(g, c, b, &o) == OMEGA_TENSOR_ERR_RANK, "matmul rank 1 refused");
    CHECK(omega_tensor_reduce(g, OMEGA_TR_SUM, a, 2, false, &o) == OMEGA_TENSOR_ERR_AXIS, "axis 2 of rank 2");
    CHECK(omega_tensor_binary(g, (OmegaTensorBinaryOp)99, a, a, &o) == OMEGA_TENSOR_ERR_BAD_ARGS, "unknown op");
    CHECK(omega_tensor_reduce(g, (OmegaTensorReduceOp)7, a, 0, false, &o) == OMEGA_TENSOR_ERR_BAD_ARGS, "unknown reduce");
    uint32_t bad_perm[2] = {0, 0};
    CHECK(omega_tensor_permute(g, a, bad_perm, &o) == OMEGA_TENSOR_ERR_AXIS, "duplicate perm axis");
    OmegaTensor sc = mk(0, NULL, d);
    CHECK(omega_tensor_transpose(g, sc, &o) == OMEGA_TENSOR_ERR_RANK, "transpose of scalar");
    CHECK(omega_tensor_reduce(g, OMEGA_TR_SUM, sc, 0, false, &o) == OMEGA_TENSOR_ERR_RANK, "reduce of scalar");
    OmegaTensor ts[] = {a, b, c, e, f16, sc};
    for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) omega_tensor_release(g, ts[i]);
    /* bad realization tables */
    OmegaTensorRealization bad = *omega_tensor_cpu_realization();
    OmegaTensorCtx *x;
    bad.reduce_order = "SEQUENTIAL_LEFT_TO_RIGHT";
    CHECK(omega_tensor_ctx_create(4, &bad, &x) == OMEGA_TENSOR_ERR_REALIZATION, "wrong order string refused");
    bad = *omega_tensor_cpu_realization();
    bad.reduce = NULL;
    CHECK(omega_tensor_ctx_create(4, &bad, &x) == OMEGA_TENSOR_ERR_REALIZATION, "missing reduce refused");
}

/* ---- 2. broadcasting -------------------------------------------------------- */
static void bcase(uint32_t ra, const uint64_t *sa, uint32_t rb, const uint64_t *sb, int expect_rc,
                  uint32_t er, const uint64_t *es, const char *name) {
    uint32_t r = 0;
    uint64_t s[8];
    int rc = omega_tensor_broadcast_shape(ra, sa, rb, sb, &r, s);
    bool ok = rc == expect_rc;
    if (ok && rc == 0) ok = r == er && (er == 0 || memcmp(s, es, er * 8) == 0);
    CHECK(ok, "broadcast shape %s rc=%d", name, rc);
}

static void test_broadcast(void) {
    uint64_t s31[] = {3, 1}, s14[] = {1, 4}, s34[] = {3, 4}, s4[] = {4}, s234[] = {2, 3, 4};
    uint64_t s23[] = {2, 3}, s32[] = {3, 2}, s54[] = {5, 4}, s41[] = {4, 1}, s1[] = {1};
    uint64_t s5114[] = {5, 1, 1, 4}, s736[] = {7, 3, 1}, s5734[] = {5, 7, 3, 4};
    bcase(2, s31, 2, s14, 0, 2, s34, "[3,1]x[1,4]");
    bcase(1, s4, 3, s234, 0, 3, s234, "[4]x[2,3,4]");
    bcase(0, NULL, 2, s23, 0, 2, s23, "scalar x [2,3]");
    bcase(1, s1, 1, s4, 0, 1, s4, "[1]x[4]");
    bcase(4, s5114, 3, s736, 0, 4, s5734, "[5,1,1,4]x[7,3,1]");
    bcase(2, s23, 2, s32, OMEGA_TENSOR_ERR_SHAPE, 0, NULL, "[2,3]x[3,2]");
    bcase(2, s54, 2, s41, OMEGA_TENSOR_ERR_SHAPE, 0, NULL, "[5,4]x[4,1]");
    bcase(1, s4, 2, s23, OMEGA_TENSOR_ERR_SHAPE, 0, NULL, "[4]x[2,3]");

    /* value parity of broadcast ops against bcast_src index arithmetic */
    struct { uint32_t ra; uint64_t sa[4]; uint32_t rb; uint64_t sb[4]; } cs[] = {
        {2, {3, 1}, 2, {1, 4}}, {1, {4}, 3, {2, 3, 4}}, {0, {0}, 2, {2, 3}},
        {4, {5, 1, 1, 4}, 3, {7, 3, 1}}, {3, {2, 1, 6}, 3, {2, 5, 1}},
    };
    for (size_t c = 0; c < sizeof(cs) / sizeof(cs[0]); c++) {
        uint64_t na = prod(cs[c].ra, cs[c].sa), nb = prod(cs[c].rb, cs[c].sb);
        float *da = malloc(na * 4), *db = malloc(nb * 4);
        fill(da, na, rand_f32);
        fill(db, nb, rand_f32);
        OmegaTensor A = mk(cs[c].ra, cs[c].sa, da), B = mk(cs[c].rb, cs[c].sb, db);
        uint32_t ro;
        uint64_t so[8];
        omega_tensor_broadcast_shape(cs[c].ra, cs[c].sa, cs[c].rb, cs[c].sb, &ro, so);
        uint64_t no = prod(ro, so);
        float *ex = malloc(no * 4);
        for (size_t k = 0; k < NBIN; k++) {
            g_selop = BIN_CASES[k].selop;
            for (uint64_t i = 0; i < no; i++)
                ex[i] = BIN_CASES[k].f(da[bcast_src(i, ro, so, cs[c].ra, cs[c].sa)],
                                       db[bcast_src(i, ro, so, cs[c].rb, cs[c].sb)]);
            OmegaTensor O;
            int rc = omega_tensor_binary(g, BIN_CASES[k].op, A, B, &O);
            float *got = rc ? NULL : rd(O);
            CHECK(got && mismatches(ex, got, no) == 0, "broadcast case %zu op %s rc=%d", c, BIN_CASES[k].name, rc);
            free(got);
            if (!rc) omega_tensor_release(g, O);
        }
        /* broadcast_to view: zero strides, values */
        OmegaTensor V;
        CHECK(omega_tensor_broadcast_to(g, A, ro, so, &V) == 0, "broadcast_to case %zu", c);
        OmegaTensorInfo vi;
        omega_tensor_info(g, V, &vi);
        bool zs = true;
        for (uint32_t d = 0; d < ro; d++) {
            bool expanded = d < ro - cs[c].ra || cs[c].sa[d - (ro - cs[c].ra)] == 1;
            if (expanded && so[d] > 1 && vi.strides[d] != 0) zs = false;
        }
        CHECK(zs && vi.is_view, "broadcast_to zero strides case %zu", c);
        float *gv = rd(V);
        for (uint64_t i = 0; i < no; i++) ex[i] = da[bcast_src(i, ro, so, cs[c].ra, cs[c].sa)];
        CHECK(gv && mismatches(ex, gv, no) == 0, "broadcast_to values case %zu", c);
        free(gv);
        omega_tensor_release(g, V);
        omega_tensor_release(g, A);
        omega_tensor_release(g, B);
        free(da); free(db); free(ex);
    }
    float d6[6] = {1, 2, 3, 4, 5, 6};
    uint64_t s3[] = {3};
    OmegaTensor A = mk(2, s23, d6), O;
    CHECK(omega_tensor_broadcast_to(g, A, 1, s3, &O) == OMEGA_TENSOR_ERR_SHAPE, "broadcast_to lower rank refused");
    CHECK(omega_tensor_broadcast_to(g, A, 2, s32, &O) == OMEGA_TENSOR_ERR_SHAPE, "broadcast_to [2,3]->[3,2] refused");
    omega_tensor_release(g, A);
}

/* ---- 3. views and strides ------------------------------------------------- */
static void test_views(void) {
    uint64_t s[3] = {4, 5, 6};
    float d[120];
    for (int i = 0; i < 120; i++) d[i] = (float)i;
    OmegaTensor T = mk(3, s, d), P, S, TS, ST, V2, R, C, Z;
    uint8_t before[32], after[32];
    vid(T, before);
    /* permute (2,0,1): out[i][j][k] = T[j][k][i] */
    uint32_t perm[3] = {2, 0, 1};
    CHECK(omega_tensor_permute(g, T, perm, &P) == 0, "permute");
    float *gp = rd(P);
    bool ok = gp != NULL;
    for (int i = 0; ok && i < 6; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 5; k++)
        ok = gp[(i * 4 + j) * 5 + k] == d[(j * 5 + k) * 6 + i];
    CHECK(ok, "permute values");
    free(gp);
    /* slice [1:4:2, 0:5:2, 2:6] then transpose last two */
    uint64_t st[3] = {1, 0, 2}, sp[3] = {4, 5, 6}, sz[3] = {2, 2, 1};
    CHECK(omega_tensor_slice(g, T, st, sp, sz, &S) == 0, "slice");
    OmegaTensorInfo si;
    omega_tensor_info(g, S, &si);
    CHECK(si.shape[0] == 2 && si.shape[1] == 3 && si.shape[2] == 4 && si.offset == 1 * 30 + 2, "slice shape/offset");
    CHECK(omega_tensor_transpose(g, S, &TS) == 0, "transpose of slice");
    float *gts = rd(TS);
    ok = gts != NULL;
    for (int a = 0; ok && a < 2; a++) for (int k = 0; k < 4; k++) for (int j = 0; j < 3; j++)
        ok = gts[(a * 4 + k) * 3 + j] == d[((1 + 2 * a) * 5 + 2 * j) * 6 + 2 + k];
    CHECK(ok, "transpose of slice values");
    free(gts);
    /* slice of transpose == transpose of slice */
    OmegaTensor TT;
    omega_tensor_transpose(g, T, &TT);
    uint64_t st2[3] = {1, 2, 0}, sp2[3] = {4, 6, 5}, sz2[3] = {2, 1, 2};
    CHECK(omega_tensor_slice(g, TT, st2, sp2, sz2, &ST) == 0, "slice of transpose");
    bool eq = false;
    omega_tensor_value_equal(g, ST, TS, &eq);
    CHECK(eq, "slice(transpose) == transpose(slice)");
    /* view of a view of a view */
    uint64_t st3[3] = {1, 1, 1}, sp3[3] = {2, 3, 3};
    CHECK(omega_tensor_slice(g, TS, st3, sp3, NULL, &V2) == 0, "slice of transposed slice");
    float *gv2 = rd(V2);
    ok = gv2 != NULL;
    for (int k = 0; ok && k < 2; k++) for (int j = 0; j < 2; j++)  /* a=1, k+1, j+1 */
        ok = gv2[k * 2 + j] == d[((1 + 2 * 1) * 5 + 2 * (j + 1)) * 6 + 2 + (k + 1)];
    CHECK(ok, "view of view of view values");
    free(gv2);
    OmegaTensorInfo i2;
    omega_tensor_info(g, V2, &i2);
    CHECK(i2.storage.slot == si.storage.slot && i2.storage.generation == si.storage.generation, "views share storage");
    /* reshape: contiguous ok, transposed refused, copy then ok */
    uint64_t rs[2] = {20, 6};
    CHECK(omega_tensor_reshape(g, T, 2, rs, &R) == 0, "reshape contiguous");
    CHECK(omega_tensor_reshape(g, TT, 2, rs, &Z) == OMEGA_TENSOR_ERR_NOT_CONTIGUOUS, "reshape transposed refused");
    uint64_t rbad[2] = {7, 6};
    CHECK(omega_tensor_reshape(g, T, 2, rbad, &Z) == OMEGA_TENSOR_ERR_SHAPE, "reshape count mismatch");
    CHECK(omega_tensor_contiguous(g, TT, &C) == 0, "contiguous copy");
    uint64_t rs2[2] = {4, 30};
    OmegaTensor RC;
    CHECK(omega_tensor_reshape(g, C, 2, rs2, &RC) == 0, "reshape of contiguous copy");
    OmegaTensorInfo ci;
    omega_tensor_info(g, C, &ci);
    CHECK(!ci.is_view && ci.storage.slot != si.storage.slot, "contiguous has own storage");
    eq = false;
    omega_tensor_value_equal(g, C, TT, &eq);
    CHECK(eq, "contiguous copy value-equal to its view");
    uint8_t id1[32], id2[32];
    vid(C, id1); vid(TT, id2);
    CHECK(memcmp(id1, id2, 32) == 0, "layout is not identity: value ids equal");
    omega_tensor_view_id(g, C, id1); omega_tensor_view_id(g, TT, id2);
    CHECK(memcmp(id1, id2, 32) != 0, "view ids differ");
    /* zero-stride: broadcast of a column, then slice and transpose of it */
    uint64_t c1[2] = {4, 1}, c46[2] = {4, 6};
    float col[4] = {10, 20, 30, 40};
    OmegaTensor K = mk(2, c1, col), KB, KBT;
    omega_tensor_broadcast_to(g, K, 2, c46, &KB);
    omega_tensor_transpose(g, KB, &KBT);
    float *gk = rd(KBT);
    ok = gk != NULL;
    for (int i = 0; ok && i < 6; i++) for (int j = 0; j < 4; j++) ok = gk[i * 4 + j] == col[j];
    CHECK(ok, "transpose of zero-stride view");
    free(gk);
    CHECK(omega_tensor_reshape(g, KB, 1, (uint64_t[]){24}, &Z) == OMEGA_TENSOR_ERR_NOT_CONTIGUOUS,
          "reshape of zero-stride refused");
    /* bounds */
    uint64_t bs[3] = {0, 0, 0}, be[3] = {5, 5, 6};
    CHECK(omega_tensor_slice(g, T, bs, be, NULL, &Z) == OMEGA_TENSOR_ERR_BOUNDS, "slice past end");
    uint64_t ee[3] = {0, 5, 6};
    CHECK(omega_tensor_slice(g, T, bs, ee, NULL, &Z) == OMEGA_TENSOR_ERR_SHAPE, "empty slice refused");
    CHECK(omega_tensor_slice(g, T, bs, sp, (uint64_t[]){1, 0, 1}, &Z) == OMEGA_TENSOR_ERR_BAD_ARGS, "step 0");
    /* parent unchanged by all of the above */
    vid(T, after);
    CHECK(memcmp(before, after, 32) == 0, "parent value id unchanged by views");
    OmegaTensor all[] = {RC, C, R, V2, ST, TT, TS, S, P, KBT, KB, K, T};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        CHECK(omega_tensor_release(g, all[i]) == 0, "release %zu", i);
}

/* ---- 4. generation safety ------------------------------------------------- */
static void test_generations(void) {
    float d[6] = {1, 2, 3, 4, 5, 6}, o[6];
    uint64_t s[2] = {2, 3};
    OmegaTensor A = mk(2, s, d), V, W, O;
    omega_tensor_transpose(g, A, &V);
    OmegaTensorInfo ai;
    omega_tensor_info(g, A, &ai);
    /* releasing a view leaves the parent alive */
    omega_tensor_transpose(g, A, &W);
    CHECK(omega_tensor_release(g, W) == 0, "release view");
    CHECK(omega_tensor_read_f32(g, W, o, 6) == OMEGA_TENSOR_ERR_STALE, "released view stale");
    CHECK(omega_tensor_read_f32(g, A, o, 6) == 0, "parent still valid after view release");
    /* releasing the owner makes every view stale */
    CHECK(omega_tensor_release(g, A) == 0, "release owner");
    CHECK(omega_tensor_read_f32(g, A, o, 6) == OMEGA_TENSOR_ERR_STALE, "use after release");
    CHECK(omega_tensor_release(g, A) == OMEGA_TENSOR_ERR_STALE, "double release");
    CHECK(!omega_tensor_storage_valid(g, ai.storage), "storage handle stale");
    CHECK(omega_tensor_read_f32(g, V, o, 6) == OMEGA_TENSOR_ERR_STALE, "stale view read");
    OmegaTensorInfo vi;
    CHECK(omega_tensor_info(g, V, &vi) == OMEGA_TENSOR_ERR_STALE, "stale view info");
    CHECK(omega_tensor_binary(g, OMEGA_TB_ADD, V, V, &O) == OMEGA_TENSOR_ERR_STALE, "stale view op");
    CHECK(omega_tensor_matmul(g, V, V, &O) == OMEGA_TENSOR_ERR_STALE, "stale view matmul");
    CHECK(omega_tensor_transpose(g, V, &O) == OMEGA_TENSOR_ERR_STALE, "view of stale view");
    uint8_t id[32];
    CHECK(omega_tensor_value_id(g, V, id) == OMEGA_TENSOR_ERR_STALE, "stale value id");
    CHECK(omega_tensor_release(g, V) == 0, "stale view descriptor can still be released");
    /* no ABA: the reused slot gets a new generation */
    OmegaTensor B = mk(2, s, d);
    OmegaTensorInfo bi;
    omega_tensor_info(g, B, &bi);
    CHECK(bi.storage.slot == ai.storage.slot && bi.storage.generation != ai.storage.generation,
          "slot reused with new generation (slot %u gen %llu vs %llu)", bi.storage.slot,
          (unsigned long long)bi.storage.generation, (unsigned long long)ai.storage.generation);
    CHECK(omega_tensor_read_f32(g, A, o, 6) == OMEGA_TENSOR_ERR_STALE, "old handle not revived");
    CHECK(!omega_tensor_storage_valid(g, ai.storage), "old storage handle not revived");
    omega_tensor_release(g, B);
    OmegaTensor bogus = {.slot = 999999, .generation = 1};
    CHECK(omega_tensor_read_f32(g, bogus, o, 6) == OMEGA_TENSOR_ERR_STALE, "out of range handle");

    /* generation never wraps: slot retired at UINT64_MAX */
    OmegaTensorCtx *c1;
    CHECK(omega_tensor_ctx_create(1, omega_tensor_cpu_realization(), &c1) == 0, "ctx cap 1");
    CHECK(omega_tensor_test_set_storage_generation(c1, 0, UINT64_MAX - 1) == 0, "set gen");
    OmegaTensor t1, t2, t3;
    omega_tensor_from_f32(c1, 2, s, d, &t1);
    OmegaTensorInfo i1;
    omega_tensor_info(c1, t1, &i1);
    CHECK(i1.storage.generation == UINT64_MAX - 1, "gen max-1");
    CHECK(omega_tensor_test_set_storage_generation(c1, 0, 5) == OMEGA_TENSOR_ERR_BAD_ARGS, "set gen refused on live slot");
    omega_tensor_release(c1, t1);
    CHECK(omega_tensor_from_f32(c1, 2, s, d, &t2) == 0, "alloc at gen max");
    omega_tensor_info(c1, t2, &i1);
    CHECK(i1.storage.generation == UINT64_MAX, "gen max");
    omega_tensor_release(c1, t2);
    CHECK(omega_tensor_from_f32(c1, 2, s, d, &t3) == OMEGA_TENSOR_ERR_CAPACITY, "retired slot never reused");
    CHECK(omega_tensor_read_f32(c1, t2, o, 6) == OMEGA_TENSOR_ERR_STALE, "handle at gen max stale after release");
    omega_tensor_ctx_destroy(c1);
}

/* ---- 5. immutability ------------------------------------------------------ */
static void test_immutability(void) {
    float da[12], db[12];
    fill(da, 12, rand_f32);
    fill(db, 12, rand_f32);
    uint64_t s[2] = {3, 4}, s43[2] = {4, 3};
    OmegaTensor A = mk(2, s, da), B = mk(2, s, db), B2 = mk(2, s43, db), O;
    uint8_t ia[32], ib[32], ib2[32], x[32];
    vid(A, ia); vid(B, ib); vid(B2, ib2);
    for (size_t k = 0; k < NBIN; k++) { omega_tensor_binary(g, BIN_CASES[k].op, A, B, &O); omega_tensor_release(g, O); }
    omega_tensor_fma(g, A, B, A, &O); omega_tensor_release(g, O);
    omega_tensor_unary(g, OMEGA_TU_SQRT, A, &O); omega_tensor_release(g, O);
    omega_tensor_reduce(g, OMEGA_TR_SUM, A, 1, false, &O); omega_tensor_release(g, O);
    omega_tensor_matmul(g, A, B2, &O); omega_tensor_release(g, O);
    omega_tensor_cast(g, A, OMEGA_DT_F16, &O); omega_tensor_release(g, O);
    vid(A, x); CHECK(memcmp(ia, x, 32) == 0, "A unchanged by ops");
    vid(B, x); CHECK(memcmp(ib, x, 32) == 0, "B unchanged by ops");
    vid(B2, x); CHECK(memcmp(ib2, x, 32) == 0, "B2 unchanged by ops");
    /* outputs never alias inputs */
    omega_tensor_binary(g, OMEGA_TB_ADD, A, B, &O);
    OmegaTensorInfo io, iA;
    omega_tensor_info(g, O, &io); omega_tensor_info(g, A, &iA);
    CHECK(!io.is_view && io.storage.slot != iA.storage.slot, "op output has its own storage");
    omega_tensor_release(g, O);
    /* NaN payload is not semantic; -0 and +0 are */
    float n1[1] = {omega_bits_to_float(0x7fc00000U)}, n2[1] = {omega_bits_to_float(0xffa12345U)};
    float z1[1] = {0.0f}, z2[1] = {omega_bits_to_float(0x80000000U)};
    uint64_t s1[1] = {1};
    OmegaTensor N1 = mk(1, s1, n1), N2 = mk(1, s1, n2), Z1 = mk(1, s1, z1), Z2 = mk(1, s1, z2);
    uint8_t a1[32], a2[32];
    vid(N1, a1); vid(N2, a2);
    CHECK(memcmp(a1, a2, 32) == 0, "NaN payloads share a value id");
    vid(Z1, a1); vid(Z2, a2);
    CHECK(memcmp(a1, a2, 32) != 0, "+0 and -0 differ");
    vid(A, a1); vid(B2, a2);
    OmegaTensor AR;
    omega_tensor_reshape(g, A, 2, s43, &AR);
    vid(AR, a1);
    CHECK(memcmp(a1, ia, 32) != 0, "shape is part of value identity");
    OmegaTensor all[] = {AR, A, B, B2, N1, N2, Z1, Z2};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) omega_tensor_release(g, all[i]);
}

/* ---- 6. CPU parity -------------------------------------------------------- */
static void test_elementwise_parity(void) {
    const size_t n = 4099;
    float *a = malloc(n * 4), *b = malloc(n * 4), *c = malloc(n * 4), *ex = malloc(n * 4);
    fill(a, n, rand_f32); fill(b, n, rand_f32); fill(c, n, rand_f32);
    for (size_t i = 0; i < NSPECIAL * NSPECIAL && i < n; i++) {  /* every special pair */
        a[i] = omega_bits_to_float(SPECIALS[i / NSPECIAL]);
        b[i] = omega_bits_to_float(SPECIALS[i % NSPECIAL]);
    }
    uint64_t s[1] = {n};
    OmegaTensor A = mk(1, s, a), B = mk(1, s, b), Cc = mk(1, s, c), O;
    for (size_t k = 0; k < NBIN; k++) {
        g_selop = BIN_CASES[k].selop;
        for (size_t i = 0; i < n; i++) ex[i] = BIN_CASES[k].f(a[i], b[i]);
        int rc = omega_tensor_binary(g, BIN_CASES[k].op, A, B, &O);
        float *got = rc ? NULL : rd(O);
        CHECK(got && mismatches(ex, got, n) == 0, "parity %s rc=%d", BIN_CASES[k].name, rc);
        free(got);
        if (!rc) omega_tensor_release(g, O);
    }
    for (size_t i = 0; i < n; i++) ex[i] = omega_ieee_sqrt(a[i]);
    int rc = omega_tensor_unary(g, OMEGA_TU_SQRT, A, &O);
    float *got = rc ? NULL : rd(O);
    CHECK(got && mismatches(ex, got, n) == 0, "parity SQRT");
    free(got); if (!rc) omega_tensor_release(g, O);
    for (size_t i = 0; i < n; i++) ex[i] = omega_ref_ffma_int(a[i], b[i], c[i]);
    rc = omega_tensor_fma(g, A, B, Cc, &O);
    got = rc ? NULL : rd(O);
    CHECK(got && mismatches(ex, got, n) == 0, "parity FMA");
    free(got); if (!rc) omega_tensor_release(g, O);
    /* FMA with broadcast scalar c */
    float c0[1] = {c[7]};
    OmegaTensor C0 = mk(0, NULL, c0);
    for (size_t i = 0; i < n; i++) ex[i] = omega_ref_ffma_int(a[i], b[i], c0[0]);
    rc = omega_tensor_fma(g, A, B, C0, &O);
    got = rc ? NULL : rd(O);
    CHECK(got && mismatches(ex, got, n) == 0, "parity FMA broadcast c");
    free(got); if (!rc) omega_tensor_release(g, O);
    /* casts */
    uint16_t *h = malloc(n * 2), *hg = malloc(n * 2);
    for (size_t i = 0; i < n; i++) h[i] = omega_ref_f32_to_f16(a[i]);
    rc = omega_tensor_cast(g, A, OMEGA_DT_F16, &O);
    size_t mm = 0;
    if (!rc) { omega_tensor_read(g, O, hg, n * 2);
        for (size_t i = 0; i < n; i++) mm += !omega_numeric_compare_equal(OMEGA_CMP_F16_BITS, h[i], hg[i]); }
    CHECK(!rc && mm == 0, "parity F32->F16 rc=%d mm=%zu", rc, mm);
    if (!rc) {
        OmegaTensor W;
        int rc2 = omega_tensor_cast(g, O, OMEGA_DT_F32, &W);
        for (size_t i = 0; i < n; i++) ex[i] = omega_ref_f16_to_f32(h[i]);
        got = rc2 ? NULL : rd(W);
        CHECK(got && mismatches(ex, got, n) == 0, "parity F16->F32");
        free(got); if (!rc2) omega_tensor_release(g, W);
        omega_tensor_release(g, O);
    }
    rc = omega_tensor_cast(g, A, OMEGA_DT_BF16, &O);
    if (omega_numeric_cpu_has_bf16()) {
        for (size_t i = 0; i < n; i++) h[i] = omega_ref_f32_to_bf16(a[i]);
        mm = 0;
        if (!rc) { omega_tensor_read(g, O, hg, n * 2);
            for (size_t i = 0; i < n; i++) mm += !omega_numeric_compare_equal(OMEGA_CMP_BF16_BITS, h[i], hg[i]); }
        CHECK(!rc && mm == 0, "parity F32->BF16 rc=%d mm=%zu", rc, mm);
        if (!rc) {
            OmegaTensor W;
            int rc2 = omega_tensor_cast(g, O, OMEGA_DT_F32, &W);
            for (size_t i = 0; i < n; i++) ex[i] = omega_ref_bf16_to_f32(h[i]);
            got = rc2 ? NULL : rd(W);
            CHECK(got && mismatches(ex, got, n) == 0, "parity BF16->F32");
            free(got); if (!rc2) omega_tensor_release(g, W);
            omega_tensor_release(g, O);
        }
    } else {
        CHECK(rc == OMEGA_TENSOR_ERR_NUMERIC && omega_tensor_last_numeric_error(g) == OMEGA_NUMERIC_ERR_NOT_ENCODED,
              "BF16 refused without FEAT_BF16");
    }
    OmegaTensor all[] = {A, B, Cc, C0};
    for (size_t i = 0; i < 4; i++) omega_tensor_release(g, all[i]);
    free(a); free(b); free(c); free(ex); free(h); free(hg);
}

static void test_reduce_parity(void) {
    /* 1-D lengths around the tile and level boundaries */
    static const size_t lens[] = {1, 2, 31, 32, 33, 63, 64, 1000, 1023, 1024, 1025, 32769};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        size_t n = lens[li];
        float *x = malloc(n * 4);
        fill(x, n, li % 2 ? rand_mod : rand_f32);
        uint64_t s[1] = {n};
        OmegaTensor X = mk(1, s, x), O;
        for (int op = 0; op < OMEGA_TR_COUNT; op++) {
            float ex = ref_tree((OmegaTensorReduceOp)op, x, n), got = 0;
            int rc = omega_tensor_reduce(g, (OmegaTensorReduceOp)op, X, 0, false, &O);
            if (!rc) { omega_tensor_read_f32(g, O, &got, 1); omega_tensor_release(g, O); }
            CHECK(!rc && same(ex, got), "reduce op %d n=%zu", op, n);
        }
        omega_tensor_release(g, X);
        free(x);
    }
    /* signed zero: SUM of all -0 is -0 (pad is -0, not +0) */
    {
        float nz[3] = {omega_bits_to_float(0x80000000U), omega_bits_to_float(0x80000000U),
                       omega_bits_to_float(0x80000000U)}, got = 0;
        uint64_t s3[1] = {3};
        OmegaTensor Z = mk(1, s3, nz), O;
        int rc = omega_tensor_reduce(g, OMEGA_TR_SUM, Z, 0, false, &O);
        if (!rc) { omega_tensor_read_f32(g, O, &got, 1); omega_tensor_release(g, O); }
        CHECK(!rc && omega_float_to_bits(got) == 0x80000000U, "SUM of -0s is -0");
        omega_tensor_release(g, Z);
    }
    /* every axis of a [3,37,5] tensor, plus keepdims and a strided view */
    uint64_t s[3] = {3, 37, 5};
    float x[555], ex[185], row[37];
    fill(x, 555, rand_mod);
    OmegaTensor X = mk(3, s, x), XT, O;
    for (uint32_t ax = 0; ax < 3; ax++)
        for (int op = 0; op < OMEGA_TR_COUNT; op++) {
            uint64_t n = s[ax], cnt = 0;
            for (uint64_t i = 0; i < (ax == 0 ? 1 : 3); i++)
                for (uint64_t j = 0; j < (ax == 1 ? 1 : 37); j++)
                    for (uint64_t k = 0; k < (ax == 2 ? 1 : 5); k++) {
                        for (uint64_t r = 0; r < n; r++) {
                            uint64_t ii = ax == 0 ? r : i, jj = ax == 1 ? r : j, kk = ax == 2 ? r : k;
                            row[r] = x[(ii * 37 + jj) * 5 + kk];
                        }
                        ex[cnt++] = ref_tree((OmegaTensorReduceOp)op, row, n);
                    }
            int rc = omega_tensor_reduce(g, (OmegaTensorReduceOp)op, X, ax, ax == 1, &O);
            float *got = rc ? NULL : rd(O);
            OmegaTensorInfo oi;
            if (!rc) omega_tensor_info(g, O, &oi);
            CHECK(got && mismatches(ex, got, cnt) == 0 && oi.rank == (ax == 1 ? 3u : 2u),
                  "reduce axis %u op %d", ax, op);
            free(got);
            if (!rc) omega_tensor_release(g, O);
        }
    /* reduce of a permuted view equals reduce of its contiguous copy */
    uint32_t perm[3] = {1, 2, 0};
    omega_tensor_permute(g, X, perm, &XT);
    OmegaTensor XC, O1, O2;
    omega_tensor_contiguous(g, XT, &XC);
    omega_tensor_reduce(g, OMEGA_TR_SUM, XT, 0, false, &O1);
    omega_tensor_reduce(g, OMEGA_TR_SUM, XC, 0, false, &O2);
    bool eq = false;
    omega_tensor_value_equal(g, O1, O2, &eq);
    CHECK(eq, "reduce of view == reduce of copy");
    OmegaTensor all[] = {O1, O2, XC, XT, X};
    for (size_t i = 0; i < 5; i++) omega_tensor_release(g, all[i]);
    CHECK(strcmp(omega_tensor_cpu_realization()->reduce_order,
                 "RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL") == 0,
          "reduce order string equals the E1 WP-D order");
}

static void check_matmul(size_t M, size_t K, size_t N, float (*gen)(void)) {
    float *a = malloc(M * K * 4), *b = malloc(K * N * 4), *ex = malloc(M * N * 4);
    fill(a, M * K, gen);
    fill(b, K * N, gen);
    ref_matmul(a, b, ex, M, K, N);
    uint64_t sa[2] = {M, K}, sb[2] = {K, N};
    OmegaTensor A = mk(2, sa, a), B = mk(2, sb, b), O;
    int rc = omega_tensor_matmul(g, A, B, &O);
    float *got = rc ? NULL : rd(O);
    size_t mm = got ? mismatches(ex, got, M * N) : M * N;
    CHECK(mm == 0, "matmul %zux%zux%zu rc=%d mismatches=%zu", M, K, N, rc, mm);
    free(got);
    if (!rc) omega_tensor_release(g, O);
    omega_tensor_release(g, A);
    omega_tensor_release(g, B);
    free(a); free(b); free(ex);
}

static void test_matmul_parity(void) {
    check_matmul(1, 1, 1, rand_mod);
    check_matmul(1, 1, 1, rand_f32);
    check_matmul(7, 13, 5, rand_mod);
    check_matmul(7, 13, 5, rand_f32);
    check_matmul(64, 64, 64, rand_mod);
    check_matmul(129, 3, 257, rand_mod);
    check_matmul(3, 1025, 2, rand_mod);   /* K across two reduction levels */
    check_matmul(1, 33, 1, rand_mod);
    /* strided operands: A^T as a view of a [K,M] tensor, B a slice */
    size_t M = 9, K = 40, N = 6;
    float *at = malloc(K * M * 4), *bb = malloc(K * 2 * N * 4), *a = malloc(M * K * 4), *b = malloc(K * N * 4);
    float *ex = malloc(M * N * 4);
    fill(at, K * M, rand_mod);
    fill(bb, K * 2 * N, rand_mod);
    for (size_t i = 0; i < M; i++) for (size_t k = 0; k < K; k++) a[i * K + k] = at[k * M + i];
    for (size_t k = 0; k < K; k++) for (size_t j = 0; j < N; j++) b[k * N + j] = bb[k * 2 * N + 2 * j + 1];
    ref_matmul(a, b, ex, M, K, N);
    uint64_t sat[2] = {K, M}, sbb[2] = {K, 2 * N};
    OmegaTensor AT = mk(2, sat, at), BB = mk(2, sbb, bb), AV, BV, O;
    omega_tensor_transpose(g, AT, &AV);
    omega_tensor_slice(g, BB, (uint64_t[]){0, 1}, (uint64_t[]){K, 2 * N}, (uint64_t[]){1, 2}, &BV);
    int rc = omega_tensor_matmul(g, AV, BV, &O);
    float *got = rc ? NULL : rd(O);
    CHECK(got && mismatches(ex, got, M * N) == 0, "matmul of strided views");
    free(got); if (!rc) omega_tensor_release(g, O);
    OmegaTensor vs[] = {AV, BV, AT, BB};
    for (size_t i = 0; i < 4; i++) omega_tensor_release(g, vs[i]);
    free(at); free(bb); free(a); free(b); free(ex);
    /* batched with broadcast batch axes: [2,1,3,4] x [5,4,6] -> [2,5,3,6] */
    uint64_t s1[4] = {2, 1, 3, 4}, s2[3] = {5, 4, 6};
    float x1[24], x2[120], e[180], o1[12], o2[24], r[18];
    fill(x1, 24, rand_mod);
    fill(x2, 120, rand_mod);
    for (int p = 0; p < 2; p++) for (int q = 0; q < 5; q++) {
        memcpy(o1, x1 + p * 12, 48);
        memcpy(o2, x2 + q * 24, 96);
        ref_matmul(o1, o2, r, 3, 4, 6);
        memcpy(e + (p * 5 + q) * 18, r, 72);
    }
    OmegaTensor X1 = mk(4, s1, x1), X2 = mk(3, s2, x2);
    rc = omega_tensor_matmul(g, X1, X2, &O);
    OmegaTensorInfo oi;
    if (!rc) omega_tensor_info(g, O, &oi);
    got = rc ? NULL : rd(O);
    CHECK(got && oi.rank == 4 && oi.shape[0] == 2 && oi.shape[1] == 5 && oi.shape[2] == 3 && oi.shape[3] == 6 &&
          mismatches(e, got, 180) == 0, "batched broadcast matmul rc=%d", rc);
    free(got); if (!rc) omega_tensor_release(g, O);
    uint64_t s3[4] = {3, 1, 4, 6};
    OmegaTensor X3 = mk(4, s3, x2);
    CHECK(omega_tensor_matmul(g, X1, X3, &O) == OMEGA_TENSOR_ERR_SHAPE, "batch axes [2,1] vs [3,1] refused");
    omega_tensor_release(g, X1); omega_tensor_release(g, X2); omega_tensor_release(g, X3);
    CHECK(strstr(OMEGA_TENSOR_MATMUL_DECLARED_ORDER, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) != NULL,
          "matmul order names the reduce order");
}

/* ---- 7. mutation: a different order or fusion is caught ------------------ */
static int seq_reduce(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    float acc = x[0];
    for (size_t i = 1; i < n; i++) acc = op == OMEGA_TR_MAX ? omega_ref_fmax(acc, x[i]) : omega_ref_fadd(acc, x[i]);
    *out = acc;
    return 0;
}
static int fma_chain_elementwise(OmegaNumericOp op, const float *a, const float *b, const float *c,
                                 float *out, size_t n) {
    if (op != OMEGA_NOP_FMUL) return omega_numeric_cpu_realize(op, a, b, c, out, n);
    /* "fused" products: keep a*b exact-ish by folding into an FFMA chain in out[0] */
    float acc = 0.0f;
    for (size_t i = 0; i < n; i++) { acc = omega_ref_ffma_int(a[i], b[i], acc); out[i] = 0.0f; }
    out[0] = acc;
    return 0;
}
static void test_mutations(void) {
    const size_t M = 16, K = 300, N = 16;
    float *a = malloc(M * K * 4), *b = malloc(K * N * 4), *ex = malloc(M * N * 4);
    fill(a, M * K, rand_mod);
    fill(b, K * N, rand_mod);
    ref_matmul(a, b, ex, M, K, N);
    uint64_t sa[2] = {M, K}, sb[2] = {K, N};
    OmegaTensorRealization mut[2];
    mut[0] = *omega_tensor_cpu_realization();
    mut[0].name = "MUTANT_SEQUENTIAL_SUM";  /* lies about its order string */
    mut[0].reduce = seq_reduce;
    mut[1] = *omega_tensor_cpu_realization();
    mut[1].name = "MUTANT_FFMA_CHAIN";
    mut[1].elementwise = fma_chain_elementwise;
    for (int m = 0; m < 2; m++) {
        OmegaTensorCtx *c;
        omega_tensor_ctx_create(16, &mut[m], &c);
        OmegaTensor A, B, O;
        omega_tensor_from_f32(c, 2, sa, a, &A);
        omega_tensor_from_f32(c, 2, sb, b, &B);
        float *got = malloc(M * N * 4);
        int rc = omega_tensor_matmul(c, A, B, &O);
        if (!rc) omega_tensor_read_f32(c, O, got, M * N);
        size_t mm = rc ? M * N : mismatches(ex, got, M * N);
        CHECK(mm > 0, "mutant %s caught (%zu of %zu outputs differ)", mut[m].name, mm, M * N);
        free(got);
        omega_tensor_ctx_destroy(c);
    }
    free(a); free(b); free(ex);
}

/* ---- 8. determinism ------------------------------------------------------- */
static void kat_digest(uint8_t out[32]) {
    uint64_t save = g_rng;
    g_rng = 0x0123456789abcdefULL;
    float a[7 * 13], b[13 * 5];
    fill(a, 91, rand_mod);
    fill(b, 65, rand_mod);
    uint64_t sa[2] = {7, 13}, sb[2] = {13, 5};
    OmegaTensor A = mk(2, sa, a), B = mk(2, sb, b), P, R, Q;
    omega_tensor_matmul(g, A, B, &P);
    omega_tensor_reduce(g, OMEGA_TR_SUM, A, 1, true, &R);
    omega_tensor_binary(g, OMEGA_TB_DIV, P, R, &Q);  /* [7,5] / [7,1] */
    vid(Q, out);
    OmegaTensor all[] = {A, B, P, R, Q};
    for (int i = 0; i < 5; i++) omega_tensor_release(g, all[i]);
    g_rng = save;
}
static void test_determinism(void) {
    uint8_t d1[32], d2[32];
    kat_digest(d1);
    kat_digest(d2);
    CHECK(memcmp(d1, d2, 32) == 0, "same inputs, same value id");
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d1[i]);
    printf("KAT value id (matmul 7x13x5, row-sum, div): %s\n", hex);
    /* Frozen on 2026-10-01 from the first CPU run; any change to an op, the
     * order or the value-id encoding changes it. */
    static const char frozen[] = "9efaa4bc9826cce169ae8da3796a188a48ca6ac0bbb7f71f7ba7f11cca46f991";
    CHECK(strcmp(frozen, hex) == 0, "KAT matches frozen %s", frozen);
}

int main(void) {
    if (!omega_numeric_fpenv_ok()) { printf("FPCR not RNE/no-FTZ: refusing to run\n"); return 2; }
    if (omega_tensor_ctx_create(4096, omega_tensor_cpu_realization(), &g)) { printf("ctx\n"); return 2; }
    printf("reduce seam: %s\n", omega_tensor_seam_reduce_source());
    test_shape_type();
    test_broadcast();
    test_views();
    test_generations();
    test_immutability();
    test_elementwise_parity();
    test_reduce_parity();
    test_matmul_parity();
    test_mutations();
    test_determinism();
    /* every test released what it made */
    uint32_t lt, ls;
    omega_tensor_live_counts(g, &lt, &ls);
    CHECK(lt == 0 && ls == 0, "no leaked tensors (%u) or storage (%u)", lt, ls);
    omega_tensor_ctx_destroy(g);
    printf("M20 OMEGA_TENSOR CPU tests: %d pass, %d fail\n", g_pass, g_fail);
    printf("M20 verdict: NOT QUALIFIED (CPU tier only; GB10 parity NOT_RUN; E1 open)\n");
    return g_fail ? 1 : 0;
}
