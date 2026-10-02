/* omega_resolve_osc.c -- see omega_resolve_osc.h. */
#include "omega_resolve_osc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int omega_osc_import_hook(void *ctx, const OscImport *imports, uint32_t n, uint32_t *bad, char *why, size_t why_cap)
{
    OmegaOscImports *h = ctx;
    const char **names = calloc(n ? n : 1, sizeof *names);
    if (!names) { snprintf(why, why_cap, "NOMEM: out of memory"); return OMEGA_RES_NOMEM; }
    for (uint32_t i = 0; i < n; i++) names[i] = imports[i].name;
    omega_closure_free(&h->closure);
    h->resolved = 0;
    h->n_imports = n;
    int rc = omega_resolve_imports(&h->resolver, h->lock, names, n, &h->closure, &h->err);
    free(names);
    if (rc) {
        *bad = UINT32_MAX;
        for (uint32_t i = 0; i < n; i++) if (strcmp(imports[i].name, h->err.subject) == 0) *bad = i;
        snprintf(why, why_cap, "%s: %s", omega_resolve_code_name(h->err.code), h->err.message);
        return rc;
    }
    h->resolved = 1;
    return 0;
}
