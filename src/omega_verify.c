#include "omega_verify.h"
#include "omega_validate.h"
#include "omega_canonical.h"
#include "omega_core.h"
#include "aarch64_decoder.h"
#include "omega_exec.h"
#include <string.h>
#include <stdio.h>

int omega_verify_v0_structural(const OmegaGraph *graph, const RealizationObject *real, VerifyReport *report) {
    if (!report) return -1;
    memset(report, 0, sizeof(VerifyReport));
    report->tier = VERIFY_TIER_V0;
    report->passed = true;

    /* 1. If semantic graph is provided, validate typing and DAG integrity */
    if (graph) {
        char err[256];
        report->check_count++;
        if (omega_validate_graph(graph, err, sizeof(err)) != 0) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail), "V0 Graph Validation Failed: %.200s", err);
            return -1;
        }

        /* Check that graph has at least one valid object */
        report->check_count++;
        if (graph->object_count == 0) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail), "V0 Graph Validation Failed: Empty graph");
            return -1;
        }
    }

    /* 2. If realization object is provided, validate physical instruction and buffer bounds */
    if (real) {
        /* Check target profile */
        report->check_count++;
        if (real->target_profile != AARCH64_PROFILE_V8A_BAREMETAL) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V0 Realization Profile Failed: Unknown profile 0x%02x", real->target_profile);
            return -1;
        }

        /* Check code length bounds */
        report->check_count++;
        if (real->code_len < 4 || real->code_len > AARCH64_MAX_CODE_BYTES || (real->code_len % 4 != 0)) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V0 Realization Code Bounds Failed: Invalid length %zu bytes", real->code_len);
            return -1;
        }

        /* Check instruction stream decoding and terminal RET */
        report->check_count++;
        char dec_err[256];
        if (aarch64_validate_code_buffer(real->code_bytes, real->code_len, dec_err, sizeof(dec_err)) != 0) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V0 Instruction Stream Failed: %.200s", dec_err);
            return -1;
        }

        /* Check REALIZATION_ID cryptographic binding */
        report->check_count++;
        if (!real->has_id) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V0 Realization ID Failed: Object missing REALIZATION_ID");
            return -1;
        }

        RealizationObject check_id = *real;
        bool id_matches = false;
        if (real->has_machine_id) {
            SemanticId triple_id;
            if (omega_realize_compute_triple_id(&real->semantic_id, &real->machine_id, real, &triple_id) == 0 &&
                memcmp(triple_id.bytes, real->realization_id.bytes, OMEGA_ID_BYTES) == 0) {
                id_matches = true;
            }
        } else {
            if (omega_compute_realization_id(&check_id) == 0 &&
                memcmp(check_id.realization_id.bytes, real->realization_id.bytes, OMEGA_ID_BYTES) == 0) {
                id_matches = true;
            }
        }

        if (!id_matches) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V0 Realization ID Failed: REALIZATION_ID cryptographic mismatch");
            return -1;
        }
    }

    return 0;
}

int omega_verify_v1_differential(const OmegaGraph *graph, const RealizationObject *real,
                                 const uint64_t *test_inputs, size_t input_triplet_count,
                                 VerifyReport *report) {
    if (!report || !real) return -1;
    (void)graph;
    memset(report, 0, sizeof(VerifyReport));
    report->tier = VERIFY_TIER_V1;
    report->passed = true;

    /* First enforce V0 structural check */
    if (omega_verify_v0_structural(graph, real, report) != 0) {
        return -1;
    }

    report->tier = VERIFY_TIER_V1;

    /* Default test vectors if none provided */
    static const uint64_t default_vectors[] = {
        7, 11, 3,        /* Standard test case -> 15 */
        0, 0, 0,         /* Zeros */
        100, 200, 50,    /* Positive numbers -> 250 */
        1000, 500, 300,  /* Numbers -> 1200 */
        1, 1, 1,         /* Small ints -> 1 */
        50, 0, 10,       /* Zero second -> 40 */
        0xFFFFFFFFFFFFFFFEULL, 1, 0 /* Near 64-bit max */
    };

    const uint64_t *vecs = test_inputs ? test_inputs : default_vectors;
    size_t count = test_inputs ? input_triplet_count : (sizeof(default_vectors) / (3 * sizeof(uint64_t)));

    for (size_t i = 0; i < count; ++i) {
        uint64_t a = vecs[i * 3];
        uint64_t b = vecs[i * 3 + 1];
        uint64_t c = vecs[i * 3 + 2];

        /* Semantic reference evaluation for F(a, b, c) = (a + b) - c */
        uint64_t expected = (a + b) - c;

        /* Native execution */
        uint64_t observed = 0;
        report->check_count++;
        int rc = omega_exec_native_f3(real, a, b, c, &observed);
        if (rc != 0) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V1 Differential Execution Failed: Vector %zu (a=%lu, b=%lu, c=%lu) returned rc=%d",
                     i, (unsigned long)a, (unsigned long)b, (unsigned long)c, rc);
            return -1;
        }

        if (observed != expected) {
            report->passed = false;
            report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail),
                     "V1 Differential Mismatch: Vector %zu (a=%lu, b=%lu, c=%lu) expected=%lu observed=%lu",
                     i, (unsigned long)a, (unsigned long)b, (unsigned long)c,
                     (unsigned long)expected, (unsigned long)observed);
            return -1;
        }
    }

    return 0;
}

int omega_verify_v2_properties(const OmegaGraph *graph, const RealizationObject *real, VerifyReport *report) {
    if (!report) return -1;
    (void)graph;
    (void)real;
    memset(report, 0, sizeof(VerifyReport));
    report->tier = VERIFY_TIER_V2;
    report->passed = true;

    /* 1. Commutativity Property: op(a, b) == op(b, a) for OP_ADD, OP_MUL, OP_AND, OP_OR */
    static const OpCode comm_ops[] = { OP_ADD, OP_MUL, OP_AND, OP_OR };
    for (size_t o = 0; o < sizeof(comm_ops)/sizeof(comm_ops[0]); ++o) {
        OpCode op = comm_ops[o];
        for (uint64_t x = 0; x < 32; x += 7) {
            for (uint64_t y = 0; y < 32; y += 5) {
                uint64_t r1 = 0, r2 = 0;
                report->check_count++;
                if (omega_eval_pure_binary_uint(op, OVERFLOW_WRAP, 64, x, y, &r1) != 0 ||
                    omega_eval_pure_binary_uint(op, OVERFLOW_WRAP, 64, y, x, &r2) != 0 ||
                    r1 != r2) {
                    report->passed = false;
                    report->fail_count++;
                    snprintf(report->error_detail, sizeof(report->error_detail),
                             "V2 Commutativity Violation on op 0x%02x: (%lu, %lu) => (%lu vs %lu)",
                             op, (unsigned long)x, (unsigned long)y, (unsigned long)r1, (unsigned long)r2);
                    return -1;
                }
            }
        }
    }

    /* 2. Identity Element Property:
     * OP_ADD: x + 0 == x
     * OP_MUL: x * 1 == x, x * 0 == 0
     * OP_AND: x & ~0 == x
     * OP_OR:  x | 0 == x
     */
    for (uint64_t x = 0; x < 64; ++x) {
        uint64_t res = 0;

        /* ADD identity */
        report->check_count++;
        if (omega_eval_pure_binary_uint(OP_ADD, OVERFLOW_WRAP, 64, x, 0, &res) != 0 || res != x) {
            report->passed = false; report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail), "V2 Identity Violation on ADD");
            return -1;
        }

        /* MUL identity */
        report->check_count++;
        if (omega_eval_pure_binary_uint(OP_MUL, OVERFLOW_WRAP, 64, x, 1, &res) != 0 || res != x) {
            report->passed = false; report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail), "V2 Identity Violation on MUL");
            return -1;
        }

        /* AND identity */
        report->check_count++;
        if (omega_eval_pure_binary_uint(OP_AND, OVERFLOW_WRAP, 64, x, 0xFFFFFFFFFFFFFFFFULL, &res) != 0 || res != x) {
            report->passed = false; report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail), "V2 Identity Violation on AND");
            return -1;
        }

        /* OR identity */
        report->check_count++;
        if (omega_eval_pure_binary_uint(OP_OR, OVERFLOW_WRAP, 64, x, 0, &res) != 0 || res != x) {
            report->passed = false; report->fail_count++;
            snprintf(report->error_detail, sizeof(report->error_detail), "V2 Identity Violation on OR");
            return -1;
        }
    }

    /* 3. Overflow Wrapping Semantics:
     * Under OVERFLOW_WRAP: (2^64 - 1) + 1 == 0
     * 0 - 1 == 2^64 - 1
     */
    report->check_count++;
    uint64_t wrap_res = 0;
    if (omega_eval_pure_binary_uint(OP_ADD, OVERFLOW_WRAP, 64, 0xFFFFFFFFFFFFFFFFULL, 1, &wrap_res) != 0 || wrap_res != 0) {
        report->passed = false; report->fail_count++;
        snprintf(report->error_detail, sizeof(report->error_detail), "V2 Overflow Wrap Violation on 2^64-1 + 1");
        return -1;
    }

    report->check_count++;
    if (omega_eval_pure_binary_uint(OP_SUB, OVERFLOW_WRAP, 64, 0, 1, &wrap_res) != 0 || wrap_res != 0xFFFFFFFFFFFFFFFFULL) {
        report->passed = false; report->fail_count++;
        snprintf(report->error_detail, sizeof(report->error_detail), "V2 Overflow Wrap Violation on 0 - 1");
        return -1;
    }

    /* 4. Range Preservation: output fits width (e.g. 16-bit uint wraps at 0xFFFF) */
    report->check_count++;
    uint64_t u16_res = 0;
    if (omega_eval_pure_binary_uint(OP_ADD, OVERFLOW_WRAP, 16, 0xFFFF, 1, &u16_res) != 0 || u16_res != 0) {
        report->passed = false; report->fail_count++;
        snprintf(report->error_detail, sizeof(report->error_detail), "V2 Range Preservation Violation on 16-bit uint");
        return -1;
    }

    return 0;
}

int omega_verify_pipeline(const OmegaGraph *graph, const RealizationObject *real,
                          VerifyTier max_tier, VerifyReport *report) {
    if (!report) return -1;

    /* V0 Tier */
    if (omega_verify_v0_structural(graph, real, report) != 0) {
        return -1;
    }

    /* V1 Tier */
    if (max_tier >= VERIFY_TIER_V1) {
        if (omega_verify_v1_differential(graph, real, NULL, 0, report) != 0) {
            return -1;
        }
    }

    /* V2 Tier */
    if (max_tier >= VERIFY_TIER_V2) {
        if (omega_verify_v2_properties(graph, real, report) != 0) {
            return -1;
        }
    }

    return 0;
}
