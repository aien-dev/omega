# CAND-3 evidence: how each directory was made

Exact commands are in the run files themselves (`command.txt` per step, `00-declared/*.sh`, `CAND3-BUILD-*/cand3_build.sh`).
This file points at them and quotes the step commands. Times are UTC unless a log says otherwise. Nothing here was re-run.

## Builds, attention identity, carry check

| Directory | Command source | Published at |
|---|---|---|
| `CAND3-BUILD-A`, `CAND3-BUILD-B` | `runners/run_builds.sh` calls `cand3_build.sh A\|B <root> $OM $AO $PH $SC` (omega f816473, aienos bbad5e4, physics 6d7cf0d, sovereign-core 80e071a), after `quietlock check`. Root A is `.../cand3/rootA`, root B is `.../cand3/rootB/deeper/path-two`. stdout in `logs/buildA.out`, `logs/buildB.out`, exit codes in `logs/builds.done` (`A exit 0`, `B exit 0`). The script is also in each build directory. | `runners/run_builds.sh`, `CAND3-BUILD-A/cand3_build.sh`, `CAND3-BUILD-B/cand3_build.sh` |
| `CAND3-ATTN-IDENTITY` | `README.txt` says: `make -j6 build/ab/gpu_attention_test` with `OUT_DIR=build/ab`, at omega 79a805d and f816473, same worktree path. No script file was kept for this step (UNVERIFIED beyond the README text). | `CAND3-ATTN-IDENTITY/README.txt` |
| `CAND3-CARRY-79a805d` | `cand3_carry_check.sh` (one build per target at the CAND-2 code commit, compared with the CAND-3 digests); logs `build-*.log`, result `result.txt`. | `CAND3-CARRY-79a805d/cand3_carry_check.sh`, `runners/cand3_carry_check.sh` |
| tree setup and compare | `runners/cand3_setup_trees.sh` (git worktree add only), `runners/cand3_compare.sh`. | `runners/` |

## Windows

W1 (`CAND3-LIVING-f816473`, INVALID) and W2 (`CAND3-LIVING-f816473-w2`) are separate attempts. Starter for W2: `runners/start_window.sh`
(waits, never interrupts, until no qemu runs, 1-minute load below 1.5 and no quiet hold; runs `cand3_setup_trees.sh`; then
`quietlock hold --owner cand3-campaign --minutes 90 ... -- env WINDOW_TAG=w2 ./run_window.sh`). Logs: `logs/window-start-w2.log`,
`logs/window-w2.out`. W1's starter output is `logs/window-start.log` and `logs/window.out`.
The window driver and lane script are `00-declared/run_window.sh`, `00-declared/cand3_ladder.sh`, `00-declared/cand3_env.sh`
(sha256 in `00-declared/sha256.txt`). Declared attempts: `00-declared/DECLARED-ATTEMPTS.md`, `00-declared/DECLARED-ATTEMPTS-w2.md`.
At publication, `cand3_env.sh`, `cand3_ladder.sh`, `run_window.sh`, `DECLARED-ATTEMPTS.md` and `DECLARED-ATTEMPTS-w2.md` under
`CAND3-LIVING-f816473-w2/00-declared/` were compared byte for byte with the originals in `~/workspace/cand3-campaign/cand3/` on the Spark: all identical.
W1's `cand3_ladder.sh` and `run_window.sh` differ from W2's (W2 appends `-$WINDOW_TAG` to the evidence directory so W1 is never written to); `cand3_env.sh` and `DECLARED-ATTEMPTS.md` are identical.

Step commands from `CAND3-LIVING-f816473-w2/<step>/command.txt`, with exit code and seconds from `exit.txt` and `seconds.txt`:

| Step | Command | exit | seconds |
| R16-ladder | `env R16_OUT_DIR=~/workspace/evidence-out/CAND3-LIVING-f816473-w2/R16-ladder/raw R16_EXPECT_COMMIT=97ee27584cda0d8eaca136f31a44293cd36c9b02 tools/r16_qualify.sh` | 3 | 880 |
| R13-testbuild-silicon | `env OMEGA_CANDIDATE_COMMIT=97ee27584cda0d8eaca136f31a44293cd36c9b02 make test-r13-testbuild-silicon` | 0 | 12 |
| prod-hygiene-silicon | `make test-prod-hygiene-silicon` | 0 | 0 |
| COMPOSITION-2-GPU | `make test-composition-gate-gpu` | 0 | 4 |
| R11-build | `make build/rx_r11_aien_test` | 0 | 1 |
| R11-living | `./build/rx_r11_aien_test` | 0 | 0 |
| CHIPWAIT | `tools/chipwait_campaign.sh --omega-candidate 97ee27584cda0d8eaca136f31a44293cd36c9b02 --physics-candidate 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf --physics-dir ~/workspace/cand3-campaign/cand3/wt-physics-chip --campaign-dir ~/workspace/evidence-out/CAND3-LIVING-f816473-w2/CHIPWAIT/campaign --runs 3` | 0 | 460 |
| M18-build | `make build/omegatool` | 0 | 0 |
| M18 | `./build/omegatool --run-m18-gates` | 0 | 5 |

(`/home/drakestapleton` is shortened to `~` in the table above only; the files keep the full paths.)
R16-ladder exit 3 is the declared NOT_RUN overall (G7 and G8 NOT_RUN). Each CHIPWAIT run's command is in
`CHIPWAIT/campaign/run-00N/command.txt`: `m19r_qualify.sh --omega-candidate 97ee27584cda0d8eaca136f31a44293cd36c9b02 --physics-candidate 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf --physics-dir <cand3>/wt-physics-chip --evidence-root <W2>/CHIPWAIT/campaign --run-id run-00N`.
