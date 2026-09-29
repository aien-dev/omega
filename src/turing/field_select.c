/* TURING Field selector v1: the record-keeping layer
 * (docs/turing/TURING_W0_PROPOSAL.md K.2, K.7).
 *
 * Selection is NOT novel here: the choice is made by turing_rank_min_cost, the
 * control arm's rule (history_selector.c). What the Field adds is the record.
 *
 * Rule, in order:
 *  1. Candidates = every spec in the store. Recorded filters with reason
 *     codes: CONTRACT_MISMATCH (spec names another contract digest),
 *     NOT_EXACT, MAX_N (query n above the spec's max_n).
 *  2. Usable evidence = rows whose receipt file still hashes to the digest the
 *     row records (re-hashed at decision time), minus the exact query cell
 *     under leave-one-cell-out.
 *  3. Footprint = (n, m) as in the control arm (sparsity changes values, not
 *     sizes). The query's own footprint when it has usable evidence for an
 *     eligible candidate, else the nearest measured one by |log2 n ratio| +
 *     |log2 m ratio|; ties -> smallest (n, m).
 *  4. Per candidate over its rows in that footprint: NO_EVIDENCE,
 *     UNVERIFIED_RUN (a row not oracle-verified), CONTENTION_FORCED (a forced
 *     block), TIER (tier text not E2/E3/E4). Filters exclude; they never
 *     reorder.
 *  5. Ranking: the surviving rows, in store order, feed one ranking-core
 *     bucket; turing_rank_min_cost picks. On the stored grid, with every
 *     filter passing, this is bit-for-bit the control arm's warm choice.
 *  6. Recorded, not used to choose: margin to the runner-up and the noise band
 *     (largest within-run spread of the chosen and runner-up rows).
 *  7. The decision cites every evidence row the ranking used.
 */
#include "turing/select.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static double foot_dist(const turing_query *q, uint64_t n, uint64_t m) {
    return fabs(log2((double)n / (double)q->n)) + fabs(log2((double)m / (double)q->m));
}

static int is_excluded(const turing_query *q, const turing_evidence *e) {
    return q->exclude_exact_cell && e->n == q->n && e->m == q->m && e->sparsity_milli == q->sparsity_milli;
}

static int tier_ok(const char *t) { return !strcmp(t, "E2") || !strcmp(t, "E3") || !strcmp(t, "E4"); }

/* Within-run relative spread of one row for a pack mode. */
static double row_noise(const turing_evidence *e, int pack) {
    double c = (double)turing_ev_cost(e, pack);
    if (pack == TURING_PACK_PER_CALL && c > 0)
        return (e->noise_ppm / 1e6 * (double)e->median_ps + e->pack_noise_ppm / 1e6 * (double)e->pack_ps) / c;
    return e->noise_ppm / 1e6;
}

int turing_field_select(const turing_store *st, const turing_query *q, const turing_digest *supersedes,
                        turing_decision *d) {
    if (!st || !q || !d || q->n == 0 || q->m == 0 || st->nspec > TURING_MAX_CAND) return -1;
    memset(d, 0, sizeof *d);
    d->contract_digest = st->contract_digest;
    snprintf(d->selector, sizeof d->selector, "%s", TURING_FIELD_SELECTOR);
    snprintf(d->constraints, sizeof d->constraints,
             "exact contract; tier in {E2,E3,E4}; receipts verify; rank = min mean cost per (n,m,pack)");
    d->n = q->n;
    d->m = q->m;
    d->sparsity_milli = q->sparsity_milli;
    d->pack = q->pack;
    d->chosen = -1;
    if (supersedes) {
        d->has_supersedes = 1;
        d->supersedes = *supersedes;
    }
    snprintf(d->tie_resolution, sizeof d->tie_resolution, "none");

    /* Receipts re-hashed now. */
    int receipt_ok[TURING_MAX_RECEIPTS] = {0};
    for (size_t r = 0; r < st->nreceipt; ++r) {
        turing_digest fd;
        receipt_ok[r] = turing_file_digest(st->receipt[r].path, &fd) == 0 && turing_digest_eq(&fd, &st->receipt[r].digest);
    }

    /* 1. Recorded contract / exactness / domain filters. */
    int eligible[TURING_MAX_CAND] = {0};
    d->ncand = st->nspec;
    for (size_t k = 0; k < st->nspec; ++k) {
        const turing_rz_spec *s = &st->spec[k];
        d->cand[k] = st->spec_id[k];
        snprintf(d->cand_rz[k], sizeof d->cand_rz[k], "%s", s->rz_id);
        if (!turing_digest_eq(&s->contract_digest, &st->contract_digest))
            d->reason[k] = TURING_R_CONTRACT_MISMATCH;
        else if (s->exact != 1)
            d->reason[k] = TURING_R_NOT_EXACT;
        else if (q->n > s->max_n)
            d->reason[k] = TURING_R_MAX_N;
        else {
            d->reason[k] = TURING_R_NO_EVIDENCE;
            eligible[k] = 1;
        }
    }

    /* 2. Usable evidence rows; spec index per row. */
    unsigned char usable[TURING_MAX_EVIDENCE];
    int row_spec[TURING_MAX_EVIDENCE];
    int any_unusable_receipt[TURING_MAX_CAND] = {0};
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        usable[i] = 0;
        row_spec[i] = -1;
        for (size_t k = 0; k < st->nspec; ++k)
            if (turing_digest_eq(&st->spec_id[k], &e->spec_id)) row_spec[i] = (int)k;
        if (row_spec[i] < 0 || is_excluded(q, e)) continue;
        int ok = 0;
        for (size_t r = 0; r < st->nreceipt; ++r)
            if (turing_digest_eq(&st->receipt[r].digest, &e->receipt_digest)) ok = receipt_ok[r];
        if (!ok) any_unusable_receipt[row_spec[i]] = 1;
        else if (eligible[row_spec[i]]) usable[i] = 1;
    }

    /* 3. Footprint. */
    int have = 0;
    double best = 0;
    for (size_t i = 0; i < st->nev; ++i) {
        if (!usable[i]) continue;
        const turing_evidence *e = &st->ev[i];
        double dd = foot_dist(q, e->n, e->m);
        if (!have || dd < best - 1e-12 ||
            (fabs(dd - best) <= 1e-12 && (e->n < d->cell_n || (e->n == d->cell_n && e->m < d->cell_m)))) {
            have = 1;
            best = dd;
            d->cell_n = e->n;
            d->cell_m = e->m;
        }
    }
    d->cell_sparsity_milli = TURING_SPARSITY_ANY;
    if (!have) {
        for (size_t k = 0; k < st->nspec; ++k)
            if (eligible[k] && any_unusable_receipt[k]) d->reason[k] = TURING_R_RECEIPT_UNVERIFIED;
        snprintf(d->verdict, sizeof d->verdict, "NONE");
        return 1;
    }
    d->exact_cell = d->cell_n == q->n && d->cell_m == q->m;

    /* 4. Per-candidate run / contention / tier filters over the footprint. */
    int rows[TURING_MAX_CAND] = {0}, bad[TURING_MAX_CAND];
    for (size_t k = 0; k < st->nspec; ++k) bad[k] = -1;
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (!usable[i] || e->n != d->cell_n || e->m != d->cell_m) continue;
        int k = row_spec[i];
        rows[k]++;
        if (!e->verified) bad[k] = TURING_R_UNVERIFIED_RUN;
        else if (e->forced) bad[k] = TURING_R_CONTENTION_FORCED;
        else if (!tier_ok(e->tier)) bad[k] = TURING_R_TIER;
    }
    int ranked[TURING_MAX_CAND] = {0};
    for (size_t k = 0; k < st->nspec; ++k) {
        if (!eligible[k]) continue;
        if (rows[k] == 0)
            d->reason[k] = any_unusable_receipt[k] ? TURING_R_RECEIPT_UNVERIFIED : TURING_R_NO_EVIDENCE;
        else if (bad[k] >= 0)
            d->reason[k] = (turing_reason)bad[k];
        else
            ranked[k] = 1, d->reason[k] = TURING_R_RANKED;
    }

    /* 5. Ranking core over the surviving rows, in store order; cite each. */
    turing_hist_bucket b;
    turing_bucket_reset(&b, d->cell_n, d->cell_m, q->pack);
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (!usable[i] || e->n != d->cell_n || e->m != d->cell_m || !ranked[row_spec[i]]) continue;
        if (d->ncite >= TURING_MAX_CITE) return -1;
        turing_bucket_observe(&b, (size_t)row_spec[i], turing_ev_cost(e, q->pack));
        d->cite[d->ncite++] = st->ev_id[i];
    }
    int chosen = turing_rank_min_cost(&b, ranked, st->nspec);
    if (chosen < 0) {
        snprintf(d->verdict, sizeof d->verdict, "NONE");
        d->ncite = 0;
        return 1;
    }
    for (size_t k = 0; k < st->nspec; ++k)
        if (ranked[k]) d->cost_ps[k] = (uint64_t)llround(b.mean_ps[k]);

    /* 6. Recorded margin and noise band (not used to choose). */
    int runner = -1;
    for (size_t k = 0; k < st->nspec; ++k)
        if (ranked[k] && (int)k != chosen && (runner < 0 || b.mean_ps[k] < b.mean_ps[runner])) runner = (int)k;
    double band = 0;
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (!usable[i] || e->n != d->cell_n || e->m != d->cell_m) continue;
        if (row_spec[i] != chosen && row_spec[i] != runner) continue;
        double w = row_noise(e, q->pack);
        if (w > band) band = w;
    }
    d->band_ppm = (uint32_t)llround(band * 1e6);
    d->margin_ppm = runner >= 0 ? (uint32_t)llround((b.mean_ps[runner] / b.mean_ps[chosen] - 1.0) * 1e6) : 0;

    d->chosen = chosen;
    d->reason[chosen] = TURING_R_CHOSEN;
    snprintf(d->verdict, sizeof d->verdict, "CHOSEN");
    return 0;
}
