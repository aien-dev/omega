/* BRW-ACT-DEV0 runner (harness side: the world generator lives only here).
 * usage: brw_active_dev0 <dev|hold> <out-dir> <commit> [deviations]
 * Writes <out-dir>/table.tsv and <out-dir>/receipt.txt, both with O_EXCL (never overwritten).
 * Spec: docs/turing/BRW_ACT_DEV0_PROFILE.md. Held-out tag `hold` is run once by the orchestrator. */
#include "brownian/brw_active.h"
#include "sha256.h"
#include "turing/ty_prd2.h"
#include "turing/ty_qcont.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef BRW_FLAGS
#define BRW_FLAGS "unknown"
#endif
#ifndef BRW_PROFILE
#define BRW_PROFILE "docs/turing/BRW_ACT_DEV0_PROFILE.md"
#endif

#define BUDGET 400
#define NHELD_PER_TAU 10
#define NHELD (BRW_NT * NHELD_PER_TAU)
#define NBOOT 10000
#define BOOT_SEED 0x0B5E55ED00000001ull
enum { C_DIFF, C_DRIFT, C_OU, C_NOISE, C_MIS, NCLS };
enum { S_ACTIVE, S_RANDOM, S_FIXED8, S_CYCLE, NSTRAT };
static const char *cls_name[NCLS] = {"diffusion", "drift", "ou", "noise", "mismatch"};
static const char *str_name[NSTRAT] = {"active", "random", "fixed8", "cycle"};
#define TAG_DEV 0x646576ull        /* "dev" */
#define TAG_HOLD 0x686F6C64ull     /* "hold" */
#define TG_PARAM 0x5041524Dull     /* "PARM" */
#define TG_READ 0x52454144ull      /* "READ" */
#define TG_HELD 0x48454C44ull      /* "HELD" */
#define TG_RAND 0x52414E44ull      /* "RAND" */
#define PI_ 3.14159265358979323846

/* ---- randomness: splitmix64, xorshift64* (Vigna), Box-Muller cosine branch, Poisson by inversion ---- */
static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t mix(uint64_t a, uint64_t b) { return splitmix64(a ^ splitmix64(b)); }
typedef struct { uint64_t s; } xs_t;
static void xs_seed(xs_t *r, uint64_t seed) { r->s = seed ? seed : 0x9E3779B97F4A7C15ull; }
static uint64_t xs_next(xs_t *r)
{
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1Dull;
}
static double u_open0(xs_t *r) { return (double)((xs_next(r) >> 11) + 1) * 0x1p-53; }   /* (0, 1] */
static double u_half(xs_t *r) { return (double)(xs_next(r) >> 11) * 0x1p-53; }          /* [0, 1) */
static double rnorm(xs_t *r)
{
    double a = u_open0(r), b = u_half(r);
    return sqrt(-2.0 * log(a)) * cos(2.0 * PI_ * b);
}
static int rpois(xs_t *r, double lam)
{
    double u = u_half(r), p = exp(-lam), c = p;
    int k = 0;
    while (u > c && k < 1000) { k++; p *= lam / k; c += p; }
    return k;
}
static double logu(xs_t *r, double a, double b) { return exp(log(a) + u_half(r) * (log(b) - log(a))); }

/* ---- worlds ---- */
typedef struct { int cls; uint32_t idx; uint64_t seed; double D, v, theta; } world_t;

static void world_make(world_t *w, int cls, uint64_t tag, uint32_t idx)
{
    xs_t r;
    memset(w, 0, sizeof *w);
    w->cls = cls;
    w->idx = idx;
    w->seed = splitmix64(tag ^ ((uint64_t)cls << 32) ^ idx);
    xs_seed(&r, mix(w->seed, TG_PARAM));
    if (cls == C_DIFF || cls == C_DRIFT || cls == C_OU || cls == C_MIS) w->D = logu(&r, 0.05, 2.0);
    if (cls == C_DRIFT) {
        double sg = u_half(&r) < 0.5 ? 1.0 : -1.0;
        w->v = sg * logu(&r, 0.05, 0.5);
    }
    if (cls == C_OU) w->theta = logu(&r, 0.03, 0.5);
}

/* one reading at wait tau; draw order from the substream: normal, then (mismatch) Poisson, jump normals */
static double world_draw(const world_t *w, int tau, xs_t *r)
{
    double z = rnorm(r), mean = 0.0, var = 1.0;
    switch (w->cls) {
    case C_DIFF: var = 2.0 * w->D * tau + 1.0; break;
    case C_DRIFT: var = 2.0 * w->D * tau + 1.0; mean = w->v * tau; break;
    case C_OU: var = (w->D / w->theta) * (1.0 - exp(-2.0 * w->theta * tau)) + 1.0; break;
    case C_NOISE: break;
    case C_MIS: {
        var = 2.0 * w->D * tau + 1.0;
        double y = sqrt(var) * z;
        int nj = rpois(r, 0.05 * tau);
        for (int i = 0; i < nj; i++) y += 3.0 * rnorm(r);
        return y;
    }
    }
    return mean + sqrt(var) * z;
}
static void read_rng(const world_t *w, uint64_t tag, uint64_t j, xs_t *r) { xs_seed(r, mix(mix(w->seed, tag), j)); }

/* ---- one run ---- */
typedef struct {
    int outcome;                 /* 0 success, 1 wrong, 2 censored, 3 not applicable (control world) */
    int first_model;             /* model that first reached 0.99, -1 none */
    int first_cost, spent, nmeas;
    double cost_restr, p_true, bits;
    int verdict, c50, c90, floor_hits, fail;
    double pm[BRW_NM];
    uint64_t lik, quad, cdf;
    unsigned n_out99;
} run_t;

static run_t run_world(const brw_grid *g, const world_t *w, int strat, char *errbuf, size_t errlen)
{
    run_t R;
    brw_state s;
    xs_t rr, rd;
    memset(&R, 0, sizeof R);
    R.first_model = -1;
    brw_state_init(&s, g);
    xs_seed(&rr, mix(mix(w->seed, TG_RAND), 0));
    int spent = 0, cyc = 0;
    unsigned j = 0;
    for (;;) {
        int rem = BUDGET - spent, ti = -1;
        if (strat == S_ACTIVE) {
            ti = brw_choose(&s, rem, NULL);
        } else if (strat == S_RANDOM) {
            int ok[BRW_NT], n = 0;
            for (int t = 0; t < BRW_NT; t++) { ok[n] = t; n += BRW_COST(t) <= rem; }
            if (n) {
                ti = ok[(int)(u_half(&rr) * n)];
            }
        } else if (strat == S_FIXED8) {
            if (BRW_COST(3) <= rem) ti = 3;
            else for (int t = BRW_NT - 1; t >= 0; t--) if (BRW_COST(t) <= rem) { ti = t; break; }
        } else {
            for (int k = 0; k < BRW_NT; k++) {
                int t = (cyc + k) % BRW_NT;
                if (BRW_COST(t) <= rem) { ti = t; cyc = (t + 1) % BRW_NT; break; }
            }
        }
        if (ti < 0) break;
        read_rng(w, TG_READ, j, &rd);
        double y = world_draw(w, brw_tau[ti], &rd);
        if (brw_observe(&s, ti, y) < 0) { snprintf(errbuf, errlen, "non-finite reading"); R.fail = 1; break; }
        spent += BRW_COST(ti);
        j++;
        if (R.first_model < 0) {
            double p[BRW_NM];
            brw_model_post(&s, p);
            for (int m = 0; m < BRW_NM; m++)
                if (p[m] >= BRW_P_SURE) { R.first_model = m; R.first_cost = spent; break; }
        }
    }
    R.spent = spent;
    R.nmeas = (int)j;
    brw_model_post(&s, R.pm);
    R.verdict = brw_verdict(&s);
    R.n_out99 = s.n_out99;
    if (w->cls <= C_OU) {
        R.p_true = R.pm[w->cls];
        R.outcome = R.first_model < 0 ? 2 : (R.first_model == w->cls ? 0 : 1);
        R.cost_restr = R.outcome == 0 ? (double)R.first_cost : (double)BUDGET;
    } else {
        R.outcome = 3;
        R.p_true = -1.0;
        R.cost_restr = R.first_model < 0 ? (double)BUDGET : (double)R.first_cost;
    }
    /* held-out: 10 readings at each menu tau, substream no strategy reads */
    tyq_pred pr[BRW_NT];
    for (int t = 0; t < BRW_NT && !R.fail; t++)
        if (brw_predict_prd2(&s, t, 0, &pr[t]) != TYQ_OK) {
            snprintf(errbuf, errlen, "PRD2 validation refused at tau index %d", t);
            R.fail = 1;
        }
    double bsum = 0.0;
    for (int t = 0; t < BRW_NT && !R.fail; t++)
        for (int q = 0; q < NHELD_PER_TAU; q++) {
            xs_t rh;
            int idx = t * NHELD_PER_TAU + q;
            read_rng(w, TG_HELD, (uint64_t)idx, &rh);
            double y = world_draw(w, brw_tau[t], &rh), b;
            int fh = 0;
            tyq_pred p = pr[t];
            p.index = (uint32_t)idx;
            if (ty_qcont2_bits(&p, (int64_t)llround(y / TYQ_DELTA), &b, &fh) != TYQ_OK) {
                snprintf(errbuf, errlen, "ty_qcont2_bits refused held-out reading %d", idx);
                R.fail = 1;
                break;
            }
            bsum += b;
            R.floor_hits += fh;
            double u = brw_cdf(&s, t, y);
            R.c50 += (u >= 0.25 && u <= 0.75);
            R.c90 += (u >= 0.05 && u <= 0.95);
        }
    R.bits = bsum / NHELD;
    R.lik = s.lik_evals;
    R.quad = s.quad_evals;
    R.cdf = s.cdf_evals;
    return R;
}

/* ---- statistics ---- */
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void boot(const double *d, size_t n, double *mean, double *lo, double *hi)
{
    xs_t r;
    double *m = malloc(NBOOT * sizeof *m);
    double t = 0;
    for (size_t i = 0; i < n; i++) t += d[i];
    *mean = n ? t / (double)n : 0.0;
    *lo = *hi = 0.0;
    if (!m || !n) { free(m); return; }
    xs_seed(&r, BOOT_SEED);
    for (int b = 0; b < NBOOT; b++) {
        double s = 0;
        for (size_t i = 0; i < n; i++) s += d[(size_t)(u_half(&r) * (double)n)];
        m[b] = s / (double)n;
    }
    qsort(m, NBOOT, sizeof *m, dcmp);
    *lo = m[(size_t)(0.025 * NBOOT)];
    *hi = m[(size_t)(0.975 * NBOOT) - 1];
    free(m);
}

static void wilson(double k, double n, double *lo, double *hi)
{
    const double z = 1.959964;
    double p = k / n, d = 1.0 + z * z / n, c = p + z * z / (2 * n), h = z * sqrt(p * (1 - p) / n + z * z / (4 * n * n));
    *lo = (c - h) / d;
    *hi = (c + h) / d;
}

/* ---- growing text buffer ---- */
typedef struct { char *p; size_t n, cap; } buf_t;
static void bprintf(buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (;;) {
        va_list cp;
        va_copy(cp, ap);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->cap - b->n, fmt, cp);
        va_end(cp);
        if (k < 0) { va_end(ap); return; }
        if ((size_t)k < b->cap - b->n) { b->n += (size_t)k; break; }
        size_t nc = b->cap ? b->cap * 2 : 1 << 16;
        while (nc - b->n <= (size_t)k) nc *= 2;
        char *q = realloc(b->p, nc);
        if (!q) { va_end(ap); return; }
        b->p = q;
        b->cap = nc;
    }
    va_end(ap);
}

static void hex(const uint8_t d[32], char out[65]) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]); }
static int file_sha(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx c;
    uint8_t d[32], tmp[65536];
    size_t k;
    sha256_init(&c);
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) sha256_update(&c, tmp, k);
    fclose(f);
    sha256_final(&c, d);
    hex(d, out);
    return 0;
}
static int write_excl(const char *path, const char *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno)); return -1; }
    size_t off = 0;
    while (off < n) {
        ssize_t k = write(fd, data + off, n - off);
        if (k <= 0) { close(fd); return -1; }
        off += (size_t)k;
    }
    return close(fd);
}

/* ---- main ---- */
int main(int argc, char **argv)
{
    if (argc < 4 || (strcmp(argv[1], "dev") && strcmp(argv[1], "hold"))) {
        fprintf(stderr, "usage: %s <dev|hold> <out-dir> <commit> [deviations]\n", argv[0]);
        return 64;
    }
    const int is_dev = !strcmp(argv[1], "dev");
    const uint64_t tag = is_dev ? TAG_DEV : TAG_HOLD;
    const int nper = is_dev ? 40 : 100, nw = nper * NCLS;
    const char *outdir = argv[2], *commit = argv[3], *dev_note = argc > 4 ? argv[4] : "none";
    if (mkdir(outdir, 0755) && errno != EEXIST) { perror("mkdir"); return 1; }

    brw_grid *g = malloc(sizeof *g);
    run_t *R = calloc((size_t)nw * NSTRAT, sizeof *R);
    world_t *W = calloc((size_t)nw, sizeof *W);
    char (*err)[96] = calloc((size_t)nw * NSTRAT, 96);
    if (!g || !R || !W || !err) return 1;
    brw_grid_init(g);
    buf_t tab = {0}, rep = {0};
    bprintf(&tab, "world\tclass\tindex\tseed\tstrategy\toutcome\tfirst_model\tfirst_cost\tcost_restricted\tnmeas\tspent\tp_true\t"
                  "verdict\tp_M0\tp_M1\tp_M2\tn_out99\theld_bits_per_reading\tcov50_of60\tcov90_of60\tfloor_hits\tlik_evals\tquad_evals\tcdf_evals\n");
    static const char *oc[4] = {"success", "wrong", "censored", "control"};
    int nfail = 0;
    for (int c = 0, wi = 0; c < NCLS; c++)
        for (int i = 0; i < nper; i++, wi++) {
            world_make(&W[wi], c, tag, (uint32_t)i);
            for (int st = 0; st < NSTRAT; st++) {
                run_t *r = &R[wi * NSTRAT + st];
                *r = run_world(g, &W[wi], st, err[wi * NSTRAT + st], 96);
                nfail += r->fail;
                bprintf(&tab, "%d\t%s\t%d\t%016llx\t%s\t%s\t%d\t%d\t%.17g\t%d\t%d\t%.17g\t%s\t%.17g\t%.17g\t%.17g\t%u\t%.17g\t%d\t%d\t%d\t%llu\t%llu\t%llu\n",
                        wi, cls_name[c], i, (unsigned long long)W[wi].seed, str_name[st], oc[r->outcome], r->first_model,
                        r->first_model < 0 ? -1 : r->first_cost, r->cost_restr, r->nmeas, r->spent, r->p_true,
                        brw_verdict_name(r->verdict), r->pm[0], r->pm[1], r->pm[2], r->n_out99, r->bits, r->c50, r->c90,
                        r->floor_hits, (unsigned long long)r->lik, (unsigned long long)r->quad, (unsigned long long)r->cdf);
            }
        }
    uint8_t dg[32];
    char tabsha[65];
    sha256_hash((const uint8_t *)tab.p, tab.n, dg);
    hex(dg, tabsha);

    /* ---- analysis ---- */
    int nd = nper * 3;                       /* discoverable worlds */
    double *cost[NSTRAT], *bits[NSTRAT];
    for (int st = 0; st < NSTRAT; st++) {
        cost[st] = malloc((size_t)nd * sizeof(double));
        bits[st] = malloc((size_t)nd * sizeof(double));
        for (int wi = 0; wi < nd; wi++) {
            cost[st][wi] = R[wi * NSTRAT + st].cost_restr;
            bits[st][wi] = R[wi * NSTRAT + st].bits;
        }
    }
    bprintf(&rep, "== BRW-ACT-DEV0 %s: %d worlds (%d per class), %d discoverable ==\n", argv[1], nw, nper, nd);
    double frac[NSTRAT][3], meancost[NSTRAT], meannm[NSTRAT], meanp[NSTRAT], meanbits[NSTRAT], cov50[NSTRAT], cov90[NSTRAT];
    double lo50[NSTRAT], hi50[NSTRAT], lo90[NSTRAT], hi90[NSTRAT];
    uint64_t lik[NSTRAT] = {0}, quad[NSTRAT] = {0}, cdf[NSTRAT] = {0};
    for (int st = 0; st < NSTRAT; st++) {
        double f[3] = {0, 0, 0}, mc = 0, nm = 0, mp = 0, mb = 0, k50 = 0, k90 = 0;
        for (int wi = 0; wi < nd; wi++) {
            const run_t *r = &R[wi * NSTRAT + st];
            f[r->outcome] += 1;
            mc += r->cost_restr; nm += r->nmeas; mp += r->p_true; mb += r->bits; k50 += r->c50; k90 += r->c90;
        }
        for (int o = 0; o < 3; o++) frac[st][o] = f[o] / nd;
        meancost[st] = mc / nd; meannm[st] = nm / nd; meanp[st] = mp / nd; meanbits[st] = mb / nd;
        cov50[st] = k50 / ((double)nd * NHELD);
        cov90[st] = k90 / ((double)nd * NHELD);
        wilson(k50, (double)nd * NHELD, &lo50[st], &hi50[st]);
        wilson(k90, (double)nd * NHELD, &lo90[st], &hi90[st]);
        for (int wi = 0; wi < nw; wi++) {
            lik[st] += R[wi * NSTRAT + st].lik;
            quad[st] += R[wi * NSTRAT + st].quad;
            cdf[st] += R[wi * NSTRAT + st].cdf;
        }
    }
    bprintf(&rep, "\nDiscoverable classes pooled (restricted mean cost, horizon %d):\n", BUDGET);
    bprintf(&rep, "%-8s %7s %7s %7s %9s %6s %7s %9s %20s %20s\n", "strategy", "success", "wrong", "censor", "mean_cost", "n_meas",
            "P(true)", "bits/read", "cov50 [Wilson95]", "cov90 [Wilson95]");
    for (int st = 0; st < NSTRAT; st++)
        bprintf(&rep, "%-8s %7.4f %7.4f %7.4f %9.3f %6.2f %7.4f %9.4f %.4f [%.4f,%.4f] %.4f [%.4f,%.4f]\n", str_name[st],
                frac[st][0], frac[st][1], frac[st][2], meancost[st], meannm[st], meanp[st], meanbits[st], cov50[st], lo50[st],
                hi50[st], cov90[st], lo90[st], hi90[st]);
    bprintf(&rep, "\nPer class success/wrong/censored and mean cost:\n");
    for (int c = 0; c < 3; c++)
        for (int st = 0; st < NSTRAT; st++) {
            double f[3] = {0, 0, 0}, mc = 0;
            for (int i = 0; i < nper; i++) {
                const run_t *r = &R[(c * nper + i) * NSTRAT + st];
                f[r->outcome] += 1; mc += r->cost_restr;
            }
            bprintf(&rep, "  %-9s %-7s %.3f %.3f %.3f cost %.2f\n", cls_name[c], str_name[st], f[0] / nper, f[1] / nper, f[2] / nper, mc / nper);
        }
    double bmean[NSTRAT], blo[NSTRAT], bhi[NSTRAT], gmean[NSTRAT], glo[NSTRAT], ghi[NSTRAT];
    bprintf(&rep, "\nPaired bootstrap (%d resamples of worlds, xorshift64* seed 0x%016llX), active minus baseline, 95%% percentile:\n",
            NBOOT, (unsigned long long)BOOT_SEED);
    int p1 = 1;
    double *dd = malloc((size_t)nd * sizeof *dd);
    for (int st = 1; st < NSTRAT; st++) {
        for (int wi = 0; wi < nd; wi++) dd[wi] = cost[S_ACTIVE][wi] - cost[st][wi];
        boot(dd, (size_t)nd, &bmean[st], &blo[st], &bhi[st]);
        for (int wi = 0; wi < nd; wi++) dd[wi] = bits[S_ACTIVE][wi] - bits[st][wi];
        boot(dd, (size_t)nd, &gmean[st], &glo[st], &ghi[st]);
        bprintf(&rep, "  vs %-7s cost diff %9.3f [%9.3f, %9.3f]   held-out bits/reading diff %8.4f [%8.4f, %8.4f]\n", str_name[st],
                bmean[st], blo[st], bhi[st], gmean[st], glo[st], ghi[st]);
        if (!(bhi[st] < 0.0)) p1 = 0;
    }
    free(dd);
    int ctl_noise[NSTRAT][5] = {{0}}, ctl_mis[NSTRAT][5] = {{0}};
    for (int st = 0; st < NSTRAT; st++)
        for (int i = 0; i < nper; i++) {
            ctl_noise[st][R[((C_NOISE)*nper + i) * NSTRAT + st].verdict]++;
            ctl_mis[st][R[((C_MIS)*nper + i) * NSTRAT + st].verdict]++;
        }
    bprintf(&rep, "\nControl verdict counts (INADEQUATE, M0, M1, M2, UNDETERMINED) of %d worlds each:\n", nper);
    for (int st = 0; st < NSTRAT; st++)
        bprintf(&rep, "  %-7s noise %d %d %d %d %d    mismatch %d %d %d %d %d\n", str_name[st], ctl_noise[st][0], ctl_noise[st][1],
                ctl_noise[st][2], ctl_noise[st][3], ctl_noise[st][4], ctl_mis[st][0], ctl_mis[st][1], ctl_mis[st][2],
                ctl_mis[st][3], ctl_mis[st][4]);
    bprintf(&rep, "\nCompute per strategy, all %d worlds (likelihood evals / design-quadrature evals / cdf evals):\n", nw);
    for (int st = 0; st < NSTRAT; st++)
        bprintf(&rep, "  %-7s %llu / %llu / %llu\n", str_name[st], (unsigned long long)lik[st], (unsigned long long)quad[st],
                (unsigned long long)cdf[st]);

    double maxbase = 0.0;
    for (int st = 1; st < NSTRAT; st++) if (frac[st][1] > maxbase) maxbase = frac[st][1];
    int p2 = frac[S_ACTIVE][1] <= 0.05 && frac[S_ACTIVE][1] <= maxbase + 0.02;
    int p3 = cov90[S_ACTIVE] >= 0.85 && cov90[S_ACTIVE] <= 0.95 && cov50[S_ACTIVE] >= 0.45 && cov50[S_ACTIVE] <= 0.55;
    int claims = ctl_noise[S_ACTIVE][BRW_V_M1] + ctl_noise[S_ACTIVE][BRW_V_M2];
    int p4 = (double)claims <= 0.05 * nper;
    int confident = ctl_mis[S_ACTIVE][BRW_V_M0] + ctl_mis[S_ACTIVE][BRW_V_M1] + ctl_mis[S_ACTIVE][BRW_V_M2];
    int p5 = (double)confident <= 0.20 * nper;
    bprintf(&rep, "\nPass rules (%s worlds):\n", argv[1]);
    bprintf(&rep, "  P1 active cost interval entirely below 0 vs random, fixed8, cycle: %s\n", p1 ? "PASS" : "FAIL");
    bprintf(&rep, "  P2 active wrong %.4f <= 0.05 and <= max baseline wrong %.4f + 0.02: %s\n", frac[S_ACTIVE][1], maxbase, p2 ? "PASS" : "FAIL");
    bprintf(&rep, "  P3 active coverage 90%% %.4f in [0.85,0.95], 50%% %.4f in [0.45,0.55]: %s\n", cov90[S_ACTIVE], cov50[S_ACTIVE], p3 ? "PASS" : "FAIL");
    bprintf(&rep, "  P4 noise control: active claims M1 or M2 in %d of %d worlds (limit %.1f): %s\n", claims, nper, 0.05 * nper, p4 ? "PASS" : "FAIL");
    bprintf(&rep, "  P5 mismatch control: active confident model verdict in %d of %d worlds (limit %.1f): %s\n", confident, nper, 0.20 * nper, p5 ? "PASS" : "FAIL");
    bprintf(&rep, "BRW_ACT_DEV0 %s: %s\n", argv[1], (p1 && p2 && p3 && p4 && p5 && !nfail) ? "PASS" : "FAIL");
    bprintf(&rep, "failed runs (validation or scorer refusals): %d\n", nfail);
    for (int k = 0; k < nw * NSTRAT; k++)
        if (R[k].fail) bprintf(&rep, "  FAILURE world %d strategy %s: %s\n", k / NSTRAT, str_name[k % NSTRAT], err[k]);

    /* ---- receipt ---- */
    buf_t rc = {0};
    char sha[65];
    bprintf(&rc, "BRW-ACT-DEV0 receipt\n");
    bprintf(&rc, "tag: %s\nomega_commit: %s\n", argv[1], commit);
    bprintf(&rc, "profile: %s sha256 %s\n", BRW_PROFILE, file_sha(BRW_PROFILE, sha) ? "UNREADABLE" : sha);
    static const char *srcs[] = {"tools/brownian/brw_active.c", "tools/brownian/brw_active.h", "tests/brownian/brw_active_dev0.c",
                                 "tests/brownian/test_brw_active.c", "mk/brownian_active.mk", "src/turing/ty_prd2.c",
                                 "src/turing/ty_prd2.h", "src/turing/ty_qcont.c", "src/turing/ty_qcont.h", "src/turing/ty_prd.c",
                                 "src/turing/ty_prd.h", "src/turing/ty_math.c", "src/turing/ty_math.h", "src/sha256.c", "src/sha256.h"};
    for (size_t i = 0; i < sizeof srcs / sizeof *srcs; i++)
        bprintf(&rc, "source: %s sha256 %s\n", srcs[i], file_sha(srcs[i], sha) ? "UNREADABLE" : sha);
    bprintf(&rc, "runner_binary: /proc/self/exe sha256 %s\n", file_sha("/proc/self/exe", sha) ? "UNREADABLE" : sha);
    bprintf(&rc, "table: table.tsv sha256 %s\n", tabsha);
    bprintf(&rc, "compiler: %s\nflags: %s\n", __VERSION__, BRW_FLAGS);
    bprintf(&rc, "rng: world seed splitmix64(tag ^ (class<<32) ^ index), tag dev=0x%llX hold=0x%llX; class order diffusion,drift,ou,noise,mismatch = 0..4;\n"
                 "     substream(world, tag, j) = splitmix64(world_seed ^ splitmix64(tag)) mixed the same way with j; tags PARM=0x%llX READ=0x%llX HELD=0x%llX RAND=0x%llX;\n"
                 "     xorshift64* (Vigna: 12,25,27; multiplier 0x2545F4914F6CDD1D); uniforms from top 53 bits; Box-Muller cosine branch; Poisson by inversion;\n"
                 "     held-out reading idx = tau_index*10 + q from substream tag HELD; bootstrap xorshift64* seed 0x%016llX, %d resamples, percentile indices 250 and 9749\n",
            (unsigned long long)TAG_DEV, (unsigned long long)TAG_HOLD, (unsigned long long)TG_PARAM, (unsigned long long)TG_READ,
            (unsigned long long)TG_HELD, (unsigned long long)TG_RAND, (unsigned long long)BOOT_SEED, NBOOT);
    bprintf(&rc, "budget: %d time units per run, menu {1,2,4,8,16,32}, cost tau+2, sigma_m 1, held-out readings per world %d\n", BUDGET, NHELD);
    bprintf(&rc, "worlds: %d per class, %d total\n", nper, nw);
    bprintf(&rc, "deviations: %s\n", dev_note);
    bprintf(&rc, "replay: not guaranteed across machines (libm exp, log, sqrt, cos, erfc, lgamma)\n\n");
    bprintf(&rc, "%s", rep.p);
    char path[1024];
    snprintf(path, sizeof path, "%s/table.tsv", outdir);
    int rcode = write_excl(path, tab.p, tab.n);
    snprintf(path, sizeof path, "%s/receipt.txt", outdir);
    if (!rcode) rcode = write_excl(path, rc.p, rc.n);
    fputs(rep.p, stdout);
    printf("table_sha256 %s\n", tabsha);
    return rcode ? 1 : 0;
}
