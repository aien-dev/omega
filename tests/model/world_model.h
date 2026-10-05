/*
 * world_model.h -- small explicit state machine of the rx_world activation
 * lifecycle (hardening lane LG).
 *
 *   wake -> admit (charge budget) -> run -> commit | invalidate | fail
 *        -> reclaim (uncharge) -> idle, plus cancel of a parent and its
 *        children.
 *
 * Four reactions in a fixed tree, written by the outside world into E:
 *
 *        E --> P --> X --> C1 --> Y --> G
 *                      \-> C2 --> Z
 *
 * The model has two roles:
 *   - SPEC: what the lifecycle should guarantee (cancel reaches every child,
 *     a deadline overrun is surfaced while the activation is still running).
 *   - AS_BUILT: what src/runtime/rx_world.c does today, step for step
 *     (admission order, re-arm, invalidation back-off, parking). This is the
 *     profile the differential harness compares against the real runtime.
 * Mutants are SPEC with one realistic fault switched on; each must make the
 * invariant it targets fail.
 */
#ifndef WORLD_MODEL_H
#define WORLD_MODEL_H

#include <stdint.h>

#define WM_N        4u          /* reactions: P, C1, C2, G */
#define WM_P        0u
#define WM_C1       1u
#define WM_C2       2u
#define WM_G        3u
#define WM_SLOTS    2u          /* admission slots in the budget */
#define WM_MEM      8u          /* memory budget */
#define WM_EXT_MAX  3u          /* outside publications per explored run */
#define WM_TICK_MAX 3u          /* logical clock bound */

/* Topology and needs (shared with the differential harness). */
extern const int      wm_parent[WM_N];
extern const uint32_t wm_mem[WM_N];
extern const uint32_t wm_deadline[WM_N];   /* 0 = none */
extern const char    *wm_name[WM_N];

enum { WM_IDLE = 0, WM_WAITING, WM_RUN, WM_ENDING };

enum { OP_EXT = 0, OP_FIN, OP_FAIL, OP_TICK, OP_CANCEL, OP_KINDS };
typedef struct { uint8_t kind, r; } WmOp;

/* Invariant violation bits. */
#define V_LEAK          0x01u   /* I1 budget held != budget of running work */
#define V_CANCEL_CHILD  0x02u   /* I2 cancel left a descendant in flight */
#define V_DOUBLE        0x04u   /* I3 one activation committed twice */
#define V_DEADLINE      0x08u   /* I4 running past its deadline, not surfaced */
#define V_CANCEL_SELF   0x10u   /* I2b cancel left the target itself in flight */

enum { CANCEL_NONE = 0, CANCEL_TRANSITIVE, CANCEL_TARGET_ONLY };

typedef struct {
    const char *name;
    int cancel_mode;
    int deadline_on_tick;       /* surface overrun when the clock passes it */
    int mut_leak_on_fail;       /* failure path forgets to reclaim the budget */
    int mut_commit_keeps_act;   /* commit path forgets to end the activation */
} WmProfile;

extern const WmProfile wm_spec, wm_as_built;
extern const WmProfile wm_mut_leak, wm_mut_noprop, wm_mut_double, wm_mut_deadline;

/* Behaviour-relevant state (the dedup key is derived from it). */
typedef struct {
    uint8_t st[WM_N];
    uint8_t rearm[WM_N], stale[WM_N], parked[WM_N], yield[WM_N];
    uint8_t surfaced[WM_N], act_committed[WM_N];
    uint32_t wait_seq[WM_N];
    uint32_t seq;
    uint8_t ext, tick;
    uint8_t used_slots;
    uint32_t used_mem;
    uint8_t in_flight;
    /* Observables, not part of the key. */
    uint32_t commits[WM_N], invalidations, failures, overdue, cancels;
    uint32_t viol;              /* violations raised by the last step */
} WmState;

void wm_init(WmState *s);
int  wm_enabled(const WmProfile *p, const WmState *s, WmOp op);
/* Applies op, then checks every invariant; returns s->viol. */
uint32_t wm_step(const WmProfile *p, WmState *s, WmOp op);
/* Canonical key bytes (wait order normalised); returns length. */
int  wm_key(const WmState *s, uint8_t *out);
int  wm_is_descendant(uint32_t d, uint32_t r);
void wm_op_str(WmOp op, char *buf, int n);
/* All candidate ops in a fixed order; returns count. */
int  wm_all_ops(WmOp *out);

#endif
