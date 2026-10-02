/* E1 row 2: general global load/store kernels for the GB10. See omega_numeric_ldst_gb10.h. */
#include "omega_numeric_ldst_gb10.h"
#include "omega_numeric.h"
#include "omega_numeric_divsqrt_gb10.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_qmd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OMEGA_NUMERIC_CPU_ONLY
#include "omega_blackwell_submit.h"
#include "m16_native.h"
#endif

#define NINSN 24u
#define PRO_INSNS 17u
#define I_LD_DST 12u      /* the load's destination register: aligned for 128-bit */
#define I_IMAD_IN 12u     /* prologue instruction indices patched */
#define I_LDG_A 13u
#define I_IMAD_OUT 16u
#define I_IADD3 17u
#define I_STG 18u
#define I_EXIT 19u
#define I_BRA 20u
#define I_NOP 21u

static const size_t LD_BYTES[OMEGA_LD_COUNT] = { 1, 1, 2, 2, 4, 8, 16 };
static const size_t ST_BYTES[OMEGA_ST_COUNT] = { 1, 2, 4, 8, 16 };
static const unsigned LD_SIZE[OMEGA_LD_COUNT] = { 0, 1, 2, 3, 4, 5, 6 };
static const unsigned ST_SIZE[OMEGA_ST_COUNT] = { 0, 2, 4, 5, 6 };
static const char *const LD_SUFFIX[OMEGA_LD_COUNT] = { ".E.U8", ".E.S8", ".E.U16", ".E.S16", ".E", ".E.64", ".E.128" };
static const char *const ST_SUFFIX[OMEGA_ST_COUNT] = { ".E.U8", ".E.U16", ".E", ".E.64", ".E.128" };
static const char *const LD_NAME[OMEGA_LD_COUNT] = { "U8", "S8", "U16", "S16", "B32", "B64", "B128" };
static const char *const ST_NAME[OMEGA_ST_COUNT] = { "B8", "B16", "B32", "B64", "B128" };

/* Texts of prologue instructions 0..16 (same as src/omega_numeric_divsqrt_gb10.c PROLOGUE_TEXT). */
static const char *const PROLOGUE_TEXT[PRO_INSNS] = {
    "LDC R1, c[0x0][0x37c]", "S2R R9, SR_CTAID.X", "LDCU UR4, c[0x0][0x360]", "S2R R0, SR_TID.X",
    "LDCU UR5, c[0x0][0x398]", "IMAD R9, R9, UR4, R0", "ISETP.GE.U32.AND P0, PT, R9, UR5, PT", "@P0 EXIT",
    "LDC.64 R2, c[0x0][0x380]", "LDCU.64 UR4, c[0x0][0x358]", "LDC.64 R4, c[0x0][0x388]",
    "LDC.64 R6, c[0x0][0x390]", "IMAD.WIDE.U32 R2, R9, 0x4, R2", "LDG.E R2, desc[UR4][R2.64]",
    "IMAD.WIDE.U32 R4, R9, 0x4, R4", "LDG.E R5, desc[UR4][R4.64]", "IMAD.WIDE.U32 R6, R9, 0x4, R6",
};

size_t omega_ld_bytes(OmegaLdKind k) { return k < OMEGA_LD_COUNT ? LD_BYTES[k] : 0; }
size_t omega_st_bytes(OmegaStKind k) { return k < OMEGA_ST_COUNT ? ST_BYTES[k] : 0; }

const char *omega_ldst_describe(const OmegaLdstSpec *s, char *buf, size_t len) {
    snprintf(buf, len, "%s@%u%+d -> %s@%u%+d", s->ld < OMEGA_LD_COUNT ? LD_NAME[s->ld] : "?", s->in_stride, (int)s->in_off,
             s->st < OMEGA_ST_COUNT ? ST_NAME[s->st] : "?", s->out_stride, (int)s->out_off);
    return buf;
}

static int refuse(char *why, size_t n, const char *fmt, ...) {
    if (why && n) { va_list ap; va_start(ap, fmt); vsnprintf(why, n, fmt, ap); va_end(ap); }
    return -1;
}

int omega_ldst_check_spec(const OmegaLdstSpec *s, size_t count, char *why, size_t why_len) {
    if (!s) return refuse(why, why_len, "no spec");
    if (s->ld >= OMEGA_LD_COUNT) return refuse(why, why_len, "unknown load kind %u", s->ld);
    if (s->st >= OMEGA_ST_COUNT) return refuse(why, why_len, "unknown store kind %u", s->st);
    size_t lb = LD_BYTES[s->ld], sb = ST_BYTES[s->st], fill = lb < 4 ? 4 : lb;
    if (sb > fill) return refuse(why, why_len, "store of %zu bytes is wider than the %zu bytes the load fills", sb, fill);
    if (s->in_stride < lb || s->in_stride % lb) return refuse(why, why_len, "input stride %u is not a multiple of %zu >= %zu", s->in_stride, lb, lb);
    if (s->out_stride < sb || s->out_stride % sb) return refuse(why, why_len, "output stride %u is not a multiple of %zu >= %zu", s->out_stride, sb, sb);
    if (s->in_stride >= (1u << 24) || s->out_stride >= (1u << 24)) return refuse(why, why_len, "stride must be below 2^24");
    if (s->in_off < -(1 << 23) || s->in_off >= (1 << 23) || s->out_off < -(1 << 23) || s->out_off >= (1 << 23))
        return refuse(why, why_len, "offset outside the signed 24-bit field");
    if (s->in_off % (int32_t)lb) return refuse(why, why_len, "input offset %d is not a multiple of %zu", (int)s->in_off, lb);
    if (s->out_off % (int32_t)sb) return refuse(why, why_len, "output offset %d is not a multiple of %zu", (int)s->out_off, sb);
    if (count == 0 || count > OMEGA_DS_MAX_BATCH) return refuse(why, why_len, "count %zu outside 1..%u", count, OMEGA_DS_MAX_BATCH);
    return 0;
}

int omega_ldst_extent(const OmegaLdstSpec *s, size_t count, int64_t *in_lo, int64_t *in_hi, int64_t *out_lo, int64_t *out_hi) {
    if (omega_ldst_check_spec(s, count, NULL, 0) != 0) return -1;
    int64_t n1 = (int64_t)count - 1;
    *in_lo = s->in_off;
    *in_hi = (int64_t)s->in_off + n1 * (int64_t)s->in_stride + (int64_t)LD_BYTES[s->ld];
    *out_lo = s->out_off;
    *out_hi = (int64_t)s->out_off + n1 * (int64_t)s->out_stride + (int64_t)ST_BYTES[s->st];
    return 0;
}

/* ---- kernel -------------------------------------------------------------------- */

static void put_words(uint8_t *p, const uint32_t w[4]) {
    for (int i = 0; i < 4; i++) { p[4 * i] = (uint8_t)w[i]; p[4 * i + 1] = (uint8_t)(w[i] >> 8); p[4 * i + 2] = (uint8_t)(w[i] >> 16); p[4 * i + 3] = (uint8_t)(w[i] >> 24); }
}

int omega_ldst_build_kernel(const OmegaLdstSpec *s, uint8_t *code, size_t max, size_t *out_len) {
    uint8_t va[OMEGA_BW_VECADD_CODE_SIZE];
    size_t vl = 0;
    if (!code || max < OMEGA_LDST_CODE_BYTES || omega_ldst_check_spec(s, 1, NULL, 0) != 0) return -1;
    if (omega_blackwell_encode_vecadd(va, sizeof(va), &vl) != 0 || vl != OMEGA_BW_VECADD_CODE_SIZE) return -1;
    memset(code, 0, OMEGA_LDST_CODE_BYTES);
    for (size_t k = 0; k < I_EXIT + 2; k++) memcpy(code + 16 * k, va + 16 * k, 16);   /* 0..16 prologue; 17,18 overwritten below */
    memcpy(code + 16 * I_EXIT, va + 16 * 19, 16);                                      /* EXIT */
    memcpy(code + 16 * I_BRA, va + 16 * 20, 16);                                       /* BRA to self */
    for (size_t k = I_NOP; k < NINSN; k++) memcpy(code + 16 * k, va + 16 * 21, 16);    /* NOP */
    uint32_t w[4];
    /* IMAD.WIDE.U32 R2, R9, in_stride, R2 */
    for (int i = 0; i < 4; i++) { const uint8_t *p = va + 16 * I_IMAD_IN + 4 * i; w[i] = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
    w[1] = s->in_stride;
    put_words(code + 16 * I_IMAD_IN, w);
    /* LDG.<kind> R12, desc[UR4][R2.64+off] */
    w[0] = 0x7981u | (I_LD_DST << 16) | (2u << 24);
    w[1] = 4u | (((uint32_t)s->in_off & 0xffffffu) << 8);
    w[2] = 0x0c1e1100u | (LD_SIZE[s->ld] << 9);
    w[3] = 0x002f2200u;
    put_words(code + 16 * I_LDG_A, w);
    /* IMAD.WIDE.U32 R6, R9, out_stride, R6 */
    for (int i = 0; i < 4; i++) { const uint8_t *p = va + 16 * I_IMAD_OUT + 4 * i; w[i] = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
    w[1] = s->out_stride;
    put_words(code + 16 * I_IMAD_OUT, w);
    /* IADD3 R9, PT, PT, R12, R5, RZ : waits on both load barriers (control word of vecadd's IADD3) */
    w[0] = 0x7210u | (9u << 16) | (I_LD_DST << 24);
    w[1] = 5u;
    w[2] = 0x07ffe0ffu;
    w[3] = 0x010fca00u;
    put_words(code + 16 * I_IADD3, w);
    /* STG.<kind> desc[UR4][R6.64+off], R12 */
    w[0] = 0x7986u | (6u << 24);
    w[1] = I_LD_DST | (((uint32_t)s->out_off & 0xffffffu) << 8);
    w[2] = 0x0c101104u | (ST_SIZE[s->st] << 9);
    w[3] = 0x000fe200u;
    put_words(code + 16 * I_STG, w);
    if (out_len) *out_len = OMEGA_LDST_CODE_BYTES;
    return 0;
}

static int signed_off(char *dst, size_t n, int32_t off) {
    if (off == 0) { dst[0] = '\0'; return 0; }
    return snprintf(dst, n, "+%s0x%x", off < 0 ? "-" : "", (unsigned)(off < 0 ? -(int64_t)off : off));
}

int omega_ldst_listing(const OmegaLdstSpec *s, char *buf, size_t len) {
    if (!buf || omega_ldst_check_spec(s, 1, NULL, 0) != 0) return -1;
    size_t pos = 0;
    char io[24], oo[24];
    signed_off(io, sizeof io, s->in_off);
    signed_off(oo, sizeof oo, s->out_off);
#define LINE(...) do { int w_ = snprintf(buf + pos, pos < len ? len - pos : 0, __VA_ARGS__); \
                       if (w_ < 0 || pos + (size_t)w_ >= len) { return -1; } \
                       pos += (size_t)w_; } while (0)
    for (size_t k = 0; k < PRO_INSNS; k++) {
        if (k == I_IMAD_IN) LINE("%04zx IMAD.WIDE.U32 R2, R9, 0x%x, R2 ;\n", k * 16, s->in_stride);
        else if (k == I_LDG_A) LINE("%04zx LDG%s R%u, desc[UR4][R2.64%s] ;\n", k * 16, LD_SUFFIX[s->ld], I_LD_DST, io);
        else if (k == I_IMAD_OUT) LINE("%04zx IMAD.WIDE.U32 R6, R9, 0x%x, R6 ;\n", k * 16, s->out_stride);
        else LINE("%04zx %s ;\n", k * 16, PROLOGUE_TEXT[k]);
    }
    LINE("%04x IADD3 R9, PT, PT, R%u, R5, RZ ;\n", I_IADD3 * 16, I_LD_DST);
    LINE("%04x STG%s desc[UR4][R6.64%s], R%u ;\n", I_STG * 16, ST_SUFFIX[s->st], oo, I_LD_DST);
    LINE("%04x EXIT ;\n", I_EXIT * 16);
    LINE("%04x BRA 0x%x;\n", I_BRA * 16, I_BRA * 16);
    for (size_t k = I_NOP; k < NINSN; k++) LINE("%04zx NOP;\n", k * 16);
#undef LINE
    return 0;
}

static void get_words(const uint8_t *p, uint32_t w[4]) {
    for (int i = 0; i < 4; i++) w[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
}

int omega_ldst_check_kernel(const OmegaLdstSpec *s, const uint8_t *code, size_t len, char *why, size_t why_len) {
    uint8_t want[OMEGA_LDST_CODE_BYTES];
    size_t wl = 0;
    if (!code || len != OMEGA_LDST_CODE_BYTES) return refuse(why, why_len, "ldst kernel length %zu != %u", len, OMEGA_LDST_CODE_BYTES);
    /* decode the fields back out of the words, then require a spec-for-spec rebuild */
    uint32_t w[4];
    OmegaLdstSpec d = *s;
    get_words(code + 16 * I_LDG_A, w);
    unsigned lsz = (w[2] >> 9) & 7u;
    int found = 0;
    for (unsigned k = 0; k < OMEGA_LD_COUNT; k++) if (LD_SIZE[k] == lsz) { d.ld = (uint8_t)k; found = 1; }
    if (!found) return refuse(why, why_len, "ldst: LDG size field %u unknown", lsz); /* CHECK:ldst_ldsize */
    d.in_off = (int32_t)(w[1] & 0xffffff00u) >> 8;
    get_words(code + 16 * I_STG, w);
    unsigned ssz = (w[2] >> 9) & 7u;
    found = 0;
    for (unsigned k = 0; k < OMEGA_ST_COUNT; k++) if (ST_SIZE[k] == ssz) { d.st = (uint8_t)k; found = 1; }
    if (!found) return refuse(why, why_len, "ldst: STG size field %u unknown", ssz); /* CHECK:ldst_stsize */
    d.out_off = (int32_t)(w[1] & 0xffffff00u) >> 8;
    get_words(code + 16 * I_IMAD_IN, w);
    d.in_stride = w[1];
    get_words(code + 16 * I_IMAD_OUT, w);
    d.out_stride = w[1];
    if (omega_ldst_check_spec(&d, 1, why, why_len) != 0) return -1;                               /* CHECK:ldst_spec */
    if (d.ld != s->ld || d.st != s->st || d.in_stride != s->in_stride || d.out_stride != s->out_stride ||
        d.in_off != s->in_off || d.out_off != s->out_off)
        return refuse(why, why_len, "ldst: kernel encodes a different spec than requested");     /* CHECK:ldst_same_spec */
    if (omega_ldst_build_kernel(&d, want, sizeof want, &wl) != 0 || wl != len || memcmp(want, code, len) != 0)
        return refuse(why, why_len, "ldst: kernel words differ from the rebuilt kernel");        /* CHECK:ldst_rebuild */
    return 0;
}

/* ---- host model ---------------------------------------------------------------- */

int omega_ldst_host_run(const OmegaLdstSpec *s, const uint8_t *in_buf, size_t in_len, uint8_t *out_buf, size_t out_len,
                        size_t pad, size_t count) {
    int64_t il, ih, ol, oh;
    if (!in_buf || !out_buf || omega_ldst_extent(s, count, &il, &ih, &ol, &oh) != 0) return -1;
    if (il < -(int64_t)pad || ih > (int64_t)in_len - (int64_t)pad || ol < -(int64_t)pad || oh > (int64_t)out_len - (int64_t)pad) return -1;
    size_t lb = LD_BYTES[s->ld], sb = ST_BYTES[s->st];
    for (size_t i = 0; i < count; i++) {
        uint8_t reg[16] = { 0 };
        const uint8_t *src = in_buf + pad + (int64_t)s->in_off + (int64_t)i * s->in_stride;
        uint8_t *dst = out_buf + pad + (int64_t)s->out_off + (int64_t)i * s->out_stride;
        if (s->ld == OMEGA_LD_S8) { int32_t v = (int8_t)src[0]; memcpy(reg, &v, 4); }
        else if (s->ld == OMEGA_LD_S16) { int32_t v = (int16_t)(src[0] | src[1] << 8); memcpy(reg, &v, 4); }
        else { for (size_t b = 0; b < lb; b++) reg[b] = src[b]; }   /* U8 U16 zero extend: the rest of reg[0..3] is 0 */
        for (size_t b = 0; b < sb; b++) dst[b] = reg[b];
    }
    return 0;
}

/* ---- chip ---------------------------------------------------------------------- */

static int g_spec_id = -1;
void omega_ldst_gb10_set_spec_id(int id) { g_spec_id = id; }
/* One stderr line saying which device step failed, then the device error code. No other behaviour. */
#define LDST_DEVERR(step, rc) (fprintf(stderr, "OMEGA_DEVERR step=%s rc=%d spec=%d\n", (step), (int)(rc), g_spec_id), OMEGA_NUMERIC_ERR_DEVICE)

int omega_ldst_gb10_run(const OmegaLdstSpec *s, const uint8_t *in_buf, size_t in_len, uint8_t *out_buf, size_t out_len,
                        size_t pad, size_t count) {
    char err[256];
    int64_t il, ih, ol, oh;
    if (!in_buf || !out_buf || omega_ldst_check_spec(s, count, err, sizeof err) != 0) {
        fprintf(stderr, "omega_ldst_gb10_run: refused: %s\n", err);
        return OMEGA_NUMERIC_ERR_BAD_ARGS;
    }
    if (omega_ldst_extent(s, count, &il, &ih, &ol, &oh) != 0 || il < -(int64_t)pad || ih > (int64_t)in_len - (int64_t)pad ||
        ol < -(int64_t)pad || oh > (int64_t)out_len - (int64_t)pad || pad > OMEGA_LDST_PAD) {
        fprintf(stderr, "omega_ldst_gb10_run: refused: access outside the buffers\n");
        return OMEGA_NUMERIC_ERR_OPERANDS;
    }
    static uint8_t code[OMEGA_LDST_CODE_BYTES];
    size_t code_len = 0;
    if (omega_ldst_build_kernel(s, code, sizeof code, &code_len) != 0 || omega_ldst_check_kernel(s, code, code_len, err, sizeof err) != 0) {
        fprintf(stderr, "omega_ldst_gb10_run: refused: %s\n", err);
        return OMEGA_NUMERIC_ERR_OPERANDS;
    }
    {
        uint32_t q[OMEGA_BW_QMD_WORDS];
        OmegaBlackwellQmdConfig c = { .code_va = 0x200000000ull, .cbank_va = 0x200100000ull, .scratch_va = 0x200204000ull,
                                      .sem_va = 0x200202000ull, .qmd0_va = 0x200200000ull, .qmd1_va = 0x200201000ull,
                                      .num_elements = (uint32_t)count, .gpr_count = OMEGA_DS_GPR_COUNT };
        omega_numeric_launch_shape(count, &c.threads_per_block, &c.grid_width);
        if (omega_blackwell_build_qmd1(q, &c) != 0 || omega_ds_check_qmd(q, c.code_va, err, sizeof err) != OMEGA_NUMERIC_OK) {
            fprintf(stderr, "omega_ldst_gb10_run: refused: %s\n", err);
            return OMEGA_NUMERIC_ERR_OPERANDS;
        }
    }
#ifdef OMEGA_NUMERIC_CPU_ONLY
    return LDST_DEVERR("cpu_only", OMEGA_NUMERIC_ERR_DEVICE);
#else
    static const uint32_t SETUP[18] = {
        0x20012061, 0x0000cec0, 0x20012092, 0x00000001, 0x200120a8, 0x0000000f, 0x2001255d, 0x00000003,
        0x2001255e, 0x20000000, 0x2001255f, 0x000fffff, 0x20012557, 0x00000003, 0x20012558, 0x22000000,
        0x20012559, 0x00000000,
    };
    M16NativeContext ctx;
    int drc;
    if ((drc = m16_native_open(&ctx)) != 0) return LDST_DEVERR("open", drc);
    if ((drc = m16_native_create_channel(&ctx)) != 0) { m16_native_close(&ctx); return LDST_DEVERR("open", drc); }
    NvrmMem large_pb;
    if ((drc = nvrm_alloc(&ctx.rm, 0x10000, &large_pb)) != 0) { m16_native_close(&ctx); return LDST_DEVERR("alloc", drc); }
    ctx.pb_mem = large_pb;
    /* the prologue also reads 4 bytes at index i of the "b" buffer: give it the input, zero padded to 4*count */
    size_t in_dev = in_len > count * 4 ? in_len : count * 4;
    size_t in_bytes = (in_dev + 0xfffULL) & ~0xfffULL, out_bytes = (out_len + 0xfffULL) & ~0xfffULL;
    NvrmMem code_mem, cbank_mem, a_mem, out_mem, marker_mem, qmd_mem;
    if ((drc = nvrm_alloc(&ctx.rm, OMEGA_DS_MAX_CODE_BYTES, &code_mem)) != 0 || (drc = nvrm_alloc(&ctx.rm, 0x1000, &cbank_mem)) != 0 ||
        (drc = nvrm_alloc(&ctx.rm, in_bytes, &a_mem)) != 0 || (drc = nvrm_alloc(&ctx.rm, out_bytes, &out_mem)) != 0 ||
        (drc = nvrm_alloc(&ctx.rm, 0x1000, &marker_mem)) != 0 || (drc = nvrm_alloc(&ctx.rm, 0x10000, &qmd_mem)) != 0) {
        m16_native_close(&ctx);
        return LDST_DEVERR("alloc", drc);
    }
    memset(a_mem.cpu, 0, in_bytes);
    memcpy(a_mem.cpu, in_buf, in_len);
    memcpy(out_mem.cpu, out_buf, out_len);
    memcpy(code_mem.cpu, code, code_len);

    uint32_t cbank_data[OMEGA_BW_CBANK_DRIVER_WORDS];
    omega_blackwell_build_cbank_driver(cbank_data, cbank_mem.va);
    cbank_data[223] = 0;
    uint64_t ain = a_mem.va + pad, aout = out_mem.va + pad;
    uint32_t args[10] = { (uint32_t)ain, (uint32_t)(ain >> 32), (uint32_t)a_mem.va, (uint32_t)(a_mem.va >> 32),
                          (uint32_t)aout, (uint32_t)(aout >> 32), (uint32_t)count, 0, 0, 0 };
    memcpy(cbank_mem.cpu, cbank_data, sizeof(cbank_data));
    memcpy((uint8_t *)cbank_mem.cpu + 0x380, args, sizeof(args));

    uint64_t qmd0_va = qmd_mem.va, qmd1_va = qmd_mem.va + 0x1000, sem_va = qmd_mem.va + 0x2000;
    OmegaBlackwellQmdConfig cfg = { .code_va = code_mem.va, .cbank_va = cbank_mem.va, .scratch_va = qmd_mem.va + 0x4000,
                                    .sem_va = sem_va, .qmd0_va = qmd0_va, .qmd1_va = qmd1_va,
                                    .num_elements = (uint32_t)count, .gpr_count = OMEGA_DS_GPR_COUNT };
    omega_numeric_launch_shape(count, &cfg.threads_per_block, &cfg.grid_width);
    uint32_t qmd0[OMEGA_BW_QMD_WORDS], qmd1[OMEGA_BW_QMD_WORDS];
    if (omega_blackwell_build_qmd0(qmd0, qmd0_va, qmd1_va) != 0 || omega_blackwell_build_qmd1(qmd1, &cfg) != 0 ||
        omega_ds_check_qmd(qmd1, code_mem.va, err, sizeof err) != OMEGA_NUMERIC_OK ||
        omega_ldst_check_kernel(s, code_mem.cpu, code_len, err, sizeof err) != 0) {
        fprintf(stderr, "omega_ldst_gb10_run: %s\n", err);
        m16_native_close(&ctx);
        return OMEGA_NUMERIC_ERR_OPERANDS;
    }
    memcpy(qmd_mem.cpu, qmd0, sizeof(qmd0));
    memcpy((uint8_t *)qmd_mem.cpu + 0x1000, qmd1, sizeof(qmd1));
    volatile uint32_t *hsem = (volatile uint32_t *)((uint8_t *)qmd_mem.cpu + 0x2000);
    volatile uint32_t *hmarker = (volatile uint32_t *)marker_mem.cpu;
    *hsem = 0;
    *hmarker = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    static uint32_t pb[1024];
    size_t n = 0;
    memcpy(&pb[n], SETUP, sizeof(SETUP)); n += 18;
    pb[n++] = nvrm_mthd(1, 0x0188, 2); pb[n++] = (uint32_t)(cbank_mem.va >> 32); pb[n++] = (uint32_t)cbank_mem.va;
    pb[n++] = nvrm_mthd(1, 0x0180, 2); pb[n++] = 0x00000380; pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1); pb[n++] = 0x00000041;
    pb[n++] = (224 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[n], cbank_data, 224 * 4); n += 224;
    pb[n++] = nvrm_mthd(1, 0x0188, 2); pb[n++] = (uint32_t)((cbank_mem.va + 0x380) >> 32); pb[n++] = (uint32_t)(cbank_mem.va + 0x380);
    pb[n++] = nvrm_mthd(1, 0x0180, 2); pb[n++] = 0x00000028; pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1); pb[n++] = 0x00000041;
    pb[n++] = (10 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    memcpy(&pb[n], args, 10 * 4); n += 10;
    pb[n++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[n++] = (1u << 30) | (uint32_t)((qmd0_va >> 40) & 0x1ff);
    pb[n++] = (uint32_t)(qmd0_va >> 8);
    memcpy(&pb[n], qmd0, 96 * 4); n += 96;
    pb[n++] = nvrm_mthd(1, 0x0188, 2); pb[n++] = (uint32_t)(sem_va >> 32); pb[n++] = (uint32_t)sem_va;
    pb[n++] = nvrm_mthd(1, 0x0180, 2); pb[n++] = 0x00000004; pb[n++] = 0x00000001;
    pb[n++] = nvrm_mthd(1, 0x01b0, 1); pb[n++] = 0x00000041;
    pb[n++] = (1 << 16) | (1 << 13) | (0x01b4 >> 2) | (6u << 28);
    pb[n++] = OMEGA_BW_SEMAPHORE_INTERMEDIATE_INIT;
    pb[n++] = (98 << 16) | (1 << 13) | (0x0318 >> 2) | (2u << 28);
    pb[n++] = (1u << 30) | (uint32_t)((qmd1_va >> 40) & 0x1ff);
    pb[n++] = (uint32_t)(qmd1_va >> 8);
    memcpy(&pb[n], qmd1, 96 * 4); n += 96;
    pb[n++] = nvrm_mthd(0, 0x005c, 5);
    pb[n++] = (uint32_t)marker_mem.va; pb[n++] = (uint32_t)(marker_mem.va >> 32);
    pb[n++] = OMEGA_BW_MARKER_COMPLETION_PAYLOAD; pb[n++] = 0; pb[n++] = 0x1 | (1u << 20);

    if ((drc = m16_native_submit_methods(&ctx, pb, n)) != 0) { m16_native_close(&ctx); return LDST_DEVERR("submit", drc); }
    /* Wait long: closing the channel under a running kernel jams the seat. */
    if ((drc = m16_native_wait_marker(hmarker, OMEGA_BW_MARKER_COMPLETION_PAYLOAD, 600000)) != 0) { m16_native_close(&ctx); return LDST_DEVERR("wait", drc); }
    if ((drc = m16_native_wait_marker(hsem, OMEGA_BW_SEMAPHORE_INTERMEDIATE_DONE, 600000)) != 0) { m16_native_close(&ctx); return LDST_DEVERR("wait", drc); }
    __asm__ volatile("dsb sy" ::: "memory");
    memcpy(out_buf, out_mem.cpu, out_len);
    m16_native_close(&ctx);
    return OMEGA_NUMERIC_OK;
#endif
}
