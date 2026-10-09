/* Exact arithmetic for the AT-0 evaluator: int128 rationals, scaled decimals,
 * 4096-bit integers, and the two tri-state comparison forms of AT0_RESULT_V1
 * section 4. Nothing here touches floating point except the bit-pattern
 * decoder sbig_from_f64_bits, which only reads integer fields of the pattern. */
#include "at0e.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *m) { fprintf(stderr, "at0e: internal: %s\n", m); abort(); }

/* ---------------- rationals ---------------- */
static i128 iabs(i128 a) { return a < 0 ? -a : a; }
static i128 igcd(i128 a, i128 b) { a = iabs(a); b = iabs(b); while (b) { i128 t = a % b; a = b; b = t; } return a; }

rat rat_make(i128 n, i128 d) {
    rat r;
    if (d == 0) die("rational with zero denominator");
    if (d < 0) { n = -n; d = -d; }
    i128 g = igcd(n, d);
    if (g == 0) g = 1;
    r.n = n / g; r.d = d / g;
    return r;
}
rat rat_add(rat a, rat b) { return rat_make(a.n * b.d + b.n * a.d, a.d * b.d); }
rat rat_sub(rat a, rat b) { return rat_make(a.n * b.d - b.n * a.d, a.d * b.d); }
rat rat_mul(rat a, rat b) { return rat_make(a.n * b.n, a.d * b.d); }
rat rat_neg(rat a) { return rat_make(-a.n, a.d); }
int rat_cmp(rat a, rat b) { i128 l = a.n * b.d, r = b.n * a.d; return (l > r) - (l < r); }
int rat_is_zero(rat a) { return a.n == 0; }
int rat_is_int(rat a) { return a.d == 1; }
rat rat_mod1(rat a) {
    i128 q = a.n / a.d;             /* truncates toward zero */
    i128 rem = a.n - q * a.d;
    if (rem < 0) rem += a.d;
    return rat_make(rem, a.d);
}
static u128 isqrt_u128(u128 v) {
    if (v < 2) return v;
    u128 lo = 1, hi = (u128)1 << 64;  /* v < 2^128 so sqrt < 2^64 */
    while (lo < hi) {
        u128 mid = lo + (hi - lo + 1) / 2;
        if (mid <= v / mid) lo = mid; else hi = mid - 1;
    }
    return lo;
}
int rat_sqrt(rat a, rat *out) {
    if (a.n < 0) return 0;
    u128 n = (u128)a.n, d = (u128)a.d;
    u128 sn = isqrt_u128(n), sd = isqrt_u128(d);
    if (sn * sn != n || sd * sd != d) return 0;
    *out = rat_make((i128)sn, (i128)sd);
    return 1;
}

/* Canonical integer text: optional '-', no leading zeros, no '+', "-0" invalid. */
static int parse_int_text(const char *s, size_t len, i128 *out, int *canonical) {
    size_t i = 0; int neg = 0;
    if (len == 0) return 0;
    if (s[0] == '-') { neg = 1; i = 1; if (len == 1) return 0; }
    if (s[i] == '+') return 0;
    i128 v = 0;
    for (size_t j = i; j < len; j++) {
        if (s[j] < '0' || s[j] > '9') return 0;
        if (j - i >= 20) { v = (i128)1 << 100; if (*canonical == 1) *canonical = -1; continue; }  /* beyond any contract limit: saturate (no overflow) and flag; rat_parse refuses it as CASE_INVALID_PARAMETER */
        v = v * 10 + (s[j] - '0');
    }
    if (len - i > 1 && s[i] == '0') *canonical = 0;   /* leading zero */
    if (neg && v == 0) *canonical = 0;                /* -0 */
    *out = neg ? -v : v;
    return 1;
}
int parse_int_canonical(const char *s, long *out) {
    i128 v; int canon = 1;
    if (!parse_int_text(s, strlen(s), &v, &canon) || !canon) return 0;
    if (v > 1000000000000L || v < -1000000000000L) return 0;
    *out = (long)v;
    return 1;
}
int rat_parse(const char *tok, rat *out, int *canonical) {
    const char *slash = strchr(tok, '/');
    i128 n, d; int canon = 1;
    if (!slash) return 0;
    if (!parse_int_text(tok, (size_t)(slash - tok), &n, &canon)) return 0;
    if (!parse_int_text(slash + 1, strlen(slash + 1), &d, &canon)) return 0;
    if (d < 1) return 0;                                   /* "d >= 1" is shape */
    int huge = canon == -1;                                /* a part saturated in parse_int_text: beyond any limit, reduction unknowable */
    if (!huge) {
        if (igcd(n, d) != 1) canon = 0;
        if (n == 0 && d != 1) canon = 0;
    }
    if (canon != 0 && (huge || iabs(n) > AT0E_RAT_LIMIT || d > AT0E_RAT_LIMIT)) canon = -1; /* beyond the section 2 limits: INVALID_PARAMETER (ambiguity A2); encoding errors still win */
    *canonical = canon;
    *out = rat_make(n, d);
    return 1;
}
void rat_format(rat a, char *buf, size_t cap) {
    long long n = (long long)a.n, d = (long long)a.d;
    snprintf(buf, cap, "%lld/%lld", n, d);
}
int crat_parse(const char *tok, crat *out, int *canonical) {
    size_t len = strlen(tok);
    if (len < 5 || tok[0] != '(' || tok[len - 1] != ')') return 0;
    const char *semi = strchr(tok, ';');
    if (!semi) return 0;
    char re[64], im[64];
    size_t rl = (size_t)(semi - tok - 1), il = len - 2 - rl - 1;
    if (rl == 0 || rl >= sizeof re || il == 0 || il >= sizeof im) return 0;
    memcpy(re, tok + 1, rl); re[rl] = 0;
    memcpy(im, semi + 1, il); im[il] = 0;
    int c1 = 1, c2 = 1;
    if (!rat_parse(re, &out->re, &c1) || !rat_parse(im, &out->im, &c2)) return 0;
    *canonical = (c1 < 0 || c2 < 0) ? -1 : (c1 && c2);
    return 1;
}
int crat_is_zero(crat z) { return rat_is_zero(z.re) && rat_is_zero(z.im); }

/* ---------------- scaled decimals ---------------- */
int scaled_parse(const char *tok, scaled *out, int *canonical) {
    const char *at = strchr(tok, '@');
    if (!at) return 0;
    size_t nl = (size_t)(at - tok);
    if (nl == 0 || nl > 38) return 0;
    u128 n = 0;
    for (size_t i = 0; i < nl; i++) { if (tok[i] < '0' || tok[i] > '9') return 0; n = n * 10 + (u128)(tok[i] - '0'); }
    int canon = 1;
    if (nl > 1 && tok[0] == '0') canon = 0;
    const char *ks = at + 1; size_t kl = strlen(ks);
    if (kl == 0 || kl > 2) return 0;
    int k = 0;
    for (size_t i = 0; i < kl; i++) { if (ks[i] < '0' || ks[i] > '9') return 0; k = k * 10 + (ks[i] - '0'); }
    if (kl == 2 && ks[0] == '0') canon = 0;
    if (k > 40) return 0;
    if (n == 0 && k != 0) canon = 0;
    if (n != 0 && k != 0 && n % 10 == 0) canon = 0;
    *canonical = canon;
    out->n = n; out->k = k;
    return 1;
}

/* ---------------- big integers ---------------- */
void big_zero(big *a) { memset(a, 0, sizeof *a); }
void big_from_u128(big *a, u128 v) { big_zero(a); for (int i = 0; i < 4; i++) { a->l[i] = (uint32_t)v; v >>= 32; } }
int big_is_zero(const big *a) { for (int i = 0; i < BIG_LIMBS; i++) if (a->l[i]) return 0; return 1; }
int big_cmp(const big *a, const big *b) {
    for (int i = BIG_LIMBS - 1; i >= 0; i--) { if (a->l[i] != b->l[i]) return a->l[i] > b->l[i] ? 1 : -1; }
    return 0;
}
void big_add(big *r, const big *a, const big *b) {
    uint64_t carry = 0;
    for (int i = 0; i < BIG_LIMBS; i++) { uint64_t s = (uint64_t)a->l[i] + b->l[i] + carry; r->l[i] = (uint32_t)s; carry = s >> 32; }
    if (carry) die("big_add overflow");
}
void big_sub(big *r, const big *a, const big *b) {
    int64_t borrow = 0;
    for (int i = 0; i < BIG_LIMBS; i++) {
        int64_t s = (int64_t)a->l[i] - b->l[i] - borrow;
        if (s < 0) { s += (int64_t)1 << 32; borrow = 1; } else borrow = 0;
        r->l[i] = (uint32_t)s;
    }
    if (borrow) die("big_sub underflow");
}
void big_shl(big *a, unsigned bits) {
    unsigned limbs = bits / 32, rem = bits % 32;
    if (limbs >= BIG_LIMBS) { if (!big_is_zero(a)) die("big_shl overflow"); return; }
    for (unsigned i = BIG_LIMBS - limbs; i < BIG_LIMBS; i++) if (a->l[i]) die("big_shl overflow");
    if (limbs) {
        for (int i = BIG_LIMBS - 1; i >= (int)limbs; i--) a->l[i] = a->l[i - limbs];
        for (unsigned i = 0; i < limbs; i++) a->l[i] = 0;
    }
    if (rem) {
        uint32_t carry = 0;
        for (int i = 0; i < BIG_LIMBS; i++) { uint64_t v = ((uint64_t)a->l[i] << rem) | carry; a->l[i] = (uint32_t)v; carry = (uint32_t)(v >> 32); }
        if (carry) die("big_shl overflow");
    }
}
void big_mul_u32(big *a, uint32_t m) {
    uint64_t carry = 0;
    for (int i = 0; i < BIG_LIMBS; i++) { uint64_t v = (uint64_t)a->l[i] * m + carry; a->l[i] = (uint32_t)v; carry = v >> 32; }
    if (carry) die("big_mul overflow");
}
void big_mul_pow10(big *a, unsigned k) { while (k >= 9) { big_mul_u32(a, 1000000000u); k -= 9; } while (k--) big_mul_u32(a, 10u); }

void sbig_zero(sbig *a) { a->neg = 0; big_zero(&a->m); }
void sbig_neg(sbig *a) { if (!big_is_zero(&a->m)) a->neg = !a->neg; }
void sbig_abs(sbig *a) { a->neg = 0; }
void sbig_add(sbig *r, const sbig *a, const sbig *b) {
    sbig t;
    if (a->neg == b->neg) { big_add(&t.m, &a->m, &b->m); t.neg = a->neg; }
    else {
        int c = big_cmp(&a->m, &b->m);
        if (c == 0) { sbig_zero(&t); }
        else if (c > 0) { big_sub(&t.m, &a->m, &b->m); t.neg = a->neg; }
        else { big_sub(&t.m, &b->m, &a->m); t.neg = b->neg; }
    }
    if (big_is_zero(&t.m)) t.neg = 0;
    *r = t;
}
int sbig_cmp(const sbig *a, const sbig *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = big_cmp(&a->m, &b->m);
    return a->neg ? -c : c;
}

/* Q = v * 2^1100 * 10^80 for a finite binary64 v = (-1)^s * m * 2^e. */
void sbig_from_f64_bits(sbig *q, uint64_t bits) {
    int sign = (int)(bits >> 63);
    int expo = (int)((bits >> 52) & 0x7ff);
    uint64_t frac = bits & 0xfffffffffffffULL;
    uint64_t m; int e;
    if (expo == 0x7ff) die("nonfinite bit pattern reached exact arithmetic");
    if (expo == 0) { m = frac; e = -1074; }
    else { m = frac | (1ULL << 52); e = expo - 1075; }
    sbig_zero(q);
    if (m == 0) return;
    big_from_u128(&q->m, m);
    big_shl(&q->m, (unsigned)(e + (int)AT0E_DEN_P2));
    big_mul_pow10(&q->m, AT0E_DEN_P10);
    q->neg = sign;
}
void sbig_from_scaled(sbig *q, scaled s) {
    sbig_zero(q);
    if (s.n == 0) return;
    big_from_u128(&q->m, s.n);
    big_mul_pow10(&q->m, AT0E_DEN_P10 - (unsigned)s.k);
    big_shl(&q->m, AT0E_DEN_P2);
}
void sbig_from_int(sbig *q, long v) {
    sbig_zero(q);
    if (v == 0) return;
    big_from_u128(&q->m, (u128)(v < 0 ? -v : v));
    big_mul_pow10(&q->m, AT0E_DEN_P10);
    big_shl(&q->m, AT0E_DEN_P2);
    q->neg = v < 0;
}

tri tri_absolute(const sbig *v, const sbig *b, const sbig *tol) {
    sbig av = *v, hi, lo, nb = *b;
    sbig_abs(&av);
    sbig_add(&hi, &av, b);
    if (sbig_cmp(&hi, tol) <= 0) return CMP_PASS;
    sbig_neg(&nb);
    sbig_add(&lo, &av, &nb);
    if (sbig_cmp(&lo, tol) > 0) return CMP_FAIL;
    return CMP_INDET;
}
tri tri_signed(const sbig *v, const sbig *b, const sbig *tol) {
    sbig hi, lo, nb = *b;
    sbig_add(&hi, v, b);
    if (sbig_cmp(&hi, tol) <= 0) return CMP_PASS;
    sbig_neg(&nb);
    sbig_add(&lo, v, &nb);
    if (sbig_cmp(&lo, tol) > 0) return CMP_FAIL;
    return CMP_INDET;
}
