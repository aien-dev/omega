# Publication notes (additive)

This file adds explanation only. It does not change, replace or reinterpret any file or verdict in the other
`CAND3-*` directories, which are byte-for-byte copies of the originals in `~/workspace/evidence-out/` on the Spark.
Verification: `ORIGINAL-SHA256SUMS` (527 files) was written from the originals before any copy was made. After copying, all 517
copied files from the six evidence directories matched it (`sha256sum -c`, 517 OK, 0 failed), and the 15 runner/log files matched
`ORIGINAL-RUNNERS-SHA256SUMS` (15 OK). 10 ELF files are in `ORIGINAL-SHA256SUMS` but not published (`excluded-binaries.txt`, 527 = 517 + 10).
No file is over 5 MB. `SHA256SUMS` here covers every file this change adds (the six evidence directories plus this directory);
verify with `sha256sum -c CAND3-AUDIT-20261006/SHA256SUMS` from `evidence/`.

## (a) W1 and W2 are separate attempts and both stay

- W1, `CAND3-LIVING-f816473/`: started 2026-10-06T01:44:15Z, INVALID. The clean chip worktrees and survey checkouts
  (`cand3_setup_trees.sh`) had never been created, so every lane stopped at `cand3_ladder.sh` line 25 with
  "cd .../wt-omega-chip: No such file or directory", exit 2. No build, no chip work, no receipt. The cause is in
  `CAND3-LIVING-f816473/INVALID-W1.txt`, and the same error text is in `logs/window.out`. The directory was sealed read-only
  with its own `SHA256SUMS`; both are published as written.
- W2, `CAND3-LIVING-f816473-w2/`: declared separately before it ran (`00-declared/DECLARED-ATTEMPTS-w2.md`), with its own evidence
  directory, 2026-10-06T01:46:13Z to 02:09:58Z. It replaces nothing; W1 is not a failed W2 and W2 does not erase W1.

## (b) CHIPWAIT limit, and how the soak links to CAND-3

- Each of the three runs is a soak of 100,000 cycles in about 41 to 43 seconds, not hours: `duration_seconds` 40.796, 42.919 and 42.701 in
  `CAND3-LIVING-f816473-w2/CHIPWAIT/campaign/run-001/run.json`, `run-002/run.json`, `run-003/run.json` (`"cycles": 100000` in each; the same
  figures are in each `m19r_soak.log`, `M19R_SOAK_JSON` line). The whole CHIPWAIT step took 460 seconds (`CHIPWAIT/seconds.txt`). It meets
  the written criterion (3 runs of 100,000 cycles) and is not an endurance test.
- The `run.json` files have no commit field (`grep -c commit run-001/run.json` gives 0). The link to CAND-3 is through:
  1. the commands: `CHIPWAIT/command.txt` (`tools/chipwait_campaign.sh --omega-candidate 97ee27584cda... --physics-candidate 6d7cf0d4d8eb... --runs 3`) and
     `CHIPWAIT/campaign/run-00N/command.txt` (`m19r_qualify.sh ... --omega-candidate 97ee27584cda... --physics-candidate 6d7cf0d4d8eb... --run-id run-00N`);
  2. the binary hashes: `CHIPWAIT/campaign/run-00N/hashes.sha256` lists `m16_concurrent` f646c138..., `m16_requalify` 7111a557..., `nvrm_lifecycle` 1b2bdc79...
     (identical in all three runs; the ELF files are in `excluded-binaries.txt`), with `build.log`, `nm.log`, `readelf.log` per run;
  3. the machine snapshots: `CHIPWAIT/machine-before/heads.txt` and `CHIPWAIT/machine-after/heads.txt` (omega 97ee27584cda..., physics 6d7cf0d4d8eb..., both `dirty=[]`,
     `physics.lock` 6d7cf0d4...), and `CHIPWAIT/campaign/run-00N/environment.json` (`omega_sha` 97ee27584cda0d8eaca136f31a44293cd36c9b02, `physics_sha` 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf).
  97ee275 is stated (CLAIM-INDEX.md, DECLARED-ATTEMPTS-w2.md) to be a map-only descendant of the frozen f816473; the check was done on the Spark and is not a file here (UNVERIFIED from published files).
- Observation, not a claim in the original record: the three CHIPWAIT binary digests above, and the `json_canon` digest in `excluded-binaries.txt`
  (7fb54b35...), are equal to the ones in `evidence/CAND2-AUDIT-20261005/excluded-binaries.txt`. Whether that is expected for identical code and toolchain is UNVERIFIED here; it is recorded as found.

## (c) The two limits in CLAIM-INDEX.md

1. Mutant receipts beside real ones: `qual-runs-after-R16/` and `qual-runs-after-R13/` hold R13 receipts `20261006T015956Z` and `20261006T020018Z`
   whose gate reads FAIL. They were written by `tests/runtime/rx_operator_mutants.sh`, whose mutated copies link `build` to the real build
   directory, so their receipts land in `build/qual-runs`. Their binary digests (4259b1f7..., 1c612eef...) are not the production binaries.
   They are mutants the test killed, not results of the candidate. (Explanation as written in CLAIM-INDEX.md; the source line of the test is not copied here, UNVERIFIED.)
2. R11 receipt file not kept: the R11 lane does not copy `build/qual-runs`; the CHIPWAIT lane's `make clean` then removed
   `build/qual-runs/20261006T020212Z-97ee27584cda`. The declared pass rule reads exit code and stdout, which are kept (`R11-living/`).

## (d) What is not claimed

R15 was not attempted (A7 NOT_RUN, no R15 lane in `00-declared/window.txt`). R16 G7 and G8 are NOT_RUN, so R16 as a whole is NOT_RUN
(exit 3). Nothing here claims physical boot, an owner-key ceremony, firmware, TPM, a real storage device, or any hardware TRUST qualification.

## Other points a reader should know

- Carry check: seven executables CHANGED between the CAND-2 code and CAND-3 (`CAND3-CARRY-79a805d/result.txt`); only omegatool, `libomega_gpu.a` and `r15_reduce` are IDENTICAL.
  Which CAND-2 results are carried is decided in `CAND-3.gates.md`, which is not copied here (UNVERIFIED from this directory).
- The runner scripts `cand3_ladder.sh`, `cand3_env.sh` and `run_window.sh` are published inside each window's `00-declared/`, not again under `runners/`.
- Foreign load, quiet-flag and machine-condition details are in the per-step `machine-before/` and `machine-after/` files, unchanged.
- Secrets check before publishing: searched every added file for `token=`, token, secret, password, private key headers, api key, CORTEX, `ghp_`, `github_pat_`, Authorization,
  and for email addresses other than noreply. Hits: "token=0x..." are GPU work-submit tokens in chip logs; "secret" is in R16 mutant and operator test names; "cortex" is the
  Cortex-X925/A725 CPU names, `rx_cortex.c` and crate names; "password" only in "passwordless" sudo preflight text; "tokenizers" in cargo logs. Email addresses appear only as crate
  author fields in `CAND3-BUILD-A/sc-1.log`, `sc-2.log` and `CAND3-BUILD-B/sc-1.log`, `sc-2.log` (cargo metadata), none from this project's account. No vault value found. Home-directory paths remain, as in earlier evidence.
