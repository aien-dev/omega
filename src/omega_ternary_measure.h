/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - MEASUREMENT (ADDITIVE)
 * Exact dynamic instruction counts by ptrace single-step, and wall-clock
 * cost per call. No PMU access or privileges required.
 * ========================================================================= */

#ifndef OMEGA_TERNARY_MEASURE_H
#define OMEGA_TERNARY_MEASURE_H

#include "omega_realize.h"
#include <stddef.h>
#include <stdint.h>

/* Executes real(a, b) once in a traced child and counts the instructions
 * retired inside the realization's code region. */
int omega_t_measure_dyn(const RealizationObject *real, uint64_t a, uint64_t b,
                        uint64_t *out_result, uint64_t *out_insns);

/* Executes real(a, b) natively in-process. */
int omega_t_exec2(const RealizationObject *real, uint64_t a, uint64_t b, uint64_t *out_result);

/* Map once, call many: for differential sweeps. */
typedef struct {
    void *mem;
    size_t size;
} TJit;

int omega_t_jit_open(const RealizationObject *real, TJit *jit);
uint64_t omega_t_jit_call(const TJit *jit, uint64_t a, uint64_t b);
void omega_t_jit_close(TJit *jit);

/* Median over `rounds` of the mean ns per call across `inputs`, minus the
 * same measurement for a bare RET. */
double omega_t_measure_ns(const RealizationObject *real, const uint64_t *inputs_a,
                          const uint64_t *inputs_b, size_t n_inputs, size_t calls_per_round,
                          size_t rounds);

#endif /* OMEGA_TERNARY_MEASURE_H */
