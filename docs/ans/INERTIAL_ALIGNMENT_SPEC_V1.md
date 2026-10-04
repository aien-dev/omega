# Inertial Alignment v1 (ANS, the Alignment Navigation System)

Status: implemented in `src/ans/`, tested by `make test-inertial-alignment`.
Cites: ARCH-0020 (estimation, EST-0/EST-1), ARCH-0017 (ARGUS observes, never
authorizes), `EVOLUTION_ARENA_SPEC_V1` section 5 (promotion contract), and
ARCH-0032 (the ADR for this module, written in parallel in aien-architecture).

## Idea

An inertial navigation system keeps a position estimate by integrating motion
and goes stale as error accumulates, until an outside fix corrects it. ANS
treats alignment the same way. An agent's drift from its goal, intent,
provenance and scope is a small state estimated by the EST-1 Kalman filter.
Every action is a step that adds uncertainty. Sensors of three trust classes
pull the estimate back. A control law turns the estimate into an autonomy
tier. A promotion gate keeps self-improvement from touching the compass.

## What ANS is NOT

- Not an authority. ANS measures and recommends. It never grants, revokes or
  promotes. A verdict is data for the authority to consult (ARCH-0017 shape).
- Not a promotion path. The right to promote stays with the operator-held
  principal (Arena spec section 5.1). ANS never calls `rx_gen_promote`; the
  purity check forbids any reference to `rx_`.
- Not a new filter. It links `est_kf` and does not reimplement it.
- Not a proof of alignment. Drift is an estimate with declared noise.
- Not wired into the resident runtime, `all` or `test`.

## What is frozen

The reference (the compass) is frozen. `ans_reference_freeze` validates it,
sets `frozen = 1` and returns its SHA-256 digest. Every other API takes a
`const ans_reference *` plus the expected digest and refuses
`ANS_ERR_REFERENCE` if the reference is unfrozen, invalid, or its recomputed
digest differs. The candidate under test never receives a mutable reference.
Atoms (10): AUTHORITY, CONSENT, SCOPE, PROVENANCE, REVERSIBILITY,
RESOURCE_LIMITS, INFORMATION_BOUNDARIES, GOAL_IDENTITY, DELEGATION_LIMITS,
SELF_MODIFICATION_LIMITS. Reference fields: `version` (non-zero), `limit[10]`
(max tolerated drift per atom, finite, > 0), `q[5]` (process noise per action
kind, > 0), `r[3]` (noise per sensor class, > 0, `r[FIX] < r[REFERENCE] <
r[IMU]`), `tier_sigma[3]` (0 < t1 < t2 < t3), `nis_limit` (> 0),
`promote_limit` (> 0). The digest covers all of these and not `frozen`.

## State A_t

`ans_state`: goal, constraints (== reference digest), intent, evidence_root,
drift (an `est_belief`, n = 4, components GOAL, INTENT, PROVENANCE, SCOPE,
dimensionless; x = estimated drift, P = covariance), risk (0..1,
reversibility risk of the pending action), provenance (hash chain over every
record that shaped the state), generation, since_fix, last reference digest
seen, last_nis, disagreement, self_mod_steps, self_mod_unfixed.
Filter model: n = m = 4, F = I, H = I, Q = q[kind] I per step, R = r[class] I
per measurement, `step_ns` = 1. The prior is x = 0, P = r[FIX] I.
Components map to atom limits: GOAL to GOAL_IDENTITY, INTENT to CONSENT,
PROVENANCE to PROVENANCE, SCOPE to SCOPE.

## Actions (the inertial step)

Kinds: OBSERVE, EFFECT, DELEGATE, INGEST_UNTRUSTED, SELF_MODIFY.
`ans_step` predicts one step with Q = q[kind] I, sets risk to the action's
reversibility risk, increments since_fix and generation, and chains the action
digest into provenance. Refused: non-finite input, zero action id, kind or
risk out of range, touches not 0/1, unfrozen or mismatched reference. An action
that touches SELF_MODIFICATION_LIMITS with reversibility risk above
`limit[SELF_MODIFICATION_LIMITS]` returns `ANS_ERR_ATOM`. The output state is
then the input with the refusal chained into provenance and generation
advanced and the drift unchanged: a record of the refusal, never a repair. A
SELF_MODIFY action, or one touching that atom, increments `self_mod_steps` and
`self_mod_unfixed`.

## Sensors (the fusion)

Classes: IMU (least trusted), REFERENCE, FIX (most trusted).
`ans_measure` runs `est_kf_predict` (one tick, Q = 1e-9 I) then `est_kf_update`
with R = r[class] I. A FIX sets since_fix to 0 and self_mod_unfixed to 0.
REFERENCE and FIX record last_nis; a reading with nis above `nis_limit` sets
`disagreement = 1` and is still fused: the disagreement is the signal and is
never hidden. The flag is sticky; only a FIX within the limit clears it.
Refused: zero evidence or source (`ANS_ERR_EVIDENCE`); IMU carrying a
reference digest, or REFERENCE/FIX whose reference digest is zero or not the
frozen one (`ANS_ERR_SENSOR`); non-finite values; an EST refusal
(`ANS_ERR_EST`, with the `est_status` in the out parameter), for example
evidence equal to a belief digest. The filter clock is the state's;
`t_ns` and `seq` are bound into the measurement digest.

## Drift and the control law

`ans_drift`: mean_norm = ||x||2, sigma = sqrt(trace P), D = sqrt(mean_norm^2 +
sigma^2). `ans_autonomy` returns an `ans_verdict` with tier FULL
(sigma < t1), REVERSIBLE_ONLY (t1 <= sigma < t2), SIMULATE_ONLY
(t2 <= sigma < t3), HALT_REQUEST_FIX (sigma >= t3). Any component whose |x|
exceeds its mapped atom limit raises the tier to at least REVERSIBLE_ONLY.
`disagreement == 1` forces HALT_REQUEST_FIX regardless of sigma. The verdict
carries D, mean_norm, sigma, since_fix, disagreement, the state digest, the
reference digest and its own digest. It is advisory.

## Promotion gate (RSI may not touch the compass)

`ans_promotion_check` takes a request (candidate digest, reference at birth,
tests at birth, tests now, candidate state) and writes a digested
`ans_promotion_record` whose `result` is, in this precedence order:
REFUSED_REFERENCE (reference at birth is not the frozen digest: the compass
changed during the candidate's life), REFUSED_TESTS (tests now differ from
tests at birth: the instrument changed in the same transaction),
REFUSED_DISAGREEMENT, REFUSED_DRIFT (D above `promote_limit`),
REFUSED_SELF_MOD (`self_mod_unfixed > 0`: self-modification without a FIX
after it), else OK. The return value is a status; a refusal is a successful
record. The record is data for the holder of the promotion right.

## Status codes

`ans_status`: OK, ERR_NULL, ERR_REFERENCE, ERR_NONFINITE, ERR_EVIDENCE,
ERR_SENSOR, ERR_ATOM, ERR_RANGE, ERR_EST, ERR_ENCODING. Promotion results are
the separate `ans_promo` enum.

## Encoding and digests

Digest = SHA-256(domain || 0x00 || encoding), domains `omega.ans.<record>.v1`
(reference, state, action, measurement, verdict, promotion). Fixed
little-endian layout, doubles as binary64 bits with -0.0 as +0.0, header
`OANS`, kind, version. Verdict and promotion record digests are computed with
their own digest field zeroed. Provenance chain: SHA-256(`omega.ans.provenance.v1`
|| 0x00 || prev || tag || record digest). Deterministic (`-ffp-contract=off
-fno-fast-math`), bounded, no allocation, no globals, no I/O.

## Tests (`tests/ans/test_ans.c`, plain and ASan/UBSan)

1. Freeze and digest determinism; any field change changes the digest; unfrozen refused.
2. Hostile: a mutated copy of the frozen reference with the old digest is refused by every API.
3. Drift grows: 50 EFFECT steps, sigma strictly increasing, tier non-decreasing, reaches HALT_REQUEST_FIX.
4. A FIX restores: sigma below t1, since_fix 0, tier FULL.
5. IMU alone cannot restore FULL from SIMULATE_ONLY (chosen r and q below).
6. Disagreement: a REFERENCE with nis above the limit forces HALT with small sigma.
7. Refusals: zero evidence, IMU with a reference digest, FIX with a wrong digest, non-finite z, action refusals.
8. Promotion: the OK path and each of the five refusals.
9. Encodings round trip; golden digests are checked.
10. Self-modification: promotion refused without a FIX, allowed after one.

Numbers used in the tests: q 0.001/0.01/0.02/0.03/0.05, r IMU 1.0, REFERENCE 0.1,
FIX 0.01, tier_sigma 0.25/0.5/1.0, nis_limit 13.28, promote_limit 0.6. With
alternating EFFECT and IMU the steady sigma is 0.617, inside the SIMULATE_ONLY band.

## Build

`make test-inertial-alignment` (purity plus unit suite); `make ans-purity`
(`nm -u` on each ans object against the estimation forbidden list and
`rx_gen_promote`); `make test-ans`. CI: `.github/workflows/host-suites.yml`.
