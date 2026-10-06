# CAND-3 qualification window: declared attempts (written before any outcome)

FINAL (filled at the CAND-3 freeze, aien-architecture 951cade, 2026-10-06). Its sha256 is published to the mindmap with the
quiet-flag announcement, before the window starts, so it cannot be changed after the results are known.

Candidate: CAND-3 (aien-architecture `qualification/candidates/CAND-3.toml`). Omega code `f816473df391bc4fbcf99df722edd70ed26ebef1`; the harness runs from
`97ee27584cda0d8eaca136f31a44293cd36c9b02` (that commit, or an R16-map-only descendant, which the ladder guard checks is code-identical). Physics
`6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf`. Scripts (copied beside the evidence): `cand3_env.sh`, `cand3_ladder.sh`,
`run_window.sh`. Evidence directory: `~/workspace/evidence-out/CAND3-LIVING-f816473/`.

Executable digests of the candidate: `CAND-3.toml [executables]` (double build, two roots). Digests of the programs the window itself builds
and runs are recorded in each lane's `executables.sha256` and compared with the double build in `executables-vs-double-build.txt`
(MATCH or DIFFER; informational, a DIFFER is reported, it does not change a pass rule).

| Executable (window build, `build/`) | Double-build digest (CAND-3.toml, aien-architecture 951cade) |
|---|---|
| rx_r13_living_host | `605054a7aafaebb5482340ad8eaa7181649e832852c90eca81872258b4d857b7` |
| rx_r13_living_silicon | `9f48df860344c81f46b90be4ff47956824cc928fe79a8ee20a1a1fd6b892eb86` |
| rx_operator | `460c6d8a1787176027e7306121ed84c5d0567c2a3d2655564cf50010fed15079` |
| rx_r13_living_testbuild_silicon | `4a8430fcbd5a843ba334b934b23608304d9f2f8b3de37a14288afb2ccb7fdfd6` |
| rx_r11_aien_test | `42ffcbadd64a1c1058f334cdaa9216b75733d527df271b5047b7eb8d690161e6` |
| rx_composition_gate_gpu | `a3fa43e8718228922c7f5057acedc0d35101edd369e56084d52dc582e222a91b` |
| omegatool | `5a3084278ce62868b3655978bc161a0c635b368bddc588963849dd2c5cb343cd` |

## Rules for every attempt

1. Each attempt below runs **once**. No result is replaced by a rerun. If an attempt cannot start (refused, instrument absent, build
   failure, harness defect), that is its result and is recorded as such; a replacement is a new, separately declared attempt (as A7b
   was for CAND-2) and the original stays.
2. Every attempt's directory is kept whole: command, stdout, stderr, exit code, seconds, and machine snapshots before and after (GPU
   processes, top CPU, Xid count).
3. No chip test is killed or wrapped in a timeout. (The G6 silicon operator test has no SIGKILL phase by design: docs/r16-operator-control.md section 7.)
4. "Instrument refused" (BLOCKED_INSTRUMENT / INSTRUMENT_UNAVAILABLE) is never reported as poor performance, and never as a pass.
5. Receipts are copied under their sha256 name. A claim index lists each claim with the receipt digest that supports it. The verdict is
   reconstructed from the receipts by a separate reader before any status page changes.
6. The window runs under `quietlock hold` (owner `cand3-campaign` or the freeze lane's id), announced on the mindmap. It never stops
   another session's run: if the machine is busy, the window waits.
7. No lane relies on a program built by another lane. Each lane builds what it runs (CAND-2 defect A7: the chipwait lane's `make clean`
   deleted the R15 programs the r15 lane then needed). The lane scripts end each build step with a check that the programs exist.

## Capture and failures (added after the 2026-10-06 intermittent host failures)

Three intermittent host failures were seen under heavy load (investigation `~/workspace/investigations/2026-10-06-op-intermittent/`, follow-up omega #313): the R13 mode-E wall-clock goal check (`tests/runtime/rx_r13_living.c:1265`), logs lost by the cleanup trap of `tests/runtime/rx_operator_mutants.sh` (line 17; its base run writes under `$W`, line 73), and the R13 failure reason printed only on stderr (`rx_r13_living.c:1001-1002`). Therefore:

- Every attempt runs under the quiet lock (`quietlock hold`, the whole window), with the full stdout and stderr of every step captured (`step` in `cand3_ladder.sh` writes `stdout.log` and `stderr.log`); the 1-minute load is in the before and after snapshots.
- Logs the harness itself deletes (the mutant suite's temporary directory) are outside what this declaration can preserve; if such a log is lost, that is recorded as lost, and the attempt's result stands as its receipt reads. It is not repaired by a rerun.
- **A FAIL is preserved, not retried.** The only exceptions are invalid runs. I found no separate invalid-run rule in the R13/R16 specs; the rules that exist are: this file's rule 1 (an attempt that cannot start is recorded as that result, and a replacement is a new, separately declared attempt, as A7b was in CAND-2); `spec/r15-performance-proof.md` section 18 (line 659: during an R15 run nothing is stopped, dropped or rerun because of machine state); and `tools/m19r_campaign.sh` (lines 12 and 23: a CHIPWAIT run directory without `run.json`, `verdict.json` or `hashes.sha256`, or failing `sha256sum -c`, makes the campaign INVALID, checked first). A run is invalid here only if the program under test never started or produced no result file (build failure, preflight refusal, quiet lock refused or lost, harness crash before any test). A run that started and printed a FAIL is a FAIL. An invalid run is kept and recorded as invalid; it is never deleted or overwritten, and a replacement is a new, separately declared attempt written before it runs (rule 1).

## Permitted machine conditions (declared now)

- Xid count 0 before and after every step. Any Xid during a step is recorded and the step's result stands as it reads.
- 1-minute load at the start of a step at most 2.0 (R11 refuses above 2 by its own rule: that is NOT_RUN, not a pass).
- No other session's quiet flag. Other accounts' idle GPU processes (e.g. a resident python at 0 % utilization) and idle servers with no
  model loaded are tolerated and recorded in `gpu-processes.csv` and `top-cpu.txt`; they are not stopped.
- Host prerequisites of the G6 host test: `sudo -n` passwordless and `gdb` present (checked on this machine 2026-10-06: both present). If either is
  missing at window time the host operator test reports NOT_RUN (docs/r16-operator-control.md section 7), which is the declared result, not a failure.
- Tree: omega worktree clean and at the harness commit, physics at 6d7cf0d (the ladder guard stops the lane otherwise).

## Attempts, in order

| # | Attempt | Command | Pass rule (fixed now) |
|---|---|---|---|
| A1 | R16 qualification ladder, including R16 G6 operator control: host test (`test-r16-operator-host`), mutants (`test-r16-operator-mutants`, 25 mutants) and the silicon test (`test-r16-operator-silicon`, no SIGKILL phase) | `cand3_ladder.sh 97ee27584cda0d8eaca136f31a44293cd36c9b02 ladder`, step R16-ladder (`tools/r16_qualify.sh`) | Each gate PASS per its receipt. G6 `operator_emergency_controls_passing` is PASS only if host, mutants and silicon all ran and passed in the same run and each operator receipt names its seat, the candidate commit on a clean tree, zero failures and skips, the seat's gate value, and the sha256 of the production binary built in the run (r16_qualify.sh `op_receipt`). A skipped case is NOT_RUN, never PASS. **G7 stays NOT_RUN** (it needs a valid R15 receipt, and R15 is not attempted tonight). **G8 stays NOT_RUN** (merge-commit step, outside the script). R16 as a whole therefore reads NOT_RUN at best tonight; that is declared. |
| A1b | R11 living under load (AIEN faculty driven through the native AIENOS authority, live) | `cand3_ladder.sh 97ee27584cda0d8eaca136f31a44293cd36c9b02 r11` (builds `build/rx_r11_aien_test`, records its digest, waits 60 s, runs it once) | exit 0, last line `checks N failures 0`, and the living run exercised: output containing "living run not exercised" (another quiet flag, an R15 program alive, or 1-minute load above 2) is NOT_RUN, never a pass |
| A2 | R13 test build on silicon, candidate bound | ladder lane, step R13-testbuild-silicon | exit 0 and its receipt PASS with `candidate_bound` true and tree clean |
| A3 | Production hygiene on silicon | ladder lane, step prod-hygiene-silicon | exit 0 |
| A4 | COMPOSITION-2 on the GPU | ladder lane, step COMPOSITION-2-GPU | exit 0 and receipt PASS |
| A5 | M19 endurance (CHIPWAIT campaign) | `cand3_ladder.sh 97ee27584cda0d8eaca136f31a44293cd36c9b02 chipwait` (3 runs of `tools/m19r_qualify.sh` full mode) | the predeclared 3/3 rule of `tools/m19r_campaign.sh`: all three runs PASS, including the long-soak criterion. Limit recorded in advance: each soak is about 41 to 43 s of chip time; it meets the written criterion, it is not hours of endurance |
| A6 | M18 matmul gates | `cand3_ladder.sh 97ee27584cda0d8eaca136f31a44293cd36c9b02 m18` (builds omegatool, records its digest, runs `--run-m18-gates`) | exit 0 and gates PASS |
| A7 | R15 silicon performance acceptance | **NOT ATTEMPTED in this window.** The SPBM energy reader is to be restored by Drake the next morning; the preflight (omega #295/#305) would refuse today (BLOCKED_INSTRUMENT). `cand3_ladder.sh ... r15` refuses to run unless `ALLOW_R15=1`, and `run_window.sh` does not list it. | Recorded as NOT_RUN: not attempted. It is a separate, later, separately declared attempt (its own `DECLARED-ATTEMPT-A7c.md`, written before it runs); its pass rule then: R15 PASS per its receipt, SEQ-06 residency 99 % not lowered, no trial discarded, no best-of-runs. The r15 lane builds its own programs (`make r15-perf-silicon`) first, so the CAND-2 A7 defect cannot recur |
| A8 | Correctness reruns for carried results, only where the carry check says CHANGED | host and QEMU/chip commands exactly as the CAND-2 results used (CAND-2.gates.md section 2): none: every carried result (Native model, Attention, AIENOS in CAND-3.gates.md section 2) rests on IDENTICAL executables; the CHANGED executables belong to R13, R11, COMPOSITION-2 and R15, which are not carried and are run as A1 to A4 (R15: A7) | The carry check (`cand3_carry_check.sh`) is run first and its output is part of the declared record. A result with a CHANGED executable is rerun and judged by its original rule; IDENTICAL results are carried and labelled as carried. If nothing is CHANGED, A8 has no runs and says so |
| - | INTERPLANE offline gates | read GitHub check runs at the pinned interplane commit | all check runs completed success |

Lane order: ladder, r11, chipwait, m18, one after another in the same hold sequence (chipwait runs `make clean`, so it comes after the
lanes that use the ladder build; m18 builds omegatool itself). Each lane is started once.

G7 and G8 follow their own process and are not part of this window. Nothing here qualifies a physical boot, an owner-key ceremony,
firmware enrollment, a TPM operation or a real storage device.
