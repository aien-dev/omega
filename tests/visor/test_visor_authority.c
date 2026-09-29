/*
 * test_visor_authority.c -- Omega Visor V1, lane 7 hostile authority tests.
 *
 * Invariant: THE VISOR CAN ASK. THE VISOR CANNOT GRANT.
 *
 * The Visor turns a canonical KIND_EFFECT object into an unauthorized
 * VisorEffectRequest. This test plays the hostile UI: it forges, replays,
 * edits and resubmits those requests and checks that the EXISTING authority
 * code refuses each one:
 *   rx_caproot_validate / rx_world_validate_cap   (capability root)
 *   rx_capadmin_mint                              (root's own mint checks)
 *   rx_aegis_evaluate                             (AEGIS policy)
 *   rx_gen_reject_replay (+ rx_gen_promote)       (generation barrier record)
 *   rx_world_publish_external                     (World publish check)
 *   omega_validate_object                         (Omega structure)
 * Every refusal is paired with a positive control (the honest request passes
 * the same call), so a refusal cannot come from a harness that refuses all.
 *
 * The harness itself is the authority side: it mints, promotes and publishes
 * with the runtime. Only the Visor objects are subject to the link check
 * (tests/visor/check_authority_link.sh).
 *
 * Test conventions (not existing runtime semantics; see spec/visor-authority.md):
 *   resource  = RES_BASE + EffectPayload.resource_class
 *   rights    = op 1 READ, op 2 WRITE, op 3 EFFECT, op 16 MINT (privileged)
 *   effect id for the generation record = first 8 bytes (LE) of request_digest
 */
#include "visor_effect_request.h"

#include "omega_canonical.h"
#include "omega_core.h"
#include "omega_validate.h"
#include "runtime/rx_aegis.h"
#include "runtime/rx_caproot.h"
#include "runtime/rx_generation.h"
#include "runtime/rx_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#define SUBJ_CLIENT    7u    /* the principal the Visor user would act as */
#define SUBJ_OTHER     9u
#define SUBJ_PROMOTER  4u
#define SUBJ_PROPOSER  8u
#define ISSUER         3u
#define RES_BASE       0x7100ull

enum { OPC_READ = 1, OPC_WRITE = 2, OPC_EFFECT = 3, OPC_MINT = 16 };

static int g_pass, g_total, g_nonclaims;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_total++;                                                              \
        if (cond) g_pass++;                                                     \
        else {                                                                  \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);                 \
            fprintf(stderr, __VA_ARGS__);                                       \
            fputc('\n', stderr);                                                \
        }                                                                       \
    } while (0)

static void case_header(const char *name) { printf("-- %s\n", name); }
static void non_claim(const char *text) {
    g_nonclaims++;
    printf("NON-CLAIM: %s\n", text);
}

/* ---- conventions ---- */

static uint32_t rights_for_op(uint16_t op) {
    switch (op) {
    case OPC_READ: return RX_RIGHT_READ;
    case OPC_WRITE: return RX_RIGHT_WRITE;
    case OPC_EFFECT: return RX_RIGHT_EFFECT;
    case OPC_MINT: return RX_RIGHT_MINT;
    default: return 0;
    }
}

static uint64_t resource_for(uint16_t cls) { return RES_BASE + cls; }

static uint64_t effect_work_id(const VisorEffectRequest *r) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->request_digest[i] << (8 * i);
    return v;
}

/* ---- the existing authority, as the outside consumer would call it ---- */

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    RxAegisPolicy policy;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = ISSUER;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(&e->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    int rc = rx_capadmin_mint(&e->admin, &m, &r);
    if (rc != RX_CAP_OK) fprintf(stderr, "harness mint failed: %s\n", rx_cap_strerror(rc));
    return r;
}

/* What the capability root says about the reference the request carries. */
static int root_check(Env *e, const VisorEffectRequest *r) {
    RxCapRef ref = { r->capability_slot, r->capability_generation };
    return rx_caproot_validate(&e->root, ref, SUBJ_CLIENT, resource_for(r->resource_class),
                               rights_for_op(r->operation_code), NULL);
}

static int world_check(Env *e, const VisorEffectRequest *r) {
    RxCapRef ref = { r->capability_slot, r->capability_generation };
    return rx_world_validate_cap(&e->w, ref, SUBJ_CLIENT, resource_for(r->resource_class),
                                 rights_for_op(r->operation_code), NULL);
}

static uint32_t aegis_check(Env *e, const VisorEffectRequest *r, uint32_t *why) {
    uint64_t lease = 0;
    uint32_t approval = 0;
    return rx_aegis_evaluate(&e->policy, SUBJ_CLIENT, resource_for(r->resource_class),
                             rights_for_op(r->operation_code), 0, &lease, why, &approval);
}

/* ---- graph helpers (the test builds canonical objects the honest way) ---- */

static OmegaObject *add_effect(OmegaGraph *g, uint16_t cls, uint16_t op, RxCapRef cap,
                               const char *params) {
    OmegaObject *o = omega_build_effect(g, cls, op, cap.cap_id, cap.generation);
    if (!o) return NULL;
    if (params) {
        EffectPayload eff;
        memcpy(&eff, o->payload, sizeof eff);
        eff.param_len = (uint16_t)strlen(params);
        memcpy(eff.param_bytes, params, eff.param_len);
        memcpy(o->payload, &eff, sizeof eff);
        omega_compute_semantic_id(o);
    }
    return o;
}

static void set_payload(OmegaObject *o, const EffectPayload *eff) {
    memcpy(o->payload, eff, sizeof *eff);
}

/* ---- generation store helpers (replay record) ---- */

typedef struct { Env *e; } GenAuth;

/* Linux-oracle promotion check: the capability root validates the promoter. */
static int gen_auth(void *ctx, uint32_t cap_id, uint32_t cap_generation, uint32_t subject,
                    uint64_t resource, uint32_t rights) {
    GenAuth *a = ctx;
    return rx_caproot_validate(&a->e->root, (RxCapRef){ cap_id, cap_generation }, subject,
                               resource, rights, NULL);
}

static int record_committed_effect(Env *e, const char *dir, uint64_t effect_id) {
    RxCapRef promo = mint(e, SUBJ_PROMOTER, RX_GEN_RES_PROMOTION, RX_RIGHT_PROMOTE);
    RxGenStore *store = NULL;
    if (rx_gen_open(dir, &store) != RX_GEN_OK) return -1;
    static const uint8_t evidence[] = "VISOR-EFFECT", model[] = "m", real[] = "r",
                         config[] = "c", prov[] = "visor-authority-test";
    RxGenObject obj;
    memset(&obj, 0, sizeof obj);
    obj.id = 1;
    obj.generation = 1;
    obj.digest[0] = 7;
    RxGenDraft d;
    memset(&d, 0, sizeof d);
    d.authority_epoch = 1;
    d.authority_generation = 1;
    d.proofs_ok = 1;
    d.objects = &obj;
    d.n_objects = 1;
    d.evidence = evidence; d.evidence_len = sizeof evidence - 1;
    d.model = model; d.model_len = sizeof model - 1;
    d.realization = real; d.realization_len = sizeof real - 1;
    d.config = config; d.config_len = sizeof config - 1;
    d.provenance = prov; d.provenance_len = sizeof prov - 1;
    uint64_t cand = 0;
    int rc = rx_gen_propose(store, SUBJ_PROPOSER, &d, &cand);
    if (rc == RX_GEN_OK) {
        RxGenWork work = { effect_id, RX_WORK_EXTERNAL, RX_WORK_ISSUED, 0 };
        rc = rx_gen_add_work(store, cand, &work);
    }
    if (rc == RX_GEN_OK) {
        RxPromotionRequest req;
        memset(&req, 0, sizeof req);
        req.candidate_id = cand;
        req.subject = SUBJ_PROMOTER;
        req.cap_id = promo.cap_id;
        req.cap_generation = promo.generation;
        req.resource = RX_GEN_RES_PROMOTION;
        req.rights = RX_GEN_RIGHT_PROMOTE;
        GenAuth a = { e };
        rc = rx_gen_promote(store, &req, gen_auth, &a, NULL, NULL, NULL, NULL);
    }
    rx_gen_close(store);
    return rc;
}

/* ---- compile-time facts about the request type ---- */

_Static_assert(sizeof(((VisorEffectRequest *)0)->authorized) == sizeof(bool),
               "authorized is a plain flag, not a handle or capability");
_Static_assert(sizeof(((VisorEffectRequest *)0)->request_digest) == 32,
               "request_digest is a sha256");
/* The adapter API has no setter / submitter / granter. These file-scope
 * definitions conflict with any declaration of the same name in
 * visor_effect_request.h, so adding one stops this test from compiling. */
static const char visor_effect_request_authorize = 0;
static const char visor_effect_request_grant = 0;
static const char visor_effect_request_submit = 0;
static const char visor_effect_request_set_authorized = 0;

int main(void) {
    Env *e = calloc(1, sizeof *e);
    OmegaGraph *g = calloc(1, sizeof *g);
    OmegaGraph *snap = calloc(1, sizeof *snap);
    if (!e || !g || !snap) { fprintf(stderr, "oom\n"); return 2; }
    (void)visor_effect_request_authorize;
    (void)visor_effect_request_grant;
    (void)visor_effect_request_submit;
    (void)visor_effect_request_set_authorized;
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) {
        fprintf(stderr, "capability root did not start\n");
        return 2;
    }
    if (rx_world_init(&e->w, &e->root, 1, 1u << 12) != RX_OK) {
        fprintf(stderr, "world did not start\n");
        rx_caproot_stop(&e->root, &e->admin);
        return 2;
    }
    e->w.external_subject = SUBJ_CLIENT;

    /* Policy: the client may hold READ|WRITE on resource class 1 only. */
    e->policy.n_rules = 1;
    e->policy.rules[0] = (RxAegisRule){ .id = 1, .subject = SUBJ_CLIENT,
                                        .res_lo = resource_for(1), .res_hi = resource_for(1),
                                        .rights = RX_RIGHT_READ | RX_RIGHT_WRITE };

    /* The honest grant the outside authority made for class 1 WRITE. */
    RxCapRef cap = mint(e, SUBJ_CLIENT, resource_for(1), RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef other = mint(e, SUBJ_OTHER, resource_for(2), RX_RIGHT_WRITE);

    OmegaObject *honest_obj = add_effect(g, 1, OPC_WRITE, cap, "set=42");
    SemanticId honest_id = honest_obj->id;
    VisorEffectRequest honest;
    memcpy(snap, g, sizeof *g);
    CHECK(visor_effect_request_build(g, &honest_id, &honest) == 0, "honest request builds");
    CHECK(memcmp(snap, g, sizeof *g) == 0, "build never mutates the graph");

    /* ================= 0. adapter contract + positive control ============ */
    case_header("0 adapter contract and positive control");
    CHECK(!honest.authorized, "request is unauthorized");
    CHECK(strcmp(honest.status, "UNAUTHORIZED_REQUEST") == 0, "status");
    CHECK(strcmp(honest.route, VISOR_EFFECT_ROUTE) == 0, "route");
    CHECK(memcmp(honest.request_digest, honest_id.bytes, 32) == 0,
          "digest equals the canonical SemanticId");
    CHECK(root_check(e, &honest) == RX_CAP_OK, "control: root accepts the honest reference");
    CHECK(world_check(e, &honest) == RX_CAP_OK, "control: world accepts the honest reference");
    uint32_t why = 0;
    CHECK(aegis_check(e, &honest, &why) == RX_AEGIS_GRANT, "control: AEGIS policy grants");
    char text[1024], json[1024];
    CHECK(visor_effect_request_format_text(&honest, text, sizeof text) == 0, "text");
    CHECK(strstr(text, "authorized: false") && strstr(text, "UNAUTHORIZED_REQUEST"), "text says unauthorized");
    CHECK(visor_effect_request_format_json(&honest, json, sizeof json) == 0, "json");
    CHECK(strstr(json, "\"authorized\":false") != NULL, "json says unauthorized");
    CHECK(visor_effect_request_format_json(&honest, json, 16) == -1, "json truncation refused");
    {
        bool need = false;
        memcpy(snap, g, sizeof *g);
        CHECK(visor_effect_request_classify(g, &honest_id, &need) == 0 && need,
              "effect object requires authority");
        CHECK(memcmp(snap, g, sizeof *g) == 0, "classify never mutates the graph");
        OmegaObject *t = omega_build_type_uint(g, 64);
        SemanticId tid = t->id;
        OmegaObject *op = omega_build_op_binary(g, OP_ADD, OVERFLOW_WRAP, &tid);
        SemanticId opid = op->id;
        OmegaObject *a = omega_build_val_uint(g, &tid, 64, 2);
        SemanticId aid = a->id;
        OmegaObject *b = omega_build_val_uint(g, &tid, 64, 3);
        SemanticId bid = b->id;
        OmegaObject *ap = omega_build_apply(g, &opid, &aid, &bid);
        SemanticId apid = ap->id;
        CHECK(visor_effect_request_classify(g, &apid, &need) == 0 && !need,
              "pure apply needs no authority");
        OmegaObject *cr = omega_build_type_cap_ref(g);
        SemanticId crid = cr->id;
        OmegaObject *holder = omega_build_val_uint(g, &crid, 64, 1);
        SemanticId hid = holder->id;
        CHECK(visor_effect_request_classify(g, &hid, &need) == 0 && need,
              "value typed CAPABILITY_REF requires authority");
        OmegaObject *rel = omega_build_val_uint(g, &tid, 64, 99);
        omega_object_add_relation(rel, REL_DEPENDS_ON, &honest_id);
        omega_compute_semantic_id(rel);
        SemanticId relid = rel->id;
        CHECK(visor_effect_request_classify(g, &relid, &need) == 0 && need,
              "effect reachable through a relation requires authority");
        SemanticId ghost;
        memset(&ghost, 0xAB, sizeof ghost);
        OmegaObject *dangling = omega_build_val_uint(g, &tid, 64, 100);
        omega_object_add_relation(dangling, REL_DEPENDS_ON, &ghost);
        omega_compute_semantic_id(dangling);
        SemanticId did = dangling->id;
        need = false;
        CHECK(visor_effect_request_classify(g, &did, &need) == -1 && need,
              "missing reference fails closed");
        CHECK(visor_effect_request_classify(g, &ghost, &need) == -1, "unknown id fails closed");
        CHECK(visor_effect_request_build(g, &apid, &(VisorEffectRequest){0}) == -1,
              "non-effect object refused");
    }

    /* ================= 1. forged capability ============================== */
    case_header("1 forged capability (random bytes / wrong slot)");
    {
        VisorEffectRequest f = honest;
        uint32_t rnd[2] = { 0, 0 };
        if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) rnd[0] = 0xDEADBEEFu;
        f.capability_slot = rnd[0] | 0x80000000u;
        f.capability_generation = rnd[1];
        f.authorized = true; /* hostile caller flips the flag; nothing reads it */
        CHECK(root_check(e, &f) == RX_CAP_ERR_BOUNDS, "random slot: root %d", root_check(e, &f));
        CHECK(world_check(e, &f) == RX_CAP_ERR_BOUNDS, "random slot: world");
        CHECK(visor_effect_request_format_json(&f, json, sizeof json) == 0 &&
              strstr(json, "\"authorized\":false"), "flipped flag still renders unauthorized");
        f = honest;
        f.capability_generation ^= 0x5A5A5A5Au;
        CHECK(root_check(e, &f) == RX_CAP_ERR_STALE_GEN, "random generation: root");
        f = honest;
        f.capability_slot = other.cap_id;             /* someone else's live cap */
        f.capability_generation = other.generation;
        CHECK(root_check(e, &f) == RX_CAP_ERR_SUBJECT, "wrong slot: root %d", root_check(e, &f));
        CHECK(world_check(e, &f) == RX_CAP_ERR_SUBJECT, "wrong slot: world");
    }

    /* ================= 5. malformed request ============================== */
    case_header("5 malformed effect request (bad payload_len / bad resource)");
    {
        OmegaObject *m = add_effect(g, 1, OPC_WRITE, cap, NULL);
        SemanticId mid = m->id;
        char err[160];
        uint32_t keep = m->payload_len;
        m->payload_len = 8;                                   /* truncated */
        VisorEffectRequest out;
        CHECK(omega_validate_object(g, m, err, sizeof err) != 0, "validate rejects short payload");
        CHECK(visor_effect_request_build(g, &mid, &out) == -1 && !out.authorized, "build refuses short payload");
        m->payload_len = keep + 16;                           /* oversized */
        CHECK(visor_effect_request_build(g, &mid, &out) == -1, "build refuses oversized payload");
        m->payload_len = keep;
        EffectPayload eff;
        memcpy(&eff, m->payload, sizeof eff);
        eff.param_len = 500;                                  /* > 128 */
        set_payload(m, &eff);
        omega_compute_semantic_id(m);
        mid = m->id;
        CHECK(visor_effect_request_build(g, &mid, &out) == -1, "build refuses param_len > 128");
        eff.param_len = 0;
        eff.resource_class = 0;                               /* invalid resource */
        set_payload(m, &eff);
        omega_compute_semantic_id(m);
        mid = m->id;
        CHECK(omega_validate_object(g, m, err, sizeof err) != 0, "validate rejects resource 0");
        CHECK(visor_effect_request_build(g, &mid, &out) == -1, "build refuses resource 0");
    }

    /* ================= 6. request exceeding rights ======================= */
    case_header("6 request exceeding rights (operation outside the grant)");
    {
        OmegaObject *x = add_effect(g, 1, OPC_EFFECT, cap, "set=42");
        SemanticId xid = x->id;
        VisorEffectRequest r;
        CHECK(visor_effect_request_build(g, &xid, &r) == 0, "build (a question is allowed)");
        CHECK(root_check(e, &r) == RX_CAP_ERR_RIGHTS, "root: EFFECT not in grant");
        CHECK(aegis_check(e, &r, &why) == RX_AEGIS_DENY && why == RX_AEGIS_WHY_NO_RULE,
              "AEGIS: no rule covers EFFECT (why=%u)", why);
        OmegaObject *y = add_effect(g, 1, OPC_WRITE, cap, "set=42");
        EffectPayload eff;
        memcpy(&eff, y->payload, sizeof eff);
        eff.resource_class = 2;                               /* outside the client's domain */
        set_payload(y, &eff);
        omega_compute_semantic_id(y);
        SemanticId yid = y->id;
        CHECK(visor_effect_request_build(g, &yid, &r) == 0, "build class 2");
        CHECK(aegis_check(e, &r, &why) == RX_AEGIS_DENY && why == RX_AEGIS_WHY_NO_RULE,
              "AEGIS: resource outside policy");
    }

    /* ================= 4. UI attempts a direct privileged call =========== */
    case_header("4 UI attempts direct privileged call");
    {
        OmegaObject *p = add_effect(g, 1, OPC_MINT, cap, NULL);
        SemanticId pid = p->id;
        VisorEffectRequest r;
        CHECK(visor_effect_request_build(g, &pid, &r) == 0, "build privileged question");
        CHECK(!r.authorized, "still unauthorized");
        CHECK(aegis_check(e, &r, &why) == RX_AEGIS_DENY && why == RX_AEGIS_WHY_PRIVILEGED,
              "AEGIS: privileged right refused (why=%u)", why);
        CHECK(root_check(e, &r) == RX_CAP_ERR_RIGHTS, "root: client cap has no MINT");
        /* The UI holds only its own reference. Presenting it as mint authority: */
        RxCapMint m;
        memset(&m, 0, sizeof m);
        m.issuer = ISSUER;
        m.subject = SUBJ_CLIENT;
        m.resource = resource_for(1);
        m.rights = RX_RIGHT_EFFECT;
        m.parent = (RxCapRef){ UINT32_MAX, 0 };
        m.authority = cap;
        RxCapRef got;
        int rc = rx_capadmin_mint(&e->admin, &m, &got);
        CHECK(rc == RX_CAP_ERR_UNAUTHORIZED, "root refuses mint under a non-office authority (%d)", rc);
        /* A forged admin handle (right channel, guessed token). */
        RxCapAdmin forged = e->admin;
        if (getrandom(forged.token, sizeof forged.token, 0) != (ssize_t)sizeof forged.token)
            memset(forged.token, 0x42, sizeof forged.token);
        m.authority = rx_capadmin_office(&e->admin);
        rc = rx_capadmin_mint(&forged, &m, &got);
        CHECK(rc == RX_CAP_ERR_UNAUTHORIZED, "root refuses mint with a forged token (%d)", rc);
        /* No handle at all. */
        RxCapAdmin none;
        memset(&none, 0, sizeof none);
        none.ctl_fd = -1;
        CHECK(rx_capadmin_mint(&none, &m, &got) == RX_CAP_ERR_IO, "no admin channel, no mint");
        printf("   link isolation is checked separately: make visor-authority-check\n");
    }

    /* ================= 7/8. resource or arguments changed after authorization */
    case_header("7 resource changed after authorization");
    {
        /* The authorized request is `honest` (digest D). The object is edited in place. */
        OmegaObject *o = omega_graph_find_object(g, &honest_id);
        EffectPayload saved;
        memcpy(&saved, o->payload, sizeof saved);
        EffectPayload eff = saved;
        eff.resource_class = 2;
        set_payload(o, &eff);
        VisorEffectRequest r;
        CHECK(visor_effect_request_build(g, &honest_id, &r) == -1,
              "edited without re-id: digest != SemanticId, refused");
        /* The attacker re-identifies the object: it is now a different object. */
        omega_compute_semantic_id(o);
        SemanticId nid = o->id;
        CHECK(visor_effect_request_build(g, &nid, &r) == 0, "re-identified object builds");
        CHECK(memcmp(r.request_digest, honest.request_digest, 32) != 0,
              "digest mismatch with the authorized request");
        CHECK(root_check(e, &r) == RX_CAP_ERR_RESOURCE, "root: cap is bound to the old resource");
        CHECK(world_check(e, &r) == RX_CAP_ERR_RESOURCE, "world: same");
        set_payload(o, &saved);
        omega_compute_semantic_id(o);
        CHECK(omega_compare_semantic_id(&o->id, &honest_id) == 0, "restored");
    }
    case_header("8 arguments changed after authorization");
    {
        OmegaObject *o = omega_graph_find_object(g, &honest_id);
        EffectPayload saved;
        memcpy(&saved, o->payload, sizeof saved);
        EffectPayload eff = saved;
        eff.param_bytes[4] = '9';                             /* set=42 -> set=92 */
        set_payload(o, &eff);
        VisorEffectRequest r;
        CHECK(visor_effect_request_build(g, &honest_id, &r) == -1,
              "edited params without re-id: refused");
        omega_compute_semantic_id(o);
        SemanticId nid = o->id;
        CHECK(visor_effect_request_build(g, &nid, &r) == 0 &&
              memcmp(r.request_digest, honest.request_digest, 32) != 0,
              "re-identified params: digest mismatch with the authorized request");
        CHECK(root_check(e, &r) == RX_CAP_OK,
              "observed: params-only change still validates at the root (basis of the non-claim)");
        non_claim("the capability root binds (subject, resource, rights, generation), not effect "
                  "parameters: a params-only change still validates at rx_caproot_validate; only the "
                  "request digest differs. No existing runtime call compares effect digests.");
        eff = saved;
        eff.operation_code = OPC_EFFECT;                      /* operation is an argument too */
        set_payload(o, &eff);
        omega_compute_semantic_id(o);
        nid = o->id;
        CHECK(visor_effect_request_build(g, &nid, &r) == 0 && root_check(e, &r) == RX_CAP_ERR_RIGHTS,
              "operation changed: root refuses rights");
        set_payload(o, &saved);
        omega_compute_semantic_id(o);
    }

    /* ================= 2/3/9. stale generation, replay, duplicate ========= */
    char dir[] = "/tmp/visor-authority-XXXXXX";
    int have_dir = mkdtemp(dir) != NULL;
    CHECK(have_dir, "temp dir");
    case_header("9 duplicate submission");
    {
        uint64_t id = effect_work_id(&honest);
        int rc = have_dir ? record_committed_effect(e, dir, id) : -1;
        CHECK(rc == RX_GEN_OK, "harness: first submission committed as external effect (%d)", rc);
        CHECK(rx_gen_reject_replay(dir, id) == RX_GEN_ERR_REPLAY, "duplicate refused by the generation record");
        VisorEffectRequest fresh;
        OmegaObject *n = add_effect(g, 1, OPC_WRITE, cap, "set=43");
        SemanticId nid = n->id;
        CHECK(visor_effect_request_build(g, &nid, &fresh) == 0 &&
              rx_gen_reject_replay(dir, effect_work_id(&fresh)) == RX_GEN_OK,
              "control: a different request is not a replay");
        non_claim("the World does not deduplicate external stimuli; rx_world_publish_external accepts "
                  "the same mutation twice under a valid cap. Duplicate refusal exists only in the "
                  "generation barrier record (rx_gen_reject_replay) of committed external effects.");
    }
    case_header("2 stale generation");
    {
        RxCapRef office = rx_capadmin_office(&e->admin);
        CHECK(rx_capadmin_revoke(&e->admin, office, cap) == RX_CAP_OK, "harness revokes");
        CHECK(root_check(e, &honest) == RX_CAP_ERR_REVOKED, "revoked: root");
        CHECK(rx_capadmin_reclaim(&e->admin, office, cap.cap_id) == RX_CAP_OK, "harness reclaims (generation advances)");
        CHECK(root_check(e, &honest) == RX_CAP_ERR_STALE_GEN, "stale generation: root %d", root_check(e, &honest));
        CHECK(world_check(e, &honest) == RX_CAP_ERR_STALE_GEN, "stale generation: world");
    }
    case_header("3 replayed authorization after generation advance");
    {
        /* The authority grants again; the slot may be reused at a newer generation. */
        RxCapRef again = mint(e, SUBJ_CLIENT, resource_for(1), RX_RIGHT_READ | RX_RIGHT_WRITE);
        CHECK(again.cap_id != UINT32_MAX, "harness re-grants");
        CHECK(root_check(e, &honest) == RX_CAP_ERR_STALE_GEN,
              "old request replayed against the new grant: root refuses");
        CHECK(rx_gen_reject_replay(dir, effect_work_id(&honest)) == RX_GEN_ERR_REPLAY,
              "old request replayed: generation record refuses");
        OmegaObject *n = add_effect(g, 1, OPC_WRITE, again, "set=42");
        SemanticId nid = n->id;
        VisorEffectRequest r;
        CHECK(visor_effect_request_build(g, &nid, &r) == 0 && root_check(e, &r) == RX_CAP_OK,
              "control: a request naming the new grant validates");
    }

    /* ================= 10. bypass the publish gate ======================= */
    case_header("10 bypass publish gate (publish without a valid cap)");
    {
        RxObjRef obj = { UINT32_MAX, 0 };
        uint64_t init[RX_MAX_FIELDS] = { 0 };
        CHECK(rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, resource_for(1), init, &obj) == RX_OK,
              "harness creates the target object");
        RxMutation mu = { obj, 0, 42 };
        RxCapRef forged = { honest.capability_slot, honest.capability_generation };  /* stale */
        CHECK(rx_world_publish_external(&e->w, forged, &mu, 1) == RX_ERR_AUTHORITY, "stale ref: refused");
        RxCapRef junk = { 0x7FFFFFFFu, 1 };
        CHECK(rx_world_publish_external(&e->w, junk, &mu, 1) == RX_ERR_AUTHORITY, "junk ref: refused");
        RxCapRef ro = mint(e, SUBJ_CLIENT, resource_for(1), RX_RIGHT_READ);
        CHECK(rx_world_publish_external(&e->w, ro, &mu, 1) == RX_ERR_AUTHORITY, "read-only cap: refused");
        RxCapRef theirs = mint(e, SUBJ_OTHER, resource_for(1), RX_RIGHT_WRITE);
        CHECK(rx_world_publish_external(&e->w, theirs, &mu, 1) == RX_ERR_AUTHORITY, "other subject's cap: refused");
        RxObjRef gone = obj;
        gone.generation ^= 1u;
        RxCapRef rw = mint(e, SUBJ_CLIENT, resource_for(1), RX_RIGHT_WRITE);
        RxMutation stale_obj = { gone, 0, 42 };
        CHECK(rx_world_publish_external(&e->w, rw, &stale_obj, 1) == RX_ERR_STALE_GEN, "stale object: refused");
        CHECK(rx_world_publish_external(&e->w, rw, &mu, 1) >= 0, "control: valid WRITE cap publishes");
        CHECK(rx_world_publish_external(&e->w, rw, &mu, 1) >= 0,
              "observed: the World accepts the same stimulus twice (basis of the duplicate non-claim)");
        non_claim("the typed-result publish gate (rc_gate_register, resident reaction) is not driven "
                  "here; only the World's external publish check is exercised.");
    }

    /* ================= 11. bypass AEGIS ================================== */
    case_header("11 bypass AEGIS (submit a reference nobody granted)");
    {
        /* The Visor has no path to AEGIS or the root. Whatever reference it
         * copies from an object, only a reference the root minted validates. */
        OmegaObject *n = add_effect(g, 1, OPC_WRITE, (RxCapRef){ RX_CAP_MAX - 1u, 1 }, "set=1");
        SemanticId nid = n->id;
        VisorEffectRequest r;
        CHECK(visor_effect_request_build(g, &nid, &r) == 0, "build ungranted reference");
        int rc = root_check(e, &r);
        CHECK(rc != RX_CAP_OK, "root refuses an ungranted slot (%s)", rx_cap_strerror(rc));
        CHECK(world_check(e, &r) == rc, "world agrees");
        RxAegisPolicy empty;
        memset(&empty, 0, sizeof empty);
        uint64_t lease;
        uint32_t appr;
        CHECK(rx_aegis_evaluate(&empty, SUBJ_CLIENT, resource_for(1), RX_RIGHT_WRITE, 0, &lease,
                                &why, &appr) == RX_AEGIS_DENY, "no policy, no grant");
        non_claim("AEGIS is not invoked on a request path (rx_aegis.h: nothing is called); "
                  "'bypass' is refused because only root.install mints and the root validates every "
                  "presented reference. The resident aegis.decide/root.install reactions are not run here.");
    }

    if (have_dir) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
    }
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
    free(snap);
    free(g);
    free(e);

    printf("non-claims: %d\n", g_nonclaims);
    printf("PASS %d/%d\n", g_pass, g_total);
    if (g_pass == g_total) {
        printf("OMEGA_VISOR_AUTHORITY_HOSTILE_PASS\n");
        return 0;
    }
    return 1;
}
