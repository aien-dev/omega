/*
 * test_osh_parse.c -- host driver for the osh parser unit (src/osh/osh_parse.osc), aien-architecture#158 cut 4b.
 *
 * Compiles osh_lex.osc and osh_parse.osc, then runs the host sequence (lex_run until done, then parse_run) in the
 * reference interpreter AND as native AArch64, with exact-size heap buffers (under ASan every out-of-range byte is a
 * report), against:
 *   - the independent C reference tokenizer and parser (osh_lex_ref.c, osh_parse_ref.c), table for table;
 *   - itself, with the input fed in random chunk splits (resumption of both units), tables byte for byte;
 *   - the conformance vectors (aien-protocols specs/osh-platform/vectors, vendored in tests/osh/vectors, plus the
 *     local parser vectors in tests/osh/parse_vectors) at parser level;
 *   - a table of hostile cases and a fixed-seed fragment fuzz, each with the end-of-input flag clear and set.
 * Usage: test_osh_parse OSH_LEX_OSC OSH_PARSE_OSC VECTORS_DIR PARSE_VECTORS_DIR [FUZZ_COUNT]
 * Final line: OSH_PARSE_PASS or OSH_PARSE_FAIL.
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
#include "osh_parse_ref.h"

#define WS ((size_t)OSH_WS_CELLS)

static unsigned long n_checks, n_fail;
static unsigned long n_cases, n_ref_cmp, n_chunk_runs, n_engine_cmp, n_traps, n_calls_i, n_calls_n;
static unsigned long n_vec, n_vec_parse, n_hostile, n_fuzz, n_idem;
static unsigned long n_outcome[4]; /* fuzz: list done, need more, lexer refusal, parser refusal */
static unsigned long n_code[256];
static unsigned long n_cmds_cmp, n_pipes_cmp;

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

/* ---- the two units ---- */
typedef struct {
    const char *entry;
    OscUnit *U;
    OscRt *RI, *RN;
    OscCode code;
    OscNative nm;
    int fi;
} Unit;

typedef enum { INTERP = 0, NATIVE = 1 } Engine;

static Unit LEX = { .entry = "lex_run" }, PARSE = { .entry = "parse_run" };

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

static int unit_load(Unit *u, const char *path, char **srcp)
{
    size_t sl;
    char *src = slurp(path, &sl);
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    u->U = calloc(1, sizeof *u->U);
    u->RI = calloc(1, sizeof *u->RI);
    u->RN = calloc(1, sizeof *u->RN);
    OscDiag d;
    if (osc_compile(src, sl, u->U, &d, NULL) != 0) {
        fprintf(stderr, "%s refused: %s line %u: %s\n", path, osc_diag_kind_name(d.kind), d.line, d.message);
        return 1;
    }
    char err[256];
    check(osc_ir_validate(u->U, err, sizeof err) == 0, "validate %s: %s", path, err);
    u->fi = -1;
    for (int f = 0; f < u->U->nfuncs; f++)
        if (strcmp(u->U->funcs[f].name, u->entry) == 0) u->fi = f;
    check(u->fi >= 0, "no %s", u->entry);
    memset(&u->code, 0, sizeof u->code);
    int cg = osc_cg_compile(u->U, &u->code, err, sizeof err);
    check(cg == 0, "codegen %s: %s", path, err);
    if (cg || u->fi < 0) return 1;
    int nm = osc_native_map(&u->nm, u->code.code, u->code.len);
    check(nm == 0, "native map %d", nm);
    if (nm) return 1;
    osc_rt_init(u->RI);
    osc_rt_init(u->RN);
    *srcp = src;
    return 0;
}

static void unit_stats(const char *label, const Unit *u)
{
    unsigned maxv = 0, maxi = 0;
    const char *vn = "", *in = "";
    for (int f = 0; f < u->U->nfuncs; f++) {
        const OscFunc *fn = &u->U->funcs[f];
        printf("%s fn %-10s params=%u vregs=%u/%u insns=%u/%u\n", label, fn->name, fn->nparams, fn->nvregs, OSC_MAX_VREGS, fn->ninsns, OSC_MAX_INSNS);
        if (fn->nvregs > maxv) { maxv = fn->nvregs; vn = fn->name; }
        if (fn->ninsns > maxi) { maxi = fn->ninsns; in = fn->name; }
        check(fn->nparams <= OSC_MAX_PARAMS, "%s has %u param registers", fn->name, fn->nparams);
    }
    printf("%s unit: %u functions (max %u); biggest vregs %s %u (headroom %u of %u); biggest insns %s %u (headroom %u of %u)\n", label, u->U->nfuncs,
           OSC_MAX_FUNCS, vn, maxv, OSC_MAX_VREGS - maxv, OSC_MAX_VREGS, in, maxi, OSC_MAX_INSNS - maxi, OSC_MAX_INSNS);
    check(u->U->nfuncs <= OSC_MAX_FUNCS, "too many functions");
}

/* one entry call on an exact-size copy of data[0..len): the return value, or ~0 on a trap */
static uint64_t ucall(Unit *u, Engine e, uint64_t *w, size_t wn, const uint8_t *data, size_t len)
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
    check(osc_ir_slice_args_ok(&u->U->funcs[u->fi], args, 4) == 0, "host entry refused valid buffers");
    if (e == INTERP) {
        osc_rt_reset(u->RI);
        trap = osc_interp_run_prevalidated(u->U, u->fi, args, 4, u->RI, &ret);
        n_calls_i++;
    } else {
        osc_rt_reset(u->RN);
        trap = osc_native_call(u->U, u->fi, &u->nm, u->code.entry[u->fi], u->RN, args, 4, &ret);
        n_calls_n++;
    }
    free(b);
    if (trap != 0) {
        n_traps++;
        check(0, "%s %s trap %d (len %zu)", u->entry, e == INTERP ? "interp" : "native", trap, len);
        return ~0ULL;
    }
    return ret;
}

static uint64_t *sess_new(void)
{
    uint64_t *w = calloc(WS, sizeof(uint64_t));
    if (!w) { fprintf(stderr, "oom\n"); exit(2); }
    w[OSH_S_MAGIC] = OSH_ABI_MAGIC;
    w[OSH_S_VERSION] = OSH_ABI_VERSION;
    w[OSH_S_WS_CELLS] = OSH_WS_CELLS;
    return w;
}

/* run an entry, repeating on 101 (budget) with no new input */
static uint64_t run_entry(Unit *u, Engine e, uint64_t *w, const uint8_t *d, size_t n)
{
    for (int r = 0; r < 12; r++) {
        uint64_t st = ucall(u, e, w, WS, d, n);
        if (st != 101) return st;
    }
    check(0, "%s: more than 12 budget resumes for %zu bytes", u->entry, n);
    return ~0ULL;
}

/* the host sequence over d[0..n): lex until it settles, then parse. Returns the final status; *from_lexer says which. */
static uint64_t host_run(Engine e, uint64_t *w, const uint8_t *d, size_t n, int *from_lexer)
{
    uint64_t st = run_entry(&LEX, e, w, d, n);
    *from_lexer = 1;
    if (st != 0) return st;
    *from_lexer = 0;
    return run_entry(&PARSE, e, w, d, n);
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

/* feed d[0..n) as the host would: random chunks (mode 0 one byte, 1 random 1..16, 2 random up to the rest); after
 * each chunk, if the lexer is done run the parser; stop when the list is complete or a refusal comes. Then, if eoi,
 * set the flag and run both again. Returns the final status. */
static uint64_t host_chunked(Engine e, uint64_t *w, const uint8_t *d, size_t n, int eoi, unsigned mode, uint64_t *rng, int *from_lexer)
{
    size_t cur = 0;
    uint64_t st = 0;
    *from_lexer = 1;
    if (n == 0) return host_run(e, w, d, 0, from_lexer);
    while (cur < n) {
        size_t ch = 1;
        if (mode == 1) ch = 1 + rng_next(rng) % 16;
        else if (mode == 2) ch = 1 + rng_next(rng) % (n - cur);
        if (ch > n - cur) ch = n - cur;
        cur += ch;
        st = host_run(e, w, d, cur, from_lexer);
        n_chunk_runs++;
        if (st >= 200) return st;
        if (st == 0 && !*from_lexer) return st; /* the list is complete: the host would start a new session */
    }
    if (eoi) {
        w[OSH_S_EOI] = 1;
        st = host_run(e, w, d, n, from_lexer);
    }
    return st;
}

/* ---- comparison against the references ---- */
static void dump_input(const uint8_t *d, size_t n)
{
    fprintf(stderr, "  input:");
    for (size_t q = 0; q < n && q < 100; q++) fprintf(stderr, " %02x", d[q]);
    fprintf(stderr, "\n");
}

/* the workspace after the host sequence vs the reference outcome */
static void cmp_ref(const char *tag, const uint8_t *d, size_t n, int eoi, const uint64_t *w, uint64_t st)
{
    OshRefLex *lx = malloc(sizeof *lx);
    OshRefParse *pr = calloc(1, sizeof *pr);
    osh_lex_ref(d, n, eoi, lx);
    int ok = 1;
    uint64_t want = lx->status, woff = lx->err_off;
    if (lx->status == 0) {
        osh_parse_ref(d, n, lx, eoi, pr);
        want = pr->status;
        woff = pr->err_off;
    }
    n_ref_cmp++;
    if (st != want) ok = 0;
    if (want >= 200) {
        if (w[OSH_S_ERR_CODE] != want || w[OSH_S_ERR_OFF] != woff || w[OSH_S_PHASE] != 255) ok = 0;
    } else if (lx->status == 0) {
        /* parser outcome 0 or 100: the tables written so far, and for 0 the cursor */
        if (w[OSH_PX_NCMD] != pr->ncmd || w[OSH_PX_NPIPE] != pr->npipe) ok = 0;
        for (unsigned c = 0; c < pr->ncmd && ok; c++)
            for (int j = 0; j < 8; j++) {
                n_cmds_cmp++;
                if (w[OSH_CMDS + 8 * c + j] != pr->cmd[c][j]) ok = 0;
            }
        for (unsigned p = 0; p < pr->npipe && ok; p++)
            for (int j = 0; j < 4; j++) {
                n_pipes_cmp++;
                if (w[OSH_PIPES + 4 * p + j] != pr->pipe[p][j]) ok = 0;
            }
        if (want == 0 && w[OSH_PX_TOK] != pr->next_tok) ok = 0;
        if (w[OSH_PX_STATE] > 4) ok = 0;
    }
    /* the parser never writes the staging, request or out regions */
    for (size_t q = OSH_STAGING; q < OSH_STAGING + 64 && ok; q++)
        if (w[q] != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "  ref mismatch [%s] eoi=%d len %zu: osc status %llu err %llu@%llu ncmd %llu npipe %llu tok %llu | ref lex %u parse %u err@%llu ncmd %u npipe %u next %u\n",
                tag, eoi, n, (unsigned long long)st, (unsigned long long)w[OSH_S_ERR_CODE], (unsigned long long)w[OSH_S_ERR_OFF],
                (unsigned long long)w[OSH_PX_NCMD], (unsigned long long)w[OSH_PX_NPIPE], (unsigned long long)w[OSH_PX_TOK], lx->status,
                pr->status, (unsigned long long)woff, pr->ncmd, pr->npipe, pr->next_tok);
        dump_input(d, n);
        for (unsigned c = 0; c < pr->ncmd && lx->status == 0; c++) {
            int diff = 0;
            for (int j = 0; j < 8; j++) if (w[OSH_CMDS + 8 * c + j] != pr->cmd[c][j]) diff = 1;
            if (diff) {
                fprintf(stderr, "  cmd %u osc:", c);
                for (int j = 0; j < 8; j++) fprintf(stderr, " %llu", (unsigned long long)w[OSH_CMDS + 8 * c + j]);
                fprintf(stderr, " | ref:");
                for (int j = 0; j < 8; j++) fprintf(stderr, " %llu", (unsigned long long)pr->cmd[c][j]);
                fprintf(stderr, "\n");
                break;
            }
        }
    }
    check(ok, "reference mismatch [%s] eoi=%d", tag, eoi);
    free(lx);
    free(pr);
}

typedef struct {
    uint64_t status;
    int from_lexer;
    uint64_t err_off;
    uint64_t npipe, ncmd;
} Outcome;

/* One input, one eoi setting, through everything. */
static void run_one(const char *tag, const uint8_t *d, size_t n, int eoi, uint64_t *rng, unsigned long iter, Outcome *out)
{
    uint64_t *a = sess_new(), *b = sess_new(), *c = sess_new();
    int fla, flb, flc;
    a[OSH_S_EOI] = (uint64_t)eoi;
    b[OSH_S_EOI] = (uint64_t)eoi;
    uint64_t sa = host_run(INTERP, a, d, n, &fla);
    uint64_t sb = host_run(NATIVE, b, d, n, &flb);
    n_cases++;
    n_engine_cmp++;
    int same = memcmp(a, b, WS * 8) == 0;
    check(sa == sb && fla == flb && same, "[%s] eoi=%d interp and native disagree (status %llu vs %llu, workspace %s)", tag, eoi,
          (unsigned long long)sa, (unsigned long long)sb, same ? "same" : "differs");
    cmp_ref(tag, d, n, eoi, a, sa);
    /* idempotence of a settled call */
    if (sa == 0 || sa == 100 || sa >= 200) {
        uint64_t *snap = malloc(WS * 8);
        memcpy(snap, a, WS * 8);
        uint64_t s1 = fla ? ucall(&LEX, INTERP, a, WS, d, n) : ucall(&PARSE, INTERP, a, WS, d, n);
        uint64_t s2 = fla ? ucall(&LEX, NATIVE, a, WS, d, n) : ucall(&PARSE, NATIVE, a, WS, d, n);
        n_idem++;
        /* a settled lexer call may rewrite its done flag identically; the whole workspace must be unchanged */
        check(s1 == sa && s2 == sa && memcmp(snap, a, WS * 8) == 0, "[%s] eoi=%d repeated call changed the outcome (%llu %llu vs %llu)", tag, eoi,
              (unsigned long long)s1, (unsigned long long)s2, (unsigned long long)sa);
        free(snap);
    }
    /* chunked resumption, alternating the engine; only when the whole buffer lexes clean (otherwise the host may
     * legitimately stop at a list end before it reaches a later lexer refusal) */
    OshRefLex *lx = malloc(sizeof *lx);
    osh_lex_ref(d, n, eoi, lx);
    int lex_clean = lx->status == 0;
    free(lx);
    if (lex_clean) {
        unsigned mode = (unsigned)(rng_next(rng) % 3);
        if (mode == 0 && n > 48) mode = 1;
        Engine ce = (iter & 1) ? INTERP : NATIVE;
        uint64_t sc = host_chunked(ce, c, d, n, eoi, mode, rng, &flc);
        cmp_ref(tag, d, n, eoi, c, sc);
        check(sc == sa, "[%s] eoi=%d chunked (mode %u) status %llu differs from single shot %llu", tag, eoi, mode, (unsigned long long)sc,
              (unsigned long long)sa);
        if ((iter & 7) == 0) {
            uint64_t *e2 = sess_new();
            int fle;
            uint64_t se = host_chunked(ce == INTERP ? NATIVE : INTERP, e2, d, n, eoi, 1, rng, &fle);
            check(se == sa, "[%s] eoi=%d chunked run on the other engine differs", tag, eoi);
            cmp_ref(tag, d, n, eoi, e2, se);
            free(e2);
        }
    }
    if (out) {
        out->status = sa;
        out->from_lexer = fla;
        out->err_off = a[OSH_S_ERR_OFF];
        out->npipe = a[OSH_PX_NPIPE];
        out->ncmd = a[OSH_PX_NCMD];
    }
    free(a);
    free(b);
    free(c);
}

static void run_all(const char *tag, const uint8_t *d, size_t n, uint64_t *rng, unsigned long iter, Outcome *o0, Outcome *o1)
{
    run_one(tag, d, n, 0, rng, iter, o0);
    run_one(tag, d, n, 1, rng, iter, o1);
}

/* ---- hostile cases ---- */
typedef struct {
    const char *name;
    const char *in;
    size_t len;
    int eoi;
    uint64_t status;
    uint64_t off;  /* refusals only */
    int npipe;     /* -1 = do not check */
    int ncmd;
} Hostile;

#define H(name, s, eoi, status, off, np, nc) { name, s, sizeof(s) - 1, eoi, status, off, np, nc }

static const Hostile HOSTILE[] = {
    H("simple", "ls -l /tmp\n", 0, 0, 0, 1, 1),
    H("empty buffer", "", 0, 0, 0, 0, 0),
    H("empty buffer eoi", "", 1, 0, 0, 0, 0),
    H("blank line", "\n", 0, 0, 0, 0, 0),
    H("blank lines: the first is a complete empty list", "\n\n  \nls\n", 0, 0, 0, 0, 0),
    H("comment only", "# note\n", 0, 0, 0, 0, 0),
    H("list", "a && b || c; d | e\n", 0, 0, 0, 4, 5),
    H("trailing semicolon", "a;\n", 0, 0, 0, 1, 1),
    H("trailing semicolon with blank", "a ;  \n", 0, 0, 0, 1, 1),
    H("semicolon then command", "a; b\n", 0, 0, 0, 2, 2),
    H("end of input after semicolon", "a;", 1, 0, 0, 1, 1),
    H("end of input after a command", "ls", 1, 0, 0, 1, 1),
    H("no newline without the flag is lexer pending", "ls", 0, 100, 0, -1, -1),
    H("assignments only", "x=1 y=2\n", 0, 0, 0, 1, 1),
    H("redirection only", "> f\n", 0, 0, 0, 1, 1),
    H("redirections interleaved with words", "cat <in a >out b 2>&1 c\n", 0, 0, 0, 1, 1),
    H("append and input", "a >>o <i\n", 0, 0, 0, 1, 1),
    H("pipeline of eight", "a|a|a|a|a|a|a|a\n", 0, 0, 0, 1, 8),
    H("pipeline of nine", "a|a|a|a|a|a|a|a|a\n", 0, 205, 16, -1, -1),
    H("assignment after a word is a word", "A=1 cmd B=2\n", 0, 0, 0, 1, 1),
    H("equals word is a word", "=x y\n", 0, 0, 0, 1, 1),
    H("leading semicolon", ";a\n", 0, 241, 0, -1, -1),
    H("semicolon after semicolon", "a; ;b\n", 0, 241, 3, -1, -1),
    H("leading pipe", "|a\n", 0, 241, 0, -1, -1),
    H("leading and", "&&a\n", 0, 241, 0, -1, -1),
    H("leading or", "|| a\n", 0, 241, 0, -1, -1),
    H("pipe pipe", "a | | b\n", 0, 241, 4, -1, -1),
    H("pipe semicolon", "a |;\n", 0, 241, 3, -1, -1),
    H("and semicolon", "a && ; b\n", 0, 241, 5, -1, -1),
    H("semicolon and", "a ; && b\n", 0, 241, 4, -1, -1),
    H("or pipe", "a || | b\n", 0, 241, 5, -1, -1),
    H("trailing and", "a &&\n", 0, 100, 0, 1, 1),
    H("trailing or", "a ||\n", 0, 100, 0, 1, 1),
    H("trailing pipe", "a |\n", 0, 100, 0, 1, 1),
    H("trailing and at end of input", "a &&\n", 1, 248, 5, -1, -1),
    H("trailing pipe at end of input", "a |\n", 1, 248, 4, -1, -1),
    H("trailing pipe pending operator at end of input", "a |", 1, 248, 3, -1, -1),
    H("continuation after and", "a &&\n\n b\n", 0, 0, 0, 2, 2),
    H("continuation after pipe", "a |\nb\n", 0, 0, 0, 1, 2),
    H("continuation after or then comment line", "a ||\n# c\nb\n", 0, 0, 0, 2, 2),
    H("newline after semicolon ends the list", "a;\nb\n", 0, 0, 0, 1, 1),
    H("two lines: the second is left", "a\nb\n", 0, 0, 0, 1, 1),
    H("redirect without target", "cat >\n", 0, 242, 5, -1, -1),
    H("redirect target is an operator", "cat > | x\n", 0, 242, 6, -1, -1),
    H("append target is a semicolon", "cat >>;\n", 0, 242, 6, -1, -1),
    H("input target missing", "cat <\n", 0, 242, 5, -1, -1),
    H("dup target missing", "cat <&\n", 0, 242, 6, -1, -1),
    H("redirect target missing at end of input", "cat >", 1, 242, 5, -1, -1),
    H("redirect target missing pending without flag", "cat >", 0, 100, 0, -1, -1),
    H("redirect target missing after a word end of input", "cat > f >", 1, 242, 9, -1, -1),
    H("dup ok 2>&1", "a 2>&1\n", 0, 0, 0, 1, 1),
    H("dup ok >&2", "a >&2\n", 0, 0, 0, 1, 1),
    H("dup ok <&0", "a <&0\n", 0, 0, 0, 1, 1),
    H("dup ok then file", "a 2>&1 >o\n", 0, 0, 0, 1, 1),
    H("dup source 3", "a >&3\n", 0, 232, 4, -1, -1),
    H("dup source 10", "a >&10\n", 0, 232, 4, -1, -1),
    H("dup source 00", "a <&00\n", 0, 232, 4, -1, -1),
    H("dup name", "a >&f\n", 0, 231, 4, -1, -1),
    H("dup close", "a >&-\n", 0, 231, 4, -1, -1),
    H("dup quoted digit", "a >&'1'\n", 0, 231, 4, -1, -1),
    H("dup variable", "a >&$X\n", 0, 231, 4, -1, -1),
    H("dup digit then name", "a >&1x\n", 0, 231, 4, -1, -1),
    H("dup operator target", "a >& ;\n", 0, 242, 5, -1, -1),
    H("reserved if", "if x\n", 0, 233, 0, -1, -1),
    H("reserved then", "then\n", 0, 233, 0, -1, -1),
    H("reserved elif", "elif x\n", 0, 233, 0, -1, -1),
    H("reserved else", "else\n", 0, 233, 0, -1, -1),
    H("reserved fi", "fi\n", 0, 233, 0, -1, -1),
    H("reserved for", "for x\n", 0, 234, 0, -1, -1),
    H("reserved while", "while x\n", 0, 234, 0, -1, -1),
    H("reserved until", "until x\n", 0, 234, 0, -1, -1),
    H("reserved do", "do\n", 0, 234, 0, -1, -1),
    H("reserved done", "done\n", 0, 234, 0, -1, -1),
    H("reserved in", "in x\n", 0, 234, 0, -1, -1),
    H("reserved select", "select x\n", 0, 234, 0, -1, -1),
    H("reserved case", "case x\n", 0, 235, 0, -1, -1),
    H("reserved esac", "esac\n", 0, 235, 0, -1, -1),
    H("reserved function", "function f\n", 0, 237, 0, -1, -1),
    H("reserved negation", "! x\n", 0, 238, 0, -1, -1),
    H("reserved time", "time ls\n", 0, 239, 0, -1, -1),
    H("reserved coproc", "coproc x\n", 0, 239, 0, -1, -1),
    H("reserved after an assignment", "A=1 if\n", 0, 233, 4, -1, -1),
    H("reserved after a redirection", ">f for\n", 0, 234, 3, -1, -1),
    H("reserved after a pipe", "a | if\n", 0, 233, 4, -1, -1),
    H("reserved after a semicolon", "a; done\n", 0, 234, 3, -1, -1),
    H("reserved after and", "a && case x\n", 0, 235, 5, -1, -1),
    H("reserved with a continuation", "i\\\nf x\n", 0, 233, 0, -1, -1),
    H("reserved as an argument", "echo if then fi\n", 0, 0, 0, 1, 1),
    H("reserved as a redirect target", "a > if\n", 0, 0, 0, 1, 1),
    H("reserved in quotes", "'if' x\n", 0, 0, 0, 1, 1),
    H("reserved escaped", "\\if x\n", 0, 0, 0, 1, 1),
    H("reserved with a suffix", "iff ifx fi2 done1 !x a!\n", 0, 0, 0, 1, 1),
    H("reserved prefix longer than eight bytes", "functionx\n", 0, 0, 0, 1, 1),
    H("ifs assignment", "IFS=: ls\n", 0, 240, 0, -1, -1),
    H("ifs assignment empty", "IFS= \n", 0, 240, 0, -1, -1),
    H("ifs assignment quoted", "IFS=\"x\" a\n", 0, 240, 0, -1, -1),
    H("ifs assignment second", "A=1 IFS=2\n", 0, 240, 4, -1, -1),
    H("ifs assignment with a continuation", "IF\\\nS=1 x\n", 0, 240, 0, -1, -1),
    H("ifs lookalikes", "IFSX=1 xIFS=1 _IFS=1 a\n", 0, 0, 0, 1, 1),
    H("ifs as an argument", "echo IFS=x\n", 0, 0, 0, 1, 1),
    H("quoted ifs name is a word", "\"IFS\"=x\n", 0, 0, 0, 1, 1),
    H("open quote at end of input", "echo \"abc", 1, 248, 9, -1, -1),
    H("open quote without the flag", "echo \"abc", 0, 100, 0, -1, -1),
    H("trailing backslash at end of input is a literal (bash)", "echo a\\", 1, 0, 0, 1, 1),
    H("trailing backslash without the flag waits", "echo a\\", 0, 100, 0, -1, -1),
    H("open brace parameter at end of input", "echo ${A", 1, 248, 8, -1, -1),
    H("dollar at end of input", "echo $", 1, 0, 0, 1, 1),
    H("background at end of input", "a &", 1, 227, 2, -1, -1),
    H("redirect operator at end of input", "a <", 1, 242, 3, -1, -1),
    H("comment at end of input", "a # c", 1, 0, 0, 1, 1),
    H("blank at end of input", "   ", 1, 0, 0, 0, 0),
    H("lexer refusal wins", "a | | $(x)\n", 0, 226, 7, -1, -1),
    H("lexer cap line", "", 0, 0, 0, 0, 0), /* replaced at run time */
};

static void build_cap(const char *name, int which, uint8_t *buf, size_t *len, uint64_t *status, uint64_t *off, int *np, int *nc)
{
    (void)name;
    size_t n = 0;
    *np = *nc = -1;
    *off = 0;
    *status = 0;
    switch (which) {
    case 0: /* 32 words: ok */
        buf[n++] = 'a'; for (int q = 0; q < 31; q++) { buf[n++] = ' '; buf[n++] = 'b'; } *np = 1; *nc = 1; break;
    case 1: /* 33 words */
        buf[n++] = 'a'; for (int q = 0; q < 32; q++) { buf[n++] = ' '; buf[n++] = 'b'; } *status = 206; *off = 64; break;
    case 2: /* 16 assignments then a word: ok */
        for (int q = 0; q < 16; q++) { memcpy(buf + n, "a=1 ", 4); n += 4; } buf[n++] = 'x'; *np = 1; *nc = 1; break;
    case 3: /* 17 assignments */
        for (int q = 0; q < 17; q++) { memcpy(buf + n, "a=1 ", 4); n += 4; } *status = 207; *off = 64; break;
    case 4: /* 8 redirections: ok */
        for (int q = 0; q < 8; q++) { memcpy(buf + n, ">f ", 3); n += 3; } *np = 1; *nc = 1; break;
    case 5: /* 9 redirections */
        for (int q = 0; q < 9; q++) { memcpy(buf + n, ">f ", 3); n += 3; } *status = 208; *off = 24; break;
    case 6: /* 16 pipelines: ok */
        for (int q = 0; q < 16; q++) { memcpy(buf + n, "a;", 2); n += 2; } *np = 16; *nc = 16; break;
    case 7: /* 17 pipelines */
        for (int q = 0; q < 17; q++) { memcpy(buf + n, "a;", 2); n += 2; } *status = 204; *off = 32; break;
    case 8: /* 16 pipelines of two: 32 commands ok */
        for (int q = 0; q < 16; q++) { memcpy(buf + n, "a|a;", 4); n += 4; } *np = 16; *nc = 32; break;
    case 9: /* 17 pipelines of two: pipelines first */
        for (int q = 0; q < 17; q++) { memcpy(buf + n, "a|a;", 4); n += 4; } *status = 204; *off = 64; break;
    case 10: /* 4 pipelines of 8 = 32 commands, then a fifth with one: the 33rd command */
        for (int q = 0; q < 4; q++) { for (int r = 0; r < 8; r++) { buf[n++] = 'a'; buf[n++] = (r == 7) ? ';' : '|'; } } buf[n++] = 'a'; *status = 203; *off = 64; break;
    case 11: /* 4 pipelines of 8 = 32 commands: ok */
        for (int q = 0; q < 4; q++) { for (int r = 0; r < 8; r++) { buf[n++] = 'a'; buf[n++] = (r == 7) ? ';' : '|'; } } *np = 4; *nc = 32; break;
    case 12: /* 9 commands in one pipeline after an and: the pipe cap is per pipeline */
        memcpy(buf, "a&&", 3); n = 3; for (int r = 0; r < 8; r++) { buf[n++] = 'a'; buf[n++] = '|'; } buf[n++] = 'a'; *status = 205; *off = 3 + 16; break;
    default: /* redirect target words do not count as words */
        buf[n++] = 'a'; for (int q = 0; q < 31; q++) { buf[n++] = ' '; buf[n++] = 'b'; } for (int q = 0; q < 8; q++) { memcpy(buf + n, " >f", 3); n += 3; } *np = 1; *nc = 1; break;
    }
    buf[n++] = '\n';
    *len = n;
}

#define NCAP 14

static void run_hostile(uint64_t *rng)
{
    for (size_t h = 0; h < sizeof HOSTILE / sizeof HOSTILE[0]; h++) {
        const Hostile *c = &HOSTILE[h];
        const uint8_t *d = (const uint8_t *)c->in;
        size_t n = c->len;
        uint8_t *big = NULL;
        uint64_t status = c->status, off = c->off;
        int np = c->npipe, nc = c->ncmd;
        if (strcmp(c->name, "lexer cap line") == 0) {
            n = 4097;
            big = malloc(n);
            memset(big, 'a', n);
            big[n - 1] = '\n';
            d = big;
            status = 201;
            off = 4096;
            np = nc = -1;
        }
        Outcome o0, o1;
        run_all(c->name, d, n, rng, h, &o0, &o1);
        n_hostile++;
        const Outcome *o = c->eoi ? &o1 : &o0;
        check(o->status == status, "hostile '%s': status %llu, want %llu", c->name, (unsigned long long)o->status, (unsigned long long)status);
        if (status >= 200) check(o->err_off == off, "hostile '%s': offset %llu, want %llu", c->name, (unsigned long long)o->err_off, (unsigned long long)off);
        if (np >= 0 && (status == 0 || status == 100)) check(o->npipe == (uint64_t)np && o->ncmd == (uint64_t)nc, "hostile '%s': %llu pipelines %llu commands, want %d %d", c->name, (unsigned long long)o->npipe, (unsigned long long)o->ncmd, np, nc);
        free(big);
    }
    /* capacity cases: every one also runs with the flag set (a complete line behaves the same) */
    for (int which = 0; which < NCAP; which++) {
        uint8_t buf[1024];
        size_t n;
        uint64_t status, off;
        int np, nc;
        build_cap("cap", which, buf, &n, &status, &off, &np, &nc);
        Outcome o0, o1;
        char tag[32];
        snprintf(tag, sizeof tag, "capacity %d", which);
        run_all(tag, buf, n, rng, 500 + (unsigned long)which, &o0, &o1);
        n_hostile++;
        check(o0.status == status && o1.status == status, "%s: status %llu/%llu, want %llu", tag, (unsigned long long)o0.status, (unsigned long long)o1.status, (unsigned long long)status);
        if (status >= 200) check(o0.err_off == off, "%s: offset %llu, want %llu", tag, (unsigned long long)o0.err_off, (unsigned long long)off);
        if (np >= 0) check(o0.npipe == (uint64_t)np && o0.ncmd == (uint64_t)nc, "%s: %llu pipelines %llu commands, want %d %d", tag, (unsigned long long)o0.npipe, (unsigned long long)o0.ncmd, np, nc);
    }
}

/* ---- ABI-level refusals of the parser entry ---- */
static void run_abi(void)
{
    const uint8_t in[] = "ls\n";
    for (int e = 0; e < 2; e++) {
        Engine en = e ? NATIVE : INTERP;
        uint64_t *w = sess_new();
        /* the parser before the lexer: refused, nothing written */
        uint64_t *snap = malloc(WS * 8);
        memcpy(snap, w, WS * 8);
        check(ucall(&PARSE, en, w, WS, in, 3) == 217 && memcmp(snap, w, WS * 8) == 0, "parse before lex is refused 217 and writes nothing");
        n_vec_parse++;
        /* header refusals */
        check(ucall(&PARSE, en, w, 1, in, 3) == 216, "parse: one cell");
        w[OSH_S_MAGIC] ^= 1;
        check(ucall(&PARSE, en, w, WS, in, 3) == 214, "parse: bad magic");
        w[OSH_S_MAGIC] ^= 1;
        w[OSH_S_VERSION] = 2;
        check(ucall(&PARSE, en, w, WS, in, 3) == 215, "parse: bad version");
        w[OSH_S_VERSION] = 1;
        check(ucall(&PARSE, en, w, WS - 1, in, 3) == 213, "parse: short workspace");
        w[48] = 1;
        check(ucall(&PARSE, en, w, WS, in, 3) == 217, "parse: reserved cell");
        w[48] = 0;
        w[OSH_S_EOI] = 2;
        check(ucall(&PARSE, en, w, WS, in, 3) == 217 && ucall(&LEX, en, w, WS, in, 3) == 217, "eoi flag above 1 is ABI_RESERVED in both units");
        w[OSH_S_EOI] = 0;
        /* a corrupt parser state is refused */
        check(ucall(&LEX, en, w, WS, in, 3) == 0, "lex ok");
        w[OSH_PX_STATE] = 9;
        check(ucall(&PARSE, en, w, WS, in, 3) == 217, "corrupt parser state is refused");
        w[OSH_PX_STATE] = 0;
        w[OSH_PX_TOK] = 99;
        check(ucall(&PARSE, en, w, WS, in, 3) == 217, "parser cursor past the tokens is refused");
        w[OSH_PX_TOK] = 0;
        check(ucall(&PARSE, en, w, WS, in, 3) == 0, "parse ok after the refusals");
        free(snap);
        free(w);
    }
    /* a refusal is terminal and repeats; the parser reports the lexer's refusal too */
    uint64_t *w = sess_new();
    const uint8_t bad[] = "a | | b\n";
    int fl;
    check(host_run(INTERP, w, bad, sizeof bad - 1, &fl) == 241 && !fl, "terminal: first");
    check(ucall(&PARSE, NATIVE, w, WS, bad, sizeof bad - 1) == 241 && ucall(&PARSE, INTERP, w, WS, (const uint8_t *)"", 0) == 241, "terminal: repeat");
    free(w);
    w = sess_new();
    const uint8_t lbad[] = "echo $(x)\n";
    check(host_run(INTERP, w, lbad, sizeof lbad - 1, &fl) == 226, "lexer refusal");
    check(ucall(&PARSE, INTERP, w, WS, lbad, sizeof lbad - 1) == 226, "parser repeats the lexer refusal");
    free(w);
    /* the parser sees a lexer that is not done: a half line */
    w = sess_new();
    const uint8_t half[] = "echo \"abc";
    check(run_entry(&LEX, INTERP, w, half, sizeof half - 1) == 100, "half line is 100");
    check(ucall(&PARSE, INTERP, w, WS, half, sizeof half - 1) == 217, "parser refuses a lexer that is not done");
    free(w);
    /* incremental: a trailing && resumes after the next line is appended and re-lexed */
    for (int e = 0; e < 2; e++) {
        Engine en = e ? NATIVE : INTERP;
        w = sess_new();
        const uint8_t l1[] = "a &&\n";
        const uint8_t l2[] = "a &&\nb | \nc\nignored\n";
        check(host_run(en, w, l1, sizeof l1 - 1, &fl) == 100 && !fl, "resume: first line 100");
        check(host_run(en, w, l2, 11, &fl) == 100 && fl, "resume: second line still open (pipe)");
        check(host_run(en, w, l2, sizeof l2 - 1, &fl) == 0 && !fl, "resume: done");
        check(w[OSH_PX_NPIPE] == 2 && w[OSH_PX_NCMD] == 3, "resume: tables %llu %llu", (unsigned long long)w[OSH_PX_NPIPE], (unsigned long long)w[OSH_PX_NCMD]);
        check(w[OSH_PX_TOK] < w[OSH_LX_NTOK], "resume: tokens of the next line are left");
        free(w);
    }
}

/* ---- conformance vectors at parser level ---- */
static void put_q(char **o, const uint8_t *s, size_t len)
{
    *o += sprintf(*o, "\"");
    for (size_t i = 0; i < len; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') *o += sprintf(*o, "\\%c", c);
        else if (c < 0x20 || c > 0x7e) *o += sprintf(*o, "\\x%02X", c);
        else *o += sprintf(*o, "%c", c);
    }
    *o += sprintf(*o, "\"");
}

/* a word's text with backslash-newline pairs removed, when it has no quote, escape or expansion */
static int literal_word(const uint8_t *in, const uint64_t *tok, uint8_t *out, size_t *olen)
{
    uint64_t flags = (tok[0] >> 8) & 0xffff;
    if (flags & ~32ULL & ~16ULL) return 0;
    size_t k = 0;
    for (uint64_t q = 0; q < tok[2]; q++) {
        if (in[tok[1] + q] == '\\' && q + 1 < tok[2] && in[tok[1] + q + 1] == '\n') { q++; continue; }
        out[k++] = in[tok[1] + q];
    }
    *olen = k;
    return 1;
}

/* render the parse tables in the vector grammar; `?` stands for an item the parser cannot know (an expanded or
 * quoted word), `*` for a whole argv list when an expansion could change its length */
static void render(const uint8_t *in, const uint64_t *w, char *out)
{
    char *o = out;
    uint8_t tmp[4200];
    size_t tl;
    for (uint64_t p = 0; p < w[OSH_PX_NPIPE]; p++) {
        const uint64_t *pe = &w[OSH_PIPES + 4 * p];
        if (p) o += sprintf(o, " | ");
        o += sprintf(o, "P%llu conn=%llu", (unsigned long long)p, (unsigned long long)pe[2]);
        for (uint64_t j = 0; j < pe[1]; j++) {
            const uint64_t *ce = &w[OSH_CMDS + 8 * (pe[0] + j)];
            o += sprintf(o, " | C%llu b=? argv=", (unsigned long long)j);
            int dollar = 0;
            for (uint64_t t = ce[0]; t < ce[1]; t++) if (((w[OSH_TOKENS + 4 * t] >> 8) & 8) && (w[OSH_TOKENS + 4 * t] & 255) == 1) dollar = 1;
            char argv[40000], as[40000], rd[40000];
            char *oa = argv, *os = as, *orr = rd;
            int any = 0;
            for (uint64_t t = ce[0]; t < ce[1];) {
                const uint64_t *tk = &w[OSH_TOKENS + 4 * t];
                uint64_t kind = tk[0] & 255, flags = (tk[0] >> 8) & 0xffff;
                if (kind == 1) {
                    if ((flags & 16) && !any) {
                        uint64_t eq = tk[3] - 1;
                        if (os != as) *os++ = ',';
                        os += snprintf(os, 4096, "%.*s=", (int)(eq - tk[1]), in + tk[1]);
                        if (literal_word(in, tk, tmp, &tl) && !(flags & 32)) put_q(&os, in + eq + 1, tk[1] + tk[2] - eq - 1);
                        else os += sprintf(os, "?");
                    } else {
                        any = 1;
                        if (oa != argv) *oa++ = ',';
                        if (literal_word(in, tk, tmp, &tl)) put_q(&oa, tmp, tl);
                        else oa += sprintf(oa, "?");
                    }
                    t++;
                } else {
                    static const char *KN[12] = { "", "", "", "", "", "", "IN", "OUT", "APPEND", "", "DUP", "DUP" };
                    const uint64_t *tg = &w[OSH_TOKENS + 4 * (t + 1)];
                    if (orr != rd) *orr++ = ',';
                    orr += sprintf(orr, "%s%llu:", KN[kind], (unsigned long long)tk[3]);
                    if (kind == 10 || kind == 11) orr += sprintf(orr, "%c", in[tg[1]]);
                    else if (literal_word(in, tg, tmp, &tl)) put_q(&orr, tmp, tl);
                    else orr += sprintf(orr, "?");
                    t += 2;
                }
            }
            *oa = *os = *orr = 0;
            o += sprintf(o, "%s as=%s rd=%s", dollar ? "*" : argv, as, rd);
        }
    }
    *o = 0;
}

/* match the rendered form against an expected line tail; `?` skips one expected item, `*` skips an argv list, and
 * `b=?` skips the builtin id */
static int item_end(const char *e)
{
    int i = 0;
    if (e[0] == '"') {
        i = 1;
        while (e[i] && e[i] != '"') i += e[i] == '\\' ? 2 : 1;
        return e[i] ? i + 1 : i;
    }
    while (e[i] && e[i] != ',' && e[i] != ' ') i++;
    return i;
}

/* the vector grammar's `"c"*N` shorthand: N repetitions of c inside quotes */
static void expand_rep(const char *e, char *out)
{
    while (*e) {
        if (e[0] == '"' && e[1] && e[1] != '\\' && e[2] == '"' && e[3] == '*' && e[4] >= '0' && e[4] <= '9') {
            char ch = e[1];
            char *end;
            unsigned long cnt = strtoul(e + 4, &end, 10);
            *out++ = '"';
            memset(out, ch, cnt);
            out += cnt;
            *out++ = '"';
            e = end;
            continue;
        }
        *out++ = *e++;
    }
    *out = 0;
}

static int match(const char *r, const char *e)
{
    while (*r) {
        if (r[0] == 'b' && r[1] == '=' && r[2] == '?') {
            if (strncmp(e, "b=", 2) != 0) return 0;
            r += 3;
            e += 3;
            while (*e >= '0' && *e <= '9') e++;
        } else if (r[0] == '*') {
            r++;
            while (*e && strncmp(e, " as=", 4) != 0) e++;
        } else if (r[0] == '?') {
            r++;
            e += item_end(e);
        } else {
            if (*r != *e) return 0;
            r++;
            e++;
        }
    }
    return *e == 0;
}

static void run_vector_dir(const char *dir, uint64_t *rng)
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
        Outcome o0, o1;
        run_all(name, (const uint8_t *)in, n, rng, n_vec, &o0, &o1);
        n_vec++;
        uint64_t *w = sess_new();
        int fl;
        uint64_t st = host_run(INTERP, w, (const uint8_t *)in, n, &fl);
        const char *p;
        if (strstr(line, " more")) {
            check(st == 100 && o0.status == 100, "vector %s: status %llu, want 100", name, (unsigned long long)st);
            check(o1.status >= 200, "vector %s: with end of input the status is %llu, want a refusal", name, (unsigned long long)o1.status);
        } else if ((p = strstr(line, " refuse "))) {
            unsigned code = 0, off = 0;
            if (sscanf(p, " refuse %u %*s off=%u", &code, &off) != 2) check(0, "%s: bad refuse line", name);
            if (code >= 244 && code <= 245) { /* the expander's: the parser must finish the list */
                check(st == 0, "vector %s: parser status %llu, want 0 (expander refusal %u)", name, (unsigned long long)st, code);
            } else {
                check(st == code && w[OSH_S_ERR_OFF] == off, "vector %s: refusal %llu@%llu, want %u@%u", name, (unsigned long long)st,
                      (unsigned long long)w[OSH_S_ERR_OFF], code, off);
            }
        } else if ((p = strstr(line, " | P0"))) {
            check(st == 0 && !fl, "vector %s: status %llu, want 0 (list complete)", name, (unsigned long long)st);
            if (st == 0) {
                char *r = malloc(1 << 16);
                render((const uint8_t *)in, w, r);
                char *exp_tail = malloc(1 << 16);
                expand_rep(p + 3, exp_tail);
                int m = match(r, exp_tail);
                if (!m) fprintf(stderr, "  vector %s:\n    rendered %s\n    expected %s\n", name, r, exp_tail);
                check(m, "vector %s: parse tables do not match the expected line", name);
                free(exp_tail);
                free(r);
            }
        } else {
            check(st == 0, "vector %s: status %llu, want 0", name, (unsigned long long)st);
        }
        n_vec_parse++;
        free(w);
        free(in);
    }
    free(exp);
}

/* ---- fuzz ---- */
static const char *FRAG[] = {
    "ls", "echo", " ", " ", "  ", "\t", "\n", "\n", "-l", "/tmp", "a", "b", "x=1", "A=", "_n", "0", "1", "2", "3",
    "'x y'", "\"a b\"", "\"$B c\"", "\"$(x)\"", "'", "\"", "\\", "\\\n", "$", "$1", "$?", "$@", "$$", "${V}", "${", "}", "{", "$(", ")", "(", "`", "*",
    "~", "{a,b}", "!", "#", "# c\n", "|", "||", "&&", "&", ";", ";;", "<", ">", ">>", ">&", "<&", "<<", ">|", "<>", "2>", "2>&1", "1>>", "0<",
    "5>", "=", ":", "\xff", "if", "then", "fi", "for", "do", "done", "case", "esac", "while", "until", "in", "function", "!", "time", "select",
    "coproc", "elif", "else", "IFS=", "IFS=x", "IF\\\nS=1", "i\\\nf", "{ ", " }", ">&1", ">&2", "<&0", ">&3", ">&-", ">&f", ">&10",
};

/* fragments that mostly parse clean, so the tables get exercised */
static const char *SAFE[] = {
    "ls", "echo", "cat", " ", " ", " ", "  ", "\t", "\n", "-l", "/tmp", "a", "b", "x=1", "A=", "_n", "0", "1", "10", "'x y'", "\"a b\"", "\"$B c\"", "$1",
    "$?", "$@", "${V}", "|", "|", "||", "&&", ";", ";", "<", ">", ">>", ">&", "<&", "2>", "2>&1", "1>>", "0<", "2", "1", "0", "f", "out", "# c\n", "\\\n",
    "iff", "ifx", "'if'", "IFSx=1",
};

/* well-formed pieces for the capacity edges */
static size_t gen(uint8_t *buf, size_t cap, uint64_t *rng)
{
    size_t n = 0;
    unsigned mode = (unsigned)(rng_next(rng) % 10);
    if (mode == 0) { /* raw bytes */
        size_t len = rng_next(rng) % 40;
        for (size_t i = 0; i < len; i++) buf[n++] = (uint8_t)rng_next(rng);
    } else if (mode <= 4) { /* mostly clean fragments */
        size_t nf = 1 + rng_next(rng) % 40;
        for (size_t i = 0; i < nf; i++) {
            const char *f = SAFE[rng_next(rng) % (sizeof SAFE / sizeof SAFE[0])];
            size_t fl = strlen(f);
            memcpy(buf + n, f, fl);
            n += fl;
        }
        if (rng_next(rng) % 4 != 0) buf[n++] = '\n';
    } else if (mode == 5 || mode == 6) { /* a repeated unit: reaches the capacity edges */
        static const char *UNIT[] = { "a ", "A=1 ", ">f ", "a|", "a;", "a&&", "a||", "a|a;", "b ", "2>&1 ", "a\n" };
        const char *u = UNIT[rng_next(rng) % (sizeof UNIT / sizeof UNIT[0])];
        size_t reps = 1 + rng_next(rng) % 40;
        size_t ul = strlen(u);
        if (rng_next(rng) % 3 == 0) { const char *pre = "cmd "; memcpy(buf + n, pre, 4); n += 4; }
        for (size_t i = 0; i < reps && n + ul < cap - 8; i++) { memcpy(buf + n, u, ul); n += ul; }
        if (rng_next(rng) % 3 != 0) { buf[n++] = 'z'; }
        if (rng_next(rng) % 4 != 0) buf[n++] = '\n';
    } else { /* hostile fragments */
        size_t nf = 1 + rng_next(rng) % (mode == 9 ? 60 : 14);
        for (size_t i = 0; i < nf; i++) {
            const char *f = FRAG[rng_next(rng) % (sizeof FRAG / sizeof FRAG[0])];
            size_t fl = strlen(f);
            if (n + fl > cap) break;
            memcpy(buf + n, f, fl);
            n += fl;
        }
        if (rng_next(rng) % 4 != 0 && n < cap) buf[n++] = '\n';
        if (rng_next(rng) % 8 == 0 && n) buf[rng_next(rng) % n] = (uint8_t)rng_next(rng);
    }
    return n;
}

static void run_fuzz(unsigned long count, uint64_t *rng)
{
    uint8_t *buf = malloc(8192);
    for (unsigned long i = 0; i < count; i++) {
        size_t n = gen(buf, 8000, rng);
        Outcome o0, o1;
        run_all("fuzz", buf, n, rng, i, &o0, &o1);
        n_fuzz++;
        for (int k = 0; k < 2; k++) {
            const Outcome *o = k ? &o1 : &o0;
            if (o->status == 0) n_outcome[0]++;
            else if (o->status == 100) n_outcome[1]++;
            else if (o->status >= 200) { n_outcome[o->from_lexer ? 2 : 3]++; n_code[o->status & 255]++; }
        }
    }
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: test_osh_parse OSH_LEX_OSC OSH_PARSE_OSC VECTORS_DIR PARSE_VECTORS_DIR [FUZZ_COUNT]\n");
        return 2;
    }
    unsigned long fuzz = argc > 5 ? strtoul(argv[5], NULL, 10) : 100000;
    check(OSH_WS_CELLS == 11456 && OSH_TOKENS == 64 && OSH_CMDS == 576 && OSH_PIPES == 832 && OSH_STAGING == 896 && OSH_REQUEST == 1928 &&
              OSH_OUT == 3264 && OSH_ABI_MAGIC == 0x4F534857ULL && OSH_S_ERR_CODE == 4 && OSH_S_ERR_OFF == 5 && OSH_S_RESERVED == 48 &&
              OSH_CAP_CMDS == 32 && OSH_CAP_PIPES == 16 && OSH_CAP_PIPE_LEN == 8 && OSH_CAP_WORDS == 32 && OSH_CAP_ASSIGNS == 16 &&
              OSH_CAP_REDIRS == 8 && OSH_PARSE_BUDGET == 64 && OSH_CMDS + 8 * OSH_CAP_CMDS == OSH_PIPES && OSH_PIPES + 4 * OSH_CAP_PIPES == OSH_STAGING,
          "generated layout differs from the ABI draft numbers");
    char *src1 = NULL, *src2 = NULL;
    if (unit_load(&LEX, argv[1], &src1) || unit_load(&PARSE, argv[2], &src2)) { printf("OSH_PARSE_FAIL\n"); return 1; }
    unit_stats("lex  ", &LEX);
    unit_stats("parse", &PARSE);

    uint64_t rng = 0x05a1e0c0ffee0005ULL;
    run_abi();
    run_hostile(&rng);
    run_vector_dir(argv[3], &rng);
    run_vector_dir(argv[4], &rng);
    run_fuzz(fuzz, &rng);

    printf("hostile cases: %lu (each: interp, native, reference, chunked, with the end-of-input flag clear and set)\n", n_hostile);
    printf("vectors: %lu run, %lu parser-level checks (plus ABI checks)\n", n_vec, n_vec_parse);
    printf("fuzz: %lu inputs x 2 flag settings (seed fixed): list done=%lu need more=%lu lexer refused=%lu parser refused=%lu\n", n_fuzz, n_outcome[0], n_outcome[1], n_outcome[2], n_outcome[3]);
    printf("refusal codes seen in fuzz:");
    for (int c = 0; c < 256; c++) if (n_code[c]) printf(" %d:%lu", c, n_code[c]);
    printf("\n");
    printf("cases %lu; interp calls %lu; native calls %lu; interp==native workspace compares %lu; reference compares %lu (%lu command rows, %lu pipeline rows); idempotence repeats %lu; chunked host runs %lu; traps %lu\n",
           n_cases, n_calls_i, n_calls_n, n_engine_cmp, n_ref_cmp, n_cmds_cmp, n_pipes_cmp, n_idem, n_chunk_runs, n_traps);
    printf("checks %lu, failures %lu\n", n_checks, n_fail);
    osc_native_unmap(&LEX.nm);
    osc_native_unmap(&PARSE.nm);
    osc_cg_free(&LEX.code);
    osc_cg_free(&PARSE.code);
    free(src1);
    free(src2);
    int ok = n_fail == 0;
    printf(ok ? "OSH_PARSE_PASS\n" : "OSH_PARSE_FAIL\n");
    return ok ? 0 : 1;
}
