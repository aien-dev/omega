/* Omega mixed algebra: Z3 (integers mod 3, values {0,1,2}).
 * Map to balanced trits: 0 <-> 0, 1 <-> +1, 2 <-> -1. This map is a ring
 * isomorphism: Z3 add = balanced-add sum trit (carry dropped), Z3 mul = H2. */
#ifndef OMA_Z3_H
#define OMA_Z3_H

#include "algebra/oma_trit.h"

typedef uint8_t oma_z3; /* one of 0, 1, 2 */

int oma_z3_make(int v, oma_z3 *out);
int oma_z3_add(oma_z3 a, oma_z3 b, oma_z3 *out);
int oma_z3_mul(oma_z3 a, oma_z3 b, oma_z3 *out);
int oma_z3_neg(oma_z3 a, oma_z3 *out);
int oma_z3_to_trit(oma_z3 a, oma_trit *out);
int oma_trit_to_z3(oma_trit t, oma_z3 *out);

/* Z3 block on bitplanes: {one, two}, lane i = bit i. Under the map above
 * it is bit-identical to an oma_block ({pos, neg}); (1,1) is invalid. */
typedef struct {
    uint64_t one;
    uint64_t two;
} oma_z3_block;

int oma_z3_block_validate(const oma_z3_block *b);
int oma_z3_block_encode(const uint8_t in[OMA_BLOCK_TRITS], oma_z3_block *out);
int oma_z3_block_decode(const oma_z3_block *b, uint8_t out[OMA_BLOCK_TRITS]);
int oma_z3_block_from_trits(const oma_block *t, oma_z3_block *out);
int oma_z3_block_to_trits(const oma_z3_block *z, oma_block *out);
/* Carry-free lane-wise ops. */
int oma_z3_block_add(const oma_z3_block *a, const oma_z3_block *b, oma_z3_block *out);
int oma_z3_block_mul(const oma_z3_block *a, const oma_z3_block *b, oma_z3_block *out);
int oma_z3_block_neg(const oma_z3_block *a, oma_z3_block *out);

#endif /* OMA_Z3_H */
