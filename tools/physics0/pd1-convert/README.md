# pd1-convert: retrospective GB10 campaign evidence -> label-free PD1REC1 stream

Part of Physics-0 Direction 7 (PD-1). Spec: aien-dev/physics `docs/PD1_MACHINE_LAW_BENCHMARK.md`.
Harness-side tool. It is NOT learner code and must never be built into the learner lane: `extract.sh`
holds the join tables that turn structural labels (arm, directory prefix, variant) into knob values,
which is the PD-1 answer key for knob `q0`.

- `extract.sh` reads the preserved evidence under `~/workspace/evidence-out` read-only and prints a TSV.
  No row is synthesised; unrecorded fields carry sentinels (255, 4294967295, INT64_MIN, -1).
- `pd1_convert.c` writes the fixed-width `PD1REC1` stream (112 bytes per record, little-endian, SHA-256
  chain, refuses to overwrite) and a projection TSV with opaque column names; `--verify` recomputes the chain.
- `leakcheck.sh` fails if the projection carries any forbidden token from the label-stripping checklist.
- `make run` does extract -> convert -> verify -> leak check and prints per-source row counts.

Run of 2026-10-04 on the evidence listed in the spec's sufficiency audit: 2330 records
(2238 per-launch latencies, 40 + 47 per-run gate outcomes, 5 single-trial variant rows), chain verified,
leak check clean. Receipt: `evidence/physics0/pd1/pd1-convert-receipt-2026-10-04.txt`.
