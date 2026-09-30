/*
 * rx_generation.h -- one active generation, and a candidate that is not
 * active until a promotion is committed.
 *
 * The organism keeps running while a candidate is prepared. The barrier is
 * strict only for that candidate, and only once promotion has started.
 * After any stop, recovery selects the previous generation or the next one
 * as a whole. It never mixes the two.
 *
 * The candidate may describe its successor. It cannot authorize the
 * transition. Authorization is the promotion right on the native authority.
 */
#ifndef RX_GENERATION_H
#define RX_GENERATION_H

#include <stddef.h>
#include <stdint.h>

#include "rx_caller.h"

#define RX_GEN_OK              0
#define RX_GEN_ERR_ARG       -40
#define RX_GEN_ERR_STALE     -41
#define RX_GEN_ERR_AUTHORITY -42
#define RX_GEN_ERR_CLOSING   -43
#define RX_GEN_ERR_TORN      -44
#define RX_GEN_ERR_MISSING   -45
#define RX_GEN_ERR_REPLAY    -46
#define RX_GEN_ERR_BUSY      -47
#define RX_GEN_ERR_VERIFY    -48
#define RX_GEN_ERR_IO        -49
#define RX_GEN_ERR_IDENTITY  -50   /* caller credential absent, forged, stale or revoked */

#define RX_GEN_MAX_OBJECTS   32u
#define RX_GEN_MAX_WORK      16u
#define RX_GEN_MAX_EXTERNAL  8u
#define RX_GEN_MAX_EXCLUDED  8u

/* Resource and right a promotion must present. The right is also defined
 * on the authority root; this is the same number. */
#define RX_GEN_RES_PROMOTION 0x905ull
#define RX_GEN_RIGHT_PROMOTE 0x200u

enum {
    RX_WORK_EPHEMERAL = 1,
    RX_WORK_EVIDENCE = 2,
    RX_WORK_EXTERNAL = 3
};

enum {
    RX_WORK_PENDING = 0,
    RX_WORK_ISSUED = 1,
    RX_WORK_DONE = 2,
    RX_WORK_CANCELLED = 3
};

/* Where a crash is injected. The process stops at that point. */
enum {
    RX_CRASH_NONE = 0,
    RX_CRASH_BEFORE_CANDIDATE_WRITE = 1,
    RX_CRASH_DURING_CANDIDATE_WRITE = 2,
    RX_CRASH_AFTER_CANDIDATE_WRITE = 3,
    RX_CRASH_BEFORE_ROOT_FLIP = 4,
    RX_CRASH_DURING_ROOT_FLIP = 5,
    RX_CRASH_AFTER_ROOT_FLIP = 6,
    RX_CRASH_BEFORE_RECEIPT = 7,
    RX_CRASH_AFTER_RECEIPT = 8
};

typedef struct RxGenStore RxGenStore;

typedef struct {
    uint32_t id;
    uint32_t generation;
    uint8_t digest[32];
} RxGenObject;

typedef struct {
    uint64_t authority_epoch;
    uint64_t authority_generation;
    int proofs_ok;
    const RxGenObject *objects;
    uint32_t n_objects;
    const uint8_t *evidence;
    size_t evidence_len;
    const uint8_t *model;
    size_t model_len;
    const uint8_t *realization;
    size_t realization_len;
    const uint8_t *config;
    size_t config_len;
    const uint8_t *provenance;
    size_t provenance_len;
} RxGenDraft;

typedef struct {
    uint64_t id;
    uint32_t class;
    uint32_t state;
    int required;
} RxGenWork;

typedef struct {
    uint64_t candidate_id;
    uint32_t subject;
    uint32_t cap_id;
    uint64_t cap_generation;
    uint64_t resource;
    uint32_t rights;
    /* R16 C5: the credential the runtime issued for `subject`. Required by a
     * store bound with rx_gen_bind_authority; ignored by an unbound one. */
    RxCallerCred caller;
} RxPromotionRequest;

typedef struct {
    int coherent;
    uint64_t active_id;
    uint64_t lineage;
    uint64_t parent_id;
    int receipt_present;
    int event_present;
    uint32_t n_external;
    uint64_t external_ids[RX_GEN_MAX_EXTERNAL];
    uint32_t n_excluded;
    uint64_t excluded_ids[RX_GEN_MAX_EXCLUDED];
} RxRecoveryRecord;

typedef int (*RxGenAuthFn)(void *ctx, uint32_t cap_id, uint64_t cap_generation,
                           uint32_t subject, uint64_t resource, uint32_t rights);
typedef int (*RxGenDrainFn)(uint64_t work_id, void *ctx);
typedef void (*RxGenLiveFn)(void *ctx);
typedef void (*RxGenDiskHook)(const char *generation_dir, void *ctx);

/* Monotonic timestamps for the most recent promotion attempt. A zero phase
 * means the attempt stopped before reaching it. Read after promotion returns. */
typedef struct {
    uint64_t candidate_id;
    uint64_t enter_ns;
    uint64_t barrier_ns;
    uint64_t flip_ns;
    uint64_t receipt_ns;
    int result;
    /* Finer stamps inside the barrier (R15 trace, spec §6.7). */
    uint64_t verified_ns;        /* work classified, observations and proofs checked */
    uint64_t blobs_ns;           /* the six generation blobs written and synced */
    uint64_t candidate_ns;       /* root and CANDIDATE journal written and synced */
    uint64_t reachable_ns;       /* root re-read from disk and checked */
    uint64_t flipped_ns;         /* FLIPPED journal written and synced */
    uint64_t receipt_file_ns;    /* receipt file written and synced */
    uint64_t event_ns;           /* event log appended and synced */
} RxGenPhases;

int rx_gen_open(const char *dir, RxGenStore **out);
void rx_gen_close(RxGenStore *store);
/* R15: process-wide bytes written and fsync calls made by every store.
 * Cumulative; a cross-check only, never a benchmark's own figure. */
void rx_gen_io_counters(uint64_t *bytes, uint64_t *syncs);
/* R15: bytes passed to write() and fsync calls issued by this store since
 * rx_gen_open (spec §6.7, L1-G). Directory creation, rename and unlink are
 * metadata operations and are not counted as bytes. */
int rx_gen_store_io(const RxGenStore *store, uint64_t *bytes, uint64_t *syncs);
int rx_gen_last_phases(const RxGenStore *store, RxGenPhases *out);
int rx_gen_active(const RxGenStore *store, uint64_t *id, uint64_t *lineage);

/* One blob of committed generation `id` ("evidence", "model", "realization",
 * "config", "provenance" or "objects"), checked against that generation's
 * root: length and SHA-256. A missing root is RX_GEN_ERR_MISSING; a blob that
 * does not match is RX_GEN_ERR_TORN. */
int rx_gen_read_blob(const RxGenStore *store, uint64_t id, const char *name, uint8_t *buf,
                     size_t cap, size_t *out_len);

int rx_gen_propose(RxGenStore *store, uint32_t proposer, const RxGenDraft *draft,
                   uint64_t *out_id);
/* R16 C5: the proposer presents its credential. On a bound store plain
 * rx_gen_propose (no credential) is refused with RX_GEN_ERR_IDENTITY. */
int rx_gen_propose_as(RxGenStore *store, uint32_t proposer, const RxCallerCred *cred,
                      const RxGenDraft *draft, uint64_t *out_id);
int rx_gen_mutate_object(RxGenStore *store, uint64_t candidate, uint32_t index,
                         uint32_t generation, const uint8_t digest[32]);
int rx_gen_set_evidence(RxGenStore *store, uint64_t candidate, const uint8_t *bytes,
                        size_t n);
int rx_gen_observe_object(RxGenStore *store, uint64_t candidate, uint32_t index,
                          uint32_t generation);
int rx_gen_add_work(RxGenStore *store, uint64_t candidate, const RxGenWork *work);
int rx_gen_finish_work(RxGenStore *store, uint64_t candidate, uint64_t work_id);

void rx_gen_set_crash(RxGenStore *store, int step);
void rx_gen_set_disk_hook(RxGenStore *store, RxGenDiskHook fn, void *ctx);
int rx_gen_hold_barrier(RxGenStore *store);
int rx_gen_release_barrier(RxGenStore *store);

int rx_gen_promote(RxGenStore *store, const RxPromotionRequest *request,
                   RxGenAuthFn auth, void *auth_ctx, RxGenDrainFn drain, void *drain_ctx,
                   RxGenLiveFn live, void *live_ctx);

/* R16 C5: bind the store to the runtime's caller check and to the native
 * promotion authority, once (RX_GEN_ERR_BUSY if already bound). From then
 * on: rx_gen_propose_as and rx_gen_promote check the proposer's / the
 * request subject's credential through `caller` before anything else
 * (RX_GEN_ERR_IDENTITY), and rx_gen_promote validates the promotion right
 * with the bound `auth` / `auth_ctx` only: an authority callback passed by the
 * caller is not consulted. There is no unbind. */
typedef int (*RxGenCallerFn)(void *ctx, uint32_t subject, const RxCallerCred *cred);
int rx_gen_bind_authority(RxGenStore *store, RxGenCallerFn caller, void *caller_ctx,
                          RxGenAuthFn auth, void *auth_ctx);

/* Durable executor (R15 G7). One thread per store that performs a store's
 * physical work (the fsyncs of a proposal and of a promotion) so that the
 * reaction deciding it does not hold a semantic worker while the disk works.
 * It decides nothing: it runs rx_gen_propose or rx_gen_promote exactly as a
 * caller would, with the caller's request and authority callback, one job at
 * a time in the order posted, and then calls `done` (outside every lock).
 * There is one job slot per kind; a slot holds its result until
 * rx_gen_job_take. The world never waits on it except to learn it finished.
 * Without rx_gen_exec_start a store is used synchronously, as before. */
enum { RX_GEN_JOB_PROPOSE = 0, RX_GEN_JOB_PROMOTE = 1, RX_GEN_JOB_KINDS = 2 };
enum { RX_GEN_JOB_IDLE = 0, RX_GEN_JOB_PENDING = 1, RX_GEN_JOB_DONE = 2 };
typedef void (*RxGenDoneFn)(void *ctx);
typedef struct {
    uint64_t key;               /* the caller's key for this job */
    int rc;                     /* what rx_gen_propose / rx_gen_promote returned */
    uint64_t id;                /* proposal: the candidate id */
    uint64_t active, lineage;   /* the store's active generation after the job */
    uint64_t ns;                /* wall time the store call took */
} RxGenJobResult;

int rx_gen_exec_start(RxGenStore *store);
int rx_gen_exec_running(const RxGenStore *store);
/* RX_GEN_OK: posted. RX_GEN_ERR_BUSY: that slot is pending or holds an
 * untaken result. The draft is copied; the caller's buffers may go away. */
int rx_gen_post_propose(RxGenStore *store, uint64_t key, uint32_t proposer,
                        const RxGenDraft *draft, RxGenDoneFn done, void *done_ctx);
int rx_gen_post_propose_as(RxGenStore *store, uint64_t key, uint32_t proposer,
                           const RxCallerCred *cred, const RxGenDraft *draft,
                           RxGenDoneFn done, void *done_ctx);
int rx_gen_post_promote(RxGenStore *store, uint64_t key, const RxPromotionRequest *request,
                        RxGenAuthFn auth, void *auth_ctx, RxGenDoneFn done, void *done_ctx);
/* State of one slot; on RX_GEN_JOB_DONE `out` (if given) holds the result. */
int rx_gen_job_state(RxGenStore *store, int kind, RxGenJobResult *out);
/* Empty a DONE slot. */
void rx_gen_job_take(RxGenStore *store, int kind);

int rx_gen_recover(const char *dir, RxRecoveryRecord *out);
int rx_gen_reject_replay(const char *dir, uint64_t effect_id);

#endif
