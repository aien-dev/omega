/*
 * rx_jspace.c -- branch-native shared cognitive state (see rx_jspace.h).
 *
 * Semantics are those of the original host reference. Production mechanics
 * (M20): slab-allocated realizations with {slot, gen} identity, reusable
 * branch slots with generations, a bounded extent allocator for the spill /
 * data file with quarantine of extents a checkpoint may still name, hard
 * limits that fail before any change, one recursive space mutex, and a
 * crash-safe metadata checkpoint (temporary file + fsync + rename) that
 * records recipes and extents, never pointers. Content checks use a 64-bit
 * word hash so the store does not pay SHA-256 on every unit; tests compare
 * full SHA-256 digests outside the store.
 */
#include "rx_jspace.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOCK(s)   pthread_mutex_lock((pthread_mutex_t *)&(s)->mu)
#define UNLOCK(s) pthread_mutex_unlock((pthread_mutex_t *)&(s)->mu)

static int restore_u(JsSpace *s, JsReal *r);
static int release_u(JsSpace *s, uint32_t bid);

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

/* ---- extents in the spill / data file -------------------------------------- */

static int ext_push(JsExtent **v, uint32_t *n, uint32_t *cap, uint32_t at, JsExtent e) {
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 64;
        JsExtent *nv = realloc(*v, nc * sizeof *nv);
        if (!nv) return JS_ERR_NOMEM;
        *v = nv; *cap = nc;
    }
    memmove(*v + at + 1, *v + at, (*n - at) * sizeof **v);
    (*v)[at] = e;
    (*n)++;
    return JS_OK;
}

/* Return an extent to the free list now: sorted insert, coalesce, and give the
 * tail back to the end of the file. */
static void ext_free_now(JsSpace *s, uint64_t off, uint64_t len) {
    if (!len) return;
    uint32_t i = 0;
    while (i < s->n_ext_free && s->ext_free[i].off < off) i++;
    JsExtent *f = s->ext_free;
    bool left = i > 0 && f[i - 1].off + f[i - 1].len == off;
    bool right = i < s->n_ext_free && off + len == f[i].off;
    if (left && right) {
        f[i - 1].len += len + f[i].len;
        memmove(f + i, f + i + 1, (s->n_ext_free - i - 1) * sizeof *f);
        s->n_ext_free--;
        i--;
    } else if (left) {
        f[i - 1].len += len; i--;
    } else if (right) {
        f[i].off = off; f[i].len += len;
    } else if (ext_push(&s->ext_free, &s->n_ext_free, &s->cap_ext_free, i,
                        (JsExtent){ off, len }) != JS_OK) {
        return;   /* out of memory: the extent leaks until the next reopen */
    }
    f = s->ext_free;
    if (f[i].off + f[i].len == s->spill_end) {
        s->spill_end = f[i].off;
        memmove(f + i, f + i + 1, (s->n_ext_free - i - 1) * sizeof *f);
        s->n_ext_free--;
    }
}

/* Freed extent. A durable checkpoint may still name it, so it waits for the
 * next commit before it can be written again. */
static void ext_release(JsSpace *s, uint64_t off, uint64_t len) {
    if (!len) return;
    if (!s->durable) { ext_free_now(s, off, len); return; }
    if (ext_push(&s->ext_pending, &s->n_ext_pending, &s->cap_ext_pending, s->n_ext_pending,
                 (JsExtent){ off, len }) != JS_OK) {
        /* cannot quarantine: leak it rather than risk overwriting named bytes */
    }
}

/* First fit, else grow the file within max_spill_bytes. */
static int ext_alloc(JsSpace *s, uint64_t len, uint64_t *off) {
    for (uint32_t i = 0; i < s->n_ext_free; i++) {
        JsExtent *e = &s->ext_free[i];
        if (e->len < len) continue;
        *off = e->off;
        e->off += len; e->len -= len;
        if (!e->len) {
            memmove(e, e + 1, (s->n_ext_free - i - 1) * sizeof *e);
            s->n_ext_free--;
        }
        return JS_OK;
    }
    if (s->limits.max_spill_bytes && s->spill_end + len > s->limits.max_spill_bytes)
        return JS_ERR_FULL;
    *off = s->spill_end;
    s->spill_end += len;
    return JS_OK;
}


/* ---- realization slab ------------------------------------------------------ */

static JsReal *slab_at(const JsSpace *s, uint32_t slot) {
    if (slot >= s->real_hw) return NULL;
    JsReal *c = s->slab[slot / JS_SLAB_CHUNK];
    return c ? &c[slot % JS_SLAB_CHUNK] : NULL;
}

/* Make sure the chunk holding `slot` exists. */
static int slab_chunk(JsSpace *s, uint32_t slot) {
    uint32_t k = slot / JS_SLAB_CHUNK;
    if (k >= s->slab_chunks) return JS_ERR_FULL;
    if (!s->slab[k]) {
        JsReal *c = calloc(JS_SLAB_CHUNK, sizeof *c);
        if (!c) return JS_ERR_NOMEM;
        for (uint32_t i = 0; i < JS_SLAB_CHUNK; i++) {
            c[i].slot = k * JS_SLAB_CHUNK + i;
            c[i].gen = s->real_gen_floor;
        }
        s->slab[k] = c;
    }
    return JS_OK;
}

static JsReal *slab_take(JsSpace *s) {
    JsReal *r = s->real_free;
    if (r) {
        s->real_free = r->free_next;
    } else {
        if (s->real_hw >= s->limits.max_reals) return NULL;
        if (slab_chunk(s, s->real_hw) != JS_OK) return NULL;
        r = &s->slab[s->real_hw / JS_SLAB_CHUNK][s->real_hw % JS_SLAB_CHUNK];
        s->real_hw++;
    }
    uint32_t slot = r->slot, gen = r->gen;
    memset(r, 0, sizeof *r);
    r->slot = slot; r->gen = gen;
    r->patch_off = UINT64_MAX;
    return r;
}

static void slab_give(JsSpace *s, JsReal *r) {
    uint32_t slot = r->slot, gen = r->gen + 1;
    memset(r, 0, sizeof *r);
    r->slot = slot; r->gen = gen;
    if (gen + 1 > s->real_gen_floor) s->real_gen_floor = gen + 1;
    r->free_next = s->real_free;
    s->real_free = r;
}

/* ---- limits ------------------------------------------------------------------ */

static int room(const JsSpace *s, uint32_t reals, uint64_t resident) {
    if (reals && s->stats.live_reals + reals > s->limits.max_reals) return JS_ERR_FULL;
    if (resident && s->limits.max_resident_bytes &&
        s->stats.resident_bytes + resident > s->limits.max_resident_bytes) return JS_ERR_FULL;
    return JS_OK;
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
    if (r->placement == JS_PLACE_SPILLED) ext_release(s, r->spill_off, r->realizer->unit_bytes);
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
        if (r->patch_off != UINT64_MAX) ext_release(s, r->patch_off, r->edit_len);
        slab_give(s, r);
        s->stats.live_reals--;
        r = parent;
    }
}

/* Callers check room() first, so NULL here means out of memory. */
static JsReal *real_new(JsSpace *s, const JsRealizer *rz, JsSemId id, JsReal *parent,
                        JsRecipeKind kind) {
    JsReal *r = slab_take(s);
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

static int restore_u(JsSpace *s, JsReal *r) {
    if (r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD) return JS_OK;
    int rc = room(s, 0, r->realizer->unit_bytes - resident_of(r));
    if (rc) return rc;
    uint8_t *buf = malloc(r->realizer->unit_bytes);
    if (!buf) return JS_ERR_NOMEM;
    rc = materialize(s, r, buf);
    if (rc != JS_OK) { free(buf); return rc; }
    drop_physical(s, r);
    set_hot(s, r, buf);
    return JS_OK;
}

/* ---- physical operations -------------------------------------------------- */

static int move_u(JsSpace *s, JsReal *r) {
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

static int compress_u(JsSpace *s, JsReal *r) {
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

static int spill_u(JsSpace *s, JsReal *r) {
    size_t n = r->realizer->unit_bytes;
    uint8_t *cur = malloc(n);
    if (!cur) return JS_ERR_NOMEM;
    uint64_t off = 0;
    int rc = materialize(s, r, cur);
    if (rc == JS_OK) rc = ext_alloc(s, n, &off);
    if (rc == JS_OK) {
        ssize_t put = pwrite(s->spill_fd, cur, n, (off_t)off);
        if (put != (ssize_t)n) { rc = JS_ERR_IO; ext_free_now(s, off, n); }
    }
    free(cur);
    if (rc != JS_OK) return rc;
    drop_physical(s, r);
    r->spill_off = off;
    r->placement = JS_PLACE_SPILLED;
    charge(s, r, +1);
    s->stats.actions[JS_ACT_SPILL]++;
    return JS_OK;
}

static int evict_u(JsSpace *s, JsReal *r) {
    if (r->placement == JS_PLACE_EVICTED) return JS_OK;
    drop_physical(s, r);
    s->stats.actions[JS_ACT_EVICT]++;
    return JS_OK;
}

/* ---- space ---------------------------------------------------------------- */

void js_limits_default(JsLimits *lim) {
    lim->max_reals = 1u << 24;
    lim->max_branches = JS_MAX_BRANCHES;
    lim->max_spill_bytes = 0;
    lim->max_resident_bytes = 0;
}

/* Everything but the files. */
static int space_setup(JsSpace *s, const JsLimits *lim) {
    memset(s, 0, sizeof *s);
    s->spill_fd = -1;
    if (lim) s->limits = *lim; else js_limits_default(&s->limits);
    if (!s->limits.max_reals || !s->limits.max_branches ||
        s->limits.max_branches > JS_MAX_BRANCHES) return JS_ERR_ARG;
    s->slab_chunks = (s->limits.max_reals + JS_SLAB_CHUNK - 1) / JS_SLAB_CHUNK;
    s->slab = calloc(s->slab_chunks, sizeof *s->slab);
    if (!s->slab) return JS_ERR_NOMEM;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    int prc = pthread_mutex_init(&s->mu, &a);
    pthread_mutexattr_destroy(&a);
    if (prc) { free(s->slab); s->slab = NULL; return JS_ERR_NOMEM; }
    s->mu_ready = true;
    s->reuse_enabled = true;
    return JS_OK;
}

int js_space_init_limits(JsSpace *s, const char *spill_path, const JsLimits *lim) {
    int rc = space_setup(s, lim);
    if (rc) return rc;
    s->spill_fd = open(spill_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (s->spill_fd < 0) { js_space_destroy(s); return JS_ERR_IO; }
    unlink(spill_path);   /* anonymous from here on */
    return JS_OK;
}

int js_space_init(JsSpace *s, const char *spill_path) {
    return js_space_init_limits(s, spill_path, NULL);
}

void js_space_destroy(JsSpace *s) {
    if (s->mu_ready) LOCK(s);
    for (uint32_t i = 0; i < s->n_branches; i++)
        if (s->branches[i]) release_u(s, i);
    if (s->spill_fd >= 0) close(s->spill_fd);
    s->spill_fd = -1;
    free(s->cache_bytes);
    s->cache_bytes = NULL;
    s->cache_r = NULL;
    s->cache_len = 0;
    for (uint32_t k = 0; s->slab && k < s->slab_chunks; k++) {
        JsReal *c = s->slab[k];
        /* Realizations no branch holds (none after the releases above, unless
         * a load failed half way) still own heap buffers. */
        for (uint32_t i = 0; c && i < JS_SLAB_CHUNK; i++) {
            free(c[i].bytes); free(c[i].packed); free(c[i].edit_bytes);
        }
        free(c);
    }
    free(s->slab); s->slab = NULL;
    free(s->ext_free); s->ext_free = NULL;
    free(s->ext_pending); s->ext_pending = NULL;
    free(s->dir); s->dir = NULL;
    if (s->mu_ready) {
        UNLOCK(s);
        pthread_mutex_destroy(&s->mu);
        s->mu_ready = false;
    }
}

static JsBranch *branch_at(JsSpace *s, uint32_t id) {
    return id < s->n_branches ? s->branches[id] : NULL;
}

static JsBranch *branch_ref_at(JsSpace *s, JsBranchRef ref) {
    JsBranch *b = branch_at(s, ref.id);
    return b && b->gen == ref.gen ? b : NULL;
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

/* A released slot is reused before a new one is taken; its generation moved
 * on at release, so old references to it are stale. */
static int branch_room(const JsSpace *s) {
    return s->n_free_branch || s->n_branches < s->limits.max_branches ? JS_OK : JS_ERR_FULL;
}

static int branch_alloc(JsSpace *s, const JsRealizer *rz, uint32_t *out) {
    if (branch_room(s)) return JS_ERR_FULL;
    JsBranch *b = calloc(1, sizeof *b);
    if (!b) return JS_ERR_NOMEM;
    uint32_t id = s->n_free_branch ? s->free_branch[--s->n_free_branch] : s->n_branches++;
    b->branch_id = id;
    b->realizer = rz;
    b->gen = s->branch_gen[id];
    b->home = s->local_home;
    b->home.locality = JS_HOME_LOCAL;
    s->branches[id] = b;
    *out = id;
    return JS_OK;
}

/* A new realization's bytes are ready in `bytes`: fix its content check,
 * make it HOT and file it in the reuse index. */
static void real_publish(JsSpace *s, JsReal *r, uint8_t *bytes) {
    content_check(bytes, r->realizer->unit_bytes, r->content);
    set_hot(s, r, bytes);
    index_insert(s, r);
}

static int root_u(JsSpace *s, const JsRealizer *rz, uint64_t seed, uint32_t *out) {
    if (!rz || !rz->derive || !rz->unit_bytes) return JS_ERR_ARG;
    JsSemId sid = js_sem_root(seed);
    JsReal *r = s->reuse_enabled ? index_find(s, &sid, rz->type) : NULL;
    int rc = branch_room(s);
    if (!rc && !r) rc = room(s, 1, rz->unit_bytes);
    if (rc) return rc;
    uint32_t id;
    if ((rc = branch_alloc(s, rz, &id))) return rc;
    JsBranch *b = s->branches[id];
    b->parent_branch = UINT32_MAX;
    b->common_ancestor = id;
    if (r) {
        s->stats.actions[JS_ACT_REUSE]++;
    } else {
        uint8_t *bytes = malloc(rz->unit_bytes);
        if (!bytes || !(r = real_new(s, rz, sid, NULL, JS_RECIPE_ROOT))) {
            free(bytes); release_u(s, id); return JS_ERR_NOMEM;
        }
        r->token = seed;
        rz->derive(NULL, seed, bytes, rz->unit_bytes);
        s->stats.derives++;
        real_publish(s, r, bytes);
    }
    rc = branch_push(b, r);
    if (rc) { if (!r->refs) { r->refs = 1; real_unref(s, r); } release_u(s, id); return rc; }
    *out = id;
    return rc;
}

static int fork_u(JsSpace *s, uint32_t parent, uint32_t *out) {
    JsBranch *p = branch_at(s, parent);
    if (!p) return JS_ERR_ARG;
    if (p->home.locality != JS_HOME_LOCAL) return JS_ERR_REMOTE;
    uint32_t id;
    int rc = branch_alloc(s, p->realizer, &id);
    if (rc) return rc;
    JsBranch *b = s->branches[id];
    b->parent_branch = parent;
    b->common_ancestor = p->common_ancestor;
    b->divergence_point = p->n_units;
    b->units = malloc((p->n_units + 64) * sizeof *b->units);
    if (!b->units) { release_u(s, id); return JS_ERR_NOMEM; }
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
    return restore_u(s, r);
}

static bool is_resident_raw(const JsReal *r) {
    return r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD;
}

static int derive_u(JsSpace *s, uint32_t bid, uint64_t token) {
    JsBranch *b = branch_at(s, bid);
    if (!b || !b->n_units) return JS_ERR_ARG;
    if (b->home.locality != JS_HOME_LOCAL) return JS_ERR_REMOTE;
    if (b->frozen) return JS_ERR_FROZEN;
    JsReal *prev = b->units[b->n_units - 1];
    JsSemId sid = js_sem_derive(&prev->semantic_state_id, token);
    JsReal *r = s->reuse_enabled ? index_find(s, &sid, b->realizer->type) : NULL;
    if (r) {
        s->stats.actions[JS_ACT_REUSE]++;
        return branch_push(b, r);
    }
    size_t n = b->realizer->unit_bytes;
    int rc = room(s, 1, n + (is_resident_raw(prev) ? 0 : n - resident_of(prev)));
    if (rc) return rc;
    rc = ensure_hot(s, prev);
    if (rc) return rc;
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

static int edit_u(JsSpace *s, uint32_t bid, uint32_t idx, uint32_t off,
                   const uint8_t *patch, uint32_t len) {
    JsBranch *b = branch_at(s, bid);
    if (!b || idx >= b->n_units || !len || off + (uint64_t)len > b->realizer->unit_bytes)
        return JS_ERR_ARG;
    if (b->home.locality != JS_HOME_LOCAL) return JS_ERR_REMOTE;
    if (b->frozen) return JS_ERR_FROZEN;
    JsReal *old = b->units[idx];
    JsSemId sid = js_sem_edit(&old->semantic_state_id, off, patch, len);
    JsReal *r = s->reuse_enabled ? index_find(s, &sid, b->realizer->type) : NULL;
    if (!r) {
        bool take = old->refs == 1 && is_resident_raw(old);
        int rc = room(s, 1, take ? 0 : b->realizer->unit_bytes);
        if (rc) return rc;
    }
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

static int read_u(JsSpace *s, uint32_t bid, uint32_t idx, uint8_t *out) {
    JsBranch *b = branch_at(s, bid);
    if (!b || idx >= b->n_units) return JS_ERR_ARG;
    JsReal *r = b->units[idx];
    r->last_use = ++s->clock;
    r->reads++;
    if (r->placement == JS_PLACE_HOT || r->placement == JS_PLACE_COLD) {
        memcpy(out, r->bytes, b->realizer->unit_bytes);
        return JS_OK;
    }
    /* The authoritative bytes live on another machine: fetching them is
     * Fabric's job, and Fabric does not exist in this build. */
    if (b->home.locality == JS_HOME_REMOTE_OWNED) return JS_ERR_REMOTE;
    return materialize(s, r, out);
}

static int digest_u(JsSpace *s, uint32_t bid, uint8_t out[32]) {
    JsBranch *b = branch_at(s, bid);
    if (!b) return JS_ERR_ARG;
    size_t n = b->realizer->unit_bytes;
    uint8_t *buf = malloc(n);
    if (!buf) return JS_ERR_NOMEM;
    sha256_ctx c;
    sha256_init(&c);
    int rc = JS_OK;
    for (uint32_t i = 0; i < b->n_units && rc == JS_OK; i++) {
        rc = read_u(s, bid, i, buf);
        if (rc == JS_OK) sha256_update(&c, buf, n);
    }
    sha256_final(&c, out);
    free(buf);
    return rc;
}

static int release_u(JsSpace *s, uint32_t bid) {
    JsBranch *b = branch_at(s, bid);
    if (!b) return JS_ERR_ARG;
    for (uint32_t i = 0; i < b->n_units; i++) {
        b->units[i]->holders--;
        real_unref(s, b->units[i]);
    }
    free(b->units);
    free(b);
    s->branches[bid] = NULL;
    s->branch_gen[bid]++;                     /* every old reference is now stale */
    s->free_branch[s->n_free_branch++] = bid;
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
static int enforce_u(JsSpace *s, uint64_t budget_bytes, JsPolicyReport *rep) {
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
        case JS_ACT_MOVE:     rc = move_u(s, r); break;
        case JS_ACT_COMPRESS: rc = compress_u(s, r); break;
        case JS_ACT_SPILL:    rc = spill_u(s, r); break;
        case JS_ACT_EVICT:    rc = evict_u(s, r); break;
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

static void calibrate_u(JsSpace *s, const JsRealizer *rz) {
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
    /* Scratch past every named extent: give the space back. */
    if (ftruncate(s->spill_fd, (off_t)s->spill_end)) { /* keep the measurement */ }
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

static void realization_u(JsSpace *s, uint32_t bid, JsSharedStateRealization *out) {
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

/* ---- public entry points: one recursive space mutex ------------------------ */

#define LOCKED_INT(call) do { LOCK(s); int rc_ = (call); UNLOCK(s); return rc_; } while (0)

int js_real_restore(JsSpace *s, JsReal *r)  { LOCKED_INT(restore_u(s, r)); }
int js_real_move(JsSpace *s, JsReal *r)     { LOCKED_INT(move_u(s, r)); }
int js_real_compress(JsSpace *s, JsReal *r) { LOCKED_INT(compress_u(s, r)); }
int js_real_spill(JsSpace *s, JsReal *r)    { LOCKED_INT(spill_u(s, r)); }
int js_real_evict(JsSpace *s, JsReal *r)    { LOCKED_INT(evict_u(s, r)); }

int js_branch_root(JsSpace *s, const JsRealizer *rz, uint64_t seed, uint32_t *out) {
    LOCKED_INT(root_u(s, rz, seed, out));
}
int js_branch_fork(JsSpace *s, uint32_t parent, uint32_t *out) { LOCKED_INT(fork_u(s, parent, out)); }
int js_branch_derive(JsSpace *s, uint32_t b, uint64_t token) { LOCKED_INT(derive_u(s, b, token)); }
int js_branch_edit(JsSpace *s, uint32_t b, uint32_t idx, uint32_t off,
                   const uint8_t *patch, uint32_t len) {
    LOCKED_INT(edit_u(s, b, idx, off, patch, len));
}
int js_branch_read(JsSpace *s, uint32_t b, uint32_t idx, uint8_t *out) {
    LOCKED_INT(read_u(s, b, idx, out));
}
int js_branch_content_digest(JsSpace *s, uint32_t b, uint8_t out[32]) {
    LOCKED_INT(digest_u(s, b, out));
}
int js_branch_release(JsSpace *s, uint32_t b) { LOCKED_INT(release_u(s, b)); }
int js_forge_enforce(JsSpace *s, uint64_t budget_bytes, JsPolicyReport *rep) {
    LOCKED_INT(enforce_u(s, budget_bytes, rep));
}

void js_branch_realization(JsSpace *s, uint32_t b, JsSharedStateRealization *out) {
    LOCK(s); realization_u(s, b, out); UNLOCK(s);
}

void js_calibrate(JsSpace *s, const JsRealizer *r) {
    LOCK(s); calibrate_u(s, r); UNLOCK(s);
}

/* ---- stable references, ownership, staging, placement ---------------------- */

int js_branch_ref(JsSpace *s, uint32_t id, JsBranchRef *out) {
    LOCK(s);
    JsBranch *b = branch_at(s, id);
    if (b) *out = (JsBranchRef){ id, b->gen };
    UNLOCK(s);
    return b ? JS_OK : JS_ERR_ARG;
}

int js_branch_check(JsSpace *s, JsBranchRef ref) {
    LOCK(s);
    int rc = branch_ref_at(s, ref) ? JS_OK : JS_ERR_STALE;
    UNLOCK(s);
    return rc;
}

int js_real_id(JsSpace *s, uint32_t bid, uint32_t idx, JsRealId *out) {
    LOCK(s);
    JsBranch *b = branch_at(s, bid);
    int rc = b && idx < b->n_units ? JS_OK : JS_ERR_ARG;
    if (!rc) *out = (JsRealId){ b->units[idx]->slot, b->units[idx]->gen };
    UNLOCK(s);
    return rc;
}

JsReal *js_real_lookup(JsSpace *s, JsRealId id) {
    LOCK(s);
    JsReal *r = slab_at(s, id.slot);
    if (r && (r->gen != id.gen || !r->realizer)) r = NULL;
    UNLOCK(s);
    return r;
}

int js_branch_fork_staged(JsSpace *s, JsBranchRef parent, uint32_t owner, JsBranchRef *out) {
    LOCK(s);
    uint32_t id = 0;
    int rc = branch_ref_at(s, parent) ? fork_u(s, parent.id, &id) : JS_ERR_STALE;
    if (!rc) {
        JsBranch *b = s->branches[id];
        b->staged = true;
        b->owner = owner;
        *out = (JsBranchRef){ id, b->gen };
    }
    UNLOCK(s);
    return rc;
}

int js_branch_seal(JsSpace *s, JsBranchRef ref) {
    LOCK(s);
    JsBranch *b = branch_ref_at(s, ref);
    if (b) b->staged = false;
    UNLOCK(s);
    return b ? JS_OK : JS_ERR_STALE;
}

int js_branch_release_ref(JsSpace *s, JsBranchRef ref, uint32_t subject) {
    LOCK(s);
    JsBranch *b = branch_ref_at(s, ref);
    int rc = !b ? JS_ERR_STALE
           : (b->owner && b->owner != subject) ? JS_ERR_OWNER
           : release_u(s, ref.id);
    UNLOCK(s);
    return rc;
}

uint32_t js_space_reclaim_staged(JsSpace *s) {
    LOCK(s);
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->n_branches; i++)
        if (s->branches[i] && s->branches[i]->staged) { release_u(s, i); n++; }
    UNLOCK(s);
    return n;
}

int js_branch_set_owner(JsSpace *s, JsBranchRef ref, uint32_t owner) {
    LOCK(s);
    JsBranch *b = branch_ref_at(s, ref);
    if (b) b->owner = owner;
    UNLOCK(s);
    return b ? JS_OK : JS_ERR_STALE;
}

int js_branch_set_home(JsSpace *s, JsBranchRef ref, const JsHome *home) {
    if (!home || home->locality > JS_HOME_REPLICA) return JS_ERR_ARG;
    LOCK(s);
    JsBranch *b = branch_ref_at(s, ref);
    if (b) b->home = *home;
    UNLOCK(s);
    return b ? JS_OK : JS_ERR_STALE;
}

int js_branch_home(JsSpace *s, JsBranchRef ref, JsHome *out) {
    LOCK(s);
    JsBranch *b = branch_ref_at(s, ref);
    if (b) *out = b->home;
    UNLOCK(s);
    return b ? JS_OK : JS_ERR_STALE;
}

/* ---- durable checkpoint ------------------------------------------------------
 *
 * jspace.meta, little-endian, all fixed-width fields:
 *   header (128 B): "OMJSPC01" | u32 version=1 | u32 header_bytes=128 |
 *     u64 commit_seq | u64 body_len | u32 n_types | u32 n_reals |
 *     u32 n_branch_slots | u32 n_branches | u64 spill_end |
 *     u32 real_gen_floor | u32 reserved | sha256(body) | sha256(header[0..96))
 *   body:
 *     n_types   x { u32 type | u32 0 | u64 unit_bytes }
 *     n_branch_slots x u32 slot generation
 *     n_reals   x 128 B { u32 slot | u32 gen | u32 parent_slot | u32 parent_gen |
 *                         u32 type | u32 recipe | u64 token | u32 edit_off |
 *                         u32 edit_len | u64 patch_off | u32 placement | u32 0 |
 *                         u64 spill_off | semid[32] | content[32] }
 *     n_branches x { u32 id | u32 gen | u32 parent | u32 common_ancestor |
 *                    u32 divergence | u32 frozen | u32 owner | u32 locality |
 *                    u32 type | u32 n_units | machine[32] |
 *                    n_units x { u32 slot | u32 gen } }
 * No pointer is stored. Placement is SPILLED (bytes at spill_off in
 * jspace.data) or EVICTED (rebuild from the recipe). EDIT patches live in
 * jspace.data at patch_off. Staged branches and realizations only they reach
 * are left out. */

#define JS_META_MAGIC   "OMJSPC01"
#define JS_META_VERSION 1u
#define JS_META_HDR     128u
#define JS_META_REAL    128u
#define JS_META_BRANCH  72u

typedef struct { uint8_t *p; size_t n, cap; bool oom; } Buf;

static void buf_put(Buf *b, const void *v, size_t n) {
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t *np = realloc(b->p, c);
        if (!np) { b->oom = true; return; }
        b->p = np; b->cap = c;
    }
    memcpy(b->p + b->n, v, n);
    b->n += n;
}
static void le32(uint8_t *o, uint32_t v) { for (int i = 0; i < 4; i++) o[i] = (uint8_t)(v >> (8 * i)); }
static void le64(uint8_t *o, uint64_t v) { for (int i = 0; i < 8; i++) o[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t rd32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static void put32(Buf *b, uint32_t v) { uint8_t o[4]; le32(o, v); buf_put(b, o, 4); }
static void put64(Buf *b, uint64_t v) { uint8_t o[8]; le64(o, v); buf_put(b, o, 8); }

static char *path_in(const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return JS_ERR_IO; }
        p += w; n -= (size_t)w;
    }
    return JS_OK;
}

/* Mark every realization a persisted (non-staged) branch reaches. */
static uint8_t *mark_persisted(JsSpace *s) {
    uint8_t *m = calloc(s->real_hw ? s->real_hw : 1, 1);
    if (!m) return NULL;
    for (uint32_t i = 0; i < s->n_branches; i++) {
        JsBranch *b = s->branches[i];
        if (!b || b->staged) continue;
        for (uint32_t u = 0; u < b->n_units; u++)
            for (JsReal *r = b->units[u]; r && !m[r->slot]; r = r->parent_realization)
                m[r->slot] = 1;
    }
    return m;
}

static int commit_u(JsSpace *s) {
    if (!s->durable) return JS_ERR_ARG;
    uint8_t *mark = mark_persisted(s);
    if (!mark) return JS_ERR_NOMEM;
    int rc = JS_OK;
    /* 1. Data: every persisted EDIT patch gets an extent. */
    for (uint32_t k = 0; k < s->real_hw && rc == JS_OK; k++) {
        JsReal *r = slab_at(s, k);
        if (!mark[k] || r->recipe != JS_RECIPE_EDIT || r->patch_off != UINT64_MAX) continue;
        uint64_t off;
        if ((rc = ext_alloc(s, r->edit_len, &off)) != JS_OK) break;
        if (pwrite(s->spill_fd, r->edit_bytes, r->edit_len, (off_t)off) != (ssize_t)r->edit_len) {
            ext_free_now(s, off, r->edit_len);
            rc = JS_ERR_IO;
            break;
        }
        r->patch_off = off;
    }
    if (rc == JS_OK && fdatasync(s->spill_fd)) rc = JS_ERR_IO;
    if (rc != JS_OK) { free(mark); return rc; }

    /* 2. Metadata body. Types are the distinct realizers in use. */
    const JsRealizer *types[64];
    uint32_t n_types = 0, n_reals = 0, n_br = 0;
    Buf body = { 0 };
    for (uint32_t i = 0; i < s->n_branches; i++) {
        JsBranch *b = s->branches[i];
        if (!b || b->staged) continue;
        n_br++;
        bool seen = false;
        for (uint32_t t = 0; t < n_types; t++) if (types[t] == b->realizer) seen = true;
        if (!seen) {
            if (n_types == 64) { free(mark); return JS_ERR_FULL; }
            types[n_types++] = b->realizer;
        }
    }
    for (uint32_t t = 0; t < n_types; t++) {
        put32(&body, (uint32_t)types[t]->type); put32(&body, 0);
        put64(&body, (uint64_t)types[t]->unit_bytes);
    }
    for (uint32_t i = 0; i < s->n_branches; i++) {
        JsBranch *b = s->branches[i];
        /* A staged occupant is not persisted: its slot reopens free, one
         * generation on, so a reference to it stays stale. */
        put32(&body, b && b->staged ? s->branch_gen[i] + 1 : s->branch_gen[i]);
    }
    for (uint32_t k = 0; k < s->real_hw; k++) {
        if (!mark[k]) continue;
        JsReal *r = slab_at(s, k);
        n_reals++;
        JsReal *p = r->parent_realization;
        put32(&body, r->slot); put32(&body, r->gen);
        put32(&body, p ? p->slot : UINT32_MAX); put32(&body, p ? p->gen : 0);
        put32(&body, (uint32_t)r->realization_type); put32(&body, (uint32_t)r->recipe);
        put64(&body, r->token);
        put32(&body, r->edit_off); put32(&body, r->edit_len);
        put64(&body, r->patch_off);
        bool sp = r->placement == JS_PLACE_SPILLED;
        put32(&body, sp ? JS_PLACE_SPILLED : JS_PLACE_EVICTED); put32(&body, 0);
        put64(&body, sp ? r->spill_off : 0);
        buf_put(&body, r->semantic_state_id.b, 32);
        buf_put(&body, r->content, 32);
    }
    free(mark);
    for (uint32_t i = 0; i < s->n_branches; i++) {
        JsBranch *b = s->branches[i];
        if (!b || b->staged) continue;
        JsBranch *pb = branch_at(s, b->parent_branch);
        put32(&body, i); put32(&body, b->gen);
        put32(&body, pb && !pb->staged ? b->parent_branch : UINT32_MAX);
        put32(&body, b->common_ancestor); put32(&body, b->divergence_point);
        put32(&body, b->frozen); put32(&body, b->owner); put32(&body, b->home.locality);
        put32(&body, (uint32_t)b->realizer->type); put32(&body, b->n_units);
        buf_put(&body, b->home.machine, JS_MACHINE_ID_BYTES);
        for (uint32_t u = 0; u < b->n_units; u++) {
            put32(&body, b->units[u]->slot); put32(&body, b->units[u]->gen);
        }
    }
    if (body.oom) { free(body.p); return JS_ERR_NOMEM; }

    /* 3. Header. */
    uint8_t h[JS_META_HDR];
    memset(h, 0, sizeof h);
    memcpy(h, JS_META_MAGIC, 8);
    le32(h + 8, JS_META_VERSION); le32(h + 12, JS_META_HDR);
    le64(h + 16, s->commit_seq + 1); le64(h + 24, body.n);
    le32(h + 32, n_types); le32(h + 36, n_reals);
    le32(h + 40, s->n_branches); le32(h + 44, n_br);
    le64(h + 48, s->spill_end);
    uint32_t floor = s->real_gen_floor;
    for (uint32_t k = 0; k < s->real_hw; k++) {
        JsReal *r = slab_at(s, k);
        if (r->gen + 1 > floor) floor = r->gen + 1;
    }
    le32(h + 56, floor);
    sha256_hash(body.p ? body.p : (const uint8_t *)"", body.n, h + 64);
    sha256_hash(h, 96, h + 96);

    /* 4. Temporary file, fsync, atomic rename, directory fsync. */
    char *tmp = path_in(s->dir, "jspace.meta.tmp"), *fin = path_in(s->dir, "jspace.meta");
    int fd = tmp && fin ? open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    rc = fd < 0 ? JS_ERR_IO : JS_OK;
    if (rc == JS_OK) rc = write_all(fd, h, sizeof h);
    if (rc == JS_OK && body.n) rc = write_all(fd, body.p, body.n);
    if (rc == JS_OK && fsync(fd)) rc = JS_ERR_IO;
    if (fd >= 0) close(fd);
    if (rc == JS_OK && rename(tmp, fin)) rc = JS_ERR_IO;
    if (rc == JS_OK) {
        int dfd = open(s->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd < 0 || fsync(dfd)) rc = JS_ERR_IO;
        if (dfd >= 0) close(dfd);
    }
    if (rc != JS_OK && tmp) unlink(tmp);
    free(tmp); free(fin); free(body.p);
    if (rc != JS_OK) return rc;

    /* 5. The new checkpoint names none of the quarantined extents. */
    s->commit_seq++;
    s->real_gen_floor = floor;
    for (uint32_t i = 0; i < s->n_ext_pending; i++)
        ext_free_now(s, s->ext_pending[i].off, s->ext_pending[i].len);
    s->n_ext_pending = 0;
    return JS_OK;
}

int js_space_commit(JsSpace *s) { LOCKED_INT(commit_u(s)); }

static int ext_cmp(const void *a, const void *b) {
    const JsExtent *x = a, *y = b;
    return (x->off > y->off) - (x->off < y->off);
}

/* Validate and load a checkpoint into a freshly set-up space. Any
 * inconsistency is JS_ERR_CORRUPT; the caller tears the space down. */
static int load_u(JsSpace *s, const uint8_t *f, size_t fn,
                  const JsRealizer *const *rzs, uint32_t n_rz) {
    if (fn < JS_META_HDR || memcmp(f, JS_META_MAGIC, 8)) return JS_ERR_CORRUPT;
    uint8_t d[32];
    sha256_hash(f, 96, d);
    if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;
    if (rd32(f + 8) != JS_META_VERSION || rd32(f + 12) != JS_META_HDR) return JS_ERR_CORRUPT;
    uint64_t body_len = rd64(f + 24);
    if (body_len != fn - JS_META_HDR) return JS_ERR_CORRUPT;
    const uint8_t *p = f + JS_META_HDR, *end = p + body_len;
    sha256_hash(p, body_len, d);
    if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;
    uint32_t n_types = rd32(f + 32), n_reals = rd32(f + 36);
    uint32_t n_slots = rd32(f + 40), n_br = rd32(f + 44);
    uint64_t spill_end = rd64(f + 48);
    s->commit_seq = rd64(f + 16);
    s->real_gen_floor = rd32(f + 56);
    if (n_types > 64 || n_slots > s->limits.max_branches || n_br > n_slots ||
        n_reals > s->limits.max_reals) return JS_ERR_CORRUPT;
    if (s->limits.max_spill_bytes && spill_end > s->limits.max_spill_bytes) return JS_ERR_FULL;
    struct stat st;
    if (fstat(s->spill_fd, &st) || (uint64_t)st.st_size < spill_end) return JS_ERR_CORRUPT;
    s->spill_end = spill_end;

    /* Types bind to the realizers the caller supplied. */
    const JsRealizer *types[64];
    if ((size_t)(end - p) < (size_t)n_types * 16) return JS_ERR_CORRUPT;
    for (uint32_t t = 0; t < n_types; t++, p += 16) {
        uint32_t ty = rd32(p);
        uint64_t ub = rd64(p + 8);
        types[t] = NULL;
        for (uint32_t i = 0; i < n_rz; i++)
            if (rzs[i] && (uint32_t)rzs[i]->type == ty && rzs[i]->unit_bytes == ub) types[t] = rzs[i];
        if (!types[t]) return JS_ERR_ARG;   /* caller did not supply this representation */
    }
    if ((size_t)(end - p) < (size_t)n_slots * 4) return JS_ERR_CORRUPT;
    for (uint32_t i = 0; i < n_slots; i++, p += 4) s->branch_gen[i] = rd32(p);
    s->n_branches = n_slots;

    /* Realizations: place each at its own slot, link parents in a second pass. */
    if ((size_t)(end - p) < (size_t)n_reals * JS_META_REAL) return JS_ERR_CORRUPT;
    uint32_t *parent_slot = malloc((n_reals ? n_reals : 1) * sizeof *parent_slot);
    uint32_t *parent_gen = malloc((n_reals ? n_reals : 1) * sizeof *parent_gen);
    JsReal **loaded = malloc((n_reals ? n_reals : 1) * sizeof *loaded);
    JsExtent *ext = malloc((2 * (size_t)n_reals + 1) * sizeof *ext);
    uint32_t n_ext = 0;
    int rc = parent_slot && parent_gen && loaded && ext ? JS_OK : JS_ERR_NOMEM;
    for (uint32_t i = 0; i < n_reals && rc == JS_OK; i++, p += JS_META_REAL) {
        uint32_t slot = rd32(p), gen = rd32(p + 4), ty = rd32(p + 16), rec = rd32(p + 20);
        const JsRealizer *rz = NULL;
        for (uint32_t t = 0; t < n_types; t++) if ((uint32_t)types[t]->type == ty) rz = types[t];
        if (!rz || slot >= s->limits.max_reals || gen >= s->real_gen_floor ||
            rec < JS_RECIPE_ROOT || rec > JS_RECIPE_EDIT) { rc = JS_ERR_CORRUPT; break; }
        if ((rc = slab_chunk(s, slot)) != JS_OK) { rc = rc == JS_ERR_FULL ? JS_ERR_CORRUPT : rc; break; }
        JsReal *r = &s->slab[slot / JS_SLAB_CHUNK][slot % JS_SLAB_CHUNK];
        if (r->realizer) { rc = JS_ERR_CORRUPT; break; }   /* duplicate slot */
        if (slot >= s->real_hw) s->real_hw = slot + 1;
        r->gen = gen;
        r->realizer = rz;
        r->realization_type = rz->type;
        r->recipe = (JsRecipeKind)rec;
        r->token = rd64(p + 24);
        r->edit_off = rd32(p + 32); r->edit_len = rd32(p + 36);
        r->patch_off = rd64(p + 40);
        uint32_t place = rd32(p + 48);
        memcpy(r->semantic_state_id.b, p + 64, 32);
        memcpy(r->content, p + 96, 32);
        r->placement = JS_PLACE_EVICTED;
        parent_slot[i] = rd32(p + 8); parent_gen[i] = rd32(p + 12);
        loaded[i] = r;
        s->stats.live_reals++;
        if (place == JS_PLACE_SPILLED) {
            r->spill_off = rd64(p + 56);
            r->placement = JS_PLACE_SPILLED;
            ext[n_ext++] = (JsExtent){ r->spill_off, rz->unit_bytes };
            charge(s, r, +1);
        } else if (place != JS_PLACE_EVICTED) {
            rc = JS_ERR_CORRUPT;
        }
        if (rec == JS_RECIPE_EDIT) {
            if (!r->edit_len || (uint64_t)r->edit_off + r->edit_len > rz->unit_bytes ||
                r->patch_off == UINT64_MAX) { rc = JS_ERR_CORRUPT; break; }
            ext[n_ext++] = (JsExtent){ r->patch_off, r->edit_len };
        } else if (r->patch_off != UINT64_MAX) {
            rc = JS_ERR_CORRUPT;
        }
    }
    /* Extents: inside the file, never overlapping. The gaps below the last
     * live extent are the free list; the file's logical end is that extent. */
    if (rc == JS_OK) {
        qsort(ext, n_ext, sizeof *ext, ext_cmp);
        uint64_t at = 0;
        for (uint32_t i = 0; i < n_ext && rc == JS_OK; i++) {
            uint64_t e = ext[i].off + ext[i].len;
            if (ext[i].off < at || e < ext[i].off || e > spill_end) { rc = JS_ERR_CORRUPT; break; }
            if (ext[i].off > at &&
                ext_push(&s->ext_free, &s->n_ext_free, &s->cap_ext_free, s->n_ext_free,
                         (JsExtent){ at, ext[i].off - at }) != JS_OK) { rc = JS_ERR_NOMEM; break; }
            at = e;
        }
        s->spill_end = at;
    }
    /* Parents, recipe shape, and semantic identity recomputed from the recipe. */
    for (uint32_t i = 0; i < n_reals && rc == JS_OK; i++) {
        JsReal *r = loaded[i];
        JsReal *par = NULL;
        if (parent_slot[i] != UINT32_MAX) {
            par = slab_at(s, parent_slot[i]);
            if (!par || !par->realizer || par->gen != parent_gen[i] ||
                par->realizer != r->realizer) { rc = JS_ERR_CORRUPT; break; }
        }
        if ((r->recipe == JS_RECIPE_ROOT) != (par == NULL)) { rc = JS_ERR_CORRUPT; break; }
        r->parent_realization = par;
        if (par) par->refs++;
        JsSemId want;
        if (r->recipe == JS_RECIPE_ROOT) {
            want = js_sem_root(r->token);
        } else if (r->recipe == JS_RECIPE_DERIVE) {
            want = js_sem_derive(&par->semantic_state_id, r->token);
        } else {
            r->edit_bytes = malloc(r->edit_len);
            if (!r->edit_bytes) { rc = JS_ERR_NOMEM; break; }
            if (pread(s->spill_fd, r->edit_bytes, r->edit_len, (off_t)r->patch_off) !=
                (ssize_t)r->edit_len) { rc = JS_ERR_CORRUPT; break; }
            want = js_sem_edit(&par->semantic_state_id, r->edit_off, r->edit_bytes, r->edit_len);
        }
        /* Identities are a hash chain, so a parent cycle cannot satisfy this
         * for every member; with it, every chain ends at a root. */
        if (memcmp(want.b, r->semantic_state_id.b, 32)) { rc = JS_ERR_CORRUPT; break; }
    }
    /* Branches. */
    for (uint32_t i = 0; i < n_br && rc == JS_OK; i++) {
        if ((size_t)(end - p) < JS_META_BRANCH) { rc = JS_ERR_CORRUPT; break; }
        uint32_t id = rd32(p), gen = rd32(p + 4), ty = rd32(p + 32), nu = rd32(p + 36);
        uint32_t loc = rd32(p + 28);
        const JsRealizer *rz = NULL;
        for (uint32_t t = 0; t < n_types; t++) if ((uint32_t)types[t]->type == ty) rz = types[t];
        if (!rz || id >= n_slots || s->branches[id] || gen != s->branch_gen[id] || !nu ||
            loc > JS_HOME_REPLICA || (size_t)(end - p) < JS_META_BRANCH + (size_t)nu * 8) {
            rc = JS_ERR_CORRUPT; break;
        }
        JsBranch *b = calloc(1, sizeof *b);
        JsReal **units = malloc((size_t)(nu + 64) * sizeof *units);
        if (!b || !units) { free(b); free(units); rc = JS_ERR_NOMEM; break; }
        b->branch_id = id; b->gen = gen; b->realizer = rz;
        b->parent_branch = rd32(p + 8); b->common_ancestor = rd32(p + 12);
        b->divergence_point = rd32(p + 16); b->frozen = rd32(p + 20) != 0;
        b->owner = rd32(p + 24); b->home.locality = loc;
        memcpy(b->home.machine, p + 40, JS_MACHINE_ID_BYTES);
        b->units = units; b->cap_units = nu + 64;
        s->branches[id] = b;
        const uint8_t *u = p + JS_META_BRANCH;
        for (uint32_t k = 0; k < nu; k++, u += 8) {
            JsReal *r = slab_at(s, rd32(u));
            if (!r || !r->realizer || r->gen != rd32(u + 4) || r->realizer != rz) {
                rc = JS_ERR_CORRUPT; break;
            }
            b->units[b->n_units++] = r;
            r->refs++; r->holders++;
        }
        p = u;
    }
    if (rc == JS_OK && p != end) rc = JS_ERR_CORRUPT;
    /* Every persisted realization is held by something; nothing dangles. */
    for (uint32_t i = 0; i < n_reals && rc == JS_OK; i++)
        if (!loaded[i]->refs) rc = JS_ERR_CORRUPT;
    if (rc == JS_OK) {
        for (uint32_t i = 0; i < n_reals; i++) index_insert(s, loaded[i]);
        /* Unused slots below the high-water mark go on the free list. */
        for (uint32_t k = s->real_hw; k-- > 0;) {
            if (slab_chunk(s, k) != JS_OK) { rc = JS_ERR_NOMEM; break; }
            JsReal *r = &s->slab[k / JS_SLAB_CHUNK][k % JS_SLAB_CHUNK];
            if (r->realizer) continue;
            r->gen = s->real_gen_floor;
            r->free_next = s->real_free;
            s->real_free = r;
        }
        for (uint32_t i = n_slots; i-- > 0;)
            if (!s->branches[i]) s->free_branch[s->n_free_branch++] = i;
    }
    free(parent_slot); free(parent_gen); free(loaded); free(ext);
    return rc;
}

int js_space_open(JsSpace *s, const char *dir, const JsRealizer *const *realizers,
                  uint32_t n_realizers, const JsLimits *lim, const JsHome *local_home) {
    if (!dir) return JS_ERR_ARG;
    int rc = space_setup(s, lim);
    if (rc) return rc;
    if (local_home) s->local_home = *local_home;
    s->local_home.locality = JS_HOME_LOCAL;
    s->durable = true;
    if (mkdir(dir, 0700) && errno != EEXIST) { js_space_destroy(s); return JS_ERR_IO; }
    s->dir = strdup(dir);
    char *data = path_in(dir, "jspace.data"), *meta = path_in(dir, "jspace.meta");
    char *tmp = path_in(dir, "jspace.meta.tmp");
    if (!s->dir || !data || !meta || !tmp) { rc = JS_ERR_NOMEM; goto out; }
    unlink(tmp);   /* a torn temporary checkpoint is never authoritative */
    s->spill_fd = open(data, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (s->spill_fd < 0) { rc = JS_ERR_IO; goto out; }
    int mfd = open(meta, O_RDONLY | O_CLOEXEC);
    if (mfd < 0) {
        if (errno != ENOENT) { rc = JS_ERR_IO; goto out; }
        /* No checkpoint yet: nothing in the data file is named. */
        if (ftruncate(s->spill_fd, 0)) { rc = JS_ERR_IO; goto out; }
        goto out;
    }
    struct stat st;
    uint8_t *f = NULL;
    if (fstat(mfd, &st)) rc = JS_ERR_IO;
    else if (!(f = malloc(st.st_size ? (size_t)st.st_size : 1))) rc = JS_ERR_NOMEM;
    else {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t k = read(mfd, f + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        rc = got == (size_t)st.st_size ? load_u(s, f, got, realizers, n_realizers) : JS_ERR_IO;
    }
    free(f);
    close(mfd);
out:
    free(data); free(meta); free(tmp);
    if (rc) js_space_destroy(s);
    return rc;
}
