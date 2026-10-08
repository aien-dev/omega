/*
 * oscc_main.c -- `oscc <file.osc>`: compile an OSC-1 source file to IR and
 * AArch64 machine code and print their identities:
 *   ir_sha256=<hex>  code_sha256=<hex>  funcs=<n>
 * With OUT, also write OUT.ir, OUT.code and OUT.entries (see emit_files).
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

/* `oscc <file.osc> OUT`: also write the compiler's own output to files, for packaging into a signed
 * unit container (aien-protocols specs/osc-unit-artifact). OUT.ir = canonical IR bytes (osc_ir_encode),
 * OUT.code = the AArch64 bytes, OUT.entries = one line per function in index order:
 *   fn_index nregs ret_kind k0 k1 k2 k3 k4 k5 code_offset name
 * where the kinds are the OscScalar values of the argument registers (a slice is its kind then 5 for the
 * length register) and unused registers are 0. Nothing is signed or packed here. */
static int write_file(const char *pre, const char *ext, const void *p, size_t n)
{
    char path[4096];
    if ((size_t)snprintf(path, sizeof path, "%s%s", pre, ext) >= sizeof path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fwrite(p, 1, n, f) != n;
    return (fclose(f) != 0 || bad) ? -1 : 0;
}

static int emit_files(const char *pre, const OscUnit *u, const OscCode *code)
{
    size_t n = 0;
    if (osc_ir_encode(u, NULL, 0, &n)) return -1;
    uint8_t *ir = malloc(n ? n : 1);
    if (!ir) return -1;
    int rc = osc_ir_encode(u, ir, n, &n) || write_file(pre, ".ir", ir, n) || write_file(pre, ".code", code->code, code->len);
    free(ir);
    if (rc) return -1;
    char path[4096];
    if ((size_t)snprintf(path, sizeof path, "%s.entries", pre) >= sizeof path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (unsigned fi = 0; fi < u->nfuncs; fi++) {
        const OscFunc *fn = &u->funcs[fi];
        fprintf(f, "%u %u %u", fi, (unsigned)fn->nparams, (unsigned)fn->ret.s);
        for (unsigned r = 0; r < 6; r++) fprintf(f, " %u", r < fn->nparams ? (unsigned)fn->vtype[r].s : 0u);
        fprintf(f, " %u %s\n", (unsigned)code->entry[fi], fn->name);
    }
    return fclose(f) != 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: oscc <file.osc> [OUT]\n");
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
    if (argc == 3 && emit_files(argv[2], u, &code)) {
        fprintf(stderr, "oscc: cannot write %s.{ir,code,entries}\n", argv[2]);
        osc_cg_free(&code);
        free(u);
        free(src);
        return 2;
    }
    osc_cg_free(&code);
    free(u);
    free(src);
    return 0;
}
