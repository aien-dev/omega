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

int osc_compile_imports_a(const OscAlloc *a, const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace,
                          OscImportResolver resolver, void *resolver_ctx)
{
    if (!a) a = osc_alloc_libc();
    memset(diag, 0, sizeof *diag);
    if (trace) { trace->n = 0; trace->overflow = 0; trace->refused = 0; trace->nfuncs = 0; }
    const size_t toks_sz = sizeof(OscToken) * OSC_LEX_MAX_TOKENS;
    OscToken *toks = a->alloc(a->ctx, toks_sz);
    OscAst *ast = a->alloc(a->ctx, sizeof *ast);
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
    if (resolver) {
        uint32_t bad = UINT32_MAX;
        char why[160];
        why[0] = 0;
        if (resolver(resolver_ctx, ast->imports, ast->nimports, &bad, why, sizeof why)) {
            const OscImport *im = bad < ast->nimports ? &ast->imports[bad] : NULL;
            char code[96];
            size_t cl = strcspn(why, ":");
            if (cl >= sizeof code) cl = sizeof code - 1;
            memcpy(code, why, cl);
            code[cl] = 0;
            osc_diag_set(diag, OSC_DIAG_IMPORT_REFUSED, im ? im->line : 1, im ? im->col : 1, im ? im->name : "imports", 0,
                         NULL, code, "%s", why[0] ? why : "import refused");
            goto out;
        }
    } else if (ast->nimports) {
        osc_diag_set(diag, OSC_DIAG_UNSUPPORTED, ast->imports[0].line, ast->imports[0].col, ast->imports[0].name, 0,
                     NULL, "import", "imports are resolved only by the verified driver (oscv); this entry point has no resolver");
        goto out;
    }
    if (ast->nfns == 0) {
        osc_diag_set(diag, OSC_DIAG_UNSUPPORTED, 1, 1, "unit", 0, NULL, "empty unit",
                     "a unit must define at least one function");
        goto out;
    }
    if (osc_check(a, ast, diag, trace)) goto out;
    if (osc_lower(a, ast, out, diag)) goto out;
    char err[160];
    int vr = osc_ir_validate_a(a, out, err, sizeof err);
    if (vr == -2) {
        osc_diag_set(diag, OSC_DIAG_CAPACITY, 0, 0, "unit", 0, NULL, "validator memory", "out of memory");
        goto out;
    }
    if (vr) {
        osc_diag_set(diag, OSC_DIAG_UNSUPPORTED, 0, 0, "internal", 0, NULL, "IR validation",
                     "internal error: lowered IR refused by the validator: %s", err);
        goto out;
    }
    rc = 0;
out:
    a->free(a->ctx, toks, toks_sz);
    a->free(a->ctx, ast, sizeof *ast);
    return rc;
}

int osc_compile_imports(const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace,
                        OscImportResolver resolver, void *resolver_ctx)
{
    return osc_compile_imports_a(osc_alloc_libc(), src, len, out, diag, trace, resolver, resolver_ctx);
}

int osc_compile_a(const OscAlloc *a, const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace)
{
    return osc_compile_imports_a(a, src, len, out, diag, trace, NULL, NULL);
}

int osc_compile(const char *src, size_t len, OscUnit *out, OscDiag *diag, OscTrace *trace)
{
    return osc_compile_imports_a(osc_alloc_libc(), src, len, out, diag, trace, NULL, NULL);
}
