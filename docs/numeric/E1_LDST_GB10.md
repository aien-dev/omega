# E1 row 2: general load/store on the GB10

Code: `src/omega_numeric_ldst_gb10.{h,c}`. Tests: `tests/test_omega_ldst_gb10.c`,
`tools/ldst_nvdisasm_check.sh`, `tools/run_numeric_ldst_chip.sh`.

## Contract
One thread per element `i < count`: `reg = LOAD_ld(in + i*in_stride + in_off)`, then
`STORE_st(out + i*out_stride + out_off, reg)`.
- Loads: U8, S8, U16, S16 (zero or sign extended to 32 bits), B32, B64, B128.
- Stores: B8, B16, B32, B64, B128 (the low bytes of the register file, little endian).
- Strides are byte counts applied by a 64-bit multiply-add; offsets are signed 24-bit byte offsets.
- Refused: a store wider than what the load filled; a stride below the width or not a multiple of it;
  an offset not a multiple of the width or outside the signed 24-bit range; a stride of 2^24 or more;
  count 0 or above `OMEGA_DS_MAX_BATCH`; any access outside the buffers (host model and executor both check).
- Buffers are 16-byte aligned with `OMEGA_LDST_PAD` bytes in front of element 0.

## Encoding source
The kernel keeps the 17 verified prologue words of the vecadd kernel and the EXIT, BRA and NOP words
(`src/omega_blackwell_encoder.c`). New or changed words (each read back with nvdisasm 13.0.85 `-b SM121`
as an offline decoder, varying one bit at a time, and then checked word for word on 148 kernels):

| Instruction | Word layout |
|---|---|
| `LDG.E[.kind] R12, desc[UR4][R2.64+off]` | w0 = `0x7981 | 12<<16 | 2<<24`, w1 = `4 | off24<<8`, w2 = `0x0c1e1100 | size<<9`, w3 = `0x002f2200` |
| `STG.E[.kind] desc[UR4][R6.64+off], R12` | w0 = `0x7986 | 6<<24`, w1 = `12 | off24<<8`, w2 = `0x0c101104 | size<<9`, w3 = `0x000fe200` |
| `IMAD.WIDE.U32 R2, R9, stride, R2` and the R6 form | vecadd word with w1 = stride |
| `IADD3 R9, PT, PT, R12, R5, RZ` | vecadd IADD3 with Ra = R12 (carries the scheduling wait for the loads) |

Size field: 0 U8, 1 S8, 2 U16, 3 S16, 4 32-bit, 5 64-bit, 6 128-bit; value 7 decodes as `INVALID7` and is never emitted.
The load destination is R12 so the 128-bit form is register-group aligned. The prologue's second load (R5,
read from the "b" argument) is kept so the scheduling of the verified words is unchanged; the executor passes the
input buffer as "b".

## Checks
Pre-submission structural check `omega_ldst_check_kernel` decodes the fields back out of the words and requires a
byte-for-byte rebuild (markers `CHECK:ldst_*`). Offline: `make test-ldst-nvdisasm`. Host: `make test-ldst-host`.
Chip: forge job `E1B-LDST-CHIP`; a FAIL with unwritten bytes (fill pattern left where the model wrote) is C3.
