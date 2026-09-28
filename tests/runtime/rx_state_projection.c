/*
 * OMEGA_STATE_PROJECTION: cognition gets a compiled state projection, not
 * everything Cortex knows.
 *
 * Part A. Five representative AIEN operations (explain a regression, verify
 * a cost claim, plan the next experiment, a what-if on a hypothetical
 * branch, assess a goal with uncertainty), 24 planted-answer histories each.
 * Every task runs twice with the same consumer: on the whole store and on
 * the projection Omega compiles from the task's CognitiveNeed. Both are
 * scored against the planted answer. Measured: bytes, objects, retrieval,
 * consumer work, latency, recall precision and the unnecessary-state ratio.
 *
 * Controls that must fail: remove any one projected item (leave-one-out),
 * a naive "newest objects" projection of the same size, a "whole subject"
 * projection, and under-specified needs. Protected state asked to be
 * summarized, derived or made ephemeral must be refused, for each of the
 * five protections. A budget below the pinned state must be refused.
 *
 * Recoverability: every summary, reference and derived feature is checked
 * back against Cortex; every excluded object is classified with one reason
 * and checked intact; a tampered source must be caught.
 *
 * Part B. The real R11 AIEN faculty (rx_aien.o, native AIENOS authority) is
 * fed one regime's whole lifetime history, and then only the projection for
 * "why did this prediction fail". Both are scored against the planted cause.
 *
 * Part C. Scale: the same explain operation over 16, 64 and 256 subsystems.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_aien.h"
#include "runtime/rx_cortex.h"
#include "runtime/rx_omega.h"   /* demand and selection object types only */
#include "runtime/rx_projection.h"
#include "runtime/rx_world.h"
#include "rx_sp_workloads.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

static int g_checks;
static int g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define SEEDS 24u
#define SUBJECTS 64u
#define T_END 5990u

static int ans_eq(const SpAnswer *a, const SpAnswer *b) { return memcmp(a, b, sizeof *a) == 0; }

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t median(const uint64_t *v, uint32_t n) {
    uint64_t tmp[256];
    if (!n || n > 256) return 0;
    memcpy(tmp, v, n * sizeof(uint64_t));
    qsort(tmp, n, sizeof(uint64_t), cmp_u64);
    return tmp[n / 2];
}

/* Reason three times; keep the fastest wall time and the first trace. */
static uint64_t reason_timed(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    uint64_t best = UINT64_MAX;
    for (int k = 0; k < 3; k++) {
        SpTrace dummy = { 0, 0, NULL };
        uint64_t t0 = now_ns();
        sp_reason(t, v, out, k == 0 ? tr : &dummy);
        uint64_t d = now_ns() - t0;
        if (d < best) best = d;
    }
    return best;
}

/* ---- Part A ---- */

typedef struct {
    uint32_t tasks, full_ok, proj_ok, agree;
    uint64_t full_bytes[SEEDS], proj_bytes[SEEDS], full_objects[SEEDS], proj_objects[SEEDS];
    uint64_t full_touched[SEEDS], proj_touched[SEEDS], full_words[SEEDS], proj_words[SEEDS];
    uint64_t examined[SEEDS], compile_ns[SEEDS], proj_build_ns[SEEDS], full_build_ns[SEEDS];
    uint64_t full_reason_ns[SEEDS], proj_reason_ns[SEEDS], full_total_ns[SEEDS], proj_total_ns[SEEDS];
    uint64_t full_used, full_provided, proj_used, proj_provided;
    uint64_t needed, needed_present;
    uint64_t loo_items, loo_necessary, tasks_with_necessary;
    uint32_t recentk_ok, subject_ok, underspec_ok, underspec_tasks;
    uint64_t recover_checked, recover_fail, tamper_caught, tamper_tried;
    uint64_t audit_ok, excluded, excluded_intact, other_branch;
    uint64_t by_why[PJ_WHY_END];
    uint64_t by_treatment[PJ_STATE_CLASS_END];
    uint32_t e2e_faster;
} WStats;

static WStats g_w[W_COUNT];
static uint32_t g_protect_refusals, g_protect_attempts;
static int g_budget_refused, g_branch_refused, g_chain_ok = 1, g_recall_ok, g_ephemeral_ok;

static int needed_present(const SpTask *t, const CxStore *s, const StateProjection *p, uint64_t id) {
    for (uint32_t i = 0; i < p->n; i++)
        if (p->e[i].id == id) return 1;
    const CxObject *o = cx_get(s, id);
    for (uint32_t i = 0; o && i < p->nf; i++) {
        const PjFeature *f = &p->f[i];
        if (f->subject == o->subject && f->cls == o->cls && f->kind == o->kind && f->branch == o->branch &&
            o->t >= f->t0 && o->t <= f->t1)
            return 1;
    }
    (void)t;
    return 0;
}

/* Remove one item (or one feature) and see whether the answer survives. */
static void leave_one_out(const SpTask *t, const PjView *v, WStats *w) {
    PjView m = *v;
    PjItem *items = malloc((v->n ? v->n : 1) * sizeof(PjItem));
    PjFeature *feat = malloc((v->nf ? v->nf : 1) * sizeof(PjFeature));
    uint64_t necessary = 0;
    for (uint32_t i = 0; i < v->n + v->nf; i++) {
        if (i < v->n) {
            memcpy(items, v->items, i * sizeof(PjItem));
            memcpy(items + i, v->items + i + 1, (v->n - i - 1) * sizeof(PjItem));
            m.items = items;
            m.n = v->n - 1;
            m.feat = v->feat;
            m.nf = v->nf;
        } else {
            uint32_t j = i - v->n;
            memcpy(feat, v->feat, j * sizeof(PjFeature));
            memcpy(feat + j, v->feat + j + 1, (v->nf - j - 1) * sizeof(PjFeature));
            m.items = v->items;
            m.n = v->n;
            m.feat = feat;
            m.nf = v->nf - 1;
        }
        SpAnswer a;
        SpTrace tr = { 0, 0, NULL };
        sp_reason(t, &m, &a, &tr);
        if (!ans_eq(&a, &t->truth)) necessary++;
    }
    w->loo_items += v->n + v->nf;
    w->loo_necessary += necessary;
    if (necessary) w->tasks_with_necessary++;
    free(items);
    free(feat);
}

static const CxStore *g_sort_store;

static int cmp_newest(const void *a, const void *b) {
    const CxObject *x = cx_get(g_sort_store, *(const uint64_t *)a), *y = cx_get(g_sort_store, *(const uint64_t *)b);
    if (x->t != y->t) return x->t > y->t ? -1 : 1;
    return x->id > y->id ? -1 : 1;
}

/* Naive baseline 1: the newest objects in Cortex, as many bytes as the projection. */
static int naive_recent(const SpTask *t, CxStore *s, uint64_t budget) {
    uint64_t *ids = malloc(s->n * sizeof(uint64_t));
    for (uint64_t i = 0; i < s->n; i++) ids[i] = i + 1;
    g_sort_store = s;
    qsort(ids, s->n, sizeof(uint64_t), cmp_newest);
    uint64_t bytes = 0;
    uint32_t k = 0;
    while (k < s->n && bytes < budget) {
        const CxObject *o = cx_get(s, ids[k++]);
        bytes += PJ_HEADER_BYTES + (uint64_t)o->n * PJ_WORD_BYTES;
    }
    PjView v;
    pj_view_ids(s, ids, k, &v);
    SpAnswer a;
    SpTrace tr = { 0, 0, NULL };
    sp_reason(t, &v, &a, &tr);
    pj_view_free(&v);
    free(ids);
    return ans_eq(&a, &t->truth);
}

/* Naive baseline 2: everything Cortex has about the subject, all time, in full. */
static int naive_subject(const SpTask *t, CxStore *s) {
    const CxIdList *l = &s->by_subject[t->subject];
    PjView v;
    pj_view_ids(s, l->ids, l->n, &v);
    SpAnswer a;
    SpTrace tr = { 0, 0, NULL };
    sp_reason(t, &v, &a, &tr);
    pj_view_free(&v);
    return ans_eq(&a, &t->truth);
}

/* An under-specified need: the operation's need with one requirement weakened. */
static int underspecified(const SpTask *t, CxStore *s, int *applicable) {
    CognitiveNeed n;
    sp_need(t, &n);
    *applicable = 1;
    if (t->workload == W_EXPLAIN) {
        for (uint32_t i = 0; i < n.n_fact_types; i++)
            if (n.required_fact_types[i].world == PJ_W_DEPENDENCIES) n.required_fact_types[i].world = PJ_W_SELF;
    } else if (t->workload == W_VERIFY) {
        n.evidence_requirement = PJ_E_REFS;      /* the receipt's body is not there to check */
    } else if (t->workload == W_PLAN) {
        n.memory_scope = PJ_M_WORKING;           /* forgets experiments before the window */
        n.required_temporal_scope.t0 = t->t_now - 1000;
    } else if (t->workload == W_BRANCH) {
        /* asks on the main line: the branch's hypothetical change is not there */
        n.branch = 0;
        uint32_t k = 0;
        for (uint32_t i = 0; i < n.n_fact_types; i++)
            if (n.required_fact_types[i].treatment != PJ_BRANCH_LOCAL) n.required_fact_types[k++] = n.required_fact_types[i];
        n.n_fact_types = k;
    } else {
        /* W_ASSESS: the whole lifetime instead of the current generation */
        n.required_temporal_scope.anchor = PJ_A_FIXED;
        n.required_temporal_scope.t0 = 0;
    }
    StateProjection p;
    if (pj_compile(s, &n, &p) != PJ_OK) return 0;
    PjView v;
    pj_view_projection(s, &p, &v);
    SpAnswer a;
    SpTrace tr = { 0, 0, NULL };
    sp_reason(t, &v, &a, &tr);
    pj_view_free(&v);
    pj_free(&p);
    return ans_eq(&a, &t->truth);
}

/* Ask for protected state to be summarized, derived or made ephemeral. */
static void protected_refusals(CxStore *s, const SpTask *t) {
    static const struct { uint32_t cls, kind, world; } prot[5] = {
        { CX_EVIDENCE, K_AUTHORITY, PJ_W_SELF },     /* authority */
        { CX_EXECUTION, K_COMMIT, PJ_W_SELF },       /* commit receipt */
        { CX_EVIDENCE, K_VERIFY, PJ_W_SELF },        /* verification evidence */
        { CX_ENTITY, K_GENERATION, PJ_W_MACHINE },   /* generation identity */
        { CX_EXECUTION, K_EFFECT, PJ_W_SELF },       /* effect receipt */
    };
    static const uint32_t bad[3] = { PJ_COMPRESSIBLE, PJ_DERIVED, PJ_EPHEMERAL };
    for (uint32_t i = 0; i < 5; i++)
        for (uint32_t k = 0; k < 3; k++) {
            CognitiveNeed n;
            pj_need_init(&n, PJ_OP_EXPLAIN, t->subject, t->t_now);
            n.memory_scope = PJ_M_EPISODIC;
            pj_need_add(&n, prot[i].cls, prot[i].kind, bad[k], prot[i].world, PJ_T_ALL);
            StateProjection p;
            int rc = pj_compile(s, &n, &p);
            g_protect_attempts++;
            if (rc == PJ_ERR_PROTECTED && p.n == 0 && p.e == NULL) g_protect_refusals++;
            else pj_free(&p);
            CHECK(rc == PJ_ERR_PROTECTED, "protected class %u kind %u as %s was not refused (rc %d)",
                  prot[i].cls, prot[i].kind, pj_state_class_name(bad[k]), rc);
        }
    /* A reference keeps the exact digest: allowed. */
    CognitiveNeed n;
    pj_need_init(&n, PJ_OP_EXPLAIN, t->subject, t->t_now);
    pj_need_add(&n, CX_EVIDENCE, K_VERIFY, PJ_REFERENCE_ONLY, PJ_W_SELF, PJ_T_ALL);
    StateProjection p;
    CHECK(pj_compile(s, &n, &p) == PJ_OK && p.n > 0 && p.nr == p.n, "protected reference refused");
    pj_free(&p);
}

static void run_task(uint32_t w, uint32_t seed) {
    WStats *st = &g_w[w];
    CxStore s;
    SpTask t;
    SpScale sc = { SUBJECTS, T_END };
    CHECK(sp_build(&s, w, seed, &sc, &t) == 0, "%s seed %u: build", sp_workload_names[w], seed);
    if (seed == 0) {
        g_chain_ok &= cx_verify_chain(&s) == CX_OK;
        protected_refusals(&s, &t);
    }
    uint32_t k = st->tasks++;

    /* whole store */
    PjView full;
    pj_view_full(&s, &full);
    full.bytes = pj_full_bytes(&s);
    uint8_t *used_full = calloc(full.n, 1);
    SpTrace trf = { 0, 0, used_full };
    SpAnswer af;
    uint64_t rf = reason_timed(&t, &full, &af, &trf);

    /* projection */
    CognitiveNeed need;
    sp_need(&t, &need);
    StateProjection p;
    int rc = pj_compile(&s, &need, &p);
    CHECK(rc == PJ_OK, "%s seed %u: compile rc %d", sp_workload_names[w], seed, rc);
    if (rc != PJ_OK) { pj_view_free(&full); free(used_full); cx_free(&s); return; }
    PjView pv;
    pj_view_projection(&s, &p, &pv);
    uint8_t *used_proj = calloc(pv.n ? pv.n : 1, 1);
    SpTrace trp = { 0, 0, used_proj };
    SpAnswer ap;
    uint64_t rp = reason_timed(&t, &pv, &ap, &trp);

    int fok = ans_eq(&af, &t.truth), pok = ans_eq(&ap, &t.truth);
    st->full_ok += fok;
    st->proj_ok += pok;
    st->agree += ans_eq(&af, &ap);
    CHECK(fok, "%s seed %u: whole-store answer %llu/%llu/%llu/%llu/%llu, planted %llu/%llu/%llu/%llu/%llu",
          sp_workload_names[w], seed, (unsigned long long)af.a[0], (unsigned long long)af.a[1],
          (unsigned long long)af.a[2], (unsigned long long)af.a[3], (unsigned long long)af.a[4],
          (unsigned long long)t.truth.a[0], (unsigned long long)t.truth.a[1], (unsigned long long)t.truth.a[2],
          (unsigned long long)t.truth.a[3], (unsigned long long)t.truth.a[4]);
    CHECK(pok, "%s seed %u: projection answer %llu/%llu/%llu/%llu/%llu, planted %llu/%llu/%llu/%llu/%llu",
          sp_workload_names[w], seed, (unsigned long long)ap.a[0], (unsigned long long)ap.a[1],
          (unsigned long long)ap.a[2], (unsigned long long)ap.a[3], (unsigned long long)ap.a[4],
          (unsigned long long)t.truth.a[0], (unsigned long long)t.truth.a[1], (unsigned long long)t.truth.a[2],
          (unsigned long long)t.truth.a[3], (unsigned long long)t.truth.a[4]);

    st->full_bytes[k] = full.bytes;
    st->proj_bytes[k] = p.bytes;
    st->full_objects[k] = full.n;
    st->proj_objects[k] = p.objects;
    st->full_touched[k] = trf.touched;
    st->proj_touched[k] = trp.touched;
    st->full_words[k] = trf.words;
    st->proj_words[k] = trp.words;
    st->examined[k] = p.examined;
    st->compile_ns[k] = p.compile_ns;
    st->proj_build_ns[k] = pv.build_ns;
    st->full_build_ns[k] = full.build_ns;
    st->full_reason_ns[k] = rf;
    st->proj_reason_ns[k] = rp;
    st->full_total_ns[k] = full.build_ns + rf;
    st->proj_total_ns[k] = p.compile_ns + pv.build_ns + rp;
    if (st->proj_total_ns[k] < st->full_total_ns[k]) st->e2e_faster++;
    CHECK(p.bytes < full.bytes && p.objects < full.n && trp.touched < trf.touched,
          "%s seed %u: projection not smaller", sp_workload_names[w], seed);

    for (uint32_t i = 0; i < full.n; i++) st->full_used += used_full[i];
    st->full_provided += full.n;
    for (uint32_t i = 0; i < pv.n; i++) st->proj_used += used_proj[i];
    st->proj_used += pv.nf;          /* a feature present is a feature read (see reason_assess) */
    st->proj_provided += pv.n + pv.nf;
    for (uint32_t i = 0; i < t.n_needed; i++) {
        st->needed++;
        st->needed_present += needed_present(&t, &s, &p, t.needed[i]);
    }
    for (uint32_t i = 0; i < p.n; i++) st->by_treatment[p.e[i].treatment]++;
    st->by_treatment[PJ_DERIVED] += p.nf;

    /* controls */
    leave_one_out(&t, &pv, st);
    st->recentk_ok += naive_recent(&t, &s, p.bytes);
    st->subject_ok += naive_subject(&t, &s);
    int applicable = 0;
    int u = underspecified(&t, &s, &applicable);
    if (applicable) { st->underspec_tasks++; st->underspec_ok += u; }

    /* recoverability and the audit */
    uint64_t checked = 0;
    int rr = pj_recover_all(&s, &p, &checked);
    st->recover_checked += checked;
    st->recover_fail += rr != PJ_OK;
    CHECK(rr == PJ_OK, "%s seed %u: recovery failed", sp_workload_names[w], seed);
    PjAudit au;
    pj_audit(&s, &need, &p, &au);
    int aok = au.included == p.n && au.included + au.excluded == s.n && au.counts_match &&
              au.excluded_intact == au.excluded && au.other_branch_included == 0;
    st->audit_ok += aok;
    st->excluded += au.excluded;
    st->excluded_intact += au.excluded_intact;
    st->other_branch += au.other_branch_included;
    for (uint32_t i = 0; i < PJ_WHY_END; i++) st->by_why[i] += au.by_why[i];
    CHECK(aok, "%s seed %u: audit included %llu/%u excluded %llu intact %llu counts %d other-branch %llu",
          sp_workload_names[w], seed, (unsigned long long)au.included, p.n, (unsigned long long)au.excluded,
          (unsigned long long)au.excluded_intact, au.counts_match, (unsigned long long)au.other_branch_included);

    /* tamper with one source the projection depends on without carrying it */
    uint64_t victim = 0;
    if (p.nr) victim = p.r[0].id;
    else if (p.nf) {
        const CxIdList *l = &s.by_subject[p.f[0].subject];
        for (uint32_t i = 0; i < l->n && !victim; i++) {
            const CxObject *o = cx_get(&s, l->ids[i]);
            if (o->kind == p.f[0].kind && o->t >= p.f[0].t0 && o->t <= p.f[0].t1 && o->n) victim = o->id;
        }
    }
    if (victim && cx_get(&s, victim)->n) {
        st->tamper_tried++;
        cx_tamper(&s, victim, 0, cx_payload(&s, cx_get(&s, victim))[0] ^ 1);
        uint32_t n_out = 0;
        int caught = pj_recover_all(&s, &p, NULL) == PJ_ERR_RECOVER &&
                     (p.nr == 0 || pj_recall(&s, &p, victim, &n_out) == NULL);
        st->tamper_caught += caught;
        CHECK(caught, "%s seed %u: tampered source %llu not caught", sp_workload_names[w], seed,
              (unsigned long long)victim);
    }

    /* budget below the pinned state: refused, never trimmed */
    if (seed == 0 && w == W_EXPLAIN) {
        CognitiveNeed small = need;
        small.resource_budget.max_bytes = 256;
        StateProjection q;
        g_budget_refused = pj_compile(&s, &small, &q) == PJ_ERR_BUDGET && q.n == 0;
        CHECK(g_budget_refused, "a budget below the pinned state was not refused");
    }
    if (seed == 0 && w == W_EXPLAIN) {
        /* RECALLABLE: the body comes back on demand, digest-checked (before any tamper) */
        g_recall_ok = 0;
        for (uint32_t i = 0; i < p.n; i++)
            if (p.e[i].treatment == PJ_RECALLABLE) {
                uint32_t n_out = 0;
                const uint64_t *b = pj_recall(&s, &p, p.e[i].id, &n_out);
                g_recall_ok = b && n_out == cx_get(&s, p.e[i].id)->n;
                break;
            }
        CHECK(g_recall_ok, "RECALLABLE body could not be recalled");
        /* EPHEMERAL: in full for this operation, outside the recoverability contract */
        CognitiveNeed e;
        pj_need_init(&e, PJ_OP_PREDICT, t.subject, t.t_now);
        e.required_temporal_scope.t0 = t.t_now - 100;
        pj_need_add(&e, CX_OBSERVATION, K_TELEMETRY, PJ_EPHEMERAL, PJ_W_SELF, PJ_T_WINDOW);
        StateProjection q;
        int erc = pj_compile(&s, &e, &q);
        g_ephemeral_ok = erc == PJ_OK && q.n > 0 && q.nr == 0 && q.e[0].form == PJ_FORM_FULL;
        CHECK(g_ephemeral_ok, "EPHEMERAL projection rc %d", erc);
        if (erc == PJ_OK) pj_free(&q);
    }
    if (seed == 0 && w == W_BRANCH) {
        CognitiveNeed mainline = need;
        mainline.branch = 0;
        StateProjection q;
        g_branch_refused = pj_compile(&s, &mainline, &q) == PJ_ERR_BRANCH;
        CHECK(g_branch_refused, "BRANCH_LOCAL without a branch compiled");
    }

    pj_view_free(&pv);
    pj_view_free(&full);
    free(used_full);
    free(used_proj);
    pj_free(&p);
    cx_free(&s);
}

/* ---- Part B: the real R11 faculty ---- */

enum { SUBJ_EXTERNAL = 100, ISSUER = 3 };
#define RES_DEMAND 0x7712000ull
#define RES_SELECTION 0x7712001ull
enum { K_R11_SELECTION = 30, K_R11_RECEIPT, K_R11_DEMAND, K_R11_PLACEMENT };
#define CLASS_A 0x111u
#define CLASS_B 0x222u
#define REGIMES 4u

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxAienFaculty a;
    RxAienCaps acaps;
    RxObjRef demand, selection;
    RxCapRef ext_demand, ext_selection, ext_placement, ext_goal;
    uint64_t publishes;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static uint64_t fld(Env *e, RxObjRef r, uint32_t i) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[i];
}

static int publish(Env *e, RxCapRef cap, RxObjRef o, const uint32_t *fields, const uint64_t *vals, uint32_t n) {
    RxMutation m[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < n; i++) m[i] = (RxMutation){ o, fields[i], vals[i] };
    int64_t id = rx_world_publish_external(&e->w, cap, m, n);
    e->publishes++;
    return id > 0 && rx_world_wait_quiescent(&e->w, 20000) == RX_OK ? 0 : -1;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof(*e));
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 4, 1u << 18) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    uint64_t z[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(&e->w, RX_OT_DEMAND, RX_PERSIST_RESIDENT, RES_DEMAND, z, &e->demand) != RX_OK ||
        rx_world_create(&e->w, RX_OT_SELECTION, RX_PERSIST_RESIDENT, RES_SELECTION, z, &e->selection) != RX_OK)
        return -1;
    e->ext_demand = mint(e, SUBJ_EXTERNAL, RES_DEMAND, RX_RIGHT_WRITE);
    e->ext_selection = mint(e, SUBJ_EXTERNAL, RES_SELECTION, RX_RIGHT_WRITE);
    RxAienConfig cfg;
    rx_aien_default_config(&cfg);
    RxAienInputs in = { e->demand, e->selection };
    if (rx_aien_create_objects(&e->a, &e->w, &cfg, &in) != RX_OK) return -1;
    const uint32_t RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    for (uint32_t i = 0; i < RX_AIEN_RES_COUNT; i++)
        e->acaps.own[i] = mint(e, RX_AIEN_SUBJ, RX_AIEN_RES_BASE + i,
                               i == RX_AIEN_RES_PLACEMENT || i == RX_AIEN_RES_GOAL ? RX_RIGHT_READ : RW);
    e->acaps.demand = mint(e, RX_AIEN_SUBJ, RES_DEMAND, RX_RIGHT_READ);
    e->acaps.selection = mint(e, RX_AIEN_SUBJ, RES_SELECTION, RX_RIGHT_READ);
    e->ext_placement = mint(e, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_PLACEMENT, RX_RIGHT_WRITE);
    e->ext_goal = mint(e, SUBJ_EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    return rx_aien_register(&e->a, &e->acaps) == RX_OK ? 0 : -1;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 20000);
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

static uint64_t aien_activations(Env *e) {
    const uint32_t r[5] = { e->a.r_observe, e->a.r_predict, e->a.r_explain, e->a.r_assess, e->a.r_plan };
    uint64_t n = 0;
    for (int i = 0; i < 5; i++) n += e->w.reactions[r[i]].activations;
    return n;
}

typedef struct {
    uint32_t kind;           /* RX_AIEN_HYP_CORE_CLASS or RX_AIEN_HYP_DRIFT */
    uint64_t then, now, expected, observed;
} R11Truth;

/* One regime's lifetime plus three other regimes, on one clock. Cortex
 * subject = regime index (1..4); X = 1. */
static void r11_history(CxStore *s, uint32_t seed, R11Truth *truth) {
    uint64_t rng = 0xA5A5A5A5ull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull);
#define RND(n) ((rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17), rng % (n))
    cx_init(s, REGIMES + 1);
    uint64_t clock = 1, calls[REGIMES + 1] = { 0 }, spent[REGIMES + 1] = { 0 }, wins[REGIMES + 1] = { 0 };
    uint64_t placement_seq = 0, cls = CLASS_A, epoch = 0;
    const uint32_t epochs = 24;
    int core_case = seed % 2 == 0;
    RxAienConfig cfg;
    rx_aien_default_config(&cfg);
    const uint32_t steady = 1 + cfg.baseline_intervals + cfg.confirm_intervals + 3;
    CxHeader h;
    uint64_t pl[8];

    #define DEMAND(reg, ns, real) do {                                                  \
        calls[reg] += 16; spent[reg] += 16 * (ns); wins[reg]++;                          \
        memset(&h, 0, sizeof h); h.cls = CX_OBSERVATION; h.kind = K_R11_DEMAND;          \
        h.subject = (reg); h.t = clock++;                                                \
        pl[0] = calls[reg]; pl[1] = spent[reg]; pl[2] = wins[reg];                       \
        pl[3] = 64 * (reg); pl[4] = 256; pl[5] = (real);                                 \
        cx_append(s, &h, pl, 6, NULL);                                                   \
    } while (0)

    for (uint32_t ep = 1; ep <= epochs; ep++) {
        /* between records the work may be moved; a move alone is not evidence */
        if (ep > 1 && RND(3) == 0) {
            cls = cls == CLASS_A ? CLASS_B : CLASS_A;
            memset(&h, 0, sizeof h);
            h.cls = CX_OBSERVATION; h.kind = K_R11_PLACEMENT; h.subject = 1; h.t = clock++;
            pl[0] = ++placement_seq; pl[1] = cls;
            cx_append(s, &h, pl, 2, NULL);
        }
        if (ep == 1) {
            memset(&h, 0, sizeof h);
            h.cls = CX_OBSERVATION; h.kind = K_R11_PLACEMENT; h.subject = 1; h.t = clock++;
            pl[0] = ++placement_seq; pl[1] = cls;
            cx_append(s, &h, pl, 2, NULL);
        }
        epoch = ep;
        uint64_t real = 0xA000 + ep, cost = 3000 + RND(4000);
        /* the record, with the verification receipt it was made under */
        memset(&h, 0, sizeof h);
        h.cls = CX_EVIDENCE; h.kind = K_R11_RECEIPT; h.subject = 1; h.t = clock++;
        h.protect = CX_PROT_VERIFY_EVIDENCE; h.tag = PJ_VERIFIED;
        pl[0] = real; pl[1] = cost;
        uint64_t rid = 0;
        cx_append(s, &h, pl, 2, &rid);
        memset(&h, 0, sizeof h);
        h.cls = CX_REALIZATION; h.kind = K_R11_SELECTION; h.subject = 1; h.t = clock++;
        h.tag = epoch; h.links[0] = rid;
        pl[0] = epoch; pl[1] = real; pl[2] = rx_aien_regime(64, 256);
        cx_append(s, &h, pl, 3, NULL);
        for (uint32_t k = 0; k < steady; k++) {
            DEMAND(1, cost, real);
            for (uint64_t o = 2; o <= REGIMES; o++) DEMAND(o, 2000 + 100 * o + RND(50), 0xB000 + o);
        }
        if (ep == epochs) {
            truth->then = cls;
            truth->expected = cost;
            uint64_t fail_cost = core_case ? cost / 2 : cost * 2;
            if (core_case) {
                cls = cls == CLASS_A ? CLASS_B : CLASS_A;
                memset(&h, 0, sizeof h);
                h.cls = CX_OBSERVATION; h.kind = K_R11_PLACEMENT; h.subject = 1; h.t = clock++;
                pl[0] = ++placement_seq; pl[1] = cls;
                cx_append(s, &h, pl, 2, NULL);
            }
            for (uint32_t k = 0; k < cfg.misses_to_fail; k++) {
                DEMAND(1, fail_cost, real);
                for (uint64_t o = 2; o <= REGIMES; o++) DEMAND(o, 2000 + 100 * o, 0xB000 + o);
            }
            truth->kind = core_case ? RX_AIEN_HYP_CORE_CLASS : RX_AIEN_HYP_DRIFT;
            truth->now = cls;
            truth->observed = fail_cost;
        }
    }
    #undef DEMAND
#undef RND
}

static const CxStore *g_t_store;
static int cmp_time(const void *a, const void *b) {
    const CxObject *x = cx_get(g_t_store, *(const uint64_t *)a), *y = cx_get(g_t_store, *(const uint64_t *)b);
    return x->t < y->t ? -1 : x->t > y->t;
}

/* Publish Cortex objects into the world, in time order, as the world saw them. */
static int replay(Env *e, const CxStore *s, uint64_t *ids, uint32_t n) {
    g_t_store = s;
    qsort(ids, n, sizeof(uint64_t), cmp_time);
    for (uint32_t i = 0; i < n; i++) {
        const CxObject *o = cx_get(s, ids[i]);
        const uint64_t *w = cx_payload(s, o);
        if (o->kind == K_R11_DEMAND) {
            uint32_t f[6] = { 0, 1, 2, 4, 5, 6 };
            uint64_t v[6] = { w[0], w[1], w[2], w[3], w[4], w[5] };
            if (publish(e, e->ext_demand, e->demand, f, v, 6)) return -1;
        } else if (o->kind == K_R11_SELECTION) {
            uint32_t f[3] = { 0, 1, 6 };
            uint64_t v[3] = { w[0], w[1], w[2] };
            if (publish(e, e->ext_selection, e->selection, f, v, 3)) return -1;
        } else if (o->kind == K_R11_PLACEMENT) {
            uint32_t f[2] = { 0, 1 };
            uint64_t v[2] = { w[0], w[1] };
            if (publish(e, e->ext_placement, e->a.o.placement, f, v, 2)) return -1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t runs, full_ok, proj_ok, agree;
    uint64_t full_objects[16], proj_objects[16], full_bytes[16], proj_bytes[16];
    uint64_t full_act[16], proj_act[16], full_ns[16], proj_ns[16], compile_ns[16];
} R11Stats;
static R11Stats g_r11;

static int hyp_matches(Env *e, const R11Truth *t) {
    RxObjRef h = e->a.o.hypothesis;
    return fld(e, h, 0) >= 1 && fld(e, h, 1) == t->kind && fld(e, h, 3) == t->then && fld(e, h, 4) == t->now &&
           fld(e, h, 5) == t->expected && fld(e, h, 6) == t->observed;
}

static void part_b(void) {
    for (uint32_t seed = 0; seed < 8; seed++) {
        CxStore s;
        R11Truth truth;
        memset(&truth, 0, sizeof truth);
        r11_history(&s, seed, &truth);
        uint32_t k = g_r11.runs++;

        /* whole history */
        uint64_t *all = malloc(s.n * sizeof(uint64_t));
        for (uint64_t i = 0; i < s.n; i++) all[i] = i + 1;
        Env e;
        CHECK(env_start(&e) == 0, "R11 env");
        uint64_t t0 = now_ns();
        CHECK(replay(&e, &s, all, (uint32_t)s.n) == 0, "R11 full replay");
        g_r11.full_ns[k] = now_ns() - t0;
        g_r11.full_act[k] = aien_activations(&e);
        int fok = hyp_matches(&e, &truth);
        uint64_t f_plan = fld(&e, e.a.o.plan, 3);
        env_stop(&e);
        g_r11.full_objects[k] = s.n;
        g_r11.full_bytes[k] = pj_full_bytes(&s);

        /* the projection for "why did X's prediction fail" */
        CognitiveNeed n;
        pj_need_init(&n, PJ_OP_EXPLAIN, 1, s.obj[s.n - 1].t);
        n.required_temporal_scope.anchor = PJ_A_SINCE_VERIFIED;
        n.required_temporal_scope.realization_kind = K_R11_SELECTION;
        n.evidence_requirement = PJ_E_REFS;
        pj_need_add(&n, CX_OBSERVATION, K_R11_DEMAND, PJ_LIVE, PJ_W_SELF, PJ_T_WINDOW);
        pj_need_add(&n, CX_OBSERVATION, K_R11_PLACEMENT, PJ_PINNED, PJ_W_SELF, PJ_T_CARRY_IN);
        StateProjection p;
        int rc = pj_compile(&s, &n, &p);
        CHECK(rc == PJ_OK, "R11 compile rc %d", rc);
        uint64_t *ids = malloc((p.n ? p.n : 1) * sizeof(uint64_t));
        for (uint32_t i = 0; i < p.n; i++) ids[i] = p.e[i].id;
        CHECK(env_start(&e) == 0, "R11 env");
        t0 = now_ns();
        CHECK(replay(&e, &s, ids, p.n) == 0, "R11 projection replay");
        g_r11.proj_ns[k] = now_ns() - t0 + p.compile_ns;
        g_r11.compile_ns[k] = p.compile_ns;
        g_r11.proj_act[k] = aien_activations(&e);
        int pok = hyp_matches(&e, &truth);
        uint64_t p_plan = fld(&e, e.a.o.plan, 3);
        env_stop(&e);
        g_r11.proj_objects[k] = p.n;
        g_r11.proj_bytes[k] = p.bytes;

        g_r11.full_ok += fok;
        g_r11.proj_ok += pok;
        g_r11.agree += fok == pok && f_plan == p_plan;
        CHECK(fok, "R11 seed %u: whole history did not produce the planted hypothesis", seed);
        CHECK(pok, "R11 seed %u: projection did not produce the planted hypothesis", seed);
        CHECK(f_plan == p_plan, "R11 seed %u: plans differ (%llu vs %llu)", seed, (unsigned long long)f_plan,
              (unsigned long long)p_plan);
        CHECK(g_r11.proj_act[k] < g_r11.full_act[k] && p.bytes < g_r11.full_bytes[k],
              "R11 seed %u: projection not cheaper", seed);
        uint64_t checked;
        CHECK(pj_recover_all(&s, &p, &checked) == PJ_OK, "R11 recovery");
        free(ids);
        free(all);
        pj_free(&p);
        cx_free(&s);
    }
}

/* ---- Part C: scale ---- */

typedef struct {
    uint32_t subjects;
    uint64_t full_bytes, proj_bytes, full_ns, proj_ns, examined;
    uint32_t ok;
} ScaleRow;
static ScaleRow g_scale[3];

static void part_c(void) {
    const uint32_t sizes[3] = { 16, 64, 256 };
    for (uint32_t i = 0; i < 3; i++) {
        uint64_t fb[8], pb[8], fn[8], pn[8], ex[8];
        uint32_t ok = 0;
        for (uint32_t seed = 0; seed < 8; seed++) {
            CxStore s;
            SpTask t;
            SpScale sc = { sizes[i], T_END };
            sp_build(&s, W_EXPLAIN, 100 + seed, &sc, &t);
            PjView full;
            pj_view_full(&s, &full);
            SpAnswer af, ap;
            SpTrace tr = { 0, 0, NULL };
            uint64_t rf = reason_timed(&t, &full, &af, &tr);
            CognitiveNeed n;
            sp_need(&t, &n);
            StateProjection p;
            pj_compile(&s, &n, &p);
            PjView pv;
            pj_view_projection(&s, &p, &pv);
            uint64_t rp = reason_timed(&t, &pv, &ap, &tr);
            ok += ans_eq(&af, &t.truth) && ans_eq(&ap, &t.truth);
            fb[seed] = pj_full_bytes(&s);
            pb[seed] = p.bytes;
            fn[seed] = full.build_ns + rf;
            pn[seed] = p.compile_ns + pv.build_ns + rp;
            ex[seed] = p.examined;
            pj_view_free(&pv);
            pj_view_free(&full);
            pj_free(&p);
            cx_free(&s);
        }
        g_scale[i] = (ScaleRow){ sizes[i], median(fb, 8), median(pb, 8), median(fn, 8), median(pn, 8),
                                 median(ex, 8), ok };
        CHECK(ok == 8, "scale %u: %u/8 correct", sizes[i], ok);
    }
}

/* ---- receipt ---- */

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *fp = fopen("/proc/self/exe", "rb");
    if (!fp) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, n);
    fclose(fp);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static const char *pass(int b) { return b ? "true" : "false"; }

static void write_receipt(int gate_pass, int preserve, int reduce, int controls, int recover, int grounded) {
    char path[512];
    if (omega_evidence_path("STATE_PROJECTION/omega_state_projection_receipt.json", path, sizeof path) != 0) return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 && !omega_evidence_tree_dirty();
    const char *aienos = getenv("AIENOS_COMMIT");
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    fprintf(fp, "{\n  \"schema\": \"AIEN_OMEGA_STATE_PROJECTION_V1\",\n");
    fprintf(fp, "  \"run_id\": \"%s\",\n", omega_evidence_run_id());
    fprintf(fp, "  \"candidate_commit\": %s%s%s,\n", candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "");
    fprintf(fp, "  \"candidate_bound\": %s,\n  \"run_commit\": \"%s\",\n  \"tree_dirty\": %s,\n", pass(bound),
            commit, pass(omega_evidence_tree_dirty()));
    fprintf(fp, "  \"aienos_commit\": %s%s%s,\n", aienos ? "\"" : "", aienos ? aienos : "null", aienos ? "\"" : "");
    fprintf(fp, "  \"checks\": %d,\n  \"failures\": %d,\n  \"test_binary_sha256\": \"%s\",\n", g_checks, g_fail, digest);
    fprintf(fp, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n", u.sysname,
            u.release, u.machine);
    fprintf(fp, "  \"consumer\": \"explicit rule-based reasoner (part A) and the R11 statistical AIEN faculty "
                "(part B); not a neural model\",\n");
    fprintf(fp, "  \"cortex\": \"in-memory host reference; L1/L2/L3 tiers not implemented\",\n");
    fprintf(fp, "  \"histories\": \"generated with planted answers; %u subsystems, t_end %u\",\n", SUBJECTS, T_END);
    fprintf(fp, "  \"workloads\": [\n");
    for (uint32_t w = 0; w < W_COUNT; w++) {
        const WStats *s = &g_w[w];
        uint32_t n = s->tasks;
        fprintf(fp, "    {\"name\": \"%s\", \"tasks\": %u, \"whole_store_correct\": %u, "
                    "\"projection_correct\": %u, \"answers_agree\": %u,\n", sp_workload_names[w], n,
                s->full_ok, s->proj_ok, s->agree);
        fprintf(fp, "     \"median\": {\"whole_bytes\": %llu, \"projection_bytes\": %llu, \"whole_objects\": %llu, "
                    "\"projection_objects\": %llu, \"cortex_objects_examined\": %llu,\n",
                (unsigned long long)median(s->full_bytes, n), (unsigned long long)median(s->proj_bytes, n),
                (unsigned long long)median(s->full_objects, n), (unsigned long long)median(s->proj_objects, n),
                (unsigned long long)median(s->examined, n));
        fprintf(fp, "       \"whole_items_touched\": %llu, \"projection_items_touched\": %llu, "
                    "\"whole_words_read\": %llu, \"projection_words_read\": %llu,\n",
                (unsigned long long)median(s->full_touched, n), (unsigned long long)median(s->proj_touched, n),
                (unsigned long long)median(s->full_words, n), (unsigned long long)median(s->proj_words, n));
        fprintf(fp, "       \"compile_ns\": %llu, \"projection_view_ns\": %llu, \"whole_view_ns\": %llu, "
                    "\"whole_reason_ns\": %llu, \"projection_reason_ns\": %llu, "
                    "\"whole_end_to_end_ns\": %llu, \"projection_end_to_end_ns\": %llu},\n",
                (unsigned long long)median(s->compile_ns, n), (unsigned long long)median(s->proj_build_ns, n),
                (unsigned long long)median(s->full_build_ns, n), (unsigned long long)median(s->full_reason_ns, n),
                (unsigned long long)median(s->proj_reason_ns, n), (unsigned long long)median(s->full_total_ns, n),
                (unsigned long long)median(s->proj_total_ns, n));
        fprintf(fp, "     \"end_to_end_faster_tasks\": %u,\n", s->e2e_faster);
        fprintf(fp, "     \"recall_precision_x1000\": {\"whole\": %llu, \"projection\": %llu},\n",
                (unsigned long long)(s->full_provided ? s->full_used * 1000 / s->full_provided : 0),
                (unsigned long long)(s->proj_provided ? s->proj_used * 1000 / s->proj_provided : 0));
        fprintf(fp, "     \"recall_x1000\": %llu,\n",
                (unsigned long long)(s->needed ? s->needed_present * 1000 / s->needed : 0));
        fprintf(fp, "     \"unnecessary_state_ratio_x1000\": {\"whole_by_trace\": %llu, \"projection_by_trace\": %llu, "
                    "\"projection_by_leave_one_out\": %llu},\n",
                (unsigned long long)(s->full_provided ? 1000 - s->full_used * 1000 / s->full_provided : 0),
                (unsigned long long)(s->proj_provided ? 1000 - s->proj_used * 1000 / s->proj_provided : 0),
                (unsigned long long)(s->loo_items ? 1000 - s->loo_necessary * 1000 / s->loo_items : 0));
        fprintf(fp, "     \"controls\": {\"leave_one_out_items\": %llu, \"leave_one_out_necessary\": %llu, "
                    "\"tasks_with_a_necessary_item\": %llu, \"newest_objects_same_bytes_correct\": %u, "
                    "\"whole_subject_correct\": %u, \"underspecified_need_correct\": %u, "
                    "\"underspecified_need_tasks\": %u},\n",
                (unsigned long long)s->loo_items, (unsigned long long)s->loo_necessary,
                (unsigned long long)s->tasks_with_necessary, s->recentk_ok, s->subject_ok, s->underspec_ok,
                s->underspec_tasks);
        fprintf(fp, "     \"recoverability\": {\"checked\": %llu, \"failed\": %llu, \"tamper_tried\": %llu, "
                    "\"tamper_caught\": %llu, \"audits_ok\": %llu, \"excluded\": %llu, \"excluded_intact\": %llu, "
                    "\"other_branch_included\": %llu},\n",
                (unsigned long long)s->recover_checked, (unsigned long long)s->recover_fail,
                (unsigned long long)s->tamper_tried, (unsigned long long)s->tamper_caught,
                (unsigned long long)s->audit_ok, (unsigned long long)s->excluded,
                (unsigned long long)s->excluded_intact, (unsigned long long)s->other_branch);
        fprintf(fp, "     \"excluded_by_reason\": {\"subject\": %llu, \"type\": %llu, \"time\": %llu, "
                    "\"branch\": %llu, \"superseded\": %llu, \"derived_source\": %llu},\n",
                (unsigned long long)s->by_why[PJ_WHY_SUBJECT], (unsigned long long)s->by_why[PJ_WHY_TYPE],
                (unsigned long long)s->by_why[PJ_WHY_TIME], (unsigned long long)s->by_why[PJ_WHY_BRANCH],
                (unsigned long long)s->by_why[PJ_WHY_SUPERSEDED], (unsigned long long)s->by_why[PJ_WHY_DERIVED_SOURCE]);
        fprintf(fp, "     \"projected_by_treatment\": {");
        for (uint32_t c = 1; c < PJ_STATE_CLASS_END; c++)
            fprintf(fp, "%s\"%s\": %llu", c > 1 ? ", " : "", pj_state_class_name(c),
                    (unsigned long long)s->by_treatment[c]);
        fprintf(fp, "}}%s\n", w + 1 < W_COUNT ? "," : "");
    }
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"protected_state\": {\"attempts\": %u, \"refused\": %u, "
                "\"kinds\": [\"authority\", \"commit receipt\", \"verification evidence\", \"generation identity\", "
                "\"effect receipt\"], \"treatments_refused\": [\"COMPRESSIBLE\", \"DERIVED\", \"EPHEMERAL\"]},\n",
            g_protect_attempts, g_protect_refusals);
    fprintf(fp, "  \"budget_below_pinned_refused\": %s,\n  \"branch_local_without_branch_refused\": %s,\n"
                "  \"cortex_chain_verified\": %s,\n  \"recallable_body_recalled\": %s,\n"
                "  \"ephemeral_outside_recoverability\": %s,\n", pass(g_budget_refused), pass(g_branch_refused),
            pass(g_chain_ok), pass(g_recall_ok), pass(g_ephemeral_ok));
    uint32_t n = g_r11.runs;
    fprintf(fp, "  \"r11_faculty\": {\"runs\": %u, \"whole_history_correct\": %u, \"projection_correct\": %u, "
                "\"plans_agree\": %u,\n    \"median\": {\"whole_objects\": %llu, \"projection_objects\": %llu, "
                "\"whole_bytes\": %llu, \"projection_bytes\": %llu, \"whole_aien_activations\": %llu, "
                "\"projection_aien_activations\": %llu, \"whole_ns\": %llu, \"projection_ns_including_compile\": %llu, "
                "\"compile_ns\": %llu}},\n",
            n, g_r11.full_ok, g_r11.proj_ok, g_r11.agree, (unsigned long long)median(g_r11.full_objects, n),
            (unsigned long long)median(g_r11.proj_objects, n), (unsigned long long)median(g_r11.full_bytes, n),
            (unsigned long long)median(g_r11.proj_bytes, n), (unsigned long long)median(g_r11.full_act, n),
            (unsigned long long)median(g_r11.proj_act, n), (unsigned long long)median(g_r11.full_ns, n),
            (unsigned long long)median(g_r11.proj_ns, n), (unsigned long long)median(g_r11.compile_ns, n));
    fprintf(fp, "  \"scale_explain_regression\": [\n");
    for (uint32_t i = 0; i < 3; i++)
        fprintf(fp, "    {\"subsystems\": %u, \"correct\": %u, \"median_whole_bytes\": %llu, "
                    "\"median_projection_bytes\": %llu, \"median_cortex_examined\": %llu, "
                    "\"median_whole_end_to_end_ns\": %llu, \"median_projection_end_to_end_ns\": %llu}%s\n",
                g_scale[i].subjects, g_scale[i].ok, (unsigned long long)g_scale[i].full_bytes,
                (unsigned long long)g_scale[i].proj_bytes, (unsigned long long)g_scale[i].examined,
                (unsigned long long)g_scale[i].full_ns, (unsigned long long)g_scale[i].proj_ns, i < 2 ? "," : "");
    fprintf(fp, "  ],\n");
    fprintf(fp, "  \"gates\": {\n    \"semantics_preserved\": %s,\n    \"cost_reduced\": %s,\n"
                "    \"controls_fail_as_expected\": %s,\n    \"protected_and_recoverable\": %s,\n"
                "    \"r11_faculty_grounded\": %s,\n    \"OMEGA_STATE_PROJECTION_PASS\": \"%s\",\n"
                "    \"not_claimed\": [\"neural cognition\", \"Cortex storage tiers\", \"energy measurement\", "
                "\"live production histories\", \"code generation\"]\n  }\n}\n",
            pass(preserve), pass(reduce), pass(controls), pass(recover), pass(grounded),
            gate_pass ? "PASS" : "FAIL");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    printf("[*] part A: five operations x %u planted histories, whole store vs projection\n", SEEDS);
    for (uint32_t w = 0; w < W_COUNT; w++) {
        for (uint32_t seed = 0; seed < SEEDS; seed++) run_task(w, seed);
        const WStats *s = &g_w[w];
        printf("    %-30s correct whole %2u/%u projection %2u/%u  bytes %7llu -> %5llu  "
               "touched %6llu -> %4llu  end-to-end %7llu -> %6llu ns\n",
               sp_workload_names[w], s->full_ok, s->tasks, s->proj_ok, s->tasks,
               (unsigned long long)median(s->full_bytes, s->tasks), (unsigned long long)median(s->proj_bytes, s->tasks),
               (unsigned long long)median(s->full_touched, s->tasks),
               (unsigned long long)median(s->proj_touched, s->tasks),
               (unsigned long long)median(s->full_total_ns, s->tasks),
               (unsigned long long)median(s->proj_total_ns, s->tasks));
        printf("    %-30s controls: newest-same-bytes %u/%u, whole-subject %u/%u, under-specified %u/%u, "
               "leave-one-out necessary %llu/%llu\n", "", s->recentk_ok, s->tasks, s->subject_ok, s->tasks,
               s->underspec_ok, s->underspec_tasks, (unsigned long long)s->loo_necessary,
               (unsigned long long)s->loo_items);
    }
    printf("[*] part B: the R11 AIEN faculty, whole regime history vs projection\n");
    part_b();
    printf("    correct whole %u/%u projection %u/%u  AIEN activations %llu -> %llu  objects %llu -> %llu\n",
           g_r11.full_ok, g_r11.runs, g_r11.proj_ok, g_r11.runs,
           (unsigned long long)median(g_r11.full_act, g_r11.runs), (unsigned long long)median(g_r11.proj_act, g_r11.runs),
           (unsigned long long)median(g_r11.full_objects, g_r11.runs),
           (unsigned long long)median(g_r11.proj_objects, g_r11.runs));
    printf("[*] part C: scale\n");
    part_c();
    for (uint32_t i = 0; i < 3; i++)
        printf("    %3u subsystems: bytes %9llu -> %6llu  end-to-end %9llu -> %7llu ns  correct %u/8\n",
               g_scale[i].subjects, (unsigned long long)g_scale[i].full_bytes,
               (unsigned long long)g_scale[i].proj_bytes, (unsigned long long)g_scale[i].full_ns,
               (unsigned long long)g_scale[i].proj_ns, g_scale[i].ok);

    /* ---- the gate, from what ran ---- */
    int preserve = 1, reduce = 1, controls = 1, recover = 1;
    for (uint32_t w = 0; w < W_COUNT; w++) {
        const WStats *s = &g_w[w];
        preserve &= s->tasks == SEEDS && s->full_ok == s->tasks && s->proj_ok == s->tasks && s->agree == s->tasks;
        reduce &= median(s->proj_bytes, s->tasks) < median(s->full_bytes, s->tasks) &&
                  median(s->proj_touched, s->tasks) < median(s->full_touched, s->tasks) &&
                  median(s->proj_total_ns, s->tasks) < median(s->full_total_ns, s->tasks) &&
                  /* even when the whole store costs nothing to hand over */
                  median(s->proj_total_ns, s->tasks) < median(s->full_reason_ns, s->tasks);
        controls &= s->tasks_with_necessary == s->tasks;
        recover &= s->recover_fail == 0 && s->tamper_caught == s->tamper_tried && s->tamper_tried > 0 &&
                   s->audit_ok == s->tasks && s->other_branch == 0;
    }
    /* a same-size naive projection must lose somewhere, and so must an under-specified need */
    uint32_t recent_all = 0, under_all = 0, under_tasks = 0;
    for (uint32_t w = 0; w < W_COUNT; w++) {
        recent_all += g_w[w].recentk_ok;
        under_all += g_w[w].underspec_ok;
        under_tasks += g_w[w].underspec_tasks;
    }
    controls &= recent_all < W_COUNT * SEEDS && under_all < under_tasks &&
                g_w[W_EXPLAIN].subject_ok < g_w[W_EXPLAIN].tasks;
    recover &= g_protect_refusals == g_protect_attempts && g_protect_attempts == 15 * W_COUNT && g_budget_refused &&
               g_branch_refused && g_chain_ok && g_recall_ok && g_ephemeral_ok;
    int grounded = g_r11.runs == 8 && g_r11.full_ok == 8 && g_r11.proj_ok == 8 && g_r11.agree == 8 &&
                   median(g_r11.proj_act, 8) < median(g_r11.full_act, 8);
    CHECK(preserve, "semantics not preserved");
    CHECK(reduce, "cost not reduced");
    CHECK(controls, "a control did not fail as expected");
    CHECK(recover, "protection or recoverability");
    CHECK(grounded, "R11 grounding");
    int gate = preserve && reduce && controls && recover && grounded && g_fail == 0;
    printf("checks %d failures %d\nOMEGA_STATE_PROJECTION_PASS %s\n", g_checks, g_fail, gate ? "PASS" : "FAIL");
    write_receipt(gate, preserve, reduce, controls, recover, grounded);
    return gate ? 0 : 1;
}
