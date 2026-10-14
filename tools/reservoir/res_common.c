/* Shared parts of the Gate 2 tool; see res_common.h. */
#include "res_common.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CZ "Codes for Zenodo/"
#define NC "NARMA Final Code/Connected Reservoir/"
#define NP "NARMA Final Code/Parallel Reservoir/"
#define LZ "Lorenz Final Code/"

const ResPin RES_PINS[] = {
    {"code.narma_connected", "src/" CZ NC "HyperParameterTuningOptimized.m",
     "620640f002068048ab2c56f5090dc945e7b81595a9bc7f863331659a1850ceb5"},
    {"code.narma_parallel", "src/" CZ NP "ParallelReservoirNARMA.m",
     "f120f9150db590e3fcf767cbd1840e0924f513b45d44cf8439be2229be0fb507"},
    {"code.lorenz43", "src/" CZ LZ "Lorenz63SolverCustomRegression.m",
     "d291460fffe4efc1b2f013060c04e3a561dcd571de813a9c95277557daafdcac"},
    {"code.lorenz13", "src/" CZ LZ "Lorenz63SolverCustomRegression13Nodes.m",
     "c9e2ad2b64832c50c95f9d61e86161496aef40640cc547d1d5ae3a9323eedbf1"},
    {"code.lorenz25", "src/" CZ LZ "Lorenz63SolverCustomRegression25Nodes.m",
     "c1b31403cc9a2014c2f3b65056873306ed52174142259b5127b30c803927dece"},
    {"code.lorenz_gen", "src/" CZ LZ "LorenzDataGenerator.m",
     "7295d55fa3e1209b74b4804f9de18f4952950020f32d0f318695286c5ccdfa1d"},
    {"narma.length5", "data/raw/" CZ NC "Length5.mat", "dc6e7914d216a21f664adcd4843e9f705616df47511879ba0bbeceb241b9b50a"},
    {"narma.length10", "data/raw/" CZ NC "Length10.mat", "ceca1f6e94f3239b2df072403a488f8929fe00673b5028319cd2094d6eae708e"},
    {"narma.length20", "data/raw/" CZ NC "Length20.mat", "aeb87fcec7655ce72386fdc28f2878b1f7b89097db523ab9fe5812fb6c4d1ee6"},
    {"narma.length50", "data/raw/" CZ NC "Length50.mat", "ff4c9d14abb6f64b1ab9f301072dc67316b7fd59fe61d0b7632ee467a8c2aae3"},
    {"narma.train_signal", "data/raw/" CZ NC "trainSignal.mat",
     "48e8e2c775fda4ca85dfc14a3c202319d9c75e2d7db0baf237744ce51a4b02c1"},
    {"narma.test_signal", "data/raw/" CZ NC "testSignal.mat",
     "f0e34c5733e2cc20852caf4874b92bcb19fcb95bd4da4ce0eb7f4919cabee8c1"},
    {"narma.results", "data/raw/" CZ NC "HyperParameterOptimizationVariablesToPlot.mat",
     "6311b719d47bfa92b76e1c71b3e9fe0e2e30d564d9ee7c7b2f231a03683e66fd"},
    {"narma.parallel_results", "data/raw/" CZ NP "ParallelReservoirDataToPlot.mat",
     "ccf13dd5045a50ce5e474915225f4961d6e56b949985ef701bffe3316822f1c6"},
    {"lorenz.data1", "data/raw/" CZ LZ "LorenzData1.mat", "6229d996681ef7e64336ef4f04e7bd82c0a8063e6e18e52667c699251091f6c4"},
    {"lorenz.data2", "data/raw/" CZ LZ "LorenzData2.mat", "d1c0dcccbee809e8880dd5b0e355dd456ab1605d0470dbfa99e1a54b2b51073a"},
    {"lorenz.fig43", "data/raw/" CZ LZ "Figure5_Data.mat", "f6055bc6b79cc686a5c7cca703fdf2e35de50e202e245bfe07f10f91ecad3500"},
    {"lorenz.fig25", "data/raw/" CZ LZ "Figure5_Data25Nodes.mat",
     "0d85b0eaef9c6bb4eed07a3fe8f395802afbde0c3a75403ba216b55220754205"},
    {"lorenz.fig13", "data/raw/" CZ LZ "Figure5_Data13Nodes.mat",
     "74988e19bd24540b0f623e20295a2813a0206917cdf6072d0dfaf69f8d091ed7"},
    {"lorenz.nrmse43", "data/raw/" CZ LZ "LorenzNormalizedNRMSEs.mat",
     "514ba3082a21eb541e0af8a9fbb80596742f9ddafee466391a00842a5d4e5616"},
    {"lorenz.nrmse25", "data/raw/" CZ LZ "LorenzNormalizedNRMSEs25Nodes.mat",
     "881e92a0a5cd6b46fb588b41593e83d66ec2af25d16fb7198444fc82c3aaf14d"},
    {"lorenz.nrmse13", "data/raw/" CZ LZ "LorenzNormalizedNRMSEs13Nodes.mat",
     "5f2d18b150d5baa9e792aa967d46a541a6031ff2805467ba30b72db745d55733"},
};
const unsigned RES_PIN_COUNT = sizeof RES_PINS / sizeof RES_PINS[0];

const ResPin *res_pin(const char *key)
{
    for (unsigned i = 0; i < RES_PIN_COUNT; i++)
        if (strcmp(RES_PINS[i].key, key) == 0) return &RES_PINS[i];
    return NULL;
}

static int unhex(const char *h, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[2 * i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int join(const char *root, const char *rel, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s/%s", root, rel);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int res_load_pinned(const char *root, const char *key, ResMatFile *f)
{
    const ResPin *p = res_pin(key);
    char path[1024];
    uint8_t want[32];
    if (!p || unhex(p->sha256, want) || join(root, p->path, path, sizeof path)) {
        fprintf(stderr, "res_qualify: no pin for %s\n", key);
        return -1;
    }
    ResMatStatus s = res_mat_load(path, want, f);
    if (s != RES_MAT_OK) {
        fprintf(stderr, "res_qualify: %s (%s): %s\n", key, path, res_mat_strerror(s));
        return -1;
    }
    return 0;
}

int res_check_pinned(const char *root, const ResPin *p, char got_hex[65])
{
    char path[1024];
    got_hex[0] = 0;
    if (join(root, p->path, path, sizeof path)) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, r);
    int bad = ferror(fp);
    fclose(fp);
    if (bad) return -1;
    uint8_t d[32];
    sha256_final(&c, d);
    res_hex(d, 32, got_hex);
    return strcmp(got_hex, p->sha256) == 0 ? 0 : 1;
}

void res_hex(const uint8_t *d, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[2 * n] = 0;
}

void res_json_str(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(o, "\\%c", c);
        else if (c < 0x20) fprintf(o, "\\u%04x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}

void res_mstats_json(FILE *o, const OmegaResMembraneStats *m)
{
    fprintf(o,
            ", \"membranes\": {\"states_observed\": %llu, \"states_implausible\": %llu, \"membranes_implausible\": %u, "
            "\"max_membrane_abs_v\": %.9g, \"max_node_abs_v\": %.9g}",
            (unsigned long long)m->states_observed, (unsigned long long)m->states_implausible, m->membranes_implausible,
            (double)m->max_membrane_abs_v, (double)m->max_node_abs_v);
}

double res_now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

const char *res_status_name(OmegaResStatus s)
{
    switch (s) {
    case OMEGA_RES_OK: return "OK";
    case OMEGA_RES_ERR_NULL: return "ERR_NULL";
    case OMEGA_RES_ERR_TOPOLOGY: return "ERR_TOPOLOGY";
    case OMEGA_RES_ERR_PARAM: return "ERR_PARAM";
    case OMEGA_RES_ERR_NONFINITE: return "ERR_NONFINITE";
    case OMEGA_RES_ERR_RANGE: return "ERR_RANGE";
    case OMEGA_RES_ERR_STIFF: return "ERR_STIFF";
    case OMEGA_RES_ERR_INPUT: return "ERR_INPUT";
    case OMEGA_RES_ERR_MISMATCH: return "ERR_MISMATCH";
    case OMEGA_RES_ERR_FPENV: return "ERR_FPENV";
    case OMEGA_RES_ERR_SINGULAR: return "ERR_SINGULAR";
    case OMEGA_RES_ERR_ALIAS: return "ERR_ALIAS";
    }
    return "ERR_UNKNOWN";
}

uint64_t res_rng_next(ResRng *r)
{
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double res_rng_unit(ResRng *r) { return (double)(res_rng_next(r) >> 11) * (1.0 / 9007199254740992.0); }

/* ----------------------------------------------------------- simulation */

OmegaResStatus res_sim_at_steps(ResSim *sim, const OmegaResInput *in, const uint64_t *steps, size_t n,
                                double *v_out)
{
    const uint32_t L = sim->cfg.topo.node_count;
    for (size_t k = 0; k < n; k++) {
        if (steps[k] < sim->s.step) return OMEGA_RES_ERR_INPUT;
        if (steps[k] > sim->s.step) {
            OmegaResStatus st = omega_res_advance_observed(&sim->cfg, in, &sim->ws, &sim->s, steps[k] - sim->s.step, &sim->mstats);
            if (st != OMEGA_RES_OK) return st;
        }
        for (uint32_t i = 0; i < L; i++) v_out[k * L + i] = (double)sim->s.v[i];
    }
    return OMEGA_RES_OK;
}

OmegaResStatus res_sim_at_positions(ResSim *sim, const OmegaResInput *in, const double *pos, size_t n,
                                    uint64_t last_step, double *v_out)
{
    const uint32_t L = sim->cfg.topo.node_count;
    uint64_t cached = UINT64_MAX; /* step whose successor is in sim->next */
    for (size_t k = 0; k < n; k++) {
        double p = pos[k];
        if (!(p >= 0.0) || p > (double)last_step) return OMEGA_RES_ERR_INPUT;
        uint64_t n0 = (uint64_t)p;
        double f = p - (double)n0;
        if (n0 < sim->s.step) return OMEGA_RES_ERR_INPUT;
        if (n0 > sim->s.step) {
            OmegaResStatus st = omega_res_advance_observed(&sim->cfg, in, &sim->ws, &sim->s, n0 - sim->s.step, &sim->mstats);
            if (st != OMEGA_RES_OK) return st;
        }
        if (f > 0.0) {
            if (cached != n0) {
                OmegaResStatus st = omega_res_step(&sim->cfg, in, &sim->ws, &sim->s, &sim->next);
                if (st != OMEGA_RES_OK) return st;
                cached = n0;
            }
            for (uint32_t i = 0; i < L; i++)
                v_out[k * L + i] = (1.0 - f) * (double)sim->s.v[i] + f * (double)sim->next.v[i];
        } else {
            for (uint32_t i = 0; i < L; i++) v_out[k * L + i] = (double)sim->s.v[i];
        }
    }
    return OMEGA_RES_OK;
}

/* ----------------------------------------------------------- statistics */

double res_sqrt(double x) { return __builtin_sqrt(x); }

double res_mean(const double *x, size_t n, size_t stride)
{
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += x[i * stride];
    return s / (double)n;
}

double res_std_pop(const double *x, size_t n, size_t stride)
{
    double m = res_mean(x, n, stride), s = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = x[i * stride] - m;
        s += d * d;
    }
    return res_sqrt(s / (double)n);
}

double res_rms_diff(const double *a, const double *b, size_t n, size_t stride)
{
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = a[i * stride] - b[i * stride];
        s += d * d;
    }
    return res_sqrt(s / (double)n);
}

double res_max_abs_diff(const double *a, const double *b, size_t n, size_t stride)
{
    double m = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = a[i * stride] - b[i * stride];
        if (d < 0) d = -d;
        if (!(d <= m)) m = d; /* NaN propagates as the maximum */
    }
    return m;
}

double res_nrmse(const double *ref, const double *pred, size_t n, size_t stride)
{
    double m = res_mean(ref, n, stride), num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; i++) {
        double e = ref[i * stride] - pred[i * stride], c = ref[i * stride] - m;
        num += e * e;
        den += c * c;
    }
    return res_sqrt(num / den);
}

void res_narma(const double *u, uint32_t n, uint32_t order, double *y)
{
    const uint32_t P = 20; /* narmaMaxOrder */
    double *Y = calloc((size_t)n + P, sizeof(double));
    double *uP = calloc((size_t)n + P, sizeof(double));
    if (!Y || !uP) {
        free(Y);
        free(uP);
        for (uint32_t i = 0; i < n; i++) y[i] = 0.0 / 0.0;
        return;
    }
    for (uint32_t i = 0; i < n; i++) uP[P + i] = u[i];
    for (uint32_t t = P - 1; t + 1 < n + P; t++) {
        double s = 0.0;
        for (uint32_t j = t + 1 - order; j <= t; j++) s = s + Y[j];
        Y[t + 1] = ((0.3 * Y[t] + (0.05 * Y[t]) * s) + (1.5 * uP[t + 1 - order]) * uP[t]) + 0.1;
    }
    for (uint32_t i = 0; i < n; i++) y[i] = Y[P + i];
    free(Y);
    free(uP);
}

/* ------------------------------------------- benchmark ridge readout + CV */

/* Heap workspaces (no statics): res_ridge_cv may run in several threads. */
static OmegaResStatus fit_apply(OmegaResReadoutWorkspace *ws, OmegaResReadout *r, const double *x, const double *y,
                                uint32_t n, uint32_t nf, uint32_t no, double lambda, const double *xa, uint32_t na,
                                double *pa)
{
    OmegaResStatus st = omega_res_readout_fit(x, y, n, nf, no, lambda, ws, r);
    if (st != OMEGA_RES_OK) return st;
    for (uint32_t i = 0; i < na; i++) {
        st = omega_res_readout_apply(r, xa + (size_t)i * nf, pa + (size_t)i * no);
        if (st != OMEGA_RES_OK) return st;
    }
    return OMEGA_RES_OK;
}

ResCvResult res_ridge_cv(const double *xtr, const double *ytr, uint32_t ntr, uint32_t nf, uint32_t no,
                         const double *xte, uint32_t nte, const double *grid, uint32_t ngrid, uint32_t folds,
                         double *pred_tr, double *pred_te)
{
    ResCvResult res = {0.0, 0.0, OMEGA_RES_ERR_PARAM};
    if (ntr < 2u * folds || nf < 1u || no < 1u || ngrid < 1u || folds < 2u) return res;
    double *mu = calloc(nf, sizeof(double)), *sd = calloc(nf, sizeof(double));
    double *ztr = malloc((size_t)ntr * nf * sizeof(double));
    double *zte = nte ? malloc((size_t)nte * nf * sizeof(double)) : NULL;
    double *zfit = malloc((size_t)ntr * nf * sizeof(double)), *yfit = malloc((size_t)ntr * no * sizeof(double));
    double *pv = malloc((size_t)ntr * no * sizeof(double));
    OmegaResReadoutWorkspace *ws = malloc(sizeof *ws);
    OmegaResReadout *rd = malloc(sizeof *rd);
    res.status = OMEGA_RES_ERR_NULL;
    if (!mu || !sd || !ztr || (nte && !zte) || !zfit || !yfit || !pv || !ws || !rd) goto out;
    for (uint32_t f = 0; f < nf; f++) {
        mu[f] = res_mean(xtr + f, ntr, nf);
        sd[f] = res_std_pop(xtr + f, ntr, nf);
        if (!(sd[f] > 0.0)) sd[f] = 1.0;
    }
    for (size_t i = 0; i < (size_t)ntr * nf; i++) ztr[i] = (xtr[i] - mu[i % nf]) / sd[i % nf];
    for (size_t i = 0; i < (size_t)nte * nf; i++) zte[i] = (xte[i] - mu[i % nf]) / sd[i % nf];
    double best = 1.0 / 0.0;
    int best_g = -1;
    for (uint32_t g = 0; g < ngrid; g++) {
        double score = 0.0;
        int ok = 1;
        for (uint32_t k = 0; k < folds && ok; k++) {
            uint32_t v0 = (uint32_t)(((uint64_t)2 * k * ntr + folds) / (2u * folds));
            uint32_t v1 = (uint32_t)(((uint64_t)2 * (k + 1) * ntr + folds) / (2u * folds));
            uint32_t m = 0;
            for (uint32_t i = 0; i < ntr; i++) {
                if (i >= v0 && i < v1) continue;
                memcpy(zfit + (size_t)m * nf, ztr + (size_t)i * nf, nf * sizeof(double));
                memcpy(yfit + (size_t)m * no, ytr + (size_t)i * no, no * sizeof(double));
                m++;
            }
            if (fit_apply(ws, rd, zfit, yfit, m, nf, no, grid[g], ztr + (size_t)v0 * nf, v1 - v0, pv) != OMEGA_RES_OK) {
                ok = 0;
                break;
            }
            double s = 0.0;
            for (uint32_t o = 0; o < no; o++) s += res_nrmse(ytr + (size_t)v0 * no + o, pv + o, v1 - v0, no);
            score += s / no;
        }
        if (!ok) continue;
        score /= folds;
        if (score < best) {
            best = score;
            best_g = (int)g;
        }
    }
    if (best_g < 0) {
        res.status = OMEGA_RES_ERR_SINGULAR;
        goto out;
    }
    res.lambda = grid[best_g];
    res.cv_score = best;
    {
        /* final fit on all training rows */
        res.status = omega_res_readout_fit(ztr, ytr, ntr, nf, no, res.lambda, ws, rd);
        for (uint32_t i = 0; res.status == OMEGA_RES_OK && i < ntr; i++)
            res.status = omega_res_readout_apply(rd, ztr + (size_t)i * nf, pred_tr + (size_t)i * no);
        for (uint32_t i = 0; res.status == OMEGA_RES_OK && pred_te && i < nte; i++)
            res.status = omega_res_readout_apply(rd, zte + (size_t)i * nf, pred_te + (size_t)i * no);
    }
out:
    free(mu);
    free(sd);
    free(ztr);
    free(zte);
    free(zfit);
    free(yfit);
    free(pv);
    free(ws);
    free(rd);
    return res;
}

int res_ols_qr(const double *x, const double *y, uint32_t n, uint32_t nf, double *w)
{
    const uint32_t p = nf + 1u;
    if (n < p) return -1;
    double *A = malloc((size_t)n * p * sizeof(double)), *b = malloc((size_t)n * sizeof(double));
    if (!A || !b) {
        free(A);
        free(b);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) { /* column-major copy of [1, x] */
        A[i] = 1.0;
        for (uint32_t f = 0; f < nf; f++) A[(size_t)(f + 1) * n + i] = x[(size_t)i * nf + f];
        b[i] = y[i];
    }
    /* column equilibration: unit 2-norm columns, so the rank test below is
     * relative to each column's own scale; undone on the weights */
    double *sc = malloc((size_t)p * sizeof(double));
    if (!sc) {
        free(A);
        free(b);
        return -1;
    }
    for (uint32_t j = 0; j < p; j++) {
        double s = 0.0, *a = A + (size_t)j * n;
        for (uint32_t i = 0; i < n; i++) s += a[i] * a[i];
        sc[j] = res_sqrt(s);
        if (sc[j] > 0.0)
            for (uint32_t i = 0; i < n; i++) a[i] /= sc[j];
    }
    double rmax = 0.0;
    int rc = 0;
    for (uint32_t j = 0; j < p && !rc; j++) {
        double *a = A + (size_t)j * n, s = 0.0;
        for (uint32_t i = j; i < n; i++) s += a[i] * a[i];
        double nrm = res_sqrt(s);
        if (!(nrm > 0.0)) {
            rc = -1;
            break;
        }
        double alpha = a[j] > 0.0 ? -nrm : nrm;
        a[j] -= alpha; /* v = a[j..n) */
        double vv = 0.0;
        for (uint32_t i = j; i < n; i++) vv += a[i] * a[i];
        for (uint32_t c = j + 1; c <= p; c++) {
            double *col = c < p ? A + (size_t)c * n : b, d = 0.0;
            for (uint32_t i = j; i < n; i++) d += a[i] * col[i];
            double t = 2.0 * d / vv;
            for (uint32_t i = j; i < n; i++) col[i] -= t * a[i];
        }
        a[j] = alpha; /* R_jj */
        double ar = alpha < 0 ? -alpha : alpha;
        if (ar > rmax) rmax = ar;
    }
    if (!rc) {
        for (uint32_t jj = p; jj-- > 0;) {
            double s = b[jj], rjj = A[(size_t)jj * n + jj];
            if (!((rjj < 0 ? -rjj : rjj) > 1e-14 * rmax)) {
                rc = -1;
                break;
            }
            for (uint32_t c = jj + 1; c < p; c++) s -= A[(size_t)c * n + jj] * w[c];
            w[jj] = s / rjj;
        }
    }
    if (!rc)
        for (uint32_t j = 0; j < p; j++) w[j] /= sc[j];
    free(sc);
    free(A);
    free(b);
    return rc;
}

/* ------------------------------------------------------------- networks */

#include "res_fixtures.h"

static uint32_t nb_edges(const uint8_t (*nb)[7], uint32_t n, OmegaResEdge *e)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 1; k <= nb[i][0]; k++) {
            uint32_t a = i, b = (uint32_t)nb[i][k] - 1u, lo = a < b ? a : b, hi = a < b ? b : a, dup = 0;
            for (uint32_t q = 0; q < m && !dup; q++) dup = e[q].a == lo && e[q].b == hi;
            if (!dup && lo != hi) {
                e[m].a = (uint16_t)lo;
                e[m].b = (uint16_t)hi;
                m++;
            }
        }
    return m;
}

OmegaResStatus res_net_topology(ResNet net, OmegaResTopology *t, uint32_t inputs0[3], uint32_t *ground0)
{
    OmegaResEdge e[OMEGA_RES_MAX_EDGES];
    uint32_t m, n, g;
    const uint8_t *in = NULL;
    switch (net) {
    case RES_NET_HEX13: n = 13; m = nb_edges(RES_FIX_HEX13_NB, n, e); g = RES_FIX_HEX13_GROUND; in = RES_FIX_HEX13_INPUTS; break;
    case RES_NET_HEX25: n = 25; m = nb_edges(RES_FIX_HEX25_NB, n, e); g = RES_FIX_HEX25_GROUND; in = RES_FIX_HEX25_INPUTS; break;
    case RES_NET_HEX43: n = 43; m = nb_edges(RES_FIX_HEX43_NB, n, e); g = RES_FIX_HEX43_GROUND; in = RES_FIX_HEX43_INPUTS; break;
    case RES_NET_CHAIN50:
        n = 50;
        m = 49;
        for (uint32_t k = 0; k < m; k++) {
            e[k].a = (uint16_t)(RES_FIX_CHAIN50_EDGES[k][0] - 1u);
            e[k].b = (uint16_t)(RES_FIX_CHAIN50_EDGES[k][1] - 1u);
        }
        g = RES_FIX_CHAIN50_GROUND;
        break;
    default: return OMEGA_RES_ERR_PARAM;
    }
    g -= 1u;
    if (ground0) *ground0 = g;
    if (in && inputs0)
        for (int k = 0; k < 3; k++) inputs0[k] = (uint32_t)in[k] - 1u;
    return omega_res_topology_build(n, e, m, &g, 1, t);
}

OmegaResStatus res_net_disconnected(OmegaResReference which, const float *conc, uint32_t nd, OmegaResTopology *t,
                                    OmegaResParams *p)
{
    OmegaResEdge e[OMEGA_RES_MAX_EDGES];
    uint32_t grounds[OMEGA_RES_MAX_NODES], ng = 0, node = nd;
    float gconc[OMEGA_RES_MAX_NODES];
    if (nd == 0 || nd >= OMEGA_RES_MAX_NODES) return OMEGA_RES_ERR_PARAM;
    for (uint32_t k = 0; k < nd; k++) {
        uint32_t found = UINT32_MAX;
        for (uint32_t q = 0; q < ng; q++) /* a ground of this class with < 6 members */
            if (gconc[q] == conc[k] && t->degree[grounds[q]] < OMEGA_RES_MAX_DEGREE) found = q;
        if (found == UINT32_MAX) {
            if (node >= OMEGA_RES_MAX_NODES) return OMEGA_RES_ERR_TOPOLOGY;
            found = ng;
            grounds[ng] = node++;
            gconc[ng] = conc[k];
            t->degree[grounds[ng]] = 0;
            ng++;
        }
        e[k].a = (uint16_t)k;
        e[k].b = (uint16_t)grounds[found];
        t->degree[grounds[found]]++; /* scratch count, rebuilt below */
    }
    OmegaResStatus st = omega_res_topology_build(node, e, nd, grounds, ng, t);
    if (st != OMEGA_RES_OK) return st;
    st = omega_res_params_reference(which, node, p);
    if (st != OMEGA_RES_OK) return st;
    for (uint32_t k = 0; k < nd; k++) p->conc_um[k] = conc[k];
    for (uint32_t q = 0; q < ng; q++) p->conc_um[grounds[q]] = gconc[q];
    return OMEGA_RES_OK;
}
