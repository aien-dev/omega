/* Tests for the strict-JSON receipt rules (est-json-1, docs/estimation/RECEIPT_JSON.md).
 * Synthetic statistics only; opens no run data.
 *   test_est_json                run the unit checks
 *   test_est_json --validate F   exit 0 when file F is strict RFC 8259 JSON, else 1
 * jq is NOT used as the validator: jq 1.7 accepts the bare tokens inf and nan. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est3c_common.h"
#include "est_replay.h"
#include "sha256.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------------------------------------------------------- strict validator */
typedef struct { const char *p, *end; int depth; } jp;
static void ws(jp *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int lit(jp *j, const char *w) { size_t n = strlen(w); if ((size_t)(j->end - j->p) < n || memcmp(j->p, w, n)) return 0; j->p += n; return 1; }
static int isd(const jp *j) { return j->p < j->end && *j->p >= '0' && *j->p <= '9'; }
static int jstring(jp *j)
{
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') return 1;
        if (c < 0x20) return 0;
        if (c == '\\') {
            if (j->p >= j->end) return 0;
            char e = *j->p++;
            if (e == 'u') { for (int k = 0; k < 4; k++) { if (j->p >= j->end || !strchr("0123456789abcdefABCDEF", *j->p)) return 0; j->p++; } }
            else if (!strchr("\"\\/bfnrt", e)) return 0;
        }
    }
    return 0;
}
static int jnumber(jp *j)
{
    if (j->p < j->end && *j->p == '-') j->p++;
    if (!isd(j)) return 0;
    if (*j->p == '0') j->p++; else while (isd(j)) j->p++;
    if (j->p < j->end && *j->p == '.') { j->p++; if (!isd(j)) return 0; while (isd(j)) j->p++; }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        j->p++; if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
        if (!isd(j)) return 0;
        while (isd(j)) j->p++;
    }
    return 1;
}
static int jvalue(jp *j)
{
    if (++j->depth > 64) return 0;
    ws(j);
    int ok = 0;
    if (j->p >= j->end) ok = 0;
    else if (*j->p == '{') {
        j->p++; ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; ok = 1; }
        else for (;;) {
            ws(j); if (!jstring(j)) break;
            ws(j); if (j->p >= j->end || *j->p++ != ':') break;
            if (!jvalue(j)) break;
            ws(j); if (j->p >= j->end) break;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == '}') { j->p++; ok = 1; }
            break;
        }
    } else if (*j->p == '[') {
        j->p++; ws(j);
        if (j->p < j->end && *j->p == ']') { j->p++; ok = 1; }
        else for (;;) {
            if (!jvalue(j)) break;
            ws(j); if (j->p >= j->end) break;
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; ok = 1; }
            break;
        }
    } else if (*j->p == '"') ok = jstring(j);
    else if (*j->p == 't') ok = lit(j, "true");
    else if (*j->p == 'f') ok = lit(j, "false");
    else if (*j->p == 'n') ok = lit(j, "null");
    else ok = jnumber(j);
    j->depth--;
    return ok;
}
static int strict_json(const char *s, size_t n)
{
    jp j = { s, s + n, 0 };
    if (!jvalue(&j)) return 0;
    ws(&j);
    return j.p == j.end;
}
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0; char *b = malloc(cap);
    for (;;) {
        if (len + 1 >= cap) b = realloc(b, cap *= 2);
        size_t r = fread(b + len, 1, cap - len - 1, f);
        if (!r) break;
        len += r;
    }
    fclose(f); b[len] = 0; *n = len;
    return b;
}

/* -------------------------------------------------------------------- tests */
static void fill_ok(c3_stats *st)
{
    memset(st, 0, sizeof *st);
    st->n = 2651; st->logscore = -1.5; st->width_mean = 800; st->width_median = 300; st->calibrated = 1; st->first_fail = -1; st->ten_n = 2641;
    for (int i = 0; i < C3_ST_COUNT; i++) { st->value[i] = 0.5; st->band_lo[i] = 0.25; st->band_hi[i] = 0.75; st->gated[i] = 1; st->pass[i] = 1; st->sub_n[i] = 600; }
    st->value[C3_ST_N] = 2651; st->band_lo[C3_ST_N] = 2000; st->band_hi[C3_ST_N] = INFINITY;   /* the unbounded threshold */
}

static char *emit(const c3_stats *st, int ten_mode, double ls10, size_t *bad, size_t *len)
{
    char *buf = NULL; size_t n = 0;
    FILE *o = open_memstream(&buf, &n);
    fputs("{\n", o);
    *bad = c3_json_stats(o, "X", st, ten_mode, ls10);
    fputs("\n}\n", o);
    fclose(o);
    *len = n;
    return buf;
}

static int has(const char *s, const char *t) { return strstr(s, t) != NULL; }
/* bare inf / nan tokens outside strings: the emitted text never holds them at all */
static int bare_nonfinite(const char *s)
{
    return has(s, "inf") || has(s, "nan") || has(s, "Inf") || has(s, "NaN");
}

static void test_validator(void)
{
    const char *good[] = { "{}", "[1,2.5e-3,-0,\"a\\u00e9\"]", "{\"a\":null,\"b\":[true,false]}", " 7 " };
    const char *bad[] = { "{\"a\": inf}", "{\"a\": nan}", "{\"a\": -inf}", "[Infinity]", "[NaN]", "{\"a\":1,}", "[01]", "[1.]", "{\"a\":1} x", "" };
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) CHECK(strict_json(good[i], strlen(good[i])), "validator rejected %s", good[i]);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!strict_json(bad[i], strlen(bad[i])), "validator accepted %s", bad[i]);
}

static void test_finite_and_unbounded(void)
{
    c3_stats st; size_t bad, n;
    fill_ok(&st);
    char *s = emit(&st, C3_TEN_MEASURED, -4.25, &bad, &n);
    CHECK(strict_json(s, n), "finite block is not strict JSON");
    CHECK(!bare_nonfinite(s), "finite block holds inf/nan");
    CHECK(bad == 0 && c3_stats_invalid(&st, C3_TEN_MEASURED, -4.25) == 0, "finite block counted invalid fields");
    CHECK(has(s, "\"n_scored\": {\"value\": 2651, \"value_status\": \"ok\", \"lo\": 2000, \"lo_unbounded\": false, \"hi\": null, \"hi_unbounded\": true"), "unbounded threshold not null + hi_unbounded true");
    CHECK(has(s, "\"coverage50\": {\"value\": 0.5, \"value_status\": \"ok\", \"lo\": 0.25, \"lo_unbounded\": false, \"hi\": 0.75, \"hi_unbounded\": false"), "finite threshold changed");
    CHECK(has(s, "\"ten_log_score\": -4.25, \"ten_log_score_status\": \"ok\""), "measured ten-step score changed");
    CHECK(has(s, "\"invalid_fields\": []"), "invalid_fields not empty");
    free(s);
    /* a lower bound of -inf is unbounded too */
    st.band_lo[C3_ST_BIAS] = -INFINITY;
    s = emit(&st, C3_TEN_MEASURED, -4.25, &bad, &n);
    CHECK(strict_json(s, n) && bad == 0 && has(s, "\"lo\": null, \"lo_unbounded\": true"), "-inf lower bound not marked unbounded");
    free(s);
}

static void test_unavailable_baseline(void)
{
    c3_stats st; size_t bad, n;
    fill_ok(&st);
    char *s = emit(&st, C3_TEN_UNAVAILABLE, NAN, &bad, &n);
    CHECK(strict_json(s, n) && !bare_nonfinite(s), "baseline block not strict JSON");
    CHECK(has(s, "\"ten_log_score\": null, \"ten_log_score_status\": \"unavailable\""), "unavailable baseline score not null + status");
    CHECK(bad == 0 && c3_stats_invalid(&st, C3_TEN_UNAVAILABLE, NAN) == 0, "an unavailable baseline score was counted invalid");
    free(s);
    s = emit(&st, C3_TEN_ABSENT, 0, &bad, &n);
    CHECK(strict_json(s, n) && !has(s, "ten_log_score"), "v3 (no ten-step field) block carries a ten-step field");
    free(s);
}

static void test_nonfinite_measured_fails_closed(void)
{
    c3_stats st; size_t bad, n; char *s;
    /* measured ten-step score of S not finite */
    fill_ok(&st);
    s = emit(&st, C3_TEN_MEASURED, -INFINITY, &bad, &n);
    CHECK(strict_json(s, n) && !bare_nonfinite(s), "invalid ten-step block not strict JSON");
    CHECK(bad == 1 && has(s, "\"ten_log_score_status\": \"invalid\"") && has(s, "\"invalid_fields\": [\"ten_log_score\"]"), "non-finite ten-step score not marked invalid");
    CHECK(c3_stats_invalid(&st, C3_TEN_MEASURED, NAN) == 1, "NaN measured ten-step score not counted");
    free(s);
    /* mean log score, width and a gated statistic value non-finite */
    fill_ok(&st);
    st.logscore = -INFINITY; st.width_mean = NAN; st.value[C3_ST_LAG1] = NAN; st.pass[C3_ST_LAG1] = 0; st.calibrated = 0;
    s = emit(&st, C3_TEN_UNAVAILABLE, NAN, &bad, &n);
    CHECK(strict_json(s, n) && !bare_nonfinite(s), "invalid block not strict JSON");
    CHECK(bad == 3 && c3_stats_invalid(&st, C3_TEN_UNAVAILABLE, NAN) == 3, "want 3 invalid fields, have %zu", bad);
    CHECK(has(s, "\"mean_log_score\": null") && has(s, "\"lag1_z\": {\"value\": null, \"value_status\": \"invalid\""), "invalid values not null + invalid status");
    CHECK(has(s, "\"stats.lag1_z.value\"") && has(s, "\"width80_mean_mc\"") && has(s, "\"mean_log_score\""), "invalid_fields misses a field");
    free(s);
    /* a NaN threshold is not "unbounded": invalid */
    fill_ok(&st); st.band_hi[C3_ST_COV95] = NAN;
    s = emit(&st, C3_TEN_UNAVAILABLE, NAN, &bad, &n);
    CHECK(strict_json(s, n) && bad == 1 && has(s, "\"hi\": null, \"hi_unbounded\": false"), "NaN threshold not invalid");
    free(s);
    /* an ungated statistic with no steps is unmeasured, not invalid */
    fill_ok(&st); st.value[C3_ST_REG_IDLE] = NAN; st.gated[C3_ST_REG_IDLE] = 0; st.pass[C3_ST_REG_IDLE] = 0;
    s = emit(&st, C3_TEN_UNAVAILABLE, NAN, &bad, &n);
    CHECK(strict_json(s, n) && bad == 0 && has(s, "\"regime_idle_cov95\": {\"value\": null, \"value_status\": \"unmeasured\""), "ungated NaN not unmeasured");
    free(s);
    /* the gate itself: band() fails a non-finite value (est3c_common.c band) */
    c3_stats j; memset(&j, 0, sizeof j);
    j.n = 0; j.logscore = -INFINITY; c3_judge(&j);
    CHECK(!j.calibrated, "an empty (all-NaN) statistics block judged calibrated");
    CHECK(c3_stats_invalid(&j, C3_TEN_UNAVAILABLE, NAN) > 0, "an empty statistics block not counted invalid");
}

static void test_num(void)
{
    char *buf = NULL; size_t n = 0; FILE *o = open_memstream(&buf, &n);
    c3_json_num(o, 0.1); fputc(' ', o); c3_json_num(o, INFINITY); fputc(' ', o); c3_json_num(o, -INFINITY); fputc(' ', o); c3_json_num(o, NAN);
    fclose(o);
    CHECK(!strcmp(buf, "0.10000000000000001 null null null"), "c3_json_num wrote '%s'", buf);
    free(buf);
}

/* the committed v5 receipt: its bytes must still hash to its name, and it is the known strict-JSON offender */
static void test_original_untouched(const char *root)
{
    static const char *name = "receipt-7f0bb7018d066bc438e7a8a9bf87c0d45615f60be2d029b6ab5194168e3db3d1.json";
    char path[1400]; snprintf(path, sizeof path, "%s/docs/estimation/receipts/est-v5/%s", root, name);
    size_t n; char *b = slurp(path, &n);
    if (!b) { fprintf(stderr, "test_est_json: committed v5 receipt not found, skipped\n"); return; }
    uint8_t dg[32]; char hex[65];
    sha256_hash((const uint8_t *)b, n, dg); est_hex(dg, 32, hex);
    CHECK(!strncmp(name + 8, hex, 64), "the committed v5 receipt no longer matches its digest name (%s)", hex);
    CHECK(!strict_json(b, n), "validator accepted the committed v5 receipt (it contains bare inf/nan)");
    free(b);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--validate")) {
        size_t n; char *b = slurp(argv[2], &n);
        if (!b) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
        int ok = strict_json(b, n);
        free(b);
        if (!ok) fprintf(stderr, "not strict JSON: %s\n", argv[2]);
        return ok ? 0 : 1;
    }
    test_validator(); test_num(); test_finite_and_unbounded(); test_unavailable_baseline(); test_nonfinite_measured_fails_closed();
    test_original_untouched(argc > 1 ? argv[1] : ".");
    printf("test_est_json: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
