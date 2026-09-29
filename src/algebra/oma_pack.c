#include "algebra/oma_pack.h"

/* Every function here validates its whole input before writing any output,
 * so on error the output buffers are left untouched (spec: return codes). */

size_t oma_bitplane_blocks(size_t n) { return n / 64 + (n % 64 != 0); }
size_t oma_dense_bytes(size_t n) { return n / 5 + (n % 5 != 0); }

int oma_pack_bitplane(const int8_t *t, size_t n, oma_block *out, size_t out_blocks) {
    size_t nb = oma_bitplane_blocks(n);
    if ((n && !t) || (nb && !out)) return OMA_E_ARG;
    if (out_blocks < nb) return OMA_E_OVERFLOW;
    for (size_t i = 0; i < n; i++)
        if (t[i] < -1 || t[i] > 1) return OMA_E_INVALID_TRIT;
    for (size_t k = 0; k < nb; k++) {
        int8_t lane[OMA_BLOCK_TRITS] = {0};
        size_t m = n - k * 64 < 64 ? n - k * 64 : 64;
        for (size_t i = 0; i < m; i++) lane[i] = t[k * 64 + i];
        int rc = oma_block_encode(lane, &out[k]); /* cannot fail: trits checked */
        if (rc) return rc;
    }
    return OMA_OK;
}

int oma_unpack_bitplane(const oma_block *in, size_t in_blocks, size_t n, int8_t *out) {
    size_t nb = oma_bitplane_blocks(n);
    if ((n && !out) || (nb && !in)) return OMA_E_ARG;
    if (in_blocks < nb) return OMA_E_ARG;
    /* pass 1: validate every block and every unused lane */
    for (size_t k = 0; k < nb; k++) {
        int rc = oma_block_validate(&in[k]);
        if (rc) return rc;
        size_t m = n - k * 64 < 64 ? n - k * 64 : 64;
        uint64_t live = m == 64 ? ~0ull : ((1ull << m) - 1u);
        if ((in[k].pos | in[k].neg) & ~live) return OMA_E_ARG;
    }
    /* pass 2: decode */
    for (size_t k = 0; k < nb; k++) {
        int8_t lane[OMA_BLOCK_TRITS];
        int rc = oma_block_decode(&in[k], lane);
        if (rc) return rc;
        size_t m = n - k * 64 < 64 ? n - k * 64 : 64;
        for (size_t i = 0; i < m; i++) out[k * 64 + i] = lane[i];
    }
    return OMA_OK;
}

int oma_block_serialize(const oma_block *b, uint8_t out[OMA_BLOCK_BYTES]) {
    int rc;
    if (!out) return OMA_E_ARG;
    if ((rc = oma_block_validate(b))) return rc;
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(b->pos >> (8 * i));
        out[8 + i] = (uint8_t)(b->neg >> (8 * i));
    }
    return OMA_OK;
}

int oma_block_deserialize(const uint8_t in[OMA_BLOCK_BYTES], oma_block *out) {
    if (!in || !out) return OMA_E_ARG;
    oma_block b = {0, 0};
    for (int i = 0; i < 8; i++) {
        b.pos |= (uint64_t)in[i] << (8 * i);
        b.neg |= (uint64_t)in[8 + i] << (8 * i);
    }
    int rc = oma_block_validate(&b);
    if (rc) return rc;
    *out = b;
    return OMA_OK;
}

int oma_pack_dense(const int8_t *t, size_t n, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t nbytes = oma_dense_bytes(n);
    if (!out_len || (n && !t) || (nbytes && !out)) return OMA_E_ARG;
    if (out_cap < nbytes) return OMA_E_OVERFLOW;
    for (size_t i = 0; i < n; i++)
        if (t[i] < -1 || t[i] > 1) return OMA_E_INVALID_TRIT;
    for (size_t k = 0; k < nbytes; k++) {
        unsigned v = 0, p = 1;
        for (size_t j = 0; j < 5; j++) {
            size_t idx = k * 5 + j;
            int d = idx < n ? t[idx] : 0;
            v += (unsigned)(d + 1) * p;
            p *= 3u;
        }
        out[k] = (uint8_t)v;
    }
    *out_len = nbytes;
    return OMA_OK;
}

int oma_dense_byte_decode(int byte, int8_t out[OMA_DENSE_TRITS_PER_BYTE]) {
    if (!out) return OMA_E_ARG;
    if (byte < 0 || byte >= OMA_DENSE_BYTE_LIMIT) return OMA_E_INVALID_BYTE;
    unsigned v = (unsigned)byte;
    for (int j = 0; j < 5; j++) {
        out[j] = (int8_t)((int)(v % 3u) - 1);
        v /= 3u;
    }
    return OMA_OK;
}

int oma_unpack_dense(const uint8_t *in, size_t in_len, size_t n, int8_t *out) {
    size_t nbytes = oma_dense_bytes(n);
    if ((n && !out) || (nbytes && !in)) return OMA_E_ARG;
    if (in_len < nbytes) return OMA_E_ARG;
    /* pass 1: every byte < 243, padding trits of the last byte zero */
    for (size_t k = 0; k < nbytes; k++) {
        int8_t d[OMA_DENSE_TRITS_PER_BYTE];
        int rc = oma_dense_byte_decode(in[k], d);
        if (rc) return rc;
        for (size_t j = 0; j < OMA_DENSE_TRITS_PER_BYTE; j++)
            if (k * 5 + j >= n && d[j] != 0) return OMA_E_INVALID_BYTE; /* non-canonical padding */
    }
    /* pass 2: decode */
    for (size_t k = 0; k < nbytes; k++) {
        int8_t d[OMA_DENSE_TRITS_PER_BYTE];
        int rc = oma_dense_byte_decode(in[k], d);
        if (rc) return rc;
        for (size_t j = 0; j < OMA_DENSE_TRITS_PER_BYTE && k * 5 + j < n; j++) out[k * 5 + j] = d[j];
    }
    return OMA_OK;
}
