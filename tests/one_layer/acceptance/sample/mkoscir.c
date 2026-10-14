/*
 * mkoscir.c -- writes the OSC binary IR bytes (spec/one-layer-format.md) of a PUBLIC SAMPLE OSC text
 * program, for the v2 selftest (HARNESS_INTERFACE_V2.md kind oscir). Not the sealed corpus.
 *   mkoscir <in.osc> <out.oscir>
 * osc_compile, then osc_ir_encode: the canonical bytes osc_ir_decode admits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_front.h"
#include "osc_ir.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: mkoscir in.osc out.oscir\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static char src[1 << 20];
    size_t n = fread(src, 1, sizeof src, f);
    fclose(f);
    OscUnit *u = calloc(1, sizeof *u);
    static uint8_t buf[OSC_IR_ENCODED_MAX];
    size_t len = 0;
    OscDiag d;
    memset(&d, 0, sizeof d);
    if (!u || osc_compile(src, n, u, &d, NULL) || osc_ir_encode(u, buf, sizeof buf, &len)) { fprintf(stderr, "mkoscir: refused\n"); return 1; }
    if (!(f = fopen(argv[2], "wb")) || fwrite(buf, 1, len, f) != len || fclose(f)) return 1;
    free(u);
    return 0;
}
