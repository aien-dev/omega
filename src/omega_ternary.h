/* =========================================================================
 * TERNARY SEMANTICS EXPERIMENT - ADDITIVE, NON-CANONICAL PROFILE
 * Balanced-ternary semantic profile for OMEGA (spec/ternary-semantics.md).
 * The binary profile (OMG0) is untouched and remains canonical.
 * ========================================================================= */

#ifndef OMEGA_TERNARY_H
#define OMEGA_TERNARY_H

#include "omega_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "OMG1" canonical magic for ternary semantic graphs (0x4F,0x4D,0x47,0x31). */
#define OMEGA_T_MAGIC 0x31474D4F
#define OMEGA_T_MAGIC_BYTE3 0x31

/* T32: one ternary word holds 32 balanced trits.
 * Value range is symmetric: [-TW_MAX, +TW_MAX], TW_MAX = (3^32 - 1) / 2. */
#define TW_TRITS 32
#define TW_MAX 926510094425920LL
#define TW_POW3_32 1853020188851841LL

/* Ternary type tags and opcodes live in a range disjoint from the binary
 * TypeTag/OpCode enums so no binary identity can collide with them. */
#define TYPE_T_TRIT  0x40
#define TYPE_T_WORD  0x41

/* Symmetric saturation: results beyond +/-TW_MAX clamp to +/-TW_MAX.
 * Unlike two's-complement wrap, it commutes with negation. */
#define OVERFLOW_T_SATURATE_SYMMETRIC 0x10

typedef enum {
    TOP_INVALID = 0x00,
    TOP_TNEG    = 0x40, /* -a (digit flip) */
    TOP_TADD    = 0x41, /* a + b, symmetric saturation */
    TOP_TSUB    = 0x42, /* a - b, symmetric saturation */
    TOP_TMUL    = 0x43, /* a * b, symmetric saturation */
    TOP_TAND    = 0x44, /* trit-wise min (Kleene AND) */
    TOP_TOR     = 0x45, /* trit-wise max (Kleene OR) */
    TOP_TXOR    = 0x46, /* trit-wise -(a_i * b_i) (Kleene XOR) */
    TOP_TCMP    = 0x47, /* sign(a - b) in {-1, 0, +1} */
    TOP_TSIGN   = 0x48, /* sign(a) in {-1, 0, +1} */
    TOP_TSHL    = 0x49, /* a * 3^k, symmetric saturation (k = operand b) */
    TOP_TSHR    = 0x4A  /* drop k low trits = round-to-nearest a / 3^k */
} TernaryOp;

#define TOP_COUNT 11

/* Brute-force digit model: little-endian balanced trits. */
#define TV_MAX_DIGITS 72
typedef struct {
    uint8_t n;
    int8_t d[TV_MAX_DIGITS];
} TritVec;

const char *omega_t_op_name(TernaryOp op);
bool omega_t_op_is_unary(TernaryOp op);
bool omega_t_word_valid(int64_t v);
int64_t omega_t_saturate(__int128 v, bool *overflowed);

/* Brute-force trit model: every op evaluated digit by digit. */
void omega_tv_from_int(int64_t v, uint8_t n, TritVec *out);
__int128 omega_tv_to_int(const TritVec *tv);
int omega_t_eval_brute(TernaryOp op, int64_t a, int64_t b, int64_t *out, bool *overflowed);

/* Fast model on the integer value (the reference the synthesizer probes with). */
int omega_t_eval(TernaryOp op, int64_t a, int64_t b, int64_t *out, bool *overflowed);

/* Packed planes: bits 0..31 = positive trits, bits 32..63 = negative trits. */
uint64_t omega_t_to_planes(int64_t v);
int64_t omega_t_from_planes(uint64_t planes);
bool omega_t_planes_valid(uint64_t planes);

/* Number of significant trits of v (0 for v == 0). */
uint32_t omega_t_trit_len(int64_t v);

/* Cross-checks the fast model against the brute-force model:
 * exhaustive over all 6-trit operand pairs, plus boundary and pseudo-random
 * full-width operands. Returns number of mismatches (0 = pass). */
size_t omega_t_selfcheck_models(size_t *out_checks, char *err, size_t err_len);

/* ---- OMG1 canonical encoding ----
 * Same grammar as OMG0 (spec/canonical-encoding.md) with magic "OMG1".
 * Ternary values are canonically tryte-packed: 5 trits per byte,
 * byte = sum((t_i + 1) * 3^i), least significant trit first, bytes < 243. */
int omega_t_build_type_word(OmegaObject *obj);
int omega_t_build_value(OmegaObject *obj, const SemanticId *type_id, int64_t v);
int omega_t_build_op(OmegaObject *obj, TernaryOp op, const SemanticId *type_id);
int omega_t_value_decode(const OmegaObject *obj, int64_t *out_v);
int omega_t_canonical_encode(const OmegaObject *obj, uint8_t *out_buf, size_t max_len, size_t *out_len);
int omega_t_compute_semantic_id(OmegaObject *obj);

#endif /* OMEGA_TERNARY_H */
