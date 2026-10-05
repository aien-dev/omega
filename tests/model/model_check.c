/*
 * model_check.c -- exhaustive check of the World lifecycle model (lane LG).
 *
 * For each profile, every reachable state is explored and every invariant is
 * checked after every step:
 *   I1 no budget leak        V_LEAK
 *   I2 cancel reaches children V_CANCEL_CHILD (and the target: V_CANCEL_SELF)
 *   I3 no double commit      V_DOUBLE
 *   I4 deadline overrun surfaced V_DEADLINE
 *
 * Expectations (anything else is a FAIL of this checker):
 *   SPEC      holds every invariant.
 *   AS_BUILT  holds I1 and I3; fails I2 and I4 (KNOWN_FAIL: rx_world has no
 *             cancel entry point and counts a deadline only at admission).
 *   each mutant fails the invariant it targets (KILLED).
 * Output: human lines plus "RESULT key=value" lines for the receipt.
 */
#include "world_explore.h"

#include <stdio.h>
#include <string.h>

static const char *bit_name[5] = { "I1_no_budget_leak", "I2_cancel_reaches_children",
                                   "I3_no_double_commit", "I4_deadline_overrun_surfaced",
                                   "I2b_cancel_reaches_target" };

static int bad;

static void show(const char *tag, const WxResult *r, int b) {
    printf("  %s %s counterexample (%d steps): ", tag, bit_name[b], r->trace_len[b]);
    wx_print_trace(r->trace[b], r->trace_len[b]);
    printf("\n");
}

static void run(const WmProfile *p, uint32_t must_hold, uint32_t must_fail, const char *role) {
    WxResult r;
    if (wx_explore(p, &r) != 0) { printf("RESULT model.%s=ERROR\n", p->name); bad++; return; }
    printf("[*] %s (%s): %llu states, %llu transitions%s\n", p->name, role,
           (unsigned long long)r.states, (unsigned long long)r.transitions,
           r.truncated ? " TRUNCATED (not exhaustive)" : "");
    printf("RESULT model.%s.states=%llu\n", p->name, (unsigned long long)r.states);
    printf("RESULT model.%s.transitions=%llu\n", p->name, (unsigned long long)r.transitions);
    printf("RESULT model.%s.exhaustive=%s\n", p->name, r.truncated ? "NO" : "YES");
    if (r.truncated) bad++;
    for (int b = 0; b < 5; b++) {
        uint32_t bit = 1u << b;
        int seen = (r.viol_seen & bit) != 0;
        const char *verdict;
        if (must_hold & bit) {
            verdict = seen ? "FAIL" : "PASS";
            if (seen) { bad++; show("UNEXPECTED", &r, b); }
        } else if (must_fail & bit) {
            verdict = seen ? (strcmp(role, "mutant") == 0 ? "KILLED" : "KNOWN_FAIL")
                           : (strcmp(role, "mutant") == 0 ? "SURVIVED" : "UNEXPECTED_PASS");
            if (!seen) bad++;
            else show(strcmp(role, "mutant") == 0 ? "killed by" : "FINDING", &r, b);
        } else {
            verdict = seen ? "violated" : "held";
        }
        printf("RESULT model.%s.%s=%s\n", p->name, bit_name[b], verdict);
    }
}

int main(void) {
    const uint32_t all = V_LEAK | V_CANCEL_CHILD | V_DOUBLE | V_DEADLINE | V_CANCEL_SELF;
    run(&wm_spec, all, 0, "spec");
    run(&wm_as_built, V_LEAK | V_DOUBLE, V_CANCEL_CHILD | V_DEADLINE | V_CANCEL_SELF, "as-built");
    run(&wm_mut_leak, 0, V_LEAK, "mutant");
    run(&wm_mut_noprop, V_CANCEL_SELF, V_CANCEL_CHILD, "mutant");
    run(&wm_mut_double, 0, V_DOUBLE, "mutant");
    run(&wm_mut_deadline, 0, V_DEADLINE, "mutant");
    printf("RESULT model.verdict=%s\n", bad ? "FAIL" : "PASS");
    printf("MODEL CHECK: %s (%d unexpected)\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
