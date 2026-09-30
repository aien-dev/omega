/*
 * numeric_oracle.h -- independent answers for the Gate 5 CPU tier.
 *
 * Test-only. Included once, by tests/test_omega_numeric.c. Nothing here calls
 * src/omega_numeric.c: the point is a second derivation that does not share
 * code, instructions or rounding helpers with the reference or the CPU
 * realization, so a bug in either one shows up as a mismatch.
 *
 *   FADD FSUB FMUL FFMA I2FP DIV RCP RSQ
 *       Integer soft-float. Operands are unpacked to (sign, integer
 *       significand, power of two). The exact result is formed in 128-bit
 *       integers (a sum of widely separated operands keeps a sticky bit),
 *       then rounded once, to nearest even, with gradual underflow
 *       (subnormals kept, no flush to zero). No floating-point instruction
 *       is used. NaN results are compared as a class (any NaN matches).
 *
 *   EXP LOG
 *       A high-precision value computed in IEEE binary128 (long double on
 *       AArch64 Linux: 113-bit significand, software arithmetic in the
 *       compiler runtime, no libm), from a Taylor series (exp) and the
 *       atanh series (log), then rounded once to FP32. The Omega sequences
 *       are polynomial approximations, not correctly rounded, so they are
 *       checked against an ulp bound measured on the corpus and stated at
 *       the check (NUM_ORACLE_EXP_ULP / NUM_ORACLE_LOG_ULP), never as
 *       bit-exact.
 */
#ifndef NUMERIC_ORACLE_H
#define NUMERIC_ORACLE_H

#include <stdint.h>
#include <stdbool.h>

typedef unsigned __int128 or_u128;

#define OR_QNAN 0x7fc00000u
#define OR_INF  0x7f800000u
#define OR_SIGN 0x80000000u

enum { OR_ZERO, OR_FINITE, OR_INFINITE, OR_NAN };

/* Unpack FP32 bits: finite value = m * 2^e (m has at most 24 bits). */
static int or_decode(uint32_t u, uint32_t *sign, uint64_t *m, int *e) {
    uint32_t ef = (u >> 23) & 0xffu, f = u & 0x007fffffu;
    *sign = u & OR_SIGN;
    if (ef == 0xffu) return f ? OR_NAN : OR_INFINITE;
    if (ef == 0) {
        if (!f) return OR_ZERO;
        *m = f; *e = -149;
        return OR_FINITE;
    }
    *m = f | 0x00800000u; *e = (int)ef - 150;
    return OR_FINITE;
}

static int or_msb(or_u128 m) {
    uint64_t hi = (uint64_t)(m >> 64);
    return hi ? 127 - __builtin_clzll(hi) : 63 - __builtin_clzll((uint64_t)m);
}

/* Round m * 2^e (m > 0, below 2^126) to FP32, nearest even, subnormals kept.
 * Callers that dropped low bits OR a 1 into bit 0 (sticky) and keep at least
 * two bits below the rounding point, so the jammed value rounds like the
 * exact one. */
static uint32_t or_pack(uint32_t sign, or_u128 m, int e) {
    int p = or_msb(m);
    int E = p + e;
    if (E > 127) return sign | OR_INF;
    int shift = (E >= -126) ? p - 23 : -149 - e;
    uint64_t q;
    if (shift <= 0) {
        q = (uint64_t)(m << -shift);
    } else if (shift > p + 1) {
        q = 0; /* below half of the smallest subnormal */
    } else {
        q = (uint64_t)(m >> shift);
        or_u128 rem = m & ((((or_u128)1) << shift) - 1);
        or_u128 half = ((or_u128)1) << (shift - 1);
        if (rem > half || (rem == half && (q & 1u))) q++;
    }
    uint32_t bits = (E >= -126) ? ((uint32_t)(E + 127) << 23) + (uint32_t)(q - 0x00800000u)
                                : (uint32_t)q;
    if ((bits & 0x7fffffffu) >= OR_INF) return sign | OR_INF;
    return sign | bits;
}

/* Exact signed sum of sa*ma*2^ea and sb*mb*2^eb (each m below 2^49), rounded
 * once. An exactly zero sum is +0 (round to nearest). */
static uint32_t or_sum(uint32_t sa, or_u128 ma, int ea, uint32_t sb, or_u128 mb, int eb) {
    if (ea < eb) {
        uint32_t ts = sa; sa = sb; sb = ts;
        or_u128 tm = ma; ma = mb; mb = tm;
        int te = ea; ea = eb; eb = te;
    }
    int d = ea - eb, e;
    or_u128 A, B;
    if (d <= 64) {
        A = ma << d; B = mb; e = eb;
    } else {
        /* B is below 2^-15 of A: keep 64 guard bits on A, fold B's lost bits
         * into a sticky bit. A is even, so the jammed result is odd and
         * never sits on a rounding boundary. */
        int k = d - 64;
        A = ma << 64; e = ea - 64;
        B = (k >= 127) ? (mb != 0) : ((mb >> k) | ((mb & ((((or_u128)1) << k) - 1)) != 0));
    }
    or_u128 m; uint32_t s;
    if (sa == sb)    { m = A + B; s = sa; }
    else if (A >= B) { m = A - B; s = sa; }
    else             { m = B - A; s = sb; }
    if (m == 0) return 0;
    return or_pack(s, m, e);
}

static uint32_t or_add(uint32_t ua, uint32_t ub) {
    uint32_t sa, sb; uint64_t ma = 0, mb = 0; int ea = 0, eb = 0;
    int ca = or_decode(ua, &sa, &ma, &ea), cb = or_decode(ub, &sb, &mb, &eb);
    if (ca == OR_NAN || cb == OR_NAN) return OR_QNAN;
    if (ca == OR_INFINITE && cb == OR_INFINITE) return sa == sb ? ua : OR_QNAN;
    if (ca == OR_INFINITE) return ua;
    if (cb == OR_INFINITE) return ub;
    if (ca == OR_ZERO && cb == OR_ZERO) return sa & sb;
    if (ca == OR_ZERO) return ub;
    if (cb == OR_ZERO) return ua;
    return or_sum(sa, ma, ea, sb, mb, eb);
}

static uint32_t or_sub(uint32_t ua, uint32_t ub) { return or_add(ua, ub ^ OR_SIGN); }

static uint32_t or_mul(uint32_t ua, uint32_t ub) {
    uint32_t sa, sb; uint64_t ma = 0, mb = 0; int ea = 0, eb = 0;
    int ca = or_decode(ua, &sa, &ma, &ea), cb = or_decode(ub, &sb, &mb, &eb);
    uint32_t s = sa ^ sb;
    if (ca == OR_NAN || cb == OR_NAN) return OR_QNAN;
    if ((ca == OR_INFINITE && cb == OR_ZERO) || (ca == OR_ZERO && cb == OR_INFINITE)) return OR_QNAN;
    if (ca == OR_INFINITE || cb == OR_INFINITE) return s | OR_INF;
    if (ca == OR_ZERO || cb == OR_ZERO) return s;
    return or_pack(s, (or_u128)ma * mb, ea + eb);
}

/* a * b + c with one rounding. */
static uint32_t or_fma(uint32_t ua, uint32_t ub, uint32_t uc) {
    uint32_t sa, sb, sc; uint64_t ma = 0, mb = 0, mc = 0; int ea = 0, eb = 0, ec = 0;
    int ca = or_decode(ua, &sa, &ma, &ea), cb = or_decode(ub, &sb, &mb, &eb);
    int cc = or_decode(uc, &sc, &mc, &ec);
    uint32_t ps = sa ^ sb;
    if (ca == OR_NAN || cb == OR_NAN || cc == OR_NAN) return OR_QNAN;
    if ((ca == OR_INFINITE && cb == OR_ZERO) || (ca == OR_ZERO && cb == OR_INFINITE)) return OR_QNAN;
    if (ca == OR_INFINITE || cb == OR_INFINITE) {
        if (cc == OR_INFINITE && sc != ps) return OR_QNAN;
        return ps | OR_INF;
    }
    if (cc == OR_INFINITE) return uc;
    if (ca == OR_ZERO || cb == OR_ZERO) {
        if (cc == OR_ZERO) return ps & sc;
        return uc;
    }
    if (cc == OR_ZERO) return or_pack(ps, (or_u128)ma * mb, ea + eb);
    return or_sum(ps, (or_u128)ma * mb, ea + eb, sc, mc, ec);
}

static uint32_t or_i2f(int32_t v) {
    if (v == 0) return 0;
    uint32_t s = v < 0 ? OR_SIGN : 0;
    uint64_t m = v < 0 ? (uint64_t)(-(int64_t)v) : (uint64_t)v;
    return or_pack(s, m, 0);
}

/* a / b, correctly rounded: 100-bit integer long division plus sticky. */
static uint32_t or_div(uint32_t ua, uint32_t ub) {
    uint32_t sa, sb; uint64_t ma = 0, mb = 0; int ea = 0, eb = 0;
    int ca = or_decode(ua, &sa, &ma, &ea), cb = or_decode(ub, &sb, &mb, &eb);
    uint32_t s = sa ^ sb;
    if (ca == OR_NAN || cb == OR_NAN) return OR_QNAN;
    if ((ca == OR_INFINITE && cb == OR_INFINITE) || (ca == OR_ZERO && cb == OR_ZERO)) return OR_QNAN;
    if (ca == OR_INFINITE || cb == OR_ZERO) return s | OR_INF;
    if (ca == OR_ZERO || cb == OR_INFINITE) return s;
    or_u128 n = ((or_u128)ma) << 100;
    or_u128 q = n / mb, r = n % mb;
    return or_pack(s, (q << 1) | (r != 0), ea - eb - 101);
}

/* sqrt, correctly rounded: integer square root of a >= 2^100 scaled value. */
static uint32_t or_sqrt(uint32_t ua) {
    uint32_t sa; uint64_t ma = 0; int ea = 0;
    int ca = or_decode(ua, &sa, &ma, &ea);
    if (ca == OR_NAN) return OR_QNAN;
    if (ca == OR_ZERO) return ua;                 /* sqrt(-0) = -0 */
    if (sa) return OR_QNAN;
    if (ca == OR_INFINITE) return ua;
    int s = 100 - or_msb(ma);
    if ((ea - s) & 1) s++;                        /* even exponent */
    or_u128 x = ((or_u128)ma) << s;
    or_u128 root = 0, bit = ((or_u128)1) << 124;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= root + bit) { x -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return or_pack(0, (root << 1) | (x != 0), (ea - s) / 2 - 1);
}

static uint32_t or_rcp(uint32_t ua) { return or_div(0x3f800000u, ua); }
static uint32_t or_rsq(uint32_t ua) { return or_div(0x3f800000u, or_sqrt(ua)); }

/* ---- binary128 references for EXP and LOG --------------------------------- */

static const long double OR_LN2 = 0.693147180559945309417232121458176568L;

static long double or_pow2(int k) {
    long double r = 1.0L;
    long double f = k >= 0 ? 2.0L : 0.5L;
    for (int n = k >= 0 ? k : -k; n; n--) r *= f;
    return r;
}

/* float bits from a binary128 value, rounded once (compiler runtime). */
static uint32_t or_ld_to_bits(long double v) {
    float f = (float)v;
    uint32_t u;
    __builtin_memcpy(&u, &f, sizeof(u));
    return u;
}

static uint32_t or_exp(uint32_t ux) {
    uint32_t sx; uint64_t mx = 0; int ex = 0;
    int cx = or_decode(ux, &sx, &mx, &ex);
    if (cx == OR_NAN) return OR_QNAN;
    if (cx == OR_INFINITE) return sx ? 0 : OR_INF;
    if (cx == OR_ZERO) return 0x3f800000u;
    long double x = (long double)mx * or_pow2(ex);
    if (sx) x = -x;
    if (x > 89.0L) return OR_INF;
    if (x < -110.0L) return 0;
    long double kq = x / OR_LN2;
    int k = (int)(kq >= 0 ? kq + 0.5L : kq - 0.5L);
    long double r = x - (long double)k * OR_LN2;  /* |r| <= ~0.35 */
    long double term = 1.0L, sum = 1.0L;
    for (int n = 1; n < 40; n++) { term *= r / (long double)n; sum += term; }
    return or_ld_to_bits(sum * or_pow2(k));
}

static uint32_t or_log(uint32_t ux) {
    uint32_t sx; uint64_t mx = 0; int ex = 0;
    int cx = or_decode(ux, &sx, &mx, &ex);
    if (cx == OR_NAN) return OR_QNAN;
    if (cx == OR_ZERO) return OR_SIGN | OR_INF;
    if (sx) return OR_QNAN;
    if (cx == OR_INFINITE) return OR_INF;
    /* x = mx * 2^ex; write it as m * 2^k with m in [sqrt(1/2), sqrt(2)) */
    int k = ex + or_msb(mx);
    long double m = (long double)mx * or_pow2(-or_msb(mx));   /* [1, 2) */
    if (m > 1.41421356237309504880168872420969808L) { m *= 0.5L; k++; }
    long double z = (m - 1.0L) / (m + 1.0L), z2 = z * z;   /* |z| < 0.172 */
    long double term = z, sum = 0.0L;
    for (int n = 1; n < 80; n += 2) { sum += term / (long double)n; term *= z2; }
    return or_ld_to_bits((long double)k * OR_LN2 + 2.0L * sum);
}

/* Distance in units in the last place between two FP32 values, counting
 * representable numbers between them (+0 and -0 are one point). */
static uint32_t or_ulp_distance(uint32_t a, uint32_t b) {
    int64_t oa = (a & OR_SIGN) ? -(int64_t)(a & 0x7fffffffu) : (int64_t)a;
    int64_t ob = (b & OR_SIGN) ? -(int64_t)(b & 0x7fffffffu) : (int64_t)b;
    int64_t d = oa - ob;
    if (d < 0) d = -d;
    return d > 0xffffffffLL ? 0xffffffffu : (uint32_t)d;
}

static bool or_is_nan(uint32_t u) { return (u & 0x7fffffffu) > OR_INF; }

/* NaN class match, else identical bits. */
static bool or_same(uint32_t want, uint32_t got) {
    if (or_is_nan(want) || or_is_nan(got)) return or_is_nan(want) && or_is_nan(got);
    return want == got;
}

#endif
