/*
 * osc_front.h -- OSC-1 front-end driver: source bytes -> validated OscUnit.
 * lex -> parse -> check (types, constants, loops, returns, ownership) ->
 * lower -> osc_ir_validate. Deterministic: output is a pure function of the
 * source bytes; all scratch memory is freed before returning.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_FRONT_H
#define OSC_FRONT_H

#include <stddef.h>
#include "osc_check.h"
#include "osc_diag.h"
#include "osc_ir.h"

/* 0 ok (out passed osc_ir_validate); -1 with *diag filled.
 * trace_or_NULL receives the per-function ownership event trace (also on a
 * refusal: the refused event is the last one, flagged). */
int osc_compile(const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace_or_NULL);

#endif /* OSC_FRONT_H */
