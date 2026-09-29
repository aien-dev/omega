/* MA-2 stand-in selector for Omega-X realizations.
 * spec/mixed-algebra-ma2.md
 *
 * STAND-IN: this selector is not wired to rx_costmodel (src/runtime). It reads
 * the measured cost table from MA-2 benchmark receipts (never hard-coded
 * costs), filters candidates by the exact contract, and picks the cheapest
 * (subject to the tie rule below).
 *
 * Exact-contract filter: a realization is a candidate only if the registry
 * marks it exact, the receipt marks it verified (bit-identical to the oracle)
 * in every run used, and the query's n is within its max_n.
 *
 * Cost of a candidate in one run: median ns per call ("pack once,
 * amortized") or median pack ns + median run ns ("pack per call"). With
 * several runs, cost = mean over runs.
 * Noise band (relative): max, over the winner and the runner-up, of
 *   - within-run spread (q75 - q25) / median of the timed blocks, and
 *   - across-run spread (max - min) / mean of the cell's costs over EVERY
 *     run in the table (>= 2 runs), also when the costs come from one run.
 * With pack per call the within-run spread weights run and pack spreads by
 * their share of the cost.
 * margin = (cost runner-up - cost winner) / cost winner.
 * TIE when margin <= noise band; the record then lists every candidate
 * inside the band of the cheapest, the cheapest included (tie set).
 * Tie rule (ADR 0019 section 9.1): on TIE the incumbent selection stands
 * (query.incumbent, when it names an eligible realization); with no
 * incumbent the digital reference realization OMA_SEL_REFERENCE is selected,
 * whether or not it is inside the band. Only if the reference is not
 * eligible for the query does the cheapest stand. The decision records which
 * rule applied in tie_resolution. Without a TIE the cheapest is chosen. */
#ifndef OMA_SELECT_H
#define OMA_SELECT_H

#include <stddef.h>
#include <stdio.h>

#define OMA_SEL_MAX_ROWS 2048
#define OMA_SEL_MAX_RUNS 4
#define OMA_SEL_MAX_CAND 16
#define OMA_SEL_ID 32
/* Digital reference realization (ADR 0019 section 9.1 tie rule). */
#define OMA_SEL_REFERENCE "R1_plain"

enum { OMA_SEL_OK = 0, OMA_SEL_E_IO = -1, OMA_SEL_E_FORMAT = -2, OMA_SEL_E_FULL = -3, OMA_SEL_E_EMPTY = -4 };

typedef struct {
    int run;                /* index of the receipt it came from */
    size_t n, m;
    double sparsity;
    char rz[OMA_SEL_ID];
    int eligible, verified;
    double median_ns, min_ns, noise_rel, pack_ns, pack_noise_rel, pct_floor;
} oma_sel_row;

typedef struct {
    int nruns;
    char run_id[OMA_SEL_MAX_RUNS][96];
    char source[OMA_SEL_MAX_RUNS][256];
    size_t nrows;
    oma_sel_row rows[OMA_SEL_MAX_ROWS];
} oma_sel_table;

typedef struct {
    size_t n, m;
    double sparsity;
    int pack_per_call;      /* 0: pack once, amortized; 1: pack every call */
    int run;                /* -1: all runs in the table; else that run only */
    const char *incumbent;  /* NULL or "": no incumbent selection */
} oma_sel_query;

typedef struct {
    char rz[OMA_SEL_ID];
    int eligible;
    const char *excluded_because; /* NULL when eligible */
    double cost_ns;               /* mean over runs used */
    double run_cost_ns[OMA_SEL_MAX_RUNS];
    int runs_used;
    double within_noise_rel;      /* max over runs used */
    double cross_run_rel;         /* over all runs in the table; 0 with one run */
    double pct_floor;             /* mean over runs used, amortized run only */
} oma_sel_candidate;

typedef struct {
    oma_sel_query q;
    size_t cell_n, cell_m;        /* measured cell the costs came from */
    double cell_sparsity;
    int exact_cell;               /* 1 if the query hit a measured cell */
    char chosen[OMA_SEL_ID];
    char cheapest[OMA_SEL_ID];    /* lowest measured cost */
    char runner_up[OMA_SEL_ID];   /* second lowest; margin is runner-up vs cheapest */
    double chosen_cost_ns, cheapest_cost_ns, runner_up_cost_ns;
    double margin_rel, noise_band_rel;
    int tie;
    /* "none" (no TIE: cheapest), "incumbent", "reference", or
     * "cheapest_reference_ineligible" (TIE, no eligible incumbent or reference) */
    const char *tie_resolution;
    char tie_set[OMA_SEL_MAX_CAND][OMA_SEL_ID];
    size_t ntie;
    size_t ncand;
    oma_sel_candidate cand[OMA_SEL_MAX_CAND];
    char reason[256];
} oma_sel_decision;

void oma_sel_init(oma_sel_table *t);
/* Append one MA-2 bench receipt as a new run. */
int oma_sel_load(oma_sel_table *t, const char *receipt_path);
int oma_sel_decide(const oma_sel_table *t, const oma_sel_query *q, oma_sel_decision *d);
/* 1 if id is in the decision's tie set (or is the chosen one). */
int oma_sel_in_tie_set(const oma_sel_decision *d, const char *id);
void oma_sel_decision_json(FILE *f, const oma_sel_decision *d, const char *indent);

#endif /* OMA_SELECT_H */
