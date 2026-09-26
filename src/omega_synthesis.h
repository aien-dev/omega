#ifndef OMEGA_SYNTHESIS_H
#define OMEGA_SYNTHESIS_H

#include "omega_program.h"
#include <stdbool.h>
#include <stddef.h>

#define SYNTH_MAX_PRIMITIVES 32
#define SYNTH_MAX_CANDIDATES 512
#define SYNTH_MAX_SIGNATURES 1024
#define SYNTH_PROBE_COUNT 6

typedef struct {
    size_t candidates_generated;
    size_t candidates_pruned_type;
    size_t candidates_pruned_equiv;
    size_t candidates_failed_v1;
    size_t solutions_found;
} SynthesisStats;

typedef struct {
    uint32_t max_depth;         /* Max composition depth (1, 2, or 3) */
    uint32_t max_cost;          /* Maximum instruction count budget */
    size_t max_candidates;      /* Global candidate cutoff */
    bool deduplicate_equiv;     /* Enable observational equivalence pruning */
} SynthesisConfig;

typedef struct {
    bool solved;
    OmegaProgram solution;
    VerifyReport verify_report;
    SynthesisStats stats;
} SynthesisResult;

typedef struct {
    OmegaProgram programs[SYNTH_MAX_PRIMITIVES];
    size_t count;
} SynthPrimitiveBank;

typedef struct {
    uint8_t hash[32];
    uint32_t cost;
} EquivSignature;

typedef struct {
    EquivSignature entries[SYNTH_MAX_SIGNATURES];
    size_t count;
} EquivTable;

/* Initialize default primitive bank (add, sub, mul, and, or with diverse imm) */
int omega_synth_bank_init(SynthPrimitiveBank *bank);

/* Destroy primitive bank */
void omega_synth_bank_destroy(SynthPrimitiveBank *bank);

/* Equivalence table functions */
void omega_synth_equiv_init(EquivTable *tbl);
bool omega_synth_equiv_contains_or_add(EquivTable *tbl, const uint8_t hash[32], uint32_t cost);
int omega_synth_compute_signature(const OmegaProgram *prog, uint8_t out_hash[32]);

/* Main deterministic synthesis entrypoint */
int omega_synthesize(const SynthesisTask *task, const SynthPrimitiveBank *bank,
                     const SynthesisConfig *config, SynthesisResult *result);

/* High-level synthesis routines for canonical milestones / demonstrations */
int omega_synthesize_target_affine(SynthesisResult *result);
int omega_synthesize_target_composed(SynthesisResult *result);

#endif /* OMEGA_SYNTHESIS_H */
