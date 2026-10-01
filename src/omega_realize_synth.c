#include "omega_realize_synth.h"
#include "omega_core.h"
#include "sha256.h"
#include "aarch64_target.h"
#include "aarch64_encoder.h"
#include "aarch64_decoder.h"
#include <string.h>
#include <stdio.h>
#include "omega_exec.h"
#include "omega_canonical.h"
#include "searchtrace/st_hook.h"

void omega_realize_task_init(RealizationSynthesisTask *task,
                             const OmegaProgram *prog,
                             const OmegaMachineGraph *mg) {
    if (!task) return;
    memset(task, 0, sizeof(*task));
    task->program = prog;
    task->machine = mg;
    task->optimize_latency = true;
    task->max_unroll_factor = 1;
}

int omega_realize_compute_triple_id(const SemanticId *semantic_id,
                                    const SemanticId *machine_id,
                                    const RealizationObject *real,
                                    SemanticId *out_id) {
    if (!semantic_id || !machine_id || !real || !out_id) return -1;

    uint8_t buffer[4096];
    size_t pos = 0;

    /* Canonical OMG0 wire header */
    buffer[pos++] = 'O';
    buffer[pos++] = 'M';
    buffer[pos++] = 'G';
    buffer[pos++] = '0';
    buffer[pos++] = KIND_REALIZATION; /* 0x08 */
    buffer[pos++] = real->target_profile;

    /* Entry offset (4 bytes big-endian) */
    buffer[pos++] = (uint8_t)((real->entry_offset >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((real->entry_offset >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((real->entry_offset >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(real->entry_offset & 0xFF);

    /* Code length (4 bytes big-endian) */
    uint32_t clen = (uint32_t)real->code_len;
    buffer[pos++] = (uint8_t)((clen >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((clen >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((clen >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(clen & 0xFF);

    /* Semantic ID (32 bytes) from G_S */
    memcpy(&buffer[pos], semantic_id->bytes, OMEGA_ID_BYTES);
    pos += OMEGA_ID_BYTES;

    /* Machine ID (32 bytes) from G_M */
    memcpy(&buffer[pos], machine_id->bytes, OMEGA_ID_BYTES);
    pos += OMEGA_ID_BYTES;

    /* Raw Machine Code Bytes */
    if (pos + real->code_len <= sizeof(buffer)) {
        memcpy(&buffer[pos], real->code_bytes, real->code_len);
        pos += real->code_len;
    }

    sha256_hash(buffer, pos, out_id->bytes);
    return 0;
}

/* ---- verification of a machine-bound realization against its program ------ */

/* Deterministic xorshift64* seeded from the program id: same program, same inputs. */
static uint64_t rng_next(uint64_t *s) {
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

size_t omega_realize_differential_inputs(const OmegaProgram *prog, uint64_t *xs, size_t max) {
    if (!prog || !xs || max < OMEGA_REALIZE_DIFF_INPUTS) return 0;
    uint16_t w = prog->contract.input_width;
    uint64_t mask = (w >= 64 || w == 0) ? ~0ull : ((1ull << w) - 1ull);
    uint64_t top = (w >= 64 || w == 0) ? (1ull << 63) : (1ull << (w - 1));
    const uint64_t fixed[] = { 0, 1, 2, 3, 5, 7, 10, 100, mask, mask - 1, mask - 2, top, top - 1, top + 1,
                               0x5555555555555555ull & mask, 0xAAAAAAAAAAAAAAAAull & mask,
                               0xFFFFull & mask, 0x10000ull & mask, 0xFFFFFFFFull & mask, 0x100000000ull & mask };
    size_t n = 0;
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; ++i) xs[n++] = fixed[i] & mask;
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < 8; ++i) s ^= (uint64_t)prog->program_id.bytes[i] << (8 * i);
    if (!s) s = 1;
    while (n < OMEGA_REALIZE_DIFF_INPUTS) xs[n++] = rng_next(&s) & mask;
    return n;
}

int omega_realization_verify_program(const OmegaProgram *prog, const OmegaMachineGraph *mg,
                                     const RealizationObject *real, uint32_t *out_checked,
                                     char *why, size_t why_len) {
    char tmp[256];
    if (out_checked) *out_checked = 0;
    if (!prog || !mg || !real) { if (why && why_len) snprintf(why, why_len, "bad arguments"); return -1; }
    /* 1. semantic side: the program itself passes the V0 gate (id recomputed from the body) */
    if (omega_program_realize_check(prog, NULL, tmp, sizeof tmp) != 0) {
        if (why && why_len) snprintf(why, why_len, "program refused: %s", tmp);
        return -1;
    }
    if (omega_compare_semantic_id(&real->semantic_id, &prog->program_id) != 0) {
        if (why && why_len) snprintf(why, why_len, "SEMANTIC_ID of realization is not this program's id");
        return -1;
    }
    /* 2. machine side: machine graph id is its own canonical id; realization bound to it */
    OmegaMachineGraph mcopy = *mg;
    if (omega_machine_compute_id(&mcopy) != 0 || omega_compare_semantic_id(&mcopy.machine_id, &mg->machine_id) != 0) {
        if (why && why_len) snprintf(why, why_len, "machine graph id does not match its content");
        return -1;
    }
    if (!real->has_machine_id || omega_compare_semantic_id(&real->machine_id, &mg->machine_id) != 0) {
        if (why && why_len) snprintf(why, why_len, "MACHINE_ID of realization is not this machine's id");
        return -1;
    }
    if (real->target_profile != mg->target_profile) {
        if (why && why_len) snprintf(why, why_len, "realization profile differs from machine profile");
        return -1;
    }
    /* 3. triple binding + structure (V0 recomputes the triple id over the code bytes) */
    VerifyReport rep;
    memset(&rep, 0, sizeof rep);
    if (omega_verify_v0_structural(NULL, real, &rep) != 0 || !rep.passed) {
        if (why && why_len) snprintf(why, why_len, "%.200s", rep.error_detail);
        return -1;
    }
    /* 4. differential: native execution of the bytes == independent semantic evaluator */
    uint64_t xs[OMEGA_REALIZE_DIFF_INPUTS], ys[OMEGA_REALIZE_DIFF_INPUTS];
    size_t n = omega_realize_differential_inputs(prog, xs, OMEGA_REALIZE_DIFF_INPUTS);
    if (n == 0 || omega_program_eval(prog, xs, n, ys) != 0) {
        if (why && why_len) snprintf(why, why_len, "semantic evaluator refused the program");
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        uint64_t got = 0;
        int rc = omega_exec_native_f3(real, xs[i], 0, 0, &got);
        if (rc == -2) {
            if (why && why_len) snprintf(why, why_len, "not verifiable here: native differential needs an AArch64 host");
            return -3;
        }
        if (rc != 0) {
            if (why && why_len) snprintf(why, why_len, "native execution failed");
            return -1;
        }
        if (got != ys[i]) {
            if (why && why_len)
                snprintf(why, why_len, "differential MISMATCH x=0x%llx: semantic=0x%llx native=0x%llx",
                         (unsigned long long)xs[i], (unsigned long long)ys[i], (unsigned long long)got);
            return -1;
        }
        if (out_checked) (*out_checked)++;
    }
    if (why && why_len) why[0] = '\0';
    return 0;
}

/* ---- G_S x G_M -> G_R: compile the actual program for the machine --------- */

OmegaRealizeSchedule omega_realize_choose_schedule(const OmegaMachineGraph *mg) {
    /* MachineGraph chooses the schedule only; both schedules compute the same function
     * (checked by the differential). Wide issue + enough registers: preload constants. */
    if (mg && mg->pipeline.issue_width >= 4 && mg->registers.gpr_count >= 16) return OMEGA_SCHED_PRELOAD;
    return OMEGA_SCHED_SEQUENTIAL;
}

static int synthesize_realization_impl(const RealizationSynthesisTask *task,
                                       RealizationSynthesisResult *result) {
    if (!task || !task->program || !task->machine || !result) return -1;
    memset(result, 0, sizeof(*result));

    const OmegaProgram *prog = task->program;
    const OmegaMachineGraph *mg = task->machine;

    /* 1. canonical program -> verified semantic operation sequence (fail closed) */
    uint16_t width = 0;
    if (omega_program_realize_check(prog, &width, result->why, sizeof result->why) != 0) return -2;
    if (mg->target_profile != AARCH64_PROFILE_V8A_BAREMETAL) {
        snprintf(result->why, sizeof result->why, "machine profile 0x%02x is not the V0 AArch64 target",
                 (unsigned)mg->target_profile);
        return -2;
    }

    /* 2. machine-aware planning (schedule choice only) */
    result->schedule = omega_realize_choose_schedule(mg);

    /* 3. target realization (AArch64 V0) through the one encoder */
    RealizationObject *r = &result->realization;
    size_t len = 0;
    if (omega_program_emit_schedule(&prog->body, width, result->schedule, r->code_bytes, &len,
                                    sizeof r->code_bytes) != 0) {
        snprintf(result->why, sizeof result->why, "emitter refused the body (code buffer or encoding)");
        return -2;
    }
    r->code_len = len;
    r->semantic_id = prog->program_id;
    r->machine_id = mg->machine_id;
    r->has_machine_id = true;
    r->target_profile = mg->target_profile;
    r->entry_offset = 0;
    result->code_bytes_len = (uint32_t)len;

    /* 4. RealizationId = triple(SEMANTIC_ID, MACHINE_ID, code) */
    omega_realize_compute_triple_id(&prog->program_id, &mg->machine_id, r, &result->realization_id);
    r->realization_id = result->realization_id;
    r->has_id = true;
    result->estimated_cycles = omega_machine_estimate_latency(mg, r);

    /* 5. verification: bindings + structure + differential vs the semantic evaluator */
    int vrc = omega_realization_verify_program(prog, mg, r, &result->inputs_checked, result->why, sizeof result->why);
    memset(&result->verify_report, 0, sizeof result->verify_report);
    if (vrc != 0) {
        result->verify_report.passed = false;
        snprintf(result->verify_report.error_detail, sizeof result->verify_report.error_detail, "%.200s", result->why);
        return vrc == -3 ? -3 : -1;
    }
    omega_verify_v2_properties(NULL, r, &result->verify_report);
    result->solved = result->verify_report.passed;
    return result->solved ? 0 : -1;
}

/* ---- M23 search-trace recorder hook (src/searchtrace/st_hook.h) ----------
 * Off by default: with no hook installed omega_synthesize_realization is the
 * unchanged body above plus one null test. The recorder sees the finished
 * result after the body returns and cannot change it. */
static _Thread_local StHookFn st_realize_hook;
static _Thread_local void *st_realize_hook_ctx;

int omega_realize_set_trace_hook(StHookFn fn, void *ctx) {
    if (!fn || st_realize_hook) return -1;
    st_realize_hook = fn;
    st_realize_hook_ctx = ctx;
    return 0;
}

int omega_realize_clear_trace_hook(void *ctx) {
    if (!st_realize_hook || st_realize_hook_ctx != ctx) return -1;
    st_realize_hook = NULL;
    st_realize_hook_ctx = NULL;
    return 0;
}

int omega_synthesize_realization(const RealizationSynthesisTask *task,
                                 RealizationSynthesisResult *result) {
    int rc = synthesize_realization_impl(task, result);
    if (st_realize_hook && task && result) {
        StEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = ST_EV_REALIZATION;
        ev.u.realize.program = task->program;
        ev.u.realize.machine = task->machine;
        ev.u.realize.result = result;
        ev.u.realize.rc = rc;
        st_realize_hook(st_realize_hook_ctx, &ev);
    }
    return rc;
}

int omega_synthesize_for_dgx_spark(const OmegaProgram *prog, RealizationSynthesisResult *result) {
    if (!prog || !result) return -1;
    OmegaMachineGraph spark;
    omega_machine_build_dgx_spark(&spark);

    RealizationSynthesisTask task;
    omega_realize_task_init(&task, prog, &spark);
    return omega_synthesize_realization(&task, result);
}

int omega_synthesize_for_qemu_virt(const OmegaProgram *prog, RealizationSynthesisResult *result) {
    if (!prog || !result) return -1;
    OmegaMachineGraph qemu;
    omega_machine_build_qemu_virt(&qemu);

    RealizationSynthesisTask task;
    omega_realize_task_init(&task, prog, &qemu);
    return omega_synthesize_realization(&task, result);
}
