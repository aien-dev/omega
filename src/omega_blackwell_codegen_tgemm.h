#ifndef OMEGA_BLACKWELL_CODEGEN_TGEMM_H
#define OMEGA_BLACKWELL_CODEGEN_TGEMM_H
/*
 * omega_blackwell_codegen_tgemm: device-resident bf16 training GEMM on the tensor cores (trainer lane E1, 2026-10-16).
 * New file. C[M][N] (f32, row-major) = A[M][K] (bf16, row-major) x B[K][N] (bf16, row-major): the NN layout of the
 * trainer's forward Linear (Y = X W^T with W stored transposed), no per-call host pack. Fragment layout, emitter rules,
 * scheduling (ldw_pf scoreboards 3/4, hmma stall 8, nop 15 + nop 14 before the first accumulator read) and the double-
 * buffered shared staging are the chip-proven ones of omega_blackwell_codegen_tc2 (OWN30B-L1G-GEMM-1e76e728).
 *
 * K is walked in 128-wide blocks of four 32-wide chunks. Inside a 32-wide chunk lane t (lane % 4) of every HMMA owns
 * k = 8t + {0..7}: HMMA step s (0, 1) of the chunk puts k = 8t + 4s + {0, 1} at fragment positions (2t, 2t + 1) and
 * k = 8t + 4s + {2, 3} at (2t + 8, 2t + 9). The 16-term HMMA sum is order independent (tests/bw_hmma_model.h), so any
 * consistent lane -> k assignment is exact; this one makes every A row word and every B row word contiguous in memory.
 *  - A tile (16 rt rows x 32 k): LDG.E.64 pieces (4 bf16 of one row), STS32 x 2 into the lane's fragment slot of the
 *    staging buffer (per row tile i and step s: 32 lanes x 16 bytes at (i * 2 + s) * 512), BAR.SYNC, LDS.128 per row tile.
 *    Two buffers of 16 rt x 32 x 2 bytes: 2048 rt bytes of shared (rt 8: 16 KB, the QMD cap).
 *  - B (K x N, N contiguous): each lane loads its B fragment rows straight from global: per step four LDG.E.64 at rows
 *    k = 8t + 4s + j, columns n0w + 4 (g / 2) .. + 3 (the lane needs the half word (g % 2) of them, selected with one LOP3
 *    per row word; lanes g, g ^ 1 touch the same 8 bytes, so no extra L2 sectors), then IMAD / IMAD.HI.U32 pack the two
 *    column tiles: tile E = the warp's even physical columns (n0w + 2 g), tile O = the odd ones (n0w + 2 g + 1).
 *    Two register sets (scoreboards 3 and 4) alternate per chunk so the next chunk's B is in flight during the HMMAs.
 *  - Epilogue F32: the tile leaves through shared memory in 8-row passes (lane (g, t) parks its words E.c, O.c of row g at the
 *    pass columns n0w + 4 t .. + 3, BAR.SYNC, then thread tid reads words tid + T j and stores them: every warp STG writes 128
 *    contiguous bytes). Storing straight from the fragment layout with 4-byte STG touched 16 half-filled sectors per
 *    instruction and cost more than half of the kernel time at K 768 (chip probe, no-store mutant 5). Store immediates stay
 *    under the encoder's 2^23 for N up to 2^17 (50304: the lm_head gradient).
 *
 * Tile table entry (32 bytes, ctaid.y): { c_va u64 (output base for this tile's K part), row0 u32, kb0 u32 (first 128-block),
 * nkb u32 (blocks, >= 1), pad u32, pad u64 }. Constant bank 0 (cbank + 0x380): tiles u64, a u64, b u64.
 * Grid: grid_x = n / span (span = warps * 8 * ct), grid_y = tiles; CTA 32 * warps threads; rows of a tile are always
 * 16 rt (the caller pads M to a multiple of the tile height).
 * mutant (tests only): 1 drops HMMA step 1 of the last chunk of every block, 2 never advances the A pointer to the next
 * block, 3 row tile i reads tile i ^ 1, 4 swaps the column parity (tile E takes the odd columns), 5 stores nothing (timing probe).
 */
#include <stdint.h>
#include "omega_blackwell_codegen.h"

#ifdef __cplusplus
extern "C" {
#endif
enum { OMEGA_TGEMM_LAYOUT_NN = 0 };
enum { OMEGA_TGEMM_EPI_F32 = 0 };
typedef struct {
    uint32_t k;      /* multiple of 128, 128 .. 65536 (the per-tile K range is kb0 .. kb0 + nkb blocks of 128) */
    uint32_t n;      /* output width, a multiple of span, < 2^17 */
    uint32_t rt;     /* row tiles of 16 per CTA: 2, 4 or 8 */
    uint32_t ct;     /* column tiles of 8 per warp: 2 */
    uint32_t warps;  /* 8 or 16 */
    uint32_t kc;     /* k per chunk: 32 */
    uint32_t aqn;    /* A fragments in flight: 1 or 2 */
    uint32_t layout; /* OMEGA_TGEMM_LAYOUT_* */
    uint32_t epi;    /* OMEGA_TGEMM_EPI_* */
    int mutant;
} OmegaTgemmSpec;
#define OMEGA_TGEMM_ARGS_WORDS 6u
#define OMEGA_TGEMM_TILE_BYTES 32u
#define OMEGA_TGEMM_MAX_INSNS 1000u
int omega_tgemm_spec_ok(const OmegaTgemmSpec *s);
uint32_t omega_tgemm_shared_bytes(const OmegaTgemmSpec *s);
uint32_t omega_tgemm_span(const OmegaTgemmSpec *s);
uint32_t omega_tgemm_bm(const OmegaTgemmSpec *s); /* rows per tile */
/* -1: refused (spec, emitter, allocator, or more than OMEGA_TGEMM_MAX_INSNS instructions / 128 declared registers) */
int omega_blackwell_codegen_tgemm_prog(const OmegaTgemmSpec *s, BlackwellIRProgram *prog);
#ifdef __cplusplus
}
#endif
#endif
