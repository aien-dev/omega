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
