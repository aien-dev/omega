/* ADAPTIVE-M4 host gate: bounded resource regulation closes a loop on the
 * World's own admission budget, inside a fixed envelope, without weakening
 * fairness or the operator emergency stop.
 *
 * A workload of 14 reactions (12 FOREGROUND, 2 BACKGROUND), each needing one
 * MiB, is woken by one outside publication ("a burst"). The World admits as
 * many as the budget allows; the rest wait in BLOCKED_RESOURCE and the
 * World gauges how many waited at once (peak_blocked, as the heartbeat test
 * reads it). That blocked-wait peak per burst is the declared objective:
 * fewer is better. The test publishes the measured peak as the condition,
 * lets the regulator decide, applies the policy, and measures the next burst.
 *
 * Phases: A nominal; B pressure -> EXPAND improves the objective; C calm ->
 * back to NOMINAL; D hysteresis: chatter and dead band bound the flips, the
 * World's own oscillation guard never trips; E envelope: a huge signal
 * cannot exceed the maximum, a forged policy is refused at apply, PROTECT
 * applies the minimum; F an operator stop halts the loop and resume restores
 * it; G fairness: the bound is unchanged in every applied budget and the
 * BACKGROUND reactions finish every burst. */
#include "runtime/aienos_cap.h"
#include "runtime/rx_regulate.h"
#include "runtime/rx_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SUBJ_EXTERNAL = 100, SUBJ_OPERATOR = 70, SUBJ_WORK = 41, ISSUER = 3 };
enum { N_FG = 12, N_BG = 2, N_WORK = N_FG + N_BG };
#define RES_TRIGGER   0x7741000ull
#define RES_WORK_BASE 0x7741100ull
#define OT_TRIGGER    0x7741u
#define OT_WORK       0x7742u
#define MIB           (1ull << 20)
#define BASE_SB       4u

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

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxRegulator reg;
    RxRegulateEnvelope env;
    RxCallerCred cr_reg, cr_work, cr_op;
    RxCapRef c_op, ext_condition, ext_trigger;
    RxObjRef trigger, work[N_WORK];
    RxCapRef work_trigger_read, work_own[N_WORK];
    uint64_t trigger_seq, cond_seq;
    uint64_t bg_done;
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

static int publish(Env *e, RxCapRef cap, RxObjRef o, const uint32_t *fields,
                   const uint64_t *vals, uint32_t n) {
    RxMutation m[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < n; i++) m[i] = (RxMutation){ o, fields[i], vals[i] };
    int64_t id = rx_world_publish_external(&e->w, cap, m, n);
    return id > 0 ? 0 : -1;
}

static void sleep_us(int us) {
    struct timespec ts = { 0, (long)us * 1000L };
    nanosleep(&ts, NULL);
}

/* ---- workload: one reaction per work object, woken by the trigger ---- */
typedef struct { Env *e; uint32_t i; } Work;
static Work g_work[N_WORK];

static int fn_work(RxCtx *c) {
    Work *wk = c->user;
    uint64_t seq = 0, done = 0;
    for (uint32_t i = 0; i < c->n_in; i++) {
        if (c->in[i].obj.id == wk->e->trigger.id) seq = c->in[i].field[0];
        else if (c->in[i].obj.id == wk->e->work[wk->i].id) done = c->in[i].field[1];
    }
    c->out[c->n_out++] = (RxMutation){ wk->e->work[wk->i], 0, seq };
    c->out[c->n_out++] = (RxMutation){ wk->e->work[wk->i], 1, done + 1 };
    return 0;
}

static int register_work(Env *e) {
    for (uint32_t i = 0; i < N_WORK; i++) {
        g_work[i] = (Work){ e, i };
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = i < N_FG ? "work.fg" : "work.bg";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = SUBJ_WORK;
        d.caller = e->cr_work;
        d.priority = i < N_FG ? RX_PRIO_FOREGROUND : RX_PRIO_BACKGROUND;
        d.need.memory_bytes = MIB;
        d.fn = fn_work;
        d.user = &g_work[i];
        d.n_triggers = 1;
        d.triggers[0] = (RxDep){ e->trigger, RX_FIELD(0) };
        d.n_reads = 2;
        d.reads[0] = (RxDep){ e->trigger, RX_FIELD(0) };
        d.reads[1] = (RxDep){ e->work[i], RX_ALL_FIELDS };
        d.n_writes = 1;
        d.writes[0] = (RxDep){ e->work[i], RX_ALL_FIELDS };
        d.n_caps = 2;
        d.caps[0] = (RxCapNeed){ e->work_trigger_read, RES_TRIGGER, RX_RIGHT_READ };
        d.caps[1] = (RxCapNeed){ e->work_own[i], RES_WORK_BASE + i, RX_RIGHT_READ | RX_RIGHT_WRITE };
        uint32_t rid;
        if (rx_world_add_reaction(&e->w, &d, &rid) != RX_OK) return -1;
    }
    return 0;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof(*e));
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 4, 1u << 20) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    RxStabilityBudget st = { 0, 0, 3, 0, 0 };    /* the World's own A-B-A guard */
    rx_world_set_stability(&e->w, &st);

    uint64_t z[RX_MAX_FIELDS] = { 0 };
    if (rx_world_create(&e->w, OT_TRIGGER, RX_PERSIST_RESIDENT, RES_TRIGGER, z, &e->trigger) != RX_OK)
        return -1;
    for (uint32_t i = 0; i < N_WORK; i++)
        if (rx_world_create(&e->w, OT_WORK, RX_PERSIST_RESIDENT, RES_WORK_BASE + i, z,
                            &e->work[i]) != RX_OK) return -1;

    RxRegulateEnvelope *v = &e->env;
    memset(v, 0, sizeof(*v));
    v->slots_min = 1;  v->slots_max = 8;
    v->memory_min = 2 * MIB;  v->memory_max = 16 * MIB;
    v->energy_min = 1;  v->energy_max = UINT64_MAX;
    v->base.slots = 2;  v->base.memory_bytes = 4 * MIB;  v->base.energy_budget = UINT64_MAX;
    v->base.starvation_bound = BASE_SB;
    v->base.offered_locality = UINT32_MAX;  v->base.offered_accel = UINT32_MAX;
    v->base.compute_mask = UINT32_MAX;
    v->contention_hi = 6;  v->contention_lo = 2;
    v->pressure_hi = 90;   v->pressure_lo = 50;
    v->min_dwell = 3;
    if (rx_regulate_create_objects(&e->reg, &e->w, v) != RX_OK) return -1;

    if (rx_world_enroll_caller(&e->w, SUBJ_OPERATOR, &e->cr_op) != RX_CALLER_OK ||
        rx_world_enroll_caller(&e->w, RX_REG_SUBJ, &e->cr_reg) != RX_CALLER_OK ||
        rx_world_enroll_caller(&e->w, SUBJ_WORK, &e->cr_work) != RX_CALLER_OK) return -1;
    e->c_op = mint(e, SUBJ_OPERATOR, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT);
    e->ext_condition = mint(e, SUBJ_EXTERNAL, RX_REG_RES_BASE + RX_REG_RES_CONDITION, RX_RIGHT_WRITE);
    e->ext_trigger = mint(e, SUBJ_EXTERNAL, RES_TRIGGER, RX_RIGHT_WRITE);
    e->work_trigger_read = mint(e, SUBJ_WORK, RES_TRIGGER, RX_RIGHT_READ);
    for (uint32_t i = 0; i < N_WORK; i++)
        e->work_own[i] = mint(e, SUBJ_WORK, RES_WORK_BASE + i, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxRegulateCaps rc = {
        mint(e, RX_REG_SUBJ, RX_REG_RES_BASE + RX_REG_RES_CONDITION, RX_RIGHT_READ),
        mint(e, RX_REG_SUBJ, RX_REG_RES_BASE + RX_REG_RES_POLICY, RX_RIGHT_READ | RX_RIGHT_WRITE),
    };
    if (rx_regulate_register(&e->reg, &rc, &e->cr_reg) != RX_OK) return -1;
    if (register_work(e) != 0) return -1;
    return rx_world_bind_callers(&e->w) == RX_OK ? 0 : -1;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 20000);
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

/* One burst: wake the whole workload, wait for every reaction to finish,
 * return how many waited for a slot at once. Negative when the publication
 * was refused. The gauge is the World's own; the rig resets it per burst. */
static int64_t burst(Env *e) {
    e->w.peak_blocked = 0;
    uint64_t seq = ++e->trigger_seq;
    uint32_t f = 0;
    if (publish(e, e->ext_trigger, e->trigger, &f, &seq, 1) != 0) { e->trigger_seq--; return -1; }
    for (int spin = 0; spin < 5000; spin++) {
        uint32_t done = 0;
        for (uint32_t i = 0; i < N_WORK; i++) done += fld(e, e->work[i], 0) == seq;
        if (done == N_WORK) break;
        sleep_us(1000);
    }
    rx_world_wait_quiescent(&e->w, 20000);
    uint32_t done = 0, bg = 0;
    for (uint32_t i = 0; i < N_WORK; i++) {
        done += fld(e, e->work[i], 0) == seq;
        if (i >= N_FG) bg += fld(e, e->work[i], 0) == seq;
    }
    CHECK(done == N_WORK, "burst %llu: %u of %u reactions finished",
          (unsigned long long)seq, done, N_WORK);
    e->bg_done += bg;
    return (int64_t)e->w.peak_blocked;
}

/* Publish one condition sample and let the regulator settle on it. */
static int sample(Env *e, uint64_t contention, uint64_t pressure) {
    uint64_t seq = e->cond_seq + 1;
    uint32_t f[4] = { 0, 1, 3, 5 };
    uint64_t v[4] = { seq, contention, pressure, e->trigger_seq };
    if (publish(e, e->ext_condition, e->reg.condition, f, v, 4) != 0) return -1;
    e->cond_seq = seq;
    rx_world_wait_quiescent(&e->w, 20000);
    CHECK(fld(e, e->reg.policy, 5) == seq, "regulator did not decide on sample %llu",
          (unsigned long long)seq);
    return 0;
}

static uint64_t regime(Env *e) { return fld(e, e->reg.policy, 4); }
static uint64_t flips(Env *e)  { return fld(e, e->reg.policy, 7); }

/* Every applied budget keeps what the regulator does not own. */
static void check_untouched(Env *e, const char *when) {
    const RxResourceBudget *a = &e->reg.applied, *b = &e->env.base;
    CHECK(a->starvation_bound == BASE_SB && a->offered_locality == b->offered_locality &&
          a->offered_accel == b->offered_accel && a->compute_mask == b->compute_mask &&
          a->logical_tick == b->logical_tick, "%s: a field outside the policy changed", when);
    CHECK(e->w.budget.starvation_bound == BASE_SB, "%s: world fairness bound is %u",
          when, e->w.budget.starvation_bound);
}

static void row(const char *phase, Env *e, int64_t deferred) {
    printf("M4 %-10s regime %llu slots %u memory %llu MiB blocked-wait/burst %lld flips %llu refused %llu\n",
           phase, (unsigned long long)regime(e), e->w.budget.slots,
           (unsigned long long)(e->w.budget.memory_bytes / MIB), (long long)deferred,
           (unsigned long long)flips(e), (unsigned long long)e->reg.refused);
}

/* ---- pure decision checks, no world ---- */
static void decision_checks(const RxRegulateEnvelope *v) {
    CHECK(rx_regulate_decide(v, RX_REG_NOMINAL, 0, v->contention_hi, 0) == RX_REG_EXPAND,
          "nominal must expand at contention_hi");
    CHECK(rx_regulate_decide(v, RX_REG_NOMINAL, 0, v->contention_hi - 1, 0) == RX_REG_NOMINAL,
          "nominal must hold below contention_hi");
    CHECK(rx_regulate_decide(v, RX_REG_EXPAND, v->min_dwell, v->contention_lo + 1, 0) == RX_REG_EXPAND,
          "expand must hold above contention_lo (dead band)");
    CHECK(rx_regulate_decide(v, RX_REG_EXPAND, v->min_dwell - 1, 0, 0) == RX_REG_EXPAND,
          "expand must hold before min_dwell");
    CHECK(rx_regulate_decide(v, RX_REG_EXPAND, v->min_dwell, v->contention_lo, 0) == RX_REG_NOMINAL,
          "expand must return to nominal once calm and settled");
    CHECK(rx_regulate_decide(v, RX_REG_EXPAND, 0, UINT64_MAX, v->pressure_hi) == RX_REG_PROTECT,
          "protect wins over expand");
    CHECK(rx_regulate_decide(v, RX_REG_PROTECT, v->min_dwell, 0, v->pressure_lo + 1) == RX_REG_PROTECT,
          "protect must hold above pressure_lo");
    CHECK(rx_regulate_decide(v, RX_REG_PROTECT, v->min_dwell, v->contention_hi, v->pressure_lo) == RX_REG_EXPAND,
          "protect leaves to expand when contention is high");
    RxRegulateEnvelope bad = *v;
    bad.base.slots = v->slots_max + 1;
    CHECK(rx_regulate_envelope_ok(&bad) == RX_ERR_ARG, "base outside the bounds must be refused");
    bad = *v; bad.contention_lo = bad.contention_hi;
    CHECK(rx_regulate_envelope_ok(&bad) == RX_ERR_ARG, "empty dead band must be refused");
    bad = *v; bad.min_dwell = 0;
    CHECK(rx_regulate_envelope_ok(&bad) == RX_ERR_ARG, "zero dwell must be refused");
    RxResourceBudget b;
    rx_regulate_policy_for(v, RX_REG_EXPAND, &b);
    CHECK(b.slots == v->slots_max && b.memory_bytes == v->memory_max && b.starvation_bound == BASE_SB,
          "expand policy must be the envelope maximum with the base fairness bound");
    rx_regulate_policy_for(v, RX_REG_PROTECT, &b);
    CHECK(b.slots == v->slots_min && b.memory_bytes == v->memory_min, "protect policy must be the minimum");
}

int main(void) {
    Env e;
    if (env_start(&e) != 0) { fprintf(stderr, "env_start failed\n"); return 2; }
    decision_checks(&e.env);

    /* A: nominal. The first apply installs the base budget. */
    CHECK(rx_regulate_apply(&e.reg) == RX_OK, "first apply must install the base budget");
    CHECK(rx_regulate_apply(&e.reg) == 1, "second apply must find nothing new");
    CHECK(e.w.budget.slots == e.env.base.slots, "base slots not in force: %u", e.w.budget.slots);
    check_untouched(&e, "A");
    int64_t d_nom = burst(&e);
    int64_t d_nom2 = burst(&e);
    CHECK(d_nom > 0 && d_nom2 > 0, "nominal bursts must make work wait (%lld, %lld)", (long long)d_nom, (long long)d_nom2);
    CHECK(regime(&e) == RX_REG_NOMINAL && flips(&e) == 0, "A: regime %llu flips %llu",
          (unsigned long long)regime(&e), (unsigned long long)flips(&e));
    row("A nominal", &e, d_nom2);

    /* B: pressure. Measured contention goes in, a bounded policy comes out,
     * and the next burst is cheaper. */
    CHECK(sample(&e, (uint64_t)d_nom2, 10) == 0, "B sample refused");
    CHECK(regime(&e) == RX_REG_EXPAND && flips(&e) == 1, "B: regime %llu flips %llu",
          (unsigned long long)regime(&e), (unsigned long long)flips(&e));
    CHECK(rx_regulate_apply(&e.reg) == RX_OK, "B: apply must install the expand policy");
    CHECK(e.w.budget.slots == e.env.slots_max && e.w.budget.memory_bytes == e.env.memory_max,
          "B: expand budget not in force (slots %u)", e.w.budget.slots);
    check_untouched(&e, "B");
    int64_t d_exp = burst(&e);
    CHECK(d_exp >= 0 && d_exp < d_nom2, "B: expand must improve the objective: %lld vs nominal %lld",
          (long long)d_exp, (long long)d_nom2);
    row("B expand", &e, d_exp);

    /* C: calm. Below the dead band for min_dwell samples returns to nominal. */
    for (uint64_t i = 0; i < e.env.min_dwell; i++) {
        CHECK(sample(&e, (uint64_t)d_exp < e.env.contention_lo ? (uint64_t)d_exp : 0, 10) == 0, "C sample refused");
        CHECK(regime(&e) == RX_REG_EXPAND, "C: left expand before min_dwell at sample %llu", (unsigned long long)i);
    }
    CHECK(sample(&e, 0, 10) == 0, "C sample refused");
    CHECK(regime(&e) == RX_REG_NOMINAL && flips(&e) == 2, "C: regime %llu flips %llu",
          (unsigned long long)regime(&e), (unsigned long long)flips(&e));
    CHECK(rx_regulate_apply(&e.reg) == RX_OK && e.w.budget.slots == e.env.base.slots,
          "C: base budget not restored (slots %u)", e.w.budget.slots);
    check_untouched(&e, "C");
    int64_t d_rec = burst(&e);
    CHECK(d_rec > d_exp, "C: recovery must make work wait again: %lld vs %lld",
          (long long)d_rec, (long long)d_exp);
    row("C recover", &e, d_rec);

    /* D: hysteresis. Chatter across both thresholds flips at most once per
     * min_dwell + 1 samples; chatter inside the dead band never flips. The
     * World's own oscillation guard (limit 3) must not trip either. */
    uint64_t f0 = flips(&e);
    const int N_CHATTER = 24;
    for (int i = 0; i < N_CHATTER; i++)
        CHECK(sample(&e, i % 2 ? 0 : e.env.contention_hi, 10) == 0, "D sample refused");
    uint64_t chatter_flips = flips(&e) - f0;
    uint64_t bound = (uint64_t)N_CHATTER / (e.env.min_dwell + 1) + 2;
    CHECK(chatter_flips <= bound, "D: chatter caused %llu flips, bound %llu",
          (unsigned long long)chatter_flips, (unsigned long long)bound);
    CHECK(chatter_flips >= 2, "D: chatter must still be followed (flips %llu)", (unsigned long long)chatter_flips);
    /* settle to nominal, then drive the dead band */
    for (uint64_t i = 0; i <= e.env.min_dwell; i++) CHECK(sample(&e, 0, 10) == 0, "D settle refused");
    CHECK(regime(&e) == RX_REG_NOMINAL, "D: not nominal after settling");
    f0 = flips(&e);
    for (int i = 0; i < 10; i++)
        CHECK(sample(&e, i % 2 ? e.env.contention_lo + 1 : e.env.contention_hi - 1, 10) == 0, "D band refused");
    CHECK(flips(&e) == f0, "D: dead-band chatter flipped the policy %llu times",
          (unsigned long long)(flips(&e) - f0));
    CHECK(e.w.stats.oscillation_trips == 0 && e.w.stats.quarantines == 0 &&
          !e.w.reactions[e.reg.rid].quarantined,
          "D: World guard tripped (osc %llu quarantines %llu)",
          (unsigned long long)e.w.stats.oscillation_trips, (unsigned long long)e.w.stats.quarantines);
    CHECK(rx_regulate_apply(&e.reg) == RX_OK && e.w.budget.slots == e.env.base.slots,
          "D: apply after chatter must land on the base budget");
    row("D chatter", &e, -1);

    /* E: envelope. A huge signal gives exactly the maximum; a forged policy
     * (the test, as issuer, mints a write it should never have) is refused
     * at apply and the budget in force stays; PROTECT gives the minimum. */
    CHECK(sample(&e, UINT64_MAX, 10) == 0, "E sample refused");
    CHECK(regime(&e) == RX_REG_EXPAND && fld(&e, e.reg.policy, 1) == e.env.slots_max &&
          fld(&e, e.reg.policy, 2) == e.env.memory_max,
          "E: policy escaped the maximum (slots %llu)", (unsigned long long)fld(&e, e.reg.policy, 1));
    CHECK(rx_regulate_apply(&e.reg) == RX_OK && e.w.budget.slots == e.env.slots_max, "E: expand apply");
    CHECK(publish(&e, e.ext_condition, e.reg.policy, (uint32_t[]){ 1 }, (uint64_t[]){ 1000 }, 1) != 0,
          "E: the condition writer must not be able to write the policy");
    RxCapRef forged = mint(&e, SUBJ_EXTERNAL, RX_REG_RES_BASE + RX_REG_RES_POLICY, RX_RIGHT_WRITE);
    uint64_t ver = fld(&e, e.reg.policy, 0);
    uint32_t ff[2] = { 0, 1 };
    uint64_t fv[2] = { ver + 100, (uint64_t)e.env.slots_max * 4 };
    CHECK(publish(&e, forged, e.reg.policy, ff, fv, 2) == 0, "E: forged write did not land (test rig)");
    rx_world_wait_quiescent(&e.w, 20000);
    uint64_t refused0 = e.reg.refused;
    CHECK(rx_regulate_apply(&e.reg) == RX_ERR_BOUNDS && e.reg.refused == refused0 + 1,
          "E: forged policy must be refused at apply");
    CHECK(e.w.budget.slots == e.env.slots_max, "E: forged policy changed the budget (slots %u)", e.w.budget.slots);
    check_untouched(&e, "E");
    CHECK(sample(&e, 0, e.env.pressure_hi) == 0, "E protect sample refused");
    CHECK(regime(&e) == RX_REG_PROTECT, "E: pressure did not protect");
    CHECK(rx_regulate_apply(&e.reg) == RX_OK && e.w.budget.slots == e.env.slots_min &&
          e.w.budget.memory_bytes == e.env.memory_min, "E: protect budget not in force (slots %u)", e.w.budget.slots);
    check_untouched(&e, "E protect");
    int64_t d_prot = burst(&e);
    CHECK(d_prot >= d_nom2, "E: protect must not wait less than nominal: %lld vs %lld",
          (long long)d_prot, (long long)d_nom2);
    row("E protect", &e, d_prot);

    /* F: operator stop in the middle of regulation. The World refuses the
     * condition and the burst, apply refuses, nothing moves; resume restores. */
    RxHaltStatus h;
    CHECK(rx_world_emergency_stop(&e.w, SUBJ_OPERATOR, &e.cr_op, e.c_op, 4, &h) == RX_OK && h.halted,
          "F: operator stop refused");
    uint32_t slots_at_stop = e.w.budget.slots;
    CHECK(sample(&e, UINT64_MAX, 10) != 0, "F: condition publication must be refused while stopped");
    CHECK(burst(&e) < 0, "F: burst must be refused while stopped");
    CHECK(rx_regulate_apply(&e.reg) == RX_ERR_HALTED, "F: apply must refuse while stopped");
    CHECK(e.w.budget.slots == slots_at_stop, "F: budget moved during the stop");
    CHECK(rx_world_emergency_resume(&e.w, SUBJ_OPERATOR, &e.cr_op, e.c_op, &h) == RX_OK && !h.halted,
          "F: resume refused");
    for (uint64_t i = 0; i <= e.env.min_dwell; i++) CHECK(sample(&e, 0, 10) == 0, "F sample refused");
    CHECK(regime(&e) == RX_REG_NOMINAL, "F: not nominal after resume");
    CHECK(rx_regulate_apply(&e.reg) == RX_OK && e.w.budget.slots == e.env.base.slots, "F: apply after resume");
    int64_t d_after = burst(&e);
    CHECK(d_after > 0, "F: burst after resume");
    row("F resumed", &e, d_after);

    /* G: fairness. The BACKGROUND reactions finished every burst that ran. */
    uint64_t bursts = e.trigger_seq;
    CHECK(e.bg_done == bursts * N_BG, "G: background reactions finished %llu of %llu",
          (unsigned long long)e.bg_done, (unsigned long long)(bursts * N_BG));
    check_untouched(&e, "G");
    CHECK(e.w.stats.quarantines == 0 && e.w.stats.oscillation_trips == 0, "G: World guard tripped");
    row("G fairness", &e, -1);

    env_stop(&e);
    printf("RX_REGULATE %s: pass %d fail %d\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
