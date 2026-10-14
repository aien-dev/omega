/* res_qualify bench-narma | bench-lorenz | perf: the Gate 2 benchmark and
 * performance protocol of docs/reservoir/RESERVOIR_REFERENCE.md section 10.
 * Omega's own data from Omega's own generator; every seed is reported,
 * failures included. Seeds run in parallel threads (one per seed); every
 * result depends only on its seed. */
#define _GNU_SOURCE
#include "res_common.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>

#define NSEED_MAX 16
#define STEP_S 1.0e-5f
#define KSTEP 10u /* steps per knot: knots every 0.1 ms */

static const double GRID[9] = {1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1.0, 1e1, 1e2};

/* One generator stream per (seed, purpose). */
enum { P_NARMA_TRAIN = 1, P_NARMA_TEST = 2, P_NARMA_MASK = 3, P_LORENZ_TRAIN = 4, P_LORENZ_TEST = 5, P_PERF = 6 };
static ResRng stream(int seed, int purpose)
{
    ResRng r = {((uint64_t)(uint32_t)seed << 32) | (uint64_t)purpose};
    return r;
}

static void jd(FILE *o, double v)
{
    if (v == v && v - v == 0.0) fprintf(o, "%.10g", v);
    else fputs("null", o);
}

static void jhex(FILE *o, const uint8_t *d)
{
    char h[65];
    res_hex(d, 32, h);
    fprintf(o, "\"%s\"", h);
}

typedef struct {
    OmegaResStatus st;
    OmegaResRunReceipt rc;
    uint64_t stopped_at;
    OmegaResMembraneStats ms; /* every accepted state of the run */
} Run;

static void run_json(FILE *o, const char *name, const Run *r)
{
    fprintf(o, "{\"run\": \"%s\", \"status\": \"%s\"", name, res_status_name(r->st));
    if (r->st == OMEGA_RES_OK) {
        fputs(", \"run_id\": ", o);
        jhex(o, r->rc.run_id);
        fputs(", \"config_id\": ", o);
        jhex(o, r->rc.config_id);
        fprintf(o, ", \"steps\": %llu", (unsigned long long)r->rc.steps);
    } else
        fprintf(o, ", \"stopped_at_step\": %llu", (unsigned long long)r->stopped_at);
    res_mstats_json(o, &r->ms);
    fputc('}', o);
}

/* Builds and runs one reservoir from rest; records all node voltages at the
 * integer steps (non-decreasing); then runs to the last knot and takes the
 * run receipt. */
static Run simulate(const OmegaResTopology *t, const OmegaResParams *p, OmegaResInit init, float initv,
                    const float *tab, uint32_t knots, const uint64_t *steps, size_t ns, double *v)
{
    Run r;
    memset(&r, 0, sizeof r);
    r.st = OMEGA_RES_ERR_NULL;
    ResSim *sim = calloc(1, sizeof *sim);
    OmegaResIntegrator ig = {STEP_S, KSTEP};
    if (!sim) return r;
    r.st = omega_res_config_build(t, p, &ig, &sim->cfg);
    if (r.st == OMEGA_RES_OK) r.st = omega_res_state_init(&sim->cfg, init, initv, &sim->s);
    OmegaResInput in = {t->node_count, knots, tab};
    const uint64_t last = (uint64_t)(knots - 1u) * KSTEP;
    if (r.st == OMEGA_RES_OK) r.st = res_sim_at_steps(sim, &in, steps, ns, v);
    if (r.st == OMEGA_RES_OK && sim->s.step < last)
        r.st = omega_res_advance_observed(&sim->cfg, &in, &sim->ws, &sim->s, last - sim->s.step, &sim->mstats);
    if (r.st == OMEGA_RES_OK) r.st = omega_res_run_receipt(&sim->cfg, &in, &sim->s, &r.rc);
    r.stopped_at = sim->s.step;
    r.ms = sim->mstats;
    free(sim);
    return r;
}

typedef struct {
    double test[3], train[3], lambda, cv;
    OmegaResStatus st;
    uint32_t nf;
} Fit;

static Fit fit(const double *xtr, const double *ytr, uint32_t ntr, const double *xte, const double *yte, uint32_t nte,
               uint32_t nf, uint32_t no)
{
    Fit f;
    memset(&f, 0, sizeof f);
    f.nf = nf;
    double *ptr = malloc((size_t)ntr * no * sizeof(double)), *pte = malloc((size_t)nte * no * sizeof(double));
    if (!ptr || !pte) {
        f.st = OMEGA_RES_ERR_NULL;
    } else {
        ResCvResult cv = res_ridge_cv(xtr, ytr, ntr, nf, no, xte, nte, GRID, 9, 5, ptr, pte);
        f.st = cv.status;
        f.lambda = cv.lambda;
        f.cv = cv.cv_score;
        for (uint32_t c = 0; c < no; c++) {
            f.train[c] = f.st == OMEGA_RES_OK ? res_nrmse(ytr + c, ptr + c, ntr, no) : 0.0 / 0.0;
            f.test[c] = f.st == OMEGA_RES_OK ? res_nrmse(yte + c, pte + c, nte, no) : 0.0 / 0.0;
        }
    }
    free(ptr);
    free(pte);
    return f;
}

static double fit_mean(const Fit *f, uint32_t no)
{
    double s = 0.0;
    for (uint32_t c = 0; c < no; c++) s += f->test[c];
    return s / no;
}

static void fit_json(FILE *o, const char *model, const char *task, const Fit *f, uint32_t no)
{
    fprintf(o, "{\"model\": \"%s\", \"task\": \"%s\", \"features\": %u, \"status\": \"%s\", \"lambda\": ", model, task,
            f->nf, res_status_name(f->st));
    jd(o, f->lambda);
    fputs(", \"cv_score\": ", o);
    jd(o, f->cv);
    fputs(", \"test_nrmse\": [", o);
    for (uint32_t c = 0; c < no; c++) {
        fputs(c ? ", " : "", o);
        jd(o, f->test[c]);
    }
    fputs("], \"train_nrmse\": [", o);
    for (uint32_t c = 0; c < no; c++) {
        fputs(c ? ", " : "", o);
        jd(o, f->train[c]);
    }
    fputs("]}", o);
}

/* Product features: x (n x nf) -> [x, x_i x_j for i <= j]. */
static double *with_products(const double *x, uint32_t n, uint32_t nf, uint32_t *nf_out)
{
    uint32_t m = nf + nf * (nf + 1u) / 2u;
    double *z = malloc((size_t)n * m * sizeof(double));
    if (!z) return NULL;
    for (uint32_t r = 0; r < n; r++) {
        const double *a = x + (size_t)r * nf;
        double *b = z + (size_t)r * m;
        uint32_t k = 0;
        for (uint32_t i = 0; i < nf; i++) b[k++] = a[i];
        for (uint32_t i = 0; i < nf; i++)
            for (uint32_t j = i; j < nf; j++) b[k++] = a[i] * a[j];
    }
    *nf_out = m;
    return z;
}

/* Squares only: x -> [x, x.^2]. */
static double *with_squares(const double *x, uint32_t n, uint32_t nf)
{
    double *z = malloc((size_t)n * 2u * nf * sizeof(double));
    if (!z) return NULL;
    for (uint32_t r = 0; r < n; r++)
        for (uint32_t i = 0; i < nf; i++) {
            double v = x[(size_t)r * nf + i];
            z[(size_t)r * 2u * nf + i] = v;
            z[(size_t)r * 2u * nf + nf + i] = v * v;
        }
    return z;
}

/* ---------------------------------------------------------------- NARMA */

#define NM 3u          /* sub-pulses per input */
#define NPW 100u       /* steps per 1 ms sub-pulse */
#define N_TRAIN 2000u
#define N_TEST 1000u
#define WASHOUT 100u
#define NDELAY 12u
#define I_HI 1e-10
#define NMODEL_N 6
#define NNET_N 2
static const uint32_t ORDERS[3] = {2, 5, 10};
/* models 0, 1: the declared 50-node chain; 4, 5: the post-hoc 5-node chain (section 10.1) */
static const char *NARMA_MODELS[NMODEL_N] = {"RC-connected",          "RC-disconnected",
                                             "BL-linear",             "BL-quadratic",
                                             "RC-connected-5 (post hoc)", "RC-disconnected-5 (post hoc)"};

typedef struct {
    int seed;
    uint32_t mask_start[64], nd[NNET_N], nodes[NNET_N][2];
    Run run[NNET_N][2][2]; /* [net][connected, disconnected][train, test] */
    Fit f[NMODEL_N][3];
} NarmaResult;

/* Knot table for one split: sub-pulse j covers knots [10 j, 10 j + 10);
 * one extra final knot repeats the last value. */
static float *narma_table(const double *u, uint32_t n, const uint32_t *drv, uint32_t nd, uint32_t L,
                          const uint8_t (*mask)[NM], uint32_t *knots)
{
    const uint32_t kn = n * NM * 10u + 1u;
    float *tab = calloc((size_t)kn * L, sizeof(float));
    if (!tab) return NULL;
    for (uint32_t k = 0; k < kn; k++) {
        uint32_t j = k / 10u;
        if (j >= n * NM) j = n * NM - 1u;
        uint32_t c = j / NM, sub = j % NM;
        double on = 2.0 * u[c] * I_HI;
        for (uint32_t d = 0; d < nd; d++) tab[(size_t)k * L + drv[d]] = (float)(mask[d][sub] ? on : I_HI - on);
    }
    *knots = kn;
    return tab;
}

static void fit_refused(Fit *F, OmegaResStatus st, uint32_t nf)
{
    memset(F, 0, sizeof *F);
    F->st = st;
    F->nf = nf;
    for (int c = 0; c < 3; c++) F->test[c] = F->train[c] = 0.0 / 0.0;
    F->lambda = F->cv = 0.0 / 0.0;
}

static void *narma_seed(void *arg)
{
    NarmaResult *R = arg;
    const int seed = R->seed;
    double u[2][N_TRAIN];
    const uint32_t n[2] = {N_TRAIN, N_TEST}, nr[2] = {N_TRAIN - WASHOUT, N_TEST - WASHOUT};
    ResRng ru[2] = {stream(seed, P_NARMA_TRAIN), stream(seed, P_NARMA_TEST)}, rm = stream(seed, P_NARMA_MASK);
    for (int sp = 0; sp < 2; sp++)
        for (uint32_t c = 0; c < n[sp]; c++) u[sp][c] = 0.5 * res_rng_unit(&ru[sp]);
    static const uint8_t DB[4] = {0, 0, 1, 1}; /* de Bruijn B(2, 2) */
    uint8_t mask[64][NM];
    for (uint32_t d = 0; d < 64; d++) { /* 49 used by the 50-node chain, the first 4 by the 5-node one */
        R->mask_start[d] = (uint32_t)(res_rng_next(&rm) % 4u);
        for (uint32_t k = 0; k < NM; k++) mask[d][k] = DB[(R->mask_start[d] + k) % 4u];
    }
    double *y[3][2];
    for (int sp = 0; sp < 2; sp++)
        for (int o = 0; o < 3; o++) {
            double *full = malloc((size_t)n[sp] * sizeof(double));
            if (!full) return NULL;
            res_narma(u[sp], n[sp], ORDERS[o], full);
            y[o][sp] = full + WASHOUT;
        }
    for (int net = 0; net < NNET_N; net++) {
        OmegaResTopology tc, td;
        OmegaResParams pc, pd;
        uint32_t inp[3], g = 4, drv_c[64], nd = 0, drv_d[64];
        float conc[64];
        OmegaResStatus st = net == 0 ? res_net_topology(RES_NET_CHAIN50, &tc, inp, &g) : omega_res_topology_chain(5, 4, &tc);
        if (st != OMEGA_RES_OK || omega_res_params_reference(OMEGA_RES_REF_NARMA, tc.node_count, &pc) != OMEGA_RES_OK)
            return NULL;
        for (uint32_t i = 0; i < tc.node_count; i++)
            if (i != g) {
                drv_c[nd] = i;
                conc[nd] = pc.conc_um[i];
                drv_d[nd] = nd;
                nd++;
            }
        OmegaResStatus dst = res_net_disconnected(OMEGA_RES_REF_NARMA, conc, nd, &td, &pd);
        R->nd[net] = nd;
        R->nodes[net][0] = tc.node_count;
        R->nodes[net][1] = dst == OMEGA_RES_OK ? td.node_count : 0;
        const uint32_t nfr = nd * NM + 1u;
        double *xr[2][2] = {{0}};
        for (int sp = 0; sp < 2; sp++) {
            uint64_t *steps = malloc((size_t)n[sp] * NM * sizeof(uint64_t));
            if (!steps) return NULL;
            for (uint32_t j = 0; j < n[sp] * NM; j++) steps[j] = (uint64_t)(j + 1u) * NPW; /* end of sub-pulse j */
            for (int m = 0; m < 2; m++) {
                const OmegaResTopology *t = m ? &td : &tc;
                const uint32_t *drv = m ? drv_d : drv_c;
                if (m && dst != OMEGA_RES_OK) {
                    R->run[net][m][sp].st = dst;
                    continue;
                }
                uint32_t kn;
                float *tab = narma_table(u[sp], n[sp], drv, nd, t->node_count, (const uint8_t(*)[NM])mask, &kn);
                double *v = malloc((size_t)n[sp] * NM * t->node_count * sizeof(double));
                xr[m][sp] = malloc((size_t)nr[sp] * nfr * sizeof(double));
                if (!tab || !v || !xr[m][sp]) return NULL;
                R->run[net][m][sp] =
                    simulate(t, m ? &pd : &pc, OMEGA_RES_INIT_N0, 0.0f, tab, kn, steps, (size_t)n[sp] * NM, v);
                for (uint32_t r = 0; r < nr[sp]; r++) {
                    uint32_t c = WASHOUT + r; /* features: sub-pulses of input c-1, then u(c-1) */
                    double *row = xr[m][sp] + (size_t)r * nfr;
                    for (uint32_t d = 0; d < nd; d++)
                        for (uint32_t k = 0; k < NM; k++)
                            row[d * NM + k] = v[((size_t)(c - 1u) * NM + k) * t->node_count + drv[d]];
                    row[nd * NM] = u[sp][c - 1u];
                }
                free(tab);
                free(v);
            }
            free(steps);
        }
        for (int o = 0; o < 3; o++)
            for (int m = 0; m < 2; m++) {
                Fit *F = &R->f[net ? 4 + m : m][o];
                const Run *a = &R->run[net][m][0], *b = &R->run[net][m][1];
                if (a->st != OMEGA_RES_OK || b->st != OMEGA_RES_OK)
                    fit_refused(F, a->st != OMEGA_RES_OK ? a->st : b->st, nfr);
                else
                    *F = fit(xr[m][0], y[o][0], nr[0], xr[m][1], y[o][1], nr[1], nfr, 1);
            }
        for (int sp = 0; sp < 2; sp++) {
            free(xr[0][sp]);
            free(xr[1][sp]);
        }
    }
    /* baselines */
    double *xb[2], *xq[2];
    uint32_t nfq = 0;
    for (int sp = 0; sp < 2; sp++) {
        xb[sp] = malloc((size_t)nr[sp] * NDELAY * sizeof(double));
        if (!xb[sp]) return NULL;
        for (uint32_t r = 0; r < nr[sp]; r++)
            for (uint32_t i = 0; i < NDELAY; i++) xb[sp][(size_t)r * NDELAY + i] = u[sp][WASHOUT + r - 1u - i];
        xq[sp] = with_products(xb[sp], nr[sp], NDELAY, &nfq);
        if (!xq[sp]) return NULL;
    }
    for (int o = 0; o < 3; o++) {
        R->f[2][o] = fit(xb[0], y[o][0], nr[0], xb[1], y[o][1], nr[1], NDELAY, 1);
        R->f[3][o] = fit(xq[0], y[o][0], nr[0], xq[1], y[o][1], nr[1], nfq, 1);
    }
    for (int sp = 0; sp < 2; sp++) {
        free(xb[sp]);
        free(xq[sp]);
        for (int o = 0; o < 3; o++) free(y[o][sp] - WASHOUT);
    }
    return R;
}

/* --------------------------------------------------------------- Lorenz */

#define LZ_DT 1e-4
#define LZ_DISCARD 200000u
#define LZ_POINTS 200001u /* 20 s recorded, the knots */
#define LZ_SAMPLE 100u    /* trajectory points per 10 ms sample */
#define LZ_NS 2001u       /* samples 0 .. 20 s */
#define LZ_ROW0 200u
#define LZ_ROW1 1975u
#define LZ_NROW (LZ_ROW1 - LZ_ROW0 + 1u)
#define LZ_LAGS 5u
#define NMODEL_L 6
static const uint32_t HORIZONS[3] = {0, 1, 25};
static const char *LZ_TASKS[3] = {"T0", "T1", "T25"};
static const char *LZ_MODELS[NMODEL_L] = {"RC-connected-lin", "RC-connected-sq", "RC-disconnected-lin",
                                          "RC-disconnected-sq", "BL-linear", "BL-quadratic"};

typedef struct {
    int seed;
    double ic[2][3];
    uint32_t nodes[2];
    Run run[2][2];
    Fit f[NMODEL_L][3];
} LorenzResult;

static void lz_deriv(const double *s, double *d)
{
    d[0] = 10.0 * (s[1] - s[0]);
    d[1] = s[0] * (28.0 - s[2]) - s[1];
    d[2] = s[0] * s[1] - (8.0 / 3.0) * s[2];
}

static void lz_rk4(double *s)
{
    double k1[3], k2[3], k3[3], k4[3], t[3];
    lz_deriv(s, k1);
    for (int i = 0; i < 3; i++) t[i] = s[i] + 0.5 * LZ_DT * k1[i];
    lz_deriv(t, k2);
    for (int i = 0; i < 3; i++) t[i] = s[i] + 0.5 * LZ_DT * k2[i];
    lz_deriv(t, k3);
    for (int i = 0; i < 3; i++) t[i] = s[i] + LZ_DT * k3[i];
    lz_deriv(t, k4);
    for (int i = 0; i < 3; i++) s[i] += LZ_DT / 6.0 * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]);
}

/* Reference injection currents (Lorenz63SolverCustomRegression.m:46-49). */
static double lz_current(double v, int c)
{
    return c < 2 ? (v * 1.05e-10 + 1e-9) / 10.0 : (v * 1e-10 * 2.05) / 10.0;
}

static void *lorenz_seed(void *arg)
{
    LorenzResult *R = arg;
    double *X[2];
    for (int sp = 0; sp < 2; sp++) {
        ResRng r = stream(R->seed, sp ? P_LORENZ_TEST : P_LORENZ_TRAIN);
        double s[3] = {-10.0 + 20.0 * res_rng_unit(&r), -10.0 + 20.0 * res_rng_unit(&r), 10.0 + 30.0 * res_rng_unit(&r)};
        memcpy(R->ic[sp], s, sizeof s);
        for (uint32_t k = 0; k < LZ_DISCARD; k++) lz_rk4(s);
        X[sp] = malloc((size_t)LZ_POINTS * 3u * sizeof(double));
        if (!X[sp]) return NULL;
        for (uint32_t k = 0; k < LZ_POINTS; k++) {
            memcpy(X[sp] + (size_t)k * 3u, s, sizeof s);
            lz_rk4(s);
        }
    }
    OmegaResTopology tc, td;
    OmegaResParams pc, pd;
    uint32_t inp[3], g, drv_d[3] = {0, 1, 2};
    if (res_net_topology(RES_NET_HEX43, &tc, inp, &g) != OMEGA_RES_OK ||
        omega_res_params_reference(OMEGA_RES_REF_LORENZ, tc.node_count, &pc) != OMEGA_RES_OK)
        return NULL;
    float conc[3] = {pc.conc_um[inp[0]], pc.conc_um[inp[1]], pc.conc_um[inp[2]]};
    OmegaResStatus dst = res_net_disconnected(OMEGA_RES_REF_LORENZ, conc, 3, &td, &pd);
    R->nodes[0] = tc.node_count;
    R->nodes[1] = dst == OMEGA_RES_OK ? td.node_count : 0;
    uint64_t steps[LZ_NS];
    for (uint32_t j = 0; j < LZ_NS; j++) steps[j] = (uint64_t)j * LZ_SAMPLE * KSTEP;
    /* reservoir features at rows j = LZ_ROW0 .. LZ_ROW1 */
    const uint32_t nfl[2] = {tc.node_count - 1u, 3u};
    double *xl[2][2] = {{0}}, *xs[2][2] = {{0}}, *xb[2], *xq[2], *y[3][2];
    uint32_t nfq = 0;
    for (int sp = 0; sp < 2; sp++) {
        for (int m = 0; m < 2; m++) {
            const OmegaResTopology *t = m ? &td : &tc;
            if (m && dst != OMEGA_RES_OK) {
                R->run[m][sp].st = dst;
                continue;
            }
            const uint32_t *drv = m ? drv_d : inp, L = t->node_count;
            float *tab = calloc((size_t)LZ_POINTS * L, sizeof(float));
            double *v = malloc((size_t)LZ_NS * L * sizeof(double));
            xl[m][sp] = malloc((size_t)LZ_NROW * nfl[m] * sizeof(double));
            if (!tab || !v || !xl[m][sp]) return NULL;
            for (uint32_t k = 0; k < LZ_POINTS; k++)
                for (int c = 0; c < 3; c++) tab[(size_t)k * L + drv[c]] = (float)lz_current(X[sp][(size_t)k * 3u + c], c);
            R->run[m][sp] = simulate(t, m ? &pd : &pc, OMEGA_RES_INIT_CONST, 1.0f, tab, LZ_POINTS, steps, LZ_NS, v);
            for (uint32_t r = 0; r < LZ_NROW; r++) {
                uint32_t j = LZ_ROW0 + r, f = 0;
                for (uint32_t i = 0; i < L; i++) {
                    if (m == 0 && i == g) continue; /* the ground is 0 V */
                    if (m == 1 && i >= 3u) break;   /* disconnected: the three driven droplets */
                    xl[m][sp][(size_t)r * nfl[m] + f++] = v[(size_t)j * L + i];
                }
            }
            xs[m][sp] = with_squares(xl[m][sp], LZ_NROW, nfl[m]);
            if (!xs[m][sp]) return NULL;
            free(tab);
            free(v);
        }
        xb[sp] = malloc((size_t)LZ_NROW * LZ_LAGS * 3u * sizeof(double));
        if (!xb[sp]) return NULL;
        for (uint32_t r = 0; r < LZ_NROW; r++)
            for (uint32_t l = 0; l < LZ_LAGS; l++)
                for (int c = 0; c < 3; c++)
                    xb[sp][(size_t)r * LZ_LAGS * 3u + l * 3u + c] =
                        X[sp][((size_t)(LZ_ROW0 + r - l) * LZ_SAMPLE) * 3u + c];
        xq[sp] = with_products(xb[sp], LZ_NROW, LZ_LAGS * 3u, &nfq);
        for (int h = 0; h < 3; h++) {
            y[h][sp] = malloc((size_t)LZ_NROW * 3u * sizeof(double));
            if (!y[h][sp] || !xq[sp]) return NULL;
            for (uint32_t r = 0; r < LZ_NROW; r++)
                for (int c = 0; c < 3; c++)
                    y[h][sp][(size_t)r * 3u + c] = X[sp][((size_t)(LZ_ROW0 + r + HORIZONS[h]) * LZ_SAMPLE) * 3u + c];
        }
        free(X[sp]);
    }
    for (int h = 0; h < 3; h++) {
        for (int m = 0; m < 2; m++) {
            int ok = R->run[m][0].st == OMEGA_RES_OK && R->run[m][1].st == OMEGA_RES_OK;
            for (int q = 0; q < 2; q++) {
                Fit *F = &R->f[2 * m + q][h];
                if (!ok) {
                    memset(F, 0, sizeof *F);
                    F->st = R->run[m][0].st != OMEGA_RES_OK ? R->run[m][0].st : R->run[m][1].st;
                    F->nf = nfl[m] * (q + 1u);
                    for (int c = 0; c < 3; c++) F->test[c] = F->train[c] = 0.0 / 0.0;
                    F->lambda = F->cv = 0.0 / 0.0;
                    continue;
                }
                *F = q ? fit(xs[m][0], y[h][0], LZ_NROW, xs[m][1], y[h][1], LZ_NROW, 2u * nfl[m], 3)
                       : fit(xl[m][0], y[h][0], LZ_NROW, xl[m][1], y[h][1], LZ_NROW, nfl[m], 3);
            }
        }
        R->f[4][h] = fit(xb[0], y[h][0], LZ_NROW, xb[1], y[h][1], LZ_NROW, LZ_LAGS * 3u, 3);
        R->f[5][h] = fit(xq[0], y[h][0], LZ_NROW, xq[1], y[h][1], LZ_NROW, nfq, 3);
    }
    for (int sp = 0; sp < 2; sp++) {
        for (int m = 0; m < 2; m++) {
            free(xl[m][sp]);
            free(xs[m][sp]);
        }
        free(xb[sp]);
        free(xq[sp]);
        for (int h = 0; h < 3; h++) free(y[h][sp]);
    }
    return R;
}

/* ------------------------------------------------------------ summaries */

typedef struct {
    double mean, sd, min, max;
    int n;
} Stat;

static Stat stat(const double *v, int n)
{
    Stat s = {0.0 / 0.0, 0.0 / 0.0, 0.0 / 0.0, 0.0 / 0.0, 0};
    double sum = 0.0, mn = 1.0 / 0.0, mx = -1.0 / 0.0;
    for (int i = 0; i < n; i++) {
        if (!(v[i] == v[i])) continue;
        sum += v[i];
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
        s.n++;
    }
    if (!s.n) return s;
    s.mean = sum / s.n;
    s.min = mn;
    s.max = mx;
    if (s.n > 1) {
        double q = 0.0;
        for (int i = 0; i < n; i++)
            if (v[i] == v[i]) q += (v[i] - s.mean) * (v[i] - s.mean);
        s.sd = res_sqrt(q / (s.n - 1));
    }
    return s;
}

static void stat_json(FILE *o, const Stat *s)
{
    fprintf(o, "{\"seeds_ok\": %d, \"mean\": ", s->n);
    jd(o, s->mean);
    fputs(", \"sd\": ", o);
    jd(o, s->sd);
    fputs(", \"min\": ", o);
    jd(o, s->min);
    fputs(", \"max\": ", o);
    jd(o, s->max);
    fputc('}', o);
}

/* Paired comparison a - b over seeds; wins = seeds where a < b. */
static void paired_json(FILE *o, const char *name, const double *a, const double *b, int n)
{
    double d[NSEED_MAX];
    int wins = 0, both = 0;
    for (int i = 0; i < n; i++) {
        d[i] = a[i] - b[i];
        if (d[i] == d[i]) {
            both++;
            wins += a[i] < b[i];
        }
    }
    Stat s = stat(d, n);
    fprintf(o, "{\"comparison\": \"%s\", \"difference\": ", name);
    stat_json(o, &s);
    fprintf(o, ", \"first_wins\": %d, \"seeds_compared\": %d}", wins, both);
}

static int parse_seeds(int argc, char **argv, int *seeds)
{
    int n = 0;
    for (int i = 0; i < argc && n < NSEED_MAX; i++) seeds[n++] = atoi(argv[i]);
    if (!n)
        for (n = 0; n < 5; n++) seeds[n] = n + 1;
    return n;
}

static void seeds_json(FILE *o, const int *seeds, int n)
{
    fputs("[", o);
    for (int i = 0; i < n; i++) fprintf(o, "%s%d", i ? ", " : "", seeds[i]);
    fputs("]", o);
}

int res_bench_narma(int argc, char **argv, FILE *o)
{
    int seeds[NSEED_MAX], ns = parse_seeds(argc, argv, seeds), bad = 0;
    NarmaResult *R = calloc((size_t)ns, sizeof *R);
    pthread_t th[NSEED_MAX];
    if (!R) return 2;
    double t0 = res_now_s();
    for (int i = 0; i < ns; i++) {
        R[i].seed = seeds[i];
        if (pthread_create(&th[i], NULL, narma_seed, &R[i])) return 2;
    }
    for (int i = 0; i < ns; i++) {
        void *ret;
        pthread_join(th[i], &ret);
        if (!ret) bad = 1;
    }
    if (bad) {
        fprintf(stderr, "res_qualify: bench-narma setup failed\n");
        return 2;
    }
    fprintf(o, "{\n  \"schema\": \"omega-reservoir-bench-narma/1\",\n  \"protocol\": \"RESERVOIR_REFERENCE.md section 10\","
               "\n  \"seeds\": ");
    seeds_json(o, seeds, ns);
    fprintf(o, ",\n  \"setup\": {\"train_inputs\": %u, \"test_inputs\": %u, \"washout\": %u, \"subpulses\": %u, "
               "\"subpulse_s\": 1e-3, \"current_hi_a\": 1e-10, \"delay_line\": %u, \"lambda_grid\": [1e-6, 1e2, 9], "
               "\"folds\": 5,\n    \"networks\": [{\"name\": \"Length50 chain (declared)\", \"driven_nodes\": %u, \"nodes_connected\": %u, "
               "\"nodes_disconnected\": %u}, {\"name\": \"Length5 chain (post hoc, section 10.1)\", \"driven_nodes\": %u, "
               "\"nodes_connected\": %u, \"nodes_disconnected\": %u}],\n    "
               "\"rc_squared_states\": \"NOT_RUN (50-node chain: 295 features > readout bound 256)\"},\n",
            N_TRAIN, N_TEST, WASHOUT, NM, NDELAY, R[0].nd[0], R[0].nodes[0][0], R[0].nodes[0][1], R[0].nd[1],
            R[0].nodes[1][0], R[0].nodes[1][1]);
    fprintf(o, "  \"wall_s\": %.1f,\n  \"per_seed\": [", res_now_s() - t0);
    for (int i = 0; i < ns; i++) {
        fprintf(o, "%s\n    {\"seed\": %d, \"runs\": [", i ? "," : "", R[i].seed);
        static const char *rn[2][2][2] = {{{"chain50 connected/train", "chain50 connected/test"},
                                           {"chain50 disconnected/train", "chain50 disconnected/test"}},
                                          {{"chain5 connected/train", "chain5 connected/test"},
                                           {"chain5 disconnected/train", "chain5 disconnected/test"}}};
        for (int net = 0; net < NNET_N; net++)
            for (int m = 0; m < 2; m++)
                for (int sp = 0; sp < 2; sp++) {
                    fputs(net || m || sp ? ", " : "", o);
                    run_json(o, rn[net][m][sp], &R[i].run[net][m][sp]);
                }
        fputs("],\n     \"mask_starts\": [", o);
        for (uint32_t d = 0; d < R[i].nd[0]; d++) fprintf(o, "%s%u", d ? ", " : "", R[i].mask_start[d]);
        fputs("],\n     \"fits\": [", o);
        for (int m = 0; m < NMODEL_N; m++)
            for (int t = 0; t < 3; t++) {
                char task[16];
                snprintf(task, sizeof task, "NARMA-%u", ORDERS[t]);
                fputs(m || t ? ",\n       " : "", o);
                fit_json(o, NARMA_MODELS[m], task, &R[i].f[m][t], 1);
            }
        fputs("]}", o);
    }
    fputs("\n  ],\n  \"summary\": [", o);
    for (int t = 0; t < 3; t++) {
        double v[NMODEL_N][NSEED_MAX], best[NSEED_MAX];
        for (int i = 0; i < ns; i++) {
            for (int m = 0; m < NMODEL_N; m++) v[m][i] = R[i].f[m][t].test[0];
            /* best baseline per seed: the lower cross-validation score (training rows only) */
            const Fit *a = &R[i].f[2][t], *b = &R[i].f[3][t];
            best[i] = (b->st == OMEGA_RES_OK && (a->st != OMEGA_RES_OK || b->cv < a->cv)) ? b->test[0] : a->test[0];
        }
        fprintf(o, "%s\n    {\"task\": \"NARMA-%u\", \"test_nrmse\": {", t ? "," : "", ORDERS[t]);
        for (int m = 0; m < NMODEL_N; m++) {
            Stat s = stat(v[m], ns);
            fprintf(o, "%s\"%s\": ", m ? ", " : "", NARMA_MODELS[m]);
            stat_json(o, &s);
        }
        fputs("},\n     \"paired\": [", o);
        paired_json(o, "RC-connected - RC-disconnected", v[0], v[1], ns);
        fputs(", ", o);
        paired_json(o, "RC-connected - best baseline (by CV score)", v[0], best, ns);
        fputs(", ", o);
        paired_json(o, "RC-connected-5 - RC-disconnected-5 (post hoc)", v[4], v[5], ns);
        fputs(", ", o);
        paired_json(o, "RC-connected-5 - best baseline (by CV score) (post hoc)", v[4], best, ns);
        fputs("]}", o);
    }
    fputs("\n  ],\n  \"significance\": \"none claimed (five seeds)\"\n}\n", o);
    free(R);
    return 0;
}

int res_bench_lorenz(int argc, char **argv, FILE *o)
{
    int seeds[NSEED_MAX], ns = parse_seeds(argc, argv, seeds), bad = 0;
    LorenzResult *R = calloc((size_t)ns, sizeof *R);
    pthread_t th[NSEED_MAX];
    if (!R) return 2;
    double t0 = res_now_s();
    for (int i = 0; i < ns; i++) {
        R[i].seed = seeds[i];
        if (pthread_create(&th[i], NULL, lorenz_seed, &R[i])) return 2;
    }
    for (int i = 0; i < ns; i++) {
        void *ret;
        pthread_join(th[i], &ret);
        if (!ret) bad = 1;
    }
    if (bad) {
        fprintf(stderr, "res_qualify: bench-lorenz setup failed\n");
        return 2;
    }
    fprintf(o, "{\n  \"schema\": \"omega-reservoir-bench-lorenz/1\",\n  \"protocol\": \"RESERVOIR_REFERENCE.md section 10\","
               "\n  \"seeds\": ");
    seeds_json(o, seeds, ns);
    fprintf(o, ",\n  \"setup\": {\"dt\": 1e-4, \"discard_s\": 20, \"record_s\": 20, \"sample_s\": 0.01, "
               "\"rows\": [%u, %u], \"horizons_samples\": [0, 1, 25], \"lags\": %u, \"lambda_grid\": [1e-6, 1e2, 9], "
               "\"folds\": 5, \"nodes_connected\": %u, \"nodes_disconnected\": %u},\n",
            LZ_ROW0, LZ_ROW1, LZ_LAGS, R[0].nodes[0], R[0].nodes[1]);
    fprintf(o, "  \"wall_s\": %.1f,\n  \"per_seed\": [", res_now_s() - t0);
    for (int i = 0; i < ns; i++) {
        fprintf(o, "%s\n    {\"seed\": %d, \"initial_state\": {\"train\": [", i ? "," : "", R[i].seed);
        for (int sp = 0; sp < 2; sp++) {
            if (sp) fputs("], \"test\": [", o);
            for (int c = 0; c < 3; c++) {
                fputs(c ? ", " : "", o);
                jd(o, R[i].ic[sp][c]);
            }
        }
        fputs("]}, \"runs\": [", o);
        static const char *rn[2][2] = {{"connected/train", "connected/test"}, {"disconnected/train", "disconnected/test"}};
        for (int m = 0; m < 2; m++)
            for (int sp = 0; sp < 2; sp++) {
                fputs(m || sp ? ", " : "", o);
                run_json(o, rn[m][sp], &R[i].run[m][sp]);
            }
        fputs("],\n     \"fits\": [", o);
        for (int m = 0; m < NMODEL_L; m++)
            for (int t = 0; t < 3; t++) {
                fputs(m || t ? ",\n       " : "", o);
                fit_json(o, LZ_MODELS[m], LZ_TASKS[t], &R[i].f[m][t], 3);
            }
        fputs("]}", o);
    }
    fputs("\n  ],\n  \"summary\": [", o);
    for (int t = 0; t < 3; t++) {
        double v[NMODEL_L][NSEED_MAX], best[NSEED_MAX];
        for (int i = 0; i < ns; i++) {
            for (int m = 0; m < NMODEL_L; m++) v[m][i] = fit_mean(&R[i].f[m][t], 3);
            const Fit *a = &R[i].f[4][t], *b = &R[i].f[5][t];
            best[i] = (b->st == OMEGA_RES_OK && (a->st != OMEGA_RES_OK || b->cv < a->cv)) ? fit_mean(b, 3)
                                                                                          : fit_mean(a, 3);
        }
        fprintf(o, "%s\n    {\"task\": \"%s\", \"test_nrmse_mean_over_xyz\": {", t ? "," : "", LZ_TASKS[t]);
        for (int m = 0; m < NMODEL_L; m++) {
            Stat s = stat(v[m], ns);
            fprintf(o, "%s\"%s\": ", m ? ", " : "", LZ_MODELS[m]);
            stat_json(o, &s);
        }
        fputs("},\n     \"paired\": [", o);
        paired_json(o, "RC-connected-lin - RC-disconnected-lin", v[0], v[2], ns);
        fputs(", ", o);
        paired_json(o, "RC-connected-sq - RC-disconnected-sq", v[1], v[3], ns);
        fputs(", ", o);
        paired_json(o, "RC-connected-lin - best baseline (by CV score)", v[0], best, ns);
        fputs(", ", o);
        paired_json(o, "RC-connected-sq - best baseline (by CV score)", v[1], best, ns);
        fputs("]}", o);
    }
    fputs("\n  ],\n  \"significance\": \"none claimed (five seeds)\"\n}\n", o);
    free(R);
    return 0;
}

/* ---------------------------------------------------------- performance */

#define PERF_WARM 10000u
#define PERF_STEPS 40000u
#define PERF_REPS 5u

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int64_t pct(const int64_t *s, size_t n, double p) /* nearest rank */
{
    size_t k = (size_t)(p * (double)n + 0.999999999);
    if (k < 1) k = 1;
    if (k > n) k = n;
    return s[k - 1];
}

/* /proc/cpuinfo "CPU part" of one logical CPU, or "unknown". */
static void cpu_part(int cpu, char *out, size_t n)
{
    snprintf(out, n, "unknown");
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        int p;
        if (sscanf(line, "processor : %d", &p) == 1) cur = p;
        char *c = strstr(line, "CPU part");
        if (c && cur == cpu) {
            char *v = strchr(c, ':');
            if (v) {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                snprintf(out, n, "%.*s", (int)strcspn(v, "\n"), v);
            }
            break;
        }
    }
    fclose(f);
}

typedef struct {
    const char *name;
    ResNet net; /* 0: the 5-node chain */
    OmegaResReference ref;
} PerfNet;

int res_perf(FILE *o)
{
    /* pin to one core: RES_PERF_CPU, else the first Cortex-X925 (part 0xd85), else CPU 0 */
    int cpu = -1;
    const char *env = getenv("RES_PERF_CPU");
    char part[64];
    if (env) cpu = atoi(env);
    for (int c = 0; cpu < 0 && c < CPU_SETSIZE && c < 1024; c++) {
        cpu_part(c, part, sizeof part);
        if (!strcmp(part, "0xd85")) cpu = c;
        if (!strcmp(part, "unknown") && c > 0) break;
    }
    if (cpu < 0) cpu = 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int pinned = sched_setaffinity(0, sizeof set, &set) == 0;
    cpu_part(cpu, part, sizeof part);
    struct utsname un;
    uname(&un);
    /* timer cost */
    int64_t tc0 = now_ns();
    for (int i = 0; i < 100000; i++) (void)now_ns();
    double timer_ns = (double)(now_ns() - tc0) / 100000.0;

    static const PerfNet nets[3] = {{"chain5 NARMA (G2-N1)", (ResNet)0, OMEGA_RES_REF_NARMA},
                                    {"chain50 NARMA", RES_NET_CHAIN50, OMEGA_RES_REF_NARMA},
                                    {"hex43 Lorenz", RES_NET_HEX43, OMEGA_RES_REF_LORENZ}};
    fprintf(o, "{\n  \"schema\": \"omega-reservoir-perf/1\",\n  \"protocol\": \"RESERVOIR_REFERENCE.md section 10\",\n");
    fprintf(o, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", \"cpu\": %d, "
               "\"cpu_part\": \"%s\", \"pinned\": %s, \"sched_getcpu\": %d},\n",
            un.sysname, un.release, un.machine, cpu, part, pinned ? "true" : "false", sched_getcpu());
    fputs("  \"compiler\": ", o);
    res_json_str(o, __VERSION__);
#ifdef RES_BUILD_FLAGS
    fputs(",\n  \"flags\": ", o);
    res_json_str(o, RES_BUILD_FLAGS);
#endif
    fprintf(o, ",\n  \"timer\": {\"clock\": \"CLOCK_MONOTONIC\", \"mean_call_ns\": %.1f, \"included_in_latency\": true},\n",
            timer_ns);
    fprintf(o, "  \"struct_bytes\": {\"OmegaResConfig\": %zu, \"OmegaResState\": %zu, \"OmegaResWorkspace\": %zu, "
               "\"OmegaResTopology\": %zu, \"OmegaResReadout\": %zu, \"OmegaResReadoutWorkspace\": %zu},\n",
            sizeof(OmegaResConfig), sizeof(OmegaResState), sizeof(OmegaResWorkspace), sizeof(OmegaResTopology),
            sizeof(OmegaResReadout), sizeof(OmegaResReadoutWorkspace));
    fprintf(o, "  \"warmup_steps\": %u, \"timed_steps\": %u, \"repetitions\": %u,\n  \"networks\": [", PERF_WARM,
            PERF_STEPS, PERF_REPS);
    const uint32_t total = PERF_WARM + PERF_STEPS * PERF_REPS, knots = total / KSTEP + 1u;
    int64_t *lat = malloc((size_t)PERF_STEPS * PERF_REPS * sizeof(int64_t));
    int rc = 0;
    if (!lat) return 2;
    struct rusage ru0, ru1;
    for (int ni = 0; ni < 3; ni++) {
        OmegaResTopology t;
        OmegaResParams p;
        uint32_t inp[3], g = 0;
        OmegaResStatus st = nets[ni].net ? res_net_topology(nets[ni].net, &t, inp, &g)
                                         : omega_res_topology_chain(5, 4, &t);
        if (!nets[ni].net) g = 4;
        if (st == OMEGA_RES_OK) st = omega_res_params_reference(nets[ni].ref, t.node_count, &p);
        ResSim *sim = calloc(1, sizeof *sim);
        float *tab = calloc((size_t)knots * t.node_count, sizeof(float));
        if (!sim || !tab) return 2;
        /* input (section 11.4): zero-mean 1 ms pulses on every non-ground node, uniform in
         * +-0.1 nA / driven nodes, which keeps the 50-node chain inside |V| <= 1 V for the whole run
         * (one-signed pulses of that size do not); or the reference Lorenz currents of a trajectory (hex43) */
        const double i_node = I_HI / (double)(t.node_count - 1u);
        ResRng r = stream(1, P_PERF);
        double s[3] = {1.0, 1.0, 20.0};
        for (uint32_t k = 0; k < knots; k++) {
            if (nets[ni].ref == OMEGA_RES_REF_LORENZ) {
                for (int c = 0; c < 3; c++) tab[(size_t)k * t.node_count + inp[c]] = (float)lz_current(s[c], c);
                lz_rk4(s);
            } else if (k % 10u == 0) {
                for (uint32_t i = 0; i < t.node_count; i++)
                    tab[(size_t)k * t.node_count + i] = i == g ? 0.0f : (float)(i_node * (2.0 * res_rng_unit(&r) - 1.0));
            } else
                memcpy(tab + (size_t)k * t.node_count, tab + (size_t)(k - 1u) * t.node_count, t.node_count * sizeof(float));
        }
        OmegaResIntegrator ig = {STEP_S, KSTEP};
        if (st == OMEGA_RES_OK) st = omega_res_config_build(&t, &p, &ig, &sim->cfg);
        if (st == OMEGA_RES_OK)
            st = omega_res_state_init(&sim->cfg, nets[ni].ref == OMEGA_RES_REF_LORENZ ? OMEGA_RES_INIT_CONST : OMEGA_RES_INIT_N0,
                                      nets[ni].ref == OMEGA_RES_REF_LORENZ ? 1.0f : 0.0f, &sim->s);
        OmegaResInput in = {t.node_count, knots, tab};
        OmegaResState *cur = &sim->s, *nxt = &sim->next, *tmp;
        for (uint32_t k = 0; st == OMEGA_RES_OK && k < PERF_WARM; k++) {
            st = omega_res_step(&sim->cfg, &in, &sim->ws, cur, nxt);
            tmp = cur, cur = nxt, nxt = tmp;
        }
        double rep_wall[PERF_REPS];
        getrusage(RUSAGE_SELF, &ru0);
        int64_t w0 = now_ns();
        for (uint32_t rep = 0; st == OMEGA_RES_OK && rep < PERF_REPS; rep++) {
            int64_t r0 = now_ns(), prev = r0;
            for (uint32_t k = 0; st == OMEGA_RES_OK && k < PERF_STEPS; k++) {
                st = omega_res_step(&sim->cfg, &in, &sim->ws, cur, nxt);
                int64_t nw = now_ns();
                lat[(size_t)rep * PERF_STEPS + k] = nw - prev;
                prev = nw;
                tmp = cur, cur = nxt, nxt = tmp;
            }
            rep_wall[rep] = (double)(prev - r0) * 1e-9;
        }
        double wall = (double)(now_ns() - w0) * 1e-9;
        getrusage(RUSAGE_SELF, &ru1);
        double cpu_s = (double)(ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + 1e-6 * (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) +
                       (double)(ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + 1e-6 * (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec);
        fprintf(o, "%s\n    {\"network\": \"%s\", \"nodes\": %u, \"membranes\": %u, \"status\": \"%s\"", ni ? "," : "",
                nets[ni].name, t.node_count, t.edge_count, res_status_name(st));
        if (st == OMEGA_RES_OK) {
            const size_t nl = (size_t)PERF_STEPS * PERF_REPS;
            fputs(", \"per_repetition\": [", o);
            for (uint32_t rep = 0; rep < PERF_REPS; rep++) {
                int64_t *a = lat + (size_t)rep * PERF_STEPS;
                qsort(a, PERF_STEPS, sizeof *a, cmp_i64);
                fprintf(o, "%s{\"p50_ns\": %lld, \"p95_ns\": %lld, \"p99_ns\": %lld, \"max_ns\": %lld, \"steps_per_s\": %.1f}",
                        rep ? ", " : "", (long long)pct(a, PERF_STEPS, 0.50), (long long)pct(a, PERF_STEPS, 0.95),
                        (long long)pct(a, PERF_STEPS, 0.99), (long long)a[PERF_STEPS - 1], PERF_STEPS / rep_wall[rep]);
            }
            qsort(lat, nl, sizeof *lat, cmp_i64);
            double sps = (double)nl / wall;
            fprintf(o, "], \"pooled\": {\"p50_ns\": %lld, \"p95_ns\": %lld, \"p99_ns\": %lld, \"max_ns\": %lld}, "
                       "\"steps_per_s\": %.1f, \"simulated_s_per_wall_s\": %.6g, \"cpu_over_wall\": %.4f}",
                    (long long)pct(lat, nl, 0.50), (long long)pct(lat, nl, 0.95), (long long)pct(lat, nl, 0.99),
                    (long long)lat[nl - 1], sps, sps * (double)STEP_S, cpu_s / wall);
        } else
            rc = 1;
        free(sim);
        free(tab);
    }
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    fprintf(o, "\n  ],\n  \"peak_rss_kib\": %ld,\n  \"energy\": \"NOT_RUN (not instrumented; no efficiency claim)\"\n}\n",
            ru.ru_maxrss);
    free(lat);
    return rc;
}
