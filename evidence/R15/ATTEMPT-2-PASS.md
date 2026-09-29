# R15 qualification attempt 2: PASS (R15_REACTION_PERFORMANCE)

- Run: `raw/20260929T025735Z-3e9e53be3358-silicon/`, 2026-09-28 21:57–22:32 CDT, GB10 (DGX Spark), silicon mode, quiet machine (`.spark-quiet` set, AI services off).
- Candidate: `3e9e53be3358f6f2849a4f02044a1fd809240cbb`, clean tree, candidate-bound. aienos `c8ab65e` and physics `fecbedb`, both equal to their lock files.
- Criteria: spec `spec/r15-performance-proof.md` with amendment A1 (§11 note, §21: G10 counts synchronization events; empty polls reported as host spin cost; decided by Drake 2026-09-28, Option A). No other criterion changed.
- Result: 16 of 16 gates PASS; 0 failed processes; 0 new Xid (the only Xid in the kernel log is the known 19:10 practice-1 fault); reducer output reproduced byte for byte by a second reduction.
- G10: RES-1 3.0 vs SEQ 4.0 synchronization events per GPU result. Host spin (not gated): RES-1 102.9 vs SEQ 106.1 empty polls per result.
- Correctness reruns (§14) on the candidate before the run: R3, R7, R8, R9, R10, R11, R12 host+silicon, R13 host+silicon, R14 host+silicon, R15 SEQ parity host+silicon, all exit 0 (`reruns-3e9e53b.json`, `reruns-3e9e53b-log-summary.txt`).
- Receipt: `065c688408aa18950f363b52be2357c3cb853827592dc71ea72b6da181b4569a.json` (schema AIEN_RX_R15_REACTION_PERFORMANCE_V1, outcome PASS). Regressions where RES-1 is slower than SEQ and the stated limits are listed in it (from `receipt-notes.tsv`).
- Attempt 1 (`ATTEMPT-1-FAIL.md`) stays FAIL as recorded.
- Stated limit: G10 is measured with one GPU claim in flight; a multi-claim-in-flight phase (Option B) is deferred to a later gate.
