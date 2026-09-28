/*
 * rx_sp_workloads.c -- planted-answer Cortex histories, the needs Omega
 * compiles for them, and the consumer that answers them. See the header.
 */
#include "rx_sp_workloads.h"

#include <stdlib.h>
#include <string.h>

const char *const sp_workload_names[W_COUNT] = {
    "explain_regression", "verify_cost_claim", "plan_next_experiment",
    "branch_what_if", "assess_goal_with_uncertainty"
};

#define STEP 10u
#define SAMPLES 8u
#define GEN_PERIOD 1400u
#define BRANCH_B 7u
#define BRANCH_C 9u

/* ---- deterministic randomness ---- */

typedef struct { uint64_t s; } Rng;

static uint64_t rn(Rng *r) {
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 2685821657736338717ull;
}

static uint64_t rb(Rng *r, uint64_t n) { return n ? rn(r) % n : 0; }

static uint64_t isqrt(uint64_t x) {
    uint64_t r = 0, b = 1ull << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* ---- events for one subject, appended in time order ---- */

typedef struct {
    uint64_t t;
    uint32_t order, cls, kind, branch, protect, n;
    uint64_t tag;
    int32_t ref[CX_LINKS];   /* event index in this subject, -1 = none */
    uint64_t pl[16];
    uint64_t id;             /* Cortex id after append */
} Ev;

typedef struct {
    Ev *e;
    uint32_t n, cap;
    uint32_t n_int;          /* telemetry intervals */
    uint64_t *mu;            /* level per interval */
    uint64_t *pct;           /* multiplier per interval, percent */
    uint64_t (*samp)[SAMPLES];
    int32_t *tel;            /* telemetry event per interval */
} Subj;

static int32_t ev_add(Subj *s, uint32_t cls, uint32_t kind, uint64_t t, uint32_t order) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->e = realloc(s->e, s->cap * sizeof(Ev));
    }
    Ev *e = &s->e[s->n];
    memset(e, 0, sizeof(*e));
    e->t = t;
    e->order = order;
    e->cls = cls;
    e->kind = kind;
    for (uint32_t i = 0; i < CX_LINKS; i++) e->ref[i] = -1;
    return (int32_t)s->n++;
}

static void fill(Rng *r, Ev *e, uint32_t n) {
    e->n = n;
    for (uint32_t i = 0; i < n; i++) e->pl[i] = rn(r) >> 16;
}

/* A realization: verification receipt, commit receipt, the realization, and
 * the cost claim made for it. Returns the realization's event index. */
static int32_t add_real(Subj *s, Rng *r, uint64_t t, uint64_t number, int verified, uint64_t mu,
                        int32_t *claim_out) {
    int32_t v = ev_add(s, CX_EVIDENCE, K_VERIFY, t, 0);
    s->e[v].protect = CX_PROT_VERIFY_EVIDENCE;
    s->e[v].tag = verified ? PJ_VERIFIED : 2;
    fill(r, &s->e[v], 8);
    s->e[v].pl[0] = number;
    s->e[v].pl[1] = s->e[v].tag;
    int32_t c = ev_add(s, CX_EXECUTION, K_COMMIT, t, 1);
    s->e[c].protect = CX_PROT_COMMIT_RECEIPT;
    fill(r, &s->e[c], 4);
    s->e[c].pl[0] = number;
    int32_t z = ev_add(s, CX_REALIZATION, K_REALIZATION, t, 2);
    s->e[z].tag = number;
    fill(r, &s->e[z], 8);
    s->e[z].pl[0] = number;
    s->e[z].pl[1] = mu;
    s->e[z].ref[0] = v;
    s->e[z].ref[1] = c;
    int32_t cl = ev_add(s, CX_CLAIM, K_COST_CLAIM, t, 3);
    s->e[cl].tag = mu * 115 / 100;
    fill(r, &s->e[cl], 4);
    s->e[cl].pl[0] = number;
    s->e[cl].pl[1] = s->e[cl].tag;
    s->e[cl].ref[0] = z;
    s->e[cl].ref[1] = v;
    if (claim_out) *claim_out = cl;
    return z;
}

static const Ev *g_sort_ev;

static int cmp_idx(const void *a, const void *b) {
    const Ev *x = &g_sort_ev[*(const uint32_t *)a], *y = &g_sort_ev[*(const uint32_t *)b];
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return *(const uint32_t *)a < *(const uint32_t *)b ? -1 : 1;
}

/* Append in time order. Events stay where they are, so saved indices hold. */
static int subj_append(CxStore *st, Subj *s, uint64_t subject) {
    uint32_t n = s->n;
    uint32_t *order = malloc((n ? n : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    g_sort_ev = s->e;
    qsort(order, n, sizeof(uint32_t), cmp_idx);
    uint32_t *pos = order;
    for (uint32_t j = 0; j < n; j++) {
        Ev *e = &s->e[order[j]];
        CxHeader h;
        memset(&h, 0, sizeof h);
        h.cls = e->cls;
        h.kind = e->kind;
        h.subject = subject;
        h.t = e->t;
        h.generation = e->t / GEN_PERIOD + 1;
        h.branch = e->branch;
        h.protect = e->protect;
        h.tag = e->tag;
        for (uint32_t k = 0; k < CX_LINKS; k++) {
            if (e->ref[k] < 0) continue;
            if (!s->e[e->ref[k]].id) { free(pos); return -1; }   /* link to a later object */
            h.links[k] = s->e[e->ref[k]].id;
        }
        if (cx_append(st, &h, e->pl, e->n, &e->id) != CX_OK) { free(pos); return -1; }
    }
    free(pos);
    return 0;
}

static void subj_free(Subj *s) {
    free(s->e);
    free(s->mu);
    free(s->pct);
    free(s->samp);
    free(s->tel);
    memset(s, 0, sizeof(*s));
}

static uint64_t imean(const uint64_t *w) {
    uint64_t sum = 0;
    for (uint32_t k = 0; k < SAMPLES; k++) sum += w[k];
    return sum / SAMPLES;
}

/* ---- per-task plan, decided before any history is written ---- */

typedef struct {
    uint64_t X, deps[2];
    uint64_t T;
    /* W_EXPLAIN */
    uint64_t t_v, t_r;
    uint32_t cause, ratio, hyp_matches, unverified_dep_distractor;
    /* W_VERIFY */
    uint64_t t_c;
    uint32_t verdict, bad_kind;
    /* W_PLAN */
    uint32_t allowed, old_mask, explored_old, explored_new;
    /* W_BRANCH, W_ASSESS */
    uint32_t goal_case;
} Plan;

static const uint32_t g_ratios[4] = { 130, 150, 180, 220 };

/* Realization schedule, levels and multipliers for one subject. */
static void schedule(Subj *s, Rng *r, const Plan *pl, uint64_t subject, uint32_t workload,
                     int32_t *real_idx, uint32_t *n_real, int32_t *claim_idx) {
    const uint64_t T = pl->T;
    int is_x = subject == pl->X, is_dep = subject == pl->deps[0] || subject == pl->deps[1];
    uint64_t stop = T + 1;                       /* no natural realization at or after */
    if (is_x && workload == W_EXPLAIN) stop = pl->t_v - 300;
    if (is_x && workload == W_VERIFY) stop = pl->t_c - 300;
    if (is_dep && workload == W_EXPLAIN) stop = pl->t_v - 300;
    uint64_t times[64];
    int ver[64];
    uint32_t n = 0;
    for (uint64_t t = 0; t < stop && n < 60; t += (400 + rb(r, 500)) / STEP * STEP) {
        times[n] = t;
        ver[n] = t == 0 || rb(r, 5) != 0;
        n++;
    }
    if (is_x && workload == W_EXPLAIN) { times[n] = pl->t_v; ver[n] = 1; n++; }
    if (is_x && workload == W_VERIFY) { times[n] = pl->t_c; ver[n] = pl->bad_kind != 1; n++; }
    s->n_int = (uint32_t)(T / STEP) + 1;
    s->mu = calloc(s->n_int, sizeof(uint64_t));
    s->pct = calloc(s->n_int, sizeof(uint64_t));
    s->tel = malloc(s->n_int * sizeof(int32_t));
    for (uint32_t i = 0; i < s->n_int; i++) { s->pct[i] = 100; s->tel[i] = -1; }
    *n_real = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t mu = 800 + rb(r, 1200);
        int32_t cl = -1;
        real_idx[(*n_real)++] = add_real(s, r, times[k], k + 1, ver[k], mu, &cl);
        if (claim_idx) *claim_idx = cl;
        uint64_t end = k + 1 < n ? times[k + 1] : T + 1;
        for (uint64_t t = times[k]; t < end && t <= T; t += STEP) s->mu[t / STEP] = mu;
    }
}

static void telemetry(Subj *s, Rng *r) {
    s->samp = calloc(s->n_int, sizeof(*s->samp));
    for (uint32_t i = 1; i < s->n_int; i++) {
        uint64_t level = s->mu[i] * s->pct[i] / 100;
        int32_t e = ev_add(s, CX_OBSERVATION, K_TELEMETRY, (uint64_t)i * STEP, 5);
        s->e[e].n = SAMPLES;
        for (uint32_t k = 0; k < SAMPLES; k++) {
            uint64_t v = level * (960 + rb(r, 81)) / 1000;
            s->samp[i][k] = v;
            s->e[e].pl[k] = v;
        }
        s->tel[i] = e;
    }
}

/* Background facts every subject has. Quiet rules keep the planted answer true. */
static void background(Subj *s, Rng *r, const Plan *pl, uint64_t subject, uint32_t workload, uint32_t S) {
    const uint64_t T = pl->T;
    int is_x = subject == pl->X;
    /* dependencies */
    for (uint32_t k = 0; k < 2; k++) {
        int32_t e = ev_add(s, CX_RELATIONSHIP, K_DEPENDS, 0, 0);
        uint64_t dep = is_x ? pl->deps[k] : 1 + rb(r, S);
        if (dep == subject) dep = dep % S + 1;
        s->e[e].tag = dep;
        fill(r, &s->e[e], 2);
    }
    /* resource changes */
    for (uint64_t t = 200 + rb(r, 300) / STEP * STEP; t <= T; t += (300 + rb(r, 400)) / STEP * STEP) {
        if (is_x && workload == W_EXPLAIN && t >= pl->t_v) break;
        int32_t e = ev_add(s, CX_OBSERVATION, K_RESOURCE, t + 3, 6);
        s->e[e].tag = 1 + rb(r, 4);
        fill(r, &s->e[e], 4);
        s->e[e].pl[0] = 100;              /* main-line factor: already in the telemetry */
    }
    /* prior regressions */
    for (uint32_t k = 0; k < 4; k++) {
        uint64_t t = (300 + rb(r, T - 1200)) / STEP * STEP + 4;
        uint64_t sig;
        do sig = 11 + rb(r, 13); while (is_x && workload == W_EXPLAIN && sig == (pl->ratio + 5) / 10);
        int32_t e = ev_add(s, CX_FAILURE, K_REGRESSION, t, 7);
        s->e[e].tag = sig;
        fill(r, &s->e[e], 8);
        s->e[e].pl[0] = 1 + rb(r, 3);
    }
    /* hypotheses: tag = (state << 8) | cause, state 1 open, 2 closed */
    for (uint32_t k = 0; k < 3; k++) {
        uint64_t t = (100 + rb(r, T - 200)) / STEP * STEP + 5;
        int32_t e = ev_add(s, CX_CLAIM, K_HYPOTHESIS, t, 8);
        uint64_t cause = 1 + rb(r, 4);
        if (is_x && workload == W_EXPLAIN && cause == pl->cause) cause = cause % 4 + 1;
        s->e[e].tag = (2ull << 8) | cause;
        fill(r, &s->e[e], 4);
    }
    /* plans: tag = condition key 1..16 */
    if (!(is_x && workload == W_PLAN))
        for (uint32_t k = 0; k < 6; k++) {
            int32_t e = ev_add(s, CX_PLAN, K_PLAN, (50 + rb(r, T - 100)) / STEP * STEP + 6, 9);
            s->e[e].tag = 1 + rb(r, 16);
            fill(r, &s->e[e], 16);
        }
    /* authority grants */
    if (!(is_x && workload == W_PLAN))
        for (uint32_t k = 0; k < 3; k++) {
            int32_t e = ev_add(s, CX_EVIDENCE, K_AUTHORITY, (k * T / 3) / STEP * STEP + 7, 10);
            s->e[e].protect = CX_PROT_AUTHORITY;
            s->e[e].tag = k + 1;
            fill(r, &s->e[e], 4);
            s->e[e].pl[0] = rn(r) & 0xffff;
        }
    /* effect receipts */
    for (uint32_t k = 0; k < 3; k++) {
        int32_t e = ev_add(s, CX_EXECUTION, K_EFFECT, (rb(r, T)) / STEP * STEP + 8, 11);
        s->e[e].protect = CX_PROT_EFFECT_RECEIPT;
        fill(r, &s->e[e], 6);
    }
    /* goal */
    int32_t g = ev_add(s, CX_ENTITY, K_GOAL, 0, 12);
    s->e[g].tag = 500 + rb(r, 3000);
    fill(r, &s->e[g], 2);
}

static void need_id(SpTask *t, uint64_t id) {
    if (id && t->n_needed < SP_MAX_NEEDED) t->needed[t->n_needed++] = id;
}

int sp_build(CxStore *st, uint32_t workload, uint32_t seed, const SpScale *sc, SpTask *t) {
    const uint32_t S = sc->subjects;
    const uint64_t T = sc->t_end;
    Rng r = { 0x9E3779B97F4A7C15ull ^ ((uint64_t)workload << 40) ^ ((uint64_t)seed * 0x100000001B3ull) };
    for (int i = 0; i < 8; i++) rn(&r);
    memset(t, 0, sizeof(*t));
    t->workload = workload;
    t->seed = seed;
    t->t_now = T;

    Plan pl;
    memset(&pl, 0, sizeof pl);
    pl.T = T;
    pl.X = 1 + rb(&r, S);
    pl.deps[0] = pl.X % S + 1;
    pl.deps[1] = (pl.X + S / 2) % S + 1;
    if (pl.deps[1] == pl.X || pl.deps[1] == pl.deps[0]) pl.deps[1] = (pl.X + 2) % S + 1;
    t->subject = pl.X;
    t->deps[0] = pl.deps[0];
    t->deps[1] = pl.deps[1];

    switch (workload) {
    case W_EXPLAIN:
        pl.t_v = (T - 1200) / STEP * STEP;
        pl.t_r = pl.t_v + 500 + rb(&r, 30) * STEP;
        pl.cause = 1 + seed % 4;
        pl.ratio = g_ratios[(seed / 4) % 4];
        pl.hyp_matches = (seed / 16) % 2 == 0;
        pl.unverified_dep_distractor = seed % 3 == 0;
        break;
    case W_VERIFY:
        pl.t_c = (T - 1500) / STEP * STEP;
        pl.verdict = 1 + seed % 3;
        pl.bad_kind = pl.verdict == V_UNVERIFIABLE ? 1 + (seed / 3) % 2 : 0;
        break;
    case W_PLAN: {
        pl.allowed = 0;
        while (__builtin_popcount(pl.allowed) < 6) pl.allowed |= 1u << rb(&r, 16);
        uint32_t lo = (uint32_t)__builtin_ctz(pl.allowed);
        pl.explored_old = 1u << lo;              /* the lowest allowed key was explored long ago */
        for (int k = 0; k < 3; k++) pl.explored_old |= 1u << rb(&r, 16);
        pl.explored_new = 0;
        for (int k = 0; k < 3; k++) pl.explored_new |= 1u << rb(&r, 16);
        if (seed % 5 == 4) pl.explored_new |= pl.allowed;    /* nothing left to try */
        do pl.old_mask = (uint32_t)(rn(&r) & 0xffff); while (pl.old_mask == pl.allowed);
        break;
    }
    default:
        pl.goal_case = seed % 3;
        break;
    }

    if (cx_init(st, S + 1) != CX_OK) return -1;

    /* the machine: generations */
    Subj m;
    memset(&m, 0, sizeof m);
    int32_t gen_ev[64];
    uint32_t n_gen = 0;
    for (uint64_t gt = 0; gt <= T && n_gen < 64; gt += GEN_PERIOD) {
        int32_t e = ev_add(&m, CX_ENTITY, K_GENERATION, gt, 0);
        m.e[e].protect = CX_PROT_GENERATION_ID;
        m.e[e].tag = gt / GEN_PERIOD + 1;
        fill(&r, &m.e[e], 4);
        m.e[e].pl[0] = m.e[e].tag;
        gen_ev[n_gen++] = e;
    }
    if (subj_append(st, &m, 0) != 0) return -1;
    uint64_t gen_id = m.e[gen_ev[n_gen - 1]].id, gen_t = m.e[gen_ev[n_gen - 1]].t;
    subj_free(&m);

    Subj *subj = calloc(S + 1, sizeof(Subj));
    int32_t planted = -1, hyp_ev = -1, auth_ev = -1, goal_ev = -1, focus_ev = -1, br_ev = -1;
    int32_t dep_real = -1, dep_rel[2] = { -1, -1 };
    int rc = 0;
    for (uint64_t x = 1; x <= S && !rc; x++) {
        Subj *s = &subj[x];
        int32_t real_idx[64], claim = -1;
        uint32_t n_real = 0;
        schedule(s, &r, &pl, x, workload, real_idx, &n_real, &claim);
        int is_x = x == pl.X;

        if (workload == W_EXPLAIN && x == pl.deps[0]) {
            if (pl.cause == CAUSE_DEPENDENCY)
                dep_real = add_real(s, &r, pl.t_v + STEP + rb(&r, (pl.t_r - pl.t_v) / STEP - 1) * STEP,
                                    90, 1, 900, NULL);
            else if (pl.unverified_dep_distractor)
                add_real(s, &r, pl.t_v + STEP * 3, 91, 0, 900, NULL);
        }
        if (is_x && workload == W_EXPLAIN) {
            for (uint64_t tt = pl.t_r; tt <= T; tt += STEP) s->pct[tt / STEP] = pl.ratio;
            if (pl.cause == CAUSE_RESOURCE) {
                planted = ev_add(s, CX_OBSERVATION, K_RESOURCE,
                                 pl.t_v + STEP + rb(&r, (pl.t_r - pl.t_v) / STEP - 1) * STEP + 3, 6);
                s->e[planted].tag = 2;
                fill(&r, &s->e[planted], 4);
            }
            if (pl.cause == CAUSE_RECURRENCE) {
                planted = ev_add(s, CX_FAILURE, K_REGRESSION, (300 + rb(&r, pl.t_v - 600)) / STEP * STEP + 4, 7);
                s->e[planted].tag = (pl.ratio + 5) / 10;
                fill(&r, &s->e[planted], 8);
            }
            /* an open hypothesis, matching the planted cause or not, and a closed one that does */
            hyp_ev = ev_add(s, CX_CLAIM, K_HYPOTHESIS, pl.t_v + 15, 8);
            uint64_t hc = pl.hyp_matches ? pl.cause : pl.cause % 4 + 1;
            s->e[hyp_ev].tag = (1ull << 8) | hc;
            fill(&r, &s->e[hyp_ev], 4);
            int32_t closed = ev_add(s, CX_CLAIM, K_HYPOTHESIS, pl.t_v + 25, 8);
            s->e[closed].tag = (2ull << 8) | pl.cause;
            fill(&r, &s->e[closed], 4);
        }
        if (is_x && workload == W_VERIFY) {
            focus_ev = claim;
            uint64_t mu = s->mu[pl.t_c / STEP];
            s->e[claim].tag = mu * 115 / 100;
            s->e[claim].pl[1] = s->e[claim].tag;
            if (pl.bad_kind == 2) {
                /* the receipt verified a different realization */
                int32_t v = s->e[claim].ref[1];
                s->e[v].pl[0] = 999;
            }
            if (pl.verdict == V_REFUTED) {
                uint64_t at = pl.t_c + 200 + rb(&r, 800) / STEP * STEP;
                for (uint64_t tt = at; tt < at + 3 * STEP && tt <= T; tt += STEP) s->pct[tt / STEP] = 130;
            }
        }
        if (is_x && workload == W_PLAN) {
            for (uint32_t k = 0; k < 16; k++) {
                if (pl.explored_old & (1u << k)) {
                    int32_t e = ev_add(s, CX_PLAN, K_PLAN, (100 + rb(&r, T / 4)) / STEP * STEP + 6, 9);
                    s->e[e].tag = k + 1;
                    fill(&r, &s->e[e], 16);
                }
                if (pl.explored_new & (1u << k)) {
                    int32_t e = ev_add(s, CX_PLAN, K_PLAN, (T - 900 + rb(&r, 800)) / STEP * STEP + 6, 9);
                    s->e[e].tag = k + 1;
                    fill(&r, &s->e[e], 16);
                }
            }
            int32_t old = ev_add(s, CX_EVIDENCE, K_AUTHORITY, T / 3 / STEP * STEP + 7, 10);
            s->e[old].protect = CX_PROT_AUTHORITY;
            fill(&r, &s->e[old], 4);
            s->e[old].pl[0] = pl.old_mask;
            auth_ev = ev_add(s, CX_EVIDENCE, K_AUTHORITY, (T - 200) / STEP * STEP + 7, 10);
            s->e[auth_ev].protect = CX_PROT_AUTHORITY;
            fill(&r, &s->e[auth_ev], 4);
            s->e[auth_ev].pl[0] = pl.allowed;
        }

        telemetry(s, &r);
        background(s, &r, &pl, x, workload, S);

        if (is_x && workload == W_BRANCH) {
            t->t0 = T - 110;
            uint64_t sum = 0, cnt = 0;
            for (uint64_t tt = (t->t0 + STEP - 1) / STEP * STEP; tt <= T; tt += STEP)
                for (uint32_t k = 0; k < SAMPLES; k++) { sum += s->samp[tt / STEP][k]; cnt++; }
            uint64_t mean = sum / cnt;
            uint64_t fb = pl.goal_case == 0 ? 70 : pl.goal_case == 1 ? 140 : 95;
            uint64_t fc = fb < 100 ? 140 : 70;
            br_ev = ev_add(s, CX_OBSERVATION, K_RESOURCE, T - 5, 13);
            s->e[br_ev].branch = BRANCH_B;
            fill(&r, &s->e[br_ev], 4);
            s->e[br_ev].pl[0] = fb;
            int32_t c2 = ev_add(s, CX_OBSERVATION, K_RESOURCE, T - 5, 14);
            s->e[c2].branch = BRANCH_C;
            fill(&r, &s->e[c2], 4);
            s->e[c2].pl[0] = fc;
            goal_ev = ev_add(s, CX_ENTITY, K_GOAL, T - 3, 15);
            s->e[goal_ev].tag = mean;          /* target: today's mean */
            fill(&r, &s->e[goal_ev], 2);
            uint64_t proj = mean * fb / 100;
            t->truth.a[0] = proj <= mean ? G_MET : G_UNMET;
            t->truth.a[1] = proj;
            t->branch = BRANCH_B;
        }
        if (is_x && workload == W_ASSESS) {
            t->t0 = gen_t;
            uint64_t S1 = 0, Q = 0, n = 0;
            for (uint64_t tt = (gen_t + STEP - 1) / STEP * STEP; tt <= T; tt += STEP)
                for (uint32_t k = 0; k < SAMPLES; k++) {
                    uint64_t v = s->samp[tt / STEP][k];
                    S1 += v;
                    Q += v * v;
                    n++;
                }
            uint64_t mean = S1 / n, var = Q / n - mean * mean, se = isqrt(var / n);
            uint64_t target = pl.goal_case == 0 ? mean + 2 * se + 20
                            : pl.goal_case == 1 ? (mean > 2 * se + 20 ? mean - 2 * se - 20 : 1) : mean;
            goal_ev = ev_add(s, CX_ENTITY, K_GOAL, T - 3, 15);
            s->e[goal_ev].tag = target;
            fill(&r, &s->e[goal_ev], 2);
            t->truth.a[0] = mean + 2 * se <= target ? G_MET : mean >= target + 2 * se + 1 ? G_UNMET : G_UNCERTAIN;
            t->truth.a[1] = mean;
            t->truth.a[2] = se;
        }
        if (is_x && workload == W_EXPLAIN) {
            /* relationships to the planted dependencies */
            for (uint32_t i = 0; i < s->n; i++)
                if (s->e[i].kind == K_DEPENDS) {
                    if (dep_rel[0] < 0) dep_rel[0] = (int32_t)i;
                    else if (dep_rel[1] < 0) dep_rel[1] = (int32_t)i;
                }
        }
        rc = subj_append(st, s, x);
    }
    if (rc) goto out;

    Subj *X = &subj[pl.X];
    /* ---- truth and the objects it rests on ---- */
    if (workload == W_EXPLAIN) {
        /* The generator's own arithmetic over the samples it wrote. */
        uint64_t base = 0, nb = 0, post = 0, np = 0;
        for (uint64_t tt = pl.t_v; tt <= T; tt += STEP) {
            uint32_t i = (uint32_t)(tt / STEP);
            if (i == 0) continue;
            if (tt < pl.t_r) { base += imean(X->samp[i]); nb++; }
            else { post += imean(X->samp[i]); np++; }
            need_id(t, X->e[X->tel[i]].id);
        }
        t->truth.a[0] = pl.cause;
        t->truth.a[2] = (post / np) * 100 / (base / nb);
        t->truth.a[3] = pl.t_r;
        if (pl.cause == CAUSE_RESOURCE || pl.cause == CAUSE_RECURRENCE) t->truth.a[1] = X->e[planted].id;
        if (pl.cause == CAUSE_DEPENDENCY) {
            Subj *D = &subj[pl.deps[0]];
            t->truth.a[1] = D->e[dep_real].id;
            need_id(t, D->e[dep_real].id);
            need_id(t, D->e[D->e[dep_real].ref[0]].id);
            for (int k = 0; k < 2; k++)
                if (dep_rel[k] >= 0 && X->e[dep_rel[k]].tag == pl.deps[0]) need_id(t, X->e[dep_rel[k]].id);
        }
        need_id(t, t->truth.a[1]);
        t->truth.a[4] = pl.hyp_matches ? X->e[hyp_ev].id : 0;
        need_id(t, t->truth.a[4]);
        /* the anchor realization and its receipt */
        for (uint32_t i = 0; i < X->n; i++)
            if (X->e[i].kind == K_REALIZATION && X->e[i].t == pl.t_v) {
                need_id(t, X->e[i].id);
                need_id(t, X->e[X->e[i].ref[0]].id);
            }
    } else if (workload == W_VERIFY) {
        const Ev *c = &X->e[focus_ev], *v = &X->e[c->ref[1]];
        t->focus = c->id;
        t->t0 = c->t;
        uint64_t worst = 0;
        for (uint64_t tt = (c->t + STEP - 1) / STEP * STEP; tt <= T; tt += STEP) {
            uint64_t m = imean(X->samp[tt / STEP]);
            if (m > worst) worst = m;
            need_id(t, X->e[X->tel[tt / STEP]].id);
        }
        uint32_t verdict = (v->tag != PJ_VERIFIED || v->pl[0] != c->pl[0]) ? V_UNVERIFIABLE
                         : worst > c->tag ? V_REFUTED : V_SUPPORTED;
        t->truth.a[0] = verdict;
        t->truth.a[1] = v->id;
        t->truth.a[2] = worst;
        need_id(t, c->id);
        need_id(t, v->id);
    } else if (workload == W_PLAN) {
        uint32_t explored = pl.explored_old | pl.explored_new, left = pl.allowed & ~explored;
        t->truth.a[0] = left ? (uint64_t)__builtin_ctz(left) + 1 : 0;
        t->truth.a[1] = X->e[auth_ev].id;
        need_id(t, X->e[auth_ev].id);
        for (uint32_t i = 0; i < X->n; i++)
            if (X->e[i].kind == K_PLAN) need_id(t, X->e[i].id);
    } else if (workload == W_BRANCH) {
        need_id(t, X->e[goal_ev].id);
        need_id(t, X->e[br_ev].id);
        for (uint64_t tt = (t->t0 + STEP - 1) / STEP * STEP; tt <= T; tt += STEP)
            need_id(t, X->e[X->tel[tt / STEP]].id);
    } else if (workload == W_ASSESS) {
        need_id(t, gen_id);
        need_id(t, X->e[goal_ev].id);
        for (uint64_t tt = (gen_t + STEP - 1) / STEP * STEP; tt <= T; tt += STEP)
            need_id(t, X->e[X->tel[tt / STEP]].id);
    }
out:
    for (uint64_t x = 1; x <= S; x++) subj_free(&subj[x]);
    free(subj);
    return rc;
}

/* ---- the needs ---- */

void sp_need(const SpTask *t, CognitiveNeed *n) {
    switch (t->workload) {
    case W_EXPLAIN:
        pj_need_init(n, PJ_OP_EXPLAIN, t->subject, t->t_now);
        n->required_temporal_scope.anchor = PJ_A_SINCE_VERIFIED;
        n->required_temporal_scope.realization_kind = K_REALIZATION;
        n->required_world_scope.dependency_kind = K_DEPENDS;
        n->evidence_requirement = PJ_E_REFS;
        n->memory_scope = PJ_M_EPISODIC;
        pj_need_add(n, CX_OBSERVATION, K_TELEMETRY, PJ_COMPRESSIBLE, PJ_W_SELF, PJ_T_WINDOW);
        pj_need_add(n, CX_OBSERVATION, K_RESOURCE, PJ_PINNED, PJ_W_SELF, PJ_T_WINDOW);
        pj_need_add(n, CX_REALIZATION, K_REALIZATION, PJ_PINNED, PJ_W_DEPENDENCIES, PJ_T_WINDOW);
        pj_need_add(n, CX_FAILURE, K_REGRESSION, PJ_REFERENCE_ONLY, PJ_W_SELF, PJ_T_MEMORY);
        pj_need_add(n, CX_CLAIM, K_HYPOTHESIS, PJ_RECALLABLE, PJ_W_SELF, PJ_T_MEMORY);
        break;
    case W_VERIFY:
        pj_need_init(n, PJ_OP_VERIFY, t->subject, t->t_now);
        n->focus = t->focus;
        n->required_temporal_scope.t0 = t->t0;
        n->evidence_requirement = PJ_E_FULL;
        pj_need_add(n, CX_OBSERVATION, K_TELEMETRY, PJ_COMPRESSIBLE, PJ_W_SELF, PJ_T_WINDOW);
        break;
    case W_PLAN:
        pj_need_init(n, PJ_OP_PLAN, t->subject, t->t_now);
        n->memory_scope = PJ_M_EPISODIC;
        pj_need_add(n, CX_PLAN, K_PLAN, PJ_REFERENCE_ONLY, PJ_W_SELF, PJ_T_MEMORY);
        pj_need_add(n, CX_EVIDENCE, K_AUTHORITY, PJ_PINNED, PJ_W_SELF, PJ_T_LATEST);
        break;
    case W_BRANCH:
        pj_need_init(n, PJ_OP_PREDICT, t->subject, t->t_now);
        n->branch = t->branch;
        n->required_temporal_scope.t0 = t->t0;
        pj_need_add(n, CX_OBSERVATION, K_TELEMETRY, PJ_COMPRESSIBLE, PJ_W_SELF, PJ_T_WINDOW);
        pj_need_add(n, CX_OBSERVATION, K_RESOURCE, PJ_BRANCH_LOCAL, PJ_W_SELF, PJ_T_WINDOW);
        pj_need_add(n, CX_ENTITY, K_GOAL, PJ_LIVE, PJ_W_SELF, PJ_T_LATEST);
        break;
    case W_ASSESS:
        pj_need_init(n, PJ_OP_ASSESS, t->subject, t->t_now);
        n->required_temporal_scope.anchor = PJ_A_GENERATION;
        n->required_temporal_scope.generation_kind = K_GENERATION;
        n->uncertainty_requirement = PJ_U_INTERVAL;
        pj_need_add(n, CX_OBSERVATION, K_TELEMETRY, PJ_DERIVED, PJ_W_SELF, PJ_T_WINDOW);
        pj_need_add(n, CX_ENTITY, K_GOAL, PJ_LIVE, PJ_W_SELF, PJ_T_LATEST);
        break;
    }
}

/* ---- the consumer ---- */

/* Interval sum and sample count, from a full series or a summary. */
static int series(const PjItem *it, SpTrace *tr, uint64_t *sum, uint64_t *count) {
    if (it->form == PJ_FORM_SUMMARY && it->n == PJ_SUMMARY_WORDS) {
        *count = it->w[0];
        *sum = it->w[1];
        tr->words += 2;
        return 0;
    }
    if (it->form != PJ_FORM_FULL || !it->n) return -1;
    uint64_t s = 0;
    for (uint32_t k = 0; k < it->n; k++) s += it->w[k];
    tr->words += it->n;
    *sum = s;
    *count = it->n;
    return 0;
}

static void use(SpTrace *tr, const PjView *v, const PjItem *it) {
    if (tr->used && it) tr->used[it - v->items] = 1;
}

static int verified(const PjView *v, const PjItem *real, SpTrace *tr) {
    const PjItem *ev = pj_view_find(v, real->links[0]);
    if (!ev) return 0;
    use(tr, v, ev);
    return ev->kind == K_VERIFY && ev->tag == PJ_VERIFIED;
}

static void reason_explain(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    const uint64_t X = t->subject, now = t->t_now;
    uint64_t deps[8];
    uint32_t nd = 0;
    const PjItem *anchor = NULL;
    /* pass 1: dependencies and the last verified realization */
    for (uint32_t i = 0; i < v->n; i++) {
        const PjItem *it = &v->items[i];
        tr->touched++;
        if (it->subject != X || it->branch != 0 || it->t > now) continue;
        if (it->kind == K_DEPENDS && nd < 8) { deps[nd++] = it->tag; use(tr, v, it); }
        if (it->kind == K_REALIZATION && (!anchor || it->t >= anchor->t) && verified(v, it, tr)) anchor = it;
    }
    if (!anchor) return;
    use(tr, v, anchor);
    const uint64_t tv = anchor->t;
    /* pass 2 */
    uint32_t cap = v->n ? v->n : 1, ntel = 0, nres = 0, ndr = 0, nfail = 0, nhyp = 0;
    const PjItem **tel = malloc(cap * sizeof(*tel)), **res = malloc(cap * sizeof(*res)),
                 **dr = malloc(cap * sizeof(*dr)), **fail = malloc(cap * sizeof(*fail)),
                 **hyp = malloc(cap * sizeof(*hyp));
    for (uint32_t i = 0; i < v->n; i++) {
        const PjItem *it = &v->items[i];
        tr->touched++;
        if (it->branch != 0 || it->t > now) continue;
        if (it->subject == X) {
            if (it->kind == K_TELEMETRY && it->t >= tv) tel[ntel++] = it;
            else if (it->kind == K_RESOURCE && it->t > tv) res[nres++] = it;
            else if (it->kind == K_REGRESSION) fail[nfail++] = it;
            else if (it->kind == K_HYPOTHESIS) hyp[nhyp++] = it;
        } else if (it->kind == K_REALIZATION && it->t > tv) {
            for (uint32_t k = 0; k < nd; k++)
                if (deps[k] == it->subject) dr[ndr++] = it;
        }
    }
    /* onset: first interval 25% above the running baseline */
    uint64_t run = 0, nrun = 0, onset = 0, post = 0, npost = 0, base = 0;
    for (uint32_t i = 0; i < ntel; i++) {
        uint64_t s, c;
        if (series(tel[i], tr, &s, &c) || !c) continue;
        use(tr, v, tel[i]);
        uint64_t m = s / c;
        if (!onset && nrun >= 3 && m * 100 > (run / nrun) * 115) { onset = tel[i]->t; base = run / nrun; }
        if (onset) { post += m; npost++; }
        else { run += m; nrun++; }
    }
    SpAnswer a;
    memset(&a, 0, sizeof a);
    a.a[3] = onset;
    if (!onset) { a.a[0] = CAUSE_DRIFT; *out = a; goto done; }
    a.a[2] = (post / npost) * 100 / base;
    const PjItem *cause = NULL;
    for (uint32_t i = 0; i < nres; i++) {
        use(tr, v, res[i]);
        if (res[i]->t <= onset && (!cause || res[i]->t > cause->t)) cause = res[i];
    }
    if (cause) a.a[0] = CAUSE_RESOURCE;
    if (!cause)
        for (uint32_t i = 0; i < ndr; i++) {
            use(tr, v, dr[i]);
            if (dr[i]->t <= onset && verified(v, dr[i], tr) && (!cause || dr[i]->t < cause->t)) cause = dr[i];
        }
    if (cause && !a.a[0]) a.a[0] = CAUSE_DEPENDENCY;
    if (!cause)
        for (uint32_t i = 0; i < nfail; i++) {
            use(tr, v, fail[i]);
            if (fail[i]->tag == (a.a[2] + 5) / 10 && (!cause || fail[i]->t > cause->t)) cause = fail[i];
        }
    if (cause && !a.a[0]) a.a[0] = CAUSE_RECURRENCE;
    if (!cause) a.a[0] = CAUSE_DRIFT;
    a.a[1] = cause ? cause->id : 0;
    for (uint32_t i = 0; i < nhyp; i++) {
        use(tr, v, hyp[i]);
        if ((hyp[i]->tag >> 8) == 1 && (hyp[i]->tag & 0xff) == a.a[0] && (!a.a[4] || hyp[i]->id > a.a[4]))
            a.a[4] = hyp[i]->id;
    }
    *out = a;
done:
    free(tel); free(res); free(dr); free(fail); free(hyp);
}

static void reason_verify(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    SpAnswer a;
    memset(&a, 0, sizeof a);
    const PjItem *c = pj_view_find(v, t->focus);
    tr->touched++;
    if (!c || c->form != PJ_FORM_FULL || c->n < 2) { a.a[0] = V_UNVERIFIABLE; *out = a; return; }
    use(tr, v, c);
    tr->words += 2;
    const PjItem *rc = pj_view_find(v, c->links[1]);
    uint64_t worst = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        const PjItem *it = &v->items[i];
        tr->touched++;
        if (it->subject != t->subject || it->branch != 0 || it->kind != K_TELEMETRY || it->t < c->t ||
            it->t > t->t_now)
            continue;
        uint64_t s, n;
        if (series(it, tr, &s, &n) || !n) continue;
        use(tr, v, it);
        if (s / n > worst) worst = s / n;
    }
    a.a[2] = worst;
    if (rc) { use(tr, v, rc); a.a[1] = rc->id; }
    if (!rc || rc->form != PJ_FORM_FULL || rc->n < 1 || rc->tag != PJ_VERIFIED || rc->w[0] != c->w[0])
        a.a[0] = V_UNVERIFIABLE;
    else
        a.a[0] = worst > c->w[1] ? V_REFUTED : V_SUPPORTED;
    if (rc) tr->words += 1;
    *out = a;
}

static void reason_plan(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    SpAnswer a;
    memset(&a, 0, sizeof a);
    const PjItem *auth = NULL;
    uint32_t explored = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        const PjItem *it = &v->items[i];
        tr->touched++;
        if (it->subject != t->subject || it->branch != 0 || it->t > t->t_now) continue;
        if (it->kind == K_AUTHORITY && (!auth || it->t > auth->t)) auth = it;
        if (it->kind == K_PLAN && it->tag >= 1 && it->tag <= 16) { explored |= 1u << (it->tag - 1); use(tr, v, it); }
    }
    if (auth && auth->form == PJ_FORM_FULL && auth->n) {
        use(tr, v, auth);
        tr->words++;
        uint32_t left = (uint32_t)auth->w[0] & ~explored;
        a.a[0] = left ? (uint64_t)__builtin_ctz(left) + 1 : 0;
        a.a[1] = auth->id;
    }
    *out = a;
}

static void reason_branch(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    SpAnswer a;
    memset(&a, 0, sizeof a);
    const PjItem *goal = NULL;
    uint64_t sum = 0, cnt = 0, num = 1, den = 1;
    const PjItem *factors[16];
    uint32_t nf = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        const PjItem *it = &v->items[i];
        tr->touched++;
        if (it->subject != t->subject || it->t > t->t_now) continue;
        if (it->branch == 0 && it->kind == K_GOAL && (!goal || it->t >= goal->t)) goal = it;
        if (it->branch == 0 && it->kind == K_TELEMETRY && it->t >= t->t0) {
            uint64_t s, n;
            if (!series(it, tr, &s, &n)) { sum += s; cnt += n; use(tr, v, it); }
        }
        if (it->branch == t->branch && it->kind == K_RESOURCE && it->t >= t->t0 && nf < 16) factors[nf++] = it;
    }
    for (uint32_t i = 0; i < nf; i++) {
        if (factors[i]->form != PJ_FORM_FULL || !factors[i]->n) continue;
        use(tr, v, factors[i]);
        tr->words++;
        num *= factors[i]->w[0];
        den *= 100;
    }
    if (!goal || !cnt) { *out = a; return; }
    use(tr, v, goal);
    uint64_t proj = (sum / cnt) * num / den;
    a.a[0] = proj <= goal->tag ? G_MET : G_UNMET;
    a.a[1] = proj;
    *out = a;
}

static void reason_assess(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    SpAnswer a;
    memset(&a, 0, sizeof a);
    const PjItem *gen = NULL, *goal = NULL;
    for (uint32_t i = 0; i < v->n; i++) {
        const PjItem *it = &v->items[i];
        tr->touched++;
        if (it->branch != 0 || it->t > t->t_now) continue;
        if (it->subject == 0 && it->kind == K_GENERATION && (!gen || it->t > gen->t)) gen = it;
        if (it->subject == t->subject && it->kind == K_GOAL && (!goal || it->t >= goal->t)) goal = it;
    }
    if (!gen || !goal) { *out = a; return; }
    use(tr, v, gen);
    use(tr, v, goal);
    uint64_t S1 = 0, Q = 0, n = 0;
    int have = 0;
    for (uint32_t i = 0; i < v->nf; i++) {
        const PjFeature *f = &v->feat[i];
        tr->touched++;
        if (f->subject == t->subject && f->kind == K_TELEMETRY && f->t0 == gen->t && f->branch == 0) {
            S1 = f->sum; Q = f->sumsq; n = f->count;
            tr->words += 3;
            have = 1;
            break;
        }
    }
    if (!have)
        for (uint32_t i = 0; i < v->n; i++) {
            const PjItem *it = &v->items[i];
            tr->touched++;
            if (it->subject != t->subject || it->branch != 0 || it->kind != K_TELEMETRY || it->t < gen->t ||
                it->t > t->t_now || it->form != PJ_FORM_FULL)
                continue;
            use(tr, v, it);
            for (uint32_t k = 0; k < it->n; k++) { S1 += it->w[k]; Q += it->w[k] * it->w[k]; }
            n += it->n;
            tr->words += it->n;
        }
    if (!n) { *out = a; return; }
    uint64_t mean = S1 / n, var = Q / n - mean * mean, se = isqrt(var / n), target = goal->tag;
    a.a[0] = mean + 2 * se <= target ? G_MET : mean >= target + 2 * se + 1 ? G_UNMET : G_UNCERTAIN;
    a.a[1] = mean;
    a.a[2] = se;
    *out = a;
}

void sp_reason(const SpTask *t, const PjView *v, SpAnswer *out, SpTrace *tr) {
    memset(out, 0, sizeof(*out));
    switch (t->workload) {
    case W_EXPLAIN: reason_explain(t, v, out, tr); break;
    case W_VERIFY: reason_verify(t, v, out, tr); break;
    case W_PLAN: reason_plan(t, v, out, tr); break;
    case W_BRANCH: reason_branch(t, v, out, tr); break;
    case W_ASSESS: reason_assess(t, v, out, tr); break;
    }
}
