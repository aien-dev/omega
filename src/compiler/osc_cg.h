/*
 * osc_cg.h -- OSC-1 back end: typed IR -> AArch64 machine code through the
 * hardened in-repo encoder (src/compiler/osc_a64.*). ABI and frame layout:
 * docs/osc/OSC-1-DESIGN.md section 7. Output is a pure function of the IR.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_CG_H
#define OSC_CG_H

#include <stddef.h>
#include <stdint.h>
#include "osc_alloc.h"
#include "osc_ir.h"

typedef struct {
    uint8_t *code;                   /* whole unit, little-endian 32-bit words */
    size_t len;                      /* bytes (multiple of 4) */
    uint32_t entry[OSC_MAX_FUNCS];   /* byte offset of each function's entry */
    uint16_t nfuncs;
    const OscAlloc *a;               /* allocator that owns .code (set by compile; NULL = libc, so a zeroed OscCode frees) */
} OscCode;

/* Compile a unit. Refuses (-1, message in err) a unit that fails
 * osc_ir_validate, any encoder refusal, an allocator refusal ("out of memory"),
 * or a failed self-check (every emitted word must decode through osc_a64_decode
 * and re-encode to itself).
 * On success out->code is allocated from the allocator; release with osc_cg_free.
 * osc_cg_compile_a takes all its memory (word and fixup buffers, the validator's
 * scratch, the code) from `a` (NULL = libc) and frees with out->a, so `a` must
 * outlive the OscCode; osc_cg_compile is the same over libc. The bytes emitted
 * never depend on the allocator. */
int osc_cg_compile_a(const OscAlloc *a, const OscUnit *u, OscCode *out, char *err, size_t n);
int osc_cg_compile(const OscUnit *u, OscCode *out, char *err, size_t n);
void osc_cg_free(OscCode *c);

/* Self-check over an emitted buffer: 0 if every word decodes and re-encodes
 * to itself, else -1 with the first failing word offset in err. */
int osc_cg_verify(const OscCode *c, char *err, size_t n);

#endif /* OSC_CG_H */
