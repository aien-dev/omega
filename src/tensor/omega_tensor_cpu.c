/* CPU realization of M20 OMEGA_TENSOR: the E1 CPU tier (explicit AArch64
 * instructions in omega_numeric_cpu_realize) for elementwise ops, and the
 * reduction seam for reductions. No arithmetic outside those two. */
#include "omega_tensor.h"
#include "omega_tensor_reduce_seam.h"

static int cpu_elementwise(OmegaNumericOp op, const float *a, const float *b, const float *c,
                           float *out, size_t n) {
    return omega_numeric_cpu_realize(op, a, b, c, out, n);
}

static const OmegaTensorRealization CPU_REALIZATION = {
    .name = "CPU_E1_TIER",
    .reduce_order = OMEGA_TENSOR_REDUCE_DECLARED_ORDER,
    .elementwise = cpu_elementwise,
    .reduce = omega_tensor_seam_reduce_cpu,
};

const OmegaTensorRealization *omega_tensor_cpu_realization(void) { return &CPU_REALIZATION; }
