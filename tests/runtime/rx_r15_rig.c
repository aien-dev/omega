/* rx_r15_rig.c -- see rx_r15_rig.h. The body is built exactly as
 * tests/runtime/rx_r13_living.c builds its POSITIVE case. */
#include "rx_r15_rig.h"
#include "omega_evidence.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EXTERNAL = 100, ISSUER = 3 };
#define RES_INTENT 0x6130010ull
#ifndef TARGET_PCT
#define TARGET_PCT 55u
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Busy wait without a system call: a 20 us sleep costs about 80 us. */
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

const char *r15_config_name(R15Config c) {
    return c == R15_RES4 ? "RES-4" : c == R15_RES1 ? "RES-1" : "SEQ";
}

static RxCapRef mint(R15Rig *r, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office, ref = {UINT32_MAX, 0};
    aienos_cap_office(r->admin, &office);
    AienosCapMint m = {ISSUER, subject, resource, rights, 0, {UINT32_MAX, 0}, office};
    if (aienos_cap_mint(r->admin, &m, &ref) == 0) r->minted++;
    return (RxCapRef){ref.cap_id, ref.generation};
}

static uint64_t field(R15Rig *r, RxObjRef ref, uint32_t i) {
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
    R15Rig *r = c->user;
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
        if (part == R15_CLASS_A725) { CPU_SET(cpu, a); na++; }
        if (part == R15_CLASS_X925) { CPU_SET(cpu, x); nx++; }
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

static int new_object(R15Rig *r, uint32_t type, uint64_t resource, RxObjRef *out) {
    uint64_t z[RX_MAX_FIELDS] = {0};
    return rx_world_create(&r->w, type, RX_PERSIST_RESIDENT, resource, z, out);
}

/* ---- completion transport (both orchestrations) ------------------------- */

/* Ring transport only: move seat completions into the world. */
static int progress(R15Rig *r) {
#ifdef R15_SILICON
    int rc = rx_resident_accept(&r->w);
    return rc == RX_ERR_NOT_FOUND || rc == RX_OK ? 0 : rc;
#else
    int rc = rx_resident_seat_step(&r->w);
    if (rc == 1) return rx_resident_accept(&r->w);
    return rc < 0 ? rc : 0;
#endif
}

/* RES: sleeps while no claim is outstanding (no polling nap that would
 * distort GPU latency), then moves completions as they arrive. */
static void *transport_main(void *arg) {
    R15Rig *r = arg;
    while (!atomic_load(&r->transport_stop)) {
        if (rx_resident_wait_outstanding(&r->w, 50) != RX_OK) continue;
        int rc = progress(r);
        if (rc < 0) atomic_store(&r->transport_error, rc);
        else sched_yield();
    }
    return NULL;
}

/* SEQ: the orchestrator's "dispatch GPU, wait" step. */
static int seq_gpu_step(void *ctx) {
    return progress((R15Rig *)ctx);
}

/* ---- SEQ orchestrator -------------------------------------------------- */

/* spec §2.2, in order; omega.{synthesize,verify,measure}.k interleaved per k. */
static const char *const k_order[] = {
    "workload.matvec.serve", "living.experiment.ask", "aegis.decide", "root.install",
    "aien.observe", "aien.predict", "aien.explain", "aien.assess", "aien.plan",
    "omega.watch", "omega.reconsider", "omega.k", "omega.select",
    "living.experiment.prepare", "living.blackwell.add", "living.experiment.evidence",
    "aien.experiment.observe", "generation.prepare", "generation.promote",
    "generation.restore",
};

static int omega_slot_rank(const char *name, uint32_t *rank) {
    static const char *const stage[] = {"omega.synthesize.", "omega.verify.", "omega.measure."};
    for (uint32_t s = 0; s < 3; s++) {
        size_t n = strlen(stage[s]);
        if (strncmp(name, stage[s], n) == 0) {
            *rank = (uint32_t)atoi(name + n) * 3u + s;
            return 1;
        }
    }
    return 0;
}

/* Caller holds w->mu. Every registered reaction appears exactly once:
 * listed ones in §2.2 order, then any others in registration order. */
static void build_plan(R15Rig *r) {
    uint32_t n = 0, nr = r->w.n_reactions;
    uint8_t used[RX_MAX_REACTIONS] = {0};
    for (uint32_t o = 0; o < sizeof k_order / sizeof k_order[0]; o++) {
        if (strcmp(k_order[o], "omega.k") == 0) {
            for (uint32_t rank = 0; rank < 3u * RX_OMEGA_SLOTS; rank++)
                for (uint32_t i = 0; i < nr; i++) {
                    uint32_t got;
                    if (!used[i] && r->w.reactions[i].desc.name &&
                        omega_slot_rank(r->w.reactions[i].desc.name, &got) && got == rank) {
                        r->order[n++] = i;
                        used[i] = 1;
                    }
                }
            continue;
        }
        for (uint32_t i = 0; i < nr; i++)
            if (!used[i] && r->w.reactions[i].desc.name &&
                strcmp(r->w.reactions[i].desc.name, k_order[o]) == 0) {
                r->order[n++] = i;
                used[i] = 1;
            }
    }
    for (uint32_t i = 0; i < nr; i++)
        if (!used[i]) r->order[n++] = i;
    r->plan.order = r->order;
    r->plan.n = n;
    r->plan.gpu_step = seq_gpu_step;
    r->plan.gpu_ctx = r;
    r->plan.gpu_timeout_ms = 5000;
    r->plan_for = nr;
}

/* The legacy loop: pulse forever; when a whole pulse runs nothing, poll
 * again without sleeping (spec §2.1). */
static void *orchestrator_main(void *arg) {
    R15Rig *r = arg;
    while (!atomic_load(&r->orch_stop)) {
        pthread_mutex_lock(&r->w.mu);
        if (r->plan_for != r->w.n_reactions) build_plan(r);
        pthread_mutex_unlock(&r->w.mu);
        uint32_t ran = 0;
        int rc = rx_seq_pulse(&r->w, &r->plan, &ran);
        if (rc != RX_OK) { atomic_store(&r->orch_error, rc); break; }
        if (ran == 0) sched_yield();
    }
    return NULL;
}

/* ---- build ------------------------------------------------------------- */

static void take_baseline(R15Rig *r) {
    pthread_mutex_lock(&r->w.mu);
    for (uint32_t i = 0; i < r->w.n_reactions; i++) {
        const RxReactionDesc *d = &r->w.reactions[i].desc;
        for (uint32_t t = 0; t < d->n_triggers; t++) {
            const RxObject *o = &r->w.objects[d->triggers[t].obj.id];
            uint64_t v = 0;
            for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
                if ((d->triggers[t].mask & RX_FIELD(f)) && o->field_version[f] > v)
                    v = o->field_version[f];
            r->baseline[i][t] = v;
        }
    }
    pthread_mutex_unlock(&r->w.mu);
}

/* R16 C5: enroll one subject into a keyring (and, for a subject two
 * components act as, copy the same credential into a second one). */
static int enroll(R15Rig *r, RxCallerKeyring *k, uint32_t subject) {
    if (k->n >= RX_CALLER_KEYRING_MAX) return -1;
    if (rx_world_enroll_caller(&r->w, subject, &k->cred[k->n]) != RX_CALLER_OK) return -1;
    k->subject[k->n++] = subject;
    return 0;
}

static int share(RxCallerKeyring *to, const RxCallerKeyring *from, uint32_t subject) {
    const RxCallerCred *c = rx_caller_find(from, subject);
    if (!c || to->n >= RX_CALLER_KEYRING_MAX) return -1;
    to->subject[to->n] = subject;
    to->cred[to->n++] = *c;
    return 0;
}

static int enroll_callers(R15Rig *r, const uint32_t *fixtures, uint32_t n_fixtures) {
    if (n_fixtures > RX_CALLER_KEYRING_MAX) return -1;
    if (enroll(r, &r->keys_omega, RX_OMEGA_SUBJ_SERVE) || enroll(r, &r->keys_omega, RX_OMEGA_SUBJ_OMEGA) ||
        enroll(r, &r->keys_aien, RX_AIEN_SUBJ) ||
        enroll(r, &r->keys_aegis, RX_AEGIS_SUBJ) || enroll(r, &r->keys_aegis, RX_AEGIS_ROOT_SUBJ) ||
        enroll(r, &r->keys_living, RX_LIVING_SUBJ) || enroll(r, &r->keys_living, RX_LIVING_SEAT_SUBJ) ||
        enroll(r, &r->keys_living, RX_LIVING_PREPARE_SUBJ) ||
        enroll(r, &r->keys_promoter, RX_LIVING_PROMOTE_SUBJ) ||
        share(&r->keys_rig, &r->keys_living, RX_LIVING_SEAT_SUBJ))
        return -1;
    for (uint32_t i = 0; i < n_fixtures; i++)
        if (enroll(r, &r->fixtures, fixtures[i])) return -1;
    return rx_world_bind_callers(&r->w) == RX_OK ? 0 : -1;
}

int r15_start(R15Rig *r, R15Config config) {
    return r15_start_with(r, config, NULL, 0);
}

int r15_start_with(R15Rig *r, R15Config config, const uint32_t *fixture_subjects,
                   uint32_t n_fixtures) {
    memset(r, 0, sizeof *r);
    r->config = config;
    r->stage = 1;
    if (aienos_cap_start(&r->admin, &r->view) != 0) return -1;
    int wrc = config == R15_SEQ
        ? rx_world_init_native_sequential_reference(&r->w, r->view, RX_CRUMBS_LONG_EPISODE)
        : rx_world_init_native(&r->w, r->view, config == R15_RES4 ? 4 : 1,
                               RX_CRUMBS_LONG_EPISODE);
    if (wrc != RX_OK) return -1;
    r->w.external_subject = EXTERNAL;
    if (enroll_callers(r, fixture_subjects, n_fixtures) != 0) {
        r->stage_why = "R16 C5: caller enrollment failed";
        return -1;
    }
    if (config == R15_SEQ) {
        /* The orchestrator is the only thing that runs reactions in SEQ, so
         * it runs from the start, as the workers do in RES. */
        pthread_mutex_lock(&r->w.mu);
        build_plan(r);
        pthread_mutex_unlock(&r->w.mu);
        if (pthread_create(&r->orchestrator, NULL, orchestrator_main, r) != 0) return -1;
        r->orch_live = 1;
    }
    if (!mkdtemp(strcpy(r->generation_dir, "/tmp/r15-rig-XXXXXX"))) return -1;
    if (rx_gen_open(r->generation_dir, &r->gen) != RX_GEN_OK) return -1;
    /* R16 C5: the store checks every proposer and promoter credential against
     * this world, and uses the native authority whatever a caller passes. */
    if (rx_gen_bind_authority(r->gen, rx_world_caller_check_fn, &r->w,
                              rx_living_native_authority, r->view) != RX_GEN_OK) return -1;
    r->stage = 2;
    const uint32_t R = RX_RIGHT_READ, RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    RxOmegaConfig oc;
    rx_omega_default_config(&oc);
    oc.hot_calls = 64; oc.hot_ns = 200000; oc.margin_pct = 10;
    if (rx_omega_create_objects(&r->omega, &r->w, &oc) != RX_OK) return -1;
    r->omega.keys = &r->keys_omega;
    RxAienConfig ac;
    rx_aien_default_config(&ac);
    RxAienInputs ai = {r->omega.o.demand, r->omega.o.selection};
    if (rx_aien_create_objects(&r->aien, &r->w, &ac, &ai) != RX_OK) return -1;
    r->aien.keys = &r->keys_aien;
    if (rx_living_create(&r->living, &r->w, &r->aien, &r->omega, r->gen, r->view) != RX_OK)
        return -1;
    r->living.keys = &r->keys_living;
    if (new_object(r, 0x6135, RES_INTENT, &r->intent) != RX_OK) return -1;

    RxAegisPolicy pol;
    memset(&pol, 0, sizeof pol);
    pol.n_rules = 1;
    pol.rules[0] = (RxAegisRule){1, RX_LIVING_SEAT_SUBJ,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, RW, 0, 0};
    RxAegisClient cl = {RX_LIVING_SEAT_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT,
        RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT, RW};
    if (rx_aegis_create(&r->aegis, &r->w, r->admin, &pol, &cl, 1) != RX_OK) return -1;
    r->aegis.keys = &r->keys_aegis;
    RxAegisCaps aeg;
    memset(&aeg, 0, sizeof aeg);
    aeg.aegis_request = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
    aeg.aegis_approval = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_APPROVAL), R);
    aeg.aegis_decision = mint(r, RX_AEGIS_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), RW);
    aeg.root_request = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_REQUEST), R);
    aeg.root_decision = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_DECISION), R);
    for (uint32_t j = 0; j < RX_AEGIS_SLOTS; j++)
        aeg.root_slot[j] = mint(r, RX_AEGIS_ROOT_SUBJ, rx_aegis_res(0, RX_AEGIS_RES_SLOT0 + j), RW);
    if (rx_aegis_register(&r->aegis, &aeg) != RX_OK) return -1;
    r->stage = 3;

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
    r->stage = 4;

    RxAienCaps aien;
    memset(&aien, 0xff, sizeof aien);
    for (uint32_t i = 0; i < RX_AIEN_RES_COUNT; i++)
        aien.own[i] = mint(r, RX_AIEN_SUBJ, RX_AIEN_RES_BASE + i,
            i == RX_AIEN_RES_PLACEMENT || i == RX_AIEN_RES_GOAL ? R : RW);
    aien.demand = mint(r, RX_AIEN_SUBJ, RX_OMEGA_RES_BASE + RX_OMEGA_RES_DEMAND, R);
    aien.selection = mint(r, RX_AIEN_SUBJ, RX_OMEGA_RES_BASE + RX_OMEGA_RES_SELECTION, R);
    if (rx_aien_register(&r->aien, &aien) != RX_OK) return -1;
    RxCapRef ev = mint(r, RX_AIEN_SUBJ, RX_LIVING_RES_BASE + RX_LIVING_RES_EVIDENCE, R);
    if (rx_aien_register_experiment(&r->aien, r->living.o.evidence, ev,
            aien.own[RX_AIEN_RES_EXPERIMENT_BELIEF]) != RX_OK) return -1;
    RxCapRef plan_read = mint(r, RX_OMEGA_SUBJ_OMEGA, RX_AIEN_RES_BASE + RX_AIEN_RES_PLAN, R);
    if (rx_omega_register_reconsider(&r->omega, r->aien.o.plan, plan_read,
            omega.omega[RX_OMEGA_RES_SEARCH], omega.omega[RX_OMEGA_RES_SELECTION]) != RX_OK)
        return -1;
    r->stage = 5;

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
    r->promoter.keys = &r->keys_promoter;
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
    /* R8 acquisition takes place before the measured episode. */
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
        rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RW), rx_aegis_res(0, RX_AEGIS_RES_REQUEST), RW};
    if (rx_world_add_reaction_keyed(&r->w, &r->keys_rig, &d, &r->r_ask) != RX_OK) return -1;
    RxMutation request[6] = {
        {r->intent, 0, 1}, {r->intent, 1, RX_LIVING_RES_BASE + RX_LIVING_RES_OUTPUT},
        {r->intent, 2, RW}, {r->intent, 3, 0}, {r->intent, 4, 0},
        {r->intent, 5, RX_AEGIS_OP_ACQUIRE}};
    if (rx_world_publish_external(&r->w, r->ext_intent, request, 6) <= 0) {
        r->stage_why = "R8 acquire: intent publication refused";
        return -1;
    }
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK) {
        r->stage_why = "R8 acquire: body did not quiesce";
        return -1;
    }
    r->seat_output = (RxCapRef){field(r, lc.output_slot, 0), field(r, lc.output_slot, 1)};
    r->seat_input = lc.input_seat_read;
    if (field(r, lc.output_slot, 2) != RX_AEGIS_SLOT_LIVE) {
        r->stage_why = "R8 acquire: slot not live after quiescence";
        return -1;
    }
    r->stage = 6;
    if (rx_world_bind_capability(&r->w, r->living.o.input, r->seat_input) != RX_OK ||
        rx_world_bind_capability(&r->w, r->living.o.output, r->seat_output) != RX_OK)
        return -1;
    if (rx_world_enable_resident(&r->w) != RX_OK) return -1;
    if (rx_living_register(&r->living, &lc, &r->promoter) != RX_OK) return -1;
    RxResourceBudget budget = {0};
    budget.slots = 8;
    budget.memory_bytes = UINT64_MAX;
    budget.energy_budget = UINT64_MAX;
    budget.offered_locality = UINT32_MAX;
    budget.offered_accel = RX_ACCEL_BLACKWELL;
    budget.compute_mask = UINT32_MAX;
    rx_world_set_resources(&r->w, &budget);
    r->ext_request = mint(r, EXTERNAL, RX_OMEGA_RES_BASE + RX_OMEGA_RES_REQUEST, RX_RIGHT_WRITE);
    r->ext_goal = mint(r, EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_GOAL, RX_RIGHT_WRITE);
    r->ext_placement = mint(r, EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_PLACEMENT,
                            RX_RIGHT_WRITE);
#ifdef R15_SILICON
    if (rx_gpu_seat_begin(&r->w, &r->seat) != 0 || !r->seat) return -1;
#endif
    if (config != R15_SEQ) {
        atomic_store(&r->transport_stop, 0);
        if (pthread_create(&r->transport, NULL, transport_main, r) != 0) return -1;
        r->transport_live = 1;
    }
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK) return -1;
    take_baseline(r);
    r->stage = 7;
    return 0;
}

/* ---- production ---------------------------------------------------------- */

static uint64_t expected_digest(uint64_t seed) {
    uint64_t *A = malloc((size_t)R15_M * R15_N * sizeof(uint64_t));
    uint64_t *x = malloc((size_t)R15_N * sizeof(uint64_t));
    uint64_t *y = malloc((size_t)R15_M * sizeof(uint64_t));
    if (!A || !x || !y) { free(A); free(x); free(y); return 0; }
    rx_omega_fill(seed, A, x, R15_M, R15_N);
    omega_matvec_reference(A, x, y, R15_M, R15_N);
    uint64_t result = rx_omega_digest(y, R15_M);
    free(A); free(x); free(y);
    return result;
}

/* One production request (spec §4 W-PROD). */
static int serve(R15Rig *r) {
    uint64_t seq = ++r->request_seq;
    uint64_t seed = seq * 0x2545F4914F6CDD1Dull;
    RxMutation m[4] = {{r->omega.o.request, 0, seq}, {r->omega.o.request, 1, R15_M},
                       {r->omega.o.request, 2, R15_N}, {r->omega.o.request, 3, seed}};
    int64_t prc = rx_world_publish_external(&r->w, r->ext_request, m, 4);
    if (prc <= 0) {
        snprintf(r->serve_why, sizeof r->serve_why, "request %llu refused: %lld",
                 (unsigned long long)seq, (long long)prc);
        return -1;
    }
    uint64_t t = now_ns();
    while (field(r, r->omega.o.result, 0) != seq) {
        if (now_ns() - t > 5000000000ull) {
            /* What was the body doing when the answer did not come? */
            char busy[200] = "";
            size_t at = 0;
            pthread_mutex_lock(&r->w.mu);
            for (uint32_t i = 0; i < r->w.n_reactions && at < sizeof busy - 40; i++) {
                const RxReaction *rx = &r->w.reactions[i];
                if (rx->state != RX_DORMANT || rx->quarantined)
                    at += (size_t)snprintf(busy + at, sizeof busy - at, " %s:%s%s",
                                           rx->desc.name ? rx->desc.name : "?",
                                           rx_state_name(rx->state),
                                           rx->quarantined ? "(Q)" : "");
            }
            uint64_t serve_state = r->w.reactions[r->omega.r_serve].state;
            uint64_t serve_acts = r->w.reactions[r->omega.r_serve].activations;
            pthread_mutex_unlock(&r->w.mu);
            snprintf(r->serve_why, sizeof r->serve_why,
                     "request %llu unanswered 5 s (result seq %llu, serve %s acts %llu,"
                     " orch err %d):%s", (unsigned long long)seq,
                     (unsigned long long)field(r, r->omega.o.result, 0),
                     rx_state_name((RxState)serve_state), (unsigned long long)serve_acts,
                     atomic_load(&r->orch_error), busy);
            return -1;
        }
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

static void *producer_main(void *arg) {
    R15Rig *r = arg;
    while (!atomic_load(&r->producer_stop))
        if (serve(r) != 0) { atomic_store(&r->producer_error, 1); break; }
    return NULL;
}

static void stop_producer(R15Rig *r) {
    if (!r->producer_live) return;
    atomic_store(&r->producer_stop, 1);
    pthread_join(r->producer, NULL);
    r->producer_live = 0;
}

static int publish_placement(R15Rig *r, uint64_t cls) {
    RxMutation m[2] = {{r->aien.o.placement, 0, field(r, r->aien.o.placement, 0) + 1},
                       {r->aien.o.placement, 1, cls}};
    return rx_world_publish_external(&r->w, r->ext_placement, m, 2) > 0 ? 0 : -1;
}

static int publish_goal(R15Rig *r, uint64_t target) {
    RxMutation m[3] = {{r->aien.o.goal, 0, 1}, {r->aien.o.goal, 1, rx_omega_regime(R15_M, R15_N)},
                       {r->aien.o.goal, 2, target}};
    r->goal_crumb = rx_world_publish_external(&r->w, r->ext_goal, m, 3);
    return r->goal_crumb > 0 ? 0 : -1;
}

int r15_producer_start(R15Rig *r) {
    if (r->producer_live) return 0;
    atomic_store(&r->producer_stop, 0);
    if (pthread_create(&r->producer, NULL, producer_main, r) != 0) return -1;
    r->producer_live = 1;
    return 0;
}

int r15_producer_stop(R15Rig *r) {
    stop_producer(r);
    return atomic_load(&r->producer_error) ? -1 : 0;
}

int r15_placement(R15Rig *r, uint32_t cls) {
    return publish_placement(r, cls);
}

RxCapRef r15_mint(R15Rig *r, uint32_t subject, uint64_t resource, uint32_t rights) {
    return mint(r, subject, resource, rights);
}

int r15_move_class(uint32_t cls) {
    cpu_set_t a, x;
    if (core_sets(&a, &x) != 0) return -1;
    return move_threads(cls == R15_CLASS_A725 ? &a : &x) > 0 ? 0 : -1;
}

static int orchestration_error(R15Rig *r) {
    return atomic_load(&r->orch_error) || atomic_load(&r->transport_error);
}

/* ---- the episode ---------------------------------------------------------- */

#define EFAIL(...) do { snprintf(out->why, sizeof out->why, __VA_ARGS__); return -1; } while (0)

static void slot_snapshot(R15Rig *r, R15Outcome *out) {
    for (uint32_t k = 0; k < r->omega.cfg.n_slots && k < RX_OMEGA_SLOTS; k++) {
        out->slot_verdict[k] = field(r, r->omega.o.verdict[k], 1);
        out->slot_measure[k] = field(r, r->omega.o.measure[k], 1);
        out->slot_cps[k] = field(r, r->omega.o.measure[k], 2);
        out->slot_rps[k] = field(r, r->omega.o.measure[k], 3);
    }
}

/* How far a failed adaptation got (reported only; the episode still fails). */
static void progress_snapshot(R15Rig *r, R15Outcome *out) {
    slot_snapshot(r, out);
    out->goal_status = field(r, r->aien.o.assessment, 4);
    out->plan_action = field(r, r->aien.o.plan, 1);
    out->plan_seq = field(r, r->aien.o.plan, 0);
    out->plan_regime = field(r, r->aien.o.plan, 2);
    out->plan_condition = field(r, r->aien.o.plan, 3);
    out->plan_reason = field(r, r->aien.o.plan, 5);
    out->plan_goal = field(r, r->aien.o.plan, 6);
    out->selection_regime = field(r, r->omega.o.selection, 6);
    out->search_epoch = field(r, r->omega.o.search, 0);
    out->selection_epoch = field(r, r->omega.o.selection, 0);
    out->candidate_id = field(r, r->living.o.candidate, 0);
    out->gpu_claims = r->w.stats.resident_claims;
    out->seat_commits = r->w.reactions[r->living.r_seat].commits;
    for (uint32_t i = 0; i < 5; i++) out->evidence[i] = field(r, r->living.o.evidence, i);
    for (uint32_t i = 0; i < 4; i++) out->belief[i] = field(r, r->aien.o.experiment_belief, i);
    out->selected_ps = field(r, r->omega.o.selection, 5);
    out->reference_ps = field(r, r->omega.o.selection, 7);
    out->served = r->served;
    out->crumbs = r->w.n_crumbs;
}

int r15_episode(R15Rig *r, uint64_t target_ns, R15Outcome *out) {
    memset(out, 0, sizeof *out);
    cpu_set_t a, x;
    if (core_sets(&a, &x) != 0 || move_threads(&a) <= 0) EFAIL("needs both core classes");
    if (publish_placement(r, R15_CLASS_A725) != 0) EFAIL("placement");
    uint64_t limit = now_ns() + 60000000000ull;
    while (field(r, r->omega.o.selection, 0) < 1 && now_ns() < limit)
        if (serve(r) != 0 || orchestration_error(r)) EFAIL("serve before first selection: %s", r->serve_why);
    if (field(r, r->omega.o.selection, 0) != 1) EFAIL("no first selection");
    while (field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED && now_ns() < limit)
        if (serve(r) != 0 || orchestration_error(r)) EFAIL("serve before confirmation: %s", r->serve_why);
    if (field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED)
        EFAIL("AIEN never confirmed the incumbent cost");
    out->incumbent_ns = field(r, r->aien.o.prediction, 5);
    out->target_ns = target_ns ? target_ns : out->incumbent_ns * TARGET_PCT / 100u;
    rx_gen_active(r->gen, &out->gen_before, &out->lineage_before);
    uint64_t before_decide = r->w.reactions[r->aegis.r_decide[0]].activations;
    uint64_t before_install = r->w.reactions[r->aegis.r_install[0]].activations;

    if (r->hooks && r->hooks->idle &&
        r->hooks->idle((R15Hooks *)r->hooks, r) != 0) EFAIL("idle window");
    atomic_store(&r->producer_stop, 0);
    if (pthread_create(&r->producer, NULL, producer_main, r) != 0) EFAIL("producer");
    r->producer_live = 1;
    if (r->hooks && r->hooks->before &&
        r->hooks->before((R15Hooks *)r->hooks, r) != 0) EFAIL("BEFORE window");
    if (atomic_load(&r->producer_error)) EFAIL("serve in BEFORE: %s", r->serve_why);
    if (move_threads(&x) <= 0 || publish_placement(r, R15_CLASS_X925) != 0 ||
        publish_goal(r, out->target_ns) != 0) EFAIL("goal");
    /* 90 s (spec-era R13 value); R15_PROMOTION_TIMEOUT_MS shortens it only to
     * exercise the failure path in tests. */
    uint64_t wait_ms = 90000;
    const char *env = getenv("R15_PROMOTION_TIMEOUT_MS");
    if (env && *env) wait_ms = strtoull(env, NULL, 10);
    limit = now_ns() + wait_ms * 1000000ull;
    while (now_ns() < limit && field(r, r->living.o.promotion, 0) == 0) {
        pause_us(20);
        if (atomic_load(&r->producer_error)) EFAIL("serve during adaptation: %s", r->serve_why);
        if (orchestration_error(r)) EFAIL("orchestration error %d/%d",
            atomic_load(&r->orch_error), atomic_load(&r->transport_error));
    }
    if (field(r, r->living.o.promotion, 0) == 0 || r->promoter.result != RX_GEN_OK) {
        progress_snapshot(r, out);
        EFAIL("no promotion: result %d, prepare refusal %d", r->promoter.result,
              r->living.last_refusal);
    }
    uint64_t epoch = field(r, r->living.o.inforce, 0);
    limit = now_ns() + 60000000000ull;
    /* Wait for AIEN's decided assessment of the confirmed X925 prediction.
     * Confirming a prediction does not change its sequence (field 0), so an
     * assessment made just before the confirmation also matches field 6 and
     * legitimately reads UNKNOWN; AIEN re-assesses when the confirmation
     * lands. Clarification C4: the decisive status is the first decided one
     * (MET or UNMET*). Still failing: UNMET*, and UNKNOWN when the limit
     * expires. */
    while (now_ns() < limit && !atomic_load(&r->producer_error) &&
           !(field(r, r->aien.o.prediction, 1) == epoch &&
             field(r, r->aien.o.prediction, 6) == RX_AIEN_PRED_CONFIRMED &&
             field(r, r->aien.o.prediction, 4) == R15_CLASS_X925 &&
             field(r, r->aien.o.assessment, 6) == field(r, r->aien.o.prediction, 0) &&
             field(r, r->aien.o.assessment, 4) != RX_AIEN_GOAL_UNKNOWN))
        pause_us(100);
    /* The goal status that decides the episode is the one at goal MET; the
     * AFTER window keeps production running and AIEN keeps re-assessing, so
     * the status after it is only reported (goal_status_final). */
    uint64_t met_status = field(r, r->aien.o.assessment, 4);
    if (r->hooks && r->hooks->after && !atomic_load(&r->producer_error) &&
        met_status == RX_AIEN_GOAL_MET &&
        r->hooks->after((R15Hooks *)r->hooks, r) != 0) {
        stop_producer(r);
        EFAIL("AFTER window");
    }
    stop_producer(r);
    if (atomic_load(&r->producer_error)) EFAIL("serve after promotion: %s", r->serve_why);
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK) EFAIL("did not quiesce");
    if (orchestration_error(r)) EFAIL("orchestration error at the end");

    /* exact semantic results */
    out->goal_seq = field(r, r->aien.o.assessment, 0);
    out->goal_regime = field(r, r->aien.o.assessment, 1);
    out->goal_status_final = field(r, r->aien.o.assessment, 4);
    out->goal_status = r->hooks ? met_status : out->goal_status_final;
    out->goal_class = field(r, r->aien.o.assessment, 5);
    out->plan_action = field(r, r->aien.o.plan, 1);
    out->plan_regime = field(r, r->aien.o.plan, 2);
    out->plan_condition = field(r, r->aien.o.plan, 3);
    out->plan_reason = field(r, r->aien.o.plan, 5);
    out->plan_goal = field(r, r->aien.o.plan, 6);
    out->search_epoch = field(r, r->omega.o.search, 0);
    out->selection_epoch = field(r, r->omega.o.selection, 0);
    for (uint32_t j = 0; j < 4; j++) {
        out->selection_id[j] = field(r, r->omega.o.selection, 1 + j);
        out->inforce_id[j] = field(r, r->living.o.inforce, 1 + j);
    }
    out->selection_regime = field(r, r->omega.o.selection, 6);
    out->selected_verdict = UINT64_MAX;
    for (uint32_t k = 0; k < r->omega.cfg.n_slots; k++) {
        int same = 1;
        for (uint32_t j = 0; j < 4; j++)
            if (field(r, r->omega.o.candidate[k], 2 + j) != out->selection_id[j]) same = 0;
        if (same) out->selected_verdict = field(r, r->omega.o.verdict[k], 1);
    }
    for (uint32_t i = 0; i < 5; i++) out->evidence[i] = field(r, r->living.o.evidence, i);
    for (uint32_t i = 0; i < 4; i++) out->belief[i] = field(r, r->aien.o.experiment_belief, i);
    rx_gen_active(r->gen, &out->gen_after, &out->lineage_after);
    out->inforce_epoch = field(r, r->living.o.inforce, 0);
    out->inforce_regime = field(r, r->living.o.inforce, 6);
    out->inforce_generation = field(r, r->living.o.inforce, 7);
    out->promote_result = (int64_t)field(r, r->living.o.promotion, 1);
    out->candidate_id = field(r, r->living.o.candidate, 0);
    out->promotion_candidate = field(r, r->living.o.promotion, 0);
    out->promotion_active = field(r, r->living.o.promotion, 3);
    out->aegis_woken = r->w.reactions[r->aegis.r_decide[0]].activations - before_decide;
    out->root_woken = r->w.reactions[r->aegis.r_install[0]].activations - before_install;
    out->gpu_claims = r->w.stats.resident_claims;
    out->seat_commits = r->w.reactions[r->living.r_seat].commits;
    /* integrity */
    out->crumbs_verified = rx_world_verify_crumbs(&r->w, &out->crumbs_checked) == 0;
    out->crumb_overflow = r->w.stats.crumb_overflow;
    out->illegal = r->w.stats.illegal_transitions;
    out->wrong = r->wrong;
    out->unpromoted_use = r->unpromoted_use;
    out->lost_triggers = r15_lost_triggers(r, NULL, 0);
    /* measured */
    out->selected_ps = field(r, r->omega.o.selection, 5);
    out->reference_ps = field(r, r->omega.o.selection, 7);
    out->inforce_ps = field(r, r->living.o.inforce, 5);
    out->final_expected_ns = field(r, r->aien.o.assessment, 3);
    out->plan_seq = field(r, r->aien.o.plan, 0);
    out->served = r->served;
    out->crumbs = r->w.n_crumbs;
    slot_snapshot(r, out);
    out->ok = out->goal_status == RX_AIEN_GOAL_MET;
    if (!out->ok) EFAIL("goal not MET (status %llu)", (unsigned long long)out->goal_status);
    return 0;
}

uint64_t r15_lost_triggers(R15Rig *r, char *first, size_t n) {
    static uint64_t last_read[RX_MAX_REACTIONS][RX_MAX_DEPS];
    uint64_t lost = 0;
    pthread_mutex_lock(&r->w.mu);
    memset(last_read, 0, sizeof last_read);
    uint32_t nr = r->w.n_reactions;
    for (uint64_t id = 1; id <= r->w.n_crumbs; id++) {
        const RxCrumb *k = &r->w.crumbs[id - 1];
        if (k->reaction >= nr) continue;
        const RxReactionDesc *d = &r->w.reactions[k->reaction].desc;
        for (uint32_t i = 0; i < k->n_inputs; i++)
            for (uint32_t t = 0; t < d->n_triggers; t++)
                if (k->inputs[i].obj.id == d->triggers[t].obj.id &&
                    k->inputs[i].obj.generation == d->triggers[t].obj.generation &&
                    k->inputs[i].version > last_read[k->reaction][t])
                    last_read[k->reaction][t] = k->inputs[i].version;
    }
    for (uint32_t i = 0; i < nr; i++) {
        const RxReaction *rx = &r->w.reactions[i];
        if (rx->quarantined) continue;
        for (uint32_t t = 0; t < rx->desc.n_triggers; t++) {
            const RxDep *dep = &rx->desc.triggers[t];
            const RxObject *o = &r->w.objects[dep->obj.id];
            if (!o->live || o->generation != dep->obj.generation) continue;
            uint64_t cur = 0;
            for (uint32_t f = 0; f < RX_MAX_FIELDS; f++)
                if ((dep->mask & RX_FIELD(f)) && o->field_version[f] > cur)
                    cur = o->field_version[f];
            if (cur > r->baseline[i][t] && cur > last_read[i][t]) {
                if (!lost && first)
                    snprintf(first, n, "%s trigger %u: object %u at version %llu, last read %llu",
                             rx->desc.name ? rx->desc.name : "?", t, dep->obj.id,
                             (unsigned long long)cur, (unsigned long long)last_read[i][t]);
                lost++;
            }
        }
    }
    pthread_mutex_unlock(&r->w.mu);
    return lost;
}

void r15_stop(R15Rig *r) {
    R15StageClock *sc = &r->stop_clock;
    memset(sc, 0, sizeof *sc);
    r15_stage_mark(sc, "begin");
    stop_producer(r);
    r15_stage_mark(sc, "producer");
    if (r->transport_live) {
        atomic_store(&r->transport_stop, 1);
        pthread_join(r->transport, NULL);
        r->transport_live = 0;
    }
    r15_stage_mark(sc, "transport");
    if (r->orch_live) {
        atomic_store(&r->orch_stop, 1);
        pthread_join(r->orchestrator, NULL);
        r->orch_live = 0;
    }
    r15_stage_mark(sc, "orchestrator");
#ifdef R15_SILICON
    if (r->seat) {
        (void)rx_resident_shutdown(&r->w);
        r15_stage_mark(sc, "seat_shutdown_post");
        (void)rx_gpu_seat_finish(r->seat);
        r15_stage_mark(sc, "seat_finish");
    }
#endif
    rx_world_destroy(&r->w);
    r15_stage_mark(sc, "world_destroy");
    rx_aegis_destroy(&r->aegis);
    rx_omega_destroy(&r->omega);
    if (r->gen) rx_gen_close(r->gen);
    if (r->admin) aienos_cap_stop(r->admin, r->view);
    r15_stage_mark(sc, "faculties_admin");
    if (r->generation_dir[0]) {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", r->generation_dir);
        if (system(cmd) != 0) fprintf(stderr, "note: could not remove %s\n", r->generation_dir);
    }
    r15_stage_mark(sc, "generation_dir");
}
