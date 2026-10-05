# Publication notes (additive)

This file adds explanation only. It does not change, replace or reinterpret any file or verdict in the other
`CAND2-*` directories, which are byte-for-byte copies of the originals in `~/workspace/evidence-out/` on the Spark.
Verification: `ORIGINAL-SHA256SUMS` was written from the originals before any copy was made; after copying,
all 518 copied files from the four evidence directories matched it (`sha256sum -c`), and the 6 runner/log files
matched `ORIGINAL-RUNNERS-SHA256SUMS`. `SHA256SUMS` here covers every file this change adds; verify with
`sha256sum -c CAND2-AUDIT-20261005/SHA256SUMS` from `evidence/`. 10 ELF files are in `ORIGINAL-SHA256SUMS` but not published
(`excluded-binaries.txt`).

## The two R15 outcomes are separate and both stay

- A7 (`CAND2-LIVING-79a805d/R15-silicon/`): NOT_RUN, a harness defect. The r15 lane of `00-declared/cand2_ladder.sh`
  never built the R15 programs, so `tools/r15_qualify.sh silicon` stopped with "missing .../build/rx_r15_perf_silicon: build it first"
  (`R15-silicon/run-dir-progress.log`, exit 2). The claim index says the CHIPWAIT lane's `make clean` is the likely reason and "not shown".
  Nothing in the published files demonstrates that cause (UNVERIFIED). The energy preflight never ran, so this is not an instrument refusal and not a performance result.
- A7b (`CAND2-LIVING-79a805d/A7b-R15/`): declared afterwards in `00-declared/A7b/DECLARED-ATTEMPT-A7b.md`, rebuilt the R15 programs
  (`A7b-R15/R15-build/`), ran once: BLOCKED_INSTRUMENT, INSTRUMENT_UNAVAILABLE ("no hwmon named aien_spbm"), exit 4.
  R15 is not PASS and R16 G7 cannot pass.

## CHIPWAIT: what the soak was, and how it links to CAND-2

- Each of the three runs was a soak of 100,000 cycles in about 41 to 43 seconds, not hours: `duration_seconds` 41.11, 41.635 and 43.191 in
  `CHIPWAIT/campaign/run-001/run.json`, `run-002/run.json`, `run-003/run.json` (same figures in each `m19r_soak.log`, `M19R_SOAK_JSON` line). The whole CHIPWAIT step took
  466 seconds (`CHIPWAIT/seconds.txt`). It meets the written criterion (100,000 cycles) and is not an endurance test.
- The `run.json` files have no commit field (a search for "commit" in `run-001/run.json` finds nothing). The link to CAND-2 is through:
  1. the commands: `CAND2-LIVING-79a805d/CHIPWAIT/command.txt` (`--omega-candidate c62f47b5ac17...`, `--physics-candidate 6d7cf0d4d8eb...`) and
     `CHIPWAIT/campaign/run-00N/command.txt` (`m19r_qualify.sh ... --omega-candidate c62f47b5ac17... --run-id run-00N`);
  2. the binary hashes: `CHIPWAIT/campaign/run-00N/hashes.sha256` lists `m16_concurrent` f646c138..., `m16_requalify` 7111a557..., `nvrm_lifecycle` 1b2bdc79... (identical in all three runs; the ELF files themselves are in `excluded-binaries.txt`),
     with `build.log`, `nm.log`, `readelf.log` per run;
  3. the machine snapshots: `CHIPWAIT/machine-before/heads.txt` and `CHIPWAIT/machine-after/heads.txt` (omega c62f47b5..., physics 6d7cf0d4..., both `dirty=[]`),
     and `CHIPWAIT/campaign/run-00N/environment.json` (`omega_sha` c62f47b5..., `physics_sha` 6d7cf0d4...).
  c62f47b is stated to be code-identical to the frozen 79a805d (only the R16 map and `.crumb` files differ), per `CLAIM-INDEX.md`; the check `git diff 79a805d c62f47b` was done on the Spark and is not reproduced as a file here.
- Observation, not a claim in the original record: the three CHIPWAIT binary digests above are equal to the CHIPWAIT digests in omega `evidence/CAND1-AUDIT-20261005/excluded-binaries.txt` (CAND-1 runs).

## Other points a reader should know

- The task counted "15 rows" in `CLAIM-INDEX.md`; the table has 14 data rows plus the header. All 14 are covered in `INDEX.md`.
- `progress.log` timestamps are local time, about five hours behind the `Z` times used elsewhere.
- Foreign load during the window (a GPU process from another account, an idle ollama server) is recorded in `CLAIM-INDEX.md` and was not stopped.
- Secrets check before publishing: searched all published files for token, key and credential patterns (`ghp_`, `github_pat`, `sk-`, private key headers, `AKIA`, `xox`, `Bearer`, `CORTEX_TOKEN=`, `password`, `api key`). Hits were only the words "secret" in R16 mutant names and in crate descriptions in cargo logs. No vault value found. Home-directory paths remain, as in earlier evidence.
