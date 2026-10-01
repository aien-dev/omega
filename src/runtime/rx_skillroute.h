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

typedef struct {
    CqCatalog *graph;                   /* the canonical Capability Graph */
    const AgSkillTable *skills;         /* procedures this machine can execute */
} SrRouter;

typedef struct {
    CqNeed need;                        /* need.semantic_operation: the required capability */
    CqTradeoffs t;                      /* ranking; t.require_held filters on held authority */
    uint32_t local_only;                /* 1: refuse remote targets */
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
    } rejected;
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

/* Route, then bind action-graph node `node` (AG_SKILL/AG_RETRY) to the local
 * winner. SR_E_REMOTE leaves the node unbound and the route filled. */
int sr_bind_node(const SrRouter *r, AgGraph *g, uint32_t node, const SrRequirement *req,
                 const CqHeld *held, uint64_t now_us, SrRoute *out);

#endif /* RX_SKILLROUTE_H */
