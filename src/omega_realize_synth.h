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
    OmegaRealizeSchedule schedule;   /* machine-aware choice (never semantic) */
    uint32_t inputs_checked;         /* differential inputs where native == semantic */
    char why[192];                   /* reason when refused or not verified; empty on success */
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

/* Main machine-aware realization synthesis entrypoint (G_S x G_M -> G_R).
 * Compiles task->program's canonical body (spec/program-realization.md): V0 gate ->
 * schedule chosen from the MachineGraph -> AArch64 bytes -> triple id -> verification
 * (bindings, V0 structural, native-vs-semantic differential, V2). Returns 0 solved;
 * -2 refused by the V0 gate (result->why); -3 not verifiable on this host; -1 failed. */
int omega_synthesize_realization(const RealizationSynthesisTask *task,
                                 RealizationSynthesisResult *result);

/* Schedule the planner picks for a machine (issue_width >= 4 and >= 16 GPRs: PRELOAD). */
OmegaRealizeSchedule omega_realize_choose_schedule(const OmegaMachineGraph *mg);

/* Differential inputs for a program: boundary values of its width + seeded random. */
#define OMEGA_REALIZE_DIFF_INPUTS 84
size_t omega_realize_differential_inputs(const OmegaProgram *prog, uint64_t *xs, size_t max);

/* Verify a machine-bound realization against program + machine: program passes the V0
 * gate; real->semantic_id == program_id; real->machine_id == mg->machine_id (and mg's id
 * matches its content); profile; V0 structural incl. the triple REALIZATION_ID over the
 * code bytes; native execution == omega_program_eval on every differential input.
 * 0 pass; -1 fail (why); -3 not verifiable here (non-AArch64 host). */
int omega_realization_verify_program(const OmegaProgram *prog, const OmegaMachineGraph *mg,
                                     const RealizationObject *real, uint32_t *out_checked,
                                     char *why, size_t why_len);

/* Synthesize realization specialized for 4-wide DGX Spark Grace Neoverse V2 pipeline */
int omega_synthesize_for_dgx_spark(const OmegaProgram *prog, RealizationSynthesisResult *result);

/* Synthesize realization specialized for 2-wide QEMU virt AArch64 baseline pipeline */
int omega_synthesize_for_qemu_virt(const OmegaProgram *prog, RealizationSynthesisResult *result);

#endif /* OMEGA_REALIZE_SYNTH_H */
