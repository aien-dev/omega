/*
 * M21 OMEGA_AUTODIFF, CPU tier: reverse-mode tape over M20 tensor ops.
 * Meaning, determinism rules and the per-op error contract are stated in
 * omega_autodiff.h and docs/autodiff/M21_OMEGA_AUTODIFF.md.
 *
 * Lines marked MUT:<NAME> are the targets of tools/autodiff_mutations.sh.
 * No heap allocation here; no floating-point arithmetic except through M20
 * calls (the MAX routing step only compares and moves values).
 */
#include "omega_autodiff.h"

#include <string.h>

/* ---- small helpers ---------------------------------------------------------- */

static void rel(OmegaAdTape *t, OmegaTensor x) {
    (void)omega_tensor_release(t->ctx, x);
}

static bool node_ok(const OmegaAdTape *t, uint32_t id) {
    return t && id < t->count;
}

static int can_record(const OmegaAdTape *t) {
    if (!t || !t->ctx || !t->nodes) return OMEGA_AD_ERR_BAD_ARGS;
    if (t->backward_done || t->failed) return OMEGA_AD_ERR_STATE;
    if (t->count >= t->capacity) return OMEGA_AD_ERR_TAPE_FULL;
    return OMEGA_AD_OK;
}

static uint32_t push(OmegaAdTape *t, OmegaAdOp op, uint32_t in0, uint32_t in1, uint32_t axis,
                     bool keepdims, bool requires_grad, bool owns, OmegaTensor value) {
    OmegaAdNode *n = &t->nodes[t->count];
    memset(n, 0, sizeof(*n));
    n->op = op;
    n->in0 = in0;
    n->in1 = in1;
    n->axis = axis;
    n->keepdims = keepdims;
    n->requires_grad = requires_grad;
    n->owns_value = owns;
    n->has_grad = false;
    n->value = value;
    return t->count++;
}

static bool same_shape(const OmegaTensorInfo *x, const OmegaTensorInfo *y) {
    if (x->rank != y->rank) return false;
    for (uint32_t d = 0; d < x->rank; d++)
        if (x->shape[d] != y->shape[d]) return false;
    return true;
}

/* Rank-0 F32 constant. */
static int scalar(OmegaAdTape *t, float v, OmegaTensor *out) {
    uint64_t dummy = 1;
    return omega_tensor_from_f32(t->ctx, 0, &dummy, &v, out);
}

/* out = x * k (x borrowed). With k = -1 this is an exact negation. */
static int scale(OmegaAdTape *t, OmegaTensor x, float k, OmegaTensor *out) {
    OmegaTensor c;
    int rc = scalar(t, k, &c);
    if (rc) return rc;
    rc = omega_tensor_binary(t->ctx, OMEGA_TB_MUL, x, c, out);
    rel(t, c);
    return rc;
}

/*
 * Sum a gradient (owned, consumed on every path) back to the shape of
 * `target`: first the extra leading axes (reduce SUM over axis 0, one at a
 * time), then every axis where target has size 1 and the gradient does not
 * (reduce SUM with keepdims). Returns a dense owned tensor in *out.
 */
static int unbroadcast(OmegaAdTape *t, OmegaTensor full, const OmegaTensorInfo *target, OmegaTensor *out) {
    OmegaTensor cur = full;
    OmegaTensorInfo ci;
    int rc = omega_tensor_info(t->ctx, cur, &ci);
    if (rc) { rel(t, cur); return rc; }
    if (ci.rank < target->rank) { rel(t, cur); return OMEGA_AD_ERR_SHAPE; }
    while (ci.rank > target->rank) {
        OmegaTensor nx;
        rc = omega_tensor_reduce(t->ctx, OMEGA_TR_SUM, cur, 0, false, &nx);
        rel(t, cur);
        if (rc) return rc;
        cur = nx;
        rc = omega_tensor_info(t->ctx, cur, &ci);
        if (rc) { rel(t, cur); return rc; }
    }
    for (uint32_t d = 0; d < target->rank; d++) {
        if (target->shape[d] == 1 && ci.shape[d] != 1) { /* MUT:UNBROADCAST */
            OmegaTensor nx;
            rc = omega_tensor_reduce(t->ctx, OMEGA_TR_SUM, cur, d, true, &nx);
            rel(t, cur);
            if (rc) return rc;
            cur = nx;
            rc = omega_tensor_info(t->ctx, cur, &ci);
            if (rc) { rel(t, cur); return rc; }
        }
    }
    *out = cur;
    return OMEGA_AD_OK;
}

/* Add an owned, already shape-matched contribution into node idx's grad. */
static int accumulate(OmegaAdTape *t, uint32_t idx, OmegaTensor contrib) {
    OmegaAdNode *n = &t->nodes[idx];
    OmegaTensorInfo vi, gi;
    int rc = omega_tensor_info(t->ctx, n->value, &vi);
    if (!rc) rc = omega_tensor_info(t->ctx, contrib, &gi);
    if (rc) { rel(t, contrib); return rc; }
    if (!same_shape(&vi, &gi)) { rel(t, contrib); return OMEGA_AD_ERR_SHAPE; }
    if (!n->has_grad) {
        n->grad = contrib;
        n->has_grad = true;
        return OMEGA_AD_OK;
    }
    OmegaTensor sum;
    rc = omega_tensor_binary(t->ctx, OMEGA_TB_ADD, n->grad, contrib, &sum); /* MUT:ACCUMULATE */
    rel(t, contrib);
    if (rc) return rc;
    rel(t, n->grad);
    n->grad = sum;
    return OMEGA_AD_OK;
}

/* Give node idx the contribution `full` (owned, consumed): unbroadcast to the
 * node's shape, then accumulate. No-op (but still consumes) when the node
 * does not require a gradient. */
static int contribute(OmegaAdTape *t, uint32_t idx, OmegaTensor full) {
    OmegaAdNode *n = &t->nodes[idx];
    if (!n->requires_grad) { rel(t, full); return OMEGA_AD_OK; }
    OmegaTensorInfo vi;
    int rc = omega_tensor_info(t->ctx, n->value, &vi);
    if (rc) { rel(t, full); return rc; }
    OmegaTensor u;
    rc = unbroadcast(t, full, &vi, &u);
    if (rc) return rc;
    return accumulate(t, idx, u);
}

static bool wants(const OmegaAdTape *t, uint32_t idx) {
    return idx != OMEGA_AD_NONE && t->nodes[idx].requires_grad;
}

/* MAX subgradient rule (header): true when g goes to a. */
static bool max_routes_to_a(float a, float b) {
    if (a != a) return false;
    if (b != b) return true;
    return a > b; /* MUT:MAX_ROUTE MUT:MAX_TIE */
}

/* ---- per-op backward ------------------------------------------------------------ */

static int backward_max(OmegaAdTape *t, const OmegaAdNode *n) {
    OmegaTensorInfo yi;
    int rc = omega_tensor_info(t->ctx, n->value, &yi);
    if (rc) return rc;
    size_t m = (size_t)yi.elements;
    if (!t->scratch || t->scratch_count / 3 < m) return OMEGA_AD_ERR_SCRATCH;
    float *sa = t->scratch, *sb = t->scratch + m, *sg = t->scratch + 2 * m;
    const uint32_t ins[2] = { n->in0, n->in1 };
    float *dst[2] = { sa, sb };
    for (int k = 0; k < 2; k++) {
        OmegaTensor v;
        rc = omega_tensor_broadcast_to(t->ctx, t->nodes[ins[k]].value, yi.rank, yi.shape, &v);
        if (rc) return rc;
        rc = omega_tensor_read_f32(t->ctx, v, dst[k], m);
        rel(t, v);
        if (rc) return rc;
    }
    rc = omega_tensor_read_f32(t->ctx, n->grad, sg, m);
    if (rc) return rc;
    for (size_t i = 0; i < m; i++) {
        bool to_a = max_routes_to_a(sa[i], sb[i]);
        float g = sg[i];
        sa[i] = to_a ? g : 0.0f;
        sb[i] = to_a ? 0.0f : g;
    }
    for (int k = 0; k < 2; k++) {
        if (!wants(t, ins[k])) continue;
        OmegaTensor full;
        rc = omega_tensor_from_f32(t->ctx, yi.rank, yi.shape, dst[k], &full);
        if (rc) return rc;
        rc = contribute(t, ins[k], full);
        if (rc) return rc;
    }
    return OMEGA_AD_OK;
}

/* Gradient of a SUM/MEAN input: g put back on the reduced axis and broadcast
 * to the input's shape (dense owned copy in *out). */
static int expand_reduced(OmegaAdTape *t, const OmegaAdNode *n, OmegaTensor *out, uint64_t *axis_len) {
    OmegaTensorInfo ai, gi;
    int rc = omega_tensor_info(t->ctx, t->nodes[n->in0].value, &ai);
    if (!rc) rc = omega_tensor_info(t->ctx, n->grad, &gi);
    if (rc) return rc;
    uint64_t kshape[OMEGA_TENSOR_MAX_RANK];
    for (uint32_t d = 0; d < ai.rank; d++) kshape[d] = (d == n->axis) ? 1 : ai.shape[d];
    OmegaTensor r, b;
    rc = omega_tensor_reshape(t->ctx, n->grad, ai.rank, kshape, &r);
    if (rc) return rc;
    rc = omega_tensor_broadcast_to(t->ctx, r, ai.rank, ai.shape, &b);
    if (rc) { rel(t, r); return rc; }
    rc = omega_tensor_contiguous(t->ctx, b, out);
    rel(t, b);
    rel(t, r);
    *axis_len = ai.shape[n->axis];
    return rc;
}

static int node_backward(OmegaAdTape *t, const OmegaAdNode *n) {
    const OmegaTensor g = n->grad;
    OmegaTensor c = { 0, 0 }, c2 = { 0, 0 }, tmp = { 0, 0 };
    int rc = OMEGA_AD_OK;
    switch (n->op) {
    case OMEGA_AD_LEAF:
        return OMEGA_AD_OK;
    case OMEGA_AD_ADD:
    case OMEGA_AD_SUB:
        if (wants(t, n->in0)) {
            rc = omega_tensor_contiguous(t->ctx, g, &c);
            if (!rc) rc = contribute(t, n->in0, c);
            if (rc) return rc;
        }
        if (wants(t, n->in1)) {
            if (n->op == OMEGA_AD_ADD) rc = omega_tensor_contiguous(t->ctx, g, &c);
            else rc = scale(t, g, -1.0f, &c); /* MUT:SUB_NEGATE */
            if (!rc) rc = contribute(t, n->in1, c);
        }
        return rc;
    case OMEGA_AD_MUL: {
        if (wants(t, n->in0)) {
            rc = omega_tensor_binary(t->ctx, OMEGA_TB_MUL, g, t->nodes[n->in1].value, &c); /* MUT:MUL_OPERAND */
            if (!rc) rc = contribute(t, n->in0, c);
            if (rc) return rc;
        }
        if (wants(t, n->in1)) {
            rc = omega_tensor_binary(t->ctx, OMEGA_TB_MUL, g, t->nodes[n->in0].value, &c);
            if (!rc) rc = contribute(t, n->in1, c);
        }
        return rc;
    }
    case OMEGA_AD_DIV: {
        const OmegaAdNode *nb = &t->nodes[n->in1];
        if (wants(t, n->in0)) {
            rc = omega_tensor_binary(t->ctx, OMEGA_TB_DIV, g, nb->value, &c);
            if (!rc) rc = contribute(t, n->in0, c);
            if (rc) return rc;
        }
        if (wants(t, n->in1)) {
            /* gb = -(g * (y / b)) */
            rc = omega_tensor_binary(t->ctx, OMEGA_TB_DIV, n->value, nb->value, &tmp); /* MUT:DIV_FORMULA */
            if (rc) return rc;
            rc = omega_tensor_binary(t->ctx, OMEGA_TB_MUL, g, tmp, &c2); /* MUT:DIV_FORMULA */
            rel(t, tmp);
            if (rc) return rc;
            rc = scale(t, c2, -1.0f, &c); /* MUT:DIV_B_SIGN */
            rel(t, c2);
            if (!rc) rc = contribute(t, n->in1, c);
        }
        return rc;
    }
    case OMEGA_AD_MAX:
        return backward_max(t, n);
    case OMEGA_AD_SQRT:
        if (!wants(t, n->in0)) return OMEGA_AD_OK;
        rc = omega_tensor_binary(t->ctx, OMEGA_TB_ADD, n->value, n->value, &tmp); /* MUT:SQRT_TWICE */
        if (rc) return rc;
        rc = omega_tensor_binary(t->ctx, OMEGA_TB_DIV, g, tmp, &c);
        rel(t, tmp);
        if (!rc) rc = contribute(t, n->in0, c);
        return rc;
    case OMEGA_AD_MATMUL: {
        const OmegaAdNode *na = &t->nodes[n->in0], *nb = &t->nodes[n->in1];
        if (wants(t, n->in0)) {
            /* gA = g @ B^T */
            rc = omega_tensor_transpose(t->ctx, nb->value, &tmp);
            if (rc) return rc;
            rc = omega_tensor_matmul(t->ctx, g, tmp, &c); /* MUT:MATMUL_TRANSPOSE */
            rel(t, tmp);
            if (!rc) rc = contribute(t, n->in0, c);
            if (rc) return rc;
        }
        if (wants(t, n->in1)) {
            /* gB = A^T @ g */
            rc = omega_tensor_transpose(t->ctx, na->value, &tmp);
            if (rc) return rc;
            rc = omega_tensor_matmul(t->ctx, tmp, g, &c);
            rel(t, tmp);
            if (!rc) rc = contribute(t, n->in1, c);
        }
        return rc;
    }
    case OMEGA_AD_TRANSPOSE:
        if (!wants(t, n->in0)) return OMEGA_AD_OK;
        rc = omega_tensor_transpose(t->ctx, g, &tmp);
        if (rc) return rc;
        rc = omega_tensor_contiguous(t->ctx, tmp, &c);
        rel(t, tmp);
        if (!rc) rc = contribute(t, n->in0, c);
        return rc;
    case OMEGA_AD_BROADCAST:
        if (!wants(t, n->in0)) return OMEGA_AD_OK;
        rc = omega_tensor_contiguous(t->ctx, g, &c);
        if (!rc) rc = contribute(t, n->in0, c);
        return rc;
    case OMEGA_AD_SUM:
    case OMEGA_AD_MEAN: {
        if (!wants(t, n->in0)) return OMEGA_AD_OK;
        uint64_t len = 0;
        rc = expand_reduced(t, n, &c2, &len);
        if (rc) return rc;
        if (n->op == OMEGA_AD_SUM) return contribute(t, n->in0, c2);
        OmegaTensor nt;
        rc = scalar(t, (float)len, &nt); /* MUT:MEAN_DIVISOR */
        if (rc) { rel(t, c2); return rc; }
        rc = omega_tensor_binary(t->ctx, OMEGA_TB_DIV, c2, nt, &c);
        rel(t, nt);
        rel(t, c2);
        if (!rc) rc = contribute(t, n->in0, c);
        return rc;
    }
    default:
        return OMEGA_AD_ERR_BAD_ARGS;
    }
}

/* ---- public API ------------------------------------------------------------------ */

int omega_ad_tape_init(OmegaAdTape *t, OmegaTensorCtx *ctx, OmegaAdNode *nodes, uint32_t capacity,
                       float *scratch, size_t scratch_count) {
    if (!t || !ctx || !nodes || capacity == 0 || capacity == OMEGA_AD_NONE ||
        (!scratch && scratch_count)) return OMEGA_AD_ERR_BAD_ARGS;
    memset(t, 0, sizeof(*t));
    t->ctx = ctx;
    t->nodes = nodes;
    t->capacity = capacity;
    t->scratch = scratch;
    t->scratch_count = scratch_count;
    return OMEGA_AD_OK;
}

void omega_ad_tape_release(OmegaAdTape *t) {
    if (!t || !t->nodes || !t->ctx) return;
    for (uint32_t i = t->count; i-- > 0;) {
        OmegaAdNode *n = &t->nodes[i];
        if (n->has_grad) rel(t, n->grad);
        if (n->owns_value) rel(t, n->value);
        n->has_grad = false;
        n->owns_value = false;
    }
    t->count = 0;
    t->backward_done = false;
    t->failed = false;
}

int omega_ad_leaf(OmegaAdTape *t, OmegaTensor value, bool requires_grad, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensorInfo vi;
    rc = omega_tensor_info(t->ctx, value, &vi);
    if (rc) return rc;
    if (vi.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    *id = push(t, OMEGA_AD_LEAF, OMEGA_AD_NONE, OMEGA_AD_NONE, 0, false, requires_grad, false, value);
    return OMEGA_AD_OK;
}

static int record_op(OmegaAdTape *t, OmegaAdOp op, uint32_t a, uint32_t b, uint32_t axis,
                     bool keepdims, OmegaTensor value, uint32_t *id) {
    bool rg = t->nodes[a].requires_grad || (b != OMEGA_AD_NONE && t->nodes[b].requires_grad);
    *id = push(t, op, a, b, axis, keepdims, rg, true, value);
    return OMEGA_AD_OK;
}

int omega_ad_binary(OmegaAdTape *t, OmegaAdOp op, uint32_t a, uint32_t b, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id || !node_ok(t, a) || !node_ok(t, b)) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensorBinaryOp bop;
    switch (op) {
    case OMEGA_AD_ADD: bop = OMEGA_TB_ADD; break;
    case OMEGA_AD_SUB: bop = OMEGA_TB_SUB; break;
    case OMEGA_AD_MUL: bop = OMEGA_TB_MUL; break;
    case OMEGA_AD_DIV: bop = OMEGA_TB_DIV; break;
    case OMEGA_AD_MAX: bop = OMEGA_TB_MAX; break;
    default: return OMEGA_AD_ERR_BAD_ARGS;
    }
    OmegaTensor y;
    rc = omega_tensor_binary(t->ctx, bop, t->nodes[a].value, t->nodes[b].value, &y);
    if (rc) return rc;
    return record_op(t, op, a, b, 0, false, y, id);
}

int omega_ad_sqrt(OmegaAdTape *t, uint32_t a, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id || !node_ok(t, a)) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensor y;
    rc = omega_tensor_unary(t->ctx, OMEGA_TU_SQRT, t->nodes[a].value, &y);
    if (rc) return rc;
    return record_op(t, OMEGA_AD_SQRT, a, OMEGA_AD_NONE, 0, false, y, id);
}

int omega_ad_matmul(OmegaAdTape *t, uint32_t a, uint32_t b, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id || !node_ok(t, a) || !node_ok(t, b)) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensor y;
    rc = omega_tensor_matmul(t->ctx, t->nodes[a].value, t->nodes[b].value, &y);
    if (rc) return rc;
    return record_op(t, OMEGA_AD_MATMUL, a, b, 0, false, y, id);
}

int omega_ad_transpose(OmegaAdTape *t, uint32_t a, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id || !node_ok(t, a)) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensor v, y;
    rc = omega_tensor_transpose(t->ctx, t->nodes[a].value, &v);
    if (rc) return rc;
    rc = omega_tensor_contiguous(t->ctx, v, &y);
    rel(t, v);
    if (rc) return rc;
    return record_op(t, OMEGA_AD_TRANSPOSE, a, OMEGA_AD_NONE, 0, false, y, id);
}

int omega_ad_broadcast(OmegaAdTape *t, uint32_t a, uint32_t rank, const uint64_t *shape, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id || !node_ok(t, a)) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensor v, y;
    rc = omega_tensor_broadcast_to(t->ctx, t->nodes[a].value, rank, shape, &v);
    if (rc) return rc;
    rc = omega_tensor_contiguous(t->ctx, v, &y);
    rel(t, v);
    if (rc) return rc;
    return record_op(t, OMEGA_AD_BROADCAST, a, OMEGA_AD_NONE, 0, false, y, id);
}

int omega_ad_reduce(OmegaAdTape *t, OmegaAdOp op, uint32_t a, uint32_t axis, bool keepdims, uint32_t *id) {
    int rc = can_record(t);
    if (rc) return rc;
    if (!id || !node_ok(t, a)) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensorReduceOp rop;
    if (op == OMEGA_AD_SUM) rop = OMEGA_TR_SUM;
    else if (op == OMEGA_AD_MEAN) rop = OMEGA_TR_MEAN;
    else return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensor y;
    rc = omega_tensor_reduce(t->ctx, rop, t->nodes[a].value, axis, keepdims, &y);
    if (rc) return rc;
    return record_op(t, op, a, OMEGA_AD_NONE, axis, keepdims, y, id);
}

int omega_ad_value(const OmegaAdTape *t, uint32_t id, OmegaTensor *out) {
    if (!out || !node_ok(t, id)) return OMEGA_AD_ERR_BAD_ARGS;
    *out = t->nodes[id].value;
    return OMEGA_AD_OK;
}

int omega_ad_scratch_need(const OmegaAdTape *t, size_t *floats) {
    if (!t || !t->nodes || !floats) return OMEGA_AD_ERR_BAD_ARGS;
    size_t need = 0;
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->nodes[i].op != OMEGA_AD_MAX) continue;
        OmegaTensorInfo yi;
        int rc = omega_tensor_info(t->ctx, t->nodes[i].value, &yi);
        if (rc) return rc;
        if (yi.elements > SIZE_MAX / 3) return OMEGA_TENSOR_ERR_CAPACITY;
        if ((size_t)yi.elements * 3 > need) need = (size_t)yi.elements * 3;
    }
    *floats = need;
    return OMEGA_AD_OK;
}

/* Runs the walk once the root's seed gradient is in place. */
static int run_backward(OmegaAdTape *t, uint32_t root) {
    for (uint32_t i = root + 1; i-- > 0;) { /* MUT:TAPE_WALK */
        const OmegaAdNode *n = &t->nodes[i];
        if (!n->has_grad || n->op == OMEGA_AD_LEAF) continue;
        int rc = node_backward(t, n);
        if (rc) return rc;
    }
    return OMEGA_AD_OK;
}

static int begin_backward(OmegaAdTape *t, uint32_t root, OmegaTensorInfo *ri) {
    if (!t || !t->ctx || !t->nodes) return OMEGA_AD_ERR_BAD_ARGS;
    if (t->backward_done || t->failed) return OMEGA_AD_ERR_STATE;
    if (!node_ok(t, root)) return OMEGA_AD_ERR_BAD_ARGS;
    if (!t->nodes[root].requires_grad) return OMEGA_AD_ERR_NO_GRAD;
    size_t need = 0;
    int rc = omega_ad_scratch_need(t, &need);
    if (rc) return rc;
    if (need > t->scratch_count) return OMEGA_AD_ERR_SCRATCH;
    return omega_tensor_info(t->ctx, t->nodes[root].value, ri);
}

static int finish_backward(OmegaAdTape *t, uint32_t root, OmegaTensor seed_owned) {
    OmegaAdNode *r = &t->nodes[root];
    r->grad = seed_owned;
    r->has_grad = true;
    t->backward_done = true;
    int rc = run_backward(t, root);
    if (rc) t->failed = true;
    return rc;
}

int omega_ad_backward(OmegaAdTape *t, uint32_t root) {
    OmegaTensorInfo ri;
    int rc = begin_backward(t, root, &ri);
    if (rc) return rc;
    if (ri.elements != 1) return OMEGA_AD_ERR_SHAPE;
    const float one = 1.0f;
    OmegaTensor seed;
    rc = omega_tensor_from_f32(t->ctx, ri.rank, ri.shape, &one, &seed);
    if (rc) return rc;
    return finish_backward(t, root, seed);
}

int omega_ad_backward_seed(OmegaAdTape *t, uint32_t root, OmegaTensor seed) {
    OmegaTensorInfo ri, si;
    int rc = begin_backward(t, root, &ri);
    if (rc) return rc;
    rc = omega_tensor_info(t->ctx, seed, &si);
    if (rc) return rc;
    if (si.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    if (!same_shape(&ri, &si)) return OMEGA_AD_ERR_SHAPE;
    OmegaTensor s;
    rc = omega_tensor_contiguous(t->ctx, seed, &s);
    if (rc) return rc;
    return finish_backward(t, root, s);
}

int omega_ad_grad(const OmegaAdTape *t, uint32_t id, OmegaTensor *out) {
    if (!out || !node_ok(t, id)) return OMEGA_AD_ERR_BAD_ARGS;
    if (t->failed) return OMEGA_AD_ERR_STATE;
    if (!t->nodes[id].has_grad) return OMEGA_AD_ERR_NO_GRAD;
    *out = t->nodes[id].grad;
    return OMEGA_AD_OK;
}

int omega_ad_grad_read_f32(const OmegaAdTape *t, uint32_t id, float *out, size_t count) {
    OmegaTensor g;
    int rc = omega_ad_grad(t, id, &g);
    if (rc) return rc;
    if (!out) return OMEGA_AD_ERR_BAD_ARGS;
    OmegaTensorInfo gi;
    rc = omega_tensor_info(t->ctx, g, &gi);
    if (rc) return rc;
    if (gi.elements != count) return OMEGA_AD_ERR_COUNT;
    return omega_tensor_read_f32(t->ctx, g, out, count);
}

int omega_ad_grads_flatten(const OmegaAdTape *t, const uint32_t *ids, uint32_t n, float *out,
                           size_t count) {
    if (!t || !ids || !out || n == 0) return OMEGA_AD_ERR_BAD_ARGS;
    size_t total = 0;
    for (uint32_t k = 0; k < n; k++) {
        OmegaTensor g;
        OmegaTensorInfo gi;
        int rc = omega_ad_grad(t, ids[k], &g);
        if (!rc) rc = omega_tensor_info(t->ctx, g, &gi);
        if (rc) return rc;
        if (gi.elements > count - total) return OMEGA_AD_ERR_COUNT;
        total += (size_t)gi.elements;
    }
    if (total != count) return OMEGA_AD_ERR_COUNT;
    size_t off = 0;
    for (uint32_t k = 0; k < n; k++) {
        OmegaTensor g;
        OmegaTensorInfo gi;
        int rc = omega_ad_grad(t, ids[k], &g);
        if (!rc) rc = omega_tensor_info(t->ctx, g, &gi);
        if (!rc) rc = omega_tensor_read_f32(t->ctx, g, out + off, (size_t)gi.elements);
        if (rc) return rc;
        off += (size_t)gi.elements;
    }
    return OMEGA_AD_OK;
}
