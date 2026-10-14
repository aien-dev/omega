/* In-house zlib / DEFLATE decoder (see res_inflate.h). RFC 1950 framing,
 * RFC 1951 blocks: stored, fixed Huffman, dynamic Huffman. Bit-serial
 * canonical decoding: slow but simple, and fast enough for the ~20 MB the
 * Gate 2 tools decode. */
#include "res_inflate.h"

#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bitbuf; /* holds bitcnt (< 8 between calls) unread bits */
    int bitcnt;
    uint8_t *out;
    size_t out_len, out_cap;
} St;

typedef struct {
    short count[MAXBITS + 1]; /* number of codes of each length */
    short symbol[FIXLCODES];  /* symbols in canonical code order */
} Huff;

static int getbits(St *s, int need, uint32_t *val)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return RES_INF_ERR_TRUNC;
        v |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    *val = v & ((1u << need) - 1u);
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return RES_INF_OK;
}

static int put(St *s, uint8_t b)
{
    if (s->out_len == s->out_cap) {
        if (s->out_cap >= RES_INFLATE_MAX_OUT) return RES_INF_ERR_NOMEM;
        size_t cap = s->out_cap ? s->out_cap * 2u : (size_t)65536;
        if (cap > RES_INFLATE_MAX_OUT) cap = RES_INFLATE_MAX_OUT;
        uint8_t *p = realloc(s->out, cap);
        if (!p) return RES_INF_ERR_NOMEM;
        s->out = p;
        s->out_cap = cap;
    }
    s->out[s->out_len++] = b;
    return RES_INF_OK;
}

static int stored(St *s)
{
    s->bitbuf = 0; /* the < 8 leftover bits are the rest of the current byte */
    s->bitcnt = 0;
    if (s->in_len - s->in_pos < 4) return RES_INF_ERR_TRUNC;
    const uint8_t *p = s->in + s->in_pos;
    unsigned len = (unsigned)p[0] | ((unsigned)p[1] << 8);
    unsigned nlen = (unsigned)p[2] | ((unsigned)p[3] << 8);
    if (len != (~nlen & 0xffffu)) return RES_INF_ERR_BLOCK;
    s->in_pos += 4;
    if (s->in_len - s->in_pos < len) return RES_INF_ERR_TRUNC;
    for (unsigned i = 0; i < len; i++) {
        int e = put(s, s->in[s->in_pos + i]);
        if (e) return e;
    }
    s->in_pos += len;
    return RES_INF_OK;
}

/* Returns 0 for a complete code, > 0 for an incomplete one, < 0 if
 * over-subscribed. */
static int construct(Huff *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int decode(St *s, const Huff *h, int *sym)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        uint32_t b;
        int e = getbits(s, 1, &b);
        if (e) return e;
        code |= (int)b;
        int count = h->count[len];
        if (code - count < first) {
            *sym = h->symbol[index + (code - first)];
            return RES_INF_OK;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return RES_INF_ERR_CODES;
}

static const short LBASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(St *s, const Huff *lc, const Huff *dc)
{
    for (;;) {
        int sym, e = decode(s, lc, &sym);
        if (e) return e;
        if (sym < 256) {
            e = put(s, (uint8_t)sym);
            if (e) return e;
            continue;
        }
        if (sym == 256) return RES_INF_OK;
        sym -= 257;
        if (sym >= 29) return RES_INF_ERR_SYMBOL;
        uint32_t x;
        e = getbits(s, LEXT[sym], &x);
        if (e) return e;
        size_t len = (size_t)LBASE[sym] + x;
        int dsym;
        e = decode(s, dc, &dsym);
        if (e) return e;
        if (dsym < 0 || dsym >= 30) return RES_INF_ERR_SYMBOL;
        e = getbits(s, DEXT[dsym], &x);
        if (e) return e;
        size_t dist = (size_t)DBASE[dsym] + x;
        if (dist > s->out_len) return RES_INF_ERR_SYMBOL;
        for (size_t i = 0; i < len; i++) {
            e = put(s, s->out[s->out_len - dist]);
            if (e) return e;
        }
    }
}

static int fixed(St *s)
{
    static Huff lc, dc;
    static int built;
    if (!built) {
        short lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lc, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&dc, lengths, MAXDCODES);
        built = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(St *s)
{
    static const short ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXCODES];
    Huff lc, dc;
    uint32_t v;
    int e;
    if ((e = getbits(s, 5, &v))) return e;
    int nlen = (int)v + 257;
    if ((e = getbits(s, 5, &v))) return e;
    int ndist = (int)v + 1;
    if ((e = getbits(s, 4, &v))) return e;
    int ncode = (int)v + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return RES_INF_ERR_CODES;
    int index;
    for (index = 0; index < ncode; index++) {
        if ((e = getbits(s, 3, &v))) return e;
        lengths[ORDER[index]] = (short)v;
    }
    for (; index < 19; index++) lengths[ORDER[index]] = 0;
    if (construct(&lc, lengths, 19) != 0) return RES_INF_ERR_CODES;
    index = 0;
    while (index < nlen + ndist) {
        int sym;
        if ((e = decode(s, &lc, &sym))) return e;
        if (sym < 16) {
            lengths[index++] = (short)sym;
            continue;
        }
        short len = 0;
        int rep;
        if (sym == 16) {
            if (index == 0) return RES_INF_ERR_CODES;
            len = lengths[index - 1];
            if ((e = getbits(s, 2, &v))) return e;
            rep = 3 + (int)v;
        } else if (sym == 17) {
            if ((e = getbits(s, 3, &v))) return e;
            rep = 3 + (int)v;
        } else {
            if ((e = getbits(s, 7, &v))) return e;
            rep = 11 + (int)v;
        }
        if (index + rep > nlen + ndist) return RES_INF_ERR_CODES;
        while (rep--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return RES_INF_ERR_CODES;
    e = construct(&lc, lengths, nlen);
    if (e && (e < 0 || nlen != lc.count[0] + lc.count[1])) return RES_INF_ERR_CODES;
    e = construct(&dc, lengths + nlen, ndist);
    if (e && (e < 0 || ndist != dc.count[0] + dc.count[1])) return RES_INF_ERR_CODES;
    return codes(s, &lc, &dc);
}

uint32_t res_adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n > 0) {
        size_t k = n < 5552u ? n : 5552u;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

ResInflateStatus res_zlib_inflate(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                                  size_t *consumed)
{
    if (!in || !out || !out_len || !consumed) return RES_INF_ERR_ARG;
    *out = NULL;
    *out_len = 0;
    *consumed = 0;
    if (in_len < 2) return RES_INF_ERR_TRUNC;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0fu) != 8u || (cmf >> 4) > 7u || ((cmf << 8) | flg) % 31u != 0u) return RES_INF_ERR_HEADER;
    if (flg & 0x20u) return RES_INF_ERR_HEADER;
    St s;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.in_len = in_len;
    s.in_pos = 2;
    int e = RES_INF_OK;
    uint32_t last = 0, type;
    while (!last) {
        if ((e = getbits(&s, 1, &last))) break;
        if ((e = getbits(&s, 2, &type))) break;
        if (type == 0) e = stored(&s);
        else if (type == 1) e = fixed(&s);
        else if (type == 2) e = dynamic(&s);
        else e = RES_INF_ERR_BLOCK;
        if (e) break;
    }
    if (!e) {
        s.bitbuf = 0;
        s.bitcnt = 0;
        if (s.in_len - s.in_pos < 4) e = RES_INF_ERR_TRUNC;
    }
    if (!e) {
        const uint8_t *t = in + s.in_pos;
        uint32_t want = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | (uint32_t)t[3];
        if (res_adler32(s.out, s.out_len) != want) e = RES_INF_ERR_ADLER;
    }
    if (!e && !s.out) {
        s.out = malloc(1);
        if (!s.out) e = RES_INF_ERR_NOMEM;
    }
    if (e) {
        free(s.out);
        return (ResInflateStatus)e;
    }
    *out = s.out;
    *out_len = s.out_len;
    *consumed = s.in_pos + 4;
    return RES_INF_OK;
}

const char *res_inflate_strerror(ResInflateStatus s)
{
    switch (s) {
    case RES_INF_OK: return "ok";
    case RES_INF_ERR_HEADER: return "bad zlib header";
    case RES_INF_ERR_TRUNC: return "truncated stream";
    case RES_INF_ERR_BLOCK: return "bad block";
    case RES_INF_ERR_CODES: return "bad Huffman code";
    case RES_INF_ERR_SYMBOL: return "bad symbol or distance";
    case RES_INF_ERR_ADLER: return "Adler-32 mismatch";
    case RES_INF_ERR_NOMEM: return "out of memory or output above cap";
    case RES_INF_ERR_ARG: return "bad argument";
    }
    return "unknown";
}
