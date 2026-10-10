/* The only stdio of the engine outside at1_main.c: read a whole file (up to
 * AT1_FILE_MAX bytes) and the fatal error exit. The compute objects stay free
 * of stdio so the isolation gate scans them strictly (AT-1 D2). */
#include "at1_model.h"

#include <stdio.h>
#include <stdlib.h>

at1_status at1_read_file(const char *path, uint8_t **bytes, size_t *len)
{
    *bytes = NULL; *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return AT1_ERR_ARGUMENT;
    size_t cap = 65536, n = 0;
    uint8_t *b = at1_xmalloc(cap);
    for (;;) {
        if (n == cap) {
            if (cap > AT1_FILE_MAX) break;
            cap *= 2;
            b = at1_xrealloc(b, cap);
        }
        size_t got = fread(b + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(b); return AT1_ERR_ARGUMENT; }
    if (n > AT1_FILE_MAX) { free(b); return AT1_ERR_RESOURCE; }
    *bytes = b; *len = n;
    return AT1_OK;
}

/* Fatal engine error: one stderr line, exit 1. Lives here, not in a compute
 * object: gcc lowers fputs(constant, stream) to fwrite, which the isolation
 * gate bans (omega#371 D2); fprintf is not lowered and at1_io.o is the one
 * object the gate exempts for file I/O anyway. */
void at1_fatal(const char *reason)
{
    fprintf(stderr, "AT1_ENGINE_ERROR %s\n", reason);
    exit(1);
}
