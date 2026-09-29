/*
 * json_canon.c -- strict JSON check and Python-compatible canonical JSON.
 *
 * Reads one JSON document on stdin. Used by tools/m19r_qualify.sh (the M19R
 * receipt digest) and tests/visor/qualification/run_qualification.sh (JSON
 * line checks); it reproduces what the retired Python glue did with the
 * json module:
 *
 *   json_canon --check          exit 0 if stdin is one valid JSON document
 *                               (Python json.loads rules, strict UTF-8),
 *                               1 otherwise; no output
 *   json_canon                  json.dumps(sort_keys=True, separators=(",",
 *                               ":"), ensure_ascii=False): compact, UTF-8 raw
 *                               (the form the M19R receipt digest covers)
 *   json_canon --sha256         SHA-256 (hex) of that compact form
 *   json_canon --pretty         json.dumps(sort_keys=True, indent=2): non-ASCII
 *                               escaped as \uXXXX (the on-disk receipt form)
 *   json_canon --write-exclusive PATH
 *                               copy stdin to PATH, created O_EXCL mode 0444,
 *                               fsync'd; fails if PATH already exists
 *
 * Numbers follow Python's json module: a literal with no '.', 'e' or 'E' is an
 * integer and is kept digit for digit ("-0" becomes "0"); any other number is
 * a double printed like Python's float repr (shortest round-trip digits, ".0"
 * on integral values, exponent form below 1e-4 and from 1e16). This is why jq
 * cannot stand in here: jq keeps "84.800" verbatim and rewrites "1e3" as
 * "1E+3", which changes the digest. Object keys are sorted by code point, a
 * repeated key keeps its last value, NaN/Infinity are accepted, and a lone
 * \uD800-\uDFFF escape is accepted but cannot be written as UTF-8 (compact
 * and --sha256 fail, as Python's encode() did). C only; links src/sha256.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>

#include "sha256.h"

typedef enum { J_NULL, J_TRUE, J_FALSE, J_INT, J_FLOAT, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    char *s;          /* J_INT digits, J_STR bytes (UTF-8) */
    size_t n;         /* string length / element count */
    double d;
    JVal **items;     /* array elements or object values */
    char **keys;      /* object keys (UTF-8) */
    size_t *klen;
    size_t *order;    /* object output order (sorted, deduplicated) */
    size_t nout;
};

static const char *src;
static size_t pos, srclen;
static int lone_surrogate; /* saw a \uD800-\uDFFF escape with no partner */

static jmp_buf *soft_fail; /* set while checking lines: fail quietly */

static void die(const char *msg) {
    if (soft_fail) longjmp(*soft_fail, 1);
    fprintf(stderr, "json_canon: %s at byte %zu\n", msg, pos);
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "json_canon: out of memory\n"); exit(1); }
    return p;
}

static void ws(void) {
    while (pos < srclen && (src[pos] == ' ' || src[pos] == '\t' ||
                            src[pos] == '\n' || src[pos] == '\r'))
        pos++;
}

static int lit(const char *w) {
    size_t l = strlen(w);
    if (pos + l <= srclen && memcmp(src + pos, w, l) == 0) { pos += l; return 1; }
    return 0;
}

typedef struct { char *b; size_t n, cap; } Buf;

static void put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->b = xrealloc(b->b, b->cap);
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }
static void putc_(Buf *b, char c) { put(b, &c, 1); }

static void put_utf8(Buf *b, uint32_t cp) {
    char o[4];
    if (cp < 0x80) { o[0] = (char)cp; put(b, o, 1); }
    else if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); put(b, o, 2); }
    else if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); put(b, o, 3);
    } else {
        o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); put(b, o, 4);
    }
}

/* Decode one UTF-8 sequence at s[*i]; strict (no overlongs, no surrogates). */
static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *i, int wtf8) {
    unsigned char c = s[*i];
    uint32_t cp; int k;
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 3; }
    else die("invalid UTF-8");
    if (*i + (size_t)k >= n) die("truncated UTF-8");
    for (int j = 1; j <= k; j++) {
        if ((s[*i + j] & 0xC0) != 0x80) die("invalid UTF-8");
        cp = (cp << 6) | (s[*i + j] & 0x3F);
    }
    if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000) ||
        cp > 0x10FFFF || (!wtf8 && cp >= 0xD800 && cp <= 0xDFFF))
        die("invalid UTF-8");
    *i += (size_t)k + 1;
    return cp;
}

static unsigned hex4(void) {
    unsigned v = 0;
    if (pos + 4 > srclen) die("truncated \\u escape");
    for (int i = 0; i < 4; i++) {
        char c = src[pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else die("bad \\u escape");
    }
    return v;
}

static char *parse_string(size_t *len) {
    Buf b = {0};
    pos++; /* opening quote */
    for (;;) {
        if (pos >= srclen) die("unterminated string");
        unsigned char c = (unsigned char)src[pos];
        if (c == '"') { pos++; break; }
        if (c < 0x20) die("control character in string");
        if (c != '\\') {
            size_t i = pos;
            uint32_t cp = utf8_next((const unsigned char *)src, srclen, &i, 0);
            (void)cp;
            put(&b, src + pos, i - pos);
            pos = i;
            continue;
        }
        pos++;
        if (pos >= srclen) die("unterminated escape");
        char e = src[pos++];
        switch (e) {
        case '"': putc_(&b, '"'); break;
        case '\\': putc_(&b, '\\'); break;
        case '/': putc_(&b, '/'); break;
        case 'b': putc_(&b, '\b'); break;
        case 'f': putc_(&b, '\f'); break;
        case 'n': putc_(&b, '\n'); break;
        case 'r': putc_(&b, '\r'); break;
        case 't': putc_(&b, '\t'); break;
        case 'u': {
            uint32_t cp = hex4();
            if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < srclen &&
                src[pos] == '\\' && src[pos + 1] == 'u') {
                size_t save = pos;
                pos += 2;
                uint32_t lo = hex4();
                if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else pos = save;
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) lone_surrogate = 1; /* kept, as Python does */
            put_utf8(&b, cp);
            break;
        }
        default: die("bad escape");
        }
    }
    *len = b.n;
    putc_(&b, '\0');
    return b.b;
}

static int isdig(char c) { return c >= '0' && c <= '9'; }

static JVal *parse_value(int depth);

static JVal *newval(JType t) {
    JVal *v = calloc(1, sizeof *v);
    if (!v) die("out of memory");
    v->type = t;
    return v;
}

static JVal *parse_number(void) {
    size_t start = pos;
    int is_float = 0;
    if (src[pos] == '-') pos++;
    if (pos < srclen && src[pos] == 'I') { /* -Infinity */
        if (!lit("Infinity")) die("bad number");
        JVal *v = newval(J_FLOAT); v->d = -INFINITY; return v;
    }
    if (pos >= srclen || !isdig(src[pos])) die("bad number");
    if (src[pos] == '0') pos++;
    else while (pos < srclen && isdig(src[pos])) pos++;
    if (pos + 1 < srclen && src[pos] == '.' && isdig(src[pos + 1])) {
        is_float = 1; pos++;
        while (pos < srclen && isdig(src[pos])) pos++;
    }
    if (pos < srclen && (src[pos] == 'e' || src[pos] == 'E')) {
        size_t p = pos + 1;
        if (p < srclen && (src[p] == '+' || src[p] == '-')) p++;
        if (p < srclen && isdig(src[p])) {
            is_float = 1; pos = p;
            while (pos < srclen && isdig(src[pos])) pos++;
        }
    }
    size_t l = pos - start;
    char *text = xrealloc(NULL, l + 1);
    memcpy(text, src + start, l);
    text[l] = '\0';
    if (is_float) {
        JVal *v = newval(J_FLOAT);
        v->d = strtod(text, NULL);
        free(text);
        return v;
    }
    JVal *v = newval(J_INT);
    if (strcmp(text, "-0") == 0) { text[0] = '0'; text[1] = '\0'; }
    v->s = text;
    return v;
}

static int keycmp_idx(const JVal *o, size_t a, size_t b) {
    size_t la = o->klen[a], lb = o->klen[b];
    int c = memcmp(o->keys[a], o->keys[b], la < lb ? la : lb);
    if (c) return c;
    return (la > lb) - (la < lb);
}

static void sort_object(JVal *o) {
    /* stable insertion sort of indices by key; for equal keys keep the last */
    size_t *idx = xrealloc(NULL, o->n * sizeof *idx);
    for (size_t i = 0; i < o->n; i++) {
        size_t j = i;
        while (j > 0 && keycmp_idx(o, idx[j - 1], i) > 0) { idx[j] = idx[j - 1]; j--; }
        idx[j] = i;
    }
    size_t out = 0;
    for (size_t i = 0; i < o->n; i++) {
        if (i + 1 < o->n && keycmp_idx(o, idx[i], idx[i + 1]) == 0) continue;
        idx[out++] = idx[i];
    }
    o->order = idx;
    o->nout = out;
}

static JVal *parse_value(int depth) {
    if (depth > 512) die("nesting too deep");
    ws();
    if (pos >= srclen) die("expecting value");
    char c = src[pos];
    if (c == '{') {
        JVal *v = newval(J_OBJ);
        size_t cap = 0;
        pos++; ws();
        if (pos < srclen && src[pos] == '}') { pos++; sort_object(v); return v; }
        for (;;) {
            ws();
            if (pos >= srclen || src[pos] != '"') die("expecting property name");
            size_t kl;
            char *k = parse_string(&kl);
            ws();
            if (pos >= srclen || src[pos] != ':') die("expecting ':'");
            pos++;
            JVal *val = parse_value(depth + 1);
            if (v->n == cap) {
                cap = cap * 2 + 8;
                v->keys = xrealloc(v->keys, cap * sizeof *v->keys);
                v->klen = xrealloc(v->klen, cap * sizeof *v->klen);
                v->items = xrealloc(v->items, cap * sizeof *v->items);
            }
            v->keys[v->n] = k; v->klen[v->n] = kl; v->items[v->n] = val; v->n++;
            ws();
            if (pos < srclen && src[pos] == ',') { pos++; continue; }
            if (pos < srclen && src[pos] == '}') { pos++; break; }
            die("expecting ',' or '}'");
        }
        sort_object(v);
        return v;
    }
    if (c == '[') {
        JVal *v = newval(J_ARR);
        size_t cap = 0;
        pos++; ws();
        if (pos < srclen && src[pos] == ']') { pos++; return v; }
        for (;;) {
            JVal *val = parse_value(depth + 1);
            if (v->n == cap) { cap = cap * 2 + 8; v->items = xrealloc(v->items, cap * sizeof *v->items); }
            v->items[v->n++] = val;
            ws();
            if (pos < srclen && src[pos] == ',') { pos++; continue; }
            if (pos < srclen && src[pos] == ']') { pos++; break; }
            die("expecting ',' or ']'");
        }
        return v;
    }
    if (c == '"') { JVal *v = newval(J_STR); v->s = parse_string(&v->n); return v; }
    if (lit("null")) return newval(J_NULL);
    if (lit("true")) return newval(J_TRUE);
    if (lit("false")) return newval(J_FALSE);
    if (lit("NaN")) { JVal *v = newval(J_FLOAT); v->d = NAN; return v; }
    if (lit("Infinity")) { JVal *v = newval(J_FLOAT); v->d = INFINITY; return v; }
    if (c == '-' || isdig(c)) return parse_number();
    die("expecting value");
    return NULL;
}

/* Python float.__repr__ */
static void put_float(Buf *b, double d) {
    if (isnan(d)) { puts_(b, "NaN"); return; }
    if (isinf(d)) { puts_(b, d < 0 ? "-Infinity" : "Infinity"); return; }
    if (d == 0) { puts_(b, signbit(d) ? "-0.0" : "0.0"); return; }
    char e[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(e, sizeof e, "%.*e", p - 1, d);
        if (strtod(e, NULL) == d) break;
    }
    /* e = [-]D[.DDD]e[+-]XX */
    char digits[32];
    size_t nd = 0;
    const char *q = e;
    int neg = 0;
    if (*q == '-') { neg = 1; q++; }
    while (*q && *q != 'e') { if (isdig(*q)) digits[nd++] = *q; q++; }
    int exp10 = atoi(q + 1);
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = '\0';
    int decpt = exp10 + 1;
    if (neg) putc_(b, '-');
    if (decpt <= -4 || decpt > 16) {
        putc_(b, digits[0]);
        if (nd > 1) { putc_(b, '.'); put(b, digits + 1, nd - 1); }
        char x[16];
        snprintf(x, sizeof x, "e%c%02d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
        puts_(b, x);
    } else if (decpt <= 0) {
        puts_(b, "0.");
        for (int i = 0; i < -decpt; i++) putc_(b, '0');
        put(b, digits, nd);
    } else if ((size_t)decpt >= nd) {
        put(b, digits, nd);
        for (size_t i = nd; i < (size_t)decpt; i++) putc_(b, '0');
        puts_(b, ".0");
    } else {
        put(b, digits, (size_t)decpt);
        putc_(b, '.');
        put(b, digits + decpt, nd - (size_t)decpt);
    }
}

static void put_str(Buf *b, const char *s, size_t n, int ascii) {
    static const char hx[] = "0123456789abcdef";
    putc_(b, '"');
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { puts_(b, "\\\""); i++; }
        else if (c == '\\') { puts_(b, "\\\\"); i++; }
        else if (c == '\n') { puts_(b, "\\n"); i++; }
        else if (c == '\r') { puts_(b, "\\r"); i++; }
        else if (c == '\t') { puts_(b, "\\t"); i++; }
        else if (c == '\b') { puts_(b, "\\b"); i++; }
        else if (c == '\f') { puts_(b, "\\f"); i++; }
        else if (c < 0x20) {
            char u[7] = {'\\', 'u', '0', '0', hx[c >> 4], hx[c & 15], 0};
            puts_(b, u); i++;
        } else if (c < 0x7F || !ascii) { putc_(b, (char)c); i++; }
        else {
            uint32_t cp = utf8_next((const unsigned char *)s, n, &i, 1);
            uint32_t units[2]; int nu = 0;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                units[nu++] = 0xD800 | (cp >> 10);
                units[nu++] = 0xDC00 | (cp & 0x3FF);
            } else units[nu++] = cp;
            for (int k = 0; k < nu; k++) {
                char u[7] = {'\\', 'u', hx[(units[k] >> 12) & 15], hx[(units[k] >> 8) & 15],
                             hx[(units[k] >> 4) & 15], hx[units[k] & 15], 0};
                puts_(b, u);
            }
        }
    }
    putc_(b, '"');
}

static void newline(Buf *b, int indent, int level) {
    putc_(b, '\n');
    for (int i = 0; i < indent * level; i++) putc_(b, ' ');
}

static void emit(Buf *b, const JVal *v, int indent, int ascii, int level) {
    switch (v->type) {
    case J_NULL: puts_(b, "null"); break;
    case J_TRUE: puts_(b, "true"); break;
    case J_FALSE: puts_(b, "false"); break;
    case J_INT: puts_(b, v->s); break;
    case J_FLOAT: put_float(b, v->d); break;
    case J_STR: put_str(b, v->s, v->n, ascii); break;
    case J_ARR:
        if (!v->n) { puts_(b, "[]"); break; }
        putc_(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) putc_(b, ',');
            if (indent) newline(b, indent, level + 1);
            emit(b, v->items[i], indent, ascii, level + 1);
        }
        if (indent) newline(b, indent, level);
        putc_(b, ']');
        break;
    case J_OBJ:
        if (!v->nout) { puts_(b, "{}"); break; }
        putc_(b, '{');
        for (size_t i = 0; i < v->nout; i++) {
            size_t k = v->order[i];
            if (i) putc_(b, ',');
            if (indent) newline(b, indent, level + 1);
            put_str(b, v->keys[k], v->klen[k], ascii);
            puts_(b, indent ? ": " : ":");
            emit(b, v->items[k], indent, ascii, level + 1);
        }
        if (indent) newline(b, indent, level);
        putc_(b, '}');
        break;
    }
}

static Buf read_all(FILE *f) {
    Buf b = {0};
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) put(&b, tmp, n);
    if (ferror(f)) { fprintf(stderr, "json_canon: read error\n"); exit(1); }
    return b;
}

static int write_exclusive(const char *path, const Buf *in) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0444);
    if (fd < 0) {
        fprintf(stderr, "json_canon: [Errno %d] %s: '%s'\n", errno, strerror(errno), path);
        return 1;
    }
    size_t off = 0;
    while (off < in->n) {
        ssize_t w = write(fd, in->b + off, in->n - off);
        if (w < 0) { if (errno == EINTR) continue; perror("json_canon: write"); close(fd); return 1; }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) { perror("json_canon: fsync"); close(fd); return 1; }
    if (close(fd) != 0) { perror("json_canon: close"); return 1; }
    return 0;
}

/* One JSON document (Python json.loads rules)? Used by --check-lines. */
static int valid_document(const char *p, size_t n) {
    jmp_buf jb;
    if (setjmp(jb)) { soft_fail = NULL; return 0; }
    soft_fail = &jb;
    src = p; srclen = n; pos = 0;
    parse_value(0);
    ws();
    soft_fail = NULL;
    return pos == srclen;
}

int main(int argc, char **argv) {
    int pretty = 0, hash = 0, check = 0;
    if (argc == 3 && strcmp(argv[1], "--write-exclusive") == 0) {
        Buf in = read_all(stdin);
        return write_exclusive(argv[2], &in);
    }
    if (argc == 2 && strcmp(argv[1], "--check-lines") == 0) {
        /* each '\n'-terminated line (the last may lack it) must be one JSON
         * document; print the 1-based number of every line that is not */
        Buf in = read_all(stdin);
        size_t start = 0, line = 0;
        int bad = 0;
        while (start < in.n) {
            size_t end = start;
            while (end < in.n && in.b[end] != '\n') end++;
            line++;
            if (!valid_document(in.b + start, end + (end < in.n) - start)) {
                printf("%zu\n", line);
                bad = 1;
            }
            start = end + 1;
        }
        return bad;
    }
    if (argc == 2 && strcmp(argv[1], "--check") == 0) check = 1;
    else if (argc == 2 && strcmp(argv[1], "--pretty") == 0) pretty = 1;
    else if (argc == 2 && strcmp(argv[1], "--sha256") == 0) hash = 1;
    else if (argc != 1) {
        fprintf(stderr, "usage: json_canon [--check | --pretty | --sha256 | --write-exclusive PATH] < json\n");
        return 2;
    }
    Buf in = read_all(stdin);
    src = in.b ? in.b : "";
    srclen = in.n;
    pos = 0;
    JVal *v = parse_value(0);
    ws();
    if (pos != srclen) die("extra data");
    if (check) return 0;
    if (!pretty && lone_surrogate) {
        fprintf(stderr, "json_canon: lone surrogate cannot be encoded as UTF-8\n");
        return 1;
    }
    Buf out = {0};
    emit(&out, v, pretty ? 2 : 0, pretty, 0);
    if (hash) {
        uint8_t d[SHA256_DIGEST_SIZE];
        sha256_hash((const uint8_t *)out.b, out.n, d);
        for (int i = 0; i < SHA256_DIGEST_SIZE; i++) printf("%02x", d[i]);
        putchar('\n');
        return 0;
    }
    if (pretty) putc_(&out, '\n');
    if (fwrite(out.b, 1, out.n, stdout) != out.n || fflush(stdout) != 0) {
        fprintf(stderr, "json_canon: write error\n");
        return 1;
    }
    return 0;
}
