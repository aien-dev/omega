/* test_syn_codec.c: J1 cross-check of the Omega synapse codec (src/synapse/syn_codec.osc) against the corpus
 * the Rust codec sealed (tests/synapse/j1-corpus/corpus.bin, written by synapse
 * crates/synapse-store/src/j1_corpus.rs; format in synapse docs/J1_OMEGA_CODEC.md).
 *
 *   test_syn_codec SYN_CODEC_OSC CORPUS_BIN [FUZZ_COUNT]      seed: SYN_FUZZ_SEED
 *
 * Every call runs the unit twice, in the OSC interpreter and as native AArch64, on exact-size copies of the
 * input and an exact-size workspace, and requires the same status, refusal cells and output from both.
 *   1. corpus integrity: SHA-256 trailer, header, Rust layout sizes == syn_layout.lst, name tables.
 *   2. bases (Rust -> Omega): decoding each accepted base gives its Rust logical form exactly.
 *   3. bases (Omega -> Rust): encoding each Rust logical form gives the Rust bytes exactly (exact-size output;
 *      one byte less is OUT_FULL).
 *   4. cases: every A1 mutation and hand-made refusal decodes to the Rust outcome tuple (kind, at, x, y,
 *      field); every accepted mutation re-encodes from the Omega logical form to the same bytes.
 *   5. encode refusals on hand-broken logical forms, ABI header refusals, the input LIMIT.
 *   6. fuzz: random logical-form edits (accepted encodes decode back to the same logical form, or to a
 *      decoder-only refusal) and random byte edits (no trap; accepts are canonical).
 * No literal key or nonce material: keys in the corpus are derived on the Rust side from labels. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_interp.h"
#include "osc_native.h"
#include "osc_rt.h"
#include "sha256.h"
#include "syn_layout.h"

static unsigned long n_checks, n_fail;
static unsigned long n_calls[2], n_traps, n_engine_cmp;
static unsigned long n_base_dec, n_base_enc, n_case, n_case_acc, n_case_ref, n_case_reenc;
static unsigned long n_kind[10], n_by_op[4][2], n_enc_neg, n_abi, n_fuzz_lf, n_fuzz_lf_acc, n_fuzz_lf_deconly;
static unsigned long n_fuzz_b, n_fuzz_b_acc, n_rec_kind[16], n_base_dec_ok, n_base_enc_ok, n_case_match;

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

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "oom\n"); exit(2); }
    return p;
}

/* ---- the unit ---- */
static OscUnit *U;
static OscRt *RI, *RN;
static OscCode CODE;
static OscNative NM;
static int FI = -1;

/* One call's result. */
typedef struct {
    uint64_t ret;
    int trap;
    uint64_t err, at, x, y, field;
    uint64_t *lf; /* decode: the logical form */
    size_t lfn;
    uint8_t *out; /* encode: the bytes */
    size_t outn;
} Res;

static void res_free(Res *r)
{
    free(r->lf);
    free(r->out);
    memset(r, 0, sizeof *r);
}

/* Header-only overrides for ABI tests. */
typedef struct {
    int set;
    size_t idx;
    uint64_t val;
} Poke;

/* One call of syn_codec with engine e. Decode: inp/inn are the bytes, lfcap cells of LF room.
 * Encode: lf/lfn is the logical form, outcap cells of output room. The workspace is exactly
 * W_HDR + lfcap + outcap cells (wcells_override if nonzero, for ABI tests). */
static void call1(int e, int entry, int mode, uint64_t cap, uint64_t ckseq, const uint8_t *inp, size_t inn,
                  const uint64_t *lf, size_t lfn, size_t lfcap, size_t outcap, const Poke *poke, size_t wover, Res *r)
{
    memset(r, 0, sizeof *r);
    size_t wn = SYN_W_HDR + lfcap + outcap;
    if (wover) wn = wover;
    uint64_t *w = calloc(wn, 8);
    if (!w) { fprintf(stderr, "oom\n"); exit(2); }
    uint8_t *b = NULL;
    if (inn) {
        b = xmalloc(inn);
        memcpy(b, inp, inn);
    }
    if (wn >= SYN_W_HDR) {
        w[SYN_W_MAGIC] = SYN_ABI_MAGIC;
        w[SYN_W_VERSION] = SYN_ABI_VERSION;
        w[SYN_W_CELLS] = wn;
        w[SYN_W_ENTRY] = (uint64_t)entry;
        w[SYN_W_MODE] = (uint64_t)mode;
        w[SYN_W_CAP] = cap;
        w[SYN_W_CKSEQ] = ckseq;
        w[SYN_W_LF_BASE] = SYN_W_HDR;
        w[SYN_W_LF_CAP] = lfcap;
        w[SYN_W_OUT_BASE] = SYN_W_HDR + lfcap;
        w[SYN_W_OUT_CAP] = outcap;
        if (mode == (int)SYN_MODE_ENCODE) {
            w[SYN_W_LF_N] = lfn;
            if (lfn) memcpy(w + SYN_W_HDR, lf, lfn * 8);
        }
        for (const Poke *p = poke; p && p->set; p++) w[p->idx] = p->val;
    }
    uint64_t args[4] = { (uint64_t)(uintptr_t)b, inn, (uint64_t)(uintptr_t)w, wn };
    check(osc_ir_slice_args_ok(&U->funcs[FI], args, 4) == 0, "host entry refused valid buffers");
    if (e == 0) {
        osc_rt_reset(RI);
        r->trap = osc_interp_run_prevalidated(U, FI, args, 4, RI, &r->ret);
    } else {
        osc_rt_reset(RN);
        r->trap = osc_native_call(U, FI, &NM, CODE.entry[FI], RN, args, 4, &r->ret);
    }
    n_calls[e]++;
    if (r->trap) n_traps++;
    if (wn >= SYN_W_HDR) {
        r->err = w[SYN_W_ERR];
        r->at = w[SYN_W_ERR_AT];
        r->x = w[SYN_W_ERR_X];
        r->y = w[SYN_W_ERR_Y];
        r->field = w[SYN_W_ERR_FIELD];
        if (!r->trap && r->err == 0 && w[SYN_W_LF_BASE] == SYN_W_HDR) {
            if (mode == (int)SYN_MODE_DECODE) {
                r->lfn = w[SYN_W_LF_N];
                if (r->lfn > lfcap) r->lfn = lfcap;
                r->lf = xmalloc(r->lfn * 8);
                memcpy(r->lf, w + SYN_W_HDR, r->lfn * 8);
            } else {
                r->outn = w[SYN_W_OUT];
                if (r->outn > outcap) r->outn = outcap;
                r->out = xmalloc(r->outn);
                int okb = 1;
                for (size_t i = 0; i < r->outn; i++) {
                    uint64_t v = w[SYN_W_HDR + lfcap + i];
                    okb &= v < 256;
                    r->out[i] = (uint8_t)v;
                }
                check(okb, "encode wrote a cell above 255");
            }
        }
    }
    free(w);
    free(b);
}

static int same_res(const Res *a, const Res *b)
{
    if (a->trap != b->trap || a->ret != b->ret || a->err != b->err || a->at != b->at || a->x != b->x ||
        a->y != b->y || a->field != b->field || a->lfn != b->lfn || a->outn != b->outn)
        return 0;
    if (a->lfn && memcmp(a->lf, b->lf, a->lfn * 8)) return 0;
    if (a->outn && memcmp(a->out, b->out, a->outn)) return 0;
    return 1;
}

/* Both engines; the interpreter result is kept in r. */
static void call2(int entry, int mode, uint64_t cap, uint64_t ckseq, const uint8_t *inp, size_t inn,
                  const uint64_t *lf, size_t lfn, size_t lfcap, size_t outcap, const Poke *poke, size_t wover,
                  Res *r, const char *what)
{
    Res nat;
    call1(0, entry, mode, cap, ckseq, inp, inn, lf, lfn, lfcap, outcap, poke, wover, r);
    call1(1, entry, mode, cap, ckseq, inp, inn, lf, lfn, lfcap, outcap, poke, wover, &nat);
    n_engine_cmp++;
    check(!r->trap, "%s: interp trap %d", what, r->trap);
    check(!nat.trap, "%s: native trap %d", what, nat.trap);
    check(same_res(r, &nat), "%s: interp and native differ (err %llu/%llu at %llu/%llu)", what,
          (unsigned long long)r->err, (unsigned long long)nat.err, (unsigned long long)r->at,
          (unsigned long long)nat.at);
    check(wover && wover < SYN_W_HDR ? 1 : r->ret == r->err, "%s: return %llu != W_ERR %llu", what, (unsigned long long)r->ret,
          (unsigned long long)r->err);
    res_free(&nat);
}

static size_t lfcap_for(size_t n) { return 2 * n + 16; }

static void decode(int entry, uint64_t cap, uint64_t ckseq, const uint8_t *b, size_t n, Res *r, const char *what)
{
    call2(entry, (int)SYN_MODE_DECODE, cap, ckseq, b, n, NULL, 0, lfcap_for(n), 0, NULL, 0, r, what);
}

static void encode(int entry, const uint64_t *lf, size_t lfn, size_t outcap, Res *r, const char *what)
{
    call2(entry, (int)SYN_MODE_ENCODE, 0, 0, NULL, 0, lf, lfn, lfn, outcap, NULL, 0, r, what);
}

/* ---- corpus ---- */
typedef struct {
    int entry;
    uint64_t cap, ckseq;
    char label[128];
    const uint8_t *bytes;
    size_t n;
    uint64_t *lf; /* NULL: a hand-made refused base */
    size_t lfn;
} Base;

static const uint8_t *P, *PE;
static int bad_parse;

static const uint8_t *take(size_t n)
{
    if ((size_t)(PE - P) < n) {
        bad_parse = 1;
        static const uint8_t zero[64];
        return zero;
    }
    const uint8_t *q = P;
    P += n;
    return q;
}
static uint64_t le(const uint8_t *q, int nb)
{
    uint64_t v = 0;
    for (int i = 0; i < nb; i++) v |= (uint64_t)q[i] << (8 * i);
    return v;
}
static uint64_t g8(void) { return le(take(1), 1); }
static uint64_t g16(void) { return le(take(2), 2); }
static uint64_t g32(void) { return le(take(4), 4); }
static uint64_t g64(void) { return le(take(8), 8); }

static void gname(char *dst, size_t cap)
{
    size_t n = g16();
    const uint8_t *q = take(n);
    size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(dst, q, k);
    dst[k] = 0;
}

static const char *FIELDS[15] = { "bool", "option", "mutation op", "file mode", "record kind",
                                  "envelope v2", "chain path", "obsolescence marker", "changesets",
                                  "idempotency index", "blob visibility", "projections", "canonical heads",
                                  "revision records", "revision record kind" };
static const char *CODES[2] = { "EnvelopeMalformed", "EnvelopeVersionUnsupported" };
static const char *KINDS[9] = { "Truncated", "LengthOverCap", "TrailingBytes", "UnknownTag", "InvalidUtf8",
                                "VersionUnsupported", "NotCanonical", "InvalidRepoId", "EmbeddedMalformed" };

static void check_table(const char **want, size_t n, const char *what)
{
    size_t got = g32();
    check(got == n, "%s table has %zu names, want %zu", what, got, n);
    for (size_t i = 0; i < got && i < 64; i++) {
        char nm[128];
        gname(nm, sizeof nm);
        check(i < n && strcmp(nm, want[i]) == 0, "%s table name %zu is \"%s\"", what, i + 1, nm);
    }
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = xmalloc((size_t)sz + 1);
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(b); return NULL; }
    b[sz] = 0;
    *n = (size_t)sz;
    return b;
}

static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* The record kind of a logical form (first token is the kind tag). */
static uint64_t lf_kind(const uint64_t *lf, size_t n) { return n >= 2 ? lf[1] : 0; }

static void expect_enc_ref(int entry, const uint64_t *lf, size_t lfn, size_t outcap, uint64_t want, const char *what)
{
    Res r;
    encode(entry, lf, lfn, outcap, &r, what);
    check(r.err == want, "%s: encode gave %llu, want %llu", what, (unsigned long long)r.err, (unsigned long long)want);
    n_enc_neg++;
    res_free(&r);
}

static size_t find_token(const uint64_t *lf, size_t n, uint64_t type, size_t minlen)
{
    size_t i = 0;
    while (i < n) {
        uint64_t h = lf[i], t = h & 255, len = h >> 8;
        if (t == type && len >= minlen) return i;
        i += 1 + (size_t)len;
    }
    return (size_t)-1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: test_syn_codec SYN_CODEC_OSC CORPUS_BIN [FUZZ_COUNT]\n");
        return 2;
    }
    unsigned long fuzz = argc > 3 ? strtoul(argv[3], NULL, 10) : 20000;
    size_t sl, cl;
    uint8_t *src = slurp(argv[1], &sl);
    uint8_t *corp = slurp(argv[2], &cl);
    if (!src || !corp) { fprintf(stderr, "cannot read %s or %s\n", argv[1], argv[2]); return 2; }

    /* 1. corpus integrity */
    check(cl > 32, "corpus too short");
    uint8_t dig[32];
    sha256_hash(corp, cl - 32, dig);
    check(memcmp(dig, corp + cl - 32, 32) == 0, "corpus SHA-256 trailer does not match");
    P = corp;
    PE = corp + cl - 32;
    check(memcmp(take(8), "SYNJ1COD", 8) == 0, "corpus magic");
    check(g16() == 1 && g16() == 0, "corpus version");
    uint64_t CAP = g32();
    check(CAP == 2048, "corpus cap %llu", (unsigned long long)CAP);
    check(g32() == 4, "layout size count");
    uint64_t sz[4] = { g32(), g32(), g32(), g32() };
    check(sz[0] == SYN_SZ_ATTACHMENT && sz[1] == SYN_SZ_MUTATION && sz[2] == SYN_SZ_REVIEW && sz[3] == SYN_SZ_STRING,
          "Rust layout sizes %llu %llu %llu %llu differ from syn_layout.lst", (unsigned long long)sz[0],
          (unsigned long long)sz[1], (unsigned long long)sz[2], (unsigned long long)sz[3]);
    check_table(FIELDS, 15, "field");
    check_table(CODES, 2, "code");
    check_table(KINDS, 9, "kind");
    size_t nb = g32();
    Base *B = calloc(nb ? nb : 1, sizeof *B);
    for (size_t i = 0; i < nb && !bad_parse; i++) {
        B[i].entry = (int)g8();
        B[i].cap = g64();
        B[i].ckseq = g64();
        gname(B[i].label, sizeof B[i].label);
        B[i].n = g32();
        B[i].bytes = take(B[i].n);
        size_t ln = g32();
        if (ln != 0xFFFFFFFFu) {
            B[i].lfn = ln;
            B[i].lf = xmalloc(ln * 8);
            for (size_t j = 0; j < ln; j++) B[i].lf[j] = g64();
        }
    }
    size_t ncase = g32();
    const uint8_t *cases = P;
    check(!bad_parse, "corpus parse ran past the end");
    if (bad_parse || n_fail) { printf("SYN_J1_FAIL\n"); return 1; }

    /* compile the unit */
    U = calloc(1, sizeof *U);
    RI = calloc(1, sizeof *RI);
    RN = calloc(1, sizeof *RN);
    OscDiag d;
    if (osc_compile((const char *)src, sl, U, &d, NULL) != 0) {
        fprintf(stderr, "syn_codec.osc refused: %s line %u: %s\n", osc_diag_kind_name(d.kind), d.line, d.message);
        printf("SYN_J1_FAIL\n");
        return 1;
    }
    char err[256];
    check(osc_ir_validate(U, err, sizeof err) == 0, "validate: %s", err);
    for (int f = 0; f < U->nfuncs; f++)
        if (strcmp(U->funcs[f].name, "syn_codec") == 0) FI = f;
    check(FI >= 0, "no syn_codec");
    memset(&CODE, 0, sizeof CODE);
    int cg = osc_cg_compile(U, &CODE, err, sizeof err);
    check(cg == 0, "codegen: %s", err);
    if (cg || FI < 0) { printf("SYN_J1_FAIL\n"); return 1; }
    int nmr = osc_native_map(&NM, CODE.code, CODE.len);
    check(nmr == 0, "native map %d", nmr);
    if (nmr) { printf("SYN_J1_FAIL\n"); return 1; }
    osc_rt_init(RI);
    osc_rt_init(RN);
    printf("unit: %d functions, %zu bytes of AArch64\n", U->nfuncs, (size_t)CODE.len);

    /* 2 + 3. bases, both directions */
    for (size_t i = 0; i < nb; i++) {
        Base *b = &B[i];
        if (!b->lf) continue;
        Res r;
        decode(b->entry, b->cap, b->ckseq, b->bytes, b->n, &r, b->label);
        n_base_dec++;
        check(r.err == 0, "base %s: Omega decode refused kind %llu at %llu", b->label, (unsigned long long)r.err,
              (unsigned long long)r.at);
        check(r.err != 0 || (r.lfn == b->lfn && memcmp(r.lf, b->lf, b->lfn * 8) == 0),
              "base %s: Omega logical form differs from Rust (%zu vs %zu cells)", b->label, r.lfn, b->lfn);
        n_base_dec_ok += r.err == 0 && r.lfn == b->lfn && memcmp(r.lf, b->lf, b->lfn * 8) == 0;
        if (b->entry == 1) n_rec_kind[lf_kind(b->lf, b->lfn) & 15]++;
        res_free(&r);
        encode(b->entry, b->lf, b->lfn, b->n, &r, b->label);
        n_base_enc++;
        check(r.err == 0, "base %s: Omega encode refused kind %llu at %llu", b->label, (unsigned long long)r.err,
              (unsigned long long)r.at);
        check(r.err != 0 || (r.outn == b->n && memcmp(r.out, b->bytes, b->n) == 0),
              "base %s: Omega bytes differ from Rust (%zu vs %zu)", b->label, r.outn, b->n);
        n_base_enc_ok += r.err == 0 && r.outn == b->n && memcmp(r.out, b->bytes, b->n) == 0;
        res_free(&r);
        if (b->n) {
            encode(b->entry, b->lf, b->lfn, b->n - 1, &r, b->label);
            check(r.err == SYN_R_OUT_FULL, "base %s: one byte short gave %llu, want OUT_FULL", b->label,
                  (unsigned long long)r.err);
            res_free(&r);
        }
    }

    /* 4. cases */
    P = cases;
    for (size_t c = 0; c < ncase && !bad_parse; c++) {
        size_t bi = g16();
        unsigned op = (unsigned)g8();
        size_t pos = g32();
        uint64_t declared = op == 3 ? g32() : 0;
        uint64_t k = g8(), at = 0, x = 0, y = 0, fld = 0;
        if (k) {
            at = g32();
            x = g32();
            y = g32();
            fld = g8();
        }
        if (bi >= nb || op > 3) { bad_parse = 1; break; }
        Base *b = &B[bi];
        size_t n = b->n;
        uint8_t *m = xmalloc(n);
        memcpy(m, b->bytes, n);
        if (op == 1) n = pos;
        if (op == 2) m[pos / 8] ^= (uint8_t)(1u << (pos % 8));
        if (op == 3) for (int j = 0; j < 4; j++) m[pos + j] = (uint8_t)(declared >> (8 * j));
        char what[192];
        snprintf(what, sizeof what, "case %zu (%s op %u pos %zu)", c, b->label, op, pos);
        Res r;
        decode(b->entry, b->cap, b->ckseq, m, n, &r, what);
        n_case++;
        n_kind[k <= 9 ? k : 0]++;
        n_by_op[op][k != 0]++;
        n_case_match += r.err == k && r.at == at && r.x == x && r.y == y && r.field == fld;
        check(r.err == k && r.at == at && r.x == x && r.y == y && r.field == fld,
              "%s: Omega (%llu,%llu,%llu,%llu,%llu) Rust (%llu,%llu,%llu,%llu,%llu)", what,
              (unsigned long long)r.err, (unsigned long long)r.at, (unsigned long long)r.x, (unsigned long long)r.y,
              (unsigned long long)r.field, (unsigned long long)k, (unsigned long long)at, (unsigned long long)x,
              (unsigned long long)y, (unsigned long long)fld);
        if (k == 0) {
            n_case_acc++;
            if (r.err == 0) {
                Res e2;
                encode(b->entry, r.lf, r.lfn, n, &e2, what);
                check(e2.err == 0 && e2.outn == n && memcmp(e2.out, m, n) == 0,
                      "%s: accepted mutation does not re-encode to itself", what);
                n_case_reenc += e2.err == 0 && e2.outn == n && memcmp(e2.out, m, n) == 0;
                res_free(&e2);
            }
        } else {
            n_case_ref++;
        }
        res_free(&r);
        free(m);
    }
    check(!bad_parse && P == PE, "case section does not end at the trailer");

    /* 5. encode refusals on hand-broken logical forms */
    {
        Base *rb = NULL, *cb = NULL;
        for (size_t i = 0; i < nb; i++) {
            if (!B[i].lf) continue;
            if (!rb && B[i].entry == 1 && lf_kind(B[i].lf, B[i].lfn) == 1) rb = &B[i];
            if (!cb && B[i].entry == 2 && strcmp(B[i].label, "revision checkpoint") == 0) cb = &B[i];
        }
        check(rb && cb, "corpus lacks a kind 1 record base or the revision checkpoint");
        if (rb && cb) {
            size_t n = rb->lfn;
            uint64_t *t = xmalloc((n + 1) * 8);
            memcpy(t, rb->lf, n * 8);
            t[0] = SYN_T_U64 | 256;
            expect_enc_ref(1, t, n, rb->n, SYN_R_LF_TYPE, "LF type");
            memcpy(t, rb->lf, n * 8);
            expect_enc_ref(1, t, n - 1, rb->n, SYN_R_LF_END, "LF end");
            t[n] = 0;
            expect_enc_ref(1, t, n + 1, rb->n + 8, SYN_R_LF_TRAILING, "LF trailing");
            for (uint64_t kk = 9; kk <= 12; kk++) {
                t[1] = kk; /* kinds 9..11 are reserved by open synapse PRs: refused until they land */
                expect_enc_ref(1, t, n, rb->n, SYN_R_LF_VALUE, "record kind 9..12");
            }
            t[1] = 1;
            size_t si = find_token(t, n, SYN_T_STR, 1);
            check(si != (size_t)-1, "no string token");
            if (si != (size_t)-1) {
                t[si + 1] = 0xFF;
                expect_enc_ref(1, t, n, rb->n, SYN_R_LF_UTF8, "LF utf8");
                t[si + 1] = 256;
                expect_enc_ref(1, t, n, rb->n, SYN_R_LF_VALUE, "LF byte above 255");
                t[si + 1] = rb->lf[si + 1];
            }
            size_t ri = find_token(t, n, SYN_T_REPO, 1);
            check(ri != (size_t)-1, "no repository token");
            if (ri != (size_t)-1) {
                t[ri + 1] = '/';
                expect_enc_ref(1, t, n, rb->n, SYN_R_LF_REPO, "LF repo");
                t[ri + 1] = rb->lf[ri + 1];
            }
            expect_enc_ref(1, t, n, 0, SYN_R_OUT_FULL, "no output room");
            free(t);
            /* map order: swap the two blob digests of the revision checkpoint */
            n = cb->lfn;
            t = xmalloc(n * 8);
            memcpy(t, cb->lf, n * 8);
            size_t hit = (size_t)-1;
            for (size_t i = 0; i + 71 < n; i++)
                if (t[i] == SYN_H_COUNT && t[i + 1] == 2 && t[i + 2] == SYN_D_FIX32 && t[i + 35] == (SYN_T_U64 | 256) &&
                    t[i + 37] == SYN_D_FIX32 && t[i + 70] == (SYN_T_U64 | 256))
                    hit = i;
            check(hit != (size_t)-1, "revision checkpoint blob section not found");
            if (hit != (size_t)-1) {
                for (size_t j = 0; j < 32; j++) {
                    uint64_t s = t[hit + 3 + j];
                    t[hit + 3 + j] = t[hit + 38 + j];
                    t[hit + 38 + j] = s;
                }
                expect_enc_ref(2, t, n, cb->n, SYN_R_LF_ORDER, "LF map order");
            }
            free(t);
        }
    }

    /* ABI header refusals */
    {
        static const uint8_t one[4] = { 2, 6, 0, 0 };
        struct {
            const char *what;
            Poke p[3];
            size_t wover;
            uint64_t want;
        } abi[] = {
            { "magic", { { 1, SYN_W_MAGIC, 0 } }, 0, SYN_R_ABI },
            { "version", { { 1, SYN_W_VERSION, 2 } }, 0, SYN_R_ABI },
            { "cells", { { 1, SYN_W_CELLS, 99 } }, 0, SYN_R_ABI },
            { "entry", { { 1, SYN_W_ENTRY, 3 } }, 0, SYN_R_ABI },
            { "mode", { { 1, SYN_W_MODE, 0 } }, 0, SYN_R_ABI },
            { "LF region in header", { { 1, SYN_W_LF_BASE, 4 } }, 0, SYN_R_ABI },
            { "LF region past end", { { 1, SYN_W_LF_CAP, 1000 } }, 0, SYN_R_ABI },
            { "regions overlap", { { 1, SYN_W_OUT_BASE, SYN_W_HDR }, { 1, SYN_W_OUT_CAP, 4 } }, 0, SYN_R_ABI },
        };
        for (size_t i = 0; i < sizeof abi / sizeof abi[0]; i++) {
            Res r;
            call2(1, (int)SYN_MODE_DECODE, CAP, 0, one, 4, NULL, 0, 24, 0, abi[i].p, abi[i].wover, &r, abi[i].what);
            check(r.err == abi[i].want, "ABI %s: %llu", abi[i].what, (unsigned long long)r.err);
            n_abi++;
            res_free(&r);
        }
        Res r;
        call2(1, (int)SYN_MODE_DECODE, CAP, 0, one, 4, NULL, 0, 24, 0, NULL, SYN_W_HDR - 1, &r, "short workspace");
        check(r.ret == SYN_R_ABI, "short workspace returned %llu", (unsigned long long)r.ret);
        n_abi++;
        res_free(&r);
        /* sanity: the same header untouched is valid (kind 6 cut short) */
        call2(1, (int)SYN_MODE_DECODE, CAP, 0, one, 4, NULL, 0, 24, 0, NULL, 0, &r, "valid header");
        check(r.err == SYN_R_TRUNCATED, "valid header gave %llu", (unsigned long long)r.err);
        res_free(&r);
        size_t big = SYN_INPUT_MAX + 1;
        uint8_t *bb = calloc(big, 1);
        call2(1, (int)SYN_MODE_DECODE, CAP, 0, bb, big, NULL, 0, 24, 0, NULL, 0, &r, "input limit");
        check(r.err == SYN_R_LIMIT, "input above the limit gave %llu", (unsigned long long)r.err);
        n_abi++;
        res_free(&r);
        free(bb);
    }

    /* 6. fuzz */
    uint64_t seed = 0x53594e4a31636f64ULL;
    const char *se = getenv("SYN_FUZZ_SEED");
    if (se && *se) seed = strtoull(se, NULL, 0);
    if (seed == 0) seed = 1;
    printf("fuzz seed: 0x%016llx (override with SYN_FUZZ_SEED)\n", (unsigned long long)seed);
    uint64_t rng = seed;
    size_t nl = 0;
    for (size_t i = 0; i < nb; i++) nl += B[i].lf != NULL;
    for (unsigned long f = 0; f < fuzz && nl; f++) {
        size_t pick = rng_next(&rng) % nl, i = 0;
        for (;; i++)
            if (B[i].lf && pick-- == 0) break;
        Base *b = &B[i];
        char what[192];
        if (f & 1) {
            /* logical-form edits */
            size_t n = b->lfn;
            uint64_t *t = xmalloc(n * 8);
            memcpy(t, b->lf, n * 8);
            int edits = 1 + (int)(rng_next(&rng) % 3);
            for (int e = 0; e < edits; e++) {
                size_t j = rng_next(&rng) % n;
                uint64_t v = rng_next(&rng);
                switch (v % 4) {
                case 0: t[j] = (v >> 8) % 300; break;
                case 1: t[j] ^= 1ULL << ((v >> 8) % 64); break;
                case 2: t[j] = t[j] + 1; break;
                default: t[j] = t[j] - 1; break;
                }
            }
            snprintf(what, sizeof what, "LF fuzz %lu (%s)", f, b->label);
            Res r;
            encode(b->entry, t, n, 8 * n + 64, &r, what);
            n_fuzz_lf++;
            if (r.err == 0) {
                n_fuzz_lf_acc++;
                Res r2;
                decode(b->entry, b->cap, b->ckseq, r.out, r.outn, &r2, what);
                if (r2.err == 0) {
                    check(r2.lfn == n && memcmp(r2.lf, t, n * 8) == 0, "%s: encode/decode round trip differs", what);
                } else {
                    /* the Rust encoder does not apply the decoder's caps or the revision log rules */
                    int deconly = r2.err == SYN_R_OVER_CAP || r2.err == SYN_R_EMBEDDED ||
                                  (r2.err == SYN_R_NOT_CANON && r2.field == SYN_F_REVS) ||
                                  (r2.err == SYN_R_TAG && r2.field == SYN_F_REVKIND);
                    check(deconly, "%s: encoded bytes refused with kind %llu field %llu", what,
                          (unsigned long long)r2.err, (unsigned long long)r2.field);
                    n_fuzz_lf_deconly++;
                }
                res_free(&r2);
            }
            res_free(&r);
            free(t);
        } else {
            /* byte edits */
            size_t n = b->n;
            uint8_t *m = xmalloc(n);
            memcpy(m, b->bytes, n);
            int edits = 1 + (int)(rng_next(&rng) % 6);
            for (int e = 0; e < edits && n; e++) m[rng_next(&rng) % n] = (uint8_t)rng_next(&rng);
            if (n && rng_next(&rng) % 4 == 0) n = rng_next(&rng) % n;
            snprintf(what, sizeof what, "byte fuzz %lu (%s)", f, b->label);
            Res r;
            decode(b->entry, b->cap, b->ckseq, m, n, &r, what);
            n_fuzz_b++;
            check(r.err <= 9, "%s: decode gave non-codec kind %llu", what, (unsigned long long)r.err);
            if (r.err == 0) {
                n_fuzz_b_acc++;
                Res e2;
                encode(b->entry, r.lf, r.lfn, n, &e2, what);
                check(e2.err == 0 && e2.outn == n && memcmp(e2.out, m, n) == 0, "%s: accept is not canonical", what);
                res_free(&e2);
            }
            res_free(&r);
            free(m);
        }
    }

    printf("corpus: %zu bases, %zu cases, sha256 trailer OK\n", nb, ncase);
    printf("bases: %lu of %lu decode to the Rust logical form, %lu of %lu encode to the Rust bytes (record kinds:",
           n_base_dec_ok, n_base_dec, n_base_enc_ok, n_base_enc);
    for (int k = 1; k < 16; k++)
        if (n_rec_kind[k]) printf(" %d:%lu", k, n_rec_kind[k]);
    printf(")\n");
    printf("cases: %lu of %lu give the Rust outcome (Rust accepts %lu, refuses %lu); Omega accepts re-encoded %lu\n",
           n_case_match, n_case, n_case_acc, n_case_ref, n_case_reenc);
    printf("Rust outcomes by op: as-is %lu/%lu truncate %lu/%lu flip %lu/%lu false-length %lu/%lu (accepted/refused)\n",
           n_by_op[0][0], n_by_op[0][1], n_by_op[1][0], n_by_op[1][1], n_by_op[2][0], n_by_op[2][1], n_by_op[3][0],
           n_by_op[3][1]);
    printf("Rust refusal kinds in the corpus:");
    for (int k = 1; k <= 9; k++) printf(" %s:%lu", KINDS[k - 1], n_kind[k]);
    printf("\n");
    printf("encode refusals: %lu; ABI cases: %lu\n", n_enc_neg, n_abi);
    printf("fuzz: %lu LF edits (%lu encoded, %lu of those decoder-only refusals), %lu byte edits (%lu accepted)\n",
           n_fuzz_lf, n_fuzz_lf_acc, n_fuzz_lf_deconly, n_fuzz_b, n_fuzz_b_acc);
    printf("calls: interp %lu, native %lu, interp==native compares %lu, traps %lu\n", n_calls[0], n_calls[1],
           n_engine_cmp, n_traps);
    for (int k = 1; k <= 9; k++) check(n_kind[k] > 0, "no case of refusal kind %s", KINDS[k - 1]);
    printf("checks %lu, failures %lu\n", n_checks, n_fail);
    osc_native_unmap(&NM);
    osc_cg_free(&CODE);
    for (size_t i = 0; i < nb; i++) free(B[i].lf);
    free(B);
    free(src);
    free(corp);
    int ok = n_fail == 0;
    printf(ok ? "SYN_J1_PASS\n" : "SYN_J1_FAIL\n");
    return ok ? 0 : 1;
}
