/* osh_core.c -- see osh_core.h. */
#include "osh_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_front.h"
#include "osc_interp.h"

static const char *const k_entry[OSH_U_COUNT] = {"lex_run", "parse_run", "expand_run"};

int osh_core_init(OshCore *c, const char *const src[OSH_U_COUNT], const size_t len[OSH_U_COUNT], char *err, size_t errsz)
{
    memset(c, 0, sizeof *c);
    for (int i = 0; i < OSH_U_COUNT; i++) {
        OshCoreUnit *u = &c->u[i];
        u->U = calloc(1, sizeof *u->U);
        u->RI = calloc(1, sizeof *u->RI);
        u->RN = calloc(1, sizeof *u->RN);
        if (!u->U || !u->RI || !u->RN) { snprintf(err, errsz, "out of memory"); return -1; }
        OscDiag d;
        if (osc_compile(src[i], len[i], u->U, &d, NULL) != 0) {
            snprintf(err, errsz, "%s unit refused: %s line %u: %s", k_entry[i], osc_diag_kind_name(d.kind), d.line, d.message);
            return -1;
        }
        char e2[200];
        if (osc_ir_validate(u->U, e2, sizeof e2) != 0) { snprintf(err, errsz, "%s unit invalid: %s", k_entry[i], e2); return -1; }
        u->fi = -1;
        for (int f = 0; f < u->U->nfuncs; f++)
            if (strcmp(u->U->funcs[f].name, k_entry[i]) == 0) u->fi = f;
        if (u->fi < 0) { snprintf(err, errsz, "no entry %s", k_entry[i]); return -1; }
        if (osc_cg_compile(u->U, &u->code, e2, sizeof e2) != 0) { snprintf(err, errsz, "%s codegen: %s", k_entry[i], e2); return -1; }
        int nm = osc_native_map(&u->nm, u->code.code, u->code.len);
        if (nm != 0) { snprintf(err, errsz, "%s native map failed (%d)", k_entry[i], nm); return -1; }
        osc_rt_init(u->RI);
        osc_rt_init(u->RN);
    }
    return 0;
}

void osh_core_free(OshCore *c)
{
    for (int i = 0; i < OSH_U_COUNT; i++) {
        OshCoreUnit *u = &c->u[i];
        if (u->code.code) {
            osc_native_unmap(&u->nm);
            osc_cg_free(&u->code);
        }
        free(u->U);
        free(u->RI);
        free(u->RN);
    }
    memset(c, 0, sizeof *c);
}

uint64_t osh_core_call(OshCore *c, int unit, int native, const uint8_t *inp, size_t n, uint64_t *w, size_t wn)
{
    OshCoreUnit *u = &c->u[unit];
    uint64_t args[4] = {(uint64_t)(uintptr_t)inp, n, (uint64_t)(uintptr_t)w, wn}, ret = 0;
    int trap;
    if (osc_ir_slice_args_ok(&u->U->funcs[u->fi], args, 4) != 0) { c->faults++; return OSH_CORE_FAULT; }
    if (!native) {
        osc_rt_reset(u->RI);
        trap = osc_interp_run_prevalidated(u->U, u->fi, args, 4, u->RI, &ret);
        c->calls_interp++;
    } else {
        osc_rt_reset(u->RN);
        trap = osc_native_call(u->U, u->fi, &u->nm, u->code.entry[u->fi], u->RN, args, 4, &ret);
        c->calls_native++;
    }
    if (trap != 0) { c->faults++; return OSH_CORE_FAULT; }
    return ret;
}
