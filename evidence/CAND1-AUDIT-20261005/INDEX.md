# CAND1-AUDIT-20261005: publication index for the CAND-1 qualification evidence

Same convention as `evidence/CAND0-AUDIT-20261005/INDEX.md`, `evidence/COMPOSITION-2/INDEX.md` and
`evidence/REQUAL-3108fc2/INDEX.md`, under the `evidence-immutable` CI check (`.github/workflows/evidence-immutable.yml`,
introduced by omega #64): this change adds new directories only, no existing file is touched, and no receipt is edited.
Run directories keep the names they have under `~/workspace/evidence-out/` on the Spark. No verdict is changed here.
This file only says which artifact backs each CAND-1 claim, whether the artifact agrees with the campaign ledger
(`~/handoffs/2026-10-05-overnight-campaign.md`) and with `aien-architecture qualification/candidates/CAND-1.toml` and
`CAND-1.gates.md` (merged ca270ee, updated by #134 9403e34), and what is missing.

## Candidate
omega `cb06d081901c03063945a99ffed1c58d33397e9c` (map-only descendant `80ca5d4f5b6cc532d660fc0cbfad7d6c8fb96ad4`, code-identical per CAND-1.toml),
physics `6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf`, aienos `bbad5e4250e57f8cbd1be4cf1390109aa65ef92c`,
aien-sovereign-core `2eec75b08aeb8f9d11f950070e13f6c2c046cf24`. Host: spark-b87b, GB10, driver 580.173.02, Linux 7.0.0-1019-nvidia.
Run-directory timestamps in file names and `ls` are local time (UTC minus 5 hours); log contents carry UTC.

## Classes used
HOST (no chip, no VM), QEMU (qualifies nothing physical), HARDWARE-GB10 (physical chip). Per row below.

## Contents (all new, additions only)
| directory or file | class | what it is |
|---|---|---|
| `CAND1-BUILD-2eec75b/` | HOST | double clean build, `summary.txt` VERDICT PASS, per-build logs, `cand1_build.sh` as run |
| `CAND1-BUILD-cb06d08/` | HOST | omega link check at cb06d08 (libomega_gpu.a, gpu_matmul_api_test, COMPOSITION-2 GPU binary): `build.log`, `sha256.txt` |
| `CAND1-NATIVE-2eec75b/` | HARDWARE-GB10 (zero_cuda is HOST static) | native strict, HF oracle, own-CPU reference, paged batch, runtime e2e (first FAILED run and rerun both kept), daemon, receipts, `cand1_native.sh` as first run |
| `CAND1-LIVING-cb06d08/` | HARDWARE-GB10 for the silicon rows, HOST for the host rows | R16 ladder, qual-runs, R13 test build, prod hygiene, COMPOSITION-2 GPU, CHIPWAIT 3 runs, M18 |
| `CAND1-CKGATES-bbad5e4/` | QEMU | ck_gates receipt and run log, 18 PASS 0 FAIL 2 NOT_RUN |
| `CAND1-TRUST-bbad5e4/` | QEMU and HOST (TPM read of this machine) | TRUST-1 M5 receipt, run log, `t1_gate7_preflight.log` (the FAIL) |
| `CAND1-R16INV-pre/`, `CAND1-R16INV-cb06d08/` | HOST | R16 loop inventory; the FAILING first scans (4 unclassified) are kept next to the PASSING after-scans |
| `CAND1-AUDIT-20261005/runners/` | n/a | `cand1_build.sh`, `cand1_ladder.sh`, `cand1_native.sh` (the post-fix version, see finding F2) |
| `CAND1-AUDIT-20261005/forge-logs/` | n/a | the 9 `~/workspace/test-queue-logs/CAND1-*.log` forge job logs |
| `CAND1-AUDIT-20261005/excluded-binaries.txt` | n/a | sha256 and size of 13 ELF files not copied (3 native test/CLI binaries, 9 CHIPWAIT chip tools, 1 json_canon) |
| `CAND1-AUDIT-20261005/SHA256SUMS` | n/a | sha256 of every file added by this change except itself; verify with `sha256sum -c` from `evidence/` |
| `CAND1-AUDIT-20261005/COMMANDS.md` | n/a | exact commands and environment per run |

Digest-named receipts: `ck_gates_654d4445...json` and `trust1_m5_qualification_37f6a233...json` are named by the sha256 of their bytes (checked, equal).
`COMPOSITION-2-GPU/receipt-1b794db8...json` is named by its ADR 0028 canonical digest (not re-verified here: UNVERIFIED).

## Claim table (every PASS in CAND-1.gates.md: artifact, hash prefix, agreement)
AGREE means the artifact says what the gates table and ledger say. Hashes are the first 12 hex of sha256.
| claim in CAND-1.gates.md | class | artifact (under `evidence/`) | sha256 prefix | vs ledger and gates |
|---|---|---|---|---|
| Clean builds identical x2, 9 executables | HOST | CAND1-BUILD-2eec75b/summary.txt | bd6bf93c61c5 | AGREE; every digest in CAND-1.toml [executables] appears as SAME in summary.txt (checked all 9 by eye against the file) |
| omega link check at cb06d08 | HOST | CAND1-BUILD-cb06d08/sha256.txt | 174d0f98a194 | AGREE; libomega_gpu.a a4af0881 equals the double-build digest |
| Strict real-model gate, 0 fallback | HARDWARE-GB10 | CAND1-NATIVE-2eec75b/receipt-strict-gate.json, strict_gate.log | 83a643875064, 51bbd68f7e57 | AGREE (fallback_count 0, verdict PASS). See F1 |
| Omega vs own CPU reference | HARDWARE-GB10 | receipt-omega-vs-ref.json, omega_vs_ref.log | 83a643875064, 08e85672ad42 | AGREE with ledger (worst dlogit 0.0594 under 0.15, one allowed near-tie flip at step 2, tokens_match=false). The gates table row says only PASS. See F1, F3 |
| Omega vs independent HF oracle | HARDWARE-GB10 | receipt-omega-vs-oracle.json, omega_vs_oracle.log | 83a643875064, 6d896f90f72b | AGREE: step0 max abs dlogit 0.0435, bound 0.15, same token 2744, 16 teacher-forced steps 0 flips, 0 fallbacks |
| Native paged_attention_batch, 3 sequences | HARDWARE-GB10 | paged_batch_gb10.log | 6e35de358ab0 | AGREE: max_batch_seqs=3, chip_calls=408, chip_errors=0, fallback_count=0, verdict PASS. Note `paged_batch_verdict.log` and `gb10_bodies_ran.log` are 0 bytes (they are step markers, the verdict is in results.txt rc=0) |
| Runtime e2e on chip | HARDWARE-GB10 | runtime_e2e_gb10.log (FAILED rc=101, kept), runtime_e2e_gb10-rerun.log | 7f7a2ba3463b, 6bdf3c324eb1 | AGREE: rerun 2 passed, same binary sha 946eeacac853 as the first run. The first failure message is the missing AIEN_CHECKPOINT_ID the ledger names. Not a real model: fixture weights (F4) |
| Daemon serves a turn; refuses without checkpoint | HARDWARE-GB10 | daemon_positive.log, daemon-turn.jsonl, daemon_missing_checkpoint.log | 76b0266ddfaa, c699bb10d5ee | AGREE: 16-token turn text equals the oracle text; missing-checkpoint refusal rc=1 with the "refusing to fall back" message, daemon rc 0 recorded |
| Zero CUDA | HOST (static) plus /proc maps of the daemon | zero_cuda.log, CAND1-BUILD-2eec75b/zero-cuda.log | 92382a7ed550 | AGREE: no CUDA-like dynamic symbols, strings or libraries |
| R16 loop inventory 513/0/0 | HOST | CAND1-R16INV-cb06d08/stderr-after.txt (PASS), stderr.txt (FAIL, 4 unclassified, kept), CAND1-R16INV-pre/ | 20e43f59d772, 266d0b4c73b7, 556694685a60, 0ddd9c626b54 | AGREE: after-scan sites=513 unclassified=0 stale=0, no WARN; before-scans 4 unclassified. The pre dir is omega ddf200d (509 sites) |
| R16 harness G1-G5 PASS, G6 G7 G8 NOT_RUN | HARDWARE-GB10 for G3/G7 silicon | CAND1-LIVING-cb06d08/R16-ladder/stdout.log and raw/receipts/6d843d66...json | 5d9b53789392, 6d843d66a65c | AGREE: Gates line G1-G5 PASS, G6-G8 NOT_RUN, harness exit rc=3. See F5 |
| R1-R10, R12-R14, R15 parity host and silicon PASS; R11 NOT_RUN | host and GB10 | same stdout.log; qual-runs-after-R16/ (16 run dirs) | 5d9b53789392 | AGREE: R11 NOT_RUN in the log; R15 acceptance receipt NOT_RUN (so G7 NOT_RUN). Not a qualification of R11 |
| R13 test build on silicon, candidate bound | HARDWARE-GB10 | qual-runs-after-R13/20261005T061903Z-80ca5d4f5b6c/R13/rx_living_test_build_receipt.json | be8e9eefb41f | AGREE: silicon_observed true, candidate_bound true, candidate_commit 80ca5d4f |
| Production hygiene on silicon | HARDWARE-GB10 | prod-hygiene-silicon/stdout.log | eeaecad7970b | AGREE: PROD_HYGIENE=PASS mode=silicon, ARGUS observing, refuses unobserved |
| COMPOSITION-2 GPU tier 14/14 | HARDWARE-GB10 | COMPOSITION-2-GPU/stdout.log, receipt-1b794db8...json | b9f95a05b0c3 | AGREE: "COMPOSITION-2 gate (GPU tier): PASS", chip failures=0, receipt verdict PASS |
| CHIPWAIT 3 runs PASS | HARDWARE-GB10 | CHIPWAIT/campaign/final-verdict.json, run-00N/run.json | 147a76529629 | AGREE: runs_required 3, usable 3, failed []. The whole campaign took 763 s (CHIPWAIT/seconds.txt); it is not an endurance soak (CAND0-AUDIT finding 3) |
| M18 18/18 | HARDWARE-GB10, classified from the gate text ("physical GB10" Tensor Core gates in M18/stdout.log), not from a run-environment log line | M18/stdout.log | 7317ef22eed3 | AGREE: 18/18 M18 gates, 36 evaluated. `M18-build` took 0 s (no rebuild, existing binary) |
| ck_gates QEMU 18 PASS 0 FAIL 2 MISSING_IMPLEMENTATION | QEMU | CAND1-CKGATES-bbad5e4/ck_gates_654d4445...json, run.log | 654d44450739, ae23ee05b2b5 | AGREE: pass=18 fail=0 not_run=2 (M0_ROLLBACK, KEYBOARD), all 13 child scripts exit 0, tree clean before and after, commit bbad5e4 |
| Continuity: M4_CONTINUITY, M4_RECOVERY, M4_ALLEN cold restore, M4_STORE_CRASH PASS (QEMU only) | QEMU | same receipt and run.log (child logs published in `CAND1-CKGATES-bbad5e4/child-logs/`) | 654d44450739 | AGREE on verdict lines and child exits. All 13 child logs in `child-logs/` match the receipt `log_sha256` fields (checked) |
| TRUST-1 M5 NOT_QUALIFIED, 20 PASS, 11 BLOCKED, 5 MISSING, 1 FAIL | QEMU rows plus HOST preflight | CAND1-TRUST-bbad5e4/trust1_m5_qualification_37f6a233...json, run.log, t1_gate7_preflight.log | 37f6a2331241, 10cf3a235a0e, 8787d0ced2fc | AGREE: counts total 37 = 20+1+0+11+5; the FAIL is `t1_gate7_preflight` (Secure Boot disabled, exit 1). The FAIL is kept as a FAIL |
| CI checks on the CAND-1 commits | GitHub | none published | n/a | DISAGREE in count for omega, see F6 |
| INTERPLANE offline gates | n/a | none published (interplane repo) | n/a | UNVERIFIED here, out of scope of this change |

## Findings (read first)
- F1. The three native receipts `receipt-strict-gate.json`, `receipt-omega-vs-ref.json`, `receipt-omega-vs-oracle.json` are byte-identical (same sha256 83a643875064...). They record only the op report, not the gate numbers.
  The oracle and reference numbers (0.0435, 0.0594) are in the logs, not the receipts. A reviewer must read the `.log` files for the per-gate evidence.
- F2. `CAND1-NATIVE-2eec75b/cand1_native.sh` is the script as first run. The runner copy in `runners/` is the post-fix version (line 219 adds `AIEN_CHECKPOINT_ID="aien-micro-gb10-v1 reference_test_weights (test fixture, not a release checkpoint)"`). The rerun command is this fixed line; the rerun's raw output is `runtime_e2e_gb10-rerun.log`.
- F3. `paged_attention_batch` is listed among native ops in all three receipts, but the oracle gate logged `paged_attention_batch:0/0` calls and the strict gate surface is single-sequence. Batch coverage comes only from `paged_batch_gb10.log`. The gates table already attributes it that way.
- F4. The runtime e2e rerun passes with test-fixture weights (`checkpoint_id` in the rerun log), not the TinyLlama checkpoint.
- F5. The ladder log prints `R15_receipt: PASS` and `G7=NOT_RUN (R15 acceptance receipt: NOT_RUN)`. These are different checks: the first is the receipt-checker self-test (`r16_qualify.sh` line `R15_receipt|test-r15-receipt|...`), the second the missing performance receipt. Not a contradiction, but easy to misread.
- F6. CAND-1.gates.md says "omega 39/39 (2 skipped)". The GitHub check-runs API (queried during this lane, 2026-10-05) gives omega cb06d08: 28 success, 2 skipped; omega 80ca5d4: 19 success, 11 skipped; aienos bbad5e4: 14 success, 2 skipped; sovereign-core 2eec75b: 1 success; interplane 5330a1c: 8 success. The omega 39 count is UNVERIFIED (it may count workflow runs or PR-head checks). No verdict changed.
- F7. GPU sharing. `CAND1-NATIVE-2eec75b/identity.log` records ollama llama-server (15.8 GB), an atlas image python process (18.4 GB) and a MAX server resident, GPU util 96%. The native gates tolerate that (correctness, not timing), but nothing here is timing evidence.
- F8. The ck_gates and TRUST run logs name child-log directories under `/tmp`. They were still present at publication and are now copied to `CAND1-CKGATES-bbad5e4/child-logs/` and `CAND1-TRUST-bbad5e4/child-logs/`. For ck_gates, the sha256 of all 13 child logs equals the receipt `log_sha256`; the TRUST child logs have not been matched to the receipt (UNVERIFIED).
- F9. The campaign ledger asked for a "CKGATES SUSPECT log". No file or ledger line containing the word SUSPECT exists for CAND-1 (searched evidence-out CAND1-*, test-queue-logs CAND1-*, and the ledger). UNVERIFIED which log was meant; the CAND-1 ck_gates run itself has all children exit 0.

## Missing artifacts (explicit)
- Nothing from `/tmp` is missing any more (see F8). QEMU disk images and other scratch from those work directories are not published.
- ELF binaries (13), listed with sha256 in `excluded-binaries.txt`. `aien-cli` (3a17ee79) and `libomega_gpu.a` (a4af0881, copied here) match CAND-1.toml. `strict_real_model` (7866df17) and `runtime_end_to_end_tests` (946eeaca) are test binaries not listed in CAND-1.toml [executables], so their binding to a clean double build is UNVERIFIED.
- The CAND-1 build outputs themselves (omegatool, aien-proof, aien-test, boot images) are not retained, only their digests in `summary.txt`.
- No GitHub CI artifact, no INTERPLANE run output.
- The 2026-10-05 quiet-window runs (all `~/workspace/evidence-out/CAND1-WINDOW*` directories) are not in this PR; they are published separately once complete.
- Physical AIENOS boot evidence: none exists (BLOCKED_HARDWARE, as the gates table says).
