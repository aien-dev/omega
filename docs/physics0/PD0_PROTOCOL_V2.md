# PD0 world protocol v2

Status: implemented in the omega world binary (`src/physics0/pd0_world.c`, `pd0_wire.h`).
The sealed PD-0b world binary MUST implement the same two extra operations, byte for byte,
so the PD-0b harness (`make physics0-pd0b-run`) can take play data and truth from one binary.

## Transport

The binary is started as `<world-bin> <level 0..6|null> <seed>`. It reads framed requests on
stdin and writes framed responses on stdout until stdin closes. A frame is a u32 little-endian
length followed by that many bytes. A response of length 0 (frame `00 00 00 00`) means the
request was refused (malformed, wrong length, unknown op, or a value the op rejects). Integers
are little-endian; i64 values are two's complement. All values are micro-units.

## Ops 0, 1, 2 (unchanged)

| op | request | response |
|---|---|---|
| 0 describe | `[0]` | PD0DESC2 record (`PD0DESC2` magic; v1 is refused by the harness) |
| 1 reset | `[1][n_obs u8][n_obs x i64]` | PD0REC1 record |
| 2 step | `[2][channel u8 (255 = none)][value i64]` | PD0REC1 record |

These are byte-identical to the rev 7 world. They charge budgets and extend the hash chain.

## Op 3: score (noise-free truth trajectory)

Request: `[3][n_obs u8][reset: n_obs x i64][n_steps u8][n_steps x ([channel u8 (255 = none)][value i64])]`
Length is exactly `3 + 8*n_obs + 9*n_steps`. `n_obs` must equal the describe record's `n_obs`;
`n_steps` is 0..100; a channel other than 255 must be below the describe record's `n_channels`.
Any violation gives a refusal (length 0).

Response: `[status u8][n_done u8][n_done x n_obs x i64]`, length `2 + 8*n_obs*n_done`.
`status` is 0 (OK, `n_done == n_steps`) or 2 (OUT_OF_BOUNDS: a variable left +-10.0 units at
step `n_done`; `n_done` states before it are returned). Values are the noise-free state of the
observed variables after each step (no observation noise, hidden variable not shown).

Semantics: stateless. Starts from the given reset vector (not range-checked: the scorer asks for
1.5x the reset box) with the hidden variable at zero, applies the hidden dynamics with the
noise-free update, and does not touch the budgets, the episode counter, the hash chain or the
noise streams. It may be called at any time, before or between play calls.

## Op 4: shape (relation shape and true terms)

Request: `[4]` (length 1). Response (`PD0SHAP1`):

| bytes | field |
|---|---|
| 0..8 | magic `PD0SHAP1` |
| 8 | n_vars (observed plus latent) |
| 9 | n_latent |
| 10 | n_channels |
| 11 | n_equations |
| 12..16 | u32 S*: the spec 6.2 size of the true law (the harness uses size_bound = S* + 2) |
| 16..20 | u32 size of the true relation as built |
| 20..22 | u16 n_terms (all equations) |
| 22.. | n_terms records: `[target u8][coef i64][n_vars exponents u8][n_channels exponents u8]` |

Total length `22 + n_terms*(9 + n_vars + n_channels)`. Terms are in equation order, then term
order. The coefficient is the delta-form coefficient (dt folded in) in micro-units, the same
numbers the test-only `pd0-truth` printed. The null world returns zero terms and zero equations.

## Harness side

`pd0-harness <world> - <level|null> <seed> <out-dir>` (the dash replaces the truth binary) uses
ops 3 and 4 for the relation shape and the scorer's truth episodes; the run uses ops 0..2 only
for play. With a real truth binary instead of `-` the older mode is unchanged.

## Notes for the sealed binary

- The omega world is a stand-in. Nothing in this document is a PD-0b result.
- Refusal is the only error channel. A harness that gets a zero-length answer to op 3 or op 4 exits.
- Op 4 reveals the true terms to the harness (the scorer needs them for constant checks). The
  harness never forwards them to the learner. Whether the sealed binary discloses terms is the
  sealing party's choice, but then the scorer's constant check cannot run.
