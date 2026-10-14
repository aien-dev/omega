/* res_qualify: omega#440 Gate 2 tool. Contract and protocol:
 * docs/reservoir/RESERVOIR_REFERENCE.md sections 7, 7.1 and 10.
 *
 *   res_qualify pins <archive>                 sha256 of every pinned file
 *   res_qualify g2-narma <archive> <N1a|N1b|NC-N>
 *   res_qualify g2-narma <archive> N1b-v2 <v2 results .mat>
 *   res_qualify g2-lorenz <archive> <13|25|43> [NC-L]
 *   res_qualify bench-narma [seed..]           (res_bench.c; default seeds 1-5)
 *   res_qualify bench-lorenz [seed..]          (res_bench.c; default seeds 1-5)
 *   res_qualify perf                           (res_bench.c)
 *   res_qualify config                         the declared constants
 *
 * <archive> is the unpacked project-zenodo-ready_with-paper/ directory of
 * Zenodo 10.5281/zenodo.17296788. Its files are data: read, hashed and
 * parsed by the in-house MAT reader, never executed. Each subcommand writes
 * one JSON document to stdout and exits 0 when its verdict is PASS (or, for
 * pins and the benchmarks, when it ran), 1 on FAIL, 2 on a usage or input
 * error. */
#include "res_common.h"

#include <stdlib.h>
#include <string.h>

int res_bench_narma(int argc, char **argv, FILE *o);
int res_bench_lorenz(int argc, char **argv, FILE *o);
int res_perf(FILE *o);

/* Declared before any run (section 7, 7.1). */
#define TOL_TARGET_ABS 1e-12
#define TOL_PRED_REL 0.01
#define TOL_NRMSE_ABS 0.005
#define TOL_NRMSE_REL 0.05
#define TOL_METRIC_REL 1e-9
#define STEP_S 1.0e-5f
#define STEPS_PER_KNOT 10u

/* Zenodo record 20277956 (version 2), src/Codes for Zenodo/NARMA Final Code/
 * Connected Reservoir/HyperParameterOptimizationVariablesToPlot.mat, 14,525,399
 * bytes (section 1.1, item 10 of section 7.1). */
static const uint8_t V2_RESULTS_SHA256[32] = {
    0x31, 0x7a, 0xf7, 0xa7, 0x60, 0x51, 0xe6, 0xc3, 0x0a, 0xa0, 0x47, 0x6a, 0x61, 0x2c, 0x29, 0xe4,
    0x17, 0xb1, 0x99, 0xc9, 0xef, 0x22, 0x6f, 0x4f, 0x00, 0xd7, 0xf7, 0x50, 0x55, 0x12, 0xfc, 0x92};

static void jd(FILE *o, double v) /* JSON number or null */
{
    if (v == v && v - v == 0.0) fprintf(o, "%.17g", v);
    else fputs("null", o);
}

static void jhex(FILE *o, const uint8_t *d)
{
    char h[65];
    res_hex(d, 32, h);
    fprintf(o, "\"%s\"", h);
}

static void receipt_json(FILE *o, const char *split, OmegaResStatus st, const OmegaResRunReceipt *r,
                         const OmegaResMembraneStats *ms)
{
    fprintf(o, "{\"split\": \"%s\", \"status\": \"%s\"", split, res_status_name(st));
    if (st == OMEGA_RES_OK) {
        fputs(", \"config_id\": ", o);
        jhex(o, r->config_id);
        fputs(", \"input_digest\": ", o);
        jhex(o, r->input_digest);
        fputs(", \"final_state_digest\": ", o);
        jhex(o, r->final_state_digest);
        fprintf(o, ", \"steps\": %llu, \"run_id\": ", (unsigned long long)r->steps);
        jhex(o, r->run_id);
    }
    res_mstats_json(o, ms);
    fputc('}', o);
}

static int nrmse_ok(double got, double ref)
{
    double d = got - ref, tol = TOL_NRMSE_REL * ref;
    if (d < 0) d = -d;
    if (tol < TOL_NRMSE_ABS) tol = TOL_NRMSE_ABS;
    return d <= tol;
}

static int rel_ok(double got, double ref, double rel)
{
    double d = got - ref, s = ref < 0 ? -ref : ref;
    if (d < 0) d = -d;
    return d <= rel * s;
}

/* ---------------------------------------------------------------- pins */

static int cmd_pins(const char *root, FILE *o)
{
    int bad = 0;
    fputs("{\n  \"schema\": \"omega-reservoir-pins/1\",\n  \"record\": \"10.5281/zenodo.17296788\",\n  \"files\": [", o);
    for (unsigned i = 0; i < RES_PIN_COUNT; i++) {
        char got[65];
        int r = res_check_pinned(root, &RES_PINS[i], got);
        bad |= r != 0;
        fprintf(o, "%s\n    {\"key\": \"%s\", \"path\": ", i ? "," : "", RES_PINS[i].key);
        res_json_str(o, RES_PINS[i].path);
        fprintf(o, ", \"sha256\": \"%s\", \"match\": %s}", got[0] ? got : "missing", r == 0 ? "true" : "false");
    }
    fprintf(o, "\n  ],\n  \"verdict\": \"%s\"\n}\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------- G2-N1 */

/* Collinearity of the design [1, x] (n x nf row-major): LDL^T of its Gram
 * matrix in binary64 without refusal; returns min_j d_j / G_jj, the squared
 * sine of the angle between column j and the span of the columns before it
 * (scale-free). omega_res_readout_fit refuses when this is <= 1e-13. */
static double design_min_pivot_ratio(const double *x, uint32_t n, uint32_t nf, uint32_t *argmin)
{
    const uint32_t p = nf + 1u;
    double *G = calloc((size_t)p * p, sizeof(double)), best = 1.0 / 0.0;
    if (!G) return 0.0 / 0.0;
    for (uint32_t r = 0; r < n; r++)
        for (uint32_t i = 0; i < p; i++) {
            double xi = i ? x[(size_t)r * nf + i - 1u] : 1.0;
            for (uint32_t j = 0; j <= i; j++) G[(size_t)i * p + j] += xi * (j ? x[(size_t)r * nf + j - 1u] : 1.0);
        }
    *argmin = 0;
    for (uint32_t j = 0; j < p; j++) { /* in place: L below the diagonal, D on it */
        double gjj = G[(size_t)j * p + j], d = gjj;
        for (uint32_t k = 0; k < j; k++) d -= G[(size_t)j * p + k] * G[(size_t)j * p + k] * G[(size_t)k * p + k];
        double ratio = gjj > 0.0 ? d / gjj : 0.0;
        if (ratio < best) {
            best = ratio;
            *argmin = j;
        }
        G[(size_t)j * p + j] = d;
        for (uint32_t i = j + 1u; i < p; i++) {
            double s = G[(size_t)i * p + j];
            for (uint32_t k = 0; k < j; k++) s -= G[(size_t)i * p + k] * G[(size_t)j * p + k] * G[(size_t)k * p + k];
            G[(size_t)i * p + j] = d != 0.0 ? s / d : 0.0;
        }
    }
    free(G);
    return best;
}

/* Sensitivity of the least-squares predictions to a relative perturbation
 * rel of every feature value (deterministic: splitmix64 seed 1, uniform in
 * [-1, 1)): RMS change of the fitted predictions over the fit rows, divided
 * by the target std. A diagnostic of how well any integrator with that
 * relative state error could reproduce the predictions. -1 if refused. */
static double qr_sensitivity(const double *x, const double *y, uint32_t n, uint32_t nf, double rel)
{
    double *xp = malloc((size_t)n * nf * sizeof(double)), *w0 = malloc((nf + 1u) * sizeof(double)),
           *w1 = malloc((nf + 1u) * sizeof(double)), acc = 0.0;
    ResRng rng = {1};
    int ok = xp && w0 && w1;
    if (ok) {
        for (size_t k = 0; k < (size_t)n * nf; k++) xp[k] = x[k] * (1.0 + rel * (2.0 * res_rng_unit(&rng) - 1.0));
        ok = res_ols_qr(x, y, n, nf, w0) == 0 && res_ols_qr(xp, y, n, nf, w1) == 0;
    }
    for (uint32_t r = 0; ok && r < n; r++) {
        double a = w0[0], b = w1[0];
        for (uint32_t c = 0; c < nf; c++) {
            a += x[(size_t)r * nf + c] * w0[c + 1u];
            b += xp[(size_t)r * nf + c] * w1[c + 1u];
        }
        acc += (a - b) * (a - b);
    }
    double sd = res_std_pop(y, n, 1), out = ok ? res_sqrt(acc / n) / sd : -1.0;
    free(xp);
    free(w0);
    free(w1);
    return out;
}

typedef struct {
    const double *u, *I, *tm; /* u: 1000, I: knots x L column-major, tm: knots */
    uint32_t n, knots;
} NarmaSignal;

static int narma_signal(const ResMatFile *f, const char *un, const char *in, const char *tn, uint32_t L,
                        NarmaSignal *s)
{
    const ResMatArray *u = res_mat_find(f, un), *I = res_mat_find(f, in), *t = res_mat_find(f, tn);
    if (!u || !I || !t || res_mat_values(u, 0, &s->u) || res_mat_values(I, 0, &s->I) || res_mat_values(t, 0, &s->tm))
        return -1;
    s->n = (uint32_t)u->numel;
    s->knots = I->dims[0];
    if (I->dims[1] != L || t->numel != s->knots) return -1;
    return 0;
}

static int cmd_g2_narma(const char *root, const char *variant, const char *v2_results, FILE *o)
{
    const int v2 = strcmp(variant, "N1b-v2") == 0, squares = v2 || strcmp(variant, "N1b") == 0,
              control = strcmp(variant, "NC-N") == 0;
    if (!squares && !control && strcmp(variant, "N1a") != 0) return 2;
    if (v2 != (v2_results != NULL)) return 2;
    static ResMatFile flen, ftr, fte, fres;
    if (res_load_pinned(root, "narma.length5", &flen) || res_load_pinned(root, "narma.train_signal", &ftr) ||
        res_load_pinned(root, "narma.test_signal", &fte))
        return 2;
    if (v2) {
        ResMatStatus ms = res_mat_load(v2_results, V2_RESULTS_SHA256, &fres);
        if (ms != RES_MAT_OK) {
            fprintf(stderr, "res_qualify: v2 results %s: %s\n", v2_results, res_mat_strerror(ms));
            return 2;
        }
    } else if (res_load_pinned(root, "narma.results", &fres))
        return 2;
    const ResMatArray *cm = res_mat_find(&flen, "conmat"), *gr = res_mat_find(&flen, "ground"),
                      *ix = res_mat_find(&flen, "inex");
    const double *conmat, *ground, *inex, *spatial, *cur, *mask;
    if (res_mat_values(cm, 25, &conmat) || res_mat_values(gr, 1, &ground) || res_mat_values(ix, 25, &inex) ||
        res_mat_values(res_mat_find(&fres, "spatialNodeOptions"), 4, &spatial) ||
        res_mat_values(res_mat_find(&fres, "currentUpperSelectionValues"), 4, &cur) ||
        res_mat_values(res_mat_find(&fres, "maskLengthOptions"), 4, &mask)) {
        fprintf(stderr, "res_qualify: archived NARMA variables missing\n");
        return 2;
    }
    const uint32_t L = 5, M = (uint32_t)mask[0], PW = 100; /* 1 ms = 100 steps */
    const double hi = cur[0];
    /* topology from the archived conmat */
    OmegaResEdge e[OMEGA_RES_MAX_EDGES];
    uint32_t m = 0, sym = 1, inex_all1 = 1;
    for (uint32_t j = 0; j < L; j++)
        for (uint32_t i = 0; i < L; i++) {
            double a = conmat[i + L * j];
            if (a != conmat[j + L * i]) sym = 0;
            if (inex[i + L * j] != 1.0) inex_all1 = 0;
            if (i < j && a != 0.0) {
                e[m].a = (uint16_t)i;
                e[m].b = (uint16_t)j;
                m++;
            }
        }
    uint32_t g = (uint32_t)ground[0] - 1u;
    OmegaResTopology topo, chain;
    OmegaResParams par;
    OmegaResIntegrator integ = {STEP_S, STEPS_PER_KNOT};
    ResSim *sim = calloc(1, sizeof *sim);
    OmegaResConfig *ccfg = calloc(1, sizeof *ccfg);
    if (!sim || !ccfg) return 2;
    OmegaResStatus st = omega_res_topology_build(L, e, m, &g, 1, &topo);
    if (st == OMEGA_RES_OK) st = omega_res_params_reference(OMEGA_RES_REF_NARMA, L, &par);
    if (control) par.eps_f_per_m = par.eps_f_per_m * 2.1f;
    if (st == OMEGA_RES_OK) st = omega_res_config_build(&topo, &par, &integ, &sim->cfg);
    int chain_same = 0;
    if (st == OMEGA_RES_OK && omega_res_topology_chain(L, L - 1u, &chain) == OMEGA_RES_OK &&
        omega_res_config_build(&chain, &par, &integ, ccfg) == OMEGA_RES_OK)
        chain_same = memcmp(ccfg->id, sim->cfg.id, 32) == 0;
    if (st != OMEGA_RES_OK) {
        fprintf(stderr, "res_qualify: config refused: %s\n", res_status_name(st));
        return 2;
    }
    NarmaSignal sig[2];
    if (narma_signal(&ftr, "uTrain", "Itrain", "trainsignaltime", L, &sig[0]) ||
        narma_signal(&fte, "uTest", "Itest", "testsignaltime", L, &sig[1]) || sig[0].n != 1000 || sig[1].n != 1000 ||
        sig[0].knots != sig[0].n * M * 10u || sig[1].knots != sig[1].n * M * 10u) {
        fprintf(stderr, "res_qualify: archived NARMA signals have unexpected shapes\n");
        return 2;
    }
    const uint32_t N = sig[0].n, KN = sig[0].knots;
    const uint32_t nsc = L * M * (squares ? 2u : 1u), ncol = nsc + 1u, nrow = N + 1u;
    const uint32_t r0 = 99, r1 = 995, nfit = r1 - r0; /* 1-based rows 100..995 */
    double *feat[2], *ys[2][11];
    float *tab = malloc((size_t)KN * L * sizeof(float));
    uint64_t *steps = malloc((size_t)N * M * sizeof(uint64_t));
    double *vs = malloc((size_t)N * M * L * sizeof(double));
    if (!tab || !steps || !vs) return 2;
    double max_round = 0.0, max_tdev = 0.0;
    uint32_t mask_bits[2][5][8] = {{{0}}}, mask_incons[2] = {0, 0};
    double isum_lo = 1.0 / 0.0, isum_hi = -1.0 / 0.0; /* total current into the non-ground nodes */
    OmegaResRunReceipt rc[2];
    OmegaResStatus rst[2];
    OmegaResMembraneStats ms[2];
    for (int sp = 0; sp < 2; sp++) {
        const NarmaSignal *s = &sig[sp];
        feat[sp] = calloc((size_t)nrow * ncol, sizeof(double));
        if (!feat[sp]) return 2;
        for (uint32_t k = 0; k < KN; k++) {
            double dev = s->tm[k] - (double)k / 10000.0;
            if (dev < 0) dev = -dev;
            if (dev > max_tdev) max_tdev = dev;
            double isum = 0.0;
            for (uint32_t i = 0; i < L; i++)
                if (i != g) isum += s->I[k + (size_t)KN * i];
            if (isum < isum_lo) isum_lo = isum;
            if (isum > isum_hi) isum_hi = isum;
            for (uint32_t i = 0; i < L; i++) {
                double x = s->I[k + (size_t)KN * i];
                float f = (float)x;
                tab[(size_t)k * L + i] = f;
                double r = x != 0.0 ? ((double)f - x) / x : 0.0;
                if (r < 0) r = -r;
                if (r > max_round) max_round = r;
            }
        }
        /* mask bits read back from the archived currents (informational) */
        for (uint32_t i = 0; i < L; i++)
            for (uint32_t mi = 0; mi < M; mi++) {
                uint32_t ones = 0;
                for (uint32_t c = 0; c < N; c++) {
                    double x = s->I[(size_t)(c * M * 10u + mi * 10u) + (size_t)KN * i], seq = s->u[c] * hi / 0.5;
                    double d1 = x - seq, d0 = x - (hi - seq);
                    if (d1 < 0) d1 = -d1;
                    if (d0 < 0) d0 = -d0;
                    ones += d1 <= d0;
                }
                mask_bits[sp][i][mi] = ones * 2u > N;
                if (ones != 0 && ones != N) mask_incons[sp]++;
            }
        OmegaResInput in = {L, KN, tab};
        const uint64_t last = (uint64_t)(KN - 1u) * STEPS_PER_KNOT;
        uint32_t ns = 0, nvalid = 0;
        for (uint32_t i = 1; i <= N; i++) { /* reference row i: t = i*pw*M + k*pw */
            uint64_t top = ((uint64_t)i * M + M) * PW;
            if (top > last) break;
            for (uint32_t k = 1; k <= M; k++) steps[ns++] = ((uint64_t)i * M + k) * PW;
            nvalid++;
        }
        omega_res_membrane_stats_init(&sim->mstats);
        rst[sp] = omega_res_state_init(&sim->cfg, OMEGA_RES_INIT_N0, 0.0f, &sim->s);
        if (rst[sp] == OMEGA_RES_OK) rst[sp] = res_sim_at_steps(sim, &in, steps, ns, vs);
        if (rst[sp] == OMEGA_RES_OK && sim->s.step < last)
            rst[sp] = omega_res_advance_observed(&sim->cfg, &in, &sim->ws, &sim->s, last - sim->s.step, &sim->mstats);
        if (rst[sp] == OMEGA_RES_OK) rst[sp] = omega_res_run_receipt(&sim->cfg, &in, &sim->s, &rc[sp]);
        ms[sp] = sim->mstats;
        /* features: row 0 zeros; row r >= 1 = reference sample row r (1-based), node-major */
        for (uint32_t r = 1; r <= nvalid; r++)
            for (uint32_t node = 0; node < L; node++)
                for (uint32_t k = 0; k < M; k++) {
                    double v = vs[((size_t)(r - 1) * M + k) * L + node];
                    feat[sp][(size_t)r * ncol + node * M + k] = v;
                    if (squares) feat[sp][(size_t)r * ncol + L * M + node * M + k] = v * v;
                }
        for (uint32_t r = 1; r <= N; r++) feat[sp][(size_t)r * ncol + nsc] = s->u[r - 1];
        for (uint32_t ord = 2; ord <= 10; ord++) {
            ys[sp][ord] = malloc((size_t)N * sizeof(double));
            if (!ys[sp][ord]) return 2;
            res_narma(s->u, N, ord, ys[sp][ord]);
        }
    }
    /* drop columns identically zero over the training fit rows */
    uint32_t keep[64], nk = 0, dropped[64], nd = 0;
    for (uint32_t c = 0; c < ncol; c++) {
        int zero = 1;
        for (uint32_t r = r0; r < r1 && zero; r++) zero = feat[0][(size_t)r * ncol + c] == 0.0;
        if (zero) dropped[nd++] = c;
        else keep[nk++] = c;
    }
    double *xf[2];
    for (int sp = 0; sp < 2; sp++) {
        xf[sp] = malloc((size_t)nfit * nk * sizeof(double));
        if (!xf[sp]) return 2;
        for (uint32_t r = 0; r < nfit; r++)
            for (uint32_t c = 0; c < nk; c++) xf[sp][(size_t)r * nk + c] = feat[sp][(size_t)(r0 + r) * ncol + keep[c]];
    }
    const ResMatArray *cells[4] = {res_mat_find(&fres, "yTrainCell"), res_mat_find(&fres, "yTestCell"),
                                   res_mat_find(&fres, "predictionTrainCell"), res_mat_find(&fres, "predictionTestCell")};
    const double *nr_ref[2];
    if (res_mat_values(res_mat_find(&fres, "NRMSEtrain"), 640, &nr_ref[0]) ||
        res_mat_values(res_mat_find(&fres, "NRMSEtest"), 640, &nr_ref[1]) || !cells[0] || !cells[1] || !cells[2] ||
        !cells[3])
        return 2;
    static OmegaResReadoutWorkspace rws;
    static OmegaResReadout ro;
    double *pred = malloc((size_t)nfit * sizeof(double)), *pqr = malloc((size_t)nfit * sizeof(double));
    double *w = malloc((size_t)(nk + 1u) * sizeof(double));
    if (!pred || !pqr || !w) return 2;
    int all_pass = rst[0] == OMEGA_RES_OK && rst[1] == OMEGA_RES_OK && sym && inex_all1;

    fprintf(o, "{\n  \"schema\": \"omega-reservoir-g2/1\",\n  \"case\": \"G2-N1\",\n  \"variant\": \"%s\",\n", variant);
    fprintf(o, "  \"negative_control\": %s,\n", control ? "\"NC-N: permittivity x 2.1\"" : "false");
    fprintf(o, "  \"reference_results\": \"%s\",\n",
            v2 ? "Zenodo record 20277956 (version 2), sha256 317af7a7...fc92" : "Zenodo record 17296788 (version 1, pinned)");
    fprintf(o, "  \"grid_cell\": {\"nodes\": %u, \"current_hi_a\": ", L);
    jd(o, hi);
    fprintf(o, ", \"mask_length\": %u, \"spatial_option0\": ", M);
    jd(o, spatial[0]);
    fprintf(o, ", \"condition\": 7},\n  \"features\": \"%s\",\n",
            squares ? "v2 code: [V, V^2, u] (node-major, M per node)" : "v1 code: [V, u] (node-major, M per node)");
    fprintf(o, "  \"topology\": {\"edges\": %u, \"ground_1based\": %u, \"symmetric\": %s, \"inex_all_1\": %s, "
               "\"same_config_id_as_omega_chain\": %s},\n",
            m, g + 1u, sym ? "true" : "false", inex_all1 ? "true" : "false", chain_same ? "true" : "false");
    fprintf(o, "  \"step_s\": %.9g, \"steps_per_knot\": %u, \"permittivity_f_per_m\": %.9g,\n", (double)STEP_S,
            STEPS_PER_KNOT, (double)par.eps_f_per_m);
    fprintf(o, "  \"inputs\": {\"knots\": %u, \"knot_spacing_s\": 1e-4, \"max_rel_binary32_rounding\": ", KN);
    jd(o, max_round);
    fputs(", \"max_time_grid_deviation_s\": ", o);
    jd(o, max_tdev);
    fputs(", \"total_nonground_current_min_a\": ", o);
    jd(o, isum_lo);
    fputs(", \"total_nonground_current_max_a\": ", o);
    jd(o, isum_hi);
    fputs(", \"mask_bits_train\": [", o);
    for (uint32_t i = 0; i < L; i++) {
        fprintf(o, "%s[", i ? ", " : "");
        for (uint32_t k = 0; k < M; k++) fprintf(o, "%s%u", k ? ", " : "", mask_bits[0][i][k]);
        fputc(']', o);
    }
    fputs("], \"mask_bits_test\": [", o);
    for (uint32_t i = 0; i < L; i++) {
        fprintf(o, "%s[", i ? ", " : "");
        for (uint32_t k = 0; k < M; k++) fprintf(o, "%s%u", k ? ", " : "", mask_bits[1][i][k]);
        fputc(']', o);
    }
    fprintf(o, "], \"mask_inconsistent_slots\": [%u, %u], \"masks_equal_train_test\": %s},\n", mask_incons[0],
            mask_incons[1], memcmp(mask_bits[0], mask_bits[1], sizeof mask_bits[0]) == 0 ? "true" : "false");
    fputs("  \"runs\": [", o);
    receipt_json(o, "train", rst[0], &rc[0], &ms[0]);
    fputs(", ", o);
    receipt_json(o, "test", rst[1], &rc[1], &ms[1]);
    fprintf(o, "],\n  \"fit\": {\"rows_1based\": [100, 995], \"columns\": %u, \"columns_dropped_zero\": [", ncol);
    for (uint32_t k = 0; k < nd; k++) fprintf(o, "%s%u", k ? ", " : "", dropped[k]);
    uint32_t argp = 0;
    double mpr = design_min_pivot_ratio(xf[0], nfit, nk, &argp);
    fputs("], \"column_rms\": [", o);
    for (uint32_t c = 0; c < nk; c++) {
        double s = 0.0;
        for (uint32_t r = 0; r < nfit; r++) s += xf[0][(size_t)r * nk + c] * xf[0][(size_t)r * nk + c];
        fputs(c ? ", " : "", o);
        jd(o, res_sqrt(s / nfit));
    }
    fputs("], \"node_last_minus_first_subpulse_rms\": [", o);
    for (uint32_t node = 0; node < L; node++) { /* the information between the M samples of a node */
        double s = 0.0;
        for (uint32_t r = r0; r < r1; r++) {
            double d = feat[0][(size_t)r * ncol + node * M + M - 1u] - feat[0][(size_t)r * ncol + node * M];
            s += d * d;
        }
        fputs(node ? ", " : "", o);
        jd(o, res_sqrt(s / nfit));
    }
    fputs("], \"design_min_pivot_ratio\": ", o);
    jd(o, mpr);
    fprintf(o, ", \"design_min_pivot_column\": %u, \"readout_refuses_at_or_below\": 1e-13", argp);
    fprintf(o, ", \"solver\": \"omega_res_readout_fit lambda=0\"},\n");
    fprintf(o, "  \"tolerances\": {\"target_max_abs\": %g, \"prediction_rms_over_target_std\": %g, "
               "\"nrmse_abs\": %g, \"nrmse_rel\": %g, \"metric_recompute_rel\": %g},\n",
            TOL_TARGET_ABS, TOL_PRED_REL, TOL_NRMSE_ABS, TOL_NRMSE_REL, TOL_METRIC_REL);
    fputs("  \"orders\": [", o);
    int fit_failed = 0;
    for (uint32_t ord = 2; ord <= 10; ord++) {
        uint32_t sub[4] = {0, 0, 0, ord - 1u};
        const double *ya[2], *pa[2];
        int ok = 1;
        for (int sp = 0; sp < 2; sp++) {
            const ResMatArray *yc = res_mat_cell_at(cells[sp], sub, 4), *pc = res_mat_cell_at(cells[2 + sp], sub, 4);
            if (res_mat_values(yc, nfit, &ya[sp]) || res_mat_values(pc, nfit, &pa[sp])) ok = 0;
        }
        if (!ok) return 2;
        const double *yfit = ys[0][ord] + r0;
        st = omega_res_readout_fit(xf[0], yfit, nfit, nk, 1, 0.0, &rws, &ro);
        int qr = res_ols_qr(xf[0], yfit, nfit, nk, w);
        fprintf(o, "%s\n    {\"order\": %u, \"fit_status\": \"%s\", \"qr_rank_ok\": %s", ord > 2 ? "," : "", ord,
                res_status_name(st), qr == 0 ? "true" : "false");
        fputs(", \"qr_sensitivity_rel_1e-6\": ", o);
        jd(o, qr_sensitivity(xf[0], yfit, nfit, nk, 1e-6));
        int pa_ok = 1, pb_ok = 1, pc_ok = 1, pm_ok = 1;
        for (int sp = 0; sp < 2; sp++) {
            const char *nm = sp ? "test" : "train";
            const double *y = ys[sp][ord] + r0;
            double tdiff = res_max_abs_diff(y, ya[sp], nfit, 1), sd = res_std_pop(ya[sp], nfit, 1);
            double prel = 1.0 / 0.0, nr = 1.0 / 0.0, qdiff = 1.0 / 0.0, qprel = 1.0 / 0.0, qnr = 1.0 / 0.0;
            for (uint32_t r = 0; r < nfit; r++) {
                if (st != OMEGA_RES_OK || omega_res_readout_apply(&ro, xf[sp] + (size_t)r * nk, &pred[r]) != OMEGA_RES_OK)
                    pred[r] = 0.0 / 0.0;
                double q = w[0];
                for (uint32_t c = 0; c < nk; c++) q += xf[sp][(size_t)r * nk + c] * w[c + 1u];
                pqr[r] = q;
            }
            if (st == OMEGA_RES_OK) {
                prel = res_rms_diff(pred, pa[sp], nfit, 1) / sd;
                nr = res_nrmse(y, pred, nfit, 1);
                if (qr == 0) qdiff = res_max_abs_diff(pred, pqr, nfit, 1) / sd;
            }
            if (qr == 0) { /* diagnostic only (section 7.1 item 3) */
                qprel = res_rms_diff(pqr, pa[sp], nfit, 1) / sd;
                qnr = res_nrmse(y, pqr, nfit, 1);
            }
            double nref = nr_ref[sp][64u * (ord - 1u)], narch = res_nrmse(ya[sp], pa[sp], nfit, 1);
            int a = tdiff <= TOL_TARGET_ABS, b = prel <= TOL_PRED_REL, c = nrmse_ok(nr, nref),
                pm = rel_ok(narch, nref, TOL_METRIC_REL);
            pa_ok &= a;
            pb_ok &= b;
            pc_ok &= c;
            pm_ok &= pm;
            fprintf(o, ", \"%s\": {\"target_max_abs_diff\": ", nm);
            jd(o, tdiff);
            fputs(", \"prediction_rms_diff_over_target_std\": ", o);
            jd(o, prel);
            fputs(", \"nrmse_omega\": ", o);
            jd(o, nr);
            fputs(", \"nrmse_reference\": ", o);
            jd(o, nref);
            fputs(", \"nrmse_recomputed_from_archived_predictions\": ", o);
            jd(o, narch);
            fputs(", \"qr_minus_ldl_max_abs_over_target_std\": ", o);
            jd(o, qdiff);
            fputs(", \"qr_diagnostic\": {\"prediction_rms_diff_over_target_std\": ", o);
            jd(o, qprel);
            fputs(", \"nrmse_omega\": ", o);
            jd(o, qnr);
            fprintf(o, ", \"b\": %s, \"c\": %s}", qprel <= TOL_PRED_REL ? "true" : "false", nrmse_ok(qnr, nref) ? "true" : "false");
            fprintf(o, ", \"a\": %s, \"b\": %s, \"c\": %s, \"metric_control\": %s}", a ? "true" : "false",
                    b ? "true" : "false", c ? "true" : "false", pm ? "true" : "false");
        }
        int pass = st == OMEGA_RES_OK && pa_ok && pb_ok && pc_ok;
        if (st != OMEGA_RES_OK) fit_failed = 1;
        if (!pm_ok) all_pass = 0; /* the metric itself would be in doubt */
        all_pass &= pass;
        fprintf(o, ", \"pass\": %s}", pass ? "true" : "false");
    }
    const char *verdict = all_pass ? "PASS" : "FAIL";
    fprintf(o, "\n  ],\n  \"fit_refused\": %s,\n  \"verdict\": \"%s\"\n}\n", fit_failed ? "true" : "false", verdict);
    return all_pass ? 0 : 1;
}

/* ------------------------------------------------------------- G2-L1 */

typedef struct {
    const double *X, *t;
    uint32_t n;
} LorenzData;

static int lorenz_data(const ResMatFile *f, LorenzData *d)
{
    const ResMatArray *X = res_mat_find(f, "X"), *t = res_mat_find(f, "t");
    if (!X || !t || res_mat_values(X, 0, &d->X) || res_mat_values(t, 0, &d->t) || X->dims[1] != 3 ||
        t->numel != X->dims[0])
        return -1;
    d->n = X->dims[0];
    for (uint32_t j = 1; j < d->n; j++)
        if (!(d->t[j] > d->t[j - 1])) return -1;
    return 0;
}

/* Current of channel c at data point j, exactly as Lorenz63SolverCustomRegression.m:46-49. */
static double lorenz_current(const LorenzData *d, uint32_t j, int c)
{
    double x = d->X[j + (size_t)d->n * c];
    if (c < 2) return (x * 1.05e-10 + 1e-9) / 10.0;
    return (x * 1e-10 * 2.05) / 10.0;
}

static double interp_col(const LorenzData *d, const double *col, double t, uint32_t *hint)
{
    uint32_t j = *hint;
    if (j + 1u >= d->n) j = d->n - 2u;
    while (j > 0 && d->t[j] > t) j--;
    while (j + 2u < d->n && d->t[j + 1u] < t) j++;
    *hint = j;
    double t0 = d->t[j], t1 = d->t[j + 1u];
    return col[j] + (t - t0) * (col[j + 1u] - col[j]) / (t1 - t0);
}

static int cmd_g2_lorenz(const char *root, int nodes, int control, FILE *o)
{
    const char *fig = nodes == 43 ? "lorenz.fig43" : nodes == 25 ? "lorenz.fig25" : "lorenz.fig13";
    const char *nrk = nodes == 43 ? "lorenz.nrmse43" : nodes == 25 ? "lorenz.nrmse25" : "lorenz.nrmse13";
    static ResMatFile fd1, fd2, ff, fn;
    if (res_load_pinned(root, "lorenz.data1", &fd1) || res_load_pinned(root, "lorenz.data2", &fd2) ||
        res_load_pinned(root, fig, &ff) || res_load_pinned(root, nrk, &fn))
        return 2;
    LorenzData D[2];
    if (lorenz_data(&fd1, &D[0]) || lorenz_data(&fd2, &D[1])) {
        fprintf(stderr, "res_qualify: Lorenz data malformed\n");
        return 2;
    }
    static const char *tsn[2] = {"timeSolutionTrain", "timeSolutionTest"};
    static const char *pn[2][3] = {{"predictedX", "predictedY", "predictedZ"},
                                   {"predictedXTest", "predictedYTest", "predictedZTest"}};
    static const char *nrn[2] = {"nrmseTraining", "nrmseTest"}, *yn[2] = {"Ytrain", "Ytest"};
    const double *ts[2], *pa[2][3], *nref[2], *nref2[2], *yarch[2];
    uint32_t nts[2];
    for (int sp = 0; sp < 2; sp++) {
        const ResMatArray *a = res_mat_find(&ff, tsn[sp]);
        if (!a || res_mat_values(a, 0, &ts[sp])) return 2;
        nts[sp] = (uint32_t)a->numel;
        for (int c = 0; c < 3; c++)
            if (res_mat_values(res_mat_find(&ff, pn[sp][c]), nts[sp], &pa[sp][c])) return 2;
        if (res_mat_values(res_mat_find(&ff, nrn[sp]), 3, &nref[sp]) ||
            res_mat_values(res_mat_find(&fn, nrn[sp]), 3, &nref2[sp]) ||
            res_mat_values(res_mat_find(&ff, yn[sp]), (size_t)D[sp].n * 3u, &yarch[sp]))
            return 2;
    }
    OmegaResTopology topo;
    OmegaResParams par;
    OmegaResIntegrator integ = {STEP_S, STEPS_PER_KNOT};
    uint32_t inp[3], g;
    ResSim *sim = calloc(1, sizeof *sim);
    if (!sim) return 2;
    OmegaResStatus st = res_net_topology((ResNet)nodes, &topo, inp, &g);
    if (st == OMEGA_RES_OK) st = omega_res_params_reference(OMEGA_RES_REF_LORENZ, (uint32_t)nodes, &par);
    if (control) par.eps_f_per_m = 8.85e-12f;
    if (st == OMEGA_RES_OK) st = omega_res_config_build(&topo, &par, &integ, &sim->cfg);
    if (st != OMEGA_RES_OK) {
        fprintf(stderr, "res_qualify: config refused: %s\n", res_status_name(st));
        return 2;
    }
    const uint32_t L = (uint32_t)nodes, KN = 400001u;
    const uint64_t last = (uint64_t)(KN - 1u) * STEPS_PER_KNOT;
    float *tab = calloc((size_t)KN * L, sizeof(float));
    double *V[2], *T[2], *cur = malloc((size_t)(D[0].n > D[1].n ? D[0].n : D[1].n) * sizeof(double));
    if (!tab || !cur) return 2;
    double max_err[3] = {0, 0, 0}, max_i[3] = {0, 0, 0};
    int yeq[2], t_ok = 1;
    OmegaResRunReceipt rc[2];
    OmegaResStatus rst[2];
    OmegaResMembraneStats ms[2];
    for (int sp = 0; sp < 2; sp++) {
        const LorenzData *d = &D[sp];
        if (d->t[0] != 0.0 || d->t[d->n - 1u] != 40.0) t_ok = 0;
        yeq[sp] = memcmp(yarch[sp], d->X, (size_t)d->n * 3u * sizeof(double)) == 0;
        for (int c = 0; c < 3; c++) {
            for (uint32_t j = 0; j < d->n; j++) cur[j] = lorenz_current(d, j, c);
            uint32_t hint = 0;
            for (uint32_t k = 0; k < KN; k++) {
                double tk = (double)k / 10000.0, I = interp_col(d, cur, tk, &hint);
                float f = (float)I;
                tab[(size_t)k * L + inp[c]] = f;
                double er = (double)f - I, ai = I < 0 ? -I : I;
                if (er < 0) er = -er;
                if (er > max_err[c]) max_err[c] = er;
                if (ai > max_i[c]) max_i[c] = ai;
            }
            for (uint32_t j = 0; j < d->n; j++) { /* the Omega input at each data breakpoint */
                double x = d->t[j] * 10000.0;
                uint32_t kk = (uint32_t)x;
                if (kk >= KN - 1u) kk = KN - 2u;
                double f = d->t[j] * 10000.0 - (double)kk;
                double a = tab[(size_t)kk * L + inp[c]], b = tab[(size_t)(kk + 1u) * L + inp[c]];
                double er = (a + f * (b - a)) - cur[j];
                if (er < 0) er = -er;
                if (er > max_err[c]) max_err[c] = er;
            }
        }
        OmegaResInput in = {L, KN, tab};
        double *pos = malloc((size_t)nts[sp] * sizeof(double));
        V[sp] = malloc((size_t)nts[sp] * L * sizeof(double));
        T[sp] = malloc((size_t)nts[sp] * 3u * sizeof(double));
        if (!pos || !V[sp] || !T[sp]) return 2;
        for (uint32_t k = 0; k < nts[sp]; k++) pos[k] = ts[sp][k] * 1e5;
        omega_res_membrane_stats_init(&sim->mstats);
        rst[sp] = omega_res_state_init(&sim->cfg, OMEGA_RES_INIT_CONST, 1.0f, &sim->s);
        if (rst[sp] == OMEGA_RES_OK) rst[sp] = res_sim_at_positions(sim, &in, pos, nts[sp], last, V[sp]);
        if (rst[sp] == OMEGA_RES_OK && sim->s.step < last)
            rst[sp] = omega_res_advance_observed(&sim->cfg, &in, &sim->ws, &sim->s, last - sim->s.step, &sim->mstats);
        if (rst[sp] == OMEGA_RES_OK) rst[sp] = omega_res_run_receipt(&sim->cfg, &in, &sim->s, &rc[sp]);
        ms[sp] = sim->mstats;
        for (int c = 0; c < 3; c++) { /* targets: interp1(t, X, timeSolution) */
            uint32_t hint = 0;
            for (uint32_t k = 0; k < nts[sp]; k++)
                T[sp][(size_t)k * 3u + c] = interp_col(d, d->X + (size_t)d->n * c, ts[sp][k], &hint);
        }
        for (uint32_t k = 0; k < nts[sp]; k++) V[sp][(size_t)k * L + g] = 0.0; /* ground pinned, as L:98 */
        free(pos);
        for (uint32_t k = 0; k < KN; k++)
            for (int c = 0; c < 3; c++) tab[(size_t)k * L + inp[c]] = 0.0f;
    }
    int all_pass = rst[0] == OMEGA_RES_OK && rst[1] == OMEGA_RES_OK && t_ok && yeq[0] && yeq[1];
    /* range scaling with training min/max (L:344-360) */
    double mn[64], rg[64];
    const uint32_t n0 = nts[0];
    for (uint32_t i = 0; i < L; i++) {
        double lo = V[0][i], hi = V[0][i];
        for (uint32_t k = 1; k < n0; k++) {
            double v = V[0][(size_t)k * L + i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        mn[i] = lo;
        rg[i] = hi - lo == 0.0 ? 1.0 : hi - lo;
    }
    for (int sp = 0; sp < 2; sp++)
        for (size_t k = 0; k < (size_t)nts[sp] * L; k++) V[sp][k] = (V[sp][k] - mn[k % L]) / rg[k % L];
    /* lambda by 5-fold blocked CV over linspace(0.033, 0.035, 101) (L:121-123, 371-405) */
    static OmegaResReadoutWorkspace rws;
    static OmegaResReadout ro;
    double *xb = malloc((size_t)n0 * L * sizeof(double)), *yb = malloc((size_t)n0 * 3u * sizeof(double));
    double *pv = malloc((size_t)n0 * 3u * sizeof(double));
    if (!xb || !yb || !pv) return 2;
    uint32_t edges[6];
    for (uint32_t k = 0; k <= 5; k++) edges[k] = (uint32_t)(((uint64_t)2 * k * n0 + 5u) / 10u);
    double best = 1.0 / 0.0, best_l = 0.0;
    int best_i = -1, cv_refusals = 0;
    double t_cv0 = res_now_s();
    for (int li = 0; li <= 100 && rst[0] == OMEGA_RES_OK; li++) {
        double lam = 0.033 + (double)li * ((0.035 - 0.033) / 100.0), score = 0.0;
        int ok = 1;
        for (uint32_t f = 0; f < 5 && ok; f++) {
            uint32_t m = 0;
            for (uint32_t k = 0; k < n0; k++) {
                if (k >= edges[f] && k < edges[f + 1u]) continue;
                memcpy(xb + (size_t)m * L, V[0] + (size_t)k * L, L * sizeof(double));
                memcpy(yb + (size_t)m * 3u, T[0] + (size_t)k * 3u, 3u * sizeof(double));
                m++;
            }
            if (omega_res_readout_fit(xb, yb, m, L, 3, lam, &rws, &ro) != OMEGA_RES_OK) {
                ok = 0;
                cv_refusals++;
                break;
            }
            uint32_t nv = edges[f + 1u] - edges[f];
            for (uint32_t k = 0; k < nv; k++)
                omega_res_readout_apply(&ro, V[0] + (size_t)(edges[f] + k) * L, pv + (size_t)k * 3u);
            double s = 0.0;
            for (int c = 0; c < 3; c++) s += res_nrmse(T[0] + (size_t)edges[f] * 3u + c, pv + c, nv, 3);
            score += s / 3.0;
        }
        if (!ok) continue;
        score /= 5.0;
        if (score < best) {
            best = score;
            best_l = lam;
            best_i = li;
        }
    }
    double t_cv = res_now_s() - t_cv0;
    st = best_i >= 0 ? omega_res_readout_fit(V[0], T[0], n0, L, 3, best_l, &rws, &ro) : OMEGA_RES_ERR_SINGULAR;
    if (st != OMEGA_RES_OK) all_pass = 0;

    fprintf(o, "{\n  \"schema\": \"omega-reservoir-g2/1\",\n  \"case\": \"G2-L1\",\n  \"nodes\": %d,\n", nodes);
    fprintf(o, "  \"negative_control\": %s,\n", control ? "\"NC-L: NARMA permittivity 8.85e-12\"" : "false");
    fprintf(o, "  \"topology\": {\"edges\": %u, \"ground_1based\": %u, \"inputs_1based\": [%u, %u, %u]},\n",
            topo.edge_count, g + 1u, inp[0] + 1u, inp[1] + 1u, inp[2] + 1u);
    fprintf(o, "  \"step_s\": %.9g, \"steps_per_knot\": %u, \"permittivity_f_per_m\": %.9g,\n", (double)STEP_S,
            STEPS_PER_KNOT, (double)par.eps_f_per_m);
    fprintf(o, "  \"archive_checks\": {\"t_from_0_to_40\": %s, \"Ytrain_equals_LorenzData1_X\": %s, "
               "\"Ytest_equals_LorenzData2_X\": %s, \"figure_nrmse_equals_nrmse_file\": %s},\n",
            t_ok ? "true" : "false", yeq[0] ? "true" : "false", yeq[1] ? "true" : "false",
            memcmp(nref[0], nref2[0], 24) == 0 && memcmp(nref[1], nref2[1], 24) == 0 ? "true" : "false");
    fprintf(o, "  \"inputs\": {\"knots\": %u, \"knot_spacing_s\": 1e-4, \"resampling_and_rounding_max_abs_a\": [", KN);
    for (int c = 0; c < 3; c++) {
        fputs(c ? ", " : "", o);
        jd(o, max_err[c]);
    }
    fputs("], \"relative_to_max_abs_current\": [", o);
    for (int c = 0; c < 3; c++) {
        fputs(c ? ", " : "", o);
        jd(o, max_err[c] / max_i[c]);
    }
    fputs("]},\n  \"runs\": [", o);
    receipt_json(o, "train", rst[0], &rc[0], &ms[0]);
    fputs(", ", o);
    receipt_json(o, "test", rst[1], &rc[1], &ms[1]);
    fprintf(o, "],\n  \"samples\": [%u, %u],\n  \"cv\": {\"grid\": \"linspace(0.033, 0.035, 101)\", \"folds\": 5, "
               "\"chosen_lambda\": ", nts[0], nts[1]);
    jd(o, best_l);
    fprintf(o, ", \"chosen_index\": %d, \"score\": ", best_i);
    jd(o, best);
    fprintf(o, ", \"refusals\": %d, \"seconds\": %.1f},\n  \"final_fit_status\": \"%s\",\n", cv_refusals, t_cv,
            res_status_name(st));
    fprintf(o, "  \"tolerances\": {\"prediction_rms_over_target_std\": %g, \"nrmse_abs\": %g, \"nrmse_rel\": %g, "
               "\"metric_recompute_rel\": %g},\n",
            TOL_PRED_REL, TOL_NRMSE_ABS, TOL_NRMSE_REL, TOL_METRIC_REL);
    fputs("  \"splits\": [", o);
    static const char *vn[3] = {"x", "y", "z"};
    for (int sp = 0; sp < 2; sp++) {
        double *P = malloc((size_t)nts[sp] * 3u * sizeof(double));
        if (!P) return 2;
        for (uint32_t k = 0; k < nts[sp]; k++) {
            if (st != OMEGA_RES_OK ||
                omega_res_readout_apply(&ro, V[sp] + (size_t)k * L, P + (size_t)k * 3u) != OMEGA_RES_OK)
                P[k * 3u] = P[k * 3u + 1u] = P[k * 3u + 2u] = 0.0 / 0.0;
        }
        fprintf(o, "%s\n    {\"split\": \"%s\", \"variables\": [", sp ? "," : "", sp ? "test" : "train");
        for (int c = 0; c < 3; c++) {
            double *pac = malloc((size_t)nts[sp] * 3u * sizeof(double));
            if (!pac) return 2;
            for (uint32_t k = 0; k < nts[sp]; k++) pac[(size_t)k * 3u + c] = pa[sp][c][k];
            double sd = res_std_pop(T[sp] + c, nts[sp], 3), prel = res_rms_diff(P + c, pac + c, nts[sp], 3) / sd;
            double nr = res_nrmse(T[sp] + c, P + c, nts[sp], 3), narch = res_nrmse(T[sp] + c, pac + c, nts[sp], 3);
            int b = prel <= TOL_PRED_REL, cc = nrmse_ok(nr, nref[sp][c]), pm = rel_ok(narch, nref[sp][c], TOL_METRIC_REL);
            all_pass &= b && cc && pm;
            fprintf(o, "%s{\"var\": \"%s\", \"prediction_rms_diff_over_target_std\": ", c ? ", " : "", vn[c]);
            jd(o, prel);
            fputs(", \"nrmse_omega\": ", o);
            jd(o, nr);
            fputs(", \"nrmse_reference\": ", o);
            jd(o, nref[sp][c]);
            fputs(", \"nrmse_recomputed_from_archived_predictions\": ", o);
            jd(o, narch);
            fprintf(o, ", \"b\": %s, \"c\": %s, \"metric_control\": %s}", b ? "true" : "false", cc ? "true" : "false",
                    pm ? "true" : "false");
            free(pac);
        }
        fputs("]}", o);
        free(P);
    }
    fprintf(o, "\n  ],\n  \"verdict\": \"%s\"\n}\n", all_pass ? "PASS" : "FAIL");
    return all_pass ? 0 : 1;
}

/* -------------------------------------------------------------- config */

static int cmd_config(FILE *o)
{
    fprintf(o, "{\n  \"schema\": \"omega-reservoir-gate2-config/1\",\n"
               "  \"doc\": \"docs/reservoir/RESERVOIR_REFERENCE.md sections 7, 7.1, 10, 10.1, 12\",\n"
               "  \"reference_record\": \"10.5281/zenodo.17296788\",\n"
               "  \"reference_zip_sha256\": \"54582293dff8e9327b9187d3e5526b0e0cdcc41ca7ac87c762850bdb342dea15\",\n"
               "  \"simulator\": {\"step_s\": %.9g, \"steps_per_knot\": %u, \"numeric_policy\": ",
            (double)STEP_S, STEPS_PER_KNOT);
    res_json_str(o, OMEGA_RES_NUMERIC_POLICY);
    fprintf(o, ", \"refused_membrane_abs_v_above\": %.9g, \"implausible_membrane_abs_v_above\": %.9g, "
               "\"node_voltage_bound\": \"finite only\"",
            (double)OMEGA_RES_VM_LIMIT, (double)OMEGA_RES_VM_PLAUSIBLE);
    fprintf(o, "},\n  \"g2_narma\": {\"grid_cell\": {\"nodes\": 5, \"current_hi_a\": 1e-10, \"mask_length\": 2}, "
               "\"variants\": [\"N1a\", \"N1b\", \"N1b-v2\"], \"v2_results_sha256\": \"317af7a76051e6c30aa0476a612c29e417b199c9ef226f4f00d7f7505512fc92\", \"negative_control\": \"NC-N\", \"fit_rows_1based\": [100, 995], "
               "\"orders\": [2, 10]},\n");
    fprintf(o, "  \"g2_lorenz\": {\"nodes\": [13, 25, 43], \"negative_control\": \"NC-L (13 nodes)\", "
               "\"knots\": 400001, \"lambda_grid\": \"linspace(0.033, 0.035, 101)\", \"folds\": 5},\n");
    fprintf(o, "  \"tolerances\": {\"target_max_abs\": %g, \"prediction_rms_over_target_std\": %g, \"nrmse_abs\": %g, "
               "\"nrmse_rel\": %g, \"metric_recompute_rel\": %g},\n",
            TOL_TARGET_ABS, TOL_PRED_REL, TOL_NRMSE_ABS, TOL_NRMSE_REL, TOL_METRIC_REL);
    fprintf(o, "  \"benchmark\": {\"seeds\": [1, 2, 3, 4, 5], \"generator\": \"splitmix64\", "
               "\"lambda_grid\": [1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1, 10, 100], \"folds\": 5,\n"
               "    \"narma\": {\"orders\": [2, 5, 10], \"train\": 2000, \"test\": 1000, \"washout\": 100, "
               "\"mask_length\": 3, \"current_hi_a\": 1e-10, \"networks\": [\"Length50 chain (declared)\", \"Length5 chain (post hoc, section 10.1)\"], \"delay_line\": 12},\n"
               "    \"lorenz\": {\"dt\": 1e-4, \"transient_s\": 20, \"record_s\": 20, \"sample_s\": 0.01, "
               "\"washout_samples\": 200, \"rows\": [200, 1975], \"horizons_samples\": [0, 1, 25], "
               "\"network\": \"hex43\", \"delay_line\": 5}},\n");
    fprintf(o, "  \"performance\": {\"warmup_steps\": 10000, \"timed_steps\": 40000, \"repetitions\": 5, "
               "\"networks\": [\"chain5 NARMA\", \"chain50 NARMA\", \"hex43 Lorenz\"]},\n"
               "  \"energy\": \"NOT_RUN (not instrumented)\"\n}\n");
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: res_qualify pins|g2-narma|g2-lorenz|bench-narma|bench-lorenz|perf|config ...\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "pins") && argc == 3) return cmd_pins(argv[2], stdout);
    if (!strcmp(c, "g2-narma") && (argc == 4 || argc == 5))
        return cmd_g2_narma(argv[2], argv[3], argc == 5 ? argv[4] : NULL, stdout);
    if (!strcmp(c, "g2-lorenz") && (argc == 4 || argc == 5)) {
        int n = atoi(argv[3]);
        if (n != 13 && n != 25 && n != 43) return usage();
        if (argc == 5 && strcmp(argv[4], "NC-L")) return usage();
        return cmd_g2_lorenz(argv[2], n, argc == 5, stdout);
    }
    if (!strcmp(c, "bench-narma")) return res_bench_narma(argc - 2, argv + 2, stdout);
    if (!strcmp(c, "bench-lorenz")) return res_bench_lorenz(argc - 2, argv + 2, stdout);
    if (!strcmp(c, "perf") && argc == 2) return res_perf(stdout);
    if (!strcmp(c, "config") && argc == 2) return cmd_config(stdout);
    return usage();
}
