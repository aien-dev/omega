#ifndef OMEGA_SEARCHTRACE_HOOK_H
#define OMEGA_SEARCHTRACE_HOOK_H
/* M23 search-trace recorder hook (G1 corpus capture).
 *
 * One function-pointer type shared by the two search sites:
 *   src/omega_synthesis.c       one event per candidate the program search touches
 *   src/omega_realize_synth.c   one event per omega_synthesize_realization call
 *
 * Pattern of the rx_world recorder (omega#114): at most one recorder per site,
 * installed explicitly, off by default. The hook is per thread (_Thread_local):
 * a search on another thread never sees it. With no hook installed each site
 * does one null test and nothing else, so search behaviour and results are
 * unchanged. The recorder observes only: it gets const pointers that are valid
 * for the duration of the call, cannot veto, prune or alter the search, and
 * must not install or clear hooks from inside the callback. It MAY call
 * omega_synthesize_realization (the realize hook then fires nested).
 */
#include <stdint.h>
#include <stddef.h>

typedef enum {
    ST_EV_SYNTH_CANDIDATE = 1,   /* omega_synthesis.c */
    ST_EV_REALIZATION     = 2    /* omega_realize_synth.c */
} StEventKind;

/* Why a candidate left the search (or ST_PRUNE_NONE when it was evaluated). */
typedef enum {
    ST_PRUNE_NONE       = 0,
    ST_PRUNE_TYPE       = 1,     /* composition refused (child absent) */
    ST_PRUNE_COST       = 2,     /* insn_count over task budget or config max_cost */
    ST_PRUNE_EQUIV      = 3,     /* observationally equivalent to a cheaper/equal seen candidate */
    ST_PRUNE_BUDGET     = 4      /* global max_candidates cutoff reached; search stops */
} StPruneReason;

/* Verifier result for an evaluated candidate. */
typedef enum {
    ST_VERDICT_NONE        = 0,  /* not evaluated (pruned) */
    ST_VERDICT_REJECT      = 1,  /* task evaluation: wrong on an example (or eval error) */
    ST_VERDICT_SOLVED      = 2,  /* task solved and omega_program_verify passed */
    ST_VERDICT_VERIFY_FAIL = 3   /* task solved but omega_program_verify refused */
} StVerdict;

typedef struct {
    uint32_t depth;              /* 1, 2 or 3 */
    int32_t  parent_index;       /* index into the previous depth's kept list; -1 at depth 1 */
    int32_t  prim_index;         /* bank primitive applied (depth 1: the primitive itself) */
    const void *parent;          /* const OmegaProgram *; NULL at depth 1 */
    const void *prim;            /* const OmegaProgram * (bank entry) */
    const void *child;           /* const OmegaProgram *; NULL when ST_PRUNE_TYPE or ST_PRUNE_BUDGET */
    uint32_t prune;              /* StPruneReason */
    uint32_t verdict;            /* StVerdict */
    int32_t  eval_rc;            /* omega_task_evaluate_candidate rc (0 when not evaluated) */
    int32_t  verify_rc;          /* omega_program_verify rc (0 when not run) */
    uint32_t has_signature;      /* 1 when the equivalence signature was computed */
    uint8_t  signature[32];      /* probe-output signature (only when has_signature) */
} StSynthEvent;

typedef struct {
    const void *program;         /* const OmegaProgram * */
    const void *machine;         /* const OmegaMachineGraph * */
    const void *result;          /* const RealizationSynthesisResult * */
    int32_t rc;                  /* return code of omega_synthesize_realization */
} StRealizeEvent;

typedef struct {
    uint32_t kind;               /* StEventKind */
    union {
        StSynthEvent synth;
        StRealizeEvent realize;
    } u;
} StEvent;

typedef void (*StHookFn)(void *ctx, const StEvent *ev);

/* Install the recorder for this thread at one site. Returns 0, or -1 if a hook
 * is already installed (at most one) or fn is null. */
int  omega_synth_set_trace_hook(StHookFn fn, void *ctx);
int  omega_realize_set_trace_hook(StHookFn fn, void *ctx);
/* Remove the hook installed with ctx. Returns 0, or -1 if ctx is not the installed one. */
int  omega_synth_clear_trace_hook(void *ctx);
int  omega_realize_clear_trace_hook(void *ctx);

#endif /* OMEGA_SEARCHTRACE_HOOK_H */
