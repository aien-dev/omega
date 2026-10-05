# CAND2-AUDIT-20261005: publication index for the CAND-2 evidence

Same convention as `evidence/CAND1-AUDIT-20261005/INDEX.md` (omega #288) and `evidence/CAND1-QUIET-AUDIT-20261005/INDEX.md`
(omega #292): run directories keep the names they have under `~/workspace/evidence-out/` on the Spark, they are added as
new directories only (the `evidence-immutable` check), and a `SHA256SUMS` covers every added file. Nothing was edited, renamed
or merged. No verdict is changed here. The CAND-2 record is `aien-architecture qualification/candidates/CAND-2.gates.md`
(frozen #138 `dfec615`, gates #139 `92e3c86`): NOT QUALIFIED. Campaign ledger on the Spark: `~/handoffs/2026-10-05-cand3-campaign.md`.

## Candidate

Frozen in `aien-architecture qualification/candidates/CAND-2.toml`. Builds here ran at omega `79a805d162bfded8c5ce5a4c14f7c29e79025f39`,
aienos `bbad5e4250e57f8cbd1be4cf1390109aa65ef92c`, physics `6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf`, aien-sovereign-core
`286fa9b7afbc71f06ed7e1dd29f68f36714fd92a` (`CAND2-BUILD-A/summary.txt` first line). The window ran at omega `c62f47b5ac178311d7d8f91c4f52a298a1754ec8`,
which `CAND2-LIVING-79a805d/CLAIM-INDEX.md` states is code-identical to 79a805d (only the R16 map and `.crumb` files differ).

## Directories added

| Directory | Kind | What |
|---|---|---|
| `CAND2-BUILD-A/` | HOST | clean build from root `.../cand2/rootA`: per-repo build logs, `cargo-fetch.log`, `toolchain.txt`, `zero-cuda.log`, `summary.txt` (VERDICT PASS), `cand2_build.sh` as run |
| `CAND2-BUILD-B/` | HOST | same build from a second root `.../cand2/rootB/deeper/path-two`; its nine digest lines equal A's (compared `diff <(sed 1d A/summary.txt) <(sed 1d B/summary.txt)`: empty) |
| `CAND2-ATTN-IDENTITY/` | HOST | `gpu_attention_test` and `libomega_gpu.a` built at omega cb06d08 and 79a805d; `*.sha` byte-identical; `README.txt`. No chip run |
| `CAND2-LIVING-79a805d/` | HARDWARE-GB10 for the silicon steps, HOST for build steps | quiet window 18:04:14Z to 18:23:25Z (`00-declared/window.txt`) plus the separately declared A7b (18:26Z), with per-step command, exit, seconds, machine snapshots before and after |
| `CAND2-AUDIT-20261005/` | n/a | this index, `PUBLICATION-NOTES.md`, `COMMANDS.md`, `SHA256SUMS` (published files), `ORIGINAL-SHA256SUMS` (digests of the originals taken before copying, including excluded files), `excluded-binaries.txt`, `runners/run_builds.sh`, `logs/` (build and window stdout) |

`CAND2-PREP-PATHFIX-20261005` was already in the repository and is not part of this change.

## Claim to artifact (every row of `CAND2-LIVING-79a805d/CLAIM-INDEX.md`)

All paths are relative to `CAND2-LIVING-79a805d/`. The "sha256 matches" column was checked on the published copy against the
digest the claim index records for it (14 of 14).

| Attempt | Claim | Published file | Matches |
|---|---|---|---|
| A1 | R16 G1..G8: G1-G5 PASS, G6 MISSING_IMPLEMENTATION, G7 NOT_RUN, G8 NOT_RUN (R16 not PASS) | `R16-ladder/raw/receipts/40742fdee4bbc4714b4742a70ff127c6033a478e426baf32fc787f0080378d53.json` | yes |
| A1 | R16 console, exit 3, NOT_RUN | `R16-ladder/stdout.log` | yes |
| A1b | R11 living, 436 checks 0 failures, living run exercised | `R11-living/stdout.log` | yes |
| A2 | R13 test build on silicon, candidate bound | `qual-runs-after-R13/20261005T181426Z-c62f47b5ac17/R13/rx_living_test_build_receipt.json` | yes |
| A3 | production hygiene on silicon | `prod-hygiene-silicon/stdout.log` | yes |
| A4 | COMPOSITION-2 GPU tier PASS | `COMPOSITION-2-GPU/receipt-06fa0b05d070331d90b118633bfa84f6d5e63a1985275bf4b0ac3aaa271de394.json` | yes |
| A5 | CHIPWAIT 3/3 mechanical verdict | `CHIPWAIT/campaign/final-verdict.json` | yes |
| A5 | M19R run 1 | `CHIPWAIT/campaign/run-001/run.json` | yes |
| A5 | M19R run 2 | `CHIPWAIT/campaign/run-002/run.json` | yes |
| A5 | M19R run 3 | `CHIPWAIT/campaign/run-003/run.json` | yes |
| A6 | M18 full qualification 18/18 | `M18/stdout.log` | yes |
| A7 | R15 silicon NOT_RUN: harness defect, programs never built, not an instrument refusal, not a performance result | `R15-silicon/run-dir-progress.log` (last line: `missing .../build/rx_r15_perf_silicon: build it first (make r15-perf-silicon)`), `R15-silicon/exit.txt` (2), `R15-silicon/stdout.log` | yes (progress log) |
| A7b | R15 silicon BLOCKED_INSTRUMENT, INSTRUMENT_UNAVAILABLE, exit 4 | `A7b-R15/R15-silicon/run-dir/progress.log`, `A7b-R15/R15-silicon/exit.txt`, `A7b-R15/R15-silicon/run-dir/instrument-unavailable.txt` | yes (progress log) |
| A7b | R15 programs built for A7b at c62f47b | `A7b-R15/R15-build/executables.sha256` | yes |

A7 and A7b are separate rows and separate directories (`R15-silicon/` and `A7b-R15/`). A7b was declared after A7 in
`00-declared/A7b/DECLARED-ATTEMPT-A7b.md` (sha256 `f56a97d6...`), which says A7 stays NOT_RUN and is not replaced. Neither was merged or renamed.
`LADDER-SUMMARY.txt` and `A7b-R15/LADDER-SUMMARY.txt` show the exit codes (A7 `rc=2`, A7b `rc=4`).

## Other claims in `CAND-2.gates.md`

| Claim | Where |
|---|---|
| Nine executable digests equal across two build roots | `CAND2-BUILD-A/summary.txt`, `CAND2-BUILD-B/summary.txt` (each: every item built twice, SAME; zero-cuda PASS; VERDICT PASS) |
| Toolchain | `CAND2-BUILD-A/toolchain.txt`: rustc 1.98.1, cargo 1.98.1, gcc 13.3.0, GNU Make 4.3, GNU ld 2.42 |
| Attention binary identical between CAND-1 and CAND-2 | `CAND2-ATTN-IDENTITY/cb06d08.sha`, `79a805d.sha`, `README.txt` |
| Declared before running | `CAND2-LIVING-79a805d/00-declared/DECLARED-ATTEMPTS.md` (sha256 `8a6e06da...85cf`, recorded in `00-declared/sha256.txt`) |
| Machine state | per step `machine-before/` and `machine-after/` (`heads.txt`, `nvidia-smi.txt`, `xid-count.txt`, `uptime.txt`, `top-cpu.txt`, `gpu-processes.csv`), plus `00-identity/machine-start/` |
| Foreign GPU process and idle ollama during the window | `00-identity/machine-start/gpu-processes.csv`, `A7b-R15/R15-silicon/run-dir/processes-before.txt` (A7b run also printed an ollama warning, `progress.log`) |

## Limits

- ELF files are not copied (convention of CAND1-AUDIT): 10 files, 760,547 bytes in total, listed with sha256 and size in `excluded-binaries.txt`. Nine of them are the CHIPWAIT chip tools, whose digests are also in each `CHIPWAIT/campaign/run-00N/hashes.sha256` (published). The tenth is `json_canon` in `R16-ladder/raw/`.
- Build outputs (omegatool, aien-proof, aien-test, images) are not retained, only their digests in `summary.txt`.
- Timestamps in `progress.log` files are local time (about UTC-5: `13:23:25` against window end `18:23:25Z`). The offset is read from that pairing, not from a timezone field.
- No GitHub CI artifacts and no status-page links are added here.
