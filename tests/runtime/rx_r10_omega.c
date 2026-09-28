/*
 * R10: Omega as a continuous realization faculty (ADR 0016 §43).
 *
 * The test only publishes outside requests and reads the world. It never
 * calls an Omega function to obtain a realization. Everything between
 * "the operation is getting hot" and "production runs the new realization"
 * has to happen by reactions becoming ready:
 *
 *   request -> serve -> demand window -> omega.watch -> search
 *     -> omega.synthesize.k -> candidate -> omega.verify.k -> verdict
 *     -> omega.measure.k -> measure -> omega.select -> selection
 *     -> (next request) serve reads selection
 *
 * Every result is checked against the semantic reference computed here.
 * Authority is the native AIENOS capability authority.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_omega.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { SUBJ_EXTERNAL = 100, ISSUER = 3 };

#define HOT_M 64u
#define HOT_N 256u   /* a multiple of 4: the remainder defect is right on this shape */
#define OMEGA_KINDS (OMEGA_MATVEC_KIND_QUAD4 + 1u)
#define DEFECT_SLOT OMEGA_KINDS

static int g_checks;
static int g_fail;

#define CHECK(cond, ...) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);        \
            fprintf(stderr, __VA_ARGS__);                                \
            fputc('\n', stderr);                                         \
        }                                                                \
    } while (0)

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_us(int us) {
    struct timespec ts = { 0, (long)us * 1000L };
    nanosleep(&ts, NULL);
}

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    RxOmegaFaculty f;
    RxOmegaCaps caps;
    RxCapRef ext;
    uint64_t seq;
    uint64_t served;
    uint64_t wrong;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = { ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    AienosCapRef r = { UINT32_MAX, 0 };
    if (aienos_cap_mint(e->admin, &m, &r) != 0) r = (AienosCapRef){ UINT32_MAX, 0 };
    return (RxCapRef){ r.cap_id, r.generation };
}

static int revoke_cap(Env *e, RxCapRef cap) {
    AienosCapRef office;
    aienos_cap_office(e->admin, &office);
    return aienos_cap_revoke(e->admin, office, (AienosCapRef){ cap.cap_id, cap.generation });
}

static int env_start(Env *e, const RxOmegaConfig *cfg) {
    memset(e, 0, sizeof(*e));
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    if (rx_world_init_native(&e->w, e->view, 4, 1u << 18) != RX_OK) {
        aienos_cap_stop(e->admin, e->view);
        return -1;
    }
    e->w.external_subject = SUBJ_EXTERNAL;
    if (rx_omega_create_objects(&e->f, &e->w, cfg) != RX_OK) return -1;
    const uint32_t RW = RX_RIGHT_READ | RX_RIGHT_WRITE;
    for (uint32_t i = 0; i < RX_OMEGA_RES_COUNT; i++) {
        e->caps.serve[i] = (RxCapRef){ UINT32_MAX, 0 };
        e->caps.omega[i] = (RxCapRef){ UINT32_MAX, 0 };
    }
    uint64_t B = RX_OMEGA_RES_BASE;
    e->caps.serve[RX_OMEGA_RES_REQUEST] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_REQUEST, RX_RIGHT_READ);
    e->caps.serve[RX_OMEGA_RES_SELECTION] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_SELECTION, RX_RIGHT_READ);
    e->caps.serve[RX_OMEGA_RES_DEMAND] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_DEMAND, RW);
    e->caps.serve[RX_OMEGA_RES_RESULT] = mint(e, RX_OMEGA_SUBJ_SERVE, B + RX_OMEGA_RES_RESULT, RW);
    /* Omega observes demand; it does not write production's objects. */
    e->caps.omega[RX_OMEGA_RES_DEMAND] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + RX_OMEGA_RES_DEMAND, RX_RIGHT_READ);
    e->caps.omega[RX_OMEGA_RES_SEARCH] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + RX_OMEGA_RES_SEARCH, RW);
    e->caps.omega[RX_OMEGA_RES_SELECTION] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + RX_OMEGA_RES_SELECTION, RW);
    for (uint32_t k = 0; k < e->f.cfg.n_slots; k++) {
        uint32_t c = RX_OMEGA_RES_CANDIDATE0 + k, v = RX_OMEGA_RES_VERDICT0 + k,
                 m = RX_OMEGA_RES_MEASURE0 + k;
        e->caps.omega[c] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + c, RW);
        e->caps.omega[v] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + v, RW);
        e->caps.omega[m] = mint(e, RX_OMEGA_SUBJ_OMEGA, B + m, RW);
    }
    e->ext = mint(e, SUBJ_EXTERNAL, B + RX_OMEGA_RES_REQUEST, RX_RIGHT_WRITE);
    return rx_omega_register(&e->f, &e->caps) == RX_OK ? 0 : -1;
}

static void env_stop(Env *e) {
    rx_world_wait_quiescent(&e->w, 10000);
    rx_world_destroy(&e->w);
    rx_omega_destroy(&e->f);
    aienos_cap_stop(e->admin, e->view);
}

static uint64_t fld(Env *e, RxObjRef r, uint32_t i) {
    RxObject o;
    if (rx_world_read(&e->w, r, &o) != RX_OK) return UINT64_MAX;
    return o.field[i];
}

static uint64_t expected_digest(uint64_t seed, uint32_t M, uint32_t N) {
    uint64_t *A = malloc((size_t)M * N * sizeof(uint64_t));
    uint64_t *x = malloc((size_t)N * sizeof(uint64_t));
    uint64_t *y = malloc((size_t)M * sizeof(uint64_t));
    rx_omega_fill(seed, A, x, M, N);
    omega_matvec_reference(A, x, y, M, N);
    uint64_t d = rx_omega_digest(y, M);
    free(A); free(x); free(y);
    return d;
}

/* One outside request. Waits for its result the way any client would; the
 * world itself never waits. Returns the realization word it was served by. */
static uint64_t request(Env *e, uint32_t M, uint32_t N) {
    uint64_t seq = ++e->seq;
    uint64_t seed = seq * 0x2545F4914F6CDD1Dull;
    RxMutation m[4] = {
        { e->f.o.request, 0, seq }, { e->f.o.request, 1, M },
        { e->f.o.request, 2, N },   { e->f.o.request, 3, seed } };
    int64_t pub = rx_world_publish_external(&e->w, e->ext, m, 4);
    if (pub <= 0) {
        fprintf(stderr, "  request %llu: publish refused (%lld)\n", (unsigned long long)seq,
                (long long)pub);
        e->wrong++;
        return UINT64_MAX;
    }
    uint64_t t0 = now_ns();
    while (fld(e, e->f.o.result, 0) != seq) {
        if (now_ns() - t0 > 5000000000ull) {
            fprintf(stderr, "  request %llu: no result (serve state %s, result seq %llu)\n",
                    (unsigned long long)seq,
                    rx_state_name(e->w.reactions[e->f.r_serve].state),
                    (unsigned long long)fld(e, e->f.o.result, 0));
            for (uint64_t id = e->w.n_crumbs > 8 ? e->w.n_crumbs - 8 : 1; id <= e->w.n_crumbs; id++) {
                const RxCrumb *c = rx_world_crumb(&e->w, id);
                if (!c) continue;
                fprintf(stderr, "    crumb %llu %s %s reason %d cause %llu out",
                        (unsigned long long)id, rx_crumb_kind_name(c->kind),
                        c->reaction < e->w.n_reactions ? e->w.reactions[c->reaction].desc.name
                                                       : "external",
                        c->reason, (unsigned long long)c->wake_cause);
                for (uint32_t i = 0; i < c->n_outputs; i++)
                    fprintf(stderr, " %u:%llx", c->outputs[i].obj.id,
                            (unsigned long long)c->outputs[i].mask);
                fprintf(stderr, " in");
                for (uint32_t i = 0; i < c->n_inputs; i++)
                    fprintf(stderr, " %u@%llu", c->inputs[i].obj.id,
                            (unsigned long long)c->inputs[i].version);
                fputc('\n', stderr);
            }
            fprintf(stderr, "    objects: request %u demand %u result %u selection %u\n",
                    e->f.o.request.id, e->f.o.demand.id, e->f.o.result.id, e->f.o.selection.id);
            e->wrong++;
            return UINT64_MAX;
        }
        sleep_us(20);
    }
    RxObject r;
    rx_world_read(&e->w, e->f.o.result, &r);
    e->served++;
    if (r.field[1] != expected_digest(seed, M, N)) {
        fprintf(stderr, "  request %llu: wrong digest from realization %llx\n",
                (unsigned long long)seq, (unsigned long long)r.field[2]);
        e->wrong++;
    }
    return r.field[2];
}

/* Serve requests until the selection records `epoch`, at most `limit`. */
static uint64_t serve_until_selected(Env *e, uint32_t M, uint32_t N, uint64_t epoch,
                                     uint64_t limit) {
    uint64_t n = 0;
    while (fld(e, e->f.o.selection, 0) < epoch && n < limit) {
        request(e, M, N);
        n++;
    }
    return n;
}

static const char *rname(Env *e, uint32_t rid) {
    if (rid == UINT32_MAX || rid >= e->w.n_reactions) return "external";
    return e->w.reactions[rid].desc.name;
}

/* Commit crumbs of one reaction, in causal id order. */
static uint64_t first_commit(Env *e, uint32_t rid, uint64_t after) {
    for (uint64_t id = after + 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (c && c->kind == RX_CRUMB_COMMIT && c->reaction == rid) return id;
    }
    return 0;
}

static uint64_t count_commits(Env *e, uint32_t rid, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (uint64_t id = lo + 1; id < hi; id++) {
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (c && c->kind == RX_CRUMB_COMMIT && c->reaction == rid) n++;
    }
    return n;
}

static uint64_t count_kind(Env *e, uint32_t rid, RxCrumbKind kind) {
    uint64_t n = 0;
    for (uint64_t id = 1; id <= e->w.n_crumbs; id++) {
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (c && c->kind == kind && c->reaction == rid) n++;
    }
    return n;
}

/* Walk every ancestor of a crumb; note which reactions and kinds appear. */
typedef struct {
    int external, serve, watch, synth, verify, measure;
} Lineage;

static void lineage(Env *e, uint64_t from, Lineage *out) {
    memset(out, 0, sizeof(*out));
    uint64_t n = e->w.n_crumbs;
    uint8_t *seen = calloc(n + 1, 1);
    uint64_t *stack = malloc((n + 1) * sizeof(uint64_t));
    uint64_t sp = 0;
    stack[sp++] = from;
    while (sp) {
        uint64_t id = stack[--sp];
        if (id == 0 || id > n || seen[id]) continue;
        seen[id] = 1;
        const RxCrumb *c = rx_world_crumb(&e->w, id);
        if (!c) continue;
        if (c->kind == RX_CRUMB_EXTERNAL) out->external = 1;
        if (c->kind == RX_CRUMB_COMMIT) {
            const char *nm = rname(e, c->reaction);
            if (!strcmp(nm, "workload.matvec.serve")) out->serve = 1;
            if (!strcmp(nm, "omega.watch")) out->watch = 1;
            if (!strncmp(nm, "omega.synthesize.", 17)) out->synth = 1;
            if (!strncmp(nm, "omega.verify.", 13)) out->verify = 1;
            if (!strncmp(nm, "omega.measure.", 14)) out->measure = 1;
        }
        for (uint32_t i = 0; i < c->n_parents && sp < n; i++) stack[sp++] = c->parents[i];
        if (c->wake_cause && sp < n) stack[sp++] = c->wake_cause;
    }
    free(seen);
    free(stack);
}

static RxOmegaConfig test_config(RxOmegaDefect defect) {
    RxOmegaConfig cfg;
    rx_omega_default_config(&cfg);
    cfg.defect = defect;
    cfg.hot_calls = 64;
    cfg.hot_ns = 200000;
    return cfg;
}

/* ---- receipt data from the main run ---- */
typedef struct {
    uint64_t kind, state, cps, rps, checks, fails, why;
} SlotRow;
static SlotRow g_rows[RX_OMEGA_SLOTS];
static uint64_t g_sel_ps, g_sel_ref_ps, g_sel_kind = UINT64_MAX, g_during, g_total, g_hot_after;
static uint64_t g_search_calls, g_search_ns;
static int g_won, g_allow_no_win;
static char g_cpus[256] = "all";
static unsigned g_cpu_part;

/* The DGX Spark mixes Cortex-X925 (part 0xd85) and Cortex-A725 (0xd87)
 * cores, and the fastest realization differs between them. Measurement and
 * production must see the same core class, so when X925 cores exist the
 * whole run is placed on them. Other hosts run where the scheduler puts them. */
static void place_on_performance_cores(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    cpu_set_t set;
    CPU_ZERO(&set);
    int count = 0;
    size_t used = 0;
    g_cpus[0] = 0;
    for (long c = 0; c < n && c < CPU_SETSIZE; c++) {
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/regs/identification/midr_el1", c);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        unsigned long long midr = 0;
        int ok = fscanf(fp, "%llx", &midr) == 1;
        fclose(fp);
        unsigned part = (unsigned)((midr >> 4) & 0xfffu);
        if (!ok || part != 0xd85u) continue;
        CPU_SET((int)c, &set);
        count++;
        g_cpu_part = part;
        used += (size_t)snprintf(g_cpus + used, sizeof g_cpus - used, "%s%ld", used ? "," : "", c);
        if (used >= sizeof g_cpus) break;
    }
    if (count == 0 || sched_setaffinity(0, sizeof set, &set) != 0) {
        snprintf(g_cpus, sizeof g_cpus, "all");
        g_cpu_part = 0;
        return;
    }
    printf("[*] placed on %d Cortex-X925 cores: %s\n", count, g_cpus);
}

/* ---- tests ---- */

static void t_cold_then_hot(void) {
    printf("[*] cold demand leaves Omega idle; hot demand runs the whole chain unasked\n");
    Env e;
    RxOmegaConfig cfg = test_config(RX_OMEGA_DEFECT_NONE);
    CHECK(env_start(&e, &cfg) == 0, "setup");
    RxOmegaFaculty *f = &e.f;

    /* Cold: fewer calls than the threshold. */
    for (int i = 0; i < 40; i++) request(&e, HOT_M, HOT_N);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "cold world did not settle");
    CHECK(fld(&e, f->o.search, 0) == 0, "a cold operation started a search");
    CHECK(e.w.reactions[f->r_synth[0]].activations == 0, "synthesis ran without demand");
    CHECK(fld(&e, f->o.demand, 0) == 40 && fld(&e, f->o.demand, 2) == 2,
          "demand evidence: calls %llu windows %llu",
          (unsigned long long)fld(&e, f->o.demand, 0), (unsigned long long)fld(&e, f->o.demand, 2));
    CHECK(e.w.reactions[f->r_watch].activations == 2, "watch should wake once per window, woke %llu",
          (unsigned long long)e.w.reactions[f->r_watch].activations);
    CHECK(f->served_realized == 0, "a realization served before any existed");

    /* Hot: keep the production path busy. Nothing asks Omega for anything. */
    uint64_t n = serve_until_selected(&e, HOT_M, HOT_N, 1, 200000);
    CHECK(fld(&e, f->o.selection, 0) == 1, "no selection after %llu requests", (unsigned long long)n);
    CHECK(fld(&e, f->o.search, 0) == 1, "search epochs %llu", (unsigned long long)fld(&e, f->o.search, 0));
    g_search_calls = fld(&e, f->o.search, 4);
    g_search_ns = fld(&e, f->o.search, 5);
    CHECK(g_search_calls >= cfg.hot_calls && g_search_ns >= cfg.hot_ns, "search fired below threshold");

    uint64_t c_search = first_commit(&e, f->r_watch, 0);
    uint64_t c_select = first_commit(&e, f->r_select, 0);
    CHECK(c_search && c_select && c_search < c_select, "search/select commit order");
    g_during = count_commits(&e, f->r_serve, c_search, c_select);
    CHECK(g_during > 0, "production stopped while Omega optimized");

    /* Every candidate took the whole path once. */
    for (uint32_t k = 0; k < f->cfg.n_slots; k++) {
        RxObject v, m, c;
        rx_world_read(&e.w, f->o.candidate[k], &c);
        rx_world_read(&e.w, f->o.verdict[k], &v);
        rx_world_read(&e.w, f->o.measure[k], &m);
        CHECK(c.field[0] == 1 && c.field[7] == RX_OMEGA_SYNTHESIZED, "candidate %u", k);
        CHECK(v.field[0] == 1 && v.field[1] == RX_OMEGA_PASSED, "verdict %u state %llu why %llu", k,
              (unsigned long long)v.field[1], (unsigned long long)v.field[5]);
        CHECK(v.field[2] > 250 && v.field[3] == 0, "verdict %u checks %llu fails %llu", k,
              (unsigned long long)v.field[2], (unsigned long long)v.field[3]);
        CHECK(m.field[0] == 1 && m.field[1] == RX_OMEGA_MEASURED && m.field[2] && m.field[3],
              "measure %u", k);
        CHECK(v.field[4] == c.field[2] && m.field[4] == c.field[2], "slot %u names one identity", k);
        g_rows[k] = (SlotRow){ c.field[1], m.field[1], m.field[2], m.field[3], v.field[2],
                               v.field[3], v.field[5] };
        CHECK(e.w.reactions[f->r_synth[k]].commits == 1, "synth %u commits", k);
        CHECK(e.w.reactions[f->r_verify[k]].commits == 1, "verify %u commits", k);
        CHECK(e.w.reactions[f->r_measure[k]].commits == 1, "measure %u commits", k);
    }

    /* The record is the rule applied to the measurements, nothing else. */
    RxObject sel;
    rx_world_read(&e.w, f->o.selection, &sel);
    uint64_t want = 0, want_ps = UINT64_MAX;
    for (uint32_t k = 0; k < f->cfg.n_slots; k++) {
        RxObject m;
        rx_world_read(&e.w, f->o.measure[k], &m);
        if (m.field[2] * 100u <= m.field[3] * (100u - cfg.margin_pct) && m.field[2] < want_ps) {
            want_ps = m.field[2];
            want = m.field[4];
            g_sel_kind = g_rows[k].kind;
        }
    }
    CHECK(sel.field[1] == want, "selection is not the fastest verified candidate");
    CHECK(sel.field[6] == rx_omega_regime(HOT_M, HOT_N), "selection regime");
    g_sel_ps = sel.field[5];
    g_sel_ref_ps = sel.field[7];
    g_won = want != 0;
    if (!g_allow_no_win)
        CHECK(want != 0, "no Omega realization beat the reference on this machine "
                         "(nothing for production to pick up)");

    /* The causal record of the selection reaches back to the outside request. */
    Lineage L;
    lineage(&e, rx_world_explain(&e.w, f->o.selection, 0), &L);
    CHECK(L.external && L.serve && L.watch && L.synth && L.verify && L.measure,
          "selection lineage: ext %d serve %d watch %d synth %d verify %d measure %d",
          L.external, L.serve, L.watch, L.synth, L.verify, L.measure);

    /* Production picks the record up on its own next requests. */
    uint64_t before = f->served_realized;
    for (int i = 0; i < 64; i++) {
        uint64_t used = request(&e, HOT_M, HOT_N);
        if (i > 0) CHECK(used == want, "request %d served by %llx, selection %llx", i,
                         (unsigned long long)used, (unsigned long long)want);
    }
    g_hot_after = f->served_realized - before;
    if (want) {
        CHECK(g_hot_after >= 63, "realized requests after selection %llu",
              (unsigned long long)g_hot_after);
        RxOmegaRealization rr;
        CHECK(rx_omega_store_find(f, want, &rr) == 0 && rr.verified,
              "selected realization not verified");
    } else {
        CHECK(g_hot_after == 0, "a realization served although the reference was retained");
    }

    /* Omega stays settled while the regime is unchanged. */
    for (int i = 0; i < 64; i++) request(&e, HOT_M, HOT_N);
    CHECK(rx_world_wait_quiescent(&e.w, 5000) == RX_OK, "world did not settle");
    CHECK(fld(&e, f->o.search, 0) == 1, "a settled regime searched again");

    /* A new shape is new demand: a second search, by the same reactions. */
    printf("[*] a new regime becomes hot and gets its own record\n");
    uint64_t n2 = serve_until_selected(&e, 16, 1021, 2, 200000);
    CHECK(fld(&e, f->o.search, 0) == 2 && fld(&e, f->o.selection, 0) == 2,
          "second regime: search %llu selection %llu after %llu requests",
          (unsigned long long)fld(&e, f->o.search, 0),
          (unsigned long long)fld(&e, f->o.selection, 0), (unsigned long long)n2);
    CHECK(fld(&e, f->o.selection, 6) == rx_omega_regime(16, 1021), "second selection regime");
    uint64_t sel2 = fld(&e, f->o.selection, 1);
    for (int i = 0; i < 16; i++) {
        uint64_t used = request(&e, 16, 1021);
        if (i > 0) CHECK(used == sel2, "second regime not served by its record");
    }

    CHECK(rx_world_wait_quiescent(&e.w, 10000) == RX_OK, "final settle");
    CHECK(e.wrong == 0, "%llu wrong or lost results of %llu", (unsigned long long)e.wrong,
          (unsigned long long)e.served);
    g_total = e.served;
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&e.w, &checked) == 0 && checked > 0, "crumb chain");
    CHECK(e.w.stats.illegal_transitions == 0, "illegal lifecycle");
    CHECK(e.w.stats.failed == 0 && e.w.stats.blocked_authority == 0 && e.w.stats.rejected == 0,
          "failed %llu blocked %llu rejected %llu", (unsigned long long)e.w.stats.failed,
          (unsigned long long)e.w.stats.blocked_authority, (unsigned long long)e.w.stats.rejected);
    env_stop(&e);
}

/* Run the defect's code at the hot shape in a throwaway child: it passes. */
static int defect_right_on_hot_shape(const RxOmegaRealization *r) {
    pid_t pid = fork();
    if (pid == 0) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        memcpy(p, r->code, r->code_len);
        __builtin___clear_cache((char *)p, (char *)p + r->code_len);
        mprotect(p, 4096, PROT_READ | PROT_EXEC);
        uint64_t *A = malloc(HOT_M * HOT_N * 8), *x = malloc(HOT_N * 8), *y = malloc(HOT_M * 8),
                 *yr = malloc(HOT_M * 8);
        rx_omega_fill(7, A, x, HOT_M, HOT_N);
        omega_matvec_reference(A, x, yr, HOT_M, HOT_N);
        union { void *p; void (*fn)(const uint64_t *, const uint64_t *, uint64_t *, uint64_t, uint64_t); } u;
        u.p = p;
        u.fn(A, x, y, HOT_M, HOT_N);
        _exit(memcmp(y, yr, HOT_M * 8) == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void t_defect(RxOmegaDefect d, uint32_t expect_why, const char *label) {
    printf("[*] planted candidate (%s) is refused, never run by the world, never selected\n", label);
    Env e;
    RxOmegaConfig cfg = test_config(d);
    CHECK(env_start(&e, &cfg) == 0, "setup");
    RxOmegaFaculty *f = &e.f;
    CHECK(f->cfg.n_slots == RX_OMEGA_SLOTS, "defect slot missing");
    uint64_t crashes0 = f->sandbox_crashes;
    serve_until_selected(&e, HOT_M, HOT_N, 1, 200000);
    CHECK(rx_world_wait_quiescent(&e.w, 10000) == RX_OK, "world did not settle");
    CHECK(fld(&e, f->o.selection, 0) == 1, "no selection");
    RxObject c, v, m;
    rx_world_read(&e.w, f->o.candidate[DEFECT_SLOT], &c);
    rx_world_read(&e.w, f->o.verdict[DEFECT_SLOT], &v);
    rx_world_read(&e.w, f->o.measure[DEFECT_SLOT], &m);
    CHECK(c.field[0] == 1 && c.field[1] == DEFECT_SLOT, "defect candidate was not synthesized");
    CHECK(v.field[1] == RX_OMEGA_REFUSED && v.field[5] == expect_why,
          "defect verdict state %llu why %llu (want %u)", (unsigned long long)v.field[1],
          (unsigned long long)v.field[5], expect_why);
    CHECK(m.field[1] == RX_OMEGA_DECLINED && m.field[2] == 0, "a refused candidate was measured");
    CHECK(fld(&e, f->o.selection, 1) != c.field[2], "a refused candidate was selected");
    RxOmegaRealization r;
    CHECK(rx_omega_store_find(f, c.field[2], &r) == 0, "defect not in the store");
    CHECK(!r.verified && r.parent_runs == 0, "the world ran refused code (runs %llu)",
          (unsigned long long)r.parent_runs);
    if (d == RX_OMEGA_DEFECT_SKIP_REMAINDER) {
        CHECK(v.field[3] > 0, "differential found no failures");
        CHECK(defect_right_on_hot_shape(&r), "the defect was supposed to pass on the hot shape");
    }
    if (d == RX_OMEGA_DEFECT_CRASH) CHECK(f->sandbox_crashes > crashes0, "no sandbox crash counted");
    for (uint32_t k = 0; k < DEFECT_SLOT; k++)
        CHECK(fld(&e, f->o.verdict[k], 1) == RX_OMEGA_PASSED, "honest slot %u refused", k);
    for (int i = 0; i < 8; i++) request(&e, HOT_M, HOT_N);
    CHECK(e.wrong == 0, "%llu wrong results", (unsigned long long)e.wrong);
    CHECK(e.w.stats.illegal_transitions == 0, "illegal lifecycle");
    env_stop(&e);
}

static void t_authority(void) {
    printf("[*] Omega without authority is blocked; production is not\n");
    Env e;
    RxOmegaConfig cfg = test_config(RX_OMEGA_DEFECT_NONE);
    CHECK(env_start(&e, &cfg) == 0, "setup");
    RxOmegaFaculty *f = &e.f;
    CHECK(revoke_cap(&e, e.caps.omega[RX_OMEGA_RES_CANDIDATE0]) == 0, "revoke");
    uint64_t n = 0;
    while (fld(&e, f->o.search, 0) == 0 && n < 200000) { request(&e, HOT_M, HOT_N); n++; }
    for (int i = 0; i < 200; i++) request(&e, HOT_M, HOT_N);
    CHECK(rx_world_wait_quiescent(&e.w, 10000) == RX_OK, "world did not settle");
    CHECK(fld(&e, f->o.search, 0) == 1, "search was not published");
    CHECK(count_kind(&e, f->r_synth[0], RX_CRUMB_BLOCKED_AUTHORITY) >= 1,
          "revoked synthesis was not blocked by the authority");
    CHECK(fld(&e, f->o.candidate[0], 0) == 0, "candidate written without authority");
    CHECK(e.w.reactions[f->r_synth[0]].commits == 0, "revoked synthesis committed");
    CHECK(fld(&e, f->o.selection, 0) == 0, "selection recorded from an incomplete search");
    CHECK(f->served_realized == 0 && e.wrong == 0 && e.served == n + 200,
          "production: realized %llu wrong %llu served %llu",
          (unsigned long long)f->served_realized, (unsigned long long)e.wrong,
          (unsigned long long)e.served);
    CHECK(e.w.reactions[f->r_serve].commits == e.served, "production commits");
    env_stop(&e);
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
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static const char *kind_name(uint64_t k) {
    switch (k) {
    case 0: return "matvec_scalar";
    case 1: return "matvec_unroll2";
    case 2: return "matvec_unroll4_dual";
    case 3: return "matvec_quad4";
    case UINT64_MAX: return "reference";
    }
    return "defect";
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("R10/rx_omega_faculty_receipt.json", path, sizeof path) != 0) return;
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    const char *aienos = getenv("AIENOS_COMMIT");
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    const char *gate = g_fail ? "FAIL" : g_won ? "PASS" : "CHAIN_PASS_NO_WIN";
    fprintf(fp,
            "{\n"
            "  \"schema\": \"AIEN_RX_R10_OMEGA_FACULTY_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"aienos_commit\": %s%s%s,\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"cpus\": {\"placed_on\": \"%s\", \"midr_part\": \"0x%03x\"},\n"
            "  \"hardware_scope\": \"host processor only: Omega's native AArch64 matvec realizations "
            "run on this CPU; no graphics processor is used\",\n"
            "  \"operation\": \"omega_matvec (M12), uint64 wrap-around, regime %ux%u\",\n"
            "  \"threshold\": {\"calls_at_search\": %llu, \"ns_at_search\": %llu},\n"
            "  \"requests_served_during_optimization\": %llu,\n"
            "  \"requests_total\": %llu,\n"
            "  \"requests_on_realization_after_selection\": %llu,\n"
            "  \"candidates\": [\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "",
            aienos ? aienos : "null", aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname,
            u.release, u.machine, g_cpus, g_cpu_part, HOT_M, HOT_N, (unsigned long long)g_search_calls,
            (unsigned long long)g_search_ns, (unsigned long long)g_during,
            (unsigned long long)g_total, (unsigned long long)g_hot_after);
    for (uint32_t k = 0; k < OMEGA_KINDS; k++)
        fprintf(fp,
                "    {\"kind\": \"%s\", \"verify_checks\": %llu, \"verify_failures\": %llu, "
                "\"candidate_ps_per_call\": %llu, \"reference_ps_per_call\": %llu}%s\n",
                kind_name(g_rows[k].kind), (unsigned long long)g_rows[k].checks,
                (unsigned long long)g_rows[k].fails, (unsigned long long)g_rows[k].cps,
                (unsigned long long)g_rows[k].rps, k + 1 < OMEGA_KINDS ? "," : "");
    fprintf(fp,
            "  ],\n"
            "  \"selected\": {\"kind\": \"%s\", \"ps_per_call\": %llu, \"reference_ps_per_call\": %llu},\n"
            "  \"gates\": {\n"
            "    \"R10_CONTINUOUS_OMEGA\": \"%s\",\n"
            "    \"production_adopted_realization\": %s,\n"
            "    \"not_claimed\": [\"R8\", \"R11\", \"R13\", \"R10 on the graphics processor\"]\n"
            "  }\n"
            "}\n",
            kind_name(g_sel_kind), (unsigned long long)g_sel_ps, (unsigned long long)g_sel_ref_ps,
            gate, g_won ? "true" : "false");
    fclose(fp);
    printf("receipt: %s\n", path);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
#if !defined(__aarch64__)
    fprintf(stderr, "R10 runs Omega's AArch64 realizations; this host cannot execute them\n");
    return 2;
#endif
    const char *nw = getenv("R10_ALLOW_NO_WIN");
    g_allow_no_win = nw && nw[0] == '1';
    place_on_performance_cores();
    t_cold_then_hot();
    t_defect(RX_OMEGA_DEFECT_SKIP_REMAINDER, RX_OMEGA_WHY_DIFFERENTIAL, "drops N mod 4");
    t_defect(RX_OMEGA_DEFECT_CRASH, RX_OMEGA_WHY_CRASHED, "faults on first load");
    t_defect(RX_OMEGA_DEFECT_TAMPER, RX_OMEGA_WHY_IDENTITY, "bytes differ from its identity");
    t_authority();
    printf("checks %d failures %d\n", g_checks, g_fail);
    write_receipt();
    return g_fail ? 1 : 0;
}
