/* tg_store.h -- M22 transactional parameter-state substrate (Lane 21).
 *
 * A training "generation" is one committed, content-addressed state:
 *   parameters (opaque bytes, usually N float32) + optimizer state (opaque
 *   bytes) + step counter + SHA-256 digest + previous generation's digest
 *   + the head of the per-dispatch provenance chain at commit time.
 *
 * Writer protocol (one writer per store directory, enforced by flock):
 *   tg_shadow_begin   copy the committed state into a private SHADOW
 *   (updaters)        mutate only the shadow; each update adds one small
 *                     per-dispatch provenance record (tier a), held with the
 *                     shadow and appended + fsynced to dispatch.log at commit
 *   tg_commit         validation callback must accept, then: write shadow
 *                     file, fsync, read back + verify digest, rename to
 *                     gen-N.bin, fsync dir, write CURRENT.tmp, fsync, rename
 *                     to CURRENT (the ONE atomic switch), fsync dir.
 * Readers (tg_read_current) take no lock and always get OLD or NEW, digest
 * verified against the pointer, never a mix.
 *
 * Provenance has two tiers, kept separate (CURRENT_EXECUTION_PLAN E5):
 *   (a) dispatch.log: fixed-size, append-only, hash-chained records holding
 *       step, op id, scalar args and input REFERENCES (buffer id, offset,
 *       length, base generation digest). Hashing cost per record is constant
 *       and independent of model size; the model is never hashed here.
 *   (b) the full parameter + optimizer digest, computed only at commit.
 *
 * Plain C, no tensor/autodiff dependency (M20/M21 integration is later).
 * This substrate does NOT qualify M22. */
#ifndef OMEGA_TG_STORE_H
#define OMEGA_TG_STORE_H

#include <stddef.h>
#include <stdint.h>

#define TG_DIGEST 32
#define TG_MAX_REFS 3

typedef enum {
    TG_OK = 0,
    TG_E_ARG = -1,        /* bad argument / size mismatch */
    TG_E_IO = -2,         /* filesystem error */
    TG_E_NOSTORE = -3,    /* no CURRENT pointer */
    TG_E_FORMAT = -4,     /* bad magic / version / pointer syntax / extra bytes */
    TG_E_TRUNC = -5,      /* generation file shorter than its header declares */
    TG_E_DIGEST = -6,     /* recomputed digest != header digest or != pointer digest */
    TG_E_CHAIN = -7,      /* dispatch provenance chain does not match the commit */
    TG_E_REJECT = -8,     /* validation callback refused the shadow */
    TG_E_STATE = -9,      /* shadow already committed or discarded (double switch) */
    TG_E_STALE = -10,     /* shadow base generation is not the current generation */
    TG_E_SHADOW = -11,    /* shadow file read-back did not verify (corrupted shadow) */
    TG_E_LOCKED = -12,    /* another writer holds the store */
    TG_E_EXISTS = -13,    /* tg_create on a non-empty store */
    TG_E_NOMEM = -14
} tg_err;

const char *tg_err_name(int err);

/* Injectable crash points inside tg_commit (test harness only; default off). */
typedef enum {
    TG_FP_NONE = 0,
    TG_FP_BEFORE_SHADOW_WRITE = 1,
    TG_FP_MID_SHADOW_WRITE = 2,      /* header + half of the parameter bytes written */
    TG_FP_AFTER_SHADOW_WRITE = 3,    /* shadow fsynced and verified, not yet renamed */
    TG_FP_AFTER_GEN_RENAME = 4,      /* gen-N.bin exists, pointer not switched */
    TG_FP_MID_SWITCH = 5,            /* CURRENT.tmp half written */
    TG_FP_BEFORE_SWITCH_RENAME = 6,  /* CURRENT.tmp complete + fsynced, not renamed */
    TG_FP_AFTER_SWITCH_BEFORE_FSYNC = 7, /* CURRENT renamed, directory not fsynced */
    TG_FP_AFTER_SWITCH = 8,          /* everything durable, in-memory state not updated */
    TG_FP_COUNT = 9
} tg_failpoint;

typedef enum {
    TG_FPMODE_EXIT = 1,     /* _exit(TG_FP_EXIT_CODE) at the point */
    TG_FPMODE_CORRUPT = 2   /* only at AFTER_SHADOW_WRITE's read-back: flip one shadow byte on disk */
} tg_fpmode;

#define TG_FP_EXIT_CODE 77

void tg_failpoint_arm(tg_failpoint fp, tg_fpmode mode);
void tg_failpoint_disarm(void);
const char *tg_failpoint_name(tg_failpoint fp);

/* Input reference for a dispatch record: buffer id + byte range. */
typedef struct {
    uint32_t buf;      /* TG_BUF_* or caller-defined id */
    uint64_t off;
    uint64_t len;
} tg_ref;

enum { TG_BUF_PARAMS = 1, TG_BUF_OPT = 2, TG_BUF_EXTERNAL = 3 };

typedef struct {
    uint8_t *params;
    size_t param_bytes;
    uint8_t *opt;
    size_t opt_bytes;
    uint64_t base_gen;
    uint64_t base_step;
    uint8_t base_digest[TG_DIGEST];
    uint32_t n_dispatch;
    uint32_t rec_cap;
    uint8_t *recs;      /* pending dispatch records, appended to dispatch.log at commit */
    int state;          /* internal: open / committed / discarded */
} tg_shadow;

typedef struct {
    uint32_t removed_tmp;       /* shadow-*.tmp and CURRENT.tmp removed */
    uint32_t removed_orphan_gen;/* gen files newer than CURRENT removed */
    uint64_t dispatch_truncated;/* uncommitted dispatch-log bytes dropped */
} tg_recovery;

typedef struct {
    uint64_t gen;
    uint64_t step;
    uint8_t digest[TG_DIGEST];
    uint8_t prev_digest[TG_DIGEST];
    uint8_t dispatch_head[TG_DIGEST];
    uint64_t dispatch_len;
    uint8_t *params;
    size_t param_bytes;
    uint8_t *opt;
    size_t opt_bytes;
} tg_snapshot;

typedef int (*tg_validate_fn)(const tg_shadow *sh, const tg_snapshot *committed, void *ctx);

typedef struct tg_store tg_store;

/* Bytes hashed for tier (a) provenance and for tier (b), for the cost test. */
typedef struct {
    uint64_t dispatch_records;
    uint64_t dispatch_hashed_bytes;
    uint64_t commit_hashed_bytes;
    uint64_t refusals;
} tg_stats;

int tg_create(const char *dir, const void *params, size_t param_bytes,
              const void *opt, size_t opt_bytes);
int tg_open(const char *dir, tg_store **out, tg_recovery *rec);
void tg_close(tg_store *st);

const tg_snapshot *tg_committed(const tg_store *st);
const tg_stats *tg_get_stats(const tg_store *st);

int tg_shadow_begin(tg_store *st, tg_shadow *sh);
void tg_shadow_discard(tg_shadow *sh);
/* Append a tier (a) record for an update applied to `sh`. */
int tg_dispatch(tg_store *st, tg_shadow *sh, uint32_t op_id, uint64_t arg0, uint64_t arg1,
                const tg_ref *refs, uint32_t n_refs);
/* Validate, then switch. On success `sh` is consumed (state committed). */
int tg_commit(tg_store *st, tg_shadow *sh, tg_validate_fn validate, void *ctx);

/* Lock-free reader: current generation, digest verified against CURRENT. */
int tg_read_current(const char *dir, tg_snapshot *out);
void tg_snapshot_free(tg_snapshot *s);

/* Read one generation by number (verified; expect_digest may be NULL). */
int tg_read_gen(const char *dir, uint64_t gen, const uint8_t *expect_digest, tg_snapshot *out);

/* Walk the prev-digest chain from CURRENT back to generation 0 (every
 * generation file must be present and verify). */
int tg_verify_history(const char *dir, uint64_t *n_verified);

/* Decoded tier (a) record (read back from dispatch.log, chain verified up to it). */
typedef struct {
    uint64_t seq, step, base_gen, arg0, arg1;
    uint32_t op_id, n_refs;
    tg_ref refs[TG_MAX_REFS];
    uint8_t base_digest[TG_DIGEST];
    uint8_t chain[TG_DIGEST];
} tg_dispatch_rec;

int tg_dispatch_read(const char *dir, uint64_t seq, tg_dispatch_rec *out);

/* Recompute the dispatch-log chain over its first `len` bytes. */
int tg_dispatch_chain(const char *dir, uint64_t len, uint8_t head[TG_DIGEST], uint64_t *n_records);

void tg_hex(const uint8_t d[TG_DIGEST], char out[2 * TG_DIGEST + 1]);

#endif
