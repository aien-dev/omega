/*
 * recheck.c -- strict validator of GEN records, schema v1 (tools/gen/abstract/RECORD_SCHEMA.md; GEN-3,
 * aien-spark/omega#478). Format and internal consistency only; it runs no program. `verify` re-runs the
 * derivation; this tool is the first gate of it and of every consumer.
 *
 *   recheck [--root <repo>] [--receipts <dir>] <record>...
 *     prints "OK <id>" per accepted record, or "REFUSED <file>: line <n>: <reason>" (first defect only);
 *     with --root, every `source` digest must equal the sha256 of <repo>/<path> (a stale digest is refused);
 *     with --receipts, <dir>/<receipt hex> must exist, start "gen-receipt 1" / "record <id>" and hash
 *     to its name.
 *   exit 0 every record accepted, 1 any refused, 2 usage.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define MAXB 65536
#define MAXL 1024
#define MAXLINES 2048

static char buf[MAXB + 2];
static char *ln[MAXLINES];
static int nl;
static const char *g_file;
static char g_err[512];
static int g_errline;

static int fail(int line, const char *fmt, const char *a)
{
    if (!g_err[0]) {
        g_errline = line;
        snprintf(g_err, sizeof g_err, fmt, a ? a : "");
    }
    return -1;
}

static int is_hex64(const char *s)
{
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return s[64] == 0;
}

static int is_taskid(const char *s, size_t n)
{
    if (n < 1 || n > 40) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

static const char *const SCALARS[] = {"bool", "uchar", "ushort", "uint", "ulong", "byte", "short", "int", "long"};
static const char *const RESERVED[] = {"fn", "to", "void", "end", "if", "else", "while", "bound", "for", "break", "ret",
    "put", "free", "trap", "add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "eq", "ne", "lt", "le",
    "gt", "ge", "neg", "not", "get", "new", "own", "edit", "view", "true", "false", "u8", "u16", "u32", "u64",
    "i8", "i16", "i32", "i64"};

static int is_scalar(const char *w)
{
    for (size_t i = 0; i < sizeof SCALARS / sizeof *SCALARS; i++) if (!strcmp(w, SCALARS[i])) return 1;
    return 0;
}

static int is_ident(const char *w)
{
    size_t n = strlen(w);
    if (n < 1 || n > 63) return 0;
    if (!((w[0] >= 'a' && w[0] <= 'z') || (w[0] >= 'A' && w[0] <= 'Z') || w[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++)
        if (!((w[i] >= 'a' && w[i] <= 'z') || (w[i] >= 'A' && w[i] <= 'Z') || (w[i] >= '0' && w[i] <= '9') || w[i] == '_')) return 0;
    if (is_scalar(w)) return 0;
    for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++) if (!strcmp(w, RESERVED[i])) return 0;
    return 1;
}

/* split a copy of s on single spaces into w (at most max words); returns the count */
static int words(const char *s, char *store, size_t storen, char **w, int max)
{
    size_t n = strlen(s);
    if (n + 1 > storen) return -1;
    memcpy(store, s, n + 1);
    int k = 0;
    char *p = store;
    while (*p) {
        if (k == max) return -1;
        w[k++] = p;
        char *q = strchr(p, ' ');
        if (!q) break;
        *q = 0;
        p = q + 1;
    }
    return k;
}

static int is_len(const char *w)
{
    if (!w[0] || (w[0] == '0') || strlen(w) > 2) return 0;
    for (const char *p = w; *p; p++) if (*p < '0' || *p > '9') return 0;
    int v = atoi(w);
    return v >= 1 && v <= 64;
}

/* a type at w[i]: scalar (1 word) or view|edit|own scalar len (3 words); returns words used or 0 */
static int type_at(char **w, int n, int i)
{
    if (i >= n) return 0;
    if (is_scalar(w[i])) return 1;
    if ((!strcmp(w[i], "view") || !strcmp(w[i], "edit") || !strcmp(w[i], "own")) && i + 2 < n && is_scalar(w[i + 1]) && is_len(w[i + 2]))
        return 3;
    return 0;
}

typedef struct {
    char name[64];
    int np;
    char pname[6][64];
    char ptype[6][32];   /* the type words joined by single spaces */
    int plen[6];         /* 0 scalar, else array length */
    char ret[32];
} Sig;

/* parse an Omegese fn line (as in the body) */
static int parse_sig(const char *s, Sig *g, int line)
{
    char st[MAXL + 1], *w[64];
    int n = words(s, st, sizeof st, w, 64);
    memset(g, 0, sizeof *g);
    if (n < 4 || strcmp(w[0], "fn")) return fail(line, "signature is not 'fn <name> (<param> <type>)* to <type>'", NULL);
    if (!is_ident(w[1])) return fail(line, "function name '%s' is not an identifier", w[1]);
    snprintf(g->name, sizeof g->name, "%s", w[1]);
    int i = 2;
    while (i < n && strcmp(w[i], "to")) {
        if (g->np == 6) return fail(line, "more than 6 parameters", NULL);
        if (!is_ident(w[i])) return fail(line, "parameter name '%s' is not an identifier (or is reserved)", w[i]);
        for (int k = 0; k < g->np; k++) if (!strcmp(g->pname[k], w[i])) return fail(line, "parameter name '%s' repeated", w[i]);
        if (!strcmp(w[i], g->name)) return fail(line, "parameter '%s' has the function's name", w[i]);
        snprintf(g->pname[g->np], 64, "%s", w[i]);
        int t = type_at(w, n, i + 1);
        if (!t) return fail(line, "parameter '%s' has no valid type", w[i]);
        if (t == 1) snprintf(g->ptype[g->np], 32, "%s", w[i + 1]);
        else { snprintf(g->ptype[g->np], 32, "%s %s %s", w[i + 1], w[i + 2], w[i + 3]); g->plen[g->np] = atoi(w[i + 3]); }
        g->np++;
        i += 1 + t;
    }
    if (i >= n) return fail(line, "signature has no 'to <type>'", NULL);
    i++;
    if (i < n && !strcmp(w[i], "void") && i + 1 == n) { snprintf(g->ret, sizeof g->ret, "void"); return 0; }
    int t = type_at(w, n, i);
    if (t != 1 || i + 1 != n) return fail(line, "return type must be one scalar type or void, and end the line", NULL);
    snprintf(g->ret, sizeof g->ret, "%s", w[i]);
    return 0;
}

static int is_int(const char *w)
{
    const char *p = w;
    if (*p == '-') p++;
    if (!*p || (p[0] == '0' && p[1]) || strlen(p) > 20) return 0;
    if (w[0] == '-' && !strcmp(p, "0")) return 0;
    for (; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* "<args> -> <value>" or "<args> -> TRAP <code>"; nargs expected; trap_ok allows TRAP */
static int parse_call(const char *s, int nargs, int trap_ok, int line, const char *what)
{
    char st[MAXL + 1], *w[80];
    int n = words(s, st, sizeof st, w, 80);
    if (n < 0) return fail(line, "%s has too many words", what);
    int a = 0;
    while (a < n && strcmp(w[a], "->")) {
        if (!is_int(w[a])) return fail(line, "%s argument is not a decimal number", what);
        a++;
    }
    if (a != nargs) return fail(line, "%s has the wrong number of arguments for the signature", what);
    if (a >= n) return fail(line, "%s has no '-> <result>'", what);
    if (a + 2 == n && is_int(w[a + 1])) return 0;
    if (trap_ok && a + 3 == n && !strcmp(w[a + 1], "TRAP") && is_int(w[a + 2]) && atoi(w[a + 2]) >= 1 && atoi(w[a + 2]) <= 14 && w[a + 2][0] != '-')
        return 0;
    return fail(line, trap_ok ? "%s result must be a decimal number or TRAP <1..14>" : "%s result must be a decimal number", what);
}

static int key(int i, const char *k, const char **val)
{
    size_t n = strlen(k);
    if (i >= nl || strncmp(ln[i], k, n) || ln[i][n] != ' ') return 0;
    *val = ln[i] + n + 1;
    return 1;
}

static int textline(const char *v, int line, const char *what)
{
    if (strlen(v) < 1 || strlen(v) > 160) return fail(line, "%s must be 1..160 bytes", what);
    return 0;
}

static void hexs(const uint8_t d[32], char out[65])
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

static int file_sha(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t b[8192];
    size_t r;
    while ((r = fread(b, 1, sizeof b, f)) > 0) sha256_update(&c, b, r);
    int bad = ferror(f);
    fclose(f);
    if (bad) return -1;
    uint8_t d[32];
    sha256_final(&c, d);
    hexs(d, out);
    return 0;
}

static int safe_path(const char *p)
{
    size_t n = strlen(p);
    if (n < 1 || n > 255 || p[0] == '/') return 0;
    for (size_t i = 0; i < n; i++)
        if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9') || strchr("_./-", p[i]))) return 0;
    const char *s = p;
    while (s) {
        const char *e = strchr(s, '/');
        size_t m = e ? (size_t)(e - s) : strlen(s);
        if (m == 0 || (m == 1 && s[0] == '.') || (m == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s = e ? e + 1 : NULL;
    }
    return 1;
}

static const char *irname(const char *s)
{
    static const char *const ir[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    for (size_t i = 0; i < sizeof SCALARS / sizeof *SCALARS; i++) if (!strcmp(s, SCALARS[i])) return ir[i];
    return "?";
}

/* family line: gs1-<16 hex> ids, single spaces, strictly ascending */
static int famlist(const char *v, int line)
{
    (void)line;
    char prev[32] = "";
    int n = 0;
    while (*v) {
        const char *e = strchr(v, ' ');
        size_t m = e ? (size_t)(e - v) : strlen(v);
        if (m != 20 || strncmp(v, "gs1-", 4)) return -1;
        for (size_t k = 4; k < 20; k++) if (!strchr("0123456789abcdef", v[k])) return -1;
        char cur[32];
        snprintf(cur, sizeof cur, "%.*s", (int)m, v);
        if (n && strcmp(prev, cur) >= 0) return -1;
        memcpy(prev, cur, sizeof cur);
        n++;
        if (!e) break;
        v = e + 1;
        if (!*v) return -1;
    }
    return n ? 0 : -1;
}

static int check(const char *root, const char *rdir)
{
    const char *v;
    int i = 0;
    char id[128] = "", kind[16] = "", receipt[65] = "";
    Sig sig;

    if (nl < 1 || strcmp(ln[0], "gen-record 1")) return fail(1, "line 1 must be 'gen-record 1'", NULL);
    i = 1;
    if (!key(i, "id", &v)) return fail(i + 1, "expected 'id <ID>'", NULL);
    snprintf(id, sizeof id, "%s", v);
    i++;
    if (!key(i, "kind", &v) || (strcmp(v, "raw") && strcmp(v, "abstraction"))) return fail(i + 1, "expected 'kind raw' or 'kind abstraction'", NULL);
    snprintf(kind, sizeof kind, "%s", v);
    int abs = !strcmp(kind, "abstraction");
    i++;
    if (abs) { if (strncmp(id, "abs-", 4) || strlen(id) != 20) return fail(2, "abstraction id must be abs-<16 hex>", NULL);
               for (int k = 4; k < 20; k++) if (!strchr("0123456789abcdef", id[k])) return fail(2, "abstraction id must be abs-<16 hex>", NULL); }
    else if (strncmp(id, "raw-", 4) || !is_taskid(id + 4, strlen(id + 4))) return fail(2, "raw id must be raw-<task_id>", NULL);
    if (!key(i, "split", &v) || (strcmp(v, "corpus") && strcmp(v, "probe"))) return fail(i + 1, "expected 'split corpus' or 'split probe'", NULL);
    if (abs && strcmp(v, "corpus")) return fail(i + 1, "an abstraction record is split corpus", NULL);
    i++;
    if (!key(i, "family", &v) || famlist(v, i + 1)) return fail(i + 1, "expected 'family <gs1-16 hex>+' (strictly ascending)", NULL);
    if (!abs && strchr(v, ' ')) return fail(i + 1, "a raw record has exactly one family", NULL);
    i++;
    const char *ent, *pars, *rt;
    int entl = i + 1;
    if (!key(i, "entry", &ent)) return fail(i + 1, "expected 'entry <name>'", NULL);
    i++;
    if (i < nl && !strcmp(ln[i], "params")) pars = "";
    else if (!key(i, "params", &pars)) return fail(i + 1, "expected 'params <type>...' (or just 'params')", NULL);
    i++;
    if (!key(i, "ret", &rt)) return fail(i + 1, "expected 'ret <type>'", NULL);
    i++;
    if (!key(i, "signature", &v)) return fail(i + 1, "expected 'signature <fn line>'", NULL);
    const char *sigline = v;
    int sigl = i + 1;
    if (parse_sig(v, &sig, i + 1)) return -1;
    if (strcmp(ent, sig.name)) return fail(entl, "entry differs from the signature's function name", NULL);
    {
        char want[512] = "", one[64];
        for (int k = 0; k < sig.np; k++) {
            if (sig.plen[k]) { char a[8], s[8]; int l; sscanf(sig.ptype[k], "%7s %7s %d", a, s, &l); snprintf(one, sizeof one, "%s %s %d", a, irname(s), l); }
            else snprintf(one, sizeof one, "%s", irname(sig.ptype[k]));
            if (k) strcat(want, " ");
            strcat(want, one);
        }
        if (strcmp(pars, want)) return fail(entl + 1, "params differ from the signature's parameter types (requirement spelling)", NULL);
        if (strcmp(rt, strcmp(sig.ret, "void") ? irname(sig.ret) : "void")) return fail(entl + 2, "ret differs from the signature's return type (requirement spelling)", NULL);
    }
    i++;
    if (!key(i, "desc", &v)) return fail(i + 1, "expected 'desc <text>'", NULL);
    if (textline(v, i + 1, "desc")) return -1;
    i++;
    if (!key(i, "receipt", &v) || !is_hex64(v)) return fail(i + 1, "expected 'receipt <64 lowercase hex>'", NULL);
    memcpy(receipt, v, 65);
    i++;
    if (!key(i, "validity", &v) || (strcmp(v, "verified") && strcmp(v, "unverified"))) return fail(i + 1, "expected 'validity verified' or 'validity unverified'", NULL);
    i++;
    /* ancestry + */
    int na = 0, ndist = 0;
    char prev[64] = "", dig[16][65];
    while (key(i, "ancestry", &v)) {
        const char *sp = strchr(v, ' ');
        if (!sp || !is_taskid(v, (size_t)(sp - v)) || !is_hex64(sp + 1)) return fail(i + 1, "expected 'ancestry <task_id> <64 hex>'", NULL);
        char t[64];
        snprintf(t, sizeof t, "%.*s", (int)(sp - v), v);
        if (na && strcmp(prev, t) >= 0) return fail(i + 1, "ancestry not strictly ascending by task id (or repeated)", NULL);
        int seen = 0;
        for (int k = 0; k < ndist && k < 16; k++) if (!strcmp(dig[k], sp + 1)) seen = 1;
        if (!seen && ndist < 16) memcpy(dig[ndist++], sp + 1, 65);
        if (!abs && strcmp(t, id + 4)) return fail(i + 1, "raw record ancestry must be its own task", NULL);
        snprintf(prev, sizeof prev, "%s", t);
        na++;
        i++;
    }
    if (na < 1) return fail(i + 1, "expected at least one 'ancestry' line", NULL);
    if (!abs && na != 1) return fail(i + 1, "raw record has exactly one ancestry line", NULL);
    if (abs && (na < 2 || ndist < 2)) return fail(i + 1, "abstraction needs ancestry in >= 2 distinct corpus solutions (task ids and digests)", NULL);
    /* source + */
    int ns = 0;
    char prevp[256] = "";
    while (key(i, "source", &v)) {
        const char *sp = strrchr(v, ' ');
        char p[256];
        if (!sp || (size_t)(sp - v) >= sizeof p) return fail(i + 1, "expected 'source <path> <64 hex>'", NULL);
        snprintf(p, sizeof p, "%.*s", (int)(sp - v), v);
        if (!safe_path(p) || !is_hex64(sp + 1)) return fail(i + 1, "expected 'source <relative path> <64 hex>'", NULL);
        if (ns && strcmp(prevp, p) >= 0) return fail(i + 1, "source not strictly ascending by path (or repeated)", NULL);
        if (root) {
            char full[1024], h[65];
            snprintf(full, sizeof full, "%s/%s", root, p);
            if (file_sha(full, h)) return fail(i + 1, "source file %s cannot be read", p);
            if (strcmp(h, sp + 1)) return fail(i + 1, "stale source digest for %s", p);
        }
        snprintf(prevp, sizeof prevp, "%s", p);
        ns++;
        i++;
    }
    if (ns < 1) return fail(i + 1, "expected at least one 'source' line", NULL);
    char fpr[65] = "";
    if (abs) {
        int nargs = 0;
        for (int k = 0; k < sig.np; k++) {
            if (!strcmp(sig.pname[k], "res")) return fail(sigl, "parameter name res is reserved for the postcondition", NULL);
            nargs += sig.plen[k] ? sig.plen[k] : 1;
        }
        if (!is_scalar(sig.ret)) return fail(sigl, "v1 abstraction helpers return a scalar", NULL);
        if (!key(i, "pre", &v)) return fail(i + 1, "expected 'pre STATED+TESTED <value>'", NULL);
        if (strncmp(v, "STATED+TESTED ", 14) || !v[14]) return fail(i + 1, "pre must be tagged STATED+TESTED (v1 proves nothing; PROVED is reserved)", NULL);
        i++;
        if (!key(i, "post", &v)) return fail(i + 1, "expected 'post STATED+TESTED <value>'", NULL);
        if (strncmp(v, "STATED+TESTED ", 14) || !v[14]) return fail(i + 1, "post must be tagged STATED+TESTED (v1 proves nothing; PROVED is reserved)", NULL);
        i++;
        if (!key(i, "example", &v)) return fail(i + 1, "expected 'example <args> -> <value>'", NULL);
        if (parse_call(v, nargs, 0, i + 1, "example")) return -1;
        i++;
        int nlim = 0;
        while (key(i, "limit", &v)) { if (textline(v, i + 1, "limit")) return -1; nlim++; i++; }
        if (!nlim) return fail(i + 1, "expected at least one 'limit <text>'", NULL);
        while (key(i, "counter", &v)) { if (parse_call(v, nargs, 1, i + 1, "counter")) return -1; i++; }
        if (!key(i, "fingerprint", &v) || !is_hex64(v)) return fail(i + 1, "expected 'fingerprint <64 hex>'", NULL);
        memcpy(fpr, v, 65);
        if (strncmp(fpr, id + 4, 16)) return fail(i + 1, "id is not abs- + the first 16 hex of the fingerprint", NULL);
        i++;
        int nd = 0;
        char prevd[64] = "";
        while (key(i, "differs", &v)) {
            const char *sp = strchr(v, ' ');
            if (!sp || !is_taskid(v, (size_t)(sp - v)) || !is_hex64(sp + 1)) return fail(i + 1, "expected 'differs <task_id> <64 hex>'", NULL);
            char t[64];
            snprintf(t, sizeof t, "%.*s", (int)(sp - v), v);
            if (nd && strcmp(prevd, t) >= 0) return fail(i + 1, "differs not strictly ascending by task id (or repeated)", NULL);
            if (!strcmp(sp + 1, fpr)) return fail(i + 1, "helper fingerprint equals the whole corpus solution of %s", t);
            snprintf(prevd, sizeof prevd, "%s", t);
            nd++;
            i++;
        }
        if (nd < 2) return fail(i + 1, "expected a 'differs' line for every corpus solution (at least 2)", NULL);
    }
    /* body */
    if (i >= nl || strcmp(ln[i], "body")) return fail(i + 1, "expected 'body'", NULL);
    int b0 = i + 1;
    if (nl - b0 < 1 || nl - b0 > 400) return fail(i + 1, "body must have 1..400 lines", NULL);
    int nfn = 0, lastfn = -1;
    for (int k = b0; k < nl; k++) if (!strncmp(ln[k], "fn ", 3)) { nfn++; lastfn = k; }
    if (!nfn) return fail(b0 + 1, "body has no 'fn' line", NULL);
    if (strcmp(ln[nl - 1], "end")) return fail(nl, "body must end with 'end'", NULL);
    if (abs) {
        if (nfn != 1 || strncmp(ln[b0], "fn ", 3)) return fail(b0 + 1, "abstraction body is exactly one function starting at its first line", NULL);
        if (strcmp(ln[b0], sigline)) return fail(sigl, "signature differs from the body's fn line", NULL);
    } else if (strcmp(ln[lastfn], sigline)) return fail(sigl, "signature differs from the body's last fn line (the entry)", NULL);
    /* receipt file */
    if (rdir) {
        char full[1024], h[65];
        snprintf(full, sizeof full, "%s/%s", rdir, receipt);
        if (file_sha(full, h)) return fail(11, "receipt file %s is missing", receipt);
        if (strcmp(h, receipt)) return fail(11, "receipt file does not hash to its id (fake or edited receipt)", NULL);
        FILE *f = fopen(full, "rb");
        char l1[64] = "", l2[160] = "", want[160];
        if (!f || !fgets(l1, sizeof l1, f) || !fgets(l2, sizeof l2, f)) { if (f) fclose(f); return fail(11, "receipt file is too short", NULL); }
        fclose(f);
        snprintf(want, sizeof want, "record %s\n", id);
        if (strcmp(l1, "gen-receipt 1\n") || strcmp(l2, want)) return fail(11, "receipt is not this record's (header or record line)", NULL);
    }
    return 0;
}

static int load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return fail(0, "cannot read the file", NULL);
    size_t n = fread(buf, 1, MAXB + 1, f);
    fclose(f);
    if (n > MAXB) return fail(0, "file over 65536 bytes", NULL);
    if (n == 0 || buf[n - 1] != '\n') return fail(0, "file is empty or does not end with LF", NULL);
    buf[n] = 0;
    nl = 0;
    int line = 1;
    char *s = buf;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)buf[k];
        if (c == '\n') {
            buf[k] = 0;
            size_t len = strlen(s);
            if (len == 0) return fail(line, "empty line", NULL);
            if (len > MAXL) return fail(line, "line over 1024 bytes", NULL);
            if (s[0] == ' ' || s[len - 1] == ' ') return fail(line, "line starts or ends with a space", NULL);
            if (strstr(s, "  ")) return fail(line, "two spaces in a row", NULL);
            if (nl == MAXLINES) return fail(line, "too many lines", NULL);
            ln[nl++] = s;
            s = buf + k + 1;
            line++;
        } else if (c < 0x20 || c > 0x7e) return fail(line, "byte outside ASCII 0x20..0x7E (CR, tab or non-ASCII)", NULL);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = NULL, *rdir = NULL;
    int i = 1, bad = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1] == '-') {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[i + 1];
        else if (!strcmp(argv[i], "--receipts") && i + 1 < argc) rdir = argv[i + 1];
        else { fprintf(stderr, "usage: recheck [--root <repo>] [--receipts <dir>] <record>...\n"); return 2; }
        i += 2;
    }
    if (i >= argc) { fprintf(stderr, "usage: recheck [--root <repo>] [--receipts <dir>] <record>...\n"); return 2; }
    for (; i < argc; i++) {
        g_file = argv[i];
        g_err[0] = 0;
        g_errline = 0;
        if (load(argv[i]) == 0 && check(root, rdir) == 0) {
            printf("OK %s\n", ln[1] + 3);
        } else {
            printf("REFUSED %s: line %d: %s\n", g_file, g_errline, g_err);
            bad = 1;
        }
    }
    return bad;
}
