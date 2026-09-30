/*
 * rx_living.c -- R13 reactions. See rx_living.h for the object map.
 *
 * Each fn_* is a reaction body: it reads its versioned snapshot and proposes
 * mutations. Two of them also make an effect outside the world, the R9
 * proposal and the R9 promotion; both are keyed so that a re-run after an
 * invalidation does not repeat the effect.
 */
#include "rx_living.h"
#include "aienos_cap.h"
#include "sha256.h"

#include <string.h>
#include <time.h>

static const RxSnapshotDep *input(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id) return &c->in[i];
    return NULL;
}

static void put(RxCtx *c, RxObjRef r, uint32_t field, uint64_t value) {
    c->out[c->n_out++] = (RxMutation){r, field, value};
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void rx_living_object_digest(uint32_t type, const uint64_t field[RX_MAX_FIELDS],
                             uint8_t out[32]) {
    sha256_ctx c;
    uint8_t b[8];
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIEN_RX_OBJECT_V1", 17);
    for (uint32_t i = 0; i < 4; i++) b[i] = (uint8_t)(type >> (8 * i));
    sha256_update(&c, b, 4);
    for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
        for (uint32_t i = 0; i < 8; i++) b[i] = (uint8_t)(field[f] >> (8 * i));
        sha256_update(&c, b, 8);
    }
    sha256_final(&c, out);
}

/* Operands of trial t. They wrap past 2^32, and consecutive sums differ, so
 * every trial changes the output the next evidence check wakes on. */
static uint64_t trial_a(uint64_t plan, uint64_t t) {
    return (uint32_t)(0xffffff00u + (uint32_t)plan * 16u + (uint32_t)t);
}
static uint64_t trial_b(uint64_t epoch, uint64_t t) {
    return (uint32_t)((uint32_t)epoch * 0x100u + (uint32_t)t);
}

/* ---- living.experiment.prepare ----
 * Omega took up an AIEN plan (search field 6 names it) and opened a new
 * search epoch. Trial 0 of the experiment is derived from both. Plan and
 * search are ordinary dependencies; nothing here calls Omega or AIEN. */
static int fn_prepare(RxCtx *c) {
    RxLiving *l = c->user;
    const RxSnapshotDep *s = input(c, l->omega->o.search);
    const RxSnapshotDep *p = input(c, l->aien->o.plan);
    const RxSnapshotDep *i = input(c, l->o.input);
    if (!s || !p || !i) return -1;
    if (s->field[0] < 2 || p->field[0] == 0 ||
        s->field[6] != p->field[0] || i->field[2] == s->field[0]) return 0;
    put(c, l->o.input, 0, trial_a(p->field[0], 0));
    put(c, l->o.input, 1, trial_b(s->field[0], 0));
    put(c, l->o.input, 2, s->field[0]);
    put(c, l->o.input, 3, 0);
    put(c, l->o.input, 4, p->field[0]);
    return 0;
}

/* The seat reaction is executed by the resident GB10 seat (or R12's
 * processor stand-in on hosts). The world never calls this body. */
static int fn_seat_is_not_cpu(RxCtx *c) { (void)c; return -1; }

/* ---- living.experiment.evidence ----
 * The seat published field 0 into the same canonical object. Each trial's
 * sum is checked against its operands; a match poses the next trial, the
 * last match or any mismatch ends the experiment with its evidence. */
static int fn_evidence(RxCtx *c) {
    RxLiving *l = c->user;
    const RxSnapshotDep *i = input(c, l->o.input);
    const RxSnapshotDep *o = input(c, l->o.output);
    const RxSnapshotDep *e = input(c, l->o.evidence);
    if (!i || !o || !e) return -1;
    uint64_t epoch = i->field[2], t = i->field[3];
    if (epoch < 2 || epoch <= e->field[0]) return 0;
    uint64_t expected = (uint32_t)((uint32_t)i->field[0] + (uint32_t)i->field[1]);
    uint64_t matched = t;               /* trials before this one all matched */
    int ok = o->field[0] == expected;
    if (ok) matched++;
    put(c, l->o.evidence, 1, expected);
    put(c, l->o.evidence, 2, o->field[0]);
    put(c, l->o.evidence, 4, matched);
    if (ok && matched < RX_LIVING_TRIALS) {
        put(c, l->o.input, 0, trial_a(i->field[4], matched));
        put(c, l->o.input, 1, trial_b(epoch, matched));
        put(c, l->o.input, 3, matched);
        return 0;
    }
    put(c, l->o.evidence, 0, epoch);
    put(c, l->o.evidence, 3, ok ? 1 : 0);
    return 0;
}

/* The crumb that published this field, if `reaction` published it. */
static const RxCrumb *authored(RxWorld *w, uint64_t crumb, uint32_t reaction) {
    const RxCrumb *k = rx_world_crumb(w, crumb);
    if (!k || k->reaction != reaction) return NULL;
    if (k->kind != RX_CRUMB_COMMIT && k->kind != RX_CRUMB_EXTERNAL) return NULL;
    return k;
}

/* Latest parent of `k` published by `reaction`. Parents are sorted. */
static const RxCrumb *parent_by(RxWorld *w, const RxCrumb *k, uint32_t reaction) {
    for (uint32_t i = k->n_parents; i-- > 0;) {
        const RxCrumb *p = authored(w, k->parents[i], reaction);
        if (p) return p;
    }
    return NULL;
}

/* Walk the trials back from the last seat publication: seat(t) <- the
 * evidence commit that posed trial t <- seat(t-1) ... <- the prepare commit
 * that posed trial 0. */
static const RxCrumb *first_trial(RxWorld *w, const RxLiving *l, const RxCrumb *seat) {
    for (uint32_t t = 0; seat && t <= RX_LIVING_TRIALS; t++) {
        const RxCrumb *p = parent_by(w, seat, l->r_prepare);
        if (p) return p;
        const RxCrumb *e = parent_by(w, seat, l->r_evidence);
        seat = e ? parent_by(w, e, l->r_seat) : NULL;
    }
    return NULL;
}

static void link(RxLivingProvenance *pv, uint32_t which, const RxCrumb *k, RxObjRef obj) {
    pv->link[which].obj = obj;
    pv->link[which].reaction = k->reaction;
    pv->link[which].crumb = k->id;
    memcpy(pv->link[which].digest, k->digest, 32);
}

static void bind_snapshot(RxGenObject *o, const RxSnapshotDep *d, uint32_t type) {
    o->id = d->obj.id;
    o->generation = d->obj.generation;
    rx_living_object_digest(type, d->field, o->digest);
}

/* An object outside the snapshot is bound by the crumb that published it. */
static void bind_crumb(RxGenObject *o, const RxLivingLink *k) {
    o->id = k->obj.id;
    o->generation = k->obj.generation;
    memcpy(o->digest, k->digest, 32);
}

static int decline(RxLiving *l, int why) {
    l->refusals++;
    l->last_refusal = why;
    return 0;
}

/* ---- generation.prepare ----
 * A new Omega selection for an epoch AIEN believes the physical experiment
 * supported. Checks who wrote every input it relies on, walks the causal
 * crumbs back to the measurement, verdict and synthesis of the selected
 * realization, and proposes an R9 draft. It proposes; it cannot promote. */
/* Durable executor completions: run the deciding activation again. */
static void resume_candidate(void *ctx) {
    RxLiving *l = ctx;
    rx_world_resume(l->world, l->r_candidate);
}

static void resume_promoter(void *ctx) {
    RxLivingPromoter *p = ctx;
    rx_world_resume(p->world, p->reaction);
}

static int fn_candidate(RxCtx *c) {
    RxLiving *l = c->user;
    RxWorld *w = l->world;
    const RxSnapshotDep *s = input(c, l->omega->o.selection);
    const RxSnapshotDep *b = input(c, l->aien->o.experiment_belief);
    const RxSnapshotDep *e = input(c, l->o.evidence);
    const RxSnapshotDep *cr = input(c, l->o.candidate);
    const RxSnapshotDep *g = input(c, l->aien->o.goal);
    const RxSnapshotDep *p = input(c, l->aien->o.plan);
    const RxSnapshotDep *sr = input(c, l->omega->o.search);
    const RxSnapshotDep *out = input(c, l->o.output);
    if (!s || !b || !e || !cr || !g || !p || !sr || !out) return -1;
    uint64_t epoch = s->field[0];
    if (epoch < 2 || cr->field[1] == epoch || s->field[1] == 0 ||
        b->field[0] != epoch || b->field[2] != RX_AIEN_EXP_SUPPORTED ||
        e->field[0] != epoch || e->field[3] != 1 || e->field[4] != RX_LIVING_TRIALS ||
        sr->field[0] != epoch)
        return 0;

    /* Authorship of every record the proposal stands on. */
    RxLivingProvenance pv;
    memset(&pv, 0, sizeof pv);
    memcpy(pv.magic, "R13PROV1", 8);
    const RxCrumb *k_sel = authored(w, s->field_writer[0], l->omega->r_select);
    const RxCrumb *k_bel = authored(w, b->field_writer[0], l->aien->r_experiment);
    const RxCrumb *k_evi = authored(w, e->field_writer[0], l->r_evidence);
    const RxCrumb *k_out = authored(w, out->field_writer[0], l->r_seat);
    const RxCrumb *k_srch = authored(w, sr->field_writer[0], l->omega->r_reconsider);
    const RxCrumb *k_plan = authored(w, p->field_writer[0], l->aien->r_plan);
    const RxCrumb *k_goal = authored(w, g->field_writer[0], UINT32_MAX);
    if (!k_sel || !k_bel || !k_evi || !k_out || !k_srch || !k_plan || !k_goal ||
        s->field_writer[1] != k_sel->id || sr->field[6] != p->field[0])
        return decline(l, RX_LIVING_WHY_AUTHOR);

    /* The realization Omega's store verified for exactly these bytes. */
    RxOmegaRealization real;
    if (rx_omega_store_find(l->omega, s->field[1], &real) != 0 || !real.verified ||
        real.code_len == 0 || real.code_len > sizeof(real.code) ||
        real.kind >= l->omega->cfg.n_slots)
        return decline(l, RX_LIVING_WHY_STORE);
    for (uint32_t k = 0; k < 4; k++) {
        uint64_t word = 0;
        for (uint32_t j = 0; j < 8; j++) word |= (uint64_t)real.id.bytes[k*8+j] << (8*j);
        if (word != s->field[1+k]) return decline(l, RX_LIVING_WHY_STORE);
    }
    SemanticId again;
    if (rx_omega_identity_of(l->omega, real.code, real.code_len, &again) != 0 ||
        memcmp(again.bytes, real.id.bytes, 32) != 0)
        return decline(l, RX_LIVING_WHY_STORE);

    /* Selection <- measure <- verdict <- synthesis, for the selected slot,
     * and GPU output <- GPU input (the seat's claim) <- prepare. */
    uint32_t k = real.kind;
    const RxCrumb *k_meas = parent_by(w, k_sel, l->omega->r_measure[k]);
    const RxCrumb *k_verd = k_meas ? parent_by(w, k_meas, l->omega->r_verify[k]) : NULL;
    const RxCrumb *k_syn = k_verd ? parent_by(w, k_verd, l->omega->r_synth[k]) : NULL;
    const RxCrumb *k_in = first_trial(w, l, k_out);
    if (!k_meas || !k_verd || !k_syn || !k_in ||
        !parent_by(w, k_bel, l->r_evidence) || !parent_by(w, k_evi, l->r_seat))
        return decline(l, RX_LIVING_WHY_CHAIN);

    pv.epoch = epoch;
    pv.goal_seq = g->field[0];
    pv.plan_seq = p->field[0];
    link(&pv, RX_LINK_GOAL, k_goal, l->aien->o.goal);
    link(&pv, RX_LINK_PLAN, k_plan, l->aien->o.plan);
    link(&pv, RX_LINK_SEARCH, k_srch, l->omega->o.search);
    link(&pv, RX_LINK_GPU_INPUT, k_in, l->o.input);
    link(&pv, RX_LINK_GPU_OUTPUT, k_out, l->o.output);
    link(&pv, RX_LINK_EVIDENCE, k_evi, l->o.evidence);
    link(&pv, RX_LINK_BELIEF, k_bel, l->aien->o.experiment_belief);
    link(&pv, RX_LINK_SYNTH, k_syn, l->omega->o.candidate[k]);
    link(&pv, RX_LINK_VERDICT, k_verd, l->omega->o.verdict[k]);
    link(&pv, RX_LINK_MEASURE, k_meas, l->omega->o.measure[k]);
    link(&pv, RX_LINK_SELECTION, k_sel, l->omega->o.selection);

    RxGenObject objs[RX_LINK_COUNT];
    bind_snapshot(&objs[RX_LINK_GOAL], g, RX_OT_GOAL);
    bind_snapshot(&objs[RX_LINK_PLAN], p, RX_OT_PLAN);
    bind_snapshot(&objs[RX_LINK_SEARCH], sr, RX_OT_SEARCH);
    bind_crumb(&objs[RX_LINK_GPU_INPUT], &pv.link[RX_LINK_GPU_INPUT]);
    bind_snapshot(&objs[RX_LINK_GPU_OUTPUT], out, RX_OT_LIVING_OUTPUT);
    bind_snapshot(&objs[RX_LINK_EVIDENCE], e, RX_OT_LIVING_EVIDENCE);
    bind_snapshot(&objs[RX_LINK_BELIEF], b, RX_OT_EXPERIMENT_BELIEF);
    bind_crumb(&objs[RX_LINK_SYNTH], &pv.link[RX_LINK_SYNTH]);
    bind_crumb(&objs[RX_LINK_VERDICT], &pv.link[RX_LINK_VERDICT]);
    bind_crumb(&objs[RX_LINK_MEASURE], &pv.link[RX_LINK_MEASURE]);
    bind_snapshot(&objs[RX_LINK_SELECTION], s, RX_OT_SELECTION);

    RxLivingEvidence ev;
    memset(&ev, 0, sizeof ev);
    memcpy(ev.magic, "R13EVID1", 8);
    ev.epoch = epoch;
    ev.expected = e->field[1];
    ev.observed = e->field[2];
    ev.selected_ps = s->field[5];
    ev.reference_ps = s->field[7];
    ev.trials = e->field[4];
    RxLivingConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    memcpy(cfg.magic, "R13CONF1", 8);
    cfg.regime = s->field[6];
    cfg.margin_pct = l->omega->cfg.margin_pct;
    cfg.kind = real.kind;
    cfg.code_len = real.code_len;
    memcpy(cfg.identity, real.id.bytes, 32);

    /* The authority epoch and generation of the experiment grant the seat
     * held: the grant the GPU work ran under. */
    RxObject slot;
    AienosCapEntry grant;
    if (rx_world_read(w, l->caps.output_slot, &slot) != RX_OK ||
        aienos_cap_inspect(l->authority,
            (AienosCapRef){(uint32_t)slot.field[0], slot.field[1]}, &grant) != 0)
        return decline(l, RX_LIVING_WHY_GRANT);

    uint64_t id = 0;
    const int durable = rx_gen_exec_running(l->store);
    if (durable && l->proposed_epoch != epoch) {
        /* A proposal the durable executor finished since the last run. */
        RxGenJobResult jr;
        if (rx_gen_job_state(l->store, RX_GEN_JOB_PROPOSE, &jr) == RX_GEN_JOB_DONE) {
            rx_gen_job_take(l->store, RX_GEN_JOB_PROPOSE);
            if (jr.rc == RX_GEN_OK) {
                l->proposed_epoch = jr.key;
                l->candidate_id = jr.id;
            } else if (jr.key == epoch) {
                decline(l, RX_LIVING_WHY_PROPOSE);
                return -1;
            }
        }
    }
    if (l->proposed_epoch == epoch) {
        id = l->candidate_id;
    } else {
        RxGenDraft draft;
        memset(&draft, 0, sizeof draft);
        draft.authority_epoch = grant.epoch;
        draft.authority_generation = grant.generation;
        draft.proofs_ok = real.verified && e->field[3] == 1 && b->field[2] == 1;
        draft.objects = objs;
        draft.n_objects = RX_LINK_COUNT;
        draft.evidence = (const uint8_t *)&ev;
        draft.evidence_len = sizeof ev;
        draft.model = (const uint8_t *)b->field;
        draft.model_len = sizeof b->field;
        draft.realization = real.code;
        draft.realization_len = real.code_len;
        draft.config = (const uint8_t *)&cfg;
        draft.config_len = sizeof cfg;
        draft.provenance = (const uint8_t *)&pv;
        draft.provenance_len = sizeof pv;
        if (durable) {
            /* The store's fsyncs run on the durable executor, not on this
             * worker. A proposal still in flight (this epoch's or an older
             * one's) resumes this activation when it lands. */
            if (rx_gen_job_state(l->store, RX_GEN_JOB_PROPOSE, NULL) == RX_GEN_JOB_IDLE &&
                rx_gen_post_propose_as(l->store, epoch, RX_LIVING_PREPARE_SUBJ,
                                       rx_caller_find(l->keys, RX_LIVING_PREPARE_SUBJ), &draft,
                                       resume_candidate, l) != RX_GEN_OK) {
                decline(l, RX_LIVING_WHY_PROPOSE);
                return -1;
            }
            return RX_FN_DEFER;
        }
        if (rx_gen_propose_as(l->store, RX_LIVING_PREPARE_SUBJ,
                              rx_caller_find(l->keys, RX_LIVING_PREPARE_SUBJ), &draft, &id) != RX_GEN_OK) {
            decline(l, RX_LIVING_WHY_PROPOSE);
            return -1;
        }
        l->proposed_epoch = epoch;
        l->candidate_id = id;
    }
    put(c, l->o.candidate, 0, id);
    put(c, l->o.candidate, 1, epoch);
    for (uint32_t j = 0; j < 4; j++) put(c, l->o.candidate, 2 + j, s->field[1 + j]);
    put(c, l->o.candidate, 6, s->field[6]);
    put(c, l->o.candidate, 7, s->field[5]);
    return 0;
}

/* R16 C5: the promotion subject presents its own credential. */
static void promotion_caller(const RxLivingPromoter *p, RxPromotionRequest *req) {
    const RxCallerCred *c = rx_caller_find(p->keys, RX_LIVING_PROMOTE_SUBJ);
    if (c) req->caller = *c;
}

int rx_living_native_authority(void *ctx, uint32_t cap_id, uint64_t generation,
                                uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapEntry entry;
    return aienos_cap_validate(ctx, (AienosCapRef){cap_id, generation},
                               subject, resource, rights, &entry);
}

static int native_promotion(void *ctx, uint32_t cap_id, uint64_t generation,
                            uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapEntry entry;
    return aienos_cap_validate(ctx, (AienosCapRef){cap_id, generation},
                               subject, resource, rights, &entry);
}

/* ---- generation.promote ----
 * The separate promotion authority. It holds RX_GEN_RIGHT_PROMOTE on
 * RX_GEN_RES_PROMOTION; R9 validates that against the native authority and
 * refuses a promoter that is also the proposer. Only after R9 commits does
 * the in-force record, which production reads, name the new realization. */
/* The promotion's durable work (blobs, root, journal, flip, receipt, event,
 * all fsynced) runs on the store's executor; this worker only decides it and,
 * once it is done, publishes it, so production keeps the worker meanwhile.
 * R9 still validates the promotion right and refuses a self-promotion inside
 * rx_gen_promote. Returns 1 while the promotion is in flight. */
static int promote_durable(RxLivingPromoter *p, uint64_t id, int *rc, uint64_t *active,
                           uint64_t *ns) {
    RxGenJobResult jr;
    uint64_t lineage = 0;
    int st = rx_gen_job_state(p->store, RX_GEN_JOB_PROMOTE, &jr);
    if (st == RX_GEN_JOB_DONE && jr.key != id) {
        /* An earlier candidate's outcome: it is on disk; the record now
         * describes a newer candidate. */
        rx_gen_job_take(p->store, RX_GEN_JOB_PROMOTE);
        st = RX_GEN_JOB_IDLE;
    }
    if (st == RX_GEN_JOB_DONE) {
        rx_gen_job_take(p->store, RX_GEN_JOB_PROMOTE);
        *rc = jr.rc;
        *ns = jr.ns;
        *active = jr.active;
        return 0;
    }
    rx_gen_active(p->store, active, &lineage);
    if (*active == id) {
        *rc = RX_GEN_OK;             /* a re-run after an invalidated publication */
        return 0;
    }
    if (st == RX_GEN_JOB_PENDING) return 1;
    RxPromotionRequest req = {id, RX_LIVING_PROMOTE_SUBJ,
        p->promotion_authority.cap_id, p->promotion_authority.generation,
        RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, {0, {0}}};
    promotion_caller(p, &req);
    *rc = rx_gen_post_promote(p->store, id, &req, native_promotion, (void *)p->authority,
                              resume_promoter, p);
    rx_caller_wipe(&req.caller);
    return *rc == RX_GEN_OK;
}

/* Without an executor (the sequential reference) the stage runs to completion. */
static void promote_inline(RxLivingPromoter *p, uint64_t id, int *rc, uint64_t *active,
                           uint64_t *ns) {
    uint64_t lineage = 0, t0 = now_ns();
    rx_gen_active(p->store, active, &lineage);
    if (*active == id) {
        *rc = RX_GEN_OK;             /* a re-run after an invalidated publication */
        return;
    }
    RxPromotionRequest req = {id, RX_LIVING_PROMOTE_SUBJ,
        p->promotion_authority.cap_id, p->promotion_authority.generation,
        RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, {0, {0}}};
    promotion_caller(p, &req);
    *rc = rx_gen_promote(p->store, &req, native_promotion, (void *)p->authority,
                         NULL, NULL, NULL, NULL);
    rx_caller_wipe(&req.caller);
    *ns = now_ns() - t0;
    rx_gen_active(p->store, active, &lineage);
}

static int fn_promote(RxCtx *c) {
    RxLivingPromoter *p = c->user;
    const RxSnapshotDep *cand = input(c, p->candidate);
    const RxSnapshotDep *done = input(c, p->promotion);
    if (!cand || !done) return -1;
    uint64_t id = cand->field[0];
    if (!id || done->field[0] == id) return 0;

    uint64_t active = 0, ns = 0;
    int rc = RX_GEN_OK;
    if (rx_gen_exec_running(p->store)) {
        if (promote_durable(p, id, &rc, &active, &ns)) return RX_FN_DEFER;
    } else {
        promote_inline(p, id, &rc, &active, &ns);
    }
    p->result = rc;
    put(c, p->promotion, 0, id);
    put(c, p->promotion, 1, (uint64_t)(int64_t)rc);
    put(c, p->promotion, 2, ns);
    put(c, p->promotion, 3, active);
    if (rc != RX_GEN_OK) return 0;
    put(c, p->inforce, 0, cand->field[1]);
    for (uint32_t j = 0; j < 4; j++) put(c, p->inforce, 1 + j, cand->field[2 + j]);
    put(c, p->inforce, 5, cand->field[7]);
    put(c, p->inforce, 6, cand->field[6]);
    put(c, p->inforce, 7, active);
    return 0;
}

int rx_living_create(RxLiving *l, RxWorld *w, RxAienFaculty *aien,
                     RxOmegaFaculty *omega, RxGenStore *store,
                     const struct AienosCapView *authority) {
    if (!l || !w || !aien || !omega || !store || !authority) return RX_ERR_ARG;
    memset(l, 0, sizeof *l);
    l->world = w; l->aien = aien; l->omega = omega;
    l->store = store; l->authority = authority;
    uint64_t z[RX_MAX_FIELDS] = {0};
    RxObjRef *r[6] = {&l->o.input, &l->o.output, &l->o.evidence,
                      &l->o.candidate, &l->o.promotion, &l->o.inforce};
    for (uint32_t k = 0; k < 6; k++) {
        int rc = rx_world_create(w, RX_OT_LIVING_INPUT + k, RX_PERSIST_RESIDENT,
                                 RX_LIVING_RES_BASE + k, z, r[k]);
        if (rc != RX_OK) return rc;
    }
    int rc = rx_world_attach_physical(w, l->o.input);
    if (rc != RX_OK) return rc;
    return rx_world_attach_physical(w, l->o.output);
}

static void base(RxReactionDesc *d, const char *name, uint32_t faculty,
                 uint32_t subject, RxFn fn, void *user) {
    memset(d, 0, sizeof *d);
    d->name = name; d->faculty = faculty; d->subject = subject;
    d->priority = RX_PRIO_LEARNING; d->fn = fn; d->user = user;
}

static RxCapNeed cap(RxCapRef ref, uint32_t res, uint32_t rights) {
    return (RxCapNeed){ref, RX_LIVING_RES_BASE + res, rights};
}

int rx_living_register(RxLiving *l, const RxLivingCaps *caps,
                       RxLivingPromoter *promoter) {
    RxWorld *w = l->world;
    if (!promoter || promoter->world != w || promoter->store != l->store ||
        promoter->candidate.id != l->o.candidate.id ||
        promoter->promotion.id != l->o.promotion.id ||
        promoter->inforce.id != l->o.inforce.id) return RX_ERR_ARG;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    l->caps = *caps;
    int rc;
    RxReactionDesc d;
    /* A resident world puts the store's physical work on its own executor so
     * a promotion never occupies a semantic worker. The sequential reference
     * runs every stage to completion on its one thread, as designed. */
    if (!w->sequential && rx_gen_exec_start(l->store) != RX_GEN_OK) return RX_ERR_FULL;

    base(&d, "living.experiment.prepare", RX_FACULTY_OMEGA, RX_LIVING_SUBJ, fn_prepare, l);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){l->omega->o.search, RX_FIELD(0) | RX_FIELD(6)};
    d.n_reads = 2;
    d.reads[0] = (RxDep){l->aien->o.plan, RX_ALL_FIELDS};
    d.reads[1] = (RxDep){l->o.input, RX_ALL_FIELDS};
    d.n_writes = 1;
    d.writes[0] = (RxDep){l->o.input, RX_ALL_FIELDS};
    d.n_caps = 3;
    d.caps[0] = (RxCapNeed){caps->search_read, RX_OMEGA_RES_BASE + RX_OMEGA_RES_SEARCH, R};
    d.caps[1] = (RxCapNeed){caps->plan_read, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R};
    d.caps[2] = cap(caps->input_write, RX_LIVING_RES_INPUT, RW);
    if ((rc = rx_world_add_reaction_keyed(w, l->keys, &d, &l->r_prepare)) != RX_OK) return rc;

    /* The seat: one data trigger, one write, R5 asks for the Blackwell
     * feature, and its output WRITE reference is read from the R8 slot at
     * every check. */
    base(&d, "living.blackwell.add", RX_FACULTY_OMEGA, RX_LIVING_SEAT_SUBJ,
         fn_seat_is_not_cpu, l);
    d.priority = RX_PRIO_FOREGROUND;
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){l->o.input, RX_FIELD(0) | RX_FIELD(1)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){l->o.output, RX_FIELD(0)};
    d.n_caps = 3;
    d.caps[0] = cap(caps->input_seat_read, RX_LIVING_RES_INPUT, R);
    d.caps[1] = cap((RxCapRef){UINT32_MAX, 0}, RX_LIVING_RES_OUTPUT, RX_RIGHT_WRITE);
    d.caps[2] = (RxCapNeed){caps->output_slot_read,
                            w->objects[caps->output_slot.id].resource, R};
    rx_aegis_use_slot(&d, 1, caps->output_slot);
    if ((rc = rx_world_add_reaction_keyed(w, l->keys, &d, &l->r_seat)) != RX_OK) return rc;

    base(&d, "living.experiment.evidence", RX_FACULTY_OMEGA, RX_LIVING_SUBJ, fn_evidence, l);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){l->o.output, RX_FIELD(0)};
    d.n_reads = 2;
    d.reads[0] = (RxDep){l->o.input, RX_ALL_FIELDS};
    d.reads[1] = (RxDep){l->o.evidence, RX_ALL_FIELDS};
    d.n_writes = 2;
    d.writes[0] = (RxDep){l->o.evidence, RX_ALL_FIELDS};
    d.writes[1] = (RxDep){l->o.input, RX_ALL_FIELDS};
    d.n_caps = 3;
    d.caps[0] = cap(caps->output_read, RX_LIVING_RES_OUTPUT, R);
    d.caps[1] = cap(caps->input_write, RX_LIVING_RES_INPUT, RW);
    d.caps[2] = cap(caps->evidence_write, RX_LIVING_RES_EVIDENCE, RW);
    if ((rc = rx_world_add_reaction_keyed(w, l->keys, &d, &l->r_evidence)) != RX_OK) return rc;

    base(&d, "generation.prepare", RX_FACULTY_OMEGA, RX_LIVING_PREPARE_SUBJ, fn_candidate, l);
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){l->omega->o.selection, RX_ALL_FIELDS};
    d.n_reads = 7;
    d.reads[0] = (RxDep){l->aien->o.experiment_belief, RX_ALL_FIELDS};
    d.reads[1] = (RxDep){l->o.evidence, RX_ALL_FIELDS};
    d.reads[2] = (RxDep){l->o.candidate, RX_ALL_FIELDS};
    d.reads[3] = (RxDep){l->aien->o.goal, RX_ALL_FIELDS};
    d.reads[4] = (RxDep){l->aien->o.plan, RX_ALL_FIELDS};
    d.reads[5] = (RxDep){l->omega->o.search, RX_ALL_FIELDS};
    d.reads[6] = (RxDep){l->o.output, RX_ALL_FIELDS};
    d.n_writes = 1;
    d.writes[0] = (RxDep){l->o.candidate, RX_ALL_FIELDS};
    d.n_caps = 8;
    d.caps[0] = (RxCapNeed){caps->selection_read, RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R};
    d.caps[1] = (RxCapNeed){caps->belief_read,
                            RX_AIEN_RES_BASE + RX_AIEN_RES_EXPERIMENT_BELIEF, R};
    d.caps[2] = cap(caps->evidence_prepare_read, RX_LIVING_RES_EVIDENCE, R);
    d.caps[3] = cap(caps->candidate_write, RX_LIVING_RES_CANDIDATE, RW);
    d.caps[4] = (RxCapNeed){caps->goal_prepare_read, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, R};
    d.caps[5] = (RxCapNeed){caps->plan_prepare_read, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R};
    d.caps[6] = (RxCapNeed){caps->search_prepare_read,
                            RX_OMEGA_RES_BASE + RX_OMEGA_RES_SEARCH, R};
    d.caps[7] = cap(caps->output_prepare_read, RX_LIVING_RES_OUTPUT, R);
    if ((rc = rx_world_add_reaction_keyed(w, l->keys, &d, &l->r_candidate)) != RX_OK) return rc;

    base(&d, "generation.promote", RX_FACULTY_ROOT, RX_LIVING_PROMOTE_SUBJ, fn_promote,
         promoter);
    d.priority = RX_PRIO_MAINTENANCE;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){l->o.candidate, RX_ALL_FIELDS};
    d.n_reads = 1;
    d.reads[0] = (RxDep){l->o.promotion, RX_ALL_FIELDS};
    d.n_writes = 2;
    d.writes[0] = (RxDep){l->o.promotion, RX_ALL_FIELDS};
    d.writes[1] = (RxDep){l->o.inforce, RX_ALL_FIELDS};
    d.n_caps = 3;
    d.caps[0] = cap(promoter->candidate_read, RX_LIVING_RES_CANDIDATE, R);
    d.caps[1] = cap(promoter->promotion_write, RX_LIVING_RES_PROMOTION, RW);
    d.caps[2] = cap(promoter->inforce_write, RX_LIVING_RES_INFORCE, RW);
    return rx_world_add_reaction_keyed(w, promoter->keys, &d, &promoter->reaction);
}

/* ---- generation.restore (R14) ----
 * The body started. The in-force record is world state, so a restarted
 * process has none; the durable generation R9 recovered says what was in
 * force. The promotion subject reads it back through R9, Omega verifies the
 * bytes again with its own verifier, and only then is the record written.
 * It never promotes and never touches R9's pointer. */
static int restore_refuse(RxCtx *c, RxLiving *l, uint64_t why) {
    put(c, l->o.restore, 2, RX_LIVING_RESTORE_REFUSED);
    put(c, l->o.restore, 5, why);
    return 0;
}

static int fn_restore(RxCtx *c) {
    RxLiving *l = c->user;
    const RxSnapshotDep *rs = input(c, l->o.restore);
    if (!rs) return -1;
    uint64_t boot = rs->field[0];
    if (!boot || rs->field[1] == boot) return 0;
    uint64_t active = 0, lineage = 0;
    rx_gen_active(l->store, &active, &lineage);
    put(c, l->o.restore, 1, boot);
    put(c, l->o.restore, 3, active);
    put(c, l->o.restore, 6, lineage);
    put(c, l->o.restore, 4, 0);
    put(c, l->o.restore, 5, 0);

    uint8_t code[AARCH64_MAX_CODE_BYTES];
    RxLivingConfig cfg;
    RxLivingEvidence ev;
    size_t n = 0, cn = 0, en = 0;
    int rc = rx_gen_read_blob(l->store, active, "realization", code, sizeof code, &n);
    if (rc != RX_GEN_OK) return restore_refuse(c, l, (uint64_t)(int64_t)rc);
    if (n == 0) {                       /* genesis: nothing was ever put in force */
        put(c, l->o.restore, 2, RX_LIVING_RESTORE_REFERENCE);
        return 0;
    }
    rc = rx_gen_read_blob(l->store, active, "config", (uint8_t *)&cfg, sizeof cfg, &cn);
    if (rc == RX_GEN_OK)
        rc = rx_gen_read_blob(l->store, active, "evidence", (uint8_t *)&ev, sizeof ev, &en);
    if (rc != RX_GEN_OK) return restore_refuse(c, l, (uint64_t)(int64_t)rc);
    if (cn != sizeof cfg || en != sizeof ev || memcmp(cfg.magic, "R13CONF1", 8) != 0 ||
        memcmp(ev.magic, "R13EVID1", 8) != 0 || cfg.code_len != n)
        return restore_refuse(c, l, RX_LIVING_RESTORE_WHY_CONFIG);

    SemanticId id;
    uint32_t why = 0;
    int ok = rx_omega_readmit(l->omega, code, n, (uint32_t)cfg.kind, cfg.regime, &id, &why);
    if (ok < 0) return -1;
    if (ok != 0) return restore_refuse(c, l, why);
    if (memcmp(id.bytes, cfg.identity, 32) != 0)
        return restore_refuse(c, l, RX_LIVING_RESTORE_WHY_IDENTITY);

    uint64_t word[4] = {0, 0, 0, 0};
    for (uint32_t k = 0; k < 4; k++)
        for (uint32_t j = 0; j < 8; j++) word[k] |= (uint64_t)id.bytes[k*8+j] << (8*j);
    put(c, l->o.inforce, 0, ev.epoch);
    for (uint32_t k = 0; k < 4; k++) put(c, l->o.inforce, 1 + k, word[k]);
    put(c, l->o.inforce, 5, ev.selected_ps);
    put(c, l->o.inforce, 6, cfg.regime);
    put(c, l->o.inforce, 7, active);
    put(c, l->o.restore, 2, RX_LIVING_RESTORE_RESTORED);
    put(c, l->o.restore, 4, word[0]);
    return 0;
}

int rx_living_register_restore(RxLiving *l, RxLivingPromoter *promoter, RxCapRef restore_write) {
    RxWorld *w = l->world;
    if (!promoter || promoter->world != w || promoter->store != l->store ||
        promoter->inforce.id != l->o.inforce.id) return RX_ERR_ARG;
    uint64_t z[RX_MAX_FIELDS] = {0};
    int rc = rx_world_create(w, RX_OT_LIVING_RESTORE, RX_PERSIST_RESIDENT,
                             RX_LIVING_RES_BASE + RX_LIVING_RES_RESTORE, z, &l->o.restore);
    if (rc != RX_OK) return rc;
    const uint32_t RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    RxReactionDesc d;
    base(&d, "generation.restore", RX_FACULTY_ROOT, RX_LIVING_PROMOTE_SUBJ, fn_restore, l);
    d.priority = RX_PRIO_MAINTENANCE;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){l->o.restore, RX_FIELD(0)};
    d.n_reads = 1;
    d.reads[0] = (RxDep){l->o.restore, RX_ALL_FIELDS};
    d.n_writes = 2;
    d.writes[0] = (RxDep){l->o.restore, RX_ALL_FIELDS & ~RX_FIELD(0)};
    d.writes[1] = (RxDep){l->o.inforce, RX_ALL_FIELDS};
    d.n_caps = 2;
    d.caps[0] = cap(restore_write, RX_LIVING_RES_RESTORE, RW);
    d.caps[1] = cap(promoter->inforce_write, RX_LIVING_RES_INFORCE, RW);
    return rx_world_add_reaction_keyed(w, promoter->keys, &d, &l->r_restore);
}

/* ---- R16 C6: production caller enrollment ---- */

static int enroll_one(RxWorld *w, RxCallerKeyring *k, uint32_t subject) {
    if (k->n >= RX_CALLER_KEYRING_MAX) return RX_ERR_FULL;
    if (rx_world_enroll_caller(w, subject, &k->cred[k->n]) != RX_CALLER_OK)
        return RX_ERR_IDENTITY;   /* closed, already enrolled, full or no entropy */
    k->subject[k->n++] = subject;
    return RX_OK;
}

int rx_living_enroll_callers(RxWorld *w, RxLivingKeyrings *k) {
    if (!w || !k) return RX_ERR_ARG;
    memset(k, 0, sizeof *k);
    int rc;
    if ((rc = enroll_one(w, &k->omega, RX_OMEGA_SUBJ_SERVE)) != RX_OK ||
        (rc = enroll_one(w, &k->omega, RX_OMEGA_SUBJ_OMEGA)) != RX_OK ||
        (rc = enroll_one(w, &k->aien, RX_AIEN_SUBJ)) != RX_OK ||
        (rc = enroll_one(w, &k->aegis, RX_AEGIS_SUBJ)) != RX_OK ||
        (rc = enroll_one(w, &k->aegis, RX_AEGIS_ROOT_SUBJ)) != RX_OK ||
        (rc = enroll_one(w, &k->living, RX_LIVING_SUBJ)) != RX_OK ||
        (rc = enroll_one(w, &k->living, RX_LIVING_SEAT_SUBJ)) != RX_OK ||
        (rc = enroll_one(w, &k->living, RX_LIVING_PREPARE_SUBJ)) != RX_OK ||
        (rc = enroll_one(w, &k->promoter, RX_LIVING_PROMOTE_SUBJ)) != RX_OK) {
        for (unsigned i = 0; i < sizeof *k; i++) ((volatile uint8_t *)k)[i] = 0;
        return rc;
    }
    return RX_OK;
}
