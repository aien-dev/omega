/* ADAPTIVE-M4: bounded resource regulation as a resident reaction.
 *
 * The World already prices admission against an RxResourceBudget
 * (rx_world_set_resources: slots, memory, energy, fairness bound) and keeps
 * reactions stable (RxStabilityBudget). Nothing in the runtime adapts that
 * budget to what the machine is going through. This faculty closes that loop
 * without touching the scheduler:
 *
 *   condition object  (written from outside: a sensor, a supervisor, a test)
 *        |  trigger
 *   regulate.budget   (one reaction: reads the condition and the current
 *        |             policy, decides a regime with hysteresis, publishes
 *        v             a bounded policy)
 *   policy object     (written only by the regulator)
 *        |  rx_regulate_apply (host step, never inside a reaction)
 *        v
 *   rx_world_set_resources
 *
 * Authority independence. The envelope (slots, memory, energy minimum and
 * maximum, the nominal base budget, the fairness bound) is fixed when the
 * objects are created and copied into the regulator. No capability, caller
 * or object write can change it: a policy outside the envelope, however it
 * got into the object, is refused at apply and the budget in force stays.
 * The fields the regulator does not own (starvation_bound, locality,
 * accelerator and compute offers, logical tick) are taken from the base
 * budget on every apply, so fairness is never regulated away.
 *
 * Regimes. NOMINAL applies the base budget. EXPAND grants the envelope
 * maximum when contention (deferred admissions in the window) reaches
 * contention_hi and holds it until contention has fallen to contention_lo
 * and the regime has been held for min_dwell samples. PROTECT applies the
 * envelope minimum as soon as memory pressure reaches pressure_hi, takes
 * precedence over EXPAND, and is left the same way (pressure_lo, dwell).
 * Entering a regime is immediate; leaving needs the dead band and the dwell,
 * so a signal that chatters around a threshold cannot flip the budget more
 * than once per min_dwell + 1 samples and a signal inside the dead band
 * never flips it.
 *
 * Emergency stop. The regulator runs under its own subject, so it can never
 * stop or resume the world. While an operator stop is in force, outside
 * condition publications are refused by the World and rx_regulate_apply
 * refuses (RX_ERR_HALTED) without touching the budget.
 *
 * Objects. condition: 0 seq, 1 contention, 2 latency ns, 3 memory pressure
 * (0..100), 4 deadline overdue, 5 window tick. policy: 0 version (changes
 * only when the budget changes), 1 slots, 2 memory bytes, 3 energy budget,
 * 4 regime, 5 condition seq decided on, 6 dwell (samples in this regime),
 * 7 flips (regime changes so far). */
#ifndef RX_REGULATE_H
#define RX_REGULATE_H

#include "runtime/rx_world.h"

#define RX_REG_SUBJ      37u
#define RX_REG_RES_BASE  0x5340000ull
enum { RX_REG_RES_CONDITION = 0, RX_REG_RES_POLICY, RX_REG_RES_COUNT };
enum { RX_OT_REG_CONDITION = 0x5340u, RX_OT_REG_POLICY };
enum { RX_REG_NOMINAL = 0, RX_REG_EXPAND = 1, RX_REG_PROTECT = 2 };

typedef struct {
    uint32_t slots_min, slots_max;
    uint64_t memory_min, memory_max;
    uint64_t energy_min, energy_max;
    RxResourceBudget base;          /* NOMINAL; must lie inside the bounds above */
    uint64_t contention_hi, contention_lo;   /* lo < hi */
    uint64_t pressure_hi, pressure_lo;       /* lo < hi <= 100 */
    uint64_t min_dwell;                      /* >= 1 */
} RxRegulateEnvelope;

typedef struct {
    RxCapRef condition_read;        /* RX_REG_SUBJ on condition, READ */
    RxCapRef policy_rw;             /* RX_REG_SUBJ on policy, READ|WRITE */
} RxRegulateCaps;

typedef struct {
    RxWorld *w;
    RxRegulateEnvelope env;
    RxObjRef condition, policy;
    uint32_t rid;
    uint64_t applied_version;       /* policy version in force (0: none applied yet) */
    RxResourceBudget applied;       /* the budget rx_regulate_apply last installed */
    uint64_t refused;               /* applies refused: outside the envelope or halted */
} RxRegulator;

/* RX_OK when every bound, the base budget and the thresholds are consistent. */
int rx_regulate_envelope_ok(const RxRegulateEnvelope *e);

/* Create the condition and policy objects (resources RX_REG_RES_BASE + i).
 * The policy starts as NOMINAL, version 1. Copies the envelope. */
int rx_regulate_create_objects(RxRegulator *r, RxWorld *w, const RxRegulateEnvelope *e);

/* Register regulate.budget under RX_REG_SUBJ. `cred` is the subject's
 * caller credential in a world that binds callers; null otherwise. */
int rx_regulate_register(RxRegulator *r, const RxRegulateCaps *caps, const RxCallerCred *cred);

/* Host step. Reads the policy and installs it with rx_world_set_resources
 * when its version is new and it lies inside the envelope. RX_OK applied;
 * 1 nothing new; RX_ERR_HALTED an operator stop is in force (budget
 * untouched); RX_ERR_BOUNDS the policy is outside the envelope (budget
 * untouched, counted in `refused`). Never call from a reaction or with the
 * world lock held. */
int rx_regulate_apply(RxRegulator *r);

/* The pure decision, exposed so a test can drive it without a world. */
uint32_t rx_regulate_decide(const RxRegulateEnvelope *e, uint32_t regime, uint64_t dwell,
                            uint64_t contention, uint64_t pressure);
/* The budget a regime stands for inside `e`. */
void rx_regulate_policy_for(const RxRegulateEnvelope *e, uint32_t regime, RxResourceBudget *out);

#endif
