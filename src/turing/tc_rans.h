/* Turing calibration reference coder B: rANS, 32-bit state, byte renormalization.
 * Normative description: calibration/docs/CODER_SPEC.md section 6.
 * Consumes only (TPS1, TSY1); never includes or calls the model code.
 *
 * Constants: scale 16 bits (M = 65536), L = 2^23, state x in [L, 2^31).
 * Encode runs from the LAST observation to the first, starting at x = L:
 *   x_max = 32768 * f;   (= ((L >> 16) << 8) * f)
 *   while (x >= x_max) { emit x & 0xFF; x >>= 8; }   (bytes emitted backwards)
 *   x = (x / f) * 65536 + (x % f) + cum;
 * then the final x is written as 4 bytes little-endian in front. The decoder
 * reads the payload forward: x = LE u32 of bytes 0..3, then per symbol
 *   m = x & 0xFFFF; s = the symbol with cum <= m < cum + f;
 *   x = f * (x >> 16) + m - cum; while (x < L) x = (x << 8) | next byte.
 * Termination: after the last symbol x == L and every payload byte consumed.
 */
#ifndef TURING_TC_RANS_H
#define TURING_TC_RANS_H

#include "turing/tc_pstream.h"

#define TC_RANS_MAGIC "TCA1"
#define TC_RANS_ID 2
#define TC_RANS_L (1u << 23)

int tc_rans_encode_raw(const uint16_t *q, unsigned K, const uint8_t *sym, uint64_t n, uint8_t **out, size_t *len);
int tc_rans_decode_raw(const uint16_t *q, unsigned K, const uint8_t *in, size_t len, uint64_t n, uint8_t *sym,
                       char *why, size_t whylen);

/* Coded file = 56-byte header (magic TCA1, coder id 2) || payload. */
int tc_rans_encode(const tc_pstream *p, const tc_symbols *s, uint8_t **out, size_t *len, char *why, size_t whylen);
int tc_rans_decode(const tc_pstream *p, const uint8_t *in, size_t len, tc_symbols *s, char *why, size_t whylen);

#endif /* TURING_TC_RANS_H */
