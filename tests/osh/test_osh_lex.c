/*
 * test_osh_lex.c -- host driver for the osh lexer unit (src/osh/osh_lex.osc), aien-architecture#158 cut 4a.
 *
 * Compiles osh_lex.osc, then runs lex_run in the reference interpreter AND as native AArch64, with exact-size
 * heap buffers for the input and the workspace (under ASan every out-of-range byte is a report), against:
 *   - the independent C reference tokenizer (src/osh/osh_lex_ref.c), token for token;
 *   - itself, with the input fed in random chunk splits (resumption), whole workspace byte for byte;
 *   - the lexer-level conformance vectors (aien-protocols specs/osh-platform/vectors, vendored in
 *     tests/osh/vectors);
 *   - a table of hostile cases and a fixed-seed byte/fragment fuzz.
 * Usage: test_osh_lex OSH_LEX_OSC VECTORS_DIR [FUZZ_COUNT]
 * Final line: OSH_LEX_PASS or OSH_LEX_FAIL.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_interp.h"
#include "osc_native.h"
#include "osc_rt.h"
#include "osh_layout.h"
#include "osh_lex_ref.h"

#define WS ((size_t)OSH_WS_CELLS)

static unsigned long n_checks, n_fail;
static unsigned long n_interp_calls, n_native_calls;
static unsigned long n_cases, n_ref_cmp, n_ref_tok, n_chunk_runs, n_engine_cmp, n_traps;
static unsigned long n_vec, n_vec_lex, n_hostile, n_fuzz, n_abi, n_bvec;
static int EOI; /* sessions are created with S_EOI = this */
static unsigned long n_status[4]; /* fuzz outcomes: done, more, refused, other */
static unsigned long n_code[256];

static void check(int cond, const char *fmt, ...)
{
    n_checks++;
    if (cond) return;
    n_fail++;
    if (n_fail <= 40) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}

/* ---- the unit ---- */
static OscUnit *U;
static OscRt *RI, *RN;
static OscCode CODE;
static OscNative NM;
static int FI = -1;
static void *ENTRY;

typedef enum { INTERP = 0, NATIVE = 1 } Engine;

/* a session: an exact-size workspace */
typedef struct {
    uint64_t *w;
} Sess;

static void sess_init(Sess *s)
{
    s->w = calloc(WS, sizeof(uint64_t));
    if (!s->w) { fprintf(stderr, "oom\n"); exit(2); }
    s->w[OSH_S_MAGIC] = OSH_ABI_MAGIC;
    s->w[OSH_S_VERSION] = OSH_ABI_VERSION;
    s->w[OSH_S_WS_CELLS] = OSH_WS_CELLS;
    s->w[OSH_S_EOI] = (uint64_t)EOI;
}

static void sess_free(Sess *s) { free(s->w); s->w = NULL; }

/* one lex_run call on an exact-size copy of data[0..len): the return value, or ~0 on a trap */
static uint64_t call_raw(Engine e, uint64_t *w, size_t wn, const uint8_t *data, size_t len)
{
    uint8_t *b = NULL;
    if (len) {
        b = malloc(len);
        if (!b) { fprintf(stderr, "oom\n"); exit(2); }
        memcpy(b, data, len);
    }
    uint64_t args[4] = { (uint64_t)(uintptr_t)b, len, (uint64_t)(uintptr_t)w, wn };
    uint64_t ret = 0;
    int trap;
    check(osc_ir_slice_args_ok(&U->funcs[FI], args, 4) == 0, "host entry refused valid buffers");
    if (e == INTERP) {
        osc_rt_reset(RI);
        trap = osc_interp_run_prevalidated(U, FI, args, 4, RI, &ret);
        n_interp_calls++;
    } else {
        osc_rt_reset(RN);
        trap = osc_native_call(U, FI, &NM, CODE.entry[FI], RN, args, 4, &ret);
        n_native_calls++;
    }
    free(b);
    if (trap != 0) {
        n_traps++;
        check(0, "%s trap %d (len %zu)", e == INTERP ? "interp" : "native", trap, len);
        return ~0ULL;
    }
    return ret;
}

static uint64_t call(Engine e, Sess *s, const uint8_t *data, size_t len)
{
    return call_raw(e, s->w, WS, data, len);
}

/* run to completion over the whole buffer (101 repeats with no new input); returns the final status */
static uint64_t lex_all(Engine e, Sess *s, const uint8_t *d, size_t n, unsigned *resumes)
{
    unsigned r = 0;
    for (;;) {
        uint64_t st = call(e, s, d, n);
        if (st == 101) {
            r++;
            if (r > 8) { check(0, "more than 8 budget resumes for %zu bytes", n); break; }
            continue;
        }
        if (resumes) *resumes = r;
        return st;
    }
    if (resumes) *resumes = r;
    return ~0ULL;
}

/* feed d[0..n) in the given chunk pattern: mode 0 one byte, 1 random 1..16, 2 random up to the rest.
 * Returns the final status (the status of the last call). */
static uint64_t lex_chunked(Engine e, Sess *s, const uint8_t *d, size_t n, unsigned mode, uint64_t *rng,
                            uint64_t (*rnd)(uint64_t *))
{
    size_t cur = 0;
    uint64_t st = 0;
    if (n == 0) return lex_all(e, s, d, 0, NULL);
    while (cur < n) {
        size_t ch = 1;
        if (mode == 1) ch = 1 + rnd(rng) % 16;
        else if (mode == 2) ch = 1 + rnd(rng) % (n - cur);
        if (ch > n - cur) ch = n - cur;
        cur += ch;
        st = lex_all(e, s, d, cur, NULL);
        n_chunk_runs++;
        if (st >= 200) break; /* terminal: the host stops feeding */
    }
    return st;
}

static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ---- comparison against the reference ---- */
static void cmp_ref(const char *tag, const uint8_t *d, size_t n, const Sess *s, uint64_t st)
{
    OshRefLex *r = malloc(sizeof *r);
    osh_lex_ref_eoi(d, n, EOI, r);
    n_ref_cmp++;
    const uint64_t *w = s->w;
    int ok = 1;
    if (st != r->status) ok = 0;
    if (w[OSH_LX_NTOK] != r->ntok) ok = 0;
    if (r->status >= 200) {
        if (w[OSH_S_ERR_CODE] != r->status || w[OSH_S_ERR_OFF] != r->err_off || w[OSH_S_PHASE] != 255) ok = 0;
    } else if (w[OSH_S_PHASE] != 0 || (w[OSH_S_CURSOR] != n && !(st == 100 && n > 0 && d[n - 1] == 0x5c && w[OSH_S_CURSOR] == n - 1))) {
        ok = 0;
    }
    unsigned m = r->ntok < w[OSH_LX_NTOK] ? r->ntok : (unsigned)w[OSH_LX_NTOK];
    if (m > OSH_REF_TOKEN_CAP) m = OSH_REF_TOKEN_CAP;
    for (unsigned t = 0; t < m && ok; t++) {
        const uint64_t *c = &w[OSH_TOKENS + 4 * t];
        const OshRefTok *k = &r->tok[t];
        n_ref_tok++;
        uint64_t hdr = k->kind | (k->flags << 8) | (k->nseg << 24) | (k->line << 32);
        if (c[0] != hdr || c[1] != k->start || c[2] != k->len || c[3] != k->aux) ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "  ref mismatch [%s] len %zu: osc status %llu ntok %llu err %llu@%llu | ref status %u ntok %u err@%llu\n  input:",
                tag, n, (unsigned long long)st, (unsigned long long)w[OSH_LX_NTOK], (unsigned long long)w[OSH_S_ERR_CODE],
                (unsigned long long)w[OSH_S_ERR_OFF], r->status, r->ntok, (unsigned long long)r->err_off);
        for (size_t q = 0; q < n && q < 100; q++) fprintf(stderr, " %02x", d[q]);
        fprintf(stderr, "\n");
        for (unsigned t = 0; t < m; t++) {
            const uint64_t *c = &w[OSH_TOKENS + 4 * t];
            const OshRefTok *k = &r->tok[t];
            uint64_t hdr = k->kind | (k->flags << 8) | (k->nseg << 24) | (k->line << 32);
            if (c[0] != hdr || c[1] != k->start || c[2] != k->len || c[3] != k->aux) {
                fprintf(stderr, "  tok %u osc %llx %llu %llu %llu | ref %llx %llu %llu %llu\n", t, (unsigned long long)c[0],
                        (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3], (unsigned long long)hdr,
                        (unsigned long long)k->start, (unsigned long long)k->len, (unsigned long long)k->aux);
                break;
            }
        }
    }
    check(ok, "reference mismatch [%s]", tag);
    free(r);
}

/* One input through everything. Returns the single-shot interpreter status; sess_out receives its workspace
 * when non-NULL (caller frees). */
typedef struct {
    uint64_t status;
    unsigned resumes;
    uint64_t err_off;
    uint64_t ntok;
    uint64_t w[OSH_LX_MID + 1];
} Outcome;

static void run_all(const char *tag, const uint8_t *d, size_t n, uint64_t *rng, unsigned long iter, Outcome *out)
{
    Sess a, b, c, e;
    unsigned ra = 0, rb = 0;
    sess_init(&a);
    sess_init(&b);
    uint64_t sa = lex_all(INTERP, &a, d, n, &ra);
    uint64_t sb = lex_all(NATIVE, &b, d, n, &rb);
    n_cases++;
    n_engine_cmp++;
    check(sa == sb && ra == rb && memcmp(a.w, b.w, WS * 8) == 0, "[%s] interp and native disagree (status %llu vs %llu, resumes %u vs %u, workspace %s)", tag,
          (unsigned long long)sa, (unsigned long long)sb, ra, rb, memcmp(a.w, b.w, WS * 8) == 0 ? "same" : "differs");
    cmp_ref(tag, d, n, &a, sa);
    /* idempotence of a settled call */
    if (sa == 0 || sa == 100 || sa >= 200) {
        uint64_t sn = call(INTERP, &a, d, n);
        uint64_t *snap = malloc(WS * 8);
        memcpy(snap, a.w, WS * 8);
        uint64_t sn2 = call(INTERP, &a, d, n);
        check(sn == sa && sn2 == sa && memcmp(snap, a.w, WS * 8) == 0, "[%s] repeated call changed the outcome", tag);
        free(snap);
    }
    /* chunked resumption, alternating the engine */
    unsigned mode = (unsigned)(rng_next(rng) % 3);
    if (mode == 0 && n > 48) mode = 1;
    Engine ce = (iter & 1) ? INTERP : NATIVE;
    sess_init(&c);
    uint64_t sc = lex_chunked(ce, &c, d, n, mode, rng, rng_next);
    check(sc == sa && memcmp(a.w, c.w, WS * 8) == 0, "[%s] chunked (mode %u) run differs from single shot (status %llu vs %llu)", tag, mode,
          (unsigned long long)sc, (unsigned long long)sa);
    if ((iter & 7) == 0) {
        sess_init(&e);
        uint64_t se = lex_chunked(ce == INTERP ? NATIVE : INTERP, &e, d, n, 1, rng, rng_next);
        check(se == sa && memcmp(a.w, e.w, WS * 8) == 0, "[%s] chunked run on the other engine differs", tag);
        sess_free(&e);
    }
    if (out) {
        out->status = sa;
        out->resumes = ra;
        out->err_off = a.w[OSH_S_ERR_OFF];
        out->ntok = a.w[OSH_LX_NTOK];
        memcpy(out->w, a.w, sizeof out->w);
    }
    sess_free(&a);
    sess_free(&b);
    sess_free(&c);
}

/* ---- hostile cases ---- */
typedef struct {
    const char *name;
    const char *in;
    size_t len;
    uint64_t status;
    uint64_t off;   /* refusals only */
    int ntok;       /* -1 = do not check */
} Hostile;

#define H(name, s, status, off, ntok) { name, s, sizeof(s) - 1, status, off, ntok }

static const Hostile HOSTILE[] = {
    H("empty", "", 0, 0, 0),
    H("only newline", "\n", 0, 0, 1),
    H("only spaces and newline", "   \t \n", 0, 0, 1),
    H("only spaces no newline", "   ", 100, 0, 0),
    H("one word no newline", "ls", 100, 0, 0),
    H("simple", "ls -l /tmp\n", 0, 0, 4),
    H("unterminated double quote", "echo \"abc", 100, 0, 1),
    H("unterminated double quote then newline", "echo \"abc\n", 100, 0, 1),
    H("unterminated single quote", "echo 'abc\n", 100, 0, -1),
    H("open brace parameter", "echo ${A", 100, 0, -1),
    H("dollar at end", "echo $", 100, 0, -1),
    H("operator at end", "a |", 100, 0, -1),
    H("backslash at end", "echo a\\", 100, 0, -1),
    H("backslash newline pending", "echo a\\\n", 100, 0, -1),
    H("backslash newline at command start", "\\\n", 100, 0, 0),
    H("continuation joins a word", "echo a\\\nb\n", 0, 0, 3),
    H("continuation between words", "echo \\\n-l\n", 0, 0, 3),
    H("comment", "ls # comment $(x) `y`\n", 0, 0, 2),
    H("comment without newline", "ls # c", 100, 0, -1),
    H("hash inside a word", "echo a#b\n", 0, 0, 3),
    H("quoted newline", "echo 'a\nb'\n", 0, 0, 3),
    H("invalid utf-8 passes through", "echo \xff\xfe\x80\xc0\n", 0, 0, 3),
    H("cr is an ordinary byte", "echo a\r\n", 0, 0, 3),
    H("glob inside double quotes", "echo \"*.c\"\n", 0, 0, 3),
    H("glob inside single quotes", "echo '*.c' '?' '['\n", 0, 0, 5),
    H("tilde inside a word", "echo a~b\n", 0, 0, 3),
    H("tilde in quotes", "echo \"~\" '~'\n", 0, 0, 4),
    H("dollar before blank", "echo $ x\n", 0, 0, 4),
    H("dollar in double quotes at end of quote", "echo \"$\"\n", 0, 0, 3),
    H("brace name", "echo \"a${B}c\" $1 $10 $_a $# $@ $* $?\n", 0, 0, 10),
    H("dollar underscore is the last-argument parameter", "echo $_\n", 225, 6, -1),
    H("dollar underscore name goes on", "echo $_9 $__ ${_a} ${_}x\n", 225, 21, -1),
    H("append assignment", "echo x+=1\n", 248, 7, -1),
    H("plus not after a name", "echo 1+=1 +=1 a-b+=1 x++=1 x+y=1\n", 0, 0, 7),
    H("assignment words", "x=1 y=\"2 3\"\n", 0, 0, 3),
    H("every operator", "a|b||c&&d;e<f>g>>h>&i<&j\n", 0, 0, 20),
    H("io numbers", "ls nofile >o 2>&1\n", 0, 0, 7),
    H("io number input and append", "cat 0<f 1>>g\n", 0, 0, 6),
    H("digit word with space is an argument", "echo 2 >f\n", 0, 0, 5),
    H("digit word touching a pipe is a word", "echo 2|cat\n", 0, 0, 5),
    H("name digit word touching redirect", "echo a2>f\n", 0, 0, 5),
    H("4096 byte line", "", 0, 0, -1), /* replaced at run time */
    H("backtick", "echo `x`\n", 221, 5, -1),
    H("backtick in double quotes", "echo \"`x`\"\n", 221, 6, -1),
    H("command substitution", "echo $(x)\n", 226, 6, -1),
    H("command substitution in double quotes", "echo \"x $(y)\"\n", 226, 9, -1),
    H("arithmetic", "echo $((1+2))\n", 226, 6, -1),
    H("glob star", "echo *.c\n", 222, 5, -1),
    H("glob question", "echo a?b\n", 222, 6, -1),
    H("glob bracket", "echo [a]\n", 222, 5, -1),
    H("leading tilde", "~/x\n", 223, 0, -1),
    H("tilde after redirect", "cat >~/x\n", 223, 5, -1),
    H("tilde after assignment equals", "A=~/x\n", 223, 2, -1),
    H("tilde after assignment colon", "A=b:~\n", 223, 4, -1),
    H("parameter operator", "echo ${A:-b}\n", 224, 8, -1),
    H("empty braces", "echo ${}\n", 224, 7, -1),
    H("braces newline", "echo ${A\n", 224, 8, -1),
    H("special parameter dollar", "echo $$\n", 225, 6, -1),
    H("special parameter bang", "echo $!\n", 225, 6, -1),
    H("special parameter dash", "echo $-\n", 225, 6, -1),
    H("ansi-c quoting", "echo $'x'\n", 225, 6, -1),
    H("locale quoting", "echo $\"x\"\n", 225, 6, -1),
    H("background", "a & b\n", 227, 2, -1),
    H("background at end of line", "a &\n", 227, 2, -1),
    H("ampersand redirect", "a &> f\n", 227, 2, -1),
    H("pipe ampersand", "a |& b\n", 227, 3, -1),
    H("subshell open", "(a)\n", 228, 0, -1),
    H("subshell close", "a )\n", 228, 2, -1),
    H("process substitution", "cat <(x)\n", 228, 5, -1),
    H("here document", "cat <<E\n", 229, 5, -1),
    H("here string", "cat <<<x\n", 229, 5, -1),
    H("case end", "a;;b\n", 230, 2, -1),
    H("clobber redirect", "a >| f\n", 231, 3, -1),
    H("read write redirect", "a <> f\n", 231, 3, -1),
    H("fd range single digit", "echo 5>f\n", 232, 6, -1),
    H("fd range two digits", "echo 12>f\n", 232, 7, -1),
    H("fd range input", "echo 3<f\n", 232, 6, -1),
    H("nul at start", "\0", 220, 0, -1),
    H("nul in word", "echo a\0b\n", 220, 6, -1),
    H("nul in single quotes", "echo 'a\0b'\n", 220, 7, -1),
    H("nul in double quotes", "echo \"a\0b\"\n", 220, 7, -1),
    H("nul in comment", "# c\0\n", 220, 3, -1),
    H("nul after backslash", "a\\\0", 220, 2, -1),
    H("nul after operator", "a |\0", 220, 3, -1),
    H("nul after dollar", "echo $\0", 220, 6, -1),
    H("nul in braces", "echo ${A\0}\n", 220, 8, -1),
    H("nul after word before newline", "echo a\0\n", 220, 6, -1),
    H("brace expansion list", "echo {a,b}\n", 246, 9, -1),
    H("brace expansion sequence", "echo {1..3}\n", 246, 10, -1),
    H("brace expansion char sequence", "echo {a..c}\n", 246, 10, -1),
    H("brace expansion with prefix and suffix", "echo a{b,c}d\n", 246, 10, -1),
    H("brace expansion empty items", "echo {,}\n", 246, 7, -1),
    H("brace expansion after assignment equals", "echo x={a,b}\n", 246, 11, -1),
    H("brace expansion with a quoted item", "echo {a,\"b\"}\n", 246, 11, -1),
    H("brace expansion nested outer", "echo {{a},b}\n", 246, 11, -1),
    H("brace expansion nested inner", "echo {{a,b}\n", 246, 10, -1),
    H("brace expansion at command start", "{a,b}\n", 246, 4, -1),
    H("brace expansion is decided before a later glob", "echo {a,b}*\n", 246, 9, -1),
    H("glob before a brace expansion", "echo *{a,b}\n", 222, 5, -1),
    H("lone open brace stays literal", "echo {\n", 0, 0, 3),
    H("lone close brace stays literal", "echo }\n", 0, 0, 3),
    H("braces around a blank stay literal", "echo { } {} }{\n", 0, 0, 6),
    H("unclosed brace with comma stays literal", "echo {a,b\n", 0, 0, 3),
    H("closing brace without opening stays literal", "echo a,b}\n", 0, 0, 3),
    H("braces without comma or dots stay literal", "echo {a} {a.b} {1.2} a{b}c\n", 0, 0, 6),
    H("quoted brace expansion stays literal", "echo '{a,b}' \"{a,b}\" {\"a,b\"} {'1..3'}\n", 0, 0, 6),
    H("escaped brace expansion stays literal", "echo \\{a,b\\} {a\\,b} {1.\\.3}\n", 0, 0, 5),
    H("parameter braces are not brace expansion", "echo ${A},{b} ${A}{b}\n", 0, 0, 4),
    H("dollar before a brace pair", "echo $,{b}\n", 0, 0, 3),
    H("lone group open at command start", "{ echo hi; }\n", 236, 0, -1),
    H("lone group close after semicolon", "echo hi; }\n", 236, 9, -1),
    H("group after and", "a && { b; }\n", 236, 5, -1),
    H("group after pipe", "a | }\n", 236, 4, -1),
    H("group after newline", "a\n{\n", 236, 2, -1),
    H("group with continuation", "{\\\n x\n", 236, 0, -1),
    H("group as an argument is a word", "echo {\n", 0, 0, 3),
    H("group after an assignment is a word", "A=1 {\n", 0, 0, 3),
    H("group-like word", "{a }b\n", 0, 0, 3),
    H("quoted group is a word", "'{' x\n", 0, 0, 3),
    H("escaped group is a word", "\\{ x\n", 0, 0, 3),
    H("group open pending", "{", 100, 0, -1),
    H("group before subshell paren", "{(\n", 236, 0, -1),
    H("positional ten", "echo ${10}\n", 247, 8, -1),
    H("positional double zero", "echo ${00}\n", 247, 8, -1),
    H("positional in double quotes", "echo \"${10}\"\n", 247, 9, -1),
    H("positional decided at the second digit", "echo ${12", 247, 8, -1),
    H("positional digit then letter", "echo ${1a}\n", 224, 8, -1),
    H("positional single digits", "echo ${1}${0}${9}\n", 0, 0, 3),
    H("name with digits", "echo ${a1} ${a12} ${_9}\n", 0, 0, 5),
};

static void run_hostile(uint64_t *rng)
{
    for (size_t h = 0; h < sizeof HOSTILE / sizeof HOSTILE[0]; h++) {
        const Hostile *c = &HOSTILE[h];
        const uint8_t *d = (const uint8_t *)c->in;
        size_t n = c->len;
        uint8_t *big = NULL;
        uint64_t status = c->status, off = c->off;
        int ntok = c->ntok;
        if (strcmp(c->name, "4096 byte line") == 0) {
            n = 4096;
            big = malloc(n);
            memset(big, 'a', n);
            memcpy(big, "echo ", 5);
            big[n - 1] = '\n';
            d = big;
            status = 0;
            ntok = 3;
        }
        Outcome o;
        run_all(c->name, d, n, rng, h, &o);
        n_hostile++;
        check(o.status == status, "hostile '%s': status %llu, want %llu", c->name, (unsigned long long)o.status, (unsigned long long)status);
        if (status >= 200) check(o.err_off == off, "hostile '%s': offset %llu, want %llu", c->name, (unsigned long long)o.err_off, (unsigned long long)off);
        if (ntok >= 0 && status < 200) check(o.ntok == (uint64_t)ntok, "hostile '%s': %llu tokens, want %d", c->name, (unsigned long long)o.ntok, ntok);
        free(big);
    }
    /* sizes around the line cap and the token cap */
    for (int variant = 0; variant < 12; variant++) {
        size_t n = 0;
        uint8_t *b = NULL;
        uint64_t want = 0, want_off = 0;
        const char *nm = "";
        switch (variant) {
        case 0: nm = "4097 byte line"; n = 4097; b = malloc(n); memset(b, 'a', n); b[n - 1] = '\n'; want = 201; want_off = 4096; break;
        case 1: nm = "5000 byte word"; n = 5000; b = malloc(n); memset(b, 'a', n); want = 201; want_off = 4096; break;
        case 2: nm = "4096 bytes no newline"; n = 4096; b = malloc(n); memset(b, 'a', n); want = 100; break;
        case 3: nm = "4096 spaces then more"; n = 4097; b = malloc(n); memset(b, ' ', n); want = 201; want_off = 4096; break;
        case 4: nm = "refusal before the line cap wins"; n = 6000; b = malloc(n); memset(b, 'a', n); memcpy(b + 10, "$(", 2); want = 226; want_off = 11; break;
        case 5: nm = "127 words and newline"; n = 254; b = malloc(n); for (size_t q = 0; q < 126; q++) { b[2 * q] = 'a'; b[2 * q + 1] = ' '; } b[252] = 'a'; b[253] = '\n'; want = 0; break;
        case 6: nm = "128 words and newline (129 tokens)"; n = 128 * 2 + 1; b = malloc(n); for (size_t q = 0; q < 128; q++) { b[2 * q] = 'a'; b[2 * q + 1] = ' '; } b[256] = '\n'; want = 202; want_off = 256; break;
        case 7: nm = "129th word"; n = 129 * 2 + 1; b = malloc(n); for (size_t q = 0; q < 129; q++) { b[2 * q] = 'a'; b[2 * q + 1] = ' '; } b[258] = '\n'; want = 202; want_off = 256; break;
        case 8: nm = "64 words and semicolons"; n = 129; b = malloc(n); for (size_t q = 0; q < 64; q++) { b[2 * q] = 'a'; b[2 * q + 1] = ';'; } b[128] = '\n'; want = 202; want_off = 128; break;
        case 9: nm = "129 newlines"; n = 129; b = malloc(n); memset(b, '\n', n); want = 202; want_off = 128; break;
        case 10: nm = "digit word merge at full table"; n = 127 * 2 + 3; b = malloc(n); for (size_t q = 0; q < 127; q++) { b[2 * q] = 'a'; b[2 * q + 1] = ' '; } memcpy(b + 254, "2>\n", 3); want = 202; want_off = 256; break;
        default: nm = "many redirects"; n = 3 * 40 + 1; b = malloc(n); for (size_t q = 0; q < 40; q++) memcpy(b + 3 * q, "2>f", 3); b[3 * 40] = '\n'; want = 0; break;
        }
        /* variant 8: semicolons are tokens too: token 129 is the 129th byte (index 128)? 128 tokens fill the table, the NL is the 129th */
        if (variant == 8) want_off = 128;
        if (variant == 10) { want = 0; } /* 127 words + "2>" merges into the 128th slot? see below */
        Outcome o;
        run_all(nm, b, n, rng, 100 + (unsigned long)variant, &o);
        n_hostile++;
        if (variant == 10) {
            /* 127 words, then `2` is word 128, `>` merges into it, NL is token 129 */
            want = 202;
            want_off = 256;
        }
        check(o.status == want, "hostile '%s': status %llu, want %llu", nm, (unsigned long long)o.status, (unsigned long long)want);
        if (want >= 200) check(o.err_off == want_off, "hostile '%s': offset %llu, want %llu", nm, (unsigned long long)o.err_off, (unsigned long long)want_off);
        free(b);
    }
}

static void run_brace_depth(uint64_t *rng)
{
    /* 62 nested braces with no comma are literal; the 63rd open brace is refused (the detector's depth cap) */
    for (int open = 61; open <= 64; open++) {
        uint8_t b[256];
        size_t n = 0;
        memcpy(b, "echo ", 5); n = 5;
        for (int q = 0; q < open; q++) b[n++] = '{';
        if (open <= 62) for (int q = 0; q < open; q++) b[n++] = '}';
        b[n++] = '\n';
        Outcome o;
        run_all("brace depth", b, n, rng, 300 + (unsigned long)open, &o);
        n_hostile++;
        if (open <= 62) check(o.status == 0, "brace depth %d: status %llu", open, (unsigned long long)o.status);
        else check(o.status == 246 && o.err_off == 67, "brace depth %d: %llu@%llu", open, (unsigned long long)o.status, (unsigned long long)o.err_off);
    }
}

/* ---- ABI header refusals ---- */
static void run_abi(void)
{
    const uint8_t in[] = "ls\n";
    static const struct { const char *name; size_t cells; int what; uint64_t want; } T[] = {
        { "no header", 0, 0, 216 },
        { "one cell", 1, 0, 216 },
        { "bad magic", OSH_WS_CELLS, 1, 214 },
        { "bad version", OSH_WS_CELLS, 2, 215 },
        { "short workspace", OSH_WS_CELLS - 1, 0, 213 },
        { "tiny workspace", 2, 0, 213 },
        { "reserved cell 48", OSH_WS_CELLS, 3, 217 },
        { "reserved cell 63", OSH_WS_CELLS, 4, 217 },
        { "good", OSH_WS_CELLS, 0, 0 },
        { "bigger workspace is fine", OSH_WS_CELLS + 5, 0, 0 },
    };
    for (size_t t = 0; t < sizeof T / sizeof T[0]; t++) {
        for (int e = 0; e < 2; e++) {
            size_t nc = T[t].cells;
            uint64_t *w = NULL;
            if (nc) {
                w = calloc(nc, 8);
                if (nc > 0) w[0] = OSH_ABI_MAGIC;
                if (nc > 1) w[1] = OSH_ABI_VERSION;
                if (T[t].what == 1) w[0] ^= 1;
                if (T[t].what == 2) w[1] = 2;
                if (T[t].what == 3) w[48] = 1;
                if (T[t].what == 4) w[63] = 7;
            }
            uint64_t *snap = nc ? malloc(nc * 8) : NULL;
            if (nc) memcpy(snap, w, nc * 8);
            uint64_t st = call_raw(e ? NATIVE : INTERP, w, nc, in, sizeof in - 1);
            check(st == T[t].want, "abi '%s': status %llu, want %llu", T[t].name, (unsigned long long)st, (unsigned long long)T[t].want);
            if (T[t].want >= 213 && T[t].want <= 217 && nc) check(memcmp(snap, w, nc * 8) == 0, "abi '%s': wrote the workspace", T[t].name);
            n_abi++;
            free(w);
            free(snap);
        }
    }
    /* a consumed cursor past the buffer (the host shrank the input) is ABI_LENGTH, nothing written */
    Sess s;
    sess_init(&s);
    s.w[OSH_S_CURSOR] = 9;
    check(call(INTERP, &s, in, 3) == 216 && s.w[OSH_S_PHASE] == 0, "abi: shrunk input");
    sess_free(&s);
    /* a refusal is terminal and repeats */
    sess_init(&s);
    const uint8_t bad[] = "echo $(x)\n";
    check(lex_all(INTERP, &s, bad, sizeof bad - 1, NULL) == 226, "terminal: first");
    check(call(NATIVE, &s, bad, sizeof bad - 1) == 226 && call(INTERP, &s, (const uint8_t *)"", 0) == 226, "terminal: repeat");
    sess_free(&s);
    /* a corrupt lexer state byte is refused, not trapped */
    sess_init(&s);
    s.w[OSH_LX_STATE] = 77;
    check(call(INTERP, &s, in, 3) == 217, "corrupt state is refused");
    sess_free(&s);
}

/* ---- conformance vectors (lexer level) ---- */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    for (;;) {
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (n < cap) break;
        cap *= 2;
        b = realloc(b, cap);
    }
    fclose(f);
    *len = n;
    return b;
}

static void run_vectors(const char *dir, uint64_t *rng)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/expected.txt", dir);
    size_t el;
    char *exp = slurp(path, &el);
    check(exp != NULL, "cannot read %s", path);
    if (!exp) return;
    exp = realloc(exp, el + 1);
    exp[el] = 0;
    for (char *line = strtok(exp, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] != 'v') continue;
        char name[16];
        if (sscanf(line, "%15s", name) != 1) continue;
        snprintf(path, sizeof path, "%s/%s.in", dir, name);
        size_t n;
        char *in = slurp(path, &n);
        check(in != NULL, "cannot read %s", path);
        if (!in) continue;
        Outcome o;
        run_all(name, (const uint8_t *)in, n, rng, n_vec, &o);
        n_vec++;
        /* what the lexer owns: refusal codes 201..232; every other line means the lexer finishes the buffer */
        unsigned code = 0, off = 0;
        unsigned lex_resumes = 0;
        int want_resumes = -1;
        const char *p = strstr(line, " refuse ");
        if (p) {
            if (sscanf(p, " refuse %u %*s off=%u", &code, &off) != 2) check(0, "%s: bad refuse line", name);
        }
        if ((p = strstr(line, "lex_resumes="))) { lex_resumes = (unsigned)strtoul(p + 12, NULL, 10); want_resumes = (int)lex_resumes; }
        if (strstr(line, " more")) {
            check(o.status == 100, "vector %s: status %llu, want 100", name, (unsigned long long)o.status);
            n_vec_lex++;
        } else if ((code >= 201 && code <= 232) || code == 236 || code == 246 || code == 247 || code == 248) {
            check(o.status == code && o.err_off == off, "vector %s: refusal %llu@%llu, want %u@%u", name, (unsigned long long)o.status,
                  (unsigned long long)o.err_off, code, off);
            n_vec_lex++;
        } else {
            check(o.status == 0, "vector %s: status %llu, want 0 (lexer done)", name, (unsigned long long)o.status);
            n_vec_lex++;
        }
        if (want_resumes >= 0) check(o.resumes == (unsigned)want_resumes, "vector %s: %u budget resumes, want %d", name, o.resumes, want_resumes);
        free(in);
    }
    free(exp);
    check(n_vec >= 20, "only %lu conformance vectors ran (an empty or missing list must fail)", n_vec);
}


/* ---- bash-verified vectors (tests/osh/vectors/bash_vectors.tsv; bash_verify.sh proves them against real bash) ---- */
/* decode \n \t \\ \xHH; returns the length */
static size_t unesc(const char *s, size_t sl, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < sl; i++) {
        if (s[i] != '\\' || i + 1 >= sl) { out[n++] = s[i]; continue; }
        i++;
        if (s[i] == 'n') out[n++] = '\n';
        else if (s[i] == 't') out[n++] = '\t';
        else if (s[i] == 'x' && i + 2 < sl + 0) { out[n++] = (char)strtoul((char[]){ s[i + 1], s[i + 2], 0 }, NULL, 16); i += 2; }
        else out[n++] = s[i];
    }
    return n;
}

/* append the escaped form of byte c (the canonical spelling used in the expect column) */
static void esc_put(char *o, size_t *n, uint8_t c)
{
    if (c == '\\') { o[(*n)++] = '\\'; o[(*n)++] = '\\'; }
    else if (c == '\n') { o[(*n)++] = '\\'; o[(*n)++] = 'n'; }
    else if (c == '\t') { o[(*n)++] = '\\'; o[(*n)++] = 't'; }
    else if (c <= 0x20 || c >= 0x7f) *n += (size_t)sprintf(o + *n, "\\x%02x", c);
    else o[(*n)++] = (char)c;
}

/* the bash view of one word: backslash-newline removed, quotes removed */
static void word_text(const uint8_t *in, size_t s, size_t e, char *o, size_t *n)
{
    size_t i = s;
    while (i < e) {
        uint8_t c = in[i];
        if (c == '\\' && i + 1 < e) {
            if (in[i + 1] == '\n') { i += 2; continue; }
            esc_put(o, n, in[i + 1]);
            i += 2;
        } else if (c == '\'') {
            i++;
            while (i < e && in[i] != '\'') esc_put(o, n, in[i++]);
            i++;
        } else if (c == '"') {
            i++;
            while (i < e && in[i] != '"') {
                if (in[i] == '\\' && i + 1 < e) {
                    uint8_t d = in[i + 1];
                    if (d == '\n') { i += 2; continue; }
                    if (d == '\\' || d == '"' || d == '$' || d == '`') { esc_put(o, n, d); i += 2; continue; }
                }
                esc_put(o, n, in[i++]);
            }
            i++;
        } else {
            esc_put(o, n, c);
            i++;
        }
    }
}

/* the lexer result of d as the expect-column text: "ok <tokens>", "more" or "refuse CODE@OFF" */
static void render(const uint8_t *d, size_t n, char *o, size_t ocap)
{
    Sess s;
    sess_init(&s);
    uint64_t st = lex_all(INTERP, &s, d, n, NULL);
    size_t k = 0;
    static const char *const NAME[] = { "", "W", "PIPE", "OR", "AND", "SEMI", "LT", "GT", "APPEND", "NL", "DUPO", "DUPI" };
    if (st >= 200) k = (size_t)sprintf(o, "refuse %llu@%llu", (unsigned long long)st, (unsigned long long)s.w[OSH_S_ERR_OFF]);
    else if (st == 100) k = (size_t)sprintf(o, "more");
    else {
        k = (size_t)sprintf(o, "ok");
        for (uint64_t t = 0; t < s.w[OSH_LX_NTOK] && k + 2048 < ocap; t++) {
            const uint64_t *c = &s.w[OSH_TOKENS + 4 * t];
            unsigned kind = (unsigned)(c[0] & 255);
            o[k++] = ' ';
            if (kind == 1) {
                o[k++] = 'W'; o[k++] = ':';
                word_text(d, c[1], c[1] + c[2], o, &k);
            } else if (kind >= 2 && kind <= 11) {
                k += (size_t)sprintf(o + k, "%s", NAME[kind]);
                if (kind == 6 || kind == 7 || kind == 8 || kind == 10 || kind == 11) k += (size_t)sprintf(o + k, ":%llu", (unsigned long long)c[3]);
            } else k += (size_t)sprintf(o + k, "?%u", kind);
        }
    }
    o[k] = 0;
    sess_free(&s);
}

static void run_bash_vectors(const char *dir, uint64_t *rng)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/bash_vectors.tsv", dir);
    size_t el;
    char *txt = slurp(path, &el);
    check(txt != NULL, "cannot read %s", path);
    if (!txt) return;
    txt = realloc(txt, el + 1);
    txt[el] = 0;
    char *sp = NULL;
    for (char *line = strtok_r(txt, "\n", &sp); line; line = strtok_r(NULL, "\n", &sp)) {
        if (line[0] != 'c') continue;
        char *f[4] = { 0 };
        char *p = line;
        for (int q = 0; q < 4; q++) {
            f[q] = p;
            char *t = strchr(p, '\t');
            if (!t) { if (q < 3) check(0, "bash vector line has %d fields: %.20s", q + 1, line); break; }
            *t = 0;
            p = t + 1;
        }
        if (!f[3]) continue;
        char *inb = malloc(strlen(f[1]) + 1), *eqb = malloc(strlen(f[3]) + 1);
        size_t inl = unesc(f[1], strlen(f[1]), inb);
        char got[8192], want[8192];
        /* the expect column keeps its own escapes, so compare against the raw text */
        snprintf(want, sizeof want, "%s", f[2]);
        Outcome o;
        run_all(f[0], (const uint8_t *)inb, inl, rng, n_bvec, &o);
        render((const uint8_t *)inb, inl, got, sizeof got);
        n_bvec++;
        check(strcmp(got, want) == 0, "bash vector %s: got '%s', want '%s'", f[0], got, want);
        /* the continuation-free form lexes the same way (offsets of a refusal may differ) */
        if (strcmp(f[3], "-") != 0) {
            size_t eql = unesc(f[3], strlen(f[3]), eqb);
            char got2[8192];
            run_all(f[0], (const uint8_t *)eqb, eql, rng, n_bvec, NULL);
            render((const uint8_t *)eqb, eql, got2, sizeof got2);
            if (strncmp(want, "refuse ", 7) == 0) {
                char *at = strchr(want, '@');
                check(at && strncmp(got2, want, (size_t)(at - want)) == 0, "bash vector %s: equiv script gave '%s', want the code of '%s'", f[0], got2, want);
            } else {
                check(strcmp(got2, want) == 0, "bash vector %s: equiv script gave '%s', want '%s'", f[0], got2, want);
            }
        }
        free(inb);
        free(eqb);
    }
    free(txt);
}
/* ---- end of input flag (S_EOI): a trailing backslash is a literal; without it the lexer waits for the next byte ---- */
static void run_eoi(uint64_t *rng)
{
    static const struct { const char *in; size_t n; uint64_t ntok_wait, ntok_eoi, state_wait, state_eoi; } T[] = {
        { "echo a|\\", 8, 2, 3, 11, 1 },   /* `|` then `\`: waiting keeps the operator pending; EOI ends it and starts a word */
        { "\\", 1, 0, 0, 0, 1 },            /* a lone `\`: waiting leaves the start state; EOI starts a word */
        { "echo \"a\\", 8, 1, 1, 3, 3 },    /* inside double quotes both stay in the quote */
        { "echo a\\", 7, 1, 1, 4, 1 },      /* the word already started */
    };
    for (size_t t = 0; t < sizeof T / sizeof T[0]; t++) {
        for (int eoi = 0; eoi <= 1; eoi++) {
            EOI = eoi;
            Outcome o;
            run_all("eoi", (const uint8_t *)T[t].in, T[t].n, rng, 700 + t, &o);
            n_hostile++;
            check(o.status == 100, "eoi %d case %zu: status %llu, want 100", eoi, t, (unsigned long long)o.status);
            check(o.ntok == (eoi ? T[t].ntok_eoi : T[t].ntok_wait), "eoi %d case %zu: %llu tokens", eoi, t, (unsigned long long)o.ntok);
            (void)T[t].state_wait; (void)T[t].state_eoi;
        }
    }
    EOI = 0;
}

/* ---- fuzz ---- */
static const char *FRAG[] = {
    "ls", "echo", " ", " ", "  ", "\t", "\n", "\n", "-l", "/tmp", "a", "b", "x=1", "A=", "_n", "0", "1", "2", "3", "10", "12",
    "'x y'", "\"a b\"", "\"$B c\"", "\"$(x)\"", "'", "\"", "\\", "\\\n", "\\\"", "\\$", "$", "$1", "$?", "$#", "$@", "$*", "$$", "$!", "$-",
    "${V}", "${", "}", "${A:-b}", "$(", "$((", ")", "(", "`", "*", "?", "[", "]", "~", "~/x", "{a,b}", "!", "#", "# c\n", "a#b",
    "|", "||", "&&", "&", ";", ";;", "<", ">", ">>", ">&", "<&", "<<", "<<<", ">|", "<>", "&>", "|&", "2>", "2>&1", "1>>", "0<", "5>", "12>", "<(", ">(",
    "=", ":", ":~", "\r", "\xff", "\x80", "\xc3\xa9", "\xe2\x82", "\x01", "\x7f",
    "|\\\n", "2\\\n", "$\\\n", ">\\\n", "&\\\n", "x+=1", "x+", "$_", "${_}", "_", "+", "\\\n#", "~\\\n~",
    "{1..3}", "${10}", "${1a}", "${0}", "${a1}", "{ }", "{}", ",", ".", "..", "{x}", "a{b,c}d", "x={a,b}", "\\{", "\\,", "{\\\n",
};

/* fragments that mostly lex clean, so the token records get exercised */
static const char *SAFE[] = {
    "ls", "echo", " ", " ", "  ", "\t", "\n", "-l", "/tmp", "a", "x=1", "A=", "_n", "0", "1", "2", "10", "{", "}", "{x}", ",", ".", "a.b", "${0}", "\\{", "'x y'", "\"a b\"", "\"$B c\"",
    "\\\n", "|\\\n", "2\\\n", ">\\\n", "\\\"", "$1", "$?", "$#", "$@", "${V}", "#", "# c\n", "a#b", "|", "||", "&&", ";", "<", ">", ">>", ">&", "<&", "2>", "2>&1", "1>>", "0<", "=", ":", "\xff", "\xc3\xa9",
};

static size_t gen(uint8_t *buf, size_t cap, uint64_t *rng)
{
    size_t n = 0;
    unsigned mode = (unsigned)(rng_next(rng) % 8);
    if (mode == 0) { /* raw bytes, any value, NUL included */
        size_t len = rng_next(rng) % 40;
        for (size_t i = 0; i < len; i++) buf[n++] = (uint8_t)rng_next(rng);
    } else if (mode == 1) { /* bytes from a hostile alphabet */
        static const char AL[] = "ab 1 2\t\n'\"\\$`|&;<>()#{}*?[]~=:!-_\r\x80\xff.,@%^";
        size_t len = rng_next(rng) % 48;
        for (size_t i = 0; i < len; i++) buf[n++] = (uint8_t)AL[rng_next(rng) % (sizeof AL - 1)];
    } else if (mode == 2 || mode == 3) { /* clean fragments */
        size_t nf = 1 + rng_next(rng) % 30;
        for (size_t i = 0; i < nf; i++) {
            const char *f = SAFE[rng_next(rng) % (sizeof SAFE / sizeof SAFE[0])];
            size_t fl = strlen(f);
            memcpy(buf + n, f, fl);
            n += fl;
        }
        if (rng_next(rng) % 4 != 0) buf[n++] = '\n';
    } else { /* fragments */
        size_t nf = 1 + rng_next(rng) % (mode == 7 ? 120 : 14);
        for (size_t i = 0; i < nf; i++) {
            const char *f = FRAG[rng_next(rng) % (sizeof FRAG / sizeof FRAG[0])];
            size_t fl = strlen(f);
            if (n + fl > cap) break;
            memcpy(buf + n, f, fl);
            n += fl;
        }
        if (rng_next(rng) % 4 != 0 && n < cap) buf[n++] = '\n';
        if (rng_next(rng) % 6 == 0 && n) buf[rng_next(rng) % n] = (uint8_t)rng_next(rng); /* one byte mutation, NUL possible */
        if (rng_next(rng) % 40 == 0) { /* a very long line */
            size_t extra = 4000 + rng_next(rng) % 300;
            if (n + extra < cap) {
                memset(buf + n, 'a' + (int)(rng_next(rng) % 3), extra);
                n += extra;
                buf[n++] = '\n';
            }
        }
    }
    return n;
}

static void run_fuzz(unsigned long count, uint64_t *rng)
{
    uint8_t *buf = malloc(8192);
    for (unsigned long i = 0; i < count; i++) {
        size_t n = gen(buf, 8000, rng);
        Outcome o;
        run_all("fuzz", buf, n, rng, i, &o);
        n_fuzz++;
        if (o.status == 0) n_status[0]++;
        else if (o.status == 100) n_status[1]++;
        else if (o.status >= 200) { n_status[2]++; n_code[o.status & 255]++; }
        else n_status[3]++;
    }
    free(buf);
}

/* ---- unit statistics ---- */
static void unit_stats(void)
{
    unsigned maxv = 0, maxi = 0;
    const char *vn = "", *in = "";
    for (int f = 0; f < U->nfuncs; f++) {
        const OscFunc *fn = &U->funcs[f];
        printf("fn %-10s params=%u vregs=%u/%u insns=%u/%u\n", fn->name, fn->nparams, fn->nvregs, OSC_MAX_VREGS, fn->ninsns, OSC_MAX_INSNS);
        if (fn->nvregs > maxv) { maxv = fn->nvregs; vn = fn->name; }
        if (fn->ninsns > maxi) { maxi = fn->ninsns; in = fn->name; }
        check(fn->nparams <= OSC_MAX_PARAMS, "%s has %u param registers", fn->name, fn->nparams);
    }
    printf("unit: %u functions (max %u); biggest vregs %s %u (headroom %u of %u); biggest insns %s %u (headroom %u of %u)\n", U->nfuncs,
           OSC_MAX_FUNCS, vn, maxv, OSC_MAX_VREGS - maxv, OSC_MAX_VREGS, in, maxi, OSC_MAX_INSNS - maxi, OSC_MAX_INSNS);
    check(U->nfuncs <= OSC_MAX_FUNCS, "too many functions");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: test_osh_lex OSH_LEX_OSC VECTORS_DIR [FUZZ_COUNT]\n");
        return 2;
    }
    unsigned long fuzz = argc > 3 ? strtoul(argv[3], NULL, 10) : 100000;
    /* the generated layout agrees with the numbers the ABI draft fixes (section 6.1, 6.2) */
    check(OSH_WS_CELLS == 11456 && OSH_TOKENS == 64 && OSH_CMDS == 576 && OSH_PIPES == 832 && OSH_STAGING == 896 && OSH_REQUEST == 1928 &&
              OSH_OUT == 3264 && OSH_ABI_MAGIC == 0x4F534857ULL && OSH_S_ERR_CODE == 4 && OSH_S_ERR_OFF == 5 && OSH_S_CURSOR == 6 &&
              OSH_S_RESERVED == 48 && OSH_TOKEN_CAP == 128 && OSH_LINE_CAP == 4096 && OSH_LEX_BUDGET == 1024,
          "generated layout differs from the ABI draft numbers");
    size_t sl;
    char *src = slurp(argv[1], &sl);
    if (!src) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    U = calloc(1, sizeof *U);
    RI = calloc(1, sizeof *RI);
    RN = calloc(1, sizeof *RN);
    OscDiag d;
    if (osc_compile(src, sl, U, &d, NULL) != 0) {
        fprintf(stderr, "osh_lex.osc refused: %s line %u: %s\n", osc_diag_kind_name(d.kind), d.line, d.message);
        return 1;
    }
    char err[256];
    check(osc_ir_validate(U, err, sizeof err) == 0, "validate: %s", err);
    for (int f = 0; f < U->nfuncs; f++)
        if (strcmp(U->funcs[f].name, "lex_run") == 0) FI = f;
    check(FI >= 0, "no lex_run");
    memset(&CODE, 0, sizeof CODE);
    int cg = osc_cg_compile(U, &CODE, err, sizeof err);
    check(cg == 0, "codegen: %s", err);
    if (cg || FI < 0) { printf("OSH_LEX_FAIL\n"); return 1; }
    int nm = osc_native_map(&NM, CODE.code, CODE.len);
    check(nm == 0, "native map %d", nm);
    if (nm) { printf("OSH_LEX_FAIL\n"); return 1; }
    ENTRY = osc_native_at(&NM, CODE.entry[FI]);
    osc_rt_init(RI);
    osc_rt_init(RN);
    unit_stats();

    uint64_t seed = 0x05a1e0c0ffee0004ULL;
    const char *se = getenv("OSH_FUZZ_SEED");
    if (se && *se) seed = strtoull(se, NULL, 0);
    if (seed == 0) seed = 1;
    printf("fuzz seed: 0x%016llx (override with OSH_FUZZ_SEED)\n", (unsigned long long)seed);
    uint64_t rng = seed;
    run_abi();
    run_hostile(&rng);
    run_brace_depth(&rng);
    run_vectors(argv[2], &rng);
    run_bash_vectors(argv[2], &rng);
    check(n_bvec >= 40, "only %lu bash-verified vectors ran, want at least 40", n_bvec);
    run_eoi(&rng);
    run_fuzz(fuzz, &rng);

    printf("abi header cases: %lu\n", n_abi);
    printf("hostile cases: %lu (each: interp, native, reference, chunked)\n", n_hostile);
    printf("vectors: %lu run, %lu lexer-level checks; bash-verified vectors: %lu\n", n_vec, n_vec_lex, n_bvec);
    printf("fuzz: %lu inputs (seed above): done=%lu more=%lu refused=%lu\n", n_fuzz, n_status[0], n_status[1], n_status[2]);
    printf("refusal codes seen in fuzz:");
    for (int c = 0; c < 256; c++) if (n_code[c]) printf(" %d:%lu", c, n_code[c]);
    printf("\n");
    printf("cases %lu; interp calls %lu; native calls %lu; interp==native workspace compares %lu; reference compares %lu (%lu tokens); chunked calls %lu; traps %lu\n",
           n_cases, n_interp_calls, n_native_calls, n_engine_cmp, n_ref_cmp, n_ref_tok, n_chunk_runs, n_traps);
    printf("checks %lu, failures %lu\n", n_checks, n_fail);
    osc_native_unmap(&NM);
    osc_cg_free(&CODE);
    free(src);
    int ok = n_fail == 0;
    printf(ok ? "OSH_LEX_PASS\n" : "OSH_LEX_FAIL\n");
    return ok ? 0 : 1;
}
