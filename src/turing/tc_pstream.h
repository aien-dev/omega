/* Turing calibration (EXP-001): canonical probability stream "TPS1" and
 * observed-symbol file "TSY1". Normative layout: calibration/docs/CODER_SPEC.md.
 *
 * A predictor emits one TPS1 record per observation BEFORE any coding. The two
 * reference coders (tc_range, tc_rans) consume exactly (TPS1, TSY1) and never
 * the model: this header and tc_pstream.c do not include ty_model.h. The
 * producer that decodes a .tym and walks a CTR1 trace lives in tc_produce.c.
 *
 * All integers little-endian. Probabilities are 16-bit quantized entries
 * q in 1..65535 with sum exactly 65536 per record (P = q / 65536). The
 * profile's probability_vector and quantized_probability_vector coincide:
 * TYM0 models are quantized tables, so no float vector exists to record.
 *
 * TPS1 = header (128 bytes) || n records || trailer digest (32 bytes)
 *   header: "TPS1" | version u16 (=1) | K u8 | qbits u8 (=16) | flags u32 (=0) |
 *           count u64 | record_bytes u32 (=24+2K) | reserved u32 (=0) |
 *           profile_digest[32] | model_digest[32] | dataset_digest[32]
 *   record: observation_index u64 (= position) | context_key u64 |
 *           crumb u32 (crumb ordinal in the dataset file, unsaturated) |
 *           norm_sum u32 (= 65536, the per-record normalization receipt) |
 *           q[K] u16
 *   trailer: SHA-256("turing.tc.pstream.v1" || 0x00 || header || records)
 * The trailer value is "the TPS1 digest".
 *
 * TSY1 = header (56 bytes) || n symbol bytes || trailer digest (32 bytes)
 *   header: "TSY1" | version u16 (=1) | K u8 | reserved u8 (=0) | count u64 |
 *           reserved u64 (=0) | dataset_digest[32]
 *   trailer: SHA-256("turing.tc.symbols.v1" || 0x00 || header || symbols)
 */
#ifndef TURING_TC_PSTREAM_H
#define TURING_TC_PSTREAM_H

#include <stddef.h>
#include <stdint.h>

#define TC_QONE 65536u
#define TC_KMIN 2
#define TC_KMAX 16
#define TC_PS_VERSION 1
#define TC_PS_HEADER 128
#define TC_PS_REC_FIXED 24
#define TC_SY_VERSION 1
#define TC_SY_HEADER 56
#define TC_DIGEST 32
#define TC_PS_DOMAIN "turing.tc.pstream.v1"
#define TC_SY_DOMAIN "turing.tc.symbols.v1"

/* Refusal codes (all negative; every refusal also fills a reason string). */
enum {
    TC_OK = 0,
    TC_E_ARG = -101,      /* NULL or out-of-range argument */
    TC_E_IO = -102,       /* file or allocation failure */
    TC_E_FORMAT = -103,   /* bad magic, version, reserved field, size, K, qbits */
    TC_E_DIGEST = -104,   /* trailer digest does not match the bytes */
    TC_E_NORM = -105,     /* a record's entries do not sum to 65536 (or norm_sum != 65536) */
    TC_E_ZERO = -106,     /* a probability entry is 0 */
    TC_E_PROFILE = -107,  /* profile digest differs from the expected one */
    TC_E_MODEL = -108,    /* model digest differs from the expected one */
    TC_E_DATASET = -109,  /* dataset digest differs (expected, or TPS1 vs TSY1) */
    TC_E_COUNT = -110,    /* symbol / observation count mismatch */
    TC_E_HEADER = -111,   /* coded-file header missing or altered */
    TC_E_BINDING = -112,  /* coded file is bound to a different TPS1 digest */
    TC_E_TRUNC = -113,    /* bitstream shorter than the decoder needs */
    TC_E_TRAIL = -114,    /* bitstream has bytes the decoder did not consume */
    TC_E_CORRUPT = -115,  /* decoder state invalid (termination check failed) */
    TC_E_SYMBOL = -116,   /* symbol outside 0..K-1, or decoded symbols differ */
    TC_E_INDEX = -117     /* observation_index is not the record position */
};
const char *tc_err_name(int e);

typedef struct {
    unsigned K;
    uint64_t n;
    uint8_t profile[TC_DIGEST], model[TC_DIGEST], dataset[TC_DIGEST];
    uint64_t *key;   /* n context keys */
    uint32_t *crumb; /* n crumb ordinals */
    uint16_t *q;     /* n*K entries, row t at q + t*K */
    uint8_t digest[TC_DIGEST]; /* trailer (filled by serialize/parse) */
} tc_pstream;

typedef struct {
    unsigned K;
    uint64_t n;
    uint8_t dataset[TC_DIGEST];
    uint8_t *sym;
    uint8_t digest[TC_DIGEST];
} tc_symbols;

void tc_ps_free(tc_pstream *p);
void tc_sy_free(tc_symbols *s);
/* Allocate arrays for n records of alphabet K (zeroed). */
int tc_ps_alloc(tc_pstream *p, unsigned K, uint64_t n);
int tc_sy_alloc(tc_symbols *s, unsigned K, uint64_t n);

/* Check every record: each entry >= 1, sum == 65536. */
int tc_ps_check_rows(const tc_pstream *p, char *why, size_t whylen);

/* Serialize (checks rows first). Fills p->digest. Caller frees *buf. */
int tc_ps_serialize(tc_pstream *p, uint8_t **buf, size_t *len, char *why, size_t whylen);
int tc_sy_serialize(tc_symbols *s, uint8_t **buf, size_t *len, char *why, size_t whylen);
/* Parse and fully verify (size, digest, header fields, every record). */
int tc_ps_parse(const uint8_t *buf, size_t len, tc_pstream *p, char *why, size_t whylen);
int tc_sy_parse(const uint8_t *buf, size_t len, tc_symbols *s, char *why, size_t whylen);

/* Refuse unless the TPS1 header digests equal the expected ones (NULL = not checked). */
int tc_ps_expect(const tc_pstream *p, const uint8_t *profile, const uint8_t *model, const uint8_t *dataset,
                 char *why, size_t whylen);
/* Refuse unless (TPS1, TSY1) are a matching pair: K, count, dataset digest, sym < K. */
int tc_pair_check(const tc_pstream *p, const tc_symbols *s, char *why, size_t whylen);

/* Ideal code length: sum over t of round(1e6 * -log2(q_t[x_t] / 65536)) micro-bits,
 * computed with ty_ubits_q16 (integer only; |error| <= 0.501 ub per symbol). */
int tc_ideal_ub(const tc_pstream *p, const tc_symbols *s, uint64_t lo, uint64_t hi, int64_t *ub);

/* Whole-file helpers. */
int tc_read_file(const char *path, uint8_t **buf, size_t *len);
int tc_write_file(const char *path, const uint8_t *buf, size_t len);
void tc_hex(const uint8_t d[TC_DIGEST], char out[65]);
int tc_unhex(const char *hex, uint8_t d[TC_DIGEST]);

/* Little-endian helpers shared by the coders' file headers. */
void tc_put_u16(uint8_t *b, uint16_t v);
void tc_put_u32(uint8_t *b, uint32_t v);
void tc_put_u64(uint8_t *b, uint64_t v);
uint16_t tc_get_u16(const uint8_t *b);
uint32_t tc_get_u32(const uint8_t *b);
uint64_t tc_get_u64(const uint8_t *b);

/* Coded-file header shared layout (56 bytes), see CODER_SPEC.md section 4:
 * magic[4] | version u8 (=1) | coder_id u8 | reserved u16 (=0) | count u64 |
 * tps1_digest[32] | payload_bytes u64. */
#define TC_CODED_HEADER 56
#define TC_CODED_VERSION 1
int tc_coded_header_write(uint8_t h[TC_CODED_HEADER], const char magic[4], unsigned coder_id, uint64_t count,
                          const uint8_t tps[TC_DIGEST], uint64_t payload);
/* Validate a coded file against the TPS1 it claims to code; payload and plen receive the payload span. */
int tc_coded_header_check(const uint8_t *buf, size_t len, const char magic[4], unsigned coder_id,
                          const tc_pstream *p, const uint8_t **payload, size_t *plen, char *why, size_t whylen);

#endif /* TURING_TC_PSTREAM_H */
