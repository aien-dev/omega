/* ESTIMATION v6 pre-freeze development analysis (docs/estimation/protocols/est-v6.md section 9
 * items 1 and 4; appendix est-v6-appendix-prefreeze.md). NOT the v6 tool, not a result about any
 * held-out data, and not a protocol tool: it changes no v5 threshold, parameter or receipt.
 *
 *   est6dev g1pit --raw R --marks M --dyn D --q Q --lam L --nu N --c C [--boot B] [--seed S] [--blocks]
 *       One-step G1 (est_v4.c, the v5 code, unchanged) with the GIVEN parameters on one run folder.
 *       Prints the v5 statistics it must reproduce (n, PIT bin shares, coverage 50/80/95, mean z,
 *       lag-1 z), the lower and upper 2.5 % miss rates separately, and a block bootstrap over load
 *       segments (a block is one marked load segment, or one idle stretch between marked segments)
 *       giving standard errors, percentile intervals and the design effect against independent
 *       steps for every bin share and miss rate.
 *   est6dev sched <seed> <seconds>
 *       The est_load schedule for a seed (same splitmix64 draw order as est_load.c) with per-level
 *       totals and the section 3 balance check (each of idle, L6, L12, L18 within 400..900 s).
 *
 * Refuses, before opening anything, any path that names a held-out run (v3, v4, v5 or v6 held-out
 * tag) or the v5 D2 folder; after loading, refuses an input whose SHA-256 is the v5 D2 raw or marks SHA (committed d2.sha256). */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "est3c_common.h"
#include "est_v4.h"

/* Allocation failure stops the tool with a message (no partial result is written). */
static void *xalloc(void *p, const char *what)
{
    if (!p) { fprintf(stderr, "est6dev: out of memory (%s)\n", what); exit(2); }
    return p;
}
#define XMALLOC(n) xalloc(malloc(n), __func__)
#define XCALLOC(n, s) xalloc(calloc((n), (s)), __func__)

#define D2_DATE_TAG "20261005T112838Z"
#define D2_RAW_SHA "7dda21f664587c4401326c3082f28039e133ba5627c8564678f259e2d7036ff0"
#define D2_MARKS_SHA "95879ed909b8fec1207695dc1093b24be56f00563cd364c62bd30007b0dc26bb"

static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static int forbidden_path(const char *path)
{
    static const char *tags[] = { "-heldout-", D2_DATE_TAG, "-est5-heldout", "-est4-heldout", "-est3c-heldout", "-est6-heldout", NULL };
    char rp[PATH_MAX];
    for (int i = 0; tags[i]; i++) {
        if (strstr(path, tags[i])) return 1;
        if (realpath(path, rp) && strstr(rp, tags[i])) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ schedule (item 4) */
static int cmd_sched(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: est6dev sched <seed> <seconds>\n"); return 2; }
    uint64_t seed = strtoull(argv[0], NULL, 0);
    long total = strtol(argv[1], NULL, 10);
    if (total <= 0) { fprintf(stderr, "seconds must be > 0\n"); return 2; }
    static const int levels[4] = { 0, 6, 12, 18 };
    long secs_by_level[4] = { 0, 0, 0, 0 };
    long acc = 0; int n = 0;
    uint64_t s = seed;
    printf("seed %llu seconds %ld\n", (unsigned long long)seed, total);
    while (acc < total && n < 4096) {
        int li = (int)(splitmix64(&s) % 4u);
        long d = 20 + (long)(splitmix64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        printf("schedule %d %d %ld\n", n, levels[li], d);
        secs_by_level[li] += d; acc += d; n++;
    }
    int ok = 1;
    for (int i = 0; i < 4; i++) if (secs_by_level[i] < 400 || secs_by_level[i] > 900) ok = 0;
    printf("segments %d idle %ld L6 %ld L12 %ld L18 %ld\n", n, secs_by_level[0], secs_by_level[1], secs_by_level[2], secs_by_level[3]);
    printf("balance %s\n", ok ? "PASS" : "FAIL");
    return 0;
}

/* first seed at or after <seed> (step +1) whose schedule passes the section 3 balance rule */
static int balance_ok(uint64_t seed, long total, long lv[4], int *nseg)
{
    static const int levels[4] = { 0, 6, 12, 18 };
    uint64_t s = seed; long acc = 0; int n = 0, ok = 1;
    for (int i = 0; i < 4; i++) lv[i] = 0;
    while (acc < total && n < 4096) {
        int li = (int)(splitmix64(&s) % 4u);
        long d = 20 + (long)(splitmix64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        lv[li] += d; acc += d; n++;
    }
    (void)levels;
    for (int i = 0; i < 4; i++) if (lv[i] < 400 || lv[i] > 900) ok = 0;
    *nseg = n;
    return ok;
}

static int cmd_pick(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: est6dev pick <seed> <seconds>\n"); return 2; }
    uint64_t seed = strtoull(argv[0], NULL, 0);
    long total = strtol(argv[1], NULL, 10), lv[4]; int ns;
    for (int k = 0; k < 100000; k++) {
        int ok = balance_ok(seed + (uint64_t)k, total, lv, &ns);
        printf("seed 0x%llx segments %d idle %ld L6 %ld L12 %ld L18 %ld balance %s\n", (unsigned long long)(seed + (uint64_t)k), ns, lv[0], lv[1], lv[2], lv[3], ok ? "PASS" : "FAIL");
        if (ok) { printf("picked 0x%llx after %d failures\n", (unsigned long long)(seed + (uint64_t)k), k); return 0; }
    }
    printf("none found\n");
    return 1;
}


/* ------------------------------------------------------------ schedule rule SR-1 (domain separated)
 * est-v6 section 3 as proposed chains "seed + 1" from each declared seed, which sends H1 (0xD6E6C7) and
 * H2 (0xD6E6C8) to the same seed 0xD6E6CB (appendix section 3). Rule SR-1 removes that:
 *   base(label, declared) = splitmix64 output of the state  declared XOR fnv1a64("est6-sched-v1/" label)
 *   candidate k           = base + k                         (k = 0, 1, 2, ...)
 *   effective seed        = first candidate whose est_load schedule passes the balance rule, k <= SR1_MAXK
 * The label is part of the derivation input, so two labels never share a chain. The balance rule is a
 * property of the schedule alone (no data); the search is bounded and fails closed. A plan with two
 * identical schedules (or seeds) is refused, never repaired by further search. */
#define SR1_MAXK 255
#define SEGMAX 4096
typedef struct { int level[SEGMAX]; long dur[SEGMAX]; int n; long lv[4]; long total; } sched_t;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 0x100000001b3ull; }
    return h;
}

static void gen_sched(uint64_t seed, long total, sched_t *o)
{
    static const int levels[4] = { 0, 6, 12, 18 };
    uint64_t s = seed; long acc = 0;
    memset(o, 0, sizeof *o);
    while (acc < total && o->n < SEGMAX) {
        int li = (int)(splitmix64(&s) % 4u);
        long d = 20 + (long)(splitmix64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        o->level[o->n] = levels[li]; o->dur[o->n] = d; o->lv[li] += d; acc += d; o->n++;
    }
    o->total = acc;
}

/* required properties: segments sum to the run length; every segment is 20..120 s except a clipped last
 * one (1..120); each of idle, L6, L12, L18 totals 400..900 s */
static int sched_props(const sched_t *o, long total, int *balanced)
{
    long sum = 0; int ok = 1;
    for (int i = 0; i < o->n; i++) {
        sum += o->dur[i];
        if (o->dur[i] > 120 || o->dur[i] < 1) ok = 0;
        if (o->dur[i] < 20 && i != o->n - 1) ok = 0;
    }
    if (sum != total || o->n >= SEGMAX) ok = 0;
    *balanced = 1;
    for (int i = 0; i < 4; i++) if (o->lv[i] < 400 || o->lv[i] > 900) *balanced = 0;
    return ok;
}

static uint64_t sr1_base(const char *label, uint64_t declared)
{
    char buf[128 + 32];
    snprintf(buf, sizeof buf, "est6-sched-v1/%.100s", label);
    uint64_t st = declared ^ fnv1a64(buf);
    return splitmix64(&st);
}

/* rule 0 = old (v6 proposal: declared + k), rule 1 = SR-1. Returns k or -1. */
static int sr_pick(int rule, const char *label, uint64_t declared, long total, uint64_t *eff, sched_t *o)
{
    uint64_t base = rule ? sr1_base(label, declared) : declared;
    for (int k = 0; k <= (rule ? SR1_MAXK : 100000); k++) {
        int bal;
        gen_sched(base + (uint64_t)k, total, o);
        if (sched_props(o, total, &bal) && bal) { *eff = base + (uint64_t)k; return k; }
    }
    return -1;
}

static int same_sched(const sched_t *a, const sched_t *b)
{
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) if (a->level[i] != b->level[i] || a->dur[i] != b->dur[i]) return 0;
    return 1;
}

/* est6dev plan6 [--rule old|sr1] <seconds> <label>=<declared seed> ... */
static int cmd_plan6(int argc, char **argv)
{
    int rule = 1;
    if (argc >= 2 && !strcmp(argv[0], "--rule")) {
        if (!strcmp(argv[1], "old")) rule = 0; else if (!strcmp(argv[1], "sr1")) rule = 1; else { fprintf(stderr, "rule must be old or sr1\n"); return 2; }
        argc -= 2; argv += 2;
    }
    if (argc < 2) { fprintf(stderr, "usage: est6dev plan6 [--rule old|sr1] <seconds> <label>=<seed> ...\n"); return 2; }
    long total = strtol(argv[0], NULL, 10);
    if (total <= 0) { fprintf(stderr, "seconds must be > 0\n"); return 2; }
    int np = argc - 1;
    static sched_t sc[16]; uint64_t eff[16]; char lab[16][64];
    if (np > 16) return 2;
    printf("rule %s seconds %ld\n", rule ? "SR-1" : "OLD(seed+1)", total);
    for (int i = 0; i < np; i++) {
        char *eq = strchr(argv[1 + i], '=');
        if (!eq || eq - argv[1 + i] >= 63) { fprintf(stderr, "bad plan item %s\n", argv[1 + i]); return 2; }
        memcpy(lab[i], argv[1 + i], (size_t)(eq - argv[1 + i])); lab[i][eq - argv[1 + i]] = 0;
        uint64_t decl = strtoull(eq + 1, NULL, 0);
        int k = sr_pick(rule, lab[i], decl, total, &eff[i], &sc[i]);
        if (k < 0) { printf("label %s declared 0x%llx NO_PASSING_SEED\n", lab[i], (unsigned long long)decl); return 1; }
        printf("label %s declared 0x%llx effective 0x%llx k %d segments %d idle %ld L6 %ld L12 %ld L18 %ld balance PASS\n",
               lab[i], (unsigned long long)decl, (unsigned long long)eff[i], k, sc[i].n, sc[i].lv[0], sc[i].lv[1], sc[i].lv[2], sc[i].lv[3]);
        for (int j = 0; j < sc[i].n; j++) printf("schedule %s %d %d %ld\n", lab[i], j, sc[i].level[j], sc[i].dur[j]);
    }
    int bad = 0;
    for (int i = 0; i < np; i++) for (int j = i + 1; j < np; j++)
        if (eff[i] == eff[j] || same_sched(&sc[i], &sc[j])) { printf("COLLISION %s %s effective 0x%llx\n", lab[i], lab[j], (unsigned long long)eff[i]); bad = 1; }
    if (bad) return 1;
    printf("distinct PASS\n");
    return 0;
}

/* ------------------------------------------------------------ one-step G1 PIT trace */
typedef struct { int64_t t; double flo, fhi, z; } step_t;

static est_obs_class classify(const c3_tick *t, est_pobs *ob)
{
    est_assumption a; c3_assumption_base(&a, EST_FAM_STUDENT_T);
    a.nparam = 2; a.param[0] = 5; a.param[1] = 100;
    if (est_pobs_classify(&a, t->value, t->present, &t->ev, ob) != EST_OK) return EST_OBS_CLASSES_;
    return ob->cls;
}

/* mirrors est5.c e4_score, one-step part only (same classification, burn-in, unscorable rule) */
static int run_g1(const c3_ticks *tk, const est4_params *p, step_t **out, size_t *nout, size_t *unscorable)
{
    est4_state s;
    est_dpred *dp = XMALLOC(sizeof *dp);
    step_t *st = XMALLOC((tk->n ? tk->n : 1) * sizeof *st);
    size_t n = 0; *unscorable = 0;
    if (!dp || !st) return 1;
    if (est4_init(p, &s) != EST_OK) return 1;
    long first = -1;
    for (size_t t = 0; t < tk->n; t++) {
        const c3_tick *tc = &tk->t[t];
        est_pobs ob; est_pinnov iv;
        est_obs_class cls = classify(tc, &ob);
        if (cls == EST_OBS_CLASSES_) return 1;
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        if (ok && scored) {
            if ((uint64_t)s.gap + 1u > EST_PRED_MAX_H) (*unscorable)++;
            else {
                est4_pred pr;
                if (est4_predict(&s, 1, &pr) != EST_OK || est4_dpred(&s, &pr, dp) != EST_OK) return 1;
                if (est_dpred_score(dp, &ob, &iv) != EST_OK) return 1;
                if (iv.valid) { st[n].t = tc->wall_ns; st[n].flo = iv.F_lo; st[n].fhi = iv.F_hi; st[n].z = iv.z; n++; }
            }
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) return 1;
    }
    free(dp);
    *out = st; *nout = n;
    return 0;
}

#define NSTAT 13   /* bins 0..9, lower miss, upper miss, cov95 */
static const char *STAT_NAME[NSTAT] = { "pit_bin0", "pit_bin1", "pit_bin2", "pit_bin3", "pit_bin4", "pit_bin5", "pit_bin6",
                                        "pit_bin7", "pit_bin8", "pit_bin9", "lower_miss_2.5", "upper_miss_2.5", "coverage95" };

static double overlap(double a, double b, double c, double d)
{
    double lo = a > c ? a : c, hi = b < d ? b : d;
    return hi > lo ? hi - lo : 0.0;
}

/* per-step contribution to each statistic: expected value under u ~ U(F_lo, F_hi) */
static void contrib(const step_t *s, double v[NSTAT])
{
    double span = s->fhi - s->flo;
    for (int j = 0; j < 10; j++) {
        if (span > 0.0) v[j] = overlap(s->flo, s->fhi, j / 10.0, (j + 1) / 10.0) / span;
        else { int k = (int)floor(s->flo * 10.0); if (k < 0) k = 0; if (k > 9) k = 9; v[j] = (k == j) ? 1.0 : 0.0; }
    }
    if (span > 0.0) {
        double l = (0.025 - s->flo) / span, u = (s->fhi - 0.975) / span;
        v[10] = l < 0 ? 0 : l > 1 ? 1 : l;
        v[11] = u < 0 ? 0 : u > 1 ? 1 : u;
    } else { v[10] = s->flo < 0.025; v[11] = s->flo > 0.975; }
    v[12] = 1.0 - v[10] - v[11];
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static double quant(const double *sorted, int n, double q)
{
    double pos = q * (double)(n - 1);
    int i = (int)floor(pos);
    if (i >= n - 1) return sorted[n - 1];
    return sorted[i] + (pos - i) * (sorted[i + 1] - sorted[i]);
}

/* block id of a time: odd 2i+1 inside marked pair i, even 2m between (m = pairs ended before t) */
static size_t block_of(const c3_marks *mk, int64_t t)
{
    size_t ended = 0;
    for (size_t i = 0; i < mk->n; i++) {
        if (t >= mk->b[i] && t <= mk->e[i]) return 2 * i + 1;
        if (mk->e[i] < t) ended++;
    }
    return 2 * ended;
}

static int cmd_g1pit(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL;
    double dyn = NAN, q = NAN, lam = NAN, nu = NAN, c = NAN;
    int boot = 2000, show_blocks = 0;
    uint64_t seed = 0xE6B007ull;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--dyn") && i + 1 < argc) dyn = atof(argv[++i]);
        else if (!strcmp(argv[i], "--q") && i + 1 < argc) q = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lam") && i + 1 < argc) lam = atof(argv[++i]);
        else if (!strcmp(argv[i], "--nu") && i + 1 < argc) nu = atof(argv[++i]);
        else if (!strcmp(argv[i], "--c") && i + 1 < argc) c = atof(argv[++i]);
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--blocks")) show_blocks = 1;
        else { fprintf(stderr, "usage: est6dev g1pit --raw R --marks M --dyn D --q Q --lam L --nu N --c C [--boot B] [--seed S] [--blocks]\n"); return 2; }
    }
    if (!raw || !mkp || isnan(dyn) || isnan(q) || isnan(lam) || isnan(nu) || isnan(c) || boot < 10) { fprintf(stderr, "est6dev g1pit: missing argument\n"); return 2; }
    if (forbidden_path(raw) || forbidden_path(mkp)) { fprintf(stderr, "refuse: a held-out path (v5 D2 and every held-out tag are forbidden in v6 development)\n"); return 2; }
    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!strcmp(f.sha_hex, D2_RAW_SHA)) { fprintf(stderr, "refuse: input is the v5 D2 raw data (SHA-256)\n"); est_file_free(&f); return 2; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    if (!strcmp(mk.sha, D2_MARKS_SHA)) { fprintf(stderr, "refuse: marks are the v5 D2 marks (SHA-256)\n"); est_file_free(&f); return 2; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); est_file_free(&f); return 1; }
    printf("input raw_sha256 %s marks_sha256 %s lines %zu marks_pairs %zu\n", f.sha_hex, mk.sha, tk.lines, mk.n);

    est4_params p = { EST4_G1_LAG, dyn, q, lam, nu, c, 0.95, 0.8, 0.2, C3_QUANTUM };
    step_t *st = NULL; size_t n = 0, unsc = 0;
    if (run_g1(&tk, &p, &st, &n, &unsc) || n == 0) { fprintf(stderr, "g1 run failed\n"); return 1; }

    /* per-step contributions, per-block sums */
    size_t nb_max = 2 * mk.n + 2;
    double (*bs)[NSTAT] = XCALLOC(nb_max, sizeof *bs);
    double *bn = XCALLOC(nb_max, sizeof *bn);
    double tot[NSTAT] = { 0 };
    double zsum = 0;
    if (!bs || !bn) return 1;
    for (size_t i = 0; i < n; i++) {
        double v[NSTAT];
        contrib(&st[i], v);
        size_t b = block_of(&mk, st[i].t);
        for (int k = 0; k < NSTAT; k++) { bs[b][k] += v[k]; tot[k] += v[k]; }
        bn[b] += 1.0;
        zsum += st[i].z;
    }
    double zm = zsum / (double)n;
    printf("params dyn %.17g q %.17g lam %.17g nu %.17g c %.17g\n", dyn, q, lam, nu, c);
    printf("n_scored %zu unscorable %zu\n", n, unsc);
    printf("mean_z_midpit %.6f\n", zm);

    /* blocks that have steps */
    size_t nb = 0, nload = 0, *idx = XMALLOC(nb_max * sizeof *idx);
    for (size_t b = 0; b < nb_max; b++) if (bn[b] > 0) { idx[nb++] = b; nload += (b & 1u); }
    printf("blocks %zu (loaded %zu, idle %zu) mean_steps_per_block %.1f\n", nb, nload, nb - nload, (double)n / (double)nb);
    if (show_blocks)
        for (size_t i = 0; i < nb; i++) {
            size_t b = idx[i];
            printf("block %zu %s level %d steps %.0f bin0_mass %.3f lower_miss_mass %.3f upper_miss_mass %.3f\n", b, (b & 1u) ? "load" : "idle",
                   (b & 1u) ? mk.level[b / 2] : 0, bn[b], bs[b][0], bs[b][10], bs[b][11]);
        }

    {   /* descriptive: shares by declared load level (idle = 0) */
        double ls[4][NSTAT] = { { 0 } }, ln[4] = { 0 };
        for (size_t i = 0; i < nb; i++) {
            size_t b = idx[i];
            int lv = (b & 1u) ? mk.level[b / 2] : 0, li = lv == 6 ? 1 : lv == 12 ? 2 : lv == 18 ? 3 : 0;
            ln[li] += bn[b];
            for (int k = 0; k < NSTAT; k++) ls[li][k] += bs[b][k];
        }
        static const int lvn[4] = { 0, 6, 12, 18 };
        for (int li = 0; li < 4; li++)
            if (ln[li] > 0) printf("by_level L%d steps %.0f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f\n", lvn[li], ln[li], ls[li][0] / ln[li], ls[li][9] / ln[li], ls[li][10] / ln[li], ls[li][11] / ln[li]);
    }
    /* bootstrap: resample whole blocks with replacement; ratio estimator */
    double *draw = XMALLOC((size_t)boot * NSTAT * sizeof(double));
    if (!draw) return 1;
    uint64_t rng = seed;
    for (int r = 0; r < boot; r++) {
        double sum[NSTAT] = { 0 }, cnt = 0;
        for (size_t i = 0; i < nb; i++) {
            size_t b = idx[splitmix64(&rng) % nb];
            for (int k = 0; k < NSTAT; k++) sum[k] += bs[b][k];
            cnt += bn[b];
        }
        for (int k = 0; k < NSTAT; k++) draw[(size_t)r * NSTAT + k] = sum[k] / cnt;
    }
    printf("bootstrap draws %d seed 0x%llx unit whole_blocks\n", boot, (unsigned long long)seed);
    printf("# stat value iid_se boot_se design_effect ci95_lo ci95_hi ci995_lo ci995_hi\n");
    double *col = XMALLOC((size_t)boot * sizeof *col);
    for (int k = 0; k < NSTAT; k++) {
        double val = tot[k] / (double)n, mean = 0, var = 0;
        for (int r = 0; r < boot; r++) { col[r] = draw[(size_t)r * NSTAT + k]; mean += col[r]; }
        mean /= boot;
        for (int r = 0; r < boot; r++) var += (col[r] - mean) * (col[r] - mean);
        var /= (boot - 1);
        double iid = sqrt(val * (1.0 - val) / (double)n);
        qsort(col, (size_t)boot, sizeof *col, cmpd);
        double bse = sqrt(var);
        printf("stat %s %.6f %.6f %.6f %.3f %.6f %.6f %.6f %.6f\n", STAT_NAME[k], val, iid, bse, iid > 0 ? (bse * bse) / (iid * iid) : NAN,
               quant(col, boot, 0.025), quant(col, boot, 0.975), quant(col, boot, 0.0025), quant(col, boot, 0.9975));
    }
    free(st); free(bs); free(bn); free(idx); free(draw); free(col);
    c3_ticks_free(&tk); est_file_free(&f);
    return 0;
}

/* ------------------------------------------------------------ G1S exploratory (item 2, reduced) */
/* Reduced item 2: the filter parameters (dyn q lam nu) stay at the v5 D1 values; only the skew kappa
 * and the scale c are searched (kappa x c grid), by 5-fold block cross-validation over load segments.
 * The two-piece t is built from the symmetric est4_fast_logp kernel (est_v4.c unchanged): scale c b on
 * the upper side and c kappa b on the lower side, side masses 1/(1+kappa) and kappa/(1+kappa). */
typedef struct { int64_t t; double y, m, b, anchor; } trace_t;

static int run_g1_trace(const c3_ticks *tk, const est4_params *p, trace_t **out, size_t *nout)
{
    est4_state s;
    trace_t *tr = XMALLOC((tk->n ? tk->n : 1) * sizeof *tr);
    size_t n = 0;
    if (!tr || est4_init(p, &s) != EST_OK) return 1;
    long first = -1;
    for (size_t t = 0; t < tk->n; t++) {
        est_pobs ob;
        est_obs_class cls = classify(&tk->t[t], &ob);
        if (cls == EST_OBS_CLASSES_) return 1;
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        if (ok && scored && (uint64_t)s.gap + 1u <= EST_PRED_MAX_H) {
            double m, v;
            if (est4_moments(&s, 1, &m, &v) != EST_OK) return 1;
            tr[n].t = tk->t[t].wall_ns; tr[n].y = ob.value; tr[n].m = m; tr[n].b = sqrt(s.g * v); tr[n].anchor = s.anchor; n++;
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) return 1;
    }
    *out = tr; *nout = n;
    return 0;
}

/* un-floored cdf at the bin edges from the symmetric kernel */
static void sym_edges(const trace_t *r, double scale, double nu, double *lo, double *hi)
{
    est4_pred pr = { 1u, r->m, scale, nu };
    double Fl, Fh;
    est4_fast_logp(&pr, C3_QUANTUM, r->anchor, r->y, &Fl, &Fh);
    double kd = nearbyint((r->y - r->anchor) / C3_QUANTUM);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N, idx = kd + (double)EST_PRED_K;
    *lo = (Fl - u * idx) / (1.0 - f);
    *hi = (Fh - u * (idx + 1.0)) / (1.0 - f);
}

static double two_piece(const trace_t *r, double c, double kappa, double nu, double *Flo, double *Fhi)
{
    double a = 1.0 / (1.0 + kappa), b = kappa / (1.0 + kappa);
    double lo_u, hi_u, lo_l, hi_l;
    sym_edges(r, c * r->b, nu, &lo_u, &hi_u);
    sym_edges(r, c * kappa * r->b, nu, &lo_l, &hi_l);
    double kd = nearbyint((r->y - r->anchor) / C3_QUANTUM);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    double mu = r->m - r->anchor, xa = (kd - 0.5) * C3_QUANTUM, xb = (kd + 0.5) * C3_QUANTUM;
    double Fa = xa < mu ? 2.0 * b * lo_l : b + 2.0 * a * (lo_u - 0.5);
    double Fb = xb < mu ? 2.0 * b * hi_l : b + 2.0 * a * (hi_u - 0.5);
    if (kd == -(double)EST_PRED_K) Fa = 0.0;
    if (kd == (double)EST_PRED_K) Fb = 1.0;
    double p = Fb - Fa; if (p < 0) p = 0;
    double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N, idx = kd + (double)EST_PRED_K;
    *Flo = (1.0 - f) * Fa + u * idx; *Fhi = (1.0 - f) * Fb + u * (idx + 1.0);
    p = (1.0 - f) * p + u;
    return log(p > 1e-300 ? p : 1e-300);
}

#define NK 5
#define NCG 21
static const double KAPPA[NK] = { 0.6, 0.7, 0.8, 0.9, 1.0 };

static int cmd_g1s(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL;
    double dyn = NAN, q = NAN, lam = NAN, nu = NAN;
    int nfold = 5;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--dyn") && i + 1 < argc) dyn = atof(argv[++i]);
        else if (!strcmp(argv[i], "--q") && i + 1 < argc) q = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lam") && i + 1 < argc) lam = atof(argv[++i]);
        else if (!strcmp(argv[i], "--nu") && i + 1 < argc) nu = atof(argv[++i]);
        else if (!strcmp(argv[i], "--folds") && i + 1 < argc) nfold = atoi(argv[++i]);
        else { fprintf(stderr, "usage: est6dev g1s --raw R --marks M --dyn D --q Q --lam L --nu N [--folds K]\n"); return 2; }
    }
    if (!raw || !mkp || isnan(dyn) || isnan(q) || isnan(lam) || isnan(nu) || nfold < 2 || nfold > 20) { fprintf(stderr, "est6dev g1s: missing argument\n"); return 2; }
    if (forbidden_path(raw) || forbidden_path(mkp)) { fprintf(stderr, "refuse: a held-out path (v5 D2 and every held-out tag are forbidden in v6 development)\n"); return 2; }
    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!strcmp(f.sha_hex, D2_RAW_SHA)) { fprintf(stderr, "refuse: input is the v5 D2 raw data (SHA-256)\n"); est_file_free(&f); return 2; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    if (!strcmp(mk.sha, D2_MARKS_SHA)) { fprintf(stderr, "refuse: marks are the v5 D2 marks (SHA-256)\n"); est_file_free(&f); return 2; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); est_file_free(&f); return 1; }
    printf("input raw_sha256 %s marks_sha256 %s\n", f.sha_hex, mk.sha);
    est4_params p = { EST4_G1_LAG, dyn, q, lam, nu, 1.0, 0.95, 0.8, 0.2, C3_QUANTUM };
    trace_t *tr = NULL; size_t n = 0;
    if (run_g1_trace(&tk, &p, &tr, &n) || n == 0) { fprintf(stderr, "g1 trace failed\n"); return 1; }
    /* fold of a step = rank of its block among blocks that have steps, modulo nfold */
    size_t nb_max = 2 * mk.n + 2;
    int *rank = XMALLOC(nb_max * sizeof *rank), *seen = XCALLOC(nb_max, sizeof *seen);
    size_t *blk = XMALLOC(n * sizeof *blk);
    for (size_t i = 0; i < n; i++) { blk[i] = block_of(&mk, tr[i].t); seen[blk[i]] = 1; }
    int nr = 0;
    for (size_t b = 0; b < nb_max; b++) rank[b] = seen[b] ? nr++ : -1;
    int *fold = XMALLOC(n * sizeof *fold);
    for (size_t i = 0; i < n; i++) fold[i] = rank[blk[i]] % nfold;
    /* per grid point, per step: logp, F_lo, F_hi */
    size_t G = (size_t)NK * NCG;
    double *lp = XMALLOC(G * n * sizeof *lp), *Flo = XMALLOC(G * n * sizeof *Flo), *Fhi = XMALLOC(G * n * sizeof *Fhi);
    if (!lp || !Flo || !Fhi) return 1;
    for (int ki = 0; ki < NK; ki++)
        for (int ci = 0; ci < NCG; ci++) {
            size_t g = (size_t)ki * NCG + (size_t)ci;
            double c = (double)(20 + 5 * ci) / 100.0;
            for (size_t i = 0; i < n; i++) lp[g * n + i] = two_piece(&tr[i], c, KAPPA[ki], nu, &Flo[g * n + i], &Fhi[g * n + i]);
        }
    /* bit-for-bit style check: kappa = 1.0 against est4_fast_logp (maximum absolute difference) */
    double maxd = 0;
    for (int ci = 0; ci < NCG; ci++)
        for (size_t i = 0; i < n; i++) {
            est4_pred pr = { 1u, tr[i].m, ((double)(20 + 5 * ci) / 100.0) * tr[i].b, nu };
            double d = fabs(est4_fast_logp(&pr, C3_QUANTUM, tr[i].anchor, tr[i].y, NULL, NULL) - lp[(size_t)(NK - 1) * NCG * n + (size_t)ci * n + i]);
            if (d > maxd) maxd = d;
        }
    printf("check kappa1_vs_est4_fast_logp max_abs_diff %.3g\n", maxd);
    /* in-sample best over the whole run, per kappa and overall */
    for (int ki = 0; ki < NK; ki++) {
        double best = -INFINITY; int bc = 0;
        for (int ci = 0; ci < NCG; ci++) {
            double s = 0; size_t g = (size_t)ki * NCG + (size_t)ci;
            for (size_t i = 0; i < n; i++) s += lp[g * n + i];
            if (s / (double)n > best) { best = s / (double)n; bc = ci; }
        }
        {
            size_t g = (size_t)ki * NCG + (size_t)bc; double b0 = 0, b9 = 0, l = 0, u = 0;
            for (size_t i = 0; i < n; i++) { step_t st = { 0, Flo[g * n + i], Fhi[g * n + i], 0 }; double v[NSTAT]; contrib(&st, v); b0 += v[0]; b9 += v[9]; l += v[10]; u += v[11]; }
            printf("insample kappa %.1f best_c %.2f mean_logscore %.6f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f\n", KAPPA[ki], (double)(20 + 5 * bc) / 100.0, best, b0 / (double)n, b9 / (double)n, l / (double)n, u / (double)n);
        }
    }
    /* out-of-fold: for each fold choose (kappa, c) on the other folds, score the held-out fold. restrict = 0: kappa free; 1: kappa fixed at 1.0 */
    for (int restrict_k = 0; restrict_k < 2; restrict_k++) {
        double bins[10] = { 0 }, lm = 0, um = 0, ls = 0;
        printf("oof %s\n", restrict_k ? "kappa=1.0 only (c refit by fold, control)" : "kappa free in {0.6..1.0}, c refit by fold");
        for (int fo = 0; fo < nfold; fo++) {
            double best = -INFINITY; size_t bg = 0;
            for (int ki = restrict_k ? NK - 1 : 0; ki < NK; ki++)
                for (int ci = 0; ci < NCG; ci++) {
                    size_t g = (size_t)ki * NCG + (size_t)ci; double s = 0;
                    for (size_t i = 0; i < n; i++) if (fold[i] != fo) s += lp[g * n + i];
                    if (s > best) { best = s; bg = g; }
                }
            size_t nf = 0; double fl = 0;
            for (size_t i = 0; i < n; i++) if (fold[i] == fo) {
                step_t st = { 0, Flo[bg * n + i], Fhi[bg * n + i], 0 };
                double v[NSTAT]; contrib(&st, v);
                for (int j = 0; j < 10; j++) bins[j] += v[j];
                lm += v[10]; um += v[11]; ls += lp[bg * n + i]; fl += lp[bg * n + i]; nf++;
            }
            printf("fold %d steps %zu chosen_kappa %.1f chosen_c %.2f oof_mean_logscore %.6f\n", fo, nf, KAPPA[bg / NCG], (double)(20 + 5 * (int)(bg % NCG)) / 100.0, nf ? fl / (double)nf : 0.0);
        }
        printf("oof_pooled n %zu mean_logscore %.6f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f bins", n, ls / (double)n, bins[0] / (double)n, bins[9] / (double)n, lm / (double)n, um / (double)n);
        for (int j = 0; j < 10; j++) printf(" %.4f", bins[j] / (double)n);
        printf("\n");
    }
    /* in-sample overall best (kappa, c) PIT shares, for reference */
    {
        double best = -INFINITY; size_t bg = 0;
        for (size_t g = 0; g < G; g++) { double s = 0; for (size_t i = 0; i < n; i++) s += lp[g * n + i]; if (s > best) { best = s; bg = g; } }
        double bins[10] = { 0 }, lm = 0, um = 0;
        for (size_t i = 0; i < n; i++) { step_t st = { 0, Flo[bg * n + i], Fhi[bg * n + i], 0 }; double v[NSTAT]; contrib(&st, v); for (int j = 0; j < 10; j++) bins[j] += v[j]; lm += v[10]; um += v[11]; }
        printf("insample_best kappa %.1f c %.2f mean_logscore %.6f pit_bin0 %.4f pit_bin9 %.4f lower_miss %.4f upper_miss %.4f\n", KAPPA[bg / NCG], (double)(20 + 5 * (int)(bg % NCG)) / 100.0, best / (double)n, bins[0] / (double)n, bins[9] / (double)n, lm / (double)n, um / (double)n);
    }
    free(tr); free(rank); free(seen); free(blk); free(fold); free(lp); free(Flo); free(Fhi);
    c3_ticks_free(&tk); est_file_free(&f);
    return 0;
}


/* ------------------------------------------------------------ G1S full fit (item 2, DEVELOPMENT only) */
/* est-v6.md section 5: G1 grid (dyn q lam nu c; lam grid extended downwards by 0.05 as section 5.5 says) times
 * kappa in {0.6..1.0}, 5-fold block cross-validation (fold = rank of the block among load segments and idle
 * stretches, modulo 5), selection by the pooled out-of-fold one-step mean log score; stage 2 (phi nu_h c_h
 * kappa_h) per fold on the training folds with stage 1 frozen; a control with kappa = 1.0 (G1 exactly: that
 * code path calls the unchanged est4_fast_logp) is fitted under the same scheme and folds. Reads D1 only. */
#define NKS 5
#define NLAMX 7
#define NDYN 6
#define NQX 9
#define NNU 7
#define NCX 21
static const double LAMX[NLAMX] = { 0.05, 0.1, 0.2, 0.3, 0.5, 0.7, 0.9 };
static const double DYNX[NDYN] = { 0.5, 1, 2, 4, 10, 30 };
static const double NUX[NNU] = { 0.8, 1, 1.25, 1.5, 2, 3, 5 };
static const double PHIX[5] = { 1, 0.95, 0.9, 0.8, 0.6 };
static const double NUHX[10] = { 0.5, 0.6, 0.7, 0.8, 1, 1.25, 1.5, 2, 3, 5 };
#define NCHX 31

typedef struct {
    size_t n1, n10;
    int64_t *t1, *t10;
    double *y1, *m1, *b1, *a1;
    double *y10, *m10, *v10, *g10, *gl10, *a10;
} tr2_t;

static int tr2_alloc(tr2_t *r, size_t cap)
{
    memset(r, 0, sizeof *r);
    cap = cap ? cap : 1;
    double **f[10] = { &r->y1, &r->m1, &r->b1, &r->a1, &r->y10, &r->m10, &r->v10, &r->g10, &r->gl10, &r->a10 };
    for (int i = 0; i < 10; i++) if (!(*f[i] = malloc(cap * sizeof(double)))) return 1;
    r->t1 = XMALLOC(cap * sizeof(int64_t)); r->t10 = XMALLOC(cap * sizeof(int64_t));
    return !(r->t1 && r->t10);
}
static void tr2_free(tr2_t *r)
{
    free(r->y1); free(r->m1); free(r->b1); free(r->a1); free(r->y10); free(r->m10); free(r->v10); free(r->g10); free(r->gl10); free(r->a10);
    free(r->t1); free(r->t10);
    memset(r, 0, sizeof *r);
}

typedef struct { int ok; double m, v, g, gl, a; } origin_t;

/* same tick walk as est5.c trace_run (one-step and ten-step origins), plus the target tick time */
static int tr2_run(const c3_ticks *tk, const est4_params *p, tr2_t *tr)
{
    est4_state s;
    if (est4_init(p, &s) != EST_OK) return 1;
    origin_t ring[C3_TEN];
    memset(ring, 0, sizeof ring);
    tr->n1 = tr->n10 = 0;
    long first = -1;
    for (size_t t = 0; t < tk->n; t++) {
        est_pobs ob;
        est_obs_class cls = classify(&tk->t[t], &ob);
        if (cls == EST_OBS_CLASSES_) return 1;
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        origin_t *o = &ring[t % C3_TEN];
        if (o->ok) {
            o->ok = 0;
            if (ok) {
                size_t k = tr->n10++;
                tr->y10[k] = ob.value; tr->m10[k] = o->m; tr->v10[k] = o->v; tr->g10[k] = o->g; tr->gl10[k] = o->gl; tr->a10[k] = o->a;
                tr->t10[k] = tk->t[t].wall_ns;
            }
        }
        if (ok && scored && (uint64_t)s.gap + 1u <= EST_PRED_MAX_H) {
            double m, v;
            if (est4_moments(&s, 1, &m, &v) != EST_OK) return 1;
            size_t k = tr->n1++;
            tr->y1[k] = ob.value; tr->m1[k] = m; tr->b1[k] = sqrt(s.g * v); tr->a1[k] = s.anchor; tr->t1[k] = tk->t[t].wall_ns;
        }
        if (est4_update(&s, ok, ok ? ob.value : NAN) != EST_OK) return 1;
        if (ok && scored) {
            double m, v;
            if (est4_moments(&s, C3_TEN, &m, &v) != EST_OK) return 1;
            o->ok = 1; o->m = m; o->v = v; o->g = s.g; o->gl = s.gl; o->a = s.anchor;
        }
    }
    return 0;
}

/* one grid point = (scale multiplier c on base b, kappa, nu). edges of the symmetric kernel with the upper
 * scale (kappa independent) are computed once per (nu, c) and reused for every kappa. */
static double tp_point(double kappa, double m, double anchor, double y, double lo_u, double hi_u, double lo_l, double hi_l,
                       double *Flo, double *Fhi)
{
    double a = 1.0 / (1.0 + kappa), b = kappa / (1.0 + kappa);
    double kd = nearbyint((y - anchor) / C3_QUANTUM);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    double mu = m - anchor, xa = (kd - 0.5) * C3_QUANTUM, xb = (kd + 0.5) * C3_QUANTUM;
    double Fa = xa < mu ? 2.0 * b * lo_l : b + 2.0 * a * (lo_u - 0.5);
    double Fb = xb < mu ? 2.0 * b * hi_l : b + 2.0 * a * (hi_u - 0.5);
    if (kd == -(double)EST_PRED_K) Fa = 0.0;
    if (kd == (double)EST_PRED_K) Fb = 1.0;
    double p = Fb - Fa; if (p < 0) p = 0;
    double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N, idx = kd + (double)EST_PRED_K;
    if (Flo) *Flo = (1.0 - f) * Fa + u * idx;
    if (Fhi) *Fhi = (1.0 - f) * Fb + u * (idx + 1.0);
    p = (1.0 - f) * p + u;
    return log(p > 1e-300 ? p : 1e-300);
}

/* symmetric kernel at scale `scale`: returns est4_fast_logp (the exact G1 score) and the un-floored edges */
static double sym_edges2(double m, double anchor, double y, double scale, double nu, double *lo, double *hi)
{
    est4_pred pr = { 1u, m, scale, nu };
    double Fl, Fh;
    double lp = est4_fast_logp(&pr, C3_QUANTUM, anchor, y, &Fl, &Fh);
    double kd = nearbyint((y - anchor) / C3_QUANTUM);
    if (kd < -(double)EST_PRED_K) kd = -(double)EST_PRED_K;
    if (kd > (double)EST_PRED_K) kd = (double)EST_PRED_K;
    double f = EST_PRED_FLOOR, u = EST_PRED_FLOOR / (double)EST_PRED_N, idx = kd + (double)EST_PRED_K;
    *lo = (Fl - u * idx) / (1.0 - f);
    *hi = (Fh - u * (idx + 1.0)) / (1.0 - f);
    return lp;
}

/* log score and floored F interval of one step under (kappa, scale c*b, nu); kappa 1.0 is est4_fast_logp itself */
static double one_step(double kappa, double m, double anchor, double y, double scale, double nu, double *Flo, double *Fhi)
{
    if (kappa == 1.0) {
        est4_pred pr = { 1u, m, scale, nu };
        double fl, fh, lp = est4_fast_logp(&pr, C3_QUANTUM, anchor, y, &fl, &fh);
        if (Flo) *Flo = fl;
        if (Fhi) *Fhi = fh;
        return lp;
    }
    double lo_u, hi_u, lo_l, hi_l;
    sym_edges2(m, anchor, y, scale, nu, &lo_u, &hi_u);
    sym_edges2(m, anchor, y, scale * kappa, nu, &lo_l, &hi_l);
    return tp_point(kappa, m, anchor, y, lo_u, hi_u, lo_l, hi_l, Flo, Fhi);
}

typedef struct { double dyn, q, lam, nu, c, kappa; int have; double train; double sum_all; } best1_t;
typedef struct { double phi, nuh, ch, kh; int have; double train; } best2_t;

/* stage 2 on the steps selected by mask (keep[j] != 0): ten-step mean log score over phi nu_h c_h kappa_h */
static void stage2(const tr2_t *tr, const int *keep, int kappa_free, best2_t *b)
{
    memset(b, 0, sizeof *b);
    size_t m = 0;
    for (size_t j = 0; j < tr->n10; j++) m += keep[j] ? 1u : 0u;
    if (!m) return;
    double best = -INFINITY;
    for (int ph = 0; ph < 5; ph++)
        for (int u = 0; u < 10; u++)
            for (int ci = 0; ci < NCHX; ci++) {
                double ch = (double)(10 + 5 * ci) / 100.0, sum[NKS] = { 0 };
                for (size_t j = 0; j < tr->n10; j++) {
                    if (!keep[j]) continue;
                    double gb = est4_gbar(tr->g10[j], tr->gl10[j], PHIX[ph], C3_TEN), sc = ch * sqrt(gb * tr->v10[j]);
                    double lo_u, hi_u;
                    sum[NKS - 1] += sym_edges2(tr->m10[j], tr->a10[j], tr->y10[j], sc, NUHX[u], &lo_u, &hi_u);
                    if (kappa_free)
                        for (int ki = 0; ki < NKS - 1; ki++) {
                            double lo_l, hi_l;
                            sym_edges2(tr->m10[j], tr->a10[j], tr->y10[j], sc * KAPPA[ki], NUHX[u], &lo_l, &hi_l);
                            sum[ki] += tp_point(KAPPA[ki], tr->m10[j], tr->a10[j], tr->y10[j], lo_u, hi_u, lo_l, hi_l, NULL, NULL);
                        }
                }
                for (int ki = kappa_free ? 0 : NKS - 1; ki < NKS; ki++) {
                    double ls = sum[ki] / (double)m;
                    if (isfinite(ls) && ls > best) { best = ls; b->have = 1; b->phi = PHIX[ph]; b->nuh = NUHX[u]; b->ch = ch; b->kh = KAPPA[ki]; b->train = ls; }
                }
            }
}

static double ten_step(const tr2_t *tr, size_t j, const best2_t *b, double *Flo, double *Fhi)
{
    double gb = est4_gbar(tr->g10[j], tr->gl10[j], b->phi, C3_TEN), sc = b->ch * sqrt(gb * tr->v10[j]);
    return one_step(b->kh, tr->m10[j], tr->a10[j], tr->y10[j], sc, b->nuh, Flo, Fhi);
}

static double probit(double u)
{
    if (u < 1e-12) u = 1e-12;
    if (u > 1.0 - 1e-12) u = 1.0 - 1e-12;
    double lo = -9, hi = 9;
    for (int i = 0; i < 80; i++) { double mid = 0.5 * (lo + hi); if (0.5 * erfc(-mid / sqrt(2.0)) < u) lo = mid; else hi = mid; }
    return 0.5 * (lo + hi);
}

/* per-step statistic vector for the screen and the load-stratum analysis */
#define NV 9
static const char *VNAME[NV] = { "pit_bin0", "pit_bin9", "lower_miss", "upper_miss", "cov50", "cov80", "cov95", "mean_u_minus_half", "mean_z_mid" };
static void stepvec(double flo, double fhi, double v[NV])
{
    step_t st = { 0, flo, fhi, 0 };
    double c[NSTAT];
    contrib(&st, c);
    v[0] = c[0]; v[1] = c[9]; v[2] = c[10]; v[3] = c[11];
    v[4] = est_frac_cover(flo, fhi, 0.50); v[5] = est_frac_cover(flo, fhi, 0.80); v[6] = est_frac_cover(flo, fhi, 0.95);
    double um = 0.5 * (flo + fhi);
    v[7] = um - 0.5; v[8] = probit(um);
}

typedef struct { size_t n; double *flo, *fhi; int *fold; int64_t *t; } oofset_t;

typedef struct { double dyn, q, lam, nu, c, kappa; } cfg_t;

static est4_params cfg_params(const cfg_t *c)
{
    est4_params p = { EST4_G1_LAG, c->dyn, c->q, c->lam, c->nu, c->c, 0.95, 0.8, 0.2, C3_QUANTUM };
    return p;
}

/* statistics over a set of one-step (or ten-step) F intervals, optionally restricted to a stratum */
static void accum(double *sum, double *cnt, double flo, double fhi)
{
    double v[NV];
    stepvec(flo, fhi, v);
    for (int k = 0; k < NV; k++) sum[k] += v[k];
    *cnt += 1.0;
}

static void print_stats(const char *tag, const double *sum, double cnt)
{
    printf("%s n %.0f", tag, cnt);
    for (int k = 0; k < NV; k++) printf(" %s %.4f", VNAME[k], cnt > 0 ? sum[k] / cnt : NAN);
    printf("\n");
}

/* gates of v6 section 6 on pooled OOF statistics: outer band and the section 5 inner band (shrunk) */
static void screen_line(const char *name, double v, double lo, double hi, double shrink)
{
    int outer = v >= lo && v <= hi, inner = v >= lo + shrink && v <= hi - shrink;
    printf("gate %s value %.4f outer [%.3f,%.3f] %s inner [%.3f,%.3f] %s\n", name, v, lo, hi, outer ? "PASS" : "FAIL", lo + shrink, hi - shrink, inner ? "PASS" : "FAIL");
}

static int cmd_g1sfull(int argc, char **argv)
{
    const char *raw = NULL, *mkp = NULL;
    int nfold = 5, stride = 1, boot = 2000;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) mkp = argv[++i];
        else if (!strcmp(argv[i], "--folds") && i + 1 < argc) nfold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--grid-stride") && i + 1 < argc) stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot = atoi(argv[++i]);
        else { fprintf(stderr, "usage: est6dev g1sfull --raw R --marks M [--folds K] [--grid-stride S] [--boot B]\n"); return 2; }
    }
    if (!raw || !mkp || nfold < 2 || nfold > 10 || stride < 1 || boot < 100) { fprintf(stderr, "est6dev g1sfull: missing or bad argument\n"); return 2; }
    if (forbidden_path(raw) || forbidden_path(mkp)) { fprintf(stderr, "refuse: a held-out path (v5 D2 and every held-out tag are forbidden in v6 development)\n"); return 2; }
    char err[512];
    est_file f;
    if (est_file_load(raw, &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    if (!strcmp(f.sha_hex, D2_RAW_SHA)) { fprintf(stderr, "refuse: input is the v5 D2 raw data (SHA-256)\n"); est_file_free(&f); return 2; }
    static c3_marks mk;
    if (c3_marks_load(mkp, 1, &mk, err, sizeof err)) { fprintf(stderr, "refuse: marks: %s\n", err); est_file_free(&f); return 1; }
    if (!strcmp(mk.sha, D2_MARKS_SHA)) { fprintf(stderr, "refuse: marks are the v5 D2 marks (SHA-256)\n"); est_file_free(&f); return 2; }
    c3_ticks tk;
    if (c3_ticks_build(&f, &tk, err, sizeof err)) { fprintf(stderr, "ticks: %s\n", err); est_file_free(&f); return 1; }
    printf("input raw_sha256 %s marks_sha256 %s\n", f.sha_hex, mk.sha);
    printf("label DEVELOPMENT RESULT (v5 D1 only; not a held-out result; v6 NOT FROZEN)\n");

    tr2_t tr;
    if (tr2_alloc(&tr, tk.n)) return 1;
    size_t nb_max = 2 * mk.n + 2;
    int *rank = XMALLOC(nb_max * sizeof *rank), *seen = XCALLOC(nb_max, sizeof *seen);
    int *fold1 = NULL;
    size_t n1 = 0;
    double bestf[10][2];              /* unused placeholder keeps layout simple */
    (void)bestf;
    best1_t bfree[10], bctl[10], ball_free, ball_ctl;   /* per fold, then whole pool */
    memset(bfree, 0, sizeof bfree); memset(bctl, 0, sizeof bctl); memset(&ball_free, 0, sizeof ball_free); memset(&ball_ctl, 0, sizeof ball_ctl);
    for (int i = 0; i < 10; i++) bfree[i].train = bctl[i].train = -INFINITY;
    ball_free.train = ball_ctl.train = -INFINITY;
    size_t points = 0, skipped = 0;
    double *edlo = NULL, *edhi = NULL;

    for (int a = 0; a < NDYN; a += stride)
        for (int bq = 0; bq < NQX; bq += stride)
            for (int l = 0; l < NLAMX; l += stride) {
                est4_params p = { EST4_G1_LAG, DYNX[a], pow(10.0, 2.0 + 0.5 * (double)bq), LAMX[l], 1.0, 1.0, 0.95, 0.8, 0.2, C3_QUANTUM };
                if (tr2_run(&tk, &p, &tr)) { skipped++; continue; }
                if (!fold1) {
                    n1 = tr.n1;
                    fold1 = XMALLOC(n1 * sizeof *fold1);
                    edlo = XMALLOC(n1 * sizeof *edlo); edhi = XMALLOC(n1 * sizeof *edhi);
                    size_t *blk = XMALLOC(n1 * sizeof *blk);
                    for (size_t i = 0; i < n1; i++) { blk[i] = block_of(&mk, tr.t1[i]); seen[blk[i]] = 1; }
                    int nr = 0;
                    for (size_t bb = 0; bb < nb_max; bb++) rank[bb] = seen[bb] ? nr++ : -1;
                    for (size_t i = 0; i < n1; i++) fold1[i] = rank[blk[i]] % nfold;
                    free(blk);
                } else if (tr.n1 != n1) { fprintf(stderr, "step set changed between grid points\n"); return 1; }
                for (int u = 0; u < NNU; u += stride)
                    for (int ci = 0; ci < NCX; ci += stride) {
                        double c = (double)(20 + 5 * ci) / 100.0;
                        double sf[NKS][10], tot[NKS];
                        memset(sf, 0, sizeof sf);
                        for (size_t i = 0; i < n1; i++) {
                            double lo_u, hi_u, lo_l, hi_l;
                            sf[NKS - 1][fold1[i]] += sym_edges2(tr.m1[i], tr.a1[i], tr.y1[i], c * tr.b1[i], NUX[u], &lo_u, &hi_u);
                            for (int ki = 0; ki < NKS - 1; ki++) {
                                sym_edges2(tr.m1[i], tr.a1[i], tr.y1[i], c * KAPPA[ki] * tr.b1[i], NUX[u], &lo_l, &hi_l);
                                sf[ki][fold1[i]] += tp_point(KAPPA[ki], tr.m1[i], tr.a1[i], tr.y1[i], lo_u, hi_u, lo_l, hi_l, NULL, NULL);
                            }
                        }
                        points++;
                        for (int ki = 0; ki < NKS; ki++) {
                            tot[ki] = 0;
                            for (int fo = 0; fo < nfold; fo++) tot[ki] += sf[ki][fo];
                            cfg_t cf = { DYNX[a], p.q, LAMX[l], NUX[u], c, KAPPA[ki] };
                            for (int fo = 0; fo < nfold; fo++) {
                                double tr_ls = (tot[ki] - sf[ki][fo]);
                                best1_t *bb = ki == NKS - 1 ? &bctl[fo] : NULL;
                                if (bb && isfinite(tr_ls) && tr_ls > bb->train) { *bb = (best1_t){ cf.dyn, cf.q, cf.lam, cf.nu, cf.c, cf.kappa, 1, tr_ls, sf[ki][fo] }; }
                                if (isfinite(tr_ls) && tr_ls > bfree[fo].train) bfree[fo] = (best1_t){ cf.dyn, cf.q, cf.lam, cf.nu, cf.c, cf.kappa, 1, tr_ls, sf[ki][fo] };
                            }
                            if (isfinite(tot[ki]) && tot[ki] > ball_free.train) ball_free = (best1_t){ cf.dyn, cf.q, cf.lam, cf.nu, cf.c, cf.kappa, 1, tot[ki], tot[ki] };
                            if (ki == NKS - 1 && isfinite(tot[ki]) && tot[ki] > ball_ctl.train) ball_ctl = (best1_t){ cf.dyn, cf.q, cf.lam, cf.nu, cf.c, cf.kappa, 1, tot[ki], tot[ki] };
                        }
                    }
            }
    if (!fold1) { fprintf(stderr, "no grid point ran\n"); return 1; }
    printf("grid dyn %d q %d lam %d nu %d c %d kappa %d stride %d points %zu filter_failed %zu steps %zu folds %d\n", NDYN, NQX, NLAMX, NNU, NCX, NKS, stride, points, skipped, n1, nfold);
    (void)edlo; (void)edhi;

    /* out-of-fold predictions for the chosen configuration of each fold: one-step and ten-step */
    oofset_t oo[2];                       /* 0 = G1S free kappa, 1 = control kappa 1.0 */
    size_t n10 = 0;
    double *flo10[2] = { NULL, NULL }, *fhi10[2] = { NULL, NULL }, *lp10[2] = { NULL, NULL };
    int64_t *t10 = NULL; int *fold10v = NULL;
    for (int w = 0; w < 2; w++) {
        oo[w].n = n1; oo[w].flo = XCALLOC(n1, sizeof(double)); oo[w].fhi = XCALLOC(n1, sizeof(double)); oo[w].fold = fold1; oo[w].t = NULL;
    }
    double *lp1o[2] = { XCALLOC(n1, sizeof(double)), XCALLOC(n1, sizeof(double)) };
    int *step_ok1 = XCALLOC(n1, sizeof(int));
    (void)step_ok1;
    char tags[2][8] = { "G1S", "G1ctl" };
    for (int w = 0; w < 2; w++) {
        printf("== %s selection by %d-fold block CV, then stage 2 (ten-step) per fold\n", w ? "control kappa=1.0 (G1 exactly)" : "G1S kappa free in {0.6..1.0}", nfold);
        for (int fo = 0; fo < nfold; fo++) {
            best1_t *b = w ? &bctl[fo] : &bfree[fo];
            cfg_t cf = { b->dyn, b->q, b->lam, b->nu, b->c, b->kappa };
            est4_params p = cfg_params(&cf);
            if (tr2_run(&tk, &p, &tr) || tr.n1 != n1) { fprintf(stderr, "refit trace failed\n"); return 1; }
            size_t nf = 0; double s = 0;
            for (size_t i = 0; i < n1; i++) if (fold1[i] == fo) {
                double fl, fh, lp = one_step(cf.kappa, tr.m1[i], tr.a1[i], tr.y1[i], cf.c * tr.b1[i], cf.nu, &fl, &fh);
                oo[w].flo[i] = fl; oo[w].fhi[i] = fh; lp1o[w][i] = lp; s += lp; nf++;
            }
            if (!fold10v) {
                n10 = tr.n10; fold10v = XMALLOC(n10 * sizeof *fold10v); t10 = XMALLOC(n10 * sizeof *t10);
                for (size_t j = 0; j < n10; j++) { fold10v[j] = rank[block_of(&mk, tr.t10[j])] % nfold; t10[j] = tr.t10[j]; }
                for (int ww = 0; ww < 2; ww++) { flo10[ww] = XCALLOC(n10, sizeof(double)); fhi10[ww] = XCALLOC(n10, sizeof(double)); lp10[ww] = XCALLOC(n10, sizeof(double)); }
            }
            int *keep = XMALLOC(tr.n10 * sizeof *keep);
            for (size_t j = 0; j < tr.n10; j++) keep[j] = fold10v[j] != fo;
            best2_t b2;
            stage2(&tr, keep, !w, &b2);
            if (!b2.have) { fprintf(stderr, "est6dev: stage 2 found no finite score (fold %d)\n", fo); return 1; }
            for (size_t j = 0; j < tr.n10; j++) if (fold10v[j] == fo) lp10[w][j] = ten_step(&tr, j, &b2, &flo10[w][j], &fhi10[w][j]);
            free(keep);
            printf("fold %d steps %zu stage1 dyn %g q %g lam %g nu %g c %.2f kappa %.1f train_mean_logscore %.6f oof_mean_logscore %.6f | stage2 phi %g nu_h %g c_h %.2f kappa_h %.1f\n",
                   fo, nf, cf.dyn, cf.q, cf.lam, cf.nu, cf.c, cf.kappa, b->train / (double)(n1 - nf), nf ? s / (double)nf : 0.0, b2.phi, b2.nuh, b2.ch, b2.kh);
        }
        /* pooled OOF statistics and the screen */
        double sum[NV] = { 0 }, cnt = 0, lsum = 0;
        for (size_t i = 0; i < n1; i++) { accum(sum, &cnt, oo[w].flo[i], oo[w].fhi[i]); lsum += lp1o[w][i]; }
        printf("oof_pooled %s mean_logscore %.6f\n", tags[w], lsum / (double)n1);
        print_stats("oof_pooled", sum, cnt);
        double ten_cov = 0, ten_ls = 0;
        for (size_t j = 0; j < n10; j++) { ten_cov += est_frac_cover(flo10[w][j], fhi10[w][j], 0.95); ten_ls += lp10[w][j]; }
        printf("oof_pooled_ten_step %s n %zu coverage95 %.4f mean_logscore %.6f\n", tags[w], n10, ten_cov / (double)n10, ten_ls / (double)n10);
        {   /* bins */
            double bins[10] = { 0 };
            for (size_t i = 0; i < n1; i++) { step_t st = { 0, oo[w].flo[i], oo[w].fhi[i], 0 }; double c[NSTAT]; contrib(&st, c); for (int j = 0; j < 10; j++) bins[j] += c[j]; }
            printf("oof_bins %s", tags[w]);
            for (int j = 0; j < 10; j++) printf(" %.4f", bins[j] / (double)n1);
            printf("\n");
            double lag = 0, zz = 0, zm = 0; size_t ln = 0;
            double zprev = NAN;
            for (size_t i = 0; i < n1; i++) {
                double z = probit(0.5 * (oo[w].flo[i] + oo[w].fhi[i]));
                zz += z * z; zm += z;
                if (i > 0 && tr.t1[i] - tr.t1[i - 1] > 500000000LL && tr.t1[i] - tr.t1[i - 1] < 1500000000LL) { lag += z * zprev; ln++; }
                zprev = z;
            }
            double zbar = zm / (double)n1, var = zz / (double)n1 - zbar * zbar;
            (void)var;
            printf("oof_lag1_z_uncentred %s %.4f over %zu consecutive pairs\n", tags[w], ln ? lag / (double)ln : NAN, ln);
            screen_line("P1 cov50", sum[4] / cnt, 0.46, 0.54, 0.01);
            screen_line("P2 cov80", sum[5] / cnt, 0.76, 0.84, 0.01);
            screen_line("P3 cov95", sum[6] / cnt, 0.93, 0.97, 0.02);
            screen_line("P4 lower_miss", sum[2] / cnt, 0.010, 0.040, 0.01);
            screen_line("P5 upper_miss", sum[3] / cnt, 0.010, 0.040, 0.01);
            for (int j = 0; j < 10; j++) { char nm[16]; snprintf(nm, sizeof nm, "P6 bin%d", j); screen_line(nm, bins[j] / (double)n1, 0.07, 0.13, 0.01); }
            screen_line("P7 ten_cov95", ten_cov / (double)n10, 0.90, 0.99, 0.03);
        }
        /* load-stratum analysis of the OOF PIT with block bootstrap within stratum */
        {
            size_t nb = nb_max;
            double (*bs)[NV] = XCALLOC(nb, sizeof *bs); double *bn = XCALLOC(nb, sizeof *bn);
            for (size_t i = 0; i < n1; i++) { size_t b = block_of(&mk, tr.t1[i]); accum(bs[b], &bn[b], oo[w].flo[i], oo[w].fhi[i]); }
            const int lvv[4] = { 0, 6, 12, 18 };
            for (int li = 0; li < 4; li++) {
                size_t *ids = XMALLOC(nb * sizeof *ids); size_t ni = 0;
                double tot[NV] = { 0 }, tn = 0;
                for (size_t b = 0; b < nb; b++) {
                    if (bn[b] <= 0) continue;
                    int lv = (b & 1u) ? mk.level[b / 2] : 0;
                    if (lv != lvv[li]) continue;
                    ids[ni++] = b; tn += bn[b];
                    for (int k = 0; k < NV; k++) tot[k] += bs[b][k];
                }
                if (ni == 0) { free(ids); continue; }
                uint64_t rng = 0xE6B007ull + (uint64_t)li;
                double *dr = XMALLOC((size_t)boot * NV * sizeof *dr);
                for (int r = 0; r < boot; r++) {
                    double ss[NV] = { 0 }, nn = 0;
                    for (size_t k = 0; k < ni; k++) { size_t b = ids[splitmix64(&rng) % ni]; nn += bn[b]; for (int q = 0; q < NV; q++) ss[q] += bs[b][q]; }
                    for (int q = 0; q < NV; q++) dr[(size_t)r * NV + q] = ss[q] / nn;
                }
                printf("stratum %s L%d blocks %zu steps %.0f", tags[w], lvv[li], ni, tn);
                for (int k = 0; k < NV; k++) {
                    if (k == 4 || k == 5) continue;
                    double mean = 0, var = 0;
                    for (int r = 0; r < boot; r++) mean += dr[(size_t)r * NV + k];
                    mean /= boot;
                    for (int r = 0; r < boot; r++) { double d = dr[(size_t)r * NV + k] - mean; var += d * d; }
                    var /= (boot - 1);
                    printf(" %s %.4f(se %.4f)", VNAME[k], tot[k] / tn, sqrt(var));
                }
                printf("\n");
                free(dr); free(ids);
            }
            free(bs); free(bn);
        }
    }
    /* final refit on the whole pool (committed params would come from this) */
    printf("== final whole-pool refit (stage 1; same grid)\n");
    printf("final G1S dyn %g q %g lam %g nu %g c %.2f kappa %.1f in_sample_mean_logscore %.6f\n", ball_free.dyn, ball_free.q, ball_free.lam, ball_free.nu, ball_free.c, ball_free.kappa, ball_free.train / (double)n1);
    printf("final control dyn %g q %g lam %g nu %g c %.2f kappa %.1f in_sample_mean_logscore %.6f\n", ball_ctl.dyn, ball_ctl.q, ball_ctl.lam, ball_ctl.nu, ball_ctl.c, ball_ctl.kappa, ball_ctl.train / (double)n1);
    printf("tie_rule G1S preferred over G1 only if out-of-fold log score exceeds the control by more than 0.01 nats\n");
    c3_ticks_free(&tk); est_file_free(&f); tr2_free(&tr);
    return 0;
}


/* two-piece predictive self check on synthetic inputs (no data file): for each kappa, nu, scale and location
 * the bin probabilities over k = -K..K sum to 1, the floored CDF is continuous and non-decreasing across bins,
 * and kappa = 1.0 is exactly est4_fast_logp. The two-piece formula at kappa = 1.0 agrees with it to rounding. */
static int cmd_twopiece_selfcheck(void)
{
    static const double nus[3] = { 0.8, 1.25, 5.0 }, scs[3] = { 0.3, 1.0, 4.0 }, locs[3] = { 0.0, 0.37, -2.6 };
    double maxsum = 0, maxjump = 0, maxk1 = 0; int bad = 0;
    for (int ki = 0; ki < NKS; ki++)
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 3; c++) {
            double anchor = 1234.0 * C3_QUANTUM, m = anchor + locs[c] * C3_QUANTUM, sum = 0, prev_hi = 0;
            for (int k = -EST_PRED_K; k <= EST_PRED_K; k++) {
                double y = anchor + (double)k * C3_QUANTUM, fl, fh;
                double lp = one_step(KAPPA[ki], m, anchor, y, scs[b] * C3_QUANTUM, nus[a], &fl, &fh);
                sum += exp(lp);
                if (k > -EST_PRED_K) { double j = fabs(fl - prev_hi); if (j > maxjump) maxjump = j; }
                if (fh < fl - 1e-15) bad = 1;
                prev_hi = fh;
                if (KAPPA[ki] == 1.0 && k >= -3 && k <= 3) {
                    double lo_u, hi_u, lp2, f2l, f2h;
                    sym_edges2(m, anchor, y, scs[b] * C3_QUANTUM, nus[a], &lo_u, &hi_u);
                    lp2 = tp_point(1.0, m, anchor, y, lo_u, hi_u, lo_u, hi_u, &f2l, &f2h);
                    double d = fabs(lp2 - lp); if (d > maxk1) maxk1 = d;
                }
            }
            if (fabs(sum - 1.0) > maxsum) maxsum = fabs(sum - 1.0);
        }
    printf("twopiece max_abs(sum p - 1) %.3g max_cdf_jump_between_bins %.3g max_abs(two_piece_formula_at_kappa1 - est4_fast_logp) %.3g monotone %s\n", maxsum, maxjump, maxk1, bad ? "NO" : "yes");
    printf("twopiece-selfcheck: %s\n", (maxsum < 1e-9 && maxjump < 1e-9 && maxk1 < 1e-9 && !bad) ? "PASS" : "FAIL");
    return (maxsum < 1e-9 && maxjump < 1e-9 && maxk1 < 1e-9 && !bad) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: est6dev g1pit|g1s|g1sfull|twopiece-selfcheck|sched|pick|plan6 ...\n"); return 2; }
    if (!strcmp(argv[1], "g1pit")) return cmd_g1pit(argc - 2, argv + 2);
    if (!strcmp(argv[1], "sched")) return cmd_sched(argc - 2, argv + 2);
    if (!strcmp(argv[1], "pick")) return cmd_pick(argc - 2, argv + 2);
    if (!strcmp(argv[1], "plan6")) return cmd_plan6(argc - 2, argv + 2);
    if (!strcmp(argv[1], "g1s")) return cmd_g1s(argc - 2, argv + 2);
    if (!strcmp(argv[1], "g1sfull")) return cmd_g1sfull(argc - 2, argv + 2);
    if (!strcmp(argv[1], "twopiece-selfcheck")) return cmd_twopiece_selfcheck();
    fprintf(stderr, "usage: est6dev g1pit|g1s|g1sfull|twopiece-selfcheck|sched|pick|plan6 ...\n");
    return 2;
}
