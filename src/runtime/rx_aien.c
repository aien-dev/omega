/*
 * rx_aien.c -- AIEN as a resident cognitive faculty (ADR 0016 §44, R11).
 *
 * See rx_aien.h for the object and reaction map. Every fn_* is a reaction
 * body: it reads its versioned snapshot and proposes mutations. AIEN keeps
 * no state outside the world, so a reaction that is invalidated and runs
 * again proposes the same thing from the same inputs.
 */
#include "rx_aien.h"

#include <string.h>

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id) return &c->in[i];
    return NULL;
}

static void put(RxCtx *c, RxObjRef o, uint32_t field, uint64_t v) {
    c->out[c->n_out++] = (RxMutation){ o, field, v };
}

static int within(uint64_t x, uint64_t mean, uint32_t tol_pct) {
    return x * 100u <= mean * (100u + tol_pct) && x * 100u >= mean * (100u - tol_pct);
}

/* A failed prediction is settled when explain has dealt with it and nothing
 * is left to try: its own hypothesis was exhausted, or it refuted the
 * hypothesis it was testing. An open or testing hypothesis is not settled:
 * its test is the next record's prediction. */
static int failure_settled(const RxSnapshotDep *p, const RxSnapshotDep *h) {
    if (p->field[6] != RX_AIEN_PRED_FAILED) return 0;
    if (h->field[7] == RX_AIEN_HYP_EXHAUSTED) return h->field[2] == p->field[0];
    if (h->field[7] == RX_AIEN_HYP_UNSUPPORTED) return h->field[2] < p->field[0];
    return 0;
}

static int explored(const RxSnapshotDep *mem, uint64_t key) {
    uint64_t n = mem->field[0] < RX_AIEN_MEMORY_SLOTS ? mem->field[0] : RX_AIEN_MEMORY_SLOTS;
    for (uint64_t i = 0; i < n; i++)
        if (mem->field[1 + i] == key) return 1;
    return 0;
}

/* ---- aien.observe: what production actually pays under the current record ----
 *
 * Wakes on each of Omega's demand windows and on a new selection. The cost
 * of an interval is the difference of two demand snapshots, so it is exact
 * however many calls landed between wakes. The first interval after a new
 * record is discarded: it may straddle the switch. */
static int fn_observe(RxCtx *c) {
    RxAienFaculty *f = c->user;
    const RxSnapshotDep *dem = in_of(c, f->in.demand);
    const RxSnapshotDep *sel = in_of(c, f->in.selection);
    const RxSnapshotDep *b = in_of(c, f->o.belief);
    if (!dem || !sel || !b) return -1;
    uint64_t epoch = sel->field[0], real = sel->field[1];
    if (epoch == 0) return 0;                      /* no record yet: nothing to believe about */
    if (rx_aien_regime(dem->field[4], dem->field[5]) != sel->field[6]) return 0;
    uint64_t calls = dem->field[0], spent = dem->field[1];

    if (b->field[0] != epoch || b->field[1] != real || calls < b->field[2]) {
        put(c, f->o.belief, 0, epoch);
        put(c, f->o.belief, 1, real);
        put(c, f->o.belief, 2, calls);
        put(c, f->o.belief, 3, spent);
        put(c, f->o.belief, 4, 0);
        put(c, f->o.belief, 5, 0);
        put(c, f->o.belief, 6, 0);
        put(c, f->o.belief, 7, 0);
        return 0;
    }
    /* A prediction about this record failed on this belief and the failure
     * is settled (explained, nothing left to try). The belief has served its
     * purpose: relearn what this record costs from fresh intervals. Without
     * this a failed belief stays failed until a new record exists, and a
     * later, real change (a core move) could never be noticed. */
    const RxSnapshotDep *p = in_of(c, f->o.prediction);
    const RxSnapshotDep *h = in_of(c, f->o.hypothesis);
    if (!p || !h) return -1;
    if (p->field[1] == epoch && b->field[7] >= f->cfg.misses_to_fail && failure_settled(p, h)) {
        put(c, f->o.belief, 2, calls);
        put(c, f->o.belief, 3, spent);
        put(c, f->o.belief, 4, 0);
        put(c, f->o.belief, 5, 0);
        put(c, f->o.belief, 6, 0);
        put(c, f->o.belief, 7, 0);
        return 0;
    }
    uint64_t dc = calls - b->field[2];
    if (dc < f->cfg.min_calls) return 0;           /* too little evidence: wait */
    if (dem->field[6] != real) {                   /* production has not switched yet */
        put(c, f->o.belief, 2, calls);
        put(c, f->o.belief, 3, spent);
        return 0;
    }
    uint64_t x = (spent - b->field[3]) / dc;
    uint64_t seen = b->field[5], mean = b->field[4], miss = b->field[7];
    if (seen == 0) {
        /* warm-up interval, discarded */
    } else if (seen <= f->cfg.baseline_intervals) {
        mean = (mean * (seen - 1) + x) / seen;
    } else {
        miss = within(x, mean, f->cfg.tolerance_pct) ? 0 : miss + 1;
    }
    put(c, f->o.belief, 2, calls);
    put(c, f->o.belief, 3, spent);
    put(c, f->o.belief, 4, mean);
    put(c, f->o.belief, 5, seen + 1);
    put(c, f->o.belief, 6, x);
    put(c, f->o.belief, 7, miss);
    return 0;
}

/* ---- aien.predict: turn the belief into an explicit, checkable prediction ---- */
static int fn_predict(RxCtx *c) {
    RxAienFaculty *f = c->user;
    const RxSnapshotDep *b = in_of(c, f->o.belief);
    const RxSnapshotDep *p = in_of(c, f->o.prediction);
    const RxSnapshotDep *pl = in_of(c, f->o.placement);
    const RxSnapshotDep *sel = in_of(c, f->in.selection);
    if (!b || !p || !pl || !sel) return -1;
    if (b->field[0] == 0 || b->field[5] <= f->cfg.baseline_intervals) return 0;
    if (b->field[0] != sel->field[0]) return 0;    /* belief is about an older record */

    if (p->field[0] == 0 || p->field[1] != b->field[0]) {
        put(c, f->o.prediction, 0, p->field[0] + 1);
        put(c, f->o.prediction, 1, b->field[0]);
        put(c, f->o.prediction, 2, sel->field[6]);
        put(c, f->o.prediction, 3, 0);
        put(c, f->o.prediction, 4, pl->field[1]);
        put(c, f->o.prediction, 5, b->field[4]);
        put(c, f->o.prediction, 6, RX_AIEN_PRED_HOLDING);
        put(c, f->o.prediction, 7, 0);
        return 0;
    }
    if (p->field[6] == RX_AIEN_PRED_FAILED) {
        /* Settled, and observe has relearned the same record from fresh
         * intervals (its misses were cleared): that belief becomes a fresh
         * prediction, so the next real change is noticed. */
        const RxSnapshotDep *h = in_of(c, f->o.hypothesis);
        if (!h) return -1;
        if (failure_settled(p, h) && b->field[0] == p->field[1] &&
            b->field[7] < f->cfg.misses_to_fail) {
            put(c, f->o.prediction, 0, p->field[0] + 1);
            put(c, f->o.prediction, 1, b->field[0]);
            put(c, f->o.prediction, 2, sel->field[6]);
            put(c, f->o.prediction, 3, 0);
            put(c, f->o.prediction, 4, pl->field[1]);
            put(c, f->o.prediction, 5, b->field[4]);
            put(c, f->o.prediction, 6, RX_AIEN_PRED_HOLDING);
            put(c, f->o.prediction, 7, 0);
        }
        return 0;
    }
    if (b->field[7] >= f->cfg.misses_to_fail) {
        put(c, f->o.prediction, 6, RX_AIEN_PRED_FAILED);
        put(c, f->o.prediction, 7, b->field[6]);
        return 0;
    }
    if (b->field[5] == f->cfg.baseline_intervals + 1) return 0;   /* nothing tested yet */
    if (b->field[7] != 0) {                        /* one miss: not yet a failure */
        put(c, f->o.prediction, 3, 0);
        return 0;
    }
    uint64_t hits = p->field[3] + 1;
    put(c, f->o.prediction, 3, hits);
    if (p->field[6] == RX_AIEN_PRED_HOLDING && hits >= f->cfg.confirm_intervals)
        put(c, f->o.prediction, 6, RX_AIEN_PRED_CONFIRMED);
    return 0;
}

/* ---- aien.explain: form a hypothesis for a failed prediction, test it later ---- */
static int fn_explain(RxCtx *c) {
    RxAienFaculty *f = c->user;
    const RxSnapshotDep *p = in_of(c, f->o.prediction);
    const RxSnapshotDep *h = in_of(c, f->o.hypothesis);
    const RxSnapshotDep *pl = in_of(c, f->o.placement);
    if (!p || !h || !pl) return -1;
    uint64_t pseq = p->field[0], state = p->field[6];
    if (pseq == 0) return 0;

    /* A later prediction under the hypothesized condition tests it. */
    if (h->field[7] == RX_AIEN_HYP_TESTING && pseq > h->field[2] && p->field[4] == h->field[4]) {
        if (state == RX_AIEN_PRED_CONFIRMED) put(c, f->o.hypothesis, 7, RX_AIEN_HYP_SUPPORTED);
        else if (state == RX_AIEN_PRED_FAILED) put(c, f->o.hypothesis, 7, RX_AIEN_HYP_UNSUPPORTED);
        return 0;
    }
    if (state != RX_AIEN_PRED_FAILED || h->field[2] == pseq) return 0;
    uint64_t now = pl->field[1], then = p->field[4];
    put(c, f->o.hypothesis, 0, h->field[0] + 1);
    put(c, f->o.hypothesis, 1, now != then ? RX_AIEN_HYP_CORE_CLASS : RX_AIEN_HYP_DRIFT);
    put(c, f->o.hypothesis, 2, pseq);
    put(c, f->o.hypothesis, 3, then);
    put(c, f->o.hypothesis, 4, now);
    put(c, f->o.hypothesis, 5, p->field[5]);
    put(c, f->o.hypothesis, 6, p->field[7]);
    put(c, f->o.hypothesis, 7, RX_AIEN_HYP_OPEN);
    return 0;
}

/* ---- aien.assess: is a human goal met by what AIEN now expects? ---- */
static int fn_assess(RxCtx *c) {
    RxAienFaculty *f = c->user;
    const RxSnapshotDep *g = in_of(c, f->o.goal);
    const RxSnapshotDep *p = in_of(c, f->o.prediction);
    const RxSnapshotDep *m = in_of(c, f->o.memory);
    const RxSnapshotDep *pl = in_of(c, f->o.placement);
    if (!g || !p || !m || !pl) return -1;
    if (g->field[0] == 0) return 0;
    uint64_t status = RX_AIEN_GOAL_UNKNOWN, expected = 0;
    if (p->field[0] != 0 && p->field[2] == g->field[1] && p->field[6] == RX_AIEN_PRED_CONFIRMED &&
        p->field[4] == pl->field[1]) {
        expected = p->field[5];
        if (expected <= g->field[2]) status = RX_AIEN_GOAL_MET;
        else if (explored(m, rx_aien_key(pl->field[1], g->field[1]))) status = RX_AIEN_GOAL_UNMET_EXPLORED;
        else status = RX_AIEN_GOAL_UNMET;
    } else if (p->field[0] != 0 && p->field[2] == g->field[1] &&
               p->field[6] == RX_AIEN_PRED_CONFIRMED) {
        /* A known cost on another core is evidence that the old regime did
         * not meet the goal. The new core remains unknown, which itself can
         * justify one bounded research plan. */
        expected = p->field[5];
    }
    put(c, f->o.assessment, 0, g->field[0]);
    put(c, f->o.assessment, 1, g->field[1]);
    put(c, f->o.assessment, 2, g->field[2]);
    put(c, f->o.assessment, 3, expected);
    put(c, f->o.assessment, 4, status);
    put(c, f->o.assessment, 5, pl->field[1]);
    put(c, f->o.assessment, 6, p->field[0]);
    return 0;
}

/* ---- aien.plan: one experiment per unexplored condition ---- */
static void remember(RxCtx *c, RxAienFaculty *f, const RxSnapshotDep *m, uint64_t key) {
    uint64_t n = m->field[0];
    /* Oldest entry is overwritten once full; the count keeps growing. */
    put(c, f->o.memory, 1 + (uint32_t)(n % RX_AIEN_MEMORY_SLOTS), key);
    put(c, f->o.memory, 0, n + 1);
}

static void publish_plan(RxCtx *c, RxAienFaculty *f, const RxSnapshotDep *pn, uint64_t regime,
                         uint64_t cls, uint64_t hseq, uint64_t why, uint64_t gseq) {
    put(c, f->o.plan, 0, pn->field[0] + 1);
    put(c, f->o.plan, 1, RX_AIEN_ACT_RESEARCH);
    put(c, f->o.plan, 2, regime);
    put(c, f->o.plan, 3, cls);
    put(c, f->o.plan, 4, hseq);
    put(c, f->o.plan, 5, why);
    put(c, f->o.plan, 6, gseq);
}

static int fn_plan(RxCtx *c) {
    RxAienFaculty *f = c->user;
    const RxSnapshotDep *h = in_of(c, f->o.hypothesis);
    const RxSnapshotDep *a = in_of(c, f->o.assessment);
    const RxSnapshotDep *m = in_of(c, f->o.memory);
    const RxSnapshotDep *p = in_of(c, f->o.prediction);
    const RxSnapshotDep *pn = in_of(c, f->o.plan);
    if (!h || !a || !m || !p || !pn) return -1;

    if (a->field[0] != 0 &&
        (a->field[4] == RX_AIEN_GOAL_UNMET ||
         (a->field[4] == RX_AIEN_GOAL_UNKNOWN &&
          a->field[3] > a->field[2])) &&
        pn->field[6] != a->field[0]) {
        uint64_t key = rx_aien_key(a->field[5], a->field[1]);
        if (!explored(m, key)) {
            publish_plan(c, f, pn, a->field[1], a->field[5], 0,
                         RX_AIEN_WHY_GOAL, a->field[0]);
            remember(c, f, m, key);
            return 0;
        }
    }

    if (h->field[7] == RX_AIEN_HYP_OPEN) {
        if (p->field[0] != h->field[2]) return 0;  /* prediction moved on; wait for it */
        uint64_t regime = p->field[2], cls = h->field[4];
        uint64_t key = rx_aien_key(cls, regime);
        if (explored(m, key)) {
            put(c, f->o.hypothesis, 7, RX_AIEN_HYP_EXHAUSTED);
            return 0;
        }
        uint64_t why = h->field[1] == RX_AIEN_HYP_CORE_CLASS ? RX_AIEN_WHY_CORE_CLASS
                                                             : RX_AIEN_WHY_DRIFT;
        publish_plan(c, f, pn, regime, cls, h->field[0], why, 0);
        remember(c, f, m, key);
        put(c, f->o.hypothesis, 7, RX_AIEN_HYP_TESTING);
        return 0;
    }
    return 0;
}

/* ---- setup ---- */

void rx_aien_default_config(RxAienConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->baseline_intervals = 4;
    cfg->tolerance_pct = 20;
    cfg->misses_to_fail = 2;
    cfg->confirm_intervals = 4;
    cfg->min_calls = 8;
}

int rx_aien_create_objects(RxAienFaculty *f, RxWorld *w, const RxAienConfig *cfg,
                           const RxAienInputs *in) {
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->cfg = *cfg;
    f->in = *in;
    if (cfg->baseline_intervals == 0 || cfg->misses_to_fail == 0 || cfg->tolerance_pct >= 100)
        return RX_ERR_ARG;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
#define MK(ref, type, res) do {                                                        \
        int rc_ = rx_world_create(w, (type), RX_PERSIST_RESIDENT,                       \
                                  RX_AIEN_RES_BASE + (res), z, &(ref));                 \
        if (rc_ != RX_OK) return rc_;                                                   \
    } while (0)
    MK(f->o.placement, RX_OT_PLACEMENT, RX_AIEN_RES_PLACEMENT);
    MK(f->o.goal, RX_OT_GOAL, RX_AIEN_RES_GOAL);
    MK(f->o.belief, RX_OT_BELIEF, RX_AIEN_RES_BELIEF);
    MK(f->o.prediction, RX_OT_PREDICTION, RX_AIEN_RES_PREDICTION);
    MK(f->o.hypothesis, RX_OT_HYPOTHESIS, RX_AIEN_RES_HYPOTHESIS);
    MK(f->o.plan, RX_OT_PLAN, RX_AIEN_RES_PLAN);
    MK(f->o.assessment, RX_OT_ASSESSMENT, RX_AIEN_RES_ASSESSMENT);
    MK(f->o.memory, RX_OT_MEMORY, RX_AIEN_RES_MEMORY);
    MK(f->o.experiment_belief, RX_OT_EXPERIMENT_BELIEF, RX_AIEN_RES_EXPERIMENT_BELIEF);
#undef MK
    return RX_OK;
}

typedef struct {
    RxReactionDesc d;
    const RxAienFaculty *f;
    const RxAienCaps *caps;
} Builder;

static RxCapRef cap_for(const Builder *b, RxObjRef o) {
    if (o.id == b->f->in.demand.id) return b->caps->demand;
    if (o.id == b->f->in.selection.id) return b->caps->selection;
    uint64_t res = b->f->w->objects[o.id].resource;
    return b->caps->own[res - RX_AIEN_RES_BASE];
}

/* This optional reaction is deliberately separate from the cost prediction:
 * a GB10 add witnesses an experiment but does not measure matvec cost. */
static int fn_experiment(RxCtx *c) {
    RxAienFaculty *f = c->user;
    const RxSnapshotDep *e = in_of(c, f->experiment_evidence);
    const RxSnapshotDep *b = in_of(c, f->o.experiment_belief);
    if (!e || !b) return -1;
    if (e->field[0] == 0 || e->field[0] <= b->field[0]) return 0;
    /* Supported only when every replicate matched; one mismatch refutes. */
    int held = e->field[3] == 1 && e->field[1] == e->field[2] && e->field[4] != 0;
    put(c, f->o.experiment_belief, 0, e->field[0]);
    put(c, f->o.experiment_belief, 1, e->field[2]);
    put(c, f->o.experiment_belief, 2, held ? RX_AIEN_EXP_SUPPORTED : RX_AIEN_EXP_REFUTED);
    put(c, f->o.experiment_belief, 3, e->field[4]);
    return 0;
}

int rx_aien_register_experiment(RxAienFaculty *f, RxObjRef evidence,
                                RxCapRef evidence_read, RxCapRef belief_write) {
    if (!f || !f->w || evidence.id >= RX_MAX_OBJECTS ||
        !f->w->objects[evidence.id].live ||
        f->w->objects[evidence.id].generation != evidence.generation) return RX_ERR_ARG;
    f->experiment_evidence = evidence;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "aien.experiment.observe";
    d.faculty = RX_FACULTY_AIEN;
    d.subject = RX_AIEN_SUBJ;
    d.priority = RX_PRIO_LEARNING;
    d.fn = fn_experiment;
    d.user = f;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ evidence, RX_FIELD(0) };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ f->o.experiment_belief, RX_ALL_FIELDS };
    d.n_writes = 1;
    d.writes[0] = (RxDep){ f->o.experiment_belief, RX_ALL_FIELDS };
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){ evidence_read, f->w->objects[evidence.id].resource, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ belief_write,
        RX_AIEN_RES_BASE + RX_AIEN_RES_EXPERIMENT_BELIEF, RX_RIGHT_READ | RX_RIGHT_WRITE };
    return rx_world_add_reaction_keyed(f->w, f->keys, &d, &f->r_experiment);
}

static void need(Builder *b, RxObjRef o, uint32_t rights) {
    uint64_t res = b->f->w->objects[o.id].resource;
    for (uint32_t i = 0; i < b->d.n_caps; i++)
        if (b->d.caps[i].resource == res) { b->d.caps[i].rights |= rights; return; }
    b->d.caps[b->d.n_caps++] = (RxCapNeed){ cap_for(b, o), res, rights };
}

static void begin(Builder *b, const char *name, uint32_t prio, RxFn fn, RxAienFaculty *f,
                  const RxAienCaps *caps) {
    memset(b, 0, sizeof(*b));
    b->d.name = name;
    b->d.faculty = RX_FACULTY_AIEN;
    b->d.subject = RX_AIEN_SUBJ;
    b->d.priority = prio;
    b->d.fn = fn;
    b->d.user = f;
    b->f = f;
    b->caps = caps;
}

static void trig(Builder *b, RxObjRef o, uint64_t mask) {
    b->d.triggers[b->d.n_triggers++] = (RxDep){ o, mask };
    need(b, o, RX_RIGHT_READ);
}

static void rd(Builder *b, RxObjRef o) {
    b->d.reads[b->d.n_reads++] = (RxDep){ o, RX_ALL_FIELDS };
    need(b, o, RX_RIGHT_READ);
}

static void wr(Builder *b, RxObjRef o) {
    b->d.writes[b->d.n_writes++] = (RxDep){ o, RX_ALL_FIELDS };
    need(b, o, RX_RIGHT_READ | RX_RIGHT_WRITE);
}

int rx_aien_register(RxAienFaculty *f, const RxAienCaps *caps) {
    RxWorld *w = f->w;
    Builder b;
    int rc;

    begin(&b, "aien.observe", RX_PRIO_LEARNING, fn_observe, f, caps);
    trig(&b, f->in.demand, RX_FIELD(2));
    trig(&b, f->in.selection, RX_FIELD(0));
    rd(&b, f->o.belief);
    rd(&b, f->o.prediction);
    rd(&b, f->o.hypothesis);
    wr(&b, f->o.belief);
    if ((rc = rx_world_add_reaction_keyed(w, f->keys, &b.d, &f->r_observe)) != RX_OK) return rc;

    begin(&b, "aien.predict", RX_PRIO_LEARNING, fn_predict, f, caps);
    trig(&b, f->o.belief, RX_FIELD(5));
    rd(&b, f->o.prediction);
    rd(&b, f->o.placement);
    rd(&b, f->in.selection);
    rd(&b, f->o.hypothesis);
    wr(&b, f->o.prediction);
    if ((rc = rx_world_add_reaction_keyed(w, f->keys, &b.d, &f->r_predict)) != RX_OK) return rc;

    begin(&b, "aien.explain", RX_PRIO_LEARNING, fn_explain, f, caps);
    trig(&b, f->o.prediction, RX_FIELD(0) | RX_FIELD(6));
    rd(&b, f->o.hypothesis);
    rd(&b, f->o.placement);
    wr(&b, f->o.hypothesis);
    if ((rc = rx_world_add_reaction_keyed(w, f->keys, &b.d, &f->r_explain)) != RX_OK) return rc;

    begin(&b, "aien.assess", RX_PRIO_LEARNING, fn_assess, f, caps);
    trig(&b, f->o.goal, RX_ALL_FIELDS);
    trig(&b, f->o.prediction, RX_FIELD(0) | RX_FIELD(6));
    rd(&b, f->o.memory);
    rd(&b, f->o.placement);
    rd(&b, f->o.assessment);
    wr(&b, f->o.assessment);
    if ((rc = rx_world_add_reaction_keyed(w, f->keys, &b.d, &f->r_assess)) != RX_OK) return rc;

    begin(&b, "aien.plan", RX_PRIO_LEARNING, fn_plan, f, caps);
    trig(&b, f->o.hypothesis, RX_FIELD(7));
    trig(&b, f->o.assessment, RX_FIELD(4) | RX_FIELD(0));
    rd(&b, f->o.memory);
    rd(&b, f->o.prediction);
    rd(&b, f->o.plan);
    wr(&b, f->o.plan);
    wr(&b, f->o.memory);
    wr(&b, f->o.hypothesis);
    if ((rc = rx_world_add_reaction_keyed(w, f->keys, &b.d, &f->r_plan)) != RX_OK) return rc;
    return RX_OK;
}
