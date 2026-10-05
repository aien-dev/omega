# CAND-1 exact commands and environment

All run on the Spark (spark-b87b, 2026-10-05). Scripts are in `runners/`; the build and native scripts also sit inside their run directories as run.
Toolchain (CAND-1.toml [toolchain]): rustc 1.98.1 (48a229cea 2026-09-01), cargo 1.98.1, cc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU Make 4.3, binutils 2.42.

- BUILD: `~/workspace/overnight-1005/C1/cand1_build.sh 2eec75b08aeb8f9d11f950070e13f6c2c046cf24` (forge log `forge-logs/CAND1-BUILD-2eec75b-055736.log`). Env set by the script: `AIENOS_LOCK_REPO=~/workspace/r16-survey/aienos`, `AIEN_OMEGA_DIR`, `AIEN_PHYSICS_DIR`, `CARGO_TARGET_DIR=<sc worktree>/target-rb`, `AIEN_DEV_FALLBACK` unset. No CARGO_HOME override (default `~/.cargo`, `--offline`).
- omega link check: forge log `forge-logs/CAND1-omega-build-cb06d08-055726.log`; outputs `CAND1-BUILD-cb06d08/`.
- CKGATES: forge log `forge-logs/CAND1-CKGATES-bbad5e4-050542.log`; the receipt names the 13 child scripts (`scripts/qemu_ck_*_test.sh`) with their sha256; the exact top-level command line is not recorded in the log (UNVERIFIED). QEMU 8.2.2 per receipt.
- TRUST: forge log `forge-logs/CAND1-TRUST-bbad5e4-054443.log`; the exact top-level command line is not recorded in the log (UNVERIFIED); the receipt records commit bbad5e4.
- NATIVE: `bash ~/workspace/overnight-1005/L5-NATIVE/cand1_native.sh 2eec75b08aeb8f9d11f950070e13f6c2c046cf24` (forge log `forge-logs/CAND1-NATIVE-2eec75b-060114.log`); rerun of the e2e step after the script fix: `forge-logs/CAND1-NATIVE-e2e-rerun-063201.log` (same test binary sha 946eeacac853d0a9). Env: `RUST_BACKTRACE=1 CARGO_BUILD_JOBS=6`, `AIEN_DEV_FALLBACK AIEN_FORCE_CPU_STUB AIEN_OMEGA_GPU_LIB` unset.
- LIVING (from `CAND1-LIVING-cb06d08/*/command.txt`, omega worktree at 80ca5d4):
  - R16 ladder: `env R16_OUT_DIR=<run>/R16-ladder/raw R16_EXPECT_COMMIT=80ca5d4f5b6cc532d660fc0cbfad7d6c8fb96ad4 tools/r16_qualify.sh` (exit 3 because G6-G8 are NOT_RUN)
  - R13: `env OMEGA_CANDIDATE_COMMIT=80ca5d4f5b6cc532d660fc0cbfad7d6c8fb96ad4 make test-r13-testbuild-silicon`
  - prod hygiene: `make test-prod-hygiene-silicon`; COMPOSITION-2: `make test-composition-gate-gpu`
  - CHIPWAIT: `tools/chipwait_campaign.sh --omega-candidate 80ca5d4... --physics-candidate 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf --physics-dir <physics worktree> --campaign-dir ~/workspace/evidence-out/CAND1-LIVING` (the run was moved into `CAND1-LIVING-cb06d08/CHIPWAIT/` afterwards)
  - M18: `make build/omegatool` then `./build/omegatool --run-m18-gates`
  - Script: `cand1_ladder.sh <omega sha> <ladder|chipwait|m18>`; forge logs `CAND1-LIVING-{ladder-061027,chipwait-061909,m18-063152}.log`
- R16 inventory: `tools/r16_loop_inventory.sh` with `R16_REPO_*` pointing at the CAND-1 trees; exit codes in `rc.txt`/`rc-after.txt` of `CAND1-R16INV-cb06d08/`.
- Verify this publication: `cd evidence && sha256sum -c CAND1-AUDIT-20261005/SHA256SUMS`
