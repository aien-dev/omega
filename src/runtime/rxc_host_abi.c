/*
 * rxc_host_abi.c -- the host ABI facade over COMPOSITION-2 (rxc_host_abi.h).
 *
 * Everything here forwards to existing calls: aien_mid_* (identity),
 * cx_open read-only (torn-tail probe), cq_* / sr_register_skill (the Skill
 * graph, built exactly as the COMPOSITION-2 tests build it), aienos_cap_*
 * (authority), rx_compose_open/run/close, cx_recall/cx_recall_id. The only
 * state it adds is the per-handle slot that lets a ctx-less RxcContract and
 * AgSkillFn reach the host callback.
 */
#include "runtime/rxc_host_abi.h"

#include "runtime/aien_machine_id.h"
#include "runtime/aienos_cap.h"
#include "runtime/rx_capq.h"
#include "runtime/rx_compose.h"
#include "runtime/rx_cortex.h"
#include "runtime/rx_cortex_record.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_skillroute.h"
#include "sha256.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(RXC_HOST_MAX_SKILLS == RXC_K, "one host Skill per composition alternative");
_Static_assert(RXC_HOST_SUBJECT_GOAL == RXC_CX_SUBJECT(RXC_SLOT_GOAL), "goal subject");
_Static_assert(RXC_HOST_SUBJECT_STATE == RXC_CX_SUBJECT(RXC_SLOT_STATE), "state subject");
_Static_assert((int)RXC_HOST_OUT_COMMITTED == (int)RXC_OUT_COMMITTED && (int)RXC_HOST_OUT_RECORD_FAILED ==
               (int)RXC_OUT_RECORD_FAILED, "outcome codes");
_Static_assert((int)RXC_HOST_ROOT_PROVISIONED == (int)AIEN_MID_ROOT_PROVISIONED &&
               (int)RXC_HOST_ROOT_HARDWARE == (int)AIEN_MID_ROOT_HARDWARE, "root kinds");
_Static_assert(RXC_HOST_NONE == RXC_NONE, "no winner");

#define RXC_HOST_OP 1u               /* the one semantic operation every host Skill provides */
#define RXC_HOST_SKILL_ID0 1u        /* Skill ids: RXC_HOST_SKILL_ID0 + index */
#define RXC_HOST_N_WORKERS 2u

typedef struct {
    char name[RXC_HOST_NAME_MAX];
    RxcHostSkillFn fn;
    void *ctx;
    uint64_t cost;
    uint8_t digest[32];
} HostSkill;

struct RxcHost {
    uint32_t slot;
    char dir[200];
    uint64_t session;
    AienMachineId self;
    AienMachineId slots[8];
    AienMachineIndex ix;
    CqCatalog cat;
    int cat_init;
    AgSkillTable skills;
    SrRouter router;
    HostSkill sk[RXC_HOST_MAX_SKILLS];
    uint32_t n_sk;
    RxcHostVerifyFn verify;
    void *verify_ctx;
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxCompose *c;
    int opened;
    RxcHostInfo info;
};

/* ---- slots: ctx-less contract / Skill functions reach their handle ----- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static RxcHost *volatile g_slot[RXC_HOST_MAX_HANDLES];

static int host_contract(uint32_t slot, uint64_t input, uint64_t result) {
    RxcHost *h = g_slot[slot];
    if (!h) return 0;
    if (h->verify) return h->verify(h->verify_ctx, input, result) == 1;
    return result != 0;
}

static uint64_t host_skill(uint32_t slot, uint32_t k, const uint64_t *in, uint32_t n, int *failed) {
    RxcHost *h = g_slot[slot];
    uint64_t result = 0;
    *failed = 1;
    if (!h || k >= h->n_sk || !h->sk[k].fn || n == 0) return 0;
    if (h->sk[k].fn(h->sk[k].ctx, in[0], &result) == 0 && result != 0) *failed = 0;
    return *failed ? 0 : result;
}

#define RXC_HOST_TRAMPOLINES(S)                                                            \
    static int host_contract_##S(uint64_t i, uint64_t r) { return host_contract(S, i, r); } \
    static uint64_t host_skill_##S##_0(const uint64_t *in, uint32_t n, uint32_t a, int *f) { \
        (void)a; return host_skill(S, 0, in, n, f); }                                       \
    static uint64_t host_skill_##S##_1(const uint64_t *in, uint32_t n, uint32_t a, int *f) { \
        (void)a; return host_skill(S, 1, in, n, f); }
RXC_HOST_TRAMPOLINES(0)
RXC_HOST_TRAMPOLINES(1)
RXC_HOST_TRAMPOLINES(2)
RXC_HOST_TRAMPOLINES(3)
_Static_assert(RXC_HOST_MAX_HANDLES == 4u, "one RXC_HOST_TRAMPOLINES line per handle slot");

static const RxcContract g_contract[RXC_HOST_MAX_HANDLES] = {
    host_contract_0, host_contract_1, host_contract_2, host_contract_3 };
static const AgSkillFn g_skillfn[RXC_HOST_MAX_HANDLES][RXC_HOST_MAX_SKILLS] = {
    { host_skill_0_0, host_skill_0_1 }, { host_skill_1_0, host_skill_1_1 },
    { host_skill_2_0, host_skill_2_1 }, { host_skill_3_0, host_skill_3_1 } };

/* ---- helpers ------------------------------------------------------------- */

static void fill_identity(RxcHostInfo *info, const AienMachineId *m) {
    info->machine_root = m->root;
    memcpy(info->machine_id, m->id, sizeof info->machine_id);
}

static void fill_record(RxcHostRecord *r, const CxRecord *x) {
    memset(r, 0, sizeof *r);
    r->id = x->hdr.id;
    r->cls = x->hdr.cls;
    r->kind = x->hdr.kind;
    r->subject = x->hdr.subject;
    r->t = x->hdr.t;
    r->generation = x->hdr.generation;
    r->tag = x->hdr.tag;
    for (uint32_t i = 0; i < 4; i++) r->links[i] = x->hdr.links[i];
    r->n_payload = x->hdr.n;
    r->verified = x->verified ? 1u : 0u;
    memcpy(r->digest, x->hdr.digest, 32);
}

static CqEntry host_provide(uint32_t cap, uint32_t real, uint64_t cost) {
    CqEntry e;
    memset(&e, 0, sizeof e);
    e.capability_id = cap;
    e.realization_id = real;
    e.op = RXC_HOST_OP;
    e.effects = CQ_FX_PURE;
    e.in_types = 0x1;
    e.out_types = 0x2;
    e.confidence_ppm = 900000;
    e.reliability_ppm = 990000;
    e.evidence_level = CQ_EV_MEASURED;
    e.evidence_ref = 0xE000u + cap;
    e.cost = cost;
    e.latency_us = 100;
    e.energy_uj = 100;
    e.live = CQ_LIVE_AVAILABLE;
    e.generation = 1;
    return e;
}

static SrRequirement host_requirement(void) {
    SrRequirement q;
    memset(&q, 0, sizeof q);
    q.need.semantic_operation = RXC_HOST_OP;
    q.need.accepted_input_types = ~0ull;
    q.need.effect_class = 0x1FF;
    q.need.authority_ceiling.resource_hi = UINT64_MAX;
    q.need.authority_ceiling.rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
    q.t.n_order = 1;
    q.t.order[0] = CQ_DIM_COST;
    q.t.k = CQ_MAX_K;
    q.local_only = 1;
    return q;
}

static void refresh_info(RxcHost *h) {
    h->info.opened = (uint32_t)h->opened;
    h->info.n_skills = h->n_sk;
    if (h->opened) {
        h->info.records = h->c->cx.n;
        h->info.recovered_record = h->c->recovered_record;
        h->info.rolled_back = h->c->rolled_back;
        h->info.recovered_completed = h->c->recovered_completed;
        h->info.anchor_unknown = h->c->anchor_unknown ? 1u : 0u;
    }
}

/* Build the Skill graph, start the authority, open the composition. */
static int host_seal(RxcHost *h) {
    if (h->opened) return RXC_HOST_OK;
    if (h->info.open_rc != 0) return RXC_HOST_E_OPEN;   /* a failed open is final */
    aien_mid_index_init(&h->ix, h->slots, 8);
    if (cq_catalog_init_canonical(&h->cat, &h->ix, &h->self, 8, 8) != CQ_OK) return RXC_HOST_E_NOMEM;
    h->cat_init = 1;
    uint32_t all = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC);
    if (cq_op_define(&h->cat, RXC_HOST_OP, 0, all) != CQ_OK) return RXC_HOST_E_STATE;
    h->skills.n = h->n_sk;
    for (uint32_t i = 0; i < h->n_sk; i++) {
        h->skills.skill[i].id = RXC_HOST_SKILL_ID0 + i;
        h->skills.skill[i].fn = g_skillfn[h->slot][i];
        memcpy(h->skills.skill[i].identity, h->sk[i].digest, 32);
        SrSkill s;
        memset(&s, 0, sizeof s);
        s.skill_id = RXC_HOST_SKILL_ID0 + i;
        s.version = 1;
        memcpy(s.digest, h->sk[i].digest, 32);
        s.machine = h->cat.self_machine;
        CqEntry e = host_provide(1 + i, 70 + i, h->sk[i].cost);
        if (sr_register_skill(&h->cat, &s, &e, 1) != SR_OK) return RXC_HOST_E_STATE;
    }
    if (cq_catalog_build(&h->cat) != CQ_OK) return RXC_HOST_E_STATE;
    h->router = (SrRouter){ &h->cat, &h->skills };
    if (aienos_cap_start(&h->admin, &h->view) != 0) {
        h->admin = NULL;
        h->info.open_rc = RX_ERR_AUTHORITY;
        return RXC_HOST_E_OPEN;
    }
    int rc = rx_compose_open(h->c, h->dir, &h->self, h->session, &h->router,
                             g_contract[h->slot], h->admin, h->view, RXC_HOST_N_WORKERS);
    h->info.open_rc = rc;
    if (rc != RX_OK) {
        aienos_cap_stop(h->admin, h->view);
        h->admin = NULL;
        return rc == RX_ERR_IDENTITY ? RXC_HOST_E_IDENTITY : RXC_HOST_E_OPEN;
    }
    h->opened = 1;
    refresh_info(h);
    return RXC_HOST_OK;
}

/* ---- public API ------------------------------------------------------------ */

int rxc_host_open(const char *dir, uint32_t root_kind, const uint8_t *root, size_t root_len,
                  uint64_t session, uint32_t flags, RxcHost **out, RxcHostInfo *info) {
    if (out) *out = NULL;
    if (info) memset(info, 0, sizeof *info);
    if (!dir || !*dir || !root || !out || strlen(dir) >= 200 - 16 ||
        (flags & ~RXC_HOST_OPEN_REFUSE_TORN))
        return RXC_HOST_E_ARG;
    AienMachineId self;
    if (aien_mid_derive((uint8_t)root_kind, root, root_len, &self) != AIEN_MID_OK)
        return RXC_HOST_E_IDENTITY;

    RxcHostInfo inf;
    memset(&inf, 0, sizeof inf);
    inf.abi_version = RXC_HOST_ABI_VERSION;
    fill_identity(&inf, &self);
    if (mkdir(dir, 0700) && errno != EEXIST) return RXC_HOST_E_ARG;

    /* Identity first (rx_compose_open checks it again). */
    char p[256];
    snprintf(p, sizeof p, "%s/machine.id", dir);
    if (access(p, F_OK) == 0) {
        AienMachineId stored;
        if (aien_mid_load(p, &stored) != AIEN_MID_OK || !aien_mid_equal(&stored, &self)) {
            if (info) *info = inf;
            return RXC_HOST_E_IDENTITY;
        }
        inf.machine_id_was_stored = 1;
    }
    /* Torn-tail probe: read-only, no lock, no repair. */
    snprintf(p, sizeof p, "%s/cortex.cx", dir);
    if (access(p, F_OK) == 0) {
        CxStore probe;
        memset(&probe, 0, sizeof probe);
        int prc = cx_open(&probe, p, RX_CORTEX_SUBJECTS, CX_OPEN_READONLY);
        if (prc == CX_OK) {
            inf.records = probe.n;
            cx_close(&probe);
        } else if (prc == CX_ERR_TORN) {
            inf.tail_torn = 1;
        } else {
            inf.open_rc = prc;
            if (info) *info = inf;
            return RXC_HOST_E_OPEN;
        }
    }
    if (inf.tail_torn && (flags & RXC_HOST_OPEN_REFUSE_TORN)) {
        if (info) *info = inf;
        return RXC_HOST_E_TORN;
    }

    RxcHost *h = calloc(1, sizeof *h);
    if (!h) return RXC_HOST_E_NOMEM;
    h->c = calloc(1, sizeof *h->c);
    if (!h->c) { free(h); return RXC_HOST_E_NOMEM; }
    pthread_mutex_lock(&g_mu);
    uint32_t slot = RXC_HOST_MAX_HANDLES;
    for (uint32_t i = 0; i < RXC_HOST_MAX_HANDLES; i++)
        if (!g_slot[i]) { slot = i; break; }
    if (slot < RXC_HOST_MAX_HANDLES) g_slot[slot] = h;
    pthread_mutex_unlock(&g_mu);
    if (slot == RXC_HOST_MAX_HANDLES) {
        free(h->c);
        free(h);
        return RXC_HOST_E_FULL;
    }
    h->slot = slot;
    snprintf(h->dir, sizeof h->dir, "%s", dir);
    h->session = session;
    h->self = self;
    h->info = inf;
    *out = h;
    if (info) *info = h->info;
    return RXC_HOST_OK;
}

int rxc_host_register_skill(RxcHost *h, const char *name, const uint8_t *digest32, uint64_t cost,
                            RxcHostSkillFn fn, void *ctx) {
    if (!h || !name || !fn) return RXC_HOST_E_ARG;
    size_t len = strlen(name);
    if (len == 0 || len >= RXC_HOST_NAME_MAX) return RXC_HOST_E_ARG;
    if (h->opened || h->info.open_rc != 0) return RXC_HOST_E_STATE;
    if (h->n_sk >= RXC_HOST_MAX_SKILLS) return RXC_HOST_E_FULL;
    HostSkill *s = &h->sk[h->n_sk];
    memset(s, 0, sizeof *s);
    memcpy(s->name, name, len);
    s->fn = fn;
    s->ctx = ctx;
    s->cost = cost;
    if (digest32) memcpy(s->digest, digest32, 32);
    else sha256_hash((const uint8_t *)name, len, s->digest);
    return (int)h->n_sk++;
}

int rxc_host_set_verify(RxcHost *h, RxcHostVerifyFn fn, void *ctx) {
    if (!h) return RXC_HOST_E_ARG;
    if (h->opened) return RXC_HOST_E_STATE;
    h->verify = fn;
    h->verify_ctx = ctx;
    return RXC_HOST_OK;
}

int rxc_host_run(RxcHost *h, uint64_t task, uint64_t now_us, RxcHostResult *out) {
    if (!h || !out || task == 0) return RXC_HOST_E_ARG;
    memset(out, 0, sizeof *out);
    out->winner = out->winner_skill = RXC_HOST_NONE;
    out->task = task;
    if (h->n_sk == 0) return RXC_HOST_E_STATE;
    int rc = host_seal(h);
    if (rc != RXC_HOST_OK) return rc;
    SrRequirement q = host_requirement();
    RxcResult r;
    memset(&r, 0, sizeof r);
    int rr = rx_compose_run(h->c, task, &q, NULL, now_us, &r);
    out->run_rc = rr;
    refresh_info(h);
    if (rr != RX_OK) return RXC_HOST_E_RUN;
    out->outcome = r.outcome;
    out->committed = r.outcome == RXC_OUT_COMMITTED;
    out->n_branches = r.n_alternatives;
    out->branches_reclaimed = r.reclaimed;
    out->winner = r.winner;
    if (r.winner < r.n_alternatives && r.winner < RXC_K) {
        uint32_t id = r.route[r.winner].chosen.skill_id;
        if (id >= RXC_HOST_SKILL_ID0 && id - RXC_HOST_SKILL_ID0 < h->n_sk)
            out->winner_skill = id - RXC_HOST_SKILL_ID0;
        out->result = r.result;
    }
    for (uint32_t k = 0; k < RXC_K; k++) {
        out->cx_candidate[k] = r.cx_candidate[k];
        out->cx_admission[k] = r.cx_admission[k];
    }
    out->cx_evidence = r.cx_evidence;
    out->cx_promotion = r.cx_promotion;
    memcpy(out->winner_digest, r.winner_digest, 32);
    /* The AEGIS verdict as recorded: pass mask from the evidence payload,
     * goal record from the candidate claim's tag (rx_compose.h layout). */
    if (r.cx_evidence) {
        const CxObject *o = cx_get(&h->c->cx, r.cx_evidence);
        const uint64_t *p = o ? cx_payload(&h->c->cx, o) : NULL;
        if (p && o->n > RXC_EP_PASSMASK) out->aegis_pass_mask = (uint32_t)p[RXC_EP_PASSMASK];
        if (o) out->cx_goal = o->tag;
    } else if (r.cx_candidate[0]) {
        const CxObject *o = cx_get(&h->c->cx, r.cx_candidate[0]);
        if (o) out->cx_goal = o->tag;
    }
    rx_compose_record_digest(&h->c->cx, out->record_digest);
    return RXC_HOST_OK;
}

int rxc_host_recall(RxcHost *h, uint64_t subject, RxcHostRecord *out, uint32_t max,
                    uint32_t *n_filled, uint64_t *n_total) {
    if (n_filled) *n_filled = 0;
    if (n_total) *n_total = 0;
    if (!h || (max && !out)) return RXC_HOST_E_ARG;
    int rc = host_seal(h);
    if (rc != RXC_HOST_OK) return rc;
    if (subject >= h->c->cx.n_subjects) return RXC_HOST_E_ARG;
    uint64_t total = cx_count_subject(&h->c->cx, subject);
    if (n_total) *n_total = total;
    if (max == 0 || total == 0) return RXC_HOST_OK;
    CxRecord *tmp = calloc(max, sizeof *tmp);
    if (!tmp) return RXC_HOST_E_NOMEM;
    uint32_t n = cx_recall(&h->c->cx, subject, 0, UINT64_MAX, NULL, tmp, max);
    for (uint32_t i = 0; i < n; i++) fill_record(&out[i], &tmp[i]);
    free(tmp);
    if (n_filled) *n_filled = n;
    for (uint32_t i = 0; i < n; i++)
        if (!out[i].verified) return RXC_HOST_E_DIGEST;
    return RXC_HOST_OK;
}

int rxc_host_record(RxcHost *h, uint64_t id, RxcHostRecord *out) {
    if (!h || !out) return RXC_HOST_E_ARG;
    memset(out, 0, sizeof *out);
    int rc = host_seal(h);
    if (rc != RXC_HOST_OK) return rc;
    CxRecord x;
    int xr = cx_recall_id(&h->c->cx, id, &x);
    if (xr == CX_ERR_ARG) return RXC_HOST_E_NOT_FOUND;
    fill_record(out, &x);
    return xr == CX_OK && x.verified ? RXC_HOST_OK : RXC_HOST_E_DIGEST;
}

int rxc_host_payload(RxcHost *h, uint64_t id, uint64_t *out, uint32_t max) {
    if (!h || (max && !out)) return RXC_HOST_E_ARG;
    int rc = host_seal(h);
    if (rc != RXC_HOST_OK) return rc;
    const CxObject *o = cx_get(&h->c->cx, id);
    if (!o) return RXC_HOST_E_NOT_FOUND;
    const uint64_t *p = cx_payload(&h->c->cx, o);
    uint32_t n = o->n < max ? o->n : max;
    if (n && p) memcpy(out, p, (size_t)n * sizeof *out);
    return (int)o->n;
}

int rxc_host_info(RxcHost *h, RxcHostInfo *info) {
    if (!h || !info) return RXC_HOST_E_ARG;
    int rc = host_seal(h);
    refresh_info(h);
    *info = h->info;
    return rc;
}

void rxc_host_close(RxcHost *h) {
    if (!h) return;
    if (h->opened) rx_compose_close(h->c);
    if (h->admin) aienos_cap_stop(h->admin, h->view);
    if (h->cat_init) cq_catalog_free(&h->cat);
    pthread_mutex_lock(&g_mu);
    if (h->slot < RXC_HOST_MAX_HANDLES && g_slot[h->slot] == h) g_slot[h->slot] = NULL;
    pthread_mutex_unlock(&g_mu);
    free(h->c);
    free(h);
}

uint32_t rxc_host_abi_layout(uint32_t out[3]) {
    if (out) {
        out[0] = (uint32_t)sizeof(RxcHostInfo);
        out[1] = (uint32_t)sizeof(RxcHostResult);
        out[2] = (uint32_t)sizeof(RxcHostRecord);
    }
    return RXC_HOST_ABI_VERSION;
}
