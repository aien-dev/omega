/*
 * glyph.c -- Glyph, a program language built for the AI writer (one-layer plan, after exp445).
 * Spec: docs/one_layer/GLYPH.md. Glyph lowers to the omega_emit op list (tools/omega_emit/SCHEMA.md),
 * which omega_emit turns into canonical OSC binary IR; the IR, checker2 and the signed receipt chain
 * are unchanged. Glyph only removes the bookkeeping the writer had to do: no register numbers, no
 * block numbers, no declared locals (every type is inferred), no punctuation.
 *
 *   glyph emit <in.glyph> <out.json>   exit 0 written; 1 refused (one line on stdout:
 *                                      "REFUSED glyph: line N: <reason>"); 2 usage / I/O
 *   glyph next <prefix.glyph>          the words that may come next after the prefix (the mask an
 *                                      engine applies while the model writes); exit 1 if the
 *                                      prefix is already invalid
 *   glyph words                        every fixed word of the language, one per line (the
 *                                      tokenizer gate checks each is one token)
 *
 * Nothing is repaired or guessed: the first defect wins.
 */
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IN (1u << 20)
#define MAX_WORDS 24
#define MAX_REGS 480
#define MAX_BLOCKS 512
#define MAX_FUNCS 32
#define MAX_PARAMS 6
#define MAX_NAMES 512
#define MAX_FRAMES 64
#define MAX_CONSTS 256
#define NAME_MAX_LEN 63

/* ---- types ---- */
enum { T_BOOL, T_U8, T_U16, T_U32, T_U64, T_I8, T_I16, T_I32, T_I64, T_NSCALAR };
enum { R_NONE, R_OWN, R_MUT, R_SHARED };
static const char *const GW[T_NSCALAR] = {"bool", "uchar", "ushort", "uint", "ulong", "byte", "short", "int", "long"};
static const char *const IRN[T_NSCALAR] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
static const int BITS[T_NSCALAR] = {1, 8, 16, 32, 64, 8, 16, 32, 64};
static const char *const REFW[] = {"", "own", "edit", "view"};
static const char *const REFIR[] = {"", "own", "mut", "shared"};

typedef struct { int ref; int s; int len; } Ty; /* ref R_NONE: scalar s; else array of s, len */

static int is_int(Ty t) { return t.ref == R_NONE && t.s != T_BOOL; }
static int is_signed(int s) { return s >= T_I8; }
static int teq(Ty a, Ty b) { return a.ref == b.ref && a.s == b.s && a.len == b.len; }
static const char *tname(Ty t) {
    static char buf[4][48];
    static int k;
    char *b = buf[k++ & 3];
    if (t.ref == R_NONE) snprintf(b, 48, "%s", GW[t.s]);
    else snprintf(b, 48, "%s %s %d", REFW[t.ref], GW[t.s], t.len);
    return b;
}
static const char *tir(Ty t) {
    static char buf[4][48];
    static int k;
    char *b = buf[k++ & 3];
    if (t.ref == R_NONE) snprintf(b, 48, "%s", IRN[t.s]);
    else snprintf(b, 48, "%s:%s:%d", REFIR[t.ref], IRN[t.s], t.len);
    return b;
}

/* ---- the fixed words ---- */
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR,
       OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NEG, OP_NOT, OP_GET, OP_NEW, OP_N };
static const char *const OPW[OP_N] = {"add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr",
                                      "eq", "ne", "lt", "le", "gt", "ge", "neg", "not", "get", "new"};
static const char *const KW[] = {"fn", "to", "void", "end", "else", "if", "while", "for", "break", "ret",
                                 "trap", "put", "free", "true", "false", "bound", "="};
#define NKW (sizeof KW / sizeof KW[0])

static int word_in(const char *w, const char *const *set, int n) {
    for (int i = 0; i < n; i++)
        if (!strcmp(w, set[i])) return i;
    return -1;
}
static int scalar_word(const char *w) { return word_in(w, GW, T_NSCALAR); }
static int ref_word(const char *w) { int r = word_in(w, REFW + 1, 3); return r < 0 ? -1 : r + 1; }
static int op_word(const char *w) { return word_in(w, OPW, OP_N); }
static int reserved(const char *w) {
    return scalar_word(w) >= 0 || ref_word(w) >= 0 || op_word(w) >= 0 || word_in(w, KW, (int)NKW) >= 0;
}

/* ---- refusal ---- */
static int g_line;
static char g_err[512];
static int fail(const char *fmt, ...) {
    va_list ap;
    int n = snprintf(g_err, sizeof g_err, "line %d: ", g_line);
    va_start(ap, fmt);
    vsnprintf(g_err + n, sizeof g_err - (size_t)n, fmt, ap);
    va_end(ap);
    return -1;
}
#define TRY(x) do { if ((x) < 0) return -1; } while (0)

/* ---- growable text ---- */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) { fputs("glyph: format error\n", stderr); exit(2); }
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = b->cap ? b->cap * 2 + (size_t)k : 256 + (size_t)k;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fputs("glyph: out of memory\n", stderr); exit(2); }
        if (b->n == 0) b->p[0] = 0;
    }
}

/* ---- program state ---- */
typedef struct { char name[NAME_MAX_LEN + 1]; int np; Ty p[MAX_PARAMS]; int has_ret; Ty ret; } Func;
typedef struct { char name[NAME_MAX_LEN + 1]; int reg; int depth; int fixed; int read; } Name; /* fixed: 1 loop counter, 2 parameter */
typedef struct { Ty t; int neg; uint64_t mag; int reg; } Const;
typedef struct { Buf ops; int nops; int order; int refd; } Block;
enum { F_FN, F_IF, F_WHILE, F_FOR };
typedef struct { int kind; int depth; int els, join, has_else; int head, exit; int ctr; } Frame;

static Func funcs[MAX_FUNCS];
static int nfuncs;
static Buf out;
static int in_fn; /* a function is open */
static Ty regs[MAX_REGS];
static int nregs, nparams;
static Name names[MAX_NAMES];
static int nnames, depth;
static Const consts[MAX_CONSTS];
static int nconsts;
static Block blocks[MAX_BLOCKS];
static int nblocks, norder, cur; /* cur: current block id, -1 when the path has ended */
static Frame frames[MAX_FRAMES];
static int nframes;

static int new_reg(Ty t) {
    if (nregs >= MAX_REGS) return fail("more than %d values in one function", MAX_REGS);
    regs[nregs] = t;
    return nregs++;
}
static int new_block(void) {
    if (nblocks >= MAX_BLOCKS) return fail("more than %d blocks in one function", MAX_BLOCKS);
    memset(&blocks[nblocks], 0, sizeof blocks[nblocks]);
    blocks[nblocks].order = -1;
    return nblocks++;
}
static void start_block(int b) { blocks[b].order = norder++; cur = b; }
static Name *find_name(const char *w) {
    for (int i = nnames - 1; i >= 0; i--)
        if (!strcmp(names[i].name, w)) return &names[i];
    return NULL;
}
static int find_func(const char *w) {
    for (int i = 0; i < nfuncs; i++)
        if (!strcmp(funcs[i].name, w)) return i;
    return -1;
}
/* a function this line may call: an earlier one (the one being written is not callable) */
static int find_callable(const char *w) {
    int g = find_func(w);
    return g >= 0 && !(in_fn && g == nfuncs - 1) ? g : -1;
}
static int valid_ident(const char *w) {
    if (!(isalpha((unsigned char)w[0]) || w[0] == '_') || strlen(w) > NAME_MAX_LEN) return 0;
    for (const char *c = w; *c; c++)
        if (!(isalnum((unsigned char)*c) || *c == '_')) return 0;
    return 1;
}
static int bind(const char *w, int reg, int fixed) {
    if (!valid_ident(w)) return fail("'%s' is not a name (a letter or _, then letters, digits or _)", w);
    if (reserved(w)) return fail("'%s' is a word of the language, not a name", w);
    if (find_func(w) >= 0) return fail("'%s' is a function name", w);
    if (nnames >= MAX_NAMES) return fail("more than %d names", MAX_NAMES);
    snprintf(names[nnames].name, sizeof names[nnames].name, "%s", w);
    names[nnames].reg = reg;
    names[nnames].depth = depth;
    names[nnames].fixed = fixed;
    names[nnames].read = 0;
    nnames++;
    return 0;
}
static int pop_scope(void) {
    for (; nnames > 0 && names[nnames - 1].depth > depth - 1; nnames--)
        if (!names[nnames - 1].fixed && !names[nnames - 1].read)
            return fail("'%s' is written but never read (a typo, or a value left unused)", names[nnames - 1].name);
    depth--;
    return 0;
}

/* ---- ops into the current block (block refs are written as @id@, numbered at the end) ---- */
static int op(const char *fmt, ...) {
    if (cur < 0) return fail("this line can never run: the path already ended (ret or break)");
    Block *b = &blocks[cur];
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    bput(&b->ops, "%s{%s}", b->nops ? ", " : "", tmp);
    b->nops++;
    return 0;
}
static int br(int to) { blocks[to].refd = 1; TRY(op("\"op\": \"br\", \"to\": @%d@", to)); cur = -1; return 0; }

/* ---- literals ---- */
static int parse_lit(const char *w, int *neg, uint64_t *mag) {
    const char *p = w;
    *neg = 0;
    if (*p == '-') { *neg = 1; p++; }
    if (!isdigit((unsigned char)*p)) return 0;
    if (p[0] == '0' && p[1]) return 0; /* no leading zeros */
    uint64_t v = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    *mag = v;
    return 1;
}
static int is_lit(const char *w) {
    int n;
    uint64_t m;
    return !strcmp(w, "true") || !strcmp(w, "false") || parse_lit(w, &n, &m);
}
/* the register holding literal w of type t (hoisted to the entry block, shared per value) */
static int lit_reg(const char *w, Ty t) {
    int neg = 0;
    uint64_t mag = 0;
    if (t.ref != R_NONE) return fail("'%s': a number cannot be an array", w);
    if (t.s == T_BOOL) {
        if (strcmp(w, "true") && strcmp(w, "false")) return fail("'%s' is not a bool (true or false)", w);
        mag = !strcmp(w, "true");
    } else {
        if (!parse_lit(w, &neg, &mag)) return fail("'%s' is not a whole number for %s", w, GW[t.s]);
        if (neg && !is_signed(t.s) && mag) return fail("%s cannot be negative (%s)", GW[t.s], w);
        int b = BITS[t.s] - (is_signed(t.s) ? 1 : 0);
        uint64_t lim = b == 64 ? UINT64_MAX : ((uint64_t)1 << b) - 1;
        if (neg ? (mag > lim + 1) : (mag > lim)) return fail("%s does not fit %s", w, GW[t.s]);
        if (neg && !mag) neg = 0;
    }
    for (int i = 0; i < nconsts; i++)
        if (teq(consts[i].t, t) && consts[i].neg == neg && consts[i].mag == mag) return consts[i].reg;
    if (nconsts >= MAX_CONSTS) return fail("more than %d distinct numbers in one function", MAX_CONSTS);
    int r = new_reg(t);
    TRY(r);
    consts[nconsts++] = (Const){t, neg, mag, r};
    return r;
}

/* an operand: a visible name, or a literal of type want (want NULL: a name is required) */
static int operand(const char *w, const Ty *want, Ty *got) {
    if (is_lit(w)) {
        if (!want) return fail("'%s': two numbers in one op; give one of them a name first", w);
        int r = lit_reg(w, *want);
        TRY(r);
        if (got) *got = *want;
        return r;
    }
    Name *n = find_name(w);
    if (!n) {
        if (find_func(w) >= 0) return fail("'%s' is a function; call it on its own line", w);
        return fail("'%s' is not a name in scope here", w);
    }
    n->read = 1;
    if (got) *got = regs[n->reg];
    return n->reg;
}

/* may a value of type a be passed for a parameter of type p (the IR's reference binding rule:
 * own from own, edit from own or edit, view from any array of the same element type and length) */
static int binds(Ty p, Ty a) {
    if (p.ref == R_NONE || a.ref == R_NONE) return teq(p, a);
    if (p.s != a.s || p.len != a.len) return 0;
    return p.ref == R_SHARED || (p.ref == R_MUT && a.ref != R_SHARED) || (p.ref == R_OWN && a.ref == R_OWN);
}
/* the argument registers of a call to f, checked against its parameters, as "r, r, ..." */
static int call_args(const Func *f, char **w, int n, Buf *a) {
    if (n != f->np) return fail("%s takes %d values, got %d", f->name, f->np, n);
    for (int i = 0; i < f->np; i++) {
        Ty ta;
        int r = operand(w[i], f->p[i].ref == R_NONE ? &f->p[i] : NULL, &ta);
        TRY(r);
        if (!binds(f->p[i], ta)) return fail("'%s' is %s; %s takes %s there", w[i], tname(ta), f->name, tname(f->p[i]));
        bput(a, "%s%d", i ? ", " : "", r);
    }
    return 0;
}

/* the type of the first name among operands (for typing literals) */
static int peer_type(char **w, int n, Ty *t) {
    for (int i = 0; i < n; i++) {
        if (is_lit(w[i])) continue;
        Name *nm = find_name(w[i]);
        if (nm) { *t = regs[nm->reg]; return 1; }
    }
    return 0;
}

/* parse a type at w[*i]: scalar word, or own|edit|view <scalar> <len> */
static int parse_type(char **w, int n, int *i, Ty *t) {
    if (*i >= n) return fail("a type is missing");
    int s = scalar_word(w[*i]);
    if (s >= 0) { *t = (Ty){R_NONE, s, 0}; (*i)++; return 0; }
    int r = ref_word(w[*i]);
    if (r < 0) return fail("'%s' is not a type (%s ..., or own/edit/view <type> <length>)", w[*i], GW[T_U32]);
    if (*i + 2 >= n) return fail("an array type is %s <element type> <length>", REFW[r]);
    s = scalar_word(w[*i + 1]);
    if (s < 0) return fail("'%s' is not an element type", w[*i + 1]);
    int neg;
    uint64_t len;
    if (!parse_lit(w[*i + 2], &neg, &len) || neg || len < 1 || len > 64) return fail("array length '%s' must be 1..64", w[*i + 2]);
    *t = (Ty){r, s, (int)len};
    *i += 3;
    return 0;
}

static const char *IROP[OP_N] = {"add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr",
                                 "eq", "ne", "lt", "le", "gt", "ge", "neg", "?", "load", "alloc"};

/*
 * expr: w[0..n) is one value. dst >= 0: write it there (its type must match); dst == -1: a fresh
 * register; dst == -2: if the value is a bare name or literal, use that register (no copy).
 * hint: the type wanted when the value is a bare literal (NULL if unknown). Returns the register.
 */
static int expr(char **w, int n, const Ty *hint, int dst, Ty *out_t) {
    Ty t, ta, tb;
    if (n == 0) return fail("a value is missing");
    int o = op_word(w[0]), s = scalar_word(w[0]), rf = ref_word(w[0]), g = find_callable(w[0]);
    if (g < 0 && find_func(w[0]) >= 0) return fail("%s cannot call itself (calls go to earlier functions only)", w[0]);
#define DST(ty) do {                                                                              \
        if (dst >= 0) { if (!teq(regs[dst], (ty))) return fail("this writes %s but the name holds %s", tname(ty), tname(regs[dst])); } \
        else { dst = new_reg(ty); TRY(dst); }                                                     \
        if (out_t) *out_t = (ty);                                                                 \
    } while (0)
    if (n == 1) { /* a bare name or literal */
        if (is_lit(w[0])) {
            if (!hint) return fail("'%s': the type of this number is unknown; write <type> %s", w[0], w[0]);
            int r = lit_reg(w[0], *hint);
            TRY(r);
            if (dst == -2) { if (out_t) *out_t = *hint; return r; }
            DST(*hint);
            TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, r));
            return dst;
        }
        int r = operand(w[0], NULL, &t);
        TRY(r);
        if (dst == -2) { if (out_t) *out_t = t; return r; }
        if (t.ref == R_OWN && dst == -1) {} /* moving an own array into a new name */
        DST(t);
        TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, r));
        return dst;
    }
    if (dst == -2) dst = -1;
    if (s >= 0) { /* <type> <literal>: a number;  <type> <name>: a conversion */
        if (n != 2) return fail("%s takes one value", GW[s]);
        t = (Ty){R_NONE, s, 0};
        if (is_lit(w[1])) {
            int r = lit_reg(w[1], t);
            TRY(r);
            DST(t);
            TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, r));
            return dst;
        }
        int a = operand(w[1], NULL, &ta);
        TRY(a);
        if (ta.ref != R_NONE) return fail("'%s' is an array; only numbers convert", w[1]);
        if (s == T_BOOL || ta.s == T_BOOL) {
            if (s != ta.s) return fail("bool does not convert to or from numbers; compare instead (ne x 0)");
            DST(t);
            TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, a));
            return dst;
        }
        DST(t);
        TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d", s == ta.s ? "mov" : "cast", dst, a));
        return dst;
    }
    if (rf >= 0) { /* own|edit|view <array name>: move or borrow */
        if (n != 2) return fail("%s takes one array name", REFW[rf]);
        int a = operand(w[1], NULL, &ta);
        TRY(a);
        if (ta.ref == R_NONE) return fail("'%s' is not an array", w[1]);
        if (rf == R_OWN && ta.ref != R_OWN) return fail("own moves an own array; '%s' is %s", w[1], tname(ta));
        if (rf == R_MUT && ta.ref == R_SHARED) return fail("edit needs an own or edit array; '%s' is view only", w[1]);
        t = (Ty){rf, ta.s, ta.len};
        DST(t);
        TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, a));
        return dst;
    }
    if (g >= 0) { /* a call */
        Func *f = &funcs[g];
        if (!f->has_ret) return fail("%s returns nothing; call it on its own line", f->name);
        Buf a = {0};
        if (call_args(f, w + 1, n - 1, &a) < 0) { free(a.p); return -1; }
        DST(f->ret);
        int rc = op("\"op\": \"call\", \"func\": \"%s\", \"args\": [%s], \"dst\": %d", f->name, a.p ? a.p : "", dst);
        free(a.p);
        TRY(rc);
        return dst;
    }
    if (o < 0) return fail("'%s' is not an op, a type or a function", w[0]);
    if (o == OP_NEG || o == OP_NOT) {
        if (n != 2) return fail("%s takes one value", OPW[o]);
        int a = operand(w[1], NULL, &ta);
        TRY(a);
        if (ta.ref != R_NONE) return fail("%s needs a number or bool", OPW[o]);
        if (o == OP_NEG && (ta.s == T_BOOL || !is_signed(ta.s))) return fail("neg needs a signed number; '%s' is %s", w[1], tname(ta));
        DST(ta);
        TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d", o == OP_NEG ? "neg" : (ta.s == T_BOOL ? "not" : "bnot"), dst, a));
        return dst;
    }
    if (o == OP_GET) { /* get <array> <index> */
        if (n != 3) return fail("get takes an array and an index");
        int a = operand(w[1], NULL, &ta);
        TRY(a);
        if (ta.ref == R_NONE) return fail("'%s' is not an array", w[1]);
        Ty it = {R_NONE, T_U32, 0};
        int i = operand(w[2], &it, &tb);
        TRY(i);
        if (!is_int(tb)) return fail("index '%s' is not a whole number", w[2]);
        t = (Ty){R_NONE, ta.s, 0};
        DST(t);
        TRY(op("\"op\": \"load\", \"dst\": %d, \"arr\": %d, \"index\": %d", dst, a, i));
        return dst;
    }
    if (o == OP_NEW) { /* new <type> <length> <init> */
        int i = 1;
        Ty et;
        if (n != 4) return fail("new takes a type, a length and a starting value");
        if (scalar_word(w[1]) < 0) return fail("'%s' is not an element type", w[1]);
        et = (Ty){R_NONE, scalar_word(w[1]), 0};
        i = 2;
        int neg;
        uint64_t len;
        if (!parse_lit(w[i], &neg, &len) || neg || len < 1 || len > 64) return fail("array length '%s' must be 1..64", w[i]);
        int init = operand(w[3], &et, &tb);
        TRY(init);
        if (!teq(tb, et)) return fail("starting value '%s' is %s, not %s", w[3], tname(tb), tname(et));
        t = (Ty){R_OWN, et.s, (int)len};
        DST(t);
        TRY(op("\"op\": \"alloc\", \"dst\": %d, \"init\": %d", dst, init));
        return dst;
    }
    /* binary ops */
    if (n != 3) return fail("%s takes two values", OPW[o]);
    Ty peer;
    int have = peer_type(w + 1, 2, &peer);
    if (!have) return fail("%s %s %s: two numbers; give one of them a name first", OPW[o], w[1], w[2]);
    int a, b;
    if (o == OP_SHL || o == OP_SHR) { /* a number takes the type of the other value, as in every op */
        Ty at;
        a = operand(w[1], &peer, &at);
        TRY(a);
        b = operand(w[2], &at, &tb);
        TRY(b);
        if (!is_int(at) || !is_int(tb)) return fail("shifts need whole numbers");
        DST(at);
        TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d, \"b\": %d", IROP[o], dst, a, b));
        return dst;
    }
    a = operand(w[1], &peer, &ta);
    TRY(a);
    b = operand(w[2], &peer, &tb);
    TRY(b);
    if (!teq(ta, tb)) return fail("%s needs two values of one type: '%s' is %s, '%s' is %s", OPW[o], w[1], tname(ta), w[2], tname(tb));
    if (ta.ref != R_NONE) return fail("%s works on numbers, not arrays", OPW[o]);
    if (o >= OP_EQ && o <= OP_GE) {
        t = (Ty){R_NONE, T_BOOL, 0};
        DST(t);
    } else {
        if (o <= OP_REM && ta.s == T_BOOL) return fail("%s needs whole numbers, not bool", OPW[o]);
        DST(ta);
    }
    TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d, \"b\": %d", IROP[o], dst, a, b));
    return dst;
#undef DST
}

/* a condition: a bool name, true/false, or one op that gives a bool */
static int cond(char **w, int n) {
    Ty b = {R_NONE, T_BOOL, 0}, t;
    int r = expr(w, n, &b, -2, &t);
    TRY(r);
    if (!teq(t, b)) return fail("a condition must be a bool; this is %s", tname(t));
    return r;
}

static Frame *push(int kind) {
    if (nframes >= MAX_FRAMES) { fail("more than %d nested blocks", MAX_FRAMES); return NULL; }
    Frame *f = &frames[nframes++];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    depth++;
    f->depth = depth;
    return f;
}

static void end_function(void) {
    /* number blocks by start order, hoist the literals into the entry block, write the function */
    Func *f = &funcs[nfuncs - 1];
    int map[MAX_BLOCKS], ord[MAX_BLOCKS];
    for (int i = 0; i < nblocks; i++) { map[i] = blocks[i].order; if (blocks[i].order >= 0) ord[blocks[i].order] = i; }
    bput(&out, "%s\n  {\"name\": \"%s\", \"params\": [", nfuncs > 1 ? "," : "", f->name);
    for (int i = 0; i < f->np; i++) bput(&out, "%s\"%s\"", i ? ", " : "", tir(f->p[i]));
    bput(&out, "], \"ret\": \"%s\", \"locals\": [", f->has_ret ? tir(f->ret) : "void");
    for (int i = nparams; i < nregs; i++) bput(&out, "%s\"%s\"", i > nparams ? ", " : "", tir(regs[i]));
    bput(&out, "],\n   \"blocks\": [");
    for (int k = 0; k < norder; k++) {
        Block *b = &blocks[ord[k]];
        bput(&out, "%s\n    [", k ? "," : "");
        int first = 1;
        if (k == 0)
            for (int c = 0; c < nconsts; c++) {
                Const *q = &consts[c];
                if (q->t.s == T_BOOL) bput(&out, "%s{\"op\": \"const\", \"dst\": %d, \"value\": %s}", first ? "" : ", ", q->reg, q->mag ? "true" : "false");
                else bput(&out, "%s{\"op\": \"const\", \"dst\": %d, \"value\": %s%" PRIu64 "}", first ? "" : ", ", q->reg, q->neg ? "-" : "", q->mag);
                first = 0;
            }
        /* copy ops, replacing @id@ with the block's number */
        const char *p = b->ops.p ? b->ops.p : "";
        if (*p && !first) bput(&out, ", ");
        while (*p) {
            if (*p == '@') {
                int id = atoi(p + 1);
                bput(&out, "%d", map[id]);
                p = strchr(p + 1, '@') + 1;
            } else { bput(&out, "%c", *p); p++; }
        }
        bput(&out, "]");
        free(b->ops.p);
    }
    bput(&out, "\n   ]}");
}

static int stmt(char **w, int n) {
    if (!strcmp(w[0], "fn")) {
        if (in_fn) return fail("fn inside a function: close the function with end first");
        if (nfuncs >= MAX_FUNCS) return fail("more than %d functions", MAX_FUNCS);
        if (n < 3) return fail("fn <name> <param> <type> ... to <type or void>");
        if (!valid_ident(w[1]) || reserved(w[1])) return fail("'%s' cannot be a function name", w[1]);
        if (find_func(w[1]) >= 0) return fail("function '%s' is already defined", w[1]);
        Func *f = &funcs[nfuncs];
        memset(f, 0, sizeof *f);
        snprintf(f->name, sizeof f->name, "%s", w[1]);
        nregs = nnames = nconsts = nblocks = norder = nframes = 0;
        depth = 0;
        Frame *fr = push(F_FN);
        if (!fr) return -1;
        int i = 2;
        while (i < n && strcmp(w[i], "to")) {
            if (f->np >= MAX_PARAMS) return fail("more than %d parameters", MAX_PARAMS);
            const char *pn = w[i++];
            Ty t;
            TRY(parse_type(w, n, &i, &t));
            f->p[f->np++] = t;
            int r = new_reg(t);
            TRY(r);
            if (find_name(pn)) return fail("parameter '%s' is named twice", pn);
            TRY(bind(pn, r, 2));
        }
        if (i >= n) return fail("fn needs 'to <type>' or 'to void' at the end");
        i++;
        if (i < n && !strcmp(w[i], "void")) { f->has_ret = 0; i++; }
        else {
            Ty t;
            TRY(parse_type(w, n, &i, &t));
            if (t.ref != R_NONE) return fail("a function returns a number or bool, not an array");
            f->has_ret = 1;
            f->ret = t;
        }
        if (i != n) return fail("'%s' after the return type", w[i]);
        nparams = nregs;
        nfuncs++;
        in_fn = 1;
        int b = new_block();
        TRY(b);
        start_block(b);
        return 0;
    }
    if (!in_fn) return fail("'%s' outside a function (start one with fn)", w[0]);
    Func *f = &funcs[nfuncs - 1];
    if (!strcmp(w[0], "end")) {
        if (n != 1) return fail("end stands alone");
        Frame *fr = &frames[nframes - 1];
        switch (fr->kind) {
        case F_IF:
            if (!fr->has_else) { if (cur >= 0) TRY(br(fr->els)); start_block(fr->els); }
            else {
                if (cur >= 0) { if (fr->join < 0) { fr->join = new_block(); TRY(fr->join); } TRY(br(fr->join)); }
                if (fr->join >= 0) start_block(fr->join); else cur = -1;
            }
            break;
        case F_WHILE:
            if (cur >= 0) TRY(br(fr->head));
            start_block(fr->exit);
            break;
        case F_FOR: {
            if (cur >= 0) {
                Ty t = regs[fr->ctr];
                int one = lit_reg("1", t);
                TRY(one);
                TRY(op("\"op\": \"add\", \"dst\": %d, \"a\": %d, \"b\": %d", fr->ctr, fr->ctr, one));
                TRY(br(fr->head));
            }
            start_block(fr->exit);
            break;
        }
        case F_FN:
            if (cur >= 0) {
                if (f->has_ret) return fail("%s can reach its end without ret", f->name);
                TRY(op("\"op\": \"ret\""));
                cur = -1;
            }
            end_function();
            in_fn = 0;
            break;
        }
        nframes--;
        TRY(pop_scope());
        return 0;
    }
    if (!strcmp(w[0], "else")) {
        if (n != 1) return fail("else stands alone");
        Frame *fr = &frames[nframes - 1];
        if (fr->kind != F_IF || fr->has_else) return fail("else without an open if");
        if (cur >= 0) { fr->join = new_block(); TRY(fr->join); TRY(br(fr->join)); }
        else fr->join = -1;
        fr->has_else = 1;
        TRY(pop_scope());
        depth++;
        start_block(fr->els);
        return 0;
    }
    if (!strcmp(w[0], "if")) {
        if (n < 2) return fail("if needs a condition");
        int c = cond(w + 1, n - 1);
        TRY(c);
        int th = new_block(), el = new_block();
        TRY(th);
        TRY(el);
        blocks[th].refd = blocks[el].refd = 1;
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", c, th, el));
        Frame *fr = push(F_IF);
        if (!fr) return -1;
        fr->els = el;
        fr->join = -1;
        start_block(th);
        return 0;
    }
    /*
     * Loops lower to the shape checker2 proves terminating (src/checker2/c2_check.c "loop bounds",
     * the same shape oscc emits for OSC `while ... bound N`): a counter written in the loop only by
     * ADD c, one; a guard block dominating every latch that compares the counter with a CONST
     * K <= 1,000,000 defined once (literals are hoisted and shared, so each is defined once).
     *   while <cond> [bound N]: hidden ulong counter; head: cond -> guard | exit;
     *     guard: ge cnt K -> trap 4 (loop bound) | body; body starts with cnt = add cnt 1.
     *   for <name> <type> <count number>: head: lt i K -> body | exit; latch: i = add i 1.
     *   for <name> <type> <count name>: as while (lt i count) with the hidden guard.
     */
    if (!strcmp(w[0], "while") || (!strcmp(w[0], "for") && n == 4 && !is_lit(w[3]))) {
        int is_for = !strcmp(w[0], "for");
        if (cur < 0) return fail("this line can never run: the path already ended (ret or break)");
        const char *bound = "1000000";
        int cn = n;
        if (!is_for && n >= 4 && !strcmp(w[n - 2], "bound")) { bound = w[n - 1]; cn = n - 2; }
        if (!is_for && cn < 2) return fail("while needs a condition");
        int neg;
        uint64_t bv;
        if (!parse_lit(bound, &neg, &bv) || neg || bv < 1 || bv > 1000000) return fail("bound '%s' must be 1..1000000", bound);
        Ty u64 = {R_NONE, T_U64, 0};
        int zero = lit_reg("0", u64), lim = lit_reg(bound, u64), one = lit_reg("1", u64);
        TRY(zero);
        TRY(lim);
        TRY(one);
        int ctr = -1, cnt = new_reg(u64), lim_t = -1;
        TRY(cnt);
        if (is_for) { /* for <name> <type> <count name> */
            int s = scalar_word(w[2]);
            if (s < 0 || s == T_BOOL) return fail("'%s' is not a whole-number type", w[2]);
            Ty t = {R_NONE, s, 0}, tc;
            lim_t = operand(w[3], NULL, &tc);
            TRY(lim_t);
            if (!teq(tc, t)) return fail("count '%s' is %s, not %s", w[3], tname(tc), tname(t));
            int z = lit_reg("0", t);
            TRY(z);
            TRY(lit_reg("1", t));
            ctr = new_reg(t);
            TRY(ctr);
            TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", ctr, z));
        }
        TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", cnt, zero));
        int h = new_block(), g = new_block(), tb = new_block(), body = new_block(), ex = new_block();
        TRY(h);
        TRY(g);
        TRY(tb);
        TRY(body);
        TRY(ex);
        TRY(br(h));
        start_block(h);
        int c;
        if (is_for) {
            c = new_reg((Ty){R_NONE, T_BOOL, 0});
            TRY(c);
            TRY(op("\"op\": \"lt\", \"dst\": %d, \"a\": %d, \"b\": %d", c, ctr, lim_t));
        } else {
            c = cond(w + 1, cn - 1);
            TRY(c);
        }
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", c, g, ex));
        start_block(g);
        int ge = new_reg((Ty){R_NONE, T_BOOL, 0});
        TRY(ge);
        TRY(op("\"op\": \"ge\", \"dst\": %d, \"a\": %d, \"b\": %d", ge, cnt, lim));
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", ge, tb, body));
        start_block(tb);
        TRY(op("\"op\": \"trap\", \"code\": 4"));
        TRY(br(ex));
        start_block(body);
        TRY(op("\"op\": \"add\", \"dst\": %d, \"a\": %d, \"b\": %d", cnt, cnt, one));
        Frame *fr = push(is_for ? F_FOR : F_WHILE);
        if (!fr) return -1;
        fr->head = h;
        fr->exit = ex;
        fr->ctr = ctr;
        if (is_for) TRY(bind(w[1], ctr, 1));
        return 0;
    }
    if (!strcmp(w[0], "for")) { /* for <name> <type> <count number>: name = 0, 1, ... count-1 */
        if (n != 4) return fail("for <name> <type> <count>");
        int s = scalar_word(w[2]);
        if (s < 0 || s == T_BOOL) return fail("'%s' is not a whole-number type", w[2]);
        Ty t = {R_NONE, s, 0};
        int neg;
        uint64_t kv;
        if (!parse_lit(w[3], &neg, &kv) || neg || kv > 1000000) return fail("count '%s' must be 0..1000000", w[3]);
        int zero = lit_reg("0", t);
        TRY(zero);
        int k = lit_reg(w[3], t);
        TRY(k);
        TRY(lit_reg("1", t));
        int ctr = new_reg(t);
        TRY(ctr);
        TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", ctr, zero));
        int h = new_block(), body = new_block(), ex = new_block();
        TRY(h);
        TRY(body);
        TRY(ex);
        TRY(br(h));
        start_block(h);
        int c = new_reg((Ty){R_NONE, T_BOOL, 0});
        TRY(c);
        TRY(op("\"op\": \"lt\", \"dst\": %d, \"a\": %d, \"b\": %d", c, ctr, k));
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", c, body, ex));
        Frame *fr = push(F_FOR);
        if (!fr) return -1;
        fr->head = h;
        fr->exit = ex;
        fr->ctr = ctr;
        TRY(bind(w[1], ctr, 1));
        start_block(body);
        return 0;
    }
    if (!strcmp(w[0], "break")) {
        if (n != 1) return fail("break stands alone");
        for (int k = nframes - 1; k >= 0; k--)
            if (frames[k].kind == F_WHILE || frames[k].kind == F_FOR) return br(frames[k].exit);
        return fail("break outside a loop");
    }
    if (!strcmp(w[0], "ret")) {
        if (!f->has_ret) {
            if (n != 1) return fail("%s returns nothing: ret stands alone", f->name);
            TRY(op("\"op\": \"ret\""));
            cur = -1;
            return 0;
        }
        if (n < 2) return fail("%s returns %s: ret needs a value", f->name, tname(f->ret));
        Ty t;
        int r = expr(w + 1, n - 1, &f->ret, -2, &t);
        TRY(r);
        if (!teq(t, f->ret)) return fail("this is %s; %s returns %s", tname(t), f->name, tname(f->ret));
        TRY(op("\"op\": \"ret\", \"a\": %d", r));
        cur = -1;
        return 0;
    }
    if (!strcmp(w[0], "trap")) {
        int neg;
        uint64_t c;
        if (n != 2 || !parse_lit(w[1], &neg, &c) || neg || c < 1 || c > 14) return fail("trap <code 1..14>");
        return op("\"op\": \"trap\", \"code\": %" PRIu64, c);
    }
    if (!strcmp(w[0], "put")) { /* put <array> <index> <value> */
        if (n != 4) return fail("put takes an array, an index and a value");
        Ty ta, tb, tv;
        int a = operand(w[1], NULL, &ta);
        TRY(a);
        if (ta.ref == R_NONE) return fail("'%s' is not an array", w[1]);
        if (ta.ref == R_SHARED) return fail("'%s' is view only; put needs an own or edit array", w[1]);
        Ty it = {R_NONE, T_U32, 0}, et = {R_NONE, ta.s, 0};
        int i = operand(w[2], &it, &tb);
        TRY(i);
        if (!is_int(tb)) return fail("index '%s' is not a whole number", w[2]);
        int v = operand(w[3], &et, &tv);
        TRY(v);
        if (!teq(tv, et)) return fail("'%s' is %s; the array holds %s", w[3], tname(tv), tname(et));
        return op("\"op\": \"store\", \"arr\": %d, \"index\": %d, \"value\": %d", a, i, v);
    }
    if (!strcmp(w[0], "free")) {
        if (n != 2) return fail("free takes one own array");
        Ty ta;
        int a = operand(w[1], NULL, &ta);
        TRY(a);
        if (ta.ref != R_OWN) return fail("'%s' is not an own array", w[1]);
        return op("\"op\": \"release\", \"arr\": %d", a);
    }
    if (find_func(w[0]) >= 0 && find_callable(w[0]) < 0) return fail("%s cannot call itself (calls go to earlier functions only)", w[0]);
    int g = find_callable(w[0]);
    if (g >= 0) { /* a call on its own line */
        Func *cf = &funcs[g];
        if (cf->has_ret) return fail("%s returns %s; keep it: <name> = %s ...", cf->name, tname(cf->ret), cf->name);
        Buf a = {0};
        if (call_args(cf, w + 1, n - 1, &a) < 0) { free(a.p); return -1; }
        int rc = op("\"op\": \"call\", \"func\": \"%s\", \"args\": [%s]", cf->name, a.p ? a.p : "");
        free(a.p);
        return rc;
    }
    if (n >= 2 && strcmp(w[1], "=")) { /* <name> <value>: the short form, "=" left out */
        for (int i = n; i > 1; i--) w[i] = w[i - 1];
        static char eqs[] = "=";
        w[1] = eqs;
        n++;
    }
    if (n >= 3 && !strcmp(w[1], "=")) { /* <name> = <value> */
        Name *nm = find_name(w[0]);
        if (nm) {
            if (nm->fixed == 1) return fail("'%s' is the loop counter; it cannot be assigned", w[0]);
            Ty t;
            int r = expr(w + 2, n - 2, &regs[nm->reg], nm->reg, &t);
            TRY(r);
            return 0;
        }
        if (find_func(w[0]) >= 0 || reserved(w[0]) || !valid_ident(w[0])) return fail("'%s' cannot be a name", w[0]);
        Ty t;
        int r = expr(w + 2, n - 2, NULL, -1, &t);
        TRY(r);
        return bind(w[0], r, 0);
    }
    return fail("'%s' does not start a line (a name =, if, while, for, ret, put, free, trap, break, else, end, or a call)", w[0]);
}

/* split one line into words; returns the count, or -1 on a bad character */
static int split(char *s, char **w) {
    int n = 0;
    for (char *p = s; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if (n >= MAX_WORDS) return fail("more than %d words on one line", MAX_WORDS);
        w[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') {
            if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '=')) return fail("character '%c' is not part of Glyph", *p);
            p++;
        }
        if (*p) *p++ = 0;
    }
    return n;
}

static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(2); }
    char *b = malloc(MAX_IN + 1);
    if (!b) exit(2);
    size_t n = fread(b, 1, MAX_IN + 1, fp);
    fclose(fp);
    if (n > MAX_IN) { fprintf(stderr, "%s: larger than %u bytes\n", path, MAX_IN); exit(2); }
    b[n] = 0;
    if (strlen(b) != n) { printf("REFUSED glyph: a zero byte in the input\n"); exit(1); }
    *len = n;
    return b;
}

/* run every complete line of src; returns 0, or -1 with g_err set. last_partial: the unfinished
 * last line (no newline) is left unparsed and returned through *tail */
static int run(char *src, int keep_tail, char **tail) {
    char *p = src;
    g_line = 0;
    out.n = 0;
    bput(&out, "{\"functions\": [");
    for (;;) {
        char *nl = strchr(p, '\n');
        if (!nl && keep_tail) { *tail = p; return 0; }
        if (nl) *nl = 0;
        g_line++;
        char *w[MAX_WORDS + 1];
        int n = split(p, w);
        TRY(n);
        if (n > 0) TRY(stmt(w, n));
        if (!nl) break;
        p = nl + 1;
    }
    g_line++;
    if (in_fn) return fail("the input ends inside function '%s' (missing end)", funcs[nfuncs - 1].name);
    if (nfuncs == 0) return fail("no function");
    bput(&out, "\n]}\n");
    return 0;
}

/* glyph next: which words may start or continue the unfinished line (coarse classes plus names) */
static void list_names(const char *label, int want_kind, Ty want) {
    /* want_kind 0: any; 1: scalars of type want; 2: arrays; 3: bools */
    printf("%s:", label);
    for (int i = 0; i < nnames; i++) {
        Ty t = regs[names[i].reg];
        int ok = want_kind == 0 || (want_kind == 1 && teq(t, want)) || (want_kind == 2 && t.ref != R_NONE) ||
                 (want_kind == 3 && t.ref == R_NONE && t.s == T_BOOL);
        int shadowed = 0;
        for (int j = i + 1; j < nnames; j++) if (!strcmp(names[j].name, names[i].name)) shadowed = 1;
        if (ok && !shadowed) printf(" %s", names[i].name);
    }
    printf("\n");
}
static int cmd_next(const char *path) {
    size_t len;
    char *src = slurp(path, &len), *tail = NULL;
    if (run(src, 1, &tail) < 0) { printf("REFUSED glyph: %s\n", g_err); return 1; }
    char *w[MAX_WORDS + 1];
    int n = split(tail, w);
    if (n < 0) { printf("REFUSED glyph: %s\n", g_err); return 1; }
    if (!in_fn) { printf(n == 0 ? "words: fn\n" : "words: (finish the fn line)\n"); return 0; }
    Func *f = &funcs[nfuncs - 1];
    if (n == 0 && cur < 0) { /* the path ended (ret or break): only closing words may follow */
        Frame *fr = &frames[nframes - 1];
        printf("words:%s end\n", fr->kind == F_IF && !fr->has_else ? " else" : "");
        return 0;
    }
    if (n == 0) {
        printf("words: if while for ret put free trap");
        int loop = 0;
        for (int k = 0; k < nframes; k++) if (frames[k].kind == F_WHILE || frames[k].kind == F_FOR) loop = 1;
        if (loop) printf(" break");
        if (frames[nframes - 1].kind == F_IF && !frames[nframes - 1].has_else) printf(" else");
        printf(" end");
        for (int i = 0; i < nfuncs - 1; i++) if (!funcs[i].has_ret) printf(" %s", funcs[i].name);
        printf("\n");
        list_names("assign", 0, (Ty){0});
        printf("new-name: any name not in use\n");
        return 0;
    }
    if (n == 1 && !strcmp(w[0], "ret")) {
        if (!f->has_ret) { printf("words: (newline)\n"); return 0; }
        list_names("names", 1, f->ret);
        printf("number: %s\nops: any op giving %s\n", tname(f->ret), tname(f->ret));
        return 0;
    }
    if (n == 1 && (!strcmp(w[0], "if") || !strcmp(w[0], "while"))) {
        list_names("names", 3, (Ty){0});
        printf("words: true false eq ne lt le gt ge not\n");
        return 0;
    }
    /* after "<op> <a>" for a binary op: the second operand must match a's type */
    if (n >= 2) {
        int o = op_word(w[n - 2]);
        Name *a = find_name(w[n - 1]);
        if (o >= 0 && o < OP_NEG && o != OP_SHL && o != OP_SHR && a) {
            list_names("names", 1, regs[a->reg]);
            printf("number: %s\n", tname(regs[a->reg]));
            return 0;
        }
    }
    printf("words: (any word of the line form; see GLYPH.md)\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "words")) {
        for (size_t i = 0; i < NKW; i++) if (strcmp(KW[i], "=")) printf("%s\n", KW[i]);
        for (int i = 0; i < OP_N; i++) printf("%s\n", OPW[i]);
        for (int i = 0; i < T_NSCALAR; i++) printf("%s\n", GW[i]);
        for (int i = 1; i <= 3; i++) printf("%s\n", REFW[i]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "next")) return cmd_next(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "emit")) {
        size_t len;
        char *src = slurp(argv[2], &len), *tail = NULL;
        if (run(src, 0, &tail) < 0) { printf("REFUSED glyph: %s\n", g_err); free(src); return 1; }
        free(src);
        FILE *fp = fopen(argv[3], "wb");
        if (!fp || fwrite(out.p, 1, out.n, fp) != out.n || fclose(fp)) { perror(argv[3]); return 2; }
        return 0;
    }
    fprintf(stderr, "usage: glyph emit <in.glyph> <out.json> | glyph next <prefix.glyph> | glyph words\n");
    return 2;
}
