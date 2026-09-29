# POLYGLOT-0: one meaning, several languages (Omega-X)

Status: CONTRACT (lead-owned). Implementation in progress. Nothing here is qualified until `POLYGLOT_0_PASS`.
Base: mixed-algebra MA-3 commit 9207ce3 (branch feat/mixed-algebra). If `oma_rz_oracle`, `oma_rz_impl` or
`oma_rz_plan` change upstream, every polyglot result is re-run before landing.

## 1. Semantic operation

Omega-X is defined once, in `src/algebra/realize_common.h` (MA-3), and is not redefined here:

- input semantic identity: `(W, x)` where `W` is the canonical row-major int8 matrix with every entry in
  {-1, 0, +1}, shape `m x n`, and `x` is int8^n;
- output semantic identity: `y = W . x` in int32^m, **exact**;
- preconditions: `m >= 1`, `1 <= n <= max_n` of the candidate (global bound `OMA_RZ_MAX_N` = 16,777,215 =
  floor(INT32_MAX / 128), because |y_i| <= 128 n when x_i may be -128); `W`, `x`, `y` non-NULL; `y` does not
  overlap `W` or `x` (checked by the verifier; candidates may assume it);
- postcondition: `y` is bit-identical to `oma_rz_oracle(W, m, n, x, y)`;
- error contract: pack returns `OMA_RZ_E_TRIT` for any weight outside {-1,0,+1}, `OMA_RZ_E_ARG` for bad shape or
  NULL, `OMA_RZ_E_NOMEM`, `OMA_RZ_E_OVERFLOW`; on error no partial plan is kept (`oma_rz_free` safe);
  run on an empty plan returns `OMA_RZ_E_ARG`;
- resources: packed weight bytes, footprint and per-call scratch are declared in `oma_rz_plan`;
- effects: none (pure; no I/O, no global state, no threads); capabilities: none.

Approximation: none allowed. Any candidate that is not bit-exact is rejected, not ranked.

## 2. Candidate axes

A polyglot candidate is `(language/toolchain, representation, backend)`:

| axis | values in POLYGLOT-0 |
|---|---|
| language/toolchain | C (gcc 13.3, -O2 as MA-3 and -O3 -mcpu=native), hand AArch64 assembly (GNU as 2.42), Omega's own AArch64 encoder (toolchain check, see 4), Mojo 1.0 (experiment only) |
| representation | int8 row-major (MA `R1_sdot`), 2-bit crumb (MA `R2c_crumb`) |
| backend | Grace CPU, one pinned core, cluster recorded |

No Rust and no Python are written for this program (Drake's standing rules). Rust-vs-C evidence comes only from
existing code measured as a black box (aienos capability authority crate vs its C port).

clang is not installed on this machine; C is represented by gcc only. Recorded as a limitation.

## 3. Registration

Every non-C candidate is an `oma_rz_impl` (same pack/run ABI as MA-3) registered through
`src/polyglot/omx_lang.h`, with language metadata. MA-3 files are never edited.

## 4. Own-encoder rule

A realization emitted by Omega's AArch64 encoder is a toolchain candidate, not a language candidate. Its gate is:
the emitted instruction bytes equal GNU `as` output for the matching assembly realization, or every difference is
listed with a reason, and its results are bit-exact. Its speed is not counted as a language result.

## 5. Independence of assembly

The assembly author records whether the code was derived from `gcc -S` output. Derived code is labelled
"compiler-derived" and does not count as an independent language realization.

## 6. Workloads (all four required)

| id | shape | regime |
|---|---|---|
| S1 | m=1, n=4096 | call overhead |
| S2 | m=256, n=1024 (256 KiB int8, L2-resident) | cache-resident |
| S3 | m=4096, n=4096 | memory bandwidth |
| S4 | m=64, n=1024, 256 different x per packed W, cache-resident | compute-bound (instruction selection) |

Sparsity 0.0, 0.33 and 0.66 for each shape. Inputs from a fixed seed; the input digest is SHA-256 of W||x.

## 7. Measurement rules

- honour `~/workspace/.spark-quiet`; message the mixed-algebra session before each timed run;
- pin to one core; record core id, cluster (X925 or A725), cpufreq governor and current frequency, and
  thermal zone temperatures before and after;
- interleave candidates round-robin (A B C A B C), never blocks; N >= 20 samples per cell;
- for small shapes, a timed sample loops enough calls to last >= 1 ms and consumes every y (checksum), so no call
  is hoisted or removed;
- report min, median and MAD; skip and re-take samples whose `/proc/thread-self/schedstat` run delay grew;
- pack cost reported per weight, and the break-even number of calls against the cheapest-to-pack candidate;
- compile time (wall, 5 runs, min), code size of the realization's text section, peak RSS of the bench process.

Informational only (not gated): source lines, unsafe surface (= assembly lines + pointer casts), defects found
while authoring.

## 8. Evidence record

One JSON receipt per candidate x workload under `evidence/POLYGLOT/`, fields: semantic_operation ("omega-x"),
contract digest (SHA-256 of this file section 1), language, toolchain + version, flags, representation, backend,
machine (cpu model, core, cluster, kernel), build digest (SHA-256 of the realization's object file), input digest,
correctness (checks, failures), latency min/median/MAD, throughput, bytes moved (weight_bytes + x + y +
scratch), footprint, pack cost, compile time, code size, informational fields, source commit. The lead maps these
onto existing `omega_evidence` / `rx_costmodel` receipt fields for POLYGLOT-1.

## 9. Explanation

`polyglot_explain` reads only the contract and the receipts and prints, per workload, which candidate it would
select, the runner-up, the measured margins, the conversion (pack) cost and break-even, and which alternatives
were rejected and why. Same inputs give byte-identical output.

## 10. POLYGLOT_0_PASS

All of:
1. this contract, unchanged since the receipts were produced (contract digest matches);
2. at least two language candidates that are not compiler-derived (C plus hand assembly at minimum) and at least
   two representations (int8 and crumb), each bit-exact;
3. verifier: MA-3's full shape grid, plus 20,000 random cases (random m, n, sparsity, x including -128/127,
   including m and n not multiples of any vector width, unaligned x and y pointers), plain build and
   ASan/UBSan build; assembly candidates additionally run with guard pages on both sides of W, x and y; 0 failures;
4. own-encoder candidate: byte comparison report per section 4 (gate is the report, not speed);
5. benchmark on S1-S4 with section 7 rules and one receipt per candidate x workload;
6. explainer output generated from receipts, reproducible byte-for-byte;
7. an independent reviewer's report with every blocking finding closed;
8. Mojo is optional: included if it builds and is bit-exact, otherwise recorded as a result, not a failure.
