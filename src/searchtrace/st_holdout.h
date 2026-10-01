/* st_holdout.h - G3 sealed-holdout commitment format, v1.
 *
 * A held-out task set (opaque bytes) is committed to publicly before any
 * learner runs. The task-set bytes and a secret 32-byte salt stay sealed.
 * A later reveal proves the revealed set is exactly the committed one.
 * Normative description: spec/searchtrace/G3_SEALED_HOLDOUT_COMMITMENT_V1.md
 *
 * Return convention: 0 ok, negative = refusal. `why` (when non-NULL)
 * receives a short NUL-terminated reason on refusal.
 */
#ifndef OMEGA_ST_HOLDOUT_H
#define OMEGA_ST_HOLDOUT_H

#include <stddef.h>
#include <stdint.h>

#define ST_HOLDOUT_DOMAIN       "omega.g3.holdout.commit.v1"
#define ST_HOLDOUT_SALT_DOMAIN  "omega.g3.holdout.salt.v1"
#define ST_HOLDOUT_MAGIC        "OMEGA-G3-HOLDOUT-COMMIT v1"
#define ST_HOLDOUT_REVEAL_MAGIC "OMEGA-G3-HOLDOUT-REVEAL v1"
#define ST_HOLDOUT_ID_MAX       64
#define ST_HOLDOUT_SALT_LEN     32
#define ST_HOLDOUT_TASK_PREFIX  "task "
#define ST_HOLDOUT_SIG_MAGIC    "OMEGA-G3-HOLDOUT-SIG v1"
#define ST_HOLDOUT_SIG_DOMAIN   "omega.g3.holdout.sig.v1"

/* Refusal codes (all negative). */
#define ST_HOLDOUT_EARG      (-1)  /* NULL / bad argument            */
#define ST_HOLDOUT_ELABEL    (-2)  /* holdout_id not [A-Za-z0-9._-]{1,64} */
#define ST_HOLDOUT_ENOTASKS  (-3)  /* task set has zero "task " lines */
#define ST_HOLDOUT_ECAP      (-4)  /* output buffer too small         */
#define ST_HOLDOUT_EFORMAT   (-5)  /* record/receipt not canonical    */
#define ST_HOLDOUT_EDIGEST   (-6)  /* end digest mismatch             */
#define ST_HOLDOUT_ESALT     (-7)  /* salt does not match salt_digest */
#define ST_HOLDOUT_ELEN      (-8)  /* taskset_len mismatch            */
#define ST_HOLDOUT_ECOUNT    (-9)  /* task_count mismatch             */
#define ST_HOLDOUT_ECOMMIT   (-10) /* commitment mismatch             */
#define ST_HOLDOUT_ECOMMITID (-11) /* repo commit not 40 lowercase hex */
#define ST_HOLDOUT_EUNSIGNED (-12) /* strict mode: no signature record  */
#define ST_HOLDOUT_EBIND     (-13) /* signature names another record    */
#define ST_HOLDOUT_EKEY      (-14) /* signed by a different key         */
#define ST_HOLDOUT_ESIG      (-15) /* Ed25519 signature does not verify */

typedef struct {
    char     holdout_id[ST_HOLDOUT_ID_MAX + 1];
    uint32_t task_count;
    uint64_t taskset_len;
    uint8_t  salt_digest[32];
    uint8_t  commitment[32];
    uint8_t  end_digest[32];    /* sha256 of record bytes before "end " */
    uint8_t  record_digest[32]; /* sha256 of the whole record (file name) */
} StHoldoutCommitment;

/* Number of lines in the task set that begin with "task ". */
uint32_t st_holdout_count_tasks(const uint8_t *taskset, size_t len);

int st_holdout_commit(const char *holdout_id, const uint8_t *taskset,
                      size_t len, const uint8_t salt[32], char *rec,
                      size_t cap, size_t *rec_len);

int st_holdout_parse(const char *rec, size_t len, StHoldoutCommitment *out,
                     char *why, size_t why_len);

int st_holdout_reveal(const StHoldoutCommitment *c, const uint8_t *taskset,
                      size_t len, const uint8_t salt[32], char *why,
                      size_t why_len);

/* Emits a PASS receipt. Caller MUST have obtained 0 from st_holdout_reveal
 * for the same commitment first; this function does not re-verify. */
int st_holdout_reveal_receipt(const StHoldoutCommitment *c,
                              const char *repo_commit_hex40, char *out,
                              size_t cap, size_t *out_len);


/* ---- owner signature over the public commitment record (optional) ------
 * A detached signature record; the commitment record itself is unchanged.
 * Pure Ed25519 (vendored src/searchtrace/sig, from aienos native/sig) over
 *   "omega.g3.holdout.sig.v1" 0x00 || <whole commitment record bytes>.
 * Signature record (canonical ASCII, LF only, fixed order):
 *   OMEGA-G3-HOLDOUT-SIG v1
 *   domain omega.g3.holdout.sig.v1
 *   holdout_id <label, equal to the record's>
 *   record_digest <64 hex: sha256 of the whole commitment record>
 *   key_id <64 hex: sha256 of the 32-byte Ed25519 public key>
 *   signature <128 lowercase hex>
 *   end <64 hex: sha256 of every byte before "end ">
 * File name: g3-sig-<record_digest>.txt (next to the commitment record). */

/* Signs a canonical commitment record (refused unless st_holdout_parse
 * accepts it). The secret key is a 32-byte Ed25519 seed. */
int st_holdout_sign(const char *rec, size_t rec_len, const uint8_t sk[32],
                    char *out, size_t cap, size_t *out_len, char *why,
                    size_t why_len);

/* Strict check: the record must parse, `sig` must be present (NULL or
 * empty = ST_HOLDOUT_EUNSIGNED), canonical, name this record, carry the key
 * id of `pk` and verify under `pk`. On success fills *out like
 * st_holdout_parse. */
int st_holdout_verify_signed(const char *rec, size_t rec_len,
                             const char *sig, size_t sig_len,
                             const uint8_t pk[32], StHoldoutCommitment *out,
                             char *why, size_t why_len);

/* argv[0] is the subcommand: commit | reveal | verify | sign.
 * verify <record> (parse only), verify --strict <public_key> <record> [<sig>],
 * sign <record> <secret_key_file> <out_dir>.
 * Exit codes: 0 ok, 2 refusal (incl. I/O failure), 1 usage. */
int st_holdout_cli(int argc, char **argv);

#endif /* OMEGA_ST_HOLDOUT_H */
