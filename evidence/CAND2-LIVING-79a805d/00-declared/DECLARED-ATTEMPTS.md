# CAND-2 qualification window: declared attempts (written before any outcome)

Declared: 2026-10-05, before the window starts. Its sha256 is posted to the mindmap with the
quiet-flag announcement, so it cannot be changed after the results are known.

Candidate: CAND-2 (aien-architecture `qualification/candidates/CAND-2.toml`). Omega code
`79a805d162bfded8c5ce5a4c14f7c29e79025f39`; the harness runs from the R16 map-only
descendant (omega #301), which the ladder guard checks is code-identical. Physics `6d7cf0d`.
Script: `cand2_ladder.sh` (copied beside the evidence). Evidence directory:
`~/workspace/evidence-out/CAND2-LIVING-79a805d/`.

## Rules for every attempt

1. Each attempt below runs **once**. No result is replaced by a rerun. If an attempt cannot
   start (refused, instrument absent, build failure), that is its result and is recorded as such.
2. Every attempt's directory is kept whole: command, stdout, stderr, exit code, seconds, and
   machine snapshots before and after (GPU processes, top CPU, Xid count).
3. No chip test is killed or wrapped in a timeout.
4. "Instrument refused" (BLOCKED_INSTRUMENT / INSTRUMENT_UNAVAILABLE) is never reported as poor
   performance, and never as a pass.
5. Receipts are copied under their sha256 name. A claim index lists each claim with the receipt
   digest that supports it. The verdict is reconstructed from the receipts by a separate reader
   before any status page changes.
6. The window runs under `quietlock hold` (owner `cand3-campaign`), announced on the mindmap. It
   never stops another session's run: if the machine is busy, the window waits.

## Attempts, in order

| # | Attempt | Command | Pass rule (fixed now) |
|---|---|---|---|
| A1 | R16 qualification ladder (G1-G7, host + silicon; stops later silicon after one silicon FAIL) | `cand2_ladder.sh <harness> ladder` step R16-ladder (`tools/r16_qualify.sh`) | R16 PASS only if every gate PASS in its receipt. Expected and accepted in advance: G6 reads MISSING_IMPLEMENTATION (operator emergency stop is not wired into the production program) and G7 cannot pass without a passing R15. Both are recorded as they read. |
| A1b | R11 living under load (AIEN faculty driven through the native AIENOS authority, live) | `cand2_ladder.sh <harness> r11` (builds `build/rx_r11_aien_test`, waits 60 s, runs it once) | exit 0, last line `checks N failures 0`, and the living run exercised: output that contains "living run not exercised" (another quiet flag, an R15 program alive, or 1-minute load above 2) is a refusal, recorded as NOT_RUN, never a pass |
| A2 | R13 test-build on silicon | same ladder, step R13-testbuild-silicon | exit 0 and its receipt PASS |
| A3 | Production hygiene on silicon | same ladder, step prod-hygiene-silicon | exit 0 |
| A4 | COMPOSITION-2 on the GPU | same ladder, step COMPOSITION-2-GPU | exit 0 and receipt PASS |
| A5 | M19 endurance (CHIPWAIT campaign) | `cand2_ladder.sh <harness> chipwait` (3 runs of `tools/m19r_qualify.sh` full mode) | the predeclared 3/3 rule of `tools/m19r_campaign.sh`: all three runs PASS, including the long-soak criterion |
| A6 | M18 matmul gates | `cand2_ladder.sh <harness> m18` | exit 0 and gates PASS |
| A7 | R15 silicon | `cand2_ladder.sh <harness> r15` (`tools/r15_qualify.sh silicon`) | R15 PASS per its receipt. Expected in advance: BLOCKED_INSTRUMENT, because the SPBM energy module is not loaded (operator action). The residency requirement (SEQ-06, 99%) is not lowered, no trial is discarded, and no best-of-runs is taken. |

Lane order: ladder, r11, chipwait, m18, r15, one after another in the same hold sequence (chipwait runs `make clean`, so it comes after the lanes that use the ladder build). Each lane is started once.

G8 follows its own process and is not part of this window. Nothing here qualifies a physical
boot, an owner-key ceremony, firmware enrollment, a TPM operation or a real storage device.
