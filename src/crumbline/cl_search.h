/*
 * cl_search.h -- Omega synthesis over a Crumbline crumb (the Omega-side adapter).
 *
 * Inputs: the learner-visible crumb, an operation bank (Omega's base
 * vocabulary from omega_synth_base_prim_defs, optionally extended from the
 * learner's OmegaLibrary) and an oracle handle. The oracle is the ONLY route to
 * the sealed verifier: it takes one candidate step chain and returns one
 * verdict byte. It cannot return held-out inputs, outputs, counterexamples or
 * labels.
 *
 * The search is a level-by-level enumeration of compositions, like
 * omega_synthesize (left untouched: the M9 gates pin its enumeration order),
 * extended with:
 *   - observational-equivalence pruning over visible inputs + public probes;
 *   - acceptance through omega_program_verify (V0 + V2) plus a native vs
 *     reference-semantics differential check before anything is submitted;
 *   - an ambiguity scan: after the first visible fit the search keeps looking
 *     for behaviourally different rival fits before it commits;
 *   - rejected candidates are recorded (event stream) and search continues;
 *   - a robust fallback (best partial fit) for observations no exact
 *     program explains.
 */
#ifndef CL_SEARCH_H
#define CL_SEARCH_H

#include "cl_crumb.h"
#include "cl_program.h"

typedef enum {
    CL_VERDICT_NONE = 0,
    CL_VERDICT_ACCEPT = 1,
    CL_VERDICT_REJECT = 2,
    CL_VERDICT_EXHAUSTED = 3,
} ClVerdict;

#define CL_SUBMIT_ROBUST 0x01u

typedef struct {
    /* Submit one candidate; writes one ClVerdict. */
    int (*submit)(void *ctx, uint32_t node, const ClSteps *cand, uint8_t flags, uint8_t hypotheses, uint8_t *verdict);
    void *ctx;
} ClOracle;

/* Search events (the learner half of the crumbs trace schema). */
enum { CL_EV_EXPAND = 1, CL_EV_SUBMIT = 2 };
enum { CL_PRUNE_NONE = 0, CL_PRUNE_EQUIV = 1, CL_PRUNE_COST = 2, CL_PRUNE_FRONTIER_CAP = 3, CL_PRUNE_STEP_CAP = 4 };
enum {
    CL_VERIFY_NOT_REACHED = 0,
    CL_VERIFY_PASS = 1,
    CL_VERIFY_FAIL_OMEGA = 2,
    CL_VERIFY_FAIL_DIFFERENTIAL = 3,
    CL_VERIFY_FAIL_BUILD = 4,
};
enum { CL_FIT_NONE = 0, CL_FIT_EXACT = 1, CL_FIT_ROBUST = 2 };

typedef struct {
    uint8_t kind, prune, verify, fit;
    uint16_t op_index;
    uint32_t parent, child;
    uint32_t exec_cost;
    uint16_t oracle_index;
} ClEvent;

typedef struct {
    void (*emit)(void *ctx, const ClEvent *ev);
    void *ctx;
} ClEventSink;

typedef struct {
    uint32_t max_candidates;
    uint32_t max_depth;
    uint32_t frontier_cap;
    uint32_t max_oracle_queries;
    uint32_t ambiguity_scan;
    uint32_t robust_min_pct;
    uint32_t max_program_insns;
} ClSearchConfig;

void cl_search_default_config(ClSearchConfig *cfg);

enum { CL_OUTCOME_SOLVED = 1, CL_OUTCOME_UNSOLVED = 2, CL_OUTCOME_UNSUPPORTED = 3 };

typedef struct {
    uint8_t outcome;
    uint32_t solution_node;
    ClSteps solution;
    uint32_t generated, evaluated, expansions;
    uint32_t pruned_equiv, pruned_cost, pruned_cap;
    uint32_t visible_fits, verify_failures, oracle_queries, oracle_rejections;
    uint32_t candidates_to_solution, hypotheses_at_first_submit;
    uint64_t exec_count, wall_ns;
} ClSearchResult;

int cl_search_run(const ClCrumb *crumb, const ClBank *bank, const ClSearchConfig *cfg, const ClOracle *oracle,
                  const ClEventSink *sink /* nullable */, ClSearchResult *res);

/* Widest visible input magnitude in bits (lane 0), used for library scope. */
uint8_t cl_crumb_input_bits(const ClCrumb *c);

#endif /* CL_SEARCH_H */
