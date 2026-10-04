/* PD-0 verifier: minimal byte layouts (PD0REC1, PDLAW1, PD0EXP1) and the
 * verifier's evidence ledger (PD0LEDG1). Written from the spec on physics
 * main (docs/PD0_HIDDEN_EQUATION_BENCHMARK.md rev 2, sections 2.5, 7, 8).
 * Little-endian, fixed-width, no floating point in any canonical byte.
 * To be deduplicated against pd0-wire when the substrate lane merges. */
#ifndef PD0_FMT_H
#define PD0_FMT_H
#include <stddef.h>
#include <stdint.h>

#define PD0_MICRO 1000000LL
#define PD0_MAX_OBS 8
#define PD0_MAX_CHAN 4
#define PD0_MAX_VARS 12          /* observed + latent */
#define PD0_MAX_TERMS 64
#define PD0_MAX_EQ 12
#define PD0_MAX_STEPS 20
#define PD0_MAX_HYP 8
#define PD0_HASH 32

enum { PD0_ST_OK = 0, PD0_ST_REFUSED_RANGE = 1, PD0_ST_OUT_OF_BOUNDS = 2,
       PD0_ST_EPISODE_END = 3, PD0_ST_BUDGET_EXHAUSTED = 4 };
enum { PD0_KIND_RESET = 0, PD0_KIND_STEP = 1 };
#define PD0_CHAN_NONE 255

/* fixed-point product per spec 2.1: 128-bit, truncate toward zero */
int64_t pd0_mul(int64_t a, int64_t b);

/* ---- PD0REC1 observation record ---- */
typedef struct {
    uint8_t n_obs, status, kind, channel;
    uint64_t seq;
    uint32_t episode, step_in_episode;
    int64_t time_micro, requested, applied;
    int64_t before[PD0_MAX_OBS], after[PD0_MAX_OBS];
    uint8_t prev_hash[PD0_HASH], record_hash[PD0_HASH];
} pd0_rec;
size_t pd0_rec_size(uint8_t n_obs);
/* write: fills record_hash; prev_hash must be set by caller. returns bytes */
size_t pd0_rec_write(const pd0_rec *r, uint8_t *out, size_t cap);
/* parse: 0 ok, else PD0V_* code (bad magic/version/truncated/hash) */
int pd0_rec_parse(const uint8_t *in, size_t len, pd0_rec *r, size_t *used);

/* ---- relationship block (shared by PDLAW1 and RELATION ledger entries) ---- */
typedef struct {
    uint8_t target;
    uint16_t n_terms;
    int64_t coef[PD0_MAX_TERMS];
    uint8_t expo[PD0_MAX_TERMS][PD0_MAX_VARS + PD0_MAX_CHAN];
} pd0_eq;
typedef struct {
    uint8_t n_vars, n_latent, n_channels, n_equations;
    uint32_t description_bits;
    uint16_t n_refutations;
    pd0_eq eq[PD0_MAX_EQ];
} pd0_rel;
size_t pd0_rel_write(const pd0_rel *r, uint8_t *out, size_t cap);
int pd0_rel_parse(const uint8_t *in, size_t len, pd0_rel *r, size_t *used);
uint32_t pd0_rel_size(const pd0_rel *r);          /* spec 6.2 |R| */
uint32_t pd0_rel_bits(const pd0_rel *r);          /* spec 6.2 description_bits */
int pd0_rel_max_degree(const pd0_rel *r);
/* one tick: state has n_vars entries (observed then latent); chan/value applied */
void pd0_rel_step(const pd0_rel *r, const int64_t *state, uint8_t chan, int64_t value, int64_t *next);

/* ---- PDLAW1 (list counts and claim length are u16, per the substrate lane amendment in omega PR #243) ---- */
enum { PDLAW_REJECTED = 0, PDLAW_REFUTED = 1, PDLAW_HYPOTHESIS = 2, PDLAW_PROVISIONAL_LAW = 3 };
typedef struct { uint64_t record_seq; uint8_t record_hash[PD0_HASH]; int64_t predicted, observed, error_micro; } pd0_exception;
typedef struct { uint8_t experiment_id[PD0_HASH]; uint8_t kind; uint8_t prereg_hash[PD0_HASH]; uint8_t outcome_hash[PD0_HASH]; uint8_t result; uint64_t first_seq, last_seq; } pd0_experiment;
#define PD0_MAX_EXC 64
#define PD0_MAX_EXP 64
#define PD0_MAX_CLAIM 512
typedef struct {
    uint8_t n_obs, n_channels;
    int64_t var_min[PD0_MAX_OBS], var_max[PD0_MAX_OBS];
    int64_t chan_min[PD0_MAX_CHAN], chan_max[PD0_MAX_CHAN];
    int64_t dt_micro;
    uint64_t n_observations;
    uint32_t n_episodes;
    int64_t reset_min, reset_max;
    uint32_t episode_len;
    int64_t latent_reset;
} pd0_domain;
typedef struct {
    uint8_t state;
    uint8_t law_id[PD0_HASH];
    pd0_rel rel;
    pd0_domain dom;
    uint32_t confidence_ppm;
    uint32_t n_exceptions; pd0_exception exc[PD0_MAX_EXC];
    uint32_t n_experiments; pd0_experiment exp[PD0_MAX_EXP];
    uint8_t chain_root[PD0_HASH];
    uint32_t claim_len; uint8_t claim[PD0_MAX_CLAIM];
} pd0_law;
size_t pd0_law_write(pd0_law *l, uint8_t *out, size_t cap);   /* computes law_id */
size_t pd0_dom_write(const pd0_domain *d, uint8_t *o);  /* canonical bytes of the observed_domain block */
int pd0_law_parse(const uint8_t *in, size_t len, pd0_law *l);  /* verifies law_id */

/* ---- PD0EXP1 preregistration ---- */
typedef struct { uint8_t channel; int64_t value; } pd0_step;
typedef struct {
    uint8_t n_obs, n_hyp, n_steps;
    int64_t reset[PD0_MAX_OBS];
    pd0_step steps[PD0_MAX_STEPS];
    int64_t expected[PD0_MAX_HYP][PD0_MAX_STEPS][PD0_MAX_OBS];
    int64_t divergence_micro;
    uint8_t schedule_hash[PD0_HASH];
} pd0_exp;
size_t pd0_exp_write(pd0_exp *e, uint8_t *out, size_t cap);   /* computes schedule_hash */
int pd0_exp_parse(const uint8_t *in, size_t len, pd0_exp *e, size_t *used);
/* schedule hash = SHA-256(reset[n_obs] i64 LE || per step: channel u8, value i64 LE) */
void pd0_schedule_hash(uint8_t n_obs, const int64_t *reset, uint8_t n_steps, const pd0_step *steps, uint8_t out[PD0_HASH]);

/* ---- PD0LEDG1 evidence ledger (verifier-owned format) ---- */
enum { LEDG_OBS = 0, LEDG_TAG = 1, LEDG_CORR = 2, LEDG_RELATION = 3, LEDG_FALSIFIER = 4, LEDG_PREREG = 5, LEDG_BATCH = 6 };
enum { TAG_FIT = 0, TAG_SELECT = 1, TAG_HOLDOUT = 2, TAG_TRIAL = 3, TAG_REP = 4 };
enum { ORIGIN_PLANNER = 0, ORIGIN_RANDOM = 1 };
enum { REL_CANDIDATE = 0, REL_RIVAL = 1, REL_NULL = 2 };
typedef struct {
    uint8_t kind;
    const uint8_t *payload; uint32_t payload_len;
    uint8_t prev_hash[PD0_HASH], entry_hash[PD0_HASH];
} pd0_entry;
#define PD0_LEDG_HDR 16
#define PD0_LEDG_TRAILER (2 * PD0_HASH)
/* append an entry to a byte buffer; chains prev_hash from last_hash (zero for first). */
size_t pd0_ledg_append(uint8_t *buf, size_t len, size_t cap, uint8_t kind, const uint8_t *payload, uint32_t plen, uint8_t last_hash[PD0_HASH]);
/* parse next entry at offset; verifies hash + chain. returns 0 or code */
int pd0_ledg_next(const uint8_t *buf, size_t len, size_t *off, const uint8_t expect_prev[PD0_HASH], pd0_entry *e);

/* payload helpers */
typedef struct { uint32_t episode; uint8_t tag; uint32_t batch_id; uint8_t origin; } pd0_tag;
typedef struct { uint8_t var_a, var_b; uint32_t n; int64_t r_micro, p_micro; uint32_t n_pairs, n_shuffles; uint64_t shuffle_seed; } pd0_corr;
typedef struct { uint8_t candidate[PD0_HASH]; uint8_t n_rivals; uint8_t rival[PD0_MAX_HYP][PD0_HASH]; int64_t eps_micro; uint32_t min_trials; } pd0_falsifier;
typedef struct { uint32_t batch_id; uint64_t stream_seed; uint32_t n_episodes; } pd0_batch;
size_t pd0_tag_write(const pd0_tag *t, uint8_t *o);      int pd0_tag_parse(const uint8_t *i, size_t n, pd0_tag *t);
size_t pd0_corr_write(const pd0_corr *c, uint8_t *o);    int pd0_corr_parse(const uint8_t *i, size_t n, pd0_corr *c);
size_t pd0_fals_write(const pd0_falsifier *f, uint8_t *o); int pd0_fals_parse(const uint8_t *i, size_t n, pd0_falsifier *f);
size_t pd0_batch_write(const pd0_batch *b, uint8_t *o);  int pd0_batch_parse(const uint8_t *i, size_t n, pd0_batch *b);

/* LE helpers (exported for tests) */
void pd0_put_u16(uint8_t *p, uint16_t v); void pd0_put_u32(uint8_t *p, uint32_t v); void pd0_put_u64(uint8_t *p, uint64_t v);
uint16_t pd0_get_u16(const uint8_t *p); uint32_t pd0_get_u32(const uint8_t *p); uint64_t pd0_get_u64(const uint8_t *p);
#endif
