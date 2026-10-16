/*
 * test_osc_ir_decode.c -- omega#444: the strict OSC binary IR decoder
 * (osc_ir_decode, src/compiler/osc_ir.c) against the format in
 * spec/one-layer-format.md.
 *
 *   1. corpus      every .osc unit under tests/compiler/{progs,slices}, src/osh and
 *                  tests/physics0/osc that the front end compiles: encode -> decode
 *                  -> encode is byte-identical, the decoded unit equals the compiled
 *                  one in every encoded field, digests agree, all five versions seen.
 *   2. valid       unit-level variants of the corpus (constants, names, contract
 *                  text, operators, trap codes) that pass osc_ir_validate: at least
 *                  VALID_MIN distinct encodings round-trip byte-exactly.
 *   3. identity    a one-character change to requires/ensures text changes the
 *                  digest and the encoding, and both decode.
 *   4. classes     one hand-built counterexample per reachable refusal class, built
 *                  by an independent writer in this file (not osc_ir_encode).
 *   5. malformed   every byte of every corpus encoding flipped three ways, every
 *                  truncation, trailing bytes, and seeded random multi-edits: each
 *                  case is refused with a named class and a zeroed unit, or
 *                  admitted only if it is itself a canonical encoding of a valid
 *                  unit (re-encodes to the same bytes). At least MALFORMED_MIN cases.
 *   6. controls    the checks above are shown to fail on planted defects.
 *
 * Usage: test_osc_ir_decode [quick]   (run from the repository root)
 * Prints OSC_IR_DECODE_PASS on success. Decoder mutants (OSC_IR_DECODE_MUTANT=k)
 * must make this program fail; mk/compiler.mk checks that they do.
 * No Python; seeded PRNG; deterministic.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_front.h"
#include "osc_ir.h"
#include "sha256.h"

/* spec/one-layer-format.md section 3 states these numbers; a renumbering must fail here. */
_Static_assert(OSC_I_CONST == 1 && OSC_I_CALL == 11 && OSC_I_RET == 15 && OSC_I_FLOAD == 16 && OSC_I_AOPEN == 18 &&
               OSC_I_POPEN == 21 && OSC_I_HSTORE == 27 && OSC_I_SSTORE == 29, "opcode numbers in the spec");
_Static_assert(OSC_T_BOOL == 1 && OSC_T_U64 == 5 && OSC_T_I64 == 9 && OSC_T_REF == 10 && OSC_T_CELLS == 12, "scalar tags in the spec");
_Static_assert(OSC_REF_OWN == 1 && OSC_REF_MUT == 3, "ref kinds in the spec");

static int g_fail;
static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "FAIL: ");
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    va_end(ap);
    if (++g_fail > 40) { printf("too many failures\n"); exit(1); }
}
#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

static uint64_t g_rng = 0x05c0de444d0c0de1ULL;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static uint64_t rndn(uint64_t n) { return n ? rnd() % n : 0; }

/* ---- corpus ------------------------------------------------------------ */
#define MAX_CORPUS 128
typedef struct {
    char name[160];
    OscUnit *u;
    uint8_t *enc;
    size_t len;
} Entry;
static Entry g_corpus[MAX_CORPUS];
static int g_ncorpus;

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[sz] = 0;
    *n = (size_t)sz;
    return b;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static uint8_t *encode_alloc(const OscUnit *u, size_t *len) {
    size_t n = 0;
    if (osc_ir_encode(u, NULL, 0, &n)) return NULL;
    uint8_t *b = malloc(n ? n : 1);
    if (!b || osc_ir_encode(u, b, n, len) || *len != n) { free(b); return NULL; }
    return b;
}

static int g_compile_refused;
static void load_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[512];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 512) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".osc")) names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof names[0], cmp_str);
    for (int i = 0; i < nn; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        free(names[i]);
        size_t n;
        char *src = read_file(path, &n);
        if (!src) { fail("cannot read %s", path); continue; }
        OscUnit *u = calloc(1, sizeof *u);
        OscDiag dg;
        memset(&dg, 0, sizeof dg);
        int rc = osc_compile(src, n, u, &dg, NULL);
        free(src);
        if (rc != 0) { g_compile_refused++; free(u); continue; }  /* e.g. units with imports */
        if (g_ncorpus >= MAX_CORPUS) { free(u); fail("corpus over %d units", MAX_CORPUS); continue; }
        Entry *c = &g_corpus[g_ncorpus];
        snprintf(c->name, sizeof c->name, "%.150s", path);
        c->u = u;
        c->enc = encode_alloc(u, &c->len);
        if (!c->enc) { fail("%s: compiled unit does not encode", path); free(u); continue; }
        g_ncorpus++;
    }
}

/* ---- field equality over every encoded field --------------------------- */
static const char *type_diff(const OscType *a, const OscType *b) {
    if (a->s != b->s || a->ref != b->ref || a->elem != b->elem || a->len != b->len || a->sid != b->sid) return "type";
    return NULL;
}
/* NULL if equal, else what differs. `line` is not encoded and is not compared. */
static const char *unit_diff(const OscUnit *x, const OscUnit *y, char *buf, size_t n) {
    if (x->nfuncs != y->nfuncs) return "nfuncs";
    if (x->nstructs != y->nstructs) return "nstructs";
    for (int k = 0; k < x->nstructs; k++) {
        const OscStruct *a = &x->structs[k], *b = &y->structs[k];
        if (strcmp(a->name, b->name) || a->nfields != b->nfields || a->ncells != b->ncells) { snprintf(buf, n, "struct %d", k); return buf; }
        for (int f = 0; f < a->nfields; f++) {
            const OscField *p = &a->fields[f], *q = &b->fields[f];
            if (strcmp(p->name, q->name) || p->s != q->s || p->alen != q->alen || p->off != q->off) { snprintf(buf, n, "struct %d field %d", k, f); return buf; }
        }
    }
    for (int fi = 0; fi < x->nfuncs; fi++) {
        const OscFunc *a = &x->funcs[fi], *b = &y->funcs[fi];
        if (strcmp(a->name, b->name)) { snprintf(buf, n, "func %d name", fi); return buf; }
        if (a->nparams != b->nparams || a->nvregs != b->nvregs || a->nblocks != b->nblocks || a->ninsns != b->ninsns) { snprintf(buf, n, "func %d counts", fi); return buf; }
        if (type_diff(&a->ret, &b->ret)) { snprintf(buf, n, "func %d ret", fi); return buf; }
        for (int v = 0; v < a->nvregs; v++) if (type_diff(&a->vtype[v], &b->vtype[v])) { snprintf(buf, n, "func %d vreg %d type", fi, v); return buf; }
        for (int k = 0; k < a->nblocks; k++)
            if (a->blocks[k].first != b->blocks[k].first || a->blocks[k].count != b->blocks[k].count) { snprintf(buf, n, "func %d block %d", fi, k); return buf; }
        for (uint32_t i = 0; i < a->ninsns; i++) {
            const OscInsn *p = &a->insns[i], *q = &b->insns[i];
            const char *w = NULL;
            if (p->op != q->op) w = "op";
            else if (p->sub != q->sub) w = "sub";
            else if (p->dst != q->dst || p->a != q->a || p->b != q->b || p->c != q->c) w = "operand";
            else if (p->blk_t != q->blk_t || p->blk_f != q->blk_f) w = "target";
            else if (p->callee != q->callee || p->nargs != q->nargs) w = "call";
            else if (memcmp(p->args, q->args, sizeof p->args)) w = "args";
            else if (p->imm != q->imm) w = "imm";
            if (w) { snprintf(buf, n, "func %d insn %u (op %u) %s", fi, i, p->op, w); return buf; }
        }
        if (strcmp(a->requires_text, b->requires_text) || strcmp(a->ensures_text, b->ensures_text)) { snprintf(buf, n, "func %d contract text", fi); return buf; }
    }
    return NULL;
}

static OscUnit *g_dec, *g_tmp;  /* scratch units (large; heap) */
static uint8_t *g_e1, *g_e2;    /* OSC_IR_ENCODED_MAX bytes each: one encode call (one validation) per encoding */

/* Copy the part of a unit inside its counts: all that encode, validate and unit_diff read.
 * Cheaper than copying the whole 6.7 MB struct for every variant. */
static void copy_unit(OscUnit *d, const OscUnit *s) {
    d->nfuncs = s->nfuncs;
    d->nstructs = s->nstructs;
    memcpy(d->structs, s->structs, sizeof d->structs);
    for (int fi = 0; fi < s->nfuncs; fi++) {
        OscFunc *a = &d->funcs[fi];
        const OscFunc *b = &s->funcs[fi];
        memcpy(a->name, b->name, sizeof a->name);
        a->nparams = b->nparams;
        a->ret = b->ret;
        a->nvregs = b->nvregs;
        memcpy(a->vtype, b->vtype, b->nvregs * sizeof b->vtype[0]);
        a->nblocks = b->nblocks;
        memcpy(a->blocks, b->blocks, b->nblocks * sizeof b->blocks[0]);
        a->ninsns = b->ninsns;
        memcpy(a->insns, b->insns, b->ninsns * sizeof b->insns[0]);
        memcpy(a->requires_text, b->requires_text, sizeof a->requires_text);
        memcpy(a->ensures_text, b->ensures_text, sizeof a->ensures_text);
    }
}

/* encode u, decode, re-encode: byte-identical, fields equal, digest equal. 0 ok. */
static int round_trip(const OscUnit *u, const char *what) {
    size_t n = 0, n2 = 0;
    if (osc_ir_encode(u, g_e1, OSC_IR_ENCODED_MAX, &n)) { fail("%s: does not encode", what); return -1; }
    int rc = osc_ir_decode(g_e1, n, g_dec);
    if (rc != OSC_IR_OK) { fail("%s: decode refused %s", what, osc_ir_refusal_name(rc)); return -1; }
    int bad = osc_ir_encode(g_dec, g_e2, OSC_IR_ENCODED_MAX, &n2) != 0 || n2 != n || memcmp(g_e1, g_e2, n) != 0;
    if (bad) fail("%s: re-encoding differs (%zu vs %zu bytes)", what, n, n2);
    char buf[128];
    const char *d = unit_diff(u, g_dec, buf, sizeof buf);
    if (d) { fail("%s: decoded unit differs: %s", what, d); bad = 1; }
    uint8_t g1[32], g2[32];
    sha256_ctx sc;  /* identity = SHA-256 of the canonical encoding (osc_ir.h) */
    sha256_init(&sc);
    sha256_update(&sc, g_e1, n);
    sha256_final(&sc, g1);
    if (osc_ir_digest(g_dec, g2) || memcmp(g1, g2, 32)) { fail("%s: digest differs", what); bad = 1; }
    return bad ? -1 : 0;
}

/* ---- 1. corpus --------------------------------------------------------- */
static void test_corpus(void) {
    int seen[OSC_IR_VERSION_MAX + 1] = {0};
    int ok = 0;
    for (int i = 0; i < g_ncorpus; i++) {
        Entry *c = &g_corpus[i];
        int v = osc_ir_encoding_version(c->enc, c->len);
        if (v >= 1 && v <= OSC_IR_VERSION_MAX) seen[v]++;
        if (round_trip(c->u, c->name) == 0) ok++;
    }
    printf("corpus: units=%d round_trip_ok=%d compile_refused=%d versions v1=%d v2=%d v3=%d v4=%d v5=%d\n", g_ncorpus, ok,
           g_compile_refused, seen[1], seen[2], seen[3], seen[4], seen[5]);
    CHECK(g_ncorpus >= 40, "corpus too small: %d units", g_ncorpus);
    CHECK(ok == g_ncorpus, "corpus round trip: %d of %d", ok, g_ncorpus);
    for (int v = 1; v <= OSC_IR_VERSION_MAX; v++) CHECK(seen[v] > 0, "corpus has no version %d unit", v);
}

/* ---- 2. valid variants -------------------------------------------------- */
static uint64_t canon_rand(OscScalar s) {
    uint64_t x = rnd();
    unsigned w = osc_scalar_width(s);
    if (s == OSC_T_BOOL) return x & 1;
    if (w == 64 || w == 0) return x;
    if (osc_scalar_signed(s)) {
        int64_t v = (int64_t)(x << (64 - w)) >> (64 - w);
        return (uint64_t)v;
    }
    return x & ((1ULL << w) - 1);
}
static void rand_ident(char *d, size_t maxlen) {
    static const char al[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    size_t n = 1 + rndn(maxlen);
    d[0] = al[rndn(52)];
    for (size_t i = 1; i < n; i++) d[i] = al[rndn(sizeof al - 1)];
    d[n] = 0;
}
static void rand_text(char *d, size_t maxlen) {
    size_t n = rndn(maxlen + 1);
    for (size_t i = 0; i < n; i++) d[i] = (char)(0x20 + rndn(95));
    d[n] = 0;
}
static int find_insn(const OscFunc *f, uint8_t op) {
    if (!f->ninsns) return -1;
    uint32_t start = (uint32_t)rndn(f->ninsns);
    for (uint32_t k = 0; k < f->ninsns; k++) {
        uint32_t i = (start + k) % f->ninsns;
        if (f->insns[i].op == op) return (int)i;
    }
    return -1;
}
static void mutate_unit(OscUnit *u) {
    if (!u->nfuncs) return;
    OscFunc *f = &u->funcs[rndn(u->nfuncs)];
    int i;
    switch (rndn(8)) {
    case 0: case 1:
        if ((i = find_insn(f, OSC_I_CONST)) >= 0) f->insns[i].imm = canon_rand(f->vtype[f->insns[i].dst].s);
        break;
    case 2: rand_ident(f->name, OSC_NAME_MAX - 1); break;
    case 3: rand_text(rndn(2) ? f->requires_text : f->ensures_text, OSC_CLAUSE_MAX - 1); break;
    case 4: if ((i = find_insn(f, OSC_I_BIN)) >= 0) f->insns[i].sub = (uint8_t)(OSC_B_ADD + rndn(OSC_B_SHR)); break;
    case 5: if ((i = find_insn(f, OSC_I_CMP)) >= 0) f->insns[i].sub = (uint8_t)(OSC_C_EQ + rndn(OSC_C_GE)); break;
    case 6: if ((i = find_insn(f, OSC_I_TRAP)) >= 0) f->insns[i].imm = 1 + rndn(OSC_TRAP_MAX); break;
    case 7:
        if (u->nstructs) {
            OscStruct *s = &u->structs[rndn(u->nstructs)];
            if (rndn(2)) rand_ident(s->name, OSC_NAME_MAX - 1);
            else rand_ident(s->fields[rndn(s->nfields)].name, OSC_NAME_MAX - 1);
        } else rand_ident(f->name, OSC_NAME_MAX - 1);
        break;
    }
}

#define DIG_CAP (1u << 16)
static uint8_t (*g_digs)[32];
static unsigned g_ndigs;
static int digest_insert(const uint8_t d[32]) {  /* 1 if new */
    unsigned h = ((unsigned)d[0] | (unsigned)d[1] << 8 | (unsigned)d[2] << 16) & (DIG_CAP - 1);
    static const uint8_t zero[32];
    for (;; h = (h + 1) & (DIG_CAP - 1)) {
        if (!memcmp(g_digs[h], zero, 32)) { memcpy(g_digs[h], d, 32); g_ndigs++; return 1; }
        if (!memcmp(g_digs[h], d, 32)) return 0;
    }
}

static void test_valid_variants(unsigned want) {
    unsigned tries = 0, invalid = 0, ok_before = (unsigned)g_fail;
    for (int i = 0; i < g_ncorpus; i++) {
        uint8_t d[32];
        if (!osc_ir_digest(g_corpus[i].u, d)) digest_insert(d);
    }
    while (g_ndigs < want && tries < want * 20) {
        tries++;
        const Entry *c = &g_corpus[rndn((uint64_t)g_ncorpus)];
        copy_unit(g_tmp, c->u);
        unsigned k = 1 + (unsigned)rndn(3);
        for (unsigned j = 0; j < k; j++) mutate_unit(g_tmp);
        if (osc_ir_validate(g_tmp, NULL, 0)) { invalid++; continue; }
        uint8_t d[32];
        if (osc_ir_digest(g_tmp, d) || !digest_insert(d)) continue;
        char what[64];
        snprintf(what, sizeof what, "variant %u", g_ndigs);
        round_trip(g_tmp, what);
        if ((unsigned)g_fail > ok_before + 5) break;
    }
    printf("valid objects: distinct=%u tries=%u invalid_variants_skipped=%u (all round-trip byte-exact)\n", g_ndigs, tries, invalid);
    CHECK(g_ndigs >= want, "only %u distinct valid objects (want %u)", g_ndigs, want);
}

/* ---- 3. requirement changes change identity ----------------------------- */
static void test_requirement_identity(void) {
    int checked = 0;
    for (int i = 0; i < g_ncorpus; i++) {
        const OscUnit *u = g_corpus[i].u;
        for (int fi = 0; fi < u->nfuncs; fi++) {
            for (int which = 0; which < 2; which++) {
                copy_unit(g_tmp, u);
                char *t = which ? g_tmp->funcs[fi].ensures_text : g_tmp->funcs[fi].requires_text;
                size_t l = strlen(t);
                if (l) t[l - 1] = (char)(t[l - 1] == 'x' ? 'y' : 'x');
                else { t[0] = 'x'; t[1] = 0; }
                uint8_t g1[32], g2[32];
                CHECK(osc_ir_digest(u, g1) == 0 && osc_ir_digest(g_tmp, g2) == 0, "requirement identity: digest");
                CHECK(memcmp(g1, g2, 32) != 0, "%s func %d: %s change kept the identity", g_corpus[i].name, fi, which ? "ensures" : "requires");
                round_trip(g_tmp, "requirement-changed unit");
                checked++;
            }
        }
    }
    printf("requirement identity: %d single-character requires/ensures changes, each a new identity\n", checked);
    CHECK(checked > 0, "no requirement identity cases");
}

/* ---- 4. one counterexample per refusal class (independent writer) -------- */
typedef struct { uint8_t b[2048]; size_t n; } Buf;  /* largest case: 481 vregs, about 560 bytes */
static void p8(Buf *b, unsigned v) { b->b[b->n++] = (uint8_t)v; }
static void p16(Buf *b, unsigned v) { p8(b, v & 0xFF); p8(b, (v >> 8) & 0xFF); }
static void p32(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) p8(b, (v >> (8 * i)) & 0xFF); }
static void p64(Buf *b, uint64_t v) { for (int i = 0; i < 8; i++) p8(b, (unsigned)(v >> (8 * i)) & 0xFF); }
static void pbytes(Buf *b, const void *p, size_t k) { memcpy(b->b + b->n, p, k); b->n += k; }

/* Knobs for the hand-built unit: fn f() -> u8 { CONST v0 = imm; RET v0 } */
typedef struct {
    unsigned version;      /* magic byte 7 */
    int struct_table;      /* write the (empty) struct table byte */
    unsigned name_len;     /* function name length byte */
    const char *name;      /* name bytes (name_len of them) */
    unsigned nvregs;
    unsigned ret_tag;      /* scalar tag of the return type */
    unsigned op0;          /* opcode of insn 0 */
    unsigned dst;          /* CONST dst field (u16) */
    uint64_t imm;
    unsigned req_len;      /* requires text length field */
} Knobs;
static Knobs knobs_default(void) {
    Knobs k = {1, 0, 1, "f", 1, OSC_T_U8, OSC_I_CONST, 0, 7, 0};
    return k;
}
static void build(Buf *b, const Knobs *k) {
    b->n = 0;
    static const uint8_t magic[7] = {'O', 'S', 'C', '1', 'I', 'R', 0};
    pbytes(b, magic, 7);
    p8(b, k->version);
    if (k->struct_table) p8(b, 0);
    p16(b, 1);                       /* nfuncs */
    p8(b, k->name_len); pbytes(b, k->name, k->name_len);
    p8(b, 0);                        /* nparams */
    p8(b, k->ret_tag);               /* ret type (scalar: tag only) */
    p16(b, k->nvregs);
    for (unsigned v = 0; v < k->nvregs; v++) p8(b, OSC_T_U8);
    p16(b, 1); p32(b, 0); p32(b, 2); /* one block: insns 0..1 */
    p32(b, 2);                       /* ninsns */
    p8(b, k->op0); p16(b, k->dst); p64(b, k->imm);
    p8(b, OSC_I_RET); p16(b, 0);
    p16(b, k->req_len); for (unsigned i = 0; i < k->req_len && i < 300; i++) p8(b, 'r');
    p16(b, 0);                       /* ensures text */
}

static int expect_class(const uint8_t *buf, size_t len, int want, const char *what) {
    int rc = osc_ir_decode(buf, len, g_dec);
    CHECK(rc == want, "counterexample %s: got %s, want %s", what, osc_ir_refusal_name(rc), osc_ir_refusal_name(want));
    if (rc != OSC_IR_OK) CHECK(g_dec->nfuncs == 0 && g_dec->nstructs == 0 && g_dec->funcs[0].ninsns == 0 && g_dec->funcs[0].name[0] == 0,
                               "counterexample %s: refused but output not zeroed", what);
    return rc == want;
}

static void test_classes(void) {
    Buf b;
    Knobs k = knobs_default();
    int got = 0, total = 0;
    uint8_t refused[OSC_IR_REFUSAL_COUNT + 1] = {0};
#define EXPECT(buf, len, want, what) do { total++; if (expect_class(buf, len, want, what)) { got++; refused[-(want)] = 1; } } while (0)
    /* the canonical base case is admitted */
    build(&b, &k);
    EXPECT(b.b, b.n, OSC_IR_OK, "hand-built canonical unit");
    CHECK(g_dec->nfuncs == 1 && g_dec->funcs[0].insns[0].imm == 7 && g_dec->funcs[0].ret.s == OSC_T_U8, "hand-built unit fields");
    uint8_t enc[2048]; size_t n = 0;
    CHECK(osc_ir_encode(g_dec, enc, sizeof enc, &n) == 0 && n == b.n && !memcmp(enc, b.b, n), "independent writer disagrees with osc_ir_encode");

    total++; if (osc_ir_decode(NULL, 8, g_dec) == OSC_IR_REFUSE_ARG && osc_ir_decode(b.b, b.n, NULL) == OSC_IR_REFUSE_ARG) { got++; refused[1] = 1; }
    else fail("counterexample ARG");

    Buf m = b; m.b[3] = '2';
    EXPECT(m.b, m.n, OSC_IR_REFUSE_MAGIC, "wrong magic");
    EXPECT((const uint8_t *)"", 0, OSC_IR_REFUSE_TRUNCATED, "empty input");
    k = knobs_default(); k.version = 0; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_UNKNOWN_VERSION, "version 0");
    k.version = OSC_IR_VERSION_MAX + 1; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_UNKNOWN_VERSION, "version 6");
    EXPECT(b.b, b.n - 1, OSC_IR_REFUSE_TRUNCATED, "last byte missing");
    EXPECT(b.b, 7, OSC_IR_REFUSE_TRUNCATED, "magic without version");
    k = knobs_default(); k.name_len = 64; k.name = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl"; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_OVERSIZED, "function name of 64 bytes");
    k = knobs_default(); k.nvregs = OSC_MAX_VREGS + 1; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_OVERSIZED, "481 vregs");
    k = knobs_default(); k.req_len = OSC_CLAUSE_MAX; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_OVERSIZED, "requires text of 256 bytes");
    {
        size_t big = (size_t)OSC_IR_ENCODED_MAX + 1;
        uint8_t *x = calloc(1, big);
        memcpy(x, b.b, b.n);
        EXPECT(x, big, OSC_IR_REFUSE_OVERSIZED, "input longer than OSC_IR_ENCODED_MAX");
        free(x);
    }
    m = b; p8(&m, 0);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_TRAILING, "one trailing byte");
    k = knobs_default(); k.op0 = 0; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_RANGE, "opcode 0");
    k.op0 = OSC_I_SSTORE + 1; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_RANGE, "opcode past SSTORE");
    k = knobs_default(); k.ret_tag = OSC_T_CELLS + 1; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_RANGE, "scalar tag 13");
    k = knobs_default(); k.dst = 0x8000; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_RANGE, "register 0x8000");
    k = knobs_default(); k.name_len = 3; k.name = "f\0g"; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_TEXT, "NUL inside a function name");
    k = knobs_default(); k.imm = 256; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_INVALID, "u8 constant 256");
    k = knobs_default(); k.dst = 1; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_INVALID, "register out of range");
    k = knobs_default(); k.version = 2; k.struct_table = 1; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_VERSION_MISMATCH, "version 2 with no structs");
    k.version = 5; build(&m, &k);
    EXPECT(m.b, m.n, OSC_IR_REFUSE_VERSION_MISMATCH, "version 5 with no slices");
#undef EXPECT
    int classes = 0;
    for (int c = 1; c <= OSC_IR_REFUSAL_COUNT; c++) classes += refused[c];
    printf("refusal classes: %d/%d cases as expected; classes exhibited=%d of %d (NONCANONICAL is the unreachable backstop)\n", got, total,
           classes, OSC_IR_REFUSAL_COUNT);
    CHECK(got == total, "counterexamples: %d of %d", got, total);
    CHECK(classes == OSC_IR_REFUSAL_COUNT - 1 && !refused[-OSC_IR_REFUSE_NONCANONICAL], "refusal class coverage");
}

/* ---- 5. malformed sweep ------------------------------------------------- */
static unsigned long g_cases, g_admitted, g_class[OSC_IR_REFUSAL_COUNT + 1];

/* The oracle for one case: NULL if the decoder's answer is allowed, else why not. */
/* Independence: for an admitted case the oracle re-encodes with osc_ir_encode,
 * which runs the same encode_unit the decoder's canonical backstop uses. Only
 * the validity half (osc_ir_encode refuses an invalid unit) is independent of
 * the decoder; the byte-identity half shares encode_unit with it, so a bug in
 * encode_unit would be invisible here. The hand-written counterexamples in
 * section 4 come from an independent writer and pin each refusal class. */
static const char *judge(const uint8_t *m, size_t mlen, int rc, const OscUnit *out) {
    if (rc == OSC_IR_OK) {
        size_t n = 0;  /* osc_ir_encode validates first: an invalid unit does not encode */
        if (osc_ir_encode(out, g_e2, OSC_IR_ENCODED_MAX, &n)) return "admitted a unit that is invalid (does not encode)";
        if (n != mlen || memcmp(g_e2, m, mlen)) return "admitted bytes that do not re-encode identically";
        return NULL;
    }
    if (rc < -OSC_IR_REFUSAL_COUNT || rc > OSC_IR_REFUSE_MAGIC) return "refused with no named class";
    if (out->nfuncs || out->nstructs || out->funcs[0].ninsns || out->funcs[0].nvregs || out->funcs[0].name[0] || out->structs[0].name[0])
        return "refused but output not zeroed";
    return NULL;
}

static void one_case(const uint8_t *m, size_t mlen, const uint8_t *orig, size_t olen, const char *unit, const char *kind, size_t x) {
    if (mlen == olen && !memcmp(m, orig, olen)) return;  /* not malformed: identical to a valid encoding */
    g_cases++;
    int rc = osc_ir_decode(m, mlen, g_dec);
    if (rc == OSC_IR_OK) g_admitted++;
    else if (rc < 0 && rc >= -OSC_IR_REFUSAL_COUNT) g_class[-rc]++;
    const char *why = judge(m, mlen, rc, g_dec);
    if (why) fail("%s %s %zu (len %zu, rc %s): %s", unit, kind, x, mlen, osc_ir_refusal_name(rc), why);
}

static void test_malformed(unsigned long want, unsigned random_per_unit, size_t max_positions) {
    uint8_t *m = malloc(1 << 20);
    for (int i = 0; i < g_ncorpus && g_fail < 20; i++) {
        const Entry *c = &g_corpus[i];
        if (c->len + 64 > (1u << 20)) continue;
        size_t stride = (c->len + max_positions - 1) / max_positions;  /* every byte when len <= max_positions */
        if (stride < 1) stride = 1;
        static const uint8_t masks[3] = {0x01, 0x80, 0xFF};
        for (size_t p = (size_t)rndn(stride); p < c->len; p += stride)
            for (int k = 0; k < 3; k++) {
                memcpy(m, c->enc, c->len);
                m[p] ^= masks[k];
                one_case(m, c->len, c->enc, c->len, c->name, k == 0 ? "byte ^0x01 at" : k == 1 ? "byte ^0x80 at" : "byte ^0xFF at", p);
            }
        for (size_t l = (size_t)rndn(stride); l < c->len; l += stride) {
            one_case(c->enc, l, c->enc, c->len, c->name, "truncated to", l);
        }
        static const uint8_t tails[3][16] = {{0}, {0xFF}, {0x0E, 0x00, 0x00}};
        for (int t = 0; t < 3; t++) {
            memcpy(m, c->enc, c->len);
            memcpy(m + c->len, tails[t], 16);
            one_case(m, c->len + (t == 0 ? 1 : 16), c->enc, c->len, c->name, "trailing pattern", (size_t)t);
        }
        for (unsigned r = 0; r < random_per_unit; r++) {
            size_t n = c->len;
            memcpy(m, c->enc, n);
            unsigned edits = 1 + (unsigned)rndn(4);
            for (unsigned e = 0; e < edits; e++) {
                size_t p = (size_t)rndn(n ? n : 1);
                switch (rndn(5)) {
                case 0: if (n) m[p] ^= (uint8_t)(1u << rndn(8)); break;
                case 1: if (n) m[p] = (uint8_t)rnd(); break;
                case 2: if (n) { memmove(m + p, m + p + 1, n - p - 1); n--; } break;
                case 3: memmove(m + p + 1, m + p, n - p); m[p] = (uint8_t)rnd(); n++; break;
                case 4: if (n > 2) { uint16_t v = (uint16_t)(rndn(2) ? 0xFFFF : rnd()); size_t q = p < n - 2 ? p : n - 2; memcpy(m + q, &v, 2); } break;
                }
            }
            one_case(m, n, c->enc, c->len, c->name, "random edit", r);
        }
    }
    free(m);
    printf("malformed: cases=%lu refused=%lu admitted_as_other_canonical_unit=%lu (each re-encodes to its own bytes)", g_cases, g_cases - g_admitted, g_admitted);
    for (int c = 1; c <= OSC_IR_REFUSAL_COUNT; c++) printf(" %s=%lu", osc_ir_refusal_name(-c), g_class[c]);
    printf("\n");
    CHECK(g_cases - g_admitted >= want, "only %lu refused malformed cases (want %lu)", g_cases - g_admitted, want);
    CHECK(g_class[-OSC_IR_REFUSE_NONCANONICAL] == 0, "NONCANONICAL backstop was reached: a structural check is missing");
}

/* ---- 6. negative controls: the checks can fail --------------------------- */
static void test_controls(void) {
    int caught = 0;
    const Entry *c = &g_corpus[0];
    /* (a) the field comparison sees a planted change in an encoded field */
    copy_unit(g_tmp, c->u);
    g_tmp->funcs[0].insns[0].op ^= 0x40;
    char buf[128];
    caught += unit_diff(c->u, g_tmp, buf, sizeof buf) != NULL;
    /* (b) the oracle rejects a forbidden admission: bytes != the unit's encoding */
    uint8_t *m = malloc(c->len + 1);
    memcpy(m, c->enc, c->len);
    m[c->len / 2] ^= 1;
    caught += osc_ir_decode(c->enc, c->len, g_dec) == OSC_IR_OK && judge(m, c->len, OSC_IR_OK, g_dec) != NULL;
    /* (c) the oracle rejects admission of an invalid unit */
    copy_unit(g_tmp, c->u);
    g_tmp->funcs[0].nblocks = 0;
    caught += judge(c->enc, c->len, OSC_IR_OK, g_tmp) != NULL;
    /* (d) the oracle rejects a refusal that left output behind, and an unnamed class */
    caught += osc_ir_decode(c->enc, c->len, g_dec) == OSC_IR_OK && judge(m, c->len, OSC_IR_REFUSE_TRUNCATED, g_dec) != NULL;
    caught += judge(m, c->len, -99, g_tmp) != NULL;
    free(m);
    printf("negative controls: %d/5 planted defects caught by the test's own checks\n", caught);
    CHECK(caught == 5, "negative controls: only %d of 5 caught", caught);
}

int main(int argc, char **argv) {
    /* quick: CI smoke counts. full: the gate counts (10,000 valid objects, >= 100,000 malformed
     * cases, every byte of every unit up to 2,000 bytes). asan: full malformed counts, fewer valid
     * variants (each validation is about 30x slower under the sanitizers). */
    const char *mode = argc > 1 ? argv[1] : "full";
    int quick = !strcmp(mode, "quick"), asan = !strcmp(mode, "asan");
    if (!quick && !asan && strcmp(mode, "full")) { printf("usage: test_osc_ir_decode [quick|full|asan]\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_dec = calloc(1, sizeof *g_dec);
    g_tmp = calloc(1, sizeof *g_tmp);
    g_digs = calloc(DIG_CAP, 32);
    g_e1 = malloc(OSC_IR_ENCODED_MAX);
    g_e2 = malloc(OSC_IR_ENCODED_MAX);
    if (!g_dec || !g_tmp || !g_digs || !g_e1 || !g_e2) { printf("out of memory\n"); return 1; }
    load_dir("tests/compiler/progs");
    load_dir("tests/compiler/slices");
    load_dir("src/osh");
    load_dir("tests/physics0/osc");
    if (g_ncorpus == 0) { printf("FAIL: no corpus (run from the repository root)\n"); return 1; }
    printf("OSC_IR_ENCODED_MAX=%u bytes; mode=%s\n", (unsigned)OSC_IR_ENCODED_MAX, quick ? "quick (smoke counts, not gate evidence)" : mode);
    test_corpus();
    test_classes();
    test_controls();
    test_requirement_identity();
    test_valid_variants(quick ? 1000 : asan ? 2000 : 10000);
    if (quick) test_malformed(5000, 40, 100);
    else test_malformed(100000, 600, 2000);
    for (int i = 0; i < g_ncorpus; i++) { free(g_corpus[i].u); free(g_corpus[i].enc); }
    free(g_dec); free(g_tmp); free(g_digs); free(g_e1); free(g_e2);
    if (g_fail) { printf("OSC_IR_DECODE_FAIL %d\n", g_fail); return 1; }
    printf("OSC_IR_DECODE_PASS\n");
    return 0;
}
