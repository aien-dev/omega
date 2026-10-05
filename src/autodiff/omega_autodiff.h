#ifndef OMEGA_AUTODIFF_H
#define OMEGA_AUTODIFF_H

/*
 * M21 OMEGA_AUTODIFF, CPU tier (docs/autodiff/M21_OMEGA_AUTODIFF.md).
 *
 * A reverse-mode tape over the M20 OMEGA_TENSOR ops that exist today
 * (src/tensor/omega_tensor.h). Forward: every recorded op is one M20 call
 * whose result is kept on the tape. Backward: walks the tape from the root
 * down to node 0 and builds every gradient with M20 calls (binary ADD / SUB /
 * MUL / DIV, matmul, transpose, reshape, broadcast_to, reduce SUM), so the
 * backward pass runs on the same E1 realization as the forward pass. The one
 * exception is the MAX routing step (see OMEGA_AD_MAX): it is a data move
 * done on the host in the caller's scratch buffer, because M20 has no
 * mask-producing op yet (tensor change request CR-3 in the doc).
 *
 * Determinism. No hidden global state, no heap allocation in this module:
 * the caller provides the node array (explicit capacity), the scratch buffer
 * and the M20 context (whose slot capacity bounds every tensor made here).
 * Backward walks nodes in strictly decreasing index order; for each node the
 * contribution to in0 is accumulated before the contribution to in1; several
 * contributions to one node are added in that order with M20 ADD. Same tape,
 * same inputs, same bits.
 *
 * Ownership. Leaf values belong to the caller and must stay live until
 * omega_ad_tape_release. Every non-leaf value and every gradient is a dense
 * row-major tensor owned by the tape and released by omega_ad_tape_release.
 * Handles returned by omega_ad_value / omega_ad_grad are borrowed.
 *
 * Errors. A forward call that fails leaves the tape unchanged (no node, no
 * leaked tensor). Errors of the M20 layer are passed through unchanged
 * (OMEGA_TENSOR_ERR_*, -1..-11); the tape's own errors are OMEGA_AD_ERR_*.
 * A backward call that fails marks the tape failed (OMEGA_AD_ERR_STATE on
 * every later backward or grad call); release it and rebuild.
 *
 * Error contract (per op, "local"): every vector-Jacobian product below is
 * compared with exact real arithmetic applied to the float32 values that are
 * actually on the tape (stored forward values and the incoming gradient g).
 * u = 2^-24, gamma(k) = k*u / (1 - k*u). L(m) = number of E1 reduction
 * levels for m terms = max(1, ceil(log32 m)); one level is a 5-step
 * pairwise lane tree, so a declared-order sum of m terms has error
 * <= gamma(5*L(m)) * sum|terms| (E1 reduction contract, padding with -0.0
 * is exact). D(x) = sum of 5*L(m) over every reduction "unbroadcast" makes
 * to bring a gradient back to x's shape (m > 1 only; D = 0 when x was not
 * broadcast). Bounds hold when no intermediate overflows or goes subnormal.
 *
 *   op         gradient                         bound per element of grad x
 *   ADD        ga = unb(g),       gb = unb(g)   gamma(D(x)) * sum|g|
 *   SUB        ga = unb(g),       gb = -unb(g)  gamma(D(x)) * sum|g|
 *   MUL        ga = unb(g*b),     gb = unb(g*a) gamma(D(x)+1) * sum|g*other|
 *   DIV        ga = unb(g/b)                    gamma(D(a)+1) * sum|g/b|
 *              gb = -unb(g*(y/b)), y = a/b      gamma(D(b)+3) * sum|g*a/b^2|
 *   MAX        routed g (see OMEGA_AD_MAX)      gamma(D(x)) * sum|routed g|
 *   SQRT       ga = g / (y + y), y = sqrt(a)    gamma(2) * |g / (2 sqrt(a))|
 *   MATMUL     gA = unb(g @ B^T)                gamma(1 + 5*L(N) + D(A)) * sum|g||B|
 *   [..,M,K]x  gB = unb(A^T @ g)                gamma(1 + 5*L(M) + D(B)) * sum|A||g|
 *   [..,K,N]
 *   TRANSPOSE  ga = g^T                          0 (exact)
 *   BROADCAST  ga = unb(g)                       gamma(D(a)) * sum|g|
 *   SUM(axis)  ga = broadcast(g)                 0 (exact)
 *   MEAN(axis) ga = broadcast(g) / n             gamma(1) * |g / n|  (n <= 2^24)
 *   fan-out    k contributions added in order   extra gamma(k-1) * sum|contrib|
 *
 * Not thread safe: one tape per thread, like the M20 context.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_tensor.h"

#define OMEGA_AD_NONE UINT32_MAX

typedef enum {
    OMEGA_AD_OK            = 0,
    OMEGA_AD_ERR_BAD_ARGS  = -101, /* NULL, unknown op, node id out of range  */
    OMEGA_AD_ERR_TAPE_FULL = -102, /* count == capacity: nothing recorded     */
    OMEGA_AD_ERR_SHAPE     = -103, /* seed/root shape, gradient shape drift   */
    OMEGA_AD_ERR_STATE     = -104, /* backward already ran, or failed         */
    OMEGA_AD_ERR_NO_GRAD   = -105, /* node does not require / did not get one */
    OMEGA_AD_ERR_SCRATCH   = -106, /* scratch smaller than omega_ad_scratch_need */
    OMEGA_AD_ERR_COUNT     = -107  /* flatten: element count != buffer count  */
} OmegaAdStatus;

typedef enum {
    OMEGA_AD_LEAF = 0,
    OMEGA_AD_ADD,        /* M20 binary ADD, numpy broadcast                     */
    OMEGA_AD_SUB,        /* M20 binary SUB                                      */
    OMEGA_AD_MUL,        /* M20 binary MUL                                      */
    OMEGA_AD_DIV,        /* M20 binary DIV (correctly rounded)                  */
    /* M20 binary MAX (FMNMX_MAX, NaN is missing data). Subgradient rule:
     * g goes to a when b is NaN and a is not, or when a > b; otherwise to b
     * (ties, including -0 vs +0, and a NaN go to b). relu(x) = MAX(x, 0)
     * therefore has gradient 0 at x == 0. Needs 3 * elements(y) floats of
     * caller scratch at backward time. */
    OMEGA_AD_MAX,
    OMEGA_AD_SQRT,       /* M20 unary SQRT (correctly rounded); grad at 0 = inf */
    OMEGA_AD_MATMUL,     /* M20 matmul, rank >= 2, batched broadcast            */
    OMEGA_AD_TRANSPOSE,  /* swap last two axes; value is a dense copy           */
    OMEGA_AD_BROADCAST,  /* broadcast_to; value is a dense copy                 */
    OMEGA_AD_SUM,        /* M20 reduce SUM along one axis                       */
    OMEGA_AD_MEAN,       /* M20 reduce MEAN along one axis                      */
    OMEGA_AD_OP_COUNT
} OmegaAdOp;

typedef struct {
    OmegaAdOp   op;
    uint32_t    in0, in1;       /* node ids or OMEGA_AD_NONE                   */
    uint32_t    axis;           /* SUM / MEAN                                  */
    bool        keepdims;       /* SUM / MEAN                                  */
    bool        requires_grad;  /* leaf: caller's choice; op: any input's      */
    bool        owns_value;     /* false only for leaves                       */
    bool        has_grad;
    OmegaTensor value;
    OmegaTensor grad;           /* valid when has_grad; dense, owned by tape   */
} OmegaAdNode;

typedef struct {
    OmegaTensorCtx *ctx;
    OmegaAdNode    *nodes;      /* caller array of `capacity` nodes            */
    uint32_t        capacity;
    uint32_t        count;
    float          *scratch;    /* caller buffer, may be NULL if no MAX node   */
    size_t          scratch_count;
    bool            backward_done;
    bool            failed;
} OmegaAdTape;

/* capacity >= 1. scratch may be NULL with scratch_count 0. */
int  omega_ad_tape_init(OmegaAdTape *t, OmegaTensorCtx *ctx, OmegaAdNode *nodes, uint32_t capacity,
                        float *scratch, size_t scratch_count);
/* Releases every tensor the tape owns (values of op nodes, all gradients).
 * Leaves' values stay with the caller. The tape is empty afterwards. */
void omega_ad_tape_release(OmegaAdTape *t);

/* ---- forward recording ----------------------------------------------------- */
/* value must be a live F32 tensor; the tape borrows it. */
int omega_ad_leaf(OmegaAdTape *t, OmegaTensor value, bool requires_grad, uint32_t *id);
/* op in OMEGA_AD_ADD .. OMEGA_AD_MAX. */
int omega_ad_binary(OmegaAdTape *t, OmegaAdOp op, uint32_t a, uint32_t b, uint32_t *id);
int omega_ad_sqrt(OmegaAdTape *t, uint32_t a, uint32_t *id);
int omega_ad_matmul(OmegaAdTape *t, uint32_t a, uint32_t b, uint32_t *id);
int omega_ad_transpose(OmegaAdTape *t, uint32_t a, uint32_t *id);
int omega_ad_broadcast(OmegaAdTape *t, uint32_t a, uint32_t rank, const uint64_t *shape, uint32_t *id);
/* op is OMEGA_AD_SUM or OMEGA_AD_MEAN. */
int omega_ad_reduce(OmegaAdTape *t, OmegaAdOp op, uint32_t a, uint32_t axis, bool keepdims, uint32_t *id);

/* Borrowed handle of a node's forward value. */
int omega_ad_value(const OmegaAdTape *t, uint32_t id, OmegaTensor *out);
/* Floats of scratch the backward pass of this tape needs (MAX nodes). */
int omega_ad_scratch_need(const OmegaAdTape *t, size_t *floats);

/* ---- backward ---------------------------------------------------------------- */
/* Root must have exactly one element; seed gradient 1.0. Runs once per tape. */
int omega_ad_backward(OmegaAdTape *t, uint32_t root);
/* Vector-Jacobian product: seed has exactly the root's shape (copied). */
int omega_ad_backward_seed(OmegaAdTape *t, uint32_t root, OmegaTensor seed);

/* Borrowed handle of a node's gradient (dense row-major, node's shape). */
int omega_ad_grad(const OmegaAdTape *t, uint32_t id, OmegaTensor *out);
/* Copies one gradient out: count must equal the node's element count. */
int omega_ad_grad_read_f32(const OmegaAdTape *t, uint32_t id, float *out, size_t count);
/*
 * M22 bridge: writes the gradients of ids[0..n) back to back, each in
 * logical row-major order, into one flat float32 buffer of exactly `count`
 * elements. This is the layout tg_sgd_step (src/train/tg_sgd.h) consumes when
 * the shadow's parameters are laid out in the same id order. Nothing is
 * written unless the total element count equals `count`.
 */
int omega_ad_grads_flatten(const OmegaAdTape *t, const uint32_t *ids, uint32_t n, float *out,
                           size_t count);

#endif /* OMEGA_AUTODIFF_H */
