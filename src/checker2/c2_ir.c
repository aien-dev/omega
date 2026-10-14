/* c2_ir.c: checker2 decoder, re-encoder and acceptance rules for canonical OSC IR bytes.
 * See c2_ir.h for the independence statement and spec/one-layer-receipts.md section 9 for the
 * rules. Every rejection names its rule; lines tagged GUARD, colon, name are removed one at a time by
 * tests/checker2/mutants.sh and each removal must make a test fail. */
#include "c2_ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(C2Verdict *v, const char *rule, int func, int insn, const char *fmt, ...)
{
    if (v) {
        v->ok = 0;
        v->rule = rule;
        v->func = func;
        v->insn = insn;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v->detail, sizeof v->detail, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* ------------------------------------------------------------------ reader */

typedef struct {
    const uint8_t *p;
    size_t n, i;
} Rd;

static int need(Rd *r, size_t k) { return r->n - r->i >= k; }
static int rd8(Rd *r, uint8_t *o)
{
    if (!need(r, 1)) return -1;
    *o = r->p[r->i++];
    return 0;
}
static int rd16(Rd *r, uint16_t *o)
{
    if (!need(r, 2)) return -1;
    *o = (uint16_t)(r->p[r->i] | (r->p[r->i + 1] << 8));
    r->i += 2;
    return 0;
}
static int rd32(Rd *r, uint32_t *o)
{
    if (!need(r, 4)) return -1;
    uint32_t x = 0;
    for (int k = 3; k >= 0; k--) x = (x << 8) | r->p[r->i + k];
    r->i += 4;
    *o = x;
    return 0;
}
static int rd64(Rd *r, uint64_t *o)
{
    if (!need(r, 8)) return -1;
    uint64_t x = 0;
    for (int k = 7; k >= 0; k--) x = (x << 8) | r->p[r->i + k];
    r->i += 8;
    *o = x;
    return 0;
}
static int rdvr(Rd *r, int16_t *o)
{
    uint16_t x;
    if (rd16(r, &x)) return -1;
    *o = (int16_t)x; /* two's complement; 0xFFFF = -1 = "none" */
    return 0;
}

static int ident_ok(const char *s, size_t n)
{
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        int digit = c >= '0' && c <= '9';
        if (!(alpha || (i > 0 && digit))) return 0;
    }
    return 1;
}

/* name with a one-byte length (functions, structs, fields) */
static int rdname(Rd *r, char *out)
{
    uint8_t n;
    if (rd8(r, &n)) return -1;
    if (n > C2_NAME_MAX || !need(r, n)) return -2;
    memcpy(out, r->p + r->i, n);
    out[n] = 0;
    r->i += n;
    return ident_ok(out, n) ? 0 : -2;
}

static int rdtype(Rd *r, C2Type *t)
{
    memset(t, 0, sizeof *t);
    if (rd8(r, &t->s)) return -1;
    if (t->s == S_REF) {
        if (rd8(r, &t->kind) || rd8(r, &t->elem) || rd16(r, &t->len)) return -1;
        /* a struct reference has elem VOID and carries its struct index + 1 */
        if (t->elem == S_VOID && rd8(r, &t->sid)) return -1;
    }
    return 0;
}

#define TRUNC(rule) return fail(v, "C2-DECODE-TRUNCATED", fi, -1, "%s", rule)

static int dec_insn(Rd *r, C2Insn *in, C2Verdict *v, int fi, int ii)
{
    memset(in, 0, sizeof *in);
    in->dst = in->a = in->b = in->c = -1;
    for (int k = 0; k < C2_MAX_PARAMS; k++) in->args[k] = -1;
    if (rd8(r, &in->op)) TRUNC("opcode");
    int bad = 0;
    switch (in->op) {
    case C2_CONST: bad = rdvr(r, &in->dst) || rd64(r, &in->imm); break;
    case C2_MOV: case C2_CAST: case C2_ALLOC: bad = rdvr(r, &in->dst) || rdvr(r, &in->a); break;
    case C2_BIN: case C2_CMP:
        bad = rd8(r, &in->sub) || rdvr(r, &in->dst) || rdvr(r, &in->a) || rdvr(r, &in->b);
        break;
    case C2_UN: bad = rd8(r, &in->sub) || rdvr(r, &in->dst) || rdvr(r, &in->a); break;
    case C2_RELEASE: case C2_ADESTROY: case C2_PCLOSE: case C2_RET: bad = rdvr(r, &in->a); break;
    case C2_AOPEN: {
        uint8_t k = 0;
        bad = rdvr(r, &in->dst) || rd8(r, &k);
        in->imm = k;
        break;
    }
    case C2_AALLOC: case C2_HALLOC: case C2_HGEN: case C2_LOAD: case C2_SLOAD:
        bad = rdvr(r, &in->dst) || rdvr(r, &in->a) || rdvr(r, &in->b);
        break;
    case C2_POPEN: bad = rdvr(r, &in->dst) || rd8(r, &in->sub) || rd8(r, &in->nargs) || rd64(r, &in->imm); break;
    case C2_HFREE: case C2_STORE: case C2_SSTORE: bad = rdvr(r, &in->a) || rdvr(r, &in->b) || rdvr(r, &in->c); break;
    case C2_HLOAD: bad = rdvr(r, &in->dst) || rdvr(r, &in->a) || rdvr(r, &in->b) || rdvr(r, &in->c); break;
    case C2_HSTORE:
        bad = rdvr(r, &in->a) || rdvr(r, &in->b) || rdvr(r, &in->c) || rdvr(r, &in->args[0]);
        in->nargs = 1;
        break;
    case C2_FLOAD: case C2_FSTORE: {
        uint8_t k = 0;
        if (in->op == C2_FLOAD) bad = rdvr(r, &in->dst) || rdvr(r, &in->a) || rdvr(r, &in->b);
        else bad = rdvr(r, &in->a) || rdvr(r, &in->b) || rdvr(r, &in->c);
        bad = bad || rd8(r, &k);
        in->imm = k;
        break;
    }
    case C2_CALL:
        bad = rdvr(r, &in->dst) || rd16(r, &in->callee) || rd8(r, &in->nargs);
        if (!bad && in->nargs > C2_MAX_PARAMS)
            return fail(v, "C2-CALL-ARITY", fi, ii, "call with %u arguments", in->nargs); /* GUARD:call-nargs-limit */
        for (int k = 0; !bad && k < in->nargs; k++) bad = rdvr(r, &in->args[k]);
        break;
    case C2_TRAP: {
        uint8_t k = 0;
        bad = rd8(r, &k);
        in->imm = k;
        break;
    }
    case C2_BR: bad = rd16(r, &in->t); break;
    case C2_CBR: bad = rdvr(r, &in->a) || rd16(r, &in->t) || rd16(r, &in->f); break;
    default: return fail(v, "C2-DECODE-OPCODE", fi, ii, "unknown opcode %u", in->op);
    }
    if (bad) TRUNC("instruction operands");
    return 0;
}

static int has_op(const C2Unit *u, uint8_t lo, uint8_t hi)
{
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (uint32_t i = 0; i < u->fn[fi].ninsns; i++)
            if (u->fn[fi].in[i].op >= lo && u->fn[fi].in[i].op <= hi) return 1;
    return 0;
}

static int has_slice(const C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (int k = 0; k < u->fn[fi].nvregs; k++)
            if (u->fn[fi].vt[k].s == S_BYTES || u->fn[fi].vt[k].s == S_CELLS) return 1;
    return 0;
}

static int canonical_version(const C2Unit *u)
{
    if (has_slice(u)) return 5;
    if (has_op(u, C2_POPEN, C2_HSTORE)) return 4;
    if (has_op(u, C2_AOPEN, C2_ADESTROY)) return 3;
    if (u->nstructs) return 2;
    return 1;
}

void c2_unit_free(C2Unit *u)
{
    if (!u) return;
    for (int i = 0; i < C2_MAX_FUNCS; i++) {
        free(u->fn[i].vt);
        free(u->fn[i].blk);
        free(u->fn[i].in);
        u->fn[i].vt = NULL;
        u->fn[i].blk = NULL;
        u->fn[i].in = NULL;
    }
}

static int rdtext(Rd *r, char *out, uint16_t *len)
{
    if (rd16(r, len)) return -1;
    if (*len > C2_CLAUSE_MAX || !need(r, *len)) return -2;
    memcpy(out, r->p + r->i, *len);
    out[*len] = 0;
    r->i += *len;
    for (unsigned k = 0; k < *len; k++)
        if (out[k] == 0) return -2; /* the producer writes strnlen bytes: an inner NUL is not canonical */
    return 0;
}

static const uint8_t MAGIC7[7] = {'O', 'S', 'C', '1', 'I', 'R', 0};

int c2_decode(const uint8_t *buf, size_t len, C2Unit *u, C2Verdict *v)
{
    int fi = -1;
    memset(u, 0, sizeof *u);
    if (v) { memset(v, 0, sizeof *v); v->ok = 1; v->func = v->insn = -1; }
    Rd rr = {buf, len, 0}, *r = &rr;
    if (!buf || len < 8 || memcmp(buf, MAGIC7, 7) != 0) return fail(v, "C2-DECODE-MAGIC", -1, -1, "bad magic");
    u->version = buf[7];
    if (u->version < 1 || u->version > 5) /* GUARD:version-range */
        return fail(v, "C2-DECODE-VERSION", -1, -1, "unknown version %u", u->version);
    r->i = 8;
    if (u->version >= 2) {
        if (rd8(r, &u->nstructs)) TRUNC("struct count");
        if (u->nstructs > C2_MAX_STRUCTS) return fail(v, "C2-LIMIT-STRUCTS", -1, -1, "%u structs", u->nstructs);
        for (int k = 0; k < u->nstructs; k++) {
            C2Struct *s = &u->st[k];
            int e = rdname(r, s->name);
            if (e == -1) TRUNC("struct name");
            if (e) return fail(v, "C2-DECODE-NAME", -1, -1, "struct %d name", k);
            if (rd8(r, &s->nfields) || rd16(r, &s->ncells)) TRUNC("struct header");
            if (s->nfields < 1 || s->nfields > C2_MAX_FIELDS)
                return fail(v, "C2-STRUCT-FIELDS", -1, -1, "struct %d has %u fields", k, s->nfields);
            for (int f = 0; f < s->nfields; f++) {
                e = rdname(r, s->f[f].name);
                if (e == -1) TRUNC("field name");
                if (e) return fail(v, "C2-DECODE-NAME", -1, -1, "struct %d field %d name", k, f);
                if (rd8(r, &s->f[f].s) || rd16(r, &s->f[f].alen) || rd16(r, &s->f[f].off)) TRUNC("field");
            }
        }
    }
    if (rd16(r, &u->nfuncs)) TRUNC("function count");
    if (u->nfuncs < 1 || u->nfuncs > C2_MAX_FUNCS)
        return fail(v, "C2-LIMIT-FUNCS", -1, -1, "%u functions", u->nfuncs);
    for (fi = 0; fi < u->nfuncs; fi++) {
        C2Func *f = &u->fn[fi];
        int e = rdname(r, f->name);
        if (e == -1) TRUNC("function name");
        if (e) return fail(v, "C2-DECODE-NAME", fi, -1, "function name");
        if (rd8(r, &f->nparams)) TRUNC("nparams");
        if (rdtype(r, &f->ret)) TRUNC("return type");
        if (rd16(r, &f->nvregs)) TRUNC("nvregs");
        if (f->nvregs > C2_MAX_VREGS) return fail(v, "C2-LIMIT-VREGS", fi, -1, "%u vregs", f->nvregs);
        f->vt = calloc(f->nvregs ? f->nvregs : 1, sizeof *f->vt);
        if (!f->vt) return fail(v, "C2-RESOURCE", fi, -1, "out of memory");
        for (int k = 0; k < f->nvregs; k++)
            if (rdtype(r, &f->vt[k])) TRUNC("vreg type");
        if (rd16(r, &f->nblocks)) TRUNC("nblocks");
        if (f->nblocks < 1 || f->nblocks > C2_MAX_BLOCKS)
            return fail(v, "C2-LIMIT-BLOCKS", fi, -1, "%u blocks", f->nblocks);
        f->blk = calloc(f->nblocks, sizeof *f->blk);
        if (!f->blk) return fail(v, "C2-RESOURCE", fi, -1, "out of memory");
        for (int b = 0; b < f->nblocks; b++)
            if (rd32(r, &f->blk[b].first) || rd32(r, &f->blk[b].count)) TRUNC("block");
        if (rd32(r, &f->ninsns)) TRUNC("ninsns");
        if (f->ninsns < 1 || f->ninsns > C2_MAX_INSNS)
            return fail(v, "C2-LIMIT-INSNS", fi, -1, "%u instructions", f->ninsns);
        f->in = calloc(f->ninsns, sizeof *f->in);
        if (!f->in) return fail(v, "C2-RESOURCE", fi, -1, "out of memory");
        for (uint32_t i = 0; i < f->ninsns; i++)
            if (dec_insn(r, &f->in[i], v, fi, (int)i)) return -1;
        e = rdtext(r, f->req, &f->req_len);
        if (e == -1) TRUNC("requires text");
        if (e) return fail(v, "C2-DECODE-TEXT", fi, -1, "requires text");
        e = rdtext(r, f->ens, &f->ens_len);
        if (e == -1) TRUNC("ensures text");
        if (e) return fail(v, "C2-DECODE-TEXT", fi, -1, "ensures text");
    }
    fi = -1;
    if (r->i != r->n) /* GUARD:trailing */
        return fail(v, "C2-DECODE-TRAILING", -1, -1, "%zu bytes after the last function", r->n - r->i);
    int cv = canonical_version(u);
    if (cv != u->version) /* GUARD:canonical-version */
        return fail(v, "C2-DECODE-VERSION-NONCANONICAL", -1, -1, "version byte %u, content needs %d", u->version, cv);
    return 0;
}

/* ------------------------------------------------------------------ re-encoder (mirror) */

typedef struct {
    uint8_t *p;
    size_t cap, n;
} Wr;

static void wb(Wr *w, const void *s, size_t k)
{
    if (w->n + k <= w->cap) memcpy(w->p + w->n, s, k);
    w->n += k;
}
static void w8(Wr *w, unsigned x) { uint8_t b = (uint8_t)x; wb(w, &b, 1); }
static void w16(Wr *w, unsigned x) { w8(w, x & 0xff); w8(w, (x >> 8) & 0xff); }
static void w32(Wr *w, uint32_t x) { for (int k = 0; k < 4; k++) w8(w, (x >> (8 * k)) & 0xff); }
static void w64(Wr *w, uint64_t x) { for (int k = 0; k < 8; k++) w8(w, (unsigned)((x >> (8 * k)) & 0xff)); }
static void wvr(Wr *w, int16_t x) { w16(w, (uint16_t)x); }
static void wname(Wr *w, const char *s) { size_t n = strlen(s); w8(w, (unsigned)n); wb(w, s, n); }
static void wtype(Wr *w, const C2Type *t)
{
    w8(w, t->s);
    if (t->s == S_REF) {
        w8(w, t->kind); w8(w, t->elem); w16(w, t->len);
        if (t->elem == S_VOID) w8(w, t->sid);
    }
}

size_t c2_encode(const C2Unit *u, uint8_t *buf, size_t cap)
{
    Wr w = {buf, cap, 0};
    wb(&w, MAGIC7, 7);
    w8(&w, u->version);
    if (u->version >= 2) {
        w8(&w, u->nstructs);
        for (int k = 0; k < u->nstructs; k++) {
            const C2Struct *s = &u->st[k];
            wname(&w, s->name); w8(&w, s->nfields); w16(&w, s->ncells);
            for (int f = 0; f < s->nfields; f++) {
                wname(&w, s->f[f].name); w8(&w, s->f[f].s); w16(&w, s->f[f].alen); w16(&w, s->f[f].off);
            }
        }
    }
    w16(&w, u->nfuncs);
    for (int fi = 0; fi < u->nfuncs; fi++) {
        const C2Func *f = &u->fn[fi];
        wname(&w, f->name); w8(&w, f->nparams); wtype(&w, &f->ret);
        w16(&w, f->nvregs);
        for (int k = 0; k < f->nvregs; k++) wtype(&w, &f->vt[k]);
        w16(&w, f->nblocks);
        for (int b = 0; b < f->nblocks; b++) { w32(&w, f->blk[b].first); w32(&w, f->blk[b].count); }
        w32(&w, f->ninsns);
        for (uint32_t i = 0; i < f->ninsns; i++) {
            const C2Insn *in = &f->in[i];
            w8(&w, in->op);
            switch (in->op) {
            case C2_CONST: wvr(&w, in->dst); w64(&w, in->imm); break;
            case C2_MOV: case C2_CAST: case C2_ALLOC: wvr(&w, in->dst); wvr(&w, in->a); break;
            case C2_BIN: case C2_CMP: w8(&w, in->sub); wvr(&w, in->dst); wvr(&w, in->a); wvr(&w, in->b); break;
            case C2_UN: w8(&w, in->sub); wvr(&w, in->dst); wvr(&w, in->a); break;
            case C2_RELEASE: case C2_ADESTROY: case C2_PCLOSE: case C2_RET: wvr(&w, in->a); break;
            case C2_AOPEN: wvr(&w, in->dst); w8(&w, (unsigned)in->imm); break;
            case C2_AALLOC: case C2_HALLOC: case C2_HGEN: case C2_LOAD: case C2_SLOAD:
                wvr(&w, in->dst); wvr(&w, in->a); wvr(&w, in->b); break;
            case C2_POPEN: wvr(&w, in->dst); w8(&w, in->sub); w8(&w, in->nargs); w64(&w, in->imm); break;
            case C2_HFREE: case C2_STORE: case C2_SSTORE: wvr(&w, in->a); wvr(&w, in->b); wvr(&w, in->c); break;
            case C2_HLOAD: wvr(&w, in->dst); wvr(&w, in->a); wvr(&w, in->b); wvr(&w, in->c); break;
            case C2_HSTORE: wvr(&w, in->a); wvr(&w, in->b); wvr(&w, in->c); wvr(&w, in->args[0]); break;
            case C2_FLOAD: wvr(&w, in->dst); wvr(&w, in->a); wvr(&w, in->b); w8(&w, (unsigned)in->imm); break;
            case C2_FSTORE: wvr(&w, in->a); wvr(&w, in->b); wvr(&w, in->c); w8(&w, (unsigned)in->imm); break;
            case C2_CALL:
                wvr(&w, in->dst); w16(&w, in->callee); w8(&w, in->nargs);
                for (int k = 0; k < in->nargs; k++) wvr(&w, in->args[k]);
                break;
            case C2_TRAP: w8(&w, (unsigned)in->imm); break;
            case C2_BR: w16(&w, in->t); break;
            case C2_CBR: wvr(&w, in->a); w16(&w, in->t); w16(&w, in->f); break;
            default: break;
            }
        }
        w16(&w, f->req_len); wb(&w, f->req, f->req_len);
        w16(&w, f->ens_len); wb(&w, f->ens, f->ens_len);
    }
    return w.n <= cap ? w.n : 0;
}

int c2_check_bytes(const uint8_t *buf, size_t len, C2Verdict *v)
{
    C2Unit *u = malloc(sizeof *u);
    if (!u) return fail(v, "C2-RESOURCE", -1, -1, "out of memory");
    int rc = c2_decode(buf, len, u, v);
    if (rc == 0) rc = c2_check_unit(u, v);
    c2_unit_free(u);
    free(u);
    return rc;
}
