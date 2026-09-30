# EST-0/EST-1 hostile review (omega-est, 7dbf33e)
Scratch programs: <scratch>/a1.c a2.c a3.c a4.c, mutants in tmp/mut/. Nothing in the repo was edited.
Build for all: gcc -std=c11 -O2 -Isrc -Isrc/estimation -ffp-contract=off <prog> src/estimation/*.c src/sha256.c -lm

## Counts: BLOCKER 0, MAJOR 3, MINOR 6, NIT 2

## MAJOR

### M1. est_kf_update trusts the prediction's contents: a forged prediction with a valid prior digest is accepted
File: src/estimation/est_kf.c:167-181 (bind_checks), 190-235 (update). est_check_prediction (est_types.c:167) checks only finiteness/PSD.
Nothing verifies the prediction was computed from prior+model: x, P, t_ns, horizon, y_mean, S are free.
Evidence (a1.c): forged pred x=42, P=1e-30, t_ns=-77 (before prior t=5000), y_mean=-1e9, S=7, horizon=999:
  "forged pred update: 0 x=42 P=1e-30 t=-77"   (0 = EST_OK; posterior time goes backwards)
Also a4.c: horizon=7 with t advanced by 1 step -> update returns 0.
Impact: the prior digest / generation "staleness" checks only bind identity, not derivation; an attacker or buggy caller can inject an overconfident state that still carries a valid parent chain and evidence root.
Fix: in update/coast, recompute the prediction (est_kf_predict with pred->horizon, same Bu) and require a bit-exact digest match; at minimum check pred->t_ns == prior->t_ns + horizon*step_ns, pred->horizon>=1, and y_mean/S consistency. Add tests for each tampered field.

### M2. Known input Bu is not recorded anywhere, so predictions with control input cannot be replayed or verified
File: est_kf.c:117-130; est_types.h est_prediction has no Bu field.
Evidence (a4.c): predict with Bu=5 gives x=5, digest differs from Bu=0, but prior/model/horizon fields are identical: "any field records Bu? ... equal: 1".
Impact: ADR 0020 section 7 (replayable evidence) and M1's fix are impossible for any prediction with input. The prediction digest also cannot be re-derived from its declared inputs.
Fix: add Bu (n doubles + a has_input flag) to est_prediction and its encoding, or refuse Bu in EST-1.

### M3. A prediction can be passed off as an observation (ADR 2.1 "prediction != observation")
File: est_types.c:158-166 (est_check_observation only requires non-zero evidence digest); est_kf.c:190.
Evidence (a1.c): observation built with z=p.y_mean, R=p.S, evidence = prediction digest, source = zero:
  "prediction-as-observation update: 0 nis=0 P=50.2493 (P prior 100.001)"   (covariance halved from nothing; double counting)
Also observation source zero is accepted ("obs zero source check: 0"), so the observation names no producer.
Impact: the layer's core anti-conflation rule holds only by C type, not by data. A serialized/decoded observation whose evidence is a model-derived digest is indistinguishable from real evidence.
Fix: require non-zero source; add an evidence-kind/domain (e.g. evidence must be an est_digest over a raw-evidence domain that est_* digests can never produce) and in update refuse obs->evidence equal to pdig, digest(prior), prior->evidence_root, or any digest the estimator produced in this call. State the limit (stateless module cannot know all).

## MINOR

### m1. Covariance PSD/negative-variance tolerance is relative to the LARGEST diagonal, so tiny variances can be negative
File: est_types.c:76-88 (tol = 1e-12*maxd).
Evidence (a1.c): diag(1e12,-0.5) -> est_check_covariance returns 0 (accepted). diag(1e12,-100) rejected (-5).
Also [[1e6,.5],[.5,0]] (det<0, indefinite) accepted; [[0,1e-170],[1e-170,0]] indefinite accepted (c*c underflows to 0, est_types.c:83-84).
Filter-produced covariances did survive: collapse (2e5 steps, Q=0,R=1e-12), scale disparity diag(1e12,1e-6), 1e200 and 1e-200 scales all kept min diag >0 (a2.c). So this is a validator hole, not shown filter breakage.
Fix: reject any negative diagonal entry outright (diag < 0), compare c^2 to d*e without underflow (use fabs(c) > sqrt-safe or scaled compare), or scale to unit diagonal before LDL.

### m2. Belief/prediction structural invariants are not enforced by est_check_belief
File: est_types.c:150-157. A belief with generation=99, zero parent, zero evidence_root passes (a3.c "forged gen99 ...: 0"), though the header says root is zero only for a declared prior. Fix: require (generation==0) iff parent zero; nonzero model digest.

### m3. Model with R=0 is accepted and can permanently wedge update
File: est_check_model; a2.c "R0-P1e-300 step 1 update err -6" (NOT_PD every later update; coast still works). Refusal is loud, not silent. Fix: reject R with zero diagonal in the model, or document.

### m4. Unbounded predict horizon (uint32) is a CPU denial of service
est_kf.c:120. a2.c: horizon 1e8 took 0.84s; 4.29e9 is ~35s. Fix: cap horizon (e.g. 1<<20) or use matrix powers.

### m5. Asymmetry is accepted (1e-9 relative) and stored/digested as given
Evidence (a1.c): [[1e12,1e3],[0,1]] accepted (asym 1e3 absolute on the small entry scale). Digests then depend on the asymmetry. Fix: canonicalize (symmetrize) in est_kf_prior, or tolerance relative to sqrt(Aii*Ajj).

### m6. Test gaps found by mutation (tmp/mut/, each mutant compiled with the real tests)
- Prediction digest domain set equal to the belief domain: test_est_types PASSES (0 failed). Domain separation is untested; only the kind byte separates them.
- est_kf_coast resetting evidence_root to zero (est_kf.c:~331): test_est_kf PASSES. The coast test compares against a prior whose root is already zero (test_est_kf.c:239,356). Evidence laundering by coast is undetected.
- Removing the pred->model != prior->model check (est_kf.c:~177): PASSES; never exercised (implementation is right, a4.c returns -9).
- Removing symmetrize(S) in predict: PASSES.
- test_est_kf.c:506 `b.P[0] < 1e-6 || b.P[0] < 1.0` is just `< 1.0`.
- No test tampers pred.t_ns, horizon, x, P, y_mean (see M1).

## NIT
- n1. mk/estimation.mk:18 EST_FORBIDDEN denylist misses malloc, getenv, time/clock_gettime, rand, read/write/printf, openat. An allowlist (memcpy, memset, sha256_*, est_*, sqrt, __*chk, stack protector) is stronger. Currently nm -u is clean (est-purity PASS).
- n2. est_types.c w_f64/r_f64 memcpy double<->u64 assumes IEEE-754 and matching int/float endianness (true on this target); add a static check or comment. -0.0 canonicalization means decode(encode(x)) is not bit-exact for -0.0 (documented).

## Tried and HELD UP
- Joseph form is used (est_kf.c:250-260); replacing it with (I-KH)P' makes 3 tests fail. Posterior symmetrized; dropping it fails tests.
- NaN/Inf anywhere refused (belief, obs, Bu, model); NaN payload in digest refused (-3).
- Covariance collapse: 2e5 steps Q=0 R=1e-12, 1e5 steps with 1e12/1e-6 scale disparity and 1e200/1e-200 scales: no invalid P, no error.
- Near-singular S (P=1e-300,R=0): loud NOT_PD, outputs untouched on refusal.
- Bounds: n,m in {8x8, 8x1, 1x8, 3x5, 1x1} full prior/predict/update/coast/encode/decode under ASan+UBSan (my own program) with 0xAB-filled model padding: no OOB, no UB. n=0, m=0, n=9 refused (-2). Model encoding max 2144 bytes < 2304.
- Decode: belief bytes decoded as prediction/observation/model/innovation all -10 (KIND); trailing byte -> -11; all 0..len-1 truncations refused; failed decode leaves output untouched; 1279/1440 single-bit flips are valid different records (expected) and header/kind/version flips refused. Trailing-bytes mutant fails 200 tests.
- Determinism: encoders write field by field (no struct memcpy, no padding bytes in digests); garbage beyond n/m does not change digests; -0.0 and +0.0 digest equal; no globals or static state (only const zero byte).
- Staleness: swapped prior belief, old prediction vs newer belief, wrong generation with matching digest, wrong model, wrong obs time, units, dims all refused with the right code; the generation and prior-digest mutants are caught.
- Digest domains: distinct strings + 0x00 + distinct kind bytes; root domain "omega.est.belief.v1-root" cannot collide with belief domain.
- Aliasing: outputs built in locals and copied only on success.
- ADR 2.3: no authority, World, file, process or memory-mapping references (nm -u shows only memcpy/memset/strlen/sha256_*/est_*/stack-protector).
