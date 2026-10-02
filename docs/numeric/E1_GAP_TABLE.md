# E1 Numerical Closure: Gap Table

Status: research note, 2026-09-30, read-only survey of `origin/main` at `529ebfa`; rows refreshed 2026-10-01 (see the status summary below).
No code was built or run for this note.

Update 2026-10-02: Gate 5 chip PASS on the corrected #225 candidate 1720a8d + physics e95e3ed, receipt `evidence/OMEGA-NUMERIC-0/3bb806d2...e5bab8.json`. M19 on that pair is INCOMPLETE: accuracy and build/tooling PASS; endurance NOT ESTABLISHED (the 1,000-job loop exited before its first GPU job), so M19 is not qualified and #225 is neither qualified nor disqualified by it. Endurance reruns on current main after E1. See `evidence/GATE14-FOUNDATION/legs-1720a8d/README.md`.

Plan source: `aien-architecture/CURRENT_EXECUTION_PLAN.md`, section 8, "E1. Numerical closure"
(must close before M20 tensor training). Evidence source: the merged M19R Gate 5
(OMEGA-NUMERIC-0, omega#107, merge `62f5ba5`), canonical receipt
`evidence/OMEGA-NUMERIC-0/dc4e6012...f860db.json` (run on omega `8024e9a` + physics `e95e3ed`, PASS 23/23).


## Status summary, 2026-10-01 (reconciled at omega `07004a8`)

**E1 is NOT closed. Verdict: PARTIAL, 2 of 6 exit requirements met.** Rows 1 to 10 below were refreshed on
this date; sections 1 and 4 and the "Existing evidence (file:line)" figures for rows 2 and 6 to 8 are the
original 2026-09-30 survey and are kept as history. Merges: #124 `d3194f6`, #127 `7a5a13c`, #134 `c54d492`,
#147 `2d8cd68`, #152 `07004a8` (all verified).

The six exit requirements are those in aien-architecture `CURRENT_EXECUTION_PLAN.md` section 8, E1:

| Req | Subject | State | Evidence |
|---|---|---|---|
| 1 | General load/store | Not met | Row 2 unchanged: fixed addressing patterns only |
| 2 | Scalar FP32 ops | Met | #124 (CPU) and #147 (GB10); receipts `evidence/OMEGA-NUMERIC-0/f4f6d362...json` (untrusted: produced while the #147 provenance-copy overflow was live, fixed by #152) and `dafb645a...json`; no receipt yet on merged main |
| 3 | Reductions incl. MAX/MIN/MEAN on GB10 | Partial | SUM/MAX/MIN/MEAN chip parity PASS, 380 cases, 0 mismatches (`evidence/E1-REDUCE/a04f4f7f...json`, omega#157, pre-rebase commit 689ad4d); MEAN final division is a declared host step; no receipt yet on merged main |
| 4 | Defined DIV and SQRT | Met on the main GB10 path | #152: Gate 5 PASS 26/26, 47 parity lines incl. DIV and SQRT, receipt `evidence/OMEGA-NUMERIC-0/dafb645a9692aab551327c4de7c5dfd7d194504b1ae2ac998a0f5480c687cbb2.json`, run on commit `7852570`, code unchanged at merge |
| 5 | Transcendentals | Partial | Owner decision made: aien-architecture #76 (merge `15808f9`) chose Option 1, frozen sequences with a declared per-op error bound, bit-identical on CPU and GB10; RSQRT, DIV and SQRT stay correctly rounded. Nine ops on CPU (#127); `test-numeric-transc` runs in PR smoke CI. GB10: EXP2, LOG2, SIGMOID, TANH chip bit parity over all 2^32 inputs (row 10, `evidence/E1-TRANSC-GB10/`). RSQRT, ERF, SIN, COS, GELU have no GB10 kernel; EXP/LOG of `OP_TABLE` still refused on GB10 |
| 6 | CPU/GB10 parity | Partial | Holds for the ops in the Gate 5 receipt; not for MAX/MIN/MEAN or transcendentals |

Receipt naming caveat: a receipt's file name is its internal `receipt_digest`, not the sha256 of the file.
For `dafb645a...` and `dc4e6012...` the file hashes differently (`dafb645a...` file sha256 begins `3e6bfec8`).
Verify content through the receipt's own fields (`digest_meaning` in each receipt says "integrity only,
not authenticity").

## 1. What Gate 5 actually counts

The brief for this note said "about 36 checks". No artifact on `origin/main` has 36. The real counts:

| Count | What it is | Source |
|---|---|---|
| 23 | Gate 5 test IDs, all PASS on chip (9 host positive, 5 chip-only, 9 negative) | receipt `test_results`; `tests/test_omega_numeric.c` `report(...)` calls, lines 551-1042 |
| 22 | GB10 parity lines (FFMA x8 constant values + 14 other ops), 4096 inputs each (REDUCE_SUM: 128 sums) | receipt `gb10_parity`; `tools/test_numeric_qualify.sh:99` |
| 33 | Pre-submission `CHECK:` markers on the kernel/patch/QMD validator, each proven load-bearing by mutation | `src/omega_numeric.c`; `tools/numeric_check_sweep.sh` |
| 133 | Self-checks of the gate runner/qualifier (physics pin, log parsing, receipt writer) | `tools/test_numeric_qualify.sh` |

The orchestrator should decide which count the plan means. This table maps the plan to the 23 gate
tests and the 22 parity lines, since those are the evidence of numeric behavior.

### The 23 Gate 5 tests (file `tests/test_omega_numeric.c`)

| Test ID | Line | Covers |
|---|---|---|
| FPCR_RNE_NO_FTZ_REQUIRED | 551 | Host FP mode is round-to-nearest-even, no flush-to-zero |
| PROVENANCE_MATCHES_EXECUTOR | 574 | Opcode provenance table matches the words the executor sends |
| NOT_ENCODED_OPS_REFUSED_BEFORE_SUBMISSION | 623 | DIV, SQRT, EXP, LOG and unencoded variants refused before any device work (line 580) |
| NEG_BAD_SHARED_AND_WARP_SHAPES_REFUSED | 651 | Bad CTA/warp sizes refused |
| NEG_PATCH_STRUCTURE_CHECKED_BEFORE_SUBMISSION | 735 | Mutated kernel words caught by the 33 checks |
| FP32_SIMT_OPCODES_ENCODED | 751 | 15 encoded ops build and pass structural checks |
| OMEGA_MATH_SEQUENCES_QUALIFIED | 846 | CPU DIV/SQRT bit-exact vs IEEE + hard cases; EXP/LOG edge and self-consistency |
| WARP_REDUCTION_ORDER_DECLARED | 862 | CPU 32-lane sum in frozen order `PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1` |
| CPU_TIER_EQUALS_REFERENCE | 874 | CPU realization equals reference for every op |
| CPU_TIER_SUBNORMALS_PRESERVED | 875 | CPU keeps subnormals |
| CPU_TIER_INDEPENDENT_ORACLE | 877 | Integer soft-float oracle; binary128 EXP/LOG ulp bounds (EXP 40, LOG 4; lines 379-380) |
| CPU_GB10_BIT_PARITY | 908 | 22 GB10 parity lines, 0 mismatches |
| SUBNORMALS_PRESERVED_NO_FTZ | 909 | GB10 keeps subnormals |
| MUFU_SEED_ONLY_NOT_COMPARED | 910 | MUFU RCP/RSQ only held to a 2^-20 seed bound, never bit-compared |
| EDGE_CLASS_BEHAVIOR_VERIFIED | 911 | Per-class (zero, subnormal, normal, inf, NaN) parity |
| HARDWARE_DESCRIPTOR_PROBED | 912 | Real device probe, not a fake descriptor |
| NEG_FTZ_DETECTED_AND_REJECTED | 934 | A flush-to-zero result is caught |
| NEG_UNORDERED_REDUCTION_DIVERGENCE_CAUGHT | 945 | A different sum order gives different bits and is caught |
| NEG_RAW_MUFU_APPROX_REJECTED_WITHOUT_REFINEMENT | 961 | Raw MUFU is not accepted as division |
| NEG_UNKNOWN_OPCODE_FAILS_CLOSED | 970 | Unknown op name refused |
| NEG_OPCODE_PROVENANCE_INTEGRITY_VERIFIED | 999 | Tampered provenance caught |
| NEG_NONDEFAULT_FPCR_REFUSED | 1032 | Non-default host FP mode refused |
| NEG_COMPARATOR_CATCHES_ONE_BIT | 1042 | A one-bit difference fails the comparator |

GB10-encoded ops (bit- or int-exact on chip): FADD, FSUB, FMUL, FFMA, FSETP_SEL, FSEL,
FMNMX_MIN, FMNMX_MAX, I2FP, F2I, LDS_STS, SHFL_DOWN, REDUCE_SUM. Seed-bound only: MUFU_RCP, MUFU_RSQ.
CPU-only (refused on GB10): DIV, SQRT, EXP, LOG (`src/omega_numeric.c:336-343`, `590-594`).

## 2. Gap table

Status key: **Covered**, **Partial**, **Missing**. Size: S (under a day), M (a few days), L (a week or more).
GPU column: **Yes** means the step needs a chip run on the GB10. Those runs go one heavy test at a time,
later, not now.

| # | Plan item | Status | Existing evidence (file:line) | Gap | Next step | Size | GPU |
|---|---|---|---|---|---|---|---|
| 1 | FP32 arithmetic | Covered (CPU and main GB10 path) | FADD/FSUB/FMUL/FFMA bit-exact (`CPU_GB10_BIT_PARITY`); per-element FFMA_V (c loaded from memory) defined on CPU by omega#124 (merge `d3194f6`) and encoded on GB10 by omega#147 (merge `2d8cd68`); chip receipt `evidence/OMEGA-NUMERIC-0/f4f6d362ad540e736ba747c40fe8b594141821f378d50ccaf7d83f90db0ef81b.json` (PASS 26/26, 45 parity lines) and later `dafb645a9692aab551327c4de7c5dfd7d194504b1ae2ac998a0f5480c687cbb2.json` (PASS 26/26, 47 lines) | None named for the scalar ops. Original gap (FFMA third operand one constant per launch) closed by FFMA_V. | None | - | No |
| 2 | FP32 load/store | Partial | Global STG fixed form (`src/omega_numeric.c:675` CHECK:stg_form); shared LDS_STS mirror exchange inside one 64-thread CTA (`omega_numeric.c:332`) | Only one fixed addressing pattern each; no general strided, offset or vector loads/stores | Define a load/store contract (offset, stride, alignment) with encoder checks, then chip parity | M | Yes |
| 3 | Comparison / select | Covered (CPU and main GB10 path) | 13 `FSETP_*_SEL` predicates plus FSEL and FMNMX; CPU contract omega#124 (merge `d3194f6`, `docs/numeric/E1_SCALAR_CONTRACT.md`); GB10 encoding omega#147 (merge `2d8cd68`); chip parity lines with 0 mismatches in receipts `f4f6d362...` and `dafb645a...` (`evidence/OMEGA-NUMERIC-0/`) | None for the original gap (only GE existed) | None | - | No |
| 4 | Conversion | Covered (CPU and main GB10 path) | I2FP, F2I; added `F2I_FLOOR/CEIL/RNI`, `F2U`, `I2FP_U32`, `F32<->F16`, `F32<->BF16` on CPU by omega#124 (merge `d3194f6`; 2^32-input exhaustive run on the 9 unary ops, 0 mismatches); GB10 encodings omega#147 (merge `2d8cd68`); chip parity 0 mismatches in receipts `f4f6d362...` and `dafb645a...` | FP16/BF16 arithmetic is not defined (conversions only) | None | - | No |
| 5 | Reductions | Covered on GB10 for SUM/MAX/MIN; MEAN = chip SUM + one declared host division | omega#134 (merge `c54d492`): SUM, MAX, MIN, MEAN over any length n >= 0 in frozen order `RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL`; CPU verdict `PASS_EXCEPT_DECLARED_CHIP_ONLY`. GB10 SUM receipt `evidence/E1-REDUCE/be9d61ce...json` (omega 6da80bf). GB10 MAX/MIN via a reduce-owned SHFL.DOWN+FMNMX patch (omega#157): receipt `evidence/E1-REDUCE/a04f4f7f...json` (PASS, SUM/MAX/MIN/MEAN 380 cases, 0 mismatches, omega 689ad4d, pre-rebase) | MEAN final division still on the host (GB10 DIV is on the main path since omega#152; switch pending); no single-launch shared-memory CTA kernel; no receipt yet on merged main | Switch MEAN to GB10 DIV; consolidated chip run on merged main; optional CTA kernel with the same bits | S | Yes |
| 6 | Division and square root (definition) | Covered on CPU | Integer-only correctly rounded sequences, bit-exact vs IEEE, hard cases (`tests/test_omega_numeric.c:756-801`); header records full SQRT sweep and 10^9 DIV pairs (`src/omega_numeric.h:112-123`) | Full DIV sweep is sampled, not exhaustive (not feasible for two inputs; acceptable) | None for definition | - | No |
| 7 | Division and square root on GB10 | Closed (chip PASS) | Standalone GB10 kernels `src/omega_numeric_divsqrt_gb10.c` (DIV 264 words, SQRT 336 words, every word from Omega's encoder and checked by nvdisasm); `make test-divsqrt-host`; gate `tools/run_divsqrt_gate.sh`; receipt `E1-DIVSQRT/de7b2dd6a930ae94529773cf4e31aa08aac7065602b1d771e13e6ed2784a9074.json` ; wired into the main GB10 path (`omega_numeric_gb10.c` dispatches DIV/SQRT to `omega_ds_gb10_run`); Gate 5 chip parity 0 of 4096 mismatches each, receipt `OMEGA-NUMERIC-0/dafb645a9692aab551327c4de7c5dfd7d194504b1ae2ac998a0f5480c687cbb2.json` | None for DIV/SQRT | None | - | No |
| 8 | Transcendental: exact sequences (wording) | Met (decided) | EXP/LOG frozen polynomial sequences (`omega_numeric.h:125-131`); edge and round-trip checks (`test_omega_numeric.c:803-844`); binary128 oracle bound EXP 40 ulp, LOG 4 ulp (lines 379-380) | Decided by aien-architecture #76 (merge `15808f9`, Option 1): "exact" means a frozen sequence with a declared per-op error bound, bit-identical on CPU and GB10, not correct rounding. RSQRT, DIV and SQRT stay correctly rounded. EXP worst case 39 ulp against its 40 ulp bound (omega#107 notes) | None for the wording; GB10 parity is tracked in row 10 | - | No |
| 9 | Transcendental: coverage | Partial (CPU only) | omega#127 (merge `7a5a13c`): SIGMOID, TANH, RSQRT, EXP2, LOG2, ERF, SIN, COS, GELU. Frozen binary32 sequences, bounded ulp (declared 3, 3, 0, 2, 2, 3, 2, 2, 3), checked against a binary128 oracle over all 2^32 inputs. Not in `OP_TABLE`; no CI job runs `make test-numeric-transc`; no receipt | pow variants not defined. Exact-versus-bounded wording is an owner decision (aien-architecture PR #76, unresolved) | Owner decision, then a CI job and a receipt | S | No |
| 10 | Transcendental on GB10 | PASS on merged main for EXP2, LOG2, SIGMOID, TANH (five ops still open) | Whole-program GB10 kernels (`src/omega_numeric_divsqrt_gb10.c`), same operation order and FMA placement as `src/omega_numeric_transc.c`, no MUFU, nvdisasm-checked, digest-pinned. Merged-main receipt `evidence/E1-TRANSC-GB10/96fbc78df41fb62244e9d16cd59531182f64a01fb8c5ef1e02c8dadb8be6f556.json` (omega `4863803`, merge of #165; physics `e95e3ed`; run 2026-10-01 16:58:11Z-17:20:50Z; chip exit 0; host tier 32/0; unsigned, content-addressed, chip log `blobs/c65e0041...log`): EXP2, LOG2, SIGMOID, TANH each every 32-bit input, 0 mismatches, 0 unwritten. Earlier pre-merge receipts (omega `d25d7ad`): `6ca4ee4e...json` (EXP2, LOG2, TANH PASS) and `147b2189...json` (SIGMOID PASS). Kept FAILs: `1dc85ef2...json` (32-register QMD, Xid 13 on LOG2/SIGMOID/TANH, fixed by a 48-register allocation) and SIGMOID in `6ca4ee4e` (98,304 outputs never written in one batch, every written value matched; the SIGMOID-only rerun passed) | RSQRT, ERF, SIN, COS, GELU have no GB10 kernel (RSQRT needs exact-midpoint checks for correct rounding). EXP/LOG of `OP_TABLE` still NOT_ENCODED. Not yet in `OP_TABLE` dispatch. Intermittent unwritten-output event on the DS executor is not root-caused (did not recur in the merged-main run) | Encode the remaining five ops; root-cause the unwritten batch | M | Yes |
| 11 | CPU/GB10 parity under frozen contracts | Partial | Gate 5 receipt `OMEGA-NUMERIC-0/dafb645a9692aab551327c4de7c5dfd7d194504b1ae2ac998a0f5480c687cbb2.json` (omega 7852570, physics e95e3ed): 40 encoded ops, 47 GB10 parity lines (FFMA x8), 0 mismatches over 4096 inputs each incl. subnormal/NaN/inf classes, DIV and SQRT included; frozen contracts = FPCR rule, declared reduction order, provenance table, physics.lock pin | EXP and LOG (row 10) and the nine #127 ops are still refused on the main path and have no Gate 5 chip parity | Rerun Gate 5 with each new op added; keep the 23 tests and extend the expected-ID list | M per batch | Yes |
| 12 | Integration (not named in the plan, implied by "before tensor training") | Missing | `omega_numeric.h` is included only by the four numeric source files; program IR refuses DIV (`tests/realize/test_program_realize.c:249`); no FP32 type in `src/omega_program.c` | Nothing downstream (program IR, visor, M20 jspace from #116) uses the qualified numeric library | M20-side decision: M20 tensor ops must call these contracts, and the program IR needs an FP32 value type | M | No |

## 3. Counts

Original 2026-09-30 counts: Covered 1 (row 6), Partial 7 (rows 1, 2, 3, 4, 5, 8, 11), Missing 4 (rows 7, 9, 10, 12).

Counts at 2026-10-01: Covered 5 (rows 1, 3, 4, 6, 7), Partial 5 (rows 2, 5, 8, 9, 11), Missing 2 (rows 10, 12).

## 4. Order of work

CPU-only, can start now (no chip time):
- Row 8 decision (plan wording vs tightening EXP/LOG).
- Row 9 new transcendental CPU sequences with binary128 oracle bounds.
- CPU references for rows 3, 4, 5 (new predicates, conversions, multi-level reduction order).
- Row 12 integration decision with the M20 owner.

Needs the GB10, later, one heavy test at a time:
- Rows 7 and 10 (DIV/SQRT and transcendental kernels): the largest gaps.
- Row 5 block reduction, rows 1-4 new encoded ops, then one Gate 5 rerun per batch (row 11).

Largest three gaps:
1. Row 7: DIV/SQRT on GB10. L, GPU.
2. Row 10 with row 9: transcendental set and its GB10 kernels. L + L, CPU first then GPU.
3. Row 5: reductions beyond one warp. L, GPU.
