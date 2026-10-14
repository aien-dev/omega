/* In-house zlib (RFC 1950) / DEFLATE (RFC 1951) decoder for the omega#440
 * Gate 2 tools: it reads the miCOMPRESSED elements of the authors' MAT v5
 * files. Written here instead of linking system zlib so the qualification
 * tools have no dependency outside this repository; the Adler-32 trailer of
 * every stream is checked, so a decoding error cannot pass silently.
 *
 * Input is untrusted: every length, distance and code table is checked,
 * output is capped (RES_INFLATE_MAX_OUT) and a preset dictionary is refused.
 * Decoder structure follows the canonical-Huffman scheme of RFC 1951
 * section 3.2 (code counts per length, symbols in code order). */
#ifndef RES_INFLATE_H
#define RES_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#define RES_INFLATE_MAX_OUT ((size_t)1 << 30) /* 1 GiB: refuse anything larger */

typedef enum {
    RES_INF_OK = 0,
    RES_INF_ERR_HEADER = -1,   /* not a zlib stream, preset dictionary, bad check bits */
    RES_INF_ERR_TRUNC = -2,    /* input ended inside the stream */
    RES_INF_ERR_BLOCK = -3,    /* reserved block type, stored length mismatch */
    RES_INF_ERR_CODES = -4,    /* bad or over-subscribed Huffman code */
    RES_INF_ERR_SYMBOL = -5,   /* invalid length/distance symbol or distance too far */
    RES_INF_ERR_ADLER = -6,    /* Adler-32 of the output differs from the trailer */
    RES_INF_ERR_NOMEM = -7,    /* allocation failed or output above the cap */
    RES_INF_ERR_ARG = -8
} ResInflateStatus;

/* Decodes one zlib stream from in[0..in_len). On success *out is a malloc'd
 * buffer of *out_len bytes (caller frees) and *consumed the number of input
 * bytes the stream occupied, trailer included. On failure *out is NULL. */
ResInflateStatus res_zlib_inflate(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                                  size_t *consumed);

uint32_t res_adler32(const uint8_t *p, size_t n);

const char *res_inflate_strerror(ResInflateStatus s);

#endif /* RES_INFLATE_H */
