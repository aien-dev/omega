/*
 * rx_jspace.c -- branch-native shared cognitive state (see rx_jspace.h).
 *
 * Simple reference implementation: single-threaded, heap buffers, one spill
 * file with no space reuse. Content checks use a 64-bit word hash so the
 * store does not pay SHA-256 on every unit; tests compare full SHA-256
 * digests outside the store.
 */
#include "rx_jspace.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- content check ------------------------------------------------------- */

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static void content_check(const uint8_t *p, size_t n, uint8_t out[32]) {
    uint64_t h[4] = { 0x243f6a8885a308d3ull, 0x13198a2e03707344ull,
                      0xa4093822299f31d0ull, 0x082efa98ec4e6c89ull };
    size_t words = n / 8;
    for (size_t i = 0; i < words; i++) {
        uint64_t v;
        memcpy(&v, p + i * 8, 8);
        h[i & 3] = mix64(h[i & 3] ^ v) + i;
    }
    for (size_t i = words * 8; i < n; i++) h[0] = mix64(h[0] ^ p[i]);
    for (int i = 0; i < 4; i++) {
        uint64_t v = mix64(h[i] ^ h[(i + 1) & 3] ^ n);
        memcpy(out + i * 8, &v, 8);
    }
}

/* ---- semantic identity --------------------------------------------------- */

static JsSemId sem_hash(const uint8_t *tag, size_t tag_len, const uint8_t *a, size_t an,
                        const uint8_t *b, size_t bn) {
    sha256_ctx c;
    JsSemId id;
    sha256_init(&c);
    sha256_update(&c, tag, tag_len);
    if (an) sha256_update(&c, a, an);
    if (bn) sha256_update(&c, b, bn);
    sha256_final(&c, id.b);
    return id;
}

JsSemId js_sem_root(uint64_t seed) {
    return sem_hash((const uint8_t *)"js.root", 7, (const uint8_t *)&seed, 8, NULL, 0);
}

JsSemId js_sem_derive(const JsSemId *prev, uint64_t token) {
    return sem_hash((const uint8_t *)"js.derive", 9, prev->b, 32, (const uint8_t *)&token, 8);
}

JsSemId js_sem_edit(const JsSemId *prev, uint32_t off, const uint8_t *patch, uint32_t len) {
    uint8_t hdr[40];
    uint8_t ph[32];
    sha256_hash(patch, len, ph);
    memcpy(hdr, prev->b, 32);
    memcpy(hdr + 32, &off, 4);
    memcpy(hdr + 36, &len, 4);
    return sem_hash((const uint8_t *)"js.edit", 7, hdr, sizeof hdr, ph, 32);
}

JsSemId js_branch_semantic_id(const JsBranch *b) {
    sha256_ctx c;
    JsSemId id;
    uint32_t n = b->n_units;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"js.branch", 9);
    sha256_update(&c, (const uint8_t *)&n, 4);
    for (uint32_t i = 0; i < n; i++) sha256_update(&c, b->units[i]->semantic_state_id.b, 32);
    sha256_final(&c, id.b);
    return id;
}

/* ---- residency accounting ------------------------------------------------ */

static uint64_t resident_of(const JsReal *r) {
    switch (r->placement) {
    case JS_PLACE_HOT: case JS_PLACE_COLD: return r->realizer->unit_bytes;
    case JS_PLACE_COMPRESSED: return r->packed_len;
    default: return 0;
    }
}

static void charge(JsSpace *s, const JsReal *r, int sign) {
    uint64_t b = resident_of(r);
    if (sign > 0) {
        s->stats.resident_bytes += b;
        if (s->stats.resident_bytes > s->stats.peak_resident_bytes)
            s->stats.peak_resident_bytes = s->stats.resident_bytes;
        if (r->placement == JS_PLACE_SPILLED) s->stats.spilled_bytes += r->realizer->unit_bytes;
    } else {
        s->stats.resident_bytes -= b;
        if (r->placement == JS_PLACE_SPILLED) s->stats.spilled_bytes -= r->realizer->unit_bytes;
    }
}

/* Drop whatever physical form r has; placement becomes EVICTED. */
static void drop_physical(JsSpace *s, JsReal *r) {
    charge(s, r, -1);
    free(r->bytes); r->bytes = NULL;
    free(r->packed); r->packed = NULL; r->packed_len = 0;
    r->placement = JS_PLACE_EVICTED;
}

static void set_hot(JsSpace *s, JsReal *r, uint8_t *bytes) {
    r->bytes = bytes;
    r->placement = JS_PLACE_HOT;
    charge(s, r, +1);
}

/* ---- semantic reuse index ------------------------------------------------- */

static uint32_t bucket_of(const JsSemId *id, JsRealType t) {
    uint32_t v;
    memcpy(&v, id->b, 4);
    return (v ^ (uint32_t)t * 0x9e3779b1u) & (JS_INDEX_BUCKETS - 1u);
}

static JsReal *index_find(JsSpace *s, const JsSemId *id, JsRealType t) {
    for (JsReal *r = s->index[bucket_of(id, t)]; r; r = r->index_next)
        if (r->realization_type == t && !memcmp(r->semantic_state_id.b, id->b, 32)) return r;
    return NULL;
}

static void index_insert(JsSpace *s, JsReal *r) {
    uint32_t k = bucket_of(&r->semantic_state_id, r->realization_type);
    r->index_next = s->index[k];
    s->index[k] = r;
}

static void index_remove(JsSpace *s, JsReal *r) {
    JsReal **p = &s->index[bucket_of(&r->semantic_state_id, r->realization_type)];
    while (*p && *p != r) p = &(*p)->index_next;
    if (*p) *p = r->index_next;
}

/* ---- realization lifetime ------------------------------------------------ */

static void real_unref(JsSpace *s, JsReal *r) {
    while (r && --r->refs == 0) {
        JsReal *parent = r->parent_realization;
        if (s->cache_r == r) s->cache_r = NULL;
        index_remove(s, r);
        drop_physical(s, r);
        free(r->edit_bytes);
        free(r);
        s->stats.live_reals--;
        r = parent;
    }
}

static JsReal *real_new(JsSpace *s, const JsRealizer *rz, JsSemId id, JsReal *parent,
                        JsRecipeKind kind) {
    JsReal *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->semantic_state_id = id;
    r->realization_type = rz->type;
    r->realizer = rz;
    r->parent_realization = parent;
    if (parent) parent->refs++;
    r->recipe = kind;
    r->placement = JS_PLACE_EVICTED;
    s->stats.live_reals++;
    return r;
}

/* ---- materialization ----------------------------------------------------- */

/* Codec for COMPRESSED: bytes XOR parent bytes (zeros for a root), then
 * 8-byte words as [u32 zero words][u32 literal words][literals]... */
static size_t pack_delta(const uint8_t *cur, const uint8_t *base, size_t n, uint8_t *out) {
    size_t words = n / 8, i = 0, o = 0;
    while (i < words) {
        uint32_t z = 0, l = 0;
        while (i + z < words) {
            uint64_t a, b = 0;
            memcpy(&a, cur + (i + z) * 8, 8);
            if (base) memcpy(&b, base + (i + z) * 8, 8);
            if (a != b) break;
            z++;
        }
        size_t lit_start = i + z;
        while (lit_start + l < words) {
            uint64_t a, b = 0;
            memcpy(&a, cur + (lit_start + l) * 8, 8);
            if (base) memcpy(&b, base + (lit_start + l) * 8, 8);
            if (a == b) break;
            l++;
        }
        memcpy(out + o, &z, 4); memcpy(out + o + 4, &l, 4); o += 8;
        for (uint32_t k = 0; k < l; k++) {
            uint64_t a, b = 0;
            memcpy(&a, cur + (lit_start + k) * 8, 8);
            if (base) memcpy(&b, base + (lit_start + k) * 8, 8);
            a ^= b;
            memcpy(out + o, &a, 8); o += 8;
        }
        i = lit_start + l;
    }
    return o;
}

static void unpack_delta(const uint8_t *in, size_t in_len, const uint8_t *base, size_t n,
                         uint8_t *out) {
    size_t i = 0, o = 0;
    if (base) memcpy(out, base, n); else memset(out, 0, n);
    while (i < in_len) {
        uint32_t z, l;
        memcpy(&z, in + i, 4); memcpy(&l, in + i + 4, 4); i += 8;
        o += z;
        for (uint32_t k = 0; k < l; k++, o++, i += 8) {
            uint64_t a, d;
            memcpy(&a, out + o * 8, 8);
            memcpy(&d, in + i, 8);
            a ^= d;
            memcpy(out + o * 8, &a, 8);
        }
    }
}

/* A node whose bytes can be produced without its parent's bytes. */
static bool is_stop(const JsReal *r) {
    return r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD ||
           r->placement == JS_PLACE_SPILLED || !r->parent_realization;
}

static int stop_bytes(JsSpace *s, JsReal *r, uint8_t *out) {
    size_t n = r->realizer->unit_bytes;
    switch (r->placement) {
    case JS_PLACE_HOT: case JS_PLACE_COLD:
        memcpy(out, r->bytes, n); return JS_OK;
    case JS_PLACE_SPILLED: {
        ssize_t got = pread(s->spill_fd, out, n, (off_t)r->spill_off);
        return got == (ssize_t)n ? JS_OK : JS_ERR_IO;
    }
    case JS_PLACE_COMPRESSED:   /* no parent: coded against zeros */
        unpack_delta(r->packed, r->packed_len, NULL, n, out);
        return JS_OK;
    default:  /* evicted root */
        r->realizer->derive(NULL, r->token, out, n);
        s->stats.derives++;
        s->stats.actions[JS_ACT_RECOMPUTE]++;
        return JS_OK;
    }
}

/* Produce r's bytes from its parent's bytes (`pb`, NULL for a root). */
static void step_bytes(JsSpace *s, JsReal *r, const uint8_t *pb, uint8_t *out) {
    size_t n = r->realizer->unit_bytes;
    if (r->placement == JS_PLACE_COMPRESSED) {
        unpack_delta(r->packed, r->packed_len, pb, n, out);
        return;
    }
    /* EVICTED: follow the recipe. */
    s->stats.actions[JS_ACT_RECOMPUTE]++;
    if (r->recipe == JS_RECIPE_DERIVE) {
        r->realizer->derive(pb, r->token, out, n);
        s->stats.derives++;
    } else {
        memcpy(out, pb, n);
        memcpy(out + r->edit_off, r->edit_bytes, r->edit_len);
    }
}

/* Write r's bytes into out without changing any placement. Walks back to the
 * nearest node that needs no parent, then replays forward. */
static int materialize(JsSpace *s, JsReal *r, uint8_t *out) {
    size_t n = r->realizer->unit_bytes;
    uint32_t depth = 0, cap = 64;
    JsReal **chain = malloc(cap * sizeof *chain);
    if (!chain) return JS_ERR_NOMEM;
    JsReal *c = r;
    bool cached = false;
    while (!is_stop(c)) {
        if (c == s->cache_r && s->cache_len == n) { cached = true; break; }
        if (depth == cap) {
            JsReal **nc = realloc(chain, (cap *= 2) * sizeof *chain);
            if (!nc) { free(chain); return JS_ERR_NOMEM; }
            chain = nc;
        }
        chain[depth++] = c;
        c = c->parent_realization;
    }
    uint8_t *a = depth ? malloc(n) : out;
    uint8_t *b = depth ? malloc(n) : NULL;
    if (depth && (!a || !b)) { free(a); free(b); free(chain); return JS_ERR_NOMEM; }
    int rc = JS_OK;
    if (cached) memcpy(a, s->cache_bytes, n);
    else rc = stop_bytes(s, c, a);
    for (uint32_t k = depth; rc == JS_OK && k-- > 0;) {
        uint8_t *dst = k == 0 ? out : b;
        step_bytes(s, chain[k], a, dst);
        if (k) { uint8_t *t = a; a = b; b = t; }
    }
    if (depth) { free(a); free(b); }
    free(chain);
    if (rc != JS_OK) return rc;
    uint8_t chk[32];
    content_check(out, n, chk);
    if (memcmp(chk, r->content, 32)) { s->stats.corrupt_restores++; return JS_ERR_CORRUPT; }
    if (depth && r->placement != JS_PLACE_HOT && r->placement != JS_PLACE_COLD) {
        if (s->cache_len != n) {
            uint8_t *nb = realloc(s->cache_bytes, n);
            if (!nb) return JS_OK;
            s->cache_bytes = nb;
            s->cache_len = n;
        }
        memcpy(s->cache_bytes, out, n);
        s->cache_r = r;
    }
    return JS_OK;
}

int js_real_restore(JsSpace *s, JsReal *r) {
    if (r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD) return JS_OK;
    uint8_t *buf = malloc(r->realizer->unit_bytes);
    if (!buf) return JS_ERR_NOMEM;
    int rc = materialize(s, r, buf);
    if (rc != JS_OK) { free(buf); return rc; }
    drop_physical(s, r);
    set_hot(s, r, buf);
    return JS_OK;
}

/* ---- physical operations -------------------------------------------------- */

int js_real_move(JsSpace *s, JsReal *r) {
    if (r->placement != JS_PLACE_HOT && r->placement != JS_PLACE_COLD) return JS_ERR_ARG;
    size_t n = r->realizer->unit_bytes;
    uint8_t *dst = malloc(n);
    if (!dst) return JS_ERR_NOMEM;
    memcpy(dst, r->bytes, n);
    free(r->bytes);
    r->bytes = dst;
    r->placement = r->placement == JS_PLACE_HOT ? JS_PLACE_COLD : JS_PLACE_HOT;
    s->stats.actions[JS_ACT_MOVE]++;
    return JS_OK;
}

int js_real_compress(JsSpace *s, JsReal *r) {
    size_t n = r->realizer->unit_bytes;
    uint8_t *cur = malloc(n), *base = NULL;
    uint8_t *tmp = malloc(n + (n / 8) * 8 + 16);
    int rc = JS_ERR_NOMEM;
    if (!cur || !tmp) goto out;
    if ((rc = materialize(s, r, cur)) != JS_OK) goto out;
    if (r->parent_realization) {
        base = malloc(n);
        if (!base) { rc = JS_ERR_NOMEM; goto out; }
        if ((rc = materialize(s, r->parent_realization, base)) != JS_OK) goto out;
    }
    size_t len = pack_delta(cur, base, n, tmp);
    uint8_t *packed = malloc(len ? len : 1);
    if (!packed) { rc = JS_ERR_NOMEM; goto out; }
    memcpy(packed, tmp, len);
    drop_physical(s, r);
    r->packed = packed;
    r->packed_len = len;
    r->placement = JS_PLACE_COMPRESSED;
    charge(s, r, +1);
    s->stats.actions[JS_ACT_COMPRESS]++;
out:
    free(cur); free(base); free(tmp);
    return rc;
}

int js_real_spill(JsSpace *s, JsReal *r) {
    size_t n = r->realizer->unit_bytes;
    uint8_t *cur = malloc(n);
    if (!cur) return JS_ERR_NOMEM;
    int rc = materialize(s, r, cur);
    if (rc == JS_OK) {
        ssize_t put = pwrite(s->spill_fd, cur, n, (off_t)s->spill_end);
        if (put != (ssize_t)n) rc = JS_ERR_IO;
    }
    free(cur);
    if (rc != JS_OK) return rc;
    drop_physical(s, r);
    r->spill_off = s->spill_end;
    s->spill_end += n;
    r->placement = JS_PLACE_SPILLED;
    charge(s, r, +1);
    s->stats.actions[JS_ACT_SPILL]++;
    return JS_OK;
}

int js_real_evict(JsSpace *s, JsReal *r) {
    if (r->placement == JS_PLACE_EVICTED) return JS_OK;
    drop_physical(s, r);
    s->stats.actions[JS_ACT_EVICT]++;
    return JS_OK;
}

/* ---- space ---------------------------------------------------------------- */

int js_space_init(JsSpace *s, const char *spill_path) {
    memset(s, 0, sizeof *s);
    s->spill_fd = open(spill_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (s->spill_fd < 0) return JS_ERR_IO;
    unlink(spill_path);   /* anonymous from here on */
    s->reuse_enabled = true;
    return JS_OK;
}

void js_space_destroy(JsSpace *s) {
    for (uint32_t i = 0; i < s->n_branches; i++)
        if (s->branches[i]) js_branch_release(s, i);
    if (s->spill_fd >= 0) close(s->spill_fd);
    s->spill_fd = -1;
    free(s->cache_bytes);
    s->cache_bytes = NULL;
    s->cache_r = NULL;
    s->cache_len = 0;
}

static JsBranch *branch_at(JsSpace *s, uint32_t id) {
    return id < s->n_branches ? s->branches[id] : NULL;
}

static int branch_push(JsBranch *b, JsReal *r) {
    if (b->n_units == b->cap_units) {
        uint32_t cap = b->cap_units ? b->cap_units * 2 : 64;
        JsReal **u = realloc(b->units, cap * sizeof *u);
        if (!u) return JS_ERR_NOMEM;
        b->units = u;
        b->cap_units = cap;
    }
    b->units[b->n_units++] = r;
    r->refs++;
    r->holders++;
    return JS_OK;
}

static int branch_alloc(JsSpace *s, const JsRealizer *rz, uint32_t *out) {
    if (s->n_branches >= JS_MAX_BRANCHES) return JS_ERR_FULL;
    JsBranch *b = calloc(1, sizeof *b);
    if (!b) return JS_ERR_NOMEM;
    b->branch_id = s->n_branches;
    b->realizer = rz;
    s->branches[s->n_branches] = b;
    *out = s->n_branches++;
    return JS_OK;
}

/* A new realization's bytes are ready in `bytes`: fix its content check,
 * make it HOT and file it in the reuse index. */
static void real_publish(JsSpace *s, JsReal *r, uint8_t *bytes) {
    content_check(bytes, r->realizer->unit_bytes, r->content);
    set_hot(s, r, bytes);
    index_insert(s, r);
}

int js_branch_root(JsSpace *s, const JsRealizer *rz, uint64_t seed, uint32_t *out) {
    uint32_t id;
    int rc = branch_alloc(s, rz, &id);
    if (rc) return rc;
    JsBranch *b = s->branches[id];
    b->parent_branch = UINT32_MAX;
    b->common_ancestor = id;
    JsSemId sid = js_sem_root(seed);
    JsReal *r = s->reuse_enabled ? index_find(s, &sid, rz->type) : NULL;
    if (r) {
        s->stats.actions[JS_ACT_REUSE]++;
    } else {
        uint8_t *bytes = malloc(rz->unit_bytes);
        if (!bytes || !(r = real_new(s, rz, sid, NULL, JS_RECIPE_ROOT))) {
            free(bytes); return JS_ERR_NOMEM;
        }
        r->token = seed;
        rz->derive(NULL, seed, bytes, rz->unit_bytes);
        s->stats.derives++;
        real_publish(s, r, bytes);
    }
    rc = branch_push(b, r);
    *out = id;
    return rc;
}

int js_branch_fork(JsSpace *s, uint32_t parent, uint32_t *out) {
    JsBranch *p = branch_at(s, parent);
    if (!p) return JS_ERR_ARG;
    uint32_t id;
    int rc = branch_alloc(s, p->realizer, &id);
    if (rc) return rc;
    JsBranch *b = s->branches[id];
    b->parent_branch = parent;
    b->common_ancestor = p->common_ancestor;
    b->divergence_point = p->n_units;
    b->units = malloc((p->n_units + 64) * sizeof *b->units);
    if (!b->units) return JS_ERR_NOMEM;
    b->cap_units = p->n_units + 64;
    memcpy(b->units, p->units, p->n_units * sizeof *b->units);
    b->n_units = p->n_units;
    for (uint32_t i = 0; i < p->n_units; i++) { p->units[i]->refs++; p->units[i]->holders++; }
    s->stats.actions[JS_ACT_REFERENCE] += p->n_units;
    p->frozen = true;
    *out = id;
    return JS_OK;
}

static int ensure_hot(JsSpace *s, JsReal *r) {
    if (r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD) return JS_OK;
    return js_real_restore(s, r);
}

int js_branch_derive(JsSpace *s, uint32_t bid, uint64_t token) {
    JsBranch *b = branch_at(s, bid);
    if (!b || !b->n_units) return JS_ERR_ARG;
    if (b->frozen) return JS_ERR_FROZEN;
    JsReal *prev = b->units[b->n_units - 1];
    JsSemId sid = js_sem_derive(&prev->semantic_state_id, token);
    JsReal *r = s->reuse_enabled ? index_find(s, &sid, b->realizer->type) : NULL;
    if (r) {
        s->stats.actions[JS_ACT_REUSE]++;
        return branch_push(b, r);
    }
    int rc = ensure_hot(s, prev);
    if (rc) return rc;
    size_t n = b->realizer->unit_bytes;
    uint8_t *bytes = malloc(n);
    if (!bytes || !(r = real_new(s, b->realizer, sid, prev, JS_RECIPE_DERIVE))) {
        free(bytes); return JS_ERR_NOMEM;
    }
    r->token = token;
    b->realizer->derive(prev->bytes, token, bytes, n);
    s->stats.derives++;
    real_publish(s, r, bytes);
    return branch_push(b, r);
}

int js_branch_edit(JsSpace *s, uint32_t bid, uint32_t idx, uint32_t off,
                   const uint8_t *patch, uint32_t len) {
    JsBranch *b = branch_at(s, bid);
    if (!b || idx >= b->n_units || !len || off + (uint64_t)len > b->realizer->unit_bytes)
        return JS_ERR_ARG;
    if (b->frozen) return JS_ERR_FROZEN;
    JsReal *old = b->units[idx];
    JsSemId sid = js_sem_edit(&old->semantic_state_id, off, patch, len);
    JsReal *r = s->reuse_enabled ? index_find(s, &sid, b->realizer->type) : NULL;
    if (r) {
        s->stats.actions[JS_ACT_REUSE]++;
        r->refs++; r->holders++;
    } else {
        size_t n = b->realizer->unit_bytes;
        uint8_t *edit = malloc(len), *bytes = NULL;
        if (!edit || !(r = real_new(s, b->realizer, sid, old, JS_RECIPE_EDIT))) {
            free(edit); return JS_ERR_NOMEM;
        }
        memcpy(edit, patch, len);
        r->edit_off = off; r->edit_len = len; r->edit_bytes = edit;
        /* Exclusively held and resident: take the buffer instead of copying. The
         * old realization keeps its recipe and becomes EVICTED; the new one holds
         * it as its parent, so it can still be rebuilt. */
        if (old->refs == 1 && (old->placement == JS_PLACE_HOT || old->placement == JS_PLACE_COLD)) {
            charge(s, old, -1);
            bytes = old->bytes;
            old->bytes = NULL;
            old->placement = JS_PLACE_EVICTED;
        } else {
            bytes = malloc(n);
            if (!bytes) { r->refs = 1; real_unref(s, r); return JS_ERR_NOMEM; }
            if (old->placement == JS_PLACE_HOT || old->placement == JS_PLACE_COLD) {
                memcpy(bytes, old->bytes, n);
                s->stats.actions[JS_ACT_COPY]++;
                s->stats.bytes_copied += n;
            } else {
                int rc = materialize(s, old, bytes);   /* counts RECOMPUTE where it rebuilds */
                if (rc) { free(bytes); r->refs = 1; real_unref(s, r); return rc; }
            }
        }
        memcpy(bytes + off, patch, len);
        real_publish(s, r, bytes);
        r->refs++; r->holders++;
    }
    b->units[idx] = r;
    old->holders--;
    real_unref(s, old);
    return JS_OK;
}

int js_branch_read(JsSpace *s, uint32_t bid, uint32_t idx, uint8_t *out) {
    JsBranch *b = branch_at(s, bid);
    if (!b || idx >= b->n_units) return JS_ERR_ARG;
    JsReal *r = b->units[idx];
    r->last_use = ++s->clock;
    r->reads++;
    if (r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD) {
        memcpy(out, r->bytes, b->realizer->unit_bytes);
        return JS_OK;
    }
    return materialize(s, r, out);
}

int js_branch_content_digest(JsSpace *s, uint32_t bid, uint8_t out[32]) {
    JsBranch *b = branch_at(s, bid);
    if (!b) return JS_ERR_ARG;
    size_t n = b->realizer->unit_bytes;
    uint8_t *buf = malloc(n);
    if (!buf) return JS_ERR_NOMEM;
    sha256_ctx c;
    sha256_init(&c);
    int rc = JS_OK;
    for (uint32_t i = 0; i < b->n_units && rc == JS_OK; i++) {
        rc = js_branch_read(s, bid, i, buf);
        if (rc == JS_OK) sha256_update(&c, buf, n);
    }
    sha256_final(&c, out);
    free(buf);
    return rc;
}

int js_branch_release(JsSpace *s, uint32_t bid) {
    JsBranch *b = branch_at(s, bid);
    if (!b) return JS_ERR_ARG;
    for (uint32_t i = 0; i < b->n_units; i++) {
        b->units[i]->holders--;
        real_unref(s, b->units[i]);
    }
    free(b->units);
    free(b);
    s->branches[bid] = NULL;
    return JS_OK;
}

/* ---- costs ----------------------------------------------------------------- */

/* Expected cost of producing r's bytes from its current physical state. */
static double restore_cost(const JsSpace *s, const JsReal *r) {
    const JsCosts *c = &s->costs;
    double n = (double)r->realizer->unit_bytes, cost = 0;
    uint32_t guard = 0;
    for (const JsReal *x = r; x && guard < 1u << 20; x = x->parent_realization, guard++) {
        switch (x->placement) {
        case JS_PLACE_HOT: case JS_PLACE_COLD: return cost + (x == r ? 0 : n * c->copy_ns_per_byte);
        case JS_PLACE_SPILLED: return cost + n * c->spill_read_ns_per_byte;
        case JS_PLACE_COMPRESSED: cost += n * c->decompress_ns_per_byte; break;
        default:
            cost += x->recipe == JS_RECIPE_EDIT ? n * c->copy_ns_per_byte : c->derive_ns;
            if (x->recipe == JS_RECIPE_ROOT) return cost;
        }
    }
    return cost;
}

static double predicted_packed(const JsSpace *s, const JsReal *r) {
    double n = (double)r->realizer->unit_bytes;
    if (r->recipe == JS_RECIPE_EDIT) return (double)r->edit_len + 16.0;
    return n * s->costs.compress_ratio;
}

/* Expected cost if r were rebuilt by recipe from its parent's current state. */
static double recompute_cost(const JsSpace *s, const JsReal *r) {
    double n = (double)r->realizer->unit_bytes;
    double own = r->recipe == JS_RECIPE_EDIT ? n * s->costs.copy_ns_per_byte : s->costs.derive_ns;
    if (!r->parent_realization) return own;
    return own + restore_cost(s, r->parent_realization);
}

/* Expected future reads: every branch holding r may read it again. */
static double expected_reads(const JsReal *r) {
    return (double)(r->holders ? r->holders : 1);
}

static JsAction choose(const JsSpace *s, const JsReal *r, double pressure, bool allow_move) {
    const JsCosts *c = &s->costs;
    double n = (double)r->realizer->unit_bytes;
    double e = expected_reads(r);
    double best = 0;
    JsAction act = JS_ACT_COUNT;   /* keep */
    if (r->placement != JS_PLACE_HOT && r->placement != JS_PLACE_COLD &&
        r->placement != JS_PLACE_COMPRESSED) return JS_ACT_COUNT;
    double held = r->placement == JS_PLACE_COMPRESSED ? (double)r->packed_len : n;
    struct { JsAction a; double now, later, freed; } opt[4];
    int k = 0;
    if (r->placement == JS_PLACE_HOT && allow_move) {
        /* MOVE frees hot-arena bytes, not total residency. */
        opt[k++] = (typeof(opt[0])){ JS_ACT_MOVE, n * c->move_ns_per_byte, 0, n * 0.5 };
    }
    if (r->placement != JS_PLACE_COMPRESSED) {
        double p = predicted_packed(s, r);
        if (p < n)
            /* A delta is decoded against its parent's bytes, so restoring it
             * also pays for producing those. */
            opt[k++] = (typeof(opt[0])){ JS_ACT_COMPRESS, n * c->compress_ns_per_byte,
                                         n * c->decompress_ns_per_byte +
                                         (r->parent_realization ? restore_cost(s, r->parent_realization) : 0),
                                         n - p };
    }
    opt[k++] = (typeof(opt[0])){ JS_ACT_SPILL, n * c->spill_write_ns_per_byte,
                                 n * c->spill_read_ns_per_byte, held };
    opt[k++] = (typeof(opt[0])){ JS_ACT_EVICT, 0, recompute_cost(s, r), held };
    for (int i = 0; i < k; i++) {
        if (opt[i].freed <= 0) continue;
        /* nanoseconds of expected work per byte of residency given back */
        double cost = (opt[i].now + e * opt[i].later) / opt[i].freed;
        if (act == JS_ACT_COUNT || cost < best) { best = cost; act = opt[i].a; }
    }
    /* Keep when holding the bytes is cheaper than the best way to give them up. */
    if (act != JS_ACT_COUNT && best >= pressure * c->retain_ns_per_byte) return JS_ACT_COUNT;
    return act;
}

JsAction js_forge_choose(const JsSpace *s, const JsReal *r, double pressure) {
    return choose(s, r, pressure, true);
}

typedef struct { JsReal *r; double score; } Cand;

static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    return (x->score > y->score) - (x->score < y->score);
}

static uint64_t hot_bytes(const JsSpace *s, Cand *cs, uint32_t n) {
    uint64_t h = 0;
    (void)s;
    for (uint32_t i = 0; i < n; i++)
        if (cs[i].r->placement == JS_PLACE_HOT) h += cs[i].r->realizer->unit_bytes;
    return h;
}

/* Collect every live realization once (through the branches that hold them). */
static uint32_t collect(JsSpace *s, Cand **out) {
    uint32_t cap = 1024, n = 0;
    Cand *cs = malloc(cap * sizeof *cs);
    for (uint32_t k = 0; k < JS_INDEX_BUCKETS && cs; k++)
        for (JsReal *r = s->index[k]; r; r = r->index_next) {
            if (n == cap) {
                Cand *nc = realloc(cs, (cap *= 2) * sizeof *cs);
                if (!nc) { free(cs); cs = NULL; break; }
                cs = nc;
            }
            cs[n].r = r; cs[n].score = 0; n++;
        }
    *out = cs;
    return cs ? n : 0;
}

/* Bring resident bytes (HOT + COLD + COMPRESSED) under budget_bytes and hot
 * bytes under half of it. Candidates are ranked by the expected nanoseconds of
 * future work per byte given back, least-used first on ties. */
int js_forge_enforce(JsSpace *s, uint64_t budget_bytes, JsPolicyReport *rep) {
    Cand *cs;
    uint32_t n = collect(s, &cs);
    if (!cs) return JS_ERR_NOMEM;
    uint64_t hot = hot_bytes(s, cs, n);
    for (uint32_t i = 0; i < n; i++) {
        JsReal *r = cs[i].r;
        cs[i].score = expected_reads(r) * recompute_cost(s, r) / (double)r->realizer->unit_bytes
                      + (double)r->last_use * 1e-6;
    }
    qsort(cs, n, sizeof *cs, cand_cmp);
    int rc = JS_OK;
    for (uint32_t i = 0; i < n && rc == JS_OK; i++) {
        bool over_total = s->stats.resident_bytes > budget_bytes;
        bool over_hot = hot > budget_bytes / 2;
        if (!over_total && !over_hot) break;
        JsReal *r = cs[i].r;
        double pressure = over_total ? 1e9 : 1e3;
        /* A move lowers hot-arena bytes, not total residency. */
        JsAction a = choose(s, r, pressure, !over_total);
        if (rep) rep->considered++;
        if (a == JS_ACT_COUNT) { if (rep) rep->kept++; continue; }
        uint64_t before_hot = r->placement == JS_PLACE_HOT ? r->realizer->unit_bytes : 0;
        switch (a) {
        case JS_ACT_MOVE:     rc = js_real_move(s, r); break;
        case JS_ACT_COMPRESS: rc = js_real_compress(s, r); break;
        case JS_ACT_SPILL:    rc = js_real_spill(s, r); break;
        case JS_ACT_EVICT:    rc = js_real_evict(s, r); break;
        default: break;
        }
        if (rc == JS_OK) {
            hot -= before_hot;
            if (rep) rep->chosen[a]++;
        }
    }
    free(cs);
    return rc;
}

/* ---- calibration ------------------------------------------------------------ */

void js_calibrate(JsSpace *s, const JsRealizer *rz) {
    size_t n = rz->unit_bytes;
    const int reps = 32;
    uint8_t *a = malloc(n), *b = malloc(n), *p = malloc(n + (n / 8) * 8 + 16);
    JsCosts *c = &s->costs;
    rz->derive(NULL, 7, a, n);
    uint64_t t0 = now_ns();
    for (int i = 0; i < reps; i++) { rz->derive(a, (uint64_t)i, b, n); memcpy(a, b, n); }
    uint64_t t1 = now_ns();
    for (int i = 0; i < reps; i++) memcpy(b, a, n), a[i] ^= 1;
    uint64_t t2 = now_ns();
    c->derive_ns = (double)(t1 - t0) / reps - (double)(t2 - t1) / reps;
    if (c->derive_ns < 1) c->derive_ns = 1;
    c->copy_ns_per_byte = (double)(t2 - t1) / reps / (double)n;
    c->move_ns_per_byte = c->copy_ns_per_byte;

    /* Compression of a derive step's delta: what a DERIVE realization packs to. */
    rz->derive(a, 99, b, n);
    size_t len = 0;
    t0 = now_ns();
    for (int i = 0; i < reps; i++) len = pack_delta(b, a, n, p);
    t1 = now_ns();
    for (int i = 0; i < reps; i++) unpack_delta(p, len, a, n, b);
    t2 = now_ns();
    c->compress_ratio = (double)len / (double)n;
    c->compress_ns_per_byte = (double)(t1 - t0) / reps / (double)n;
    c->decompress_ns_per_byte = (double)(t2 - t1) / reps / (double)n;

    t0 = now_ns();
    for (int i = 0; i < reps; i++)
        if (pwrite(s->spill_fd, a, n, (off_t)(s->spill_end + (uint64_t)i * n)) != (ssize_t)n) break;
    fdatasync(s->spill_fd);
    t1 = now_ns();
    for (int i = 0; i < reps; i++)
        if (pread(s->spill_fd, b, n, (off_t)(s->spill_end + (uint64_t)i * n)) != (ssize_t)n) break;
    t2 = now_ns();
    s->spill_end += (uint64_t)reps * n;
    c->spill_write_ns_per_byte = (double)(t1 - t0) / reps / (double)n;
    c->spill_read_ns_per_byte = (double)(t2 - t1) / reps / (double)n;

    /* Price of residency: faulting in fresh memory for someone else. */
    size_t m = n * 64;
    t0 = now_ns();
    uint8_t *f = mmap(NULL, m, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (f != MAP_FAILED) {
        for (size_t i = 0; i < m; i += 4096) f[i] = 1;
        munmap(f, m);
    }
    t1 = now_ns();
    c->retain_ns_per_byte = (double)(t1 - t0) / (double)m;
    free(a); free(b); free(p);
}

/* ---- inspection ------------------------------------------------------------- */

void js_branch_realization(JsSpace *s, uint32_t bid, JsSharedStateRealization *out) {
    memset(out, 0, sizeof *out);
    JsBranch *b = branch_at(s, bid);
    if (!b) return;
    out->semantic_state_id = js_branch_semantic_id(b);
    out->realization_type = b->realizer->type;
    out->parent_realization = b->parent_branch;
    for (uint32_t i = 0; i < b->n_units; i++) {
        JsReal *r = b->units[i];
        if (r->holders > 1) out->n_shared++; else out->n_private++;
        out->resident_bytes += resident_of(r);
        if (r->placement < 6) out->placement_count[r->placement]++;
        out->recompute_cost_ns += recompute_cost(s, r);
        out->transfer_cost_ns += (double)r->realizer->unit_bytes * s->costs.copy_ns_per_byte;
        out->retain_cost_ns += (double)resident_of(r) * s->costs.retain_ns_per_byte;
        out->eviction_cost_ns += expected_reads(r) * recompute_cost(s, r);
    }
}

const char *js_action_name(JsAction a) {
    static const char *n[] = { "reuse", "copy", "reference", "recompute", "move",
                               "compress", "spill", "evict" };
    return a < JS_ACT_COUNT ? n[a] : "keep";
}

const char *js_placement_name(JsPlacement p) {
    static const char *n[] = { "?", "hot", "cold", "compressed", "spilled", "evicted" };
    return p <= JS_PLACE_EVICTED ? n[p] : "?";
}

const char *js_real_type_name(JsRealType t) {
    static const char *n[] = { "?", "latent_checkpoint", "activation_checkpoint", "kv_state",
                               "compiled_features", "world_projection", "cortex_projection",
                               "physical_tensor" };
    return t <= JS_REAL_PHYSICAL_TENSOR ? n[t] : "?";
}
