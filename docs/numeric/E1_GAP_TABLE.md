# E1 Numerical Closure: Gap Table

Status: research note, 2026-09-30. Read-only survey of `origin/main` at `529ebfa`.
No code was built or run for this note.

Plan source: `aien-architecture/CURRENT_EXECUTION_PLAN.md`, section 8, "E1. Numerical closure"
(must close before M20 tensor training). Evidence source: the merged M19R Gate 5
(OMEGA-NUMERIC-0, omega#107, merge `62f5ba5`), canonical receipt
`evidence/OMEGA-NUMERIC-0/dc4e6012...f860db.json` (run on omega `8024e9a` + physics `e95e3ed`, PASS 23/23).

## 1. What Gate 5 actually counts

The brief for this note said "about 36 checks". No artifact on `origin/main` has 36. The real counts:

| Count | What it is | Source |
|---|---|---|
| 23 | Gate 5 test IDs, all PASS on chip (9 positive host/chip, 4 chip-only, 10 negative) | receipt `test_results`; `tests/test_omega_numeric.c` `report(...)` calls, lines 551-1042 |
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
| 1 | FP32 arithmetic | Covered | FADD/FSUB/FMUL/FFMA bit-exact, 4096 inputs each, receipt `gb10_parity`; test `CPU_GB10_BIT_PARITY` | FFMA third operand is one constant per launch (`src/omega_numeric.h:152`, 8 fixed values), not per element | Add per-element FFMA (c from memory) as a new encoded op | M | Yes |
| 2 | FP32 load/store | Partial | Global STG fixed form (`src/omega_numeric.c:675` CHECK:stg_form); shared LDS_STS mirror exchange inside one 64-thread CTA (`omega_numeric.c:332`) | Only one fixed addressing pattern each; no general strided, offset or vector loads/stores | Define a load/store contract (offset, stride, alignment) with encoder checks, then chip parity | M | Yes |
| 3 | Comparison / select | Partial | FSETP.GE + FSEL (`src/omega_numeric.h:153-154`); FMNMX min/max with NaN and signed-zero rules (`omega_numeric.c:320-323`) | Only the GE predicate; no LT, LE, EQ, NE, unordered/NaN-aware predicates | Add the remaining predicates to the op table, CPU reference first, then encode | M | CPU part No, parity Yes |
| 4 | Conversion | Partial | I2FP round-to-nearest-even; F2I truncate, NaN to 0, saturating (`omega_numeric.c:324-326`) | No floor/ceil/round-to-nearest to int, no unsigned, no FP32 to FP16/BF16 and back | Add conversion ops (rounding-mode variants, narrowing formats) with CPU reference, then encode | M | CPU part No, parity Yes |
| 5 | Reductions | Partial | REDUCE_SUM, one warp (32 lanes), frozen order, bit-exact on chip (`omega_numeric.h:26`, `:137`; test `WARP_REDUCTION_ORDER_DECLARED` line 862) | No block-level or whole-array reduction; no max/min/mean reductions; no frozen order for lengths above 32 | Freeze a multi-level order (warp tree then block tree via shared memory), add max/min, CPU reference then chip | L | Yes |
| 6 | Division and square root (definition) | Covered on CPU | Integer-only correctly rounded sequences, bit-exact vs IEEE, hard cases (`tests/test_omega_numeric.c:756-801`); header records full SQRT sweep and 10^9 DIV pairs (`src/omega_numeric.h:112-123`) | Full DIV sweep is sampled, not exhaustive (not feasible for two inputs; acceptable) | None for definition | - | No |
| 7 | Division and square root on GB10 | Missing | Refused as NOT_ENCODED (`omega_numeric.c:336-339`, `590-594`); test line 580 | No GB10 kernel runs the Omega sequence; GPU has only the raw MUFU seed, which is correctly not accepted (test line 961) | Write a GB10 kernel for the integer sequence (or MUFU seed plus a refinement proven correctly rounded), then bit parity vs CPU | L | Yes |
| 8 | Transcendental: exact sequences (wording) | Partial | EXP/LOG frozen polynomial sequences (`omega_numeric.h:125-131`); edge and round-trip checks (`test_omega_numeric.c:803-844`); binary128 oracle bound EXP 40 ulp, LOG 4 ulp (lines 379-380) | Plan says "exact sequences". Code is frozen and bounded, not correctly rounded: EXP worst case 39 ulp against a 40 ulp bound (omega#107 notes); test labels them self-consistent (line 255) | Plan-authority call: either tighten EXP/LOG toward correct rounding (CPU work) or amend the plan to "frozen, bounded sequences" | S (amend) / L (tighten) | No |
| 9 | Transcendental: coverage | Missing | Only EXP and LOG exist | Training needs more: at least tanh or sigmoid, and likely erf (GELU), pow/rsqrt variants, sin/cos for position encodings | Pick the M20/M21 list, write frozen CPU sequences with binary128 oracle bounds | L | No (CPU) |
| 10 | Transcendental on GB10 | Missing | EXP/LOG refused as NOT_ENCODED (`omega_numeric.c:340-343`) | No GB10 kernel for any transcendental | Encode each frozen sequence as a GB10 kernel, bit parity vs CPU | L | Yes |
| 11 | CPU/GB10 parity under frozen contracts | Partial | 15 encoded ops, 0 mismatches over 4096 inputs incl. subnormal/NaN/inf classes; frozen contracts = FPCR rule (line 551), declared reduction order, provenance table, physics.lock pin (receipt) | Parity exists only for the 15 encoded ops. DIV, SQRT, EXP, LOG and every new op in rows 1-10 have no chip parity | Rerun Gate 5 with each new op added; keep the 23 tests and extend the expected-ID list | M per batch | Yes |
| 12 | Integration (not named in the plan, implied by "before tensor training") | Missing | `omega_numeric.h` is included only by the four numeric source files; program IR refuses DIV (`tests/realize/test_program_realize.c:249`); no FP32 type in `src/omega_program.c` | Nothing downstream (program IR, visor, M20 jspace from #116) uses the qualified numeric library | M20-side decision: M20 tensor ops must call these contracts, and the program IR needs an FP32 value type | M | No |

## 3. Counts

- Covered: 2 (rows 1, 6)
- Partial: 6 (rows 2, 3, 4, 5, 8, 11)
- Missing: 4 (rows 7, 9, 10, 12)

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
