#ifndef OMEGA_MATVEC_H
#define OMEGA_MATVEC_H

#include "omega_types.h"
#include "omega_machine.h"
#include "omega_realize.h"
#include "omega_verify.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MATVEC_MAX_REALIZATIONS 3

typedef enum {
    MATVEC_REALIZATION_SCALAR = 0,       /* Baseline sequential row/col loop */
    MATVEC_REALIZATION_UNROLL2 = 1,      /* 2-way inner loop unrolling */
    MATVEC_REALIZATION_UNROLL4_DUAL = 2, /* 4-way inner loop unroll with dual accumulators (4-wide Neoverse V2) */
    MATVEC_REALIZATION_COUNT = 3
} MatVecRealizationKind;

typedef struct {
    SemanticId spec_id;
    char name[64];
    uint32_t max_m;
    uint32_t max_n;
} MatVecSemanticSpec;

typedef struct {
    MatVecRealizationKind kind;
    const char *name;
    RealizationObject realization;
    SemanticId realization_id;
    uint32_t unroll_factor;
    uint32_t accumulators;
    VerifyReport verify_report;
    bool is_verified;
} MatVecRealization;

typedef struct {
    uint32_t m;
    uint32_t n;
    uint64_t elapsed_ns;
    uint64_t cycle_count;
    double gflops;
    double bandwidth_gbps;
    bool numerical_parity;
} MatVecBenchmarkMetric;

typedef struct {
    MatVecRealizationKind selected_kind;
    uint64_t benchmark_ns[MATVEC_REALIZATION_COUNT];
    double speedup_ratio;
    bool adapted;
} MatVecSelectionDecision;

typedef struct {
    MatVecSemanticSpec spec;
    const OmegaMachineGraph *machine;
    MatVecRealization realizations[MATVEC_REALIZATION_COUNT];
    size_t realization_count;
    bool is_initialized;
} MatVecLivingKernel;

/* Initialize semantic specification for MatVec operator */
int omega_matvec_spec_init(MatVecSemanticSpec *spec, const char *name, uint32_t max_m, uint32_t max_n);

/* Initialize living kernel with specification and target machine graph */
int omega_matvec_kernel_init(MatVecLivingKernel *kernel, const MatVecSemanticSpec *spec, const OmegaMachineGraph *mg);

/* Synthesize a specific realization kind directly into memory */
int omega_matvec_synthesize(MatVecLivingKernel *kernel, MatVecRealizationKind kind, MatVecRealization *out_real);

/* Reference Oracle implementation: y = A * x (pure semantic calculation) */
void omega_matvec_reference(const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N);

/* Execute realization on native hardware */
int omega_matvec_exec(const MatVecRealization *real, const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N);

/* Benchmark a realization over a given dimension regime (M x N) */
int omega_matvec_benchmark(const MatVecRealization *real, uint32_t M, uint32_t N, size_t iterations, MatVecBenchmarkMetric *out_metric);

/* Living kernel adaptive selection: evaluate regimes and dynamically pick best realization */
int omega_matvec_adapt(MatVecLivingKernel *kernel, uint32_t M, uint32_t N, MatVecSelectionDecision *decision);

/* Dispatch optimal realization for regime */
int omega_matvec_dispatch(MatVecLivingKernel *kernel, const uint64_t *A, const uint64_t *x, uint64_t *y, uint32_t M, uint32_t N);

/* Free resources owned by kernel */
void omega_matvec_kernel_destroy(MatVecLivingKernel *kernel);

#endif /* OMEGA_MATVEC_H */
