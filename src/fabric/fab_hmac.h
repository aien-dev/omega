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
/* Constant-time compare of two 32-byte tags: 1 when equal. */
int fab_tag_equal(const uint8_t a[32], const uint8_t b[32]);

#endif
