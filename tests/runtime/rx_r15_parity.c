/*
 * rx_r15_parity.c -- R15 SEQ semantic parity gate (spec §16 C1 item 9).
 *
 * SEQ may serve as the R15 baseline only if it does the same semantic work.
 * Each pair builds the R13 body twice from the same initial state, once as
 * RES-1 and once as SEQ, and drives the same stimulus: placement A725,
 * production requests with the §4 seed schedule, placement X925, and one
 * goal with the same target. The side that runs first derives the target
 * from AIEN's confirmed cost; the other is given it. Pairs alternate which
 * side goes first.
 *
 * Gated, exactly equal between the two sides of every pair: goal, plan,
 * search epoch, the verified selected realization, the GPU experiment
 * evidence, AIEN's belief, generation identity, the in-force realization
 * and the authority outcome. Gated on each side: goal MET, crumbs verify,
 * no crumb overflow, no illegal transition, no wrong result, no use of a
 * realization not in force, and no lost trigger. Measured costs are printed
 * and not compared.
 *
 * R15_PARITY_PAIRS (default 5) sets the number of pairs.
 * Exit status 0 = SEQ_SEMANTIC_PARITY PASS.
 */
#include "rx_r15_rig.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U(x) ((unsigned long long)(x))

static int g_fail;

static void check_side(int pair, const char *side, const R15Outcome *o, const char *first) {
    const char *bad = NULL;
    if (!o->ok) bad = "goal not MET";
    else if (!o->crumbs_verified) bad = "crumbs do not verify";
    else if (o->crumb_overflow) bad = "crumb log overflowed";
    else if (o->illegal) bad = "illegal lifecycle transition";
    else if (o->wrong) bad = "wrong production result";
    else if (o->unpromoted_use) bad = "production ran a realization not in force";
    else if (o->lost_triggers) bad = "lost trigger";
    else if (o->selected_verdict != RX_OMEGA_PASSED) bad = "selected realization not verified";
    else if (o->gen_after != o->gen_before + 1 && o->lineage_after != o->lineage_before + 1)
        bad = "generation did not advance exactly once";
    else if (o->aegis_woken || o->root_woken) bad = "AEGIS or root woke during the episode";
    else if (o->promote_result != RX_GEN_OK) bad = "promotion refused";
    if (bad) {
        g_fail++;
        fprintf(stderr, "  FAIL pair %d %s: %s%s%s\n", pair, side, bad,
                first && first[0] ? " -- " : "", first ? first : "");
    }
}

#define SAME(name) do {                                                          \
        if (a->name != b->name) {                                                \
            g_fail++;                                                            \
            fprintf(stderr, "  FAIL pair %d: %s differs: RES-1 %llu, SEQ %llu\n",  \
                    pair, #name, U(a->name), U(b->name));                        \
        }                                                                        \
    } while (0)
#define SAME_ARR(name, n) do {                                                   \
        for (uint32_t j_ = 0; j_ < (n); j_++)                                    \
            if (a->name[j_] != b->name[j_]) {                                    \
                g_fail++;                                                        \
                fprintf(stderr, "  FAIL pair %d: %s[%u] differs: RES-1 %llu, SEQ %llu\n", \
                        pair, #name, j_, U(a->name[j_]), U(b->name[j_]));        \
            }                                                                    \
    } while (0)

static void compare(int pair, const R15Outcome *a, const R15Outcome *b) {
    SAME(target_ns);
    SAME(goal_seq); SAME(goal_regime); SAME(goal_status); SAME(goal_class);
    SAME(plan_action); SAME(plan_regime); SAME(plan_condition); SAME(plan_reason);
    SAME(plan_goal);
    SAME(search_epoch); SAME(selection_epoch); SAME_ARR(selection_id, 4);
    SAME(selection_regime); SAME(selected_verdict);
    SAME_ARR(evidence, 5);
    SAME_ARR(belief, 4);
    SAME(gen_before); SAME(gen_after); SAME(lineage_before); SAME(lineage_after);
    SAME(inforce_epoch); SAME_ARR(inforce_id, 4); SAME(inforce_regime);
    SAME(inforce_generation);
    SAME(promote_result); SAME(promotion_active);
    SAME(aegis_woken); SAME(root_woken);
    SAME(gpu_claims); SAME(seat_commits);
}

static int run_side(R15Config c, uint64_t target, R15Outcome *o, char *first, size_t n) {
    R15Rig *r = calloc(1, sizeof *r);
    if (!r) return -1;
    int rc = r15_start(r, c);
    if (rc != 0) snprintf(o->why, sizeof o->why, "setup failed at stage %d", r->stage);
    else rc = r15_episode(r, target, o);
    if (rc == 0 && o->lost_triggers) r15_lost_triggers(r, first, n);
    r15_stop(r);
    free(r);
    return rc;
}

static void print_side(const char *side, const R15Outcome *o) {
    printf("    %-5s goal status %llu, plan action %llu reason %llu, epoch %llu, "
           "selected %016llx (verdict %llu), evidence %llu/%llu trials, belief %llu, "
           "generation %llu -> %llu, in force %016llx; measured: incumbent %llu ns, "
           "selected %llu ps, reference %llu ps, served %llu, crumbs %llu\n",
           side, U(o->goal_status), U(o->plan_action), U(o->plan_reason), U(o->selection_epoch),
           U(o->selection_id[0]), U(o->selected_verdict), U(o->evidence[4]), U(o->evidence[3]),
           U(o->belief[2]), U(o->gen_before), U(o->gen_after), U(o->inforce_id[0]),
           U(o->incumbent_ns), U(o->selected_ps), U(o->reference_ps), U(o->served),
           U(o->crumbs));
}

/* The lost-trigger detector must see a trigger that did not run. With no
 * run slots, a production request (it triggers workload.matvec.serve) is
 * left unrun; the detector must count it. With slots back, the reaction runs
 * and the count returns to zero. */
static void detector_control(R15Config c) {
    R15Rig *r = calloc(1, sizeof *r);
    if (!r) { g_fail++; return; }
    char first[200] = "";
    if (r15_start(r, c) != 0) {
        g_fail++;
        fprintf(stderr, "  FAIL detector control %s: setup stage %d\n", r15_config_name(c),
                r->stage);
        r15_stop(r);
        free(r);
        return;
    }
    RxResourceBudget keep = r->w.budget, none = r->w.budget;
    none.slots = 0;
    rx_world_set_resources(&r->w, &none);
    RxMutation m[4] = {{r->omega.o.request, 0, 1}, {r->omega.o.request, 1, R15_M},
                       {r->omega.o.request, 2, R15_N},
                       {r->omega.o.request, 3, 0x2545F4914F6CDD1Dull}};
    int pub = rx_world_publish_external(&r->w, r->ext_request, m, 4) > 0;
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
    uint64_t starved = r15_lost_triggers(r, first, sizeof first);
    rx_world_set_resources(&r->w, &keep);
    int quiet = rx_world_wait_quiescent(&r->w, 10000) == RX_OK;
    uint64_t after = r15_lost_triggers(r, NULL, 0);
    printf("  detector control %s: starved %llu (%s), after slots restored %llu\n",
           r15_config_name(c), U(starved), first, U(after));
    if (!pub || starved == 0 || !quiet || after != 0) {
        g_fail++;
        fprintf(stderr, "  FAIL detector control %s\n", r15_config_name(c));
    }
    r15_stop(r);
    free(r);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    int pairs = getenv("R15_PARITY_PAIRS") ? atoi(getenv("R15_PARITY_PAIRS")) : 5;
    if (pairs < 1) pairs = 1;
#ifdef R15_SILICON
    printf("R15 SEQ parity: GB10 resident seat\n");
#else
    printf("R15 SEQ parity: processor stand-in seat (host)\n");
#endif
    /* Diagnostic only (not the gate): R15_PARITY_REPEAT=<RES4|RES1|SEQ>:<n>
     * runs n episodes of one configuration and reports each outcome. */
    const char *rep = getenv("R15_PARITY_REPEAT");
    if (rep) {
        R15Config c = strncmp(rep, "SEQ", 3) == 0 ? R15_SEQ
                    : strncmp(rep, "RES4", 4) == 0 ? R15_RES4 : R15_RES1;
        const char *colon = strchr(rep, ':');
        int n = colon ? atoi(colon + 1) : 10, bad = 0;
        for (int i = 0; i < n; i++) {
            R15Outcome o;
            char lost[200] = "";
            memset(&o, 0, sizeof o);
            int rc = run_side(c, 0, &o, lost, sizeof lost);
            if (rc != 0) bad++;
            printf("  %s episode %d: %s%s incumbent %llu ns target %llu ns expected after "
                   "%llu ns (selected %llu ps, reference %llu ps) served %llu crumbs %llu\n",
                   r15_config_name(c), i, rc ? "FAIL " : "ok", rc ? o.why : "",
                   U(o.incumbent_ns), U(o.target_ns), U(o.final_expected_ns),
                   U(o.selected_ps), U(o.reference_ps), U(o.served), U(o.crumbs));
        }
        printf("R15 repeat %s: %d of %d failed\n", r15_config_name(c), bad, n);
        return bad ? 1 : 0;
    }
    detector_control(R15_RES1);
    detector_control(R15_SEQ);
    for (int p = 0; p < pairs; p++) {
        R15Outcome res, seq;
        char lost_res[200] = "", lost_seq[200] = "";
        memset(&res, 0, sizeof res);
        memset(&seq, 0, sizeof seq);
        int res_first = (p % 2) == 0;
        int rc_a, rc_b;
        if (res_first) {
            rc_a = run_side(R15_RES1, 0, &res, lost_res, sizeof lost_res);
            rc_b = rc_a == 0 ? run_side(R15_SEQ, res.target_ns, &seq, lost_seq, sizeof lost_seq)
                             : -1;
        } else {
            rc_b = run_side(R15_SEQ, 0, &seq, lost_seq, sizeof lost_seq);
            rc_a = rc_b == 0 ? run_side(R15_RES1, seq.target_ns, &res, lost_res, sizeof lost_res)
                             : -1;
        }
        printf("  pair %d (%s first, target %llu ns)\n", p, res_first ? "RES-1" : "SEQ",
               U(res_first ? res.target_ns : seq.target_ns));
        if (rc_a != 0 || rc_b != 0) {
            g_fail++;
            fprintf(stderr, "  FAIL pair %d: episode did not complete: RES-1 [%s] SEQ [%s]\n", p,
                    rc_a ? res.why : "ok", rc_b ? seq.why : "ok");
            continue;
        }
        print_side("RES-1", &res);
        print_side("SEQ", &seq);
        check_side(p, "RES-1", &res, lost_res);
        check_side(p, "SEQ", &seq, lost_seq);
        compare(p, &res, &seq);
    }
    printf("R15 SEQ parity: %d pairs, %d failures\n", pairs, g_fail);
    printf("SEQ_SEMANTIC_PARITY=%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
