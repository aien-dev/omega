/*
 * oscc_main.c -- `oscc <file.osc>`: compile an OSC-1 source file to IR and
 * AArch64 machine code and print their identities:
 *   ir_sha256=<hex>  code_sha256=<hex>  funcs=<n>
 * On a refusal print the diagnostic fields and exit 1.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "sha256.h"

static void hex(const uint8_t *d, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: oscc <file.osc>\n");
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "oscc: cannot open %s\n", argv[1]); return 2; }
    size_t cap = 1 << 16, len = 0;
    char *src = malloc(cap);
    for (;;) {
        if (!src) { fclose(fp); fprintf(stderr, "oscc: out of memory\n"); return 2; }
        size_t r = fread(src + len, 1, cap - len, fp);
        len += r;
        if (len < cap) break;
        if (cap >= (1u << 24)) { fclose(fp); free(src); fprintf(stderr, "oscc: file too large\n"); return 2; }
        cap *= 2;
        char *n = realloc(src, cap);
        if (!n) free(src);
        src = n;
    }
    fclose(fp);
    OscUnit *u = malloc(sizeof *u);
    OscDiag d;
    if (!u) { free(src); fprintf(stderr, "oscc: out of memory\n"); return 2; }
    if (osc_compile(src, len, u, &d, NULL)) {
        printf("error kind=%s line=%u col=%u object=%s origin_line=%u other=%s transition=%s\n",
               osc_diag_kind_name(d.kind), d.line, d.col, d.object, d.origin_line, d.other, d.transition);
        printf("message=%s\n", d.message);
        free(u);
        free(src);
        return 1;
    }
    uint8_t dig[32];
    char hx[65], err[160];
    OscCode code;
    memset(&code, 0, sizeof code);
    if (osc_ir_digest(u, dig) || osc_cg_compile(u, &code, err, sizeof err)) {
        printf("error kind=backend message=%s\n", err);
        free(u);
        free(src);
        return 1;
    }
    hex(dig, hx);
    printf("ir_sha256=%s\n", hx);
    sha256_hash(code.code, code.len, dig);
    hex(dig, hx);
    printf("code_sha256=%s\n", hx);
    printf("funcs=%u\n", (unsigned)u->nfuncs);
    osc_cg_free(&code);
    free(u);
    free(src);
    return 0;
}
