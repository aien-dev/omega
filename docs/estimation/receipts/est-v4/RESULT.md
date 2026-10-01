# ESTIMATION v4: result

**Verdict: INCONCLUSIVE.** All three collection attempts the protocol allows were
voided for foreign machine load in the measurement windows. No attempt counts, so v4
gives no calibration verdict. EST-3 stays FAILED (v1, v2, v3), and EST-4 and EST-5
stay blocked.

Protocol: `docs/estimation/protocols/est-v4.md`, sha256
`7baff66f1baf13f648b38ea04516b423e34e4c4b4d7528701f15272ca441f116`, frozen at `1a147e4`,
unchanged. No threshold was loosened and no void data was refitted or rescored.

## Attempts

| Attempt | Seeds D1 / D2 | Phase A | Sealed run | Why void | Record |
|---|---|---|---|---|---|
| 1 | 0xE5C4D1 / 0xD2E5C4 | PASS (G1) | PASS, G1 cov95 0.9476, ten-step 0.9321 | Lane 26 multi-core host build/test suites and a QEMU run overlapped both windows (coordinator report); Lane 29 host tests at ~10:05Z also fell in its D2 window (09:35:59-10:20:59Z) | `void-attempt1/` |
| 2 | 0xE5C4D2 / 0xD2E5C5 | PASS (G1) | PASS, G1 cov95 0.9570, ten-step 0.9534 | Lane 29 ~2-min build (11:36-11:38Z) in the D2 window; the watcher shows foreign builds and single-core tests in both windows | `void-attempt2/` |
| 3 | 0xE5C4D3 / not collected | not run | NOT_RUN | Another lane's `make test-compiler` ran 14:23:18-14:24:00Z, 4 s after the flag was taken; fails the standard fixed before attempt 3 | `void-attempt3/` |

Each void attempt was voided on grounds of machine load alone, recorded before
attempt 3, and attempts 1 and 2 had passed, so the voiding never moved a result toward
PASS. The void PASS numbers are not evidence of calibration and are not the verdict.

## Per-family record (void attempts, for the record only)

| Family | Attempt 1 Phase A | Attempt 2 Phase A | Attempt 2 one-step log score (D1) | Selected |
|---|---|---|---|---|
| G1 lag | PASS | PASS | -1.95967 | yes (both) |
| G2 AR change | PASS | PASS | -1.96012 | no |
| G3 two-tank | PASS | PASS | -1.97353 | no (lower one-step log score; attempt 2 tau and q on grid edges, flagged only) |

## Single most informative finding for a v5

The blocker is measurement isolation, not the model. On both voided attempts every v4
family passed the D1 screen and the selected G1 passed every held-out rule, which v1-v3
never did, but a courtesy quiet flag did not keep other lanes' host builds and tests
out of a 45-minute window on a shared machine. A v5 should keep the v4 families and
rules unchanged and change only the collection: a window that the machine enforces
(host builds and tests refuse to start while the flag is held, or an operator-reserved
slot with the other lanes stopped), with the watcher's "no foreign build or test"
rule written into the frozen protocol instead of added after the freeze. This finding
comes from the contamination record, not from tuning on any score.

## Downstream

EST-4 (world belief state) and EST-5 (uncertainty-aware cost model) stay **blocked**:
protocol section 10 unblocks them only on a v4 PASS, and after an INCONCLUSIVE nothing downstream may treat any estimator output as calibrated.
