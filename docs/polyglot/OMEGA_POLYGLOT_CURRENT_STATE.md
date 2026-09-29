# Omega POLYGLOT: verified current state (lane A audit)

Date: 2026-09-29. Author: worker A (audit lane, Opus 5.5). Read-only audit; the only file written is this one.
Rule: implementation and evidence beat plans. README, ROADMAP and plan docs are cited only as "claims", never as
evidence. Every row below names repo + path(:line) + commit + the test or receipt that backs it.

## 0. Pins (what was audited)

| Repo | Commit | How read |
|---|---|---|
| omega | `9207ce3` (branch feat/mixed-algebra, MA-3), which contains origin/main `6fdc4c3` | this worktree (`polyglot/audit`, parent `1a9df4d`) |
| omega (OSC-0) | origin/osc/osc-0 `5c4b707` (draft PR #76) | `git show` |
| aienos | origin/main `603c91d` | fresh shallow clone in job scratch |
| physics | origin/main `f63a6ef` | fresh shallow clone in job scratch |
| aien-architecture | origin/main `8ec9b1d` | fresh shallow clone in job scratch |
| aien-sovereign-core | `63fe7a7` | `gh api` (tree + languages only) |
| aegis-runtime | `2bbce76` | `gh api` (tree + one file) |

Note: `9207ce3` is on no remote branch (`git branch -r --contains 9207ce3` is empty). The polyglot base exists only
in local worktrees until the mixed-algebra session pushes it.

Tests re-run for this audit (host, CPU, correctness only, one at a time, no timing):

| Command (omega 9207ce3) | Result |
|---|---|
| `make test-realize` | `MA3 test-realize PASS: 179684 checks, 0 failures, 10 realizations`, plain and ASan/UBSan builds |
| `make test-algebra` | `OMA ALGEBRA: 25709261 checks, 0 failures -> PASS` |
| `make test-costmodel` | `checks 68 failures 0` |
| `make test-program-id PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0` | `PASS 4516979/4516979` (incl. library 25/25, visor 10/10 regression gates) |
| physics `tests/run_forge_v2_gates.sh` (scratch clone f63a6ef) | KAT `51 passed, 0 failed`; gates `6 passed, 0 failed` |

Status vocabulary: IMPLEMENTED (code + passing test/receipt), PARTIAL (works for a narrow case), EXPERIMENTAL
(code on a side branch or stand-in), PLANNED (docs only), MISSING (neither), OBSOLETE (to retire), CONFLICTING
(contradicts a standing rule or another source).

---

## 1. Semantic objects and identity

| Item | Status | Evidence (omega 9207ce3 unless noted) |
|---|---|---|
| Canonical object encoding "OMG0" | IMPLEMENTED | `src/omega_types.h:14` magic; kinds VALUE..PROOF `:30-43`; encoder `src/omega_canonical.c` (key-sorted attributes). Covered by `test-program-id` regression gates (above). |
| SHA-256 | IMPLEMENTED (own C) | `src/sha256.c`, used by `src/omega_program.c:122`. No outside crypto library. |
| Type tags | PARTIAL | `src/omega_types.h:45-60`: UNIT, BOOL, UNSIGNED_INT, SIGNED_INT, BITVECTOR, BYTE, SEQUENCE, TUPLE, ADDRESS, RESOURCE, CAPABILITY_REF, EFFECT_INTENT_REF/RECEIPT_REF. No float, trit, tensor/matrix, int8-vector or modulus tag. Omega-X `(W, x) -> y` cannot be written as a typed OMG0 object today. |
| Program identity v2 | IMPLEMENTED (narrow) | domain `"omega.program.v2"` `src/omega_program.h:41`; id = SHA256(domain, body_root, in/out type, pre, post) `:83`, impl `src/omega_program.c:103-122`. Body = chain of unary u64 steps, ops ADD/SUB/MUL/AND/OR only (`omega_program.h:44,126`). Receipts `evidence/PROGRAM_ID/*.json` (schema `omega-program-id-qualification/1`, PASS, candidates `0a46063`, `19c692b`). |
| Semantic id for Omega-X | MISSING in the identity system | MA-3 names the operation only as a string plus C contract: `src/algebra/realize_common.h` (oracle `oma_rz_oracle` `:73`, bound `OMA_RZ_MAX_N` `:25`). POLYGLOT-0 uses a SHA-256 of spec section 1 as "contract digest" (`spec/polyglot-0.md` §8), which is outside program-id v2. Matvec/matmul also use private spec ids (`MatVecSemanticSpec`, `OmegaMatMulSpec`). |
| Realization identity | IMPLEMENTED (CPU AArch64 bytes) | `RealizationObject` + `omega_realize_compute_triple_id(semantic, machine, real)` `src/omega_realize.h:8,21`. |

## 2. Compiler, synthesis, language

| Item | Status | Evidence |
|---|---|---|
| General Omega compiler | MISSING | No source-to-machine-code compiler exists. |
| M6 "self-host" | OBSOLETE (fixed-output stand-in) | `src/omega_self_host.c:73-88`: the "compiler" writes hard-coded instruction words (ADD `0x8B010000`, SUB, RET) for two fixed graphs; C1==C2==C3 fixed point is a copy (`:228` memcpy of its own code). Receipt `evidence/omega_self_host_qualification_receipt.json` "QUALIFIED / PASS" certifies only that. OSC-0 class F. |
| `omega_program_realize` | MISSING | declared `src/omega_program.h:117`, no definition; `src/visor/visor_realization.c:437` reports "declared but not implemented in core". |
| Synthesis v0 / realize_synth | PARTIAL / OBSOLETE | `src/omega_synthesis.c` (336 lines), `src/omega_realize_synth.c` (201 lines); OSC-0 marks realize_synth a fixed-output stand-in (class F). |
| Surface language V0 | PARTIAL | `src/language/omega_{lex,parse,lower}.c` (1,117 lines). Lowers integer literals, bool, binary ops, unary fn chains to existing OMG0 objects and program-id v2; never evaluates or emits code (`src/language/omega_lower.h:1-12`). Test `make test-language` (`mk/visor-language.mk:3`, golden `tests/language/golden/v0.txt`). Not re-run here. |
| OSC-0 / OSC-0B compiler plan | PLANNED (PROPOSED, draft PR #76) | `OMEGA_SYSTEMS_CORE_CODE_AUDIT.md` (osc/osc-0 `5c4b707`) §III.6: parser extends V0 grammar, typed AST, ownership/borrow, effects, "Flow IR", backend = direct AArch64 through the in-repo encoder after hardening; C-emitting backend only as differential oracle. §III.7 order OSC-0 -> OSC-0B (hard gate) -> OSC-1 -> OSC-2. 4 Drake decisions open. |
| Own AArch64 encoder | IMPLEMENTED (scalar only) | `src/aarch64_encoder.c`: exactly 25 emitters (add/sub/mul/and/orr/eor/mov reg, movz/movk, ret, b, b.cond, cbz/cbnz, adr, ldr/str/ldrb/strb uoff, ldr/str post, x post, subs imm/reg). **No SIMD: no SDOT, no LD1, no vector registers** (grep for sdot/neon/simd = 0). Decoder `src/aarch64_decoder.c` mirrors it. Receipt `evidence/omega_aarch64_qualification_receipt.json`. |
| JIT execution | IMPLEMENTED | W^X mmap/mprotect `src/omega_exec.c`; sandbox with canaries `src/runtime/rx_omega.c` (OSC-0 §I.5). |

## 3. IRs (all host C, `src/runtime/`)

| IR | Status | Evidence |
|---|---|---|
| Action graph (rx_graph) | IMPLEMENTED | API `src/runtime/rx_graph.h:181-196` (node/data/guard/order/need/validate); receipts `evidence/ACTION_GRAPH/`; CI `test-action-graph`. |
| State projection (rx_projection) | IMPLEMENTED | `pj_compile` `src/runtime/rx_projection.h:217`; cognitive-state domain, not numeric. Receipts `evidence/SEM*`. |
| Capability query (rx_capq) | IMPLEMENTED | `cq_compile/cq_query/cq_rank` `src/runtime/rx_capq.h:227,256,263`; `CqCandidate` carries latency/energy/cost/confidence/evidence; no numeric-domain or language field. Receipts `evidence/CAPABILITY_QUERY/`. |
| Plan IR (rx_plan) | IMPLEMENTED | `rx_plan_decode/execute` `src/runtime/rx_plan.h:167,270`; receipts `evidence/PLAN_REUSE/`. |
| Typed result contracts (rx_contract) | IMPLEMENTED, exact integer only | field types integer/enum/digest; no tolerance/ULP (audit-A §1, re-checked header `src/runtime/rx_contract.h`). |
| Workflow fusion (rx_fusion) | IMPLEMENTED | `src/runtime/rx_fusion.c`; receipt dir per memory (#61). Not re-checked in depth. |
| Omega-X IR | MISSING | Omega-X exists only as a C function contract (`realize_common.h`), not as any IR node. |

## 4. C implementation scope

| Item | Status | Evidence |
|---|---|---|
| Omega is C only | IMPLEMENTED | `git ls-files` at 9207ce3: no `.rs`/Cargo in build; only `.rs` are R16 inventory fixtures `tests/r16_inventory/fixture/**` (test data, not built). ~94k lines C (zoom-out review; OSC-0 counted 87,486 at 193a7e7). |
| Build conventions | IMPLEMENTED | `Makefile:1-14` (`gnu11 -Wall -Wextra -Werror -O2`), `-include mk/*.mk`; algebra uses `-std=c11 -pedantic -march=armv8.6-a+dotprod+i8mm+sve` (`Makefile` OMA_RZ_ARCH, ~line 922). |
| Sanitizers | PARTIAL | Only mixed algebra: `test-algebra-asan`, `test_realize_asan` with `-fsanitize=address,undefined -fno-sanitize-recover=all` (`Makefile:890-953`). None for runtime, GPU, encoder. (Audit A "no sanitizers anywhere" is now stale.) |
| Compilers present | IMPLEMENTED | gcc 13.3.0, GNU as 2.42; **clang absent** (`which clang` empty). |

## 5. Rust still present and its removal

| Item | Status | Evidence |
|---|---|---|
| aienos Rust | CONFLICTING (No-Rust rule, migration pending) | 14 crates `crates/*/Cargo.toml` (aienos 603c91d), 57,872 lines `.rs`; C/asm 17,168 lines in `native/`. |
| Capability authority: Rust crate vs C port | PARTIAL (both live) | C twin `native/capability/aienos_capability.c`; Rust `crates/aienos-capability` + `-ffi` (2,129 lines). Kernel still re-exports the Rust crate: `crates/aienos-kernel/src/caps.rs:9` `pub use aienos_capability::{`, `crates/aienos-kernel/Cargo.toml:16`. C twin tested in CI: `.github/workflows/ci.yml:29` `make -C native/capability test`. Omega links the C authority pinned by `aienos.lock` (`d39dd5b`), enforced in `Makefile:185-201`. No in-tree Rust-vs-C differential harness or "1M random ops" evidence was found (grep of native/, docs/, evidence/): that claim in the migration plan is unverified here. |
| aien-sovereign-core | CONFLICTING | `63fe7a7`: Rust 3.1 MB, plus Python, CUDA (3 `.cu`), Mojo (8 files). Not linked into Omega/AIENOS. |
| aegis-runtime | OBSOLETE (planned retirement at R16) | `2bbce76`: Rust 316 kB + 1 Mojo file. Plan: `aien-architecture/docs/plans/RUST_TO_C_MIGRATION.md` §3 Step 2 "retired at R16, not ported". |
| Removal plan | PLANNED (direction decided, order PROPOSED) | `RUST_TO_C_MIGRATION.md:1,5` (decided 2026-09-27; step order awaits Drake): Step 1 crumbs -> omega `src/crumbline`, Step 2 retire aegis-runtime, Step 3.0 drop Rust capability crate, 3.1-3.4 boot/kernel/store/crypto. Nothing removed yet. |

## 6. Mojo

| Item | Status | Evidence |
|---|---|---|
| Mojo toolchain | IMPLEMENTED (installed) | `~/.pixi/bin/mojo` = static aarch64 ELF, `mojo --version` -> `Mojo 1.0.0 (ed45d567)`. |
| Mojo on PATH | CONFLICTING | `~/.local/bin/mojo` (first on PATH) is a **Python script** (`#!/home/drakestapleton/max-env/bin/python`, imports `mojo._entrypoints`). Lane B3 should call `~/.pixi/bin/mojo` explicitly. |
| Mojo in omega / aienos / physics | MISSING | 0 `.mojo` files in all three. Only mentions: `spec/polyglot-0.md`, `src/polyglot/omx_lang.h:16,33`. |
| Mojo elsewhere (prior art) | EXPERIMENTAL | aegis-runtime `mojo/aegis_simd.mojo`: `@export("...") ... abi("C")` C-ABI exports (header says "Mojo 1.1", host has 1.0.0: syntax may differ). sovereign-core `crates/aien-inference-abi/mojo/**` (inference kernels). Neither built or tested here. |
| Mojo doctrine | PLANNED (memory only) | No Mojo rule in aien-architecture doctrine/ADRs (grep). Drake's rule "Mojo is default, not dogma; leave MAX" lives in operator memory, not in repo authority. |

## 7. GPU / Blackwell (own encoder, codegen, submit)

| Item | Status | Evidence |
|---|---|---|
| Own SASS encoder + codegen (sm_121) | IMPLEMENTED | `src/omega_blackwell_{encoder,codegen}.c`; IR ops incl. IMAD/IADD3/LOP3/LDG/STG and `BW_IR_HMMA_F16`, `BW_IR_HMMA_BF16` (`src/omega_blackwell_codegen.h:34-35`). No IMMA/int8, no FP32 SIMT on main, no FP8/FP4. |
| QMD + pushbuffer submit, no CUDA | IMPLEMENTED | `src/omega_blackwell_qmd.c`; `src/omega_blackwell_submit.c:5` includes physics `m16_native.h`; physics pinned `physics.lock` = `fecbedb`, enforced by `check-physics-lock` (`Makefile:47`). |
| What it can run (silicon receipts) | PARTIAL | vector add `evidence/omega_blackwell_vector_qualification_receipt.json` ("QUALIFIED / PASS"); INT32 matmul stage 1 `..._matmul_stage1_receipt.json` (impl `645c13f`); FP16/BF16 HMMA stage 2 `..._matmul_stage2_receipt.json` (16x16x16, 32x16x32 ... "100% exact"). Size cap 1024 per dim `src/omega_blackwell_matmul.h:8-10`. Resident GPU seat `src/runtime/rx_resident_gpu.c` (R12-R15 receipts). |
| FP32 SIMT numeric | EXPERIMENTAL | open PR #32 `feat/forge-v1-numeric`, conflicts with main codegen (audit A §3). |
| Omega-X on GPU | MISSING | owned by MA-4 lane; POLYGLOT starts no GPU work (`POLYGLOT_AGENT_OWNERSHIP.md` lane D). |

## 8. FORGE (physics)

| Item | Status | Evidence (physics f63a6ef) |
|---|---|---|
| FORGE v1 realize/submit | PARTIAL (stand-in lowering) | `forge/forge_realize.c:68` "Lowering simulation"; submit pushes only a semaphore release `:255` (`m16_native_build_release`); never launches a compute kernel. Gates `tests/run_forge_gates.sh` (touch GPU, not re-run). |
| FORGE Substrate V2 data contract | IMPLEMENTED (host-only) | `forge/v2/forge_substrate_v2.{h,c}`; re-run 51/51 KAT, 6/6 gates. |
| V2 representation enum | PARTIAL for polyglot | `forge/v2/forge_substrate_v2.h:114-118`: DIGITAL_INT, DIGITAL_FLOAT, FIXED_POINT, ANALOG_VOLTAGE, ANALOG_CURRENT, SPIKE_TRAIN, OPTICAL_INTENSITY, OPTICAL_PHASE (+ bits). No ternary/crumb/bitplane/RNS, no language or toolchain axis. POLYGLOT's "representation" (int8 row-major, 2-bit crumb) and "language" axes have no FORGE home. |
| V2 execution path | MISSING | V2 spec §1 non-goals: no hardware, no submission. |

## 9. AEGIS

| Item | Status | Evidence |
|---|---|---|
| Resident AEGIS (R8) | IMPLEMENTED | `src/runtime/rx_aegis.{h,c}` (345 lines): authority as world objects, capability slots validated by native AIENOS authority (`rx_aegis.h:1-10`); receipts `evidence/R8/`. |
| Native capability authority (C) | IMPLEMENTED | aienos `native/capability/` (see §5); Omega binds via `src/runtime/aienos_cap.h`, `rx_native_bind.c`. |
| FORGE v1 AEGIS token | PARTIAL | SHA-256 over compiled-in constant, not a keyed MAC (audit B §1.1). |
| aienos-aegis Rust crate | CONFLICTING / undecided | `crates/aienos-aegis` host-side; migration plan §3 table "Not decided". |
| AEGIS role for polyglot | MISSING | Omega-X is effect-free and capability-free (`spec/polyglot-0.md` §1), so AEGIS is not on the POLYGLOT-0 path. |

## 10. World / resident state, Cortex

| Item | Status | Evidence |
|---|---|---|
| Shared reaction world | IMPLEMENTED | `src/runtime/rx_world.{h,c}` (2,020 lines): dependency index, snapshot, atomic publish, causal crumbs (`rx_world.h:1-10`); R1-R15 receipts `evidence/R1..R15`. |
| Shared-world ABI | IMPLEMENTED | `src/runtime/omega_shared_world_abi.h` (offset-based, OSC-0 §I.5). |
| Generations / promotion | IMPLEMENTED | `src/runtime/rx_generation.{h,c}` (R9 barrier), u64 cap generation (#71, `evidence/EFFECT_CAP64/`). |
| Cortex | IMPLEMENTED (host reference) | `src/runtime/rx_cortex.{h,c}` (193 lines): append-only typed memory, digest per object (`rx_cortex.h:1-12`). Rust `crates/aienos-cortex` also exists (undecided). |
| Omega-X in the World | MISSING | MA-3 and POLYGLOT run as standalone test binaries, not as reactions. |

## 11. Evidence and provenance

| Item | Status | Evidence |
|---|---|---|
| `omega_evidence` library | IMPLEMENTED | `src/omega_evidence.h:48-74`: run-scoped path, write-by-digest, `run_id`, `run_commit`, `tree_dirty`, `physics_commit`. |
| Receipt convention (runtime gates) | IMPLEMENTED | Named by SHA-256 of own bytes: `evidence/EMPIRICAL/73bfa1b6....json` (schema `OMEGA_EMPIRICAL_OPTIMIZER_V1`, candidate `3d20125`, 44 checks, 0 failures). Immutability guard `.github/workflows/evidence-immutable.yml`. |
| MA-3 receipts | IMPLEMENTED (different convention) | `evidence/MIXED_ALGEBRA/ma3_bench_run{1,2}.json` (schema `OMEGA_MIXED_ALGEBRA_MA3_BENCH_V1`, run_id, run_commit `c262666`, tree_dirty false, bench_binary_sha256, host core/thermal/loadavg), `ma3_select_receipt.json`. Fixed names, not digest names; not written through `omega_evidence`. |
| Shared JSON schema | MISSING | Each gate writes its own shape; no schema file. POLYGLOT §8 defines yet another shape. |
| FORGE V2 ExecutionEvidenceV2 | IMPLEMENTED (data type only) | physics `forge/v2`, `provenance_class` PHYSICAL / SIMULATED_DEVELOPMENT. No Omega code emits it. |
| Evidence roots / Merkle | PARTIAL | per-object digests and corpus digest lists (`evidence/m*_corpus_digests.txt`); no single evidence root across gates. |

## 12. Realization selection

| Item | Status | Evidence |
|---|---|---|
| Matvec living kernel | IMPLEMENTED | `src/omega_matvec.h:15-18` (SCALAR, UNROLL2, UNROLL4_DUAL) + quad4 `src/omega_matvec_quad.c:11`; u64 wrap oracle `omega_matvec_reference`. |
| Cognitive routing rx_route | IMPLEMENTED, not numeric | ops RECOGNIZE/PLAN only `src/runtime/rx_route.h:48-50`; hardware bits `:61`; precision APPROX/EXACT `:63`; admission `rx_route.c:323`. `RX_COG_HW_GPU` unused outside the header. |
| Empirical cost model rx_costmodel | IMPLEMENTED, matvec only | `RX_CM_ARMS 8`, `RX_CM_OPS 1 /* matvec only today */` `src/runtime/rx_costmodel.h:36-37`; observe/decide/serialize `:154,165,180`. Merged omega#60 `c0edef0` (ancestor of main). `test-costmodel` 68/0 (re-run). Receipts `evidence/EMPIRICAL/*.json`. |
| MA-3 selector `oma_select` | EXPERIMENTAL (stand-in) | `src/algebra/oma_select.h:1-4` "STAND-IN: not wired to rx_costmodel"; receipt `ma3_select_receipt.json` label "stand-in selector, not wired to rx_costmodel"; 72 decisions, 64 agree, 8 ties, 0 disagree (`spec/mixed-algebra-ma3.md:198-200`). |
| Omega-X realizations | IMPLEMENTED (C + intrinsics) | 10 impls `src/algebra/realize_common.h:76-80`: R1_plain, R1_sdot, R1_sdot_il, R1_smmla, R2_bitplane, R2b_lut, R2c_crumb, R3_sparse, R4_rns, R5_dense5; `test-realize` 179,684/0 plain + ASan/UBSan (re-run). |
| Polyglot registry | PLANNED (header only) | `src/polyglot/omx_lang.h:14-34` (`omx_candidate`: impl, language, toolchain, compiler_derived, toolchain_only, source; lanes asm/encoder/mojo). No `.c`, no tests/polyglot yet. `mk/polyglot.mk` only declares phony targets. |

## 13. Physical cost model, empirical learning, energy

| Item | Status | Evidence |
|---|---|---|
| Bayesian ridge per (op, core, pressure) cell | IMPLEMENTED | `rx_costmodel.h:36-45`; features M, N, state_bytes as u64 (`*8`) `:57-71`; link-time purity check (audit A, `Makefile:704-722`). |
| Calibration / drift handling | IMPLEMENTED (synthetic) | `test-costmodel` output: 80% coverage 0.323 -> 0.820 after recalibration; blob 28,112 bytes. |
| Energy meter | IMPLEMENTED (test-side) | hwmon `aien_spbm` read in `tests/runtime/rx_empirical_optimizer.c` (audit A cites `:345-371`) and `r15_measure.c`. Not a library. |
| Language / representation axis in cost model | MISSING | arms are a fixed table of 8; no language, toolchain, representation or pack-cost feature; widening changes the serialized blob. |

## 14. Benchmark infrastructure

| Item | Status | Evidence |
|---|---|---|
| Core pinning, idle-core choice, thermal, loadavg | IMPLEMENTED (MA-3 bench) | `tests/algebra/bench_mixed_algebra.c:512` `sched_setaffinity`; thermal `:71-72`; host block in `ma3_bench_run1.json`. |
| schedstat run-delay floor | IMPLEMENTED (MA-3, empirical, R11, plan-reuse) | `tests/algebra/bench_mixed_algebra.c:10,52` reads `/proc/thread-self/schedstat`; blocks with run_delay > 2% retried (receipt text). Also `tests/runtime/rx_empirical_optimizer.c`, `rx_r11_aien.c`, `rx_plan_reuse.c`. |
| cpufreq governor / frequency | MISSING | not recorded by MA-3 bench (grep `cpufreq` = 0). POLYGLOT §7 requires it: lane F must add. |
| Quiet flag `~/workspace/.spark-quiet` | PARTIAL (convention only) | defined in `~/workspace/OMEGA-SESSIONS-COORDINATION.md:11-14` (outside any repo); no omega code checks it (grep = 0 outside the polyglot ownership doc). Flag absent at audit time. |
| Interleaved round-robin, N>=20, MAD, compile time, code size, peak RSS | MISSING as shared harness | MA-3 bench has its own loop; nothing reusable as a library. POLYGLOT lane F builds `omx_bench*`. |

## 15. Language, compiler and unsafe boundaries

| Boundary | Status | Evidence |
|---|---|---|
| C <-> AIENOS C authority | IMPLEMENTED | 19 `aienos_cap_*` functions via hand-copied `src/runtime/aienos_cap.h`; only size and two offsets checked (OSC-0 §I.6, `rx_native_bind.c:10-16`). |
| C <-> physics nvrm/m16 | IMPLEMENTED | physics sources compiled into Omega; `Nvrm` layout is a de-facto ABI (OSC-0 §I.6). |
| C <-> JIT code | IMPLEMENTED | W^X pages, canaries (OSC-0 §I.5). |
| C <-> Rust | CONFLICTING | only inside aienos (`aienos-capability-ffi`); no Rust in Omega. |
| C <-> Mojo | MISSING | no build rule anywhere in omega. Prior art: aegis-runtime `@export ... abi("C")`. |
| C <-> hand assembly | MISSING in omega | no `.s`/`.S` under omega `src/` today (physics has 4.4k lines asm). POLYGLOT lane B1 owns `src/polyglot/asm/**`. |
| Unsafe hazard counts | IMPLEMENTED (measured once) | OSC-0 §I.3 at `193a7e7`: 312 `void*`, 53 mapped-pointer arithmetic sites, 75 `volatile`, 17 `_Static_assert`. |
| OSC-0B memory/aliasing/unsafe rules | PLANNED (PROPOSED) | OSC-0 Part II / III.3-III.5, awaiting Drake. |

## 16. Mixed-algebra Omega-X (9207ce3)

| Item | Status | Evidence |
|---|---|---|
| Trit, Z3, packing, absmean quantization reference | IMPLEMENTED | `src/algebra/oma_{trit,z3,pack,quant}.c`; `test-algebra` 25,709,261/0 (re-run). |
| Omega-X contract (pack/run ABI, oracle, errors, plan resources) | IMPLEMENTED | `src/algebra/realize_common.h:14-80`; spec `spec/mixed-algebra-ma3.md`. |
| 10 realizations bit-exact | IMPLEMENTED | §12 above. |
| Benchmark + stand-in selector receipts | IMPLEMENTED (2 runs) | `evidence/MIXED_ALGEBRA/*`. |
| Pushed / merged | MISSING | 9207ce3 on no remote; not on omega main. |
| MA-4 (GPU) | PLANNED | external lane. |

---

## 17. What POLYGLOT-0 reuses vs must build

| Need (spec/polyglot-0.md) | Reuse as-is | Must build |
|---|---|---|
| Semantic operation + oracle | `oma_rz_oracle`, `OMA_RZ_MAX_N`, error codes (`realize_common.h`) | nothing; contract digest = SHA-256 of spec §1 (new, outside program-id v2) |
| C candidates | `R1_sdot` (int8), `R2c_crumb` (crumb), `R1_plain` baseline; pack/run ABI | `-O3 -mcpu=native` build variant (MA uses `-O2 -march=armv8.6-a+...`) |
| Registry with language metadata | `omx_lang.h` header | `omx_lang.c` + per-lane tables (lanes B1/B2/B3/F) |
| Hand assembly | GNU as 2.42 | all asm kernels, derivation record, guard-page harness |
| Own-encoder candidate | `src/aarch64_encoder.c` style (scalar ops, byte writer) | every SIMD encoding (SDOT, LD1, vector MOVI/ADD/ADDV, etc.): encoder has 0 SIMD instructions and is read-only for this program, so lane B2 needs its own emitters in `src/polyglot/omx_encoder*` plus a GNU-as byte diff |
| Mojo candidate | Mojo 1.0.0 ELF at `~/.pixi/bin/mojo`; aegis-runtime C-ABI export pattern | `.mojo` kernels, C glue, make rule; avoid the Python `mojo` wrapper on PATH |
| Verifier | MA-3 shape grid in `tests/algebra/test_realize.c`; ASan/UBSan pattern (`Makefile:890-953`) | 20k random cases, unaligned pointers, guard pages |
| Benchmark | MA-3 pinning, idle X925 choice, schedstat retry, thermal, receipt host block | cpufreq/governor capture, round-robin interleave, N>=20, MAD, compile time, text size, peak RSS, pack break-even, quiet-flag check |
| Receipts | `omega_evidence` (run_id, commit, dirty, digest naming) | POLYGLOT JSON shape (§8); mapping to `rx_costmodel` later (POLYGLOT-1) |
| Selection + explanation | `oma_select` policy (exact filter, median, noise band, TIE) as reference | `polyglot_explain` (deterministic, receipts-only) |
| Rust-vs-C history (lane H) | aienos C authority + Rust crate both present; CI runs C tests | a black-box harness; no existing differential evidence found in-tree |
| Integration into Omega proper | none | semantic type for Omega-X, `rx_costmodel` language/representation arm, World reaction (all POLYGLOT-1+) |

## 18. Conflicts and stale docs

| # | Conflict / stale claim | Sources | Truth found |
|---|---|---|---|
| 1 | R16 status: ROADMAP says PASS, execution plan says IN PROGRESS / draft | aien-architecture `doctrine/ROADMAP.md:282` "R16 ... PASS ... #68 merged (6fdc4c3)"; `CURRENT_EXECUTION_PLAN.md:45` "IN PROGRESS ... #68 draft" | Both wrong in part. #68 is merged (6fdc4c3) but it is methodology + inventory only: `spec/r16-orchestrator-retirement.md:3` "Status: METHODOLOGY"; only G1/G2 inventory done (`evidence/R16/inventory.json` result PASS, 0 unclassified); gates R16-G3..G8 (`:194-199`) have no receipt; no `evidence/R16/<sha256>.json`. ROADMAP overclaims R16 PASS. |
| 2 | Runtime edit freeze | `CURRENT_EXECUTION_PLAN.md:540` "No edits to omega/src/runtime/ until R16 closes" | Still formally in force (R16 not closed). POLYGLOT-1 wiring into `rx_costmodel` would need an explicit lift. |
| 3 | FORGE V2 KAT count | `ANALOG_REALIZATION_CURRENT_STATE.md:30`, `ANALOG_REALIZATION_TEST_PLAN.md:90` say 47/47 | 51/51 at physics f63a6ef (re-run). |
| 4 | ADR 0018 says ARCH-0017 "reserved, not yet on main" | `docs/adr/0018-*.md:6,158` | `docs/adr/0017-argus-defensive-plane.md` is on main. |
| 5 | OSC-0 "build does not enforce aienos.lock" | OSC-0 audit Pins section (`193a7e7`) | Now enforced: omega `Makefile:185-201` extracts the lock commit by `git archive`. |
| 6 | Migration plan "aienos CI never builds native/capability" | `RUST_TO_C_MIGRATION.md` §4 | Stale: aienos `.github/workflows/ci.yml:29` runs `make -C native/capability test`. |
| 7 | Migration plan "capability proven by 1M random ops" | `RUST_TO_C_MIGRATION.md` §4 item 3 | No differential harness or receipt found in aienos tree; treat as unverified. |
| 8 | Audit A "no sanitizers anywhere" | `~/.claude/jobs/c8884ded/tmp/audit-A-omega.md` §4 | Stale since MA: ASan/UBSan targets for algebra exist. |
| 9 | Zoom-out "physics py 5.3k" | `~/handoffs/2026-09-29-zoom-out-review.md` table | physics has 5 `.py` files, 1,717 lines (likely a byte/line mix-up). |
| 10 | Mixed-algebra base not on any remote | POLYGLOT spec base 9207ce3 | local-only; polyglot results would rest on an unpublished base. |
| 11 | Program identity cannot name Omega-X | `omega_program.h:44` unary u64 ops only | Polyglot contract digest is a parallel identity until types/program id are extended. |
| 12 | Three machine-identity shapes | `CqCandidate.machine_id` u32 (rx_capq), FORGE V2 32-byte `machine_identity`, aienos OS-0010 `MachineId` (Proposed) | unreconciled (audit B §3); POLYGLOT receipts add a fourth (cpu model + core + cluster). |
| 13 | CUDA in sovereign-core | `aien-sovereign-core` 3 `.cu` files | conflicts with "no CUDA" decision; not linked into Omega. |

### Python remaining (Drake forbids Python): all CONFLICTING

| Repo @ commit | Python | Wired in? |
|---|---|---|
| omega 9207ce3 | `tools/m19r_qualify.py` (250 lines), `tools/test_m19r_qualify.py` (57) | not referenced by Makefile/mk/CI (grep) |
| omega CI | `.github/workflows/rx-host.yml:212,230,252` three inline `python3 - <<'PY'` blocks (receipt hashing/JSON checks) | yes, runs on every CI job that reaches them |
| physics f63a6ef | `generate_physics_audit.py`, `m2_build.py`, `run_milestone2_gates.py`, `seam1_physics_audit.py`, `seam2_physics_harness.py` (1,717 lines) | called by `tests/run_m15_gates.sh`, `tests/run_m16_requalification.sh` |
| aienos 603c91d | `vendor/libc/etc/libc-util.py` (vendored crate) | vendored, not run by build scripts found |
| aien-sovereign-core 63fe7a7 | 11 `.py` | repo slated for migration |
| aien-architecture 8ec9b1d | none | n/a |
| host toolchain | `~/.local/bin/mojo` launcher is a Python script | only if invoked by name `mojo` |

---

## 19. Ten findings that matter most for POLYGLOT

1. Omega-X is real and solid: 10 bit-exact C realizations, 179,684 checks plain + ASan/UBSan, re-run PASS.
2. Omega-X has no Omega semantic identity: program-id v2 covers only unary u64 chains; POLYGLOT's contract digest is
   a parallel identity.
3. Omega's own AArch64 encoder has 25 scalar instructions and zero SIMD; lane B2 must write SDOT/LD1/vector encodings
   from scratch outside the (read-only) encoder file.
4. There is no general compiler; M6 self-host is a fixed-output stand-in; `omega_program_realize` is undefined.
5. Selection today: `rx_costmodel` (real, matvec-only, 8 fixed arms, no language axis) and `oma_select` (stand-in, not
   wired). Neither knows language, toolchain or representation.
6. Benchmark hygiene pieces exist in MA-3 (pinning, schedstat retry, thermal) but cpufreq capture, interleaving,
   MAD, compile time and code size do not; the quiet flag is a convention no code checks.
7. Mojo 1.0.0 works via the pixi ELF; the `mojo` first on PATH is a Python wrapper. No Mojo code in omega.
8. Rust is still live in aienos (57.9k lines; kernel still re-exports the Rust capability crate) though the C twin is
   tested in CI; no in-tree differential Rust-vs-C evidence found.
9. R16 is not closed despite ROADMAP "PASS"; the `omega/src/runtime/` edit freeze still formally blocks POLYGLOT-1
   wiring into the cost model.
10. Python remains in omega CI (3 inline blocks), omega tools (2 files) and physics (5 files, called by gate scripts).
