/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - SYNTHESIS AND COMPARISON (ADDITIVE)
 * Ternary primitive bank, lowering-aware enumerative synthesis, and the
 * same-task comparison against the canonical binary synthesizer.
 * ========================================================================= */

#ifndef OMEGA_TERNARY_SYNTH_H
#define OMEGA_TERNARY_SYNTH_H

#include "omega_ternary_verify.h"
#include "omega_program.h"
#include "omega_synthesis.h"

#define TSYN_MAX_BANK 32
#define TSYN_MAX_EXAMPLES 16
#define TSYN_PROBES 8
#define TSYN_MAX_TASKS 24
#define TSYN_DOMAIN_BOUND 265720 /* (3^12 - 1) / 2: twelve-trit inputs */

typedef struct {
    char name[24];
    TStep step;
} TPrim;

typedef struct {
    TPrim prims[TSYN_MAX_BANK];
    size_t count;
} TBank;

/* One task, stated as examples over signed integers; both paths get the
 * identical examples (the binary path sees them as two's-complement u64). */
typedef struct {
    char name[32];
    char formula[48];
    int64_t (*fn)(int64_t);
    int64_t inputs[TSYN_MAX_EXAMPLES];
    int64_t outputs[TSYN_MAX_EXAMPLES];
    size_t n;
    int64_t domain_bound;
} TTask;

typedef struct {
    size_t candidates;
    size_t pruned_equiv;
    size_t solutions;
} TSynthStats;

typedef struct {
    bool solved;
    TProgram prog;
    double predicted_cost; /* mean predicted dynamic insns over the task examples */
    TSynthStats stats;
} TSynthResult;

void omega_t_bank_init(TBank *bank);
size_t omega_t_tasks_init(TTask *tasks, size_t max);

/* Lowering-aware cost: interval analysis for flags, then the realization
 * assignment minimizing the mean predicted dynamic instruction count over
 * the examples. Fills reps and flags. */
double omega_t_best_realization(const TChain *chain, const int64_t *xs, size_t n, int64_t bound,
                                TernaryRep *reps, TStepFlags *flags);

int omega_t_synthesize(const TTask *task, const TBank *bank, uint32_t max_depth, TSynthResult *res);

/* Binary control banks: the canonical bank (omega_synth_bank_init,
 * untouched) and the canonical bank plus neg, lsl_1, asr_1. */
int omega_t_binary_bank_augmented(SynthPrimitiveBank *bank);

/* One row of the comparison. */
typedef struct {
    bool solved;
    bool verified;
    uint32_t steps;
    uint32_t static_insns;
    double predicted_dyn;
    double measured_dyn;
    double ns_per_call;
    char program[96];
    char realization[16];
} TPathResult;

typedef struct {
    char task[32];
    char formula[48];
    TPathResult ternary;
    TPathResult binary;
    TPathResult binary_aug;
} TCompareRow;

/* Runs every task through the ternary synthesizer and both binary banks,
 * realizes, verifies, and measures each solution on the Spark. */
size_t omega_t_run_comparison(TCompareRow *rows, size_t max_rows, bool measure_time);

/* AEGIS parity: single-bit mutants of every verified solution, judged by
 * each path's verifier and by exhaustive domain truth in a sandboxed child. */
typedef struct {
    size_t mutants;
    size_t accepted;
    size_t rejected;
    size_t false_accepts;   /* accepted but not equivalent on the domain */
    size_t false_rejects;   /* rejected but equivalent on the domain */
    size_t crashed;
} TParity;

int omega_t_run_parity(TParity *ternary, TParity *binary, size_t flips_per_insn);

#endif /* OMEGA_TERNARY_SYNTH_H */
