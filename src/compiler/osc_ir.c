/*
 * osc_ir.c -- OSC-1 typed IR: structural validation, canonical encoding,
 * SHA-256 identity. See osc_ir.h and docs/osc/OSC-1-DESIGN.md section 4.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_ir.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- scalar helpers ---------------------------------------------------- */
unsigned osc_scalar_width(OscScalar s) {
    switch (s) {
    case OSC_T_BOOL: return 1;
    case OSC_T_U8: case OSC_T_I8: return 8;
    case OSC_T_U16: case OSC_T_I16: return 16;
    case OSC_T_U32: case OSC_T_I32: return 32;
    case OSC_T_U64: case OSC_T_I64: return 64;
    default: return 0;
    }
}
bool osc_scalar_signed(OscScalar s) { return s >= OSC_T_I8 && s <= OSC_T_I64; }
bool osc_scalar_is_int(OscScalar s) { return s >= OSC_T_U8 && s <= OSC_T_I64; }
const char *osc_scalar_name(OscScalar s) {
    static const char *n[] = {"void", "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "ref"};
    return ((unsigned)s <= OSC_T_REF) ? n[s] : "?";
}

/* ---- validation ------------------------------------------------------- */
static int vfail(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static bool is_value_scalar(OscScalar s) { return s >= OSC_T_BOOL && s <= OSC_T_I64; }

static bool type_is_scalar(const OscType *t) {
    return is_value_scalar(t->s) && t->ref == OSC_REF_NONE && t->elem == OSC_T_VOID && t->len == 0;
}
static bool type_is_ref(const OscType *t) {
    return t->s == OSC_T_REF && t->ref >= OSC_REF_OWN && t->ref <= OSC_REF_MUT &&
           is_value_scalar(t->elem) && t->len >= 1 && t->len <= OSC_MAX_ARRAY_LEN;
}
static bool type_eq(const OscType *a, const OscType *b) {
    return a->s == b->s && a->ref == b->ref && a->elem == b->elem && a->len == b->len;
}
/* may a value of REF type `src` be bound to a REF slot of type `dst`? */
static bool ref_bindable(const OscType *dst, const OscType *src) {
    if (!type_is_ref(dst) || !type_is_ref(src)) return false;
    if (dst->elem != src->elem || dst->len != src->len) return false;
    switch (dst->ref) {
    case OSC_REF_OWN: return src->ref == OSC_REF_OWN;               /* move */
    case OSC_REF_MUT: return src->ref == OSC_REF_OWN || src->ref == OSC_REF_MUT;
    case OSC_REF_SHARED: return true;
    default: return false;
    }
}
static bool canonical_for(OscScalar s, uint64_t v) {
    unsigned w = osc_scalar_width(s);
    if (s == OSC_T_BOOL) return v <= 1;
    if (w == 64) return true;
    if (osc_scalar_signed(s)) {
        int64_t x = (int64_t)v, lo = -(1LL << (w - 1)), hi = (1LL << (w - 1)) - 1;
        return x >= lo && x <= hi;
    }
    return v < (1ULL << w);
}

static bool is_term(uint8_t op) { return op == OSC_I_BR || op == OSC_I_CBR || op == OSC_I_RET; }

/* uses / def of an instruction (for def-before-use) */
static int insn_uses(const OscInsn *in, int16_t *u) {
    int k = 0;
    switch (in->op) {
    case OSC_I_MOV: case OSC_I_UN: case OSC_I_CAST: case OSC_I_ALLOC: case OSC_I_RELEASE:
    case OSC_I_CBR: u[k++] = in->a; break;
    case OSC_I_BIN: case OSC_I_CMP: case OSC_I_LOAD: u[k++] = in->a; u[k++] = in->b; break;
    case OSC_I_STORE: u[k++] = in->a; u[k++] = in->b; u[k++] = in->c; break;
    case OSC_I_CALL: for (int i = 0; i < in->nargs && i < OSC_MAX_PARAMS; i++) u[k++] = in->args[i]; break;
    case OSC_I_RET: if (in->a >= 0) u[k++] = in->a; break;
    default: break;
    }
    return k;
}
static int insn_def(const OscInsn *in) {
    switch (in->op) {
    case OSC_I_CONST: case OSC_I_MOV: case OSC_I_BIN: case OSC_I_UN: case OSC_I_CMP:
    case OSC_I_CAST: case OSC_I_ALLOC: case OSC_I_LOAD: return in->dst;
    case OSC_I_CALL: return in->dst;  /* may be -1 */
    default: return -1;
    }
}

#define BSW ((OSC_MAX_VREGS + 63) / 64)

static int validate_insn(const OscUnit *u, int fi, const OscFunc *f, uint32_t ii, char *err, size_t n) {
    const OscInsn *in = &f->insns[ii];
    const OscType *T = f->vtype;
    int nv = f->nvregs;
#define VR(x) ((x) >= 0 && (x) < nv)
#define NEED(cond, ...) do { if (!(cond)) return vfail(err, n, "func %d insn %u: " __VA_ARGS__); } while (0)
    switch (in->op) {
    case OSC_I_CONST:
        NEED(VR(in->dst), "CONST bad dst vreg %d", fi, ii, in->dst);
        NEED(type_is_scalar(&T[in->dst]), "CONST dst not scalar", fi, ii);
        NEED(canonical_for(T[in->dst].s, in->imm), "CONST imm not canonical for %s", fi, ii, osc_scalar_name(T[in->dst].s));
        break;
    case OSC_I_MOV:
        NEED(VR(in->dst) && VR(in->a), "MOV bad vreg", fi, ii);
        if (type_is_ref(&T[in->dst]))
            NEED(ref_bindable(&T[in->dst], &T[in->a]), "MOV ref kind/type mismatch", fi, ii);
        else
            NEED(type_is_scalar(&T[in->dst]) && type_eq(&T[in->dst], &T[in->a]), "MOV type mismatch", fi, ii);
        break;
    case OSC_I_BIN: {
        NEED(VR(in->dst) && VR(in->a) && VR(in->b), "BIN bad vreg", fi, ii);
        NEED(in->sub >= OSC_B_ADD && in->sub <= OSC_B_SHR, "BIN bad sub-op %u", fi, ii, in->sub);
        const OscType *d = &T[in->dst], *a = &T[in->a], *b = &T[in->b];
        NEED(type_is_scalar(d) && type_eq(d, a), "BIN dst/a type mismatch", fi, ii);
        if (in->sub == OSC_B_SHL || in->sub == OSC_B_SHR) {
            NEED(osc_scalar_is_int(a->s), "shift of non-integer", fi, ii);
            NEED(type_is_scalar(b) && osc_scalar_is_int(b->s), "shift amount not integer", fi, ii);
        } else if (in->sub == OSC_B_AND || in->sub == OSC_B_OR || in->sub == OSC_B_XOR) {
            NEED(type_eq(a, b), "BIN operand type mismatch", fi, ii);
        } else {
            NEED(osc_scalar_is_int(a->s) && type_eq(a, b), "arith BIN needs identical integer types", fi, ii);
        }
        break;
    }
    case OSC_I_UN: {
        NEED(VR(in->dst) && VR(in->a), "UN bad vreg", fi, ii);
        const OscType *d = &T[in->dst], *a = &T[in->a];
        NEED(type_is_scalar(d) && type_eq(d, a), "UN type mismatch", fi, ii);
        if (in->sub == OSC_U_LNOT) NEED(a->s == OSC_T_BOOL, "LNOT needs bool", fi, ii);
        else if (in->sub == OSC_U_NEG || in->sub == OSC_U_BNOT) NEED(osc_scalar_is_int(a->s), "NEG/BNOT need integer", fi, ii);
        else return vfail(err, n, "func %d insn %u: UN bad sub-op %u", fi, ii, in->sub);
        break;
    }
    case OSC_I_CMP:
        NEED(VR(in->dst) && VR(in->a) && VR(in->b), "CMP bad vreg", fi, ii);
        NEED(in->sub >= OSC_C_EQ && in->sub <= OSC_C_GE, "CMP bad cc %u", fi, ii, in->sub);
        NEED(type_is_scalar(&T[in->dst]) && T[in->dst].s == OSC_T_BOOL, "CMP dst not bool", fi, ii);
        NEED(type_is_scalar(&T[in->a]) && type_eq(&T[in->a], &T[in->b]), "CMP operand type mismatch", fi, ii);
        break;
    case OSC_I_CAST:
        NEED(VR(in->dst) && VR(in->a), "CAST bad vreg", fi, ii);
        NEED(type_is_scalar(&T[in->dst]) && osc_scalar_is_int(T[in->dst].s) &&
             type_is_scalar(&T[in->a]) && osc_scalar_is_int(T[in->a].s), "CAST needs integer types", fi, ii);
        break;
    case OSC_I_ALLOC:
        NEED(VR(in->dst) && VR(in->a), "ALLOC bad vreg", fi, ii);
        NEED(type_is_ref(&T[in->dst]) && T[in->dst].ref == OSC_REF_OWN, "ALLOC dst not an owner", fi, ii);
        NEED(type_is_scalar(&T[in->a]) && T[in->a].s == T[in->dst].elem, "ALLOC init type != elem type", fi, ii);
        break;
    case OSC_I_RELEASE:
        NEED(VR(in->a), "RELEASE bad vreg", fi, ii);
        NEED(type_is_ref(&T[in->a]) && T[in->a].ref == OSC_REF_OWN, "RELEASE of a non-owner", fi, ii);
        break;
    case OSC_I_LOAD:
        NEED(VR(in->dst) && VR(in->a) && VR(in->b), "LOAD bad vreg", fi, ii);
        NEED(type_is_ref(&T[in->a]), "LOAD base not a ref", fi, ii);
        NEED(type_is_scalar(&T[in->b]) && osc_scalar_is_int(T[in->b].s), "LOAD index not integer", fi, ii);
        NEED(type_is_scalar(&T[in->dst]) && T[in->dst].s == T[in->a].elem, "LOAD dst type != elem", fi, ii);
        break;
    case OSC_I_STORE:
        NEED(VR(in->a) && VR(in->b) && VR(in->c), "STORE bad vreg", fi, ii);
        NEED(type_is_ref(&T[in->a]) && (T[in->a].ref == OSC_REF_OWN || T[in->a].ref == OSC_REF_MUT),
             "STORE through a shared borrow or non-ref", fi, ii);
        NEED(type_is_scalar(&T[in->b]) && osc_scalar_is_int(T[in->b].s), "STORE index not integer", fi, ii);
        NEED(type_is_scalar(&T[in->c]) && T[in->c].s == T[in->a].elem, "STORE value type != elem", fi, ii);
        break;
    case OSC_I_CALL: {
        NEED(in->callee >= 0 && in->callee < fi, "CALL to function %d not defined earlier", fi, ii, in->callee);
        const OscFunc *g = &u->funcs[in->callee];
        NEED(in->nargs == g->nparams, "CALL arity %u != %u", fi, ii, in->nargs, g->nparams);
        for (int k = 0; k < in->nargs; k++) {
            int a = in->args[k];
            NEED(VR(a), "CALL bad arg vreg", fi, ii);
            const OscType *p = &g->vtype[k];
            if (type_is_ref(p)) NEED(ref_bindable(p, &T[a]), "CALL arg %d ref mismatch", fi, ii, k);
            else NEED(type_eq(p, &T[a]), "CALL arg %d type mismatch", fi, ii, k);
        }
        if (g->ret.s == OSC_T_VOID) NEED(in->dst == -1, "CALL of void function with dst", fi, ii);
        else NEED(VR(in->dst) && type_eq(&T[in->dst], &g->ret), "CALL dst type != callee return", fi, ii);
        break;
    }
    case OSC_I_TRAP:
        NEED(in->imm >= OSC_TRAP_OVERFLOW && in->imm <= OSC_TRAP_MAX, "TRAP bad code", fi, ii);
        break;
    case OSC_I_BR:
        NEED(in->blk_t >= 0 && in->blk_t < f->nblocks, "BR bad target", fi, ii);
        break;
    case OSC_I_CBR:
        NEED(VR(in->a) && type_is_scalar(&T[in->a]) && T[in->a].s == OSC_T_BOOL, "CBR condition not bool", fi, ii);
        NEED(in->blk_t >= 0 && in->blk_t < f->nblocks && in->blk_f >= 0 && in->blk_f < f->nblocks, "CBR bad target", fi, ii);
        break;
    case OSC_I_RET:
        if (f->ret.s == OSC_T_VOID) NEED(in->a == -1, "RET value from void function", fi, ii);
        else NEED(VR(in->a) && type_eq(&T[in->a], &f->ret), "RET type mismatch", fi, ii);
        break;
    default:
        return vfail(err, n, "func %d insn %u: unknown op %u", fi, ii, in->op);
    }
#undef NEED
#undef VR
    return 0;
}

static bool str_ok(const char *s, size_t cap) { return memchr(s, 0, cap) != NULL; }

static int validate_func(const OscUnit *u, int fi, char *err, size_t n) {
    const OscFunc *f = &u->funcs[fi];
    if (!str_ok(f->name, sizeof f->name)) return vfail(err, n, "func %d: name not terminated", fi);
    if (!str_ok(f->requires_text, sizeof f->requires_text) || !str_ok(f->ensures_text, sizeof f->ensures_text))
        return vfail(err, n, "func %d: contract text not terminated", fi);
    if (f->nparams > OSC_MAX_PARAMS) return vfail(err, n, "func %d: %u params > %d", fi, f->nparams, OSC_MAX_PARAMS);
    if (f->nvregs > OSC_MAX_VREGS || f->nparams > f->nvregs) return vfail(err, n, "func %d: bad nvregs", fi);
    if (!(f->ret.s == OSC_T_VOID && f->ret.ref == OSC_REF_NONE && f->ret.elem == OSC_T_VOID && f->ret.len == 0) &&
        !type_is_scalar(&f->ret))
        return vfail(err, n, "func %d: return type must be void or scalar", fi);
    for (int v = 0; v < f->nvregs; v++)
        if (!type_is_scalar(&f->vtype[v]) && !type_is_ref(&f->vtype[v]))
            return vfail(err, n, "func %d: vreg %d has invalid type", fi, v);
    if (f->ninsns < 1 || f->ninsns > OSC_MAX_INSNS) return vfail(err, n, "func %d: bad ninsns", fi);
    if (f->nblocks < 1 || f->nblocks > OSC_MAX_BLOCKS) return vfail(err, n, "func %d: bad nblocks", fi);

    /* blocks partition the instruction array */
    uint8_t cover[OSC_MAX_INSNS];
    memset(cover, 0, sizeof cover);
    for (int b = 0; b < f->nblocks; b++) {
        const OscBlock *bl = &f->blocks[b];
        if (bl->count < 1 || (uint64_t)bl->first + bl->count > f->ninsns)
            return vfail(err, n, "func %d block %d: bad range", fi, b);
        for (uint32_t i = bl->first; i < bl->first + bl->count; i++) {
            if (cover[i]) return vfail(err, n, "func %d: insn %u in two blocks", fi, i);
            cover[i] = 1;
            bool last = i == bl->first + bl->count - 1;
            if (is_term(f->insns[i].op) != last)
                return vfail(err, n, last ? "func %d block %d: missing terminator" :
                                            "func %d block %d: terminator before block end", fi, b);
        }
    }
    for (uint32_t i = 0; i < f->ninsns; i++)
        if (!cover[i]) return vfail(err, n, "func %d: insn %u in no block", fi, i);
    for (uint32_t i = 0; i < f->ninsns; i++)
        if (validate_insn(u, fi, f, i, err, n)) return -1;

    /* definite assignment: every use reads a vreg defined on every path */
    int nb = f->nblocks;
    uint64_t *in = malloc(sizeof(uint64_t) * BSW * nb), *out = malloc(sizeof(uint64_t) * BSW * nb);
    if (!in || !out) { free(in); free(out); return vfail(err, n, "out of memory"); }
    uint64_t params[BSW] = {0};
    for (int v = 0; v < f->nparams; v++) params[v / 64] |= 1ULL << (v % 64);
    for (int b = 0; b < nb; b++) for (int k = 0; k < BSW; k++) { in[b * BSW + k] = ~0ULL; out[b * BSW + k] = ~0ULL; }
    int rc = 0;
    for (int changed = 1; changed;) {  /* monotone decreasing: terminates */
        changed = 0;
        for (int b = 0; b < nb; b++) {
            uint64_t cur[BSW];
            for (int k = 0; k < BSW; k++) cur[k] = (b == 0) ? params[k] : ~0ULL;
            /* meet over predecessors */
            for (int p = 0; p < nb; p++) {
                const OscInsn *t = &f->insns[f->blocks[p].first + f->blocks[p].count - 1];
                bool pred = (t->op == OSC_I_BR && t->blk_t == b) ||
                            (t->op == OSC_I_CBR && (t->blk_t == b || t->blk_f == b));
                if (pred) for (int k = 0; k < BSW; k++) cur[k] &= out[p * BSW + k];
            }
            if (memcmp(cur, &in[b * BSW], sizeof cur)) { memcpy(&in[b * BSW], cur, sizeof cur); changed = 1; }
            for (uint32_t i = f->blocks[b].first; i < f->blocks[b].first + f->blocks[b].count; i++) {
                int d = insn_def(&f->insns[i]);
                if (d >= 0) cur[d / 64] |= 1ULL << (d % 64);
            }
            if (memcmp(cur, &out[b * BSW], sizeof cur)) { memcpy(&out[b * BSW], cur, sizeof cur); changed = 1; }
        }
    }
    for (int b = 0; b < nb && !rc; b++) {
        uint64_t cur[BSW];
        memcpy(cur, &in[b * BSW], sizeof cur);
        for (uint32_t i = f->blocks[b].first; i < f->blocks[b].first + f->blocks[b].count && !rc; i++) {
            int16_t us[OSC_MAX_PARAMS + 2];
            int k = insn_uses(&f->insns[i], us);
            for (int j = 0; j < k; j++)
                if (!(cur[us[j] / 64] >> (us[j] % 64) & 1)) {
                    rc = vfail(err, n, "func %d insn %u: vreg %d may be used before it is defined", fi, i, us[j]);
                    break;
                }
            int d = insn_def(&f->insns[i]);
            if (d >= 0) cur[d / 64] |= 1ULL << (d % 64);
        }
    }
    free(in);
    free(out);
    return rc;
}

int osc_ir_validate(const OscUnit *u, char *err, size_t n) {
    if (!u) return vfail(err, n, "null unit");
    if (u->nfuncs > OSC_MAX_FUNCS) return vfail(err, n, "too many functions");
    for (int fi = 0; fi < u->nfuncs; fi++)
        if (validate_func(u, fi, err, n)) return -1;
    if (err && n) err[0] = 0;
    return 0;
}

/* ---- canonical encoding ------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    size_t cap, len;
    sha256_ctx *sha;
} W;

static void wbytes(W *w, const void *p, size_t k) {
    if (w->sha) sha256_update(w->sha, (const uint8_t *)p, k);
    if (w->buf && w->len + k <= w->cap) memcpy(w->buf + w->len, p, k);
    w->len += k;
}
static void w8(W *w, uint64_t v) { uint8_t b = (uint8_t)v; wbytes(w, &b, 1); }
static void w16(W *w, uint64_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; wbytes(w, b, 2); }
static void w32(W *w, uint64_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    wbytes(w, b, 4);
}
static void w64(W *w, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    wbytes(w, b, 8);
}
static void wvr(W *w, int16_t v) { w16(w, (uint16_t)v); }  /* two's complement LE; -1 = 0xFFFF */
static void wtype(W *w, const OscType *t) {
    w8(w, (uint8_t)t->s);
    if (t->s == OSC_T_REF) { w8(w, (uint8_t)t->ref); w8(w, (uint8_t)t->elem); w16(w, t->len); }
}
static void wtext(W *w, const char *s, size_t cap) {
    size_t k = strnlen(s, cap);
    w16(w, k);
    wbytes(w, s, k);
}

static void encode_unit(const OscUnit *u, W *w) {
    static const uint8_t magic[8] = {'O', 'S', 'C', '1', 'I', 'R', 0, 1}; /* format version 1 */
    wbytes(w, magic, sizeof magic);
    w16(w, u->nfuncs);
    for (int fi = 0; fi < u->nfuncs; fi++) {
        const OscFunc *f = &u->funcs[fi];
        size_t nl = strnlen(f->name, sizeof f->name);
        w8(w, nl);
        wbytes(w, f->name, nl);
        w8(w, f->nparams);
        wtype(w, &f->ret);
        w16(w, f->nvregs);
        for (int v = 0; v < f->nvregs; v++) wtype(w, &f->vtype[v]);
        w16(w, f->nblocks);
        for (int b = 0; b < f->nblocks; b++) { w32(w, f->blocks[b].first); w32(w, f->blocks[b].count); }
        w32(w, f->ninsns);
        for (uint32_t i = 0; i < f->ninsns; i++) {
            const OscInsn *in = &f->insns[i];
            w8(w, in->op);
            switch (in->op) {
            case OSC_I_CONST: wvr(w, in->dst); w64(w, in->imm); break;
            case OSC_I_MOV: case OSC_I_CAST: case OSC_I_ALLOC: wvr(w, in->dst); wvr(w, in->a); break;
            case OSC_I_BIN: case OSC_I_CMP: w8(w, in->sub); wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); break;
            case OSC_I_UN: w8(w, in->sub); wvr(w, in->dst); wvr(w, in->a); break;
            case OSC_I_RELEASE: wvr(w, in->a); break;
            case OSC_I_LOAD: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); break;
            case OSC_I_STORE: wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); break;
            case OSC_I_CALL:
                wvr(w, in->dst); w16(w, (uint16_t)in->callee); w8(w, in->nargs);
                for (int k = 0; k < in->nargs; k++) wvr(w, in->args[k]);
                break;
            case OSC_I_TRAP: w8(w, in->imm); break;
            case OSC_I_BR: w16(w, (uint16_t)in->blk_t); break;
            case OSC_I_CBR: wvr(w, in->a); w16(w, (uint16_t)in->blk_t); w16(w, (uint16_t)in->blk_f); break;
            case OSC_I_RET: wvr(w, in->a); break;
            default: break; /* unreachable after validation */
            }
            /* `line` is debug information and deliberately not encoded: source
             * spelling and layout never reach the identity (DESIGN s.4). */
        }
        wtext(w, f->requires_text, sizeof f->requires_text);
        wtext(w, f->ensures_text, sizeof f->ensures_text);
    }
}

int osc_ir_encode(const OscUnit *u, uint8_t *buf, size_t cap, size_t *len) {
    if (osc_ir_validate(u, NULL, 0)) return -1;
    W w = {buf, buf ? cap : 0, 0, NULL};
    encode_unit(u, &w);
    if (len) *len = w.len;
    if (buf && w.len > cap) return -1;
    return 0;
}

int osc_ir_digest(const OscUnit *u, uint8_t out[32]) {
    if (osc_ir_validate(u, NULL, 0)) return -1;
    sha256_ctx c;
    sha256_init(&c);
    W w = {NULL, 0, 0, &c};
    encode_unit(u, &w);
    sha256_final(&c, out);
    return 0;
}
