# AIEN Prime Drag Race: implementation side

This folder holds the code every Prime Drag Race entry is built on. The Drag Race is a benchmark where
each entry sieves for prime numbers as many times as it can in a fixed time, and the runner checks every
answer before any speed is believed.

What is here:

- `prime_race_impl.h` and `prime_race_impl.c`: the shared protocol. It reads the command line, runs the
  timed loop, writes the report file and the one-line result. Every entry uses this one copy.
- `cpu_base.c`: the plain, conventional CPU sieve (one thread, one bit per odd number, built the way the
  upstream "base" rules describe). It is the yardstick the faster entries are compared to.
- `tests/prime_race_host_test.c`: checks cpu_base against a slow, independent trial-division oracle for
  every limit from 0 to 3000, the design limit list up to 2,000,000, and known prime counts.

## Build and run

From the repository root:

    make prime-race-cpu
    ./bench/prime_race/build/cpu_base --limit 1000000 --min-seconds 5

    make prime-race-host-test

Options: `--limit N` (default 1000000, at most 2^40), `--min-seconds S` (default 5), `--bitmap-out PATH`,
`--report-out PATH`, `--audit-passes K`. Exit code 0 is success, 2 is an execution failure (no result
line is printed), 64 is a usage mistake. Audit mode runs K passes and saves each pass's answer so the
runner can prove every pass really recomputed; it prints no result line because it makes no timing claim.

## The result line and the labels

On success the program prints one line in the upstream format:

    aien-<name>;<passes>;<elapsed seconds>;<threads>;algorithm=..,faithful=..,bits=..

The labels say honestly what the entry does. `cpu_base` is `algorithm=base`, `faithful=yes` (it builds a
fresh sieve each pass, like upstream), `bits=1`, one thread.

The line is upstream compatible, but the entry is NONCONFORMING for upstream submission (different
license and not an upstream language), so results are kept as AIEN receipts, not submitted upstream.

## The answer format

The canonical answer is the contract: bit k of the output is set exactly when 2k+1 is prime. The number 2
is implied. Unused bits at the end are zero. A previous pass's result is never reused.

The full contract will live in the aien-dev/benchmarks repository once it exists (link to be added).
