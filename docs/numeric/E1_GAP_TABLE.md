# E1 Numerical Closure: Gap Table

Status 2026-10-02: E1 CLOSED on the fb36109 chip campaign, receipt `evidence/E1-CLOSURE/e7851c69d34ac777a9436af16d0bdd5d69264c8528c62d562b600f5d89153061.json`; see the closure section at the end. Everything between here and there is dated history and is kept as written.

Status: research note, 2026-09-30, read-only survey of `origin/main` at `529ebfa`; rows refreshed 2026-10-01 (see the status summary below).
The 2026-10-01 rows were built and run; see the update section below.

Plan source: `aien-architecture/CURRENT_EXECUTION_PLAN.md`, section 8, "E1. Numerical closure"
(must close before M20 tensor training). Evidence source: the merged M19R Gate 5
(OMEGA-NUMERIC-0, omega#107, merge `62f5ba5`), canonical receipt
`evidence/OMEGA-NUMERIC-0/dc4e6012...f860db.json` (run on omega `8024e9a` + physics `e95e3ed`, PASS 23/23).


## Status summary, 2026-10-01 (reconciled at omega `07004a8`)

**E1 is NOT closed. Verdict: PARTIAL, 2 of 6 exit requirements met.** (This was true on 2026-10-01. Superseded 2026-10-02: see "Closure, 2026-10-02" at the end of this file.) Rows 1 to 10 below were refreshed on
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

## Update, branch `e1-gap-close` (convergence plan item 3, part 2)

Classification of every row (C3 = the unwritten-output bug, branch `c3-h1-fence`, not touched here):

| Row | Class | State |
|---|---|---|
| 1, 3, 4, 6, 7 | Already closed | No change |
| 2 | Closed in code, chip run queued (e1-gap-close-2) | General LDG/STG kernels (all widths, signed and unsigned narrow loads, byte strides, signed 24-bit offsets) built from nvdisasm-read encodings; host model equals a byte oracle; 148 table kernels decode with nvdisasm; chip job `E1B-LDST-CHIP` queued |
| 5 | Closed in code, chip receipt pending | MEAN divides on the GB10 DIV kernel, mutant hook in the build |
| 8 | Already closed (decided) | No change |
| 9 | Closed | CI runs the check; full-domain CPU run PASS |
| 10 | Closed in code for all five ops (e1-gap-close, e1-gap-close-2), chip receipts pending | SIN, COS, ERF, GELU (e1-gap-close) and RSQRT (e1-gap-close-2, exact-rounding by FMA error-free transforms, no integer multiply) equal the CPU tier on the host model; chip jobs `E1-CLOSE-TRANSC-CHIP` and `E1B-RSQRT-CHIP` queued |
| 11 | Open, needs the receipts from rows 5 and 10 and a Gate 5 expected-ID extension | Gate 5 rerun queued (`E1-CLOSE-GATE5`) on commit `9d11565`, the first commit of this PR, not on the PR head (`b48d132`); the rerun on the head is still to be queued |
| 12 | Closed in code (e1-gap-close-2) | FP32 is an explicit program IR type (tag `0x0E`) with an explicit `CONVERT` op; the type check refuses mixed types; evaluation uses the library tier; `make test-program-fp32` PASS 33/33; `spec/program-fp32.md`. Amendment to the ratified type table, no existing id changes |
| Unwritten-output root cause (row 10 note) | Blocked on C3 | Not touched |

Chip evidence for rows 5, 10 and 11 comes from the shared forge queue (`E1-CLOSE-REDUCE`,
`E1-CLOSE-TRANSC-CHIP`, `E1-CLOSE-GATE5`); the receipts are added to this table when they land.

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
| 5 | Reductions | Closed in code (MEAN divides on the GB10); chip receipt pending | omega#134 (merge `c54d492`): SUM, MAX, MIN, MEAN over any length n >= 0 in frozen order `RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL`. GB10 SUM receipt `evidence/E1-REDUCE/be9d61ce...json` (omega 6da80bf); MAX/MIN via a reduce-owned SHFL.DOWN+FMNMX patch (omega#157): receipt `evidence/E1-REDUCE/a04f4f7f...json` (SUM/MAX/MIN/MEAN 380 cases, 0 mismatches, omega 689ad4d). This PR (`e1-gap-close`, commit `9d11565`): the final MEAN division now runs on the GB10 DIV kernel (`omega_ds_gb10_run(OMEGA_DS_DIV, ...)`), no host step; build flag `-DOMEGA_REDUCE_MUTATE_MEAN_DIV` divides by n+1 so the chip run must FAIL; `make test-numeric-reduce-cpu` PASS_EXCEPT_DECLARED_CHIP_ONLY and `make test-numeric-reduce-nvdisasm` PASS | Chip run of the new MEAN path and its mutant on a clean commit (queued: forge job `E1-CLOSE-REDUCE`); optional single-launch CTA kernel not built | Read the receipt when the forge job finishes | S | Yes |
| 6 | Division and square root (definition) | Covered on CPU | Integer-only correctly rounded sequences, bit-exact vs IEEE, hard cases (`tests/test_omega_numeric.c:756-801`); header records full SQRT sweep and 10^9 DIV pairs (`src/omega_numeric.h:112-123`) | Full DIV sweep is sampled, not exhaustive (not feasible for two inputs; acceptable) | None for definition | - | No |
| 7 | Division and square root on GB10 | Closed (chip PASS) | Standalone GB10 kernels `src/omega_numeric_divsqrt_gb10.c` (DIV 264 words, SQRT 336 words, every word from Omega's encoder and checked by nvdisasm); `make test-divsqrt-host`; gate `tools/run_divsqrt_gate.sh`; receipt `E1-DIVSQRT/de7b2dd6a930ae94529773cf4e31aa08aac7065602b1d771e13e6ed2784a9074.json` ; wired into the main GB10 path (`omega_numeric_gb10.c` dispatches DIV/SQRT to `omega_ds_gb10_run`); Gate 5 chip parity 0 of 4096 mismatches each, receipt `OMEGA-NUMERIC-0/dafb645a9692aab551327c4de7c5dfd7d194504b1ae2ac998a0f5480c687cbb2.json` | None for DIV/SQRT | None | - | No |
| 8 | Transcendental: exact sequences (wording) | Met (decided) | EXP/LOG frozen polynomial sequences (`omega_numeric.h:125-131`); edge and round-trip checks (`test_omega_numeric.c:803-844`); binary128 oracle bound EXP 40 ulp, LOG 4 ulp (lines 379-380) | Decided by aien-architecture #76 (merge `15808f9`, Option 1): "exact" means a frozen sequence with a declared per-op error bound, bit-identical on CPU and GB10, not correct rounding. RSQRT, DIV and SQRT stay correctly rounded. EXP worst case 39 ulp against its 40 ulp bound (omega#107 notes) | None for the wording; GB10 parity is tracked in row 10 | - | No |
| 9 | Transcendental: coverage | Closed on CPU | omega#127 (merge `7a5a13c`): SIGMOID, TANH, RSQRT, EXP2, LOG2, ERF, SIN, COS, GELU. Frozen binary32 sequences, bounded ulp (declared 3, 3, 0, 2, 2, 3, 2, 2, 3), checked against a binary128 oracle over all 2^32 inputs. CI: `.github/workflows/pr-smoke.yml` runs `test-numeric-transc` (sampled bound check and 10 mutants, must print `all 10 mutants killed`) and `host-suites-3.yml` runs it with `test-numeric-transc-digest`. Full-domain run `test-numeric-transc-full` PASS on omega `9d11565` (forge job `E1-CLOSE-TRANSC-FULL`, "E1 WP-B verdict: PASS", every op's full-domain digest matches the frozen one; GELU max 2 ulp at bound 3). The erfcx tables now live in one header, `src/omega_numeric_transc_tables.h`, read by both the CPU sequence and the GB10 kernels | pow variants not defined (no plan requirement); not in `OP_TABLE` dispatch | None | - | No |
| 10 | Transcendental on GB10 | EXP2, LOG2, SIGMOID, TANH PASS on chip; SIN, COS, ERF, GELU closed in code, chip receipt pending; RSQRT open | Whole-program GB10 kernels (`src/omega_numeric_divsqrt_gb10.c`), same operation order and FMA placement as `src/omega_numeric_transc.c`, no MUFU, nvdisasm-checked, digest-pinned. Merged-main receipt `evidence/E1-TRANSC-GB10/96fbc78df41fb62244e9d16cd59531182f64a01fb8c5ef1e02c8dadb8be6f556.json` (omega `4863803`, physics `e95e3ed`): EXP2, LOG2, SIGMOID, TANH each every 32-bit input, 0 mismatches. This PR (commit `966c47c`) adds SIN (120 words), COS (120), ERF (360), GELU (424): `tools/divsqrt_nvdisasm_check.sh` VERDICT PASS for all ten kernels; `make test-numeric-transc-gb10-host` 64/0 (edge set, stride 4093, 1M random per op, digest recorded); the host model of each new kernel equals the CPU tier on every one of the 2^32 inputs (`--host-all` SIN, COS, ERF, GELU: 4294967296 checked, 0 mismatches, forge job `E1-CLOSE-HOSTALL`). Kept FAILs from earlier runs are listed in the receipts' history | Chip run of the four new ops over all 2^32 inputs (queued: forge job `E1-CLOSE-TRANSC-CHIP`). RSQRT has no GB10 kernel: the CPU sequence decides rounding with a 128-bit integer midpoint test (`mid_below`), and the DS frame has no integer multiply, so a bit-identical kernel needs a different exact-rounding scheme (FMA residual checks); not attempted here. Unwritten-output intermittent event on the DS executor is C3 part 1 (branch `c3-h1-fence`), not touched | Run the chip job; design the RSQRT exact-rounding scheme | M (RSQRT) | Yes |
| 11 | CPU/GB10 parity under frozen contracts | Partial | Gate 5 receipt `OMEGA-NUMERIC-0/dafb645a9692aab551327c4de7c5dfd7d194504b1ae2ac998a0f5480c687cbb2.json` (omega 7852570, physics e95e3ed): 40 encoded ops, 47 GB10 parity lines (FFMA x8), 0 mismatches over 4096 inputs each incl. subnormal/NaN/inf classes, DIV and SQRT included; frozen contracts = FPCR rule, declared reduction order, provenance table, physics.lock pin | EXP and LOG (row 10) and the nine #127 ops are still refused on the main path and have no Gate 5 chip parity | Rerun Gate 5 with each new op added; keep the 23 tests and extend the expected-ID list | M per batch | Yes |
| 12 | Integration (not named in the plan, implied by "before tensor training") | Partial (tensor tier closed; program IR FP32 type open) | The M20 tensor CPU tier now calls the qualified library: `src/tensor/omega_tensor_cpu.c` routes elementwise ops through `omega_numeric_cpu_realize`, EXP2/LOG2/SIGMOID/TANH/SIN/COS/RSQRT/ERF/GELU through `omega_numeric_transc.c`, and reductions through `omega_tensor_reduce_seam.c` into `omega_reduce_cpu`; `tools/tensor_mutations.sh` and `tools/autodiff_mutations.sh` mutate those call sites. Program IR still refuses DIV (`tests/realize/test_program_realize.c:249`) and `src/omega_program.c` has no FP32 type | The program IR and visor do not carry FP32 values | Design decision (FP32 value type in the program IR) belongs to the program-IR owner and Drake; not code that can be closed by a test alone. Blocked on that decision | M | No |

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

## Update, branch `e1-gap-close-2` (based on `e1-gap-close` at `b48d132`)

Row 4 of the brief (wording fix): the table line for row 11 said the Gate 5 rerun was queued "at this PR's
head". The queued job `E1-CLOSE-GATE5` is pinned to commit `9d11565`, the first commit of PR #198, not to
the head `b48d132`. The classification entry now says so, and the rerun on the head is still to be queued.

Row 10, RSQRT (`src/omega_numeric_divsqrt_gb10.c`, `body_rsqrt`, 344 words). The CPU sequence decides rounding with
a 128-bit midpoint test; the DS frame has no integer multiply. The kernel decides the same question with
FP32 FMA error-free transforms: |x| is normalized to `x'` in [1,4) (a subnormal by an exact 2^24, the parity of
the exponent folded into `x'`), a seed `y0` within one ulp of the correctly rounded 1/sqrt(x') comes from a magic
number, three FMA Newton steps and one FMA residual correction, then the sign of `mu^2 x' - 1` and `ml^2 x' - 1`
at the two midpoints `y0 +- 2^-25` is computed exactly (TwoProduct by FMA, Knuth TwoSum, Shewchuk
GROW-EXPANSION; the sign of an expansion is the sign of its highest nonzero component). The result is
`y0 + [mu^2 x' < 1] + [ml^2 x' < 1] - 1`, rescaled by an exact power of two (two exact FMULs). Evidence: `tools/divsqrt_nvdisasm_check.sh`
decodes every word (all eleven kernels), `make test-numeric-transc-gb10-host` equals `omega_math_rsqrt` on the
edge set, every 4093rd input and 10^6 random inputs, and the all-input host run (`--host-all RSQRT`, forge job
`E1B-RSQRT-HOSTALL`) covers all 2^32. Chip job `E1B-RSQRT-CHIP`
(`tools/run_numeric_transc_gate.sh RSQRT`).

Row 2, general load/store (`src/omega_numeric_ldst_gb10.{h,c}`, `docs/numeric/E1_LDST_GB10.md`). Evidence source
for the encodings: the verified vecadd words in `src/omega_blackwell_encoder.c` (the `LDG.E` and `STG.E` forms
the existing kernels use), extended only for the fields nvdisasm 13.0.85 `-b SM121` decoded when each bit
was varied one at a time: the signed 24-bit byte offset (word 1 bits 8 to 31, both LDG and STG), the access
size (word 2 bits 9 to 11: U8, S8, U16, S16, 32, 64, 128 for loads; 8, 16, 32, 64, 128 for stores) and the
stride immediate of `IMAD.WIDE.U32`. `make test-ldst-nvdisasm`: 148 of 148 table kernels decode word for word
to the encoder's text (a flipped offset bit changes the decode, so the check is not vacuous);
`make test-ldst-host`: 33 PASS (host model equals a typed-C byte oracle on every spec, 13 refusal rules,
10 kernel mutations caught). Chip job `E1B-LDST-CHIP` (`tools/run_numeric_ldst_chip.sh`).

Row 12, FP32 in the program IR: see `spec/program-fp32.md`. No program-IR owner document forbids it
(`spec/type-system.md` is ratified for M4, so tag `0x0E` is an amendment, not a conflict).

Chip failure classification for these jobs: a FAIL whose log shows bytes still equal to the fill pattern or
block-aligned unwritten spans is the unwritten-output bug C3 (branch `c3-h1-fence`), not a defect of these kernels.

C3 hardening (branch `e1-gap-close`): the reduce launcher (`run_chunk`) and the ldst launcher (`omega_ldst_gb10_run`) had the same
pre-fix pattern as `omega_ds_gb10_run` and now take its L2 flush plus second release marker, waited on before every readback (step
`marker2_wait`). A/B script: `~/workspace/scripts/lt-e1-c3-ab.sh` (control arm `-DOMEGA_C3_PROTECT_OFF`, compiles out only the new
reduce and ldst protection). Round 1 result: bug not reproduced / comparison inconclusive (round 1 complete, rounds 2-5 not started); record `docs/numeric/E1_C3_REDUCE_LDST_AB_ROUND1.md`. No claim is made that this fixes the observed rc=-4.

## Closure, 2026-10-02 (campaign on omega `fb36109`, merged as `40d1ea37` by #225)

**E1 numerical closure receipt: `evidence/E1-CLOSURE/e7851c69d34ac777a9436af16d0bdd5d69264c8528c62d562b600f5d89153061.json`** (schema
`AIEN_E1_CLOSURE_V1`, written by `tools/e1_combine.sh`, self-test `make test-e1-combine`). The receipt binds, and
refuses to exist unless every one of these holds; the chip ran on candidate `fb36109d39bdf0ad55d5b683643c9a4a7b19b0fc`
with Physics `e95e3ed2a86fe4bffe4d954fa94c27dfb5284280` (the `physics.lock` pin), both trees clean. The receipt says
`chip_ran_on: candidate`; it never says the chip ran on main.

| Constituent | Evidence | Result |
|---|---|---|
| Gate 5 (OMEGA-NUMERIC-0) | `evidence/OMEGA-NUMERIC-0/3b8f599518bd5f686a4b7b1ae97369f1f3930cc5734b76ac507d262c2a7a927f.json` | PASS 26/26 tests, 47 GB10 parity lines (FADD..FFMA_V, 13 FSETP_*_SEL, conversions, DIV, SQRT, REDUCE_SUM), 0 mismatches, hardware descriptor `91684948...` |
| Reductions on GB10 | `evidence/E1-REDUCE/3faff6c5e763223d9b1388b2e9120000145b205e5d424d5995b54b50aa3c95b0.json` | SUM, MAX, MIN, MEAN: 380 cases, 0 mismatches; the MEAN division runs on the GB10 DIV kernel (no host step) |
| MEAN mutant | `evidence/E1-REDUCE-MUTANT/6718c615bbea67ea135679606a020c8ce9474b2eacd7dbae560e1c1a8bc438c6.log` | `-DOMEGA_REDUCE_MUTATE_MEAN_DIV` build: MEAN FAIL (95 of 95 mismatches), SUM/MAX/MIN PASS; campaign line `E1 MEAN MUTANT: KILLED` |
| Transcendentals on GB10, new ops | `evidence/E1-TRANSC-GB10/a2f1401631e5386ed0d5689960a4580be08aba0163c76a51ad9a037e72f6b3a6.json` | SIN, COS, ERF, GELU, RSQRT: every one of the 2^32 inputs each, 0 mismatches, 0 unwritten, bit-identical to the CPU tier |
| Transcendentals on GB10, earlier ops | `evidence/E1-TRANSC-GB10/96fbc78df41fb62244e9d16cd59531182f64a01fb8c5ef1e02c8dadb8be6f556.json` (omega `4863803`, #165) | EXP2, LOG2, SIGMOID, TANH: all 2^32 inputs each, 0 mismatches. Not rerun on `fb36109`; the combiner requires the four kernel digests in this receipt to equal the same four kernels listed in the `a2f14016...` receipt, so the words that ran then are the words the candidate carries |
| General load/store on GB10 | `evidence/E1-SIMT-C3/20261002T211613Z/ldst/chip.log` (sha256 `d246700b...`) | 148 of 148 table kernels PASS on chip (1000 elements each, 0 mismatches, 0 unwritten); host oracle 33/33; nvdisasm 148/148 decode |
| Campaign log | `evidence/E1-SIMT-C3/20261002T211613Z/campaign.log` (sha256 `0b3b8399...`) | five steps, `VERDICT PASS`, zero `GB10_COMPLETION_UNCERTAIN` |
| Candidate = main equivalence | `evidence/E1-SIMT-C3/e1-equivalence-fb36109-40d1ea37.json` (`tools/e1_manifest.sh`) | 71 E1 files: 70 byte-identical, 1 differs (`Makefile`, host class; the squash merge picked up VC1 Makefile rules). Every chip-class file identical. The four chip binaries rebuilt from main with the gate scripts' own compiler lines equal the binaries named in the Gate 5, reduce and transc receipts byte for byte |
| Host tier rerun on main | `evidence/E1-HOST-RERUN/e2206f44234f49dc741711f7d565c37addb65aa48833c2585e5d3f8afd9720cb.json` (`tools/e1_host_rerun.sh`) | the six host lines of the campaign (GB10 compile both arms, numeric host suites, program FP32/realize/id/visor, transcendental sampled+digest+full-domain, chip-run manifest tests, all-input host model of SIN/COS/ERF/GELU/RSQRT) PASS on `40d1ea37`, tracked tree clean before and after |

Why the host rerun: the only file that differs between the qualified candidate and main is `Makefile`. A chip rerun on
main would re-prove nothing the binaries do not already prove (they are byte-identical), but the host-class Makefile
difference is covered by evidence, not by assertion.

The six exit requirements of aien-architecture `CURRENT_EXECUTION_PLAN.md` section 8, E1, after this campaign:

| Req | Subject | State 2026-10-02 | Evidence |
|---|---|---|---|
| 1 | General load/store | Met | LDST 148 chip specs PASS (row 2) |
| 2 | Scalar FP32 ops | Met | Gate 5 `3b8f5995...` on the candidate; rebuilt-from-main binary identical |
| 3 | Reductions incl. MAX/MIN/MEAN on GB10 | Met | reduce `3faff6c5...`, MEAN divides on the chip, mutant killed (row 5) |
| 4 | Defined DIV and SQRT | Met | Gate 5 parity lines DIV and SQRT, 0 mismatches (rows 6, 7) |
| 5 | Transcendentals, declared bounds (owner decision aien-architecture #76) | Met for the nine declared ops | EXP2, LOG2, SIGMOID, TANH, SIN, COS, ERF, GELU, RSQRT bit-identical CPU/GB10 over all 2^32 inputs each. These are bounded-parity sequences (declared ulp bounds against a binary128 oracle), not correctly rounded, except RSQRT, DIV and SQRT which are correctly rounded |
| 6 | CPU/GB10 parity under frozen contracts | Met for every op that has a GB10 kernel | Gate 5 + the two transc receipts + reduce + LDST |

**Verdict 2026-10-02: E1 campaign PASS; E1 is closed on the evidence set above, with the exclusions below recorded.**
The 2026-10-01 summary ("NOT closed, 2 of 6") stands as history of that date.

Exclusions and remaining work (recorded, not hidden):

- EXP and LOG (natural base, the 40 ulp / 4 ulp bounded `OP_TABLE` sequences) have no GB10 kernel and are refused
  on the GB10 path (`src/omega_numeric_gb10.c` header). EXP2 and LOG2 are on the chip. Nothing in the plan's six
  bullets names natural-base EXP/LOG; if a consumer needs them on the chip, that is a new row.
- Gate 5's expected-ID list was not extended with the transcendental ops (row 11 as written on 2026-10-01). Their
  chip parity is established by the two `E1-TRANSC-GB10` receipts, not by Gate 5 parity lines.
- Row 12 (FP32 in the program IR, `spec/program-fp32.md`) is merged and its tests ran in the host rerun
  (`test-program-fp32`); the visor carrying FP32 values is not part of E1.
- The C3 unwritten-output event is explained and fixed on this candidate (GPU-uncached completion marker page plus an
  L2 flush and a second marker in all four launchers, #225); the campaign shows zero uncertain completions and zero
  `OMEGA_DEVERR`. Older A/B records (`docs/numeric/E1_C3_REDUCE_LDST_AB_ROUND1.md`) stay as history.
- Rows 2, 5, 10 and 11 of the "e1-gap-close" update tables above said "chip receipt pending" or "queued"; those
  receipts are the ones in the constituent table. PR #198 (`e1-gap-close`, the same code before the C3 completion
  hardening) is superseded by #225 and this receipt.

Receipt naming: `tools/e1_combine.sh` writes `evidence/E1-CLOSURE/<receipt_digest>.json`, where `receipt_digest` is the
SHA-256 of the canonical body without that field (`tools/json_canon.c`), the same rule as Gate 5 and Gate 14. The
reduce, transc, host-rerun and mutant files are named for the SHA-256 of the file itself. Digests make alteration
detectable; they are not signatures. To re-verify, run the combiner again on the same inputs with
`E1_TIMESTAMP_UTC` set to the receipt's `timestamp_utc`: it must print the same digest.
