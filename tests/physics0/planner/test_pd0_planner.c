/* PD-0 planner contract tests (spec section 8 behaviours 1 to 6, gate G3).
 * Uses only verifier formats and made-up relations; no generator is linked. */
#include <stdio.h>
#include <string.h>
#include "pd0_planner.h"

static int checks = 0, failed = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); } } while (0)

/* a two-variable, one-channel relation in D form: Ds0 = a*s1, Ds1 = b*s0 + c*s1 + d*u */
static void mk_rel(pd0_rel *r, int64_t a, int64_t b, int64_t c, int64_t d) {
    memset(r, 0, sizeof *r);
    r->n_vars = 2; r->n_latent = 0; r->n_channels = 1; r->n_equations = 2;
    r->eq[0].target = 0; r->eq[0].n_terms = 1; r->eq[0].coef[0] = a; r->eq[0].expo[0][1] = 1;
    r->eq[1].target = 1; r->eq[1].n_terms = 0;
    if (b) { int t = r->eq[1].n_terms++; r->eq[1].coef[t] = b; r->eq[1].expo[t][0] = 1; }
    if (c) { int t = r->eq[1].n_terms++; r->eq[1].coef[t] = c; r->eq[1].expo[t][1] = 1; }
    if (d) { int t = r->eq[1].n_terms++; r->eq[1].coef[t] = d; r->eq[1].expo[t][2 + 0] = 1;   /* channel 0 at index n_vars + 0 */ }
    r->description_bits = pd0_rel_bits(r);
}

static void mk_bounds(pd0p_bounds *b) {
    memset(b, 0, sizeof *b);
    b->n_obs = 2; b->n_channels = 1;
    b->chan_min[0] = -2000000; b->chan_max[0] = 2000000;
    for (int i = 0; i < 2; i++) { b->reset_min[i] = -2000000; b->reset_max[i] = 2000000; }
    b->episode_max_steps = 100;
}

static void mk_dom(pd0p_domain *d) {
    memset(d, 0, sizeof *d);
    for (int i = 0; i < 2; i++) { d->var_min[i] = -2000000; d->var_max[i] = 2000000; }
}

int main(void) {
    pd0p_bounds b; pd0p_domain dom; mk_bounds(&b); mk_dom(&dom);
    static pd0_rel truth, r110, r150, missing, same;
    mk_rel(&truth,   50000, -150000, -25000, 50000);   /* dt 0.05, k 3, c 0.5 */
    mk_rel(&r110,    50000, -165000, -25000, 50000);   /* k x 1.1 */
    mk_rel(&r150,    50000, -225000, -25000, 50000);   /* k x 1.5 */
    mk_rel(&missing, 50000, -150000,      0, 50000);   /* damping term missing */
    memcpy(&same, &truth, sizeof same);
    static pd0p_prior prior; memset(&prior, 0, sizeof prior);
    pd0p_hyps h = { 2, { &truth, &r110 }, 10000 };
    static pd0p_proposal p1, p2;
    static uint8_t buf1[8192], buf2[8192], plan1[PD0P_PLAN_SIZE], plan2[PD0P_PLAN_SIZE];

    /* behaviour 1: deterministic given inputs and seed */
    int s1 = pd0p_propose(&b, &dom, &h, 3000, &prior, 7, &p1);
    int s2 = pd0p_propose(&b, &dom, &h, 3000, &prior, 7, &p2);
    CHECK(s1 == PD0P_OK && s2 == PD0P_OK, "proposal OK");
    size_t n1 = pd0p_prereg_bytes(&p1, buf1, sizeof buf1), n2 = pd0p_prereg_bytes(&p2, buf2, sizeof buf2);
    CHECK(n1 > 0 && n1 == n2 && memcmp(buf1, buf2, n1) == 0, "two runs byte-identical PD0EXP1");
    pd0p_plan_write(&p1, plan1); pd0p_plan_write(&p2, plan2);
    CHECK(memcmp(plan1, plan2, PD0P_PLAN_SIZE) == 0, "two runs byte-identical PD0PLN1");
    CHECK(p1.n_candidates >= 256, "at least 256 candidates");
    CHECK(p1.exp.n_steps == 20, "20-step schedule");
    CHECK(p1.exp.n_hyp == 2, "expected trajectories for every hypothesis");

    /* a different seed gives a different (still valid) schedule */
    static pd0p_proposal p3;
    CHECK(pd0p_propose(&b, &dom, &h, 3000, &prior, 8, &p3) == PD0P_OK, "seed 8 OK");
    CHECK(memcmp(p3.exp.schedule_hash, p1.exp.schedule_hash, PD0_HASH) != 0, "seed changes the schedule");

    /* PD0EXP1 round trip and the rev 5 schedule hash (requested values only) */
    pd0_exp back; size_t used = 0;
    CHECK(pd0_exp_parse(buf1, n1, &back, &used) == 0 && used == n1, "PD0EXP1 parses");
    uint8_t hh[PD0_HASH];
    pd0_schedule_hash(back.n_obs, back.reset, back.n_steps, back.steps, hh);
    CHECK(memcmp(hh, p1.exp.schedule_hash, PD0_HASH) == 0, "schedule hash recomputes from requested values");
    CHECK(pd0p_validate(&b, &prior, &p1.exp) == PD0P_OK, "proposal validates");
    pd0p_proposal pp; CHECK(pd0p_plan_parse(plan1, PD0P_PLAN_SIZE, &pp) == 0 && pp.divergence_micro == p1.divergence_micro, "PD0PLN1 parses");
    plan2[8] = 2; CHECK(pd0p_plan_parse(plan2, PD0P_PLAN_SIZE, &pp) != 0, "PD0PLN1 unknown version refused");

    /* behaviour 2: out-of-range mutant is refused as a planner defect */
    pd0_exp mut; memcpy(&mut, &p1.exp, sizeof mut);
    mut.steps[3].value = b.chan_max[0] + 1;
    pd0_schedule_hash(mut.n_obs, mut.reset, mut.n_steps, mut.steps, mut.schedule_hash);
    CHECK(pd0p_validate(&b, &prior, &mut) == PD0P_ERR_BOUNDS, "out-of-range step refused (BOUNDS)");
    memcpy(&mut, &p1.exp, sizeof mut); mut.reset[1] = b.reset_min[1] - 1;
    pd0_schedule_hash(mut.n_obs, mut.reset, mut.n_steps, mut.steps, mut.schedule_hash);
    CHECK(pd0p_validate(&b, &prior, &mut) == PD0P_ERR_BOUNDS, "out-of-box reset refused (BOUNDS)");
    memcpy(&mut, &p1.exp, sizeof mut); mut.steps[0].value += 1;   /* stale hash */
    CHECK(pd0p_validate(&b, &prior, &mut) == PD0P_ERR_INPUT, "stale schedule hash refused (INPUT)");
    /* the planner itself never emits out-of-range values over many seeds */
    int in_bounds = 1; uint8_t flags[32];
    for (uint32_t s = 0; s < 32; s++) {
        static pd0p_proposal q;
        if (pd0p_propose(&b, &dom, &h, 3000, &prior, 100 + s, &q) != PD0P_OK || pd0p_validate(&b, &prior, &q.exp) != PD0P_OK) in_bounds = 0;
        flags[s] = q.outside_middle;
    }
    CHECK(in_bounds, "32 seeds: every proposal within bounds and valid");
    uint32_t frac = pd0p_outside_fraction_ppm(flags, 32);
    printf("behaviour 5 (provisional 20%% rule): %u ppm of proposals start or reach outside the middle 50%% of the visited range\n", frac);
    CHECK(frac >= 200000, "behaviour 5: at least 20% outside the middle (provisional value)");

    /* behaviour 3: never repeat a schedule hash */
    pd0p_prior_add(&prior, p1.exp.schedule_hash);
    CHECK(pd0p_validate(&b, &prior, &p1.exp) == PD0P_ERR_DUPLICATE, "duplicate schedule refused (DUPLICATE)");
    static pd0p_proposal p4;
    CHECK(pd0p_propose(&b, &dom, &h, 3000, &prior, 7, &p4) == PD0P_OK, "same seed with prior: still proposes");
    CHECK(memcmp(p4.exp.schedule_hash, p1.exp.schedule_hash, PD0_HASH) != 0, "same seed with prior: a different schedule");
    CHECK(p4.n_skipped_duplicate >= 1, "duplicate candidate was skipped, not emitted");

    /* behaviour 4: identical hypotheses have no discriminating experiment */
    pd0p_hyps hs = { 2, { &truth, &same }, 10000 };
    static pd0p_proposal p5;
    CHECK(pd0p_propose(&b, &dom, &hs, 3000, NULL, 7, &p5) == PD0P_NO_DISCRIMINATING_EXPERIMENT, "identical hypotheses: NO_DISCRIMINATING_EXPERIMENT");
    CHECK(p5.divergence_micro == 0, "D is zero for identical hypotheses");
    CHECK(pd0p_prereg_bytes(&p5, buf1, sizeof buf1) == 0, "no preregistration bytes without a discriminating experiment");

    /* D monotone: a larger perturbation diverges more; a missing term is also found */
    pd0p_hyps h150 = { 2, { &truth, &r150 }, 10000 };
    pd0p_hyps hmiss = { 2, { &truth, &missing }, 10000 };
    static pd0p_proposal p6, p7;
    CHECK(pd0p_propose(&b, &dom, &h150, 3000, NULL, 7, &p6) == PD0P_OK, "k x1.5 OK");
    CHECK(p6.divergence_micro > p1.divergence_micro, "D(k x1.5) > D(k x1.1)");
    CHECK(pd0p_propose(&b, &dom, &hmiss, 3000, NULL, 7, &p7) == PD0P_OK && p7.divergence_micro >= PD0P_MIN_D_MICRO, "missing-term rival is discriminable");
    /* three hypotheses at once */
    pd0p_hyps h3 = { 3, { &truth, &r110, &missing }, 10000 };
    static pd0p_proposal p8;
    CHECK(pd0p_propose(&b, &dom, &h3, 3000, NULL, 7, &p8) == PD0P_OK && p8.exp.n_hyp == 3, "three hypotheses: proposal carries three trajectories");
    CHECK(p8.divergence_micro >= p1.divergence_micro, "D over three hypotheses is at least the pair's D");

    /* budget and input errors */
    static pd0p_proposal p9;
    CHECK(pd0p_propose(&b, &dom, &h, 1, NULL, 7, &p9) == PD0P_ERR_BUDGET, "budget below 2 steps refused");
    CHECK(pd0p_propose(&b, &dom, &h, 10, NULL, 7, &p9) == PD0P_OK && p9.exp.n_steps == 10, "budget shortens the schedule");
    pd0p_hyps h1 = { 1, { &truth }, 10000 };
    CHECK(pd0p_propose(&b, &dom, &h1, 3000, NULL, 7, &p9) == PD0P_ERR_INPUT, "one hypothesis refused (a rival is required)");
    pd0p_hyps h0 = { 2, { &truth, &r110 }, 0 };
    CHECK(pd0p_propose(&b, &dom, &h0, 3000, NULL, 7, &p9) == PD0P_ERR_INPUT, "zero residual sd refused");

    /* passive baseline: deterministic, in bounds, distinct per index, no hypothesis consulted */
    pd0_exp pa, pb2, pc;
    CHECK(pd0p_passive(&b, 3000, 99, 0, &pa) == PD0P_OK && pd0p_passive(&b, 3000, 99, 0, &pb2) == PD0P_OK, "passive OK");
    CHECK(memcmp(&pa, &pb2, sizeof pa) == 0, "passive deterministic");
    CHECK(pd0p_passive(&b, 3000, 99, 1, &pc) == PD0P_OK && memcmp(pc.schedule_hash, pa.schedule_hash, PD0_HASH) != 0, "passive index 1 differs");
    CHECK(pd0p_validate(&b, NULL, &pa) == PD0P_OK, "passive schedule within bounds");
    CHECK(pa.n_hyp == 0, "passive carries no hypothesis trajectory");

    printf("test_pd0_planner: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
