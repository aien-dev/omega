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

/* VC1 stage 4: import resolution hook. The front end parses `import NAME;` lines and, when a
 * resolver is supplied, hands it ALL imports of the unit (possibly none) right after parsing.
 * It returns 0 to accept, or nonzero with why = "CODE_NAME: plain text" (and *bad = the index of
 * the import it refused, or UINT32_MAX when the refusal is about the unit as a whole). There is no
 * default resolver and no way to turn the hook off: osc_compile (below) passes none and REFUSES a
 * unit that has imports. The resolver is src/omega_resolve.c, linked only by the verified driver. */
typedef int (*OscImportResolver)(void *ctx, const OscImport *imports, uint32_t n, uint32_t *bad, char *why, size_t why_cap);

int osc_compile_imports(const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace_or_NULL,
                        OscImportResolver resolver, void *resolver_ctx);

/* 0 ok (out passed osc_ir_validate); -1 with *diag filled. A unit with imports is refused here
 * (UNSUPPORTED): only osc_compile_imports with a resolver can accept one.
 * trace_or_NULL receives the per-function ownership event trace (also on a
 * refusal: the refused event is the last one, flagged). */
int osc_compile(const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace_or_NULL);

#endif /* OSC_FRONT_H */
