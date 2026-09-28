/*
 * rx_contract.c -- Omega typed result constraints. See rx_contract.h.
 */
#include "rx_contract.h"
#include "rx_aien.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A compiled domain's explicit set must hold every value any source can
 * offer; a truncated set would mask valid answers away. */
_Static_assert(RC_MAX_SET >= RX_MAX_OBJECTS && RC_MAX_SET >= RC_MAX_EVIDENCE &&
               RC_MAX_SET >= AG_MAX_SKILLS && RC_MAX_SET >= AG_MAX_CAPS &&
               RC_MAX_SET >= RC_MAX_EFFECT_RES + 2,
               "domain sets must not truncate");

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- context ---- */

void rc_context_init(RcContext *ctx, RxWorld *w, uint32_t principal, const AgCapTable *caps) {
    memset(ctx, 0, sizeof *ctx);
    ctx->w = w;
    ctx->principal = principal;
    ctx->caps = caps;
    ctx->next_seq = 1;
    ctx->max_effects = UINT32_MAX;
    ctx->max_graph_nodes = AG_MAX_NODES;
    pthread_mutex_init(&ctx->mu, NULL);
}

void rc_context_destroy(RcContext *ctx) { pthread_mutex_destroy(&ctx->mu); }

int rc_context_add_evidence(RcContext *ctx, uint32_t subject_kind, uint64_t subject,
                            uint32_t verdict) {
    pthread_mutex_lock(&ctx->mu);
    int rc = -1;
    if (ctx->n_evidence < RC_MAX_EVIDENCE) {
        ctx->evidence[ctx->n_evidence++] = (RcEvidenceRecord){ subject_kind, subject, verdict };
        rc = 0;
    }
    pthread_mutex_unlock(&ctx->mu);
    return rc;
}

static int evidence_pass(const RcContext *ctx, uint32_t kind, uint64_t subject) {
    RcContext *m = (RcContext *)ctx;
    pthread_mutex_lock(&m->mu);
    int found = 0;
    for (uint32_t i = 0; i < ctx->n_evidence && !found; i++)
        found = ctx->evidence[i].subject_kind == kind && ctx->evidence[i].subject == subject &&
                ctx->evidence[i].verdict == RC_VERDICT_PASS;
    pthread_mutex_unlock(&m->mu);
    return found;
}

static uint32_t effects_left(const RcContext *ctx) {
    RcContext *m = (RcContext *)ctx;
    pthread_mutex_lock(&m->mu);
    uint32_t left = ctx->effects_used >= ctx->max_effects ? 0 : ctx->max_effects - ctx->effects_used;
    pthread_mutex_unlock(&m->mu);
    return left;
}

/* Current object in slot id (any generation), under the world lock. */
static int obj_current(RxWorld *w, uint64_t id, RxObject *out) {
    if (!w || id >= RX_MAX_OBJECTS) return 0;
    pthread_mutex_lock(&w->mu);
    *out = w->objects[id];
    pthread_mutex_unlock(&w->mu);
    return out->live;
}

/* ---- contracts ---- */

#define EV(v) (1ull << (v))

static void field(RcContract *c, const char *name, RcType t, int derived) {
    c->field[c->n_fields].name = name;
    c->field[c->n_fields].type = t;
    c->field[c->n_fields].derived = (uint8_t)derived;
    c->n_fields++;
}

static RcRule *rule(RcContract *c, uint8_t kind, uint8_t op, uint8_t a) {
    RcRule *r = &c->rule[c->n_rules++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->op = op;
    r->a = a;
    r->b = RC_NONE;
    r->g = RC_NONE;
    return r;
}

static void range(RcContract *c, uint8_t a, uint64_t lo, uint64_t hi) {
    RcRule *r = rule(c, RC_RULE_RANGE, 0, a);
    r->k = lo;
    r->k2 = hi;
}

static void enumr(RcContract *c, uint8_t a, uint64_t set) { rule(c, RC_RULE_ENUM, 0, a)->k = set; }

static void cross(RcContract *c, uint8_t a, uint8_t cmp, uint8_t b, uint64_t k, uint8_t g,
                  uint8_t gmode, uint64_t kg) {
    RcRule *r = rule(c, RC_RULE_CROSS, 0, a);
    r->cmp = cmp;
    r->b = b;
    r->k = k;
    r->g = g;
    r->gmode = gmode;
    r->kg = kg;
}

static void order(RcContract *c, const uint8_t *o) { memcpy(c->order, o, c->n_fields); }

const char *rc_kind_name(RcKind k) {
    static const char *n[] = { "none", "plan", "hypothesis", "capability_need", "action_graph",
                               "skill_invocation", "world_mutation", "effect_proposal",
                               "generation_candidate", "evidence" };
    return (unsigned)k < RC_KIND_COUNT ? n[k] : "?";
}

const char *rc_rule_kind_name(RcRuleKind k) {
    static const char *n[] = { "type", "range", "enum", "structural", "cross_field", "authority",
                               "generation", "referential", "required_evidence", "effect" };
    return (unsigned)k < RC_RULE_KIND_COUNT ? n[k] : "?";
}

void rc_contract_identify(RcContract *c) {
    sha256_ctx h;
    sha256_init(&h);
    uint8_t hdr[12];
    uint32_t v[3] = { (uint32_t)c->kind, c->n_fields, c->n_rules };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) hdr[i * 4 + j] = (uint8_t)(v[i] >> (8 * j));
    sha256_update(&h, hdr, sizeof hdr);
    for (uint32_t i = 0; i < c->n_fields; i++) {
        uint8_t f[3] = { (uint8_t)c->field[i].type, c->field[i].derived, c->order[i] };
        sha256_update(&h, f, 3);
    }
    for (uint32_t i = 0; i < c->n_rules; i++) {
        const RcRule *r = &c->rule[i];
        uint8_t b[8 + 24] = { r->kind, r->op, r->a, r->b, r->g, r->gmode, r->cmp, r->repair };
        uint64_t k[3] = { r->k, r->k2, r->kg };
        for (int x = 0; x < 3; x++)
            for (int j = 0; j < 8; j++) b[8 + x * 8 + j] = (uint8_t)(k[x] >> (8 * j));
        sha256_update(&h, b, sizeof b);
    }
    uint8_t wt[4];
    for (int j = 0; j < 4; j++) wt[j] = (uint8_t)(c->world_type >> (8 * j));
    sha256_update(&h, wt, 4);
    sha256_final(&h, c->identity);
}

int rc_contract_std(RcKind kind, RcContract *c) {
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->name = rc_kind_name(kind);
    c->world_type = RC_OT_PUBLISH + (uint32_t)kind;
    switch (kind) {
    case RC_PLAN: {
        /* rx_aien plan layout: 0 seq, 1 action, 2 regime, 3 condition,
         * 4 hypothesis seq, 5 reason, 6 goal seq. */
        c->world_type = RX_OT_PLAN;
        field(c, "seq", RC_T_U64, 1);
        field(c, "action", RC_T_ENUM, 0);
        field(c, "regime", RC_T_U64, 0);
        field(c, "condition", RC_T_U32, 0);
        field(c, "hypothesis", RC_T_U64, 0);
        field(c, "reason", RC_T_ENUM, 0);
        field(c, "goal", RC_T_U64, 0);
        field(c, "reserved", RC_T_U64, 0);
        order(c, (const uint8_t[]){ 1, 5, 2, 3, 4, 6, 0, 7 });
        enumr(c, 1, EV(RX_AIEN_ACT_RESEARCH));
        enumr(c, 5, EV(RX_AIEN_WHY_CORE_CLASS) | EV(RX_AIEN_WHY_DRIFT) | EV(RX_AIEN_WHY_GOAL));
        range(c, 2, (1ull << 32) | 1u, (0xfffffull << 32) | 0xfffffu);
        range(c, 3, 1, 0xfff);
        RcRule *r = rule(c, RC_RULE_REFERENCE, RC_R_CTX_FIELD, 4);
        r->k = 0; r->k2 = 0; r->b = 1;                   /* the hypothesis object, or none */
        cross(c, 4, RC_NE, RC_NONE, 0, 5, RC_GUARD_IN,
              EV(RX_AIEN_WHY_CORE_CLASS) | EV(RX_AIEN_WHY_DRIFT));
        r = rule(c, RC_RULE_REFERENCE, RC_R_CTX_FIELD, 6);
        r->k = 1; r->k2 = 0; r->b = 1;                   /* the goal object, or none */
        cross(c, 6, RC_NE, RC_NONE, 0, 5, RC_GUARD_IN, EV(RX_AIEN_WHY_GOAL));
        rule(c, RC_RULE_REFERENCE, RC_R_NEXT_SEQ, 0)->repair = RC_FIX_DERIVE;
        range(c, 7, 0, 0);
        break;
    }
    case RC_HYPOTHESIS: {
        /* rx_aien hypothesis layout: 0 seq, 1 kind, 2 prediction seq,
         * 3 class then, 4 class now, 5 expected, 6 observed, 7 state. */
        c->world_type = RX_OT_HYPOTHESIS;
        field(c, "seq", RC_T_U64, 1);
        field(c, "hypothesis_kind", RC_T_ENUM, 0);
        field(c, "prediction", RC_T_U64, 0);
        field(c, "class_then", RC_T_U32, 0);
        field(c, "class_now", RC_T_U32, 0);
        field(c, "expected_ns", RC_T_U64, 0);
        field(c, "observed_ns", RC_T_U64, 0);
        field(c, "state", RC_T_ENUM, 0);
        order(c, (const uint8_t[]){ 1, 7, 2, 3, 4, 5, 6, 0 });
        enumr(c, 1, EV(RX_AIEN_HYP_CORE_CLASS) | EV(RX_AIEN_HYP_DRIFT));
        enumr(c, 7, EV(RX_AIEN_HYP_OPEN) | EV(RX_AIEN_HYP_TESTING) | EV(RX_AIEN_HYP_SUPPORTED) |
                        EV(RX_AIEN_HYP_UNSUPPORTED) | EV(RX_AIEN_HYP_EXHAUSTED));
        RcRule *r = rule(c, RC_RULE_REFERENCE, RC_R_CTX_FIELD, 2);
        r->k = 2; r->k2 = 0;                             /* the prediction that failed */
        range(c, 3, 1, 0xfff);
        range(c, 4, 1, 0xfff);
        r = rule(c, RC_RULE_REFERENCE, RC_R_CTX_FIELD, 4);
        r->k = 3; r->k2 = 1;                             /* where the body runs now */
        cross(c, 4, RC_NE, 3, 0, 1, RC_GUARD_IN, EV(RX_AIEN_HYP_CORE_CLASS));
        cross(c, 4, RC_EQ, 3, 0, 1, RC_GUARD_IN, EV(RX_AIEN_HYP_DRIFT));
        range(c, 5, 1, 1000000000000ull);
        cross(c, 6, RC_NE, 5, 0, RC_NONE, RC_ALWAYS, 0);  /* a failure differs from expectation */
        rule(c, RC_RULE_STRUCT, RC_S_NONZERO, 6);
        rule(c, RC_RULE_REFERENCE, RC_R_NEXT_SEQ, 0)->repair = RC_FIX_DERIVE;
        break;
    }
    case RC_CAPABILITY_NEED: {
        /* What AIEN needs done, not who does it (the capability query's
         * CapabilityNeed): the operation, the most authority it will use and
         * on what, the effect classes it accepts, where it may run, and its
         * latency budget. */
        field(c, "principal", RC_T_SUBJECT, 0);
        field(c, "operation", RC_T_U32, 0);
        field(c, "resource", RC_T_RESOURCE, 0);
        field(c, "rights_ceiling", RC_T_BITS, 0);
        field(c, "effects", RC_T_BITS, 0);
        field(c, "locality", RC_T_BITS, 0);
        field(c, "latency_us", RC_T_U64, 0);
        field(c, "human_approval", RC_T_BOOL, 0);
        order(c, (const uint8_t[]){ 0, 1, 2, 3, 4, 5, 6, 7 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 0);
        range(c, 1, 1, 1u << 20);
        rule(c, RC_RULE_REFERENCE, RC_R_RESOURCE_LIVE, 2);
        rule(c, RC_RULE_EFFECT, RC_E_FORBIDDEN, 2);
        RcRule *r = rule(c, RC_RULE_STRUCT, RC_S_BITS_WITHIN, 3);
        r->k = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT | RX_RIGHT_DELEGATE;
        r->repair = RC_FIX_NARROW;
        r = rule(c, RC_RULE_AUTHORITY, RC_A_NO_PRIVILEGED, 3);
        r->k = RX_RIGHT_PRIVILEGED;
        r->repair = RC_FIX_NARROW;
        rule(c, RC_RULE_STRUCT, RC_S_NONZERO, 3);
        rule(c, RC_RULE_STRUCT, RC_S_BITS_WITHIN, 4)->k = RC_EFFECT_CLASSES;
        /* A ceiling without the effect right accepts no effect class. */
        uint64_t no_effect = 0;
        for (uint64_t v = 0; v < 16; v++)
            if (!(v & RX_RIGHT_EFFECT)) no_effect |= EV(v);
        cross(c, 4, RC_EQ, RC_NONE, 0, 3, RC_GUARD_IN, no_effect);
        rule(c, RC_RULE_STRUCT, RC_S_BITS_WITHIN, 5)->k = RC_LOCALITIES;
        rule(c, RC_RULE_STRUCT, RC_S_NONZERO, 5);
        range(c, 6, 1, 60000000);
        cross(c, 7, RC_EQ, RC_NONE, 1, 3, RC_GUARD_BITS, RX_RIGHT_EFFECT);
        break;
    }
    case RC_ACTION_GRAPH: {
        field(c, "digest0", RC_T_DIGEST, 1);
        field(c, "digest1", RC_T_DIGEST, 1);
        field(c, "subject", RC_T_SUBJECT, 0);
        field(c, "nodes", RC_T_U32, 1);
        field(c, "effects", RC_T_U32, 1);
        field(c, "goal_kind", RC_T_U32, 0);
        field(c, "evidence", RC_T_U32, 1);
        field(c, "reserved", RC_T_U64, 0);
        order(c, (const uint8_t[]){ 2, 5, 0, 1, 3, 4, 6, 7 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 2);
        range(c, 5, 1, 16);
        rule(c, RC_RULE_STRUCT, RC_S_ACTION_GRAPH, 0);
        rule(c, RC_RULE_STRUCT, RC_S_GRAPH_FIELDS, 0)->repair = RC_FIX_DERIVE;
        rule(c, RC_RULE_EFFECT, RC_E_GRAPH_CLEAR, 0);
        rule(c, RC_RULE_EFFECT, RC_E_MAX, 4);
        range(c, 7, 0, 0);
        break;
    }
    case RC_SKILL_INVOCATION: {
        field(c, "skill", RC_T_U32, 0);
        field(c, "arity", RC_T_U32, 0);
        field(c, "arg0", RC_T_U64, 0);
        field(c, "arg1", RC_T_U64, 0);
        field(c, "arg2", RC_T_U64, 0);
        field(c, "max_attempts", RC_T_U32, 0);
        field(c, "principal", RC_T_SUBJECT, 0);
        order(c, (const uint8_t[]){ 6, 0, 1, 2, 3, 4, 5 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 6);
        rule(c, RC_RULE_REFERENCE, RC_R_SKILL, 0);
        rule(c, RC_RULE_REFERENCE, RC_R_ARITY, 1)->b = 0;
        for (uint8_t i = 2; i <= 4; i++) range(c, i, 0, 1ull << 40);
        cross(c, 2, RC_EQ, RC_NONE, 0, 1, RC_GUARD_IN, EV(0));
        cross(c, 3, RC_EQ, RC_NONE, 0, 1, RC_GUARD_IN, EV(0) | EV(1));
        cross(c, 4, RC_EQ, RC_NONE, 0, 1, RC_GUARD_IN, EV(0) | EV(1) | EV(2));
        range(c, 5, 1, 8);
        break;
    }
    case RC_WORLD_MUTATION: {
        field(c, "object", RC_T_OBJ, 0);
        field(c, "generation", RC_T_U32, 0);
        field(c, "field", RC_T_U32, 0);
        field(c, "value", RC_T_U64, 0);
        field(c, "principal", RC_T_SUBJECT, 0);
        field(c, "resource", RC_T_RESOURCE, 1);
        /* The object carries the meaning; its resource is derived from it and
         * comes after, so a slip in the resource cannot steer the object. */
        order(c, (const uint8_t[]){ 4, 0, 1, 5, 2, 3 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 4);
        RcRule *r = rule(c, RC_RULE_AUTHORITY, RC_A_HOLDS, 5);
        r->k = RX_RIGHT_WRITE;
        rule(c, RC_RULE_EFFECT, RC_E_FORBIDDEN, 5);
        r = rule(c, RC_RULE_REFERENCE, RC_R_RESOURCE_OF, 5);
        r->b = 0;
        r->repair = RC_FIX_DERIVE;
        rule(c, RC_RULE_REFERENCE, RC_R_OBJECT, 0);
        rule(c, RC_RULE_GENERATION, RC_G_LIVE, 0)->b = 1;
        range(c, 2, 0, RX_MAX_FIELDS - 1);
        break;
    }
    case RC_EFFECT_PROPOSAL: {
        field(c, "resource", RC_T_RESOURCE, 0);
        field(c, "value", RC_T_U64, 0);
        field(c, "verdict", RC_T_U64, 0);
        field(c, "irreversible", RC_T_BOOL, 0);
        field(c, "approval", RC_T_U64, 0);
        field(c, "principal", RC_T_SUBJECT, 0);
        field(c, "effect_kind", RC_T_ENUM, 0);
        order(c, (const uint8_t[]){ 5, 0, 6, 3, 4, 1, 2 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 5);
        rule(c, RC_RULE_EFFECT, RC_E_BUDGET, 5);
        rule(c, RC_RULE_EFFECT, RC_E_ALLOWED, 0);
        RcRule *r = rule(c, RC_RULE_AUTHORITY, RC_A_HOLDS, 0);
        r->k = RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
        enumr(c, 6, EV(1) | EV(2) | EV(3));
        cross(c, 3, RC_EQ, RC_NONE, 1, 6, RC_GUARD_IN, EV(3));   /* delete is irreversible */
        r = rule(c, RC_RULE_EFFECT, RC_E_APPROVAL, 4);
        r->g = 3; r->gmode = RC_GUARD_IN; r->kg = EV(1);        /* irreversible needs an approval */
        rule(c, RC_RULE_EVIDENCE, 0, 2)->k = RC_EV_VERIFY;
        break;
    }
    case RC_GENERATION_CANDIDATE: {
        field(c, "candidate", RC_T_U64, 0);
        field(c, "parent", RC_T_U64, 0);
        field(c, "authority_epoch", RC_T_U64, 0);
        field(c, "evidence", RC_T_U64, 0);
        field(c, "proofs_ok", RC_T_BOOL, 0);
        field(c, "objects", RC_T_U32, 0);
        field(c, "proposer", RC_T_SUBJECT, 0);
        field(c, "lineage", RC_T_U64, 0);
        order(c, (const uint8_t[]){ 6, 1, 0, 2, 7, 3, 4, 5 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 6);
        rule(c, RC_RULE_GENERATION, RC_G_ACTIVE, 1);
        rule(c, RC_RULE_GENERATION, RC_G_NEXT, 0);
        cross(c, 0, RC_GT, 1, 0, RC_NONE, RC_ALWAYS, 0);
        rule(c, RC_RULE_GENERATION, RC_G_EPOCH, 2);
        rule(c, RC_RULE_REFERENCE, RC_R_CTX_VALUE, 7)->k = 0;
        rule(c, RC_RULE_EVIDENCE, 0, 3)->k = RC_EV_GENERATION;
        range(c, 4, 1, 1);
        range(c, 5, 1, 32);
        break;
    }
    case RC_EVIDENCE: {
        field(c, "subject_kind", RC_T_ENUM, 0);
        field(c, "subject", RC_T_U64, 0);
        field(c, "run", RC_T_U64, 0);
        field(c, "checks", RC_T_U64, 0);
        field(c, "failures", RC_T_U64, 0);
        field(c, "verdict", RC_T_ENUM, 0);
        field(c, "digest", RC_T_DIGEST, 0);
        field(c, "producer", RC_T_SUBJECT, 0);
        order(c, (const uint8_t[]){ 7, 0, 1, 2, 3, 5, 4, 6 });
        rule(c, RC_RULE_AUTHORITY, RC_A_PRINCIPAL, 7);
        enumr(c, 0, EV(RC_EV_VERIFY) | EV(RC_EV_GENERATION) | EV(RC_EV_MEASURE));
        rule(c, RC_RULE_STRUCT, RC_S_NONZERO, 1);
        rule(c, RC_RULE_STRUCT, RC_S_NONZERO, 2);
        range(c, 3, 1, 1ull << 32);
        enumr(c, 5, EV(RC_VERDICT_PASS) | EV(RC_VERDICT_FAIL));
        cross(c, 4, RC_LE, 3, 0, RC_NONE, RC_ALWAYS, 0);
        cross(c, 4, RC_EQ, RC_NONE, 0, 5, RC_GUARD_IN, EV(RC_VERDICT_PASS));
        cross(c, 4, RC_GT, RC_NONE, 0, 5, RC_GUARD_IN, EV(RC_VERDICT_FAIL));
        rule(c, RC_RULE_STRUCT, RC_S_NONZERO, 6);
        break;
    }
    default:
        return -1;
    }
    rc_contract_identify(c);
    return 0;
}

/* ---- checking ---- */

static int type_ok(RcType t, uint64_t v) {
    switch (t) {
    case RC_T_U64: case RC_T_DIGEST: return 1;
    case RC_T_U32: case RC_T_BITS: return v <= UINT32_MAX;
    case RC_T_BOOL: return v <= 1;
    case RC_T_ENUM: return v < 64;
    case RC_T_OBJ: return v < RX_MAX_OBJECTS;
    case RC_T_SUBJECT: return v >= 1 && v <= UINT32_MAX;
    case RC_T_RESOURCE: return v != 0;
    }
    return 0;
}

static uint64_t type_hi(RcType t) {
    switch (t) {
    case RC_T_U32: case RC_T_BITS: case RC_T_SUBJECT: return UINT32_MAX;
    case RC_T_BOOL: return 1;
    case RC_T_ENUM: return 63;
    case RC_T_OBJ: return RX_MAX_OBJECTS - 1;
    default: return UINT64_MAX;
    }
}

static int guard_holds(const RcRule *r, const uint64_t *f) {
    if (r->gmode == RC_ALWAYS || r->g == RC_NONE) return 1;
    uint64_t v = f[r->g];
    if (r->gmode == RC_GUARD_IN) return v < 64 && ((r->kg >> v) & 1u);
    return (v & r->kg) != 0;
}

static int cmp_ok(uint8_t cmp, uint64_t x, uint64_t y) {
    switch (cmp) {
    case RC_EQ: return x == y;
    case RC_NE: return x != y;
    case RC_LT: return x < y;
    case RC_LE: return x <= y;
    case RC_GT: return x > y;
    case RC_GE: return x >= y;
    }
    return 0;
}

static int forbidden(const RcContext *ctx, uint64_t res) {
    return ctx->forbid_hi && res >= ctx->forbid_lo && res <= ctx->forbid_hi;
}

/* Rights the principal holds on `res`, each reference validated now. */
static uint32_t held_rights(const RcContext *ctx, uint64_t res) {
    uint32_t have = 0;
    if (!ctx->caps || ctx->caps->subject != ctx->principal) return 0;
    for (uint32_t i = 0; i < ctx->caps->n; i++) {
        if (ctx->caps->cap[i].resource != res) continue;
        RxCapEntry e;
        uint32_t rights = ctx->caps->cap[i].rights;
        if (rx_world_validate_cap(ctx->w, ctx->caps->cap[i].ref, ctx->principal, res, rights, &e) ==
            RX_CAP_OK)
            have |= rights;
    }
    return have;
}

static int skill_index(const RcContext *ctx, uint64_t id) {
    if (!ctx->skills) return -1;
    for (uint32_t i = 0; i < ctx->skills->n; i++)
        if (ctx->skills->skill[i].id == id) return (int)i;
    return -1;
}

static int ctx_field(const RcContext *ctx, uint64_t ref, uint64_t fieldno, uint64_t *out) {
    if (ref >= RC_MAX_REFS || fieldno >= RX_MAX_FIELDS) return 0;
    RxObject o;
    if (rx_world_read(ctx->w, ctx->refs[ref], &o) != RX_OK || !o.live) return 0;
    *out = o.field[fieldno];
    return 1;
}

static int resource_live(const RcContext *ctx, uint64_t res) {
    int found = 0;
    pthread_mutex_lock(&ctx->w->mu);
    for (uint32_t i = 0; i < RX_MAX_OBJECTS && !found; i++)
        found = ctx->w->objects[i].live && ctx->w->objects[i].resource == res;
    pthread_mutex_unlock(&ctx->w->mu);
    return found;
}

/* The attached graph, validated on a copy. 0 when it validates. */
static int graph_digest(const RcContext *ctx, const AgGraph *g, AgGraph **out) {
    *out = NULL;
    if (!g) return -1;
    AgGraph *copy = malloc(sizeof *copy);
    if (!copy) return -1;
    memcpy(copy, g, sizeof *copy);
    if (rx_graph_validate(copy, ctx->w) != AG_OK_READY) {
        free(copy);
        return -1;
    }
    *out = copy;
    return 0;
}

static uint32_t graph_live_nodes(const AgGraph *g) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g->n_nodes; i++) n += g->nodes[i].alive ? 1u : 0u;
    return n;
}

static uint64_t digest_word(const uint8_t *d, uint32_t w) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)d[w * 8 + (uint32_t)i] << (8 * i);
    return v;
}

/* Fields an ACTION_GRAPH object must carry for its graph. */
static void graph_fields(const AgGraph *vg, uint64_t f[RC_MAX_FIELDS]) {
    f[0] = digest_word(vg->digest, 0);
    f[1] = digest_word(vg->digest, 1);
    f[3] = graph_live_nodes(vg);
    f[4] = vg->n_effects;
    f[6] = vg->n_evidence;
}

static int rule_holds(const RcContract *c, const RcContext *ctx, const RcObject *o,
                      const RcRule *r) {
    const uint64_t *f = o->f;
    uint64_t x = f[r->a];
    (void)c;
    switch (r->kind) {
    case RC_RULE_RANGE: return x >= r->k && x <= r->k2;
    case RC_RULE_ENUM: return x < 64 && ((r->k >> x) & 1u);
    case RC_RULE_STRUCT:
        switch (r->op) {
        case RC_S_NONZERO: return x != 0;
        case RC_S_BITS_WITHIN: return (x & ~r->k) == 0;
        case RC_S_ACTION_GRAPH: {
            AgGraph *vg;
            if (graph_digest(ctx, o->graph, &vg) != 0) return 0;
            int ok = vg->subject == f[2] && graph_live_nodes(vg) <= ctx->max_graph_nodes &&
                     vg->n_evidence >= vg->n_effects;
            for (uint32_t i = 0; i < vg->n_auth && ok; i++)
                ok = (vg->auth[i].rights & RX_RIGHT_PRIVILEGED) == 0;
            free(vg);
            return ok;
        }
        case RC_S_GRAPH_FIELDS: {
            AgGraph *vg;
            if (graph_digest(ctx, o->graph, &vg) != 0) return 1;  /* RC_S_ACTION_GRAPH reports it */
            uint64_t want[RC_MAX_FIELDS] = { 0 };
            graph_fields(vg, want);
            free(vg);
            return f[0] == want[0] && f[1] == want[1] && f[3] == want[3] && f[4] == want[4] &&
                   f[6] == want[6];
        }
        }
        return 0;
    case RC_RULE_CROSS:
    case RC_RULE_EFFECT:
        if (r->kind == RC_RULE_CROSS) {
            if (!guard_holds(r, f)) return 1;
            uint64_t y = (r->b == RC_NONE ? 0 : f[r->b]) + r->k;
            return cmp_ok(r->cmp, x, y);
        }
        switch (r->op) {
        case RC_E_FORBIDDEN: return !forbidden(ctx, x);
        case RC_E_ALLOWED: {
            if (forbidden(ctx, x)) return 0;
            for (uint32_t i = 0; i < ctx->n_effect_res; i++)
                if (ctx->effect_res[i] == x) return 1;
            return 0;
        }
        case RC_E_BUDGET: return effects_left(ctx) > 0;
        case RC_E_MAX: return x <= effects_left(ctx);
        case RC_E_APPROVAL: return !guard_holds(r, f) || x != 0;
        case RC_E_GRAPH_CLEAR: {
            /* Requirements exist only once validation has derived them. */
            AgGraph *vg;
            if (graph_digest(ctx, o->graph, &vg) != 0) return 1;  /* RC_S_ACTION_GRAPH reports it */
            int clear = 1;
            for (uint32_t i = 0; i < vg->n_auth && clear; i++)
                clear = !forbidden(ctx, vg->auth[i].resource);
            for (uint32_t i = 0; i < vg->n_nodes && clear; i++)
                clear = !(vg->nodes[i].alive && vg->nodes[i].kind == AG_CAP_RESOLVE &&
                          forbidden(ctx, vg->nodes[i].imm));
            free(vg);
            return clear;
        }
        }
        return 0;
    case RC_RULE_AUTHORITY:
        switch (r->op) {
        case RC_A_PRINCIPAL: return x == ctx->principal;
        case RC_A_HOLDS: {
            uint32_t need = r->b == RC_NONE ? (uint32_t)r->k : (uint32_t)f[r->b];
            return need != 0 && (held_rights(ctx, x) & need) == need;
        }
        case RC_A_NO_PRIVILEGED: return (x & r->k) == 0;
        }
        return 0;
    case RC_RULE_GENERATION:
        switch (r->op) {
        case RC_G_LIVE: {
            RxObject ob;
            return obj_current(ctx->w, x, &ob) && ob.generation == f[r->b];
        }
        case RC_G_ACTIVE: return x == ctx->active_generation;
        case RC_G_NEXT: return x > ctx->active_generation;
        case RC_G_EPOCH: return x == ctx->authority_epoch;
        }
        return 0;
    case RC_RULE_REFERENCE:
        switch (r->op) {
        case RC_R_CTX_FIELD: {
            if (r->b == 1 && x == 0) return 1;
            uint64_t v;
            return ctx_field(ctx, r->k, r->k2, &v) && v == x;
        }
        case RC_R_NEXT_SEQ: return x == ctx->next_seq;
        case RC_R_SKILL: return skill_index(ctx, x) >= 0;
        case RC_R_ARITY: {
            int s = skill_index(ctx, f[r->b]);
            return s >= 0 && x == ctx->skill_arity[s];
        }
        case RC_R_OBJECT: {
            RxObject ob;
            return obj_current(ctx->w, x, &ob);
        }
        case RC_R_RESOURCE_OF: {
            RxObject ob;
            return obj_current(ctx->w, f[r->b], &ob) && ob.resource == x;
        }
        case RC_R_RESOURCE_LIVE: return resource_live(ctx, x);
        case RC_R_CTX_VALUE: return r->k < 4 && x == ctx->values[r->k];
        }
        return 0;
    case RC_RULE_EVIDENCE: return evidence_pass(ctx, (uint32_t)r->k, x);
    }
    return 0;
}

int rc_check(const RcContract *c, const RcContext *ctx, const RcObject *o, RcVerdict *v) {
    memset(v, 0, sizeof *v);
    if (o->kind != c->kind) {
        v->v[v->n++] = (typeof(v->v[0])){ RC_TYPE_RULE, RC_NONE, 0 };
        v->kinds_violated |= 1u;
        return 0;
    }
    for (uint32_t i = 0; i < c->n_fields; i++) {
        if (!type_ok(c->field[i].type, o->f[i])) {
            v->v[v->n++] = (typeof(v->v[0])){ RC_TYPE_RULE, (uint8_t)i, 0 };
            v->kinds_violated |= 1u;
        }
    }
    for (uint32_t i = c->n_fields; i < RC_MAX_FIELDS; i++) {
        if (o->f[i] != 0) {
            v->v[v->n++] = (typeof(v->v[0])){ RC_TYPE_RULE, (uint8_t)i, 0 };
            v->kinds_violated |= 1u;
        }
    }
    /* Rules are evaluated only on well-typed objects: a rule reading an
     * ill-typed field has no meaning. */
    if (v->n == 0) {
        for (uint32_t i = 0; i < c->n_rules; i++) {
            const RcRule *r = &c->rule[i];
            if (!rule_holds(c, ctx, o, r)) {
                v->v[v->n++] = (typeof(v->v[0])){ (uint16_t)i, r->a, r->kind };
                v->kinds_violated |= 1u << r->kind;
            }
        }
    }
    v->ok = v->n == 0;
    return v->ok ? 0 : -1;
}

int rc_repair(const RcContract *c, const RcContext *ctx, RcObject *o, const RcVerdict *v) {
    int changed = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        if (v->v[i].rule == RC_TYPE_RULE) return -1;
        const RcRule *r = &c->rule[v->v[i].rule];
        switch (r->repair) {
        case RC_FIX_NARROW: {
            uint64_t before = o->f[r->a];
            if (r->kind == RC_RULE_STRUCT && r->op == RC_S_BITS_WITHIN) o->f[r->a] &= r->k;
            else if (r->kind == RC_RULE_AUTHORITY && r->op == RC_A_NO_PRIVILEGED) o->f[r->a] &= ~r->k;
            else return -1;
            changed += o->f[r->a] != before;
            break;
        }
        case RC_FIX_DERIVE: {
            if (r->kind == RC_RULE_REFERENCE && r->op == RC_R_NEXT_SEQ) {
                if (!c->field[r->a].derived) return -1;
                o->f[r->a] = ctx->next_seq;
                changed++;
            } else if (r->kind == RC_RULE_REFERENCE && r->op == RC_R_RESOURCE_OF) {
                RxObject ob;
                if (!c->field[r->a].derived || !obj_current(ctx->w, o->f[r->b], &ob)) return -1;
                o->f[r->a] = ob.resource;
                changed++;
            } else if (r->kind == RC_RULE_STRUCT && r->op == RC_S_GRAPH_FIELDS) {
                AgGraph *vg;
                if (graph_digest(ctx, o->graph, &vg) != 0) return -1;
                uint64_t want[RC_MAX_FIELDS] = { 0 };
                graph_fields(vg, want);
                free(vg);
                const uint8_t fs[5] = { 0, 1, 3, 4, 6 };
                for (int k = 0; k < 5; k++) {
                    if (!c->field[fs[k]].derived) return -1;
                    o->f[fs[k]] = want[fs[k]];
                }
                changed++;
            } else {
                return -1;
            }
            break;
        }
        default:
            return -1;
        }
    }
    return changed;
}

/* ---- compiling ---- */

static int post_only(const RcRule *r) {
    return (r->kind == RC_RULE_STRUCT && r->op == RC_S_ACTION_GRAPH) ||
           (r->kind == RC_RULE_EFFECT && r->op == RC_E_GRAPH_CLEAR);
}

int rc_compile(const RcContract *c, RcCompiled *p) {
    memset(p, 0, sizeof *p);
    p->c = c;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < c->n_fields; i++) {
        if (c->order[i] >= c->n_fields || (seen & (1u << c->order[i]))) return -1;
        seen |= 1u << c->order[i];
        p->pos[c->order[i]] = (uint8_t)i;
    }
    for (uint32_t i = 0; i < c->n_rules; i++) {
        const RcRule *r = &c->rule[i];
        int e = !post_only(r);
        /* Whatever the rule reads besides its target must come first. */
        if (e && r->g != RC_NONE && r->gmode != RC_ALWAYS && p->pos[r->g] >= p->pos[r->a]) e = 0;
        if (e && r->kind == RC_RULE_CROSS && r->b != RC_NONE && p->pos[r->b] >= p->pos[r->a]) e = 0;
        if (e && r->kind == RC_RULE_REFERENCE && r->op == RC_R_ARITY && p->pos[r->b] >= p->pos[r->a])
            e = 0;
        if (e && r->kind == RC_RULE_GENERATION && r->op == RC_G_LIVE && p->pos[r->a] >= p->pos[r->b])
            e = 0;
        if (e && r->kind == RC_RULE_AUTHORITY && r->op == RC_A_HOLDS && r->b != RC_NONE &&
            p->pos[r->a] >= p->pos[r->b])
            e = 0;
        p->enforced[i] = (uint8_t)e;
        if (e) p->n_enforced++;
        else p->n_post++;
    }
    return 0;
}

static void dom_set(RcDomain *d, const uint64_t *vals, uint32_t n) {
    if (!d->has_set) {
        d->has_set = 1;
        d->n_set = 0;
        for (uint32_t i = 0; i < n && d->n_set < RC_MAX_SET; i++) d->set[d->n_set++] = vals[i];
        return;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < d->n_set; i++) {
        int keep = 0;
        for (uint32_t j = 0; j < n && !keep; j++) keep = d->set[i] == vals[j];
        if (keep) d->set[k++] = d->set[i];
    }
    d->n_set = k;
}

static void dom_one(RcDomain *d, uint64_t v) { dom_set(d, &v, 1); }

static void dom_ne(RcDomain *d, uint64_t v) {
    if (d->n_ne < RC_MAX_RULES) d->ne[d->n_ne++] = v;
}

static void dom_cmp(RcDomain *d, uint8_t cmp, uint64_t y) {
    switch (cmp) {
    case RC_EQ: dom_one(d, y); break;
    case RC_NE: dom_ne(d, y); break;
    case RC_LT: if (y == 0) d->empty = 1; else if (y - 1 < d->hi) d->hi = y - 1; break;
    case RC_LE: if (y < d->hi) d->hi = y; break;
    case RC_GT: if (y == UINT64_MAX) d->empty = 1; else if (y + 1 > d->lo) d->lo = y + 1; break;
    case RC_GE: if (y > d->lo) d->lo = y; break;
    }
}

int rc_domain_contains(const RcDomain *d, uint64_t v) {
    if (d->empty || v < d->lo || v > d->hi) return 0;
    if (d->has_x && v >= d->xlo && v <= d->xhi) return 0;
    if (d->has_enum && (v >= 64 || !((d->bits >> v) & 1u))) return 0;
    if (d->is_bits && (v & ~d->bits)) return 0;
    for (uint32_t i = 0; i < d->n_ne; i++)
        if (d->ne[i] == v) return 0;
    if (d->has_set) {
        for (uint32_t i = 0; i < d->n_set; i++)
            if (d->set[i] == v) return 1;
        return 0;
    }
    return 1;
}

static uint64_t absdiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

int rc_domain_pick(const RcDomain *d, uint64_t v, uint64_t *out) {
    if (d->empty) return -1;
    if (rc_domain_contains(d, v)) { *out = v; return 0; }
    int found = 0;
    uint64_t best = 0;
    if (d->has_set) {
        for (uint32_t i = 0; i < d->n_set; i++)
            if (rc_domain_contains(d, d->set[i]) &&
                (!found || absdiff(d->set[i], v) < absdiff(best, v))) {
                best = d->set[i];
                found = 1;
            }
    } else if (d->has_enum) {
        for (uint64_t x = 0; x < 64; x++)
            if (rc_domain_contains(d, x) && (!found || absdiff(x, v) < absdiff(best, v))) {
                best = x;
                found = 1;
            }
    } else if (d->is_bits) {
        /* Narrowing first: the value with the forbidden bits taken away. */
        const uint64_t tries[3] = { v & d->bits, d->bits, d->lo };
        for (int i = 0; i < 3 && !found; i++)
            if (rc_domain_contains(d, tries[i])) { best = tries[i]; found = 1; }
    } else {
        uint64_t c = v < d->lo ? d->lo : (v > d->hi ? d->hi : v);
        if (d->has_x && c >= d->xlo && c <= d->xhi) {
            if (d->xhi < d->hi) c = d->xhi + 1;
            else if (d->xlo > d->lo) c = d->xlo - 1;
        }
        for (uint32_t step = 0; step <= RC_MAX_RULES + 1 && !found; step++) {
            if (c + step >= c && rc_domain_contains(d, c + step)) { best = c + step; found = 1; }
            else if (c >= step && rc_domain_contains(d, c - step)) { best = c - step; found = 1; }
        }
    }
    if (!found) return -1;
    *out = best;
    return 0;
}

static int dom_nonempty(const RcDomain *d) {
    uint64_t x;
    return rc_domain_pick(d, d->lo, &x) == 0;
}

int rc_domain(const RcCompiled *p, const RcContext *ctx, const RcObject *partial,
              uint32_t fld, RcDomain *d) {
    const RcContract *c = p->c;
    memset(d, 0, sizeof *d);
    if (fld >= c->n_fields) return -1;
    RcType t = c->field[fld].type;
    d->hi = type_hi(t);
    if (t == RC_T_SUBJECT || t == RC_T_RESOURCE) d->lo = 1;
    if (t == RC_T_BITS) { d->is_bits = 1; d->bits = UINT32_MAX; }
    const uint64_t *f = partial->f;
    uint64_t vals[RC_MAX_SET];
    uint32_t n;
    for (uint32_t i = 0; i < c->n_rules; i++) {
        if (!p->enforced[i]) continue;
        const RcRule *r = &c->rule[i];
        int target = r->a == fld;
        switch (r->kind) {
        case RC_RULE_RANGE:
            if (target) { if (r->k > d->lo) d->lo = r->k; if (r->k2 < d->hi) d->hi = r->k2; }
            break;
        case RC_RULE_ENUM:
            if (target) { d->bits = d->has_enum ? (d->bits & r->k) : r->k; d->has_enum = 1; }
            break;
        case RC_RULE_STRUCT:
            if (r->op == RC_S_NONZERO && target) { if (d->lo < 1) d->lo = 1; }
            else if (r->op == RC_S_BITS_WITHIN && target) { d->is_bits = 1; d->bits &= r->k; }
            else if (r->op == RC_S_GRAPH_FIELDS && (fld == 0 || fld == 1 || fld == 3 || fld == 4 ||
                                                     fld == 6)) {
                AgGraph *vg;
                if (graph_digest(ctx, partial->graph, &vg) == 0) {
                    uint64_t want[RC_MAX_FIELDS] = { 0 };
                    graph_fields(vg, want);
                    free(vg);
                    dom_one(d, want[fld]);
                }
            }
            break;
        case RC_RULE_CROSS:
            if (target && guard_holds(r, f))
                dom_cmp(d, r->cmp, (r->b == RC_NONE ? 0 : f[r->b]) + r->k);
            break;
        case RC_RULE_AUTHORITY:
            if (r->op == RC_A_PRINCIPAL && target) dom_one(d, ctx->principal);
            else if (r->op == RC_A_NO_PRIVILEGED && target) { d->is_bits = 1; d->bits &= ~r->k; }
            else if (r->op == RC_A_HOLDS) {
                uint32_t need = r->b == RC_NONE ? (uint32_t)r->k : 0;
                if (target) {
                    /* Each reference validated once; rights summed per resource. */
                    n = 0;
                    uint32_t have[RC_MAX_SET];
                    for (uint32_t k = 0; ctx->caps && ctx->caps->subject == ctx->principal &&
                                         k < ctx->caps->n; k++) {
                        uint64_t res = ctx->caps->cap[k].resource;
                        uint32_t rights = ctx->caps->cap[k].rights;
                        RxCapEntry e;
                        if (rx_world_validate_cap(ctx->w, ctx->caps->cap[k].ref, ctx->principal, res,
                                                  rights, &e) != RX_CAP_OK)
                            continue;
                        uint32_t j = 0;
                        while (j < n && vals[j] != res) j++;
                        if (j == n) {
                            if (n == RC_MAX_SET) continue;
                            vals[n] = res;
                            have[n++] = 0;
                        }
                        have[j] |= rights;
                    }
                    uint32_t m = 0;
                    for (uint32_t j = 0; j < n; j++)
                        if (have[j] && (have[j] & need) == need) vals[m++] = vals[j];
                    dom_set(d, vals, m);
                } else if (r->b == fld) {
                    d->is_bits = 1;
                    d->bits &= held_rights(ctx, f[r->a]);
                    if (d->lo < 1) d->lo = 1;
                }
            }
            break;
        case RC_RULE_GENERATION:
            if (r->op == RC_G_LIVE) {
                if (target) {
                    n = 0;
                    RxObject ob;
                    for (uint32_t k = 0; k < RX_MAX_OBJECTS && n < RC_MAX_SET; k++)
                        if (obj_current(ctx->w, k, &ob)) vals[n++] = k;
                    dom_set(d, vals, n);
                } else if (r->b == fld) {
                    RxObject ob;
                    if (obj_current(ctx->w, f[r->a], &ob)) dom_one(d, ob.generation);
                    else d->empty = 1;
                }
            } else if (target) {
                if (r->op == RC_G_ACTIVE) dom_one(d, ctx->active_generation);
                else if (r->op == RC_G_EPOCH) dom_one(d, ctx->authority_epoch);
                else if (r->op == RC_G_NEXT) dom_cmp(d, RC_GT, ctx->active_generation);
            }
            break;
        case RC_RULE_REFERENCE:
            switch (r->op) {
            case RC_R_CTX_FIELD:
                if (target) {
                    n = 0;
                    uint64_t v;
                    if (ctx_field(ctx, r->k, r->k2, &v)) vals[n++] = v;
                    if (r->b == 1) vals[n++] = 0;
                    dom_set(d, vals, n);
                }
                break;
            case RC_R_NEXT_SEQ: if (target) dom_one(d, ctx->next_seq); break;
            case RC_R_CTX_VALUE: if (target && r->k < 4) dom_one(d, ctx->values[r->k]); break;
            case RC_R_SKILL:
                if (target) {
                    n = 0;
                    for (uint32_t k = 0; ctx->skills && k < ctx->skills->n; k++)
                        vals[n++] = ctx->skills->skill[k].id;
                    dom_set(d, vals, n);
                }
                break;
            case RC_R_ARITY:
                if (target) {
                    int s = skill_index(ctx, f[r->b]);
                    if (s >= 0) dom_one(d, ctx->skill_arity[s]);
                    else d->empty = 1;
                }
                break;
            case RC_R_OBJECT:
            case RC_R_RESOURCE_OF:
            case RC_R_RESOURCE_LIVE: {
                int want_obj = (r->op == RC_R_OBJECT && target) ||
                               (r->op == RC_R_RESOURCE_OF && r->b == fld && p->pos[r->b] > p->pos[r->a]);
                int want_res = (r->op == RC_R_RESOURCE_LIVE && target) ||
                               (r->op == RC_R_RESOURCE_OF && target && p->pos[r->a] > p->pos[r->b]);
                if (!want_obj && !want_res) break;
                n = 0;
                RxObject ob;
                if (r->op == RC_R_RESOURCE_OF && want_res) {
                    if (obj_current(ctx->w, f[r->b], &ob)) vals[n++] = ob.resource;
                } else {
                    for (uint32_t k = 0; k < RX_MAX_OBJECTS && n < RC_MAX_SET; k++) {
                        if (!obj_current(ctx->w, k, &ob)) continue;
                        if (r->op == RC_R_RESOURCE_OF && ob.resource != f[r->a]) continue;
                        uint64_t v = want_obj ? k : ob.resource;
                        int dup = 0;
                        for (uint32_t j = 0; j < n && !dup; j++) dup = vals[j] == v;
                        if (!dup) vals[n++] = v;
                    }
                }
                dom_set(d, vals, n);
                break;
            }
            }
            break;
        case RC_RULE_EVIDENCE:
            if (target) {
                RcContext *m = (RcContext *)ctx;
                n = 0;
                pthread_mutex_lock(&m->mu);
                for (uint32_t k = 0; k < ctx->n_evidence && n < RC_MAX_SET; k++)
                    if (ctx->evidence[k].subject_kind == r->k &&
                        ctx->evidence[k].verdict == RC_VERDICT_PASS)
                        vals[n++] = ctx->evidence[k].subject;
                pthread_mutex_unlock(&m->mu);
                dom_set(d, vals, n);
            }
            break;
        case RC_RULE_EFFECT:
            if (!target) break;
            if (r->op == RC_E_FORBIDDEN) {
                if (ctx->forbid_hi) {
                    d->has_x = 1;
                    d->xlo = ctx->forbid_lo;
                    d->xhi = ctx->forbid_hi;
                }
            } else if (r->op == RC_E_ALLOWED) {
                n = 0;
                for (uint32_t k = 0; k < ctx->n_effect_res; k++)
                    if (!forbidden(ctx, ctx->effect_res[k])) vals[n++] = ctx->effect_res[k];
                dom_set(d, vals, n);
            } else if (r->op == RC_E_BUDGET) {
                if (effects_left(ctx) == 0) d->empty = 1;
            } else if (r->op == RC_E_MAX) {
                uint64_t left = effects_left(ctx);
                if (left < d->hi) d->hi = left;
            } else if (r->op == RC_E_APPROVAL) {
                if (guard_holds(r, f)) { if (d->lo < 1) d->lo = 1; }
            }
            break;
        }
    }
    if (!d->empty && !dom_nonempty(d)) d->empty = 1;
    return 0;
}

/* ---- producing ---- */

int rc_produce(const RcCompiled *p, const RcContext *ctx, const RcBackend *b,
               uint32_t max_retries, RcObject *out, RcStats *st) {
    const RcContract *c = p->c;
    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) st->retries++;
        uint64_t t0 = now_ns();
        if (b->begin) b->begin(b->self, attempt);
        RcObject o;
        memset(&o, 0, sizeof o);
        o.kind = c->kind;
        o.graph = (c->kind == RC_ACTION_GRAPH && b->graph) ? b->graph(b->self, attempt) : NULL;
        int dead = 0;
        for (uint32_t i = 0; i < c->n_fields; i++) {
            uint32_t fld = c->order[i];
            if (b->constrained) {
                RcDomain d;
                rc_domain(p, ctx, &o, fld, &d);
                if (d.empty) {
                    st->gen_ns += now_ns() - t0;
                    /* Nothing at all fits before anything was chosen: no
                     * candidate can ever be valid in this context. */
                    if (i == 0) return RC_OUT_UNSATISFIABLE;
                    dead = 1;
                    break;
                }
                uint64_t v = b->draw(b->self, c, attempt, fld, &o, &d);
                st->draws++;
                if (!rc_domain_contains(&d, v) && rc_domain_pick(&d, v, &v) != 0) { dead = 1; break; }
                o.f[fld] = v;
            } else {
                o.f[fld] = b->draw(b->self, c, attempt, fld, &o, NULL);
                st->draws++;
            }
        }
        st->gen_ns += now_ns() - t0;
        if (dead) {
            st->dead_ends++;
            continue;
        }
        st->candidates++;
        uint64_t t1 = now_ns();
        RcVerdict v;
        rc_check(c, ctx, &o, &v);
        st->checks++;
        if (v.ok) {
            st->check_ns += now_ns() - t1;
            *out = o;
            return attempt == 0 ? RC_OUT_VALID : RC_OUT_RETRIED;
        }
        st->invalid++;
        st->kinds_violated |= v.kinds_violated;
        RcObject r = o;
        if (rc_repair(c, ctx, &r, &v) >= 0) {
            RcVerdict v2;
            rc_check(c, ctx, &r, &v2);
            st->checks++;
            if (v2.ok) {
                st->check_ns += now_ns() - t1;
                st->repairs++;
                *out = r;
                return RC_OUT_REPAIRED;
            }
        }
        st->check_ns += now_ns() - t1;
    }
    return RC_OUT_REJECTED;
}

/* ---- serialization ---- */

size_t rc_encode(const RcContract *c, const RcObject *o, uint8_t *buf, size_t cap) {
    size_t need = 4 + 32 + 8u * c->n_fields + (o->graph ? 32u : 0u);
    if (cap < need) return 0;
    for (int j = 0; j < 4; j++) buf[j] = (uint8_t)((uint32_t)o->kind >> (8 * j));
    memcpy(buf + 4, c->identity, 32);
    for (uint32_t i = 0; i < c->n_fields; i++)
        for (int j = 0; j < 8; j++) buf[36 + i * 8 + (uint32_t)j] = (uint8_t)(o->f[i] >> (8 * j));
    if (o->graph) memcpy(buf + 36 + 8u * c->n_fields, o->graph->digest, 32);
    return need;
}

int rc_decode(const RcContract *c, const uint8_t *buf, size_t n, RcObject *o) {
    memset(o, 0, sizeof *o);
    if (n != 4 + 32 + 8u * c->n_fields) return -1;
    uint32_t kind = 0;
    for (int j = 0; j < 4; j++) kind |= (uint32_t)buf[j] << (8 * j);
    if (kind != (uint32_t)c->kind || memcmp(buf + 4, c->identity, 32) != 0) return -1;
    o->kind = c->kind;
    for (uint32_t i = 0; i < c->n_fields; i++)
        for (int j = 0; j < 8; j++) o->f[i] |= (uint64_t)buf[36 + i * 8 + (uint32_t)j] << (8 * j);
    return 0;
}

void rc_identity(const RcContract *c, const RcObject *o, uint8_t out[32]) {
    uint8_t buf[4 + 32 + 8 * RC_MAX_FIELDS + 32];
    size_t n = rc_encode(c, o, buf, sizeof buf);
    sha256_hash(buf, n, out);
}

int rc_to_json(const RcContract *c, const RcObject *o, char *buf, size_t cap) {
    size_t at = 0;
    int w = snprintf(buf, cap, "{\"kind\":\"%s\"", c->name);
    if (w < 0 || (size_t)w >= cap) return -1;
    at = (size_t)w;
    for (uint32_t i = 0; i < c->n_fields; i++) {
        w = snprintf(buf + at, cap - at, ",\"%s\":%llu", c->field[i].name,
                     (unsigned long long)o->f[i]);
        if (w < 0 || (size_t)w >= cap - at) return -1;
        at += (size_t)w;
    }
    if (at + 2 > cap) return -1;
    buf[at++] = '}';
    buf[at] = 0;
    return (int)at;
}

static const char *ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

/* A JSON string without escapes (keys and the kind are plain identifiers). */
static const char *jstr(const char *s, char *out, size_t cap) {
    if (*s != '"') return NULL;
    s++;
    size_t n = 0;
    while (*s && *s != '"') {
        if (*s == '\\' || (unsigned char)*s < 0x20 || n + 1 >= cap) return NULL;
        out[n++] = *s++;
    }
    if (*s != '"') return NULL;
    out[n] = 0;
    return s + 1;
}

static const char *juint(const char *s, uint64_t *out) {
    if (*s < '0' || *s > '9') return NULL;
    if (*s == '0' && s[1] >= '0' && s[1] <= '9') return NULL;   /* no leading zeros */
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return NULL;
        v = v * 10 + d;
        s++;
    }
    if (*s == '.' || *s == 'e' || *s == 'E') return NULL;
    *out = v;
    return s;
}

int rc_from_json(const RcContract *c, const char *text, RcObject *o) {
    memset(o, 0, sizeof *o);
    o->kind = c->kind;
    const char *s = ws(text);
    if (*s != '{') return -1;
    s = ws(s + 1);
    uint32_t seen = 0;
    int kind_seen = 0;
    for (;;) {
        char key[64];
        if (!(s = jstr(s, key, sizeof key))) return -2;
        s = ws(s);
        if (*s != ':') return -2;
        s = ws(s + 1);
        if (strcmp(key, "kind") == 0) {
            char kn[64];
            if (kind_seen || !(s = jstr(s, kn, sizeof kn)) || strcmp(kn, c->name) != 0) return -3;
            kind_seen = 1;
        } else {
            uint32_t i = 0;
            while (i < c->n_fields && strcmp(c->field[i].name, key) != 0) i++;
            if (i == c->n_fields || (seen & (1u << i))) return -4;
            if (!(s = juint(s, &o->f[i]))) return -5;
            seen |= 1u << i;
        }
        s = ws(s);
        if (*s == ',') { s = ws(s + 1); continue; }
        if (*s == '}') { s = ws(s + 1); break; }
        return -2;
    }
    if (*s != 0) return -2;
    if (!kind_seen || seen != (c->n_fields >= 32 ? UINT32_MAX : (1u << c->n_fields) - 1u)) return -6;
    return 0;
}

/* ---- the publish gate ---- */

static int gate_fn(RxCtx *x) {
    RcGate *g = x->user;
    const RcContract *c = g->c;
    const RxSnapshotDep *draft = &x->in[0], *status = &x->in[1];
    RcObject o;
    memset(&o, 0, sizeof o);
    o.kind = c->kind;
    for (uint32_t i = 0; i < c->n_fields; i++) o.f[i] = draft->field[i];
    for (uint32_t i = c->n_fields; i < RC_MAX_FIELDS; i++) o.f[i] = draft->field[i];
    o.graph = g->graph;
    RcVerdict v;
    rc_check(c, g->ctx, &o, &v);
    uint64_t verdict = RC_GATE_PUBLISHED, first = 0, kinds = v.kinds_violated;
    if (!v.ok) {
        first = v.v[0].rule;
        RcObject r = o;
        RcVerdict v2;
        if (rc_repair(c, g->ctx, &r, &v) >= 0 && rc_check(c, g->ctx, &r, &v2) == 0) {
            o = r;
            verdict = RC_GATE_REPAIRED;
        } else {
            verdict = RC_GATE_REJECTED;
        }
    }
    uint64_t st[RX_MAX_FIELDS];
    memcpy(st, status->field, sizeof st);
    st[0]++;
    st[1] = verdict;
    st[2] = first;
    st[3] = kinds;
    if (verdict == RC_GATE_PUBLISHED) st[4]++;
    if (verdict == RC_GATE_REPAIRED) { st[4]++; st[6]++; }
    if (verdict == RC_GATE_REJECTED) st[5]++;
    uint8_t id[32];
    rc_identity(c, &o, id);
    st[7] = digest_word(id, 0);
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++)
        x->out[x->n_out++] = (RxMutation){ g->status, i, st[i] };
    if (verdict != RC_GATE_REJECTED)
        for (uint32_t i = 0; i < RC_MAX_FIELDS; i++)
            x->out[x->n_out++] = (RxMutation){ g->published, i, o.f[i] };
    return 0;
}

int rc_gate_register(RcGate *g, RxWorld *w, const RcContract *c, RcContext *ctx,
                     uint32_t subject, RxObjRef draft, RxCapRef draft_read,
                     RxObjRef published, RxCapRef published_rw,
                     RxObjRef status, RxCapRef status_rw) {
    memset(g, 0, sizeof *g);
    g->c = c;
    g->ctx = ctx;
    g->draft = draft;
    g->published = published;
    g->status = status;
    g->subject = subject;
    RxObject od, op, os;
    if (rx_world_read(w, draft, &od) != RX_OK || rx_world_read(w, published, &op) != RX_OK ||
        rx_world_read(w, status, &os) != RX_OK)
        return RX_ERR_ARG;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "contract.gate";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = subject;
    d.priority = RX_PRIO_FOREGROUND;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){ draft, RX_ALL_FIELDS };
    d.n_reads = 1;
    d.reads[0] = (RxDep){ status, RX_ALL_FIELDS };
    d.n_writes = 2;
    d.writes[0] = (RxDep){ status, RX_ALL_FIELDS };
    d.writes[1] = (RxDep){ published, RX_ALL_FIELDS };
    d.n_caps = 3;
    d.caps[0] = (RxCapNeed){ draft_read, od.resource, RX_RIGHT_READ };
    d.caps[1] = (RxCapNeed){ published_rw, op.resource, RX_RIGHT_READ | RX_RIGHT_WRITE };
    d.caps[2] = (RxCapNeed){ status_rw, os.resource, RX_RIGHT_READ | RX_RIGHT_WRITE };
    d.stamp_proposed = true;
    d.fn = gate_fn;
    d.user = g;
    return rx_world_add_reaction(w, &d, &g->reaction);
}
