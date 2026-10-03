/* PD-0 relations in PDLAW1 delta form (spec sections 6.2 and 7):
 *   D target = sum over terms of coef * monomial,   monomial = prod var^ex * prod chan^ex
 * coef in micro-units, monomial products via pd0_mul; a term with all
 * exponents 0 is the constant monomial 1.0 (1_000_000 micro).
 * Variable order: observed, then latent (latent start at 0 on every reset).
 * Also here: rollout prediction, one-step residual, NRMSE, size |R|, and the
 * PDLAW1 law record encoder. No generator knowledge. */
#ifndef PD0_RELATION_H
#define PD0_RELATION_H

#include <stddef.h>
#include <stdint.h>

#include "physics0/pd0_wire.h"

#define PD0_MAX_LATENT 2
#define PD0_MAX_VARS (PD0_MAX_OBS + PD0_MAX_LATENT)
#define PD0_MAX_EX (PD0_MAX_VARS + PD0_MAX_CH)
#define PD0_MAX_TERMS 12
#define PD0_MAX_DEG 3
#define PD0_SCHED_STEPS 20

typedef struct { int64_t coef; uint8_t ex[PD0_MAX_EX]; } pd0_term;
typedef struct { uint8_t target, n_terms; pd0_term t[PD0_MAX_TERMS]; } pd0_eq;
typedef struct {
    uint8_t n_vars, n_latent, n_channels, n_eq;
    pd0_eq eq[PD0_MAX_VARS];
} pd0_relation;

/* a schedule: reset state (observed vars) + up to PD0_SCHED_STEPS steps */
typedef struct {
    uint8_t n_obs, n_steps;
    int64_t reset[PD0_MAX_OBS];
    uint8_t channel[PD0_SCHED_STEPS];
    int64_t value[PD0_SCHED_STEPS];
} pd0_sched;

/* a trajectory of observed states: traj[0] = reset, traj[i] after step i */
typedef struct { uint8_t n_obs, n; int64_t s[PD0_SCHED_STEPS + 1][PD0_MAX_OBS]; } pd0_traj;

int  pd0_relation_valid(const pd0_relation *r);           /* 0 ok, -1 malformed */
int  pd0_relation_size(const pd0_relation *r);            /* |R| of section 6.2 */
int  pd0_relation_degree_ok(const pd0_relation *r);       /* every monomial degree <= 3 */
/* rev 2 section 6.2 rule: sum over terms (8*(n_vars+n_channels) + 24) + 8*n_latent */
uint32_t pd0_relation_description_bits(const pd0_relation *r);
/* evaluate the monomial of term t at state (vars incl latent) + channels */
int64_t pd0_monomial(const pd0_term *t, const int64_t *vars, const int64_t *u, int n_vars, int n_ch);
/* one tick: vars_in (n_vars) + u -> vars_out; equations without a target
 * leave the variable unchanged. */
void pd0_relation_step(const pd0_relation *r, const int64_t *vars_in, const int64_t *u, int64_t *vars_out);
/* roll out a schedule from its reset state (latent 0) into traj */
void pd0_relation_rollout(const pd0_relation *r, const pd0_sched *s, pd0_traj *out);
/* NRMSE of section 6.1 over a set of (predicted, true) trajectories:
 * max_j RMSE_j / std_j. Trajectory step 0 (reset) is excluded (it is given).
 * std_j == 0 gives +inf unless RMSE_j == 0 too. */
double pd0_nrmse(const pd0_traj *pred, const pd0_traj *truth, int n_traj);
/* SHA-256 of a schedule's canonical bytes */
void pd0_sched_hash(const pd0_sched *s, uint8_t out[PD0_HASH]);
/* 1 if every true term's monomial appears in r (6.3 support) */
int pd0_relation_supports(const pd0_relation *r, const pd0_relation *truth);
/* max over true terms of |coef - true| / |true|, in ppm; -1 if support fails */
int64_t pd0_relation_coef_err_ppm(const pd0_relation *r, const pd0_relation *truth);

/* ---- PDLAW1 (section 7) ---- */
enum { PD0_LAW_REJECTED = 0, PD0_LAW_REFUTED = 1, PD0_LAW_HYPOTHESIS = 2, PD0_LAW_PROVISIONAL = 3 };
#define PD0_MAX_EXCEPT 16
#define PD0_MAX_EXPER 16

typedef struct { uint64_t seq; uint8_t hash[PD0_HASH]; int64_t predicted, observed, error_micro; } pd0_exception;
typedef struct {
    uint8_t id[PD0_HASH], kind, prereg[PD0_HASH], outcome[PD0_HASH], result;
    uint64_t first_seq, last_seq;
} pd0_experiment;
typedef struct {
    int64_t var_min[PD0_MAX_OBS], var_max[PD0_MAX_OBS];
    int64_t ch_min[PD0_MAX_CH], ch_max[PD0_MAX_CH];
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
    pd0_relation rel;
    pd0_domain dom;
    uint32_t confidence_ppm;
    uint16_t n_exceptions, n_experiments;
    pd0_exception exc[PD0_MAX_EXCEPT];
    pd0_experiment exp[PD0_MAX_EXPER];
    uint8_t chain_root[PD0_HASH];
    uint16_t n_refutations;      /* rev 2: passes through REFUTED before this record */
    uint32_t eps_micro; /* declared eps (NRMSE as ppm of 1.0) for claim_text */
} pd0_law;

#define PD0_LAW_MAX 4096
/* encodes canonical bytes, computes law_id; returns bytes or 0 */
size_t pd0_law_encode(pd0_law *l, uint8_t *out, size_t cap);
int    pd0_law_decode(const uint8_t *in, size_t len, pd0_law *out); /* 0 ok; -1 refused */
/* section 7 rule: floor(1e6 * (p+1) / (p+f+2)) */
uint32_t pd0_confidence_ppm(uint32_t p, uint32_t f);
/* the fixed claim_text template into buf; returns length */
size_t pd0_claim_text(const pd0_law *l, char *buf, size_t cap);
/* human JSON projection (non-canonical) */
size_t pd0_law_json(const pd0_law *l, char *buf, size_t cap);
void   pd0_hex(const uint8_t *h, size_t n, char *out); /* 2n+1 bytes */

#endif
