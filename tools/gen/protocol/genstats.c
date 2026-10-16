/*
 * genstats.c -- reference statistics of the GEN decision rule (aien-spark/omega#476,
 * docs/one_layer/GEN_PROTOCOL_V1.md section 9). Writes tests/one_layer/generalization/stats_vectors.tsv,
 * the test vectors the GEN reducer (tools/gen/reducer, omega#477) must reproduce.
 *
 *   genstats wilson <k> <n> <95|90>             "p lo hi"
 *   genstats mcnemar1 <b> <c>                   "p_num p_den p": exact one-sided McNemar,
 *                                               P(Bin(b+c, 1/2) >= b) = p_num / p_den, p_den = 2^(b+c)
 *   genstats newcombe <a> <b> <c> <d> <95|90> [8|10]
 *                                               "theta lo hi": Newcombe (1998, Stat Med 17:2635) paired
 *                                               interval for p1 - p2, method 10 (default) or 8; a = both,
 *                                               b = first only, c = second only, d = neither
 *   genstats verdict <valid 0|1> <A> <B> <C> <D>  the full decision from four 0/1 strings (one char per task)
 *   genstats vectors                            print the whole stats_vectors.tsv
 *   genstats selftest                           check Newcombe methods 8 and 10 against every row of the
 *                                               paper's Table III (4 decimals), and internal identities
 * Exact integer arithmetic for McNemar (unsigned __int128, b + c <= 120); IEEE double elsewhere.
 * Deterministic: no clock, no randomness, no environment.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

/* standard normal quantiles, pinned (GEN_PROTOCOL_V1 section 9) */
#define Z95 1.959963984540054   /* z(0.975): two-sided 95% */
#define Z90 1.644853626951472   /* z(0.95):  two-sided 90% */

static double zof(int conf) { return conf == 95 ? Z95 : Z90; }

/* Wilson score interval without continuity correction; exact 0 / 1 at k = 0 / k = n */
static void wilson(long k, long n, double z, double *p, double *lo, double *hi)
{
    double pp = (double)k / (double)n, z2 = z * z, nn = (double)n, den = 1 + z2 / nn;
    double c = (pp + z2 / (2 * nn)) / den, h = z * sqrt(pp * (1 - pp) / nn + z2 / (4 * nn * nn)) / den;
    *p = pp;
    *lo = k == 0 ? 0.0 : c - h;
    *hi = k == n ? 1.0 : c + h;
}

static void u128str(u128 v, char *out)
{
    char t[64];
    int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

/* P(Y >= b), Y ~ Bin(b + c, 1/2), as num / 2^(b+c) */
static void mcnemar1(int b, int c, u128 *num, u128 *den)
{
    int n = b + c;
    u128 s = 0, binom = 1;            /* binom = C(n, i) */
    for (int i = 0; i <= n; i++) {
        if (i >= b) s += binom;
        binom = binom * (u128)(n - i) / (u128)(i + 1);
    }
    *num = s;
    *den = (u128)1 << n;
}

/* Newcombe paired interval for theta = (b - c) / n. method 10: phi numerator a*d - b*c replaced by
 * max(a*d - b*c - n/2, 0) when a*d > b*c; method 8: plain phi. phi = 0 when any marginal is 0. */
static void newcombe(long a, long b, long c, long d, double z, int method, double *theta, double *lo, double *hi)
{
    long n = a + b + c + d;
    double p1, l1, u1, p2, l2, u2;
    wilson(a + b, n, z, &p1, &l1, &u1);
    wilson(a + c, n, z, &p2, &l2, &u2);
    double num = (double)a * (double)d - (double)b * (double)c;
    if (method == 10 && num > 0) { num -= (double)n / 2.0; if (num < 0) num = 0; }
    double dprod = (double)(a + b) * (double)(c + d) * (double)(a + c) * (double)(b + d);
    double phi = dprod == 0 ? 0.0 : num / sqrt(dprod);
    double dl1 = p1 - l1, du1 = u1 - p1, dl2 = p2 - l2, du2 = u2 - p2;
    double s1 = dl1 * dl1 - 2 * phi * dl1 * du2 + du2 * du2;
    double s2 = du1 * du1 - 2 * phi * du1 * dl2 + dl2 * dl2;
    double delta = sqrt(s1 > 0 ? s1 : 0), eps = sqrt(s2 > 0 ? s2 : 0);
    *theta = (double)(b - c) / (double)n;
    *lo = *theta - delta;
    *hi = *theta + eps;
    if (*lo < -1) *lo = -1;
    if (*hi > 1) *hi = 1;
}

/* ---- the decision ---------------------------------------------------------------------------- */

typedef struct {
    int n, s[4];                 /* successes of A B C D */
    int both[3], donly[3], xonly[3], neither[3];
    char pnum[3][48], pden[3][48];
    double p[3];
    int lift_ok[3], p_ok[3];
    double lo95, hi95, lo90, hi90;   /* Newcombe method 10, D - C */
    int a_minus_b;
    const char *verdict;
    char reasons[256];
} Decision;

static int decide(int valid, char *const v[4], Decision *r, char *err, size_t errn)
{
    memset(r, 0, sizeof *r);
    size_t n = strlen(v[0]);
    if (n == 0 || n > 120) { snprintf(err, errn, "vector length %zu, must be 1..120", n); return -1; }
    for (int x = 0; x < 4; x++) {
        if (strlen(v[x]) != n) { snprintf(err, errn, "vectors differ in length (incomplete paired matrix)"); return -1; }
        for (size_t i = 0; i < n; i++) if (v[x][i] != '0' && v[x][i] != '1') { snprintf(err, errn, "vector %c holds a byte other than 0/1", "ABCD"[x]); return -1; }
    }
    r->n = (int)n;
    for (int x = 0; x < 4; x++) for (size_t i = 0; i < n; i++) r->s[x] += v[x][i] == '1';
    for (int x = 0; x < 3; x++) {
        for (size_t i = 0; i < n; i++) {
            int d = v[3][i] == '1', o = v[x][i] == '1';
            if (d && o) r->both[x]++; else if (d) r->donly[x]++; else if (o) r->xonly[x]++; else r->neither[x]++;
        }
        u128 num, den;
        mcnemar1(r->donly[x], r->xonly[x], &num, &den);
        u128str(num, r->pnum[x]);
        u128str(den, r->pden[x]);
        r->p[x] = (double)num / (double)den;
        r->p_ok[x] = (u128)20 * num < den;                       /* p < 0.05, exactly */
        r->lift_ok[x] = 10 * (r->donly[x] - r->xonly[x]) >= r->n; /* (d_only - x_only) / n >= 0.10, exactly */
    }
    double th;
    newcombe(r->both[2], r->donly[2], r->xonly[2], r->neither[2], Z95, 10, &th, &r->lo95, &r->hi95);
    newcombe(r->both[2], r->donly[2], r->xonly[2], r->neither[2], Z90, 10, &th, &r->lo90, &r->hi90);
    r->a_minus_b = r->s[0] - r->s[1];
    int pass = 1;
    for (int x = 0; x < 3; x++) pass &= r->lift_ok[x] && r->p_ok[x];
    if (!valid || r->a_minus_b > 2) {
        r->verdict = "INVALID";
        snprintf(r->reasons, sizeof r->reasons, "%s%s%s", !valid ? "validity_failure" : "", !valid && r->a_minus_b > 2 ? "," : "",
                 r->a_minus_b > 2 ? "sanity_a_minus_b_gt_2" : "");
    } else if (pass) {
        r->verdict = "PASS";
        snprintf(r->reasons, sizeof r->reasons, "all_controls_lift_and_p");
    } else if (r->hi95 < 0.10) {
        r->verdict = "FAIL";
        snprintf(r->reasons, sizeof r->reasons, "newcombe95_hi_D-C_below_delta");
    } else {
        r->verdict = "INCONCLUSIVE";
        int k = 0;
        for (int x = 0; x < 3; x++) {
            if (!r->lift_ok[x]) k += snprintf(r->reasons + k, sizeof r->reasons - (size_t)k, "%slift_%c", k ? "," : "", "ABC"[x]);
            if (!r->p_ok[x]) k += snprintf(r->reasons + k, sizeof r->reasons - (size_t)k, "%sp_%c", k ? "," : "", "ABC"[x]);
        }
    }
    return 0;
}

/* ---- vectors --------------------------------------------------------------------------------- */

/* a verdict case: groups of (ABCD pattern, count), concatenated in order */
typedef struct { const char *id; int valid; const char *groups; const char *note; } VCase;

static const VCase vcases[] = {
    {"v01_pass", 1, "1111:18 0001:16 0011:4 0111:2 0000:20", "D far ahead of every control"},
    {"v02_fail_equal", 1, "1111:20 0010:3 0001:3 0000:34", "D = C exactly; upper bound below 10pp"},
    {"v03_inconclusive_small_lift", 1, "1111:20 0011:5 0001:4 0010:1 0000:30", "D - C = 3 tasks: neither PASS nor FAIL"},
    {"v04_inconclusive_c_p", 1, "1111:15 0011:10 0001:8 0010:2 0000:25", "lift >= 10pp vs C (6 tasks) but p vs C = 0.0547"},
    {"v05_invalid_validity", 0, "1111:18 0001:16 0011:4 0111:2 0000:20", "v01 with a validity failure"},
    {"v06_invalid_sanity", 1, "1111:15 1001:3 0011:1 0001:20 0000:21", "A - B = 3 tasks"},
    {"v07_sanity_boundary_pass", 1, "1111:15 1011:2 0001:20 0000:23", "A - B = 2 tasks: allowed"},
    {"v08_boundary_lift6_p", 1, "1111:20 0001:6 0000:34", "d_only = 6, x_only = 0 vs every control: lift exactly 10pp, p = 1/64"},
    {"v09_boundary_lift5", 1, "1111:20 0001:5 0000:35", "lift 5/60 < 10pp vs every control"},
    {"v10_boundary_8_2", 1, "1111:20 0001:8 1110:2 0000:30", "d_only 8, x_only 2: lift 10pp, p = 56/1024 = 0.0547 (not < 0.05)"},
    {"v11_boundary_7_1", 1, "1111:20 0001:7 1110:1 0000:32", "d_only 7, x_only 1: lift 10pp, p = 9/256"},
    {"v12_all_fail", 1, "0000:60", "nothing succeeds: D - C = 0 with a narrow interval"},
    {"v13_all_pass", 1, "1111:60", "everything succeeds"},
    {"v14_d_worse", 1, "1111:20 1110:8 0001:1 0000:31", "D behind every control"},
    {"v15_dev_size", 1, "1111:3 0001:4 0000:3", "n = 10 (dev size)"},
    {"v16_pass_vs_ab_not_c", 1, "1111:10 0011:12 0001:4 0000:34", "D ahead of A and B, tied-ish with C"},
};

static int build(const char *groups, char *v[4])
{
    static char buf[4][128];
    int n = 0;
    const char *s = groups;
    while (*s) {
        char pat[5];
        int cnt, used;
        if (sscanf(s, "%4[01]:%d%n", pat, &cnt, &used) != 2 || strlen(pat) != 4) return -1;
        for (int i = 0; i < cnt; i++) { if (n >= 120) return -1; for (int x = 0; x < 4; x++) buf[x][n] = pat[x]; n++; }
        s += used;
        while (*s == ' ') s++;
    }
    for (int x = 0; x < 4; x++) { buf[x][n] = 0; v[x] = buf[x]; }
    return n;
}

static void print_decision_cols(const Decision *r)
{
    printf("%d\t%d\t%d\t%d", r->s[0], r->s[1], r->s[2], r->s[3]);
    for (int x = 0; x < 3; x++)
        printf("\t%d\t%d\t%d\t%d\t%s\t%s\t%.10f\t%s\t%s", r->both[x], r->donly[x], r->xonly[x], r->neither[x], r->pnum[x], r->pden[x], r->p[x],
               r->lift_ok[x] ? "yes" : "no", r->p_ok[x] ? "yes" : "no");
    printf("\t%.10f\t%.10f\t%.10f\t%.10f\t%d\t%s\t%s", r->lo95, r->hi95, r->lo90, r->hi90, r->a_minus_b, r->verdict, r->reasons);
}

/* Table III of Newcombe (1998): e f g h, method 8 lo hi, method 10 lo hi (95%, 4 decimals as printed) */
static const double table3[][8] = {
    {36, 12, 2, 0, 0.0569, 0.3404, 0.0569, 0.3404},
    {20, 12, 2, 16, 0.0618, 0.3242, 0.0562, 0.3292},
    {18, 12, 2, 18, 0.0618, 0.3239, 0.0562, 0.3290},
    {36, 14, 0, 0, 0.1528, 0.4167, 0.1528, 0.4167},
    {35, 14, 0, 1, 0.1573, 0.4149, 0.1461, 0.4175},
    {18, 14, 0, 18, 0.1504, 0.3910, 0.1441, 0.3963},
    {2, 97, 1, 0, 0.8721, 0.9854, 0.8721, 0.9854},
    {1, 97, 1, 1, 0.8737, 0.9850, 0.8736, 0.9850},
    {0, 29, 1, 0, 0.6666, 0.9882, 0.6666, 0.9882},
    {2, 98, 0, 0, 0.9178, 0.9945, 0.9178, 0.9945},
    {1, 98, 0, 1, 0.9174, 0.9916, 0.9171, 0.9916},
    {0, 30, 0, 0, 0.8395, 1.0, 0.8395, 1.0},
    {54, 0, 0, 0, -0.0664, 0.0664, -0.0664, 0.0664},
    {53, 0, 0, 1, -0.0640, 0.0640, -0.0729, 0.0729},
    {30, 0, 0, 24, -0.0074, 0.0074, -0.0358, 0.0358},
    {29, 0, 0, 25, -0.0049, 0.0049, -0.0354, 0.0354},
    {28, 0, 0, 26, -0.0025, 0.0025, -0.0352, 0.0352},
    {27, 0, 0, 27, 0.0, 0.0, -0.0351, 0.0351},
};
#define NT3 (sizeof table3 / sizeof table3[0])

static int vectors(void)
{
    printf("# GEN decision statistics test vectors v1 (aien-spark/omega#476). Generated by tools/gen/protocol/genstats.c\n");
    printf("# (`genstats vectors`); never edit by hand. Rules: docs/one_layer/GEN_PROTOCOL_V1.md section 9.\n");
    printf("# z95 = %.15f (z 0.975), z90 = %.15f (z 0.95). Reals printed %%.10f; a reader must agree within 1e-9,\n", Z95, Z90);
    printf("# and its 6-decimal receipt values must equal these rounded to 6 decimals. Counts, p_num, p_den, yes/no and verdicts exactly.\n");
    printf("# One record per line, TAB-separated; field 1 names the record kind; each kind has a #cols line.\n");
    printf("#cols\twilson\tid\tk\tn\tconf\tp\tlo\thi\n");
    static const int wk[][2] = {{0, 60}, {1, 60}, {6, 60}, {20, 60}, {30, 60}, {48, 60}, {59, 60}, {60, 60}, {3, 10}, {0, 10}, {10, 10}};
    for (size_t i = 0; i < sizeof wk / sizeof wk[0]; i++)
        for (int conf = 95; conf >= 90; conf -= 5) {
            double p, lo, hi;
            wilson(wk[i][0], wk[i][1], zof(conf), &p, &lo, &hi);
            printf("wilson\tw%02zu_%d\t%d\t%d\t%d\t%.10f\t%.10f\t%.10f\n", i + 1, conf, wk[i][0], wk[i][1], conf, p, lo, hi);
        }
    printf("#cols\tmcnemar1\tid\tb\tc\tp_num\tp_den\tp\tp_lt_0.05\n");
    static const int mc[][2] = {{0, 0}, {1, 0}, {0, 1}, {4, 0}, {5, 0}, {6, 0}, {7, 1}, {8, 2}, {9, 3}, {10, 4}, {12, 6}, {3, 9}, {15, 15}, {30, 30}, {60, 0}, {0, 60}, {16, 2}, {45, 15}};
    for (size_t i = 0; i < sizeof mc / sizeof mc[0]; i++) {
        u128 num, den;
        char ns[48], ds[48];
        mcnemar1(mc[i][0], mc[i][1], &num, &den);
        u128str(num, ns);
        u128str(den, ds);
        printf("mcnemar1\tm%02zu\t%d\t%d\t%s\t%s\t%.10f\t%s\n", i + 1, mc[i][0], mc[i][1], ns, ds, (double)num / (double)den, (u128)20 * num < den ? "yes" : "no");
    }
    printf("#cols\tnewcombe\tid\ta_both\tb_first_only\tc_second_only\td_neither\tconf\tmethod\ttheta\tlo\thi\tsource\n");
    int id = 0;
    for (size_t i = 0; i < NT3; i++) {
        double th, lo, hi;
        newcombe((long)table3[i][0], (long)table3[i][1], (long)table3[i][2], (long)table3[i][3], Z95, 10, &th, &lo, &hi);
        printf("newcombe\tn%02d\t%.0f\t%.0f\t%.0f\t%.0f\t95\t10\t%.10f\t%.10f\t%.10f\tNewcombe1998_TableIII_published_%.4f_%.4f\n", ++id,
               table3[i][0], table3[i][1], table3[i][2], table3[i][3], th, lo, hi, table3[i][6], table3[i][7]);
    }
    static const int gc[][4] = {{20, 6, 0, 34}, {20, 8, 2, 30}, {20, 3, 3, 34}, {0, 0, 0, 60}, {60, 0, 0, 0}, {25, 12, 6, 17}, {20, 4, 1, 35}, {3, 4, 0, 3}};
    for (size_t i = 0; i < sizeof gc / sizeof gc[0]; i++)
        for (int conf = 95; conf >= 90; conf -= 5) {
            double th, lo, hi;
            newcombe(gc[i][0], gc[i][1], gc[i][2], gc[i][3], zof(conf), 10, &th, &lo, &hi);
            printf("newcombe\tn%02d\t%d\t%d\t%d\t%d\t%d\t10\t%.10f\t%.10f\t%.10f\tgen\n", ++id, gc[i][0], gc[i][1], gc[i][2], gc[i][3], conf, th, lo, hi);
        }
    printf("#cols\tverdict\tid\tvalid\tvec_A\tvec_B\tvec_C\tvec_D\tsucc_A\tsucc_B\tsucc_C\tsucc_D");
    for (int x = 0; x < 3; x++)
        printf("\tD-%c.both\tD-%c.d_only\tD-%c.x_only\tD-%c.neither\tD-%c.p_num\tD-%c.p_den\tD-%c.p\tD-%c.lift_ok\tD-%c.p_ok", "ABC"[x], "ABC"[x], "ABC"[x], "ABC"[x], "ABC"[x], "ABC"[x], "ABC"[x], "ABC"[x], "ABC"[x]);
    printf("\tD-C.newcombe95_lo\tD-C.newcombe95_hi\tD-C.newcombe90_lo\tD-C.newcombe90_hi\ta_minus_b\tverdict\treasons\n");
    for (size_t i = 0; i < sizeof vcases / sizeof vcases[0]; i++) {
        char *v[4];
        if (build(vcases[i].groups, v) < 0) { fprintf(stderr, "genstats: bad case %s\n", vcases[i].id); return 1; }
        Decision r;
        char err[160];
        if (decide(vcases[i].valid, v, &r, err, sizeof err)) { fprintf(stderr, "genstats: %s: %s\n", vcases[i].id, err); return 1; }
        printf("verdict\t%s\t%d\t%s\t%s\t%s\t%s\t", vcases[i].id, vcases[i].valid, v[0], v[1], v[2], v[3]);
        print_decision_cols(&r);
        putchar('\n');
    }
    return 0;
}

static int selftest(void)
{
    int bad = 0;
    for (size_t i = 0; i < NT3; i++)
        for (int m = 0; m < 2; m++) {
            double th, lo, hi;
            newcombe((long)table3[i][0], (long)table3[i][1], (long)table3[i][2], (long)table3[i][3], Z95, m ? 10 : 8, &th, &lo, &hi);
            double plo = table3[i][4 + 2 * m], phi = table3[i][5 + 2 * m];
            /* printed to 4 decimals: the exact value must round to the printed one. One known exception:
             * row 1 97 1 1 has a*d < b*c, so methods 8 and 10 are the same formula, yet the paper prints
             * lower limits 0.8737 (method 8) and 0.8736 (method 10); the computed 0.873672 matches the method 8
             * cell, so that single method-10 cell is allowed one unit in the 4th decimal. */
            double tol = (m && table3[i][0] == 1 && table3[i][1] == 97 && table3[i][3] == 1) ? 0.0001 + 1e-12 : 0.00005 + 1e-12;
            if (fabs(lo - plo) > tol || fabs(hi - phi) > 0.00005 + 1e-12) {
                printf("FAIL newcombe method %d %.0f %.0f %.0f %.0f: got %.6f %.6f, paper %.4f %.4f\n", m ? 10 : 8, table3[i][0], table3[i][1], table3[i][2], table3[i][3], lo, hi, plo, phi);
                bad++;
            }
        }
    /* McNemar: the one-sided tails of b and of c sum to 1 + P(Y = b) */
    for (int b = 0; b <= 30; b++)
        for (int c = 0; c <= 30; c++) {
            u128 n1, d1, n2, d2, pb = 1;
            mcnemar1(b, c, &n1, &d1);
            mcnemar1(c, b, &n2, &d2);
            for (int i = 0; i < b; i++) pb = pb * (u128)(b + c - i) / (u128)(i + 1);
            if (d1 != d2 || n1 + n2 != d1 + pb) { printf("FAIL mcnemar identity b=%d c=%d\n", b, c); bad++; }
        }
    /* Wilson: the 95% interval contains the 90% one and the point */
    for (int k = 0; k <= 60; k++) {
        double p, l95, h95, l90, h90;
        wilson(k, 60, Z95, &p, &l95, &h95);
        wilson(k, 60, Z90, &p, &l90, &h90);
        if (!(l95 <= l90 && l90 <= p && p <= h90 && h90 <= h95)) { printf("FAIL wilson nesting k=%d\n", k); bad++; }
    }
    printf(bad ? "SELFTEST FAIL (%d)\n" : "SELFTEST PASS (Newcombe Table III: %d rows x methods 8 and 10; McNemar tail identity 31x31; Wilson nesting 61)\n", bad ? bad : (int)NT3);
    return bad ? 1 : 0;
}

static long num(const char *s, long lo, long hi)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (!*s || *e || v < lo || v > hi) { fprintf(stderr, "genstats: '%s' is not an integer in %ld..%ld\n", s, lo, hi); exit(2); }
    return v;
}

static int conf_arg(const char *s)
{
    if (!strcmp(s, "95")) return 95;
    if (!strcmp(s, "90")) return 90;
    fprintf(stderr, "genstats: confidence must be 95 or 90\n");
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "wilson")) {
        long n = num(argv[3], 1, 1000000), k = num(argv[2], 0, n);
        double p, lo, hi;
        wilson(k, n, zof(conf_arg(argv[4])), &p, &lo, &hi);
        printf("%.10f %.10f %.10f\n", p, lo, hi);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "mcnemar1")) {
        int b = (int)num(argv[2], 0, 120), c = (int)num(argv[3], 0, 120 - b);
        u128 n, d;
        char ns[48], ds[48];
        mcnemar1(b, c, &n, &d);
        u128str(n, ns);
        u128str(d, ds);
        printf("%s %s %.10f\n", ns, ds, (double)n / (double)d);
        return 0;
    }
    if ((argc == 7 || argc == 8) && !strcmp(argv[1], "newcombe")) {
        long a = num(argv[2], 0, 1000000), b = num(argv[3], 0, 1000000), c = num(argv[4], 0, 1000000), d = num(argv[5], 0, 1000000);
        if (a + b + c + d == 0) { fprintf(stderr, "genstats: n = 0\n"); return 2; }
        int m = argc == 8 ? (int)num(argv[7], 8, 10) : 10;
        if (m == 9) { fprintf(stderr, "genstats: method 8 or 10\n"); return 2; }
        double th, lo, hi;
        newcombe(a, b, c, d, zof(conf_arg(argv[6])), m, &th, &lo, &hi);
        printf("%.10f %.10f %.10f\n", th, lo, hi);
        return 0;
    }
    if (argc == 7 && !strcmp(argv[1], "verdict")) {
        Decision r;
        char err[160];
        if (decide((int)num(argv[2], 0, 1), argv + 3, &r, err, sizeof err)) { fprintf(stderr, "genstats: %s\n", err); return 1; }
        print_decision_cols(&r);
        putchar('\n');
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "vectors")) return vectors();
    if (argc == 2 && !strcmp(argv[1], "selftest")) return selftest();
    fprintf(stderr, "usage: genstats wilson <k> <n> <95|90> | mcnemar1 <b> <c> | newcombe <a> <b> <c> <d> <95|90> [8|10] | verdict <valid> <A> <B> <C> <D> | vectors | selftest\n");
    return 2;
}
