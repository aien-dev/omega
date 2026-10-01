/* EST-3c protocol v3 shared tool code. See est3c_common.h for the documented
 * replay, tick, burn-in, E0 and statistic choices. */
#include "est3c_common.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"

#define NS 1000000000ll

static void msg(char *err, size_t cap, const char *fmt, const char *a)
{
    if (err && cap) snprintf(err, cap, fmt, a);
}

/* ------------------------------------------------------------------ fields */

int c3_json_field(const char *s, size_t len, const char *key, const char **v, size_t *vl)
{
    size_t i = 0, kl = strlen(key);
#define WS() while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++
    WS();
    if (i >= len || s[i] != '{') return 1;
    i++;
    for (;;) {
        WS();
        if (i >= len || s[i] != '"') return 1;
        size_t ks = ++i;
        while (i < len && s[i] != '"') { if (s[i] == '\\') return 1; i++; }
        if (i >= len) return 1;
        size_t ke = i++;
        WS();
        if (i >= len || s[i] != ':') return 1;
        i++;
        WS();
        if (i >= len) return 1;
        size_t vs, ve;
        if (s[i] == '"') {
            /* a string value is skipped by looking only for its closing quote;
             * its bytes are never converted unless this is the asked key */
            vs = ++i;
            while (i < len && s[i] != '"') { if (s[i] == '\\') i++; i++; }
            if (i >= len) return 1;
            ve = i++;
        } else {
            if (s[i] == '{' || s[i] == '[') return 1;
            vs = i;
            while (i < len && s[i] != ',' && s[i] != '}') i++;
            ve = i;
            while (ve > vs && s[ve - 1] == ' ') ve--;
        }
        if (ke - ks == kl && memcmp(s + ks, key, kl) == 0) { *v = s + vs; *vl = ve - vs; return 0; }
        WS();
        if (i < len && s[i] == ',') { i++; continue; }
        return 1;
    }
#undef WS
}

/* zone 0 = first space-separated token of "thermal_mc". present = 0 when the
 * key or the token is absent; value NaN when the token is not a number. */
static void thermal0(const char *ln, size_t ll, int *present, double *value)
{
    const char *v; size_t vl;
    *present = 0; *value = NAN;
    if (c3_json_field(ln, ll, "thermal_mc", &v, &vl)) return;
    size_t a = 0;
    while (a < vl && v[a] == ' ') a++;
    size_t b = a;
    while (b < vl && v[b] != ' ') b++;
    if (b == a) return;
    *present = 1;
    char tok[48];
    if (b - a >= sizeof tok) return;
    memcpy(tok, v + a, b - a); tok[b - a] = 0;
    for (size_t i = 0; tok[i]; i++) {
        char c = tok[i];
        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E'
              || c == 'i' || c == 'n' || c == 'f' || c == 'a' || c == 'I' || c == 'N' || c == 'F' || c == 'A')) return;
    }
    char *end; errno = 0;
    double x = strtod(tok, &end);
    if (*end != 0) return;
    *value = x;      /* may be inf / nan: classified NONFINITE by est_pred */
}

/* ------------------------------------------------------------------- ticks */

static int push_tick(c3_ticks *tk, const c3_tick *t)
{
    if (tk->n == tk->cap) {
        size_t nc = tk->cap ? tk->cap * 2 : 4096;
        c3_tick *nt = realloc(tk->t, nc * sizeof *nt);
        if (!nt) return 1;
        tk->t = nt; tk->cap = nc;
    }
    tk->t[tk->n++] = *t;
    return 0;
}

int c3_ticks_build(const est_file *f, c3_ticks *tk, char *err, size_t cap)
{
    memset(tk, 0, sizeof *tk);
    tk->lines = f->nlines;
    int64_t prev = 0; int prev_ok = 0;
    static const char tag[] = "est3c.inserted-missing";
    for (size_t li = 0; li < f->nlines; li++) {
        const char *ln = (const char *)f->data + f->off[li];
        size_t ll = f->llen[li];
        int64_t w = 0;
        int t_ok = est_parse_t(ln, ll, &w) == 0;
        if (!t_ok) tk->bad_t++;
        uint64_t h = 1;
        if (t_ok && prev_ok) {
            int64_t gap = w - prev;
            if (gap == 0) tk->gap_zero++;
            else if (gap < 0) tk->gap_backward++;
            else {
                int64_t hr = (gap + NS / 2) / NS;
                if (hr < 1) hr = 1;
                h = (uint64_t)hr;
            }
        }
        if (t_ok) { prev = w; prev_ok = 1; }
        else if (prev_ok) prev += NS;      /* unparsable t: one nominal step */
        if (h > 1) {
            tk->lines_multi++;
            uint64_t ins = h - 1;
            if (ins > C3_MAX_INSERT) { ins = C3_MAX_INSERT; tk->gaps_capped++; }
            for (uint64_t j = 0; j < ins; j++) {
                c3_tick t; memset(&t, 0, sizeof t);
                t.line = li; t.inserted = 1; t.value = NAN;
                sha256_ctx c; sha256_init(&c);
                sha256_update(&c, (const uint8_t *)tag, sizeof tag); /* includes the 0x00 */
                sha256_update(&c, (const uint8_t *)ln, ll);
                uint8_t le[8]; for (int k = 0; k < 8; k++) le[k] = (uint8_t)(j >> (8 * k));
                sha256_update(&c, le, 8);
                sha256_final(&c, t.ev.b);
                if (push_tick(tk, &t)) { msg(err, cap, "out of memory%s", ""); return 1; }
                tk->inserted++;
            }
        }
        c3_tick t; memset(&t, 0, sizeof t);
        t.line = li; t.t_ok = t_ok; t.wall_ns = t_ok ? w : 0;
        thermal0(ln, ll, &t.present, &t.value);
        if (!t.present) tk->absent_value++;
        else if (!isfinite(t.value)) tk->unparsable_value++;
        if (!t_ok) { t.present = 0; }      /* v1: a line without a usable t is missing */
        sha256_hash((const uint8_t *)ln, ll, t.ev.b);
        if (push_tick(tk, &t)) { msg(err, cap, "out of memory%s", ""); return 1; }
    }
    return 0;
}

void c3_ticks_free(c3_ticks *tk) { free(tk->t); memset(tk, 0, sizeof *tk); }

/* ------------------------------------------------------------------- marks */

static int parse_name(const char *s, size_t n, int *level, int *index)
{
    *level = -1; *index = -1;
    if (n < 6 || memcmp(s, "load-L", 6) != 0) return 1;
    size_t i = 6; long lv = 0, ix = 0; size_t d = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && d < 6) { lv = lv * 10 + (s[i] - '0'); i++; d++; }
    if (d == 0 || i >= n || s[i] != '-') return 1;
    i++; d = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && d < 6) { ix = ix * 10 + (s[i] - '0'); i++; d++; }
    if (d == 0 || (i < n && s[i] != ' ')) return 1;
    *level = (int)lv; *index = (int)ix;
    return 0;
}

int c3_marks_load(const char *path, int verify, c3_marks *mk, char *err, size_t cap)
{
    memset(mk, 0, sizeof *mk);
    est_file f;
    if (verify ? est_file_load(path, &f, err, cap) : est_file_load_raw(path, &f, err, cap)) return 1;
    memcpy(mk->sha, f.sha_hex, 65);
    int64_t open_t = 0; int is_open = 0, open_lv = -1, open_ix = -1;
    for (size_t i = 0; i < f.nlines; i++) {
        const char *ln = (const char *)f.data + f.off[i]; size_t ll = f.llen[i];
        if (ll == 0) continue;
        int64_t t; size_t used;
        if (est_parse_time_prefix(ln, ll, &t, &used)) { mk->bad_lines++; continue; }
        const char *rest = ln + used; size_t rl = ll - used;
        int is_b = rl >= 7 && memcmp(rest, " begin ", 7) == 0;
        int is_e = !is_b && rl >= 5 && memcmp(rest, " end ", 5) == 0;
        if (!is_b && !is_e) { mk->bad_lines++; continue; }
        int lv, ix;
        size_t sk = is_b ? 7 : 5;
        parse_name(rest + sk, rl - sk, &lv, &ix);
        if (mk->nev < C3_MAX_EV) {
            mk->ev_t[mk->nev] = t; mk->ev_end[mk->nev] = is_e; mk->ev_level[mk->nev] = lv; mk->ev_index[mk->nev] = ix;
            mk->nev++;
        } else mk->bad_lines++;
        if (is_b) {
            mk->begins++;
            if (is_open) {
                mk->nested_begins++;
                if (mk->n >= C3_MAX_MARKS) { msg(err, cap, "too many marks pairs%s", ""); est_file_free(&f); return 1; }
                mk->b[mk->n] = open_t; mk->e[mk->n] = t; mk->level[mk->n] = open_lv; mk->index[mk->n] = open_ix; mk->n++;
            }
            open_t = t; is_open = 1; open_lv = lv; open_ix = ix;
        } else {
            mk->ends++;
            if (!is_open) { mk->stray_ends++; continue; }
            if (mk->n >= C3_MAX_MARKS) { msg(err, cap, "too many marks pairs%s", ""); est_file_free(&f); return 1; }
            mk->b[mk->n] = open_t; mk->e[mk->n] = t; mk->level[mk->n] = open_lv; mk->index[mk->n] = open_ix; mk->n++;
            is_open = 0;
        }
    }
    if (is_open) {
        if (mk->n >= C3_MAX_MARKS) { msg(err, cap, "too many marks pairs%s", ""); est_file_free(&f); return 1; }
        mk->b[mk->n] = open_t; mk->e[mk->n] = INT64_MAX; mk->level[mk->n] = open_lv; mk->index[mk->n] = open_ix; mk->n++;
        mk->unclosed = 1;
    }
    est_file_free(&f);
    return 0;
}

int c3_in_trial(const c3_marks *mk, int64_t t)
{
    if (!mk) return 0;
    for (size_t i = 0; i < mk->n; i++) if (t >= mk->b[i] && t <= mk->e[i]) return 1;
    return 0;
}

/* ------------------------------------------------------------------ models */

const char *c3_name(int id)
{
    static const char *n[6] = { "E0", "F1", "F2", "F3", "F4", "F5" };
    return (id >= 0 && id <= 5) ? n[id] : "??";
}

void c3_assumption_base(est_assumption *a, est_family fam)
{
    memset(a, 0, sizeof *a);
    a->family = fam; a->unit = EST_UNIT_MILLI_CELSIUS;
    a->quantum = C3_QUANTUM; a->lo = C3_LO; a->hi = C3_HI;
}

void c3_make_f1(c3_model *m, double q, double r)
{
    memset(m, 0, sizeof *m); m->id = 1;
    c3_assumption_base(&m->a, EST_FAM_GAUSS_KF);
    m->a.nparam = 3; m->a.param[0] = q; m->a.param[1] = r; m->a.param[2] = r; /* p0 = r */
}
void c3_make_f2(c3_model *m, double q, double r, double c)
{
    memset(m, 0, sizeof *m); m->id = 2;
    c3_assumption_base(&m->a, EST_FAM_HUBER_KF);
    m->a.nparam = 4; m->a.param[0] = q; m->a.param[1] = r; m->a.param[2] = r; m->a.param[3] = c;
}
void c3_make_f3(c3_model *m, double nu, double s)
{
    memset(m, 0, sizeof *m); m->id = 3;
    c3_assumption_base(&m->a, EST_FAM_STUDENT_T);
    m->a.nparam = 2; m->a.param[0] = nu; m->a.param[1] = s;
}
int c3_make_f4(c3_model *m, const est_mix *mx)
{
    memset(m, 0, sizeof *m); m->id = 4;
    c3_assumption_base(&m->a, EST_FAM_SCALE_MIX);
    if (!mx || mx->k < 1 || mx->k > 3) return 1;
    m->a.nparam = 7; m->a.param[0] = (double)mx->k;
    for (uint32_t j = 0; j < mx->k; j++) { m->a.param[1 + j] = mx->w[j]; m->a.param[4 + j] = mx->v[j]; }
    return 0;
}
void c3_make_f5(c3_model *m, double lambda, double nu, double c, double floor_var)
{
    memset(m, 0, sizeof *m); m->id = 5;
    c3_assumption_base(&m->a, EST_FAM_ADAPTIVE_T);
    m->a.nparam = 5; m->a.param[0] = lambda; m->a.param[1] = nu; m->a.param[2] = c;
    m->a.param[3] = floor_var; m->a.param[4] = sqrt(floor_var);   /* s0^2 = floor */
}
void c3_make_e0(c3_model *m, const uint32_t *counts, uint64_t n)
{
    memset(m, 0, sizeof *m); m->id = 0;
    c3_assumption_base(&m->a, EST_FAM_STUDENT_T);   /* used only to classify observations */
    /* one pseudo-observation spread uniformly (1/EST_PRED_N per bin): no zero bin,
     * without the ~25 % uniform mass that add-one over 801 bins would add */
    double den = (double)n + 1.0;
    for (int i = 0; i < EST_PRED_N; i++) m->e0_one[i] = ((double)counts[i] + 1.0 / (double)EST_PRED_N) / den;
}

static est_obs_class classify(const c3_tick *t, est_pobs *ob)
{
    est_assumption a; c3_assumption_base(&a, EST_FAM_STUDENT_T);
    a.nparam = 2; a.param[0] = 5; a.param[1] = 100;
    if (est_pobs_classify(&a, t->value, t->present, &t->ev, ob) != EST_OK) return EST_OBS_CLASSES_;
    return ob->cls;
}

size_t c3_h1_changes(const c3_ticks *tk, double *e, size_t cap)
{
    size_t n = 0; long first = -1; int prev_ok = 0; double prev_v = 0;
    for (size_t t = 0; t < tk->n; t++) {
        est_pobs ob;
        int ok = classify(&tk->t[t], &ob) == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        if (ok && prev_ok && first >= 0 && (long)t - first >= (long)C3_BURN_IN && n < cap)
            e[n++] = tk->t[t].value - prev_v;
        prev_ok = ok; if (ok) prev_v = tk->t[t].value;
    }
    return n;
}

void c3_e0_counts(const double *e, size_t n, uint32_t *counts)
{
    memset(counts, 0, EST_PRED_N * sizeof *counts);
    for (size_t i = 0; i < n; i++) {
        long k = lround(e[i] / C3_QUANTUM);
        if (k < -EST_PRED_K) k = -EST_PRED_K;
        if (k > EST_PRED_K) k = EST_PRED_K;
        counts[k + EST_PRED_K]++;
    }
}

/* -------------------------------------------------------------- statistics */

const char *c3_stat_name(int s)
{
    static char buf[C3_ST_COUNT][24];
    static int init;
    if (!init) {
        for (int i = 0; i < C3_ST_COUNT; i++) snprintf(buf[i], sizeof buf[i], "stat%d", i);
        snprintf(buf[C3_ST_COV50], 24, "coverage50");
        snprintf(buf[C3_ST_COV80], 24, "coverage80");
        snprintf(buf[C3_ST_COV95], 24, "coverage95");
        for (int j = 0; j < 10; j++) snprintf(buf[C3_ST_PIT0 + j], 24, "pit_bin%d", j);
        snprintf(buf[C3_ST_BIAS], 24, "bias_mean_z");
        snprintf(buf[C3_ST_LAG1], 24, "lag1_z");
        for (int q = 0; q < 4; q++) snprintf(buf[C3_ST_Q0 + q], 24, "quarter%d_coverage95", q + 1);
        snprintf(buf[C3_ST_REG_IDLE], 24, "regime_idle_cov95");
        snprintf(buf[C3_ST_REG_TRIAL], 24, "regime_trial_cov95");
        snprintf(buf[C3_ST_TEN], 24, "ten_step_coverage95");
        snprintf(buf[C3_ST_N], 24, "n_scored");
        init = 1;
    }
    return (s >= 0 && s < C3_ST_COUNT) ? buf[s] : "none";
}

static void e0_pmf(const c3_model *m, uint32_t H, double *out, double *cache10, int *have10)
{
    if (H == 1) { memcpy(out, m->e0_one, sizeof m->e0_one); return; }
    if (H == C3_TEN && cache10 && *have10) { memcpy(out, cache10, EST_PRED_N * sizeof *out); return; }
    est_pmf_conv_pow(m->e0_one, H, out);
    est_pmf_floor(out);
    if (H == C3_TEN && cache10) { memcpy(cache10, out, EST_PRED_N * sizeof *out); *have10 = 1; }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int serr(c3_stats *st, const char *what, size_t t, est_status s)
{
    st->error = 1;
    snprintf(st->errmsg, sizeof st->errmsg, "%s at tick %zu: status %d", what, t, (int)s);
    return 1;
}

int c3_score(const c3_ticks *tk, const c3_marks *mk, const c3_model *m, int full, c3_stats *st)
{
    memset(st, 0, sizeof *st);
    est_calib_init(&st->c);
    st->first_fail = -1;
    est_dpred *ring = NULL, *p = NULL;
    int ring_ok[C3_TEN];
    double *cache10 = NULL; int have10 = 0;
    memset(ring_ok, 0, sizeof ring_ok);
    p = malloc(sizeof *p);
    if (!p) return serr(st, "out of memory", 0, EST_OK);
    if (full) {
        st->cap = tk->n ? tk->n : 1;
        st->cov95_step = malloc(st->cap * sizeof(double));
        st->width_step = malloc(st->cap * sizeof(double));
        st->regime_step = malloc(st->cap);
        ring = malloc(C3_TEN * sizeof *ring);
        cache10 = malloc(EST_PRED_N * sizeof(double));
        if (!st->cov95_step || !st->width_step || !st->regime_step || !ring || !cache10) {
            free(ring); free(cache10); free(p); return serr(st, "out of memory", 0, EST_OK);
        }
    }
    est_bstate *b = malloc(sizeof *b);
    if (!b) { free(ring); free(cache10); free(p); return serr(st, "out of memory", 0, EST_OK); }
    est_status s;
    if (m->id) {
        s = est_pred_init(&m->a, b);
        if (s != EST_OK) { free(b); free(ring); free(cache10); free(p); return serr(st, "init", 0, s); }
    }
    int e_has = 0; double e_anchor = 0; uint32_t e_gap = 0;
    long first = -1;
    int rc = 0;
    for (size_t t = 0; t < tk->n && !rc; t++) {
        const c3_tick *tc = &tk->t[t];
        est_pobs ob;
        est_obs_class cls = classify(tc, &ob);
        if (cls == EST_OBS_CLASSES_) { rc = serr(st, "classify", t, EST_ERR_KIND); break; }
        int ok = cls == EST_OBS_OK;
        if (ok && first < 0) first = (long)t;
        int scored_region = first >= 0 && (long)t - first >= (long)C3_BURN_IN;
        est_pinnov iv;
        /* ten-step target */
        if (full && ring_ok[t % C3_TEN]) {
            ring_ok[t % C3_TEN] = 0;
            if (ok) {
                s = est_dpred_score(&ring[t % C3_TEN], &ob, &iv);
                if (s != EST_OK) { rc = serr(st, "ten-step score", t, s); break; }
                if (iv.valid) { st->ten_cov += est_frac_cover(iv.F_lo, iv.F_hi, 0.95); st->ten_n++; }
            }
        }
        /* one-step */
        if (ok && scored_region) {
            int have_p = 0;
            if (m->id) {
                s = est_pred_predict(&m->a, b, 1, p);
                if (s == EST_ERR_TIME) st->unscorable++;
                else if (s != EST_OK) { rc = serr(st, "predict", t, s); break; }
                else {
                    s = est_pred_innov(b, p, &ob, &iv);
                    if (s != EST_OK) { rc = serr(st, "innov", t, s); break; }
                    have_p = 1;
                }
            } else if (e_has) {
                uint32_t H = e_gap + 1;
                if (H > EST_PRED_MAX_H) st->unscorable++;
                else {
                    memset(p, 0, sizeof *p);
                    p->horizon = H; p->anchor = e_anchor; p->quantum = C3_QUANTUM;
                    p->family = (est_family)0;   /* E0 is tool-built, not an est_pred family */
                    e0_pmf(m, H, p->pmf, NULL, NULL);
                    s = est_dpred_score(p, &ob, &iv);
                    if (s != EST_OK) { rc = serr(st, "E0 score", t, s); break; }
                    have_p = 1;
                }
            }
            if (have_p && iv.valid) {
                st->logsum += iv.logp; st->n++;
                if (full) {
                    s = est_calib_add(&st->c, p, &iv);
                    if (s != EST_OK) { rc = serr(st, "calib", t, s); break; }
                    size_t k = st->n - 1;
                    st->cov95_step[k] = est_frac_cover(iv.F_lo, iv.F_hi, 0.95);
                    double w = NAN;
                    s = est_dpred_width80(p, &w);
                    if (s != EST_OK) { rc = serr(st, "width", t, s); break; }
                    st->width_step[k] = w;
                    st->regime_step[k] = (unsigned char)c3_in_trial(mk, tc->wall_ns);
                }
            }
        }
        /* update */
        if (m->id) {
            s = est_pred_update(&m->a, b, &ob);
            if (s != EST_OK) { rc = serr(st, "update", t, s); break; }
        } else {
            if (ok) { e_has = 1; e_anchor = tc->value; e_gap = 0; }
            else if (e_has) e_gap++;
        }
        /* ten-step origin */
        if (full && ok && scored_region) {
            est_dpred *q = &ring[t % C3_TEN];
            if (m->id) {
                s = est_pred_predict(&m->a, b, C3_TEN, q);
                if (s == EST_OK) ring_ok[t % C3_TEN] = 1;
                else if (s != EST_ERR_TIME) { rc = serr(st, "ten-step predict", t, s); break; }
            } else {
                memset(q, 0, sizeof *q);
                q->horizon = C3_TEN; q->anchor = e_anchor; q->quantum = C3_QUANTUM; q->family = (est_family)0;
                e0_pmf(m, C3_TEN, q->pmf, cache10, &have10);
                ring_ok[t % C3_TEN] = 1;
            }
        }
    }
    free(b); free(ring); free(cache10); free(p);
    if (rc) return rc;
    st->logscore = st->n ? st->logsum / (double)st->n : -INFINITY;
    if (full) c3_judge(st);
    return 0;
}

static void band(c3_stats *st, int i, double v, double lo, double hi)
{
    st->value[i] = v; st->band_lo[i] = lo; st->band_hi[i] = hi; st->gated[i] = 1;
    st->pass[i] = isfinite(v) && v >= lo && v <= hi;
}

void c3_judge(c3_stats *st)
{
    double n = (double)st->n;
    double nn = st->n ? n : NAN;
    band(st, C3_ST_COV50, st->c.cov[0] / nn, C3_COV50_LO, C3_COV50_HI);
    band(st, C3_ST_COV80, st->c.cov[1] / nn, C3_COV80_LO, C3_COV80_HI);
    band(st, C3_ST_COV95, st->c.cov[2] / nn, C3_COV95_LO, C3_COV95_HI);
    for (int j = 0; j < 10; j++) band(st, C3_ST_PIT0 + j, st->c.pit[j] / nn, C3_PIT_LO, C3_PIT_HI);
    band(st, C3_ST_BIAS, st->c.z_sum / nn, -C3_BIAS_MAX, C3_BIAS_MAX);
    band(st, C3_ST_LAG1, st->n >= 2 ? est_calib_lag1(&st->c) : NAN, -C3_LAG1_MAX, C3_LAG1_MAX);
    for (int q = 0; q < 4; q++) {
        size_t lo = (size_t)q * st->n / 4, hi = (size_t)(q + 1) * st->n / 4;
        double sum = 0;
        for (size_t k = lo; k < hi; k++) sum += st->cov95_step[k];
        st->sub_n[C3_ST_Q0 + q] = hi - lo;
        band(st, C3_ST_Q0 + q, hi > lo ? sum / (double)(hi - lo) : NAN, C3_SUB95_LO, C3_SUB95_HI);
    }
    double rs[2] = { 0, 0 }; size_t rn[2] = { 0, 0 };
    for (size_t k = 0; k < st->n; k++) { int g = st->regime_step[k] ? 1 : 0; rs[g] += st->cov95_step[k]; rn[g]++; }
    for (int g = 0; g < 2; g++) {
        int i = g ? C3_ST_REG_TRIAL : C3_ST_REG_IDLE;
        st->sub_n[i] = rn[g];
        band(st, i, rn[g] ? rs[g] / (double)rn[g] : NAN, C3_SUB95_LO, C3_SUB95_HI);
        if (rn[g] < C3_REGIME_MIN) { st->gated[i] = 0; }
    }
    st->sub_n[C3_ST_TEN] = st->ten_n;
    band(st, C3_ST_TEN, st->ten_n ? st->ten_cov / (double)st->ten_n : NAN, C3_SUB95_LO, C3_SUB95_HI);
    band(st, C3_ST_N, n, (double)C3_MIN_N, INFINITY);
    st->calibrated = 1; st->first_fail = -1;
    for (int i = 0; i < C3_ST_COUNT; i++)
        if (st->gated[i] && !st->pass[i]) { st->calibrated = 0; if (st->first_fail < 0) st->first_fail = i; }
    st->width_mean = st->n ? st->c.width_sum / n : NAN;
    st->width_median = NAN;
    if (st->n) {
        double *w = malloc(st->n * sizeof *w);
        if (w) {
            memcpy(w, st->width_step, st->n * sizeof *w);
            qsort(w, st->n, sizeof *w, cmp_d);
            st->width_median = (st->n % 2) ? w[st->n / 2] : 0.5 * (w[st->n / 2 - 1] + w[st->n / 2]);
            free(w);
        }
    }
}

void c3_stats_free(c3_stats *st)
{
    free(st->cov95_step); free(st->width_step); free(st->regime_step);
    st->cov95_step = NULL; st->width_step = NULL; st->regime_step = NULL;
}

/* ---------------------------------------------------------------- precheck */

static int parse_cpu(const char *v, size_t vl, unsigned long long out[8])
{
    size_t i = 0;
    for (int k = 0; k < 8; k++) {
        while (i < vl && v[i] == ' ') i++;
        if (i >= vl || v[i] < '0' || v[i] > '9') return 1;
        unsigned long long x = 0; int d = 0;
        while (i < vl && v[i] >= '0' && v[i] <= '9') {
            if (d >= 19) return 1;
            x = x * 10 + (unsigned long long)(v[i] - '0'); i++; d++;
        }
        out[k] = x;
    }
    while (i < vl && v[i] == ' ') i++;
    return i != vl;
}

static double overlap(int64_t a0, int64_t a1, int64_t b0, int64_t b1)
{
    int64_t lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
    return hi > lo ? (double)(hi - lo) : 0.0;
}

int c3_precheck(const char *dir, c3_pre *p, char *err, size_t cap)
{
    memset(p, 0, sizeof *p);
    char path[1200];
    snprintf(path, sizeof path, "%s/machine-state.ndjson", dir);
    est_file f;
    if (est_file_load(path, &f, err, cap)) return 1;
    memcpy(p->raw_sha, f.sha_hex, 65);
    static c3_marks mk;
    snprintf(path, sizeof path, "%s/machine-state-marks.txt", dir);
    char e2[256] = "";
    int have_marks = c3_marks_load(path, 1, &mk, e2, sizeof e2) == 0;
    if (have_marks) memcpy(p->marks_sha, mk.sha, 65); else snprintf(p->marks_sha, 65, "unavailable");

    /* schedule */
    int sidx[C3_MAX_MARKS], slv[C3_MAX_MARKS]; size_t ns = 0; int sched_bad = 0;
    snprintf(path, sizeof path, "%s/schedule.txt", dir);
    est_file sf;
    char e3[256] = "";
    snprintf(p->sched_sha, 65, "unavailable");
    if (est_file_load(path, &sf, e3, sizeof e3) == 0) {
        p->have_schedule = 1;
        memcpy(p->sched_sha, sf.sha_hex, 65);
        for (size_t i = 0; i < sf.nlines; i++) {
            const char *ln = (const char *)sf.data + sf.off[i]; size_t ll = sf.llen[i];
            if (ll < 9 || memcmp(ln, "schedule ", 9) != 0) continue;
            char buf[128]; if (ll >= sizeof buf) { sched_bad = 1; continue; }
            memcpy(buf, ln, ll); buf[ll] = 0;
            int a, l; long s; char tail;
            if (sscanf(buf, "schedule %d %d %ld%c", &a, &l, &s, &tail) != 3 || a < 0 || l < 0 || s <= 0) { sched_bad = 1; continue; }
            if (ns >= C3_MAX_MARKS) { sched_bad = 1; continue; }
            sidx[ns] = a; slv[ns] = l; ns++;
        }
        est_file_free(&sf);
        p->sched_segments = ns;
        for (size_t i = 0; i < ns; i++) if (slv[i] > 0) p->sched_trials++;
        if (ns == 0) sched_bad = 1;
    }

    /* marks pair rule: the begin/end lines are exactly begin(i), end(i) for each
     * scheduled trial i in schedule order, names "load-L<level>-<i>", times non-decreasing */
    p->marks_ok = 1;
    if (!p->have_schedule) { p->marks_ok = 0; snprintf(p->marks_why, sizeof p->marks_why, "schedule.txt missing or not in SHA256SUMS"); }
    else if (sched_bad) { p->marks_ok = 0; snprintf(p->marks_why, sizeof p->marks_why, "schedule.txt malformed or empty"); }
    else if (!have_marks) { p->marks_ok = 0; snprintf(p->marks_why, sizeof p->marks_why, "marks file unreadable or not in SHA256SUMS"); }
    else if (mk.bad_lines) { p->marks_ok = 0; snprintf(p->marks_why, sizeof p->marks_why, "%zu marks lines are not begin/end lines", mk.bad_lines); }
    else if (mk.nev != 2 * p->sched_trials) {
        p->marks_ok = 0;
        snprintf(p->marks_why, sizeof p->marks_why, "%zu begin/end lines for %zu scheduled trials", mk.nev, p->sched_trials);
    } else {
        size_t e = 0;
        for (size_t i = 0; i < ns && p->marks_ok; i++) {
            if (slv[i] <= 0) continue;
            for (int k = 0; k < 2; k++, e++) {
                if (mk.ev_end[e] != k || mk.ev_index[e] != sidx[i] || mk.ev_level[e] != slv[i]
                    || (e > 0 && mk.ev_t[e] < mk.ev_t[e - 1])) {
                    p->marks_ok = 0;
                    snprintf(p->marks_why, sizeof p->marks_why, "marks line %zu is not the %s of scheduled trial %d (level %d)",
                             e + 1, k ? "end" : "begin", sidx[i], slv[i]);
                    break;
                }
            }
        }
    }
    if (p->marks_ok) snprintf(p->marks_why, sizeof p->marks_why, "one begin/end pair per scheduled trial");

    /* times, gaps, loadavg, foreign load: only "t", "loadavg" and "cpu" are read */
    p->lines = f.nlines;
    int64_t pt = 0; int pt_ok = 0; int pc_ok = 0; unsigned long long pc[8] = { 0 };
    double fsum = 0, lsum = 0;
    for (size_t li = 0; li < f.nlines; li++) {
        const char *ln = (const char *)f.data + f.off[li]; size_t ll = f.llen[li];
        int64_t t = 0;
        int t_ok = est_parse_t(ln, ll, &t) == 0;
        if (!t_ok) p->bad_t++;
        const char *v; size_t vl;
        if (c3_json_field(ln, ll, "loadavg", &v, &vl) == 0) {
            char buf[32]; size_t n = 0;
            while (n < vl && v[n] != ' ' && n < sizeof buf - 1) { buf[n] = v[n]; n++; }
            buf[n] = 0; char *end; double x = strtod(buf, &end);
            if (n && *end == 0 && isfinite(x)) { lsum += x; p->loadavg_n++; }
        }
        unsigned long long c[8];
        int c_ok = c3_json_field(ln, ll, "cpu", &v, &vl) == 0 && parse_cpu(v, vl, c) == 0;
        if (li > 0) {
            if (t_ok && pt_ok) {
                p->gaps++;
                if (t - pt > 1500000000ll) p->gaps_big++;
            }
            int measured = 0;
            if (t_ok && pt_ok && c_ok && pc_ok && t > pt) {
                unsigned long long d[8]; int mono = 1; double tot = 0;
                for (int k = 0; k < 8; k++) { if (c[k] < pc[k]) mono = 0; d[k] = c[k] - pc[k]; tot += (double)d[k]; }
                if (mono && tot > 0) {
                    double idle = (double)d[3] + (double)d[4];
                    double busy = C3_NCORES * (tot - idle) / tot;
                    double sched = 0;
                    if (have_marks)
                        for (size_t i = 0; i < mk.n; i++)
                            if (mk.level[i] > 0) sched += overlap(pt, t, mk.b[i], mk.e[i]) * (double)mk.level[i];
                    sched /= (double)(t - pt);
                    double fr = busy - sched;
                    fsum += fr; p->foreign_n++;
                    if (fr > 3.0) p->foreign_over3++;
                    measured = 1;
                }
            }
            if (!measured) p->foreign_unmeasured++;
        }
        if (t_ok) { pt = t; pt_ok = 1; } else pt_ok = 0;
        if (c_ok) { memcpy(pc, c, sizeof pc); pc_ok = 1; } else pc_ok = 0;
    }
    est_file_free(&f);
    p->gap_big_frac = p->gaps ? (double)p->gaps_big / (double)p->gaps : 1.0;
    p->foreign_mean = p->foreign_n ? fsum / (double)p->foreign_n : NAN;
    p->foreign_over3_frac = p->foreign_n ? (double)p->foreign_over3 / (double)p->foreign_n : NAN;
    p->loadavg1_mean = p->loadavg_n ? lsum / (double)p->loadavg_n : NAN;

    p->ok_lines = p->lines >= C3_MIN_LINES;
    p->ok_gaps = p->gaps > 0 && p->gap_big_frac <= 0.02;
    p->ok_marks = p->marks_ok;
    size_t pairs = p->lines ? p->lines - 1 : 0;
    p->ok_foreign_measured = p->foreign_n > 0 && (double)p->foreign_unmeasured <= 0.01 * (double)pairs;
    p->ok_foreign_mean = p->foreign_n > 0 && p->foreign_mean <= 1.5;
    p->ok_foreign_tail = p->foreign_n > 0 && p->foreign_over3_frac <= 0.10;
    p->valid = p->ok_lines && p->ok_gaps && p->ok_marks && p->ok_foreign_measured && p->ok_foreign_mean && p->ok_foreign_tail;
    p->why[0] = 0;
    size_t w = 0;
#define WHY(c, s) do { if (!(c) && w < sizeof p->why) w += (size_t)snprintf(p->why + w, sizeof p->why - w, "%s%s", w ? "; " : "", s); } while (0)
    WHY(p->ok_lines, "fewer than 2030 lines");
    WHY(p->ok_gaps, "more than 2 % of gaps above 1.5 s");
    WHY(p->ok_marks, "marks do not hold exactly one begin/end pair per scheduled trial");
    WHY(p->ok_foreign_measured, "foreign load not measurable on more than 1 % of samples");
    WHY(p->ok_foreign_mean || !p->foreign_n, "mean foreign busy cores above 1.5");
    WHY(p->ok_foreign_tail || !p->foreign_n, "more than 10 % of samples with foreign busy cores above 3.0");
#undef WHY
    if (p->valid) snprintf(p->why, sizeof p->why, "all section 7 rules hold");
    return 0;
}

/* ------------------------------------------------------------ params read */

static int is_hex64(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

int c3_params_read(const char *path, c3_params *p, char *err, size_t cap)
{
    /* Every parsed key may appear once; family and screen_result once per
     * family id, e0_count once per k. Unparsed record lines (rule, screen,
     * ticks, marks, ...) are not checked. */
    static const char *const single[] = { "est3c_params", "protocol_doc_sha256", "tool_commit", "tool_dirty", "synthetic_test",
                                          "fit_raw_path", "fit_raw_sha256", "fit_marks_path", "fit_marks_sha256", "fit_lines",
                                          "phase_a", "selected", "e0_changes", "assumption" };
    enum { NSINGLE = (int)(sizeof single / sizeof single[0]) };
    unsigned char seen[NSINGLE], seen_fam[6], seen_scr[6], seen_k[EST_PRED_N];
    memset(seen, 0, sizeof seen); memset(seen_fam, 0, sizeof seen_fam); memset(seen_scr, 0, sizeof seen_scr); memset(seen_k, 0, sizeof seen_k);
    memset(p, 0, sizeof *p);
    FILE *fp = fopen(path, "r");
    if (!fp) { msg(err, cap, "cannot open %s", path); return 1; }
    char line[4096];
    int have_magic = 0, have_phase = 0, have_sel = 0, have_e0n = 0;
    p->tool_dirty = -1; p->synthetic = -1;
    uint64_t e0sum = 0;
    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        char k[64], v[2048];
        if (sscanf(line, "%63s %2047s", k, v) < 1) continue;
        for (int i = 0; i < NSINGLE; i++) if (!strcmp(k, single[i])) { if (seen[i]++) goto dup; break; }
        if (!strcmp(k, "est3c_params")) have_magic = 1;
        else if (!strcmp(k, "protocol_doc_sha256")) { if (strlen(v) >= sizeof p->protocol_sha) goto bad; memcpy(p->protocol_sha, v, strlen(v) + 1); }
        else if (!strcmp(k, "tool_commit")) { if (strlen(v) >= sizeof p->tool_commit) goto bad; memcpy(p->tool_commit, v, strlen(v) + 1); }
        else if (!strcmp(k, "tool_dirty")) p->tool_dirty = atoi(v);
        else if (!strcmp(k, "synthetic_test")) p->synthetic = atoi(v);
        else if (!strcmp(k, "fit_raw_path")) { if (strlen(v) >= sizeof p->fit_raw_path) goto bad; memcpy(p->fit_raw_path, v, strlen(v) + 1); }
        else if (!strcmp(k, "fit_raw_sha256")) { if (strlen(v) >= sizeof p->fit_raw_sha) goto bad; memcpy(p->fit_raw_sha, v, strlen(v) + 1); }
        else if (!strcmp(k, "fit_marks_path")) { if (strlen(v) >= sizeof p->fit_marks_path) goto bad; memcpy(p->fit_marks_path, v, strlen(v) + 1); }
        else if (!strcmp(k, "fit_marks_sha256")) { if (strlen(v) >= sizeof p->fit_marks_sha) goto bad; memcpy(p->fit_marks_sha, v, strlen(v) + 1); }
        else if (!strcmp(k, "fit_lines")) p->fit_lines = (size_t)strtoull(v, NULL, 10);
        else if (!strcmp(k, "phase_a")) { have_phase = 1; p->phase_a_pass = !strcmp(v, "PASS"); if (strcmp(v, "PASS") && strcmp(v, "PHASE_A_FAIL")) goto bad; }
        else if (!strcmp(k, "selected")) {
            have_sel = 1;
            if (!strcmp(v, "none")) p->selected = 0;
            else if (v[0] == 'F' && v[1] >= '1' && v[1] <= '5' && v[2] == 0) p->selected = v[1] - '0';
            else goto bad;
        } else if (!strcmp(k, "e0_changes")) { have_e0n = 1; p->e0_n = strtoull(v, NULL, 10); }
        else if (!strcmp(k, "e0_count")) {
            long kk; unsigned long cc; char tail;
            if (sscanf(line, "e0_count %ld %lu%c", &kk, &cc, &tail) != 2 || kk < -EST_PRED_K || kk > EST_PRED_K) goto bad;
            if (seen_k[kk + EST_PRED_K]++) goto dup;
            p->e0_count[kk + EST_PRED_K] = (uint32_t)cc; e0sum += cc;
        } else if (!strcmp(k, "family")) {
            /* family F<i> available <0|1> nparam <n> param <8 values> logscore <v> */
            char fn[8]; int av; unsigned np; double pr[8], ls;
            int got = sscanf(line, "family %7s available %d nparam %u param %lg %lg %lg %lg %lg %lg %lg %lg logscore %lg",
                             fn, &av, &np, &pr[0], &pr[1], &pr[2], &pr[3], &pr[4], &pr[5], &pr[6], &pr[7], &ls);
            if (got != 12 || fn[0] != 'F' || fn[1] < '1' || fn[1] > '5' || fn[2] || np > EST_PRED_NPARAM) goto bad;
            int id = fn[1] - '0';
            if (seen_fam[id]++) goto dup;
            static const est_family fams[6] = { 0, EST_FAM_GAUSS_KF, EST_FAM_HUBER_KF, EST_FAM_STUDENT_T, EST_FAM_SCALE_MIX, EST_FAM_ADAPTIVE_T };
            c3_assumption_base(&p->fam[id], fams[id]);
            p->fam[id].nparam = np;
            for (int j = 0; j < 8; j++) p->fam[id].param[j] = pr[j];
            p->have[id] = av; p->logscore[id] = ls;
        } else if (!strcmp(k, "screen_result")) {
            char fn[8]; int ps;
            if (sscanf(line, "screen_result %7s pass %d", fn, &ps) == 2 && fn[0] == 'F' && fn[1] >= '1' && fn[1] <= '5' && !fn[2]) {
                if (seen_scr[fn[1] - '0']++) goto dup;
                p->screen_pass[fn[1] - '0'] = ps;
            }
        } else if (!strcmp(k, "assumption")) {
            double q, lo, hi;
            if (sscanf(line, "assumption quantum %lg lo %lg hi %lg", &q, &lo, &hi) != 3 || q != C3_QUANTUM || lo != C3_LO || hi != C3_HI) goto bad;
        }
    }
    fclose(fp);
    if (!have_magic || !have_phase || !have_sel || !have_e0n || e0sum != p->e0_n || !is_hex64(p->fit_raw_sha)
        || p->tool_dirty < 0 || p->synthetic < 0) {
        msg(err, cap, "%s: incomplete or inconsistent parameter file", path);
        return 1;
    }
    if (p->phase_a_pass && (p->selected < 1 || !p->have[p->selected] || !p->have[1])) {
        msg(err, cap, "%s: PASS without an available selected family and F1", path);
        return 1;
    }
    return 0;
bad:
    fclose(fp);
    msg(err, cap, "%s: malformed line in parameter file", path);
    return 1;
dup:
    fclose(fp);
    msg(err, cap, "%s: duplicate key in parameter file", path);
    return 1;
}

int c3_model_from_params(const c3_params *p, int id, c3_model *m)
{
    memset(m, 0, sizeof *m);
    if (id == 0) { c3_make_e0(m, p->e0_count, p->e0_n); return 0; }
    if (id < 1 || id > 5 || !p->have[id]) return 1;
    m->id = id; m->a = p->fam[id];
    return est_assumption_check(&m->a) != EST_OK;
}

/* ------------------------------------------------------------------- misc */

int c3_path_is_heldout(const char *path)
{
    char rp[PATH_MAX];
    if (strstr(path, C3_HELDOUT_TAG)) return 1;
    if (realpath(path, rp) && strstr(rp, C3_HELDOUT_TAG)) return 1;
    return 0;
}

int c3_write_sums(const char *dir, const char *const *names, size_t n)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/SHA256SUMS", dir);
    FILE *fp = fopen(p, "w");
    if (!fp) return 1;
    for (size_t i = 0; i < n; i++) {
        char f[1200], hex[65];
        snprintf(f, sizeof f, "%s/%s", dir, names[i]);
        if (est_sha_file_hex(f, hex)) { fclose(fp); return 1; }
        fprintf(fp, "%s  %s\n", hex, names[i]);
    }
    return fclose(fp) != 0;
}

void c3_json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(fp, "\\%c", c);
        else if (c < 0x20) fprintf(fp, "\\u%04x", c);
        else fputc(c, fp);
    }
    fputc('"', fp);
}

/* -------------------------------------------------------------- synthetic */

static uint64_t sm64(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static double u01(uint64_t *s) { return (double)(sm64(s) >> 11) * (1.0 / 9007199254740992.0); }

static int draw_k(uint64_t *s, int heavy)
{
    double u = u01(s);
    if (!heavy) {
        if (u < 0.60) return 0;
        if (u < 0.75) return 1;
        if (u < 0.90) return -1;
        if (u < 0.95) return 2;
        return -2;
    }
    if (u < 0.30) return 0;
    if (u < 0.50) return 1;
    if (u < 0.70) return -1;
    if (u < 0.80) return 2;
    if (u < 0.90) return -2;
    if (u < 0.95) return 3;
    return -3;
}

int c3_synth_write(const char *dir, const c3_synth *sy)
{
    if (mkdir(dir, 0755) && errno != EEXIST) return 1;
    uint64_t s = sy->seed;
    long total = (long)sy->lines + 10;
    static const int levels[4] = { 0, 6, 12, 18 };
    int seg_lv[C3_MAX_MARKS]; long seg_s[C3_MAX_MARKS]; int nseg = 0; long acc = 0;
    while (acc < total && nseg < C3_MAX_MARKS) {
        int lv = levels[sm64(&s) % 4u];
        long d = 20 + (long)(sm64(&s) % 101u);
        if (acc + d > total) d = total - acc;
        seg_lv[nseg] = lv; seg_s[nseg] = d; acc += d; nseg++;
    }
    const int64_t t0 = 1790000000ll * NS + 123456789ll;
    char p[1200];
    snprintf(p, sizeof p, "%s/schedule.txt", dir);
    FILE *fs = fopen(p, "w"); if (!fs) return 1;
    fprintf(fs, "est_load seed %llu seconds %ld segments %d\n", (unsigned long long)sy->seed, total, nseg);
    for (int i = 0; i < nseg; i++) fprintf(fs, "schedule %d %d %ld\n", i, seg_lv[i], seg_s[i]);
    fprintf(fs, "est_load done\n");
    fclose(fs);
    snprintf(p, sizeof p, "%s/machine-state-marks.txt", dir);
    FILE *fm = fopen(p, "w"); if (!fm) return 1;
    long st = 0; int dropped = 0;
    for (int i = 0; i < nseg; i++) {
        if (seg_lv[i] > 0) {
            int64_t b = t0 + (int64_t)st * NS + 510000000ll, e = t0 + (int64_t)(st + seg_s[i]) * NS + 510000000ll;
            fprintf(fm, "%lld.%09lld begin load-L%d-%03d\n", (long long)(b / NS), (long long)(b % NS), seg_lv[i], i);
            if (sy->drop_end_mark && !dropped) dropped = 1;
            else fprintf(fm, "%lld.%09lld end load-L%d-%03d exit 0\n", (long long)(e / NS), (long long)(e % NS), seg_lv[i], i);
        }
        st += seg_s[i];
    }
    fclose(fm);
    snprintf(p, sizeof p, "%s/machine-state.ndjson", dir);
    FILE *fn = fopen(p, "w"); if (!fn) return 1;
    long y = 40000; unsigned long long cpu[8] = { 100000, 10, 5000, 900000, 300, 0, 50, 0 };
    long sec = 0;
    for (size_t i = 0; i < sy->lines; i++) {
        long step = (sy->gap_every && i > 0 && i % sy->gap_every == 0) ? 3 : 1;
        if (i > 0) {
            for (long k = 0; k < step; k++) {
                long at = sec + k; int lv = 0; long a2 = 0;
                for (int g = 0; g < nseg; g++) { if (at >= a2 && at < a2 + seg_s[g]) { lv = seg_lv[g]; break; } a2 += seg_s[g]; }
                double busy = lv + sy->foreign + ((sy->spike_every && (at % (long)sy->spike_every) == 0) ? 4.0 : 0.0);
                if (busy > C3_NCORES) busy = C3_NCORES;
                unsigned long long bj = (unsigned long long)llround(busy * 100.0);
                cpu[0] += bj; cpu[3] += 2000ull - bj;
                y += 100 * draw_k(&s, sy->heavy_trial_noise && lv > 0);
            }
            sec += step;
        }
        int64_t t = t0 + (int64_t)sec * NS + 500000000ll + (int64_t)(sm64(&s) % 20000000ull);
        fprintf(fn, "{\"t\":%lld.%09lld,\"thermal_mc\":\"%ld 31800 31900 \",\"loadavg\":\"%.2f 1.00 1.00\",\"cpu\":\"%llu %llu %llu %llu %llu %llu %llu %llu\"}\n",
                (long long)(t / NS), (long long)(t % NS), y, 0.5 + sy->foreign,
                cpu[0], cpu[1], cpu[2], cpu[3], cpu[4], cpu[5], cpu[6], cpu[7]);
    }
    fclose(fn);
    static const char *const names[3] = { "machine-state.ndjson", "machine-state-marks.txt", "schedule.txt" };
    return c3_write_sums(dir, names, 3);
}

int c3_grid_take(int have, double ls, double best)
{
    return isfinite(ls) && (!have || ls > best);
}

int c3_select(const int *avail, const int *pass, const double *ls)
{
    double best = -INFINITY; int any = 0;
    for (int id = 1; id <= 5; id++) if (avail[id] && pass[id] && isfinite(ls[id]) && (!any || ls[id] > best)) { best = ls[id]; any = 1; }
    if (!any) return 0;
    for (int id = 1; id <= 5; id++) if (avail[id] && pass[id] && isfinite(ls[id]) && ls[id] >= best - C3_TIE_NATS) return id;
    return 0;
}

/* Runtime clean-tree check (in addition to the build-time TOOL_DIRTY flag):
 * runs `git status --porcelain` in the current directory. Any output line, a
 * failure to run git, or a non-zero exit status counts as dirty (returns 1). */
int c3_tree_dirty_now(void)
{
    FILE *p = popen("git status --porcelain 2>&1", "r");
    if (!p) return 1;
    int any = 0, ch;
    while ((ch = fgetc(p)) != EOF) any = 1;
    int st = pclose(p);
    return (any || st != 0) ? 1 : 0;
}
