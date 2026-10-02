#ifndef OMEGA_PROGRAM_IR_H
#define OMEGA_PROGRAM_IR_H

/* omega_program_ir: the canonical IR of an Omega program, the bytes behind the Verified Crumb
 * field source_or_ir_digest when digest_kind is OMEGA_VC_DIGEST_IR (VC1 stage 6, SPEC 6 step 3).
 *
 * The program id (omega_program_compute_id) binds exactly: the body steps, the input and output
 * types and widths, and the precondition and postcondition ids. The IR carries exactly those
 * fields and nothing else (no name, cost or realization: they are metadata), so the id can be
 * RECOMPUTED from the IR bytes alone. That recomputation is what lets the resolver check that a
 * stored blob really is the program its semantic id names.
 *
 * Layout (all integers big-endian, no padding):
 *   "AIEN_OMEGA_PROGRAM_IR_V1"  24 bytes
 *   u8  input_type   u16 input_width   u8 output_type   u16 output_width
 *   32  precondition_id   32 postcondition_id
 *   u16 step_count (0..OMEGA_PROGRAM_MAX_STEPS)
 *   step_count x { u8 op, u64 imm }
 * Decoding is exact: wrong tag, truncation, trailing bytes, too many steps, or an op or type the
 * program id does not accept are refusals. */

#include <stddef.h>
#include <stdint.h>

#include "omega_program.h"

#define OMEGA_PROGRAM_IR_TAG "AIEN_OMEGA_PROGRAM_IR_V1"
#define OMEGA_PROGRAM_IR_TAG_LEN 24

/* Canonical IR of prog (malloc'd, caller frees). Refuses a program with no body. 0 or -1. */
int omega_program_ir_encode(const OmegaProgram *prog, uint8_t **out, size_t *len);

/* Decode the IR into *out (zeroed first, program_id recomputed with omega_program_compute_id).
 * 0 or -1; on -1 *out holds nothing to free. */
int omega_program_ir_decode(const uint8_t *bytes, size_t len, OmegaProgram *out);

/* Recompute the program id from the IR alone. 0 with id set, or -1. */
int omega_program_ir_recompute_id(const uint8_t *bytes, size_t len, uint8_t id[32]);

#endif /* OMEGA_PROGRAM_IR_H */
