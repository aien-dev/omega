/*
 * omega_lower.h -- Omega surface language V0 lowering (spec/omega-language-v0.md).
 *
 * Lowering turns the disposable syntax tree into EXISTING canonical objects:
 *   integer literal -> omega_build_type_uint + omega_build_val_uint
 *   true / false    -> omega_build_type_bool + omega_build_val_bool
 *   a op b          -> omega_build_op_binary(op, OVERFLOW_WRAP, T) + omega_build_apply
 *   fn chain        -> omega_program_build_unary_op + omega_program_compose
 * It never evaluates anything. Every failure leaves the graph exactly as it
 * was (object_count is restored), and identical objects are reused (dedupe
 * by SemanticId), so a line can be re-entered without growing the graph.
 */
#ifndef OMEGA_LANG_LOWER_H
#define OMEGA_LANG_LOWER_H

#include <stddef.h>

#include "omega_types.h"
#include "omega_program.h"
#include "visor.h"
#include "omega_parse.h"

typedef enum {
    OMEGA_LANG_NONE = 0,
    OMEGA_LANG_BINDING,
    OMEGA_LANG_EXPRESSION,
    OMEGA_LANG_PROGRAM
} OmegaLangResultKind;

typedef struct {
    OmegaLangResultKind kind;
    SemanticId id;          /* value/apply id in the graph, or program_id */
    char name[64];          /* binding or program name ("" for expressions) */
    int program_index;      /* index in session->programs, else -1 */
    SemanticId type_id;     /* TYPE object id; all-zero for PROGRAM */
    char type_text[32];     /* "u64", "bool", "u64 -> u64" */
} OmegaLangResult;

/* Evaluate one console line against the session:
 *   `let x: u64 = 7`  -> objects in s->graph (deduped), binding x -> value id.   kind BINDING
 *   `x + y`, `7: u64` -> objects in s->graph (deduped).                           kind EXPRESSION
 *   `fn f(x: u64) -> u64 ... { x * 2 + 1 }` -> OmegaProgram built (not re-realized,
 *                        not verified), stored via visor_program_add, bound.        kind PROGRAM
 *   blank / comment    -> kind NONE, returns 0.
 * Does NOT touch `_` (the console owns visor_set_last).
 * Returns 0 ok, -1 syntax error, -2 unsupported/unrepresentable, -3 capacity.
 * On any error nothing in the session changes and `err` holds a one-line
 * message starting "column N: ". */
int omega_language_eval_line(VisorSession *s, const char *line, OmegaLangResult *out,
                             char *err, size_t err_len);

/* Pure lowering of a parsed OSTMT_EXPR or OSTMT_LET (its right-hand side,
 * typed by the let annotation) into `g`. Names resolve through `b`; `_`
 * resolves through `last` (may be NULL). `info` is optional; when given, its
 * kind/id/type_id/type_text are filled (name and binding are left to the
 * caller). Same return codes as eval_line; on failure g is unchanged. */
int omega_language_lower_to_graph(OmegaGraph *g, const VisorBindings *b, const VisorBinding *last,
                                  const OmegaAst *ast, SemanticId *out_id, OmegaLangResult *info,
                                  char *err, size_t err_len);

/* Pure lowering of a parsed OSTMT_FN into an OmegaProgram (u64 -> u64 only),
 * named after the fn, with the canonical requires/ensures text as contract
 * pre/postcondition (ids via omega_build_constraint_id) and program_id
 * recomputed. `out` is written only on success. */
int omega_language_lower_program(const OmegaAst *ast, OmegaProgram *out, char *err, size_t err_len);

#endif /* OMEGA_LANG_LOWER_H */
