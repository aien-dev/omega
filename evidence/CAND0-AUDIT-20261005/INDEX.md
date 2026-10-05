# CAND0-AUDIT-20261005: evidence audit index against CAND-0

Written by lane L2-EVID of the 2026-10-05 overnight campaign. Same convention as `evidence/COMPOSITION-2/INDEX.md`
and `evidence/REQUAL-3108fc2/INDEX.md`: receipts are never edited; this index says which are authoritative and
what each does and does not show. Existing INDEX.md files are not touched, because `evidence-immutable` (CI)
refuses any modification of an existing file under `evidence/`. This is a new file, additions only.

## Candidate (campaign ledger, frozen 2026-10-05 03:25Z)
omega `e5593ae488fcb9038557a4317a07faa7625649a3` (pins physics `6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf`),
aien-sovereign-core `0c1d249f2d119c7c2d726a2d252dc913e6185d28`, aienos `9d41efc9d1bea31be0640ca70eeeec9d1d000fad`,
aien-protocols `3a4cdbe8d360a2e1ddb3e8b108684048b9d4ba10`, aien-architecture `c5d80978c7a22cc039e67def9745cede5010f388`.
Note: `aien-architecture/qualification/candidates/CAND-0.toml` (2026-10-01, draft) is a different, older candidate with other SHAs.

## Classes
- VALID-FOR-CAND0: binds CAND-0, or binds a SHA whose exercised sources are byte-identical to CAND-0 (stated per row).
- VALID-HISTORICAL-ONLY: a real result for an older SHA whose exercised sources differ, or that predates composition changes.
- INCOMPLETE: something required is missing (named per row).
- INVALID: not usable as a qualification claim (defective script, dirty diagnostics, wrong backend, superseded and failed audit).
  Failure records are published anyway so nothing is lost.

## What the audit found (read first)
1. No chip result in the endurance, ladder, composition, World, TRUST-1/M5 or real-model areas is labelled with CAND-0 e5593ae.
   For attention, `FB1-CUT5-cd80bf4` is VALID-FOR-CAND0 by byte-identical sources (OBSERVED: `git diff --stat cd80bf4 e5593ae -- src/ tests/ tools/ Makefile`
   lists only `src/dual/*`, `tests/dual/*` and `.crumb` files; sha256 of the three copied attention sources equals the CAND-0 blobs).
2. Source drift since the last ladder requalification (3108fc2) is in the runtime itself (rx_world, rx_jspace, rx_compose, rx_cortex,
   omega_accelerator_world.c, physics nvrm.c and m16_native.c). R1-R16 results are therefore historical.
3. The ledger claim "M19 endurance 21 runs / 7 h / 0 failures" is true but each run is 11-12 s; it is not a soak. Three real M19 FAIL runs are preserved here.
4. Timing evidence was taken with other GPU users possibly resident (the MAX server was running; ollama and atlas python were resident at 2026-10-05 03:15Z).
   No log records the GPU process list, so sharing is UNVERIFIED for 2026-10-04 runs.
5. The FB1 receipts are the test programs' own JSON sealed by a `SHA256SUMS` file, not ADR 0028 canonical `EvidenceReceiptV1`. The M20 and M19-main receipts are digest-named
   (sha256 of bytes, or json_canon digest; both checked, see below).
6. Plan documents (CURRENT_EXECUTION_PLAN.md) still say R15 and R16 passed; REQUAL-3108fc2 recorded both FAIL. Discrepancy recorded, not resolved here.

## Published by this change (new directories, additions only)
Raw files as produced; ELF binaries outside `blobs/` are not copied, their sha256 and size are in `excluded-binaries.txt`
(the run directories' own `hashes.sha256` files still list them, so `sha256sum -c` on those lists reports exactly those files missing and nothing else;
OBSERVED when publishing). `FB1-CUT5-b016322/SHA256SUMS` has one bogus self-line (hash of an empty file, script artifact); the 8 payload files verify.
Digest-named receipts: all `<sha256>.json` receipts under `M19-main-*` verify with `jq 'del(.receipt_digest)' | json_canon --sha256` (10/10); the M20 receipts and blobs equal sha256 of their bytes.
Directories: FB1-CUT5-cd80bf4, FB1-CUT5-b016322, FB1-ATTN-DIV, FB1-CUT1B-{4345406,efb4cfc,f629798}, FB1-CUT3B-3ed1f36, FB1-CUT3C-{0f086a7,21f995d,86bbc5f},
FB1-CUT3D-{0213f49,02dfa2e,02dfa2e-drift,diag1,e319ee0}, FB1-CUT4-{0d0241a,ac0d42c}, FB1-CUT4B-{0396844,88f3f59-negctl,88f3f59-probe}, FB1-CUT6A,
KV-ADMISSION-20261003, M20-TENSOR-GB10, M19-ENDURANCE, M19-main-{40d1ea3,40d1ea3-run1-FAIL-231810Z,40d1ea3-run2-FAIL-20261003T103400Z,87e8793,f523edd},
CHIPWAIT1-6fa4d97, CHIPWAIT1C-1a853f9, TRUST1-M5-20261003-130aa20 (+ -run1), PHYSICS27.

## Local-only evidence that is NOT published, and why
- CHIPWAIT-ASTRA-20261003, CHIPWAIT-DIAG-20261003, CHIPWAIT-FIX-20261003, STALEGEN-DIAG-20261003, L6C: dirty-tree diagnostics (qualification=false), about 35 MB.
- GPU-MATMUL-API-2ea8e67: no SHA bound in any file, superseded by the published 6cd2dc0 run4.
- E1-* and OMEGA-NUMERIC-0 digest-named receipts not yet in `evidence/E1-*`: not copied here to keep this change to new directories.
- STRICT-REAL-MODEL-20261003: CUDA backend, run002 already in sovereign-core docs/campaigns; run001 (937 B) not copied.
- R-LADDER-1720a8d: empty directory, NOT_RUN.
- WORLD-UNCACHED per-rep logs (1.2 MB) and the CHIPWAIT campaign raw logs (1.5 MB): summaries are published under existing directories.
- aienos ALLEN raw serial logs (33 files, 1.9 MB) and the PR #260 mutant logs: in another session's scratch directory, not copied by this lane.
- `sc3-omega-parity-gb10-chip` log: `~/workspace/test-queue-logs/sc3-omega-parity-gb10-chip-194525.log`; plain log, not copied.

## Table (claim, candidate binding, command, evidence class, verdict, path)
| claim | candidate binding | command | evidence class | verdict | path |
|---|---|---|---|---|---|
| Attention battery + head/kv sweep + mutants + timing on GB10: OMEGA_GPU_ATTENTION_PASS, 108 + 73 + 2 checks, mutants NO_MAX 1/1, KV_HEAD 11/11, SLOT 9/9, NO_RESCALE 4/4, Q_ROW 6/6, OUT_ROW 6/6 | omega cd80bf4 (PR head, 0 uncommitted at start; squashed as CAND-0 e5593ae); physics 6d7cf0d = CAND-0. omega_gpu_attention_api.{c,h}, gpu_attention_test.c, run_gpu_attention_chip.sh and Makefile byte-identical to e5593ae (OBSERVED git diff; only dual/ and .crumb files differ). Not CAND-0 sha-labelled. | `tools/run_gpu_attention_chip.sh` (queue lane fb1-cut5-om2-cd80bf4, 2026-10-04T19:19Z): sim first, then chip, sweep, timing | HARDWARE (device_opens=1, chip_ns>0); sim.log separate | VALID-FOR-CAND0 by source identity. Caveats: not an ADR 0028 receipt (script SHA256SUMS only, 12/12 OK); tree-clean recorded at start only; timing taken while the MAX server was resident (GPU possibly shared; 1.204 ms median, gate 3.0 ms) | evidence/FB1-CUT5-cd80bf4/ |
| FB1 cut 4b: matmul 18, elementwise 31, attention 46, session timings, GPU-open probe | omega 0396844, physics 6d7cf0d; matmul api, elementwise api, gpu_session, wait, codegen identical to CAND-0; attention differs | per-dir run files | HARDWARE | VALID-FOR-CAND0 for matmul / elementwise / session only; attention portion HISTORICAL | evidence/FB1-CUT4B-0396844/ (+ -88f3f59-probe, -negctl) |
| sc3-omega-parity-gb10-chip: 4/4 ignored parity tests PASS on GB10 | sc 0fcb3ad (INFERRED), omega cd80bf4, physics 6d7cf0d; sc crates identical to CAND-0 (omega-gpu ffi.rs 5 comment lines) | queue lane only; no manifest or spec found; de-facto spec crates/aien-inference-abi/tests/omega_backend_parity.rs | HARDWARE (native, fallbacks=[], chip_errors=0), random data not real model | VALID-FOR-CAND0, weak binding: plain log, no sha lines, no sealed receipt | ~/workspace/test-queue-logs/sc3-omega-parity-gb10-chip-194525.log (local-only) |
| FB1 cut 5 first cut, 45 checks | omega b016322; attention sources differ materially | run_gpu_attention_chip.sh | HARDWARE | VALID-HISTORICAL-ONLY (SHA256SUMS self-line is an empty-file hash, script artifact, 8 payload files OK) | evidence/FB1-CUT5-b016322/ |
| FB1 attention divergence fix: red 13/86 fail, green 86/86 x5, timing PASS | omega f8d0aca; fix merged as 3d4648c (ancestor of CAND-0) | run.sh in dir | HARDWARE | VALID-HISTORICAL-ONLY (no seal; 2 ELF binaries not published) | evidence/FB1-ATTN-DIV/ |
| FB1 cut 1b matmul: PASS (4345406); sweep FAIL 1/18 (efb4cfc); sweep FAIL 4/15 (f629798, 20 s stall) | omega 4345406 / efb4cfc / f629798 | run files | HARDWARE | 4345406 VALID-HISTORICAL-ONLY; efb4cfc and f629798 INVALID as qualification, kept as failure records | evidence/FB1-CUT1B-* |
| FB1 cut 3b/3c/3d real-model parity + gate (TinyLlama) | sc 3ed1f36 .. e319ee0, omega 5e29b82 .. 3d4648c | run.sh in dir | HARDWARE | PASS dirs (3B-3ed1f36, 3C-0f086a7, 3C-86bbc5f, 3D-e319ee0): VALID-HISTORICAL-ONLY. FAIL dirs (3C-21f995d, 3D-0213f49, 3D-02dfa2e, -drift, -diag1): INVALID as qualification, kept | evidence/FB1-CUT3*/ |
| FB1 cut 4 elementwise 30 / 28 checks | omega 0d0241a / ac0d42c | run files | HARDWARE | VALID-HISTORICAL-ONLY (pre-4b) | evidence/FB1-CUT4-0d0241a/, evidence/FB1-CUT4-ac0d42c/ |
| FB1 cut 6a real-model strict gate PASS, no CUDA | sc bfeecdb, omega pin 3d4648c; omega attention differs (dedupe + counters) | run.sh | HARDWARE (real TinyLlama) | VALID-HISTORICAL-ONLY. NO real-model gate exists at CAND-0 | evidence/FB1-CUT6A/ |
| PREFILL-E2E-1 (N=1..500) and E2E-2, FAIL-N2 run | sc c60724b / bd6acd4c; gb10: NOT_RUN in every receipt | prefill_e2e2*.gate | CPU (ReferenceCpuBackend) | VALID-HISTORICAL-ONLY (host evidence, nothing about native GPU inference) | evidence/KV-ADMISSION-20261003/ |
| M20 tensor GB10: 11 receipts, 9 PASS, 2 FAIL | omega up to f633e34, physics pin e95e3ed | tools/m20_receipt.sh | HARDWARE | VALID-HISTORICAL-ONLY (numeric sources and physics.lock changed since; receipt names = sha256 of bytes, 20 blobs OK) | evidence/M20-TENSOR-GB10/ |
| GPU matmul API runs 1-3: FAIL, FAIL, PASS (5 checks) | no sha in any file | run files | HARDWARE | INCOMPLETE (not bindable; superseded by 6cd2dc0 run4). NOT in this PR (dir GPU-MATMUL-API-2ea8e67 stays local-only) | ~/workspace/evidence-out/GPU-MATMUL-API-2ea8e67/ (local-only) |
| GPU matmul API run4 15/15; GPU_ENGINE gb10-1 PASS (ADR 0028 receipt) | omega 6cd2dc0 / 2eb912d, physics 6d7cf0d | per dir | HARDWARE | VALID-HISTORICAL-ONLY, already published | evidence/GPU_MATMUL_API/, evidence/OMEGA-GPU-ENGINE/gb10-1-2eb912d/ |
| Strict real-model gate x2 PASS | sc strict worktree, no sha in receipt; cuBLAS backend (deleted in FB-1 cut 6) | see README | HARDWARE (CUDA) | INVALID for the native claim; historical record only; run002 already in sc docs/campaigns/gb10-4-strict-real-model/ | sovereign-core docs/campaigns/gb10-4-strict-real-model/; ~/workspace/evidence-out/STRICT-REAL-MODEL-20261003/ (local-only, 20 KB) |
| CHIPWAIT campaign 3/3 PASS | omega 7ea5c4a (squashed into 4e82e50), physics 6d7cf0d = CAND-0; wait/world sources identical to CAND-0, omegatool binary differs | tools/chipwait_campaign.sh (3x M19R) | HARDWARE | VALID-HISTORICAL-ONLY (strong transfer by source identity, not strictly CAND-0). JSON already published; raw logs now published only for 1/1C records, campaign raw logs 1.5 MB local-only | evidence/CHIPWAIT/campaign-2026-10-04/ |
| M19 endurance "21 runs / 7 h / 0 failures" | omega 4e82e50, physics 6d7cf0d | `omegatool --run-m19-gates` loop, 20 min cadence | HARDWARE | VALID-HISTORICAL-ONLY and INCOMPLETE as endurance: each run 11-12 s (about 4 min chip time in 7 h wall), 18 + 157 gates only, no soak, no receipt, no per-run clean-tree check, no binary hash for the loop | evidence/M19-ENDURANCE/ |
| M19R + Gate 14 + numeric + forge PASS | omega f523edd, physics 6d7cf0d | tools/m19r_qualify.sh --record | HARDWARE | VALID-HISTORICAL-ONLY (pre-CHIPWAIT code path); receipts re-hashed with json_canon, 10/10 match | evidence/M19-main-f523edd/ |
| Preserved M19 FAILs: 40d1ea3 run1 (QUEUE_WRAP, CLEAN_CLONE), run2 (m16_concurrent hang, exit 5), 87e8793 (WORLD_STALE_GENERATION_MARKER_REFUSED) | omega 40d1ea3 / 87e8793 | m19r_qualify.sh | HARDWARE | VALID-HISTORICAL-ONLY, real FAILs, preserved (three, not two) | evidence/M19-main-40d1ea3*/, evidence/M19-main-87e8793/ |
| CHIPWAIT-1 (INVALID: run1 evidence destroyed by make clean) and CHIPWAIT-1c (FAIL, run3 operator-stopped) | omega 6fa4d97 / 1a853f9, physics e95e3ed | campaign scripts | HARDWARE | preserved failure records; INVALID as qualification | evidence/CHIPWAIT1-6fa4d97/, evidence/CHIPWAIT1C-1a853f9/ |
| CHIPWAIT Astra / diag / fix, STALEGEN-DIAG (qualification=false, patched dirty trees) | various | scratch | HARDWARE | INVALID as qualification, root-cause record. NOT published (about 35 MB) | ~/workspace/evidence-out/CHIPWAIT-{ASTRA,DIAG,FIX}-20261003, STALEGEN-DIAG-20261003, L6C (local-only) |
| M18 uncached A/B: base 1/20 FAIL, fix 0/20 | omega b20ff9e (merged ccdfef3), physics 6d7cf0d; submit.c differs 36 lines from CAND-0 | ab_campaign.sh | HARDWARE under CPU load | VALID-HISTORICAL-ONLY (README says the A/B alone does not prove the fix) | evidence/M18-UNCACHED/b20ff9e/ (published) |
| World lifecycle gates 12/12 PASS, M18 flake unresolved | omega 6ab33f7, physics 6d7cf0d = CAND-0 | see README | HARDWARE | VALID-HISTORICAL-ONLY (accelerator_world, world_gates, gpu_wait differ from CAND-0) | evidence/WORLD-UNCACHED/6ab33f7/ (published; per-rep logs 1.2 MB local-only) |
| E1 chip closure, numerical PASS | chip on fb36109, main 40d1ea3, physics e95e3ed (before physics #28 made pushbuffer/GPFIFO uncached) | tools/e1_combine.sh | HARDWARE | VALID-HISTORICAL-ONLY | evidence/E1-CLOSURE/e7851c69...json (published) |
| E1 other chip dirs (DIVSQRT, REDUCE, REDUCE-MUTANT, TRANSC-GB10, SIMT-C3, C3-AB, ON-225, PREFIX-FAILURE, STALL-PROBE) and OMEGA-NUMERIC-0 (26/26 best at f523edd) | many older shas, physics e95e3ed / f63a6ef | per receipt | HARDWARE | VALID-HISTORICAL-ONLY (PASS) / preserved FAIL. About 11 PASS and 9 FAIL digest-named receipts remain local-only; NOT in this PR | evidence/E1-*/, ~/workspace/evidence-out/E1-*, OMEGA-NUMERIC-0 (partly published) |
| Gate 14 foundation: 8024e9a PASS (50dd611b); 1720a8d M19 NOT QUALIFIED; 62f5ba5 preview | omega 8024e9a / 1720a8d / 62f5ba5 | tools/gate14_combine.sh | HARDWARE | VALID-HISTORICAL-ONLY; 1720a8d is a preserved FAIL | evidence/GATE14-FOUNDATION/ (published) |
| PHYSICS #27 CHIPWAIT-2 chip run: nvrm 10/10, m15 10/10 PASS, forge FAIL by construction | physics dbefa01 (merged a25df81, AHEAD of CAND-0 pin 6d7cf0d), omega ea60ea6 | tests/run_nvrm_lifecycle_gates.sh etc | HARDWARE | VALID-HISTORICAL-ONLY / INCOMPLETE for CAND-0 | evidence/PHYSICS27/ |
| R1-R14 receipts (host and silicon) | omega 467d8a8 .. 6b38173, physics 29bf6ea / fecbedb | per Make target | HOST and HARDWARE | VALID-HISTORICAL-ONLY (runtime changed: rx_world, rx_jspace, rx_compose, rx_cortex, accelerator_world, physics nvrm) | evidence/R1 .. R14 (published) |
| R15 PASS 16/16 (065c6884) | omega 3e9e53b, physics fecbedb | R15 attempt-2 script | HARDWARE | VALID-HISTORICAL-ONLY | evidence/R15 (published) |
| REQUAL at 3108fc2: R15 FAIL 14/16 (G1, G15), R16 FAIL | omega 3108fc2, physics e95e3ed | REQUAL scripts | HARDWARE | VALID-HISTORICAL-ONLY, recorded failure; R11 living run skipped, R16 G6/G8 NOT_RUN. Plan docs still say R15/R16 passed (discrepancy recorded, not resolved here) | evidence/REQUAL-3108fc2/ (published) |
| R16 canonical receipt 22d7a79a G1-G8 PASS (orchestrator retired) | omega 850fc54, physics e95e3ed | R16 script later found defective | HARDWARE | INVALID as a current or CAND-0 claim; HISTORICAL for 850fc54 only | evidence/R16/ (published) |
| C4-REQUAL 109/109, mutants 9 killed / 3 SURVIVED_GAP / 4 REDUNDANT | omega ce10fce (PR head, not an ancestor of CAND-0) | tools/c4_requal.sh | HOST | VALID-HISTORICAL-ONLY; INCOMPLETE for J-Space integrity digests | evidence/C4-REQUAL-ce10fce/ (published) |
| COMPOSITION-2 host 14/14 x2; GPU tier 14/14 x2; R13 silicon SILICON_PASS_UNBOUND | omega 4aa71bf / 7d554e4, physics not in receipt | tools/composition_gate.sh | HOST / HARDWARE | VALID-HISTORICAL-ONLY; R13 silicon INCOMPLETE (unbound by its own verdict) | evidence/COMPOSITION-2/ (published, INDEX.md) |
| R-LADDER-1720a8d | none | rung-1720a8d.sh | none (empty dir) | INCOMPLETE (NOT_RUN) | ~/workspace/evidence-out/R-LADDER-1720a8d/ (empty, not published) |
| World / J-Space / commit / Cortex composition in sovereign-core | none | n/a | n/a | MISSING_IMPLEMENTATION in sovereign-core (all of it lives in omega) | n/a |
| Host CI on CAND-0 e5593ae (7 workflows success) | omega e5593ae; chip, R13-R16 steps declared SKIP; workflow pins physics e95e3ed | GitHub Actions | CI_HOST (not a receipt) | VALID-FOR-CAND0 as host-tier CI only | GitHub Actions runs on e5593ae |
| TRUST-1 / M5 qualify run | aienos 130aa20 (not CAND-0 9d41efc) | tools trust1_m5_qualify.sh | QEMU and software | VALID-HISTORICAL-ONLY: NOT_QUALIFIED, 20 PASS / 1 FAIL / 16 not run; per-gate logs lost | evidence/TRUST1-M5-20261003-130aa20/ |
| TRUST-1 / M5 first run | aienos 130aa20 | same | QEMU and software | INCOMPLETE (aborted, tool missing; 18 PASS / 1 FAIL / 2 NOT_RUN) | evidence/TRUST1-M5-20261003-130aa20-run1/ |
| ALLEN native genesis + QEMU cold restore, M4_ALLEN 37/37, omega cross-check (aienos side) | receipts bind aienos 640522a; differs from CAND-0 9d41efc only in .crumb and native/kernel/GATES.md (OBSERVED git diff) | scripts/ck_gates.sh / M4_ALLEN gate | QEMU (real QEMU 8.2.2; 33/33 raw serial logs re-hashed by auditor) | VALID-FOR-CAND0 (QEMU only, never hardware). Raw serial logs exist only in a session scratch dir | aienos evidence/allen_native_* (published) |
| aienos ck_gates full run: 15 PASS / 5 NOT_RUN of 20 | aienos 9d41efc-equivalent | scripts/ck_gates.sh --out evidence | QEMU | INCOMPLETE as a full-suite receipt (FPU, INFER, SCREEN skipped on a shared QEMU lock); the 15 PASS rows are VALID-FOR-CAND0 | aienos evidence/ck_gates_1ecf5bcd... |
| "mutants subject_restore_mints / subject_accept_foreign KILLED" | PR #260 body | n/a | n/a | INCOMPLETE: no receipt or log in any repo | none |
| ALLEN v0 host gates PASS (omega) | candidate 6b78165 / cda7609; ALLEN code first in omega b544d81, NOT in CAND-0 e5593ae | omega ALLEN host tool | HOST | VALID-HISTORICAL-ONLY (out of candidate); no raw log anywhere | evidence/ALLEN/ (published) |
| ck_gates ad05e004 17 PASS / 2 NOT_RUN of 19 | aienos 2e44c0b (not an ancestor of 9d41efc) | ck_gates.sh | QEMU | VALID-HISTORICAL-ONLY | aienos evidence/ |
| Physics M2 qualification_receipt.json QUALIFIED_QEMU_VIRT; m15 accelerator link | physics 6d7cf0d (byte-identical M2 artifacts; m15 by source identity) | named runner scripts no longer exist | QEMU virt / source identity | VALID-FOR-CAND0 for the byte-identical artifacts only; not reproducible as named | physics qualification_receipt.json, evidence/ |
| Physics m16 original receipt QUALIFIED; m16 requalification a2c0d7f; M19R FORGE gates at 0fb9042; AR2 / FORM0 simulated | physics 102b42c / a2c0d7f / 0fb9042 | various | HARDWARE / SIMULATED | m16 original INVALID (its own requal record says superseded and failed independent audit, file still says QUALIFIED); others VALID-HISTORICAL-ONLY; AR2 and FORM0 INCOMPLETE and never hardware | physics evidence/ |
| sovereign-core campaign receipts (DUAL macro/serve, release install, chip PASS claims in PRs #176-#189) | base 91fba8f / earlier omega.lock | n/a | HOST (mock) / HARDWARE local-only | VALID-HISTORICAL-ONLY; chip evidence cited by PR bodies is local-only | sovereign-core docs/campaigns/, docs/release/receipts/ |
