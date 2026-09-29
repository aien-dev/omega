/* POLYGLOT-0 lane B2 gate for the own-encoder candidates (enc_sdot,
 * enc_crumb): B1's kernels emitted by Omega's own AArch64 encoder.
 * spec/polyglot-0.md sections 4 and 10.3-10.4.
 *   A. Byte comparison: B1's .S assembled by GNU as, .text extracted with
 *      objcopy -O binary (paths in argv[1], argv[2]), compared instruction by
 *      instruction with the emitted stream in object form (data relocations
 *      left zero, as in the unlinked .o). Every difference is printed with
 *      both words decoded.
 *   B. Final form: the executable copy equals the emitted stream built for
 *      its address; it differs from object form only in the ADRP/ADD data
 *      relocation fields; the mapping is r-x (not writable).
 *   C. Round trip through Omega's decoder (src/aarch64_decoder.c) for every
 *      instruction produced by Omega's original encoder; ext instructions
 *      are classified (decoder unsupported / decoder aliases them).
 *   D. Fail-closed checks of the ext encoders and a full round trip of the
 *      logical-immediate encoder against an independent decoder.
 *   Correctness (copied from tests/polyglot/test_asm.c, B1):
 *   1. MA-3 shape grid (copied from tests/algebra/test_realize.c), every
 *      weight/x kind, bit-exact vs oma_rz_oracle, run twice per plan.
 *   2. >= 20,000 random cases: m 1..70, n 1..2100, random sparsity, x kinds
 *      including -128/127, x and y unaligned by offsets 1..15, canary bytes
 *      around y (no write outside y[0..m)).
 *   3. Large magnitudes up to n = max_n (= OMA_RZ_MAX_N).
 *   4. Guard pages: W (packed plan memory and pack input), x and y each
 *      placed so their last byte touches a PROT_NONE page, then so their
 *      first byte follows one.
 *   5. Error contract: bad trits, NULL, m/n = 0, n > max_n, empty plan,
 *      no partial plan kept on error.
 * Fixed seed; reproducible. */
#include "polyglot/omx_lang.h"
#include "polyglot/omx_encoder.h"
#include "polyglot/omx_encoder_ext.h"
#include "aarch64_decoder.h"
#include "aarch64_encoder.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long long g_checks, g_fail, g_cases;
static uint64_t g_rng = 0x504f4c59454e4331ULL; /* "POLYENC1" */

static uint64_t rnd(void) {
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static double rnd01(void) { return (double)(rnd() >> 11) * (1.0 / 9007199254740992.0); }

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            if (g_fail <= 20) {                                               \
                fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);          \
                fprintf(stderr, __VA_ARGS__);                                 \
                fputc('\n', stderr);                                          \
            }                                                                 \
        }                                                                     \
    } while (0)

enum { WK_RANDOM, WK_ZERO, WK_POS, WK_NEG };
enum { XK_RANDOM, XK_MIN, XK_MAX, XK_ALT, XK_ZERO };

static void fill_w(int8_t *w, size_t cnt, int kind, double sparsity) {
    for (size_t i = 0; i < cnt; i++) {
        switch (kind) {
        case WK_ZERO: w[i] = 0; break;
        case WK_POS: w[i] = 1; break;
        case WK_NEG: w[i] = -1; break;
        default: w[i] = rnd01() < sparsity ? 0 : ((rnd() & 1) ? 1 : -1); break;
        }
    }
}

static void fill_x(int8_t *x, size_t n, int kind) {
    for (size_t j = 0; j < n; j++) {
        switch (kind) {
        case XK_MIN: x[j] = -128; break;
        case XK_MAX: x[j] = 127; break;
        case XK_ALT: x[j] = (j & 1) ? 127 : -128; break;
        case XK_ZERO: x[j] = 0; break;
        default: x[j] = (int8_t)(uint8_t)rnd(); break;
        }
    }
}

/* y may be unaligned: compare through memcpy, never an int32 dereference. */
static size_t count_bad(const void *y, const int32_t *yref, size_t m, size_t *first, int32_t *got) {
    size_t bad = 0;
    for (size_t i = 0; i < m; i++) {
        int32_t v;
        memcpy(&v, (const char *)y + 4 * i, 4);
        if (v != yref[i] && bad++ == 0) { *first = i; *got = v; }
    }
    return bad;
}

/* pack w, run twice into y (any alignment), compare with yref. */
static void check_impl(const oma_rz_impl *im, const int8_t *w, size_t m, size_t n, const int8_t *x,
                       int32_t *y, const int32_t *yref, const char *tag) {
    oma_rz_plan p;
    int rc = im->pack(&p, w, m, n);
    CHECK(rc == OMA_RZ_OK, "%s %s pack m=%zu n=%zu rc %d", im->id, tag, m, n, rc);
    if (rc) { oma_rz_free(&p); return; }
    CHECK(p.weight_bytes > 0 && p.footprint_bytes >= p.weight_bytes, "%s byte accounting", im->id);
    for (int rep = 0; rep < 2; rep++) {
        memset(y, 0x5a, m * 4);
        rc = im->run(&p, x, y);
        CHECK(rc == OMA_RZ_OK, "%s %s run rc %d", im->id, tag, rc);
        size_t first = 0;
        int32_t got = 0;
        size_t bad = count_bad(y, yref, m, &first, &got);
        CHECK(bad == 0, "%s %s m=%zu n=%zu: %zu rows differ, first %zu got %d want %d", im->id, tag, m, n,
              bad, first, got, yref[first]);
    }
    oma_rz_free(&p);
    g_cases++;
}

static void run_case(size_t m, size_t n, int wk, double sp, int xk) {
    int8_t *w = malloc(m * n), *x = malloc(n);
    int32_t *yref = malloc(m * 4), *y = malloc(m * 4);
    if (!w || !x || !yref || !y) { CHECK(0, "oom"); goto out; }
    fill_w(w, m * n, wk, sp);
    fill_x(x, n, xk);
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    for (size_t c = 0; c < omx_lane_encoder_count; c++) check_impl(omx_lane_encoder[c].impl, w, m, n, x, y, yref, "grid");
out:
    free(w); free(x); free(yref); free(y);
}

/* random case with unaligned x/y and canaries around y */
static void run_random(size_t m, size_t n, int wk, double sp, int xk, size_t xo, size_t yo) {
    int8_t *w = malloc(m * n), *xb = malloc(n + 32);
    int32_t *yref = malloc(m * 4);
    unsigned char *yb = malloc(m * 4 + 64);
    if (!w || !xb || !yref || !yb) { CHECK(0, "oom"); goto out; }
    int8_t *x = xb + xo;
    fill_w(w, m * n, wk, sp);
    fill_x(x, n, xk);
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    for (size_t c = 0; c < omx_lane_encoder_count; c++) {
        const oma_rz_impl *im = omx_lane_encoder[c].impl;
        memset(yb, 0xa5, m * 4 + 64);
        unsigned char *y = yb + 16 + yo;
        check_impl(im, w, m, n, x, (int32_t *)(void *)y, yref, "random");
        size_t spill = 0;
        for (size_t i = 0; i < 16 + yo; i++) spill += yb[i] != 0xa5;
        for (size_t i = 16 + yo + m * 4; i < m * 4 + 64; i++) spill += yb[i] != 0xa5;
        CHECK(spill == 0, "%s wrote %zu bytes outside y (m=%zu n=%zu yo=%zu)", im->id, spill, m, n, yo);
    }
out:
    free(w); free(xb); free(yref); free(yb);
}

/* ---- guard pages ---- */
typedef struct { unsigned char *base; size_t len; } gmap;

static void *gplace(gmap *g, size_t bytes, int at_end) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    size_t dp = (bytes + pg - 1) / pg;
    if (dp == 0) dp = 1;
    g->len = (dp + 2) * pg;
    void *b = mmap(NULL, g->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) { g->base = NULL; return NULL; }
    g->base = b;
    if (mprotect(g->base, pg, PROT_NONE) || mprotect(g->base + (dp + 1) * pg, pg, PROT_NONE)) return NULL;
    unsigned char *data = g->base + pg;
    return at_end ? data + dp * pg - bytes : data;
}
static void gfree(gmap *g) { if (g->base) munmap(g->base, g->len); g->base = NULL; }

static void run_guard(size_t m, size_t n, int at_end) {
    gmap gw = {0}, gx = {0}, gy = {0}, gp = {0};
    int8_t *w = gplace(&gw, m * n, at_end), *x = gplace(&gx, n, at_end);
    unsigned char *y = gplace(&gy, m * 4, at_end);
    int32_t *yref = malloc(m * 4);
    if (!w || !x || !y || !yref) { CHECK(0, "guard mmap"); goto out; }
    fill_w(w, m * n, WK_RANDOM, rnd01());
    fill_x(x, n, XK_RANDOM);
    x[0] = -128;
    x[n - 1] = 127;
    CHECK(oma_rz_oracle(w, m, n, x, yref) == OMA_RZ_OK, "oracle rc");
    for (size_t c = 0; c < omx_lane_encoder_count; c++) {
        const oma_rz_impl *im = omx_lane_encoder[c].impl;
        oma_rz_plan p;
        int rc = im->pack(&p, w, m, n);
        CHECK(rc == OMA_RZ_OK, "%s guard pack rc %d", im->id, rc);
        if (rc) { oma_rz_free(&p); continue; }
        /* move the packed weights against the guard page */
        void *pw = gplace(&gp, p.weight_bytes, at_end);
        if (!pw) { CHECK(0, "guard mmap"); oma_rz_free(&p); continue; }
        memcpy(pw, p.mem, p.weight_bytes);
        void *own = p.mem;
        p.mem = pw;
        memset(y, 0x5a, m * 4);
        rc = im->run(&p, x, (int32_t *)(void *)y);
        p.mem = own;
        size_t first = 0;
        int32_t got = 0;
        size_t bad = count_bad(y, yref, m, &first, &got);
        CHECK(rc == OMA_RZ_OK && bad == 0, "%s guard(%s) m=%zu n=%zu rc %d: %zu rows differ, first %zu got %d want %d",
              im->id, at_end ? "end" : "start", m, n, rc, bad, first, got, yref[first]);
        gfree(&gp);
        oma_rz_free(&p);
        g_cases++;
    }
out:
    gfree(&gw); gfree(&gx); gfree(&gy);
    free(yref);
}

/* ---- error contract ---- */
static int plan_is_zero(const oma_rz_plan *p) {
    static const oma_rz_plan z;
    return memcmp(p, &z, sizeof z) == 0;
}

static void test_errors(void) {
    int8_t w[6] = {1, 0, -1, 2, 0, 1}, x[3] = {1, 2, 3};
    int32_t y[2];
    for (size_t c = 0; c < omx_lane_encoder_count; c++) {
        const omx_candidate *cd = &omx_lane_encoder[c];
        const oma_rz_impl *im = cd->impl;
        oma_rz_plan p;
        CHECK(strcmp(cd->language, "omega-encoder") == 0 && cd->toolchain && cd->source, "%s metadata", im->id);
        CHECK(cd->compiler_derived == 0 && cd->toolchain_only == 1, "%s derivation flags", im->id);
        CHECK(im->exact == 1 && im->max_n == OMA_RZ_MAX_N, "%s exact/max_n", im->id);
        static const int8_t bad[] = {2, -2, (int8_t)-128, 127, 3};
        for (size_t b = 0; b < sizeof bad; b++) {
            w[3] = bad[b];
            memset(&p, 0x77, sizeof p);
            CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_E_TRIT, "%s accepts weight %d", im->id, bad[b]);
            CHECK(plan_is_zero(&p), "%s keeps a partial plan after E_TRIT", im->id);
            oma_rz_free(&p);
        }
        w[3] = 1;
        memset(&p, 0x77, sizeof p);
        CHECK(im->pack(&p, w, 0, 3) == OMA_RZ_E_ARG, "%s accepts m=0", im->id);
        CHECK(plan_is_zero(&p), "%s partial plan after m=0", im->id);
        CHECK(im->pack(&p, w, 2, 0) == OMA_RZ_E_ARG, "%s accepts n=0", im->id);
        CHECK(im->pack(&p, NULL, 2, 3) == OMA_RZ_E_ARG, "%s accepts NULL w", im->id);
        CHECK(plan_is_zero(&p), "%s partial plan after NULL w", im->id);
        CHECK(im->pack(NULL, w, 2, 3) == OMA_RZ_E_ARG, "%s accepts NULL plan", im->id);
        /* shape is checked before any weight is read (w is only 6 bytes) */
        CHECK(im->pack(&p, w, 1, OMA_RZ_MAX_N + 1) == OMA_RZ_E_ARG, "%s accepts n = max_n + 1", im->id);
        CHECK(im->pack(&p, w, 1, SIZE_MAX) == OMA_RZ_E_ARG, "%s accepts n = SIZE_MAX", im->id);
        CHECK(im->pack(&p, w, SIZE_MAX / 2, 1000) == OMA_RZ_E_OVERFLOW, "%s size overflow", im->id);
        CHECK(plan_is_zero(&p), "%s partial plan after overflow", im->id);
        oma_rz_free(&p);
        /* empty plan and NULL run arguments */
        oma_rz_plan empty;
        memset(&empty, 0, sizeof empty);
        CHECK(im->run(&empty, x, y) == OMA_RZ_E_ARG, "%s runs an empty plan", im->id);
        CHECK(im->run(NULL, x, y) == OMA_RZ_E_ARG, "%s runs NULL plan", im->id);
        CHECK(im->pack(&p, w, 2, 3) == OMA_RZ_OK, "%s pack 2x3", im->id);
        CHECK(im->run(&p, NULL, y) == OMA_RZ_E_ARG, "%s runs NULL x", im->id);
        CHECK(im->run(&p, x, NULL) == OMA_RZ_E_ARG, "%s runs NULL y", im->id);
        oma_rz_plan zm = p;
        zm.m = 0;
        CHECK(im->run(&zm, x, y) == OMA_RZ_E_ARG, "%s runs m=0 plan", im->id);
        zm = p;
        zm.n = 0;
        CHECK(im->run(&zm, x, y) == OMA_RZ_E_ARG, "%s runs n=0 plan", im->id);
        CHECK(im->run(&p, x, y) == OMA_RZ_OK && y[0] == 1 * 1 + 0 * 2 + -1 * 3 && y[1] == 1 * 1 + 0 * 2 + 1 * 3,
              "%s 2x3 value", im->id);
        oma_rz_free(&p);
    }
}

/* ================================================================== */
/* A-D: own-encoder checks (lane B2)                                   */
/* ================================================================== */

static const char *const k_name[OMX_ENC_KERNELS] = {"enc_sdot", "enc_crumb"};
static unsigned long long g_identical, g_differ, g_rt_full, g_rt_partial, g_rt_ext_unsupported, g_rt_ext_alias;

static const char *dec_name(uint32_t w) {
    static const char *nm[] = {"INVALID", "ADD", "SUB", "MUL", "AND", "ORR", "EOR", "MOVZ", "RET", "B", "B.cond",
                               "CBZ", "CBNZ", "MOVK", "ADR", "LDR", "STR", "LDRB", "STRB", "SUBS"};
    DecodedInsn d;
    if (aarch64_decode_instruction(w, &d) != 0) return "not-decodable-by-omega";
    return (unsigned)d.op < sizeof nm / sizeof nm[0] ? nm[d.op] : "?";
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? 2 * cap : 4096;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    *len = n;
    return buf;
}

/* A. object-form stream vs GNU as .text */
static void byte_compare(int k, const char *path) {
    static omx_enc_insn ins[1024];
    size_t n = 0, len = 0;
    CHECK(omx_encoder_build(k, 0, ins, 1024, &n, NULL, NULL, NULL) == 0, "%s build (object form)", k_name[k]);
    unsigned char *gnu = read_file(path, &len);
    CHECK(gnu != NULL, "cannot read GNU as .text %s", path);
    if (!gnu) return;
    CHECK(len == 4 * n, "%s: GNU as .text %zu bytes, emitted %zu bytes", k_name[k], len, 4 * n);
    size_t same = 0, diff = 0, relocs = 0;
    for (size_t i = 0; i < n && 4 * i + 4 <= len; i++) {
        uint32_t g = rd32(gnu + 4 * i), e = ins[i].word;
        relocs += ins[i].reloc != OMX_ENC_RELOC_NONE;
        if (g == e) { same++; continue; }
        diff++;
        printf("  DIFF %s +0x%03zx: omega %08x [%s | omega-decoder %s]  gnu-as %08x [omega-decoder %s]  xor %08x\n",
               k_name[k], 4 * i, e, ins[i].text, dec_name(e), g, dec_name(g), e ^ g);
    }
    g_identical += same;
    g_differ += diff;
    CHECK(diff == 0, "%s: %zu instructions differ from GNU as", k_name[k], diff);
    printf("  byte-compare %s: %zu instructions, %zu identical, %zu differ (object form; %zu carry a data "
           "relocation, fields zero in both)\n", k_name[k], n, same, diff, relocs);
    free(gnu);
}

static int mapping_perms(const void *addr, char perms[5]) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    uintptr_t a = (uintptr_t)addr;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char p[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, p) == 3 && a >= lo && a < hi) {
            memcpy(perms, p, 5);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

/* B. final (executable) form */
static void final_form(int k) {
    static omx_enc_insn obj[1024], fin[1024];
    size_t n0 = 0, n1 = 0, bytes = 0, doff = 0, dlen = 0;
    const uint8_t *data = NULL;
    const unsigned char *code = omx_encoder_code(k, &bytes);
    CHECK(code != NULL, "%s: no executable copy", k_name[k]);
    if (!code) return;
    CHECK(((uintptr_t)code & 0xFFFu) == 0, "%s code not page aligned", k_name[k]);
    CHECK(omx_encoder_build(k, 0, obj, 1024, &n0, NULL, NULL, NULL) == 0, "%s object build", k_name[k]);
    CHECK(omx_encoder_build(k, (uint64_t)(uintptr_t)code, fin, 1024, &n1, &doff, &data, &dlen) == 0,
          "%s final build", k_name[k]);
    CHECK(n0 == n1 && bytes == 4 * n1, "%s sizes %zu %zu %zu", k_name[k], n0, n1, bytes);
    size_t mism = 0, reloc_diff = 0, other_diff = 0;
    for (size_t i = 0; i < n1 && i < n0; i++) {
        mism += rd32(code + 4 * i) != fin[i].word;
        if (obj[i].word != fin[i].word) {
            if (fin[i].reloc) reloc_diff++; else other_diff++;
        }
    }
    CHECK(mism == 0, "%s: executable bytes differ from emitted stream at %zu places", k_name[k], mism);
    CHECK(other_diff == 0, "%s: final form differs from object form outside relocations (%zu)", k_name[k], other_diff);
    if (dlen) CHECK(memcmp(code + doff, data, dlen) == 0, "%s data block", k_name[k]);
    char perms[5] = "????";
    CHECK(mapping_perms(code, perms) == 0 && perms[0] == 'r' && perms[1] == '-' && perms[2] == 'x',
          "%s mapping perms %s (want r-x)", k_name[k], perms);
    printf("  final %s: %zu bytes at a page-aligned %s mapping; equals emitted stream; %zu relocated "
           "instruction(s) differ from object form, 0 others\n", k_name[k], bytes, perms, reloc_diff);
}

/* C. Omega decoder round trip */
static int32_t sext(int32_t v, unsigned bits) {
    uint32_t m = 1u << (bits - 1);
    uint32_t u = (uint32_t)v & ((1u << bits) - 1u);
    return (int32_t)((u ^ m) - m);
}

static void decoder_round_trip(int k) {
    static omx_enc_insn ins[1024];
    size_t n = 0;
    CHECK(omx_encoder_build(k, 0, ins, 1024, &n, NULL, NULL, NULL) == 0, "%s build", k_name[k]);
    for (size_t i = 0; i < n; i++) {
        uint32_t w = ins[i].word;
        DecodedInsn d;
        int rc = aarch64_decode_instruction(w, &d);
        if (ins[i].origin == OMX_ENC_FROM_EXT) {
            if (rc != 0) g_rt_ext_unsupported++;
            else {
                g_rt_ext_alias++; /* only known case: ADD shifted register, decoder drops the shift */
                CHECK(d.op == DECODED_ADD && strstr(ins[i].text, ", lsl #") != NULL,
                      "%s +0x%zx %s: Omega decoder misreads ext instruction as op %d", k_name[k], 4 * i, ins[i].text, d.op);
            }
            continue;
        }
        CHECK(rc == 0, "%s +0x%zx %s: Omega decoder rejects Omega encoder output %08x", k_name[k], 4 * i,
              ins[i].text, w);
        if (rc) continue;
        uint8_t b[4];
        size_t p = 0;
        int full = 1, er = 0;
        switch (d.op) {
        case DECODED_B: er = aarch64_emit_b(b, &p, 4, sext(d.branch_imm, 26));
            CHECK(sext(d.branch_imm, 26) == ins[i].branch_words, "%s B disp", k_name[k]); break;
        case DECODED_B_COND: er = aarch64_emit_b_cond(b, &p, 4, d.cond, sext(d.branch_imm, 19));
            CHECK(sext(d.branch_imm, 19) == ins[i].branch_words, "%s B.cond disp", k_name[k]); break;
        case DECODED_CBZ: er = aarch64_emit_cbz(b, &p, 4, d.sf, d.rd, sext(d.branch_imm, 19));
            CHECK(sext(d.branch_imm, 19) == ins[i].branch_words, "%s CBZ disp", k_name[k]); break;
        case DECODED_ADD: er = aarch64_emit_add_reg(b, &p, 4, d.sf, d.rd, d.rn, d.rm); break;
        case DECODED_SUB: er = aarch64_emit_sub_reg(b, &p, 4, d.sf, d.rd, d.rn, d.rm); break;
        case DECODED_ORR: er = aarch64_emit_orr_reg(b, &p, 4, d.sf, d.rd, d.rn, d.rm); break;
        case DECODED_MOVZ: er = aarch64_emit_movz(b, &p, 4, d.sf, d.rd, d.imm16, 0); break;
        case DECODED_RET: er = aarch64_emit_ret(b, &p, 4); break;
        default: full = 0; break; /* SUBS/LDR/STR: decoder keeps no immediate */
        }
        if (full) {
            CHECK(er == 0 && rd32(b) == w, "%s +0x%zx %s: decode->encode gives %08x, want %08x", k_name[k], 4 * i,
                  ins[i].text, rd32(b), w);
            g_rt_full++;
        } else {
            CHECK(d.op == DECODED_SUBS || d.op == DECODED_LDR || d.op == DECODED_STR, "%s +0x%zx %s: decoded as op %d",
                  k_name[k], 4 * i, ins[i].text, d.op);
            CHECK(d.rd == (w & 31u) && d.rn == ((w >> 5) & 31u), "%s +0x%zx register fields", k_name[k], 4 * i);
            g_rt_partial++;
        }
    }
}

/* D. fail-closed ext encoders + logical immediates */
static uint64_t decode_bitmask(unsigned nbit, unsigned immr, unsigned imms, int *ok) {
    unsigned v = (nbit << 6) | (~imms & 0x3Fu);
    int len = -1;
    for (int b = 6; b >= 0; b--) if (v & (1u << b)) { len = b; break; }
    *ok = 0;
    if (len < 1) return 0;
    unsigned levels = (1u << len) - 1u, s = imms & levels, r = immr & levels, esize = 1u << len;
    if (s == levels) return 0;
    uint64_t emask = esize == 64 ? ~(uint64_t)0 : (((uint64_t)1 << esize) - 1);
    uint64_t welem = ((uint64_t)1 << (s + 1)) - 1;
    uint64_t elem = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; i += esize) out |= elem << i;
    *ok = 1;
    return out;
}

static void fail_closed(void) {
    uint32_t w = 0xDEADBEEF;
    unsigned rej = 0;
#define REJ(call) do { w = 0xDEADBEEF; rej++; CHECK((call) == -1 && w == 0xDEADBEEF, "accepted: %s", #call); } while (0)
    REJ(oxe_sdot_4s(&w, 32, 0, 0));
    REJ(oxe_sdot_4s(&w, 0, 0, 40));
    REJ(oxe_sdot_4s(NULL, 0, 0, 0));
    REJ(oxe_sshr_16b(&w, 0, 0, 0));
    REJ(oxe_sshr_16b(&w, 0, 0, 9));
    REJ(oxe_shl_16b(&w, 0, 0, 8));
    REJ(oxe_movi_16b(&w, 0, 256));
    REJ(oxe_ld1_4x16b_post(&w, 0, 9, 32));
    REJ(oxe_ld1_4x16b(&w, 0, 32));
    REJ(oxe_ldp_x_off(&w, 3, 4, 0, 4));
    REJ(oxe_ldp_x_off(&w, 3, 4, 0, 512));
    REJ(oxe_ldp_x_off(&w, 3, 3, 0, 0));
    REJ(oxe_ldr_q_post(&w, 0, 9, 256));
    REJ(oxe_str_q_post(&w, 0, 9, -257));
    REJ(oxe_ldr_q_uoff(&w, 0, 9, 8));
    REJ(oxe_ldr_q_uoff(&w, 0, 9, 65536));
    REJ(oxe_stp_q_off(&w, 0, 0, 31, 1024));
    REJ(oxe_stp_q_off(&w, 0, 0, 31, 8));
    REJ(oxe_ldrsb_w_post(&w, 5, 5, 1));
    REJ(oxe_and_imm_x(&w, 8, 4, 0));
    REJ(oxe_and_imm_x(&w, 8, 4, ~(uint64_t)0));
    REJ(oxe_and_imm_x(&w, 8, 4, 5));
    REJ(oxe_ands_imm_x(&w, 8, 4, 0x1234));
    REJ(oxe_ubfx_x(&w, 7, 4, 60, 5));
    REJ(oxe_ubfx_x(&w, 7, 4, 4, 0));
    REJ(oxe_lsl_imm_x(&w, 7, 7, 64));
    REJ(oxe_lsr_imm_x(&w, 7, 7, 64));
    REJ(oxe_add_imm_x(&w, 31, 31, 4096, 0));
    REJ(oxe_sub_imm_x(&w, 31, 31, 64, 2));
    REJ(oxe_add_lsl_x(&w, 5, 5, 4, 64));
    REJ(oxe_cinc_x(&w, 7, 6, 14));
    REJ(oxe_csinc_x(&w, 7, 6, 6, 16));
    REJ(oxe_movn_w(&w, 0, 0x10000, 0));
    REJ(oxe_movn_w(&w, 0, 0, 2));
    REJ(oxe_adrp(&w, 16, (int64_t)1 << 20));
    REJ(oxe_adrp(&w, 16, -((int64_t)1 << 20) - 1));
    REJ(oxe_madd_w(&w, 6, 10, 11, 32));
    REJ(oxe_addv_4s(&w, 32, 24));
    REJ(oxe_str_s_post(&w, 24, 2, 300));
#undef REJ
    /* every valid 64-bit logical immediate: decode (Arm DecodeBitMasks) then encode */
    unsigned valid = 0;
    for (unsigned nb = 0; nb < 2; nb++)
        for (unsigned immr = 0; immr < 64; immr++)
            for (unsigned imms = 0; imms < 64; imms++) {
                int ok;
                uint64_t v = decode_bitmask(nb, immr, imms, &ok);
                if (!ok) continue;
                unsigned esize = nb ? 64u : 0u;
                if (!nb) { for (unsigned b = 5; b >= 1; b--) if (!(imms & (1u << b))) { esize = 1u << b; break; } }
                if (esize == 0 || immr >= esize) continue; /* canonical immr only */
                valid++;
                unsigned en, er, es;
                CHECK(oxe_bitmask_imm64(v, &en, &er, &es) == 0 && en == nb && er == immr && es == imms,
                      "bitmask %016llx: got %u:%u:%u want %u:%u:%u", (unsigned long long)v, en, er, es, nb, immr, imms);
            }
    CHECK(valid == 5334, "logical immediates enumerated: %u (want 5334)", valid);
    printf("  fail-closed: %u invalid-operand calls checked; logical immediates round-tripped: %u\n", rej, valid);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <gnu-as sdot .text.bin> <gnu-as crumb .text.bin>\n", argv[0]);
        return 2;
    }
    printf("POLYGLOT test-encoder: byte comparison vs GNU as (B1 .text)\n");
    byte_compare(OMX_ENC_SDOT, argv[1]);
    byte_compare(OMX_ENC_CRUMB, argv[2]);
    for (int k = 0; k < OMX_ENC_KERNELS; k++) { final_form(k); decoder_round_trip(k); }
    printf("  omega-decoder round trip: %llu full (decode->re-encode identical), %llu partial (SUBS/LDR/STR: decoder keeps "
           "no immediate; op+registers checked); ext instructions: %llu not decodable by Omega decoder, %llu read by it as plain ADD "
           "(ADD ..., lsl #k: decoder drops the shift)\n", g_rt_full, g_rt_partial, g_rt_ext_unsupported, g_rt_ext_alias);
    fail_closed();
    unsigned long long enc_checks = g_checks;
    test_errors();
    unsigned long long c0 = g_checks;

    /* 1. MA-3 grid (copied from tests/algebra/test_realize.c) */
    static const size_t ns[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65, 79, 80, 81,
                                127, 128, 129, 159, 160, 161, 255, 256, 257, 1000, 1023, 1024, 1025};
    static const size_t ms[] = {1, 2, 3, 4, 5, 7, 15, 16, 17, 33};
    static const double sps[] = {0.0, 0.3, 0.6, 0.9, 1.0};
    for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
        for (size_t b = 0; b < sizeof ms / sizeof ms[0]; b++) {
            run_case(ms[b], ns[a], WK_ZERO, 0, XK_RANDOM);
            run_case(ms[b], ns[a], WK_POS, 0, XK_MIN);
            run_case(ms[b], ns[a], WK_POS, 0, XK_MAX);
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MIN);
            run_case(ms[b], ns[a], WK_NEG, 0, XK_MAX);
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a + b) % 5], XK_ALT);
            run_case(ms[b], ns[a], WK_RANDOM, sps[(a * 3 + b) % 5], XK_RANDOM);
        }
    unsigned long long grid_cases = g_cases;

    /* 2. random: 20,000 shapes, unaligned x/y (offsets 1..15) */
    const int n_random = 20000;
    for (int it = 0; it < n_random; it++) {
        size_t n = 1 + (size_t)(rnd() % 2100), m = 1 + (size_t)(rnd() % 70);
        int xk = (rnd() % 4) ? XK_RANDOM : (int)(rnd() % 5);
        size_t xo = 1 + (size_t)(rnd() % 15), yo = 1 + (size_t)(rnd() % 15);
        run_random(m, n, WK_RANDOM, rnd01(), xk, xo, yo);
    }
    unsigned long long random_cases = g_cases - grid_cases;

    /* 3. large magnitudes up to max_n */
    run_case(5, 16384, WK_NEG, 0, XK_MIN);                /* y = +2,097,152 */
    run_case(5, 16384, WK_POS, 0, XK_MIN);
    run_case(1, 100003, WK_NEG, 0, XK_MIN);               /* y = 12,800,384 */
    run_case(6, (1u << 20) + 37, WK_RANDOM, 0.3, XK_RANDOM);
    run_case(17, 4099, WK_RANDOM, 0.97, XK_RANDOM);
    run_case(1, OMA_RZ_MAX_N, WK_NEG, 0, XK_MIN);         /* y = +2,147,483,520 */
    run_case(1, OMA_RZ_MAX_N, WK_POS, 0, XK_MIN);         /* y = -2,147,483,520 */
    run_case(1, OMA_RZ_MAX_N, WK_RANDOM, 0.5, XK_ALT);
    unsigned long long large_cases = g_cases - grid_cases - random_cases;

    /* 4. guard pages on both sides of W (input and packed), x and y */
    static const size_t gms[] = {1, 2, 3, 4, 5, 7, 8, 17};
    for (int side = 0; side < 2; side++)
        for (size_t a = 0; a < sizeof ns / sizeof ns[0]; a++)
            for (size_t b = 0; b < sizeof gms / sizeof gms[0]; b++) run_guard(gms[b], ns[a], side);
    for (int it = 0; it < 200; it++)
        run_guard(1 + (size_t)(rnd() % 70), 1 + (size_t)(rnd() % 2100), it & 1);
    unsigned long long guard_cases = g_cases - grid_cases - random_cases - large_cases;

    printf("POLYGLOT test-encoder %s: %llu checks, %llu failures; %zu candidates; byte-compare %llu identical, "
           "%llu differ; encoder checks %llu; cases: grid %llu, random %llu, large %llu, guard %llu; "
           "error-contract checks %llu\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, omx_lane_encoder_count, g_identical, g_differ, enc_checks,
           grid_cases, random_cases, large_cases, guard_cases, c0 - enc_checks);
    return g_fail ? 1 : 0;
}
