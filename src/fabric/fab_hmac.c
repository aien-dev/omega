/* fab_hmac.c -- HMAC-SHA256. See fab_hmac.h. */
#include "fab_hmac.h"

#include "sha256.h"

#include <string.h>

void fab_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]) {
    uint8_t k[SHA256_BLOCK_SIZE], pad[SHA256_BLOCK_SIZE], inner[SHA256_DIGEST_SIZE];
    memset(k, 0, sizeof k);
    if (key_len > SHA256_BLOCK_SIZE) sha256_hash(key, key_len, k);
    else if (key_len) memcpy(k, key, key_len);
    sha256_ctx h;
    for (unsigned i = 0; i < SHA256_BLOCK_SIZE; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&h);
    sha256_update(&h, pad, sizeof pad);
    if (msg_len) sha256_update(&h, msg, msg_len);
    sha256_final(&h, inner);
    for (unsigned i = 0; i < SHA256_BLOCK_SIZE; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&h);
    sha256_update(&h, pad, sizeof pad);
    sha256_update(&h, inner, sizeof inner);
    sha256_final(&h, out);
    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
}

int fab_ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= (uint8_t)(a[i] ^ b[i]);
    return acc == 0;
}

void fab_hmac_sig64(const uint8_t key[32], const uint8_t *msg, size_t msg_len, uint8_t out[64]) {
    static const char half2[] = "AFAB stand-in sig64 half 2";
    uint8_t k2[32];
    fab_hmac_sha256(key, 32, msg, msg_len, out);
    fab_hmac_sha256(key, 32, (const uint8_t *)half2, sizeof half2 - 1, k2);
    fab_hmac_sha256(k2, 32, msg, msg_len, out + 32);
    memset(k2, 0, sizeof k2);
}
