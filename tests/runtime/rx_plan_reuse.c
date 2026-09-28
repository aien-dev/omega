/*
 * OMEGA_PLAN_REUSE -- plan IR and verified plan cache (spec/plan-reuse.md).
 *
 * Every instance has its own World and its own native AIENOS authority. The
 * test plays the outside: it holds the admin (to give the principal its
 * authority, and to revoke it), builds the World an instance starts from,
 * and publishes run tokens. Cognition (goal shape, retrieval, applicability,
 * binding, or AIEN's search) and execution (the plan as an action graph on
 * resident reactions) are measured separately.
 *
 * For every repeated-workload instance the first-use path (cache off) runs
 * in a mirror World built identically; its outcome is compared with the
 * reuse path's.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_plan.h"
#include "runtime/rx_plan_arrange.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <linux/perf_event.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, SUBJ_PLAN = 71 };

#define RES_CELL     0xB000001ull
#define RES_RUN      0xB000002ull
#define RES_DECISION 0xB000003ull
#define RES_INDEX    0xB000004ull
#define RES_ENV      0xB000005ull
#define RES_UNIT     0xB000100ull

#define WORLD_GEN    7u
#define COG_GEN      3u
#define N_REPEAT     40u
#define N_ADV        10u
#define MAX_EXPAND   2000000u

static int g_checks, g_fail;

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

static uint64_t cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- instructions retired (user space, this thread) ---- */

static int g_perf = -1;

static void perf_open(void) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.size = sizeof a;
    a.config = PERF_COUNT_HW_INSTRUCTIONS;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    g_perf = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (g_perf >= 0) ioctl(g_perf, PERF_EVENT_IOC_ENABLE, 0);
}

static uint64_t instr(void) {
    uint64_t v = 0;
    if (g_perf < 0 || read(g_perf, &v, sizeof v) != (ssize_t)sizeof v) return 0;
    return v;
}

/* ---- receipt figures ---- */

typedef struct {
    uint32_t n;
    uint64_t ops[N_REPEAT], cpu[N_REPEAT], wall[N_REPEAT], ins[N_REPEAT], e2e[N_REPEAT];
    uint64_t exec[N_REPEAT];
    uint64_t compile[N_REPEAT], run[N_REPEAT], verify[N_REPEAT];
} Series;

static struct {
    char cpus[64];
    uint64_t core_class;
    Series first[2], reuse[2];
    uint32_t reuse_accepted[2], reuse_success[2], outcome_equal[2], evidence_equal[2];
    uint32_t cold_success[2], moves[2], evidence_words[2], nodes[2];
    uint32_t semantic_ids_equal[2], realizations_distinct[2];
    uint32_t candidate_reference_runs, promotions;
    uint32_t adv_cases, adv_refused, adv_axis_match, adv_decision_crumbs;
    uint32_t adv_by_axis[PL_AX_COUNT], adv_refused_by_axis[PL_AX_COUNT];
    uint32_t blind_by_axis[PL_AX_COUNT], blind_ok_by_axis[PL_AX_COUNT];
    uint32_t blind_violation_by_axis[PL_AX_COUNT];
    uint32_t accepted_total, false_applicable;
    uint32_t id_roundtrip, id_order_invariant, id_mutations, id_mutations_changed;
    uint32_t id_nonsemantic, id_nonsemantic_same, rederived_same_id, rederived_then_reused;
    uint32_t adapt_n, adapt_ok, adapt_then_reuse, adapt_steps, adapt_cold_steps;
    uint64_t adapt_ops_med, adapt_cold_ops_med, adapt_cpu_med, adapt_cold_cpu_med;
    int energy_ok;
    uint64_t energy_reuse_refused;
    char energy_why[160];
    double idle_w, cold_pkg_mj_per, reuse_pkg_mj_per, cold_cpup_mj_per, reuse_cpup_mj_per;
    uint64_t cold_loops, reuse_loops, loop_ns;
    int perf_ok;
    uint64_t stored, dedup;
} R;

/* ---- environment ---- */

enum { SHAPE_REVERSE = 0, SHAPE_MERGE = 1, SHAPE_OTHER = 2 };

typedef enum {
    PERT_NONE = 0, PERT_BINDING, PERT_GOAL, PERT_WORLD_GEN, PERT_COG_GEN, PERT_ENV,
    PERT_STATE_SWAP, PERT_STATE_OUTSIDER, PERT_AUTHORITY, PERT_RESOURCES
} Pert;

typedef struct {
    uint32_t shape, seed;
    Pert pert;
    uint64_t world_gen, cog_gen, max_steps;
} Spec;

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    PlEnv env;
    RxObjRef decision, index;
    uint32_t n_units, n_goal_units;
    RxObjRef unit[PLA_MAX_UNITS];       /* role order: goal units, then distractors */
    RxCapRef unit_cap[PLA_MAX_UNITS];
    RxCapRef ext_unit[PLA_MAX_UNITS];
    RxCapRef ext_run, ext_decision, ext_index;
    AgCapTable caps;
    PlExecCtx x;
    PlRunStore rs;
    uint64_t seq;
} Env;

static AgSkillTable g_skills;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static int put_unit(Env *e, uint32_t k, const RxObjRef *on) {
    RxMutation m[2] = { { e->unit[k], PLA_F_SUPPORT, on ? pl_ref_pack(*on) : 0 },
                        { e->unit[k], PLA_F_SELF, pl_ref_pack(e->unit[k]) } };
    return rx_world_publish_external(&e->w, e->ext_unit[k], m, 2) > 0 ? 0 : -1;
}

/* Goal units per shape, bottom to top of the start, and goal facts. */
static uint32_t goal_units(uint32_t shape) { return shape == SHAPE_OTHER ? 5 : 6; }

static int env_build(Env *e, const Spec *sp) {
    memset(e, 0, sizeof *e);
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 4, 1u << 16) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    const uint32_t R_ = RX_RIGHT_READ, W = RX_RIGHT_WRITE, RW = R_ | W;
    uint32_t seed = sp->seed * 2654435761u + 17u;

    /* Vary object identities: fillers, some retired (their slots come back
     * with a new generation). */
    uint32_t nf = 1 + lcg(&seed) % 9;
    RxObjRef fill[16];
    for (uint32_t i = 0; i < nf; i++)
        if (rx_world_create(&e->w, 0x5B0F, RX_PERSIST_EPHEMERAL, RES_ENV, NULL, &fill[i]) != RX_OK)
            return -1;
    for (uint32_t i = 0; i < nf; i += 2) rx_world_retire(&e->w, fill[i]);

    e->n_goal_units = goal_units(sp->shape);
    e->n_units = e->n_goal_units + 4;
    /* Create units in a seed-dependent order. */
    uint32_t order[PLA_MAX_UNITS];
    for (uint32_t k = 0; k < e->n_units; k++) order[k] = k;
    for (uint32_t k = e->n_units; k > 1; k--) {
        uint32_t j = lcg(&seed) % k, t = order[k - 1];
        order[k - 1] = order[j];
        order[j] = t;
    }
    for (uint32_t i = 0; i < e->n_units; i++) {
        uint32_t k = order[i];
        uint64_t f[RX_MAX_FIELDS] = { 0, 0x100u + k };
        if (rx_world_create(&e->w, PL_OT_UNIT, RX_PERSIST_RESIDENT, RES_UNIT + k, f, &e->unit[k]) != RX_OK)
            return -1;
    }
    for (uint32_t k = 0; k < e->n_units; k++) e->ext_unit[k] = mint(e, SUBJ_EXTERNAL, RES_UNIT + k, W);

    /* Start arrangement. */
    const RxObjRef *U = e->unit;
    uint32_t g = e->n_goal_units;
    if (sp->shape == SHAPE_MERGE) {
        /* P = 0,1,2 and Q = 3,4,5, two towers of three. */
        put_unit(e, 0, NULL); put_unit(e, 1, &U[0]); put_unit(e, 2, &U[1]);
        put_unit(e, 3, NULL); put_unit(e, 4, &U[3]); put_unit(e, 5, &U[4]);
    } else {
        put_unit(e, 0, NULL);
        for (uint32_t k = 1; k < g; k++) put_unit(e, k, &U[k - 1]);
    }
    /* Distractors: one tower of two, two alone. */
    put_unit(e, g, NULL); put_unit(e, g + 1, &U[g]); put_unit(e, g + 2, NULL); put_unit(e, g + 3, NULL);
    uint32_t top = sp->shape == SHAPE_MERGE ? 2 : g - 1;
    if (sp->pert == PERT_STATE_SWAP) {
        /* The top two of the (first) tower change places. */
        put_unit(e, top, &U[top - 2]);
        put_unit(e, top - 1, &U[top]);
    }
    if (sp->pert == PERT_STATE_OUTSIDER) put_unit(e, g + 2, &U[top]);

    /* Goal facts. */
    uint64_t facts[PLA_MAX_FACTS] = { 0 };
    uint32_t nfct = 0;
    if (sp->shape == SHAPE_MERGE) {
        /* One tower P0 Q0 P1 Q1 P2 Q2 from the floor up. */
        uint32_t seqv[6] = { 0, 3, 1, 4, 2, 5 };
        facts[nfct++] = pla_fact(U[seqv[0]], NULL);
        for (uint32_t i = 1; i < 6; i++) facts[nfct++] = pla_fact(U[seqv[i]], &U[seqv[i - 1]]);
    } else {
        /* Reverse the tower. */
        facts[nfct++] = pla_fact(U[g - 1], NULL);
        for (uint32_t k = 0; k + 1 < g; k++) facts[nfct++] = pla_fact(U[k], &U[k + 1]);
    }
    for (uint32_t k = nfct; k > 1; k--) {                 /* listing order is not meaning */
        uint32_t j = lcg(&seed) % k;
        uint64_t t = facts[k - 1];
        facts[k - 1] = facts[j];
        facts[j] = t;
    }
    if (sp->pert == PERT_BINDING) {
        /* The goal names a unit that is then retired; a replacement takes its
         * place (same slot id, next generation). */
        uint32_t k = g - 1;
        RxObjRef old = U[k];
        uint64_t f[RX_MAX_FIELDS] = { 0, 0x100u + k };
        if (rx_world_retire(&e->w, old) != RX_OK ||
            rx_world_create(&e->w, PL_OT_UNIT, RX_PERSIST_RESIDENT, RES_UNIT + k, f, &e->unit[k]) != RX_OK)
            return -1;
        put_unit(e, k, sp->shape == SHAPE_MERGE ? &U[k - 1] : &U[k - 1]);
    }

    uint64_t z[RX_MAX_FIELDS] = { 0 };
    uint64_t wg[RX_MAX_FIELDS] = { sp->world_gen };
    uint64_t cg[RX_MAX_FIELDS] = { sp->cog_gen, 0xA1E0 };
    uint64_t st[RX_MAX_FIELDS] = { sp->pert == PERT_ENV ? 0u : 1u };
    RxObjRef fr;
    if (rx_world_create(&e->w, PL_OT_WORLDGEN, RX_PERSIST_RESIDENT, RES_ENV, wg, &e->env.obj[PL_ENV_WORLDGEN]) != RX_OK ||
        rx_world_create(&e->w, PL_OT_COGNITION, RX_PERSIST_RESIDENT, RES_ENV, cg, &e->env.obj[PL_ENV_COGNITION]) != RX_OK ||
        rx_world_create(&e->w, PL_OT_STATION, RX_PERSIST_RESIDENT, RES_ENV, st, &e->env.obj[PL_ENV_STATION]) != RX_OK ||
        rx_world_create(&e->w, PL_OT_GOALFACTS, RX_PERSIST_RESIDENT, RES_ENV, facts, &fr) != RX_OK)
        return -1;
    uint64_t gl[RX_MAX_FIELDS] = { 1000u + sp->seed, PLA_GOAL_ARRANGE, sp->max_steps, 1000, pl_ref_pack(fr) };
    if (rx_world_create(&e->w, PL_OT_GOAL, RX_PERSIST_RESIDENT, RES_ENV, gl, &e->env.obj[PL_ENV_GOAL]) != RX_OK ||
        rx_world_create(&e->w, PL_OT_DECISION, RX_PERSIST_RESIDENT, RES_DECISION, z, &e->decision) != RX_OK ||
        rx_world_create(&e->w, PL_OT_INDEX, RX_PERSIST_RESIDENT, RES_INDEX, z, &e->index) != RX_OK)
        return -1;
    e->env.obj[PL_ENV_FACTS] = fr;

    e->caps.subject = SUBJ_PLAN;
    for (uint32_t k = 0; k < e->n_units; k++) {
        e->unit_cap[k] = mint(e, SUBJ_PLAN, RES_UNIT + k, RW);
        e->caps.cap[e->caps.n].ref = e->unit_cap[k];
        e->caps.cap[e->caps.n].resource = RES_UNIT + k;
        e->caps.cap[e->caps.n].rights = RW;
        e->caps.n++;
    }
    e->ext_run = mint(e, SUBJ_EXTERNAL, RES_RUN, W);
    e->ext_decision = mint(e, SUBJ_EXTERNAL, RES_DECISION, W);
    e->ext_index = mint(e, SUBJ_EXTERNAL, RES_INDEX, W);
    e->x.cell_cap = mint(e, SUBJ_PLAN, RES_CELL, RW);
    e->x.run_cap = mint(e, SUBJ_PLAN, RES_RUN, R_);
    e->x.ext_run_cap = e->ext_run;
    e->x.cell_res = RES_CELL;
    e->x.run_res = RES_RUN;
    e->x.skills = &g_skills;
    e->x.core_class = R.core_class;

    if (sp->pert == PERT_AUTHORITY) {
        AienosCapRef office;
        aienos_cap_office(e->admin, &office);
        uint32_t k = 1;
        aienos_cap_revoke(e->admin, office, (AienosCapRef){ e->unit_cap[k].cap_id, e->unit_cap[k].generation });
    }
    if (sp->pert == PERT_RESOURCES) {
        RxResourceBudget b = e->w.budget;
        b.energy_budget = 1;
        rx_world_set_resources(&e->w, &b);
    }
    rx_world_wait_quiescent(&e->w, 30000);
    return 0;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 30000);
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

static Spec spec(uint32_t shape, uint32_t seed, Pert p) {
    return (Spec){ shape, seed, p, WORLD_GEN, COG_GEN, 32 };
}

/* ---- one goal ---- */

enum { PATH_FIRST = 1, PATH_REUSE, PATH_ADAPT, PATH_REFUSED };

/* Every candidate checked and refused: the axis its furthest binding reached. */
typedef struct {
    uint32_t n;
    PlanTemplate *t[PL_MAX_TEMPLATES];
    PlAxis ax[PL_MAX_TEMPLATES];
    int pred[PL_MAX_TEMPLATES];
    RxObjRef slots[PL_MAX_TEMPLATES][PL_MAX_SLOTS];
    int64_t crumb[PL_MAX_TEMPLATES];
} Refusals;

typedef struct {
    int path;
    PlAxis axis;
    int pred;
    int64_t decision_crumb;
    PlCost cost;
    uint64_t cog_ns, cog_cpu, cog_ins, e2e_ns;
    int plan_rc, gen_rc, executed;
    uint32_t prefix_moves;
    Refusals rf;
    PlExecResult ex;
    uint32_t status_before, moves;
    uint8_t semantic_id[32];
    PlanTemplate *t;                    /* cache entry used or stored (NULL for cold) */
    RxObjRef slots[PL_MAX_SLOTS];
} Trial;

static PlView g_view, g_view2;
static PlanTemplate g_tmp, g_tmp2;

/* Retrieval + applicability + binding. Returns the template to run or NULL;
 * *refused receives the candidate that got furthest. */
/* Retrieval + binding + applicability. Returns the template to run (slots
 * filled) or NULL. rf lists every candidate refused before the choice. */
static PlanTemplate *decide(Env *e, PlCache *c, const PlView *v, const PlaGoal *g, RxObjRef *slots,
                            PlCost *cost, Refusals *rf) {
    PlanTemplate *cand[PL_MAX_TEMPLATES];
    uint32_t n = rx_plan_cache_retrieve(c, (uint32_t)g->kind, g->shape, cand, PL_MAX_TEMPLATES, cost);
    rf->n = 0;
    RxObjRef s[PL_MAX_SLOTS];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = rf->n;
        int seen = 0;
        for (uint32_t attempt = 0;; attempt++) {
            int rc = pla_bind(cand[i], v, g, attempt, s, cost);
            if (rc == 0) break;
            PlAxis ax = PL_AX_BINDING;
            int pr = -1;
            if (rc > 0) ax = rx_plan_applicable(cand[i], &e->w, v, &e->env, s, &e->caps, cost, &pr);
            if (ax == PL_AX_OK) {
                memcpy(slots, s, sizeof s);
                return cand[i];
            }
            if (!seen || ax > rf->ax[k]) {
                rf->t[k] = cand[i];
                rf->ax[k] = ax;
                rf->pred[k] = pr;
                memcpy(rf->slots[k], s, sizeof s);
                seen = 1;
            }
        }
        if (seen) {
            cand[i]->refusals[rf->ax[k]]++;
            rf->n++;
        }
    }
    return NULL;
}

static PlAxis furthest(const Refusals *rf) {
    PlAxis a = PL_AX_SHAPE;
    for (uint32_t i = 0; i < rf->n; i++)
        if (rf->ax[i] > a) a = rf->ax[i];
    return a;
}

/* One World decision per refused candidate (or one SHAPE refusal when there
 * was none), then the accept. Returns the last crumb. */
static int64_t record(Env *e, uint64_t goal_seq, Refusals *rf, const PlanTemplate *use) {
    int64_t last = 0;
    for (uint32_t i = 0; i < rf->n; i++)
        last = rf->crumb[i] = rx_plan_decide(&e->w, e->ext_decision, e->decision, goal_seq, rf->ax[i],
                                             rf->t[i], rf->pred[i], ++e->seq, NULL);
    if (!rf->n && !use)
        last = rx_plan_decide(&e->w, e->ext_decision, e->decision, goal_seq, PL_AX_SHAPE, NULL, -1,
                              ++e->seq, NULL);
    if (use)
        last = rx_plan_decide(&e->w, e->ext_decision, e->decision, goal_seq, PL_AX_OK, use, -1,
                              ++e->seq, NULL);
    return last;
}

enum { MODE_COLD = 0, MODE_WARM = 1, MODE_WARM_ADAPT = 2, MODE_DECIDE_ONLY = 3 };

static void cog_start(Trial *t, uint64_t *w0, uint64_t *c0, uint64_t *i0) {
    (void)t;
    *w0 = now_ns();
    *c0 = cpu_ns();
    *i0 = instr();
}

static void cog_end(Trial *t, uint64_t w0, uint64_t c0, uint64_t i0) {
    t->cog_ins = instr() - i0;
    t->cog_cpu = cpu_ns() - c0;
    t->cog_ns = now_ns() - w0;
}

static int solve(Env *e, PlCache *c, int mode, Trial *t) {
    memset(t, 0, sizeof *t);
    uint64_t e0 = now_ns(), w0, c0, i0;
    cog_start(t, &w0, &c0, &i0);
    rx_plan_view(&e->w, &g_view);
    PlaGoal g;
    if (pla_goal_read(&g_view, &e->env, &g, &t->cost) != 0) return -1;
    PlanTemplate *use = NULL;
    Refusals *rf = &t->rf;
    rf->n = 0;
    if (mode != MODE_COLD) {
        use = decide(e, c, &g_view, &g, t->slots, &t->cost, rf);
        t->axis = use ? PL_AX_OK : furthest(rf);
        if (use) {
            t->path = PATH_REUSE;
            t->status_before = use->status;
            memcpy(t->semantic_id, use->semantic_id, 32);
        }
    }
    const PlanTemplate *run = use;
    if (!use && mode == MODE_DECIDE_ONLY) {
        cog_end(t, w0, c0, i0);
        t->path = PATH_REFUSED;
        t->decision_crumb = record(e, g.seq, rf, NULL);
        t->e2e_ns = now_ns() - e0;
        return 0;
    }
    if (!use) {
        PlaTarget tg;
        PlaPlan plan;
        const uint8_t *parent = NULL;
        uint32_t origin = PL_ORIGIN_SEARCH;
        int adapted = 0;
        if (mode == MODE_WARM_ADAPT) {
            /* For each candidate refused only on the World state it needs:
             * reach that state, then run it. Keep the shortest whole plan. */
            for (uint32_t i = 0; i < rf->n; i++) {
                if (rf->ax[i] != PL_AX_STATE) continue;
                PlaTarget tt;
                PlaPlan prefix, rest;
                if (pla_target_from_template(rf->t[i], rf->slots[i], &tt) != 0) continue;
                if (pla_plan(&g_view, &g, &tt, MAX_EXPAND, &prefix, &t->cost) != 0) continue;
                if (pla_template_moves(rf->t[i], rf->slots[i], &rest) != 0) continue;
                if (prefix.n + rest.n > PLA_MAX_MOVES || (adapted && prefix.n + rest.n >= plan.n)) continue;
                plan = prefix;
                t->prefix_moves = prefix.n;
                for (uint32_t k = 0; k < rest.n; k++) {
                    plan.mover[plan.n] = rest.mover[k];
                    plan.dest[plan.n] = rest.dest[k];
                    plan.to_floor[plan.n++] = rest.to_floor[k];
                }
                adapted = 1;
                origin = PL_ORIGIN_ADAPTED;
                parent = rf->t[i]->semantic_id;
            }
        }
        if (!adapted) {
            pla_target_from_goal(&g, &tg);
            t->plan_rc = pla_plan(&g_view, &g, &tg, MAX_EXPAND, &plan, &t->cost);
            if (t->plan_rc != 0) { cog_end(t, w0, c0, i0); return -2; }
        }
        const uint64_t *cf = g_view.o[e->env.obj[PL_ENV_COGNITION].id].field;
        t->gen_rc = pla_generalize(&g_view, &g, &plan, origin, parent, cf[0], &g_tmp, t->slots);
        if (t->gen_rc != 0) { cog_end(t, w0, c0, i0); return -3; }
        t->path = adapted ? PATH_ADAPT : PATH_FIRST;
        memcpy(t->semantic_id, g_tmp.semantic_id, 32);
        run = &g_tmp;
    }
    cog_end(t, w0, c0, i0);
    if (mode != MODE_COLD) t->decision_crumb = record(e, g.seq, rf, use);
    e->x.reference_first = use && use->status == PL_STATUS_CANDIDATE;
    rx_plan_execute(run, &e->w, &e->env, t->slots, &e->caps, &e->x, 1, &e->rs, pla_legal, &t->ex);
    t->executed = 1;
    t->moves = run->n_steps;
    if (use) {
        rx_plan_record_use(use, t->ex.success);
        t->t = use;
    } else if (mode != MODE_COLD && t->ex.success) {
        rx_plan_view(&e->w, &g_view2);
        uint64_t wg = g_view2.o[e->env.obj[PL_ENV_WORLDGEN].id].field[0];
        uint64_t cg = g_view2.o[e->env.obj[PL_ENV_COGNITION].id].field[0];
        t->t = rx_plan_cache_store(c, &g_tmp, wg, cg);
    }
    if (t->t) rx_plan_publish_index(&e->w, e->ext_index, e->index, c, t->t);
    t->e2e_ns = now_ns() - e0;
    return 0;
}

/* ---- statistics ---- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t median(const uint64_t *v, uint32_t n) {
    if (!n) return 0;
    uint64_t *c = malloc(n * sizeof *c);
    memcpy(c, v, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_u64);
    uint64_t m = n % 2 ? c[n / 2] : (c[n / 2 - 1] + c[n / 2]) / 2;
    free(c);
    return m;
}

static void add_series(Series *s, const Trial *t) {
    uint32_t i = s->n++;
    s->ops[i] = pl_cost_ops(&t->cost);
    s->cpu[i] = t->cog_cpu;
    s->wall[i] = t->cog_ns;
    s->ins[i] = t->cog_ins;
    s->e2e[i] = t->e2e_ns;
    s->exec[i] = t->ex.compile_ns + t->ex.reference_ns + t->ex.run_ns + t->ex.verify_ns;
    s->compile[i] = t->ex.compile_ns;
    s->run[i] = t->ex.run_ns;
    s->verify[i] = t->ex.verify_ns;
}

static int goal_holds(Env *e) {
    PlaGoal g;
    rx_plan_view(&e->w, &g_view2);
    if (pla_goal_read(&g_view2, &e->env, &g, NULL) != 0) return 0;
    for (uint32_t i = 0; i < g.n_facts; i++) {
        uint64_t f = g_view2.o[g.x[i].id].field[PLA_F_SUPPORT];
        uint64_t want = g.floor[i] ? 0 : pl_ref_pack((RxObjRef){ g.t[i].id, g_view2.o[g.t[i].id].generation });
        if (f != want) return 0;
    }
    return pla_legal(&g_view2);
}

/* ---- phases ---- */

static PlCache g_cache;
static Env *g_a, *g_b;

static void dump_cache(const char *when) {
    if (!getenv("PLAN_REUSE_DEBUG")) return;
    fprintf(stderr, "  cache %s: %u templates\n", when, g_cache.n);
    for (uint32_t i = 0; i < g_cache.n; i++) {
        const PlanTemplate *t = g_cache.t[i];
        fprintf(stderr, "    [%u] id %016llx shape %016llx steps %u slots %u/%u origin %u status %s wg %llu uses %llu\n", i,
                (unsigned long long)pl_word(t->semantic_id, 0), (unsigned long long)pl_word(t->goal_shape, 0),
                t->n_steps, t->n_goal_slots, t->n_slots, t->ancestry.origin, rx_plan_status_name(t->status),
                (unsigned long long)t->verified_world_gen, (unsigned long long)t->uses);
    }
}

static void t_repeated(uint32_t shape) {
    printf("[*] repeated workload: %s, %u instances\n",
           shape == SHAPE_MERGE ? "merge two towers" : "reverse a tower", N_REPEAT);
    uint8_t first_id[32];
    uint8_t real[N_REPEAT][32];
    uint32_t n_real = 0;
    R.semantic_ids_equal[shape] = 1;
    for (uint32_t i = 0; i < N_REPEAT; i++) {
        Spec sp = spec(shape, 100 * shape + i, PERT_NONE);
        Trial cold, warm;
        CHECK(env_build(g_a, &sp) == 0, "build cold %u", i);
        int rc = solve(g_a, NULL, MODE_COLD, &cold);
        CHECK(rc == 0 && cold.ex.success, "cold %u rc %d plan %d gen %d success %d fail %u", i, rc,
              cold.plan_rc, cold.gen_rc, cold.ex.success, cold.ex.fail);
        int cold_goal = goal_holds(g_a);
        CHECK(cold_goal, "cold goal %u", i);
        R.cold_success[shape] += cold.ex.success && cold_goal;
        if (i == 0) memcpy(first_id, cold.semantic_id, 32);
        else if (memcmp(first_id, cold.semantic_id, 32) != 0) R.semantic_ids_equal[shape] = 0;
        add_series(&R.first[shape], &cold);
        env_stop(g_a);

        CHECK(env_build(g_b, &sp) == 0, "build warm %u", i);
        rc = solve(g_b, &g_cache, MODE_WARM, &warm);
        int warm_goal = goal_holds(g_b);
        CHECK(rc == 0 && warm.ex.success && warm_goal, "warm %u rc %d path %d axis %s success %d fail %u",
              i, rc, warm.path, rx_plan_axis_name(warm.axis), warm.ex.success, warm.ex.fail);
        CHECK(warm.decision_crumb > 0 && rx_world_crumb(&g_b->w, (uint64_t)warm.decision_crumb),
              "decision crumb %u", i);
        if (i == 0) {
            CHECK(warm.path == PATH_FIRST && warm.t && warm.t->status == PL_STATUS_CANDIDATE,
                  "first instance stores a CANDIDATE");
        } else {
            CHECK(warm.path == PATH_REUSE, "instance %u reused (axis %s)", i, rx_plan_axis_name(warm.axis));
            if (warm.path == PATH_REUSE) {
                R.reuse_accepted[shape]++;
                R.accepted_total++;
                if (!(warm.ex.success && warm_goal)) R.false_applicable++;
                R.reuse_success[shape] += warm.ex.success && warm_goal;
                add_series(&R.reuse[shape], &warm);
                if (i == 1) {
                    CHECK(warm.status_before == PL_STATUS_CANDIDATE && warm.ex.reference_outcome == AG_RUN_SUCCESS,
                          "CANDIDATE reuse runs the reference first");
                    CHECK(warm.t->status == PL_STATUS_VERIFIED, "verified reuse promotes");
                    R.candidate_reference_runs++;
                    R.promotions += warm.t->status == PL_STATUS_VERIFIED;
                } else {
                    CHECK(warm.status_before == PL_STATUS_VERIFIED && warm.ex.reference_outcome == 0,
                          "VERIFIED reuse skips the reference");
                }
                memcpy(real[n_real++], warm.ex.realization_id, 32);
                CHECK(memcmp(warm.semantic_id, first_id, 32) == 0, "reuse %u same semantic id", i);
            }
        }
        /* Outcome and evidence are the first-use outcome and evidence. */
        int same = cold.ex.success == warm.ex.success && cold.moves == warm.moves && cold_goal == warm_goal;
        int ev = cold.ex.evidence_present == warm.ex.evidence_present &&
                 cold.ex.evidence_required == warm.ex.evidence_required &&
                 warm.ex.evidence_present == warm.ex.evidence_required;
        CHECK(same, "outcome %u: moves %u vs %u", i, cold.moves, warm.moves);
        CHECK(ev, "evidence %u: %u/%u vs %u/%u", i, cold.ex.evidence_present, cold.ex.evidence_required,
              warm.ex.evidence_present, warm.ex.evidence_required);
        CHECK(warm.ex.crumbs_checked > 0 && !(warm.ex.fail & PL_FAIL_CRUMBS), "crumbs verify %u", i);
        R.outcome_equal[shape] += same;
        R.evidence_equal[shape] += ev;
        R.moves[shape] = cold.moves;
        R.evidence_words[shape] = warm.ex.evidence_present;
        R.nodes[shape] = warm.ex.nodes;
        env_stop(g_b);
    }
    R.realizations_distinct[shape] = 1;
    for (uint32_t a = 0; a < n_real; a++)
        for (uint32_t b = a + 1; b < n_real; b++)
            if (memcmp(real[a], real[b], 32) == 0) R.realizations_distinct[shape] = 0;
    CHECK(R.semantic_ids_equal[shape], "one semantic id across bindings");
    CHECK(R.realizations_distinct[shape], "distinct realizations");
    Series *f = &R.first[shape], *u = &R.reuse[shape];
    if (!R.perf_ok) printf("    (instruction counter unavailable: perf_event_paranoid restricts it)\n");
    printf("    first use: median %llu cognitive ops, %.1f us cpu, %llu instructions; e2e %.1f us\n",
           (unsigned long long)median(f->ops, f->n), median(f->cpu, f->n) / 1e3,
           (unsigned long long)median(f->ins, f->n), median(f->e2e, f->n) / 1e3);
    printf("    reuse:     median %llu cognitive ops, %.1f us cpu, %llu instructions; e2e %.1f us\n",
           (unsigned long long)median(u->ops, u->n), median(u->cpu, u->n) / 1e3,
           (unsigned long long)median(u->ins, u->n), median(u->e2e, u->n) / 1e3);
    printf("    plan %u moves, %u graph nodes after optimisation, %u evidence words; accepted %u, succeeded %u\n",
           R.moves[shape], R.nodes[shape], R.evidence_words[shape], R.reuse_accepted[shape],
           R.reuse_success[shape]);
}

static PlAxis expected_axis(Pert p) {
    switch (p) {
    case PERT_BINDING: return PL_AX_BINDING;
    case PERT_GOAL: return PL_AX_GOAL;
    case PERT_WORLD_GEN: return PL_AX_WORLD_GEN;
    case PERT_COG_GEN: return PL_AX_COG_GEN;
    case PERT_ENV: return PL_AX_ENVIRONMENT;
    case PERT_STATE_SWAP: case PERT_STATE_OUTSIDER: return PL_AX_STATE;
    case PERT_AUTHORITY: return PL_AX_AUTHORITY;
    case PERT_RESOURCES: return PL_AX_RESOURCES;
    default: return PL_AX_SHAPE;
    }
}

static void adversarial_case(uint32_t shape, uint32_t seed, Pert p, const PlanTemplate *stored) {
    Spec sp = spec(shape, seed, p);
    if (p == PERT_WORLD_GEN) sp.world_gen = WORLD_GEN + 1;
    if (p == PERT_COG_GEN) sp.cog_gen = COG_GEN + 1;
    if (p == PERT_GOAL) sp.max_steps = stored ? stored->n_steps - 1 : 1;
    PlAxis want = expected_axis(p);
    Trial t;
    CHECK(env_build(g_b, &sp) == 0, "build adversarial");
    solve(g_b, &g_cache, MODE_DECIDE_ONLY, &t);
    R.adv_cases++;
    R.adv_by_axis[want]++;
    if (t.path == PATH_REUSE) {
        /* Accepted: it ran. Counted against false applicability if it did not verify. */
        R.accepted_total++;
        if (!(t.ex.success && goal_holds(g_b))) R.false_applicable++;
        CHECK(0, "adversarial %s seed %u was accepted", rx_plan_axis_name(want), seed);
    } else {
        R.adv_refused++;
        R.adv_refused_by_axis[want]++;
        /* The template made for this start arrangement must be refused on the
         * perturbed axis. Other templates of the same goal shape (a different
         * start) may be refused on an earlier or later axis of their own. */
        PlAxis got = PL_AX_SHAPE;
        int pr = -1, found = !stored;
        for (uint32_t i = 0; i < t.rf.n; i++)
            if (t.rf.t[i] == stored) { got = t.rf.ax[i]; pr = t.rf.pred[i]; found = 1; }
        R.adv_axis_match += found && got == want;
        CHECK(found && got == want, "adversarial seed %u: its template refused on %s (pred %d), expected %s",
              seed, rx_plan_axis_name(got), pr, rx_plan_axis_name(want));
        /* Every refusal is a World decision with its own crumb. */
        int crumb_ok = 1;
        uint32_t n_rec = t.rf.n ? t.rf.n : 1;
        for (uint32_t i = 0; i < n_rec; i++) {
            int64_t id = t.rf.n ? t.rf.crumb[i] : t.decision_crumb;
            const RxCrumb *k = id > 0 ? rx_world_crumb(&g_b->w, (uint64_t)id) : NULL;
            crumb_ok &= k && k->kind == RX_CRUMB_EXTERNAL && k->n_outputs == 1 &&
                        k->outputs[0].obj.id == g_b->decision.id;
        }
        RxObject d;
        crumb_ok = crumb_ok && rx_world_read(&g_b->w, g_b->decision, &d) == RX_OK &&
                   d.field[1] == (uint64_t)(t.rf.n ? t.rf.ax[t.rf.n - 1] : PL_AX_SHAPE);
        R.adv_decision_crumbs += crumb_ok;
        CHECK(crumb_ok, "every refusal recorded in the World");
        /* Blind replay: bind and run it anyway. */
        if (stored) {
            PlaGoal g;
            RxObjRef s[PL_MAX_SLOTS];
            rx_plan_view(&g_b->w, &g_view);
            if (pla_goal_read(&g_view, &g_b->env, &g, NULL) == 0 && pla_bind(stored, &g_view, &g, 0, s, NULL) > 0) {
                PlExecResult ex;
                g_b->x.reference_first = 0;
                rx_plan_execute(stored, &g_b->w, &g_b->env, s, &g_b->caps, &g_b->x, 1, &g_b->rs, pla_legal, &ex);
                int ok = ex.success && goal_holds(g_b);
                R.blind_by_axis[want]++;
                R.blind_ok_by_axis[want] += ok;
                /* A violation: it did not reach the goal, or it broke a stated condition. */
                int violated = !ok || p == PERT_GOAL || p == PERT_ENV || p == PERT_WORLD_GEN ||
                               p == PERT_COG_GEN;
                R.blind_violation_by_axis[want] += violated;
            }
        }
    }
    env_stop(g_b);
}

static void t_adversarial(void) {
    printf("[*] adversarial goals of stored shapes: every applicability axis\n");
    PlanTemplate *rev = NULL, *mer = NULL;
    for (uint32_t i = 0; i < g_cache.n; i++) {
        if (g_cache.t[i]->ancestry.origin != PL_ORIGIN_SEARCH) continue;
        if (g_cache.t[i]->n_goal_slots == 6 && !rev && g_cache.t[i]->graph.n_nodes && g_cache.t[i]->success.n == 6) {
            /* distinguish by shape: the first stored is reverse */
            rev = g_cache.t[i];
        } else if (!mer) {
            mer = g_cache.t[i];
        }
    }
    const Pert perts[] = { PERT_BINDING, PERT_GOAL, PERT_WORLD_GEN, PERT_COG_GEN, PERT_ENV,
                           PERT_STATE_SWAP, PERT_STATE_OUTSIDER, PERT_AUTHORITY, PERT_RESOURCES };
    for (uint32_t k = 0; k < sizeof perts / sizeof perts[0]; k++)
        for (uint32_t i = 0; i < N_ADV; i++) {
            uint32_t shape = i % 2 ? SHAPE_MERGE : SHAPE_REVERSE;
            adversarial_case(shape, 5000 + 100 * k + i, perts[k], shape == SHAPE_MERGE ? mer : rev);
        }
    for (uint32_t i = 0; i < N_ADV; i++) adversarial_case(SHAPE_OTHER, 7000 + i, PERT_NONE, NULL);
    printf("    %u adversarial goals, %u refused (%u on the expected axis), %u refusals recorded as World decisions\n",
           R.adv_cases, R.adv_refused, R.adv_axis_match, R.adv_decision_crumbs);
    for (uint32_t a = 1; a < PL_AX_COUNT; a++)
        if (R.adv_by_axis[a])
            printf("      %-22s %2u refused of %2u; blind replay reached the goal %u of %u, violated %u\n",
                   rx_plan_axis_name((PlAxis)a), R.adv_refused_by_axis[a], R.adv_by_axis[a],
                   R.blind_ok_by_axis[a], R.blind_by_axis[a], R.blind_violation_by_axis[a]);
}

static void t_rederive(void) {
    printf("[*] new World generation: refuse, re-derive, same semantic id, reuse again\n");
    uint32_t n_before = g_cache.n;
    Spec sp = spec(SHAPE_REVERSE, 8000, PERT_NONE);
    sp.world_gen = WORLD_GEN + 1;
    Trial t;
    CHECK(env_build(g_b, &sp) == 0, "build");
    solve(g_b, &g_cache, MODE_WARM, &t);
    CHECK(t.path == PATH_FIRST && t.ex.success, "refused then planned (path %d)", t.path);
    PlanTemplate *have = t.t;
    CHECK(g_cache.n == n_before && have && have->rederivations == 1 &&
          have->verified_world_gen == WORLD_GEN + 1, "re-derived into the existing record");
    R.rederived_same_id = g_cache.n == n_before && have && have->rederivations == 1;
    env_stop(g_b);
    sp.seed = 8001;
    CHECK(env_build(g_b, &sp) == 0, "build");
    solve(g_b, &g_cache, MODE_WARM, &t);
    CHECK(t.path == PATH_REUSE && t.ex.success && t.ex.reference_outcome == AG_RUN_SUCCESS,
          "reused under the new generation after re-derivation");
    R.rederived_then_reused = t.path == PATH_REUSE && t.ex.success;
    if (t.path == PATH_REUSE) {
        R.accepted_total++;
        if (!t.ex.success) R.false_applicable++;
    }
    env_stop(g_b);
}

static void t_adapt(void) {
    printf("[*] adaptation: an outsider sits on the tower\n");
    uint64_t ops[N_ADV], cold_ops[N_ADV], cpu[N_ADV], cold_cpu[N_ADV];
    uint32_t na = 0;
    for (uint32_t i = 0; i < N_ADV; i++) {
        Spec sp = spec(SHAPE_REVERSE, 9000 + i, PERT_STATE_OUTSIDER);
        sp.world_gen = WORLD_GEN + 1;   /* the generation the organism is at after t_rederive */
        Trial cold, warm;
        CHECK(env_build(g_a, &sp) == 0, "build");
        solve(g_a, NULL, MODE_COLD, &cold);
        env_stop(g_a);
        CHECK(env_build(g_b, &sp) == 0, "build");
        solve(g_b, &g_cache, MODE_WARM_ADAPT, &warm);
        int ok = warm.ex.success && goal_holds(g_b);
        CHECK(ok, "adapt %u path %d", i, warm.path);
        if (getenv("PLAN_REUSE_DEBUG")) fprintf(stderr, "  adapt %u path %d prefix %u total %u cold %u\n", i, warm.path, warm.prefix_moves, warm.moves, cold.moves);
        if (i == 0) {
            CHECK(warm.path == PATH_ADAPT, "first outsider goal adapts (path %d)", warm.path);
            R.adapt_n++;
            R.adapt_ok += ok && warm.path == PATH_ADAPT;
            ops[na] = pl_cost_ops(&warm.cost);
            cpu[na] = warm.cog_cpu;
            cold_ops[na] = pl_cost_ops(&cold.cost);
            cold_cpu[na++] = cold.cog_cpu;
            R.adapt_steps = warm.moves;
            R.adapt_cold_steps = cold.moves;
            CHECK(warm.t && warm.t->ancestry.origin == PL_ORIGIN_ADAPTED && warm.t->ancestry.has_parent,
                  "adapted template records its parent");
        } else {
            R.adapt_then_reuse += warm.path == PATH_REUSE && ok;
            if (warm.path == PATH_REUSE) {
                R.accepted_total++;
                if (!ok) R.false_applicable++;
            }
        }
        env_stop(g_b);
    }
    R.adapt_ops_med = median(ops, na);
    R.adapt_cold_ops_med = median(cold_ops, na);
    R.adapt_cpu_med = median(cpu, na);
    R.adapt_cold_cpu_med = median(cold_cpu, na);
    printf("    adapted: %llu ops, %.1f us cpu, %u steps; full search: %llu ops, %.1f us cpu, %u steps; "
           "then reused %u of %u\n", (unsigned long long)R.adapt_ops_med, R.adapt_cpu_med / 1e3,
           R.adapt_steps, (unsigned long long)R.adapt_cold_ops_med, R.adapt_cold_cpu_med / 1e3,
           R.adapt_cold_steps, R.adapt_then_reuse, N_ADV - 1);
}

static void t_identity(void) {
    printf("[*] identity\n");
    PlanTemplate *t = g_cache.t[0];
    static uint8_t a[1u << 16], b[1u << 16];
    size_t na = rx_plan_encode(t, a, sizeof a);
    CHECK(na > 0 && rx_plan_decode(a, na, &g_tmp) == 0, "decode");
    size_t nb = rx_plan_encode(&g_tmp, b, sizeof b);
    R.id_roundtrip = na == nb && memcmp(a, b, na) == 0;
    CHECK(R.id_roundtrip, "canonical round trip");
    /* Builder order. */
    g_tmp = *t;
    PlPredList *lists[] = { &g_tmp.pre, &g_tmp.state, &g_tmp.env, &g_tmp.invariants, &g_tmp.success };
    for (uint32_t l = 0; l < 5; l++)
        for (uint32_t i = 0; i < lists[l]->n / 2; i++) {
            PlPred x = lists[l]->p[i];
            lists[l]->p[i] = lists[l]->p[lists[l]->n - 1 - i];
            lists[l]->p[lists[l]->n - 1 - i] = x;
        }
    rx_plan_identify(&g_tmp);
    R.id_order_invariant = memcmp(g_tmp.semantic_id, t->semantic_id, 32) == 0;
    CHECK(R.id_order_invariant, "predicate order does not change the id");
    /* Every semantic field changes it. */
    for (int m = 0; m < 10; m++) {
        g_tmp = *t;
        switch (m) {
        case 0: g_tmp.goal_kind++; break;
        case 1: g_tmp.goal_shape[3] ^= 1; break;
        case 2: g_tmp.slots[0].rights ^= RX_RIGHT_WRITE; break;
        case 3: g_tmp.state.p[0].value ^= 1; break;
        case 4: g_tmp.env.p[0].value ^= 1; break;
        case 5: g_tmp.success.p[0].slot2 ^= 1; break;
        case 6: g_tmp.n_steps++; break;
        case 7: g_tmp.step_need.energy_cost++; break;
        case 8: g_tmp.invariants.n--; break;
        case 9: g_tmp.graph.nodes[0].field ^= 1; break;
        }
        rx_plan_identify(&g_tmp);
        R.id_mutations++;
        R.id_mutations_changed += memcmp(g_tmp.semantic_id, t->semantic_id, 32) != 0;
    }
    CHECK(R.id_mutations_changed == R.id_mutations, "semantic changes change the id");
    /* Non-semantic fields do not. */
    for (int m = 0; m < 4; m++) {
        g_tmp = *t;
        switch (m) {
        case 0: g_tmp.status = PL_STATUS_STALE; break;
        case 1: g_tmp.ancestry.goal_seq += 99; g_tmp.ancestry.origin = PL_ORIGIN_ADAPTED; break;
        case 2: g_tmp.uses += 7; g_tmp.refusals[3] += 2; break;
        case 3: g_tmp.verified_world_gen += 5; break;
        }
        rx_plan_identify(&g_tmp);
        R.id_nonsemantic++;
        R.id_nonsemantic_same += memcmp(g_tmp.semantic_id, t->semantic_id, 32) == 0;
    }
    CHECK(R.id_nonsemantic_same == R.id_nonsemantic, "ancestry and status are not semantics");
}

/* ---- energy (reported, not gated) ---- */

static int hwmon_dir(char *out, size_t n) {
    for (int i = 0; i < 16; i++) {
        char p[96], name[64] = { 0 };
        snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int ok = fgets(name, sizeof name, f) != NULL;
        fclose(f);
        if (ok && strncmp(name, "aien_spbm", 9) == 0) {
            snprintf(out, n, "/sys/class/hwmon/hwmon%d", i);
            return 0;
        }
    }
    return -1;
}

static int energy_uj(const char *dir, int ch, uint64_t *v) {
    char p[160];
    snprintf(p, sizeof p, "%s/energy%d_input", dir, ch);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    unsigned long long x = 0;
    int ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    *v = x;
    return ok ? 0 : -1;
}

static int other_load(void) {
    FILE *f = popen("pgrep -f 'rx_r15|r15_qualify|r15_gpu_load' >/dev/null 2>&1 && echo busy", "r");
    if (!f) return 0;
    char b[16] = { 0 };
    int busy = fgets(b, sizeof b, f) != NULL;
    pclose(f);
    return busy;
}

static void t_energy(void) {
    printf("[*] energy per cognition (package telemetry; reported, not gated)\n");
    char dir[96];
    if (hwmon_dir(dir, sizeof dir) != 0) {
        snprintf(R.energy_why, sizeof R.energy_why, "no aien_spbm hwmon reader");
        printf("    unavailable: %s\n", R.energy_why);
        return;
    }
    if (other_load()) {
        snprintf(R.energy_why, sizeof R.energy_why, "an R15 measurement was running; not measured");
        printf("    %s\n", R.energy_why);
        return;
    }
    Spec sp = spec(SHAPE_REVERSE, 100, PERT_NONE);
    sp.world_gen = WORLD_GEN + 1;
    CHECK(env_build(g_b, &sp) == 0, "build");
    PlaGoal g;
    const uint64_t T = 4000000000ull;
    uint64_t p0, p1, c0, c1, t0, t1;
    /* Idle baseline. */
    energy_uj(dir, 1, &p0); energy_uj(dir, 3, &c0); t0 = now_ns();
    struct timespec ts = { 3, 0 };
    nanosleep(&ts, NULL);
    energy_uj(dir, 1, &p1); energy_uj(dir, 3, &c1); t1 = now_ns();
    double idle_pkg_w = (double)(p1 - p0) / 1e6 / ((double)(t1 - t0) / 1e9);
    double idle_cpup_w = (double)(c1 - c0) / 1e6 / ((double)(t1 - t0) / 1e9);
    R.idle_w = idle_pkg_w;
    for (int phase = 0; phase < 2; phase++) {
        uint64_t loops = 0;
        energy_uj(dir, 1, &p0); energy_uj(dir, 3, &c0); t0 = now_ns();
        do {
            PlCost cost;
            memset(&cost, 0, sizeof cost);
            rx_plan_view(&g_b->w, &g_view);
            pla_goal_read(&g_view, &g_b->env, &g, &cost);
            if (phase == 0) {
                PlaTarget tg;
                PlaPlan plan;
                RxObjRef s[PL_MAX_SLOTS];
                pla_target_from_goal(&g, &tg);
                pla_plan(&g_view, &g, &tg, MAX_EXPAND, &plan, &cost);
                pla_generalize(&g_view, &g, &plan, PL_ORIGIN_SEARCH, NULL, COG_GEN, &g_tmp2, s);
            } else {
                RxObjRef s[PL_MAX_SLOTS];
                static Refusals rf;
                if (!decide(g_b, &g_cache, &g_view, &g, s, &cost, &rf)) R.energy_reuse_refused++;
            }
            loops++;
        } while (now_ns() - t0 < T);
        energy_uj(dir, 1, &p1); energy_uj(dir, 3, &c1); t1 = now_ns();
        double secs = (double)(t1 - t0) / 1e9;
        double pkg_mj = (double)(p1 - p0) / 1e3 - idle_pkg_w * secs * 1e3;
        double cpup_mj = (double)(c1 - c0) / 1e3 - idle_cpup_w * secs * 1e3;
        if (phase == 0) {
            R.cold_loops = loops;
            R.cold_pkg_mj_per = pkg_mj / (double)loops;
            R.cold_cpup_mj_per = cpup_mj / (double)loops;
        } else {
            R.reuse_loops = loops;
            R.reuse_pkg_mj_per = pkg_mj / (double)loops;
            R.reuse_cpup_mj_per = cpup_mj / (double)loops;
        }
    }
    CHECK(R.energy_reuse_refused == 0, "energy reuse loop measured accepted reuses (%llu refused)",
          (unsigned long long)R.energy_reuse_refused);
    R.loop_ns = T;
    R.energy_ok = 1;
    env_stop(g_b);
    printf("    idle package %.2f W; first-use cognition %.4f mJ (package) / %.4f mJ (P cores) over %llu loops; "
           "reuse cognition %.6f mJ / %.6f mJ over %llu loops\n", idle_pkg_w, R.cold_pkg_mj_per,
           R.cold_cpup_mj_per, (unsigned long long)R.cold_loops, R.reuse_pkg_mj_per, R.reuse_cpup_mj_per,
           (unsigned long long)R.reuse_loops);
}

/* ---- placement and receipt ---- */

static void place(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    int count = 0;
    char *p = R.cpus;
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = 0; c < n; c++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", c);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        unsigned long long midr = 0;
        int ok = fscanf(fp, "%llx", &midr) == 1;
        fclose(fp);
        if (ok && ((midr >> 4) & 0xfffu) == 0xd85u) {
            CPU_SET(c, &set);
            count++;
            R.core_class = 0xd85u;
            if (p < R.cpus + sizeof R.cpus - 5) p += sprintf(p, "%s%ld", count > 1 ? "," : "", c);
        }
    }
    if (count == 0 || sched_setaffinity(0, sizeof set, &set) != 0) {
        snprintf(R.cpus, sizeof R.cpus, "unpinned");
        return;
    }
    printf("[*] placed on %d Cortex-X925 cores: %s\n", count, R.cpus);
}

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

typedef struct { int pass; double ops_ratio[2], cpu_ratio[2], e2e_ratio[2]; } Gates;

static int g1, g2, g3, g4, g5, g6;

static Gates gates(void) {
    Gates G;
    memset(&G, 0, sizeof G);
    g1 = g2 = g6 = 1;
    for (int s = 0; s < 2; s++) {
        Series *f = &R.first[s], *u = &R.reuse[s];
        uint64_t fo = median(f->ops, f->n), uo = median(u->ops, u->n);
        uint64_t fc = median(f->cpu, f->n), uc = median(u->cpu, u->n);
        uint64_t fe = median(f->e2e, f->n), ue = median(u->e2e, u->n);
        G.ops_ratio[s] = uo ? (double)fo / (double)uo : 0;
        G.cpu_ratio[s] = uc ? (double)fc / (double)uc : 0;
        G.e2e_ratio[s] = ue ? (double)fe / (double)ue : 0;
        g1 &= u->n > 0 && uo * 20 <= fo;
        g2 &= u->n > 0 && uc * 10 <= fc;
        g6 &= u->n > 0 && ue < fe;
    }
    g3 = 1;
    for (int s = 0; s < 2; s++)
        g3 &= R.reuse_accepted[s] == N_REPEAT - 1 && R.reuse_success[s] == R.reuse_accepted[s] &&
              R.outcome_equal[s] == N_REPEAT && R.evidence_equal[s] == N_REPEAT;
    uint32_t per_axis_ok = 1;
    for (uint32_t a = PL_AX_SHAPE; a <= PL_AX_RESOURCES; a++)
        per_axis_ok &= R.adv_by_axis[a] >= 8 && R.adv_refused_by_axis[a] == R.adv_by_axis[a];
    g4 = R.false_applicable == 0 && R.adv_cases >= 80 && R.adv_refused == R.adv_cases &&
         R.adv_decision_crumbs == R.adv_refused && per_axis_ok;
    g5 = R.id_roundtrip && R.id_order_invariant && R.id_mutations_changed == R.id_mutations &&
         R.id_nonsemantic_same == R.id_nonsemantic && R.semantic_ids_equal[0] && R.semantic_ids_equal[1] &&
         R.realizations_distinct[0] && R.realizations_distinct[1] && R.rederived_same_id;
    G.pass = g1 && g2 && g3 && g4 && g5 && g6 && g_fail == 0;
    return G;
}

static void json_series(FILE *fp, const char *key, const Series *s) {
    fprintf(fp, "      \"%s\": {\"n\": %u, \"median_cognitive_ops\": %llu, \"median_cognition_cpu_ns\": %llu, "
                "\"median_cognition_wall_ns\": %llu, \"median_instructions\": %llu, \"median_execution_ns\": %llu, "
                "\"median_end_to_end_ns\": %llu, \"median_compile_ns\": %llu, \"median_graph_run_ns\": %llu, \"median_verify_ns\": %llu}",
            key, s->n, (unsigned long long)median(s->ops, s->n), (unsigned long long)median(s->cpu, s->n),
            (unsigned long long)median(s->wall, s->n), (unsigned long long)median(s->ins, s->n),
            (unsigned long long)median(s->exec, s->n), (unsigned long long)median(s->e2e, s->n),
            (unsigned long long)median(s->compile, s->n), (unsigned long long)median(s->run, s->n),
            (unsigned long long)median(s->verify, s->n));
}

static void write_receipt(void) {
    Gates G = gates();
    char path[512];
    if (omega_evidence_path("PLAN_REUSE/rx_plan_reuse_receipt.json", path, sizeof path) != 0) return;
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
    fprintf(fp, "{\n  \"schema\": \"OMEGA_PLAN_REUSE_V1\",\n  \"run_id\": \"%s\",\n", omega_evidence_run_id());
    fprintf(fp, "  \"candidate_commit\": %s%s%s,\n  \"candidate_bound\": %s,\n  \"run_commit\": \"%s\",\n"
                "  \"tree_dirty\": %s,\n  \"aienos_commit\": %s%s%s,\n",
            candidate ? "\"" : "", candidate ? candidate : "null", candidate ? "\"" : "", bound ? "true" : "false",
            commit, omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "");
    fprintf(fp, "  \"checks\": %d,\n  \"failures\": %d,\n  \"test_binary_sha256\": \"%s\",\n", g_checks, g_fail, digest);
    fprintf(fp, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", \"cpus\": \"%s\", "
                "\"core_class_midr_part\": \"0x%llx\"},\n",
            u.sysname, u.release, u.machine, R.cpus, (unsigned long long)R.core_class);
    fprintf(fp, "  \"scope\": \"host processor; native AIENOS authority library; resident reaction world; "
                "one planning domain (arrangement); planner is explicit A* search, not neural\",\n");
    fprintf(fp, "  \"cognitive_ops_definition\": \"first use: states expanded + successors generated + heuristic "
                "evaluations + goal-shape steps; reuse: goal-shape steps + candidates retrieved + predicates "
                "evaluated + binding attempts\",\n");
    fprintf(fp, "  \"workloads\": {\n");
    const char *names[2] = { "reverse_tower_6", "merge_two_towers_3_3" };
    for (int s = 0; s < 2; s++) {
        fprintf(fp, "    \"%s\": {\n      \"instances\": %u, \"plan_steps\": %u, \"graph_nodes\": %u, "
                    "\"evidence_words\": %u,\n",
                names[s], N_REPEAT, R.moves[s], R.nodes[s], R.evidence_words[s]);
        json_series(fp, "first_use", &R.first[s]);
        fprintf(fp, ",\n");
        json_series(fp, "reuse", &R.reuse[s]);
        fprintf(fp, ",\n      \"ratios\": {\"cognitive_ops\": %.1f, \"cognition_cpu\": %.1f, \"end_to_end\": %.2f},\n"
                    "      \"reuse_accepted\": %u, \"reuse_succeeded\": %u, \"outcome_equal_to_first_use\": %u, "
                    "\"evidence_equal_to_first_use\": %u, \"one_semantic_id\": %s, \"realizations_distinct\": %s\n    }%s\n",
                G.ops_ratio[s], G.cpu_ratio[s], G.e2e_ratio[s], R.reuse_accepted[s], R.reuse_success[s],
                R.outcome_equal[s], R.evidence_equal[s], R.semantic_ids_equal[s] ? "true" : "false",
                R.realizations_distinct[s] ? "true" : "false", s == 0 ? "," : "");
    }
    fprintf(fp, "  },\n  \"lifecycle\": {\"candidate_reference_runs\": %u, \"promotions_to_verified\": %u, "
                "\"templates_stored\": %llu, \"rederivations_deduplicated\": %llu},\n",
            R.candidate_reference_runs, R.promotions, (unsigned long long)g_cache.stored,
            (unsigned long long)g_cache.deduplicated);
    fprintf(fp, "  \"applicability\": {\"accepted_reuses\": %u, \"false_applicable\": %u, \"false_applicability_rate\": %.4f,\n"
                "    \"adversarial\": %u, \"refused\": %u, \"refused_on_expected_axis\": %u, \"refusals_recorded_in_world\": %u,\n"
                "    \"by_axis\": {",
            R.accepted_total, R.false_applicable,
            R.accepted_total ? (double)R.false_applicable / (double)R.accepted_total : 0.0, R.adv_cases,
            R.adv_refused, R.adv_axis_match, R.adv_decision_crumbs);
    int first = 1;
    for (uint32_t a = 1; a < PL_AX_COUNT; a++) {
        if (!R.adv_by_axis[a]) continue;
        fprintf(fp, "%s\n      \"%s\": {\"cases\": %u, \"refused\": %u, \"blind_replays\": %u, "
                    "\"blind_reached_goal\": %u, \"blind_violated\": %u}",
                first ? "" : ",", rx_plan_axis_name((PlAxis)a), R.adv_by_axis[a], R.adv_refused_by_axis[a],
                R.blind_by_axis[a], R.blind_ok_by_axis[a], R.blind_violation_by_axis[a]);
        first = 0;
    }
    fprintf(fp, "\n    }},\n");
    fprintf(fp, "  \"identity\": {\"canonical_round_trip\": %s, \"predicate_order_invariant\": %s, "
                "\"semantic_mutations\": %u, \"changed_id\": %u, \"nonsemantic_mutations\": %u, \"kept_id\": %u, "
                "\"rederived_same_id\": %s, \"rederived_then_reused\": %s},\n",
            R.id_roundtrip ? "true" : "false", R.id_order_invariant ? "true" : "false", R.id_mutations,
            R.id_mutations_changed, R.id_nonsemantic, R.id_nonsemantic_same, R.rederived_same_id ? "true" : "false",
            R.rederived_then_reused ? "true" : "false");
    fprintf(fp, "  \"adaptation\": {\"adapted\": %u, \"succeeded\": %u, \"cognitive_ops\": %llu, "
                "\"full_search_cognitive_ops\": %llu, \"cognition_cpu_ns\": %llu, \"full_search_cognition_cpu_ns\": %llu, "
                "\"steps\": %u, \"full_search_steps\": %u, \"later_goals_reused_adapted\": %u},\n",
            R.adapt_n, R.adapt_ok, (unsigned long long)R.adapt_ops_med, (unsigned long long)R.adapt_cold_ops_med,
            (unsigned long long)R.adapt_cpu_med, (unsigned long long)R.adapt_cold_cpu_med, R.adapt_steps,
            R.adapt_cold_steps, R.adapt_then_reuse);
    if (R.energy_ok)
        fprintf(fp, "  \"energy\": {\"source\": \"aien_spbm hwmon (on-device package counter, 1 mJ steps; "
                    "machine not quiet; idle baseline subtracted)\", \"idle_package_w\": %.3f, \"loop_ns\": %llu,\n"
                    "    \"first_use_loops\": %llu, \"first_use_package_mj\": %.5f, \"first_use_cpu_p_mj\": %.5f,\n"
                    "    \"reuse_loops\": %llu, \"reuse_package_mj\": %.7f, \"reuse_cpu_p_mj\": %.7f},\n",
                R.idle_w, (unsigned long long)R.loop_ns, (unsigned long long)R.cold_loops, R.cold_pkg_mj_per,
                R.cold_cpup_mj_per, (unsigned long long)R.reuse_loops, R.reuse_pkg_mj_per, R.reuse_cpup_mj_per);
    else
        fprintf(fp, "  \"energy\": {\"measured\": false, \"why\": \"%s\"},\n", R.energy_why);
    fprintf(fp, "  \"instructions_counter\": %s,\n", R.perf_ok ? "true" : "false");
    fprintf(fp, "  \"gates\": {\n    \"G1_cognitive_ops_20x\": \"%s\",\n    \"G2_cognition_cpu_10x\": \"%s\",\n"
                "    \"G3_outcome_and_evidence\": \"%s\",\n    \"G4_false_applicability_zero\": \"%s\",\n"
                "    \"G5_identity\": \"%s\",\n    \"G6_end_to_end_faster\": \"%s\",\n"
                "    \"OMEGA_PLAN_REUSE_PASS\": \"%s\",\n",
            g1 ? "PASS" : "FAIL", g2 ? "PASS" : "FAIL", g3 ? "PASS" : "FAIL", g4 ? "PASS" : "FAIL",
            g5 ? "PASS" : "FAIL", g6 ? "PASS" : "FAIL", G.pass ? "PASS" : "FAIL");
    fprintf(fp, "    \"not_claimed\": [\"a neural or learned planner\", \"more than one planning domain\", "
                "\"durable template storage (R9)\", \"graphics-processor execution\", "
                "\"cognition reading the World through capabilities (the cache reads a host projection; "
                "execution is capability-checked)\", \"templates above 8 slots or 64 graph nodes\"]\n  }\n}\n");
    fclose(fp);
    printf("receipt: %s\n", path);
    printf("OMEGA_PLAN_REUSE_PASS: %s (G1 %s, G2 %s, G3 %s, G4 %s, G5 %s, G6 %s)\n", G.pass ? "PASS" : "FAIL",
           g1 ? "PASS" : "FAIL", g2 ? "PASS" : "FAIL", g3 ? "PASS" : "FAIL", g4 ? "PASS" : "FAIL",
           g5 ? "PASS" : "FAIL", g6 ? "PASS" : "FAIL");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);
    place();
    perf_open();
    R.perf_ok = g_perf >= 0;
    memset(&g_skills, 0, sizeof g_skills);
    rx_plan_cache_init(&g_cache);
    g_a = calloc(1, sizeof *g_a);
    g_b = calloc(1, sizeof *g_b);
    if (!g_a || !g_b) return 2;
    t_repeated(SHAPE_REVERSE);
    t_repeated(SHAPE_MERGE);
    dump_cache("after repeated");
    t_identity();
    t_adversarial();
    dump_cache("after adversarial");
    t_rederive();
    dump_cache("after rederive");
    t_adapt();
    dump_cache("after adapt");
    if (!getenv("PLAN_REUSE_NO_ENERGY")) t_energy();
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    rx_plan_cache_free(&g_cache);
    return g_fail ? 1 : 0;
}
