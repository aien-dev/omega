# R15 handoff (Claude → Codex), 2026-09-28

Task: finish ADR 0016. R15 performance proof first, then R16 orchestrator
retirement, and only if R15 genuinely PASSES. The full operator brief is the
"FINISH ADR 0016 — R15 PERFORMANCE PROOF, THEN R16" prompt. Follow it exactly.
Drake is not a programmer, so talk to him in plain English (see ~/AGENTS.md).

## Where things are

- Worktree: `~/workspace/omega-r15`, branch `feat/r15-performance-proof`
  (pushed; base is omega main `f70ae10`, which is R14).
- Build: `make AIENOS_R7_DIR=../aienos-capability-c PHYSICS_DIR=../physics-r13 <target>`.
- **Pre-registered methodology: `spec/r15-performance-proof.md`, commit
  `a4c6123`**, pushed before any R15 data was collected. Its acceptance
  criteria G1–G16 are binding. To change a criterion: new commit to the spec,
  explain why, then rerun the whole qualification. Clarifications that are not
  criteria (for example, trial-window latency distributions stored as
  per-window histograms rather than one line per activation) should be a
  small spec commit before qualification.

## Done

1. Surveyed the legacy loops. sovereign-core `run_until_complete`/`step` is
   LLM batched decode. aegis-runtime `execute_task` is an LLM tool-calling
   loop, and `pulse_once` runs SQLite chores. None of them does R13's semantic
   work, so the spec marks them NOT DIRECTLY COMPARABLE (§2). The baseline is
   the sequential control (SEQ).
2. Engine instrumentation (uncommitted before this handoff, committed with
   it). Existing tests still pass: R3 host, R13 host, R12 silicon (209 checks).
   - `RxStats`: `activations`, `externals`, `snapshot_bytes`, `crumb_bytes`,
     `proj_bytes`, `ring_bytes`, `gpu_results_taken`, `gpu_polls_empty`,
     `gpu_host_waits`, `seq_pulses`, `seq_polls`, `seq_runs`.
   - `RxTiming` ring (`rx_world_set_timing`). Each activation records
     demand, ready, run, fn_end, visible and sched_ns. Timestamps are taken
     only when the ring is installed.
   - `w->last_prop_ns` / `last_prop_hits`, for the propagation cost of the
     most recent wave.
   - `-DRX_MEASURE_NO_CAUSAL_DIGEST` skips the crumb SHA-256. It is for the
     RES-1-NODIGEST measurement build only. `rx_world_causal_digest_enabled`
     reports whether digests are on.
   - `rx_resident_wait_outstanding()` plus `claim_cv`, so the GPU completion
     acceptor sleeps until a claim is posted. Do not reuse R13's 1 ms nap: it
     would distort GPU latency.
   - `rx_gen_io_counters()` counts R9 bytes written and fsync calls.
   - GPU seat: S2R `%globaltimer` (0x52, defined locally in
     rx_resident_gpu.c, because `omega_blackwell_codegen.h` is hash-pinned).
     It writes the pickup time, the done time and the claim index to heartbeat
     words 20/24/28 (`RX_SEAT_HB_T_PICK/T_DONE/CLAIM`). R12 silicon passes
     with this change. **Not yet checked: that the stamp values are sane
     (nonzero, done > pick, a few µs apart).**
3. Sequential control: `src/runtime/rx_seq_reference.{c,h}`, labelled
   REFERENCE ORACLE ONLY.
   - Setup: `rx_world_init_native_sequential_reference()` creates a world
     with no workers and turns off the dependency wakes.
   - `rx_seq_pulse()` polls each stage's trigger field versions in a fixed
     order. It runs a ready stage through the engine's own `run_one`, via
     `rx_world_seq_activate_locked`, which keeps R6 budgets, admission,
     validation and crumbs. For a GPU stage it host-waits through a callback.
   - `rx_seq_run_until_complete()` has the legacy loop shape.
   - `rx_world_wait_quiescent` in sequential mode waits for an idle pulse.

## Next (in order)

1. Add R9 phase stamps: enter, barrier (after `live`), flip (after the
   `active` commit_file), receipt. Expose them with `rx_gen_last_phases`, for
   L1-G.
2. Write `tests/runtime/rx_r15_perf.c`, modes `trial <cfg> <round> <out>`,
   `l1 ...` and `l2 ...`. Build binaries for RES and NODIGEST, both host and
   silicon.
   - Rig: copy `start()` from `rx_r13_living.c`. Config RES4/RES1 uses
     `rx_world_init_native(...,4|1,...)`. SEQ uses the sequential init, plus
     an orchestrator thread that rebuilds its plan whenever `n_reactions`
     changes. It orders reactions by spec §2.2 names: `workload.matvec.serve`,
     `living.experiment.ask`, `aegis.decide`, `root.install`, `aien.observe`,
     `aien.predict`, `aien.explain`, `aien.assess`, `aien.plan`,
     `omega.watch`, `omega.reconsider`, `omega.synthesize.k`,
     `omega.verify.k`, `omega.measure.k`, `omega.select`,
     `living.experiment.prepare`, `living.blackwell.add`,
     `living.experiment.evidence`, `aien.experiment.observe`,
     `generation.prepare`, `generation.promote`, `generation.restore`. It
     yields when a pulse runs nothing.
   - Trial phases per spec §4: warm-up on A725 until the first selection and
     AIEN is confirmed; a BEFORE window of 2 s + 5 s; then X925, placement,
     goal and the ADAPT window; wait for goal MET; an AFTER window of
     2 s + 5 s. Record JSONL for windows, the episode, telemetry at 10 Hz,
     residency and correctness.
   - Crumb capacity: the log does not wrap. Size it to about 12M crumbs
     (about 1128 B each) and prefault it. Overflow must fail the trial.
   - PMU: per-CPU counters on the cores in use: `cpu_cycles` 0x11,
     `inst_retired` 0x08, `bus_access` 0x19, `ll_cache_miss_rd` 0x37,
     `l2d_cache_refill` 0x17, `mem_access` 0x13. PMU type `armv8_pmuv3_0` is
     A725 (cpus 0-4,10-14); `_1` is X925 (5-9,15-19). The qualification
     script sets `kernel.perf_event_paranoid` to 0 and restores it afterwards
     (sudo works).
   - L1: A activation latency (crumb t_start − cause t_end, timing off),
     B fanout, C scheduler (timing ring), D publication, E caps
     (batches of 100 validations), G barrier. L1-F comes from the AFTER
     windows of RES-1 vs NODIGEST.
   - L2: the R12 chain A→seat→B→dep→C (see `rig_up` in `rx_r12_silicon.c`).
3. `tools/r15_reduce.c`: C only, no Python. Deterministic, bootstrap seed
   0x15, 10k resamples. It reproduces every metric and gate G1–G16 from raw
   data.
4. `tools/r15_qualify.sh`: 12 Latin-square rounds of RES-4/RES-1/SEQ/NODIGEST
   + L1 + L2, with thermal wait and machine.json. Detach it (`setsid nohup`
   plus a finisher script), because usage limits can kill long runs.
5. **Energy blocker: must be resolved before any PASS.** See below.
6. Candidate-bound clean-tree qualification on GB10. Then the correctness
   reruns: R3, R7, R8, R9, R10, R11, R12 host+silicon, R13 host+silicon, R14
   host+silicon. Then the receipt `AIEN_RX_R15_REACTION_PERFORMANCE_V1`, the
   evidence commit, and a PR **merged with a merge commit** (not squash).
7. Only if R15 PASSES: R16, per the brief.

## Energy blocker (Drake decision needed)

- The GPU-domain energy counter works:
  `nvmlDeviceGetTotalEnergyConsumption` via dlopen `libnvidia-ml.so.1`, in
  mJ. It does not see CPU load. A test with all 20 cores busy moved it only
  from 4.1 W to 4.7 W.
- Whole-chip energy accumulators exist in firmware: `SPBM`, ACPI `NVDA8800`
  "MTEL", physical address `0x1c238000`, 4 KiB. Offsets: PKG energy 0x344,
  CPU_E 0x350, CPU_P 0x35c, GPC 0x368, GPM 0x374; SYS_TOTAL power 0x300; SoC
  package 0x304. Units are unverified.
- They are unreadable today. Secure Boot is on, so kernel lockdown blocks
  `/dev/mem`. No driver binds the device, and only Canonical's key is
  enrolled.
- Options for Drake:
  - (1) Enroll a machine-owner key and load a small signed read-only
    telemetry driver that exposes these as hwmon `energy*_input`. This
    changes the Secure Boot trust setup and needs him at the console for the
    MOK enrollment screen after a reboot.
  - (2) An external logging wall/USB-C power meter.
  - (3) Accept that R15 cannot PASS yet.
- Do NOT disable Secure Boot or bypass lockdown without Drake.

## Energy decision (Drake, 2026-09-28): OPTION 1

Drake chose option 1: enroll a machine-owner key and load a small signed,
read-only telemetry driver.

- Key made by Claude: `/var/lib/aien-mok/MOK.der` (public) and `MOK.priv`
  (root-only, mode 600). CN "AIEN Spark owner key (telemetry)", SHA-256
  fingerprint `19:0C:18:68:...:DF:D7:AE`.
- Enrollment is **pending Drake**. He runs
  `sudo mokutil --import /var/lib/aien-mok/MOK.der`, picks a one-time
  password, reboots, and at the blue MOK Manager screen chooses Enroll MOK →
  Continue → Yes → types the password → Reboot. Check it with
  `mokutil --list-enrolled | grep AIEN`.
- Codex still has to:
  - Write the driver: an out-of-tree, read-only platform driver bound to
    ACPI `NVDA8800` that ioremaps `0x1c238000`/0x1000 only. It exposes hwmon
    `energy*_input` for PKG/CPU_P/CPU_E/GPC/GPM and `power*_input` for
    SYS_TOTAL/SOC_PKG. It never writes. Put it in a repo (C, no Rust).
  - Build it against `/lib/modules/$(uname -r)/build` and sign it with
    `/usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256
    /var/lib/aien-mok/MOK.priv /var/lib/aien-mok/MOK.der <mod>.ko`.
  - After Drake's enrollment, `insmod` it.
  - Calibrate the units: find the raw units and the overflow behaviour
    against a known load, and cross-check GPC against NVML GPU energy.
  - Record the method in spec §7 in a new commit before qualification.
- No systemd, including for loading the module (see memory no-systemd).

- 2026-09-28: the enrollment request is QUEUED (Drake confirmed with `sudo mokutil --list-new`; the one-time password is known to Drake). Next: Drake reboots and approves at the blue screen. Afterwards, verify with `mokutil --list-enrolled | grep AIEN`.

- 2026-09-28 10:02: KEY ENROLLED. `mokutil --list-enrolled` shows CN=AIEN Spark owner key (telemetry). Secure Boot stays on. Modules signed with /var/lib/aien-mok/MOK.priv will now load. Next: write, sign and load the read-only SPBM telemetry driver, then calibrate it.

## Recovery audit and rework (Claude, 2026-09-28 afternoon)

- Forensic audit before any change; snapshot of the pre-audit local state
  (unpushed `f714c08`, uncommitted Makefile, untracked draft
  `rx_r15_perf.c` / `r15_reduce.c`) is in
  `~/workspace/forensics/r15-2026-09-28/` (patches + git bundle). The old
  `~/workspace/omega-r15` worktree is left untouched as evidence.
- Machine: Secure Boot on, owner key enrolled and matching the loaded
  module's signature, `MOK.priv` root 0600. The read-only SPBM module is
  loaded (owner approved keeping it loaded). `kernel.perf_event_paranoid`
  had been left at 0 by an unlogged session; restored to 4 (boot default).
- Rework branch: `r15-rework`, pushed to `feat/r15-performance-proof`.
  - SPBM reader and preflight runs kept under `research/m15/spbm/`; its
    in-place §7 spec rewrite not adopted.
  - Instrumentation fixes (CPU vs wall scheduler time, propagation counters,
    per-path copied bytes, per-store R9 I/O, timing overflow status) with
    `make test-r15-instr`.
  - Spec §16 clarification C1 (paranoid 4→1→4; PMU per-task inherited on
    both PMUs, raw sums, never scaled; byte definitions; parity gate).
  - Shared rig `tests/runtime/rx_r15_rig.{c,h}` and SEQ parity gate
    `make test-r15-parity-host` / `test-r15-parity-silicon`.
- The draft `rx_r15_perf.c` / `r15_reduce.c` predate these fixes and are not
  carried forward; the harness is to be rebuilt on `rx_r15_rig`.
- Still to do, in order: energy clarification C2 (needs load tests: give
  Drake a heads-up first; a power surge was reported during the earlier CPU
  load collection), harness, reducer, qualify script, dry run, full run.

### Finding: W-EPISODE goal miss rate (pre-qualification, host stand-in)

A 100-episode soak of each of SEQ and RES-1 (`R15_PARITY_REPEAT`) failed 7 of
100 in both, with identical failure types: 6 × goal UNMET_EXPLORED (AIEN's
post-promotion cost on X925 above the TARGET_PCT 55 goal) and 1 × the post-
promotion wait for AIEN's confirmed prediction running long enough that
continuous production filled the 2^18-crumb log (publication refused). A
later 40-episode RES-1 run had 0 failures, cost after/before 0.40-0.54
(target 0.55). The misses clustered in time, which points at machine
interference. Not orchestration-specific; no lost trigger or semantic
mismatch was found in any completed episode.

Consequence for R15: G1 requires goal MET in every one of 48 trials. At a
~6% per-trial miss rate the chance all 48 pass is ~5%. This is a property of
the R13 workload's target margin, recorded here BEFORE any qualification
data. TARGET_PCT and G1 are unchanged; any correction is an owner decision
and must be committed as a methodology clarification before qualification.
The harness must size the crumb log for the longest allowed window and fail
a trial on overflow (§16 C1 item 4 applies equally to crumbs).

### Blocker found: R14 silicon F is intermittent (2026-09-28 afternoon)

`test-r14-silicon` failed F_checkpoint_crash (and once B) on the rework
branch AND on the pre-change head `0ab53fb`, so the R15 instrumentation did
not cause it. Rerun of `rx_r14_recovery_silicon BF` on a quiet machine
(all cores < 3% busy, governor performance, max clocks): B passed, F adapted
after 3 crash points and failed at ~5 with `adapt -4` (AIEN never confirmed
the incumbent within 60 s while production filled the 2^18 crumb log) or
`adapt -11` (goal not MET). Host R14 passes. R14 last passed on silicon at
08:45 today (evidence 2c647c6); since then: two reboots (MOK enrollment),
the SPBM reader loaded, LM Studio and the NVIDIA Personal AI Router
resident. Cause not yet found. §14 requires R14 silicon on the candidate, so
this must be resolved before qualification.

### R14 question settled against the exact candidate `6b38173` (2026-09-28 13:09-13:22)

The R14 silicon test binary was built from `6b38173` itself (clean worktree,
no R15 change) and run 5 times back to back on a quiet machine, sampling the
machine once a second (`research/r14-failure-modes/exact-20260928-130858/`).

- Result: **5 of 5 runs FAIL** (`R14_LIVING_RECOVERY=FAIL`). F failed in all
  5; A in 4; C and D once each. B and E passed every time.
- Dominant failure: the organism adapts but the goal is never MET (`adapt -11`)
  or AIEN never confirms within 60 s (`adapt -4`); the crumb-log overflow and
  "production did not resume" lines follow from that long wait (the lazily
  backed log, `80f102a`, removes the overflow but not the goal miss).
- The machine was quiet (top process the test itself; temperatures 31-34 C;
  A725 2.808 GHz, X925 3.9 GHz). The `performance` governor is set at every
  boot by NVIDIA's stock `nv-cpu-governor` service, before this morning's pass
  too, so it is not the change.

**Conclusion:** the R14 silicon failure is not caused by any R15 change. The
same code that passed at 08:45 fails today; R14's pass depended on the
episode's goal margin (quad4 on X925 reaching <= 55% of the A725 incumbent
cost), which is marginal on this machine now. This is the same property as
the W-EPISODE miss-rate finding above. It blocks the §14 R14-silicon rerun and
G1 (goal MET in every trial) until TARGET_PCT / the goal margin is settled as
an owner decision committed before qualification. It does **not** block
building the harness, reducer and qualification runner; per the owner's
direction, no further environment analysis is done before those exist.

## Harness, reducer and qualification runner (built 2026-09-28 afternoon)

Built on the shared rig, as the rework plan required; no further environment
analysis was done first.

- `tests/runtime/rx_r15_perf.c` (+ `r15_measure.{c,h}`): one observation per
  process, raw JSON Lines only.
  - `trial <RES4|RES1|SEQ>`: the §4 episode with measurement hooks in the rig
    (`R15Hooks`: IDLE 5 s quiescent, BEFORE 2+5 s on A725, AFTER 2+5 s on X925
    at goal MET). ADAPT (goal crumb -> promotion crumb) and the L3 intervals
    (plan, selection, first seat claim, evidence, candidate, in force) come
    from crumbs. Live windows record served ops, rusage, RxStats deltas, SPBM
    + NVML energy, PMU (both CPU PMUs, inherited, raw sums), R9 store vs
    process I/O, per-thread CPU, R5 slots and seat residency (1 ms sampler).
    G1 uses the goal status at MET; the status after the AFTER window is
    reported as `goal_status_final` (AIEN keeps re-assessing while production
    continues; on the host it was seen falling back to UNKNOWN).
  - `l1 <A|B|C|D|E|G|W0|W1> <RES1|SEQ>`: X925 cpu 7 driver / cpu 8 worker for
    A-E; G (30 R9 barriers under production) and W0/W1 (timing off/on) use the
    rig on X925.
  - `l2 <RES1|SEQ>`: 256 closed-loop claims through `living.blackwell.add`;
    chip `%globaltimer` pick/done from the heartbeat block.
  - Four binaries: `make r15-perf-host` / `make r15-perf-silicon` (production
    and `_nodigest`) plus `build/r15_reduce`.
- `tools/r15_reduce.c`: verifies every file against SHA256SUMS, recomputes
  metrics 1-17 and G1-G16, writes `summary.json`. Seed 0x15, 10,000
  resamples, percentile CI; missing data = FAIL.
- `tools/r15_qualify.sh [--detach] <host|silicon> [rounds] [l1] [l2]`: balanced
  Latin square, L1, L2, machine.json, paranoid 4 -> 1 -> 4, SHA256SUMS, reducer.
  `--detach` = setsid nohup; progress in plain words in `progress.log`.
  Set `AIENOS_R7_DIR`/`PHYSICS_DIR` so the recorded commits are right, and
  `R15_OUT_BASE` for dry runs outside `evidence/`.

Still blocking a real qualification: (1) the goal-margin decision above (R14
silicon and G1); (2) §9: LM Studio and Ollama were resident at 13:45 and must
be stopped for the run; (3) a clean, committed candidate.
