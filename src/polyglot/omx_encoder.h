/* POLYGLOT-0 lane B2: Omega's own AArch64 encoder emits the B1 kernels
 * (src/polyglot/asm/omx_sdot.S, omx_crumb.S) as instruction bytes.
 * spec/polyglot-0.md section 4 (toolchain candidate, not a language result).
 */
#ifndef OMX_ENCODER_H
#define OMX_ENCODER_H

#include <stddef.h>
#include <stdint.h>

enum { OMX_ENC_SDOT = 0, OMX_ENC_CRUMB = 1, OMX_ENC_KERNELS = 2 };

/* Which encoder produced an instruction word. */
enum { OMX_ENC_FROM_OMEGA = 0, /* src/aarch64_encoder.c (Omega's original encoder) */
       OMX_ENC_FROM_EXT = 1 }; /* src/polyglot/omx_encoder_ext.c (lane B2 additions) */

/* Data relocation carried by an instruction (like an object-file relocation). */
enum { OMX_ENC_RELOC_NONE = 0, OMX_ENC_RELOC_ADRP_PAGE = 1, OMX_ENC_RELOC_ADD_LO12 = 2 };

typedef struct {
    uint32_t word;
    uint8_t origin; /* OMX_ENC_FROM_* */
    uint8_t reloc;  /* OMX_ENC_RELOC_* */
    int32_t branch_words; /* resolved PC-relative branch distance in instructions, 0 if not a branch */
    char text[56];  /* assembly text of what was emitted */
} omx_enc_insn;

/* Build kernel `k` into `out` (capacity `cap`), count in *n_insn.
 * base == 0: object form, data relocations left as zero fields (what GNU as
 *            writes into an unlinked .o).
 * base != 0: final form for code placed at address `base`; data (the sdot
 *            tail mask) sits at base + *data_off (16-byte aligned, after the
 *            code); ADRP/ADD fields are resolved against it.
 * Returns 0, or -1 if any encoder rejected an operand or a branch/label was
 * out of range or unresolved (fail closed). */
int omx_encoder_build(int k, uint64_t base, omx_enc_insn *out, size_t cap, size_t *n_insn, size_t *data_off,
                      const uint8_t **data, size_t *data_len);

/* The executable kernel for `k` (mapped RW, written, then mprotect RX; never
 * W and X together). NULL on failure. Built once, thread-safe. The returned
 * pointer is the entry of int fn(const oma_rz_plan *, const int8_t *, int32_t *). */
const void *omx_encoder_code(int k, size_t *code_bytes);

#endif /* OMX_ENCODER_H */
