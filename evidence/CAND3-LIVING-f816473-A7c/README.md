# CAND-3 A7c: R15 silicon, and the R16 G7/G8 attempt (2026-10-06)

Declared before running in `00-declared/DECLARED-ATTEMPT-A7c.md` (sha256 70824bb8b6c116a72a047c61e424169338b7dc8eb0ded9c8b65260e73ecdeec9; script cand3_r15c.sh sha256 cdbff59d5ac40acf90da47ada9f4e7d985957db4a6d1a3828f98fdbe14013005). Original bytes, additive; CAND-2, W1, W2 and EST evidence are untouched. One binary (`json_canon`, ELF, in R16-G7/raw) is not copied.

| Attempt | Verdict | Evidence |
|---|---|---|
| R15 silicon (A7c) | **PASS**: receipt outcome PASS, all 16 gates G1-G16 PASS (G2 1.066, G5 p50 7392 ns / p99 9952 ns, G9 energy RES-1/SEQ 0.880, G10 3.0 vs 4.0 sync events, G15 residency 1.0), 0 failed processes, 0 Xid, candidate-bound to 97ee275, clean tree, silicon observed, 13 correctness reruns PASS | R15-receipt-B/90fdebb8...fdd.json (with notes); R15-receipt-A/1b525c90...6224.json (same raw, no notes); R15-raw/20261006T113938Z-97ee27584cda-silicon/ |
| R16 G7 | **NOT_RUN**: the R15 acceptance part read PASS, but ladder rung R11 was NOT_RUN ("SKIPPED-LOADED: 1-minute load average 2.11 is above 2": the living part refuses above load 2). All other rungs PASS. Not rerun (rule: each attempt runs once; a replacement is a new declaration) | R16-G7/stdout.log, R16-G7/raw/*/r11_aien.log |
| R16 G8 | **NOT_RUN**: needs the PR merged with a merge commit; no merge exists. Script reports receipt preconditions met | R16-G7/stdout.log line "R16-G8" |
| R16 G1-G6 (re-observed in this run) | all PASS | R16-G7/raw/receipts/52e43195...f32.json (overall NOT_RUN) |

## Stated deviations and limits
- **Secure Boot was OFF** (Drake's decision), a deviation from R15 spec clarification C2. The SPBM reader loaded with "module verification failed: signature and/or required key missing - tainting kernel" then "firmware contract verified; read-only SPBM telemetry ready" (00-reader/reader-facts.txt). Reader: /sys/class/hwmon/hwmon3, module sha256 2d37f51a...dd8b0, srcversion D7345BB5C0CCFCB7B177335. It was not unloaded.
- Other users' boot-time services were running (atlas image server holding 18408 MiB GPU memory idle, caption server, MAX Llama serve, two auto-restarting services). Not stopped, not corrected; see processes-before/after in the raw run. The R11 skip above is attributed to load by its own log; its cause (which process) is UNVERIFIED.
- Receipt-B does not itemize spec section 11 regressions (stated in its notes). The reduce.log line "17 of 16 gates pass" is a script counting artifact; summary.json has 16 gates all PASS.
- CAND-3 is not qualified overall from this result alone: R16 remains NOT_RUN until a G7 attempt with a loaded-machine-free R11 and the G8 merge step.
