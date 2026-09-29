/* Turing calibration: the predictor side. Decodes a TYM0 model and walks a
 * CTR1 event stream, emitting the TPS1 probability stream and TSY1 symbol file
 * BEFORE any coding. This is the only tc_* unit that includes ty_model.h; the
 * coders (tc_range, tc_rans) are built and tested without it.
 *
 * Context rule (identical to ty_model_score, src/turing/ty_model.c): previous
 * outcomes reset to K at each crumb start (e->first); key = mixed radix over
 * OP (16), DEPTH (8), PREV1..PREV5 (K+1 each), POS ((crumb << 20) | min(idx,
 * 2^20-1), radix 2^36), in that order, low to high; a key with no stored row
 * uses the default row. tc_produce cross-checks itself: the ideal code length
 * of the emitted stream must equal ty_model_score on the same events.
 */
#ifndef TURING_TC_PRODUCE_H
#define TURING_TC_PRODUCE_H

#include "turing/tc_pstream.h"
#include "turing/ty_model.h"

/* Model digest = ty_model_digest(code bytes) (domain "turing.ymodel.v0"). */
int tc_produce(const ty_model *m, const ty_stream *s, const uint8_t profile[TC_DIGEST],
               const uint8_t model_digest[TC_DIGEST], const uint8_t dataset[TC_DIGEST], tc_pstream *p,
               tc_symbols *sy, char *why, size_t whylen);

/* Load and decode a .tym file; *digest = ty_model_digest of its bytes. */
int tc_load_model(const char *path, ty_model *m, uint8_t digest[TC_DIGEST], uint64_t *lm_bits, char *why,
                  size_t whylen);
/* Plain SHA-256 of a file's bytes (the dataset digest of a CTR1 trace file). */
int tc_file_sha256(const char *path, uint8_t out[TC_DIGEST]);

#endif /* TURING_TC_PRODUCE_H */
