#include "omega_ternary.h"
#include "omega_canonical.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static const char *const OP_NAMES[TOP_COUNT] = {
    "tneg", "tadd", "tsub", "tmul", "tand", "tor", "txor", "tcmp", "tsign", "tshl", "tshr"
};

const char *omega_t_op_name(TernaryOp op) {
    if (op < TOP_TNEG || op > TOP_TSHR) return "tinvalid";
    return OP_NAMES[op - TOP_TNEG];
}

bool omega_t_op_is_unary(TernaryOp op) {
    return op == TOP_TNEG || op == TOP_TSIGN;
}

bool omega_t_word_valid(int64_t v) {
    return v >= -TW_MAX && v <= TW_MAX;
}

int64_t omega_t_saturate(__int128 v, bool *overflowed) {
    bool ovf = false;
    if (v > TW_MAX) { v = TW_MAX; ovf = true; }
    if (v < -TW_MAX) { v = -TW_MAX; ovf = true; }
    if (overflowed) *overflowed = ovf;
    return (int64_t)v;
}

uint32_t omega_t_trit_len(int64_t v) {
    uint32_t n = 0;
    while (v != 0) {
        int64_t r = v % 3;
        if (r == 2) r = -1;
        if (r == -2) r = 1;
        v = (v - r) / 3;
        n++;
    }
    return n;
}

/* =========================================================================
 * Brute-force digit model
 * ========================================================================= */

void omega_tv_from_int(int64_t v, uint8_t n, TritVec *out) {
    memset(out, 0, sizeof(*out));
    out->n = n;
    __int128 x = v;
    for (uint8_t i = 0; i < n; ++i) {
        int r = (int)(x % 3);
        if (r == 2) r = -1;
        if (r == -2) r = 1;
        out->d[i] = (int8_t)r;
        x = (x - r) / 3;
    }
}

__int128 omega_tv_to_int(const TritVec *tv) {
    __int128 acc = 0;
    for (int i = (int)tv->n - 1; i >= 0; --i) {
        acc = acc * 3 + tv->d[i];
    }
    return acc;
}

/* Digit-serial add with balanced carry; out may alias a or b. */
static void tv_add(const TritVec *a, const TritVec *b, TritVec *out) {
    int carry = 0;
    uint8_t n = a->n;
    for (uint8_t i = 0; i < n; ++i) {
        int s = a->d[i] + b->d[i] + carry;
        carry = 0;
        if (s > 1) { s -= 3; carry = 1; }
        if (s < -1) { s += 3; carry = -1; }
        out->d[i] = (int8_t)s;
    }
    out->n = n;
}

static void tv_neg(const TritVec *a, TritVec *out) {
    out->n = a->n;
    for (uint8_t i = 0; i < a->n; ++i) out->d[i] = (int8_t)-a->d[i];
}

static int tv_sign(const TritVec *a) {
    for (int i = (int)a->n - 1; i >= 0; --i) {
        if (a->d[i] != 0) return a->d[i];
    }
    return 0;
}

/* Collapse a wide digit vector to a T32 result: overflow iff any digit at or
 * above TW_TRITS is non-zero; saturation direction is the sign of the
 * most significant non-zero digit. */
static int64_t tv_narrow(const TritVec *wide, bool *overflowed) {
    bool ovf = false;
    for (uint8_t i = TW_TRITS; i < wide->n; ++i) {
        if (wide->d[i] != 0) { ovf = true; break; }
    }
    if (overflowed) *overflowed = ovf;
    if (ovf) return tv_sign(wide) > 0 ? TW_MAX : -TW_MAX;
    TritVec narrow = *wide;
    narrow.n = TW_TRITS;
    return (int64_t)omega_tv_to_int(&narrow);
}

static int8_t trit_min(int8_t a, int8_t b) { return a < b ? a : b; }
static int8_t trit_max(int8_t a, int8_t b) { return a > b ? a : b; }

int omega_t_eval_brute(TernaryOp op, int64_t a, int64_t b, int64_t *out, bool *overflowed) {
    if (!out || !omega_t_word_valid(a)) return -1;
    bool is_shift = (op == TOP_TSHL || op == TOP_TSHR);
    if (is_shift) {
        if (b < 0 || b > 64) return -1;
    } else if (!omega_t_op_is_unary(op) && !omega_t_word_valid(b)) {
        return -1;
    }
    if (overflowed) *overflowed = false;

    TritVec ta, tb, tr;
    omega_tv_from_int(a, TV_MAX_DIGITS, &ta);
    omega_tv_from_int(is_shift ? 0 : b, TV_MAX_DIGITS, &tb);
    memset(&tr, 0, sizeof(tr));
    tr.n = TV_MAX_DIGITS;

    switch (op) {
        case TOP_TNEG:
            tv_neg(&ta, &tr);
            *out = tv_narrow(&tr, overflowed);
            return 0;
        case TOP_TADD:
            tv_add(&ta, &tb, &tr);
            *out = tv_narrow(&tr, overflowed);
            return 0;
        case TOP_TSUB: {
            TritVec nb;
            tv_neg(&tb, &nb);
            tv_add(&ta, &nb, &tr);
            *out = tv_narrow(&tr, overflowed);
            return 0;
        }
        case TOP_TMUL: {
            /* Schoolbook: for each trit of b, add or subtract a shifted copy of a. */
            for (uint8_t j = 0; j < TW_TRITS; ++j) {
                if (tb.d[j] == 0) continue;
                TritVec shifted;
                memset(&shifted, 0, sizeof(shifted));
                shifted.n = TV_MAX_DIGITS;
                for (uint8_t i = 0; i + j < TV_MAX_DIGITS && i < TW_TRITS; ++i) {
                    shifted.d[i + j] = (int8_t)(ta.d[i] * tb.d[j]);
                }
                tv_add(&tr, &shifted, &tr);
            }
            *out = tv_narrow(&tr, overflowed);
            return 0;
        }
        case TOP_TAND:
        case TOP_TOR:
        case TOP_TXOR:
            for (uint8_t i = 0; i < TW_TRITS; ++i) {
                int8_t x = ta.d[i], y = tb.d[i];
                tr.d[i] = (op == TOP_TAND) ? trit_min(x, y)
                        : (op == TOP_TOR) ? trit_max(x, y)
                        : (int8_t)-(x * y);
            }
            *out = tv_narrow(&tr, overflowed);
            return 0;
        case TOP_TCMP: {
            /* Lexicographic from the most significant trit: in balanced ternary
             * digit order and numeric order coincide. */
            int s = 0;
            for (int i = TW_TRITS - 1; i >= 0 && s == 0; --i) {
                if (ta.d[i] != tb.d[i]) s = (ta.d[i] > tb.d[i]) ? 1 : -1;
            }
            *out = s;
            return 0;
        }
        case TOP_TSIGN:
            *out = tv_sign(&ta);
            return 0;
        case TOP_TSHL:
            for (int i = 0; i < TV_MAX_DIGITS; ++i) {
                int src = i - (int)b;
                tr.d[i] = (src >= 0 && src < TW_TRITS) ? ta.d[src] : 0;
            }
            /* Trits shifted past the vector top still count as overflow. */
            for (int i = TW_TRITS - (int)b; i < TW_TRITS && b > 0; ++i) {
                if (i >= 0 && i + (int)b >= TV_MAX_DIGITS && ta.d[i] != 0) {
                    if (overflowed) *overflowed = true;
                    *out = tv_sign(&ta) > 0 ? TW_MAX : -TW_MAX;
                    return 0;
                }
            }
            *out = tv_narrow(&tr, overflowed);
            return 0;
        case TOP_TSHR:
            for (int i = 0; i < TW_TRITS; ++i) {
                int src = i + (int)b;
                tr.d[i] = (src < TW_TRITS) ? ta.d[src] : 0;
            }
            *out = tv_narrow(&tr, overflowed);
            return 0;
        default:
            return -1;
    }
}

/* =========================================================================
 * Packed planes
 * ========================================================================= */

uint64_t omega_t_to_planes(int64_t v) {
    uint64_t pos = 0, neg = 0;
    for (uint32_t i = 0; i < TW_TRITS && v != 0; ++i) {
        int64_t r = v % 3;
        if (r == 2) r = -1;
        if (r == -2) r = 1;
        if (r == 1) pos |= (1ULL << i);
        if (r == -1) neg |= (1ULL << i);
        v = (v - r) / 3;
    }
    return pos | (neg << 32);
}

int64_t omega_t_from_planes(uint64_t planes) {
    uint32_t pos = (uint32_t)planes, neg = (uint32_t)(planes >> 32);
    int64_t acc = 0;
    for (int i = TW_TRITS - 1; i >= 0; --i) {
        acc = acc * 3 + (int64_t)((pos >> i) & 1) - (int64_t)((neg >> i) & 1);
    }
    return acc;
}

bool omega_t_planes_valid(uint64_t planes) {
    return (((uint32_t)planes) & (uint32_t)(planes >> 32)) == 0;
}

/* =========================================================================
 * Fast model
 * ========================================================================= */

static int64_t round_div_pow3(int64_t a, uint32_t k) {
    if (k >= TW_TRITS) return 0;
    int64_t d = 1;
    for (uint32_t i = 0; i < k; ++i) d *= 3;
    int64_t q = a / d, r = a - q * d;
    if (2 * r > d) q++;
    if (2 * r < -d) q--;
    return q;
}

int omega_t_eval(TernaryOp op, int64_t a, int64_t b, int64_t *out, bool *overflowed) {
    if (!out || !omega_t_word_valid(a)) return -1;
    bool is_shift = (op == TOP_TSHL || op == TOP_TSHR);
    if (is_shift) {
        if (b < 0 || b > 64) return -1;
    } else if (!omega_t_op_is_unary(op) && !omega_t_word_valid(b)) {
        return -1;
    }
    if (overflowed) *overflowed = false;

    switch (op) {
        case TOP_TNEG: *out = -a; return 0;
        case TOP_TADD: *out = omega_t_saturate((__int128)a + b, overflowed); return 0;
        case TOP_TSUB: *out = omega_t_saturate((__int128)a - b, overflowed); return 0;
        case TOP_TMUL: *out = omega_t_saturate((__int128)a * b, overflowed); return 0;
        case TOP_TAND:
        case TOP_TOR:
        case TOP_TXOR: {
            uint64_t pa = omega_t_to_planes(a), pb = omega_t_to_planes(b);
            uint32_t ap = (uint32_t)pa, an = (uint32_t)(pa >> 32);
            uint32_t bp = (uint32_t)pb, bn = (uint32_t)(pb >> 32);
            uint32_t rp, rn;
            if (op == TOP_TAND) { rp = ap & bp; rn = an | bn; }
            else if (op == TOP_TOR) { rp = ap | bp; rn = an & bn; }
            else { rp = (ap & bn) | (an & bp); rn = (ap & bp) | (an & bn); }
            *out = omega_t_from_planes((uint64_t)rp | ((uint64_t)rn << 32));
            return 0;
        }
        case TOP_TCMP: *out = (a > b) - (a < b); return 0;
        case TOP_TSIGN: *out = (a > 0) - (a < 0); return 0;
        case TOP_TSHL: {
            __int128 v = a;
            for (int64_t i = 0; i < b && v != 0; ++i) {
                v *= 3;
                if (v > TW_MAX || v < -TW_MAX) break;
            }
            *out = omega_t_saturate(v, overflowed);
            return 0;
        }
        case TOP_TSHR: *out = round_div_pow3(a, (uint32_t)b); return 0;
        default: return -1;
    }
}

/* =========================================================================
 * Model cross-check
 * ========================================================================= */

static uint64_t lcg_next(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

/* Pseudo-random word with a random significant trit length (1..32). */
static int64_t random_word(uint64_t *s) {
    uint32_t len = (uint32_t)(lcg_next(s) >> 59) + 1; /* 1..32 */
    int64_t v = 0;
    for (uint32_t i = 0; i < len; ++i) {
        v = v * 3 + (int64_t)((lcg_next(s) >> 33) % 3) - 1;
    }
    return v;
}

static const TernaryOp ALL_OPS[TOP_COUNT] = {
    TOP_TNEG, TOP_TADD, TOP_TSUB, TOP_TMUL, TOP_TAND, TOP_TOR,
    TOP_TXOR, TOP_TCMP, TOP_TSIGN, TOP_TSHL, TOP_TSHR
};

static bool check_pair(TernaryOp op, int64_t a, int64_t b, size_t *checks, char *err, size_t err_len) {
    int64_t r1 = 0, r2 = 0;
    bool o1 = false, o2 = false;
    (*checks)++;
    int e1 = omega_t_eval(op, a, b, &r1, &o1);
    int e2 = omega_t_eval_brute(op, a, b, &r2, &o2);
    if (e1 != e2 || (e1 == 0 && (r1 != r2 || o1 != o2))) {
        if (err) {
            snprintf(err, err_len, "%s(%lld, %lld): fast=%lld/%d brute=%lld/%d",
                     omega_t_op_name(op), (long long)a, (long long)b,
                     (long long)r1, (int)o1, (long long)r2, (int)o2);
        }
        return false;
    }
    return true;
}

size_t omega_t_selfcheck_models(size_t *out_checks, char *err, size_t err_len) {
    size_t checks = 0, fails = 0;

    /* 1. Exhaustive: every pair of 6-trit words (729 x 729) for every op. */
    for (size_t o = 0; o < TOP_COUNT; ++o) {
        TernaryOp op = ALL_OPS[o];
        for (int64_t a = -364; a <= 364; ++a) {
            if (op == TOP_TSHL || op == TOP_TSHR) {
                for (int64_t k = 0; k <= 34; ++k) {
                    if (!check_pair(op, a, k, &checks, err, err_len)) fails++;
                }
            } else if (omega_t_op_is_unary(op)) {
                if (!check_pair(op, a, 0, &checks, err, err_len)) fails++;
            } else {
                for (int64_t b = -364; b <= 364; ++b) {
                    if (!check_pair(op, a, b, &checks, err, err_len)) fails++;
                }
            }
        }
    }

    /* 2. Boundary set: every pair among symmetric extremes and powers of 3. */
    int64_t edges[24];
    size_t ne = 0;
    int64_t p3 = 1;
    edges[ne++] = 0;
    edges[ne++] = TW_MAX; edges[ne++] = -TW_MAX;
    edges[ne++] = TW_MAX - 1; edges[ne++] = -(TW_MAX - 1);
    for (int i = 0; i < 32 && ne + 2 <= 24; i += 3) {
        edges[ne++] = p3; edges[ne++] = -p3;
        p3 *= 27;
    }
    for (size_t o = 0; o < TOP_COUNT; ++o) {
        for (size_t i = 0; i < ne; ++i) {
            for (size_t j = 0; j < ne; ++j) {
                TernaryOp op = ALL_OPS[o];
                int64_t b = (op == TOP_TSHL || op == TOP_TSHR) ? (int64_t)(j * 3) : edges[j];
                if (!check_pair(op, edges[i], b, &checks, err, err_len)) fails++;
            }
        }
    }

    /* 3. Pseudo-random full-width operands with random trit lengths. */
    uint64_t seed = 0x7E5A7E5A0000001DULL;
    for (size_t n = 0; n < 200000; ++n) {
        TernaryOp op = ALL_OPS[n % TOP_COUNT];
        int64_t a = random_word(&seed);
        int64_t b = (op == TOP_TSHL || op == TOP_TSHR) ? (int64_t)(lcg_next(&seed) >> 58)
                                                       : random_word(&seed);
        if (!check_pair(op, a, b, &checks, err, err_len)) fails++;
    }

    if (out_checks) *out_checks = checks;
    return fails;
}

/* =========================================================================
 * OMG1 canonical encoding
 * ========================================================================= */

#define TRYTE_TRITS 5
#define TW_TRYTES ((TW_TRITS + TRYTE_TRITS - 1) / TRYTE_TRITS)

static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

int omega_t_build_type_word(OmegaObject *obj) {
    if (!obj) return -1;
    memset(obj, 0, sizeof(*obj));
    obj->kind = KIND_TYPE;
    obj->payload[0] = TYPE_T_WORD;
    put_be16(&obj->payload[1], TW_TRITS);
    obj->payload_len = 3;
    return omega_t_compute_semantic_id(obj);
}

int omega_t_build_value(OmegaObject *obj, const SemanticId *type_id, int64_t v) {
    if (!obj || !type_id || !omega_t_word_valid(v)) return -1;
    memset(obj, 0, sizeof(*obj));
    obj->kind = KIND_VALUE;
    uint8_t *p = obj->payload;
    memcpy(p, type_id->bytes, OMEGA_ID_BYTES);
    put_be16(p + OMEGA_ID_BYTES, TW_TRITS);
    TritVec tv;
    omega_tv_from_int(v, TW_TRYTES * TRYTE_TRITS, &tv);
    for (size_t t = 0; t < TW_TRYTES; ++t) {
        unsigned byte = 0, w = 1;
        for (size_t i = 0; i < TRYTE_TRITS; ++i) {
            byte += (unsigned)(tv.d[t * TRYTE_TRITS + i] + 1) * w;
            w *= 3;
        }
        p[OMEGA_ID_BYTES + 2 + t] = (uint8_t)byte;
    }
    obj->payload_len = OMEGA_ID_BYTES + 2 + TW_TRYTES;
    return omega_t_compute_semantic_id(obj);
}

int omega_t_value_decode(const OmegaObject *obj, int64_t *out_v) {
    if (!obj || obj->kind != KIND_VALUE) return -1;
    if (obj->payload_len != OMEGA_ID_BYTES + 2 + TW_TRYTES) return -1;
    const uint8_t *p = obj->payload;
    if (get_be16(p + OMEGA_ID_BYTES) != TW_TRITS) return -1;
    TritVec tv;
    memset(&tv, 0, sizeof(tv));
    tv.n = TW_TRYTES * TRYTE_TRITS;
    for (size_t t = 0; t < TW_TRYTES; ++t) {
        unsigned byte = p[OMEGA_ID_BYTES + 2 + t];
        if (byte >= 243) return -1; /* 3^5: non-canonical tryte */
        for (size_t i = 0; i < TRYTE_TRITS; ++i) {
            tv.d[t * TRYTE_TRITS + i] = (int8_t)((int)(byte % 3) - 1);
            byte /= 3;
        }
    }
    /* Padding trits above the word width must be zero. */
    for (size_t i = TW_TRITS; i < tv.n; ++i) {
        if (tv.d[i] != 0) return -1;
    }
    if (out_v) *out_v = (int64_t)omega_tv_to_int(&tv);
    return 0;
}

int omega_t_build_op(OmegaObject *obj, TernaryOp op, const SemanticId *type_id) {
    if (!obj || !type_id || op < TOP_TNEG || op > TOP_TSHR) return -1;
    memset(obj, 0, sizeof(*obj));
    obj->kind = KIND_OPERATION;
    obj->payload[0] = (uint8_t)op;
    obj->payload[1] = OVERFLOW_T_SATURATE_SYMMETRIC;
    memcpy(&obj->payload[2], type_id->bytes, OMEGA_ID_BYTES);
    obj->payload[2 + OMEGA_ID_BYTES] = omega_t_op_is_unary(op) ? 1 : 2;
    obj->payload_len = 3 + OMEGA_ID_BYTES;
    return omega_t_compute_semantic_id(obj);
}

static int validate_ternary_payload(const OmegaObject *obj) {
    switch (obj->kind) {
        case KIND_TYPE:
            if (obj->payload_len != 3) return -1;
            if (obj->payload[0] != TYPE_T_WORD && obj->payload[0] != TYPE_T_TRIT) return -1;
            return 0;
        case KIND_VALUE:
            return omega_t_value_decode(obj, NULL);
        case KIND_OPERATION:
            if (obj->payload_len != 3 + OMEGA_ID_BYTES) return -1;
            if (obj->payload[0] < TOP_TNEG || obj->payload[0] > TOP_TSHR) return -1;
            if (obj->payload[1] != OVERFLOW_T_SATURATE_SYMMETRIC) return -1;
            return 0;
        default:
            return 0;
    }
}

int omega_t_canonical_encode(const OmegaObject *obj, uint8_t *out_buf, size_t max_len, size_t *out_len) {
    if (!obj || !out_buf || !out_len) return -1;
    if (validate_ternary_payload(obj) != 0) return -1;
    /* OMG1 shares the OMG0 grammar; only the magic and the payload formats differ. */
    if (omega_canonical_encode(obj, out_buf, max_len, out_len) != 0) return -1;
    if (*out_len < 4) return -1;
    out_buf[3] = OMEGA_T_MAGIC_BYTE3;
    return 0;
}

int omega_t_compute_semantic_id(OmegaObject *obj) {
    if (!obj) return -1;
    uint8_t buf[4096];
    size_t len = 0;
    if (omega_t_canonical_encode(obj, buf, sizeof(buf), &len) != 0) return -1;
    sha256_hash(buf, len, obj->id.bytes);
    obj->has_id = true;
    return 0;
}
