# CAND1-QUIET-AUDIT-20261005: publication index for the CAND-1 quiet-window evidence

Same convention as `evidence/CAND1-AUDIT-20261005/INDEX.md` (omega #288), under the `evidence-immutable` CI check:
this change adds new directories only, no existing file is touched, and no receipt is edited. Run directories keep
the names they have under `~/workspace/evidence-out/` on the Spark. No verdict is changed here. This file says which
artifact backs each claim in `aien-architecture qualification/candidates/CAND-1.gates.md` (quiet-window update, aien-architecture #135, merged 79dd430) and what is missing. Campaign ledger: `~/handoffs/2026-10-05-cand2-campaign.md` on the Spark.

## Candidate

CAND-1 = aien-sovereign-core `2eec75b08aeb8f9d11f950070e13f6c2c046cf24`, omega `cb06d081901c03063945a99ffed1c58d33397e9c`,
physics `6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf`, aienos `bbad5e4250e57f8cbd1be4cf1390109aa65ef92c`. Unchanged by this
campaign: every fix below is harness, test or tool code, so no CAND-2 exists and no old result is relabelled.

## Directories added

| Directory | What ran | Tree | Exclusive | Use |
|---|---|---|---|---|
| `CAND1-WINDOW-20261005T1252Z` | quiet window 1: R11 living, R15 silicon acceptance, attention timing x3 | omega 80ca5d4 (map-only descendant of cb06d08) | yes, quietlock hold from 12:52:56Z | qualifying for attention timing only |
| `CAND1-WINDOW2-20261005T1259Z` | quiet window 2: R11 living, R15 silicon acceptance | omega 81efb09 = cb06d08 plus the omega #286 harness fixes | yes, quietlock hold from 12:59Z | qualifying for R11 and R15 (R15 declared binding before its result, 13:03Z) |
| `CAND1-PATH2-2eec75b` | second-path build of every CAND-1 executable, plus a control build of omegatool | CAND-1 commits, checked out under a different directory | host build | path independence |
| `DIAG-G15-SEQ30-20261005T1339Z` | 30 R15 SEQ trials with the CAND-1 R15 executables, seat liveness only | omega 81efb09 | yes, 20 min hold, but see contamination note | DIAGNOSTIC, never qualifying |

## Claim to artifact

| Claim (CAND-1.gates.md) | Artifact | Check | Agrees |
|---|---|---|---|
| Attention timing PASS, medians 1.24, 1.21, 1.18 ms against 3.0 ms, 0 violations | `CAND1-WINDOW-20261005T1252Z/ATTN-timing-{1,2,3}/timing.json`, `exit.txt` | `median_ms` per run, `gate_median_ms` 3.0, `violations` 0, exit 0; executable `gpu_attention_test` sha256 e28bf26b... in `00-identity/executables.sha256` | YES |
| Window 1 R11 NOT_RUN (refused its own hold), R15 INVALID (preflight crash, no trial) | `CAND1-WINDOW-20261005T1252Z/R11-living/stdout.log`, `R15-silicon/stdout.log`, `SUMMARY.txt` (R15 rc=3 after 21 s) | log lines; no R15 receipt exists for window 1 | YES |
| R11 living PASS, 436 checks, 0 failures | `CAND1-WINDOW2-20261005T1259Z/R11-living/receipt/20261005T130010Z-81efb0941a5a/R11/rx_aien_faculty_receipt.json` | `checks` 436, `failures` 0, `living_run.exercised` true, `run_commit` 81efb09, `tree_dirty` false. `candidate_bound` is false: the receipt names no candidate, so the tie to CAND-1 is the source equivalence below. Copied from the run's own output directory after the window (`R11-living/COPIED.txt`) | YES, with that limit |
| R15 acceptance FAIL: 13 of 16 PASS; G9, G15, G16 FAIL | `CAND1-WINDOW2-20261005T1259Z/R15-silicon/receipt/f3c7726600894b25d0a9f52f65c2d8b18365c17a270fae9347133d512386e871.json`, `raw/` | `outcome` FAIL, `candidate_commit` = `run_commit` = 81efb09; G15 value 0.849114424 (threshold 0.99); G9 "0 valid pairs"; G16 "missing 17" | YES |
| R15 executables are the CAND-1 build | `CAND1-WINDOW2-20261005T1259Z/00-identity/executables.sha256` and window 1's | `rx_r15_perf_silicon` b44b3320..., `rx_r15_perf_silicon_nodigest` dda79ab7..., `r15_reduce` 9302c41a... equal in both windows; window 1 ran the CAND-1 tree 80ca5d4 | YES |
| Source equivalence of 81efb09 and cb06d08 | `git diff --quiet cb06d08 81efb09 -- src physics.lock aienos.lock` (branch `cand2/cand1-harness-overlay`) | exit 0, also checked by the window-2 guard before each step (`00-identity`) | YES |
| G9 and G16 instrument absent | `CAND1-WINDOW2-20261005T1259Z/R15-silicon/raw/machine-state.ndjson` | `spbm_uj` is -1 on every line (energy reader not loaded) | YES |
| Path independence: 8 of 9 SAME, omegatool DIFFERENT | `CAND1-PATH2-2eec75b/compare.txt`, `summary.txt`, `SHA256SUMS` | `omega-runtime orig=f96bdb7c... path2=47816667...`; all others SAME | YES |
| omegatool difference is the embedded physics path | `CAND1-PATH2-2eec75b/control/{command.txt,digest.txt,cmp-ranges.txt}` | control: second-path omega checkout, physics path set to the original, digest f96bdb7c... = CAND-1 | YES |
| R11 test binary differs between windows | `00-identity/executables.sha256` (c9f3c5df... vs 8090d108...) | expected: omega #286 changed `tests/runtime/rx_r11_aien.c` (test code, not a candidate executable) | YES |

## Erratum to CAND1-AUDIT-20261005 finding F6

F6 says the GitHub check-runs API gave omega cb06d08 "28 success, 2 skipped" and so contradicted the
CAND-1 record's "39/39 (2 skipped)". That query read only the first page (30 results; 28 + 2 = 30). With all pages
(`gh api repos/aien-dev/omega/commits/<sha>/check-runs --paginate`, 2026-10-05 ~13:45Z): omega cb06d08 39 success,
2 skipped; omega 80ca5d4 30 success, 11 skipped; aienos bbad5e4 14 success, 2 skipped; sovereign-core 2eec75b 1
success; interplane 5330a1c 8 success. The CAND-1 record was right and F6 is withdrawn. F6 itself is left
unedited (immutable); this note supersedes it.

## Runners

`runners/window.sh` (window 1) and `runners/window2.sh` (window 2) are the exact scripts that ran: each step records
command, start, end, exit code and machine snapshots before and after, and window 2 refuses a step unless HEAD is
81efb09 and the source-equivalence diff is empty. `SHA256SUMS` in this directory covers every file added by this change.

## Harness fixes found by these windows (none changes the candidate)

- omega #286: R11 recognises the quiet hold it runs under; the R15 preflight counts CPU PMU cycles only (newer
  kernels also list SMMU cycle counters, which crashed the parser in window 1).
- omega #290: the R15 machine-state sampler ends its own perf workload on stop instead of leaving a root
  `sleep 86400` that kept the quiet flag held.

## G15 diagnostic (not qualifying)

`DIAG-G15-SEQ30-20261005T1339Z` (runner `diag_g15.sh` in the directory): 30 SEQ trials of
`./build/rx_r15_perf_silicon trial SEQ` on omega 81efb09, the same R15 executable as window 2 (sha256 b44b3320...,
`executable.sha256`), under an exclusive 20 min quietlock hold from 13:39:12Z, trials ending 13:39:34Z to 13:50:23Z.
Result: 30 of 30 trials exit 0 and every residency record has `seat_live` equal to `intervals` (no lost samples;
`SUMMARY.txt`). The atlas image python process (18.4 GB GPU memory) was resident before and after
(`machine-before/`, `machine-after/gpu-processes.csv`), as it was in window 2.

Contamination, stated: another session's offline test job (INTERPLANE `cargo test` and `pytest`, CPU only, started
before the hold) ran until about 13:40:40Z, so trials 1 to 4 (ending 13:39:34Z to 13:40:59Z) overlapped it.
Trials 5 to 30 ran with no foreign build. All 30 were stall-free either way.

What it shows: the window-2 stall (one process of 58) did not recur in 30 trials with the atlas process present,
so that co-resident process alone does not reproduce it; the stall is intermittent and its cause is not
established. With 0 of 30, a per-trial stall rate above about 10 % is unlikely (one-sided 95 % bound 3/30).
What it does not do: it is not a qualification run, it does not replace or soften the binding window-2 R15 FAIL
(declared 13:03Z before that result), and it measures G15 only (no energy, no pairs, no reducer).


## Not proven here

- The cause of the one G15 stall in window 2 (trial SEQ-06, AFTER phase).
- Energy (G9) and the metric set (G16): the instrument was absent; nothing was measured.
- The R11 receipt does not itself name CAND-1 (`candidate_bound: false`).
- Nothing here is a hardware boot of AIENOS or a TRUST gate.
