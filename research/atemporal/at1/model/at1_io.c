/* The only file-system access of the engine: read a whole file, up to AT1_FILE_MAX bytes. */
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
