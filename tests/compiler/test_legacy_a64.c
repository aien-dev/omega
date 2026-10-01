/*
 * test_legacy_a64.c -- OSC-2 item 4: the legacy AArch64 writer src/aarch64_encoder.c
 * (docs/osc/OSC-2-DESIGN.md section 4).
 * OSC-2 slice; not a general Omega compiler; no self-hosting.
 *
 * (a) differential: for every emitter, in-range fields (exhaustive where the field
 *     space is <= ~2^24, otherwise seeded random plus every range edge) give the same
 *     word as the pre-OSC-2 masking formula (copied below as the oracle, old_*), and
 *     the word decodes through src/aarch64_decoder.c to the same op and fields (the
 *     fields that decoder reports);
 * (b) refusal: every field of every emitter, given values just outside its range and
 *     extreme values, is refused: the child process dies with SIGABRT, its stderr is
 *     exactly "A64_LEGACY_FIELD_OUT_OF_RANGE <emitter> <field>=<value>\n", and the
 *     output buffer and position (shared memory) are untouched.
 * Usage: test_legacy_a64 [quick]   (quick: random samples only, for the ASan leg)
 * Final line: OSC2_LEGACY_A64_PASS or OSC2_LEGACY_A64_FAIL.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aarch64_decoder.h"
#include "aarch64_encoder.h"

static unsigned long n_checks, n_fail, n_words, n_decoded, n_refusal_cases, n_refusal_ok;
static int quick;

static void check(int cond, const char *fmt, ...) {
    n_checks++;
    if (cond) return;
    n_fail++;
    if (n_fail <= 40) {
        va_list ap;
        va_start(ap, fmt);
        fputs("FAIL: ", stdout);
        vprintf(fmt, ap);
        fputc('\n', stdout);
        va_end(ap);
    }
}

static unsigned long long rng = 0x0a64c0de5eed0004ULL;
static unsigned long long rnd(void) { /* splitmix64 */
    unsigned long long z = (rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
/* uniform in [lo, hi], with a 1/8 chance of an edge value */
static long long pick(long long lo, long long hi) {
    unsigned long long r = rnd();
    switch (r & 15) {
    case 0: return lo;
    case 1: return hi;
    default: return lo + (long long)((r >> 4) % (unsigned long long)(hi - lo + 1));
    }
}

/* ---- oracle: the pre-OSC-2 formulas, verbatim apart from the return ---- */
static uint32_t old_add_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    return ((uint32_t)sf << 31) | (0x0B << 24) | ((uint32_t)(rm & 0x1f) << 16) | ((uint32_t)(rn & 0x1f) << 5) | (rd & 0x1f);
}
static uint32_t old_sub_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    return ((uint32_t)sf << 31) | (1U << 30) | (0x0B << 24) | ((uint32_t)(rm & 0x1f) << 16) | ((uint32_t)(rn & 0x1f) << 5) |
           (rd & 0x1f);
}
static uint32_t old_mul_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    return ((uint32_t)sf << 31) | (0x1B << 24) | ((uint32_t)(rm & 0x1f) << 16) | (0x1f << 10) | ((uint32_t)(rn & 0x1f) << 5) |
           (rd & 0x1f);
}
static uint32_t old_and_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    return ((uint32_t)sf << 31) | (0x0A << 24) | ((uint32_t)(rm & 0x1f) << 16) | ((uint32_t)(rn & 0x1f) << 5) | (rd & 0x1f);
}
static uint32_t old_orr_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    return ((uint32_t)sf << 31) | (0x2A << 24) | ((uint32_t)(rm & 0x1f) << 16) | ((uint32_t)(rn & 0x1f) << 5) | (rd & 0x1f);
}
static uint32_t old_eor_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    return ((uint32_t)sf << 31) | (0x4A << 24) | ((uint32_t)(rm & 0x1f) << 16) | ((uint32_t)(rn & 0x1f) << 5) | (rd & 0x1f);
}
static uint32_t old_mov_reg(bool sf, uint8_t rd, uint8_t rm) { return old_orr_reg(sf, rd, REG_XZR, rm); }
static uint32_t old_movz(bool sf, uint8_t rd, uint16_t imm16, uint8_t shift) {
    uint32_t hw = (shift / 16) & 3;
    return ((uint32_t)sf << 31) | (0x52800000) | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1f);
}
static uint32_t old_ret(void) { return 0xD65F03C0; }
static uint32_t old_b(int32_t imm26) { return 0x14000000 | ((uint32_t)imm26 & 0x03FFFFFF); }
static uint32_t old_b_cond(Aarch64Cond cond, int32_t imm19) {
    return 0x54000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | ((uint32_t)cond & 0x0F);
}
static uint32_t old_cbz(bool sf, uint8_t rt, int32_t imm19) {
    return ((uint32_t)sf << 31) | 0x34000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
}
static uint32_t old_cbnz(bool sf, uint8_t rt, int32_t imm19) {
    return ((uint32_t)sf << 31) | 0x35000000 | (((uint32_t)imm19 & 0x7FFFF) << 5) | (rt & 0x1F);
}
static uint32_t old_movk(bool sf, uint8_t rd, uint16_t imm16, uint8_t shift) {
    uint32_t hw = (shift / 16) & 3;
    return ((uint32_t)sf << 31) | 0x72800000 | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1f);
}
static uint32_t old_adr(uint8_t rd, int32_t imm21) {
    uint32_t immlo = ((uint32_t)imm21 & 3) << 29;
    uint32_t immhi = (((uint32_t)imm21 >> 2) & 0x7FFFF) << 5;
    return 0x10000000 | immlo | immhi | (rd & 0x1F);
}
static uint32_t old_ldr_uoff(bool sf, uint8_t rt, uint8_t rn, uint16_t uoff) {
    uint32_t scale = sf ? 3 : 2;
    uint32_t imm12 = ((uint32_t)uoff >> scale) & 0xFFF;
    uint32_t base = sf ? 0xF9400000 : 0xB9400000;
    return base | (imm12 << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
}
static uint32_t old_str_uoff(bool sf, uint8_t rt, uint8_t rn, uint16_t uoff) {
    uint32_t scale = sf ? 3 : 2;
    uint32_t imm12 = ((uint32_t)uoff >> scale) & 0xFFF;
    uint32_t base = sf ? 0xF9000000 : 0xB9000000;
    return base | (imm12 << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
}
static uint32_t old_ldrb_uoff(uint8_t rt, uint8_t rn, uint16_t uoff) {
    return 0x39400000 | (((uint32_t)uoff & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
}
static uint32_t old_strb_uoff(uint8_t rt, uint8_t rn, uint16_t uoff) {
    return 0x39000000 | (((uint32_t)uoff & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
}
static uint32_t old_post(uint32_t base, uint8_t rt, uint8_t rn, int16_t simm9) {
    return base | (((uint32_t)simm9 & 0x1FF) << 12) | ((uint32_t)(rn & 0x1F) << 5) | (rt & 0x1F);
}
static uint32_t old_subs_imm(bool sf, uint8_t rd, uint8_t rn, uint16_t imm12) {
    uint32_t base = sf ? 0xF1000000 : 0x71000000;
    return base | (((uint32_t)imm12 & 0xFFF) << 10) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
}
static uint32_t old_subs_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t base = sf ? 0xEB000000 : 0x6B000000;
    return base | ((uint32_t)(rm & 0x1F) << 16) | ((uint32_t)(rn & 0x1F) << 5) | (rd & 0x1F);
}

/* ---- one emitted word ---- */
static uint8_t g_buf[8];
static size_t g_pos;
static uint32_t word_of(int rc, const char *what) {
    check(rc == 0 && g_pos == 4, "%s: emitter returned %d pos %zu", what, rc, g_pos);
    n_words++;
    return (uint32_t)g_buf[0] | (uint32_t)g_buf[1] << 8 | (uint32_t)g_buf[2] << 16 | (uint32_t)g_buf[3] << 24;
}
#define EMIT(call) (g_pos = 0, memset(g_buf, 0, sizeof g_buf), (call))
#define B g_buf, &g_pos, 4

/* expected decoder fields; -1 = not reported by src/aarch64_decoder.c */
typedef struct { DecodedOp op; int sf, rd, rn, rm, imm16, hw, br, cond; } Want;
static Want want(DecodedOp op) { Want w = {op, -1, -1, -1, -1, -1, -1, -1, -1}; return w; }

static void dcheck(const char *what, uint32_t w, Want e) {
    DecodedInsn d;
    int rc = aarch64_decode_instruction(w, &d);
    n_decoded++;
    check(rc == 0 && d.op == e.op, "%s: %08x decodes as op %d (rc %d), want %d", what, w, (int)d.op, rc, (int)e.op);
    if (rc || d.op != e.op) return;
    check(e.sf < 0 || (int)d.sf == e.sf, "%s: %08x sf %d want %d", what, w, d.sf, e.sf);
    check(e.rd < 0 || d.rd == e.rd, "%s: %08x rd/rt %u want %d", what, w, d.rd, e.rd);
    check(e.rn < 0 || d.rn == e.rn, "%s: %08x rn %u want %d", what, w, d.rn, e.rn);
    check(e.rm < 0 || d.rm == e.rm, "%s: %08x rm %u want %d", what, w, d.rm, e.rm);
    check(e.imm16 < 0 || d.imm16 == e.imm16, "%s: %08x imm16 %u want %d", what, w, d.imm16, e.imm16);
    check(e.hw < 0 || d.hw == e.hw, "%s: %08x hw %u want %d", what, w, d.hw, e.hw);
    check(e.br < 0 || d.branch_imm == e.br, "%s: %08x branch field %d want %d", what, w, d.branch_imm, e.br);
    check(e.cond < 0 || (int)d.cond == e.cond, "%s: %08x cond %d want %d", what, w, (int)d.cond, e.cond);
}

typedef int (*Emit3)(uint8_t *, size_t *, size_t, bool, uint8_t, uint8_t, uint8_t);
typedef uint32_t (*Old3)(bool, uint8_t, uint8_t, uint8_t);

static void diff_reg3(const char *name, Emit3 f, Old3 o, DecodedOp op) {
    for (int sf = 0; sf < 2; sf++)
        for (int rd = 0; rd < 32; rd++)
            for (int rn = 0; rn < 32; rn++)
                for (int rm = 0; rm < 32; rm++) { /* exhaustive: 65536 words */
                    uint32_t w = word_of(EMIT(f(B, sf, rd, rn, rm)), name);
                    check(w == o(sf, rd, rn, rm), "%s sf=%d rd=%d rn=%d rm=%d: %08x != old %08x", name, sf, rd, rn, rm, w,
                          o(sf, rd, rn, rm));
                    Want e = want(op);
                    e.sf = sf; e.rd = rd; e.rn = rn; e.rm = rm;
                    dcheck(name, w, e);
                }
}

static void diff_mov_reg(void) {
    for (int sf = 0; sf < 2; sf++)
        for (int rd = 0; rd < 32; rd++)
            for (int rm = 0; rm < 32; rm++) {
                uint32_t w = word_of(EMIT(aarch64_emit_mov_reg(B, sf, rd, rm)), "mov_reg");
                check(w == old_mov_reg(sf, rd, rm), "mov_reg sf=%d rd=%d rm=%d", sf, rd, rm);
                Want e = want(DECODED_ORR);
                e.sf = sf; e.rd = rd; e.rn = 31; e.rm = rm;
                dcheck("mov_reg", w, e);
            }
}

static void diff_movzk(int k) {
    const char *name = k ? "movk" : "movz";
    for (int sf = 0; sf < 2; sf++)
        for (int hw = 0; hw < (sf ? 4 : 2); hw++)
            for (int rd = 0; rd < 32; rd++) {
                unsigned step = quick ? 4093u : 1u; /* plain: all 65536 immediates */
                for (unsigned imm = 0; imm <= 0xFFFFu; imm += step) {
                    uint8_t shift = (uint8_t)(16 * hw);
                    uint32_t w = word_of(EMIT(k ? aarch64_emit_movk(B, sf, rd, (uint16_t)imm, shift)
                                                : aarch64_emit_movz(B, sf, rd, (uint16_t)imm, shift)), name);
                    uint32_t ow = k ? old_movk(sf, rd, (uint16_t)imm, shift) : old_movz(sf, rd, (uint16_t)imm, shift);
                    check(w == ow, "%s sf=%d rd=%d imm=%u shift=%u: %08x != old %08x", name, sf, rd, imm, shift, w, ow);
                    Want e = want(k ? DECODED_MOVK : DECODED_MOVZ);
                    e.sf = sf; e.rd = rd; e.imm16 = (int)imm; e.hw = hw;
                    dcheck(name, w, e);
                }
                if (quick) { /* the top immediate too */
                    uint32_t w = word_of(EMIT(k ? aarch64_emit_movk(B, sf, rd, 0xFFFF, (uint8_t)(16 * hw))
                                                : aarch64_emit_movz(B, sf, rd, 0xFFFF, (uint8_t)(16 * hw))), name);
                    check(w == (k ? old_movk(sf, rd, 0xFFFF, (uint8_t)(16 * hw)) : old_movz(sf, rd, 0xFFFF, (uint8_t)(16 * hw))),
                          "%s top imm", name);
                }
            }
}

static void diff_ret(void) {
    uint32_t w = word_of(EMIT(aarch64_emit_ret(B)), "ret");
    check(w == old_ret(), "ret %08x", w);
    Want e = want(DECODED_RET);
    e.rn = 30;
    dcheck("ret", w, e);
}

static void diff_b(void) {
    long n = quick ? 20000 : 400000;
    for (long i = 0; i < n; i++) {
        int32_t imm = (int32_t)pick(-(1LL << 25), (1LL << 25) - 1);
        uint32_t w = word_of(EMIT(aarch64_emit_b(B, imm)), "b");
        check(w == old_b(imm), "b imm26=%d: %08x != old %08x", imm, w, old_b(imm));
        Want e = want(DECODED_B);
        e.br = (int)((uint32_t)imm & 0x03FFFFFF);
        dcheck("b", w, e);
    }
}

static void diff_b_cond(void) {
    for (int c = 0; c < 16; c++) {
        int32_t lo = -(1 << 18), hi = (1 << 18) - 1, step = quick ? 997 : 1; /* plain: exhaustive, 8M words */
        for (int32_t imm = lo; imm <= hi; imm += step) {
            uint32_t w = word_of(EMIT(aarch64_emit_b_cond(B, (Aarch64Cond)c, imm)), "b_cond");
            check(w == old_b_cond((Aarch64Cond)c, imm), "b_cond c=%d imm19=%d", c, imm);
            Want e = want(DECODED_B_COND);
            e.cond = c; e.br = (int)((uint32_t)imm & 0x7FFFF);
            dcheck("b_cond", w, e);
        }
        uint32_t w = word_of(EMIT(aarch64_emit_b_cond(B, (Aarch64Cond)c, hi)), "b_cond");
        check(w == old_b_cond((Aarch64Cond)c, hi), "b_cond c=%d hi", c);
    }
}

static void diff_cbz(int nz) {
    const char *name = nz ? "cbnz" : "cbz";
    long n = quick ? 20000 : 400000;
    for (long i = 0; i < n; i++) {
        int sf = (int)(rnd() & 1);
        uint8_t rt = (uint8_t)pick(0, 31);
        int32_t imm = (int32_t)pick(-(1LL << 18), (1LL << 18) - 1);
        uint32_t w = word_of(EMIT(nz ? aarch64_emit_cbnz(B, sf, rt, imm) : aarch64_emit_cbz(B, sf, rt, imm)), name);
        uint32_t ow = nz ? old_cbnz(sf, rt, imm) : old_cbz(sf, rt, imm);
        check(w == ow, "%s sf=%d rt=%u imm19=%d: %08x != old %08x", name, sf, rt, imm, w, ow);
        Want e = want(nz ? DECODED_CBNZ : DECODED_CBZ);
        e.sf = sf; e.rd = rt; e.br = (int)((uint32_t)imm & 0x7FFFF);
        dcheck(name, w, e);
    }
}

static void diff_adr(void) {
    long n = quick ? 20000 : 400000;
    for (long i = 0; i < n; i++) {
        uint8_t rd = (uint8_t)pick(0, 31);
        int32_t imm = (int32_t)pick(-(1LL << 20), (1LL << 20) - 1);
        uint32_t w = word_of(EMIT(aarch64_emit_adr(B, rd, imm)), "adr");
        check(w == old_adr(rd, imm), "adr rd=%u imm21=%d: %08x != old %08x", rd, imm, w, old_adr(rd, imm));
        /* architectural field check (the decoder reports only rd for ADR) */
        int32_t back = (int32_t)(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2));
        back = (back ^ (1 << 20)) - (1 << 20);
        check(back == imm, "adr imm21 %d reassembles as %d", imm, back);
        Want e = want(DECODED_ADR);
        e.rd = rd;
        dcheck("adr", w, e);
    }
}

static void diff_uoff(int st) {
    const char *name = st ? "str_uoff" : "ldr_uoff";
    for (int sf = 0; sf < 2; sf++) {
        unsigned sz = sf ? 8u : 4u;
        for (int rt = 0; rt < 32; rt++)
            for (int rn = 0; rn < 32; rn++)
                for (unsigned k = 0; k <= 0xFFFu; k += quick ? 409u : 1u) { /* plain: exhaustive, 8M words */
                    uint16_t uoff = (uint16_t)(k * sz);
                    uint32_t w = word_of(EMIT(st ? aarch64_emit_str_uoff(B, sf, rt, rn, uoff)
                                                 : aarch64_emit_ldr_uoff(B, sf, rt, rn, uoff)), name);
                    uint32_t ow = st ? old_str_uoff(sf, rt, rn, uoff) : old_ldr_uoff(sf, rt, rn, uoff);
                    check(w == ow, "%s sf=%d rt=%d rn=%d uoff=%u: %08x != old %08x", name, sf, rt, rn, uoff, w, ow);
                    check(((w >> 10) & 0xFFF) == k, "%s imm12 field", name);
                    Want e = want(st ? DECODED_STR : DECODED_LDR);
                    e.sf = sf; e.rd = rt; e.rn = rn;
                    dcheck(name, w, e);
                }
    }
}

static void diff_byte(int st) {
    const char *name = st ? "strb_uoff" : "ldrb_uoff";
    for (int rt = 0; rt < 32; rt++)
        for (int rn = 0; rn < 32; rn++)
            for (unsigned uoff = 0; uoff <= 0xFFFu; uoff += quick ? 409u : 1u) { /* plain: exhaustive, 4M words */
                uint32_t w = word_of(EMIT(st ? aarch64_emit_strb_uoff(B, rt, rn, (uint16_t)uoff)
                                             : aarch64_emit_ldrb_uoff(B, rt, rn, (uint16_t)uoff)), name);
                uint32_t ow = st ? old_strb_uoff(rt, rn, (uint16_t)uoff) : old_ldrb_uoff(rt, rn, (uint16_t)uoff);
                check(w == ow, "%s rt=%d rn=%d uoff=%u: %08x != old %08x", name, rt, rn, uoff, w, ow);
                Want e = want(st ? DECODED_STRB : DECODED_LDRB);
                e.rd = rt; e.rn = rn;
                dcheck(name, w, e);
            }
}

typedef int (*EmitPost)(uint8_t *, size_t *, size_t, uint8_t, uint8_t, int16_t);
static void diff_post(const char *name, EmitPost f, uint32_t base, DecodedOp op, int sf) {
    for (int rt = 0; rt < 32; rt++)
        for (int rn = 0; rn < 32; rn++)
            for (int s = -256; s <= 255; s += quick ? 37 : 1) { /* plain: exhaustive, 512K words */
                uint32_t w = word_of(EMIT(f(B, rt, rn, (int16_t)s)), name);
                uint32_t ow = old_post(base, rt, rn, (int16_t)s);
                check(w == ow, "%s rt=%d rn=%d simm9=%d: %08x != old %08x", name, rt, rn, s, w, ow);
                Want e = want(op);
                e.sf = sf; e.rd = rt; e.rn = rn;
                dcheck(name, w, e);
            }
}

static void diff_subs_imm(void) {
    for (int sf = 0; sf < 2; sf++)
        for (int rd = 0; rd < 32; rd++)
            for (int rn = 0; rn < 32; rn++)
                for (unsigned imm = 0; imm <= 0xFFFu; imm += quick ? 409u : 1u) { /* plain: exhaustive, 8M words */
                    uint32_t w = word_of(EMIT(aarch64_emit_subs_imm(B, sf, rd, rn, (uint16_t)imm)), "subs_imm");
                    check(w == old_subs_imm(sf, rd, rn, (uint16_t)imm), "subs_imm sf=%d rd=%d rn=%d imm=%u", sf, rd, rn, imm);
                    Want e = want(DECODED_SUBS);
                    e.sf = sf; e.rd = rd; e.rn = rn;
                    dcheck("subs_imm", w, e);
                }
}

/* ---- refusal path ---- */
typedef struct {
    int id;             /* which call */
    long long a, b, c, d; /* arguments */
    const char *expect; /* exact stderr */
} Refusal;

static void call(const Refusal *r, uint8_t *buf, size_t *pos) {
    switch (r->id) {
#define R3(n, fn) case n: fn(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint8_t)r->c, (uint8_t)r->d); break;
    R3(0, aarch64_emit_add_reg) R3(1, aarch64_emit_sub_reg) R3(2, aarch64_emit_mul_reg) R3(3, aarch64_emit_and_reg)
    R3(4, aarch64_emit_orr_reg) R3(5, aarch64_emit_eor_reg) R3(6, aarch64_emit_subs_reg)
#undef R3
    case 7: aarch64_emit_mov_reg(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint8_t)r->c); break;
    case 8: aarch64_emit_movz(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint16_t)r->c, (uint8_t)r->d); break;
    case 9: aarch64_emit_movk(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint16_t)r->c, (uint8_t)r->d); break;
    case 10: aarch64_emit_b(buf, pos, 8, (int32_t)r->a); break;
    case 11: aarch64_emit_b_cond(buf, pos, 8, (Aarch64Cond)r->a, (int32_t)r->b); break;
    case 12: aarch64_emit_cbz(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (int32_t)r->c); break;
    case 13: aarch64_emit_cbnz(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (int32_t)r->c); break;
    case 14: aarch64_emit_adr(buf, pos, 8, (uint8_t)r->a, (int32_t)r->b); break;
    case 15: aarch64_emit_ldr_uoff(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint8_t)r->c, (uint16_t)r->d); break;
    case 16: aarch64_emit_str_uoff(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint8_t)r->c, (uint16_t)r->d); break;
    case 17: aarch64_emit_ldrb_uoff(buf, pos, 8, (uint8_t)r->a, (uint8_t)r->b, (uint16_t)r->c); break;
    case 18: aarch64_emit_strb_uoff(buf, pos, 8, (uint8_t)r->a, (uint8_t)r->b, (uint16_t)r->c); break;
    case 19: aarch64_emit_ldr_post(buf, pos, 8, (uint8_t)r->a, (uint8_t)r->b, (int16_t)r->c); break;
    case 20: aarch64_emit_str_post(buf, pos, 8, (uint8_t)r->a, (uint8_t)r->b, (int16_t)r->c); break;
    case 21: aarch64_emit_ldr_x_post(buf, pos, 8, (uint8_t)r->a, (uint8_t)r->b, (int16_t)r->c); break;
    case 22: aarch64_emit_str_x_post(buf, pos, 8, (uint8_t)r->a, (uint8_t)r->b, (int16_t)r->c); break;
    case 23: aarch64_emit_subs_imm(buf, pos, 8, (bool)r->a, (uint8_t)r->b, (uint8_t)r->c, (uint16_t)r->d); break;
    }
}

static const Refusal refusals[] = {
    {0, 1, 32, 0, 0, "aarch64_emit_add_reg rd=32"},     {0, 1, 0, 255, 0, "aarch64_emit_add_reg rn=255"},
    {0, 0, 0, 0, 33, "aarch64_emit_add_reg rm=33"},     {1, 1, 40, 0, 0, "aarch64_emit_sub_reg rd=40"},
    {1, 1, 0, 32, 0, "aarch64_emit_sub_reg rn=32"},     {1, 1, 0, 0, 32, "aarch64_emit_sub_reg rm=32"},
    {2, 1, 32, 0, 0, "aarch64_emit_mul_reg rd=32"},     {2, 1, 0, 32, 0, "aarch64_emit_mul_reg rn=32"},
    {2, 1, 0, 0, 128, "aarch64_emit_mul_reg rm=128"},   {3, 1, 32, 0, 0, "aarch64_emit_and_reg rd=32"},
    {3, 1, 0, 32, 0, "aarch64_emit_and_reg rn=32"},     {3, 1, 0, 0, 32, "aarch64_emit_and_reg rm=32"},
    {4, 1, 32, 0, 0, "aarch64_emit_orr_reg rd=32"},     {4, 1, 0, 32, 0, "aarch64_emit_orr_reg rn=32"},
    {4, 1, 0, 0, 32, "aarch64_emit_orr_reg rm=32"},     {5, 1, 32, 0, 0, "aarch64_emit_eor_reg rd=32"},
    {5, 1, 0, 32, 0, "aarch64_emit_eor_reg rn=32"},     {5, 1, 0, 0, 32, "aarch64_emit_eor_reg rm=32"},
    {6, 1, 32, 0, 0, "aarch64_emit_subs_reg rd=32"},    {6, 1, 0, 32, 0, "aarch64_emit_subs_reg rn=32"},
    {6, 1, 0, 0, 32, "aarch64_emit_subs_reg rm=32"},    {7, 1, 32, 0, 0, "aarch64_emit_mov_reg rd=32"},
    {7, 1, 0, 32, 0, "aarch64_emit_mov_reg rm=32"},
    {8, 1, 32, 0, 0, "aarch64_emit_movz rd=32"},        {8, 1, 0, 0, 8, "aarch64_emit_movz shift=8"},
    {8, 1, 0, 0, 17, "aarch64_emit_movz shift=17"},     {8, 1, 0, 0, 64, "aarch64_emit_movz shift=64"},
    {8, 0, 0, 0, 32, "aarch64_emit_movz shift=32"},     {8, 0, 0, 0, 48, "aarch64_emit_movz shift=48"},
    {9, 1, 32, 0, 16, "aarch64_emit_movk rd=32"},       {9, 1, 0, 0, 1, "aarch64_emit_movk shift=1"},
    {9, 1, 0, 0, 80, "aarch64_emit_movk shift=80"},     {9, 0, 0, 0, 32, "aarch64_emit_movk shift=32"},
    {10, 1LL << 25, 0, 0, 0, "aarch64_emit_b imm26=33554432"},
    {10, -(1LL << 25) - 1, 0, 0, 0, "aarch64_emit_b imm26=-33554433"},
    {10, 2147483647LL, 0, 0, 0, "aarch64_emit_b imm26=2147483647"},
    {10, -2147483647LL - 1, 0, 0, 0, "aarch64_emit_b imm26=-2147483648"},
    {11, 16, 0, 0, 0, "aarch64_emit_b_cond cond=16"},   {11, -1, 0, 0, 0, "aarch64_emit_b_cond cond=-1"},
    {11, 0, 1 << 18, 0, 0, "aarch64_emit_b_cond imm19=262144"},
    {11, 1, -(1 << 18) - 1, 0, 0, "aarch64_emit_b_cond imm19=-262145"},
    {12, 1, 32, 0, 0, "aarch64_emit_cbz rt=32"},        {12, 1, 0, 1 << 18, 0, "aarch64_emit_cbz imm19=262144"},
    {12, 0, 0, -(1 << 18) - 1, 0, "aarch64_emit_cbz imm19=-262145"},
    {13, 1, 32, 0, 0, "aarch64_emit_cbnz rt=32"},       {13, 1, 0, 1 << 18, 0, "aarch64_emit_cbnz imm19=262144"},
    {13, 0, 0, -(1 << 18) - 1, 0, "aarch64_emit_cbnz imm19=-262145"},
    {14, 32, 0, 0, 0, "aarch64_emit_adr rd=32"},        {14, 0, 1 << 20, 0, 0, "aarch64_emit_adr imm21=1048576"},
    {14, 0, -(1 << 20) - 1, 0, 0, "aarch64_emit_adr imm21=-1048577"},
    {15, 1, 32, 0, 0, "aarch64_emit_ldr_uoff rt=32"},   {15, 1, 0, 32, 0, "aarch64_emit_ldr_uoff rn=32"},
    {15, 1, 0, 0, 4, "aarch64_emit_ldr_uoff uoff=4"},   {15, 1, 0, 0, 32768, "aarch64_emit_ldr_uoff uoff=32768"},
    {15, 0, 0, 0, 2, "aarch64_emit_ldr_uoff uoff=2"},   {15, 0, 0, 0, 16384, "aarch64_emit_ldr_uoff uoff=16384"},
    {15, 1, 0, 0, 65535, "aarch64_emit_ldr_uoff uoff=65535"},
    {16, 1, 32, 0, 0, "aarch64_emit_str_uoff rt=32"},   {16, 1, 0, 32, 0, "aarch64_emit_str_uoff rn=32"},
    {16, 1, 0, 0, 12, "aarch64_emit_str_uoff uoff=12"}, {16, 1, 0, 0, 32768, "aarch64_emit_str_uoff uoff=32768"},
    {16, 0, 0, 0, 6, "aarch64_emit_str_uoff uoff=6"},   {16, 0, 0, 0, 16384, "aarch64_emit_str_uoff uoff=16384"},
    {17, 32, 0, 0, 0, "aarch64_emit_ldrb_uoff rt=32"},  {17, 0, 32, 0, 0, "aarch64_emit_ldrb_uoff rn=32"},
    {17, 0, 0, 4096, 0, "aarch64_emit_ldrb_uoff uoff=4096"},
    {17, 0, 0, 65535, 0, "aarch64_emit_ldrb_uoff uoff=65535"},
    {18, 32, 0, 0, 0, "aarch64_emit_strb_uoff rt=32"},  {18, 0, 32, 0, 0, "aarch64_emit_strb_uoff rn=32"},
    {18, 0, 0, 4096, 0, "aarch64_emit_strb_uoff uoff=4096"},
    {19, 32, 0, 0, 0, "aarch64_emit_ldr_post rt=32"},   {19, 0, 32, 0, 0, "aarch64_emit_ldr_post rn=32"},
    {19, 0, 0, 256, 0, "aarch64_emit_ldr_post simm9=256"},
    {19, 0, 0, -257, 0, "aarch64_emit_ldr_post simm9=-257"},
    {20, 32, 0, 0, 0, "aarch64_emit_str_post rt=32"},   {20, 0, 32, 0, 0, "aarch64_emit_str_post rn=32"},
    {20, 0, 0, 256, 0, "aarch64_emit_str_post simm9=256"},
    {20, 0, 0, -32768, 0, "aarch64_emit_str_post simm9=-32768"},
    {21, 32, 0, 0, 0, "aarch64_emit_ldr_x_post rt=32"}, {21, 0, 32, 0, 0, "aarch64_emit_ldr_x_post rn=32"},
    {21, 0, 0, 512, 0, "aarch64_emit_ldr_x_post simm9=512"},
    {21, 0, 0, -257, 0, "aarch64_emit_ldr_x_post simm9=-257"},
    {22, 32, 0, 0, 0, "aarch64_emit_str_x_post rt=32"}, {22, 0, 32, 0, 0, "aarch64_emit_str_x_post rn=32"},
    {22, 0, 0, 32767, 0, "aarch64_emit_str_x_post simm9=32767"},
    {22, 0, 0, -257, 0, "aarch64_emit_str_x_post simm9=-257"},
    {23, 1, 32, 0, 0, "aarch64_emit_subs_imm rd=32"},   {23, 1, 0, 32, 0, "aarch64_emit_subs_imm rn=32"},
    {23, 1, 0, 0, 4096, "aarch64_emit_subs_imm imm12=4096"},
    {23, 0, 0, 0, 65535, "aarch64_emit_subs_imm imm12=65535"},
};

typedef struct { uint8_t buf[8]; size_t pos; } Shared;

static void refusal_case(const Refusal *r, Shared *sh) {
    char want[160], got[256];
    snprintf(want, sizeof want, "A64_LEGACY_FIELD_OUT_OF_RANGE %s\n", r->expect);
    memset(sh->buf, 0xA5, sizeof sh->buf);
    sh->pos = 0;
    int fd[2];
    if (pipe(fd) != 0) { check(0, "pipe"); return; }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], 2);
        struct rlimit nocore = {0, 0};
        setrlimit(RLIMIT_CORE, &nocore);   /* the expected abort must not write a core dump */
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        call(r, sh->buf, &sh->pos);
        _exit(0); /* reached only if the emitter did not refuse */
    }
    close(fd[1]);
    size_t n = 0;
    ssize_t k;
    while (n < sizeof got - 1 && (k = read(fd[0], got + n, sizeof got - 1 - n)) > 0) n += (size_t)k;
    got[n] = '\0';
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    n_refusal_cases++;
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "refusal %s: child not aborted (status %d)", r->expect, st);
    check(strcmp(got, want) == 0, "refusal %s: stderr \"%s\"", r->expect, got);
    int untouched = sh->pos == 0;
    for (size_t i = 0; i < sizeof sh->buf; i++) untouched &= sh->buf[i] == 0xA5;
    check(untouched, "refusal %s: buffer or position written", r->expect);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strcmp(got, want) == 0 && untouched) n_refusal_ok++;
}

int main(int argc, char **argv) {
    quick = argc > 1 && strcmp(argv[1], "quick") == 0;
    diff_reg3("add_reg", aarch64_emit_add_reg, old_add_reg, DECODED_ADD);
    diff_reg3("sub_reg", aarch64_emit_sub_reg, old_sub_reg, DECODED_SUB);
    diff_reg3("mul_reg", aarch64_emit_mul_reg, old_mul_reg, DECODED_MUL);
    diff_reg3("and_reg", aarch64_emit_and_reg, old_and_reg, DECODED_AND);
    diff_reg3("orr_reg", aarch64_emit_orr_reg, old_orr_reg, DECODED_ORR);
    diff_reg3("eor_reg", aarch64_emit_eor_reg, old_eor_reg, DECODED_EOR);
    diff_reg3("subs_reg", aarch64_emit_subs_reg, old_subs_reg, DECODED_SUBS);
    diff_mov_reg();
    diff_movzk(0);
    diff_movzk(1);
    diff_ret();
    diff_b();
    diff_b_cond();
    diff_cbz(0);
    diff_cbz(1);
    diff_adr();
    diff_uoff(0);
    diff_uoff(1);
    diff_byte(0);
    diff_byte(1);
    diff_post("ldr_post", aarch64_emit_ldr_post, 0xB8400400, DECODED_LDR, 0);
    diff_post("str_post", aarch64_emit_str_post, 0xB8000400, DECODED_STR, 0);
    diff_post("ldr_x_post", aarch64_emit_ldr_x_post, 0xF8400400, DECODED_LDR, 1);
    diff_post("str_x_post", aarch64_emit_str_x_post, 0xF8000400, DECODED_STR, 1);
    diff_subs_imm();
    unsigned long diff_fail = n_fail;

    /* buffer-full is unchanged: -1, nothing written */
    {
        uint8_t b[4] = {1, 2, 3, 4};
        size_t p = 1;
        check(aarch64_emit_ret(b, &p, 4) == -1 && p == 1 && b[1] == 2, "buffer-full path changed");
    }

    Shared *sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) { puts("FAIL: mmap"); puts("OSC2_LEGACY_A64_FAIL"); return 1; }
    size_t nref = sizeof refusals / sizeof refusals[0];
    for (size_t i = 0; i < nref; i++) refusal_case(&refusals[i], sh);
    munmap(sh, sizeof(Shared));

    printf("legacy a64 differential: emitters=25 mode=%s words=%lu decoded=%lu mismatches=%lu\n", quick ? "quick" : "full",
           n_words, n_decoded, diff_fail);
    printf("legacy a64 refusal: cases=%lu named_abort_and_no_write=%lu\n", n_refusal_cases, n_refusal_ok);
    printf("checks=%lu failures=%lu\n", n_checks, n_fail);
    puts(n_fail ? "OSC2_LEGACY_A64_FAIL" : "OSC2_LEGACY_A64_PASS");
    return n_fail ? 1 : 0;
}
