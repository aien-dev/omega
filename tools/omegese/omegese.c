/*
 * omegese.c -- Omegese, a program language built for the AI writer (one-layer plan, after exp445).
 * Spec: docs/one_layer/OMEGESE.md. Omegese lowers to the omega_emit op list (tools/omega_emit/SCHEMA.md),
 * which omega_emit turns into canonical OSC binary IR; the IR, checker2 and the signed receipt chain
 * are unchanged. Omegese only removes the bookkeeping the writer had to do: no register numbers, no
 * block numbers, no declared locals (every type is inferred), no punctuation besides optional ( ).
 *
 * Values are prefix expressions: every op has a fixed number of values after it, so `add mul r r 4`
 * is add(mul(r, r), 4) with no brackets. They lower into fresh hidden registers, innermost first, left
 * to right.
 *
 *   omegese emit <in.omegese> <out.json>   exit 0 written; 1 refused (one line on stdout:
 *                                      "REFUSED omegese: line N: <reason>"); 2 usage / I/O
 *   omegese next <prefix.omegese>          the words that may come next after the prefix (the mask an
 *                                      engine applies while the model writes); exit 1 if the
 *                                      prefix is already invalid
 *   omegese words                        every fixed word of the language, one per line (the
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
#define MAX_WORDS 96
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
/* sets of scalar types, one bit per type */
#define S_ALL 0x1FFu
#define S_INT 0x1FEu
#define S_SIGNED 0x1E0u

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
static int is_shift(int o) { return o == OP_SHL || o == OP_SHR; }
static int is_cmp(int o) { return o >= OP_EQ && o <= OP_GE; }
static int is_arith(int o) { return o <= OP_REM; } /* whole numbers only */

static int word_in(const char *w, const char *const *set, int n) {
    for (int i = 0; i < n; i++)
        if (!strcmp(w, set[i])) return i;
    return -1;
}
/* a scalar type: the one-token Omegese word, or the IR name the task signature uses (u8 .. i64) */
static int scalar_word(const char *w) { int s = word_in(w, GW, T_NSCALAR); return s >= 0 ? s : word_in(w, IRN, T_NSCALAR); }
static int ref_word(const char *w) { int r = word_in(w, REFW + 1, 3); return r < 0 ? -1 : r + 1; }
static int op_word(const char *w) { return word_in(w, OPW, OP_N); }
static int reserved(const char *w) {
    return scalar_word(w) >= 0 || ref_word(w) >= 0 || op_word(w) >= 0 || word_in(w, KW, (int)NKW) >= 0;
}

/* ---- refusal ---- */
#define NEED (-2)    /* next: the prefix ended here; g_next holds the words that may follow */
#define MISSING (-3) /* value: no value starts here (end of line, ')' or a keyword); the caller words it */
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
#define TRY(x) do { int try_ = (x); if (try_ < 0) return try_; } while (0)

/* ---- growable text ---- */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) { fputs("omegese: format error\n", stderr); exit(2); }
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = b->cap ? b->cap * 2 + (size_t)k : 256 + (size_t)k;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fputs("omegese: out of memory\n", stderr); exit(2); }
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

/* the line being read: its words, the read position, and the next mode (partial: the line is an
 * unfinished prefix; reaching its end returns NEED with the allowed words in g_next) */
static char **lw;
static int ln, lp, partial, pdepth;
static Buf g_next;

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
/* may w become a new name here */
static int check_new_name(const char *w) {
    if (!valid_ident(w)) return fail("'%s' is not a name (a letter or _, then letters, digits or _)", w);
    if (reserved(w)) return fail("'%s' is a word of the language, not a name", w);
    if (find_func(w) >= 0) return fail("'%s' is a function name", w);
    if (find_name(w)) return fail("'%s' is already a name here; pick a new name", w);
    return 0;
}
static int bind(const char *w, int reg, int fixed) {
    TRY(check_new_name(w));
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
    if (cur < 0) return fail("this line can never run: the path already ended (ret, trap or break)");
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
/* literal w as scalar type s: 0 and its value, or why not (1 not a bool, 2 not a whole number,
 * 3 negative for an unsigned type, 4 does not fit) */
static int lit_conv(const char *w, int s, int *neg, uint64_t *mag) {
    *neg = 0;
    *mag = 0;
    if (s == T_BOOL) {
        if (strcmp(w, "true") && strcmp(w, "false")) return 1;
        *mag = !strcmp(w, "true");
        return 0;
    }
    if (!parse_lit(w, neg, mag)) return 2;
    if (*neg && !is_signed(s) && *mag) return 3;
    int b = BITS[s] - (is_signed(s) ? 1 : 0);
    uint64_t lim = b == 64 ? UINT64_MAX : ((uint64_t)1 << b) - 1;
    if (*neg ? (*mag > lim + 1) : (*mag > lim)) return 4;
    if (*neg && !*mag) *neg = 0;
    return 0;
}
/* the scalar types literal w fits, out of the set m */
static unsigned lit_fits(const char *w, unsigned m) {
    unsigned r = 0;
    int neg;
    uint64_t mag;
    for (int s = 0; s < T_NSCALAR; s++)
        if ((m >> s & 1) && !lit_conv(w, s, &neg, &mag)) r |= 1u << s;
    return r;
}
/* the register holding literal w of type t (hoisted to the entry block, shared per value) */
static int lit_reg(const char *w, Ty t) {
    int neg = 0;
    uint64_t mag = 0;
    if (t.ref != R_NONE) return fail("'%s': a number cannot be an array", w);
    switch (lit_conv(w, t.s, &neg, &mag)) {
    case 1: return fail("'%s' is not a bool (true or false)", w);
    case 2: return fail("'%s' is not a whole number for %s", w, GW[t.s]);
    case 3: return fail("%s cannot be negative (%s)", GW[t.s], w);
    case 4: return fail("%s does not fit %s", w, GW[t.s]);
    }
    for (int i = 0; i < nconsts; i++)
        if (teq(consts[i].t, t) && consts[i].neg == neg && consts[i].mag == mag) return consts[i].reg;
    if (nconsts >= MAX_CONSTS) return fail("more than %d distinct numbers in one function", MAX_CONSTS);
    int r = new_reg(t);
    TRY(r);
    consts[nconsts++] = (Const){t, neg, mag, r};
    return r;
}

/* may a value of type a be passed for a parameter of type p (the IR's reference binding rule:
 * own from own, edit from own or edit, view from any array of the same element type and length) */
static int binds(Ty p, Ty a) {
    if (p.ref == R_NONE || a.ref == R_NONE) return teq(p, a);
    if (p.s != a.s || p.len != a.len) return 0;
    return p.ref == R_SHARED || (p.ref == R_MUT && a.ref != R_SHARED) || (p.ref == R_OWN && a.ref == R_OWN);
}
/* may own|edit|view (rf) be applied to an array of type a */
static int ref_ok(int rf, Ty a) { return a.ref != R_NONE && (rf == R_SHARED || (rf == R_MUT && a.ref != R_SHARED) || a.ref == R_OWN); }

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

/* ---- what a value slot accepts (used to type a bare number and for `omegese next`) ---- */
enum { L_NO, L_TYPED, L_OPEN };      /* bare number: refused; typed lt; typed later by the other value */
enum { A_NONE, A_ANY, A_EXACT, A_BIND }; /* arrays: none; any; exactly p; any that binds to parameter p */
typedef struct { unsigned scal; int arr; Ty p; int lit; Ty lt; int top; } Want; /* top: new allowed */

static Want want_type(Ty t) {
    Want w = {0};
    if (t.ref == R_NONE) { w.scal = 1u << t.s; w.lit = L_TYPED; w.lt = t; }
    else { w.arr = A_EXACT; w.p = t; }
    return w;
}
static Want want_param(Ty p) {
    if (p.ref == R_NONE) return want_type(p);
    Want w = {0};
    w.arr = A_BIND;
    w.p = p;
    return w;
}
static int want_ok(const Want *w, Ty t) {
    if (t.ref == R_NONE) return w->scal >> t.s & 1;
    return w->arr == A_ANY || (w->arr == A_EXACT && teq(w->p, t)) || (w->arr == A_BIND && binds(w->p, t));
}

/* ---- the words of a line ---- */
/* a word that ends a value: ')' or a keyword (true and false are values) */
static int is_term(const char *t) {
    return !strcmp(t, ")") || (word_in(t, KW, (int)NKW) >= 0 && strcmp(t, "true") && strcmp(t, "false"));
}
/* words a..b of the line, joined by spaces (for messages) */
static const char *span(int a, int b) {
    static char buf[4][80];
    static int k;
    char *s = buf[k++ & 3];
    size_t n = 0;
    s[0] = 0;
    for (int i = a; i < b && i < ln && n < sizeof buf[0] - 1; i++) {
        int sp = i > a && strcmp(lw[i - 1], "(") && strcmp(lw[i], ")"); /* written as (a b), not ( a b ) */
        int m = snprintf(s + n, sizeof buf[0] - n, "%s%s", sp ? " " : "", lw[i]);
        if (m < 0) break;
        n += (size_t)m;
        if (n >= sizeof buf[0] - 1) { n = sizeof buf[0] - 1; memcpy(s + n - 3, "...", 4); }
    }
    return s;
}
/* the end of the one value that starts at word p, by arity alone (no checks), or -1 */
static int skip_value(int p) {
    if (p >= ln) return -1;
    const char *t = lw[p];
    if (!strcmp(t, "(")) {
        int q = skip_value(p + 1);
        return q < 0 || q >= ln || strcmp(lw[q], ")") ? -1 : q + 1;
    }
    if (is_term(t)) return -1;
    if (is_lit(t) || find_name(t)) return p + 1;
    int g = find_callable(t);
    if (g >= 0) {
        p++;
        for (int i = 0; i < funcs[g].np && p >= 0; i++) p = skip_value(p);
        return p;
    }
    if (scalar_word(t) >= 0) return skip_value(p + 1);
    if (ref_word(t) >= 0) return p + 2 <= ln ? p + 2 : -1;
    int o = op_word(t);
    if (o == OP_GET) return p + 2 <= ln ? skip_value(p + 2) : -1;
    if (o == OP_NEW) return p + 3 <= ln ? skip_value(p + 3) : -1;
    if (o == OP_NEG || o == OP_NOT) return skip_value(p + 1);
    if (o >= 0) { int q = skip_value(p + 1); return q < 0 ? -1 : skip_value(q); }
    return -1;
}

/* ---- next: what may follow the prefix ---- */
static int need_text(const char *fmt, ...) {
    va_list ap;
    char tmp[512];
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    bput(&g_next, "%s", tmp);
    return NEED;
}
static unsigned array_elems(void) { /* element types of the visible arrays */
    unsigned m = 0;
    for (int i = 0; i < nnames; i++)
        if (regs[names[i].reg].ref != R_NONE) m |= 1u << regs[names[i].reg].s;
    return m;
}
/* a visible array name a with ref_ok(rf, a) (rf 0: the array itself) whose result fits w */
static int array_fits(int rf, const Want *w) {
    for (int i = 0; i < nnames; i++) {
        Ty t = regs[names[i].reg];
        if (t.ref == R_NONE) continue;
        if (rf == 0 ? want_ok(w, t) : ref_ok(rf, t) && want_ok(w, (Ty){rf, t.s, t.len})) return 1;
    }
    return 0;
}
static int callable_fits(int g, const Want *w) {
    const Func *f = &funcs[g];
    if (!f->has_ret || !want_ok(w, f->ret)) return 0;
    for (int i = 0; i < f->np; i++) {
        if (f->p[i].ref == R_NONE) continue;
        Want pw = want_param(f->p[i]);
        if (!array_fits(0, &pw) && !array_fits(R_OWN, &pw) && !array_fits(R_MUT, &pw) && !array_fits(R_SHARED, &pw)) return 0;
    }
    return 1;
}
static int op_fits(int o, const Want *w) {
    if (o == OP_NEW) return w->top && (w->arr == A_ANY || (w->arr == A_EXACT && w->p.ref == R_OWN));
    if (o == OP_GET) return (array_elems() & w->scal) != 0;
    unsigned m = is_arith(o) || is_shift(o) ? S_INT : is_cmp(o) ? 1u : o == OP_NEG ? S_SIGNED : S_ALL;
    return (m & w->scal) != 0;
}
/* the value slot w: names, number and words that may start a value there */
static int need_value(const Want *w, const char *extra) {
    Buf b = {0};
    int any = 0;
    bput(&b, "names:");
    for (int i = 0; i < nnames; i++)
        if (want_ok(w, regs[names[i].reg])) { bput(&b, " %s", names[i].name); any = 1; }
    bput(&b, "\n");
    if (w->lit == L_TYPED && w->lt.s != T_BOOL) { bput(&b, "number: %s\n", GW[w->lt.s]); any = 1; }
    if (w->lit == L_OPEN && (w->scal & S_INT)) { bput(&b, "number: any (it takes the type of the other value)\n"); any = 1; }
    bput(&b, "words:");
    if ((w->lit == L_TYPED && w->lt.s == T_BOOL) || (w->lit == L_OPEN && (w->scal & 1))) { bput(&b, " true false"); any = 1; }
    for (int o = 0; o < OP_N; o++)
        if (op_fits(o, w)) { bput(&b, " %s", OPW[o]); any = 1; }
    for (int s = 0; s < T_NSCALAR; s++)
        if (w->scal >> s & 1) { bput(&b, " %s", GW[s]); any = 1; }
    for (int s = 1; s < T_NSCALAR; s++) /* the IR names are accepted too */
        if (w->scal >> s & 1) bput(&b, " %s", IRN[s]);
    for (int rf = R_OWN; rf <= R_SHARED; rf++)
        if (array_fits(rf, w)) { bput(&b, " %s", REFW[rf]); any = 1; }
    for (int g = 0; g < nfuncs; g++)
        if (find_callable(funcs[g].name) == g && callable_fits(g, w)) { bput(&b, " %s", funcs[g].name); any = 1; }
    if (any) bput(&b, " (");
    if (extra) bput(&b, " %s", extra);
    bput(&b, "\n");
    if (!any) { free(b.p); return fail("no value of a fitting type can be written here"); }
    bput(&g_next, "%s", b.p);
    free(b.p);
    return NEED;
}

/* ---- values ---- */
/* a parsed value: an untyped bare number (lit, words[word]) or a register of type t. head: the word
 * of its op, type, own/edit/view or function (-1 for a name or number), arity: the values it takes */
typedef struct { int lit, word, reg, konst, head, arity, paren, a, b; Ty t; } Val;

static int to_dst(int *dst, Ty t) {
    if (*dst >= 0) {
        if (!teq(regs[*dst], t)) return fail("this writes %s but the name holds %s", tname(t), tname(regs[*dst]));
        return 0;
    }
    int r = new_reg(t);
    TRY(r);
    *dst = r;
    return 0;
}
/* a name's or number's register: used as is (dst -2), or copied into dst */
static int leaf(Val *v, int r, Ty t, int dst) {
    v->t = t;
    v->b = lp;
    if (dst == -2) { v->reg = r; return 0; }
    TRY(to_dst(&dst, t));
    TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, r));
    v->reg = dst;
    return 0;
}
static const char *plural(int n) { return n == 1 ? "" : "s"; }
/* words left over after the value v (in_paren: before its ')') */
static int extra_fail(const Val *v, int in_paren) {
    if (!in_paren && !strcmp(lw[lp], ")")) return fail("')' has no matching '('");
    int m = 0, p = lp, e = lp;
    while (p < ln && !is_term(lw[p])) {
        int q = skip_value(p);
        if (q < 0) break;
        m++;
        p = q;
    }
    while (e < ln && !(in_paren && !strcmp(lw[e], ")"))) e++;
    if (m > 0 && v && v->head >= 0 && !v->paren)
        return fail("%s has %d values after it; it takes %d", lw[v->head], v->arity + m, v->arity);
    if (in_paren) return fail("( ... ) must hold exactly one value; '%s' is extra", span(lp, e));
    return fail("extra words at the end of the line: '%s'", span(lp, ln));
}

static int value(const Want *want, int dst, Val *v);
/* operand k (0-based) of the word at head, which takes arity values */
static int slot(const Want *w, int dst, Val *v, int head, int k, int arity) {
    int r = value(w, dst, v);
    if (r == MISSING) return fail("%s has %d value%s after it; it takes %d", lw[head], k, plural(k), arity);
    return r;
}
enum { AN_GET, AN_PUT, AN_FREE, AN_REF };
/* an array name after the word at head (arity: the values head takes); for next, the names that
 * fit: get (element type in want), put (own or edit), free (own), own|edit|view rf (result in want) */
static int array_name(int head, int arity, int kind, int rf, const Want *want, int *reg, Ty *t) {
    if (lp >= ln) {
        if (!partial) return fail("%s has 0 values after it; it takes %d", lw[head], arity);
        Buf b = {0};
        int any = 0;
        bput(&b, "names:");
        for (int i = 0; i < nnames; i++) {
            Ty a = regs[names[i].reg];
            int ok = a.ref != R_NONE &&
                     (kind == AN_GET ? (want->scal >> a.s & 1) : kind == AN_PUT ? a.ref != R_SHARED :
                      kind == AN_FREE ? a.ref == R_OWN : ref_ok(rf, a) && want_ok(want, (Ty){rf, a.s, a.len}));
            if (ok) { bput(&b, " %s", names[i].name); any = 1; }
        }
        bput(&b, "\n");
        if (any) bput(&g_next, "%s", b.p);
        free(b.p);
        return any ? NEED : fail("no array of a fitting type is in scope here");
    }
    const char *w = lw[lp];
    Name *nm = find_name(w);
    if (!nm) {
        if (is_term(w) || !strcmp(w, "(")) return fail("%s takes an array name here, not '%s'", lw[head], w);
        if (is_lit(w)) return fail("'%s' is not an array", w);
        return fail("'%s' is not a name in scope here", w);
    }
    lp++;
    nm->read = 1;
    *reg = nm->reg;
    *t = regs[nm->reg];
    if (t->ref == R_NONE) return fail("'%s' is not an array", w);
    return 0;
}

/* the argument registers of a call to f (its name at word head), checked against its parameters */
static int call_args(const Func *f, int head, Buf *a) {
    for (int i = 0; i < f->np; i++) {
        Want w = want_param(f->p[i]);
        Val av;
        TRY(slot(&w, -2, &av, head, i, f->np));
        if (av.lit) return fail("'%s' is a number; %s takes %s there", lw[av.word], f->name, tname(f->p[i]));
        if (!binds(f->p[i], av.t))
            return fail("'%s' is %s; %s takes %s there", span(av.a, av.b), tname(av.t), f->name, tname(f->p[i]));
        bput(a, "%s%d", i ? ", " : "", av.reg);
    }
    return 0;
}

/*
 * value: one value starting at word lp, lowered into the current block, innermost first and left to
 * right. dst >= 0: write it there (its type must match); dst == -1: a fresh register; dst == -2: a
 * bare name or number uses its own register (no copy), anything else a fresh hidden register.
 * want: what the slot accepts (a bare number is typed by want->lt when L_TYPED, else left untyped
 * for the caller). Returns 0, MISSING (no value starts here), NEED (next) or -1.
 */
static int value(const Want *want, int dst, Val *v) {
    memset(v, 0, sizeof *v);
    v->head = v->word = v->reg = -1;
    v->a = lp;
    if (lp >= ln) return partial ? need_value(want, NULL) : MISSING;
    const char *t = lw[lp];
    if (!strcmp(t, ")")) return pdepth ? MISSING : fail("')' has no matching '('");
    if (is_term(t)) return MISSING;
    if (!strcmp(t, "(")) { /* ( value ): grouping only */
        int start = lp++;
        pdepth++;
        int r = value(want, dst, v);
        pdepth--;
        if (r == MISSING) {
            if (lp < ln && !strcmp(lw[lp], ")")) return fail("( ) holds no value; ( ... ) must hold exactly one value");
            return fail("a value is missing after '('");
        }
        TRY(r);
        if (lp >= ln) return partial ? need_text("words: )\n") : fail("'(' has no matching ')'");
        if (strcmp(lw[lp], ")")) return extra_fail(v, 1);
        lp++;
        v->paren = 1;
        v->a = start;
        v->b = lp;
        return 0;
    }
    if (is_lit(t)) {
        v->word = lp++;
        v->b = lp;
        if (want->lit != L_TYPED) { v->lit = 1; return 0; }
        int r = lit_reg(t, want->lt);
        TRY(r);
        v->konst = 1;
        return leaf(v, r, want->lt, dst);
    }
    Name *nm = find_name(t);
    if (nm) {
        lp++;
        nm->read = 1;
        return leaf(v, nm->reg, regs[nm->reg], dst);
    }
    int g = find_callable(t);
    if (g < 0 && find_func(t) >= 0) return fail("%s cannot call itself (calls go to earlier functions only)", t);
    v->head = lp++;
    if (g >= 0) { /* a call: one value per parameter */
        Func *f = &funcs[g];
        if (!f->has_ret) return fail("%s returns nothing; call it on its own line", f->name);
        v->arity = f->np;
        Buf a = {0};
        int r = call_args(f, v->head, &a);
        if (r >= 0) r = to_dst(&dst, f->ret);
        if (r >= 0) r = op("\"op\": \"call\", \"func\": \"%s\", \"args\": [%s], \"dst\": %d", f->name, a.p ? a.p : "", dst);
        free(a.p);
        TRY(r);
        v->t = f->ret;
        goto done;
    }
    v->arity = 1;
    int s = scalar_word(t);
    if (s >= 0) { /* <type> <number>: a typed number;  <type> <value>: a conversion */
        Ty ty = {R_NONE, s, 0};
        Want w = {s == T_BOOL ? 1u : S_INT, A_NONE, {0}, L_TYPED, ty, 0};
        Val a;
        TRY(slot(&w, -2, &a, v->head, 0, 1));
        if (a.t.ref != R_NONE) return fail("'%s' is an array; only numbers convert", span(a.a, a.b));
        if ((s == T_BOOL) != (a.t.s == T_BOOL)) return fail("bool does not convert to or from numbers; compare instead (ne x 0)");
        v->t = ty;
        if (a.konst && dst == -2 && a.t.s == s) { v->reg = a.reg; v->konst = 1; v->b = lp; return 0; } /* the number itself */
        TRY(to_dst(&dst, ty));
        TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d", s == a.t.s ? "mov" : "cast", dst, a.reg));
        goto done;
    }
    int rf = ref_word(t);
    if (rf >= 0) { /* own|edit|view <array name>: move or borrow */
        int a;
        Ty ta;
        TRY(array_name(v->head, 1, AN_REF, rf, want, &a, &ta));
        if (rf == R_OWN && ta.ref != R_OWN) return fail("own moves an own array; '%s' is %s", lw[lp - 1], tname(ta));
        if (rf == R_MUT && ta.ref == R_SHARED) return fail("edit needs an own or edit array; '%s' is view only", lw[lp - 1]);
        v->t = (Ty){rf, ta.s, ta.len};
        TRY(to_dst(&dst, v->t));
        TRY(op("\"op\": \"mov\", \"dst\": %d, \"a\": %d", dst, a));
        goto done;
    }
    int o = op_word(t);
    if (o < 0) return fail("'%s' is not a name in scope here, nor an op, a type or a function", t);
    if (o == OP_NEG || o == OP_NOT) {
        Want w = {want->scal & (o == OP_NEG ? S_SIGNED : S_ALL), A_NONE, {0}, L_NO, {0}, 0};
        Val a;
        TRY(slot(&w, -2, &a, v->head, 0, 1));
        if (a.lit) return fail("%s %s: the type of this number is unknown; write <type> %s", OPW[o], lw[a.word], lw[a.word]);
        if (a.t.ref != R_NONE) return fail("%s needs a number or bool", OPW[o]);
        if (o == OP_NEG && (a.t.s == T_BOOL || !is_signed(a.t.s))) return fail("neg needs a signed number; '%s' is %s", span(a.a, a.b), tname(a.t));
        v->t = a.t;
        TRY(to_dst(&dst, a.t));
        TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d", o == OP_NEG ? "neg" : (a.t.s == T_BOOL ? "not" : "bnot"), dst, a.reg));
        goto done;
    }
    if (o == OP_GET) { /* get <array name> <index> */
        v->arity = 2;
        int a;
        Ty ta;
        TRY(array_name(v->head, 2, AN_GET, 0, want, &a, &ta));
        Want iw = {S_INT, A_NONE, {0}, L_TYPED, {R_NONE, T_U32, 0}, 0};
        Val i;
        TRY(slot(&iw, -2, &i, v->head, 1, 2));
        if (!is_int(i.t)) return fail("index '%s' is not a whole number", span(i.a, i.b));
        v->t = (Ty){R_NONE, ta.s, 0};
        TRY(to_dst(&dst, v->t));
        TRY(op("\"op\": \"load\", \"dst\": %d, \"arr\": %d, \"index\": %d", dst, a, i.reg));
        goto done;
    }
    if (o == OP_NEW) { /* new <type> <length> <init> */
        v->arity = 3;
        if (!want->top) return fail("new makes an array for a name: write it on its own line (x = new ...)");
        if (lp >= ln) {
            if (!partial) return fail("new has 0 values after it; it takes 3");
            return need_text("words: bool uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64\n");
        }
        if (scalar_word(lw[lp]) < 0) return fail("'%s' is not an element type", lw[lp]);
        Ty et = {R_NONE, scalar_word(lw[lp++]), 0};
        if (lp >= ln) {
            if (!partial) return fail("new has 1 value after it; it takes 3");
            return need_text("number: 1..64\n");
        }
        int neg;
        uint64_t len;
        if (!parse_lit(lw[lp], &neg, &len) || neg || len < 1 || len > 64) return fail("array length '%s' must be 1..64", lw[lp]);
        lp++;
        Want iw = want_type(et);
        Val init;
        TRY(slot(&iw, -2, &init, v->head, 2, 3));
        if (!teq(init.t, et)) return fail("starting value '%s' is %s, not %s", span(init.a, init.b), tname(init.t), tname(et));
        v->t = (Ty){R_OWN, et.s, (int)len};
        TRY(to_dst(&dst, v->t));
        TRY(op("\"op\": \"alloc\", \"dst\": %d, \"init\": %d", dst, init.reg));
        goto done;
    }
    /* binary ops: a bare number takes the type of the other value; two bare numbers are refused */
    v->arity = 2;
    Want w1 = {is_cmp(o) ? S_ALL : want->scal & (is_arith(o) || is_shift(o) ? S_INT : S_ALL), A_NONE, {0}, L_OPEN, {0}, 0};
    Val a, b;
    TRY(slot(&w1, -2, &a, v->head, 0, 2));
    if (!a.lit) {
        if (a.t.ref != R_NONE) return fail("%s works on numbers, not arrays", OPW[o]);
        if ((is_arith(o) || is_shift(o)) && a.t.s == T_BOOL) return fail("%s needs whole numbers, not bool", OPW[o]);
    }
    Want w2 = {0};
    if (a.lit) w2 = (Want){lit_fits(lw[a.word], w1.scal), A_NONE, {0}, L_NO, {0}, 0};
    else w2 = (Want){is_shift(o) ? S_INT : 1u << a.t.s, A_NONE, {0}, L_TYPED, a.t, 0};
    TRY(slot(&w2, -2, &b, v->head, 1, 2));
    if (a.lit && b.lit) return fail("%s %s %s: two numbers; give one of them a name first", OPW[o], lw[a.word], lw[b.word]);
    if (b.t.ref != R_NONE) return fail("%s works on numbers, not arrays", OPW[o]);
    if (a.lit) { /* typed by the other value */
        if ((is_arith(o) || is_shift(o)) && b.t.s == T_BOOL) return fail("%s needs whole numbers, not bool", OPW[o]);
        int r = lit_reg(lw[a.word], b.t);
        TRY(r);
        a.reg = r;
        a.t = b.t;
    }
    if (is_shift(o)) {
        if (!is_int(a.t) || !is_int(b.t)) return fail("shifts need whole numbers");
        v->t = a.t;
    } else {
        if (!teq(a.t, b.t))
            return fail("%s needs two values of one type: '%s' is %s, '%s' is %s", OPW[o], span(a.a, a.b), tname(a.t), span(b.a, b.b), tname(b.t));
        if (is_arith(o) && a.t.s == T_BOOL) return fail("%s needs whole numbers, not bool", OPW[o]);
        v->t = is_cmp(o) ? (Ty){R_NONE, T_BOOL, 0} : a.t;
    }
    TRY(to_dst(&dst, v->t));
    TRY(op("\"op\": \"%s\", \"dst\": %d, \"a\": %d, \"b\": %d", IROP[o], dst, a.reg, b.reg));
done:
    v->reg = dst;
    v->b = lp;
    return 0;
}

/* the end of a statement: nothing more on the line (more: other words that may follow, for next) */
static int line_end(const Val *v, const char *more) {
    if (lp >= ln) return partial ? need_text("words: %s(newline)\n", more) : 0;
    return extra_fail(v, 0);
}
/* a statement word that stands alone (end, else, break) */
static int alone(const char *w) {
    if (lp < ln) return fail("%s stands alone", w);
    return partial ? need_text("words: (newline)\n") : 0;
}
/* a condition: any value giving bool */
static int cond(Val *c, const char *what) {
    Want b = want_type((Ty){R_NONE, T_BOOL, 0});
    int r = value(&b, -2, c);
    if (r == MISSING) return fail("%s needs a condition", what);
    TRY(r);
    if (!(c->t.ref == R_NONE && c->t.s == T_BOOL)) return fail("a condition must be a bool; this is %s", tname(c->t));
    return 0;
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
        b->ops.p = NULL;
    }
    bput(&out, "\n   ]}");
}

static int fn_line(char **w, int n) {
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

/*
 * Loops lower to the shape checker2 proves terminating (src/checker2/c2_check.c "loop bounds",
 * the same shape oscc emits for OSC `while ... bound N`): a counter written in the loop only by
 * ADD c, one; a guard block dominating every latch that compares the counter with a CONST
 * K <= 1,000,000 defined once (literals are hoisted and shared, so each is defined once).
 *   while <cond> [bound N]: hidden ulong counter; head: cond -> guard | exit;
 *     guard: ge cnt K -> trap 4 (loop bound) | body; body starts with cnt = add cnt 1.
 *   for <name> <type> <count number>: head: lt i K -> body | exit; latch: i = add i 1.
 *   for <name> <type> <count value>: as while (lt i count) with the hidden guard; a name is read at
 *     every pass, any other value is computed once, before the loop.
 */
static int guarded_loop(int is_for, int s, const Val *count, const char *forname) {
    Ty u64 = {R_NONE, T_U64, 0};
    int zero = lit_reg("0", u64), one = lit_reg("1", u64);
    TRY(zero);
    TRY(one);
    int ctr = -1, cnt = new_reg(u64);
    TRY(cnt);
    if (is_for) {
        Ty t = {R_NONE, s, 0};
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
    const char *bound = "1000000";
    if (is_for) {
        c = new_reg((Ty){R_NONE, T_BOOL, 0});
        TRY(c);
        TRY(op("\"op\": \"lt\", \"dst\": %d, \"a\": %d, \"b\": %d", c, ctr, count->reg));
    } else { /* the condition is computed in the head: again before every pass */
        Val cv;
        TRY(cond(&cv, "while"));
        c = cv.reg;
        if (lp < ln && !strcmp(lw[lp], "bound")) {
            lp++;
            if (lp >= ln) return partial ? need_text("number: 1..1000000\n") : fail("bound needs a number 1..1000000");
            bound = lw[lp++];
            int neg;
            uint64_t bv;
            if (!parse_lit(bound, &neg, &bv) || neg || bv < 1 || bv > 1000000) return fail("bound '%s' must be 1..1000000", bound);
            TRY(line_end(NULL, ""));
        } else TRY(line_end(&cv, "bound "));
    }
    int lim = lit_reg(bound, u64);
    TRY(lim);
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
    if (is_for) TRY(bind(forname, ctr, 1));
    return 0;
}

/* a for count that is a plain number, possibly in brackets: ( ( 8 ) ). Returns the number's word
 * index and sets *owed to the ')' still missing (above 0 only in next mode, when the prefix ends
 * inside the brackets); -1 when the count is any other value. */
static int count_number(int *owed) {
    int i = lp, k = 0;
    while (i < ln && !strcmp(lw[i], "(")) { i++; k++; }
    if (i >= ln || !is_lit(lw[i])) return -1;
    int j = i + 1, c = 0;
    while (j < ln && c < k && !strcmp(lw[j], ")")) { j++; c++; }
    if (j != ln || (c < k && !partial)) return -1;
    *owed = k - c;
    return i;
}
static int for_line(void) {
    if (lp >= ln) return partial ? need_text("new-name: any name not in use\n") : fail("for <name> <type> <count>");
    const char *nm = lw[lp++];
    TRY(check_new_name(nm));
    if (lp >= ln) return partial ? need_text("words: uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64\n") : fail("for <name> <type> <count>");
    int s = scalar_word(lw[lp]);
    if (s < 0 || s == T_BOOL) return fail("'%s' is not a whole-number type", lw[lp]);
    lp++;
    Ty t = {R_NONE, s, 0};
    if (cur < 0) return fail("this line can never run: the path already ended (ret, trap or break)");
    int owed, ki = count_number(&owed);
    if (ki >= 0) { /* for <name> <type> <count number>: name = 0, 1, ... count-1 (brackets change nothing) */
        const char *kw = lw[ki];
        int neg;
        uint64_t kv;
        if (!parse_lit(kw, &neg, &kv) || neg || kv > 1000000) return fail("count '%s' must be 0..1000000", kw);
        if (owed) return need_text("words: )\n");
        lp = ln;
        TRY(line_end(NULL, ""));
        int zero = lit_reg("0", t);
        TRY(zero);
        int k = lit_reg(kw, t);
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
        TRY(bind(nm, ctr, 1));
        start_block(body);
        return 0;
    }
    /* for <name> <type> <count value> */
    Want w = want_type(t);
    Val c;
    int r = value(&w, -2, &c);
    if (r == MISSING) return fail("for <name> <type> <count>");
    TRY(r);
    if (!teq(c.t, t)) return fail("count '%s' is %s, not %s", span(c.a, c.b), tname(c.t), tname(t));
    TRY(line_end(&c, ""));
    return guarded_loop(1, s, &c, nm);
}

static int stmt(void) {
    if (!strcmp(lw[0], "fn")) return fn_line(lw, ln);
    if (!in_fn) return fail("'%s' outside a function (start one with fn)", lw[0]);
    Func *f = &funcs[nfuncs - 1];
    const char *w0 = lw[0];
    lp = 1;
    if (!strcmp(w0, "end")) {
        Frame *fr = &frames[nframes - 1];
        if (fr->kind == F_FN && cur >= 0 && f->has_ret) return fail("%s can reach its end without ret", f->name);
        TRY(alone("end"));
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
    if (!strcmp(w0, "else")) {
        Frame *fr = &frames[nframes - 1];
        if (fr->kind != F_IF || fr->has_else) return fail("else without an open if");
        TRY(alone("else"));
        if (cur >= 0) { fr->join = new_block(); TRY(fr->join); TRY(br(fr->join)); }
        else fr->join = -1;
        fr->has_else = 1;
        TRY(pop_scope());
        depth++;
        start_block(fr->els);
        return 0;
    }
    if (!strcmp(w0, "if")) {
        Val c;
        TRY(cond(&c, "if"));
        TRY(line_end(&c, ""));
        int th = new_block(), el = new_block();
        TRY(th);
        TRY(el);
        blocks[th].refd = blocks[el].refd = 1;
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", c.reg, th, el));
        Frame *fr = push(F_IF);
        if (!fr) return -1;
        fr->els = el;
        fr->join = -1;
        start_block(th);
        return 0;
    }
    if (!strcmp(w0, "while")) {
        if (cur < 0) return fail("this line can never run: the path already ended (ret, trap or break)");
        return guarded_loop(0, 0, NULL, NULL);
    }
    if (!strcmp(w0, "for")) return for_line();
    if (!strcmp(w0, "break")) {
        for (int k = nframes - 1; k >= 0; k--)
            if (frames[k].kind == F_WHILE || frames[k].kind == F_FOR) { TRY(alone("break")); return br(frames[k].exit); }
        return fail("break outside a loop");
    }
    if (!strcmp(w0, "ret")) {
        if (!f->has_ret) {
            if (lp < ln) return fail("%s returns nothing: ret stands alone", f->name);
            if (partial) return need_text("words: (newline)\n");
            TRY(op("\"op\": \"ret\""));
            cur = -1;
            return 0;
        }
        Want w = want_type(f->ret);
        Val v;
        int r = value(&w, -2, &v);
        if (r == MISSING) return fail("%s returns %s: ret needs a value", f->name, tname(f->ret));
        TRY(r);
        if (!teq(v.t, f->ret)) return fail("this is %s; %s returns %s", tname(v.t), f->name, tname(f->ret));
        TRY(line_end(&v, ""));
        TRY(op("\"op\": \"ret\", \"a\": %d", v.reg));
        cur = -1;
        return 0;
    }
    if (!strcmp(w0, "trap")) {
        if (lp >= ln) return partial ? need_text("number: 1..14\n") : fail("trap <code 1..14>");
        int neg;
        uint64_t c;
        if (!parse_lit(lw[lp], &neg, &c) || neg || c < 1 || c > 14) return fail("trap <code 1..14>");
        lp++;
        if (lp < ln) return fail("trap <code 1..14>");
        if (partial) return need_text("words: (newline)\n");
        /* a trap ends the path, as ret does; the IR block still needs a terminator, so a ret that is
         * never reached follows (with a zero of the return type) */
        TRY(op("\"op\": \"trap\", \"code\": %" PRIu64, c));
        if (f->has_ret) {
            int z = lit_reg(f->ret.s == T_BOOL ? "false" : "0", f->ret);
            TRY(z);
            TRY(op("\"op\": \"ret\", \"a\": %d", z));
        } else TRY(op("\"op\": \"ret\""));
        cur = -1;
        return 0;
    }
    if (!strcmp(w0, "put")) { /* put <array name> <index> <value> */
        int a;
        Ty ta;
        Want none = {0};
        TRY(array_name(0, 3, AN_PUT, 0, &none, &a, &ta));
        if (ta.ref == R_SHARED) return fail("'%s' is view only; put needs an own or edit array", lw[lp - 1]);
        Want iw = {S_INT, A_NONE, {0}, L_TYPED, {R_NONE, T_U32, 0}, 0}, vw = want_type((Ty){R_NONE, ta.s, 0});
        Val i, v;
        TRY(slot(&iw, -2, &i, 0, 1, 3));
        if (!is_int(i.t)) return fail("index '%s' is not a whole number", span(i.a, i.b));
        TRY(slot(&vw, -2, &v, 0, 2, 3));
        if (!teq(v.t, vw.lt)) return fail("'%s' is %s; the array holds %s", span(v.a, v.b), tname(v.t), tname(vw.lt));
        Val whole = {.head = 0, .arity = 3};
        TRY(line_end(&whole, ""));
        return op("\"op\": \"store\", \"arr\": %d, \"index\": %d, \"value\": %d", a, i.reg, v.reg);
    }
    if (!strcmp(w0, "free")) {
        int a;
        Ty ta;
        Want none = {0};
        TRY(array_name(0, 1, AN_FREE, 0, &none, &a, &ta));
        if (ta.ref != R_OWN) return fail("'%s' is not an own array", lw[lp - 1]);
        Val whole = {.head = 0, .arity = 1};
        TRY(line_end(&whole, ""));
        return op("\"op\": \"release\", \"arr\": %d", a);
    }
    if (find_func(w0) >= 0 && find_callable(w0) < 0) return fail("%s cannot call itself (calls go to earlier functions only)", w0);
    int g = find_callable(w0);
    if (g >= 0) { /* a call on its own line */
        Func *cf = &funcs[g];
        if (cf->has_ret) return fail("%s returns %s; keep it: <name> = %s ...", cf->name, tname(cf->ret), cf->name);
        Buf a = {0};
        Val whole = {.head = 0, .arity = cf->np};
        int r = call_args(cf, 0, &a);
        if (r >= 0) r = line_end(&whole, "");
        if (r >= 0) r = op("\"op\": \"call\", \"func\": \"%s\", \"args\": [%s]", cf->name, a.p ? a.p : "");
        free(a.p);
        return r;
    }
    /* <name> = <value>, or <name> <value> (the short form, "=" left out) */
    Name *nm = find_name(w0);
    if (ln == 1 && !partial)
        return fail("'%s' does not start a line (a name =, if, while, for, ret, put, free, trap, break, else, end, or a call)", w0);
    if (nm && nm->fixed == 1) return fail("'%s' is the loop counter; it cannot be assigned", w0);
    if (!nm && (find_func(w0) >= 0 || reserved(w0) || !valid_ident(w0))) return fail("'%s' cannot be a name", w0);
    Want w = {S_ALL, A_ANY, {0}, L_NO, {0}, 1};
    if (nm) { w = want_type(regs[nm->reg]); w.top = 1; }
    if (lp < ln && !strcmp(lw[lp], "=")) lp++;
    else if (partial && lp >= ln) return need_value(&w, "=");
    Val v;
    int r = value(&w, nm ? nm->reg : -1, &v);
    if (r == MISSING) return fail("a value is missing after '%s'", span(0, lp));
    TRY(r);
    if (v.lit) return fail("'%s': the type of this number is unknown; write <type> %s", lw[v.word], lw[v.word]);
    TRY(line_end(&v, ""));
    if (!nm) TRY(bind(w0, v.reg, 0));
    return 0;
}

/* split one line into words; ( and ) are words of their own. Returns the count, or -1 */
static char LPAR[] = "(", RPAR[] = ")";
static int split(char *s, char **w) {
    int n = 0;
    char *p = s;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (n >= MAX_WORDS) return fail("more than %d words on one line", MAX_WORDS);
        if (*p == '(' || *p == ')') { w[n++] = *p++ == '(' ? LPAR : RPAR; continue; }
        w[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '(' && *p != ')') {
            if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '=')) return fail("character '%c' is not part of Omegese", *p);
            p++;
        }
        char c = *p;
        if (!c) break;
        *p++ = 0;
        if (c == '(' || c == ')') {
            if (n >= MAX_WORDS) return fail("more than %d words on one line", MAX_WORDS);
            w[n++] = c == '(' ? LPAR : RPAR;
        }
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
    if (strlen(b) != n) { printf("REFUSED omegese: a zero byte in the input\n"); exit(1); }
    *len = n;
    return b;
}

/* run every complete line of src; returns 0, or -1 with g_err set. keep_tail: the unfinished
 * last line (no newline) is left unparsed and returned through *tail */
static int run(char *src, int keep_tail, char **tail) {
    char *p = src;
    g_line = 0;
    out.n = 0;
    partial = 0;
    bput(&out, "{\"functions\": [");
    for (;;) {
        char *nl = strchr(p, '\n');
        if (!nl && keep_tail) { *tail = p; return 0; }
        if (nl) *nl = 0;
        g_line++;
        char *w[MAX_WORDS + 1];
        int n = split(p, w);
        if (n < 0) return -1;
        if (n > 0) {
            lw = w;
            ln = n;
            lp = 0;
            pdepth = 0;
            if (stmt() < 0) return -1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    g_line++;
    if (in_fn) return fail("the input ends inside function '%s' (missing end)", funcs[nfuncs - 1].name);
    if (nfuncs == 0) return fail("no function");
    bput(&out, "\n]}\n");
    return 0;
}

/* omegese next: which words may start or continue the unfinished line */
static int next_words(char *src) {
    char *tail = NULL;
    if (run(src, 1, &tail) < 0) { printf("REFUSED omegese: %s\n", g_err); return 1; }
    g_line++;
    char *w[MAX_WORDS + 1];
    int n = split(tail, w);
    if (n < 0) { printf("REFUSED omegese: %s\n", g_err); return 1; }
    if (!in_fn) {
        if (n > 0 && strcmp(w[0], "fn")) { printf("REFUSED omegese: a program starts with fn\n"); return 1; }
        printf(n == 0 ? "words: fn\n" : "words: (finish the fn line)\n");
        return 0;
    }
    if (n == 0 && cur < 0) { /* the path ended (ret, trap or break): only closing words may follow */
        Frame *fr = &frames[nframes - 1];
        printf("words:%s end\n", fr->kind == F_IF && !fr->has_else ? " else" : "");
        return 0;
    }
    if (cur < 0 && strcmp(w[0], "end") && strcmp(w[0], "else")) { printf("REFUSED omegese: this line can never run: the path already ended (ret, trap or break)\n"); return 1; }
    if (n == 0) {
        printf("words: if while for ret put free trap");
        int loop = 0;
        for (int k = 0; k < nframes; k++) if (frames[k].kind == F_WHILE || frames[k].kind == F_FOR) loop = 1;
        if (loop) printf(" break");
        if (frames[nframes - 1].kind == F_IF && !frames[nframes - 1].has_else) printf(" else");
        if (!(frames[nframes - 1].kind == F_FN && funcs[nfuncs - 1].has_ret)) printf(" end");
        for (int i = 0; i < nfuncs - 1; i++) if (!funcs[i].has_ret) printf(" %s", funcs[i].name);
        printf("\n");
        printf("assign:");
        for (int i = 0; i < nnames; i++) if (names[i].fixed != 1) printf(" %s", names[i].name);
        printf("\nnew-name: any name not in use\n");
        return 0;
    }
    /* the unfinished line, read in next mode: it stops where the prefix ends */
    lw = w;
    ln = n;
    lp = 0;
    pdepth = 0;
    partial = 1;
    g_next.n = 0;
    int r = stmt();
    if (r == NEED) { fputs(g_next.p, stdout); return 0; }
    if (r < 0) { printf("REFUSED omegese: %s\n", g_err); return 1; }
    printf("words: (newline)\n"); /* a line complete without a newline (fn) */
    return 0;
}

/* first cut of --machine: today's coarse sets, one item per line (to be replaced by the exact oracle) */
static const char *const LO[T_NSCALAR] = {"0", "0", "0", "0", "0", "-128", "-32768", "-2147483648", "-9223372036854775808"};
static const char *const HI[T_NSCALAR] = {"1", "255", "65535", "4294967295", "18446744073709551615", "127", "32767", "2147483647", "9223372036854775807"};
static void machine_print(char *txt, int eof) {
    if (!strncmp(txt, "REFUSED", 7)) { fputs(txt, stdout); return; }
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        char *c = strchr(line, ':');
        if (!c) continue;
        *c = 0;
        char *rest = c + 1;
        if (!strcmp(line, "new-name")) { printf("newname\n"); continue; }
        if (!strcmp(line, "number")) {
            while (*rest == ' ') rest++;
            char numw[64]; snprintf(numw, sizeof numw, "%s", rest); numw[strcspn(numw, " ")] = 0;
            int s = scalar_word(numw);
            if (s > 0) printf("number %s %s\n", LO[s], HI[s]);
            else if (!strncmp(rest, "any", 3)) printf("number -9223372036854775808 18446744073709551615\n");
            else { char *dd = strstr(rest, ".."); if (dd) { *dd = 0; printf("number %s %s\n", rest, dd + 2); } }
            continue;
        }
        for (char *save = NULL, *w = strtok_r(rest, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            if (!strcmp(w, "(newline)")) printf("newline\n");
            else if (!strcmp(w, "(") || !strcmp(w, ")")) printf("%s\n", w);
            else if (w[0] == '(') { printf("newname\nword to\nword void\n"); for (int s = 0; s < T_NSCALAR; s++) printf("word %s\n", GW[s]); break; }
            else printf("word %s\n", w);
        }
    }
    printf("space\n");
    if (eof) printf("eof\n");
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "next") && !strcmp(argv[2], "--machine")) {
        size_t len, mlen;
        char *src = slurp(argv[3], &len), *mbuf = NULL;
        FILE *real = stdout, *mem = open_memstream(&mbuf, &mlen);
        if (!mem) return 2;
        stdout = mem;
        int rc = next_words(src);
        fclose(mem);
        stdout = real;
        int eof = !in_fn && nfuncs > 0 && rc == 0 && mbuf && !strcmp(mbuf, "words: fn\n");
        machine_print(mbuf ? mbuf : (char *)"", eof);
        free(mbuf);
        free(src);
        return rc;
    }
    if (argc == 2 && !strcmp(argv[1], "words")) {
        for (size_t i = 0; i < NKW; i++) if (strcmp(KW[i], "=")) printf("%s\n", KW[i]);
        for (int i = 0; i < OP_N; i++) printf("%s\n", OPW[i]);
        for (int i = 0; i < T_NSCALAR; i++) printf("%s\n", GW[i]);
        for (int i = 1; i <= 3; i++) printf("%s\n", REFW[i]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "next")) {
        size_t len;
        char *src = slurp(argv[2], &len);
        int rc = next_words(src);
        free(src);
        return rc;
    }
    if (argc == 4 && !strcmp(argv[1], "emit")) {
        size_t len;
        char *src = slurp(argv[2], &len), *tail = NULL;
        if (run(src, 0, &tail) < 0) { printf("REFUSED omegese: %s\n", g_err); free(src); return 1; }
        free(src);
        FILE *fp = fopen(argv[3], "wb");
        if (!fp || fwrite(out.p, 1, out.n, fp) != out.n || fclose(fp)) { perror(argv[3]); return 2; }
        return 0;
    }
    fprintf(stderr, "usage: omegese emit <in.omegese> <out.json> | omegese next <prefix.omegese> | omegese words\n");
    return 2;
}
