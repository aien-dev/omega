# DUAL records (DUAL-0a), ADR 0031 / ARCH-0031

Standalone module `src/dual/` (`mk/dual.mk`, targets `test-dual-types`, `dual-purity`, `test-dual`; not in `all` or `test`;
nothing under `src/runtime/` includes it). DUAL is scarcity information. It never authorizes, promotes or decides validity.

## Records and kind bytes (format version 1)
| Kind | Byte | Domain | Fields (canonical little-endian order) |
|---|---|---|---|
| resource | 1 | `omega.dual.resource.v1` | u32 resource_id, u32 unit, f64 scale, dig contract |
| constraint | 2 | `omega.dual.constraint.v1` | u32 resource_id, u32 unit, u32 class, f64 budget, dig budget_contract, dig observation_ref, dig estimate_ref, u32 estimate_kind, f64 estimate, f64 uncertainty, dig calibration_ref, f64 lambda, u32 lambda_state, dig controller_id, u64 generation, u64 tick, dig evidence_root, dig parent |
| controller | 3 | `omega.dual.controller.v1` | f64 eta, f64 rho, f64 k_sigma, u64 max_age, u64 cadence, u32 n, n x (u32 resource_id, f64 lambda_max) |
| price vector | 4 | `omega.dual.pricevec.v1` | u64 generation, dig context, u32 n, n x (u32 resource_id, dig state) |
| recommendation | 5 | `omega.dual.recommendation.v1` | dig decision_site, dig price_vector, u64 generation, u32 n_candidates, n x dig, u32 actual, u32 recommended, u32 n_consequences, n x (u32 resource_id, u32 unit, f64 predicted, f64 predicted_sd, u32 has_measured, f64 measured, f64 error), u32 authority (= 0) |
| outcome | 6 | `omega.dual.outcome.v1` | dig recommendation, dig before_vector, dig after_vector, u32 target_resource_id, u64 generation_before, u64 generation_after, f64 lambda_before, f64 lambda_after |
| site | 7 | `omega.dual.site.v1` | u32 site_id, 32 bytes name (printable ASCII, NUL padded), dig owner, u32 max_alternatives |

Every encoding starts with `u8 kind, u8 version`. Doubles are IEEE-754 binary64 bits; -0.0 is written as +0.0; NaN and Inf are
refused before encoding. Digest = SHA-256(domain || 0x00 || encoding). Decoders refuse a wrong kind byte, a wrong version,
truncation and trailing bytes, then run the same validation as the constructor. No wall-clock value is part of any identity.

## Rules enforced by `rx_dual_check_*` (ADR 0031 section 3 and 4)
- Class: only `CAPACITY` (diagnostic price) and `SOFT` (priceable) construct. `INVARIANT` and an undeclared class are refused.
- Units: the unit registry is closed (`RxDualUnit`); `NONE` is refused; a constraint whose unit differs from its registry entry is refused (no implicit conversion).
- Scale strictly positive and finite. Uncertainty and lambda finite and nonnegative. lambda <= lambda_max is enforced by the controller (DUAL-1a).
- `FRESH` requires a calibration receipt digest; a zero `calibration_ref` may only be `UNCALIBRATED`, `STALE`, `FROZEN` or `REFUSED`.
- `MEASURED` requires an `observation_ref`. `estimate_ref`, `budget_contract`, `controller_id`, `evidence_root` are required.
- History: `tick == 0` iff `parent` is zero.
- Price vector: strictly ascending resource ids (duplicates and unordered input refused, never sorted), all states at one generation, nonzero context.
- Recommendation: `authority` must be `NONE`; indices in range; consequences in ascending resource order; `error` must equal the recomputed `(measured - predicted) / predicted_sd`; an unmeasured row carries zero.
- Controller: eta >= 0, rho in [0,1], k_sigma >= 0, max_age >= 1, cadence >= 1, per-resource lambda_max > 0, ascending ids. Any parameter change changes `controller_id`.

Status: DUAL-0a, DUAL-0b (rx_dual_bind.h: binding to est_* records, evidence verification, FRESH/STALE/FROZEN/UNCALIBRATED/REFUSED) and DUAL-1a (rx_dual_update.h: reference update) IMPLEMENTED; `DUAL_CONSTRAINT_CONTRACT = PASS` on host (evidence/DUAL/0b-1a-binding-controller); `DUAL_PRICE_REFERENCE` waits for DUAL-1b replay.
