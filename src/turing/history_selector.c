/* TURING control arm: StarPU-style history-based selector
 * (docs/turing/TURING_W0_PROPOSAL.md K.3, Fable item 7).
 *
 * Model: per (spec_id, footprint bucket) running mean of the observed per-call
 * cost. The footprint is what StarPU's history model hashes: the data sizes,
 * here (n, m) plus the pack mode (packing per call is a different codelet
 * cost). Sparsity changes values, not sizes, so it is not in the footprint.
 * Choice: among specs that can execute the query (n <= max_n, StarPU's
 * can_execute), if any has no observation in the bucket, pick the next one in
 * a seeded round-robin order (cold start / calibration); otherwise the minimum
 * running mean (lowest spec index on an exact tie). No filters beyond
 * can_execute, no noise band, no tie rule, no receipts cited.
 */
#include "turing/select.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

uint64_t turing_splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void turing_history_init(turing_history *h, uint64_t seed) {
    memset(h, 0, sizeof *h);
    h->seed = seed;
}

static turing_hist_bucket *bucket_get(turing_history *h, const turing_store *st, uint64_t n, uint64_t m, int pack) {
    for (size_t i = 0; i < h->nbucket; ++i)
        if (h->bucket[i].n == n && h->bucket[i].m == m && h->bucket[i].pack == pack) return &h->bucket[i];
    if (h->nbucket >= TURING_HIST_MAX_BUCKETS) return NULL;
    turing_hist_bucket *b = &h->bucket[h->nbucket++];
    memset(b, 0, sizeof *b);
    b->n = n;
    b->m = m;
    b->pack = pack;
    /* Seeded Fisher-Yates over spec indices, one stream per bucket. */
    uint64_t s = h->seed ^ (n * 0x100000001B3ull) ^ (m << 20) ^ (uint64_t)pack;
    for (size_t k = 0; k < st->nspec; ++k) b->order[k] = (uint32_t)k;
    for (size_t k = st->nspec; k > 1; --k) {
        size_t j = (size_t)(turing_splitmix64(&s) % k);
        uint32_t t = b->order[k - 1];
        b->order[k - 1] = b->order[j];
        b->order[j] = t;
    }
    return b;
}

int turing_history_observe(turing_history *h, const turing_store *st, size_t k, uint64_t n, uint64_t m, int pack,
                           uint64_t cost_ps) {
    if (!h || !st || k >= st->nspec) return -1;
    turing_hist_bucket *b = bucket_get(h, st, n, m, pack);
    if (!b) return -1;
    b->count[k] += 1;
    b->mean_ps[k] += ((double)cost_ps - b->mean_ps[k]) / (double)b->count[k];
    return 0;
}

int turing_history_load(turing_history *h, const turing_store *st, const turing_query *skip) {
    for (size_t i = 0; i < st->nev; ++i) {
        const turing_evidence *e = &st->ev[i];
        if (skip && e->n == skip->n && e->m == skip->m && e->sparsity_milli == skip->sparsity_milli) continue;
        int k = -1;
        for (size_t j = 0; j < st->nspec; ++j)
            if (turing_digest_eq(&st->spec_id[j], &e->spec_id)) k = (int)j;
        if (k < 0) return -1;
        for (int pack = 0; pack < 2; ++pack)
            if (turing_history_observe(h, st, (size_t)k, e->n, e->m, pack, turing_ev_cost(e, pack)) != 0) return -1;
    }
    return 0;
}

int turing_history_select(turing_history *h, const turing_store *st, const turing_query *q, turing_decision *d) {
    if (!h || !st || !q || !d || st->nspec > TURING_MAX_CAND) return -1;
    turing_hist_bucket *b = bucket_get(h, st, q->n, q->m, q->pack);
    if (!b) return -1;
    memset(d, 0, sizeof *d);
    d->contract_digest = st->contract_digest;
    snprintf(d->selector, sizeof d->selector, "%s", TURING_HISTORY_SELECTOR);
    snprintf(d->constraints, sizeof d->constraints, "can_execute (n <= max_n)");
    d->n = d->cell_n = q->n;
    d->m = d->cell_m = q->m;
    d->sparsity_milli = d->cell_sparsity_milli = q->sparsity_milli;
    d->pack = q->pack;
    d->exact_cell = 0; /* bucket = footprint, not a measured cell */
    d->ncand = st->nspec;
    d->chosen = -1;
    snprintf(d->tie_resolution, sizeof d->tie_resolution, "none");
    int can[TURING_MAX_CAND] = {0};
    for (size_t k = 0; k < st->nspec; ++k) {
        d->cand[k] = st->spec_id[k];
        snprintf(d->cand_rz[k], sizeof d->cand_rz[k], "%s", st->spec[k].rz_id);
        can[k] = q->n <= st->spec[k].max_n;
        d->reason[k] = can[k] ? TURING_R_RANKED : TURING_R_MAX_N;
        if (can[k] && b->count[k]) d->cost_ps[k] = (uint64_t)llround(b->mean_ps[k]);
    }
    /* Cold start: next unobserved spec in the seeded round-robin order. */
    for (size_t t = 0; t < st->nspec; ++t) {
        uint32_t k = b->order[(b->cursor + t) % st->nspec];
        if (can[k] && b->count[k] == 0) {
            b->cursor = (uint32_t)((b->cursor + t + 1) % st->nspec);
            d->chosen = (int)k;
            d->reason[k] = TURING_R_EXPLORE;
            snprintf(d->verdict, sizeof d->verdict, "EXPLORE");
            return (int)k;
        }
    }
    int best = -1;
    for (size_t k = 0; k < st->nspec; ++k)
        if (can[k] && (best < 0 || b->mean_ps[k] < b->mean_ps[best])) best = (int)k;
    if (best < 0) {
        snprintf(d->verdict, sizeof d->verdict, "NONE");
        return -1;
    }
    d->chosen = best;
    d->reason[best] = TURING_R_CHOSEN;
    snprintf(d->verdict, sizeof d->verdict, "CHOSEN");
    return best;
}
