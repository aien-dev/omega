/* fabric.c -- AIEN Fabric interface F5-0. See fabric.h. */
#include "fabric.h"

#include <string.h>

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t r64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static size_t body_size(uint32_t kind) {
    switch (kind) {
    case FAB_MSG_JOIN: return FAB_JOIN_BODY;
    case FAB_MSG_RENEW: return FAB_RENEW_BODY;
    case FAB_MSG_ADVERTISE: return FAB_ADVERTISE_BODY;
    case FAB_MSG_LEAVE: return 0;
    default: return (size_t)-1;
    }
}

static int on_roster(const FabRoster *r, const AienMachineId *m) {
    for (uint32_t i = 0; i < r->n; i++)
        if (aien_mid_equal(&r->ids[i], m)) return 1;
    return 0;
}

static FabMember *member_find(FabNode *n, const AienMachineId *m) {
    for (uint32_t i = 0; i < n->n_members; i++)
        if (aien_mid_equal(&n->members[i].machine, m)) return &n->members[i];
    return NULL;
}

static void event(FabNode *n, uint32_t kind, int code, int capq, uint32_t msg_kind,
                  const AienMachineId *m, uint64_t gen, uint64_t seq, uint64_t at) {
    FabEvent *e = &n->events[n->n_events % FAB_EVENT_RING];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->code = code;
    e->capq = capq;
    e->msg_kind = msg_kind;
    if (m) e->machine = *m;
    e->generation = gen;
    e->seq = seq;
    e->at_us = at;
    n->n_events++;
    /* Canonical bytes, not the struct (padding). */
    uint8_t b[4 * 4 + 1 + AIEN_MID_ID_BYTES + 3 * 8];
    w32(b, kind);
    w32(b + 4, (uint32_t)code);
    w32(b + 8, (uint32_t)capq);
    w32(b + 12, msg_kind);
    b[16] = e->machine.root;
    memcpy(b + 17, e->machine.id, AIEN_MID_ID_BYTES);
    w64(b + 49, gen);
    w64(b + 57, seq);
    w64(b + 65, at);
    sha256_update(&n->event_hash, b, sizeof b);
    if (code < 0 && -code < 16) n->counts[-code]++;
}

/* Withdraw every entry the member advertised. */
static void withdraw_all(FabNode *n, FabMember *m) {
    for (uint32_t i = 0; i < m->n_keys; i++) (void)cq_withdraw(n->cfg.catalog, &m->keys[i]);
}

int fab_node_init(FabNode *n, const FabConfig *cfg) {
    if (!n || !cfg || !cfg->roster || !cfg->catalog || !cfg->transport || !cfg->auth ||
        !cfg->transport->send || !cfg->transport->recv || !cfg->auth->sign ||
        !cfg->auth->verify || cfg->generation == 0 || cfg->max_lease_us == 0)
        return FAB_E_ARG;
    if (aien_mid_is_zero_(cfg->self.id) || !cfg->catalog->machines) return FAB_E_ARG;
    if (cq_machine_index(cfg->catalog, &cfg->self) != cfg->catalog->self_machine) return FAB_E_ARG;
    memset(n, 0, sizeof *n);
    n->cfg = *cfg;
    sha256_init(&n->event_hash);
    return FAB_OK;
}

int fab_node_set_generation(FabNode *n, uint64_t generation) {
    if (!n || generation <= n->cfg.generation) return FAB_E_STALE_GEN;
    n->cfg.generation = generation;
    n->seq = 0;
    return FAB_OK;
}

uint64_t fab_entry_generation(const FabNode *n, uint32_t rev) {
    return n->cfg.generation << 32 | rev;
}

int fab_seal(FabNode *n, const AienMachineId *dest, uint32_t kind, const uint8_t *body,
             size_t body_len, uint64_t now_us, uint8_t *out, size_t *out_len) {
    if (!n || !dest || !out || !out_len || body_size(kind) != body_len || (body_len && !body))
        return FAB_E_ARG;
    memcpy(out, "AFAB", 4);
    out[4] = FAB_VERSION;
    out[5] = (uint8_t)kind;
    w16(out + 6, (uint16_t)body_len);
    if (aien_mid_encode(&n->cfg.self, out + 8) != AIEN_MID_OK ||
        aien_mid_encode(dest, out + 52) != AIEN_MID_OK)
        return FAB_E_ARG;
    w64(out + 96, n->cfg.generation);
    w64(out + 104, ++n->seq);
    w64(out + 112, now_us);
    if (body_len) memcpy(out + FAB_HDR_BYTES, body, body_len);
    if (n->cfg.auth->sign(n->cfg.auth->ctx, out, FAB_HDR_BYTES + body_len,
                          out + FAB_HDR_BYTES + body_len) != 0)
        return FAB_E_AUTH;
    *out_len = FAB_HDR_BYTES + body_len + FAB_TAG_BYTES;
    return FAB_OK;
}

static int broadcast(FabNode *n, uint32_t kind, const uint8_t *body, size_t body_len,
                     uint64_t now_us) {
    uint8_t msg[FAB_MSG_MAX];
    size_t len;
    int first = FAB_OK;
    for (uint32_t i = 0; i < n->cfg.roster->n; i++) {
        const AienMachineId *to = &n->cfg.roster->ids[i];
        if (aien_mid_equal(to, &n->cfg.self)) continue;
        int rc = fab_seal(n, to, kind, body, body_len, now_us, msg, &len);
        if (rc == FAB_OK && n->cfg.transport->send(n->cfg.transport->ctx, &n->cfg.self, to, msg, len) != 0)
            rc = FAB_E_TRANSPORT;
        if (rc != FAB_OK && first == FAB_OK) first = rc;
    }
    return first;
}

int fab_join(FabNode *n, uint64_t lease_us, uint64_t now_us) {
    if (!n || lease_us == 0) return FAB_E_ARG;
    uint8_t b[FAB_JOIN_BODY];
    cq_ontology_digest(n->cfg.catalog, b);
    w64(b + 32, lease_us);
    return broadcast(n, FAB_MSG_JOIN, b, sizeof b, now_us);
}

int fab_renew(FabNode *n, uint64_t lease_us, uint64_t now_us) {
    if (!n || lease_us == 0) return FAB_E_ARG;
    uint8_t b[FAB_RENEW_BODY];
    w64(b, lease_us);
    return broadcast(n, FAB_MSG_RENEW, b, sizeof b, now_us);
}

int fab_leave(FabNode *n, uint64_t now_us) {
    if (!n) return FAB_E_ARG;
    return broadcast(n, FAB_MSG_LEAVE, NULL, 0, now_us);
}

int fab_advertise(FabNode *n, const CqKey *own, uint32_t wire_kind, uint64_t now_us) {
    if (!n || !own) return FAB_E_ARG;
    const CqEntry *e = cq_lookup(n->cfg.catalog, own);
    if (!e) return FAB_E_ARG;
    uint8_t rec[CQ_WIRE_BYTES];
    if (cq_wire_encode(n->cfg.catalog, e, wire_kind, rec) != CQ_OK) return FAB_E_CAPQ;
    return broadcast(n, FAB_MSG_ADVERTISE, rec, sizeof rec, now_us);
}

int fab_tick(FabNode *n, uint64_t now_us) {
    if (!n) return FAB_E_ARG;
    int lost = 0;
    for (uint32_t i = 0; i < n->n_members; i++) {
        FabMember *m = &n->members[i];
        if (m->state != FAB_ST_JOINED || m->lease_until_us > now_us) continue;
        withdraw_all(n, m);
        m->state = FAB_ST_LOST;
        event(n, FAB_EV_LOST, FAB_OK, 0, 0, &m->machine, m->generation, m->last_seq, now_us);
        lost++;
    }
    return lost;
}

/* The lease a request gets, or 0 when none can be granted (zero, or an end
 * past UINT64_MAX). */
static uint64_t lease_for(const FabNode *n, uint64_t requested, uint64_t now_us) {
    uint64_t lease = requested < n->cfg.max_lease_us ? requested : n->cfg.max_lease_us;
    return lease > UINT64_MAX - now_us ? 0 : lease;
}

static int grant(FabNode *n, FabMember *m, uint64_t requested, uint64_t now_us) {
    uint64_t lease = lease_for(n, requested, now_us);
    if (lease == 0) return FAB_E_FORMAT;
    uint32_t idx = 0;
    if (cq_machine_advertise_id(n->cfg.catalog, &m->machine, now_us + lease, &idx) != CQ_OK)
        return FAB_E_FULL;               /* nothing changed */
    m->lease_until_us = now_us + lease;
    m->index = idx;
    return FAB_OK;
}

static int refuse(FabNode *n, FabVerdict *v, int code, int capq, const AienMachineId *who,
                  uint64_t gen, uint64_t seq, uint64_t now_us) {
    v->code = code;
    v->capq = capq;
    event(n, FAB_EV_REFUSED, code, capq, v->msg_kind, who, gen, seq, now_us);
    return code;
}

int fab_receive(FabNode *n, const uint8_t *msg, size_t len, uint64_t now_us, FabVerdict *v) {
    FabVerdict scratch;
    if (!v) v = &scratch;
    memset(v, 0, sizeof *v);
    if (!n || !msg) return v->code = FAB_E_ARG;
    fab_tick(n, now_us);

    /* form */
    if (len < FAB_HDR_BYTES + FAB_TAG_BYTES || memcmp(msg, "AFAB", 4) != 0 || msg[4] != FAB_VERSION)
        return refuse(n, v, FAB_E_FORMAT, 0, NULL, 0, 0, now_us);
    uint32_t kind = msg[5];
    size_t body_len = r16(msg + 6);
    v->msg_kind = kind;
    if (body_size(kind) != body_len || len != FAB_HDR_BYTES + body_len + FAB_TAG_BYTES)
        return refuse(n, v, FAB_E_FORMAT, 0, NULL, 0, 0, now_us);
    AienMachineId sender, dest;
    if (aien_mid_decode(msg + 8, AIEN_MID_RECORD_BYTES, &sender) != AIEN_MID_OK ||
        aien_mid_decode(msg + 52, AIEN_MID_RECORD_BYTES, &dest) != AIEN_MID_OK)
        return refuse(n, v, FAB_E_FORMAT, 0, NULL, 0, 0, now_us);
    v->sender = sender;
    uint64_t gen = r64(msg + 96), seq = r64(msg + 104);
    if (gen == 0 || seq == 0) return refuse(n, v, FAB_E_FORMAT, 0, &sender, gen, seq, now_us);

    /* addressing and identity */
    if (!aien_mid_equal(&dest, &n->cfg.self) || aien_mid_equal(&sender, &n->cfg.self))
        return refuse(n, v, FAB_E_MISMATCH, 0, &sender, gen, seq, now_us);
    if (!on_roster(n->cfg.roster, &sender))
        return refuse(n, v, FAB_E_NOT_ENROLLED, 0, &sender, gen, seq, now_us);
    if (n->cfg.auth->verify(n->cfg.auth->ctx, &sender, msg, FAB_HDR_BYTES + body_len,
                            msg + FAB_HDR_BYTES + body_len) != 0)
        return refuse(n, v, FAB_E_AUTH, 0, &sender, gen, seq, now_us);

    /* freshness and membership */
    FabMember *m = member_find(n, &sender);
    if (m && m->state != FAB_ST_NONE) {
        if (gen < m->generation) return refuse(n, v, FAB_E_STALE_GEN, 0, &sender, gen, seq, now_us);
        if (gen == m->generation && seq <= m->last_seq)
            return refuse(n, v, FAB_E_REPLAY, 0, &sender, gen, seq, now_us);
        /* An authenticated, fresh message of the held generation consumes its
         * sequence number whatever the outcome, so a refused message (for
         * example a RENEW refused for catalog capacity) cannot succeed if it
         * is replayed later. */
        if (gen == m->generation) m->last_seq = seq;
    }
    const uint8_t *body = msg + FAB_HDR_BYTES;
    if (kind == FAB_MSG_JOIN) {
        if (m && m->state != FAB_ST_NONE && gen <= m->generation)
            return refuse(n, v, FAB_E_STALE_GEN, 0, &sender, gen, seq, now_us);
        uint8_t dg[32];
        cq_ontology_digest(n->cfg.catalog, dg);
        if (memcmp(dg, body, 32) != 0) return refuse(n, v, FAB_E_ONTOLOGY, 0, &sender, gen, seq, now_us);
        if (lease_for(n, r64(body + 32), now_us) == 0) return refuse(n, v, FAB_E_FORMAT, 0, &sender, gen, seq, now_us);
        int fresh = !m;
        if (!m) {
            if (n->n_members == FAB_MAX_MEMBERS)
                return refuse(n, v, FAB_E_FULL, 0, &sender, gen, seq, now_us);
            m = &n->members[n->n_members++];
            memset(m, 0, sizeof *m);
            m->machine = sender;
        }
        int rc = grant(n, m, r64(body + 32), now_us);
        if (rc != FAB_OK) {
            if (fresh) n->n_members--;  /* refused: leave no trace */
            return refuse(n, v, rc, 0, &sender, gen, seq, now_us);
        }
        withdraw_all(n, m);             /* anything from an earlier generation */
        m->n_keys = 0;
        m->state = FAB_ST_JOINED;
        m->generation = gen;
        m->last_seq = seq;
        event(n, FAB_EV_JOINED, FAB_OK, 0, kind, &sender, gen, seq, now_us);
        return v->code = FAB_OK;
    }
    if (!m || m->state == FAB_ST_NONE || m->state == FAB_ST_LEFT || gen != m->generation)
        return refuse(n, v, FAB_E_NOT_MEMBER, 0, &sender, gen, seq, now_us);
    if (m->state == FAB_ST_LOST || m->lease_until_us <= now_us)
        return refuse(n, v, FAB_E_LEASE_EXPIRED, 0, &sender, gen, seq, now_us);

    switch (kind) {
    case FAB_MSG_RENEW: {
        uint64_t req = r64(body);
        if (lease_for(n, req, now_us) == 0) return refuse(n, v, FAB_E_FORMAT, 0, &sender, gen, seq, now_us);
        int rc = grant(n, m, req, now_us);
        if (rc != FAB_OK) return refuse(n, v, rc, 0, &sender, gen, seq, now_us);
        m->last_seq = seq;
        event(n, FAB_EV_RENEWED, FAB_OK, 0, kind, &sender, gen, seq, now_us);
        return v->code = FAB_OK;
    }
    case FAB_MSG_ADVERTISE: {
        uint32_t wkind;
        AienMachineId named;
        CqEntry e;
        int cq = cq_wire_decode(body, CQ_WIRE_BYTES, &wkind, &named, &e);
        if (cq != CQ_OK) return refuse(n, v, FAB_E_CAPQ, cq, &sender, gen, seq, now_us);
        if (!aien_mid_equal(&named, &sender))
            return refuse(n, v, FAB_E_MISMATCH, 0, &sender, gen, seq, now_us);
        CqKey k = { e.capability_id, e.realization_id, m->index, e.skill_id };
        uint32_t i = 0;
        while (i < m->n_keys && memcmp(&m->keys[i], &k, sizeof k) != 0) i++;
        if (i == m->n_keys && m->n_keys == FAB_MAX_KEYS)
            return refuse(n, v, FAB_E_FULL, 0, &sender, gen, seq, now_us);
        CqKey got;
        cq = cq_wire_apply(n->cfg.catalog, body, CQ_WIRE_BYTES, &got);
        if (cq != CQ_OK) return refuse(n, v, FAB_E_CAPQ, cq, &sender, gen, seq, now_us);
        /* Track the key the graph actually holds, so loss and LEAVE always
         * withdraw the real entry. */
        uint32_t j = 0;
        while (j < m->n_keys && memcmp(&m->keys[j], &got, sizeof got) != 0) j++;
        if (j == m->n_keys) {
            if (m->n_keys == FAB_MAX_KEYS) {  /* untrackable: take it back out */
                (void)cq_withdraw(n->cfg.catalog, &got);
                return refuse(n, v, FAB_E_FULL, 0, &sender, gen, seq, now_us);
            }
            m->keys[m->n_keys++] = got;
        }
        if (!n->cfg.catalog->built) (void)cq_catalog_build(n->cfg.catalog);
        m->last_seq = seq;
        event(n, FAB_EV_ADVERTISED, FAB_OK, CQ_OK, kind, &sender, gen, seq, now_us);
        return v->code = FAB_OK;
    }
    case FAB_MSG_LEAVE: {
        withdraw_all(n, m);
        m->state = FAB_ST_LEFT;
        m->lease_until_us = 0;
        (void)cq_machine_advertise_id(n->cfg.catalog, &m->machine, 0, NULL);
        m->last_seq = seq;
        event(n, FAB_EV_LEFT, FAB_OK, 0, kind, &sender, gen, seq, now_us);
        return v->code = FAB_OK;
    }
    }
    return refuse(n, v, FAB_E_FORMAT, 0, &sender, gen, seq, now_us);
}

int fab_poll(FabNode *n, uint64_t now_us, FabVerdict *v) {
    if (!n) return FAB_E_ARG;
    uint8_t buf[FAB_MSG_MAX + 64];
    size_t len = 0;
    int got = n->cfg.transport->recv(n->cfg.transport->ctx, &n->cfg.self, buf, sizeof buf, &len);
    if (got <= 0) return got < 0 ? FAB_E_TRANSPORT : 0;
    fab_receive(n, buf, len, now_us, v);
    return 1;
}

const FabMember *fab_member(const FabNode *n, const AienMachineId *m) {
    return n && m ? member_find((FabNode *)n, m) : NULL;
}

int fab_member_live(const FabNode *n, const AienMachineId *m, uint64_t now_us) {
    const FabMember *x = fab_member(n, m);
    return x && x->state == FAB_ST_JOINED && x->lease_until_us > now_us;
}

int fab_home(const FabNode *n, const AienMachineId *m, uint64_t now_us, JsHome *out) {
    if (!n || !m || !out) return FAB_E_ARG;
    memset(out, 0, sizeof *out);
    if (aien_mid_equal(m, &n->cfg.self)) {
        out->locality = JS_HOME_LOCAL;
    } else if (fab_member_live(n, m, now_us)) {
        out->locality = JS_HOME_REMOTE_OWNED;
    } else {
        return FAB_E_NOT_MEMBER;
    }
    aien_mid_to_slot(m, out->machine);
    return FAB_OK;
}

void fab_state_digest(const FabNode *n, uint8_t out[32]) {
    sha256_ctx h, ev = n->event_hash;
    uint8_t evd[32], b[8];
    sha256_final(&ev, evd);
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)"AFAB-STATE-V1", 13);
    w64(b, n->n_events);
    sha256_update(&h, b, 8);
    sha256_update(&h, evd, 32);
    for (uint32_t i = 0; i < n->n_members; i++) {
        const FabMember *m = &n->members[i];
        sha256_update(&h, &m->machine.root, 1);  /* provenance, not identity */
        sha256_update(&h, m->machine.id, AIEN_MID_ID_BYTES);
        w32(b, m->state); sha256_update(&h, b, 4);
        w64(b, m->generation); sha256_update(&h, b, 8);
        w64(b, m->last_seq); sha256_update(&h, b, 8);
        w64(b, m->lease_until_us); sha256_update(&h, b, 8);
        w32(b, m->n_keys); sha256_update(&h, b, 4);
        for (uint32_t k = 0; k < m->n_keys; k++) {
            w32(b, m->keys[k].capability_id); sha256_update(&h, b, 4);
            w32(b, m->keys[k].realization_id); sha256_update(&h, b, 4);
            w32(b, m->keys[k].skill_id); sha256_update(&h, b, 4);
        }
    }
    sha256_final(&h, out);
}

const char *fab_strerror(int code) {
    switch (code) {
    case FAB_OK: return "ok";
    case FAB_E_ARG: return "bad argument";
    case FAB_E_FORMAT: return "malformed message";
    case FAB_E_MISMATCH: return "machine mismatch";
    case FAB_E_NOT_ENROLLED: return "sender not enrolled";
    case FAB_E_AUTH: return "authentication failed (forged identity or altered bytes)";
    case FAB_E_STALE_GEN: return "stale generation";
    case FAB_E_REPLAY: return "replayed message";
    case FAB_E_LEASE_EXPIRED: return "lease expired";
    case FAB_E_NOT_MEMBER: return "not a member";
    case FAB_E_ONTOLOGY: return "operation catalog differs";
    case FAB_E_CAPQ: return "capability graph refused the record";
    case FAB_E_FULL: return "table full";
    case FAB_E_TRANSPORT: return "transport error";
    default: return "unknown";
    }
}
