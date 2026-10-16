/*
 * gen_expected.c -- independent C computation of hidden/expected for the PUBLIC GEN dev tasks
 * devgen01..devgen10 (aien-spark/omega#476; tests/one_layer/generalization/dev/). Written from each task's
 * prompt, not from its reference.osc (closed forms or exact 128-bit arithmetic where the reference loops),
 * so a reference that disagrees with its prompt is caught by `ola.sh tasks-check`.
 *   gen_expected <task_id> < hidden/inputs > hidden/expected
 * Refuses (exit 1) an input outside what the prompt promises ("for every input checked ...").
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef __int128 i128;

static int rd(int64_t *v, int n)
{
    char buf[256];
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    char *s = buf;
    for (int i = 0; i < n; i++) {
        char *e;
        v[i] = strtoll(s, &e, 10);
        if (e == s) { fprintf(stderr, "gen_expected: bad input line\n"); exit(1); }
        s = e;
    }
    return 1;
}

static void refuse(const char *t, const char *why) { fprintf(stderr, "gen_expected: %s: input %s\n", t, why); exit(1); }

static void pr128(i128 v)
{
    char b[64];
    int n = 0, neg = v < 0;
    unsigned __int128 u = neg ? (unsigned __int128)(-(v + 1)) + 1 : (unsigned __int128)v;
    do { b[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    if (neg) putchar('-');
    while (n) putchar(b[--n]);
    putchar('\n');
}

static int fits32(i128 v) { return v >= INT32_MIN && v <= INT32_MAX; }
static int fits64(i128 v) { return v >= INT64_MIN && v <= INT64_MAX; }

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: gen_expected <task_id> < inputs\n"); return 2; }
    const char *t = argv[1];
    int64_t v[8];
    if (!strcmp(t, "devgen01")) while (rd(v, 1)) {
        /* sum_{i<n} 3i^2 + 2i + 1 = (n-1)n(2n-1)/2 + (n-1)n + n */
        i128 n = v[0], s = (n - 1) * n * (2 * n - 1) / 2 + (n - 1) * n + n;
        if (n == 0) s = 0;
        if (s > UINT32_MAX) refuse(t, "result exceeds 32 bits");
        pr128(s);
    }
    else if (!strcmp(t, "devgen02")) while (rd(v, 2)) {
        /* exact sum; i32 semantics: trap iff some partial result leaves i32. The prompt promises that
         * either every partial result fits or the final sum does not fit; check that promise. */
        i128 n = v[0], c = v[1], s = 0;
        int partial_overflow = 0;
        for (i128 x = 0; x < n; x++) {
            i128 a = x - c, b = x + c, p = a * b, g = p + x;
            if (!fits32(a) || !fits32(b) || !fits32(p) || !fits32(g)) partial_overflow = 1;
            s += g;
            if (!fits32(s)) partial_overflow = 1;
        }
        if (partial_overflow && fits32(s)) refuse(t, "a partial result overflows but the final sum fits");
        if (partial_overflow) printf("TRAP\n"); else pr128(s);
    }
    else if (!strcmp(t, "devgen03")) while (rd(v, 2)) {
        uint64_t s = (uint64_t)v[0];
        for (uint64_t i = 0; i < (uint64_t)v[1]; i++) s = (s * 31 + i * i + 7) % 65536;
        printf("%" PRIu64 "\n", s);
    }
    else if (!strcmp(t, "devgen04")) while (rd(v, 1)) {
        i128 n = v[0];
        pr128(n % 2 == 0 ? n * (n + 1) / 2 : n * n * n - 2 * n + 5);
    }
    else if (!strcmp(t, "devgen05")) while (rd(v, 2)) {
        i128 x = v[0], k = v[1];
        if (x < 0) { pr128(3 * x * x + 5 * x - 2); continue; }
        /* t_k = (x + 1) * 2^k - 1; 2t overflows before t_k does only if t_k overflows (2t <= 2^63 - 2) */
        i128 t1 = x + 1;
        int over = 0;
        for (i128 i = 0; i < k && !over; i++) { t1 *= 2; if (t1 - 1 > INT64_MAX) over = 1; }
        if (over) printf("TRAP\n"); else { if (!fits64(t1 - 1)) refuse(t, "range"); pr128(t1 - 1); }
    }
    else if (!strcmp(t, "devgen06")) while (rd(v, 2)) {
        i128 b = v[0], e = v[1];
        if (b < 10) {
            unsigned __int128 r = 1;
            for (i128 i = 0; i < e; i++) { r *= (unsigned __int128)b; if (r > UINT64_MAX) refuse(t, "b^e exceeds 64 bits"); }
            pr128((i128)r);
        } else pr128((b * b + e * e) * 3 + b);
    }
    else if (!strcmp(t, "devgen07")) while (rd(v, 4)) {
        i128 best = (i128)v[1] - v[0];
        for (int i = 1; i < 3; i++) if ((i128)v[i + 1] - v[i] > best) best = (i128)v[i + 1] - v[i];
        pr128(best);
    }
    else if (!strcmp(t, "devgen08")) while (rd(v, 5)) {
        int k = 0;
        for (int i = 0; i < 4; i++) k += v[i + 1] > v[i];
        printf("%d\n", k);
    }
    else if (!strcmp(t, "devgen09")) while (rd(v, 2)) {
        /* count by residues: pairs i < j with (i mod k) + (j mod k) = 0 mod k */
        int64_t n = v[0], k = v[1];
        if (k < 1) refuse(t, "k = 0");
        int64_t cnt[256] = {0};
        for (int64_t i = 1; i <= n; i++) cnt[i % k]++;
        int64_t pairs = cnt[0] * (cnt[0] - 1) / 2;
        for (int64_t r = 1; r < k; r++) {
            int64_t q = k - r;
            if (r < q) pairs += cnt[r] * cnt[q];
            else if (r == q) pairs += cnt[r] * (cnt[r] - 1) / 2;
        }
        printf("%" PRId64 "\n", pairs);
    }
    else if (!strcmp(t, "devgen10")) while (rd(v, 2)) {
        /* for each i, the j count is min(n, m / i) */
        int64_t n = v[0], m = v[1], c = 0;
        for (int64_t i = 1; i <= n; i++) { int64_t q = m / i; c += q < n ? q : n; }
        printf("%" PRId64 "\n", c);
    }
    else { fprintf(stderr, "gen_expected: unknown task %s\n", t); return 2; }
    return 0;
}
