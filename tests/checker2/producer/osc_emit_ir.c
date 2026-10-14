/* osc_emit_ir: PRODUCER-side helper for the checker2 tests (omega#446). Not part of checker2.
 *
 * `oscc <file> OUT` writes IR bytes only when the unit also fits an aienos unit container, which
 * refuses reference parameters; this helper writes the canonical IR bytes of every unit the
 * producer front end accepts, through the producer's public API only (osc_compile, osc_ir_encode
 * from src/compiler/osc_front.h and osc_ir.h). It is linked with the producer library, never with
 * checker2. The test compares its digest line with `oscc <file>` ir_sha256 so both are the same bytes.
 *
 * Usage: osc_emit_ir <file.osc> <out.ir>   exit 0 written, 1 producer rejected, 2 usage / I/O. */
#include <stdio.h>
#include <stdlib.h>

#include "osc_front.h"
#include "sha256.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: osc_emit_ir <file.osc> <out.ir>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static char src[1 << 20];
    size_t n = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[n] = 0;
    OscUnit *u = calloc(1, sizeof *u);
    OscDiag d;
    if (!u) return 2;
    if (osc_compile(src, n, u, &d, NULL) != 0) { printf("producer=REJECT\n"); free(u); return 1; }
    size_t len = 0;
    if (osc_ir_encode(u, NULL, 0, &len) != 0 && len == 0) { free(u); return 2; }
    uint8_t *buf = malloc(len);
    if (!buf || osc_ir_encode(u, buf, len, &len) != 0) { free(u); free(buf); return 2; }
    uint8_t h[32];
    sha256_hash(buf, len, h);
    printf("producer=ACCEPT ir_sha256=");
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
    printf("\n");
    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(buf, 1, len, o) != len || fclose(o) != 0) { perror(argv[2]); return 2; }
    free(buf);
    free(u);
    return 0;
}
