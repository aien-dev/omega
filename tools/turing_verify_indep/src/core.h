/* Lane D independent scorer for EXP-001: core formats and arithmetic.
 * Written only from calibration/docs (MODEL_DESCRIPTION_ENCODING.md, CODER_SPEC.md,
 * UNCERTAINTY_PROTOCOL.md, EVALUATOR.md) and TURING_YIELD_PROFILE_V0.md sections 2-3. */
#ifndef IS_CORE_H
#define IS_CORE_H
#include <stdint.h>
#include <stddef.h>

#define IS_K 9
#define IS_UB 1000000LL

/* ---------- ideal code length table (micro-bits) ---------- */
extern int64_t is_ub_q16[65537];      /* ub for q = 1..65536 */
void is_ub_table_init(void);
uint64_t is_log2_q32(uint32_t q);      /* floor-ish log2(q) * 2^32 via Q62 squaring */

/* ---------- TYM0 model ---------- */
typedef struct {
    int K, mask, qbits, keybits;
    uint64_t nrows, S, lm_bits;
    uint16_t def[16];
    uint64_t *keys;       /* nrows */
    uint16_t *rows;       /* nrows * K */
    uint8_t model_digest[32];
    uint8_t file_sha[32];
    uint64_t file_bytes;
} is_model;
/* returns 0 or a negative refusal number (1..8 as in MDE section 5, negated); msg filled */
int is_model_load(const char *path, is_model *m, char *msg, size_t msglen);
void is_model_free(is_model *m);
const uint16_t *is_model_row(const is_model *m, uint64_t key, int *found);

/* ---------- CTR1 events ---------- */
typedef struct {
    uint64_t n;
    uint8_t *sym, *op, *depth, *first;
    uint32_t *evidx;
    uint8_t sha[32];
    uint64_t bytes;
    uint64_t crumbs;         /* number of crumb starts = nonempty crumbs */
    uint64_t gaps;           /* event_index increments != 1 (not a refusal in V0 text) */
} is_events;
int is_ctr1_load(const char *path, is_events *ev, char *msg, size_t msglen);
void is_events_free(is_events *ev);

/* ---------- key walk ---------- */
typedef struct { int p[5]; uint64_t crumb; } is_walk;
uint64_t is_key(const is_model *m, const is_events *ev, uint64_t t, const is_walk *w);

/* ---------- coders (decoders) ---------- */
typedef struct { const uint8_t *b; uint64_t L, pos; uint32_t code, range; int err; } is_rdec;
typedef struct { const uint8_t *b; uint64_t L, pos; uint32_t x; int err; } is_adec;
int is_rdec_init(is_rdec *d, const uint8_t *b, uint64_t L);
int is_rdec_step(is_rdec *d, const uint16_t *q, int K);   /* returns symbol or <0 */
int is_rdec_finish(is_rdec *d);
int is_adec_init(is_adec *d, const uint8_t *b, uint64_t L);
int is_adec_step(is_adec *d, const uint16_t *q, int K);
int is_adec_finish(is_adec *d);

/* ---------- RNG ---------- */
uint64_t is_splitmix64(uint64_t *s);
uint64_t is_draw(uint64_t *s, uint64_t C);

/* ---------- helpers ---------- */
int is_read_file(const char *path, uint8_t **buf, uint64_t *len);
void is_domain_digest_init(void *ctx, const char *tag);
/* Experiment selection, same as the evaluator and the shell scripts: env EXP_ID = EXP-001 (default, profile v1.0)
 * or EXP-001R (profile v1.1). Returns the profile version string, or NULL for an unknown id. */
const char *is_profile_version(void);
#endif
