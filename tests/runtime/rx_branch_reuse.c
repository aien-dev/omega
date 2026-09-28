/*
 * rx_branch_reuse.c -- OMEGA_BRANCH_STATE_REUSE gate.
 *
 * 100+ J-Space branches share a large common semantic prefix. For each of
 * three realizers with different cost shapes, the same branch program runs
 * twice, each in its own child process so memory is measured from a clean
 * start:
 *
 *   independent: every branch recomputes its whole state from the seed and
 *                keeps it (no sharing of any kind);
 *   shared:      one prefix, branches fork by reference, copy on write,
 *                semantic reuse across branches, FORGE placement policy under
 *                memory pressure.
 *
 * Measured per run: memory (bytes held and resident-set growth), compute
 * (derive calls and processor time), latency (all branches ready, per-branch
 * ready), energy (GB10 package and CPU-cluster counters) and branch creation
 * time. Correctness: every shared branch's bytes equal the independent run's,
 * the shared ancestor never changes, semantic identity does not move with
 * placement or with the choice of representation.
 *
 * Usage: rx_branch_reuse [branches] [prefix_units] [diverge_units]
 */
#include "runtime/rx_jspace.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <dirent.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define U(x) ((unsigned long long)(x))
#define MAX_BRANCHES 1024u
#define UNIT_BYTES   65536u
#define EDITS        2u
#define EDIT_LEN     256u
#define ENERGY_WINDOW_NS 2000000000ull  /* the GB10 counters tick every 100 ms */

static unsigned g_branches = 128, g_prefix = 512, g_diverge = 16;
/* Branch k and k + g_choices take the same steps (96 of 128 by default). */
static unsigned g_choices = 96;
static int g_fail;

#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- realizers --------------------------------------------------------------- */

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void fill_seed(uint64_t seed, uint8_t *out, size_t n) {
    uint64_t x = seed;
    for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t v = splitmix(&x); memcpy(out + i, &v, 8); }
}

/* Native latent-state checkpoint: an integer recurrence over the whole state. */
static void derive_latent(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    if (!prev) { fill_seed(token, out, n); return; }
    const uint32_t *p = (const uint32_t *)prev;
    uint32_t *o = (uint32_t *)out;
    size_t w = n / 4, m = w - 1;
    uint32_t t = (uint32_t)(token * 0x9e3779b1u) ^ (uint32_t)(token >> 32);
    for (size_t j = 0; j < w; j++) {
        uint32_t v = p[j] ^ t;
        for (int r = 0; r < 6; r++) {
            v = v * 0x85ebca6bu + p[(j * 31u + 7u + (unsigned)r) & m];
            v = (v << 13) | (v >> 19);
            v ^= p[(j + 1u + (unsigned)r) & m];
        }
        o[j] = v;
    }
}

/* Neural activation checkpoint: one dense 128x128 layer applied to 128 rows,
 * softsign activation, bias from the token. */
#define DIM 128
static float g_w[DIM][DIM];

static void init_weights(void) {
    uint64_t x = 0x5eed;
    for (int i = 0; i < DIM; i++)
        for (int k = 0; k < DIM; k++)
            g_w[i][k] = ((float)(splitmix(&x) >> 40) / (float)(1u << 24) - 0.5f) * 0.25f;
}

static void derive_activation(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    float *o = (float *)out;
    if (!prev) {
        uint64_t x = token;
        for (size_t i = 0; i < n / 4; i++) o[i] = (float)(splitmix(&x) >> 40) / (float)(1u << 24) - 0.5f;
        return;
    }
    const float *p = (const float *)prev;
    uint64_t x = token;
    float bias[DIM];
    for (int i = 0; i < DIM; i++) bias[i] = (float)(splitmix(&x) >> 40) / (float)(1u << 24) * 0.1f;
    for (int r = 0; r < DIM; r++) {
        const float *in = p + r * DIM;
        for (int i = 0; i < DIM; i++) {
            float acc = bias[i];
            for (int k = 0; k < DIM; k++) acc += g_w[i][k] * in[k];
            o[r * DIM + i] = acc / (1.0f + fabsf(acc));
        }
    }
}

/* Attention/KV state: a window of 128 rows of 512 bytes; each step drops the
 * oldest row and appends one computed from the token and the newest row. */
#define KV_ROW 512u
static void derive_kv(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    if (!prev) { fill_seed(token, out, n); return; }
    memcpy(out, prev + KV_ROW, n - KV_ROW);
    const uint8_t *last = prev + n - KV_ROW;
    uint8_t *row = out + n - KV_ROW;
    uint64_t x = token;
    for (unsigned i = 0; i < KV_ROW; i += 8) {
        uint64_t a, k = splitmix(&x);
        memcpy(&a, last + i, 8);
        a = (a ^ k) * 0xff51afd7ed558ccdull;
        memcpy(row + i, &a, 8);
    }
}

/* World projection: 128 objects of 512 bytes. A step re-derives only the 8
 * objects its token touches (a sparse update), each through 256 mixing rounds;
 * the rest carry over unchanged. */
#define WORLD_OBJ 512u
static void derive_world(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    if (!prev) { fill_seed(token, out, n); return; }
    memcpy(out, prev, n);
    uint64_t x = token;
    unsigned objs = (unsigned)(n / WORLD_OBJ);
    for (int u = 0; u < 8; u++) {
        unsigned obj = (unsigned)(splitmix(&x) % objs);
        const uint64_t *p = (const uint64_t *)(prev + (size_t)obj * WORLD_OBJ);
        uint64_t *o = (uint64_t *)(out + (size_t)obj * WORLD_OBJ);
        for (unsigned w = 0; w < WORLD_OBJ / 8; w++) {
            uint64_t v = p[w] ^ x;
            for (unsigned r = 0; r < 256; r++) {
                v += p[(w + r) & (WORLD_OBJ / 8 - 1)];
                v ^= v >> 29; v *= 0xbf58476d1ce4e5b9ull;
            }
            o[w] = v;
        }
    }
}

static const JsRealizer g_realizers[] = {
    { JS_REAL_LATENT_CHECKPOINT, "latent_checkpoint", UNIT_BYTES, derive_latent },
    { JS_REAL_ACTIVATION_CHECKPOINT, "activation_checkpoint", UNIT_BYTES, derive_activation },
    { JS_REAL_KV_STATE, "kv_state", UNIT_BYTES, derive_kv },
    { JS_REAL_WORLD_PROJECTION, "world_projection", UNIT_BYTES, derive_world },
};
#define N_REAL (sizeof g_realizers / sizeof g_realizers[0])

/* ---- the branch program (identical for both runs) ------------------------------- */

#define ROOT_SEED 0xa1e5a1e5ull
static uint64_t prefix_token(unsigned i) { return 1000u + i; }
static unsigned choice_of(unsigned k) { return k % g_choices; }
static uint64_t diverge_token(unsigned k, unsigned j) { return (uint64_t)(choice_of(k) + 1) * 1000003ull + j; }
static void edit_of(unsigned k, unsigned e, uint32_t *idx, uint32_t *off, uint8_t patch[EDIT_LEN]) {
    unsigned c = choice_of(k);
    *idx = (c * 37u + e * 101u) % g_prefix;
    *off = ((c * 13u + e * 7u) % (UNIT_BYTES / EDIT_LEN)) * EDIT_LEN;
    fill_seed(0xed17ull * (c + 1) + e, patch, EDIT_LEN);
}

/* ---- measurement ------------------------------------------------------------------ */

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t cpu_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t rss_bytes(void) {
    unsigned long long size = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%llu %llu", &size, &res) != 2) res = 0; fclose(f); }
    return res * (uint64_t)sysconf(_SC_PAGESIZE);
}

/* GB10 energy counters (hwmon "aien_spbm", microjoules): pkg, cpu_e, cpu_p. */
static char g_hwmon[320];
static void find_hwmon(void) {
    DIR *d = opendir("/sys/class/hwmon");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char p[512], name[64] = "";
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f) && !strncmp(name, "aien_spbm", 9))
            snprintf(g_hwmon, sizeof g_hwmon, "/sys/class/hwmon/%s", e->d_name);
        fclose(f);
    }
    if (d) closedir(d);
}
static bool read_energy(uint64_t out[3]) {
    if (!g_hwmon[0]) return false;
    for (int i = 0; i < 3; i++) {
        char p[340];
        snprintf(p, sizeof p, "%.300s/energy%d_input", g_hwmon, i + 1);
        FILE *f = fopen(p, "r");
        unsigned long long v = 0;
        if (!f) return false;
        int ok = fscanf(f, "%llu", &v) == 1;
        fclose(f);
        if (!ok) return false;
        out[i] = v;
    }
    return true;
}

static int pin_performance_core(void) {
    for (int cpu = 63; cpu >= 0; cpu--) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", cpu);
        FILE *f = fopen(p, "r");
        unsigned long long midr = 0;
        if (!f) continue;
        int ok = fscanf(f, "%llx", &midr) == 1;
        fclose(f);
        if (ok && ((midr >> 4) & 0xfffu) == 0xd85u) {   /* Cortex-X925 */
            cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
            if (sched_setaffinity(0, sizeof set, &set) == 0) return cpu;
        }
    }
    return -1;
}

/* ---- per-run results (shared with the parent through an anonymous mapping) ----- */

typedef struct {
    int ok;
    uint64_t wall_ns, cpu_ns, derives;
    uint64_t held_bytes;            /* bytes of state held when all branches are ready */
    uint64_t rss_growth;
    bool energy_ok;
    uint64_t energy_uj[3];          /* per construction: pkg, cpu_e, cpu_p */
    uint64_t energy_reps, energy_window_ns;
    uint64_t first_ready_ns;        /* start to first branch ready */
    double branch_ready_mean_ns, branch_ready_p99_ns;
    double create_median_ns, create_p99_ns, create_max_ns;
    uint8_t digest[MAX_BRANCHES][32];
    uint8_t semid[MAX_BRANCHES][32];
    /* shared-run only */
    uint64_t actions[JS_ACT_COUNT];
    uint64_t live_reals, bytes_copied, reuse_pairs_shared;
    int ancestor_intact, frozen_refused, cow_isolated, reuse_ok, pressure_ok, placement_cycle_ok;
    JsPolicyReport policy;
    uint64_t budget_bytes, resident_after_policy, spilled_after_policy;
    uint32_t placement_after[6];
    JsCosts costs;
    char fail_msg[256];
} RunResult;

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double pct(double *v, unsigned n, double q) {
    qsort(v, n, sizeof *v, cmp_double);
    unsigned i = (unsigned)(q * (n - 1) + 0.5);
    return v[i];
}

/* Wait for the next counter tick so a window starts and ends on one. */
static bool energy_edge(uint64_t e[3]) {
    uint64_t a[3], deadline = now_ns() + 500000000ull;
    if (!read_energy(a)) return false;
    do { if (!read_energy(e)) return false; } while (e[0] == a[0] && now_ns() < deadline);
    return e[0] != a[0];
}

static void begin(RunResult *R, uint64_t *t0, uint64_t *c0, uint64_t *r0, uint64_t e0[3]) {
    R->energy_ok = energy_edge(e0);
    *r0 = rss_bytes();
    *c0 = cpu_ns();
    *t0 = now_ns();
}
static void end(RunResult *R, uint64_t t0, uint64_t c0, uint64_t r0, const uint64_t e0[3]) {
    uint64_t e1[3];
    R->wall_ns = now_ns() - t0;
    R->cpu_ns = cpu_ns() - c0;
    uint64_t r1 = rss_bytes();
    R->rss_growth = r1 > r0 ? r1 - r0 : 0;
    if (R->energy_ok && energy_edge(e1)) {
        for (int i = 0; i < 3; i++) {
            if (e1[i] < e0[i]) R->energy_ok = false;
            R->energy_uj[i] = e1[i] - e0[i];
        }
        R->energy_reps = 1;
        R->energy_window_ns = now_ns() - t0;
    } else R->energy_ok = false;
}

/* A run shorter than the window is too short for 100 ms counters: repeat the
 * whole construction (and teardown) until the window is long enough, starting
 * and ending on a tick, and report energy per construction. */
static void energy_by_repetition(RunResult *R, void (*rep)(void *), void *ctx) {
    if (!R->energy_ok || R->energy_window_ns >= ENERGY_WINDOW_NS) return;
    uint64_t e0[3], e1[3], e_at[3];
    if (!energy_edge(e0)) { R->energy_ok = false; return; }
    uint64_t t0 = now_ns(), reps = 0;
    bool armed = false;
    for (;;) {
        rep(ctx);
        reps++;
        if (!armed && now_ns() - t0 >= ENERGY_WINDOW_NS) {
            if (!read_energy(e_at)) { R->energy_ok = false; return; }
            armed = true;
        } else if (armed) {
            if (!read_energy(e1)) { R->energy_ok = false; return; }
            if (e1[0] != e_at[0]) break;   /* a tick landed during this construction */
        }
    }
    R->energy_window_ns = now_ns() - t0;
    R->energy_reps = reps;
    for (int i = 0; i < 3; i++) R->energy_uj[i] = (e1[i] - e0[i]) / reps;
}

/* ---- independent recomputation ---------------------------------------------------- */

static void fail(RunResult *R, const char *m) {
    if (!R->fail_msg[0]) snprintf(R->fail_msg, sizeof R->fail_msg, "%s", m);
}

/* Branch k's whole state, computed from the seed. */
static uint8_t *indep_branch(const JsRealizer *rz, unsigned k) {
    unsigned units = g_prefix + 1 + g_diverge;
    uint8_t *s = malloc((size_t)units * UNIT_BYTES);
    if (!s) return NULL;
    rz->derive(NULL, ROOT_SEED, s, UNIT_BYTES);
    for (unsigned i = 0; i < g_prefix; i++)
        rz->derive(s + (size_t)i * UNIT_BYTES, prefix_token(i), s + (size_t)(i + 1) * UNIT_BYTES, UNIT_BYTES);
    for (unsigned j = 0; j < g_diverge; j++) {
        unsigned i = g_prefix + j;
        rz->derive(s + (size_t)i * UNIT_BYTES, diverge_token(k, j), s + (size_t)(i + 1) * UNIT_BYTES, UNIT_BYTES);
    }
    for (unsigned e = 0; e < EDITS; e++) {
        uint32_t idx, off; uint8_t patch[EDIT_LEN];
        edit_of(k, e, &idx, &off, patch);
        memcpy(s + (size_t)(idx + 1) * UNIT_BYTES + off, patch, EDIT_LEN);  /* unit idx+1: after the root */
    }
    return s;
}

/* One full construction and teardown, for energy repetition. */
static void indep_rep(void *ctx) {
    const JsRealizer *rz = ctx;
    uint8_t **state = calloc(g_branches, sizeof *state);
    for (unsigned k = 0; state && k < g_branches; k++) state[k] = indep_branch(rz, k);
    for (unsigned k = 0; state && k < g_branches; k++) free(state[k]);
    free(state);
}

static void run_independent(const JsRealizer *rz, RunResult *R) {
    unsigned units = g_prefix + 1 + g_diverge;
    uint8_t **state = calloc(g_branches, sizeof *state);
    double *ready = malloc(g_branches * sizeof *ready);
    uint64_t t0, c0, r0, e0[3];
    begin(R, &t0, &c0, &r0, e0);
    for (unsigned k = 0; k < g_branches; k++) {
        uint64_t bt = now_ns();
        /* Creating an independent branch is creating its whole state. */
        state[k] = indep_branch(rz, k);
        if (!state[k]) { fail(R, "out of memory"); return; }
        R->derives += 1 + g_prefix + g_diverge;
        ready[k] = (double)(now_ns() - bt);
        if (k == 0) R->first_ready_ns = now_ns() - t0;
    }
    end(R, t0, c0, r0, e0);
    R->held_bytes = (uint64_t)g_branches * units * UNIT_BYTES;
    double sum = 0;
    for (unsigned k = 0; k < g_branches; k++) sum += ready[k];
    R->branch_ready_mean_ns = sum / g_branches;
    R->branch_ready_p99_ns = pct(ready, g_branches, 0.99);
    R->create_median_ns = pct(ready, g_branches, 0.5);
    R->create_p99_ns = pct(ready, g_branches, 0.99);
    R->create_max_ns = ready[g_branches - 1];
    for (unsigned k = 0; k < g_branches; k++) {
        sha256_hash(state[k], (size_t)units * UNIT_BYTES, R->digest[k]);
        free(state[k]);
    }
    free(state); free(ready);
    energy_by_repetition(R, indep_rep, (void *)rz);
    R->ok = 1;
}

/* ---- shared-state realization ------------------------------------------------------- */

/* Root, common prefix, then every branch: fork, private steps, edits. */
static int shared_construct(JsSpace *S, const JsRealizer *rz, uint32_t *root, uint32_t *ids,
                            double *create, double *ready, uint64_t t0, uint64_t *first_ready) {
    int rc = js_branch_root(S, rz, ROOT_SEED, root);
    for (unsigned i = 0; i < g_prefix && rc == JS_OK; i++) rc = js_branch_derive(S, *root, prefix_token(i));
    for (unsigned k = 0; k < g_branches && rc == JS_OK; k++) {
        uint64_t bt = now_ns();
        rc = js_branch_fork(S, *root, &ids[k]);
        if (create) create[k] = (double)(now_ns() - bt);
        for (unsigned j = 0; j < g_diverge && rc == JS_OK; j++) rc = js_branch_derive(S, ids[k], diverge_token(k, j));
        for (unsigned e = 0; e < EDITS && rc == JS_OK; e++) {
            uint32_t idx, off; uint8_t patch[EDIT_LEN];
            edit_of(k, e, &idx, &off, patch);
            rc = js_branch_edit(S, ids[k], idx + 1, off, patch, EDIT_LEN);
        }
        if (ready) ready[k] = (double)(now_ns() - bt);
        if (k == 0 && first_ready) *first_ready = now_ns() - t0;
    }
    return rc;
}

typedef struct { const JsRealizer *rz; const char *spill; JsCosts costs; } SharedRepCtx;

static void shared_rep(void *p) {
    SharedRepCtx *c = p;
    JsSpace *S = calloc(1, sizeof *S);
    uint32_t root, *ids = malloc(g_branches * sizeof *ids);
    if (S && ids && js_space_init(S, c->spill) == JS_OK) {
        S->costs = c->costs;
        shared_construct(S, c->rz, &root, ids, NULL, NULL, now_ns(), NULL);
        js_space_destroy(S);
    }
    free(S); free(ids);
}

static void run_shared(const JsRealizer *rz, RunResult *R, const char *spill) {
    JsSpace *S = calloc(1, sizeof *S);
    if (!S || js_space_init(S, spill) != JS_OK) { fail(R, "space init"); return; }
    js_calibrate(S, rz);
    R->costs = S->costs;
    uint64_t derives0 = S->stats.derives;
    memset(S->stats.actions, 0, sizeof S->stats.actions);
    uint32_t *ids = malloc(g_branches * sizeof *ids);
    double *create = malloc(g_branches * sizeof *create), *ready = malloc(g_branches * sizeof *ready);
    uint32_t root;
    uint64_t t0, c0, r0, e0[3];

    begin(R, &t0, &c0, &r0, e0);
    int rc = shared_construct(S, rz, &root, ids, create, ready, t0, &R->first_ready_ns);
    end(R, t0, c0, r0, e0);
    if (rc != JS_OK) { fail(R, "branch construction"); goto out; }
    R->derives = S->stats.derives - derives0;
    R->held_bytes = S->stats.peak_resident_bytes;
    {
        double sum = 0;
        for (unsigned k = 0; k < g_branches; k++) sum += ready[k];
        R->branch_ready_mean_ns = sum / g_branches;
        R->branch_ready_p99_ns = pct(ready, g_branches, 0.99);
        R->create_max_ns = 0;
        for (unsigned k = 0; k < g_branches; k++) if (create[k] > R->create_max_ns) R->create_max_ns = create[k];
        R->create_median_ns = pct(create, g_branches, 0.5);
        R->create_p99_ns = pct(create, g_branches, 0.99);
    }
    memcpy(R->actions, S->stats.actions, sizeof R->actions);
    R->live_reals = S->stats.live_reals;
    R->bytes_copied = S->stats.bytes_copied;

    /* --- correctness, outside the measured window --- */
    uint8_t anc_digest[32], anc_digest2[32];
    JsSemId anc_sem = js_branch_semantic_id(S->branches[root]);
    if (js_branch_content_digest(S, root, anc_digest) != JS_OK) { fail(R, "ancestor digest"); goto out; }

    /* The shared ancestor: every unit still holds the prefix realizations in
     * order, its identity and bytes match a fresh prefix, and it refuses writes. */
    {
        JsSpace *F = calloc(1, sizeof *F);
        uint32_t fr; uint8_t fresh[32];
        int ok = F && js_space_init(F, spill) == JS_OK && js_branch_root(F, rz, ROOT_SEED, &fr) == JS_OK;
        for (unsigned i = 0; ok && i < g_prefix; i++) ok = js_branch_derive(F, fr, prefix_token(i)) == JS_OK;
        ok = ok && js_branch_content_digest(F, fr, fresh) == JS_OK;
        JsSemId fsem = ok ? js_branch_semantic_id(F->branches[fr]) : anc_sem;
        R->ancestor_intact = ok && !memcmp(fresh, anc_digest, 32) && !memcmp(fsem.b, anc_sem.b, 32);
        if (F) { js_space_destroy(F); free(F); }
    }
    {
        uint8_t patch[EDIT_LEN] = { 1 };
        int a = js_branch_edit(S, root, 5, 0, patch, EDIT_LEN);
        int b = js_branch_derive(S, root, 42);
        js_branch_content_digest(S, root, anc_digest2);
        R->frozen_refused = a == JS_ERR_FROZEN && b == JS_ERR_FROZEN && !memcmp(anc_digest, anc_digest2, 32);
    }
    /* Copy on write: an edited unit is private to the branches that made that
     * edit; every other branch still holds the ancestor's realization. */
    {
        int iso = 1;
        JsBranch *rb = S->branches[root];
        for (unsigned k = 0; k < g_branches; k++) {
            JsBranch *b = S->branches[ids[k]];
            for (unsigned i = 0; i < g_prefix + 1; i++) {
                bool edited = false;
                for (unsigned e = 0; e < EDITS; e++) {
                    uint32_t idx, off; uint8_t patch[EDIT_LEN];
                    edit_of(k, e, &idx, &off, patch);
                    if (idx + 1 == i) edited = true;
                }
                if (edited == (b->units[i] == rb->units[i])) iso = 0;
            }
        }
        R->cow_isolated = iso;
    }
    /* Semantic reuse: branches k and k + g_choices took the same steps, so they hold
     * the same realizations; branches that differ share none past the fork. */
    {
        int ok = 1;
        for (unsigned k = 0; k + g_choices < g_branches; k++) {
            JsBranch *a = S->branches[ids[k]], *b = S->branches[ids[k + g_choices]];
            for (unsigned i = 0; i < a->n_units; i++) if (a->units[i] != b->units[i]) ok = 0;
            R->reuse_pairs_shared++;
        }
        if (g_branches > 1) {
            JsBranch *a = S->branches[ids[0]], *b = S->branches[ids[1]];
            if (a->units[a->n_units - 1] == b->units[b->n_units - 1]) ok = 0;
        }
        R->reuse_ok = ok && R->reuse_pairs_shared > 0;
    }
    for (unsigned k = 0; k < g_branches; k++) {
        JsSemId sid = js_branch_semantic_id(S->branches[ids[k]]);
        memcpy(R->semid[k], sid.b, 32);
    }

    /* --- memory pressure: FORGE decides per realization, then everything is
     * read back through whatever placement it chose --- */
    R->budget_bytes = S->stats.resident_bytes / 4;
    if (js_forge_enforce(S, R->budget_bytes, &R->policy) != JS_OK) { fail(R, "policy enforce"); goto out; }
    R->resident_after_policy = S->stats.resident_bytes;
    R->spilled_after_policy = S->stats.spilled_bytes;
    for (uint32_t k = 0; k < JS_INDEX_BUCKETS; k++)
        for (JsReal *r = S->index[k]; r; r = r->index_next)
            if (r->placement < 6) R->placement_after[r->placement]++;
    {
        int ok = R->resident_after_policy <= R->budget_bytes;
        for (unsigned k = 0; k < g_branches; k++) {
            JsSemId sid = js_branch_semantic_id(S->branches[ids[k]]);
            if (memcmp(sid.b, R->semid[k], 32)) ok = 0;
            if (js_branch_content_digest(S, ids[k], R->digest[k]) != JS_OK) ok = 0;
        }
        if (js_branch_content_digest(S, root, anc_digest2) != JS_OK || memcmp(anc_digest, anc_digest2, 32)) ok = 0;
        if (S->stats.corrupt_restores) ok = 0;
        R->pressure_ok = ok;
        /* Actions of construction, the policy and the reads that followed.
         * The forced placement cycle below is a correctness probe and is not
         * counted toward the gate. */
        memcpy(R->actions, S->stats.actions, sizeof R->actions);
    }
    /* One branch's every unit walked through every placement, checked each step. */
    {
        JsBranch *b = S->branches[ids[g_branches - 1]];
        uint8_t *want = malloc(UNIT_BYTES), *got = malloc(UNIT_BYTES);
        JsSemId before = js_branch_semantic_id(b);
        int ok = want && got;
        for (unsigned i = 0; ok && i < b->n_units; i++) {
            JsReal *r = b->units[i];
            ok = js_branch_read(S, ids[g_branches - 1], i, want) == JS_OK;
            int (*ops[])(JsSpace *, JsReal *) = { js_real_restore, js_real_move, js_real_compress,
                                                  js_real_spill, js_real_evict, js_real_restore };
            for (unsigned o = 0; ok && o < sizeof ops / sizeof ops[0]; o++) {
                if (ops[o] == js_real_move && r->placement != JS_PLACE_HOT) continue;
                ok = ops[o](S, r) == JS_OK &&
                     js_branch_read(S, ids[g_branches - 1], i, got) == JS_OK &&
                     !memcmp(want, got, UNIT_BYTES);
            }
        }
        JsSemId after = js_branch_semantic_id(b);
        R->placement_cycle_ok = ok && !memcmp(before.b, after.b, 32);
        free(want); free(got);
    }
    R->ok = 1;
out:
    js_space_destroy(S);
    if (R->ok) {
        SharedRepCtx c = { rz, spill, R->costs };
        energy_by_repetition(R, shared_rep, &c);
    }
    free(S); free(ids); free(create); free(ready);
}

/* ---- driver ------------------------------------------------------------------------ */

static RunResult *run_child(void (*fn)(const JsRealizer *, RunResult *, const char *),
                            const JsRealizer *rz, const char *spill) {
    RunResult *R = mmap(NULL, sizeof *R, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (R == MAP_FAILED) return NULL;
    memset(R, 0, sizeof *R);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        pin_performance_core();
        fn(rz, R, spill);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) R->ok = 0;
    return R;
}
static void indep_adapter(const JsRealizer *rz, RunResult *R, const char *spill) {
    (void)spill; run_independent(rz, R);
}

typedef struct {
    const JsRealizer *rz;
    RunResult *ind, *sh;
    int digests_match, beats_memory, beats_compute, beats_latency, beats_energy, correct;
} RealizerOutcome;

static double ms(double ns) { return ns / 1e6; }
static double mib(uint64_t b) { return (double)b / 1048576.0; }
static double joules(uint64_t uj) { return (double)uj / 1e6; }

static void hex(const uint8_t *b, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]); }

int main(int argc, char **argv) {
    if (argc > 1) g_branches = (unsigned)atoi(argv[1]);
    if (argc > 2) g_prefix = (unsigned)atoi(argv[2]);
    if (argc > 3) g_diverge = (unsigned)atoi(argv[3]);
    if (g_branches < 2 || g_branches > MAX_BRANCHES || g_prefix < 8 || !g_diverge) {
        fprintf(stderr, "usage: %s [branches 2..%u] [prefix >=8] [diverge >=1]\n", argv[0], MAX_BRANCHES);
        return 2;
    }
    g_choices = g_branches - g_branches / 4;
    init_weights();
    find_hwmon();
    char spill[512];
    if (omega_evidence_path("BRANCH_REUSE/spill.bin", spill, sizeof spill) != 0) return 2;

    printf("OMEGA branch-state reuse: %u branches, %u-unit common prefix, %u private units + %u edits each,"
           " %u-byte units (prefix %.0f MiB)\n", g_branches, g_prefix, g_diverge, EDITS, UNIT_BYTES,
           mib((uint64_t)(g_prefix + 1) * UNIT_BYTES));
    printf("energy counters: %s\n", g_hwmon[0] ? g_hwmon : "NOT FOUND");

    RealizerOutcome out[N_REAL];
    for (unsigned t = 0; t < N_REAL; t++) {
        const JsRealizer *rz = &g_realizers[t];
        RealizerOutcome *o = &out[t];
        memset(o, 0, sizeof *o);
        o->rz = rz;
        printf("\n[%s]\n", rz->name);
        o->ind = run_child(indep_adapter, rz, spill);
        o->sh = run_child(run_shared, rz, spill);
        RunResult *I = o->ind, *S = o->sh;
        CHECK(I && I->ok, "%s: independent run did not complete", rz->name);
        CHECK(S && S->ok, "%s: shared run did not complete (%s)", rz->name, S ? S->fail_msg : "?");
        if (!I || !S || !I->ok || !S->ok) continue;
        o->digests_match = 1;
        for (unsigned k = 0; k < g_branches; k++) if (memcmp(I->digest[k], S->digest[k], 32)) o->digests_match = 0;
        CHECK(o->digests_match, "%s: a shared branch's bytes differ from its independent recomputation", rz->name);
        CHECK(S->ancestor_intact, "%s: shared ancestor changed", rz->name);
        CHECK(S->frozen_refused, "%s: a write to the frozen ancestor was not refused", rz->name);
        CHECK(S->cow_isolated, "%s: copy-on-write isolation broken", rz->name);
        CHECK(S->reuse_ok, "%s: semantic reuse did not share identical branches", rz->name);
        CHECK(S->pressure_ok, "%s: identity or bytes changed under the placement policy", rz->name);
        CHECK(S->placement_cycle_ok, "%s: a placement cycle changed bytes or identity", rz->name);
        o->correct = o->digests_match && S->ancestor_intact && S->frozen_refused && S->cow_isolated &&
                     S->reuse_ok && S->pressure_ok && S->placement_cycle_ok;
        o->beats_memory = S->held_bytes < I->held_bytes && S->rss_growth < I->rss_growth;
        o->beats_compute = S->derives < I->derives && S->cpu_ns < I->cpu_ns;
        o->beats_latency = S->wall_ns < I->wall_ns && S->branch_ready_mean_ns < I->branch_ready_mean_ns;
        o->beats_energy = I->energy_ok && S->energy_ok && S->energy_uj[0] < I->energy_uj[0];
        CHECK(o->beats_memory, "%s: shared realization did not use less memory", rz->name);
        CHECK(o->beats_compute, "%s: shared realization did not use less compute", rz->name);
        CHECK(o->beats_latency, "%s: shared realization was not faster", rz->name);
        CHECK(o->beats_energy, "%s: energy not measured lower (counters %s)", rz->name,
              I->energy_ok && S->energy_ok ? "ok" : "unavailable");

        printf("  measured costs: derive %.1f us/unit, copy %.3f ns/B, compress %.3f ns/B (delta ratio %.3f),"
               " spill w/r %.3f/%.3f ns/B, retain %.3f ns/B\n",
               S->costs.derive_ns / 1e3, S->costs.copy_ns_per_byte, S->costs.compress_ns_per_byte,
               S->costs.compress_ratio, S->costs.spill_write_ns_per_byte, S->costs.spill_read_ns_per_byte,
               S->costs.retain_ns_per_byte);
        printf("  %-22s %14s %14s %9s\n", "", "independent", "shared", "ratio");
        printf("  %-22s %14.1f %14.1f %8.1fx\n", "memory held (MiB)", mib(I->held_bytes), mib(S->held_bytes),
               (double)I->held_bytes / (double)S->held_bytes);
        printf("  %-22s %14.1f %14.1f %8.1fx\n", "RSS growth (MiB)", mib(I->rss_growth), mib(S->rss_growth),
               (double)I->rss_growth / (double)(S->rss_growth ? S->rss_growth : 1));
        printf("  %-22s %14llu %14llu %8.1fx\n", "derive calls", U(I->derives), U(S->derives),
               (double)I->derives / (double)S->derives);
        printf("  %-22s %14.1f %14.1f %8.1fx\n", "CPU time (ms)", ms(I->cpu_ns), ms(S->cpu_ns),
               (double)I->cpu_ns / (double)S->cpu_ns);
        printf("  %-22s %14.1f %14.1f %8.1fx\n", "all branches ready (ms)", ms(I->wall_ns), ms(S->wall_ns),
               (double)I->wall_ns / (double)S->wall_ns);
        printf("  %-22s %14.1f %14.1f\n", "first branch ready (ms)", ms(I->first_ready_ns), ms(S->first_ready_ns));
        printf("  %-22s %14.3f %14.3f %8.1fx\n", "per-branch ready (ms)", ms(I->branch_ready_mean_ns),
               ms(S->branch_ready_mean_ns), I->branch_ready_mean_ns / S->branch_ready_mean_ns);
        printf("  %-22s %14.1f %14.4f\n", "branch create med (ms)", ms(I->create_median_ns), ms(S->create_median_ns));
        printf("  %-22s %14.1f %14.4f\n", "branch create p99 (ms)", ms(I->create_p99_ns), ms(S->create_p99_ns));
        if (I->energy_ok && S->energy_ok) {
            printf("  %-22s %14.2f %14.2f %8.1fx\n", "energy pkg (J)", joules(I->energy_uj[0]),
                   joules(S->energy_uj[0]), (double)I->energy_uj[0] / (double)S->energy_uj[0]);
            printf("  %-22s %14.2f %14.2f\n", "energy cpu_p (J)", joules(I->energy_uj[2]), joules(S->energy_uj[2]));
            printf("  %-22s %14llu %14llu\n", "energy constructions", U(I->energy_reps), U(S->energy_reps));
        }
        printf("  actions:");
        for (int a = 0; a < JS_ACT_COUNT; a++) printf(" %s=%llu", js_action_name((JsAction)a), U(S->actions[a]));
        printf("\n  policy under %.1f MiB budget: considered %llu, kept %llu, move %llu, compress %llu,"
               " spill %llu, evict %llu; resident after %.1f MiB, spilled %.1f MiB\n",
               mib(S->budget_bytes), U(S->policy.considered), U(S->policy.kept),
               U(S->policy.chosen[JS_ACT_MOVE]), U(S->policy.chosen[JS_ACT_COMPRESS]),
               U(S->policy.chosen[JS_ACT_SPILL]), U(S->policy.chosen[JS_ACT_EVICT]),
               mib(S->resident_after_policy), mib(S->spilled_after_policy));
        printf("  correctness: bytes %s, ancestor %s, frozen %s, copy-on-write %s, reuse %s, pressure %s,"
               " placement cycle %s\n",
               o->digests_match ? "ok" : "FAIL", S->ancestor_intact ? "ok" : "FAIL",
               S->frozen_refused ? "ok" : "FAIL", S->cow_isolated ? "ok" : "FAIL",
               S->reuse_ok ? "ok" : "FAIL", S->pressure_ok ? "ok" : "FAIL",
               S->placement_cycle_ok ? "ok" : "FAIL");
    }

    /* Representation independence: the same branch program gives the same
     * semantic identities under every realizer, and different bytes. */
    int rep_indep = 1;
    for (unsigned t = 1; t < N_REAL; t++) {
        if (!out[0].sh || !out[t].sh || !out[0].sh->ok || !out[t].sh->ok) { rep_indep = 0; continue; }
        for (unsigned k = 0; k < g_branches; k++) {
            if (memcmp(out[0].sh->semid[k], out[t].sh->semid[k], 32)) rep_indep = 0;
            if (!memcmp(out[0].sh->digest[k], out[t].sh->digest[k], 32)) rep_indep = 0;
        }
    }
    CHECK(rep_indep, "semantic identity depends on the realization");
    /* All eight policy actions must have happened somewhere. */
    uint64_t act_total[JS_ACT_COUNT] = { 0 };
    for (unsigned t = 0; t < N_REAL; t++)
        if (out[t].sh && out[t].sh->ok)
            for (int a = 0; a < JS_ACT_COUNT; a++) act_total[a] += out[t].sh->actions[a];
    int all_actions = 1;
    for (int a = 0; a < JS_ACT_COUNT; a++) if (!act_total[a]) all_actions = 0;
    CHECK(all_actions, "not every policy action was exercised");
    CHECK(g_branches >= 100, "fewer than 100 branches");

    int pass = g_fail == 0;
    char commit[41] = "unknown", physics[64] = "unknown", path[512];
    if (!omega_evidence_run_commit(commit)) strcpy(commit, "unknown");
    if (!omega_evidence_physics_commit(physics, sizeof physics)) strcpy(physics, "unknown");
    int dirty = omega_evidence_tree_dirty();
    const char *gate = !pass ? "FAIL" : dirty ? "PASS_UNBOUND_DIRTY_TREE" : "PASS";

    if (omega_evidence_path("BRANCH_REUSE/rx_branch_reuse_receipt.json", path, sizeof path) == 0) {
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "{\n  \"gate\": \"OMEGA_BRANCH_STATE_REUSE\",\n  \"result\": \"%s\",\n"
                       "  \"run_id\": \"%s\",\n  \"commit\": \"%s\",\n  \"tree_dirty\": %s,\n"
                       "  \"physics_commit\": \"%s\",\n  \"energy_counters\": \"%s\",\n"
                       "  \"branches\": %u,\n  \"prefix_units\": %u,\n  \"diverge_units\": %u,\n"
                       "  \"edits_per_branch\": %u,\n  \"unit_bytes\": %u,\n"
                       "  \"representation_independent_identity\": %s,\n  \"all_actions_exercised\": %s,\n"
                       "  \"realizers\": [\n",
                    gate, omega_evidence_run_id(), commit, dirty ? "true" : "false", physics,
                    g_hwmon[0] ? g_hwmon : "none", g_branches, g_prefix, g_diverge, EDITS, UNIT_BYTES,
                    rep_indep ? "true" : "false", all_actions ? "true" : "false");
            for (unsigned t = 0; t < N_REAL; t++) {
                RunResult *I = out[t].ind, *S = out[t].sh;
                if (!I || !S) continue;
                char h[65];
                hex(S->semid[0], h);
                fprintf(f, "    {\"realizer\": \"%s\", \"type\": \"%s\", \"correct\": %s,"
                           " \"bytes_equal_independent\": %s, \"ancestor_intact\": %s, \"frozen_refused\": %s,"
                           " \"cow_isolated\": %s, \"reuse_ok\": %s, \"pressure_ok\": %s, \"placement_cycle_ok\": %s,\n"
                           "     \"branch0_semantic_id\": \"%s\",\n"
                           "     \"independent\": {\"held_bytes\": %llu, \"rss_growth\": %llu, \"derives\": %llu,"
                           " \"cpu_ns\": %llu, \"wall_ns\": %llu, \"first_ready_ns\": %llu, \"branch_ready_mean_ns\": %.0f,"
                           " \"branch_create_median_ns\": %.0f, \"branch_create_p99_ns\": %.0f,"
                           " \"energy_ok\": %s, \"energy_uj_per_construction\": {\"pkg\": %llu, \"cpu_e\": %llu, \"cpu_p\": %llu},"
                           " \"energy_constructions\": %llu, \"energy_window_ns\": %llu},\n"
                           "     \"shared\": {\"held_bytes\": %llu, \"rss_growth\": %llu, \"derives\": %llu,"
                           " \"cpu_ns\": %llu, \"wall_ns\": %llu, \"first_ready_ns\": %llu, \"branch_ready_mean_ns\": %.0f,"
                           " \"branch_create_median_ns\": %.0f, \"branch_create_p99_ns\": %.0f, \"branch_create_max_ns\": %.0f,"
                           " \"energy_ok\": %s, \"energy_uj_per_construction\": {\"pkg\": %llu, \"cpu_e\": %llu, \"cpu_p\": %llu},"
                           " \"energy_constructions\": %llu, \"energy_window_ns\": %llu, \"live_realizations\": %llu, \"bytes_copied\": %llu},\n"
                           "     \"measured_costs\": {\"derive_ns\": %.1f, \"copy_ns_per_byte\": %.4f,"
                           " \"compress_ns_per_byte\": %.4f, \"decompress_ns_per_byte\": %.4f, \"compress_ratio\": %.4f,"
                           " \"spill_write_ns_per_byte\": %.4f, \"spill_read_ns_per_byte\": %.4f,"
                           " \"retain_ns_per_byte\": %.4f},\n     \"actions\": {",
                        out[t].rz->name, js_real_type_name(out[t].rz->type), out[t].correct ? "true" : "false",
                        out[t].digests_match ? "true" : "false", S->ancestor_intact ? "true" : "false",
                        S->frozen_refused ? "true" : "false", S->cow_isolated ? "true" : "false",
                        S->reuse_ok ? "true" : "false", S->pressure_ok ? "true" : "false",
                        S->placement_cycle_ok ? "true" : "false", h,
                        U(I->held_bytes), U(I->rss_growth), U(I->derives), U(I->cpu_ns), U(I->wall_ns),
                        U(I->first_ready_ns), I->branch_ready_mean_ns, I->create_median_ns, I->create_p99_ns,
                        I->energy_ok ? "true" : "false", U(I->energy_uj[0]), U(I->energy_uj[1]), U(I->energy_uj[2]),
                        U(I->energy_reps), U(I->energy_window_ns),
                        U(S->held_bytes), U(S->rss_growth), U(S->derives), U(S->cpu_ns), U(S->wall_ns),
                        U(S->first_ready_ns), S->branch_ready_mean_ns, S->create_median_ns, S->create_p99_ns,
                        S->create_max_ns, S->energy_ok ? "true" : "false", U(S->energy_uj[0]), U(S->energy_uj[1]),
                        U(S->energy_uj[2]), U(S->energy_reps), U(S->energy_window_ns),
                        U(S->live_reals), U(S->bytes_copied),
                        S->costs.derive_ns, S->costs.copy_ns_per_byte, S->costs.compress_ns_per_byte,
                        S->costs.decompress_ns_per_byte, S->costs.compress_ratio, S->costs.spill_write_ns_per_byte,
                        S->costs.spill_read_ns_per_byte, S->costs.retain_ns_per_byte);
                for (int a = 0; a < JS_ACT_COUNT; a++)
                    fprintf(f, "%s\"%s\": %llu", a ? ", " : "", js_action_name((JsAction)a), U(S->actions[a]));
                fprintf(f, "},\n     \"policy\": {\"budget_bytes\": %llu, \"considered\": %llu, \"kept\": %llu,"
                           " \"move\": %llu, \"compress\": %llu, \"spill\": %llu, \"evict\": %llu,"
                           " \"resident_after\": %llu, \"spilled_after\": %llu,"
                           " \"placements_after\": {\"hot\": %u, \"cold\": %u, \"compressed\": %u, \"spilled\": %u, \"evicted\": %u}}}%s\n",
                        U(S->budget_bytes), U(S->policy.considered), U(S->policy.kept),
                        U(S->policy.chosen[JS_ACT_MOVE]), U(S->policy.chosen[JS_ACT_COMPRESS]),
                        U(S->policy.chosen[JS_ACT_SPILL]), U(S->policy.chosen[JS_ACT_EVICT]),
                        U(S->resident_after_policy), U(S->spilled_after_policy),
                        S->placement_after[JS_PLACE_HOT], S->placement_after[JS_PLACE_COLD],
                        S->placement_after[JS_PLACE_COMPRESSED], S->placement_after[JS_PLACE_SPILLED],
                        S->placement_after[JS_PLACE_EVICTED], t + 1 < N_REAL ? "," : "");
            }
            fprintf(f, "  ],\n  \"failures\": %d\n}\n", g_fail);
            fclose(f);
        }
    }
    printf("\nrepresentation-independent identity: %s; all eight actions exercised: %s\n",
           rep_indep ? "yes" : "NO", all_actions ? "yes" : "NO");
    if (pass && !dirty) printf("OMEGA_BRANCH_STATE_REUSE_PASS\n");
    printf("gate: OMEGA_BRANCH_STATE_REUSE=%s\nreceipt: %s\n", gate, path);
    return pass ? 0 : 1;
}
