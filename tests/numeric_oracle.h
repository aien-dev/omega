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

/* ---- E1 scalar contract (docs/numeric/E1_SCALAR_CONTRACT.md) ------------------
 * Second derivations of the E1 ops, written differently from the reference
 * in src/omega_numeric.c on purpose:
 *   integer conversions  x scaled to a fixed point with 26 fraction bits in
 *                        an int64; floor is the arithmetic right shift, ceil
 *                        is -floor(-x), RNI adds one half and fixes the tie.
 *   U32 -> FP32          or_pack of the integer.
 *   FP32 -> F16          binary search over the 31744 finite F16 bit
 *                        patterns, then an exact comparison with the
 *                        midpoint (all values scaled to integers).
 *   FP32 -> BF16         the bias trick on the bit pattern: add 0x7fff plus
 *                        the kept lsb, drop 16 bits.
 *   F16/BF16 -> FP32     or_pack of the narrow value's integer significand.
 *   compare predicates   the total-order key (negative patterns inverted),
 *                        -0 folded onto +0 first.
 *   FFMA_V               or_fma above.
 */
enum { OR_RTZ, OR_FLOOR, OR_CEIL, OR_RNE };

/* FP32 bits -> integer in rounding direction mode, saturated to [lo, hi],
 * NaN -> 0. */
static int64_t or_to_int(uint32_t u, int mode, int64_t lo, int64_t hi) {
    uint32_t s; uint64_t m = 0; int e = 0;
    int cl = or_decode(u, &s, &m, &e);
    if (cl == OR_NAN || cl == OR_ZERO) return 0;
    if (cl == OR_INFINITE) return s ? lo : hi;
    int lead = 63 - __builtin_clzll(m) + e;          /* |x| in [2^lead, 2^(lead+1)) */
    if (lead >= 32) return s ? lo : hi;              /* beyond every target range */
    if (lead < -2) {                                 /* 0 < |x| < 1/4: no tie possible */
        if (mode == OR_FLOOR) return s ? -1 : 0;
        if (mode == OR_CEIL)  return s ? 0 : 1;
        return 0;
    }
    /* lead >= -2 gives e >= -25: X = x * 2^26 is an exact integer below 2^58 */
    int64_t X = (int64_t)(m << (e + 26));
    if (s) X = -X;
    const int64_t ONE = 1ll << 26, HALF = 1ll << 25;
    int64_t q;
    switch (mode) {
    case OR_FLOOR: q = X >> 26; break;               /* arithmetic shift = floor */
    case OR_CEIL:  q = -((-X) >> 26); break;
    case OR_RNE: {
        int64_t t = X + HALF;                        /* floor(x + 1/2) */
        q = t >> 26;
        if ((t & (ONE - 1)) == 0 && (q & 1)) q -= 1; /* exact tie: to even */
        break;
    }
    default: q = s ? -((-X) >> 26) : (X >> 26); break;   /* toward zero */
    }
    return q < lo ? lo : q > hi ? hi : q;
}

static uint32_t or_u2f(uint32_t v) { return v ? or_pack(0, v, 0) : 0; }

/* Exact value of a positive finite F16 pattern h (0..0x7bff) as hm * 2^he. */
static void or_f16_val(uint32_t h, uint64_t *hm, int *he) {
    uint32_t ef = (h >> 10) & 0x1fu, f = h & 0x3ffu;
    if (ef == 0) { *hm = f; *he = -24; }
    else         { *hm = f | 0x400u; *he = (int)ef - 25; }
}

/* v * 2^e scaled by 2^60 (e >= -60), as an integer. */
static or_u128 or_scaled(uint64_t v, int e) { return ((or_u128)v) << (e + 60); }

static uint32_t or_f32_to_f16(uint32_t u) {
    uint32_t s; uint64_t m = 0; int e = 0;
    int cl = or_decode(u, &s, &m, &e);
    uint32_t sign = s >> 16;
    if (cl == OR_NAN) return sign | 0x7e00u;
    if (cl == OR_INFINITE) return sign | 0x7c00u;
    if (cl == OR_ZERO) return sign;
    int lead = 63 - __builtin_clzll(m) + e;         /* |x| in [2^lead, 2^(lead+1)) */
    if (lead >= 17) return sign | 0x7c00u;          /* |x| >= 2^17: far past 65520 */
    if (lead < -26) return sign;                    /* |x| < 2^-26 < half of 2^-24 */
    /* now e >= -26 - 23 = -49, so |x| * 2^60 is an integer below 2^78 */
    or_u128 X = or_scaled(m, e);
    /* largest finite pattern lo with value(lo) <= |x| (0 if |x| < 2^-24) */
    uint32_t lo = 0, hi = 0x7bffu;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        uint64_t hm; int he; or_f16_val(mid, &hm, &he);
        if (or_scaled(hm, he) <= X) lo = mid; else hi = mid - 1;
    }
    uint64_t lm; int le; or_f16_val(lo, &lm, &le);
    or_u128 L = or_scaled(lm, le), H;
    if (lo == 0x7bffu) H = or_scaled(1, 16);        /* the next step past 65504 is 2^16 (inf) */
    else { uint64_t hm; int he; or_f16_val(lo + 1, &hm, &he); H = or_scaled(hm, he); }
    if (L == X) return sign | lo;
    or_u128 twice = X * 2, sum = L + H;
    uint32_t pick = (twice < sum) ? lo : (twice > sum) ? lo + 1 : ((lo & 1u) ? lo + 1 : lo);
    return sign | pick;                             /* lo + 1 == 0x7c00 is +inf */
}

static uint32_t or_f32_to_bf16(uint32_t u) {
    if (or_is_nan(u)) return ((u >> 16) & 0x8000u) | 0x7fc0u;
    uint32_t lsb = (u >> 16) & 1u;
    return (uint32_t)(((uint64_t)u + 0x7fffu + lsb) >> 16) & 0xffffu;
}

static uint32_t or_f16_to_f32(uint32_t h) {
    uint32_t sign = (h & 0x8000u) << 16, ef = (h >> 10) & 0x1fu;
    if (ef == 0x1fu) return (h & 0x3ffu) ? OR_QNAN : (sign | OR_INF);
    uint64_t hm; int he; or_f16_val(h & 0x7fffu, &hm, &he);
    return hm ? or_pack(sign, hm, he) : sign;
}

static uint32_t or_bf16_to_f32(uint32_t h) {
    uint32_t sign = (h & 0x8000u) << 16, ef = (h >> 7) & 0xffu, f = h & 0x7fu;
    if (ef == 0xffu) return f ? OR_QNAN : (sign | OR_INF);
    if (ef == 0) return f ? or_pack(sign, f, -133) : sign;     /* f * 2^-126 / 2^7 */
    return or_pack(sign, f | 0x80u, (int)ef - 134);
}

/* Total-order key: larger key, larger value. -0 is folded onto +0. */
static uint32_t or_key(uint32_t u) {
    if (u == OR_SIGN) u = 0;
    return (u & OR_SIGN) ? ~u : (u | OR_SIGN);
}

/* Predicate by name: "LT" "LE" "GT" "GE" "EQ" "NE" "NUM" "NAN" and the U
 * forms (true also when unordered). */
static bool or_pred(const char *p, uint32_t a, uint32_t b) {
    bool un = or_is_nan(a) || or_is_nan(b);
    size_t n = 0; while (p[n]) n++;
    if (p[0] == 'N' && p[1] == 'U' && p[2] == 'M') return !un;
    if (p[0] == 'N' && p[1] == 'A' && p[2] == 'N') return un;
    bool u_form = n == 3 && p[2] == 'U';
    if (un) return u_form;
    uint32_t ka = or_key(a), kb = or_key(b);
    if (p[0] == 'L' && p[1] == 'T') return ka < kb;
    if (p[0] == 'L' && p[1] == 'E') return ka <= kb;
    if (p[0] == 'G' && p[1] == 'T') return ka > kb;
    if (p[0] == 'G' && p[1] == 'E') return ka >= kb;
    if (p[0] == 'E' && p[1] == 'Q') return ka == kb;
    if (p[0] == 'N' && p[1] == 'E') return ka != kb;
    return false;
}

#endif
