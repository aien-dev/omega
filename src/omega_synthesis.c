#include "omega_synthesis.h"
#include "omega_canonical.h"
#include "omega_exec.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const uint64_t PROBE_INPUTS[SYNTH_PROBE_COUNT] = { 0, 1, 3, 7, 13, 42 };

static const OmegaSynthPrimDef BASE_PRIM_DEFS[] = {
    { "add_1",  OP_ADD, 1 },
    { "add_2",  OP_ADD, 2 },
    { "add_3",  OP_ADD, 3 },
    { "add_5",  OP_ADD, 5 },
    { "sub_1",  OP_SUB, 1 },
    { "sub_2",  OP_SUB, 2 },
    { "sub_3",  OP_SUB, 3 },
    { "sub_5",  OP_SUB, 5 },
    { "mul_2",  OP_MUL, 2 },
    { "mul_3",  OP_MUL, 3 },
    { "mul_4",  OP_MUL, 4 },
    { "and_ff", OP_AND, 0xFF },
    { "and_0f", OP_AND, 0x0F },
    { "or_1",   OP_OR,  1 },
    { "or_2",   OP_OR,  2 }
};

size_t omega_synth_base_prim_defs(const OmegaSynthPrimDef **out) {
    if (out) *out = BASE_PRIM_DEFS;
    return sizeof(BASE_PRIM_DEFS) / sizeof(BASE_PRIM_DEFS[0]);
}

int omega_synth_bank_init(SynthPrimitiveBank *bank) {
    if (!bank) return -1;
    memset(bank, 0, sizeof(SynthPrimitiveBank));

    const OmegaSynthPrimDef *prim_defs = NULL;
    size_t num_defs = omega_synth_base_prim_defs(&prim_defs);

    for (size_t i = 0; i < num_defs && i < SYNTH_MAX_PRIMITIVES; ++i) {
        if (omega_program_build_unary_op(&bank->programs[i], prim_defs[i].name,
                                         prim_defs[i].op, prim_defs[i].imm) == 0) {
            bank->count++;
        }
    }

    return 0;
}

void omega_synth_bank_destroy(SynthPrimitiveBank *bank) {
    if (!bank) return;
    for (size_t i = 0; i < bank->count; ++i) {
        omega_program_destroy(&bank->programs[i]);
    }
    bank->count = 0;
}

void omega_synth_equiv_init(EquivTable *tbl) {
    if (!tbl) return;
    memset(tbl, 0, sizeof(EquivTable));
}

bool omega_synth_equiv_contains_or_add(EquivTable *tbl, const uint8_t hash[32], uint32_t cost) {
    if (!tbl) return false;

    for (size_t i = 0; i < tbl->count; ++i) {
        if (memcmp(tbl->entries[i].hash, hash, 32) == 0) {
            if (tbl->entries[i].cost <= cost) {
                return true; /* Prune: already seen with equal or lower cost */
            } else {
                tbl->entries[i].cost = cost;
                return false; /* Keep: current candidate is strictly lower cost */
            }
        }
    }

    if (tbl->count < SYNTH_MAX_SIGNATURES) {
        memcpy(tbl->entries[tbl->count].hash, hash, 32);
        tbl->entries[tbl->count].cost = cost;
        tbl->count++;
    }
    return false;
}

int omega_synth_compute_signature(const OmegaProgram *prog, uint8_t out_hash[32]) {
    if (!prog || !out_hash || !prog->is_realized) return -1;

    uint64_t outputs[SYNTH_PROBE_COUNT];
    for (size_t i = 0; i < SYNTH_PROBE_COUNT; ++i) {
        uint64_t out_val = 0;
        if (omega_program_exec(prog, PROBE_INPUTS[i], &out_val) != 0) {
            return -1;
        }
        outputs[i] = out_val;
    }

    sha256_hash((const uint8_t*)outputs, sizeof(outputs), out_hash);
    return 0;
}

int omega_synthesize(const SynthesisTask *task, const SynthPrimitiveBank *bank,
                     const SynthesisConfig *config, SynthesisResult *result) {
    if (!task || !bank || !config || !result) return -1;
    memset(result, 0, sizeof(SynthesisResult));

    EquivTable equiv_tbl;
    omega_synth_equiv_init(&equiv_tbl);

    OmegaProgram depth1[SYNTH_MAX_PRIMITIVES];
    size_t depth1_count = 0;

    OmegaProgram depth2[SYNTH_MAX_CANDIDATES];
    size_t depth2_count = 0;

    /* -------------------------------------------------------------
     * DEPTH 1: Base primitives exploration
     * ------------------------------------------------------------- */
    for (size_t i = 0; i < bank->count; ++i) {
        const OmegaProgram *p = &bank->programs[i];
        result->stats.candidates_generated++;

        if (config->deduplicate_equiv) {
            uint8_t sig[32];
            if (omega_synth_compute_signature(p, sig) == 0) {
                if (omega_synth_equiv_contains_or_add(&equiv_tbl, sig, p->cost.insn_count)) {
                    result->stats.candidates_pruned_equiv++;
                    continue;
                }
            }
        }

        bool solved = false;
        if (omega_task_evaluate_candidate(task, p, &solved) == 0 && solved) {
            VerifyReport rep;
            if (omega_program_verify((OmegaProgram*)p, &rep) == 0) {
                result->solved = true;
                result->solution = *p;
                result->verify_report = rep;
                result->stats.solutions_found++;
                return 0;
            }
        } else {
            result->stats.candidates_failed_v1++;
        }

        if (depth1_count < SYNTH_MAX_PRIMITIVES) {
            depth1[depth1_count++] = *p;
        }
    }

    if (config->max_depth < 2) {
        return 0;
    }

    /* -------------------------------------------------------------
     * DEPTH 2: Binary compositions C = A o B
     * ------------------------------------------------------------- */
    for (size_t i = 0; i < depth1_count; ++i) {
        for (size_t j = 0; j < bank->count; ++j) {
            if (result->stats.candidates_generated >= config->max_candidates) {
                goto cleanup;
            }
            result->stats.candidates_generated++;

            OmegaProgram cand;
            char err[256];
            if (omega_program_compose(&depth1[i], &bank->programs[j], &cand, err, sizeof(err)) != 0) {
                result->stats.candidates_pruned_type++;
                continue;
            }

            if (cand.cost.insn_count > task->cost_budget.insn_count ||
                cand.cost.insn_count > config->max_cost) {
                omega_program_destroy(&cand);
                continue;
            }

            if (config->deduplicate_equiv) {
                uint8_t sig[32];
                if (omega_synth_compute_signature(&cand, sig) == 0) {
                    if (omega_synth_equiv_contains_or_add(&equiv_tbl, sig, cand.cost.insn_count)) {
                        result->stats.candidates_pruned_equiv++;
                        omega_program_destroy(&cand);
                        continue;
                    }
                }
            }

            bool solved = false;
            if (omega_task_evaluate_candidate(task, &cand, &solved) == 0 && solved) {
                VerifyReport rep;
                if (omega_program_verify(&cand, &rep) == 0) {
                    result->solved = true;
                    result->solution = cand;
                    result->verify_report = rep;
                    result->stats.solutions_found++;
                    goto cleanup;
                }
            } else {
                result->stats.candidates_failed_v1++;
            }

            if (depth2_count < SYNTH_MAX_CANDIDATES) {
                depth2[depth2_count++] = cand;
            } else {
                omega_program_destroy(&cand);
            }
        }
    }

    if (config->max_depth < 3 || result->solved) {
        goto cleanup;
    }

    /* -------------------------------------------------------------
     * DEPTH 3: Ternary compositions C = (A o B) o C
     * ------------------------------------------------------------- */
    for (size_t i = 0; i < depth2_count; ++i) {
        for (size_t j = 0; j < bank->count; ++j) {
            if (result->stats.candidates_generated >= config->max_candidates) {
                goto cleanup;
            }
            result->stats.candidates_generated++;

            OmegaProgram cand;
            char err[256];
            if (omega_program_compose(&depth2[i], &bank->programs[j], &cand, err, sizeof(err)) != 0) {
                result->stats.candidates_pruned_type++;
                continue;
            }

            if (cand.cost.insn_count > task->cost_budget.insn_count ||
                cand.cost.insn_count > config->max_cost) {
                omega_program_destroy(&cand);
                continue;
            }

            if (config->deduplicate_equiv) {
                uint8_t sig[32];
                if (omega_synth_compute_signature(&cand, sig) == 0) {
                    if (omega_synth_equiv_contains_or_add(&equiv_tbl, sig, cand.cost.insn_count)) {
                        result->stats.candidates_pruned_equiv++;
                        omega_program_destroy(&cand);
                        continue;
                    }
                }
            }

            bool solved = false;
            if (omega_task_evaluate_candidate(task, &cand, &solved) == 0 && solved) {
                VerifyReport rep;
                if (omega_program_verify(&cand, &rep) == 0) {
                    result->solved = true;
                    result->solution = cand;
                    result->verify_report = rep;
                    result->stats.solutions_found++;
                    goto cleanup;
                }
            } else {
                result->stats.candidates_failed_v1++;
            }

            omega_program_destroy(&cand);
        }
    }

cleanup:
    /* Clean up depth2 candidates that weren't selected as solution */
    for (size_t i = 0; i < depth2_count; ++i) {
        if (!result->solved ||
            memcmp(depth2[i].program_id.bytes, result->solution.program_id.bytes, OMEGA_ID_BYTES) != 0) {
            omega_program_destroy(&depth2[i]);
        }
    }

    return 0;
}

int omega_synthesize_target_affine(SynthesisResult *result) {
    if (!result) return -1;

    SynthesisTask task;
    static const uint64_t inputs[] = { 0, 1, 2, 3, 5, 10 };
    static const uint64_t outputs[] = { 1, 3, 5, 7, 11, 21 };
    size_t count = sizeof(inputs) / sizeof(inputs[0]);

    if (omega_task_init(&task, "task_affine_2x_plus_1",
                        TYPE_UNSIGNED_INT, 64, TYPE_UNSIGNED_INT, 64,
                        inputs, outputs, count) != 0) {
        return -1;
    }

    SynthPrimitiveBank bank;
    if (omega_synth_bank_init(&bank) != 0) return -1;

    SynthesisConfig config = {
        .max_depth = 2,
        .max_cost = 10,
        .max_candidates = 500,
        .deduplicate_equiv = true
    };

    int rc = omega_synthesize(&task, &bank, &config, result);
    omega_synth_bank_destroy(&bank);
    return rc;
}

int omega_synthesize_target_composed(SynthesisResult *result) {
    if (!result) return -1;

    SynthesisTask task;
    static const uint64_t inputs[] = { 1, 2, 3, 4, 10 };
    static const uint64_t outputs[] = { 1, 4, 7, 10, 28 };
    size_t count = sizeof(inputs) / sizeof(inputs[0]);

    if (omega_task_init(&task, "task_composed_3x_minus_2",
                        TYPE_UNSIGNED_INT, 64, TYPE_UNSIGNED_INT, 64,
                        inputs, outputs, count) != 0) {
        return -1;
    }

    SynthPrimitiveBank bank;
    if (omega_synth_bank_init(&bank) != 0) return -1;

    SynthesisConfig config = {
        .max_depth = 2,
        .max_cost = 10,
        .max_candidates = 500,
        .deduplicate_equiv = true
    };

    int rc = omega_synthesize(&task, &bank, &config, result);
    omega_synth_bank_destroy(&bank);
    return rc;
}
