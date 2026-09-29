# MIXED_ALGEBRA_DIGITAL_V1: closure gate (pre-registration)

Status: PRE-REGISTERED. This file is committed before any timed run of the
gate. Every threshold, query, receipt rule and PASS/FAIL condition below is
fixed here; the gate implementation (`make gate-mixed-algebra-digital-v1`)
checks exactly these and nothing looser. Results are written to a
digest-named wrapper receipt, never back into this file.

Scope: the digital (CPU) leg of the mixed-algebra program. It closes the
question "do we have several verified, exact, differently-built realizations
of one operation, measured, and a recorded, reproducible selection among them
that can say why a realization was not chosen". It does not claim energy
results and does not introduce any new realization.

## 1. Operation (no redefinition)

Omega-X is the existing exact ternary GEMV contract, unchanged:
`spec/mixed-algebra-ma2.md`, section "Operation Omega-X", and
`src/algebra/realize_common.h`:

    y = W . x,  W in {-1,0,+1}^(m x n) (row-major int8), x in int8^n,
    y in int32^m, EXACT (bit-identical to oma_rz_oracle), n <= 16,777,215.

The TURING contract digest used by the selector is the PROVISIONAL digest of
this same contract (`turing_contract_omega_x`, `src/turing/field.h`).

## 2. Realization families (R1 / R2 / R3)

Families are counted by the registry `family` string of `oma_rz_impl`,
literally (7 distinct values over 10 realizations, `oma_rz_get` order):

| class | family string | realizations | max_n |
|---|---|---|---|
| R1 conventional binary / int8 | `binary` | R1_plain (weak baseline), R1_sdot, R1_sdot_il, R1_smmla | 16,777,215 |
| R2 packed ternary | `bitplane` | R2_bitplane (H1 two-bitplane code) | 16,777,215 |
| R2 packed ternary | `lut` | R2b_lut | 16,777,215 |
| R2 packed ternary | `crumb2` | R2c_crumb (2-bit two's-complement crumb code) | 16,777,215 |
| R2 packed ternary | `dense5` | R5_dense5 (5 trits / byte) | 16,777,215 |
| R3 residue | `rns` | R4_rns (RNS {256,255,253}) | 64,512 |
| (sparse, counted as its own family) | `sparse` | R3_sparse (uint16 index lists) | 65,536 |

Note on names: the registry id `R3_sparse` is unrelated to class R3; class R3
here means residue arithmetic (`R4_rns`). The class column is only a
grouping for the >= 3-class check in section 7; the family string is what the
gate counts.

### Two distinct 2-bit trit encodings (both retained)

- H1 two-bitplane code (`src/algebra/oma_trit.h`, reference spec "Encoding
  (H1)"; used by the reference library and by R2_bitplane): a trit is a
  (pos, neg) bit pair. (0,0) = 0, (1,0) = +1, (0,1) = -1, (1,1) = ⊥ (invalid).
  As the 2-bit code value (bit0 = pos, bit1 = neg): 0 = 0, 1 = +1, 2 = -1,
  3 = ⊥.
- Crumb code (R2c_crumb, `src/algebra/realize_bitplane.c`): 2-bit two's
  complement, 00 = 0, 01 = +1, 11 = -1; 10 is never written (it would decode
  to -2, outside the contract). 10 is the crumb code's ⊥.

The ⊥ rule, explicit: ⊥ is never a value. In the reference library every
operation (per-trit neg/add/mul, block validate/decode/neg/add/mul/dot,
pack/unpack, serialize) given ⊥ returns an error code
(`OMA_E_INVALID_CODE` / `OMA_E_INVALID_PLANES`) and leaves its outputs
untouched; ⊥ does not "propagate" as a result value, it stops the operation.
Realizations take the canonical int8 matrix; any weight outside {-1,0,+1}
(which is the only way ⊥ could reach them) is refused at pack time with
`OMA_RZ_E_TRIT`, and no realization's packed form may contain its own ⊥
pattern.

## 3. Correctness leg

All of these must pass (plain build and ASan+UBSan build where listed):

1. `make test-algebra` and `make test-algebra-asan` (reference library;
   sections include "invalid (1,1) rejection", "per-trit truth tables",
   "pack/unpack random", "errors leave outputs").
2. `make test-realize` (plain + ASan/UBSan): every realization bit-identical
   to `oma_rz_oracle` on the same corpus: structured tail shapes x
   deterministic W kinds (all 0, all +1, all -1) x x kinds (all -128, all 127,
   alternating), random sparsity; 400 random shapes; magnitude and boundary
   cases (n = 16,384, 64,512, 64,513 refused by RNS, 65,536, 65,537 refused
   by sparse, 100,003); each plan run twice; rejections.
3. `make test-turing` (the selector the gate uses; plain + ASan/UBSan).
4. NEW `tests/algebra/test_ma_digital_bottom.c` (plain + ASan/UBSan), ⊥
   coverage at the realization layer, which no existing test covers:
   - H1 code: ⊥ round trip is refused (encode of every invalid int8 value,
     decode/unpack of a (1,1) block) and add/mul with ⊥ on either side
     return the error code with outputs untouched (re-asserted at the gate
     level);
   - every one of the 253 int8 values outside {-1,0,+1}, placed at a random
     position of an otherwise valid W, is refused by every realization's pack
     with `OMA_RZ_E_TRIT`;
   - R2_bitplane packed planes never contain (pos=1, neg=1) at any bit, and
     decode back to W (layout: 128-weight chunks, 16 bytes pos then 16 bytes
     neg, bit k of byte j = column 128c + 16k + j);
   - R2c_crumb packed bytes never contain crumb 10, and decode back to W
     (layout: 64-weight chunks of 16 bytes, bits 2k..2k+1 of byte j = column
     64c + 16k + j);
   over deterministic, random, all +1, all -1 and boundary (tail) shapes.
5. The benchmark re-verifies every realization bit-exact against the oracle
   on every grid cell; each fresh receipt must report
   `correctness.mismatches == 0`.

## 4. Measurement leg

Receipts that feed the decision (primary): exactly the two FRESH receipts
written by `make bench-mixed-algebra MA2_RUN_ID=<id>` during the gate run:
`evidence/MIXED_ALGEBRA/runs/<id>/ma2_bench_run1.json` then
`.../ma2_bench_run2.json`, ingested in that order (store order is part of the
decision digest: ties break on lowest spec index and cites are in store
order). Their schema is `OMEGA_MIXED_ALGEBRA_MA2_BENCH_V1` (the bench's
current writer); the committed `ma3_bench_run{1,2}.json` carry
`OMEGA_MIXED_ALGEBRA_MA3_BENCH_V1`. `turing_ingest_receipt` accepts both
(prefix `OMEGA_MIXED_ALGEBRA_MA` + `_BENCH_V1`).

The committed `ma3_bench_run{1,2}.json` do NOT feed the primary decision. The
gate also computes a continuity decision per query from those two committed
receipts alone and records whether its winner agrees; this is informational
and never affects PASS.

Tier guard (so the decision cannot silently cite old evidence): ingest gives
a row tier E2 only when its receipt reports `mismatches == 0` and the row is
oracle-verified, else E0, and the selector drops non-E2/E3/E4 rows with
`TURING_R_TIER`. The gate additionally asserts that every evidence digest
cited by every primary decision resolves to a row whose receipt digest is one
of the two fresh receipt digests, and that each fresh receipt has
`run_commit` = the gate's HEAD and `tree_dirty` = false.

Fresh-run requirements: not `OMA_BENCH_QUICK`; the quiet flag
`~/workspace/.spark-quiet` must be absent before the run (the gate refuses to
start the timed leg otherwise), is created for the timed leg and removed on
exit, success or failure. One heavy test at a time.

Reuse mode: `MA_DV1_REUSE_RUN=<id>` skips the bench and uses the existing
receipts of `runs/<id>/`. The wrapper receipt records `mode: reuse`; all
other checks (including run_commit, tree_dirty and cite checks) apply
unchanged, so reuse can only reproduce the decision of the run that made
those receipts at the same commit.

Measured (from the bench, per realization and grid cell): per-call latency
(median and best of >= 9 interleaved blocks, q25/q75), pack cost, within-run
spread (noise_rel, pack_noise_rel), weight bytes read per call, working-set
bytes. Grid: n in {1024, 4096, 16384} x m in {1, 64, 4096} x sparsity
{0, 0.3, 0.6, 0.9}, one pinned Cortex-X925 core.

NOT measured in this gate: energy. The bench writes indicative whole-cluster
energy rows into its receipt, but ingest stops at the `"energy"` section, so
no energy number reaches any decision; energy is pending omega#86. Also not
measured: multi-core, GPU, cold-cache, x-vector batching.

The bench target also runs the RETIRED MA-2 selector and writes
`ma2_select_receipt.json` into the run directory; it is kept as the bench
writes it and is not used by this gate.

## 5. Selection leg

Selector: the EXISTING TURING Field selector v1 (`turing_field_select`,
`src/turing/field_select.c`), unchanged. The `turing-field` CLI cannot express
these queries (it hardcodes shapes S1-S4, has no n/m/pack arguments and no
query above any max_n), so the gate uses a small new C driver
(`tests/algebra/ma_digital_v1_gate.c`) that calls only public `turing_*`
functions: `turing_ingest_registry`, `turing_ingest_receipt`,
`turing_field_select` (no supersedes), `turing_decision_digest`,
`turing_decision_verify`.

Pre-registered queries (sparsity 0.30, a measured grid value; sparsity does
not change the footprint):

| id | shape | n | m | pack | expected cell |
|---|---|---|---|---|---|
| Q1 | L2-resident (256 KiB int8 W) | 4096 | 64 | once | exact |
| Q2 | L2-resident | 4096 | 64 | per_call | exact |
| Q3 | DRAM-bound (64 MiB int8 W) | 16384 | 4096 | once | exact |
| Q4 | DRAM-bound | 16384 | 4096 | per_call | exact |
| Q5 | above RNS and sparse max_n | 65537 | 64 | once | nearest (16384 x 64) |
| Q6 | above RNS and sparse max_n | 65537 | 64 | per_call | nearest (16384 x 64) |

Q5/Q6 are the named-rejection queries: from the real registry, R4_rns
(max_n 64,512) and R3_sparse (max_n 65,536) must be recorded with
`TURING_R_MAX_N`. Their other 8 candidates are ranked on the nearest measured
footprint, so the decision records `exact_cell = 0`; this is expected and is
not an extrapolation claim about n = 65,537 timings. No broken or non-exact
realization is added.

"Verified-eligible" = candidate reason in {CHOSEN, RANKED, TIED}, i.e. it
passed the contract, exactness, max_n, receipt-verify, oracle-verified-run,
contention and tier filters and has E2 evidence in the footprint. The weak
baseline R1_plain is not filtered (Field v1 has no weak-baseline reason code)
and ranks like any other candidate.

## 6. Reproducibility

- The decision digests (`turing_decision_digest`, domain
  `turing.decision.v1`) must be bit-identical when selection is re-run from
  the stored receipts: (a) a second, independently ingested store in the same
  process; (b) a separate process of the driver; (c) a separate process of an
  ASan+UBSan build of the driver. Receipt paths are not part of any digest.
- Oracle checks are bit-identical by construction (section 3).
- Timings are NOT expected to reproduce. The wrapper receipt records, per
  query, the winner's and runner-up's mean cost, the margin, the selector's
  noise band (largest within-run spread of winner and runner-up rows) and the
  run1-vs-run2 relative difference of the winner's footprint cost. A winner
  whose margin is inside the band is reported as such (not a stable win).

## 7. PASS criteria (all required)

1. Every correctness command in section 3 passes; both fresh receipts report
   0 mismatches.
2. Clean tree: no modified or deleted tracked file anywhere
   (`git status --porcelain --untracked-files=no` empty) and no untracked file
   under `src tests tools spec Makefile`; untracked fresh receipts under
   `evidence/MIXED_ALGEBRA/runs/<id>/` are expected and listed by path and
   digest. Recorded as `tree_dirty`; must be false. Both fresh receipts have
   `run_commit` = HEAD and `tree_dirty` = false.
3. Every query returns a chosen realization (verdict CHOSEN) and
   `turing_decision_verify` succeeds (every cited receipt re-hashes).
4. Every cited evidence row of every primary decision comes from a fresh
   receipt (section 4 tier guard).
5. Families: in Q1-Q4 all 10 realizations are verified-eligible, spanning all
   three classes R1 (binary), R2 (bitplane/lut/crumb2/dense5), R3 (rns); in
   every query the verified-eligible set spans >= 3 distinct family strings.
6. Named rejections: in Q5 and Q6 exactly R3_sparse and R4_rns carry
   `TURING_R_MAX_N` and nothing else is filtered; at least one named
   rejection overall.
7. Reproducibility (a), (b) and (c) of section 6 all give identical digests.
8. The wrapper receipt is written to
   `evidence/MIXED_ALGEBRA/digital_v1/<sha256 of its bytes>.json`, never over
   an existing file, and records: run_commit, tree_dirty, mode, run id, every
   receipt path + digest, per-query decision digest, chosen realization,
   reason + cost for every candidate, margin, band, families, rejections,
   continuity result, correctness totals, reproduction results and the
   verdict `MIXED_ALGEBRA_DIGITAL_V1 PASS` or `MIXED_ALGEBRA_DIGITAL_V1 FAIL`.

FAIL: any of 1-7 not met. A FAIL receipt is still written (from a clean tree)
with the failing checks named. A dirty tree never publishes a receipt into
`evidence/`; its receipt stays under `build/`. The quiet flag being present is
not a FAIL: the gate refuses to start the timed leg and writes nothing.

## 8. What PASS does not claim

No energy result; no claim that ternary beats int8 in general (earlier
MA-2/MA-3 results: ternary wins only when DRAM-bound); Q5/Q6 timings are the
nearest footprint's; the selector's rule is the control arm's minimum-mean
rule (TURING K.7), not a novel selector.
