/*
 * osc_front.c -- see osc_front.h.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_front.h"
#include "osc_lex.h"
#include "osc_lower.h"
#include "osc_parse.h"

#include <stdlib.h>
#include <string.h>

int osc_compile(const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace)
{
    memset(diag, 0, sizeof *diag);
    if (trace) { trace->n = 0; trace->overflow = 0; trace->refused = 0; trace->nfuncs = 0; }
    OscToken *toks = malloc(sizeof(OscToken) * OSC_LEX_MAX_TOKENS);
    OscAst *ast = malloc(sizeof *ast);
    int rc = -1;
    if (!toks || !ast) {
        osc_diag_set(diag, OSC_DIAG_CAPACITY, 0, 0, "unit", 0, NULL, "compiler memory", "out of memory");
        goto out;
    }
    memset(ast, 0, sizeof *ast);
    uint32_t ntok = 0;
    if (osc_lex(src, len, toks, OSC_LEX_MAX_TOKENS, &ntok, diag)) goto out;
    ast->src = src;
    ast->toks = toks;
    ast->ntok = ntok;
    if (osc_parse(ast, diag)) goto out;
    if (ast->nfns == 0) {
        osc_diag_set(diag, OSC_DIAG_UNSUPPORTED, 1, 1, "unit", 0, NULL, "empty unit",
                     "a unit must define at least one function");
        goto out;
    }
    if (osc_check(ast, diag, trace)) goto out;
    if (osc_lower(ast, out, diag)) goto out;
    char err[160];
    if (osc_ir_validate(out, err, sizeof err)) {
        osc_diag_set(diag, OSC_DIAG_UNSUPPORTED, 0, 0, "internal", 0, NULL, "IR validation",
                     "internal error: lowered IR refused by the validator: %s", err);
        goto out;
    }
    rc = 0;
out:
    free(toks);
    free(ast);
    return rc;
}
