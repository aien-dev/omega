/*
 * rx_sem_incremental.c -- qualification of Omega semantic variables and
 * incremental recomputation (gate OMEGA_INCREMENTAL_SEMANTICS_PASS).
 *
 * PROVE, against the resident world (rx_world) as the source of truth:
 *   dependency_tracking     declared inputs, reverse index, transitive downstream
 *   correct_invalidation    temperature dirties thermal placement (and what reads
 *                           it) only; repository analysis, memory retrieval and
 *                           capability resolution stay clean; unrelated world
 *                           objects dirty nothing
 *   no_stale_reuse          randomized differential run against a from-scratch
 *                           evaluator, every hit re-computed (audit); 0 mismatches
 *   generation_isolation    object retired and re-created with the same bytes, and
 *                           a promoted function generation, are never served old results
 *   branch_isolation        J-Space candidate writes never reach the parent, a
 *                           sibling, or the world
 *   cross_branch_reuse      same inputs across branch, agent, task and candidate
 *                           reuse one result
 *   causal_provenance       every entry and reuse event re-verified; a reused
 *                           value explains back to its producer and to the world
 *                           crumbs of its inputs; tampering is detected
 *
 * MEASURE, one deterministic workload replayed four ways:
 *   FULL    recompute every derived value on every query (no incremental state)
 *   OBJECT  memo + early cutoff, object-granular invalidation
 *   FIELD   memo + early cutoff, field-granular invalidation
 *   ENGINE  FIELD + the shared content-addressed cache
 * recomputation avoided, compute saved, latency saved, energy saved (package
 * energy counter, when the machine exposes one), cache-hit rate, false-hit
 * rate, unnecessary invalidation rate.
 *
 * The derivations are stand-ins with real CPU cost (SHA-256 chains, vector
 * scoring, table walks). They are not the AIEN model; the claim is when
 * derived work runs, not what the cognition is.
 */
#include "runtime/rx_semantic.h"
#include "runtime/rx_world.h"
#include "runtime/rx_caproot.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define U(x) ((unsigned long long)(x))

/* ---- harness --------------------------------------------------------------- */

typedef struct {
    const char *name;
    int checks;
    int failures;
} Test;

static Test g_tests[32];
static int g_ntests;
static Test *g_cur;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_cur->checks++;                                                       \
        if (!(cond)) {                                                         \
            g_cur->failures++;                                                 \
            fprintf(stderr, "  FAIL %s:%d [%s] ", __FILE__, __LINE__, g_cur->name); \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

static void begin(const char *name) {
    g_cur = &g_tests[g_ntests++];
    g_cur->name = name;
    printf("[*] %s\n", name);
}

static bool test_ok(const char *name) {
    for (int i = 0; i < g_ntests; i++)
        if (strcmp(g_tests[i].name, name) == 0) return g_tests[i].failures == 0 && g_tests[i].checks > 0;
    return false;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void hex(const uint8_t *b, size_t n, char *out) {
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* ---- the derivations ------------------------------------------------------- */

/* Work sizes. The qualification checks use small ones so the differential
 * evaluator can run many operations; the measurement uses realistic ones. */
typedef struct {
    uint32_t thermal;    /* mixing rounds */
    uint32_t repo;       /* SHA-256 chain length */
    uint32_t memory;     /* candidate vectors scored (32 dims each) */
    uint32_t caps;       /* table entries walked */
    uint32_t plan;       /* mixing rounds */
} Work;

static Work g_work;

enum { T_TEMP = 0x100, T_POWER, T_FAN, T_REPO, T_MEM, T_CAPS, T_KNOB,
       T_PLACEMENT = 0x200, T_FAN_HEALTH, T_REPO_SEM, T_RECALL, T_CAPRES, T_PLAN };

static uint64_t mix_rounds(uint64_t seed, uint32_t rounds) {
    uint64_t s = seed;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < rounds; i++) acc ^= splitmix(&s) + i;
    return acc;
}

/* Thermal placement over (temperature mC, power mW, knob). The decision
 * depends on 5 C buckets, so a small drift recomputes it and returns the same. */
static int fn_thermal(const RxSemValue *const *in, uint32_t n, RxSemValue *out, void *user) {
    (void)user;
    if (n != 3) return -1;
    uint64_t bucket = in[0]->value[0] / 5000u;
    uint64_t pw = in[1]->value[0] / 1000u;
    uint64_t knob = in[2]->value[0];
    uint64_t acc = mix_rounds(bucket * 1000003u + pw * 7919u + knob, g_work.thermal);
    uint64_t core = (bucket >= 16 ? 1u : 0u) ^ (knob & 1u);   /* 1 = efficiency cluster */
    out->n_words = 4;
    out->value[0] = core;
    out->value[1] = bucket;
    out->value[2] = knob;
    out->value[3] = acc & 0xffu;
    if (bucket >= 18) out->confidence = out->confidence * 9u / 10u;
    return 0;
}

static int fn_fan(const RxSemValue *const *in, uint32_t n, RxSemValue *out, void *user) {
    (void)user;
    if (n != 1) return -1;
    out->n_words = 1;
    out->value[0] = in[0]->value[0] > 8000u ? 1u : 0u;
    return 0;
}

static int fn_repo(const RxSemValue *const *in, uint32_t n, RxSemValue *out, void *user) {
    (void)user;
    if (n != 3) return -1;
    uint8_t d[32];
    uint64_t seed[3] = { in[0]->value[0], in[1]->value[0], in[2]->value[0] };
    sha256_hash((const uint8_t *)seed, sizeof seed, d);
    for (uint32_t i = 0; i < g_work.repo; i++) sha256_hash(d, 32, d);
    out->n_words = 4;
    for (int w = 0; w < 4; w++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v |= (uint64_t)d[8 * w + k] << (8 * k);
        out->value[w] = v;
    }
    return 0;
}

static int fn_recall(const RxSemValue *const *in, uint32_t n, RxSemValue *out, void *user) {
    (void)user;
    if (n != 2) return -1;
    uint64_t qs = in[1]->value[0] * 0x9e37u + 1u;
    int32_t q[32];
    for (int i = 0; i < 32; i++) q[i] = (int32_t)(splitmix(&qs) & 0xff) - 128;
    uint64_t ms = in[0]->value[0] * 0x51edu + 7u;
    int64_t best = INT64_MIN;
    uint64_t best_id = 0;
    for (uint32_t k = 0; k < g_work.memory; k++) {
        int64_t dot = 0;
        for (int i = 0; i < 32; i += 8) {
            uint64_t r = splitmix(&ms);
            for (int j = 0; j < 8; j++) dot += (int64_t)q[i + j] * ((int64_t)((r >> (8 * j)) & 0xff) - 128);
        }
        if (dot > best) {
            best = dot;
            best_id = k;
        }
    }
    out->n_words = 2;
    out->value[0] = best_id;
    out->value[1] = (uint64_t)best;
    return 0;
}

static int fn_capres(const RxSemValue *const *in, uint32_t n, RxSemValue *out, void *user) {
    (void)user;
    if (n != 2) return -1;
    uint64_t s = in[0]->value[0] * 31u + 3u;
    uint64_t want = in[1]->value[0];
    uint64_t granted = 0, rights = 0;
    for (uint32_t i = 0; i < g_work.caps; i++) {
        uint64_t row = splitmix(&s);
        if ((row & 0xf) == (want & 0xf)) {
            granted++;
            rights |= (row >> 8) & 0xff;
        }
    }
    out->n_words = 2;
    out->value[0] = granted;
    out->value[1] = rights;
    return 0;
}

static int fn_plan(const RxSemValue *const *in, uint32_t n, RxSemValue *out, void *user) {
    (void)user;
    if (n != 4) return -1;
    uint64_t seed = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t w = 0; w < in[i]->n_words; w++) seed = seed * 0x100000001b3ull ^ in[i]->value[w];
    out->n_words = 3;
    out->value[0] = in[0]->value[0];              /* core class chosen */
    out->value[1] = in[2]->value[0];              /* memory recalled */
    out->value[2] = mix_rounds(seed, g_work.plan);
    return 0;
}

/* ---- world + engine fixture ------------------------------------------------- */

enum { SUBJ_EXTERNAL = 100, ISSUER = 3, RES_WORLD = 0x10 };
enum { F_TEMP = 0, F_FAN = 1, F_POWER = 2 };
#define N_NOISE 8

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    RxCapRef ext;
    RxObjRef machine, repo, mem, caps, knob;
    RxObjRef noise[N_NOISE];

    RxSemEngine e;
    uint32_t f_thermal, f_fan, f_repo, f_recall, f_capres, f_plan;
    uint64_t s_temp, s_fan, s_power, s_repo0, s_repo1, s_repo2, s_mem0, s_mem1, s_caps0,
        s_caps1, s_knob;
    uint64_t n_placement, n_fan, n_repo, n_recall, n_capres, n_plan;
    uint32_t root_branch;
} Fx;

static RxCapRef mint(Fx *x, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = ISSUER;
    m.subject = SUBJ_EXTERNAL;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(&x->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    if (rx_capadmin_mint(&x->admin, &m, &r) != RX_CAP_OK) fprintf(stderr, "mint failed\n");
    return r;
}

static RxObjRef mkobj(Fx *x, uint32_t type, const uint64_t *init) {
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&x->w, type, RX_PERSIST_RESIDENT, RES_WORLD, init, &r);
    return r;
}

static int64_t put(Fx *x, RxObjRef o, uint32_t f, uint64_t v) {
    RxMutation m = { o, f, v };
    return rx_world_publish_external(&x->w, x->ext, &m, 1);
}

static RxSemDigest impl(const char *s) {
    RxSemDigest d;
    rx_sem_digest_impl(s, &d);
    return d;
}

static uint32_t add_fn(Fx *x, const char *name, uint32_t type, uint32_t gen, RxSemFn fn) {
    RxSemFnDesc d = { name, type, gen, impl(name), fn, NULL };
    uint32_t id = UINT32_MAX;
    rx_sem_add_fn(&x->e, &d, &id);
    return id;
}

static uint64_t src(Fx *x, RxObjRef o, uint32_t f, uint32_t t) {
    uint64_t sid = 0;
    rx_sem_add_source(&x->e, o, f, t, true, &sid);
    return sid;
}

static uint64_t node(Fx *x, uint32_t fn, const uint64_t *in, uint32_t n) {
    uint64_t sid = 0;
    rx_sem_add_node(&x->e, fn, in, n, &sid);
    return sid;
}

static int fx_start(Fx *x) {
    memset(x, 0, sizeof *x);
    if (rx_caproot_start(&x->root, &x->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&x->w, &x->root, 1, 1u << 20) != RX_OK) return -1;
    x->w.external_subject = SUBJ_EXTERNAL;
    x->ext = mint(x, RES_WORLD, RX_RIGHT_WRITE | RX_RIGHT_READ);
    uint64_t m0[RX_MAX_FIELDS] = { 60000, 3000, 45000 };
    uint64_t r0[RX_MAX_FIELDS] = { 0xabc, 0xdef, 120 };
    uint64_t me0[RX_MAX_FIELDS] = { 1, 7 };
    uint64_t c0[RX_MAX_FIELDS] = { 1, 3 };
    uint64_t k0[RX_MAX_FIELDS] = { 0 };
    x->machine = mkobj(x, 1, m0);
    x->repo = mkobj(x, 2, r0);
    x->mem = mkobj(x, 3, me0);
    x->caps = mkobj(x, 4, c0);
    x->knob = mkobj(x, 5, k0);
    for (int i = 0; i < N_NOISE; i++) {
        uint64_t z[RX_MAX_FIELDS] = { (uint64_t)i };
        x->noise[i] = mkobj(x, 9, z);
    }
    if (rx_sem_init(&x->e, 1u << 20) != RX_SEM_OK) return -1;
    x->f_thermal = add_fn(x, "omega.thermal_placement", T_PLACEMENT, 1, fn_thermal);
    x->f_fan = add_fn(x, "omega.fan_health", T_FAN_HEALTH, 1, fn_fan);
    x->f_repo = add_fn(x, "aien.repository_semantics", T_REPO_SEM, 1, fn_repo);
    x->f_recall = add_fn(x, "aien.memory_retrieval", T_RECALL, 1, fn_recall);
    x->f_capres = add_fn(x, "aegis.capability_resolution", T_CAPRES, 1, fn_capres);
    x->f_plan = add_fn(x, "aien.plan", T_PLAN, 1, fn_plan);
    x->s_temp = src(x, x->machine, F_TEMP, T_TEMP);
    x->s_fan = src(x, x->machine, F_FAN, T_FAN);
    x->s_power = src(x, x->machine, F_POWER, T_POWER);
    x->s_repo0 = src(x, x->repo, 0, T_REPO);
    x->s_repo1 = src(x, x->repo, 1, T_REPO);
    x->s_repo2 = src(x, x->repo, 2, T_REPO);
    x->s_mem0 = src(x, x->mem, 0, T_MEM);
    x->s_mem1 = src(x, x->mem, 1, T_MEM);
    x->s_caps0 = src(x, x->caps, 0, T_CAPS);
    x->s_caps1 = src(x, x->caps, 1, T_CAPS);
    x->s_knob = src(x, x->knob, 0, T_KNOB);
    uint64_t in_p[3] = { x->s_temp, x->s_power, x->s_knob };
    uint64_t in_f[1] = { x->s_fan };
    uint64_t in_r[3] = { x->s_repo0, x->s_repo1, x->s_repo2 };
    uint64_t in_m[2] = { x->s_mem0, x->s_mem1 };
    uint64_t in_c[2] = { x->s_caps0, x->s_caps1 };
    x->n_placement = node(x, x->f_thermal, in_p, 3);
    x->n_fan = node(x, x->f_fan, in_f, 1);
    x->n_repo = node(x, x->f_repo, in_r, 3);
    x->n_recall = node(x, x->f_recall, in_m, 2);
    x->n_capres = node(x, x->f_capres, in_c, 2);
    uint64_t in_plan[4] = { x->n_placement, x->n_repo, x->n_recall, x->n_capres };
    x->n_plan = node(x, x->f_plan, in_plan, 4);
    if (rx_sem_branch_root(&x->e, 1, 1, &x->root_branch) != RX_SEM_OK) return -1;
    return rx_sem_sync_world(&x->e, x->root_branch, &x->w) >= 0 ? 0 : -1;
}

static void fx_stop(Fx *x) {
    rx_sem_destroy(&x->e);
    rx_world_destroy(&x->w);
    rx_caproot_stop(&x->root, &x->admin);
}

static int st(Fx *x, uint32_t b, uint64_t sid) { return rx_sem_state(&x->e, b, sid); }

static RxSemServed served_of(Fx *x, uint32_t b, uint64_t sid, RxSemValue *v) {
    RxSemServed s = 0;
    RxSemValue tmp;
    if (rx_sem_get(&x->e, b, sid, v ? v : &tmp, &s) != RX_SEM_OK) return 0;
    return s;
}

static Work small_work(void) { return (Work){ 8, 4, 16, 16, 8 }; }

/* ---- a from-scratch evaluator (no engine state) ----------------------------- */

typedef struct {
    RxSemFn fn;
    uint32_t out_type;
    uint32_t n;
    int in[4];          /* >= 0: source slot; < 0: -(node+1) */
} ONode;

enum { OS_TEMP, OS_POWER, OS_KNOB, OS_FAN, OS_R0, OS_R1, OS_R2, OS_M0, OS_M1, OS_C0, OS_C1, OS_N };
enum { ON_PLACE, ON_FAN, ON_REPO, ON_RECALL, ON_CAPRES, ON_PLAN, ON_N };

static const ONode g_on[ON_N] = {
    { fn_thermal, T_PLACEMENT, 3, { OS_TEMP, OS_POWER, OS_KNOB } },
    { fn_fan, T_FAN_HEALTH, 1, { OS_FAN } },
    { fn_repo, T_REPO_SEM, 3, { OS_R0, OS_R1, OS_R2 } },
    { fn_recall, T_RECALL, 2, { OS_M0, OS_M1 } },
    { fn_capres, T_CAPRES, 2, { OS_C0, OS_C1 } },
    { fn_plan, T_PLAN, 4, { -(ON_PLACE + 1), -(ON_REPO + 1), -(ON_RECALL + 1), -(ON_CAPRES + 1) } },
};

static void oracle_eval(int node_ix, const uint64_t *srcv, RxSemValue *out) {
    const ONode *o = &g_on[node_ix];
    RxSemValue vals[4];
    const RxSemValue *in[4];
    for (uint32_t i = 0; i < o->n; i++) {
        memset(&vals[i], 0, sizeof vals[i]);
        if (o->in[i] >= 0) {
            vals[i].n_words = 1;
            vals[i].value[0] = srcv[o->in[i]];
            vals[i].confidence = RX_SEM_CONFIDENCE_ONE;
        } else oracle_eval(-o->in[i] - 1, srcv, &vals[i]);
        in[i] = &vals[i];
    }
    memset(out, 0, sizeof *out);
    uint32_t conf = RX_SEM_CONFIDENCE_ONE;
    for (uint32_t i = 0; i < o->n; i++) if (in[i]->confidence < conf) conf = in[i]->confidence;
    out->confidence = conf;
    o->fn(in, o->n, out, NULL);
    out->type = o->out_type;
}

static uint64_t fx_sid_of_slot(Fx *x, int s) {
    const uint64_t m[OS_N] = { x->s_temp, x->s_power, x->s_knob, x->s_fan, x->s_repo0, x->s_repo1,
                               x->s_repo2, x->s_mem0, x->s_mem1, x->s_caps0, x->s_caps1 };
    return m[s];
}

static uint64_t fx_node_of(Fx *x, int n) {
    const uint64_t m[ON_N] = { x->n_placement, x->n_fan, x->n_repo, x->n_recall, x->n_capres, x->n_plan };
    return m[n];
}

static bool same_value(const RxSemValue *a, const RxSemValue *b) {
    return a->n_words == b->n_words && a->confidence == b->confidence && a->type == b->type &&
           memcmp(a->value, b->value, a->n_words * sizeof a->value[0]) == 0;
}

/* ---- PROVE ------------------------------------------------------------------ */

static struct {
    uint64_t diff_ops, diff_gets, diff_mismatch, diff_hits, diff_audits, diff_false_hits;
    uint64_t diff_forks, diff_writes, diff_cross_branch_hits;
    uint64_t inv_temp_dirty, inv_temp_clean_kept, inv_noise_invalidations, inv_noise_computes;
    uint64_t prov_entries, prov_events, prov_memos, prov_world_crumbs;
    bool tamper_entry_detected, tamper_event_detected;
    uint64_t jspace_candidates, jspace_computes, jspace_cache_hits;
    uint64_t gen_obj_recomputed, gen_fn_recomputed;
} g_p;

static void t_dependency(void) {
    begin("dependency_tracking");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    uint64_t in[8];
    uint32_t n = 0;
    CHECK(rx_sem_node_inputs(&x.e, x.n_placement, in, &n) == RX_SEM_OK && n == 3 &&
          in[0] == x.s_temp && in[1] == x.s_power && in[2] == x.s_knob, "placement inputs");
    CHECK(rx_sem_node_inputs(&x.e, x.n_plan, in, &n) == RX_SEM_OK && n == 4 &&
          in[0] == x.n_placement && in[1] == x.n_repo, "plan inputs");
    uint64_t ds[16];
    CHECK(rx_sem_downstream(&x.e, x.s_temp, ds, 16, &n) == RX_SEM_OK && n == 2, "temp downstream %u", n);
    bool has_p = false, has_plan = false;
    for (uint32_t i = 0; i < n; i++) {
        has_p |= ds[i] == x.n_placement;
        has_plan |= ds[i] == x.n_plan;
    }
    CHECK(has_p && has_plan, "temp reaches placement and plan only");
    CHECK(rx_sem_downstream(&x.e, x.s_fan, ds, 16, &n) == RX_SEM_OK && n == 1 && ds[0] == x.n_fan,
          "fan reaches fan health only");
    CHECK(rx_sem_downstream(&x.e, x.s_repo1, ds, 16, &n) == RX_SEM_OK && n == 2, "repo downstream");
    CHECK(rx_sem_downstream(&x.e, x.n_plan, ds, 16, &n) == RX_SEM_OK && n == 0, "plan is a sink");
    /* Same question, same node; a different question with a colliding id is refused. */
    uint64_t again = 0, in_p[3] = { x.s_temp, x.s_power, x.s_knob };
    CHECK(rx_sem_add_node(&x.e, x.f_thermal, in_p, 3, &again) == RX_SEM_OK && again == x.n_placement,
          "idempotent node id");
    uint64_t swapped[3] = { x.s_power, x.s_temp, x.s_knob };
    uint64_t other = 0;
    CHECK(rx_sem_add_node(&x.e, x.f_thermal, swapped, 3, &other) == RX_SEM_OK && other != x.n_placement,
          "input order is part of the question");
    uint64_t bogus[1] = { ((uint64_t)RX_SEM_KIND_DERIVED << 56) | 12345u };
    CHECK(rx_sem_add_node(&x.e, x.f_fan, bogus, 1, &other) == RX_SEM_ERR_NOT_FOUND,
          "inputs must exist first (acyclic by construction)");
    uint64_t twice[2] = { x.s_temp, x.s_temp };
    CHECK(rx_sem_add_node(&x.e, x.f_fan, twice, 2, &other) == RX_SEM_ERR_ARG,
          "an input listed twice is refused");
    /* The value carries its dependencies and derivation. */
    RxSemValue v;
    CHECK(rx_sem_get(&x.e, x.root_branch, x.n_plan, &v, NULL) == RX_SEM_OK, "get plan");
    CHECK(v.n_deps == 4 && v.dependencies[0] == x.n_placement && v.dependencies[3] == x.n_capres,
          "dependencies on value");
    CHECK(v.generation == 1 && v.semantic_id == x.n_plan && v.type == T_PLAN, "identity on value");
    CHECK(v.n_evidence == 2, "evidence refs: producing entry and input evidence");
    RxSemValue t;
    CHECK(rx_sem_read_source(&x.e, x.root_branch, x.s_temp, &t) == RX_SEM_OK && t.value[0] == 60000 &&
          t.generation == x.machine.generation && t.n_evidence == 1, "source value from world");
    fx_stop(&x);
}

static void t_invalidation(void) {
    begin("correct_invalidation");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    uint32_t b = x.root_branch;
    RxSemValue plan0;
    CHECK(served_of(&x, b, x.n_plan, &plan0) == RX_SEM_SERVED_COMPUTED, "first plan computed");
    CHECK(served_of(&x, b, x.n_fan, NULL) == RX_SEM_SERVED_COMPUTED, "fan health computed");
    uint64_t c0 = x.e.stats.computes;
    CHECK(c0 == 6, "six derivations ran once each (%llu)", U(c0));

    /* Unrelated world objects: nothing is invalidated, nothing runs. */
    uint64_t inv0 = x.e.stats.invalidations;
    for (int r = 0; r < 50; r++)
        for (int i = 0; i < N_NOISE; i++) put(&x, x.noise[i], (uint32_t)(r % 4), (uint64_t)(r * 31 + i));
    CHECK(rx_sem_sync_world(&x.e, b, &x.w) == 0, "noise changes no bound source");
    RxSemValue p;
    CHECK(served_of(&x, b, x.n_plan, &p) == RX_SEM_SERVED_MEMO && same_value(&p, &plan0),
          "plan served from memo after 400 unrelated writes");
    g_p.inv_noise_invalidations = x.e.stats.invalidations - inv0;
    g_p.inv_noise_computes = x.e.stats.computes - c0;
    CHECK(g_p.inv_noise_invalidations == 0 && g_p.inv_noise_computes == 0, "no work for noise");

    /* Temperature: placement (and plan, which reads it) only. */
    put(&x, x.machine, F_TEMP, 82000);
    CHECK(rx_sem_sync_world(&x.e, b, &x.w) == 1, "one source changed");
    CHECK(st(&x, b, x.n_placement) == RX_SEM_DIRTY, "placement dirty");
    CHECK(st(&x, b, x.n_plan) == RX_SEM_DIRTY, "plan dirty (reads placement)");
    int clean = (st(&x, b, x.n_repo) == RX_SEM_CLEAN) + (st(&x, b, x.n_recall) == RX_SEM_CLEAN) +
                (st(&x, b, x.n_capres) == RX_SEM_CLEAN) + (st(&x, b, x.n_fan) == RX_SEM_CLEAN);
    CHECK(clean == 4, "repository, memory, capability, fan health stay clean (%d/4)", clean);
    g_p.inv_temp_dirty = (st(&x, b, x.n_placement) == RX_SEM_DIRTY) + (st(&x, b, x.n_plan) == RX_SEM_DIRTY);
    g_p.inv_temp_clean_kept = (uint64_t)clean;
    uint64_t c1 = x.e.stats.computes;
    RxSemValue pl;
    CHECK(served_of(&x, b, x.n_plan, &p) == RX_SEM_SERVED_COMPUTED, "plan recomputed");
    CHECK(x.e.stats.computes - c1 == 2, "exactly placement and plan ran (%llu)", U(x.e.stats.computes - c1));
    rx_sem_get(&x.e, b, x.n_placement, &pl, NULL);
    CHECK(pl.value[1] == 82000 / 5000, "placement sees the new temperature");
    CHECK(served_of(&x, b, x.n_repo, NULL) == RX_SEM_SERVED_MEMO, "repository analysis untouched");

    /* Another field of the same object (fan): fan health only. */
    put(&x, x.machine, F_FAN, 9500);
    rx_sem_sync_world(&x.e, b, &x.w);
    CHECK(st(&x, b, x.n_fan) == RX_SEM_DIRTY && st(&x, b, x.n_placement) == RX_SEM_CLEAN &&
          st(&x, b, x.n_plan) == RX_SEM_CLEAN, "fan field dirties fan health, not placement");

    /* World rewrites the same value: version moves, meaning does not. */
    uint64_t inv1 = x.e.stats.invalidations;
    put(&x, x.machine, F_TEMP, 82000);
    rx_sem_sync_world(&x.e, b, &x.w);
    CHECK(x.e.stats.invalidations == inv1 && st(&x, b, x.n_placement) == RX_SEM_CLEAN,
          "same-value write invalidates nothing");

    /* Early cutoff: drift inside the bucket reruns placement, not plan. */
    put(&x, x.machine, F_TEMP, 83500);
    rx_sem_sync_world(&x.e, b, &x.w);
    uint64_t c2 = x.e.stats.computes;
    RxSemServed ps = served_of(&x, b, x.n_plan, NULL);
    CHECK(ps == RX_SEM_SERVED_CUTOFF, "plan cut off (placement unchanged) served=%d", ps);
    CHECK(x.e.stats.computes - c2 == 1, "only placement ran (%llu)", U(x.e.stats.computes - c2));

    /* The object-granular baseline would have thrown away the plan for the fan. */
    rx_sem_set_mode(&x.e, RX_SEM_GRAIN_OBJECT, true, false);
    put(&x, x.machine, F_FAN, 3000);
    rx_sem_sync_world(&x.e, b, &x.w);
    CHECK(st(&x, b, x.n_placement) == RX_SEM_DIRTY && st(&x, b, x.n_plan) == RX_SEM_DIRTY,
          "object grain: fan dirties placement and plan (the baseline's waste)");
    rx_sem_set_mode(&x.e, RX_SEM_GRAIN_FIELD, true, false);
    fx_stop(&x);
}

/* Randomized differential run: every get compared with a from-scratch
 * evaluation; every hit re-computed in audit mode. */
static void t_no_stale(void) {
    begin("no_stale_reuse");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    rx_sem_set_mode(&x.e, RX_SEM_GRAIN_FIELD, true, true);
    enum { NB = 12 };
    uint32_t bid[NB];
    bool live[NB] = { false };
    uint64_t shadow[NB][OS_N];
    uint32_t gen[NB][OS_N];
    bid[0] = x.root_branch;
    live[0] = true;
    for (int s = 0; s < OS_N; s++) {
        RxSemValue v;
        rx_sem_read_source(&x.e, bid[0], fx_sid_of_slot(&x, s), &v);
        shadow[0][s] = v.value[0];
        gen[0][s] = v.generation;
    }
    uint64_t rng = 0x5eed0001;
    const uint64_t ops = 200000;
    for (uint64_t op = 0; op < ops; op++) {
        uint64_t r = splitmix(&rng);
        int bi = (int)(r % NB);
        if (!live[bi]) bi = 0;
        uint32_t kind = (uint32_t)((r >> 8) % 100);
        if (kind < 40) {
            /* Write a small-domain value so the same content recurs (ABA). */
            int s = (int)((r >> 16) % OS_N);
            uint64_t v = (r >> 24) % 4;
            if (s == OS_TEMP) v = 50000 + v * 4000;   /* crosses 5 C buckets and stays inside them */
            rx_sem_set_source(&x.e, bid[bi], fx_sid_of_slot(&x, s), gen[bi][s], &v, 1,
                              RX_SEM_CONFIDENCE_ONE, NULL);
            shadow[bi][s] = v;
            g_p.diff_writes++;
        } else if (kind < 45) {
            int slot = -1;
            for (int k = 1; k < NB; k++) if (!live[k]) { slot = k; break; }
            if (slot > 0) {
                uint32_t nb;
                if (rx_sem_branch_fork(&x.e, bid[bi], (uint32_t)(r >> 40) % 5, r >> 48, op + 1, &nb) ==
                    RX_SEM_OK) {
                    bid[slot] = nb;
                    live[slot] = true;
                    memcpy(shadow[slot], shadow[bi], sizeof shadow[slot]);
                    memcpy(gen[slot], gen[bi], sizeof gen[slot]);
                    g_p.diff_forks++;
                }
            }
        } else if (kind < 47) {
            if (bi != 0) {
                rx_sem_branch_drop(&x.e, bid[bi]);
                live[bi] = false;
            }
        } else {
            int n = (int)((r >> 16) % ON_N);
            RxSemValue got, want;
            RxSemServed sv = 0;
            int rc = rx_sem_get(&x.e, bid[bi], fx_node_of(&x, n), &got, &sv);
            oracle_eval(n, shadow[bi], &want);
            g_p.diff_gets++;
            if (sv != RX_SEM_SERVED_COMPUTED) g_p.diff_hits++;
            if (rc != RX_SEM_OK || !same_value(&got, &want)) {
                g_p.diff_mismatch++;
                if (g_p.diff_mismatch < 5)
                    fprintf(stderr, "  mismatch op %llu node %d served %d\n", U(op), n, sv);
            }
        }
        g_p.diff_ops++;
    }
    g_p.diff_audits = x.e.stats.audits;
    g_p.diff_false_hits = x.e.stats.false_hits;
    g_p.diff_cross_branch_hits = x.e.stats.cache_hits_cross_branch;
    CHECK(g_p.diff_mismatch == 0, "differential mismatches %llu of %llu gets", U(g_p.diff_mismatch),
          U(g_p.diff_gets));
    CHECK(g_p.diff_false_hits == 0, "audit false hits %llu of %llu", U(g_p.diff_false_hits),
          U(g_p.diff_audits));
    CHECK(g_p.diff_hits > g_p.diff_gets / 2, "reuse exercised (%llu hits)", U(g_p.diff_hits));
    CHECK(x.e.stats.cutoffs > 0 && x.e.stats.cache_hits > 0 && x.e.stats.memo_hits > 0,
          "every reuse path exercised (memo %llu cutoff %llu cache %llu)", U(x.e.stats.memo_hits),
          U(x.e.stats.cutoffs), U(x.e.stats.cache_hits));
    CHECK(g_p.diff_cross_branch_hits > 0, "cross-branch reuse inside the differential run");
    uint64_t ne, nv, nm;
    CHECK(rx_sem_verify(&x.e, &ne, &nv, &nm) == RX_SEM_OK, "verify after differential run");

    /* Directed stale case: the old entry exists but its key no longer matches. */
    uint32_t b = x.root_branch;
    uint64_t t1 = 50000, t2 = 90000;
    rx_sem_set_source(&x.e, b, x.s_temp, gen[0][OS_TEMP], &t1, 1, RX_SEM_CONFIDENCE_ONE, NULL);
    RxSemValue a1, a2, a3;
    rx_sem_get(&x.e, b, x.n_placement, &a1, NULL);
    rx_sem_set_source(&x.e, b, x.s_temp, gen[0][OS_TEMP], &t2, 1, RX_SEM_CONFIDENCE_ONE, NULL);
    rx_sem_get(&x.e, b, x.n_placement, &a2, NULL);
    CHECK(a2.value[1] == 90000 / 5000 && a1.value[1] == 50000 / 5000, "new temperature, new answer");
    rx_sem_set_source(&x.e, b, x.s_temp, gen[0][OS_TEMP], &t1, 1, RX_SEM_CONFIDENCE_ONE, NULL);
    RxSemServed sv;
    rx_sem_get(&x.e, b, x.n_placement, &a3, &sv);
    CHECK(same_value(&a3, &a1) && sv == RX_SEM_SERVED_CACHE, "A-B-A: the A result is valid reuse");
    fx_stop(&x);
}

static void t_generation(void) {
    begin("generation_isolation");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    uint32_t b = x.root_branch;
    RxSemValue before;
    CHECK(served_of(&x, b, x.n_plan, &before) == RX_SEM_SERVED_COMPUTED, "plan");

    /* (a) Retire the repository object and create another in its slot with
     * the same bytes. Same id, same content, new generation. */
    RxObjRef old = x.repo;
    CHECK(rx_world_retire(&x.w, old) == RX_OK, "retire");
    rx_sem_sync_world(&x.e, b, &x.w);
    CHECK(st(&x, b, x.n_repo) == RX_SEM_DIRTY && st(&x, b, x.n_plan) == RX_SEM_DIRTY,
          "retirement dirties what read the old object");
    RxSemValue v;
    CHECK(rx_sem_get(&x.e, b, x.n_plan, &v, NULL) == RX_SEM_ERR_STALE,
          "nothing derived from a retired object is served");
    uint64_t r0[RX_MAX_FIELDS] = { 0xabc, 0xdef, 120 };
    x.repo = mkobj(&x, 2, r0);
    CHECK(x.repo.id == old.id && x.repo.generation != old.generation, "same slot, new generation");
    rx_sem_rebind(&x.e, x.s_repo0, x.repo);
    rx_sem_rebind(&x.e, x.s_repo1, x.repo);
    rx_sem_rebind(&x.e, x.s_repo2, x.repo);
    CHECK(rx_sem_sync_world(&x.e, b, &x.w) == 3, "three sources re-read");
    uint64_t hits0 = x.e.stats.cache_hits;
    RxSemServed sr = served_of(&x, b, x.n_repo, &v);
    CHECK(sr == RX_SEM_SERVED_COMPUTED, "same bytes, new generation: recomputed (served %d)", sr);
    CHECK(x.e.stats.cache_hits == hits0, "old-generation entry not served");
    RxSemDerived ex;
    CHECK(rx_sem_explain(&x.e, b, x.n_repo, &ex) == RX_SEM_OK &&
          ex.input_versions[0].generation == x.repo.generation, "entry records the new generation");
    g_p.gen_obj_recomputed = sr == RX_SEM_SERVED_COMPUTED;

    /* (b) A promoted implementation generation of the same function. */
    RxSemFnDesc d2 = { "aien.repository_semantics", T_REPO_SEM, 2, impl("aien.repository_semantics"),
                       fn_repo, NULL };
    uint32_t f2;
    CHECK(rx_sem_add_fn(&x.e, &d2, &f2) == RX_SEM_OK, "register generation 2");
    uint64_t in_r[3] = { x.s_repo0, x.s_repo1, x.s_repo2 };
    uint64_t n2 = node(&x, f2, in_r, 3);
    CHECK(n2 != x.n_repo, "a new generation is a new derivation");
    uint64_t h1 = x.e.stats.cache_hits;
    RxSemValue v2;
    sr = served_of(&x, b, n2, &v2);
    CHECK(sr == RX_SEM_SERVED_COMPUTED && x.e.stats.cache_hits == h1,
          "generation 1 results are not served for generation 2");
    CHECK(v2.generation == 2 && !rx_sem_digest_eq(&v2.derivation_id, &v.derivation_id),
          "value names its generation and derivation");
    g_p.gen_fn_recomputed = sr == RX_SEM_SERVED_COMPUTED;
    /* A second branch on generation 2 reuses generation 2, not 1. */
    uint32_t b2;
    rx_sem_branch_root(&x.e, 2, 2, &b2);
    rx_sem_sync_world(&x.e, b2, &x.w);
    RxSemValue v3;
    CHECK(served_of(&x, b2, n2, &v3) == RX_SEM_SERVED_CACHE, "generation 2 reuses generation 2");
    CHECK(rx_sem_explain(&x.e, b2, n2, &ex) == RX_SEM_OK && ex.result.generation == 2,
          "served entry is generation 2");
    uint64_t ne, nv, nm;
    CHECK(rx_sem_verify(&x.e, &ne, &nv, &nm) == RX_SEM_OK, "verify");
    fx_stop(&x);
}

static void t_branch_isolation(void) {
    begin("branch_isolation");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    uint32_t R = x.root_branch, A, B;
    RxSemValue planR, planA, planB, v;
    served_of(&x, R, x.n_plan, &planR);
    served_of(&x, R, x.n_fan, NULL);
    uint8_t wd0[32], wd1[32];
    rx_world_digest(&x.w, wd0);
    CHECK(rx_sem_branch_fork(&x.e, R, 10, 100, 1001, &A) == RX_SEM_OK, "fork candidate A");
    CHECK(rx_sem_branch_fork(&x.e, R, 11, 100, 1002, &B) == RX_SEM_OK, "fork candidate B");
    RxSemValue knob;
    rx_sem_read_source(&x.e, R, x.s_knob, &knob);
    uint64_t ka = 1, kb = 2, temp_b = 95000;
    rx_sem_set_source(&x.e, A, x.s_knob, knob.generation, &ka, 1, RX_SEM_CONFIDENCE_ONE, NULL);
    rx_sem_set_source(&x.e, B, x.s_knob, knob.generation, &kb, 1, RX_SEM_CONFIDENCE_ONE, NULL);
    RxSemValue tmp;
    rx_sem_read_source(&x.e, R, x.s_temp, &tmp);
    rx_sem_set_source(&x.e, B, x.s_temp, tmp.generation, &temp_b, 1, RX_SEM_CONFIDENCE_ONE, NULL);
    served_of(&x, A, x.n_plan, &planA);
    served_of(&x, B, x.n_plan, &planB);
    CHECK(planA.value[2] != planR.value[2] && planB.value[2] != planA.value[2], "candidates differ");
    int cleanR = 0;
    for (int n = 0; n < ON_N; n++) cleanR += st(&x, R, fx_node_of(&x, n)) == RX_SEM_CLEAN;
    CHECK(cleanR == ON_N, "parent memo untouched by candidate writes (%d/%d clean)", cleanR, ON_N);
    CHECK(served_of(&x, R, x.n_plan, &v) == RX_SEM_SERVED_MEMO && same_value(&v, &planR),
          "parent plan unchanged");
    rx_sem_read_source(&x.e, R, x.s_knob, &v);
    CHECK(v.value[0] == 0, "parent knob unchanged");
    rx_sem_read_source(&x.e, A, x.s_temp, &v);
    CHECK(v.value[0] == tmp.value[0], "sibling A does not see B's temperature");
    rx_world_digest(&x.w, wd1);
    CHECK(memcmp(wd0, wd1, 32) == 0, "branch work never reaches the world");
    /* After the fork, a world change in the parent does not reach candidates. */
    put(&x, x.machine, F_TEMP, 99000);
    rx_sem_sync_world(&x.e, R, &x.w);
    rx_sem_read_source(&x.e, A, x.s_temp, &v);
    CHECK(v.value[0] == tmp.value[0] && st(&x, A, x.n_placement) == RX_SEM_CLEAN,
          "candidate keeps its forked view");
    CHECK(st(&x, R, x.n_placement) == RX_SEM_DIRTY, "parent sees the world change");
    CHECK(rx_sem_branch_drop(&x.e, A) == RX_SEM_OK, "drop A");
    CHECK(served_of(&x, B, x.n_plan, &v) == RX_SEM_SERVED_MEMO && same_value(&v, &planB),
          "dropping A leaves B intact");
    CHECK(rx_sem_get(&x.e, A, x.n_plan, &v, NULL) == RX_SEM_ERR_NOT_FOUND, "dropped branch is gone");
    uint64_t ne, nv, nm;
    CHECK(rx_sem_verify(&x.e, &ne, &nv, &nm) == RX_SEM_OK, "verify");
    fx_stop(&x);
}

static void t_cross_reuse(void) {
    begin("cross_branch_valid_reuse");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    uint32_t R = x.root_branch;
    served_of(&x, R, x.n_plan, NULL);
    uint64_t c0 = x.e.stats.computes;

    /* Another agent on another task, its own branch synced from the world. */
    uint32_t other;
    rx_sem_branch_root(&x.e, 7, 77, &other);
    rx_sem_sync_world(&x.e, other, &x.w);
    CHECK(served_of(&x, other, x.n_plan, NULL) == RX_SEM_SERVED_CACHE, "other agent: plan from cache");
    CHECK(x.e.stats.computes == c0, "no derivation ran for the second agent");
    CHECK(x.e.stats.cache_hits_cross_agent >= 1 && x.e.stats.cache_hits_cross_task >= 1 &&
          x.e.stats.cache_hits_cross_branch >= 1, "counted as cross agent, task and branch");

    /* J-Space: 16 candidates propose 4 distinct knob settings. */
    RxSemValue knob;
    rx_sem_read_source(&x.e, R, x.s_knob, &knob);
    uint64_t c1 = x.e.stats.computes, h1 = x.e.stats.cache_hits;
    uint64_t hc = x.e.stats.cache_hits_cross_candidate;
    RxSemValue plan_of[4];
    bool seen[4] = { false };
    int consistent = 1;
    for (int c = 0; c < 16; c++) {
        uint32_t cb;
        rx_sem_branch_fork(&x.e, R, 20u + (uint32_t)c, 500, 9000u + (uint64_t)c, &cb);
        uint64_t k = (uint64_t)(c % 4) + 1;
        rx_sem_set_source(&x.e, cb, x.s_knob, knob.generation, &k, 1, RX_SEM_CONFIDENCE_ONE, NULL);
        RxSemValue p;
        served_of(&x, cb, x.n_plan, &p);
        if (seen[c % 4]) consistent &= same_value(&p, &plan_of[c % 4]);
        else {
            plan_of[c % 4] = p;
            seen[c % 4] = true;
        }
        rx_sem_branch_drop(&x.e, cb);
    }
    g_p.jspace_candidates = 16;
    g_p.jspace_computes = x.e.stats.computes - c1;
    g_p.jspace_cache_hits = x.e.stats.cache_hits - h1;
    CHECK(g_p.jspace_computes == 8, "4 distinct proposals x (placement, plan) = 8 runs (%llu)",
          U(g_p.jspace_computes));
    CHECK(g_p.jspace_cache_hits == 24, "12 duplicate candidates x 2 served from cache (%llu)",
          U(g_p.jspace_cache_hits));
    CHECK(x.e.stats.cache_hits_cross_candidate - hc == 24, "counted as cross-candidate");
    CHECK(consistent, "duplicate candidates got identical answers");
    /* Repository analysis, memory and capability were never recomputed for any candidate. */
    RxSemDerived ex;
    CHECK(rx_sem_explain(&x.e, R, x.n_repo, &ex) == RX_SEM_OK && ex.reuses >= 1, "shared repo entry reused");
    uint64_t ne, nv, nm;
    CHECK(rx_sem_verify(&x.e, &ne, &nv, &nm) == RX_SEM_OK, "verify");
    fx_stop(&x);
}

static void t_provenance(void) {
    begin("causal_provenance");
    Fx x;
    g_work = small_work();
    CHECK(fx_start(&x) == 0, "fixture");
    uint32_t R = x.root_branch;
    put(&x, x.machine, F_TEMP, 71000);
    int64_t repo_crumb = put(&x, x.repo, 0, 0x1234);
    CHECK(repo_crumb > 0, "world write");
    rx_sem_sync_world(&x.e, R, &x.w);
    served_of(&x, R, x.n_plan, NULL);
    uint32_t cand;
    rx_sem_branch_fork(&x.e, R, 42, 4242, 777, &cand);
    uint32_t other;
    rx_sem_branch_root(&x.e, 43, 4343, &other);
    rx_sem_sync_world(&x.e, other, &x.w);
    CHECK(served_of(&x, other, x.n_repo, NULL) == RX_SEM_SERVED_CACHE, "reused by another agent");

    /* The reused value explains back to its producer ... */
    RxSemDerived ex;
    CHECK(rx_sem_explain(&x.e, other, x.n_repo, &ex) == RX_SEM_OK, "explain");
    CHECK(ex.producer_branch == R && ex.producer_agent == 1 && ex.producer_task == 1,
          "producer is the root branch's agent and task");
    /* ... and to the world crumbs that wrote its inputs. */
    uint64_t writer = rx_world_explain(&x.w, x.repo, 0);
    const RxCrumb *cr = rx_world_crumb(&x.w, writer);
    CHECK(writer == (uint64_t)repo_crumb && cr && memcmp(cr->digest, ex.input_versions[0].evidence.b, 32) == 0,
          "input evidence is the world crumb that wrote repo field 0");
    uint64_t checked = 0;
    CHECK(rx_world_verify_crumbs(&x.w, &checked) == 0, "world crumbs verify");
    g_p.prov_world_crumbs = checked;
    /* The consumer's REUSE event names the consumer and the entry. */
    bool found = false;
    for (uint64_t i = 0; i < x.e.n_events; i++) {
        const RxSemEvent *ev = &x.e.events[i];
        if (ev->kind == RX_SEM_EVENT_REUSE && ev->branch == other && ev->agent == 43 &&
            rx_sem_digest_eq(&ev->entry_digest, &ex.entry_digest))
            found = true;
    }
    CHECK(found, "REUSE event for the consumer");
    RxSemValue v;
    rx_sem_get(&x.e, other, x.n_repo, &v, NULL);
    CHECK(v.n_evidence == 2 && rx_sem_digest_eq(&v.evidence_refs[0], &ex.entry_digest),
          "served value points at its producing entry");
    uint64_t ne, nv, nm;
    CHECK(rx_sem_verify(&x.e, &ne, &nv, &nm) == RX_SEM_OK, "verify");
    g_p.prov_entries = ne;
    g_p.prov_events = nv;
    g_p.prov_memos = nm;
    CHECK(ne > 0 && nv >= ne && nm > 0, "entries %llu events %llu memos %llu", U(ne), U(nv), U(nm));

    /* Tampering is detected. */
    uint32_t slot = UINT32_MAX;
    for (uint32_t s = 0; s < RX_SEM_CACHE_SLOTS; s++)
        if (x.e.cache[s].used && rx_sem_digest_eq(&x.e.cache[s].entry_digest, &ex.entry_digest)) slot = s;
    CHECK(slot != UINT32_MAX, "entry slot");
    if (slot != UINT32_MAX) {
        x.e.cache[slot].result.value[0] ^= 1;
        g_p.tamper_entry_detected = rx_sem_verify(&x.e, NULL, NULL, NULL) == RX_SEM_ERR_VERIFY;
        x.e.cache[slot].result.value[0] ^= 1;
        x.e.cache[slot].producer_agent ^= 1;
        g_p.tamper_entry_detected &= rx_sem_verify(&x.e, NULL, NULL, NULL) == RX_SEM_ERR_VERIFY;
        x.e.cache[slot].producer_agent ^= 1;
    }
    x.e.events[0].agent ^= 1;
    g_p.tamper_event_detected = rx_sem_verify(&x.e, NULL, NULL, NULL) == RX_SEM_ERR_VERIFY;
    x.e.events[0].agent ^= 1;
    CHECK(g_p.tamper_entry_detected && g_p.tamper_event_detected, "tamper detected");
    CHECK(rx_sem_verify(&x.e, NULL, NULL, NULL) == RX_SEM_OK, "restored");
    fx_stop(&x);
}

/* ---- MEASURE ---------------------------------------------------------------- */

enum { MODE_FULL = 0, MODE_OBJECT, MODE_FIELD, MODE_ENGINE, MODE_N };
static const char *g_mode_name[MODE_N] = { "FULL", "OBJECT", "FIELD", "ENGINE" };

typedef struct {
    uint64_t queries;
    uint64_t wall_ns;          /* whole replay */
    uint64_t query_ns;         /* time inside queries */
    uint64_t computes;
    uint64_t compute_ns;
    uint64_t memo_hits, cutoffs, cache_hits, gets;
    uint64_t invalidations;
    uint64_t false_hits, audits;
    uint64_t saved_ns;
    uint64_t p50_ns, p99_ns;
    uint64_t mismatches;       /* engine answer != FULL answer */
    uint64_t answer_digest_lo;
} RunStats;

#define TRACE_TICKS 600
#define JSPACE_EVERY 20
#define JSPACE_WIDTH 6
#define MAX_Q (TRACE_TICKS * (1 + JSPACE_WIDTH) + 16)

static uint64_t g_lat[MAX_Q];

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void full_query(Fx *x, const uint64_t *srcv, RxSemValue *out) {
    (void)x;
    oracle_eval(ON_PLAN, srcv, out);
}

static void world_srcv(Fx *x, uint64_t *s) {
    RxObject m, r, me, c, k;
    rx_world_read(&x->w, x->machine, &m);
    rx_world_read(&x->w, x->repo, &r);
    rx_world_read(&x->w, x->mem, &me);
    rx_world_read(&x->w, x->caps, &c);
    rx_world_read(&x->w, x->knob, &k);
    s[OS_TEMP] = m.field[F_TEMP];
    s[OS_POWER] = m.field[F_POWER];
    s[OS_FAN] = m.field[F_FAN];
    s[OS_KNOB] = k.field[0];
    s[OS_R0] = r.field[0];
    s[OS_R1] = r.field[1];
    s[OS_R2] = r.field[2];
    s[OS_M0] = me.field[0];
    s[OS_M1] = me.field[1];
    s[OS_C0] = c.field[0];
    s[OS_C1] = c.field[1];
}

/* One deterministic organism trace: sensors tick constantly, temperature
 * moves every few ticks (usually inside its 5 C bucket), the repository,
 * memory query and capability table change rarely, the agent asks for a
 * plan every tick, and every JSPACE_EVERY ticks J-Space evaluates
 * JSPACE_WIDTH candidate knob settings (drawn from 3) in forked branches. */
static int replay(int mode, bool audit, bool check, RunStats *rs) {
    Fx x;
    if (fx_start(&x) != 0) return -1;
    memset(rs, 0, sizeof *rs);
    if (mode == MODE_OBJECT) rx_sem_set_mode(&x.e, RX_SEM_GRAIN_OBJECT, false, audit);
    else if (mode == MODE_FIELD) rx_sem_set_mode(&x.e, RX_SEM_GRAIN_FIELD, false, audit);
    else rx_sem_set_mode(&x.e, RX_SEM_GRAIN_FIELD, true, audit);
    uint64_t rng = 0x7ace0001;
    uint64_t temp = 60000;
    uint64_t nq = 0;
    uint64_t dig = 0xcbf29ce484222325ull;
    uint64_t t0 = now_ns();
    for (int tick = 0; tick < TRACE_TICKS; tick++) {
        for (int i = 0; i < N_NOISE; i++) put(&x, x.noise[i], (uint32_t)(tick % 4), splitmix(&rng));
        put(&x, x.machine, F_FAN, 2000 + splitmix(&rng) % 8000);           /* fan jitters every tick */
        if (tick % 3 == 0) {
            uint64_t r = splitmix(&rng) % 100;
            temp = r < 80 ? (temp / 5000) * 5000 + splitmix(&rng) % 5000   /* drift inside bucket */
                          : 55000 + (splitmix(&rng) % 6) * 5000;           /* a real move */
            put(&x, x.machine, F_TEMP, temp);
        }
        if (tick % 50 == 49) put(&x, x.repo, 0, splitmix(&rng));
        if (tick % 25 == 24) put(&x, x.mem, 1, splitmix(&rng) % 6);          /* topics recur */
        if (tick % 100 == 99) put(&x, x.caps, 0, splitmix(&rng) % 3);
        uint64_t srcv[OS_N];
        RxSemValue plan;
        uint64_t q0 = now_ns();
        if (mode == MODE_FULL) {
            world_srcv(&x, srcv);
            full_query(&x, srcv, &plan);
        } else {
            rx_sem_sync_world(&x.e, x.root_branch, &x.w);
            rx_sem_get(&x.e, x.root_branch, x.n_plan, &plan, NULL);
        }
        uint64_t q1 = now_ns();
        g_lat[nq++] = q1 - q0;
        rs->query_ns += q1 - q0;
        for (uint32_t w = 0; w < plan.n_words; w++) dig = (dig ^ plan.value[w]) * 0x100000001b3ull;
        if (mode != MODE_FULL && check) {
            /* Cross-check every engine answer against a from-scratch one (not timed;
             * off in the energy windows, where it would be most of the work). */
            RxSemValue want;
            world_srcv(&x, srcv);
            full_query(&x, srcv, &want);
            if (!same_value(&want, &plan)) rs->mismatches++;
        }
        if (tick % JSPACE_EVERY == JSPACE_EVERY - 1) {
            for (int c = 0; c < JSPACE_WIDTH; c++) {
                uint64_t k = splitmix(&rng) % 3 + 1;
                uint64_t cq0 = now_ns();
                RxSemValue p;
                if (mode == MODE_FULL) {
                    world_srcv(&x, srcv);
                    srcv[OS_KNOB] = k;
                    full_query(&x, srcv, &p);
                } else {
                    uint32_t cb;
                    rx_sem_branch_fork(&x.e, x.root_branch, 100u + (uint32_t)c, (uint64_t)tick,
                                       (uint64_t)tick * 16u + (uint64_t)c + 1u, &cb);
                    RxSemValue kv;
                    rx_sem_read_source(&x.e, cb, x.s_knob, &kv);
                    rx_sem_set_source(&x.e, cb, x.s_knob, kv.generation, &k, 1, RX_SEM_CONFIDENCE_ONE, NULL);
                    rx_sem_get(&x.e, cb, x.n_plan, &p, NULL);
                    rx_sem_branch_drop(&x.e, cb);
                }
                uint64_t cq1 = now_ns();
                g_lat[nq++] = cq1 - cq0;
                rs->query_ns += cq1 - cq0;
                for (uint32_t w = 0; w < p.n_words; w++) dig = (dig ^ p.value[w]) * 0x100000001b3ull;
                if (mode != MODE_FULL && check) {
                    RxSemValue want;
                    world_srcv(&x, srcv);
                    srcv[OS_KNOB] = k;
                    full_query(&x, srcv, &want);
                    if (!same_value(&want, &p)) rs->mismatches++;
                }
            }
        }
    }
    rs->wall_ns = now_ns() - t0;
    rs->queries = nq;
    rs->answer_digest_lo = dig;
    qsort(g_lat, nq, sizeof g_lat[0], cmp_u64);
    rs->p50_ns = g_lat[nq / 2];
    rs->p99_ns = g_lat[(nq * 99) / 100];
    if (mode == MODE_FULL) {
        /* FULL runs every derivation of the plan on every query. */
        rs->computes = nq * 5u;
        rs->gets = nq * 5u;
    } else {
        rs->computes = x.e.stats.computes;
        rs->compute_ns = x.e.stats.compute_ns;
        rs->memo_hits = x.e.stats.memo_hits;
        rs->cutoffs = x.e.stats.cutoffs;
        rs->cache_hits = x.e.stats.cache_hits;
        rs->gets = x.e.stats.gets;
        rs->invalidations = x.e.stats.invalidations;
        rs->false_hits = x.e.stats.false_hits;
        rs->audits = x.e.stats.audits;
        rs->saved_ns = x.e.stats.saved_ns;
        /* Verification is a check, not service: kept out of the energy windows. */
        if (check && rx_sem_verify(&x.e, NULL, NULL, NULL) != RX_SEM_OK) rs->mismatches++;
    }
    fx_stop(&x);
    return 0;
}

/* Package energy counter (hwmon "aien_spbm", energy1 = pkg, microjoules).
 * A window is valid only if both overflow indicators read zero and the
 * counter did not go backwards. */
typedef struct {
    bool present;
    char dir[300];
} Meter;

static Meter g_meter;

static bool read_u64_file(const char *path, uint64_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    unsigned long long v = 0;
    int ok = fscanf(f, "%llu", &v) == 1;
    fclose(f);
    *out = v;
    return ok;
}

static void meter_find(void) {
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[600], name[64] = { 0 };
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", de->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f) && strncmp(name, "aien_spbm", 9) == 0) {
            snprintf(g_meter.dir, sizeof g_meter.dir, "/sys/class/hwmon/%s", de->d_name);
            uint64_t v;
            char e[400];
            snprintf(e, sizeof e, "%s/energy1_input", g_meter.dir);
            g_meter.present = read_u64_file(e, &v);
        }
        fclose(f);
    }
    closedir(d);
}
/* Channels: 1 package, 2 CPU efficiency cores, 3 CPU performance cores. */
#define E_CH 3
static const char *g_ch_name[E_CH] = { "package", "cpu_e", "cpu_p" };

static bool meter_read(uint64_t uj[E_CH]) {
    for (int c = 0; c < E_CH; c++) {
        char e[400], o[400];
        uint64_t ov0 = 1, ov1 = 1;
        snprintf(e, sizeof e, "%s/energy%d_input", g_meter.dir, c + 1);
        snprintf(o, sizeof o, "%s/energy%d_overflow_raw", g_meter.dir, c + 1);
        if (!read_u64_file(o, &ov0) || !read_u64_file(e, &uj[c]) || !read_u64_file(o, &ov1)) return false;
        if (ov0 != 0 || ov1 != 0) return false;
    }
    return true;
}

typedef struct {
    bool valid;
    uint64_t uj[E_CH];
    uint64_t ns;          /* window length */
    uint64_t busy_ns;     /* time the replay itself took */
} EWin;

static void sleep_ns(uint64_t ns) {
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (nanosleep(&ts, &ts) != 0) {}
}

/* Run one replay inside a window of at least `window_ns`; the remainder is
 * idle. Two windows of equal length differ only by the work done in them
 * (plus whatever else the machine did), so their difference is the marginal
 * energy of that work. */
/* Replays per window. One replay saves a few joules of CPU work, which is
 * inside the window-to-window noise of a shared machine; several replays
 * per window raise the signal without changing what is compared. */
#define E_REPLAYS_PER_WINDOW 8

static EWin window_run(int mode, uint64_t window_ns) {
    EWin w;
    memset(&w, 0, sizeof w);
    uint64_t a[E_CH], b[E_CH];
    RunStats tmp;
    bool ok = g_meter.present && meter_read(a);
    uint64_t t0 = now_ns();
    for (int k = 0; k < E_REPLAYS_PER_WINDOW; k++) replay(mode, false, false, &tmp);
    w.busy_ns = now_ns() - t0;
    if (window_ns > w.busy_ns) sleep_ns(window_ns - w.busy_ns);
    w.ns = now_ns() - t0;
    ok = ok && meter_read(b);
    for (int c = 0; c < E_CH && ok; c++) {
        ok = b[c] >= a[c];
        w.uj[c] = ok ? b[c] - a[c] : 0;
    }
    w.valid = ok;
    return w;
}

#define E_REPS 5

static struct {
    RunStats run[MODE_N];
    RunStats audit_engine;
    bool answers_agree;
    EWin full[E_REPS], engine[E_REPS];
    bool energy_valid;
    double saved_j[E_CH][E_REPS];       /* FULL window minus ENGINE window, same length */
    double saved_med[E_CH], saved_min[E_CH], saved_max[E_CH];
    double cpu_saved[E_REPS], cpu_saved_med;   /* cpu_e + cpu_p, per pair */
    int cpu_positive;
    double full_med[E_CH], engine_med[E_CH];
    double loadavg1;
} g_m;

static double median_n(const double *v, int n) {
    double s[E_REPS];
    memcpy(s, v, (size_t)n * sizeof *v);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            double t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    return s[n / 2];
}

static void t_measure(void) {
    begin("measure");
    g_work = (Work){ 3000, 6000, 12000, 20000, 3000 };
    for (int m = 0; m < MODE_N; m++) {
        CHECK(replay(m, false, true, &g_m.run[m]) == 0, "replay %s", g_mode_name[m]);
        printf("    %-6s query %8.1f ms  runs %6llu  p50 %8.1f us  p99 %8.1f us\n",
               g_mode_name[m], g_m.run[m].query_ns / 1e6, U(g_m.run[m].computes),
               g_m.run[m].p50_ns / 1e3, g_m.run[m].p99_ns / 1e3);
    }
    g_m.answers_agree = true;
    for (int m = 1; m < MODE_N; m++) {
        g_m.answers_agree &= g_m.run[m].answer_digest_lo == g_m.run[MODE_FULL].answer_digest_lo;
        CHECK(g_m.run[m].mismatches == 0, "%s answers equal FULL per query", g_mode_name[m]);
    }
    CHECK(g_m.answers_agree, "every mode produced the same answer stream");
    /* The same ENGINE replay with every hit re-computed and compared. */
    CHECK(replay(MODE_ENGINE, true, true, &g_m.audit_engine) == 0, "audited replay");
    CHECK(g_m.audit_engine.false_hits == 0 && g_m.audit_engine.audits > 0,
          "false hits %llu of %llu audited", U(g_m.audit_engine.false_hits), U(g_m.audit_engine.audits));
    const RunStats *E = &g_m.run[MODE_ENGINE], *F = &g_m.run[MODE_FULL];
    CHECK(E->computes < F->computes, "recomputation avoided");
    CHECK(E->query_ns < F->query_ns, "latency saved");
    CHECK(E->computes <= g_m.run[MODE_FIELD].computes && g_m.run[MODE_FIELD].computes <=
          g_m.run[MODE_OBJECT].computes, "each layer does no more work than the one below it");
    CHECK(g_m.run[MODE_FIELD].invalidations < g_m.run[MODE_OBJECT].invalidations,
          "field grain invalidates less than object grain");

    meter_find();
    FILE *la = fopen("/proc/loadavg", "r");
    if (la) {
        if (fscanf(la, "%lf", &g_m.loadavg1) != 1) g_m.loadavg1 = -1;
        fclose(la);
    }
    if (!g_meter.present) {
        printf("    energy: no package energy counter on this machine; not measured\n");
        return;
    }
    bool ok = true;
    uint64_t last_full = 0;
    for (int r = 0; r < E_REPS; r++) {
        /* Alternate the order so drift in background load does not favour one side. */
        if (r % 2 == 0 || last_full == 0) {
            g_m.full[r] = window_run(MODE_FULL, 0);
            g_m.engine[r] = window_run(MODE_ENGINE, g_m.full[r].ns);
        } else {
            g_m.engine[r] = window_run(MODE_ENGINE, last_full);
            g_m.full[r] = window_run(MODE_FULL, 0);
        }
        last_full = g_m.full[r].ns;
        ok &= g_m.full[r].valid && g_m.engine[r].valid;
        double scale = g_m.engine[r].ns ? (double)g_m.full[r].ns / (double)g_m.engine[r].ns : 1.0;
        for (int c = 0; c < E_CH; c++)
            g_m.saved_j[c][r] = (g_m.full[r].uj[c] - g_m.engine[r].uj[c] * scale) / 1e6;
    }
    g_m.energy_valid = ok;
    for (int c = 0; c < E_CH; c++) {
        double fv[E_REPS], ev[E_REPS];
        g_m.saved_min[c] = g_m.saved_max[c] = g_m.saved_j[c][0];
        for (int r = 0; r < E_REPS; r++) {
            fv[r] = g_m.full[r].uj[c] / 1e6;
            ev[r] = g_m.engine[r].uj[c] / 1e6;
            if (g_m.saved_j[c][r] < g_m.saved_min[c]) g_m.saved_min[c] = g_m.saved_j[c][r];
            if (g_m.saved_j[c][r] > g_m.saved_max[c]) g_m.saved_max[c] = g_m.saved_j[c][r];
        }
        g_m.full_med[c] = median_n(fv, E_REPS);
        g_m.engine_med[c] = median_n(ev, E_REPS);
        g_m.saved_med[c] = median_n(g_m.saved_j[c], E_REPS);
        printf("    energy %-7s FULL window %7.3f J  ENGINE window %7.3f J  saved median %6.3f J "
               "(min %6.3f, max %6.3f)\n", g_ch_name[c], g_m.full_med[c], g_m.engine_med[c],
               g_m.saved_med[c], g_m.saved_min[c], g_m.saved_max[c]);
    }
    /* The saved work is one thread of CPU derivations. The CPU clusters are
     * where it runs; the package counter also carries memory, graphics and
     * every other process on the machine, whose window-to-window swing is
     * larger than this saving. The gate therefore judges the CPU clusters,
     * per pair, and requires the saving to be consistent (at least 4 of 5
     * pairs), not just a positive median. The package figure is reported. */
    for (int r = 0; r < E_REPS; r++) {
        g_m.cpu_saved[r] = g_m.saved_j[1][r] + g_m.saved_j[2][r];
        g_m.cpu_positive += g_m.cpu_saved[r] > 0;
    }
    g_m.cpu_saved_med = median_n(g_m.cpu_saved, E_REPS);
    printf("    energy CPU clusters saved per pair median %.3f J, %d of %d pairs positive\n",
           g_m.cpu_saved_med, g_m.cpu_positive, E_REPS);
    CHECK(ok, "energy windows valid (no overflow, monotonic)");
    CHECK(g_m.cpu_saved_med > 0 && g_m.cpu_positive >= E_REPS - 1,
          "CPU energy saved consistently (median %.3f J, %d/%d pairs positive)", g_m.cpu_saved_med,
          g_m.cpu_positive, E_REPS);
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

static double ratio(uint64_t a, uint64_t b) { return b ? (double)a / (double)b : 0.0; }

static void mode_json(FILE *f, const char *name, const RunStats *r, bool last) {
    uint64_t hits = r->memo_hits + r->cutoffs + r->cache_hits;
    fprintf(f,
        "    \"%s\": {\"queries\": %llu, \"derivation_runs\": %llu, \"derivation_ns\": %llu, "
        "\"query_ns\": %llu, \"wall_ns\": %llu, \"p50_query_ns\": %llu, \"p99_query_ns\": %llu, "
        "\"resolutions\": %llu, \"memo_hits\": %llu, \"cutoffs\": %llu, \"cache_hits\": %llu, "
        "\"hit_rate\": %.4f, \"invalidations\": %llu, \"unnecessary_invalidations\": %llu, "
        "\"unnecessary_invalidation_rate\": %.4f, \"answers_differing_from_full\": %llu}%s\n",
        name, U(r->queries), U(r->computes), U(r->compute_ns), U(r->query_ns), U(r->wall_ns),
        U(r->p50_ns), U(r->p99_ns), U(r->gets), U(r->memo_hits), U(r->cutoffs), U(r->cache_hits),
        ratio(hits, r->gets), U(r->invalidations), U(r->cutoffs), ratio(r->cutoffs, r->invalidations),
        U(r->mismatches), last ? "" : ",");
}

static const char *g_gate = "FAIL";

static int receipt(int prove_ok, int measure_ok) {
    char path[512], commit[41] = { 0 }, binary[65];
    if (omega_evidence_path("SEM/omega_incremental_semantics_receipt.json", path, sizeof path) != 0) return 0;
    if (!omega_evidence_run_commit(commit)) strcpy(commit, "unknown");
    binary_digest(binary);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    int dirty = omega_evidence_tree_dirty();
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 && !dirty;
    int all = prove_ok && measure_ok;
    int energy = g_meter.present && g_m.energy_valid;
    g_gate = !all ? "FAIL" : !energy ? "HOST_PASS_ENERGY_UNMEASURED" : !bound ? "PASS_UNBOUND" : "PASS";
    struct utsname host;
    memset(&host, 0, sizeof host);
    uname(&host);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    const RunStats *F = &g_m.run[MODE_FULL], *O = &g_m.run[MODE_OBJECT], *D = &g_m.run[MODE_FIELD],
                   *E = &g_m.run[MODE_ENGINE];
    uint64_t e_hits = E->memo_hits + E->cutoffs + E->cache_hits;
    fprintf(f,
        "{\n"
        "  \"schema\": \"AIEN_OMEGA_INCREMENTAL_SEMANTICS_V1\",\n"
        "  \"run_id\": \"%s\",\n"
        "  \"candidate_commit\": \"%s\",\n"
        "  \"run_commit\": \"%s\",\n"
        "  \"candidate_bound\": %s,\n"
        "  \"tree_dirty\": %s,\n"
        "  \"test_binary_sha256\": \"%s\",\n"
        "  \"machine_identity\": {\"node\": \"%s\", \"system\": \"%s\", \"release\": \"%s\", "
            "\"architecture\": \"%s\"},\n"
        "  \"gate\": {\"OMEGA_INCREMENTAL_SEMANTICS_PASS\": \"%s\"},\n",
        omega_evidence_run_id(), candidate ? candidate : "unknown", commit, bound ? "true" : "false",
        dirty ? "true" : "false", binary, host.nodename, host.sysname, host.release, host.machine, g_gate);
    fprintf(f, "  \"prove\": {\n");
    const char *names[] = { "dependency_tracking", "correct_invalidation", "no_stale_reuse",
                            "generation_isolation", "branch_isolation", "cross_branch_valid_reuse",
                            "causal_provenance" };
    for (int i = 0; i < 7; i++) {
        int checks = 0;
        for (int t = 0; t < g_ntests; t++) if (strcmp(g_tests[t].name, names[i]) == 0) checks = g_tests[t].checks;
        fprintf(f, "    \"%s\": {\"result\": \"%s\", \"checks\": %d},\n", names[i],
                test_ok(names[i]) ? "PASS" : "FAIL", checks);
    }
    fprintf(f,
        "    \"details\": {\"temperature_change_dirtied\": %llu, \"unrelated_nodes_kept_clean\": %llu, "
        "\"unrelated_world_writes_invalidations\": %llu, \"unrelated_world_writes_runs\": %llu, "
        "\"differential_ops\": %llu, \"differential_gets\": %llu, \"differential_hits\": %llu, "
        "\"differential_mismatches\": %llu, \"differential_audits\": %llu, \"differential_false_hits\": %llu, "
        "\"differential_forks\": %llu, \"differential_cross_branch_hits\": %llu, "
        "\"retired_object_same_bytes_recomputed\": %s, \"promoted_fn_generation_recomputed\": %s, "
        "\"jspace_candidates\": %llu, \"jspace_derivation_runs\": %llu, \"jspace_cache_hits\": %llu, "
        "\"provenance_entries_verified\": %llu, \"provenance_events_verified\": %llu, "
        "\"provenance_memos_verified\": %llu, \"world_crumbs_verified\": %llu, "
        "\"tampered_entry_detected\": %s, \"tampered_event_detected\": %s}\n"
        "  },\n",
        U(g_p.inv_temp_dirty), U(g_p.inv_temp_clean_kept), U(g_p.inv_noise_invalidations),
        U(g_p.inv_noise_computes), U(g_p.diff_ops), U(g_p.diff_gets), U(g_p.diff_hits),
        U(g_p.diff_mismatch), U(g_p.diff_audits), U(g_p.diff_false_hits), U(g_p.diff_forks),
        U(g_p.diff_cross_branch_hits), g_p.gen_obj_recomputed ? "true" : "false",
        g_p.gen_fn_recomputed ? "true" : "false", U(g_p.jspace_candidates), U(g_p.jspace_computes),
        U(g_p.jspace_cache_hits), U(g_p.prov_entries), U(g_p.prov_events), U(g_p.prov_memos),
        U(g_p.prov_world_crumbs), g_p.tamper_entry_detected ? "true" : "false",
        g_p.tamper_event_detected ? "true" : "false");
    fprintf(f,
        "  \"workload\": {\"ticks\": %d, \"noise_objects\": %d, \"jspace_every\": %d, \"jspace_width\": %d, "
        "\"work\": {\"thermal_rounds\": %u, \"repo_sha256_chain\": %u, \"memory_vectors\": %u, "
        "\"caps_rows\": %u, \"plan_rounds\": %u}, "
        "\"note\": \"stand-in derivations with real CPU cost; not the AIEN model\"},\n",
        TRACE_TICKS, N_NOISE, JSPACE_EVERY, JSPACE_WIDTH, g_work.thermal, g_work.repo, g_work.memory,
        g_work.caps, g_work.plan);
    fprintf(f, "  \"modes\": {\n");
    mode_json(f, "FULL", F, false);
    mode_json(f, "OBJECT", O, false);
    mode_json(f, "FIELD", D, false);
    mode_json(f, "ENGINE", E, true);
    fprintf(f, "  },\n");
    fprintf(f,
        "  \"measure\": {\"recomputation_avoided\": %llu, \"recomputation_avoided_fraction\": %.4f, "
        "\"compute_saved_ns\": %llu, \"compute_saved_ns_by_producer_timing\": %llu, "
        "\"latency_saved_ns\": %llu, \"latency_speedup\": %.2f, "
        "\"p50_latency_ns\": {\"FULL\": %llu, \"ENGINE\": %llu}, \"p99_latency_ns\": {\"FULL\": %llu, \"ENGINE\": %llu}, "
        "\"cache_hit_rate\": %.4f, \"cache_hits\": %llu, \"resolutions\": %llu, "
        "\"false_hits\": %llu, \"audited_hits\": %llu, \"false_hit_rate\": %.6f, "
        "\"unnecessary_invalidation_rate\": {\"OBJECT\": %.4f, \"FIELD\": %.4f, \"ENGINE\": %.4f}, "
        "\"invalidations\": {\"OBJECT\": %llu, \"FIELD\": %llu, \"ENGINE\": %llu}, "
        "\"answers_identical_across_modes\": %s,\n",
        U(F->computes - E->computes), 1.0 - ratio(E->computes, F->computes),
        U(F->query_ns > E->query_ns ? F->query_ns - E->query_ns : 0), U(E->saved_ns),
        U(F->query_ns > E->query_ns ? F->query_ns - E->query_ns : 0), ratio(F->query_ns, E->query_ns),
        U(F->p50_ns), U(E->p50_ns), U(F->p99_ns), U(E->p99_ns),
        ratio(e_hits, E->gets), U(e_hits), U(E->gets), U(g_m.audit_engine.false_hits),
        U(g_m.audit_engine.audits), ratio(g_m.audit_engine.false_hits, g_m.audit_engine.audits),
        ratio(O->cutoffs, O->invalidations), ratio(D->cutoffs, D->invalidations),
        ratio(E->cutoffs, E->invalidations), U(O->invalidations), U(D->invalidations),
        U(E->invalidations), g_m.answers_agree ? "true" : "false");
    if (g_meter.present) {
        fprintf(f,
            "    \"energy\": {\"source\": \"hwmon aien_spbm energy1-3 (package, cpu_e, cpu_p), microjoules; "
            "overflow indicators checked before and after each read\", "
            "\"method\": \"paired equal-length windows: FULL replay, and ENGINE replay followed by idle "
            "to the same length; saved = FULL window - ENGINE window; order alternated\", "
            "\"replays_per_window\": %d, "
            "\"valid\": %s, \"repetitions\": %d, \"loadavg_1min_at_start\": %.2f, \"channels\": {",
            E_REPLAYS_PER_WINDOW, g_m.energy_valid ? "true" : "false", E_REPS, g_m.loadavg1);
        for (int c = 0; c < E_CH; c++) {
            fprintf(f, "%s\"%s\": {\"full_window_uj\": [", c ? ", " : "", g_ch_name[c]);
            for (int r = 0; r < E_REPS; r++) fprintf(f, "%s%llu", r ? ", " : "", U(g_m.full[r].uj[c]));
            fprintf(f, "], \"engine_window_uj\": [");
            for (int r = 0; r < E_REPS; r++) fprintf(f, "%s%llu", r ? ", " : "", U(g_m.engine[r].uj[c]));
            fprintf(f, "], \"saved_j\": [");
            for (int r = 0; r < E_REPS; r++) fprintf(f, "%s%.4f", r ? ", " : "", g_m.saved_j[c][r]);
            fprintf(f, "], \"saved_j_median\": %.4f, \"saved_j_min\": %.4f, \"saved_j_max\": %.4f}",
                    g_m.saved_med[c], g_m.saved_min[c], g_m.saved_max[c]);
        }
        fprintf(f, "}, \"cpu_clusters_saved_j\": [");
        for (int r = 0; r < E_REPS; r++) fprintf(f, "%s%.4f", r ? ", " : "", g_m.cpu_saved[r]);
        fprintf(f, "], \"cpu_clusters_saved_j_median\": %.4f, \"cpu_clusters_pairs_positive\": %d, "
                   "\"gated_on\": \"cpu_e + cpu_p per pair: median > 0 and at least %d of %d pairs positive; "
                   "package is reported, not gated (its swing from other machine activity exceeds this saving)\"",
                g_m.cpu_saved_med, g_m.cpu_positive, E_REPS - 1, E_REPS);
        fprintf(f, ", \"window_ns\": [");
        for (int r = 0; r < E_REPS; r++) fprintf(f, "%s%llu", r ? ", " : "", U(g_m.full[r].ns));
        fprintf(f, "], \"engine_busy_ns\": [");
        for (int r = 0; r < E_REPS; r++) fprintf(f, "%s%llu", r ? ", " : "", U(g_m.engine[r].busy_ns));
        fprintf(f, "], \"limits\": \"on-device counters, not a wall-outlet meter; other machine activity "
                   "falls in both windows; not an R15 quiet-machine qualification\"}\n");
    } else {
        fprintf(f, "    \"energy\": {\"valid\": false, \"reason\": \"no package energy counter on this host\"}\n");
    }
    fprintf(f, "  },\n");
    fprintf(f,
        "  \"definitions\": {\n"
        "    \"recomputation_avoided\": \"derivation runs FULL minus ENGINE on the same trace\",\n"
        "    \"compute_saved_ns\": \"time inside queries, FULL minus ENGINE\",\n"
        "    \"compute_saved_ns_by_producer_timing\": \"sum over every hit of the producing run's measured time\",\n"
        "    \"cache_hit_rate\": \"(memo + cutoff + cache hits) / all node resolutions, nested ones included\",\n"
        "    \"false_hit_rate\": \"hits whose audit recomputation differed / hits audited (every hit, audited replay)\",\n"
        "    \"unnecessary_invalidation_rate\": \"invalidated memos whose inputs came back unchanged (cut off) / invalidations\"\n"
        "  },\n"
        "  \"limits\": [\n"
        "    \"functions must be pure over declared inputs; the engine cannot see an undeclared read (the audit replay is how that would show)\",\n"
        "    \"a fork copies the branch tables (simple reference, not copy-on-write)\",\n"
        "    \"the cache is bounded and never evicts; when 7/8 full, new results are not filed (counted)\",\n"
        "    \"single engine mutex; one process; the cache does not survive a restart\",\n"
        "    \"cache reuse is not gated by capabilities: a reader must present the inputs to form a key, but a key's result is visible to any branch in the engine\"\n"
        "  ],\n"
        "  \"not_claimed\": [\"the AIEN model or neural cognition\", \"GPU realization\", \"R15 performance qualification\", "
        "\"durable or cross-process cache\", \"AIENOS kernel integration\"]\n"
        "}\n");
    fclose(f);
    printf("receipt: %s\n", path);
    return 1;
}

int main(void) {
    t_dependency();
    t_invalidation();
    t_no_stale();
    t_generation();
    t_branch_isolation();
    t_cross_reuse();
    t_provenance();
    int prove_ok = 1;
    for (int i = 0; i < g_ntests; i++) prove_ok &= g_tests[i].failures == 0;
    t_measure();
    int measure_ok = g_tests[g_ntests - 1].failures == 0;
    int total_checks = 0, total_fail = 0;
    for (int i = 0; i < g_ntests; i++) {
        printf("  %-28s %s (%d checks)\n", g_tests[i].name, g_tests[i].failures ? "FAIL" : "PASS",
               g_tests[i].checks);
        total_checks += g_tests[i].checks;
        total_fail += g_tests[i].failures;
    }
    receipt(prove_ok, measure_ok);
    printf("TOTAL: %d tests, %d checks, %d failures\n", g_ntests, total_checks, total_fail);
    printf("OMEGA_INCREMENTAL_SEMANTICS_PASS: %s\n", g_gate);
    return total_fail == 0 ? 0 : 1;
}
