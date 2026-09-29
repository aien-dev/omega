# Omega language evidence ledger (POLYGLOT-0)

What this is: for one exact operation (Omega-X, `y = W.x` with ternary W, int8 x, int32 y; contract
`spec/polyglot-0.md` section 1), which language and weight representation was fastest on each workload shape,
by how much, and where the difference is too small to call. Every number below comes from the receipts of one
timed run. Nothing here generalises beyond that run, that operation, and that core.

## Run

| item | value |
|---|---|
| run id | `20260929T190337Z-2e2238f74ba3` |
| source commit | `2e2238f74ba3b8681606ce0fb08f8feade5dec6d` (branch `feat/polyglot-0`), 0 dirty files |
| contract digest | `4b9034ae795cbcc1bca6739080fce8e83e346b9c4378ce2f320f5228b90d46b1` |
| receipts | 72 (18 candidates x 4 shapes), 216 cells (x 3 sparsities), all `gate_eligible`, not smoke |
| core | cpu 5, Cortex-X925 (midr 0xd85), picked as the most idle X925 (100% idle over 0.3 s) |
| clock | governor `performance`, requested 3.90 GHz at start, end and at every cell start; effective clock measured separately right after the run with `perf stat` on cpu 5: 5,410,961,821 cycles in 1.388 s = 3.90 GHz (no X925 clock cap) |
| load | load average 0.49 before, 0.60 after; no other build/bench/test process, no GPU compute apps; `~/workspace/.spark-quiet` held for the run and removed after |
| samples | N = 21 per cell, round-robin across candidates; shortest kept sample 1.26 ms; 0 short samples kept; 94 samples retaken because scheduler run delay grew; 0 contaminated |
| correctness in run | 21,123 checksum checks, 0 failures (full verifier: `make test-polyglot`, see review) |
| timed wall | 14.8 s; peak RSS 227,192 KiB |
| explainer | `make polyglot-explain POLYGLOT_RUN=20260929T190337Z-2e2238f74ba3`, two runs byte-identical; copy in `evidence/POLYGLOT/polyglot_explain_20260929T190337Z-2e2238f74ba3.txt` |

Decision rule (explainer, spec section 9 and review G-S7): the selected candidate is the lowest median among
selectable candidates. It is CHOSEN only if the runner-up is slower by more than the band
`max(3 x larger relative MAD, 5%)`; otherwise TIE. In this run `3 x relative MAD` never exceeded 1.63%, so the
band was 5% in every cell. Toolchain-only candidates (Omega's own encoder, spec section 4) and weak baselines
(`R1_plain`) are measured but not selectable.

## Results per shape

Medians in ns per call on one X925 core at 3.90 GHz. "Best C" and "best asm" are the fastest candidate of that
language in the cell.

### S1: m=1, n=4096 (call overhead)

| sparsity | selected | median | runner-up | margin | decision | best Mojo |
|---|---|---|---|---|---|---|
| 0.00 | asm_sdot (asm) | 36.0 | R1_sdot@O3 (C) 36.8 | +2.1% | TIE | MJ1_sdot 41.1 |
| 0.33 | asm_sdot (asm) | 35.9 | R1_sdot@O3 (C) 36.7 | +2.3% | TIE | MJ1_sdot 40.9 |
| 0.66 | R1_sdot@O3 (C) | 36.8 | asm_sdot (asm) 37.0 | +0.7% | TIE | MJ1_sdot 41.1 |

- For S1 on one X925 core, hand asm (int8 SDOT) and C int8 SDOT at -O3 are tied (within 5%).
- For S1 on one X925 core, the Mojo int8 kernel MJ1_sdot was about 12-14% slower than the tied leaders
  (41.1 vs 36.0-36.8 ns). This is outside the band in every S1 cell.
- asm_sdot moved from 35.9-36.0 ns at sparsity 0 and 0.33 to 37.0 ns at 0.66 (2.9%), although a dense int8
  kernel does the same work at any sparsity. Its within-cell MAD is 0.03 ns, so this is a real but unexplained
  per-cell effect, not sampling noise. It is the kind of shift the 5% floor exists for.

### S2: m=256, n=1024 (cache-resident, 256 KiB int8 in L2)

| sparsity | selected | median | runner-up | margin | decision | best Mojo |
|---|---|---|---|---|---|---|
| 0.00 | asm_sdot (asm) | 2187.2 | R1_sdot_il@O3 (C) 2191.2 | +0.2% | TIE | MJ1_sdot 2224.6 (+1.7%) |
| 0.33 | R1_sdot_il (C) | 2191.2 | R1_sdot_il@O3 (C) 2191.8 | +0.0% | TIE | MJ1_sdot 2235.5 (+2.0%) |
| 0.66 | R1_sdot_il (C) | 2191.3 | R1_sdot_il@O3 (C) 2191.9 | +0.0% | TIE | MJ1_sdot 2228.2 (+1.7%) |

- For S2 on one X925 core, asm, C (interleaved SDOT, -O2 or -O3) and Mojo int8 kernels are all within 2.1%
  of each other: no language wins. The five fastest selectable candidates in each cell span under 3%.
- Crumb (2 bit per weight) kernels are 19-24% slower here (asm_crumb +19.3%, R2c_crumb +24.0% at sparsity
  0.66): at this size the weights sit in L2 and decoding crumbs costs more than the smaller footprint saves.

### S3: m=4096, n=4096 (memory bandwidth)

| sparsity | selected | median | runner-up | margin | decision | best C |
|---|---|---|---|---|---|---|
| 0.00 | MJ2c_crumb (Mojo) | 139,187 | asm_crumb (asm) 160,802 | +15.5% | CHOSEN | R2c_crumb@O3 164,564 (+18.2%) |
| 0.33 | MJ2c_crumb (Mojo) | 138,958 | asm_crumb (asm) 161,368 | +16.1% | CHOSEN | R2c_crumb@O3 165,828 (+19.3%) |
| 0.66 | MJ2c_crumb (Mojo) | 139,135 | R2c_crumb@O3 (C) 160,978 | +15.7% | CHOSEN | (runner-up); asm_crumb 161,732 (+16.2%) |

- For S3 on one X925 core, the Mojo crumb kernel MJ2c_crumb beat the hand asm crumb kernel and the best C
  crumb kernel by 15.5% to 19.3% on the median, at all three sparsities. The asm minimum at sparsity 0
  (157,834 ns) is still 13.4% above the Mojo median.
- For S3 on one X925 core, every int8 (1 byte per weight) kernel was 228-297% slower than MJ2c_crumb, in all
  languages: the representation (crumb2, 4x fewer weight bytes) decides this shape; language choice then decides
  about 16% within crumb2.
- The claim is limited to this session. The earlier paired rerun used to set the 5% floor showed run-to-run
  median shifts up to 10.4% (p99), so the 15-16% margin should be re-measured in a second run before it is
  treated as stable.
- Each S3 sample holds only 8 (asm) to 16 (Mojo) calls, because one call takes 139-162 us.

### S4: m=64, n=1024, 256 different x per packed W (compute-bound)

| sparsity | selected | median | runner-up | margin | decision | best C |
|---|---|---|---|---|---|---|
| 0.00 | asm_sdot (asm) | 414.8 | MJ1_sdot (Mojo) 419.5 | +1.1% | TIE | R1_smmla 420.4 (+1.4%) |
| 0.33 | asm_sdot (asm) | 413.8 | MJ1_sdot (Mojo) 418.8 | +1.2% | TIE | R1_smmla 420.4 (+1.6%) |
| 0.66 | asm_sdot (asm) | 415.7 | MJ1_sdot (Mojo) 419.6 | +0.9% | TIE | R1_smmla 420.3 (+1.1%) |

- For S4 on one X925 core, hand asm SDOT, Mojo int8 SDOT and C int8 SMMLA are tied (within 1.6%).
- Plain C SDOT (R1_sdot) was 29-30% slower than those three at every sparsity; crumb kernels 35-66% slower.

### Omega's own encoder (toolchain-only, not selectable)

The encoder candidates are machine code emitted at run time by Omega's own AArch64 encoder. Their gate is the
byte comparison report (spec section 4), not speed; they are listed for comparison only.

- enc_sdot was within 0.3% of asm_sdot in every S1, S2 and S4 cell except S1 sparsity 0.66, where it was
  2.6% faster (36.1 vs 37.0 ns), the cell where asm_sdot shows its unexplained shift. At S4 sparsity 0.66 it was
  0.3% faster (414.7 vs 415.7 ns), inside the band.
- enc_crumb was within 0.6% of asm_crumb at S3 (161,320 vs 160,802 ns at sparsity 0), and so also about 16%
  behind MJ2c_crumb.

## Code size and build time

Code size = run + pack function symbols (or the candidate's named code parts: Mojo kernels behind the C adapter,
encoder-emitted bytes plus C thunk and pack). Build time = min of 5 wall runs of the object's build command. For
C and Mojo that object holds several realizations, so the time is shared, not per candidate. Source lines and
unsafe surface are per source file, informational only.

| candidate | language | repr | code bytes | build time | build note | source lines (file) | unsafe surface (file) |
|---|---|---|---|---|---|---|---|
| asm_sdot | asm | int8 | 1,112 | 5.4 ms | gnu-as 2.42 | 214 | 163 |
| asm_crumb | asm | crumb2 | 1,224 | 5.3 ms | gnu-as 2.42 | 162 | 113 |
| enc_sdot | Omega encoder | int8 | 1,160 | 40 us | in-process emission | 814 | 5 |
| enc_crumb | Omega encoder | crumb2 | 1,276 | 48 us | in-process emission | 814 | 5 |
| MJ1_sdot | Mojo | int8 | 2,036 | 356 ms | mojo 1.0.0, shared object | 250 | 0 |
| MJ2c_crumb | Mojo | crumb2 | 1,716 | 356 ms | mojo 1.0.0, shared object | 250 | 0 |
| R1_sdot / @O3 | C | int8 | 1,792 / 1,792 | 177 / 195 ms | gcc 13.3.0, shared object | 318 | 0 |
| R1_sdot_il / @O3 | C | int8 | 1,012 / 1,084 | 177 / 195 ms | gcc 13.3.0, shared object | 318 | 0 |
| R1_smmla / @O3 | C | int8 | 1,412 / 2,112 | 177 / 195 ms | gcc 13.3.0, shared object | 318 | 0 |
| R2c_crumb / @O3 | C | crumb2 | 1,628 / 1,624 | 197 / 228 ms | gcc 13.3.0, shared object | 389 | 2 |
| R2_bitplane / @O3 | C | bitplane | 2,224 / 2,872 | 197 / 228 ms | gcc 13.3.0, shared object | 389 | 2 |
| R1_plain / @O3 | C | int8 | 708 / 708 | 177 / 195 ms | weak baseline | 318 | 0 |

Code size spread among the winners and ties is small (1,012 to 2,036 bytes). Mojo's unsafe-surface count of 0 is
what the rule (assembly lines + pointer casts in the file) measures; it does not mean the Mojo code has no raw
pointer use.

## Pack cost

Packing converts W into the candidate's layout once. The receipts give per-weight pack cost and the number of
calls after which a faster kernel pays back its extra pack time against the cheapest-to-pack candidate
(MJ1_sdot, about 0.07-0.08 ns per weight).

- S3: MJ2c_crumb packs at about 0.60 ns per weight (10.0 ms for 16.8 M weights) and pays back after 22-23 calls.
- S1, S2, S4 selected kernels pack at 0.58-0.79 ns per weight; break-even against MJ1_sdot is 448-492 calls
  (S1), 3,969-5,110 calls (S2) and 7,483-8,862 calls (S4). Because the S1, S2 and S4 winners are ties, the
  cheaper-to-pack tied candidate is a legitimate choice when W is used only a few thousand times.

## Independent statistics check (Grok)

Grok (`grok -p`, one shot) was given the per-cell top-four medians, MADs, mins, N and the decisions. Its view,
with every figure it cited checked against the receipts:

- The tie band is 5% in every cell; the MAD term never binds (largest `3 x relMAD` is 1.63%, S3 asm_crumb at
  sparsity 0). Against within-run sampling error alone 5% is very conservative; against the earlier run-to-run
  scatter (p95 3.6%, p99 10.4%) it is about right.
- The S3 CHOSEN decisions are justified: gaps of 21,615-22,410 ns against MADs of 127-876 ns.
- The 2.9% S1 asm_sdot shift across sparsity is real (about 36x its MAD) and shows that orderings under about 5%
  are not identified by this run. The 5% floor is what keeps S1 from a false ranking.
- A qualified S3 statement matches the data; generalising to "Mojo beats asm" or treating 15.5% as stable
  past this session would over-claim.
- Grok also estimated, from a fitted tail, that about 3% of run-to-run shifts exceed 5%. That is its model, not a
  receipt figure, and is not relied on here.

## What this run does not show

- Anything about other operations, other shapes, other cores (A725), several cores, or the GPU.
- That Mojo is generally faster than hand asm or C: it won one shape (S3) and tied or lost the other three.
- Energy: no energy or power was measured.
- Stability across runs: this is one run. A second timed run is needed before the S3 margin is used for
  automatic selection.

## Confirmation run (2026-09-29, run 20260929T191231Z-071bfdbd76c7)

Grok asked for a second run before trusting the S3 margin. Same machine, same core, quiet flag up, 72 receipts, timed wall 14.8 s.
Receipts are kept under `evidence/POLYGLOT/confirm-20260929T191231Z-071bfdbd76c7/` (the bench writes fixed file names, so the
confirmation copy is filed separately and the primary run's receipts stay unchanged).

S3 medians (ns), best four per sparsity:

| sparsity | MJ2c_crumb | asm_crumb | enc_crumb | R2c_crumb@O3 |
|---|---|---|---|---|
| 0.00 | 139262 | 160404 | 167166 | 163588 |
| 0.33 | 139479 | 158946 | 162982 | 163122 |
| 0.66 | 139441 | 161312 | 161990 | 163292 |

The S3 winner is unchanged: the Mojo 2-bit kernel beats hand assembly by 14.0 to 15.7% and the best C by 17.1 to 17.5%.
The qualified statement stands for S3 on one X925 core at 3.90 GHz. Known gap: the bench overwrites receipts by file name;
immutable run-named receipt files are a follow-up.
