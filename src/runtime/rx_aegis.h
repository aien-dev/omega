/*
 * rx_aegis.h -- AEGIS resident authority (ADR 0016 §41, R8).
 *
 * Authority relationships are world objects. Each client principal has
 * capability slots: objects that hold the reference it presents for one
 * resource. A reaction declares a slotted capability need, and the world
 * reads the reference from the slot at every check (rx_world.h,
 * cap_slotted). Holding a slot is not permission; the native AIENOS authority
 * still validates the reference.
 *
 *   fast path   slot holds a live reference -> the reaction runs. No AEGIS
 *               activation, no policy round trip.
 *   slow path   client publishes a request -> aegis.decide.k applies policy
 *               (GRANT, DENY, ESCALATE to a human, REVOKE on release)
 *               -> root.install.k mints through the AIENOS root and writes
 *               the slot -> the client's reaction wakes on the slot.
 *
 * Nothing here is called. The client does not wait for AEGIS; its reaction
 * is simply not ready until its slot holds authority.
 *
 * Mint authentication. root.install is the only holder of the AIENOS admin.
 * It mints only when
 *   - every decision field it uses was last written by that client's
 *     aegis.decide reaction (subject RX_AEGIS_SUBJ, faculty AEGIS),
 *   - every request field was last written by a reaction of the client's
 *     own subject, and the decision echoes that request, and
 *   - the grant stays inside the client's domain and rights ceiling and
 *     carries no privileged right.
 * So a forged decision, a request written for someone else, or a policy
 * mistake that widens a grant beyond the client's domain is refused by the
 * root, whatever AEGIS said. AEGIS decides policy. The root decides only
 * what may exist.
 */
#ifndef RX_AEGIS_H
#define RX_AEGIS_H

#include "aienos_cap.h"
#include "rx_world.h"

#include <pthread.h>
#include <stdint.h>

#define RX_AEGIS_CLIENTS 4u
#define RX_AEGIS_SLOTS   4u
#define RX_AEGIS_RULES   16u

enum { RX_OT_AUTH_REQUEST = 0x5810u, RX_OT_AUTH_APPROVAL, RX_OT_AUTH_DECISION, RX_OT_CAP_SLOT };

/* request:  0 seq, 1 resource, 2 rights, 3 lease ticks (0 = policy default),
 *           4 slot index, 5 op                                       (client)
 * approval: 0 request seq, 1 verdict                                 (external, human)
 * decision: 0 request seq, 1 verdict, 2 resource, 3 rights, 4 lease ticks,
 *           5 slot index, 6 rule id or reason, 7 op                  (AEGIS)
 * slot:     0 cap id, 1 generation, 2 state, 3 resource, 4 rights,
 *           5 decision seq it answers, 6 lease expiry (0 = none),
 *           7 root refusal reason (0 = none)                         (root) */
enum { RX_AEGIS_OP_ACQUIRE = 1, RX_AEGIS_OP_RELEASE };
enum { RX_AEGIS_APPROVE = 1, RX_AEGIS_REJECT };
enum { RX_AEGIS_GRANT = 1, RX_AEGIS_DENY, RX_AEGIS_ESCALATE, RX_AEGIS_REVOKE };
enum { RX_AEGIS_SLOT_EMPTY = 0, RX_AEGIS_SLOT_LIVE, RX_AEGIS_SLOT_REVOKED };

/* Reasons in decision field 6 when not a rule id, and in slot field 7. */
enum {
    RX_AEGIS_WHY_NO_RULE = 1000,   /* no rule covers subject, resource and rights */
    RX_AEGIS_WHY_PRIVILEGED,       /* asked for a privileged right */
    RX_AEGIS_WHY_HUMAN,            /* a human rejected it */
    RX_AEGIS_WHY_BAD_REQUEST,      /* slot out of range, unknown op, zero rights */
    /* root refusals */
    RX_AEGIS_WHY_DECISION_ORIGIN,  /* a decision field was not written by AEGIS */
    RX_AEGIS_WHY_REQUEST_ORIGIN,   /* a request field was not written by the client */
    RX_AEGIS_WHY_MISMATCH,         /* the decision does not echo the request */
    RX_AEGIS_WHY_DOMAIN,           /* outside the client's domain or rights ceiling */
    RX_AEGIS_WHY_MINT              /* the AIENOS root refused the mint */
};

/* One policy rule: subject may hold at most `rights` on [res_lo, res_hi]. */
typedef struct {
    uint32_t id;
    uint32_t subject;
    uint64_t res_lo, res_hi;
    uint32_t rights;
    uint64_t max_lease;            /* 0 = no lease */
    uint32_t needs_approval;
} RxAegisRule;

typedef struct {
    uint32_t n_rules;
    RxAegisRule rules[RX_AEGIS_RULES];
} RxAegisPolicy;

/* What the root will ever let exist for a client, whatever AEGIS decides. */
typedef struct {
    uint32_t subject;
    uint64_t domain_lo, domain_hi;
    uint32_t max_rights;
} RxAegisClient;

typedef struct {
    RxObjRef request, approval, decision;
    RxObjRef slot[RX_AEGIS_SLOTS];
} RxAegisClientObjects;

/* Resources: RX_AEGIS_RES_BASE + client * RX_AEGIS_RES_STRIDE + index. */
#define RX_AEGIS_RES_BASE   0x5810000ull
#define RX_AEGIS_RES_STRIDE 0x100ull
enum { RX_AEGIS_RES_REQUEST = 0, RX_AEGIS_RES_APPROVAL, RX_AEGIS_RES_DECISION, RX_AEGIS_RES_SLOT0 };

static inline uint64_t rx_aegis_res(uint32_t client, uint32_t index) {
    return RX_AEGIS_RES_BASE + client * RX_AEGIS_RES_STRIDE + index;
}

enum { RX_AEGIS_SUBJ = 41, RX_AEGIS_ROOT_SUBJ = 42 };

/* Capability references the two faculties run under, per client. */
typedef struct {
    RxCapRef aegis_request, aegis_approval, aegis_decision;    /* R, R, RW */
    RxCapRef root_request, root_decision;                       /* R, R */
    RxCapRef root_slot[RX_AEGIS_SLOTS];                         /* RW */
} RxAegisCaps;

typedef struct {
    uint64_t seq;
    RxCapRef ref;
} RxAegisMinted;

typedef struct RxAegisFaculty {
    RxWorld *w;
    AienosCapAdmin *admin;         /* only root.install uses it */
    RxAegisPolicy policy;
    uint32_t n_clients;
    RxAegisClient client[RX_AEGIS_CLIENTS];
    RxAegisClientObjects o[RX_AEGIS_CLIENTS];
    uint32_t r_decide[RX_AEGIS_CLIENTS], r_install[RX_AEGIS_CLIENTS];
    struct { struct RxAegisFaculty *f; uint32_t k; } ctx[RX_AEGIS_CLIENTS];

    pthread_mutex_t mu;            /* guards the root's record below */
    RxAegisMinted minted[RX_AEGIS_CLIENTS][RX_AEGIS_SLOTS];

    /* Observability only. */
    uint64_t mints, revokes, root_refusals;
} RxAegisFaculty;

/* Pure policy evaluation. Returns a verdict; *rule_or_why gets the rule id or
 * a reason, *lease the granted lease ticks. Also used as the synchronous
 * baseline when measuring what the fast path saves. */
uint32_t rx_aegis_evaluate(const RxAegisPolicy *p, uint32_t subject, uint64_t resource,
                           uint32_t rights, uint64_t lease, uint64_t *out_lease,
                           uint32_t *rule_or_why, uint32_t *needs_approval);

int rx_aegis_create(RxAegisFaculty *f, RxWorld *w, AienosCapAdmin *admin,
                    const RxAegisPolicy *policy, const RxAegisClient *clients, uint32_t n);

int rx_aegis_register(RxAegisFaculty *f, const RxAegisCaps caps[]);

void rx_aegis_destroy(RxAegisFaculty *f);

/* Make caps[i] of a client reaction read its reference from a slot, and let
 * the slot wake the reaction. Call before rx_world_add_reaction. */
void rx_aegis_use_slot(RxReactionDesc *d, uint32_t i, RxObjRef slot);

#endif /* RX_AEGIS_H */
