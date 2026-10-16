#ifndef OMEGA_GPU_TGEMM_API_H
#define OMEGA_GPU_TGEMM_API_H
/*
 * omega_gpu_tgemm_api: the trainer's device-resident bf16 GEMM (lane E1, 2026-10-16). New file.
 * C[m][n] f32 = A[m][k] bf16 x B[k][n] bf16, all three row-major and already resident on the GPU (no host pack per call).
 * Kernels: omega_blackwell_codegen_tgemm.h, cached through the prefill module's resident-kernel cache (omega_gpu_prefill.h,
 * kind OMEGA_TGEMM_KIND). Return codes are the prefill module's (OMEGA_GPU_PF_OK, _BAD_ARGS, _TOO_LARGE, _CHIP_FAIL).
 * Memory rule (trainer plan R-8): buffers from omega_gpu_pf_alloc_dev are GPU-cached and must never be read or written by the
 * CPU; the tile table is a host (uncached) buffer the CPU fills and the session syncs.
 * Limits: m a multiple of the tile height (omega_gpu_tgemm_bm), n a multiple of the span (warps * 16), k a multiple of 128,
 * m / bm <= 65535 tiles.
 */
#include <stdint.h>
#include "omega_gpu_session.h"
#include "omega_blackwell_codegen_tgemm.h"

#ifdef __cplusplus
extern "C" {
#endif
#define OMEGA_TGEMM_KIND 0x60u
typedef struct {
    uint32_t layout;  /* OMEGA_TGEMM_LAYOUT_* */
    uint32_t epi;     /* OMEGA_TGEMM_EPI_* */
    uint32_t m, n, k;
    uint64_t a_va, b_va, c_va;
} OmegaTgemmCall;
typedef struct { uint64_t c_va; uint32_t row0, kb0, nkb, pad; uint64_t pad2; } OmegaTgemmTile;

/* the kernel spec the current knobs pick for a call (BAD_ARGS when the call cannot be served) */
int omega_gpu_tgemm_spec(const OmegaTgemmCall *c, OmegaTgemmSpec *out);
uint32_t omega_gpu_tgemm_bm(void); /* rows per tile under the current knobs */
/* tile table for a call: one entry per bm rows; returns the count (0: refused or more than max) */
uint32_t omega_gpu_tgemm_tiles(const OmegaTgemmCall *c, OmegaTgemmTile *tab, uint32_t max);
/* synchronous: builds the table in a module-owned host buffer, launches, waits; chip ns added to *ns when given */
int omega_gpu_tgemm(const OmegaTgemmCall *c, uint64_t *ns);
/* non-blocking: lock held and device open (omega_gpu_pf_open_locked); fills *L for omega_gpu_cmdlist_add with the resident
 * kernel, the grid and args[OMEGA_TGEMM_ARGS_WORDS] (the caller's array; L->args points at it, so it must outlive the add).
 * tiles_va: a GPU-visible table of ntiles entries already synced. */
int omega_gpu_tgemm_fill_launch_locked(const OmegaTgemmCall *c, uint64_t tiles_va, uint32_t ntiles, OmegaGpuLaunch *L, uint32_t *args);
/* knobs: rt (2/4/8), warps (8/16), aqn (1/2); 0 keeps the default (rt 4, warps 16, aqn 1; warps 8 when n % 256 != 0) */
void omega_gpu_tgemm_set_shape(uint32_t rt, uint32_t warps, uint32_t aqn);
void omega_gpu_tgemm_test_set_mutant(int m);
#ifdef __cplusplus
}
#endif
#endif
