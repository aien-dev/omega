# Omega mixed algebra MA-2: one exact operation, ten realizations, measured selection

Status: ADR 0019 gate MA-2 (several CPU realizations of one operation with
tiered equivalence evidence and measured cost including conversion) and the
ADR's first success gate (one Omega operation, at least three verified
realizations, one selected with a recorded reason and margin, reproducible
from receipts) delivered. It is not ADR gate MA-3 (contract and identity).
This work was first labelled "MA-3"; it was renamed to match the ADR gate
table. Code: `src/algebra/realize_*.{h,c}` (realizations),
`src/algebra/oma_select.{h,c}` (stand-in selector), tests
`tests/algebra/test_realize.c`, `tests/algebra/bench_mixed_algebra.c`,
`tests/algebra/bench_select.c`. Receipts: `evidence/MIXED_ALGEBRA/`.
Commands: `make test-realize` (plain + ASan/UBSan), `make bench-mixed-algebra`
(two benchmark runs + selector, about 90 s wall). C11 with NEON/I8MM
intrinsics, `-O2 -march=armv8.6-a+dotprod+i8mm+sve`, no outside dependencies.

Receipts and labels: the two benchmark runs, `ma3_bench_run1.json` and
`ma3_bench_run2.json` (schema `OMEGA_MIXED_ALGEBRA_MA3_BENCH_V1`), are the
historical measurement records produced under the earlier "MA-3" label; they
are kept unchanged. `ma2_select_receipt.json` was regenerated from them
(without rerunning the benchmark) with the ADR 9.1 tie rule:
`build/tests-algebra/bench_select evidence/MIXED_ALGEBRA/ma2_select_receipt.json
evidence/MIXED_ALGEBRA/ma3_bench_run1.json evidence/MIXED_ALGEBRA/ma3_bench_run2.json`.
Future `make bench-mixed-algebra` runs write `ma2_bench_run{1,2}.json`
(schema `OMEGA_MIXED_ALGEBRA_MA2_BENCH_V1`); the selector reads both schemas.

Outcome in one line: **on the Grace CPU, packed ternary (2 bits/weight) beats
the best int8 realization 2.2x to 3.0x when the weight matrix no longer fits
in L2 (m = 4096 rows); int8 wins everywhere the weights are cache-resident or
the call is a single row, and int8 wins or ties whenever the weights must be
packed on every call.**

## Operation Omega-X

y = W . x with W in {-1,0,+1}^(m x n) (row-major int8 source), x in int8^n,
y in int32^m, **exact**: every realization must be bit-identical to the naive
integer oracle `oma_rz_oracle`. |y_i| <= 128 n (x may be -128), so n is limited
to 16,777,215 = floor(INT32_MAX / 128) (the same bound as `oma_dot_tw_i8`;
the brief's "n*127" bound is one short for x = -128, so 128 n is enforced).
Realizations with narrower index or residue ranges declare a lower `max_n`
and refuse larger n at pack time.

A realization is (pack, run). Pack converts and validates the canonical int8
ternary matrix into its own weight form; run computes y for one x. Pack cost is
measured separately.

## Realizations

| id | family | weight form (bits/weight) | kernel |
|---|---|---|---|
| R1_plain | binary | int8 (8) | plain C loop, -O2. **Labelled weak baseline.** |
| R1_sdot | binary | int8 row-major (8) | NEON SDOT, 4 rows x 64 B/step, 8 accumulators; best for m = 1 |
| R1_sdot_il | binary | int8, 4-row interleaved tiles (8) | NEON SDOT, one sequential stream per 4 rows |
| R1_smmla | binary | int8, row pairs interleaved in 8-byte pieces (8) | I8MM SMMLA; half of each 2x2 result unused for GEMV, useful MACs per instruction equal SDOT |
| R2_bitplane | bitplane | H1 pos/neg planes, bit k of byte j = weight 16k+j (2) | vtst byte masks, w = negmask - posmask, SDOT (4 SIMD ops / 16 weights) |
| R2b_lut | lut | pos/neg nibble per 4 weights (2) + per-call tables | T-MAC style: per group of 4 activations a 16-entry subset-sum table split lo/hi bytes; vqtbl4q over 4 groups x 4 rows; USDOT/SDOT with +/-1 sums lanes; table build is inside every run |
| R2c_crumb | crumb2 | 2-bit two's-complement codes (+1=01, -1=11), 4 per byte, strided (2) | sign-extending shift pair, then SDOT (2.75 SIMD ops / 16 weights) |
| R3_sparse | sparse | uint16 +1 and -1 index lists per row (16 per non-zero) | scalar gather add/sub, 4 accumulators; max_n 65,536 |
| R4_rns | rns | int8 (8) + per-call residues of x | moduli {256, 255, 253} (M = 16,515,840, all centered residues fit int8), 3 SDOT channels sharing each weight load, per-row mod reduction and mixed-radix CRT inside the run; max_n 64,512 |
| R5_dense5 | dense5 | 5 trits/byte, reference byte rule, strided trit order (1.6) | NEON divide-by-3 decode ((b*171)>>9), SDOT on digits d = t+1, then subtract sum(x) |

SVE SDOT is not a separate realization: the SVE vector length here is 128 bits
(`svcntb() == 16`), so it does the same work as NEON SDOT.

R2c_crumb was added during the work after R2_bitplane measured compute-bound
at 4 SIMD ops per 16 weights. It is a different 2-bit code (not the H1
bitplane code); conversion from int8 is part of its pack.

## Correctness evidence

`make test-realize`: **`MA2 test-realize PASS: 179712 checks, 0 failures, 10
realizations`**, and the same line under `-fsanitize=address,undefined
-fno-sanitize-recover=all`. Coverage:
- every n in {1..5, 7, 15, 16, 17, 31..33, 63..65, 79..81, 127..129,
  159..161, 255..257, 1000, 1023..1025} x every m in {1..5, 7, 15..17, 33}
  (tails of 4, 16, 64, 80, 128 in both dimensions), with all-zero W, all +1,
  all -1, x all -128, all 127, alternating -128/127, and random sparsity in
  {0, 0.3, 0.6, 0.9, 1.0};
- 400 random shapes (n <= 2100, m <= 70), sparsity uniform in [0, 1];
- magnitude edges: n = 16,384 (y = +/-2,097,152), n = 64,512 at the RNS limit
  (y = 8,257,536), n = 64,513 refused by RNS, n = 65,536 accepted and
  65,537 refused by sparse, n = 100,003 (y = 12,800,384);
- each plan is run twice (per-call scratch must not leak state);
- rejections: weights 2, -2, -128, m = 0, n = 0, NULL, n above the int32 bound,
  running an empty plan;
- the oracle is cross-checked row by row against the reference library
  (`oma_pack_bitplane` + `oma_dot_tw_i8`); R5 bytes are checked with
  `oma_dense_byte_decode` (all < 243, right trit in the right place).
- mutation check: swapping one SMMLA result lane makes 500 checks fail.
- the same binary also runs 28 selector tie-rule checks (see Selector); the
  realization checks alone are 179,684, unchanged.

The benchmark re-verifies every realization bit-exact against the oracle on
every grid cell before and after timing: 720 checks per run, 0 mismatches in
both runs. The two historical run JSONs predate the top-level
`oracle_checks` / `oracle_mismatches` fields that the benchmark now writes;
for them the counts come from the run logs and from the nested
`correctness` object each file already carries
(`"runs_checked_against_oracle": 720, "mismatches": 0`).

### Evidence tiers (ADR 0019 section 8)

- The ten realizations (exact contract): **E2, differential with declared
  coverage**. Reference = the naive integer oracle `oma_rz_oracle`; the
  declared coverage is the list above (boundary and tail shapes, all-zero /
  all +1 / all -1 weights, x extremes -128 and 127, sparsity 0 to 1, random
  shapes, magnitude and `max_n` edges, repeated runs, every rejection path),
  plus the benchmark's 720 bit-exact checks per run on the measured grid.
  E2 is the tier ADR section 8 allows for a composed exact kernel.
- The per-element reference core (MA-1, `spec/mixed-algebra-reference.md`):
  **E3, exhaustive**, for per-trit ops (all 9 valid pairs), all 256 codes and
  dense bytes, Z3 scalars (all 256 x 256 inputs), int16 <-> ternary (all
  65,536 values) and every vector pair of length k <= 6 (3^k x 3^k). The
  parts of that library tested on generated suites (int64 conversion,
  multi-block dots, random packing) are E2. The absmean quantizer is an
  approximate transform (ADR section 6.2), not an exact realization, and is
  outside this gate.
- The oracle itself is additionally cross-checked row by row against the E3
  reference core (`oma_pack_bitplane` + `oma_dot_tw_i8`), so the E2 claim is
  anchored to exhaustively checked elements.

## Measurement method

- One Cortex-X925 core (MIDR part 0xd85; cpus 5-9 and 15-19 are X925,
  0-4 and 10-14 are A725), chosen as the most idle X925 over 0.3 s and pinned
  with `sched_setaffinity`: **cpu 5** in both runs.
- Grid: n in {1024, 4096, 16384} x m in {1, 64, 4096} x weight sparsity
  (fraction of zeros) in {0, 0.3, 0.6, 0.9}; x uniform int8.
- Per cell all realizations are packed, then 9 interleaved rounds; each
  round times one block (about 0.3 ms or at least one call) per realization
  with CLOCK_MONOTONIC. Reported: best (floor) and median ns per call, q25/q75.
  Blocks where `/proc/thread-self/schedstat` run_delay grew by more than 2% of
  the block were retried (run 1: 5 retries, run 2: 11, none forced).
- Roofline floor = working-set bytes (packed weights + per-call scratch + x +
  y) / read bandwidth at the smallest measured buffer size >= the working set.
  Bandwidth is the best of three NEON read kernels in the same binary
  (hot buffer): 16 KiB 235 GB/s, 48 KiB 244, 256 KiB 137, 1 MiB 128,
  1.5 MiB 125, 4 MiB 86, 8 MiB 69, 16 MiB 50, 32 MiB 40, 64 MiB 37,
  128 MiB 35, 256 MiB 35 GB/s. "% of floor" = floor / best time. Values above
  100% (R1 at m = 64) mean the single-core read kernel is not the true ceiling
  at L2 sizes; they are reported as measured.
- Because rounds interleave realizations, a large working set is read in
  whatever cache state the previous realization left; for m = 4096 this is
  effectively L3/DRAM streaming, which is also how a model layer would see it.
- Pack cost: median of 3-7 repeated packs (fresh allocation, validation,
  conversion). "Pack per call" total = median pack + median run.
- Load average 0.53 -> 1.04 (run 1), 1.04 -> 0.96 (run 2); thermal zone
  36-44 C. Grid 18.7 s per run; each run with energy about 44 s.

## Results (amortized: pack once), mean of the two runs

Best binary = best of R1_sdot / R1_sdot_il / R1_smmla. Best ternary = best of
R2, R2b, R2c, R3, R5. Ratio > 1 means ternary is faster. The ratio does not
change with sparsity except where noted.

| n | m | sparsity | best binary, ns (% floor) | best ternary, ns (% floor) | binary/ternary | winner |
|---:|---:|---|---|---|---:|---|
| 1024 | 1 | 0-0.6 | R1_sdot 11.2 (78%) | R2_bitplane 23.7 (23%) | 0.47 | binary |
| 1024 | 1 | 0.9 | R1_sdot 11.2 (79%) | R3_sparse 18.2 (29%) | 0.62 | binary |
| 1024 | 64 | all | R1_smmla 399-404 (121-123%) | R2c_crumb 667 (11%) | 0.60 | binary |
| 1024 | 4096 | all | R1_sdot_il 98,000-101,500 (61-63%) | R2c_crumb 43,200-43,900 (20%) | 2.23-2.32 | **ternary** |
| 4096 | 1 | 0-0.6 | R1_sdot 35.9 (98%) | R2_bitplane 85.5 (26%) | 0.42 | binary |
| 4096 | 1 | 0.9 | R1_sdot 35.9 (98%) | R3_sparse 55.4 (37%) | 0.65 | binary |
| 4096 | 64 | all | R1_smmla 2,041-2,049 (103%) | R2c_crumb 2,624-2,632 (19%) | 0.78 | binary |
| 4096 | 4096 | all | R1_smmla 502,000-514,000 (83-85%) | R2c_crumb 172,500-174,000 (36%) | 2.90-2.97 | **ternary** |
| 16384 | 1 | 0-0.6 | R1_sdot 137.5 (98%) | R2_bitplane 337 (25%) | 0.41 | binary |
| 16384 | 1 | 0.9 | R1_sdot 137.1 (98%) | R3_sparse 230 (35%) | 0.60 | binary |
| 16384 | 64 | all | R1_smmla / R1_sdot_il 9,880-9,970 (86-89%) | R2c_crumb 10,600-10,650 (21%) | 0.93-0.94 | binary (small margin) |
| 16384 | 4096 | all | R1_sdot_il / R1_smmla 2,038,000-2,085,000 (93-95%) | R2c_crumb 785,000-809,000 (53-55%) | 2.52-2.66 | **ternary** |

Full per-realization rows (min, median, q25/q75, bytes read, footprint,
floor level, pack cost, per-call total) are in `ma3_bench_run{1,2}.json`
under `cost_table`; the selector's per-cell decisions and the table above are
in `ma2_select_receipt.json` (`decisions`, `binary_vs_ternary`).

**R1 against its roofline:** 78-98% for m = 1 (L1-resident; 98% from n = 4096
up), 86-123% for m = 64 (above 100% at L2 sizes, see method), 61-63% at
1024 x 4096 (4 MiB, L2/L3 boundary), 83-85% at 4096 x 4096, 93-95% at
16384 x 4096 (64 MiB from DRAM). The int8 champion is memory-bound and close to
the floor wherever the matrix streams from outside L2.

### Where ternary wins, and why

Only at m = 4096, where the int8 matrix (4, 16 or 64 MiB) streams from L3 or
DRAM. R2c_crumb moves a quarter of the bytes and its decode (7 shifts + 4 SDOT
per 64 weights per row) keeps up well enough to finish 2.2x to 3.0x sooner, at
20-55% of its own (4x lower) floor: it is compute-bound, the int8 kernel is
bandwidth-bound. The win is independent of sparsity (dense 2-bit code).

### Where ternary loses, and why

- m = 1 and m = 64: the int8 weights are L1/L2-resident, bandwidth is not
  the limit, and every packed form spends 2.75-4 SIMD ops per 16 weights
  decoding where SDOT spends one. Binary is 1.1x to 2.4x faster.
- At 16384 x 64 (1 MiB of int8 weights, L2) the gap narrows to 7%, still binary.
- **Pack per call:** every realization's pack reads and validates the whole
  int8 source, so no packed form can beat a copy. In per-call mode the cheapest
  is always an int8-form realization (R1_*, R4_rns or R1_plain, whose packs
  are all a validated copy and mostly tie inside the noise band, so the tie
  rule selects R1_plain there); the ternary packs
  are scalar loops costing 1.1x (crumb) to 3-4x (bitplane, LUT) the int8 copy.
  Ternary pays off only when weights are packed once and reused.

### Negative results (recorded, not tuned away)

- **R4_rns** loses everywhere amortized: 1.4x (DRAM-bound) to 90x (m = 1,
  where residue build dominates) slower than R1. With SDOT the int32 lane is
  already exact and as cheap as any narrower lane, so three channels triple the
  MAC work and add residue build and CRT; nothing is saved.
- **R3_sparse** never wins on this grid. Scalar gather runs at about one index
  per cycle; at 90% zeros it is the best ternary form for m = 1 but still
  1.5-1.7x behind SDOT, and at m = 4096 it is 1.3x behind R2c_crumb even at 0.9
  (1.05 ms vs 0.81 ms at 16384 x 4096).
- **R2b_lut** (T-MAC style) is correct but loses to both bitplane and crumb:
  vqtbl4q plus lo/hi split costs more than decoding, and the per-call table
  build makes m = 1 the worst case (2.1 us at n = 4096, 58x R1).
- **R5_dense5** reaches R1's speed only at 16384 x 4096 (1.73 ms vs 2.05 ms,
  1.2x win over int8 but 2.2x behind crumb); the divide-by-3 decode is
  compute-bound. Its 1.6 bits/weight buys nothing over 2 bits here.
- **R1_smmla** is not faster than a well laid out SDOT for GEMV (half of each
  2x2 product is wasted); its advantage over row-major R1_sdot at m >= 64 came
  from the interleaved layout, which R1_sdot_il reproduces with SDOT.

## Selector (stand-in, not wired to rx_costmodel)

`oma_select` loads MA-2 (and the historical MA-3-label) bench receipts (costs
are read from the JSON, never hard-coded), applies the exact-contract filter
(registry `exact`, verified in every run used, query n <= `max_n`), and ranks
the eligible realizations by median cost (plus median pack cost when packing
per call), mean over runs. Off-grid queries use the nearest measured cell
(log2 distance in n and m, plus 4x the sparsity difference) and say so
(`exact_cell: false`). Each decision record holds the query, the cell, the
chosen realization, the cheapest, the runner-up, every candidate with its
measured cost, within-run and across-run spread and % of floor (or the reason
it was excluded), the margin (runner-up vs cheapest), the noise band, a
verdict CHOSEN or TIE, the tie set (every eligible candidate inside the band
of the cheapest, the cheapest included), the incumbent given with the query
and `tie_resolution`.

Noise band = max over cheapest and runner-up of (a) the within-run spread
(q75 - q25) / median of the timed blocks (pack spread weighted in when packing
per call) and (b) the across-run spread (max - min) / mean of the cell's costs
over every run in the table. TIE when margin <= band.

**Tie rule (ADR 0019 section 9.1).** Without a TIE the cheapest is chosen
(`tie_resolution: "none"`). On a TIE the incumbent selection stands when the
query names an eligible incumbent (`"incumbent"`); with no incumbent the
digital reference realization, `R1_plain` (`OMA_SEL_REFERENCE`), is selected
(`"reference"`). The reference is selected whether or not it is inside the
band, as the ADR states the rule; only when the reference is not eligible for
the query does the cheapest stand (`"cheapest_reference_ineligible"`). The
receipt's decisions carry no incumbent (there is no prior selection), so
every TIE selects `R1_plain`. `test-realize` checks every branch of the rule
(reference, incumbent, unknown or unmeasured incumbent, no TIE, reference
unmeasured, reference unverified in one run, TIE from across-run spread alone).

Effect on the receipt (72 decisions over both runs): **38 TIE, 32 per-call
and 6 pack-once**, and all 38 now select `R1_plain`. Before the tie rule
1 decision chose `R1_plain`; in the 38 tied decisions the cheapest stood
(R1_sdot 26, R4_rns 5, R1_smmla 3, R1_sdot_il 3, R1_plain 1). The 6 pack-once
ties are 1024 x 64 at sparsity 0 and 0.6 (R1_smmla vs R1_sdot_il) and
all four 16384 x 64 cells (R1_smmla vs R1_sdot_il within about 1%); there
`R1_plain` is outside the tie set and costs 2.05x to 2.78x the cheapest
(for example 1119 ns vs 403 ns at 1024 x 64). In all 32 per-call ties
`R1_plain` is inside the tie set and costs 1.00x to 1.26x the cheapest. The
34 non-TIE decisions are unchanged (R1_sdot 16, R1_smmla 6, R2c_crumb
12). This is the ADR's conservative rule applied as written; a rule that
takes the reference only from inside the tie set would be an ADR amendment,
not a selector choice.

**Reproducibility check** (`MA2_SELECTOR_REPRO`): each of the 36 cells x 2
pack modes is decided from run 1 alone and from run 2 alone.
**72 decisions: 64 agree, 8 recorded ties, 0 disagree -> PASS**, with the
tie rule applied. "Recorded ties" counts only the cells where the two
single-run choices differ and one run's TIE set contains the other run's
choice; it is not the number of ties. Total TIE verdicts in the decisions
over both runs: **38 of 72 (32 per-call, 6 pack-once)**, listed in
`decision_summary`.

## Energy (indicative)

The `aien_spbm` hwmon meter is readable without root (`cpu_p` = the whole
10-core X925 cluster, microjoules, updates about every 0.1 s). Per
realization: 0.4 s idle window, then 0.8 s of back-to-back calls; the receipt
lists cluster idle and run power, total and dynamic (run minus idle) energy
per call. At 16384 x 4096 (sparsity 0.3), run 1 total energy per call:
R2c_crumb 7.5 mJ, R2_bitplane 8.6 mJ, best R1 12.7 mJ, R4_rns 20.9 mJ,
R3_sparse 48.8 mJ. At 1024 x 4096 the forms are within about 20% of each
other (0.42-0.51 mJ for R1/R2/R2c). Idle power moved between 1.1 and 3.8 W
across windows because other sessions share the cluster, so these are not
gate-quality numbers; energy is not used by the selector.

## Limits

- Single core, single thread; no multi-core scaling, no GPU (no GPU test was
  run).
- Shared machine: contention is detected per block and retried, not excluded.
  Costs are best-of and median of 9 blocks, two runs.
- Floors come from this binary's single-core read kernels; they are a
  reference, not a proven hardware maximum (R1 exceeds them at L2 sizes).
- Pack paths are plain scalar C; faster packers would narrow, not reverse, the
  per-call result (every pack must read the int8 source once).
- The selector is a stand-in over a 36-cell table with nearest-cell lookup; it
  is not connected to `rx_costmodel` or the resident omega.select reaction.
- Library audit note: no defect was found in `oma_trit`/`oma_z3`/`oma_pack`/
  `oma_quant` while using them here (`oma_dot_tw_i8`, `oma_pack_bitplane`,
  `oma_dense_byte_decode` agreed with the oracle on every checked row and
  byte).
