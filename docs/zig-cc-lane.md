# Zig cc cross-check lane

Workflow: `.github/workflows/zig-cc.yml` ("Zig cc cross-check"). It runs on every
pull request and on pushes to `main`, on the same `ubuntu-24.04-arm` hosted runner
as `pr-smoke.yml`.

## What it is

Omega is built with gcc everywhere else. This lane builds the same sources with
`zig cc` from a pinned Zig 0.17.0 (clang 22) and asks one question: does an
independent compiler agree with gcc about what the code means? It is a
cross-check, not a second source of truth. The gcc lanes (`pr-smoke.yml` and the
host-suites workflows) stay authoritative, and this is not a migration to Zig.

The Zig tarball is downloaded from ziglang.org and checked against a pinned
SHA-256 before it is unpacked. The version and digest live in the workflow file;
changing the version means changing both together.

## What it checks

1. `omegatool` and the compiler tests build under clang with the Makefile's own
   flags (`-Wall -Wextra -Werror`). A warning that clang reports and gcc does not
   fails the lane.
2. `make test` passes with the same assertions as `pr-smoke.yml`: every
   `TOTAL GATES` line shows zero failures.
3. `make test-language` (Omega language V0 golden, 407 cases) passes.
4. `make test-compiler-quick` (OSC compiler smoke) passes.
5. Gate and demonstration output from the zig-built `omegatool` is compared with a
   gcc-built `omegatool` from the same commit, for 18 suites: `--run-gates`,
   `--run-m6-gates` to `--run-m14-gates`, and the arithmetic, physics, self-host,
   verify, program, synthesis, library and discovery demonstrations. The output
   must be byte-identical, so every gate total and every printed 64-hex identity
   matches.

## What it does not check

- Any GPU or chip suite. Nothing here touches `/dev/nvidia*`, the GB10, or the
  M15 and later gate sets. Chip qualification stays on the Spark.
- The QEMU-backed pieces: `--run-m5-gates` and `--reference-demonstrate-realization`.
  No omega workflow installs QEMU, so they are left out of the hosted comparison.
  They were run by hand against both compilers on the Spark (QEMU present) when
  this lane was added, with identical output.
- Makefile targets that hard-code `gcc` instead of `$(CC)` (the numeric CPU,
  reduce and transcendental checks). They do not exercise Zig.
- AddressSanitizer. `zig cc` accepts `-fsanitize=address` but does not instrument
  the code with it, so the `_asan` test binaries in `test-compiler-quick` run as
  plain builds. UBSan is applied (through Zig's own runtime). The gcc lanes remain
  the ASan evidence.
- Byte-for-byte reproducible binaries. Two clean zig builds of the same commit
  gave different `omegatool` bytes (the gcc builds were identical), so the lane
  compares program behaviour, not binary hashes.

## Resolved item

clang 22 reported `-Wunterminated-string-initialization` for
`src/omega_blackwell_encoder.c` (`static const char HEXDIGITS[16] =
"0123456789abcdef";` left no room for the terminating NUL; gcc 13 does not
warn). The array is now declared `HEXDIGITS[]` (17 bytes, omega#432), and the
lane no longer passes `-Wno-error=unterminated-string-initialization`: every
warning is an error, as in the gcc lanes.

## Running it by hand

From a checkout with the pinned Physics at `PHYSICS_DIR`:

    ZIG=/path/to/zig-aarch64-linux-0.17.0/zig
    make CC="$ZIG cc" PHYSICS_DIR=../physics test

Use a separate `OUT_DIR` per compiler if you want both builds side by side. The M5
QEMU gate writes to a fixed `build/` directory, so create `build/` first when
`OUT_DIR` is something else.
