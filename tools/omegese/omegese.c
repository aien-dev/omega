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
 *   omegese next <prefix.omegese>        what may come next after the prefix, exactly: every
 *                                      continuation that can still end in a program emit accepts
 *                                      (the mask an engine applies while the model writes); exit 1
 *                                      (REFUSED) when no continuation can
 *   omegese next --machine <prefix>      the same set, one item per line (OMEGESE.md section 6)
 *   omegese complete <prefix.omegese>    a text that, written after the prefix, makes a program emit
 *                                      accepts (finish the line, then close everything); exit 1 if
 *                                      there is none
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
/* an own array's may-state (checker2 "ownership flow"): bits OS_U not yet holding, OS_L holding, OS_D
 * freed or moved, on some path to here; m: the loop heads (bit = frame index) whose state, not yet
 * known while the loop is open, is also included */
enum { OS_U = 1, OS_L = 2, OS_D = 4 };
#define MAX_OWN 64
typedef struct { uint8_t b; uint64_t m; } Os;
/* oin: if, the state the else path starts with; loop, the state before it. oacc: if, the then path's
 * end (ohas); loop, the joined states at its breaks (ohas). opost: loop, the state after its head */
typedef struct { int kind; int depth; int els, join, has_else; int head, exit; int ctr; int ohas; Os oin[MAX_OWN], oacc[MAX_OWN], opost[MAX_OWN]; } Frame;

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

/* ---- ownership: the may-states of checker2's ownership flow, kept along the path being parsed ----
 * checker2 rejects an own array that is used, moved or freed while it holds nothing on every path to
 * that point, and one that gets a new array or reaches a return while it holds one on every path
 * (src/checker2/c2_check.c, C2-OWN-USE-DEAD, C2-OWN-DOUBLE-RELEASE, C2-OWN-LEAK). The same states are
 * kept here so that emit refuses those programs at the line that makes them wrong (OMEGESE.md 6.4).
 * Inside an open loop a state that includes the loop head is not known until the loop ends (a later
 * line of the body can change it): such a check waits in owq and is decided at the loop's end line,
 * when the head state is known. Borrows are not followed. */
static int nown;               /* tracked own registers of this function (beyond MAX_OWN: not tracked) */
static int own_reg[MAX_OWN];
static Os ost[MAX_OWN];        /* along the current path */
static Os own_pend[MAX_OWN];   /* the state before the loop whose head line is being read */
static int own_pk = -1;        /* that loop's frame index while its head is read, else -1 */
static uint64_t g_loops;       /* the loop heads whose state is still open, bit = frame index */
/* a check that waits for a loop to end: at line `line`, `name` needed an array (OW_USE, OW_FREE) or
 * needed to hold none on some path (OW_BIRTH, OW_RET) in state o, which still includes loop k's head */
enum { OW_USE, OW_FREE, OW_BIRTH, OW_RET };
#define MAX_OWQ 256
typedef struct { int k, kind, s, line; Os o; char name[NAME_MAX_LEN + 1]; } Ow;
static Ow owq[MAX_OWQ];
static int nowq;
static uint64_t below(int f) { return f >= 64 ? ~0ull : (1ull << f) - 1; }
static Os os_join(Os a, Os b) { return (Os){(uint8_t)(a.b | b.b), a.m | b.m}; }
static int own_slot(int r) {
    for (int s = 0; s < nown; s++)
        if (own_reg[s] == r) return s;
    return -1;
}
/* a new own register holds nothing on every path so far; the open loops may bring a state back */
static void own_track(int r) {
    if (nown >= MAX_OWN) return;
    int s = nown++;
    own_reg[s] = r;
    for (int f = 0; f < nframes && f < MAX_FRAMES; f++) {
        int loop = frames[f].kind == F_WHILE || frames[f].kind == F_FOR;
        frames[f].oin[s] = (Os){OS_U, g_loops & below(f)};
        frames[f].oacc[s] = (Os){OS_U, g_loops & below(loop ? f + 1 : f)};
        frames[f].opost[s] = (Os){OS_U, g_loops & below(f + 1)};
    }
    if (own_pk >= 0) own_pend[s] = (Os){OS_U, g_loops & below(own_pk)};
    ost[s] = (Os){OS_U, g_loops};
}

/* the line being read: its words and the read position */
static char **lw;
static int ln, lp, pdepth;
/* next and complete (OMEGESE.md section 6). g_fill: where the line runs out of words, write the
 * words it still needs (a witness completion) instead of stopping. g_quiet: keep no op text (only
 * acceptance matters). g_ret: the line is ret <value>; its value also reads the names the ended path
 * would leave unread (unread, not fixed, depth >= g_rfloor). g_pick, g_nopt: the array chosen at the
 * one array-name choice of the line, out of g_nopt. */
static int g_fill, g_quiet, g_ret, g_rfloor, g_pick, g_nopt;
/* the words a completion adds: lw is g_lw while a line is completed */
#define MAX_FILL (8 * MAX_WORDS)
static char *g_lw[MAX_FILL + 1];
static char g_arena[1 << 16];
static size_t g_an;
static int g_gen; /* the first word the completion added, or -1 */
static int fill_word(const char *w) {
    size_t k = strlen(w) + 1;
    if (ln >= MAX_FILL || g_an + k > sizeof g_arena) return fail("no completion fits in one line");
    memcpy(g_arena + g_an, w, k);
    if (g_gen < 0) g_gen = ln;
    lw[ln++] = g_arena + g_an;
    g_an += k;
    return 0;
}
/* add the words of s (separated by single spaces) */
static int fill_text(const char *s) {
    char tmp[NAME_MAX_LEN + 1];
    while (*s) {
        size_t k = strcspn(s, " ");
        if (k >= sizeof tmp) return fail("no completion fits in one line");
        if (k) {
            memcpy(tmp, s, k);
            tmp[k] = 0;
            TRY(fill_word(tmp));
        }
        s += k;
        if (*s) s++;
    }
    return 0;
}

static int new_reg(Ty t) {
    if (nregs >= MAX_REGS) return fail("more than %d values in one function", MAX_REGS);
    regs[nregs] = t;
    if (t.ref == R_OWN) own_track(nregs);
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

static const char *own_label(int r) {
    for (int i = nnames - 1; i >= 0; i--)
        if (names[i].reg == r) return names[i].name;
    return NULL;
}
static int os_none(Os o) { return o.m == 0 && !(o.b & OS_L); } /* holds nothing on every path */
static int os_only(Os o) { return o.m == 0 && o.b == OS_L; }   /* holds an array on every path */
/* an own array that holds nothing on every path here: no line may use, move or free it */
static int own_dead(int r) { int s = own_slot(r); return s >= 0 && os_none(ost[s]); }
/* a check on a state that still includes an open loop's head waits for the innermost such loop */
static int own_defer(int kind, int s, Os o) {
    int k = 63 - __builtin_clzll(o.m);
    for (int i = 0; i < nowq; i++) /* the same check on the same state: the same verdict, first line kept */
        if (owq[i].k == k && owq[i].kind == kind && owq[i].s == s && owq[i].o.b == o.b && owq[i].o.m == o.m) return 0;
    if (nowq >= MAX_OWQ) return fail("more than %d ownership checks wait for their loops to end", MAX_OWQ);
    const char *n = own_label(own_reg[s]);
    Ow *e = &owq[nowq++];
    e->k = k;
    e->kind = kind;
    e->s = s;
    e->line = g_line;
    e->o = o;
    snprintf(e->name, sizeof e->name, "%s", n ? n : "");
    return 0;
}
static int needs_array(Os o) { return o.m != 0 && !(o.b & OS_L); }           /* may turn out to hold none */
static int needs_room(Os o) { return o.m != 0 && !(o.b & (OS_U | OS_D)); }   /* may turn out to hold one */
/* a use: get, put, edit, view, an argument (C2-OWN-USE-DEAD) */
static int own_use(int r) {
    int s = own_slot(r);
    if (s >= 0 && os_none(ost[s])) {
        const char *n = own_label(r);
        return fail("'%s' holds no array here: it was freed or moved on every path to this line", n ? n : "own");
    }
    if (s >= 0 && needs_array(ost[s])) return own_defer(OW_USE, s, ost[s]);
    return 0;
}
static int own_kill(int r) { /* moved away */
    TRY(own_use(r));
    int s = own_slot(r);
    if (s >= 0) ost[s] = (Os){OS_D, 0};
    return 0;
}
static int own_birth(int r) { /* gets an array (C2-OWN-LEAK when it still holds one) */
    int s = own_slot(r);
    if (s < 0) return 0;
    if (os_only(ost[s])) {
        const char *n = own_label(r);
        return fail("'%s' still holds an array on every path to this line; free it before it gets another", n ? n : "own");
    }
    if (needs_room(ost[s])) TRY(own_defer(OW_BIRTH, s, ost[s]));
    ost[s] = (Os){OS_L, 0};
    return 0;
}
static int own_release(int r) { /* free (C2-OWN-DOUBLE-RELEASE) */
    int s = own_slot(r);
    if (s < 0) return 0;
    if (os_none(ost[s])) {
        const char *n = own_label(r);
        return fail("'%s' holds no array here: it was already freed or moved on every path to this line", n ? n : "own");
    }
    if (needs_array(ost[s])) TRY(own_defer(OW_FREE, s, ost[s]));
    ost[s] = (Os){OS_D, 0};
    return 0;
}
/* a mov dst <- a: an own array into an own register moves it, anything else borrows (a use) */
static int own_mov(int dst, int a) {
    if (regs[a].ref != R_OWN) return 0;
    if (regs[dst].ref == R_OWN) { TRY(own_kill(a)); return own_birth(dst); }
    return own_use(a);
}
/* a call: every array argument is used; one given to an own parameter is moved */
static int own_call(const Func *g, const int *args) {
    for (int k = 0; k < g->np; k++) {
        if (regs[args[k]].ref == R_NONE) continue;
        TRY(own_use(args[k]));
        if (g->p[k].ref == R_OWN) TRY(own_kill(args[k]));
    }
    return 0;
}
/* a return: no own array may still hold its array on every path (C2-OWN-LEAK) */
static int own_ret(void) {
    for (int s = 0; s < nown; s++)
        if (os_only(ost[s])) {
            const char *n = own_label(own_reg[s]);
            if (n) return fail("'%s' still holds its array at this return on every path; free it first", n);
            return fail("an array whose name is out of scope here still holds its data on every path to this return; free it inside its block, or end this path with trap");
        }
    for (int s = 0; s < nown; s++)
        if (needs_room(ost[s])) TRY(own_defer(OW_RET, s, ost[s]));
    return 0;
}
/* the head of a loop at frame index nframes is read next: its state is open until the loop ends */
static int own_loop_open(void) {
    int k = nframes;
    if (k >= MAX_FRAMES) return fail("more than %d nested blocks", MAX_FRAMES); /* before the bit k below */
    memcpy(own_pend, ost, sizeof ost[0] * (size_t)nown);
    own_pk = k;
    g_loops |= 1ull << k;
    for (int s = 0; s < nown; s++) ost[s] = (Os){0, 1ull << k};
    return 0;
}
/* the loop's frame is pushed: keep the state before it and the state after its head */
static void own_loop_pushed(Frame *fr) {
    memcpy(fr->oin, own_pend, sizeof ost[0] * (size_t)nown);
    memcpy(fr->opost, ost, sizeof ost[0] * (size_t)nown);
    fr->ohas = 0;
    own_pk = -1;
}
static Os os_subst(Os o, uint64_t kb, Os h) { return (o.m & kb) ? (Os){(uint8_t)(o.b | h.b), (o.m & ~kb) | h.m} : o; }
/* the loop at frame k ends: its head state is the state before it joined with the state at the end
 * of its body (the fixed point: the body's states only add the head's own), and the path continues
 * with the state after the head (the condition is false) joined with the states at its breaks */
static int own_loop_close(int k) {
    Frame *fr = &frames[k];
    uint64_t kb = 1ull << k;
    Os h[MAX_OWN];
    for (int s = 0; s < nown; s++) {
        h[s] = fr->oin[s];
        if (cur >= 0) h[s] = os_join(h[s], (Os){ost[s].b, ost[s].m & ~kb});
        Os x = os_subst(fr->opost[s], kb, h[s]);
        if (fr->ohas) x = os_join(x, os_subst(fr->oacc[s], kb, h[s]));
        ost[s] = x;
    }
    g_loops &= ~kb;
    /* the checks that waited for this loop: decided now, or passed on to the loop around it */
    int keep = 0;
    for (int i = 0; i < nowq; i++) {
        Ow e = owq[i];
        if (e.k == k) {
            e.o = os_subst(e.o, kb, h[e.s]);
            if (e.o.m) e.k = 63 - __builtin_clzll(e.o.m);
            else {
                const char *n = e.name[0] ? e.name : "an array out of scope here";
                if ((e.kind == OW_USE || e.kind == OW_FREE) && !(e.o.b & OS_L))
                    return fail("'%s' holds no array at line %d on every path, once this loop is closed: it was freed or moved and the loop gives it no new one", n, e.line);
                if (e.kind == OW_BIRTH && e.o.b == OS_L)
                    return fail("'%s' still holds an array at line %d on every path, once this loop is closed, where it gets another; free it first", n, e.line);
                if (e.kind == OW_RET && e.o.b == OS_L)
                    return fail("'%s' still holds its array at the return on line %d on every path, once this loop is closed; free it first", n, e.line);
                continue;
            }
        }
        owq[keep++] = e;
    }
    nowq = keep;
    return 0;
}
static void own_save(Os *to) { memcpy(to, ost, sizeof ost[0] * (size_t)nown); }
static void own_join_into(Os *to, int has) {
    for (int s = 0; s < nown; s++) to[s] = has ? os_join(to[s], ost[s]) : ost[s];
}
/* an if ends: the path after it joins this path (if it still runs) with the other one: the else path
 * of an if without else, or the then path of an if with else (if that one still ran at the else) */
static void own_if_end(const Frame *fr) {
    const Os *other = fr->has_else ? (fr->ohas ? fr->oacc : NULL) : fr->oin;
    if (!other) return;
    for (int s = 0; s < nown; s++) ost[s] = cur >= 0 ? os_join(ost[s], other[s]) : other[s];
}

/* ---- ops into the current block (block refs are written as @id@, numbered at the end) ---- */
static int op(const char *fmt, ...) {
    if (cur < 0) return fail("this line can never run: the path already ended (ret, trap or break)");
    if (g_quiet) return 0;
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
    if (*neg && !v) return 0; /* -0 is written 0 */
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

/* parse a type at lw[lp]: scalar word, or own|edit|view <scalar> <len> (completing it in fill mode) */
static int parse_type(Ty *t) {
    if (lp >= ln) {
        if (!g_fill) return fail("a type is missing");
        TRY(fill_word(GW[T_U32]));
    }
    int s = scalar_word(lw[lp]);
    if (s >= 0) { *t = (Ty){R_NONE, s, 0}; lp++; return 0; }
    int r = ref_word(lw[lp]);
    if (r < 0) return fail("'%s' is not a type (%s ..., or own/edit/view <type> <length>)", lw[lp], GW[T_U32]);
    if (lp + 2 >= ln && !g_fill) return fail("an array type is %s <element type> <length>", REFW[r]);
    if (lp + 1 >= ln) TRY(fill_word(GW[T_U32]));
    s = scalar_word(lw[lp + 1]);
    if (s < 0) return fail("'%s' is not an element type", lw[lp + 1]);
    if (lp + 2 >= ln) TRY(fill_word("1"));
    int neg;
    uint64_t len;
    if (!parse_lit(lw[lp + 2], &neg, &len) || neg || len < 1 || len > 64) return fail("array length '%s' must be 1..64", lw[lp + 2]);
    *t = (Ty){r, s, (int)len};
    lp += 3;
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

/* ---- completing a line: the words a value slot still needs (fill mode) ---- */
/* a name no word of the line, no name in scope and no function uses */
static const char *fresh_name(void) {
    static char b[24];
    for (int k = 0;; k++) {
        snprintf(b, sizeof b, "w%d", k);
        int used = find_name(b) || find_func(b) >= 0;
        for (int i = 0; i < ln && !used; i++) used = !strcmp(lw[i], b);
        if (!used) return b;
    }
}
/* R: a name the ended path would leave unread, so this ret line must read it */
static char g_taken[MAX_NAMES]; /* names of R already written into the completion */
static int in_r(int i) { return g_ret && !names[i].fixed && !names[i].read && names[i].depth >= g_rfloor && !g_taken[i]; }
/* a visible array for a slot that takes the array w, one of R first (it is read there); -1 if none */
static int pick_array(const Want *w) {
    int best = -1;
    for (int i = 0; i < nnames; i++) {
        Ty t = regs[names[i].reg];
        if (t.ref == R_NONE || !want_ok(w, t) || own_dead(names[i].reg)) continue;
        if (in_r(i)) return i;
        if (best < 0) best = i;
    }
    return best;
}
/* may f be called here with every array parameter given a visible array */
static int args_exist(const Func *f) {
    for (int k = 0; k < f->np; k++) {
        if (f->p[k].ref == R_NONE) continue;
        Want pw = want_param(f->p[k]);
        if (pick_array(&pw) < 0) return 0;
    }
    return 1;
}
static int gives_int(int g) {
    const Func *f = &funcs[g];
    return find_callable(f->name) == g && f->has_ret && f->ret.ref == R_NONE && f->ret.s != T_BOOL && args_exist(f);
}
/* a call of g as an int value of type s: bx (if set) goes to its first bool parameter; each array
 * parameter takes an array of R that binds (else any that binds) */
static void call_text(int g, int s, const char *bx, Buf *b) {
    const Func *f = &funcs[g];
    if (f->ret.s != s) bput(b, "%s ", GW[s]);
    bput(b, "%s", f->name);
    for (int k = 0; k < f->np; k++) {
        Ty p = f->p[k];
        if (p.ref == R_NONE) {
            if (p.s == T_BOOL && bx) { bput(b, " %s", bx); bx = NULL; }
            else bput(b, " %s", p.s == T_BOOL ? "true" : "0");
            continue;
        }
        Want pw = want_param(p);
        int i = pick_array(&pw);
        g_taken[i] = 1;
        bput(b, " %s", names[i].name);
    }
}
static void join_terms(Buf *b, const char *opw, Buf *tl, int n, const char *base) {
    if (!n) { bput(b, "%s", base); return; }
    char *p = tl->p;
    for (int k = 0; k < n; k++) {
        char *e = strchr(p, '\n');
        *e = 0;
        if (k < n - 1) bput(b, "%s %s ", opw, p);
        else bput(b, "%s", p);
        p = e + 1;
    }
}
/* a value of scalar type s that reads every name of R it can (base: the value when R is empty).
 * bool reads everything: ints by eq r r, arrays by get. An int reads ints by conversion, arrays of
 * ints by get, and bools only through a function that gives an int: its bool parameter, or an array
 * parameter for an array of bools. */
static void gen_scalar(int s, const char *base, Buf *b) {
    Buf tl = {0};
    int n = 0, needb = 0;
    for (int i = 0; i < nnames; i++) {
        if (!in_r(i)) continue;
        Ty t = regs[names[i].reg];
        const char *nm = names[i].name;
        if (s == T_BOOL) {
            if (t.ref == R_NONE && t.s == T_BOOL) bput(&tl, "%s\n", nm);
            else if (t.ref == R_NONE) bput(&tl, "eq %s %s\n", nm, nm);
            else if (t.s == T_BOOL) bput(&tl, "get %s 0\n", nm);
            else bput(&tl, "eq get %s 0 0\n", nm);
        } else if (t.s == T_BOOL) { needb = 1; continue; }
        else if (t.ref == R_NONE) { if (t.s == s) bput(&tl, "%s\n", nm); else bput(&tl, "%s %s\n", GW[s], nm); }
        else { if (t.s == s) bput(&tl, "get %s 0\n", nm); else bput(&tl, "%s get %s 0\n", GW[s], nm); }
        g_taken[i] = 1;
        n++;
    }
    if (needb) {
        int h = -1;
        for (int g = 0; g < nfuncs && h < 0; g++)
            if (gives_int(g))
                for (int k = 0; k < funcs[g].np; k++)
                    if (funcs[g].p[k].ref == R_NONE && funcs[g].p[k].s == T_BOOL) { h = g; break; }
        if (h >= 0) { /* every bool of R goes into h's bool parameter */
            Buf bx = {0};
            gen_scalar(T_BOOL, "true", &bx);
            call_text(h, s, bx.p, &tl);
            bput(&tl, "\n");
            free(bx.p);
            n++;
        } else /* arrays of bools of R, each through an array parameter that binds it */
            for (int i = 0; i < nnames; i++) {
                if (!in_r(i) || regs[names[i].reg].ref == R_NONE) continue;
                Ty t = regs[names[i].reg];
                for (int g = 0; g < nfuncs && in_r(i); g++) {
                    if (!gives_int(g)) continue;
                    for (int k = 0; k < funcs[g].np; k++)
                        if (funcs[g].p[k].ref != R_NONE && binds(funcs[g].p[k], t)) {
                            call_text(g, s, NULL, &tl); /* takes one more array of R */
                            bput(&tl, "\n");
                            n++;
                            g--; /* again, until i is read */
                            break;
                        }
                }
            }
    }
    join_terms(b, s == T_BOOL ? "and" : "add", &tl, n, base);
    free(tl.p);
}
/* the words of one value for the slot w */
static int gen_value(const Want *w) {
    memset(g_taken, 0, sizeof g_taken);
    Buf b = {0};
    int r = 0;
    if (!w->scal) { /* an array: a name, own/edit/view of a name, or new (on its own line) */
        int i = pick_array(w);
        if (i >= 0) bput(&b, "%s", names[i].name);
        for (int rf = R_OWN; i < 0 && rf <= R_SHARED; rf++)
            for (int k = 0; k < nnames && i < 0; k++) {
                Ty t = regs[names[k].reg];
                if (ref_ok(rf, t) && want_ok(w, (Ty){rf, t.s, t.len}) && !own_dead(names[k].reg)) { i = k; bput(&b, "%s %s", REFW[rf], names[k].name); }
            }
        if (i < 0 && w->top && w->arr == A_EXACT && w->p.ref == R_OWN)
            bput(&b, "new %s %d %s", GW[w->p.s], w->p.len, w->p.s == T_BOOL ? "false" : "0");
        else if (i < 0) r = fail("no array of a fitting type is in scope here");
    } else {
        unsigned m = w->scal;
        int s = -1, nr = 0;
        for (int i = 0; i < nnames; i++) nr += in_r(i);
        if (nr && (m & 1)) s = T_BOOL; /* bool reads every kind of name */
        else if (w->lit == L_TYPED && (m >> w->lt.s & 1)) s = w->lt.s;
        else if (m >> T_U32 & 1) s = T_U32;
        else for (int k = 1; k < T_NSCALAR && s < 0; k++) if (m >> k & 1) s = k;
        if (s < 0) s = T_BOOL;
        const char *lit = s == T_BOOL ? "true" : "0";
        char base[32];
        if (w->lit == L_TYPED && w->lt.s == s) snprintf(base, sizeof base, "%s", lit);
        else snprintf(base, sizeof base, "%s %s", GW[s], lit);
        gen_scalar(s, base, &b);
    }
    if (r == 0) r = fill_text(b.p);
    free(b.p);
    return r;
}
/* the depth from which the names must be read before a ret here: the innermost block, and the blocks
 * around it while closing one leaves the path ended (an if whose both branches ended) */
static int chain_floor(void) {
    int k = nframes - 1;
    while (k > 0 && frames[k].kind == F_IF && frames[k].has_else && frames[k].join < 0) k--;
    return frames[k].depth;
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
    TRY(own_mov(dst, r));
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
/* an array name after the word at head (arity: the values head takes) that fits: get (element type
 * in want), put (own or edit), free (own), own|edit|view rf (result in want). In fill mode a missing
 * name is chosen: one of R first; the first such choice of a line is the one g_pick walks through */
static int array_name(int head, int arity, int kind, int rf, const Want *want, int *reg, Ty *t) {
    if (lp >= ln) {
        if (!g_fill) return fail("%s has 0 values after it; it takes %d", lw[head], arity);
        int cand[MAX_NAMES], nc = 0, nd = 0;
        memset(g_taken, 0, sizeof g_taken);
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < nnames; i++) {
                Ty a = regs[names[i].reg];
                int ok = a.ref != R_NONE &&
                         (kind == AN_GET ? (want->scal >> a.s & 1) : kind == AN_PUT ? a.ref != R_SHARED :
                          kind == AN_FREE ? a.ref == R_OWN : ref_ok(rf, a) && want_ok(want, (Ty){rf, a.s, a.len}));
                if (ok && own_dead(names[i].reg)) nd += !pass;
                else if (ok && in_r(i) == !pass) cand[nc++] = i;
            }
        if (!nc && nd) return fail("every array that fits here holds nothing: it was freed or moved on every path to this line");
        if (!nc) return fail("no array of a fitting type is in scope here");
        int k = 0;
        if (nc > 1 && !g_nopt) { g_nopt = nc; k = g_pick % nc; }
        TRY(fill_word(names[cand[k]].name));
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
static int call_args(const Func *f, int head, Buf *a, int *args) {
    for (int i = 0; i < f->np; i++) {
        Want w = want_param(f->p[i]);
        Val av;
        TRY(slot(&w, -2, &av, head, i, f->np));
        if (av.lit) return fail("'%s' is a number; %s takes %s there", lw[av.word], f->name, tname(f->p[i]));
        if (!binds(f->p[i], av.t))
            return fail("'%s' is %s; %s takes %s there", span(av.a, av.b), tname(av.t), f->name, tname(f->p[i]));
        bput(a, "%s%d", i ? ", " : "", av.reg);
        args[i] = av.reg;
    }
    return 0;
}

/*
 * value: one value starting at word lp, lowered into the current block, innermost first and left to
 * right. dst >= 0: write it there (its type must match); dst == -1: a fresh register; dst == -2: a
 * bare name or number uses its own register (no copy), anything else a fresh hidden register.
 * want: what the slot accepts (a bare number is typed by want->lt when L_TYPED, else left untyped
 * for the caller). Returns 0, MISSING (no value starts here) or -1. In fill mode a value that is not
 * written yet is completed (gen_value).
 */
static int value(const Want *want, int dst, Val *v) {
    memset(v, 0, sizeof *v);
    v->head = v->word = v->reg = -1;
    v->a = lp;
    if (lp >= ln) {
        if (!g_fill) return MISSING;
        TRY(gen_value(want));
    }
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
        if (lp >= ln) {
            if (!g_fill) return fail("'(' has no matching ')'");
            TRY(fill_word(")"));
        }
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
    if (isdigit((unsigned char)t[0]) || t[0] == '-')
        return fail("'%s' is not a number (digits, an optional minus sign, no leading zeros, no -0)", t);
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
        int args[MAX_PARAMS];
        int r = call_args(f, v->head, &a, args);
        if (r >= 0) r = to_dst(&dst, f->ret);
        if (r >= 0) r = op("\"op\": \"call\", \"func\": \"%s\", \"args\": [%s], \"dst\": %d", f->name, a.p ? a.p : "", dst);
        if (r >= 0) r = own_call(f, args);
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
        TRY(own_mov(dst, a));
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
        TRY(own_use(a));
        goto done;
    }
    if (o == OP_NEW) { /* new <type> <length> <init> */
        v->arity = 3;
        if (!want->top) return fail("new makes an array for a name: write it on its own line (x = new ...)");
        if (lp >= ln) {
            if (!g_fill) return fail("new has 0 values after it; it takes 3");
            TRY(fill_word(GW[want->arr == A_EXACT ? want->p.s : T_U32]));
        }
        if (scalar_word(lw[lp]) < 0) return fail("'%s' is not an element type", lw[lp]);
        Ty et = {R_NONE, scalar_word(lw[lp++]), 0};
        if (lp >= ln) {
            if (!g_fill) return fail("new has 1 value after it; it takes 3");
            char lb[8];
            snprintf(lb, sizeof lb, "%d", want->arr == A_EXACT ? want->p.len : 1);
            TRY(fill_word(lb));
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
        TRY(own_birth(dst));
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

/* the end of a statement: nothing more on the line */
static int line_end(const Val *v) {
    if (lp >= ln) return 0;
    return extra_fail(v, 0);
}
/* a statement word that stands alone (end, else, break) */
static int alone(const char *w) {
    if (lp < ln) return fail("%s stands alone", w);
    return 0;
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

static int fn_line(void) {
    if (in_fn) return fail("fn inside a function: close the function with end first");
    if (nfuncs >= MAX_FUNCS) return fail("more than %d functions", MAX_FUNCS);
    if (ln < 3 && !g_fill) return fail("fn <name> <param> <type> ... to <type or void>");
    if (ln < 2) TRY(fill_word(fresh_name()));
    if (!valid_ident(lw[1]) || reserved(lw[1])) return fail("'%s' cannot be a function name", lw[1]);
    if (find_func(lw[1]) >= 0) return fail("function '%s' is already defined", lw[1]);
    Func *f = &funcs[nfuncs];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", lw[1]);
    nregs = nnames = nconsts = nblocks = norder = nframes = 0;
    nown = 0;
    nowq = 0;
    g_loops = 0;
    own_pk = -1;
    depth = 0;
    Frame *fr = push(F_FN);
    if (!fr) return -1;
    lp = 2;
    for (;;) {
        if (lp >= ln) {
            if (!g_fill) return fail("fn needs 'to <type>' or 'to void' at the end");
            TRY(fill_word("to"));
        }
        if (!strcmp(lw[lp], "to")) break;
        if (f->np >= MAX_PARAMS) return fail("more than %d parameters", MAX_PARAMS);
        const char *pn = lw[lp++];
        Ty t;
        TRY(parse_type(&t));
        f->p[f->np++] = t;
        int r = new_reg(t);
        TRY(r);
        if (find_name(pn)) return fail("parameter '%s' is named twice", pn);
        TRY(bind(pn, r, 2));
        if (t.ref == R_OWN && own_slot(r) >= 0) ost[own_slot(r)] = (Os){OS_L, 0}; /* a parameter holds its array */
    }
    lp++;
    if (lp >= ln && g_fill) TRY(fill_word("void"));
    if (lp < ln && !strcmp(lw[lp], "void")) { f->has_ret = 0; lp++; }
    else {
        Ty t;
        TRY(parse_type(&t));
        if (t.ref != R_NONE) return fail("a function returns a number or bool, not an array");
        f->has_ret = 1;
        f->ret = t;
    }
    if (lp != ln) return fail("'%s' after the return type", lw[lp]);
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
    TRY(own_loop_open());
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
            if (lp >= ln) {
                if (!g_fill) return fail("bound needs a number 1..1000000");
                TRY(fill_word("1"));
            }
            bound = lw[lp++];
            int neg;
            uint64_t bv;
            if (!parse_lit(bound, &neg, &bv) || neg || bv < 1 || bv > 1000000) return fail("bound '%s' must be 1..1000000", bound);
            TRY(line_end(NULL));
        } else TRY(line_end(&cv));
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
    own_loop_pushed(fr);
    fr->head = h;
    fr->exit = ex;
    fr->ctr = ctr;
    if (is_for) TRY(bind(forname, ctr, 1));
    return 0;
}

/* a for count that is a plain number, possibly in brackets: ( ( 8 ) ). Returns the number's word
 * index (in fill mode closing the brackets left open), or -1 when the count is any other value. */
static int count_number(void) {
    int i = lp, k = 0;
    while (i < ln && !strcmp(lw[i], "(")) { i++; k++; }
    if (i >= ln || !is_lit(lw[i])) return -1;
    int j = i + 1, c = 0;
    while (j < ln && c < k && !strcmp(lw[j], ")")) { j++; c++; }
    if (j == ln && g_fill)
        for (; c < k; c++, j++)
            if (fill_word(")") < 0) return -1;
    if (j != ln || c < k) return -1;
    return i;
}
static int for_line(void) {
    if (lp >= ln) {
        if (!g_fill) return fail("for <name> <type> <count>");
        TRY(fill_word(fresh_name()));
    }
    const char *nm = lw[lp++];
    TRY(check_new_name(nm));
    if (lp >= ln) {
        if (!g_fill) return fail("for <name> <type> <count>");
        TRY(fill_word(GW[T_U32]));
    }
    int s = scalar_word(lw[lp]);
    if (s < 0 || s == T_BOOL) return fail("'%s' is not a whole-number type", lw[lp]);
    lp++;
    Ty t = {R_NONE, s, 0};
    if (cur < 0) return fail("this line can never run: the path already ended (ret, trap or break)");
    int ki = count_number();
    if (ki >= 0) { /* for <name> <type> <count number>: name = 0, 1, ... count-1 (brackets change nothing) */
        const char *kw = lw[ki];
        int neg;
        uint64_t kv;
        if (!parse_lit(kw, &neg, &kv) || neg || kv > 1000000) return fail("count '%s' must be 0..1000000", kw);
        lp = ln;
        TRY(line_end(NULL));
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
        TRY(own_loop_open());
        start_block(h);
        int c = new_reg((Ty){R_NONE, T_BOOL, 0});
        TRY(c);
        TRY(op("\"op\": \"lt\", \"dst\": %d, \"a\": %d, \"b\": %d", c, ctr, k));
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", c, body, ex));
        Frame *fr = push(F_FOR);
        if (!fr) return -1;
        own_loop_pushed(fr);
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
    TRY(line_end(&c));
    return guarded_loop(1, s, &c, nm);
}

static int stmt(void) {
    if (!strcmp(lw[0], "fn")) return fn_line();
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
            own_if_end(fr);
            if (!fr->has_else) { if (cur >= 0) TRY(br(fr->els)); start_block(fr->els); }
            else {
                if (cur >= 0) { if (fr->join < 0) { fr->join = new_block(); TRY(fr->join); } TRY(br(fr->join)); }
                if (fr->join >= 0) start_block(fr->join); else cur = -1;
            }
            break;
        case F_WHILE:
            TRY(own_loop_close(nframes - 1));
            if (cur >= 0) TRY(br(fr->head));
            start_block(fr->exit);
            break;
        case F_FOR: {
            TRY(own_loop_close(nframes - 1));
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
                TRY(own_ret());
                cur = -1;
            }
            if (!g_quiet) end_function();
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
        if (cur >= 0) { own_save(fr->oacc); fr->ohas = 1; }
        if (cur >= 0) { fr->join = new_block(); TRY(fr->join); TRY(br(fr->join)); }
        else fr->join = -1;
        fr->has_else = 1;
        TRY(pop_scope());
        depth++;
        start_block(fr->els);
        memcpy(ost, fr->oin, sizeof ost[0] * (size_t)nown);
        return 0;
    }
    if (!strcmp(w0, "if")) {
        Val c;
        TRY(cond(&c, "if"));
        TRY(line_end(&c));
        int th = new_block(), el = new_block();
        TRY(th);
        TRY(el);
        blocks[th].refd = blocks[el].refd = 1;
        TRY(op("\"op\": \"cbr\", \"cond\": %d, \"then\": @%d@, \"else\": @%d@", c.reg, th, el));
        Frame *fr = push(F_IF);
        if (fr) { own_save(fr->oin); fr->ohas = 0; }
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
            if (frames[k].kind == F_WHILE || frames[k].kind == F_FOR) {
                TRY(alone("break"));
                if (cur >= 0) { own_join_into(frames[k].oacc, frames[k].ohas); frames[k].ohas = 1; }
                return br(frames[k].exit);
            }
        return fail("break outside a loop");
    }
    if (!strcmp(w0, "ret")) {
        if (!f->has_ret) {
            if (lp < ln) return fail("%s returns nothing: ret stands alone", f->name);
            TRY(op("\"op\": \"ret\""));
            TRY(own_ret());
            cur = -1;
            return 0;
        }
        if (g_fill) { g_ret = 1; g_rfloor = chain_floor(); }
        Want w = want_type(f->ret);
        Val v;
        int r = value(&w, -2, &v);
        if (r == MISSING) return fail("%s returns %s: ret needs a value", f->name, tname(f->ret));
        TRY(r);
        if (!teq(v.t, f->ret)) return fail("this is %s; %s returns %s", tname(v.t), f->name, tname(f->ret));
        TRY(line_end(&v));
        TRY(op("\"op\": \"ret\", \"a\": %d", v.reg));
        TRY(own_ret());
        cur = -1;
        return 0;
    }
    if (!strcmp(w0, "trap")) {
        if (lp >= ln) {
            if (!g_fill) return fail("trap <code 1..14>");
            TRY(fill_word("1"));
        }
        int neg;
        uint64_t c;
        if (!parse_lit(lw[lp], &neg, &c) || neg || c < 1 || c > 14) return fail("trap <code 1..14>");
        lp++;
        if (lp < ln) return fail("trap <code 1..14>");
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
        TRY(line_end(&whole));
        TRY(op("\"op\": \"store\", \"arr\": %d, \"index\": %d, \"value\": %d", a, i.reg, v.reg));
        return own_use(a);
    }
    if (!strcmp(w0, "free")) {
        int a;
        Ty ta;
        Want none = {0};
        TRY(array_name(0, 1, AN_FREE, 0, &none, &a, &ta));
        if (ta.ref != R_OWN) return fail("'%s' is not an own array", lw[lp - 1]);
        Val whole = {.head = 0, .arity = 1};
        TRY(line_end(&whole));
        TRY(op("\"op\": \"release\", \"arr\": %d", a));
        return own_release(a);
    }
    if (find_func(w0) >= 0 && find_callable(w0) < 0) return fail("%s cannot call itself (calls go to earlier functions only)", w0);
    int g = find_callable(w0);
    if (g >= 0) { /* a call on its own line */
        Func *cf = &funcs[g];
        if (cf->has_ret) return fail("%s returns %s; keep it: <name> = %s ...", cf->name, tname(cf->ret), cf->name);
        Buf a = {0};
        Val whole = {.head = 0, .arity = cf->np};
        int args[MAX_PARAMS];
        int r = call_args(cf, 0, &a, args);
        if (r >= 0) r = line_end(&whole);
        if (r >= 0) r = op("\"op\": \"call\", \"func\": \"%s\", \"args\": [%s]", cf->name, a.p ? a.p : "");
        if (r >= 0) r = own_call(cf, args);
        free(a.p);
        return r;
    }
    /* <name> = <value>, or <name> <value> (the short form, "=" left out) */
    Name *nm = find_name(w0);
    if (ln == 1 && !g_fill)
        return fail("'%s' does not start a line (a name =, if, while, for, ret, put, free, trap, break, else, end, or a call)", w0);
    if (nm && nm->fixed == 1) return fail("'%s' is the loop counter; it cannot be assigned", w0);
    if (!nm && (find_func(w0) >= 0 || reserved(w0) || !valid_ident(w0))) return fail("'%s' cannot be a name", w0);
    Want w = {S_ALL, A_ANY, {0}, L_NO, {0}, 1};
    if (nm) { w = want_type(regs[nm->reg]); w.top = 1; }
    if (lp < ln && !strcmp(lw[lp], "=")) lp++;
    Val v;
    int r = value(&w, nm ? nm->reg : -1, &v);
    if (r == MISSING) return fail("a value is missing after '%s'", span(0, lp));
    TRY(r);
    if (v.lit) return fail("'%s': the type of this number is unknown; write <type> %s", lw[v.word], lw[v.word]);
    TRY(line_end(&v));
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
#if defined(OMEGESE_CANARY) && OMEGESE_CANARY == 1 /* negative control for the ASan run: a read after free */
    { char *volatile q = malloc(8); if (!q) exit(2); free(q); volatile char c = q[0]; (void)c; }
#elif defined(OMEGESE_CANARY) && OMEGESE_CANARY == 2 /* negative control for the UBSan run: signed overflow */
    { volatile int k = 2147483647; k += (int)n + 1; (void)k; }
#endif
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

/* ---- next and complete (OMEGESE.md section 6) ----
 * The prefix is its complete lines (the head) and the unfinished line: its complete words W and the
 * word U still being written (empty after a space, a bracket, or at a line start). An item (a word,
 * a class of names or numbers, a bracket, a space, a newline, the end of input) is legal when some
 * text through it completes the prefix into a program emit accepts. Each item is decided by a
 * witness: the line is completed in fill mode (the words it still needs; a ret line also reads the
 * names the ended path would leave unread), everything is closed (close_all), and the judge parses
 * that line and those lines again, in normal mode, from the state after the head. The judge is the
 * parser emit runs, so an item printed is always legal; an item is missed only where the witness
 * finds no completion although one exists (the limits in OMEGESE.md section 6). */
typedef __int128 i128;

/* the state after the head; every witness starts from it */
static struct { int nfuncs, in_fn, nregs, nparams, nnames, depth, nconsts, nblocks, norder, cur, nframes, nown, nowq; uint64_t loops; } S0;
static Ow S0q[MAX_OWQ];
static int S0or[MAX_OWN];
static Os S0os[MAX_OWN];
static Func S0f[MAX_FUNCS];
static Ty S0r[MAX_REGS];
static Name S0n[MAX_NAMES];
static Const S0k[MAX_CONSTS];
static Block S0b[MAX_BLOCKS];
static Frame S0fr[MAX_FRAMES];
#define COPY(dst, src, n) memcpy(dst, src, sizeof (src)[0] * (size_t)(n))
static void save(void) {
    S0.nfuncs = nfuncs; S0.in_fn = in_fn; S0.nregs = nregs; S0.nparams = nparams; S0.nnames = nnames;
    S0.depth = depth; S0.nconsts = nconsts; S0.nblocks = nblocks; S0.norder = norder; S0.cur = cur;
    S0.nframes = nframes;
    COPY(S0f, funcs, nfuncs); COPY(S0r, regs, nregs); COPY(S0n, names, nnames);
    COPY(S0k, consts, nconsts); COPY(S0b, blocks, nblocks); COPY(S0fr, frames, nframes);
    S0.nown = nown; S0.loops = g_loops; COPY(S0or, own_reg, nown); COPY(S0os, ost, nown);
    S0.nowq = nowq; COPY(S0q, owq, nowq);
}
static void restore(void) {
    nfuncs = S0.nfuncs; in_fn = S0.in_fn; nregs = S0.nregs; nparams = S0.nparams; nnames = S0.nnames;
    depth = S0.depth; nconsts = S0.nconsts; nblocks = S0.nblocks; norder = S0.norder; cur = S0.cur;
    nframes = S0.nframes;
    COPY(funcs, S0f, nfuncs); COPY(regs, S0r, nregs); COPY(names, S0n, nnames);
    COPY(consts, S0k, nconsts); COPY(blocks, S0b, nblocks); COPY(frames, S0fr, nframes);
    nown = S0.nown; g_loops = S0.loops; own_pk = -1; COPY(own_reg, S0or, nown); COPY(ost, S0os, nown);
    nowq = S0.nowq; COPY(owq, S0q, nowq);
}

static char *W[MAX_WORDS + 1]; /* the complete words of the unfinished line */
static int nW, g_tline;        /* g_tline: its line number */
static const char *U = "";     /* the word being written */
static Buf g_close;            /* the closing lines of the last witness */
static int g_fl, g_phase;      /* its line: g_lw[0..g_fl); g_phase: it failed after the completion began */

/* one more line, parsed in normal mode (its words are kept until the next call) */
static int run_line(const char *text) {
    static char buf[256];
    static char *w[MAX_WORDS + 1];
    snprintf(buf, sizeof buf, "%s", text);
    g_line++;
    int n = split(buf, w);
    if (n <= 0) return n;
    lw = w;
    ln = n;
    lp = 0;
    pdepth = 0;
    return stmt();
}
/* before the end line of the loop at the top: a check waiting for that loop that would fail there gets
 * the line that settles it, when the name is in scope: an array for a name that needs one at the loop
 * head (x = new ...), or a free for one that would still hold its array there */
static int own_repair(char *line, size_t cap) {
    int k = nframes - 1;
    const Frame *fr = &frames[k];
    uint64_t kb = 1ull << k;
    for (int i = 0; i < nowq; i++) {
        const Ow *e = &owq[i];
        if (e->k != k) continue;
        int s = e->s, r = own_reg[s];
        Os o = os_subst(e->o, kb, os_join(fr->oin[s], (Os){ost[s].b, ost[s].m & ~kb}));
        const char *n = own_label(r);
        if (o.m || !n || find_name(n)->reg != r) continue;
        if ((e->kind == OW_USE || e->kind == OW_FREE) && !(o.b & OS_L) && !os_only(ost[s])) {
            snprintf(line, cap, "%s = new %s %d %s", n, GW[regs[r].s], regs[r].len, regs[r].s == T_BOOL ? "false" : "0");
            return 1;
        }
        if ((e->kind == OW_BIRTH || e->kind == OW_RET) && o.b == OS_L && !os_none(ost[s])) {
            snprintf(line, cap, "free %s", n);
            return 1;
        }
    }
    return 0;
}
/* close everything from a line start: each unread name is read (x = x), at the function's end each own
 * array that holds its array on every path is freed (or the path ends with trap 1 when such an array is
 * out of scope), before a loop ends the lines own_repair finds, a function that owes a value returns one,
 * every block ends, and a function is written if there is none */
static int close_all(Buf *txt) {
    for (int guard = 0; guard < 2 * (MAX_NAMES + MAX_FRAMES) + MAX_OWN + MAX_OWQ + 3; guard++) {
        char line[160];
        if (!in_fn) {
            if (nfuncs > 0) break;
            snprintf(line, sizeof line, "fn %s to void", fresh_name());
        } else if (cur >= 0) {
            int u = -1;
            for (int i = 0; i < nnames && u < 0; i++)
                if (!names[i].fixed && !names[i].read && names[i].depth >= depth) u = i;
            int o = -1, hidden = 0; /* at the function's end: an own array that would leak (own_ret) */
            if (u < 0 && frames[nframes - 1].kind == F_FN)
                for (int s = 0; s < nown && o < 0; s++)
                    if (os_only(ost[s])) { if (own_label(own_reg[s])) o = s; else hidden = 1; }
            if (u >= 0) snprintf(line, sizeof line, "%s = %s", names[u].name, names[u].name);
            else if (o >= 0) snprintf(line, sizeof line, "free %s", own_label(own_reg[o]));
            else if (hidden) snprintf(line, sizeof line, "trap 1");
            else if ((frames[nframes - 1].kind == F_WHILE || frames[nframes - 1].kind == F_FOR) && own_repair(line, sizeof line)) {}
            else if (frames[nframes - 1].kind == F_FN && funcs[nfuncs - 1].has_ret)
                snprintf(line, sizeof line, "ret %s", funcs[nfuncs - 1].ret.s == T_BOOL ? "false" : "0");
            else snprintf(line, sizeof line, "end");
        } else snprintf(line, sizeof line, "end");
        TRY(run_line(line));
        bput(txt, "%s\n", line);
    }
    return 0;
}
/* the judge: the completed line (g_lw[0..nw)) and the closing lines, parsed in normal mode from the
 * state after the head, then the checks at the end of input */
static int judge(int nw, const char *closing) {
    restore();
    lw = g_lw;
    ln = nw;
    lp = 0;
    pdepth = 0;
    g_fill = 0;
    g_line = g_tline;
    if (ln > MAX_WORDS) return fail("more than %d words on one line", MAX_WORDS);
    if (ln > 0) TRY(stmt());
    for (const char *p = closing; *p;) {
        const char *e = strchr(p, '\n');
        char line[160];
        snprintf(line, sizeof line, "%.*s", (int)(e - p), p);
        TRY(run_line(line));
        p = e + 1;
    }
    g_line++;
    if (in_fn) return fail("the input ends inside function '%s' (missing end)", funcs[nfuncs - 1].name);
    if (nfuncs == 0) return fail("no function");
    return 0;
}
/* a witness for the line g_lw[0..nw): completed (fill) or as it is, then closed and judged; the one
 * array-name choice of the line is tried in turn (g_pick) */
static int witness(int nw, int fill) {
    int r = -1;
    for (g_pick = 0; g_pick < 64; g_pick++) {
        restore();
        g_an = 0;
        g_gen = -1;
        g_nopt = 0;
        g_ret = 0;
        lw = g_lw;
        ln = nw;
        lp = 0;
        pdepth = 0;
        g_fill = fill;
        g_line = g_tline;
        r = ln > 0 ? stmt() : 0;
        g_fill = 0;
        g_ret = 0;
        g_phase = g_gen >= 0;
        g_fl = ln;
        g_close.n = 0;
        if (g_close.p) g_close.p[0] = 0;
        if (r == 0) {
            g_phase = 1;
            r = close_all(&g_close);
        }
        if (r == 0) r = judge(g_fl, g_close.p ? g_close.p : "");
        if (r == 0 || g_pick + 1 >= g_nopt) break;
    }
    return r;
}
/* is the line W, then a, then b (each may be NULL) legal: completed (fill) or ending there */
static int legal(const char *a, const char *b, int fill) {
    int n = 0;
    for (; n < nW; n++) g_lw[n] = W[n];
    if (a) g_lw[n++] = (char *)a;
    if (b) g_lw[n++] = (char *)b;
    return witness(n, fill) == 0;
}
/* may the input end here: the line as it is, then the checks at the end of input */
static int legal_eof(void) {
    restore();
    int n = 0;
    for (; n < nW; n++) g_lw[n] = W[n];
    if (*U) g_lw[n++] = (char *)U;
    lw = g_lw;
    ln = n;
    lp = 0;
    pdepth = 0;
    g_line = g_tline;
    if (ln > 0 && stmt() < 0) return 0;
    return !in_fn && nfuncs > 0;
}

/* the fixed words, in the order they are printed */
static const char *const KWORDER[] = {"fn", "if", "while", "for", "ret", "put", "free", "trap", "break", "else",
                                      "end", "to", "void", "bound", "=", "true", "false"};
#define NKWO ((int)(sizeof KWORDER / sizeof KWORDER[0]))
#define NFIXED (NKWO + OP_N + T_NSCALAR + T_NSCALAR - 1 + 3)
static const char *fixed_word(int k) {
    if (k < NKWO) return KWORDER[k];
    k -= NKWO;
    if (k < OP_N) return OPW[k];
    k -= OP_N;
    if (k < T_NSCALAR) return GW[k];
    k -= T_NSCALAR;
    if (k < T_NSCALAR - 1) return IRN[k + 1];
    return REFW[k - (T_NSCALAR - 1) + 1];
}

/* the names that are decided one by one: names in scope, functions, and the names on the line. Every
 * other name behaves as a name not in use (the parser only compares names for equality), so one
 * representative decides them all. kind: 0 a name, 1 a function, 2 a name only on this line */
typedef struct { char name[NAME_MAX_LEN + 1]; int kind, legal; } Id;
static Id *ids;
static int nids;
static int find_id(const char *w) {
    for (int i = 0; i < nids; i++)
        if (!strcmp(ids[i].name, w)) return i;
    return -1;
}
static void add_id(const char *w, int kind) {
    if (!valid_ident(w) || reserved(w) || find_id(w) >= 0) return;
    Id *p = realloc(ids, sizeof *ids * (size_t)(nids + 1));
    if (!p) { fputs("omegese: out of memory\n", stderr); exit(2); }
    ids = p;
    snprintf(ids[nids].name, sizeof ids[nids].name, "%s", w);
    ids[nids].kind = kind;
    ids[nids].legal = 0;
    nids++;
}
static int extends(const char *w, const char *u) { return !strncmp(w, u, strlen(u)); }
/* a name not in use that extends u: u itself, or u with _ and a number after it */
static int representative(const char *u, char *rep) {
    if (*u && !valid_ident(u)) return 0;
    for (int k = -2; k < 1000; k++) {
        if (k == -2) { if (!*u) continue; snprintf(rep, NAME_MAX_LEN + 2, "%s", u); }
        else if (k == -1) snprintf(rep, NAME_MAX_LEN + 2, "%s_", *u ? u : "n");
        else snprintf(rep, NAME_MAX_LEN + 2, "%s_%d", *u ? u : "n", k);
        if (strlen(rep) > NAME_MAX_LEN) return 0;
        if (!reserved(rep) && find_id(rep) < 0) return 1;
    }
    return 0;
}

/* numbers: the parser treats a number by comparisons with fixed bounds (type ranges, lengths 1..64,
 * trap 1..14, bound 1..1000000, count 0..1000000) and with the lengths of the arrays in scope, so
 * every number strictly between two neighbouring breakpoints behaves alike */
static const char *const BREAKS[] = {"-18446744073709551615", "-9223372036854775808", "-2147483648", "-32768", "-128",
                                     "0", "1", "14", "64", "127", "255", "32767", "65535", "1000000", "2147483647",
                                     "4294967295", "9223372036854775807", "18446744073709551615"};
#define NBREAKS ((int)(sizeof BREAKS / sizeof BREAKS[0]))
static i128 i128_of(const char *s) {
    int neg = *s == '-';
    i128 v = 0;
    for (s += neg; *s; s++) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}
static const char *str_of(i128 v, char *b) {
    char t[48];
    int k = 0, neg = v < 0;
    unsigned __int128 m = neg ? -(unsigned __int128)v : (unsigned __int128)v;
    do { t[k++] = (char)('0' + (int)(m % 10)); m /= 10; } while (m);
    int j = 0;
    if (neg) b[j++] = '-';
    while (k) b[j++] = t[--k];
    b[j] = 0;
    return b;
}
static int cmp128(const void *a, const void *b) {
    i128 x = *(const i128 *)a, y = *(const i128 *)b;
    return x < y ? -1 : x > y;
}
/* is there a number written as Omegese writes it (no leading zeros, no -0) in lo..hi whose text
 * extends u; *val: one, the shortest */
static int number_extends(const char *u, i128 lo, i128 hi, i128 *val) {
    if (lo > hi) return 0;
    int neg = *u == '-';
    const char *d = u + neg;
    if (!*u) { *val = lo > 0 ? lo : hi < 0 ? hi : 0; return 1; }
    if (!*d) { if (lo > -1) return 0; *val = hi < -1 ? hi : -1; return 1; }
    if (strlen(d) > 20) return 0;
    for (const char *c = d; *c; c++) if (!isdigit((unsigned char)*c)) return 0;
    if (d[0] == '0') { if (d[1] || neg || lo > 0 || hi < 0) return 0; *val = 0; return 1; }
    i128 D = i128_of(d), p = 1;
    for (int k = 0; k <= 20 && D <= (i128)UINT64_MAX / p; k++, p *= 10) {
        i128 a = D * p, b = a + p - 1, x = neg ? -b : a, y = neg ? -a : b;
        i128 l = x > lo ? x : lo, h = y < hi ? y : hi;
        if (l <= h) { *val = neg ? h : l; return 1; }
    }
    return 0;
}

enum { M_HUMAN, M_MACHINE, M_COMPLETE };
static int refuse(void) {
    if (!g_phase) { printf("REFUSED omegese: %s\n", g_err); return 1; }
    const char *m = g_err;
    if (!strncmp(m, "line ", 5) && strstr(m, ": ")) m = strstr(m, ": ") + 2;
    printf("REFUSED omegese: line %d: no text after this prefix makes a program emit accepts (%s)\n", g_tline, m);
    return 1;
}
static void human_line(const char *label, Buf *b) {
    if (b->n) printf("%s:%s\n", label, b->p);
    b->n = 0;
    if (b->p) b->p[0] = 0;
}
static int oracle(char *src, int mode) {
    g_quiet = 1;
    char *nl = strrchr(src, '\n'), *tail = nl ? nl + 1 : src;
    size_t tl = strlen(tail);
    int mid = tl > 0 && !strchr(" \t\r()", tail[tl - 1]);
    char *tb = strdup(tail), *rest = NULL;
    if (!tb) exit(2);
    *tail = 0;
    int rc = 0;
    g_phase = 0;
    if (run(src, 1, &rest) < 0) { rc = refuse(); goto out; }
    g_tline = g_line + 1;
    g_line = g_tline;
    nW = split(tb, W);
    if (nW < 0) { rc = refuse(); goto out; }
    U = mid ? W[--nW] : "";
    save();
    for (int i = 0; i < nnames; i++) add_id(names[i].name, 0);
    for (int g = 0; g < nfuncs; g++) add_id(funcs[g].name, 1);
    for (int i = 0; i < nW; i++) add_id(W[i], 2);

    /* the items */
    char fx[NFIXED] = {0}, rep[NAME_MAX_LEN + 2];
    int any = 0;
    for (int k = 0; k < NFIXED; k++)
        if (extends(fixed_word(k), U)) any |= fx[k] = (char)legal(fixed_word(k), NULL, 1);
    for (int i = 0; i < nids; i++)
        if (extends(ids[i].name, U)) any |= ids[i].legal = legal(ids[i].name, NULL, 1);
    int nn = representative(U, rep) && legal(rep, NULL, 1);
    any |= nn;
    i128 lo[2 * NBREAKS + 2 * 64], hi[2 * NBREAKS + 2 * 64], bp[NBREAKS + 64];
    int niv = 0, nb = 0;
    if (!*U || U[0] == '-' || isdigit((unsigned char)U[0])) {
        for (int k = 0; k < NBREAKS; k++) bp[nb++] = i128_of(BREAKS[k]);
        unsigned long long lens = 0; /* bit L-1: an array of length L is in scope */
        for (int i = 0; i < S0.nnames; i++) { Ty t = S0r[S0n[i].reg]; if (t.ref != R_NONE) lens |= 1ull << (t.len - 1); }
        for (int L = 1; L <= 64; L++) if (lens >> (L - 1) & 1) bp[nb++] = L;
        qsort(bp, (size_t)nb, sizeof bp[0], cmp128);
        int m = 0;
        for (int k = 0; k < nb; k++) if (!m || bp[k] != bp[m - 1]) bp[m++] = bp[k];
        nb = m;
        int open = 0; /* the last interval is still growing */
        for (int k = 0; k < nb; k++)
            for (int gap = 0; gap < 2; gap++) {
                i128 a = gap ? bp[k] + 1 : bp[k], b = gap ? (k + 1 < nb ? bp[k + 1] - 1 : bp[k]) : bp[k];
                if (gap && a > b) continue;
                char num[48];
                if (legal(str_of(a, num), NULL, 1)) {
                    if (open) hi[niv - 1] = b;
                    else { lo[niv] = a; hi[niv] = b; niv++; open = 1; }
                } else open = 0;
            }
        m = 0;
        for (int k = 0; k < niv; k++) {
            i128 v;
            if (number_extends(U, lo[k], hi[k], &v)) { lo[m] = lo[k]; hi[m] = hi[k]; m++; }
        }
        niv = m;
        any |= niv > 0;
    }
    int lpar = *U ? legal(U, "(", 1) : legal("(", NULL, 1);
    int rpar = *U ? legal(U, ")", 1) : legal(")", NULL, 1);
    int newl = *U ? legal(U, NULL, 0) : legal(NULL, NULL, 0);
    int eof = legal_eof();
    any |= lpar | rpar | newl | eof;
    int sp = *U ? legal(U, NULL, 1) : any;
    any |= sp;
    if (!any) {
        /* a defect in the complete words is the prefix's own; otherwise the unfinished word U cannot
         * be finished into anything that leads on */
        if (*U && (legal(NULL, NULL, 1) || g_phase)) { legal(U, NULL, 1); g_phase = 1; }
        else if (!*U) legal(NULL, NULL, 1);
        rc = refuse();
        goto out;
    }

    char num[48], num2[48];
    if (mode == M_MACHINE) {
        for (int k = 0; k < NFIXED; k++) if (fx[k]) printf("word %s\n", fixed_word(k));
        for (int i = 0; i < nids; i++) if (ids[i].legal) printf("word %s\n", ids[i].name);
        if (nn) {
            printf("newname");
            for (int i = 0; i < nids; i++) if (extends(ids[i].name, U)) printf(" %s", ids[i].name);
            printf("\n");
        }
        for (int k = 0; k < niv; k++) printf("number %s %s\n", str_of(lo[k], num), str_of(hi[k], num2));
        if (lpar) printf("(\n");
        if (rpar) printf(")\n");
        if (sp) printf("space\n");
        if (newl) printf("newline\n");
        if (eof) printf("eof\n");
    } else if (mode == M_HUMAN) {
        Buf b = {0};
        int start = !nW && !*U;
        for (int i = 0; i < nids; i++) if (ids[i].legal && ids[i].kind != 1) bput(&b, " %s", ids[i].name);
        human_line(start ? "assign" : "names", &b);
        for (int k = 0; k < niv; k++) bput(&b, "%s %s..%s", k ? "," : "", str_of(lo[k], num), str_of(hi[k], num2));
        human_line("number", &b);
        if (nn) bput(&b, " any name not in use");
        human_line("new-name", &b);
        for (int k = 0; k < NFIXED; k++) if (fx[k]) bput(&b, " %s", fixed_word(k));
        for (int i = 0; i < nids; i++) if (ids[i].legal && ids[i].kind == 1) bput(&b, " %s", ids[i].name);
        if (lpar) bput(&b, " (");
        if (rpar) bput(&b, " )");
        if (sp && *U) bput(&b, " (space)");
        if (newl && !start) bput(&b, " (newline)");
        if (eof) bput(&b, " (end of input)");
        human_line("words", &b);
        free(b.p);
    } else { /* complete: a word that finishes U, then the witness for it */
        if (eof) goto out;
        const char *first = NULL;
        if (*U) {
            if (sp) first = U;
            for (int k = 0; k < NFIXED && !first; k++) if (fx[k]) first = fixed_word(k);
            for (int i = 0; i < nids && !first; i++) if (ids[i].legal) first = ids[i].name;
            if (!first && nn) first = rep;
            for (int k = 0; k < niv && !first; k++) { i128 v; if (number_extends(U, lo[k], hi[k], &v)) first = str_of(v, num); }
            if (!first) { legal(U, NULL, 1); rc = refuse(); goto out; }
        }
        if (!legal(first, NULL, 1)) { rc = refuse(); goto out; }
        if (first) printf("%s", first + strlen(U));
        for (int k = nW + (first ? 1 : 0); k < g_fl; k++) printf(" %s", g_lw[k]);
        if (g_fl > 0) printf("\n");
        printf("%s", g_close.p ? g_close.p : "");
    }
out:
    free(tb);
    free(ids);
    ids = NULL;
    nids = 0;
    return rc;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "words")) {
        for (size_t i = 0; i < NKW; i++) if (strcmp(KW[i], "=")) printf("%s\n", KW[i]);
        for (int i = 0; i < OP_N; i++) printf("%s\n", OPW[i]);
        for (int i = 0; i < T_NSCALAR; i++) printf("%s\n", GW[i]);
        for (int i = 1; i <= 3; i++) printf("%s\n", REFW[i]);
        return 0;
    }
    int machine = argc == 4 && !strcmp(argv[1], "next") && !strcmp(argv[2], "--machine");
    if ((argc == 3 && (!strcmp(argv[1], "next") || !strcmp(argv[1], "complete"))) || machine) {
        size_t len;
        char *src = slurp(argv[argc - 1], &len);
        int rc = oracle(src, machine ? M_MACHINE : !strcmp(argv[1], "next") ? M_HUMAN : M_COMPLETE);
        free(src);
        free(g_close.p);
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
    fprintf(stderr, "usage: omegese emit <in.omegese> <out.json> | omegese next [--machine] <prefix.omegese> | omegese complete <prefix.omegese> | omegese words\n");
    return 2;
}
