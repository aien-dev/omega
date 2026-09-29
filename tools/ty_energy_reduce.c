/*
 * ty_energy_reduce: TY-4/TY-5 reducer (docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md).
 *
 *   ty_energy_reduce --check  <raw-dir>                         validity + I + attribution lines
 *   ty_energy_reduce --stage1 <raw-dir> <summary.json>          sweep: contention score, selection
 *   ty_energy_reduce --stage2 <raw-dir> <summary.json> <records.jsonl>
 *                                                               checks (a), (b), attribution records
 *
 * Reuse, not copies: tools/r15_reduce.c is compiled into this translation unit
 * (its main renamed) so the bad-window rule is r15_reduce's own energy_ok()
 * (spbm_ok and every overflow flag zero) and win_pkg_uj() (end >= start, never
 * unwrapped), and the JSON parser, statistics helpers, splitmix64 generator and
 * JSON writer are r15_reduce's as well. The TY records and conservation checks
 * are src/turing/ty_energy.c.
 *
 * Deterministic: files in name order, bootstrap seed 0x15, 10,000 resamples.
 */
#define main r15_reduce_main_unused
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
/* The reused, uncalled legacy CLI main truncates its diagnostic note. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#include "r15_reduce.c"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#undef main

#include "turing/ty_energy.h"

#define NCONDS 4
static const char *const k_cond[NCONDS] = {"IDLE", "A", "B", "AB"};
enum { CH_PKG = 0, CH_CPU_E = 1, CH_CPU_P = 2, CH_REST = 3, NCHX = 4 };
static const char *const k_chx[NCHX] = {"pkg", "cpu_e", "cpu_p", "rest"};

typedef struct {
    char file[256];
    J *j;
    ty_interval w;
    ty_energy_status st;
    turing_digest d;
    int64_t e[NCHX]; /* normalised to the planned length, µJ */
    double cyc, inst, l2; /* summed over participants, raw counts */
    char spec_a[64], spec_b[64];
} Win;

static Win *g_win;
static size_t g_nwin;
__extension__ typedef unsigned __int128 tye_reduce_u128;

static int edge_shape(const J *edge) {
    J *flag = jget(edge, "spbm_ok"), *time = jget(edge, "t_ns");
    J *energy = jget(edge, "energy_uj"), *overflow = jget(edge, "overflow");
    if (!flag || flag->t != J_NUM || !flag->is_u || flag->u > 1 ||
        !time || time->t != J_NUM || !time->is_u ||
        !energy || energy->t != J_ARR || energy->n < TY_NCH ||
        !overflow || overflow->t != J_ARR || overflow->n < TY_NCH) return 0;
    for (size_t i = 0; i < energy->n; i++)
        if (energy->v[i]->t != J_NUM || !energy->v[i]->is_u) return 0;
    for (size_t i = 0; i < overflow->n; i++)
        if (overflow->v[i]->t != J_NUM || !overflow->v[i]->is_u) return 0;
    return 1;
}

/* ---- loading ---- */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    if (!b) { fclose(f); return NULL; }
    size_t k;
    for (k = fread(b, 1, cap - n - 1, f); k > 0; k = fread(b + n, 1, cap - n - 1, f)) {
        n += k;
        if (n + 1 >= cap) {
            if (cap >= 64u * 1024u * 1024u) { free(b); fclose(f); return NULL; }
            char *grown = realloc(b, cap * 2u);
            if (!grown) { free(b); fclose(f); return NULL; }
            b = grown; cap *= 2u;
        }
    }
    fclose(f);
    b[n] = 0;
    return b;
}

static void cpu_mask(const char *list, int *mask) {
    for (const char *p = list; *p;) {
        char *end;
        long c = strtol(p, &end, 10);
        if (end == p) break;
        if (c >= 0 && c < 64) mask[c] = 1;
        p = *end == ',' ? end + 1 : end;
    }
}

static void fill(Win *x) {
    J *r = x->j;
    ty_interval *w = &x->w;
    memset(w, 0, sizeof *w);
    J *raw_parts = jget(r, "participants");
    if ((raw_parts && (raw_parts->t != J_ARR || raw_parts->n > TY_MAX_PART)) ||
        strlen(js(r, "run")) >= sizeof w->run_id ||
        strlen(js(r, "config")) >= sizeof w->config ||
        strlen(js(r, "level")) >= sizeof w->cond ||
        ju(r, "round") > UINT32_MAX || ju(r, "trial") > UINT32_MAX) {
        x->st = TYE_E_ARG; return;
    }
    snprintf(w->run_id, sizeof w->run_id, "%s", js(r, "run"));
    snprintf(w->config, sizeof w->config, "%s", js(r, "config"));
    snprintf(w->cond, sizeof w->cond, "%s", js(r, "level"));
    snprintf(x->spec_a, sizeof x->spec_a, "%s", js(r, "spec_a"));
    snprintf(x->spec_b, sizeof x->spec_b, "%s", js(r, "spec_b"));
    w->round = (uint32_t)ju(r, "round");
    w->pos = (uint32_t)ju(r, "trial");
    w->planned_ns = ju(r, "planned_ns");
    J *e0 = jget(r, "e0"), *e1 = jget(r, "e1");
    double tool_cpu = jd(r, "tool_cpu");
    if (!edge_shape(e0) || !edge_shape(e1) || !isfinite(tool_cpu) || tool_cpu < 0 ||
        tool_cpu >= 64 || floor(tool_cpu) != tool_cpu) {
        w->planned_ns = 0; x->st = TYE_E_ARG; return;
    }
    int meter = jd(r, "meter_ok") == 1.0;
    w->t0_ns = ju(e0, "t_ns");
    w->t1_ns = ju(e1, "t_ns");
    /* r15_reduce's rule decides; the raw flag only names which refusal. */
    int ok0 = meter && energy_ok(e0), ok1 = meter && energy_ok(e1);
    w->spbm_ok0 = meter && jd(e0, "spbm_ok") == 1.0;
    w->spbm_ok1 = meter && jd(e1, "spbm_ok") == 1.0;
    w->overflow = (w->spbm_ok0 && !ok0) || (w->spbm_ok1 && !ok1);
    for (int c = 0; c < TY_NCH; c++) {
        w->e0_uj[c] = jau(jget(e0, "energy_uj"), (size_t)c);
        w->e1_uj[c] = jau(jget(e1, "energy_uj"), (size_t)c);
    }
    /* win_pkg_uj (r15_reduce) refuses a decreasing pkg counter; ty_interval_check
     * must agree with it. */
    if (ok0 && ok1 && isnan(win_pkg_uj(r)) && w->e1_uj[0] >= w->e0_uj[0]) {
        fprintf(stderr, "%s: r15_reduce and ty_energy disagree on the pkg rule\n", x->file);
        exit(3);
    }
    J *tel = jget(r, "tel"), *tt = jget(tel, "t_ns"), *tok = jget(tel, "ok");
    uint64_t prev = w->t0_ns, gap = 0, n = 0;
    for (size_t i = 0; tt && tt->t == J_ARR && i < tt->n; i++) {
        if (!jau(tok, i)) continue;
        uint64_t t = jau(tt, i);
        if (t > prev && t - prev > gap) gap = t - prev;
        if (t > prev) prev = t;
        n++;
    }
    if (w->t1_ns > prev && w->t1_ns - prev > gap) gap = w->t1_ns - prev;
    w->n_samples = n;
    w->max_gap_ns = gap;

    int used[64] = {0};
    used[(int)tool_cpu] = 1;
    J *parts = jget(r, "participants");
    for (size_t i = 0; parts && parts->t == J_ARR && i < parts->n && w->npart < TY_MAX_PART; i++) {
        J *p = parts->v[i], *res = jget(p, "result");
        ty_participant *q = &w->part[w->npart++];
        q->exited_ok = jd(p, "ok") == 1.0;
        if (!res || res->t != J_OBJ) continue;
        snprintf(q->tag, sizeof q->tag, "%s", js(res, "tag"));
        snprintf(q->rz, sizeof q->rz, "%s", js(res, "rz"));
        snprintf(q->cpus, sizeof q->cpus, "%s", js(res, "cpus"));
        cpu_mask(q->cpus, used);
        q->pinned = jd(res, "pinned") == 1.0;
        q->oracle_ok = jd(res, "oracle_ok") == 1.0;
        q->planned_ns = ju(res, "planned_ns");
        q->go_ns = ju(res, "go_ns");
        q->t0_ns = ju(res, "t0_ns");
        q->t1_ns = ju(res, "t1_ns");
        q->done_ns = ju(res, "done_ns");
        q->calls = ju(res, "calls");
        J *pm = jget(res, "pmu"), *sum = jget(pm, "sum"), *list = jget(pm, "pmu");
        q->pmu_ok = jd(res, "pmu_open") == 1.0 && jd(pm, "ok") == 1.0;
        q->cycles = ju(sum, "cpu_cycles");
        q->inst = ju(sum, "inst_retired");
        q->l2refill = ju(sum, "l2d_cache_refill");
        /* Multiplexing, per PMU that counted anything: running >= 99.9% of
         * enabled for every event. (r15's own flag compares the sum of running
         * over both PMUs with PMU 0's enabled time, which a multi-thread,
         * inherit=1 process trips without any multiplexing; it is not used.) */
        for (size_t k = 0; list && list->t == J_ARR && k < list->n; k++) {
            J *val = jget(list->v[k], "value"), *en = jget(list->v[k], "enabled"), *ru = jget(list->v[k], "running");
            uint64_t any = 0;
            for (size_t e = 0; e < 6; e++) any |= jau(val, e);
            for (size_t e = 0; any && e < 6; e++)
                if ((tye_reduce_u128)jau(ru, e) * 1000u < (tye_reduce_u128)jau(en, e) * 999u) q->multiplexed = 1;
        }
        x->cyc += (double)q->cycles;
        x->inst += (double)q->inst;
        x->l2 += (double)q->l2refill;
    }
    J *s0 = jget(r, "s0"), *s1 = jget(r, "s1");
    J *b0 = jget(s0, "busy_jiffies"), *b1 = jget(s1, "busy_jiffies");
    uint64_t busy = 0;
    for (size_t c = 0; b0 && b1 && b0->t == J_ARR && c < b0->n && c < 64; c++)
        if (!used[c] && jau(b1, c) >= jau(b0, c)) busy += jau(b1, c) - jau(b0, c);
    w->foreign_busy_ms = busy * 10u; /* USER_HZ = 100 */
    J *tz[2] = {jget(s0, "temp_mc"), jget(s1, "temp_mc")};
    for (int s = 0; s < 2; s++)
        for (size_t z = 0; tz[s] && tz[s]->t == J_ARR && z < tz[s]->n; z++)
            if (tz[s]->v[z]->t == J_NUM && tz[s]->v[z]->is_u && tz[s]->v[z]->u <= UINT32_MAX &&
                tz[s]->v[z]->u > w->max_temp_mc) w->max_temp_mc = (uint32_t)tz[s]->v[z]->u;

    x->st = ty_interval_check(w);
    ty_interval_digest(w, &x->d);
    for (int c = 0; c < TY_NCH; c++)
        if (ty_interval_energy(w, c, &x->e[c]) != TYE_OK) x->e[c] = 0;
    x->e[CH_REST] = x->e[CH_PKG] - x->e[CH_CPU_E] - x->e[CH_CPU_P];
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void free_json(J *j) {
    if (!j) return;
    for (size_t i = 0; i < j->n; i++) {
        if (j->v) free_json(j->v[i]);
        if (j->k) free(j->k[i]);
    }
    free(j->s); free(j->v); free(j->k); free(j);
}

static int load(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    for (de = readdir(d); de; de = readdir(d)) {
        size_t L = strlen(de->d_name);
        if (L < 7 || strcmp(de->d_name + L - 6, ".jsonl")) continue;
        if (n == cap) names = realloc(names, (cap = cap ? cap * 2 : 64) * sizeof *names);
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (!n) { free(names); return -1; }
    qsort(names, n, sizeof *names, cmp_name);
    g_win = calloc(n ? n : 1, sizeof *g_win);
    int invalid_file = 0;
    for (size_t i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        char *text = slurp(path);
        Win *x = &g_win[g_nwin];
        snprintf(x->file, sizeof x->file, "%s", names[i]);
        free(names[i]);
        if (!text) { invalid_file = 1; continue; }
        jp = text;
        x->j = jparse();
        jws();
        if (!x->j || x->j->t != J_OBJ || *jp != 0) {
            fprintf(stderr, "%s: not a JSON object\n", x->file);
            free_json(x->j); x->j = NULL; invalid_file = 1;
            free(text);
            continue;
        }
        fill(x);
        free(text);
        g_nwin++;
    }
    free(names);
    if (invalid_file) return -1;
    /* Overlap: a window that overlaps another is refused (both of them). */
    for (size_t i = 0; i < g_nwin; i++)
        for (size_t j = i + 1; j < g_nwin; j++) {
            const ty_interval *pair[2] = {&g_win[i].w, &g_win[j].w};
            if (ty_intervals_disjoint(pair, 2, NULL, NULL) == TYE_E_OVERLAP) {
                g_win[i].st = g_win[j].st = TYE_E_OVERLAP;
                printf("OVERLAP %s %s\n", g_win[i].file, g_win[j].file);
            }
        }
    return 0;
}

/* ---- statistics ---- */

static double tq975(int df) {
    static const double t[31] = {NAN,   12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                                 2.201, 2.179,  2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080,
                                 2.074, 2.069,  2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
    if (df < 1) return NAN;
    if (df <= 30) return t[df];
    return df <= 60 ? 2.042 : df <= 120 ? 2.000 : 1.96;
}

static void mean_sd(const double *v, size_t n, double *m, double *sd) {
    double s = 0, q = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    *m = n ? s / (double)n : NAN;
    for (size_t i = 0; i < n; i++) q += (v[i] - *m) * (v[i] - *m);
    *sd = n > 1 ? sqrt(q / (double)(n - 1)) : NAN;
}

/* 95% percentile bootstrap of the mean, r15_reduce's generator and seed. */
static void boot_mean(const double *v, size_t n, double *lo, double *hi) {
    *lo = *hi = NAN;
    if (n < 2) return;
    g_rng = 0x15;
    double *mu = malloc(RESAMPLES * sizeof *mu);
    for (int b = 0; b < RESAMPLES; b++) {
        double s = 0;
        for (size_t i = 0; i < n; i++) s += v[splitmix() % n];
        mu[b] = s / (double)n;
    }
    qsort(mu, RESAMPLES, sizeof *mu, cmp_d);
    *lo = mu[(size_t)(0.025 * RESAMPLES) - 1];
    *hi = mu[(size_t)(0.975 * RESAMPLES) - 1];
    free(mu);
}

/* ---- rounds ---- */

typedef struct {
    char config[16];
    uint32_t round;
    Win *c[NCONDS];
    ty_energy_status st;
    double I[NCHX];       /* J */
    double bound95;       /* analytic edge bound on I (pkg), J */
} Round;

static Round *g_round;
static size_t g_nround;

static int cond_index(const char *c) {
    for (int k = 0; k < NCONDS; k++)
        if (!strcmp(c, k_cond[k])) return k;
    return -1;
}

static void build_rounds(void) {
    g_round = calloc(g_nwin ? g_nwin : 1, sizeof *g_round);
    for (size_t i = 0; i < g_nwin; i++) {
        Win *x = &g_win[i];
        Round *r = NULL;
        for (size_t k = 0; k < g_nround && !r; k++)
            if (!strcmp(g_round[k].config, x->w.config) && g_round[k].round == x->w.round) r = &g_round[k];
        if (!r) {
            r = &g_round[g_nround++];
            snprintf(r->config, sizeof r->config, "%s", x->w.config);
            r->round = x->w.round;
        }
        int ci = cond_index(x->w.cond);
        if (ci < 0) { r->st = TYE_E_CONDITION; continue; }
        if (r->c[ci]) r->st = TYE_E_DOUBLE_COUNT; /* one condition measured twice in a round */
        else r->c[ci] = x;
    }
    for (size_t k = 0; k < g_nround; k++) {
        Round *r = &g_round[k];
        for (int c = 0; c < NCHX; c++) r->I[c] = NAN;
        r->bound95 = NAN;
        if (r->st != TYE_OK) continue;
        if (!r->c[1] || !r->c[2] || !r->c[3]) { r->st = TYE_E_CONDITION; continue; }
        int64_t I = 0;
        r->st = ty_interaction(r->c[0] ? &r->c[0]->w : NULL, &r->c[1]->w, &r->c[2]->w, &r->c[3]->w, TY_CH_PKG, &I);
        if (r->st == TYE_OK)
            for (int c = 0; c < NCONDS; c++)
                if (r->c[c]->st != TYE_OK) r->st = r->c[c]->st; /* overlap marks */
        if (r->st != TYE_OK) continue;
        double var = 0;
        for (int c = 0; c < NCHX; c++)
            r->I[c] = (double)(r->c[3]->e[c] - r->c[1]->e[c] - r->c[2]->e[c] + r->c[0]->e[c]) / 1e6;
        for (int c = 0; c < NCONDS; c++) {
            double pw = (double)r->c[c]->e[CH_PKG] / ((double)r->c[c]->w.planned_ns / 1e9) / 1e6; /* W */
            double sd_edge = pw * 0.1 / sqrt(3.0);
            var += 2 * sd_edge * sd_edge;
        }
        r->bound95 = 1.96 * sqrt(var);
    }
}

/* ---- linear fit (protocol section 11) ---- */

#define K 3
typedef struct {
    int ok, degenerate, n;
    double beta[K], inv[K][K], s, cond;
} Fit;

static int inv3(double a[K][K], double o[K][K]) {
    double d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabs(d) < 1e-300) return -1;
    o[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / d;
    o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / d;
    o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / d;
    o[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / d;
    o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / d;
    o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / d;
    o[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / d;
    o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / d;
    o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / d;
    return 0;
}

/* Eigenvalues of a symmetric 3x3 matrix, cyclic Jacobi, fixed 60 sweeps. */
static void eig3(double a[K][K], double ev[K]) {
    double m[K][K];
    memcpy(m, a, sizeof m);
    for (int it = 0; it < 60; it++)
        for (int p = 0; p < K; p++)
            for (int q = p + 1; q < K; q++) {
                if (fabs(m[p][q]) < 1e-300) continue;
                double th = (m[q][q] - m[p][p]) / (2 * m[p][q]);
                double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1)), c = 1 / sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < K; k++) {
                    double mkp = m[k][p], mkq = m[k][q];
                    m[k][p] = c * mkp - s * mkq;
                    m[k][q] = s * mkp + c * mkq;
                }
                for (int k = 0; k < K; k++) {
                    double mpk = m[p][k], mqk = m[q][k];
                    m[p][k] = c * mpk - s * mqk;
                    m[q][k] = s * mpk + c * mqk;
                }
            }
    for (int k = 0; k < K; k++) ev[k] = m[k][k];
}

/* feature 2: 0 = inst_retired, 1 = l2d_cache_refill. Units: 1e9 counts, J. */
static void features(const Win *x, int f2, double v[K]) {
    v[0] = 1.0;
    v[1] = x->cyc / 1e9;
    v[2] = (f2 ? x->l2 : x->inst) / 1e9;
}

static Fit fit(const char *config, int ch, int f2) {
    Fit F;
    memset(&F, 0, sizeof F);
    double xtx[K][K] = {{0}}, xty[K] = {0}, norm[K] = {0};
    for (size_t i = 0; i < g_nwin; i++) {
        const Win *x = &g_win[i];
        if (strcmp(x->w.config, config) || x->st != TYE_OK || !strcmp(x->w.cond, "AB")) continue;
        double v[K], y = (double)x->e[ch] / 1e6;
        features(x, f2, v);
        for (int a = 0; a < K; a++) {
            norm[a] += v[a] * v[a];
            xty[a] += v[a] * y;
            for (int b = 0; b < K; b++) xtx[a][b] += v[a] * v[b];
        }
        F.n++;
    }
    if (F.n <= K) return F;
    double sc[K][K], ev[K];
    for (int a = 0; a < K; a++)
        for (int b = 0; b < K; b++) sc[a][b] = norm[a] > 0 && norm[b] > 0 ? xtx[a][b] / sqrt(norm[a] * norm[b]) : 0;
    eig3(sc, ev);
    double lo = ev[0], hi = ev[0];
    for (int k = 1; k < K; k++) {
        if (ev[k] < lo) lo = ev[k];
        if (ev[k] > hi) hi = ev[k];
    }
    F.cond = lo > 0 ? sqrt(hi / lo) : INFINITY;
    F.degenerate = !(F.cond <= 1e4);
    if (inv3(xtx, F.inv) != 0) { F.degenerate = 1; return F; }
    for (int a = 0; a < K; a++)
        for (int b = 0; b < K; b++) F.beta[a] += F.inv[a][b] * xty[b];
    double rss = 0;
    for (size_t i = 0; i < g_nwin; i++) {
        const Win *x = &g_win[i];
        if (strcmp(x->w.config, config) || x->st != TYE_OK || !strcmp(x->w.cond, "AB")) continue;
        double v[K], y = (double)x->e[ch] / 1e6, p = 0;
        features(x, f2, v);
        for (int a = 0; a < K; a++) p += F.beta[a] * v[a];
        rss += (y - p) * (y - p);
    }
    F.s = sqrt(rss / (F.n - K));
    F.ok = 1;
    return F;
}

typedef struct {
    int n, inside, pass;
    double mean_err, lo, hi, rms, mean_rel;
} Holdout;

static Holdout holdout(const char *config, int ch, int f2, const Fit *F) {
    Holdout H;
    memset(&H, 0, sizeof H);
    H.mean_err = H.lo = H.hi = H.rms = NAN;
    if (!F->ok) return H;
    double err[256], rel[256];
    double tp = tq975(F->n - K);
    for (size_t i = 0; i < g_nwin && H.n < 256; i++) {
        const Win *x = &g_win[i];
        if (strcmp(x->w.config, config) || x->st != TYE_OK || strcmp(x->w.cond, "AB")) continue;
        double v[K], y = (double)x->e[ch] / 1e6, p = 0, h = 0;
        features(x, f2, v);
        for (int a = 0; a < K; a++) {
            p += F->beta[a] * v[a];
            for (int b = 0; b < K; b++) h += v[a] * F->inv[a][b] * v[b];
        }
        double half = tp * F->s * sqrt(1 + h);
        err[H.n] = y - p;
        rel[H.n] = (y - p) / y;
        if (fabs(y - p) <= half) H.inside++;
        H.n++;
    }
    if (H.n < 2) return H;
    double sd, sdr;
    mean_sd(err, (size_t)H.n, &H.mean_err, &sd);
    mean_sd(rel, (size_t)H.n, &H.mean_rel, &sdr);
    double half = tq975(H.n - 1) * sd / sqrt((double)H.n);
    H.lo = H.mean_err - half;
    H.hi = H.mean_err + half;
    double q = 0;
    for (int i = 0; i < H.n; i++) q += err[i] * err[i];
    H.rms = sqrt(q / H.n);
    H.pass = !F->degenerate && H.lo <= 0 && H.hi >= 0 && H.inside * 5 >= H.n * 4;
    return H;
}

/* ---- attribution ---- */

static FILE *g_rec;

static void emit_attr(const ty_attribution *at, const Round *r) {
    turing_digest d;
    char hex[65], mh[65];
    ty_attribution_digest(at, &d);
    ty_energy_hex(&d, hex);
    ty_energy_hex(&at->measured, mh);
    printf("ATTR %s r%u %s %s measured=%" PRId64 " allocated=%" PRId64 " unattributed=%" PRId64 " tol=%" PRIu64
           " overall=%s\n",
           r->config, r->round, at->method, ty_energy_status_name(at->verdict), at->measured_uj, at->allocated_uj,
           at->remainder_uj, at->tolerance_uj, ty_conf_name(at->all_unattributed ? TY_UNATTRIBUTED : at->overall));
    if (!g_rec) return;
    fprintf(g_rec,
            "{\"record\":\"%s\",\"digest\":\"%s\",\"config\":\"%s\",\"round\":%u,\"method\":\"%s\","
            "\"measured_interval\":\"%s\",\"verdict\":\"%s\",\"measured_uj\":%" PRId64 ",\"allocated_uj\":%" PRId64
            ",\"unattributed_uj\":%" PRId64 ",\"tolerance_uj\":%" PRIu64 ",\"overall\":\"%s\",\"forge\":%u,\"shares\":[",
            TY_DOMAIN_ATTRIBUTION, hex, r->config, r->round, at->method, mh, ty_energy_status_name(at->verdict),
            at->measured_uj, at->allocated_uj, at->remainder_uj, at->tolerance_uj, ty_conf_name(at->overall),
            ty_conf_forge(at->overall));
    for (size_t i = 0; i < at->nshare; i++)
        fprintf(g_rec, "%s{\"who\":\"%s\",\"uj\":%" PRId64 ",\"conf\":\"%s\",\"forge\":%u}", i ? "," : "",
                at->share[i].who, at->share[i].uj, ty_conf_name(at->share[i].conf), ty_conf_forge(at->share[i].conf));
    fputs("]}\n", g_rec);
}

static void add_share(ty_attribution *at, const char *who, int64_t uj, ty_conf c) {
    ty_share *s = &at->share[at->nshare++];
    snprintf(s->who, sizeof s->who, "%s", who);
    s->uj = uj;
    s->conf = c;
}

static uint64_t tolerance_uj(const Round *r, double sd_I) {
    double t = r->bound95;
    if (!isnan(sd_I) && 1.96 * sd_I > t) t = 1.96 * sd_I;
    return isnan(t) ? 0 : (uint64_t)llround(t * 1e6);
}

/* Solo counterfactual attribution of the AB window of round r. A missing
 * window is passed through so the check names what is missing. */
static ty_energy_status attribute_solo(const Round *r, double sd_I) {
    if (!r->c[3]) return TYE_E_CONDITION;
    ty_attribution at;
    memset(&at, 0, sizeof at);
    snprintf(at.method, sizeof at.method, "solo_counterfactual.v0");
    at.ch = TY_CH_PKG;
    at.tolerance_uj = tolerance_uj(r, sd_I);
    int64_t e0 = r->c[0] ? r->c[0]->e[CH_PKG] : 0;
    if (r->c[0]) {
        add_share(&at, TY_WHO_IDLE, e0, TY_COUNTERFACTUAL);
        at.src[at.nsrc++] = r->c[0]->d;
    }
    if (r->c[1]) { add_share(&at, "A", r->c[1]->e[CH_PKG] - e0, TY_COUNTERFACTUAL); at.src[at.nsrc++] = r->c[1]->d; }
    if (r->c[2]) { add_share(&at, "B", r->c[2]->e[CH_PKG] - e0, TY_COUNTERFACTUAL); at.src[at.nsrc++] = r->c[2]->d; }
    ty_attribution_check(&at, &r->c[3]->w);
    if (r->st != TYE_OK && at.verdict == TYE_OK) at.verdict = r->st; /* e.g. overlap found across windows */
    emit_attr(&at, r);
    return at.verdict;
}

static ty_energy_status attribute_model(const Round *r, const Fit *F, double sd_I) {
    if (!r->c[3] || !F->ok) return TYE_E_ARG;
    ty_attribution at;
    memset(&at, 0, sizeof at);
    snprintf(at.method, sizeof at.method, "counter_model.v0");
    at.ch = TY_CH_PKG;
    at.tolerance_uj = tolerance_uj(r, sd_I);
    add_share(&at, TY_WHO_IDLE, (int64_t)llround(F->beta[0] * 1e6), TY_MODELLED);
    const ty_interval *w = &r->c[3]->w;
    for (size_t i = 0; i < w->npart; i++) {
        const ty_participant *p = &w->part[i];
        double j = F->beta[1] * (double)p->cycles / 1e9 + F->beta[2] * (double)p->inst / 1e9;
        add_share(&at, p->tag, (int64_t)llround(j * 1e6), TY_COUNTER_DERIVED);
    }
    for (int c = 0; c < 3; c++)
        if (r->c[c]) at.src[at.nsrc++] = r->c[c]->d;
    ty_attribution_check(&at, w);
    emit_attr(&at, r);
    return at.verdict;
}

/* ---- modes ---- */

static void print_windows(void) {
    for (size_t i = 0; i < g_nwin; i++) {
        const Win *x = &g_win[i];
        char h[65];
        ty_energy_hex(&x->d, h);
        printf("WINDOW %s %s %s r%u %s pkg_uj=%" PRId64 " samples=%" PRIu64 " gap_ms=%" PRIu64
               " foreign_busy_ms=%" PRIu64 " digest=%.16s\n",
               x->file, x->w.config, x->w.cond, x->w.round, ty_energy_status_name(x->st), x->e[CH_PKG], x->w.n_samples,
               x->w.max_gap_ns / 1000000u, x->w.foreign_busy_ms, h);
        if (g_rec)
            fprintf(g_rec,
                    "{\"record\":\"%s\",\"digest\":\"%s\",\"file\":\"%s\",\"config\":\"%s\",\"cond\":\"%s\","
                    "\"round\":%u,\"pos\":%u,\"status\":\"%s\",\"confidence\":\"DIRECT\",\"forge\":%u,"
                    "\"e_uj\":{\"pkg\":%" PRId64 ",\"cpu_e\":%" PRId64 ",\"cpu_p\":%" PRId64 "},\"samples\":%" PRIu64
                    ",\"max_gap_ns\":%" PRIu64 ",\"foreign_busy_ms\":%" PRIu64 ",\"max_temp_mc\":%u}\n",
                    TY_DOMAIN_INTERVAL, h, x->file, x->w.config, x->w.cond, x->w.round, x->w.pos,
                    ty_energy_status_name(x->st), ty_conf_forge(TY_DIRECT), x->e[CH_PKG], x->e[CH_CPU_E], x->e[CH_CPU_P],
                    x->w.n_samples, x->w.max_gap_ns, x->w.foreign_busy_ms, x->w.max_temp_mc);
    }
}

static double sd_I_pkg(const char *config) {
    double v[512] = {0}, m, sd;
    size_t n = 0;
    for (size_t k = 0; k < g_nround && n < 512; k++)
        if (!strcmp(g_round[k].config, config) && g_round[k].st == TYE_OK) v[n++] = g_round[k].I[CH_PKG];
    mean_sd(v, n, &m, &sd);
    return sd;
}

static int mode_check(void) {
    print_windows();
    for (size_t k = 0; k < g_nround; k++) {
        const Round *r = &g_round[k];
        printf("ROUND %s r%u %s I_pkg_J=%.3f bound95_J=%.3f\n", r->config, r->round, ty_energy_status_name(r->st),
               r->I[CH_PKG], r->bound95);
    }
    for (size_t k = 0; k < g_nround; k++) attribute_solo(&g_round[k], sd_I_pkg(g_round[k].config));
    return 0;
}

typedef struct {
    char config[16];
} Cfg;

static size_t configs(Cfg *c, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < g_nwin; i++) {
        int seen = 0;
        for (size_t k = 0; k < n; k++) seen |= !strcmp(c[k].config, g_win[i].w.config);
        if (!seen && n < max) snprintf(c[n++].config, sizeof c[0].config, "%s", g_win[i].w.config);
    }
    return n;
}

static uint64_t calls_of(const Win *x, const char *tag) {
    for (size_t i = 0; x && i < x->w.npart; i++)
        if (!strcmp(x->w.part[i].tag, tag)) return x->w.part[i].calls;
    return 0;
}

/* I statistics block for one config. */
static void i_stats(const char *config, int ch, double *mean, double *lo, double *hi, double *blo, double *bhi,
                    int *n) {
    double v[512] = {0}, sd;
    size_t k2 = 0;
    for (size_t k = 0; k < g_nround && k2 < 512; k++)
        if (!strcmp(g_round[k].config, config) && g_round[k].st == TYE_OK) v[k2++] = g_round[k].I[ch];
    *n = (int)k2;
    mean_sd(v, k2, mean, &sd);
    double half = k2 > 1 ? tq975((int)k2 - 1) * sd / sqrt((double)k2) : NAN;
    *lo = *mean - half;
    *hi = *mean + half;
    boot_mean(v, k2, blo, bhi);
}

static int mode_stage1(const char *path) {
    print_windows();
    Cfg cf[32];
    size_t nc = configs(cf, 32);
    out = fopen(path, "w");
    if (!out) return 2;
    first_stack[0] = 1;
    depth = 0;
    o_open(NULL, '{');
    o_str("protocol", "docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md section 5");
    o_open("configs", '[');
    double best = -INFINITY, least = INFINITY;
    int ibest = -1, ileast = -1, best_real = 0;
    for (size_t c = 0; c < nc; c++) {
        double sc[64];
        int ns = 0, all_ok = 1;
        o_open(NULL, '{');
        o_str("config", cf[c].config);
        o_open("rounds", '[');
        for (size_t k = 0; k < g_nround && ns < 64; k++) {
            const Round *r = &g_round[k];
            if (strcmp(r->config, cf[c].config)) continue;
            int ok = r->c[1] && r->c[2] && r->c[3] && r->c[1]->st == TYE_OK && r->c[2]->st == TYE_OK &&
                     r->c[3]->st == TYE_OK;
            o_open(NULL, '{');
            o_u("round", r->round);
            o_str("status", ok ? "OK" : "REFUSED");
            if (ok) {
                double sa = 1 - (double)calls_of(r->c[3], "A") / (double)calls_of(r->c[1], "A");
                double sb = 1 - (double)calls_of(r->c[3], "B") / (double)calls_of(r->c[2], "B");
                o_num("slowdown_A", sa);
                o_num("slowdown_B", sb);
                o_num("score", (sa + sb) / 2);
                o_num("I_pkg_J", r->I[CH_PKG]);
                o_num("I_cpu_p_J", r->I[CH_CPU_P]);
                o_num("I_rest_J", r->I[CH_REST]);
                sc[ns++] = (sa + sb) / 2;
                if ((sa + sb) / 2 < 0.02) all_ok = 0;
            }
            o_close('}');
        }
        o_close(']');
        double m = NAN, sd;
        mean_sd(sc, (size_t)ns, &m, &sd);
        o_num("score", m);
        o_u("valid_rounds", (uint64_t)ns);
        double im, lo, hi, blo, bhi;
        int n;
        i_stats(cf[c].config, CH_PKG, &im, &lo, &hi, &blo, &bhi, &n);
        o_num("I_pkg_mean_J", im);
        o_num("I_pkg_t95_lo_J", lo);
        o_num("I_pkg_t95_hi_J", hi);
        const Win *any = NULL;
        for (size_t i = 0; i < g_nwin && !any; i++)
            if (!strcmp(g_win[i].w.config, cf[c].config)) any = &g_win[i];
        o_str("spec_a", any ? any->spec_a : "");
        o_str("spec_b", any ? any->spec_b : "");
        o_close('}');
        printf("CONFIG %s score=%.4f rounds=%d all_rounds>=0.02=%d I_pkg=%.2f J [%.2f, %.2f] A=%s B=%s\n",
               cf[c].config, m, ns, all_ok, im, lo, hi, any ? any->spec_a : "", any ? any->spec_b : "");
        if (ns > 0 && m > best) { best = m; ibest = (int)c; best_real = m >= 0.05 && all_ok; }
        if (ns > 0 && fabs(m) < least) { least = fabs(m); ileast = (int)c; }
    }
    o_close(']');
    o_open("selection", '{');
    o_str("rule", "max score is the contention case; REAL iff score >= 0.05 and every round >= 0.02; control = min |score|");
    o_str("contention", ibest >= 0 ? cf[ibest].config : "");
    o_bool("real_contention", best_real);
    o_str("control", ileast >= 0 ? cf[ileast].config : "");
    o_close('}');
    o_close('}');
    fputc('\n', out);
    fclose(out);
    printf("SELECT contention=%s real=%d control=%s\n", ibest >= 0 ? cf[ibest].config : "-", best_real,
           ileast >= 0 ? cf[ileast].config : "-");
    return 0;
}

static void fit_block(const char *key, const char *config, int ch, int f2, Fit *Fout, Holdout *Hout) {
    Fit F = fit(config, ch, f2);
    Holdout H = holdout(config, ch, f2, &F);
    o_open(key, '{');
    o_str("target", k_chx[ch]);
    o_str("features", f2 ? "1, cpu_cycles/1e9, l2d_cache_refill/1e9" : "1, cpu_cycles/1e9, inst_retired/1e9");
    o_u("fit_windows", (uint64_t)F.n);
    o_bool("fit_ok", F.ok);
    o_num("p0_T_J", F.beta[0]);
    o_num("a_J_per_1e9_cycles", F.beta[1]);
    o_num(f2 ? "b_J_per_1e9_l2refill" : "b_J_per_1e9_inst", F.beta[2]);
    o_num("residual_sd_J", F.s);
    o_num("condition_number_scaled", F.cond);
    o_bool("degenerate", F.degenerate);
    o_u("heldout_windows", (uint64_t)H.n);
    o_num("heldout_mean_error_J", H.mean_err);
    o_num("heldout_mean_error_t95_lo_J", H.lo);
    o_num("heldout_mean_error_t95_hi_J", H.hi);
    o_num("heldout_rms_error_J", H.rms);
    o_num("heldout_mean_relative_error", H.mean_rel);
    o_u("heldout_inside_95pi", (uint64_t)H.inside);
    o_bool("pass", H.pass);
    o_close('}');
    printf("FIT %s %s %s n=%d cond=%.1f s=%.3f J | heldout n=%d mean_err=%.3f J [%.3f, %.3f] inside=%d/%d rms=%.3f "
           "rel=%.4f pass=%d\n",
           config, k_chx[ch], f2 ? "l2" : "inst", F.n, F.cond, F.s, H.n, H.mean_err, H.lo, H.hi, H.inside, H.n,
           H.rms, H.mean_rel, H.pass);
    if (Fout) *Fout = F;
    if (Hout) *Hout = H;
}

static int mode_stage2(const char *path, const char *recs) {
    g_rec = fopen(recs, "w");
    if (!g_rec) return 2;
    print_windows();
    Cfg cf[32];
    size_t nc = configs(cf, 32);
    out = fopen(path, "w");
    if (!out) return 2;
    first_stack[0] = 1;
    depth = 0;
    o_open(NULL, '{');
    o_str("protocol", "docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md sections 9-12");
    o_open("meter", '{');
    o_u("resolution_uj", TY_RESOLUTION_UJ);
    o_u("sample_period_ns", TY_SAMPLE_PERIOD_NS);
    o_str("wrap", "32-bit mJ counters, about 4.29 MJ; overflow_raw flags refused");
    o_str("accuracy", "not claimed: no external calibration; 1 mJ step is resolution only");
    o_close('}');
    o_open("configs", '[');
    for (size_t c = 0; c < nc; c++) {
        const char *cfg = cf[c].config;
        o_open(NULL, '{');
        o_str("config", cfg);
        /* per-condition variance */
        o_open("conditions", '{');
        for (int k = 0; k < NCONDS; k++) {
            double v[512] = {0}, m, sd;
            size_t n = 0;
            for (size_t i = 0; i < g_nwin && n < 512; i++)
                if (!strcmp(g_win[i].w.config, cfg) && g_win[i].st == TYE_OK && !strcmp(g_win[i].w.cond, k_cond[k]))
                    v[n++] = (double)g_win[i].e[CH_PKG] / 1e6;
            mean_sd(v, n, &m, &sd);
            o_open(k_cond[k], '{');
            o_u("valid_windows", n);
            o_num("pkg_mean_J", m);
            o_num("pkg_sd_J", sd);
            o_close('}');
        }
        o_close('}');
        int total_rounds = 0;
        for (size_t k = 0; k < g_nround; k++) total_rounds += !strcmp(g_round[k].config, cfg);
        o_open("check_a", '{');
        int pass_a = 0, dist = 0;
        double eab = NAN;
        for (int ch = 0; ch < NCHX; ch++) {
            double m, lo, hi, blo, bhi;
            int n;
            i_stats(cfg, ch, &m, &lo, &hi, &blo, &bhi, &n);
            o_open(k_chx[ch], '{');
            o_u("valid_rounds", (uint64_t)n);
            o_num("I_mean_J", m);
            o_num("I_t95_lo_J", lo);
            o_num("I_t95_hi_J", hi);
            o_num("I_boot95_lo_J", blo);
            o_num("I_boot95_hi_J", bhi);
            o_bool("distinguishable_from_zero", lo > 0 || hi < 0);
            o_close('}');
            printf("I %s %s n=%d mean=%.3f J t95=[%.3f, %.3f] boot95=[%.3f, %.3f]\n", cfg, k_chx[ch], n, m, lo, hi,
                   blo, bhi);
            if (ch == CH_PKG) {
                double v[512] = {0}, sd;
                size_t k2 = 0;
                for (size_t i = 0; i < g_nwin && k2 < 512; i++)
                    if (!strcmp(g_win[i].w.config, cfg) && g_win[i].st == TYE_OK && !strcmp(g_win[i].w.cond, "AB"))
                        v[k2++] = (double)g_win[i].e[CH_PKG] / 1e6;
                mean_sd(v, k2, &eab, &sd);
                double half = (hi - lo) / 2;
                pass_a = n >= 8 && n * 10 >= total_rounds * 8 && half <= 0.02 * eab;
                dist = lo > 0 || hi < 0;
                o_num("E_AB_pkg_mean_J", eab);
                o_num("pkg_halfwidth_over_E_AB", half / eab);
            }
        }
        double bsum = 0;
        int bn = 0;
        for (size_t k = 0; k < g_nround; k++)
            if (!strcmp(g_round[k].config, cfg) && g_round[k].st == TYE_OK) { bsum += g_round[k].bound95; bn++; }
        o_num("analytic_edge_bound95_per_round_J", bn ? bsum / bn : NAN);
        o_u("rounds_total", (uint64_t)total_rounds);
        o_bool("pass", pass_a);
        o_bool("I_pkg_distinguishable_from_zero", dist);
        o_close('}');
        printf("CHECK_A %s pass=%d distinguishable=%d rounds=%d\n", cfg, pass_a, dist, total_rounds);

        Fit F;
        Holdout H;
        o_open("check_b", '{');
        fit_block("pkg_cycles_inst", cfg, CH_PKG, 0, &F, &H);
        o_bool("pass", H.pass);
        o_open("reported_not_gated", '{');
        fit_block("cpu_p_cycles_inst", cfg, CH_CPU_P, 0, NULL, NULL);
        fit_block("pkg_cycles_l2refill", cfg, CH_PKG, 1, NULL, NULL);
        o_str("naive_additive", "E_A + E_B - E_0; held-out error = I (see check_a)");
        o_close('}');
        o_close('}');
        printf("CHECK_B %s pass=%d\n", cfg, H.pass);

        double sdI = sd_I_pkg(cfg);
        int solo_ok = 0, solo_n = 0, mod_ok = 0, mod_n = 0;
        for (size_t k = 0; k < g_nround; k++) {
            const Round *r = &g_round[k];
            if (strcmp(r->config, cfg) || r->st != TYE_OK) continue;
            solo_n++;
            solo_ok += attribute_solo(r, sdI) == TYE_OK;
            if (H.pass) {
                mod_n++;
                mod_ok += attribute_model(r, &F, sdI) == TYE_OK;
            }
        }
        o_open("attribution", '{');
        o_u("solo_counterfactual_conserved", (uint64_t)solo_ok);
        o_u("solo_counterfactual_total", (uint64_t)solo_n);
        o_u("counter_model_conserved", (uint64_t)mod_ok);
        o_u("counter_model_total", (uint64_t)mod_n);
        o_num("tolerance_sd_I_pkg_J", sdI);
        o_close('}');
        o_close('}');
    }
    o_close(']');
    o_close('}');
    fputc('\n', out);
    fclose(out);
    fclose(g_rec);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ty_energy_reduce --check DIR | --stage1 DIR OUT.json | --stage2 DIR OUT.json RECS.jsonl\n");
        return 64;
    }
    if (load(argv[2]) != 0) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    build_rounds();
    if (!strcmp(argv[1], "--check")) return mode_check();
    if (!strcmp(argv[1], "--stage1") && argc == 4) return mode_stage1(argv[3]);
    if (!strcmp(argv[1], "--stage2") && argc == 5) return mode_stage2(argv[3], argv[4]);
    fprintf(stderr, "bad arguments\n");
    return 64;
}
