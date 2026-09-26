#include "omega_matvec.h"
#include "omega_core.h"
#include "sha256.h"
#include "aarch64_target.h"
#include "aarch64_encoder.h"
#include "aarch64_decoder.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>

int omega_matvec_spec_init(MatVecSemanticSpec *spec, const char *name, uint32_t max_m, uint32_t max_n) {
    if (!spec) return -1;
    memset(spec, 0, sizeof(*spec));
    snprintf(spec->name, sizeof(spec->name), "%s", name ? name : "omega_matvec");
    spec->max_m = max_m;
    spec->max_n = max_n;

    /* Compute deterministic canonical SemanticId for MatVec operator */
    uint8_t buffer[512];
    size_t pos = 0;
    buffer[pos++] = 'O'; buffer[pos++] = 'M'; buffer[pos++] = 'G'; buffer[pos++] = '0';
    buffer[pos++] = KIND_OPERATION;
    buffer[pos++] = 0xAA; /* MATVEC operator tag */

    /* Write dimensions */
    buffer[pos++] = (uint8_t)((max_m >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((max_m >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((max_m >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(max_m & 0xFF);

    buffer[pos++] = (uint8_t)((max_n >> 24) & 0xFF);
    buffer[pos++] = (uint8_t)((max_n >> 16) & 0xFF);
    buffer[pos++] = (uint8_t)((max_n >> 8) & 0xFF);
    buffer[pos++] = (uint8_t)(max_n & 0xFF);

    size_t nlen = strlen(spec->name);
    if (pos + nlen <= sizeof(buffer)) {
        memcpy(&buffer[pos], spec->name, nlen);
        pos += nlen;
    }

    sha256_hash(buffer, pos, spec->spec_id.bytes);
    return 0;
}

int omega_matvec_kernel_init(MatVecLivingKernel *kernel, const MatVecSemanticSpec *spec, const OmegaMachineGraph *mg) {
    if (!kernel || !spec || !mg) return -1;
    memset(kernel, 0, sizeof(*kernel));
    kernel->spec = *spec;
    kernel->machine = mg;

    /* Synthesize all 3 candidate realizations */
    for (int k = 0; k < MATVEC_REALIZATION_COUNT; ++k) {
        if (omega_matvec_synthesize(kernel, (MatVecRealizationKind)k, &kernel->realizations[k]) != 0) {
            return -1;
        }
    }
    kernel->realization_count = MATVEC_REALIZATION_COUNT;
    kernel->is_initialized = true;
    return 0;
}

static int emit_matvec_scalar(uint8_t *code, size_t *pos, size_t max_len) {
    /* Word 0-1: Guards */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X3, 17); /* -> ret (17) */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X4, 16); /* -> ret (17) */
    /* Word 2: i_count = M */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X5, REG_X3);
    /* Word 3 (row_loop): */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X6, REG_X0);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X7, REG_X1);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X8, REG_XZR);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X13, REG_X4);
    /* Word 7 (col_loop): */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X10, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X11, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X12, REG_X10, REG_X11);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X12);
    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X13, REG_X13, 1);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -5); /* -> col_loop (7) */
    /* Word 13 (row_end): */
    aarch64_emit_str_x_post(code, pos, max_len, REG_X8, REG_X2, 8);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X0, REG_X6);
    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X5, REG_X5, 1);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -13); /* -> row_loop (3) */
    /* Word 17 (ret): */
    aarch64_emit_ret(code, pos, max_len);
    return 0;
}

static int emit_matvec_unroll2(uint8_t *code, size_t *pos, size_t max_len) {
    /* Word 0-1: Guards */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X3, 30); /* -> ret (30) */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X4, 29); /* -> ret (30) */
    /* Word 2-5: Setup pair count and remainder */
    aarch64_emit_movz(code, pos, max_len, true, REG_X16, 1, 0);
    aarch64_emit_and_reg(code, pos, max_len, true, REG_X15, REG_X4, REG_X16); /* rem = N & 1 */
    aarch64_emit_sub_reg(code, pos, max_len, true, REG_X14, REG_X4, REG_X15); /* pairs_count = N - rem */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X5, REG_X3);          /* i_count = M */

    /* Word 6 (row_loop): */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X6, REG_X0);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X7, REG_X1);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X8, REG_XZR);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X13, REG_X14);
    aarch64_emit_cbz(code, pos, max_len, true, REG_X13, 11); /* -> check_rem (21) */

    /* Word 11 (unroll2_loop): */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X10, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X11, REG_X7, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X12, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X16, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X10, REG_X10, REG_X11);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X12, REG_X12, REG_X16);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X10);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X12);
    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X13, REG_X13, 2);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -9); /* -> unroll2_loop (11) */

    /* Word 21 (check_rem): */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X15, 5); /* -> row_end (26) */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X10, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X11, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X10, REG_X10, REG_X11);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X10);

    /* Word 26 (row_end): */
    aarch64_emit_str_x_post(code, pos, max_len, REG_X8, REG_X2, 8);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X0, REG_X6);
    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X5, REG_X5, 1);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -23); /* -> row_loop (6) */

    /* Word 30 (ret): */
    aarch64_emit_ret(code, pos, max_len);
    return 0;
}

static int emit_matvec_unroll4_dual(uint8_t *code, size_t *pos, size_t max_len) {
    /* Word 0-1: Guards */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X3, 43); /* -> ret (43) */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X4, 42); /* -> ret (43) */
    /* Word 2-5: Setup quad count and remainder */
    aarch64_emit_movz(code, pos, max_len, true, REG_X16, 3, 0);
    aarch64_emit_and_reg(code, pos, max_len, true, REG_X15, REG_X4, REG_X16); /* rem = N & 3 */
    aarch64_emit_sub_reg(code, pos, max_len, true, REG_X14, REG_X4, REG_X15); /* quad_count = N - rem */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X5, REG_X3);          /* i_count = M */

    /* Word 6 (row_loop): */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X6, REG_X0);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X7, REG_X1);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X8, REG_XZR);          /* acc0 = 0 */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X9, REG_XZR);          /* acc1 = 0 */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X13, REG_X14);
    aarch64_emit_cbz(code, pos, max_len, true, REG_X13, 19); /* -> check_rem4 (30) */

    /* Word 12 (unroll4_loop): */
    /* Pair 0 -> acc0 */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X10, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X11, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X10, REG_X10, REG_X11);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X10);

    /* Pair 1 -> acc1 (independent ALU port!) */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X12, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X16, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X12, REG_X12, REG_X16);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X9, REG_X9, REG_X12);

    /* Pair 2 -> acc0 */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X10, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X11, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X10, REG_X10, REG_X11);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X10);

    /* Pair 3 -> acc1 (independent ALU port!) */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X12, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X16, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X12, REG_X12, REG_X16);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X9, REG_X9, REG_X12);

    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X13, REG_X13, 4);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -17); /* -> unroll4_loop (12) */

    /* Word 30 (check_rem4): */
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X9); /* acc = acc0 + acc1 */
    aarch64_emit_cbz(code, pos, max_len, true, REG_X15, 8);                  /* -> row_end (39) */
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X13, REG_X15);        /* rem_counter = X15 */

    /* Word 33 (rem_loop): */
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X10, REG_X6, 8);
    aarch64_emit_ldr_x_post(code, pos, max_len, REG_X11, REG_X7, 8);
    aarch64_emit_mul_reg(code, pos, max_len, true, REG_X10, REG_X10, REG_X11);
    aarch64_emit_add_reg(code, pos, max_len, true, REG_X8, REG_X8, REG_X10);
    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X13, REG_X13, 1);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -5); /* -> rem_loop (33) */

    /* Word 39 (row_end): */
    aarch64_emit_str_x_post(code, pos, max_len, REG_X8, REG_X2, 8);
    aarch64_emit_mov_reg(code, pos, max_len, true, REG_X0, REG_X6);
    aarch64_emit_subs_imm(code, pos, max_len, true, REG_X5, REG_X5, 1);
    aarch64_emit_b_cond(code, pos, max_len, COND_NE, -36); /* -> row_loop (6) */

    /* Word 43 (ret): */
    aarch64_emit_ret(code, pos, max_len);
    return 0;
}

int omega_matvec_synthesize(MatVecLivingKernel *kernel, MatVecRealizationKind kind, MatVecRealization *out_real) {
    if (!kernel || !out_real) return -1;
    memset(out_real, 0, sizeof(*out_real));

    out_real->kind = kind;
    out_real->realization.target_profile = kernel->machine->target_profile;
    out_real->realization.entry_offset = 0;
    out_real->realization.semantic_id = kernel->spec.spec_id;
    out_real->realization.machine_id = kernel->machine->machine_id;
    out_real->realization.has_machine_id = true;

    size_t pos = 0;
    size_t max_len = sizeof(out_real->realization.code_bytes);

    switch (kind) {
        case MATVEC_REALIZATION_SCALAR:
            out_real->name = "matvec_scalar";
            out_real->unroll_factor = 1;
            out_real->accumulators = 1;
            emit_matvec_scalar(out_real->realization.code_bytes, &pos, max_len);
            break;
        case MATVEC_REALIZATION_UNROLL2:
            out_real->name = "matvec_unroll2";
            out_real->unroll_factor = 2;
            out_real->accumulators = 1;
            emit_matvec_unroll2(out_real->realization.code_bytes, &pos, max_len);
            break;
        case MATVEC_REALIZATION_UNROLL4_DUAL:
            out_real->name = "matvec_unroll4_dual";
            out_real->unroll_factor = 4;
            out_real->accumulators = 2;
            emit_matvec_unroll4_dual(out_real->realization.code_bytes, &pos, max_len);
            break;
        default:
            return -1;
    }

    out_real->realization.code_len = pos;

    /* Compute Triple Binding Identity */
    omega_realize_compute_triple_id(&kernel->spec.spec_id, &kernel->machine->machine_id,
                                    &out_real->realization, &out_real->realization_id);
    out_real->realization.realization_id = out_real->realization_id;
    out_real->realization.has_id = true;

    /* Mandatory M7 V0 Structural Verification */
    VerifyReport rep;
    memset(&rep, 0, sizeof(rep));
    omega_verify_v0_structural(NULL, &out_real->realization, &rep);
    out_real->verify_report = rep;
    out_real->is_verified = rep.passed;

    return rep.passed ? 0 : -1;
}

void omega_matvec_reference(const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N) {
    if (!A || !x || !y) return;
    for (uint32_t i = 0; i < M; ++i) {
        uint64_t acc = 0;
        for (uint32_t j = 0; j < N; ++j) {
            acc += A[(size_t)i * N + j] * x[j];
        }
        y[i] = acc;
    }
}

int omega_matvec_exec(const MatVecRealization *real, const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N) {
    if (!real || !A || !x || !y || real->realization.code_len == 0) return -1;

    typedef void (*MatVecFn)(const uint64_t *A, const uint64_t *x, uint64_t *y, uint64_t M, uint64_t N);

    void *exec_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (exec_mem == MAP_FAILED) return -1;

    memcpy(exec_mem, real->realization.code_bytes, real->realization.code_len);
    __builtin___clear_cache((char*)exec_mem, (char*)exec_mem + real->realization.code_len);

    if (mprotect(exec_mem, 4096, PROT_READ | PROT_EXEC) != 0) {
        munmap(exec_mem, 4096);
        return -1;
    }

    union {
        void *ptr;
        MatVecFn fn;
    } u;
    u.ptr = exec_mem;
    u.fn(A, x, y, (uint64_t)M, (uint64_t)N);

    munmap(exec_mem, 4096);
    return 0;
}

int omega_matvec_benchmark(const MatVecRealization *real, uint32_t M, uint32_t N, size_t iterations, MatVecBenchmarkMetric *out_metric) {
    if (!real || !out_metric || M == 0 || N == 0 || iterations == 0) return -1;
    memset(out_metric, 0, sizeof(*out_metric));
    out_metric->m = M;
    out_metric->n = N;

    size_t mat_size = (size_t)M * N;
    uint64_t *A = (uint64_t*)malloc(mat_size * sizeof(uint64_t));
    uint64_t *x = (uint64_t*)malloc(N * sizeof(uint64_t));
    uint64_t *y_act = (uint64_t*)malloc(M * sizeof(uint64_t));
    uint64_t *y_ref = (uint64_t*)malloc(M * sizeof(uint64_t));

    if (!A || !x || !y_act || !y_ref) {
        free(A); free(x); free(y_act); free(y_ref);
        return -1;
    }

    /* Initialize pseudo-random reproducible test vectors */
    for (size_t i = 0; i < mat_size; ++i) A[i] = (uint64_t)(i * 3 + 1);
    for (size_t j = 0; j < N; ++j) x[j] = (uint64_t)(j * 5 + 2);
    memset(y_act, 0, M * sizeof(uint64_t));
    memset(y_ref, 0, M * sizeof(uint64_t));

    /* Oracle reference calculation */
    omega_matvec_reference(A, x, y_ref, M, N);

    /* Allocate execution page once for benchmark duration */
    typedef void (*MatVecFn)(const uint64_t *A, const uint64_t *x, uint64_t *y, uint64_t M, uint64_t N);
    void *exec_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (exec_mem == MAP_FAILED) {
        free(A); free(x); free(y_act); free(y_ref);
        return -1;
    }
    memcpy(exec_mem, real->realization.code_bytes, real->realization.code_len);
    __builtin___clear_cache((char*)exec_mem, (char*)exec_mem + real->realization.code_len);
    mprotect(exec_mem, 4096, PROT_READ | PROT_EXEC);

    union {
        void *ptr;
        MatVecFn fn;
    } u;
    u.ptr = exec_mem;

    /* Warm-up run */
    u.fn(A, x, y_act, (uint64_t)M, (uint64_t)N);

    /* Verify numerical parity */
    out_metric->numerical_parity = true;
    for (size_t i = 0; i < M; ++i) {
        if (y_act[i] != y_ref[i]) {
            out_metric->numerical_parity = false;
            break;
        }
    }

    /* Timed benchmarking loop */
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    for (size_t it = 0; it < iterations; ++it) {
        u.fn(A, x, y_act, (uint64_t)M, (uint64_t)N);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    munmap(exec_mem, 4096);
    free(A); free(x); free(y_act); free(y_ref);

    uint64_t total_ns = (uint64_t)(ts_end.tv_sec - ts_start.tv_sec) * 1000000000ULL +
                        (uint64_t)(ts_end.tv_nsec - ts_start.tv_nsec);
    out_metric->elapsed_ns = total_ns / iterations;

    /* Calculate operations: 2 * M * N per matvec */
    double ops = 2.0 * (double)M * (double)N;
    double time_s = (double)out_metric->elapsed_ns / 1e9;
    if (time_s > 0.0) {
        out_metric->gflops = (ops / time_s) / 1e9;
        /* Memory traffic: (M*N + N + M) * 8 bytes */
        double bytes = (double)((size_t)M * N + N + M) * 8.0;
        out_metric->bandwidth_gbps = (bytes / time_s) / 1e9;
    }

    return 0;
}

int omega_matvec_adapt(MatVecLivingKernel *kernel, uint32_t M, uint32_t N, MatVecSelectionDecision *decision) {
    if (!kernel || !decision || !kernel->is_initialized) return -1;
    memset(decision, 0, sizeof(*decision));

    size_t iters = 500;
    if ((size_t)M * N > 1024) iters = 100;
    if ((size_t)M * N > 16384) iters = 20;

    MatVecBenchmarkMetric metrics[MATVEC_REALIZATION_COUNT];
    for (int k = 0; k < MATVEC_REALIZATION_COUNT; ++k) {
        omega_matvec_benchmark(&kernel->realizations[k], M, N, iters, &metrics[k]);
        decision->benchmark_ns[k] = metrics[k].elapsed_ns;
    }

    /* Select lowest latency realization */
    uint64_t min_ns = decision->benchmark_ns[0];
    MatVecRealizationKind best_kind = MATVEC_REALIZATION_SCALAR;

    for (int k = 1; k < MATVEC_REALIZATION_COUNT; ++k) {
        if (decision->benchmark_ns[k] < min_ns) {
            min_ns = decision->benchmark_ns[k];
            best_kind = (MatVecRealizationKind)k;
        }
    }

    decision->selected_kind = best_kind;
    decision->adapted = true;
    if (decision->benchmark_ns[best_kind] > 0) {
        decision->speedup_ratio = (double)decision->benchmark_ns[MATVEC_REALIZATION_SCALAR] /
                                  (double)decision->benchmark_ns[best_kind];
    } else {
        decision->speedup_ratio = 1.0;
    }

    return 0;
}

int omega_matvec_dispatch(MatVecLivingKernel *kernel, const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N) {
    if (!kernel || !A || !x || !y || !kernel->is_initialized) return -1;

    /* Living adaptation heuristic based on MachineGraph cache sizing:
     * - For small sizes (N <= 8), startup overhead of unrolling favors scalar or unroll2.
     * - For larger sizes (N > 8), unroll4_dual saturates multi-issue pipelines.
     */
    MatVecRealizationKind choice;
    if (N <= 8) {
        choice = MATVEC_REALIZATION_UNROLL2;
    } else {
        choice = MATVEC_REALIZATION_UNROLL4_DUAL;
    }

    return omega_matvec_exec(&kernel->realizations[choice], A, x, y, M, N);
}

void omega_matvec_kernel_destroy(MatVecLivingKernel *kernel) {
    if (!kernel) return;
    memset(kernel, 0, sizeof(*kernel));
}
