/*
 * test_osh_expand.c -- host driver for the osh expander unit (src/osh/osh_expand.osc), aien-architecture#158 cut 4c.
 *
 * Compiles the lexer, parser and expander units and runs the host sequence (lex_run until done, parse_run, then
 * expand_run answering NEED_VAR from an environment table and feeding back pipeline statuses) in the reference
 * interpreter AND as native AArch64, with exact-size heap buffers (under ASan every out-of-range byte is a report),
 * against:
 *   - the independent C reference expander (osh_expand_ref.c), request record cell for cell, expanded bytes, the
 *     sequence of variable requests, refusal code and offset;
 *   - the host request decoder (osh_req.c): every record the unit returns must decode;
 *   - itself, with the input fed in random chunk splits, with repeated calls at 101 and 102, interp vs native workspace
 *     byte for byte;
 *   - the conformance vectors (tests/osh/vectors, expected.txt, lexer + parser + expander level);
 *   - a table of hostile cases with hand-written expected records (many cross-checked against GNU bash), caps, ABI
 *     header refusals, corrupted tables (no memory error, engines agree);
 *   - a fixed-seed fuzz of random command lines over a fixed environment.
 * Usage: test_osh_expand LEX.osc PARSE.osc EXPAND.osc VECTORS_DIR [FUZZ_COUNT]
 * Final line: OSH_EXPAND_PASS or OSH_EXPAND_FAIL.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osh_core.h"
#include "osh_expand_ref.h"
#include "osh_host.h"
#include "osh_layout.h"
#include "osh_lex_ref.h"
#include "osh_parse_ref.h"

#define WS ((size_t)OSH_WS_CELLS)
#define REC_MAX (8 + 8 * 164)

static unsigned long n_checks, n_fail;
static unsigned long n_cases, n_ref_cmp, n_eng_cmp, n_chunk, n_idem, n_decode, n_req, n_pipes, n_resume102, n_hostile, n_vec, n_fuzz, n_corrupt, n_corrupt_trap;
static unsigned long n_code[256];
static unsigned long n_final_ok, n_final_err;

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

static OshCore CORE;
typedef enum { INTERP = 0, NATIVE = 1 } Engine;

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

static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* one entry call on an exact-size copy of d[0..n): the status, or ~0 on a fault */
static uint64_t ucall(int unit, Engine e, uint64_t *w, size_t wn, const uint8_t *d, size_t n)
{
    uint8_t *b = NULL;
    if (n) {
        b = malloc(n);
        if (!b) { fprintf(stderr, "oom\n"); exit(2); }
        memcpy(b, d, n);
    }
    uint64_t r = osh_core_call(&CORE, unit, e == NATIVE, b, n, w, wn);
    free(b);
    return r;
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

/* ---- environment ---- */
static void env_set_var(OshXrEnv *env, const char *name, const uint8_t *v, size_t n)
{
    int i;
    for (i = 0; i < env->nvars; i++)
        if (strcmp(env->var[i].name, name) == 0) break;
    if (i == env->nvars) {
        if (i >= OSH_XR_MAXVARS) return;
        env->nvars++;
        snprintf(env->var[i].name, sizeof env->var[i].name, "%s", name);
    }
    env->var[i].v.set = 1;
    env->var[i].v.len = n;
    memcpy(env->var[i].v.b, v, n < OSH_XR_VALCAP ? n : OSH_XR_VALCAP);
}

static void env_set_pos(OshXrEnv *env, int k, const uint8_t *v, size_t n)
{
    OshXrStr *s = k == 0 ? &env->arg0 : &env->pos[k - 1];
    s->set = 1;
    s->len = n;
    memcpy(s->b, v, n < OSH_XR_VALCAP ? n : OSH_XR_VALCAP);
    if (k > env->npos) env->npos = k;
}

/* unescape \n \t \0 \\ for hostile-table environment text */
static size_t unesc(const char *s, size_t n, uint8_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            out[o++] = s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i] == '0' ? 0 : (uint8_t)s[i];
        } else out[o++] = (uint8_t)s[i];
    }
    return o;
}

/* env text: lines NAME=VALUE, @status=N, @argK=VALUE (README of the vectors). esc: process \n \t \0 \\ in values. */
static void env_parse(OshXrEnv *env, const char *text, size_t len, int esc)
{
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        const char *l = text + i;
        size_t ll = e - i;
        i = e + 1;
        if (ll == 0) continue;
        const char *eq = memchr(l, '=', ll);
        if (!eq) continue;
        size_t nl = (size_t)(eq - l), vl = ll - nl - 1;
        uint8_t v[OSH_XR_VALCAP];
        size_t n2;
        if (esc) n2 = unesc(eq + 1, vl, v);
        else { memcpy(v, eq + 1, vl); n2 = vl; }
        if (l[0] == '@') {
            if (nl == 7 && memcmp(l, "@status", 7) == 0) env->status = (uint64_t)strtoull((const char *)v, NULL, 10);
            else if (nl == 5 && memcmp(l, "@arg", 4) == 0) env_set_pos(env, l[4] - '0', v, n2);
        } else {
            char name[64];
            if (nl >= sizeof name) continue;
            memcpy(name, l, nl);
            name[nl] = 0;
            env_set_var(env, name, v, n2);
        }
    }
}

/* ---- pipeline statuses the "host" reports ---- */
static uint64_t pstat(uint64_t seed, unsigned idx)
{
    uint64_t s = seed + 0x9E3779B97F4A7C15ULL * (idx + 1);
    s ^= s >> 30; s *= 0xBF58476D1CE4E5B9ULL; s ^= s >> 27; s *= 0x94D049BB133111EBULL; s ^= s >> 31;
    return s % 3 == 0 ? 0 : s % 5 + 1;
}

/* ---- the host run ---- */
typedef struct {
    unsigned idx; /* pipeline index in the list */
    uint64_t rec[REC_MAX];
    size_t nrec;
    uint8_t out[OSH_XR_OUTCAP];
    size_t out_used;
    unsigned nreq;
    OshXrReq req[OSH_XR_MAXREQ];
} HPipe;

typedef struct {
    uint64_t final; /* 104 complete, 100 more input, else refusal code */
    uint64_t err_off;
    int from_lexer;
    unsigned np;
    HPipe p[16];
    unsigned lex_resumes;
    unsigned budget_resumes;
} HRes;

typedef struct {
    int eoi, noskip;
    uint64_t seed;      /* pipeline status stream; 0 with fixed_status */
    int fixed_status;   /* vectors: $? is env.status for every request, and no skipping */
    unsigned chunk_mode; /* 0 single shot, 1 one byte, 2 random 1..16, 3 random up to the rest */
    uint64_t *rng;
    int probe;          /* repeat calls at 101 / 102 and require identical outcome */
} HOpt;

static uint64_t run_entry(int unit, Engine e, uint64_t *w, const uint8_t *d, size_t n, unsigned *resumes)
{
    for (int r = 0; r < 40; r++) {
        uint64_t st = ucall(unit, e, w, WS, d, n);
        if (st != 101) return st;
        if (resumes) (*resumes)++;
    }
    check(0, "more than 40 budget resumes");
    return ~0ULL;
}

static uint64_t lex_parse(Engine e, uint64_t *w, const uint8_t *d, size_t n, int *from_lexer, unsigned *lres)
{
    uint64_t st = run_entry(OSH_U_LEX, e, w, d, n, lres);
    *from_lexer = 1;
    if (st != 0) return st;
    *from_lexer = 0;
    return run_entry(OSH_U_PARSE, e, w, d, n, NULL);
}

static uint64_t lex_parse_chunked(Engine e, uint64_t *w, const uint8_t *d, size_t n, int eoi, unsigned mode, uint64_t *rng, int *from_lexer, unsigned *lres)
{
    size_t cur = 0;
    uint64_t st = 0;
    *from_lexer = 1;
    if (n == 0 || mode == 0) {
        w[OSH_S_EOI] = (uint64_t)eoi;
        return lex_parse(e, w, d, n, from_lexer, lres);
    }
    while (cur < n) {
        size_t ch = 1;
        if (mode == 2) ch = 1 + rng_next(rng) % 16;
        else if (mode == 3) ch = 1 + rng_next(rng) % (n - cur);
        if (ch > n - cur) ch = n - cur;
        cur += ch;
        st = lex_parse(e, w, d, cur, from_lexer, lres);
        if (st >= 200) return st;
        if (st == 0 && !*from_lexer) return st;
    }
    if (eoi) {
        w[OSH_S_EOI] = 1;
        st = lex_parse(e, w, d, n, from_lexer, lres);
    }
    return st;
}

static void answer_var(uint64_t *w, const uint8_t *d, size_t n, const OshXrEnv *env, OshXrReq *rq)
{
    uint64_t kind = w[OSH_S_VR_KIND], a = w[OSH_S_VR_A], len = w[OSH_S_VR_LEN];
    rq->kind = (int)kind;
    rq->a = a;
    rq->len = len;
    int found;
    size_t vl;
    uint64_t np;
    uint8_t val[OSH_XR_VALCAP];
    const uint8_t *name = NULL;
    size_t nl = 0;
    if (kind == 1) {
        if (a > n || len > n - a) { check(0, "NAME request outside the input"); return; }
        name = d + a;
        nl = len;
    }
    osh_xr_env_get(env, (int)kind, name, nl, a, &found, &vl, &np, val);
    w[OSH_STAGING + 0] = (uint64_t)found;
    w[OSH_STAGING + 1] = vl;
    w[OSH_STAGING + 2] = np;
    w[OSH_STAGING + 3] = 0;
    for (size_t i = 0; i < vl && i < 1024; i++) w[OSH_STAGING + 4 + i] = val[i];
}

static void host_run(Engine e, const uint8_t *d, size_t n, const OshXrEnv *env0, const HOpt *o, HRes *r, uint64_t **wout)
{
    uint64_t *w = sess_new();
    OshXrEnv *env = malloc(sizeof *env);
    memcpy(env, env0, sizeof *env);
    memset(r, 0, sizeof *r);
    uint64_t st = lex_parse_chunked(e, w, d, n, o->eoi, o->chunk_mode, o->rng, &r->from_lexer, &r->lex_resumes);
    if (st != 0 || r->from_lexer) {
        r->final = st;
        r->err_off = st >= 200 ? w[OSH_S_ERR_OFF] : 0;
        goto done;
    }
    if (w[OSH_PX_STATE] != 4) { /* parser consumed every token of a list that has no end yet: blank so far; the host reads more or, at end of input, has an empty list */
        r->final = 104;
        goto done;
    }
    w[OSH_S_NOSKIP] = (uint64_t)o->noskip;
    w[OSH_S_LAST_STATUS] = env->status;
    unsigned cur_pipe = 0;
    for (int guard = 0; guard < 4000; guard++) {
        st = ucall(OSH_U_EXPAND, e, w, WS, d, n);
        if (st == 101) {
            r->budget_resumes++;
            continue;
        }
        if (st == 102) {
            n_resume102++;
            if (r->np >= 16) { check(0, "more than 16 pipelines"); break; }
            OshXrReq rq;
            if (o->probe) {
                uint64_t *snap = malloc(WS * 8);
                memcpy(snap, w, WS * 8);
                uint64_t again = ucall(OSH_U_EXPAND, e, w, WS, d, n);
                check(again == 102 && memcmp(snap, w, WS * 8) == 0, "repeated call at NEED_VAR before the answer changed the outcome (%llu)", (unsigned long long)again);
                n_idem++;
                free(snap);
            }
            answer_var(w, d, n, env, &rq);
            HPipe *hp = &r->p[r->np];
            if (hp->nreq < OSH_XR_MAXREQ) hp->req[hp->nreq++] = rq;
            n_req++;
            continue;
        }
        if (st == 103) {
            HPipe *hp = &r->p[r->np];
            uint64_t nc = w[OSH_REQUEST + 1];
            hp->nrec = (size_t)(8 + nc * 164);
            if (nc < 1 || nc > 8) { check(0, "ncmds %llu", (unsigned long long)nc); break; }
            memcpy(hp->rec, w + OSH_REQUEST, hp->nrec * 8);
            hp->out_used = (size_t)w[OSH_REQUEST + 4];
            if (hp->out_used > OSH_XR_OUTCAP) { check(0, "out_used %zu", hp->out_used); break; }
            for (size_t i = 0; i < hp->out_used; i++) hp->out[i] = (uint8_t)w[OSH_OUT + i];
            hp->idx = (unsigned)w[OSH_EX_PIPE];
            /* the host decoder must accept the record */
            OshRequest *rq = malloc(sizeof *rq);
            int rc = osh_req_decode(w + OSH_REQUEST, hp->nrec, w + OSH_OUT, hp->out_used, rq);
            n_decode++;
            check(rc == 0, "record refused by the host decoder (%d)", rc);
            free(rq);
            uint64_t s = o->fixed_status ? env->status : pstat(o->seed, cur_pipe);
            cur_pipe++;
            env->status = s;
            w[OSH_S_LAST_STATUS] = s;
            r->np++;
            n_pipes++;
            continue;
        }
        r->final = st;
        r->err_off = st >= 200 ? w[OSH_S_ERR_OFF] : 0;
        if (st >= 200 && (st < 213 || st > 217)) check(w[OSH_S_PHASE] == 255 && w[OSH_S_ERR_CODE] == st, "refusal not recorded terminal");
        goto done;
    }
    check(0, "host loop did not settle");
done:
    free(env);
    if (wout) *wout = w;
    else free(w);
}

/* ---- the reference run ---- */
typedef struct {
    uint64_t final;
    uint64_t err_off;
    unsigned np;
    OshXrOut *o[16];
} RRes;

static void ref_run(const uint8_t *d, size_t n, const OshXrEnv *env0, const HOpt *o, RRes *rr)
{
    memset(rr, 0, sizeof *rr);
    OshRefLex *lx = malloc(sizeof *lx);
    OshRefParse *pr = calloc(1, sizeof *pr);
    osh_lex_ref(d, n, o->eoi, lx);
    if (lx->status != 0) { rr->final = lx->status; rr->err_off = lx->status >= 200 ? lx->err_off : 0; goto out; }
    osh_parse_ref(d, n, lx, o->eoi, pr);
    if (pr->status != 0) { rr->final = pr->status; rr->err_off = pr->status >= 200 ? pr->err_off : 0; goto out; }
    OshXrEnv *env = malloc(sizeof *env);
    memcpy(env, env0, sizeof *env);
    unsigned cand = 0, k = 0;
    uint64_t last = env->status;
    for (;;) {
        unsigned p = osh_xr_next(pr, cand, last, o->noskip || o->fixed_status);
        if (p >= pr->npipe) { rr->final = 104; break; }
        OshXrOut *x = malloc(sizeof *x);
        osh_xr_pipe(d, n, lx, pr, env, p, x);
        rr->o[rr->np] = x;
        rr->np++;
        if (x->status != 103) { rr->final = x->status; rr->err_off = x->err_off; break; }
        last = o->fixed_status ? env->status : pstat(o->seed, k);
        k++;
        env->status = last;
        cand = p + 1;
    }
    free(env);
out:
    free(lx);
    free(pr);
}

static void rr_free(RRes *rr)
{
    for (unsigned i = 0; i < rr->np; i++) free(rr->o[i]);
}

/* ---- comparisons ---- */
static void dump_input(const uint8_t *d, size_t n)
{
    fprintf(stderr, "  input (%zu): ", n);
    for (size_t q = 0; q < n && q < 160; q++) {
        if (d[q] >= 32 && d[q] < 127) fputc(d[q], stderr);
        else fprintf(stderr, "\\x%02x", d[q]);
    }
    fprintf(stderr, "\n");
}

static int cmp_ref(const char *tag, const uint8_t *d, size_t n, const HRes *h, const RRes *rr)
{
    n_ref_cmp++;
    int ok = 1;
    if (h->final != rr->final) ok = 0;
    if (rr->final >= 200 && h->err_off != rr->err_off) ok = 0;
    /* pipelines the reference expanded before a refusal are the ones the host saw (the refusing one is last in ref) */
    unsigned want = (rr->final >= 200 && rr->np) ? rr->np - 1 : rr->np;
    if (rr->final < 200 || h->final >= 200) {
        if (h->np != want && h->final == rr->final) ok = 0;
    }
    for (unsigned i = 0; i < h->np && i < rr->np && ok; i++) {
        const OshXrOut *x = rr->o[i];
        const HPipe *p = &h->p[i];
        if (x->status != 103) break;
        if (p->nrec != x->nrec || memcmp(p->rec, x->rec, x->nrec * 8) != 0) ok = 0;
        if (p->out_used != x->out_used || memcmp(p->out, x->out, x->out_used) != 0) ok = 0;
        if (p->nreq != x->nreq) ok = 0;
        else
            for (unsigned q = 0; q < x->nreq; q++)
                if (p->req[q].kind != x->req[q].kind || p->req[q].a != x->req[q].a || p->req[q].len != x->req[q].len) ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "  ref mismatch [%s]: osc final %llu@%llu np %u | ref final %llu@%llu np %u\n", tag, (unsigned long long)h->final,
                (unsigned long long)h->err_off, h->np, (unsigned long long)rr->final, (unsigned long long)rr->err_off, rr->np);
        dump_input(d, n);
        for (unsigned i = 0; i < h->np && i < rr->np; i++) {
            const OshXrOut *x = rr->o[i];
            const HPipe *p = &h->p[i];
            if (x->status != 103) break;
            for (size_t c = 0; c < x->nrec; c++)
                if (p->rec[c] != x->rec[c]) {
                    fprintf(stderr, "  pipe %u rec cell %zu: osc %llu ref %llu\n", i, c, (unsigned long long)p->rec[c], (unsigned long long)x->rec[c]);
                    break;
                }
            if (p->out_used != x->out_used) fprintf(stderr, "  pipe %u out_used osc %zu ref %zu\n", i, p->out_used, x->out_used);
            else
                for (size_t c = 0; c < x->out_used; c++)
                    if (p->out[c] != x->out[c]) { fprintf(stderr, "  pipe %u out byte %zu: osc %u ref %u\n", i, c, p->out[c], x->out[c]); break; }
            if (p->nreq != x->nreq) fprintf(stderr, "  pipe %u nreq osc %u ref %u\n", i, p->nreq, x->nreq);
        }
    }
    check(ok, "reference mismatch [%s]", tag);
    return ok;
}

static int same_hres(const HRes *a, const HRes *b)
{
    if (a->final != b->final || a->err_off != b->err_off || a->np != b->np || a->from_lexer != b->from_lexer) return 0;
    for (unsigned i = 0; i < a->np; i++) {
        if (a->p[i].nrec != b->p[i].nrec || memcmp(a->p[i].rec, b->p[i].rec, a->p[i].nrec * 8)) return 0;
        if (a->p[i].out_used != b->p[i].out_used || memcmp(a->p[i].out, b->p[i].out, a->p[i].out_used)) return 0;
        if (a->p[i].nreq != b->p[i].nreq) return 0;
        for (unsigned q = 0; q < a->p[i].nreq; q++)
            if (a->p[i].req[q].kind != b->p[i].req[q].kind || a->p[i].req[q].a != b->p[i].req[q].a || a->p[i].req[q].len != b->p[i].req[q].len) return 0;
    }
    return 1;
}

/* One input through everything: both engines (single shot, probes), the reference, and a chunked run. */
static void run_case(const char *tag, const uint8_t *d, size_t n, const OshXrEnv *env, int eoi, int noskip, uint64_t seed, uint64_t *rng, unsigned long iter, HRes *keep)
{
    HOpt o;
    memset(&o, 0, sizeof o);
    o.eoi = eoi;
    o.noskip = noskip;
    o.seed = seed;
    o.rng = rng;
    o.probe = 1;
    HRes *a = malloc(sizeof *a), *b = malloc(sizeof *b), *c = malloc(sizeof *c);
    uint64_t *wa, *wb;
    host_run(INTERP, d, n, env, &o, a, &wa);
    host_run(NATIVE, d, n, env, &o, b, &wb);
    n_cases++;
    n_eng_cmp++;
    int same = memcmp(wa, wb, WS * 8) == 0 && same_hres(a, b);
    check(same, "[%s] eoi=%d interp and native disagree (final %llu vs %llu)", tag, eoi, (unsigned long long)a->final, (unsigned long long)b->final);
    RRes rr;
    ref_run(d, n, env, &o, &rr);
    cmp_ref(tag, d, n, a, &rr);
    n_code[a->final & 255]++;
    if (a->final == 104) n_final_ok++;
    else if (a->final >= 200) n_final_err++;
    /* chunked lexing and parsing, either engine, same outcome */
    OshRefLex *lxr = malloc(sizeof *lxr);
    osh_lex_ref(d, n, eoi, lxr);
    int clean = lxr->status == 0;
    free(lxr);
    if (clean) {
        HOpt oc = o;
        oc.chunk_mode = 1 + (unsigned)(rng_next(rng) % 3);
        if (oc.chunk_mode == 1 && n > 48) oc.chunk_mode = 2;
        oc.probe = 0;
        uint64_t *wc;
        host_run((iter & 1) ? INTERP : NATIVE, d, n, env, &oc, c, &wc);
        n_chunk++;
        check(same_hres(a, c), "[%s] eoi=%d chunked (mode %u) differs from single shot (final %llu vs %llu)", tag, eoi, oc.chunk_mode, (unsigned long long)c->final,
              (unsigned long long)a->final);
        free(wc);
    }
    if (keep) memcpy(keep, a, sizeof *keep);
    rr_free(&rr);
    free(wa);
    free(wb);
    free(a);
    free(b);
    free(c);
}

/* ---- the vector grammar ---- */
static void fq(char *out, size_t *o, const uint8_t *s, size_t n)
{
    int rep = n >= 64;
    for (size_t i = 1; i < n && rep; i++)
        if (s[i] != s[0]) rep = 0;
    if (rep) {
        *o += (size_t)sprintf(out + *o, "\"%c\"*%zu", s[0], n);
        return;
    }
    out[(*o)++] = '"';
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') { out[(*o)++] = '\\'; out[(*o)++] = (char)c; }
        else if (c < 0x20 || c > 0x7e) *o += (size_t)sprintf(out + *o, "\\x%02x", c);
        else out[(*o)++] = (char)c;
    }
    out[(*o)++] = '"';
}

static void fmt_result(const HRes *h, int eoi, char *out)
{
    size_t o = 0;
    (void)eoi;
    if (h->final == 100) { strcpy(out, "more"); return; }
    if (h->final >= 200) {
        const char *nm = osh_code_name((unsigned)h->final);
        sprintf(out, "refuse %llu %s off=%llu", (unsigned long long)h->final, nm ? nm : "?", (unsigned long long)h->err_off);
        return;
    }
    o += (size_t)sprintf(out, "ok");
    if (h->lex_resumes) o += (size_t)sprintf(out + o, " lex_resumes=%u", h->lex_resumes);
    for (unsigned i = 0; i < h->np; i++) {
        const HPipe *p = &h->p[i];
        o += (size_t)sprintf(out + o, " | P%u conn=%llu", p->idx, (unsigned long long)p->rec[2]);
        uint64_t nc = p->rec[1];
        for (uint64_t c = 0; c < nc; c++) {
            const uint64_t *b = p->rec + 8 + c * 164;
            o += (size_t)sprintf(out + o, " | C%llu b=%llu argv=", (unsigned long long)c, (unsigned long long)b[3]);
            for (uint64_t k = 0; k < b[0]; k++) {
                if (k) out[o++] = ',';
                fq(out, &o, p->out + b[4 + 2 * k], (size_t)b[5 + 2 * k]);
            }
            o += (size_t)sprintf(out + o, " as=");
            for (uint64_t k = 0; k < b[1]; k++) {
                const uint64_t *e = b + 68 + 4 * k;
                if (k) out[o++] = ',';
                memcpy(out + o, p->out + e[0], (size_t)e[1]);
                o += (size_t)e[1];
                out[o++] = '=';
                fq(out, &o, p->out + e[2], (size_t)e[3]);
            }
            o += (size_t)sprintf(out + o, " rd=");
            for (uint64_t k = 0; k < b[2]; k++) {
                const uint64_t *e = b + 132 + 4 * k;
                static const char *const kn[] = {"?", "IN", "OUT", "APPEND", "DUP"};
                if (k) out[o++] = ',';
                o += (size_t)sprintf(out + o, "%s%llu:", kn[e[0] < 5 ? e[0] : 0], (unsigned long long)e[1]);
                if (e[0] == 4) o += (size_t)sprintf(out + o, "%llu", (unsigned long long)e[2]);
                else fq(out, &o, p->out + e[2], (size_t)e[3]);
            }
        }
    }
    out[o] = 0;
}

/* ---- conformance vectors ---- */
static int run_vectors(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/expected.txt", dir);
    size_t el;
    char *exp = slurp(path, &el);
    if (!exp) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    size_t i = 0;
    uint64_t rng = 0x1234567ULL;
    while (i < el) {
        size_t e = i;
        while (e < el && exp[e] != '\n') e++;
        char line[8192];
        size_t ll = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
        memcpy(line, exp + i, ll);
        line[ll] = 0;
        i = e + 1;
        if (ll == 0 || line[0] == '#') continue;
        char vn[16];
        const char *sp = strchr(line, ' ');
        if (!sp || (size_t)(sp - line) >= sizeof vn) continue;
        memcpy(vn, line, (size_t)(sp - line));
        vn[sp - line] = 0;
        char fn[600];
        snprintf(fn, sizeof fn, "%s/%s.in", dir, vn);
        size_t dl;
        char *in = slurp(fn, &dl);
        if (!in) { check(0, "missing %s", fn); continue; }
        OshXrEnv *env = calloc(1, sizeof *env);
        snprintf(fn, sizeof fn, "%s/%s.env", dir, vn);
        size_t vl;
        char *ev = slurp(fn, &vl);
        if (ev) { env_parse(env, ev, vl, 0); free(ev); }
        HOpt o;
        memset(&o, 0, sizeof o);
        o.fixed_status = 1;
        o.noskip = 1;
        o.rng = &rng;
        o.probe = 1;
        for (Engine en = INTERP; en <= NATIVE; en++) {
            HRes *h = malloc(sizeof *h);
            host_run(en, (const uint8_t *)in, dl, env, &o, h, NULL);
            char got[16384];
            fmt_result(h, 0, got);
            n_vec++;
            int ok = strcmp(got, line + (sp - line) + 1) == 0;
            if (!ok) {
                fprintf(stderr, "  vector %s %s:\n    want: %s\n    got:  %s\n", vn, en == INTERP ? "interp" : "native", sp + 1, got);
            }
            check(ok, "vector %s %s differs from expected.txt", vn, en == INTERP ? "interp" : "native");
            free(h);
        }
        /* and the full differential on the vector input */
        run_case(vn, (const uint8_t *)in, dl, env, 0, 1, 0, &rng, n_vec, NULL);
        free(env);
        free(in);
    }
    free(exp);
    return 0;
}

/* ---- hostile table: input, environment, expected line in the vector grammar (pipelines run with status 0, no skipping) ---- */
typedef struct {
    const char *name, *in, *env, *want;
} Case;

#define Q "\""
static const Case CASES[] = {
    {"plain", "echo a b\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\",\"b\" as= rd="},
    {"single quotes keep everything", "echo 'a  $X \\ b'\n", "X=v", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a  $X \\\\ b\" as= rd="},
    {"double quotes expand", "echo \"a  $X  b\"\n", "X=v", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a  v  b\" as= rd="},
    {"unquoted split", "echo $S\n", "S=a b  c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\",\"b\",\"c\" as= rd="},
    {"quoted no split", "echo \"$S\"\n", "S=a b  c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a b  c\" as= rd="},
    {"empty unquoted dropped", "echo $E x\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\" as= rd="},
    {"unset unquoted dropped", "echo $U x\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\" as= rd="},
    {"empty quoted kept", "echo \"$E\" '' \"\" x\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\",\"\",\"\",\"x\" as= rd="},
    {"quoted unset kept", "echo \"$U\"\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\" as= rd="},
    {"empty beside literal", "echo a${E}b\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"ab\" as= rd="},
    {"IFS value beside literal", "echo a${S}b\n", "S= x ", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\",\"x\",\"b\" as= rd="},
    {"leading IFS joins nothing", "echo a$L\n", "L= b", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\",\"b\" as= rd="},
    {"trailing IFS then literal", "echo ${T}z\n", "T=b ", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"b\",\"z\" as= rd="},
    {"all IFS value drops", "echo x $S y\n", "S=   ", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\",\"y\" as= rd="},
    {"all IFS beside literal", "echo x$S\n", "S=  ", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\" as= rd="},
    {"tab and newline split", "echo $W\n", "W=w\\tx\\ny", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"w\",\"x\",\"y\" as= rd="},
    {"two expansions join", "echo $A$B\n", "A=1\nB=2", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"12\" as= rd="},
    {"expansion ends at non-name", "echo $A-$B.x\n", "A=1\nB=2", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"1-2.x\" as= rd="},
    {"braces", "echo ${A}b ${A}_ $A_\n", "A=1\nA_=u", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"1b\",\"1_\",\"u\" as= rd="},
    {"positional", "echo $0 $1 $2 $3 ${1}x $10\n", "@arg0=osh\n@arg1=a\n@arg2=b c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"osh\",\"a\",\"b\",\"c\",\"ax\",\"a0\" as= rd="},
    {"count and status", "echo $# $?\n", "@status=7\n@arg1=a\n@arg2=b", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"2\",\"7\" as= rd="},
    {"quoted at", "echo \"$@\"\n", "@arg1=a b\n@arg2=\n@arg3=c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a b\",\"\",\"c\" as= rd="},
    {"quoted at, none", "echo x \"$@\" y\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\",\"y\" as= rd="},
    {"quoted at with text", "echo \"a$@b\"\n", "@arg1=1\n@arg2=2", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a1\",\"2b\" as= rd="},
    {"quoted at with text, none", "echo \"a$@b\"\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"ab\" as= rd="},
    {"quoted at then empty", "echo \"$@\"\"\"\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\" as= rd="},
    {"quoted at then var, none (bash: no field)", "echo \"$@$E\"\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\" as= rd="},
    {"quoted at single", "echo \"$@\"\n", "@arg1=only", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"only\" as= rd="},
    {"quoted at empties", "echo \"$@\"\n", "@arg1=\n@arg2=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\",\"\" as= rd="},
    {"quoted var then at, none (bash: no field)", "echo \"$E$@\"\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\" as= rd="},
    {"quoted positional then at, none", "echo \"$1$@\" x\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\" as= rd="},
    {"quoted var alone, empty, is one field", "echo \"$E\"\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\" as= rd="},
    {"quoted star then var, none, is one field", "echo \"$*$E\"\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\" as= rd="},
    {"quoted var with value then at, none", "echo \"$X$@\"\n", "X=v", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"v\" as= rd="},
    {"unquoted at splits", "echo $@\n", "@arg1=a b\n@arg2=\n@arg3=c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\",\"b\",\"c\" as= rd="},
    {"unquoted star splits", "echo $*\n", "@arg1=a b\n@arg2=c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\",\"b\",\"c\" as= rd="},
    {"quoted star joins", "echo \"$*\"\n", "@arg1=a b\n@arg2=\n@arg3=c", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a b  c\" as= rd="},
    {"quoted star none", "echo \"$*\" x\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\",\"x\" as= rd="},
    {"unquoted at none", "echo $@ x\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\" as= rd="},
    {"unquoted at joins text", "echo x$@y\n", "@arg1=a\n@arg2=b", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"xa\",\"by\" as= rd="},
    {"unquoted at, empty positionals", "echo x$@y\n", "@arg1=\n@arg2=", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"x\",\"y\" as= rd="},
    {"backslash unquoted", "echo a\\ b \\$X \\\\ \\\"\n", "X=v", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a b\",\"$X\",\"\\\\\",\"\\\"\" as= rd="},
    {"backslash in double quotes", "echo \"\\$X \\\" \\\\ \\a \\`\"\n", "X=v", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"$X \\\" \\\\ \\\\a `\" as= rd="},
    {"continuation", "ec\\\nho a\\\nb\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"ab\" as= rd="},
    {"continuation in double quotes", "echo \"a\\\nb\"\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"ab\" as= rd="},
    {"adjacent quotes join", "echo 'a'\"b\"c$X\n", "X=v", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"abcv\" as= rd="},
    {"literal dollar", "echo $ \"$\" a$ $/ \"$.\"\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"$\",\"$\",\"a$\",\"$/\",\"$.\" as= rd="},
    {"quoted glob chars are literal", "echo '*' \"?\" \"[x\"\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"*\",\"?\",\"[x\" as= rd="},
    {"glob in quoted value fine", "echo \"$G\"\n", "G=a*b", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a*b\" as= rd="},
    {"glob in unquoted value refused", "echo $G\n", "G=a*b", "refuse 245 VALUE_GLOB off=5"},
    {"question in unquoted value refused", "echo x $G\n", "G=a?b", "refuse 245 VALUE_GLOB off=7"},
    {"bracket in unquoted value refused", "echo $G\n", "G=a[b", "refuse 245 VALUE_GLOB off=5"},
    {"glob in positional unquoted at refused", "echo $@\n", "@arg1=*", "refuse 245 VALUE_GLOB off=5"},
    {"glob refused in redirect target", "echo >$G\n", "G=a*", "refuse 245 VALUE_GLOB off=6"},
    {"glob fine in assignment value", "A=$G\n", "G=a*", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"a*\" rd="},
    {"NUL in value refused", "echo \"$N\"\n", "N=a\\0b", "refuse 244 VALUE_NUL off=5"},
    {"NUL in unquoted value refused", "echo $N\n", "N=a\\0b", "refuse 244 VALUE_NUL off=5"},
    {"assignment only", "A=1 B=\"x y\" C=$S\n", "S=p q", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"1\",B=\"x y\",C=\"p q\" rd="},
    {"assignment empty value", "A= B=''\n", "", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"\",B=\"\" rd="},
    {"assignment not split", "A=$S\n", "S=a  b", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"a  b\" rd="},
    {"assignment at joins", "A=$@\n", "@arg1=a b\n@arg2=c", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"a b c\" rd="},
    {"assignment quoted at joins", "A=\"$@\"\n", "@arg1=a\n@arg2=c", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"a c\" rd="},
    {"assignment star none", "A=$*\n", "", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"\" rd="},
    {"prefix assignment sees old value", "A=1 B=$A cmd\n", "A=old", "ok | P0 conn=1 | C0 b=0 argv=\"cmd\" as=A=\"1\",B=\"old\" rd="},
    {"standalone assignments are sequential", "A=1 B=$A\n", "A=old", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"1\",B=\"1\" rd="},
    {"standalone sequential, latest wins", "A=1 A=$A$A B=${A}x\n", "", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"1\",A=\"11\",B=\"11x\" rd="},
    {"standalone sequential, other name from host", "A=1 B=$C\n", "C=h", "ok | P0 conn=1 | C0 b=0 argv= as=A=\"1\",B=\"h\" rd="},
    {"assignment after a word is a word", "echo A=$S\n", "S=a b", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"A=a\",\"b\" as= rd="},
    {"assignment after an empty expansion is a word", "$E A=1\n", "E=", "ok | P0 conn=1 | C0 b=0 argv=\"A=1\" as= rd="},
    {"assignment words, redirection between", "A=1 >f B=2 cmd\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"cmd\" as=A=\"1\",B=\"2\" rd=OUT1:\"f\""},
    {"redirections", "cat <in >out >>app 2>err\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"cat\" as= rd=IN0:\"in\",OUT1:\"out\",APPEND1:\"app\",OUT2:\"err\""},
    {"dup order is kept", "ls 2>&1 >o\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"ls\" as= rd=DUP2:1,OUT1:\"o\""},
    {"dup order is kept 2", "ls >o 2>&1\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"ls\" as= rd=OUT1:\"o\",DUP2:1"},
    {"dup in", "cat <&0 >&2 1>&2\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"cat\" as= rd=DUP0:0,DUP1:2,DUP1:2"},
    {"redirect target expands", "cat <$X >\"$X\"\n", "X=f.txt", "ok | P0 conn=1 | C0 b=0 argv=\"cat\" as= rd=IN0:\"f.txt\",OUT1:\"f.txt\""},
    {"redirect target empty quoted", "cat > ''\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"cat\" as= rd=OUT1:\"\""},
    {"redirect target splits to two", "cat > $S\n", "S=a b", "refuse 243 AMBIGUOUS_REDIRECT off=6"},
    {"redirect target empty", "cat > $E\n", "E=", "refuse 243 AMBIGUOUS_REDIRECT off=6"},
    {"redirect target unset", "cat > $U\n", "", "refuse 243 AMBIGUOUS_REDIRECT off=6"},
    {"redirect target quoted at none", "cat > \"$@\"\n", "", "refuse 243 AMBIGUOUS_REDIRECT off=6"},
    {"redirect target quoted at one", "cat > \"$@\"\n", "@arg1=f", "ok | P0 conn=1 | C0 b=0 argv=\"cat\" as= rd=OUT1:\"f\""},
    {"redirect target one field from split", "cat > $S\n", "S= f ", "ok | P0 conn=1 | C0 b=0 argv=\"cat\" as= rd=OUT1:\"f\""},
    {"pipeline and connectors", "a | b | c; d && e || f\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"a\" as= rd= | C1 b=0 argv=\"b\" as= rd= | C2 b=0 argv=\"c\" as= rd= | P1 conn=2 | C0 b=0 argv=\"d\" as= rd= | P2 conn=3 | C0 b=0 argv=\"e\" as= rd= | P3 conn=1 | C0 b=0 argv=\"f\" as= rd="},
    {"builtins", "cd /tmp; pwd; printf x; export A=1; unset A; exit 3\n", "", "ok | P0 conn=1 | C0 b=1 argv=\"cd\",\"/tmp\" as= rd= | P1 conn=1 | C0 b=2 argv=\"pwd\" as= rd= | P2 conn=1 | C0 b=3 argv=\"printf\",\"x\" as= rd= | P3 conn=1 | C0 b=4 argv=\"export\",\"A=1\" as= rd= | P4 conn=1 | C0 b=5 argv=\"unset\",\"A\" as= rd= | P5 conn=1 | C0 b=6 argv=\"exit\",\"3\" as= rd="},
    {"builtin by variable", "$B x\n", "B=cd", "ok | P0 conn=1 | C0 b=1 argv=\"cd\",\"x\" as= rd="},
    {"builtin quoted", "\"cd\" 'pwd'\n", "", "ok | P0 conn=1 | C0 b=1 argv=\"cd\",\"pwd\" as= rd="},
    {"builtin name as argument is not a builtin", "echo cd exit\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"cd\",\"exit\" as= rd="},
    {"builtin after assignment", "A=1 cd x\n", "", "ok | P0 conn=1 | C0 b=1 argv=\"cd\",\"x\" as=A=\"1\" rd="},
    {"builtin in a pipeline", "cd x | cat\n", "", "ok | P0 conn=1 | C0 b=1 argv=\"cd\",\"x\" as= rd= | C1 b=0 argv=\"cat\" as= rd="},
    {"unsupported set", "set -e\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"unsupported via variable", "echo ok; $B -e\n", "B=eval", "refuse 239 UNSUPPORTED_BUILTIN off=9"},
    {"unsupported quoted", "'shift'\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"unsupported dot", ". ./x\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"unsupported after empty expansion", "$E source x\n", "E=", "refuse 239 UNSUPPORTED_BUILTIN off=3"},
    {"unsupported in second command", "a | exec b\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=4"},
    {"each unsupported name", "trap\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"read is unsupported", "read x\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"continue return break local readonly alias wait", "wait\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"export NAME=value is not split", "export A=$Y B\n", "Y=a  b", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"A=a  b\",\"B\" as= rd="},
    {"export value keeps glob bytes", "export A=$Y\n", "Y=*", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"A=*\" as= rd="},
    {"export value joins $@", "export A=$@ C=\"$@\"\n", "@arg1=p\n@arg2=q", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"A=p q\",\"C=p q\" as= rd="},
    {"quoted export word is not an assignment: $@ splits (bash)", "export \"B=$@\"\n", "@arg1=p\n@arg2=q", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"B=p\",\"q\" as= rd="},
    {"export through a variable splits (bash)", "$E A=$Y\n", "E=export\nY=a b", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"A=a\",\"b\" as= rd="},
    {"quoted export splits (bash)", "\"export\" A=$Y\n", "Y=a b", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"A=a\",\"b\" as= rd="},
    {"non NAME= argument of export still splits", "export $Y\n", "Y=a b", "ok | P0 conn=1 | C0 b=4 argv=\"export\",\"a\",\"b\" as= rd="},
    {"assignment-like argument of another command splits", "echo A=$Y\n", "Y=a b", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"A=a\",\"b\" as= rd="},
    {"colon is unsupported", ": x\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"declare is unsupported", "declare x=1\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"complete (8 bytes) is unsupported", "complete\n", "", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"shopt via variable is unsupported", "$S -s x\n", "S=shopt", "refuse 239 UNSUPPORTED_BUILTIN off=0"},
    {"external lookalike", "settings readx evalx\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"settings\",\"readx\",\"evalx\" as= rd="},
    {"long name is not a builtin", "continuex\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"continuex\" as= rd="},
    {"blank list", "\n", "", "ok"},
    {"comment", "echo a # not b $X\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a\" as= rd="},
    {"hash inside word", "echo a#b\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"a#b\" as= rd="},
    {"high bytes pass through", "echo \xc3\xa9 \xff\n", "", "ok | P0 conn=1 | C0 b=0 argv=\"echo\",\"\\xc3\\xa9\",\"\\xff\" as= rd="},
    {"lexer refusal keeps its code", "echo $(x)\n", "", "refuse 226 CMDSUB off=6"},
    {"parser refusal keeps its code", "if x\n", "", "refuse 233 IF_COMPOUND off=0"},
    {"more input", "echo \"abc", "", "more"},
};

static void run_hostile(uint64_t *rng)
{
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const Case *c = &CASES[i];
        OshXrEnv *env = calloc(1, sizeof *env);
        env_parse(env, c->env, strlen(c->env), 1);
        HOpt o;
        memset(&o, 0, sizeof o);
        o.fixed_status = 1;
        o.noskip = 1;
        o.rng = rng;
        o.probe = 1;
        for (Engine en = INTERP; en <= NATIVE; en++) {
            HRes *h = malloc(sizeof *h);
            host_run(en, (const uint8_t *)c->in, strlen(c->in), env, &o, h, NULL);
            char got[16384];
            fmt_result(h, 0, got);
            int ok = strcmp(got, c->want) == 0;
            if (!ok) fprintf(stderr, "  case [%s] %s:\n    want: %s\n    got:  %s\n", c->name, en == INTERP ? "interp" : "native", c->want, got);
            check(ok, "hostile [%s] %s", c->name, en == INTERP ? "interp" : "native");
            free(h);
        }
        n_hostile++;
        run_case(c->name, (const uint8_t *)c->in, strlen(c->in), env, 0, 1, 0, rng, i, NULL);
        free(env);
    }
}

/* ---- capacities and larger shapes (built at run time) ---- */
static size_t setb(uint8_t *b, const char *s)
{
    size_t l = strlen(s);
    memcpy(b, s, l);
    return l;
}

static void run_caps(uint64_t *rng)
{
    OshXrEnv *env = calloc(1, sizeof *env);
    static uint8_t buf[8192], val[1200];
    size_t n;
    HRes *h = malloc(sizeof *h);
    HOpt o;
    memset(&o, 0, sizeof o);
    o.fixed_status = 1;
    o.noskip = 1;
    o.rng = rng;
    o.probe = 1;
#define RUN_EXPECT(label, wantfinal, wantoff) \
    do { \
        for (Engine en = INTERP; en <= NATIVE; en++) { \
            host_run(en, buf, n, env, &o, h, NULL); \
            check(h->final == (wantfinal) && (((wantfinal) < 200) || (long long)(wantoff) < 0 || h->err_off == (wantoff)), "cap [%s] %s: final %llu@%llu want %d@%d", label, \
                  en == INTERP ? "interp" : "native", (unsigned long long)h->final, (unsigned long long)h->err_off, (int)(wantfinal), (int)(wantoff)); \
        } \
        n_hostile++; \
        run_case(label, buf, n, env, 0, 1, 0, rng, 0, NULL); \
    } while (0)

    /* 32 fields: ok; 33: CAP_FIELDS */
    n = 0; n = setb(buf, "echo");
    for (int i = 0; i < 31; i++) { buf[n++] = ' '; buf[n++] = 'x'; }
    buf[n++] = '\n';
    RUN_EXPECT("32 argv words", 104, 0);
    n = 0; n = setb(buf, "echo");
    for (int i = 0; i < 31; i++) { buf[n++] = ' '; buf[n++] = 'x'; }
    n += setb(buf + n, " y\n");
    RUN_EXPECT("33 argv words (CAP_WORDS)", 206, n - 2);
    /* split into 33 fields: the 33rd closes at the word end */
    {
        size_t k = 0;
        for (int i = 0; i < 33; i++) { val[k++] = 'f'; val[k++] = ' '; }
        env_set_var(env, "F", val, k);
    }
    n = setb(buf, "echo $F\n");
    RUN_EXPECT("split into 33 fields", 209, 5);
    n = setb(buf, "$F\n");
    RUN_EXPECT("split into 33 fields (first word)", 209, 0);
    {
        size_t k = 0;
        for (int i = 0; i < 32; i++) { val[k++] = 'f'; val[k++] = ' '; }
        env_set_var(env, "F", val, k);
    }
    n = setb(buf, "$F\n");
    RUN_EXPECT("split into 32 fields", 104, 0);
    /* values: 1024 ok, 1025 CAP_VALUE */
    memset(val, 'v', 1024);
    env_set_var(env, "V1024", val, 1024);
    memset(val, 'v', 1025);
    env_set_var(env, "V1025", val, 1025);
    n = setb(buf, "echo \"$V1024\"\n");
    RUN_EXPECT("value of 1024 bytes", 104, 0);
    n = setb(buf, "echo \"$V1025\"\n");
    RUN_EXPECT("value of 1025 bytes", 211, 5);
    n = setb(buf, "echo $V1025\n");
    RUN_EXPECT("value of 1025 bytes unquoted", 211, 5);
    /* CAP_OUT is 8192 bytes of argv strings in one pipeline, the command name included */
    memset(val, 'x', 1023);
    val[1023] = 0;
    n = setb(buf, "e");
    for (int i = 0; i < 7; i++) n += setb(buf + n, " \"$V1024\"");
    n += setb(buf + n, " ");
    n += setb(buf + n, (const char *)val);
    n += setb(buf + n, "\n");
    RUN_EXPECT("8192 expanded bytes", 104, -1);
    n = setb(buf, "e");
    for (int i = 0; i < 7; i++) n += setb(buf + n, " \"$V1024\"");
    n += setb(buf + n, " ");
    n += setb(buf + n, (const char *)val);
    n += setb(buf + n, "x\n");
    RUN_EXPECT("8193 expanded bytes", 210, -1);
    n = 0;
    for (int i = 0; i < 8; i++) n += setb(buf + n, "A=\"$V1024\" ");
    buf[n - 1] = '\n';
    RUN_EXPECT("assignment names and values count toward CAP_OUT", 210, -1);
    /* CAP_VARREQ: 64 requests ok, 65 refused (words are capped at 32 per command, so spread over commands) */
    val[0] = 'x';
    env_set_var(env, "X", val, 1);
    {
        static const unsigned c64[] = {22, 21, 21}, c65[] = {22, 22, 21};
        for (int v = 0; v < 2; v++) {
            const unsigned *cn = v ? c65 : c64;
            n = 0;
            for (int c = 0; c < 3; c++) {
                n += setb(buf + n, "echo");
                for (unsigned i = 0; i < cn[c]; i++) n += setb(buf + n, " $X");
                n += setb(buf + n, c < 2 ? " | " : "\n");
            }
            if (v) RUN_EXPECT("65 variable requests over three commands", 212, -1);
            else RUN_EXPECT("64 variable requests over three commands", 104, 0);
        }
    }
    /* "$@" costs 1 + npos requests */
    for (int k = 1; k <= 10; k++) {
        uint8_t v = (uint8_t)('a' + k % 26);
        env_set_pos(env, k, &v, 1);
    }
    n = setb(buf, "echo \"$@\"\n");
    RUN_EXPECT("quoted at, 10 positionals", 104, 0);
    n = setb(buf, "echo \"$@\" \"$@\" | echo \"$@\" \"$@\"\n");
    RUN_EXPECT("quoted at, 44 requests", 104, 0);
    n = setb(buf, "echo \"$@\" \"$@\" | echo \"$@\" \"$@\" | echo \"$@\" \"$@\"\n");
    RUN_EXPECT("quoted at, 66 requests", 212, -1);
    n = setb(buf, "echo \"$@\" \"$@\" \"$@\" \"$@\"\n");
    RUN_EXPECT("quoted at four times (41 fields)", 209, -1);
    /* budget: a 1024 byte value copied many times needs several calls */
    n = setb(buf, "echo \"$V1024\" \"$V1024\" \"$V1024\"\n");
    RUN_EXPECT("3072 bytes, several budget resumes", 104, 0);
    host_run(INTERP, buf, n, env, &o, h, NULL);
    check(h->budget_resumes >= 2, "budget resumes %u", h->budget_resumes);
    free(h);
    free(env);
#undef RUN_EXPECT
}

/* ---- header refusals and corrupted state ---- */
static void run_abi(uint64_t *rng)
{
    const char *line = "echo $X\n";
    size_t n = strlen(line);
    OshXrEnv *env = calloc(1, sizeof *env);
    for (Engine en = INTERP; en <= NATIVE; en++) {
        uint64_t *w = sess_new();
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 217, "expand_run before the lexer and parser: ABI_RESERVED");
        free(w);
        w = sess_new();
        w[OSH_S_MAGIC] = 1;
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 214, "bad magic");
        free(w);
        w = sess_new();
        w[OSH_S_VERSION] = 2;
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 215, "bad version");
        free(w);
        w = sess_new();
        check(ucall(OSH_U_EXPAND, en, w, 1, (const uint8_t *)line, n) == 216, "short header");
        free(w);
        w = sess_new();
        check(ucall(OSH_U_EXPAND, en, w, WS - 1, (const uint8_t *)line, n) == 213, "workspace one cell short");
        free(w);
        w = sess_new();
        w[50] = 1;
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 217, "reserved cell");
        free(w);
        /* a refusal is terminal and repeated */
        w = sess_new();
        int fl;
        unsigned lr = 0;
        check(lex_parse(en, w, (const uint8_t *)line, n, &fl, &lr) == 0, "lex+parse");
        uint64_t st = ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n);
        check(st == 102, "NEED_VAR expected, got %llu", (unsigned long long)st);
        w[OSH_STAGING] = 1;
        w[OSH_STAGING + 1] = 2;
        w[OSH_STAGING + 4] = 'a';
        w[OSH_STAGING + 5] = '*';
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 245, "VALUE_GLOB");
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 245, "refusal repeats");
        check(w[OSH_S_PHASE] == 255 && w[OSH_S_ERR_OFF] == 5, "terminal with offset");
        free(w);
        /* a staging value of 1025 with found=1 */
        w = sess_new();
        lex_parse(en, w, (const uint8_t *)line, n, &fl, &lr);
        ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n);
        w[OSH_STAGING] = 1;
        w[OSH_STAGING + 1] = 1025;
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 211, "CAP_VALUE from staging length");
        free(w);
        /* an unanswered request stays 102 and changes nothing */
        w = sess_new();
        lex_parse(en, w, (const uint8_t *)line, n, &fl, &lr);
        ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n);
        uint64_t *snap = malloc(WS * 8);
        memcpy(snap, w, WS * 8);
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 102 && memcmp(snap, w, WS * 8) == 0, "unanswered repeat");
        free(snap);
        free(w);
        /* corrupt expander cells */
        w = sess_new();
        lex_parse(en, w, (const uint8_t *)line, n, &fl, &lr);
        w[OSH_EX_INIT] = 1;
        w[OSH_EX_ST] = 9;
        check(ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, n) == 217, "corrupt state cell");
        free(w);
        /* an input slice shorter than the tokens say: bounds trap or refusal, never a read outside */
        w = sess_new();
        lex_parse(en, w, (const uint8_t *)line, n, &fl, &lr);
        uint64_t r = ucall(OSH_U_EXPAND, en, w, WS, (const uint8_t *)line, 3);
        check(r == OSH_CORE_FAULT || r >= 100, "short input slice returned %llu", (unsigned long long)r);
        free(w);
        /* NULL/empty input slice with a workspace that expects bytes */
        w = sess_new();
        lex_parse(en, w, (const uint8_t *)line, n, &fl, &lr);
        r = ucall(OSH_U_EXPAND, en, w, WS, NULL, 0);
        check(r == OSH_CORE_FAULT || r >= 100, "empty input slice returned %llu", (unsigned long long)r);
        free(w);
    }
    (void)rng;
    free(env);
}

/* flip random cells of a valid post-parse workspace and call the expander on both engines: no memory error (ASan),
 * same outcome and same workspace on both engines (a trap is an outcome) */
static void run_corrupt(uint64_t *rng, unsigned count)
{
    static const char *const seeds[] = {
        "echo a$X \"b $S\" 'c' >out 2>&1 <in\n",
        "A=1 B=$A c=$@ cmd \"$@\" $* ${1}x && x | y || z; q\n",
        "cd /tmp; pwd | cat > $E\n",
        "echo \"$#\" $? ${0}\n",
    };
    OshXrEnv *env = calloc(1, sizeof *env);
    { const char *et = "X=v\nS=a b\nE=\n@arg1=p\n@arg2=q r\n"; env_parse(env, et, strlen(et), 0); }
    HRes *dummy = malloc(sizeof *dummy);
    (void)dummy;
    for (unsigned it = 0; it < count; it++) {
        const char *line = seeds[it % 4];
        size_t n = strlen(line);
        uint64_t *wa = sess_new(), *wb;
        int fl;
        unsigned lr = 0;
        lex_parse(INTERP, wa, (const uint8_t *)line, n, &fl, &lr);
        wa[OSH_S_NOSKIP] = rng_next(rng) & 1;
        /* corrupt */
        unsigned nf = 1 + (unsigned)(rng_next(rng) % 4);
        for (unsigned f = 0; f < nf; f++) {
            uint64_t r = rng_next(rng);
            size_t cell;
            switch (r % 5) {
            case 0: cell = OSH_TOKENS + (r >> 8) % 64; break;
            case 1: cell = OSH_CMDS + (r >> 8) % 48; break;
            case 2: cell = OSH_PIPES + (r >> 8) % 24; break;
            case 3: cell = 12 + (r >> 8) % 36; break;
            default: cell = OSH_STAGING + (r >> 8) % 8; break;
            }
            uint64_t v = rng_next(rng);
            switch ((v >> 60) % 4) {
            case 0: v &= 0xFF; break;
            case 1: v &= 0xFFFF; break;
            case 2: v = ~0ULL - (v & 3); break;
            default: v &= 0xFFFFFFFFULL; break;
            }
            wa[cell] = v;
        }
        wb = malloc(WS * 8);
        memcpy(wb, wa, WS * 8);
        uint64_t ra = ~0ULL, rb = ~0ULL;
        for (int step = 0; step < 6; step++) {
            ra = ucall(OSH_U_EXPAND, INTERP, wa, WS, (const uint8_t *)line, n);
            rb = ucall(OSH_U_EXPAND, NATIVE, wb, WS, (const uint8_t *)line, n);
            if (ra != rb) break;
            if (ra == OSH_CORE_FAULT) break;
            if (ra == 102) {
                for (uint64_t *w = wa, k = 0; k < 2; k++, w = wb) {
                    w[OSH_STAGING] = 1;
                    w[OSH_STAGING + 1] = 1;
                    w[OSH_STAGING + 2] = 2;
                    w[OSH_STAGING + 4] = 'q';
                }
                continue;
            }
            if (ra == 103) continue;
            break;
        }
        n_corrupt++;
        if (ra == OSH_CORE_FAULT) n_corrupt_trap++;
        check(ra == rb && memcmp(wa, wb, WS * 8) == 0, "corrupt %u: engines disagree (%llx vs %llx, workspace %s)", it, (unsigned long long)ra, (unsigned long long)rb,
              memcmp(wa, wb, WS * 8) == 0 ? "same" : "differs");
        free(wa);
        free(wb);
    }
    free(dummy);
    free(env);
}

/* ---- fuzz ---- */
static uint64_t g_rng_state;
static unsigned rnd(unsigned m) { return (unsigned)(rng_next(&g_rng_state) % m); }

static const char *const FWORD[] = {"a", "bc", "x-y", "/tmp/f", "=", "1", "a=b", "-n", "it's", "A", "foo.txt", "#h", "{", "}", "x\\ y", "\\*", "\\$X", "\\\"", "\\'"};
static const char *const FVAR[] = {"$X", "${X}", "$S", "${S}", "$E", "$U", "$L", "$T", "$G", "$Q", "$W", "$B", "$M", "$1", "$2", "$3", "$0", "$?", "$#", "$@", "$*",
                                   "${1}", "$9", "$", "${E}x", "$X$S", "$X-$S", "${X}_", "$X_", "$H", "$Z", "$N", "$V"};
static const char *const FDQ[] = {"a", "b c", "  ", "", "\\$", "\\\"", "\\\\", "\\n", "'", "`x", "$X", "${S}", "$S", "$E", "$U", "$@", "$*", "$#", "$?", "$1", "$G", "$", "*", "?", "[", "\\\n"};
static const char *const FSQ[] = {"a", "b c", "", "$X", "\\", "\"", "*"};
static const char *const FOP[] = {" ;", " &&", " ||", " |", " ;", " |"};
static const char *const FREDIR[] = {">", ">>", "<", "2>", "2>>", "0<", "1>", "2>&1", ">&2", "<&0", "1>&2", ">&1", "2<"};
static const char *const FJUNK[] = {"*", "?", "[", "~", "`", "(", ")", "&", "$(", "$((", "${", "${X", "${10}", "${X:-y}", "$$", "$!", "<<", "<<<", ">|", "{a,b}", "\\", "\"", "'", "!", "if", "set", "exec", "cd", "pwd", "exit", "printf", "export", "unset", "IFS=x", "wait", "time"};

static size_t gen_word(char *b, size_t n)
{
    size_t o = 0;
    unsigned parts = 1 + rnd(4);
    for (unsigned i = 0; i < parts; i++) {
        const char *s;
        switch (rnd(8)) {
        case 0: case 1: s = FWORD[rnd(sizeof FWORD / sizeof *FWORD)]; break;
        case 2: case 3: s = FVAR[rnd(sizeof FVAR / sizeof *FVAR)]; break;
        case 4: {
            o += (size_t)snprintf(b + o, n - o, "\"");
            unsigned k = rnd(4);
            for (unsigned j = 0; j < k; j++) o += (size_t)snprintf(b + o, n - o, "%s", FDQ[rnd(sizeof FDQ / sizeof *FDQ)]);
            o += (size_t)snprintf(b + o, n - o, "\"");
            continue;
        }
        case 5: o += (size_t)snprintf(b + o, n - o, "'%s'", FSQ[rnd(sizeof FSQ / sizeof *FSQ)]); continue;
        case 6: s = rnd(3) == 0 ? FJUNK[rnd(sizeof FJUNK / sizeof *FJUNK)] : FVAR[rnd(sizeof FVAR / sizeof *FVAR)]; break;
        default: s = ""; break;
        }
        o += (size_t)snprintf(b + o, n - o, "%s", s);
    }
    return o;
}

static size_t gen_line(char *b, size_t n)
{
    size_t o = 0;
    unsigned npipe = 1 + rnd(4);
    for (unsigned p = 0; p < npipe && o + 400 < n; p++) {
        unsigned ncmd = 1 + (rnd(4) == 0 ? rnd(3) : 0);
        for (unsigned c = 0; c < ncmd; c++) {
            if (c) o += (size_t)snprintf(b + o, n - o, " |");
            unsigned nt = rnd(6);
            for (unsigned t = 0; t < nt; t++) {
                o += (size_t)snprintf(b + o, n - o, " ");
                unsigned k = rnd(10);
                if (k == 0) {
                    o += (size_t)snprintf(b + o, n - o, "%s ", FREDIR[rnd(sizeof FREDIR / sizeof *FREDIR)]);
                    o += gen_word(b + o, n - o);
                } else if (k == 1 && t == 0) {
                    o += (size_t)snprintf(b + o, n - o, "%c=", 'A' + (int)rnd(3));
                    o += gen_word(b + o, n - o);
                } else if (k == 2) {
                    o += (size_t)snprintf(b + o, n - o, "%s", FJUNK[rnd(sizeof FJUNK / sizeof *FJUNK)]);
                } else if (k == 3 && t == 0) {
                    static const char *const cmds[] = {"cd", "pwd", "printf", "export", "unset", "exit", "echo", "set", "eval", "wait"};
                    o += (size_t)snprintf(b + o, n - o, "%s", cmds[rnd(sizeof cmds / sizeof *cmds)]);
                } else {
                    o += gen_word(b + o, n - o);
                }
            }
        }
        if (p + 1 < npipe) o += (size_t)snprintf(b + o, n - o, "%s", FOP[rnd(sizeof FOP / sizeof *FOP)]);
    }
    o += (size_t)snprintf(b + o, n - o, "\n");
    return o;
}

static void fuzz_env(OshXrEnv *env)
{
    memset(env, 0, sizeof *env);
    static uint8_t big[1100];
    const char *txt = "X=v\nS=a b  c\nE=\nL= lead\nT=trail \nG=x*y\nQ=q?\nW=w\\tx\\ny\nB=b\nM=mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm\nZ=z\nV=-\nN=n\\0z\n";
    env_parse(env, txt, strlen(txt), 1);
    memset(big, 'h', 1030);
    env_set_var(env, "H", big, 1030);
}

static void run_fuzz(uint64_t seed, unsigned long count)
{
    g_rng_state = seed;
    uint64_t rng = seed ^ 0xABCDEF;
    OshXrEnv *env = malloc(sizeof *env);
    char *line = malloc(4096);
    unsigned long tick = 0;
    for (unsigned long it = 0; it < count; it++) {
        fuzz_env(env);
        env->npos = 0;
        unsigned np = rnd(5);
        static const char *const pv[] = {"p1", "", "a b", " x ", "*", "q", "-", "p?"};
        for (unsigned k = 1; k <= np; k++) {
            const char *s = pv[rnd(sizeof pv / sizeof *pv)];
            env_set_pos(env, (int)k, (const uint8_t *)s, strlen(s));
        }
        if (rnd(4) != 0) env_set_pos(env, 0, (const uint8_t *)"osh", 3);
        env->status = rnd(4);
        size_t n = gen_line(line, 4000);
        int eoi = rnd(8) == 0;
        int noskip = rnd(3) == 0;
        uint64_t sseed = rng_next(&rng);
        HRes *keep = NULL;
        run_case("fuzz", (const uint8_t *)line, n, env, eoi, noskip, sseed, &rng, it, keep);
        n_fuzz++;
        if (++tick == 20000) { tick = 0; fprintf(stderr, "  fuzz %lu / %lu\n", it + 1, count); }
    }
    free(line);
    free(env);
}

static void unit_stats(void)
{
    static const char *const nm[] = {"lex", "parse", "expand"};
    for (int u = 0; u < OSH_U_COUNT; u++) {
        const OscUnit *U = CORE.u[u].U;
        unsigned maxv = 0, maxi = 0, maxp = 0;
        const char *vn = "", *in = "";
        for (int f = 0; f < U->nfuncs; f++) {
            const OscFunc *fn = &U->funcs[f];
            if (u == OSH_U_EXPAND) printf("expand fn %-12s params=%u vregs=%u/%u insns=%u/%u\n", fn->name, fn->nparams, fn->nvregs, OSC_MAX_VREGS, fn->ninsns, OSC_MAX_INSNS);
            if (fn->nvregs > maxv) { maxv = fn->nvregs; vn = fn->name; }
            if (fn->ninsns > maxi) { maxi = fn->ninsns; in = fn->name; }
            if (fn->nparams > maxp) maxp = fn->nparams;
            check(fn->nparams <= OSC_MAX_PARAMS, "%s has %u param registers", fn->name, fn->nparams);
        }
        printf("%s unit: %d functions (max %u), max params %u (max %u); biggest vregs %s %u (headroom %u of %u); biggest insns %s %u (headroom %u of %u)\n", nm[u],
               U->nfuncs, OSC_MAX_FUNCS, maxp, OSC_MAX_PARAMS, vn, maxv, OSC_MAX_VREGS - maxv, OSC_MAX_VREGS, in, maxi, OSC_MAX_INSNS - maxi, OSC_MAX_INSNS);
        check(U->nfuncs <= (int)OSC_MAX_FUNCS, "too many functions in %s", nm[u]);
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s LEX.osc PARSE.osc EXPAND.osc VECTORS_DIR [FUZZ_COUNT]\n", argv[0]);
        return 2;
    }
    unsigned long fuzz = argc > 5 ? strtoul(argv[5], NULL, 10) : 100000;
    char *src[3];
    size_t len[3];
    for (int i = 0; i < 3; i++) {
        src[i] = slurp(argv[1 + i], &len[i]);
        if (!src[i]) { fprintf(stderr, "cannot read %s\n", argv[1 + i]); return 2; }
    }
    char err[300];
    if (osh_core_init(&CORE, (const char *const *)src, len, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        printf("OSH_EXPAND_FAIL\n");
        return 1;
    }
    unit_stats();
    uint64_t rng = 0xD1CEC0FFEEULL;
    if (run_vectors(argv[4]) != 0) return 2;
    printf("vectors: %lu comparisons\n", n_vec);
    run_hostile(&rng);
    run_caps(&rng);
    run_abi(&rng);
    unsigned long hostile = n_hostile;
    run_corrupt(&rng, 20000);
    run_fuzz(0x5EED5EEDULL, fuzz);
    printf("cases %lu (hostile and cap cases %lu, fuzz %lu), interp-vs-native workspace comparisons %lu, reference comparisons %lu, chunked reruns %lu\n", n_cases,
           hostile, n_fuzz, n_eng_cmp, n_ref_cmp, n_chunk);
    printf("pipelines emitted %lu (each decoded by the host decoder: %lu), variable requests answered %lu, repeated-call probes %lu\n", n_pipes, n_decode, n_req, n_idem);
    printf("outcomes: %lu lists complete, %lu refused; corrupted-workspace runs %lu (%lu trapped, engines agreed on all)\n", n_final_ok, n_final_err, n_corrupt, n_corrupt_trap);
    printf("refusal codes seen:");
    for (int c = 0; c < 256; c++)
        if (n_code[c] && c >= 200) printf(" %d=%lu", c, n_code[c]);
    printf("\ncalls: interp %lu native %lu faults %lu\n", CORE.calls_interp, CORE.calls_native, CORE.faults);
    printf("checks %lu, failures %lu\n", n_checks, n_fail);
    int bad = n_fail != 0;
    for (int c = 220; c <= 248; c++) (void)c;
    osh_core_free(&CORE);
    for (int i = 0; i < 3; i++) free(src[i]);
    printf(bad ? "OSH_EXPAND_FAIL\n" : "OSH_EXPAND_PASS\n");
    return bad;
}
