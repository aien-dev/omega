# physics0: PD-0 hidden-equation benchmark substrate

Spec: aien-dev/physics `docs/PD0_HIDDEN_EQUATION_BENCHMARK.md` and `docs/PHYSICS0_DISCOVERY_ENGINE.md` at `5bd2b04` (revision 3). Program note: mind map `2026-10-03-physics-0-discovery-engine`.

This directory implements **Direction 2 only** (the substrate). Other lanes own the ladder checker, scorer, negative-control harness and learner (`src/physics0/ladder`, `src/physics0/score`, `src/physics0/controls` are reserved for them and do not exist here).

| File | Role | Side |
| --- | --- | --- |
| `src/physics0/pd0_wire.[ch]` | describe record, `PD0REC1` with SHA-256 chain, request bytes | both |
| `src/physics0/pd0_rng.[ch]` | SplitMix64 streams, uniform, constant draw, Irwin-Hall noise, `pd0_mul` | both |
| `src/physics0/pd0_relation.[ch]` | relations in `PDLAW1` delta form, rollout, NRMSE, size, `description_bits`, `PDLAW1` encode/decode/JSON | both |
| `src/physics0/pd0_chan.c` | learner-side channel: pipe or in-process, no file I/O | learner |
| `src/physics0/pd0_gen.[ch]` | **the only place the equations live** (L0 to L6, NULL world), true relation per instance | world / harness |
| `src/physics0/pd0_guard.[ch]` | STAND_IN range guard (AEGIS stand-in) | world |
| `src/physics0/pd0_world.[ch]` | describe/reset/step, budgets, STAND_IN recorder, framed pipe server | world |
| `src/physics0/pd0_world_main.c` | `pd0-world <level|null> <seed>`: the world as its own process | world |
| `tests/physics0/test_pd0_world.c` | G0 self-test + section 2.3 rules + SplitMix64 check + process transport | gate |
| `tests/physics0/pd0_calib.[ch]`, `pd0_sparse.[ch]`, `pd0_oracle.c` | test-only oracle solver, reference sparse solver, V1/V3 calibration, G1 receipts | gate |
| `tests/physics0/test_pd0_osc.c`, `osc/pd0_helpers.osc` | Omega-language helpers + interp/native/C differential | gate |
| `tests/physics0/isolation.sh` | G5: learner-side objects reference no generator, world or file symbol | gate |
| `mk/physics0.mk` | `make physics0-test`, `make physics0-evidence`, `make pd0-world` | build |
| `docs/physics0/CALIBRATION.md` | section 11 step 3 numbers and spec findings | doc |
| `docs/physics0/OMEGA-MIGRATION.md` | what is Omega now, what moves when | doc |
| `evidence/physics0/` | `pd0-g0-receipt.txt`, `pd0-oracle-L<n>.json` | receipts |

Gate lines printed by `make physics0-test`: `PHYSICS0_G0`, `PHYSICS0_ISOLATION`, `PHYSICS0_G1_V1_L<n>`, `PHYSICS0_G1_V3_L<n>`, `PHYSICS0_ORACLE`, `PHYSICS0_MUTANT_SIGN`, `PHYSICS0_OSC_DIFF`. V3 lines may say FAIL without failing the make target: a breach rate above 5 % is a spec finding recorded in the receipt (CALIBRATION.md section 3), not a code defect.

Not claimed: no learner exists, no ladder ran, `PD0_CONTROLS_PASS` was not produced, nothing here is a hardware result. Recorder and range guard are labelled `STAND_IN` in every receipt.
