/* cx_config_freestanding.h -- allocation hooks for the CX_NO_POSIX build of
 * rx_cortex.c (tests/runtime/rx_cortex_journal.c supplies them). A real
 * freestanding host (AIENOS) names its own header through CX_CONFIG_H. */
#ifndef CX_CONFIG_FREESTANDING_H
#define CX_CONFIG_FREESTANDING_H
#include <stddef.h>
void *cx_fs_alloc(size_t n);
void *cx_fs_realloc(void *p, size_t n);
void cx_fs_free(void *p);
#define CX_MALLOC(n) cx_fs_alloc(n)
#define CX_REALLOC(p, n) cx_fs_realloc((p), (n))
#define CX_FREE(p) cx_fs_free(p)
#endif
