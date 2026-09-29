/* R13: one world; after the goal, only production requests and observation. */
#include "runtime/rx_living.h"
#include "runtime/rx_resident_gpu.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <stdatomic.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum { EXTERNAL = 100, ISSUER = 3 };
enum { POSITIVE, NO_AIEN, NO_PROMOTION, REVOKED_EXPERIMENT,
       STALE_GPU, FAILED_VERIFICATION };
#define M 64u
#define N 256u
#define CLASS_A725 0xd87u
#define CLASS_X925 0xd85u
#define RES_INTENT 0x6130010ull

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxOmegaFaculty omega;
    RxAienFaculty aien;
    RxAegisFaculty aegis;
    RxLiving living;
    RxLivingPromoter promoter;
    RxGenStore *gen;
    RxGpuSeat *seat;
    pthread_t acceptor;
    atomic_int acceptor_stop;
    int acceptor_live;
    atomic_int acceptor_error;
    RxObjRef intent;
    RxCapRef ext_request, ext_goal, ext_placement, ext_intent;
    RxCapRef seat_input, seat_output;
    uint32_t r_ask;
    uint64_t request_seq, served, wrong;
    uint64_t unpromoted_use;    /* results that used a realization not yet in force */
    pthread_t producer;
    atomic_int producer_stop, producer_error;
    int producer_live;
    uint64_t minted;
    char generation_dir[128];
} Rig;
static int g_stage;
typedef struct {
    uint64_t before, after, recovered;
    RxObjRef goal, plan, search, gpu_input, gpu_output, evidence, belief;
    RxObjRef selection, candidate, promotion, inforce;
    uint64_t target_ns, incumbent_ns, selected_ps, reference_ps;
    uint64_t final_expected_ns, final_status, final_prediction_epoch;
    uint64_t aien_acts, omega_acts, seat_acts, aegis_decide_acts, root_install_acts;
    uint64_t promote_acts, prepare_acts;
    uint64_t gpu_claims, gpu_completions, gpu_worker;
    uint64_t production_aien, production_omega, production_gpu;
    uint64_t production_before_promotion, production_after_promotion, production_during_promotion;
    uint64_t unpromoted_use, externals_after_goal, crumbs, links_verified, aien_overlap;
    uint64_t caps_swept, promote_holders;
    uint64_t promotion_barrier_ns, promotion_latency_ns;
    int causal_verified, silicon_observed;
    char selected_identity[65], promoted_identity[65], rebuilt_identity[65];
    char realization_sha256[65];
} Receipt;
static Receipt g_receipt;
static int g_controls[5];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Busy wait without a system call: a 20 us sleep costs about 80 us on Linux. */
static void spin_us(unsigned us) {
    uint64_t until = now_ns() + (uint64_t)us * 1000u;
    while (now_ns() < until) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
}

static void pause_us(int us) {
    struct timespec ts = {0, (long)us * 1000L};
    nanosleep(&ts, NULL);
}

static RxCapRef mint(Rig *r, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office, ref = {UINT32_MAX, 0};
    aienos_cap_office(r->admin, &office);
    AienosCapMint m = {ISSUER, subject, resource, rights, 0,
                       {UINT32_MAX, 0}, office};
    if (aienos_cap_mint(r->admin, &m, &ref) == 0) r->minted++;
    return (RxCapRef){ref.cap_id, ref.generation};
}

static uint64_t field(Rig *r, RxObjRef ref, uint32_t i) {
    RxObject o;
    return rx_world_read(&r->w, ref, &o) == RX_OK ? o.field[i] : UINT64_MAX;
}

static const RxSnapshotDep *snapshot(RxCtx *c, RxObjRef ref) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == ref.id) return &c->in[i];
    return NULL;
}

/* Client intent becomes an R8 request through a reaction of that client. */
static int ask(RxCtx *c) {
    Rig *r = c->user;
    const RxSnapshotDep *in = snapshot(c, r->intent);
    const RxSnapshotDep *out = snapshot(c, r->aegis.o[0].request);
    if (!in || !out) return -1;
    if (!in->field[0] || in->field[0] == out->field[0]) return 0;
    for (uint32_t i = 0; i < 6; i++)
        c->out[c->n_out++] = (RxMutation){r->aegis.o[0].request, i, in->field[i]};
    return 0;
}

static int core_sets(cpu_set_t *a, cpu_set_t *x) {
    CPU_ZERO(a); CPU_ZERO(x);
    DIR *d = opendir("/sys/devices/system/cpu");
    if (!d) return -1;
    struct dirent *e;
    int na = 0, nx = 0;
    while ((e = readdir(d))) {
        int cpu = -1;
        if (sscanf(e->d_name, "cpu%d", &cpu) != 1 || cpu < 0 || cpu >= CPU_SETSIZE)
            continue;
        char path[256];
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", cpu);
        FILE *f = fopen(path, "r");
        unsigned long long midr = 0;
        if (!f) continue;
        int ok = fscanf(f, "%llx", &midr);
        fclose(f);
        if (ok != 1) continue;
        uint32_t part = (uint32_t)(midr >> 4) & 0xfffu;
        if (part == CLASS_A725) { CPU_SET(cpu, a); na++; }
        if (part == CLASS_X925) { CPU_SET(cpu, x); nx++; }
    }
    closedir(d);
    return na && nx ? 0 : -1;
}

static int move_threads(const cpu_set_t *set) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return -1;
    struct dirent *e;
    int moved = 0;
    while ((e = readdir(d))) {
        int tid = atoi(e->d_name);
        if (tid > 0 && sched_setaffinity(tid, sizeof(*set), set) == 0) moved++;
    }
    closedir(d);
    return moved;
}

static int new_object(Rig *r, uint32_t type, uint64_t resource, RxObjRef *out) {
    uint64_t z[RX_MAX_FIELDS] = {0};
    return rx_world_create(&r->w, type, RX_PERSIST_RESIDENT, resource, z, out);
}

static int start(Rig *r, int mode) {
    int with_aien = mode != NO_AIEN;
    int promotion_authority = mode != NO_PROMOTION;
    memset(r, 0, sizeof *r);
    g_stage = 1;
    if (aienos_cap_start(&r->admin, &r->view) != 0) return -1;
    if (rx_world_init_native(&r->w, r->view, 4, 1u << 17) != RX_OK) return -1;
    r->w.external_subject = EXTERNAL;
    if (!mkdtemp(strcpy(r->generation_dir, "/tmp/r13-living-XXXXXX"))) return -1;
    if (rx_gen_open(r->generation_dir, &r->gen) != RX_GEN_OK) return -1;
    g_stage = 2;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    RxOmegaConfig oc;
    rx_omega_default_config(&oc);
    oc.hot_calls = 64; oc.hot_ns = 200000; oc.margin_pct = 10;
    if (mode == FAILED_VERIFICATION) oc.defect = RX_OMEGA_DEFECT_CRASH;
    if (rx_omega_create_objects(&r->omega, &r->w, &oc) != RX_OK) return -1;
    RxAienConfig ac;
    rx_aien_default_config(&ac);
    RxAienInputs ai = {r->omega.o.demand, r->omega.o.selection};
    if (rx_aien_create_objects(&r->aien, &r->w, &ac, &ai) != RX_OK) return -1;
    if (rx_living_create(&r->living, &r->w, &r->aien, &r->omega,
                         r->gen, r->view) != RX_OK) return -1;
    if (new_object(r, 0x6135, RES_INTENT, &r->intent) != RX_OK) return -1;

    /* R8 permits exactly one experiment output resource. */
    RxAegisPolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.n_rules = 1;
    pol.rules[0] = (RxAegisRule){1, RX_LIVING_SEAT_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, RW, 0, 0};
    RxAegisClient cl = {RX_LIVING_SEAT_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, RW};
    if (rx_aegis_create(&r->aegis, &r->w, r->admin, &pol, &cl, 1) != RX_OK) return -1;
    RxAegisCaps aeg;
    memset(&aeg, 0, sizeof aeg);
    aeg.aegis_request = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
    aeg.aegis_approval = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_APPROVAL), R);
    aeg.aegis_decision = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RW);
    aeg.root_request = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
    aeg.root_decision = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), R);
    for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
        aeg.root_slot[j] = mint(r, RX_AEGIS_ROOT_SUBJ,
            rx_aegis_res(0, RX_AEGIS_RES_SLOT0+j), RW);
    if (rx_aegis_register(&r->aegis, &aeg) != RX_OK) return -1;
    g_stage = 3;

    RxOmegaCaps omega;
    memset(&omega, 0xff, sizeof omega);
    omega.serve[RX_OMEGA_RES_REQUEST] = mint(r, RX_OMEGA_SUBJ_SERVE,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_REQUEST, R);
    omega.serve[RX_OMEGA_RES_SELECTION] = mint(r, RX_OMEGA_SUBJ_SERVE,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R);
    omega.serve[RX_OMEGA_RES_DEMAND] = mint(r, RX_OMEGA_SUBJ_SERVE,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_DEMAND, RW);
    omega.serve[RX_OMEGA_RES_RESULT] = mint(r, RX_OMEGA_SUBJ_SERVE,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_RESULT, RW);
    omega.omega[RX_OMEGA_RES_DEMAND] = mint(r, RX_OMEGA_SUBJ_OMEGA,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_DEMAND, R);
    omega.omega[RX_OMEGA_RES_SEARCH] = mint(r, RX_OMEGA_SUBJ_OMEGA,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SEARCH, RW);
    omega.omega[RX_OMEGA_RES_SELECTION] = mint(r, RX_OMEGA_SUBJ_OMEGA,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, RW);
    for (uint32_t k = 0; k < r->omega.cfg.n_slots; k++) {
        uint32_t indices[3] = {RX_OMEGA_RES_CANDIDATE0+k,
            RX_OMEGA_RES_VERDICT0+k, RX_OMEGA_RES_MEASURE0+k};
        for (uint32_t j = 0; j < 3; j++)
            omega.omega[indices[j]] = mint(r, RX_OMEGA_SUBJ_OMEGA,
                RX_OMEGA_RES_BASE + indices[j], RW);
    }
    RxCapRef omega_belief = mint(r, RX_OMEGA_SUBJ_OMEGA,
        RX_AIEN_RES_BASE + RX_AIEN_RES_EXPERIMENT_BELIEF, R);
    if (rx_omega_require_evidence(&r->omega, r->aien.o.experiment_belief,
                                  omega_belief) != RX_OK) return -1;
    /* Production runs what the promotion authority put in force, not what
     * Omega merely selected. */
    if (rx_omega_serve_from(&r->omega, r->living.o.inforce, mint(r, RX_OMEGA_SUBJ_SERVE,
            RX_LIVING_RES_BASE + RX_LIVING_RES_INFORCE, R)) != RX_OK) return -1;
    if (rx_omega_register(&r->omega, &omega) != RX_OK) return -1;
    g_stage = 4;

    RxAienCaps aien;
    memset(&aien, 0xff, sizeof aien);
    for (uint32_t i = 0; i < RX_AIEN_RES_COUNT; i++)
        aien.own[i] = mint(r, RX_AIEN_SUBJ, RX_AIEN_RES_BASE + i,
            i == RX_AIEN_RES_PLACEMENT || i == RX_AIEN_RES_GOAL ? R : RW);
    aien.demand = mint(r, RX_AIEN_SUBJ,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_DEMAND, R);
    aien.selection = mint(r, RX_AIEN_SUBJ,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R);
    if (with_aien) {
        if (rx_aien_register(&r->aien, &aien) != RX_OK) return -1;
        g_stage = 41;
        RxCapRef ev = mint(r, RX_AIEN_SUBJ,
            RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, R);
        if (rx_aien_register_experiment(&r->aien, r->living.o.evidence,
                ev, aien.own[RX_AIEN_RES_EXPERIMENT_BELIEF]) != RX_OK) return -1;
        g_stage = 42;
        RxCapRef plan_read = mint(r, RX_OMEGA_SUBJ_OMEGA,
            RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R);
        if (rx_omega_register_reconsider(&r->omega, r->aien.o.plan,
            plan_read, omega.omega[RX_OMEGA_RES_SEARCH],
            omega.omega[RX_OMEGA_RES_SELECTION]) != RX_OK) return -1;
        g_stage = 43;
    }

    RxLivingCaps lc;
    memset(&lc, 0, sizeof lc);
    lc.plan_read = mint(r, RX_LIVING_SUBJ, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R);
    lc.search_read = mint(r, RX_LIVING_SUBJ,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SEARCH, R);
    lc.input_write = mint(r, RX_LIVING_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_INPUT, RW);
    lc.input_seat_read = mint(r, RX_LIVING_SEAT_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_INPUT, R);
    lc.output_read = mint(r, RX_LIVING_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, R);
    lc.evidence_write = mint(r, RX_LIVING_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, RW);
    lc.belief_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_AIEN_RES_BASE + RX_AIEN_RES_EXPERIMENT_BELIEF, R);
    lc.selection_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R);
    lc.evidence_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, R);
    lc.candidate_write = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_CANDIDATE, RW);
    lc.goal_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, R);
    lc.plan_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R);
    lc.search_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SEARCH, R);
    lc.output_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, R);
    r->promoter.world = &r->w;
    r->promoter.store = r->gen;
    r->promoter.authority = r->view;
    r->promoter.candidate = r->living.o.candidate;
    r->promoter.promotion = r->living.o.promotion;
    r->promoter.inforce = r->living.o.inforce;
    r->promoter.candidate_read = mint(r, RX_LIVING_PROMOTE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_CANDIDATE, R);
    r->promoter.promotion_write = mint(r, RX_LIVING_PROMOTE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_PROMOTION, RW);
    r->promoter.inforce_write = mint(r, RX_LIVING_PROMOTE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_INFORCE, RW);
    r->promoter.promotion_authority = promotion_authority ? mint(r, RX_LIVING_PROMOTE_SUBJ,
        RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE) : (RxCapRef){UINT32_MAX, 0};
    lc.output_slot = r->aegis.o[0].slot[0];
    lc.output_slot_read = mint(r, RX_LIVING_SEAT_SUBJ,
        rx_aegis_res(0, RX_AEGIS_RES_SLOT0), R);
    /* R8 acquisition takes place before the measured human episode. */
    r->ext_intent = mint(r, EXTERNAL, RES_INTENT, RX_RIGHT_WRITE);
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "living.experiment.ask";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = RX_LIVING_SEAT_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = ask; d.user = r; d.stamp_proposed = true;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){r->intent, RX_ALL_FIELDS};
    d.n_reads = 1;
    d.reads[0] = (RxDep){r->aegis.o[0].request, RX_ALL_FIELDS};
    d.n_writes = 1;
    d.writes[0] = (RxDep){r->aegis.o[0].request, RX_ALL_FIELDS};
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){mint(r, RX_LIVING_SEAT_SUBJ, RES_INTENT, R), RES_INTENT, R};
    d.caps[1] = (RxCapNeed){mint(r, RX_LIVING_SEAT_SUBJ,
        rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RW),
        rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RW};
    if (rx_world_add_reaction(&r->w, &d, &r->r_ask) != RX_OK) return -1;
    RxMutation request[6] = {
        {r->intent, 0, 1},
        {r->intent, 1, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT},
        {r->intent, 2, RW}, {r->intent, 3, 0}, {r->intent, 4, 0},
        {r->intent, 5, RX_AEGIS_OP_ACQUIRE}};
    if (rx_world_publish_external(&r->w, r->ext_intent, request, 6) <= 0 ||
        rx_world_wait_quiescent(&r->w, 10000) != RX_OK) return -1;
    r->seat_output = (RxCapRef){field(r, lc.output_slot, 0),
                                 field(r, lc.output_slot, 1)};
    r->seat_input = lc.input_seat_read;
    if (field(r, lc.output_slot, 2) != RX_AEGIS_SLOT_LIVE) return -1;
    g_stage = 6;
    if (rx_world_bind_capability(&r->w, r->living.o.input, r->seat_input) != RX_OK ||
        rx_world_bind_capability(&r->w, r->living.o.output, r->seat_output) != RX_OK)
        return -1;
    if (rx_world_enable_resident(&r->w) != RX_OK) return -1;
    int lrc = rx_living_register(&r->living, &lc, &r->promoter);
    if (lrc != RX_OK) {
        fprintf(stderr, "living register rc=%d reactions=%u\n", lrc, r->w.n_reactions);
        return -1;
    }
    RxResourceBudget budget = {0};
    budget.slots = 8;
    budget.memory_bytes = UINT64_MAX;
    budget.energy_budget = UINT64_MAX;
    budget.offered_locality = UINT32_MAX;
    budget.offered_accel = RX_ACCEL_BLACKWELL;
    budget.compute_mask = UINT32_MAX;
    rx_world_set_resources(&r->w, &budget);
    r->ext_request = mint(r, EXTERNAL,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_REQUEST, RX_RIGHT_WRITE);
    r->ext_goal = mint(r, EXTERNAL,
        RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    r->ext_placement = mint(r, EXTERNAL,
        RX_AIEN_RES_BASE + RX_AIEN_RES_PLACEMENT, RX_RIGHT_WRITE);
#ifdef R13_SILICON
    if (rx_gpu_seat_begin(&r->w, &r->seat) != 0 || !r->seat) return -1;
#endif
    g_stage = 7;
    return 0;
}

/* The human goal: bring this matvec below TARGET_PCT percent of the cost the
 * body had confirmed before the goal, preserving its semantics. */
#ifndef TARGET_PCT
#define TARGET_PCT 55u
#endif

static uint64_t expected_digest(uint64_t seed) {
    uint64_t *A = malloc((size_t)M*N*sizeof(uint64_t));
    uint64_t *x = malloc((size_t)N*sizeof(uint64_t));
    uint64_t *y = malloc((size_t)M*sizeof(uint64_t));
    if (!A || !x || !y) { free(A); free(x); free(y); return 0; }
    rx_omega_fill(seed, A, x, M, N);
    omega_matvec_reference(A, x, y, M, N);
    uint64_t result = rx_omega_digest(y, M);
    free(A); free(x); free(y);
    return result;
}

/* One ordinary production request. The harness publishes it and reads the
 * result; it never touches a faculty. */
static int serve(Rig *r) {
    uint64_t seq = ++r->request_seq;
    uint64_t seed = seq * 0x2545F4914F6CDD1Dull;
    RxMutation m[4] = {{r->omega.o.request, 0, seq},
        {r->omega.o.request, 1, M}, {r->omega.o.request, 2, N},
        {r->omega.o.request, 3, seed}};
    if (rx_world_publish_external(&r->w, r->ext_request, m, 4) <= 0) return -1;
    uint64_t t = now_ns();
    while (field(r, r->omega.o.result, 0) != seq) {
        if (now_ns() - t > 5000000000ull) return -1;
        spin_us(5);   /* a workload does not nap between requests */
    }
    if (field(r, r->omega.o.result, 1) != expected_digest(seed)) r->wrong++;
    uint64_t used = field(r, r->omega.o.result, 2);
    /* The in-force record only moves forward, so reading it after the result
     * can only excuse a use that was in force by then. */
    if (used != 0 && (field(r, r->living.o.inforce, 0) == 0 ||
                      field(r, r->living.o.inforce, 1) != used))
        r->unpromoted_use++;
    __atomic_add_fetch(&r->served, 1, __ATOMIC_RELAXED);
    return 0;
}

static void *producer_main(void *arg) {
    Rig *r = arg;
    while (!atomic_load(&r->producer_stop))
        if (serve(r) != 0) { atomic_store(&r->producer_error, 1); break; }
    return NULL;
}

static int start_producer(Rig *r) {
    atomic_store(&r->producer_stop, 0);
    if (pthread_create(&r->producer, NULL, producer_main, r) != 0) return -1;
    r->producer_live = 1;
    return 0;
}

static void stop_producer(Rig *r) {
    if (!r->producer_live) return;
    atomic_store(&r->producer_stop, 1);
    pthread_join(r->producer, NULL);
    r->producer_live = 0;
}

static int publish_placement(Rig *r, uint64_t cls) {
    RxMutation m[2] = {{r->aien.o.placement, 0,
                         field(r, r->aien.o.placement, 0)+1},
                        {r->aien.o.placement, 1, cls}};
    return rx_world_publish_external(&r->w, r->ext_placement, m, 2) > 0 ? 0 : -1;
}

static int publish_goal(Rig *r, uint64_t target) {
    RxMutation m[3] = {{r->aien.o.goal, 0, 1},
        {r->aien.o.goal, 1, rx_omega_regime(M, N)},
        {r->aien.o.goal, 2, target}};
    return rx_world_publish_external(&r->w, r->ext_goal, m, 3) > 0 ? 0 : -1;
}

/* Is `wanted` a causal ancestor of (or equal to) `node`? Iterative, with a
 * visited set; parents always have smaller ids. */
static uint8_t *g_seen;
static uint64_t *g_stack;
static int ancestor(RxWorld *w, uint64_t node, uint64_t wanted) {
    if (!node || !wanted || wanted > node || node > w->n_crumbs) return 0;
    if (node == wanted) return 1;
    memset(g_seen, 0, (size_t)w->n_crumbs + 1);
    uint64_t sp = 0;
    g_stack[sp++] = node;
    g_seen[node] = 1;
    while (sp) {
        const RxCrumb *c = rx_world_crumb(w, g_stack[--sp]);
        if (!c) continue;
        for (uint32_t i = 0; i < c->n_parents; i++) {
            uint64_t p = c->parents[i];
            if (p == wanted) return 1;
            if (p < wanted || p > w->n_crumbs || g_seen[p]) continue;
            g_seen[p] = 1;
            g_stack[sp++] = p;
        }
    }
    return 0;
}

/* Production commits that ended while an activation of `faculty` (after
 * crumb `lo`) was executing. */
static uint64_t overlap(Rig *r, uint64_t lo, uint32_t faculty) {
    enum { MAXW = 4096 };
    static uint64_t ws[MAXW], we[MAXW];
    uint32_t nw = 0;
    for (uint64_t id = lo + 1; id <= r->w.n_crumbs && nw < MAXW; id++) {
        const RxCrumb *a = rx_world_crumb(&r->w, id);
        if (a && a->faculty == faculty && a->reaction != UINT32_MAX) {
            ws[nw] = a->t_start_ns;
            we[nw++] = a->t_end_ns;
        }
    }
    uint64_t n = 0;
    for (uint64_t id = lo + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *p = rx_world_crumb(&r->w, id);
        if (!p || p->kind != RX_CRUMB_COMMIT || p->reaction != r->omega.r_serve) continue;
        for (uint32_t j = 0; j < nw; j++)
            if (ws[j] <= p->t_end_ns && p->t_end_ns <= we[j]) { n++; break; }
    }
    return n;
}

static uint64_t production_between(Rig *r, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (uint64_t id = lo + 1; id < hi; id++) {
        const RxCrumb *c = rx_world_crumb(&r->w, id);
        if (c && c->kind == RX_CRUMB_COMMIT && c->reaction == r->omega.r_serve)
            n++;
    }
    return n;
}

/* Ring transport only: accept the seat's completion notices into the world.
 * It carries no meaning and decides nothing. */
static int progress(Rig *r) {
#ifdef R13_SILICON
    int rc = rx_resident_accept(&r->w);
    return rc == RX_ERR_NOT_FOUND || rc == RX_OK ? 0 : rc;
#else
    int rc = rx_resident_seat_step(&r->w);
    if (rc == 1) return rx_resident_accept(&r->w);
    return rc < 0 ? rc : 0;
#endif
}

static void *acceptor_main(void *arg) {
    Rig *r = arg;
    while (!atomic_load(&r->acceptor_stop)) {
        int rc = progress(r);
        if (rc != 0) atomic_store(&r->acceptor_error, rc);
        pause_us(1000);
    }
    return NULL;
}

/* Every capability the authority holds. Only the promotion authority may
 * hold the promotion right; no faculty or experiment subject may hold a
 * privileged right (mint, revoke, reclaim, epoch, clock, promote). */
static int authority_sweep(Rig *r, uint64_t *swept, uint64_t *holders) {
    const uint32_t unprivileged[] = {RX_AIEN_SUBJ, RX_OMEGA_SUBJ_SERVE, RX_OMEGA_SUBJ_OMEGA,
        RX_LIVING_SUBJ, RX_LIVING_SEAT_SUBJ, RX_LIVING_PREPARE_SUBJ, RX_AEGIS_SUBJ, EXTERNAL};
    AienosCapRef office;
    if (aienos_cap_office(r->admin, &office) != 0) return -4;
    *swept = 0; *holders = 0;
    /* Entry 0 is the AIENOS root office itself: the mint, not a holder. Every
     * other entry starts on the boot generation and moves only on reclaim. */
    for (uint32_t id = 1; id < 1024; id++)
        for (uint64_t gen = office.generation; gen < office.generation + 8; gen++) {
            AienosCapEntry e;
            if (aienos_cap_inspect(r->view, (AienosCapRef){id, gen}, &e) != 0) continue;
            if (e.state != 1u) continue;          /* AIENOS_CAP_STATE_LIVE */
            (*swept)++;
            if ((e.rights & RX_RIGHT_PROMOTE) || e.resource == RX_GEN_RES_PROMOTION) {
                if (e.subject != RX_LIVING_PROMOTE_SUBJ || e.rights != RX_GEN_RIGHT_PROMOTE)
                    return -1;
                (*holders)++;
            }
            for (uint32_t i = 0; i < sizeof unprivileged / sizeof unprivileged[0]; i++)
                if (e.subject == unprivileged[i] && (e.rights & RX_RIGHT_PRIVILEGED))
                    return -2;
            if (e.subject == RX_LIVING_PROMOTE_SUBJ &&
                (e.rights & RX_RIGHT_PRIVILEGED & ~RX_RIGHT_PROMOTE))
                return -3;
        }
    return 0;
}

/* After the goal, the only outside publications are production requests. */
static int externals_after(Rig *r, uint64_t goal_crumb, uint64_t *n) {
    *n = 0;
    for (uint64_t id = goal_crumb + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&r->w, id);
        if (!c || c->kind != RX_CRUMB_EXTERNAL) continue;
        (*n)++;
        if (c->n_outputs != 1 || c->outputs[0].obj.id != r->omega.o.request.id) return -1;
    }
    return 0;
}

static long read_file(const char *dir, uint64_t gen, const char *name, void *buf, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/g/%llu/%s", dir, (unsigned long long)gen, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return more ? -1 : (long)got;
}

static void hex(const uint8_t *b, char out[65]) {
    for (uint32_t i = 0; i < 32; i++) sprintf(out + i*2, "%02x", b[i]);
}

/* The promoted generation explains itself backward from what is on disk:
 * its provenance names the crumbs, each crumb still verifies in the world,
 * each is an ancestor of the promotion, and the stored bytes rebuild the
 * identity the in-force record names. */
static int check_durable(Rig *r, uint64_t active, uint64_t promotion, uint64_t *links,
                         char rebuilt[65], char code_sha[65]) {
    RxLivingProvenance pv;
    RxLivingConfig cfg;
    uint8_t code[AARCH64_MAX_CODE_BYTES];
    if (read_file(r->generation_dir, active, "provenance", &pv, sizeof pv) != (long)sizeof pv ||
        memcmp(pv.magic, "R13PROV1", 8) != 0) return -1;
    if (read_file(r->generation_dir, active, "config", &cfg, sizeof cfg) != (long)sizeof cfg ||
        memcmp(cfg.magic, "R13CONF1", 8) != 0) return -2;
    long n = read_file(r->generation_dir, active, "realization", code, sizeof code);
    if (n <= 0 || (uint64_t)n != cfg.code_len) return -3;
    const uint32_t author[RX_LINK_COUNT] = {
        UINT32_MAX, r->aien.r_plan, r->omega.r_reconsider, r->living.r_prepare,
        r->living.r_seat, r->living.r_evidence, r->aien.r_experiment,
        r->omega.r_synth[cfg.kind], r->omega.r_verify[cfg.kind], r->omega.r_measure[cfg.kind],
        r->omega.r_select };
    *links = 0;
    for (uint32_t i = 0; i < RX_LINK_COUNT; i++) {
        const RxCrumb *k = rx_world_crumb(&r->w, pv.link[i].crumb);
        if (!k || memcmp(k->digest, pv.link[i].digest, 32) != 0 ||
            k->reaction != author[i] || pv.link[i].reaction != author[i]) return -10 - (int)i;
        if (!ancestor(&r->w, promotion, pv.link[i].crumb)) return -30 - (int)i;
        (*links)++;
    }
    /* Adjacent links are themselves causally ordered. */
    const uint32_t chain[][2] = {
        {RX_LINK_PLAN, RX_LINK_GOAL}, {RX_LINK_SEARCH, RX_LINK_PLAN},
        {RX_LINK_GPU_INPUT, RX_LINK_SEARCH}, {RX_LINK_GPU_OUTPUT, RX_LINK_GPU_INPUT},
        {RX_LINK_EVIDENCE, RX_LINK_GPU_OUTPUT}, {RX_LINK_BELIEF, RX_LINK_EVIDENCE},
        {RX_LINK_SELECTION, RX_LINK_BELIEF}, {RX_LINK_SYNTH, RX_LINK_SEARCH},
        {RX_LINK_VERDICT, RX_LINK_SYNTH}, {RX_LINK_MEASURE, RX_LINK_VERDICT},
        {RX_LINK_SELECTION, RX_LINK_MEASURE} };
    for (uint32_t i = 0; i < sizeof chain / sizeof chain[0]; i++)
        if (!ancestor(&r->w, pv.link[chain[i][0]].crumb, pv.link[chain[i][1]].crumb))
            return -50 - (int)i;
    SemanticId id;
    if (rx_omega_identity_of(&r->omega, code, (size_t)n, &id) != 0 ||
        memcmp(id.bytes, cfg.identity, 32) != 0) return -4;
    for (uint32_t j = 0; j < 4; j++) {
        uint64_t word = 0;
        for (uint32_t b = 0; b < 8; b++) word |= (uint64_t)id.bytes[j*8+b] << (8*b);
        if (word != field(r, r->living.o.inforce, 1 + j)) return -5;
    }
    hex(id.bytes, rebuilt);
    sha256_ctx c;
    uint8_t h[32];
    sha256_init(&c);
    sha256_update(&c, code, (size_t)n);
    sha256_final(&c, h);
    hex(h, code_sha);
    return 0;
}

/* The seat was made ready and stopped by the authority check. */
static int seat_blocked(Rig *r, uint64_t after) {
    for (uint64_t id = after + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&r->w, id);
        if (c && c->kind == RX_CRUMB_BLOCKED_AUTHORITY && c->reaction == r->living.r_seat)
            return 1;
    }
    return 0;
}

static uint64_t acts(Rig *r, uint32_t id) { return r->w.reactions[id].activations; }

#define FAIL(...) do { fprintf(stderr, "R13 mode %d: ", mode); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return -1; } while (0)

#define U(x) ((unsigned long long)(x))
static int run(Rig *r, int mode) {
    int with_aien = mode != NO_AIEN;
    cpu_set_t a, x;
    if (core_sets(&a, &x) != 0 || move_threads(&a) <= 0) FAIL("needs both core classes");
    if (publish_placement(r, CLASS_A725) != 0) FAIL("placement");
    uint64_t limit = now_ns() + 60000000000ull;
    while (field(r, r->omega.o.selection, 0) < 1 && now_ns() < limit)
        if (serve(r) != 0) FAIL("serve");
    if (field(r, r->omega.o.selection, 0) != 1) FAIL("no first selection");
    uint64_t incumbent = field(r, r->omega.o.selection, 1);
    uint64_t incumbent_ns = 0;
    if (with_aien) {
        while (field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED &&
               now_ns() < limit)
            if (serve(r) != 0) FAIL("serve");
        if (field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED)
            FAIL("AIEN never confirmed the incumbent cost");
        incumbent_ns = field(r, r->aien.o.prediction, 5);
    } else {
        incumbent_ns = field(r, r->omega.o.demand, 1) / field(r, r->omega.o.demand, 0);
    }
    uint64_t target = incumbent_ns * TARGET_PCT / 100u;
    uint64_t before_generation = 0, lineage = 0;
    rx_gen_active(r->gen, &before_generation, &lineage);
    uint64_t before_decide = acts(r, r->aegis.r_decide[0]);
    uint64_t before_install = acts(r, r->aegis.r_install[0]);
    uint64_t before_production = r->w.reactions[r->omega.r_serve].commits;
    uint64_t before_served = r->served;
    if (mode == REVOKED_EXPERIMENT) {
        AienosCapRef office;
        aienos_cap_office(r->admin, &office);
        if (aienos_cap_revoke(r->admin, office,
            (AienosCapRef){r->seat_output.cap_id, r->seat_output.generation}) != 0)
            FAIL("revoke");
    }
    /* Production is a workload of its own, already running when the goal
     * arrives. From here this thread only observes. */
    if (start_producer(r) != 0) FAIL("producer");
    /* The environment changes (the body now runs on the X925 cores) and the
     * human states the goal. After this, the harness only serves and reads. */
    if (move_threads(&x) <= 0 || publish_placement(r, CLASS_X925) != 0 ||
        publish_goal(r, target) != 0) FAIL("goal");
    uint64_t goal_crumb = rx_world_explain(&r->w, r->aien.o.goal, 0);
    if (mode != STALE_GPU) {
        atomic_store(&r->acceptor_stop, 0);
        if (pthread_create(&r->acceptor, NULL, acceptor_main, r) != 0) FAIL("thread");
        r->acceptor_live = 1;
    }
    /* From here production runs on its own thread, like any workload, and
     * this thread only observes. Nothing below advances a faculty. */
    limit = now_ns() + 90000000000ull;
    int stale_injected = 0;
    while (now_ns() < limit) {
        pause_us(20);
        if (atomic_load(&r->producer_error)) FAIL("serve");
        if (mode == STALE_GPU && !stale_injected &&
            r->w.reactions[r->living.r_seat].state == RX_RUNNING) {
            /* The object the seat is writing changes generation mid-claim. */
            if (rx_world_retire(&r->w, r->living.o.output) != RX_OK) FAIL("retire");
            stale_injected = 1;
        }
        int step = mode == STALE_GPU ?
            (stale_injected ? progress(r) : 0) : atomic_load(&r->acceptor_error);
        if (step != 0 && mode != STALE_GPU) FAIL("seat transport %d", step);
        if (field(r, r->living.o.promotion, 0) != 0) break;
        uint64_t served = __atomic_load_n(&r->served, __ATOMIC_RELAXED);
        if (mode == NO_AIEN && served > before_served + 512) break;
        if ((mode == REVOKED_EXPERIMENT || mode == STALE_GPU) &&
            field(r, r->aien.o.plan, 0) && served > before_served + 512) break;
    }
    uint64_t at_promotion = r->w.reactions[r->omega.r_serve].commits;
    int promoted = field(r, r->living.o.promotion, 0) != 0 && r->promoter.result == RX_GEN_OK;

    /* The organism keeps working. With a promotion, until AIEN has a
     * confirmed prediction for the promoted record and has assessed the goal
     * on it. Without one, long enough to see production stay put. */
    uint64_t final_limit = now_ns() + 60000000000ull;
    if (promoted) {
        uint64_t epoch = field(r, r->living.o.inforce, 0);
        while (now_ns() < final_limit && !atomic_load(&r->producer_error) &&
               !(field(r, r->aien.o.prediction, 1) == epoch &&
                 field(r, r->aien.o.prediction, 6) == RX_AIEN_PRED_CONFIRMED &&
                 field(r, r->aien.o.prediction, 4) == CLASS_X925 &&
                 field(r, r->aien.o.assessment, 6) == field(r, r->aien.o.prediction, 0)))
            pause_us(100);
    } else {
        uint64_t until = __atomic_load_n(&r->served, __ATOMIC_RELAXED) + 256;
        while (now_ns() < final_limit && !atomic_load(&r->producer_error) &&
               __atomic_load_n(&r->served, __ATOMIC_RELAXED) < until)
            pause_us(100);
    }
    stop_producer(r);
    if (atomic_load(&r->producer_error)) FAIL("serve");
    uint64_t active = 0, after_lineage = 0;
    rx_gen_active(r->gen, &active, &after_lineage);
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK &&
        mode != REVOKED_EXPERIMENT && mode != STALE_GPU) FAIL("did not quiesce");
    uint64_t checked = 0;
    if (rx_world_verify_crumbs(&r->w, &checked) != 0) FAIL("crumbs do not verify");
    if (r->w.stats.illegal_transitions) FAIL("illegal lifecycle transition");
    if (r->wrong || !r->served ||
        r->w.reactions[r->omega.r_serve].commits <= before_production) FAIL("production");
    if (r->unpromoted_use) FAIL("production ran a realization not in force (%llu)",
                                (unsigned long long)r->unpromoted_use);
    if (acts(r, r->aegis.r_decide[0]) != before_decide ||
        acts(r, r->aegis.r_install[0]) != before_install)
        FAIL("AEGIS or root woke during the episode");
    uint64_t externals = 0, swept = 0, holders = 0;
    if (externals_after(r, goal_crumb, &externals) != 0)
        FAIL("an outside publication other than production after the goal");
    int sw = authority_sweep(r, &swept, &holders);
    if (sw != 0 || swept < r->minted / 2) FAIL("authority sweep %d (%llu live)", sw,
                                               (unsigned long long)swept);
    if (holders != (mode == NO_PROMOTION ? 0u : 1u)) FAIL("promotion holders %llu",
                                                         (unsigned long long)holders);

    if (!with_aien) {
        if (field(r, r->aien.o.plan, 0) || r->w.stats.resident_claims ||
            field(r, r->living.o.candidate, 0) || active != before_generation ||
            field(r, r->living.o.inforce, 0) || r->omega.served_realized)
            FAIL("adaptation without AIEN");
        printf("R13 control A: no AIEN -> no plan, no GPU experiment, no candidate, "
               "generation stays %llu\n", (unsigned long long)active);
        g_controls[0] = 1;
        return 0;
    }
    if (mode == NO_PROMOTION) {
        RxRecoveryRecord rec;
        if (!field(r, r->living.o.candidate, 0) ||
            r->promoter.result != RX_GEN_ERR_AUTHORITY ||
            (int64_t)field(r, r->living.o.promotion, 1) != RX_GEN_ERR_AUTHORITY ||
            active != before_generation || after_lineage != lineage ||
            field(r, r->living.o.inforce, 0) || r->omega.served_realized ||
            rx_gen_recover(r->generation_dir, &rec) != RX_GEN_OK ||
            rec.active_id != before_generation)
            FAIL("candidate without promotion authority");
        printf("R13 control B: verified candidate %llu refused (RX_GEN_ERR_AUTHORITY); "
               "never in force, production stayed on the reference\n",
               (unsigned long long)field(r, r->living.o.candidate, 0));
        g_controls[1] = 1;
        return 0;
    }
    if (mode == REVOKED_EXPERIMENT) {
        if (!field(r, r->aien.o.plan, 0) ||
            !seat_blocked(r, goal_crumb) ||
            r->w.reactions[r->living.r_seat].commits || r->w.stats.resident_claims ||
            field(r, r->living.o.evidence, 0) || field(r, r->living.o.candidate, 0) ||
            active != before_generation || field(r, r->living.o.inforce, 0))
            FAIL("revoked experiment: seat state %s commits %llu claims %llu evidence %llu",
                 rx_state_name(r->w.reactions[r->living.r_seat].state),
                 U(r->w.reactions[r->living.r_seat].commits), U(r->w.stats.resident_claims),
                 U(field(r, r->living.o.evidence, 0)));
        printf("R13 control C: revoked grant -> seat blocked on authority, no claim, "
               "no evidence; %llu requests served meanwhile\n",
               (unsigned long long)(r->served - before_served));
        g_controls[2] = 1;
        return 0;
    }
    if (mode == STALE_GPU) {
        if (!stale_injected || r->w.reactions[r->living.r_seat].commits ||
            field(r, r->living.o.evidence, 0) || field(r, r->living.o.candidate, 0) ||
            active != before_generation || field(r, r->living.o.inforce, 0))
            FAIL("stale GPU result");
        printf("R13 control D: output generation changed mid-claim -> result refused, "
               "no evidence, no candidate\n");
        g_controls[3] = 1;
        return 0;
    }

    /* POSITIVE and FAILED_VERIFICATION: the full episode. */
    if (!promoted) FAIL("no promotion: result %d, prepare refusal %d", r->promoter.result,
                        r->living.last_refusal);
    uint64_t plan = rx_world_explain(&r->w, r->aien.o.plan, 0);
    uint64_t search = rx_world_explain(&r->w, r->omega.o.search, 0);
    uint64_t gpu_input = rx_world_explain(&r->w, r->living.o.input, 2);
    uint64_t gpu = rx_world_explain(&r->w, r->living.o.output, 0);
    uint64_t evidence = rx_world_explain(&r->w, r->living.o.evidence, 0);
    uint64_t belief = rx_world_explain(&r->w, r->aien.o.experiment_belief, 0);
    uint64_t selection = rx_world_explain(&r->w, r->omega.o.selection, 0);
    uint64_t candidate = rx_world_explain(&r->w, r->living.o.candidate, 0);
    uint64_t promotion = rx_world_explain(&r->w, r->living.o.promotion, 0);
    uint64_t inforce = rx_world_explain(&r->w, r->living.o.inforce, 0);
    if (field(r, r->aien.o.plan, 5) != RX_AIEN_WHY_GOAL || field(r, r->aien.o.plan, 6) != 1 ||
        field(r, r->aien.o.assessment, 0) != 1) FAIL("plan was not for the goal");
    uint64_t nodes[] = {goal_crumb, plan, search, gpu_input, gpu, evidence, belief,
                        selection, candidate};
    for (uint32_t i = 0; i < sizeof nodes / sizeof nodes[0]; i++)
        if (!ancestor(&r->w, promotion, nodes[i])) FAIL("promotion does not reach node %u", i);
    if (!ancestor(&r->w, inforce, promotion) || inforce != promotion)
        FAIL("in-force record not published by the promotion");
    if (incumbent != 0 || field(r, r->omega.o.selection, 1) == 0 ||
        field(r, r->aien.o.experiment_belief, 0) != 2 ||
        field(r, r->aien.o.experiment_belief, 2) != RX_AIEN_EXP_SUPPORTED ||
        field(r, r->aien.o.experiment_belief, 3) != RX_LIVING_TRIALS ||
        field(r, r->living.o.evidence, 4) != RX_LIVING_TRIALS ||
        r->w.stats.resident_claims != RX_LIVING_TRIALS ||
        r->w.reactions[r->living.r_seat].commits != RX_LIVING_TRIALS ||
        r->w.reactions[r->living.r_candidate].commits != 1 ||
        r->w.reactions[r->promoter.reaction].commits != 1)
        FAIL("episode shape: claims %llu seat commits %llu trials %llu",
             U(r->w.stats.resident_claims), U(r->w.reactions[r->living.r_seat].commits),
             U(field(r, r->living.o.evidence, 4)));
    if (after_lineage != lineage + 1 || active == before_generation ||
        field(r, r->living.o.inforce, 7) != active)
        FAIL("generation did not advance exactly once");
    for (uint32_t j = 0; j < 4; j++)
        if (field(r, r->living.o.inforce, 1 + j) != field(r, r->omega.o.selection, 1 + j) ||
            field(r, r->living.o.candidate, 2 + j) != field(r, r->omega.o.selection, 1 + j))
            FAIL("in force is not the selected realization");
    if (!r->omega.served_realized) FAIL("production never ran the promoted realization");
    if (mode == FAILED_VERIFICATION) {
        uint32_t bad = RX_OMEGA_SLOTS - 1;
        if (field(r, r->omega.o.verdict[bad], 1) != RX_OMEGA_REFUSED ||
            field(r, r->omega.o.verdict[bad], 5) != RX_OMEGA_WHY_CRASHED ||
            field(r, r->living.o.inforce, 1) == field(r, r->omega.o.candidate[bad], 2) ||
            field(r, r->living.o.candidate, 2) == field(r, r->omega.o.candidate[bad], 2))
            FAIL("defective candidate");
        printf("R13 control E: crashing candidate refused by the verifier; the promoted "
               "generation holds a verified one\n");
        g_controls[4] = 1;
    }
    RxRecoveryRecord recovery;
    if (rx_gen_recover(r->generation_dir, &recovery) != RX_GEN_OK || !recovery.coherent ||
        recovery.active_id != active || recovery.lineage != after_lineage)
        FAIL("recovery");
    uint64_t links = 0;
    char rebuilt[65], code_sha[65];
    int dr = check_durable(r, active, promotion, &links, rebuilt, code_sha);
    if (dr != 0) FAIL("durable provenance %d", dr);
    /* AIEN's belief after the change: a confirmed prediction on the new
     * record, and the goal assessed against it. */
    uint64_t final_status = field(r, r->aien.o.assessment, 4);
    uint64_t final_expected = field(r, r->aien.o.assessment, 3);
    /* AIEN's part of the episode: from the goal to its belief about the
     * physical evidence. The strict count, production that committed while an
     * AIEN activation itself was executing, is reported, not gated. */
    uint64_t during_aien = production_between(r, goal_crumb, belief);
    uint64_t aien_overlap = overlap(r, goal_crumb, RX_FACULTY_AIEN);
    uint64_t during_omega = production_between(r, plan, selection);
    uint64_t during_gpu = production_between(r, gpu_input, evidence);
    uint64_t before_promotion = production_between(r, goal_crumb, promotion);
    /* The R9 barrier runs inside generation.promote. Production that committed
     * between the candidate and its promotion shows it is not a global stop. */
    uint64_t during_promotion = production_between(r, candidate, promotion);
    printf("R13 costs: incumbent %llu ns, target %llu ns, selected %llu ps, reference on new "
           "cores %llu ps, AIEN now expects %llu ns, goal status %llu\n",
           (unsigned long long)incumbent_ns, (unsigned long long)target,
           (unsigned long long)field(r, r->omega.o.selection, 5),
           (unsigned long long)field(r, r->omega.o.selection, 7),
           (unsigned long long)final_expected, (unsigned long long)final_status);
    if (final_status != RX_AIEN_GOAL_MET || final_expected > target ||
        field(r, r->aien.o.prediction, 1) != field(r, r->living.o.inforce, 0))
        FAIL("AIEN's belief does not show the goal met under the new generation");
    int need_gpu_overlap = 0;
#ifdef R13_SILICON
    need_gpu_overlap = 1;   /* the processor stand-in runs the add instantly */
#endif
    if (mode == POSITIVE && (!during_aien || !during_omega ||
                             (need_gpu_overlap && !during_gpu) || !before_promotion ||
                             !during_promotion))
        FAIL("production intervals: AIEN=%llu Omega=%llu GPU=%llu before promotion=%llu "
             "during promotion=%llu",
             (unsigned long long)during_aien, (unsigned long long)during_omega,
             (unsigned long long)during_gpu, (unsigned long long)before_promotion,
             (unsigned long long)during_promotion);
    const RxCrumb *gpu_c = rx_world_crumb(&r->w, gpu);
#ifdef R13_SILICON
    if (!r->seat || !gpu_c || gpu_c->worker != RX_SEAT_BLACKWELL)
        FAIL("GPU result was not published by the resident Blackwell seat");
#endif
    if (mode == POSITIVE) {
        Receipt *g = &g_receipt;
        g->before = before_generation;
        g->after = active;
        g->recovered = recovery.active_id;
        g->goal = r->aien.o.goal;
        g->plan = r->aien.o.plan;
        g->search = r->omega.o.search;
        g->gpu_input = r->living.o.input;
        g->gpu_output = r->living.o.output;
        g->evidence = r->living.o.evidence;
        g->belief = r->aien.o.experiment_belief;
        g->selection = r->omega.o.selection;
        g->candidate = r->living.o.candidate;
        g->promotion = r->living.o.promotion;
        g->inforce = r->living.o.inforce;
        g->target_ns = target;
        g->incumbent_ns = incumbent_ns;
        g->selected_ps = field(r, r->omega.o.selection, 5);
        g->reference_ps = field(r, r->omega.o.selection, 7);
        g->final_expected_ns = final_expected;
        g->final_status = final_status;
        g->final_prediction_epoch = field(r, r->aien.o.prediction, 1);
        g->aien_acts = acts(r, r->aien.r_observe) + acts(r, r->aien.r_predict) +
            acts(r, r->aien.r_explain) + acts(r, r->aien.r_assess) +
            acts(r, r->aien.r_plan) + acts(r, r->aien.r_experiment);
        g->omega_acts = acts(r, r->omega.r_watch) + acts(r, r->omega.r_reconsider) +
            acts(r, r->omega.r_select);
        for (uint32_t k = 0; k < r->omega.cfg.n_slots; k++)
            g->omega_acts += acts(r, r->omega.r_synth[k]) + acts(r, r->omega.r_verify[k]) +
                acts(r, r->omega.r_measure[k]);
        g->seat_acts = acts(r, r->living.r_seat);
        g->aegis_decide_acts = acts(r, r->aegis.r_decide[0]) - before_decide;
        g->root_install_acts = acts(r, r->aegis.r_install[0]) - before_install;
        g->promote_acts = acts(r, r->promoter.reaction);
        g->prepare_acts = acts(r, r->living.r_candidate);
        g->gpu_claims = r->w.stats.resident_claims;
        g->gpu_completions = r->w.reactions[r->living.r_seat].commits;
        g->gpu_worker = gpu_c ? gpu_c->worker : 0;
        g->production_aien = during_aien;
        g->aien_overlap = aien_overlap;
        g->production_omega = during_omega;
        g->production_gpu = during_gpu;
        g->production_before_promotion = before_promotion;
        g->production_during_promotion = during_promotion;
        g->production_after_promotion = r->w.reactions[r->omega.r_serve].commits - at_promotion;
        g->unpromoted_use = r->unpromoted_use;
        g->externals_after_goal = externals;
        g->crumbs = checked;
        g->links_verified = links;
        g->caps_swept = swept;
        g->promote_holders = holders;
        g->promotion_barrier_ns = field(r, r->living.o.promotion, 2);
        const RxCrumb *kc = rx_world_crumb(&r->w, candidate);
        const RxCrumb *kp = rx_world_crumb(&r->w, promotion);
        if (kc && kp) g->promotion_latency_ns = kp->t_end_ns - kc->t_end_ns;
        g->causal_verified = 1;
        RxOmegaRealization sel;
        if (rx_omega_store_find(&r->omega, field(r, r->omega.o.selection, 1), &sel) == 0)
            hex(sel.id.bytes, g->selected_identity);
        uint8_t pid[32];
        for (uint32_t j = 0; j < 4; j++)
            for (uint32_t b = 0; b < 8; b++)
                pid[j*8+b] = (uint8_t)(field(r, r->living.o.inforce, 1 + j) >> (8*b));
        hex(pid, g->promoted_identity);
        memcpy(g->rebuilt_identity, rebuilt, 65);
        memcpy(g->realization_sha256, code_sha, 65);
#ifdef R13_SILICON
        g->silicon_observed = 1;
#endif
    }
    printf("R13 %s: generation %llu -> %llu, recovered %llu; served %llu (%llu after "
           "promotion), crumbs %llu, GPU claims %llu, durable links %llu\n",
           mode == POSITIVE ? "positive" : "control E episode",
           (unsigned long long)before_generation, (unsigned long long)active,
           (unsigned long long)recovery.active_id, (unsigned long long)r->served,
           (unsigned long long)(r->w.reactions[r->omega.r_serve].commits - at_promotion),
           (unsigned long long)checked, (unsigned long long)r->w.stats.resident_claims,
           (unsigned long long)links);
    return 0;
}

static void stop(Rig *r) {
    stop_producer(r);
    if (r->acceptor_live) {
        atomic_store(&r->acceptor_stop, 1);
        pthread_join(r->acceptor, NULL);
    }
#ifdef R13_SILICON
    if (r->seat) {
        (void)rx_resident_shutdown(&r->w);
        (void)rx_gpu_seat_finish(r->seat);
    }
#endif
    rx_world_destroy(&r->w);
    rx_aegis_destroy(&r->aegis);
    rx_omega_destroy(&r->omega);
    rx_gen_close(r->gen);
    aienos_cap_stop(r->admin, r->view);
}

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t block[65536], digest[32];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0)
        sha256_update(&c, block, n);
    fclose(f);
    sha256_final(&c, digest);
    hex(digest, out);
}

#define OBJ(o) (o).id, (o).generation

static void receipt(int tests_ok) {
    char path[512], commit[41] = {0}, binary[65], physics[80] = {0};
    if (omega_evidence_path("R13/rx_living_receipt.json", path, sizeof path) != 0)
        return;
    if (!omega_evidence_run_commit(commit)) strcpy(commit, "unknown");
    if (!omega_evidence_physics_commit(physics, sizeof physics))
        strcpy(physics, "unknown");
    binary_digest(binary);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    const char *aienos = getenv("AIENOS_COMMIT");
    int dirty = omega_evidence_tree_dirty();
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 && !dirty;
    int controls = 1;
    for (uint32_t i = 0; i < 5; i++) controls &= g_controls[i];
    const Receipt *g = &g_receipt;
    int pass = tests_ok && controls && bound && g->silicon_observed;
    const char *gate = pass ? "PASS"
        : !tests_ok || !controls ? "FAIL"
        : !g->silicon_observed ? "HOST_PASS_NON_SILICON"
        : "SILICON_PASS_UNBOUND";
    struct utsname host;
    memset(&host, 0, sizeof host);
    uname(&host);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "{\n"
        "  \"schema\": \"AIEN_RX_R13_LIVING_SYSTEM_V1\",\n"
        "  \"run_id\": \"%s\",\n"
        "  \"candidate_commit\": \"%s\",\n"
        "  \"run_commit\": \"%s\",\n"
        "  \"candidate_bound\": %s,\n"
        "  \"tree_dirty\": %s,\n"
        "  \"aienos_commit\": \"%s\",\n"
        "  \"physics_forge_commit\": \"%s\",\n"
        "  \"test_binary_sha256\": \"%s\",\n"
        "  \"machine_identity\": {\"node\": \"%s\", \"system\": \"%s\", "
            "\"release\": \"%s\", \"architecture\": \"%s\"},\n"
        "  \"silicon_observed\": %s,\n"
        "  \"gate\": {\"R13_LIVING_SYSTEM\": \"%s\"},\n"
        "  \"scenario\": {\"operation\": \"omega integer matvec 64x256\", "
            "\"goal\": \"cost below %u%% of the confirmed incumbent, semantics preserved\", "
            "\"incumbent_ns\": %llu, \"target_ns\": %llu, "
            "\"selected_ps_per_call\": %llu, \"reference_ps_per_call_new_cores\": %llu, "
            "\"aien_final_expected_ns\": %llu, \"aien_final_goal_status\": \"%s\", "
            "\"aien_final_prediction_epoch\": %llu},\n"
        "  \"active_generation_before\": %llu,\n"
        "  \"active_generation_after\": %llu,\n"
        "  \"recovered_generation\": %llu,\n"
        "  \"objects\": {\n"
        "    \"human_goal\": {\"id\": %u, \"generation\": %u},\n"
        "    \"aien_plan\": {\"id\": %u, \"generation\": %u},\n"
        "    \"omega_search\": {\"id\": %u, \"generation\": %u},\n"
        "    \"gpu_input\": {\"id\": %u, \"generation\": %u},\n"
        "    \"gpu_output\": {\"id\": %u, \"generation\": %u},\n"
        "    \"experiment_evidence\": {\"id\": %u, \"generation\": %u},\n"
        "    \"aien_experiment_belief\": {\"id\": %u, \"generation\": %u},\n"
        "    \"omega_selection\": {\"id\": %u, \"generation\": %u},\n"
        "    \"generation_candidate\": {\"id\": %u, \"generation\": %u},\n"
        "    \"promotion\": {\"id\": %u, \"generation\": %u},\n"
        "    \"in_force\": {\"id\": %u, \"generation\": %u}\n"
        "  },\n"
        "  \"selected_realization_identity\": \"%s\",\n"
        "  \"promoted_realization_identity\": \"%s\",\n"
        "  \"rebuilt_from_durable_bytes_identity\": \"%s\",\n"
        "  \"promoted_realization_bytes_sha256\": \"%s\",\n"
        "  \"aien_activations\": %llu,\n"
        "  \"omega_activations\": %llu,\n"
        "  \"experiment_seat_activations_on_existing_grant\": %llu,\n"
        "  \"aegis_decide_activations_during_episode\": %llu,\n"
        "  \"root_install_activations_during_episode\": %llu,\n"
        "  \"generation_prepare_activations\": %llu,\n"
        "  \"promotion_authority_activations\": %llu,\n"
        "  \"gpu_claims\": %llu,\n"
        "  \"gpu_completions\": %llu,\n"
        "  \"gpu_publication_worker\": \"0x%llx\",\n"
        "  \"production_commits_during_aien\": %llu,\n"
        "  \"production_commits_while_an_aien_activation_executed\": %llu,\n"
        "  \"production_commits_during_omega\": %llu,\n"
        "  \"production_commits_during_gpu_experiment\": %llu,\n"
        "  \"production_commits_before_promotion\": %llu,\n"
        "  \"production_commits_during_promotion_barrier\": %llu,\n"
        "  \"production_commits_after_promotion\": %llu,\n"
        "  \"production_results_using_unpromoted_realization\": %llu,\n"
        "  \"outside_publications_after_goal_all_production_requests\": %llu,\n"
        "  \"generation_promotion_barrier_ns\": %llu,\n"
        "  \"generation_candidate_to_promotion_ns\": %llu,\n"
        "  \"causal_crumb_count\": %llu,\n"
        "  \"durable_provenance_links_verified\": %llu,\n"
        "  \"causal_verification_result\": \"%s\",\n"
        "  \"authority\": {\"live_capabilities_swept\": %llu, "
            "\"promotion_right_holders\": %llu, \"promotion_subject\": %u},\n"
        "  \"controls\": {\"A_no_aien\": \"%s\", "
            "\"B_no_promotion_authority\": \"%s\", "
            "\"C_revoked_experiment\": \"%s\", "
            "\"D_stale_generation\": \"%s\", "
            "\"E_failed_verification\": \"%s\"},\n"
        "  \"not_claimed\": [\"R14\", \"R15\", \"R16\", "
            "\"neural/general cognition beyond R11\", "
            "\"open-ended realization invention beyond R10\", "
            "\"AIENOS-kernel isolation (authority is the host-library C port)\", "
            "\"the GPU add measures matvec cost (it is a physical witness of the experiment)\"]\n"
        "}\n",
        omega_evidence_run_id(), candidate ? candidate : "unknown", commit,
        bound ? "true" : "false", dirty ? "true" : "false",
        aienos ? aienos : "unknown", physics, binary,
        host.nodename, host.sysname, host.release, host.machine,
        g->silicon_observed ? "true" : "false", gate,
        TARGET_PCT, U(g->incumbent_ns), U(g->target_ns), U(g->selected_ps), U(g->reference_ps),
        U(g->final_expected_ns), g->final_status == RX_AIEN_GOAL_MET ? "MET" : "NOT_MET",
        U(g->final_prediction_epoch),
        U(g->before), U(g->after), U(g->recovered),
        OBJ(g->goal), OBJ(g->plan), OBJ(g->search), OBJ(g->gpu_input), OBJ(g->gpu_output),
        OBJ(g->evidence), OBJ(g->belief), OBJ(g->selection), OBJ(g->candidate),
        OBJ(g->promotion), OBJ(g->inforce),
        g->selected_identity, g->promoted_identity, g->rebuilt_identity,
        g->realization_sha256,
        U(g->aien_acts), U(g->omega_acts), U(g->seat_acts), U(g->aegis_decide_acts),
        U(g->root_install_acts), U(g->prepare_acts), U(g->promote_acts),
        U(g->gpu_claims), U(g->gpu_completions), U(g->gpu_worker),
        U(g->production_aien), U(g->aien_overlap), U(g->production_omega), U(g->production_gpu),
        U(g->production_before_promotion), U(g->production_during_promotion),
        U(g->production_after_promotion),
        U(g->unpromoted_use), U(g->externals_after_goal),
        U(g->promotion_barrier_ns), U(g->promotion_latency_ns),
        U(g->crumbs), U(g->links_verified), g->causal_verified ? "PASS" : "FAIL",
        U(g->caps_swept), U(g->promote_holders), (unsigned)RX_LIVING_PROMOTE_SUBJ,
        g_controls[0] ? "PASS" : "FAIL", g_controls[1] ? "PASS" : "FAIL",
        g_controls[2] ? "PASS" : "FAIL", g_controls[3] ? "PASS" : "FAIL",
        g_controls[4] ? "PASS" : "FAIL");
    fclose(f);
    printf("R13 gate: R13_LIVING_SYSTEM=%s\nR13 receipt: %s\n", gate, path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    g_seen = malloc((1u << 17) + 1);
    g_stack = malloc(sizeof(uint64_t) * ((1u << 17) + 1));
    if (!g_seen || !g_stack) return 1;
    int result = 0;
    static const char *names[] = {"positive", "A no AIEN", "B no promotion authority",
        "C revoked experiment", "D stale generation", "E failed verification"};
    for (int mode = POSITIVE; mode <= FAILED_VERIFICATION; mode++) {
        Rig *r = calloc(1, sizeof *r);
        if (!r) return 1;
        int rc = start(r, mode);
        if (rc != 0) fprintf(stderr, "R13 %s: setup failed at stage %d\n", names[mode], g_stage);
        if (rc == 0) rc = run(r, mode);
        if (rc != 0)
            fprintf(stderr, "R13 %s FAILED: plan %llu search %llu GPU %llu evidence %llu "
                    "belief %llu selection %llu candidate %llu promotion %llu in force %llu "
                    "result %d prepare refusal %d\n", names[mode],
                    U(field(r, r->aien.o.plan, 0)), U(field(r, r->omega.o.search, 0)),
                    U(field(r, r->living.o.output, 0)), U(field(r, r->living.o.evidence, 0)),
                    U(field(r, r->aien.o.experiment_belief, 0)),
                    U(field(r, r->omega.o.selection, 0)), U(field(r, r->living.o.candidate, 0)),
                    U(field(r, r->living.o.promotion, 0)), U(field(r, r->living.o.inforce, 0)),
                    r->promoter.result, r->living.last_refusal);
        if (r->admin && r->view) stop(r);
        free(r);
        if (rc != 0) { result = 1; break; }
    }
    receipt(result == 0);
    free(g_seen);
    free(g_stack);
    return result;
}
