# E1 row 7: correctly rounded FP32 DIV and SQRT on the GB10

Status: see "Result" at the end. This page covers gap-table row 7 in
`E1_GAP_TABLE.md`.

## What this closes

The GB10 (Blackwell, sm_121) now has its own FP32 divide and square root kernels.
Each one gives the same bits as the CPU semantic `omega_math_div` /
`omega_math_sqrt`, which is the IEEE 754 result: round to nearest even,
subnormal inputs and outputs kept (no flush to zero), and every NaN result
returned as the canonical quiet NaN `0x7fc00000`. The comparison on the chip
is against the AArch64 `FDIV` / `FSQRT` instructions and against
`omega_math_*`. Both must match, with zero mismatches.

## How the kernels work

Neither kernel uses the hardware's approximate reciprocal or square root
units (MUFU), and neither uses floating-point multiply-add. Each one does the
whole operation with exact integer arithmetic, the same way the CPU semantic
does, so the rounding is exact by construction:

* **Unpack.** `|x| = m * 2^e` with `m` in `[2^23, 2^24)`. For subnormal inputs,
  the leading bit comes from `I2FP.F32.S32` (exact for values below 2^24), and
  `m` is the input shifted left by `23 - msb`.
* **DIV.** Restoring long division gives 27 quotient bits
  (6 instructions per bit). The remainder is the sticky bit.
* **SQRT.** If the exponent is odd, it is first made even by shifting `m`.
  Then digit-by-digit square root over 27 bit pairs (8 to 10 instructions per
  bit). The remainder is the sticky bit.
* **Round and pack.** One shared sequence, the same as `omega_round_pack`.
  It handles the normal and subnormal result ranges, round to nearest even
  from the guard bit, the bits below it and the sticky bit, overflow to
  infinity, and underflow to zero.
* **Special operands.** These are applied last, in the priority order of
  `omega_math_div` / `omega_math_sqrt`: zero, infinity, 0/0, inf/inf, NaN, and
  negative inputs to SQRT.

Every shift amount on a path that affects the result stays inside `[0, 31]`.

## Kernel layout

| Part | Words |
|---|---|
| Prologue | the 17 vecadd words, unchanged: thread index, bounds check, addresses, `LDG a` into R2, `LDG b` into R5 |
| Body | 13 instruction forms, all `PT`-guarded, control word `0x010fde00` |
| Epilogue | vecadd `STG [R6.64], R9`, `EXIT`, `BRA` |
| Padding | NOPs up to a multiple of 8 |

About the body control word `0x010fde00`: it means stall 15, no barrier set,
and wait on scoreboard SB4. SB4 is the write barrier both vecadd `LDG`s set.
Every body instruction is fixed-latency, so stall 15 is the conservative
ptxas `-O0` pattern for this class.

The QMD allocates 32 registers. The body writes R8 through R27 and R9, and
never touches R1, R6, R7 or the uniform registers.

## Provenance (no invented encodings)

* Every body word comes from `omega_ds_encode` in
  `src/omega_numeric_divsqrt_gb10.c`.
* The form templates (opcode, fixed bits and field positions) come from two
  sources, both decoded offline by nvdisasm:
  * ptxas 13.0.88 `-O0` oracle output for each instruction class;
  * single-word probes.
* No ptxas kernel is copied.
* `tools/divsqrt_nvdisasm_check.sh` dumps both kernels and disassembles every
  word with `nvdisasm -b SM121` (version 13.0.85 on this machine). It then
  requires the text to equal, line by line, the listing the encoder produces
  for itself (`omega_ds_listing`).
* The kernel SHA-256 values recorded in `OMEGA_DS_KERNEL_SHA256` were taken
  after that check passed. The executor refuses any kernel with a different
  digest.

## Pre-submission checks

Each check in `src/omega_numeric_divsqrt_gb10.c` is one line that ends in a
`CHECK:` marker. `tools/divsqrt_check_sweep.sh` deletes each check in turn and
shows that the host tier then fails.

| Check | What it guards |
|---|---|
| `ds_op` | the op is known |
| `ds_len` | the kernel length is valid |
| `ds_prologue` | the prologue is identical to the vecadd prologue |
| `ds_epilogue` | the kernel ends with `STG`, `EXIT`, `BRA` |
| `ds_pad` | the padding is NOPs only |
| `ds_body` | the body is not empty and not oversize |
| `ds_guard` | no body instruction is predicated |
| `ds_ctrl` | every body control word is the fixed control word |
| `ds_forms` | every body word decodes and re-encodes to the same bits (a recorded form) |
| `ds_def_use` | no register is read before it is written |
| `ds_pred_def_use` | no predicate is read before it is written |
| `ds_reserved` | the body never writes R1, R6 or R7 |
| `ds_gpr` | every register fits inside the QMD allocation |
| `ds_result` | the last body instruction writes R9 |
| `ds_digest` | the kernel digest equals the recorded nvdisasm-verified digest |
| `ds_args_op`, `ds_buffers`, `ds_count` | arguments |
| `ds_qmd`, `ds_qmd_gpr`, `ds_qmd_code` | the QMD |

## Verification

| Tier | Coverage |
|---|---|
| Host (`make test-divsqrt-host`) | encoder round trips; one refusal per check; a host model of the exact body (register-level interpreter) against FDIV/FSQRT and `omega_math_*` on the hard corpus from `tests/test_omega_numeric.c`, the edge set and 200k random samples |
| Host, long | `--host-sqrt-all` runs all 2^32 SQRT inputs through the host model; `--host-div N` runs N random DIV pairs |
| Chip (`tools/run_divsqrt_gate.sh`) | SQRT: all 2^32 inputs. DIV: 7 batches of 2^24 pairs (117,440,512), with batch 0 starting with the corpus and the edge set |

The DIV edge set covers:

* every pair of 35 edge magnitudes with both signs: 0, subnormals, the minimum
  and maximum normal, values near 1, the maximum finite value, infinity and
  NaNs;
* neighbours of 2^±126.

The DIV random pairs are stratified across five shapes:

* uniform bits;
* both operands near 1;
* quotients from 2^-118 to 2^-155 (the subnormal result range);
* quotients from 2^124 to 2^130 (the overflow boundary);
* subnormal operands.

The receipt is `~/workspace/evidence-out/E1-DIVSQRT/<sha256>.json` (mode 0444). It
records:

* the omega commit and the physics commit (the `physics.lock` pin);
* whether each tree was clean before and after the run;
* the binary digest and the kernel digests;
* the counts and the verdict.

The script refuses to run on a dirty tree.

## Result
