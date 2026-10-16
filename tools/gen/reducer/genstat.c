/*
 * genstat.c -- the statistics of the GEN decision receipt (omega#477, GEN-2; formats in
 * tools/gen/runner/FORMATS.md section 5; rule from the GEN plan, Fable review). No hidden data, no I/O
 * besides the named file and stdout. Deterministic: the same input gives the same bytes.
 *
 *   genstat wilson <k> <n> [z]          "p lo hi": Wilson score interval (default z = 1.959963984540054)
 *   genstat wilson4 <k> <n> [z]         the same with 4 decimals: equals tools/exp445 x445 wilson (cross-check)
 *   genstat mcnemar1 <b> <c>            exact one-sided McNemar p for H1 "first arm better":
 *                                       P(X >= b), X ~ Binomial(b + c, 1/2); 1 when b + c = 0
 *   genstat mcnemar1x <b> <c>           the same p as an exact fraction "<numerator>/2^<b+c>"
 *   genstat newcombe <a> <b> <c> <d> [z] "diff lo hi": Newcombe (1998) method 10 interval for the paired
 *                                       difference p1 - p2, a = both succeed, b = only arm 1, c = only arm 2,
 *                                       d = neither; p1 = (a+b)/n, p2 = (a+c)/n, no continuity correction
 *   genstat paired <matrix.tsv>         the paired decision
 *   genstat vectors <file>              checks every row of a stats vector file (Agent A format): reals within
 *                                       1e-9 and equal at 6 decimals, counts, p_num/p_den, yes/no, verdicts exactly: lines "<task>\t<A>\t<B>\t<C>\t<D>" with 0/1
 *                                       attempt-1 functional success; prints the pair./sanity./decision
 *                                       lines of the receipt (key=value, fixed order)
 *
 * Exactness: binomial tails are summed in 128-bit integers from Pascal's triangle (n <= 120, so 20 * 2^n
 * fits and "p < 0.05" is decided exactly as 20 * numerator < 2^n). Wilson and Newcombe use long double.
 * Output: fixed point, 6 decimals.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Z95 1.959963984540054L
#define Z90 1.644853626951472L
#define NMAX 120

typedef unsigned __int128 u128;

static int num_arg(const char *s, long lo, long hi, long *v)
{
    char *e;
    if (!*s) return -1;
    long x = strtol(s, &e, 10);
    if (*e || x < lo || x > hi) return -1;
    *v = x;
    return 0;
}

static void wilson(long k, long n, long double z, long double *p, long double *lo, long double *hi)
{
    long double nn = (long double)n, ph = (long double)k / nn, z2 = z * z, d = 1 + z2 / nn;
    long double c = (ph + z2 / (2 * nn)) / d, h = z * sqrtl(ph * (1 - ph) / nn + z2 / (4 * nn * nn)) / d;
    *p = ph;
    *lo = c - h < 0 ? 0 : c - h;
    *hi = c + h > 1 ? 1 : c + h;
    if (k == 0) *lo = 0; /* exact ends: avoid -0.000000 from rounding */
    if (k == n) *hi = 1;
}

/* tail(b, n) = sum_{i=b}^{n} C(n, i), exact; n <= NMAX */
static u128 tail(long b, long n)
{
    u128 row[NMAX + 2];
    memset(row, 0, sizeof row);
    row[0] = 1;
    for (long r = 1; r <= n; r++)
        for (long i = r; i >= 1; i--) row[i] += row[i - 1];
    u128 s = 0;
    for (long i = b; i <= n; i++) s += row[i];
    return s;
}

static long double mcnemar1(long b, long c, u128 *numer)
{
    long n = b + c;
    u128 t = n == 0 ? 1 : tail(b, n);
    if (numer) *numer = t;
    return n == 0 ? 1.0L : ldexpl((long double)t, (int)-n);
}

static void put_u128(u128 v)
{
    char buf[48];
    int i = 47;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    fputs(buf + i, stdout);
}

static void newcombe(long a, long b, long c, long d, long double z, long double *diff, long double *lo, long double *hi)
{
    long n = a + b + c + d;
    long double p1, l1, u1, p2, l2, u2;
    wilson(a + b, n, z, &p1, &l1, &u1);
    wilson(a + c, n, z, &p2, &l2, &u2);
    long double den = (long double)(a + b) * (long double)(c + d) * (long double)(a + c) * (long double)(b + d);
    /* GEN_PROTOCOL_V1 section 9 (Agent A vectors): numerator max(ad - bc - n/2, 0) when ad > bc; 0 when a marginal is 0 */
    long double num = (long double)a * d - (long double)b * c;
    if (num > 0) num = fmaxl(num - (long double)n / 2, 0);
    long double phi = den > 0 ? num / sqrtl(den) : 0;
    if (phi > 1) phi = 1;
    if (phi < -1) phi = -1;
    long double dl = sqrtl(fmaxl(0, (p1 - l1) * (p1 - l1) - 2 * phi * (p1 - l1) * (u2 - p2) + (u2 - p2) * (u2 - p2)));
    long double eu = sqrtl(fmaxl(0, (u1 - p1) * (u1 - p1) - 2 * phi * (u1 - p1) * (p2 - l2) + (p2 - l2) * (p2 - l2)));
    *diff = (long double)(b - c) / (long double)n;
    *lo = *diff - dl < -1 ? -1 : *diff - dl;
    *hi = *diff + eu > 1 ? 1 : *diff + eu;
}

/* f6: fixed point with 6 decimals, never "-0.000000" */
static void f6(const char *key, long double v)
{
    char b[64];
    snprintf(b, sizeof b, "%.6Lf", v);
    if (!strcmp(b, "-0.000000")) strcpy(b, "0.000000");
    printf("%s=%s\n", key, b);
}

/* the paired decision of one complete matrix: k[] = successes of A B C D; pairs D-A, D-B, D-C */
typedef struct {
    long n, k[4], both[3], donly[3], xonly[3], neither[3];
    u128 pnum[3];
    long double p[3], diff[3], lo95[3], hi95[3], lo90[3], hi90[3];
    int lift[3], pok[3];
    const char *dec;
    char code[128], detail[256];
} Decision;

static void decide(Decision *D, int valid)
{
    static const char *const X = "ABC";
    for (int x = 0; x < 3; x++) {
        long double d;
        newcombe(D->both[x], D->donly[x], D->xonly[x], D->neither[x], Z95, &D->diff[x], &D->lo95[x], &D->hi95[x]);
        newcombe(D->both[x], D->donly[x], D->xonly[x], D->neither[x], Z90, &d, &D->lo90[x], &D->hi90[x]);
        D->p[x] = mcnemar1(D->donly[x], D->xonly[x], &D->pnum[x]);
        long m = D->donly[x] + D->xonly[x];
        D->lift[x] = 10 * (D->donly[x] - D->xonly[x]) >= D->n;
        D->pok[x] = 20 * D->pnum[x] < ((u128)1 << m); /* p < 0.05 exactly (p = 1 when m = 0) */
    }
    long ab = D->k[0] - D->k[1];
    if (!valid) {
        D->dec = "INVALID"; strcpy(D->code, "validity_failure");
        snprintf(D->detail, sizeof D->detail, "a validity check failed");
    } else if (ab > 2) {
        D->dec = "INVALID"; strcpy(D->code, "sanity_a_minus_b_gt_2");
        snprintf(D->detail, sizeof D->detail, "B solved %ld fewer tasks than A (more than 2): the many-shot prefix is invalid", ab);
    } else if (D->lift[0] && D->pok[0] && D->lift[1] && D->pok[1] && D->lift[2] && D->pok[2]) {
        D->dec = "PASS"; strcpy(D->code, "all_controls_lift_and_p");
        snprintf(D->detail, sizeof D->detail, "D beats A, B and C each by at least 10 points with one-sided exact McNemar p < 0.05");
    } else if (D->hi95[2] < 0.10L) {
        D->dec = "FAIL"; strcpy(D->code, "newcombe95_hi_D-C_below_delta");
        snprintf(D->detail, sizeof D->detail, "upper bound of the 95%% Newcombe paired interval for D-C is %.6Lf, below 0.10", D->hi95[2]);
    } else {
        int w = 0;
        D->dec = "INCONCLUSIVE";
        D->code[0] = 0;
        for (int x = 0; x < 3; x++) {
            if (!D->lift[x]) w += snprintf(D->code + w, sizeof D->code - (size_t)w, "%slift_%c", w ? "," : "", X[x]);
            if (!D->pok[x]) w += snprintf(D->code + w, sizeof D->code - (size_t)w, "%sp_%c", w ? "," : "", X[x]);
        }
        snprintf(D->detail, sizeof D->detail, "PASS not met (%s); D-C 95%% upper bound %.6Lf is not below 0.10", D->code, D->hi95[2]);
    }
}

static void tally(Decision *D, const int v[4])
{
    D->n++;
    for (int i = 0; i < 4; i++) D->k[i] += v[i];
    for (int x = 0; x < 3; x++) {
        if (v[3] && v[x]) D->both[x]++; else if (v[3]) D->donly[x]++; else if (v[x]) D->xonly[x]++; else D->neither[x]++;
    }
}

static int cmd_paired(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "genstat: cannot read %s\n", path); return 2; }
    static char seen[NMAX + 1][64];
    char line[256];
    Decision D;
    memset(&D, 0, sizeof D);
    while (fgets(line, sizeof line, f)) {
        char t[64], extra[8];
        int v[4];
        size_t L = strlen(line);
        if (!L || line[L - 1] != '\n') { fprintf(stderr, "genstat: line %ld too long or unterminated\n", D.n + 1); fclose(f); return 2; }
        if (sscanf(line, "%63[a-z0-9_-]\t%d\t%d\t%d\t%d%7s", t, &v[0], &v[1], &v[2], &v[3], extra) != 5) {
            fprintf(stderr, "genstat: malformed matrix line %ld\n", D.n + 1); fclose(f); return 2;
        }
        for (int i = 0; i < 4; i++) if (v[i] != 0 && v[i] != 1) { fprintf(stderr, "genstat: line %ld: success must be 0 or 1\n", D.n + 1); fclose(f); return 2; }
        if (D.n >= NMAX) { fprintf(stderr, "genstat: more than %d tasks\n", NMAX); fclose(f); return 2; }
        for (long j = 0; j < D.n; j++) if (!strcmp(seen[j], t)) { fprintf(stderr, "genstat: task %s twice\n", t); fclose(f); return 2; }
        snprintf(seen[D.n], sizeof seen[D.n], "%s", t);
        tally(&D, v);
    }
    fclose(f);
    if (D.n == 0) { fprintf(stderr, "genstat: empty matrix\n"); return 2; }
    decide(&D, 1);
    static const char *const X = "ABC";
    char key[64];
    for (int x = 0; x < 3; x++) {
        printf("pair.D-%c.both=%ld\npair.D-%c.d_only=%ld\npair.D-%c.x_only=%ld\npair.D-%c.neither=%ld\n", X[x], D.both[x], X[x], D.donly[x], X[x], D.xonly[x], X[x], D.neither[x]);
        snprintf(key, sizeof key, "pair.D-%c.diff", X[x]); f6(key, D.diff[x]);
        snprintf(key, sizeof key, "pair.D-%c.mcnemar_p_one_sided", X[x]); f6(key, D.p[x]);
        snprintf(key, sizeof key, "pair.D-%c.newcombe95_lo", X[x]); f6(key, D.lo95[x]);
        snprintf(key, sizeof key, "pair.D-%c.newcombe95_hi", X[x]); f6(key, D.hi95[x]);
        snprintf(key, sizeof key, "pair.D-%c.newcombe90_lo", X[x]); f6(key, D.lo90[x]);
        snprintf(key, sizeof key, "pair.D-%c.newcombe90_hi", X[x]); f6(key, D.hi90[x]);
        printf("pair.D-%c.lift_ok=%s\npair.D-%c.p_ok=%s\n", X[x], D.lift[x] ? "yes" : "no", X[x], D.pok[x] ? "yes" : "no");
    }
    printf("sanity.a_minus_b=%ld\ndecision=%s\ndecision_code=%s\ndecision_detail=%s\n", D.k[0] - D.k[1], D.dec, D.code, D.detail);
    return 0;
}

/* ---- vectors: check every row of a stats vector file (Agent A, tests/one_layer/generalization/stats_vectors.tsv) */
static int vbad;
static void vreal(const char *id, const char *what, const char *want, long double got)
{
    char *e;
    long double w = strtold(want, &e);
    char g6[40], w6[40];
    snprintf(g6, sizeof g6, "%.6Lf", got); snprintf(w6, sizeof w6, "%.6Lf", w);
    if (!strcmp(g6, "-0.000000")) strcpy(g6, "0.000000");
    if (!strcmp(w6, "-0.000000")) strcpy(w6, "0.000000");
    if (*e || fabsl(w - got) > 1e-9L || strcmp(g6, w6)) { printf("FAIL %s %s: want %s got %.10Lf\n", id, what, want, got); vbad++; }
}
static void vstr(const char *id, const char *what, const char *want, const char *got)
{
    if (strcmp(want, got)) { printf("FAIL %s %s: want %s got %s\n", id, what, want, got); vbad++; }
}
static void vlong(const char *id, const char *what, const char *want, long got)
{
    char g[32];
    snprintf(g, sizeof g, "%ld", got);
    vstr(id, what, want, g);
}
static void vu128(const char *id, const char *what, const char *want, u128 got)
{
    char g[48];
    int i = 47;
    g[i] = 0;
    do { g[--i] = (char)('0' + (int)(got % 10)); got /= 10; } while (got);
    vstr(id, what, want, g + i);
}

static int cmd_vectors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "genstat: cannot read %s\n", path); return 2; }
    static char line[8192];
    int rows = 0, kinds[4] = {0, 0, 0, 0};
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        line[strcspn(line, "\n")] = 0;
        char *fl[64];
        int nf = 0;
        for (char *s = line, *t; nf < 64; s = t + 1) { fl[nf++] = s; t = strchr(s, '\t'); if (!t) break; *t = 0; }
        rows++;
        const char *id = nf > 1 ? fl[1] : "?";
        if (!strcmp(fl[0], "wilson") && nf == 8) {
            kinds[0]++;
            long double p, lo, hi, z = !strcmp(fl[4], "90") ? Z90 : Z95;
            wilson(atol(fl[2]), atol(fl[3]), z, &p, &lo, &hi);
            vreal(id, "p", fl[5], p); vreal(id, "lo", fl[6], lo); vreal(id, "hi", fl[7], hi);
        } else if (!strcmp(fl[0], "mcnemar1") && nf == 8) {
            kinds[1]++;
            long b = atol(fl[2]), c = atol(fl[3]);
            u128 num;
            long double p = mcnemar1(b, c, &num);
            vu128(id, "p_num", fl[4], num); vu128(id, "p_den", fl[5], (u128)1 << (b + c)); vreal(id, "p", fl[6], p);
            vstr(id, "p_lt_0.05", fl[7], 20 * num < ((u128)1 << (b + c)) ? "yes" : "no");
        } else if (!strcmp(fl[0], "newcombe") && nf == 12) {
            kinds[2]++;
            long double d, lo, hi, z = !strcmp(fl[6], "90") ? Z90 : Z95;
            vstr(id, "method", fl[7], "10");
            newcombe(atol(fl[2]), atol(fl[3]), atol(fl[4]), atol(fl[5]), z, &d, &lo, &hi);
            vreal(id, "theta", fl[8], d); vreal(id, "lo", fl[9], lo); vreal(id, "hi", fl[10], hi);
        } else if (!strcmp(fl[0], "verdict") && nf == 45) {
            kinds[3]++;
            Decision D;
            memset(&D, 0, sizeof D);
            size_t n = strlen(fl[3]);
            int ok = n >= 1 && n <= NMAX;
            for (int a = 4; a <= 6; a++) ok = ok && strlen(fl[a]) == n;
            if (!ok) { printf("FAIL %s: vectors of different lengths\n", id); vbad++; continue; }
            for (size_t i = 0; i < n; i++) {
                int v[4];
                for (int a = 0; a < 4; a++) v[a] = fl[3 + a][i] == '1';
                tally(&D, v);
            }
            decide(&D, !strcmp(fl[2], "1"));
            for (int a = 0; a < 4; a++) vlong(id, "succ", fl[7 + a], D.k[a]);
            for (int x = 0; x < 3; x++) {
                int o = 11 + 9 * x;
                vlong(id, "both", fl[o], D.both[x]); vlong(id, "d_only", fl[o + 1], D.donly[x]);
                vlong(id, "x_only", fl[o + 2], D.xonly[x]); vlong(id, "neither", fl[o + 3], D.neither[x]);
                vu128(id, "p_num", fl[o + 4], D.pnum[x]); vu128(id, "p_den", fl[o + 5], (u128)1 << (D.donly[x] + D.xonly[x]));
                vreal(id, "p", fl[o + 6], D.p[x]);
                vstr(id, "lift_ok", fl[o + 7], D.lift[x] ? "yes" : "no"); vstr(id, "p_ok", fl[o + 8], D.pok[x] ? "yes" : "no");
            }
            vreal(id, "D-C.newcombe95_lo", fl[38], D.lo95[2]); vreal(id, "D-C.newcombe95_hi", fl[39], D.hi95[2]);
            vreal(id, "D-C.newcombe90_lo", fl[40], D.lo90[2]); vreal(id, "D-C.newcombe90_hi", fl[41], D.hi90[2]);
            vlong(id, "a_minus_b", fl[42], D.k[0] - D.k[1]);
            vstr(id, "verdict", fl[43], D.dec);
            /* fl[44], the reason string, is informative only (Agent A) */
        } else { printf("FAIL %s: unknown record kind or field count (%s, %d fields)\n", id, fl[0], nf); vbad++; }
    }
    fclose(f);
    printf("vectors %s: %d rows (wilson %d, mcnemar1 %d, newcombe %d, verdict %d), %d failures\n", vbad ? "FAIL" : "PASS", rows, kinds[0], kinds[1], kinds[2], kinds[3], vbad);
    return vbad ? 1 : 0;
}

int main(int argc, char **argv)
{
    long a, b, c, d;
    if ((argc == 4 || argc == 5) && (!strcmp(argv[1], "wilson") || !strcmp(argv[1], "wilson4"))) {
        long double z = argc == 5 ? strtold(argv[4], NULL) : Z95, p, lo, hi;
        if (num_arg(argv[3], 1, 1000000, &b) || num_arg(argv[2], 0, b, &a) || !(z > 0)) { fprintf(stderr, "genstat: wilson k n [z], 0 <= k <= n, n >= 1\n"); return 2; }
        wilson(a, b, z, &p, &lo, &hi);
        if (argv[1][6]) printf("%.4Lf %.4Lf %.4Lf\n", p, lo, hi); /* x445 wilson spelling, for the cross-check */
        else printf("%.6Lf %.6Lf %.6Lf\n", p, lo, hi);
        return 0;
    }
    if (argc == 4 && (!strcmp(argv[1], "mcnemar1") || !strcmp(argv[1], "mcnemar1x"))) {
        if (num_arg(argv[2], 0, NMAX, &b) || num_arg(argv[3], 0, NMAX, &c) || b + c > NMAX) { fprintf(stderr, "genstat: mcnemar1 b c, b + c <= %d\n", NMAX); return 2; }
        u128 num;
        long double p = mcnemar1(b, c, &num);
        if (argv[1][8] == 'x') { put_u128(num); printf("/2^%ld\n", b + c); }
        else printf("%.6Lf\n", p);
        return 0;
    }
    if ((argc == 6 || argc == 7) && !strcmp(argv[1], "newcombe")) {
        long double z = argc == 7 ? strtold(argv[6], NULL) : Z95, df, lo, hi;
        if (num_arg(argv[2], 0, 1000000, &a) || num_arg(argv[3], 0, 1000000, &b) || num_arg(argv[4], 0, 1000000, &c) ||
            num_arg(argv[5], 0, 1000000, &d) || a + b + c + d < 1 || !(z > 0)) { fprintf(stderr, "genstat: newcombe a b c d [z], n >= 1\n"); return 2; }
        newcombe(a, b, c, d, z, &df, &lo, &hi);
        char s[3][32];
        snprintf(s[0], 32, "%.6Lf", df); snprintf(s[1], 32, "%.6Lf", lo); snprintf(s[2], 32, "%.6Lf", hi);
        for (int i = 0; i < 3; i++) if (!strcmp(s[i], "-0.000000")) strcpy(s[i], "0.000000");
        printf("%s %s %s\n", s[0], s[1], s[2]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "paired")) return cmd_paired(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "vectors")) return cmd_vectors(argv[2]);
    fprintf(stderr, "usage: genstat wilson <k> <n> [z] | mcnemar1 <b> <c> | mcnemar1x <b> <c> | newcombe <a> <b> <c> <d> [z] | paired <matrix.tsv> | vectors <stats_vectors.tsv>\n");
    return 2;
}
