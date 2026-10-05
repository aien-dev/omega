# CAND-2: declared attempt A7b (written after A7's harness defect, before A7b runs)

A7 (R15 silicon, `DECLARED-ATTEMPTS.md` sha256 8a6e06da...85cf) did not start: `tools/r15_qualify.sh` found no
`build/rx_r15_perf_silicon` because the chipwait lane ran `make clean` and the r15 lane of `cand2_ladder.sh` did not
rebuild. A7 stays recorded as NOT_RUN (harness defect). It is not replaced. A7b is a new attempt, declared here.

| # | Attempt | Command | Pass rule (fixed now) |
|---|---|---|---|
| A7b | R15 silicon on CAND-2 | `cand2_r15b.sh` (same guard and step recorder as `cand2_ladder.sh`): `make r15-perf-silicon`, then `tools/r15_qualify.sh silicon`, once | R15 PASS per its receipt. Expected in advance: BLOCKED_INSTRUMENT / INSTRUMENT_UNAVAILABLE, because the SPBM energy module is not loaded (operator action); that is recorded as such, never as performance and never as a pass. If the instrument is present and the run proceeds, the SEQ-06 residency requirement (99 %) is not lowered, no trial is discarded, no best-of-runs is taken. |

Runs once, under its own `quietlock hold` (owner cand3-campaign, at most 20 minutes, no approval token needed),
only after the machine is free (no other session's quiet flag); never kills anything. Evidence:
`~/workspace/evidence-out/CAND2-LIVING-79a805d/A7b-R15/`.
