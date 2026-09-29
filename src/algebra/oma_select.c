/* MA-2 stand-in selector (not wired to rx_costmodel). See oma_select.h. */
#include "algebra/oma_select.h"

#include "algebra/realize_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void oma_sel_init(oma_sel_table *t) { memset(t, 0, sizeof *t); }

/* Flat-object key lookup on one receipt line: "key": value */
static const char *find_key(const char *line, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\": ", key);
    const char *p = strstr(line, pat);
    return p ? p + strlen(pat) : NULL;
}

static int kv_num(const char *line, const char *key, double *out) {
    const char *p = find_key(line, key);
    if (!p) return -1;
    char *end;
    double v = strtod(p, &end);
    if (end == p) return -1;
    *out = v;
    return 0;
}

static int kv_bool(const char *line, const char *key, int *out) {
    const char *p = find_key(line, key);
    if (!p) return -1;
    if (strncmp(p, "true", 4) == 0) { *out = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 0; }
    return -1;
}

static int kv_str(const char *line, const char *key, char *out, size_t cap) {
    const char *p = find_key(line, key);
    if (!p || *p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= cap) return -1;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 0;
}

int oma_sel_load(oma_sel_table *t, const char *path) {
    if (!t || !path) return OMA_SEL_E_IO;
    if (t->nruns >= OMA_SEL_MAX_RUNS) return OMA_SEL_E_FULL;
    FILE *f = fopen(path, "r");
    if (!f) return OMA_SEL_E_IO;
    int run = t->nruns;
    char line[4096];
    int in_table = 0, schema_ok = 0;
    size_t added = 0;
    t->run_id[run][0] = 0;
    while (fgets(line, sizeof line, f)) {
        /* MA3_BENCH_V1: the historical runs recorded under the earlier MA-3 label */
        if (strstr(line, "\"schema\": \"OMEGA_MIXED_ALGEBRA_MA2_BENCH_V1\"") ||
            strstr(line, "\"schema\": \"OMEGA_MIXED_ALGEBRA_MA3_BENCH_V1\""))
            schema_ok = 1;
        if (!in_table && strstr(line, "\"run_id\": ")) kv_str(line, "run_id", t->run_id[run], sizeof t->run_id[run]);
        if (strstr(line, "\"cost_table\": [")) { in_table = 1; continue; }
        if (!in_table) continue;
        if (strncmp(line, "  ]", 3) == 0) break;
        if (!strstr(line, "{\"n\": ")) continue;
        if (t->nrows >= OMA_SEL_MAX_ROWS) { fclose(f); return OMA_SEL_E_FULL; }
        oma_sel_row *r = &t->rows[t->nrows];
        memset(r, 0, sizeof *r);
        double n, m, sp;
        if (kv_num(line, "n", &n) || kv_num(line, "m", &m) || kv_num(line, "sparsity", &sp) ||
            kv_str(line, "rz", r->rz, sizeof r->rz) || kv_bool(line, "eligible", &r->eligible)) {
            fclose(f);
            return OMA_SEL_E_FORMAT;
        }
        r->run = run;
        r->n = (size_t)n;
        r->m = (size_t)m;
        r->sparsity = sp;
        if (kv_bool(line, "verified", &r->verified)) r->verified = 0;
        if (r->eligible) {
            double med, mn, nz, pk, pf;
            if (kv_num(line, "median_ps", &med) || kv_num(line, "min_ps", &mn) || kv_num(line, "noise_rel", &nz) ||
                kv_num(line, "pack_ps", &pk) || kv_num(line, "pct_floor_best", &pf)) {
                fclose(f);
                return OMA_SEL_E_FORMAT;
            }
            r->median_ns = med / 1e3;
            r->min_ns = mn / 1e3;
            r->noise_rel = nz;
            r->pack_ns = pk / 1e3;
            r->pct_floor = pf;
            if (kv_num(line, "pack_noise_rel", &r->pack_noise_rel)) r->pack_noise_rel = 0;
        }
        t->nrows++;
        added++;
    }
    fclose(f);
    if (!schema_ok) return OMA_SEL_E_FORMAT;
    if (!added) return OMA_SEL_E_EMPTY;
    snprintf(t->source[run], sizeof t->source[run], "%s", path);
    t->nruns++;
    return OMA_SEL_OK;
}

static double cell_dist(size_t n, size_t m, double s, size_t n2, size_t m2, double s2) {
    return fabs(log2((double)n / (double)n2)) + fabs(log2((double)m / (double)m2)) + 4.0 * fabs(s - s2);
}

int oma_sel_in_tie_set(const oma_sel_decision *d, const char *id) {
    if (strcmp(d->chosen, id) == 0) return 1;
    for (size_t i = 0; i < d->ntie; i++)
        if (strcmp(d->tie_set[i], id) == 0) return 1;
    return 0;
}

int oma_sel_decide(const oma_sel_table *t, const oma_sel_query *q, oma_sel_decision *d) {
    if (!t || !q || !d) return OMA_SEL_E_FORMAT;
    memset(d, 0, sizeof *d);
    d->q = *q;
    /* nearest measured cell */
    double best = INFINITY;
    for (size_t i = 0; i < t->nrows; i++) {
        const oma_sel_row *r = &t->rows[i];
        if (q->run >= 0 && r->run != q->run) continue;
        double dist = cell_dist(q->n, q->m, q->sparsity, r->n, r->m, r->sparsity);
        if (dist < best - 1e-12) {
            best = dist;
            d->cell_n = r->n;
            d->cell_m = r->m;
            d->cell_sparsity = r->sparsity;
        }
    }
    if (!isfinite(best)) return OMA_SEL_E_EMPTY;
    d->exact_cell = best < 1e-9;
    /* candidates: every registered realization */
    for (size_t k = 0; k < oma_rz_count() && d->ncand < OMA_SEL_MAX_CAND; k++) {
        const oma_rz_impl *im = oma_rz_get(k);
        oma_sel_candidate *c = &d->cand[d->ncand++];
        snprintf(c->rz, sizeof c->rz, "%s", im->id);
        c->eligible = 1;
        if (!im->exact) { c->eligible = 0; c->excluded_because = "not exact"; continue; }
        if (q->n > im->max_n) { c->eligible = 0; c->excluded_because = "query n above max_n"; continue; }
        int seen = 0, bad = 0, nall = 0;
        double pf = 0, all_cost[OMA_SEL_MAX_RUNS];
        for (size_t i = 0; i < t->nrows; i++) {
            const oma_sel_row *r = &t->rows[i];
            if (r->n != d->cell_n || r->m != d->cell_m || fabs(r->sparsity - d->cell_sparsity) > 1e-9) continue;
            if (strcmp(r->rz, im->id) != 0) continue;
            int used = q->run < 0 || r->run == q->run;
            if (!r->eligible || !r->verified) { if (used) bad = 1; continue; }
            double pack = q->pack_per_call ? r->pack_ns : 0;
            double cost = r->median_ns + pack;
            /* the band always sees every run of this cell (repeated-run spread) */
            if (nall < OMA_SEL_MAX_RUNS) all_cost[nall++] = cost;
            if (!used) continue;
            double noise = (r->noise_rel * r->median_ns + (q->pack_per_call ? r->pack_noise_rel * pack : 0)) / cost;
            if (c->runs_used < OMA_SEL_MAX_RUNS) c->run_cost_ns[c->runs_used] = cost;
            c->runs_used++;
            c->cost_ns += cost;
            pf += r->pct_floor;
            if (noise > c->within_noise_rel) c->within_noise_rel = noise;
            seen = 1;
        }
        if (bad) { c->eligible = 0; c->excluded_because = "not verified (or not measured) in every run"; continue; }
        if (!seen) { c->eligible = 0; c->excluded_because = "no measurement for this cell"; continue; }
        c->cost_ns /= c->runs_used;
        c->pct_floor = pf / c->runs_used;
        if (nall > 1) {
            double lo = all_cost[0], hi = lo, mean = 0;
            for (int r = 0; r < nall; r++) {
                if (all_cost[r] < lo) lo = all_cost[r];
                if (all_cost[r] > hi) hi = all_cost[r];
                mean += all_cost[r];
            }
            c->cross_run_rel = (hi - lo) / (mean / nall);
        }
    }
    /* winner and runner-up */
    int w = -1, u = -1;
    for (size_t k = 0; k < d->ncand; k++) {
        if (!d->cand[k].eligible) continue;
        if (w < 0 || d->cand[k].cost_ns < d->cand[w].cost_ns) { u = w; w = (int)k; }
        else if (u < 0 || d->cand[k].cost_ns < d->cand[u].cost_ns) u = (int)k;
    }
    if (w < 0) {
        snprintf(d->reason, sizeof d->reason, "no eligible realization");
        return OMA_SEL_E_EMPTY;
    }
    const oma_sel_candidate *W = &d->cand[w];
    snprintf(d->chosen, sizeof d->chosen, "%s", W->rz);
    snprintf(d->cheapest, sizeof d->cheapest, "%s", W->rz);
    d->chosen_cost_ns = d->cheapest_cost_ns = W->cost_ns;
    d->tie_resolution = "none";
    if (u < 0) {
        snprintf(d->reason, sizeof d->reason, "only eligible realization");
        return OMA_SEL_OK;
    }
    const oma_sel_candidate *U = &d->cand[u];
    snprintf(d->runner_up, sizeof d->runner_up, "%s", U->rz);
    d->runner_up_cost_ns = U->cost_ns;
    d->margin_rel = (U->cost_ns - W->cost_ns) / W->cost_ns;
    double band = W->within_noise_rel;
    if (U->within_noise_rel > band) band = U->within_noise_rel;
    if (W->cross_run_rel > band) band = W->cross_run_rel;
    if (U->cross_run_rel > band) band = U->cross_run_rel;
    d->noise_band_rel = band;
    /* tied set: every eligible candidate whose cost is within the band of the
     * cheapest, cheapest first */
    memcpy(d->tie_set[d->ntie++], W->rz, OMA_SEL_ID);
    for (size_t k = 0; k < d->ncand && d->ntie < OMA_SEL_MAX_CAND; k++) {
        const oma_sel_candidate *c = &d->cand[k];
        if (!c->eligible || (int)k == w) continue;
        if ((c->cost_ns - W->cost_ns) / W->cost_ns <= band) snprintf(d->tie_set[d->ntie++], OMA_SEL_ID, "%s", c->rz);
    }
    /* TIE when the tied set has more than one member (the runner-up is in it
     * exactly when margin <= band) */
    d->tie = d->ntie > 1;
    if (!d->tie) {
        snprintf(d->reason, sizeof d->reason,
                 "chosen: cheapest exact verified realization, %.1f%% under %s (margin outside the measured noise band)",
                 100.0 * d->margin_rel, U->rz);
        return OMA_SEL_OK;
    }
    /* ADR 0019 section 9.1: resolved only inside the tied set. The incumbent
     * stands if it is in the tied set; else the digital reference if it is in
     * the tied set; else the cheapest member. A realization outside the tied
     * set is never selected. */
    const oma_sel_candidate *inc = NULL, *ref = NULL;
    for (size_t k = 0; k < d->ncand; k++) {
        const oma_sel_candidate *c = &d->cand[k];
        if (!c->eligible || !oma_sel_in_tie_set(d, c->rz)) continue;
        if (q->incumbent && q->incumbent[0] && strcmp(c->rz, q->incumbent) == 0) inc = c;
        if (strcmp(c->rz, OMA_SEL_REFERENCE) == 0) ref = c;
    }
    const oma_sel_candidate *pick = W;
    if (inc) { pick = inc; d->tie_resolution = "incumbent"; }
    else if (ref) { pick = ref; d->tie_resolution = "reference"; }
    else d->tie_resolution = "cheapest";
    memcpy(d->chosen, pick->rz, sizeof d->chosen); /* both OMA_SEL_ID, NUL-terminated */
    d->chosen_cost_ns = pick->cost_ns;
    snprintf(d->reason, sizeof d->reason,
             "TIE: %s %.1f%% under %s, inside the measured noise band %.1f%%; %s %s selected from the tied set of %zu "
             "(ADR 0019 9.1)",
             W->rz, 100.0 * d->margin_rel, U->rz, 100.0 * band,
             inc ? "incumbent" : ref ? "digital reference" : "cheapest", pick->rz, d->ntie);
    return OMA_SEL_OK;
}

void oma_sel_decision_json(FILE *f, const oma_sel_decision *d, const char *ind) {
    fprintf(f, "%s{\"query\": {\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"pack\": \"%s\", \"runs\": \"%s\"},\n", ind,
            d->q.n, d->q.m, d->q.sparsity, d->q.pack_per_call ? "per_call" : "once_amortized",
            d->q.run < 0 ? "all" : (d->q.run == 0 ? "run1" : "run2"));
    fprintf(f, "%s \"cell\": {\"n\": %zu, \"m\": %zu, \"sparsity\": %.2f, \"exact_cell\": %s},\n", ind, d->cell_n,
            d->cell_m, d->cell_sparsity, d->exact_cell ? "true" : "false");
    fprintf(f, "%s \"chosen\": \"%s\", \"chosen_ns\": %.1f, \"cheapest\": \"%s\", \"cheapest_ns\": %.1f, "
               "\"runner_up\": \"%s\", \"runner_up_ns\": %.1f, \"margin_rel\": %.4f, \"noise_band_rel\": %.4f, "
               "\"verdict\": \"%s\", \"incumbent\": \"%s\", \"tie_resolution\": \"%s\", \"tied_set\": [",
            ind, d->chosen, d->chosen_cost_ns, d->cheapest, d->cheapest_cost_ns, d->runner_up, d->runner_up_cost_ns,
            d->margin_rel, d->noise_band_rel, d->tie ? "TIE" : "CHOSEN",
            (d->q.incumbent && d->q.incumbent[0]) ? d->q.incumbent : "", d->tie_resolution ? d->tie_resolution : "none");
    for (size_t i = 0; i < d->ntie; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", d->tie_set[i]);
    fprintf(f, "],\n%s \"reason\": \"%s\",\n%s \"candidates\": [", ind, d->reason, ind);
    for (size_t k = 0; k < d->ncand; k++) {
        const oma_sel_candidate *c = &d->cand[k];
        if (c->eligible)
            fprintf(f, "%s{\"rz\": \"%s\", \"cost_ns\": %.1f, \"within_noise_rel\": %.4f, \"cross_run_rel\": %.4f, "
                       "\"pct_floor\": %.1f}",
                    k ? ", " : "", c->rz, c->cost_ns, c->within_noise_rel, c->cross_run_rel, c->pct_floor);
        else
            fprintf(f, "%s{\"rz\": \"%s\", \"excluded\": \"%s\"}", k ? ", " : "", c->rz, c->excluded_because);
    }
    fprintf(f, "]}");
}
