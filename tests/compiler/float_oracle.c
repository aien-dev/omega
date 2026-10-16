/*
 * float_oracle.c -- the C reference that produced every `// expect-run:` value of the
 * OSC-4 Slice A float golden programs (tests/compiler/progs/f_*.osc).
 *
 * Each OSC function is re-spelled in C one operation per source operation, with the
 * inputs in volatile locals so nothing is folded at compile time and every rounding
 * step happens on this CPU, under the same flags as the compiler
 * (-O2 -ffp-contract=off -fno-fast-math -fno-math-errno, mk/compiler.mk OSC_CFLAGS).
 * It prints one line per expect-run, in the exact text of the program headers, so
 * `make test-compiler-float-oracle` can diff the two: a changed expectation that the
 * oracle does not reproduce fails the target.
 *
 * Where C leaves a conversion undefined (float -> int out of range, NaN -> int) the
 * oracle spells the FCVTZS/FCVTZU rule out (truncate, saturate, NaN -> 0); min/max
 * spell out the FMINNM/FMAXNM rule (docs/osc/OSC-4-DESIGN.md 1.1 item 6). Those two
 * rules are the design's own and are stated here independently of src/compiler/osc_interp.c.
 * No Python, no libm beyond sqrt/sqrtf/fabs/fabsf (all inlined to one instruction).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t b32(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static uint64_t b64(double d) { uint64_t w; memcpy(&w, &d, 8); return w; }
static float f32(uint64_t w) { uint32_t x = (uint32_t)w; float f; memcpy(&f, &x, 4); return f; }
static double f64(uint64_t w) { double d; memcpy(&d, &w, 8); return d; }

/* one expect-run line: NAME, the argument text exactly as the program header spells it, the value */
static void hexline(const char *name, const char *args, uint64_t v) { printf("%s%s%s -> %#llx\n", name, *args ? " " : "", args, (unsigned long long)v); }
static void sline(const char *name, const char *args, int64_t v) { printf("%s%s%s -> %lld\n", name, *args ? " " : "", args, (long long)v); }
static void uline(const char *name, const char *args, uint64_t v) { printf("%s%s%s -> %llu\n", name, *args ? " " : "", args, (unsigned long long)v); }

/* FCVTZS / FCVTZU: truncate toward zero, saturate to the destination, NaN -> 0 */
static int64_t ftoi_s(double x, unsigned w) {
    if (x != x) return 0;
    double half = (double)(1ULL << (w - 1));
    int64_t mx = (int64_t)((1ULL << (w - 1)) - 1), mn = -mx - 1;
    if (x >= half) return mx;
    if (x <= -half) return mn;
    return (int64_t)x;
}
static uint64_t ftoi_u(double x, unsigned w) {
    if (x != x || x <= 0.0) return 0;
    double top = (double)(1ULL << (w - 1)) * 2.0;
    if (x >= top) return w == 64 ? UINT64_MAX : (1ULL << w) - 1;
    return (uint64_t)x;
}
/* FMINNM / FMAXNM on f64 bit patterns */
static int isnan64(uint64_t a) { return (a & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (a & 0x000fffffffffffffull); }
static int issnan64(uint64_t a) { return isnan64(a) && !(a & 0x0008000000000000ull); }
static uint64_t minmax64(uint64_t a, uint64_t b, int is_max) {
    if (isnan64(a) || isnan64(b)) {
        if (issnan64(a)) return a | 0x0008000000000000ull;
        if (issnan64(b)) return b | 0x0008000000000000ull;
        if (isnan64(a) && isnan64(b)) return a;
        return isnan64(a) ? b : a;
    }
    volatile double x = f64(a), y = f64(b);
    if (x == y) { int neg_a = (a >> 63) != 0; return is_max ? (neg_a ? b : a) : (neg_a ? a : b); }
    return is_max ? (x < y ? b : a) : (x < y ? a : b);
}

/* ---------------------------------------------------------------- f_adam_step.osc */
static float adam32(uint64_t P, uint64_t G, uint64_t M, uint64_t V) {
    volatile float p = f32(P), g = f32(G), m = f32(M), v = f32(V);
    volatile float b1 = 0.9f, b2 = 0.999f, lr = 0.001f, eps = 1.0e-8f;
    float m1 = b1 * m + (1.0f - b1) * g;
    float v1 = b2 * v + (1.0f - b2) * (g * g);
    float mh = m1 / (1.0f - b1);
    float vh = v1 / (1.0f - b2);
    return p - lr * mh / (sqrtf(vh) + eps);
}
static double adam64(uint64_t P, uint64_t G, uint64_t M, uint64_t V) {
    volatile double p = f64(P), g = f64(G), m = f64(M), v = f64(V);
    volatile double b1 = 0.9, b2 = 0.999, lr = 0.001, eps = 1.0e-8;
    double m1 = b1 * m + (1.0 - b1) * g;
    double v1 = b2 * v + (1.0 - b2) * (g * g);
    double mh = m1 / (1.0 - b1);
    double vh = v1 / (1.0 - b2);
    return p - lr * mh / (sqrt(vh) + eps);
}
static float lerp(uint64_t A, uint64_t B, uint64_t T) { volatile float a = f32(A), b = f32(B), t = f32(T); return a + (b - a) * t; }
static float sumsq(uint64_t X) {
    volatile float x = f32(X), s = 0.0f;
    for (int i = 0; i < 4; i++) s = s + x * x;
    return s;
}
static void f_adam_step(void) {
    hexline("adam32", "0x3f800000 0x3f000000 0 0", b32(adam32(0x3f800000, 0x3f000000, 0, 0)));
    hexline("adam32", "0xc0200000 0x3dcccccd 0x3e4ccccd 0x3e99999a", b32(adam32(0xc0200000, 0x3dcccccd, 0x3e4ccccd, 0x3e99999a)));
    hexline("adam64", "0x3ff0000000000000 0x3fe0000000000000 0 0", b64(adam64(0x3ff0000000000000ull, 0x3fe0000000000000ull, 0, 0)));
    hexline("adam64", "0xc004000000000000 0x3fb999999999999a 0x3fc999999999999a 0x3fd3333333333333",
            b64(adam64(0xc004000000000000ull, 0x3fb999999999999aull, 0x3fc999999999999aull, 0x3fd3333333333333ull)));
    hexline("lerp", "0x3f800000 0x40400000 0x3e800000", b32(lerp(0x3f800000, 0x40400000, 0x3e800000)));
    hexline("sumsq", "0x3dcccccd", b32(sumsq(0x3dcccccd)));
}

/* ---------------------------------------------------------------- f_cast_sat.osc */
static void f_cast_sat(void) {
    sline("f2i32", "0x4f000000", ftoi_s(f32(0x4f000000), 32));
    sline("f2i32", "0xcf000001", ftoi_s(f32(0xcf000001), 32));
    sline("f2i32", "0xc0490fd0", ftoi_s(f32(0xc0490fd0), 32));
    sline("f2i32", "0x7fc00000", ftoi_s(f32(0x7fc00000), 32));
    sline("f2i32", "0xff800000", ftoi_s(f32(0xff800000), 32));
    uline("f2u32", "0xbf800000", ftoi_u(f32(0xbf800000), 32));
    uline("f2u32", "0x4f000000", ftoi_u(f32(0x4f000000), 32));
    uline("f2u32", "0x7f800000", ftoi_u(f32(0x7f800000), 32));
    uline("f2u32", "0x40490fd0", ftoi_u(f32(0x40490fd0), 32));
    sline("f2i64", "0x7ff0000000000000", ftoi_s(f64(0x7ff0000000000000ull), 64));
    sline("f2i64", "0xfff0000000000000", ftoi_s(f64(0xfff0000000000000ull), 64));
    sline("f2i64", "0xc004000000000000", ftoi_s(f64(0xc004000000000000ull), 64));
    sline("f2i64", "0x7ff8000000000000", ftoi_s(f64(0x7ff8000000000000ull), 64));
    uline("f2u64", "0x43f0000000000000", ftoi_u(f64(0x43f0000000000000ull), 64));
    uline("f2u64", "0xbff8000000000000", ftoi_u(f64(0xbff8000000000000ull), 64));
    uline("f2u64", "0x40f0000000000000", ftoi_u(f64(0x40f0000000000000ull), 64));
    { volatile int64_t x = 16777217; hexline("i2f", "16777217", b32((float)x)); }
    { volatile int64_t x = 16777219; hexline("i2f", "16777219", b32((float)x)); }
    { volatile int64_t x = -1; hexline("i2f", "-1", b32((float)x)); }
    { volatile uint64_t x = 18446744073709551615ull; hexline("u2d", "18446744073709551615", b64((double)x)); }
    { volatile uint64_t x = 9007199254740993ull; hexline("u2d", "9007199254740993", b64((double)x)); }
    { volatile double x = f64(0x3ff0000010000000ull); hexline("d2f", "0x3ff0000010000000", b32((float)x)); }
    { volatile double x = f64(0x7e37e43c8800759cull); hexline("d2f", "0x7e37e43c8800759c", b32((float)x)); }
    { volatile double x = f64(0xbff8000000000000ull); hexline("d2f", "0xbff8000000000000", b32((float)x)); }
    { volatile float x = f32(0x3f800000); hexline("f2d", "0x3f800000", b64((double)x)); }
    { volatile float x = f32(0xffc00000); hexline("f2d", "0xffc00000", b64((double)x)); }
    { volatile uint8_t x = 200; hexline("i2f_small", "200", b32((float)x)); }
}

/* ---------------------------------------------------------------- f_nan_cmp.osc */
static uint64_t cmp64(uint64_t A, uint64_t B) {
    volatile double a = f64(A), b = f64(B);
    uint64_t r = 0;
    if (a == b) r |= 1;
    if (a != b) r |= 2;
    if (a < b) r |= 4;
    if (a <= b) r |= 8;
    if (a > b) r |= 16;
    if (a >= b) r |= 32;
    return r;
}
static uint64_t cmp32(uint64_t A, uint64_t B) {
    volatile float a = f32(A), b = f32(B);
    uint64_t r = 0;
    if (a == b) r |= 1;
    if (a != b) r |= 2;
    if (a < b) r |= 4;
    if (a <= b) r |= 8;
    if (a > b) r |= 16;
    if (a >= b) r |= 32;
    return r;
}
static void f_nan_cmp(void) {
    uline("cmp64", "0x3ff0000000000000 0x4000000000000000", cmp64(0x3ff0000000000000ull, 0x4000000000000000ull));
    uline("cmp64", "0x4000000000000000 0x4000000000000000", cmp64(0x4000000000000000ull, 0x4000000000000000ull));
    uline("cmp64", "0x4000000000000000 0x3ff0000000000000", cmp64(0x4000000000000000ull, 0x3ff0000000000000ull));
    uline("cmp64", "0x7ff8000000000000 0x3ff0000000000000", cmp64(0x7ff8000000000000ull, 0x3ff0000000000000ull));
    uline("cmp64", "0x3ff0000000000000 0x7ff0000000000001", cmp64(0x3ff0000000000000ull, 0x7ff0000000000001ull));
    uline("cmp64", "0x7ff8000000000000 0x7ff8000000000000", cmp64(0x7ff8000000000000ull, 0x7ff8000000000000ull));
    uline("cmp64", "0x8000000000000000 0", cmp64(0x8000000000000000ull, 0));
    uline("cmp32", "0x7fc00000 0x7fc00000", cmp32(0x7fc00000, 0x7fc00000));
    uline("cmp32", "0x80000000 0", cmp32(0x80000000, 0));
    uline("cmp32", "0xff800000 0x7f800000", cmp32(0xff800000, 0x7f800000));
    static const uint64_t props[6][2] = {
        {0x7ff0000000000001ull, 0x3ff0000000000000ull}, {0x3ff0000000000000ull, 0x7ff0000000000001ull},
        {0x7ff80000000000abull, 0x4000000000000000ull}, {0x7ff0000000000000ull, 0x7ff0000000000000ull},
        {0x7ff0000000000001ull, 0x7ff80000000000abull}, {0x7ff80000000000abull, 0x7ff0000000000001ull}};
    for (int i = 0; i < 6; i++) {
        char args[64];
        snprintf(args, sizeof args, "%#llx %#llx", (unsigned long long)props[i][0], (unsigned long long)props[i][1]);
        volatile double a = f64(props[i][0]), b = f64(props[i][1]);
        hexline("prop", args, b64(a - b));
    }
    static const uint64_t negs[3] = {0x7ff80000000000abull, 0xfff80000000000abull, 0x3ff0000000000000ull};
    for (int i = 0; i < 3; i++) {
        char args[32];
        snprintf(args, sizeof args, "%#llx", (unsigned long long)negs[i]);
        volatile double a = f64(negs[i]);
        hexline("neg_abs", args, b64(fabs(-a)));
    }
    static const struct { uint64_t a, b; int which; const char *txt; } mm[] = {
        {0x7ff0000000000001ull, 0x3ff0000000000000ull, 0, "0x7ff0000000000001 0x3ff0000000000000 0"},
        {0x7ff8000000000000ull, 0x3ff0000000000000ull, 0, "0x7ff8000000000000 0x3ff0000000000000 0"},
        {0x3ff0000000000000ull, 0x7ff8000000000000ull, 1, "0x3ff0000000000000 0x7ff8000000000000 1"},
        {0x7ff80000000000abull, 0x7ff8000000000000ull, 1, "0x7ff80000000000ab 0x7ff8000000000000 1"},
        {0x7ff8000000000000ull, 0x7ff0000000000001ull, 1, "0x7ff8000000000000 0x7ff0000000000001 1"},
        {0x8000000000000000ull, 0, 0, "0x8000000000000000 0 0"},
        {0, 0x8000000000000000ull, 0, "0 0x8000000000000000 0"},
        {0x8000000000000000ull, 0, 1, "0x8000000000000000 0 1"},
        {0, 0x8000000000000000ull, 1, "0 0x8000000000000000 1"},
        {0x3ff0000000000000ull, 0x4000000000000000ull, 0, "0x3ff0000000000000 0x4000000000000000 0"},
        {0x3ff0000000000000ull, 0x4000000000000000ull, 1, "0x3ff0000000000000 0x4000000000000000 1"},
    };
    for (size_t i = 0; i < sizeof mm / sizeof mm[0]; i++) hexline("minmax", mm[i].txt, minmax64(mm[i].a, mm[i].b, mm[i].which));
    { volatile double a = f64(0xbff0000000000000ull); hexline("sqrt64", "0xbff0000000000000", b64(sqrt(a))); }
    { volatile double a = f64(0x4000000000000000ull); hexline("sqrt64", "0x4000000000000000", b64(sqrt(a))); }
    hexline("bits_rt", "0x7ff0000000000001", 0x7ff0000000000001ull); /* bit casts move no value */
    hexline("bits_rt", "0x8000000000000000", 0x8000000000000000ull);
}

/* ---------------------------------------------------------------- f_neg_zero.osc */
static void f_neg_zero(void) {
    { volatile float x = f32(0); hexline("neg", "0", b32(-x)); }
    { volatile float x = f32(0x80000000); hexline("neg", "0x80000000", b32(-x)); }
    { volatile float x = f32(0x3f800000); hexline("neg", "0x3f800000", b32(-x)); }
    { volatile float x = f32(0); hexline("zero_minus", "0", b32(0.0f - x)); }
    { volatile float x = f32(0x80000000); hexline("zero_minus", "0x80000000", b32(0.0f - x)); }
    hexline("neg_lit", "0", b32(-0.0f));
    hexline("neg_lit", "1", b32(-1.5f));
    { volatile float x = f32(0x80000000); uline("eq_zero", "0x80000000", x == 0.0f); }
    { volatile float x = f32(0); uline("eq_zero", "0", x == 0.0f); }
    { volatile float x = f32(0x3f800000); uline("eq_zero", "0x3f800000", x == 0.0f); }
    { volatile float x = f32(0x3f800000), nz = -0.0f; hexline("div_nz", "0x3f800000", b32(x / nz)); }
    { volatile float x = f32(0xbf800000), nz = -0.0f; hexline("div_nz", "0xbf800000", b32(x / nz)); }
    { volatile float x = f32(0x80000000); hexline("abs_nz", "0x80000000", b32(fabsf(x))); }
    { volatile float x = f32(0x80000000); hexline("sqrt_nz", "0x80000000", b32(sqrtf(x))); }
    uline("sign_bit", "0x80000000", 0x80000000u >> 31);
    uline("sign_bit", "0", 0u >> 31);
    hexline("from_bits", "2147483648", 2147483648ull);
    hexline("from_bits", "1065353216", 1065353216ull);
    hexline("lit_bits", "0", b32(-0.0f));
    hexline("lit_bits", "1", b64(-0.0));
    { volatile double nz = -0.0; hexline("sub_mul", "0", b64(nz + 0.0)); }
    { volatile double nz = -0.0; hexline("sub_mul", "1", b64(nz * 1.0)); }
}

/* ---------------------------------------------------------------- f_array_dot.osc */
static double dot(uint32_t seed) {
    volatile double a[8], b[8], s = 0.0;
    for (int i = 0; i < 8; i++) { b[i] = 1.5; volatile int64_t k = (int64_t)seed + i; a[i] = (double)k * 0.5; }
    for (int i = 0; i < 8; i++) s = s + a[i] * b[i];
    return s;
}
static float fill_sum(uint64_t X) {
    volatile float a[4], s = 0.0f;
    for (int i = 0; i < 4; i++) a[i] = f32(X);
    for (int i = 0; i < 4; i++) s = s + a[i];
    return s;
}
static double scale_ref(uint64_t X) {
    volatile double a[5];
    for (int i = 0; i < 5; i++) a[i] = f64(X);
    for (int i = 0; i < 5; i++) a[i] = a[i] * 5.0;
    return a[4] * 2.0;
}
static double ret_arr(uint8_t k) {
    volatile double a[2] = {0.0, 0.0};
    volatile double v = (double)k - 1.0;
    a[0] = v;
    a[1] = a[0] + 1.0;
    return a[1];
}
static double widen_sum(uint64_t X) {
    volatile float a[4];
    volatile double s = 0.0;
    for (int i = 0; i < 4; i++) a[i] = f32(X);
    for (int i = 0; i < 4; i++) s = s + (double)a[i];
    return s;
}
static void f_array_dot(void) {
    hexline("dot", "0", b64(dot(0)));
    hexline("dot", "4", b64(dot(4)));
    hexline("fill_sum", "0x3dcccccd", b32(fill_sum(0x3dcccccd)));
    hexline("fill_sum", "0x7fc00000", b32(fill_sum(0x7fc00000)));
    hexline("scale_ref", "0x4000000000000000", b64(scale_ref(0x4000000000000000ull)));
    hexline("ret_arr", "3", b64(ret_arr(3)));
    hexline("widen_sum", "0x3dcccccd", b64(widen_sum(0x3dcccccd)));
}

/* ---------------------------------------------------------------- f_literals.osc */
static void f_literals(void) {
    char s[80] = "0.";
    memset(s + 2, '0', 60);
    s[62] = '1';
    s[63] = 0; /* 63 characters: the longest literal the lexer accepts */
    hexline("tiny63", "", b64(strtod(s, NULL)));
    hexline("tiny_e", "", b64(strtod("1.0e-161", NULL)));
    hexline("tiny32", "", b32(strtof("0.0000000000000000000000000000001", NULL)));
    hexline("one_e", "", b64(strtod("10.0e-1", NULL)));
}

int main(void) {
    f_adam_step();
    f_cast_sat();
    f_nan_cmp();
    f_neg_zero();
    f_array_dot();
    f_literals();
    return 0;
}
