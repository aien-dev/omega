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
    uint32_t authority_generation;
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
    uint32_t cap_generation;
    uint64_t resource;
    uint32_t rights;
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

typedef int (*RxGenAuthFn)(void *ctx, uint32_t cap_id, uint32_t cap_generation,
                           uint32_t subject, uint64_t resource, uint32_t rights);
typedef int (*RxGenDrainFn)(uint64_t work_id, void *ctx);
typedef void (*RxGenLiveFn)(void *ctx);
typedef void (*RxGenDiskHook)(const char *generation_dir, void *ctx);

int rx_gen_open(const char *dir, RxGenStore **out);
void rx_gen_close(RxGenStore *store);
int rx_gen_active(const RxGenStore *store, uint64_t *id, uint64_t *lineage);

int rx_gen_propose(RxGenStore *store, uint32_t proposer, const RxGenDraft *draft,
                   uint64_t *out_id);
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

int rx_gen_recover(const char *dir, RxRecoveryRecord *out);
int rx_gen_reject_replay(const char *dir, uint64_t effect_id);

#endif
