# E1 C3 REDUCE LDST A/B, round 1 record

Verdict, in the operator's words: **bug not reproduced / comparison inconclusive (round 1 complete, rounds 2-5 not started)**.

Script verdict line (log line 30): `E1 C3 REDUCE LDST AB: RUN_INCOMPLETE`, reason on log line 29: `incomplete because: quiet flag appeared before fix round 2`.

Every number below is copied from the summary log. Line numbers refer to `~/workspace/test-queue-logs/E1N-C3AB-101051.log` (30 lines).

## What was compared
Two builds of the same chip harness (`tests/test_omega_c3_ab_gb10.c`), run interleaved on the GB10 chip:
- control arm: built with `-DOMEGA_C3_PROTECT_OFF`, which compiles out only the new reduce and load/store protection (GPU L2 flush plus a second release marker, step `marker2_wait`).
- fix arm: the same sources with the protection compiled in.

Built from omega `617971aa09c93649908e84e06cb681044d5bde41` (log line 1) and physics `e95e3ed2a86fe4bffe4d954fa94c27dfb5284280` (line 2). The PR head is now `44741c1b`, which adds a later diagnostics-only commit (marker2 late-arrival probe, stderr evidence) that touches `src/omega_numeric_reduce_gb10.c`; the chip run did not include it.
Control binary sha256 `93ee1659a96ffed5e87228d7d63b32357a8efe068e52f207ec10ea00b2cc0972` (line 7). Fix binary sha256 `6c64ce80bceb7cdae0c26623ec756bdf7d050341db1398a51d0665fd0d024448` (line 8).

## Settings (line 6)
`rounds=5 repeats=200 sizes=1000003,65 ops=SUM,MAX,MEAN,LDST`. Expected runs per arm if the whole plan had run: 4000 (line 28). Round 1 is 800 runs per arm (lines 14 and 21), so 1 of 5 planned rounds ran.

## Results, round 1 (lines 9, 10, 14-27)
Qualifying-hit threshold fixed before the run: at least 10 unwritten-output hits in the control arm. A hit means the output was left unwritten (OMEGA_UNWRITTEN_TRAP). A device error (the chip call itself failed) is a refusal and is NOT a hit.

| Arm | Runs | Hits | Device errors |
|---|---|---|---|
| control (protection off) | 800 | 1 | 11 |
| fix (protection on) | 800 | 0 | 6 |

Per operation:

| Op | Control hits | Control device errors | Fix hits | Fix device errors |
|---|---|---|---|---|
| SUM | 0 | 5 | 0 | 1 |
| MAX | 0 | 3 | 0 | 3 |
| MEAN | 0 | 3 | 0 | 2 |
| LDST | 1 | 0 | 0 | 0 |
| total | 1 | 11 | 0 | 6 |

Launcher failure lines by step (lines 20 and 27):
- control: 11 x `GB10_DEVFAIL fn=run_chunk step=marker_wait drv_rc=-1`
- fix: 6 x `GB10_DEVFAIL fn=run_chunk step=marker2_wait drv_rc=-1`

The control arm has no `marker2_wait` step (its protection is compiled out), so the two arms fail at different waits by construction. This record draws no conclusion from the difference.

## How to read it
- Control hits = 1, threshold = 10. The control arm did not reproduce the unwritten-output bug at the required rate, so the A/B cannot say anything about whether the protection fixes it. This is the "Known limits" case in the PR body.
- Device errors are NOT counted as hits, in either arm. They are listed separately above.
- The fix arm's 0 hits in 800 runs is NOT a proof of a fix. At the control arm's observed rate (1 hit in 800 runs) a build with no effect would be expected to show about 1 hit in 800 runs, so 0 of 800 does not separate the two arms.
- The 11 control and 6 fix device errors are a separate open question (failed device calls, `drv_rc=-1`). They were not diagnosed here.
- The threshold was not changed after seeing the data.

## Why it stopped
Drake's rule (2026-10-02): finish round 1 untouched, stop at the round boundary only if the control arm ends below 10 qualifying hits.
Stop-watcher log `~/workspace/test-queue-logs/e1-c3ab-stop-after-round1.log` (7 lines):
- line 3, 12:10:50Z: control round 1 finished, `OMEGA_UNWRITTEN_TRAP: 1/800 device_errors=11`, qualifying hits 1, threshold 10.
- line 4, 12:10:50Z: control below threshold, stop at the round boundary.
- line 5, 12:10:50Z: waiting for fix round 1 to reach its last op (LDST); the next stop-watcher entry is 14:08:25Z.
- line 6, 14:08:25Z: quiet flag raised (the stop mechanism). Line 7, 14:09:55Z: the script exited and the quiet flag dropped. The flag was up for 90 seconds.
- The script then printed `RUN_INCOMPLETE` because the flag appeared before fix round 2 (log line 29). Round 1 had finished for both arms (log lines 9 and 10); rounds 2-5 were never started.

## Evidence paths
- Summary log: `~/workspace/test-queue-logs/E1N-C3AB-101051.log`, sha256 `80801f6db509a530f76c60fdeac24223e7f103df70f82ebae5697ad068471264`.
- Stop-watcher log: `~/workspace/test-queue-logs/e1-c3ab-stop-after-round1.log`, sha256 `f12a90e03860b5581ea80f0b0336607e42aa839ba57adb2fb44e1b217cd8d2f6`.
- Transcript (outside the repo): `~/workspace/evidence-out/E1-C3-AB/ee2571feacd98b5252653c80dc65287d53ff1386145d525b563435896526f3a1.log`, sha256 `ee2571feacd98b5252653c80dc65287d53ff1386145d525b563435896526f3a1` (log lines 12 and 13).
- Script: `~/workspace/scripts/lt-e1-c3-ab.sh` (outside the repo).

## Status
This is the chip comparison, not the chip gate. The PR title keeps `[host NOT_RUN][chip NOT_RUN]`. No claim is made that the protection fixes the rc=-4 device errors. Rounds 2-5 remain not run.
