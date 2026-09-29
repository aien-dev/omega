/* POLYGLOT-0 lane B2: the B1 kernels (asm_sdot, asm_crumb) emitted as
 * machine code by Omega-owned C code, not by GNU as. spec/polyglot-0.md
 * section 4: a toolchain candidate; its gate is a byte comparison with GNU as
 * plus bit-exact results, not speed.
 *
 * Encoders used:
 *   - src/aarch64_encoder.c (Omega's original scalar encoder) for every
 *     instruction it has: CBZ, B, B.cond, SUBS/CMP imm, ADD/SUB reg, MOV reg
 *     (ORR), MOVZ, RET, LDR X unsigned offset, STR W post-index. It does not
 *     validate operands, so every call here is range-checked first.
 *   - src/polyglot/omx_encoder_ext.c for everything else (SIMD, UBFM, logical
 *     immediates, pair/post-index loads, ...). DEBT: fold into
 *     src/aarch64_encoder.c later.
 *
 * Instruction streams mirror src/polyglot/asm/omx_sdot.S and omx_crumb.S line
 * by line (same registers, same order, same label layout). Branches use
 * labels with fixups resolved after the whole stream is emitted. The sdot
 * tail mask (16 x 0x00, 16 x 0xff), which B1 keeps in .rodata, is placed
 * after the code in the same mapping and reached with the same ADRP + ADD
 * pair.
 *
 * Executable memory (W^X): the kernel is written into an anonymous mapping
 * that is PROT_READ|PROT_WRITE only, then mprotect'ed to PROT_READ|PROT_EXEC
 * (never writable and executable at the same time), then the instruction
 * cache is synchronised with __builtin___clear_cache. Built once per process,
 * by the first pack (pthread_once), never written again; run makes no syscall.
 *
 * Pack: B1's pack functions are static in omx_asm.c, so the minimal pack logic
 * (identical packed forms: int8 row-major; 2-bit crumbs in 64-weight chunks
 * of 16 bytes) is duplicated here.
 */
#define _GNU_SOURCE
#include "polyglot/omx_encoder.h"
#include "polyglot/omx_encoder_ext.h"
#include "polyglot/omx_lang.h"
#include "aarch64_encoder.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

_Static_assert(offsetof(oma_rz_plan, m) == 0, "kernel ABI: plan.m at 0");
_Static_assert(offsetof(oma_rz_plan, n) == 8, "kernel ABI: plan.n at 8");
_Static_assert(offsetof(oma_rz_plan, mem) == 16, "kernel ABI: plan.mem at 16");
_Static_assert(sizeof(size_t) == 8 && sizeof(void *) == 8, "kernel ABI: LP64");

/* ------------------------------------------------------------------ */
/* Tiny assembler: instruction list, labels, branch fixups             */
/* ------------------------------------------------------------------ */

enum { LAB_MAX = 32, FIX_MAX = 64 };
enum { FX_B, FX_BCOND, FX_CBZ, FX_CBNZ };
enum { XZR = 31, SP = 31 };

typedef struct {
    omx_enc_insn *ins;
    size_t n, cap;
    int err;
    long lab[LAB_MAX];
    struct { size_t at; int kind, label; unsigned cond, rt; } fix[FIX_MAX];
    size_t nfix;
    uint64_t base;     /* 0: object form */
    size_t data_off;   /* byte offset of the data block from the code start */
    size_t reloc_at[4];
    size_t nreloc;
} A;

static void vput(A *a, int rc, uint32_t w, int origin, const char *fmt, va_list ap) {
    if (a->err) return;
    if (rc != 0 || a->n >= a->cap) { a->err = 1; return; }
    omx_enc_insn *in = &a->ins[a->n++];
    memset(in, 0, sizeof *in);
    in->word = w;
    in->origin = (uint8_t)origin;
    vsnprintf(in->text, sizeof in->text, fmt, ap);
}

/* new encoder (ext): rc and word come from an oxe_* call */
static void X(A *a, int rc, uint32_t w, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void X(A *a, int rc, uint32_t w, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vput(a, rc, w, OMX_ENC_FROM_EXT, fmt, ap);
    va_end(ap);
}

/* Omega's original encoder: emit into a 4-byte buffer, read back LE */
static uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void O(A *a, int ok, int rc, const uint8_t *b, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void O(A *a, int ok, int rc, const uint8_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vput(a, ok ? rc : -1, ok && rc == 0 ? le32(b) : 0, OMX_ENC_FROM_OMEGA, fmt, ap);
    va_end(ap);
}
#define LEG(ok, call, ...)                                        \
    do {                                                          \
        uint8_t b_[4] = {0};                                      \
        size_t p_ = 0;                                            \
        int rc_ = (ok) ? call : -1;                               \
        O(a, (ok), rc_, b_, __VA_ARGS__);                         \
    } while (0)

static int greg(unsigned r) { return r <= 31u; }

static void label(A *a, int l) {
    if (l < 0 || l >= LAB_MAX || a->lab[l] >= 0) { a->err = 1; return; }
    a->lab[l] = (long)a->n;
}

static void add_fix(A *a, int kind, int l, unsigned cond, unsigned rt) {
    if (a->err) return;
    if (a->nfix >= FIX_MAX || l < 0 || l >= LAB_MAX || a->n == 0) { a->err = 1; return; }
    a->fix[a->nfix].at = a->n - 1;
    a->fix[a->nfix].kind = kind;
    a->fix[a->nfix].label = l;
    a->fix[a->nfix].cond = cond;
    a->fix[a->nfix].rt = rt;
    a->nfix++;
}

static const char *cond_name(unsigned c) {
    static const char *nm[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                               "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
    return nm[c & 15u];
}

/* branch placeholders (displacement 0) are re-encoded in resolve() */
static void cbz_x(A *a, unsigned rt, int l) {
    LEG(greg(rt), aarch64_emit_cbz(b_, &p_, 4, true, (uint8_t)rt, 0), "cbz x%u, L%d", rt, l);
    add_fix(a, FX_CBZ, l, 0, rt);
}
static void br(A *a, int l) {
    LEG(1, aarch64_emit_b(b_, &p_, 4, 0), "b L%d", l);
    add_fix(a, FX_B, l, 0, 0);
}
static void bcond(A *a, unsigned cond, int l) {
    LEG(cond <= 14u, aarch64_emit_b_cond(b_, &p_, 4, (Aarch64Cond)cond, 0), "b.%s L%d", cond_name(cond), l);
    add_fix(a, FX_BCOND, l, cond, 0);
}

static void subs_x_imm(A *a, unsigned rd, unsigned rn, unsigned imm) {
    char t_[16];
    snprintf(t_, sizeof t_, rd == XZR ? "cmp" : "subs x%u,", rd);
    LEG(greg(rd) && greg(rn) && imm <= 0xFFFu, aarch64_emit_subs_imm(b_, &p_, 4, true, (uint8_t)rd, (uint8_t)rn, (uint16_t)imm),
        "%s x%u, #%u", t_, rn, imm);
}
static void cmp_x_imm(A *a, unsigned rn, unsigned imm) { subs_x_imm(a, XZR, rn, imm); }
static void add_x(A *a, unsigned rd, unsigned rn, unsigned rm) {
    LEG(greg(rd) && greg(rn) && greg(rm), aarch64_emit_add_reg(b_, &p_, 4, true, (uint8_t)rd, (uint8_t)rn, (uint8_t)rm),
        "add x%u, x%u, x%u", rd, rn, rm);
}
static void sub_x(A *a, unsigned rd, unsigned rn, unsigned rm) {
    LEG(greg(rd) && greg(rn) && greg(rm), aarch64_emit_sub_reg(b_, &p_, 4, true, (uint8_t)rd, (uint8_t)rn, (uint8_t)rm),
        "sub x%u, x%u, x%u", rd, rn, rm);
}
static void mov_x(A *a, unsigned rd, unsigned rm) { /* ORR rd, xzr, rm: not for SP */
    LEG(rd <= 30u && rm <= 30u, aarch64_emit_mov_reg(b_, &p_, 4, true, (uint8_t)rd, (uint8_t)rm), "mov x%u, x%u", rd, rm);
}
static void movz_x(A *a, unsigned rd, unsigned imm) {
    LEG(rd <= 30u && imm <= 0xFFFFu, aarch64_emit_movz(b_, &p_, 4, true, (uint8_t)rd, (uint16_t)imm, 0), "mov x%u, #%u", rd, imm);
}
static void movz_w(A *a, unsigned rd, unsigned imm) {
    LEG(rd <= 30u && imm <= 0xFFFFu, aarch64_emit_movz(b_, &p_, 4, false, (uint8_t)rd, (uint16_t)imm, 0), "mov w%u, #%u", rd, imm);
}
static void ret_(A *a) { LEG(1, aarch64_emit_ret(b_, &p_, 4), "ret"); }
static void ldr_x_uoff(A *a, unsigned rt, unsigned rn, unsigned off) {
    LEG(rt <= 30u && greg(rn) && off % 8u == 0 && off / 8u <= 0xFFFu && off <= 0xFFFFu,
        aarch64_emit_ldr_uoff(b_, &p_, 4, true, (uint8_t)rt, (uint8_t)rn, (uint16_t)off), "ldr x%u, [x%u, #%u]", rt, rn, off);
}
static void str_w_post(A *a, unsigned rt, unsigned rn, int imm) {
    LEG(greg(rt) && greg(rn) && imm >= -256 && imm <= 255 && rt != rn,
        aarch64_emit_str_post(b_, &p_, 4, (uint8_t)rt, (uint8_t)rn, (int16_t)imm), "str w%u, [x%u], #%d", rt, rn, imm);
}

/* ext wrappers */
#define EXT(call, ...)                  \
    do {                                \
        uint32_t w_ = 0;                \
        int rc_ = call;                 \
        X(a, rc_, w_, __VA_ARGS__);     \
    } while (0)

static void ldp_x(A *a, unsigned t, unsigned t2, unsigned n) { EXT(oxe_ldp_x_off(&w_, t, t2, n, 0), "ldp x%u, x%u, [x%u]", t, t2, n); }
static void lsr_x(A *a, unsigned d, unsigned n, unsigned s) { EXT(oxe_lsr_imm_x(&w_, d, n, s), "lsr x%u, x%u, #%u", d, n, s); }
static void lsl_x(A *a, unsigned d, unsigned n, unsigned s) { EXT(oxe_lsl_imm_x(&w_, d, n, s), "lsl x%u, x%u, #%u", d, n, s); }
static void ubfx_x(A *a, unsigned d, unsigned n, unsigned l, unsigned w) { EXT(oxe_ubfx_x(&w_, d, n, l, w), "ubfx x%u, x%u, #%u, #%u", d, n, l, w); }
static void and_x_imm(A *a, unsigned d, unsigned n, uint64_t v) { EXT(oxe_and_imm_x(&w_, d, n, v), "and x%u, x%u, #%llu", d, n, (unsigned long long)v); }
static void ands_x_imm(A *a, unsigned d, unsigned n, uint64_t v) { EXT(oxe_ands_imm_x(&w_, d, n, v), "ands x%u, x%u, #%llu", d, n, (unsigned long long)v); }
static void cinc_x(A *a, unsigned d, unsigned n, unsigned c) { EXT(oxe_cinc_x(&w_, d, n, c), "cinc x%u, x%u, %s", d, n, cond_name(c)); }
static void add_x_lsl(A *a, unsigned d, unsigned n, unsigned m, unsigned s) { EXT(oxe_add_lsl_x(&w_, d, n, m, s), "add x%u, x%u, x%u, lsl #%u", d, n, m, s); }
static void add_sp_imm(A *a, unsigned v) { EXT(oxe_add_imm_x(&w_, SP, SP, v, 0), "add sp, sp, #%u", v); }
static void sub_x_imm(A *a, unsigned d, unsigned n, unsigned v) { EXT(oxe_sub_imm_x(&w_, d, n, v, 0), "sub x%u, x%u, #%u", d, n, v); }
static void mov_x_from_sp(A *a, unsigned d) { EXT(oxe_add_imm_x(&w_, d, SP, 0, 0), "mov x%u, sp", d); }
static void madd_w(A *a, unsigned d, unsigned n, unsigned m, unsigned r) { EXT(oxe_madd_w(&w_, d, n, m, r), "madd w%u, w%u, w%u, w%u", d, n, m, r); }
static void mov_w_m1(A *a, unsigned d) { EXT(oxe_movn_w(&w_, d, 0, 0), "mov w%u, #-1", d); }
static void ldrsb_post(A *a, unsigned t, unsigned n, int i) { EXT(oxe_ldrsb_w_post(&w_, t, n, i), "ldrsb w%u, [x%u], #%d", t, n, i); }
static void ldrb_post(A *a, unsigned t, unsigned n, int i) { EXT(oxe_ldrb_w_post(&w_, t, n, i), "ldrb w%u, [x%u], #%d", t, n, i); }
static void strb_post(A *a, unsigned t, unsigned n, int i) { EXT(oxe_strb_w_post(&w_, t, n, i), "strb w%u, [x%u], #%d", t, n, i); }
static void ldrq_post(A *a, unsigned t, unsigned n, int i) { EXT(oxe_ldr_q_post(&w_, t, n, i), "ldr q%u, [x%u], #%d", t, n, i); }
static void strq_post(A *a, unsigned t, unsigned n, int i) { EXT(oxe_str_q_post(&w_, t, n, i), "str q%u, [x%u], #%d", t, n, i); }
static void strs_post(A *a, unsigned t, unsigned n, int i) { EXT(oxe_str_s_post(&w_, t, n, i), "str s%u, [x%u], #%d", t, n, i); }
static void ldrq(A *a, unsigned t, unsigned n) { EXT(oxe_ldr_q_uoff(&w_, t, n, 0), "ldr q%u, [x%u]", t, n); }
static void stpq_sp(A *a, unsigned t, unsigned t2, int off) { EXT(oxe_stp_q_off(&w_, t, t2, SP, off), "stp q%u, q%u, [sp, #%d]", t, t2, off); }
static void ld1x4_post(A *a, unsigned t, unsigned n) { EXT(oxe_ld1_4x16b_post(&w_, t, n, 64), "ld1 {v%u.16b-v%u.16b}, [x%u], #64", t, (t + 3) & 31, n); }
static void ld1x4_sp(A *a, unsigned t) { EXT(oxe_ld1_4x16b(&w_, t, SP), "ld1 {v%u.16b-v%u.16b}, [sp]", t, (t + 3) & 31); }
static void movi0(A *a, unsigned d) { EXT(oxe_movi_16b(&w_, d, 0), "movi v%u.16b, #0", d); }
static void sdot(A *a, unsigned d, unsigned n, unsigned m) { EXT(oxe_sdot_4s(&w_, d, n, m), "sdot v%u.4s, v%u.16b, v%u.16b", d, n, m); }
static void andv(A *a, unsigned d, unsigned n, unsigned m) { EXT(oxe_and_16b(&w_, d, n, m), "and v%u.16b, v%u.16b, v%u.16b", d, n, m); }
static void add4s(A *a, unsigned d, unsigned n, unsigned m) { EXT(oxe_add_4s(&w_, d, n, m), "add v%u.4s, v%u.4s, v%u.4s", d, n, m); }
static void addp4s(A *a, unsigned d, unsigned n, unsigned m) { EXT(oxe_addp_4s(&w_, d, n, m), "addp v%u.4s, v%u.4s, v%u.4s", d, n, m); }
static void addv4s(A *a, unsigned d, unsigned n) { EXT(oxe_addv_4s(&w_, d, n), "addv s%u, v%u.4s", d, n); }
static void shl16(A *a, unsigned d, unsigned n, unsigned s) { EXT(oxe_shl_16b(&w_, d, n, s), "shl v%u.16b, v%u.16b, #%u", d, n, s); }
static void sshr16(A *a, unsigned d, unsigned n, unsigned s) { EXT(oxe_sshr_16b(&w_, d, n, s), "sshr v%u.16b, v%u.16b, #%u", d, n, s); }

/* ADRP + ADD :lo12: to the data block. Object form: fields zero. */
static void adr_data(A *a, unsigned rd) {
    size_t at = a->n;
    int64_t page = 0;
    unsigned lo12 = 0;
    if (a->base) {
        uint64_t pc = a->base + 4u * at, tgt = a->base + a->data_off;
        page = (int64_t)(tgt >> 12) - (int64_t)(pc >> 12);
        lo12 = (unsigned)(tgt & 0xFFFu);
    }
    EXT(oxe_adrp(&w_, rd, page), "adrp x%u, data", rd);
    if (!a->err) a->ins[a->n - 1].reloc = OMX_ENC_RELOC_ADRP_PAGE;
    EXT(oxe_add_imm_x(&w_, rd, rd, lo12, 0), "add x%u, x%u, :lo12:data", rd, rd);
    if (!a->err) a->ins[a->n - 1].reloc = OMX_ENC_RELOC_ADD_LO12;
}

static int resolve(A *a) {
    if (a->err) return -1;
    for (size_t i = 0; i < a->nfix; i++) {
        long tgt = a->lab[a->fix[i].label];
        if (tgt < 0) return -1; /* unresolved label */
        long d = tgt - (long)a->fix[i].at;
        uint8_t b[4];
        size_t p = 0;
        int rc;
        switch (a->fix[i].kind) {
        case FX_B:
            if (d < -(1l << 25) || d >= (1l << 25)) return -1;
            rc = aarch64_emit_b(b, &p, 4, (int32_t)d);
            break;
        case FX_BCOND:
            if (d < -(1l << 18) || d >= (1l << 18)) return -1;
            rc = aarch64_emit_b_cond(b, &p, 4, (Aarch64Cond)a->fix[i].cond, (int32_t)d);
            break;
        case FX_CBZ:
            if (d < -(1l << 18) || d >= (1l << 18)) return -1;
            rc = aarch64_emit_cbz(b, &p, 4, true, (uint8_t)a->fix[i].rt, (int32_t)d);
            break;
        default: return -1;
        }
        if (rc) return -1;
        omx_enc_insn *in = &a->ins[a->fix[i].at];
        in->word = le32(b);
        in->branch_words = (int32_t)d;
        /* replace "Ln" in the text by the resolved displacement */
        char *l = strrchr(in->text, 'L');
        if (l) snprintf(l, sizeof in->text - (size_t)(l - in->text), ".%+ld", 4 * d);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Kernel: asm_sdot (mirrors src/polyglot/asm/omx_sdot.S)             */
/* ------------------------------------------------------------------ */

static const uint8_t sdot_mask[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
enum { NE = COND_NE, LO = COND_CC };
enum { S_ERR, S_SCALAR, S_BLK4, S_ROWS1, S_ROW1, S_OK, S_B0, S_B1, S_B2, S_B3, S_B4,
       S_R0, S_R1, S_R2, S_R3, S_R4, S_C0, S_C1 };

static void emit_sdot(A *a) {
    cbz_x(a, 0, S_ERR);
    cbz_x(a, 1, S_ERR);
    cbz_x(a, 2, S_ERR);
    ldp_x(a, 3, 4, 0);
    ldr_x_uoff(a, 5, 0, 16);
    cbz_x(a, 5, S_ERR);
    cbz_x(a, 3, S_ERR);
    cbz_x(a, 4, S_ERR);
    cmp_x_imm(a, 4, 16);
    bcond(a, LO, S_SCALAR);

    lsr_x(a, 6, 4, 6);
    ubfx_x(a, 7, 4, 4, 2);
    and_x_imm(a, 8, 4, 15);
    adr_data(a, 16);
    add_x(a, 16, 16, 8);
    movz_x(a, 15, 16);
    sub_x(a, 15, 15, 8);

    label(a, S_BLK4);
    cmp_x_imm(a, 3, 4);
    bcond(a, LO, S_ROWS1);
    for (unsigned v = 24; v <= 31; v++) movi0(a, v);
    mov_x(a, 9, 1);
    mov_x(a, 10, 5);
    add_x(a, 11, 10, 4);
    add_x(a, 12, 11, 4);
    add_x(a, 13, 12, 4);
    cbz_x(a, 6, S_B1);
    mov_x(a, 14, 6);
    label(a, S_B0);
    ld1x4_post(a, 0, 9);
    ld1x4_post(a, 4, 10);
    ld1x4_post(a, 16, 11);
    sdot(a, 24, 4, 0);
    sdot(a, 28, 5, 1);
    sdot(a, 25, 16, 0);
    sdot(a, 29, 17, 1);
    ld1x4_post(a, 20, 12);
    sdot(a, 24, 6, 2);
    sdot(a, 28, 7, 3);
    sdot(a, 25, 18, 2);
    sdot(a, 29, 19, 3);
    ld1x4_post(a, 4, 13);
    sdot(a, 26, 20, 0);
    sdot(a, 30, 21, 1);
    sdot(a, 27, 4, 0);
    sdot(a, 31, 5, 1);
    sdot(a, 26, 22, 2);
    sdot(a, 30, 23, 3);
    sdot(a, 27, 6, 2);
    sdot(a, 31, 7, 3);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, S_B0);
    label(a, S_B1);
    cbz_x(a, 7, S_B3);
    mov_x(a, 14, 7);
    label(a, S_B2);
    ldrq_post(a, 0, 9, 16);
    ldrq_post(a, 4, 10, 16);
    ldrq_post(a, 5, 11, 16);
    ldrq_post(a, 6, 12, 16);
    ldrq_post(a, 7, 13, 16);
    sdot(a, 24, 4, 0);
    sdot(a, 25, 5, 0);
    sdot(a, 26, 6, 0);
    sdot(a, 27, 7, 0);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, S_B2);
    label(a, S_B3);
    cbz_x(a, 8, S_B4);
    for (unsigned r = 9; r <= 13; r++) sub_x(a, r, r, 15);
    ldrq(a, 0, 9);
    ldrq(a, 1, 16);
    andv(a, 0, 0, 1);
    ldrq(a, 4, 10);
    ldrq(a, 5, 11);
    ldrq(a, 6, 12);
    ldrq(a, 7, 13);
    sdot(a, 28, 4, 0);
    sdot(a, 29, 5, 0);
    sdot(a, 30, 6, 0);
    sdot(a, 31, 7, 0);
    label(a, S_B4);
    add4s(a, 24, 24, 28);
    add4s(a, 25, 25, 29);
    add4s(a, 26, 26, 30);
    add4s(a, 27, 27, 31);
    addp4s(a, 24, 24, 25);
    addp4s(a, 26, 26, 27);
    addp4s(a, 24, 24, 26);
    strq_post(a, 24, 2, 16);
    add_x_lsl(a, 5, 5, 4, 2);
    sub_x_imm(a, 3, 3, 4);
    br(a, S_BLK4);

    label(a, S_ROWS1);
    cbz_x(a, 3, S_OK);
    label(a, S_ROW1);
    for (unsigned v = 24; v <= 27; v++) movi0(a, v);
    mov_x(a, 9, 1);
    mov_x(a, 10, 5);
    cbz_x(a, 6, S_R1);
    mov_x(a, 14, 6);
    label(a, S_R0);
    ld1x4_post(a, 0, 9);
    ld1x4_post(a, 4, 10);
    sdot(a, 24, 4, 0);
    sdot(a, 25, 5, 1);
    sdot(a, 26, 6, 2);
    sdot(a, 27, 7, 3);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, S_R0);
    label(a, S_R1);
    cbz_x(a, 7, S_R3);
    mov_x(a, 14, 7);
    label(a, S_R2);
    ldrq_post(a, 0, 9, 16);
    ldrq_post(a, 4, 10, 16);
    sdot(a, 24, 4, 0);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, S_R2);
    label(a, S_R3);
    cbz_x(a, 8, S_R4);
    sub_x(a, 9, 9, 15);
    sub_x(a, 10, 10, 15);
    ldrq(a, 0, 9);
    ldrq(a, 1, 16);
    andv(a, 0, 0, 1);
    ldrq(a, 4, 10);
    sdot(a, 25, 4, 0);
    label(a, S_R4);
    add4s(a, 24, 24, 25);
    add4s(a, 26, 26, 27);
    add4s(a, 24, 24, 26);
    addv4s(a, 24, 24);
    strs_post(a, 24, 2, 4);
    add_x(a, 5, 5, 4);
    subs_x_imm(a, 3, 3, 1);
    bcond(a, NE, S_ROW1);
    label(a, S_OK);
    movz_w(a, 0, 0);
    ret_(a);

    label(a, S_SCALAR);
    label(a, S_C0);
    movz_w(a, 6, 0);
    mov_x(a, 9, 1);
    mov_x(a, 14, 4);
    label(a, S_C1);
    ldrsb_post(a, 10, 5, 1);
    ldrsb_post(a, 11, 9, 1);
    madd_w(a, 6, 10, 11, 6);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, S_C1);
    str_w_post(a, 6, 2, 4);
    subs_x_imm(a, 3, 3, 1);
    bcond(a, NE, S_C0);
    movz_w(a, 0, 0);
    ret_(a);

    label(a, S_ERR);
    mov_w_m1(a, 0);
    ret_(a);
}

/* ------------------------------------------------------------------ */
/* Kernel: asm_crumb (mirrors src/polyglot/asm/omx_crumb.S)           */
/* ------------------------------------------------------------------ */

enum { C_ERR, C_BLK4, C_ROWS1, C_ROW1, C_OK, C_T0, C_T1, C_B0, C_B1, C_B2, C_R0, C_R1, C_R2 };

/* CROW macro: one row, one 64-weight chunk; x in v0-v3 */
static void crow(A *a, unsigned wv, unsigned A_, unsigned B, unsigned t0, unsigned t1, unsigned t2, unsigned t3) {
    shl16(a, t0, wv, 6);
    shl16(a, t1, wv, 4);
    shl16(a, t2, wv, 2);
    sshr16(a, t3, wv, 6);
    sshr16(a, t0, t0, 6);
    sshr16(a, t1, t1, 6);
    sshr16(a, t2, t2, 6);
    sdot(a, B, t3, 3);
    sdot(a, A_, t0, 0);
    sdot(a, B, t1, 1);
    sdot(a, A_, t2, 2);
}

/* CHUNK4 macro: four rows (x10..x13), one chunk */
static void chunk4(A *a) {
    ldrq_post(a, 4, 10, 16);
    ldrq_post(a, 5, 11, 16);
    ldrq_post(a, 6, 12, 16);
    ldrq_post(a, 7, 13, 16);
    crow(a, 4, 24, 28, 16, 17, 18, 19);
    crow(a, 5, 25, 29, 20, 21, 22, 23);
    crow(a, 6, 26, 30, 16, 17, 18, 19);
    crow(a, 7, 27, 31, 20, 21, 22, 23);
}

static void emit_crumb(A *a) {
    cbz_x(a, 0, C_ERR);
    cbz_x(a, 1, C_ERR);
    cbz_x(a, 2, C_ERR);
    ldp_x(a, 3, 4, 0);
    ldr_x_uoff(a, 5, 0, 16);
    cbz_x(a, 5, C_ERR);
    cbz_x(a, 3, C_ERR);
    cbz_x(a, 4, C_ERR);

    EXT(oxe_sub_imm_x(&w_, SP, SP, 64, 0), "sub sp, sp, #64");
    movi0(a, 0);
    stpq_sp(a, 0, 0, 0);
    stpq_sp(a, 0, 0, 32);
    lsr_x(a, 6, 4, 6);
    ands_x_imm(a, 8, 4, 63);
    cinc_x(a, 7, 6, NE);
    lsl_x(a, 7, 7, 4);
    cbz_x(a, 8, C_T1);
    add_x_lsl(a, 9, 1, 6, 6);
    mov_x_from_sp(a, 10);
    mov_x(a, 14, 8);
    label(a, C_T0);
    ldrb_post(a, 11, 9, 1);
    strb_post(a, 11, 10, 1);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, C_T0);
    label(a, C_T1);

    label(a, C_BLK4);
    cmp_x_imm(a, 3, 4);
    bcond(a, LO, C_ROWS1);
    for (unsigned v = 24; v <= 31; v++) movi0(a, v);
    mov_x(a, 9, 1);
    mov_x(a, 10, 5);
    add_x(a, 11, 10, 7);
    add_x(a, 12, 11, 7);
    add_x(a, 13, 12, 7);
    cbz_x(a, 6, C_B1);
    mov_x(a, 14, 6);
    label(a, C_B0);
    ld1x4_post(a, 0, 9);
    chunk4(a);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, C_B0);
    label(a, C_B1);
    cbz_x(a, 8, C_B2);
    ld1x4_sp(a, 0);
    chunk4(a);
    label(a, C_B2);
    add4s(a, 24, 24, 28);
    add4s(a, 25, 25, 29);
    add4s(a, 26, 26, 30);
    add4s(a, 27, 27, 31);
    addp4s(a, 24, 24, 25);
    addp4s(a, 26, 26, 27);
    addp4s(a, 24, 24, 26);
    strq_post(a, 24, 2, 16);
    add_x_lsl(a, 5, 5, 7, 2);
    sub_x_imm(a, 3, 3, 4);
    br(a, C_BLK4);

    label(a, C_ROWS1);
    cbz_x(a, 3, C_OK);
    label(a, C_ROW1);
    movi0(a, 24);
    movi0(a, 28);
    mov_x(a, 9, 1);
    mov_x(a, 10, 5);
    cbz_x(a, 6, C_R1);
    mov_x(a, 14, 6);
    label(a, C_R0);
    ld1x4_post(a, 0, 9);
    ldrq_post(a, 4, 10, 16);
    crow(a, 4, 24, 28, 16, 17, 18, 19);
    subs_x_imm(a, 14, 14, 1);
    bcond(a, NE, C_R0);
    label(a, C_R1);
    cbz_x(a, 8, C_R2);
    ld1x4_sp(a, 0);
    ldrq_post(a, 4, 10, 16);
    crow(a, 4, 24, 28, 16, 17, 18, 19);
    label(a, C_R2);
    add4s(a, 24, 24, 28);
    addv4s(a, 24, 24);
    strs_post(a, 24, 2, 4);
    add_x(a, 5, 5, 7);
    subs_x_imm(a, 3, 3, 1);
    bcond(a, NE, C_ROW1);
    label(a, C_OK);
    add_sp_imm(a, 64);
    movz_w(a, 0, 0);
    ret_(a);

    label(a, C_ERR);
    mov_w_m1(a, 0);
    ret_(a);
}

/* ------------------------------------------------------------------ */

static size_t code_upper_bound(int k) { return k == OMX_ENC_SDOT ? 256u : 512u; }

int omx_encoder_build(int k, uint64_t base, omx_enc_insn *out, size_t cap, size_t *n_insn, size_t *data_off,
                      const uint8_t **data, size_t *data_len) {
    if (!out || !n_insn || (k != OMX_ENC_SDOT && k != OMX_ENC_CRUMB)) return -1;
    if (base & 0xFFFu) return -1; /* code must start page-aligned (B1: .p2align 6 at section start) */
    A a;
    memset(&a, 0, sizeof a);
    a.ins = out;
    a.cap = cap;
    a.base = base;
    for (int i = 0; i < LAB_MAX; i++) a.lab[i] = -1;
    /* data goes right after the code, 16-byte aligned; its offset depends
     * only on the (fixed) instruction count, so a first pass sizes it */
    size_t off = 0;
    for (int pass = 0; pass < 2; pass++) {
        memset(a.lab, 0xff, sizeof a.lab);
        a.n = a.nfix = 0;
        a.err = 0;
        a.data_off = off;
        if (k == OMX_ENC_SDOT) emit_sdot(&a); else emit_crumb(&a);
        if (a.err) return -1;
        off = (a.n * 4u + 15u) & ~(size_t)15u;
        if (off == a.data_off) break;
        if (pass == 1) return -1;
    }
    if (a.n > code_upper_bound(k) || resolve(&a)) return -1;
    *n_insn = a.n;
    if (data_off) *data_off = off;
    if (data) *data = k == OMX_ENC_SDOT ? sdot_mask : NULL;
    if (data_len) *data_len = k == OMX_ENC_SDOT ? sizeof sdot_mask : 0;
    return 0;
}

/* ---- executable copies (W^X) ---- */

/* One complete emission of kernel k into a fresh mapping: size with an
 * object-form build, mmap RW, final-form build at the mapping's address,
 * write the words and the data, mprotect RX (write dropped before execute is
 * added), synchronise the instruction cache. 0 on success. */
static int jit_emit(int k, void **map_out, size_t *map_len, size_t *code_bytes) {
    omx_enc_insn ins[512];
    size_t n = 0, doff = 0, dlen = 0;
    const uint8_t *data = NULL;
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    if (omx_encoder_build(k, 0, ins, 512, &n, &doff, &data, &dlen)) return -1;
    size_t total = (doff + dlen + pg - 1) / pg * pg;
    void *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return -1;
    uint8_t *mem = m;
    if (omx_encoder_build(k, (uint64_t)(uintptr_t)mem, ins, 512, &n, &doff, &data, &dlen)) {
        munmap(m, total);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        mem[4 * i + 0] = (uint8_t)ins[i].word;
        mem[4 * i + 1] = (uint8_t)(ins[i].word >> 8);
        mem[4 * i + 2] = (uint8_t)(ins[i].word >> 16);
        mem[4 * i + 3] = (uint8_t)(ins[i].word >> 24);
    }
    if (dlen) memcpy(mem + doff, data, dlen);
    if (mprotect(m, total, PROT_READ | PROT_EXEC)) {
        munmap(m, total);
        return -1;
    }
    __builtin___clear_cache((char *)mem, (char *)mem + 4 * n);
    *map_out = m;
    *map_len = total;
    *code_bytes = 4 * n;
    return 0;
}

static struct { pthread_once_t once; const void *code; size_t bytes; } g_jit[OMX_ENC_KERNELS] = {
    {PTHREAD_ONCE_INIT, NULL, 0}, {PTHREAD_ONCE_INIT, NULL, 0}};

static void jit_build(int k) {
    void *m;
    size_t len, bytes;
    if (jit_emit(k, &m, &len, &bytes)) return;
    g_jit[k].code = m;
    g_jit[k].bytes = bytes;
}
static void jit_sdot(void) { jit_build(OMX_ENC_SDOT); }
static void jit_crumb(void) { jit_build(OMX_ENC_CRUMB); }

const void *omx_encoder_code(int k, size_t *code_bytes) {
    if (k == OMX_ENC_SDOT) pthread_once(&g_jit[k].once, jit_sdot);
    else if (k == OMX_ENC_CRUMB) pthread_once(&g_jit[k].once, jit_crumb);
    else return NULL;
    if (code_bytes) *code_bytes = g_jit[k].bytes;
    return g_jit[k].code;
}

int omx_encoder_emit_ns(int k, double *ns, size_t *code_bytes) {
    if (k != OMX_ENC_SDOT && k != OMX_ENC_CRUMB) return -1;
    struct timespec t0, t1;
    void *m;
    size_t len, bytes;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
    int rc = jit_emit(k, &m, &len, &bytes);
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    if (rc) return -1;
    munmap(m, len);
    if (ns) *ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    if (code_bytes) *code_bytes = bytes;
    return 0;
}

/* ------------------------------------------------------------------ */
/* oma_rz_impl candidates                                              */
/* ------------------------------------------------------------------ */

typedef int (*run_fn)(const oma_rz_plan *, const int8_t *, int32_t *);

/* The kernel is emitted by pack (below), so run makes no syscall and touches
 * no global state beyond reading the code pointer. A plan that did not come
 * from this pack (kernel never emitted) is refused with E_ARG. */
static int run_k(int k, const oma_rz_plan *p, const int8_t *x, int32_t *y) {
    const void *c = g_jit[k].code;
    if (!c) return OMA_RZ_E_ARG;
    run_fn f;
    memcpy(&f, &c, sizeof f); /* object -> function pointer without a pedantic cast */
    return f(p, x, y);
}
static int enc_sdot_run(const oma_rz_plan *p, const int8_t *x, int32_t *y) { return run_k(OMX_ENC_SDOT, p, x, y); }
static int enc_crumb_run(const oma_rz_plan *p, const int8_t *x, int32_t *y) { return run_k(OMX_ENC_CRUMB, p, x, y); }

/* pack: same packed forms as B1 (duplicated; B1's are static). Pack also
 * makes sure the kernel for k has been emitted (once per process); if the
 * executable mapping cannot be made, pack fails with E_NOMEM (allowed for
 * pack by spec section 1) and no plan is kept. */
static int pack_begin(int k, oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    if (!p) return OMA_RZ_E_ARG;
    memset(p, 0, sizeof *p);
    int rc = oma_rz_check_shape(m, n, OMA_RZ_MAX_N);
    if (rc) return rc;
    if (!w) return OMA_RZ_E_ARG;
    rc = oma_rz_validate(w, m, n);
    if (rc) return rc;
    return omx_encoder_code(k, NULL) ? OMA_RZ_OK : OMA_RZ_E_NOMEM;
}

static int pack_sdot(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = pack_begin(OMX_ENC_SDOT, p, w, m, n);
    if (rc) return rc;
    int8_t *buf = oma_rz_alloc(m * n);
    if (!buf) return OMA_RZ_E_NOMEM;
    memcpy(buf, w, m * n);
    uint64_t nnz = 0;
    for (size_t i = 0; i < m * n; i++) nnz += (w[i] != 0);
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * n;
    p->footprint_bytes = m * n;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

static int pack_crumb(oma_rz_plan *p, const int8_t *w, size_t m, size_t n) {
    int rc = pack_begin(OMX_ENC_CRUMB, p, w, m, n);
    if (rc) return rc;
    size_t chunks = (n + 63u) / 64u, row_bytes = chunks * 16u;
    if (row_bytes != 0 && m > SIZE_MAX / row_bytes) return OMA_RZ_E_OVERFLOW;
    uint8_t *buf = oma_rz_alloc(m * row_bytes);
    if (!buf) return OMA_RZ_E_NOMEM;
    uint64_t nnz = 0;
    for (size_t i = 0; i < m; i++) {
        const int8_t *row = w + i * n;
        uint8_t *dst = buf + i * row_bytes;
        for (size_t c = 0; c < chunks; c++)
            for (unsigned j = 0; j < 16; j++) {
                unsigned byte = 0;
                for (unsigned q = 0; q < 4; q++) {
                    size_t col = c * 64u + 16u * q + j;
                    int v = col < n ? row[col] : 0;
                    nnz += (v != 0);
                    byte |= ((unsigned)v & 3u) << (2u * q);
                }
                dst[c * 16u + j] = (uint8_t)byte;
            }
    }
    p->m = m;
    p->n = n;
    p->mem = buf;
    p->weight_bytes = m * row_bytes;
    p->footprint_bytes = m * row_bytes;
    p->nnz = nnz;
    return OMA_RZ_OK;
}

const oma_rz_impl omx_rz_enc_sdot = {
    "enc_sdot", "int8 W row-major, B1 SDOT kernel emitted by Omega's own AArch64 encoder", "binary", 1, 0,
    OMA_RZ_MAX_N, pack_sdot, enc_sdot_run};
const oma_rz_impl omx_rz_enc_crumb = {
    "enc_crumb", "2-bit crumbs, B1 shift-pair -> SDOT kernel emitted by Omega's own AArch64 encoder", "crumb2", 1, 0,
    OMA_RZ_MAX_N, pack_crumb, enc_crumb_run};

const omx_candidate omx_lane_encoder[] = {
    {&omx_rz_enc_sdot, "omega-encoder", "omega aarch64_encoder + omx_encoder_ext (W^X mmap)", 0, 1,
     "src/polyglot/omx_encoder.c"},
    {&omx_rz_enc_crumb, "omega-encoder", "omega aarch64_encoder + omx_encoder_ext (W^X mmap)", 0, 1,
     "src/polyglot/omx_encoder.c"},
};
const size_t omx_lane_encoder_count = sizeof omx_lane_encoder / sizeof omx_lane_encoder[0];
