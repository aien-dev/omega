# PD-0 verifier receipts (Direction 3)

Produced by `make test-physics0-verify` (verifier commit in each file). One receipt per verdict, append-only, never overwritten (the writer refuses an existing name).
Recorder and range check are STAND_IN. Truth generators used to build the evidence bundles live in `tests/physics0/verify/truth.h` (test only; `p0v-purity` proves no verifier object references them).

Calibration finding (threshold untouched, reported for the spec owner): at L6, seeds 1 to 3, the harness-fitted latent-free reference (greedy sparse least squares, degree <= 3, size budget per spec 6.2) reaches in-box 20-step NRMSE 0.018 to 0.047 against a 0.03 bound, so the spec 6.3 L6 condition "reference fails by a factor of 3" (needs >= 0.09) is not met even by the oracle. See `score-oracle-*` receipts with `ref_nrmse_micro`. The hidden variable relaxes with time constant 1/w in [0.5, 1.0], so it is nearly a function of s0 and a latent-free polynomial absorbs it. Spec defect candidate: V2/6.3 need either a slower hidden variable (smaller w) or a different reference condition.
