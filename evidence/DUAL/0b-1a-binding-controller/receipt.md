# DUAL-0b + DUAL-1a receipt: ESTIMATION binding, staleness, reference controller (ADR 0031 s4.2, s5)

| Field | Value |
|---|---|
| repository | aien-dev/omega |
| base commit | f056b77f64ee94a80f4ed2899b5f9495aa3ef840 (branch dual/0a-records, PR #266, on top of main cb7cc216147664549484fcca8e8ceadbd6f6cdbf) |
| candidate commit | this commit |
| compiler | cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| flags | EST_CFLAGS (-std=c11 -Wall -Wextra -Werror -pedantic -O2 -ffp-contract=off -fno-fast-math) and EST_ASAN |
| host | Spark aarch64 Linux, host-only, no GPU |
| controller schema | RxDualController (eta, rho, k_sigma, max_age, cadence, per-resource lambda_max), digest domain omega.dual.controller.v1 = controller_id |

## Commands and results
| Command | Result |
|---|---|
| make dual-purity | PASS (rx_dual.o, rx_dual_bind.o, rx_dual_update.o: no authority/promotion/process symbol) |
| make test-dual-types | 14107 / 1667 (ASan) checks, 0 failures |
| make test-dual-bind | 85 / 85 checks, 0 failures |
| make test-dual-update | 355348 / 35837 (ASan) checks, 0 failures |

## What is proven
- Binding: estimate and uncertainty are copied from est_belief / est_prediction / est_observation and the record digest is cited; a prediction must cite the exact model (unit source) and prior belief (evidence source) by digest; a declared prior (zero evidence root) is refused as evidence; est units map by one explicit table, NONE/unknown refused.
- Evidence: a belief's root must equal est_evidence_root_extend(parent_root, digest(obs)); broken root, wrong observation, wrong parent root and an input not bound from the belief are all refused. The DUAL state root chains estimate_ref digests with the same primitive.
- Classification: REFUSED (NaN/Inf/negative uncertainty/missing digests/future generation), STALE (age > max_age, older than prev, unverified evidence), FROZEN (regime signal), UNCALIBRATED (no EST-3 receipt), FRESH; unit mismatch is a structural refusal (no conversion).
- Controller: analytic cases match the independent closed form (tests/dual/dual_ref.c, derived on paper from s5.1) to 1e-9: single binding constraint, slack constraint decaying to 0, two competing constraints, inside-deadband no movement, clip saturation, zero eta no movement, rho>0 geometric forgetting; byte-identical records for identical inputs. Property suite (3000 rounds x 39 ticks): lambda finite, 0 <= lambda <= lambda_max, held under STALE, equals clipped recursion otherwise. Held states keep lambda, estimate, generation and evidence root; parent/tick advance. Parameter mutation (controller_id mismatch), unknown resource, unit/id mismatch, zero/negative scale, INVARIANT class, lambda above max, malformed parent, overflow to Inf are refused with no record.

## Negative controls (mutants, same suites)
| Mutant | Result |
|---|---|
| remove lambda_max clipping | test_dual_update 35634 failures: FAIL (killed) |
| remove deadband (g always = pressure) | test_dual_update 1052 failures: FAIL (killed) |
| remove controller_id check | test_dual_update 1 failure: FAIL (killed) |
| unverified evidence no longer STALE | test_dual_bind 1 failure: FAIL (killed) |
| evidence root comparison removed | test_dual_bind 4 failures: FAIL (killed) |

## Verdict
DUAL-0b IMPLEMENTED. DUAL-1a IMPLEMENTED. DUAL_CONSTRAINT_CONTRACT: PASS on host for the record/binding contract (typed records,
canonical round trip, kind bytes and digest domains, unit registry, class rules, staleness detection, property tests, zero runtime
linkage). DUAL_PRICE_REFERENCE: NOT YET CLAIMED (needs DUAL-1b replay on digest-bound traces with the oscillation bound).
No production influence. EST-3 remains FAILED on main, so no price may control anything.
