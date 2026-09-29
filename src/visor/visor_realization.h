/*
 * visor_realization.h -- Omega Visor V1, lane 5: realization lab.
 *
 * Wraps the EXISTING realizers (omega_realize_pure_binary, omega_program_realize
 * compiling the program body, omega_synthesize_realization) and the machine
 * estimate. Costs live in four separate slots and are never merged:
 * predicted (static), estimated (machine model), measured and qualified
 * (both ALWAYS absent in V1: no receipts are ingested).
 * The only execution path is visor_realization_run_pure.
 */
#ifndef OMEGA_VISOR_REALIZATION_H
#define OMEGA_VISOR_REALIZATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "visor.h"
#include "omega_machine.h"
#include "omega_program.h"

typedef enum {
    VISOR_COST_PREDICTED = 0,   /* static model: instruction count, 1 cycle each */
    VISOR_COST_ESTIMATED = 1,   /* omega_machine_estimate_latency on a machine model */
    VISOR_COST_MEASURED = 2,    /* from a receipt: NOT available in V1 */
    VISOR_COST_QUALIFIED = 3    /* from a qualified gate: NOT available in V1 */
} VisorCostClass;

typedef struct {
    bool present;
    VisorCostClass cls;
    const char *cls_name;
    uint64_t cycles;
    uint32_t insn_count;
    uint32_t code_bytes;
    char source[128];
} VisorCostItem;

typedef struct {
    VisorCostItem predicted, estimated, measured, qualified;
} VisorCostView;

#define VISOR_DISASM_MAX 64

typedef struct {
    char label[48];              /* alternatives: "direct@dgx-spark", ...; else "" */
    char realization_id[72];
    char subject_id[72];
    char realized_id[72];        /* real.semantic_id */
    char machine_id[72];         /* machine model used for estimates (entry.machine_id) */
    char bound_machine_id[72];   /* real.machine_id if the realizer bound one, else "none" */
    char machine_name[64];
    char target[32];
    uint8_t profile;
    uint32_t code_len;
    uint32_t insn_count;
    char disasm[VISOR_DISASM_MAX][48];
    size_t disasm_count;
    bool compatible;
    bool runnable;               /* run_pure would accept this entry */
    char why[256];
    VisorCostView cost;
} VisorRealizationView;

/* Return codes: 0 ok, -1 bad args / internal, -2 unsupported (why set), -3 refused to run. */
int visor_realize_apply(const OmegaGraph *g, const SemanticId *apply_id, const OmegaMachineGraph *mg,
                        VisorRealizationEntry *out_entry, VisorRealizationView *out_view);
int visor_realize_program(const OmegaProgram *p, const OmegaMachineGraph *mg,
                          VisorRealizationEntry *out_entry, VisorRealizationView *out_view);
int visor_realization_view(const VisorRealizationEntry *e, const OmegaMachineGraph *mg,
                           VisorRealizationView *out);
int visor_realization_cost(const VisorRealizationEntry *e, const OmegaMachineGraph *mg,
                           VisorCostView *out);
/* Deterministic order: [0] direct@<mg>, [1] direct@<other canonical profile>,
 * [2] synth@<mg>, [3] synth@<other>. direct = omega_program_realize (canonical);
 * synth = omega_synthesize_realization for that machine (verified against the
 * semantic evaluator), additionally differentially checked
 * against the direct realization; mismatch => compatible=false, runnable=false. */
int visor_realization_alternatives(const OmegaProgram *p, const OmegaMachineGraph *mg,
                                   VisorRealizationView *out, size_t max, size_t *count);
/* Same, also returning the entries (entries may be NULL). */
int visor_realization_alternatives_ex(const OmegaProgram *p, const OmegaMachineGraph *mg,
                                      VisorRealizationEntry *entries, VisorRealizationView *out,
                                      size_t max, size_t *count);
int visor_realization_compare(const VisorRealizationView *a, const VisorRealizationView *b,
                              char *out, size_t n);
int visor_realization_why(const VisorRealizationView *v, char *out, size_t n);
int visor_realization_format_text(const VisorRealizationView *v, char *out, size_t n);
int visor_realization_format_json(const VisorRealizationView *v, char *out, size_t n);
int visor_cost_format_text(const VisorCostView *c, char *out, size_t n);
int visor_cost_format_json(const VisorCostView *c, char *out, size_t n);

/* Execute a pure AArch64 realization natively: f(a,b,c), missing args = 0.
 * Refuses (-3): Blackwell / unknown targets, non-V8A profile, code failing
 * aarch64_validate_code_buffer, any instruction other than
 * ADD/SUB/MUL/AND/ORR/EOR/MOVZ/MOVK/RET, no trailing RET, argc > 3,
 * or a non-aarch64 host. */
int visor_realization_run_pure(const VisorRealizationEntry *e, const uint64_t *args, size_t argc,
                               uint64_t *out);

/* Disassemble with aarch64_decoder (deterministic). Returns line count. */
size_t visor_disasm(const uint8_t *code, size_t len, char lines[][48], size_t max);

#endif /* OMEGA_VISOR_REALIZATION_H */
