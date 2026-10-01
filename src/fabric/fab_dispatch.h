/*
 * fab_dispatch.h -- the Fabric side of a living composition (F5-0, loopback).
 *
 * rx_compose runs a candidate whose route is remote (SR_E_REMOTE: the Skill
 * Router chose a CQ_SRC_FABRIC provider) through an RxcRemoteRun hook; this
 * is that hook for F5-0. Before every dispatch, under one lock:
 *   1. waiting Fabric traffic is applied (fab_poll) and the clock with it
 *      (fab_tick), so a LEAVE or a lease that ended since routing withdraws
 *      the machine's entries first;
 *   2. the route is revalidated against the Capability Graph: sr_route_check
 *      must still say SR_E_REMOTE (entry live, same generation, version and
 *      digest, machine identity unchanged, lease live at now);
 *   3. the route's machine must be a live member of this node (fab_home ==
 *      JS_HOME_REMOTE_OWNED): never the local machine, never a lost one;
 *   4. that machine must run exactly the procedure it advertised: its
 *      executor has the route's skill id with the advertised digest.
 * Any failure refuses (nonzero) and the candidate proposes nothing; the
 * result of a dispatch is only a candidate, checked by the AEGIS verifier.
 *
 * Stand-in, stated plainly: F5-0 has no work message (its wire carries JOIN,
 * RENEW, ADVERTISE and LEAVE only), so step 4 calls the simulated machine's
 * procedure table in this process. Checks 1-3 are the ones a real dispatch
 * makes before it sends; sending the work and authenticating the reply is
 * the job of the AIENOS M6 transport (through AEGIS as an outbound effect).
 * The dispatcher mints no authority and touches no World (fabric-purity).
 *
 * Concurrency: fab_dispatch_run may be called by two candidate reactions at
 * once; all Fabric traffic and catalog changes while a composition can run
 * go through fab_dispatch_pump / fab_dispatch_run (same lock). The catalog
 * belongs to the composition: rx_compose_run routes (reads it) before any
 * candidate runs, and only candidates call fab_dispatch_run during a run, so
 * nothing else may pump this node while rx_compose_run is active (a
 * background pump would race the routing read). The procedure runs under
 * the lock: it must not call back into the dispatcher. The route is checked
 * at the goal's time (rx_compose_run now_us), the composition's only clock.
 */
#ifndef FAB_DISPATCH_H
#define FAB_DISPATCH_H

#include "fabric.h"
#include "runtime/rx_skillroute.h"

#include <pthread.h>
#include <stdint.h>

#define FAB_DISPATCH_MAX 8u

/* Refusal reasons (FabDispatch.refused[...], last_refusal). */
enum {
    FAB_DX_OK = 0,
    FAB_DX_ARG,              /* bad call, or the router's graph is not this node's catalog */
    FAB_DX_ROUTE,            /* sr_route_check is no longer SR_E_REMOTE (withdrawn, stale, lease) */
    FAB_DX_TARGET,           /* route has no canonical target machine */
    FAB_DX_NOT_MEMBER,       /* target is not a live member of this node (or is this machine) */
    FAB_DX_NO_EXECUTOR,      /* target runs no such Skill */
    FAB_DX_DIGEST,           /* target's procedure differs from what it advertised */
    FAB_DX_FAILED,           /* the procedure reported failure */
    FAB_DX_TRANSPORT,        /* applying waiting Fabric traffic failed (transport error) */
    FAB_DX_N
};

typedef struct {
    AienMachineId machine;
    const AgSkillTable *skills;     /* the procedures that machine runs */
} FabExecutor;

typedef struct {
    FabNode *node;                  /* this machine's node; its catalog is the router's graph */
    pthread_mutex_t mu;
    uint32_t n_exec;
    FabExecutor exec[FAB_DISPATCH_MAX];
    uint64_t dispatched;
    uint64_t refused[FAB_DX_N];
    int last_refusal;               /* FAB_DX_* of the last refusal */
    uint64_t polled, refused_msgs;  /* Fabric messages applied by the dispatcher / refused */
} FabDispatch;

int  fab_dispatch_init(FabDispatch *d, FabNode *node);
void fab_dispatch_destroy(FabDispatch *d);
/* The simulated machine m runs `skills` (loopback stand-in for its executor). */
int  fab_dispatch_add(FabDispatch *d, const AienMachineId *m, const AgSkillTable *skills);
/* Apply every waiting Fabric message and the clock, under the lock. Returns
 * messages applied, or <0 on a transport error. */
int  fab_dispatch_pump(FabDispatch *d, uint64_t now_us);
/* RxcRemoteRun (rx_compose.h): ctx is the FabDispatch. 0 = ran, else FAB_DX_*. */
int  fab_dispatch_run(void *ctx, const SrRouter *router, const SrRoute *route, uint64_t input,
                      uint64_t now_us, uint64_t *result);

#endif /* FAB_DISPATCH_H */
