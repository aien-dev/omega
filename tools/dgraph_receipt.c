/*
 * dgraph_receipt.c -- decode-graph performance receipt (aien-architecture#233, lane P1). Host only.
 *
 * Turns one existing run of the decode-graph chip test (tests/gpu_decode_graph_test.c, driven by
 * tools/run_gpu_decode_graph_chip.sh) into ONE machine-readable, commit-bound receipt
 * "omega-decode-graph-receipt/1", and checks such receipts. It reads files only: no GPU, no
 * libomega_gpu, no new counters. Every number it writes is either copied from the run's own files
 * or derived from them by a rule the checker re-applies. Spec: docs/perf/DECODE_GRAPH_RECEIPT.md.
 *
 *   dgraph_receipt --run-dir DIR --stats FILE --log FILE --status STATUS --repo OWNER/NAME
 *                  --model TEXT --precision TEXT --mode TEXT --runner TEXT [--evidence PATH]...
 *                  [--out FILE]
 *       DIR holds the runner's device.txt, binaries.sha256 and build.log; FILE (stats) is the test's
 *       --out JSON; the log is the test's stdout (its "dgraph prefetch" and "fp8 knobs" lines).
 *       The receipt is checked before it is written; a receipt the checker refuses is never written.
 *   dgraph_receipt --check FILE [--evidence-root DIR]
 *       exit 0 and "dgraph_receipt: OK FILE" when the receipt passes; exit 1 and
 *       "dgraph_receipt: REJECT FILE: <reason>" otherwise. With --evidence-root every evidence path
 *       must exist under DIR.
 * Exit 2: usage or unreadable input.
 */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SCHEMA "omega-decode-graph-receipt/1"
#define MAX_INPUT (4u << 20)
#define MAX_DEPTH 32

/* ------------------------------------------------------------------ JSON reader (strict) */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JV {
    JType t;
    int b;          /* J_BOOL */
    int is_int;     /* J_NUM: literal had no '.', 'e', 'E' and fits int64 */
    int64_t i;
    double d;
    char *s;        /* J_STR (NUL-terminated, no embedded NUL accepted) */
    size_t n;       /* J_ARR / J_OBJ element count */
    struct JV **v;
    char **k;       /* J_OBJ keys */
} JV;

typedef struct { const char *p, *end; char err[160]; } Parser;

static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fputs("dgraph_receipt: out of memory\n", stderr); exit(2); } return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { fputs("dgraph_receipt: out of memory\n", stderr); exit(2); } return p; }

static void jfree(JV *v) {
    if (!v) return;
    for (size_t i = 0; i < v->n; i++) { jfree(v->v[i]); if (v->k) free(v->k[i]); }
    free(v->v); free(v->k); free(v->s); free(v);
}

static void perr(Parser *P, const char *m) { if (!P->err[0]) snprintf(P->err, sizeof P->err, "invalid JSON: %s", m); }
static void skip_ws(Parser *P) { while (P->p < P->end && (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r')) P->p++; }
static JV *jnew(JType t) { JV *v = xmalloc(sizeof *v); memset(v, 0, sizeof *v); v->t = t; return v; }

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int read_hex4(Parser *P, unsigned *out) {
    if (P->end - P->p < 4) return -1;
    unsigned u = 0;
    for (int i = 0; i < 4; i++) { int h = hexv(P->p[i]); if (h < 0) return -1; u = u * 16u + (unsigned)h; }
    P->p += 4; *out = u; return 0;
}
static void put_utf8(char **b, size_t *n, size_t *cap, unsigned cp) {
    if (*n + 5 > *cap) { *cap = *cap * 2 + 8; *b = xrealloc(*b, *cap); }
    if (cp < 0x80) (*b)[(*n)++] = (char)cp;
    else if (cp < 0x800) { (*b)[(*n)++] = (char)(0xC0 | (cp >> 6)); (*b)[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { (*b)[(*n)++] = (char)(0xE0 | (cp >> 12)); (*b)[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); (*b)[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
    else { (*b)[(*n)++] = (char)(0xF0 | (cp >> 18)); (*b)[(*n)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); (*b)[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); (*b)[(*n)++] = (char)(0x80 | (cp & 0x3F)); }
}

static char *parse_str(Parser *P) {
    if (P->p >= P->end || *P->p != '"') { perr(P, "expected string"); return NULL; }
    P->p++;
    size_t n = 0, cap = 32; char *b = xmalloc(cap);
    while (P->p < P->end && *P->p != '"') {
        unsigned char c = (unsigned char)*P->p++;
        if (c < 0x20) { perr(P, "control character in string"); free(b); return NULL; }
        if (c == '\\') {
            if (P->p >= P->end) break;
            char e = *P->p++; unsigned cp;
            switch (e) {
            case '"': cp = '"'; break; case '\\': cp = '\\'; break; case '/': cp = '/'; break;
            case 'b': cp = 8; break; case 'f': cp = 12; break; case 'n': cp = 10; break; case 'r': cp = 13; break; case 't': cp = 9; break;
            case 'u':
                if (read_hex4(P, &cp)) { perr(P, "bad \\u escape"); free(b); return NULL; }
                if (cp >= 0xD800 && cp < 0xDC00) {
                    unsigned lo;
                    if (P->end - P->p < 6 || P->p[0] != '\\' || P->p[1] != 'u') { perr(P, "lone surrogate"); free(b); return NULL; }
                    P->p += 2;
                    if (read_hex4(P, &lo) || lo < 0xDC00 || lo > 0xDFFF) { perr(P, "lone surrogate"); free(b); return NULL; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) { perr(P, "lone surrogate"); free(b); return NULL; }
                if (cp == 0) { perr(P, "NUL in string"); free(b); return NULL; }
                break;
            default: perr(P, "bad escape"); free(b); return NULL;
            }
            put_utf8(&b, &n, &cap, cp);
        } else {
            if (n + 2 > cap) { cap *= 2; b = xrealloc(b, cap); }
            b[n++] = (char)c;
        }
    }
    if (P->p >= P->end) { perr(P, "unterminated string"); free(b); return NULL; }
    P->p++;
    b[n] = 0;
    return b;
}

static JV *parse_val(Parser *P, int depth);

static JV *parse_num(Parser *P) {
    const char *s = P->p; int frac = 0;
    if (P->p < P->end && *P->p == '-') P->p++;
    if (P->p >= P->end || !isdigit((unsigned char)*P->p)) { perr(P, "bad number"); return NULL; }
    if (*P->p == '0') P->p++; else while (P->p < P->end && isdigit((unsigned char)*P->p)) P->p++;
    if (P->p < P->end && *P->p == '.') {
        frac = 1; P->p++;
        if (P->p >= P->end || !isdigit((unsigned char)*P->p)) { perr(P, "bad number"); return NULL; }
        while (P->p < P->end && isdigit((unsigned char)*P->p)) P->p++;
    }
    if (P->p < P->end && (*P->p == 'e' || *P->p == 'E')) {
        frac = 1; P->p++;
        if (P->p < P->end && (*P->p == '+' || *P->p == '-')) P->p++;
        if (P->p >= P->end || !isdigit((unsigned char)*P->p)) { perr(P, "bad number"); return NULL; }
        while (P->p < P->end && isdigit((unsigned char)*P->p)) P->p++;
    }
    char tmp[64]; size_t len = (size_t)(P->p - s);
    if (len >= sizeof tmp) { perr(P, "number too long"); return NULL; }
    memcpy(tmp, s, len); tmp[len] = 0;
    JV *v = jnew(J_NUM);
    v->d = strtod(tmp, NULL);
    if (!frac) { errno = 0; long long ll = strtoll(tmp, NULL, 10); if (errno == 0) { v->is_int = 1; v->i = ll; } }
    return v;
}

static JV *parse_val(Parser *P, int depth) {
    if (depth > MAX_DEPTH) { perr(P, "nested too deep"); return NULL; }
    skip_ws(P);
    if (P->p >= P->end) { perr(P, "unexpected end"); return NULL; }
    char c = *P->p;
    if (c == '{' || c == '[') {
        const int obj = c == '{';
        JV *v = jnew(obj ? J_OBJ : J_ARR); P->p++;
        skip_ws(P);
        if (P->p < P->end && *P->p == (obj ? '}' : ']')) { P->p++; return v; }
        for (;;) {
            char *key = NULL;
            if (obj) {
                skip_ws(P);
                key = parse_str(P);
                if (!key) { jfree(v); return NULL; }
                for (size_t i = 0; i < v->n; i++) if (!strcmp(v->k[i], key)) { perr(P, "duplicate key"); free(key); jfree(v); return NULL; }
                skip_ws(P);
                if (P->p >= P->end || *P->p != ':') { perr(P, "expected ':'"); free(key); jfree(v); return NULL; }
                P->p++;
            }
            JV *e = parse_val(P, depth + 1);
            if (!e) { free(key); jfree(v); return NULL; }
            /* v->v and v->k are arrays of POINTERS (JV *, char *): the element size is the pointer size, not sizeof(JV).
             * Under-sizing would overflow on the 2nd element; tests/dgraph_receipt/run.sh parses wide objects and arrays under ASan. */
            v->v = xrealloc(v->v, (v->n + 1) * sizeof *v->v);
            if (obj) { v->k = xrealloc(v->k, (v->n + 1) * sizeof *v->k); v->k[v->n] = key; }
            v->v[v->n++] = e;
            skip_ws(P);
            if (P->p < P->end && *P->p == ',') { P->p++; continue; }
            if (P->p < P->end && *P->p == (obj ? '}' : ']')) { P->p++; return v; }
            perr(P, obj ? "expected ',' or '}'" : "expected ',' or ']'"); jfree(v); return NULL;
        }
    }
    if (c == '"') { char *s = parse_str(P); if (!s) return NULL; JV *v = jnew(J_STR); v->s = s; return v; }
    if (c == '-' || isdigit((unsigned char)c)) return parse_num(P);
    if (P->end - P->p >= 4 && !memcmp(P->p, "true", 4)) { P->p += 4; JV *v = jnew(J_BOOL); v->b = 1; return v; }
    if (P->end - P->p >= 5 && !memcmp(P->p, "false", 5)) { P->p += 5; return jnew(J_BOOL); }
    if (P->end - P->p >= 4 && !memcmp(P->p, "null", 4)) { P->p += 4; return jnew(J_NULL); }
    perr(P, "unexpected character");
    return NULL;
}

static JV *jparse(const char *buf, size_t n, char *err, size_t cap) {
    Parser P; P.p = buf; P.end = buf + n; P.err[0] = 0;
    if (memchr(buf, 0, n)) { snprintf(err, cap, "invalid JSON: NUL byte"); return NULL; }
    JV *v = parse_val(&P, 0);
    if (v) { skip_ws(&P); if (P.p != P.end) { perr(&P, "trailing data"); jfree(v); v = NULL; } }
    if (!v) snprintf(err, cap, "%s", P.err);
    return v;
}

static JV *jget(const JV *o, const char *k) {
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++) if (!strcmp(o->k[i], k)) return o->v[i];
    return NULL;
}
/* dotted path: "timeline.span_ns" */
static JV *jpath(const JV *o, const char *path) {
    char tmp[128]; snprintf(tmp, sizeof tmp, "%s", path);
    const JV *cur = o;
    for (char *save = NULL, *tok = strtok_r(tmp, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) { cur = jget(cur, tok); if (!cur) return NULL; }
    return (JV *)cur;
}

/* ------------------------------------------------------------------ files */
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0; char *b = xmalloc(cap + 1);
    for (;;) {
        if (n == cap) { if (cap >= MAX_INPUT) { fclose(f); free(b); errno = EFBIG; return NULL; } cap *= 2; b = xrealloc(b, cap + 1); }
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    int bad = ferror(f); fclose(f);
    if (bad) { free(b); return NULL; }
    b[n] = 0; if (len) *len = n;
    return b;
}

static char *join(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    char *r = xmalloc(a + b + 2);
    memcpy(r, dir, a); r[a] = '/'; memcpy(r + a + 1, name, b + 1);
    if (a && dir[a - 1] == '/') memmove(r + a, r + a + 1, b + 1);
    return r;
}

/* ------------------------------------------------------------------ JSON writer (pretty, fixed key order) */
typedef struct { FILE *f; int depth; int first[MAX_DEPTH + 2]; } W;
static void w_indent(W *w) { fputc('\n', w->f); for (int i = 0; i < w->depth; i++) fputs("  ", w->f); }
static void w_str(W *w, const char *s) {
    fputc('"', w->f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', w->f); fputc(*p, w->f); }
        else if (*p == '\n') fputs("\\n", w->f);
        else if (*p == '\t') fputs("\\t", w->f);
        else if (*p < 0x20) fprintf(w->f, "\\u%04x", *p);
        else fputc(*p, w->f);
    }
    fputc('"', w->f);
}
static void w_key(W *w, const char *k) {
    if (!w->first[w->depth]) fputc(',', w->f);
    w->first[w->depth] = 0;
    w_indent(w);
    if (k) { w_str(w, k); fputs(": ", w->f); }
}
static void w_open(W *w, const char *k, char c) { w_key(w, k); fputc(c, w->f); w->depth++; w->first[w->depth] = 1; }
static void w_close(W *w, char c) { int empty = w->first[w->depth]; w->depth--; if (!empty) w_indent(w); fputc(c, w->f); }
static void kv_s(W *w, const char *k, const char *v) { w_key(w, k); w_str(w, v); }
static void kv_i(W *w, const char *k, int64_t v) { w_key(w, k); fprintf(w->f, "%" PRId64, v); }
static void kv_b(W *w, const char *k, int v) { w_key(w, k); fputs(v ? "true" : "false", w->f); }
static void kv_null(W *w, const char *k) { w_key(w, k); fputs("null", w->f); }
static void kv_fmt(W *w, const char *k, const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); kv_s(w, k, b);
}

/* ------------------------------------------------------------------ checker */
static const char *const STATUSES[] = { "VERIFIED", "EXPERIMENTAL", "HISTORICAL", "NOT_RUN", "FAIL", "SUPERSEDED" };
static const char *const TOP_KEYS[] = { "schema", "status", "status_note", "run_verdict", "not_run_reason", "source", "run", "workload", "hardware",
                                        "build", "per_token", "stamped_token", "aggregate", "serial_vs_overlap", "methodology", "evidence", "limitations" };

typedef struct { char *msg; size_t cap; int bad; } Err;
static int rej(Err *e, const char *fmt, ...) {
    if (!e->bad) { va_list ap; va_start(ap, fmt); vsnprintf(e->msg, e->cap, fmt, ap); va_end(ap); e->bad = 1; }
    return -1;
}

static int is_placeholder(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n == 0) return 1;
    static const char *const P[] = { "tbd", "tba", "todo", "fixme", "unknown", "unk", "n/a", "na", "none", "null", "nil", "placeholder", "xxx", "?", "??", "???", "-", "...", "<tbd>", "<unknown>" };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) if (strlen(P[i]) == n && !strncasecmp(s, P[i], n)) return 1;
    return 0;
}
/* every string (value and key) anywhere in the document */
static int scan_placeholders(const JV *v, const char *where, Err *e) {
    if (v->t == J_STR && is_placeholder(v->s)) return rej(e, "placeholder value \"%s\" at %s", v->s, where);
    for (size_t i = 0; i < v->n; i++) {
        char sub[256];
        if (v->t == J_OBJ) {
            if (is_placeholder(v->k[i])) return rej(e, "placeholder key \"%s\" in %s", v->k[i], where);
            snprintf(sub, sizeof sub, "%s.%s", where, v->k[i]);
        } else snprintf(sub, sizeof sub, "%s[%zu]", where, i);
        if (scan_placeholders(v->v[i], sub, e)) return -1;
    }
    return 0;
}

static const JV *need(const JV *o, const char *path, JType t, Err *e) {
    const JV *v = jpath(o, path);
    if (!v) { rej(e, "missing field %s", path); return NULL; }
    if (v->t != t) { rej(e, "field %s has the wrong type", path); return NULL; }
    return v;
}
static const char *need_s(const JV *o, const char *path, Err *e) { const JV *v = need(o, path, J_STR, e); return v ? v->s : NULL; }
static int need_i(const JV *o, const char *path, int64_t min, int64_t *out, Err *e) {
    const JV *v = need(o, path, J_NUM, e);
    if (!v) return -1;
    if (!v->is_int) return rej(e, "field %s is not an integer", path);
    if (v->i < min) return rej(e, "field %s = %" PRId64 " is below %" PRId64, path, v->i, min);
    if (out) *out = v->i;
    return 0;
}
static int need_b(const JV *o, const char *path, int *out, Err *e) { const JV *v = need(o, path, J_BOOL, e); if (!v) return -1; if (out) *out = v->b; return 0; }
static int is_hex(const char *s, size_t n) {
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
static int need_hex(const JV *o, const char *path, size_t n, Err *e) {
    const char *s = need_s(o, path, e);
    if (!s) return -1;
    if (!is_hex(s, n)) return rej(e, "field %s \"%s\" is not %zu lowercase hex digits", path, s, n);
    return 0;
}
static int need_str_list(const JV *o, const char *path, Err *e) {
    const JV *a = need(o, path, J_ARR, e);
    if (!a) return -1;
    if (a->n == 0) return rej(e, "field %s is an empty list", path);
    for (size_t i = 0; i < a->n; i++) if (a->v[i]->t != J_STR) return rej(e, "field %s[%zu] is not a string", path, i);
    return 0;
}

/* the derived-number rules (also used by the generator, so both sides apply the same rule) */
static int check_overhead(const JV *sec, const char *name, Err *e) {
    char p[96]; int64_t tot, chip, oh;
    snprintf(p, sizeof p, "%s.total_ns", name); if (need_i(sec, p, 0, &tot, e)) return -1;
    snprintf(p, sizeof p, "%s.chip_ns", name); if (need_i(sec, p, 0, &chip, e)) return -1;
    snprintf(p, sizeof p, "%s.host_overhead_ns", name); if (need_i(sec, p, INT64_MIN, &oh, e)) return -1;
    if (oh != tot - chip) return rej(e, "derived %s.host_overhead_ns = %" PRId64 " but total_ns - chip_ns = %" PRId64, name, oh, tot - chip);
    int64_t l, s;
    snprintf(p, sizeof p, "%s.launches", name); if (need_i(sec, p, 1, &l, e)) return -1;
    snprintf(p, sizeof p, "%s.submits", name); if (need_i(sec, p, 1, &s, e)) return -1;
    if (s > l) return rej(e, "%s.submits %" PRId64 " exceeds launches %" PRId64, name, s, l);
    snprintf(p, sizeof p, "%s.sems_done", name);
    const JV *sd = jpath(sec, p);
    if (!sd) return rej(e, "missing field per_token.%s", p);
    if (sd->t == J_NULL) { snprintf(p, sizeof p, "%s.sems_done_note", name); if (!need_s(sec, p, e)) return -1; }
    else if (sd->t != J_NUM || !sd->is_int || sd->i != l) return rej(e, "per_token.%s must equal launches or be null with a note", p);
    return 0;
}

static int check_measurements(const JV *r, Err *e) {
    const JV *pt = need(r, "per_token", J_OBJ, e);
    if (!pt) return -1;
    if (!need_s(pt, "statistic", e)) return -1;
    int64_t tokens, tok_meas;
    if (need_i(r, "workload.tokens", 1, &tokens, e)) return -1;
    if (need_i(pt, "tokens", 1, &tok_meas, e)) return -1;
    if (tok_meas != tokens) return rej(e, "per_token.tokens %" PRId64 " differs from workload.tokens %" PRId64, tok_meas, tokens);
    if (!need(pt, "graph", J_OBJ, e) || check_overhead(pt, "graph", e)) return -1;
    if (need_i(pt, "graph.record_ns", 0, NULL, e)) return -1;
    if (!need(pt, "flush_each", J_OBJ, e) || check_overhead(pt, "flush_each", e)) return -1;
    if (need_b(pt, "flush_each.identical_to_graph", NULL, e)) return -1;

    const JV *st = need(r, "stamped_token", J_OBJ, e);
    if (!st) return -1;
    int64_t sl, span, chip, un, sum;
    if (need_i(st, "launches", 1, &sl, e) || need_i(st, "span_ns", 1, &span, e) || need_i(st, "chip_ns", 1, &chip, e) ||
        need_i(st, "unattributed_ns", INT64_MIN, &un, e) || need_i(st, "stage_sum_ns", 0, &sum, e)) return -1;
    if (span > chip) return rej(e, "stamped_token.span_ns %" PRId64 " (device) exceeds chip_ns %" PRId64 " (the host span that contains it)", span, chip);
    if (un != chip - span) return rej(e, "derived stamped_token.unattributed_ns = %" PRId64 " but chip_ns - span_ns = %" PRId64, un, chip - span);
    const JV *stages = need(st, "stages", J_ARR, e);
    if (!stages) return -1;
    if (stages->n == 0) return rej(e, "stamped_token.stages is empty");
    int64_t ssum = 0, lsum = 0;
    for (size_t i = 0; i < stages->n; i++) {
        const JV *g = stages->v[i]; int64_t gl, gn;
        if (g->t != J_OBJ) return rej(e, "stamped_token.stages[%zu] is not an object", i);
        if (!need_s(g, "kind", e) || need_i(g, "launches", 1, &gl, e) || need_i(g, "device_ns", 0, &gn, e)) return -1;
        ssum += gn; lsum += gl;
    }
    if (ssum != sum) return rej(e, "derived stamped_token.stage_sum_ns = %" PRId64 " but the stages add up to %" PRId64, sum, ssum);
    if (sum != span) return rej(e, "stamped_token.stage_sum_ns %" PRId64 " differs from span_ns %" PRId64 " (consecutive stamps must partition the span)", sum, span);
    if (lsum != sl) return rej(e, "stamped_token stages count %" PRId64 " launches, launches = %" PRId64, lsum, sl);
    int64_t gl; if (need_i(pt, "graph.launches", 1, &gl, e)) return -1;
    if (gl != sl) return rej(e, "stamped_token.launches %" PRId64 " differs from per_token.graph.launches %" PRId64, sl, gl);
    return 0;
}

static int check_receipt(const JV *r, const char *evroot, Err *e) {
    if (r->t != J_OBJ) return rej(e, "receipt is not a JSON object");
    for (size_t i = 0; i < r->n; i++) {
        int ok = 0;
        for (size_t j = 0; j < sizeof TOP_KEYS / sizeof TOP_KEYS[0]; j++) if (!strcmp(r->k[i], TOP_KEYS[j])) ok = 1;
        if (!ok) return rej(e, "unknown top-level field %s", r->k[i]);
    }
    const char *schema = need_s(r, "schema", e);
    if (!schema) return -1;
    if (strcmp(schema, SCHEMA)) return rej(e, "schema \"%s\" is not \"" SCHEMA "\"", schema);
    if (scan_placeholders(r, "receipt", e)) return -1;
    const char *status = need_s(r, "status", e);
    if (!status) return -1;
    int known = 0;
    for (size_t i = 0; i < sizeof STATUSES / sizeof STATUSES[0]; i++) if (!strcmp(status, STATUSES[i])) known = 1;
    if (!known) return rej(e, "status \"%s\" is not one of VERIFIED EXPERIMENTAL HISTORICAL NOT_RUN FAIL SUPERSEDED", status);
    if (!need_s(r, "status_note", e)) return -1;
    const char *verdict = need_s(r, "run_verdict", e);
    if (!verdict) return -1;
    if (strcmp(verdict, "PASS") && strcmp(verdict, "FAIL") && strcmp(verdict, "NOT_RUN")) return rej(e, "run_verdict \"%s\" is not PASS, FAIL or NOT_RUN", verdict);

    /* source: commit-bound */
    if (!need(r, "source", J_OBJ, e) || !need_s(r, "source.repo", e) || need_hex(r, "source.commit", 40, e)) return -1;
    const char *repo = need_s(r, "source.repo", e);
    if (!strchr(repo, '/') || repo[0] == '/' || strchr(repo, ' ')) return rej(e, "source.repo \"%s\" is not OWNER/NAME", repo);
    int dirty; if (need_b(r, "source.dirty", &dirty, e)) return -1;

    /* status rules */
    const int not_run = !strcmp(status, "NOT_RUN");
    if (not_run != !strcmp(verdict, "NOT_RUN")) return rej(e, "status NOT_RUN and run_verdict NOT_RUN go together (status %s, run_verdict %s)", status, verdict);
    if (!strcmp(status, "VERIFIED") && dirty) return rej(e, "status VERIFIED with source.dirty true: a dirty tree cannot be verified");
    if ((!strcmp(status, "VERIFIED") || !strcmp(status, "EXPERIMENTAL")) && strcmp(verdict, "PASS")) return rej(e, "status %s needs run_verdict PASS (got %s)", status, verdict);

    /* workload, hardware, build */
    if (!need(r, "workload", J_OBJ, e) || !need_s(r, "workload.model", e) || !need_s(r, "workload.precision", e) || !need_s(r, "workload.mode", e) ||
        need_i(r, "workload.tokens", 1, NULL, e)) return -1;
    if (!need(r, "hardware", J_OBJ, e) || !need_s(r, "hardware.host", e) || !need_s(r, "hardware.gpu", e)) return -1;
    if (!need(r, "build", J_OBJ, e) || !need_s(r, "build.runner", e)) return -1;

    /* measurements: present for a run, absent (null) for NOT_RUN */
    const JV *pt = jget(r, "per_token"), *st = jget(r, "stamped_token");
    if (not_run) {
        if (!need_s(r, "not_run_reason", e)) return -1;
        if (!pt || pt->t != J_NULL || !st || st->t != J_NULL) return rej(e, "status NOT_RUN must carry per_token and stamped_token as null (nothing was measured)");
    } else {
        if (jget(r, "not_run_reason")) return rej(e, "not_run_reason is only for status NOT_RUN");
        if (need_hex(r, "build.binary_sha256", 64, e) || need_hex(r, "build.library_sha256", 64, e)) return -1;
        if (check_measurements(r, e)) return -1;
    }
    if (!need(r, "aggregate", J_OBJ, e)) return -1;
    if (!need(r, "serial_vs_overlap", J_OBJ, e) || !need_s(r, "serial_vs_overlap.chip_ns", e) || !need_s(r, "serial_vs_overlap.stages", e) ||
        need_b(r, "serial_vs_overlap.overlap_measurable", NULL, e) || !need_s(r, "serial_vs_overlap.reason", e)) return -1;
    if (!need(r, "methodology", J_OBJ, e) || !need_s(r, "methodology.device_clock", e) || !need_s(r, "methodology.host_clock", e) ||
        !need(r, "methodology.counters", J_OBJ, e)) return -1;
    static const char *const CTR[] = { "launches", "submits", "chip_ns", "record_ns", "total_ns", "sems_done", "stamps" };
    for (size_t i = 0; i < sizeof CTR / sizeof CTR[0]; i++) { char p[64]; snprintf(p, sizeof p, "methodology.counters.%s", CTR[i]); if (!need_s(r, p, e)) return -1; }
    if (need_str_list(r, "evidence", e) || need_str_list(r, "limitations", e)) return -1;
    const JV *ev = jget(r, "evidence");
    for (size_t i = 0; i < ev->n; i++) {
        const char *p = ev->v[i]->s;
        if (p[0] == '/' || strstr(p, "..") || strstr(p, "//")) return rej(e, "evidence path \"%s\" must be repo-relative without '..'", p);
        if (evroot) {
            char *full = join(evroot, p); struct stat sb; int ok = stat(full, &sb) == 0;
            free(full);
            if (!ok) return rej(e, "evidence path \"%s\" does not exist under %s", p, evroot);
        }
    }
    return 0;
}

static int check_buffer(const char *buf, size_t n, const char *evroot, char *msg, size_t cap) {
    JV *r = jparse(buf, n, msg, cap);
    if (!r) return -1;
    Err e = { msg, cap, 0 };
    int rc = check_receipt(r, evroot, &e);
    jfree(r);
    return rc;
}

/* ------------------------------------------------------------------ generator inputs */
typedef struct {
    char started[64], ended[64], runner_mode[64], commit[64], branch[128], physics_commit[64];
    long dirty;
    char host[128], kernel[128], gpu[128], driver[64];
} Device;

/* device.txt as tools/run_gpu_decode_graph_chip.sh writes it: "start <utc> mode=M omega=SHA branch=B dirty=N physics=P
 * physics_commit=SHA", then uname -a, nvidia-smi name,driver (absent when no GPU answered), free -g, "end <utc> ..." */
static int read_device(const char *path, Device *d, char *err, size_t cap) {
    char *txt = read_file(path, NULL);
    if (!txt) { snprintf(err, cap, "cannot read %s", path); return -1; }
    memset(d, 0, sizeof *d); d->dirty = -1;
    for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "start ", 6)) {
            char *s2 = NULL; int first = 1;
            for (char *t = strtok_r(line + 6, " ", &s2); t; t = strtok_r(NULL, " ", &s2)) {
                if (first) { snprintf(d->started, sizeof d->started, "%s", t); first = 0; continue; }
                char *eq = strchr(t, '='); if (!eq) continue; *eq = 0; const char *v = eq + 1;
                if (!strcmp(t, "mode")) snprintf(d->runner_mode, sizeof d->runner_mode, "%s", v);
                else if (!strcmp(t, "omega")) snprintf(d->commit, sizeof d->commit, "%s", v);
                else if (!strcmp(t, "branch")) snprintf(d->branch, sizeof d->branch, "%s", v);
                else if (!strcmp(t, "dirty")) { char *end; d->dirty = strtol(v, &end, 10); if (*end || d->dirty < 0) d->dirty = -1; }
                else if (!strcmp(t, "physics_commit")) snprintf(d->physics_commit, sizeof d->physics_commit, "%s", v);
            }
        } else if (!strncmp(line, "end ", 4)) {
            char *sp = strchr(line + 4, ' '); if (sp) *sp = 0;
            snprintf(d->ended, sizeof d->ended, "%s", line + 4);
        } else if (!strncmp(line, "Linux ", 6)) {
            char *s2 = NULL; strtok_r(line, " ", &s2);
            char *h = strtok_r(NULL, " ", &s2), *k = strtok_r(NULL, " ", &s2);
            if (h) snprintf(d->host, sizeof d->host, "%s", h);
            if (k) snprintf(d->kernel, sizeof d->kernel, "%s", k);
        } else if (!strncmp(line, "NVIDIA", 6) && !d->gpu[0]) {
            char *c = strstr(line, ", ");
            if (c) { *c = 0; snprintf(d->driver, sizeof d->driver, "%s", c + 2); }
            snprintf(d->gpu, sizeof d->gpu, "%s", line);
        }
    }
    free(txt);
    if (!d->started[0] || !d->commit[0] || d->dirty < 0 || !d->host[0]) { snprintf(err, cap, "%s lacks the runner's start line (omega=, dirty=) or the uname line", path); return -1; }
    if (!d->gpu[0]) { snprintf(d->gpu, sizeof d->gpu, "none_detected"); snprintf(d->driver, sizeof d->driver, "none_detected"); }
    return 0;
}

static int ends_with(const char *s, size_t n, const char *suf) { size_t m = strlen(suf); return n >= m && !memcmp(s + n - m, suf, m); }

/* binaries.sha256: "<sha>  ./build/gpu_decode_graph_test" and "<sha>  build/libomega_gpu.a" */
static int read_binaries(const char *path, char *bin, char *lib, char *err, size_t cap) {
    char *txt = read_file(path, NULL);
    if (!txt) { snprintf(err, cap, "cannot read %s", path); return -1; }
    bin[0] = lib[0] = 0;
    for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char sha[65]; if (strlen(line) < 66) continue;
        memcpy(sha, line, 64); sha[64] = 0;
        const char *name = line + 64; while (*name == ' ' || *name == '*') name++;
        if (!is_hex(sha, 64)) continue;
        size_t nl = strlen(name);
        if (ends_with(name, nl, "/gpu_decode_graph_test")) memcpy(bin, sha, 65);
        else if (ends_with(name, nl, "/libomega_gpu.a")) memcpy(lib, sha, 65);
    }
    free(txt);
    if (!bin[0] || !lib[0]) { snprintf(err, cap, "%s lacks the sha256 of gpu_decode_graph_test or libomega_gpu.a", path); return -1; }
    return 0;
}

typedef struct { int prefetch, red, fp8_variant, barrier, cached; char barrier_name[64]; } Knobs;
/* the test's first lines: "dgraph prefetch P (red R)" and "fp8 knobs: variant V barrier B (NAME) ... cached C" */
static int read_knobs(const char *path, Knobs *k, char *err, size_t cap) {
    char *txt = read_file(path, NULL);
    if (!txt) { snprintf(err, cap, "cannot read %s", path); return -1; }
    int have_pf = 0, have_fp8 = 0;
    for (char *save = NULL, *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!have_pf && sscanf(line, "dgraph prefetch %d (red %d)", &k->prefetch, &k->red) == 2) have_pf = 1;
        else if (!have_fp8 && sscanf(line, "fp8 knobs: variant %d barrier %d (%63[^)])", &k->fp8_variant, &k->barrier, k->barrier_name) == 3) {
            const char *c = strstr(line, " cached ");
            if (c && sscanf(c, " cached %d", &k->cached) == 1) have_fp8 = 1;
        }
    }
    free(txt);
    if (!have_pf || !have_fp8) {
        snprintf(err, cap, "%s lacks the \"dgraph prefetch\" or \"fp8 knobs\" line: the receipt needs the run's barrier and prefetch settings", path);
        return -1;
    }
    return 0;
}

static int sget(const JV *s, const char *path, int64_t *out, char *err, size_t cap) {
    const JV *v = jpath(s, path);
    if (!v || v->t != J_NUM || !v->is_int || v->i < 0) { snprintf(err, cap, "stats file: %s missing or not a non-negative integer", path); return -1; }
    *out = v->i; return 0;
}

/* ------------------------------------------------------------------ generator */
typedef struct {
    const char *run_dir, *stats, *log, *status, *repo, *model, *precision, *mode, *runner, *out;
    const char *evidence[32]; int n_evidence;
} Args;

static const char *STAMP_KINDS[] = { "norm", "qkv", "attn", "o_proj", "res_add", "router", "gate_up", "swiglu", "down", "combine_norm", "lm_head", "argmax" };

static int generate(const Args *a) {
    char err[512];
    size_t sn; char *sbuf = read_file(a->stats, &sn);
    if (!sbuf) { fprintf(stderr, "dgraph_receipt: cannot read %s\n", a->stats); return 2; }
    JV *s = jparse(sbuf, sn, err, sizeof err);
    free(sbuf);
    if (!s) { fprintf(stderr, "dgraph_receipt: %s: %s\n", a->stats, err); return 2; }
    char *dev_p = join(a->run_dir, "device.txt"), *bin_p = join(a->run_dir, "binaries.sha256"), *blog_p = join(a->run_dir, "build.log");
    Device d; Knobs k; memset(&k, 0, sizeof k);
    char bin[65], lib[65];
    int rc = 2;
    if (read_device(dev_p, &d, err, sizeof err) || read_binaries(bin_p, bin, lib, err, sizeof err) || read_knobs(a->log, &k, err, sizeof err)) goto fail;

    int64_t layers, vocab, pos, reps, red, cached, cl, cs, tl, span, tchip, mt, mc, mr, bt, bc, bl, bs, bid, checks, failed;
    if (sget(s, "layers", &layers, err, sizeof err) || sget(s, "vocab", &vocab, err, sizeof err) || sget(s, "pos", &pos, err, sizeof err) ||
        sget(s, "reps", &reps, err, sizeof err) || sget(s, "red", &red, err, sizeof err) || sget(s, "cached_weights", &cached, err, sizeof err) ||
        sget(s, "C.launches", &cl, err, sizeof err) || sget(s, "C.submits", &cs, err, sizeof err) ||
        sget(s, "timeline.launches", &tl, err, sizeof err) || sget(s, "timeline.span_ns", &span, err, sizeof err) || sget(s, "timeline.chip_ns", &tchip, err, sizeof err) ||
        sget(s, "median_ns.C_total", &mt, err, sizeof err) || sget(s, "median_ns.C_chip", &mc, err, sizeof err) || sget(s, "median_ns.C_record", &mr, err, sizeof err) ||
        sget(s, "median_ns.B_total", &bt, err, sizeof err) || sget(s, "median_ns.B_chip", &bc, err, sizeof err) ||
        sget(s, "B.launches", &bl, err, sizeof err) || sget(s, "B.submits", &bs, err, sizeof err) || sget(s, "B.identical", &bid, err, sizeof err) ||
        sget(s, "checks", &checks, err, sizeof err) || sget(s, "failed", &failed, err, sizeof err)) goto fail;
    const JV *verdict = jget(s, "verdict"), *pk = jpath(s, "timeline.per_kind_ns");
    if (!verdict || verdict->t != J_STR || (strcmp(verdict->s, "PASS") && strcmp(verdict->s, "FAIL"))) { snprintf(err, sizeof err, "stats file: verdict missing or not PASS/FAIL"); goto fail; }
    if (!pk || pk->t != J_OBJ || pk->n == 0) { snprintf(err, sizeof err, "stats file: timeline.per_kind_ns missing or empty"); goto fail; }
    if (cached != k.cached || red != k.red) { snprintf(err, sizeof err, "stats file and log disagree (cached %" PRId64 "/%d, red %" PRId64 "/%d): not the same run", cached, k.cached, red, k.red); goto fail; }
    if (!strcmp(verdict->s, "PASS") && failed != 0) { snprintf(err, sizeof err, "stats file: verdict PASS with failed %" PRId64, failed); goto fail; }
    for (size_t i = 0; i < pk->n; i++) {
        const JV *e = pk->v[i];
        if (e->t != J_ARR || e->n != 2 || e->v[0]->t != J_NUM || !e->v[0]->is_int || e->v[1]->t != J_NUM || !e->v[1]->is_int || e->v[0]->i < 1 || e->v[1]->i < 0) {
            snprintf(err, sizeof err, "stats file: timeline.per_kind_ns.%s is not [launches, ns]", pk->k[i]); goto fail;
        }
        int ok = 0; for (size_t j = 0; j < sizeof STAMP_KINDS / sizeof STAMP_KINDS[0]; j++) if (!strcmp(pk->k[i], STAMP_KINDS[j])) ok = 1;
        if (!ok) { snprintf(err, sizeof err, "stats file: unknown stage kind %s", pk->k[i]); goto fail; }
    }

    const int indep = k.prefetch ? (int)layers : 0; /* omega_gpu_decode_graph.c record_layer: one independent o_proj prefetch per layer */
    char *buf = NULL; size_t blen = 0;
    FILE *mf = open_memstream(&buf, &blen);
    if (!mf) { snprintf(err, sizeof err, "open_memstream failed"); goto fail; }
    W w; memset(&w, 0, sizeof w); w.f = mf; w.first[0] = 1;
    w.depth = 0; fputc('{', mf); w.depth = 1; w.first[1] = 1;

    kv_s(&w, "schema", SCHEMA);
    kv_s(&w, "status", a->status);
    if (!strcmp(a->status, "HISTORICAL"))
        kv_fmt(&w, "status_note", "reproduced from an existing evidence run (%s, started %s) by tools/dgraph_receipt.c; the chip was not run to make this receipt", a->run_dir, d.started);
    else
        kv_fmt(&w, "status_note", "status %s set by the operator for the run in %s (started %s)", a->status, a->run_dir, d.started);
    kv_s(&w, "run_verdict", verdict->s);

    w_open(&w, "source", '{');
    kv_s(&w, "repo", a->repo); kv_s(&w, "commit", d.commit); kv_b(&w, "dirty", d.dirty != 0);
    kv_fmt(&w, "dirty_basis", "device.txt dirty=%ld (git status --porcelain line count at run start)", d.dirty);
    if (d.branch[0]) kv_s(&w, "branch", d.branch);
    if (d.physics_commit[0]) kv_s(&w, "physics_commit", d.physics_commit);
    w_close(&w, '}');

    w_open(&w, "run", '{');
    kv_s(&w, "started_utc", d.started);
    if (d.ended[0]) kv_s(&w, "ended_utc", d.ended);
    if (d.runner_mode[0]) kv_s(&w, "runner_mode", d.runner_mode);
    kv_fmt(&w, "test_checks", "%" PRId64 " checks, %" PRId64 " failed", checks, failed);
    w_close(&w, '}');

    w_open(&w, "workload", '{');
    kv_s(&w, "model", a->model); kv_s(&w, "precision", a->precision); kv_s(&w, "mode", a->mode);
    kv_i(&w, "tokens", reps);
    kv_fmt(&w, "tokens_note", "%" PRId64 " timed graph tokens (the test's --reps), every one the same step at position %" PRId64 "; plus one stamped token", reps, pos);
    kv_i(&w, "layers", layers); kv_i(&w, "vocab", vocab); kv_i(&w, "pos", pos);
    kv_b(&w, "cached_weights", cached != 0); kv_b(&w, "prefetch", k.prefetch != 0); kv_b(&w, "red_mutant", red != 0);
    kv_fmt(&w, "barrier", "%d (%s)", k.barrier, k.barrier_name);
    kv_i(&w, "fp8_variant", k.fp8_variant);
    w_close(&w, '}');

    w_open(&w, "hardware", '{');
    kv_s(&w, "host", d.host); if (d.kernel[0]) kv_s(&w, "kernel", d.kernel);
    kv_s(&w, "gpu", d.gpu); kv_s(&w, "driver", d.driver);
    kv_s(&w, "basis", "device.txt: uname -a and nvidia-smi --query-gpu=name,driver_version at run start");
    w_close(&w, '}');

    w_open(&w, "build", '{');
    kv_s(&w, "runner", a->runner);
    kv_s(&w, "binary_sha256", bin); kv_s(&w, "library_sha256", lib);
    {
        size_t bn = 0; char *bl_txt = read_file(blog_p, &bn);
        char *line = NULL;
        if (bl_txt) for (char *save = NULL, *l = strtok_r(bl_txt, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) if (strstr(l, "-o build/gpu_decode_graph_test ")) { line = l; break; }
        if (line) kv_s(&w, "compile_command", line);
        else kv_fmt(&w, "compile_command_note", "%s (%zu bytes) holds no compile line for build/gpu_decode_graph_test: make did not rebuild it in this run, so the flags are not recorded; the binary is identified by binary_sha256", blog_p, bn);
        free(bl_txt);
    }
    w_close(&w, '}');

    w_open(&w, "per_token", '{');
    kv_fmt(&w, "statistic", "median over %" PRId64 " tokens (sorted value at index n/2), each field's median taken on its own, so derived differences are differences of medians", reps);
    kv_i(&w, "tokens", reps);
    w_open(&w, "graph", '{');
    kv_i(&w, "launches", cl); kv_i(&w, "submits", cs);
    kv_i(&w, "total_ns", mt); kv_i(&w, "chip_ns", mc); kv_i(&w, "record_ns", mr); kv_i(&w, "host_overhead_ns", mt - mc);
    kv_null(&w, "sems_done");
    kv_s(&w, "sems_done_note", "not written by gpu_decode_graph_test; submit_now() in omega_gpu_decode_graph.c fails the token when sems_done != launches, so a PASS run implies sems_done == launches for every timed token (inferred, not a recorded value)");
    w_close(&w, '}');
    w_open(&w, "flush_each", '{');
    kv_i(&w, "launches", bl); kv_i(&w, "submits", bs);
    kv_i(&w, "total_ns", bt); kv_i(&w, "chip_ns", bc); kv_i(&w, "host_overhead_ns", bt - bc);
    kv_null(&w, "sems_done");
    kv_s(&w, "sems_done_note", "as for graph: not recorded, implied by the PASS verdict");
    kv_b(&w, "identical_to_graph", bid != 0);
    kv_s(&w, "note", "the same recorded list submitted one launch per submission (TEST ONLY set_flush_each): the batching comparison inside the same run");
    w_close(&w, '}');
    w_close(&w, '}');

    w_open(&w, "stamped_token", '{');
    int64_t ssum = 0;
    for (size_t i = 0; i < pk->n; i++) ssum += pk->v[i]->v[1]->i;
    kv_i(&w, "launches", tl); kv_i(&w, "span_ns", span); kv_i(&w, "chip_ns", tchip);
    kv_i(&w, "unattributed_ns", tchip - span); kv_i(&w, "stage_sum_ns", ssum);
    kv_s(&w, "note", "one extra token run with set_stamps(1): span_ns and stage times are device PTIMER differences; chip_ns is that token's host-clock submit span; unattributed_ns = chip_ns - span_ns (submit call, front-end start, completion marker and host poll)");
    w_open(&w, "stages", '[');
    for (size_t i = 0; i < pk->n; i++) {
        w_open(&w, NULL, '{');
        kv_s(&w, "kind", pk->k[i]); kv_i(&w, "launches", pk->v[i]->v[0]->i); kv_i(&w, "device_ns", pk->v[i]->v[1]->i);
        w_close(&w, '}');
    }
    w_close(&w, ']');
    w_close(&w, '}');

    w_open(&w, "aggregate", '{');
    kv_b(&w, "available", 0);
    kv_s(&w, "reason", "the input receipt carries per-token medians and one stamped token only, no per-token arrays or run totals, so no aggregate sums or percentiles are derived (adding them needs the test to write every token's stats)");
    w_close(&w, '}');

    w_open(&w, "serial_vs_overlap", '{');
    kv_fmt(&w, "chip_ns", "serial: the sum over the token's submissions of one host-clock span each (%" PRId64 " submission per graph token, %" PRId64 " per flush_each token); spans never overlap each other", cs, bs);
    if (indep)
        kv_fmt(&w, "stages", "serial partition with %d independent launches: prefetch on, each layer's o_proj prefetch is marked independent (no barrier before it) and tagged attn, so its stamp is the front-end time while the attention may still run; the attn stage is the attention+prefetch window and the stages still add up to span_ns", indep);
    else if (k.barrier == 1)
        kv_s(&w, "stages", "barrier 1 (none, TEST ONLY): no WAIT_FOR_IDLE before the stamps, so a stage time is a front-end interval, not a launch duration");
    else
        kv_fmt(&w, "stages", "serial partition: barrier %d (%s) puts a WAIT_FOR_IDLE before every stamp and prefetch is off, so launches run one after another and the stage times add up exactly to span_ns", k.barrier, k.barrier_name);
    kv_b(&w, "overlap_measurable", 0);
    kv_s(&w, "reason", indep
         ? "an overlapped window is visible (the attn stage holds attention + prefetch), but no counter splits it, so how much of the two launches overlapped is not measurable from these counters"
         : "every launch waits for the previous one (WFI) and the host records before it submits, so nothing overlaps to measure; device time inside the host span is span_ns, the rest is unattributed_ns");
    w_close(&w, '}');

    w_open(&w, "methodology", '{');
    kv_s(&w, "device_clock", "GPU PTIMER ns from compute-class REPORT_SEMAPHORE timestamps (omega_gpu_cmdlist.h: \"out[0] = GPU timestamp (ns, PTIMER) before launch 0, out[i + 1] = after launch i completed (taken after its WAIT_FOR_IDLE)\"); used only by stamped_token span_ns and stages");
    kv_s(&w, "host_clock", "omega_gpu_session_now_ns(): the platform's time_now_ns, CLOCK_MONOTONIC on Linux (omega_gpu_platform_linux.c ref_now_ns); used by chip_ns, record_ns and total_ns");
    w_open(&w, "counters", '{');
    kv_s(&w, "launches", "omega_gpu_decode_graph.h: \"launches recorded for the token\"");
    kv_s(&w, "submits", "omega_gpu_decode_graph.h: \"submissions (1 normally; = launches with flush_each)\"");
    kv_s(&w, "chip_ns", "omega_gpu_decode_graph.h: \"sum of the list submissions' first-submit-to-last-marker time\". HOST clock, not device time: omega_gpu_cmdlist_submit() reads time_now_ns just before queue_submit and again after both completion marker words were seen and synced, so it holds device execution plus submit, fetch and poll latency");
    kv_s(&w, "record_ns", "omega_gpu_decode_graph.h: \"host: begin_token to the submit (recording)\"");
    kv_s(&w, "total_ns", "omega_gpu_decode_graph.h: \"host: begin_token to end_token return\"");
    kv_s(&w, "sems_done", "omega_gpu_decode_graph.h: \"launch semaphores found DONE\"");
    kv_s(&w, "stamps", "omega_gpu_decode_graph.h: \"Launch i took stamps[i + 1] - stamps[i] ns\"; gpu_decode_graph_test sums them per kind (per_kind_ns)");
    kv_s(&w, "host_overhead_ns", "derived: total_ns - chip_ns, both host clock: recording, attention counter check, output sync, token read and lock, everything outside the submit span");
    w_close(&w, '}');
    kv_s(&w, "source", "numbers copied from the stats file written by tests/gpu_decode_graph_test.c --out; settings from its log; identity from the runner's device.txt and binaries.sha256");
    w_close(&w, '}');

    w_open(&w, "evidence", '[');
    w_key(&w, NULL); w_str(&w, a->stats);
    w_key(&w, NULL); w_str(&w, a->log);
    w_key(&w, NULL); w_str(&w, dev_p);
    w_key(&w, NULL); w_str(&w, bin_p);
    w_key(&w, NULL); w_str(&w, blog_p);
    for (int i = 0; i < a->n_evidence; i++) { w_key(&w, NULL); w_str(&w, a->evidence[i]); }
    w_close(&w, ']');

    w_open(&w, "limitations", '[');
    static const char *const LIM[] = {
        "chip_ns is host-clock time around the device work; the only device-clock numbers are the stamped token's span_ns and stages",
        "per-token values are independent medians of a synthetic step on random weights (Qwen3-30B-A3B shapes, not a checkpoint), one fixed position, the same token every rep; not a serving or end-to-end tokens-per-second figure",
        "the stamped token is a single token: its stage times are one sample, not a distribution",
        "sems_done, the command list's own host_ns (method-stream build) and the marker-wait part of chip_ns are not recorded per token",
        "no aggregate totals or percentiles: the input has medians only",
        "this receipt is not a qualification: it re-expresses an existing run and claims nothing beyond that run's files",
    };
    for (size_t i = 0; i < sizeof LIM / sizeof LIM[0]; i++) { w_key(&w, NULL); w_str(&w, LIM[i]); }
    if (d.dirty) { w_key(&w, NULL); w_str(&w, "the run's tree was dirty: the commit alone does not reproduce the binary; see worktree.diff in the run directory when present"); }
    w_close(&w, ']');

    w_close(&w, '}');
    fputc('\n', mf);
    if (fclose(mf) != 0) { snprintf(err, sizeof err, "writing the receipt failed"); free(buf); goto fail; }

    if (check_buffer(buf, blen, NULL, err, sizeof err)) {
        fprintf(stderr, "dgraph_receipt: refusing to write: the receipt fails its own check: %s\n", err);
        free(buf); rc = 1; goto out;
    }
    FILE *o = a->out ? fopen(a->out, "wb") : stdout;
    if (!o) { fprintf(stderr, "dgraph_receipt: cannot write %s\n", a->out); free(buf); goto out; }
    int wbad = fwrite(buf, 1, blen, o) != blen;
    if (a->out) wbad |= fclose(o) != 0; else wbad |= fflush(o) != 0;
    free(buf);
    if (wbad) { fprintf(stderr, "dgraph_receipt: write failed\n"); goto out; }
    rc = 0;
    goto out;
fail:
    fprintf(stderr, "dgraph_receipt: %s\n", err);
out:
    jfree(s); free(dev_p); free(bin_p); free(blog_p);
    return rc;
}

static int usage(void) {
    fputs("usage: dgraph_receipt --run-dir DIR --stats FILE --log FILE --status STATUS --repo OWNER/NAME --model TEXT\n"
          "                      --precision TEXT --mode TEXT --runner TEXT [--evidence PATH]... [--out FILE]\n"
          "       dgraph_receipt --check FILE [--evidence-root DIR]\n", stderr);
    return 2;
}

int main(int argc, char **argv) {
    Args a; memset(&a, 0, sizeof a);
    const char *check = NULL, *evroot = NULL;
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        const char **slot = NULL;
        if (!strcmp(o, "--check")) slot = &check;
        else if (!strcmp(o, "--evidence-root")) slot = &evroot;
        else if (!strcmp(o, "--run-dir")) slot = &a.run_dir;
        else if (!strcmp(o, "--stats")) slot = &a.stats;
        else if (!strcmp(o, "--log")) slot = &a.log;
        else if (!strcmp(o, "--status")) slot = &a.status;
        else if (!strcmp(o, "--repo")) slot = &a.repo;
        else if (!strcmp(o, "--model")) slot = &a.model;
        else if (!strcmp(o, "--precision")) slot = &a.precision;
        else if (!strcmp(o, "--mode")) slot = &a.mode;
        else if (!strcmp(o, "--runner")) slot = &a.runner;
        else if (!strcmp(o, "--out")) slot = &a.out;
        else if (!strcmp(o, "--evidence")) {
            if (!v || a.n_evidence == (int)(sizeof a.evidence / sizeof a.evidence[0])) return usage();
            a.evidence[a.n_evidence++] = v; i++; continue;
        } else return usage();
        if (!v || *slot) return usage();
        *slot = v; i++;
    }
    if (check) {
        if (a.run_dir || a.stats || a.log || a.status || a.repo || a.model || a.precision || a.mode || a.runner || a.out || a.n_evidence) return usage();
        size_t n; char *buf = read_file(check, &n);
        if (!buf) { fprintf(stderr, "dgraph_receipt: cannot read %s\n", check); return 2; }
        char msg[512];
        int rc = check_buffer(buf, n, evroot, msg, sizeof msg);
        free(buf);
        if (rc) { printf("dgraph_receipt: REJECT %s: %s\n", check, msg); return 1; }
        printf("dgraph_receipt: OK %s\n", check);
        return 0;
    }
    if (evroot || !a.run_dir || !a.stats || !a.log || !a.status || !a.repo || !a.model || !a.precision || !a.mode || !a.runner) return usage();
    return generate(&a);
}
