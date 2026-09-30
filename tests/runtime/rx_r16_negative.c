/*
 * rx_r16_negative.c -- R16-G4: legacy paths cannot bypass authority
 * (spec/r16-orchestrator-retirement.md §5 R16-G4, clarification C4).
 *
 * The production body (the R13 living system, built by the R15 rig in RES-4)
 * is started and left quiescent. A legacy / reference / oracle context then
 * tries six acts. It holds what such code could hold after R16: the world
 * handle, the read-only AIENOS view, the R9 store handle, every capability
 * reference it could observe (the harness's external grants, the promoter's
 * grants) and grants of its own on its own resources. It never holds the
 * AIENOS admin handle; only this harness does, to build fixtures before the
 * acts start.
 *
 *   (1) write authoritative AIEN belief
 *   (2) select an Omega realization (write Omega's selection)
 *   (3) mint (or revoke) authority
 *   (4) promote a generation
 *   (5) bypass the effect/authority boundary (forged publication descriptor,
 *       overwritten physical record)
 *   (6) advance the world generation (write the in-force record production
 *       reads; move the R9 active generation)
 *
 * Every attempt must be refused by the R7 native authority, R9's promotion
 * rules or the publication boundary, with no change to authoritative state:
 * the watched objects (fields, version, generation), the R9 active generation
 * and lineage (in memory and recovered from disk) and the AIENOS table (no
 * state change reported by the authority observer).
 *
 * "Removing any one guard turns the test red" is checked by
 * tests/r16_negative/mutate.sh (make test-r16-negative-mutants), which builds
 * this test against scratch copies with one guard removed at a time.
 * Two cases exist so that redundant guards are each load-bearing: a legacy
 * reaction whose real grant the office revokes while it runs (the commit
 * re-check), and at the boundary a real grant that is not the writer of
 * record's (identity) and the writer's own grant after revocation (validation).
 *
 * Spec C5 (caller identity). The legacy context also holds runtime-issued
 * caller credentials of its own (LEGACY, LEGACY2: enrolled by the rig as
 * fixtures before the world's caller set is bound), never another subject's.
 * The pre-fix exploit is kept: last, a promotion naming the promoter itself as
 * subject with the promoter's grant reference, and a reaction naming the
 * promoter as subject with its in-force grant. At 44d8c06 (before C5) both were
 * ACCEPTED (generation moved 1 -> 3 in memory and on disk; the in-force record
 * written) and the gate printed FAIL. Now each credential variant the legacy
 * context could present (none, zero, random, its own, its own secret at the
 * promoter's generation, a permissive authority callback) must be refused with
 * the identity error, with nothing moved. Then the C5 identity probes: spoofed
 * subject, forged / stale / unknown / absent credentials, enrollment after
 * bind, revocation needing the credential, secret collision (rig credentials
 * and 64 enrollments on a scratch world), an identity revoked while its
 * reaction runs (the write must not commit, later activations blocked), the
 * revoked credential replayed at check, revocation, proposal, promotion and
 * registration, and a bound R9 store ignoring a permissive authority callback.
 * Last, a positive control: the promoter, with its own credential and grant,
 * still promotes. The gate line covers all of it and is what the mutant suite
 * judges; the six-act core is also printed on its own line ("R16 G4 core:").
 *
 * Also checked (reported, outside the six): the SEQ reference loop refuses to
 * drive a production world.
 *
 * Limits (stated in the receipt, not tested as refusals):
 *   - world-owner calls (rx_world_create, rx_world_retire) take no capability:
 *     any code holding the RxWorld pointer can retire an object, which moves
 *     that object's generation. rx_world_add_reaction now needs the named
 *     subject's credential on a bound world, and registering a reaction still
 *     grants it nothing; its grants are checked at every activation.
 *   - rx_gen_promote on a store bound with rx_gen_bind_authority uses the
 *     store's own authority and ignores the caller's RxGenAuthFn (tested
 *     below). An unbound store (older tests) still takes the caller's.
 *   - Caller credentials are secrets in process memory: code in the same
 *     address space that reads another component's keyring can act as it.
 *   - C code in the same address space can write any memory. G3 shows the
 *     production binary does not link or exec the legacy code at all.
 */
#include "rx_r15_rig.h"
#include "runtime/aienos_cap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#define U(x) ((unsigned long long)(x))

enum { EXTERNAL_SUBJ = 100, LEGACY_SUBJ = 160, LEGACY2_SUBJ = 161 };
#define RES_LEGACY_SCRATCH 0x7160000ull

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("R16 G4 FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); g_fail++; } } while (0)

/* ---- authority observer: counts AIENOS state changes ------------------------ */
static atomic_ullong g_auth_changes;
static void observer(void *ctx, uint32_t op, const AienosCapEntry *entry, int result) {
    (void)ctx; (void)op;
    if (entry && result == 0) atomic_fetch_add(&g_auth_changes, 1);
}
/* AIENOS changes the harness itself makes during the acts (the office
 * revoking a grant, see revoked_in_flight and the end of act 5). Any change
 * beyond these fails the act. */
static atomic_ullong g_expected_changes;

/* ---- authoritative state snapshot ------------------------------------------ */
#define N_WATCH 16
typedef struct {
    const char *name[N_WATCH];
    RxObjRef ref[N_WATCH];
    RxObject obj[N_WATCH];
    uint32_t n;
    uint64_t gen_active, gen_lineage, recovered_active, recovered_lineage;
    uint64_t auth_changes;
    uint8_t digest[32];
} Snap;

static void watch(Snap *s, const char *name, RxObjRef ref) {
    if (s->n < N_WATCH) { s->name[s->n] = name; s->ref[s->n] = ref; s->n++; }
}

static int take(R15Rig *r, Snap *s) {
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK) return -1;
    for (uint32_t i = 0; i < s->n; i++) {
        memset(&s->obj[i], 0, sizeof s->obj[i]);
        if (rx_world_read(&r->w, s->ref[i], &s->obj[i]) != RX_OK) return -1;
    }
    if (rx_gen_active(r->gen, &s->gen_active, &s->gen_lineage) != RX_GEN_OK) return -1;
    RxRecoveryRecord rec;
    if (rx_gen_recover(r->generation_dir, &rec) != RX_GEN_OK) return -1;
    s->recovered_active = rec.active_id;
    s->recovered_lineage = rec.lineage;
    s->auth_changes = atomic_load(&g_auth_changes);
    return 0;
}

/* Same authoritative state? Prints the first difference. */
static int same_x(const Snap *a, const Snap *b, const char *act, unsigned long long expected) {
    for (uint32_t i = 0; i < a->n; i++) {
        const RxObject *x = &a->obj[i], *y = &b->obj[i];
        if (x->version != y->version || x->generation != y->generation || x->live != y->live ||
            memcmp(x->field, y->field, sizeof x->field) != 0) {
            printf("R16 G4 FAIL: %s changed %s (version %llu -> %llu, generation %u -> %u)\n",
                   act, a->name[i], U(x->version), U(y->version), x->generation, y->generation);
            return 0;
        }
    }
    if (a->gen_active != b->gen_active || a->gen_lineage != b->gen_lineage ||
        a->recovered_active != b->recovered_active ||
        a->recovered_lineage != b->recovered_lineage) {
        printf("R16 G4 FAIL: %s moved the R9 generation (%llu/%llu -> %llu/%llu, disk %llu/%llu -> %llu/%llu)\n",
               act, U(a->gen_active), U(a->gen_lineage), U(b->gen_active), U(b->gen_lineage),
               U(a->recovered_active), U(a->recovered_lineage), U(b->recovered_active),
               U(b->recovered_lineage));
        return 0;
    }
    if (b->auth_changes - a->auth_changes != expected) {
        printf("R16 G4 FAIL: %s changed the AIENOS table (%llu state changes, %llu by the harness)\n",
               act, U(b->auth_changes - a->auth_changes), U(expected));
        return 0;
    }
    return 1;
}
static int same(const Snap *a, const Snap *b, const char *act) {
    return same_x(a, b, act, atomic_load(&g_expected_changes));
}

/* ---- the legacy context -------------------------------------------------------- */
typedef struct {
    RxWorld *w;                  /* world handle (world-owner calls only) */
    const AienosCapView *view;   /* read-only authority view */
    RxGenStore *gen;             /* R9 store handle */
    /* references it could observe */
    RxCapRef stolen_goal;        /* the harness's external WRITE on AIEN's goal */
    RxCapRef borrowed_inforce;   /* the promoter's WRITE on the in-force record */
    RxCapRef borrowed_promote;   /* the promoter's promotion right */
    RxCapRef forged;             /* a made-up reference */
    /* its own grants (issued by the harness as fixtures) */
    RxCapRef own_scratch;        /* LEGACY: READ|WRITE on its scratch object */
    RxCapRef own_promote;        /* LEGACY: PROMOTE on the promotion resource */
    RxCapRef own2_read_promote;  /* LEGACY2: READ on the promotion resource */
    RxCapRef ext_scratch;        /* EXTERNAL: WRITE on the scratch object (trigger) */
    RxCapRef inflight_belief;    /* LEGACY: WRITE on AIEN's belief, revoked while it runs */
    RxCapRef own_output;         /* LEGACY: WRITE on the seat output's resource (not the
                                    writer of record) */
    AienosCapAdmin *office;      /* the harness's office: used only to revoke, never by
                                    the legacy context's own calls */
    RxObjRef scratch;
    RxObjRef scratch2;           /* a second scratch object (C5 in-flight probe target) */
    const RxCallerKeyring *keys; /* its own caller credentials (LEGACY, LEGACY2), issued
                                    by the runtime at enrollment */
    uint64_t candidate;          /* a draft proposed by LEGACY */
} Legacy;

typedef struct { uint32_t tried, refused; } Tally;
static void tally(Tally *t, int refused, const char *act, const char *what, long long rc) {
    t->tried++;
    if (refused) t->refused++;
    else printf("R16 G4 FAIL: %s: %s was NOT refused (rc %lld)\n", act, what, rc);
    if (!refused) g_fail++;
}

static int64_t publish_one(Legacy *L, RxCapRef cap, RxObjRef obj, uint32_t field, uint64_t v) {
    RxMutation m = {obj, field, v};
    return rx_world_publish_external(L->w, cap, &m, 1);
}

typedef struct { RxObjRef target; uint32_t field; } Poke;
static int fn_poke(RxCtx *c) {
    Poke *p = c->user;
    c->out[c->n_out++] = (RxMutation){p->target, p->field, 0xBADBADull};
    return 0;
}

/* A reaction registered by the legacy context: woken by its scratch object,
 * it writes `target` holding `cap` declared for the target's resource. */
static uint32_t g_named_subject;   /* 0: LEGACY_SUBJ; else the subject the reaction names */
static RxDep g_named_trigger;      /* with g_named_subject: its trigger and read grant */
static RxCapNeed g_named_read;
/* With g_named_explicit: the credential the descriptor carries (NULL: none),
 * instead of the one its keyring holds for the named subject. */
static const RxCallerCred *g_named_cred;
static int g_named_explicit;
static int legacy_reaction_fn(Legacy *L, const char *name, RxObjRef target, uint64_t target_res,
                              RxCapRef cap, int declare_write, Poke *p, RxFn fn, void *user,
                              uint32_t *id) {
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = g_named_subject ? g_named_subject : LEGACY_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn;
    d.user = user;
    *p = (Poke){target, 0};
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){L->scratch, RX_FIELD(0)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){target, RX_FIELD(0)};
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){L->own_scratch, RES_LEGACY_SCRATCH, RX_RIGHT_READ};
    if (g_named_subject) {             /* its scratch grant is LEGACY_SUBJ's */
        d.triggers[0] = g_named_trigger;
        d.caps[0] = g_named_read;
    }
    if (declare_write) d.caps[d.n_caps++] = (RxCapNeed){cap, target_res, RX_RIGHT_WRITE};
    if (g_named_explicit) {
        if (g_named_cred) d.caller = *g_named_cred;
        int rc = rx_world_add_reaction(L->w, &d, id);
        rx_caller_wipe(&d.caller);
        return rc;
    }
    return rx_world_add_reaction_keyed(L->w, L->keys, &d, id);
}
static int legacy_reaction(Legacy *L, const char *name, RxObjRef target, uint64_t target_res,
                           RxCapRef cap, int declare_write, Poke *p, uint32_t *id) {
    return legacy_reaction_fn(L, name, target, target_res, cap, declare_write, p, fn_poke, p, id);
}

static uint64_t g_trigger;
/* Wake the legacy reactions: an honest external write to the scratch object. */
static int wake_legacy(Legacy *L) {
    if (publish_one(L, L->ext_scratch, L->scratch, 0, ++g_trigger) <= 0) return -1;
    return rx_world_wait_quiescent(L->w, 10000);
}

/* Acts (1), (2) and the record half of (6) share one shape: a legacy write
 * to an authoritative object, through every door it could try. */
static void write_attempts(Legacy *L, const char *act, RxObjRef target, RxCapRef borrowed,
                           const char *borrowed_name, Tally *t) {
    RxObject o;
    if (rx_world_read(L->w, target, &o) != RX_OK) { CHECK(0, "%s: target unreadable", act); return; }
    uint64_t res = o.resource;
    int64_t rc;
    rc = publish_one(L, L->forged, target, 0, 0xBADBADull);
    tally(t, rc == RX_ERR_AUTHORITY, act, "outside publication with a forged reference", rc);
    rc = publish_one(L, L->stolen_goal, target, 0, 0xBADBADull);
    tally(t, rc == RX_ERR_AUTHORITY, act, "outside publication with the external goal grant", rc);
    rc = publish_one(L, borrowed, target, 0, 0xBADBADull);
    tally(t, rc == RX_ERR_AUTHORITY, act, borrowed_name, rc);
    rc = publish_one(L, L->own_scratch, target, 0, 0xBADBADull);
    tally(t, rc == RX_ERR_AUTHORITY, act, "outside publication with its own scratch grant", rc);

    /* Registered reactions. Undeclared write: refused at registration. */
    static Poke p_undeclared, p_forged, p_own;
    uint32_t id_u = 0, id_f = 0, id_o = 0;
    int add_u = legacy_reaction(L, "legacy.undeclared", target, res, L->forged, 0, &p_undeclared,
                                &id_u);
    tally(t, add_u == RX_ERR_AUTHORITY, act, "reaction writing without a declared grant", add_u);
    int add_f = legacy_reaction(L, "legacy.forged", target, res, L->forged, 1, &p_forged, &id_f);
    int add_o = legacy_reaction(L, "legacy.own", target, res, L->own_scratch, 1, &p_own, &id_o);
    CHECK(add_f == RX_OK && add_o == RX_OK, "%s: could not register legacy reactions (%d %d)",
          act, add_f, add_o);
    if (add_f != RX_OK || add_o != RX_OK) return;
    uint64_t blocked0 = L->w->stats.blocked_authority;
    CHECK(wake_legacy(L) == 0, "%s: legacy reactions did not settle", act);
    pthread_mutex_lock(&L->w->mu);
    RxReaction rf = L->w->reactions[id_f], ro = L->w->reactions[id_o];
    const RxCrumb *kf = rx_world_crumb(L->w, rf.last_crumb), *ko = rx_world_crumb(L->w, ro.last_crumb);
    int bf = kf && kf->kind == RX_CRUMB_BLOCKED_AUTHORITY, bo = ko && ko->kind == RX_CRUMB_BLOCKED_AUTHORITY;
    uint64_t blocked = L->w->stats.blocked_authority - blocked0;
    pthread_mutex_unlock(&L->w->mu);
    tally(t, rf.activations >= 1 && rf.commits == 0 && bf, act,
          "reaction holding a forged reference", (long long)rf.commits);
    tally(t, ro.activations >= 1 && ro.commits == 0 && bo, act,
          "reaction holding its own grant declared for the target", (long long)ro.commits);
    CHECK(blocked >= 2, "%s: %llu blocked activations, expected 2", act, U(blocked));
}


/* A legacy reaction holding a genuine WRITE grant on the target, which the
 * office revokes while the activation runs (the body is only the fixed point
 * in time; the revocation is the harness's). The write must not land: the
 * commit re-check refuses it (RX_CRUMB_REJECTED, no commit). */
typedef struct { Poke p; AienosCapAdmin *office; RxCapRef cap; int revoked; } Inflight;
static int fn_inflight(RxCtx *c) {
    Inflight *f = c->user;
    if (!f->revoked) {
        AienosCapRef office = {0, 0};
        aienos_cap_office(f->office, &office);
        unsigned long long before = atomic_load(&g_auth_changes);
        f->revoked = aienos_cap_revoke(f->office, office,
                                       (AienosCapRef){f->cap.cap_id, f->cap.generation}) == 0;
        atomic_fetch_add(&g_expected_changes, atomic_load(&g_auth_changes) - before);
    }
    c->out[c->n_out++] = (RxMutation){f->p.target, f->p.field, 0xBADBADull};
    return 0;
}

static void revoked_in_flight(Legacy *L, const char *act, RxObjRef target, Tally *t) {
    RxObject o;
    if (rx_world_read(L->w, target, &o) != RX_OK) { CHECK(0, "%s: target unreadable", act); return; }
    static Inflight f;
    memset(&f, 0, sizeof f);
    f.office = L->office;
    f.cap = L->inflight_belief;
    uint32_t id = 0;
    int add = legacy_reaction_fn(L, "legacy.revoked-in-flight", target, o.resource, f.cap, 1, &f.p,
                                 fn_inflight, &f, &id);
    CHECK(add == RX_OK, "%s: could not register the in-flight reaction (%d)", act, add);
    if (add != RX_OK) return;
    CHECK(wake_legacy(L) == 0, "%s: the in-flight reaction did not settle", act);
    pthread_mutex_lock(&L->w->mu);
    RxReaction rr = L->w->reactions[id];
    const RxCrumb *k = rx_world_crumb(L->w, rr.last_crumb);
    int rejected = k && k->kind == RX_CRUMB_REJECTED;
    pthread_mutex_unlock(&L->w->mu);
    CHECK(f.revoked, "%s: the office could not revoke the in-flight grant", act);
    tally(t, f.revoked && rr.activations >= 1 && rr.commits == 0 && rejected, act,
          "reaction whose real grant is revoked while it runs", (long long)rr.commits);
}
static int native_auth(void *ctx, uint32_t cap_id, uint64_t generation, uint32_t subject,
                       uint64_t resource, uint32_t rights) {
    AienosCapEntry e;
    return aienos_cap_validate(ctx, (AienosCapRef){cap_id, generation}, subject, resource,
                               rights, &e);
}

/* A caller-supplied authority callback that accepts everything. A store bound
 * to the runtime's authority (rx_gen_bind_authority) must ignore it. */
static int permissive_auth(void *ctx, uint32_t cap_id, uint64_t generation, uint32_t subject,
                           uint64_t resource, uint32_t rights) {
    (void)ctx; (void)cap_id; (void)generation; (void)subject; (void)resource; (void)rights;
    return 0;
}

static void fill_random(uint8_t *p, size_t n) {
    size_t k = 0;
    while (k < n) {
        ssize_t g = getrandom(p + k, n - k, 0);
        if (g > 0) k += (size_t)g;
    }
}

/* A LEGACY reaction whose body revokes LEGACY's own caller credential while
 * the activation runs, then writes a target its real grant covers. The commit
 * re-check must refuse the write: the identity the reaction was admitted under
 * is gone. */
typedef struct { Poke p; RxWorld *w; RxCallerCred cred; int revoked; } IdRevoke;
static int fn_id_revoke(RxCtx *c) {
    IdRevoke *f = c->user;
    if (!f->revoked)
        f->revoked = rx_world_revoke_caller(f->w, LEGACY_SUBJ, &f->cred) == RX_CALLER_OK;
    c->out[c->n_out++] = (RxMutation){f->p.target, f->p.field, 0xBADBADull};
    return 0;
}

static const char *caller_err(int rc) {
    switch (rc) {
    case RX_CALLER_OK: return "OK";
    case RX_CALLER_ERR_ABSENT: return "ABSENT";
    case RX_CALLER_ERR_UNKNOWN: return "UNKNOWN";
    case RX_CALLER_ERR_REVOKED: return "REVOKED";
    case RX_CALLER_ERR_STALE: return "STALE";
    case RX_CALLER_ERR_FORGED: return "FORGED";
    case RX_CALLER_ERR_CLOSED: return "CLOSED";
    case RX_CALLER_ERR_EXISTS: return "EXISTS";
    default: return "other";
    }
}
/* One identity probe: rc must equal want. */
static void expect(Tally *t, long long rc, long long want, const char *what) {
    t->tried++;
    if (rc == want) { t->refused++; return; }
    printf("R16 G4 FAIL: C5 identity: %s: rc %lld (%s), expected %lld (%s)\n", what, rc,
           caller_err((int)rc), want, caller_err((int)want));
    g_fail++;
}

static void st32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void st64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static OmegaSharedWorldDesc publication(RxWorld *w, const RxObject *o, RxCapRef cap) {
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof d);
    d.msg_type = RX_RING_PUBLISH;
    d.sequence = rx_world_publication_tail(w);
    d.world_epoch = w->world_epoch;
    d.object_id = o->id;
    d.object_generation = o->generation;
    d.object_length = (uint32_t)o->size_bytes;
    d.payload_len = RX_CAP_PAYLOAD;
    st32(d.payload, cap.cap_id);
    st32(d.payload + 4, (uint32_t)cap.generation);
    st32(d.payload + RX_CAP_GEN_HI_A, (uint32_t)(cap.generation >> 32));
    st64(d.payload + 8, o->version);
    st64(d.payload + 16, rx_world_explain(w, (RxObjRef){o->id, o->generation}, 0));
    rx_world_seal_descriptor(&d);
    return d;
}

int main(void) {
    R15Rig *r = calloc(1, sizeof *r);
    static const uint32_t fixtures[2] = {LEGACY_SUBJ, LEGACY2_SUBJ};
    if (!r || r15_start_with(r, R15_RES4, fixtures, 2) != 0) {
        printf("R16 G4 FAIL: production body did not start (stage %d%s%s)\n", r ? r->stage : 0,
               r && r->stage_why ? ": " : "", r && r->stage_why ? r->stage_why : "");
        printf("R16 gate: R16_G4_LEGACY_REFUSED=FAIL\n");
        return 1;
    }
    aienos_cap_set_observer(r->admin, observer, NULL);

    /* ---- fixtures (harness authority; before the acts) ---- */
    Legacy L;
    memset(&L, 0, sizeof L);
    L.w = &r->w;
    L.view = r->view;
    L.gen = r->gen;
    L.keys = &r->fixtures;
    L.stolen_goal = r->ext_goal;
    L.borrowed_inforce = r->promoter.inforce_write;
    L.borrowed_promote = r->promoter.promotion_authority;
    L.forged = (RxCapRef){777, 3};
    L.own_scratch = r15_mint(r, LEGACY_SUBJ, RES_LEGACY_SCRATCH, RX_RIGHT_READ | RX_RIGHT_WRITE);
    L.own_promote = r15_mint(r, LEGACY_SUBJ, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE);
    L.own2_read_promote = r15_mint(r, LEGACY2_SUBJ, RX_GEN_RES_PROMOTION, RX_RIGHT_READ);
    L.ext_scratch = r15_mint(r, EXTERNAL_SUBJ, RES_LEGACY_SCRATCH, RX_RIGHT_WRITE);
    L.office = r->admin;
    RxObject belief_obj, output_obj;
    int fx0 = rx_world_read(&r->w, r->aien.o.belief, &belief_obj) == RX_OK &&
              rx_world_read(&r->w, r->living.o.output, &output_obj) == RX_OK;
    L.inflight_belief = r15_mint(r, LEGACY_SUBJ, belief_obj.resource, RX_RIGHT_WRITE);
    L.own_output = r15_mint(r, LEGACY_SUBJ, output_obj.resource, RX_RIGHT_WRITE);
    uint64_t init[RX_MAX_FIELDS] = {0};
    int fx = fx0 && rx_world_create(&r->w, 0x7160, RX_PERSIST_EPHEMERAL, RES_LEGACY_SCRATCH, init,
                             &L.scratch) == RX_OK;
    fx = fx && rx_world_create(&r->w, 0x7161, RX_PERSIST_EPHEMERAL, RES_LEGACY_SCRATCH, init,
                               &L.scratch2) == RX_OK;
    RxGenDraft gd;
    memset(&gd, 0, sizeof gd);
    static const uint8_t junk[] = "legacy";
    gd.proofs_ok = 1;
    gd.evidence = junk; gd.evidence_len = sizeof junk - 1;
    gd.realization = junk; gd.realization_len = sizeof junk - 1;
    fx = fx && rx_gen_propose_as(r->gen, LEGACY_SUBJ, rx_caller_find(L.keys, LEGACY_SUBJ), &gd,
                                  &L.candidate) == RX_GEN_OK;
    RxCapRef honest_output = r->seat_output;
    CHECK(fx, "fixtures could not be built");

    Snap s0;
    memset(&s0, 0, sizeof s0);
    watch(&s0, "aien.belief", r->aien.o.belief);
    watch(&s0, "aien.experiment_belief", r->aien.o.experiment_belief);
    watch(&s0, "aien.prediction", r->aien.o.prediction);
    watch(&s0, "aien.plan", r->aien.o.plan);
    watch(&s0, "aien.assessment", r->aien.o.assessment);
    watch(&s0, "omega.selection", r->omega.o.selection);
    watch(&s0, "omega.search", r->omega.o.search);
    watch(&s0, "omega.result", r->omega.o.result);
    watch(&s0, "living.candidate", r->living.o.candidate);
    watch(&s0, "living.promotion", r->living.o.promotion);
    watch(&s0, "living.inforce", r->living.o.inforce);
    watch(&s0, "living.evidence", r->living.o.evidence);
    watch(&s0, "living.output", r->living.o.output);
    CHECK(take(r, &s0) == 0, "baseline snapshot failed");
    Snap s = s0;

    Tally t[7];
    memset(t, 0, sizeof t);

    /* (1) write authoritative AIEN belief */
    write_attempts(&L, "(1) belief", r->aien.o.belief, L.borrowed_inforce,
                   "outside publication with the promoter's in-force grant", &t[1]);
    write_attempts(&L, "(1) experiment belief", r->aien.o.experiment_belief, L.borrowed_inforce,
                   "outside publication with the promoter's in-force grant", &t[1]);
    revoked_in_flight(&L, "(1) belief", r->aien.o.belief, &t[1]);
    CHECK(take(r, &s) == 0 && same(&s0, &s, "(1)"), "(1) authoritative state moved");

    /* (2) select an Omega realization */
    write_attempts(&L, "(2) selection", r->omega.o.selection, L.borrowed_inforce,
                   "outside publication with the promoter's in-force grant", &t[2]);
    CHECK(take(r, &s) == 0 && same(&s0, &s, "(2)"), "(2) authoritative state moved");

    /* (3) mint or revoke authority through the view it holds */
    {
        AienosCapRef office = {0, 0};
        aienos_cap_office(r->admin, &office);   /* a reference value, guessable */
        AienosCapMint m = {3, LEGACY_SUBJ, 0, 0, 0, {UINT32_MAX, 0}, office};
        RxObject bo;
        rx_world_read(&r->w, r->aien.o.belief, &bo);
        m.resource = bo.resource;
        m.rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_MINT;
        uint8_t guessed[32];
        memset(guessed, 0, sizeof guessed);
        int rc = aienos_cap_cognition_mint(L.view, &m, guessed);
        tally(&t[3], rc != 0, "(3) mint", "cognition mint with a guessed token", rc);
        rc = aienos_cap_cognition_mint(L.view, &m, NULL);
        tally(&t[3], rc != 0, "(3) mint", "cognition mint with no token", rc);
        rc = aienos_cap_cognition_admin(L.view, 2, office, (AienosCapRef){r->ext_goal.cap_id,
                                        r->ext_goal.generation});
        tally(&t[3], rc != 0, "(3) mint", "cognition revoke of the external goal grant", rc);
        CHECK(rx_world_validate_cap(&r->w, r->ext_goal, EXTERNAL_SUBJ,
                                    RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE,
                                    NULL) == RX_CAP_OK, "(3) the external goal grant was revoked");
    }
    CHECK(take(r, &s) == 0 && same(&s0, &s, "(3)"), "(3) authoritative state moved");

    /* (4) promote a generation (the legacy draft) */
    {
        typedef struct { uint32_t subject; RxCapRef cap; uint32_t rights; const char *what; } Try;
        Try tries[4] = {
            {LEGACY_SUBJ, L.own_promote, RX_GEN_RIGHT_PROMOTE,
             "self-promotion with a real promotion grant"},
            {LEGACY2_SUBJ, L.borrowed_promote, RX_GEN_RIGHT_PROMOTE,
             "promotion with the promoter's grant, another subject"},
            {LEGACY2_SUBJ, L.forged, RX_GEN_RIGHT_PROMOTE, "promotion with a forged reference"},
            {LEGACY2_SUBJ, L.own2_read_promote, RX_RIGHT_READ,
             "promotion asking a READ right it really holds"},
        };
        for (int i = 0; i < 4; i++) {
            RxPromotionRequest req = {L.candidate, tries[i].subject, tries[i].cap.cap_id,
                                      tries[i].cap.generation, RX_GEN_RES_PROMOTION,
                                      tries[i].rights, {0, {0}}};
            const RxCallerCred *c = rx_caller_find(L.keys, tries[i].subject);
            if (c) req.caller = *c;
            int rc = rx_gen_promote(L.gen, &req, native_auth, (void *)L.view, NULL, NULL, NULL,
                                    NULL);
            tally(&t[4], rc == RX_GEN_ERR_AUTHORITY, "(4) promote", tries[i].what, rc);
        }
    }
    CHECK(take(r, &s) == 0 && same(&s0, &s, "(4)"), "(4) authoritative state moved");

    /* (5) bypass the effect/authority boundary */
    {
        RxObject out;
        CHECK(rx_world_read(&r->w, r->living.o.output, &out) == RX_OK && out.placed,
              "(5) the seat output is not placed");
        uint32_t fault = 0;
        OmegaSharedWorldDesc honest = publication(&r->w, &out, honest_output);
        int hrc = rx_world_check_descriptor(&r->w, &honest, honest.sequence, &fault);
        CHECK(fault != RX_FAULT_CAP, "(5) control: the honest grant was refused (rc %d fault 0x%x)",
              hrc, fault);
        RxCapRef tries[4] = {L.forged, L.own_scratch, L.borrowed_inforce, L.own_output};
        const char *what[4] = {"publication descriptor with a forged reference",
                               "publication descriptor with its own scratch grant",
                               "publication descriptor with the promoter's in-force grant",
                               "publication descriptor with its own real grant on the output"};
        for (int i = 0; i < 4; i++) {
            OmegaSharedWorldDesc d = publication(&r->w, &out, tries[i]);
            fault = 0;
            int rc = rx_world_check_descriptor(&r->w, &d, d.sequence, &fault);
            tally(&t[5], rc == RX_ERR_AUTHORITY && fault == RX_FAULT_CAP, "(5) boundary", what[i],
                  rc);
        }
        /* Overwrite the shared physical record: the canonical object is not
         * moved, and the boundary then refuses the diverged object. */
        OmegaSharedWorldObject phys, forged_phys;
        CHECK(rx_world_physical(&r->w, r->living.o.output, &phys) == RX_OK, "(5) physical");
        forged_phys = phys;
        forged_phys.generation = phys.generation + 7;
        rx_world_overwrite_physical(&r->w, out.id, &forged_phys);
        RxObject after;
        rx_world_read(&r->w, r->living.o.output, &after);
        OmegaSharedWorldDesc d = publication(&r->w, &out, honest_output);
        fault = 0;
        int rc = rx_world_check_descriptor(&r->w, &d, d.sequence, &fault);
        tally(&t[5], after.generation == out.generation && after.version == out.version &&
                     rc != RX_OK && fault == RX_FAULT_DIVERGED,
              "(5) boundary", "overwritten physical record", rc);
        rx_world_overwrite_physical(&r->w, out.id, &phys);   /* put the projection back */
    }
    CHECK(take(r, &s) == 0 && same(&s0, &s, "(5)"), "(5) authoritative state moved");

    /* (6) advance the world generation: the in-force record production reads,
     * and the R9 active generation (checked by the snapshot after every act) */
    write_attempts(&L, "(6) in-force", r->living.o.inforce, L.borrowed_inforce,
                   "outside publication with the promoter's in-force grant", &t[6]);
    write_attempts(&L, "(6) promotion record", r->living.o.promotion, L.borrowed_inforce,
                   "outside publication with the promoter's in-force grant", &t[6]);
    CHECK(take(r, &s) == 0 && same(&s0, &s, "(6)"), "(6) authoritative state moved");

    /* Outside the six: the SEQ reference loop refuses the production world. */
    uint32_t order[1] = {0}, ran = 0;
    RxSeqPlan plan = {order, 1, NULL, NULL, 1000};
    int seq_rc = rx_seq_pulse(&r->w, &plan, &ran);
    pthread_mutex_lock(&r->w.mu);
    int act_rc = rx_world_seq_activate_locked(&r->w, 0, 0);
    pthread_mutex_unlock(&r->w.mu);
    int seq_refused = seq_rc == RX_ERR_ARG && act_rc == RX_ERR_ARG && ran == 0;
    CHECK(seq_refused, "the SEQ reference drove the production world (%d %d)", seq_rc, act_rc);
    CHECK(take(r, &s) == 0 && same(&s0, &s, "SEQ"), "SEQ authoritative state moved");

    /* The body still works after the attempts: one honest production request. */
    uint64_t served0 = r->served;
    int prod = r15_producer_start(r) == 0;
    struct timespec ts = {0, 200000000};
    for (int i = 0; i < 50 && r->served == served0; i++) nanosleep(&ts, NULL);
    prod = r15_producer_stop(r) == 0 && prod && r->served > served0;
    CHECK(prod, "the production body did not serve after the attempts");

    /* (5), last: the office revokes the seat's output grant (production is
     * stopped and served above), then a publication presenting the writer of
     * record's own, now stale, grant must be refused at the boundary. */
    {
        RxObject out;
        CHECK(rx_world_read(&r->w, r->living.o.output, &out) == RX_OK, "(5) output unreadable");
        Snap sb = s0, sa;
        CHECK(take(r, &sb) == 0, "(5) snapshot before the revocation failed");
        unsigned long long e0 = atomic_load(&g_expected_changes);
        AienosCapRef office = {0, 0};
        aienos_cap_office(r->admin, &office);
        unsigned long long before = atomic_load(&g_auth_changes);
        int rv = aienos_cap_revoke(r->admin, office,
                                   (AienosCapRef){honest_output.cap_id, honest_output.generation});
        atomic_fetch_add(&g_expected_changes, atomic_load(&g_auth_changes) - before);
        CHECK(rv == 0, "(5) the office could not revoke the seat output grant (%d)", rv);
        OmegaSharedWorldDesc d = publication(&r->w, &out, honest_output);
        uint32_t fault = 0;
        int rc = rx_world_check_descriptor(&r->w, &d, d.sequence, &fault);
        tally(&t[5], rv == 0 && rc == RX_ERR_AUTHORITY && fault == RX_FAULT_CAP, "(5) boundary",
              "publication descriptor with the writer's revoked grant", rc);
        sa = sb;
        CHECK(take(r, &sa) == 0 && same_x(&sb, &sa, "(5) revoked", atomic_load(&g_expected_changes) - e0),
              "(5) authoritative state moved");
    }

    /* (4), last: promotion naming the promoter itself as the subject, with the
     * promoter's grant reference (which the legacy context can observe). This
     * is the pre-fix exploit: before R16 C5 the request's subject was a value
     * the caller supplied, and this promotion was accepted (generation moved in
     * memory and on disk). Now the request must also carry the promoter's
     * runtime-issued caller credential, which the legacy context does not hold;
     * every way it could fill that field is tried. Run last among the acts
     * because an accepted promotion moves the generation. A fresh draft is
     * proposed so the parent is current. */
    int probe_refused = 0;
    uint64_t probe_cand = 0;
    {
        const RxCallerCred *own = rx_caller_find(L.keys, LEGACY_SUBJ);
        const RxCallerCred *prom = rx_caller_find(&r->keys_promoter, RX_LIVING_PROMOTE_SUBJ);
        RxCallerCred zero, rnd, relabel, mixed;
        memset(&zero, 0, sizeof zero);
        zero.generation = prom->generation;           /* generations are guessable */
        rnd = zero;
        fill_random(rnd.secret, sizeof rnd.secret);
        relabel = *own;                               /* its own real credential */
        mixed = *own;
        mixed.generation = prom->generation;          /* its own secret, promoter's generation */
        struct { const RxCallerCred *c; RxGenAuthFn auth; const char *what; } v[6] = {
            {NULL, native_auth, "no credential"},
            {&zero, native_auth, "zero secret at the promoter's generation"},
            {&rnd, native_auth, "random secret at the promoter's generation"},
            {&relabel, native_auth, "its own real credential (LEGACY's)"},
            {&mixed, native_auth, "its own secret at the promoter's generation"},
            {&rnd, permissive_auth, "random secret with a permissive authority callback"},
        };
        uint64_t a0 = 0, l0 = 0, a1 = 0, l1 = 0;
        RxRecoveryRecord d0, d1;
        memset(&d0, 0, sizeof d0); memset(&d1, 0, sizeof d1);
        int prc = rx_gen_propose_as(L.gen, LEGACY_SUBJ, own, &gd, &probe_cand);
        CHECK(prc == RX_GEN_OK, "(4) the probe draft could not be proposed (%d)", prc);
        rx_gen_active(L.gen, &a0, &l0);
        rx_gen_recover(r->generation_dir, &d0);
        int refused = 0, last_rc = 0;
        for (int i = 0; i < 6; i++) {
            RxPromotionRequest req = {probe_cand, RX_LIVING_PROMOTE_SUBJ, L.borrowed_promote.cap_id,
                                      L.borrowed_promote.generation, RX_GEN_RES_PROMOTION,
                                      RX_GEN_RIGHT_PROMOTE, {0, {0}}};
            if (v[i].c) req.caller = *v[i].c;
            int rc = rx_gen_promote(L.gen, &req, v[i].auth, (void *)L.view, NULL, NULL, NULL, NULL);
            rx_caller_wipe(&req.caller);
            if (rc == RX_GEN_ERR_IDENTITY) refused++;
            else printf("R16 G4 FAIL: (4) promoter-subject promotion, %s: rc %d (expected "
                        "IDENTITY %d)\n", v[i].what, rc, RX_GEN_ERR_IDENTITY);
            if (rc != RX_GEN_ERR_IDENTITY || i == 0) last_rc = rc;
        }
        rx_gen_active(L.gen, &a1, &l1);
        rx_gen_recover(r->generation_dir, &d1);
        probe_refused = prc == RX_GEN_OK && refused == 6 && a1 == a0 && l1 == l0 &&
                        d1.active_id == d0.active_id && d1.lineage == d0.lineage;
        printf("R16 G4 (4) promotion naming the promoter as subject, with the promoter's grant: "
               "%s (%d/6 credential variants refused with IDENTITY; rc %d; generation %llu -> %llu "
               "in memory, %llu -> %llu on disk)\n",
               probe_refused ? "REFUSED" : "ACCEPTED", refused, last_rc, U(a0), U(a1),
               U(d0.active_id), U(d1.active_id));
        if (!probe_refused)
            printf("R16 G4 OPEN: promotion accepts the subject the caller names; any in-process "
                   "holder of the promoter's grant reference can promote.\n");
    }

    /* (6), last: the same exploit on the reaction path. A legacy reaction whose
     * descriptor names the promoter as its subject and carries the promoter's
     * in-force grant. Before R16 C5 validate_caps checked the grant against the
     * subject the descriptor names and this write committed. Now registration
     * needs the named subject's credential; each variant the legacy context
     * could present is tried, and any that registers is woken to show its
     * effect. */
    int rprobe_refused = 0;
    {
        RxObject b0, b1;
        rx_world_read(&r->w, r->living.o.inforce, &b0);
        const RxCallerCred *own = rx_caller_find(L.keys, LEGACY_SUBJ);
        const RxCallerCred *prom = rx_caller_find(&r->keys_promoter, RX_LIVING_PROMOTE_SUBJ);
        RxCallerCred rnd;
        rnd.generation = prom->generation;
        fill_random(rnd.secret, sizeof rnd.secret);
        const RxCallerCred *v[3] = {NULL, own, &rnd};
        const char *vw[3] = {"no credential", "its own real credential",
                             "random secret at the promoter's generation"};
        /* Wake: a harness fixture grant lets the promoter subject read the
         * legacy scratch object, so the probe can be woken on demand (in normal
         * operation a candidate trigger with the promoter's candidate grant
         * would wake it). The grant under test is the borrowed in-force one. */
        g_named_trigger = (RxDep){L.scratch, RX_FIELD(0)};
        g_named_read = (RxCapNeed){r15_mint(r, RX_LIVING_PROMOTE_SUBJ, RES_LEGACY_SCRATCH,
                                            RX_RIGHT_READ), RES_LEGACY_SCRATCH, RX_RIGHT_READ};
        static Poke p_named[3];
        uint32_t id_n[3] = {0, 0, 0};
        int add[3], refused = 0, any_added = 0;
        for (int i = 0; i < 3; i++) {
            g_named_subject = RX_LIVING_PROMOTE_SUBJ;
            g_named_cred = v[i];
            g_named_explicit = 1;
            add[i] = legacy_reaction(&L, "legacy.named-subject", r->living.o.inforce, b0.resource,
                                     L.borrowed_inforce, 1, &p_named[i], &id_n[i]);
            g_named_subject = 0;
            g_named_explicit = 0;
            g_named_cred = NULL;
            if (add[i] == RX_ERR_IDENTITY) refused++;
            else printf("R16 G4 FAIL: (6) promoter-subject reaction, %s: register rc %d "
                        "(expected IDENTITY %d)\n", vw[i], add[i], RX_ERR_IDENTITY);
            if (add[i] == RX_OK) any_added = 1;
        }
        int woke = -1;
        uint64_t commits = 0, acts = 0;
        int crumb = -1;
        if (any_added) {
            woke = wake_legacy(&L);
            pthread_mutex_lock(&r->w.mu);
            for (int i = 0; i < 3; i++) {
                if (add[i] != RX_OK) continue;
                commits += r->w.reactions[id_n[i]].commits;
                acts += r->w.reactions[id_n[i]].activations;
                const RxCrumb *k = rx_world_crumb(&r->w, r->w.reactions[id_n[i]].last_crumb);
                crumb = k ? (int)k->kind : -1;
            }
            pthread_mutex_unlock(&r->w.mu);
        }
        rx_world_read(&r->w, r->living.o.inforce, &b1);
        int moved = commits > 0 || b1.field[0] == 0xBADBADull || b1.version != b0.version;
        rprobe_refused = refused == 3 && !moved;
        printf("R16 G4 (6) reaction naming the promoter as subject, with the promoter's in-force "
               "grant: %s (%d/3 credential variants refused at registration with IDENTITY; woken "
               "%s; activations %llu, commits %llu, last crumb %d; in-force field0 0x%llx)\n",
               rprobe_refused ? "REFUSED" : moved ? "ACCEPTED" : "NOT REFUSED AT REGISTRATION",
               refused, woke == 0 ? "yes" : "no", U(acts), U(commits), crumb, U(b1.field[0]));
        if (!rprobe_refused)
            printf("R16 G4 OPEN: a reaction's subject is whatever its descriptor names; any "
                   "in-process holder of a writer's grant reference can write as that writer.\n");
    }

    /* ---- R16 C5: the caller credential itself (hostile probes) ----------- */
    Tally tc = {0, 0};
    {
        RxWorld *w = &r->w;
        const RxCallerCred *own = rx_caller_find(L.keys, LEGACY_SUBJ);
        const RxCallerCred *own2 = rx_caller_find(L.keys, LEGACY2_SUBJ);
        const RxCallerCred *prom = rx_caller_find(&r->keys_promoter, RX_LIVING_PROMOTE_SUBJ);
        RxCallerCred x;
        /* spoofed caller: every credential it could present for the promoter */
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, NULL), RX_CALLER_ERR_ABSENT,
               "promoter subject, no credential");
        memset(&x, 0, sizeof x);
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, &x), RX_CALLER_ERR_ABSENT,
               "promoter subject, all-zero credential");
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, own), RX_CALLER_ERR_STALE,
               "promoter subject, LEGACY's real credential");
        x = *own; x.generation = prom->generation;
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, &x), RX_CALLER_ERR_FORGED,
               "promoter subject, LEGACY's secret at the promoter's generation");
        x = *prom; x.secret[0] ^= 1;
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, &x), RX_CALLER_ERR_FORGED,
               "promoter subject, promoter's secret with one bit flipped");
        x = *prom; x.generation += 1000;
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, &x), RX_CALLER_ERR_STALE,
               "promoter subject, promoter's secret at another generation");
        expect(&tc, rx_world_check_caller(w, 999, own), RX_CALLER_ERR_UNKNOWN,
               "never-enrolled subject");
        expect(&tc, rx_world_check_caller(w, LEGACY_SUBJ, own), RX_CALLER_OK,
               "control: LEGACY's own credential for LEGACY");
        /* enrollment is closed once the world is bound */
        expect(&tc, rx_world_enroll_caller(w, 170, &x), RX_CALLER_ERR_CLOSED, "enroll after bind");
        expect(&tc, x.generation, 0, "enroll after bind hands out no credential");
        expect(&tc, rx_world_enroll_caller(w, RX_LIVING_PROMOTE_SUBJ, &x), RX_CALLER_ERR_CLOSED,
               "re-enroll the promoter after bind");
        /* revocation needs the credential itself */
        expect(&tc, rx_world_revoke_caller(w, RX_LIVING_PROMOTE_SUBJ, own), RX_CALLER_ERR_STALE,
               "revoke the promoter with LEGACY's credential");
        expect(&tc, rx_world_revoke_caller(w, RX_LIVING_PROMOTE_SUBJ, NULL), RX_CALLER_ERR_ABSENT,
               "revoke the promoter with no credential");
        expect(&tc, rx_world_check_caller(w, RX_LIVING_PROMOTE_SUBJ, prom), RX_CALLER_OK,
               "promoter still enrolled after the revoke attempts");
        /* R9 proposal: the proposer is checked as well */
        uint64_t c2 = 0;
        expect(&tc, rx_gen_propose_as(L.gen, RX_LIVING_PREPARE_SUBJ, NULL, &gd, &c2),
               RX_GEN_ERR_IDENTITY, "propose as the prepare subject with no credential");
        expect(&tc, rx_gen_propose_as(L.gen, RX_LIVING_PREPARE_SUBJ, own, &gd, &c2),
               RX_GEN_ERR_IDENTITY, "propose as the prepare subject with LEGACY's credential");
        expect(&tc, rx_gen_propose(L.gen, LEGACY_SUBJ, &gd, &c2), RX_GEN_ERR_IDENTITY,
               "legacy rx_gen_propose (no credential) as LEGACY");
        /* A bound store ignores a permissive authority callback: LEGACY2 with
         * its real credential and a forged grant is refused by the native
         * authority. */
        uint64_t a0 = 0, l0 = 0, a1 = 0, l1 = 0;
        rx_gen_active(L.gen, &a0, &l0);
        RxPromotionRequest req = {probe_cand, LEGACY2_SUBJ, L.forged.cap_id, L.forged.generation,
                                  RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, {0, {0}}};
        req.caller = *own2;
        expect(&tc, rx_gen_promote(L.gen, &req, permissive_auth, NULL, NULL, NULL, NULL, NULL),
               RX_GEN_ERR_AUTHORITY, "permissive authority callback on a bound store");
        rx_gen_active(L.gen, &a1, &l1);
        expect(&tc, a1 == a0 && l1 == l0, 1, "generation unchanged by the permissive callback");
        rx_caller_wipe(&req.caller);

        /* collision: every credential the rig issued is distinct, and none
         * validates for another subject */
        const RxCallerKeyring *rings[6] = {&r->keys_omega, &r->keys_aien, &r->keys_aegis,
                                           &r->keys_living, &r->keys_promoter, &r->fixtures};
        uint32_t subj[32]; const RxCallerCred *cred[32]; uint32_t n = 0;
        for (int k = 0; k < 6; k++)
            for (uint32_t i = 0; i < rings[k]->n && n < 32; i++) {
                subj[n] = rings[k]->subject[i]; cred[n++] = &rings[k]->cred[i];
            }
        uint32_t dup = 0, cross_ok = 0, cross = 0;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < n; j++) {
                if (i == j) continue;
                if (j > i && memcmp(cred[i]->secret, cred[j]->secret, RX_CALLER_SECRET_LEN) == 0) dup++;
                x = *cred[i]; x.generation = cred[j]->generation;
                cross++;
                cross_ok += rx_world_check_caller(w, subj[j], cred[i]) != RX_CALLER_OK &&
                            rx_world_check_caller(w, subj[j], &x) == RX_CALLER_ERR_FORGED;
            }
        expect(&tc, dup, 0, "duplicate secrets among the rig's credentials");
        expect(&tc, cross_ok, cross, "rig credentials cross-validating for another subject");
        /* collision at capacity, on a scratch world */
        RxWorld *sw = calloc(1, sizeof *sw);
        int sw_ok = sw && rx_world_init_native(sw, r->view, 1, 1024) == RX_OK;
        expect(&tc, sw_ok, 1, "scratch world for the capacity collision probe");
        if (sw_ok) {
            static RxCallerCred many[RX_CALLER_MAX];
            uint32_t enrolled = 0, sdup = 0, scross = 0, sbad = 0;
            for (uint32_t i = 0; i < RX_CALLER_MAX; i++)
                enrolled += rx_world_enroll_caller(sw, 1000 + i, &many[i]) == RX_CALLER_OK;
            expect(&tc, enrolled, RX_CALLER_MAX, "enroll to capacity");
            expect(&tc, rx_world_enroll_caller(sw, 5000, &x), RX_ERR_FULL, "enroll past capacity");
            expect(&tc, rx_world_enroll_caller(sw, 1000, &x), RX_CALLER_ERR_EXISTS,
                   "enroll a subject twice");
            for (uint32_t i = 0; i < RX_CALLER_MAX; i++)
                for (uint32_t j = 0; j < RX_CALLER_MAX; j++) {
                    if (i == j) {
                        sbad += rx_world_check_caller(sw, 1000 + i, &many[i]) != RX_CALLER_OK;
                        continue;
                    }
                    if (j > i && memcmp(many[i].secret, many[j].secret, RX_CALLER_SECRET_LEN) == 0) sdup++;
                    x = many[i]; x.generation = many[j].generation;
                    scross += rx_world_check_caller(sw, 1000 + j, &x) == RX_CALLER_ERR_FORGED;
                }
            expect(&tc, sdup, 0, "duplicate secrets among 64 enrollments");
            expect(&tc, sbad, 0, "a credential not valid for its own subject");
            expect(&tc, scross, RX_CALLER_MAX * (RX_CALLER_MAX - 1),
                   "cross-subject checks refused as FORGED");
            rx_world_bind_callers(sw);
            expect(&tc, rx_world_enroll_caller(sw, 6000, &x), RX_CALLER_ERR_CLOSED,
                   "scratch world: enroll after bind");
            for (uint32_t i = 0; i < RX_CALLER_MAX; i++) rx_caller_wipe(&many[i]);
            rx_world_destroy(sw);
        }
        free(sw);
    }

    /* ---- R16 C5: revocation, and the credential replayed after it --------- */
    {
        RxWorld *w = &r->w;
        const RxCallerCred *own = rx_caller_find(L.keys, LEGACY_SUBJ);
        const RxCallerCred *own2 = rx_caller_find(L.keys, LEGACY2_SUBJ);
        RxObject s2a, s2b;
        rx_world_read(w, L.scratch2, &s2a);
        /* LEGACY's identity is revoked while its admitted reaction runs; the
         * write its real grant covers must not commit. */
        static IdRevoke f;
        memset(&f, 0, sizeof f);
        f.w = w;
        f.cred = *own;
        uint32_t id = 0;
        int add = legacy_reaction_fn(&L, "legacy.identity-revoked-in-flight", L.scratch2,
                                     RES_LEGACY_SCRATCH, L.own_scratch, 1, &f.p, fn_id_revoke, &f,
                                     &id);
        expect(&tc, add, RX_OK, "register LEGACY's reaction under its own credential");
        if (add == RX_OK) {
            CHECK(wake_legacy(&L) == 0, "C5: the in-flight identity reaction did not settle");
            pthread_mutex_lock(&w->mu);
            RxReaction rr = w->reactions[id];
            const RxCrumb *k = rx_world_crumb(w, rr.last_crumb);
            int rejected = k && k->kind == RX_CRUMB_REJECTED;
            pthread_mutex_unlock(&w->mu);
            expect(&tc, f.revoked, 1, "LEGACY revoked its own identity in flight");
            expect(&tc, rr.activations >= 1 && rr.commits == 0 && rejected, 1,
                   "write of a reaction whose identity was revoked in flight");
            /* woken again: blocked at activation */
            CHECK(wake_legacy(&L) == 0, "C5: the revoked reaction did not settle");
            pthread_mutex_lock(&w->mu);
            rr = w->reactions[id];
            k = rx_world_crumb(w, rr.last_crumb);
            int blocked = k && k->kind == RX_CRUMB_BLOCKED_AUTHORITY;
            pthread_mutex_unlock(&w->mu);
            expect(&tc, rr.commits == 0 && blocked, 1,
                   "activation of a reaction whose identity is revoked");
        }
        rx_world_read(w, L.scratch2, &s2b);
        expect(&tc, s2b.version == s2a.version && s2b.field[0] == s2a.field[0], 1,
               "scratch target unchanged");
        /* replay: the revoked credential opens nothing */
        expect(&tc, rx_world_check_caller(w, LEGACY_SUBJ, own), RX_CALLER_ERR_REVOKED,
               "revoked credential replayed at the check");
        expect(&tc, rx_world_revoke_caller(w, LEGACY_SUBJ, own), RX_CALLER_ERR_REVOKED,
               "revoked credential replayed at revocation");
        uint64_t c3 = 0;
        expect(&tc, rx_gen_propose_as(L.gen, LEGACY_SUBJ, own, &gd, &c3), RX_GEN_ERR_IDENTITY,
               "revoked credential replayed at proposal");
        RxPromotionRequest req = {probe_cand, LEGACY_SUBJ, L.own_promote.cap_id,
                                  L.own_promote.generation, RX_GEN_RES_PROMOTION,
                                  RX_GEN_RIGHT_PROMOTE, {0, {0}}};
        req.caller = *own;
        expect(&tc, rx_gen_promote(L.gen, &req, native_auth, (void *)L.view, NULL, NULL, NULL,
                                   NULL), RX_GEN_ERR_IDENTITY,
               "revoked credential replayed at promotion");
        rx_caller_wipe(&req.caller);
        static Poke p_replay;
        uint32_t id_r = 0;
        expect(&tc, legacy_reaction(&L, "legacy.replay", L.scratch2, RES_LEGACY_SCRATCH,
                                    L.own_scratch, 1, &p_replay, &id_r), RX_ERR_IDENTITY,
               "revoked credential replayed at registration");
        expect(&tc, rx_world_check_caller(w, LEGACY2_SUBJ, own2), RX_CALLER_OK,
               "another subject's enrollment unaffected");
        rx_caller_wipe(&f.cred);
    }
    int ident_ok = tc.tried > 0 && tc.refused == tc.tried;
    printf("R16 G4 C5 identity probes: %u/%u as expected  %s\n", tc.refused, tc.tried,
           ident_ok ? "PASS" : "FAIL");

    /* ---- positive control: promotion is not disabled ----------------------- */
    int control_ok = 0;
    {
        uint64_t cand = 0, a0 = 0, l0 = 0, a1 = 0, l1 = 0;
        int prc = rx_gen_propose_as(r->gen, RX_LIVING_PREPARE_SUBJ,
                                    rx_caller_find(&r->keys_living, RX_LIVING_PREPARE_SUBJ), &gd,
                                    &cand);
        rx_gen_active(r->gen, &a0, &l0);
        RxPromotionRequest req = {cand, RX_LIVING_PROMOTE_SUBJ,
                                  r->promoter.promotion_authority.cap_id,
                                  r->promoter.promotion_authority.generation,
                                  RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, {0, {0}}};
        req.caller = *rx_caller_find(&r->keys_promoter, RX_LIVING_PROMOTE_SUBJ);
        int rc = prc == RX_GEN_OK
            ? rx_gen_promote(r->gen, &req, NULL, NULL, NULL, NULL, NULL, NULL) : prc;
        rx_caller_wipe(&req.caller);
        rx_gen_active(r->gen, &a1, &l1);
        RxRecoveryRecord d1;
        memset(&d1, 0, sizeof d1);
        rx_gen_recover(r->generation_dir, &d1);
        control_ok = rc == RX_GEN_OK && a1 == cand && a1 != a0 && d1.active_id == cand;
        printf("R16 G4 control: the promoter, with its own credential and grant, promotes: %s "
               "(propose rc %d, promote rc %d; generation %llu -> %llu, on disk %llu)\n",
               control_ok ? "yes" : "NO", prc, rc, U(a0), U(a1), U(d1.active_id));
    }

    int acts_ok = 0;
    const char *names[7] = {"", "belief", "selection", "mint", "promote", "boundary", "generation"};
    for (int a = 1; a <= 6; a++) {
        int ok = t[a].tried > 0 && t[a].refused == t[a].tried;
        acts_ok += ok;
        printf("R16 G4 (%d) %-10s %u/%u attempts refused  %s\n", a, names[a], t[a].refused,
               t[a].tried, ok ? "REFUSED" : "NOT REFUSED");
    }
    printf("R16 G4 SEQ reference on a production world: %s\n", seq_refused ? "refused" : "RAN");
    printf("R16 G4 authoritative state unchanged: %s; AIENOS state changes other than the "
           "harness office's own revocations (%llu): %s; production served after: %s\n",
           g_fail ? "see failures" : "yes", U(atomic_load(&g_expected_changes)),
           g_fail ? "see failures" : "none", prod ? "yes" : "no");
    r15_stop(r);
    free(r);
    int core = acts_ok == 6 && g_fail == 0;
    int pass = core && probe_refused && rprobe_refused && ident_ok && control_ok;
    printf("R16 G4 acts refused: %d/6\n", acts_ok);
    printf("R16 G4 core: %s\n", core ? "six acts refused, state unchanged, for the counted per-act attempts" : "FAIL");
    printf("R16 G4 promoter-subject exploit (4)+(6): %s; C5 identity probes: %s; promotion control: %s\n",
           probe_refused && rprobe_refused ? "refused" : "ACCEPTED", ident_ok ? "pass" : "FAIL",
           control_ok ? "promotes" : "FAIL");
    printf("R16 gate: R16_G4_LEGACY_REFUSED=%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
