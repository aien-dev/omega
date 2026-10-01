/*
 * osc_diag.h -- OSC-1 typed compile diagnostics (docs/osc/OSC-1-DESIGN.md
 * section 6). Every refusal of the front end is exactly one OscDiag.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_DIAG_H
#define OSC_DIAG_H

#include <stdint.h>

typedef enum {
    OSC_DIAG_NONE = 0,
    OSC_DIAG_SYNTAX,
    OSC_DIAG_UNSUPPORTED,
    OSC_DIAG_CAPACITY,
    OSC_DIAG_UNDEFINED_NAME,
    OSC_DIAG_REDEFINED_NAME,
    OSC_DIAG_TYPE_MISMATCH,
    OSC_DIAG_AMBIGUOUS_WIDTH,
    OSC_DIAG_OVERFLOW_UNSAFE,
    OSC_DIAG_UNBOUNDED_LOOP,
    OSC_DIAG_MISSING_RETURN,
    OSC_DIAG_USE_AFTER_MOVE,
    OSC_DIAG_CONDITIONAL_MOVE,
    OSC_DIAG_MUTABLE_ALIAS,
    OSC_DIAG_BORROW_OUTLIVES_OWNER,
    OSC_DIAG_STATIC_OUT_OF_BOUNDS,
    OSC_DIAG_IMMUTABLE_ASSIGN,
    OSC_DIAG_READ_ONLY_BORROW,
    /* appended for OSC-2 contracts (docs/osc/OSC-2-DESIGN.md section 1) */
    OSC_DIAG_CONTRACT_INVALID,     /* ill-formed clause: result in requires / void fn, call, own read */
    OSC_DIAG_CONTRACT_VIOLATION,   /* clause decided false at compile time (constant folding) */
    /* appended for OSC-2 structs (docs/osc/OSC-2-DESIGN.md section 2) */
    OSC_DIAG_UNKNOWN_FIELD,        /* s.f / literal names a field the struct does not declare */
    OSC_DIAG_DUPLICATE_FIELD,      /* field declared twice, or initialised twice in a literal */
    OSC_DIAG_MISSING_FIELD,        /* struct literal leaves a field uninitialised */
    OSC_DIAG_UNDEFINED_TYPE,       /* struct type name not declared (before use) */
    OSC_DIAG_RECURSIVE_STRUCT,     /* struct field names the struct itself */
    /* OSC-2 item 3 (arenas, docs/osc/OSC-2-DESIGN.md section 3), appended */
    OSC_DIAG_ARENA_ESCAPE,         /* a borrow of an arena object would outlive the arena */
    OSC_DIAG_ARENA_MOVE,           /* an arena object is moved (out of the arena, or into own param) */
    OSC_DIAG_ARENA_CAPACITY,       /* arena bound out of range, or allocations statically exceed it */
    OSC_DIAG__COUNT
} OscDiagKind;

typedef struct {
    OscDiagKind kind;
    uint32_t line, col;        /* where the refused construct is (1-based) */
    char object[64];           /* the name involved */
    uint32_t origin_line;      /* where that object / borrow was created (0 = n/a) */
    char other[64];            /* conflicting borrow / name / type, if any */
    char transition[96];       /* the attempted transition as text */
    char message[160];         /* human-readable summary */
} OscDiag;

/* "SYNTAX", ..., "CONTRACT_VIOLATION"; "NONE" for 0; "?" out of range. */
const char *osc_diag_kind_name(int kind);
/* Inverse of osc_diag_kind_name; -1 if unknown. */
int osc_diag_kind_from_name(const char *name);
/* Fill d (zeroed first). Strings may be NULL. message is printf-style. */
void osc_diag_set(OscDiag *d, OscDiagKind kind, uint32_t line, uint32_t col, const char *object,
                  uint32_t origin_line, const char *other, const char *transition, const char *fmt, ...)
    __attribute__((format(printf, 9, 10)));

#endif /* OSC_DIAG_H */
