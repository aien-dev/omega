# CAND2-PREP-PATHFIX-20261005: omegatool no longer depends on the checkout path

Host only (CPU), no chip, no QEMU. Branch `cand3/omegatool-path`, omega code commit
`9384895bdd08e6e8f6944d671fa0e3102b7d037c` (main a386d65 + this branch). Later commits on the
branch add only this evidence directory and regenerated crumbs (no build input changes).

## Root cause (source: CAND1-PATH2-2eec75b + baseline build, 2026-10-05)
`Makefile` CFLAGS passed `-DOMEGA_PHYSICS_DIR="$(PHYSICS_DIR)"`. `src/omega_blackwell_gates.c`
(M17 physics authority: two string literals `<dir>/m16/m16_native.h` and `cd '<dir>' && git status`)
and `src/omega_blackwell_gates.c` / `src/omega_world_gates.c` (M18/M19 fallback `pd = OMEGA_PHYSICS_DIR`,
one pooled literal) compiled it in: 3 strings, found with `strings -a` on a baseline build. No `__FILE__`,
debug-info or omega-checkout paths were present (sources compile with relative paths; no `-g`).

## Fix
Removed the define. New `src/omega_physics_dir.h`: physics is resolved at run time:
1. `PHYSICS_DIR` environment variable if set and non-empty (make exports command-line variables, so
   `make PHYSICS_DIR=/x test-m17` still works);
2. else the project default `../physics` relative to the current directory (same as `Makefile PHYSICS_DIR ?= ../physics`).
It must contain `m16/m16_native.h` (realpath returned). Otherwise the gate fails with a message naming the
value, its source and the fix; nothing else is tried. M17 then still checks the physics tree is clean; M18/M19
still pass the absolute path to the nested clean-clone build. Build-time `physics.lock` check is unchanged.

## Tests (`make test-omegatool-path`, mk/omegatool-path.mk)
- `tests/omegatool_path/physics_dir_test.c`: env valid, env missing, env not physics (no fallback to ../physics),
  empty env with/without ../physics. PASS.
- `tests/omegatool_path/no_embedded_path_test.sh`: builds omegatool from two copies at different paths, greps both
  for the copy path and PHYSICS_DIR, compares digests. PASS. Negative control: the baseline binary contained the
  physics path (3 strings, see above), so the grep detects the defect.
- Not run: `--run-m17-gates` / M18 / M19 end to end (they touch the chip). The gate code path changed only in how
  the directory string is obtained.

## Verification build (`pathfix_build.sh`, summary.txt, digests.tsv)
Two checkout locations (build-A, build-B/nested/dir), A built twice, B once, fresh GitHub clones of omega (branch
head above), physics 6d7cf0d, aienos bbad5e4, sovereign-core 2eec75b; fresh empty CARGO_HOME per location
(`find "$CARGO_HOME" -mindepth 1 | wc -l` = 0 before `cargo fetch --locked`, see summary.txt). Same recipe as
CAND1-RECON. Result: 9/9 artifacts SAME across 3 builds (table in summary.txt, per-build rows in digests.tsv);
omegatool = 5a3084278ce62868b3655978bc161a0c635b368bddc588963849dd2c5cb343cd in both locations and both repeats,
and contains neither checkout path nor the physics path.
Deviation: sovereign-core's `omega.lock` pins CAND-1 omega cb06d08 and its build refuses another omega, so the
three Rust artifacts are linked against a clean clone of the pinned omega (`omega-pinned`); the fixed omega's
`libomega_gpu.a` has the same digest as CAND-1 (a4af088...), so they equal CAND-1's digests. A real CAND-2 needs
the sovereign-core lock bump and a fresh run. Toolchain: see loc*-toolchain.txt (cc 13.3.0, rustc/cargo 1.98.1,
mojo reported by the script but not used by these builds).
Logs are truncated to the last 40 lines (full logs in scratch, not committed).

CAND-1 records are untouched. This changes a candidate binary: a new candidate (CAND-2) is required.

## Review round 1 (coordinator: M17 authority gap)
The first version (d70bc4a, omegatool 6cd61d10...) only probed `m16/m16_native.h` and a clean `git status`, so an
unrelated clean repo or a non-git directory passed. Now `omega_physics_dir_verify` (src/omega_physics_dir.h) requires
`git -C <dir> rev-parse HEAD` == first line of ./physics.lock (40 hex, cwd-relative like src/omega_evidence.c) and, for
M17, `git status --porcelain` to succeed and be empty; any git error refuses. M18/M19 check the pin only (their nested
make re-checks it). Negative tests in test-omegatool-path: unrelated clean repo, non-repo dir, dirty pinned checkout,
malformed/missing lock refused; correct clean checkout accepted. The old check (emulated in the test as `legacy_m17`)
accepts the unrelated repo and the non-repo dir (red), the new one refuses both. The digests above are from the re-run
at 9384895 (omegatool changed, all other 8 unchanged).
