# OSC external byte slice: design spec (not implemented)

**Status**: implemented on branch osh/osc-ext-bytes (arch#158).
**Written by**: Lane 42, migration seam 1 (CRB1 crumb reader), 2026-10-01.
**Why**: prerequisite for an Omega-written reader of crumb lesson files. The reader contract is
aien-protocols `specs/crumb-visible/CRUMB_READER_CONTRACT.md` 1.0.0 (reader = pure function of a
caller-owned, read-only byte buffer given as pointer + length). OSC today has no way to receive
such a buffer: arrays come only from its own pool (`osc_rt.h` cells, `OSC_MAX_ARRAY_LEN` 64 in
`osc_ir.h`), and a CRB1 record can be thousands of bytes.

## 1. Feature

Two new parameter types, usable only as function parameters:

| Type | Meaning | Passed as |
|---|---|---|
| `bytes` | read-only borrowed external byte slice | two consecutive parameter registers: pointer, then length (u64) |
| `cells` | caller-supplied writable u64 output slice | two consecutive parameter registers: pointer, then length in cells (u64) |

`bytes` is the required minimum ("read a caller-supplied byte buffer as an external input").
`cells` is the matching output side: a CRB1 record decodes to up to 64 x 16 lane values, more
than one OSC array holds, so results go to a caller buffer (or are reduced to a digest).

## 2. Grammar (additions)

```
param  += NAME ':' 'bytes' | NAME ':' 'cells'
expr   += NAME '.' 'len'            (u64; NAME is a bytes or cells parameter)
        | NAME '[' expr ']'          (bytes: u8 load zero-extended to u64; cells: u64 load)
stmt   += NAME '[' expr ']' '=' expr ';'   (cells only: u64 store)
```

## 3. Semantics

- **Borrow for one call.** The caller owns both buffers. They are valid from entry to return of
  the outermost compiled function and never afterwards. The runtime keeps no reference.
- **Bounds.** Every `b[i]` and `c[i]` checks `i < len` (unsigned) before the access and traps
  `OSC_TRAP_BOUNDS` (3) otherwise. No unchecked form exists.
- **Read only input.** A `bytes` parameter is never written; there is no syntax for it.
- **Empty slices.** `len = 0` is valid; the pointer may then be null and every index traps.
- **No aliasing.** The caller guarantees the `bytes` and `cells` ranges do not overlap. The host
  harness checks this before the call and refuses overlapping buffers (it never calls in).
- **Passing down.** A slice parameter may be passed whole to another compiled function's slice
  parameter (same type). It uses two of the six parameter registers (x0..x5; x7 stays the
  runtime context).
- **Determinism.** Loads and stores have no side effects beyond the cells written; the result of
  a call is a function of the input bytes and the arguments only.

## 4. Static refusals (named, at compile time)

- `SLICE_ESCAPE`: a slice is returned, stored in a local, struct field, array element, pool, arena
  or any durable state.
- `SLICE_VALUE`: a slice is used as a value (arithmetic, comparison, cast), other than
  `.len` and indexing.
- `SLICE_WRITE`: an assignment to an element of a `bytes` parameter.
- `SLICE_PARAMS`: a function whose parameters need more than six registers once each slice counts
  as two.

## 5. Native ABI (AArch64)

Pointer in `xN`, length in `xN+1`, in declaration order. Element access lowers to
`CMP idx, len` + `B.HS trap_bounds` + `LDRB`/`LDR`/`STR` with register offset (cells scaled by 8).
Native code is entered through `osc_rt_call_native` (`osc_rt.h`) and the interpreter through
`osc_interp_run` (`osc_interp.h`); both take the same pointer and length, so the native vs
interpreter differential covers slices unchanged.

## 6. Encoding and versioning

A compiled unit's canonical encoding gains two type tags. Per the repository rule (magic +
version; old versions refused, never reinterpreted) this is a unit-encoding version bump; a unit
using slices is refused by a compiler or loader that does not know the new version.

## 7. Acceptance (for the implementing lane)

1. Compiler tests: in-range loads; `i = len` traps BOUNDS; `len = 0` traps on any index; each
   named refusal fires; native and interpreter agree on every test.
2. Host harness under ASan/UBSan: a buffer of exactly `len` bytes (no slack) with guard pages or
   ASan redzones shows no out-of-range access.
3. Then, as separate work (not part of this feature): a CRB1 reader written in `.osc` behind the
   same entry shape as `cl_crumb_decode` passes
   `CRUMB_VISIBLE_V1_CONFORMANCE impl=osc pass=171 fail=0` on the shared corpus with the
   **contract** codes (aien-protocols specs/crumb-visible, vendored in `tests/crumbline/crb1`),
   not the current C codes: the C reader has 16 recorded code differences
   (`tests/crumbline/crb1_findings.txt`). Contract section 6 also requires a UTF-8 validity check
   (invalid UTF-8 is LANE, valid non-digit is NONCANONICAL), which the `.osc` reader must code.

## 8. Not in scope

Writing the Omega reader; any change to `src/runtime`; strings, allocation from slices, pointer
arithmetic, mutable input, slices in structs; any change to the crumb format.

## 9. Design gaps found while implementing

1. No byte load or store in the assembler. Added `OSC_A64_LDRB_REG` (register offset,
   `0x38606800 | Rm<<16 | Rn<<5 | Rt`) with its decoder entry, so the self-check in `osc_cg.c`
   passes. The encoding was cross-checked against GNU `as`/`objdump`.
2. `bounds()` compared against an immediate. Added `bounds_r()` in `osc_cg.c`: an unsigned
   register compare (`cmp` then `b.hs` to the BOUNDS trap). Negative signed indices are huge
   unsigned values, so they trap too. An empty slice never touches its pointer.
3. The interpreter refuses addresses outside its pool. Slice access is its own checked path
   (`sload`/`sstore` in `osc_interp.c`: `idx >= len` traps, then reads or writes through the
   caller pointer). The pool check is unchanged.

Smaller gaps closed on the way: there was no unit loader, so `osc_ir_encoding_version` gives the
"unknown version refused" rule (plain units stay version 1 to 4, slice units are 5); there was no
host entry, so `osc_ir_slice_args_ok` refuses overlapping, NULL-with-length or wrapping buffers
before the call; `b[i]` is typed u64 (u8 zero-extended).

### Review fixes (PR #335 hostile review)

- Native entry was unchecked: `osc_native_call` (osc_native.c) runs `osc_ir_slice_args_ok` and
  refuses (returns -1, never calls in) before `osc_rt_call_native`, which stays the raw entry.
  The slice tests' native path goes through it.
- Static aliasing: passing one `cells` slice to two parameters of one call is refused at compile
  time as `SLICE_ALIAS` (a cells passed as `bytes` is already a type mismatch). `bytes` twice is
  harmless and allowed.
- `osc_ir_slice_args_ok` also refuses a `cells` pointer that is not 8-byte aligned (both paths).
- `ensures result <= b.len` is now accepted: a slice length is immutable, so contracts may read
  `.len`. Reading `cells` elements in `ensures` stays refused.
