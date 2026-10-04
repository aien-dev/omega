/* DUAL-1a: the reference price update (ADR 0031 section 5.1) as a pure,
 * deterministic function of (previous state, bound input, controller).
 *
 *   pressure = (estimate - budget) / scale
 *   deadband = k_sigma * uncertainty / scale
 *   g        = 0 if |pressure| <= deadband else pressure - sign(pressure) * deadband
 *   lambda'  = clip((1 - rho) * lambda + eta * g, 0, lambda_max)
 *
 * Properties (checked by tests/dual): lambda' is finite, 0 <= lambda' <=
 * lambda_max; inside the deadband there is no pressure-driven movement;
 * persistent positive pressure raises lambda until clipped; slack decays it
 * toward zero; eta = 0 never moves on gradient; rho > 0 forgets old scarcity;
 * the same inputs give byte-identical records.
 *
 * Refusals (negative status, no record): NULL, malformed prev/resource/
 * controller, controller digest != prev.controller_id, resource not priced by
 * the controller, resource id or unit mismatch, prev.lambda above lambda_max.
 * Held states (record produced, lambda unchanged): REFUSED (bad values, future
 * generation), STALE (too old, going backwards, evidence not verified),
 * FROZEN (regime change). UNCALIBRATED updates lambda but may never influence
 * production. The cadence parameter is identity only: the caller invokes the
 * update once per cadence tick. */
#ifndef OMEGA_RX_DUAL_UPDATE_H
#define OMEGA_RX_DUAL_UPDATE_H

#include "rx_dual_bind.h"

/* Initial state (tick 0, parent zero) from a first input. The class and budget
 * come from the owning contract (budget_contract cites it). INVARIANT and
 * undeclared classes are refused here as everywhere. lambda starts at 0. */
RxDualStatus rx_dual_init_state(const RxDualResource *res, RxDualClass cls, double budget,
                                const RxDualDigest *budget_contract, const RxDualInput *in,
                                const RxDualController *ctl, uint64_t now_generation,
                                RxDualConstraintState *out);

/* One tick. out->parent = digest(prev), out->tick = prev->tick + 1. */
RxDualStatus rx_dual_update(const RxDualConstraintState *prev, const RxDualResource *res,
                            const RxDualInput *in, const RxDualController *ctl,
                            uint64_t now_generation, RxDualConstraintState *out);

/* The arithmetic alone, for the independent reference test. Returns the new
 * lambda or refuses on nonfinite / nonpositive scale. */
RxDualStatus rx_dual_step_lambda(double lambda, double estimate, double uncertainty, double budget,
                                 double scale, const RxDualController *ctl, double lambda_max, double *out);

#endif /* OMEGA_RX_DUAL_UPDATE_H */
