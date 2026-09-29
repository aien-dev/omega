/*
 * R14: attack the R13 organism while it is alive, and watch the mechanisms it
 * already has contain, recover and resume (ADR 0016 R14).
 *
 * Every scenario starts a fresh R13 body: the same objects, faculties,
 * authority and R9 store as tests/runtime/rx_r13_living.c. The harness may
 * inject a fault and may publish what an outside party would publish (a
 * production request, a human goal, a lane request, the body starting). It
 * never writes a faculty's object, selects a fallback, wakes a reaction, edits
 * a generation file or repairs the authority table.
 *
 *   A  corrupt candidate      R10 verifier refuses; the valid candidate promotes
 *   B  forged authority       R7/R8/R9 refuse; honest authority works after revocation
 *   C  reaction cycle         R6 quarantines the cycle inside its episode
 *   D  GPU saturation         R5 holds work back; nothing is dropped or doubled
 *   E  seat killed mid-claim  R12 seat loss; the dead seat's result never lands
 *   F  crash in promotion     R9 crash points in a child process; restart,
 *                             recover, restore what was in force, resume
 *
 * R14_SILICON runs D and E on the resident GB10 seat. The host build uses the
 * R12 processor stand-in and cannot claim the gate.
 */
#include "runtime/rx_living.h"
#include "runtime/rx_resident_gpu.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { EXTERNAL = 100, ISSUER = 3, LANE_SUBJ = 65, ROGUE_SUBJ = 66, STUFFER_SUBJ = 67,
       CYCLE_SUBJ = 68 };
#define M 64u
#define N 256u
#define CLASS_A725 0xd87u
#define CLASS_X925 0xd85u
#define RES_INTENT 0x6130010ull
#define RES_LANE_IN 0x6140001ull
#define RES_LANE_OUT 0x6140002ull
#define RES_LANE_CHK 0x6140003ull
#define RES_LANE_INTENT 0x6140004ull
#define RES_ROGUE 0x6140010ull
#define RES_CYCLE 0x6140020ull
#define LANES 16u
/* What one lane claim holds while it is on the seat (its two windows). R5
 * admits at most LANE_CAP of them at once; the rest wait. */
#define LANE_BYTES (2u * RX_OBJECT_WINDOW)
#define LANE_CAP 4u
#define BUDGET 256u            /* R6 activations per reaction per causal episode */
#ifndef TARGET_PCT
#define TARGET_PCT 55u
#endif
#define U(x) ((unsigned long long)(x))

typedef struct {
    int defect;                 /* RxOmegaDefect for scenario A */
    uint32_t lanes;             /* GPU work lanes (B, D, E) */
    int oscillation;            /* R6 oscillation limit in addition to the budget (C) */
    int crash_step;             /* R9 crash point (F, first process) */
    int seat;                   /* start the resident GB10 seat (silicon build only) */
    const char *gen_dir;        /* reuse an R9 store (F restart); else a new one */
} Opts;

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
    Opts opt;
    pthread_t acceptor;
    atomic_int acceptor_stop, acceptor_pause, acceptor_error;
    int acceptor_live;
    atomic_ullong acc_ok, acc_stale, acc_torn, acc_auth, acc_other;
    RxObjRef intent;
    RxCapRef ext_request, ext_goal, ext_placement, ext_intent, ext_restore;
    RxCapRef seat_input, seat_output;
    uint32_t r_ask;
    uint64_t request_seq, served, wrong, unpromoted_use;
    pthread_t producer;
    atomic_int producer_stop, producer_error;
    int producer_live;
    uint64_t minted;
    char generation_dir[128];
    int own_dir;
    /* GPU lanes: in -> seat add -> out -> CPU check. Output WRITE is read from
     * AEGIS client 1, slot 0. */
    RxObjRef lin[LANES], lout[LANES], lchk[LANES], lane_intent;
    uint32_t r_lane[LANES], r_check[LANES], r_lane_ask;
    RxCapRef ext_lane, ext_lane_intent, lane_in_read;
    uint64_t lane_sent[LANES];
    uint64_t lane_intent_seq;
    /* Everything the harness itself gave a hostile party, so the sweep can
     * prove none of it is live at the end. */
    RxCapRef hostile[32];
    uint32_t n_hostile;
} Rig;

typedef struct { Rig *r; uint32_t k; } LaneCtx;
static LaneCtx g_lane_ctx[LANES];

static int g_stage;

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
    struct timespec ts = {us / 1000000, (long)(us % 1000000) * 1000L};
    nanosleep(&ts, NULL);
}

static RxCapRef mint(Rig *r, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office, ref = {UINT32_MAX, 0};
    aienos_cap_office(r->admin, &office);
    AienosCapMint m = {ISSUER, subject, resource, rights, 0, {UINT32_MAX, 0}, office};
    if (aienos_cap_mint(r->admin, &m, &ref) == 0) r->minted++;
    return (RxCapRef){ref.cap_id, ref.generation};
}

static RxCapRef hostile(Rig *r, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapRef c = mint(r, subject, resource, rights);
    if (r->n_hostile < 32) r->hostile[r->n_hostile++] = c;
    return c;
}

static int cap_revoke(Rig *r, RxCapRef c) {
    AienosCapRef office;
    aienos_cap_office(r->admin, &office);
    return aienos_cap_revoke(r->admin, office, (AienosCapRef){c.cap_id, c.generation});
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

static uint64_t acts(Rig *r, uint32_t id) { return r->w.reactions[id].activations; }

/* Client intent becomes an R8 request through a reaction of that client.
 * The rig's user pointer says which client. */
typedef struct { Rig *r; RxObjRef intent, request; } Ask;
static Ask g_ask[2];
static int ask(RxCtx *c) {
    Ask *a = c->user;
    const RxSnapshotDep *in = snapshot(c, a->intent);
    const RxSnapshotDep *out = snapshot(c, a->request);
    if (!in || !out) return -1;
    if (!in->field[0] || in->field[0] == out->field[0]) return 0;
    for (uint32_t i = 0; i < 6; i++)
        c->out[c->n_out++] = (RxMutation){a->request, i, in->field[i]};
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
        if (sscanf(e->d_name, "cpu%d", &cpu) != 1 || cpu < 0 || cpu >= CPU_SETSIZE) continue;
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

/* ---- GPU lanes ------------------------------------------------------------
 * Workload of the same kind as production, but on the seat: an outside lane
 * request writes operands into lane.in[k]; lane.blackwell.k (R5 Blackwell
 * need, LANE_BYTES held while claimed, output WRITE from an R8 slot) is
 * executed by the resident seat; lane.check.k checks the sum on the CPU. */
static int fn_lane_seat(RxCtx *c) { (void)c; return -1; }

static int fn_lane_check(RxCtx *c) {
    LaneCtx *lc = c->user;
    Rig *r = lc->r;
    uint32_t k = lc->k;
    const RxSnapshotDep *in = snapshot(c, r->lin[k]);
    const RxSnapshotDep *out = snapshot(c, r->lout[k]);
    const RxSnapshotDep *chk = snapshot(c, r->lchk[k]);
    if (!in || !out || !chk) return -1;
    uint64_t expected = (uint32_t)((uint32_t)in->field[0] + (uint32_t)in->field[1]);
    c->out[c->n_out++] = (RxMutation){r->lchk[k], 0, chk->field[0] + 1};
    c->out[c->n_out++] = (RxMutation){r->lchk[k], 1, in->field[2]};
    c->out[c->n_out++] = (RxMutation){r->lchk[k], 2, chk->field[2] + (out->field[0] != expected)};
    c->out[c->n_out++] = (RxMutation){r->lchk[k], 3, out->field[0]};
    return 0;
}

static int lanes_create(Rig *r) {
    for (uint32_t k = 0; k < r->opt.lanes; k++) {
        if (new_object(r, 0x6141, RES_LANE_IN, &r->lin[k]) != RX_OK ||
            new_object(r, 0x6142, RES_LANE_OUT, &r->lout[k]) != RX_OK ||
            new_object(r, 0x6143, RES_LANE_CHK, &r->lchk[k]) != RX_OK ||
            rx_world_attach_physical(&r->w, r->lin[k]) != RX_OK ||
            rx_world_attach_physical(&r->w, r->lout[k]) != RX_OK)
            return -1;
    }
    return new_object(r, 0x6144, RES_LANE_INTENT, &r->lane_intent);
}

static int lanes_register(Rig *r) {
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    RxObjRef slot = r->aegis.o[1].slot[0];
    r->lane_in_read = mint(r, LANE_SUBJ, RES_LANE_IN, R);
    RxCapRef slot_read = mint(r, LANE_SUBJ, rx_aegis_res(1, RX_AEGIS_RES_SLOT0), R);
    RxCapRef out_read = mint(r, LANE_SUBJ, RES_LANE_OUT, R);
    RxCapRef chk_rw = mint(r, LANE_SUBJ, RES_LANE_CHK, RW);
    for (uint32_t k = 0; k < r->opt.lanes; k++) {
        g_lane_ctx[k] = (LaneCtx){r, k};
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = "lane.blackwell";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = LANE_SUBJ;
        d.priority = RX_PRIO_LEARNING;
        d.fn = fn_lane_seat;
        d.user = &g_lane_ctx[k];
        d.need.accelerator_features = RX_ACCEL_BLACKWELL;
        d.need.memory_bytes = LANE_BYTES;
        d.n_triggers = 1;
        d.triggers[0] = (RxDep){r->lin[k], RX_FIELD(0) | RX_FIELD(1)};
        d.n_writes = 1;
        d.writes[0] = (RxDep){r->lout[k], RX_FIELD(0)};
        d.n_caps = 3;
        d.caps[0] = (RxCapNeed){r->lane_in_read, RES_LANE_IN, R};
        d.caps[1] = (RxCapNeed){{UINT32_MAX, 0}, RES_LANE_OUT, RX_RIGHT_WRITE};
        d.caps[2] = (RxCapNeed){slot_read, rx_aegis_res(1, RX_AEGIS_RES_SLOT0), R};
        rx_aegis_use_slot(&d, 1, slot);
        if (rx_world_add_reaction(&r->w, &d, &r->r_lane[k]) != RX_OK) return -1;

        memset(&d, 0, sizeof d);
        d.name = "lane.check";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = LANE_SUBJ;
        d.priority = RX_PRIO_LEARNING;
        d.fn = fn_lane_check;
        d.user = &g_lane_ctx[k];
        d.n_triggers = 1;
        d.triggers[0] = (RxDep){r->lout[k], RX_FIELD(0)};
        d.n_reads = 2;
        d.reads[0] = (RxDep){r->lin[k], RX_ALL_FIELDS};
        d.reads[1] = (RxDep){r->lchk[k], RX_ALL_FIELDS};
        d.n_writes = 1;
        d.writes[0] = (RxDep){r->lchk[k], RX_ALL_FIELDS};
        d.n_caps = 3;
        d.caps[0] = (RxCapNeed){out_read, RES_LANE_OUT, R};
        d.caps[1] = (RxCapNeed){r->lane_in_read, RES_LANE_IN, R};
        d.caps[2] = (RxCapNeed){chk_rw, RES_LANE_CHK, RW};
        if (rx_world_add_reaction(&r->w, &d, &r->r_check[k]) != RX_OK) return -1;
    }
    return 0;
}

/* The lane client asks AEGIS for its output grant, as the R13 seat does. */
static int lane_acquire(Rig *r) {
    RxMutation m[6] = {{r->lane_intent, 0, ++r->lane_intent_seq},
                       {r->lane_intent, 1, RES_LANE_OUT},
                       {r->lane_intent, 2, RX_RIGHT_READ | RX_RIGHT_WRITE},
                       {r->lane_intent, 3, 0}, {r->lane_intent, 4, 0},
                       {r->lane_intent, 5, RX_AEGIS_OP_ACQUIRE}};
    return rx_world_publish_external(&r->w, r->ext_lane_intent, m, 6) > 0 ? 0 : -1;
}

/* One lane request: operands chosen so consecutive sums differ. */
static int lane_submit(Rig *r, uint32_t k) {
    uint64_t j = ++r->lane_sent[k];
    RxMutation m[3] = {{r->lin[k], 0, 0xfffff000u + j * 7u + k},
                       {r->lin[k], 1, 0x1000u + k}, {r->lin[k], 2, j}};
    if (rx_world_publish_external(&r->w, r->ext_lane, m, 3) <= 0) {
        r->lane_sent[k]--;
        return -1;
    }
    return 0;
}

static uint64_t lane_done(Rig *r, uint32_t k) { return field(r, r->lchk[k], 1); }

/* The R13 body, as tests/runtime/rx_r13_living.c starts it, with R6 switched
 * on (an activation budget per causal episode), the restart reaction, and
 * optionally the GPU lanes on a second AEGIS client. */
static int start(Rig *r, const Opts *opt) {
    memset(r, 0, sizeof *r);
    r->opt = *opt;
    g_stage = 1;
    if (aienos_cap_start(&r->admin, &r->view) != 0) return -1;
    if (rx_world_init_native(&r->w, r->view, 4, RX_CRUMBS_LONG_EPISODE) != RX_OK) return -1;
    r->w.external_subject = EXTERNAL;
    if (opt->gen_dir) {
        snprintf(r->generation_dir, sizeof r->generation_dir, "%s", opt->gen_dir);
    } else {
        if (!mkdtemp(strcpy(r->generation_dir, "/tmp/r14-living-XXXXXX"))) return -1;
        r->own_dir = 1;
    }
    if (rx_gen_open(r->generation_dir, &r->gen) != RX_GEN_OK) return -1;
    if (opt->crash_step) rx_gen_set_crash(r->gen, opt->crash_step);
    RxStabilityBudget sb;
    memset(&sb, 0, sizeof sb);
    sb.activation_budget = BUDGET;
    sb.oscillation_limit = (uint32_t)opt->oscillation;
    rx_world_set_stability(&r->w, &sb);
    g_stage = 2;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    RxOmegaConfig oc;
    rx_omega_default_config(&oc);
    oc.hot_calls = 64; oc.hot_ns = 200000; oc.margin_pct = 10;
    oc.defect = (RxOmegaDefect)opt->defect;
    if (rx_omega_create_objects(&r->omega, &r->w, &oc) != RX_OK) return -1;
    RxAienConfig ac;
    rx_aien_default_config(&ac);
    RxAienInputs ai = {r->omega.o.demand, r->omega.o.selection};
    if (rx_aien_create_objects(&r->aien, &r->w, &ac, &ai) != RX_OK) return -1;
    if (rx_living_create(&r->living, &r->w, &r->aien, &r->omega, r->gen, r->view) != RX_OK)
        return -1;
    if (new_object(r, 0x6135, RES_INTENT, &r->intent) != RX_OK) return -1;
    if (opt->lanes && lanes_create(r) != 0) return -1;

    /* R8: client 0 is the R13 experiment seat, client 1 the GPU lanes. */
    RxAegisPolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.n_rules = 2;
    pol.rules[0] = (RxAegisRule){1, RX_LIVING_SEAT_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, RW, 0, 0};
    pol.rules[1] = (RxAegisRule){2, LANE_SUBJ, RES_LANE_OUT, RES_LANE_OUT, RW, 0, 0};
    RxAegisClient cl[2] = {
        {RX_LIVING_SEAT_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT,
         RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, RW},
        {LANE_SUBJ, RES_LANE_OUT, RES_LANE_OUT, RW}};
    uint32_t n_clients = opt->lanes ? 2u : 1u;
    if (rx_aegis_create(&r->aegis, &r->w, r->admin, &pol, cl, n_clients) != RX_OK) return -1;
    RxAegisCaps aeg[2];
    memset(aeg, 0, sizeof aeg);
    for (uint32_t k = 0; k < n_clients; k++) {
        aeg[k].aegis_request = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(k, RX_AEGIS_RES_REQUEST), R);
        aeg[k].aegis_approval = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(k, RX_AEGIS_RES_APPROVAL), R);
        aeg[k].aegis_decision = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(k, RX_AEGIS_RES_DECISION), RW);
        aeg[k].root_request = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(k, RX_AEGIS_RES_REQUEST), R);
        aeg[k].root_decision = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(k, RX_AEGIS_RES_DECISION), R);
        for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
            aeg[k].root_slot[j] = mint(r, RX_AEGIS_ROOT_SUBJ,
                rx_aegis_res(k, RX_AEGIS_RES_SLOT0 + j), RW);
    }
    if (rx_aegis_register(&r->aegis, aeg) != RX_OK) return -1;
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
        uint32_t idx[3] = {RX_OMEGA_RES_CANDIDATE0 + k, RX_OMEGA_RES_VERDICT0 + k,
                           RX_OMEGA_RES_MEASURE0 + k};
        for (uint32_t j = 0; j < 3; j++)
            omega.omega[idx[j]] = mint(r, RX_OMEGA_SUBJ_OMEGA, RX_OMEGA_RES_BASE + idx[j], RW);
    }
    RxCapRef omega_belief = mint(r, RX_OMEGA_SUBJ_OMEGA,
        RX_AIEN_RES_BASE + RX_AIEN_RES_EXPERIMENT_BELIEF, R);
    if (rx_omega_require_evidence(&r->omega, r->aien.o.experiment_belief, omega_belief) != RX_OK)
        return -1;
    if (rx_omega_serve_from(&r->omega, r->living.o.inforce, mint(r, RX_OMEGA_SUBJ_SERVE,
            RX_LIVING_RES_BASE + RX_LIVING_RES_INFORCE, R)) != RX_OK) return -1;
    if (rx_omega_register(&r->omega, &omega) != RX_OK) return -1;
    g_stage = 4;

    RxAienCaps aien;
    memset(&aien, 0xff, sizeof aien);
    for (uint32_t i = 0; i < RX_AIEN_RES_COUNT; i++)
        aien.own[i] = mint(r, RX_AIEN_SUBJ, RX_AIEN_RES_BASE + i,
            i == RX_AIEN_RES_PLACEMENT || i == RX_AIEN_RES_GOAL ? R : RW);
    aien.demand = mint(r, RX_AIEN_SUBJ, RX_OMEGA_RES_BASE + RX_OMEGA_RES_DEMAND, R);
    aien.selection = mint(r, RX_AIEN_SUBJ, RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R);
    if (rx_aien_register(&r->aien, &aien) != RX_OK) return -1;
    if (rx_aien_register_experiment(&r->aien, r->living.o.evidence,
            mint(r, RX_AIEN_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, R),
            aien.own[RX_AIEN_RES_EXPERIMENT_BELIEF]) != RX_OK) return -1;
    if (rx_omega_register_reconsider(&r->omega, r->aien.o.plan,
            mint(r, RX_OMEGA_SUBJ_OMEGA, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R),
            omega.omega[RX_OMEGA_RES_SEARCH], omega.omega[RX_OMEGA_RES_SELECTION]) != RX_OK)
        return -1;
    g_stage = 5;

    RxLivingCaps lc;
    memset(&lc, 0, sizeof lc);
    lc.plan_read = mint(r, RX_LIVING_SUBJ, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R);
    lc.search_read = mint(r, RX_LIVING_SUBJ, RX_OMEGA_RES_BASE + RX_OMEGA_RES_SEARCH, R);
    lc.input_write = mint(r, RX_LIVING_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_INPUT, RW);
    lc.input_seat_read = mint(r, RX_LIVING_SEAT_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_INPUT, R);
    lc.output_read = mint(r, RX_LIVING_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, R);
    lc.evidence_write = mint(r, RX_LIVING_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, RW);
    lc.belief_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_AIEN_RES_BASE + RX_AIEN_RES_EXPERIMENT_BELIEF, R);
    lc.selection_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R);
    lc.evidence_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, R);
    lc.candidate_write = mint(r, RX_LIVING_PREPARE_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_CANDIDATE, RW);
    lc.goal_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, R);
    lc.plan_prepare_read = mint(r, RX_LIVING_PREPARE_SUBJ, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R);
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
    r->promoter.promotion_authority = mint(r, RX_LIVING_PROMOTE_SUBJ,
        RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE);
    lc.output_slot = r->aegis.o[0].slot[0];
    lc.output_slot_read = mint(r, RX_LIVING_SEAT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_SLOT0), R);

    /* R8 acquisition before any episode: the seat's grant, and the lanes'. */
    r->ext_intent = mint(r, EXTERNAL, RES_INTENT, RX_RIGHT_WRITE);
    uint32_t n_ask = opt->lanes ? 2u : 1u;
    for (uint32_t k = 0; k < n_ask; k++) {
        uint32_t subj = k ? LANE_SUBJ : RX_LIVING_SEAT_SUBJ;
        uint64_t res_in = k ? RES_LANE_INTENT : RES_INTENT;
        g_ask[k] = (Ask){r, k ? r->lane_intent : r->intent, r->aegis.o[k].request};
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = k ? "lane.ask" : "living.experiment.ask";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = subj;
        d.priority = RX_PRIO_FOREGROUND;
        d.fn = ask; d.user = &g_ask[k]; d.stamp_proposed = true;
        d.n_triggers = 1;
        d.triggers[0] = (RxDep){g_ask[k].intent, RX_ALL_FIELDS};
        d.n_reads = 1;
        d.reads[0] = (RxDep){g_ask[k].request, RX_ALL_FIELDS};
        d.n_writes = 1;
        d.writes[0] = (RxDep){g_ask[k].request, RX_ALL_FIELDS};
        d.n_caps = 2;
        d.caps[0] = (RxCapNeed){mint(r, subj, res_in, R), res_in, R};
        d.caps[1] = (RxCapNeed){mint(r, subj, rx_aegis_res(k, RX_AEGIS_RES_REQUEST), RW),
                                rx_aegis_res(k, RX_AEGIS_RES_REQUEST), RW};
        if (rx_world_add_reaction(&r->w, &d, k ? &r->r_lane_ask : &r->r_ask) != RX_OK) return -1;
    }
    RxMutation request[6] = {{r->intent, 0, 1},
        {r->intent, 1, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT},
        {r->intent, 2, RW}, {r->intent, 3, 0}, {r->intent, 4, 0},
        {r->intent, 5, RX_AEGIS_OP_ACQUIRE}};
    if (rx_world_publish_external(&r->w, r->ext_intent, request, 6) <= 0) return -1;
    if (opt->lanes) {
        r->ext_lane_intent = mint(r, EXTERNAL, RES_LANE_INTENT, RX_RIGHT_WRITE);
        if (lane_acquire(r) != 0) return -1;
    }
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK) return -1;
    r->seat_output = (RxCapRef){field(r, lc.output_slot, 0), field(r, lc.output_slot, 1)};
    r->seat_input = lc.input_seat_read;
    if (field(r, lc.output_slot, 2) != RX_AEGIS_SLOT_LIVE) return -1;
    if (opt->lanes && field(r, r->aegis.o[1].slot[0], 2) != RX_AEGIS_SLOT_LIVE) return -1;
    g_stage = 6;
    if (rx_world_bind_capability(&r->w, r->living.o.input, r->seat_input) != RX_OK ||
        rx_world_bind_capability(&r->w, r->living.o.output, r->seat_output) != RX_OK)
        return -1;
    if (rx_world_enable_resident(&r->w) != RX_OK) return -1;
    if (rx_living_register(&r->living, &lc, &r->promoter) != RX_OK) return -1;
    if (rx_living_register_restore(&r->living, &r->promoter, mint(r, RX_LIVING_PROMOTE_SUBJ,
            RX_LIVING_RES_BASE + RX_LIVING_RES_RESTORE, RW)) != RX_OK) return -1;
    r->ext_restore = mint(r, EXTERNAL, RX_LIVING_RES_BASE + RX_LIVING_RES_RESTORE, RX_RIGHT_WRITE);
    if (opt->lanes) {
        if (lanes_register(r) != 0) return -1;
        for (uint32_t k = 0; k < opt->lanes; k++)
            if (rx_world_bind_capability(&r->w, r->lin[k], r->lane_in_read) != RX_OK) return -1;
        r->ext_lane = mint(r, EXTERNAL, RES_LANE_IN, RX_RIGHT_WRITE);
    }
    RxResourceBudget budget;
    memset(&budget, 0, sizeof budget);
    budget.slots = 8;
    budget.memory_bytes = opt->lanes ? (uint64_t)LANE_CAP * LANE_BYTES : UINT64_MAX;
    budget.energy_budget = UINT64_MAX;
    budget.offered_locality = UINT32_MAX;
    budget.offered_accel = RX_ACCEL_BLACKWELL;
    budget.compute_mask = UINT32_MAX;
    rx_world_set_resources(&r->w, &budget);
    r->ext_request = mint(r, EXTERNAL, RX_OMEGA_RES_BASE + RX_OMEGA_RES_REQUEST, RX_RIGHT_WRITE);
    r->ext_goal = mint(r, EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    r->ext_placement = mint(r, EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_PLACEMENT, RX_RIGHT_WRITE);
#ifdef R14_SILICON
    if (opt->seat && (rx_gpu_seat_begin(&r->w, &r->seat) != 0 || !r->seat)) return -1;
#endif
    g_stage = 7;
    return 0;
}

/* ---- production and transport -------------------------------------------- */

static uint64_t expected_digest(uint64_t seed) {
    uint64_t *A = malloc((size_t)M * N * sizeof(uint64_t));
    uint64_t *x = malloc((size_t)N * sizeof(uint64_t));
    uint64_t *y = malloc((size_t)M * sizeof(uint64_t));
    if (!A || !x || !y) { free(A); free(x); free(y); return 0; }
    rx_omega_fill(seed, A, x, M, N);
    omega_matvec_reference(A, x, y, M, N);
    uint64_t result = rx_omega_digest(y, M);
    free(A); free(x); free(y);
    return result;
}

/* One ordinary production request; the harness publishes it and reads the
 * result, and never touches a faculty. */
static int serve(Rig *r) {
    uint64_t seq = ++r->request_seq;
    uint64_t seed = seq * 0x2545F4914F6CDD1Dull;
    RxMutation m[4] = {{r->omega.o.request, 0, seq}, {r->omega.o.request, 1, M},
                       {r->omega.o.request, 2, N}, {r->omega.o.request, 3, seed}};
    if (rx_world_publish_external(&r->w, r->ext_request, m, 4) <= 0) return -1;
    uint64_t t = now_ns();
    while (field(r, r->omega.o.result, 0) != seq) {
        if (now_ns() - t > 10000000000ull) return -1;
        spin_us(5);
    }
    if (field(r, r->omega.o.result, 1) != expected_digest(seed)) r->wrong++;
    uint64_t used = field(r, r->omega.o.result, 2);
    if (used != 0 && (field(r, r->living.o.inforce, 0) == 0 ||
                      field(r, r->living.o.inforce, 1) != used))
        r->unpromoted_use++;
    __atomic_add_fetch(&r->served, 1, __ATOMIC_RELAXED);
    return 0;
}

static uint64_t served(Rig *r) { return __atomic_load_n(&r->served, __ATOMIC_RELAXED); }

static void *producer_main(void *arg) {
    Rig *r = arg;
    while (!atomic_load(&r->producer_stop))
        if (serve(r) != 0) { atomic_store(&r->producer_error, 1); break; }
    return NULL;
}

static int start_producer(Rig *r) {
    if (r->producer_live) return 0;
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

/* Production continues for n more correct results. */
static int produce(Rig *r, uint64_t n, uint64_t timeout_ms) {
    uint64_t until = served(r) + n, t = now_ns();
    while (served(r) < until) {
        if (atomic_load(&r->producer_error)) return -1;
        if (now_ns() - t > timeout_ms * 1000000ull) return -1;
        pause_us(200);
    }
    return 0;
}

/* Ring transport only: accept the seat's completion notices into the world.
 * It decides nothing. Every refusal is tallied by its reason. */
static void tally(Rig *r, int rc) {
    if (rc == RX_OK) atomic_fetch_add(&r->acc_ok, 1);
    else if (rc == RX_ERR_STALE_GEN) atomic_fetch_add(&r->acc_stale, 1);
    else if (rc == RX_ERR_TORN) atomic_fetch_add(&r->acc_torn, 1);
    else if (rc == RX_ERR_AUTHORITY) atomic_fetch_add(&r->acc_auth, 1);
    else if (rc != RX_ERR_NOT_FOUND) atomic_fetch_add(&r->acc_other, 1);
}

static void progress(Rig *r) {
#ifdef R14_SILICON
    for (int i = 0; i < 64; i++) {
        int rc = rx_resident_accept(&r->w);
        if (rc == RX_ERR_NOT_FOUND) break;
        tally(r, rc);
    }
#else
    for (int i = 0; i < 64; i++) {
        int rc = rx_resident_seat_step(&r->w);
        if (rc == 0) break;
        if (rc == 1) tally(r, rx_resident_accept(&r->w));
        else if (rc < 0) tally(r, rx_resident_accept(&r->w));
        else break;
    }
#endif
}

static void *acceptor_main(void *arg) {
    Rig *r = arg;
    while (!atomic_load(&r->acceptor_stop)) {
        if (!atomic_load(&r->acceptor_pause)) progress(r);
        pause_us(200);
    }
    return NULL;
}

static int start_acceptor(Rig *r) {
    if (r->acceptor_live) return 0;
    atomic_store(&r->acceptor_stop, 0);
    if (pthread_create(&r->acceptor, NULL, acceptor_main, r) != 0) return -1;
    r->acceptor_live = 1;
    return 0;
}

static void stop_acceptor(Rig *r) {
    if (!r->acceptor_live) return;
    atomic_store(&r->acceptor_stop, 1);
    pthread_join(r->acceptor, NULL);
    r->acceptor_live = 0;
}

static int publish_placement(Rig *r, uint64_t cls) {
    RxMutation m[2] = {{r->aien.o.placement, 0, field(r, r->aien.o.placement, 0) + 1},
                       {r->aien.o.placement, 1, cls}};
    return rx_world_publish_external(&r->w, r->ext_placement, m, 2) > 0 ? 0 : -1;
}

static int publish_goal(Rig *r, uint64_t target) {
    RxMutation m[3] = {{r->aien.o.goal, 0, field(r, r->aien.o.goal, 0) + 1},
                       {r->aien.o.goal, 1, rx_omega_regime(M, N)}, {r->aien.o.goal, 2, target}};
    return rx_world_publish_external(&r->w, r->ext_goal, m, 3) > 0 ? 0 : -1;
}

/* The body started: the one outside event generation.restore answers. */
static int64_t publish_boot(Rig *r) {
    RxMutation m = {r->living.o.restore, 0, field(r, r->living.o.restore, 0) + 1};
    return rx_world_publish_external(&r->w, r->ext_restore, &m, 1);
}

static void stop(Rig *r) {
    stop_producer(r);
    stop_acceptor(r);
#ifdef R14_SILICON
    if (r->seat) {
        (void)rx_resident_shutdown(&r->w);
        (void)rx_gpu_seat_finish(r->seat);
        r->seat = NULL;
    }
#endif
    rx_world_destroy(&r->w);
    rx_aegis_destroy(&r->aegis);
    rx_omega_destroy(&r->omega);
    rx_gen_close(r->gen);
    aienos_cap_stop(r->admin, r->view);
    if (r->own_dir) {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", r->generation_dir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", r->generation_dir);
    }
}

/* ---- audits ---------------------------------------------------------------- */

/* Every capability the authority holds. Only the promotion subject holds the
 * promotion right; no faculty, lane, seat, experiment, hostile or outside
 * subject holds a privileged right; nothing the harness gave a hostile party
 * is still live. */
static int authority_sweep(Rig *r, uint64_t *swept, uint64_t *holders, uint64_t *unexpected) {
    const uint32_t unprivileged[] = {RX_AIEN_SUBJ, RX_OMEGA_SUBJ_SERVE, RX_OMEGA_SUBJ_OMEGA,
        RX_LIVING_SUBJ, RX_LIVING_SEAT_SUBJ, RX_LIVING_PREPARE_SUBJ, RX_AEGIS_SUBJ, EXTERNAL,
        LANE_SUBJ, ROGUE_SUBJ, STUFFER_SUBJ, CYCLE_SUBJ};
    AienosCapRef office;
    if (aienos_cap_office(r->admin, &office) != 0) return -4;
    *swept = 0; *holders = 0; *unexpected = 0;
    for (uint32_t id = 1; id < 2048; id++)
        for (uint32_t gen = office.generation; gen < office.generation + 8; gen++) {
            AienosCapEntry e;
            if (aienos_cap_inspect(r->view, (AienosCapRef){id, gen}, &e) != 0) continue;
            if (e.state != 1u) continue;          /* AIENOS_CAP_STATE_LIVE */
            (*swept)++;
            if ((e.rights & RX_RIGHT_PROMOTE) || e.resource == RX_GEN_RES_PROMOTION) {
                if (e.subject != RX_LIVING_PROMOTE_SUBJ || e.rights != RX_GEN_RIGHT_PROMOTE)
                    (*unexpected)++;
                else (*holders)++;
            }
            for (uint32_t i = 0; i < sizeof unprivileged / sizeof unprivileged[0]; i++)
                if (e.subject == unprivileged[i] && (e.rights & RX_RIGHT_PRIVILEGED))
                    (*unexpected)++;
            if (e.subject == RX_LIVING_PROMOTE_SUBJ &&
                (e.rights & RX_RIGHT_PRIVILEGED & ~RX_RIGHT_PROMOTE))
                (*unexpected)++;
        }
    for (uint32_t i = 0; i < r->n_hostile; i++) {
        AienosCapEntry e;
        if (aienos_cap_inspect(r->view, (AienosCapRef){r->hostile[i].cap_id,
                r->hostile[i].generation}, &e) == 0 && e.state == 1u)
            (*unexpected)++;
    }
    return *unexpected == 0 && *holders == 1 ? 0 : -1;
}

/* No claim outstanding, no charge held, every claim ended exactly once. */
typedef struct {
    uint64_t claims, closed, failed_seat, committed_seat, inflight, used_slots, used_memory;
    uint64_t holding, blocked_peak;
    int ok;
} Accounting;

static void account(Rig *r, Accounting *a) {
    memset(a, 0, sizeof *a);
    pthread_mutex_lock(&r->w.mu);
    a->claims = r->w.stats.resident_claims;
    a->closed = r->w.stats.resident_closed;
    a->used_slots = r->w.used_slots;
    a->used_memory = r->w.used_memory;
    a->blocked_peak = r->w.peak_blocked;
    for (uint32_t i = 0; i < r->w.n_reactions; i++) {
        const RxReaction *x = &r->w.reactions[i];
        if (x->resident_seat) a->inflight++;
        if (x->holding) a->holding++;
    }
    for (uint64_t id = 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = &r->w.crumbs[id - 1];
        if (k->worker != RX_SEAT_BLACKWELL) continue;
        if (k->kind == RX_CRUMB_COMMIT) a->committed_seat++;
        else a->failed_seat++;
    }
    pthread_mutex_unlock(&r->w.mu);
    a->ok = a->claims == a->closed && a->claims == a->committed_seat + a->failed_seat &&
            a->inflight == 0 && a->used_slots == 0 && a->used_memory == 0 && a->holding == 0;
}

/* Is `wanted` a causal ancestor of (or equal to) `node`? */
static uint8_t *g_seen;
static uint64_t *g_stack;
#define CRUMBS (1u << 18)
/* The visited set and stack grow with the causal log. */
static uint64_t g_seen_cap;
static int seen_fit(uint64_t n) {
    if (n + 1 <= g_seen_cap) return 0;
    uint64_t cap = g_seen_cap ? g_seen_cap : 1024;
    while (cap < n + 1) cap *= 2;
    uint8_t *s = realloc(g_seen, (size_t)cap);
    if (!s) return -1;
    g_seen = s;
    uint64_t *k = realloc(g_stack, sizeof(uint64_t) * (size_t)cap);
    if (!k) return -1;
    g_stack = k;
    g_seen_cap = cap;
    return 0;
}
static int ancestor(RxWorld *w, uint64_t node, uint64_t wanted) {
    if (!node || !wanted || wanted > node || node > w->n_crumbs) return 0;
    if (node == wanted) return 1;
    if (seen_fit(w->n_crumbs) != 0) return 0;
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

static uint64_t crumbs_of(Rig *r, uint32_t reaction, RxCrumbKind kind, int reason,
                          uint64_t after, uint64_t *last) {
    uint64_t n = 0;
    for (uint64_t id = after + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&r->w, id);
        if (k->reaction != reaction || k->kind != kind) continue;
        if (reason && k->reason != reason) continue;
        n++;
        if (last) *last = id;
    }
    return n;
}

/* ---- the living body -------------------------------------------------------
 * R13's episode: production confirms the incumbent on the A725 cores, the
 * body moves to the X925 cores, a human goal arrives, and the organism adapts
 * by itself (plan, search, GPU experiment, belief, selection, R9 promotion).
 * Production runs on its own thread from the goal on and keeps running. */
typedef struct {
    uint64_t goal_crumb, served_at_goal, served_at_promotion, gen_before, gen_after;
    uint64_t promotion_crumb;
    int promoted;
} Episode;

static int adapt(Rig *r, Episode *ep) {
    memset(ep, 0, sizeof *ep);
    cpu_set_t a, x;
    if (core_sets(&a, &x) != 0 || move_threads(&a) <= 0) return -1;
    if (field(r, r->aien.o.placement, 1) != CLASS_A725 && publish_placement(r, CLASS_A725) != 0)
        return -2;
    if (start_acceptor(r) != 0) return -3;
    uint64_t limit = now_ns() + 60000000000ull;
    while ((field(r, r->omega.o.selection, 0) < 1 ||
            field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED) && now_ns() < limit)
        if (serve(r) != 0) return -4;
    if (field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED) return -5;
    uint64_t target = field(r, r->aien.o.prediction, 5) * TARGET_PCT / 100u;
    uint64_t lineage = 0;
    rx_gen_active(r->gen, &ep->gen_before, &lineage);
    if (start_producer(r) != 0) return -6;
    if (move_threads(&x) <= 0 || publish_placement(r, CLASS_X925) != 0) return -7;
    ep->served_at_goal = served(r);
    if (publish_goal(r, target) != 0) return -8;
    ep->goal_crumb = rx_world_explain(&r->w, r->aien.o.goal, 0);
    limit = now_ns() + 90000000000ull;
    while (now_ns() < limit && field(r, r->living.o.promotion, 0) == 0) {
        if (atomic_load(&r->producer_error)) return -9;
        pause_us(200);
    }
    ep->served_at_promotion = served(r);
    ep->promoted = field(r, r->living.o.promotion, 0) != 0 && r->promoter.result == RX_GEN_OK;
    rx_gen_active(r->gen, &ep->gen_after, &lineage);
    ep->promotion_crumb = rx_world_explain(&r->w, r->living.o.promotion, 0);
    if (!ep->promoted) return -10;
    uint64_t epoch = field(r, r->living.o.inforce, 0);
    limit = now_ns() + 60000000000ull;
    while (now_ns() < limit && !atomic_load(&r->producer_error) &&
           !(field(r, r->aien.o.prediction, 1) == epoch &&
             field(r, r->aien.o.prediction, 6) == RX_AIEN_PRED_CONFIRMED &&
             field(r, r->aien.o.assessment, 4) == RX_AIEN_GOAL_MET))
        pause_us(200);
    return field(r, r->aien.o.assessment, 4) == RX_AIEN_GOAL_MET ? 0 : -11;
}

/* ---- lane driver: an outside party with lane requests ----------------------
 * Closed loop per lane: the next request waits for the check of the last one,
 * so every request is a distinct, countable piece of work. */
typedef struct {
    Rig *r;
    pthread_t t;
    atomic_int stop, pause;
    uint64_t quota;             /* per lane; 0 = unbounded */
    unsigned gap_us;
    int live;
    atomic_int error;
} Driver;

static void *driver_main(void *arg) {
    Driver *d = arg;
    Rig *r = d->r;
    while (!atomic_load(&d->stop)) {
        if (!atomic_load(&d->pause))
            for (uint32_t k = 0; k < r->opt.lanes; k++) {
                if (d->quota && r->lane_sent[k] >= d->quota) continue;
                if (lane_done(r, k) != r->lane_sent[k]) continue;
                if (lane_submit(r, k) != 0) atomic_store(&d->error, 1);
            }
        pause_us((int)d->gap_us);
    }
    return NULL;
}

static int driver_start(Driver *d, Rig *r, uint64_t quota, unsigned gap_us) {
    memset(d, 0, sizeof *d);
    d->r = r;
    d->quota = quota;
    d->gap_us = gap_us;
    if (pthread_create(&d->t, NULL, driver_main, d) != 0) return -1;
    d->live = 1;
    return 0;
}

static void driver_stop(Driver *d) {
    if (!d->live) return;
    atomic_store(&d->stop, 1);
    pthread_join(d->t, NULL);
    d->live = 0;
}

static uint64_t lanes_sent(Rig *r) {
    uint64_t n = 0;
    for (uint32_t k = 0; k < r->opt.lanes; k++) n += r->lane_sent[k];
    return n;
}

static uint64_t lanes_done(Rig *r) {
    uint64_t n = 0;
    for (uint32_t k = 0; k < r->opt.lanes; k++) n += lane_done(r, k);
    return n;
}

/* Every lane: each request published exactly once (a seat commit that changed
 * the output), checked once, no mismatch. Seat commits that re-published an
 * unchanged value (a slot wake with the same operands) are counted apart. */
typedef struct { uint64_t sent, published, rewakes, checked, mismatches, lost, duplicates; } LaneTotals;
static void lane_totals(Rig *r, LaneTotals *t) {
    memset(t, 0, sizeof *t);
    for (uint32_t k = 0; k < r->opt.lanes; k++) {
        uint64_t pub = 0, same = 0;
        for (uint64_t id = 1; id <= r->w.n_crumbs; id++) {
            const RxCrumb *c = rx_world_crumb(&r->w, id);
            if (c->reaction != r->r_lane[k] || c->kind != RX_CRUMB_COMMIT) continue;
            if (c->n_outputs) pub++; else same++;
        }
        t->sent += r->lane_sent[k];
        t->published += pub;
        t->rewakes += same;
        t->checked += field(r, r->lchk[k], 0);
        t->mismatches += field(r, r->lchk[k], 2);
        if (pub < r->lane_sent[k]) t->lost += r->lane_sent[k] - pub;
        if (pub > r->lane_sent[k]) t->duplicates += pub - r->lane_sent[k];
        if (lane_done(r, k) != r->lane_sent[k]) t->lost++;
    }
}

/* ---- per-scenario result ---------------------------------------------------- */
typedef struct {
    int ran, pass;
    int fault_observed;
    char containment[200], recovery[200];
    uint64_t prod_before, prod_during, prod_after;
    uint64_t crumbs;
    int crumbs_ok;
    int sweep_ok;
    uint64_t swept, holders, unexpected;
    Accounting acc;
    uint64_t gen_before, gen_after;
    char extra[16384];
} Result;

static Result g_res[6];
static const char *g_names[6] = {"A_corrupt_candidate", "B_forged_capability",
    "C_reaction_cycle", "D_gpu_saturation", "E_gpu_seat_kill", "F_checkpoint_crash"};

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; \
        fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void extra(Result *res, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void extra(Result *res, const char *fmt, ...) {
    size_t n = strlen(res->extra);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->extra + n, sizeof res->extra - n, fmt, ap);
    va_end(ap);
}

/* Production continues for `after` more results, then the body settles and
 * is audited: causal crumbs verify, every lifecycle transition was legal, no
 * production result was wrong or used a realization not in force, the
 * authority sweep is clean, every claim ended once and no charge is held. */
static int finish(Rig *r, Result *res, uint64_t after) {
    uint64_t base = served(r);
    int ok = start_producer(r) == 0 && produce(r, after, 60000) == 0;
    res->prod_after = served(r) - base;
    CHECK(ok, "production did not resume (%llu of %llu)", U(res->prod_after), U(after));
    stop_producer(r);
    CHECK(rx_world_wait_quiescent(&r->w, 20000) == RX_OK, "the body did not settle");
    res->crumbs_ok = rx_world_verify_crumbs(&r->w, &res->crumbs) == 0;
    CHECK(res->crumbs_ok, "crumbs do not verify");
    CHECK(r->w.stats.crumb_overflow == 0, "the crumb log overflowed");
    CHECK(r->w.stats.illegal_transitions == 0, "illegal lifecycle transition");
    CHECK(r->wrong == 0, "%llu wrong production results", U(r->wrong));
    CHECK(r->unpromoted_use == 0, "production used a realization not in force");
    CHECK(atomic_load(&r->acc_other) == 0, "the transport saw %llu unexplained refusals",
          U(atomic_load(&r->acc_other)));
    res->sweep_ok = authority_sweep(r, &res->swept, &res->holders, &res->unexpected) == 0;
    CHECK(res->sweep_ok, "authority sweep: %llu promotion holders, %llu unexpected",
          U(res->holders), U(res->unexpected));
    account(r, &res->acc);
    CHECK(res->acc.ok, "accounting: claims %llu closed %llu committed %llu failed %llu "
          "in flight %llu slots %llu memory %llu holding %llu", U(res->acc.claims),
          U(res->acc.closed), U(res->acc.committed_seat), U(res->acc.failed_seat),
          U(res->acc.inflight), U(res->acc.used_slots), U(res->acc.used_memory),
          U(res->acc.holding));
    uint64_t lineage = 0;
    rx_gen_active(r->gen, &res->gen_after, &lineage);
    return 0;
}

static Opts opts(void) {
    Opts o;
    memset(&o, 0, sizeof o);
#ifdef R14_SILICON
    o.seat = 1;
#endif
    return o;
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
}

/* ---- A: corrupt candidate realization ---------------------------------------
 * Omega's own test defects (R10): slot 4 carries unroll4 bytes that were
 * changed after their identity was computed (TAMPER), that drop N mod 4
 * (semantically wrong), or that load from address zero (crash). The valid
 * quad4 candidate is synthesized beside it in the same epoch. */
static int scenario_a_case(int defect, uint32_t want, const char *label, Result *res, int first) {
    Rig *r = calloc(1, sizeof *r);
    if (!r) return -1;
    Opts o = opts();
    o.defect = defect;
    int rc = start(r, &o);
    CHECK(rc == 0, "A %s: setup failed at stage %d", label, g_stage);
    Episode ep;
    if (rc == 0) {
        rc = adapt(r, &ep);
        CHECK(rc == 0, "A %s: the organism did not adapt (%d, refusal %d, promotion %d)", label,
              rc, r->living.last_refusal, r->promoter.result);
    }
    if (rc != 0) { if (r->admin) stop(r); free(r); return -1; }
    uint32_t bad = RX_OMEGA_SLOTS - 1;
    uint64_t bad_id = field(r, r->omega.o.candidate[bad], 2);
    uint64_t verdict = field(r, r->omega.o.verdict[bad], 1);
    uint64_t why = field(r, r->omega.o.verdict[bad], 5);
    int observed = bad_id != 0 && verdict == RX_OMEGA_REFUSED && why == want;
    CHECK(observed, "A %s: verdict %llu reason %llu (want refused, %u)", label, U(verdict),
          U(why), want);
    RxOmegaRealization e;
    int have = rx_omega_store_find(&r->omega, bad_id, &e) == 0;
    CHECK(have && !e.verified && e.parent_runs == 0,
          "A %s: the refused bytes are verified or ran in the parent", label);
    CHECK(field(r, r->omega.o.selection, 1) != bad_id && field(r, r->living.o.candidate, 2) != bad_id &&
              field(r, r->living.o.inforce, 1) != bad_id,
          "A %s: the refused realization was selected, proposed or put in force", label);
    /* The active R9 generation holds the valid realization, bytes and identity. */
    uint8_t code[AARCH64_MAX_CODE_BYTES];
    size_t n = 0;
    SemanticId id;
    RxOmegaRealization good;
    int durable = rx_gen_read_blob(r->gen, ep.gen_after, "realization", code, sizeof code, &n) ==
                      RX_GEN_OK &&
                  rx_omega_identity_of(&r->omega, code, n, &id) == 0 &&
                  rx_omega_store_find(&r->omega, field(r, r->living.o.inforce, 1), &good) == 0 &&
                  memcmp(id.bytes, good.id.bytes, 32) == 0 && good.verified &&
                  good.kind == OMEGA_MATVEC_KIND_QUAD4;
    uint64_t durable_w0 = 0;
    for (uint32_t j = 0; j < 8; j++) durable_w0 |= (uint64_t)id.bytes[j] << (8 * j);
    CHECK(durable && durable_w0 != bad_id, "A %s: the active generation is not the verified quad4",
          label);
    /* The refusal is attributable: the verdict was written by verify.4, and its
     * ancestry reaches the synthesis that produced the bytes. */
    uint64_t kv = rx_world_explain(&r->w, r->omega.o.verdict[bad], 1);
    uint64_t ks = rx_world_explain(&r->w, r->omega.o.candidate[bad], 2);
    const RxCrumb *cv = rx_world_crumb(&r->w, kv), *cs = rx_world_crumb(&r->w, ks);
    int attributed = cv && cs && cv->reaction == r->omega.r_verify[bad] &&
                     cs->reaction == r->omega.r_synth[bad] && ancestor(&r->w, kv, ks);
    CHECK(attributed, "A %s: the refusal is not attributable", label);
    /* The store's metadata still tells the truth: every verified entry's bytes
     * hash to its identity; the refused one is not verified. */
    uint32_t verified = 0, entries = 0, lies = 0;
    pthread_mutex_lock(&r->omega.mu);
    entries = r->omega.n_store;
    for (uint32_t i = 0; i < r->omega.n_store; i++) {
        if (!r->omega.store[i].verified) continue;
        verified++;
        SemanticId again;
        if (rx_omega_identity_of(&r->omega, r->omega.store[i].code, r->omega.store[i].code_len,
                                 &again) != 0 ||
            memcmp(again.bytes, r->omega.store[i].id.bytes, 32) != 0) lies++;
    }
    pthread_mutex_unlock(&r->omega.mu);
    CHECK(lies == 0, "A %s: %u verified store entries do not match their bytes", label, lies);
    uint64_t during = ep.served_at_promotion - ep.served_at_goal;
    CHECK(during > 0, "A %s: production stopped while the candidate was refused", label);
    Result sub;
    memset(&sub, 0, sizeof sub);
    finish(r, &sub, 100);
    if (first) {
        *res = sub;
        res->prod_before = ep.served_at_goal;
        res->prod_during = during;
        res->gen_before = ep.gen_before;
        res->fault_observed = 1;
        res->extra[0] = 0;
    } else {
        res->prod_after += sub.prod_after;
        res->crumbs += sub.crumbs;
        res->crumbs_ok &= sub.crumbs_ok;
        res->sweep_ok &= sub.sweep_ok;
        res->acc.ok &= sub.acc.ok;
        res->unexpected += sub.unexpected;
    }
    res->fault_observed &= observed;
    char idhex[65];
    hex(id.bytes, 32, idhex);
    extra(res, "%s{\"defect\": \"%s\", \"refusal_reason\": %llu, \"refused_id_word0\": \"0x%016llx\", "
          "\"refused_verified\": %s, \"refused_parent_runs\": %llu, "
          "\"store_entries\": %u, \"store_verified\": %u, \"store_mismatches\": %u, "
          "\"promoted_generation\": %llu, \"promoted_identity\": \"%s\", "
          "\"refusal_attributed\": %s, \"production_during\": %llu, \"production_after\": %llu}",
          first ? "" : ", ", label, U(why), U(bad_id), have && e.verified ? "true" : "false",
          U(have ? e.parent_runs : 0), entries, verified, lies, U(ep.gen_after), idhex,
          attributed ? "true" : "false", U(during), U(sub.prod_after));
    printf("R14 A %s: verdict REFUSED reason %llu; generation %llu -> %llu holds verified quad4; "
           "%llu served during, %llu after\n", label, U(why), U(ep.gen_before), U(ep.gen_after),
           U(during), U(sub.prod_after));
    stop(r);
    free(r);
    return observed && durable && attributed && lies == 0 ? 0 : -1;
}

static void scenario_a(void) {
    Result *res = &g_res[0];
    int fails = g_fail;
    int rc = 0;
    rc |= scenario_a_case(RX_OMEGA_DEFECT_TAMPER, RX_OMEGA_WHY_IDENTITY, "tamper", res, 1);
    rc |= scenario_a_case(RX_OMEGA_DEFECT_SKIP_REMAINDER, RX_OMEGA_WHY_DIFFERENTIAL,
                          "semantic", res, 0);
    rc |= scenario_a_case(RX_OMEGA_DEFECT_CRASH, RX_OMEGA_WHY_CRASHED, "crash", res, 0);
    char cases[sizeof res->extra];
    memcpy(cases, res->extra, sizeof cases);
    cases[sizeof cases - 16] = 0;
    res->extra[0] = 0;
    extra(res, "\"cases\": [%s]", cases);
    snprintf(res->containment, sizeof res->containment,
             "R10 verifier refused each corrupt realization (identity, differential, crash)");
    snprintf(res->recovery, sizeof res->recovery,
             "none needed: the valid quad4 candidate was verified, selected and promoted by R9");
    res->ran = 1;
    res->pass = rc == 0 && g_fail == fails;
}

/* ---- B: forged authority while the organism serves ---------------------- */
typedef struct { RxObjRef target; uint32_t field; uint64_t value; } Poke;
static int fn_poke(RxCtx *c) {
    Poke *p = c->user;
    c->out[c->n_out++] = (RxMutation){p->target, p->field, p->value};
    return 0;
}

/* rogue.decide: a reaction wrongly holding WRITE on the lane client's decision
 * overwrites it with a GRANT echoing the request (R8's jam). */
typedef struct { RxObjRef decision, request; } Forge;
static int fn_forge(RxCtx *c) {
    Forge *f = c->user;
    const RxSnapshotDep *rq = snapshot(c, f->request);
    if (!rq || rq->field[0] == 0) return 0;
    uint64_t v[8] = {rq->field[0], RX_AEGIS_GRANT, rq->field[1], rq->field[2], 0, rq->field[4],
                     1, rq->field[5]};
    for (uint32_t i = 0; i < 8; i++) c->out[c->n_out++] = (RxMutation){f->decision, i, v[i]};
    return 0;
}

static int rogue_add(Rig *r, const char *name, RxObjRef trig, RxObjRef write, uint32_t wfield,
                     RxCapRef read_cap, uint64_t read_res, RxCapRef write_cap, uint64_t write_res,
                     Poke *p, uint32_t *id) {
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = ROGUE_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_poke;
    d.user = p;
    *p = (Poke){write, wfield, 0xBADBADull};
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){trig, RX_FIELD(0)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){write, RX_FIELD(wfield)};
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){read_cap, read_res, RX_RIGHT_READ};
    d.caps[1] = (RxCapNeed){write_cap, write_res, RX_RIGHT_READ | RX_RIGHT_WRITE};
    return rx_world_add_reaction(&r->w, &d, id);
}

/* Wait until reaction `id` has run at least `n` times and is not in flight. */
static int ran(Rig *r, uint32_t id, uint64_t n, int ms) {
    uint64_t t = now_ns();
    while (now_ns() - t < (uint64_t)ms * 1000000ull) {
        pthread_mutex_lock(&r->w.mu);
        RxState s = r->w.reactions[id].state;
        uint64_t a = r->w.reactions[id].activations;
        pthread_mutex_unlock(&r->w.mu);
        if (a >= n && s == RX_DORMANT) return 0;
        pause_us(500);
    }
    return -1;
}

static int wait_lanes(Rig *r, uint64_t done, int ms) {
    uint64_t t = now_ns();
    while (lanes_done(r) < done)
        if (now_ns() - t > (uint64_t)ms * 1000000ull) return -1;
        else pause_us(500);
    return 0;
}

static int crumb_has_cap(const RxCrumb *k, RxCapRef c) {
    for (uint32_t i = 0; i < k->n_caps; i++)
        if (k->caps[i].cap_id == c.cap_id && k->caps[i].generation == c.generation) return 1;
    return 0;
}

static int b_native_promotion(void *ctx, uint32_t cap_id, uint32_t generation, uint32_t subject,
                              uint64_t resource, uint32_t rights) {
    AienosCapEntry entry;
    return aienos_cap_validate(ctx, (AienosCapRef){cap_id, generation}, subject, resource, rights,
                               &entry);
}

static void scenario_b(void) {
    Result *res = &g_res[1];
    int fails = g_fail;
    Rig *r = calloc(1, sizeof *r);
    if (!r) return;
    Opts o = opts();
    o.lanes = LANES;
    int rc = start(r, &o);
    CHECK(rc == 0, "B: setup failed at stage %d", g_stage);
    Episode ep;
    if (rc == 0) {
        rc = adapt(r, &ep);
        CHECK(rc == 0, "B: the organism did not adapt (%d)", rc);
    }
    if (rc != 0) { if (r->admin) stop(r); free(r); return; }
    Driver dv;
    CHECK(driver_start(&dv, r, 0, 300) == 0, "B: lane driver");
    CHECK(wait_lanes(r, 3 * LANES, 30000) == 0, "B: lanes never produced");
    res->prod_before = served(r);
    uint64_t lanes_before = lanes_done(r);
    uint64_t mints0 = r->aegis.mints, gen0 = 0, lin0 = 0;
    rx_gen_active(r->gen, &gen0, &lin0);
    res->gen_before = gen0;
    uint64_t mark = r->w.n_crumbs;
    uint64_t inforce_before[RX_MAX_FIELDS];
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++) inforce_before[i] = field(r, r->living.o.inforce, i);

    /* The outside party that pokes the rogue, and what the rogue holds. */
    RxObjRef trig, scratch, rin;
    CHECK(new_object(r, 0x6150, RES_ROGUE, &trig) == RX_OK &&
              new_object(r, 0x6151, RES_ROGUE + 2, &scratch) == RX_OK &&
              new_object(r, 0x6152, RES_ROGUE + 1, &rin) == RX_OK &&
              rx_world_attach_physical(&r->w, rin) == RX_OK, "B: rogue objects");
    RxCapRef ext_trig = mint(r, EXTERNAL, RES_ROGUE, RX_RIGHT_WRITE);
    RxCapRef ext_rin = mint(r, EXTERNAL, RES_ROGUE + 1, RX_RIGHT_WRITE);
    RxCapRef rogue_read = hostile(r, ROGUE_SUBJ, RES_ROGUE, RX_RIGHT_READ);
    RxCapRef rin_read = hostile(r, ROGUE_SUBJ, RES_ROGUE + 1, RX_RIGHT_READ);
    RxCapRef scratch_rw = hostile(r, ROGUE_SUBJ, RES_ROGUE + 2, RX_RIGHT_READ | RX_RIGHT_WRITE);
    const RxCapRef forged = {777, 3}, forged_gpu = {778, 1}, forged_promote = {779, 1};
    const RxCapRef stale = {scratch_rw.cap_id, scratch_rw.generation + 1};
    uint64_t inforce_res = RX_LIVING_RES_BASE + RX_LIVING_RES_INFORCE;
    static Poke p_forged, p_borrowed, p_revoked, p_stale, p_gpu;
    uint32_t r_forged, r_borrowed, r_revoked, r_stale, r_gpu;
    CHECK(rogue_add(r, "rogue.forged", trig, r->living.o.inforce, 1, rogue_read, RES_ROGUE,
                    forged, inforce_res, &p_forged, &r_forged) == RX_OK &&
          rogue_add(r, "rogue.borrowed", trig, r->living.o.inforce, 1, rogue_read, RES_ROGUE,
                    r->promoter.inforce_write, inforce_res, &p_borrowed, &r_borrowed) == RX_OK &&
          rogue_add(r, "rogue.revoked", trig, scratch, 0, rogue_read, RES_ROGUE, scratch_rw,
                    RES_ROGUE + 2, &p_revoked, &r_revoked) == RX_OK &&
          rogue_add(r, "rogue.stale", trig, scratch, 1, rogue_read, RES_ROGUE, stale,
                    RES_ROGUE + 2, &p_stale, &r_stale) == RX_OK, "B: rogue reactions");
    /* A rogue seat reaction: a GPU claim on lane 0's output with a forged grant. */
    CHECK(rx_world_bind_capability(&r->w, rin, rin_read) == RX_OK, "B: rogue input bind");
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "rogue.blackwell";
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = ROGUE_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_lane_seat;
    d.user = &p_gpu;
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.need.memory_bytes = LANE_BYTES;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){rin, RX_FIELD(0) | RX_FIELD(1)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){r->lout[0], RX_FIELD(0)};
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){rin_read, RES_ROGUE + 1, RX_RIGHT_READ};
    d.caps[1] = (RxCapNeed){forged_gpu, RES_LANE_OUT, RX_RIGHT_WRITE};
    CHECK(rx_world_add_reaction(&r->w, &d, &r_gpu) == RX_OK, "B: rogue seat");

    uint64_t t0 = served(r);
    /* B1-B3 fire together; rogue.revoked is still legitimate the first time. */
    RxMutation poke = {trig, 0, 1};
    CHECK(rx_world_publish_external(&r->w, ext_trig, &poke, 1) > 0, "B: poke");
    RxMutation gpu_poke[2] = {{rin, 0, 5}, {rin, 1, 6}};
    CHECK(rx_world_publish_external(&r->w, ext_rin, gpu_poke, 2) > 0, "B: gpu poke");
    CHECK(ran(r, r_forged, 1, 5000) == 0 && ran(r, r_borrowed, 1, 5000) == 0 &&
              ran(r, r_revoked, 1, 5000) == 0 && ran(r, r_stale, 1, 5000) == 0 &&
              ran(r, r_gpu, 1, 5000) == 0, "B: rogues did not run");
    CHECK(field(r, scratch, 0) == 0xBADBADull, "B: the legitimately held grant did not work");
    CHECK(cap_revoke(r, scratch_rw) == 0, "B: revoke");
    poke.value = 2;
    CHECK(rx_world_publish_external(&r->w, ext_trig, &poke, 1) > 0, "B: poke 2");
    CHECK(ran(r, r_revoked, 2, 5000) == 0, "B: revoked rogue did not run again");
    uint64_t blocked[5] = {0}, commits[5] = {0};
    uint32_t rogues[5] = {r_forged, r_borrowed, r_revoked, r_stale, r_gpu};
    uint64_t kblock = 0;
    for (int i = 0; i < 5; i++) {
        blocked[i] = crumbs_of(r, rogues[i], RX_CRUMB_BLOCKED_AUTHORITY, 0, mark, &kblock);
        commits[i] = r->w.reactions[rogues[i]].commits;
    }
    const RxCrumb *kf = NULL;
    for (uint64_t id = mark + 1; id <= r->w.n_crumbs && !kf; id++) {
        const RxCrumb *k = rx_world_crumb(&r->w, id);
        if (k->reaction == r_forged && k->kind == RX_CRUMB_BLOCKED_AUTHORITY) kf = k;
    }
    uint64_t rogue_claims = 0;
    for (uint64_t id = mark + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&r->w, id);
        if (k->reaction == r_gpu && k->worker == RX_SEAT_BLACKWELL) rogue_claims++;
    }
    int b13 = blocked[0] && blocked[1] && blocked[2] && blocked[3] && blocked[4] &&
              !commits[0] && !commits[1] && commits[2] == 1 && !commits[3] && !commits[4] &&
              rogue_claims == 0 && kf && crumb_has_cap(kf, forged);
    for (uint32_t i = 0; i < RX_MAX_FIELDS; i++)
        if (field(r, r->living.o.inforce, i) != inforce_before[i]) b13 = 0;
    CHECK(b13, "B1-3: blocked %llu/%llu/%llu/%llu/%llu commits %llu/%llu/%llu/%llu/%llu claims %llu",
          U(blocked[0]), U(blocked[1]), U(blocked[2]), U(blocked[3]), U(blocked[4]),
          U(commits[0]), U(commits[1]), U(commits[2]), U(commits[3]), U(commits[4]),
          U(rogue_claims));

    /* B4: an outside writer wrongly holding WRITE on the lanes' slot stuffs it
     * with another subject's valid grant. */
    RxObjRef lslot = r->aegis.o[1].slot[0];
    RxCapRef slot0 = {(uint32_t)field(r, lslot, 0), (uint32_t)field(r, lslot, 1)};
    RxCapRef stuffer = hostile(r, EXTERNAL, rx_aegis_res(1, RX_AEGIS_RES_SLOT0), RX_RIGHT_WRITE);
    RxCapRef other = hostile(r, ROGUE_SUBJ, RES_LANE_OUT, RX_RIGHT_READ | RX_RIGHT_WRITE);
    uint64_t mark4 = r->w.n_crumbs, claims4 = r->w.stats.resident_claims;
    RxMutation st[3] = {{lslot, 0, other.cap_id}, {lslot, 1, other.generation},
                        {lslot, 2, RX_AEGIS_SLOT_LIVE}};
    CHECK(rx_world_publish_external(&r->w, stuffer, st, 3) > 0, "B4: stuffed");
    pause_us(50000);
    uint64_t lanes_at_stuff = lanes_done(r), served_at_stuff = served(r);
    uint64_t claims_at_stuff = r->w.stats.resident_claims;
    pause_us(300000);
    uint64_t lane_blocked = 0, stuffed_claims = 0, inflight_refused = 0;
    for (uint64_t id = mark4 + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&r->w, id);
        for (uint32_t j = 0; j < r->opt.lanes; j++)
            if (k->reaction == r->r_lane[j] && k->kind == RX_CRUMB_BLOCKED_AUTHORITY) lane_blocked++;
        if (k->worker == RX_SEAT_BLACKWELL && crumb_has_cap(k, other)) {
            if (k->kind == RX_CRUMB_COMMIT) stuffed_claims++;
            else if (k->kind == RX_CRUMB_REJECTED && k->reason == RX_ERR_AUTHORITY) inflight_refused++;
        }
    }
    int b4 = lane_blocked > 0 && stuffed_claims == 0 && lanes_done(r) == lanes_at_stuff &&
             r->w.stats.resident_claims == claims_at_stuff && served(r) > served_at_stuff;
    CHECK(b4, "B4: lane refusals %llu, publications under the stuffed grant %llu, lanes %llu -> %llu, "
          "claims %llu -> %llu, production %llu -> %llu", U(lane_blocked), U(stuffed_claims),
          U(lanes_at_stuff), U(lanes_done(r)), U(claims_at_stuff), U(r->w.stats.resident_claims),
          U(served_at_stuff), U(served(r)));
    (void)claims4;

    /* B5: forged AEGIS decisions. A reaction wrongly holding WRITE on the lane
     * client's decision rewrites every decision as a GRANT; an outside writer
     * publishes one. The lane client (honestly) asks for the promotion right. */
    static Forge fg;
    fg = (Forge){r->aegis.o[1].decision, r->aegis.o[1].request};
    RxCapRef dec_w = hostile(r, ROGUE_SUBJ, rx_aegis_res(1, RX_AEGIS_RES_DECISION),
                             RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef req_r = hostile(r, ROGUE_SUBJ, rx_aegis_res(1, RX_AEGIS_RES_REQUEST), RX_RIGHT_READ);
    memset(&d, 0, sizeof d);
    d.name = "rogue.decide";
    d.faculty = RX_FACULTY_EXTERNAL;
    d.subject = ROGUE_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_forge;
    d.user = &fg;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){fg.request, RX_ALL_FIELDS};
    d.n_writes = 1;
    d.writes[0] = (RxDep){fg.decision, RX_ALL_FIELDS};
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){req_r, rx_aegis_res(1, RX_AEGIS_RES_REQUEST), RX_RIGHT_READ};
    d.caps[1] = (RxCapNeed){dec_w, rx_aegis_res(1, RX_AEGIS_RES_DECISION),
                            RX_RIGHT_READ | RX_RIGHT_WRITE};
    uint32_t r_decide;
    CHECK(rx_world_add_reaction(&r->w, &d, &r_decide) == RX_OK, "B5: rogue.decide");
    uint64_t mark5 = r->w.n_crumbs;
    RxMutation ask_promote[6] = {{r->lane_intent, 0, ++r->lane_intent_seq},
                                 {r->lane_intent, 1, RX_GEN_RES_PROMOTION},
                                 {r->lane_intent, 2, RX_GEN_RIGHT_PROMOTE},
                                 {r->lane_intent, 3, 0}, {r->lane_intent, 4, 1},
                                 {r->lane_intent, 5, RX_AEGIS_OP_ACQUIRE}};
    CHECK(rx_world_publish_external(&r->w, r->ext_lane_intent, ask_promote, 6) > 0, "B5: ask");
    CHECK(ran(r, r_decide, 1, 5000) == 0 && ran(r, r->aegis.r_install[1], 1, 5000) == 0,
          "B5: decision path did not run");
    pause_us(50000);
    RxCapRef ext_dec = hostile(r, EXTERNAL, rx_aegis_res(1, RX_AEGIS_RES_DECISION), RX_RIGHT_WRITE);
    uint64_t rs = field(r, r->aegis.o[1].request, 0);
    RxMutation fd[8] = {{fg.decision, 0, rs}, {fg.decision, 1, RX_AEGIS_GRANT},
                        {fg.decision, 2, RX_GEN_RES_PROMOTION}, {fg.decision, 3, RX_GEN_RIGHT_PROMOTE},
                        {fg.decision, 4, 0}, {fg.decision, 5, 2}, {fg.decision, 6, 1},
                        {fg.decision, 7, RX_AEGIS_OP_ACQUIRE}};
    CHECK(rx_world_publish_external(&r->w, ext_dec, fd, 8) > 0, "B5: outside decision");
    pause_us(100000);
    uint64_t root_refusals = 0;
    for (uint64_t id = mark5 + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&r->w, id);
        if (k->reaction == r->aegis.r_install[1] && k->kind == RX_CRUMB_COMMIT) root_refusals++;
    }
    int b5 = r->aegis.mints == mints0 && field(r, r->aegis.o[1].slot[1], 2) != RX_AEGIS_SLOT_LIVE &&
             field(r, r->aegis.o[1].slot[2], 2) != RX_AEGIS_SLOT_LIVE &&
             r->w.reactions[r_decide].commits > 0;
    CHECK(b5, "B5: mints %llu -> %llu, slot1 %llu slot2 %llu, rogue decisions %llu",
          U(mints0), U(r->aegis.mints), U(field(r, r->aegis.o[1].slot[1], 2)),
          U(field(r, r->aegis.o[1].slot[2], 2)), U(r->w.reactions[r_decide].commits));

    /* B6: promotion forgery straight at R9, against a draft the rogue proposed. */
    uint64_t rogue_draft = 0;
    RxGenDraft gd;
    memset(&gd, 0, sizeof gd);
    static const uint8_t junk[] = "rogue";
    gd.proofs_ok = 1;
    gd.evidence = junk; gd.evidence_len = sizeof junk - 1;
    gd.realization = junk; gd.realization_len = sizeof junk - 1;
    CHECK(rx_gen_propose(r->gen, ROGUE_SUBJ, &gd, &rogue_draft) == RX_GEN_OK, "B6: rogue draft");
    typedef struct { uint32_t subject; RxCapRef cap; } Try;
    Try tries[3] = {{RX_LIVING_PROMOTE_SUBJ, forged_promote},
                    {ROGUE_SUBJ, r->promoter.promotion_authority},
                    {ROGUE_SUBJ, forged_promote}};
    int b6 = 1;
    for (int i = 0; i < 3; i++) {
        RxPromotionRequest req = {rogue_draft, tries[i].subject, tries[i].cap.cap_id,
                                  tries[i].cap.generation, RX_GEN_RES_PROMOTION,
                                  RX_GEN_RIGHT_PROMOTE};
        int prc = rx_gen_promote(r->gen, &req, b_native_promotion, (void *)r->view,
                             NULL, NULL, NULL, NULL);
        if (prc != RX_GEN_ERR_AUTHORITY) b6 = 0;
        CHECK(prc == RX_GEN_ERR_AUTHORITY, "B6: try %d returned %d", i, prc);
    }
    uint64_t gen6 = 0, lin6 = 0;
    rx_gen_active(r->gen, &gen6, &lin6);
    RxRecoveryRecord rec;
    b6 = b6 && gen6 == gen0 && lin6 == lin0 && rx_gen_recover(r->generation_dir, &rec) == RX_GEN_OK &&
         rec.active_id == gen0;
    CHECK(b6, "B6: generation %llu -> %llu", U(gen0), U(gen6));

    /* Containment: revoke what the compromised writers held. Then the lane
     * client asks again, honestly; AEGIS grants, the root renews the slot, and
     * the lanes resume under the renewed grant. */
    uint64_t mints_attack = r->aegis.mints - mints0;
    res->prod_during = served(r) - t0;
    for (uint32_t i = 0; i < r->n_hostile; i++) (void)cap_revoke(r, r->hostile[i]);
    uint64_t lanes_blocked_at = lanes_done(r);
    CHECK(lane_acquire(r) == 0, "B: honest re-acquire");
    uint64_t t = now_ns();
    while (now_ns() - t < 10000000000ull &&
           !(field(r, lslot, 2) == RX_AEGIS_SLOT_LIVE && field(r, lslot, 0) != other.cap_id &&
             field(r, lslot, 5) == field(r, r->aegis.o[1].request, 0)))
        pause_us(500);
    RxCapRef slot1 = {(uint32_t)field(r, lslot, 0), (uint32_t)field(r, lslot, 1)};
    int renewed = field(r, lslot, 2) == RX_AEGIS_SLOT_LIVE &&
                  (slot1.cap_id != slot0.cap_id || slot1.generation != slot0.generation) &&
                  r->aegis.mints == mints0 + 1 &&
                  rx_world_validate_cap(&r->w, slot1, LANE_SUBJ, RES_LANE_OUT, RX_RIGHT_WRITE,
                                        NULL) == RX_CAP_OK &&
                  rx_world_validate_cap(&r->w, slot0, LANE_SUBJ, RES_LANE_OUT, RX_RIGHT_WRITE,
                                        NULL) != RX_CAP_OK;
    CHECK(renewed, "B: the honest grant was not renewed (state %llu, mints %llu)",
          U(field(r, lslot, 2)), U(r->aegis.mints));
    int resumed = wait_lanes(r, lanes_blocked_at + 2 * LANES, 30000) == 0;
    CHECK(resumed, "B: lanes did not resume (%llu -> %llu)", U(lanes_blocked_at), U(lanes_done(r)));
    driver_stop(&dv);
    CHECK(wait_lanes(r, lanes_sent(r), 30000) == 0, "B: lanes did not drain");
    int promote_ok = rx_world_validate_cap(&r->w, r->promoter.promotion_authority,
        RX_LIVING_PROMOTE_SUBJ, RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, NULL) == RX_CAP_OK;
    CHECK(promote_ok, "B: the legitimate promotion right no longer validates");
    res->fault_observed = b13 && b4 && b5 && b6;
    finish(r, res, 100);
    LaneTotals lt;
    lane_totals(r, &lt);
    CHECK(lt.lost == 0 && lt.duplicates == 0 && lt.mismatches == 0,
          "B: lanes lost %llu duplicated %llu mismatched %llu", U(lt.lost), U(lt.duplicates),
          U(lt.mismatches));
    snprintf(res->containment, sizeof res->containment,
             "R7 refused forged, borrowed, revoked and stale references and the rogue GPU claim; "
             "R8 root refused stuffed and forged authority; R9 refused promotion forgery");
    snprintf(res->recovery, sizeof res->recovery,
             "compromised writers revoked; the lane client re-acquired through AEGIS; the root "
             "renewed the slot; lanes resumed under the renewed grant");
    extra(res, "\"attacks\": {\"forged_reference\": \"%s\", \"wrong_subject_valid_reference\": \"%s\", "
          "\"revoked_reference\": \"%s\", \"stale_generation\": \"%s\", \"forged_gpu_claim\": \"%s\", "
          "\"slot_stuffing\": \"%s\", \"forged_aegis_decision\": \"%s\", \"promote_forgery\": \"%s\"}, "
          "\"rogue_blocked_crumbs\": [%llu, %llu, %llu, %llu, %llu], "
          "\"rogue_gpu_claims\": %llu, \"lane_refusals_while_stuffed\": %llu, "
          "\"publications_under_stuffed_grant\": %llu, \"inflight_claims_refused_at_publication\": %llu, \"root_install_runs_during_forgery\": %llu, "
          "\"mints_during_attacks\": %llu, \"renewal_mints\": %llu, "
          "\"rogue_draft_id_left_unpromoted\": %llu, \"lanes_before\": %llu, \"lanes_sent\": %llu, "
          "\"lanes_published\": %llu, \"lanes_lost\": %llu, \"lanes_duplicated\": %llu, "
          "\"legitimate_promotion_right_valid\": %s",
          b13 ? "BLOCKED" : "FAIL", b13 ? "BLOCKED" : "FAIL", b13 ? "BLOCKED" : "FAIL",
          b13 ? "BLOCKED" : "FAIL", b13 ? "BLOCKED" : "FAIL", b4 ? "CONTAINED" : "FAIL",
          b5 ? "NO_MINT" : "FAIL", b6 ? "REFUSED" : "FAIL",
          U(blocked[0]), U(blocked[1]), U(blocked[2]), U(blocked[3]), U(blocked[4]),
          U(rogue_claims), U(lane_blocked), U(stuffed_claims), U(inflight_refused), U(root_refusals), U(mints_attack),
          U(r->aegis.mints - mints0), U(rogue_draft), U(lanes_before), U(lt.sent),
          U(lt.published), U(lt.lost), U(lt.duplicates), promote_ok ? "true" : "false");
    printf("R14 B: forged/borrowed/revoked/stale/GPU refused, slot stuffing contained, forged "
           "decisions minted nothing, promotion forgery refused; renewed grant, lanes %llu "
           "published, production %llu during, %llu after\n", U(lt.published),
           U(res->prod_during), U(res->prod_after));
    stop(r);
    free(r);
    res->ran = 1;
    res->pass = g_fail == fails;
}

/* ---- C: pathological reaction cycles in the same world ---------------------- */
typedef struct { RxObjRef from, to; int mode; int left; } Hop;
enum { HOP_BUMP, HOP_FLIP, HOP_DOWN, HOP_FLIP_N };
static int fn_hop(RxCtx *c) {
    Hop *h = c->user;
    uint64_t v = snapshot(c, h->from)->field[0];
    uint64_t out = h->mode == HOP_BUMP ? v + 1 : h->mode == HOP_DOWN ? (v ? v - 1 : 0) : v ^ 1u;
    if (h->mode == HOP_FLIP_N) {
        if (h->left <= 0) return 0;
        h->left--;
    }
    c->out[c->n_out++] = (RxMutation){h->to, 0, out};
    return 0;
}

static int hop_add(Rig *r, const char *name, Hop *h, RxCapRef rw, uint32_t loop_kind,
                   uint32_t *id) {
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.faculty = RX_FACULTY_AIEN;
    d.subject = CYCLE_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_hop;
    d.user = h;
    d.loop_kind = loop_kind;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){h->from, RX_FIELD(0)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){h->to, RX_FIELD(0)};
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){rw, RES_CYCLE, RX_RIGHT_READ | RX_RIGHT_WRITE};
    return rx_world_add_reaction(&r->w, &d, id);
}

static void scenario_c(void) {
    Result *res = &g_res[2];
    int fails = g_fail;
    Rig *r = calloc(1, sizeof *r);
    if (!r) return;
    Opts o = opts();
    o.oscillation = 8;
    int rc = start(r, &o);
    CHECK(rc == 0, "C: setup failed at stage %d", g_stage);
    Episode ep;
    if (rc == 0) {
        rc = adapt(r, &ep);
        CHECK(rc == 0, "C: the organism did not adapt with R6 on (%d)", rc);
    }
    if (rc != 0) { if (r->admin) stop(r); free(r); return; }
    res->gen_before = ep.gen_after;
    CHECK(produce(r, 100, 30000) == 0, "C: production before the storm");
    res->prod_before = served(r);
    uint32_t organism = r->w.n_reactions;   /* every reaction registered so far */
    uint64_t quarantines0 = r->w.stats.quarantines;

    RxObjRef a, b, x, y, z, f, cv, pd;
    RxObjRef *objs[8] = {&a, &b, &x, &y, &z, &f, &cv, &pd};
    for (int i = 0; i < 8; i++) CHECK(new_object(r, 0x6160 + (uint32_t)i, RES_CYCLE, objs[i]) == RX_OK,
                                      "C: object");
    RxCapRef rw = mint(r, CYCLE_SUBJ, RES_CYCLE, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef ext = mint(r, EXTERNAL, RES_CYCLE, RX_RIGHT_WRITE);
    static Hop h[8];
    h[0] = (Hop){a, b, HOP_BUMP, 0};  h[1] = (Hop){b, a, HOP_BUMP, 0};     /* A <-> B, diverges */
    h[2] = (Hop){x, y, HOP_BUMP, 0};  h[3] = (Hop){y, z, HOP_BUMP, 0};
    h[4] = (Hop){z, x, HOP_BUMP, 0};                                     /* X -> Y -> Z -> X */
    h[5] = (Hop){f, f, HOP_FLIP, 0};                                     /* A-B-A flip */
    h[6] = (Hop){cv, cv, HOP_DOWN, 0};                                   /* converges to 0 */
    h[7] = (Hop){pd, pd, HOP_FLIP_N, 40};                                /* declared periodic */
    const char *names[8] = {"cycle.ab.a", "cycle.ab.b", "cycle.xyz.x", "cycle.xyz.y",
                            "cycle.xyz.z", "cycle.flip", "control.converge", "control.periodic"};
    uint32_t id[8];
    for (int i = 0; i < 8; i++)
        CHECK(hop_add(r, names[i], &h[i], rw, i == 7 ? RX_LOOP_PERIODIC : RX_LOOP_ORDINARY,
                      &id[i]) == RX_OK, "C: %s", names[i]);
    uint64_t aien0 = acts(r, r->aien.r_observe), omega0 = acts(r, r->omega.r_watch);
    uint64_t crumbs0 = r->w.n_crumbs, served0 = served(r);
    uint64_t t0 = now_ns();
    RxObjRef starts[5] = {a, x, f, cv, pd};
    uint64_t start_v[5] = {1, 1, 1, 40, 1};
    int64_t root[5];
    for (int i = 0; i < 5; i++) {
        RxMutation m = {starts[i], 0, start_v[i]};
        root[i] = rx_world_publish_external(&r->w, ext, &m, 1);
        CHECK(root[i] > 0, "C: stimulus %d", i);
    }
    /* Containment: each pathological cycle stops inside its own episode. */
    uint64_t t = now_ns();
    int contained = 0;
    while (now_ns() - t < 20000000000ull && !contained) {
        pthread_mutex_lock(&r->w.mu);
        contained = (r->w.reactions[id[0]].quarantined || r->w.reactions[id[1]].quarantined) &&
                    (r->w.reactions[id[2]].quarantined || r->w.reactions[id[3]].quarantined ||
                     r->w.reactions[id[4]].quarantined) &&
                    r->w.reactions[id[5]].quarantined && r->w.objects[cv.id].field[0] == 0 &&
                    r->w.reactions[id[6]].state == RX_DORMANT &&
                    r->w.reactions[id[7]].activations >= 40;
        pthread_mutex_unlock(&r->w.mu);
        pause_us(1000);
    }
    uint64_t contain_ms = (now_ns() - t0) / 1000000u;
    CHECK(contained, "C: the cycles were not contained");
    pause_us(200000);
    uint64_t cycle_crumbs_a = 0;
    for (uint64_t k = crumbs0 + 1; k <= r->w.n_crumbs; k++) {
        const RxCrumb *c = rx_world_crumb(&r->w, k);
        for (int i = 0; i < 6; i++) if (c->reaction == id[i]) cycle_crumbs_a++;
    }
    pause_us(500000);                   /* after containment the cycles stay stopped */
    uint64_t cycle_crumbs_b = 0;
    for (uint64_t k = crumbs0 + 1; k <= r->w.n_crumbs; k++) {
        const RxCrumb *c = rx_world_crumb(&r->w, k);
        for (int i = 0; i < 6; i++) if (c->reaction == id[i]) cycle_crumbs_b++;
    }
    uint64_t storm_served = served(r) - served0;
    uint64_t aien1 = acts(r, r->aien.r_observe), omega1 = acts(r, r->omega.r_watch);
    /* Evidence: each containment crumb names a cycle reaction, the limit, and
     * the outside publication that started that cycle's episode. */
    uint64_t named = 0, wrong_episode = 0, organism_contained = 0;
    int by_cycle[3] = {0, 0, 0};
    for (uint64_t k = crumbs0 + 1; k <= r->w.n_crumbs; k++) {
        const RxCrumb *c = rx_world_crumb(&r->w, k);
        if (c->kind != RX_CRUMB_QUARANTINE) continue;
        int which = -1;
        for (int i = 0; i < 8; i++) if (c->reaction == id[i]) which = i;
        if (which < 0) { organism_contained++; continue; }
        named++;
        int cyc = which <= 1 ? 0 : which <= 4 ? 1 : which == 5 ? 2 : -1;
        if (cyc < 0) { wrong_episode++; continue; }
        uint64_t want_root = (uint64_t)root[cyc];
        int want_reason = cyc == 2 ? RX_CONTAIN_OSCILLATION : RX_CONTAIN_BUDGET;
        if (c->episode != want_root || c->reason != want_reason) wrong_episode++;
        else by_cycle[cyc] = 1;
    }
    uint64_t max_acts = 0;
    for (int i = 0; i < 6; i++)
        if (r->w.reactions[id[i]].activations > max_acts) max_acts = r->w.reactions[id[i]].activations;
    int organism_quarantined = 0;
    for (uint32_t i = 0; i < organism; i++)
        if (r->w.reactions[i].quarantined) organism_quarantined++;
    CHECK(by_cycle[0] && by_cycle[1] && by_cycle[2] && wrong_episode == 0,
          "C: containment evidence %d/%d/%d, %llu wrong", by_cycle[0], by_cycle[1], by_cycle[2],
          U(wrong_episode));
    CHECK(organism_contained == 0 && organism_quarantined == 0,
          "C: %llu organism containments, %d organism reactions quarantined",
          U(organism_contained), organism_quarantined);
    CHECK(max_acts <= BUDGET + 2, "C: a cycle ran %llu times in one episode", U(max_acts));
    CHECK(cycle_crumbs_b == cycle_crumbs_a, "C: the cycles kept writing crumbs (%llu -> %llu)",
          U(cycle_crumbs_a), U(cycle_crumbs_b));
    CHECK(!r->w.reactions[id[6]].quarantined && field(r, cv, 0) == 0,
          "C: the convergent control was stopped");
    CHECK(!r->w.reactions[id[7]].quarantined && r->w.reactions[id[7]].activations >= 40,
          "C: the periodic control was stopped");
    CHECK(storm_served >= 100 && aien1 > aien0 && omega1 > omega0,
          "C: production %llu, aien.observe %llu -> %llu, omega.watch %llu -> %llu",
          U(storm_served), U(aien0), U(aien1), U(omega0), U(omega1));
    uint64_t deferred_peak = r->w.stats.deferred_peak;
    CHECK(deferred_peak < RX_MAX_REACTIONS, "C: deferred queue peaked at %llu", U(deferred_peak));
    /* A new outside stimulus is a new episode: it gets a fresh, still bounded budget. */
    uint64_t before_again = r->w.reactions[id[0]].activations;
    RxMutation again = {a, 0, field(r, a, 0) + 1000};
    int64_t root2 = rx_world_publish_external(&r->w, ext, &again, 1);
    t = now_ns();
    while (now_ns() - t < 10000000000ull && r->w.reactions[id[0]].activations == before_again)
        pause_us(1000);
    pause_us(300000);
    uint64_t second = 0;
    for (uint64_t k = (uint64_t)root2 + 1; k <= r->w.n_crumbs; k++) {
        const RxCrumb *c = rx_world_crumb(&r->w, k);
        if (c->kind == RX_CRUMB_QUARANTINE && c->episode == (uint64_t)root2 &&
            (c->reaction == id[0] || c->reaction == id[1])) second++;
    }
    uint64_t again_acts = r->w.reactions[id[0]].activations - before_again;
    CHECK(root2 > 0 && again_acts > 0 && again_acts <= BUDGET + 2 && second >= 1,
          "C: second episode ran %llu, containment crumbs %llu", U(again_acts), U(second));
    res->prod_during = storm_served;
    res->fault_observed = contained;
    finish(r, res, 100);
    snprintf(res->containment, sizeof res->containment,
             "R6: A<->B and X->Y->Z->X quarantined by the per-episode activation budget, "
             "the flip by the oscillation limit; each with a QUARANTINE crumb naming its episode");
    snprintf(res->recovery, sizeof res->recovery,
             "none needed: production, AIEN and Omega never stopped; the controls finished");
    extra(res, "\"activation_budget_per_episode\": %u, \"oscillation_limit\": %d, "
          "\"cycles\": [\"A<->B (+1 each way)\", \"X->Y->Z->X (+1)\", \"self flip A-B-A\"], "
          "\"controls\": [\"countdown converges to 0\", \"declared periodic flip x40\"], "
          "\"containment_crumbs_named\": %llu, \"containment_wrong\": %llu, "
          "\"organism_reactions_quarantined\": %d, \"max_cycle_activations\": %llu, "
          "\"cycle_crumbs_at_containment\": %llu, \"cycle_crumbs_500ms_later\": %llu, "
          "\"deferred_queue_peak\": %llu, \"containment_ms\": %llu, "
          "\"production_during_storm\": %llu, \"aien_observe_activations_during\": %llu, "
          "\"omega_watch_activations_during\": %llu, \"second_episode_activations\": %llu, "
          "\"quarantines\": %llu", BUDGET, o.oscillation, U(named), U(wrong_episode),
          organism_quarantined, U(max_acts), U(cycle_crumbs_a), U(cycle_crumbs_b),
          U(deferred_peak), U(contain_ms), U(storm_served), U(aien1 - aien0), U(omega1 - omega0),
          U(again_acts), U(r->w.stats.quarantines - quarantines0));
    printf("R14 C: cycles contained in %llu ms (max %llu activations), %llu containment crumbs, "
           "organism untouched; production %llu during, %llu after\n", U(contain_ms),
           U(max_acts), U(named), U(storm_served), U(res->prod_after));
    stop(r);
    free(r);
    res->ran = 1;
    res->pass = g_fail == fails;
}

/* ---- D: saturate the GPU ------------------------------------------------------ */
typedef struct {
    uint64_t max_inflight, max_blocked, max_ring, max_ring_claims, max_memory, samples;
} Pressure;

static void sample(Rig *r, Pressure *p) {
    pthread_mutex_lock(&r->w.mu);
    uint64_t inflight = 0, blocked = 0;
    for (uint32_t k = 0; k < r->opt.lanes; k++) {
        const RxReaction *x = &r->w.reactions[r->r_lane[k]];
        if (x->resident_seat) inflight++;
        if (x->state == RX_BLOCKED_RESOURCE) blocked++;
    }
    const OmegaSharedWorldRing *ring =
        (const OmegaSharedWorldRing *)(r->w.coherent + rx_world_off_c2g());
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    uint64_t occ = tail - head, claims = 0;
    /* The ring carries claims and publication notices; count the claims. */
    for (uint64_t q = head; q < tail; q++)
        if (ring->slots[q & ring->mask].msg_type == RX_RING_CLAIM) claims++;
    uint64_t mem = r->w.used_memory;
    pthread_mutex_unlock(&r->w.mu);
    if (inflight > p->max_inflight) p->max_inflight = inflight;
    if (blocked > p->max_blocked) p->max_blocked = blocked;
    if (occ > p->max_ring) p->max_ring = occ;
    if (claims > p->max_ring_claims) p->max_ring_claims = claims;
    if (mem > p->max_memory) p->max_memory = mem;
    p->samples++;
}

static void scenario_d(void) {
    Result *res = &g_res[3];
    int fails = g_fail;
    Rig *r = calloc(1, sizeof *r);
    if (!r) return;
    Opts o = opts();
    o.lanes = LANES;
    int rc = start(r, &o);
    CHECK(rc == 0, "D: setup failed at stage %d", g_stage);
    Episode ep;
    if (rc == 0) {
        rc = adapt(r, &ep);
        CHECK(rc == 0, "D: the organism did not adapt (%d)", rc);
    }
    if (rc != 0) { if (r->admin) stop(r); free(r); return; }
    res->gen_before = ep.gen_after;
    res->prod_before = served(r);
    const uint64_t quota = 200;
    uint64_t claims0 = r->w.stats.resident_claims, mark = r->w.n_crumbs;
    uint64_t rejected0 = r->w.stats.desc_rejected;
    uint64_t served0 = served(r), aien0 = acts(r, r->aien.r_observe);
    Pressure p;
    memset(&p, 0, sizeof p);
    Driver dv;
    uint64_t t0 = now_ns();
    CHECK(driver_start(&dv, r, quota, 20) == 0, "D: driver");
    uint64_t total = quota * LANES, t_last_submit = 0;
    while (now_ns() - t0 < 120000000000ull) {
        sample(r, &p);
        if (!t_last_submit && lanes_sent(r) == total) t_last_submit = now_ns();
        if (t_last_submit && lanes_done(r) == total) break;
        pause_us(100);
    }
    uint64_t t_drained = now_ns();
    driver_stop(&dv);
    uint64_t during = served(r) - served0;
    uint64_t aien1 = acts(r, r->aien.r_observe);
    LaneTotals lt;
    lane_totals(r, &lt);
    uint64_t retried = 0, failed = 0;
    for (uint64_t id = mark + 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(&r->w, id);
        for (uint32_t j = 0; j < LANES; j++)
            if (k->reaction == r->r_lane[j] && k->worker == RX_SEAT_BLACKWELL &&
                k->kind != RX_CRUMB_COMMIT) failed++;
    }
    int reached = p.max_inflight == LANE_CAP && p.max_blocked >= 1 &&
                  p.max_memory <= (uint64_t)LANE_CAP * LANE_BYTES;
    CHECK(reached, "D: the limit was not reached: %llu concurrent claims (cap %u), %llu waiting, "
          "%llu bytes held", U(p.max_inflight), LANE_CAP, U(p.max_blocked), U(p.max_memory));
    CHECK(lt.sent == total && lt.published == total && lt.checked == total && lt.lost == 0 &&
              lt.duplicates == 0 && lt.mismatches == 0,
          "D: sent %llu published %llu checked %llu lost %llu duplicated %llu mismatched %llu",
          U(lt.sent), U(lt.published), U(lt.checked), U(lt.lost), U(lt.duplicates),
          U(lt.mismatches));
    CHECK(atomic_load(&r->acc_torn) == 0 && atomic_load(&r->acc_stale) == 0 &&
              atomic_load(&r->acc_auth) == 0 && failed == 0,
          "D: torn %llu stale %llu refused %llu failed %llu", U(atomic_load(&r->acc_torn)),
          U(atomic_load(&r->acc_stale)), U(atomic_load(&r->acc_auth)), U(failed));
    uint64_t dropped = r->w.stats.desc_rejected - rejected0;
    CHECK(p.max_ring_claims <= LANE_CAP && p.max_ring < OMEGA_SW_RING_CAPACITY && dropped == 0,
          "D: ring held %llu claims, %llu notices in all, %llu dropped or refused",
          U(p.max_ring_claims), U(p.max_ring), U(dropped));
    CHECK(during >= 100 && aien1 > aien0, "D: CPU work during saturation: production %llu, "
          "aien.observe %llu", U(during), U(aien1 - aien0));
    res->prod_during = during;
    res->fault_observed = reached;
    finish(r, res, 100);
    uint64_t claims = r->w.stats.resident_claims - claims0;
    snprintf(res->containment, sizeof res->containment,
             "R5 admitted at most %u lane claims (their windows held against the memory "
             "budget); the rest waited BLOCKED_RESOURCE", LANE_CAP);
    snprintf(res->recovery, sizeof res->recovery,
             "backlog drained after submission stopped; every request published and checked once");
    extra(res, "\"lanes\": %u, \"requests_per_lane\": %llu, \"claim_capacity\": %u, "
          "\"max_concurrent_claims\": %llu, \"max_backlog_waiting\": %llu, "
          "\"max_ring_occupancy\": %llu, \"max_ring_claims\": %llu, \"ring_notices_dropped\": %llu, \"max_bytes_held\": %llu, \"total_submitted\": %llu, "
          "\"total_completed\": %llu, \"total_checked\": %llu, \"total_retried\": %llu, "
          "\"total_failed\": %llu, \"duplicates\": %llu, \"lost\": %llu, \"mismatches\": %llu, "
          "\"claims\": %llu, \"slot_rewakes_same_value\": %llu, \"drain_ms\": %llu, "
          "\"saturation_ms\": %llu, \"production_during\": %llu, \"torn_refused\": %llu, "
          "\"pressure_samples\": %llu",
          LANES, U(quota), LANE_CAP, U(p.max_inflight), U(p.max_blocked), U(p.max_ring), U(p.max_ring_claims),
          U(dropped), U(p.max_memory), U(lt.sent), U(lt.published), U(lt.checked), U(retried), U(failed),
          U(lt.duplicates), U(lt.lost), U(lt.mismatches), U(claims), U(lt.rewakes),
          U(t_last_submit ? (t_drained - t_last_submit) / 1000000u : 0),
          U((t_drained - t0) / 1000000u), U(during), U(atomic_load(&r->acc_torn)), U(p.samples));
    printf("R14 D: %llu lane requests, at most %llu claims at once (cap %u), up to %llu waiting; "
           "%llu published once, 0 lost, 0 duplicated; production %llu during, %llu after\n",
           U(lt.sent), U(p.max_inflight), LANE_CAP, U(p.max_blocked), U(lt.published), U(during),
           U(res->prod_after));
    stop(r);
    free(r);
    res->ran = 1;
    res->pass = g_fail == fails;
}

/* ---- E: kill the resident seat while it holds real lane work ------------------ */
#ifdef R14_SILICON
static volatile uint32_t *hb_word(Rig *r, uint32_t off) {
    return (volatile uint32_t *)(r->w.coherent + rx_world_off_heartbeat() + off);
}

static void barrier(void) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}
#endif

static void scenario_e(void) {
    Result *res = &g_res[4];
    int fails = g_fail;
    Rig *r = calloc(1, sizeof *r);
    if (!r) return;
    Opts o = opts();
    o.lanes = LANES;
    int rc = start(r, &o);
    CHECK(rc == 0, "E: setup failed at stage %d", g_stage);
    Episode ep;
    if (rc == 0) {
        rc = adapt(r, &ep);
        CHECK(rc == 0, "E: the organism did not adapt (%d)", rc);
    }
    if (rc != 0) { if (r->admin) stop(r); free(r); return; }
    res->gen_before = ep.gen_after;
    Driver dv;
    CHECK(driver_start(&dv, r, 0, 300) == 0, "E: driver");
    CHECK(wait_lanes(r, 3 * LANES, 30000) == 0, "E: lanes never produced");
    atomic_store(&dv.pause, 1);
    pause_us(2000);
    CHECK(wait_lanes(r, lanes_sent(r), 30000) == 0, "E: lanes did not go idle");
    res->prod_before = served(r);
    uint32_t seat_gen0 = r->w.seat_generation;
    uint32_t gens[RX_MAX_OBJECTS];
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) gens[i] = r->w.objects[i].generation;
    uint64_t mints0 = r->aegis.mints;
    RxObjRef lslot = r->aegis.o[1].slot[0];
    uint64_t slot_id0 = field(r, lslot, 0), slot_gen0 = field(r, lslot, 1);
    uint64_t pub0 = r->w.reactions[r->r_lane[0]].commits;

    /* Hold: the chip keeps the next claim it takes (the R12 hold word). The
     * stand-in simply is not stepped. */
#ifdef R14_SILICON
    *hb_word(r, RX_SEAT_HB_HOLD) = 1;
    barrier();
#else
    atomic_store(&r->acceptor_pause, 1);
    pause_us(2000);
#endif
    uint64_t served_hold = served(r), mark = r->w.n_crumbs;
    CHECK(lane_submit(r, 0) == 0, "E: lane request");
    uint64_t stimulus = rx_world_explain(&r->w, r->lin[0], 2);   /* the lane request */
    uint64_t t = now_ns();
    while (!(r->w.reactions[r->r_lane[0]].resident_seat &&
             r->w.reactions[r->r_lane[0]].state == RX_RUNNING) &&
           now_ns() - t < 5000000000ull)
        pause_us(200);
    uint64_t seq = r->w.reactions[r->r_lane[0]].resident_seq;
    int claimed = r->w.reactions[r->r_lane[0]].resident_seat;
    int held = claimed;
#ifdef R14_SILICON
    held = 0;
    t = now_ns();
    while (claimed && !held && now_ns() - t < 5000000000ull) {
        barrier();
        if (*hb_word(r, RX_SEAT_HB_HELD) == (uint32_t)(seq + 1)) held = 1;
        else pause_us(500);
    }
#endif
    CHECK(claimed && held, "E: the seat did not hold the lane claim (claimed %d held %d)", claimed,
          held);
    uint64_t mem_held = r->w.used_memory;
    OmegaSharedWorldRing *c2g = (OmegaSharedWorldRing *)(r->w.coherent + rx_world_off_c2g());
    OmegaSharedWorldDesc old = c2g->slots[seq & c2g->mask];
    uint64_t fails0 = r->w.stats.transitions[RX_RUNNING][RX_FAILED];
    uint64_t t_kill = now_ns();
    int killed = 1;
#ifdef R14_SILICON
    killed = rx_gpu_seat_kill(r->seat) == 0;
#endif
    CHECK(killed, "E: the seat channel was not destroyed");
    int lost = rx_resident_seat_lost(&r->w, 1);
    uint64_t kill_ms = (now_ns() - t_kill) / 1000000u;
    uint64_t fail_id = 0;
    uint64_t n_fail = crumbs_of(r, r->r_lane[0], RX_CRUMB_FAILED, RX_ERR_SEAT_LOST, mark, &fail_id);
    const RxCrumb *kfail = rx_world_crumb(&r->w, fail_id);
    uint64_t mem_after = r->w.used_memory;
    int classified = lost == 1 && r->w.seat_generation == seat_gen0 + 1 &&
                     r->w.stats.transitions[RX_RUNNING][RX_FAILED] == fails0 + 1 && n_fail == 1 &&
                     kfail && kfail->worker == RX_SEAT_BLACKWELL && kfail->wake_cause == stimulus &&
                     mem_after <= LANE_BYTES;
    CHECK(classified, "E: lost %d, seat generation %u -> %u, failures +%llu, loss crumbs %llu, "
          "memory %llu -> %llu, cause %llu want %llu", lost, seat_gen0, r->w.seat_generation,
          U(r->w.stats.transitions[RX_RUNNING][RX_FAILED] - fails0), U(n_fail), U(mem_held),
          U(mem_after), U(kfail ? kfail->wake_cause : 0), U(stimulus));
    /* The old seat's window is not the object: the canonical value came back. */
    CHECK(field(r, r->lout[0], 0) != UINT64_MAX, "E: output");
    /* A new seat on the same image; the retry was already posted under the
     * new seat generation by the loss itself. */
    int relaunched = 1;
#ifdef R14_SILICON
    relaunched = rx_gpu_seat_relaunch(r->seat) == 0;
#else
    atomic_store(&r->acceptor_pause, 0);
#endif
    CHECK(relaunched, "E: the seat did not come back");
    t = now_ns();
    while (lane_done(r, 0) != r->lane_sent[0] && now_ns() - t < 10000000000ull) pause_us(500);
    uint64_t recover_ms = (now_ns() - t_kill) / 1000000u;
    uint64_t pub_id = 0;
    uint64_t retried_pub = crumbs_of(r, r->r_lane[0], RX_CRUMB_COMMIT, 0, mark, &pub_id);
    const RxCrumb *kpub = rx_world_crumb(&r->w, pub_id);
    int retried = lane_done(r, 0) == r->lane_sent[0] && retried_pub == 1 && kpub &&
                  kpub->wake_cause == fail_id && kpub->worker == RX_SEAT_BLACKWELL &&
                  r->w.reactions[r->r_lane[0]].commits == pub0 + 1 && field(r, r->lchk[0], 2) == 0;
    CHECK(retried, "E: retry published %llu times, lane %llu of %llu", U(retried_pub),
          U(lane_done(r, 0)), U(r->lane_sent[0]));
    /* The dead seat's claim, posted again: the new seat refuses it and nothing lands. */
    uint64_t stale0 = atomic_load(&r->acc_stale);
    uint64_t out_before = field(r, r->lout[0], 0);
    CHECK(rx_world_inject_descriptor(&r->w, &old) == RX_OK, "E: old claim");
    t = now_ns();
    while (atomic_load(&r->acc_stale) == stale0 && now_ns() - t < 5000000000ull) pause_us(500);
    int stale_refused = atomic_load(&r->acc_stale) > stale0 &&
                        r->w.reactions[r->r_lane[0]].commits == pub0 + 1 &&
                        field(r, r->lout[0], 0) == out_before;
    CHECK(stale_refused, "E: the dead seat's claim was not refused");
    uint32_t moved = 0;
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++)
        if (r->w.objects[i].live && r->w.objects[i].generation != gens[i]) moved++;
    int no_remint = r->aegis.mints == mints0 && field(r, lslot, 0) == slot_id0 &&
                    field(r, lslot, 1) == slot_gen0;
    CHECK(moved == 0 && no_remint, "E: %u object generations moved, mints %llu -> %llu", moved,
          U(mints0), U(r->aegis.mints));
    uint64_t during = served(r) - served_hold;
    /* Resume: the lanes and production keep going on the new seat. */
    uint64_t lanes_at = lanes_done(r);
    atomic_store(&dv.pause, 0);
    CHECK(wait_lanes(r, lanes_at + 3 * LANES, 30000) == 0, "E: lanes did not resume");
    driver_stop(&dv);
    CHECK(wait_lanes(r, lanes_sent(r), 30000) == 0, "E: lanes did not drain");
    res->prod_during = during;
    res->fault_observed = claimed && held && killed;
    finish(r, res, 100);
    LaneTotals lt;
    lane_totals(r, &lt);
    CHECK(lt.lost == 0 && lt.duplicates == 0 && lt.mismatches == 0,
          "E: lanes lost %llu duplicated %llu mismatched %llu", U(lt.lost), U(lt.duplicates),
          U(lt.mismatches));
    snprintf(res->containment, sizeof res->containment,
             "R12 seat loss: the held claim ended FAILED (RX_ERR_SEAT_LOST) once, its charge "
             "released, the seat generation moved");
    snprintf(res->recovery, sizeof res->recovery,
             "the seat was relaunched on the same image; the retry, caused by the loss crumb, "
             "published once; the dead seat's claim was refused");
    extra(res, "\"kill_method\": \"%s\", \"claim_held_at_kill\": %s, \"claims_lost\": %d, "
          "\"seat_generation_before\": %u, \"seat_generation_after\": %u, "
          "\"loss_crumbs\": %llu, \"loss_names_stimulus\": %s, \"bytes_held_before\": %llu, "
          "\"bytes_held_after_loss\": %llu, \"retry_publications\": %llu, "
          "\"retry_caused_by_loss\": %s, \"stale_result_refused\": %s, "
          "\"object_generations_moved\": %u, \"authority_reminted\": %s, "
          "\"kill_ms\": %llu, \"recovery_ms\": %llu, \"claims\": %llu, \"completions\": %llu, "
          "\"failures\": %llu, \"retries\": 1, \"duplicates\": %llu, \"lost\": %llu",
#ifdef R14_SILICON
          "host destroys the running GB10 channel group (rx_gpu_seat_kill); not a chip-raised fault",
#else
          "processor stand-in not stepped, then declared lost",
#endif
          held ? "true" : "false", lost, seat_gen0, r->w.seat_generation, U(n_fail),
          kfail && kfail->wake_cause == stimulus ? "true" : "false", U(mem_held), U(mem_after),
          U(retried_pub), kpub && kpub->wake_cause == fail_id ? "true" : "false",
          stale_refused ? "true" : "false", moved, no_remint ? "false" : "true", U(kill_ms),
          U(recover_ms), U(res->acc.claims), U(res->acc.committed_seat), U(res->acc.failed_seat),
          U(lt.duplicates), U(lt.lost));
    printf("R14 E: seat killed holding lane claim %llu; lost once, seat generation %u -> %u, "
           "retry published once, stale claim refused; production %llu during, %llu after\n",
           U(seq), seat_gen0, r->w.seat_generation, U(during), U(res->prod_after));
    stop(r);
    free(r);
    res->ran = 1;
    res->pass = g_fail == fails;
}

/* ---- F: crash inside the R9 promotion the organism itself makes -------------
 * First process: the R13 body adapts; R9 is set to stop the process (_exit)
 * at one crash point inside rx_gen_promote, on the promotion authority's own
 * activation. Second process: the canonical recovery entry (rx_gen_recover),
 * then a new body on the same R9 directory; the body starting is published,
 * generation.restore rebuilds what was in force; production resumes. With the
 * old generation, the body adapts again and promotes a new one. */
typedef struct {
    /* first process */
    uint64_t served, gen_before, hook_ran, hook_links, hook_links_ok, hook_crumbs, hook_crumbs_ok;
    uint8_t hook_prov_sha[32];
    /* second process */
    int rec_rc, coherent, receipt_before, receipt_after, event_present, torn;
    uint64_t rec_active, rec_lineage;
    uint64_t restore_outcome, restore_why, restore_active, restore_id0, inforce_gen;
    int restore_by_boot, prov_ok, prov_same;
    uint64_t prov_links, served2, wrong2, realized2;
    int readapt_ok;
    uint64_t readapt_gen, readapt_lineage, readapt_served;
    int crumbs_ok;
    uint64_t crumbs;
    int sweep_ok, acc_ok;
    uint64_t holders, unexpected;
    char err[160];
} FShared;

static FShared *g_fs;
static Rig *g_frig;

/* R9 disk hook: after the candidate is on disk and before the root flip.
 * The dying process checks the durable provenance against its live crumbs. */
static void f_hook(const char *generation_dir, void *ctx) {
    Rig *r = ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/provenance", generation_dir);
    RxLivingProvenance pv;
    FILE *f = fopen(path, "rb");
    size_t got = f ? fread(&pv, 1, sizeof pv, f) : 0;
    if (f) fclose(f);
    g_fs->hook_ran = 1;
    if (got != sizeof pv || memcmp(pv.magic, "R13PROV1", 8) != 0) return;
    sha256_hash((const uint8_t *)&pv, sizeof pv, g_fs->hook_prov_sha);
    uint64_t ok = 0;
    for (uint32_t i = 0; i < RX_LINK_COUNT; i++) {
        const RxCrumb *k = rx_world_crumb(&r->w, pv.link[i].crumb);
        if (k && memcmp(k->digest, pv.link[i].digest, 32) == 0 &&
            k->reaction == pv.link[i].reaction) ok++;
    }
    g_fs->hook_links = RX_LINK_COUNT;
    g_fs->hook_links_ok = ok;
    g_fs->hook_crumbs_ok = rx_world_verify_crumbs(&r->w, &g_fs->hook_crumbs) == 0;
}

static void *f_reporter(void *arg) {
    Rig *r = arg;
    for (;;) {
        g_fs->served = served(r);
        pause_us(1000);
    }
    return NULL;
}

static int f_first(const char *dir, int step) {
    Rig *r = calloc(1, sizeof *r);
    Opts o = opts();
    o.gen_dir = dir;
    o.crash_step = step;
    if (!r || start(r, &o) != 0) return 2;
    g_frig = r;
    uint64_t lineage = 0;
    rx_gen_active(r->gen, &g_fs->gen_before, &lineage);
    rx_gen_set_disk_hook(r->gen, f_hook, r);
    pthread_t rep;
    pthread_create(&rep, NULL, f_reporter, r);
    Episode ep;
    int rc = adapt(r, &ep);
    snprintf(g_fs->err, sizeof g_fs->err, "the first process was not stopped (adapt %d)", rc);
    return 3;                           /* the crash point never fired */
}

static int receipt_of(const char *dir, uint64_t id) {
    char path[512];
    snprintf(path, sizeof path, "%s/g/%llu/receipt", dir, U(id));
    return access(path, R_OK) == 0;
}

static int f_second(const char *dir, uint64_t want_active) {
    FShared *s = g_fs;
    s->receipt_before = receipt_of(dir, 2);
    RxRecoveryRecord rec;
    s->rec_rc = rx_gen_recover(dir, &rec);
    s->torn = s->rec_rc == RX_GEN_ERR_TORN;
    if (s->rec_rc != RX_GEN_OK) return 4;
    s->coherent = rec.coherent;
    s->rec_active = rec.active_id;
    s->rec_lineage = rec.lineage;
    s->event_present = rec.event_present;
    s->receipt_after = receipt_of(dir, 2);
    Rig *r = calloc(1, sizeof *r);
    Opts o = opts();
    o.gen_dir = dir;
    if (!r || start(r, &o) != 0) return 5;
    if (start_acceptor(r) != 0) return 6;
    /* The body starts where R13 starts it: on the A725 cores, and says so. */
    cpu_set_t ca, cx;
    if (core_sets(&ca, &cx) != 0 || move_threads(&ca) <= 0 || publish_placement(r, CLASS_A725) != 0)
        return 9;
    int64_t boot = publish_boot(r);
    uint64_t t = now_ns();
    while (field(r, r->living.o.restore, 1) != (uint64_t)field(r, r->living.o.restore, 0) &&
           now_ns() - t < 30000000000ull)
        pause_us(500);
    s->restore_outcome = field(r, r->living.o.restore, 2);
    s->restore_why = field(r, r->living.o.restore, 5);
    s->restore_active = field(r, r->living.o.restore, 3);
    s->restore_id0 = field(r, r->living.o.restore, 4);
    s->inforce_gen = field(r, r->living.o.inforce, 7);
    uint64_t kr = rx_world_explain(&r->w, r->living.o.restore, 2);
    const RxCrumb *k = rx_world_crumb(&r->w, kr);
    s->restore_by_boot = boot > 0 && k && k->reaction == r->living.r_restore &&
                         k->wake_cause == (uint64_t)boot && k->episode == (uint64_t)boot;
    /* The durable provenance of the recovered generation, read through R9. */
    if (rec.active_id != 1) {
        RxLivingProvenance pv;
        size_t n = 0;
        if (rx_gen_read_blob(r->gen, rec.active_id, "provenance", (uint8_t *)&pv, sizeof pv, &n) ==
                RX_GEN_OK && n == sizeof pv && memcmp(pv.magic, "R13PROV1", 8) == 0) {
            uint8_t sha[32];
            sha256_hash((const uint8_t *)&pv, sizeof pv, sha);
            s->prov_same = memcmp(sha, s->hook_prov_sha, 32) == 0;
            uint64_t links = 0;
            for (uint32_t i = 0; i < RX_LINK_COUNT; i++) {
                int nonzero = 0;
                for (uint32_t j = 0; j < 32; j++) nonzero |= pv.link[i].digest[j];
                if (pv.link[i].crumb && nonzero) links++;
            }
            const uint32_t order[] = {RX_LINK_GOAL, RX_LINK_PLAN, RX_LINK_SEARCH,
                RX_LINK_GPU_INPUT, RX_LINK_GPU_OUTPUT, RX_LINK_EVIDENCE, RX_LINK_BELIEF,
                RX_LINK_SELECTION};
            int ordered = 1;
            for (uint32_t i = 1; i < sizeof order / sizeof order[0]; i++)
                if (pv.link[order[i]].crumb <= pv.link[order[i - 1]].crumb) ordered = 0;
            s->prov_links = links;
            s->prov_ok = links == RX_LINK_COUNT && ordered && pv.epoch >= 2;
        }
    }
    /* Production restarts on whatever is in force now. */
    if (start_producer(r) != 0 || produce(r, 200, 60000) != 0) return 7;
    s->served2 = served(r);
    s->wrong2 = r->wrong;
    s->realized2 = r->omega.served_realized;
    if (want_active == 1) {
        /* The old generation came back: the organism adapts again by itself. */
        stop_producer(r);
        Episode ep;
        int arc = adapt(r, &ep);
        s->readapt_ok = arc == 0 && ep.promoted;
        if (!s->readapt_ok)
            snprintf(s->err, sizeof s->err, "adapt %d plan %llu search %llu selection %llu "
                     "evidence %llu candidate %llu promotion %llu refusal %d result %d crumbs %llu",
                     arc, U(field(r, r->aien.o.plan, 0)), U(field(r, r->omega.o.search, 0)),
                     U(field(r, r->omega.o.selection, 0)), U(field(r, r->living.o.evidence, 0)),
                     U(field(r, r->living.o.candidate, 0)), U(field(r, r->living.o.promotion, 0)),
                     r->living.last_refusal, r->promoter.result, U(r->w.n_crumbs));
        uint64_t lin = 0;
        rx_gen_active(r->gen, &s->readapt_gen, &lin);
        s->readapt_lineage = lin;
        s->readapt_served = served(r);
    }
    Result res;
    memset(&res, 0, sizeof res);
    finish(r, &res, 100);
    s->crumbs_ok = res.crumbs_ok;
    s->crumbs = res.crumbs;
    s->sweep_ok = res.sweep_ok;
    s->acc_ok = res.acc.ok;
    s->holders = res.holders;
    s->unexpected = res.unexpected;
    s->wrong2 = r->wrong;
    stop(r);
    free(r);
    return g_fail ? 8 : 0;
}

static int run_child(int (*fn)(const char *, int), const char *dir, int arg, int timeout_s,
                     int *status) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        g_fail = 0;
        _exit(fn(dir, arg));
    }
    uint64_t t = now_ns();
    for (;;) {
        pid_t w = waitpid(pid, status, WNOHANG);
        if (w == pid) return 0;
        if (w < 0 && errno != EINTR) return -1;
        if (now_ns() - t > (uint64_t)timeout_s * 1000000000ull) {
            kill(pid, SIGKILL);
            waitpid(pid, status, 0);
            return -2;
        }
        pause_us(10000);
    }
}

static int f_second_int(const char *dir, int want) { return f_second(dir, (uint64_t)want); }

typedef struct {
    int step, exit_first, exit_second;
    uint64_t want, recovered;
    FShared s;
} FRow;
static FRow g_rows[8];

static void scenario_f(void) {
    Result *res = &g_res[5];
    int fails = g_fail;
    g_fs = mmap(NULL, sizeof *g_fs, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_fs == MAP_FAILED) { CHECK(0, "F: shared memory"); return; }
    const int steps[8] = {RX_CRASH_BEFORE_CANDIDATE_WRITE, RX_CRASH_DURING_CANDIDATE_WRITE,
        RX_CRASH_AFTER_CANDIDATE_WRITE, RX_CRASH_BEFORE_ROOT_FLIP, RX_CRASH_DURING_ROOT_FLIP,
        RX_CRASH_AFTER_ROOT_FLIP, RX_CRASH_BEFORE_RECEIPT, RX_CRASH_AFTER_RECEIPT};
    /* The root flip is the commit: before it the old generation, after it the new. */
    const uint64_t want[8] = {1, 1, 1, 1, 1, 2, 2, 2};
    static const char *names[8] = {"BEFORE_CANDIDATE_WRITE", "DURING_CANDIDATE_WRITE",
        "AFTER_CANDIDATE_WRITE", "BEFORE_ROOT_FLIP", "DURING_ROOT_FLIP", "AFTER_ROOT_FLIP",
        "BEFORE_RECEIPT", "AFTER_RECEIPT"};
    int all_crashed = 1;
    uint64_t prod_before = 0, prod_after = 0, crumbs = 0;
    int crumbs_ok = 1, sweep_ok = 1, acc_ok = 1;
    extra(res, "\"crash_matrix\": [");
    for (int i = 0; i < 8; i++) {
        FRow *row = &g_rows[i];
        memset(g_fs, 0, sizeof *g_fs);
        char dir[64];
        if (!mkdtemp(strcpy(dir, "/tmp/r14-crash-XXXXXX"))) { CHECK(0, "F: dir"); continue; }
        int st = 0;
        int w = run_child(f_first, dir, steps[i], 240, &st);
        row->step = steps[i];
        row->want = want[i];
        row->exit_first = w == 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        CHECK(row->exit_first == 86, "F %s: the first process ended with %d (%s)", names[i],
              row->exit_first, g_fs->err);
        if (row->exit_first != 86) all_crashed = 0;
        w = run_child(f_second_int, dir, (int)want[i], 400, &st);
        row->exit_second = w == 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        row->s = *g_fs;
        FShared *s = &row->s;
        row->recovered = s->rec_active;
        CHECK(row->exit_second == 0, "F %s: the restarted process ended with %d", names[i],
              row->exit_second);
        CHECK(s->rec_rc == RX_GEN_OK && s->coherent && !s->torn && s->rec_active == want[i] &&
                  s->rec_lineage == want[i],
              "F %s: recovered %llu (lineage %llu, rc %d), want %llu", names[i], U(s->rec_active),
              U(s->rec_lineage), s->rec_rc, U(want[i]));
        /* A receipt exists only for a generation that was committed; recovery may
         * complete a committed flip's receipt, never invent one. */
        CHECK(want[i] == 2 ? s->receipt_after : (!s->receipt_before && !s->receipt_after),
              "F %s: receipt before %d after %d", names[i], s->receipt_before, s->receipt_after);
        CHECK(s->event_present, "F %s: the recovered generation has no event", names[i]);
        uint64_t want_outcome = want[i] == 2 ? RX_LIVING_RESTORE_RESTORED
                                             : RX_LIVING_RESTORE_REFERENCE;
        CHECK(s->restore_outcome == want_outcome && s->restore_active == want[i] &&
                  s->restore_by_boot,
              "F %s: restore outcome %llu (why %llu) for generation %llu, caused by boot %d",
              names[i], U(s->restore_outcome), U(s->restore_why), U(s->restore_active),
              s->restore_by_boot);
        if (want[i] == 2) {
            CHECK(s->inforce_gen == 2 && s->realized2 > 0 && s->prov_ok && s->prov_same &&
                      s->hook_links_ok == RX_LINK_COUNT && s->hook_crumbs_ok,
                  "F %s: in force %llu, realized %llu, provenance %d same %d, links %llu/%llu",
                  names[i], U(s->inforce_gen), U(s->realized2), s->prov_ok, s->prov_same,
                  U(s->hook_links_ok), U(s->hook_links));
        } else {
            CHECK(s->inforce_gen == 0 && s->realized2 == 0 && s->readapt_ok &&
                      s->readapt_lineage == 2 && s->readapt_gen >= 3,
                  "F %s: in force %llu, realized %llu, adapted again %d to %llu (lineage %llu): %s",
                  names[i], U(s->inforce_gen), U(s->realized2), s->readapt_ok,
                  U(s->readapt_gen), U(s->readapt_lineage), s->err);
        }
        CHECK(s->wrong2 == 0 && s->served2 >= 200, "F %s: production after restart %llu, wrong %llu",
              names[i], U(s->served2), U(s->wrong2));
        CHECK(s->crumbs_ok && s->sweep_ok && s->acc_ok, "F %s: audits crumbs %d sweep %d "
              "accounting %d", names[i], s->crumbs_ok, s->sweep_ok, s->acc_ok);
        prod_before += s->served;
        prod_after += s->served2;
        crumbs += s->crumbs;
        crumbs_ok &= s->crumbs_ok;
        sweep_ok &= s->sweep_ok;
        acc_ok &= s->acc_ok;
        extra(res, "%s{\"crash_point\": \"%s\", \"first_process_exit\": %d, "
              "\"recovered_old_or_new\": \"%s\", \"recovered_generation\": %llu, "
              "\"recovered_lineage\": %llu, \"torn_generation_detected\": %s, "
              "\"receipt_before_recovery\": %s, \"receipt_after_recovery\": %s, "
              "\"restore\": \"%s\", \"restore_caused_by_boot\": %s, \"in_force_generation\": %llu, "
              "\"production_before_crash\": %llu, \"production_after_restart\": %llu, "
              "\"production_used_restored_realization\": %llu, "
              "\"durable_provenance_links\": %llu, \"provenance_links_checked_before_crash\": %llu, "
              "\"provenance_bytes_same_after_restart\": %s, \"adapted_again_to_generation\": %llu}",
              i ? ", " : "", names[i], row->exit_first, s->rec_active == 1 ? "OLD" : "NEW",
              U(s->rec_active), U(s->rec_lineage), s->torn ? "true" : "false",
              s->receipt_before ? "true" : "false", s->receipt_after ? "true" : "false",
              s->restore_outcome == RX_LIVING_RESTORE_RESTORED ? "RESTORED"
              : s->restore_outcome == RX_LIVING_RESTORE_REFERENCE ? "REFERENCE" : "REFUSED",
              s->restore_by_boot ? "true" : "false", U(s->inforce_gen), U(s->served),
              U(s->served2), U(s->realized2), U(s->prov_links), U(s->hook_links_ok),
              s->prov_same ? "true" : "false", U(s->readapt_gen));
        printf("R14 F %s: first process exit %d; recovered %s generation %llu; restore %s; "
               "%llu served after restart%s\n", names[i], row->exit_first,
               s->rec_active == 1 ? "OLD" : "NEW", U(s->rec_active),
               s->restore_outcome == RX_LIVING_RESTORE_RESTORED ? "RESTORED" : "REFERENCE",
               U(s->served2), want[i] == 1 ? (s->readapt_ok ? ", adapted again" : ", NOT adapted")
                                           : "");
        char cmd[128];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);
    }
    extra(res, "]");
    res->fault_observed = all_crashed;
    res->prod_before = prod_before;
    res->prod_during = 0;               /* the process was dead */
    res->prod_after = prod_after;
    res->crumbs = crumbs;
    res->crumbs_ok = crumbs_ok;
    res->sweep_ok = sweep_ok;
    res->acc.ok = acc_ok;
    res->holders = 1;
    res->gen_before = 1;
    res->gen_after = 0;
    for (int i = 0; i < 8; i++)
        if (g_rows[i].recovered > res->gen_after) res->gen_after = g_rows[i].recovered;
    snprintf(res->containment, sizeof res->containment,
             "the process stopped inside rx_gen_promote at each R9 crash point");
    snprintf(res->recovery, sizeof res->recovery,
             "rx_gen_recover in a new process; generation.restore re-verified and put the "
             "recovered generation in force (or the reference for genesis); production resumed");
    munmap(g_fs, sizeof *g_fs);
    g_fs = NULL;
    res->ran = 1;
    res->pass = g_fail == fails;
}

/* ---- receipt ------------------------------------------------------------------ */
static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t block[65536], digest[32];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0) sha256_update(&c, block, n);
    fclose(f);
    sha256_final(&c, digest);
    hex(digest, 32, out);
}

static int receipt(int tests_ok) {
    char path[512], commit[41] = {0}, binary[65], physics[80] = {0};
    if (omega_evidence_path("R14/rx_recovery_receipt.json", path, sizeof path) != 0) return 0;
    if (!omega_evidence_run_commit(commit)) strcpy(commit, "unknown");
    if (!omega_evidence_physics_commit(physics, sizeof physics)) strcpy(physics, "unknown");
    binary_digest(binary);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    const char *aienos = getenv("AIENOS_COMMIT");
    int dirty = omega_evidence_tree_dirty();
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 && !dirty;
    int silicon = 0;
#ifdef R14_SILICON
    silicon = 1;
#endif
    int all = tests_ok;
    for (int i = 0; i < 6; i++) all &= g_res[i].ran && g_res[i].pass;
    int pass = all && bound && silicon;
    const char *gate = pass ? "PASS" : !all ? "FAIL" : !silicon ? "HOST_PASS_NON_SILICON"
                                                             : "SILICON_PASS_UNBOUND";
    struct utsname host;
    memset(&host, 0, sizeof host);
    uname(&host);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    uint64_t crumbs = 0, holders_max = 0, unexpected = 0;
    uint64_t final_gen = 0;
    for (int i = 0; i < 5; i++) if (g_res[i].gen_after > final_gen) final_gen = g_res[i].gen_after;
    int causal = 1;
    for (int i = 0; i < 6; i++) {
        crumbs += g_res[i].crumbs;
        causal &= g_res[i].crumbs_ok;
        unexpected += g_res[i].unexpected;
        if (g_res[i].holders > holders_max) holders_max = g_res[i].holders;
    }
    fprintf(f,
        "{\n"
        "  \"schema\": \"AIEN_RX_R14_LIVING_RECOVERY_V1\",\n"
        "  \"run_id\": \"%s\",\n"
        "  \"candidate_commit\": \"%s\",\n"
        "  \"run_commit\": \"%s\",\n"
        "  \"candidate_bound\": %s,\n"
        "  \"tree_dirty\": %s,\n"
        "  \"aienos_commit\": \"%s\",\n"
        "  \"physics_forge_commit\": \"%s\",\n"
        "  \"test_binary_sha256\": \"%s\",\n"
        "  \"machine_identity\": {\"node\": \"%s\", \"system\": \"%s\", \"release\": \"%s\", "
            "\"architecture\": \"%s\"},\n"
        "  \"silicon_observed\": %s,\n"
        "  \"gate\": {\"R14_LIVING_RECOVERY\": \"%s\"},\n"
        "  \"r13_baseline_generation\": 1,\n"
        "  \"final_generation\": %llu,\n"
        "  \"recovered_generations\": [%llu, %llu, %llu, %llu, %llu, %llu, %llu, %llu],\n"
        "  \"body\": \"R13 organism (rx_living) with R6 on: activation budget %u per causal "
            "episode\",\n"
        "  \"scenarios\": {\n",
        omega_evidence_run_id(), candidate ? candidate : "unknown", commit,
        bound ? "true" : "false", dirty ? "true" : "false", aienos ? aienos : "unknown", physics,
        binary, host.nodename, host.sysname, host.release, host.machine,
        silicon ? "true" : "false", gate, U(final_gen), U(g_rows[0].recovered),
        U(g_rows[1].recovered), U(g_rows[2].recovered), U(g_rows[3].recovered),
        U(g_rows[4].recovered), U(g_rows[5].recovered), U(g_rows[6].recovered),
        U(g_rows[7].recovered), BUDGET);
    for (int i = 0; i < 6; i++) {
        const Result *s = &g_res[i];
        fprintf(f,
            "    \"%s\": {\"result\": \"%s\", \"fault_observed\": %s, "
            "\"containment_result\": \"%s\", \"recovery_result\": \"%s\", "
            "\"production_before\": %llu, \"production_during_fault_window\": %llu, "
            "\"production_after\": %llu, \"crumbs_verified\": %llu, "
            "\"crumbs_evidence_verified\": %s, \"authority_sweep\": \"%s\", "
            "\"promotion_right_holders\": %llu, \"unexpected_privileged_holders\": %llu, "
            "\"resource_claim_accounting\": \"%s\", \"claims\": %llu, \"claims_closed\": %llu, "
            "\"inflight_final\": %llu, \"held_resource_charges\": %llu, "
            "\"active_generation_before\": %llu, \"active_generation_after\": %llu, %s}%s\n",
            g_names[i], s->ran ? (s->pass ? "PASS" : "FAIL") : "NOT_RUN",
            s->fault_observed ? "true" : "false", s->containment, s->recovery,
            U(s->prod_before), U(s->prod_during), U(s->prod_after), U(s->crumbs),
            s->crumbs_ok ? "true" : "false", s->sweep_ok ? "PASS" : "FAIL", U(s->holders),
            U(s->unexpected), s->acc.ok ? "PASS" : "FAIL", U(s->acc.claims), U(s->acc.closed),
            U(s->acc.inflight), U(s->acc.used_slots + s->acc.holding), U(s->gen_before),
            U(s->gen_after), s->extra[0] ? s->extra : "\"-\": null", i < 5 ? "," : "");
    }
    fprintf(f,
        "  },\n"
        "  \"authority\": {\"promotion_right_holders\": %llu, \"promotion_subject\": %u, "
            "\"unexpected_privileged_holders\": %llu},\n"
        "  \"causal\": {\"crumb_count\": %llu, \"causal_verification_result\": \"%s\", "
            "\"durable_evidence_links_verified\": %s},\n"
        "  \"earlier_gate_defects_fixed\": [\n"
        "    \"R6: the activation budget reset only on a direct outside wake, so reactions one "
            "step down a chain (omega.watch, aien.observe) were quarantined after 256 "
            "activations over the body's life; now counted per causal episode (regression: "
            "episode_activation_budget in test-r3)\",\n"
        "    \"R6: containment left no causal record; now a QUARANTINE crumb names the reaction, "
            "the limit and the episode\",\n"
        "    \"R8/R12: after an R8 renewal the seat claim presented the revoked reference bound "
            "at start, so honest GPU work was refused forever; the claim now presents what the "
            "slot holds (regression: slot renewal in test-r12)\",\n"
        "    \"R12: the GB10 seat refused every claim on an object id of 64 or more (it used the frozen "
            "ABI's 64-entry table size; the world projects RX_MAX_OBJECTS = 256 and the host "
            "stand-in accepted them), so GPU work on later objects failed on silicon only; found by "
            "scenario D (regression: objects past id 64 in test-r12-silicon)\",\n"
"    \"R9: draft slots were never freed, so a body could promote at most four times per "
            "process; a promoted or stale draft now frees its slot (regression in test-r9)\"\n"
        "  ],\n"
        "  \"limits\": [\n"
        "    \"R8: a principal wrongly given WRITE on a decision object can jam decisions until "
            "it is revoked; it cannot cause a mint\",\n"
        "    \"R9: rx_gen_propose has no authority check; an in-process proposer can hold up to "
            "four unpromotable drafts until the generation moves\",\n"
        "    \"R10: Omega's store keeps the first bytes filed under an identity; bytes that "
            "falsely claim an identity block the honest bytes for that identity in that "
            "process (found by reading, not exercised)\",\n"
        "    \"the authority table is the host-library C port; it does not survive a process "
            "restart, and the restarted body mints its grants again\",\n"
        "    \"the seat is killed by destroying its channel from the host; a chip-raised fault "
            "(MMU fault, Xid) is not exercised\"\n"
        "  ],\n"
        "  \"not_claimed\": [\"R15\", \"R16\", \"neural/general cognition beyond R11\", "
            "\"open-ended invention beyond R10\", "
            "\"AIENOS kernel isolation (authority is the host-library C port)\", "
            "\"immunity to every hardware fault\", \"recovery from machine destruction\", "
            "\"Byzantine hardware\", \"kernel/root compromise\", \"performance\"]\n"
        "}\n",
        U(holders_max), (unsigned)RX_LIVING_PROMOTE_SUBJ, U(unexpected), U(crumbs),
        causal ? "PASS" : "FAIL", g_res[5].pass ? "true" : "false");
    fclose(f);
    printf("R14 gate: R14_LIVING_RECOVERY=%s\nR14 receipt: %s\n", gate, path);
    return pass;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (seen_fit(CRUMBS) != 0) return 1;
    const char *only = argc > 1 ? argv[1] : NULL;
    /* F first: its processes are forked before this one ever opens the GPU. */
    if (!only || strchr(only, 'F')) scenario_f();
    if (!only || strchr(only, 'A')) scenario_a();
    if (!only || strchr(only, 'B')) scenario_b();
    if (!only || strchr(only, 'C')) scenario_c();
    if (!only || strchr(only, 'D')) scenario_d();
    if (!only || strchr(only, 'E')) scenario_e();
    for (int i = 0; i < 6; i++)
        printf("R14 %-22s %s\n", g_names[i],
               g_res[i].ran ? (g_res[i].pass ? "PASS" : "FAIL") : "NOT RUN");
    receipt(g_fail == 0);
    free(g_seen);
    free(g_stack);
    return g_fail ? 1 : 0;
}
