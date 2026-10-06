# CAND3-AUDIT-20261006: publication index for the CAND-3 evidence

Same convention as `evidence/CAND2-AUDIT-20261005/INDEX.md` (omega #304): run directories keep the names they have under
`~/workspace/evidence-out/` on the Spark, they are added as new directories only (the `evidence-immutable` check), and a
`SHA256SUMS` covers every added file. Nothing was edited, renamed or merged. No verdict is changed here. Campaign ledger on
the Spark: `~/handoffs/2026-10-05-cand3-campaign.md` (not published). The CAND-3 record is `aien-architecture
qualification/candidates/CAND-3.toml` and `CAND-3.gates.md` (not copied here; UNVERIFIED from this directory alone).

## Candidate

Builds ran at omega `f816473df391bc4fbcf99df722edd70ed26ebef1`, aienos `bbad5e4250e57f8cbd1be4cf1390109aa65ef92c`, physics
`6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf`, aien-sovereign-core `80e071a3ef700dbe31b1b081a957eebdcbf5ffc3`, ARGUS
`b375dcaa2887d53cda407d773c2e4e489a0b1365` (`CAND3-BUILD-A/summary.txt` first line). Window W2 ran at omega
`97ee27584cda0d8eaca136f31a44293cd36c9b02`, which `CAND3-LIVING-f816473-w2/CLAIM-INDEX.md` and `DECLARED-ATTEMPTS-w2.md` state is a
map-only descendant of f816473 (guard checked at every lane start and end). That check was done on the Spark and is not
reproduced as a file here (UNVERIFIED from the published files).

## Directories added

| Directory | Kind | What |
|---|---|---|
| `CAND3-BUILD-A/` | HOST | clean build from root `.../cand3/rootA`: per-repo logs, `cargo-fetch.log`, `toolchain.txt`, `zero-cuda.log`, `digests.txt`, `summary.txt` (VERDICT PASS), `cand3_build.sh` as run |
| `CAND3-BUILD-B/` | HOST | same build from `.../cand3/rootB/deeper/path-two`; summary lines after the first equal A's (`diff <(sed 1d A/summary.txt) <(sed 1d B/summary.txt)` is empty, rechecked at publication) |
| `CAND3-ATTN-IDENTITY/` | HOST | `gpu_attention_test` and `libomega_gpu.a` built at omega 79a805d and f816473; `*.sha` byte-identical; `README.txt`. No chip run |
| `CAND3-CARRY-79a805d/` | HOST | executables rebuilt at the CAND-2 code commit and compared with CAND-3 digests (`result.txt`), with build logs and `cand3_carry_check.sh` |
| `CAND3-LIVING-f816473/` | none | window W1, INVALID (harness defect, no test ran), sealed with its own `SHA256SUMS` and `INVALID-W1.txt` |
| `CAND3-LIVING-f816473-w2/` | HARDWARE-GB10 for the silicon steps, HOST for build steps | window W2, 2026-10-06T01:46:13Z to 02:09:58Z, per-step command, exit, seconds, machine snapshots before and after, `CLAIM-INDEX.md` |
| `CAND3-AUDIT-20261006/` | n/a | this index, `PUBLICATION-NOTES.md`, `COMMANDS.md`, `SHA256SUMS`, `ORIGINAL-SHA256SUMS` (digests of the originals taken before copying, including excluded files), `ORIGINAL-RUNNERS-SHA256SUMS`, `excluded-binaries.txt`, `runners/`, `logs/` |

## Builds, attention identity and carry check

| Item | Result | Published file | sha256 (first 12) |
|---|---|---|---|
| Build A | VERDICT PASS, 18 digests recorded, zero-CUDA PASS | `CAND3-BUILD-A/summary.txt` | fd4dbfb8c602 |
| Build A digests | the 18 digest lines | `CAND3-BUILD-A/digests.txt` | 5504165058e2 |
| Build B | same summary lines as A (`SAME` on every digest line, VERDICT PASS) | `CAND3-BUILD-B/summary.txt` | 5eaab7d6769d |
| Build stdout and exit codes | `A exit 0`, `B exit 0` | `CAND3-AUDIT-20261006/logs/buildA.out`, `logs/buildB.out`, `logs/builds.done` | fd4dbfb8c602, 5eaab7d6769d, 42976899f695 |
| Attention identity | `gpu_attention_test` e28bf26b... and `libomega_gpu.a` a4af0881... identical at 79a805d and f816473; no chip run | `CAND3-ATTN-IDENTITY/README.txt`, `79a805d.sha`, `f816473.sha` | a2839b8f0682, 8d59d939905c, 8d59d939905c |
| Carry check | IDENTICAL: omegatool, libomega_gpu.a, r15_reduce. CHANGED: seven rx_* executables (r13 host, r13 silicon, r13 testbuild silicon, r11 aien test, composition gate gpu, r15 perf silicon, r15 perf silicon nodigest) | `CAND3-CARRY-79a805d/result.txt` | fc9e9b92381e |
| W1 | INVALID, cause in file | `CAND3-LIVING-f816473/INVALID-W1.txt` | 421e8c0da70e |

## Claim to artifact (every row of `CAND3-LIVING-f816473-w2/CLAIM-INDEX.md`)

Paths are relative to `CAND3-LIVING-f816473-w2/`. The digest prefixes the claim index names were recomputed on the published
copies at publication: every digest prefix the index quotes (R16 stdout, R16 receipt, operator host and silicon receipts, operator mutants log, R13 test build receipt, prod hygiene, COMPOSITION-2 receipt, R11 stdout, campaign.json, M18 stdout, both declared files) matched.
The index has 11 data rows (A1, A1/G6, A2, A3, A4, A1b, A5, A6, A7, A8 and the machine-condition row); all are covered below, some split into several files.
"Result" is copied from the claim index; it is not a new judgment.

| Attempt | Claim | Result | Published file | sha256 (first 12) |
|---|---|---|---|---|
| A1 | R16 ladder | NOT_RUN overall (declared): G1-G6 PASS, G7 NOT_RUN, G8 NOT_RUN; exit 3 | `R16-ladder/stdout.log` | 197a641eb627 |
| A1 | R16 receipt | same | `R16-ladder/raw/receipts/95ce06cf27c4c832c64cf9fa5c209e83ac7ed2696a8382ce65766c9918c35115.json` | 95ce06cf27c4 |
| A1/G6 | operator control, host receipt | PASS | `R16-ladder/raw/20261006T014614Z-97ee27584cda/operator_host_receipt.json` | ed8d7d5ba022 |
| A1/G6 | operator control, silicon receipt | PASS | `R16-ladder/raw/20261006T014614Z-97ee27584cda/operator_silicon_receipt.json` | b617f1802421 |
| A1/G6 | operator mutants | 25 mutants, 0 failures, 0 skipped | `R16-ladder/raw/20261006T014614Z-97ee27584cda/r16_operator_mutants.log` | 3b94e8ef4a6c |
| A1/G6 | executables equal the double build | MATCH | `R16-ladder/executables-vs-double-build.txt` | 6ad93a3fd0de |
| A2 | R13 test build on silicon, candidate bound | PASS | `qual-runs-after-R13/20261006T020106Z-97ee27584cda/R13/rx_living_test_build_receipt.json` | 638b19f605b8 |
| A3 | production hygiene on silicon | PASS | `prod-hygiene-silicon/stdout.log` | eeaecad7970b |
| A4 | COMPOSITION-2 on the GPU | PASS | `COMPOSITION-2-GPU/receipt-0eda1feced329c026540d90439ba07da84145b52e086b4a84014133ca3987e6a.json` | 0eda1feced32 |
| A1b | R11 living under load | PASS (436 checks, 0 failures) | `R11-living/stdout.log` | a1c1e88dff77 |
| A5 | M19 / CHIPWAIT campaign, 3/3 rule | PASS: 3 usable, 0 invalid, 0 failed; see limit in PUBLICATION-NOTES.md | `CHIPWAIT/campaign/campaign.json` | 22fecfd0d284 |
| A6 | M18 matmul gates | PASS: 18/18, 36 evaluated | `M18/stdout.log` | 7317ef22eed3 |
| A7 | R15 silicon performance | NOT_RUN: not attempted (declared) | no lane run; `00-declared/window.txt` lists no r15 lane | bb3a86b038ae |
| A8 | correctness reruns for carried results | none needed (declared) | stated in `CAND-3.gates.md` section 2 (not copied here; UNVERIFIED from this directory) | n/a |
| - | machine condition | Xid 0 before and after every step | `*/machine-before/xid-count.txt`, `*/machine-after/xid-count.txt` | n/a |

Declared before the window ran: `00-declared/DECLARED-ATTEMPTS.md` (9f4b8b917132) and
`00-declared/DECLARED-ATTEMPTS-w2.md` (ef42d9c814b9); copies also in `CAND3-AUDIT-20261006/runners/`.
The claim index quotes the prefixes 9f4b8b91 and ef42d9c8 for them, which match.
