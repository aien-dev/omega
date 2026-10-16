# OSC-4: floats, large buffers, extern C, syscalls, long loops (design)

Status: design, not yet implemented. Target: the host program of a language-model
trainer written in Omega instead of Rust. Style follows OSC-2-DESIGN.md; every claim
about existing code carries a file:line tag against commit 6c1827ba; unconfirmed
items are marked UNVERIFIED. Each slice ships as one PR with red-before-green
tests and a dual-run fuzz leg reporting `mismatches=0` (tests/compiler/test_osc_compiler.c:572-575).

## 0. Ground truth this design stands on

1. OSC today is integer only. OscScalar has VOID, BOOL, U8..U64, I8..I64, REF,
   BYTES, CELLS (src/compiler/osc_ir.h:44-54). The validator refuses
   "arith BIN needs identical integer types" (src/compiler/osc_ir.c:158) and
   "CAST needs integer types" (src/compiler/osc_ir.c:179-180).
2. Neither encoder has a floating-point instruction. The validating encoder's ops
   are OSC_A64_ADD_IMM..OSC_A64_LDRB_REG, all X forms (src/compiler/osc_a64.h:28-64);
   a grep for fmov|fadd|fmul|fcvt|scvtf|fcmp|fsub|fdiv|fsqrt|fneg|fabs|frint|fmadd|
   fpcr|float|double over src/aarch64_encoder.[ch], src/compiler/osc_a64.[ch] and
   src/compiler/osc_cg.c is empty. src/aarch64_encoder.c is the frozen legacy
   writer, untouched by OSC work (docs/osc/OSC-1-DESIGN.md:202-216).
3. There is no register allocator. osc_cg.c keeps every vreg in a stack slot
   at [sp, #8*(v+1)], the runtime pointer at [sp, #0], and uses fixed scratch
   registers x9..x13 plus x16 for BLR (src/compiler/osc_cg.c:5-11, :104-106).
   src/compiler/osc_alloc.c is the explicit memory allocator (OscAlloc, OscAllocMeter,
   docs/osc/OSC-1-DESIGN.md:257-278), not a register allocator. The brief's
   phrase "how osc_alloc.c handles a second register class" rests on a wrong
   premise; section 1.7 gives the V-register scheme in the same fixed-scratch
   style instead.
4. Arrays are tiny. OSC_MAX_ARRAY_LEN is 64 (src/compiler/osc_ir.h:37), the pool is
   `uint64_t cells[OSC_RT_SLOTS][OSC_MAX_ARRAY_LEN]` with 64 slots (src/compiler/osc_rt.h:32, :101),
   OscType.len and OscRtEvent.len are uint16_t (src/compiler/osc_ir.h:67, src/compiler/osc_rt.h:64).
   Parameter-only slices `bytes` / `cells` exist with a pointer vreg and a length
   vreg at a+1 (src/compiler/osc_parse.c:230-235; SLOAD/SSTORE src/compiler/osc_cg.c:449-470;
   `osc_ir_slice_args_ok` src/compiler/osc_ir.c:705-720). OSC-EXT-BYTES-DESIGN.md
   still says "on branch osh/osc-ext-bytes"; the code is merged on main (e55e2af9, eb8dedf7). Stale line.
5. Loops are capped at one million. `#define LOOP_BOUND_MAX 1000000`
   (src/compiler/osc_parse.c:11) is enforced in the parser for `while ... bound N`
   (src/compiler/osc_parse.c:895-922) and `for` literal ranges (:924-946). While
   lowers to a u64 counter, CMP GE, TRAP LOOP_BOUND (src/compiler/osc_lower.c:509-540).
   The interpreter also caps steps at OSC_INTERP_MAX_STEPS = 1<<32 (src/compiler/osc_interp.h:17).
6. `extern` and `syscall` are already reserved words that the lexer refuses
   (src/compiler/osc_lex.c:22-27), so turning them into keywords breaks no program.
7. Determinism doctrine: interpreter and native agree on trap code, return value
   and event log (docs/adr/OMEGA-SYSTEMS-CORE-0000.md:14-24 decisions 6, 9, 10;
   `osc_rt_same_outcome` src/compiler/osc_rt.c:313-336). IR versions are never
   reinterpreted: OSC_IR_VERSION_MAX is 5 (src/compiler/osc_ir.h:257), `unit_version`
   picks the smallest (src/compiler/osc_ir.c:612-618). Traps run 1..14 (src/compiler/osc_ir.h:151-176).
   OscRt entry points sit at fixed offsets 0..96, new ones appended (src/compiler/osc_rt.h:85-99, osc_rt.c:12-25).
8. Compiler flags: OSC_CFLAGS is `-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 ...`
   with no `-ffp-contract=off` (mk/compiler.mk:22). Other makefiles already use
   `-ffp-contract=off -fno-fast-math` (mk/tensor.mk:24, mk/omega-gpu-engine.mk:76).
   Compiler on this box: gcc 13.3.0.
9. Customer surface in this repo: `int omega_gpu_matmul_f32(uint32_t m, uint32_t k,
   uint32_t n, ...)` (src/omega_gpu_matmul_api.h:109) and `int omega_gpu_rmsnorm_f32(uint32_t rows,
   uint32_t dim, const float *x, const float *weight, float eps, float *out, OmegaGpuEwInfo *info)`
   (src/omega_gpu_elementwise_api.h:121), archived in libomega_gpu.a (mk/prime_race_gb10.mk:2-3,25).
   The Rust trainer declares the same two externs (aien-lm-rs/crates/aien-lm-sys/src/lib.rs:26-55).
   UNVERIFIED: no `tk_*` kernel exists in this repo or in aien-lm-rs; names and
   signatures must come from aien-dev-ba before Slice C tests are written.

## 1. Slice A: f32 and f64 scalars and fixed arrays

### 1.1 Subset statement
1. Two new scalar types `f32` and `f64`: IEEE-754 binary32 and binary64.
2. Float literals: `1.0`, `0.5e-3`, `3.0f32`, `1.0f64`. A literal with no suffix
   takes its type from the context exactly like integer literals; an unresolvable
   width is refused with the existing AMBIGUOUS_WIDTH diagnostic (src/compiler/osc_check.c:1192, :1326, :1350).
3. Operators on floats: `+ - * /`, unary `-`, comparisons `== != < <= > >=`.
   No `%`, no bit operators, no `&&`-style short circuits beyond what exists for bool.
4. Conversions with `as`: int to f32/f64, f32/f64 to int, f32 <-> f64, and bit
   casts spelled `x as u32 bits` / `x as f32 bits` (same widths only). Bit casts
   are the only way to inspect NaN payloads or the sign of zero.
5. `let`, params, returns, and `own [f32; N]` / `own [f64; N]` arrays with the
   existing N range 1..OSC_MAX_ARRAY_LEN (src/compiler/osc_ir.h:37); Slice B
   raises that range.
6. Intrinsics `sqrt(x)`, `abs(x)`, `min(a, b)`, `max(a, b)` on one float type; `min`/`max` follow
   FMINNM/FMAXNM, the IEEE minNum/maxNum rule: if exactly one operand is a quiet NaN the other is
   returned, otherwise NaNs propagate as in 1.3 item 4; -0.0 orders below +0.0. No `fma` or `pow`;
   `exp` and `log` reach the trainer as `extern fn expf/logf` (3.1), never as intrinsics.

### 1.2 Grammar delta (against docs/osc/OSC-1-DESIGN.md:42-83)
```
type     := ... | "f32" | "f64"
literal  := ... | FLOAT                   # digits "." digits [exp] [("f32"|"f64")]
expr     := ... | expr "as" type ["bits"]
         | ("sqrt"|"abs") "(" expr ")" | ("min"|"max") "(" expr "," expr ")"
```
Lexer: `.` is currently OT_DOT (src/compiler/osc_lex.c:169) and integer
literals are the only numbers; a FLOAT token is accepted only when a digit
precedes the dot, so `a.b` member syntax (none today) stays free.
`f32`, `f64`, `sqrt`, `abs`, `min`, `max`, `bits` become keywords (src/compiler/osc_lex.c:9-18).

### 1.3 Semantics (shared by interpreter and native)
1. Rounding is round-to-nearest-even for every operation. FPCR stays at the
   process default (RMode 00, FZ 0, DN 0, exceptions untrapped); OSC never
   writes FPCR. Subnormals are kept, never flushed.
2. No fused operations. There is no FMADD/FMSUB/FNMADD/FNMSUB in the encoder and
   the lowering never produces one. `a * b + c` is two IR ops and two rounds.
3. Interpreter arithmetic is plain C `float` / `double` on values held in `volatile` locals, compiled
   with `-ffp-contract=off -fno-fast-math -fno-math-errno` added to OSC_CFLAGS (mk/compiler.mk:22).
   FLT_EVAL_METHOD is 0 on AArch64 GCC, so there is no excess precision. Each op is one C expression
   per IR op: no reassociation. `sqrt`/`sqrtf` are the only libm calls: IEEE makes them correctly
   rounded, so they match FSQRT, and -fno-math-errno lets GCC inline them.
4. NaN: arithmetic on NaN yields the hardware's default-propagated NaN (first
   operand's payload, quieted). The interpreter runs on the same CPU, so the bits
   agree there; NaN payload equality across machines is outside the contract.
   Tests compare f32/f64 results by bit pattern on the same host.
5. -0.0: `==` treats -0.0 == +0.0 as true (FCMP semantics). Bit casts expose the sign.
   No trap distinguishes them.
6. Comparisons with NaN: `<`, `<=`, `>`, `>=`, `==` are false; `!=` is true. These
   are the unordered results of FCMP consumed with conditions MI/LS/GT/GE/EQ/NE
   as listed in 1.6. Interpreter uses the C operators, which match.
7. Division by zero, overflow, invalid: no traps; inf and NaN flow on. This
   differs on purpose from integer `/` (tests/compiler/neg/ovf_zero_divisor.osc):
   a trainer must be able to see a loss go inf and report it, not die.
8. Float to int `as`: FCVTZS/FCVTZU semantics (truncate, saturate, NaN -> 0); the
   interpreter saturates explicitly and never relies on C's undefined conversion.
9. Int to float: SCVTF/UCVTF (RNE). f32 <-> f64: FCVT (RNE).
10. Interpreter NEG is `-x`, never `0.0 - x` (0.0 - (+0.0) is +0.0; FNEG gives -0.0).
11. Interpreter MIN/MAX spell out the minNum/maxNum rule of 1.1 item 6 in C (NaN tests, then the
    signed-zero order, then `<`); C `fmin`/`fmax` are never called.
12. f32 literals are parsed with `strtof`, f64 with `strtod`; never strtod then cast (double rounding).
13. Item 7 is a permanent language rule, not an OSC-4 exception. The ADR docs/adr/OMEGA-SYSTEMS-CORE-0000.md
    is frozen, so the Slice A PR does not edit the text of decision 6; it appends a dated clarification
    note under decision 6 stating that inf and NaN are values, not invariant violations (reviewer
    condition 2, 2026-10-16).

### 1.4 Static refusals (new diagnostic kinds appended after SLICE_ALIAS, src/compiler/osc_diag.h:12-56)
| Kind | Transition | Cause |
|---|---|---|
| FLOAT_MIXED | check | arithmetic or compare between f32 and f64, or float and int, without `as` |
| FLOAT_BITOP | check | `% & \| ^ << >>` applied to a float |
| FLOAT_INDEX | check | float used as array index, loop bound or `bound` |
| FLOAT_BITS_WIDTH | check | `as ... bits` between sizes that differ (f32 bits only to/from u32/i32, f64 only to/from u64/i64) |
| FLOAT_CONST_RANGE | check | literal that does not round to a finite value of its type (overflow to inf is refused at compile time) |
| AMBIGUOUS_WIDTH | check | unsuffixed float literal with no context type (existing kind) |

### 1.5 IR (version 6; OSC_IR_VERSION_MAX becomes 6, src/compiler/osc_ir.h:257)
1. OscScalar gains OSC_S_F32 and OSC_S_F64 appended after CELLS (src/compiler/osc_ir.h:44-54).
   `osc_scalar_is_int` (src/compiler/osc_ir.c:26) stays false for them.
2. New OscOp values appended after SSTORE (src/compiler/osc_ir.h:74-127):
   FBIN (sub: ADD SUB MUL DIV MIN MAX), FUN (sub: NEG ABS SQRT), FCMP (sub: EQ NE LT LE GT GE,
   dst bool), FCVT (float <-> float), ITOF (sub: signed/unsigned), FTOI (sub: signed/unsigned,
   dst width from type), FBITS (bit cast). CONST carries float bits in `imm`
   (OscInsn.imm is uint64_t, src/compiler/osc_ir.h:179-190); f32 constants
   occupy the low 32 bits, upper 32 zero.
3. Validator rules beside the integer ones (src/compiler/osc_ir.c:158, :179): FBIN/FCMP
   need identical float types; FUN a float; FCVT two distinct float types; ITOF
   int -> float; FTOI float -> int; FBITS equal widths. Violations are decoder
   refusals in the style of src/compiler/osc_ir.h:268-283.
4. `unit_version` returns 6 when any float type or op is present (src/compiler/osc_ir.c:612-618).

### 1.6 Native lowering and AArch64 encodings
Vreg slots stay 8 bytes. An f64 fills the slot. An f32 occupies the low 4 bytes
and the upper 4 bytes are always zero: results are moved out with FMOV Wd, Sn
(which zero-extends) and stored with the existing STR_UOFF; loads use LDR S
from the slot. This keeps `osc_rt_call_native` (src/compiler/osc_rt.c:300) and
the `expect-run` harness untouched: f32/f64 arguments and returns travel as bit
patterns in uint64_t.

Scratch V registers: d16..d20, mirroring x9..x13 (src/compiler/osc_cg.c:7-9).
v16..v31 are caller-saved and not argument registers under AAPCS64, and
v8..v15 (callee-saved low halves) are avoided entirely. No V register is live
across a BL, so Slice C's extern calls need no V saves.

New OscA64Op entries (appended before OSC_A64_NOPS, src/compiler/osc_a64.h:28-64),
their fixed bits and the golden word for the backend test in the
`enc_expect(insn, word, name)` style (tests/compiler/test_osc_backend.c:619-626).
Field layout for every scalar FP data-processing op: Rd bits 4:0, Rn bits 9:5,
Rm bits 20:16, type bits 23:22 (00 = S, 01 = D). MEASURED (2026-10-16): the words
below are now backed by `golden/aarch64_fp_golden.md` and `golden/aarch64_fp_golden.h`
(sibling of this checkout, /home/drakestapleton/workspace/osc4/golden/): 87 forms,
1365 samples assembled by GNU as 2.42 on this box and disassembled with objdump,
re-encoded from derived field layouts with 0 mismatches, and a negative control
(one deliberately corrupted word gives exactly 1 mismatch). FCMP NZCV measured on
this CPU: less = 1000, equal = 0110, greater = 0010, unordered (NaN) = 0011, so the
condition choices in 1.6 (LT -> MI, LE -> LS, GT, GE, EQ, NE) read false on NaN as
intended. Two facts the table adds: `fmov d0, #0.0` is not encodable (zero has no
FMOV-immediate form; use `fmov dD, xzr` / `fmov sD, wzr`), and FMOV immediates take
the 8-bit imm8 code, not the float (1.0 = 0x70, 2.0 = 0x00, 0.5 = 0x60, -1.0 = 0xF0).
The builder still runs `docs-read omega` before touching the encoder (README.md:32-34
standing rules; manual-guard hook) and adds `enc_expect` goldens taken from the .h table.

| Op | Fixed bits (S / D) | Golden example | Word |
|---|---|---|---|
| FMOV_RR | 0x1E204000 / 0x1E604000 | fmov d0, d1 | 0x1E604020 |
| FMOV_XD (Xn -> Dd) | 0x9E670000 | fmov d0, x1 | 0x9E670020 |
| FMOV_DX (Dn -> Xd) | 0x9E660000 | fmov x0, d1 | 0x9E660020 |
| FMOV_WS (Wn -> Sd) | 0x1E270000 | fmov s0, w1 | 0x1E270020 |
| FMOV_SW (Sn -> Wd) | 0x1E260000 | fmov w0, s1 | 0x1E260020 |
| FADD | 0x1E202800 / 0x1E602800 | fadd d0, d1, d2 | 0x1E622820 |
| FSUB | 0x1E203800 / 0x1E603800 | fsub d0, d1, d2 | 0x1E623820 |
| FMUL | 0x1E200800 / 0x1E600800 | fmul d0, d1, d2 | 0x1E620820 |
| FDIV | 0x1E201800 / 0x1E601800 | fdiv d0, d1, d2 | 0x1E621820 |
| FMAXNM | 0x1E206800 / 0x1E606800 | fmaxnm d0, d1, d2 | 0x1E626820 |
| FMINNM | 0x1E207800 / 0x1E607800 | fminnm d0, d1, d2 | 0x1E627820 |
| FSQRT | 0x1E21C000 / 0x1E61C000 | fsqrt d0, d1 | 0x1E61C020 |
| FNEG | 0x1E214000 / 0x1E614000 | fneg d0, d1 | 0x1E614020 |
| FABS | 0x1E20C000 / 0x1E60C000 | fabs d0, d1 | 0x1E60C020 |
| FCMP | 0x1E202000 / 0x1E602000 | fcmp d0, d1 | 0x1E612000 |
| FCVT_SD (Dn -> Sd) | 0x1E624000 | fcvt s0, d1 | 0x1E624020 |
| FCVT_DS (Sn -> Dd) | 0x1E22C000 | fcvt d0, s1 | 0x1E22C020 |
| SCVTF_X | 0x9E220000 / 0x9E620000 | scvtf d0, x1 | 0x9E620020 |
| UCVTF_X | 0x9E230000 / 0x9E630000 | ucvtf d0, x1 | 0x9E630020 |
| FCVTZS_X | 0x9E380000 / 0x9E780000 | fcvtzs x0, d1 | 0x9E780020 |
| FCVTZU_X | 0x9E390000 / 0x9E790000 | fcvtzu x0, d1 | 0x9E790020 |
| LDR_S_UOFF / LDR_D_UOFF | 0xBD400000 / 0xFD400000, imm12 scaled by 4 / 8 | ldr d0, [sp, #8] | 0xFD4007E0 |
| STR_S_UOFF / STR_D_UOFF | 0xBD000000 / 0xFD000000 | str d0, [sp, #8] | 0xFD0007E0 |
| LDR_S_REG / LDR_D_REG | 0xBC607800 / 0xFC607800 (LSL #2 / #3) | ldr d0, [x1, x2, lsl #3] | 0xFC627820 |
| STR_S_REG / STR_D_REG | 0xBC207800 / 0xFC207800 | str d0, [x1, x2, lsl #3] | 0xFC227820 |

Not added: FMADD/FMSUB (forbidden), FRINTx (no rounding intrinsics), FCCMP
(every compare is one FCMP plus one CSINC), FMIN/FMAX non-NM, W-form converts
(narrow ints widen through the existing SBFM/UBFM first, same value). Each new
op gets one `enc_expect` golden, one decoder round trip and one field-flip mutant
in test_osc_backend.c; the mirror decoder (src/compiler/osc_a64.c:244) and
`osc_a64_reencode` (:235) learn the new classes.

Lowering sketch (one IR op -> fixed sequence, in src/compiler/osc_cg.c next to gen_bin :187-268 and gen_cast :299-319):
1. FBIN: LDR d16, slot(a); LDR d17, slot(b); F<op> d16, d16, d17; STR d16, slot(dst).
2. FCMP: LDR d16/d17; FCMP; CSINC x9, xzr, xzr, inv(cond) (src/compiler/osc_a64.c:316);
   STR x9. Conditions EQ, NE, LT -> MI, LE -> LS, GT, GE (unordered reads false).
3. ITOF/FTOI/FCVT/FUN: LDR, one instruction, STR. CONST float: `mat` (src/compiler/osc_cg.c:108-133)
   into x9 then STR. ALOAD/ASTORE on float arrays: unchanged (cells are uint64).

### 1.7 ABI for floats between OSC functions
Unchanged convention: params x0..x5, return x0 (docs/osc/OSC-1-DESIGN.md:202-216).
Floats travel as bit patterns in X registers (FMOV_DX before BL, slot store after).
V registers are never part of the OSC-internal ABI. Host callers pass float
bits in `const uint64_t *args` (src/compiler/osc_rt.h:148) and oscc OUT lines
print them as hex bits with a new entry kind 13 (f32) and 14 (f64) after the
existing 1..9, 11, 12 (src/compiler/oscc_main.c:62-63).
`canon` for F32 (src/compiler/osc_interp.c:23) is "low 32 bits, upper 32 zero, NaN payload untouched";
F64 keeps all 64 bits, so the argument check at osc_interp.c:221 refuses only dirty upper halves.

### 1.8 Evidence
1. Red first: one tests/compiler/neg/*.osc per kind in 1.4 with the
   `// expect: KIND object=.. line=..` header (tests/compiler/neg/ovf_zero_divisor.osc).
2. Goldens f_adam_step.osc, f_cast_sat.osc, f_nan_cmp.osc, f_neg_zero.osc whose
   `// expect-run:` VALUE is the bit pattern, checked against a 20-line C oracle.
3. Fuzz: the dual-run generator gains float ops and constants, and `gen_arg`
   (tests/compiler/test_osc_compiler.c:143) emits f32/f64 argument patterns including quiet and
   signalling NaN, both infinities, -0.0 and subnormals; `mismatches=0` over 100k programs plus
   the ASan/UBSan leg (mk/compiler.mk:23). `make test-compiler` runs 250 units; the 100k run is
   `make test-compiler-float-fuzz-full` and the oracle check is `make test-compiler-float-oracle`.
4. Backend: 25 `enc_expect` goldens, round trips, mutants. determinism.sh passes on version 6 units.

### 1.9 Limits
No fused ops, no FRINT, no transcendental intrinsics, no f16/bf16, no SIMD, no
float `%`, no float array constants. These omissions buy bit-exact agreement.

## Slice A implementation notes

Written while building Slice A (branch osc4/slice-a, 2026-10-16). Each note is a place where the
text above left a choice open, or where the code deviates from a line above; the reading chosen is
the one that keeps the interpreter and the native code bit-identical. The code is the truth.

1. **Names.** The scalar tags are `OSC_T_F32` / `OSC_T_F64` (the existing `OSC_T_` prefix,
   src/compiler/osc_ir.h), not the `OSC_S_` spelling in 1.5. Tags 13 and 14; opcodes FBIN 30 ..
   FBITS 36; sub-op enums `OscFBin` (ADD 1 .. MAX 6), `OscFUn` (NEG 1, ABS 2, SQRT 3), `OscFInt`
   (SIGNED 1, UNSIGNED 2). spec/one-layer-format.md carries the wire layout.
2. **Canonical f32 value.** `canon` for F32 reuses the width-32 unsigned path: low 32 bits kept,
   upper 32 zero, payload untouched. A dirty upper half in an F32 argument is refused exactly like a
   dirty u32.
3. **Constant folding.** The only compile-time float evaluation is a literal (strtof / strtod, per
   1.3 item 12) and its negation (sign-bit flip, which is what FNEG does). `1.0 + 2.0` is NOT folded:
   it is emitted as a run-time FBIN, so every rounding step has exactly one implementation (the
   machine's). `ceval` returns "unknown" for every float-typed node, so a contract that mentions a
   float is decided at run time, never folded.
4. **Literal typing.** An unsuffixed literal takes its type from the context (`let x: f32 = 1.0`,
   `a * 2.0`); a suffixed literal must match the context or it is TYPE_MISMATCH. A float literal
   where an integer is required, or an integer literal where a float is required (`x * 2` with
   `x: f64`), is TYPE_MISMATCH with a message saying to write the `.`. FLOAT_MIXED (1.4) fires when
   a float meets a float of the other width or an integer-typed expression; `bool` against a float
   stays TYPE_MISMATCH.
5. **`as`.** Same float to same float lowers to MOV. `as ... bits` requires exactly one float side
   and one integer side of equal width (f32 with u32/i32, f64 with u64/i64), else
   FLOAT_BITS_WIDTH; `bool` to or from a float is TYPE_MISMATCH. ITOF `sub` is the signedness of
   the source, FTOI `sub` the signedness of the destination; the validator checks both.
6. **FLOAT_BITOP** covers `% & | ^ << >>` and unary `~` on a float operand. `x / 0.0` with a
   constant zero divisor is allowed (inf is a value, 1.3 item 7); the integer constant-zero-divisor
   refusal does not apply to floats.
7. **FLOAT_INDEX** object: the array or slice name when a float indexes it (also for a float
   literal index); the word `for` for a float loop-range end and `bound` for a float `while`/arena
   bound (those two are decided in the parser, since ranges and bounds take integer literals only).
8. **min/max** (interpreter, mirrors FMINNM/FMAXNM): a signalling NaN in `a` returns quiet(`a`); a
   signalling NaN in `b` returns quiet(`b`); both quiet NaN returns `a`; exactly one quiet NaN
   returns the other operand; equal values (including +0.0 and -0.0) return the negative-signed one
   for `min` and the positive-signed one for `max`; otherwise `<`.
9. **FTOI.** Interpreter: saturate in `double` (`half = (double)(1ULL << (w-1))`), NaN -> 0, then
   canon to the destination width. Native: FCVTZS/FCVTZU to an X register (64-bit saturation, NaN
   -> 0), then for destinations narrower than 64 bits a clamp: materialise the bound in x10, CMP,
   skip over an `ORR x9, xzr, x10` with B.cond (LE for the upper signed bound, GE for the lower
   signed bound, LS for the unsigned bound). The two agree bit for bit on every pattern the fuzz
   draws (signalling/quiet NaN, both infinities, -0.0, subnormals, values past both bounds).
10. **f32 slot traffic.** Store: `FMOV w9, sN` (zero-extends) then the existing 64-bit STR, so the
    upper half of an f32 slot is always zero. Load: `LDR sN, [sp, #off]`. FBITS to i32 sign-extends
    (SBFM); to u32 or f32 it zero-extends.
11. **Fixed-form encodings.** FMOV_XD/DX/WS/SW and FCVT_SD/DS have one form each and refuse
    `imm2 != 0`; the type bit is in the opcode name. All other float ops take `imm2` 0 (S) or 1 (D)
    and refuse anything else. The 25 ops each have one golden word from
    tests/compiler/aarch64_fp_golden.h (1365 header samples re-encoded with 0 mismatches), one
    decoder round trip and field-flip mutants.
12. **Harness change (the one existing check that moved).** test_osc_compiler.c used to assert that
    IR version byte 6 is refused as unknown. Version 6 is now the float version, so that check
    asserts instead: `osc_ir_encoding_version` recognises 6, a float unit encodes as version 6, and
    version 7 is the first unknown one. test_osc_ir_decode.c adds the strict-reader side: version 6
    on a unit without floats is VERSION_MISMATCH, version 7 is UNKNOWN_VERSION, tag 15 and opcode 37
    are RANGE. No other existing test changed.
13. **Build flags.** `-ffp-contract=off -fno-fast-math -fno-math-errno` are on OSC_CFLAGS for every
    compiler-path C file and on the determinism.sh fallback build; `-lm` is linked for `sqrt`.
14. **Float literal length.** A float literal is at most OSC_LEX_NAME_MAX (63) characters, the
    identifier limit; a longer spelling is CAPACITY in the lexer (and, as a belt, in the checker),
    never truncated: a cut spelling would parse to a different value (review of PR 489 found a
    163-character 1e-161 compiling to 0.0 through a 128-byte buffer). Every other fixed-size text
    buffer on the float path only shapes a diagnostic message or object name, never a value.
15. **Evidence targets.** `make test-compiler` runs the float dual run at 250 units (smoke);
    `make test-compiler-float-fuzz-full` runs 100,000 (receipt); `make test-compiler-float-oracle`
    rebuilds every expect-run value of the f_*.osc goldens with tests/compiler/float_oracle.c and
    diffs them.

## 2. Slice B: large buffers and typed slices

### 2.1 Subset statement
1. Raising OSC_MAX_ARRAY_LEN to hundreds of millions is refused: the pool is an
   inline 64x64 uint64 array (src/compiler/osc_rt.h:101), OscType.len is uint16_t
   (src/compiler/osc_ir.h:67) and OscRtEvent.len is uint16_t (src/compiler/osc_rt.h:64).
   Instead a new buffer kind `buf` is added: a heap-backed, host-visible, typed,
   runtime-length region.
2. Types: `&[T]` and `&mut [T]` for T in {u8, u16, u32, u64, i32, i64, f32, f64}
   generalising `bytes` (u8, read) and `cells` (u64) of OSC-EXT-BYTES. The two
   old names stay as aliases (`bytes` = `&[u8]`, `cells` = `&mut [u64]`) so the
   47 existing goldens and version 5 units stay valid.
3. Slices stay parameter-only (SLICE_ESCAPE, SLICE_VALUE of docs/osc/OSC-EXT-BYTES-DESIGN.md
   kept). Buffers are created by the host (harness, oscc, or Slice D `mmap`) and
   handed in as slices: millions of elements with no new ownership rules.
4. `len(s)` yields u64; `s[i]` reads or writes one T.

### 2.2 IR and validation
1. OscScalar gains SLICE_RO and SLICE_RW with an element scalar stored in
   OscType (the existing `elem` field, src/compiler/osc_ir.h:66, whose OscScalar gains F32 and F64 in Slice A). BYTES and
   CELLS are decoded as SLICE_RO/u8 and SLICE_RW/u64 for version 5 units.
2. SLOAD/SSTORE gain `sub` = element width (1, 2, 4, 8); `osc_ir_slice_args_ok`
   (src/compiler/osc_ir.c:705-720) also checks pointer alignment and length * width overflow.
3. Version 6 if it lands with Slice A, 7 otherwise; `unit_version` picks the smallest.

### 2.3 Bounds checks, cheap and deterministic
1. Native: index vreg in x9, length vreg (a+1) in x10, `CMP x9, x10; B.HS trap`
   via the existing `bounds_r` (src/compiler/osc_cg.c:182-185); the new element
   forms use LDR/STR register-offset with LSL by log2(width) (LDRH/LDRW forms for
   u16/u32 added to the encoder alongside the FP table: LDRH_REG 0x78606800,
   LDR_W_REG 0xB8606800, STRH_REG 0x78206800, STR_W_REG 0xB8206800, STRB_REG 0x38206800;
   UNVERIFIED until docs-read and enc_expect).
2. Interpreter: `sload`/`sstore` (src/compiler/osc_interp.c:139-147) compare the
   same unsigned index against the same length and load through a width switch.
3. The out-of-range trap stays whatever SLOAD raises today (UNVERIFIED which of
   codes 1..14, src/compiler/osc_ir.h:151-176; the builder copies it, no new code).
4. No bounds-check elimination: one compare per access, a 124M-element loop
   stays under a second on Grace.

### 2.4 Evidence
Goldens sum_u16.osc (2^20 elements), saxpy_f32.osc, fill_f64.osc. Fuzz over random
lengths 0..2^20, widths and index streams with out-of-range probes, `mismatches=0`.
One harness test maps a 512 MB `&[u16]` to prove nothing truncates at 2^16 or 2^31.

## 3. Slice C: extern C calls

### 3.1 Subset statement
```
extern fn NAME "(" [param {"," param}] ")" ["->" type] ";"
```
Params and returns limited to: integers (u8..u64, i8..i64), f32, f64, and slices
(`&[T]`, `&mut [T]`) which cross as a pointer only. Every slice parameter of an
`extern fn` must be followed immediately by an explicit `u64` length parameter
(`x: &[f32], n: u64` spells the C prototype `const float *x, uint64_t n`); the
checker passes `len(x)` is NOT implied, the Omega author writes it, and a slice
parameter with no following `u64` is the static refusal EXTERN_SLICE_LEN (reviewer
condition 3, 2026-10-16). At most 8 integer-class and 8
float-class arguments; no struct arguments; no variadics; `OmegaGpuEwInfo *info`
style out-structs cross as `&mut [u8]` of the right byte size, filled by C.
The static table (3.2 item 4) is the allowlist: an extern not in it cannot be called,
whatever the flag says.
First extern table entries (beside the two GPU entry points of 0.9): `extern fn expf(x: f32) -> f32;`
and `extern fn logf(x: f32) -> f32;`, which give train.osc its cross-entropy. Assumption: both engines
and the Rust trainer call the same glibc libm (2.39 on this box); Rust's f32::exp/ln lower to libm
expf/logf on linux-gnu. glibc expf/logf are not promised correctly rounded, so identity needs the
same libm build. Slice C ships a red check for this: a test runs `ldd` on the OSC harness binary and
on the Rust trainer binary and fails unless both resolve libm.so.6 to the same path and the two files
have the same SHA-256; the check stays red (and Slice C does not merge) until that identity is observed
(reviewer condition 3, 2026-10-16).

### 3.2 AAPCS64 lowering
1. Integer-class args in x0..x7, float-class in v0..v7, counted separately. More
   than 8 of either class is the static refusal EXTERN_ARITY (no stack args).
2. Nothing is saved before BLR: every vreg already lives in its stack slot and
   x7 (rt) is reloaded from [sp, #0] after the call (src/compiler/osc_cg.c:5-11).
   The frame is a multiple of 16 (osc_cg.c:330-341) so SP alignment holds. The target
   address is loaded into x16 from the extern table (item 4), then BLR x16 (src/compiler/osc_a64.c:333).
3. Return: x0 for ints (widened with SBFM/UBFM), v0 for floats (FMOV_DX then
   slot store). x9..x16 and v16..v31 are dead across the call by construction.
4. Target address: the unit records extern names; `osc_native_load` resolves them against a static
   table `{name, fnptr}` compiled into oscc and the test harness (no dlsym, no dlopen; both link -lm).
   Unknown name: load-time refusal EXTERN_UNRESOLVED. The resolved pointers sit in an array reached
   through a new OscRt field `externs` (`uint64_t (**externs)(void)`, offset 104) appended after
   h_store, followed by the entry `log_ext` (offset 112, 3.4), so the fixed-offset region grows by
   16 bytes, not 8, and host-side state from src/compiler/osc_rt.h:100 moves down 16 bytes (native code
   never reads it by offset). Two `_Static_assert`s beside osc_rt.c:12-25 pin offsets 104 and 112.
   `osc_rt_init` sets `externs = NULL` and `log_ext = osc_rt_log_ext`; the host that owns the table
   (oscc, the test harness) stores its pointer after init. Every runner (oscc run, test harness,
   determinism.sh's runs) refuses to start a unit that declares an extern while `rt->externs` is
   NULL: refusal EXTERN_NO_TABLE before any native code runs. Call k is
   `ldr x16, [sp, #0]; ldr x16, [x16, #104]; ldr x16, [x16, #8*k]; blr x16`, the runtime-entry shape
   of osc_cg.c:10. No address lives in the code, so oscc's code SHA-256 stays reproducible and
   determinism.sh is unaffected (reviewer condition 1, 2026-10-16).
5. Argument storage: EXTERN takes up to 16 argument vregs, more than OscInsn.args[OSC_MAX_PARAMS]
   (src/compiler/osc_ir.h:187) holds. Its `imm` indexes a per-function side table of
   `{uint16_t ext; uint8_t n; int16_t v[16];}`, so OscInsn and version 5 decoding stay unchanged.

### 3.3 Interpreter
The same registered host-function table; the interpreter dispatches on
(int count, float count, return kind) through a macro-generated `switch` over C
function pointer types in osc_interp.c (81 shapes for 0..8 by 0..8).

### 3.4 Effect doctrine
An extern call is an effect. Under decision 6 (docs/adr/OMEGA-SYSTEMS-CORE-0000.md:14-24)
the C side returns int status and never unwinds; Omega must check it. Concretely: no C function in
the static table may call `rt->trap`, `longjmp`, `exit` or anything else that leaves its frame
abnormally (rt->trap longjmps over the C frame, which is undefined behaviour); an extern that
cannot do its job returns a status word and Omega traps or branches on it. OSC-3 item 4
(effects/capabilities) is not shipped (docs/osc/OSC-3-DESIGN.md:312-318); until it
is, `extern fn` needs `oscc --allow-extern`. The flag is recorded in the unit header (a new header
byte, 0 or 1), the unit SHA-256 therefore covers it, and determinism.sh compares it like every other
header field: two compiles that differ only in the flag are different units, never "the same unit
with a different gate". When OSC-3 item 4 ships, each extern declares its effects there and the flag
is retired (reviewer condition 3, 2026-10-16).
Dual-run equivalence: both engines call the same C function with the same argument bits, then log
an EXTERN event (new kind 11 after POOL_CLOSE = 10, src/compiler/osc_rt.h:58) whose fold covers the
extern index, every argument word and the return word, appended after the 8 header bytes the way pool
events fold their generation (osc_rt.c:87-88). The fold reaches `ev_hash`, which osc_rt_same_outcome
compares (osc_rt.c:314).
How `log_ext` receives up to 16 argument words plus the return word (reviewer condition 1): the
lowering reserves 17 contiguous 8-byte words at the top of the frame (`ext area`, frame stays a
multiple of 16). Before the call it copies the n argument vregs (3.2 item 5) into ext[0..n-1] and
loads x0..x7 / v0..v7 from there; after the BLR it stores the return word (x0 or FMOV_DX v0) into
ext[16] and into the dst slot. It then calls the OscRt entry at offset 112,
`void log_ext(OscRt *rt, uint64_t ext_index, uint64_t n, const uint64_t *words)`, with
`words = &ext[0]` (4 integer arguments, no V registers live, the runtime-entry shape of osc_cg.c:10).
`osc_rt_log_ext` writes the 8 header bytes `{kind = 11, slot = (uint8_t)n, len = (uint16_t)ext_index,
serial = (uint32_t)n}`, then folds ext_index as a u64, then words[0..n-1], then words[16] (the return
word), each as 8 little-endian bytes. The interpreter calls the same `osc_rt_log_ext` with the same
words, so the two folds are byte-identical. A void extern logs return word 0.
Nondeterministic externs are the host's problem.

### 3.5 Evidence
Goldens calling a harness-defined test function (int, double, uint32 args) and
`omega_gpu_rmsnorm_f32` on a 4-row input, output bits compared with the Rust
path. Negatives for EXTERN_ARITY, EXTERN_SLICE_LEN, EXTERN_UNRESOLVED, EXTERN_NO_TABLE (unit with an extern run
against an OscRt whose `externs` is NULL), and a unit compiled without --allow-extern. The libm identity
`ldd` check of 3.1 is a red check: it must fail first against a deliberately different library path and
then pass against the real trainer binary before Slice C merges.

## 4. Slice D: syscall primitive and std-in-Omega

### 4.1 Subset statement
```
syscall "(" expr {"," expr} ")"           # 1 to 7 args: nr, a0..a5, all u64/i64, yields i64
```
`syscall` becomes a keyword (reserved today, src/compiler/osc_lex.c:25). Allowed
only in units compiled with `oscc --allow-syscall` (same gate as externs).

### 4.2 Lowering
Native: load nr into x8, a0..a5 into x0..x5 (from slots), `SVC #0` (new op OSC_A64_SVC, word
0xD4000001, UNVERIFIED pending docs-read), store x0 to dst; a failing call leaves -errno in x0.
Interpreter: `syscall(2)` from glibc with the same six arguments; glibc returns -1 and sets errno,
so the interpreter yields `r == -1 ? -errno : r` (no std/os.osc call returns -1 on success) and
both engines agree on failing calls. A SYSCALL event (kind 12) folds nr, the six argument words and
the result into `ev_hash` as EXTERN does (3.4). Determinism covers the same program, host and inputs only.

### 4.3 std-in-Omega (one file, `std/os.osc`, no C)
Numbers from /usr/include/asm-generic/unistd.h on this aarch64 box:
`__NR_openat 56` (:158), `__NR_close 57` (:160), `__NR3264_lseek 62` (:170, `__NR_lseek` :864),
`__NR_read 63` (:172), `__NR_write 64` (:174), `__NR_exit_group 94` (:260), `__NR_clock_gettime 113` (:318),
`__NR_munmap 215` (:556), `__NR3264_mmap 222` (:570) with `__NR_mmap __NR3264_mmap` (:870).
Functions: `write(fd, buf: &[u8], n) -> i64`, `openat(dirfd, path: &[u8], flags, mode) -> i64`,
`read`, `close`, `lseek(fd, off, whence) -> i64`, `mmap(addr, len, prot, flags, fd, off) -> i64`,
`munmap`, `clock_gettime(clk, out: &mut [u64]) -> i64` (length 2: tv_sec, tv_nsec), `exit_group(code)`.
The shard size is `lseek(fd, 0, SEEK_END)` (SEEK_END 2, /usr/include/unistd.h:318), never hard-coded;
fstat (`__NR3264_fstat 80`, :212) is left out because it needs the struct stat byte layout.
Paths are NUL-terminated byte arrays. Slice pointers reach `syscall` as u64 through a gated `addr(s)`
intrinsic (ADDR op). An `mmap` result becomes a slice through `slice_from(addr, len)`, legal only
under `oscc --allow-syscall`; it traps on addr 0, misalignment for the element width or len * width
overflow, the checks of `osc_ir_slice_args_ok` (src/compiler/osc_ir.c:705-720). UNVERIFIED whether
the checker accepts it without a new unsafe-block form (`unsafe` is reserved, src/compiler/osc_lex.c:25).
For acceptance 2 the harness maps the shard and passes it in as a slice.
Exit status: oscc only compiles today (exit 0, 1 or 2, src/compiler/oscc_main.c:104-163). The new
runner exits 100 + trap code (101..114) on a trap and std `exit_group` traps on codes >= 100, so a
trap is never mistaken for `exit_group(0)`.

### 4.4 Evidence
Goldens hello_write.osc (returns 6), clock_monotonic.osc, mmap_anon_u16.osc
(map 200 MB, fill, sum, unmap), exit_code.osc run as a child process, trap_exit.osc expecting
exit 100 + k. Negatives: SYSCALL_GATED, `slice_from` without --allow-syscall.

## 5. Slice E: long loops and runtime-length for

1. `LOOP_BOUND_MAX` rises from 1000000 to 1<<40 (src/compiler/osc_parse.c:11,
   :910-922, :936-944). The u64 counter lowering (src/compiler/osc_lower.c:509-540) needs
   no change: `kconst lim` already takes a 64-bit immediate through `mat`.
2. `for i in 0..len(s)` with a runtime u64 end: lowering emits the same counter
   with a CMP against the slice length vreg instead of a literal; the parser's
   literal-only rule (src/compiler/osc_parse.c:924-946) is relaxed to allow `len(IDENT)`.
   The checker still proves: the induction variable is not assigned in the body,
   the bound is a u64 known not to change (slice lengths are immutable), and the
   trip count is at most the slice length, which is at most 2^40 by the host check.
3. OSC_INTERP_MAX_STEPS (1<<32, src/compiler/osc_interp.h:17) becomes a run-time
   parameter: 1<<32 for fuzz, 1<<44 for trainer runs. Native has no step cap; the
   dual-run harness compares only programs that finish under the interpreter cap, as today.
4. Evidence: loop_2p33.osc (2^33 iterations natively, interpreter skipped above the
   cap and counted like `traces_skipped_overflow`, tests/compiler/test_osc_compiler.c:492),
   for_len.osc over a 2^20 slice, negative LOOP_BOUND_RANGE for 1<<41.

## 6. First Omega-written trainer piece and acceptance

1. A+B acceptance: `adamw_step(pmv: &mut [f32], g: &[f32], hp: &[u64])` over 124_000_000
   elements, in Omega. A slice takes two parameter registers (src/compiler/osc_parse.c:1070-1075),
   so three slices use exactly the six of OSC_MAX_PARAMS (src/compiler/osc_ir.h:33). `pmv` holds p, m
   and v back to back (3N elements, N = len(g)); hp[0] to hp[4] hold lr, b1, b2, eps, wd as f32 bit
   patterns in the low 32 bits (read with `(hp[i] as u32) as f32 bits`) and hp[5] holds the step.
   Oracle: a C file compiled with `-O0 -ffp-contract=off` doing the same operation order
   (m = b1*m + (1-b1)*g; v = b2*v + (1-b2)*g*g; bias corrections 1 - b1^step and 1 - b2^step in
   f32 by `step` repeated multiplies, no pow; p = p - lr*(mhat/(sqrt(vhat)+eps) + wd*p)).
   Pass = all 124M output words identical by bit pattern, native and interpreter
   (interpreter on N = 1M for time). UNVERIFIED: aien-lm-rs has no
   AdamW and src/train/tg_sgd.h:1-3 defers Adam/AdamW, so the C oracle is the reference.
2. C+D+E acceptance: a host trainer `train.osc` that takes a uint16 LE token shard (200 MB, size
   from `lseek`, mapped by the harness for this run, 4.3), builds batches, calls `omega_gpu_matmul_f32`
   and `omega_gpu_rmsnorm_f32` through `extern fn`, computes cross-entropy with `extern fn expf/logf`
   (3.1) and the LR schedule in f32/f64, runs adamw_step, writes a log line per step with `write`,
   exits 0. Pass = same loss bits per step as the Rust trainer for 20 steps on the same shard, same
   exit code. `omega_gpu_matmul_f32` rounds f32 inputs to bf16 on the host, round-to-nearest-even
   (src/omega_gpu_matmul_api.h:107-108), so both trainers must feed identical f32 inputs.

## 7. Velocity and estimate

From `git log --format='%h %ad %s' --date=short -- src/compiler docs/osc`:

| Date | Commits | What shipped |
|---|---|---|
| 2026-09-29 | a927bd6c | OSC-0/0B freeze (#76) |
| 2026-10-01 | b81f8500 | OSC-1 whole subset (#144), lane started 2026-09-29/30 |
| 2026-10-01 | 0abdb08a 845dce4d c7736224 7e713e30 | OSC-2 items 1-4 (#148-#151), 09:13Z to 11:29Z |
| 2026-10-01 | 38a6dea0 104541a7 334117d1 103ec7d4 | OSC-3 items 0-3 (#156 #166 #175), ext-bytes design |
| 2026-10-02/04 | dce821cc 3c82389f cb7cc216 | VC1 stages 4, 6 (#223 #224), compiler crumb (#265) |
| 2026-10-07/08 | e55e2af9 eb8dedf7 6eaac642 bcb9751a f7b43890 | ext-bytes impl and review fixes, oscc OUT |
| 2026-10-12/13 | e0ff7201 fb13b3bb 18531691 5901baa5 | docs, explicit allocation (#435) |
| 2026-10-14 | 1edf41a5 | strict IR decoder (#444/#449) |

Reading: an OSC-2 or OSC-3 sized item ships in 1 to 3 hours of builder time; a
whole subset (OSC-1) took about 2 calendar days; ext-bytes took 6 days from
design to merge, mostly waiting. A review round (eb8dedf7) adds about half a day.

Estimate, one builder agent per slice, in builder-days (working time including review rounds):
1. Slice A floats: 2.5 days (encoder table with docs-read 0.5, IR+validator+interp 0.5,
   lowering 0.5, lexer/parser/checker 0.5, fuzz and goldens 0.5). Main risk: encoder
   word mistakes, mitigated by the objdump cross-check.
2. Slice B buffers: 1.5 days, in parallel with A (f32/f64 element widths added when A merges).
3. Slice C extern: 1.5 days after A (needs the FMOV ops and v0..v7 moves).
4. Slice D syscall and std: 1 day after C's gating flag; parser work can start earlier.
5. Slice E loops: 0.5 day, any time after B.
6. Acceptance 1 (AdamW 124M): 1 day after A+B. Acceptance 2 (trainer): 2 days after
   C+D+E, dominated by the Rust comparison and GPU time under the quiet lock.
Critical path A (2.5) -> C (1.5) -> D (1) -> trainer acceptance (2) = 7 builder-days; with B, E
and AdamW in parallel, all of OSC-4 is 7 to 9 builder-days. The git log shows 3 to 4 day gaps
between pushes (10-04 to 10-07, 10-08 to 10-12), so plan 10 to 14 calendar days.

## 8. Open questions

For aien-dev-ba (GPU engine and compiler dispatcher):
1. Exact names and C prototypes of the `tk_*` kernels the trainer must call (none in this repo).
2. Is `omega_gpu_matmul_f32`'s host-side bf16 rounding stable across calls so a bit-exact oracle is possible?
3. Which `OmegaGpuEwInfo` fields does the trainer read, so its byte size can be fixed in Omega?
4. Who allocates the parameter buffers: Omega via mmap (Slice D) or the GPU engine returning host pointers?

For the advisor:
1. Accept "floats as bit patterns in X registers" for the OSC-internal ABI (no V-register ABI until a real need)?
2. Accept no float traps (inf/NaN flow) as a permanent language rule or only for OSC-4?
   SETTLED 2026-10-16: permanent (1.3 items 7 and 13; the dated note under decision 6 of
   docs/adr/OMEGA-SYSTEMS-CORE-0000.md). Not open.
3. Gate `extern` and `syscall` behind oscc flags now, or wait for OSC-3 item 4 capabilities?
4. Is a host-specific code SHA-256 for units with externs acceptable, given the IR SHA-256 stays reproducible?
5. Should Slice B's `&[T]` replace `bytes`/`cells` names outright instead of aliasing them?

## 9. Plain-language summary for Drake

1. Our own compiler can only count with whole numbers today. Training needs decimals, so the first job is teaching it decimal math.
2. We do decimal math the plain way, one step at a time, never the "fast combined" way, so the slow checker and the fast machine code always get the same answer bit for bit.
3. The compiler's arrays hold at most 64 numbers today. The trainer needs hundreds of millions, so we add a way to hand it big memory blocks and check every access.
4. We let Omega programs call the existing GPU routines written in C, using the standard calling rules every program on this chip follows.
5. We let Omega talk to Linux directly for files, memory and the clock, with the nine call numbers copied from the system's own header.
6. Loops can run a trillion times instead of a million, and the checker still proves they end.
7. Each piece ships as its own small change with failing tests written first and a random-program cross-check that must show zero disagreements.
8. The first real proof is an Omega routine updating 124 million weights exactly like a reference C program, bit for bit.
9. The second proof is a whole training loop written in Omega matching the Rust one for 20 steps.
10. Honest estimate from past speed: 7 to 9 days of builder work, which is 10 to 14 days on the calendar once waiting for reviews is counted; 2.5 working days for the decimal-math piece alone.

## Changes folded in (2026-10-16)

1. Citations: 0.4 and 1.1 item 5 now osc_ir.h:37; 0.5 and 5 item 3 now osc_interp.h:17; 6.2 drops UNVERIFIED, cites :107-108.
2. 1.1 item 6 states the minNum/maxNum rule; 1.3 item 11 has the interpreter spell it out, no fmin/fmax.
3. 1.3 item 3 (-fno-math-errno, sqrt inlined), item 10 (NEG is `-x`), item 12 (strtof/strtod).
4. 1.7 defines canon() for F32; 1.8 item 3 extends gen_arg with NaN, inf, -0.0 and subnormals.
5. 3.2 items 2 and 4: extern table via OscRt `externs` at offset 104; no address in code.
6. 3.2 item 5: EXTERN side table of up to 16 argument vregs; OscInsn unchanged.
7. 3.4 (EXTERN event, kind 11) and 4.2 (SYSCALL event, kind 12) fold argument words into ev_hash.
8. 4.2: interpreter returns `-errno` when glibc syscall() returns -1.
9. 4.3: lseek 62 added (shard size via SEEK_END); fstat 80 verified but left out.
10. 6.1: `adamw_step(pmv: &mut [f32], g: &[f32], hp: &[u64])`, three slices = six registers.
11. 3.1 adds `extern fn expf/logf` with the glibc-shared assumption; 6.2 uses them for cross-entropy.
12. 4.3: slice_from gated by --allow-syscall with osc_ir_slice_args_ok checks; clock_gettime takes `&mut [u64]` of length 2; 4.4 negative.
13. 1.3 item 13 (doctrine amendment); 4.3 exit status 100 + trap code; 4.4 trap_exit.osc.
14. 7 and 9 item 10: 7 to 9 builder-days, 10 to 14 calendar days.

## Advisor review (Fable 5.1, 2026-10-16)

Read-only review against commit 6c1827ba. No builds were run.

### Claims checked against the source (12)

1. Fixed scratch registers, no register allocator: confirmed. osc_cg.c:5-11 (header comment: slots at [sp,#8*(v+1)], operands in x9..x13, runtime via x16 BLR), osc_cg.c:104-106 (slot/ld/st helpers). Note OSC-1-DESIGN.md:208 says x9..x15; the code comment says x9..x13. Harmless, but pick one.
2. OSC_MAX_ARRAY_LEN is 64: confirmed, but it lives at osc_ir.h:37, not :41 (:41 is OSC_MAX_FIELDS). Fix the three citations in sections 0.4, 1.1 and 1.5.
3. OSC_IR_VERSION_MAX is 5: confirmed, osc_ir.h:257. unit_version picks the smallest: confirmed, osc_ir.c:612-618. Traps 1..14: confirmed, osc_ir.h:151-176. OscInsn.imm is uint64_t: confirmed, osc_ir.h:188.
4. Cells pool and runtime table: confirmed. OSC_RT_SLOTS 64 at osc_rt.h:32, `cells[OSC_RT_SLOTS][OSC_MAX_ARRAY_LEN]` at :101, OscRtEvent.len uint16_t at :64, entry points at offsets 0..96 at :85-99, osc_rt_call_native at :148 and osc_rt.c:300, osc_rt_same_outcome at osc_rt.c:313-336.
5. Interpreter arithmetic is integer only, no libm: confirmed. do_bin at osc_interp.c:49-65 uses i128 and canon(); no math.h is included anywhere in src/compiler. sload/sstore at :139-147 as cited.
6. OSC_INTERP_MAX_STEPS is 1<<32: confirmed, but at osc_interp.h:17 (the file has 32 lines), not :402. Fix in sections 0.5 and 5.3.
7. Ext-bytes code merged on main: confirmed. e55e2af9 and eb8dedf7 are both ancestors of HEAD and on main; OSC-EXT-BYTES-DESIGN.md:3 still says "on branch osh/osc-ext-bytes" (stale, as the design says).
8. Build flags and harness: confirmed. OSC_CFLAGS at mk/compiler.mk:22 has no -ffp-contract=off; OSC_ASAN at :23; fuzz gates grep `mismatches=0` at :157-160; determinism.sh at :154. Dual-run compare at tests/compiler/test_osc_compiler.c:572-575 (osc_rt_same_outcome plus return value). mk/tensor.mk:24 and mk/omega-gpu-engine.mk:76 carry -ffp-contract=off as cited.
9. Syscall numbers: all nine confirmed at the quoted lines of /usr/include/asm-generic/unistd.h (openat 56 :158, close 57 :160, read 63 :172, write 64 :174, exit_group 94 :260, clock_gettime 113 :318, munmap 215 :556, __NR3264_mmap 222 :570, mmap alias :870); the aarch64 header includes asm-generic at /usr/include/aarch64-linux-gnu/asm/unistd.h:25. gcc 13.3.0 and FLT_EVAL_METHOD 0 confirmed on this box.
10. AAPCS64 claims: correct. x0..x7 and v0..v7 are argument registers, v8..v15 have callee-saved low halves, v16..v31 are caller-saved, x16 is a scratch IP register, x8 is the Linux syscall number register. Caveat: x7 is an argument register, so an extern with 8 integer-class args overwrites rt; the design's reload from [sp,#0] covers it.
11. Velocity table: every hash and date confirmed by `git log --date=short` (a927bd6c 09-29; b81f8500 and the OSC-2/OSC-3 items 10-01; dce821cc/3c82389f 10-02; cb7cc216 10-04; e55e2af9..f7b43890 10-07/08; e0ff7201..5901baa5 10-12/13; 1edf41a5 10-14).
12. bf16 host rounding: confirmed, src/omega_gpu_matmul_api.h:107-108 ("rounded to bf16 on the host first (round-to-nearest-even)"). The design's UNVERIFIED tag in 6.2 can be dropped. The Rust externs match (aien-lm-rs crates/aien-lm-sys/src/lib.rs:27-55). tg_sgd.h:1-3 defers Adam, as stated.

Encoding cross-check against golden/aarch64_fp_golden: fmov d0,d1 = 0x1E604020, fmov d0,x1 = 0x9E670020, fmov x0,d1 = 0x9E660020, fadd d0,d1,d2 = 0x1E622820 (base 0x1e602800 at md:213), fcmp d0,d1 = 0x1E612000, fsqrt d0,d1 = 0x1E61C020, fcvt s0,d1 = 0x1E624020, scvtf d0,x1 = 0x9E620020, fcvtzs x0,d1 = 0x9E780020, ldr d0,[sp,#8] = 0xFD4007E0 (base 0xfd400000 at md:1149), ldr d0,[x1,x2,lsl #3] = 0xFC627820. All eleven match the design. Every base word in the 1.6 table that I looked up (FSUB, FMUL, FDIV, FMAXNM, FMINNM, FNEG, FABS, FCVT_DS, UCVTF, FCVTZU, FMOV_WS, FMOV_SW, STR_S/D_UOFF, LDR/STR_S_REG) also matches. No encoding mismatch found.

### Judgement

1. Floats as bit patterns in X registers: sound and the right call. The slot model, osc_rt_call_native and the harness stay untouched, and identity holds because both engines see the same 64-bit words. Two conditions: canon() for F32 must be "low 32 bits, upper zero, payload untouched" (the interpreter refuses non-canonical args at osc_interp.c:221), and gen_arg (test_osc_compiler.c:143) must emit float patterns including NaN, inf, -0.0 and subnormals.
2. No float traps: consistent with decision 6. Inf and NaN are values, not invariant violations, and the result is deterministic on one host. Make it a permanent language rule, recorded as a one-line amendment to the doctrine, not an OSC-4 exception.
3. -ffp-contract=off and FLT_EVAL_METHOD 0 are necessary but not sufficient. Four things must be stated or the interpreter will drift from native: (a) FMINNM/FMAXNM return the non-NaN operand when one side is a quiet NaN; the design's "NaN-propagating" is wrong, and C fmin/fmax must not be used; write the minNum rule out explicitly in C. (b) NEG must be `-x`, never `0.0 - x` (0.0 - (+0.0) is +0.0, FNEG gives -0.0). (c) sqrt is the only libm call; it is correctly rounded by IEEE so it matches FSQRT, but add -fno-math-errno so GCC inlines it. (d) f32 literals must be parsed with strtof, never strtod then cast (double rounding). Autovectorization is not a hazard here: AdvSIMD and scalar FP on AArch64 share FPCR and NaN rules, and the interpreter does one op per IR instruction anyway.
4. Extern design: safe (static table, no dlsym, gated, load-time refusal). An extern is an effect under decision 6; logging an EXTERN event with the argument bits folded into ev_hash makes osc_rt_same_outcome cover it. Materialising the absolute address in code is the weak point: it makes the code SHA host-specific and breaks determinism.sh. Put the extern table pointer in OscRt as the next appended entry (offset 104) and call through `ldr x16,[x7slot]; ldr x16,[x16,#104]; ldr x16,[x16,#8*k]; blr x16`, the same shape osc_cg.c:10 already uses for runtime entries. Code SHA stays reproducible and no address lives in the code.
5. Syscall slice: minimal and correctly numbered, with two gaps. glibc syscall(2) returns -1 and sets errno while the raw SVC returns -errno in x0; the interpreter must return `r == -1 ? -errno : r` or the two engines disagree on every failing call. And there is no way to learn the shard size: add lseek (62) or fstat (80).
6. Ordering is right (A before C, B and E parallel). The estimate holds as builder-days; the git log shows 3 to 4 day gaps between pushes, so plan 10 to 14 calendar days and keep 7 to 9 as the builder figure.
7. Missing items that would sink acceptance: (a) adamw_step as written needs 14 argument registers (4 slices = 8, plus 6 scalars); OSC_MAX_PARAMS is 6 (osc_ir.h:33), so it cannot be compiled; pack the five hyperparameters and step into one `&[f32]`/`&[u64]` pair or widen the ABI first. (b) EXTERN with up to 16 arguments does not fit OscInsn.args[OSC_MAX_PARAMS]; it needs its own argument storage. (c) Loss needs exp and log; without them train.osc cannot compute cross-entropy; use `extern fn expf/logf` from the same glibc the Rust trainer links (Rust lowers f32::exp to libm expf on linux-gnu), or take the loss from a GPU kernel. (d) `slice_from(addr, len)` is an unchecked soundness hole; gate it behind --allow-syscall, refuse addr 0 and len*width overflow (same checks as osc_ir_slice_args_ok), and let the harness map the shard for the acceptance run. (e) clock_gettime should fill a `&mut [u64]` of length 2, not bytes. (f) The oscc runner must map a trap to a distinct non-zero exit code so a trap is never mistaken for `exit_group(0)`.

### Verdict: APPROVE WITH CHANGES

1. Fix the citations: OSC_MAX_ARRAY_LEN is osc_ir.h:37; OSC_INTERP_MAX_STEPS is osc_interp.h:17; drop the UNVERIFIED tag on bf16 rounding (matmul_api.h:107-108).
2. Correct 1.1.6 and 1.3: FMINNM/FMAXNM return the non-NaN side; the interpreter implements minNum/maxNum explicitly in C, never via fmin/fmax.
3. State in 1.3 that interpreter NEG is `-x`, f32 literals use strtof, f64 use strtod, and -fno-math-errno joins -ffp-contract=off -fno-fast-math in OSC_CFLAGS.
4. Define canon() for F32 (low 32 bits, payload preserved) and extend gen_arg with NaN, inf, -0.0 and subnormal patterns.
5. Replace the materialised extern address with an extern table reached through an appended OscRt entry at offset 104, so the code SHA stays reproducible.
6. Give the EXTERN instruction its own argument storage (up to 16) instead of OscInsn.args[OSC_MAX_PARAMS].
7. Fold extern and syscall argument bits into the event hash so osc_rt_same_outcome catches a divergence.
8. In 4.2, have the interpreter return `-errno` when glibc syscall() returns -1, matching the raw SVC result.
9. Add lseek (62) or fstat (80) to std/os.osc so the shard size is not hard-coded.
10. Rewrite the adamw_step acceptance signature to fit six parameter registers (hyperparameters in one slice) or make widening the ABI an explicit first step.
11. Add `extern fn expf/logf` (or a GPU-side loss) to Slice C so train.osc can compute cross-entropy, and state the libm-identity assumption against the Rust build.
12. Gate `slice_from` behind --allow-syscall with the osc_ir_slice_args_ok checks, and use `&mut [u64]` of length 2 for clock_gettime.
13. Make "no float traps" a permanent rule by a one-line doctrine amendment, and require the oscc runner to map traps to a distinct non-zero exit code.
14. Restate the estimate as 7 to 9 builder-days and 10 to 14 calendar days.

## Reviewer conditions (aien-dev-ba, 2026-10-16)

aien-dev-ba answered the three questions the advisor review left open. The answers are binding on
Slices A and C; the affected sections (1.3 item 13, 3.1, 3.2 item 4, 3.4, 3.5) were rewritten to match.

1. **OscRt grows by 16 bytes, not 8.** `externs` sits at offset 104 and `log_ext` at offset 112, so
   host-side state moves 16 bytes. Slice C adds `_Static_assert`s for both offsets, `osc_rt_init`
   initialises both fields (`externs = NULL`, `log_ext = osc_rt_log_ext`), every runner refuses an
   extern-using unit while `externs` is NULL (EXTERN_NO_TABLE), and 3.4 now specifies how `log_ext`
   receives up to 16 argument words plus the return word (a 17-word ext area in the frame, one pointer
   argument, fold order ext_index, args, return).
2. **inf and NaN as values.** The ADR OMEGA-SYSTEMS-CORE-0000 is frozen. Slice A does not edit the
   text of decision 6; it appends a dated clarification note under decision 6 saying that inf and NaN
   are ordinary values, not invariant violations, so float arithmetic raises no trap.
3. **extern is an effect, with conditions.** No unwinding or longjmp from C: an extern never calls
   `rt->trap`, it returns a status word. The static `{name, fnptr}` table is the allowlist.
   `--allow-extern` is recorded in the unit header and compared by determinism.sh. A slice-taking
   extern also takes an explicit `u64` length (EXTERN_SLICE_LEN otherwise). Effects are declared per
   extern when OSC-3 item 4 ships, and the flag retires then. The libm identity `ldd` check (3.1) is a
   red check in Slice C.
