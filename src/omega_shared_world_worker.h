/*
 * omega_shared_world_worker.h -- persistent GB10 qualification worker + the CPU
 * campaign harness for M20 Stage 1 (OMEGA_SHARED_WORLD).
 *
 * VERIFICATION STATUS (2026-09-26): IMPLEMENTED, NOT YET COMPILED, NOT SILICON-
 * OBSERVED. The GB10 GPU was contended and build/test execution was unavailable
 * during authoring (see evidence/omega_shared_world_stage1_status.md). The
 * worker's SASS scheduling/scoreboard control words (BlackwellIRInsn.control)
 * are marked for on-silicon calibration; do not treat this file as qualified
 * until the campaign in omega_sw_run_campaign() passes on real GB10.
 *
 * The resident worker is NOT AIEN. It is the smallest possible persistent GPU
 * program that proves the coherent communication substrate:
 *
 *   loop (until shutdown message or bounded give-up deadline):
 *     acquire-observe CPU_TO_GPU_RING.tail        (LDG.E.STRONG.SYS + CCTL.IVALL)
 *     if empty: continue
 *     read slot[head]                              (STRONG.SYS loads)
 *     validate magic/epoch/sequence/msg_type
 *     out = in XOR arg_a                           (trivial deterministic xform)
 *     write result into GPU_TO_CPU_RING.slot[tail]
 *     release-publish GPU_TO_CPU_RING.tail         (MEMBAR.ALL.SYS + STG.E.STRONG.SYS)
 *     release-advance CPU_TO_GPU_RING.head         (frees the request slot)
 *
 * It stays resident for the whole campaign; the CPU launches it once and then
 * communicates only through coherent memory. A bounded give-up (iteration/clock
 * deadline) guarantees the kernel always terminates so a hang cannot wedge the
 * GPU.
 */
#ifndef OMEGA_SHARED_WORLD_WORKER_H
#define OMEGA_SHARED_WORLD_WORKER_H

#include "omega_blackwell_codegen.h"
#include <stddef.h>
#include <stdint.h>

/* Worker code-generation parameters. All addressing inside the worker is
 * relative to the coherent region base, which is passed to the kernel as a
 * constant-bank argument (a GPU VA used by the trusted launcher only, never
 * placed in the shared ABI). */
typedef struct {
    uint32_t world_epoch;        /* epoch the worker validates against          */
    uint32_t xform;              /* OMEGA_SW_XFORM_* (Stage 1: XOR_CONST)        */
    uint64_t off_c2g;            /* region offset of CPU_TO_GPU_RING             */
    uint64_t off_g2c;            /* region offset of GPU_TO_CPU_RING             */
    uint64_t off_fault;          /* region offset of FAULT_MAILBOX               */
    uint32_t ring_capacity;      /* power-of-two                                 */
    uint64_t giveup_iterations;  /* bounded spin budget before self-exit         */
} OmegaSwWorkerParams;

/* Build the resident worker as a native Blackwell IR program (uses the M20
 * STRONG.SYS / MEMBAR ordering opcodes). Returns 0 on success. The caller then
 * runs register allocation + encoding via the existing codegen pipeline. */
int omega_sw_worker_build_ir(const OmegaSwWorkerParams *params,
                             BlackwellIRProgram *prog);

/* Campaign result. */
typedef struct {
    uint64_t requested;          /* messages the CPU intended to exchange        */
    uint64_t sent;               /* CPU_TO_GPU publishes accepted                */
    uint64_t received;           /* GPU_TO_CPU responses consumed & validated    */
    uint64_t duplicates;
    uint64_t missing;            /* reordered-as-valid / skipped                 */
    uint64_t torn;               /* payload mismatch vs expected transform       */
    uint64_t replays;            /* stale sequence accepted (must stay 0)        */
    uint64_t rejects;            /* hostile/rejected entries observed            */
    uint32_t final_fault_code;
    int      worker_launched;    /* 1 if the single launch succeeded            */
    int      silicon_observed;   /* 1 only if run on real GB10                   */
} OmegaSwCampaignResult;

/*
 * Run the CPU side of the Stage 1 campaign against a resident worker on GB10:
 * open a PHYSICS coherent context, allocate ONE coherent region, format it as a
 * shared world, register the payload object, launch the worker once, then
 * exchange `count` messages (variable delays / bursts driven by `stress`),
 * verifying every response against its originating sequence. Returns 0 on a
 * fully clean campaign. Requires an idle GB10 GPU.
 */
int omega_sw_run_campaign(uint64_t count, unsigned stress_mask,
                          OmegaSwCampaignResult *out);

/* Stress-mode bits for omega_sw_run_campaign(). */
#define OMEGA_SW_STRESS_VAR_PRODUCER_DELAY  0x1u
#define OMEGA_SW_STRESS_VAR_CONSUMER_DELAY  0x2u
#define OMEGA_SW_STRESS_BURST               0x4u
#define OMEGA_SW_STRESS_GENERATION_CHANGES  0x8u
#define OMEGA_SW_STRESS_HOSTILE_INJECTION   0x10u

#endif /* OMEGA_SHARED_WORLD_WORKER_H */
