/* Turing Yield: context-table predictors and their exact model code.
 *
 * A model is a table of 16-bit quantized distributions over K outcome symbols:
 * one default row plus one row per stored context key. P(x_t | ctx_t) =
 * row(ctx_t)[x_t] / 65536, or default[x_t] / 65536 when ctx_t has no row.
 * Every entry is >= 1 (the floor, 2^-16) and each row sums to exactly 65536.
 *
 * L(M) is the exact bit length of the model code written by ty_model_encode
 * (MSB-first bitstream):
 *   "TYM0" 32 | version 8 (=0) | K 8 | feature mask 8 | qbits 8 (=16) |
 *   key bits 8 | row count 32 | default row: K-1 entries x 16 |
 *   per row, keys strictly increasing: key (key bits) | K-1 entries x 16.
 * The K-th entry of each row is implied (65536 minus the others). The decoder
 * (ty_model_decode) rebuilds the identical table, and scoring always runs on a
 * decoded table, so the model that is scored is the model that is paid for.
 */
#ifndef TURING_TY_MODEL_H
#define TURING_TY_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "turing/ty_ctr1.h"
#include "turing/ty_math.h"

#define TY_MODEL_VERSION 0
#define TY_MODEL_DOMAIN "turing.ymodel.v0"
#define TY_MODEL_HEADER_BITS 104

/* Context features (mixed-radix key, in this order, low to high). */
enum {
    TY_F_OP = 1,    /* op feature, radix 16 */
    TY_F_DEPTH = 2, /* clipped depth, radix 8 */
    TY_F_PREV1 = 4, /* previous outcome in the crumb or start-of-crumb, radix K+1 */
    TY_F_PREV2 = 8,
    TY_F_PREV3 = 16,
    TY_F_POS = 32,  /* (crumb ordinal, event_index): a position lookup key, radix 2^36 */
    TY_F_PREV4 = 64,
    TY_F_PREV5 = 128,
    TY_F_ALL = 255
};

typedef struct {
    uint64_t key;
    uint32_t q[TY_KMAX];
} ty_row;

typedef struct {
    unsigned K;
    unsigned mask;
    unsigned keybits;
    uint32_t def[TY_KMAX];
    size_t nrows;
    ty_row *rows;
} ty_model;

/* Fit procedures (declared in the profile). */
enum { TY_FIT_KEEP_ALL = 0, TY_FIT_MDL_PRUNE = 1 };

void ty_model_free(ty_model *m);
/* Radix product of the mask's features, and ceil(log2) of it. */
int ty_key_space(unsigned mask, unsigned K, uint64_t *space, unsigned *bits);
/* Fit on streams with KT smoothing + 16-bit quantization; rows kept per rule. */
int ty_model_fit(ty_model *m, const ty_stream *const *streams, size_t ns, unsigned mask, unsigned K, int rule);
/* Uniform-as-quantized model (no counts, no rows). */
int ty_model_uniform(ty_model *m, unsigned K);
/* Model code. *bits = L(M). Caller frees *buf. */
int ty_model_encode(const ty_model *m, uint8_t **buf, size_t *nbytes, uint64_t *bits);
int ty_model_decode(const uint8_t *buf, size_t nbytes, ty_model *m, uint64_t *bits, char *why, size_t whylen);
/* L(D|M) in micro-bits over a stream; *nsym = events scored. */
int ty_model_score(const ty_model *m, const ty_stream *s, int64_t *ub, uint64_t *nsym);
/* SHA-256(TY_MODEL_DOMAIN || 0x00 || code bytes). */
void ty_model_digest(const uint8_t *buf, size_t nbytes, uint8_t out[32]);

#endif /* TURING_TY_MODEL_H */
