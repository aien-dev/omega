#ifndef OMEGA_REALIZE_SYNTH_H
#define OMEGA_REALIZE_SYNTH_H

#include "omega_program.h"
#include "omega_machine.h"
#include "omega_realize.h"
#include "omega_verify.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const OmegaProgram *program;
    const OmegaMachineGraph *machine;
    bool optimize_latency;
    uint32_t max_unroll_factor;
} RealizationSynthesisTask;

typedef struct {
    bool solved;
    RealizationObject realization;
    SemanticId realization_id;
    uint32_t estimated_cycles;
    uint32_t code_bytes_len;
    VerifyReport verify_report;
} RealizationSynthesisResult;

/* Initialize realization synthesis task */
void omega_realize_task_init(RealizationSynthesisTask *task,
                             const OmegaProgram *prog,
                             const OmegaMachineGraph *mg);

/* Compute cryptographic triple identity:
 * REALIZATION_ID = SHA-256(OMG0 | KIND_REALIZATION | profile | SEMANTIC_ID | MACHINE_ID | code_bytes)
 */
int omega_realize_compute_triple_id(const SemanticId *semantic_id,
                                    const SemanticId *machine_id,
                                    const RealizationObject *real,
                                    SemanticId *out_id);

/* Main machine-aware realization synthesis entrypoint (G_S x G_M -> G_R) */
int omega_synthesize_realization(const RealizationSynthesisTask *task,
                                 RealizationSynthesisResult *result);

/* Synthesize realization specialized for 4-wide DGX Spark Grace Neoverse V2 pipeline */
int omega_synthesize_for_dgx_spark(const OmegaProgram *prog, RealizationSynthesisResult *result);

/* Synthesize realization specialized for 2-wide QEMU virt AArch64 baseline pipeline */
int omega_synthesize_for_qemu_virt(const OmegaProgram *prog, RealizationSynthesisResult *result);

#endif /* OMEGA_REALIZE_SYNTH_H */
