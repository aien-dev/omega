/*
 * rx_cognitive_routing.c -- OMEGA_COGNITIVE_ROUTING qualification.
 *
 * 1. With no evidence the router runs each operation's reference realization.
 * 2. Labelled trials and metered energy go into a ledger. The ledger becomes a
 *    candidate generation. The routing does not change until the candidate is
 *    promoted with the promotion right on the native AIENOS authority; a
 *    promotion by the proposer, or without the right, is refused.
 * 3. Gate: the same held-out workload runs routed and on the always-most-
 *    expensive realization. Both must meet the same quality requirement;
 *    routed must use less processor time and less metered energy.
 * 4. Learning: the neural module is damaged. Routed quality drops under the
 *    active generation. A new ledger shows it; nothing changes until that
 *    candidate is promoted; after promotion quality is back.
 * 5. Hard constraints, bounded escalation, torn and foreign models.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_cog_engines.h"
#include "runtime/rx_route.h"

#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int g_checks, g_fail;
#define CHECK(c, ...)                                  \
    do {                                               \
        g_checks++;                                    \
        if (!(c)) {                                    \
            g_fail++;                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

#define SUBJ_PROPOSER  8u
#define SUBJ_AUTHORITY 4u

#define REC_CALIB   RX_COG_MAX_SAMPLE
#define PLAN_CALIB  RX_COG_MAX_SAMPLE
#define REC_EVAL    20000u
#define PLAN_EVAL   2000u
#define PASSES      9
#define SEED_TRAIN  0x5eed0001ull

/* ---- energy meter (aien_spbm hwmon, microjoules) ---- */

static char g_meter[512];

typedef struct {
    uint64_t pkg, cpu_e, cpu_p;
} Energy;

static void meter_find(void) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[600], name[64] = {0};
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(name, sizeof name, fp) && strncmp(name, "aien_spbm", 9) == 0)
            snprintf(g_meter, sizeof g_meter, "/sys/class/hwmon/%s", e->d_name);
        fclose(fp);
    }
    closedir(d);
}

static uint64_t meter_channel(const char *label) {
    for (int i = 1; i <= 8; i++) {
        char path[640], lab[64] = {0};
        snprintf(path, sizeof path, "%s/energy%d_label", g_meter, i);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        int ok = fgets(lab, sizeof lab, fp) != NULL;
        fclose(fp);
        lab[strcspn(lab, "\n")] = 0;
        if (!ok || strcmp(lab, label) != 0) continue;
        snprintf(path, sizeof path, "%s/energy%d_input", g_meter, i);
        fp = fopen(path, "r");
        if (!fp) return 0;
        unsigned long long v = 0;
        if (fscanf(fp, "%llu", &v) != 1) v = 0;
        fclose(fp);
        return v;
    }
    return 0;
}

static Energy meter_read(void) {
    Energy e = {0, 0, 0};
    if (!g_meter[0]) return e;
    e.pkg = meter_channel("pkg");
    e.cpu_e = meter_channel("cpu_e");
    e.cpu_p = meter_channel("cpu_p");
    return e;
}

static uint64_t cpu_uj(Energy a, Energy b) { return (b.cpu_p - a.cpu_p) + (b.cpu_e - a.cpu_e); }

static uint64_t now_ns(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- placement ---- */

static char g_cpus[64] = "unpinned";

static void place(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = n - 1; c >= 0; c--) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", c);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        unsigned long long midr = 0;
        int ok = fscanf(fp, "%llx", &midr) == 1;
        fclose(fp);
        if (ok && ((midr >> 4) & 0xfffu) == 0xd85u) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(c, &set);
            if (sched_setaffinity(0, sizeof set, &set) == 0) {
                snprintf(g_cpus, sizeof g_cpus, "cpu%ld (Cortex-X925)", c);
                printf("[*] placed on %s\n", g_cpus);
            }
            return;
        }
    }
}

/* ---- authority ---- */

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
} Auth;

static int native_auth(void *ctx, uint32_t cap_id, uint32_t cap_generation, uint32_t subject,
                       uint64_t resource, uint32_t rights) {
    Auth *a = ctx;
    AienosCapRef ref = {cap_id, cap_generation};
    AienosCapEntry entry;
    return aienos_cap_validate(a->view, ref, subject, resource, rights, &entry);
}

static int mint(Auth *a, uint32_t subject, uint32_t rights, AienosCapRef *out) {
    AienosCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = 3;
    m.subject = subject;
    m.resource = RX_GEN_RES_PROMOTION;
    m.rights = rights;
    m.parent.cap_id = UINT32_MAX;
    if (aienos_cap_office(a->admin, &m.authority) != 0) return -1;
    return aienos_cap_mint(a->admin, &m, out);
}

static int promote(RxGenStore *store, Auth *a, uint64_t cand, uint32_t subject, AienosCapRef cap) {
    RxPromotionRequest q;
    memset(&q, 0, sizeof q);
    q.candidate_id = cand;
    q.subject = subject;
    q.cap_id = cap.cap_id;
    q.cap_generation = cap.generation;
    q.resource = RX_GEN_RES_PROMOTION;
    q.rights = RX_GEN_RIGHT_PROMOTE;
    return rx_gen_promote(store, &q, native_auth, a, NULL, NULL, NULL, NULL);
}

/* ---- workloads ---- */

typedef struct {
    RecInput *rec;
    uint8_t *rec_truth;
    uint32_t n_rec;
    uint8_t *map;
    PlanInput *plan;
    int *plan_truth;
    uint32_t n_plan;
} Workload;

static void workload_make(Workload *w, uint64_t seed, uint32_t n_rec, uint32_t n_plan) {
    memset(w, 0, sizeof *w);
    uint64_t rng = seed;
    w->n_rec = n_rec;
    w->rec = malloc(n_rec * sizeof(RecInput));
    w->rec_truth = malloc(n_rec);
    for (uint32_t i = 0; i < n_rec; i++) {
        rec_sample(&rng, &w->rec[i]);
        w->rec_truth[i] = (uint8_t)rec_truth(&w->rec[i]);
    }
    w->map = malloc(GRID_CELLS);
    plan_map(seed ^ 0xabcdef, w->map);
    w->n_plan = n_plan;
    w->plan = malloc(n_plan * sizeof(PlanInput));
    w->plan_truth = malloc(n_plan * sizeof(int));
    for (uint32_t i = 0; i < n_plan; i++) {
        plan_sample(&rng, w->map, &w->plan[i]);
        w->plan_truth[i] = plan_truth(&w->plan[i]);
    }
}

static void workload_free(Workload *w) {
    free(w->rec);
    free(w->rec_truth);
    free(w->map);
    free(w->plan);
    free(w->plan_truth);
}

static RxCogRequirement req_rec(void) {
    RxCogRequirement q;
    memset(&q, 0, sizeof q);
    q.operation_class = RX_COG_OP_RECOGNIZE;
    q.minimum_quality_ppm = 950000;
    q.uncertainty_budget_ppm = 20000;
    q.memory_requirement = 64;
    q.precision_requirement = RX_COG_PREC_APPROX;
    q.latency_budget_ns = 200000;
    q.hardware_constraints = RX_COG_HW_CPU_P | RX_COG_HW_CPU_E;
    q.escalation_allowed = 3;
    return q;
}

static RxCogRequirement req_plan(uint32_t depth) {
    RxCogRequirement q;
    memset(&q, 0, sizeof q);
    q.operation_class = RX_COG_OP_PLAN;
    q.minimum_quality_ppm = 998000;
    q.uncertainty_budget_ppm = 1000;
    q.memory_requirement = GRID_CELLS;
    q.planning_depth = depth;
    q.precision_requirement = RX_COG_PREC_EXACT;
    q.latency_budget_ns = 2000000;
    q.hardware_constraints = RX_COG_HW_CPU_P | RX_COG_HW_CPU_E;
    q.escalation_allowed = 2;
    return q;
}

/* All engines of an operation on the same labelled requests. */
static const uint32_t REC_ENGINES[] = {ENG_POLICY, ENG_NEURAL, ENG_GENERAL, ENG_ENSEMBLE};
static const uint32_t PLAN_ENGINES[] = {ENG_GREEDY, ENG_FOCUSED, ENG_JSPACE};

static PlanOutput g_pout;

static uint64_t thread_ns(void) { return now_ns(CLOCK_THREAD_CPUTIME_ID); }

static void calibrate(RxCogLedger *l, const Workload *w) {
    for (uint32_t i = 0; i < w->n_rec; i++) {
        uint32_t conf[4];
        uint8_t ok[4], failed[4];
        uint64_t ns[4];
        for (int k = 0; k < 4; k++) {
            RecOutput out;
            uint64_t t0 = thread_ns();
            int rc = cog_engine_fn(REC_ENGINES[k])(NULL, RX_COG_OP_RECOGNIZE, &w->rec[i], &out, &conf[k]);
            ns[k] = thread_ns() - t0;
            failed[k] = rc != 0;
            ok[k] = rc == 0 && out.label == w->rec_truth[i];
        }
        CHECK(rx_route_observe(l, RX_COG_OP_RECOGNIZE, 4, REC_ENGINES, conf, ok, ns, failed) == RX_COG_OK,
              "observe recognition");
    }
    for (uint32_t i = 0; i < w->n_plan; i++) {
        uint32_t conf[3];
        uint8_t ok[3], failed[3];
        uint64_t ns[3];
        for (int k = 0; k < 3; k++) {
            uint64_t t0 = thread_ns();
            int rc = cog_engine_fn(PLAN_ENGINES[k])(NULL, RX_COG_OP_PLAN, &w->plan[i], &g_pout, &conf[k]);
            ns[k] = thread_ns() - t0;
            failed[k] = rc != 0;
            ok[k] = rc == 0 && plan_correct(&w->plan[i], &g_pout, w->plan_truth[i]);
        }
        CHECK(rx_route_observe(l, RX_COG_OP_PLAN, 3, PLAN_ENGINES, conf, ok, ns, failed) == RX_COG_OK,
              "observe planning");
    }
    /* Cost per call: each engine alone over its calibration set, repeated for
     * at least 400 ms: processor time for the batch, and metered energy when
     * there is a meter (the 1 mJ counter step is then < 0.1%). */
    for (int op = 0; op < 2; op++) {
        const uint32_t *ids = op == 0 ? REC_ENGINES : PLAN_ENGINES;
        int n = op == 0 ? 4 : 3;
        for (int k = 0; k < n; k++) {
            RxCogEngineFn fn = cog_engine_fn(ids[k]);
            uint64_t calls = 0, t0 = now_ns(CLOCK_MONOTONIC), c0 = thread_ns();
            Energy e0 = meter_read();
            do {
                uint32_t conf;
                if (op == 0) {
                    RecOutput out;
                    for (uint32_t i = 0; i < w->n_rec; i++) fn(NULL, 0, &w->rec[i], &out, &conf);
                    calls += w->n_rec;
                } else {
                    for (uint32_t i = 0; i < w->n_plan; i++) fn(NULL, 1, &w->plan[i], &g_pout, &conf);
                    calls += w->n_plan;
                }
            } while (now_ns(CLOCK_MONOTONIC) - t0 < 400000000ull);
            Energy e1 = meter_read();
            uint64_t cpu = thread_ns() - c0;
            CHECK(rx_route_observe_batch(l, ids[k], (uint32_t)op, calls, cpu, g_meter[0] != 0,
                                         cpu_uj(e0, e1) * 1000ull) ==
                      RX_COG_OK, "observe batch cost");
        }
    }
}

typedef struct {
    uint64_t cpu_ns, wall_ns, cpu_uj, pkg_uj;
    uint32_t rec_correct, plan_correct;
    uint32_t exhausted, refused, max_escalations, escalation_violations;
    uint32_t final_count[32];
    uint32_t escalated;
} PassResult;

/* One pass over the workload. routed = 1: through the router.
 * routed = 0: the always-most-expensive realization for each operation. */
static void run_pass(RxCogRouter *r, const Workload *w, int routed, PassResult *pr) {
    memset(pr, 0, sizeof *pr);
    RxCogRequirement qr = req_rec();
    uint32_t top_rec = 0, top_plan = 0;
    RxCogEngineFn frec = NULL, fplan = NULL;
    if (!routed) {
        rx_route_most_expensive(r, &qr, &top_rec);
        RxCogRequirement qp = req_plan(0);
        rx_route_most_expensive(r, &qp, &top_plan);
        frec = cog_engine_fn(top_rec);
        fplan = cog_engine_fn(top_plan);
    }
    Energy e0 = meter_read();
    uint64_t c0 = thread_ns(), w0 = now_ns(CLOCK_MONOTONIC);
    for (uint32_t i = 0; i < w->n_rec; i++) {
        RecOutput out = {-1};
        if (routed) {
            RxCogTrace t;
            int rc = rx_route_run(r, &qr, &w->rec[i], &out, sizeof out, &t);
            if (rc == RX_COG_ERR_EXHAUSTED) pr->exhausted++;
            else if (rc != RX_COG_OK) pr->refused++;
            if (t.escalations > pr->max_escalations) pr->max_escalations = t.escalations;
            if (t.escalations > qr.escalation_allowed) pr->escalation_violations++;
            if (t.escalations) pr->escalated++;
            pr->final_count[t.final_engine % 32]++;
        } else {
            uint32_t conf;
            frec(NULL, RX_COG_OP_RECOGNIZE, &w->rec[i], &out, &conf);
            pr->final_count[top_rec % 32]++;
        }
        pr->rec_correct += out.label == w->rec_truth[i];
    }
    for (uint32_t i = 0; i < w->n_plan; i++) {
        RxCogRequirement qp = req_plan(w->plan[i].bound);
        g_pout.status = -1;
        g_pout.len = 0;
        if (routed) {
            RxCogTrace t;
            int rc = rx_route_run(r, &qp, &w->plan[i], &g_pout, sizeof g_pout, &t);
            if (rc == RX_COG_ERR_EXHAUSTED) pr->exhausted++;
            else if (rc != RX_COG_OK) pr->refused++;
            if (t.escalations > pr->max_escalations) pr->max_escalations = t.escalations;
            if (t.escalations > qp.escalation_allowed) pr->escalation_violations++;
            if (t.escalations) pr->escalated++;
            pr->final_count[t.final_engine % 32]++;
        } else {
            uint32_t conf;
            fplan(NULL, RX_COG_OP_PLAN, &w->plan[i], &g_pout, &conf);
            pr->final_count[top_plan % 32]++;
        }
        pr->plan_correct += (uint32_t)plan_correct(&w->plan[i], &g_pout, w->plan_truth[i]);
    }
    pr->cpu_ns = thread_ns() - c0;
    pr->wall_ns = now_ns(CLOCK_MONOTONIC) - w0;
    Energy e1 = meter_read();
    pr->cpu_uj = cpu_uj(e0, e1);
    pr->pkg_uj = e1.pkg - e0.pkg;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t median(uint64_t *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_u64);
    return v[n / 2];
}

/* ---- results for the receipt ---- */

static struct {
    double train_s;
    uint32_t gen0, gen1, gen2;
    RxCogLadder ladder0_rec, ladder0_plan, ladder1_rec, ladder1_plan, ladder2_rec;
    uint32_t top_rec, top_plan;
    PassResult routed, base;
    uint64_t r_cpu_ns, b_cpu_ns, r_cpu_uj, b_cpu_uj, r_pkg_uj, b_pkg_uj, r_wall, b_wall;
    int energy_measured;
    uint32_t bypass_attempts, bypass_refused;
    uint32_t damaged_rec_correct, damaged_n, repaired_rec_correct, repaired_n;
    uint64_t repaired_cpu_ns, repaired_cpu_uj, damaged_base_cpu_uj, damaged_base_cpu_ns;
    uint32_t repaired_base_correct;
    RxCogRealization prof[8];
    uint32_t n_prof;
    char model_digest[65];
} R;

static void hex(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static int ladder_eq(const RxCogLadder *a, const RxCogLadder *b) {
    if (a->n != b->n) return 0;
    for (uint32_t i = 0; i < a->n; i++)
        if (a->engine[i] != b->engine[i] || a->accept_min_bin[i] != b->accept_min_bin[i]) return 0;
    return 1;
}

static void print_ladder(const char *name, const RxCogLadder *l) {
    printf("    %-22s", name);
    for (uint32_t i = 0; i < l->n; i++) printf(" %u(bin>=%u)", l->engine[i], l->accept_min_bin[i]);
    printf("  q=%.4f lower=%.4f ns=%llu nj=%llu why=%u\n", l->expected_quality_ppm / 1e6,
           l->quality_lower_ppm / 1e6, (unsigned long long)l->expected_ns,
           (unsigned long long)l->expected_nj, l->reason);
}

static void rm_rf(const char *dir) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("warning: could not clear %s\n", dir);
}

/* ---- scenarios ---- */

static void t_constraints(RxCogRouter *r) {
    RxCogLadder l;
    RxCogRequirement q = req_rec();
    q.precision_requirement = RX_COG_PREC_EXACT;
    CHECK(rx_route_plan(r, &q, &l) == RX_COG_ERR_UNSATISFIABLE, "exact recognition has no realization");
    q = req_rec();
    q.temporal_requirement = 4;
    CHECK(rx_route_plan(r, &q, &l) == RX_COG_ERR_UNSATISFIABLE, "no engine keeps 4 steps of history");
    q = req_rec();
    q.hardware_constraints = RX_COG_HW_CPU_E;
    int rc = rx_route_plan(r, &q, &l);
    int uses_p = 0;
    for (uint32_t i = 0; rc == RX_COG_OK && i < l.n; i++)
        uses_p |= l.engine[i] == ENG_GENERAL || l.engine[i] == ENG_ENSEMBLE;
    CHECK(rc != RX_COG_OK || !uses_p, "efficiency-core-only request never reaches a P-core engine");
    q = req_rec();
    q.memory_requirement = 1u << 20;
    CHECK(rx_route_plan(r, &q, &l) == RX_COG_ERR_UNSATISFIABLE, "no engine holds 1 MiB");
    q = req_rec();
    q.minimum_quality_ppm = 999900;
    CHECK(rx_route_plan(r, &q, &l) == RX_COG_ERR_UNSATISFIABLE,
          "a quality no ladder is shown to reach is refused, not approximated");
    q = req_rec();
    q.latency_budget_ns = 1;
    CHECK(rx_route_plan(r, &q, &l) == RX_COG_ERR_UNSATISFIABLE, "latency budget is enforced");
    q = req_rec();
    q.escalation_allowed = 0;
    rc = rx_route_plan(r, &q, &l);
    CHECK(rc == RX_COG_OK && l.n == 1, "no escalation allowed: one rung (rc %d n %u)", rc, l.n);
    print_ladder("recognize, no escal.", &l);
    q = req_plan(2000);
    rc = rx_route_plan(r, &q, &l);
    CHECK(rc == RX_COG_OK && l.n == 1 && l.engine[0] == ENG_JSPACE,
          "planning depth 2000 leaves only exhaustive exploration");
    q = req_plan(100);
    q.escalation_allowed = 9;
    rc = rx_route_plan(r, &q, &l);
    CHECK(rc == RX_COG_OK && l.n <= RX_COG_MAX_LADDER, "ladder length is capped");
    if (R.energy_measured) {
        q = req_rec();
        RxCogLadder base;
        rx_route_plan(r, &q, &base);
        q.energy_budget_nj = base.expected_nj / 2 ? base.expected_nj / 2 : 1;
        rc = rx_route_plan(r, &q, &l);
        CHECK(rc == RX_COG_ERR_UNSATISFIABLE || l.expected_nj <= q.energy_budget_nj, "energy budget is enforced");
    }
}

/* A model built against a different registry, and a model torn on disk,
 * never replace the loaded profiles. */
static void t_model_integrity(Auth *a, AienosCapRef cap, const Workload *calib) {
    char dir[512];
    if (omega_evidence_path("COGNITIVE_ROUTING/store-integrity/x", dir, sizeof dir) != 0) return;
    dir[strlen(dir) - 2] = 0;
    rm_rf(dir);
    mkdir(dir, 0700);
    RxGenStore *store = NULL;
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open integrity store");
    if (!store) return;
    RxCogRouter *r = NULL, *other = NULL;
    rx_route_create(&r);
    cog_register_all(r);
    RxCogLedger *l = NULL;
    rx_route_ledger_create(r, &l);
    Workload small = *calib;
    small.n_rec = 256;
    small.n_plan = 64;
    calibrate(l, &small);
    uint64_t good = 0;
    CHECK(rx_route_propose(l, store, SUBJ_PROPOSER, 1, 1, &good) == RX_COG_OK, "propose good");
    CHECK(promote(store, a, good, SUBJ_AUTHORITY, cap) == RX_GEN_OK, "promote good");
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == good, "load good");
    RxCogRealization before;
    rx_route_realization(r, ENG_POLICY, &before);

    /* foreign: a router whose policy engine declares another capacity */
    rx_route_create(&other);
    RxCogDeclaration d = {ENG_POLICY, RX_COG_CLASS_SMALL_POLICY, 1, 0, 9999, 0, 0, RX_COG_PREC_APPROX, 0};
    rx_route_register(other, &d, cog_engine_fn(ENG_POLICY), NULL, 1);
    RxCogLedger *lf = NULL;
    rx_route_ledger_create(other, &lf);
    uint64_t foreign = 0;
    CHECK(rx_route_propose(lf, store, SUBJ_PROPOSER, 1, 1, &foreign) == RX_COG_OK, "propose foreign");
    CHECK(promote(store, a, foreign, SUBJ_AUTHORITY, cap) == RX_GEN_OK, "promote foreign");
    CHECK(rx_route_load(r, store) == RX_COG_ERR_MODEL, "foreign model refused");
    CHECK(rx_route_generation(r) == good, "router kept the good generation after a foreign model");
    RxCogRealization after;
    rx_route_realization(r, ENG_POLICY, &after);
    CHECK(memcmp(&before, &after, sizeof before) == 0, "profiles unchanged after foreign model");

    /* torn: promote the good ledger again, then flip one byte of its model */
    uint64_t again = 0;
    CHECK(rx_route_propose(l, store, SUBJ_PROPOSER, 1, 1, &again) == RX_COG_OK, "propose again");
    CHECK(promote(store, a, again, SUBJ_AUTHORITY, cap) == RX_GEN_OK, "promote again");
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == again, "load again");
    char path[640];
    snprintf(path, sizeof path, "%s/g/%llu/model", dir, (unsigned long long)again);
    int fd = open(path, O_RDWR);
    CHECK(fd >= 0, "open model file");
    if (fd >= 0) {
        uint8_t b;
        if (pread(fd, &b, 1, 100) == 1) {
            b ^= 1;
            CHECK(pwrite(fd, &b, 1, 100) == 1, "flip");
        }
        close(fd);
    }
    RxCogRouter *fresh = NULL;
    rx_route_create(&fresh);
    cog_register_all(fresh);
    CHECK(rx_route_load(fresh, store) == RX_COG_ERR_MODEL, "torn model refused by a fresh router");
    CHECK(rx_route_load(r, store) == RX_COG_ERR_MODEL && rx_route_generation(r) == again,
          "torn model refused; loaded generation kept");
    rx_route_destroy(fresh);
    rx_route_ledger_destroy(lf);
    rx_route_destroy(other);
    rx_route_ledger_destroy(l);
    rx_route_destroy(r);
    rx_gen_close(store);
}

/* Each pair is one routed and one most-expensive pass, back to back, order
 * alternating. The meter covers whole CPU clusters, so other work on the
 * machine lands in both; a pair is won only by the routed pass using less. */
static uint64_t g_pair_cpu_uj[PASSES][2], g_pair_cpu_ns[PASSES][2];
static uint32_t g_pair_compute_wins, g_pair_energy_wins;
static double g_load_before, g_load_after;

static double loadavg(void) {
    double v = -1;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp) {
        if (fscanf(fp, "%lf", &v) != 1) v = -1;
        fclose(fp);
    }
    return v;
}

/* An engine that fails outright on part of its input: the failures are in its
 * profile, and a failed rung escalates like an unconfident one. */
static int eng_flaky(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    const RecInput *in = input;
    if (op != RX_COG_OP_RECOGNIZE || in->x[0] > 0.3f) return -1;
    ((RecOutput *)output)->label = rec_truth(in);
    *conf = RX_COG_CONF_ONE;
    return 0;
}

static void t_failure(Auth *a, AienosCapRef cap) {
    char dir[512];
    if (omega_evidence_path("COGNITIVE_ROUTING/store-failure/x", dir, sizeof dir) != 0) return;
    dir[strlen(dir) - 2] = 0;
    rm_rf(dir);
    mkdir(dir, 0700);
    RxGenStore *store = NULL;
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open failure store");
    if (!store) return;
    RxCogRouter *r = NULL;
    rx_route_create(&r);
    RxCogDeclaration flaky = {31, RX_COG_CLASS_SMALL_POLICY, 1, 0, 64, 0, 0, RX_COG_PREC_APPROX, 0};
    RxCogDeclaration general = {ENG_GENERAL, RX_COG_CLASS_GENERAL, 1, 0, 1u << 16, 0, 0, RX_COG_PREC_APPROX, 0};
    CHECK(rx_route_register(r, &flaky, eng_flaky, NULL, 0) == RX_COG_OK, "register flaky");
    CHECK(rx_route_register(r, &general, cog_engine_fn(ENG_GENERAL), NULL, 1) == RX_COG_OK, "register general");
    RxCogLedger *l = NULL;
    rx_route_ledger_create(r, &l);
    uint64_t rng = 0xf1a4e;
    const uint32_t ids[2] = {31, ENG_GENERAL};
    uint32_t failures = 0;
    for (uint32_t i = 0; i < 1500; i++) {
        RecInput in;
        rec_sample(&rng, &in);
        uint32_t conf[2];
        uint8_t ok[2], failed[2];
        uint64_t ns[2];
        for (int k = 0; k < 2; k++) {
            RecOutput out = {-1};
            RxCogEngineFn fn = k ? cog_engine_fn(ENG_GENERAL) : eng_flaky;
            uint64_t t0 = thread_ns();
            int rc = fn(NULL, 0, &in, &out, &conf[k]);
            ns[k] = thread_ns() - t0;
            failed[k] = rc != 0;
            ok[k] = rc == 0 && out.label == rec_truth(&in);
        }
        failures += failed[0];
        rx_route_observe(l, 0, 2, ids, conf, ok, ns, failed);
    }
    uint64_t cand = 0;
    CHECK(rx_route_propose(l, store, SUBJ_PROPOSER, 1, 1, &cand) == RX_COG_OK, "propose flaky evidence");
    CHECK(promote(store, a, cand, SUBJ_AUTHORITY, cap) == RX_GEN_OK, "promote flaky evidence");
    CHECK(rx_route_load(r, store) == RX_COG_OK, "load flaky evidence");
    RxCogRealization x;
    rx_route_realization(r, 31, &x);
    CHECK(x.quality_profile[0].failures == failures && failures > 300,
          "failures are in the profile (%u of %u)", x.quality_profile[0].failures, x.quality_profile[0].observations);
    RxCogRequirement q = req_rec();
    q.hardware_constraints = RX_COG_HW_CPU_P | RX_COG_HW_CPU_E;
    RxCogLadder lad;
    int rc = rx_route_plan(r, &q, &lad);
    CHECK(rc == RX_COG_OK && lad.n == 2 && lad.engine[0] == 31 && lad.engine[1] == ENG_GENERAL,
          "flaky but cheap and right when it answers: tried first");
    uint32_t failed_runs = 0, escalated = 0, wrong_escalation = 0;
    for (uint32_t i = 0; i < 2000; i++) {
        RecInput in;
        rec_sample(&rng, &in);
        RecOutput out = {-1};
        RxCogTrace t;
        rx_route_run(r, &q, &in, &out, sizeof out, &t);
        int flaky_failed = in.x[0] > 0.3f;
        failed_runs += (uint32_t)flaky_failed;
        escalated += t.escalations > 0;
        wrong_escalation += (uint32_t)(flaky_failed != (t.escalations == 1));
    }
    CHECK(wrong_escalation == 0 && escalated == failed_runs,
          "every failure escalated and nothing else did (%u failures, %u escalations)", failed_runs, escalated);
    rx_route_ledger_destroy(l);
    rx_route_destroy(r);
    rx_gen_close(store);
}

static void measure(RxCogRouter *r, const Workload *w, PassResult *routed, PassResult *base,
                    uint64_t out[8]) {
    uint64_t rc[PASSES], bc[PASSES], re[PASSES], be[PASSES], rp[PASSES], bp[PASSES], rw[PASSES], bw[PASSES];
    PassResult tmp;
    run_pass(r, w, 1, &tmp); /* warm caches for both */
    run_pass(r, w, 0, &tmp);
    for (int p = 0; p < PASSES; p++) {
        PassResult a, b;
        if (p % 2 == 0) {
            run_pass(r, w, 1, &a);
            run_pass(r, w, 0, &b);
        } else {
            run_pass(r, w, 0, &b);
            run_pass(r, w, 1, &a);
        }
        if (p == 0) {
            *routed = a;
            *base = b;
        }
        CHECK(a.rec_correct == routed->rec_correct && a.plan_correct == routed->plan_correct &&
                  b.rec_correct == base->rec_correct && b.plan_correct == base->plan_correct,
              "answers identical across passes");
        rc[p] = a.cpu_ns, bc[p] = b.cpu_ns, re[p] = a.cpu_uj, be[p] = b.cpu_uj;
        rp[p] = a.pkg_uj, bp[p] = b.pkg_uj, rw[p] = a.wall_ns, bw[p] = b.wall_ns;
        g_pair_cpu_uj[p][0] = a.cpu_uj, g_pair_cpu_uj[p][1] = b.cpu_uj;
        g_pair_cpu_ns[p][0] = a.cpu_ns, g_pair_cpu_ns[p][1] = b.cpu_ns;
        g_pair_compute_wins += a.cpu_ns < b.cpu_ns;
        g_pair_energy_wins += a.cpu_uj < b.cpu_uj;
    }
    out[0] = median(rc, PASSES), out[1] = median(bc, PASSES), out[2] = median(re, PASSES);
    out[3] = median(be, PASSES), out[4] = median(rp, PASSES), out[5] = median(bp, PASSES);
    out[6] = median(rw, PASSES), out[7] = median(bw, PASSES);
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
    hex(d, out);
}

static void ladder_json(FILE *fp, const RxCogLadder *l) {
    fprintf(fp, "{\"engines\": [");
    for (uint32_t i = 0; i < l->n; i++) fprintf(fp, "%s%u", i ? ", " : "", l->engine[i]);
    fprintf(fp, "], \"accept_from_bin\": [");
    for (uint32_t i = 0; i < l->n; i++) fprintf(fp, "%s%u", i ? ", " : "", l->accept_min_bin[i]);
    fprintf(fp, "], \"expected_quality\": %.4f, \"quality_lower_95\": %.4f, \"expected_ns\": %llu, "
                "\"expected_nj\": %llu, \"reason\": \"%s\"}",
            l->expected_quality_ppm / 1e6, l->quality_lower_ppm / 1e6, (unsigned long long)l->expected_ns,
            (unsigned long long)l->expected_nj, l->reason == RX_COG_WHY_NO_EVIDENCE ? "no_evidence_reference" : "evidence");
}

static double ece(const RxCogCalibration *c) {
    uint64_t n = 0;
    double e = 0;
    for (uint32_t b = 0; b < RX_COG_BINS; b++) n += c->n[b];
    if (!n) return 0;
    for (uint32_t b = 0; b < RX_COG_BINS; b++) {
        if (!c->n[b]) continue;
        double mid = (b + 0.5) / RX_COG_BINS, acc = (double)c->correct[b] / c->n[b];
        e += (double)c->n[b] / (double)n * (acc > mid ? acc - mid : mid - acc);
    }
    return e;
}

static int gate_pass(void) {
    double qmin_rec = req_rec().minimum_quality_ppm / 1e6, qmin_plan = req_plan(0).minimum_quality_ppm / 1e6;
    int quality = (double)R.routed.rec_correct / REC_EVAL >= qmin_rec &&
                  (double)R.routed.plan_correct / PLAN_EVAL >= qmin_plan &&
                  (double)R.base.rec_correct / REC_EVAL >= qmin_rec &&
                  (double)R.base.plan_correct / PLAN_EVAL >= qmin_plan;
    int compute = R.r_cpu_ns < R.b_cpu_ns && g_pair_compute_wins == PASSES;
    int energy = !R.energy_measured || (R.r_cpu_uj < R.b_cpu_uj && g_pair_energy_wins == PASSES);
    return g_fail == 0 && quality && compute && energy && R.bypass_refused == R.bypass_attempts &&
           R.routed.escalation_violations == 0;
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("COGNITIVE_ROUTING/rx_cognitive_routing_receipt.json", path, sizeof path) != 0) return;
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
    int pass = gate_pass();
    fprintf(fp, "{\n  \"schema\": \"OMEGA_COGNITIVE_ROUTING_V1\",\n  \"run_id\": \"%s\",\n",
            omega_evidence_run_id());
    fprintf(fp, "  \"candidate_commit\": %s%s%s,\n  \"candidate_bound\": %s,\n  \"run_commit\": \"%s\",\n",
            candidate ? "\"" : "", candidate ? candidate : "null", candidate ? "\"" : "",
            bound ? "true" : "false", commit);
    fprintf(fp, "  \"tree_dirty\": %s,\n  \"aienos_commit\": %s%s%s,\n  \"checks\": %d,\n  \"failures\": %d,\n",
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "", aienos ? aienos : "null",
            aienos ? "\"" : "", g_checks, g_fail);
    fprintf(fp, "  \"test_binary_sha256\": \"%s\",\n", digest);
    fprintf(fp, "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\", \"cpus\": \"%s\"},\n",
            u.sysname, u.release, u.machine, g_cpus);
    fprintf(fp, "  \"scope\": \"host processor; stand-in engines (not AIEN); native AIENOS authority library\",\n");
    fprintf(fp, "  \"energy_meter\": %s%s%s,\n", g_meter[0] ? "\"" : "", g_meter[0] ? "aien_spbm cpu_p+cpu_e, pkg" : "null",
            g_meter[0] ? "\"" : "");
    fprintf(fp, "  \"engines_train_seconds\": %.2f,\n", R.train_s);
    fprintf(fp, "  \"generations\": {\"before_evidence\": %u, \"calibrated\": %u, \"after_damage\": %u, "
                "\"model_sha256\": \"%s\"},\n", R.gen0, R.gen1, R.gen2, R.model_digest);
    fprintf(fp, "  \"realizations\": [\n");
    for (uint32_t i = 0; i < R.n_prof; i++) {
        const RxCogRealization *x = &R.prof[i];
        uint32_t op = (x->decl.supported_operations & 1) ? 0 : 1;
        const RxCogCost *c = &x->resource_cost_model[op];
        const RxCogQuality *q = &x->quality_profile[op];
        fprintf(fp, "    {\"id\": %u, \"class\": %u, \"op\": %u, \"generation\": %llu, \"accuracy\": %.4f, "
                    "\"failures\": %u, \"calibration_error\": %.4f, \"mean_ns\": %llu, \"p99_ns\": %llu, "
                    "\"nj_per_call\": %llu, \"energy_measured\": %s}%s\n",
                x->cognitive_engine_id, x->decl.realization_class, op, (unsigned long long)x->generation,
                q->observations ? (double)q->correct / q->observations : 0.0, q->failures,
                ece(&x->uncertainty_profile[op]), (unsigned long long)c->mean_ns,
                (unsigned long long)c->p99_ns, (unsigned long long)c->nj_per_call,
                c->energy_measured ? "true" : "false", i + 1 < R.n_prof ? "," : "");
    }
    fprintf(fp, "  ],\n  \"ladders\": {\n    \"recognize_no_evidence\": ");
    ladder_json(fp, &R.ladder0_rec);
    fprintf(fp, ",\n    \"plan_no_evidence\": ");
    ladder_json(fp, &R.ladder0_plan);
    fprintf(fp, ",\n    \"recognize\": ");
    ladder_json(fp, &R.ladder1_rec);
    fprintf(fp, ",\n    \"plan_bound_100\": ");
    ladder_json(fp, &R.ladder1_plan);
    fprintf(fp, ",\n    \"recognize_after_damage\": ");
    ladder_json(fp, &R.ladder2_rec);
    fprintf(fp, "\n  },\n");
    fprintf(fp, "  \"gate_workload\": {\"recognize\": %u, \"plan\": %u, \"passes\": %d, "
                "\"most_expensive\": {\"recognize\": %u, \"plan\": %u},\n",
            REC_EVAL, PLAN_EVAL, PASSES, R.top_rec, R.top_plan);
    fprintf(fp, "    \"requirement\": {\"recognize_min_quality\": %.4f, \"plan_min_quality\": %.4f},\n",
            req_rec().minimum_quality_ppm / 1e6, req_plan(0).minimum_quality_ppm / 1e6);
    fprintf(fp, "    \"routed\": {\"recognize_quality\": %.4f, \"plan_quality\": %.4f, \"median_cpu_ns\": %llu, "
                "\"median_wall_ns\": %llu, \"median_cpu_energy_uj\": %llu, \"median_pkg_energy_uj\": %llu, "
                "\"escalated\": %u, \"exhausted\": %u, \"refused\": %u, \"max_escalations\": %u, "
                "\"escalation_bound_violations\": %u},\n",
            (double)R.routed.rec_correct / REC_EVAL, (double)R.routed.plan_correct / PLAN_EVAL,
            (unsigned long long)R.r_cpu_ns, (unsigned long long)R.r_wall, (unsigned long long)R.r_cpu_uj,
            (unsigned long long)R.r_pkg_uj, R.routed.escalated, R.routed.exhausted, R.routed.refused,
            R.routed.max_escalations, R.routed.escalation_violations);
    fprintf(fp, "    \"most_expensive_always\": {\"recognize_quality\": %.4f, \"plan_quality\": %.4f, "
                "\"median_cpu_ns\": %llu, \"median_wall_ns\": %llu, \"median_cpu_energy_uj\": %llu, "
                "\"median_pkg_energy_uj\": %llu},\n",
            (double)R.base.rec_correct / REC_EVAL, (double)R.base.plan_correct / PLAN_EVAL,
            (unsigned long long)R.b_cpu_ns, (unsigned long long)R.b_wall, (unsigned long long)R.b_cpu_uj,
            (unsigned long long)R.b_pkg_uj);
    fprintf(fp, "    \"compute_ratio\": %.4f, \"cpu_energy_ratio\": %s%.4f%s, \"pkg_energy_ratio\": %.4f\n  },\n",
            (double)R.r_cpu_ns / (double)(R.b_cpu_ns ? R.b_cpu_ns : 1), R.energy_measured ? "" : "\"n/a ",
            R.energy_measured ? (double)R.r_cpu_uj / (double)(R.b_cpu_uj ? R.b_cpu_uj : 1) : 0.0,
            R.energy_measured ? "" : "\"", R.energy_measured ? (double)R.r_pkg_uj / (double)(R.b_pkg_uj ? R.b_pkg_uj : 1) : 0.0);
    fprintf(fp, "  \"pairs\": {\"load_average_before\": %.2f, \"load_average_after\": %.2f, "
                "\"compute_wins\": %u, \"energy_wins\": %u, \"of\": %d,\n    \"cpu_ns\": [",
            g_load_before, g_load_after, g_pair_compute_wins, g_pair_energy_wins, PASSES);
    for (int p = 0; p < PASSES; p++)
        fprintf(fp, "%s[%llu, %llu]", p ? ", " : "", (unsigned long long)g_pair_cpu_ns[p][0],
                (unsigned long long)g_pair_cpu_ns[p][1]);
    fprintf(fp, "],\n    \"cpu_energy_uj\": [");
    for (int p = 0; p < PASSES; p++)
        fprintf(fp, "%s[%llu, %llu]", p ? ", " : "", (unsigned long long)g_pair_cpu_uj[p][0],
                (unsigned long long)g_pair_cpu_uj[p][1]);
    fprintf(fp, "]},\n");
    fprintf(fp, "  \"learning\": {\"damaged_engine\": %u, \"quality_under_old_generation\": %.4f, "
                "\"quality_after_promotion\": %.4f, \"most_expensive_quality_same_inputs\": %.4f, "
                "\"cpu_ns_after_promotion\": %llu, \"most_expensive_cpu_ns\": %llu, "
                "\"cpu_energy_uj_after_promotion\": %llu, \"most_expensive_cpu_energy_uj\": %llu},\n",
            ENG_NEURAL, R.damaged_n ? (double)R.damaged_rec_correct / R.damaged_n : 0.0,
            R.repaired_n ? (double)R.repaired_rec_correct / R.repaired_n : 0.0,
            R.repaired_n ? (double)R.repaired_base_correct / R.repaired_n : 0.0,
            (unsigned long long)R.repaired_cpu_ns, (unsigned long long)R.damaged_base_cpu_ns,
            (unsigned long long)R.repaired_cpu_uj, (unsigned long long)R.damaged_base_cpu_uj);
    fprintf(fp, "  \"promotion\": {\"bypass_attempts\": %u, \"bypass_refused\": %u, "
                "\"router_links_promote\": false, \"router_links_authority_admin\": false},\n",
            R.bypass_attempts, R.bypass_refused);
    fprintf(fp, "  \"gates\": {\n    \"OMEGA_COGNITIVE_ROUTING_PASS\": \"%s\",\n", pass ? "PASS" : "FAIL");
    fprintf(fp, "    \"not_claimed\": [\"AIEN cognition (engines are stand-ins)\", \"graphics-processor realizations\", "
                "\"energy below idle (the meter is whole-cluster and includes idle draw)\", "
                "\"routing across concurrent requests\", \"online learning without labelled trials\"]\n  }\n}\n");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    place();
    meter_find();
    R.energy_measured = g_meter[0] != 0;
    printf("[*] energy meter: %s\n", g_meter[0] ? g_meter : "none (compute only)");

    uint64_t t0 = now_ns(CLOCK_MONOTONIC);
    cog_engines_train(SEED_TRAIN, 20000, 8);
    R.train_s = (double)(now_ns(CLOCK_MONOTONIC) - t0) / 1e9;
    printf("[*] engines trained in %.1f s\n", R.train_s);

    Auth a;
    CHECK(aienos_cap_start(&a.admin, &a.view) == 0, "authority start");
    AienosCapRef cap = {0, 0}, weak = {0, 0};
    CHECK(mint(&a, SUBJ_AUTHORITY, RX_GEN_RIGHT_PROMOTE, &cap) == 0, "mint promotion right");
    CHECK(mint(&a, SUBJ_AUTHORITY, 0x1u, &weak) == 0, "mint a right that is not promotion");

    char dir[512];
    CHECK(omega_evidence_path("COGNITIVE_ROUTING/store/x", dir, sizeof dir) == 0, "store path");
    dir[strlen(dir) - 2] = 0;
    rm_rf(dir);
    mkdir(dir, 0700);
    RxGenStore *store = NULL;
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open store");

    RxCogRouter *r = NULL;
    CHECK(rx_route_create(&r) == RX_COG_OK, "router");
    CHECK(cog_register_all(r) == RX_COG_OK, "register engines");

    /* 1. no evidence */
    {
        uint64_t genesis = 0, lineage = 0;
        rx_gen_active(store, &genesis, &lineage);
        R.gen0 = (uint32_t)genesis;
    }
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == R.gen0, "load the genesis generation (no model)");
    RxCogRequirement qr = req_rec(), qp = req_plan(100);
    CHECK(rx_route_plan(r, &qr, &R.ladder0_rec) == RX_COG_OK && R.ladder0_rec.n == 1 &&
              R.ladder0_rec.engine[0] == ENG_GENERAL && R.ladder0_rec.reason == RX_COG_WHY_NO_EVIDENCE,
          "no evidence: recognition runs the reference (general) alone");
    CHECK(rx_route_plan(r, &qp, &R.ladder0_plan) == RX_COG_OK && R.ladder0_plan.n == 1 &&
              R.ladder0_plan.engine[0] == ENG_JSPACE, "no evidence: planning runs exhaustive exploration");
    CHECK(rx_route_register(r, &(RxCogDeclaration){99, 1, 1, 0, 0, 0, 0, 1, 0}, cog_engine_fn(ENG_POLICY), NULL, 0) ==
              RX_COG_ERR_ARG, "registry is sealed once routing starts");
    print_ladder("recognize, gen 0", &R.ladder0_rec);
    print_ladder("plan, gen 0", &R.ladder0_plan);

    /* 2. evidence -> candidate -> promotion */
    Workload calib;
    workload_make(&calib, 0xca11b0001ull, REC_CALIB, PLAN_CALIB);
    RxCogLedger *l1 = NULL;
    CHECK(rx_route_ledger_create(r, &l1) == RX_COG_OK, "ledger");
    calibrate(l1, &calib);
    uint64_t cand = 0;
    CHECK(rx_route_propose(l1, store, SUBJ_PROPOSER, 1, 1, &cand) == RX_COG_OK, "propose calibrated model");
    {
        uint8_t *m = NULL;
        size_t mn = 0;
        rx_route_ledger_model(l1, &m, &mn);
        uint8_t d[32];
        sha256_hash(m, mn, d);
        hex(d, R.model_digest);
        printf("[*] model %zu bytes\n", mn);
        free(m);
    }
    RxCogLadder chk;
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == R.gen0, "a proposal changes nothing");
    CHECK(rx_route_plan(r, &qr, &chk) == RX_COG_OK && ladder_eq(&chk, &R.ladder0_rec), "ladder unchanged by proposal");
    R.bypass_attempts++;
    R.bypass_refused += promote(store, &a, cand, SUBJ_PROPOSER, cap) != RX_GEN_OK;
    R.bypass_attempts++;
    R.bypass_refused += promote(store, &a, cand, SUBJ_AUTHORITY, weak) != RX_GEN_OK;
    CHECK(R.bypass_refused == R.bypass_attempts, "promotion by the proposer or without the right is refused");
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == R.gen0, "refused promotion changes nothing");
    CHECK(promote(store, &a, cand, SUBJ_AUTHORITY, cap) == RX_GEN_OK, "promotion with the right");
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == cand, "router follows the promotion");
    R.gen1 = (uint32_t)cand;
    CHECK(rx_route_plan(r, &qr, &R.ladder1_rec) == RX_COG_OK, "calibrated recognition ladder");
    CHECK(rx_route_plan(r, &qp, &R.ladder1_plan) == RX_COG_OK, "calibrated planning ladder");
    print_ladder("recognize, gen 1", &R.ladder1_rec);
    print_ladder("plan<=100, gen 1", &R.ladder1_plan);
    CHECK(R.ladder1_rec.quality_lower_ppm >= qr.minimum_quality_ppm, "chosen recognition ladder meets requirement on evidence");
    static const uint32_t ALL[] = {ENG_POLICY, ENG_NEURAL, ENG_GENERAL, ENG_ENSEMBLE, ENG_GREEDY, ENG_FOCUSED, ENG_JSPACE};
    for (uint32_t i = 0; i < 7; i++) {
        rx_route_realization(r, ALL[i], &R.prof[i]);
        uint32_t op = (R.prof[i].decl.supported_operations & 1) ? 0 : 1;
        const RxCogQuality *q = &R.prof[i].quality_profile[op];
        const RxCogCost *c = &R.prof[i].resource_cost_model[op];
        printf("    engine %u class %u: accuracy %.4f ece %.4f mean %llu ns p99 %llu ns %llu nJ/call\n", ALL[i],
               R.prof[i].decl.realization_class, (double)q->correct / (q->observations ? q->observations : 1),
               ece(&R.prof[i].uncertainty_profile[op]), (unsigned long long)c->mean_ns,
               (unsigned long long)c->p99_ns, (unsigned long long)c->nj_per_call);
        CHECK(R.prof[i].generation == cand, "profile carries its generation");
    }
    R.n_prof = 7;

    /* 5a. constraints */
    t_constraints(r);

    /* 3. gate */
    Workload eval;
    workload_make(&eval, 0xe7a10001ull, REC_EVAL, PLAN_EVAL);
    rx_route_most_expensive(r, &qr, &R.top_rec);
    rx_route_most_expensive(r, &qp, &R.top_plan);
    printf("[*] most expensive: recognize %u, plan %u\n", R.top_rec, R.top_plan);
    uint64_t m[8];
    g_load_before = loadavg();
    measure(r, &eval, &R.routed, &R.base, m);
    g_load_after = loadavg();
    CHECK(g_pair_compute_wins == PASSES, "routed used less processor time in every pair (%u of %d)", g_pair_compute_wins, PASSES);
    if (R.energy_measured)
        CHECK(g_pair_energy_wins == PASSES, "routed used less metered energy in every pair (%u of %d)", g_pair_energy_wins, PASSES);
    R.r_cpu_ns = m[0], R.b_cpu_ns = m[1], R.r_cpu_uj = m[2], R.b_cpu_uj = m[3];
    R.r_pkg_uj = m[4], R.b_pkg_uj = m[5], R.r_wall = m[6], R.b_wall = m[7];
    printf("[*] routed:   recognize %.4f plan %.4f  cpu %.1f ms  cpu energy %.1f mJ  pkg %.1f mJ  escalated %u\n",
           (double)R.routed.rec_correct / REC_EVAL, (double)R.routed.plan_correct / PLAN_EVAL, R.r_cpu_ns / 1e6,
           R.r_cpu_uj / 1e3, R.r_pkg_uj / 1e3, R.routed.escalated);
    printf("[*] baseline: recognize %.4f plan %.4f  cpu %.1f ms  cpu energy %.1f mJ  pkg %.1f mJ\n",
           (double)R.base.rec_correct / REC_EVAL, (double)R.base.plan_correct / PLAN_EVAL, R.b_cpu_ns / 1e6,
           R.b_cpu_uj / 1e3, R.b_pkg_uj / 1e3);
    printf("    final engines (routed):");
    for (int i = 0; i < 32; i++)
        if (R.routed.final_count[i]) printf(" %d:%u", i, R.routed.final_count[i]);
    printf("\n");
    CHECK(R.routed.escalation_violations == 0, "escalation stayed within its bound");
    CHECK(R.routed.refused == 0, "no routed request refused");
    CHECK((double)R.routed.rec_correct / REC_EVAL >= qr.minimum_quality_ppm / 1e6, "routed recognition meets requirement");
    CHECK((double)R.routed.plan_correct / PLAN_EVAL >= qp.minimum_quality_ppm / 1e6, "routed planning meets requirement");
    CHECK((double)R.base.rec_correct / REC_EVAL >= qr.minimum_quality_ppm / 1e6, "baseline recognition meets requirement");
    CHECK((double)R.base.plan_correct / PLAN_EVAL >= qp.minimum_quality_ppm / 1e6, "baseline planning meets requirement");
    CHECK(R.r_cpu_ns < R.b_cpu_ns, "routed uses less processor time");
    if (R.energy_measured) CHECK(R.r_cpu_uj < R.b_cpu_uj, "routed uses less metered energy");

    /* 4. learning under promotion */
    cog_damage_neural(0xdead, 0.6f);
    Workload audit;
    workload_make(&audit, 0xa0d17001ull, REC_EVAL, 0);
    PassResult dam;
    run_pass(r, &audit, 1, &dam);
    R.damaged_rec_correct = dam.rec_correct;
    R.damaged_n = REC_EVAL;
    printf("[*] neural module damaged: routed recognition %.4f under still-active generation %u\n",
           (double)dam.rec_correct / REC_EVAL, R.gen1);
    CHECK((double)dam.rec_correct / REC_EVAL < qr.minimum_quality_ppm / 1e6,
          "the damage is real: quality under the old generation falls below the requirement");
    Workload calib2;
    workload_make(&calib2, 0xca11b0002ull, REC_CALIB, PLAN_CALIB);
    RxCogLedger *l2 = NULL;
    rx_route_ledger_create(r, &l2);
    calibrate(l2, &calib2);
    uint64_t cand2 = 0;
    CHECK(rx_route_propose(l2, store, SUBJ_PROPOSER, 1, 1, &cand2) == RX_COG_OK, "propose after damage");
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == cand, "new evidence alone changes nothing");
    CHECK(rx_route_plan(r, &qr, &chk) == RX_COG_OK && ladder_eq(&chk, &R.ladder1_rec), "ladder unchanged before promotion");
    R.bypass_attempts++;
    R.bypass_refused += promote(store, &a, cand2, SUBJ_AUTHORITY, weak) != RX_GEN_OK;
    CHECK(promote(store, &a, cand2, SUBJ_AUTHORITY, cap) == RX_GEN_OK, "promote the new evidence");
    CHECK(rx_route_load(r, store) == RX_COG_OK && rx_route_generation(r) == cand2, "router follows");
    R.gen2 = (uint32_t)cand2;
    CHECK(rx_route_plan(r, &qr, &R.ladder2_rec) == RX_COG_OK, "recognition ladder after damage");
    print_ladder("recognize, gen 2", &R.ladder2_rec);
    Workload after;
    workload_make(&after, 0xaf7e0001ull, REC_EVAL, 0);
    PassResult rep, rbase;
    uint64_t best_r = UINT64_MAX, best_b = UINT64_MAX, e_r = 0, e_b = 0;
    for (int p = 0; p < 3; p++) {
        run_pass(r, &after, 1, &rep);
        run_pass(r, &after, 0, &rbase);
        if (rep.cpu_ns < best_r) best_r = rep.cpu_ns, e_r = rep.cpu_uj;
        if (rbase.cpu_ns < best_b) best_b = rbase.cpu_ns, e_b = rbase.cpu_uj;
    }
    R.repaired_rec_correct = rep.rec_correct;
    R.repaired_base_correct = rbase.rec_correct;
    R.repaired_n = REC_EVAL;
    R.repaired_cpu_ns = best_r, R.damaged_base_cpu_ns = best_b, R.repaired_cpu_uj = e_r, R.damaged_base_cpu_uj = e_b;
    printf("[*] after promotion: routed recognition %.4f (most expensive %.4f), cpu %.1f vs %.1f ms\n",
           (double)rep.rec_correct / REC_EVAL, (double)rbase.rec_correct / REC_EVAL, best_r / 1e6, best_b / 1e6);
    CHECK((double)rep.rec_correct / REC_EVAL >= qr.minimum_quality_ppm / 1e6, "quality restored after promotion");
    CHECK(best_r < best_b, "still cheaper than the most expensive after learning");
    cog_restore_neural();

    /* 5b. model integrity */
    t_model_integrity(&a, cap, &calib);
    t_failure(&a, cap);

    rx_route_ledger_destroy(l1);
    rx_route_ledger_destroy(l2);
    workload_free(&calib);
    workload_free(&calib2);
    workload_free(&eval);
    workload_free(&audit);
    workload_free(&after);
    rx_route_destroy(r);
    rx_gen_close(store);
    aienos_cap_stop(a.admin, a.view);
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    printf("OMEGA_COGNITIVE_ROUTING_PASS: %s\n", gate_pass() ? "PASS" : "FAIL");
    return gate_pass() ? 0 : 1;
}
