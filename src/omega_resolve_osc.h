#ifndef OMEGA_RESOLVE_OSC_H
#define OMEGA_RESOLVE_OSC_H

/* The adapter between the OSC front end's import hook (osc_front.h) and the resolver
 * (omega_resolve.h). Lives in src/, not src/compiler/, so the plain compiler suite never links
 * the resolver. Used by the verified driver (src/oscv_main.c) and by tests. */

#include "omega_resolve.h"
#include "osc_front.h"

typedef struct {
    OmegaResolver resolver;
    const OmegaLock *lock;       /* NULL = no lock file (an empty lock) */
    OmegaClosure closure;        /* filled by a successful call; free with omega_closure_free */
    OmegaResolveError err;       /* filled by a refused call */
    int resolved;                /* 1 once a call succeeded */
    uint32_t n_imports;          /* imports seen by the last call */
} OmegaOscImports;

/* An OscImportResolver. why = "CODE_NAME: message". */
int omega_osc_import_hook(void *ctx, const OscImport *imports, uint32_t n, uint32_t *bad, char *why, size_t why_cap);

#endif /* OMEGA_RESOLVE_OSC_H */
