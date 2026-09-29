/* EXP-001 coder tests: TPS1/TSY1 formats, both reference coders, round trips on
 * random and adversarial distributions, and fail-closed refusals.
 *
 * Built WITHOUT ty_model.c (mk/turing_exp001_b.mk): the coders cannot reach
 * the model code. argv[1] = output prefix; <prefix>.det receives a determinism
 * printout that must be byte-identical across the plain and ASan/UBSan builds.
 */
#include "turing/tc_pstream.h"
#include "turing/tc_range.h"
#include "turing/tc_rans.h"
#include "turing/ty_math.h"

#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
static FILE *g_det;
#define CHECK(cond, ...)                                \
    do {                                                \
        if (cond) {                                     \
            ++g_pass;                                   \
        } else {                                        \
            ++g_fail;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static char why[512];
static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Row shapes. */
enum { SH_RANDOM, SH_SKEW_FIRST, SH_SKEW_LAST, SH_UNIFORM, SH_SPIKY };
/* Symbol pickers. */
enum { PK_SAMPLE, PK_RAREST, PK_LAST, PK_FIRST };

static void make_row(uint16_t *q, unsigned K, int shape) {
    uint32_t left = TC_QONE;
    switch (shape) {
    case SH_SKEW_FIRST:
        for (unsigned x = 1; x < K; ++x) q[x] = 1;
        q[0] = (uint16_t)(TC_QONE - (K - 1));
        return;
    case SH_SKEW_LAST:
        for (unsigned x = 0; x + 1 < K; ++x) q[x] = 1;
        q[K - 1] = (uint16_t)(TC_QONE - (K - 1));
        return;
    case SH_UNIFORM:
        for (unsigned x = 0; x < K; ++x) q[x] = (uint16_t)(TC_QONE / K);
        q[0] = (uint16_t)(q[0] + TC_QONE % K);
        return;
    default: break;
    }
    /* random weights with floor 1; SH_SPIKY puts most mass on one random symbol */
    uint32_t w[TC_KMAX], tot = 0;
    for (unsigned x = 0; x < K; ++x) {
        w[x] = (uint32_t)(rnd() % 1000) + 1;
        if (shape == SH_SPIKY && rnd() % 4 == 0) w[x] = 1;
        tot += w[x];
    }
    if (shape == SH_SPIKY) {
        w[rnd() % K] += 100000;
        tot = 0;
        for (unsigned x = 0; x < K; ++x) tot += w[x];
    }
    uint32_t spare = TC_QONE - K; /* each entry gets 1 + share of spare */
    for (unsigned x = 0; x < K; ++x) {
        uint32_t v = 1 + (uint32_t)((uint64_t)spare * w[x] / tot);
        q[x] = (uint16_t)v;
        left -= v;
    }
    for (unsigned x = 0; left; x = (x + 1) % K) {
        if (q[x] < 65535) {
            q[x]++;
            left--;
        }
    }
}

static unsigned pick(const uint16_t *q, unsigned K, int picker) {
    if (picker == PK_LAST) return K - 1;
    if (picker == PK_FIRST) return 0;
    if (picker == PK_RAREST) {
        unsigned b = 0;
        for (unsigned x = 1; x < K; ++x)
            if (q[x] < q[b]) b = x;
        return b;
    }
    uint32_t u = (uint32_t)(rnd() & 0xFFFF), c = 0;
    for (unsigned x = 0; x < K; ++x) {
        c += q[x];
        if (u < c) return x;
    }
    return K - 1;
}

static void fill_digest(uint8_t d[32], uint8_t tag) {
    for (int i = 0; i < 32; ++i) d[i] = (uint8_t)(tag * 31 + i);
}

static void make_pair(tc_pstream *p, tc_symbols *s, unsigned K, uint64_t n, int shape, int picker) {
    tc_ps_alloc(p, K, n);
    tc_sy_alloc(s, K, n);
    fill_digest(p->profile, 1), fill_digest(p->model, 2), fill_digest(p->dataset, 3), fill_digest(s->dataset, 3);
    for (uint64_t t = 0; t < n; ++t) {
        uint16_t *q = p->q + t * K;
        make_row(q, K, shape);
        p->key[t] = t * 7;
        p->crumb[t] = (uint32_t)(t / 1000);
        s->sym[t] = (uint8_t)pick(q, K, picker);
    }
}

static uint64_t g_carries, g_pending_max;

/* Full file-level round trip for both coders; returns 1 on success. Reports overhead. */
static int round_trip(tc_pstream *p0, tc_symbols *s0, const char *label, double max_ovh_per_sym) {
    uint8_t *pb, *sb;
    size_t pn, sn;
    tc_pstream p;
    tc_symbols s;
    int ok = tc_ps_serialize(p0, &pb, &pn, why, sizeof why) == TC_OK &&
             tc_sy_serialize(s0, &sb, &sn, why, sizeof why) == TC_OK && tc_ps_parse(pb, pn, &p, why, sizeof why) == TC_OK &&
             tc_sy_parse(sb, sn, &s, why, sizeof why) == TC_OK;
    CHECK(ok, "%s: canonical serialize/parse: %s", label, why);
    if (!ok) return 0;
    int64_t ub;
    tc_ideal_ub(&p, &s, 0, p.n, &ub);
    for (int c = 1; c <= 2; ++c) {
        uint8_t *cb, *db;
        size_t cn, dn;
        tc_symbols d;
        int rc = c == 1 ? tc_range_encode(&p, &s, &cb, &cn, why, sizeof why) : tc_rans_encode(&p, &s, &cb, &cn, why, sizeof why);
        CHECK(rc == TC_OK, "%s coder %d encode: %s %s", label, c, tc_err_name(rc), why);
        if (rc != TC_OK) continue;
        rc = c == 1 ? tc_range_decode(&p, cb, cn, &d, why, sizeof why) : tc_rans_decode(&p, cb, cn, &d, why, sizeof why);
        CHECK(rc == TC_OK, "%s coder %d decode: %s %s", label, c, tc_err_name(rc), why);
        if (rc == TC_OK) {
            CHECK(tc_sy_serialize(&d, &db, &dn, why, sizeof why) == TC_OK && dn == sn && memcmp(db, sb, sn) == 0,
                  "%s coder %d: decoded TSY1 not byte-exact", label, c);
            free(db);
            tc_sy_free(&d);
        }
        /* accounting: every byte counted; payload overhead bounded */
        double ovh = (8.0 * (double)(cn - TC_CODED_HEADER) * 1e6 - (double)ub) / 1e6;
        CHECK(ovh <= 64.0 + max_ovh_per_sym * (double)p.n, "%s coder %d: payload overhead %.3f bits on n=%" PRIu64,
              label, c, ovh, p.n);
        uint8_t dg[32];
        char h[65];
        sha256_hash(cb, cn, dg);
        tc_hex(dg, h);
        fprintf(g_det, "%s coder=%d n=%" PRIu64 " bytes=%zu sha=%s\n", label, c, p.n, cn, h);
        free(cb);
    }
    if (p.n) {
        tc_range_stats st;
        uint8_t *x;
        size_t nx;
        if (tc_range_encode_raw(p.q, p.K, s.sym, p.n, &x, &nx, &st) == TC_OK) {
            g_carries += st.carries;
            if (st.pending_max > g_pending_max) g_pending_max = st.pending_max;
            free(x);
        }
    }
    char h[65];
    tc_hex(p.digest, h);
    fprintf(g_det, "%s tps1=%s ideal_ub=%" PRId64 "\n", label, h, ub);
    free(pb), free(sb), tc_ps_free(&p), tc_sy_free(&s);
    return 1;
}

static void test_round_trips(void) {
    static const uint64_t ns[] = {0, 1, 2, 17, 1000, 20000};
    char label[128];
    for (unsigned K = 2; K <= 16; ++K)
        for (size_t i = 0; i < sizeof ns / sizeof *ns; ++i) {
            tc_pstream p;
            tc_symbols s;
            make_pair(&p, &s, K, ns[i], SH_RANDOM, PK_SAMPLE);
            snprintf(label, sizeof label, "random K=%u n=%" PRIu64, K, ns[i]);
            round_trip(&p, &s, label, 0.01);
            tc_ps_free(&p), tc_sy_free(&s);
        }
    struct {
        unsigned K;
        uint64_t n;
        int shape, picker;
        const char *name;
    } adv[] = {
        {2, 50000, SH_SKEW_FIRST, PK_RAREST, "skew K=2 rare-run (p=2^-16 each)"},
        {16, 50000, SH_SKEW_FIRST, PK_RAREST, "skew K=16 rare-run"},
        {2, 300000, SH_SKEW_LAST, PK_LAST, "skew K=2 dominant-last run (carry pressure)"},
        {16, 300000, SH_SKEW_LAST, PK_LAST, "skew K=16 dominant-last run"},
        {9, 300000, SH_SKEW_FIRST, PK_FIRST, "skew K=9 dominant-first run"},
        {9, 50000, SH_SKEW_LAST, PK_FIRST, "skew K=9 floor symbol 0 run"},
        {16, 200000, SH_SPIKY, PK_SAMPLE, "spiky K=16 sampled"},
        {9, 200000, SH_SPIKY, PK_RAREST, "spiky K=9 rarest"},
        {9, 200000, SH_UNIFORM, PK_SAMPLE, "uniform K=9"},
        {16, 200000, SH_RANDOM, PK_SAMPLE, "random K=16 long"},
    };
    for (size_t i = 0; i < sizeof adv / sizeof *adv; ++i) {
        tc_pstream p;
        tc_symbols s;
        make_pair(&p, &s, adv[i].K, adv[i].n, adv[i].shape, adv[i].picker);
        round_trip(&p, &s, adv[i].name, 0.01);
        tc_ps_free(&p), tc_sy_free(&s);
    }
    /* Mixed: runs of dominant-last symbols interleaved with random rows (exercises 0xFF runs + carries). */
    {
        tc_pstream p;
        tc_symbols s;
        unsigned K = 9;
        uint64_t n = 400000;
        tc_ps_alloc(&p, K, n), tc_sy_alloc(&s, K, n);
        fill_digest(p.profile, 1), fill_digest(p.model, 2), fill_digest(p.dataset, 3), fill_digest(s.dataset, 3);
        for (uint64_t t = 0; t < n; ++t) {
            int run = (t / 997) % 2;
            make_row(p.q + t * K, K, run ? SH_SKEW_LAST : SH_RANDOM);
            s.sym[t] = (uint8_t)pick(p.q + t * K, K, run ? PK_LAST : PK_SAMPLE);
        }
        round_trip(&p, &s, "mixed runs K=9", 0.01);
        tc_ps_free(&p), tc_sy_free(&s);
    }
    CHECK(g_carries > 0, "range coder carry propagation never exercised");
    CHECK(g_pending_max > 1, "range coder never deferred a run of 0xFF bytes (max %" PRIu64 ")", g_pending_max);
    fprintf(g_det, "range carries=%" PRIu64 " pending_max=%" PRIu64 "\n", g_carries, g_pending_max);
}

/* ---------------------------------------------------------- fail-closed */

/* Recompute a TPS1/TSY1 trailer after a deliberate edit (an attacker who re-hashes). */
static void redigest(uint8_t *b, size_t n, const char *dom) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)dom, strlen(dom) + 1);
    sha256_update(&ctx, b, n - 32);
    sha256_final(&ctx, b + n - 32);
}

typedef struct {
    tc_pstream p;
    tc_symbols s;
    uint8_t *pb, *sb, *cr, *ca;
    size_t pn, sn, crn, can;
} base;

static void base_make(base *B) {
    tc_pstream p0;
    tc_symbols s0;
    make_pair(&p0, &s0, 9, 5000, SH_RANDOM, PK_SAMPLE);
    tc_ps_serialize(&p0, &B->pb, &B->pn, why, sizeof why);
    tc_sy_serialize(&s0, &B->sb, &B->sn, why, sizeof why);
    tc_ps_free(&p0), tc_sy_free(&s0);
    tc_ps_parse(B->pb, B->pn, &B->p, why, sizeof why);
    tc_sy_parse(B->sb, B->sn, &B->s, why, sizeof why);
    tc_range_encode(&B->p, &B->s, &B->cr, &B->crn, why, sizeof why);
    tc_rans_encode(&B->p, &B->s, &B->ca, &B->can, why, sizeof why);
}

static void base_free(base *B) {
    tc_ps_free(&B->p), tc_sy_free(&B->s);
    free(B->pb), free(B->sb), free(B->cr), free(B->ca);
}

static uint8_t *dup(const uint8_t *b, size_t n) {
    uint8_t *d = malloc(n ? n : 1);
    memcpy(d, b, n);
    return d;
}

static int decode_c(int c, const tc_pstream *p, const uint8_t *b, size_t n) {
    tc_symbols d;
    why[0] = 0;
    int rc = c == 1 ? tc_range_decode(p, b, n, &d, why, sizeof why) : tc_rans_decode(p, b, n, &d, why, sizeof why);
    if (rc == TC_OK) tc_sy_free(&d);
    return rc;
}

#define REFUSED(expr, want, label)                                                                       \
    do {                                                                                                 \
        why[0] = 0;                                                                                      \
        int rc_ = (expr);                                                                                \
        CHECK(rc_ == (want), "%s: got %s, want %s", label, tc_err_name(rc_), tc_err_name(want));         \
        CHECK(rc_ == TC_OK || why[0] != 0, "%s: refusal without a reason", label);                       \
        fprintf(g_det, "refuse %-44s %s | %s\n", label, tc_err_name(rc_), why);                          \
    } while (0)

static void test_fail_closed(void) {
    base B;
    base_make(&B);
    size_t rb = TC_PS_REC_FIXED + 2 * 9;
    tc_pstream p;
    tc_symbols s;

    /* sanity: the base decodes */
    CHECK(decode_c(1, &B.p, B.cr, B.crn) == TC_OK && decode_c(2, &B.p, B.ca, B.can) == TC_OK, "base decode");

    /* 1. corrupted probability stream: one byte flipped in a record */
    uint8_t *x = dup(B.pb, B.pn);
    x[TC_PS_HEADER + 3 * rb + 24] ^= 1;
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_DIGEST, "corrupted probability stream");
    /* 1b. same edit, re-digested but still normalized: coded files are bound to the old digest */
    free(x);
    x = dup(B.pb, B.pn);
    {
        uint8_t *r = x + TC_PS_HEADER + 3 * rb + 24;
        uint16_t q0 = tc_get_u16(r), q1 = tc_get_u16(r + 2);
        if (q0 > 1) tc_put_u16(r, (uint16_t)(q0 - 1)), tc_put_u16(r + 2, (uint16_t)(q1 + 1));
        else tc_put_u16(r, (uint16_t)(q0 + 1)), tc_put_u16(r + 2, (uint16_t)(q1 - 1));
    }
    redigest(x, B.pn, TC_PS_DOMAIN);
    CHECK(tc_ps_parse(x, B.pn, &p, why, sizeof why) == TC_OK, "re-digested altered stream parses");
    REFUSED(decode_c(1, &p, B.cr, B.crn), TC_E_BINDING, "altered+rehashed stream, range decode");
    REFUSED(decode_c(2, &p, B.ca, B.can), TC_E_BINDING, "altered+rehashed stream, rANS decode");
    tc_ps_free(&p);
    free(x);

    /* 2. truncated bitstream */
    REFUSED(decode_c(1, &B.p, B.cr, B.crn - 1), TC_E_TRUNC, "range truncated by 1 byte");
    REFUSED(decode_c(2, &B.p, B.ca, B.can - 1), TC_E_TRUNC, "rANS truncated by 1 byte");
    for (int c = 1; c <= 2; ++c) {
        size_t n = c == 1 ? B.crn : B.can;
        x = dup(c == 1 ? B.cr : B.ca, n);
        tc_put_u64(x + 48, n - TC_CODED_HEADER - 1); /* header lies to match */
        REFUSED(decode_c(c, &B.p, x, n - 1), TC_E_TRUNC, c == 1 ? "range truncated, header adjusted" : "rANS truncated, header adjusted");
        tc_put_u64(x + 48, 2);
        REFUSED(decode_c(c, &B.p, x, TC_CODED_HEADER + 2), TC_E_TRUNC, c == 1 ? "range payload 2 bytes" : "rANS payload 2 bytes");
        free(x);
        /* extra trailing byte, header adjusted */
        x = malloc(n + 1);
        memcpy(x, c == 1 ? B.cr : B.ca, n);
        x[n] = 0;
        REFUSED(decode_c(c, &B.p, x, n + 1), TC_E_TRAIL, c == 1 ? "range extra byte" : "rANS extra byte");
        tc_put_u64(x + 48, n - TC_CODED_HEADER + 1);
        REFUSED(decode_c(c, &B.p, x, n + 1), TC_E_TRAIL, c == 1 ? "range extra byte, header adjusted" : "rANS extra byte, header adjusted");
        free(x);
    }

    /* 3. wrong model / profile / dataset digest */
    uint8_t bad[32];
    fill_digest(bad, 99);
    uint8_t pr[32], mo[32], da[32];
    fill_digest(pr, 1), fill_digest(mo, 2), fill_digest(da, 3);
    REFUSED(tc_ps_expect(&B.p, pr, mo, da, why, sizeof why), TC_OK, "all expected digests match");
    REFUSED(tc_ps_expect(&B.p, pr, bad, da, why, sizeof why), TC_E_MODEL, "wrong model digest");
    REFUSED(tc_ps_expect(&B.p, bad, mo, da, why, sizeof why), TC_E_PROFILE, "wrong profile digest");
    REFUSED(tc_ps_expect(&B.p, pr, mo, bad, why, sizeof why), TC_E_DATASET, "wrong dataset digest");
    /* TSY1 names a different dataset than the TPS1 */
    x = dup(B.sb, B.sn);
    x[24] ^= 0x80;
    redigest(x, B.sn, TC_SY_DOMAIN);
    CHECK(tc_sy_parse(x, B.sn, &s, why, sizeof why) == TC_OK, "rehashed TSY1 parses");
    uint8_t *cb;
    size_t cn;
    REFUSED(tc_range_encode(&B.p, &s, &cb, &cn, why, sizeof why), TC_E_DATASET, "TSY1/TPS1 dataset mismatch (range)");
    REFUSED(tc_rans_encode(&B.p, &s, &cb, &cn, why, sizeof why), TC_E_DATASET, "TSY1/TPS1 dataset mismatch (rANS)");
    tc_sy_free(&s);
    free(x);

    /* 4. malformed normalization and zero-probability entries (re-digested, so only the row check can catch them) */
    x = dup(B.pb, B.pn);
    tc_put_u16(x + TC_PS_HEADER + 10 * rb + 24, (uint16_t)(tc_get_u16(x + TC_PS_HEADER + 10 * rb + 24) + 1));
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_NORM, "row sum 65537");
    free(x);
    x = dup(B.pb, B.pn);
    tc_put_u32(x + TC_PS_HEADER + 10 * rb + 20, 65535);
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_NORM, "norm_sum receipt 65535");
    free(x);
    x = dup(B.pb, B.pn);
    {
        uint8_t *r = x + TC_PS_HEADER + 11 * rb + 24;
        uint16_t q0 = tc_get_u16(r), q1 = tc_get_u16(r + 2);
        tc_put_u16(r, 0);
        tc_put_u16(r + 2, (uint16_t)(q1 + q0)); /* sum still 65536 */
    }
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_ZERO, "zero-probability entry");
    free(x);
    /* in-memory streams: serializer and both raw coders refuse too */
    {
        tc_pstream q;
        tc_symbols t;
        make_pair(&q, &t, 4, 10, SH_RANDOM, PK_SAMPLE);
        q.q[5 * 4 + 1] = (uint16_t)(q.q[5 * 4 + 1] + 1);
        uint8_t *b;
        size_t n;
        REFUSED(tc_ps_serialize(&q, &b, &n, why, sizeof why), TC_E_NORM, "serialize refuses sum != 65536");
        uint8_t *o;
        size_t on;
        CHECK(tc_range_encode_raw(q.q, 4, t.sym, 10, &o, &on, NULL) == TC_E_NORM, "range raw refuses bad row");
        CHECK(tc_rans_encode_raw(q.q, 4, t.sym, 10, &o, &on) == TC_E_NORM, "rANS raw refuses bad row");
        q.q[5 * 4 + 1] = (uint16_t)(q.q[5 * 4 + 1] - 1);
        q.q[5 * 4 + 1] = (uint16_t)(q.q[5 * 4 + 1] + q.q[5 * 4 + 2]);
        q.q[5 * 4 + 2] = 0;
        REFUSED(tc_ps_serialize(&q, &b, &n, why, sizeof why), TC_E_ZERO, "serialize refuses zero entry");
        CHECK(tc_rans_encode_raw(q.q, 4, t.sym, 10, &o, &on) == TC_E_ZERO, "rANS raw refuses zero entry");
        CHECK(tc_range_encode_raw(q.q, 4, t.sym, 10, &o, &on, NULL) == TC_E_NORM, "range raw refuses zero entry");
        tc_ps_free(&q), tc_sy_free(&t);
    }

    /* 5. header omitted / altered */
    for (int c = 1; c <= 2; ++c) {
        const uint8_t *cf = c == 1 ? B.cr : B.ca;
        size_t n = c == 1 ? B.crn : B.can;
        const char *nm = c == 1 ? "range" : "rANS";
        char lab[96];
        snprintf(lab, sizeof lab, "%s header omitted", nm);
        REFUSED(decode_c(c, &B.p, cf + TC_CODED_HEADER, n - TC_CODED_HEADER), TC_E_HEADER, lab);
        static const struct {
            size_t off;
            uint8_t xr;
            int want;
            const char *what;
        } alt[] = {{0, 0x20, TC_E_HEADER, "magic"},       {4, 0x02, TC_E_HEADER, "version"},
                   {5, 0x03, TC_E_HEADER, "coder id"},    {6, 0x01, TC_E_HEADER, "reserved"},
                   {8, 0x01, TC_E_COUNT, "symbol count"}, {20, 0x10, TC_E_BINDING, "TPS1 digest"},
                   {48, 0x01, TC_E_TRUNC, "payload length"}};
        for (size_t i = 0; i < sizeof alt / sizeof *alt; ++i) {
            x = dup(cf, n);
            x[alt[i].off] ^= alt[i].xr;
            snprintf(lab, sizeof lab, "%s header altered: %s", nm, alt[i].what);
            int want = alt[i].want;
            if (alt[i].off == 48 && (tc_get_u64(x + 48) < n - TC_CODED_HEADER)) want = TC_E_TRAIL;
            REFUSED(decode_c(c, &B.p, x, n), want, lab);
            free(x);
        }
        /* range file handed to the rANS decoder and vice versa */
        snprintf(lab, sizeof lab, "%s file given to the other decoder", nm);
        REFUSED(decode_c(3 - c, &B.p, cf, n), TC_E_HEADER, lab);
    }
    /* accounting: the coded file is exactly header + payload, nothing hidden elsewhere */
    {
        uint8_t *o;
        size_t on;
        tc_range_encode_raw(B.p.q, 9, B.s.sym, B.s.n, &o, &on, NULL);
        CHECK(B.crn == TC_CODED_HEADER + on, "range coded length != header + payload");
        free(o);
        tc_rans_encode_raw(B.p.q, 9, B.s.sym, B.s.n, &o, &on);
        CHECK(B.can == TC_CODED_HEADER + on, "rANS coded length != header + payload");
        free(o);
    }

    /* 6. symbol count mismatch: TSY1 one symbol short (a valid, re-serialized file) */
    {
        tc_symbols t;
        tc_sy_alloc(&t, 9, B.s.n - 1);
        memcpy(t.sym, B.s.sym, B.s.n - 1);
        memcpy(t.dataset, B.s.dataset, 32);
        REFUSED(tc_range_encode(&B.p, &t, &cb, &cn, why, sizeof why), TC_E_COUNT, "symbol count mismatch (range)");
        REFUSED(tc_rans_encode(&B.p, &t, &cb, &cn, why, sizeof why), TC_E_COUNT, "symbol count mismatch (rANS)");
        tc_sy_free(&t);
        x = dup(B.sb, B.sn);
        tc_put_u64(x + 8, B.s.n + 1);
        redigest(x, B.sn, TC_SY_DOMAIN);
        REFUSED(tc_sy_parse(x, B.sn, &t, why, sizeof why), TC_E_COUNT, "TSY1 count field disagrees with size");
        free(x);
        x = dup(B.sb, B.sn);
        x[TC_SY_HEADER + 7] = 9; /* outside K=9 */
        redigest(x, B.sn, TC_SY_DOMAIN);
        REFUSED(tc_sy_parse(x, B.sn, &t, why, sizeof why), TC_E_SYMBOL, "TSY1 symbol outside alphabet");
        free(x);
    }

    /* 7. TPS1 structure: index, count, truncation, version */
    x = dup(B.pb, B.pn);
    tc_put_u64(x + TC_PS_HEADER + 4 * rb, 5);
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_INDEX, "observation_index out of order");
    free(x);
    x = dup(B.pb, B.pn);
    tc_put_u64(x + 12, B.p.n - 1);
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_FORMAT, "TPS1 count field disagrees with size");
    free(x);
    REFUSED(tc_ps_parse(B.pb, B.pn - rb, &p, why, sizeof why), TC_E_DIGEST, "TPS1 truncated by one record");
    x = dup(B.pb, B.pn);
    x[4] = 2;
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_FORMAT, "TPS1 version 2");
    free(x);
    x = dup(B.pb, B.pn);
    x[7] = 15;
    redigest(x, B.pn, TC_PS_DOMAIN);
    REFUSED(tc_ps_parse(x, B.pn, &p, why, sizeof why), TC_E_FORMAT, "TPS1 qbits 15");
    free(x);

    /* 8. payload corruption: every single-byte flip is refused or decodes to different symbols */
    for (int c = 1; c <= 2; ++c) {
        const uint8_t *cf = c == 1 ? B.cr : B.ca;
        size_t n = c == 1 ? B.crn : B.can;
        unsigned refused = 0, differ = 0, silent = 0;
        for (size_t off = TC_CODED_HEADER; off < n; off += 7) {
            x = dup(cf, n);
            x[off] ^= (uint8_t)(1u << (off % 8));
            tc_symbols d;
            int rc = c == 1 ? tc_range_decode(&B.p, x, n, &d, why, sizeof why) : tc_rans_decode(&B.p, x, n, &d, why, sizeof why);
            if (rc != TC_OK)
                ++refused;
            else {
                if (memcmp(d.sym, B.s.sym, B.s.n) != 0) ++differ;
                else ++silent;
                tc_sy_free(&d);
            }
            free(x);
        }
        CHECK(silent == 0, "coder %d: %u payload flips decoded to the original symbols", c, silent);
        CHECK(refused > 0, "coder %d: no payload flip was refused by the termination check", c);
        fprintf(g_det, "payload flips coder=%d refused=%u decoded_different=%u silent=%u\n", c, refused, differ, silent);
    }
    base_free(&B);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_tc <output prefix>\n");
        return 2;
    }
    char path[600];
    snprintf(path, sizeof path, "%s.det", argv[1]);
    g_det = fopen(path, "w");
    if (!g_det) return 2;
    test_round_trips();
    test_fail_closed();
    fclose(g_det);
    printf("test_tc: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
