/*
 * rx_skillroute.h -- Skill Router over the canonical Capability Graph (M20).
 *
 * A Skill is a provider of capabilities: one procedure (skill id, version,
 * content digest = AgSkill.identity) that realizes one or more semantic
 * operations. The router turns a work requirement into a target:
 *
 *   requirement (CqNeed: the capability needed, not an implementation)
 *     -> cq_compile / cq_query_admit over the Capability Graph (rx_capq)
 *     -> Skill providers of that capability (skill_id != 0)
 *     -> admissible targets: a local Skill must be in this machine's
 *        executable table with the digest the graph advertises; a remote one
 *        must be on a leased Fabric machine; authority is held when the
 *        requirement says so (otherwise compile/AEGIS decides, never here)
 *     -> ranked with the rx_capq order (Pareto front, caller's priorities,
 *        ids last), so the same graph and requirement give the same target.
 *
 * Ranking happens over admissible providers only (the admit function runs
 * before ranking), so a cheaper provider this machine cannot run never hides
 * one it can.
 *
 * The runtime path: an action-graph template leaves its AG_SKILL nodes
 * unbound (op 0: the engine fails such a node, there is no skill 0). The
 * caller states, per node, the capability it needs; sr_bind_node routes it
 * and binds the node to the chosen Skill (cq_bind_skill_node), then the usual
 * rx_graph_compile -> rx_graph_lower -> rx_graph_start runs it on the World.
 * A remote winner is returned with its canonical AienMachineId and is not
 * bound: sending work to it is the Fabric's job (SR_E_REMOTE).
 *
 * This router discovers; it does not authorize. It has no authority admin
 * handle and mints nothing; the build checks rx_skillroute.o for admin
 * symbols as it does for rx_capq.o.
 */
#ifndef RX_SKILLROUTE_H
#define RX_SKILLROUTE_H

#include "rx_capq.h"

#include <stdint.h>

#define SR_OK            0
#define SR_E_ARG       -40
#define SR_E_NO_CANDIDATE -41   /* nothing admissible provides the capability */
#define SR_E_REMOTE    -42      /* the winner is on another machine: hand it to the Fabric */
#define SR_E_QUERY     -43      /* the graph query failed (see route.query_verdict) */
#define SR_E_BIND      -44
/* COMPOSITION-2: revalidating a held route (sr_route_check / sr_bind_route). */
#define SR_E_STALE     -45      /* the entry changed since the route (generation, version, digest) */
#define SR_E_WITHDRAWN -46      /* the chosen provider was withdrawn (or is gone) */
#define SR_E_UNAVAILABLE -47    /* the chosen provider is registered but not usable now */
#define SR_E_MACHINE   -48      /* pinned or target machine unknown to the graph, or its lease ended */

/* SrRoute.why on SR_E_NO_CANDIDATE: every rejection class seen (bit set). */
#define SR_WHY_GONE          0x001u /* withdrawn, or its machine's lease ended */
#define SR_WHY_UNAVAILABLE   0x002u
#define SR_WHY_AUTHORITY     0x004u /* require_held and the authority is not held */
#define SR_WHY_NOT_SKILL     0x008u
#define SR_WHY_NOT_EXECUTABLE 0x010u
#define SR_WHY_DIGEST        0x020u /* local procedure differs from the graph */
#define SR_WHY_REMOTE_REFUSED 0x040u
#define SR_WHY_PIN           0x080u /* requirement pins another skill version or digest */
#define SR_WHY_MACHINE       0x100u /* provider not on the pinned machine */
#define SR_WHY_MACHINE_UNKNOWN 0x200u /* provider machine has no canonical identity */

typedef struct {
    CqCatalog *graph;                   /* the canonical Capability Graph */
    const AgSkillTable *skills;         /* procedures this machine can execute */
} SrRouter;

typedef struct {
    CqNeed need;                        /* need.semantic_operation: the required capability */
    CqTradeoffs t;                      /* ranking; t.require_held filters on held authority */
    uint32_t local_only;                /* 1: refuse remote targets */
    /* COMPOSITION-2 pins; zero = unpinned. */
    uint32_t pin_skill_version;         /* only this version of the provider's Skill */
    uint8_t pin_skill_digest[32];       /* only a provider advertising this AgSkill.identity */
    uint32_t pin_machine_set;           /* 1: only providers on pin_machine */
    AienMachineId pin_machine;          /* canonical; unknown to the graph = SR_E_MACHINE */
} SrRequirement;

typedef struct {
    int verdict;                        /* SR_OK, SR_E_REMOTE or an error */
    int query_verdict;                  /* CQ_* from the graph */
    CqCandidate chosen;
    uint8_t remote;
    uint8_t target_known;               /* 1: target holds the canonical identity */
    AienMachineId target;               /* canonical machine of the chosen provider */
    uint32_t skill_version;
    uint8_t skill_digest[32];
    uint8_t plan_digest[32];            /* cq_compile identity of the requirement */
    uint32_t n_admissible;
    CqStats stats;
    struct {
        uint64_t not_a_skill;           /* provider is not a Skill */
        uint64_t not_executable;        /* local Skill missing from the executable table */
        uint64_t digest_mismatch;       /* local procedure differs from what the graph says */
        uint64_t remote_refused;        /* local_only */
        uint64_t pin_mismatch;          /* requirement pins another version or digest */
        uint64_t machine_mismatch;      /* not on the pinned machine */
        uint64_t machine_unknown;       /* machine has no canonical identity (canonical graph) */
    } rejected;
    /* COMPOSITION-2: what the route was decided on, for sr_route_check. */
    CqKey key;                          /* graph key of the chosen provider */
    uint64_t generation;                /* its CqEntry.generation at routing time */
    uint32_t why;                       /* SR_WHY_* on SR_E_NO_CANDIDATE */
} SrRoute;

/* A Skill and the capabilities it provides. */
typedef struct {
    uint32_t skill_id, version;
    uint8_t digest[32];                 /* AgSkill.identity */
    uint32_t machine;                   /* index in the graph (self for a local Skill) */
} SrSkill;

/* Register every capability a Skill provides: each `provides[i]` is a full
 * CqEntry minus the Skill fields, which this fills (skill_id, skill_version,
 * skill_digest, machine_id; source SKILL on this machine, FABRIC elsewhere). */
int sr_register_skill(CqCatalog *graph, const SrSkill *skill, const CqEntry *provides, uint32_t n);

/* Skill -> capabilities: keys of the live entries this Skill provides, in
 * catalog order; returns the total count (may exceed max). */
uint32_t sr_skill_capabilities(const CqCatalog *graph, uint32_t skill_id, uint32_t version,
                               CqKey *out, uint32_t max);

/* Requirement -> chosen target. */
int sr_route(const SrRouter *r, const SrRequirement *req, const CqHeld *held, uint64_t now_us,
             SrRoute *out);

/* COMPOSITION-2: ranked admissible alternatives (dominated included), at
 * most `max`; out[0] is sr_route's choice. Returns the count or an SR_E_*. */
int sr_route_alternatives(const SrRouter *r, const SrRequirement *req, const CqHeld *held,
                          uint64_t now_us, SrRoute *out, uint32_t max);

/* Route, then bind action-graph node `node` (AG_SKILL/AG_RETRY) to the local
 * winner. SR_E_REMOTE leaves the node unbound and the route filled. Any
 * failure leaves the node as it was (unbound). */
int sr_bind_node(const SrRouter *r, AgGraph *g, uint32_t node, const SrRequirement *req,
                 const CqHeld *held, uint64_t now_us, SrRoute *out);

/* COMPOSITION-2: is a route the caller holds (from sr_route or
 * sr_route_alternatives) still what the graph says? Fails closed with
 * SR_E_WITHDRAWN, SR_E_UNAVAILABLE, SR_E_STALE (generation, version or digest
 * moved, or the local procedure no longer matches) or SR_E_MACHINE (lease
 * ended at now_us, identity gone). Authority is not decided here: binding declares it
 * and compile finds it held or missing. */
int sr_route_check(const SrRouter *r, const SrRoute *route, uint64_t now_us);

/* Check a held route, then bind node `node` to it. SR_E_REMOTE for a remote
 * route; any failure leaves the node unbound. */
int sr_bind_route(const SrRouter *r, AgGraph *g, uint32_t node, const SrRoute *route,
                  uint64_t now_us);

#endif /* RX_SKILLROUTE_H */
