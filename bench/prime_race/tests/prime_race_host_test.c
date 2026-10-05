/* Host test for the prime race protocol helper and the cpu_base baseline.
 * Oracle: independent trial division (no sieve). Compares canonical bitmaps word for word. */
#include "../prime_race_impl.h"

#include <stdlib.h>
#include <string.h>

extern const pr_impl pr_cpu_base_impl;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- independent oracle: canonical bitmap by trial division ---- */
static int is_prime_td(uint64_t n) {
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (uint64_t d = 3; d * d <= n; d += 2) if (n % d == 0) return 0;
    return 1;
}
static void oracle(uint64_t limit, uint64_t *out) {
    size_t nw = pr_words64(limit);
    memset(out, 0, (nw ? nw : 1) * 8);
    for (uint64_t k = 0; k < pr_odd_count(limit); k++)
        if (is_prime_td(2 * k + 1)) out[k / 64] |= 1ULL << (k % 64);
}
static uint64_t count_primes(const uint64_t *bm, uint64_t limit) {
    uint64_t c = (limit >= 2);
    for (size_t w = 0; w < pr_words64(limit); w++) c += (uint64_t)__builtin_popcountll(bm[w]);
    return c;
}
/* comparator: returns -1 if identical, else index of first differing word */
static long first_diff(const uint64_t *a, const uint64_t *b, size_t nw) {
    for (size_t w = 0; w < nw; w++) if (a[w] != b[w]) return (long)w;
    return -1;
}

static uint64_t *run_cpu(uint64_t limit, void **stp) {
    const pr_impl *im = &pr_cpu_base_impl;
    void *st = NULL;
    if (im->setup(&st, limit) != 0 || im->pass(st, limit) != 0) return NULL;
    uint64_t *out = (uint64_t *)calloc(pr_words64(limit) ? pr_words64(limit) : 1, 8);
    if (!out || im->export_bitmap(st, limit, out) != 0) return NULL;
    *stp = st;
    return out;
}

static void check_limit(uint64_t limit, const uint64_t *expect) {
    void *st = NULL;
    uint64_t *got = run_cpu(limit, &st);
    CHECK(got != NULL, "cpu_base failed at limit %llu", (unsigned long long)limit);
    if (!got) return;
    long d = first_diff(expect, got, pr_words64(limit));
    CHECK(d < 0, "limit %llu: first differing word %ld", (unsigned long long)limit, d);
    pr_cpu_base_impl.teardown(st);
    free(got);
}

static void test_all_small(void) {
    uint64_t *exp = (uint64_t *)malloc(64 * 8);
    for (uint64_t limit = 0; limit <= 3000; limit++) {
        oracle(limit, exp);
        check_limit(limit, exp);
    }
    free(exp);
}

/* design limits up to 2,000,000: expected bitmap derived from one trial-division master (prefix + tail mask) */

static size_t g_nlim = 0;
static uint64_t g_lims[8192];
static void addlim(uint64_t l) { if (l <= 2000000 && g_nlim < 8192) g_lims[g_nlim++] = l; }
static void addrange(uint64_t a, uint64_t b) { for (uint64_t l = a; l <= b; l++) addlim(l); }

static void test_design_limits(void) {
    static const uint64_t one[] = {0, 1, 2, 3, 4, 5, 8, 9, 15, 25, 49, 121, 169, 289, 361, 961, 1000000, 2000000};
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++) addlim(one[i]);
    addrange(31, 34); addrange(63, 66); addrange(95, 97); addrange(127, 130); addrange(255, 258);
    addrange(1023, 1025); addrange(1999, 2001); addrange(4095, 4097);
    addrange(999983, 1000003);
    for (uint64_t k = 1; 2048 * k <= 2000000; k++) { addlim(2048 * k - 1); addlim(2048 * k); addlim(2048 * k + 1); }
    for (uint64_t p = 2; p <= 997; p++) if (is_prime_td(p)) { addlim(p * p - 1); addlim(p * p); addlim(p * p + 1); }

    size_t mw = pr_words64(2000000);
    uint64_t *m = (uint64_t *)malloc(mw * 8);
    oracle(2000000, m);
    uint64_t *exp = (uint64_t *)malloc(mw * 8);
    for (size_t i = 0; i < g_nlim; i++) {
        uint64_t limit = g_lims[i];
        size_t nw = pr_words64(limit);
        uint64_t odd = pr_odd_count(limit);
        for (size_t w = 0; w < nw; w++) exp[w] = m[w];
        if (nw && odd % 64) exp[nw - 1] &= (1ULL << (odd % 64)) - 1;
        if (limit <= 3000) { /* master prefix must agree with direct oracle */
            uint64_t d[64]; oracle(limit, d);
            CHECK(first_diff(d, exp, nw) < 0, "master prefix disagrees with direct oracle at %llu", (unsigned long long)limit);
        }
        check_limit(limit, exp);
    }
    printf("design limits checked: %zu\n", g_nlim);
    free(m); free(exp);
}

static void test_pi_table(void) {
    static const struct { uint64_t limit, pi; } t[] = {{10, 4}, {100, 25}, {1000, 168}, {10000, 1229}, {100000, 9592}, {1000000, 78498}, {10000000, 664579}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        void *st = NULL;
        uint64_t *bm = run_cpu(t[i].limit, &st);
        CHECK(bm != NULL, "run failed at %llu", (unsigned long long)t[i].limit);
        if (!bm) continue;
        uint64_t c = count_primes(bm, t[i].limit);
        CHECK(c == t[i].pi, "pi(%llu)=%llu expected %llu", (unsigned long long)t[i].limit, (unsigned long long)c, (unsigned long long)t[i].pi);
        pr_cpu_base_impl.teardown(st);
        free(bm);
    }
}

/* negative self-checks: the comparator must catch corruption */
static void test_negative(void) {
    static const uint64_t lims[] = {100, 1000, 4097, 1000000};
    for (size_t i = 0; i < sizeof lims / sizeof lims[0]; i++) {
        uint64_t limit = lims[i];
        size_t nw = pr_words64(limit);
        uint64_t *exp = (uint64_t *)malloc(nw * 8), *bad = (uint64_t *)malloc(nw * 8);
        oracle(limit, exp);
        memcpy(bad, exp, nw * 8);
        CHECK(first_diff(exp, bad, nw) < 0, "identical copies must compare equal");
        bad[nw / 2] ^= 1ULL << 1; /* flip one bit */
        CHECK(first_diff(exp, bad, nw) == (long)(nw / 2), "one flipped bit not caught at %llu", (unsigned long long)limit);
        memcpy(bad, exp, nw * 8);
        uint64_t odd = pr_odd_count(limit);
        if (odd % 64) { bad[nw - 1] |= 1ULL << 63; CHECK(first_diff(exp, bad, nw) >= 0, "tail garbage not caught"); }
        memcpy(bad, exp, nw * 8);
        memset(bad, 0, nw * 8);
        CHECK(first_diff(exp, bad, nw) >= 0, "all-zero not caught");
        free(exp); free(bad);
    }
}

/* each pass recomputes; second pass on same state gives the same canonical result */
static void test_repeat_pass(void) {
    void *st = NULL;
    const pr_impl *im = &pr_cpu_base_impl;
    uint64_t limit = 12345, a[256], b[256];
    CHECK(im->setup(&st, limit) == 0, "setup");
    CHECK(im->pass(st, limit) == 0 && im->export_bitmap(st, limit, a) == 0, "pass1");
    CHECK(im->pass(st, limit) == 0 && im->export_bitmap(st, limit, b) == 0, "pass2");
    CHECK(first_diff(a, b, pr_words64(limit)) < 0, "pass1 != pass2");
    CHECK(im->threads(st) == 1, "threads");
    im->teardown(st);
}

static void test_formatting(void) {
    char buf[512];
    const pr_impl *im = &pr_cpu_base_impl;
    int n = pr_format_line(buf, sizeof buf, im, 1234, 5.000123456, 1);
    CHECK(n > 0 && strcmp(buf, "aien-cpu-c-base;1234;5.000123;1;algorithm=base,faithful=yes,bits=1") == 0, "line: %s", buf);
    char tiny[8];
    CHECK(pr_format_line(tiny, sizeof tiny, im, 1, 1.0, 1) < 0, "truncated line must be rejected");
    CHECK(pr_line_allowed(0, 1, 5.0, 5.0) == 1, "allowed");
    CHECK(pr_line_allowed(1, 1, 5.0, 5.0) == 0, "failed -> no line");
    CHECK(pr_line_allowed(0, 0, 5.0, 5.0) == 0, "passes 0 -> no line");
    CHECK(pr_line_allowed(0, 3, 4.999, 5.0) == 0, "short elapsed -> no line");

    char *mem = NULL; size_t sz = 0;
    FILE *f = open_memstream(&mem, &sz);
    pr_json_string(f, "a\"b\\c\n\t\x01z");
    fclose(f);
    CHECK(strcmp(mem, "\"a\\\"b\\\\c\\n\\t\\u0001z\"") == 0, "json escape: %s", mem);
    free(mem);

    void *st = NULL;
    im->setup(&st, 100);
    pr_report r = {im, st, 1, 100, 5.0, "timed", 7, 5.25, "COMPLETE", NULL};
    f = open_memstream(&mem, &sz);
    CHECK(pr_write_report(f, &r) == 0, "report write");
    fclose(f);
    CHECK(strstr(mem, "\"schema\": \"aien-prime-race/impl-report/v1\"") != NULL, "schema");
    CHECK(strstr(mem, "\"label\": \"aien-cpu-c-base\"") != NULL, "label");
    CHECK(strstr(mem, "\"status\": \"OK\"") != NULL && strstr(mem, "\"error\": null") != NULL, "status/error");
    CHECK(strstr(mem, "\"passes\": 7") != NULL && strstr(mem, "\"elapsed_s\": 5.250000") != NULL, "passes/elapsed");
    CHECK(strstr(mem, "\"environment\": \"linux-host-cpu\"") != NULL, "environment");
    CHECK(strstr(mem, "\"build\": {\"source_commit\"") != NULL, "build");
    free(mem);
    r.status = "EXEC_FAILED"; r.error = "boom \"x\"";
    f = open_memstream(&mem, &sz);
    pr_write_report(f, &r);
    fclose(f);
    CHECK(strstr(mem, "\"error\": \"boom \\\"x\\\"\"") != NULL, "failure report error escaped");
    free(mem);
    im->teardown(st);
}

static void test_args(void) {
    pr_args a; char err[128];
    char *ok[] = {"x", "--limit", "1000", "--min-seconds", "0.5", "--audit-passes", "3", "--bitmap-out", "/tmp/b"};
    CHECK(pr_parse_args(9, ok, &a, err, sizeof err) == 0 && a.limit == 1000 && a.min_seconds == 0.5 && a.audit_passes == 3, "valid args");
    char *def[] = {"x"};
    CHECK(pr_parse_args(1, def, &a, err, sizeof err) == 0 && a.limit == 1000000 && a.min_seconds == 5.0 && a.audit_passes == 0, "defaults");
    char *bad1[] = {"x", "--limit", "12abc"};
    char *bad2[] = {"x", "--limit", "-5"};
    char *bad3[] = {"x", "--limit", "1099511627777"}; /* 2^40 + 1 */
    char *bad4[] = {"x", "--limit"};
    char *bad5[] = {"x", "--min-seconds", "nan"};
    char *bad6[] = {"x", "--audit-passes", "0", "--bitmap-out", "/tmp/b"};
    char *bad7[] = {"x", "--bogus"};
    char *bad8[] = {"x", "--audit-passes", "2"};
    char *bad9[] = {"x", "--limit", ""};
    char *good40[] = {"x", "--limit", "1099511627776"};
    CHECK(pr_parse_args(3, bad1, &a, err, sizeof err) != 0, "trailing junk");
    CHECK(pr_parse_args(3, bad2, &a, err, sizeof err) != 0, "negative");
    CHECK(pr_parse_args(3, bad3, &a, err, sizeof err) != 0, "above 2^40");
    CHECK(pr_parse_args(2, bad4, &a, err, sizeof err) != 0, "missing value");
    CHECK(pr_parse_args(3, bad5, &a, err, sizeof err) != 0, "nan");
    CHECK(pr_parse_args(5, bad6, &a, err, sizeof err) != 0, "audit 0");
    CHECK(pr_parse_args(2, bad7, &a, err, sizeof err) != 0, "unknown option");
    CHECK(pr_parse_args(3, bad8, &a, err, sizeof err) != 0, "audit without bitmap-out");
    CHECK(pr_parse_args(3, bad9, &a, err, sizeof err) != 0, "empty limit");
    CHECK(pr_parse_args(3, good40, &a, err, sizeof err) == 0, "2^40 allowed");
}

int main(void) {
    test_formatting();
    test_args();
    test_negative();
    test_repeat_pass();
    test_pi_table();
    test_all_small();
    test_design_limits();
    printf("prime_race_host_test: %d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "PRIME_RACE_HOST_TEST FAIL\n" : "PRIME_RACE_HOST_TEST PASS\n");
    return g_fail ? 1 : 0;
}
