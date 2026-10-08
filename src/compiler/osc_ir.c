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
    static const char *n[] = {"void", "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "ref", "bytes", "cells"};
    return ((unsigned)s <= OSC_T_CELLS) ? n[s] : "?";
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
    return is_value_scalar(t->s) && t->ref == OSC_REF_NONE && t->elem == OSC_T_VOID && t->len == 0 && t->sid == 0;
}
/* The unit whose struct table type_is_ref consults (set by osc_ir_validate;
 * validation is single-threaded per call). */
static const OscUnit *g_unit;
static bool type_is_ref(const OscType *t) {
    if (t->s != OSC_T_REF || t->ref < OSC_REF_OWN || t->ref > OSC_REF_MUT) return false;
    if (t->sid == 0) return is_value_scalar(t->elem) && t->len >= 1 && t->len <= OSC_MAX_ARRAY_LEN;
    return g_unit && t->sid <= g_unit->nstructs && t->elem == OSC_T_VOID &&
           t->len == g_unit->structs[t->sid - 1].ncells;
}
/* pointer register of an external byte slice parameter (length = next vreg) */
static bool type_is_slice(const OscType *t) {
    return (t->s == OSC_T_BYTES || t->s == OSC_T_CELLS) && t->ref == OSC_REF_NONE && t->elem == OSC_T_VOID &&
           t->len == 0 && t->sid == 0;
}
static bool type_is_struct_ref(const OscType *t) { return type_is_ref(t) && t->sid != 0; }
static bool type_is_array_ref(const OscType *t) { return type_is_ref(t) && t->sid == 0; }
static bool type_eq(const OscType *a, const OscType *b) {
    return a->s == b->s && a->ref == b->ref && a->elem == b->elem && a->len == b->len && a->sid == b->sid;
}
/* may a value of REF type `src` be bound to a REF slot of type `dst`? */
static bool ref_bindable(const OscType *dst, const OscType *src) {
    if (!type_is_ref(dst) || !type_is_ref(src)) return false;
    if (dst->elem != src->elem || dst->len != src->len || dst->sid != src->sid) return false;
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
    case OSC_I_AALLOC: u[k++] = in->a; u[k++] = in->b; break;
    case OSC_I_ADESTROY: u[k++] = in->a; break;
    case OSC_I_PCLOSE: u[k++] = in->a; break;
    case OSC_I_HALLOC: case OSC_I_HGEN: u[k++] = in->a; u[k++] = in->b; break;
    case OSC_I_HFREE: case OSC_I_HLOAD: u[k++] = in->a; u[k++] = in->b; u[k++] = in->c; break;
    case OSC_I_HSTORE: u[k++] = in->a; u[k++] = in->b; u[k++] = in->c; u[k++] = in->args[0]; break;
    case OSC_I_BIN: case OSC_I_CMP: case OSC_I_LOAD: u[k++] = in->a; u[k++] = in->b; break;
    case OSC_I_STORE: u[k++] = in->a; u[k++] = in->b; u[k++] = in->c; break;
    case OSC_I_SLOAD: u[k++] = in->a; u[k++] = in->a + 1; u[k++] = in->b; break;
    case OSC_I_SSTORE: u[k++] = in->a; u[k++] = in->a + 1; u[k++] = in->b; u[k++] = in->c; break;
    case OSC_I_FLOAD: u[k++] = in->a; if (in->b >= 0) u[k++] = in->b; break;
    case OSC_I_FSTORE: u[k++] = in->a; if (in->b >= 0) u[k++] = in->b; u[k++] = in->c; break;
    case OSC_I_CALL: for (int i = 0; i < in->nargs && i < OSC_MAX_PARAMS; i++) u[k++] = in->args[i]; break;
    case OSC_I_RET: if (in->a >= 0) u[k++] = in->a; break;
    default: break;
    }
    return k;
}
static int insn_def(const OscInsn *in) {
    switch (in->op) {
    case OSC_I_CONST: case OSC_I_MOV: case OSC_I_BIN: case OSC_I_UN: case OSC_I_CMP:
    case OSC_I_CAST: case OSC_I_ALLOC: case OSC_I_LOAD: case OSC_I_FLOAD: case OSC_I_SLOAD: return in->dst;
    case OSC_I_AOPEN: case OSC_I_AALLOC: return in->dst;
    case OSC_I_POPEN: case OSC_I_HALLOC: case OSC_I_HGEN: case OSC_I_HLOAD: return in->dst;
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
        if (T[in->dst].sid)  /* struct: every cell starts at 0 (u64 0), fields are stored next */
            NEED(type_is_scalar(&T[in->a]) && T[in->a].s == OSC_T_U64, "ALLOC struct init not u64", fi, ii);
        else
            NEED(type_is_scalar(&T[in->a]) && T[in->a].s == T[in->dst].elem, "ALLOC init type != elem type", fi, ii);
        break;
    case OSC_I_RELEASE:
        NEED(VR(in->a), "RELEASE bad vreg", fi, ii);
        NEED(type_is_ref(&T[in->a]) && T[in->a].ref == OSC_REF_OWN, "RELEASE of a non-owner", fi, ii);
        break;
    case OSC_I_AOPEN:
        NEED(VR(in->dst), "AOPEN bad vreg", fi, ii);
        NEED(type_is_scalar(&T[in->dst]) && T[in->dst].s == OSC_T_U64, "AOPEN handle not u64", fi, ii);
        NEED(in->imm >= 1 && in->imm <= OSC_MAX_ARRAY_LEN, "AOPEN capacity out of range", fi, ii);
        break;
    case OSC_I_AALLOC:
        NEED(VR(in->dst) && VR(in->a) && VR(in->b), "AALLOC bad vreg", fi, ii);
        NEED(type_is_ref(&T[in->dst]) && T[in->dst].ref == OSC_REF_OWN, "AALLOC dst not an owner", fi, ii);
        if (T[in->dst].sid)
            NEED(type_is_scalar(&T[in->a]) && T[in->a].s == OSC_T_U64, "AALLOC struct init not u64", fi, ii);
        else
            NEED(type_is_scalar(&T[in->a]) && T[in->a].s == T[in->dst].elem, "AALLOC init type != elem type", fi, ii);
        NEED(type_is_scalar(&T[in->b]) && T[in->b].s == OSC_T_U64, "AALLOC handle not u64", fi, ii);
        break;
    case OSC_I_ADESTROY:
        NEED(VR(in->a) && type_is_scalar(&T[in->a]) && T[in->a].s == OSC_T_U64, "ADESTROY handle not u64", fi, ii);
        break;
    case OSC_I_POPEN:
        NEED(VR(in->dst) && type_is_scalar(&T[in->dst]) && T[in->dst].s == OSC_T_U64, "POPEN pool not u64", fi, ii);
        NEED(in->nargs >= 1 && in->nargs <= OSC_POOL_SLOTS, "POPEN slot count out of range", fi, ii);
        NEED(is_value_scalar((OscScalar)in->sub), "POPEN element type not scalar", fi, ii);
        break;
    case OSC_I_PCLOSE:
        NEED(VR(in->a) && type_is_scalar(&T[in->a]) && T[in->a].s == OSC_T_U64, "PCLOSE pool not u64", fi, ii);
        break;
    case OSC_I_HALLOC:
    case OSC_I_HGEN:
    case OSC_I_HFREE:
    case OSC_I_HLOAD:
    case OSC_I_HSTORE: {
        int16_t us[OSC_MAX_PARAMS + 3];
        int k = insn_uses(in, us);
        for (int j = 0; j < k; j++) NEED(VR(us[j]) && type_is_scalar(&T[us[j]]), "handle op bad operand", fi, ii);
        int d = insn_def(in);
        if (in->op == OSC_I_HALLOC || in->op == OSC_I_HGEN || in->op == OSC_I_HLOAD)
            NEED(VR(d) && type_is_scalar(&T[d]), "handle op bad dst", fi, ii);
        if (in->op == OSC_I_HALLOC || in->op == OSC_I_HGEN) NEED(T[d].s == OSC_T_U64, "handle slot/gen not u64", fi, ii);
        if (in->op == OSC_I_HSTORE) NEED(in->nargs == 1, "HSTORE needs one value", fi, ii);
        /* slot and gen are u64; the pool operand and the element types are
         * checked against the defining POPEN by the pool discipline */
        if (in->op == OSC_I_HALLOC) break;
        NEED(T[in->b].s == OSC_T_U64, "handle slot not u64", fi, ii);
        if (in->op != OSC_I_HGEN) NEED(T[in->c].s == OSC_T_U64, "handle gen not u64", fi, ii);
        break;
    }
    case OSC_I_LOAD:
        NEED(VR(in->dst) && VR(in->a) && VR(in->b), "LOAD bad vreg", fi, ii);
        NEED(type_is_array_ref(&T[in->a]), "LOAD base not an array ref", fi, ii);
        NEED(type_is_scalar(&T[in->b]) && osc_scalar_is_int(T[in->b].s), "LOAD index not integer", fi, ii);
        NEED(type_is_scalar(&T[in->dst]) && T[in->dst].s == T[in->a].elem, "LOAD dst type != elem", fi, ii);
        break;
    case OSC_I_STORE:
        NEED(VR(in->a) && VR(in->b) && VR(in->c), "STORE bad vreg", fi, ii);
        NEED(type_is_array_ref(&T[in->a]) && (T[in->a].ref == OSC_REF_OWN || T[in->a].ref == OSC_REF_MUT),
             "STORE through a shared borrow or non-ref", fi, ii);
        NEED(type_is_scalar(&T[in->b]) && osc_scalar_is_int(T[in->b].s), "STORE index not integer", fi, ii);
        NEED(type_is_scalar(&T[in->c]) && T[in->c].s == T[in->a].elem, "STORE value type != elem", fi, ii);
        break;
    case OSC_I_SLOAD:
    case OSC_I_SSTORE: {
        bool ld = in->op == OSC_I_SLOAD;
        const char *nm = ld ? "SLOAD" : "SSTORE";
        NEED(VR(in->a) && VR(in->b), "%s bad vreg", fi, ii, nm);
        NEED(type_is_slice(&T[in->a]) && VR(in->a + 1) && type_is_scalar(&T[in->a + 1]) && T[in->a + 1].s == OSC_T_U64,
             "%s base not a slice pointer", fi, ii, nm);
        NEED(type_is_scalar(&T[in->b]) && osc_scalar_is_int(T[in->b].s), "%s index not integer", fi, ii, nm);
        int v = ld ? in->dst : in->c;
        NEED(VR(v) && type_is_scalar(&T[v]) && T[v].s == OSC_T_U64, "%s value not u64", fi, ii, nm);
        if (!ld) NEED(T[in->a].s == OSC_T_CELLS, "SSTORE into a bytes slice", fi, ii);
        break;
    }
    case OSC_I_FLOAD: case OSC_I_FSTORE: {
        bool ld = in->op == OSC_I_FLOAD;
        NEED(VR(in->a) && type_is_struct_ref(&T[in->a]), "%s base not a struct ref", fi, ii, ld ? "FLOAD" : "FSTORE");
        const OscStruct *st = &u->structs[T[in->a].sid - 1];
        NEED(in->imm < st->nfields, "%s field %llu out of range", fi, ii, ld ? "FLOAD" : "FSTORE",
             (unsigned long long)in->imm);
        const OscField *fd = &st->fields[in->imm];
        if (fd->alen) NEED(VR(in->b) && type_is_scalar(&T[in->b]) && osc_scalar_is_int(T[in->b].s),
                           "field index not integer", fi, ii);
        else NEED(in->b == -1, "scalar field with an index", fi, ii);
        int v = ld ? in->dst : in->c;
        NEED(VR(v) && type_is_scalar(&T[v]) && T[v].s == fd->s, "field value type != field type", fi, ii);
        if (!ld) NEED(T[in->a].ref == OSC_REF_OWN || T[in->a].ref == OSC_REF_MUT,
                      "FSTORE through a shared borrow", fi, ii);
        break;
    }
    case OSC_I_CALL: {
        NEED(in->callee >= 0 && in->callee < fi, "CALL to function %d not defined earlier", fi, ii, in->callee);
        const OscFunc *g = &u->funcs[in->callee];
        NEED(in->nargs == g->nparams, "CALL arity %u != %u", fi, ii, in->nargs, g->nparams);
        for (int k = 0; k < in->nargs; k++) {
            int a = in->args[k];
            NEED(VR(a), "CALL bad arg vreg", fi, ii);
            const OscType *p = &g->vtype[k];
            if (type_is_slice(p)) {
                NEED(type_eq(p, &T[a]) && k + 1 < in->nargs && in->args[k + 1] == a + 1, "CALL arg %d slice mismatch", fi, ii, k);
                continue;
            }
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
    if (!(f->ret.s == OSC_T_VOID && f->ret.ref == OSC_REF_NONE && f->ret.elem == OSC_T_VOID && f->ret.len == 0 && f->ret.sid == 0) &&
        !type_is_scalar(&f->ret))
        return vfail(err, n, "func %d: return type must be void or scalar", fi);
    for (int v = 0; v < f->nvregs; v++)
        if (!type_is_scalar(&f->vtype[v]) && !type_is_ref(&f->vtype[v]) && !type_is_slice(&f->vtype[v]))
            return vfail(err, n, "func %d: vreg %d has invalid type", fi, v);
    /* a slice is a parameter pair: pointer register (BYTES/CELLS) then u64 length; no instruction defines either */
    uint8_t sl[OSC_MAX_VREGS];
    memset(sl, 0, sizeof sl);
    for (int v = 0; v < f->nvregs; v++) {
        if (!type_is_slice(&f->vtype[v])) continue;
        if (v + 1 >= f->nparams || !type_is_scalar(&f->vtype[v + 1]) || f->vtype[v + 1].s != OSC_T_U64)
            return vfail(err, n, "func %d: slice vreg %d is not a parameter pair with a u64 length", fi, v);
        sl[v] = sl[v + 1] = 1;
        v++;
    }
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
    for (uint32_t i = 0; i < f->ninsns; i++) {
        int d = insn_def(&f->insns[i]);
        if (d >= 0 && d < f->nvregs && sl[d]) return vfail(err, n, "func %d insn %u: slice register redefined", fi, i);
    }

    /* OSC-2 arenas: an arena handle vreg is defined by exactly one AOPEN (no
     * other definition, not a parameter) and is used only as the handle of
     * AALLOC / ADESTROY, so a handle (an address) never reaches a value. */
    uint8_t hk[OSC_MAX_VREGS];
    memset(hk, 0, sizeof hk);
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const OscInsn *x = &f->insns[i];
        if (x->op != OSC_I_AOPEN) continue;
        if (hk[x->dst] || x->dst < f->nparams) return vfail(err, n, "func %d insn %u: AOPEN handle vreg reused", fi, i);
        hk[x->dst] = 1;
    }
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const OscInsn *x = &f->insns[i];
        int d = insn_def(x);
        if (x->op != OSC_I_AOPEN && d >= 0 && hk[d])
            return vfail(err, n, "func %d insn %u: arena handle vreg redefined", fi, i);
        if (x->op == OSC_I_AALLOC) {
            if (!hk[x->b] || hk[x->a]) return vfail(err, n, "func %d insn %u: AALLOC handle misuse", fi, i);
        } else if (x->op == OSC_I_ADESTROY) {
            if (!hk[x->a]) return vfail(err, n, "func %d insn %u: ADESTROY of a non-handle", fi, i);
        } else {
            int16_t us[OSC_MAX_PARAMS + 3];
            int k = insn_uses(x, us);
            for (int j = 0; j < k; j++)
                if (hk[us[j]]) return vfail(err, n, "func %d insn %u: arena handle used as a value", fi, i);
        }
    }

    /* OSC-3 item 2: a pool vreg is defined by exactly one POPEN (not a
     * parameter, never redefined) and is used only as the pool operand of
     * HALLOC / HGEN / HFREE / HLOAD / HSTORE / PCLOSE. Element types of
     * HALLOC init, HLOAD dst and HSTORE value match the POPEN element type. */
    uint8_t pk[OSC_MAX_VREGS];
    memset(pk, 0, sizeof pk);
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const OscInsn *x = &f->insns[i];
        if (x->op != OSC_I_POPEN) continue;
        if (pk[x->dst] || hk[x->dst] || x->dst < f->nparams)
            return vfail(err, n, "func %d insn %u: POPEN pool vreg reused", fi, i);
        pk[x->dst] = (uint8_t)x->sub;
    }
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const OscInsn *x = &f->insns[i];
        int d = insn_def(x);
        if (x->op != OSC_I_POPEN && d >= 0 && pk[d])
            return vfail(err, n, "func %d insn %u: pool vreg redefined", fi, i);
        switch (x->op) {
        case OSC_I_PCLOSE:
            if (!pk[x->a]) return vfail(err, n, "func %d insn %u: PCLOSE of a non-pool", fi, i);
            break;
        case OSC_I_HALLOC:
            if (!pk[x->b] || pk[x->a]) return vfail(err, n, "func %d insn %u: HALLOC pool misuse", fi, i);
            if (f->vtype[x->a].s != (OscScalar)pk[x->b])
                return vfail(err, n, "func %d insn %u: HALLOC init type != pool element type", fi, i);
            break;
        case OSC_I_HGEN: case OSC_I_HFREE: case OSC_I_HLOAD: case OSC_I_HSTORE:
            if (!pk[x->a] || pk[x->b] || (x->op != OSC_I_HGEN && pk[x->c]))
                return vfail(err, n, "func %d insn %u: handle pool misuse", fi, i);
            if (x->op == OSC_I_HLOAD && f->vtype[x->dst].s != (OscScalar)pk[x->a])
                return vfail(err, n, "func %d insn %u: HLOAD type != pool element type", fi, i);
            if (x->op == OSC_I_HSTORE && (pk[x->args[0]] || f->vtype[x->args[0]].s != (OscScalar)pk[x->a]))
                return vfail(err, n, "func %d insn %u: HSTORE value type != pool element type", fi, i);
            break;
        default: {
            int16_t us[OSC_MAX_PARAMS + 3];
            int k = insn_uses(x, us);
            for (int j = 0; j < k; j++)
                if (pk[us[j]]) return vfail(err, n, "func %d insn %u: pool used as a value", fi, i);
        }
        }
    }

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

/* struct table: names terminated and unique, fields well-typed, offsets are
 * the prefix sums of the field cell counts (no padding), 1..64 cells. */
static int validate_structs(const OscUnit *u, char *err, size_t n) {
    if (u->nstructs > OSC_MAX_STRUCTS) return vfail(err, n, "too many structs");
    for (int k = 0; k < u->nstructs; k++) {
        const OscStruct *s = &u->structs[k];
        if (!str_ok(s->name, sizeof s->name) || !s->name[0]) return vfail(err, n, "struct %d: bad name", k);
        for (int j = 0; j < k; j++)
            if (!strcmp(u->structs[j].name, s->name)) return vfail(err, n, "struct %d: duplicate name", k);
        if (s->nfields < 1 || s->nfields > OSC_MAX_FIELDS) return vfail(err, n, "struct %d: bad field count", k);
        unsigned off = 0;
        for (int f = 0; f < s->nfields; f++) {
            const OscField *fd = &s->fields[f];
            if (!str_ok(fd->name, sizeof fd->name) || !fd->name[0]) return vfail(err, n, "struct %d field %d: bad name", k, f);
            for (int g = 0; g < f; g++)
                if (!strcmp(s->fields[g].name, fd->name)) return vfail(err, n, "struct %d field %d: duplicate", k, f);
            if (!is_value_scalar(fd->s) || fd->alen > OSC_MAX_ARRAY_LEN)
                return vfail(err, n, "struct %d field %d: bad type", k, f);
            if (fd->off != off) return vfail(err, n, "struct %d field %d: offset %u != %u", k, f, fd->off, off);
            off += fd->alen ? fd->alen : 1;
        }
        if (off != s->ncells || off < 1 || off > OSC_MAX_ARRAY_LEN) return vfail(err, n, "struct %d: bad size", k);
    }
    return 0;
}

int osc_ir_validate(const OscUnit *u, char *err, size_t n) {
    if (!u) return vfail(err, n, "null unit");
    if (u->nfuncs > OSC_MAX_FUNCS) return vfail(err, n, "too many functions");
    g_unit = u;
    if (validate_structs(u, err, n)) return -1;
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
    if (t->s == OSC_T_REF) {
        w8(w, (uint8_t)t->ref); w8(w, (uint8_t)t->elem); w16(w, t->len);
        if (t->sid) w8(w, t->sid);  /* struct ref (elem VOID); array refs encode as OSC-1 */
    }
}
static void wtext(W *w, const char *s, size_t cap) {
    size_t k = strnlen(s, cap);
    w16(w, k);
    wbytes(w, s, k);
}

/* 1 if any function uses an arena instruction (OSC-2 section 3) */
static bool unit_has_arena(const OscUnit *u) {
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (uint32_t i = 0; i < u->funcs[fi].ninsns; i++) {
            uint8_t op = u->funcs[fi].insns[i].op;
            if (op == OSC_I_AOPEN || op == OSC_I_AALLOC || op == OSC_I_ADESTROY) return true;
        }
    return false;
}

/* 1 if any function uses a pool instruction (OSC-3 item 2) */
static bool unit_has_pool(const OscUnit *u) {
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (uint32_t i = 0; i < u->funcs[fi].ninsns; i++) {
            uint8_t op = u->funcs[fi].insns[i].op;
            if (op >= OSC_I_POPEN && op <= OSC_I_HSTORE) return true;
        }
    return false;
}

/* 1 if any function uses an external byte slice (bytes / cells parameter) */
static bool unit_has_slice(const OscUnit *u) {
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (int v = 0; v < u->funcs[fi].nvregs; v++)
            if (type_is_slice(&u->funcs[fi].vtype[v])) return true;
    return false;
}

static void encode_unit(const OscUnit *u, W *w) {
    /* format version 1 (OSC-1, no structs) / 2 (OSC-2 structs: struct table
     * after the magic) / 3 (OSC-2 arenas: AOPEN/AALLOC/ADESTROY present; the
     * struct count byte and table always follow the magic, even when 0).
     * A unit without structs or arenas encodes exactly as version 1; one with
     * structs but no arenas exactly as version 2. */
    uint8_t magic[8] = {'O', 'S', 'C', '1', 'I', 'R', 0, 1};
    bool arena = unit_has_arena(u);
    if (u->nstructs) magic[7] = 2;
    if (arena) magic[7] = 3;
    bool pool = unit_has_pool(u); /* OSC-3 item 2: version 4, same layout as 3 */
    if (pool) magic[7] = 4;
    bool slice = unit_has_slice(u); /* external byte slices: version 5, same layout as 3/4 */
    if (slice) magic[7] = 5;
    wbytes(w, magic, sizeof magic);
    if (u->nstructs || arena || pool || slice) {
        w8(w, u->nstructs);
        for (int k = 0; k < u->nstructs; k++) {
            const OscStruct *s = &u->structs[k];
            size_t sl = strnlen(s->name, sizeof s->name);
            w8(w, sl); wbytes(w, s->name, sl);
            w8(w, s->nfields); w16(w, s->ncells);
            for (int f = 0; f < s->nfields; f++) {
                const OscField *fd = &s->fields[f];
                size_t fl = strnlen(fd->name, sizeof fd->name);
                w8(w, fl); wbytes(w, fd->name, fl);
                w8(w, (uint8_t)fd->s); w16(w, fd->alen); w16(w, fd->off);
            }
        }
    }
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
            case OSC_I_AOPEN: wvr(w, in->dst); w8(w, in->imm); break;
            case OSC_I_AALLOC: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); break;
            case OSC_I_ADESTROY: wvr(w, in->a); break;
            case OSC_I_POPEN: wvr(w, in->dst); w8(w, in->sub); w8(w, in->nargs); w64(w, in->imm); break;
            case OSC_I_PCLOSE: wvr(w, in->a); break;
            case OSC_I_HALLOC: case OSC_I_HGEN: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); break;
            case OSC_I_HFREE: wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); break;
            case OSC_I_HLOAD: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); break;
            case OSC_I_HSTORE: wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); wvr(w, in->args[0]); break;
            case OSC_I_LOAD: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); break;
            case OSC_I_STORE: wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); break;
            case OSC_I_SLOAD: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); break;
            case OSC_I_SSTORE: wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); break;
            case OSC_I_FLOAD: wvr(w, in->dst); wvr(w, in->a); wvr(w, in->b); w8(w, in->imm); break;
            case OSC_I_FSTORE: wvr(w, in->a); wvr(w, in->b); wvr(w, in->c); w8(w, in->imm); break;
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

int osc_ir_encoding_version(const uint8_t *buf, size_t len) {
    static const uint8_t magic[7] = {'O', 'S', 'C', '1', 'I', 'R', 0};
    if (!buf || len < 8 || memcmp(buf, magic, sizeof magic) != 0) return -1;
    return (buf[7] >= 1 && buf[7] <= OSC_IR_VERSION_MAX) ? buf[7] : -1;
}

int osc_ir_slice_args_ok(const OscFunc *f, const uint64_t *args, unsigned nargs) {
    if (!f || nargs != f->nparams || (nargs && !args)) return -1;
    for (unsigned p = 0; p + 1 < nargs; p++) {
        if (!type_is_slice(&f->vtype[p])) continue;
        uint64_t ptr = args[p], n = args[p + 1], sz = n * (f->vtype[p].s == OSC_T_CELLS ? 8 : 1);
        if ((f->vtype[p].s == OSC_T_CELLS && n > UINT64_MAX / 8) || (n && !ptr) || sz > UINT64_MAX - ptr) return -1;
        for (unsigned q = 0; q + 1 < nargs; q++) {
            if (q == p || !type_is_slice(&f->vtype[q])) continue;
            if (f->vtype[p].s != OSC_T_CELLS && f->vtype[q].s != OSC_T_CELLS) continue;
            uint64_t qn = args[q + 1], qsz = qn * (f->vtype[q].s == OSC_T_CELLS ? 8 : 1);
            if (!sz || !qn || (f->vtype[q].s == OSC_T_CELLS && qn > UINT64_MAX / 8) || qsz > UINT64_MAX - args[q]) continue;
            if (ptr < args[q] + qsz && args[q] < ptr + sz) return -1;
        }
    }
    return 0;
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
