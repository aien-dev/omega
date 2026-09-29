/* Turing calibration reference coder A: byte-oriented range coder.
 * Normative description: calibration/docs/CODER_SPEC.md section 5.
 * Consumes only (TPS1, TSY1); never includes or calls the model code.
 *
 * State: low (64-bit, 33 significant bits), range (32-bit), a one-byte cache
 * plus a count of pending 0xFF bytes for explicit carry propagation.
 * Per symbol with row q (sum 65536), cum = q[0] + ... + q[s-1]:
 *   r = range >> 16; low += r * cum;
 *   range = (s == K-1) ? range - r * cum : r * q[s];
 *   while (range < 2^24) { range <<= 8; shift_low(); }
 * Flush: 5 shift_low calls; the payload is exactly the bytes emitted (no
 * leading dummy byte), so the payload value equals the final low. The decoder
 * reads 4 bytes to start and one per renormalization, and must end with
 * code == 0 and every payload byte consumed.
 */
#ifndef TURING_TC_RANGE_H
#define TURING_TC_RANGE_H

#include "turing/tc_pstream.h"

#define TC_RANGE_MAGIC "TCR1"
#define TC_RANGE_ID 1

typedef struct {
    uint64_t carries;     /* carry propagations into already-deferred bytes */
    uint64_t pending_max; /* longest run of deferred 0xFF bytes */
} tc_range_stats;

/* Payload only (no header). q = n rows of K entries. Caller frees *out. st may be NULL. */
int tc_range_encode_raw(const uint16_t *q, unsigned K, const uint8_t *sym, uint64_t n, uint8_t **out, size_t *len,
                        tc_range_stats *st);
int tc_range_decode_raw(const uint16_t *q, unsigned K, const uint8_t *in, size_t len, uint64_t n, uint8_t *sym,
                        char *why, size_t whylen);

/* Coded file = 56-byte header (magic TCR1, coder id 1) || payload. */
int tc_range_encode(const tc_pstream *p, const tc_symbols *s, uint8_t **out, size_t *len, char *why, size_t whylen);
/* Decodes into *s (allocated here; dataset digest copied from the TPS1). */
int tc_range_decode(const tc_pstream *p, const uint8_t *in, size_t len, tc_symbols *s, char *why, size_t whylen);

#endif /* TURING_TC_RANGE_H */
