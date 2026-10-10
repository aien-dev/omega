/* PD-1 run harness (Physics-0 Direction 7, aien-dev/physics docs/PD1_MACHINE_LAW_BENCHMARK.md).
 *
 * Harness side. Reads the label-free PD1REC1 stream (tools/physics0/pd1-convert), verifies its
 * SHA-256 chain, makes the spec section 3 splits, drives the UNCHANGED PD-0 learner on two views of
 * the records, builds the mandatory rivals, preregisters the holdout predictions by hash before any
 * holdout outcome is scored, runs the section 6 controls and prints the receipt.
 *
 * Views (the learner sees opaque observed variables only; no channel, passive steps):
 *   latency view  vars [q0, q4, cov, r0]: before = [q0, q4, cov, 0], after = [q0, q4, cov, r0];
 *                 rows that carry r0 (spec: src 1 and src 3 records with latency)
 *   class view    vars [q0, q4, cov, r1]: same, with r1 (0 or 1) as the outcome; all src 1 and 3 rows
 * Micro units: q0 in {0, 1e6}, q4 = ordinal x 1e6, cov = milli x 1000 (0 when absent), r0 as recorded
 * (micro), r1 in {0, 1e6}. A one-step change of the outcome variable from 0 is the outcome itself, so
 * the PD-0 vocabulary (monomials over the observed variables) is exactly "outcome = f(knobs)".
 *
 * What this is: development evidence of the machinery on real machine data (spec section 4.2).
 * It is not a blind benchmark and supports no discovery claim. The harness author has read the
 * stores named in spec section 5; the learner code is unchanged and generic.
 *
 * usage: pd1-run <pd1-records.bin> <out-dir> [seed]
 * exit 0 on PD1_RUN: PASS, 1 on FAIL, 2+ on refusal/error. No Python, no floats in any canonical byte.
 */
#include "pd0_fmt.h"
#include "pd0_rng.h"
#include "pd0_ladder.h"
#include "pd0_learner.h"
#include "pd0_score.h"
#include "sha256.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REC 112
#define MAXROWS 65536u
#define MICRO 1000000LL
#define NV 4                       /* observed variables per view: q0, q4, cov, outcome */
#define OUTV 3                     /* index of the outcome variable */

/* margins fixed before the run (spec section 7) */
#define NRMSE_BOUND_MICRO 10000    /* PD-0 L0..L4 one-step bound 0.01, inherited and provisional */
#define LOGLOSS_MARGIN 0.05        /* nats: candidate must beat the empty relation by this on src 2 */
#define P_CLIP 0.001               /* class probability clip for log-loss */

typedef struct { uint64_t seq; uint8_t src, q0, q1, r1; uint32_t q4, cov; int64_t r0; uint8_t tag; uint8_t hash[32]; } row;
enum { T_FIT = TAG_FIT, T_SELECT = TAG_SELECT, T_HOLD = TAG_HOLDOUT, T_EXCL = 9 };

static row *rows; static uint32_t nrows; static uint8_t root[32];

static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void hex(const uint8_t *h, size_t n, char *o) { for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", h[i]); o[2 * n] = 0; }

static int load(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    rows = calloc(MAXROWS, sizeof *rows); if (!rows) return -1;
    uint8_t rec[REC], prev[32] = { 0 }, h[32];
    while (fread(rec, 1, REC, f) == REC) {
        if (memcmp(rec, "PD1REC1\0", 8) || rec[8] != 1 || rec[9] != 0) { fprintf(stderr, "bad magic/version at %u\n", nrows); return -1; }
        if (r64(rec + 16) != nrows) { fprintf(stderr, "seq gap at %u\n", nrows); return -1; }
        if (memcmp(rec + 48, prev, 32)) { fprintf(stderr, "chain broken at %u\n", nrows); return -1; }
        sha256_hash(rec, 80, h); if (memcmp(rec + 80, h, 32)) { fprintf(stderr, "record hash wrong at %u\n", nrows); return -1; }
        if (nrows >= MAXROWS) { fprintf(stderr, "too many records\n"); return -1; }
        row *r = &rows[nrows++]; r->seq = nrows - 1; r->src = rec[10]; r->r1 = rec[11]; r->q0 = rec[12]; r->q1 = rec[13];
        r->q4 = r32(rec + 24); r->cov = r32(rec + 28); r->r0 = (int64_t)r64(rec + 40); memcpy(r->hash, h, 32); memcpy(prev, h, 32);
    }
    fclose(f); memcpy(root, prev, 32); return nrows ? 0 : -1;
}

/* spec section 3 item 2: FIT 60 / SELECT 20 / HOLDOUT 20 within src 1 and 3 stratified by q0;
 * all of src 2 is HOLDOUT (cross-series); src 4 is never scored. Seeded, deterministic. */
static void split(uint64_t seed)
{
    for (uint32_t i = 0; i < nrows; i++) rows[i].tag = T_EXCL;
    for (int src = 1; src <= 3; src += 2) for (int q0 = 0; q0 <= 1; q0++) {
        uint32_t idx[MAXROWS], n = 0;
        for (uint32_t i = 0; i < nrows; i++) if (rows[i].src == src && rows[i].q0 == q0) idx[n++] = i;
        pd0_rng g; char tag[32]; snprintf(tag, sizeof tag, "pd1-split-%d-%d", src, q0); pd0_rng_stream(&g, seed, tag);
        for (uint32_t i = n; i > 1; i--) { uint32_t j = (uint32_t)(pd0_rng_next(&g) % i); uint32_t t = idx[i - 1]; idx[i - 1] = idx[j]; idx[j] = t; }
        uint32_t nf = (n * 60 + 50) / 100, ns = (n * 20 + 50) / 100;
        for (uint32_t i = 0; i < n; i++) rows[idx[i]].tag = i < nf ? T_FIT : i < nf + ns ? T_SELECT : T_HOLD;
    }
    for (uint32_t i = 0; i < nrows; i++) if (rows[i].src == 2) rows[i].tag = T_HOLD;
}

/* a view turns a row into the learner's observed variables (micro) */
typedef struct { int latency; int extra_knob; uint64_t extra_seed; } view;
static int row_in_view(const view *v, const row *r) { if (r->src == 4 || r->q0 == 255) return 0; return v->latency ? r->r0 >= 0 : r->r1 != 255; }
static void knobs(const view *v, const row *r, int64_t *x)
{
    x[0] = (int64_t)r->q0 * MICRO; x[1] = (int64_t)r->q4 * MICRO; x[2] = r->cov == 4294967295u ? 0 : (int64_t)r->cov * 1000; x[3] = 0;
    if (v->extra_knob) { pd0_rng g; pd0_rng_stream(&g, v->extra_seed, "pd1-null-knob"); for (uint64_t k = 0; k <= r->seq; k++) x[4] = pd0_rng_unit(&g); }
}
static int64_t outcome(const view *v, const row *r) { return v->latency ? r->r0 : (int64_t)r->r1 * MICRO; }
static uint8_t nobs(const view *v) { return (uint8_t)(NV + (v->extra_knob ? 1 : 0)); }

static int feed(pd0_learner *L, const view *v, uint8_t tag)
{
    int n = 0;
    for (uint32_t i = 0; i < nrows; i++) { const row *r = &rows[i]; if (r->tag != tag || !row_in_view(v, r)) continue;
        pd0_rec rec; memset(&rec, 0, sizeof rec); rec.n_obs = nobs(v); rec.status = PD0_ST_OK; rec.kind = PD0_KIND_STEP; rec.channel = PD0_CHAN_NONE;
        rec.seq = r->seq; rec.episode = (uint32_t)r->seq; rec.step_in_episode = 1;
        knobs(v, r, rec.before); memcpy(rec.after, rec.before, sizeof rec.before); rec.after[OUTV] = outcome(v, r);
        if (pd0_learner_observe(L, &rec, tag) == 0) n++; }
    return n;
}
static void desc_for(const view *v, pd0l_desc *d)
{
    memset(d, 0, sizeof *d); d->n_obs = nobs(v); d->n_channels = 0; d->dt_micro = MICRO; d->episode_max_steps = 1; d->budget_steps = MAXROWS; d->budget_episodes = MAXROWS;
    for (int j = 0; j < d->n_obs; j++) { d->reset_min[j] = INT64_MAX; d->reset_max[j] = INT64_MIN; }
    for (uint32_t i = 0; i < nrows; i++) if (row_in_view(v, &rows[i])) { int64_t x[PD0_MAX_OBS]; knobs(v, &rows[i], x); x[OUTV] = outcome(v, &rows[i]);
        for (int j = 0; j < d->n_obs; j++) { if (x[j] < d->reset_min[j]) d->reset_min[j] = x[j]; if (x[j] > d->reset_max[j]) d->reset_max[j] = x[j]; } }
}

/* prediction of the outcome from knobs only (the integer evaluator of the relation) */
static int64_t predict(const pd0_rel *rel, const int64_t *x)
{
    int64_t st[PD0_MAX_VARS] = { 0 }, nx[PD0_MAX_VARS] = { 0 }; memcpy(st, x, sizeof(int64_t) * rel->n_vars);
    pd0_rel_step(rel, st, PD0_CHAN_NONE, 0, nx); return nx[OUTV];
}
/* knob usage of the outcome equation: bit j set when some term has a positive exponent on input j */
static unsigned uses(const pd0_rel *rel)
{
    unsigned m = 0;
    for (int e = 0; e < rel->n_equations; e++) for (int t = 0; t < rel->eq[e].n_terms; t++) { if (rel->eq[e].coef[t] == 0) continue;   /* zero terms carry no dependence */
        if (rel->eq[e].target != OUTV) { m |= 1u << 8; continue; }   /* a live equation on a knob variable is itself a defect */
        for (int j = 0; j < rel->n_vars; j++) if (rel->eq[e].expo[t][j]) m |= 1u << j; }
    return m;
}
static int64_t q0_coef(const pd0_rel *rel)     /* coefficient of the first pure-q0 term (any power) of the outcome equation, 0 if none */
{
    for (int e = 0; e < rel->n_equations; e++) if (rel->eq[e].target == OUTV) for (int t = 0; t < rel->eq[e].n_terms; t++) {
        int pure = rel->eq[e].expo[t][0] >= 1 && rel->eq[e].coef[t] != 0;   /* any power: on 0/1 data q0^k == q0 */ for (int j = 1; j < rel->n_vars; j++) if (rel->eq[e].expo[t][j]) pure = 0; if (pure) return rel->eq[e].coef[t]; }
    return 0;
}

/* harness rivals: least squares on FIT with a fixed term set over the outcome equation */
static void rival(const view *v, int knob, pd0_rel *out)   /* knob -1: constant only; else constant + knob */
{
    double sxx = 0, sxy = 0, sx = 0, sy = 0; uint32_t n = 0;
    for (uint32_t i = 0; i < nrows; i++) { const row *r = &rows[i]; if (r->tag != T_FIT || !row_in_view(v, r)) continue;
        int64_t x[PD0_MAX_OBS]; knobs(v, r, x); double xk = knob >= 0 ? (double)x[knob] / 1e6 : 0, y = (double)outcome(v, r) / 1e6;
        sx += xk; sy += y; sxx += xk * xk; sxy += xk * y; n++; }
    double a = 0, b = n ? sy / n : 0;
    if (knob >= 0 && n) { double den = sxx - sx * sx / n; if (den > 1e-12) { a = (sxy - sx * sy / n) / den; b = (sy - a * sx) / n; } }
    memset(out, 0, sizeof *out); out->n_vars = nobs(v); out->n_channels = 0; out->n_equations = 1; pd0_eq *q = &out->eq[0]; q->target = OUTV; q->n_terms = 0;
    q->coef[q->n_terms++] = (int64_t)llround(b * 1e6);                                   /* constant: all exponents zero */
    if (knob >= 0 && a != 0) { q->expo[q->n_terms][knob] = 1; q->coef[q->n_terms++] = (int64_t)llround(a * 1e6); }
    out->description_bits = pd0_rel_bits(out);
}

/* scores on HOLDOUT: latency NRMSE (micro) over rows that carry r0; class log-loss (nats) over src 2 and 3 */
typedef struct { int64_t nrmse; uint32_t n_lat; double logloss; uint32_t n_cls; } score_t;
static score_t score_model(const view *v, const pd0_rel *rel, const view *cv, const pd0_rel *crel)
{
    score_t s; memset(&s, 0, sizeof s);
    static int64_t pred[MAXROWS], obs[MAXROWS]; uint32_t n = 0; double ll = 0; uint32_t nc = 0;
    for (uint32_t i = 0; i < nrows; i++) { const row *r = &rows[i]; if (r->tag != T_HOLD || r->src == 4 || r->q0 == 255) continue;
        if (r->r0 >= 0 && rel) { int64_t x[PD0_MAX_OBS]; knobs(v, r, x); pred[n] = predict(rel, x); obs[n] = r->r0; n++; }
        if ((r->src == 2 || r->src == 3) && r->r1 != 255 && crel) { int64_t x[PD0_MAX_OBS]; knobs(cv, r, x); double p = (double)predict(crel, x) / 1e6;
            if (p < P_CLIP) p = P_CLIP; if (p > 1 - P_CLIP) p = 1 - P_CLIP; ll += r->r1 ? -log(p) : -log(1 - p); nc++; } }
    s.n_lat = n; s.nrmse = n ? pd0_nrmse_micro(pred, obs, n, 1) : -1; s.n_cls = nc; s.logloss = nc ? ll / nc : -1; return s;
}

/* preregistration: holdout predictions of every model from knobs only, hashed before scoring */
static void prereg(const char *dir, const view *v, const pd0_rel **models, const char **names, int nm, uint8_t out[32])
{
    char path[1024]; snprintf(path, sizeof path, "%s/pd1-prereg-%s.tsv", dir, v->latency ? "latency" : "class");
    FILE *f = fopen(path, "w"); if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(3); }
    sha256_ctx c; sha256_init(&c); char line[4096];
    int n = snprintf(line, sizeof line, "seq"); for (int m = 0; m < nm; m++) n += snprintf(line + n, sizeof line - (size_t)n, "\t%s", names[m]); n += snprintf(line + n, sizeof line - (size_t)n, "\n");
    fputs(line, f); sha256_update(&c, (const uint8_t *)line, (size_t)n);
    for (uint32_t i = 0; i < nrows; i++) { const row *r = &rows[i]; if (r->tag != T_HOLD || r->src == 4 || r->q0 == 255) continue;
        if (v->latency ? r->r0 < 0 : !(r->src == 2 || r->src == 3)) continue;
        int64_t x[PD0_MAX_OBS]; knobs(v, r, x); n = snprintf(line, sizeof line, "%" PRIu64, r->seq);
        for (int m = 0; m < nm; m++) n += snprintf(line + n, sizeof line - (size_t)n, "\t%" PRId64, predict(models[m], x));
        n += snprintf(line + n, sizeof line - (size_t)n, "\n"); fputs(line, f); sha256_update(&c, (const uint8_t *)line, (size_t)n); }
    fclose(f); sha256_final(&c, out);
}
static void outcome_hash(uint8_t out[32])
{
    sha256_ctx c; sha256_init(&c);
    for (uint32_t i = 0; i < nrows; i++) if (rows[i].tag == T_HOLD && rows[i].src != 4) { uint8_t b[17]; for (int k = 0; k < 8; k++) b[k] = (uint8_t)((uint64_t)rows[i].r0 >> (8 * k));
        for (int k = 0; k < 8; k++) b[8 + k] = (uint8_t)(rows[i].seq >> (8 * k)); b[16] = rows[i].r1; sha256_update(&c, b, 17); }
    sha256_final(&c, out);
}

static void print_rel(const char *name, const pd0_rel *r)
{
    printf("  %s: equations=%u bits=%u size=%u", name, r->n_equations, pd0_rel_bits(r), pd0_rel_size(r));
    for (int e = 0; e < r->n_equations; e++) { printf(" | d v%u =", r->eq[e].target);
        for (int t = 0; t < r->eq[e].n_terms; t++) { printf(" %+" PRId64 "e-6", r->eq[e].coef[t]); for (int j = 0; j < r->n_vars; j++) for (int k = 0; k < r->eq[e].expo[t][j]; k++) printf("*v%d", j); } }
    printf("\n");
}

/* one learner run on a view; returns the best candidate (null if nothing beats it) */
static int run_view(const view *v, uint64_t seed, pd0_rel *best, int *best_is_null, uint32_t *nfit, uint32_t *nsel)
{
    pd0l_desc d; desc_for(v, &d); pd0_learner *L = pd0_learner_new(&d, seed); if (!L) return -1;
    *nfit = (uint32_t)feed(L, v, TAG_FIT); *nsel = (uint32_t)feed(L, v, TAG_SELECT);
    int nc = pd0_learner_fit(L); const pd0l_candidate *c0 = pd0_learner_candidate(L, 0);
    *best = c0->rel; *best_is_null = c0->is_null;
    printf("  learner: fit_rows=%u select_rows=%u candidates=%d best_is_null=%d", *nfit, *nsel, nc, c0->is_null);
    for (int i = 0; i < nc; i++) { const pd0l_candidate *c = pd0_learner_candidate(L, i); printf(" [%d: %s bits=%u sel_nrmse=%" PRId64 " mdl=%.1f uses=0x%x]", i, c->is_null ? "null" : "cand", c->bits, c->select_nrmse_micro, c->mdl_bits, uses(&c->rel)); }
    printf("\n"); pd0_learner_free(L); return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: pd1-run <pd1-records.bin> <out-dir> [seed]\n"); return 2; }
    uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 0) : 1; const char *dir = argv[2];
    if (load(argv[1]) != 0) { printf("PD1_RUN: REFUSED (records unreadable or chain broken)\n"); return 2; }
    char rh[65]; hex(root, 32, rh);
    printf("receipt: PD1_RUN\nstatus_label: DEVELOPMENT EVIDENCE of the machinery on real machine data (spec PD1 section 4.2). NOT a blind benchmark. NO discovery claim.\n");
    printf("spec: aien-dev/physics docs/PD1_MACHINE_LAW_BENCHMARK.md (DRAFT, 425e734)\nrecords=%u root=%s seed=%" PRIu64 "\n", nrows, rh, seed);
    printf("margins_fixed_before_run: latency NRMSE bound %d micro (PD-0 L0..L4 one-step bound, inherited, provisional); class log-loss margin %.3f nats over the empty relation on src 2; probability clip %.3f\n", NRMSE_BOUND_MICRO, LOGLOSS_MARGIN, P_CLIP);
    split(seed);
    uint32_t cnt[5][4] = { { 0 } }; for (uint32_t i = 0; i < nrows; i++) { int t = rows[i].tag == T_FIT ? 0 : rows[i].tag == T_SELECT ? 1 : rows[i].tag == T_HOLD ? 2 : 3; if (rows[i].src <= 4) cnt[rows[i].src][t]++; }
    printf("splits (src: fit select holdout excluded):"); for (int s = 1; s <= 4; s++) printf(" %d: %u %u %u %u;", s, cnt[s][0], cnt[s][1], cnt[s][2], cnt[s][3]); printf("\n");

    view vl = { 1, 0, 0 }, vc = { 0, 0, 0 };
    pd0_rel cand_l, cand_c; int null_l, null_c; uint32_t nf, ns;
    printf("--- latency view (vars v0=q0 v1=q4 v2=cov v3=outcome r0)\n"); run_view(&vl, seed, &cand_l, &null_l, &nf, &ns); print_rel("candidate", &cand_l);
    printf("--- class view (vars v0=q0 v1=q4 v2=cov v3=outcome r1)\n"); run_view(&vc, seed, &cand_c, &null_c, &nf, &ns); print_rel("candidate", &cand_c);

    /* rivals (spec section 3 item 3) on both views */
    pd0_rel emp_l, q4_l, cov_l, emp_c, q4_c, cov_c;
    rival(&vl, -1, &emp_l); rival(&vl, 1, &q4_l); rival(&vl, 2, &cov_l); rival(&vc, -1, &emp_c); rival(&vc, 1, &q4_c); rival(&vc, 2, &cov_c);
    pd0_rel null_rel; memset(&null_rel, 0, sizeof null_rel); null_rel.n_vars = NV; null_rel.n_equations = 0;
    printf("--- rivals (least squares on FIT)\n"); print_rel("empty_l", &emp_l); print_rel("q4only_l", &q4_l); print_rel("covonly_l", &cov_l); print_rel("empty_c", &emp_c); print_rel("q4only_c", &q4_c); print_rel("covonly_c", &cov_c);

    /* preregistration before scoring */
    const pd0_rel *ml[] = { &cand_l, &emp_l, &q4_l, &cov_l, &null_rel }; const pd0_rel *mc[] = { &cand_c, &emp_c, &q4_c, &cov_c, &null_rel };
    const char *names[] = { "candidate", "empty", "q4only", "covonly", "pd0null" };
    uint8_t ph_l[32], ph_c[32], oh[32]; prereg(dir, &vl, ml, names, 5, ph_l); prereg(dir, &vc, mc, names, 5, ph_c);
    char h1[65], h2[65]; hex(ph_l, 32, h1); hex(ph_c, 32, h2); printf("prereg_sha256: latency=%s class=%s (files pd1-prereg-*.tsv in %s; written before any holdout outcome was read for scoring)\n", h1, h2, dir);
    outcome_hash(oh); hex(oh, 32, h1); printf("holdout_outcome_sha256: %s\n", h1);

    /* scoring (spec section 3 items 5 and 6, section 7) */
    printf("--- HOLDOUT-0 scores\n");
    score_t sc[5]; for (int m = 0; m < 5; m++) { sc[m] = score_model(&vl, ml[m], &vc, mc[m]);
        printf("  %-9s latency: n=%u nrmse_micro=%" PRId64 " bits=%u | class: n=%u logloss=%.4f bits=%u\n", names[m], sc[m].n_lat, sc[m].nrmse, pd0_rel_bits(ml[m]), sc[m].n_cls, sc[m].logloss, pd0_rel_bits(mc[m])); }
    /* src 2 alone (cross-series transfer), class view */
    double ll2[5]; uint32_t n2 = 0; for (int m = 0; m < 5; m++) { double ll = 0; n2 = 0; for (uint32_t i = 0; i < nrows; i++) { const row *r = &rows[i]; if (r->src != 2 || r->tag != T_HOLD || r->r1 == 255) continue;
            int64_t x[PD0_MAX_OBS]; knobs(&vc, r, x); double p = (double)predict(mc[m], x) / 1e6; if (p < P_CLIP) p = P_CLIP; if (p > 1 - P_CLIP) p = 1 - P_CLIP; ll += r->r1 ? -log(p) : -log(1 - p); n2++; } ll2[m] = n2 ? ll / n2 : -1; }
    printf("  src2 cross-series class log-loss (n=%u):", n2); for (int m = 0; m < 5; m++) printf(" %s=%.4f", names[m], ll2[m]); printf("\n");

    /* verdict parts */
    unsigned ul = uses(&cand_l), uc = uses(&cand_c);
    int lat_knobs_ok = !null_l && ul == 1u;                                 /* only q0 */
    int lat_beats = !null_l; for (int m = 1; m < 5 && lat_beats; m++) if (sc[m].nrmse >= 0 && sc[0].nrmse >= sc[m].nrmse) lat_beats = 0;
    int lat_bound_ok = sc[0].nrmse >= 0 && sc[0].nrmse <= NRMSE_BOUND_MICRO;
    int cls_knobs_ok = !null_c && uc == 1u;
    int cls_beats = !null_c; for (int m = 1; m < 5 && cls_beats; m++) if (ll2[m] >= 0 && ll2[0] >= ll2[m]) cls_beats = 0;
    int cls_margin_ok = !null_c && ll2[0] >= 0 && ll2[1] - ll2[0] >= LOGLOSS_MARGIN;
    printf("--- section 7 checks\n  latency: candidate_uses_only_q0=%d (uses=0x%x) beats_every_rival=%d within_bound=%d\n  class: candidate_uses_only_q0=%d (uses=0x%x) beats_every_rival_on_src2=%d margin_ok=%d\n", lat_knobs_ok, ul, lat_beats, lat_bound_ok, cls_knobs_ok, uc, cls_beats, cls_margin_ok);

    /* controls (spec section 6) */
    printf("--- controls\n"); int ctrl_ok = 1;
    { /* NC-P1 shuffled outcomes within src, seeded; spec: no relation better than the empty (constant-only) one, so the
       winning candidate may use no knob on either view (null or constant are both acceptable) */
        row *save = malloc(sizeof(row) * nrows); memcpy(save, rows, sizeof(row) * nrows);
        for (int src = 1; src <= 3; src++) { uint32_t idx[MAXROWS], n = 0; for (uint32_t i = 0; i < nrows; i++) if (rows[i].src == src) idx[n++] = i;
            pd0_rng g; char tag[32]; snprintf(tag, sizeof tag, "pd1-ncp1-%d", src); pd0_rng_stream(&g, seed, tag);
            for (uint32_t i = n; i > 1; i--) { uint32_t j = (uint32_t)(pd0_rng_next(&g) % i); int64_t t0 = rows[idx[i - 1]].r0; rows[idx[i - 1]].r0 = rows[idx[j]].r0; rows[idx[j]].r0 = t0; uint8_t t1 = rows[idx[i - 1]].r1; rows[idx[i - 1]].r1 = rows[idx[j]].r1; rows[idx[j]].r1 = t1; } }
        pd0_rel bl, bc; int isnull_l, isnull_c; run_view(&vl, seed, &bl, &isnull_l, &nf, &ns); run_view(&vc, seed, &bc, &isnull_c, &nf, &ns);
        unsigned u1 = isnull_l ? 0 : uses(&bl), u2 = isnull_c ? 0 : uses(&bc); int ok = (u1 & 0xffu) == 0 && (u2 & 0xffu) == 0; ctrl_ok &= ok;
        printf("  NC-P1 shuffled outcomes: latency best_null=%d uses=0x%x; class best_null=%d uses=0x%x (no knob may survive) -> %s\n", isnull_l, u1, isnull_c, u2, ok ? "PASS" : "FAIL");
        memcpy(rows, save, sizeof(row) * nrows); free(save); }
    { /* NC-P2 swap q0 within src 1: the pure q0 coefficient of the latency candidate must flip sign */
        for (uint32_t i = 0; i < nrows; i++) if (rows[i].src == 1 && rows[i].q0 != 255) rows[i].q0 = (uint8_t)(1 - rows[i].q0);
        pd0_rel b; int isnull; run_view(&vl, seed, &b, &isnull, &nf, &ns); int64_t c_orig = q0_coef(&cand_l), c_swap = q0_coef(&b);
        int ok = !isnull && c_orig != 0 && c_swap != 0 && ((c_orig > 0) != (c_swap > 0)); ctrl_ok &= ok;
        printf("  NC-P2 q0 swapped in src 1: q0_coef original=%" PRId64 " swapped=%" PRId64 " -> %s\n", c_orig, c_swap, ok ? "PASS" : "FAIL");
        for (uint32_t i = 0; i < nrows; i++) if (rows[i].src == 1 && rows[i].q0 != 255) rows[i].q0 = (uint8_t)(1 - rows[i].q0); }
    { /* NC-P3 null knob q6 (v4) from a recorded stream independent of everything: MDL must exclude it */
        view vx = { 1, 1, seed ^ 0x6B6E6F62ULL }; pd0_rel b; int isnull; run_view(&vx, seed, &b, &isnull, &nf, &ns); unsigned u = uses(&b);
        int ok = !isnull && !(u & (1u << 4)) && (u & 1u); ctrl_ok &= ok; printf("  NC-P3 null knob v4 added (seed 0x%" PRIx64 "): uses=0x%x -> %s\n", vx.extra_seed, u, ok ? "PASS" : "FAIL"); }
    { /* NC-P4 order knob: q4-only must lose to the candidate on HOLDOUT-0 latency */
        int ok = sc[0].nrmse >= 0 && sc[2].nrmse >= 0 && sc[0].nrmse < sc[2].nrmse; ctrl_ok &= ok;
        printf("  NC-P4 q4-only rival: candidate nrmse=%" PRId64 " q4only nrmse=%" PRId64 " -> %s\n", sc[0].nrmse, sc[2].nrmse, ok ? "PASS" : "FAIL"); }
    { /* positive control: the harness reference sparse solver on FIT must pass the holdout bound */
        static pd0_transition fit[MAXROWS]; uint32_t n = 0;
        for (uint32_t i = 0; i < nrows; i++) { const row *r = &rows[i]; if (r->tag != T_FIT || !row_in_view(&vl, r)) continue; memset(&fit[n], 0, sizeof fit[n]);
            knobs(&vl, r, fit[n].before); memcpy(fit[n].after, fit[n].before, sizeof fit[n].before); fit[n].after[OUTV] = r->r0; fit[n].chan = PD0_CHAN_NONE; n++; }
        pd0_rel ref; memset(&ref, 0, sizeof ref); int rc = pd0_reference_fit(NV, 0, 3, fit, n, &ref); score_t s = score_model(&vl, &ref, NULL, NULL);
        int ok = rc == 0 && s.nrmse >= 0 && s.nrmse <= NRMSE_BOUND_MICRO; ctrl_ok &= ok; print_rel("reference", &ref);
        printf("  positive control (reference sparse solver, %u fit rows): rc=%d holdout nrmse_micro=%" PRId64 " uses=0x%x -> %s\n", n, rc, s.nrmse, uses(&ref), ok ? "PASS" : "FAIL"); }

    /* law record (spec section 3 item 7): state HYPOTHESIS, holdout result as the one experiment, src 3 failures as exceptions */
    { static pd0_law law; memset(&law, 0, sizeof law); law.state = PDLAW_HYPOTHESIS; law.rel = cand_l; pd0l_desc d; desc_for(&vl, &d);
        law.dom.n_obs = NV; law.dom.n_channels = 0; law.dom.dt_micro = MICRO; law.dom.episode_len = 1; law.dom.n_observations = 0;
        for (int j = 0; j < NV; j++) { law.dom.var_min[j] = d.reset_min[j]; law.dom.var_max[j] = d.reset_max[j]; }
        for (uint32_t i = 0; i < nrows; i++) if ((rows[i].tag == T_FIT || rows[i].tag == T_SELECT) && row_in_view(&vl, &rows[i])) { law.dom.n_observations++; law.dom.n_episodes++; }
        law.dom.reset_min = d.reset_min[OUTV]; law.dom.reset_max = d.reset_max[OUTV];
        for (uint32_t i = 0; i < nrows && law.n_exceptions < PD0_MAX_EXC; i++) if (rows[i].src == 3 && rows[i].r1 == 1) { pd0_exception *e = &law.exc[law.n_exceptions++]; e->record_seq = rows[i].seq; memcpy(e->record_hash, rows[i].hash, 32);
            int64_t x[PD0_MAX_OBS]; knobs(&vc, &rows[i], x); e->predicted = predict(&cand_c, x); e->observed = MICRO; e->error_micro = e->observed - e->predicted; }
        pd0_experiment *x = &law.exp[law.n_experiments++]; x->kind = 1; memcpy(x->prereg_hash, ph_l, 32); memcpy(x->outcome_hash, oh, 32); x->result = (uint8_t)(lat_beats && lat_bound_ok); memcpy(x->experiment_id, ph_l, 32);
        x->first_seq = 0; x->last_seq = nrows - 1; memcpy(law.chain_root, root, 32);
        uint32_t p = 0, f = 0; for (uint32_t i = 0; i < nrows; i++) if (rows[i].tag == T_HOLD && rows[i].r0 >= 0) { int64_t xx[PD0_MAX_OBS]; knobs(&vl, &rows[i], xx); int64_t pr = predict(&cand_l, xx); if (llabs(pr - rows[i].r0) * 100 <= llabs(rows[i].r0) + 1) p++; else f++; }
        law.confidence_ppm = pd0_confidence_ppm(p, f);
        law.claim_len = (uint32_t)snprintf((char *)law.claim, PD0_MAX_CLAIM, "HYPOTHESIS (no live intervention, PD-1 replay): outcome v3 depends on knob v0 over the observed domain (two values of v0, one series of machine runs, one day); holdout within 1 percent: %u of %u; %u recorded exceptions (series-3 failures under both values of v0). Development evidence only, no discovery claim.", p, p + f, law.n_exceptions);
        static uint8_t buf[65536]; size_t n = pd0_law_write(&law, buf, sizeof buf);
        char path[1024]; snprintf(path, sizeof path, "%s/pd1-law.bin", dir); FILE *fo = n ? fopen(path, "wb") : NULL; if (fo) { fwrite(buf, 1, n, fo); fclose(fo); }
        char lid[65]; hex(law.law_id, 32, lid); printf("--- law record\n  state=HYPOTHESIS law_id=%s bytes=%zu exceptions=%u experiments=%u confidence_ppm=%u holdout_within_1pct=%u/%u\n  claim: %s\n", lid, n, law.n_exceptions, law.n_experiments, law.confidence_ppm, p, p + f, (const char *)law.claim); }

    int pass = ctrl_ok && lat_knobs_ok && lat_beats && lat_bound_ok && cls_knobs_ok && cls_beats && cls_margin_ok;
    printf("--- verdict (spec section 7: every part required)\n  controls=%s latency_part=%s class_part=%s\n", ctrl_ok ? "PASS" : "FAIL",
           (lat_knobs_ok && lat_beats && lat_bound_ok) ? "PASS" : "FAIL", (cls_knobs_ok && cls_beats && cls_margin_ok) ? "PASS" : "FAIL");
    printf("PD1_RUN: %s (development evidence; not a blind benchmark; no discovery claim)\n", pass ? "PASS" : "FAIL");
    free(rows); return pass ? 0 : 1;
}
