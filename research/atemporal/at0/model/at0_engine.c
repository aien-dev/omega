/* Engine orchestration: builds every quantity of AT0_RESULT_V1 section 3 from
 * the case alone, attaches ESTIMATED error bounds (README section "Error
 * estimates"), and decides label status with exact integer comparisons. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

#define EPS 1.1102230246251565e-16   /* 2^-53 */

static double fmax_d(double a, double b) { return a > b ? a : b; }

static at0_status bound_from(double est, at0_scaled *out)
{
    if (!isfinite(est) || est < 0) return AT0_ERR_NONFINITE;
    return at0_scaled_from_double_ceil(est, out);
}

static at0_status run(const at0_case *c, at0_engine_result *out, int reversed)
{
    if (!c || !out) return AT0_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    out->bound_kind = AT0_BOUND_ESTIMATED;
    out->label_count = c->label_count;
    at0_status st;

    at0_system_hamiltonian hs;
    if ((st = at0_hamiltonian_system(c, &hs)) != AT0_OK) return st;
    at0_kernel ker;
    if ((st = at0_constraint_kernel(c, &hs, &ker)) != AT0_OK) return st;
    out->kernel_dim = ker.kernel_dim;
    at0_state psi;
    if ((st = at0_constraint_physical_state(c, &hs, &ker, &psi)) != AT0_OK) return st;
    double psi_norm2;
    if ((st = at0_state_norm2(&psi, &psi_norm2)) != AT0_OK) return st;
    out->psi_nonzero = psi_norm2 > 0.0;

    double n = (double)c->clock_dim, m = (double)c->label_count;
    double w = at0_rat_to_double(c->povm_weight);
    double hmax = fmax_d(fabs(at0_rat_to_double(c->clock_energies[0])),
                         fabs(at0_rat_to_double(c->clock_energies[c->clock_dim - 1])))
                  + fabs(at0_rat_to_double(c->h0)) + at0_rat_to_double(c->h_norm);

    if (out->psi_nonzero) {
        if ((st = at0_constraint_residual(c, &hs, &psi, &out->constraint_residual)) != AT0_OK) return st;
        if ((st = bound_from(32.0 * EPS * n * (hmax + 1.0), &out->constraint_residual_bound)) != AT0_OK) return st;
    } else {
        out->constraint_residual = 0.0;
        out->constraint_residual_bound.n = 0; out->constraint_residual_bound.k = 0;
    }
    if ((st = at0_povm_residual(c, &out->povm_residual)) != AT0_OK) return st;
    if ((st = bound_from(16.0 * EPS * n * (m * w + 1.0), &out->povm_residual_bound)) != AT0_OK) return st;

    for (int i = 0; i < c->label_count; i++) {
        int k = reversed ? c->label_count - 1 - i : i;
        at0_label_values *lv = &out->label[k];
        memset(lv, 0, sizeof *lv);
        if (!out->psi_nonzero) { lv->status = AT0_LABEL_UNDEFINED; continue; }
        at0_conditional cond;
        if ((st = at0_conditional_compute(c, &psi, psi_norm2, k, &cond)) != AT0_OK) return st;
        lv->clock_probability = cond.clock_probability;
        if ((st = bound_from(32.0 * EPS * (n + 4.0) * (cond.clock_probability + 1.0), &lv->clock_probability_bound)) != AT0_OK) return st;
        int le, gt;
        if ((st = at0_exact_prob_status(lv->clock_probability, lv->clock_probability_bound, c->tol_zero_probability, &le, &gt)) != AT0_OK) return st;
        lv->status = le ? AT0_LABEL_UNDEFINED : (gt ? AT0_LABEL_DEFINED : AT0_LABEL_INDETERMINATE);
        if (lv->status != AT0_LABEL_DEFINED) continue;
        if (!cond.defined_numeric) return AT0_ERR_INTERNAL;   /* DEFINED implies ||phi|| > 0 */
        for (int ax = 0; ax < 3; ax++) {
            for (int sg = 0; sg < 2; sg++) {
                if ((st = at0_observable_probability(&cond.rho, (at0_axis)ax, (at0_sign)sg, &lv->pauli[ax][sg])) != AT0_OK) return st;
                if ((st = bound_from(32.0 * EPS * (n + 8.0), &lv->pauli_bound[ax][sg])) != AT0_OK) return st;
            }
        }
    }
    return AT0_OK;
}

at0_status at0_engine_run(const at0_case *c, at0_engine_result *out) { return run(c, out, 0); }
at0_status at0_engine_run_reversed(const at0_case *c, at0_engine_result *out) { return run(c, out, 1); }
