# R16: production entry point and legacy surface

Spec: `spec/r16-orchestrator-retirement.md` (R16-G5). Map:
`spec/r16-orchestrator-retirement-map.md`. Checked by `make test-r16-surface`.

## The production entry point

The production path is the resident reaction world: the R13 living system
(goal in, AIEN reacts, Omega reacts, authority, GPU, evidence, generation
promotes) and its R14 recovery paths. Nothing sequences the faculties; each
reaction wakes on the objects it declared and commits under the grants it
holds, checked by the native AIENOS capability authority (C, pinned by
`aienos.lock`).

Who may act is fixed by the runtime too (spec C6): at startup the entry point
enrolls every production subject (`rx_living_enroll_callers`), closes
enrollment (`rx_world_bind_callers`) and binds the generation store to the
world and to the native authority (`rx_gen_bind_authority`). After that a
reaction registration, a generation proposal or a promotion that does not
carry the named subject's runtime-issued credential is refused.

| what | sources | make target |
|---|---|---|
| living system, host seat | `$(RX_R13_SRCS)` (omega `src/`, `src/runtime/`, `tests/runtime/rx_r13_living.c`) + `libaienos_capability.a` | `test-r13-host` |
| living system, GB10 seat | the same + `rx_resident_gpu.c`, the Blackwell encoder/codegen and the physics seat (`m16_native.c`, `nvrm.c`) | `test-r13-silicon` |
| recovery | `tests/runtime/rx_r14_recovery.c` on the same sources | `test-r14-host`, `test-r14-silicon` |
| proof that no legacy orchestrator is in the path (G3) | link map, strings and exec trace of the two binaries above | `test-r16-authpath`, `test-r16-authpath-silicon` |

## Legacy and reference paths (not production)

| path | class (map) | explicit name | notes |
|---|---|---|---|
| omegatool hand-sequenced living matvec | A, retired | `--legacy-oracle-living-matvec`; symbols `legacy_oracle_*` | the old `--demonstrate-living-matvec` mode is gone and rejected |
| omegatool milestone demonstrations (16 modes) | D, reference oracle | `--reference-demonstrate-<name>` | historical M4 to M17 demonstrations; the old `--demonstrate-<name>` modes are rejected |
| omegatool gate runners `--run-gates`, `--run-m<N>-gates` | B, kept | qualification runners | they run the historical milestone gates; they sequence tests, not faculties. The map does not flag them for G5 |
| omegatool default mode | none | none | with no argument omegatool prints usage and exits 1; it runs nothing |
| SEQ reference loop | D | `rx_seq_reference.[ch]` (`rx_seq_pulse`, `rx_seq_run_until_complete`) | R15 comparison baseline only; refuses a production world (`make test-r16-negative`) |
| M19 `omega_world_*` | C, known-good fallback | M19 world gates | kept for recovery |
| Omega Visor (`src/visor/`) | operator console | `./omega` | one operator command per line, only on operator request; not a production entry point |
| aien-sovereign-core, aegis-runtime (Rust) | A by non-use, or LEGACY / NOT-IN-CHARGE | map rows carry the label | not linked or executed by the production path (G3 checks both) |

## Host gates on this branch

Run from the omega tree. `AIENOS_LOCK_REPO` must name a clone that has the
`aienos.lock` commit (the default `../aienos-argus-cap` exists only for a
checkout directly under `~/workspace`); it is needed only the first time, to
extract the pinned authority into `build/aienos-authority/`. Without it the
build stops and says exactly what to set.

```
A="AIENOS_LOCK_REPO=$HOME/workspace/aienos-argus-cap"
make $A r16-inventory test-r16-inventory     # G1/G2 (seconds)
make $A test-r16-authpath                    # G3 host stand-in (about 30 s)
make $A test-r16-negative                    # G4 (under 1 s); PASS since spec C6: every subject acts only with a
                                             # runtime-issued caller credential, so the promoter-subject exploit that
                                             # was accepted at 44d8c06 (spec C5) is refused with the identity error
make $A test-r16-negative-mutants            # G4 guards load-bearing (about 1 min; judged on the gate line;
                                             # 27 mutants incl. 12 identity mutants, all killed)
make $A test-r16-surface                     # G5 (seconds)
```

## Full ladder (G7) and the R15 qualification: not run on this branch

These touch the graphics processor and must run on the frozen candidate
commit with a clean tree, one heavy run at a time.

Preconditions:

1. Candidate commit frozen, `git status` clean, branch rebased on omega main.
2. `PHYSICS_DIR` set explicitly to a physics checkout at `physics.lock`
   (`fecbedb`); `~/workspace/physics` is at a different commit. Use
   `~/workspace/hive-worktrees/physics` or a fresh checkout at the lock.
3. `AIENOS_LOCK_REPO` and `AIENOS_R7_DIR` as in the host section (the
   `mk/visor-authority.mk` default is wrong until fixed).
4. Quiet machine: `~/workspace/.spark-quiet` set by the session that runs the
   ladder, AI services off, no other heavy test (omega sessions rules).
5. GPU seat clean: no leftover `rx_*` or `omegatool` process holding the seat.
   Never wrap a chip test in `timeout` and never kill one.
6. Long runs detached (`setsid nohup`, or `tools/r15_qualify.sh --detach`), so
   a closed terminal or agent cannot stop them.
7. `sudo -n sysctl` allowed for `kernel.perf_event_paranoid` (the R15 run sets
   it 4 -> 1 and restores 4).

Command sequence (`V="PHYSICS_DIR=<physics at fecbedb> $A"`):

```
make $V test-r3              # R1, R2, R3, R4 (causal trace), R5, R6: one binary
make $V test-r7
make $V test-r8
make $V test-r9
make $V test-r10
make $V test-r11
make $V test-r12             # R12 host
make $V test-r13-host
make $V test-r14-host
make $V test-r15-parity-host
make $V test-r12-silicon
make $V test-r13-silicon
make $V test-r14-silicon
make $V test-r15-parity-silicon
make $V test-r16-authpath-silicon     # G3 silicon
make $V r15-perf-silicon              # builds the R15 binaries and reducer
PHYSICS_DIR=... AIENOS_R7_DIR=... tools/r15_qualify.sh --detach silicon
make r15-receipt RUN=evidence/R15/raw/<run-id> CANDIDATE=<commit> RERUNS=<file> NOTES=<file>
```

Time, from the R15 attempt 2 records on the same machine: the correctness
ladder took 2 min 39 s (`evidence/R15/reruns-3e9e53b-log-summary.txt`); the
R15 silicon qualification took about 35 min (21:57 to 22:32,
`evidence/R15/ATTEMPT-2-PASS.md`). With builds, G3 silicon under exec tracing
(about 1.5 min) and setting up the quiet machine, plan on 45 to 60 minutes.
