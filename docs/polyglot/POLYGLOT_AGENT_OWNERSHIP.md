# POLYGLOT agent ownership

Lead orchestrator: session ff5a6f (Opus 5.5). Branch `feat/polyglot-0` in `~/workspace/omega-polyglot`, based on
mixed-algebra eb7a788. All workers run on Opus 5.5 unless marked (Sonnet 5.5 = supervised trial). Fable reviews
the plan and the final result once each; it does not implement. No agent certifies its own work.

Shared interfaces (lead only): `spec/polyglot-0.md`, `src/polyglot/omx_lang.h`, the one `-include` line in
`Makefile`, this file. Workers never edit another lane's files; they ask the lead.

Everyone, read-only: `src/algebra/**`, `tests/algebra/**`, `spec/mixed-algebra*.md` (owned by the mixed-algebra
session 22f65f). Everyone, forbidden: any `.py` file, any Rust source or Cargo file, `src/aarch64_encoder.c`,
other lanes' files, GPU launches, killing any GPU test.

| lane | agent / model | objective | owned files | acceptance |
|---|---|---|---|---|
| A audit | worker A / Opus | verified current state for the polyglot program | `docs/polyglot/OMEGA_POLYGLOT_CURRENT_STATE.md` | every claim has repo + path + commit + test/evidence; lead spot-checks 5 |
| B1 hand assembly | worker B1 / Opus | Omega-X int8 SDOT and crumb kernels in hand AArch64 assembly | `src/polyglot/asm/**`, `src/polyglot/omx_asm.c`, `tests/polyglot/test_asm.c`, `mk/polyglot-asm.mk` | bit-exact on MA grid + 20k random + guard pages; derivation recorded |
| B2 own encoder | worker B2 / Opus (after B1) | emit B1's kernels as bytes with Omega's AArch64 encoder | `src/polyglot/omx_encoder*.{c,h}`, `tests/polyglot/test_encoder.c`, `mk/polyglot-encoder.mk` | byte diff vs GNU as report + bit-exact |
| B3 Mojo | worker B3 / Opus | Omega-X int8 and crumb in Mojo 1.0 (CPU), C ABI or standalone bench on same fixtures | `polyglot/mojo/**`, `src/polyglot/omx_mojo.c`, `mk/polyglot-mojo.mk` | bit-exact or a recorded negative result; no Python touched |
| F verify + bench + evidence | worker F / Opus | shared verifier, benchmark harness, receipts, explainer | `src/polyglot/omx_lang.c`, `src/polyglot/omx_bench*.{c,h}`, `tests/polyglot/verify_polyglot.c`, `tests/polyglot/bench_polyglot.c`, `tests/polyglot/polyglot_explain.c`, `mk/polyglot.mk`, `evidence/POLYGLOT/*.json` | spec sections 7-10 |
| H Rust-vs-C history | worker H / Sonnet (trial) | black-box measurement of aienos capability authority: Rust crate vs C port | `docs/polyglot/evidence-rust-vs-c-capability.md`, `evidence/POLYGLOT/historical/**` | numbers re-derivable; lead re-checks a sample |
| G adversarial review | worker G / Opus (after B1 + F) | break the contract, verifier and bench method | `docs/polyglot/review-polyglot-0.md` | findings ranked; blocking ones closed by owners |
| C math representations | mixed-algebra session 22f65f | external lane, not duplicated | its own files | its own gates |
| D Blackwell | mixed-algebra MA-4 | external lane; polyglot starts no GPU work | none here | n/a |
| E analog / phase | ADR 0018 lane | parked on Drake's AR0 decision | none here | n/a |
| Ledger | lead | `docs/polyglot/OMEGA_LANGUAGE_EVIDENCE_LEDGER.md` from receipts only | that file | entries cite receipts |

Timed benchmarks: one at a time on the Spark, check `~/workspace/.spark-quiet`, message the mixed-algebra
session first. Only lane F runs timed benchmarks; other lanes run correctness tests only.
