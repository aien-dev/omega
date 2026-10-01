# TURING: current state of what already exists (read-only audit)

Update 2026-09-30: R16 is CLOSED (omega#112, `3dd5eaa`, all gates G1 to G8 PASS) and the `src/runtime/` edit freeze is LIFTED (aien-architecture #70, `e89ba94`). Statements below that the runtime is frozen "until R16" describe the state when this was written.

Correction 2026-10-01 (recorded now, not at merge time): omega#112 (merge `3dd5eaa`) merged while the evidence-immutable check was FAILED (run 36799862685, head `6831117`), because `evidence/R16/inventory.json` was edited in place by commits `1edb56b` and `6831117`. The exception was not recorded at the time. The R16 receipt `evidence/R16/22d7a79a985514ac38139d39c71c9638d9b6b0a6e05425b6810bdb4833d1ea64.json` is QUALIFIED at candidate `850fc545` only. All R13 to R16 candidate-bound receipts predate omega#126 (`4f8485b`), so the current living build is IMPLEMENTED / NOT QUALIFIED and re-qualification is owed.

Date 2026-09-29. Read-only: nothing built, run or edited. Ground truth = code. README/roadmaps = navigation only.

## 0. Pins

| Repo | Commit | How read |
|---|---|---|
| omega main | `4b217aa` (MA-2/MA-3 merged via #78) | `git show/grep origin/main` in omega-polyglot worktree |
| omega polyglot | `53f7430` branch feat/polyglot-0 (local only: `git branch -r --contains` is empty) | worktree HEAD |
| omega OSC-0 | origin/osc/osc-0 `5c4b707`, PR #76 OPEN, draft | `git show` |
| aienos | origin/main `603c91d` | fresh shallow clone (job tmp/aienos) |
| aien-architecture | main `8ec9b1d`; PR #58 head `ed4474a` (OPEN) | shallow clone (job tmp/arch); `gh api` |

Reused audits (not re-derived): `docs/polyglot/OMEGA_POLYGLOT_CURRENT_STATE.md` (feat/polyglot-0, pinned 9207ce3; its
"MA-3 not merged" row is now stale: MA-3 is on main via #78 `4b217aa`), `OMEGA_SYSTEMS_CORE_CODE_AUDIT.md` (osc/osc-0),
`docs/plans/mixed-algebra/MIXED_ALGEBRA_CURRENT_STATE.md` + `docs/adr/0019-mixed-algebra-realization.md` (arch PR #58).

Vocabulary: IMPLEMENTED / PARTIAL / EXPERIMENTAL / PLANNED / MISSING / CONFLICTING / OBSOLETE.

## 1. Concept classification (paths on omega origin/main unless noted)

| Concept | Status | Evidence |
|---|---|---|
| Canonical object encoding OMG0 + SHA-256 | IMPLEMENTED | `src/omega_types.h:14`, `src/omega_canonical.c`, own `src/sha256.c` |
| Program identity v2 | IMPLEMENTED, narrow | domain `"omega.program.v2"` `src/omega_program.h:41`; body = chain of unary u64 ADD/SUB/MUL/AND/OR (`:44`). Cannot name matvec / Omega-X (PR #58 state doc row 37) |
| Semantic id for Omega-X (ternary GEMV) | MISSING | only a string id + C contract `src/algebra/realize_common.h:47-56,73`; polyglot uses SHA-256 of spec §1 as a parallel "contract digest" (polyglot audit §1, conflict #11) |
| Semantic object graph | IMPLEMENTED | `OmegaGraph` (`src/visor/visor.h:9` "OmegaGraph is the only semantic store"); action graph `src/runtime/rx_graph.h:181-196` |
| Realization objects (AArch64 bytes) | IMPLEMENTED, narrow | `RealizationObject{realization_id, semantic_id, machine_id, code_bytes}` + triple id `src/omega_realize.h:8-24` |
| Algebra realizations `realize_*` | IMPLEMENTED (10 bit-exact C/intrinsic) | `oma_rz_impl{id,label,family,exact,weak_baseline,max_n,pack,run}` `src/algebra/realize_common.h:47-56`; oracle `:73`; files `src/algebra/realize_{binary,bitplane,dense,rns,sparse}.c` |
| rx_costmodel | IMPLEMENTED, matvec only | `RX_CM_ARMS 8`, `RX_CM_OPS 1 /* matvec only */` `src/runtime/rx_costmodel.h:36-37`; `RxCmDecision{action,arm,why,p_runner_up_better,expected_improvement,pred[]}` `:102-110` |
| Stand-in selector oma_select | EXPERIMENTAL | `src/algebra/oma_select.h:4` "STAND-IN: not wired to rx_costmodel"; `oma_sel_candidate{eligible,cost_ns,runs_used,within_noise_rel,...}` `:72-82`, `oma_sel_decision{tie,...}` `:84-103` |
| Polyglot registry + explainer | EXPERIMENTAL (unpushed branch) | `omx_candidate{impl,language,toolchain,compiler_derived,toolchain_only,source}` `src/polyglot/omx_lang.h:14-21`; `tests/polyglot/polyglot_explain.c:1-20` (receipts-only deterministic selector + TIE verdict) |
| Mixed-algebra doctrine (identity vs realization, evidence tiers E0-E4, selection by measured cost) | PLANNED (ADR 0019 PROPOSED, arch PR #58) | `docs/adr/0019-mixed-algebra-realization.md` §3.3 (encoding enters `realization_id`, never semantic id), §8 tiers E0-E4 (lines 116-126), §9.1 selection |
| Cognitive routing rx_route | IMPLEMENTED, cognitive ops only | `src/runtime/rx_route.h:48-63` (RECOGNIZE/PLAN, hw bits, APPROX/EXACT) |
| MachineGraph | IMPLEMENTED (CPU profile) | `OmegaMachineGraph` `src/omega_machine.h:53-65`, `omega_machine_compute_id` `:88`, DGX Spark / QEMU profiles `:91-94` |
| Machine identity (cross-repo) | CONFLICTING | 3 shapes: `CqCandidate.machine_id` u32 (rx_capq), FORGE V2 32-byte id, aienos ADR 0010 `MachineId` (Proposed) (polyglot audit conflict #12); aienos `docs/adr/0010-fabric-machine-identity-and-capability-advertisement.md:3` "Status: Proposed" |
| Capability graph / query | IMPLEMENTED | `cq_compile/cq_query/cq_rank` `src/runtime/rx_capq.h:227-263`; aienos C authority `native/capability/` (aienos) |
| World / commit | IMPLEMENTED | `src/runtime/rx_world.{h,c}`; crumb kinds CREATE..QUARANTINE `rx_world.h:88-100`; no type named `WorldCommit` in omega (grep empty); ARGUS event `ARGUS_EV_WORLD_COMMITTED=70` (aienos `native/argus/argus_abi.h:163`) |
| Generations / promotion / generation IDs | IMPLEMENTED | `src/runtime/rx_generation.h:1-12` (candidate, barrier, promote right); u64 cap generation (#71) |
| J-Space | IMPLEMENTED (host reference) | `src/runtime/rx_jspace.h:1-20`: semantic identity = SHA-256 chain; "realization" = physical holding with recipe, placement, measured costs |
| Cortex | IMPLEMENTED (host ref); Rust twin CONFLICTING | `src/runtime/rx_cortex.h:1-14` append-only, digest per object; aienos `crates/aienos-cortex` (Rust) |
| Provenance / EvidenceReceipt | PARTIAL / PLANNED | omega: `src/omega_evidence.h:48-74` (run_id, run_commit, tree_dirty, digest-named writes); receipts named by own SHA-256 (`evidence/EMPIRICAL/*`, `evidence/R15/065c...json`), immutability CI `.github/workflows/evidence-immutable.yml`. No shared schema (polyglot audit §11). `EvidenceReceiptV1` only in aienos `docs/TRUST-1-IMPLEMENTATION-PLAN.md:38` (PLANNED) |
| BuildIdentity | PLANNED / MISSING in code | aienos `docs/TRUST-1-IMPLEMENTATION-PLAN.md:225` `BuildIdentityV1` (Rust-shaped: Cargo.lock); OSC-0B §II.9 PROPOSED compiler-digest build receipt (osc/osc-0 audit lines 434-450); ARGUS kind `RUNTIME_BUILD_CHANGED=72` exists |
| FORGE | PARTIAL (v1 stand-in lowering) / IMPLEMENTED host-only (V2 data contract) | physics `forge/forge_realize.c:68`, `forge/v2/forge_substrate_v2.h:114-118` (per polyglot audit §8, not re-read) |
| AEGIS | IMPLEMENTED (resident R8) | `src/runtime/rx_aegis.{h,c}`; aienos Rust `crates/aienos-aegis` CONFLICTING |
| Effect Broker | PARTIAL / CONFLICTING | only Rust: aienos `crates/aienos-aegis/src/broker.rs`; omega has `src/visor/visor_effect_request.h:6-7` (read-only unauthorized EffectIntent view); "whole-product Effect Broker" deferred in `spec/r16-orchestrator-retirement.md:244` |
| Compute Fabric | PARTIAL (in-process catalog) / PLANNED | `CQ_LOC_FABRIC` `src/runtime/rx_capq.h:74`, lease adverts `:203`; `spec/capability-query-ir.md:153` "Fabric ... in-process catalogs"; arch "AIEN Fabric"/"Compute Fabric" (archive docs) |
| Accelerator / Blackwell path | PARTIAL (silicon receipts: vector add, INT32 + FP16/BF16 matmul) | `src/omega_blackwell_*`, `src/omega_accelerator{,_world}.{h,c}`, `src/runtime/rx_resident_gpu.c`; FP32 SIMT EXPERIMENTAL PR #32 open |
| CPU path | IMPLEMENTED (scalar encoder, JIT, NEON via C intrinsics) | `src/aarch64_encoder.c` (25 scalar emitters, 0 SIMD), `src/omega_exec.c` W^X; matvec arms `src/omega_matvec{,_quad}.c` |
| General compiler / `omega_program_realize` | MISSING | declared `src/omega_program.h:117`, no definition; M6 self-host OBSOLETE (fixed output) (polyglot audit §2) |
| RSI | PLANNED (doctrine term only) | arch `CONTEXT.md:37`, `CURRENT_EXECUTION_PLAN.md:40` "not on critical path"; no omega code symbol (case-sensitive `git grep -w RSI` empty) |
| Visor | IMPLEMENTED (V1 REPL) | `src/visor/*` 21 files; `why <x>` "explain where x came from" `src/visor/visor_console.c:42,417`; Visor has no ARGUS/Cortex consumer (grep: 0 cortex/argus hits in rx_argus.c->visor, only 2 "cortex" mentions in `visor_world.h`) |
| polyglot-explain | EXPERIMENTAL (unpushed) | `tests/polyglot/polyglot_explain.c` |
| Content-addressed stores | IMPLEMENTED (several, separate) | omega receipts by digest; Cortex digests; J-Space unit chain; rx_plan verified plan cache (`src/runtime/rx_plan.h:28`); aienos Store v1 Rust `crates/aienos-kernel/src/store.rs` + ADR 0003/0016. No single store, no refs/tags, no Merkle root across gates (polyglot audit §11) |
| Benchmark harnesses | PARTIAL, per-gate | MA-3 `tests/algebra/bench_mixed_algebra.c` (pinning, schedstat, thermal); R15 `tests/runtime/r15_measure.c`, `rx_r15_instr.c`; empirical optimizer `tests/runtime/rx_empirical_optimizer.c`; polyglot `src/polyglot/omx_bench.c` (branch). No shared library; no cpufreq capture |
| Crumbline (AIEN curriculum) | IMPLEMENTED | `src/crumbline/cl_*.{c,h}` |

## 2. Existing tracing / observer / telemetry / event paths

| Path | Status | What it is | Measured overhead |
|---|---|---|---|
| **ARGUS producer in the runtime** | IMPLEMENTED; built with `RX_ARGUS ?= 2` for AEGIS/promotion suites, default runtime build RX_ARGUS=0 = no code | `src/runtime/rx_argus.h:1-60`: per-thread SPSC ring, per-stream seq, "emit on transition, count on use" (256-slot use table flushed as `CAPABILITY_USE_SUMMARY`), stamp-ordered consumer merge; counters emitted/pushed/refused `rx_argus.c:40-42`; `Makefile:795-822` | emit 2.25 ns p50 per use (`evidence/ARGUS/h2-verify/VERIFY.md:54`); R8 wall: emit-only +2.09%, ingest shipped default +3.87% [CI +3.27,+4.47] (`evidence/ARGUS/speed2/SPEED2.md:74-76`); earlier +6.1% FAIL (`VERIFY.md:74-75`); ingest ~1 us/event SHA-256 chain (aienos `native/argus/docs/ARGUS0_GATES.md:52`). ADR 0017 target 2%, lane limit 5% (`SPEED2.md:11-12`) |
| ARGUS ABI + drop classes | IMPLEMENTED (aienos C) | `native/argus/argus_abi.h:127-134`: CRITICAL never dropped silently (full ring, sticky overflow), SECURITY/AUDIT/INFORMATIONAL refused above watermarks and counted; `TELEMETRY_DROPPED=80` synthesized; finding `F_TELEMETRY_LOSS` `:263`; 30+ stable append-only event kinds `:138-179` (authority, effects 50-52, WORLD_COMMITTED 70, RUNTIME_BUILD_CHANGED 72, artifacts, machines, providers) | as above |
| aienos authority observer hook | IMPLEMENTED | `src/runtime/aienos_cap.h:70-91` (aienos 12add16): one call per state change, after lock release; unset = one NULL check per admin op | "one NULL check" (header claim; no number) |
| Causal crumbs (World) | IMPLEMENTED | content-addressed crumb per outcome `src/runtime/rx_world.h:10,88-100`; long-episode capacity 2^25 `:41-43` | R15 G8 throughput RES-1 / RES-1-NODIGEST = 0.832 (`evidence/R15/ATTEMPT-1-FAIL.md:23`, criterion >= 0.50 `spec/r15-performance-proof.md:395`); attempt-2 G8 value UNVERIFIED |
| R15 instrumentation counters | IMPLEMENTED (test-side) | `tests/runtime/rx_r15_instr.c:1-16` (propagation, scheduler, ring bytes, R9 I/O), `RX_MEASURE_NO_CAUSAL_DIGEST` build macro | counters themselves: not measured separately (UNVERIFIED) |
| Shared-world rings | IMPLEMENTED | `src/runtime/omega_shared_world_abi.h:146-160` SPSC, u64 free-running head/tail, 128-byte descriptors, ring ops `rx_world.h:129-135` | none found |
| Fusion observer / canary shadow | IMPLEMENTED | `AgFusionObserver` `src/runtime/rx_fusion.h:105,227-239`; shadow canary `:16,274` | none found |
| Energy meter | IMPLEMENTED test-side only | hwmon read in `tests/runtime/rx_empirical_optimizer.c`, `r15_measure.c` | n/a |

## 3. Messages / dialects between participants

| Item | Status | Evidence |
|---|---|---|
| Semantic communication (need -> projection -> delta, one canonical encoder) | IMPLEMENTED | `src/runtime/rx_semcomm.h:1-30` (InformationNeed, SemanticProjection, `rx_sem_encode`, `rx_sem_delta`, authority-checked, carries no capability) |
| ARGUS event format (fixed binary, append-only kinds) | IMPLEMENTED | aienos `native/argus/argus_abi.h` |
| AIEN faculty messages | IMPLEMENTED as World objects (no wire format) | `src/runtime/rx_aien.h:1-14` (observe/predict/explain/plan as reactions) |
| Visor command language | IMPLEMENTED | `src/visor/visor_parse_command.c`, `visor_console.c` |
| Surface language V0 -> OMG0 | PARTIAL | `src/language/omega_{lex,parse,lower}.c` |
| Versioned dialect dictionaries, cross-participant translation, round-trip tests | MISSING | `git grep -w -i dialect` empty in omega, aienos, arch |
| Human/LLM <-> Omega message format | MISSING | no code found (UNVERIFIED beyond grep of the three repos) |

## 4. Answers

**Q1. Smallest existing set for "one meaning, many realizations, evidence, cost, selection".**
Today it is spread over five pieces, none of which links to the others:
- meaning: `SemanticId` (`src/omega_types.h`) but Omega-X has none; the realization contract is `oma_rz_oracle` + `OMA_RZ_MAX_N` (`realize_common.h:25,73`);
- realizations: `oma_rz_impl` (`realize_common.h:47-56`, family = representation) + `omx_candidate` (language, toolchain; branch only) + `RealizationObject` (machine-bound code bytes, `omega_realize.h:8-18`);
- evidence: MA-3 receipts `evidence/MIXED_ALGEBRA/*` + `omega_evidence.h` run fields; tiers E0-E4 only in ADR 0019 (PROPOSED);
- cost: `RxCmPrediction/RxCmDecision` (`rx_costmodel.h:79-110`, matvec only, 8 fixed arms) and `oma_sel_candidate` (`oma_select.h:72-82`);
- selection: `oma_select` (stand-in) / `polyglot_explain` (branch) / `rx_costmodel` decide.
Also J-Space already separates semantic unit identity from physical "realizations" with recipe, placement and measured costs (`rx_jspace.h:1-20`), and rx_capq `CqCandidate` carries latency/energy/cost/confidence/evidence (polyglot audit §3).
Missing for a Field V0 record: (1) a semantic id for the operation inside the identity system (program id v2 cannot express it; OSC-0B gate); (2) one realization-spec record joining impl id + representation/algebra + language/toolchain + precision + backend + machine id + source/build digest; (3) the E-tier per realization as data; (4) a shared receipt schema (none exists, polyglot audit §11); (5) a cost-model arm that is not a fixed 8-slot matvec table (`rx_costmodel.h:36-37`, widening changes the serialized blob); (6) a link from selector output to receipts by digest. Items 2, 3, 6 can be a pure C record over existing digests; 1 and 5 touch frozen/gated areas.

**Q2. Natural home for Shadow V0.** ARGUS is already the hot-path event append plane: per-thread SPSC rings, no lock, count-on-use summaries, ordered merge, CRITICAL-never-silently-dropped vs degradable classes with synthesized `TELEMETRY_DROPPED` (aienos `argus_abi.h:127-179`; omega `rx_argus.h`). Its CRITICAL/SECURITY/AUDIT/INFORMATIONAL classes are the HOT/OBSERVED/FORENSIC split under other names, and its kinds already cover effects (50-52), authority, World commits (70), build change (72). Promotions are covered only via the PROMOTE right / generation events, not a dedicated kind (UNVERIFIED whether promotion emits an ARGUS event). Causal crumbs are the second existing plane (World provenance). Overhead numbers exist: ARGUS 2.25 ns/use, +2.1% emit-only, +3.9% shipped ingest (SPEED2); crumb digests cost 17% throughput on R15 RES-1 (G8 0.832). Missing: no ARGUS -> Cortex -> Visor path (Visor has no ARGUS reader); ARGUS is security-scoped ("observes authority decisions; it never authorizes", `rx_argus.h:4`), not realization/perf telemetry.

**Q3. Fabric/dialect today.** Closest: `rx_semcomm` (need -> projection -> canonical encoding -> delta), the ARGUS binary ABI, and the OMG0 canonical encoding. "Fabric" in code means remote-machine capability leases in rx_capq (in-process catalog). No dialect versioning, dictionaries or round-trip translation tests exist anywhere.

**Q4. Naming collisions.**
- Fabric: omega `CQ_LOC_FABRIC` + "house Fabric" `src/runtime/rx_capq.h:16,74,203`, `rx_contract.h:176`, `spec/capability-query-ir.md:23,52`; aienos "Personal Fabric" ADR 0010, `CONTEXT.md:90`, ARCHITECTURE §2; arch "AIEN Fabric" (5), "Compute Fabric" (2), 23 files. **Heavy collision.**
- Field: generic (struct fields; `rx_contract.h:74` "Field types"; semcomm "fields"); arch doctrine `DISCOVERY.md:363` "Field-Like Systems". Ambiguous in prose.
- Shadow: ARGUS "shadow state/generation" (aienos `argus_abi.h:8,47,51,163`; ADR 0017); `Shadow` struct `src/runtime/rx_graph.c:1460-1585`; fusion "runs in shadow" canary `rx_fusion.h:16`, `spec/workflow-fusion.md:31,175`; aienos artifact loader. **Direct collision with ARGUS.**
- Route: `rx_route` cognitive routing (`src/runtime/rx_route.h`); "Route of a legitimate effect" `spec/visor-authority.md:22`.
- Dialect: none. Turing: none (aienos hits are "capturing"/"measurement" substrings).
- Also "realization" already means three things: `RealizationObject` (code bytes), `oma_rz_impl`, J-Space physical realization.

**Q5. Duplication risks.**
- Shadow duplicates ARGUS (rings, classes, drop accounting, evidence chain) and partly causal crumbs. Build Shadow as new ARGUS event kinds/streams (append-only ABI permits it) rather than a second plane.
- Field selector duplicates `rx_costmodel` + `oma_select` + `polyglot_explain` (three selectors already); Field record duplicates `omx_candidate`/`oma_rz_impl` and J-Space realization metadata; E-tiers duplicate ADR 0019 §8.
- Fabric duplicates `rx_semcomm` (transport of meaning) and rx_capq Fabric (remote capabilities).
- Content-addressed store duplicates omega digest-named evidence, Cortex, rx_plan cache, J-Space chains and aienos Store v1 (Rust). Identity for contracts duplicates program id v2 / OSC-0B `SemanticId<T>`.
- Explanation duplicates Visor `why` and `polyglot_explain`; Cortex already is append-only evidence memory.
- BuildIdentity duplicates aienos TRUST-1 `BuildIdentityV1` (planned) and OSC-0B §II.9 build receipt (proposed).

**Q6. Constraint conflicts.**
- OSC-0B (omega PR #76, OPEN draft) is the hard gate before identity/compiler changes (`OMEGA_SYSTEMS_CORE_CODE_AUDIT.md:19`); it flags layout-dependent hashing (`:95-99`, e.g. `omega_program.c:276`) and live handles in durable identity (`:93`, `:385` "identity break ... needs version bump and Drake-visible plan"). Turing (d) content addressing of contracts/realization specs and any Field semantic id **conflict** until OSC-0B is decided. A Field V0 keyed by existing digests only (no new SemanticId domain) avoids it.
- Runtime edit freeze "until R16 closes" (`CURRENT_EXECUTION_PLAN.md:540`, per polyglot audit #2) vs arch `8ec9b1d` "record R16 closure" and `CURRENT_EXECUTION_PLAN.md:45` still "IN PROGRESS": **CONFLICTING**; wiring Field into `rx_costmodel` or Shadow into `src/runtime/` needs an explicit ruling.
- No Rust: aienos Cortex, AEGIS broker (Effect Broker), evidence crate, Store v1, TRUST-1 `BuildIdentityV1` (Cargo.lock) are Rust; Turing must not extend them. ARGUS, capability authority, all of omega are C: fine.
- No Python: omega CI `.github/workflows/rx-host.yml` has 3 inline python blocks; physics gate scripts call 5 `.py` (polyglot audit §18). Any Turing receipt checking must not reuse them. Mojo launcher on PATH is Python (use `~/.pixi/bin/mojo`).
- Fabric "models" participants: LLM/model participants imply outside runtimes (MAX/CUDA) that Drake is leaving; no in-house model message path exists.
- No systemd / no outside deps: nothing found that conflicts; ARGUS consumer is an in-process thread.
- ARGUS perf budget: ADR 0017 2% target is not met (+3.9% shipped). Adding Shadow traffic to the same rings makes that worse unless new kinds are count-on-use.
- ADR 0019 and ADR 0018 are PROPOSED; Field semantics resting on them inherit that status.

## 5. Could not verify
- Whether promotion (`rx_gen_promote`) emits an ARGUS event: UNVERIFIED.
- R15 attempt-2 G8 ratio; R15 counter overhead: UNVERIFIED.
- FORGE file lines cited from the polyglot audit, not re-read at physics HEAD.
- Evolution Arena / ADR 0018 contents: navigation only, not read.
