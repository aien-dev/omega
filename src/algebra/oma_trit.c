/* Omega mixed algebra: trit, block, dot, integer conversion. */
#include "algebra/oma_trit.h"

const char *oma_strerror(int rc) {
    switch (rc) {
    case OMA_OK: return "ok";
    case OMA_E_INVALID_TRIT: return "invalid trit value";
    case OMA_E_INVALID_CODE: return "invalid trit code (pos=1,neg=1)";
    case OMA_E_INVALID_PLANES: return "invalid block (pos & neg != 0)";
    case OMA_E_OVERFLOW: return "overflow";
    case OMA_E_INVALID_BYTE: return "invalid dense byte";
    case OMA_E_INVALID_Z3: return "invalid Z3 value";
    case OMA_E_ARG: return "invalid argument";
    default: return "unknown error";
    }
}

static int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return (int)((x * 0x0101010101010101ull) >> 56);
#endif
}

static int ctz64(uint64_t x) { /* x != 0 */
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    int n = 0;
    while (!(x & 1u)) { x >>= 1; n++; }
    return n;
#endif
}

int oma_trit_make(int v, oma_trit *out) {
    if (!out) return OMA_E_ARG;
    if (v < -1 || v > 1) return OMA_E_INVALID_TRIT;
    *out = (oma_trit)v;
    return OMA_OK;
}

int oma_trit_to_code(oma_trit t, uint8_t *code) {
    if (!code) return OMA_E_ARG;
    switch (t) {
    case 0: *code = OMA_CODE_ZERO; return OMA_OK;
    case 1: *code = OMA_CODE_POS; return OMA_OK;
    case -1: *code = OMA_CODE_NEG; return OMA_OK;
    default: return OMA_E_INVALID_TRIT;
    }
}

int oma_code_to_trit(uint8_t code, oma_trit *out) {
    if (!out) return OMA_E_ARG;
    if (code > 2u) return OMA_E_INVALID_CODE; /* 0b11 and anything wider */
    *out = (oma_trit)((int)(code & 1u) - (int)((code >> 1) & 1u));
    return OMA_OK;
}

/* Per-trit ops use the same plane formulas as the block ops, on 1-bit planes. */
int oma_code_neg(uint8_t a, uint8_t *out) {
    if (!out) return OMA_E_ARG;
    if (a > 2u) return OMA_E_INVALID_CODE;
    *out = (uint8_t)(((a & 1u) << 1) | ((a >> 1) & 1u));
    return OMA_OK;
}

int oma_code_add(uint8_t a, uint8_t b, uint8_t *sum, uint8_t *carry) {
    if (!sum || !carry) return OMA_E_ARG;
    if (a > 2u || b > 2u) return OMA_E_INVALID_CODE;
    oma_block A = {a & 1u, (a >> 1) & 1u}, B = {b & 1u, (b >> 1) & 1u}, S, C;
    int rc = oma_block_add(&A, &B, &S, &C);
    if (rc) return rc;
    *sum = (uint8_t)((S.pos & 1u) | ((S.neg & 1u) << 1));
    *carry = (uint8_t)((C.pos & 1u) | ((C.neg & 1u) << 1));
    return OMA_OK;
}

int oma_code_mul(uint8_t a, uint8_t b, uint8_t *out) {
    if (!out) return OMA_E_ARG;
    if (a > 2u || b > 2u) return OMA_E_INVALID_CODE;
    oma_block A = {a & 1u, (a >> 1) & 1u}, B = {b & 1u, (b >> 1) & 1u}, P;
    int rc = oma_block_mul(&A, &B, &P);
    if (rc) return rc;
    *out = (uint8_t)((P.pos & 1u) | ((P.neg & 1u) << 1));
    return OMA_OK;
}

int oma_trit_neg(oma_trit a, oma_trit *out) {
    uint8_t ca, r;
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_trit_to_code(a, &ca))) return rc;
    if ((rc = oma_code_neg(ca, &r))) return rc;
    return oma_code_to_trit(r, out);
}

int oma_trit_add(oma_trit a, oma_trit b, oma_trit *sum, oma_trit *carry) {
    uint8_t ca, cb, s, c;
    int rc;
    if (!sum || !carry) return OMA_E_ARG;
    if ((rc = oma_trit_to_code(a, &ca)) || (rc = oma_trit_to_code(b, &cb))) return rc;
    if ((rc = oma_code_add(ca, cb, &s, &c))) return rc;
    if ((rc = oma_code_to_trit(s, sum))) return rc;
    return oma_code_to_trit(c, carry);
}

int oma_trit_mul(oma_trit a, oma_trit b, oma_trit *out) {
    uint8_t ca, cb, r;
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_trit_to_code(a, &ca)) || (rc = oma_trit_to_code(b, &cb))) return rc;
    if ((rc = oma_code_mul(ca, cb, &r))) return rc;
    return oma_code_to_trit(r, out);
}

/* ---- blocks ---- */
int oma_block_validate(const oma_block *b) {
    if (!b) return OMA_E_ARG;
    return (b->pos & b->neg) ? OMA_E_INVALID_PLANES : OMA_OK;
}

int oma_block_encode(const int8_t in[OMA_BLOCK_TRITS], oma_block *out) {
    uint64_t p = 0, n = 0;
    if (!in || !out) return OMA_E_ARG;
    for (int i = 0; i < OMA_BLOCK_TRITS; i++) {
        if (in[i] == 1) p |= 1ull << i;
        else if (in[i] == -1) n |= 1ull << i;
        else if (in[i] != 0) return OMA_E_INVALID_TRIT;
    }
    out->pos = p;
    out->neg = n;
    return OMA_OK;
}

int oma_block_decode(const oma_block *b, int8_t out[OMA_BLOCK_TRITS]) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_block_validate(b))) return rc;
    for (int i = 0; i < OMA_BLOCK_TRITS; i++)
        out[i] = (int8_t)((int)((b->pos >> i) & 1u) - (int)((b->neg >> i) & 1u));
    return OMA_OK;
}

int oma_block_neg(const oma_block *a, oma_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_block_validate(a))) return rc;
    oma_block r = {a->neg, a->pos};
    *out = r;
    return OMA_OK;
}

int oma_block_add(const oma_block *a, const oma_block *b, oma_block *sum, oma_block *carry) {
    int rc;
    if (!sum || !carry) return OMA_E_ARG;
    if ((rc = oma_block_validate(a)) || (rc = oma_block_validate(b))) return rc;
    uint64_t ap = a->pos, an = a->neg, bp = b->pos, bn = b->neg;
    uint64_t az = ~(ap | an), bz = ~(bp | bn);
    /* a+b in {-2..2}: +1 -> (1,0); +2 -> (-1,+1); -1 -> (-1,0); -2 -> (+1,-1). */
    oma_block s = {(ap & bz) | (az & bp) | (an & bn), (an & bz) | (az & bn) | (ap & bp)};
    oma_block c = {ap & bp, an & bn};
    *sum = s;
    *carry = c;
    return OMA_OK;
}

int oma_block_mul(const oma_block *a, const oma_block *b, oma_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_block_validate(a)) || (rc = oma_block_validate(b))) return rc;
    oma_block r = {(a->pos & b->pos) | (a->neg & b->neg), (a->pos & b->neg) | (a->neg & b->pos)};
    *out = r;
    return OMA_OK;
}

int oma_block_dot(const oma_block *a, const oma_block *b, int32_t *out) {
    oma_block p;
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_block_mul(a, b, &p))) return rc;
    *out = (int32_t)(popcount64(p.pos) - popcount64(p.neg));
    return OMA_OK;
}

int oma_dot_tw_i8(const oma_block *w, const int8_t *x, size_t n, int32_t *out) {
    if (!out || (n && (!w || !x))) return OMA_E_ARG;
    if (n > OMA_DOT_I8_MAX_N) return OMA_E_OVERFLOW;
    size_t nb = (n + 63) / 64;
    int32_t acc = 0; /* |acc| <= 128*n < 2^31 by the n bound */
    for (size_t k = 0; k < nb; k++) {
        int rc = oma_block_validate(&w[k]);
        if (rc) return rc;
        size_t lanes = (n - k * 64) < 64 ? (n - k * 64) : 64;
        uint64_t live = lanes == 64 ? ~0ull : ((1ull << lanes) - 1u);
        if ((w[k].pos | w[k].neg) & ~live) return OMA_E_ARG;
        const int8_t *xs = x + k * 64;
        for (uint64_t m = w[k].pos; m; m &= m - 1) acc += xs[ctz64(m)];
        for (uint64_t m = w[k].neg; m; m &= m - 1) acc -= xs[ctz64(m)];
    }
    *out = acc;
    return OMA_OK;
}

/* ---- integer <-> balanced ternary ---- */
int oma_int_to_bt(int64_t v, int8_t *digits, size_t cap, size_t *ndigits) {
    int8_t tmp[OMA_INT64_MAX_DIGITS];
    size_t n = 0;
    if (!ndigits || (cap && !digits)) return OMA_E_ARG;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v; /* safe for INT64_MIN */
    while (u) {
        unsigned r = (unsigned)(u % 3u);
        u /= 3u;
        if (r == 2u) { tmp[n++] = -1; u += 1u; }
        else tmp[n++] = (int8_t)r;
    }
    if (n > cap) return OMA_E_OVERFLOW;
    for (size_t i = 0; i < n; i++) digits[i] = (int8_t)(v < 0 ? -tmp[i] : tmp[i]);
    *ndigits = n;
    return OMA_OK;
}

int oma_int_to_bt_fixed(int64_t v, int8_t *digits, size_t n) {
    size_t used;
    int rc = oma_int_to_bt(v, digits, n, &used);
    if (rc) return rc;
    for (size_t i = used; i < n; i++) digits[i] = 0;
    return OMA_OK;
}

static int add_i64(int64_t a, int64_t b, int64_t *r) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return OMA_E_OVERFLOW;
    *r = a + b;
    return OMA_OK;
}

int oma_bt_to_int(const int8_t *digits, size_t n, int64_t *out) {
    int64_t acc = 0;
    if (!out || (n && !digits)) return OMA_E_ARG;
    for (size_t i = 0; i < n; i++)
        if (digits[i] < -1 || digits[i] > 1) return OMA_E_INVALID_TRIT;
    for (size_t i = n; i-- > 0;) {
        /* acc*3 + d, ordered (acc+d)+acc+acc so every partial sum lies between
         * zero and the final value: never a false overflow at INT64_MIN. */
        int64_t t;
        if (add_i64(acc, digits[i], &t) || add_i64(t, acc, &t) || add_i64(t, acc, &t))
            return OMA_E_OVERFLOW;
        acc = t;
    }
    *out = acc;
    return OMA_OK;
}
