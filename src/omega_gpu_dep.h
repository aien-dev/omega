#ifndef OMEGA_GPU_DEP_H
#define OMEGA_GPU_DEP_H
/* omega_gpu_dep: dependency flags between the launches of one command list (DEPFLAGS, 2026-10-16), the host-visible part.
 *
 * Mode (process-wide, omega_gpu_dep_mode): 0 = none (the WAIT_FOR_IDLE barrier orders the launches, the default),
 * 1 = flags (OMEGA_DEP_FLAGS), 2 = camera only, 3 = flags + camera. Setter, else env AIEN_OMEGA_DEP read once. The decode
 * graph passes it to its command list (omega_gpu_cmdlist_set_dep); every kernel API folds the list's value into its spec
 * (kernel cache key), so a direct launch (no list) always builds dep 0 kernels. Changing the mode after kernels were built
 * rebuilds them on the next call (different spec); the caller clears caches it pinned.
 *
 * Protocol (one shared counter per command list, omega_gpu_cmdlist.c writes the bank words, omega_bw_dep.h the kernel):
 *   counter line  64 B, GPU-uncached (omega_gpu_session_alloc): +0 u32 done (every CTA of every launch adds 1 before it
 *                 exits), +4 u32 camera slot allocator, +8 u64 camera base (0 = off). The host zeroes +0 and +4 before
 *                 every submit, so targets are plain CTA counts.
 *   bank words    OMEGA_DEP_ARG_WAIT u64 = line va, OMEGA_DEP_ARG_DONE u64 = line va (the same line; two words so a
 *                 per-launch-line layout stays possible), OMEGA_DEP_ARG_TARGET u32 = S_(i-1) = CTAs of launches 0..i-1.
 *   kernel        warp 0 polls until done - target < 2^31 (wrap-safe), BAR.SYNC, CCTL.IVALL; at exit MEMBAR.ALL.GPU,
 *                 BAR.SYNC, thread 0 adds 1. Why that orders the list: the first moment done reaches S_i, no CTA of a launch
 *                 j > i has passed its poll (it needs done >= S_(j-1) >= S_i), so all S_i adds came from launches 0..i, which
 *                 have therefore all finished. A launch marked independent waits for S_(i-2) instead; the launch after it
 *                 keeps S_i, which still counts both. A poll that never succeeds (OMEGA_DEP_POLL_LIMIT reads) adds
 *                 OMEGA_DEP_TIMEOUT_SENTINEL and continues, so nothing hangs the chip and the host sees the wrong count.
 *   host check    after the completion marker: done == sum of all CTAs, else an error naming the stage. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OMEGA_DEP_FLAGS 1u            /* kernels order themselves with the counter (barrier NONE in the list) */
#define OMEGA_DEP_CAMERA 2u           /* every CTA's thread 0 records {GLOBALTIMER at entry, at exit} */
#define OMEGA_DEP_TEST_NO_PROLOGUE 4u /* TEST ONLY: emit no wait (a kernel that reads too early; the sim test must catch it) */
#define OMEGA_DEP_TEST_NO_EPILOGUE 8u /* TEST ONLY: emit no add (a kernel that never reports; the host count must catch it) */
#define OMEGA_DEP_ARG_WAIT 0x3e8u     /* u64 va of the counter to wait on */
#define OMEGA_DEP_ARG_DONE 0x3f0u     /* u64 va of the counter this launch adds to */
#define OMEGA_DEP_ARG_TARGET 0x3f8u   /* u32 */
#define OMEGA_DEP_ARGS_FIRST 0x3e8u   /* kernel arguments must end here: at most (0x3e8 - 0x380) / 4 = 26 words in dep mode */
#define OMEGA_DEP_MAX_KERNEL_ARGS 26u
#define OMEGA_DEP_LINE_BYTES 64u
#define OMEGA_DEP_CAM_SLOTS 4096u     /* records per launch (16 B each): CTAs beyond that wrap onto the first slots */
#define OMEGA_DEP_POLL_LIMIT (1u << 18)
#define OMEGA_DEP_TIMEOUT_SENTINEL 0x12345678u

uint32_t omega_gpu_dep_mode(void);
void omega_gpu_dep_set_mode(uint32_t mode); /* masked to OMEGA_DEP_FLAGS | OMEGA_DEP_CAMERA */
#ifdef __cplusplus
}
#endif
#endif
