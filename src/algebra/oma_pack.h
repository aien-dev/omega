/* Omega mixed algebra: packing.
 *  - bitplane form: n trits -> ceil(n/64) oma_blocks, unused lanes zero;
 *    serialized as 16 bytes per block: pos then neg, each little-endian.
 *  - dense form: 5 trits per byte, byte = sum_{i<5} (t_i + 1) * 3^i
 *    (least significant trit first), valid bytes 0..242. Same rule as the
 *    parked OMG1 tryte encoding. Padding trits in the last byte must be 0.
 * Every function validates its whole input before writing: on error the
 * output buffers are untouched. */
#ifndef OMA_PACK_H
#define OMA_PACK_H

#include "algebra/oma_trit.h"

#define OMA_DENSE_TRITS_PER_BYTE 5
#define OMA_DENSE_BYTE_LIMIT 243 /* 3^5 */
#define OMA_BLOCK_BYTES 16

size_t oma_bitplane_blocks(size_t n);
size_t oma_dense_bytes(size_t n);

int oma_pack_bitplane(const int8_t *t, size_t n, oma_block *out, size_t out_blocks);
/* Rejects invalid planes and any non-zero lane at index >= n. */
int oma_unpack_bitplane(const oma_block *in, size_t in_blocks, size_t n, int8_t *out);

int oma_block_serialize(const oma_block *b, uint8_t out[OMA_BLOCK_BYTES]);
int oma_block_deserialize(const uint8_t in[OMA_BLOCK_BYTES], oma_block *out);

int oma_pack_dense(const int8_t *t, size_t n, uint8_t *out, size_t out_cap, size_t *out_len);
/* Byte >= 243 or non-zero padding trit -> OMA_E_INVALID_BYTE. */
int oma_unpack_dense(const uint8_t *in, size_t in_len, size_t n, int8_t *out);

/* One byte -> 5 trits. OMA_E_INVALID_BYTE for byte < 0 or >= 243 (int, so
 * a wide value is rejected rather than wrapped). */
int oma_dense_byte_decode(int byte, int8_t out[OMA_DENSE_TRITS_PER_BYTE]);

#endif /* OMA_PACK_H */
