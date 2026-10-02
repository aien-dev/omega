#ifndef OMEGA_NUMERIC_LDST_GB10_H
#define OMEGA_NUMERIC_LDST_GB10_H
/*
 * E1 row 2: general global load/store on the GB10 (docs/numeric/E1_LDST_GB10.md).
 *
 * Contract. One thread per element i < count copies one element from the input
 * to the output through the register file:
 *     reg  = LOAD_ld ( in  + i * in_stride  + in_off  )
 *     STORE_st( out + i * out_stride + out_off, reg )
 * ld is U8, S8, U16, S16 (zero or sign extended to 32 bits), B32, B64 or B128.
 * st is B8, B16, B32, B64 or B128 (the low st bytes of the register file,
 * little endian). Strides are byte counts applied with a 64-bit multiply-add,
 * offsets are signed 24-bit byte offsets in the instruction.
 *
 * Rules (omega_ldst_check_spec): a store may not be wider than what the load
 * filled (4 bytes for the narrow loads, else the load width); stride and
 * offset of each side are multiples of that side's width and the stride is at
 * least the width (so output elements never overlap); stride < 2^24; offset in
 * [-2^23, 2^23); count <= OMEGA_DS_MAX_BATCH. Buffer bases are 16-byte aligned.
 *
 * Encodings (every field below was read back with nvdisasm 13.0.85 -b SM121 as
 * an offline decoder, see tools/ldst_nvdisasm_check.sh):
 *   LDG: w0 = 0x7981 | Rd<<16 | Ra<<24; w1 = UR | off24<<8;
 *        w2 = 0x0c1e1100 | size<<9;      size 0 U8, 1 S8, 2 U16, 3 S16, 4 32, 5 64, 6 128
 *   STG: w0 = 0x7986 | Ra<<24;           w1 = Rdata | off24<<8;
 *        w2 = 0x0c100004 | 0x1100 | size<<9 (UR4); size 0 8, 2 16, 4 32, 5 64, 6 128
 *   IMAD.WIDE.U32 imm: w1 = imm (the stride in bytes).
 * The kernel keeps the 17 verified prologue words of the vecadd kernel
 * (src/omega_blackwell_encoder.c); only the words above are new.
 */
#include <stddef.h>
#include <stdint.h>

typedef enum { OMEGA_LD_U8 = 0, OMEGA_LD_S8, OMEGA_LD_U16, OMEGA_LD_S16, OMEGA_LD_B32, OMEGA_LD_B64, OMEGA_LD_B128, OMEGA_LD_COUNT } OmegaLdKind;
typedef enum { OMEGA_ST_B8 = 0, OMEGA_ST_B16, OMEGA_ST_B32, OMEGA_ST_B64, OMEGA_ST_B128, OMEGA_ST_COUNT } OmegaStKind;

typedef struct {
    uint8_t ld, st;
    uint32_t in_stride, out_stride;   /* bytes */
    int32_t in_off, out_off;          /* bytes, signed 24-bit */
} OmegaLdstSpec;

#define OMEGA_LDST_CODE_BYTES 384u    /* 24 instructions */
#define OMEGA_LDST_PAD        0x10000u /* bytes in front of element 0 the executor allocates, so negative offsets stay in the buffer */

size_t omega_ld_bytes(OmegaLdKind k);
size_t omega_st_bytes(OmegaStKind k);
const char *omega_ldst_describe(const OmegaLdstSpec *s, char *buf, size_t len);

/* 0 ok, -1 refused with the reason in why. */
int omega_ldst_check_spec(const OmegaLdstSpec *s, size_t count, char *why, size_t why_len);

/* Lowest and one-past-highest byte (relative to the base) each side touches over count elements. */
int omega_ldst_extent(const OmegaLdstSpec *s, size_t count, int64_t *in_lo, int64_t *in_hi, int64_t *out_lo, int64_t *out_hi);

/* The kernel image, its expected nvdisasm listing (one "addr text ;" line per instruction), a
 * structural check (rebuilds the kernel from the fields it decodes and compares every byte). */
int omega_ldst_build_kernel(const OmegaLdstSpec *s, uint8_t *code, size_t max, size_t *out_len);
int omega_ldst_listing(const OmegaLdstSpec *s, char *buf, size_t len);
int omega_ldst_check_kernel(const OmegaLdstSpec *s, const uint8_t *code, size_t len, char *why, size_t why_len);

/* Host model. in/out point at element-0 base addresses; in_len/out_len are the bytes available
 * from lo_pad bytes before the base to the end. Returns 0, or -1 when any access leaves
 * [-pad, len-pad) or the spec is refused. */
int omega_ldst_host_run(const OmegaLdstSpec *s, const uint8_t *in_buf, size_t in_len, uint8_t *out_buf, size_t out_len,
                        size_t pad, size_t count);

/* GB10: same contract on the chip. in_buf/out_buf are whole buffers with pad bytes before element 0;
 * out_buf is copied in first (so the caller's fill pattern is what unwritten bytes read back as)
 * and copied back after. OMEGA_NUMERIC_* code; OMEGA_NUMERIC_ERR_DEVICE in a CPU-only build. */
int omega_ldst_gb10_run(const OmegaLdstSpec *s, const uint8_t *in_buf, size_t in_len, uint8_t *out_buf, size_t out_len,
                        size_t pad, size_t count);

#endif
