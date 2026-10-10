/* Exact arithmetic for AT-0: reduced rationals (64-bit fields, 128-bit
 * intermediates, limits of AT0_CASE_V1 section 2), scaled decimals, canonical
 * token parsing/printing, and an exact comparison of a binary64 value against
 * scaled-decimal bounds using a small fixed-size unsigned bignum. */
#include "at0_model.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

static const char *const status_names[] = {
    "OK", "CASE_PARSE_ERROR", "CASE_NONCANONICAL", "CASE_UNSUPPORTED_VERSION",
    "CASE_INVALID_PARAMETER", "CASE_IRRATIONAL_SPECTRUM", "CASE_ID_MISMATCH",
    "ERR_ARGUMENT", "ERR_OVERFLOW", "ERR_NONFINITE", "ERR_IO", "ERR_INTERNAL"
};

const char *at0_status_name(at0_status s)
{
    if ((int)s < 0 || (size_t)s >= sizeof status_names / sizeof status_names[0]) return "ERR_UNKNOWN";
    return status_names[s];
}

int at0_status_is_refusal(at0_status s)
{
    return s >= AT0_CASE_PARSE_ERROR && s <= AT0_CASE_ID_MISMATCH;
}

/* ---- 128-bit helpers ------------------------------------------------------ */
static at0_i128 i128_abs(at0_i128 x) { return x < 0 ? -x : x; }

static at0_i128 i128_gcd(at0_i128 a, at0_i128 b)
{
    a = i128_abs(a); b = i128_abs(b);
    while (b != 0) { at0_i128 t = a % b; a = b; b = t; }
    return a;
}

/* reduce n/d (d != 0) and check the contract limits */
static at0_status rat_from_i128(at0_i128 n, at0_i128 d, at0_rat *out)
{
    /* reduce; the only limit here is representability (|n|, d < 2^62) so that every further
     * product of two rationals fits in 128 bits. The contract limit 2^20 applies to tokens
     * written in a case file and is enforced by validity rule 3 (at0_rat_in_limits). */
    if (!out || d == 0) return AT0_ERR_ARGUMENT;
    if (d < 0) { n = -n; d = -d; }
    at0_i128 g = i128_gcd(n, d);
    if (g > 1) { n /= g; d /= g; }
    if (i128_abs(n) > AT0_RAT_REPR_LIMIT || d > AT0_RAT_REPR_LIMIT) return AT0_ERR_OVERFLOW;
    out->n = (int64_t)n; out->d = (int64_t)d;
    return AT0_OK;
}

at0_status at0_rat_make(int64_t n, int64_t d, at0_rat *out) { return rat_from_i128(n, d, out); }
int at0_rat_in_limits(at0_rat a) { return a.d >= 1 && a.d <= AT0_RATIONAL_LIMIT && a.n <= AT0_RATIONAL_LIMIT && a.n >= -AT0_RATIONAL_LIMIT; }
int at0_scaled_in_limits(at0_scaled s) { return s.k >= 0 && s.k <= AT0_SCALED_K_MAX && s.n < (uint64_t)AT0_INT_SATURATED; }

at0_status at0_rat_add(at0_rat a, at0_rat b, at0_rat *out)
{
    return rat_from_i128((at0_i128)a.n * b.d + (at0_i128)b.n * a.d, (at0_i128)a.d * b.d, out);
}
at0_status at0_rat_sub(at0_rat a, at0_rat b, at0_rat *out)
{
    return rat_from_i128((at0_i128)a.n * b.d - (at0_i128)b.n * a.d, (at0_i128)a.d * b.d, out);
}
at0_status at0_rat_mul(at0_rat a, at0_rat b, at0_rat *out)
{
    return rat_from_i128((at0_i128)a.n * b.n, (at0_i128)a.d * b.d, out);
}
int at0_rat_cmp(at0_rat a, at0_rat b)
{
    at0_i128 l = (at0_i128)a.n * b.d, r = (at0_i128)b.n * a.d;
    return l < r ? -1 : (l > r ? 1 : 0);
}
int at0_rat_is_zero(at0_rat a) { return a.n == 0; }
double at0_rat_to_double(at0_rat a) { return (double)a.n / (double)a.d; }

/* integer square root of a nonnegative 128-bit value, exact test */
static int i128_is_square(at0_i128 x, at0_i128 *root)
{
    if (x < 0) return 0;
    if (x == 0) { *root = 0; return 1; }
    at0_i128 r = (at0_i128)sqrtl((long double)x);
    if (r < 0) r = 0;
    while (r > 0 && r * r > x) r--;
    while ((r + 1) * (r + 1) <= x) r++;
    *root = r;
    return r * r == x;
}

at0_status at0_rat_sqrt_exact(at0_rat a, at0_rat *root, int *is_square)
{
    if (!root || !is_square) return AT0_ERR_ARGUMENT;
    *is_square = 0;
    if (a.n < 0) return AT0_OK;
    at0_i128 rn, rd;
    if (!i128_is_square((at0_i128)a.n, &rn)) return AT0_OK;
    if (!i128_is_square((at0_i128)a.d, &rd)) return AT0_OK;
    at0_status st = rat_from_i128(rn, rd, root);
    if (st != AT0_OK) return st;
    *is_square = 1;
    return AT0_OK;
}

at0_status at0_rat_reduce_turn(at0_rat x, at0_rat *out)
{
    /* x - round(x) with round half up, result in [-1/2, 1/2) */
    if (!out) return AT0_ERR_ARGUMENT;
    at0_i128 n = x.n, d = x.d;
    at0_i128 twice = 2 * n + d;               /* 2x + 1 numerator over 2d */
    at0_i128 q = twice / (2 * d);             /* floor for nonnegative */
    if (twice < 0 && twice % (2 * d) != 0) q -= 1;
    return rat_from_i128(n - q * d, d, out);
}

/* ---- canonical integer token -------------------------------------------- */
/* Returns 1 for a canonical integer, 0 for a shape error (rule 1: empty, non-digit, "+",
 * too long) and -1 for a well-formed but noncanonical one (rule 2: leading zeros, "-0").
 * A canonical digit string longer than 18 digits cannot fit the contract limits; it is
 * parsed as the saturation value AT0_INT_SATURATED (above every limit, not divisible by 10)
 * so that rule 3 refuses it as CASE_INVALID_PARAMETER after the version check, and no
 * int64 arithmetic can overflow. */
static int parse_canonical_int(const char *s, size_t len, int64_t *out)
{
    if (len == 0 || len > 64) return 0;
    size_t i = 0; int neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; if (len == 1) return 0; }
    for (size_t j = i; j < len; j++) if (!isdigit((unsigned char)s[j])) return 0;
    int canonical = 1;
    if (s[i] == '0' && len - i > 1) canonical = 0;    /* leading zero */
    if (neg && s[i] == '0') canonical = 0;             /* "-0" (and "-0..." ) */
    int64_t v = 0;
    size_t digits = len - i;
    for (; i < len; i++) if (digits <= 18) v = v * 10 + (s[i] - '0');
    if (digits > 18) v = AT0_INT_SATURATED;
    *out = neg ? -v : v;
    return canonical ? 1 : -1;
}

at0_status at0_rat_parse(const char *tok, at0_rat *out)
{
    if (!tok || !out) return AT0_ERR_ARGUMENT;
    const char *slash = strchr(tok, '/');
    if (!slash) return AT0_CASE_PARSE_ERROR;
    int64_t n, d;
    int cn = parse_canonical_int(tok, (size_t)(slash - tok), &n);
    int cd = parse_canonical_int(slash + 1, strlen(slash + 1), &d);
    if (cn == 0 || cd == 0) return AT0_CASE_PARSE_ERROR;      /* shape first (rule 1) */
    if (cn < 0 || cd < 0) return AT0_CASE_NONCANONICAL;       /* then canonicality (rule 2) */
    if (d < 1) return AT0_CASE_NONCANONICAL;
    int saturated = n == AT0_INT_SATURATED || n == -AT0_INT_SATURATED || d == AT0_INT_SATURATED;
    if (!saturated && i128_gcd(n, d) != 1) return AT0_CASE_NONCANONICAL;   /* covers 0/5 and 2/4; limits are rule 3 */
    out->n = n; out->d = d;
    return AT0_OK;
}

at0_status at0_crat_parse(const char *tok, at0_crat *out)
{
    if (!tok || !out) return AT0_ERR_ARGUMENT;
    size_t len = strlen(tok);
    if (len < 7 || tok[0] != '(' || tok[len - 1] != ')') return AT0_CASE_PARSE_ERROR;
    const char *semi = strchr(tok, ';');
    if (!semi) return AT0_CASE_PARSE_ERROR;
    char a[64], b[64];
    size_t la = (size_t)(semi - tok - 1), lb = len - 2 - la - 1;
    if (la == 0 || lb == 0 || la >= sizeof a || lb >= sizeof b) return AT0_CASE_PARSE_ERROR;
    memcpy(a, tok + 1, la); a[la] = 0;
    memcpy(b, semi + 1, lb); b[lb] = 0;
    at0_status sa = at0_rat_parse(a, &out->re), sb = at0_rat_parse(b, &out->im);
    if (sa == AT0_CASE_PARSE_ERROR || sb == AT0_CASE_PARSE_ERROR) return AT0_CASE_PARSE_ERROR;
    if (sa != AT0_OK) return sa;
    return sb;
}

at0_status at0_scaled_parse(const char *tok, at0_scaled *out)
{
    if (!tok || !out) return AT0_ERR_ARGUMENT;
    const char *at = strchr(tok, '@');
    if (!at) return AT0_CASE_PARSE_ERROR;
    int64_t n, k;
    int cn = parse_canonical_int(tok, (size_t)(at - tok), &n);
    int ck = parse_canonical_int(at + 1, strlen(at + 1), &k);
    if (cn == 0 || ck == 0) return AT0_CASE_PARSE_ERROR;
    if (cn < 0 || ck < 0) return AT0_CASE_NONCANONICAL;
    if (n < 0 || k < 0) return AT0_CASE_NONCANONICAL;
    if (k > 1000) k = 1000;                 /* above every limit; rule 3 refuses it */
    if (n == 0 && k != 0) return AT0_CASE_NONCANONICAL;
    if (n != 0 && k != 0 && n % 10 == 0) return AT0_CASE_NONCANONICAL;
    out->n = (uint64_t)n; out->k = (int)k;
    return AT0_OK;
}

int at0_rat_format(at0_rat a, char *buf, size_t cap)
{
    return snprintf(buf, cap, "%lld/%lld", (long long)a.n, (long long)a.d);
}
int at0_crat_format(at0_crat a, char *buf, size_t cap)
{
    return snprintf(buf, cap, "(%lld/%lld;%lld/%lld)", (long long)a.re.n, (long long)a.re.d,
                    (long long)a.im.n, (long long)a.im.d);
}
int at0_scaled_format(at0_scaled s, char *buf, size_t cap)
{
    return snprintf(buf, cap, "%llu@%d", (unsigned long long)s.n, s.k);
}

/* ---- fixed-size unsigned bignum for exact comparisons -------------------- */
/* 2^1074 (smallest subnormal scale) * 2^53 * 10^40 * 10^40 < 2^1400 ; 48 limbs = 1536 bits */
#define BN_LIMBS 48
typedef struct { uint32_t l[BN_LIMBS]; } bn_t;

static void bn_zero(bn_t *x) { memset(x, 0, sizeof *x); }
static void bn_from_u64(bn_t *x, uint64_t v) { bn_zero(x); x->l[0] = (uint32_t)v; x->l[1] = (uint32_t)(v >> 32); }
static int bn_mul_small(bn_t *x, uint32_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        uint64_t t = (uint64_t)x->l[i] * m + carry;
        x->l[i] = (uint32_t)t; carry = t >> 32;
    }
    return carry == 0;
}
static int bn_shl(bn_t *x, int bits)
{
    while (bits >= 32) {
        if (x->l[BN_LIMBS - 1] != 0) return 0;
        for (int i = BN_LIMBS - 1; i > 0; i--) x->l[i] = x->l[i - 1];
        x->l[0] = 0; bits -= 32;
    }
    if (bits == 0) return 1;
    if (x->l[BN_LIMBS - 1] >> (32 - bits)) return 0;
    for (int i = BN_LIMBS - 1; i > 0; i--) x->l[i] = (x->l[i] << bits) | (x->l[i - 1] >> (32 - bits));
    x->l[0] <<= bits;
    return 1;
}
static int bn_pow10_mul(bn_t *x, int k) { for (int i = 0; i < k; i++) if (!bn_mul_small(x, 10)) return 0; return 1; }
static int bn_add(bn_t *x, const bn_t *y)
{
    uint64_t carry = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        uint64_t t = (uint64_t)x->l[i] + y->l[i] + carry;
        x->l[i] = (uint32_t)t; carry = t >> 32;
    }
    return carry == 0;
}
static int bn_cmp(const bn_t *a, const bn_t *b)
{
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->l[i] != b->l[i]) return a->l[i] < b->l[i] ? -1 : 1;
    }
    return 0;
}

at0_status at0_exact_prob_status(double p, at0_scaled b, at0_scaled tol, int *le, int *gt)
{
    if (!le || !gt) return AT0_ERR_ARGUMENT;
    if (!isfinite(p) || p < 0) return AT0_ERR_ARGUMENT;
    if (b.k > AT0_SCALED_K_MAX || tol.k > AT0_SCALED_K_MAX || b.k < 0 || tol.k < 0) return AT0_ERR_ARGUMENT;
    /* p = m * 2^e exactly, m integer < 2^53 */
    int e = 0; uint64_t m = 0;
    if (p != 0.0) {
        int ex; double fr = frexp(p, &ex);        /* p = fr * 2^ex, fr in [0.5,1) */
        m = (uint64_t)ldexp(fr, 53);               /* exact integer */
        e = ex - 53;
    }
    int s = e < 0 ? -e : 0;                        /* scale by 2^s so p's exponent is >= 0 */
    int K = b.k > tol.k ? b.k : tol.k;
    bn_t P, B, T;
    bn_from_u64(&P, m);
    if (!bn_shl(&P, s + e) || !bn_pow10_mul(&P, K)) return AT0_ERR_OVERFLOW;
    bn_from_u64(&B, b.n);
    if (!bn_shl(&B, s) || !bn_pow10_mul(&B, K - b.k)) return AT0_ERR_OVERFLOW;
    bn_from_u64(&T, tol.n);
    if (!bn_shl(&T, s) || !bn_pow10_mul(&T, K - tol.k)) return AT0_ERR_OVERFLOW;
    bn_t PB = P; if (!bn_add(&PB, &B)) return AT0_ERR_OVERFLOW;   /* p + b */
    bn_t TB = T; if (!bn_add(&TB, &B)) return AT0_ERR_OVERFLOW;   /* tol + b */
    *le = bn_cmp(&PB, &T) <= 0;                                   /* p + b <= tol */
    *gt = bn_cmp(&P, &TB) > 0;                                    /* p - b > tol  <=> p > tol + b */
    return AT0_OK;
}

at0_status at0_scaled_from_double_ceil(double x, at0_scaled *out)
{
    if (!out || !isfinite(x) || x < 0) return AT0_ERR_ARGUMENT;
    if (x == 0.0) { out->n = 0; out->k = 0; return AT0_OK; }
    /* pick k so that x * 10^k is in [10^6, 10^7): seven significant digits, round up, then add one unit */
    int k = 0; long double y = (long double)x;
    while (y < 1e6L && k < AT0_SCALED_K_MAX) { y *= 10.0L; k++; }
    while (y >= 1e7L && k > 0) { y /= 10.0L; k--; }
    if (y >= 1.8e19L) return AT0_ERR_OVERFLOW;
    uint64_t n = (uint64_t)ceill(y) + 1;           /* +1 absorbs the decimal conversion itself */
    while (k > 0 && n % 10 == 0) { n /= 10; k--; }  /* canonical form */
    out->n = n; out->k = k;
    return AT0_OK;
}

/* ---- overflow-free exact tests on in-limit case data ------------------------ */
/* Every token of a valid case satisfies rule 3 (|n|, d <= 2^20), but values derived from
 * several tokens with different denominators (|h|, |h| + hz, eigenvector overlaps) have
 * reduced denominators far beyond 2^62, so they must never pass through at0_rat. The
 * functions below keep every intermediate as an unreduced 128-bit integer whose bound is
 * stated at the use site, and verify each product and sum with checked arithmetic:
 * an exceeded bound is reported as AT0_ERR_OVERFLOW, never wrapped. */

int at0_i128_mul(at0_i128 a, at0_i128 b, at0_i128 *out) { return !__builtin_mul_overflow(a, b, out); }
int at0_i128_add(at0_i128 a, at0_i128 b, at0_i128 *out) { return !__builtin_add_overflow(a, b, out); }

static int rat_in_token_limits(at0_rat a) { return at0_rat_in_limits(a); }

at0_status at0_rat_norm_exact(at0_rat hx, at0_rat hy, at0_rat hz, at0_rat *norm, int *is_square)
{
    /* |h|^2 = (nx^2 (dy dz)^2 + ny^2 (dx dz)^2 + nz^2 (dx dy)^2) / (dx dy dz)^2.
     * With tokens <= 2^20: each term <= 2^40 * 2^80 = 2^120, the sum <= 3 * 2^120 < 2^122, D = dx dy dz <= 2^60.
     * The denominator is a perfect square, so |h| is rational iff the numerator is a perfect
     * square S^2, and then |h| = S / D with S < 2^61 (representable). */
    if (!norm || !is_square) return AT0_ERR_ARGUMENT;
    if (!rat_in_token_limits(hx) || !rat_in_token_limits(hy) || !rat_in_token_limits(hz)) return AT0_ERR_OVERFLOW;
    *is_square = 0;
    at0_i128 dx = hx.d, dy = hy.d, dz = hz.d, D, t, term, num = 0;
    if (!at0_i128_mul(dx, dy, &t) || !at0_i128_mul(t, dz, &D)) return AT0_ERR_OVERFLOW;
    const at0_i128 n[3] = { hx.n, hy.n, hz.n }, other[3] = { dy * dz, dx * dz, dx * dy };
    for (int i = 0; i < 3; i++) {
        if (!at0_i128_mul(n[i], other[i], &t)) return AT0_ERR_OVERFLOW;     /* n_i * (D / d_i) */
        if (!at0_i128_mul(t, t, &term)) return AT0_ERR_OVERFLOW;
        if (!at0_i128_add(num, term, &num)) return AT0_ERR_OVERFLOW;
    }
    at0_i128 S;
    if (!i128_is_square(num, &S)) return AT0_OK;
    at0_status st = rat_from_i128(S, D, norm);
    if (st != AT0_OK) return st;
    *is_square = 1;
    return AT0_OK;
}

at0_status at0_exact_norm_plus_hz(at0_rat hx, at0_rat hy, at0_rat hz, at0_rat h_norm,
                                  at0_i128 *A, at0_i128 *D)
{
    /* a = |h| + hz = (S + nz dx dy) / D with D = dx dy dz and |h| = S / D unreduced:
     * S <= sqrt3 * 2^60 < 2^61 and |nz dx dy| <= 2^60, so |A| < 2^62. h_norm is the reduced S / D, hence
     * h_norm.d divides D and S = h_norm.n * (D / h_norm.d). */
    if (!A || !D) return AT0_ERR_ARGUMENT;
    if (!rat_in_token_limits(hx) || !rat_in_token_limits(hy) || !rat_in_token_limits(hz)) return AT0_ERR_OVERFLOW;
    at0_i128 dx = hx.d, dy = hy.d, dz = hz.d, t, d, S, z;
    if (!at0_i128_mul(dx, dy, &t) || !at0_i128_mul(t, dz, &d)) return AT0_ERR_OVERFLOW;
    if (h_norm.d < 1 || d % h_norm.d != 0) return AT0_ERR_INTERNAL;
    if (!at0_i128_mul((at0_i128)h_norm.n, d / h_norm.d, &S)) return AT0_ERR_OVERFLOW;
    if (!at0_i128_mul((at0_i128)hz.n, dx * dy, &z)) return AT0_ERR_OVERFLOW;
    if (!at0_i128_add(S, z, A)) return AT0_ERR_OVERFLOW;
    *D = d;
    return AT0_OK;
}

at0_status at0_exact_sum_is_zero(int count, const at0_i128 *num, const at0_i128 *den, int *zero)
{
    /* sum_i num[i]/den[i] == 0  <=>  sum_i num[i] * prod_{j != i} den[j] == 0.
     * Terms with a zero numerator are dropped first so that the products only involve the
     * denominators of the terms that can contribute. The caller states the bound. */
    if (!num || !den || !zero || count < 0 || count > 4) return AT0_ERR_ARGUMENT;
    at0_i128 n[4], d[4]; int m = 0;
    for (int i = 0; i < count; i++) {
        if (den[i] < 1) return AT0_ERR_ARGUMENT;
        if (num[i] != 0) { n[m] = num[i]; d[m] = den[i]; m++; }
    }
    at0_i128 acc = 0;
    for (int i = 0; i < m; i++) {
        at0_i128 t = n[i];
        for (int j = 0; j < m; j++) if (j != i && !at0_i128_mul(t, d[j], &t)) return AT0_ERR_OVERFLOW;
        if (!at0_i128_add(acc, t, &acc)) return AT0_ERR_OVERFLOW;
    }
    *zero = acc == 0;
    return AT0_OK;
}
