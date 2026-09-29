#include "turing/ty_energy.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #expr); } } while (0)

static ty_interval window(const char *cond, uint64_t start, uint64_t energy) {
    ty_interval w = {0};
    strcpy(w.run_id, "fixture"); strcpy(w.config, "C0"); strcpy(w.cond, cond);
    w.round = 1; w.t0_ns = start; w.t1_ns = start + 1000000000;
    w.planned_ns = 1000000000; w.spbm_ok0 = w.spbm_ok1 = 1;
    w.n_samples = 10; w.max_gap_ns = 100000000;
    for (int ch = 0; ch < TY_NCH; ch++) { w.e0_uj[ch] = 1000000; w.e1_uj[ch] = 1000000 + energy; }
    w.npart = !strcmp(cond, "IDLE") ? 0 : strlen(cond);
    for (size_t i = 0; i < w.npart; i++) {
        ty_participant *p = &w.part[i];
        p->tag[0] = cond[i]; strcpy(p->rz, "R1_sdot"); strcpy(p->cpus, "0-9");
        p->go_ns = start; p->t0_ns = start + 1000000; p->t1_ns = start + 999000000;
        p->done_ns = p->t1_ns + 1; p->planned_ns = w.planned_ns; p->calls = 1;
        p->pinned = p->oracle_ok = p->exited_ok = p->pmu_ok = 1;
    }
    return w;
}

static ty_attribution attribution(void) {
    ty_attribution a = {0}; strcpy(a.method, "fixture.v0"); a.nshare = 3; a.tolerance_uj = 1;
    strcpy(a.share[0].who, "IDLE"); a.share[0].uj = 20; a.share[0].conf = TY_COUNTERFACTUAL;
    strcpy(a.share[1].who, "A"); a.share[1].uj = 30; a.share[1].conf = TY_COUNTERFACTUAL;
    strcpy(a.share[2].who, "B"); a.share[2].uj = 50; a.share[2].conf = TY_COUNTERFACTUAL;
    return a;
}

int main(void) {
    ty_interval w = window("AB", 0, 100), bad;
    int64_t energy = -1;
    CHECK(ty_interval_check(&w) == TYE_OK);
    CHECK(ty_interval_energy(&w, TY_CH_PKG, &energy) == TYE_OK && energy == 100);
    CHECK(ty_interval_check(NULL) == TYE_E_ARG);
    CHECK(ty_interval_energy(&w, -1, &energy) == TYE_E_ARG);
    CHECK(ty_interval_energy(&w, 0, NULL) == TYE_E_ARG);
    bad = w; bad.spbm_ok0 = 0; CHECK(ty_interval_check(&bad) == TYE_E_SENSOR);
    bad = w; bad.overflow = 1; CHECK(ty_interval_check(&bad) == TYE_E_WRAP);
    bad = w; bad.e1_uj[0] = 0; CHECK(ty_interval_check(&bad) == TYE_E_WRAP);
    bad = w; bad.e0_uj[0] = TY_WRAP_UJ - 1000; bad.e1_uj[0] = TY_WRAP_UJ - 100;
    CHECK(ty_interval_check(&bad) == TYE_E_HEADROOM);
    bad = w; bad.n_samples = 0; CHECK(ty_interval_check(&bad) == TYE_E_SAMPLING_GAP);
    bad = w; bad.n_samples = UINT64_MAX; CHECK(ty_interval_check(&bad) == TYE_E_SAMPLING_GAP);
    bad = w; bad.max_gap_ns = TY_MAX_GAP_NS + 1; CHECK(ty_interval_check(&bad) == TYE_E_SAMPLING_GAP);
    bad = w; bad.part[0].pinned = 0; CHECK(ty_interval_check(&bad) == TYE_E_PARTIAL_RUN);
    bad = w; bad.part[0].oracle_ok = 0; CHECK(ty_interval_check(&bad) == TYE_E_PARTIAL_RUN);
    bad = w; bad.part[0].multiplexed = 1; CHECK(ty_interval_check(&bad) == TYE_E_MULTIPLEXED);
    bad = w; bad.part[0].pmu_ok = 0; CHECK(ty_interval_check(&bad) == TYE_E_SENSOR);
    bad = w; bad.part[0].t1_ns = bad.part[0].t0_ns + 1; CHECK(ty_interval_check(&bad) == TYE_E_PARTIAL_RUN);
    bad = w; bad.part[0].done_ns = w.t1_ns + 1; CHECK(ty_interval_check(&bad) == TYE_E_INTERVAL_MISMATCH);
    bad = w; bad.planned_ns = UINT64_MAX; CHECK(ty_interval_check(&bad) == TYE_E_INTERVAL_MISMATCH);
    bad = w; bad.npart = TY_MAX_PART + 1; CHECK(ty_interval_check(&bad) == TYE_E_ARG);
    bad = w; memset(bad.cond, 'A', sizeof bad.cond); CHECK(ty_interval_check(&bad) == TYE_E_ARG);
    bad = w; strcpy(bad.cond, "AA"); CHECK(ty_interval_check(&bad) == TYE_E_CONDITION);
    bad = w; bad.part[1].tag[0] = 'A'; CHECK(ty_interval_check(&bad) == TYE_E_ARG);
    bad = w; strcpy(bad.part[0].rz, "a|b"); CHECK(ty_interval_check(&bad) == TYE_E_ARG);

    ty_interval large = window("IDLE", 0, 1000000000000ULL);
    large.planned_ns = 30000000000ULL; large.t1_ns = large.planned_ns; large.n_samples = 300;
    CHECK(ty_interval_energy(&large, 0, &energy) == TYE_OK && energy == 1000000000000LL);
    ty_interval idle = window("IDLE", 0, 1000), a = window("A", 2000000000, 2000);
    ty_interval b = window("B", 4000000000, 3000), ab = window("AB", 6000000000, 5000);
    CHECK(ty_interaction(&idle, &a, &b, &ab, 0, &energy) == TYE_OK && energy == 1000);
    CHECK(ty_interaction(NULL, &a, &b, &ab, 0, &energy) == TYE_E_MISSING_IDLE);
    bad = a; bad.round++; CHECK(ty_interaction(&idle, &bad, &b, &ab, 0, &energy) == TYE_E_CONDITION);
    const ty_interval *disjoint[] = {&idle, &a};
    CHECK(ty_intervals_disjoint(disjoint, 2, NULL, NULL) == TYE_OK);
    const ty_interval *overlap[] = {&a, &a};
    CHECK(ty_intervals_disjoint(overlap, 2, NULL, NULL) == TYE_E_OVERLAP);
    CHECK(ty_intervals_disjoint(NULL, 1, NULL, NULL) == TYE_E_ARG);

    ty_attribution at = attribution();
    CHECK(ty_attribution_check(&at, &w) == TYE_OK);
    CHECK(at.allocated_uj == 100 && at.remainder_uj == 0 && at.overall == TY_COUNTERFACTUAL);
    CHECK(ty_conf_forge(at.overall) == TY_FORGE_ENERGY_ESTIMATED);
    at = attribution(); at.share[0].uj = INT64_MIN;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_NEGATIVE_SHARE);
    at = attribution(); at.share[0].uj = INT64_MAX; at.share[1].uj = INT64_MAX;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_ARG);
    at = attribution(); at.tolerance_uj = UINT64_MAX;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_ARG && at.verdict == TYE_E_ARG);
    at = attribution(); at.share[0].uj = 100;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_OVER_ALLOCATED);
    at = attribution(); strcpy(at.share[2].who, "A");
    CHECK(ty_attribution_check(&at, &w) == TYE_E_DOUBLE_COUNT);
    at = attribution(); strcpy(at.share[2].who, "C");
    CHECK(ty_attribution_check(&at, &w) == TYE_E_UNKNOWN_PARTICIPANT);
    at = attribution(); at.nshare = 2;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_MISSING_PARTICIPANT);
    at = attribution(); at.share[0] = at.share[1]; at.share[1] = at.share[2]; at.nshare = 2;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_MISSING_IDLE);
    at = attribution(); at.share[0].conf = (ty_conf)-1;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_ARG);
    at = attribution(); CHECK(ty_attribution_check(&at, &w) == TYE_OK);
    at.nsrc = 1; at.src[0] = at.measured;
    CHECK(ty_attribution_check(&at, &w) == TYE_E_DOUBLE_COUNT);
    at = attribution(); at.all_unattributed = 1; at.nshare = 0;
    CHECK(ty_attribution_check(&at, &w) == TYE_OK && at.remainder_uj == 100);
    CHECK(!strcmp(ty_conf_name((ty_conf)-1), "INVALID"));
    CHECK(ty_conf_forge((ty_conf)-1) == TY_FORGE_ENERGY_NOT_MEASURED);
    CHECK(!strcmp(ty_energy_status_name((ty_energy_status)-1), "INVALID"));
    turing_digest d1, d2;
    CHECK(ty_interval_digest(&w, &d1) == 0);
    CHECK(ty_interval_digest(&w, &d2) == 0 && !memcmp(&d1, &d2, sizeof d1));
    bad = w; bad.part[0].cycles++;
    CHECK(ty_interval_digest(&bad, &d2) == 0 && memcmp(&d1, &d2, sizeof d1));
    bad = w; memset(bad.run_id, 'x', sizeof bad.run_id);
    CHECK(ty_interval_digest(&bad, &d2) != 0);
    CHECK(ty_attribution_digest(&at, &d2) == 0);
    printf("ty_energy: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
