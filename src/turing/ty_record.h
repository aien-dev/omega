/* Turing Yield companion records (TY-1). New digest domains only; no existing
 * turing.* record, domain or golden is touched.
 *
 *   turing.yprofile.v0  measurement profile (what is coded, how, what passes)
 *   turing.ysplit.v0    split manifest: SHA-256 of every fit file and every
 *                       held-out file, in declared order
 *   turing.yfit.v0      fit provenance of one model: profile, split, role,
 *                       feature mask, fit rule, fit file digests, model digest
 *   turing.yield.v0     one T measurement: baseline and candidate code lengths
 *                       on the held-out files, DL, T in micro-bits, verdict
 *
 * Each digest = SHA-256(domain || 0x00 || OMG0 bytes) through the existing
 * omega_canonical_encode + src/sha256.c path, as in src/turing/field.c. These
 * digests are record digests, never Omega semantic ids (OSC-0B).
 */
#ifndef TURING_TY_RECORD_H
#define TURING_TY_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "turing/ty_model.h"

#define TY_DOMAIN_PROFILE "turing.yprofile.v0"
#define TY_DOMAIN_SPLIT "turing.ysplit.v0"
#define TY_DOMAIN_FIT "turing.yfit.v0"
#define TY_DOMAIN_GAIN "turing.yield.v0"

#define TY_MAX_FILES 12
#define TY_TEXT 256

typedef struct {
    uint8_t b[32];
} ty_digest;

void ty_hex(const ty_digest *d, char out[65]);
int ty_parse_hex(const char *hex, ty_digest *out);
int ty_digest_eq(const ty_digest *a, const ty_digest *b);
int ty_file_sha256(const char *path, ty_digest *out);

typedef struct {
    char name[64];
    char dataset[TY_TEXT];
    ty_digest manifest;            /* SHA-256 of the manifest file bytes */
    char x_t[TY_TEXT];
    char side_info[TY_TEXT];
    char context_reset[128];
    unsigned K;
    unsigned qbits, floor_q;       /* 16, 1 */
    char estimator[TY_TEXT];
    char lm_code[TY_TEXT];
    unsigned fit_rule;             /* TY_FIT_* used for every model under this profile */
    char baseline_rule[TY_TEXT];
    unsigned baseline_mask;        /* the declared baseline's feature mask */
    char candidate_rule[TY_TEXT];
    unsigned candidate_mask;       /* the tuned candidate's feature mask */
    char pass_rule[TY_TEXT];
    int64_t margin_ub;             /* pooled T must exceed this */
    unsigned qerr_milli_ub;        /* per-symbol fixed-point error bound, 1/1000 ub */
    char energy_denominator[TY_TEXT];
} ty_yprofile;

typedef struct {
    size_t nfit, nheld;
    ty_digest fit[TY_MAX_FILES], held[TY_MAX_FILES];
    char fit_label[TY_MAX_FILES][64], held_label[TY_MAX_FILES][64];
} ty_split;

enum { TY_ROLE_BASELINE = 0, TY_ROLE_CANDIDATE = 1 };

typedef struct {
    ty_digest profile, split;
    unsigned role, mask, rule;
    size_t nfit;
    ty_digest fit[TY_MAX_FILES];
    ty_digest model;               /* ty_model_digest of the model code */
    uint64_t model_bits;           /* L(M) */
} ty_fit_rec;

typedef struct {
    ty_digest profile, split, baseline_fit, candidate_fit, baseline_model, candidate_model;
    uint64_t nsym;
    uint64_t lm_b_bits, lm_c_bits;
    int64_t ld_b_ub, ld_c_ub, dl_b_ub, dl_c_ub, t_ub;
    size_t nheld;
    uint64_t file_nsym[TY_MAX_FILES];
    int64_t file_t_ub[TY_MAX_FILES]; /* per held file, each model's full L(M) charged */
    int64_t qerr_bound_ub;           /* |t_ub - exact| <= this */
    int64_t margin_ub;
    char verdict[16];                /* PASS or FAIL */
} ty_gain_rec;

/* Measurement profile V0 (docs/turing/TURING_YIELD_PROFILE_V0.md), all fields except
 * manifest, which the caller sets to the SHA-256 of the committed manifest file. */
void ty_yprofile_v0(ty_yprofile *p);

int ty_profile_digest(const ty_yprofile *p, ty_digest *out);
int ty_split_digest(const ty_split *s, ty_digest *out);
int ty_fit_digest(const ty_fit_rec *f, ty_digest *out);
int ty_gain_digest(const ty_gain_rec *g, ty_digest *out);

/* Read and fit: re-hash each fit file against the digest given, read it, fit
 * the model, encode it, fill the provenance record. Caller frees *code. */
int ty_fit_files(const ty_yprofile *p, const ty_split *sp, const char *const *paths, const ty_digest *digests,
                 size_t nfiles, unsigned role, unsigned mask, ty_fit_rec *rec, uint8_t **code, size_t *ncode,
                 char *why, size_t whylen);

/* Compute T of candidate over baseline on the split's held-out files. Refuses
 * (without scoring) on profile, baseline, provenance, leak or digest mismatch. */
int ty_gain_compute(const ty_yprofile *p, const ty_split *sp, const ty_fit_rec *brec, const uint8_t *bcode,
                    size_t nb, const ty_fit_rec *crec, const uint8_t *ccode, size_t nc,
                    const char *const *held_paths, ty_gain_rec *out, char *why, size_t whylen);

/* Independent re-derivation: refit baseline and candidate from the fit files
 * (re-hashed), recompute the gain on the held-out files (re-hashed) and
 * compare every field and the record digest against rec/rec_digest. */
int ty_gain_verify(const ty_yprofile *p, const ty_split *sp, const char *const *fit_paths,
                   const char *const *held_paths, const ty_gain_rec *rec, const ty_digest *rec_digest, char *why,
                   size_t whylen);

#endif /* TURING_TY_RECORD_H */
