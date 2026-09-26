# Ternary Semantics Experiment: PARKED

```text
Status:   PARKED 2026-09-26 at the owner's request; next steps to be decided later.
Scope:    User-approved exploration (2026-09-26). Additive only.
Doctrine: No change. The binary profile (OMG0) stays canonical.
```

## What exists on this branch

Everything is additive. It adds new files only, plus a separate `ternary` Makefile target. `omegatool`, the binary encoder, the verifier, the synthesizer, and the M10 library are untouched.

| Work item | File | State |
|---|---|---|
| 1. Ternary types/ops and the brute-force trit model | `src/omega_ternary.{h,c}` | Done. T32 word, 11 ops, symmetric saturation, OMG1 encoding (tryte-packed, 5 trits per byte) |
| 3. Lowering-aware cost model | `src/omega_ternary_a64.c` (`*_dyn`) | Done. Exact dynamic instruction-count prediction |
| 4. PHYSICS AArch64 lowering | `src/omega_ternary_a64.{h,c}` | Done. INT and PLANES realizations, conversions, own encoders |
| Measurement | `src/omega_ternary_measure.{h,c}` | Done. ptrace single-step instruction counts, ns/call |
| 6. AEGIS ternary invariants (T-V0..T-V2) | `src/omega_ternary_verify.{h,c}` | Written, builds, not yet gated |
| 2. Synthesis bank and search, plus the binary control | `src/omega_ternary_synth.{h,c}` | Written, builds, not yet run or gated |
| 5. Blackwell lowering (static counts and register pressure) | `src/omega_ternary_sass.{h,c}` | Written, builds, not yet run; nothing executed on GB10 |
| 7. Gates | `tools/omega_ternary_tool.c`, `tests/ternary/` | TG1-TG5 pass (below); TG6+ and `tests/run_ternary_gates.sh` are not written |

## Gate results at park time (DGX Spark, `make ternary && ./build/omega_ternary --run-gates`)

- TG1: fast model == brute-force trit model: 3,978,394 checks, 0 mismatches.
- TG2: OMG1 identity. PLANES-realized and INT-realized values get the same id. OMG1 ids differ from OMG0 ids. Non-canonical trytes and padding are refused. IDs are equal exactly when values are equal.
- TG3: the 41 extension encodings match the GNU `as` oracle fixture (`tests/ternary/regen_a64_oracle.sh`, research-only).
- TG4: every op in both realizations, run natively, matches the model: 95 programs, 336,000 executions.
- TG5: the cost model matches the ptrace-measured dynamic count on 360/360 traced runs.

## Open points for the decision

- **Baseline.** The owner pointed out (2026-09-26) that the realistic binary baseline is the stack's compiled Rust (rustc -O / LLVM), not OMEGA's unoptimized canonical synthesizer. If this resumes, the comparison should be ternary against the best binary result, compiled Rust included.
- **Binary V2 finding.** `omega_verify_v2_properties` does not look at the realization, so binary `omega_program_verify` (V0+V2) accepts any structurally valid code. This is recorded here only; the binary path was not changed.
- **Blackwell.** Nothing runs on GB10 yet. That needs oracle-checked encodings for LOP3 (any LUT), SEL, ISETP.EX, SHF.L and IMAD.WIDE.
- **Separate track.** AIEN-T0 (ternary weights) vs AIEN-FP4 is a separate model experiment and is not part of this branch.
