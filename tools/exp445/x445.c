/*
 * x445.c -- C helpers of the exp445 generation driver (omega#445, docs/one_layer/EXP445_PROTOCOL.md).
 * No hidden data is read here: only the public requirement, the model's answer and the repo's own
 * compiler / decoder. The same checks run in every arm; only the representation's own reader differs.
 *
 *   x445 extract <content> <out>          payload of a model answer: the first fenced block (``` lines),
 *                                         or the whole text if there is none. Prints fenced|unfenced|open.
 *   x445 hex <in.hex> <out.oscir>         strict hex: pairs of [0-9a-fA-F], whitespace only between bytes.
 *        exit 0 "OK bytes=N"; exit 1 "REFUSED hex: ..." (nothing written).
 *   x445 tohex <in>                       lowercase hex, 32 bytes per line (the bytes-arm spelling)
 *   x445 check osc|oscir <file> <requirement>
 *        exit 0 "OK"; exit 1 one line "REFUSED <stage>: <reason>", stage compiler | decoder |
 *        signature | backend. Signature = the requirement's entry, params, ret.
 *   x445 wilson <k> <n>                   "p lo hi": Wilson score 95% interval (z = 1.959964)
 *   x445 mcnemar <b> <c>                  exact two-sided McNemar p on the discordant pairs (b, c)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_ir.h"

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    unsigned char *b = malloc(cap + 1);
    size_t r;
    while (b && (r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) {
            if (cap >= (64u << 20)) { free(b); b = NULL; break; }
            unsigned char *nb = realloc(b, (cap *= 2) + 1);
            if (!nb) { free(b); b = NULL; break; }
            b = nb;
        }
    }
    fclose(f);
    if (b) { b[len] = 0; *n = len; }
    return b;
}

static int put(const char *p, const void *b, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int bad = fwrite(b, 1, n, f) != n;
    return fclose(f) || bad ? -1 : 0;
}

static int cmd_extract(const char *in, const char *out)
{
    size_t n;
    unsigned char *t = slurp(in, &n);
    if (!t) { fprintf(stderr, "x445: cannot read %s\n", in); return 2; }
    const char *s = (const char *)t, *open = NULL;
    /* a fence is a line that starts with ``` (after optional spaces); its opening line may carry a tag */
    for (const char *l = s; *l; ) {
        const char *e = strchr(l, '\n'), *q = l;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, "```", 3)) { open = e ? e + 1 : l + strlen(l); break; }
        if (!e) break;
        l = e + 1;
    }
    if (!open) { if (put(out, t, n)) return 2; printf("unfenced\n"); free(t); return 0; }
    const char *end = NULL;
    for (const char *l = open; *l; ) {
        const char *e = strchr(l, '\n'), *q = l;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, "```", 3)) { end = l; break; }
        if (!e) break;
        l = e + 1;
    }
    size_t len = end ? (size_t)(end - open) : strlen(open);
    if (put(out, open, len)) return 2;
    printf("%s\n", end ? "fenced" : "open");
    free(t);
    return 0;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int cmd_hex(const char *in, const char *out)
{
    size_t n;
    unsigned char *t = slurp(in, &n);
    if (!t) { fprintf(stderr, "x445: cannot read %s\n", in); return 2; }
    unsigned char *b = malloc(n / 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; ) {
        if (ws(t[i])) { i++; continue; }
        int hi = hexv(t[i]);
        if (hi < 0) { printf("REFUSED hex: character 0x%02x at offset %zu is not a hex digit\n", t[i], i); return 1; }
        if (i + 1 >= n) { printf("REFUSED hex: odd number of hex digits (offset %zu)\n", i); return 1; }
        int lo = hexv(t[i + 1]);
        if (lo < 0) { printf("REFUSED hex: byte at offset %zu is not two hex digits\n", i); return 1; }
        b[k++] = (unsigned char)(hi << 4 | lo);
        i += 2;
    }
    if (!k) { printf("REFUSED hex: no bytes\n"); return 1; }
    if (put(out, b, k)) return 2;
    printf("OK bytes=%zu\n", k);
    free(t); free(b);
    return 0;
}

static int cmd_tohex(const char *in)
{
    size_t n;
    unsigned char *t = slurp(in, &n);
    if (!t) return 2;
    for (size_t i = 0; i < n; i++) printf("%02x%s", t[i], (i % 32 == 31 || i + 1 == n) ? "\n" : "");
    free(t);
    return 0;
}

/* section 7 of spec/one-layer-format.md, in words */
static const char *meaning(int rc)
{
    switch (rc) {
    case OSC_IR_REFUSE_MAGIC: return "the first 7 bytes are not the magic 4f 53 43 31 49 52 00";
    case OSC_IR_REFUSE_UNKNOWN_VERSION: return "the version byte is 0 or above 5";
    case OSC_IR_REFUSE_TRUNCATED: return "the input ends inside a field";
    case OSC_IR_REFUSE_OVERSIZED: return "a count or length is over the format's limits";
    case OSC_IR_REFUSE_TRAILING: return "bytes after the last function";
    case OSC_IR_REFUSE_RANGE: return "unknown opcode, type tag or ref kind, or a register/block/function field 0x8000..0xfffe";
    case OSC_IR_REFUSE_TEXT: return "a NUL byte inside a name or contract text";
    case OSC_IR_REFUSE_INVALID: return "well-formed, but the unit fails validation (types, registers, blocks, calls, definite assignment)";
    case OSC_IR_REFUSE_VERSION_MISMATCH: return "the version byte is not the one the content requires (1 for these programs)";
    case OSC_IR_REFUSE_NONCANONICAL: return "the unit re-encodes to different bytes";
    default: return "refused";
    }
}

static int kind_of(const char *s)
{
    static const char *const nm[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    static const int k[] = {OSC_T_BOOL, OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_U64, OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64};
    for (int i = 0; i < 9; i++) if (!strcmp(s, nm[i])) return k[i];
    return -1;
}

static const char *kname(int s)
{
    static const char *const nm[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    static const int k[] = {OSC_T_BOOL, OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_U64, OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64};
    for (int i = 0; i < 9; i++) if (k[i] == s) return nm[i];
    return s == OSC_T_VOID ? "void" : "array/other";
}

static int cmd_check(const char *mode, const char *file, const char *reqf)
{
    size_t n, rn;
    unsigned char *src = slurp(file, &n), *req = slurp(reqf, &rn);
    if (!src || !req) { fprintf(stderr, "x445: cannot read input\n"); return 2; }
    char entry[64] = "", pk[6][8];
    int np = -1, ret = -1;
    for (char *l = strtok((char *)req, "\n"); l; l = strtok(NULL, "\n")) {
        char *h = strchr(l, '#');
        if (h) *h = 0;
        char a[16], b[64];
        if (sscanf(l, "%15s %63s", a, b) < 1) continue;
        if (!strcmp(a, "entry")) snprintf(entry, sizeof entry, "%s", b);
        else if (!strcmp(a, "ret")) ret = kind_of(b);
        else if (!strcmp(a, "params")) {
            np = 0;
            char *save = NULL;
            strtok_r(l, " \t", &save);
            for (char *t = strtok_r(NULL, " \t", &save); t && np < 6; t = strtok_r(NULL, " \t", &save)) snprintf(pk[np++], 8, "%s", t);
        }
    }
    if (!entry[0] || np < 0 || ret < 0) { fprintf(stderr, "x445: requirement lacks entry/params/ret\n"); return 2; }
    OscUnit *u = calloc(1, sizeof *u);
    if (!u) return 2;
    if (!strcmp(mode, "osc")) {
        OscDiag d;
        memset(&d, 0, sizeof d);
        if (osc_compile((const char *)src, n, u, &d, NULL)) {
            printf("REFUSED compiler: %s at line %u: %s\n", osc_diag_kind_name(d.kind), d.line, d.message);
            return 1;
        }
    } else if (!strcmp(mode, "oscir")) {
        int rc = osc_ir_decode(src, n, u);
        if (rc) { printf("REFUSED decoder: %s (%s)\n", osc_ir_refusal_name(rc), meaning(rc)); return 1; }
    } else { fprintf(stderr, "x445: mode osc|oscir\n"); return 2; }
    int fi = -1;
    for (unsigned i = 0; i < u->nfuncs; i++) if (!strcmp(u->funcs[i].name, entry)) { fi = (int)i; break; }
    if (fi < 0) { printf("REFUSED signature: no function named %s\n", entry); return 1; }
    const OscFunc *f = &u->funcs[fi];
    int ok = (int)f->nparams == np && (int)f->ret.s == ret && f->ret.ref == OSC_REF_NONE;
    for (int i = 0; ok && i < np; i++) ok = (int)f->vtype[i].s == kind_of(pk[i]) && f->vtype[i].ref == OSC_REF_NONE;
    if (!ok) {
        char have[128] = "", want[128] = "";
        for (unsigned i = 0; i < f->nparams; i++) { strcat(have, i ? ", " : ""); strcat(have, f->vtype[i].ref ? "array" : kname(f->vtype[i].s)); }
        for (int i = 0; i < np; i++) { strcat(want, i ? ", " : ""); strcat(want, pk[i]); }
        printf("REFUSED signature: %s takes (%s) -> %s; the task needs (%s) -> %s\n", entry, have,
               f->ret.ref ? "array" : kname(f->ret.s), want, kname(ret));
        return 1;
    }
    OscCode code;
    memset(&code, 0, sizeof code);
    char err[160] = "";
    if (osc_cg_compile(u, &code, err, sizeof err)) { printf("REFUSED backend: %s\n", err); return 1; }
    osc_cg_free(&code);
    printf("OK\n");
    return 0;
}

static int cmd_wilson(const char *ks, const char *ns)
{
    char *e1, *e2;
    long k = strtol(ks, &e1, 10), n = strtol(ns, &e2, 10);
    if (*e1 || *e2 || n <= 0 || k < 0 || k > n) { fprintf(stderr, "x445: wilson k n with 0 <= k <= n, n > 0\n"); return 2; }
    const double z = 1.959964, p = (double)k / (double)n, z2 = z * z, d = 1 + z2 / (double)n;
    const double c = (p + z2 / (2.0 * (double)n)) / d, h = z * sqrt(p * (1 - p) / (double)n + z2 / (4.0 * (double)n * (double)n)) / d;
    double lo = c - h, hi = c + h;
    if (lo < 0) lo = 0;
    if (hi > 1) hi = 1;
    printf("%.4f %.4f %.4f\n", p, lo, hi);
    return 0;
}

/* exact two-sided McNemar test on the discordant pairs: b (only the first arm succeeds) and c (only the
 * second): p = min(1, 2 * P(X <= min(b, c))), X ~ Binomial(b + c, 1/2); p = 1 when b + c = 0 */
static int cmd_mcnemar(const char *bs, const char *cs)
{
    char *e1, *e2;
    long b = strtol(bs, &e1, 10), c = strtol(cs, &e2, 10);
    if (*e1 || *e2 || !*bs || !*cs || b < 0 || c < 0 || b > 1000000 || c > 1000000 || b + c > 1000000) { fprintf(stderr, "x445: mcnemar b c with 0 <= b, c and b + c <= 1000000\n"); return 2; }
    long n = b + c, m = b < c ? b : c;
    double s = 0;
    for (long i = 0; i <= m && n > 0; i++)
        s += exp(lgamma((double)n + 1) - lgamma((double)i + 1) - lgamma((double)(n - i) + 1) - (double)n * log(2.0));
    double p = n == 0 ? 1.0 : 2 * s;
    if (p > 1) p = 1;
    printf("%.6f\n", p);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "extract")) return cmd_extract(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "hex")) return cmd_hex(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "tohex")) return cmd_tohex(argv[2]);
    if (argc == 5 && !strcmp(argv[1], "check")) return cmd_check(argv[2], argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[1], "wilson")) return cmd_wilson(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "mcnemar")) return cmd_mcnemar(argv[2], argv[3]);
    fprintf(stderr, "usage: x445 extract <content> <out> | hex <in> <out> | tohex <in> | check osc|oscir <file> <requirement> | wilson <k> <n> | mcnemar <b> <c>\n");
    return 2;
}
