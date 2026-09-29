/* Omega mixed algebra MA-2: realizations of one exact semantic operation.
 * spec/mixed-algebra-ma2.md
 *
 * Operation Omega-X ("ternary GEMV"):
 *   y = W . x,  W in {-1,0,+1}^(m x n) (row-major int8),  x in int8^n,
 *   y in int32^m, EXACT (every realization is bit-identical to the naive
 *   integer oracle).
 *
 * Overflow contract: |y_i| <= 128 * n (x may be -128), so n is limited to
 * OMA_RZ_MAX_N = floor(INT32_MAX / 128) = 16,777,215 (same bound as
 * oma_dot_tw_i8). Realizations with narrower index types (sparse: uint16
 * column indices) state their own lower limit through max_n.
 *
 * A realization is a pair (pack, run): pack converts the canonical int8
 * ternary matrix into the realization's own weight form (validating every
 * weight), run computes y for one x. Pack cost is measured separately so the
 * benchmark can report "pack once, amortized" and "pack per call".
 */
#ifndef OMA_REALIZE_COMMON_H
#define OMA_REALIZE_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define OMA_RZ_MAX_N ((size_t)16777215u) /* INT32_MAX / 128 */

enum {
    OMA_RZ_OK = 0,
    OMA_RZ_E_ARG = -1,      /* NULL, zero-size where not allowed, n above max_n */
    OMA_RZ_E_TRIT = -2,     /* a weight outside {-1,0,+1} */
    OMA_RZ_E_NOMEM = -3,
    OMA_RZ_E_OVERFLOW = -4  /* size arithmetic would overflow */
};

typedef struct {
    size_t m, n;
    void *mem;              /* owned allocation(s) holding the packed weights */
    void *aux;              /* second owned allocation (index lists, row offsets) */
    void *scratch;          /* per-call scratch (tables, residues), owned */
    size_t weight_bytes;    /* bytes of packed weight data read by one run */
    size_t footprint_bytes; /* all bytes the plan holds (weights + aux + scratch) */
    size_t scratch_bytes;   /* per-call scratch written/read by one run */
    uint64_t nnz;           /* non-zero weights (informational) */
    int stride;             /* realization-private layout parameter */
} oma_rz_plan;

typedef struct {
    const char *id;         /* short stable id, e.g. "R1_sdot" */
    const char *label;      /* one-line human description */
    const char *family;     /* "binary", "bitplane", "lut", "crumb2", "sparse", "rns", "dense5" */
    int exact;              /* 1: bit-exact contract; every realization here is exact */
    int weak_baseline;      /* 1: labelled weak baseline, not a champion candidate */
    size_t max_n;           /* largest n the realization accepts */
    int (*pack)(oma_rz_plan *p, const int8_t *w, size_t m, size_t n);
    int (*run)(const oma_rz_plan *p, const int8_t *x, int32_t *y);
} oma_rz_impl;

/* All realizations, in a fixed order. */
size_t oma_rz_count(void);
const oma_rz_impl *oma_rz_get(size_t i);
const oma_rz_impl *oma_rz_find(const char *id);

void oma_rz_free(oma_rz_plan *p);
const char *oma_rz_strerror(int rc);

/* Shared helpers. */
int oma_rz_check_shape(size_t m, size_t n, size_t max_n);
/* OMA_RZ_OK if every entry of w[m*n] is -1, 0 or +1. */
int oma_rz_validate(const int8_t *w, size_t m, size_t n);
void *oma_rz_alloc(size_t bytes); /* 64-byte aligned, zeroed; NULL on failure */

/* Naive oracle (plain int32 loop, no vectorization tricks). */
int oma_rz_oracle(const int8_t *w, size_t m, size_t n, const int8_t *x, int32_t *y);

/* Per-family declarations (each file registers its impls). */
extern const oma_rz_impl oma_rz_r1_plain, oma_rz_r1_sdot, oma_rz_r1_sdot_il, oma_rz_r1_smmla;
extern const oma_rz_impl oma_rz_r2_bitplane, oma_rz_r2b_lut, oma_rz_r2c_crumb;
extern const oma_rz_impl oma_rz_r3_sparse;
extern const oma_rz_impl oma_rz_r4_rns;
extern const oma_rz_impl oma_rz_r5_dense5;

#endif /* OMA_REALIZE_COMMON_H */
