/* EXP-001 power simulation (Turing-profile-v1.0, lane A). C only, CPU only.
 *
 *   power-simulation MANIFEST ROOT
 *
 * Question: how many sealed seeds (independent crumbline control worlds) must one
 * replication group hold so that the preregistered 95% per-crumb bootstrap
 * interval of T(M_candidate vs B2) excludes 0 on the positive side AND the
 * interval of T(M_mem vs B2) lies below 0, with probability >= 0.95, at the
 * smallest effect declared meaningful (calibration/preregistration/EXP-001.md)?
 *
 * Stand-in data (development only, never sealed): models are refit on dev seeds
 * 1-6 with the profile fit rules (B2: mask 4 MDL prune; M_candidate: mask 93 MDL
 * prune; M_mem: mask TY_F_POS keep-all; M_mem_seed1: mask TY_F_POS keep-all on
 * seed 1 alone, rows sharpened) and scored on dev seed 7 crumb by crumb. Context
 * resets at every crumb start, so per-crumb code lengths add exactly to the
 * whole-file code length (checked below).
 *
 * Model costs charged in T are the FROZEN seeds-1-7 L(M) values (what the sealed
 * run charges), not the 1-6 refits.
 *
 * Simulated sealed group of n seeds = 185 * n crumbs drawn with replacement from
 * seed 7's crumbs (assumption A1: sealed crumbs are exchangeable with dev crumbs;
 * A2: crumbs are independent, the unit of correlation; A3: seed-to-seed variation
 * beyond crumb sampling is small, checked against the LOSO spread below).
 *
 * Effect sizes: per-crumb gain g_c = L(D_c|B2) - L(D_c|M_candidate).
 *   primary (shift):   g_c(f) = g_c - (1 - f) * gbar * n_c, gbar = sum g / sum n.
 *                      Mean gain per event becomes f * gbar; spread kept as observed.
 *   secondary (scale): g_c(f) = f * g_c (also shrinks the spread; optimistic).
 *   sensitivity (inflated): mean f * gbar per event, deviations from the mean
 *                      multiplied by PS_INFL = 2.04 (seed-level spread the crumb
 *                      bootstrap misses, from the A3 check).
 * T(f) = sum over crumbs of g_c(f) - (L(M_candidate) - L(B2)).
 *
 * Interval: the preregistered percentile bootstrap (UNCERTAINTY_PROTOCOL.md),
 * here with BOOT resamples per simulated world and WORLDS worlds per cell.
 * RNG: splitmix64, index draw by rejection (same as the protocol).
 */
#include "turing_cal_dev.h"

#include <math.h>
#include <time.h>

#define PS_SEED 0x4558503030315053ULL /* "EXP001PS" */
#define PS_WORLDS 500
#define PS_BOOT 1000
#define PS_NMAX 10
#define PS_INFL 2.04 /* 1 / 0.49: LOSO spread over one-seed crumb-bootstrap spread (A3 check) */
#define PS_CRUMBS_PER_SEED 185 /* dev seed 7 nonempty crumbs; dev seeds 1-7 range 181-188 */

/* Frozen seeds-1-7 model costs (tools/turing_cal_candidates.c output). */
#define LM_B2 1156LL
#define LM_MC 180834LL
#define LM_MEM 607838516LL
#define LM_MEMS1 356335496LL

static const double FRAC[] = {1.0, 0.5, 0.25, 0.1, 0.05, 0.02, 0.01, 0.001};
#define NFRAC (sizeof FRAC / sizeof FRAC[0])
#define F_MIN_IDX 3 /* declared smallest meaningful effect: f = 0.1 */

static uint64_t sm_state;
static uint64_t sm_next(void) {
    uint64_t z = (sm_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
/* Uniform index in [0, n): reject r >= floor(2^64 / n) * n, then r mod n. */
static uint64_t sm_index(uint64_t n) {
    uint64_t lim = UINT64_MAX - (UINT64_MAX % n + 1) % n; /* largest multiple of n, minus 1 */
    uint64_t r;
    do r = sm_next();
    while (r > lim);
    return r % n;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    size_t nc;
    double *g_mc;   /* bits: L(D_c|B2) - L(D_c|M_candidate) */
    double *g_mem;  /* bits: L(D_c|B2) - L(D_c|M_mem) */
    double *g_ms1;  /* bits: L(D_c|B2) - L(D_c|M_mem_seed1) */
    double *n;      /* events in crumb */
} crumbs;

static int fit(unsigned mask, int rule, const ty_stream *const *st, size_t ns, ty_model *m) {
    return ty_model_fit(m, st, ns, mask, TY_OUT_K, rule);
}

/* Round trip through the model code so the scored model is exactly the code. */
static int roundtrip(ty_model *m) {
    uint8_t *code;
    size_t nb;
    uint64_t bits;
    int rc = ty_model_encode(m, &code, &nb, &bits);
    if (rc != TY_OK) return rc;
    ty_model_free(m);
    rc = ty_model_decode(code, nb, m, &bits, NULL, 0);
    free(code);
    return rc;
}

static int score_crumbs(const ty_model *m, const ty_stream *s, int64_t *per, size_t nc, int64_t *total) {
    size_t c = 0, start = 0;
    int64_t sum = 0;
    for (size_t t = 1; t <= s->n; ++t) {
        if (t == s->n || s->ev[t].first) {
            if (c >= nc) return TY_E_RANGE;
            ty_stream v = {s->ev + start, t - start, t - start, 1, 0};
            int rc = ty_model_score(m, &v, &per[c], NULL);
            if (rc != TY_OK) return rc;
            sum += per[c];
            ++c;
            start = t;
        }
    }
    int64_t whole;
    int rc = ty_model_score(m, s, &whole, NULL);
    if (rc != TY_OK) return rc;
    if (whole != sum || c != nc) return TY_E_FORMAT; /* additivity over crumbs must be exact */
    *total = whole;
    return TY_OK;
}

/* lo/hi percentile indices of the preregistered rule for B sorted values. */
static void ci_idx(size_t B, size_t *lo, size_t *hi) {
    *lo = (B * 25) / 1000;
    *hi = B - 1 - (B * 25) / 1000;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: power-simulation MANIFEST ROOT\n");
        return 2;
    }
    time_t t0 = time(NULL);
    int rc = tcd_load_manifest(argv[1], argv[2]);
    if (rc != TY_OK) return fprintf(stderr, "manifest: %s\n", ty_err_name(rc)), 2;
    for (int s = 1; s <= TCD_NDEV; ++s)
        if ((rc = tcd_load_seed(s)) != TY_OK) return fprintf(stderr, "seed %d: %s\n", s, ty_err_name(rc)), 2;
    const ty_stream *fit6[6];
    for (int i = 0; i < 6; ++i) fit6[i] = &TCD_S[i];
    const ty_stream *s1 = &TCD_S[0];
    const ty_stream *v7 = &TCD_S[6];
    ty_model mb, mc, mm, ms;
    if ((rc = fit(TY_F_PREV1, TY_FIT_MDL_PRUNE, fit6, 6, &mb)) || (rc = roundtrip(&mb)) ||
        (rc = fit(TY_F_OP | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4, TY_FIT_MDL_PRUNE, fit6, 6, &mc)) ||
        (rc = roundtrip(&mc)) || (rc = fit(TY_F_POS, TY_FIT_KEEP_ALL, fit6, 6, &mm)) || (rc = roundtrip(&mm)) ||
        (rc = fit(TY_F_POS, TY_FIT_KEEP_ALL, &s1, 1, &ms)))
        return fprintf(stderr, "fit: %s\n", ty_err_name(rc)), 2;
    for (size_t i = 0; i < ms.nrows; ++i) { /* sharpen: same rule as M_mem_seed1 */
        unsigned best = 0;
        for (unsigned x = 1; x < ms.K; ++x)
            if (ms.rows[i].q[x] > ms.rows[i].q[best]) best = x;
        for (unsigned x = 0; x < ms.K; ++x) ms.rows[i].q[x] = x == best ? TY_QONE - (ms.K - 1) : 1;
    }
    if ((rc = roundtrip(&ms)) != TY_OK) return fprintf(stderr, "roundtrip: %s\n", ty_err_name(rc)), 2;

    size_t nc = v7->ncrumb;
    int64_t *pb = calloc(nc, 8), *pc = calloc(nc, 8), *pm = calloc(nc, 8), *ps = calloc(nc, 8);
    crumbs C = {nc, calloc(nc, 8), calloc(nc, 8), calloc(nc, 8), calloc(nc, 8)};
    if (!pb || !pc || !pm || !ps || !C.g_mc || !C.g_mem || !C.g_ms1 || !C.n) return 2;
    int64_t tb, tc, tm, ts;
    if ((rc = score_crumbs(&mb, v7, pb, nc, &tb)) || (rc = score_crumbs(&mc, v7, pc, nc, &tc)) ||
        (rc = score_crumbs(&mm, v7, pm, nc, &tm)) || (rc = score_crumbs(&ms, v7, ps, nc, &ts)))
        return fprintf(stderr, "score: %s\n", ty_err_name(rc)), 2;
    double sum_g = 0, sum_n = 0;
    {
        size_t c = 0, start = 0;
        for (size_t t = 1; t <= v7->n; ++t)
            if (t == v7->n || v7->ev[t].first) {
                C.n[c] = (double)(t - start);
                start = t;
                ++c;
            }
    }
    for (size_t c = 0; c < nc; ++c) {
        C.g_mc[c] = (double)(pb[c] - pc[c]) / 1e6;
        C.g_mem[c] = (double)(pb[c] - pm[c]) / 1e6;
        C.g_ms1[c] = (double)(pb[c] - ps[c]) / 1e6;
        sum_g += C.g_mc[c];
        sum_n += C.n[c];
    }
    const double gbar = sum_g / sum_n;
    const double dl_mc = (double)(LM_MC - LM_B2), dl_mem = (double)(LM_MEM - LM_B2), dl_ms1 = (double)(LM_MEMS1 - LM_B2);
    printf("# EXP-001 power simulation (Turing-profile-v1.0). Stand-in: fit dev seeds 1-6, score dev seed 7 per crumb.\n");
    printf("seed7_crumbs=%zu seed7_events=%.0f\n", nc, sum_n);
    printf("seed7_bits B2=%.3f M_candidate=%.3f M_mem=%.3f M_mem_seed1=%.3f\n", tb / 1e6, tc / 1e6, tm / 1e6, ts / 1e6);
    printf("fit1-6 L(M) bits: B2 recomputed below is not charged; charged (frozen 1-7): B2=%lld M_candidate=%lld M_mem=%lld "
           "M_mem_seed1=%lld\n",
           (long long)LM_B2, (long long)LM_MC, (long long)LM_MEM, (long long)LM_MEMS1);
    printf("observed gain per event gbar=%.6f bits/event; data gain on seed 7=%.3f bits; T_seed7(M_candidate)=%.3f bits\n", gbar,
           sum_g, sum_g - dl_mc);
    double sgm = 0, sgs = 0;
    for (size_t c = 0; c < nc; ++c) sgm += C.g_mem[c], sgs += C.g_ms1[c];
    printf("data gain on seed 7: M_mem=%.3f bits (T=%.3f), M_mem_seed1=%.3f bits (T=%.3f)\n", sgm, sgm - dl_mem, sgs,
           sgs - dl_ms1);

    /* A3 check: per-seed bootstrap sd of T per event (185 crumbs) vs LOSO spread of T per event (0.3788-0.3870,
     * sd 0.00316 bits/event over seeds 1-7, TURING_YIELD_PROFILE_V0.md section 9). */
    sm_state = PS_SEED;
    {
        double s1s = 0, s2s = 0;
        const int B = 2000;
        for (int b = 0; b < B; ++b) {
            double g = 0, n = 0;
            for (size_t k = 0; k < nc; ++k) {
                uint64_t j = sm_index(nc);
                g += C.g_mc[j];
                n += C.n[j];
            }
            double r = g / n;
            s1s += r;
            s2s += r * r;
        }
        double m = s1s / B, sd = sqrt(s2s / B - m * m);
        printf("A3 check: bootstrap sd of gain/event for one seed = %.5f bits/event; LOSO sd across dev seeds = 0.00316 "
               "bits/event (ratio %.2f)\n",
               sd, sd / 0.00316);
    }

    /* Main simulation. */
    size_t lo, hi;
    ci_idx(PS_BOOT, &lo, &hi);
    static double tv[NFRAC][PS_BOOT], tvs[NFRAC][PS_BOOT], tvi[NFRAC][PS_BOOT], tmem[PS_BOOT], tms1[PS_BOOT];
    int pw_shift[NFRAC][PS_NMAX + 1], pw_infl[NFRAC][PS_NMAX + 1], pw_scale[NFRAC][PS_NMAX + 1], pos_shift[NFRAC][PS_NMAX + 1];
    int memok[PS_NMAX + 1], ms1ok[PS_NMAX + 1];
    /* Protocol outputs (lab protocol, power simulation): false-positive rate at true T = 0, false-negative and
     * INCONCLUSIVE rates and mean 95% interval width at the declared f_min. Computed from the same draws, so the
     * numbers above are unchanged. */
    static double tv0[PS_BOOT];
    int fp0[PS_NMAX + 1], fn_min[PS_NMAX + 1], inc_min[PS_NMAX + 1];
    double width_min[PS_NMAX + 1], f0n[PS_NMAX + 1];
    memset(fp0, 0, sizeof fp0);
    memset(fn_min, 0, sizeof fn_min);
    memset(inc_min, 0, sizeof inc_min);
    memset(width_min, 0, sizeof width_min);
    memset(pw_shift, 0, sizeof pw_shift);
    memset(pw_scale, 0, sizeof pw_scale);
    memset(pw_infl, 0, sizeof pw_infl);
    memset(pos_shift, 0, sizeof pos_shift);
    memset(memok, 0, sizeof memok);
    memset(ms1ok, 0, sizeof ms1ok);
    size_t *world = malloc(sizeof(size_t) * PS_CRUMBS_PER_SEED * PS_NMAX);
    if (!world) return 2;
    for (int ns = 1; ns <= PS_NMAX; ++ns) {
        size_t W = (size_t)PS_CRUMBS_PER_SEED * (size_t)ns;
        f0n[ns] = dl_mc / (gbar * (sum_n / (double)nc) * (double)W); /* effect fraction with expected T = 0 */
        for (int w = 0; w < PS_WORLDS; ++w) {
            for (size_t k = 0; k < W; ++k) world[k] = sm_index(nc);
            for (int b = 0; b < PS_BOOT; ++b) {
                double g = 0, n = 0, gm = 0, gs = 0;
                for (size_t k = 0; k < W; ++k) {
                    size_t j = world[sm_index(W)];
                    g += C.g_mc[j];
                    n += C.n[j];
                    gm += C.g_mem[j];
                    gs += C.g_ms1[j];
                }
                for (size_t f = 0; f < NFRAC; ++f) {
                    tv[f][b] = g - (1.0 - FRAC[f]) * gbar * n - dl_mc;
                    tvs[f][b] = FRAC[f] * g - dl_mc;
                    tvi[f][b] = FRAC[f] * gbar * n + PS_INFL * (g - gbar * n) - dl_mc;
                }
                tv0[b] = g - (1.0 - f0n[ns]) * gbar * n - dl_mc;
                tmem[b] = gm - dl_mem;
                tms1[b] = gs - dl_ms1;
            }
            qsort(tmem, PS_BOOT, sizeof(double), cmp_d);
            qsort(tms1, PS_BOOT, sizeof(double), cmp_d);
            int mem_below = tmem[hi] < 0, ms1_below = tms1[hi] < 0;
            qsort(tv0, PS_BOOT, sizeof(double), cmp_d);
            fp0[ns] += tv0[lo] > 0;
            memok[ns] += mem_below;
            ms1ok[ns] += ms1_below;
            for (size_t f = 0; f < NFRAC; ++f) {
                qsort(tv[f], PS_BOOT, sizeof(double), cmp_d);
                qsort(tvs[f], PS_BOOT, sizeof(double), cmp_d);
                qsort(tvi[f], PS_BOOT, sizeof(double), cmp_d);
                pw_infl[f][ns] += (tvi[f][lo] > 0) && mem_below && ms1_below;
                pos_shift[f][ns] += tv[f][lo] > 0;
                pw_shift[f][ns] += (tv[f][lo] > 0) && mem_below && ms1_below;
                pw_scale[f][ns] += (tvs[f][lo] > 0) && mem_below && ms1_below;
                if (f == F_MIN_IDX) {
                    fn_min[ns] += tv[f][hi] < 0;
                    inc_min[ns] += tv[f][lo] <= 0 && tv[f][hi] >= 0;
                    width_min[ns] += tv[f][hi] - tv[f][lo];
                }
            }
        }
    }
    printf("worlds_per_cell=%d bootstrap_resamples=%d crumbs_per_seed=%d rng=splitmix64 seed=0x%016llx ci_index=[%zu,%zu]\n",
           PS_WORLDS, PS_BOOT, PS_CRUMBS_PER_SEED, (unsigned long long)PS_SEED, lo, hi);
    printf("memorizer interval below 0 (fraction of worlds) by n: ");
    for (int ns = 1; ns <= PS_NMAX; ++ns)
        printf("n%d M_mem=%.3f M_mem_seed1=%.3f ", ns, (double)memok[ns] / PS_WORLDS, (double)ms1ok[ns] / PS_WORLDS);
    printf("\n");
    printf("# power (joint criterion), primary shift model; rows = effect fraction f (gain/event = f*gbar), cols = seeds n\n");
    printf("f\tgain/event\tbreakeven_n");
    for (int ns = 1; ns <= PS_NMAX; ++ns) printf("\tn=%d", ns);
    printf("\n");
    int chosen = -1;
    for (size_t f = 0; f < NFRAC; ++f) {
        double ge = FRAC[f] * gbar;
        double be = dl_mc / (ge * (sum_n / (double)nc) * PS_CRUMBS_PER_SEED);
        printf("%.3f\t%.6f\t%.2f", FRAC[f], ge, be);
        for (int ns = 1; ns <= PS_NMAX; ++ns) printf("\t%.3f", (double)pw_shift[f][ns] / PS_WORLDS);
        printf("\n");
    }
    printf("# power (joint criterion), secondary scale model\n");
    for (size_t f = 0; f < NFRAC; ++f) {
        printf("%.3f", FRAC[f]);
        for (int ns = 1; ns <= PS_NMAX; ++ns) printf("\t%.3f", (double)pw_scale[f][ns] / PS_WORLDS);
        printf("\n");
    }
    printf("# power (joint criterion), sensitivity: shift model with per-crumb spread inflated x%.2f (A3 ratio)\n", PS_INFL);
    for (size_t f = 0; f < NFRAC; ++f) {
        printf("%.3f", FRAC[f]);
        for (int ns = 1; ns <= PS_NMAX; ++ns) printf("\t%.3f", (double)pw_infl[f][ns] / PS_WORLDS);
        printf("\n");
    }
    printf("# S6 alone, shift model, by n: false-positive rate at true T = 0 (effect fraction f0 below); at f_min: FAIL rate (hi < 0), INCONCLUSIVE rate (interval contains 0), mean 95%% interval width in bits\n");
    printf("n\tf0\tfalse_positive\tfail_at_fmin\tinconclusive_at_fmin\tmean_width_bits_at_fmin\n");
    for (int ns = 1; ns <= PS_NMAX; ++ns)
        printf("%d\t%.4f\t%.3f\t%.3f\t%.3f\t%.1f\n", ns, f0n[ns], (double)fp0[ns] / PS_WORLDS, (double)fn_min[ns] / PS_WORLDS,
               (double)inc_min[ns] / PS_WORLDS, width_min[ns] / PS_WORLDS);
    printf("simulation_code: calibration/scripts/power_simulation.c; its SHA-256 is pinned in candidate_manifest.json shared_background_sha256\n");
    for (int ns = 1; ns <= PS_NMAX; ++ns)
        if (chosen < 0 && pw_shift[F_MIN_IDX][ns] * 100 >= 95 * PS_WORLDS) chosen = ns;
    printf("declared_min_effect_f=%.3f declared_min_gain_per_event=%.6f bits\n", FRAC[F_MIN_IDX], FRAC[F_MIN_IDX] * gbar);
    if (chosen > 0)
        printf("CHOSEN_SEEDS_PER_GROUP=%d (smallest n with joint power >= 0.95 at f_min, shift model)\n", chosen);
    else
        printf("CHOSEN_SEEDS_PER_GROUP=none within n<=%d\n", PS_NMAX);
    printf("runtime_seconds=%ld\n", (long)(time(NULL) - t0));
    return 0;
}
