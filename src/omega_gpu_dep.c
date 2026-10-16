/* omega_gpu_dep.c: see the header. */
#include "omega_gpu_dep.h"
#include <stdlib.h>
static int g_set; static uint32_t g_mode;
uint32_t omega_gpu_dep_mode(void) {
    if (!g_set) { const char *e = getenv("AIEN_OMEGA_DEP"); g_mode = e ? (uint32_t)atoi(e) & 3u : 0u; g_set = 1; }
    return g_mode;
}
void omega_gpu_dep_set_mode(uint32_t mode) { g_mode = mode & 3u; g_set = 1; }
