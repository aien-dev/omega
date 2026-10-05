/* ESTIMATION v6 pre-freeze operating-characteristics simulation (docs/estimation/protocols/est-v6.md
 * section 6, section 9 item 3; appendix est-v6-appendix-prefreeze.md). Development tool: it reads NO
 * data file at all, so no held-out file can be touched. It simulates the pooled H1 + H2 scoring of the
 * primary gates P1 to P10 and the three-way verdict on synthetic PIT sequences.
 *
 *   est6opchar run --case a|b|c|d [--bin0 X] [--reps N] [--boot B] [--runs R] [--seconds S]
 *                  [--w W] [--rho RHO] [--rho10 RHO10] [--seed SEED]
 *   est6opchar deff [--reps N] [--runs R] [--w W] [--rho RHO] [--seed SEED]
 *   est6opchar selftest
 *
 * Generator. Each run has 20..120 s segments (splitmix64, est_load.c draw order, level uniform on
 * {0,6,12,18}) and one step per second after a 30 s burn-in. Latent z_t = sqrt(W) b_k + sqrt(1 - W) a_t
 * with b_k ~ N(0,1) per segment and a_t AR(1) with coefficient RHO, so z_t ~ N(0,1) marginally. The
 * randomised PIT is u = G(Phi(z)):
 *   case a  G = identity (perfectly calibrated forecaster);
 *   case b  u = Phi(s z) with s chosen so the nominal 95 % interval truly covers 92 %;
 *   case c  G^-1 = piecewise-uniform CDF over cells built from the v5 D1 in-sample PIT shares
 *           (--bin0 X rescales bin 0 to X and the other bins pro rata; default 0.0760 is the D1 value).
 * The ten-step PIT sequence for P7 is an independent sequence from the same generator with RHO10
 * (overlapping ten-step windows are strongly dependent); its marginal coverage is 0.95 (a), 0.92 (b) or
 * the D1 value 0.9402 (c, by the same scale trick).
 *
 * Gates, verdict. As protocol section 6 on the pooled steps; intervals are 99.5 % block-bootstrap
 * percentiles over whole segments for P1 to P7 and mean z; the lag-1 part of P8 and the P9 quarter and
 * regime coverages use the point value only (interval = point), which can only make HELD_OUT_FAIL more
 * likely than the real tool. The baseline comparison is not simulated (assumed met). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
static u64 splitmix64(u64 *s)
{
    u64 z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static double unif(u64 *s) { return ((double)(splitmix64(s) >> 11) + 0.5) / 9007199254740992.0; }
static double gauss(u64 *s)
{
    double u1 = unif(s), u2 = unif(s);
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}
static double Phi(double x) { return 0.5 * erfc(-x / 1.4142135623730951); }
/* Acklam inverse normal CDF, then one Halley step (error ~1e-15) */
static double probit(double p)
{
    static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
    static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
    static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00 };
    double x;
    if (p < 0.02425) { double q = sqrt(-2 * log(p)); x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    else if (p > 1 - 0.02425) { double q = sqrt(-2 * log(1 - p)); x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
    else { double q = p - 0.5, r = q * q; x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1); }
    double e = Phi(x) - p, u = e * 2.5066282746310002 * exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

/* ---- case c cells: cut points and masses (v5 D1 in-sample shares, receipts/est-v5/params.txt and
 * est6dev g1pit: lower miss 0.0122 and upper miss 0.0349 are inside bins 0 and 9) ---- */
#define NCELL 12
static const double D1_BIN[10] = { 0.075981, 0.115129, 0.105204, 0.098830, 0.105904, 0.108086, 0.098644, 0.083913, 0.085837, 0.122472 };
static const double D1_LOW = 0.012163, D1_UP = 0.034887;
static double cell_cut[NCELL + 1], cell_cum[NCELL + 1];
static void make_cells(double bin0)
{
    double m[NCELL], rest = 0;
    for (int j = 1; j < 10; j++) rest += D1_BIN[j];
    double scale = (1.0 - bin0) / rest;
    double b[10];
    b[0] = bin0;
    for (int j = 1; j < 10; j++) b[j] = D1_BIN[j] * scale;
    double lfrac = D1_LOW / D1_BIN[0], ufrac = D1_UP / D1_BIN[9];
    m[0] = b[0] * lfrac; m[1] = b[0] - m[0];
    for (int j = 1; j < 9; j++) m[j + 1] = b[j];
    m[10] = b[9] - b[9] * ufrac; m[11] = b[9] * ufrac;
    double cut[NCELL + 1] = { 0, 0.025, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.975, 1.0 };
    double cum = 0;
    for (int i = 0; i <= NCELL; i++) { cell_cut[i] = cut[i]; cell_cum[i] = cum; if (i < NCELL) cum += m[i]; }
    cell_cum[NCELL] = 1.0;
}
/* u such that the cell CDF at u equals p */
static double cells_inv(double p)
{
    for (int i = 0; i < NCELL; i++)
        if (p <= cell_cum[i + 1] || i == NCELL - 1) {
            double w = cell_cum[i + 1] - cell_cum[i];
            double f = w > 0 ? (p - cell_cum[i]) / w : 0.5;
            return cell_cut[i] + f * (cell_cut[i + 1] - cell_cut[i]);
        }
    return 1.0;
}
/* case d: regime-dependent PIT. Per declared load level (idle, L6, L12, L18) a piecewise-uniform PIT built from the
 * measured v5 D1 per-level G1 shares (appendix section 2: bin 0, bin 9, lower miss, upper miss); bins 1 to 8 share the
 * remaining mass equally (an ASSUMPTION: per-level middle bins were not tabulated). */
static const double LV_BIN0[4] = { 0.116, 0.111, 0.025, 0.000 }, LV_BIN9[4] = { 0.077, 0.114, 0.159, 0.184 };
static const double LV_LOW[4] = { 0.030, 0.010, 0.000, 0.000 }, LV_UP[4] = { 0.036, 0.017, 0.041, 0.085 };
static double lv_cum[4][NCELL + 1];
static void make_level_cells(void)
{
    for (int l = 0; l < 4; l++) {
        double m[NCELL], mid = (1.0 - LV_BIN0[l] - LV_BIN9[l]) / 8.0;
        m[0] = LV_LOW[l]; m[1] = LV_BIN0[l] - LV_LOW[l];
        for (int j = 1; j < 9; j++) m[j + 1] = mid;
        m[10] = LV_BIN9[l] - LV_UP[l]; m[11] = LV_UP[l];
        double cum = 0;
        for (int i = 0; i <= NCELL; i++) { lv_cum[l][i] = cum; if (i < NCELL) cum += m[i]; }
        lv_cum[l][NCELL] = 1.0;
    }
}
static double level_inv(int l, double p)
{
    static const double cut[NCELL + 1] = { 0, 0.025, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.975, 1.0 };
    for (int i = 0; i < NCELL; i++) {
        double w = lv_cum[l][i + 1] - lv_cum[l][i];
        if ((p <= lv_cum[l][i + 1] && w > 0) || i == NCELL - 1) {
            double f = w > 0 ? (p - lv_cum[l][i]) / w : 0.5;
            return cut[i] + f * (cut[i + 1] - cut[i]);
        }
    }
    return 1.0;
}
/* scale s so that P(|s z| < 1.959964) = cov */
static double scale_for_cov(double cov)
{
    double lo = 0.5, hi = 3.0;
    for (int i = 0; i < 80; i++) {
        double s = 0.5 * (lo + hi);
        double c = 2 * Phi(1.959963984540054 / s) - 1;
        if (c > cov) lo = s; else hi = s;
    }
    return 0.5 * (lo + hi);
}

typedef struct { int nrun; long seconds; double w, rho, rho10; int cas; double bin0; double s_b, s_b10, s_c10; } cfg_t;

#define MAXN 20000
#define MAXB 1000
typedef struct {
    int n, nb;
    double u[MAXN], z[MAXN];
    int blk[MAXN]; unsigned char idle[MAXN];
    int bstart[MAXB], blevel[MAXB];
} seq_t;

/* one sequence over nrun runs; kind 0 = one-step, 1 = ten-step */
static void gen_seq(const cfg_t *g, u64 *rng, int kind, seq_t *sq)
{
    double rho = kind ? g->rho10 : g->rho;
    int n = 0, nb = 0;
    for (int r = 0; r < g->nrun; r++) {
        long acc = 0, step = 0;
        double a = gauss(rng);
        while (acc < g->seconds && nb < MAXB - 1) {
            int lv = (int)(splitmix64(rng) % 4u);
            long d = 20 + (long)(splitmix64(rng) % 101u);
            if (acc + d > g->seconds) d = g->seconds - acc;
            double b = gauss(rng);
            sq->bstart[nb] = n; sq->blevel[nb] = lv;
            for (long k = 0; k < d; k++, step++) {
                a = rho * a + sqrt(1 - rho * rho) * gauss(rng);
                double z0 = sqrt(g->w) * b + sqrt(1 - g->w) * a;
                if (step >= 30 && n < MAXN) {
                    double u, zz;
                    if (g->cas == 1) { double s = kind ? g->s_b10 : g->s_b; zz = s * z0; u = Phi(zz); }
                    else if (g->cas == 3 && kind == 0) { u = level_inv(lv, Phi(z0)); zz = probit(u < 1e-12 ? 1e-12 : u > 1 - 1e-12 ? 1 - 1e-12 : u); }
                    else if (g->cas == 3) { zz = g->s_c10 * z0; u = Phi(zz); }
                    else if (g->cas == 2 && kind == 0) { u = cells_inv(Phi(z0)); zz = probit(u < 1e-12 ? 1e-12 : u > 1 - 1e-12 ? 1 - 1e-12 : u); }
                    else if (g->cas == 2) { zz = g->s_c10 * z0; u = Phi(zz); }
                    else { zz = z0; u = Phi(z0); }
                    sq->u[n] = u; sq->z[n] = zz; sq->blk[n] = nb; sq->idle[n] = lv == 0; n++;
                }
            }
            acc += d; nb++;
        }
    }
    sq->n = n; sq->nb = nb;
    sq->bstart[nb] = n;
}

/* statistic vector: cov50 cov80 cov95 low up bin0..9 z */
#define NS 16
static void contrib1(double u, double z, double v[NS])
{
    double d = fabs(u - 0.5);
    v[0] = d < 0.25; v[1] = d < 0.4; v[2] = d < 0.475; v[3] = u < 0.025; v[4] = u > 0.975;
    int j = (int)(u * 10); if (j > 9) j = 9; if (j < 0) j = 0;
    for (int k = 0; k < 10; k++) v[5 + k] = (k == j);
    v[15] = z;
}
static const double LO[NS] = { 0.46, 0.76, 0.93, 0.010, 0.010, 0.07, 0.07, 0.07, 0.07, 0.07, 0.07, 0.07, 0.07, 0.07, 0.07, -0.10 };
static const double HI[NS] = { 0.54, 0.84, 0.97, 0.040, 0.040, 0.13, 0.13, 0.13, 0.13, 0.13, 0.13, 0.13, 0.13, 0.13, 0.13, 0.10 };

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

/* outcome 0 PASS, 1 NOT_ESTABLISHED, 2 HELD_OUT_FAIL */
static int one_rep(const cfg_t *g, u64 *rng, int boot)
{
    static seq_t s1, s10;
    static double bs[MAXB][NS], bs10[MAXB];
    static double lo[NS], hi[NS], val[NS], *draw[NS + 1];
    gen_seq(g, rng, 0, &s1);
    gen_seq(g, rng, 1, &s10);
    memset(bs, 0, sizeof bs); memset(bs10, 0, sizeof bs10);
    double tot[NS] = { 0 }, bn[MAXB] = { 0 }, bn10[MAXB] = { 0 }, tot10 = 0;
    for (int i = 0; i < s1.n; i++) {
        double v[NS]; contrib1(s1.u[i], s1.z[i], v);
        for (int k = 0; k < NS; k++) { bs[s1.blk[i]][k] += v[k]; tot[k] += v[k]; }
        bn[s1.blk[i]] += 1;
    }
    for (int i = 0; i < s10.n; i++) { double c = fabs(s10.u[i] - 0.5) < 0.475; bs10[s10.blk[i]] += c; tot10 += c; bn10[s10.blk[i]] += 1; }
    int n = s1.n, n10 = s10.n;
    for (int k = 0; k < NS; k++) val[k] = tot[k] / n;
    double v10 = tot10 / n10;
    /* bootstrap */
    static double buf[NS + 1][2000];
    if (boot > 2000) boot = 2000;
    for (int k = 0; k <= NS; k++) draw[k] = buf[k];
    for (int r = 0; r < boot; r++) {
        double sum[NS] = { 0 }, cnt = 0, sum10 = 0, cnt10 = 0;
        for (int i = 0; i < s1.nb; i++) { int b = (int)(splitmix64(rng) % (u64)s1.nb); for (int k = 0; k < NS; k++) sum[k] += bs[b][k]; cnt += bn[b]; }
        for (int i = 0; i < s10.nb; i++) { int b = (int)(splitmix64(rng) % (u64)s10.nb); sum10 += bs10[b]; cnt10 += bn10[b]; }
        for (int k = 0; k < NS; k++) draw[k][r] = sum[k] / cnt;
        draw[NS][r] = sum10 / cnt10;
    }
    int ilo = (int)floor(0.0025 * (boot - 1)), ihi = (int)ceil(0.9975 * (boot - 1));
    for (int k = 0; k <= NS; k++) {
        qsort(draw[k], (size_t)boot, sizeof(double), cmpd);
        if (k < NS) { lo[k] = draw[k][ilo]; hi[k] = draw[k][ihi]; }
    }
    double lo10 = draw[NS][ilo], hi10 = draw[NS][ihi];
    int fail = 0, definite = 0;
    for (int k = 0; k < NS; k++) {
        if (val[k] < LO[k]) { fail = 1; if (hi[k] < LO[k]) definite = 1; }
        if (val[k] > HI[k]) { fail = 1; if (lo[k] > HI[k]) definite = 1; }
    }
    if (v10 < 0.90) { fail = 1; if (hi10 < 0.90) definite = 1; }
    if (v10 > 0.99) { fail = 1; if (lo10 > 0.99) definite = 1; }
    /* P8 lag-1 of z, point only */
    double zm = val[15], num = 0, den = 0;
    for (int i = 0; i < n; i++) { den += (s1.z[i] - zm) * (s1.z[i] - zm); if (i) num += (s1.z[i] - zm) * (s1.z[i - 1] - zm); }
    if (fabs(num / den) > 0.20) { fail = 1; definite = 1; }
    /* P9 quarters and regimes, point only */
    for (int q = 0; q < 4; q++) {
        long a = (long)q * n / 4, b = (long)(q + 1) * n / 4; double c = 0;
        for (long i = a; i < b; i++) c += fabs(s1.u[i] - 0.5) < 0.475;
        c /= (double)(b - a);
        if (c < 0.90 || c > 0.99) { fail = 1; definite = 1; }
    }
    for (int rg = 0; rg < 2; rg++) {
        double c = 0, m = 0;
        for (int i = 0; i < n; i++) if (s1.idle[i] == rg) { c += fabs(s1.u[i] - 0.5) < 0.475; m++; }
        if (m >= 100 && (c / m < 0.90 || c / m > 0.99)) { fail = 1; definite = 1; }
    }
    if (n < 4000) { fail = 1; definite = 1; }
    return !fail ? 0 : definite ? 2 : 1;
}

static void setup(cfg_t *g)
{
    g->s_b = scale_for_cov(0.92);
    g->s_b10 = scale_for_cov(0.92);
    g->s_c10 = scale_for_cov(0.9402);
}

static int cmd_run(int argc, char **argv)
{
    cfg_t g = { 2, 2700, 0.08, 0.02, 0.90, 0, 0.075981, 0, 0, 0 };
    int reps = 1000, boot = 1000; u64 seed = 0xE6A001ull; char cs = 'a';
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--case") && i + 1 < argc) cs = argv[++i][0];
        else if (!strcmp(argv[i], "--bin0") && i + 1 < argc) g.bin0 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc) g.nrun = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g.seconds = atol(argv[++i]);
        else if (!strcmp(argv[i], "--w") && i + 1 < argc) g.w = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rho") && i + 1 < argc) g.rho = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rho10") && i + 1 < argc) g.rho10 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else { fprintf(stderr, "est6opchar run: bad argument %s\n", argv[i]); return 2; }
    }
    if (cs != 'a' && cs != 'b' && cs != 'c' && cs != 'd') { fprintf(stderr, "case must be a, b, c or d\n"); return 2; }
    if (g.nrun < 1 || g.nrun > 4 || g.seconds < 100 || g.seconds > 4000 || reps < 1 || boot < 200 || g.w < 0 || g.w >= 1 || g.rho < 0 || g.rho >= 1 || g.rho10 < 0 || g.rho10 >= 1) { fprintf(stderr, "bad parameter range\n"); return 2; }
    g.cas = cs == 'a' ? 0 : cs == 'b' ? 1 : cs == 'c' ? 2 : 3;
    setup(&g);
    make_cells(g.bin0);
    make_level_cells();
    u64 rng = seed;
    int cnt[3] = { 0, 0, 0 };
    for (int r = 0; r < reps; r++) cnt[one_rep(&g, &rng, boot)]++;
    printf("case %c bin0 %.4f runs %d seconds %ld w %.3f rho %.3f rho10 %.3f reps %d boot %d seed 0x%llx\n", cs, g.bin0, g.nrun, g.seconds, g.w, g.rho, g.rho10, reps, boot, seed);
    printf("PASS %.4f NOT_ESTABLISHED %.4f HELD_OUT_FAIL %.4f (counts %d %d %d; Monte Carlo SE at most %.4f)\n", (double)cnt[0] / reps, (double)cnt[1] / reps, (double)cnt[2] / reps, cnt[0], cnt[1], cnt[2], 0.5 / sqrt((double)reps));
    return 0;
}

/* design effect of a bin-0 share (p = 0.1) over reps of calibrated case-a sequences, no bootstrap */
static int cmd_deff(int argc, char **argv)
{
    cfg_t g = { 1, 2700, 0.08, 0.02, 0.90, 0, 0, 0, 0, 0 };
    int reps = 2000; u64 seed = 0xE6A002ull;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc) g.nrun = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--w") && i + 1 < argc) g.w = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rho") && i + 1 < argc) g.rho = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else { fprintf(stderr, "est6opchar deff: bad argument %s\n", argv[i]); return 2; }
    }
    static seq_t s;
    u64 rng = seed;
    double m = 0, m2 = 0, nn = 0;
    for (int r = 0; r < reps; r++) {
        gen_seq(&g, &rng, 0, &s);
        double c = 0;
        for (int i = 0; i < s.n; i++) c += s.u[i] < 0.1;
        c /= s.n; nn += s.n; m += c; m2 += c * c;
    }
    m /= reps; m2 /= reps; nn /= reps;
    double var = m2 - m * m;
    printf("deff w %.3f rho %.3f runs %d reps %d mean_steps %.0f bin0_mean %.4f bin0_se %.5f design_effect %.3f\n", g.w, g.rho, g.nrun, reps, nn, m, sqrt(var), var / (0.1 * 0.9 / nn));
    return 0;
}

static int check(int ok, const char *what) { if (!ok) fprintf(stderr, "FAIL %s\n", what); return !ok; }

static int cmd_selftest(void)
{
    int bad = 0;
    bad += check(fabs(Phi(probit(0.975)) - 0.975) < 1e-12, "probit round trip");
    bad += check(fabs(probit(0.5)) < 1e-12 && fabs(probit(0.975) - 1.959963984540054) < 1e-9, "probit values");
    bad += check(fabs(2 * Phi(1.959963984540054 / scale_for_cov(0.92)) - 1 - 0.92) < 1e-9, "scale for 92 percent");
    make_cells(0.075981);
    bad += check(fabs(cell_cum[2] - 0.075981) < 1e-9 && fabs(cell_cum[11] - (1 - 0.034887)) < 1e-9, "cells reproduce D1 bin shares");
    bad += check(fabs(cells_inv(0.075981) - 0.1) < 1e-9 && fabs(cells_inv(0.012163) - 0.025) < 1e-9, "cell inverse");
    cfg_t g = { 2, 2700, 0.08, 0.02, 0.90, 0, 0.075981, 0, 0, 0 };
    setup(&g);
    u64 r1 = 77, r2 = 77;
    static seq_t a, b;
    gen_seq(&g, &r1, 0, &a); gen_seq(&g, &r2, 0, &b);
    bad += check(a.n == b.n && a.n > 5000 && a.n < 5400 && !memcmp(a.u, b.u, sizeof(double) * (size_t)a.n), "deterministic, about 2 x 2650 steps");
    double m = 0; for (int i = 0; i < a.n; i++) m += a.u[i];
    bad += check(fabs(m / a.n - 0.5) < 0.05, "case a PIT mean near 0.5");
    /* case a at large n: pass; case b: never pass; deterministic outcomes */
    g.cas = 0; u64 r3 = 5; int pass_a = 0;
    for (int i = 0; i < 20; i++) pass_a += one_rep(&g, &r3, 300) == 0;
    bad += check(pass_a >= 14, "calibrated forecaster passes most reps");
    g.cas = 1; u64 r4 = 6; int pass_b = 0;
    for (int i = 0; i < 20; i++) pass_b += one_rep(&g, &r4, 300) == 0;
    bad += check(pass_b == 0, "92 percent coverage never passes");
    if (!bad) printf("est6opchar selftest: PASS\n");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "run")) return cmd_run(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "deff")) return cmd_deff(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return cmd_selftest();
    fprintf(stderr, "usage: est6opchar run|deff|selftest ...\n");
    return 2;
}
