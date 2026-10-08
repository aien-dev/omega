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
 * length register) and unused registers are 0. Nothing is signed or packed here.
 * The three files are valid only together and only when oscc exits 0: any earlier OUT.* is removed first,
 * each file is written under OUT.*.tmp and renamed at the end, and on any failure none of them is left.
 * A parameter kind the container refuses (10, an array reference) is refused here, before anything is written. */
static const char *const k_ext[3] = {".ir", ".code", ".entries"};

static int out_path(char *buf, size_t n, const char *pre, const char *ext, const char *tmp)
{
    return (size_t)snprintf(buf, n, "%s%s%s", pre, ext, tmp) >= n ? -1 : 0;
}

static void remove_outputs(const char *pre)
{
    char p[4096];
    for (int i = 0; i < 3; i++) {
        if (out_path(p, sizeof p, pre, k_ext[i], "") == 0) remove(p);
        if (out_path(p, sizeof p, pre, k_ext[i], ".tmp") == 0) remove(p);
    }
}

static int write_tmp(const char *pre, int i, const void *p, size_t n, const char *text)
{
    char path[4096];
    if (out_path(path, sizeof path, pre, k_ext[i], ".tmp")) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = text ? fputs(text, f) == EOF : fwrite(p, 1, n, f) != n;
    return (fclose(f) != 0 || bad) ? -1 : 0;
}

/* 1..9 scalars, 11 bytes, 12 cells (OSC_UNIT_ARTIFACT.md section 6.1); 0 and 10 are refused. */
static int kind_ok(unsigned k) { return (k >= 1 && k <= 9) || k == 11 || k == 12; }

/* 0 written; 1 refused (message in err); -1 cannot write. */
static int emit_files(const char *pre, const OscUnit *u, const OscCode *code, char *err, size_t errn)
{
    static char ent[OSC_MAX_FUNCS * 128];
    size_t at = 0;
    for (unsigned fi = 0; fi < u->nfuncs; fi++) {
        const OscFunc *fn = &u->funcs[fi];
        for (unsigned r = 0; r < fn->nparams; r++)
            if (!kind_ok((unsigned)fn->vtype[r].s)) {
                snprintf(err, errn, "function %s parameter register %u has kind %u, which a unit container refuses",
                         fn->name, r, (unsigned)fn->vtype[r].s);
                return 1;
            }
        int w = snprintf(ent + at, sizeof ent - at, "%u %u %u", fi, (unsigned)fn->nparams, (unsigned)fn->ret.s);
        for (unsigned r = 0; r < 6 && w > 0 && (size_t)w < sizeof ent - at; r++)
            w += snprintf(ent + at + w, sizeof ent - at - w, " %u", r < fn->nparams ? (unsigned)fn->vtype[r].s : 0u);
        if (w > 0 && (size_t)w < sizeof ent - at)
            w += snprintf(ent + at + w, sizeof ent - at - w, " %u %s\n", (unsigned)code->entry[fi], fn->name);
        if (w <= 0 || (size_t)w >= sizeof ent - at) return -1;
        at += (size_t)w;
    }
    size_t n = 0;
    if (osc_ir_encode(u, NULL, 0, &n)) return -1;
    uint8_t *ir = malloc(n ? n : 1);
    if (!ir) return -1;
    int rc = osc_ir_encode(u, ir, n, &n) || write_tmp(pre, 0, ir, n, NULL) ||
             write_tmp(pre, 1, code->code, code->len, NULL) || write_tmp(pre, 2, NULL, 0, ent);
    free(ir);
    char from[4096], to[4096];
    for (int i = 0; !rc && i < 3; i++)
        rc = out_path(from, sizeof from, pre, k_ext[i], ".tmp") || out_path(to, sizeof to, pre, k_ext[i], "") ||
             rename(from, to) != 0;
    return rc ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: oscc <file.osc> [OUT]\n");
        return 2;
    }
    if (argc == 3) remove_outputs(argv[2]); /* a stale triple from an earlier source never survives a run */
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
    int erc = argc == 3 ? emit_files(argv[2], u, &code, err, sizeof err) : 0;
    if (erc) {
        remove_outputs(argv[2]);
        if (erc > 0) printf("error kind=unit_container message=%s\n", err);
        else fprintf(stderr, "oscc: cannot write %s.{ir,code,entries}\n", argv[2]);
        osc_cg_free(&code);
        free(u);
        free(src);
        return erc > 0 ? 1 : 2;
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
