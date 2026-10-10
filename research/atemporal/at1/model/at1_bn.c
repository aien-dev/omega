/* Exact arithmetic for the AT-1 engine: arbitrary-precision integers (32-bit limbs, Knuth
 * algorithm D for division), reduced rationals and Gaussian rationals. Used for validation
 * (rules 3 and 4 of AT1_CASE_V1 section 4 are decided exactly for every in-limit case, as
 * section 2 requires), for the exact nullspace of the literal H_total, and for the exact
 * verdict comparisons of AT0_RESULT_V2 section 4. Nothing here ever wraps around. */
#include "at1_model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *at1_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("AT1_ENGINE_ERROR RESOURCE_LIMIT\n", stderr); exit(1); }
    return p;
}
void *at1_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fputs("AT1_ENGINE_ERROR RESOURCE_LIMIT\n", stderr); exit(1); }
    return p;
}
void *at1_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("AT1_ENGINE_ERROR RESOURCE_LIMIT\n", stderr); exit(1); }
    return q;
}

/* ---- integers ------------------------------------------------------------------------------ */
void bn_init(bn *a) { a->neg = 0; a->n = 0; a->cap = 0; a->d = NULL; }
void bn_free(bn *a) { free(a->d); bn_init(a); }

static void bn_reserve(bn *a, int n)
{
    if (a->cap >= n) return;
    int cap = a->cap ? a->cap : 4;
    while (cap < n) cap *= 2;
    a->d = at1_xrealloc(a->d, (size_t)cap * sizeof(uint32_t));
    a->cap = cap;
}
static void bn_trim(bn *a)
{
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
    if (a->n == 0) a->neg = 0;
}
static void bn_swap(bn *a, bn *b) { bn t = *a; *a = *b; *b = t; }

void bn_copy(bn *r, const bn *a)
{
    if (r == a) return;
    bn_reserve(r, a->n);
    if (a->n) memcpy(r->d, a->d, (size_t)a->n * sizeof(uint32_t));
    r->n = a->n; r->neg = a->neg;
}
void bn_set_u64(bn *r, uint64_t v)
{
    bn_reserve(r, 2);
    r->d[0] = (uint32_t)v; r->d[1] = (uint32_t)(v >> 32); r->n = 2; r->neg = 0;
    bn_trim(r);
}
void bn_set_i64(bn *r, int64_t v)
{
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    bn_set_u64(r, m);
    r->neg = v < 0 && m != 0;
}
int bn_is_zero(const bn *a) { return a->n == 0; }
int bn_sign(const bn *a) { return a->n == 0 ? 0 : a->neg ? -1 : 1; }

static int mag_cmp(const uint32_t *a, int an, const uint32_t *b, int bn_)
{
    if (an != bn_) return an < bn_ ? -1 : 1;
    for (int i = an - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
int bn_cmp_abs(const bn *a, const bn *b) { return mag_cmp(a->d, a->n, b->d, b->n); }
int bn_cmp(const bn *a, const bn *b)
{
    int sa = bn_sign(a), sb = bn_sign(b);
    if (sa != sb) return sa < sb ? -1 : 1;
    int c = bn_cmp_abs(a, b);
    return sa < 0 ? -c : c;
}

/* r = |a| + |b| (magnitudes), sign set by caller */
static void mag_add(bn *r, const bn *a, const bn *b)
{
    bn t; bn_init(&t);
    int n = (a->n > b->n ? a->n : b->n) + 1;
    bn_reserve(&t, n);
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (i < a->n ? a->d[i] : 0u);
        c += (i < b->n ? b->d[i] : 0u);
        t.d[i] = (uint32_t)c; c >>= 32;
    }
    t.n = n; t.neg = 0; bn_trim(&t);
    bn_swap(r, &t); bn_free(&t);
}
/* r = |a| - |b|, requires |a| >= |b| */
static void mag_sub(bn *r, const bn *a, const bn *b)
{
    bn t; bn_init(&t);
    bn_reserve(&t, a->n);
    int64_t br = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t x = (int64_t)a->d[i] - (i < b->n ? (int64_t)b->d[i] : 0) - br;
        br = x < 0;
        if (x < 0) x += (int64_t)1 << 32;
        t.d[i] = (uint32_t)x;
    }
    t.n = a->n; t.neg = 0; bn_trim(&t);
    bn_swap(r, &t); bn_free(&t);
}
void bn_add(bn *r, const bn *a, const bn *b)
{
    int an = a->neg, bneg = b->neg;
    if (an == bneg) { mag_add(r, a, b); r->neg = an && r->n; return; }
    int c = bn_cmp_abs(a, b);
    if (c == 0) { r->n = 0; r->neg = 0; return; }
    if (c > 0) { mag_sub(r, a, b); r->neg = an && r->n; }
    else { mag_sub(r, b, a); r->neg = bneg && r->n; }
}
void bn_neg(bn *r, const bn *a) { bn_copy(r, a); if (r->n) r->neg = !r->neg; }
void bn_abs(bn *r, const bn *a) { bn_copy(r, a); r->neg = 0; }
void bn_sub(bn *r, const bn *a, const bn *b)
{
    bn nb; bn_init(&nb); bn_neg(&nb, b);
    bn_add(r, a, &nb);
    bn_free(&nb);
}
void bn_mul(bn *r, const bn *a, const bn *b)
{
    if (a->n == 0 || b->n == 0) { r->n = 0; r->neg = 0; return; }
    bn t; bn_init(&t);
    int n = a->n + b->n;
    bn_reserve(&t, n);
    memset(t.d, 0, (size_t)n * sizeof(uint32_t));
    for (int i = 0; i < a->n; i++) {
        uint64_t c = 0, ai = a->d[i];
        for (int j = 0; j < b->n; j++) {
            c += ai * b->d[j] + t.d[i + j];
            t.d[i + j] = (uint32_t)c; c >>= 32;
        }
        t.d[i + b->n] = (uint32_t)c;
    }
    t.n = n; t.neg = a->neg != b->neg; bn_trim(&t);
    bn_swap(r, &t); bn_free(&t);
}

static int clz32(uint32_t x) { int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }

/* Knuth TAOCP vol. 2, 4.3.1 algorithm D, on magnitudes; truncated signs applied after */
void bn_divmod(bn *q, bn *rem, const bn *a, const bn *b)
{
    if (b->n == 0) { fputs("AT1_ENGINE_ERROR INTERNAL_ERROR division by zero\n", stderr); exit(1); }
    int qneg = a->neg != b->neg, rneg = a->neg;
    if (bn_cmp_abs(a, b) < 0) {
        if (rem) { bn_copy(rem, a); }
        if (q) { q->n = 0; q->neg = 0; }
        return;
    }
    bn Q, R; bn_init(&Q); bn_init(&R);
    if (b->n == 1) {
        uint64_t d = b->d[0], r = 0;
        bn_reserve(&Q, a->n);
        for (int i = a->n - 1; i >= 0; i--) {
            uint64_t cur = (r << 32) | a->d[i];
            Q.d[i] = (uint32_t)(cur / d); r = cur % d;
        }
        Q.n = a->n; bn_trim(&Q);
        bn_set_u64(&R, r);
    } else {
        int s = clz32(b->d[b->n - 1]);
        int n = b->n, m = a->n - b->n;
        uint32_t *v = at1_xmalloc((size_t)n * sizeof(uint32_t));
        uint32_t *u = at1_xmalloc((size_t)(a->n + 1) * sizeof(uint32_t));
        for (int i = n - 1; i > 0; i--) v[i] = (b->d[i] << s) | (s ? (uint32_t)((uint64_t)b->d[i - 1] >> (32 - s)) : 0);
        v[0] = b->d[0] << s;
        u[a->n] = s ? (uint32_t)((uint64_t)a->d[a->n - 1] >> (32 - s)) : 0;
        for (int i = a->n - 1; i > 0; i--) u[i] = (a->d[i] << s) | (s ? (uint32_t)((uint64_t)a->d[i - 1] >> (32 - s)) : 0);
        u[0] = a->d[0] << s;
        bn_reserve(&Q, m + 1);
        for (int j = m; j >= 0; j--) {
            uint64_t num = ((uint64_t)u[j + n] << 32) | u[j + n - 1];
            uint64_t qhat = num / v[n - 1], rhat = num % v[n - 1];
            while (qhat >= ((uint64_t)1 << 32) || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
                qhat--; rhat += v[n - 1];
                if (rhat >= ((uint64_t)1 << 32)) break;
            }
            int64_t borrow = 0; uint64_t carry = 0;
            for (int i = 0; i < n; i++) {
                uint64_t p = qhat * v[i] + carry;
                carry = p >> 32;
                int64_t t = (int64_t)u[i + j] - (int64_t)(uint32_t)p - borrow;
                borrow = t < 0;
                u[i + j] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
            }
            int64_t t = (int64_t)u[j + n] - (int64_t)carry - borrow;
            borrow = t < 0;
            u[j + n] = (uint32_t)(t + (borrow ? ((int64_t)1 << 32) : 0));
            if (borrow) {
                qhat--;
                uint64_t c = 0;
                for (int i = 0; i < n; i++) {
                    c += (uint64_t)u[i + j] + v[i];
                    u[i + j] = (uint32_t)c; c >>= 32;
                }
                u[j + n] = (uint32_t)((uint64_t)u[j + n] + c);
            }
            Q.d[j] = (uint32_t)qhat;
        }
        Q.n = m + 1; bn_trim(&Q);
        bn_reserve(&R, n);
        for (int i = 0; i < n; i++) R.d[i] = (u[i] >> s) | (s ? (uint32_t)((uint64_t)u[i + 1] << (32 - s)) : 0);
        R.n = n; bn_trim(&R);
        free(u); free(v);
    }
    Q.neg = qneg && Q.n; R.neg = rneg && R.n;
    if (q) bn_swap(q, &Q);
    if (rem) bn_swap(rem, &R);
    bn_free(&Q); bn_free(&R);
}

void bn_gcd(bn *r, const bn *a, const bn *b)
{
    bn x, y, t; bn_init(&x); bn_init(&y); bn_init(&t);
    bn_abs(&x, a); bn_abs(&y, b);
    while (y.n) { bn_divmod(NULL, &t, &x, &y); bn_swap(&x, &y); bn_swap(&y, &t); }
    bn_swap(r, &x);
    bn_free(&x); bn_free(&y); bn_free(&t);
}

void bn_shl(bn *r, const bn *a, unsigned bits)
{
    if (a->n == 0) { r->n = 0; r->neg = 0; return; }
    unsigned ws = bits / 32, bs = bits % 32;
    bn t; bn_init(&t);
    int n = a->n + (int)ws + 1;
    bn_reserve(&t, n);
    memset(t.d, 0, (size_t)n * sizeof(uint32_t));
    for (int i = 0; i < a->n; i++) {
        uint64_t x = (uint64_t)a->d[i] << bs;
        t.d[i + (int)ws] |= (uint32_t)x;
        t.d[i + (int)ws + 1] |= (uint32_t)(x >> 32);
    }
    t.n = n; t.neg = a->neg; bn_trim(&t);
    bn_swap(r, &t); bn_free(&t);
}

void bn_pow10(bn *r, unsigned k)
{
    bn ten; bn_init(&ten); bn_set_u64(&ten, 10);
    bn_set_u64(r, 1);
    for (unsigned i = 0; i < k; i++) bn_mul(r, r, &ten);
    bn_free(&ten);
}

int bn_bitlen(const bn *a)
{
    if (a->n == 0) return 0;
    return 32 * (a->n - 1) + (32 - clz32(a->d[a->n - 1]));
}

int bn_isqrt_exact(const bn *a, bn *root)
{
    if (a->neg) return 0;
    if (a->n == 0) { root->n = 0; root->neg = 0; return 1; }
    /* Newton from above: x0 = 2^ceil(bits/2) >= sqrt(a) */
    bn x, y, t, one, two; bn_init(&x); bn_init(&y); bn_init(&t); bn_init(&one); bn_init(&two);
    bn_set_u64(&one, 1); bn_set_u64(&two, 2);
    bn_shl(&x, &one, (unsigned)(bn_bitlen(a) + 1) / 2 + 1);
    for (;;) {
        bn_divmod(&t, NULL, a, &x);
        bn_add(&y, &x, &t);
        bn_divmod(&y, NULL, &y, &two);
        if (bn_cmp(&y, &x) >= 0) break;
        bn_swap(&x, &y);
    }
    bn_mul(&t, &x, &x);
    int sq = bn_cmp(&t, a) == 0;
    if (sq) bn_copy(root, &x);
    bn_free(&x); bn_free(&y); bn_free(&t); bn_free(&one); bn_free(&two);
    return sq;
}

int bn_fits_i64(const bn *a, int64_t *out)
{
    if (a->n > 2) return 0;
    uint64_t m = a->n == 0 ? 0 : a->n == 1 ? a->d[0] : ((uint64_t)a->d[1] << 32) | a->d[0];
    if (m > (uint64_t)INT64_MAX) return 0;
    *out = a->neg ? -(int64_t)m : (int64_t)m;
    return 1;
}

int bn_from_digits(bn *r, const char *s, size_t len)
{
    if (len > AT1_TOKEN_DIGITS_MAX) return 0;
    bn t, chunk, scale; bn_init(&t); bn_init(&chunk); bn_init(&scale);
    size_t i = 0;
    while (i < len) {
        size_t take = len - i < 9 ? len - i : 9;
        uint64_t v = 0, sc = 1;
        for (size_t j = 0; j < take; j++) { v = v * 10 + (uint64_t)(s[i + j] - '0'); sc *= 10; }
        bn_set_u64(&scale, sc); bn_set_u64(&chunk, v);
        bn_mul(&t, &t, &scale); bn_add(&t, &t, &chunk);
        i += take;
    }
    bn_swap(r, &t);
    bn_free(&t); bn_free(&chunk); bn_free(&scale);
    return 1;
}

/* ---- rationals ------------------------------------------------------------------------------ */
void bq_init(bq *a) { bn_init(&a->num); bn_init(&a->den); bn_set_u64(&a->den, 1); }
void bq_free(bq *a) { bn_free(&a->num); bn_free(&a->den); }
void bq_copy(bq *r, const bq *a) { bn_copy(&r->num, &a->num); bn_copy(&r->den, &a->den); }

static void bq_reduce(bq *r)
{
    if (r->den.neg) { bn_neg(&r->den, &r->den); bn_neg(&r->num, &r->num); }
    if (r->num.n == 0) { bn_set_u64(&r->den, 1); return; }
    bn g; bn_init(&g);
    bn_gcd(&g, &r->num, &r->den);
    if (!(g.n == 1 && g.d[0] == 1)) {
        bn_divmod(&r->num, NULL, &r->num, &g);
        bn_divmod(&r->den, NULL, &r->den, &g);
    }
    bn_free(&g);
}
void bq_set_bn(bq *r, const bn *n, const bn *d) { bn_copy(&r->num, n); bn_copy(&r->den, d); bq_reduce(r); }
void bq_set_i64(bq *r, int64_t n, int64_t d) { bn_set_i64(&r->num, n); bn_set_i64(&r->den, d); bq_reduce(r); }
int bq_is_zero(const bq *a) { return a->num.n == 0; }
int bq_sign(const bq *a) { return bn_sign(&a->num); }
int bq_cmp(const bq *a, const bq *b)
{
    bn x, y; bn_init(&x); bn_init(&y);
    bn_mul(&x, &a->num, &b->den); bn_mul(&y, &b->num, &a->den);
    int c = bn_cmp(&x, &y);
    bn_free(&x); bn_free(&y);
    return c;
}
void bq_add(bq *r, const bq *a, const bq *b)
{
    bn x, y, d; bn_init(&x); bn_init(&y); bn_init(&d);
    bn_mul(&x, &a->num, &b->den); bn_mul(&y, &b->num, &a->den); bn_mul(&d, &a->den, &b->den);
    bn_add(&r->num, &x, &y); bn_swap(&r->den, &d);
    bq_reduce(r);
    bn_free(&x); bn_free(&y); bn_free(&d);
}
void bq_neg(bq *r, const bq *a) { bq_copy(r, a); bn_neg(&r->num, &r->num); }
void bq_abs(bq *r, const bq *a) { bq_copy(r, a); r->num.neg = 0; }
void bq_sub(bq *r, const bq *a, const bq *b)
{
    bq nb; bq_init(&nb); bq_neg(&nb, b); bq_add(r, a, &nb); bq_free(&nb);
}
void bq_mul(bq *r, const bq *a, const bq *b)
{
    bn x, d; bn_init(&x); bn_init(&d);
    bn_mul(&x, &a->num, &b->num); bn_mul(&d, &a->den, &b->den);
    bn_swap(&r->num, &x); bn_swap(&r->den, &d);
    bq_reduce(r);
    bn_free(&x); bn_free(&d);
}
void bq_div(bq *r, const bq *a, const bq *b)
{
    bn x, d; bn_init(&x); bn_init(&d);
    bn_mul(&x, &a->num, &b->den); bn_mul(&d, &a->den, &b->num);
    bn_swap(&r->num, &x); bn_swap(&r->den, &d);
    bq_reduce(r);
    bn_free(&x); bn_free(&d);
}

double bq_to_double(const bq *a)
{
    if (a->num.n == 0) return 0.0;
    /* q = floor(|num| 2^s / den) with 65..66 significant bits, sticky bit for the remainder,
     * then one correctly rounded uint64 -> double conversion and an exact scaling */
    bn n, d, q, rem; bn_init(&n); bn_init(&d); bn_init(&q); bn_init(&rem);
    bn_abs(&n, &a->num); bn_copy(&d, &a->den);
    int s = 66 + bn_bitlen(&d) - bn_bitlen(&n);
    if (s >= 0) bn_shl(&n, &n, (unsigned)s); else bn_shl(&d, &d, (unsigned)(-s));
    bn_divmod(&q, &rem, &n, &d);
    int qb = bn_bitlen(&q), drop = qb - 63;      /* keep 63 bits, fold the rest into sticky */
    uint64_t top = 0; int sticky = rem.n != 0;
    for (int i = qb - 1; i >= 0; i--) {
        int bit = (int)((q.d[i / 32] >> (i % 32)) & 1u);
        if (i >= drop) top = (top << 1) | (uint64_t)bit; else if (bit) sticky = 1;
    }
    top = (top << 1) | (uint64_t)sticky;           /* 64 bits: 63 significant + sticky */
    double r = ldexp((double)top, drop - 1 - s);
    bn_free(&n); bn_free(&d); bn_free(&q); bn_free(&rem);
    return a->num.neg ? -r : r;
}

void bq_from_double(bq *r, double x)
{
    uint64_t bits; memcpy(&bits, &x, sizeof bits);
    int neg = (int)(bits >> 63);
    int e = (int)((bits >> 52) & 0x7ff);
    uint64_t m = bits & (((uint64_t)1 << 52) - 1);
    if (e == 0) e = 1; else m |= (uint64_t)1 << 52;
    int p = e - 1075;                                /* x = m * 2^p */
    bn one; bn_init(&one);
    bn_set_u64(&r->num, m); bn_set_u64(&r->den, 1); bn_set_u64(&one, 1);
    if (p >= 0) bn_shl(&r->num, &r->num, (unsigned)p); else bn_shl(&r->den, &one, (unsigned)(-p));
    if (neg && r->num.n) r->num.neg = 1;
    bq_reduce(r);
    bn_free(&one);
}

void bq_from_scaled(bq *r, const bn *n, unsigned k)
{
    bn d; bn_init(&d); bn_pow10(&d, k);
    bq_set_bn(r, n, &d);
    bn_free(&d);
}

/* ---- Gaussian rationals -------------------------------------------------------------------- */
void gq_init(gq *a) { bq_init(&a->re); bq_init(&a->im); }
void gq_free(gq *a) { bq_free(&a->re); bq_free(&a->im); }
void gq_copy(gq *r, const gq *a) { bq_copy(&r->re, &a->re); bq_copy(&r->im, &a->im); }
int gq_is_zero(const gq *a) { return bq_is_zero(&a->re) && bq_is_zero(&a->im); }
void gq_add(gq *r, const gq *a, const gq *b) { bq_add(&r->re, &a->re, &b->re); bq_add(&r->im, &a->im, &b->im); }
void gq_sub(gq *r, const gq *a, const gq *b) { bq_sub(&r->re, &a->re, &b->re); bq_sub(&r->im, &a->im, &b->im); }
void gq_conj(gq *r, const gq *a) { bq_copy(&r->re, &a->re); bq_neg(&r->im, &a->im); }
void gq_neg(gq *r, const gq *a) { bq_neg(&r->re, &a->re); bq_neg(&r->im, &a->im); }
void gq_mul(gq *r, const gq *a, const gq *b)
{
    bq t1, t2, re, im; bq_init(&t1); bq_init(&t2); bq_init(&re); bq_init(&im);
    bq_mul(&t1, &a->re, &b->re); bq_mul(&t2, &a->im, &b->im); bq_sub(&re, &t1, &t2);
    bq_mul(&t1, &a->re, &b->im); bq_mul(&t2, &a->im, &b->re); bq_add(&im, &t1, &t2);
    bq_copy(&r->re, &re); bq_copy(&r->im, &im);
    bq_free(&t1); bq_free(&t2); bq_free(&re); bq_free(&im);
}
void gq_div(gq *r, const gq *a, const gq *b)
{
    gq cb, num; bq n2, t; gq_init(&cb); gq_init(&num); bq_init(&n2); bq_init(&t);
    gq_conj(&cb, b);
    gq_mul(&num, a, &cb);
    bq_mul(&n2, &b->re, &b->re); bq_mul(&t, &b->im, &b->im); bq_add(&n2, &n2, &t);
    bq_div(&r->re, &num.re, &n2); bq_div(&r->im, &num.im, &n2);
    gq_free(&cb); gq_free(&num); bq_free(&n2); bq_free(&t);
}
