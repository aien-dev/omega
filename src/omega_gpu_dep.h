#ifndef OMEGA_GPU_DEP_H
#define OMEGA_GPU_DEP_H
/* omega_gpu_dep: the process-wide dependency-flag mode for the decode kernels (DEPFLAGS, 2026-10-16).
 * 0 = none (the WAIT_FOR_IDLE barrier orders the launches, the default), 1 = flags (OMEGA_DEP_FLAGS), 2 = camera only,
 * 3 = flags + camera. Setter, else env AIEN_OMEGA_DEP read once. Every kernel API folds it into its spec (kernel cache key),
 * the decode graph passes it to the command list (opts.dep). Changing it after kernels were built rebuilds them on the
 * next call (different spec); the caller clears caches it pinned. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t omega_gpu_dep_mode(void);
void omega_gpu_dep_set_mode(uint32_t mode);
#ifdef __cplusplus
}
#endif
#endif
