# FP32 in the program IR (amendment, E1 gap row 12)

Status: implemented on branch e1-gap-close-2. Amends the ratified type table
(spec/type-system.md) with tag 0x0E and the operation table with opcode 0x10.
No existing tag, opcode or program id changes.

## What is added
- Type `FP32` (tag 0x0E, width 32): an IEEE binary32 value, carried as its 32-bit pattern.
- Operation `CONVERT` (opcode 0x10): `imm` is the target type tag (FP32 or u32).
- `src/omega_program_fp32.{h,c}`: build, type check, and library-tier evaluation.

## Type check (omega_program_fp32_check)
A two-state machine over the body, starting in the contract input type:
- FP32 state: ADD, SUB, MUL, DIV with a binary32 constant (`imm` = its bits, at most 32 bits),
  or CONVERT to u32.
- u32 state: only CONVERT to FP32. Any other step is refused with the reason
  "mixed types need an explicit CONVERT".
- CONVERT to the type the value already has is refused.
- The final state must equal the contract output type; contract widths must be 32.
- A stale or forged program id is refused (v2 id recomputed).

## Identity
The v2 program id already binds body root, input/output type ids and constraint ids; the type ids
differ for FP32 vs u32 so the same steps over different types never share an id. Composition
requires OutType == InType, so FP32 and u32 programs join only through a CONVERT program.

## Tier
FP32 programs are evaluated by the qualified library tier (omega_numeric_cpu_realize: FADD FSUB
FMUL DIV I2FP_U32 F2U, every one on the E1 gap-table "closed" list). The AArch64 integer
realizer refuses them on type (omega_program_realize_ex returns a reason). There is no new
machine code path.

## Evidence
`make test-program-fp32` prints `GATE OMEGA_PROGRAM_FP32_PASS PASS 33/33`.
Regression: test-program-realize, test-program-id, test-visor-core, test-numeric-cpu all pass.
