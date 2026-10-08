/*
 * osh_core.h -- loads the three OSC units of the shell core (lexer, parser, expander) and calls them through the
 * checked entries (aien-architecture#158). The units borrow two slices per call: input bytes and the 11456-cell
 * workspace. Native calls go through osc_native_call (refuses overlapping, NULL or misaligned slices), interpreter
 * calls through osc_interp_run_prevalidated after osc_ir_slice_args_ok. Host code is scaffolding (ADR 0024).
 */
#ifndef OSH_CORE_H
#define OSH_CORE_H
#include <stddef.h>
#include <stdint.h>

#include "osc_cg.h"
#include "osc_ir.h"
#include "osc_native.h"
#include "osc_rt.h"

enum { OSH_U_LEX = 0, OSH_U_PARSE = 1, OSH_U_EXPAND = 2, OSH_U_COUNT = 3 };
#define OSH_CORE_FAULT (~(uint64_t)0) /* a trap, or the checked entry refused the arguments */

typedef struct {
    OscUnit *U;
    OscRt *RI, *RN;
    OscCode code;
    OscNative nm;
    int fi;
} OshCoreUnit;

typedef struct {
    OshCoreUnit u[OSH_U_COUNT];
    unsigned long calls_interp, calls_native, faults;
} OshCore;

/* Compile the three sources (lexer, parser, expander order). 0 ok, else -1 with a message in err. */
int osh_core_init(OshCore *c, const char *const src[OSH_U_COUNT], const size_t len[OSH_U_COUNT], char *err, size_t errsz);
void osh_core_free(OshCore *c);

/* One entry call. native: 0 interpreter, 1 native AArch64. inp may be NULL when n == 0. wn = workspace cells.
 * Returns the unit's status, or OSH_CORE_FAULT. */
uint64_t osh_core_call(OshCore *c, int unit, int native, const uint8_t *inp, size_t n, uint64_t *w, size_t wn);

/* Printable name of a refusal code ("CMDSUB"), or NULL for a number that is not a named code. */
const char *osh_code_name(unsigned code);
#endif
