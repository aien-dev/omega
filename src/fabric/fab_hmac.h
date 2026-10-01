/*
 * fab_hmac.h -- HMAC-SHA256 (RFC 2104) over omega's sha256.c. Used only by the
 * loopback authenticator (fab_loopback.h), the F5-0 stand-in for a real
 * per-machine signature. Checked against RFC 4231 in tests/fabric.
 */
#ifndef FAB_HMAC_H
#define FAB_HMAC_H

#include <stddef.h>
#include <stdint.h>

void fab_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                     uint8_t out[32]);
/* Constant-time compare of n bytes: 1 when equal. */
int fab_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* Stand-in 64-byte signature (Fabric FAB_SIG_BYTES) from one 32-byte key:
 * out[0..32] = HMAC(key, msg), out[32..64] = HMAC(k2, msg) with
 * k2 = HMAC(key, "AFAB stand-in sig64 half 2"). Every byte depends on key and
 * msg; a verifier recomputes and compares all 64. Not a real signature. */
void fab_hmac_sig64(const uint8_t key[32], const uint8_t *msg, size_t msg_len, uint8_t out[64]);

#endif
