/* test_c2_ir: checker2's decoder and acceptance rules (omega#446).
 *
 * Usage: test_c2_ir <dir of producer-emitted .ir files>   (tests/checker2/differential.sh fills it)
 *
 * 1. Every unit in the dir: strict decode, ACCEPT, and checker2's own re-encoding is byte-identical.
 * 2. Robustness: truncated prefixes are refused; deterministic bit flips never crash (run under
 *    ASan/UBSan by the make target).
 * 3. Planted defects: each named defect is applied to the first unit (sorted by name) where it
 *    fits, and checker2 must refuse the mutated unit with the expected rule, and refuse its bytes.
 *    A defect that fits no unit fails the test (no silent skips).
 * 4. Hand-built units for the control-flow defects no producer output contains. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_ir.h"

static int fails, checks;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            fails++;                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
        }                                                      \
    } while (0)

typedef struct { char *name; uint8_t *b; size_t n; } Unit;
static Unit corpus[512];
static int ncorpus;

static int load(const char *dir)
{
    struct dirent **ents;
    int n = scandir(dir, &ents, NULL, alphasort);
    if (n < 0) { perror(dir); return -1; }
    for (int i = 0; i < n; i++) {
        size_t l = strlen(ents[i]->d_name);
        if (l > 3 && strcmp(ents[i]->d_name + l - 3, ".ir") == 0 && ncorpus < 512) {
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", dir, ents[i]->d_name);
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                Unit *u = &corpus[ncorpus];
                u->b = malloc((size_t)sz + 1);
                u->n = fread(u->b, 1, (size_t)sz, f);
                u->name = strdup(ents[i]->d_name);
                fclose(f);
                ncorpus++;
            }
        }
        free(ents[i]);
    }
    free(ents);
    return 0;
}

/* ------------------------------------------------------------------ mutation helpers */

static int isval(uint8_t s) { return s >= S_BOOL && s <= S_I64; }
static int isint(uint8_t s) { return s >= S_U8 && s <= S_I64; }
static unsigned widthof(uint8_t s)
{
    static const unsigned w[] = {0, 1, 8, 16, 32, 64, 8, 16, 32, 64};
    return s <= S_I64 ? w[s] : 0;
}

/* first register >= from with a value type, matching want (0 any value, else that scalar) and
 * differing from not (0 = no restriction); int_only restricts to integers. -1 if none. */
static int reg_val(const C2Func *f, int from, uint8_t want, uint8_t not, int int_only)
{
    for (int r = from; r < f->nvregs; r++) {
        uint8_t s = f->vt[r].s;
        if (!isval(s) || (want && s != want) || (not && s == not) || (int_only && !isint(s))) continue;
        if (r > 0 && r - 1 < f->nparams && (f->vt[r - 1].s == S_BYTES || f->vt[r - 1].s == S_CELLS)) continue;
        return r;
    }
    return -1;
}
static int reg_ref(const C2Func *f, int from)
{
    for (int r = from; r < f->nvregs; r++)
        if (f->vt[r].s == S_REF) return r;
    return -1;
}
static int newreg(C2Func *f, C2Type t)
{
    C2Type *nv = realloc(f->vt, (f->nvregs + 1u) * sizeof *nv);
    if (!nv) abort();
    f->vt = nv;
    f->vt[f->nvregs] = t;
    return f->nvregs++;
}
static void ins(C2Func *f, uint32_t pos, C2Insn in)
{
    C2Insn *ni = realloc(f->in, (f->ninsns + 1u) * sizeof *ni);
    if (!ni) abort();
    f->in = ni;
    memmove(&f->in[pos + 1], &f->in[pos], (f->ninsns - pos) * sizeof *f->in);
    f->in[pos] = in;
    f->ninsns++;
    for (int b = 0; b < f->nblocks; b++) {
        C2Block *k = &f->blk[b];
        if (pos >= k->first && pos < k->first + k->count) k->count++;
        else if (k->first > pos) k->first++;
    }
}
static void del(C2Func *f, uint32_t pos)
{
    memmove(&f->in[pos], &f->in[pos + 1], (f->ninsns - pos - 1) * sizeof *f->in);
    f->ninsns--;
    for (int b = 0; b < f->nblocks; b++) {
        C2Block *k = &f->blk[b];
        if (pos >= k->first && pos < k->first + k->count) k->count--;
        else if (k->first > pos) k->first--;
    }
}
static C2Insn mk(uint8_t op, int16_t a)
{
    C2Insn in;
    memset(&in, 0, sizeof in);
    in.op = op;
    in.dst = in.b = in.c = -1;
    for (int k = 0; k < C2_MAX_PARAMS; k++) in.args[k] = -1;
    in.a = a;
    return in;
}
static int block_of(const C2Func *f, uint32_t i)
{
    for (int b = 0; b < f->nblocks; b++)
        if (i >= f->blk[b].first && i < f->blk[b].first + f->blk[b].count) return b;
    return -1;
}
static int block_ends_ret(const C2Func *f, int b) { return f->in[f->blk[b].first + f->blk[b].count - 1].op == C2_RET; }
static int single_def_const(const C2Func *f, int r, uint32_t *at)
{
    int n = r < f->nparams ? 1 : 0;
    for (uint32_t i = 0; i < f->ninsns; i++) {
        const C2Insn *in = &f->in[i];
        int d = -1;
        switch (in->op) {
        case C2_BR: case C2_CBR: case C2_RET: case C2_TRAP: case C2_STORE: case C2_FSTORE: case C2_RELEASE:
        case C2_ADESTROY: case C2_PCLOSE: case C2_HFREE: case C2_HSTORE: case C2_SSTORE: break;
        default: d = in->dst;
        }
        if (d == r) { n++; *at = i; }
    }
    return n == 1 && f->in[*at].op == C2_CONST;
}
/* a CBR closing a loop header: some block at or after it branches back to it */
static int is_loop_header(const C2Func *f, int h)
{
    for (int b = h; b < f->nblocks; b++) {
        const C2Insn *t = &f->in[f->blk[b].first + f->blk[b].count - 1];
        if ((t->op == C2_BR && t->t == h) || (t->op == C2_CBR && (t->t == h || t->f == h))) return 1;
    }
    return 0;
}

#define EACH_INSN(op_)                                     \
    for (int fi = 0; fi < u->nfuncs; fi++)                 \
        for (uint32_t i = 0; i < u->fn[fi].ninsns; i++)    \
            if (u->fn[fi].in[i].op == (op_))
#define FN (&u->fn[fi])
#define IN (&u->fn[fi].in[i])

/* ------------------------------------------------------------------ the defects */

static int d_const_noncanon(C2Unit *u)
{
    EACH_INSN(C2_CONST) {
        uint8_t s = FN->vt[IN->dst].s;
        if (s == S_BOOL) { IN->imm = 2; return 1; }
        if (widthof(s) && widthof(s) < 64) { IN->imm = 1ull << widthof(s); return 1; }
    }
    return 0;
}
static int d_const_into_ref(C2Unit *u)
{
    EACH_INSN(C2_CONST) { int r = reg_ref(FN, FN->nparams); if (r >= 0) { IN->dst = (int16_t)r; return 1; } }
    return 0;
}
static int d_mov_scalar_mismatch(C2Unit *u)
{
    EACH_INSN(C2_MOV) {
        uint8_t s = FN->vt[IN->dst].s;
        if (!isval(s)) continue;
        int r = reg_val(FN, 0, 0, s, 0);
        if (r >= 0) { IN->a = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_mov_ref_from_scalar(C2Unit *u)
{
    EACH_INSN(C2_MOV) {
        if (FN->vt[IN->dst].s != S_REF) continue;
        int r = reg_val(FN, 0, 0, 0, 0);
        if (r >= 0) { IN->a = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_mov_kind_widen(C2Unit *u) /* shared borrow into a mut or own destination */
{
    EACH_INSN(C2_MOV) {
        const C2Type *td = &FN->vt[IN->dst];
        if (td->s != S_REF || td->kind == K_SHARED) continue;
        for (int r = 0; r < FN->nvregs; r++) {
            const C2Type *t = &FN->vt[r];
            if (t->s == S_REF && t->kind == K_SHARED && t->elem == td->elem && t->len == td->len && t->sid == td->sid) {
                IN->a = (int16_t)r;
                return 1;
            }
        }
    }
    return 0;
}
static int d_bin_subop(C2Unit *u) { EACH_INSN(C2_BIN) { IN->sub = B_LIMIT; return 1; } return 0; }
static int d_bin_mismatch(C2Unit *u)
{
    EACH_INSN(C2_BIN) {
        if (IN->sub == B_SHL || IN->sub == B_SHR) continue;
        uint8_t s = FN->vt[IN->dst].s;
        int r = reg_val(FN, 0, 0, s, 1);
        if (isint(s) && r >= 0) { IN->b = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_shift_bool(C2Unit *u)
{
    EACH_INSN(C2_BIN) {
        if (IN->sub != B_SHL && IN->sub != B_SHR) continue;
        int r = reg_val(FN, 0, S_BOOL, 0, 0);
        if (r >= 0) { IN->b = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_bin_bool_arith(C2Unit *u)
{
    EACH_INSN(C2_BIN) { if (FN->vt[IN->dst].s == S_BOOL) { IN->sub = B_ADD; return 1; } }
    EACH_INSN(C2_CMP) { /* no bool BIN in the corpus: make one from a comparison result */
        int16_t d = IN->dst;
        for (uint32_t j = i + 1; j < FN->ninsns; j++)
            if (FN->in[j].op == C2_BIN && isint(FN->vt[FN->in[j].dst].s) && FN->in[j].sub == B_ADD) {
                FN->in[j].dst = FN->in[j].a = FN->in[j].b = d;
                return 1;
            }
    }
    return 0;
}
static int d_un_subop(C2Unit *u) { EACH_INSN(C2_UN) { IN->sub = U_LIMIT; return 1; } return 0; }
static int d_un_lnot_int(C2Unit *u)
{
    EACH_INSN(C2_UN) { if (IN->sub != U_LNOT) { IN->sub = U_LNOT; return 1; } }
    return 0;
}
static int d_un_neg_bool(C2Unit *u)
{
    EACH_INSN(C2_UN) { if (IN->sub == U_LNOT) { IN->sub = U_NEG; return 1; } }
    return 0;
}
static int d_cmp_nonbool(C2Unit *u)
{
    EACH_INSN(C2_CMP) { int r = reg_val(FN, FN->nparams, 0, S_BOOL, 1); if (r >= 0) { IN->dst = (int16_t)r; return 1; } }
    return 0;
}
static int d_cmp_subop(C2Unit *u) { EACH_INSN(C2_CMP) { IN->sub = CC_LIMIT; return 1; } return 0; }
static int d_cmp_mixed(C2Unit *u)
{
    EACH_INSN(C2_CMP) {
        int r = reg_val(FN, 0, 0, FN->vt[IN->a].s, 0);
        if (r >= 0) { IN->b = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_cast_bool(C2Unit *u)
{
    EACH_INSN(C2_CAST) { int r = reg_val(FN, 0, S_BOOL, 0, 0); if (r >= 0) { IN->a = (int16_t)r; return 1; } }
    return 0;
}
static int d_alloc_nonowner(C2Unit *u)
{
    EACH_INSN(C2_ALLOC) { int r = reg_val(FN, FN->nparams, 0, 0, 0); if (r >= 0) { IN->dst = (int16_t)r; return 1; } }
    return 0;
}
static int d_alloc_init(C2Unit *u)
{
    EACH_INSN(C2_ALLOC) {
        const C2Type *t = &FN->vt[IN->dst];
        uint8_t want = t->sid ? S_U64 : t->elem;
        int r = reg_val(FN, 0, 0, want, 0);
        if (r >= 0) { IN->a = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_release_scalar(C2Unit *u)
{
    EACH_INSN(C2_RELEASE) { int r = reg_val(FN, 0, 0, 0, 0); if (r >= 0) { IN->a = (int16_t)r; return 1; } }
    return 0;
}
static int d_load_index_bool(C2Unit *u)
{
    EACH_INSN(C2_LOAD) { int r = reg_val(FN, 0, S_BOOL, 0, 0); if (r >= 0) { IN->b = (int16_t)r; return 1; } }
    return 0;
}
static int d_load_nonarray(C2Unit *u)
{
    EACH_INSN(C2_LOAD) { int r = reg_val(FN, 0, 0, 0, 0); if (r >= 0) { IN->a = (int16_t)r; return 1; } }
    return 0;
}
static int d_load_elem(C2Unit *u)
{
    EACH_INSN(C2_LOAD) {
        int r = reg_val(FN, FN->nparams, 0, FN->vt[IN->a].elem, 0);
        if (r >= 0) { IN->dst = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_store_shared(C2Unit *u)
{
    EACH_INSN(C2_STORE) { if (FN->vt[IN->a].kind == K_MUT) { FN->vt[IN->a].kind = K_SHARED; return 1; } }
    return 0;
}
static int d_fload_field_range(C2Unit *u)
{
    EACH_INSN(C2_FLOAD) { IN->imm = u->st[FN->vt[IN->a].sid - 1].nfields; return 1; }
    return 0;
}
static int d_fload_type(C2Unit *u)
{
    EACH_INSN(C2_FLOAD) {
        uint8_t s = u->st[FN->vt[IN->a].sid - 1].f[IN->imm].s;
        int r = reg_val(FN, FN->nparams, 0, s, 0);
        if (r >= 0) { IN->dst = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_fstore_elem_index(C2Unit *u)
{
    EACH_INSN(C2_FSTORE) {
        if (IN->b != -1) continue;
        int r = reg_val(FN, 0, 0, 0, 1);
        if (r >= 0) { IN->b = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_fstore_shared(C2Unit *u)
{
    EACH_INSN(C2_FSTORE) { if (FN->vt[IN->a].kind == K_MUT) { FN->vt[IN->a].kind = K_SHARED; return 1; } }
    return 0;
}
static int d_arena_cap0(C2Unit *u) { EACH_INSN(C2_AOPEN) { IN->imm = 0; return 1; } return 0; }
static int d_arena_cap65(C2Unit *u) { EACH_INSN(C2_AOPEN) { IN->imm = C2_MAX_ARRAY + 1; return 1; } return 0; }
static int d_pool_cap17(C2Unit *u) { EACH_INSN(C2_POPEN) { IN->nargs = C2_POOL_SLOTS + 1; return 1; } return 0; }
static int d_pool_cap0(C2Unit *u) { EACH_INSN(C2_POPEN) { IN->nargs = 0; return 1; } return 0; }
static int d_pool_elem_ref(C2Unit *u) { EACH_INSN(C2_POPEN) { IN->sub = S_REF; return 1; } return 0; }
static int d_trap0(C2Unit *u) { EACH_INSN(C2_TRAP) { IN->imm = 0; return 1; } return 0; }
static int d_trap15(C2Unit *u) { EACH_INSN(C2_TRAP) { IN->imm = C2_TRAP_MAX + 1; return 1; } return 0; }
static int d_br_range(C2Unit *u) { EACH_INSN(C2_BR) { IN->t = FN->nblocks; return 1; } return 0; }
static int d_cbr_range(C2Unit *u) { EACH_INSN(C2_CBR) { IN->f = FN->nblocks; return 1; } return 0; }
static int d_cbr_nonbool(C2Unit *u)
{
    EACH_INSN(C2_CBR) { int r = reg_val(FN, 0, 0, 0, 1); if (r >= 0) { IN->a = (int16_t)r; return 1; } }
    return 0;
}
static int d_ret_type(C2Unit *u)
{
    EACH_INSN(C2_RET) {
        if (IN->a < 0) continue;
        int r = reg_val(FN, 0, 0, FN->ret.s, 0);
        if (r >= 0) { IN->a = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_ret_void_value(C2Unit *u)
{
    EACH_INSN(C2_RET) { if (IN->a < 0 && FN->nvregs > 0) { IN->a = 0; return 1; } }
    return 0;
}
static int d_call_self(C2Unit *u) { EACH_INSN(C2_CALL) { IN->callee = (uint16_t)fi; return 1; } return 0; }
static int d_call_arity(C2Unit *u)
{
    EACH_INSN(C2_CALL) { if (IN->nargs > 0) { IN->nargs--; return 1; } }
    return 0;
}
static int d_call_arg_type(C2Unit *u)
{
    EACH_INSN(C2_CALL) {
        const C2Func *g = &u->fn[IN->callee];
        for (int k = 0; k < IN->nargs; k++) {
            if (!isval(g->vt[k].s)) continue;
            if (k > 0 && (g->vt[k - 1].s == S_BYTES || g->vt[k - 1].s == S_CELLS)) continue;
            int r = reg_val(FN, 0, 0, g->vt[k].s, 0);
            if (r >= 0) { IN->args[k] = (int16_t)r; return 1; }
        }
    }
    return 0;
}
static int d_call_ref_kind(C2Unit *u) /* shared borrow passed to a mut parameter */
{
    EACH_INSN(C2_CALL) {
        const C2Func *g = &u->fn[IN->callee];
        for (int k = 0; k < IN->nargs; k++) {
            const C2Type *p = &g->vt[k];
            if (p->s != S_REF || p->kind != K_MUT) continue;
            for (int r = 0; r < FN->nvregs; r++) {
                const C2Type *t = &FN->vt[r];
                if (t->s == S_REF && t->kind == K_SHARED && t->elem == p->elem && t->len == p->len && t->sid == p->sid) {
                    IN->args[k] = (int16_t)r;
                    return 1;
                }
            }
        }
    }
    return 0;
}
static int d_call_ret_type(C2Unit *u)
{
    EACH_INSN(C2_CALL) {
        if (IN->dst < 0) continue;
        int r = reg_val(FN, FN->nparams, 0, FN->vt[IN->dst].s, 0);
        if (r >= 0) { IN->dst = (int16_t)r; return 1; }
    }
    return 0;
}
static int d_call_alias(C2Unit *u) /* one register bound to two parameters, one of them writable */
{
    EACH_INSN(C2_CALL) {
        const C2Func *g = &u->fn[IN->callee];
        for (int k = 0; k < IN->nargs; k++)
            for (int j = 0; j < IN->nargs; j++) {
                const C2Type *pk = &g->vt[k], *pj = &g->vt[j];
                if (j == k || pk->s != S_REF || pj->s != S_REF || pk->kind != K_MUT) continue;
                if (pk->elem != pj->elem || pk->len != pj->len || pk->sid != pj->sid) continue;
                IN->args[j] = IN->args[k];
                return 1;
            }
    }
    return 0;
}
static int d_term_missing(C2Unit *u)
{
    EACH_INSN(C2_BR) { IN->op = C2_TRAP; IN->imm = 1; return 1; }
    return 0;
}
static int d_term_inside(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (int b = 0; b < FN->nblocks; b++)
            if (FN->blk[b].count >= 2) {
                C2Insn *in = &FN->in[FN->blk[b].first];
                *in = mk(C2_BR, -1);
                in->t = 0;
                return 1;
            }
    return 0;
}
static int d_block_gap(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++)
        if (FN->nblocks >= 2 && FN->blk[1].count >= 2) { FN->blk[1].first++; FN->blk[1].count--; return 1; }
    return 0;
}
static int d_undefined_use(C2Unit *u)
{
    EACH_INSN(C2_BIN) { IN->a = (int16_t)newreg(FN, FN->vt[IN->a]); return 1; }
    return 0;
}
static int d_operand_range(C2Unit *u) { EACH_INSN(C2_BIN) { IN->a = (int16_t)FN->nvregs; return 1; } return 0; }
static int slice_param(const C2Func *f, uint8_t s)
{
    for (int r = 0; r + 1 < f->nparams; r++)
        if (f->vt[r].s == s || (!s && (f->vt[r].s == S_BYTES || f->vt[r].s == S_CELLS))) return r;
    return -1;
}
static int d_slice_len_write(C2Unit *u)
{
    EACH_INSN(C2_CONST) {
        int p = slice_param(FN, 0);
        if (p >= 0 && FN->vt[IN->dst].s == S_U64) { IN->dst = (int16_t)(p + 1); return 1; }
    }
    return 0;
}
static int d_slice_ptr_write(C2Unit *u)
{
    EACH_INSN(C2_CONST) { int p = slice_param(FN, 0); if (p >= 0) { IN->dst = (int16_t)p; return 1; } }
    return 0;
}
static int d_sstore_bytes(C2Unit *u)
{
    EACH_INSN(C2_SSTORE) { FN->vt[IN->a].s = S_BYTES; return 1; }
    return 0;
}
static int d_slice_len_type(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) { int p = slice_param(FN, 0); if (p >= 0) { FN->vt[p + 1].s = S_U32; return 1; } }
    return 0;
}
static int d_slice_nonparam(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) { int p = slice_param(FN, 0); if (p >= 0) { FN->nparams = (uint8_t)p; return 1; } }
    return 0;
}
static int d_double_release(C2Unit *u) { EACH_INSN(C2_RELEASE) { ins(FN, i + 1, *IN); return 1; } return 0; }
static int d_use_after_release(C2Unit *u)
{
    EACH_INSN(C2_LOAD) {
        int16_t a = IN->a;
        if (FN->vt[a].kind != K_OWN || a < FN->nparams) continue;
        ins(FN, i, mk(C2_RELEASE, a));
        return 1;
    }
    return 0;
}
static int d_leak(C2Unit *u)
{
    EACH_INSN(C2_RELEASE) {
        if (!block_ends_ret(FN, block_of(FN, i))) continue;
        del(FN, i);
        return 1;
    }
    return 0;
}
static int d_owner_overwrite(C2Unit *u) { EACH_INSN(C2_ALLOC) { ins(FN, i + 1, *IN); return 1; } return 0; }
static int d_release_arena_obj(C2Unit *u) { EACH_INSN(C2_AALLOC) { ins(FN, i + 1, mk(C2_RELEASE, IN->dst)); return 1; } return 0; }
static int d_arena_alloc_after_destroy(C2Unit *u) { EACH_INSN(C2_AALLOC) { ins(FN, i, mk(C2_ADESTROY, IN->b)); return 1; } return 0; }
static int d_arena_double_destroy(C2Unit *u) { EACH_INSN(C2_ADESTROY) { ins(FN, i + 1, *IN); return 1; } return 0; }
static int d_pool_double_close(C2Unit *u) { EACH_INSN(C2_PCLOSE) { ins(FN, i + 1, *IN); return 1; } return 0; }
static int d_pool_use_after_close(C2Unit *u) { EACH_INSN(C2_HALLOC) { ins(FN, i, mk(C2_PCLOSE, IN->b)); return 1; } return 0; }
static int d_arena_leak(C2Unit *u)
{
    EACH_INSN(C2_ADESTROY) { if (block_ends_ret(FN, block_of(FN, i))) { del(FN, i); return 1; } }
    return 0;
}
static int d_pool_leak(C2Unit *u)
{
    EACH_INSN(C2_PCLOSE) { if (block_ends_ret(FN, block_of(FN, i))) { del(FN, i); return 1; } }
    return 0;
}
static int d_handle_as_data(C2Unit *u)
{
    EACH_INSN(C2_AOPEN) {
        int16_t h = IN->dst;
        for (uint32_t j = 0; j < FN->ninsns; j++) {
            C2Insn *o = &FN->in[j];
            if (o->op == C2_BIN && FN->vt[o->a].s == S_U64) { o->a = h; return 1; }
            if (o->op == C2_CMP && FN->vt[o->a].s == S_U64) { o->a = h; return 1; }
        }
        C2Insn c = mk(C2_CMP, h); /* no u64 arithmetic nearby: compare the handle with itself */
        c.sub = CC_EQ;
        c.b = h;
        c.dst = (int16_t)newreg(FN, (C2Type){.s = S_BOOL});
        ins(FN, i + 1, c);
        return 1;
    }
    return 0;
}
static int d_handle_forged(C2Unit *u)
{
    EACH_INSN(C2_AALLOC) { IN->b = (int16_t)newreg(FN, (C2Type){.s = S_U64}); return 1; }
    return 0;
}
static int d_pool_value_type(C2Unit *u)
{
    EACH_INSN(C2_HLOAD) {
        int r = newreg(FN, (C2Type){.s = FN->vt[IN->dst].s == S_U8 ? S_U16 : S_U8});
        if (r >= 0) { IN->dst = (int16_t)r; return 1; }
    }
    return 0;
}
/* the CMP feeding the CBR that ends a loop header block, with its bound K a single-def CONST */
static int loop_guard(C2Func *f, uint32_t *cmp_at, uint32_t *k_at, int *hdr)
{
    for (int h = 0; h < f->nblocks; h++) {
        if (!is_loop_header(f, h)) continue;
        for (int g = h; g < f->nblocks; g++) {
            C2Insn *t = &f->in[f->blk[g].first + f->blk[g].count - 1];
            if (t->op != C2_CBR) continue;
            for (uint32_t i = f->blk[g].first; i < f->blk[g].first + f->blk[g].count; i++) {
                C2Insn *c = &f->in[i];
                if (c->op == C2_CMP && c->dst == t->a && (c->sub == CC_GE || c->sub == CC_LT) &&
                    single_def_const(f, c->b, k_at)) {
                    *cmp_at = i;
                    *hdr = g;
                    return 1;
                }
            }
        }
    }
    return 0;
}
static int d_loop_bound_huge(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) {
        uint32_t c, k;
        int g;
        if (!loop_guard(FN, &c, &k, &g)) continue;
        uint8_t s = FN->vt[FN->in[c].b].s;
        if (widthof(s) < 32) continue;
        FN->in[k].imm = 2000000;
        return 1;
    }
    return 0;
}
static int d_loop_counter_down(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) {
        uint32_t c, k;
        int g;
        if (!loop_guard(FN, &c, &k, &g)) continue;
        int16_t ctr = FN->in[c].a;
        for (uint32_t i = 0; i < FN->ninsns; i++)
            if (IN->op == C2_BIN && IN->dst == ctr && IN->a == ctr && IN->sub == B_ADD) { IN->sub = B_SUB; return 1; }
    }
    return 0;
}
static int d_loop_guard_swapped(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) {
        uint32_t c, k;
        int g;
        if (!loop_guard(FN, &c, &k, &g)) continue;
        C2Insn *t = &FN->in[FN->blk[g].first + FN->blk[g].count - 1];
        uint16_t x = t->t;
        t->t = t->f;
        t->f = x;
        return 1;
    }
    return 0;
}
static int d_struct_offset(C2Unit *u)
{
    for (int s = 0; s < u->nstructs; s++) if (u->st[s].nfields >= 2) { u->st[s].f[1].off++; return 1; }
    return 0;
}
static int d_struct_cells(C2Unit *u) { if (u->nstructs) { u->st[0].ncells++; return 1; } return 0; }
static int d_struct_dup_field(C2Unit *u)
{
    for (int s = 0; s < u->nstructs; s++)
        if (u->st[s].nfields >= 2) { strcpy(u->st[s].f[1].name, u->st[s].f[0].name); return 1; }
    return 0;
}
static int d_struct_field_ref(C2Unit *u) { if (u->nstructs) { u->st[0].f[0].s = S_REF; return 1; } return 0; }
static int d_func_dup_name(C2Unit *u) { if (u->nfuncs >= 2) { strcpy(u->fn[1].name, u->fn[0].name); return 1; } return 0; }
static int d_func_ret_ref(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) {
        int r = reg_ref(FN, 0);
        if (r >= 0) { FN->ret = FN->vt[r]; return 1; }
    }
    return 0;
}
static int d_vreg_kind(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++) { int r = reg_ref(FN, 0); if (r >= 0) { FN->vt[r].kind = 4; return 1; } }
    return 0;
}
static int d_vreg_len0(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (int r = 0; r < FN->nvregs; r++)
            if (FN->vt[r].s == S_REF && !FN->vt[r].sid) { FN->vt[r].len = 0; return 1; }
    return 0;
}
static int d_vreg_sid(C2Unit *u)
{
    for (int fi = 0; fi < u->nfuncs; fi++)
        for (int r = 0; r < FN->nvregs; r++)
            if (FN->vt[r].s == S_REF && FN->vt[r].sid) { FN->vt[r].sid = (uint8_t)(u->nstructs + 1); return 1; }
    return 0;
}
static int d_params_over(C2Unit *u) { u->fn[0].nparams = C2_MAX_PARAMS + 1; return u->fn[0].nvregs > C2_MAX_PARAMS; }
static int d_borrow_outlives(C2Unit *u)
{
    EACH_INSN(C2_MOV) {
        int16_t b = IN->dst, o = IN->a;
        if (FN->vt[b].s != S_REF || FN->vt[b].kind == K_OWN || FN->vt[o].kind != K_OWN || o < FN->nparams) continue;
        int arena = 0;
        for (uint32_t j = 0; j < FN->ninsns; j++) arena |= FN->in[j].op == C2_AALLOC && FN->in[j].dst == o;
        if (arena) continue;
        for (uint32_t j = i + 1; j < FN->ninsns && block_of(FN, j) == block_of(FN, i); j++) {
            uint8_t op = FN->in[j].op;
            if ((op == C2_LOAD || op == C2_STORE || op == C2_FLOAD || op == C2_FSTORE) && FN->in[j].a == b) {
                ins(FN, j, mk(C2_RELEASE, o));
                return 1;
            }
        }
    }
    return 0;
}
static int d_use_after_move(C2Unit *u)
{
    EACH_INSN(C2_CALL) {
        const C2Func *g = &u->fn[IN->callee];
        for (int k = 0; k < IN->nargs; k++)
            if (g->vt[k].s == S_REF && g->vt[k].kind == K_OWN) { ins(FN, i + 1, *IN); return 1; }
    }
    return 0;
}
static int d_mov_after_move(C2Unit *u) /* own -> own move, then the moved-from owner moved again */
{
    EACH_INSN(C2_MOV) {
        if (FN->vt[IN->dst].kind != K_OWN || FN->vt[IN->a].s != S_REF || FN->vt[IN->a].kind != K_OWN) continue;
        C2Insn again = *IN;
        again.dst = (int16_t)newreg(FN, FN->vt[IN->dst]);
        ins(FN, i + 1, again);
        return 1;
    }
    return 0;
}

static int d_alloc_into_borrow(C2Unit *u)
{
    EACH_INSN(C2_ALLOC) {
        C2Type t = FN->vt[IN->dst];
        t.kind = K_MUT;
        IN->dst = (int16_t)newreg(FN, t);
        return 1;
    }
    return 0;
}
/* a table slot past nfields holding a plausible field: the field index must still be refused */
static int d_field_stale_slot(C2Unit *u)
{
    EACH_INSN(C2_FLOAD) {
        C2Struct *s = &u->st[FN->vt[IN->a].sid - 1];
        if (s->nfields >= C2_MAX_FIELDS) continue;
        s->f[s->nfields] = s->f[IN->imm];
        IN->imm = s->nfields;
        return 1;
    }
    return 0;
}
/* a scalar register whose unused element byte happens to match: indexing it must be refused */
static int d_index_scalar_stale_elem(C2Unit *u)
{
    EACH_INSN(C2_LOAD) {
        if (FN->vt[IN->a].sid) continue;
        int r = reg_val(FN, 0, S_U64, 0, 0);
        if (r < 0) continue;
        FN->vt[r].elem = FN->vt[IN->a].elem;
        IN->a = (int16_t)r;
        return 1;
    }
    return 0;
}
static int d_sload_non_slice(C2Unit *u)
{
    EACH_INSN(C2_SLOAD) {
        int r = reg_val(FN, 0, S_U64, 0, 0);
        IN->a = (int16_t)(r >= 0 ? r : newreg(FN, (C2Type){.s = S_U64}));
        return 1;
    }
    return 0;
}

/* hand-built function bodies replacing fn[0] (control-flow shapes no producer emits) */
static void replace_fn0(C2Unit *u, int nblocks, const C2Insn *code, int ncode, const int *counts)
{
    C2Func *f = &u->fn[0];
    free(f->in);
    free(f->blk);
    f->in = calloc((size_t)ncode, sizeof *f->in);
    f->blk = calloc((size_t)nblocks, sizeof *f->blk);
    memcpy(f->in, code, (size_t)ncode * sizeof *code);
    f->ninsns = (uint32_t)ncode;
    f->nblocks = (uint16_t)nblocks;
    uint32_t at = 0;
    for (int b = 0; b < nblocks; b++) { f->blk[b].first = at; f->blk[b].count = (uint32_t)counts[b]; at += (uint32_t)counts[b]; }
    f->ret.s = S_VOID;
}
static int d_irreducible(C2Unit *u)
{
    /* v_bool <- param or const; B0: CBR c B1 B2; B1: BR B2; B2: BR B1 (two entries into one cycle) */
    C2Func *f = &u->fn[0];
    int c = reg_val(f, 0, S_BOOL, 0, 0);
    if (c < 0 || c >= f->nparams) return 0;
    C2Insn code[3] = {mk(C2_CBR, (int16_t)c), mk(C2_BR, -1), mk(C2_BR, -1)};
    code[0].t = 1; code[0].f = 2; code[1].t = 2; code[2].t = 1;
    int counts[3] = {1, 1, 1};
    replace_fn0(u, 3, code, 3, counts);
    return 1;
}
static int d_endless_loop(C2Unit *u)
{
    C2Insn code[2] = {mk(C2_BR, -1), mk(C2_BR, -1)};
    code[0].t = 1; code[1].t = 1;
    int counts[2] = {1, 1};
    if (u->fn[0].ret.s != S_VOID) return 0;
    replace_fn0(u, 2, code, 2, counts);
    return 1;
}

typedef struct { const char *name, *rule; int (*apply)(C2Unit *); } Defect;
static const Defect defects[] = {
    {"const-noncanonical", "C2-CONST-CANONICAL", d_const_noncanon},
    {"const-into-reference", "C2-TYPE-CONST", d_const_into_ref},
    {"mov-scalar-mismatch", "C2-TYPE-MOV", d_mov_scalar_mismatch},
    {"mov-reference-from-scalar", "C2-TYPE-MOV", d_mov_ref_from_scalar},
    {"mov-shared-widened", "C2-TYPE-MOV", d_mov_kind_widen},
    {"bin-subop", "C2-DECODE-SUBOP", d_bin_subop},
    {"bin-operand-mismatch", "C2-TYPE-BIN", d_bin_mismatch},
    {"shift-by-bool", "C2-TYPE-BIN", d_shift_bool},
    {"bool-arithmetic", "C2-TYPE-BIN", d_bin_bool_arith},
    {"un-subop", "C2-DECODE-SUBOP", d_un_subop},
    {"lnot-on-int", "C2-TYPE-UN", d_un_lnot_int},
    {"neg-on-bool", "C2-TYPE-UN", d_un_neg_bool},
    {"cmp-result-not-bool", "C2-TYPE-CMP", d_cmp_nonbool},
    {"cmp-subop", "C2-DECODE-SUBOP", d_cmp_subop},
    {"cmp-mixed-types", "C2-TYPE-CMP", d_cmp_mixed},
    {"cast-from-bool", "C2-TYPE-CAST", d_cast_bool},
    {"alloc-into-scalar", "C2-TYPE-ALLOC", d_alloc_nonowner},
    {"alloc-init-type", "C2-TYPE-ALLOC", d_alloc_init},
    {"release-scalar", "C2-TYPE-RELEASE", d_release_scalar},
    {"index-by-bool", "C2-TYPE-INDEX", d_load_index_bool},
    {"index-non-array", "C2-TYPE-INDEX", d_load_nonarray},
    {"load-element-type", "C2-TYPE-INDEX", d_load_elem},
    {"store-through-shared", "C2-READ-ONLY", d_store_shared},
    {"field-out-of-range", "C2-TYPE-FIELD", d_fload_field_range},
    {"field-load-type", "C2-TYPE-FIELD", d_fload_type},
    {"field-scalar-indexed", "C2-TYPE-FIELD", d_fstore_elem_index},
    {"field-store-through-shared", "C2-READ-ONLY", d_fstore_shared},
    {"arena-capacity-0", "C2-ARENA-CAPACITY", d_arena_cap0},
    {"arena-capacity-65", "C2-ARENA-CAPACITY", d_arena_cap65},
    {"pool-slots-17", "C2-POOL-CAPACITY", d_pool_cap17},
    {"pool-slots-0", "C2-POOL-CAPACITY", d_pool_cap0},
    {"pool-of-references", "C2-TYPE-POOL", d_pool_elem_ref},
    {"trap-code-0", "C2-TRAP-CODE", d_trap0},
    {"trap-code-15", "C2-TRAP-CODE", d_trap15},
    {"branch-out-of-range", "C2-CFG-TARGET", d_br_range},
    {"cond-branch-out-of-range", "C2-CFG-TARGET", d_cbr_range},
    {"branch-on-int", "C2-TYPE-COND", d_cbr_nonbool},
    {"return-type", "C2-TYPE-RET", d_ret_type},
    {"void-returns-value", "C2-TYPE-RET", d_ret_void_value},
    {"recursive-call", "C2-CALL-ORDER", d_call_self},
    {"call-arity", "C2-CALL-ARITY", d_call_arity},
    {"call-argument-type", "C2-TYPE-CALL", d_call_arg_type},
    {"call-shared-to-mut", "C2-TYPE-CALL", d_call_ref_kind},
    {"call-result-type", "C2-TYPE-CALL", d_call_ret_type},
    {"call-aliased-mut", "C2-CALL-ALIAS", d_call_alias},
    {"block-without-terminator", "C2-CFG-TERMINATOR", d_term_missing},
    {"terminator-mid-block", "C2-CFG-TERMINATOR", d_term_inside},
    {"block-layout-gap", "C2-CFG-LAYOUT", d_block_gap},
    {"read-before-write", "C2-UNDEFINED-USE", d_undefined_use},
    {"operand-out-of-range", "C2-OPERAND-RANGE", d_operand_range},
    {"slice-length-written", "C2-SLICE-WRITE", d_slice_len_write},
    {"slice-pointer-written", "C2-SLICE-WRITE", d_slice_ptr_write},
    {"store-into-bytes-slice", "C2-SLICE-WRITE", d_sstore_bytes},
    {"slice-length-not-u64", "C2-SLICE-PARAM", d_slice_len_type},
    {"slice-not-a-parameter", "C2-SLICE-PARAM", d_slice_nonparam},
    {"double-release", "C2-OWN-DOUBLE-RELEASE", d_double_release},
    {"use-after-release", "C2-OWN-USE-DEAD", d_use_after_release},
    {"leak-at-return", "C2-OWN-LEAK", d_leak},
    {"owner-overwritten", "C2-OWN-LEAK", d_owner_overwrite},
    {"release-arena-object", "C2-OWN-RELEASE-ARENA", d_release_arena_obj},
    {"alloc-from-destroyed-arena", "C2-ARENA-DEAD", d_arena_alloc_after_destroy},
    {"arena-destroyed-twice", "C2-HANDLE-DOUBLE-CLOSE", d_arena_double_destroy},
    {"pool-closed-twice", "C2-HANDLE-DOUBLE-CLOSE", d_pool_double_close},
    {"pool-use-after-close", "C2-HANDLE-DEAD", d_pool_use_after_close},
    {"arena-never-destroyed", "C2-HANDLE-LEAK", d_arena_leak},
    {"pool-never-closed", "C2-HANDLE-LEAK", d_pool_leak},
    {"handle-used-as-data", "C2-HANDLE-DISCIPLINE", d_handle_as_data},
    {"forged-arena-handle", "C2-HANDLE-DISCIPLINE", d_handle_forged},
    {"pool-value-type", "C2-TYPE-POOL", d_pool_value_type},
    {"loop-bound-too-large", "C2-LOOP-UNBOUNDED", d_loop_bound_huge},
    {"loop-counter-decrements", "C2-LOOP-UNBOUNDED", d_loop_counter_down},
    {"loop-guard-inverted", "C2-LOOP-UNBOUNDED", d_loop_guard_swapped},
    {"struct-field-offset", "C2-STRUCT-LAYOUT", d_struct_offset},
    {"struct-cell-count", "C2-STRUCT-LAYOUT", d_struct_cells},
    {"struct-duplicate-field", "C2-STRUCT-NAME", d_struct_dup_field},
    {"struct-field-reference", "C2-STRUCT-FIELD-TYPE", d_struct_field_ref},
    {"duplicate-function", "C2-FUNC-NAME", d_func_dup_name},
    {"function-returns-reference", "C2-FUNC-RETURN", d_func_ret_ref},
    {"reference-kind-4", "C2-TYPE-INVALID", d_vreg_kind},
    {"array-length-0", "C2-TYPE-INVALID", d_vreg_len0},
    {"struct-id-out-of-range", "C2-TYPE-INVALID", d_vreg_sid},
    {"seven-parameters", "C2-FUNC-PARAMS", d_params_over},
    {"borrow-outlives-owner", "C2-BORROW-OUTLIVES-OWNER", d_borrow_outlives},
    {"use-after-move-into-call", "C2-OWN-USE-DEAD", d_use_after_move},
    {"move-after-move", "C2-OWN-USE-DEAD", d_mov_after_move},
    {"irreducible-cycle", "C2-CFG-IRREDUCIBLE", d_irreducible},
    {"endless-loop", "C2-LOOP-UNBOUNDED", d_endless_loop},
    {"alloc-into-borrow", "C2-TYPE-ALLOC", d_alloc_into_borrow},
    {"field-index-past-table", "C2-TYPE-FIELD", d_field_stale_slot},
    {"index-scalar-register", "C2-TYPE-INDEX", d_index_scalar_stale_elem},
    {"slice-load-from-scalar", "C2-TYPE-SLICE", d_sload_non_slice},
};
#define NDEFECTS ((int)(sizeof defects / sizeof defects[0]))

/* ------------------------------------------------------------------ byte-level defects */

static int bytes_reject(const uint8_t *b, size_t n, const char *want_rule, const char *what)
{
    C2Verdict v;
    int rc = c2_check_bytes(b, n, &v);
    CHECK(rc != 0, "%s: bytes accepted", what);
    if (rc != 0 && want_rule) CHECK(strcmp(v.rule, want_rule) == 0, "%s: rule %s, want %s", what, v.rule, want_rule);
    return rc != 0;
}

static int byte_defects(void)
{
    int planted = 0;
    const Unit *s = NULL, *plain = NULL;
    for (int i = 0; i < ncorpus; i++) {
        if (!s && corpus[i].b[7] >= 2) s = &corpus[i];
        if (!plain && corpus[i].b[7] == 1) plain = &corpus[i];
    }
    CHECK(s && plain, "corpus needs a version-1 and a version>=2 unit");
    if (!s || !plain) return 0;
    uint8_t *b = malloc(s->n + 1);
    memcpy(b, s->b, s->n);
    b[0] ^= 1;
    planted += bytes_reject(b, s->n, "C2-DECODE-MAGIC", "bad-magic");
    memcpy(b, s->b, s->n);
    b[7] = 6;
    planted += bytes_reject(b, s->n, "C2-DECODE-VERSION", "unknown-version");
    memcpy(b, s->b, s->n);
    b[s->n] = 0;
    planted += bytes_reject(b, s->n + 1, "C2-DECODE-TRAILING", "trailing-byte");
    planted += bytes_reject(s->b, s->n - 1, NULL, "truncated-by-one");
    free(b);
    /* a version-1 unit relabelled as version 2 needs a struct table it does not have */
    b = malloc(plain->n + 1);
    memcpy(b, plain->b, 7);
    b[7] = 2;
    b[8] = 0; /* empty struct table: well formed but the canonical version for no structs is 1 */
    memcpy(b + 9, plain->b + 8, plain->n - 8);
    planted += bytes_reject(b, plain->n + 1, "C2-DECODE-VERSION-NONCANONICAL", "version-not-canonical");
    free(b);
    return planted;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: test_c2_ir <ir-dir>\n"); return 2; }
    if (load(argv[1]) || ncorpus == 0) { fprintf(stderr, "test_c2_ir: no .ir files in %s\n", argv[1]); return 2; }
    static uint8_t enc[1 << 20];
    C2Unit *u = malloc(sizeof *u);
    C2Verdict v;

    /* 1. accept and round-trip */
    int accepted = 0;
    for (int i = 0; i < ncorpus; i++) {
        int d = c2_decode(corpus[i].b, corpus[i].n, u, &v);
        CHECK(d == 0, "%s: decode refused: %s %s", corpus[i].name, v.rule, v.detail);
        if (d == 0) {
            int c = c2_check_unit(u, &v);
            CHECK(c == 0, "%s: rejected: %s f%d i%d %s", corpus[i].name, v.rule, v.func, v.insn, v.detail);
            size_t n = c2_encode(u, enc, sizeof enc);
            CHECK(n == corpus[i].n && memcmp(enc, corpus[i].b, n) == 0, "%s: re-encoding differs", corpus[i].name);
            accepted += c == 0;
        }
        c2_unit_free(u);
    }
    CHECK(accepted == ncorpus, "accepted %d of %d", accepted, ncorpus);

    /* 2. robustness: truncations refused; bit flips never crash */
    int truncs = 0, flips = 0, flip_rejects = 0;
    for (int i = 0; i < ncorpus; i++) {
        size_t n = corpus[i].n;
        for (size_t k = 0; k < 72; k++) {
            size_t cut = k < 64 ? n * k / 64 : n - (72 - k);
            if (cut >= n) continue;
            truncs++;
            CHECK(c2_check_bytes(corpus[i].b, cut, &v) != 0, "%s: prefix of %zu bytes accepted", corpus[i].name, cut);
        }
        uint8_t *b = malloc(n);
        for (int k = 0; k < 96; k++) {
            memcpy(b, corpus[i].b, n);
            size_t pos = (size_t)(((uint64_t)k * 2654435761u + (uint64_t)i * 40503u) % n);
            b[pos] ^= (uint8_t)(1u << (k & 7));
            flips++;
            flip_rejects += c2_check_bytes(b, n, &v) != 0;
        }
        free(b);
    }

    /* 3. planted defects */
    int planted = 0;
    for (int d = 0; d < NDEFECTS; d++) {
        int applied = 0;
        for (int i = 0; i < ncorpus && !applied; i++) {
            if (c2_decode(corpus[i].b, corpus[i].n, u, &v)) { c2_unit_free(u); continue; }
            if (defects[d].apply(u)) {
                applied = 1;
                int c = c2_check_unit(u, &v);
                CHECK(c != 0, "defect %s on %s: accepted", defects[d].name, corpus[i].name);
                if (c != 0)
                    CHECK(strcmp(v.rule, defects[d].rule) == 0, "defect %s on %s: rule %s (%s), want %s", defects[d].name,
                          corpus[i].name, v.rule, v.detail, defects[d].rule);
                int ok = c != 0 && strcmp(v.rule, defects[d].rule) == 0;
                size_t n = c2_encode(u, enc, sizeof enc);
                if (n) CHECK(c2_check_bytes(enc, n, &v) != 0, "defect %s on %s: its bytes are accepted", defects[d].name, corpus[i].name);
                planted += ok;
            }
            c2_unit_free(u);
        }
        CHECK(applied, "defect %s fits no corpus unit", defects[d].name);
    }
    planted += byte_defects();
    CHECK(planted >= 50, "gate needs >= 50 planted defects refused, got %d", planted);
    free(u);

    printf("test_c2_ir: %d units accepted and re-encoded identically, %d truncations refused, %d bit flips (%d refused), "
           "%d planted defects refused; %d checks, %d failed\n",
           accepted, truncs, flips, flip_rejects, planted, checks, fails);
    return fails ? 1 : 0;
}
