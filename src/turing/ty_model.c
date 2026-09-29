/* Turing Yield context-table models. See ty_model.h. */
#include "turing/ty_model.h"

#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

#define POS_IDX_BITS 20
#define POS_RADIX_BITS 36 /* 16-bit crumb ordinal x 20-bit event index */

void ty_model_free(ty_model *m) {
    if (!m) return;
    free(m->rows);
    memset(m, 0, sizeof *m);
}

int ty_key_space(unsigned mask, unsigned K, uint64_t *space, unsigned *bits) {
    if (mask & ~(unsigned)TY_F_ALL || K < 2 || K > TY_KMAX) return TY_E_ARG;
    ty_u128 p = 1;
    if (mask & TY_F_OP) p *= 16;
    if (mask & TY_F_DEPTH) p *= TY_DEPTH_CLIP + 1;
    if (mask & TY_F_PREV1) p *= K + 1;
    if (mask & TY_F_PREV2) p *= K + 1;
    if (mask & TY_F_PREV3) p *= K + 1;
    if (mask & TY_F_PREV4) p *= K + 1;
    if (mask & TY_F_PREV5) p *= K + 1;
    if (mask & TY_F_POS) p *= (ty_u128)1 << POS_RADIX_BITS;
    if (p > ((ty_u128)1 << 62)) return TY_E_RANGE;
    uint64_t s = (uint64_t)p;
    if (space) *space = s;
    if (bits) *bits = s <= 1 ? 0u : 64u - (unsigned)__builtin_clzll(s - 1);
    return TY_OK;
}

/* Walks a stream computing each event's context key. */
typedef struct {
    unsigned mask, K;
    unsigned p1, p2, p3, p4, p5;
} key_walk;

static void kw_init(key_walk *w, unsigned mask, unsigned K) {
    w->mask = mask;
    w->K = K;
    w->p1 = w->p2 = w->p3 = w->p4 = w->p5 = K;
}

static uint64_t kw_key(key_walk *w, const ty_ev *e) {
    if (e->first) w->p1 = w->p2 = w->p3 = w->p4 = w->p5 = w->K; /* context resets per crumb */
    uint64_t key = 0, mul = 1;
    if (w->mask & TY_F_OP) {
        key += (uint64_t)e->op * mul;
        mul *= 16;
    }
    if (w->mask & TY_F_DEPTH) {
        key += (uint64_t)e->depth * mul;
        mul *= TY_DEPTH_CLIP + 1;
    }
    if (w->mask & TY_F_PREV1) {
        key += (uint64_t)w->p1 * mul;
        mul *= w->K + 1;
    }
    if (w->mask & TY_F_PREV2) {
        key += (uint64_t)w->p2 * mul;
        mul *= w->K + 1;
    }
    if (w->mask & TY_F_PREV3) {
        key += (uint64_t)w->p3 * mul;
        mul *= w->K + 1;
    }
    if (w->mask & TY_F_PREV4) {
        key += (uint64_t)w->p4 * mul;
        mul *= w->K + 1;
    }
    if (w->mask & TY_F_PREV5) {
        key += (uint64_t)w->p5 * mul;
        mul *= w->K + 1;
    }
    if (w->mask & TY_F_POS) {
        uint64_t idx = e->idx >= (1u << POS_IDX_BITS) ? (1u << POS_IDX_BITS) - 1 : e->idx;
        key += (((uint64_t)e->crumb << POS_IDX_BITS) | idx) * mul;
    }
    return key;
}

static void kw_advance(key_walk *w, const ty_ev *e) {
    w->p5 = w->p4;
    w->p4 = w->p3;
    w->p3 = w->p2;
    w->p2 = w->p1;
    w->p1 = e->sym;
}

/* ------------------------------------------------------------ fit tables */

typedef struct {
    uint64_t key;
    uint64_t c[TY_KMAX];
} cnt_row;

typedef struct {
    uint64_t *keys;
    uint32_t *slot; /* row index + 1; 0 = empty */
    size_t cap;
    cnt_row *rows;
    size_t n, rcap;
} cnt_map;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int cm_grow(cnt_map *h) {
    size_t nc = h->cap ? h->cap * 2 : 1024;
    uint64_t *k = calloc(nc, sizeof *k);
    uint32_t *s = calloc(nc, sizeof *s);
    if (!k || !s) {
        free(k);
        free(s);
        return TY_E_IO;
    }
    for (size_t i = 0; i < h->cap; ++i) {
        if (!h->slot[i]) continue;
        size_t j = mix64(h->keys[i]) & (nc - 1);
        while (s[j]) j = (j + 1) & (nc - 1);
        k[j] = h->keys[i];
        s[j] = h->slot[i];
    }
    free(h->keys);
    free(h->slot);
    h->keys = k;
    h->slot = s;
    h->cap = nc;
    return TY_OK;
}

static cnt_row *cm_get(cnt_map *h, uint64_t key) {
    if ((h->n + 1) * 2 > h->cap && cm_grow(h) != TY_OK) return NULL;
    size_t j = mix64(key) & (h->cap - 1);
    while (h->slot[j]) {
        if (h->keys[j] == key) return &h->rows[h->slot[j] - 1];
        j = (j + 1) & (h->cap - 1);
    }
    if (h->n == h->rcap) {
        size_t nr = h->rcap ? h->rcap * 2 : 1024;
        cnt_row *r = realloc(h->rows, nr * sizeof *r);
        if (!r) return NULL;
        h->rows = r;
        h->rcap = nr;
    }
    if (h->n >= UINT32_MAX - 1) return NULL;
    cnt_row *r = &h->rows[h->n++];
    memset(r, 0, sizeof *r);
    r->key = key;
    h->keys[j] = key;
    h->slot[j] = (uint32_t)h->n;
    return r;
}

static void cm_free(cnt_map *h) {
    free(h->keys);
    free(h->slot);
    free(h->rows);
    memset(h, 0, sizeof *h);
}

static int cmp_row(const void *a, const void *b) {
    uint64_t x = ((const ty_row *)a)->key, y = ((const ty_row *)b)->key;
    return x < y ? -1 : x > y;
}

int ty_model_uniform(ty_model *m, unsigned K) {
    if (!m || K < 2 || K > TY_KMAX) return TY_E_ARG;
    memset(m, 0, sizeof *m);
    uint64_t zero[TY_KMAX] = {0};
    m->K = K;
    return ty_quantize_kt(zero, K, m->def);
}

int ty_model_fit(ty_model *m, const ty_stream *const *streams, size_t ns, unsigned mask, unsigned K, int rule) {
    if (!m || (ns && !streams) || (rule != TY_FIT_KEEP_ALL && rule != TY_FIT_MDL_PRUNE)) return TY_E_ARG;
    unsigned kb;
    int rc = ty_key_space(mask, K, NULL, &kb);
    if (rc != TY_OK) return rc;
    memset(m, 0, sizeof *m);
    m->K = K;
    m->mask = mask;
    m->keybits = kb;
    uint64_t tot[TY_KMAX] = {0};
    cnt_map h;
    memset(&h, 0, sizeof h);
    for (size_t si = 0; si < ns; ++si) {
        const ty_stream *s = streams[si];
        key_walk w;
        kw_init(&w, mask, K);
        for (size_t t = 0; t < s->n; ++t) {
            const ty_ev *e = &s->ev[t];
            if (e->sym >= K) {
                cm_free(&h);
                return TY_E_RANGE;
            }
            uint64_t key = kw_key(&w, e);
            tot[e->sym]++;
            if (mask) {
                cnt_row *r = cm_get(&h, key);
                if (!r) {
                    cm_free(&h);
                    return TY_E_IO;
                }
                r->c[e->sym]++;
            }
            kw_advance(&w, e);
        }
    }
    if ((rc = ty_quantize_kt(tot, K, m->def)) != TY_OK) {
        cm_free(&h);
        return rc;
    }
    m->rows = h.n ? malloc(h.n * sizeof *m->rows) : NULL;
    if (h.n && !m->rows) {
        cm_free(&h);
        return TY_E_IO;
    }
    const int64_t row_cost_ub = (int64_t)(kb + (K - 1) * TY_QBITS) * TY_UB_PER_BIT;
    for (size_t i = 0; i < h.n; ++i) {
        ty_row r;
        memset(&r, 0, sizeof r);
        r.key = h.rows[i].key;
        if ((rc = ty_quantize_kt(h.rows[i].c, K, r.q)) != TY_OK) break;
        if (rule == TY_FIT_MDL_PRUNE) {
            /* Keep the row only if, on the fit data, it saves more than it costs. */
            int64_t save = 0;
            for (unsigned x = 0; x < K; ++x) {
                int64_t d = ty_ubits_q16(m->def[x]) - ty_ubits_q16(r.q[x]);
                save += (int64_t)h.rows[i].c[x] * d; /* fit counts < 2^40, |d| < 2^25 */
            }
            if (save <= row_cost_ub) continue;
        }
        m->rows[m->nrows++] = r;
    }
    cm_free(&h);
    if (rc != TY_OK) {
        ty_model_free(m);
        return rc;
    }
    if (m->nrows) qsort(m->rows, m->nrows, sizeof *m->rows, cmp_row);
    return TY_OK;
}

/* ------------------------------------------------------------ bit code */

typedef struct {
    uint8_t *b;
    size_t cap;
    uint64_t bits;
    int err;
} bitw;

static void bw_put(bitw *w, uint64_t v, unsigned n) {
    for (unsigned i = n; i-- > 0;) {
        size_t byte = (size_t)(w->bits >> 3);
        if (byte >= w->cap) {
            size_t nc = w->cap ? w->cap * 2 : 256;
            uint8_t *p = realloc(w->b, nc);
            if (!p) {
                w->err = 1;
                return;
            }
            memset(p + w->cap, 0, nc - w->cap);
            w->b = p;
            w->cap = nc;
        }
        if ((v >> i) & 1) w->b[byte] |= (uint8_t)(0x80u >> (w->bits & 7));
        w->bits++;
    }
}

typedef struct {
    const uint8_t *b;
    uint64_t nbits, pos;
    int err;
} bitr;

static uint64_t br_get(bitr *r, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (r->pos >= r->nbits) {
            r->err = 1;
            return 0;
        }
        v = (v << 1) | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

static void put_row(bitw *w, const uint32_t *q, unsigned K) {
    for (unsigned x = 0; x + 1 < K; ++x) bw_put(w, q[x], TY_QBITS);
}

int ty_model_encode(const ty_model *m, uint8_t **buf, size_t *nbytes, uint64_t *bits) {
    if (!m || !buf || !nbytes || !bits || m->K < 2 || m->K > TY_KMAX) return TY_E_ARG;
    if (m->nrows > UINT32_MAX) return TY_E_RANGE;
    bitw w;
    memset(&w, 0, sizeof w);
    bw_put(&w, 0x54594D30u, 32); /* "TYM0" */
    bw_put(&w, TY_MODEL_VERSION, 8);
    bw_put(&w, m->K, 8);
    bw_put(&w, m->mask, 8);
    bw_put(&w, TY_QBITS, 8);
    bw_put(&w, m->keybits, 8);
    bw_put(&w, m->nrows, 32);
    put_row(&w, m->def, m->K);
    for (size_t i = 0; i < m->nrows; ++i) {
        bw_put(&w, m->rows[i].key, m->keybits);
        put_row(&w, m->rows[i].q, m->K);
    }
    if (w.err) {
        free(w.b);
        return TY_E_IO;
    }
    *buf = w.b;
    *nbytes = (size_t)((w.bits + 7) / 8);
    *bits = w.bits;
    return TY_OK;
}

static int get_row(bitr *r, unsigned K, uint32_t *q) {
    uint32_t s = 0;
    for (unsigned x = 0; x + 1 < K; ++x) {
        q[x] = (uint32_t)br_get(r, TY_QBITS);
        if (q[x] == 0) return TY_E_PROFILE; /* below the floor */
        s += q[x];
    }
    if (s >= TY_QONE) return TY_E_PROFILE; /* implied last entry would be < 1 */
    q[K - 1] = TY_QONE - s;
    return r->err ? TY_E_FORMAT : TY_OK;
}

int ty_model_decode(const uint8_t *buf, size_t nbytes, ty_model *m, uint64_t *bits, char *why, size_t whylen) {
    if (!buf || !m) return TY_E_ARG;
    memset(m, 0, sizeof *m);
    bitr r = {buf, (uint64_t)nbytes * 8, 0, 0};
    if (br_get(&r, 32) != 0x54594D30u || r.err) {
        WHY("bad model magic");
        return TY_E_FORMAT;
    }
    unsigned ver = (unsigned)br_get(&r, 8), K = (unsigned)br_get(&r, 8), mask = (unsigned)br_get(&r, 8);
    unsigned qb = (unsigned)br_get(&r, 8), kb = (unsigned)br_get(&r, 8);
    uint64_t nrows = br_get(&r, 32);
    if (r.err) {
        WHY("truncated header");
        return TY_E_FORMAT;
    }
    if (ver != TY_MODEL_VERSION) {
        WHY("model code version %u (profile encoder is version %u)", ver, TY_MODEL_VERSION);
        return TY_E_PROFILE;
    }
    if (qb != TY_QBITS) {
        WHY("quantization %u bits (profile pins %u)", qb, TY_QBITS);
        return TY_E_PROFILE;
    }
    uint64_t space;
    unsigned want_kb;
    if (ty_key_space(mask, K, &space, &want_kb) != TY_OK || kb != want_kb) {
        WHY("mask %u / K %u / key bits %u inconsistent", mask, K, kb);
        return TY_E_FORMAT;
    }
    uint64_t need = TY_MODEL_HEADER_BITS + (uint64_t)(K - 1) * TY_QBITS +
                    nrows * ((uint64_t)kb + (uint64_t)(K - 1) * TY_QBITS);
    if ((need + 7) / 8 != nbytes) {
        WHY("code is %zu bytes, header implies %llu bits", nbytes, (unsigned long long)need);
        return TY_E_FORMAT;
    }
    m->K = K;
    m->mask = mask;
    m->keybits = kb;
    int rc = get_row(&r, K, m->def);
    if (rc != TY_OK) {
        WHY("default row: %s", ty_err_name(rc));
        return rc;
    }
    m->rows = nrows ? malloc((size_t)nrows * sizeof *m->rows) : NULL;
    if (nrows && !m->rows) return TY_E_IO;
    for (uint64_t i = 0; i < nrows; ++i) {
        ty_row *row = &m->rows[i];
        memset(row, 0, sizeof *row);
        row->key = br_get(&r, kb);
        if (row->key >= space || (i > 0 && row->key <= m->rows[i - 1].key)) {
            WHY("row %llu key out of range or not increasing", (unsigned long long)i);
            ty_model_free(m);
            return TY_E_FORMAT;
        }
        if ((rc = get_row(&r, K, row->q)) != TY_OK) {
            WHY("row %llu: %s", (unsigned long long)i, ty_err_name(rc));
            ty_model_free(m);
            return rc;
        }
        m->nrows++;
    }
    while (r.pos < r.nbits) {
        if (br_get(&r, 1) != 0) {
            WHY("nonzero padding");
            ty_model_free(m);
            return TY_E_FORMAT;
        }
    }
    if (bits) *bits = need;
    return TY_OK;
}

static const ty_row *find_row(const ty_model *m, uint64_t key) {
    size_t lo = 0, hi = m->nrows;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->rows[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < m->nrows && m->rows[lo].key == key) ? &m->rows[lo] : NULL;
}

int ty_model_score(const ty_model *m, const ty_stream *s, int64_t *ub, uint64_t *nsym) {
    if (!m || !s || !ub) return TY_E_ARG;
    key_walk w;
    kw_init(&w, m->mask, m->K);
    int64_t total = 0;
    for (size_t t = 0; t < s->n; ++t) { /* file order; integer sum, order-free */
        const ty_ev *e = &s->ev[t];
        if (e->sym >= m->K) return TY_E_RANGE;
        uint64_t key = kw_key(&w, e);
        const ty_row *r = m->nrows ? find_row(m, key) : NULL;
        uint32_t q = r ? r->q[e->sym] : m->def[e->sym];
        int64_t b = ty_ubits_q16(q);
        if (b < 0) return TY_E_ZERO_REALIZED;
        if (ty_add(total, b, &total) != TY_OK) return TY_E_OVERFLOW;
        kw_advance(&w, e);
    }
    *ub = total;
    if (nsym) *nsym = s->n;
    return TY_OK;
}

void ty_model_digest(const uint8_t *buf, size_t nbytes, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)TY_MODEL_DOMAIN, sizeof TY_MODEL_DOMAIN); /* includes 0x00 */
    sha256_update(&c, buf, nbytes);
    sha256_final(&c, out);
}
