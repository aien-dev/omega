#ifndef OMEGA_SELF_HOST_H
#define OMEGA_SELF_HOST_H

#include "omega_types.h"
#include "omega_realize.h"
#include <stdbool.h>

#define OP_COMPILE_REALIZER OP_COMPILE

/* Builds the canonical OMEGA semantic compiler graph G_C */
int omega_build_compiler_graph(OmegaGraph *g, SemanticId *out_semantic_id);

/* Reference C0 lowering of compiler graph G_C into AArch64 machine bytes C1 */
int omega_self_host_compile_c0(const OmegaGraph *g, const SemanticId *g_c_id, RealizationObject *out_c1);

/* Executes native AArch64 compiler in memory:
 * runs compiler->code_bytes on in_omg0 wire bytes, producing out_real */
int omega_self_host_run_native_compiler(const RealizationObject *compiler,
                                        const uint8_t *in_omg0, size_t in_len,
                                        RealizationObject *out_real);

/* Runs the 3-generation bootstrap sequence:
 * C0(G_C) -> C1
 * C1(G_C) -> C2
 * C2(G_C) -> C3
 * Verifies byte-for-byte fixed point C1 == C2 == C3 */
int omega_self_host_bootstrap_sequence(RealizationObject *out_c1,
                                       RealizationObject *out_c2,
                                       RealizationObject *out_c3);

/* Verifies compilation parity on M5 target G_S:
 * runs C1 on serialized G_S, checks byte parity with M5 and executes to 15 */
int omega_self_host_verify_m5_parity(const RealizationObject *c1, uint64_t *out_observed);

#endif /* OMEGA_SELF_HOST_H */
