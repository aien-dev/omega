#include "core.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= ideal length =================
 * Profile ideal_codelength_method: integer-only log2 (32 fractional bits, truncated, Q62 mantissa squaring),
 * -log2(q/65536) rounded to the nearest micro-bit once per q value. This is the exact rule now written in the profile,
 * CODER_SPEC.md and EVALUATOR.md (SPEC_GAPS G2, resolved): 32 truncating squarings, then (x * 1e6 + 2^31) >> 32. */
int64_t is_ub_q16[65537];

uint64_t is_log2_q32(uint32_t q) {
    int ip = 31 - __builtin_clz(q);
    uint64_t m = (uint64_t)q << (62 - ip);         /* Q62 in [1,2) */
    uint64_t frac = 0; int i;
    for (i = 1; i <= 32; i++) {
        unsigned __int128 sq = (unsigned __int128)m * m;
        m = (uint64_t)(sq >> 62);                   /* truncate, Q62 in [1,4) */
        if (m >= (1ULL << 63)) { frac |= 1ULL << (32 - i); m >>= 1; }
    }
    return ((uint64_t)ip << 32) | frac;
}
void is_ub_table_init(void) {
    uint32_t q;
    is_ub_q16[0] = -1;
    for (q = 1; q <= 65536; q++) {
        uint64_t x = (16ULL << 32) - is_log2_q32(q);       /* -log2(q/65536) in Q32 */
        is_ub_q16[q] = (int64_t)((x * 1000000ULL + (1ULL << 31)) >> 32);
    }
}

/* ================= helpers ================= */
int is_read_file(const char *path, uint8_t **buf, uint64_t *len) {
    FILE *f = fopen(path, "rb"); long n; uint8_t *b;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    b = malloc(n ? (size_t)n : 1);
    if (!b) { fclose(f); return -1; }
    if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return -1; }
    fclose(f); *buf = b; *len = (uint64_t)n; return 0;
}
void is_domain_digest_init(void *ctx, const char *tag) {
    uint8_t z = 0;
    is_sha256_init(ctx); is_sha256_update(ctx, tag, strlen(tag)); is_sha256_update(ctx, &z, 1);
}

/* ================= TYM0 ================= */
typedef struct { const uint8_t *b; uint64_t nbits, pos; } bitrd;
static uint64_t getbits(bitrd *r, int w) {
    uint64_t v = 0; int i;
    for (i = 0; i < w; i++) {
        uint64_t p = r->pos++;
        v = (v << 1) | ((r->b[p >> 3] >> (7 - (p & 7))) & 1);
    }
    return v;
}
static const int feat_bit[8]  = {1, 2, 4, 8, 16, 64, 128, 32};   /* code order: op depth prev1..prev5 pos */
static uint64_t feat_radix(int i, int K) {
    switch (i) { case 0: return 16; case 1: return 8; case 7: return 1ULL << 36; default: return (uint64_t)K + 1; }
}
#define REF(n, ...) do { snprintf(msg, msglen, __VA_ARGS__); ret = -(n); goto fail; } while (0)
int is_model_load(const char *path, is_model *m, char *msg, size_t msglen) {
    uint8_t *buf = NULL; uint64_t len, i, S = 1, need; int ret = 0, j, K;
    bitrd r; is_sha256 c;
    memset(m, 0, sizeof *m);
    if (is_read_file(path, &buf, &len)) { snprintf(msg, msglen, "IO: cannot read %s", path); return -100; }
    m->file_bytes = len;
    is_sha256_init(&c); is_sha256_update(&c, buf, len); is_sha256_final(&c, m->file_sha);
    is_domain_digest_init(&c, "turing.ymodel.v0"); is_sha256_update(&c, buf, len); is_sha256_final(&c, m->model_digest);
    r.b = buf; r.nbits = len * 8; r.pos = 0;
    if (r.nbits < 104) REF(1, "fewer than 104 header bits");
    if (getbits(&r, 32) != 0x54594D30ULL) REF(1, "bad magic");
    if (getbits(&r, 8) != 0) REF(2, "version != 0");
    K = (int)getbits(&r, 8); m->K = K;
    m->mask = (int)getbits(&r, 8);
    m->qbits = (int)getbits(&r, 8);
    m->keybits = (int)getbits(&r, 8);
    m->nrows = getbits(&r, 32);
    if (m->qbits != 16) REF(3, "qbits != 16");
    if (K < 2 || K > 16) REF(4, "K out of 2..16");
    for (j = 0; j < 8; j++) if (m->mask & feat_bit[j]) {
        uint64_t rd = feat_radix(j, K);
        if (S > (1ULL << 62) / rd + 1) REF(4, "S > 2^62");
        S *= rd;
        if (S > (1ULL << 62)) REF(4, "S > 2^62");
    }
    m->S = S;
    { int kb = 0; if (S > 1) { uint64_t v = S - 1; while (v) { kb++; v >>= 1; } }
      if (m->keybits != kb) REF(4, "keybits %d != %d", m->keybits, kb); }
    { unsigned __int128 L = 104 + (unsigned __int128)16 * (K - 1) + (unsigned __int128)m->nrows * (m->keybits + 16 * (K - 1));
      if ((L + 7) / 8 != len) REF(5, "file length %llu != ceil(L/8)", (unsigned long long)len);
      m->lm_bits = (uint64_t)L; }
    need = m->lm_bits;
    { uint32_t s = 0; for (j = 0; j < K - 1; j++) { uint64_t v = getbits(&r, 16); if (!v) REF(6, "zero entry (default row)"); m->def[j] = (uint16_t)v; s += (uint32_t)v; }
      if (s >= 65536) REF(6, "default row sum >= 65536");
      m->def[K - 1] = (uint16_t)(65536 - s); }
    m->keys = malloc((m->nrows ? m->nrows : 1) * sizeof(uint64_t));
    m->rows = malloc((m->nrows ? m->nrows : 1) * (size_t)K * sizeof(uint16_t));
    if (!m->keys || !m->rows) REF(100, "out of memory");
    for (i = 0; i < m->nrows; i++) {
        uint64_t key = m->keybits ? getbits(&r, m->keybits) : 0; uint32_t s = 0; uint16_t *row = m->rows + i * K;
        if (key >= S) REF(7, "row key >= S");
        if (i && key <= m->keys[i - 1]) REF(7, "keys not strictly increasing");
        m->keys[i] = key;
        for (j = 0; j < K - 1; j++) { uint64_t v = getbits(&r, 16); if (!v) REF(6, "zero entry row %llu", (unsigned long long)i); row[j] = (uint16_t)v; s += (uint32_t)v; }
        if (s >= 65536) REF(6, "row sum >= 65536");
        row[K - 1] = (uint16_t)(65536 - s);
    }
    if (r.pos != need) REF(5, "internal length mismatch");
    while (r.pos < r.nbits) if (getbits(&r, 1)) REF(8, "nonzero padding bit");
    free(buf);
    return 0;
fail:
    free(buf); is_model_free(m); return ret;
}
void is_model_free(is_model *m) { free(m->keys); free(m->rows); m->keys = NULL; m->rows = NULL; }
const uint16_t *is_model_row(const is_model *m, uint64_t key, int *found) {
    uint64_t lo = 0, hi = m->nrows;
    while (lo < hi) { uint64_t mid = lo + (hi - lo) / 2; if (m->keys[mid] < key) lo = mid + 1; else hi = mid; }
    if (lo < m->nrows && m->keys[lo] == key) { *found = 1; return m->rows + lo * m->K; }
    *found = 0; return m->def;
}

/* ================= CTR1 =================
 * Byte offsets are NOT in the docs (V0 section 2 points at ty_ctr1.h). Offsets below were inferred from the
 * dev data and validated (SPEC_GAPS G1): magic 0..3, version u16 @4, kind @6, result_class @7,
 * event_index u32 LE @8, op_index @20, prune @23, verify @24, fit @25, depth @29. */
#define CTR1_REC 247
int is_ctr1_load(const char *path, is_events *ev, char *msg, size_t msglen) {
    FILE *f = fopen(path, "rb"); static uint8_t buf[CTR1_REC * 16384]; size_t got; uint64_t t = 0, cap, bytes = 0;
    uint32_t prev = 0; is_sha256 c; int ret = 0;
    memset(ev, 0, sizeof *ev);
    if (!f) { snprintf(msg, msglen, "IO: cannot open %s", path); return -100; }
    fseek(f, 0, SEEK_END); cap = (uint64_t)ftell(f); fseek(f, 0, SEEK_SET);
    if (cap % CTR1_REC) { snprintf(msg, msglen, "size not a multiple of 247"); fclose(f); return -1; }
    cap /= CTR1_REC;
    ev->sym = malloc(cap + 1); ev->op = malloc(cap + 1); ev->depth = malloc(cap + 1); ev->first = malloc(cap + 1);
    ev->evidx = malloc((cap + 1) * 4);
    if (!ev->sym || !ev->op || !ev->depth || !ev->first || !ev->evidx) { fclose(f); snprintf(msg, msglen, "oom"); return -100; }
    is_sha256_init(&c);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        size_t k;
        is_sha256_update(&c, buf, got); bytes += got;
        if (got % CTR1_REC) { snprintf(msg, msglen, "short read"); ret = -1; break; }
        for (k = 0; k < got; k += CTR1_REC) {
            const uint8_t *r = buf + k;
            uint32_t e = (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16 | (uint32_t)r[11] << 24;
            int kind = r[6], rc = r[7], prune = r[23], op = r[20], sym;
            if (memcmp(r, "CTR1", 4) || r[4] != 1 || r[5] != 0) { snprintf(msg, msglen, "rec %llu: magic/version", (unsigned long long)t); ret = -1; goto out; }
            if (kind != 1 && kind != 2) { snprintf(msg, msglen, "rec %llu: kind %d", (unsigned long long)t, kind); ret = -1; goto out; }
            if (prune > 4 || r[24] > 4 || r[25] > 2) { snprintf(msg, msglen, "rec %llu: prune/verify/fit range", (unsigned long long)t); ret = -1; goto out; }
            if (kind == 1) {
                if (op >= 15) { snprintf(msg, msglen, "rec %llu: op_index >= 15", (unsigned long long)t); ret = -1; goto out; }
                if (prune) { if (rc != 1) { snprintf(msg, msglen, "rec %llu: pruned but result_class %d", (unsigned long long)t, rc); ret = -1; goto out; } sym = prune - 1; }
                else { if (rc < 2 || rc > 4) { snprintf(msg, msglen, "rec %llu: unpruned result_class %d", (unsigned long long)t, rc); ret = -1; goto out; } sym = rc + 2; }
            } else {
                if (prune != 0 || rc < 5 || rc > 7) { snprintf(msg, msglen, "rec %llu: bad SUBMIT", (unsigned long long)t); ret = -1; goto out; }
                sym = rc == 5 ? 7 : 8; op = 15;
            }
            if (t > 0 && e != 0) {
                if (e < prev) { snprintf(msg, msglen, "rec %llu: event_index decreases without restart", (unsigned long long)t); ret = -1; goto out; }
                if (e != prev + 1) ev->gaps++;
            }
            ev->sym[t] = (uint8_t)sym; ev->op[t] = (uint8_t)op; ev->depth[t] = (uint8_t)(r[29] > 7 ? 7 : r[29]);
            ev->first[t] = (uint8_t)(t == 0 || e == 0); ev->evidx[t] = e;
            if (ev->first[t]) ev->crumbs++;
            prev = e; t++;
        }
    }
out:
    fclose(f);
    ev->n = t; ev->bytes = bytes;
    is_sha256_final(&c, ev->sha);
    if (!ret && t != cap) { snprintf(msg, msglen, "read %llu of %llu records", (unsigned long long)t, (unsigned long long)cap); ret = -1; }
    return ret;
}
void is_events_free(is_events *ev) { free(ev->sym); free(ev->op); free(ev->depth); free(ev->first); free(ev->evidx); memset(ev, 0, sizeof *ev); }

uint64_t is_key(const is_model *m, const is_events *ev, uint64_t t, const is_walk *w) {
    uint64_t key = 0, mult = 1; int j;
    for (j = 0; j < 8; j++) {
        uint64_t d;
        if (!(m->mask & feat_bit[j])) continue;
        switch (j) {
        case 0: d = ev->op[t]; break;
        case 1: d = ev->depth[t]; break;
        case 7: { uint64_t cr = w->crumb > 65535 ? 65535 : w->crumb; uint64_t e = ev->evidx[t] > 0xFFFFF ? 0xFFFFF : ev->evidx[t]; d = (cr << 20) | e; } break;
        default: d = (uint64_t)w->p[j - 2]; break;
        }
        key += d * mult;
        mult *= feat_radix(j, m->K);
    }
    return key;
}

/* ================= range decoder (CODER_SPEC 5) ================= */
#define TOP (1U << 24)
int is_rdec_init(is_rdec *d, const uint8_t *b, uint64_t L) {
    memset(d, 0, sizeof *d); d->b = b; d->L = L;
    if (L < 4) { d->err = -113; return d->err; }
    d->code = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    d->pos = 4; d->range = 0xFFFFFFFFU; return 0;
}
int is_rdec_step(is_rdec *d, const uint16_t *q, int K) {
    uint32_t r, cum = 0; int s;
    if (d->err) return d->err;
    if (d->code >= d->range) return d->err = -115;
    r = d->range >> 16;
    for (s = 0; s < K - 1; s++) { if (d->code < r * (cum + q[s])) break; cum += q[s]; }
    d->code -= r * cum;
    d->range = (s == K - 1) ? d->range - r * cum : r * q[s];
    while (d->range < TOP) {
        if (d->pos == d->L) return d->err = -113;
        d->code = (d->code << 8) | d->b[d->pos++]; d->range <<= 8;
    }
    return s;
}
int is_rdec_finish(is_rdec *d) {
    if (d->err) return d->err;
    if (d->code != 0) return -115;
    if (d->pos != d->L) return -114;
    return 0;
}
/* ================= rANS decoder (CODER_SPEC 6) ================= */
#define RL (1U << 23)
int is_adec_init(is_adec *d, const uint8_t *b, uint64_t L) {
    memset(d, 0, sizeof *d); d->b = b; d->L = L;
    if (L < 4) return d->err = -113;
    d->x = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    d->pos = 4;
    if (d->x < RL || d->x >= (1U << 31)) return d->err = -115;
    return 0;
}
int is_adec_step(is_adec *d, const uint16_t *q, int K) {
    uint32_t m, cum = 0; int s;
    if (d->err) return d->err;
    m = d->x & 0xFFFF;
    for (s = 0; s < K; s++) { if (m < cum + q[s]) break; cum += q[s]; }
    if (s == K) return d->err = -115;
    d->x = (uint32_t)q[s] * (d->x >> 16) + m - cum;
    while (d->x < RL) {
        if (d->pos == d->L) return d->err = -113;
        d->x = (d->x << 8) | d->b[d->pos++];
    }
    return s;
}
int is_adec_finish(is_adec *d) {
    if (d->err) return d->err;
    if (d->x != RL) return -115;
    if (d->pos != d->L) return -114;
    return 0;
}

/* ================= RNG (UNCERTAINTY_PROTOCOL 4) ================= */
uint64_t is_splitmix64(uint64_t *s) {
    uint64_t z;
    *s += 0x9E3779B97F4A7C15ULL; z = *s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
uint64_t is_draw(uint64_t *s, uint64_t C) {
    uint64_t lim = UINT64_MAX - ((UINT64_MAX % C) + 1) % C, r;
    do r = is_splitmix64(s); while (r > lim);
    return r % C;
}
