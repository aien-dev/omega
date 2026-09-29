/* TURING companion records for continuous-value (quantized) evidence, and the
 * sidecar manifest line formats they bind. New digest domains only; no
 * existing turing.* record, domain or golden is touched.
 *
 *   turing.qprofile.v0  measurement profile (constraint)
 *   turing.qworld.v0    one scored world block (evidence)
 *   turing.qfreeze.v0   frozen artifact and its pre-run checks (evidence)
 *   turing.qgain.v0     one T measurement (evidence)
 *
 * Digest = SHA-256(domain || 0x00 || OMG0 bytes) via omega_canonical_encode
 * and src/sha256.c, exactly as ty_record.c. These are record digests, never
 * Omega semantic ids (OSC-0B).
 *
 * The library carries every descriptive value (names, versions, rule ids,
 * counts) as caller-supplied data; it holds no profile constants. Values
 * longer than 512 bytes and keys of 64 bytes or more are refused with
 * TY_E_FORMAT, never truncated. Builders and verifiers refuse a malformed
 * record; a verifier additionally recomputes the digest.
 *
 * Return codes are TY_E_* from ty_math.h (TY_OK on success), plus
 * TYQR_E_EXISTS for a write-once writer that finds the file already there.
 */
#ifndef TURING_TY_QRECORD_H
#define TURING_TY_QRECORD_H

#include <stddef.h>
#include <stdint.h>

#include "turing/ty_record.h"

#define TYQR_DOMAIN_PROFILE "turing.qprofile.v0"
#define TYQR_DOMAIN_WORLD "turing.qworld.v0"
#define TYQR_DOMAIN_FREEZE "turing.qfreeze.v0"
#define TYQR_DOMAIN_GAIN "turing.qgain.v0"

#define TYQR_E_EXISTS (-201)
#define TYQR_VAL 513      /* 512 value bytes plus NUL */
#define TYQR_MAX_LINEAGE 8
#define TYQR_CANON_CAP 40000

/* Text fields are printable ASCII (0x20..0x7e), non-empty. List fields are
 * tokens separated by exactly one space (no leading, trailing or doubled
 * space). Digest fields: an all-zero digest is the "absent" value. */

typedef struct {
    char profile[TYQR_VAL];
    ty_digest spec, packet;
    char protocol[TYQR_VAL], quantizer[TYQR_VAL];
    int64_t delta_log2;
    char edge_rule[TYQR_VAL];
    int64_t sd_min_log2;
    char floor_rule[TYQR_VAL];
    char families[TYQR_VAL];          /* list */
    uint64_t kmax, n_points, split;
    ty_digest cells;                  /* digest of the cell-table sidecar */
    char rider[TYQR_VAL], lm_rule[TYQR_VAL];
    char baselines[TYQR_VAL];         /* list */
    ty_digest thresholds;             /* digest of the thresholds sidecar */
    char thresholds_format[TYQR_VAL], gen_version[TYQR_VAL];
    uint64_t bootstrap_b;
    char unit[TYQR_VAL];
} tyqr_profile;

typedef struct {
    ty_digest qprofile;
    char mode[TYQR_VAL];              /* dev | certification | discovery */
    char parent_kind[TYQR_VAL];       /* dev | sealed */
    ty_digest qfreeze;                /* zero iff parent_kind = dev */
    char cell[TYQR_VAL];
    uint64_t block;
    char label[TYQR_VAL];
    char gen_version[TYQR_VAL];
    ty_digest generator;
    uint64_t rep_first, rep_count;
    ty_digest traj_manifest;
    char traj_format[TYQR_VAL];
} tyqr_world;

typedef struct {
    char mode[TYQR_VAL];              /* certification | discovery */
    ty_digest artifact;
    char artifact_kind[TYQR_VAL];
    ty_digest artifact_bin;           /* zero for certification */
    ty_digest qprofile, dev_manifest;
    char b2_emerging_label[TYQR_VAL]; /* POSITIVE | NEGATIVE | AMBIGUOUS */
    char bayes_plugin_check[TYQR_VAL];/* AGREE | STOP */
    char diag_fw_rate_ppm[TYQR_VAL];  /* list */
    char lengthening_test[TYQR_VAL];  /* PASS | FAIL */
    char stability_test[TYQR_VAL];    /* PASS | FAIL */
    char calibration_extra[TYQR_VAL];
    char run_commit[41];              /* 40 lowercase hex */
    uint64_t tree_dirty;              /* 0 or 1 */
    char base_commits[TYQR_VAL];      /* list */
    char producer_tool[TYQR_VAL], producer_model[TYQR_VAL];
    ty_digest prompt_digest, transcript_digest;
    uint64_t n_lineage;               /* 0..8 */
    ty_digest lineage[TYQR_MAX_LINEAGE];
} tyqr_freeze;

typedef struct {
    ty_digest qprofile, qworld, qfreeze;
    char mode[TYQR_VAL];              /* certification | discovery */
    char cell[TYQR_VAL], gen_version[TYQR_VAL];
    uint64_t baseline_opcode;         /* 1..7 */
    ty_digest baseline_artifact;
    ty_digest candidate;
    uint64_t candidate_opcode;        /* 1..7, or 255 */
    ty_digest prd_b, prd_m;
    uint64_t official;                /* 1 or 0 */
    char ladder_item[TYQR_VAL];       /* "-" iff official */
    int64_t lengths[6];               /* L(B) L(D|B) L(M) L(D|M) DL(B,D) DL(M,D) */
    int64_t t_ub, t_lo_ub, t_hi_ub, numeric_bound_ub, mc_err_ub;
    uint64_t counts[4];               /* B worlds heldout_per_world scored_points */
    uint64_t floor_hits, protocol_failures;
    char leak_flag[TYQR_VAL];         /* 0 | 1 | INCONCLUSIVE */
    int64_t coverage[3];              /* -1 where absent */
    char diag_warnings[TYQR_VAL];     /* list or "-" */
    char classification[TYQR_VAL];    /* POSITIVE NEGATIVE AMBIGUOUS WEAK STOPPED */
    char status[TYQR_VAL];            /* PASS | FAIL_* | STOPPED */
    char unit[TYQR_VAL];
} tyqr_gain;

/* Digest of a record; refuses (TY_E_FORMAT / TY_E_ARG) anything malformed. */
int tyqr_profile_digest(const tyqr_profile *r, ty_digest *out);
int tyqr_world_digest(const tyqr_world *r, ty_digest *out);
int tyqr_freeze_digest(const tyqr_freeze *r, ty_digest *out);
int tyqr_gain_digest(const tyqr_gain *r, ty_digest *out);

/* Rebuild from the fields and compare with the recorded digest:
 * TY_OK, TY_E_DIGEST on mismatch, TY_E_FORMAT on a malformed record. */
int tyqr_profile_verify(const tyqr_profile *r, const ty_digest *want, char *why, size_t whylen);
int tyqr_world_verify(const tyqr_world *r, const ty_digest *want, char *why, size_t whylen);
int tyqr_freeze_verify(const tyqr_freeze *r, const ty_digest *want, char *why, size_t whylen);
int tyqr_gain_verify(const tyqr_gain *r, const ty_digest *want, char *why, size_t whylen);

/* Cross-record binding for a gain (TY_E_PROFILE on any disagreement):
 * the gain, world and freeze all name the same qprofile digest (P); the gain
 * names the world's digest and the freeze's digest; mode, cell and
 * gen_version equal the world's; the world is sealed by that freeze; the
 * freeze's mode equals the gain's; candidate equals the freeze's artifact.
 * Each record is also verified against its digest. */
int tyqr_gain_bind(const tyqr_gain *g, const ty_digest *g_digest,
                   const tyqr_world *w, const ty_digest *w_digest,
                   const tyqr_freeze *f, const ty_digest *f_digest,
                   const ty_digest *profile_digest, char *why, size_t whylen);

/* Write-once, digest-named writer: <dir>/<record>.<digest hex>.omg0 holding
 * the canonical OMG0 bytes, O_CREAT|O_EXCL, mode 0444. Returns TY_OK,
 * TYQR_E_EXISTS if the file is already there (left untouched), or an error.
 * `path` (may be NULL) receives the file name. */
int tyqr_profile_write(const char *dir, const tyqr_profile *r, ty_digest *d, char *path, size_t pathlen);
int tyqr_world_write(const char *dir, const tyqr_world *r, ty_digest *d, char *path, size_t pathlen);
int tyqr_freeze_write(const char *dir, const tyqr_freeze *r, ty_digest *d, char *path, size_t pathlen);
int tyqr_gain_write(const char *dir, const tyqr_gain *r, ty_digest *d, char *path, size_t pathlen);

/* Recompute SHA-256(domain || 0x00 || file bytes) of a stored record file
 * and compare with `want` (TY_E_DIGEST on mismatch, TY_E_IO if unreadable). */
int tyqr_file_verify(const char *path, const char *domain, const ty_digest *want);

/* ---- sidecar manifests: ASCII, one record per line, fields separated by one
 * space, every line ends with LF (including the last), no other bytes. The
 * sidecar digest is SHA-256 of the file bytes. ---- */
#define TYQR_CELL_MAX 128

typedef struct { uint64_t rep; ty_digest d; } tyqr_repline;         /* traj, prd */
typedef struct { char cell[TYQR_CELL_MAX]; uint64_t block; ty_digest d; } tyqr_devline;

/* Render `<rep dec> <digest hex>` lines, rep strictly ascending. */
int tyqr_repman_build(const tyqr_repline *e, size_t n, char *out, size_t cap, size_t *len);
int tyqr_repman_parse(const char *buf, size_t len, tyqr_repline *out, size_t cap, size_t *n);
/* Render `<cell> <block dec> <digest hex>`: cell by byte order, then block
 * ascending (strictly). */
int tyqr_devman_build(const tyqr_devline *e, size_t n, char *out, size_t cap, size_t *len);
int tyqr_devman_parse(const char *buf, size_t len, tyqr_devline *out, size_t cap, size_t *n);
/* Format checks only (the values are caller-supplied):
 *   cell table:  `<cell> <field>=<value> ...`, cells strictly ascending bytes
 *   thresholds:  `<item_id> <field> <value>`, lines strictly ascending bytes */
int tyqr_celltab_check(const char *buf, size_t len);
int tyqr_thrtab_check(const char *buf, size_t len);
/* SHA-256 of sidecar bytes. */
void tyqr_sidecar_digest(const char *buf, size_t len, ty_digest *out);

#endif
