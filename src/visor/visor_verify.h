/* Omega Visor V1 - lane 4: structured verification report.
 *
 * This is a VIEW over the existing verifier (src/omega_verify.c V0/V1/V2 and
 * the same tiers omega_program_verify runs). It does not add a verifier: each
 * row is one call into an existing check, reported with its own counts.
 *
 * Rows, fixed order:
 *   0 STRUCTURAL   omega_verify_v0_structural (graph for objects; realization for programs)
 *   1 TYPE         omega_validate_object on the queried object / omega_program_validate_contract
 *   2 INVARIANTS   omega_verify_v2_properties (reference-evaluator algebra; graph-independent)
 *   3 REALIZATION  omega_verify_v1_differential, only where its fixed reference
 *                  model (a+b)-c applies: OP_ADD pure-binary realizations (c=0) and
 *                  SUB(ADD(a,b),c) applies. Otherwise NOT_RUN with the reason.
 *   4 AUTHORITY    what the reachable graph REQUIRES (effects / capability refs).
 *                  Reports REQUIRED or NONE. Never grants, mints or checks a grant.
 *   5 MACHINE      realization target profile (and machine id if bound) vs machine model.
 *
 * `passed` is true only when at least one row ran and no row FAILED.
 * Fixed storage, no allocation, fail closed. */
#ifndef VISOR_VERIFY_H
#define VISOR_VERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_types.h"
#include "omega_realize.h"
#include "omega_machine.h"
#include "omega_program.h"

typedef enum { VISOR_CHECK_PASS, VISOR_CHECK_FAIL, VISOR_CHECK_NOT_RUN } VisorCheckStatus;

enum {
    VISOR_ROW_STRUCTURAL = 0,
    VISOR_ROW_TYPE = 1,
    VISOR_ROW_INVARIANTS = 2,
    VISOR_ROW_REALIZATION = 3,
    VISOR_ROW_AUTHORITY = 4,
    VISOR_ROW_MACHINE = 5,
    VISOR_ROW_COUNT = 6
};

typedef struct {
    const char *name;
    VisorCheckStatus status;
    uint32_t checks;
    uint32_t fails;
    char detail[256];
} VisorVerifyRow;

typedef struct {
    VisorVerifyRow rows[8];
    size_t row_count;
    bool passed;
    char first_violation[256];
    char object_involved[72];   /* "sha256:<hex>" only when the failure is on a known object */
    char expected[128];
    char observed[128];
    /* AUTHORITY row facts (requirements only, never a grant). */
    bool authority_required;
    uint32_t effect_count;
    uint32_t capability_ref_count;
} VisorVerifyReport;

/* Returns 0 when the report was produced (read out->passed for the verdict),
 * -1 on invalid arguments (out is zeroed with passed=false when out != NULL). */
int visor_verify_object(const OmegaGraph *g, const SemanticId *id,
                        const RealizationObject *real_or_null,
                        const OmegaMachineGraph *mg_or_null,
                        VisorVerifyReport *out);
int visor_verify_program(const OmegaProgram *p, VisorVerifyReport *out);

const char *visor_check_status_name(VisorCheckStatus s);   /* "PASS" / "FAIL" / "NOT_RUN" */

/* Deterministic renderings. Return bytes written (excl. NUL) or -1 if truncated/invalid. */
int visor_verify_format_text(const VisorVerifyReport *r, char *out, size_t n);
int visor_verify_format_json(const VisorVerifyReport *r, char *out, size_t n);

#endif /* VISOR_VERIFY_H */
