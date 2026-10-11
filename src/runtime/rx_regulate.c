/* ADAPTIVE-M4: bounded resource regulation. Contract in rx_regulate.h. */
#include "runtime/rx_regulate.h"

#include <string.h>

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id) return &c->in[i];
    return NULL;
}

static void put(RxCtx *c, RxObjRef o, uint32_t field, uint64_t v) {
    c->out[c->n_out++] = (RxMutation){ o, field, v };
}

int rx_regulate_envelope_ok(const RxRegulateEnvelope *e) {
    if (!e) return RX_ERR_ARG;
    if (e->slots_min == 0 || e->slots_min > e->slots_max) return RX_ERR_ARG;
    if (e->memory_min > e->memory_max || e->energy_min > e->energy_max) return RX_ERR_ARG;
    if (e->base.slots < e->slots_min || e->base.slots > e->slots_max) return RX_ERR_ARG;
    if (e->base.memory_bytes < e->memory_min || e->base.memory_bytes > e->memory_max)
        return RX_ERR_ARG;
    if (e->base.energy_budget < e->energy_min || e->base.energy_budget > e->energy_max)
        return RX_ERR_ARG;
    if (e->contention_lo >= e->contention_hi) return RX_ERR_ARG;
    if (e->pressure_lo >= e->pressure_hi || e->pressure_hi > 100) return RX_ERR_ARG;
    if (e->min_dwell == 0) return RX_ERR_ARG;
    return RX_OK;
}

void rx_regulate_policy_for(const RxRegulateEnvelope *e, uint32_t regime, RxResourceBudget *out) {
    *out = e->base;
    if (regime == RX_REG_EXPAND) {
        out->slots = e->slots_max;
        out->memory_bytes = e->memory_max;
        out->energy_budget = e->energy_max;
    } else if (regime == RX_REG_PROTECT) {
        out->slots = e->slots_min;
        out->memory_bytes = e->memory_min;
        out->energy_budget = e->energy_min;
    }
}

uint32_t rx_regulate_decide(const RxRegulateEnvelope *e, uint32_t regime, uint64_t dwell,
                            uint64_t contention, uint64_t pressure) {
    int settled = dwell >= e->min_dwell;
    /* Protection enters at once and wins over expansion. */
    if (pressure >= e->pressure_hi) return RX_REG_PROTECT;
    switch (regime) {
    case RX_REG_PROTECT:
        if (!settled || pressure > e->pressure_lo) return RX_REG_PROTECT;
        return contention >= e->contention_hi ? RX_REG_EXPAND : RX_REG_NOMINAL;
    case RX_REG_EXPAND:
        if (!settled || contention > e->contention_lo) return RX_REG_EXPAND;
        return RX_REG_NOMINAL;
    default:
        return contention >= e->contention_hi ? RX_REG_EXPAND : RX_REG_NOMINAL;
    }
}

/* Policy written only here. A condition sample already decided on (same seq)
 * publishes nothing, so a re-run of the same activation is a no-op. */
static int fn_regulate(RxCtx *c) {
    RxRegulator *r = c->user;
    const RxSnapshotDep *cond = in_of(c, r->condition);
    const RxSnapshotDep *pol = in_of(c, r->policy);
    if (!cond || !pol) return RX_ERR_ARG;
    uint64_t cseq = cond->field[0];
    if (cseq == pol->field[5]) return 0;
    uint32_t regime = (uint32_t)pol->field[4];
    uint64_t dwell = pol->field[6];
    uint32_t next = rx_regulate_decide(&r->env, regime, dwell, cond->field[1], cond->field[3]);
    put(c, r->policy, 5, cseq);
    if (next == regime) {
        put(c, r->policy, 6, dwell + 1);
        return 0;
    }
    RxResourceBudget b;
    rx_regulate_policy_for(&r->env, next, &b);
    put(c, r->policy, 0, pol->field[0] + 1);
    put(c, r->policy, 1, b.slots);
    put(c, r->policy, 2, b.memory_bytes);
    put(c, r->policy, 3, b.energy_budget);
    put(c, r->policy, 4, next);
    put(c, r->policy, 6, 0);
    put(c, r->policy, 7, pol->field[7] + 1);
    return 0;
}

int rx_regulate_create_objects(RxRegulator *r, RxWorld *w, const RxRegulateEnvelope *e) {
    if (!r || !w) return RX_ERR_ARG;
    int rc = rx_regulate_envelope_ok(e);
    if (rc != RX_OK) return rc;
    memset(r, 0, sizeof(*r));
    r->w = w;
    r->env = *e;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    rc = rx_world_create(w, RX_OT_REG_CONDITION, RX_PERSIST_RESIDENT,
                         RX_REG_RES_BASE + RX_REG_RES_CONDITION, z, &r->condition);
    if (rc != RX_OK) return rc;
    uint64_t p[RX_MAX_FIELDS] = { 1, e->base.slots, e->base.memory_bytes, e->base.energy_budget,
                                  RX_REG_NOMINAL, 0, 0, 0 };
    return rx_world_create(w, RX_OT_REG_POLICY, RX_PERSIST_RESIDENT,
                           RX_REG_RES_BASE + RX_REG_RES_POLICY, p, &r->policy);
}

int rx_regulate_register(RxRegulator *r, const RxRegulateCaps *caps, const RxCallerCred *cred) {
    if (!r || !r->w || !caps) return RX_ERR_ARG;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "regulate.budget";
    d.faculty = RX_FACULTY_AIEN;
    d.subject = RX_REG_SUBJ;
    d.priority = RX_PRIO_CRITICAL;
    d.fn = fn_regulate;
    d.user = r;
    if (cred) d.caller = *cred;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ r->condition, RX_FIELD(0) };
    d.n_reads = 2;
    d.reads[0] = (RxDep){ r->condition, RX_ALL_FIELDS };
    d.reads[1] = (RxDep){ r->policy, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ r->policy, RX_ALL_FIELDS };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ caps->condition_read, RX_REG_RES_BASE + RX_REG_RES_CONDITION,
                             RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ caps->policy_rw, RX_REG_RES_BASE + RX_REG_RES_POLICY,
                             RX_RIGHT_READ | RX_RIGHT_WRITE };
    return rx_world_add_reaction(r->w, &d, &r->rid);
}

int rx_regulate_apply(RxRegulator *r) {
    if (!r || !r->w) return RX_ERR_ARG;
    RxHaltStatus h;
    rx_world_halt_status(r->w, &h);
    if (h.halted) { r->refused++; return RX_ERR_HALTED; }
    RxObject p;
    int rc = rx_world_read(r->w, r->policy, &p);
    if (rc != RX_OK) return rc;
    if (p.field[0] == r->applied_version) return 1;
    const RxRegulateEnvelope *e = &r->env;
    RxResourceBudget b = e->base;           /* fairness and offers: never regulated */
    b.slots = (uint32_t)p.field[1];
    b.memory_bytes = p.field[2];
    b.energy_budget = p.field[3];
    if (p.field[1] > UINT32_MAX || b.slots < e->slots_min || b.slots > e->slots_max ||
        b.memory_bytes < e->memory_min || b.memory_bytes > e->memory_max ||
        b.energy_budget < e->energy_min || b.energy_budget > e->energy_max ||
        p.field[4] > RX_REG_PROTECT) {
        r->refused++;
        return RX_ERR_BOUNDS;
    }
    rx_world_set_resources(r->w, &b);
    r->applied = b;
    r->applied_version = p.field[0];
    return RX_OK;
}
