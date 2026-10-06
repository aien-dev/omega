/*
 * rxc_host_abi.h -- NEXT-PHASE-1 cut 1a: a thin C ABI over COMPOSITION-2
 * (rx_compose) for a host program in another language (the sovereign-core
 * aien-omega-compose crate).
 *
 * A FACADE, nothing more: open = rx_compose_open on a fresh AIENOS authority,
 * run = rx_compose_run, recall = cx_recall / cx_recall_id on the composition's
 * own Cortex journal (<dir>/cortex.cx), close = rx_compose_close. No new
 * scheduler, store or receipt format. The record of a run is the ordinary
 * COMPOSITION-2 Cortex record (rx_compose.h): candidate claims, the AEGIS
 * verification evidence, one promotion, one admission per loser.
 *
 * Pointer-light: one opaque handle, plain fixed-size structs, no pointer is
 * kept from the caller except the skill/verify ctx it registers.
 *
 * Lifecycle:
 *   rxc_host_open      identity check + Cortex torn-tail probe; nothing runs yet
 *   rxc_host_register_skill (0..RXC_HOST_MAX_SKILLS times)
 *   first rxc_host_run / rxc_host_recall / rxc_host_info: the Skill graph is
 *       built and the composition opens (OLD-or-NEW recovery). Registration
 *       closes here (RXC_HOST_E_STATE afterwards).
 *   rxc_host_close
 *
 * Skills. A Skill is a host callback; the composition calls it on a World
 * worker thread with the run's task handle (the 64-bit `task` given to
 * rxc_host_run, which the host maps to its own goal text). It returns 0 and
 * a nonzero 64-bit result (a proposal handle the host can resolve), or
 * nonzero to report failure. Up to RXC_K (2) Skills compete per goal; two
 * may run at once, so the callback must be thread-safe. Every Skill is
 * pinned to a 32-byte procedure digest (default: sha256 of its name).
 *
 * Verification. The AEGIS verifier enforces a contract on (task, result).
 * With a verify callback the host decides (1 = meets the contract); without
 * one the contract is "result != 0". The callback runs on a World worker.
 *
 * Bounds: RXC_HOST_MAX_HANDLES open handles per process (the composition
 * contract and Skill functions take no user pointer, so each handle owns a
 * fixed slot of trampolines). One handle per directory (the Cortex journal
 * takes an exclusive writer lock; a second open of the same dir from any
 * process fails at the first run with RXC_HOST_E_OPEN).
 *
 * Torn journal tail. rxc_host_open probes <dir>/cortex.cx read-only first.
 * If the journal ends inside a record it reports info->tail_torn = 1 and:
 *   - with RXC_HOST_OPEN_REFUSE_TORN, refuses (RXC_HOST_E_TORN, no handle);
 *   - otherwise opens; rx_compose_open then truncates the torn record
 *     (CX_OPEN_REPAIR_TAIL), cross-checks the J-Space anchor (a cut inside
 *     the committed prefix is refused at first run with RXC_HOST_E_OPEN) and
 *     completes missing composition records (RxcHostInfo.recovered_completed).
 * A repair is therefore always reported, never silent.
 *
 * Production only: this file is part of build/librx_compose.a, which is
 * built without AIEN_TEST_BUILD / RXC_TEST_HOOKS (mk/rx_compose_lib.mk).
 */
#ifndef RXC_HOST_ABI_H
#define RXC_HOST_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RXC_HOST_ABI_VERSION 1u
#define RXC_HOST_MAX_HANDLES 4u
#define RXC_HOST_MAX_SKILLS 2u        /* = RXC_K */
#define RXC_HOST_NAME_MAX 64u
#define RXC_HOST_NONE 0xFFFFFFFFu     /* winner: none */

enum {
    RXC_HOST_OK = 0,
    RXC_HOST_E_ARG = -1,
    RXC_HOST_E_IDENTITY = -2,   /* machine.id in dir names another machine, or bad root */
    RXC_HOST_E_TORN = -3,       /* journal tail torn and RXC_HOST_OPEN_REFUSE_TORN set */
    RXC_HOST_E_OPEN = -4,       /* rx_compose_open refused (see RxcHostInfo.open_rc) */
    RXC_HOST_E_FULL = -5,       /* RXC_HOST_MAX_HANDLES / RXC_HOST_MAX_SKILLS reached */
    RXC_HOST_E_STATE = -6,      /* wrong lifecycle phase (registration closed, no Skill) */
    RXC_HOST_E_RUN = -7,        /* rx_compose_run returned an error (RxcHostResult.run_rc) */
    RXC_HOST_E_NOMEM = -8,
    RXC_HOST_E_NOT_FOUND = -9,  /* recall: no such record */
    RXC_HOST_E_DIGEST = -10     /* recall: stored digest does not match the record */
};

/* rxc_host_open flags */
#define RXC_HOST_OPEN_REFUSE_TORN 1u

/* Machine root kinds (= AIEN_MID_ROOT_*). */
enum { RXC_HOST_ROOT_PROVISIONED = 1, RXC_HOST_ROOT_HARDWARE = 2 };

/* Cortex subjects of the composition record (= RXC_CX_SUBJECT(slot)). */
#define RXC_HOST_SUBJECT_GOAL  1ull
#define RXC_HOST_SUBJECT_STATE 5ull   /* candidates, evidence, promotion, admissions */

/* Composition outcomes (= RXC_OUT_*). */
enum {
    RXC_HOST_OUT_COMMITTED = 1, RXC_HOST_OUT_NO_WINNER = 2, RXC_HOST_OUT_NOT_COMMITTED = 3,
    RXC_HOST_OUT_NOT_DURABLE = 4, RXC_HOST_OUT_RECORD_FAILED = 5
};

typedef struct RxcHost RxcHost;

/* 0 + *result (nonzero) = proposal; nonzero return = the Skill failed. */
typedef int (*RxcHostSkillFn)(void *ctx, uint64_t task, uint64_t *result);
/* 1 = result meets the contract for task, 0 = it does not. */
typedef int (*RxcHostVerifyFn)(void *ctx, uint64_t task, uint64_t result);

typedef struct {
    uint32_t abi_version;           /* RXC_HOST_ABI_VERSION */
    uint8_t  machine_root;          /* RXC_HOST_ROOT_* */
    uint8_t  machine_id[32];        /* AienMachineId.id: identical across restarts */
    uint32_t machine_id_was_stored; /* 1: dir already held machine.id (and it matched) */
    uint32_t tail_torn;             /* 1: cortex.cx ended inside a record at open */
    uint32_t opened;                /* 1: the composition is open (first run/recall/info) */
    int32_t  open_rc;               /* rx_compose_open return (0 = RX_OK) */
    uint64_t records;               /* Cortex records now */
    uint64_t recovered_record;      /* Cortex id naming the recovered state (0 = genesis) */
    uint32_t rolled_back;           /* newer records refused at open (rollback admissions) */
    uint32_t recovered_completed;   /* composition records completed at open */
    uint32_t anchor_unknown;        /* 1: no Cortex anchor in the checkpoint (audit only) */
    uint32_t n_skills;
} RxcHostInfo;

typedef struct {
    int32_t  run_rc;                /* rx_compose_run return (0 = RX_OK) */
    int32_t  outcome;               /* RXC_HOST_OUT_* */
    uint32_t committed;             /* 1 iff outcome == COMMITTED (NEW committed, durable) */
    uint32_t n_branches;            /* staged J-Space branches forked (= routed alternatives) */
    uint32_t branches_reclaimed;    /* staged branches reclaimed at settle */
    uint32_t winner;                /* index of the winning alternative, RXC_HOST_NONE = none */
    uint32_t winner_skill;          /* host Skill index of the winner, RXC_HOST_NONE = none */
    uint32_t aegis_pass_mask;       /* bit k: alternative k met the contract (AEGIS verdict) */
    uint64_t task;
    uint64_t result;                /* the committed result (winner's), 0 if none */
    uint64_t cx_goal;               /* Cortex id of the goal record (0 if none) */
    uint64_t cx_candidate[2];       /* Cortex ids, 0 = none */
    uint64_t cx_evidence;           /* AEGIS verification evidence */
    uint64_t cx_promotion;
    uint64_t cx_admission[2];
    uint8_t  winner_digest[32];     /* J-Space content digest of the committed branch */
    uint8_t  record_digest[32];     /* rx_compose_record_digest after the run */
} RxcHostResult;

typedef struct {
    uint64_t id;
    uint32_t cls, kind;             /* CxClass, CX_K_* */
    uint64_t subject, t, generation, tag;
    uint64_t links[4];
    uint32_t n_payload;             /* payload words (read with rxc_host_payload) */
    uint32_t verified;              /* 1: digest recomputed and matched */
    uint8_t  digest[32];
} RxcHostRecord;

/* Open (or create) the composition home `dir` (made 0700 if absent) for
 * the machine derived from (root_kind, root[0..root_len)). Identity: a dir
 * that already holds machine.id must hold this machine (RXC_HOST_E_IDENTITY
 * otherwise). `info` (may be NULL) is filled on success and on
 * RXC_HOST_E_TORN / RXC_HOST_E_IDENTITY. */
int rxc_host_open(const char *dir, uint32_t root_kind, const uint8_t *root, size_t root_len,
                  uint64_t session, uint32_t flags, RxcHost **out, RxcHostInfo *info);

/* Register a Skill (before the first run/recall/info). `name` is 1..63
 * bytes; `digest32` pins the procedure (NULL = sha256(name)); `cost` ranks
 * it (lower first). Returns the Skill index (0..) or RXC_HOST_E_*. */
int rxc_host_register_skill(RxcHost *h, const char *name, const uint8_t *digest32, uint64_t cost,
                            RxcHostSkillFn fn, void *ctx);

/* Set the AEGIS contract callback (before the first run; NULL = result != 0). */
int rxc_host_set_verify(RxcHost *h, RxcHostVerifyFn fn, void *ctx);

/* One goal through the whole COMPOSITION-2 path. `task` must be nonzero.
 * `now_us` is the run's clock (route liveness). RXC_HOST_OK with out filled
 * (check out->outcome), or RXC_HOST_E_* (out->run_rc holds the cause). */
int rxc_host_run(RxcHost *h, uint64_t task, uint64_t now_us, RxcHostResult *out);

/* Records of `subject`, oldest first, each digest checked: fills up to
 * `max`, *n_total = how many exist (may exceed max). */
int rxc_host_recall(RxcHost *h, uint64_t subject, RxcHostRecord *out, uint32_t max,
                    uint32_t *n_filled, uint64_t *n_total);

/* One record by Cortex id (RXC_HOST_E_NOT_FOUND / RXC_HOST_E_DIGEST). */
int rxc_host_record(RxcHost *h, uint64_t id, RxcHostRecord *out);

/* Payload words of record `id`: copies up to `max`, returns the count or <0. */
int rxc_host_payload(RxcHost *h, uint64_t id, uint64_t *out, uint32_t max);

/* Current state (opens the composition if not yet open). */
int rxc_host_info(RxcHost *h, RxcHostInfo *info);

/* Close the composition and its authority; frees the handle. NULL is a no-op. */
void rxc_host_close(RxcHost *h);

#ifdef __cplusplus
}
#endif
#endif /* RXC_HOST_ABI_H */
