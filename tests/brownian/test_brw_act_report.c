/* Regression tests for the BRW-ACT report statistics (tests/brownian/brw_act_report.h).
 * Both defects were listed in docs/turing/BRW_ACT_RESULTS.md section 5 as known reporting issues. */
#include "brw_act_report.h"

#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    double k[200], mean, lo, hi;

    /* Readings in a world share that world's fit, so coverage clusters by world. 100 worlds cover all 60
     * readings and 100 cover none: pooled mean 0.5, but only 200 independent units. The interval must be about
     * +-1.96*0.5/sqrt(200) = +-0.069 wide, not the +-0.009 of 12000 independent readings. */
    for (int i = 0; i < 200; i++) k[i] = i < 100 ? 60 : 0;
    brwr_coverage(k, 200, 60, &mean, &lo, &hi);
    CHECK(fabs(mean - 0.5) < 1e-12);
    CHECK(hi - lo > 0.12 && hi - lo < 0.16);

    /* No spread between worlds: every world covers 54 of 60. Interval collapses onto 0.9. */
    for (int i = 0; i < 200; i++) k[i] = 54;
    brwr_coverage(k, 200, 60, &mean, &lo, &hi);
    CHECK(fabs(mean - 0.9) < 1e-12 && fabs(lo - 0.9) < 1e-12 && fabs(hi - 0.9) < 1e-12);

    /* Bounds stay inside [0, 1]. */
    for (int i = 0; i < 200; i++) k[i] = i < 2 ? 0 : 60;
    brwr_coverage(k, 200, 60, &mean, &lo, &hi);
    CHECK(lo >= 0.0 && hi <= 1.0 && lo <= mean && mean <= hi);

    /* P4: noise control passes (0 claims), DEV1 nested-null control fails (10 of 100). The noise line must say
     * PASS; only the combined verdict fails. */
    int nok, dok, all = brwr_p4(0, 10, 100, 1, &nok, &dok);
    CHECK(nok == 1 && dok == 0 && all == 0);
    all = brwr_p4(6, 0, 100, 1, &nok, &dok);
    CHECK(nok == 0 && dok == 1 && all == 0);
    all = brwr_p4(5, 10, 100, 0, &nok, &dok);                  /* DEV0 has no nested-null control */
    CHECK(nok == 1 && dok == 1 && all == 1);

    printf("test_brw_act_report: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
