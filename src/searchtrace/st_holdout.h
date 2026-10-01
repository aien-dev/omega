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

/* argv[0] is the subcommand: commit | reveal | verify.
 * Exit codes: 0 ok, 2 refusal (incl. I/O failure), 1 usage. */
int st_holdout_cli(int argc, char **argv);

#endif /* OMEGA_ST_HOLDOUT_H */
