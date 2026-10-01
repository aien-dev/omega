/*
 * M20 OMEGA_TENSOR semantic layer (see omega_tensor.h). This file holds the
 * meaning: shapes, strides, views, storage lifetime, identity, and the
 * definition of each op as index movement plus calls into the realization.
 * It performs no floating-point arithmetic of its own.
 */
#include "omega_tensor.h"

#include <stdlib.h>
#include <string.h>

#include "sha256.h"

typedef struct {
    uint64_t   gen;
    bool       live;
    bool       retired;   /* generation reached UINT64_MAX: never reused */
    OmegaDType dtype;
    uint64_t   elems;
    void      *data;
} StorageSlot;

typedef struct {
    uint64_t        gen;
    bool            live;
    bool            retired;
    bool            owns_storage;
    OmegaTensorInfo info;
} TensorSlot;

struct OmegaTensorCtx {
    uint32_t                      cap;
    StorageSlot                  *st;
    TensorSlot                   *ts;
    const OmegaTensorRealization *real;
    int                           last_numeric;
};

size_t omega_dtype_size(OmegaDType dt) {
    switch (dt) {
    case OMEGA_DT_F32:  return 4;
    case OMEGA_DT_F16:
    case OMEGA_DT_BF16: return 2;
    default:            return 0;
    }
}

/* ---- generations ------------------------------------------------------- */

static void bump_generation(uint64_t *gen, bool *retired) {
    if (*gen == UINT64_MAX) *retired = true;  /* never wrap */
    else (*gen)++;                            /* MUT:RELEASE_BUMP */
}

int omega_tensor_ctx_create(uint32_t capacity, const OmegaTensorRealization *real,
                            OmegaTensorCtx **out) {
    if (!out || capacity == 0 || !real) return OMEGA_TENSOR_ERR_BAD_ARGS;
    *out = NULL;
    if (!real->name || !real->elementwise || !real->reduce || !real->reduce_order ||
        strcmp(real->reduce_order, OMEGA_TENSOR_REDUCE_DECLARED_ORDER) != 0)
        return OMEGA_TENSOR_ERR_REALIZATION;
    OmegaTensorCtx *c = calloc(1, sizeof(*c));
    if (!c) return OMEGA_TENSOR_ERR_CAPACITY;
    c->st = calloc(capacity, sizeof(StorageSlot));
    c->ts = calloc(capacity, sizeof(TensorSlot));
    if (!c->st || !c->ts) { free(c->st); free(c->ts); free(c); return OMEGA_TENSOR_ERR_CAPACITY; }
    for (uint32_t i = 0; i < capacity; i++) { c->st[i].gen = 1; c->ts[i].gen = 1; }
    c->cap = capacity;
    c->real = real;
    *out = c;
    return OMEGA_TENSOR_OK;
}

void omega_tensor_ctx_destroy(OmegaTensorCtx *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->cap; i++) free(ctx->st[i].data);
    free(ctx->st);
    free(ctx->ts);
    free(ctx);
}

int omega_tensor_last_numeric_error(const OmegaTensorCtx *ctx) { return ctx ? ctx->last_numeric : 0; }

#ifdef OMEGA_TENSOR_TEST_HOOKS /* test build only, see omega_tensor.h */
int omega_tensor_test_set_storage_generation(OmegaTensorCtx *ctx, uint32_t slot, uint64_t gen) {
    if (!ctx || slot >= ctx->cap || gen == 0) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (ctx->st[slot].live || ctx->st[slot].retired) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (gen <= ctx->st[slot].gen) return OMEGA_TENSOR_ERR_BAD_ARGS;  /* forward only: a rollback would revive stale handles (Codex) */
    ctx->st[slot].gen = gen;
    return OMEGA_TENSOR_OK;
}
#endif

static StorageSlot *storage_get(const OmegaTensorCtx *ctx, OmegaStorageHandle h) {
    if (h.slot >= ctx->cap) return NULL;
    StorageSlot *s = &ctx->st[h.slot];
    if (!s->live || s->gen != h.generation) return NULL; /* MUT:STORAGE_GEN_CHECK */
    return s;
}

bool omega_tensor_storage_valid(const OmegaTensorCtx *ctx, OmegaStorageHandle s) {
    return ctx && storage_get(ctx, s) != NULL;
}

/* Tensor descriptor lookup; also requires its storage to be valid. */
static int tensor_get(const OmegaTensorCtx *ctx, OmegaTensor t, TensorSlot **ts, StorageSlot **st) {
    if (!ctx) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (t.slot >= ctx->cap) return OMEGA_TENSOR_ERR_STALE;
    TensorSlot *x = &ctx->ts[t.slot];
    if (!x->live || x->gen != t.generation) return OMEGA_TENSOR_ERR_STALE;
    StorageSlot *s = storage_get(ctx, x->info.storage);
    if (!s) return OMEGA_TENSOR_ERR_STALE;
    if (ts) *ts = x;
    if (st) *st = s;
    return OMEGA_TENSOR_OK;
}

static int storage_alloc(OmegaTensorCtx *ctx, OmegaDType dt, uint64_t elems, OmegaStorageHandle *h) {
    size_t es = omega_dtype_size(dt);
    if (!es || elems == 0 || elems > OMEGA_TENSOR_MAX_ELEMS) return OMEGA_TENSOR_ERR_CAPACITY;
    for (uint32_t i = 0; i < ctx->cap; i++) {
        StorageSlot *s = &ctx->st[i];
        if (s->live || s->retired) continue;
        void *d = calloc((size_t)elems, es);
        if (!d) return OMEGA_TENSOR_ERR_CAPACITY;
        s->live = true;
        s->dtype = dt;
        s->elems = elems;
        s->data = d;
        h->slot = i;
        h->generation = s->gen;
        return OMEGA_TENSOR_OK;
    }
    return OMEGA_TENSOR_ERR_CAPACITY;
}

static void storage_release(OmegaTensorCtx *ctx, OmegaStorageHandle h) {
    StorageSlot *s = storage_get(ctx, h);
    if (!s) return;
    free(s->data);
    s->data = NULL;
    s->live = false;
    bump_generation(&s->gen, &s->retired);
}

static int tensor_slot_alloc(OmegaTensorCtx *ctx, const OmegaTensorInfo *info, bool owns, OmegaTensor *out) {
    for (uint32_t i = 0; i < ctx->cap; i++) {
        TensorSlot *x = &ctx->ts[i];
        if (x->live || x->retired) continue;
        x->live = true;
        x->owns_storage = owns;
        x->info = *info;
        out->slot = i;
        out->generation = x->gen;
        return OMEGA_TENSOR_OK;
    }
    return OMEGA_TENSOR_ERR_CAPACITY;
}

int omega_tensor_release(OmegaTensorCtx *ctx, OmegaTensor t) {
    if (!ctx) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (t.slot >= ctx->cap) return OMEGA_TENSOR_ERR_STALE;
    TensorSlot *x = &ctx->ts[t.slot];
    if (!x->live || x->gen != t.generation) return OMEGA_TENSOR_ERR_STALE;
    if (x->owns_storage) storage_release(ctx, x->info.storage);
    x->live = false;
    bump_generation(&x->gen, &x->retired);
    return OMEGA_TENSOR_OK;
}

/* ---- shapes ------------------------------------------------------------- */

static int shape_check(uint32_t rank, const uint64_t *shape, uint64_t *elems) {
    if (rank > OMEGA_TENSOR_MAX_RANK) return OMEGA_TENSOR_ERR_RANK;
    if (rank && !shape) return OMEGA_TENSOR_ERR_BAD_ARGS;
    uint64_t n = 1;
    for (uint32_t d = 0; d < rank; d++) {
        if (shape[d] == 0) return OMEGA_TENSOR_ERR_SHAPE;
        if (shape[d] > OMEGA_TENSOR_MAX_ELEMS || __builtin_mul_overflow(n, shape[d], &n) ||
            n > OMEGA_TENSOR_MAX_ELEMS)
            return OMEGA_TENSOR_ERR_CAPACITY;
    }
    *elems = n;
    return OMEGA_TENSOR_OK;
}

static void dense_strides(uint32_t rank, const uint64_t *shape, uint64_t *strides) {
    uint64_t s = 1;
    for (uint32_t d = rank; d-- > 0;) {
        strides[d] = s;
        s *= shape[d];
    }
}

/* Largest storage index a descriptor reaches must lie inside the storage. */
static int bounds_check(const OmegaTensorInfo *in, uint64_t storage_elems) {
    uint64_t hi = in->offset;
    for (uint32_t d = 0; d < in->rank; d++) {
        uint64_t step;
        if (__builtin_mul_overflow(in->shape[d] - 1, in->strides[d], &step) ||
            __builtin_add_overflow(hi, step, &hi))
            return OMEGA_TENSOR_ERR_BOUNDS;
    }
    return hi < storage_elems ? OMEGA_TENSOR_OK : OMEGA_TENSOR_ERR_BOUNDS;
}

int omega_tensor_broadcast_shape(uint32_t ra, const uint64_t *sa, uint32_t rb, const uint64_t *sb,
                                 uint32_t *rout, uint64_t *sout) {
    if (!rout || !sout || (ra && !sa) || (rb && !sb)) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (ra > OMEGA_TENSOR_MAX_RANK || rb > OMEGA_TENSOR_MAX_RANK) return OMEGA_TENSOR_ERR_RANK;
    uint32_t r = ra > rb ? ra : rb;
    uint64_t tmp[OMEGA_TENSOR_MAX_RANK];
    for (uint32_t i = 0; i < r; i++) {   /* i counts from the trailing axis */
        uint64_t da = i < ra ? sa[ra - 1 - i] : 1;
        uint64_t db = i < rb ? sb[rb - 1 - i] : 1;
        if (da == 0 || db == 0) return OMEGA_TENSOR_ERR_SHAPE;
        if (da != db && da != 1 && db != 1) return OMEGA_TENSOR_ERR_SHAPE;
        tmp[r - 1 - i] = da > db ? da : db;
    }
    memcpy(sout, tmp, r * sizeof(uint64_t));
    *rout = r;
    return OMEGA_TENSOR_OK;
}

/* Directional broadcast of a descriptor to (rank, shape): stride 0 on every
 * expanded axis. Pure descriptor arithmetic. */
static int broadcast_info(const OmegaTensorInfo *in, uint32_t rank, const uint64_t *shape,
                          OmegaTensorInfo *out) {
    if (rank < in->rank) return OMEGA_TENSOR_ERR_SHAPE;
    uint64_t n;
    int rc = shape_check(rank, shape, &n);
    if (rc) return rc;
    OmegaTensorInfo o = *in;
    o.rank = rank;
    for (uint32_t i = 0; i < rank; i++) {
        uint32_t d = rank - 1 - i;
        if (i < in->rank) {
            uint32_t sd = in->rank - 1 - i;
            if (in->shape[sd] == shape[d]) { o.shape[d] = shape[d]; o.strides[d] = in->strides[sd]; }
            else if (in->shape[sd] == 1)   { o.shape[d] = shape[d]; o.strides[d] = 0; } /* MUT:BCAST_ZERO_STRIDE */
            else return OMEGA_TENSOR_ERR_SHAPE;
        } else {
            o.shape[d] = shape[d];
            o.strides[d] = 0;
        }
    }
    for (uint32_t d = rank; d < OMEGA_TENSOR_MAX_RANK; d++) { o.shape[d] = 0; o.strides[d] = 0; }
    o.elements = n;
    *out = o;
    return OMEGA_TENSOR_OK;
}

/* Copy every element in logical row-major order into dst (element size es). */
static void gather(const StorageSlot *s, const OmegaTensorInfo *in, void *dst) {
    size_t es = omega_dtype_size(in->dtype);
    uint64_t idx[OMEGA_TENSOR_MAX_RANK] = {0};
    uint64_t pos = in->offset;
    const uint8_t *src = s->data;
    uint8_t *d = dst;
    for (uint64_t e = 0; e < in->elements; e++) {
        memcpy(d + e * es, src + pos * es, es);
        for (uint32_t ax = in->rank; ax-- > 0;) {
            if (++idx[ax] < in->shape[ax]) { pos += in->strides[ax]; break; }
            pos -= (in->shape[ax] - 1) * in->strides[ax];
            idx[ax] = 0;
        }
    }
}

/* New tensor owning fresh dense storage. *data receives the storage buffer
 * for the caller to fill before anyone else can see the tensor. */
static int new_dense(OmegaTensorCtx *ctx, OmegaDType dt, uint32_t rank, const uint64_t *shape,
                     OmegaTensor *out, void **data) {
    uint64_t n;
    int rc = shape_check(rank, shape, &n);
    if (rc) return rc;
    OmegaStorageHandle h;
    rc = storage_alloc(ctx, dt, n, &h);
    if (rc) return rc;
    OmegaTensorInfo info;
    memset(&info, 0, sizeof(info));
    info.dtype = dt;
    info.rank = rank;
    if (rank) memcpy(info.shape, shape, rank * sizeof(uint64_t));
    dense_strides(rank, info.shape, info.strides);
    info.elements = n;
    info.storage = h;
    info.is_view = false;
    rc = tensor_slot_alloc(ctx, &info, true, out);
    if (rc) { storage_release(ctx, h); return rc; }
    ctx->ts[out->slot].info.parent = *out;
    *data = ctx->st[h.slot].data;
    return OMEGA_TENSOR_OK;
}

/* ---- creation / inspection --------------------------------------------- */

int omega_tensor_from_data(OmegaTensorCtx *ctx, OmegaDType dtype, uint32_t rank,
                           const uint64_t *shape, const void *data, OmegaTensor *out) {
    if (!ctx || !data || !out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (!omega_dtype_size(dtype)) return OMEGA_TENSOR_ERR_DTYPE;
    void *buf;
    int rc = new_dense(ctx, dtype, rank, shape, out, &buf);
    if (rc) return rc;
    memcpy(buf, data, (size_t)ctx->ts[out->slot].info.elements * omega_dtype_size(dtype));
    return OMEGA_TENSOR_OK;
}

int omega_tensor_from_f32(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape,
                          const float *data, OmegaTensor *out) {
    return omega_tensor_from_data(ctx, OMEGA_DT_F32, rank, shape, data, out);
}

int omega_tensor_info(const OmegaTensorCtx *ctx, OmegaTensor t, OmegaTensorInfo *out) {
    TensorSlot *x;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, NULL);
    if (rc) return rc;
    *out = x->info;
    return OMEGA_TENSOR_OK;
}

int omega_tensor_read(const OmegaTensorCtx *ctx, OmegaTensor t, void *out, size_t out_bytes) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    if (out_bytes < x->info.elements * omega_dtype_size(x->info.dtype)) return OMEGA_TENSOR_ERR_CAPACITY;
    gather(s, &x->info, out);
    return OMEGA_TENSOR_OK;
}

int omega_tensor_read_f32(const OmegaTensorCtx *ctx, OmegaTensor t, float *out, size_t count) {
    TensorSlot *x;
    int rc = tensor_get(ctx, t, &x, NULL);
    if (rc) return rc;
    if (x->info.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    return omega_tensor_read(ctx, t, out, count * sizeof(float));
}

static void put_u64(sha256_ctx *h, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    sha256_update(h, b, 8);
}

/* Canonical element bits: every NaN becomes the canonical quiet NaN. */
static uint32_t canon_bits(OmegaDType dt, const uint8_t *p) {
    if (dt == OMEGA_DT_F32) {
        uint32_t u;
        memcpy(&u, p, 4);
        return ((u & 0x7f800000U) == 0x7f800000U && (u & 0x007fffffU)) ? 0x7fc00000U : u;
    }
    uint16_t h;
    memcpy(&h, p, 2);
    if (dt == OMEGA_DT_F16) return ((h & 0x7c00U) == 0x7c00U && (h & 0x03ffU)) ? 0x7e00U : h;
    return ((h & 0x7f80U) == 0x7f80U && (h & 0x007fU)) ? 0x7fc0U : h;
}

int omega_tensor_value_id(const OmegaTensorCtx *ctx, OmegaTensor t, uint8_t id[32]) {
    TensorSlot *x;
    StorageSlot *s;
    if (!id) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    size_t es = omega_dtype_size(x->info.dtype);
    uint8_t *buf = malloc((size_t)x->info.elements * es);
    if (!buf) return OMEGA_TENSOR_ERR_CAPACITY;
    gather(s, &x->info, buf);
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)OMEGA_TENSOR_VALUE_ID_DOMAIN, sizeof(OMEGA_TENSOR_VALUE_ID_DOMAIN));
    put_u64(&h, (uint64_t)x->info.dtype);
    put_u64(&h, x->info.rank);
    for (uint32_t d = 0; d < x->info.rank; d++) put_u64(&h, x->info.shape[d]);
    for (uint64_t e = 0; e < x->info.elements; e++) {
        uint32_t c = canon_bits(x->info.dtype, buf + e * es);
        uint8_t b[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
        sha256_update(&h, b, es);
    }
    sha256_final(&h, id);
    free(buf);
    return OMEGA_TENSOR_OK;
}

int omega_tensor_value_equal(const OmegaTensorCtx *ctx, OmegaTensor a, OmegaTensor b, bool *eq) {
    TensorSlot *xa, *xb;
    StorageSlot *sa, *sb;
    if (!eq) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, a, &xa, &sa);
    if (!rc) rc = tensor_get(ctx, b, &xb, &sb);
    if (rc) return rc;
    *eq = false;
    if (xa->info.dtype != xb->info.dtype || xa->info.rank != xb->info.rank) return OMEGA_TENSOR_OK;
    for (uint32_t d = 0; d < xa->info.rank; d++)
        if (xa->info.shape[d] != xb->info.shape[d]) return OMEGA_TENSOR_OK;
    size_t es = omega_dtype_size(xa->info.dtype);
    size_t bytes = (size_t)xa->info.elements * es;
    uint8_t *ba = malloc(bytes), *bb = malloc(bytes);
    if (!ba || !bb) { free(ba); free(bb); return OMEGA_TENSOR_ERR_CAPACITY; }
    gather(sa, &xa->info, ba);
    gather(sb, &xb->info, bb);
    bool same = true;
    for (uint64_t e = 0; e < xa->info.elements && same; e++)
        same = canon_bits(xa->info.dtype, ba + e * es) == canon_bits(xb->info.dtype, bb + e * es);
    free(ba);
    free(bb);
    *eq = same;
    return OMEGA_TENSOR_OK;
}

int omega_tensor_view_id(const OmegaTensorCtx *ctx, OmegaTensor t, uint8_t id[32]) {
    TensorSlot *x;
    if (!id) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, NULL);
    if (rc) return rc;
    const OmegaTensorInfo *in = &x->info;
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)OMEGA_TENSOR_VIEW_ID_DOMAIN, sizeof(OMEGA_TENSOR_VIEW_ID_DOMAIN));
    put_u64(&h, in->storage.slot);
    put_u64(&h, in->storage.generation);
    put_u64(&h, in->parent.slot);
    put_u64(&h, in->parent.generation);
    put_u64(&h, (uint64_t)in->dtype);
    put_u64(&h, in->offset);
    put_u64(&h, in->rank);
    for (uint32_t d = 0; d < in->rank; d++) { put_u64(&h, in->shape[d]); put_u64(&h, in->strides[d]); }
    sha256_final(&h, id);
    return OMEGA_TENSOR_OK;
}

/* ---- views -------------------------------------------------------------- */

static int register_view(OmegaTensorCtx *ctx, OmegaTensor parent, const StorageSlot *s,
                         OmegaTensorInfo *v, OmegaTensor *out) {
    int rc = bounds_check(v, s->elems);
    if (rc) return rc;
    v->is_view = true;
    v->parent = parent;
    return tensor_slot_alloc(ctx, v, false, out);
}

int omega_tensor_permute(OmegaTensorCtx *ctx, OmegaTensor t, const uint32_t *perm, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    const OmegaTensorInfo *in = &x->info;
    if (in->rank && !perm) return OMEGA_TENSOR_ERR_BAD_ARGS;
    bool seen[OMEGA_TENSOR_MAX_RANK] = {false};
    OmegaTensorInfo v = *in;
    for (uint32_t d = 0; d < in->rank; d++) {
        if (perm[d] >= in->rank || seen[perm[d]]) return OMEGA_TENSOR_ERR_AXIS;
        seen[perm[d]] = true;
        v.shape[d] = in->shape[perm[d]];
        v.strides[d] = in->strides[perm[d]]; /* MUT:PERMUTE_STRIDES */
    }
    return register_view(ctx, t, s, &v, out);
}

int omega_tensor_transpose(OmegaTensorCtx *ctx, OmegaTensor t, OmegaTensor *out) {
    TensorSlot *x;
    int rc = tensor_get(ctx, t, &x, NULL);
    if (rc) return rc;
    uint32_t r = x->info.rank;
    if (r < 2) return OMEGA_TENSOR_ERR_RANK;
    uint32_t perm[OMEGA_TENSOR_MAX_RANK];
    for (uint32_t d = 0; d < r; d++) perm[d] = d;
    perm[r - 2] = r - 1;
    perm[r - 1] = r - 2;
    return omega_tensor_permute(ctx, t, perm, out);
}

int omega_tensor_slice(OmegaTensorCtx *ctx, OmegaTensor t, const uint64_t *start,
                       const uint64_t *stop, const uint64_t *step, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    const OmegaTensorInfo *in = &x->info;
    if (in->rank == 0) return OMEGA_TENSOR_ERR_RANK;
    if (!start || !stop) return OMEGA_TENSOR_ERR_BAD_ARGS;
    OmegaTensorInfo v = *in;
    uint64_t n = 1;
    for (uint32_t d = 0; d < in->rank; d++) {
        uint64_t st = step ? step[d] : 1;
        if (st == 0) return OMEGA_TENSOR_ERR_BAD_ARGS;
        if (stop[d] > in->shape[d]) return OMEGA_TENSOR_ERR_BOUNDS;
        if (start[d] >= stop[d]) return OMEGA_TENSOR_ERR_SHAPE;
        v.shape[d] = (stop[d] - start[d] - 1) / st + 1;  /* ceil, no overflow */
        uint64_t so;
        if (__builtin_mul_overflow(start[d], in->strides[d], &so) || /* MUT:SLICE_OFFSET */
            __builtin_add_overflow(v.offset, so, &v.offset))
            return OMEGA_TENSOR_ERR_BOUNDS;
        /* A single-element axis never uses its stride; 0 avoids overflow. */
        if (v.shape[d] == 1) v.strides[d] = 0;
        else if (__builtin_mul_overflow(in->strides[d], st, &v.strides[d])) return OMEGA_TENSOR_ERR_BOUNDS;
        n *= v.shape[d];
    }
    v.elements = n;
    return register_view(ctx, t, s, &v, out);
}

static bool is_row_major(const OmegaTensorInfo *in) {
    uint64_t expect = 1;
    for (uint32_t d = in->rank; d-- > 0;) {
        if (in->shape[d] != 1 && in->strides[d] != expect) return false;
        expect *= in->shape[d];
    }
    return true;
}

int omega_tensor_reshape(OmegaTensorCtx *ctx, OmegaTensor t, uint32_t rank,
                         const uint64_t *shape, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    uint64_t n;
    rc = shape_check(rank, shape, &n);
    if (rc) return rc;
    if (n != x->info.elements) return OMEGA_TENSOR_ERR_SHAPE;
    if (!is_row_major(&x->info)) return OMEGA_TENSOR_ERR_NOT_CONTIGUOUS;
    OmegaTensorInfo v = x->info;
    v.rank = rank;
    memset(v.shape, 0, sizeof(v.shape));
    memset(v.strides, 0, sizeof(v.strides));
    if (rank) memcpy(v.shape, shape, rank * sizeof(uint64_t));
    dense_strides(rank, v.shape, v.strides);
    return register_view(ctx, t, s, &v, out);
}

int omega_tensor_broadcast_to(OmegaTensorCtx *ctx, OmegaTensor t, uint32_t rank,
                              const uint64_t *shape, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    if (rank > OMEGA_TENSOR_MAX_RANK) return OMEGA_TENSOR_ERR_RANK;
    OmegaTensorInfo v;
    rc = broadcast_info(&x->info, rank, shape, &v);
    if (rc) return rc;
    return register_view(ctx, t, s, &v, out);
}

/* ---- ops ---------------------------------------------------------------- */

static int numeric_fail(OmegaTensorCtx *ctx, int code, OmegaTensor made) {
    ctx->last_numeric = code;
    omega_tensor_release(ctx, made);
    return OMEGA_TENSOR_ERR_NUMERIC;
}

int omega_tensor_contiguous(OmegaTensorCtx *ctx, OmegaTensor t, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    OmegaTensorInfo in = x->info;  /* copy: slot table may be reused below */
    void *buf;
    rc = new_dense(ctx, in.dtype, in.rank, in.shape, out, &buf);
    if (rc) return rc;
    gather(s, &in, buf);
    return OMEGA_TENSOR_OK;
}

int omega_tensor_cast(OmegaTensorCtx *ctx, OmegaTensor t, OmegaDType to, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (!omega_dtype_size(to)) return OMEGA_TENSOR_ERR_DTYPE;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    OmegaTensorInfo in = x->info;
    if (in.dtype == to) return omega_tensor_contiguous(ctx, t, out);
    if (in.dtype != OMEGA_DT_F32 && to != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    size_t n = (size_t)in.elements;
    float *src = malloc(n * sizeof(float)), *dst = malloc(n * sizeof(float));
    if (!src || !dst) { free(src); free(dst); return OMEGA_TENSOR_ERR_CAPACITY; }
    OmegaNumericOp op;
    if (in.dtype == OMEGA_DT_F32) {
        gather(s, &in, src);
        op = to == OMEGA_DT_F16 ? OMEGA_NOP_F32_TO_F16 : OMEGA_NOP_F32_TO_BF16;
    } else {
        uint16_t *h = malloc(n * sizeof(uint16_t));
        if (!h) { free(src); free(dst); return OMEGA_TENSOR_ERR_CAPACITY; }
        gather(s, &in, h);
        for (size_t i = 0; i < n; i++) src[i] = omega_bits_to_float(h[i]);
        free(h);
        op = in.dtype == OMEGA_DT_F16 ? OMEGA_NOP_F16_TO_F32 : OMEGA_NOP_BF16_TO_F32;
    }
    void *buf;
    rc = new_dense(ctx, to, in.rank, in.shape, out, &buf);
    if (rc) { free(src); free(dst); return rc; }
    int nrc = ctx->real->elementwise(op, src, NULL, NULL, dst, n);
    if (nrc) { free(src); free(dst); return numeric_fail(ctx, nrc, *out); }
    if (to == OMEGA_DT_F32) {
        memcpy(buf, dst, n * sizeof(float));
    } else {
        uint16_t *o = buf;
        for (size_t i = 0; i < n; i++) o[i] = (uint16_t)(omega_float_to_bits(dst[i]) & 0xffffU);
    }
    free(src);
    free(dst);
    return OMEGA_TENSOR_OK;
}

static const OmegaNumericOp BINARY_MAP[OMEGA_TB_COUNT] = {
    [OMEGA_TB_ADD] = OMEGA_NOP_FADD,           [OMEGA_TB_SUB] = OMEGA_NOP_FSUB,
    [OMEGA_TB_MUL] = OMEGA_NOP_FMUL,           [OMEGA_TB_DIV] = OMEGA_NOP_DIV,
    [OMEGA_TB_MIN] = OMEGA_NOP_FMNMX_MIN,      [OMEGA_TB_MAX] = OMEGA_NOP_FMNMX_MAX,
    [OMEGA_TB_SEL_GE] = OMEGA_NOP_FSETP_SEL,   [OMEGA_TB_SEL_LT] = OMEGA_NOP_FSETP_LT_SEL,
    [OMEGA_TB_SEL_LE] = OMEGA_NOP_FSETP_LE_SEL, [OMEGA_TB_SEL_GT] = OMEGA_NOP_FSETP_GT_SEL,
    [OMEGA_TB_SEL_EQ] = OMEGA_NOP_FSETP_EQ_SEL, [OMEGA_TB_SEL_NE] = OMEGA_NOP_FSETP_NE_SEL,
    [OMEGA_TB_SEL_NUM] = OMEGA_NOP_FSETP_NUM_SEL, [OMEGA_TB_SEL_NAN] = OMEGA_NOP_FSETP_NAN_SEL,
    [OMEGA_TB_SEL_LTU] = OMEGA_NOP_FSETP_LTU_SEL, [OMEGA_TB_SEL_LEU] = OMEGA_NOP_FSETP_LEU_SEL,
    [OMEGA_TB_SEL_GTU] = OMEGA_NOP_FSETP_GTU_SEL, [OMEGA_TB_SEL_GEU] = OMEGA_NOP_FSETP_GEU_SEL,
    [OMEGA_TB_SEL_EQU] = OMEGA_NOP_FSETP_EQU_SEL, [OMEGA_TB_SEL_NEU] = OMEGA_NOP_FSETP_NEU_SEL,
};

/* Elementwise over 1..3 F32 operands broadcast together. */
static int elementwise_n(OmegaTensorCtx *ctx, OmegaNumericOp op, unsigned arity, const OmegaTensor *ops,
                         OmegaTensor *out) {
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    TensorSlot *x[3];
    StorageSlot *s[3];
    for (unsigned i = 0; i < arity; i++) {
        int rc = tensor_get(ctx, ops[i], &x[i], &s[i]);
        if (rc) return rc;
    }
    for (unsigned i = 0; i < arity; i++)
        if (x[i]->info.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    uint32_t r = x[0]->info.rank;
    uint64_t shape[OMEGA_TENSOR_MAX_RANK];
    if (r) memcpy(shape, x[0]->info.shape, r * sizeof(uint64_t));
    for (unsigned i = 1; i < arity; i++) {
        int rc = omega_tensor_broadcast_shape(r, shape, x[i]->info.rank, x[i]->info.shape, &r, shape);
        if (rc) return rc;
    }
    OmegaTensorInfo bi[3];
    for (unsigned i = 0; i < arity; i++) {
        int rc = broadcast_info(&x[i]->info, r, shape, &bi[i]);
        if (rc) return rc;
    }
    size_t n = (size_t)bi[0].elements;
    float *in[3] = {NULL, NULL, NULL};
    for (unsigned i = 0; i < arity; i++) {
        in[i] = malloc(n * sizeof(float));
        if (!in[i]) { for (unsigned j = 0; j < i; j++) free(in[j]); return OMEGA_TENSOR_ERR_CAPACITY; }
        gather(s[i], &bi[i], in[i]);
    }
    void *buf;
    int rc = new_dense(ctx, OMEGA_DT_F32, r, shape, out, &buf);
    if (!rc) {
        int nrc = ctx->real->elementwise(op, in[0], in[1], in[2], buf, n);
        if (nrc) rc = numeric_fail(ctx, nrc, *out);
    }
    for (unsigned i = 0; i < arity; i++) free(in[i]);
    return rc;
}
/* NEG: sign-bit flip; any NaN becomes the canonical quiet NaN. */
static uint32_t neg_bits(uint32_t u) {
    if ((u & 0x7f800000U) == 0x7f800000U && (u & 0x007fffffU)) return 0x7fc00000U; /* MUT:NEG_NAN */
    return u ^ 0x80000000U; /* MUT:NEG_SUB */
}


/* RELU (M20 cut ops): no E1 op exists, so the tensor layer defines it as a
 * pure bit-level select on the FP32 pattern, with no float arithmetic and no
 * realization call. Rule: any NaN (exponent all ones, mantissa nonzero, any
 * sign or payload) -> the canonical E1 qNaN OMEGA_QNAN_BITS (omega_numeric.h:48,
 * as E1 ops return it, e.g. omega_numeric.c:459); sign bit set (-0, negative
 * normals and subnormals, -inf) -> +0.0; otherwise (+0, positive values, +inf)
 * -> the input bits unchanged. */
static uint32_t relu_bits(uint32_t u) {
    if ((u & 0x7fffffffU) > OMEGA_INF_POS) return OMEGA_QNAN_BITS; /* MUT:RELU_NAN_PAYLOAD */
    if (u >> 31) return 0U; /* MUT:RELU_NEG_ZERO */
    return u;
}

static void relu_dense(const float *src, float *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &src[i], sizeof u);
        u = relu_bits(u);
        memcpy(&dst[i], &u, sizeof u);
    }
}

/* Unary ops: gather the operand through its own view descriptor (offset,
 * shape, strides, including stride-0 broadcast axes) into a dense buffer,
 * then one realization call. SQRT goes through the E1 elementwise op; the
 * bounded-contract transcendentals and EXP / LOG through the realization's
 * transc entry; RELU is the tensor-layer bit select above (no realization). */
int omega_tensor_unary(OmegaTensorCtx *ctx, OmegaTensorUnaryOp op, OmegaTensor a, OmegaTensor *out) {
    if (!ctx || !out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if ((unsigned)op >= OMEGA_TU_COUNT) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (op != OMEGA_TU_SQRT && op != OMEGA_TU_RELU && op != OMEGA_TU_NEG && !ctx->real->transc) return OMEGA_TENSOR_ERR_REALIZATION;
    TensorSlot *x;
    StorageSlot *s;
    int rc = tensor_get(ctx, a, &x, &s);
    if (rc) return rc;
    OmegaTensorInfo in = x->info;  /* copy: slot table may be reused below */
    if (in.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    size_t n = (size_t)in.elements;
    float *src = malloc(n * sizeof(float));
    if (!src) return OMEGA_TENSOR_ERR_CAPACITY;
    gather(s, &in, src); /* MUT:UNARY_VIEW_STRIDE MUT:GELU_VIEW_STRIDE */
    void *buf;
    rc = new_dense(ctx, OMEGA_DT_F32, in.rank, in.shape, out, &buf);
    if (!rc) {
        int nrc = 0;
        if (op == OMEGA_TU_RELU) relu_dense(src, buf, n);
        else if (op == OMEGA_TU_NEG) {
            uint32_t *ob = buf;
            for (size_t i = 0; i < n; i++) ob[i] = neg_bits(omega_float_to_bits(src[i]));
        } else {
            nrc = op == OMEGA_TU_SQRT ? ctx->real->elementwise(OMEGA_NOP_SQRT, src, NULL, NULL, buf, n)
                                      : ctx->real->transc(op, src, buf, n);
        }
        if (nrc) rc = numeric_fail(ctx, nrc, *out);
    }
    free(src);
    return rc;
}

/* Constant tensors: pure bit fill, no arithmetic. */
int omega_tensor_full(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, float value,
                      OmegaTensor *out) {
    if (!ctx || !out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    void *buf;
    int rc = new_dense(ctx, OMEGA_DT_F32, rank, shape, out, &buf);
    if (rc) return rc;
    uint32_t u = omega_float_to_bits(value); /* MUT:FULL_CANON */
    uint32_t *p = buf;
    size_t n = (size_t)ctx->ts[out->slot].info.elements;
    for (size_t i = 0; i < n; i++) p[i] = u;
    return OMEGA_TENSOR_OK;
}

int omega_tensor_zeros(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, OmegaTensor *out) {
    return omega_tensor_full(ctx, rank, shape, 0.0f, out); /* MUT:ZEROS_NEG0 */
}

int omega_tensor_ones(OmegaTensorCtx *ctx, uint32_t rank, const uint64_t *shape, OmegaTensor *out) {
    return omega_tensor_full(ctx, rank, shape, 1.0f, out);
}

int omega_tensor_binary(OmegaTensorCtx *ctx, OmegaTensorBinaryOp op, OmegaTensor a, OmegaTensor b,
                        OmegaTensor *out) {
    if (!ctx) return OMEGA_TENSOR_ERR_BAD_ARGS;
    if ((unsigned)op >= OMEGA_TB_COUNT) return OMEGA_TENSOR_ERR_BAD_ARGS;
    OmegaTensor ops[2] = {a, b};
    return elementwise_n(ctx, BINARY_MAP[op], 2, ops, out);
}

int omega_tensor_fma(OmegaTensorCtx *ctx, OmegaTensor a, OmegaTensor b, OmegaTensor c, OmegaTensor *out) {
    if (!ctx) return OMEGA_TENSOR_ERR_BAD_ARGS;
    OmegaTensor ops[3] = {a, b, c};
    return elementwise_n(ctx, OMEGA_NOP_FFMA_V, 3, ops, out);
}

int omega_tensor_reduce(OmegaTensorCtx *ctx, OmegaTensorReduceOp op, OmegaTensor t, uint32_t axis,
                        bool keepdims, OmegaTensor *out) {
    TensorSlot *x;
    StorageSlot *s;
    if (!out || (unsigned)op >= OMEGA_TR_COUNT) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, t, &x, &s);
    if (rc) return rc;
    OmegaTensorInfo in = x->info;
    if (in.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    if (in.rank == 0) return OMEGA_TENSOR_ERR_RANK;
    if (axis >= in.rank) return OMEGA_TENSOR_ERR_AXIS;
    /* Move the axis last (descriptor only), gather rows densely. */
    OmegaTensorInfo moved = in;
    uint32_t k = 0;
    for (uint32_t d = 0; d < in.rank; d++) {
        if (d == axis) continue;
        moved.shape[k] = in.shape[d];
        moved.strides[k] = in.strides[d];
        k++;
    }
    moved.shape[k] = in.shape[axis];
    moved.strides[k] = in.strides[axis];
    size_t n = (size_t)in.shape[axis], rows = (size_t)(in.elements / in.shape[axis]);
    uint32_t orank = keepdims ? in.rank : in.rank - 1;
    uint64_t oshape[OMEGA_TENSOR_MAX_RANK];
    for (uint32_t d = 0, j = 0; d < in.rank; d++) {
        if (d == axis) { if (keepdims) oshape[j++] = 1; continue; }
        oshape[j++] = in.shape[d];
    }
    float *dense = malloc((size_t)in.elements * sizeof(float));
    if (!dense) return OMEGA_TENSOR_ERR_CAPACITY;
    gather(s, &moved, dense);
    void *buf;
    rc = new_dense(ctx, OMEGA_DT_F32, orank, oshape, out, &buf);
    if (!rc) {
        float *o = buf;
        for (size_t r = 0; r < rows; r++) {
            int nrc = ctx->real->reduce(op, dense + r * n, n, &o[r]);
            if (nrc) { rc = numeric_fail(ctx, nrc, *out); break; }
        }
    }
    free(dense);
    return rc;
}

int omega_tensor_matmul(OmegaTensorCtx *ctx, OmegaTensor a, OmegaTensor b, OmegaTensor *out) {
    TensorSlot *xa, *xb;
    StorageSlot *sa, *sb;
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    int rc = tensor_get(ctx, a, &xa, &sa);
    if (!rc) rc = tensor_get(ctx, b, &xb, &sb);
    if (rc) return rc;
    OmegaTensorInfo ia = xa->info, ib = xb->info;
    if (ia.dtype != OMEGA_DT_F32 || ib.dtype != OMEGA_DT_F32) return OMEGA_TENSOR_ERR_DTYPE;
    if (ia.rank < 2 || ib.rank < 2) return OMEGA_TENSOR_ERR_RANK;
    uint64_t M = ia.shape[ia.rank - 2], K = ia.shape[ia.rank - 1];
    uint64_t K2 = ib.shape[ib.rank - 2], N = ib.shape[ib.rank - 1];
    if (K != K2) return OMEGA_TENSOR_ERR_SHAPE;
    uint32_t br;
    uint64_t bshape[OMEGA_TENSOR_MAX_RANK];
    rc = omega_tensor_broadcast_shape(ia.rank - 2, ia.shape, ib.rank - 2, ib.shape, &br, bshape);
    if (rc) return rc;
    if (br + 2 > OMEGA_TENSOR_MAX_RANK) return OMEGA_TENSOR_ERR_RANK;
    uint64_t sha[OMEGA_TENSOR_MAX_RANK], shb[OMEGA_TENSOR_MAX_RANK], sho[OMEGA_TENSOR_MAX_RANK];
    memcpy(sha, bshape, br * sizeof(uint64_t));
    memcpy(shb, bshape, br * sizeof(uint64_t));
    memcpy(sho, bshape, br * sizeof(uint64_t));
    sha[br] = M; sha[br + 1] = K;
    shb[br] = K; shb[br + 1] = N;
    sho[br] = M; sho[br + 1] = N;
    OmegaTensorInfo va, vb;
    rc = broadcast_info(&ia, br + 2, sha, &va);
    if (!rc) rc = broadcast_info(&ib, br + 2, shb, &vb);
    if (rc) return rc;
    /* B with its last two axes swapped, so each column is a dense row. */
    uint64_t t0 = vb.shape[br], t1 = vb.strides[br];
    vb.shape[br] = vb.shape[br + 1]; vb.strides[br] = vb.strides[br + 1];
    vb.shape[br + 1] = t0;           vb.strides[br + 1] = t1;
    uint64_t batches = va.elements / (M * K);
    float *A = malloc((size_t)va.elements * sizeof(float));
    float *BT = malloc((size_t)vb.elements * sizeof(float));
    float *prod = malloc((size_t)K * sizeof(float));
    if (!A || !BT || !prod) { free(A); free(BT); free(prod); return OMEGA_TENSOR_ERR_CAPACITY; }
    gather(sa, &va, A);
    gather(sb, &vb, BT);
    void *buf;
    rc = new_dense(ctx, OMEGA_DT_F32, br + 2, sho, out, &buf);
    if (!rc) {
        float *o = buf;
        for (uint64_t bt = 0; bt < batches && !rc; bt++)
            for (uint64_t i = 0; i < M && !rc; i++)
                for (uint64_t j = 0; j < N && !rc; j++) {
                    const float *ar = A + (bt * M + i) * K;
                    const float *bc = BT + (bt * N + j) * K;
                    int nrc = ctx->real->elementwise(OMEGA_NOP_FMUL, ar, bc, NULL, prod, (size_t)K);
                    if (!nrc) nrc = ctx->real->reduce(OMEGA_TR_SUM, prod, (size_t)K, &o[(bt * M + i) * N + j]);
                    if (nrc) rc = numeric_fail(ctx, nrc, *out);
                }
    }
    free(A);
    free(BT);
    free(prod);
    return rc;
}

void omega_tensor_live_counts(const OmegaTensorCtx *ctx, uint32_t *tensors, uint32_t *storages) {
    uint32_t t = 0, s = 0;
    for (uint32_t i = 0; ctx && i < ctx->cap; i++) { t += ctx->ts[i].live; s += ctx->st[i].live; }
    if (tensors) *tensors = t;
    if (storages) *storages = s;
}
