/*
 * osc_check.h -- OSC-1 name/type resolution, literal typing, compile-time
 * overflow-unsafe checks, loop/return rules and ownership + borrow analysis
 * with lexical lifetimes (docs/osc/OSC-1-DESIGN.md section 3 and 3.1).
 *
 * The checker annotates the AST in place (types, symbols, folded constants,
 * release lists) and records, per function, the ownership event sequence it
 * checked, in the II.11 model event format (src/compiler/model/osc_model.h),
 * so tests can replay it through the executable model.
 *
 * Trace format: a flat list of entries. OSC_TR_EVENT carries one model event.
 * Control flow is lexical, so the trace brackets every branch and loop body
 * with OSC_TR_SAVE ... OSC_TR_RESTORE: a replayer must snapshot the model at
 * SAVE and restore that snapshot at RESTORE (an `if` arm or a loop body leaves
 * no trace in the state, except moves that hold on every non-returning path,
 * which the checker re-emits after the `if` as MOVE obj -> 0). Model ids are
 * per function: object k+1 / borrow k+1 for the checker's k-th object /
 * borrow. A borrow parameter is modelled as ALLOC of a stand-in object
 * followed by a borrow of it at function entry; an own parameter as ALLOC.
 * On an ownership refusal the attempted (refused) event is appended last with
 * refused = 1; for a move inside a loop the trace holds the first iteration's
 * MOVE and the refused second-iteration MOVE; for borrow-outlives-owner it
 * holds the borrow and the refused scope-end RELEASE / END_BORROW.
 *
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_CHECK_H
#define OSC_CHECK_H

#include <stdint.h>
#include "model/osc_model.h"
#include "osc_diag.h"
#include "osc_parse.h"

#define OSC_CHECK_MAX_SYMS   480   /* symbols per function (<= OSC_MAX_VREGS) */
#define OSC_CHECK_MAX_OBJS   256   /* owners per function (static sites) */
#define OSC_CHECK_MAX_BORS   512   /* borrows per function (static sites) */
#define OSC_TRACE_MAX       16384

typedef enum { OSC_TR_EVENT = 0, OSC_TR_SAVE = 1, OSC_TR_RESTORE = 2 } OscTraceOp;

typedef struct {
    uint8_t op;          /* OscTraceOp */
    uint8_t refused;     /* 1 on the event the compiler refused (always last) */
    uint16_t func;       /* function index in the unit */
    uint32_t line;       /* source line */
    OscModelEvent ev;    /* OSC_TR_EVENT only */
} OscTraceEntry;

typedef struct OscTrace {
    uint32_t n;
    uint8_t overflow;    /* entries dropped (OSC_TRACE_MAX) or model id > model capacity */
    uint8_t refused;     /* 1 if the last entry is a refused event */
    uint16_t nfuncs;     /* functions checked (including a refused one) */
    OscTraceEntry e[OSC_TRACE_MAX];
} OscTrace;

/* Check ast (after osc_parse). 0 ok; -1 with *d filled. trace may be NULL. */
int osc_check(OscAst *ast, OscDiag *d, OscTrace *trace);

#endif /* OSC_CHECK_H */
