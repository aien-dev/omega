#include "omega_tensor_reduce_seam.h"

#include <stdlib.h>
#include <string.h>

#include "omega_numeric.h"

#include "omega_numeric_reduce.h"

_Static_assert(sizeof(OMEGA_REDUCE_DECLARED_ORDER) == sizeof(OMEGA_TENSOR_REDUCE_DECLARED_ORDER),
               "tensor reduce order must equal the E1 reduction order");

int omega_tensor_seam_reduce_cpu(OmegaTensorReduceOp op, const float *x, size_t n, float *out) {
    if (strcmp(OMEGA_REDUCE_DECLARED_ORDER, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) != 0)
        return OMEGA_NUMERIC_ERR_BAD_ARGS;
    OmegaReduceOp r;
    switch (op) {
    case OMEGA_TR_SUM:  r = OMEGA_RED_SUM;  break;
    case OMEGA_TR_MAX:  r = OMEGA_RED_MAX;  break;
    case OMEGA_TR_MIN:  r = OMEGA_RED_MIN;  break;
    case OMEGA_TR_MEAN: r = OMEGA_RED_MEAN; break;
    default: return OMEGA_NUMERIC_ERR_BAD_ARGS;
    }
    if (n == 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    return omega_reduce_cpu(r, x, n, out);
}

const char *omega_tensor_seam_reduce_source(void) { return "E1_WP_D_OMEGA_REDUCE_CPU"; }

