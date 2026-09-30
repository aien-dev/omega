/*
 * rx_aegis.c -- AEGIS resident authority (ADR 0016 §41, R8).
 *
 * See rx_aegis.h. aegis.decide.k is a reaction body with no side effect
 * outside the world. root.install.k mints and revokes through the AIENOS
 * root; it keeps a per-slot record of what it minted for which decision, so
 * a run that is invalidated and repeated never mints twice.
 */
#include "rx_aegis.h"
#include "rx_argus.h"

#include <string.h>

/* ARGUS tick source: the native view when the world is bound to it. */
static inline const AienosCapView *aegis_view(const RxAegisFaculty *f) {
    return f->w && f->w->auth_validate ? (const AienosCapView *)f->w->auth_ctx : NULL;
}

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef r) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == r.id) return &c->in[i];
    return NULL;
}

static void put(RxCtx *c, RxObjRef o, uint32_t field, uint64_t v) {
    c->out[c->n_out++] = (RxMutation){ o, field, v };
}

/* ---- policy ---- */

uint32_t rx_aegis_evaluate(const RxAegisPolicy *p, uint32_t subject, uint64_t resource,
                           uint32_t rights, uint64_t lease, uint64_t *out_lease,
                           uint32_t *rule_or_why, uint32_t *needs_approval) {
    *out_lease = 0;
    *needs_approval = 0;
    if (rights == 0 || (rights & ~RX_RIGHT_KNOWN)) {
        *rule_or_why = RX_AEGIS_WHY_BAD_REQUEST;
        return RX_AEGIS_DENY;
    }
    if (rights & RX_RIGHT_PRIVILEGED) {
        *rule_or_why = RX_AEGIS_WHY_PRIVILEGED;
        return RX_AEGIS_DENY;
    }
    for (uint32_t i = 0; i < p->n_rules; i++) {
        const RxAegisRule *r = &p->rules[i];
        if (r->subject != subject || resource < r->res_lo || resource > r->res_hi) continue;
        if (rights & ~r->rights) continue;
        uint64_t l = lease;
        if (r->max_lease && (l == 0 || l > r->max_lease)) l = r->max_lease;
        if (!r->max_lease) l = lease;
        *out_lease = l;
        *rule_or_why = r->id;
        *needs_approval = r->needs_approval;
        return RX_AEGIS_GRANT;
    }
    *rule_or_why = RX_AEGIS_WHY_NO_RULE;
    return RX_AEGIS_DENY;
}

/* ---- aegis.decide.k ---- */

static int fn_decide(RxCtx *c) {
    struct { RxAegisFaculty *f; uint32_t k; } *x = c->user;
    RxAegisFaculty *f = x->f;
    uint32_t k = x->k;
    const RxAegisClientObjects *o = &f->o[k];
    const RxSnapshotDep *req = in_of(c, o->request);
    const RxSnapshotDep *ap = in_of(c, o->approval);
    const RxSnapshotDep *dec = in_of(c, o->decision);
    if (!req || !ap || !dec) return -1;
    uint64_t seq = req->field[0];
    if (seq == 0) return 0;
    if (dec->field[0] == seq) {
        if (dec->field[1] != RX_AEGIS_ESCALATE) return 0;   /* already decided */
        if (ap->field[0] != seq) return 0;                  /* still waiting for a human */
    }
    uint64_t res = req->field[1], lease = 0, op = req->field[5], slot = req->field[4];
    uint32_t rights = (uint32_t)req->field[2], why = 0, na = 0, verdict;
    if (slot >= RX_AEGIS_SLOTS || req->field[2] > UINT32_MAX ||
        (op != RX_AEGIS_OP_ACQUIRE && op != RX_AEGIS_OP_RELEASE)) {
        verdict = RX_AEGIS_DENY;
        why = RX_AEGIS_WHY_BAD_REQUEST;
        rights = 0;
    } else if (op == RX_AEGIS_OP_RELEASE) {
        verdict = RX_AEGIS_REVOKE;
        rights = 0;
    } else {
        verdict = rx_aegis_evaluate(&f->policy, f->client[k].subject, res, rights,
                                    req->field[3], &lease, &why, &na);
        if (verdict == RX_AEGIS_GRANT && na) {
            if (ap->field[0] != seq) verdict = RX_AEGIS_ESCALATE;
            else if (ap->field[1] != RX_AEGIS_APPROVE) {
                verdict = RX_AEGIS_DENY;
                why = RX_AEGIS_WHY_HUMAN;
            }
        }
        if (verdict != RX_AEGIS_GRANT && verdict != RX_AEGIS_ESCALATE) { rights = 0; lease = 0; }
    }
    /* ARGUS: a policy refusal. GRANT and REVOKE become real at root.install;
     * ESCALATE has no ABI v1 kind. A repeated (invalidated) run re-emits. */
    if (verdict == RX_AEGIS_DENY)
        RX_ARGUS_EMIT(rx_argus_emit_cap_denied(aegis_view(f), f->client[k].subject, ARGUS_CAP_NONE, 0, res,
                                               (uint32_t)req->field[2], (int)why));
    put(c, o->decision, 0, seq);
    put(c, o->decision, 1, verdict);
    put(c, o->decision, 2, res);
    put(c, o->decision, 3, rights);
    put(c, o->decision, 4, lease);
    put(c, o->decision, 5, slot);
    put(c, o->decision, 6, why);
    put(c, o->decision, 7, op);
    return 0;
}

/* ---- root.install.k ---- */

static int written_by(RxAegisFaculty *f, const RxSnapshotDep *d, uint32_t n_fields,
                      uint32_t reaction, uint32_t subject, uint32_t faculty, int any_reaction) {
    for (uint32_t i = 0; i < n_fields; i++) {
        uint32_t r, s, fa;
        int rc = rx_world_crumb_origin(f->w, d->field_writer[i], &r, &s, &fa);
        if (rc == 1) continue;                     /* never written since creation */
        if (rc != RX_OK) return 0;
        if (r == UINT32_MAX || s != subject) return 0;
        if (!any_reaction && (r != reaction || fa != faculty)) return 0;
    }
    return 1;
}

static void refuse(RxCtx *c, RxObjRef slot, uint64_t seq, uint64_t why) {
#if RX_ARGUS
    /* ARGUS: every root refusal is a denial of the client. */
    struct { RxAegisFaculty *f; uint32_t k; } *x = c->user;
    rx_argus_emit_cap_denied(aegis_view(x->f), x->f->client[x->k].subject, ARGUS_CAP_NONE, 0, 0, 0,
                             (int)why);
#endif
    put(c, slot, 5, seq);
    put(c, slot, 7, why);
}

/* Revoke the reference the root itself minted for this slot. The slot's
 * contents are never trusted for this: a hostile writer could put another
 * principal's reference there. Caller holds f->mu. */
static void revoke_minted(RxAegisFaculty *f, uint32_t k, uint64_t j, AienosCapRef office) {
    RxCapRef r = f->minted[k][j].ref;
    if (r.cap_id == UINT32_MAX) return;
    int rc = aienos_cap_revoke(f->admin, office, (AienosCapRef){ r.cap_id, r.generation });
    if (rc == 0) f->revokes++;
    RX_ARGUS_EMIT_AUTH(rx_argus_emit_cap_revoked(aegis_view(f), f->client[k].subject, r.cap_id,
                                            (uint64_t)r.generation, rc));
    f->minted[k][j].ref = (RxCapRef){ UINT32_MAX, 0 };
}

static int fn_install(RxCtx *c) {
    struct { RxAegisFaculty *f; uint32_t k; } *x = c->user;
    RxAegisFaculty *f = x->f;
    uint32_t k = x->k;
    const RxAegisClientObjects *o = &f->o[k];
    const RxAegisClient *cl = &f->client[k];
    const RxSnapshotDep *dec = in_of(c, o->decision);
    const RxSnapshotDep *req = in_of(c, o->request);
    if (!dec || !req) return -1;
    uint64_t seq = dec->field[0], verdict = dec->field[1], j = dec->field[5];
    if (seq == 0 || (verdict != RX_AEGIS_GRANT && verdict != RX_AEGIS_REVOKE)) return 0;
    if (j >= RX_AEGIS_SLOTS) return 0;
    const RxSnapshotDep *s = in_of(c, o->slot[j]);
    if (!s) return -1;
    if (s->field[5] == seq) return 0;              /* already answered */

    if (!written_by(f, dec, RX_MAX_FIELDS, f->r_decide[k], RX_AEGIS_SUBJ, RX_FACULTY_AEGIS, 0)) {
        refuse(c, o->slot[j], seq, RX_AEGIS_WHY_DECISION_ORIGIN);
        return 0;
    }
    if (!written_by(f, req, 6, 0, cl->subject, 0, 1)) {
        refuse(c, o->slot[j], seq, RX_AEGIS_WHY_REQUEST_ORIGIN);
        return 0;
    }
    if (dec->field[0] != req->field[0] || dec->field[2] != req->field[1] ||
        dec->field[5] != req->field[4] || dec->field[7] != req->field[5] ||
        (dec->field[3] & ~req->field[2])) {
        refuse(c, o->slot[j], seq, RX_AEGIS_WHY_MISMATCH);
        return 0;
    }

    AienosCapRef office;
    aienos_cap_office(f->admin, &office);
    uint64_t res = dec->field[2];
    uint32_t rights = (uint32_t)dec->field[3];

    if (verdict == RX_AEGIS_REVOKE) {
        pthread_mutex_lock(&f->mu);
        if (f->minted[k][j].seq != seq) {
            revoke_minted(f, k, j, office);
            f->minted[k][j].seq = seq;
        }
        pthread_mutex_unlock(&f->mu);
        put(c, o->slot[j], 2, RX_AEGIS_SLOT_REVOKED);
        put(c, o->slot[j], 5, seq);
        put(c, o->slot[j], 7, 0);
        return 0;
    }

    if (res < cl->domain_lo || res > cl->domain_hi || (rights & ~cl->max_rights) ||
        (rights & RX_RIGHT_PRIVILEGED) || rights == 0) {
        refuse(c, o->slot[j], seq, RX_AEGIS_WHY_DOMAIN);
        return 0;
    }

    RxCapRef ref;
    int ok = 1;
    pthread_mutex_lock(&f->mu);
    if (f->minted[k][j].seq == seq) {
        ref = f->minted[k][j].ref;
    } else {
        revoke_minted(f, k, j, office);            /* renewal replaces the old grant */
        AienosCapMint m = { RX_AEGIS_SUBJ, cl->subject, res, rights, dec->field[4],
                            { UINT32_MAX, 0 }, office };
        AienosCapRef got = { UINT32_MAX, 0 };
        if (aienos_cap_mint(f->admin, &m, &got) == 0) {
            ref = (RxCapRef){ got.cap_id, got.generation };
            f->minted[k][j] = (RxAegisMinted){ seq, ref };
            f->mints++;
            RX_ARGUS_EMIT_AUTH(rx_argus_emit_cap_granted(aegis_view(f), cl->subject, got.cap_id,
                                                    (uint64_t)got.generation, res, rights, 0));
        } else {
            ok = 0;
            ref = (RxCapRef){ UINT32_MAX, 0 };
        }
    }
    pthread_mutex_unlock(&f->mu);
    if (!ok) {
        refuse(c, o->slot[j], seq, RX_AEGIS_WHY_MINT);
        return 0;
    }
    RxCapEntry e;
    uint64_t expiry = rx_world_inspect_cap(f->w, ref, &e) == RX_CAP_OK ? e.lease_expiry : 0;
    put(c, o->slot[j], 0, ref.cap_id);
    put(c, o->slot[j], 1, ref.generation);
    put(c, o->slot[j], 2, RX_AEGIS_SLOT_LIVE);
    put(c, o->slot[j], 3, res);
    put(c, o->slot[j], 4, rights);
    put(c, o->slot[j], 5, seq);
    put(c, o->slot[j], 6, expiry);
    put(c, o->slot[j], 7, 0);
    return 0;
}

/* ---- setup ---- */

int rx_aegis_create(RxAegisFaculty *f, RxWorld *w, AienosCapAdmin *admin,
                    const RxAegisPolicy *policy, const RxAegisClient *clients, uint32_t n) {
    memset(f, 0, sizeof(*f));
    if (!admin || n == 0 || n > RX_AEGIS_CLIENTS || policy->n_rules > RX_AEGIS_RULES)
        return RX_ERR_ARG;
    f->w = w;
    f->admin = admin;
    f->policy = *policy;
    f->n_clients = n;
    if (pthread_mutex_init(&f->mu, NULL) != 0) return RX_ERR_ARG;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    for (uint32_t k = 0; k < n; k++) {
        f->client[k] = clients[k];
        for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
            f->minted[k][j] = (RxAegisMinted){ 0, { UINT32_MAX, 0 } };
#define MK(ref, type, idx) do {                                                         \
        int rc_ = rx_world_create(w, (type), RX_PERSIST_RESIDENT, rx_aegis_res(k, (idx)), \
                                  z, &(ref));                                           \
        if (rc_ != RX_OK) return rc_;                                                   \
    } while (0)
        MK(f->o[k].request, RX_OT_AUTH_REQUEST, RX_AEGIS_RES_REQUEST);
        MK(f->o[k].approval, RX_OT_AUTH_APPROVAL, RX_AEGIS_RES_APPROVAL);
        MK(f->o[k].decision, RX_OT_AUTH_DECISION, RX_AEGIS_RES_DECISION);
        for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
            MK(f->o[k].slot[j], RX_OT_CAP_SLOT, RX_AEGIS_RES_SLOT0 + j);
#undef MK
    }
    return RX_OK;
}

static uint64_t res_of(const RxWorld *w, RxObjRef o) { return w->objects[o.id].resource; }

int rx_aegis_register(RxAegisFaculty *f, const RxAegisCaps caps[]) {
    RxWorld *w = f->w;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    for (uint32_t k = 0; k < f->n_clients; k++) {
        const RxAegisClientObjects *o = &f->o[k];
        f->ctx[k].f = f;
        f->ctx[k].k = k;
        RxReactionDesc d;
        int rc;

        memset(&d, 0, sizeof d);
        d.name = "aegis.decide";
        d.faculty = RX_FACULTY_AEGIS;
        d.subject = RX_AEGIS_SUBJ;
        d.priority = RX_PRIO_FOREGROUND;
        d.fn = fn_decide;
        d.user = &f->ctx[k];
        d.stamp_proposed = true;                   /* the root checks every field's author */
        d.n_triggers = 2;
        d.triggers[0] = (RxDep){ o->request, RX_ALL_FIELDS };
        d.triggers[1] = (RxDep){ o->approval, RX_ALL_FIELDS };
        d.n_reads = 1;
        d.reads[0] = (RxDep){ o->decision, RX_ALL_FIELDS };
        d.n_writes = 1;
        d.writes[0] = (RxDep){ o->decision, RX_ALL_FIELDS };
        d.n_caps = 3;
        d.caps[0] = (RxCapNeed){ caps[k].aegis_request, res_of(w, o->request), R };
        d.caps[1] = (RxCapNeed){ caps[k].aegis_approval, res_of(w, o->approval), R };
        d.caps[2] = (RxCapNeed){ caps[k].aegis_decision, res_of(w, o->decision), RW };
        if ((rc = rx_world_add_reaction_keyed(w, f->keys, &d, &f->r_decide[k])) != RX_OK) return rc;

        memset(&d, 0, sizeof d);
        d.name = "root.install";
        d.faculty = RX_FACULTY_ROOT;
        d.subject = RX_AEGIS_ROOT_SUBJ;
        d.priority = RX_PRIO_FOREGROUND;
        d.fn = fn_install;
        d.user = &f->ctx[k];
        d.n_triggers = 1;
        d.triggers[0] = (RxDep){ o->decision, RX_ALL_FIELDS };
        d.n_reads = 1 + RX_AEGIS_SLOTS;
        d.reads[0] = (RxDep){ o->request, RX_ALL_FIELDS };
        d.n_writes = RX_AEGIS_SLOTS;
        d.n_caps = 2 + RX_AEGIS_SLOTS;
        d.caps[0] = (RxCapNeed){ caps[k].root_request, res_of(w, o->request), R };
        d.caps[1] = (RxCapNeed){ caps[k].root_decision, res_of(w, o->decision), R };
        for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++) {
            d.reads[1 + j] = (RxDep){ o->slot[j], RX_ALL_FIELDS };
            d.writes[j] = (RxDep){ o->slot[j], RX_ALL_FIELDS };
            d.caps[2 + j] = (RxCapNeed){ caps[k].root_slot[j], res_of(w, o->slot[j]), RW };
        }
        if ((rc = rx_world_add_reaction_keyed(w, f->keys, &d, &f->r_install[k])) != RX_OK) return rc;
    }
    return RX_OK;
}

void rx_aegis_destroy(RxAegisFaculty *f) { pthread_mutex_destroy(&f->mu); }

void rx_aegis_use_slot(RxReactionDesc *d, uint32_t i, RxObjRef slot) {
    d->cap_slotted[i] = true;
    d->cap_slot[i] = slot;
    if (d->n_triggers < RX_MAX_DEPS)
        d->triggers[d->n_triggers++] = (RxDep){ slot, RX_FIELD(0) | RX_FIELD(1) | RX_FIELD(2) };
}
