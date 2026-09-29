/* TURING Wave 1 selectors over Field records (docs/turing/TURING_W0_PROPOSAL.md
 * sections K.2, K.3, K.4).
 *
 *  - Field selector (field_select.c): contract / exactness / domain / receipt /
 *    run-verification / contention / tier filters with reason codes, nearest
 *    measured cell (sparsity-aware), noise band, ADR 0019 9.1 tie rule, and a
 *    decision record citing every evidence digest it used.
 *  - Control arm (history_selector.c): StarPU-style history model. Per
 *    (spec_id, footprint bucket) running mean of observed per-call cost; pick
 *    the minimum expected cost; seeded round-robin cold start.
 *
 * Both run post hoc on stored receipts. No timed runs.
 */
#ifndef TURING_SELECT_H
#define TURING_SELECT_H

#include "turing/field.h"

typedef struct {
    uint64_t n, m;
    uint32_t sparsity_milli;
    int pack;              /* TURING_PACK_* */
    int exclude_exact_cell; /* leave-one-cell-out: ignore evidence for (n, m, sparsity) */
} turing_query;

/* ------------------------------------------------------------ Field loop */

#define TURING_FIELD_SELECTOR "turing.field.v0"
#define TURING_REFERENCE_RZ "R1_plain" /* ADR 0019 9.1 digital reference */

/* incumbent_rz may be NULL; supersedes may be NULL. Returns 0 when a
 * realization was chosen, -1 on error, 1 when no candidate survived. */
int turing_field_select(const turing_store *st, const turing_query *q, const char *incumbent_rz,
                        const turing_digest *supersedes, turing_decision *out);

/* ----------------------------------------------------------- control arm */

#define TURING_HISTORY_SELECTOR "turing.history.v0"
#define TURING_HIST_MAX_BUCKETS 64

typedef struct {
    uint64_t n, m;
    int pack;
    uint32_t count[TURING_MAX_SPECS];
    double mean_ps[TURING_MAX_SPECS];
    uint32_t order[TURING_MAX_SPECS]; /* seeded shuffle of spec indices */
    uint32_t cursor;
} turing_hist_bucket;

typedef struct {
    uint64_t seed;
    size_t nbucket;
    turing_hist_bucket bucket[TURING_HIST_MAX_BUCKETS];
} turing_history;

void turing_history_init(turing_history *h, uint64_t seed);
/* Feed one observed per-call cost for spec index k at footprint (n, m, pack). */
int turing_history_observe(turing_history *h, const turing_store *st, size_t k, uint64_t n, uint64_t m, int pack,
                           uint64_t cost_ps);
/* Warm start: observe every evidence row (both pack modes), optionally
 * skipping one exact cell. */
int turing_history_load(turing_history *h, const turing_store *st, const turing_query *skip);
/* Choose. Fills a decision record (it cites no evidence: the control arm
 * keeps only running means). Returns the chosen spec index or -1. */
int turing_history_select(turing_history *h, const turing_store *st, const turing_query *q, turing_decision *out);

/* --------------------------------------------------------------- replay */

uint64_t turing_splitmix64(uint64_t *state);

/* Oracle: mean per-call cost over every verified receipt row of the exact
 * cell; cheapest spec. Returns spec index or -1. */
int turing_oracle(const turing_store *st, const turing_query *q, double *best_ps);
/* Mean per-call cost of spec k at the exact cell, or <0 when unmeasured. */
double turing_cell_cost(const turing_store *st, size_t k, const turing_query *q);

typedef struct {
    size_t decisions;
    double field_mean, field_max;
    double hist_mean, hist_max;
    size_t field_cites_ok;    /* decisions whose cited receipts verify */
    size_t hist_cites_ok;
    size_t agree;             /* same realization chosen */
} turing_regret;

/* Distinct measured cells (n, m, sparsity) in the store, sorted. */
size_t turing_cells(const turing_store *st, turing_query *cells, size_t cap);

/* In-sample (control warm on the same receipts) and leave-one-cell-out over
 * the given queries (pack mode taken from each query). */
int turing_compare(const turing_store *st, const turing_query *qs, size_t nq, int leave_one_out, uint64_t seed,
                   turing_regret *out);
/* Cold online replay: the control arm starts empty and learns from the costs
 * it observes; T queries drawn from qs in seeded shuffled rounds. Field
 * decisions are static (stored evidence). */
int turing_compare_online(const turing_store *st, const turing_query *qs, size_t nq, size_t rounds, uint64_t seed,
                          turing_regret *out);

#endif /* TURING_SELECT_H */
