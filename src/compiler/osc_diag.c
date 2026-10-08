/*
 * osc_diag.c -- OSC-1 diagnostic kind names and filling helper.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const kind_names[OSC_DIAG__COUNT] = {
    "NONE", "SYNTAX", "UNSUPPORTED", "CAPACITY", "UNDEFINED_NAME", "REDEFINED_NAME", "TYPE_MISMATCH",
    "AMBIGUOUS_WIDTH", "OVERFLOW_UNSAFE", "UNBOUNDED_LOOP", "MISSING_RETURN", "USE_AFTER_MOVE",
    "CONDITIONAL_MOVE", "MUTABLE_ALIAS", "BORROW_OUTLIVES_OWNER", "STATIC_OUT_OF_BOUNDS", "IMMUTABLE_ASSIGN",
    "READ_ONLY_BORROW", "CONTRACT_INVALID", "CONTRACT_VIOLATION", "UNKNOWN_FIELD", "DUPLICATE_FIELD",
    "MISSING_FIELD", "UNDEFINED_TYPE", "RECURSIVE_STRUCT", "ARENA_ESCAPE", "ARENA_MOVE", "ARENA_CAPACITY",
    "STALE_HANDLE", "WRONG_POOL", "POOL_CAPACITY", "GENERATION_EXHAUSTED", "IMPORT_REFUSED",
    "SLICE_ESCAPE", "SLICE_VALUE", "SLICE_WRITE", "SLICE_PARAMS"};

const char *osc_diag_kind_name(int kind)
{
    if (kind < 0 || kind >= OSC_DIAG__COUNT) return "?";
    return kind_names[kind];
}

int osc_diag_kind_from_name(const char *name)
{
    for (int k = 0; k < OSC_DIAG__COUNT; k++)
        if (strcmp(kind_names[k], name) == 0) return k;
    return -1;
}

static void cpy(char *dst, size_t cap, const char *s)
{
    if (!s) { dst[0] = 0; return; }
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

void osc_diag_set(OscDiag *d, OscDiagKind kind, uint32_t line, uint32_t col, const char *object,
                  uint32_t origin_line, const char *other, const char *transition, const char *fmt, ...)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->kind = kind;
    d->line = line;
    d->col = col;
    d->origin_line = origin_line;
    cpy(d->object, sizeof d->object, object);
    cpy(d->other, sizeof d->other, other);
    cpy(d->transition, sizeof d->transition, transition);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d->message, sizeof d->message, fmt, ap);
        va_end(ap);
    }
}
