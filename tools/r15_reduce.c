/*
 * r15_reduce.c -- the R15 reducer (spec/r15-performance-proof.md §10-§12).
 *
 *   r15_reduce <raw-dir> <summary.json>
 *
 * Reads only <raw-dir>: SHA256SUMS and the *.jsonl files it lists (every
 * listed file must hash to its line; an unlisted .jsonl is an error). Recomputes
 * every metric (§6, 1-17) and every gate (§11, G1-G16) and writes summary.json.
 * Deterministic: files in name order, bootstrap seed 0x15, 10,000 resamples,
 * percentile method, nothing discarded (§10). No number here is edited by
 * hand; missing data makes a gate FAIL, never PASS.
 */
#include "sha256.h"

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal JSON ----------------------------------------------------------- */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JT;
typedef struct J {
    JT t;
    double d;
    uint64_t u;
    int is_u;
    char *s;
    struct J **v;
    char **k;
    size_t n, cap;
} J;

static const char *jp;

static void jws(void) { while (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r') jp++; }

static J *jnew(JT t) {
    J *j = calloc(1, sizeof *j);
    if (!j) { fprintf(stderr, "out of memory\n"); exit(3); }
    j->t = t;
    return j;
}

static void jpush(J *p, char *key, J *v) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->v = realloc(p->v, p->cap * sizeof *p->v);
        if (p->t == J_OBJ) p->k = realloc(p->k, p->cap * sizeof *p->k);
        if (!p->v || (p->t == J_OBJ && !p->k)) { fprintf(stderr, "out of memory\n"); exit(3); }
    }
    if (p->t == J_OBJ) p->k[p->n] = key;
    p->v[p->n++] = v;
}

static char *jstr_raw(void) {
    if (*jp != '"') return NULL;
    jp++;
    size_t cap = 32, n = 0;
    char *s = malloc(cap);
    while (*jp && *jp != '"') {
        char c = *jp++;
        if (c == '\\' && *jp) c = *jp++;
        if (n + 2 > cap) s = realloc(s, cap *= 2);
        s[n++] = c;
    }
    if (*jp != '"') { free(s); return NULL; }
    jp++;
    s[n] = 0;
    return s;
}

static J *jparse(void) {
    jws();
    if (*jp == '{') {
        jp++;
        J *o = jnew(J_OBJ);
        jws();
        if (*jp == '}') { jp++; return o; }
        for (;;) {
            jws();
            char *k = jstr_raw();
            if (!k) return NULL;
            jws();
            if (*jp++ != ':') return NULL;
            J *v = jparse();
            if (!v) return NULL;
            jpush(o, k, v);
            jws();
            if (*jp == ',') { jp++; continue; }
            if (*jp == '}') { jp++; return o; }
            return NULL;
        }
    }
    if (*jp == '[') {
        jp++;
        J *a = jnew(J_ARR);
        jws();
        if (*jp == ']') { jp++; return a; }
        for (;;) {
            J *v = jparse();
            if (!v) return NULL;
            jpush(a, NULL, v);
            jws();
            if (*jp == ',') { jp++; continue; }
            if (*jp == ']') { jp++; return a; }
            return NULL;
        }
    }
    if (*jp == '"') { J *s = jnew(J_STR); s->s = jstr_raw(); return s->s ? s : NULL; }
    if (!strncmp(jp, "true", 4)) { jp += 4; J *b = jnew(J_BOOL); b->d = 1; return b; }
    if (!strncmp(jp, "false", 5)) { jp += 5; return jnew(J_BOOL); }
    if (!strncmp(jp, "null", 4)) { jp += 4; return jnew(J_NULL); }
    char *end;
    J *x = jnew(J_NUM);
    if (*jp >= '0' && *jp <= '9') {
        const char *q = jp;
        while (*q >= '0' && *q <= '9') q++;
        if (*q != '.' && *q != 'e' && *q != 'E') {
            x->u = strtoull(jp, &end, 10);
            x->is_u = 1;
            x->d = (double)x->u;
            jp = end;
            return x;
        }
    }
    x->d = strtod(jp, &end);
    if (end == jp) return NULL;
    jp = end;
    return x;
}

static void jfree(J *j) {
    if (!j) return;
    for (size_t i = 0; i < j->n; i++) { jfree(j->v[i]); if (j->k) free(j->k[i]); }
    free(j->v); free(j->k); free(j->s); free(j);
}

static J *jget(const J *o, const char *k) {
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++) if (!strcmp(o->k[i], k)) return o->v[i];
    return NULL;
}
static double jd(const J *o, const char *k) { J *v = jget(o, k); return v && v->t == J_NUM ? v->d : NAN; }
static uint64_t ju(const J *o, const char *k) { J *v = jget(o, k); return v && v->t == J_NUM ? (v->is_u ? v->u : (uint64_t)v->d) : 0; }
static const char *js(const J *o, const char *k) { J *v = jget(o, k); return v && v->t == J_STR ? v->s : ""; }
static uint64_t jau(const J *a, size_t i) { return a && a->t == J_ARR && i < a->n ? a->v[i]->u : 0; }

/* ---- vectors and statistics ----------------------------------------------- */

typedef struct { double *v; size_t n, cap; } Vd;
typedef struct { uint64_t *v; size_t n, cap; } Vu;
static void vd_push(Vd *a, double x) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 64; a->v = realloc(a->v, a->cap * sizeof *a->v); }
    a->v[a->n++] = x;
}
static void vu_push(Vu *a, uint64_t x) {
    if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 1024; a->v = realloc(a->v, a->cap * sizeof *a->v); }
    a->v[a->n++] = x;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int cmp_u(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

static double median_d(const double *v, size_t n) {
    if (!n) return NAN;
    double *c = malloc(n * sizeof *c);
    memcpy(c, v, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_d);
    double m = n % 2 ? c[n / 2] : (c[n / 2 - 1] + c[n / 2]) / 2;
    free(c);
    return m;
}

/* nearest rank on a sorted array */
static uint64_t pct_u(const uint64_t *s, size_t n, double p) {
    if (!n) return 0;
    size_t r = (size_t)ceil(p * (double)n);
    if (r < 1) r = 1;
    if (r > n) r = n;
    return s[r - 1];
}

static uint64_t g_rng;
static uint64_t splitmix(void) {
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#define RESAMPLES 10000
/* 95% percentile CI of the median; the generator restarts at 0x15 for every
 * comparison so one result never depends on another's order. */
static void boot_ci(const double *v, size_t n, double *lo, double *hi) {
    *lo = *hi = NAN;
    if (n < 2) return;
    g_rng = 0x15;
    double *med = malloc(RESAMPLES * sizeof *med), *s = malloc(n * sizeof *s);
    for (int b = 0; b < RESAMPLES; b++) {
        for (size_t i = 0; i < n; i++) s[i] = v[splitmix() % n];
        med[b] = median_d(s, n);
    }
    qsort(med, RESAMPLES, sizeof *med, cmp_d);
    *lo = med[(size_t)(0.025 * RESAMPLES) - 1];
    *hi = med[(size_t)(0.975 * RESAMPLES) - 1];
    free(med); free(s);
}

/* ---- output ------------------------------------------------------------------ */

static FILE *out;
static int first_stack[32], depth;
static void o_sep(void) { if (!first_stack[depth]) fputc(',', out); first_stack[depth] = 0; }
static void o_key(const char *k) { o_sep(); if (k) fprintf(out, "\"%s\":", k); }
static void o_open(const char *k, char c) { o_key(k); fputc(c, out); first_stack[++depth] = 1; }
static void o_close(char c) { fputc(c, out); depth--; }
static void o_num(const char *k, double x) {
    o_key(k);
    if (isnan(x) || isinf(x)) fputs("null", out);
    else if (x == floor(x) && fabs(x) < 9e15) fprintf(out, "%.0f", x);
    else fprintf(out, "%.9g", x);
}
static void o_u(const char *k, uint64_t x) { o_key(k); fprintf(out, "%" PRIu64, x); }
static void o_str(const char *k, const char *s) { o_key(k); fprintf(out, "\"%s\"", s); }
static void o_bool(const char *k, int b) { o_key(k); fputs(b ? "true" : "false", out); }

/* ---- inputs -------------------------------------------------------------------- */

#define MAXF 4096
static char *g_files[MAXF];
static char g_hash[MAXF][65];
static int g_nfiles;

static void hex(const uint8_t *d, char *o) { for (int i = 0; i < 32; i++) sprintf(o + 2 * i, "%02x", d[i]); }

static int hash_file(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n);
    fclose(f);
    uint8_t d[32];
    sha256_final(&c, d);
    hex(d, out_hex);
    return 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* ---- per-process records --------------------------------------------------------- */

enum { W_IDLE, W_BEFORE, W_AFTER, W_SPAN, W_N };
static const char *const k_win[W_N] = {"IDLE", "BEFORE", "AFTER", "ADAPT_AFTER"};
enum { C_ADAPT, C_COST, C_BEFORE, C_AFTER, C_SPAN, C_N };
static const char *const k_cwin[C_N] = {"ADAPT", "ADAPT_COST", "BEFORE", "AFTER", "ADAPT_AFTER"};

typedef struct { uint64_t t, pkg; int ok; } Tel;

typedef struct {
    char file[256], config[24];
    int round, silicon, exit_code, complete;
    J *episode, *adapt, *residency, *observers;
    J *win[W_N], *cwin[C_N];
    Tel *tel;
    size_t ntel, captel;
} Trial;

#define MAXT 512
static Trial g_trials[MAXT];
static int g_nt;

typedef struct { char key[96]; Vu v; } L1Set;
#define MAXL1 256
static L1Set g_l1[MAXL1];
static int g_nl1;
static int g_l1_runs, g_l1_failed_runs, g_l1_timing_bad, g_l1_b_failed;

static Vu *l1_set(const char *measure, const char *field, uint64_t param, const char *cfg) {
    char key[96];
    snprintf(key, sizeof key, "%s|%s|%" PRIu64 "|%s", measure, field, param, cfg);
    for (int i = 0; i < g_nl1; i++) if (!strcmp(g_l1[i].key, key)) return &g_l1[i].v;
    if (g_nl1 == MAXL1) { fprintf(stderr, "too many L1 sets\n"); exit(3); }
    snprintf(g_l1[g_nl1].key, sizeof g_l1[g_nl1].key, "%s", key);
    return &g_l1[g_nl1++].v;
}

typedef struct { char config[24]; uint64_t enter, begin, flip, receipt, commits; int rc, trial; } Barrier;
static Barrier *g_bar;
static size_t g_nbar, g_capbar;

typedef struct { char config[24]; int exit_code, silicon; J *window, *residency; Vu e2e, gpu, enq, pub, dep; uint64_t claims; } L2Run;
#define MAXL2 64
static L2Run g_l2[MAXL2];
static int g_nl2;

typedef struct { char config[24]; int timing; double thr; } WRun;
static WRun g_w[64];
static int g_nw;

static void add_barrier(const char *cfg, uint64_t en, uint64_t be, uint64_t fl, uint64_t re, uint64_t c,
                        int rc, int trial) {
    if (g_nbar == g_capbar) { g_capbar = g_capbar ? g_capbar * 2 : 64; g_bar = realloc(g_bar, g_capbar * sizeof *g_bar); }
    Barrier *b = &g_bar[g_nbar++];
    snprintf(b->config, sizeof b->config, "%s", cfg);
    b->enter = en; b->begin = be; b->flip = fl; b->receipt = re; b->commits = c; b->rc = rc; b->trial = trial;
}

static int read_file(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t cap = 1 << 20;
    char *line = malloc(cap);
    Trial *t = NULL;
    L2Run *l2 = NULL;
    const char *level = NULL;
    char lvl[32] = "", cfg[24] = "";
    int round = 0, lineno = 0, bad = 0, silicon = 0;
    while (1) {
        size_t n = 0;
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n') {
            if (n + 2 > cap) line = realloc(line, cap *= 2);
            line[n++] = (char)c;
        }
        if (n == 0 && c == EOF) break;
        line[n] = 0;
        lineno++;
        jp = line;
        J *r = jparse();
        if (!r) { fprintf(stderr, "%s:%d: not JSON\n", name, lineno); bad = 1; break; }
        const char *kind = js(r, "kind");
        if (!lvl[0]) {
            snprintf(lvl, sizeof lvl, "%s", js(r, "level"));
            snprintf(cfg, sizeof cfg, "%s", js(r, "config"));
            round = (int)jd(r, "round");
            level = lvl;
            if (!strcmp(level, "trial")) {
                if (g_nt == MAXT) { fprintf(stderr, "too many trials\n"); exit(3); }
                t = &g_trials[g_nt++];
                snprintf(t->file, sizeof t->file, "%s", name);
                snprintf(t->config, sizeof t->config, "%s", cfg);
                t->round = round;
                t->exit_code = -1;
            } else if (!strcmp(level, "l2")) {
                if (g_nl2 == MAXL2) { fprintf(stderr, "too many L2 runs\n"); exit(3); }
                l2 = &g_l2[g_nl2++];
                snprintf(l2->config, sizeof l2->config, "%s", cfg);
                l2->exit_code = -1;
            } else {
                g_l1_runs++;
            }
        }
        int keep = 0;
        if (!strcmp(kind, "start")) silicon = (int)jd(r, "silicon");
        if (!strcmp(kind, "end")) {
            int ex = (int)jd(r, "exit");
            if (t) t->exit_code = ex;
            else if (l2) l2->exit_code = ex;
            else if (ex != 0) g_l1_failed_runs++;
        }
        if (t) {
            t->silicon = silicon;
            if (!strcmp(kind, "episode")) { t->episode = r; keep = 1; }
            else if (!strcmp(kind, "adapt")) {
                t->adapt = r; keep = 1;
                if (ju(r, "barrier_enter"))
                    add_barrier(cfg, ju(r, "barrier_enter"), ju(r, "barrier_begin"), ju(r, "barrier_flip"),
                                ju(r, "barrier_receipt"), ju(r, "barrier_production_commits"),
                                (int)jd(r, "barrier_result"), 1);
            }
            else if (!strcmp(kind, "residency")) { t->residency = r; keep = 1; }
            else if (!strcmp(kind, "observers")) { t->observers = r; keep = 1; }
            else if (!strcmp(kind, "result")) t->complete = (int)jd(r, "complete");
            else if (!strcmp(kind, "window")) {
                for (int w = 0; w < W_N; w++) if (!strcmp(js(r, "window"), k_win[w])) { t->win[w] = r; keep = 1; }
            } else if (!strcmp(kind, "cwindow")) {
                for (int w = 0; w < C_N; w++) if (!strcmp(js(r, "window"), k_cwin[w])) { t->cwin[w] = r; keep = 1; }
            } else if (!strcmp(kind, "telemetry")) {
                J *e = jget(r, "e");
                if (t->ntel == t->captel) { t->captel = t->captel ? t->captel * 2 : 1024; t->tel = realloc(t->tel, t->captel * sizeof *t->tel); }
                t->tel[t->ntel++] = (Tel){ju(e, "t_ns"), jau(jget(e, "energy_uj"), 0), (int)jd(e, "spbm_ok")};
            }
        } else if (l2) {
            l2->silicon = silicon;
            if (!strcmp(kind, "window")) { l2->window = r; keep = 1; }
            else if (!strcmp(kind, "residency")) { l2->residency = r; keep = 1; }
            else if (!strcmp(kind, "l2_claim")) {
                uint64_t in = ju(r, "input_t"), ss = ju(r, "seat_start_t"), sc = ju(r, "seat_commit_t"),
                         ds = ju(r, "dependent_start_t");
                uint32_t pick = (uint32_t)ju(r, "chip_pick"), done = (uint32_t)ju(r, "chip_done");
                uint64_t g = silicon ? (uint32_t)(done - pick) : 0;
                l2->claims++;
                if (in && ss >= in) vu_push(&l2->enq, ss - in);
                if (silicon) vu_push(&l2->gpu, g);
                if (sc >= ss && ss) vu_push(&l2->pub, sc - ss > g ? sc - ss - g : 0);
                if (ds >= sc && sc) vu_push(&l2->dep, ds - sc);
                if (ds >= in && in && ds) vu_push(&l2->e2e, ds - in);
            }
        } else {
            if (!strcmp(kind, "l1")) {
                J *v = jget(r, "v");
                Vu *s = l1_set(js(r, "measure"), js(r, "field"), ju(r, "param"), cfg);
                for (size_t i = 0; v && i < v->n; i++) vu_push(s, v->v[i]->u);
            } else if (!strcmp(kind, "timing_status")) {
                if (!(int)jd(r, "ok")) g_l1_timing_bad++;
            } else if (!strcmp(kind, "l1_b_failed")) {
                if (ju(r, "failed_samples")) g_l1_b_failed++;
            } else if (!strcmp(kind, "l1_g")) {
                add_barrier(cfg, ju(r, "enter"), ju(r, "begin"), ju(r, "flip"), ju(r, "receipt"),
                            ju(r, "production_commits_barrier"), (int)jd(r, "promote_rc"), 0);
            } else if (!strcmp(kind, "window") && !strncmp(js(r, "window"), "W_TIMING", 8) && g_nw < 64) {
                WRun *w = &g_w[g_nw++];
                snprintf(w->config, sizeof w->config, "%s", cfg);
                w->timing = !strcmp(js(r, "window"), "W_TIMING_ON");
                w->thr = jd(r, "ops") / (jd(r, "wall_ns") / 1e9);
            }
        }
        if (!keep) jfree(r);
        if (c == EOF) break;
    }
    free(line);
    fclose(f);
    return bad ? -1 : 0;
}

/* ---- per-trial derived values ------------------------------------------------ */

static const J *st(const J *w, const char *k) { return jget(jget(w, "stats"), k); }
static double stat(const J *w, const char *k) { const J *v = st(w, k); return v ? v->d : NAN; }

static double win_thr(const J *w) { return w ? jd(w, "ops") / (jd(w, "wall_ns") / 1e9) : NAN; }

static int energy_ok(const J *e) {
    if (!e || !(int)jd(e, "spbm_ok")) return 0;
    J *ov = jget(e, "overflow");
    for (size_t i = 0; ov && i < ov->n; i++) if (ov->v[i]->u) return 0;
    return 1;
}

/* package energy over a window, µJ; NAN if not a valid physical reading */
static double win_pkg_uj(const J *w) {
    if (!w) return NAN;
    J *e0 = jget(w, "e0"), *e1 = jget(w, "e1");
    if (!energy_ok(e0) || !energy_ok(e1)) return NAN;
    uint64_t a = jau(jget(e0, "energy_uj"), 0), b = jau(jget(e1, "energy_uj"), 0);
    return b >= a ? (double)(b - a) : NAN;
}
static double win_nvml_mj(const J *w) {
    if (!w) return NAN;
    J *e0 = jget(w, "e0"), *e1 = jget(w, "e1");
    if (!(int)jd(e0, "nvml_ok") || !(int)jd(e1, "nvml_ok")) return NAN;
    return jd(e1, "nvml_mj") - jd(e0, "nvml_mj");
}

static double idle_w(const Trial *t) {
    double uj = win_pkg_uj(t->win[W_IDLE]);
    return uj / jd(t->win[W_IDLE], "wall_ns") * 1e3;   /* µJ/ns = kW; ×1e3 → W */
}

/* net package µJ per production operation in window w */
static double energy_per_op(const Trial *t, int w) {
    const J *x = t->win[w];
    double uj = win_pkg_uj(x), p = idle_w(t), ops = jd(x, "ops");
    if (isnan(uj) || isnan(p) || !(ops > 0)) return NAN;
    return (uj - p * jd(x, "wall_ns") / 1e9 * 1e6) / ops;
}
static double gross_energy_per_op(const Trial *t, int w) {
    const J *x = t->win[w];
    double uj = win_pkg_uj(x), ops = jd(x, "ops");
    return ops > 0 ? uj / ops : NAN;
}

/* package energy between two instants from the 10 Hz telemetry, µJ */
static double tel_energy(const Trial *t, uint64_t a, uint64_t b) {
    const Tel *lo = NULL, *hi = NULL;
    for (size_t i = 0; i < t->ntel; i++) {
        if (!t->tel[i].ok) continue;
        if (t->tel[i].t <= a) lo = &t->tel[i];
        if (t->tel[i].t >= b && !hi) hi = &t->tel[i];
    }
    return lo && hi && hi->pkg >= lo->pkg ? (double)(hi->pkg - lo->pkg) : NAN;
}

static int is_cfg(const Trial *t, const char *c) { return !strcmp(t->config, c); }
static int nodigest(const Trial *t) { return strstr(t->config, "NODIGEST") != NULL; }

/* G1 for one trial; why receives the first failure */
static int trial_correct(const Trial *t, char *why, size_t n) {
    const J *e = t->episode;
#define NEED(c, ...) do { if (!(c)) { snprintf(why, n, __VA_ARGS__); return 0; } } while (0)
    NEED(t->exit_code == 0 && t->complete, "trial did not complete (exit %d)", t->exit_code);
    NEED(e, "no episode record");
    NEED(ju(e, "wrong") == 0, "wrong results %" PRIu64, ju(e, "wrong"));
    NEED(ju(e, "illegal") == 0, "illegal transitions");
    NEED(nodigest(t) || (int)jd(e, "crumbs_verified") == 1, "crumbs do not verify");
    NEED(ju(e, "crumb_overflow") == 0, "crumb log overflowed");
    NEED(ju(e, "goal_status") == 2, "goal not MET (status %" PRIu64 ")", ju(e, "goal_status"));
    NEED(ju(e, "gen_after") == ju(e, "gen_before") + 1, "generation did not advance exactly once");
    NEED(ju(e, "unpromoted_use") == 0, "a realization not in force was used");
    NEED(ju(e, "aegis_woken") == 0 && ju(e, "root_woken") == 0, "AEGIS/root woken during the episode");
    NEED(ju(e, "lost_triggers") == 0, "lost triggers");
#undef NEED
    return 1;
}

static int same_realization(const Trial *a, const Trial *b) {
    J *x = jget(a->episode, "inforce_id"), *y = jget(b->episode, "inforce_id");
    if (!x || !y || x->n != 4 || y->n != 4) return 0;
    for (int i = 0; i < 4; i++) if (x->v[i]->u != y->v[i]->u) return 0;
    return 1;
}

static const Trial *find_trial(const char *cfg, int round) {
    for (int i = 0; i < g_nt; i++) if (is_cfg(&g_trials[i], cfg) && g_trials[i].round == round) return &g_trials[i];
    return NULL;
}

static double adapt_rate(const Trial *t) {
    const J *c = t->cwin[C_ADAPT];
    if (!c) return NAN;
    double dt = (jd(c, "t1") - jd(c, "t0")) / 1e9;
    return dt > 0 ? jd(c, "serve_commits") / dt : NAN;
}

static double wasted(const J *c) {
    J *k = jget(c, "kinds");
    if (!k) return NAN;
    return jd(k, "invalidated") + jd(k, "blocked_authority") + jd(k, "rejected") + jd(k, "failed") +
           jd(k, "noop") + jd(k, "quarantine");
}

static double per_cons(const Trial *t, double x) {
    double c = jd(t->cwin[C_SPAN], "consequential");
    return c > 0 ? x / c : NAN;
}

static double wake_amp(const Trial *t) {
    const J *w = t->win[W_SPAN];
    if (!w) return NAN;
    return per_cons(t, strncmp(t->config, "SEQ", 3) == 0 ? stat(w, "seq_polls") : stat(w, "wakes"));
}

static double crossover_ops(const Trial *t) {
    double cost = jd(t->cwin[C_COST], "adapt_cost_ns");
    double gain = (jd(t->adapt, "reference_ps") - jd(t->adapt, "selected_ps")) / 1000.0;
    return gain > 0 ? cost / gain : NAN;
}

/* ---- paired comparisons ------------------------------------------------------ */

typedef struct { double med, lo, hi; size_t n, excluded; } Cmp;

typedef double (*TrialVal)(const Trial *);
static double v_after_thr(const Trial *t) { return win_thr(t->win[W_AFTER]); }
static double v_before_thr(const Trial *t) { return win_thr(t->win[W_BEFORE]); }
static double v_adapt_rate(const Trial *t) { return adapt_rate(t); }
static double v_energy_after(const Trial *t) { return energy_per_op(t, W_AFTER); }
static double v_wasted(const Trial *t) { return per_cons(t, wasted(t->cwin[C_SPAN])); }

/* median and CI of per-round ratios num/den; AFTER metrics require the same
 * in-force realization (§8) */
static Cmp paired(const char *num, const char *den, TrialVal f, int need_same) {
    Cmp c = {NAN, NAN, NAN, 0, 0};
    Vd r = {0};
    for (int i = 0; i < g_nt; i++) {
        const Trial *a = &g_trials[i];
        if (!is_cfg(a, num)) continue;
        const Trial *b = find_trial(den, a->round);
        if (!b) continue;
        if (need_same && !same_realization(a, b)) { c.excluded++; continue; }
        double x = f(a), y = f(b);
        if (!isnan(x) && !isnan(y) && y > 0) vd_push(&r, x / y);
        else c.excluded++;
    }
    c.n = r.n;
    c.med = median_d(r.v, r.n);
    boot_ci(r.v, r.n, &c.lo, &c.hi);
    free(r.v);
    return c;
}

/* G15 diagnostic (observability only; G15 itself is min seat_live/intervals and
 * is not touched). For the process with the lowest live fraction, say what the
 * sampler's own record can tell: whether the lost samples are one long run of
 * not-live samples (the seat stopped advancing its heartbeat) or scattered,
 * and whether the sampler itself ran late. Data recorded before the sampler
 * carried diag fields reports UNMEASURED; nothing is inferred. */
static void o_residency_diagnostic(const J *tot, const char *name) {
    if (!tot) return;
    uint64_t lost = ju(tot, "intervals") - ju(tot, "seat_live");
    const J *d = jget(tot, "diag");
    o_open("worst_process_diagnostic", '{');
    o_str("process", name);
    o_u("lost_samples", lost);
    if (!lost) {
        o_str("class", "no_loss");
    } else if (!d) {
        o_str("class", "UNMEASURED");
        o_str("why", "raw evidence has no sampler diag fields; starvation, residency loss and stale reads are not distinguishable");
    } else {
        uint64_t longest = ju(d, "dead_longest");
        o_str("class", longest >= 100 ? "seat_stopped_long_run" : "scattered_not_live");
        o_u("dead_runs", ju(d, "dead_runs"));
        o_u("dead_longest", longest);
        o_u("dead_at_end", ju(d, "dead_at_end"));
        o_u("late_2ms", ju(d, "late_2ms"));
        o_u("late_10ms", ju(d, "late_10ms"));
        o_bool("sampler_late_observed", ju(d, "late_10ms") > 0);
        o_u("max_gap_ns", ju(d, "max_gap_ns"));
        o_u("stale_max_ns", ju(d, "stale_max_ns"));
        if (ju(d, "first_dead_t") && ju(d, "t_first")) o_u("first_dead_after_ns", ju(d, "first_dead_t") - ju(d, "t_first"));
    }
    o_close('}');
}

static void o_cmp(const char *k, const Cmp *c) {
    o_open(k, '{');
    o_num("median_ratio", c->med);
    o_num("ci95_lo", c->lo);
    o_num("ci95_hi", c->hi);
    o_u("pairs", c->n);
    o_u("pairs_excluded", c->excluded);
    o_close('}');
}

static double med_of(const char *cfg, TrialVal f, size_t *n) {
    Vd v = {0};
    for (int i = 0; i < g_nt; i++)
        if (is_cfg(&g_trials[i], cfg)) { double x = f(&g_trials[i]); if (!isnan(x)) vd_push(&v, x); }
    double m = median_d(v.v, v.n);
    if (n) *n = v.n;
    free(v.v);
    return m;
}

/* ---- latency sets --------------------------------------------------------- */

typedef struct { uint64_t n, p50, p90, p95, p99, p999, max; } Pct;
static Pct pct_of(Vu *v) {
    Pct p = {0};
    if (!v || !v->n) return p;
    qsort(v->v, v->n, sizeof *v->v, cmp_u);
    p.n = v->n;
    p.p50 = pct_u(v->v, v->n, 0.50); p.p90 = pct_u(v->v, v->n, 0.90); p.p95 = pct_u(v->v, v->n, 0.95);
    p.p99 = pct_u(v->v, v->n, 0.99); p.p999 = pct_u(v->v, v->n, 0.999); p.max = v->v[v->n - 1];
    return p;
}
static void o_pct(const char *k, Pct p) {
    o_open(k, '{');
    o_u("n", p.n); o_u("p50", p.p50); o_u("p90", p.p90); o_u("p95", p.p95); o_u("p99", p.p99);
    o_u("p99_9", p.p999); o_u("max", p.max);
    o_close('}');
}
static Vu *l1_get(const char *measure, const char *field, uint64_t param, const char *cfg) {
    char key[96];
    snprintf(key, sizeof key, "%s|%s|%" PRIu64 "|%s", measure, field, param, cfg);
    for (int i = 0; i < g_nl1; i++) if (!strcmp(g_l1[i].key, key)) return &g_l1[i].v;
    return NULL;
}

/* merged crumb-latency histograms of one config/window (metric 2) */
static Pct hist_pct(const char *cfg, int cw) {
    Vu lower = {0}, count = {0};
    for (int i = 0; i < g_nt; i++) {
        const Trial *t = &g_trials[i];
        if (!is_cfg(t, cfg) || !t->cwin[cw]) continue;
        J *b = jget(jget(t->cwin[cw], "latency"), "b");
        for (size_t j = 0; b && j < b->n; j++) {
            vu_push(&lower, jau(b->v[j], 0));
            vu_push(&count, jau(b->v[j], 1));
        }
    }
    Pct p = {0};
    uint64_t total = 0;
    for (size_t i = 0; i < count.n; i++) total += count.v[i];
    if (!total) { free(lower.v); free(count.v); return p; }
    /* sort pairs by lower bound (insertion via index array) */
    size_t *ix = malloc(lower.n * sizeof *ix);
    for (size_t i = 0; i < lower.n; i++) ix[i] = i;
    for (size_t i = 1; i < lower.n; i++) {
        size_t v = ix[i], j = i;
        while (j > 0 && lower.v[ix[j - 1]] > lower.v[v]) { ix[j] = ix[j - 1]; j--; }
        ix[j] = v;
    }
    double q[5] = {0.5, 0.9, 0.95, 0.99, 0.999};
    uint64_t *dst[5] = {&p.p50, &p.p90, &p.p95, &p.p99, &p.p999};
    uint64_t cum = 0;
    int k = 0;
    for (size_t i = 0; i < lower.n; i++) {
        cum += count.v[ix[i]];
        while (k < 5 && (double)cum >= ceil(q[k] * (double)total)) *dst[k++] = lower.v[ix[i]];
        p.max = lower.v[ix[i]];
    }
    p.n = total;
    free(ix); free(lower.v); free(count.v);
    return p;
}

/* ---- gates ---------------------------------------------------------------------- */

typedef struct { const char *id, *what, *threshold; double value; int pass; char note[200]; } Gate;
static Gate g_gate[16];

static void gate(int i, const char *id, const char *what, const char *thr, double v, int pass,
                 const char *note) {
    g_gate[i] = (Gate){id, what, thr, v, pass, ""};
    snprintf(g_gate[i].note, sizeof g_gate[i].note, "%s", note ? note : "");
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: r15_reduce <raw-dir> <summary.json>\n"); return 64; }
    const char *dir = argv[1];
    char sums[1024];
    snprintf(sums, sizeof sums, "%s/SHA256SUMS", dir);
    char sums_hex[65];
    if (hash_file(sums, sums_hex) != 0) { fprintf(stderr, "no SHA256SUMS in %s\n", dir); return 2; }
    FILE *sf = fopen(sums, "r");
    char line[1024];
    int errors = 0;
    while (sf && fgets(line, sizeof line, sf)) {
        char h[65], name[900];
        if (sscanf(line, "%64s %899s", h, name) != 2) continue;
        char *nm = name[0] == '*' ? name + 1 : name;
        size_t L = strlen(nm);
        if (L < 6 || strcmp(nm + L - 6, ".jsonl") != 0) continue;
        char path[2048], got[65];
        snprintf(path, sizeof path, "%s/%s", dir, nm);
        if (hash_file(path, got) != 0 || strcmp(got, h) != 0) { fprintf(stderr, "hash mismatch: %s\n", nm); errors++; }
        if (g_nfiles < MAXF) { g_files[g_nfiles] = strdup(nm); memcpy(g_hash[g_nfiles], got, 65); g_nfiles++; }
    }
    if (sf) fclose(sf);
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        size_t L = strlen(de->d_name);
        if (L < 6 || strcmp(de->d_name + L - 6, ".jsonl")) continue;
        int listed = 0;
        for (int i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i], de->d_name)) listed = 1;
        if (!listed) { fprintf(stderr, "not in SHA256SUMS: %s\n", de->d_name); errors++; }
    }
    if (d) closedir(d);
    if (errors) return 2;
    qsort(g_files, (size_t)g_nfiles, sizeof *g_files, cmp_str);
    for (int i = 0; i < g_nfiles; i++)
        if (read_file(dir, g_files[i]) != 0) { fprintf(stderr, "unreadable: %s\n", g_files[i]); return 2; }
    /* hashes follow the sorted order */
    for (int i = 0; i < g_nfiles; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, g_files[i]);
        hash_file(path, g_hash[i]);
    }

    out = fopen(argv[2], "w");
    if (!out) return 2;
    depth = 0;
    first_stack[0] = 1;
    o_open(NULL, '{');
    o_str("schema", "AIEN_RX_R15_SUMMARY_V1");
    o_str("raw_digest_sha256_of_SHA256SUMS", sums_hex);
    o_str("bootstrap", "percentile, 10000 resamples, splitmix64 seed 0x15 per comparison");
    o_open("files", '[');
    for (int i = 0; i < g_nfiles; i++) {
        o_open(NULL, '{'); o_str("name", g_files[i]); o_str("sha256", g_hash[i]); o_close('}');
    }
    o_close(']');

    static const char *const cfgs[4] = {"RES-4", "RES-1", "SEQ", "RES-1-NODIGEST"};

    /* ---- per trial ---- */
    int g1_ok = 1, compared = 0, silicon_trials = 0;
    char g1_why[256] = "";
    o_open("trials", '[');
    for (int i = 0; i < g_nt; i++) {
        Trial *t = &g_trials[i];
        char why[200] = "";
        int ok = trial_correct(t, why, sizeof why);
        compared++;
        silicon_trials += t->silicon;
        if (!ok && g1_ok) { g1_ok = 0; snprintf(g1_why, sizeof g1_why, "%s round %d: %s", t->config, t->round, why); }
        o_open(NULL, '{');
        o_str("file", t->file);
        o_str("config", t->config);
        o_num("round", t->round);
        o_bool("silicon", t->silicon);
        o_bool("correct", ok);
        o_str("why", why);
        o_num("before_ops_per_s", win_thr(t->win[W_BEFORE]));
        o_num("adapt_ops_per_s", adapt_rate(t));
        o_num("after_ops_per_s", win_thr(t->win[W_AFTER]));
        o_num("idle_power_w", idle_w(t));
        o_num("energy_uj_per_op_after_net", energy_per_op(t, W_AFTER));
        o_num("energy_uj_per_op_after_gross", gross_energy_per_op(t, W_AFTER));
        o_num("energy_uj_per_op_before_net", energy_per_op(t, W_BEFORE));
        o_num("gpu_domain_nvml_mj_after", win_nvml_mj(t->win[W_AFTER]));
        uint64_t g = ju(t->adapt, "goal_t"), met = ju(t->adapt, "met_t");
        o_num("episode_wall_ns", met > g && g ? (double)(met - g) : NAN);
        o_num("episode_energy_uj", g && met ? tel_energy(t, g, met) : NAN);
        o_num("cognition_ns", (double)ju(t->adapt, "plan_t") - (double)g);
        o_num("search_ns", jd(t->adapt, "select_t") - jd(t->adapt, "plan_t"));
        o_num("gpu_experiment_ns", jd(t->adapt, "evidence_t") - jd(t->adapt, "claim_t"));
        o_num("promotion_ns", jd(t->adapt, "inforce_t") - jd(t->adapt, "candidate_t"));
        o_num("adapt_cost_ns", jd(t->cwin[C_COST], "adapt_cost_ns"));
        o_num("gain_ns_per_op", (jd(t->adapt, "reference_ps") - jd(t->adapt, "selected_ps")) / 1000.0);
        o_num("crossover_ops", crossover_ops(t));
        o_num("crossover_s", crossover_ops(t) / win_thr(t->win[W_AFTER]));
        o_num("wake_amplification", wake_amp(t));
        o_num("commits_per_stimulus", per_cons(t, jd(jget(t->cwin[C_SPAN], "kinds"), "commit")));
        o_num("wasted_per_stimulus", v_wasted(t));
        o_num("replays", jd(t->cwin[C_SPAN], "replays"));
        const J *ca = t->cwin[C_ADAPT];
        double act = jd(ca, "activations");
        J *k = jget(ca, "kinds");
        o_num("invalidation_rate_adapt", act > 0 ? jd(k, "invalidated") / act : NAN);
        o_num("conflict_rate_adapt", act > 0 ? (jd(k, "invalidated") + jd(k, "rejected")) / act : NAN);
        const J *wa = t->win[W_AFTER];
        double ops = jd(wa, "ops");
        o_open("after_per_op", '{');
        static const char *const bytes[] = {"snapshot_bytes", "stage_bytes", "crumb_bytes", "proj_bytes",
            "c2g_write_bytes", "c2g_read_bytes", "g2c_write_bytes", "g2c_read_bytes", "window_move_bytes",
            "setup_copy_bytes"};
        for (size_t b = 0; b < sizeof bytes / sizeof bytes[0]; b++) o_num(bytes[b], stat(wa, bytes[b]) / ops);
        o_num("gen_store_bytes", jd(wa, "gen_store_bytes") / ops);
        o_num("cpu_ns", (jd(wa, "cpu_user_ns") + jd(wa, "cpu_sys_ns")) / ops);
        J *pm = jget(wa, "pmu");
        int pmu_ok = pm && (int)jd(pm, "ok") && !(int)jd(pm, "multiplexed");
        J *sum = jget(pm, "sum");
        o_num("cpu_cycles", pmu_ok ? jd(sum, "cpu_cycles") / ops : NAN);
        o_num("inst_retired", pmu_ok ? jd(sum, "inst_retired") / ops : NAN);
        o_num("dram_read_bytes_ll_miss_x64", pmu_ok ? jd(sum, "ll_cache_miss_rd") * 64 / ops : NAN);
        o_num("bus_bytes_bus_access_x64", pmu_ok ? jd(sum, "bus_access") * 64 / ops : NAN);
        o_num("l2d_cache_refill", pmu_ok ? jd(sum, "l2d_cache_refill") / ops : NAN);
        o_num("mem_access", pmu_ok ? jd(sum, "mem_access") / ops : NAN);
        J *sp = jget(wa, "sampler");
        o_num("r5_slots_mean", jd(sp, "intervals") > 0 ? jd(sp, "slot_sum") / jd(sp, "intervals") : NAN);
        o_num("r5_slots_peak", jd(sp, "slot_max"));
        o_num("resident_claims_peak", jd(sp, "claims_max"));
        o_close('}');
        J *tot = jget(t->residency, "total");
        o_num("gpu_residency", t->silicon && jd(tot, "intervals") > 0 ? jd(tot, "seat_live") / jd(tot, "intervals") : NAN);
        o_close('}');
    }
    o_close(']');

    /* ---- comparisons ---- */
    Cmp c_after_1s = paired("RES-1", "SEQ", v_after_thr, 1);
    Cmp c_after_4s = paired("RES-4", "SEQ", v_after_thr, 1);
    Cmp c_before_1s = paired("RES-1", "SEQ", v_before_thr, 0);
    Cmp c_adapt_4s = paired("RES-4", "SEQ", v_adapt_rate, 0);
    Cmp c_adapt_1s = paired("RES-1", "SEQ", v_adapt_rate, 0);
    Cmp c_nd = paired("RES-1", "RES-1-NODIGEST", v_after_thr, 1);
    Cmp c_energy = paired("RES-1", "SEQ", v_energy_after, 1);
    Cmp c_wasted = paired("RES-1", "SEQ", v_wasted, 0);
    Cmp c_after_41 = paired("RES-4", "RES-1", v_after_thr, 1);
    o_open("comparisons", '{');
    o_cmp("after_throughput_RES-1_over_SEQ", &c_after_1s);
    o_cmp("after_throughput_RES-4_over_SEQ", &c_after_4s);
    o_cmp("after_throughput_RES-4_over_RES-1", &c_after_41);
    o_cmp("before_throughput_RES-1_over_SEQ", &c_before_1s);
    o_cmp("adapt_rate_RES-4_over_SEQ", &c_adapt_4s);
    o_cmp("adapt_rate_RES-1_over_SEQ", &c_adapt_1s);
    o_cmp("after_throughput_RES-1_over_NODIGEST", &c_nd);
    o_cmp("energy_per_op_after_RES-1_over_SEQ", &c_energy);
    o_cmp("wasted_per_stimulus_RES-1_over_SEQ", &c_wasted);
    o_close('}');

    /* ---- metrics 1-17 ---- */
    int have[18] = {0};
    o_open("metrics", '{');
    o_open("m1_semantic_ops_per_s", '{');
    for (int c = 0; c < 4; c++) {
        size_t n;
        o_open(cfgs[c], '{');
        o_num("before_median", med_of(cfgs[c], v_before_thr, &n));
        o_num("adapt_median", med_of(cfgs[c], v_adapt_rate, NULL));
        o_num("after_median", med_of(cfgs[c], v_after_thr, NULL));
        o_u("trials", n);
        o_close('}');
        if (n) have[1] = 1;
    }
    o_close('}');
    Pct a_res = pct_of(l1_get("A", "latency_ns", 0, "RES-1")), a_seq = pct_of(l1_get("A", "latency_ns", 0, "SEQ"));
    o_open("m2_activation_latency_ns", '{');
    o_pct("L1A_RES-1", a_res);
    o_pct("L1A_SEQ", a_seq);
    for (int c = 0; c < 4; c++) {
        char k[64];
        snprintf(k, sizeof k, "trial_crumbs_%s", cfgs[c]);
        o_pct(k, hist_pct(cfgs[c], C_SPAN));
    }
    o_close('}');
    have[2] = a_res.n > 0;
    o_open("m3_propagation_ns", '{');
    static const uint64_t fan[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    for (size_t f = 0; f < 9; f++) {
        char k[32];
        snprintf(k, sizeof k, "fanout_%" PRIu64, fan[f]);
        o_open(k, '{');
        Pct w = pct_of(l1_get("B", "wave_wall_ns", fan[f], "RES-1")), c = pct_of(l1_get("B", "wave_cpu_ns", fan[f], "RES-1"));
        o_pct("wave_wall", w);
        o_pct("wave_cpu", c);
        o_num("wall_per_dependent_p50", (double)w.p50 / (double)fan[f]);
        o_num("cpu_per_dependent_p50", (double)c.p50 / (double)fan[f]);
        o_close('}');
        if (w.n) have[3] = 1;
    }
    o_num("failed_fanouts", g_l1_b_failed);
    o_close('}');
    o_open("m4_scheduler_ns", '{');
    static const char *const cm[] = {"C_uncontended", "C_blocked", "C_priority"};
    static const char *const cf[] = {"sched_cpu_ns", "sched_wall_ns", "ready_to_run_ns", "demand_to_ready_ns"};
    for (int c = 0; c < 2; c++) {
        o_open(cfgs[c + 1], '{');
        for (int m = 0; m < 3; m++)
            for (int f = 0; f < 4; f++) {
                char k[64];
                snprintf(k, sizeof k, "%s.%s", cm[m], cf[f]);
                Pct p = pct_of(l1_get(cm[m], cf[f], 0, cfgs[c + 1]));
                o_pct(k, p);
                if (p.n) have[4] = 1;
            }
        o_close('}');
    }
    o_close('}');
    /* E: per-check ns from batches of 100 */
    Pct e_res = {0};
    {
        Vu *b = l1_get("E", "batch100_ns", 0, "RES-1");
        Vu per = {0};
        for (size_t i = 0; b && i < b->n; i++) vu_push(&per, b->v[i] / 100);
        e_res = pct_of(&per);
        free(per.v);
    }
    o_open("m5_aegis_fast_path_ns", '{');
    o_pct("validate_per_check_RES-1", e_res);
    o_pct("activation_with_grant_RES-1", pct_of(l1_get("E_activation", "run_to_visible_ns", 0, "RES-1")));
    o_pct("activation_with_grant_SEQ", pct_of(l1_get("E_activation", "run_to_visible_ns", 0, "SEQ")));
    o_close('}');
    have[5] = e_res.n > 0;
    /* G: barriers */
    Vu blat = {0};
    size_t bar_n = 0, bar_live = 0, bar_fail = 0;
    for (size_t i = 0; i < g_nbar; i++) {
        if (g_bar[i].rc != 0 || !g_bar[i].receipt) { bar_fail++; continue; }
        vu_push(&blat, g_bar[i].receipt - g_bar[i].enter);
        bar_n++;
        if (g_bar[i].commits > 0) bar_live++;
    }
    Pct bp = pct_of(&blat);
    o_open("m6_generation_barrier", '{');
    o_pct("latency_ns", bp);
    o_u("barriers", bar_n);
    o_u("barriers_failed", bar_fail);
    o_num("fraction_with_production_commits", bar_n ? (double)bar_live / (double)bar_n : NAN);
    /* Reported only (C5): the same barriers split by configuration and by
     * source (per-trial promotion vs L1-G). G7 is gated on the totals above. */
    o_open("by_configuration", '{');
    for (int src = 0; src < 2; src++) {
        o_open(src ? "trial" : "l1_g", '{');
        for (int c = 0; c < 4; c++) {
            Vu lat = {0};
            size_t n = 0, live = 0;
            for (size_t i = 0; i < g_nbar; i++) {
                const Barrier *b = &g_bar[i];
                if (b->trial != src || strcmp(b->config, cfgs[c]) || b->rc != 0 || !b->receipt)
                    continue;
                vu_push(&lat, b->receipt - b->enter);
                n++;
                if (b->commits > 0) live++;
            }
            if (!n) continue;
            o_open(cfgs[c], '{');
            o_u("barriers", n);
            o_u("with_production_commits", live);
            o_num("fraction", (double)live / (double)n);
            o_pct("latency_ns", pct_of(&lat));
            o_close('}');
        }
        o_close('}');
    }
    o_close('}');
    o_close('}');
    have[6] = bar_n > 0;
    o_open("m7_copied_bytes_per_op_after", '{');
    static const char *const bk[] = {"snapshot_bytes", "stage_bytes", "crumb_bytes", "proj_bytes",
        "c2g_write_bytes", "c2g_read_bytes", "g2c_write_bytes", "g2c_read_bytes", "window_move_bytes",
        "setup_copy_bytes", "gen_store_bytes"};
    for (int c = 0; c < 4; c++) {
        o_open(cfgs[c], '{');
        for (size_t b = 0; b < sizeof bk / sizeof bk[0]; b++) {
            Vd v = {0};
            for (int i = 0; i < g_nt; i++) {
                const Trial *t = &g_trials[i];
                if (!is_cfg(t, cfgs[c]) || !t->win[W_AFTER]) continue;
                double ops = jd(t->win[W_AFTER], "ops");
                double x = b == 10 ? jd(t->win[W_AFTER], "gen_store_bytes") : stat(t->win[W_AFTER], bk[b]);
                if (ops > 0 && !isnan(x)) vd_push(&v, x / ops);
            }
            o_num(bk[b], median_d(v.v, v.n));
            if (v.n) have[7] = 1;
            free(v.v);
        }
        o_close('}');
    }
    o_str("chip_transferred_bytes", "derived: claims x 128 B descriptor read + results x 128 B written + per-claim window/heartbeat bytes (see receipt)");
    o_close('}');
    /* L2 */
    double sync_med[2] = {NAN, NAN}, spin_med[2] = {NAN, NAN};
    o_open("m8_cpu_gpu", '{');
    for (int c = 0; c < 2; c++) {
        const char *cfg = cfgs[c + 1];
        Vd syn = {0}, spin = {0};
        Vu e2e = {0}, gpu = {0}, enq = {0}, pub = {0}, dep = {0};
        int runs = 0, failed = 0;
        for (int i = 0; i < g_nl2; i++) {
            L2Run *l = &g_l2[i];
            if (strcmp(l->config, cfg)) continue;
            runs++;
            if (l->exit_code != 0) failed++;
            const J *w = l->window;
            double res = stat(w, "gpu_results_taken");
            /* Amendment A1 (G10, Drake 2026-09-28): synchronization EVENTS only. Empty
             * completion polls are host spin cost, reported below, not gated. */
            double s = stat(w, "resident_claims") + 2 * res + stat(w, "gpu_host_waits");
            if (res > 0) vd_push(&syn, s / res);
            if (res > 0) vd_push(&spin, stat(w, "gpu_polls_empty") / res);
            for (size_t j = 0; j < l->e2e.n; j++) vu_push(&e2e, l->e2e.v[j]);
            for (size_t j = 0; j < l->gpu.n; j++) vu_push(&gpu, l->gpu.v[j]);
            for (size_t j = 0; j < l->enq.n; j++) vu_push(&enq, l->enq.v[j]);
            for (size_t j = 0; j < l->pub.n; j++) vu_push(&pub, l->pub.v[j]);
            for (size_t j = 0; j < l->dep.n; j++) vu_push(&dep, l->dep.v[j]);
        }
        sync_med[c] = median_d(syn.v, syn.n);
        spin_med[c] = median_d(spin.v, spin.n);
        o_open(cfg, '{');
        o_num("runs", runs);
        o_num("runs_failed", failed);
        o_num("syncs_per_gpu_result_median", sync_med[c]);
        o_str("sync_definition", "synchronization events per result (spec amendment A1): claim notices + completion notices taken + seat fences (1 per result) + host blocking waits; empty completion polls excluded");
        o_num("host_spin_empty_polls_per_gpu_result_median", spin_med[c]);
        o_str("host_spin_definition", "reported, not gated (amendment A1): host completion polls that found nothing, per GPU result");
        o_pct("enqueue_ns", pct_of(&enq));
        o_pct("gpu_exec_ns_chip_clock", pct_of(&gpu));
        o_pct("completion_publication_ns", pct_of(&pub));
        o_pct("dependent_wake_ns", pct_of(&dep));
        o_pct("end_to_end_ns", pct_of(&e2e));
        o_close('}');
        if (syn.n) have[8] = 1;
        free(syn.v); free(spin.v); free(e2e.v); free(gpu.v); free(enq.v); free(pub.v); free(dep.v);
    }
    o_close('}');
    /* m9 memory traffic, AFTER per op */
    o_open("m9_memory_traffic_per_op_after", '{');
    static const char *const pe[] = {"ll_cache_miss_rd", "bus_access", "l2d_cache_refill", "mem_access", "cpu_cycles", "inst_retired"};
    for (int c = 0; c < 4; c++) {
        o_open(cfgs[c], '{');
        size_t used = 0, mux = 0;
        for (size_t e = 0; e < 6; e++) {
            Vd v = {0};
            for (int i = 0; i < g_nt; i++) {
                const Trial *t = &g_trials[i];
                if (!is_cfg(t, cfgs[c]) || !t->win[W_AFTER]) continue;
                J *pm = jget(t->win[W_AFTER], "pmu");
                if (!pm || !(int)jd(pm, "ok")) continue;
                if ((int)jd(pm, "multiplexed")) { if (e == 0) mux++; continue; }
                vd_push(&v, jd(jget(pm, "sum"), pe[e]) / jd(t->win[W_AFTER], "ops"));
            }
            o_num(pe[e], median_d(v.v, v.n));
            if (e == 0) used = v.n;
            free(v.v);
        }
        o_u("windows_used", used);
        o_u("windows_multiplexed_excluded", mux);
        o_str("derivation", "DRAM read bytes = ll_cache_miss_rd x 64 B; bus bytes = bus_access x 64 B; raw sum over both CPU PMUs, never scaled");
        o_close('}');
        if (used) have[9] = 1;
    }
    o_close('}');
    /* m10 residency */
    double res_min = NAN;
    size_t res_n = 0;
    J *res_worst = NULL;   /* the process behind res_min; used only for the diagnostic below */
    char res_worst_name[300] = "";
    for (int i = 0; i < g_nt; i++) {
        J *tot = jget(g_trials[i].residency, "total");
        if (!g_trials[i].silicon || !(jd(tot, "intervals") > 0)) continue;
        double r = jd(tot, "seat_live") / jd(tot, "intervals");
        if (isnan(res_min) || r < res_min) {
            res_min = r; res_worst = tot;
            snprintf(res_worst_name, sizeof res_worst_name, "%.250s", g_trials[i].file);
        }
        res_n++;
    }
    for (int i = 0; i < g_nl2; i++) {
        J *tot = jget(g_l2[i].residency, "total");
        if (!g_l2[i].silicon || !(jd(tot, "intervals") > 0)) continue;
        double r = jd(tot, "seat_live") / jd(tot, "intervals");
        if (isnan(res_min) || r < res_min) {
            res_min = r; res_worst = tot;
            snprintf(res_worst_name, sizeof res_worst_name, "l2-%.20s-%d", g_l2[i].config, i);
        }
        res_n++;
    }
    o_open("m10_gpu_residency", '{');
    o_num("min_fraction_live", res_min);
    o_u("silicon_processes", res_n);
    o_residency_diagnostic(res_worst, res_worst_name);
    o_close('}');
    have[10] = res_n > 0;
    /* m11 resource utilization */
    o_open("m11_resources_after", '{');
    for (int c = 0; c < 4; c++) {
        Vd cpu = {0}, slots = {0};
        for (int i = 0; i < g_nt; i++) {
            const Trial *t = &g_trials[i];
            if (!is_cfg(t, cfgs[c]) || !t->win[W_AFTER]) continue;
            const J *w = t->win[W_AFTER];
            vd_push(&cpu, (jd(w, "cpu_user_ns") + jd(w, "cpu_sys_ns")) / jd(w, "wall_ns"));
            J *sp = jget(w, "sampler");
            if (jd(sp, "intervals") > 0) vd_push(&slots, jd(sp, "slot_sum") / jd(sp, "intervals"));
        }
        o_open(cfgs[c], '{');
        o_num("process_cpus_busy_median", median_d(cpu.v, cpu.n));
        o_num("r5_slots_mean_median", median_d(slots.v, slots.n));
        o_close('}');
        if (cpu.n) have[11] = 1;
        free(cpu.v); free(slots.v);
    }
    o_str("per_thread", "per-thread CPU ticks are in each window's threads0/threads1 in the raw records");
    o_close('}');
    o_open("m12_13_14_15_reactions", '{');
    for (int c = 0; c < 4; c++) {
        o_open(cfgs[c], '{');
        o_num("wasted_per_stimulus_median", med_of(cfgs[c], v_wasted, NULL));
        Vd inv = {0}, con = {0}, amp = {0};
        for (int i = 0; i < g_nt; i++) {
            const Trial *t = &g_trials[i];
            if (!is_cfg(t, cfgs[c]) || !t->cwin[C_ADAPT]) continue;
            double act = jd(t->cwin[C_ADAPT], "activations");
            J *k = jget(t->cwin[C_ADAPT], "kinds");
            if (act > 0) {
                vd_push(&inv, jd(k, "invalidated") / act);
                vd_push(&con, (jd(k, "invalidated") + jd(k, "rejected")) / act);
            }
            double a = wake_amp(t);
            if (!isnan(a)) vd_push(&amp, a);
        }
        o_num("invalidation_rate_adapt_median", median_d(inv.v, inv.n));
        o_num("conflict_rate_adapt_median", median_d(con.v, con.n));
        o_num("wake_amplification_median", median_d(amp.v, amp.n));
        if (inv.n) have[12] = have[13] = have[14] = 1;
        if (amp.n) have[15] = 1;
        free(inv.v); free(con.v); free(amp.v);
        o_close('}');
    }
    o_close('}');
    o_open("m16_causal_trace_overhead", '{');
    o_cmp("after_throughput_RES-1_over_NODIGEST", &c_nd);
    {
        Vd cy = {0};
        for (int i = 0; i < g_nt; i++) {
            const Trial *a = &g_trials[i];
            if (!is_cfg(a, "RES-1")) continue;
            const Trial *b = find_trial("RES-1-NODIGEST", a->round);
            if (!b || !same_realization(a, b)) continue;
            J *pa = jget(a->win[W_AFTER], "pmu"), *pb = jget(b->win[W_AFTER], "pmu");
            if (!pa || !pb || !(int)jd(pa, "ok") || !(int)jd(pb, "ok") || (int)jd(pa, "multiplexed") || (int)jd(pb, "multiplexed")) continue;
            double x = jd(jget(pa, "sum"), "cpu_cycles") / jd(a->win[W_AFTER], "ops");
            double y = jd(jget(pb, "sum"), "cpu_cycles") / jd(b->win[W_AFTER], "ops");
            if (y > 0) vd_push(&cy, x / y);
        }
        o_num("cycles_per_op_ratio_median", median_d(cy.v, cy.n));
        free(cy.v);
    }
    o_close('}');
    have[16] = c_nd.n > 0;
    o_open("m17_energy", '{');
    size_t en_n = 0;
    for (int c = 0; c < 4; c++) {
        Vd ep = {0}, eg = {0}, ee = {0}, nv = {0};
        for (int i = 0; i < g_nt; i++) {
            const Trial *t = &g_trials[i];
            if (!is_cfg(t, cfgs[c])) continue;
            double x = energy_per_op(t, W_AFTER);
            if (!isnan(x)) vd_push(&ep, x);
            x = gross_energy_per_op(t, W_AFTER);
            if (!isnan(x)) vd_push(&eg, x);
            uint64_t g = ju(t->adapt, "goal_t"), met = ju(t->adapt, "met_t");
            x = g && met ? tel_energy(t, g, met) : NAN;
            if (!isnan(x)) vd_push(&ee, x);
            x = win_nvml_mj(t->win[W_AFTER]) / jd(t->win[W_AFTER], "ops");
            if (!isnan(x)) vd_push(&nv, x);
        }
        o_open(cfgs[c], '{');
        o_num("pkg_uj_per_op_after_net_median", median_d(ep.v, ep.n));
        o_num("pkg_uj_per_op_after_gross_median", median_d(eg.v, eg.n));
        o_num("pkg_uj_per_episode_median", median_d(ee.v, ee.n));
        o_num("gpu_domain_nvml_mj_per_op_after_median", median_d(nv.v, nv.n));
        o_u("windows", ep.n);
        o_close('}');
        en_n += ep.n;
        free(ep.v); free(eg.v); free(ee.v); free(nv.v);
    }
    o_cmp("energy_per_op_after_RES-1_over_SEQ", &c_energy);
    o_str("uncertainty", "1 mJ resolution; +-1 sample period (100 ms) of power at each window edge; accumulator vs power discrepancy up to 0.5 W; no external meter calibration (spec §17)");
    o_close('}');
    have[17] = en_n > 0;
    o_open("instrumentation_overhead", '{');
    for (int c = 0; c < 2; c++) {
        Vd on = {0}, off = {0};
        for (int i = 0; i < g_nw; i++) {
            if (strcmp(g_w[i].config, cfgs[c + 1])) continue;
            vd_push(g_w[i].timing ? &on : &off, g_w[i].thr);
        }
        o_num(cfgs[c + 1], median_d(on.v, on.n) / median_d(off.v, off.n));
        free(on.v); free(off.v);
    }
    o_close('}');
    o_close('}');   /* metrics */

    /* ---- gates ---- */
    char note[200];
    int enough = 1;
    for (int c = 0; c < 4; c++) {
        size_t n = 0;
        for (int i = 0; i < g_nt; i++) n += is_cfg(&g_trials[i], cfgs[c]);
        if (n < 12) enough = 0;
    }
    snprintf(note, sizeof note, "%s%s", g1_why, enough ? "" : (g1_why[0] ? "; fewer than 12 trials of a configuration" : "fewer than 12 trials of a configuration"));
    gate(0, "G1", "correctness in every trial", "all correct", compared, g1_ok && enough && compared > 0, note);
    snprintf(note, sizeof note, "%zu valid pairs (need >= 10)", c_after_1s.n);
    gate(1, "G2", "AFTER throughput RES-1/SEQ", "median >= 0.90 and CI lo >= 0.85", c_after_1s.med,
         c_after_1s.n >= 10 && c_after_1s.med >= 0.90 && c_after_1s.lo >= 0.85, note);
    gate(2, "G3", "ADAPT rate RES-4/SEQ", "CI lo > 1.0", c_adapt_4s.lo, c_adapt_4s.n >= 2 && c_adapt_4s.lo > 1.0, "");
    size_t n4;
    double g4 = NAN;
    {
        Vd v = {0};
        for (int i = 0; i < g_nt; i++) {
            const Trial *t = &g_trials[i];
            if (!is_cfg(t, "RES-4")) continue;
            double a = adapt_rate(t), b = win_thr(t->win[W_BEFORE]);
            if (!isnan(a) && b > 0) vd_push(&v, a / b);
        }
        g4 = median_d(v.v, v.n);
        n4 = v.n;
        free(v.v);
    }
    gate(3, "G4", "RES-4 ADAPT rate / BEFORE rate", "median >= 0.50", g4, n4 > 0 && g4 >= 0.50, "");
    snprintf(note, sizeof note, "p50 %" PRIu64 " ns, p99 %" PRIu64 " ns, n %" PRIu64, a_res.p50, a_res.p99, a_res.n);
    gate(4, "G5", "L1-A activation latency RES-1", "p50 <= 50 us and p99 <= 221 us", (double)a_res.p99,
         a_res.n > 0 && a_res.p50 <= 50000 && a_res.p99 <= 221000 && !g_l1_timing_bad, note);
    snprintf(note, sizeof note, "p50 %" PRIu64 " ns, p99 %" PRIu64 " ns per check (batch mean)", e_res.p50, e_res.p99);
    gate(5, "G6", "L1-E AEGIS fast path", "p50 <= 1 us and p99 <= 5 us", (double)e_res.p99,
         e_res.n > 0 && e_res.p50 <= 1000 && e_res.p99 <= 5000, note);
    double live_frac = bar_n ? (double)bar_live / (double)bar_n : NAN;
    snprintf(note, sizeof note, "%zu barriers, %zu failed, p99 %" PRIu64 " ns", bar_n, bar_fail, bp.p99);
    gate(6, "G7", "generation barrier", "commits during >= 90% of barriers; p99 <= 500 ms", live_frac,
         bar_n > 0 && !bar_fail && live_frac >= 0.90 && bp.p99 <= 500000000ull, note);
    snprintf(note, sizeof note, "%zu valid pairs", c_nd.n);
    gate(7, "G8", "AFTER throughput RES-1/NODIGEST", "median >= 0.50", c_nd.med, c_nd.n >= 10 && c_nd.med >= 0.50, note);
    snprintf(note, sizeof note, "%zu valid pairs", c_energy.n);
    gate(8, "G9", "energy per op AFTER RES-1/SEQ (package, net)", "median <= 1.15 and CI hi <= 1.25",
         c_energy.med, c_energy.n >= 10 && c_energy.med <= 1.15 && c_energy.hi <= 1.25, note);
    snprintf(note, sizeof note, "RES-1 %.3f, SEQ %.3f sync events per result (host spin, not gated: RES-1 %.1f, SEQ %.1f empty polls per result)", sync_med[0], sync_med[1], spin_med[0], spin_med[1]);
    gate(9, "G10", "CPU-GPU synchronization events per GPU result (amendment A1)", "RES-1 <= SEQ", sync_med[0],
         !isnan(sync_med[0]) && !isnan(sync_med[1]) && sync_med[0] <= sync_med[1], note);
    size_t nco;
    double co = med_of("RES-4", crossover_ops, &nco);
    gate(10, "G11", "adaptation crossover RES-4", "median <= 10,000,000 ops", co, nco > 0 && co <= 1e7, "");
    double wa4 = med_of("RES-4", wake_amp, NULL), was = med_of("SEQ", wake_amp, NULL);
    snprintf(note, sizeof note, "RES-4 %.3f, SEQ %.3f per consequential stimulus", wa4, was);
    gate(11, "G12", "wake amplification RES-4 vs SEQ", "RES-4 <= SEQ", wa4, !isnan(wa4) && !isnan(was) && wa4 <= was, note);
    gate(12, "G13", "wasted reactions RES-1/SEQ", "median ratio <= 1.10", c_wasted.med, c_wasted.n > 0 && c_wasted.med <= 1.10, "");
    double cr = NAN;
    {
        Vd v = {0};
        for (int i = 0; i < g_nt; i++) {
            const Trial *t = &g_trials[i];
            if (!is_cfg(t, "RES-4") || !t->cwin[C_ADAPT]) continue;
            double act = jd(t->cwin[C_ADAPT], "activations");
            J *k = jget(t->cwin[C_ADAPT], "kinds");
            if (act > 0) vd_push(&v, (jd(k, "invalidated") + jd(k, "rejected")) / act);
        }
        cr = median_d(v.v, v.n);
        free(v.v);
    }
    gate(13, "G14", "conflict rate RES-4 ADAPT", "median <= 25%", cr, !isnan(cr) && cr <= 0.25, "");
    snprintf(note, sizeof note, "%zu silicon processes (trials + L2); %d silicon trials", res_n, silicon_trials);
    gate(14, "G15", "GPU residency", ">= 99% of samples live", res_min, res_n > 0 && silicon_trials > 0 && res_min >= 0.99, note);
    int all17 = 1;
    char miss[200] = "";
    for (int m = 1; m <= 17; m++)
        if (!have[m]) { all17 = 0; size_t L = strlen(miss); snprintf(miss + L, sizeof miss - L, "%s%d", L ? "," : "missing ", m); }
    gate(15, "G16", "metrics 1-17 present, reduced from raw", "all present", all17, all17, miss);

    int all = 1;
    o_open("gates", '[');
    for (int i = 0; i < 16; i++) {
        o_open(NULL, '{');
        o_str("id", g_gate[i].id);
        o_str("criterion", g_gate[i].what);
        o_str("threshold", g_gate[i].threshold);
        o_num("value", g_gate[i].value);
        o_str("outcome", g_gate[i].pass ? "PASS" : "FAIL");
        o_str("note", g_gate[i].note);
        o_close('}');
        if (!g_gate[i].pass) all = 0;
    }
    o_close(']');
    o_open("inputs_seen", '{');
    o_num("trials", g_nt);
    o_num("l1_processes", g_l1_runs);
    o_num("l1_processes_failed", g_l1_failed_runs);
    o_num("l1_timing_buffers_truncated", g_l1_timing_bad);
    o_num("l2_processes", g_nl2);
    o_close('}');
    o_bool("all_gates_pass", all);
    o_close('}');
    fputc('\n', out);
    fclose(out);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "%-4s %-4s %s\n", g_gate[i].id, g_gate[i].pass ? "PASS" : "FAIL", g_gate[i].note);
    fprintf(stderr, "all gates: %s\n", all ? "PASS" : "FAIL");
    return 0;
}
