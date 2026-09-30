/* Predictor side of EXP-001: model + CTR1 events -> TPS1 + TSY1. See tc_produce.h. */
#include "turing/tc_produce.h"

#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHY(...)                                              \
    do {                                                      \
        if (why && whylen) snprintf(why, whylen, __VA_ARGS__); \
    } while (0)

#define POS_IDX_BITS 20

typedef struct {
    unsigned mask, K;
    unsigned p[5];
} walk;

static uint64_t walk_key(walk *w, const ty_ev *e) {
    if (e->first)
        for (int i = 0; i < 5; ++i) w->p[i] = w->K;
    uint64_t key = 0, mul = 1;
    if (w->mask & TY_F_OP) {
        key += (uint64_t)e->op * mul;
        mul *= 16;
    }
    if (w->mask & TY_F_DEPTH) {
        key += (uint64_t)e->depth * mul;
        mul *= TY_DEPTH_CLIP + 1;
    }
    static const unsigned prevbit[5] = {TY_F_PREV1, TY_F_PREV2, TY_F_PREV3, TY_F_PREV4, TY_F_PREV5};
    for (int i = 0; i < 5; ++i)
        if (w->mask & prevbit[i]) {
            key += (uint64_t)w->p[i] * mul;
            mul *= w->K + 1;
        }
    if (w->mask & TY_F_POS) {
        uint64_t idx = e->idx >= (1u << POS_IDX_BITS) ? (1u << POS_IDX_BITS) - 1 : e->idx;
        key += (((uint64_t)e->crumb << POS_IDX_BITS) | idx) * mul;
    }
    return key;
}

static void walk_advance(walk *w, const ty_ev *e) {
    for (int i = 4; i > 0; --i) w->p[i] = w->p[i - 1];
    w->p[0] = e->sym;
}

static const uint32_t *lookup(const ty_model *m, uint64_t key) {
    size_t lo = 0, hi = m->nrows;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->rows[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < m->nrows && m->rows[lo].key == key) ? m->rows[lo].q : m->def;
}

int tc_produce(const ty_model *m, const ty_stream *s, const uint8_t profile[TC_DIGEST],
               const uint8_t model_digest[TC_DIGEST], const uint8_t dataset[TC_DIGEST], tc_pstream *p,
               tc_symbols *sy, char *why, size_t whylen) {
    if (!m || !s || !profile || !model_digest || !dataset || !p || !sy) return TC_E_ARG;
    int rc = tc_ps_alloc(p, m->K, s->n);
    if (rc != TC_OK) return rc;
    if ((rc = tc_sy_alloc(sy, m->K, s->n)) != TC_OK) {
        tc_ps_free(p);
        return rc;
    }
    memcpy(p->profile, profile, TC_DIGEST);
    memcpy(p->model, model_digest, TC_DIGEST);
    memcpy(p->dataset, dataset, TC_DIGEST);
    memcpy(sy->dataset, dataset, TC_DIGEST);
    walk w = {m->mask, m->K, {m->K, m->K, m->K, m->K, m->K}};
    uint32_t crumb = 0;
    for (size_t t = 0; t < s->n; ++t) {
        const ty_ev *e = &s->ev[t];
        if (e->sym >= m->K) {
            WHY("event %zu symbol %u outside the model alphabet K=%u", t, e->sym, m->K);
            rc = TC_E_SYMBOL;
            goto fail;
        }
        if (e->first && t > 0) ++crumb;
        uint64_t key = walk_key(&w, e);
        const uint32_t *row = m->nrows ? lookup(m, key) : m->def;
        p->key[t] = key;
        p->crumb[t] = crumb;
        for (unsigned x = 0; x < m->K; ++x) {
            if (row[x] == 0 || row[x] >= TC_QONE) {
                WHY("model row entry %u out of 1..65535", row[x]);
                rc = TC_E_ZERO;
                goto fail;
            }
            p->q[t * m->K + x] = (uint16_t)row[x];
        }
        sy->sym[t] = e->sym;
        walk_advance(&w, e);
    }
    if ((rc = tc_ps_check_rows(p, why, whylen)) != TC_OK) goto fail;
    /* Cross-check against the TY-2 scorer: identical ideal code length. */
    int64_t want = 0, got = 0;
    if (ty_model_score(m, s, &want, NULL) != TY_OK || tc_ideal_ub(p, sy, 0, p->n, &got) != TC_OK || want != got) {
        WHY("probability stream ideal %" PRId64 " ub differs from ty_model_score %" PRId64 " ub", got, want);
        rc = TC_E_CORRUPT;
        goto fail;
    }
    return TC_OK;
fail:
    tc_ps_free(p);
    tc_sy_free(sy);
    return rc;
}

int tc_load_model(const char *path, ty_model *m, uint8_t digest[TC_DIGEST], uint64_t *lm_bits, char *why,
                  size_t whylen) {
    uint8_t *b;
    size_t n;
    if (tc_read_file(path, &b, &n) != TC_OK) {
        WHY("cannot read model %s", path);
        return TC_E_IO;
    }
    int rc = ty_model_decode(b, n, m, lm_bits, why, whylen);
    if (rc == TY_OK) ty_model_digest(b, n, digest);
    free(b);
    return rc == TY_OK ? TC_OK : TC_E_FORMAT;
}

int tc_file_sha256(const char *path, uint8_t out[TC_DIGEST]) {
    FILE *f = fopen(path, "rb");
    if (!f) return TC_E_IO;
    sha256_ctx c;
    sha256_init(&c);
    static uint8_t buf[1 << 16];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, k);
    int err = ferror(f);
    fclose(f);
    if (err) return TC_E_IO;
    sha256_final(&c, out);
    return TC_OK;
}
