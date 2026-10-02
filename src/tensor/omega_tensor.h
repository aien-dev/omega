#ifndef OMEGA_TENSOR_H
#define OMEGA_TENSOR_H

/*
 * M20 OMEGA_TENSOR: immutable semantic tensors (docs/tensor/M20_OMEGA_TENSOR.md).
 *
 * Meaning and realization are separate. This header defines what a tensor is
 * and what each op computes. Arithmetic is never done here: every arithmetic
 * step goes through an OmegaTensorRealization (a table of functions). The
 * semantic layer only moves data (gathers elements by index) and checks.
 * The CPU realization (omega_tensor_cpu_realization) is the only one today;
 * a GB10 SIMT or Tensor Core realization plugs into the same table later and
 * must reproduce the declared bits, or declare its own bounded contract.
 *
 * Tensors are immutable. There is no API that writes into an existing tensor:
 * creation copies the caller's data into new storage, every op returns a new
 * tensor, and views (permute, slice, reshape, broadcast) share the parent's
 * storage read-only and never change the parent.
 *
 * Identity:
 *   value identity   = dtype + shape + element values in logical row-major
 *                      order (NaN payload is not semantic: every NaN counts as
 *                      the canonical quiet NaN; -0 and +0 differ). Strides,
 *                      offset and storage are NOT part of it: "physical layout
 *                      is not identity". omega_tensor_value_id hashes exactly
 *                      this (SHA-256, OMEGA_TENSOR_VALUE_ID_DOMAIN).
 *   storage identity = the allocation: storage slot + 64-bit generation.
 *   view identity    = storage identity + offset + shape + strides + dtype,
 *                      plus the parent tensor it was taken from.
 *
 * Storage lifetime: every storage slot carries a 64-bit generation. A handle
 * is valid only while its generation equals the slot's and the slot is live.
 * Releasing bumps the generation, so every older handle (and every view of
 * that storage) fails closed with OMEGA_TENSOR_ERR_STALE. A slot whose
 * generation reaches UINT64_MAX is retired forever instead of wrapping.
 *
 * Shapes: rank 0..OMEGA_TENSOR_MAX_RANK, every dimension >= 1 (no empty
 * tensors), total elements <= OMEGA_TENSOR_MAX_ELEMS. Strides are counted in
 * elements and are >= 0; 0 appears only in broadcast views. Negative strides
 * are not defined (refused).
 *
 * Dtypes: F32 is the arithmetic dtype. F16 and BF16 are storage dtypes: any
 * arithmetic op on them is refused (OMEGA_TENSOR_ERR_DTYPE); convert first
 * with omega_tensor_cast, which uses the E1 conversion ops
 * (F32_TO_F16 / F32_TO_BF16 RNE, F16_TO_F32 / BF16_TO_F32 exact).
 *
 * Not thread safe: one context per thread.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_numeric.h"

#define OMEGA_TENSOR_MAX_RANK  8u
#define OMEGA_TENSOR_MAX_ELEMS ((uint64_t)1 << 30)
#define OMEGA_TENSOR_VALUE_ID_DOMAIN "OMEGA_TENSOR_VALUE_ID_V1"
#define OMEGA_TENSOR_VIEW_ID_DOMAIN  "OMEGA_TENSOR_VIEW_ID_V1"

/* Frozen reduction order: must equal OMEGA_REDUCE_DECLARED_ORDER of the E1
 * reduction contract (docs/numeric/E1_REDUCTION_CONTRACT.md, omega PR #134).
 * See src/tensor/omega_tensor_reduce_seam.h. */
#define OMEGA_TENSOR_REDUCE_DECLARED_ORDER \
    "RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL"

/*
 * Declared accumulation order of matmul. For every output element
 * out[..., i, j]: products p[k] = FMUL_RNE(a[..., i, k], b[..., k, j]) for
 * k = 0..K-1 (each rounded once, no fused multiply-add), then
 * out = E1 REDUCE_SUM over p[0..K) in OMEGA_TENSOR_REDUCE_DECLARED_ORDER.
 * A realization that fuses (FFMA chain, Tensor Core MMA) computes different
 * bits and must declare its own bounded contract instead of this one.
 */
#define OMEGA_TENSOR_MATMUL_DECLARED_ORDER \
    "MATMUL_V1_FMUL_RNE_PRODUCTS_THEN_E1_REDUCE_SUM_" \
    "RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL"

typedef enum {
    OMEGA_TENSOR_OK                  = 0,
    OMEGA_TENSOR_ERR_BAD_ARGS        = -1,  /* NULL pointer, unknown op/enum      */
    OMEGA_TENSOR_ERR_RANK            = -2,  /* rank out of range for this op      */
    OMEGA_TENSOR_ERR_SHAPE           = -3,  /* dims mismatch / not broadcastable  */
    OMEGA_TENSOR_ERR_DTYPE           = -4,  /* dtype not allowed for this op      */
    OMEGA_TENSOR_ERR_STALE           = -5,  /* handle generation does not match   */
    OMEGA_TENSOR_ERR_BOUNDS          = -6,  /* view reaches outside its storage   */
    OMEGA_TENSOR_ERR_NOT_CONTIGUOUS  = -7,  /* reshape of a non row-major layout  */
    OMEGA_TENSOR_ERR_CAPACITY        = -8,  /* no free slot / size overflow / OOM */
    OMEGA_TENSOR_ERR_AXIS            = -9,  /* axis / permutation invalid         */
    OMEGA_TENSOR_ERR_NUMERIC         = -10, /* realization refused (E1 code kept) */
    OMEGA_TENSOR_ERR_REALIZATION     = -11, /* realization table incomplete       */
    OMEGA_TENSOR_ERR_MASK            = -12  /* where(): cond not exactly +1.0/+0.0 */
} OmegaTensorStatus;

typedef enum {
    OMEGA_DT_F32  = 1,
    OMEGA_DT_F16  = 2,
    OMEGA_DT_BF16 = 3
} OmegaDType;

/* Opaque handles. Copying a handle never copies or shares write access. */
typedef struct { uint32_t slot; uint64_t generation; } OmegaStorageHandle;
typedef struct { uint32_t slot; uint64_t generation; } OmegaTensor;

/* Elementwise ops. Every op is an E1 scalar op applied per element.        */
typedef enum {
    OMEGA_TU_SQRT = 0,   /* omega_math_sqrt, correctly rounded              */
    /* E1 bounded-contract transcendentals (NOT correctly rounded): each is
     * the frozen E1 CPU sequence in src/omega_numeric_transc.c applied per
     * element, bit-exact with calling it directly; max ulp distance from the
     * correctly rounded result per docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md. */
    OMEGA_TU_EXP2,       /* omega_math_exp2,    <= OMEGA_TRANSC_MAX_ULP_EXP2 (2)    */
    OMEGA_TU_LOG2,       /* omega_math_log2,    <= OMEGA_TRANSC_MAX_ULP_LOG2 (2)    */
    OMEGA_TU_SIGMOID,    /* omega_math_sigmoid, <= OMEGA_TRANSC_MAX_ULP_SIGMOID (3) */
    OMEGA_TU_TANH,       /* omega_math_tanh,    <= OMEGA_TRANSC_MAX_ULP_TANH (3)    */
    /* M20 cut ops (LT-M21 CR-1..CR-5). EXP / LOG: the E1 Omega-defined
     * polynomial sequences omega_math_exp / omega_math_log
     * (src/omega_numeric.c:458, :501) applied per element through the
     * realization's transc entry, bit-exact with calling them directly. NOT
     * correctly rounded: E1 checks them against binary128 within EXP 40 ulp,
     * LOG 4 ulp (tests/test_omega_numeric.c:376-384). E1 has no GB10 kernel
     * for either (src/omega_numeric.c:585-588).
     * RELU: no E1 op; a tensor-layer bit select, no float arithmetic:
     * x > +0 (incl. +inf, +subnormals) -> x; +0, -0, negatives, -inf -> +0;
     * any NaN (any sign or payload) -> canonical qNaN OMEGA_QNAN_BITS. */
    OMEGA_TU_EXP,        /* omega_math_exp, E1 polynomial                   */
    OMEGA_TU_LOG,        /* omega_math_log, E1 polynomial                   */
    OMEGA_TU_RELU,       /* bit select in the tensor layer (needs no transc) */
    /* LT-M21 CR-1 (unary2), same pattern: E1 sequence per element, bit-exact
     * with calling it directly. Served by the realization's transc entry. */
    OMEGA_TU_RSQRT,      /* omega_math_rsqrt, correctly rounded (OMEGA_TRANSC_MAX_ULP_RSQRT 0) */
    OMEGA_TU_ERF,        /* omega_math_erf,   <= OMEGA_TRANSC_MAX_ULP_ERF (3)     */
    OMEGA_TU_GELU,       /* omega_math_gelu,  <= OMEGA_TRANSC_MAX_ULP_GELU (3), erf form */
    /* E1 trig (bounded contract, not correctly rounded): omega_math_sin /
     * omega_math_cos per element, bit-exact with calling them directly.
     * Admitted domain |x| <= 2^22 (OMEGA_TRANSC_TRIG_MAX_ABS, inclusive);
     * outside it, +-inf and NaN give the canonical qNaN 0x7fc00000.
     * sin(+-0) = +-0, cos(+-0) = +1. */
    OMEGA_TU_SIN,        /* omega_math_sin,     <= OMEGA_TRANSC_MAX_ULP_SIN (2)     */
    OMEGA_TU_COS,        /* omega_math_cos,     <= OMEGA_TRANSC_MAX_ULP_COS (2)     */
    /* Pure sign-bit flip, no E1 call, no arithmetic: bits ^ 0x80000000, so
     * neg(+0) = -0 and neg(-0) = +0. Any NaN in gives the canonical quiet
     * NaN 0x7fc00000 (E1 convention, omega_numeric.h:137). Needs no transc. */
    OMEGA_TU_NEG,
    OMEGA_TU_COUNT
} OmegaTensorUnaryOp;

typedef enum {
    OMEGA_TB_ADD = 0,    /* FADD RNE                                        */
    OMEGA_TB_SUB,        /* FSUB RNE                                        */
    OMEGA_TB_MUL,        /* FMUL RNE                                        */
    OMEGA_TB_DIV,        /* omega_math_div, correctly rounded               */
    OMEGA_TB_MIN,        /* FMNMX_MIN: NaN is missing data, -0 < +0         */
    OMEGA_TB_MAX,        /* FMNMX_MAX                                       */
    /* compare-select: out = P(a, b) ? a : b (E1 FSETP_<P>_SEL)            */
    OMEGA_TB_SEL_GE, OMEGA_TB_SEL_LT, OMEGA_TB_SEL_LE, OMEGA_TB_SEL_GT,
    OMEGA_TB_SEL_EQ, OMEGA_TB_SEL_NE, OMEGA_TB_SEL_NUM, OMEGA_TB_SEL_NAN,
    OMEGA_TB_SEL_LTU, OMEGA_TB_SEL_LEU, OMEGA_TB_SEL_GTU, OMEGA_TB_SEL_GEU,
    OMEGA_TB_SEL_EQU, OMEGA_TB_SEL_NEU,
    /* CR-3 mask compare: out = P(a, b) ? +1.0 : +0.0 (bits 0x3f800000 /
     * 0x00000000, never -0.0). P is EXACTLY the predicate of the ordered E1
     * compare-select op named on the right (omega_ref_fsetp_pred,
     * src/omega_numeric.c:322): false when either operand is NaN (CMP_NE too:
     * NaN != x gives +0.0, unlike IEEE !=), and -0 == +0. Realized by the
     * realization's compare entry (NULL: OMEGA_TENSOR_ERR_REALIZATION).    */
    OMEGA_TB_CMP_EQ,     /* FSETP_EQ_SEL predicate                           */
    OMEGA_TB_CMP_NE,     /* FSETP_NE_SEL predicate (ordered)                 */
    OMEGA_TB_CMP_LT,     /* FSETP_LT_SEL predicate                           */
    OMEGA_TB_CMP_LE,     /* FSETP_LE_SEL predicate                           */
    OMEGA_TB_CMP_GT,     /* FSETP_GT_SEL predicate                           */
    OMEGA_TB_CMP_GE,     /* FSETP_SEL (GE) predicate                         */
    OMEGA_TB_COUNT
} OmegaTensorBinaryOp;

typedef enum {
    OMEGA_TR_SUM = 0,    /* identity -0.0                                   */
    OMEGA_TR_MAX,        /* FMNMX_MAX, identity quiet NaN                   */
    OMEGA_TR_MIN,        /* FMNMX_MIN, identity quiet NaN                   */
    OMEGA_TR_MEAN,       /* omega_math_div(SUM, (float)n), n <= 2^24        */
    OMEGA_TR_COUNT
} OmegaTensorReduceOp;

/*
 * Realization table. All buffers are dense and contiguous; the semantic
 * layer gathers strided/broadcast operands before calling.
 *   elementwise: out[i] = op(a[i], b[i], c[i]) for an E1 OmegaNumericOp,
 *                with the omega_numeric_cpu_realize argument conventions
 *                (b/c may be NULL when the op does not read them).
 *   reduce:      *out = reduce(op, x[0..n)) in OMEGA_TENSOR_REDUCE_DECLARED_ORDER.
 *   reduce_order must equal OMEGA_TENSOR_REDUCE_DECLARED_ORDER.
 * Return 0 or an OMEGA_NUMERIC_* code (kept in the context, mapped to
 * OMEGA_TENSOR_ERR_NUMERIC).
 */
typedef struct {
    const char *name;
    const char *reduce_order;
    int (*elementwise)(OmegaNumericOp op, const float *a, const float *b, const float *c,
                       float *out, size_t n);
    int (*reduce)(OmegaTensorReduceOp op, const float *x, size_t n, float *out);
    /* Optional. out[i] = op(a[i]) for the bounded-contract unary ops
     * (OMEGA_TU_EXP2 .. OMEGA_TU_TANH, OMEGA_TU_EXP, OMEGA_TU_LOG, OMEGA_TU_RSQRT,
     * OMEGA_TU_ERF, OMEGA_TU_GELU, OMEGA_TU_SIN, OMEGA_TU_COS), bit-exact with
     * the E1 CPU sequences.
     * NULL: those ops return OMEGA_TENSOR_ERR_REALIZATION (other ops work). */
    int (*transc)(OmegaTensorUnaryOp op, const float *a, float *out, size_t n);
    /* Optional (CR-3). out[i] = +1.0f if the E1 predicate of the compare-
     * select op sel_op (OMEGA_NOP_FSETP_SEL or an OMEGA_NOP_FSETP_<P>_SEL)
     * holds for (a[i], b[i]), else +0.0f; the predicate equals
     * omega_ref_fsetp_pred(sel_op, a[i], b[i]) bit for bit.
     * NULL: the CMP_* binary ops return OMEGA_TENSOR_ERR_REALIZATION. */
    int (*compare)(OmegaNumericOp sel_op, const float *a, const float *b, float *out, size_t n);
} OmegaTensorRealization;

const OmegaTensorRealization *omega_tensor_cpu_realization(void);

typedef struct OmegaTensorCtx OmegaTensorCtx;

/* capacity = number of storage slots and of tensor descriptor slots. */
int  omega_tensor_ctx_create(uint32_t capacity, const OmegaTensorRealization *real,
                             OmegaTensorCtx **out);
void omega_tensor_ctx_destroy(OmegaTensorCtx *ctx);
/* Last OMEGA_NUMERIC_* code a realization returned (0 if none). */
int  omega_tensor_last_numeric_error(const OmegaTensorCtx *ctx);

/* ---- creation (copies data; data is logical row-major in dtype) ---------- */
int omega_tensor_from_data(OmegaTensorCtx *ctx, OmegaDType dtype, uint32_t rank,
                           const uint64_t *shape, const void *data, OmegaTensor *out);
int omega_tensor_from_f32(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape,
                          const float *data, OmegaTensor *out);

/*
 * Release. On a tensor that owns its storage (made by from_data or an op),
 * releases the descriptor AND the storage: every view of that storage turns
 * stale. On a view, releases only the view descriptor. Releasing twice, or
 * through any stale handle, returns OMEGA_TENSOR_ERR_STALE and changes nothing.
 */
int omega_tensor_release(OmegaTensorCtx *ctx, OmegaTensor t);

/* ---- read-only inspection ------------------------------------------------- */
typedef struct {
    OmegaDType         dtype;
    uint32_t           rank;
    uint64_t           shape[OMEGA_TENSOR_MAX_RANK];
    uint64_t           strides[OMEGA_TENSOR_MAX_RANK];  /* elements, >= 0 */
    uint64_t           offset;                          /* elements       */
    uint64_t           elements;
    OmegaStorageHandle storage;
    bool               is_view;
    OmegaTensor        parent;   /* tensor the view was taken from        */
} OmegaTensorInfo;

int omega_tensor_info(const OmegaTensorCtx *ctx, OmegaTensor t, OmegaTensorInfo *out);
/* Copies elements out in logical row-major order (elem size of the dtype). */
int omega_tensor_read(const OmegaTensorCtx *ctx, OmegaTensor t, void *out, size_t out_bytes);
int omega_tensor_read_f32(const OmegaTensorCtx *ctx, OmegaTensor t, float *out, size_t count);
int omega_tensor_value_id(const OmegaTensorCtx *ctx, OmegaTensor t, uint8_t id[32]);
/* Semantic equality: same dtype, shape and values (NaN class, -0 != +0). */
int omega_tensor_value_equal(const OmegaTensorCtx *ctx, OmegaTensor a, OmegaTensor b, bool *eq);
int omega_tensor_view_id(const OmegaTensorCtx *ctx, OmegaTensor t, uint8_t id[32]);
bool omega_tensor_storage_valid(const OmegaTensorCtx *ctx, OmegaStorageHandle s);
size_t omega_dtype_size(OmegaDType dt);

/* ---- views (share storage, never copy, never mutate the parent) ---------- */
/* out dim d = in dim perm[d]. */
int omega_tensor_permute(OmegaTensorCtx *ctx, OmegaTensor t, const uint32_t *perm, OmegaTensor *out);
/* Swaps the last two axes (rank >= 2). */
int omega_tensor_transpose(OmegaTensorCtx *ctx, OmegaTensor t, OmegaTensor *out);
/* Per axis: start < stop <= dim, step >= 1; out dim = ceil((stop-start)/step).
 * step may be NULL (all 1). */
int omega_tensor_slice(OmegaTensorCtx *ctx, OmegaTensor t, const uint64_t *start,
                       const uint64_t *stop, const uint64_t *step, OmegaTensor *out);
/* View only if t is row-major contiguous (strides equal the dense row-major
 * strides, ignoring axes of size 1); otherwise OMEGA_TENSOR_ERR_NOT_CONTIGUOUS.
 * Use omega_tensor_contiguous first to get a copy. */
int omega_tensor_reshape(OmegaTensorCtx *ctx, OmegaTensor t, uint32_t rank,
                         const uint64_t *shape, OmegaTensor *out);
/* Broadcast view (numpy rules, see omega_tensor_broadcast_shape): stride 0 on
 * expanded axes. */
int omega_tensor_broadcast_to(OmegaTensorCtx *ctx, OmegaTensor t, uint32_t rank,
                              const uint64_t *shape, OmegaTensor *out);

/* ---- new tensors ---------------------------------------------------------- */
/* Dense row-major copy with its own storage (same value identity). */
int omega_tensor_contiguous(OmegaTensorCtx *ctx, OmegaTensor t, OmegaTensor *out);
/* F32 <-> F16/BF16 via E1 conversion ops; same dtype = contiguous copy;
 * F16 <-> BF16 is refused (go through F32 explicitly). */
int omega_tensor_cast(OmegaTensorCtx *ctx, OmegaTensor t, OmegaDType to, OmegaTensor *out);

/*
 * Broadcasting (numpy rules, stated): align shapes at the trailing axis; the
 * missing leading axes of the shorter shape count as 1; for every axis the
 * two sizes must be equal or one of them must be 1; the result size is the
 * larger. Anything else is OMEGA_TENSOR_ERR_SHAPE.
 */
int omega_tensor_broadcast_shape(uint32_t ra, const uint64_t *sa, uint32_t rb, const uint64_t *sb,
                                 uint32_t *rout, uint64_t *sout);

int omega_tensor_unary(OmegaTensorCtx *ctx, OmegaTensorUnaryOp op, OmegaTensor a, OmegaTensor *out);
/* Constant tensors (answers LT-M21 CR-2). full() writes the F32 bits of
 * `value` exactly into every element: -0.0 and any NaN payload are kept (no
 * canonicalization; value ids still treat all NaNs as one). zeros = +0.0
 * (bits 0x00000000), ones = 1.0f. Shape rules as omega_tensor_from_f32
 * (rank <= 8, dims >= 1, elements <= OMEGA_TENSOR_MAX_ELEMS). */
int omega_tensor_full(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, float value,
                      OmegaTensor *out);
int omega_tensor_zeros(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, OmegaTensor *out);
int omega_tensor_ones(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, OmegaTensor *out);
int omega_tensor_binary(OmegaTensorCtx *ctx, OmegaTensorBinaryOp op, OmegaTensor a, OmegaTensor b,
                        OmegaTensor *out);
/* a*b + c rounded once (E1 FFMA), all three broadcast together. */
int omega_tensor_fma(OmegaTensorCtx *ctx, OmegaTensor a, OmegaTensor b, OmegaTensor c, OmegaTensor *out);

/*
 * CR-3 where: out[i] = cond[i] is +1.0 ? a[i] : b[i], cond, a and b broadcast
 * together (numpy rules above), all F32. The result is a bit copy of the
 * chosen element (no arithmetic: -0.0 and NaN payloads are kept). cond must
 * hold only the bit patterns OMEGA_TENSOR_MASK_TRUE_BITS (+1.0) and
 * OMEGA_TENSOR_MASK_FALSE_BITS (+0.0), as the CMP_* ops produce; any other
 * element (-0.0, NaN, 0.5, 2.0, ...) is refused with OMEGA_TENSOR_ERR_MASK
 * and no tensor is made.
 */
#define OMEGA_TENSOR_MASK_TRUE_BITS  0x3f800000U
#define OMEGA_TENSOR_MASK_FALSE_BITS 0x00000000U
int omega_tensor_where(OmegaTensorCtx *ctx, OmegaTensor cond, OmegaTensor a, OmegaTensor b,
                       OmegaTensor *out);

/* Reduce along one axis. keepdims keeps that axis with size 1. */
int omega_tensor_reduce(OmegaTensorCtx *ctx, OmegaTensorReduceOp op, OmegaTensor t, uint32_t axis,
                        bool keepdims, OmegaTensor *out);

/*
 * General matmul, OMEGA_TENSOR_MATMUL_DECLARED_ORDER, F32 only.
 * Rank 2: [M,K] x [K,N] -> [M,N], any M, N, K >= 1.
 * Rank >= 3 (batched): [...,M,K] x [...,K,N]; the batch axes broadcast with
 * the numpy rules above; both operands must have rank >= 2.
 */
int omega_tensor_matmul(OmegaTensorCtx *ctx, OmegaTensor a, OmegaTensor b, OmegaTensor *out);

/* ---- placement (CR-4): pure bit copies, no arithmetic -------------------- */
/*
 * Embed: the exact inverse placement of omega_tensor_slice. Returns a new
 * dense tensor of (rank, out_shape), dtype of src, every element all-zero
 * bits (+0.0 for F32/F16/BF16), except that src element i (logical row-major
 * multi-index) is copied bit for bit to out position start + i * step, per
 * axis. step may be NULL (all 1). For y = embed(x, start, step) and
 * stop[d] = start[d] + (x.shape[d] - 1) * step[d] + 1,
 * slice(y, start, stop, step) is x bit for bit.
 * Refused (typed, nothing created): rank != src rank or rank 0 -> ERR_RANK;
 * out_shape with a 0 dim -> ERR_SHAPE; result > OMEGA_TENSOR_MAX_ELEMS ->
 * ERR_CAPACITY; a step of 0 (steps are unsigned, so "<= 0" is exactly 0) ->
 * ERR_BAD_ARGS (as in slice); any placed position outside out_shape ->
 * ERR_BOUNDS. Works for every dtype (no arithmetic).
 */
int omega_tensor_embed(OmegaTensorCtx *ctx, OmegaTensor src, uint32_t rank, const uint64_t *out_shape,
                       const uint64_t *start, const uint64_t *step, OmegaTensor *out);
/*
 * Concat: n >= 1 tensors joined along axis, in array order, bit for bit.
 * All parts must have the same dtype (else ERR_DTYPE), the same rank >= 1
 * (else ERR_RANK) and equal sizes on every other axis (else ERR_SHAPE);
 * axis >= rank -> ERR_AXIS; total > OMEGA_TENSOR_MAX_ELEMS -> ERR_CAPACITY;
 * n == 0 or NULL arrays -> ERR_BAD_ARGS. Parts may be views (strided,
 * broadcast). Part k occupies [off_k, off_k + part_k.shape[axis]) on axis,
 * off_k = sum of earlier parts' sizes on axis.
 */
int omega_tensor_concat(OmegaTensorCtx *ctx, uint32_t n, const OmegaTensor *tensors, uint32_t axis,
                        OmegaTensor *out);

/* Live tensor descriptors and live storage slots (lifetime audits). */
void omega_tensor_live_counts(const OmegaTensorCtx *ctx, uint32_t *tensors, uint32_t *storages);

#ifdef OMEGA_TENSOR_TEST_HOOKS
/* Test-only: set the generation of a free storage slot, to exercise the
 * UINT64_MAX retirement rule without 2^64 releases. Refused on a live slot.
 * Compiled only when OMEGA_TENSOR_TEST_HOOKS is defined (the test build in
 * mk/tensor.mk); the default library build does not contain this symbol and
 * test-tensor-no-hooks checks that with nm. */
int omega_tensor_test_set_storage_generation(OmegaTensorCtx *ctx, uint32_t slot, uint64_t gen);
#endif

#endif /* OMEGA_TENSOR_H */
