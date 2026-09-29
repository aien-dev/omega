/* Omega mixed algebra (oma_): correctness-first CPU reference ("parity oracle").
 * spec/mixed-algebra-reference.md
 *
 * Balanced trit encoding (H1), bitplanes:
 *   -1 -> (pos=0,neg=1)   0 -> (0,0)   +1 -> (1,0)   (1,1) -> INVALID
 * value = pos - neg. Every decoder/validator rejects (1,1) with an error code.
 */
#ifndef OMA_TRIT_H
#define OMA_TRIT_H

#include <stddef.h>
#include <stdint.h>

/* Explicit return codes. OMA_OK is 0; every error is negative. */
enum {
    OMA_OK = 0,
    OMA_E_INVALID_TRIT = -1,   /* int8 value outside {-1,0,1} */
    OMA_E_INVALID_CODE = -2,   /* 2-bit code 0b11 (pos=1,neg=1), < 0 or > 3 */
    OMA_E_INVALID_PLANES = -3, /* block with pos & neg != 0 */
    OMA_E_OVERFLOW = -4,       /* result does not fit the requested width */
    OMA_E_INVALID_BYTE = -5,   /* dense byte >= 243 or non-zero padding */
    OMA_E_INVALID_Z3 = -6,     /* Z3 value outside {0,1,2} */
    OMA_E_ARG = -7,            /* NULL pointer, bad length, non-finite input */
    OMA_E_UNDERFLOW = -8       /* quant: non-zero input whose mean |w| is below FLT_MIN */
};

const char *oma_strerror(int rc);

/* ---- single trit ---- */
typedef int8_t oma_trit; /* always one of -1, 0, +1 once constructed */

/* Checked constructor: accepts only -1, 0, +1. */
int oma_trit_make(int v, oma_trit *out);

/* 2-bit code: bit0 = pos plane, bit1 = neg plane. 0b11 is invalid. */
#define OMA_CODE_ZERO 0u
#define OMA_CODE_POS 1u
#define OMA_CODE_NEG 2u
#define OMA_CODE_INVALID 3u

/* Scalar inputs are taken as int so that out-of-range values (256, -129, ...)
 * reach the range check instead of wrapping at the call. */
int oma_trit_to_code(int t, uint8_t *code);
int oma_code_to_trit(int code, oma_trit *out);

/* Per-trit ops on codes. Any input code 0b11 (or < 0 or > 3) -> OMA_E_INVALID_CODE. */
int oma_code_neg(int a, uint8_t *out);
/* Balanced add: a + b = sum + 3*carry, sum and carry both trits. */
int oma_code_add(int a, int b, uint8_t *sum, uint8_t *carry);
int oma_code_mul(int a, int b, uint8_t *out);

/* Same ops on trit values (inputs checked, outputs are trits). */
int oma_trit_neg(int a, oma_trit *out);
int oma_trit_add(int a, int b, oma_trit *sum, oma_trit *carry);
int oma_trit_mul(int a, int b, oma_trit *out);

/* ---- 64-trit block: lane i is bit i of each plane ---- */
#define OMA_BLOCK_TRITS 64
typedef struct {
    uint64_t pos;
    uint64_t neg;
} oma_block;

int oma_block_validate(const oma_block *b); /* OMA_OK or OMA_E_INVALID_PLANES */
int oma_block_encode(const int8_t in[OMA_BLOCK_TRITS], oma_block *out);
int oma_block_decode(const oma_block *b, int8_t out[OMA_BLOCK_TRITS]);

/* Lane-wise ops; all validate their inputs first. */
int oma_block_neg(const oma_block *a, oma_block *out);
/* Lane-wise balanced add, no carry propagation between lanes:
 * a_i + b_i = sum_i + 3*carry_i. */
int oma_block_add(const oma_block *a, const oma_block *b, oma_block *sum, oma_block *carry);
/* H2 elementwise multiply: pos=(ap&bp)|(an&bn), neg=(ap&bn)|(an&bp). */
int oma_block_mul(const oma_block *a, const oma_block *b, oma_block *out);

/* ---- dot products (exact) ---- */
/* block . block = popcount(pos) - popcount(neg) of the H2 product. */
int oma_block_dot(const oma_block *a, const oma_block *b, int32_t *out);

/* Ternary weights x int8 activations via add/sub only (H3), exact int32.
 * w holds ceil(n/64) blocks; lanes at index >= n must be zero (else OMA_E_ARG).
 * n <= OMA_DOT_I8_MAX_N guarantees |result| <= 128*n fits int32. */
#define OMA_DOT_I8_MAX_N ((size_t)16777215u)
int oma_dot_tw_i8(const oma_block *w, const int8_t *x, size_t n, int32_t *out);

/* ---- integer <-> balanced ternary digits (little-endian) ---- */
/* Enough digits for every int64: (3^41 - 1)/2 > 2^63. */
#define OMA_INT64_MAX_DIGITS 41
/* Canonical (minimal) form: no leading zero digit; 0 has zero digits.
 * cap too small -> OMA_E_OVERFLOW. */
int oma_int_to_bt(int64_t v, int8_t *digits, size_t cap, size_t *ndigits);
/* Exactly n digits, zero padded; value needing more than n -> OMA_E_OVERFLOW. */
int oma_int_to_bt_fixed(int64_t v, int8_t *digits, size_t n);
/* Any digit not in {-1,0,1} -> OMA_E_INVALID_TRIT; value outside int64 ->
 * OMA_E_OVERFLOW. Non-canonical (leading zero) input is accepted. */
int oma_bt_to_int(const int8_t *digits, size_t n, int64_t *out);

#endif /* OMA_TRIT_H */
