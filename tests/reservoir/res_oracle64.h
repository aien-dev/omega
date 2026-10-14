/* Independent binary64 oracle for the reservoir tests. Written from the
 * equations of Text S4 as the reference code integrates them (see
 * docs/reservoir/RESERVOIR_REFERENCE.md), NOT from omega_reservoir.c:
 *   - state is the pore density N itself (not ln N), as in the reference;
 *   - 1 - (N/Nss)^Z with libm pow, alpha and Nss with libm exp;
 *   - M V' = B solved by LU with partial pivoting;
 *   - adaptive Dormand-Prince 5(4) with relative/absolute tolerances
 *     (the reference uses ode15s with RelTol 1e-6, AbsTol 1e-8), restarted
 *     at every input knot so the piecewise-linear current is smooth inside
 *     each integration interval.
 * It reads the module's topology and parameter structs as plain data only.
 * Test code: uses libm, allocates nothing. */
#ifndef RES_ORACLE64_H
#define RES_ORACLE64_H

#include "omega_reservoir.h"

typedef struct {
    double rtol, atol;
    unsigned long steps_accepted, steps_rejected;
} res64_opts;

/* Integrates from V = 0 and N(0) = n_init[side] (side order as the module:
 * 2e = a->b, 2e+1 = b->a over topo->edge) across knots 0..knot_count-1 of
 * the knot-major current table with spacing dt_s. Writes the node voltages
 * at every knot into v_out[k * node_count + i] and the final densities into
 * n_final (may be NULL). Returns 0 on success, -1 on failure (step size
 * underflow or non-finite values). */
int res64_run(const OmegaResTopology *topo, const OmegaResParams *par, const double *n_init,
              const float *current_a, unsigned knot_count, double dt_s, res64_opts *opt, double *v_out,
              double *n_final);

#endif /* RES_ORACLE64_H */
