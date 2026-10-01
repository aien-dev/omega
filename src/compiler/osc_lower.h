/*
 * osc_lower.h -- OSC-1 lowering: checked AST (osc_check annotations) ->
 * OscUnit IR (docs/osc/OSC-1-DESIGN.md section 4). Blocks, short-circuit
 * && / ||, explicit while counters with OSC_I_TRAP 4, RELEASE at every scope
 * exit (reverse declaration order, early returns included), bounds-checked
 * LOAD/STORE (checked by the IR semantics), CALL.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_LOWER_H
#define OSC_LOWER_H

#include "osc_diag.h"
#include "osc_ir.h"
#include "osc_parse.h"

/* 0 ok; -1 with *d filled (CAPACITY when a function exceeds the IR limits). */
int osc_lower(const OscAst *ast, OscUnit *out, OscDiag *d);

#endif /* OSC_LOWER_H */
