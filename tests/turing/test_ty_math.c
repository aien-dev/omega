/* Turing Yield TY-1 tests: math contract, CTR1 reader, model code, companion
 * records, and hostile cases (leakage, profile/floor/encoder/baseline changes,
 * tampering, memorizers, train-only gains). Runs on the committed CTR1 fixture
 * (tests/turing/fixtures, extracted from fit seed 1; see PROVENANCE.md there).
 *
 * argv[1] = output prefix: temp files go to <prefix>.*, and <prefix>.det gets a
 * determinism printout that must be byte-identical across plain and ASan builds.
 * libm is used here only to cross-check the integer path, never to produce it.
 */
#include "turing/ty_record.h"

#include "sha256.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            ++g_pass;                                     \
        } else {                                          \
            ++g_fail;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static const char *FIX_A = "tests/turing/fixtures/ctr1_seed1_crumbs05-07.ctr";
static const char *FIX_B = "tests/turing/fixtures/ctr1_seed1_crumbs10-12.ctr";

/* Goldens for the NEW domains only (existing turing.* goldens live in test_turing.c, untouched). */
static const char *GOLD_FIX_A = "777193633a4ff8092659024f18194bd111da5129c6e8bc3f04a292c138a732cc";
static const char *GOLD_FIX_B = "c01b91468d5b6cfb4953166d6f161cee2dceeb174675d0f7d90418b2b8967ccc";
static const char *GOLD_PROFILE_V0_REAL = "f4292df9748e306165f30b250673a0062391054321ea6dbe68855ecaa0fa8be8";
static const char *GOLD_MANIFEST_REAL = "6efc04b525af60706bd0f3176013fe234d8e8a52e2554cff3add8a4e16171dca";
static const char *GOLD_FIXTURE_GAIN = "acf0b17065f0d4930425e55ad4af9bcfd91a2c0c0d0aa4dc6f086c3457e86cd6";

static char g_prefix[512];
static FILE *g_det;

static void hexs(const ty_digest *d, char out[65]) { ty_hex(d, out); }

static int copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb"), *b = fopen(dst, "wb");
    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return -1;
    }
    char buf[8192];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, k, b);
    fclose(a);
    return fclose(b);
}

static int patch_byte(const char *path, long off, uint8_t v) {
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;
    fseek(f, off, SEEK_SET);
    fputc(v, f);
    return fclose(f);
}

static void tmp_path(char *out, size_t n, const char *suffix) { snprintf(out, n, "%s.%s", g_prefix, suffix); }

/* ---------------------------------------------------------------- math */

static void test_math(void) {
    /* integer -log2 table vs libm: <= 0.501 ub everywhere */
    double worst = 0;
    int64_t sum = 0;
    for (uint32_t q = 1; q <= TY_QONE; ++q) {
        int64_t ub = ty_ubits_q16(q);
        double exact = (16.0 - log2((double)q)) * 1e6;
        double err = fabs((double)ub - exact);
        if (err > worst) worst = err;
        sum += ub;
    }
    CHECK(worst <= 0.501, "table error %.6f ub exceeds the 0.501 ub bound", worst);
    CHECK(ty_ubits_q16(TY_QONE) == 0, "q=65536 must cost 0");
    CHECK(ty_ubits_q16(32768) == 1000000, "q=32768 must cost exactly 1 bit");
    CHECK(ty_ubits_q16(1) == 16000000, "q=1 (the floor) must cost exactly 16 bits");
    CHECK(ty_ubits_q16(0) == TY_E_RANGE && ty_ubits_q16(65537) == TY_E_RANGE, "q out of range refused");
    fprintf(g_det, "ub_table_sum=%" PRId64 "\n", sum);

    int64_t v;
    CHECK(ty_ubits_ratio(1, 2, &v) == TY_OK && v == 1000000, "1/2 -> 1 bit");
    CHECK(ty_ubits_ratio(7, 7, &v) == TY_OK && v == 0, "1 -> 0 bits");
    CHECK(ty_ubits_ratio(0, 5, &v) == TY_E_ZERO_REALIZED, "zero numerator refused");
    CHECK(ty_ubits_ratio(3, 2, &v) == TY_E_RANGE, "p > 1 refused");
    CHECK(ty_ubits_ratio(1, (uint64_t)1 << 62, &v) == TY_OK && v == 62000000, "2^-62 -> 62 bits");

    /* validation of outside probability vectors */
    double ok[3] = {0.5, 0.25, 0.25};
    double nanv[3] = {0.5, NAN, 0.5}, infv[3] = {0.5, INFINITY, 0.0}, neg[3] = {0.75, -0.25, 0.5};
    double off[3] = {0.5, 0.25, 0.2}, negz[3] = {0.5, -0.0, 0.5};
    CHECK(ty_validate_f64(ok, 3, 1e-12) == TY_OK, "valid vector accepted");
    CHECK(ty_validate_f64(nanv, 3, 1e-12) == TY_E_NAN, "NaN refused");
    CHECK(ty_validate_f64(infv, 3, 1e-12) == TY_E_INF, "Inf refused");
    CHECK(ty_validate_f64(neg, 3, 1e-12) == TY_E_NEG, "negative refused");
    CHECK(ty_validate_f64(off, 3, 1e-12) == TY_E_NORM, "sum 0.95 refused");
    CHECK(ty_validate_f64(off, 3, 0.1) == TY_OK, "sum 0.95 accepted at tolerance 0.1 (declared)");
    CHECK(ty_validate_f64(negz, 3, 1e-12) == TY_OK, "-0.0 is zero, not negative");
    CHECK(ty_validate_f64(ok, 0, 1e-12) == TY_E_ARG, "empty vector refused");

    CHECK(ty_ubits_f64(ok, 3, 0, 1e-12, &v) == TY_OK && v == 1000000, "p=0.5 -> 1,000,000 ub");
    CHECK(ty_ubits_f64(ok, 3, 1, 1e-12, &v) == TY_OK && v == 2000000, "p=0.25 -> 2,000,000 ub");
    double z[3] = {0.5, 0.5, 0.0};
    CHECK(ty_ubits_f64(z, 3, 2, 1e-12, &v) == TY_E_ZERO_REALIZED, "zero probability on realized outcome refused");
    double tiny[3] = {0.5, 0.5, 1e-300};
    CHECK(ty_ubits_f64(tiny, 3, 2, 1e-12, &v) == TY_E_UNDERFLOW, "1e-300 on realized outcome refused (underflow)");
    double sub[3] = {0.5, 0.5, 4.9e-324};
    CHECK(ty_ubits_f64(sub, 3, 2, 1e-12, &v) == TY_E_UNDERFLOW, "subnormal refused");
    double lowok[3] = {0.5, 0.5, ldexp(1.0, -60)};
    CHECK(ty_ubits_f64(lowok, 3, 2, 1e-12, &v) == TY_OK && v == 60000000, "2^-60 -> 60 bits");
    CHECK(ty_ubits_f64(ok, 3, 3, 1e-12, &v) == TY_E_ARG, "realized index out of range");

    /* bits, never nats: cross-check against a natural-log computation converted by /ln2 */
    double p3[2] = {0.3, 0.7};
    CHECK(ty_ubits_f64(p3, 2, 0, 1e-12, &v) == TY_OK, "p=0.3 scored");
    double nats = -log(0.3), from_nats = nats / log(2.0) * 1e6;
    CHECK(fabs((double)v - from_nats) <= 1.0, "p=0.3: %" PRId64 " ub vs nats/ln2 %.3f", v, from_nats);
    CHECK(fabs((double)v - nats * 1e6) > 100000.0, "result must not be in nats");

    /* DL and T arithmetic */
    int64_t dl, t;
    CHECK(ty_dl(1000, 5000000, &dl) == TY_OK && dl == 1005000000, "DL = L(M) bits + L(D|M) ub");
    CHECK(ty_dl(UINT64_MAX / 2, 0, &dl) == TY_E_OVERFLOW, "L(M) overflow refused");
    CHECK(ty_dl(1, -1, &dl) == TY_E_RANGE, "negative L(D|M) refused");
    CHECK(ty_gain(10, 4, &t) == TY_OK && t == 6, "T = DL(B) - DL(M)");
    CHECK(ty_gain(4, 10, &t) == TY_OK && t == -6, "T can be negative");
    CHECK(ty_add(INT64_MAX, 1, &t) == TY_E_OVERFLOW, "add overflow refused");

    /* quantizer: sum 65536, floor 1, deterministic, largest remainder */
    uint64_t c0[9] = {0}, c1[9] = {1000000, 0, 0, 0, 0, 0, 0, 0, 1}, c2[9] = {5, 5, 5, 5, 5, 5, 5, 5, 5};
    uint32_t q[9];
    for (int k = 0; k < 3; ++k) {
        const uint64_t *c = k == 0 ? c0 : k == 1 ? c1 : c2;
        CHECK(ty_quantize_kt(c, 9, q) == TY_OK, "quantize");
        uint32_t s = 0, mn = UINT32_MAX;
        for (int x = 0; x < 9; ++x) s += q[x], mn = q[x] < mn ? q[x] : mn;
        CHECK(s == TY_QONE && mn >= 1, "row sums to 65536 with floor 1 (sum %u min %u)", s, mn);
        fprintf(g_det, "quant%d=", k);
        for (int x = 0; x < 9; ++x) fprintf(g_det, "%u ", q[x]);
        fprintf(g_det, "\n");
    }
    ty_quantize_kt(c1, 9, q);
    CHECK(q[1] == 1 && q[0] > 65000, "dominant symbol takes the mass, others sit at the floor");
    ty_quantize_kt(c0, 9, q);
    CHECK(q[0] == 7282 && q[8] == 7281, "zero counts give near-uniform, remainder to lower symbols");
    CHECK(ty_quantize_kt(c0, 1, q) == TY_E_ARG && ty_quantize_kt(c0, 17, q) == TY_E_ARG, "K out of range");
}

/* ------------------------------------------------------------- reader */

static void test_reader(void) {
    ty_stream a, b;
    ty_stream_init(&a);
    ty_stream_init(&b);
    char why[256] = "";
    int64_t na = ty_ctr1_read(FIX_A, &a, why, sizeof why);
    int64_t nb = ty_ctr1_read(FIX_B, &b, why, sizeof why);
    CHECK(na == 1418 && a.ncrumb == 3 && a.gaps == 0, "fixture A: %lld records %u crumbs (%s)", (long long)na,
          a.ncrumb, why);
    CHECK(nb == 1330 && b.ncrumb == 3, "fixture B: %lld records %u crumbs", (long long)nb, b.ncrumb);
    ty_digest d;
    char h[65];
    ty_file_sha256(FIX_A, &d);
    hexs(&d, h);
    CHECK(strcmp(h, GOLD_FIX_A) == 0, "fixture A digest %s", h);
    ty_file_sha256(FIX_B, &d);
    hexs(&d, h);
    CHECK(strcmp(h, GOLD_FIX_B) == 0, "fixture B digest %s", h);
    uint64_t hist[TY_OUT_K] = {0};
    for (size_t i = 0; i < a.n; ++i) hist[a.ev[i].sym]++;
    fprintf(g_det, "fixA_hist=");
    for (int x = 0; x < TY_OUT_K; ++x) fprintf(g_det, "%" PRIu64 " ", hist[x]);
    fprintf(g_det, "\n");
    ty_stream_free(&a);
    ty_stream_free(&b);

    /* hostile CTR1 files: every one must be refused */
    struct {
        long off;
        uint8_t v;
        const char *what;
    } bad[] = {
        {0, 'X', "magic"},
        {4, 2, "version 2"},
        {6, 3, "kind 3"},
        {22, 1, "op_origin 1 (library op in control)"},
        {20, 15, "op_index 15 on EXPAND"},
        {25, 3, "fit 3"},
        {247 * 5 + 8, 0, "event_index restarts mid-crumb without being a crumb start? (0 is a start; allowed)"},
    };
    char tp[600];
    tmp_path(tp, sizeof tp, "bad.ctr");
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        copy_file(FIX_A, tp);
        patch_byte(tp, bad[i].off, bad[i].v);
        ty_stream s;
        ty_stream_init(&s);
        int64_t n = ty_ctr1_read(tp, &s, why, sizeof why);
        if (i + 1 == sizeof bad / sizeof *bad) {
            /* setting event_index 0 inside a crumb splits it: accepted as a new crumb, and counted */
            CHECK(n == 1418 && s.ncrumb == 4, "index 0 opens a crumb (%lld, %u)", (long long)n, s.ncrumb);
        } else {
            CHECK(n == TY_E_FORMAT, "refuse %s (got %lld)", bad[i].what, (long long)n);
        }
        ty_stream_free(&s);
    }
    /* event_index going backwards without restarting at 0 */
    copy_file(FIX_A, tp);
    patch_byte(tp, 247 * 5 + 8, 2); /* record 5 had index 5 */
    {
        ty_stream s;
        ty_stream_init(&s);
        CHECK(ty_ctr1_read(tp, &s, why, sizeof why) == TY_E_FORMAT, "decreasing event_index refused");
        ty_stream_free(&s);
    }
    /* find a pruned record and make its result_class inconsistent */
    {
        FILE *f = fopen(FIX_A, "rb");
        uint8_t r[TY_CTR1_BYTES];
        long idx = -1;
        for (long i = 0; fread(r, 1, TY_CTR1_BYTES, f) == TY_CTR1_BYTES; ++i)
            if (r[6] == 1 && r[23] != 0) {
                idx = i;
                break;
            }
        fclose(f);
        copy_file(FIX_A, tp);
        patch_byte(tp, idx * TY_CTR1_BYTES + 7, 4);
        ty_stream s;
        ty_stream_init(&s);
        CHECK(idx >= 0 && ty_ctr1_read(tp, &s, why, sizeof why) == TY_E_FORMAT,
              "pruned record claiming Improved refused");
        ty_stream_free(&s);
    }
    /* size not a multiple of 247 */
    {
        copy_file(FIX_A, tp);
        FILE *f = fopen(tp, "ab");
        fputc(0, f);
        fclose(f);
        ty_stream s;
        ty_stream_init(&s);
        CHECK(ty_ctr1_read(tp, &s, why, sizeof why) == TY_E_FORMAT, "size %% 247 != 0 refused");
        ty_stream_free(&s);
    }
    remove(tp);
}

/* -------------------------------------------------------------- models */

static int load(const char *p, ty_stream *s) {
    ty_stream_init(s);
    char why[256];
    return ty_ctr1_read(p, s, why, sizeof why) < 0 ? -1 : 0;
}

/* DL of a model fit on `fit` and scored on `score`, from its decoded code. */
static int64_t dl_of(const ty_stream *fit, const ty_stream *score, unsigned mask, int rule, int uniform,
                     uint64_t *lm_out) {
    ty_model m, d;
    const ty_stream *sv[1] = {fit};
    int rc = uniform ? ty_model_uniform(&m, TY_OUT_K) : ty_model_fit(&m, sv, 1, mask, TY_OUT_K, rule);
    uint8_t *code = NULL;
    size_t nb;
    uint64_t lm = 0;
    if (rc == TY_OK) rc = ty_model_encode(&m, &code, &nb, &lm);
    ty_model_free(&m);
    if (rc == TY_OK) rc = ty_model_decode(code, nb, &d, NULL, NULL, 0);
    free(code);
    int64_t ub = 0, dl = 0;
    if (rc == TY_OK) rc = ty_model_score(&d, score, &ub, NULL);
    ty_model_free(&d);
    if (rc == TY_OK) rc = ty_dl(lm, ub, &dl);
    if (lm_out) *lm_out = lm;
    return rc == TY_OK ? dl : -1;
}

static void test_model_code(void) {
    ty_stream a;
    CHECK(load(FIX_A, &a) == 0, "load A");
    const ty_stream *sv[1] = {&a};
    ty_model m, d;
    CHECK(ty_model_fit(&m, sv, 1, TY_F_OP | TY_F_PREV1, TY_OUT_K, TY_FIT_KEEP_ALL) == TY_OK, "fit");
    uint8_t *code = NULL;
    size_t nb = 0;
    uint64_t bits = 0, bits2 = 0;
    CHECK(ty_model_encode(&m, &code, &nb, &bits) == TY_OK, "encode");
    CHECK(bits == TY_MODEL_HEADER_BITS + 16 * 8 + m.nrows * (m.keybits + 16 * 8), "L(M) formula (%" PRIu64 ")",
          bits);
    CHECK(ty_model_decode(code, nb, &d, &bits2, NULL, 0) == TY_OK && bits2 == bits && d.nrows == m.nrows,
          "decode round trip");
    int same = memcmp(d.def, m.def, sizeof m.def) == 0;
    for (size_t i = 0; same && i < m.nrows; ++i)
        same = d.rows[i].key == m.rows[i].key && memcmp(d.rows[i].q, m.rows[i].q, sizeof m.rows[i].q) == 0;
    CHECK(same, "decoded table identical to the fitted table");
    int64_t s1, s2;
    ty_model_score(&m, &a, &s1, NULL);
    ty_model_score(&d, &a, &s2, NULL);
    CHECK(s1 == s2, "scored model = paid-for model");
    ty_model_free(&d);

    char why[160];
    uint8_t *t = malloc(nb);
#define MUTATE(off, val, expect, what)                                                         \
    do {                                                                                       \
        memcpy(t, code, nb);                                                                   \
        t[off] = (uint8_t)(val);                                                               \
        int r_ = ty_model_decode(t, nb, &d, NULL, why, sizeof why);                            \
        CHECK(r_ == (expect), "%s: got %s", what, ty_err_name(r_));                            \
        if (r_ == TY_OK) ty_model_free(&d);                                                    \
    } while (0)
    MUTATE(0, 'X', TY_E_FORMAT, "bad magic");
    MUTATE(4, 1, TY_E_PROFILE, "encoder version 1");
    MUTATE(7, 12, TY_E_PROFILE, "12-bit quantization");
    MUTATE(8, m.keybits + 1, TY_E_FORMAT, "key bits inconsistent with mask");
    memcpy(t, code, nb);
    t[13] = 0;
    t[14] = 0; /* first default entry = 0: below the floor */
    CHECK(ty_model_decode(t, nb, &d, NULL, why, sizeof why) == TY_E_PROFILE, "zero entry (floor) refused");
    CHECK(ty_model_decode(code, nb - 1, &d, NULL, why, sizeof why) == TY_E_FORMAT, "truncated code refused");
    free(t);
    free(code);
    ty_model_free(&m);
    ty_stream_free(&a);
}

/* A lookup table that memorizes (crumb ordinal, event_index) of the fit data. */
static void test_memorizer(void) {
    ty_stream a, b;
    CHECK(load(FIX_A, &a) == 0 && load(FIX_B, &b) == 0, "load");
    uint64_t lm_b, lm_m;
    int64_t dlb_held = dl_of(&a, &b, TY_F_PREV1, TY_FIT_MDL_PRUNE, 0, &lm_b);
    int64_t dlm_held = dl_of(&a, &b, TY_F_POS, TY_FIT_KEEP_ALL, 0, &lm_m);
    int64_t dlb_fit = dl_of(&a, &a, TY_F_PREV1, TY_FIT_MDL_PRUNE, 0, NULL);
    int64_t dlm_fit = dl_of(&a, &a, TY_F_POS, TY_FIT_KEEP_ALL, 0, NULL);
    int64_t t_held = dlb_held - dlm_held, t_fit = dlb_fit - dlm_fit;
    CHECK(lm_m > 1418u * 160u, "memorizer pays for every stored event (%" PRIu64 " bits)", lm_m);
    CHECK(t_held < 0, "memorizer held-out T must be negative (%" PRId64 " ub)", t_held);
    CHECK(t_fit < 0, "memorizer T is negative even on its own fit data once L(M) is paid (%" PRId64 ")", t_fit);
    fprintf(g_det, "memorizer lm=%" PRIu64 " t_held_ub=%" PRId64 " t_fit_ub=%" PRId64 "\n", lm_m, t_held, t_fit);
    ty_stream_free(&a);
    ty_stream_free(&b);
}

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* A structure present in the fit data and absent from the held-out data:
 * positive compression on the fit data, negative held-out T. */
static void test_train_only_gain(void) {
    ty_stream fit, held;
    ty_stream_init(&fit);
    ty_stream_init(&held);
    uint64_t seed = 12345;
    for (int c = 0; c < 20; ++c)
        for (int i = 0; i < 500; ++i) {
            ty_ev e = {0};
            e.first = i == 0;
            e.idx = (uint32_t)i;
            e.crumb = (uint16_t)c;
            e.op = (uint8_t)(i % 15);
            e.sym = (uint8_t)(i % TY_OUT_K); /* fit data: a deterministic cycle */
            ty_stream_push(&fit, &e);
            e.sym = (uint8_t)(splitmix(&seed) % TY_OUT_K); /* held-out: no cycle */
            ty_stream_push(&held, &e);
        }
    int64_t b_fit = dl_of(&fit, &fit, 0, TY_FIT_MDL_PRUNE, 0, NULL);
    int64_t c_fit = dl_of(&fit, &fit, TY_F_PREV1, TY_FIT_MDL_PRUNE, 0, NULL);
    int64_t b_held = dl_of(&fit, &held, 0, TY_FIT_MDL_PRUNE, 0, NULL);
    int64_t c_held = dl_of(&fit, &held, TY_F_PREV1, TY_FIT_MDL_PRUNE, 0, NULL);
    CHECK(b_fit - c_fit > 0, "candidate compresses its fit data (T_fit %" PRId64 ")", b_fit - c_fit);
    CHECK(b_held - c_held < 0, "same candidate has negative held-out T (%" PRId64 ")", b_held - c_held);
    fprintf(g_det, "train_only t_fit_ub=%" PRId64 " t_held_ub=%" PRId64 "\n", b_fit - c_fit, b_held - c_held);
    ty_stream_free(&fit);
    ty_stream_free(&held);
}

/* ------------------------------------------------ records, refusals, tamper */

static void fixture_profile(ty_yprofile *p, ty_split *sp) {
    ty_yprofile_v0(p);
    ty_digest a, b;
    ty_file_sha256(FIX_A, &a);
    ty_file_sha256(FIX_B, &b);
    /* manifest digest for the fixture profile = SHA-256 of the two fixture digests */
    uint8_t cat[64];
    memcpy(cat, a.b, 32);
    memcpy(cat + 32, b.b, 32);
    sha256_hash(cat, 64, p->manifest.b);
    memset(sp, 0, sizeof *sp);
    sp->nfit = 1;
    sp->nheld = 1;
    sp->fit[0] = a;
    sp->held[0] = b;
    snprintf(sp->fit_label[0], sizeof sp->fit_label[0], "fixture A");
    snprintf(sp->held_label[0], sizeof sp->held_label[0], "fixture B");
}

static void test_records(void) {
    ty_yprofile p;
    ty_split sp;
    fixture_profile(&p, &sp);
    char why[512] = "";
    const char *fitp[1] = {FIX_A}, *heldp[1] = {FIX_B};

    /* the real V0 profile digest is pinned (docs/turing/TURING_YIELD_PROFILE_V0.md) */
    {
        ty_yprofile r;
        ty_yprofile_v0(&r);
        ty_parse_hex(GOLD_MANIFEST_REAL, &r.manifest);
        ty_digest d;
        char h[65];
        ty_profile_digest(&r, &d);
        hexs(&d, h);
        CHECK(strcmp(h, GOLD_PROFILE_V0_REAL) == 0, "profile V0 digest drifted: %s", h);
        r.floor_q = 2;
        ty_profile_digest(&r, &d);
        hexs(&d, h);
        CHECK(strcmp(h, GOLD_PROFILE_V0_REAL) != 0, "profile digest covers the floor");
    }

    ty_fit_rec br, cr;
    uint8_t *bc = NULL, *cc = NULL;
    size_t nb = 0, nc = 0;
    CHECK(ty_fit_files(&p, &sp, fitp, sp.fit, 1, TY_ROLE_BASELINE, p.baseline_mask, &br, &bc, &nb, why,
                       sizeof why) == TY_OK,
          "fit baseline: %s", why);
    CHECK(ty_fit_files(&p, &sp, fitp, sp.fit, 1, TY_ROLE_CANDIDATE, p.candidate_mask, &cr, &cc, &nc, why,
                       sizeof why) == TY_OK,
          "fit candidate: %s", why);
    ty_gain_rec g;
    int rc = ty_gain_compute(&p, &sp, &br, bc, nb, &cr, cc, nc, heldp, &g, why, sizeof why);
    CHECK(rc == TY_OK, "gain on fixture: %s (%s)", why, ty_err_name(rc));
    ty_digest gd;
    ty_gain_digest(&g, &gd);
    CHECK(g.nsym == 1330 && g.dl_b_ub - g.dl_c_ub == g.t_ub, "T = DL(B) - DL(C)");
    CHECK(g.file_t_ub[0] == g.t_ub, "single held file: per-file T = pooled T");
    CHECK(g.qerr_bound_ub == (int64_t)((2 * 1330 * 501 + 999) / 1000), "fixed-point bound recorded");
    CHECK(strcmp(g.verdict, "FAIL") == 0, "fixture T is far below the V0 margin, so FAIL");
    rc = ty_gain_verify(&p, &sp, fitp, heldp, &g, &gd, why, sizeof why);
    CHECK(rc == TY_OK, "verify re-derives the fixture gain: %s", why);
    {
        char h[65];
        hexs(&gd, h);
        CHECK(strcmp(h, GOLD_FIXTURE_GAIN) == 0, "fixture turing.yield.v0 digest drifted: %s", h);
        fprintf(g_det, "fixture gain t_ub=%" PRId64 " lm_b=%" PRIu64 " lm_c=%" PRIu64 " digest=%s\n", g.t_ub,
                g.lm_b_bits, g.lm_c_bits, h);
    }

    /* 1. leakage: a candidate fit on the held-out file is refused via the split manifest */
    {
        ty_split leak = sp;
        leak.fit[0] = sp.held[0]; /* fit list names the held-out digest */
        ty_fit_rec lr;
        uint8_t *lc = NULL;
        size_t ln = 0;
        rc = ty_fit_files(&p, &sp, heldp, leak.fit, 1, TY_ROLE_CANDIDATE, p.candidate_mask, &lr, &lc, &ln, why,
                          sizeof why);
        CHECK(rc == TY_OK, "a model can be fit on held-out data (the refusal happens at scoring)");
        rc = ty_gain_compute(&p, &sp, &br, bc, nb, &lr, lc, ln, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_LEAK, "held-out-trained candidate refused: %s", ty_err_name(rc));
        /* a candidate that hides the leak (claims fixture A while fit on B) fails verification by refit */
        lr.fit[0] = sp.fit[0];
        rc = ty_gain_compute(&p, &sp, &br, bc, nb, &lr, lc, ln, heldp, &g, why, sizeof why);
        ty_digest ld;
        ty_gain_digest(&g, &ld);
        int vr = rc == TY_OK ? ty_gain_verify(&p, &sp, fitp, heldp, &g, &ld, why, sizeof why) : rc;
        CHECK(vr == TY_E_DIGEST, "hidden leak caught by refit: %s", ty_err_name(vr));
        free(lc);
    }
    /* 2. provenance: fit files not the split's fit files */
    {
        ty_fit_rec x = cr;
        x.fit[0].b[0] ^= 1;
        rc = ty_gain_compute(&p, &sp, &br, bc, nb, &x, cc, nc, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_PROVENANCE, "foreign fit file refused: %s", ty_err_name(rc));
    }
    /* 3. candidate made under a changed profile (estimator text, floor) */
    {
        ty_yprofile p2 = p;
        p2.floor_q = 2;
        ty_fit_rec x;
        uint8_t *xc = NULL;
        size_t xn = 0;
        ty_fit_files(&p2, &sp, fitp, sp.fit, 1, TY_ROLE_CANDIDATE, p.candidate_mask, &x, &xc, &xn, why, sizeof why);
        rc = ty_gain_compute(&p, &sp, &br, bc, nb, &x, xc, xn, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_PROFILE, "candidate under another floor refused: %s", ty_err_name(rc));
        free(xc);
        rc = ty_gain_compute(&p2, &sp, &br, bc, nb, &cr, cc, nc, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_PROFILE, "scorer refuses a profile whose floor it does not implement: %s",
              ty_err_name(rc));
    }
    /* 4. candidate that changes the encoder / quantization / floor inside its own code (digest kept consistent) */
    {
        const struct {
            int off;
            uint8_t v;
            const char *what;
        } mut[] = {{4, 1, "encoder version"}, {7, 12, "quantization bits"}, {13, 0, "floor (zero entry)"}};
        for (size_t i = 0; i < 3; ++i) {
            uint8_t *t = malloc(nc);
            memcpy(t, cc, nc);
            t[mut[i].off] = mut[i].v;
            if (mut[i].off == 13) t[14] = 0;
            ty_fit_rec x = cr;
            ty_model_digest(t, nc, x.model.b);
            rc = ty_gain_compute(&p, &sp, &br, bc, nb, &x, t, nc, heldp, &g, why, sizeof why);
            CHECK(rc == TY_E_PROFILE, "candidate changing %s refused: %s", mut[i].what, ty_err_name(rc));
            free(t);
        }
    }
    /* 5. baseline other than the declared one */
    {
        ty_fit_rec x;
        uint8_t *xc = NULL;
        size_t xn = 0;
        ty_fit_files(&p, &sp, fitp, sp.fit, 1, TY_ROLE_BASELINE, 0 /* order-0 */, &x, &xc, &xn, why, sizeof why);
        rc = ty_gain_compute(&p, &sp, &x, xc, xn, &cr, cc, nc, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_BASELINE, "weaker baseline refused: %s", ty_err_name(rc));
        free(xc);
    }
    /* 6. tampered baseline and candidate model codes */
    {
        uint8_t *t = malloc(nb);
        memcpy(t, bc, nb);
        t[nb - 1] ^= 0x80;
        rc = ty_gain_compute(&p, &sp, &br, t, nb, &cr, cc, nc, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "tampered baseline code refused: %s", ty_err_name(rc));
        free(t);
        t = malloc(nc);
        memcpy(t, cc, nc);
        t[20] ^= 0x01;
        rc = ty_gain_compute(&p, &sp, &br, bc, nb, &cr, t, nc, heldp, &g, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "tampered candidate code refused: %s", ty_err_name(rc));
        free(t);
    }
    /* 7. tampered dataset: a valid-looking edit of one held-out record */
    {
        char tp[600];
        tmp_path(tp, sizeof tp, "held_tamper.ctr");
        copy_file(FIX_B, tp);
        FILE *f = fopen(FIX_B, "rb");
        uint8_t r[TY_CTR1_BYTES];
        long idx = -1;
        for (long i = 0; fread(r, 1, TY_CTR1_BYTES, f) == TY_CTR1_BYTES; ++i)
            if (r[6] == 1 && r[23] == 0 && r[7] == 2) {
                idx = i;
                break;
            }
        fclose(f);
        patch_byte(tp, idx * TY_CTR1_BYTES + 7, 4); /* Failed -> Improved, still a valid record */
        const char *tpp[1] = {tp};
        ty_stream s;
        CHECK(idx >= 0 && load(tp, &s) == 0, "tampered record is still well-formed");
        ty_stream_free(&s);
        rc = ty_gain_compute(&p, &sp, &br, bc, nb, &cr, cc, nc, tpp, &g, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "tampered held-out file refused: %s", ty_err_name(rc));
        ty_gain_compute(&p, &sp, &br, bc, nb, &cr, cc, nc, heldp, &g, why, sizeof why);
        ty_gain_digest(&g, &gd);
        rc = ty_gain_verify(&p, &sp, fitp, tpp, &g, &gd, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "verify against a tampered dataset fails: %s", ty_err_name(rc));
        const char *tfp[1] = {tp};
        rc = ty_gain_verify(&p, &sp, tfp, heldp, &g, &gd, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "verify with a tampered fit file fails: %s", ty_err_name(rc));
        remove(tp);
    }
    /* 8. tampered receipt: edited T, and an edited T with a recomputed digest */
    {
        ty_gain_compute(&p, &sp, &br, bc, nb, &cr, cc, nc, heldp, &g, why, sizeof why);
        ty_gain_digest(&g, &gd);
        ty_gain_rec x = g;
        x.t_ub += 1000000;
        rc = ty_gain_verify(&p, &sp, fitp, heldp, &x, &gd, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "receipt with edited T fails: %s", ty_err_name(rc));
        ty_digest xd;
        ty_gain_digest(&x, &xd);
        rc = ty_gain_verify(&p, &sp, fitp, heldp, &x, &xd, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "self-consistent forged receipt fails re-derivation: %s", ty_err_name(rc));
        x = g;
        snprintf(x.verdict, sizeof x.verdict, "PASS");
        ty_gain_digest(&x, &xd);
        rc = ty_gain_verify(&p, &sp, fitp, heldp, &x, &xd, why, sizeof why);
        CHECK(rc == TY_E_DIGEST, "forged PASS verdict fails: %s", ty_err_name(rc));
    }
    free(bc);
    free(cc);
}

int main(int argc, char **argv) {
    snprintf(g_prefix, sizeof g_prefix, "%s", argc > 1 ? argv[1] : "build/test_ty_math");
    char dp[600];
    snprintf(dp, sizeof dp, "%s.det", g_prefix);
    g_det = fopen(dp, "w");
    if (!g_det) {
        printf("cannot open %s\n", dp);
        return 2;
    }
    test_math();
    test_reader();
    test_model_code();
    test_memorizer();
    test_train_only_gain();
    test_records();
    fclose(g_det);
    printf("test_ty_math: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
