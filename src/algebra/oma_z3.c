#include "algebra/oma_z3.h"

int oma_z3_make(int v, oma_z3 *out) {
    if (!out) return OMA_E_ARG;
    if (v < 0 || v > 2) return OMA_E_INVALID_Z3;
    *out = (oma_z3)v;
    return OMA_OK;
}

int oma_z3_add(int a, int b, oma_z3 *out) {
    if (!out) return OMA_E_ARG;
    if (a < 0 || a > 2 || b < 0 || b > 2) return OMA_E_INVALID_Z3;
    *out = (oma_z3)((a + b) % 3);
    return OMA_OK;
}

int oma_z3_mul(int a, int b, oma_z3 *out) {
    if (!out) return OMA_E_ARG;
    if (a < 0 || a > 2 || b < 0 || b > 2) return OMA_E_INVALID_Z3;
    *out = (oma_z3)((a * b) % 3);
    return OMA_OK;
}

int oma_z3_neg(int a, oma_z3 *out) {
    if (!out) return OMA_E_ARG;
    if (a < 0 || a > 2) return OMA_E_INVALID_Z3;
    *out = (oma_z3)((3 - a) % 3);
    return OMA_OK;
}

int oma_z3_to_trit(int a, oma_trit *out) {
    if (!out) return OMA_E_ARG;
    if (a < 0 || a > 2) return OMA_E_INVALID_Z3;
    *out = (oma_trit)(a == 2 ? -1 : (int)a);
    return OMA_OK;
}

int oma_trit_to_z3(int t, oma_z3 *out) {
    if (!out) return OMA_E_ARG;
    if (t < -1 || t > 1) return OMA_E_INVALID_TRIT;
    *out = (oma_z3)(t < 0 ? 2 : t);
    return OMA_OK;
}

static oma_block as_trits(const oma_z3_block *z) {
    oma_block t = {z->one, z->two};
    return t;
}

static oma_z3_block as_z3(const oma_block *t) {
    oma_z3_block z = {t->pos, t->neg};
    return z;
}

int oma_z3_block_validate(const oma_z3_block *b) {
    if (!b) return OMA_E_ARG;
    return (b->one & b->two) ? OMA_E_INVALID_PLANES : OMA_OK;
}

int oma_z3_block_encode(const uint8_t in[OMA_BLOCK_TRITS], oma_z3_block *out) {
    uint64_t o = 0, t = 0;
    if (!in || !out) return OMA_E_ARG;
    for (int i = 0; i < OMA_BLOCK_TRITS; i++) {
        if (in[i] == 1) o |= 1ull << i;
        else if (in[i] == 2) t |= 1ull << i;
        else if (in[i] != 0) return OMA_E_INVALID_Z3;
    }
    out->one = o;
    out->two = t;
    return OMA_OK;
}

int oma_z3_block_decode(const oma_z3_block *b, uint8_t out[OMA_BLOCK_TRITS]) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_z3_block_validate(b))) return rc;
    for (int i = 0; i < OMA_BLOCK_TRITS; i++)
        out[i] = (uint8_t)(((b->one >> i) & 1u) | (((b->two >> i) & 1u) << 1));
    return OMA_OK;
}

int oma_z3_block_from_trits(const oma_block *t, oma_z3_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_block_validate(t))) return rc;
    *out = as_z3(t);
    return OMA_OK;
}

int oma_z3_block_to_trits(const oma_z3_block *z, oma_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_z3_block_validate(z))) return rc;
    *out = as_trits(z);
    return OMA_OK;
}

int oma_z3_block_add(const oma_z3_block *a, const oma_z3_block *b, oma_z3_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_z3_block_validate(a)) || (rc = oma_z3_block_validate(b))) return rc;
    oma_block ta = as_trits(a), tb = as_trits(b), s, c;
    if ((rc = oma_block_add(&ta, &tb, &s, &c))) return rc;
    *out = as_z3(&s); /* carry discarded: arithmetic mod 3 */
    return OMA_OK;
}

int oma_z3_block_mul(const oma_z3_block *a, const oma_z3_block *b, oma_z3_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_z3_block_validate(a)) || (rc = oma_z3_block_validate(b))) return rc;
    oma_block ta = as_trits(a), tb = as_trits(b), p;
    if ((rc = oma_block_mul(&ta, &tb, &p))) return rc;
    *out = as_z3(&p);
    return OMA_OK;
}

int oma_z3_block_neg(const oma_z3_block *a, oma_z3_block *out) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_z3_block_validate(a))) return rc;
    oma_z3_block r = {a->two, a->one};
    *out = r;
    return OMA_OK;
}
