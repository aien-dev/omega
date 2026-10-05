/*
 * rx_r15_perf.c -- the R15 performance harness (spec/r15-performance-proof.md).
 *
 * One process = one observation unit. It measures and records; it decides
 * nothing. Every number is written raw to a JSON Lines file (§12) and the
 * reducer (tools/r15_reduce.c) computes every summary and gate from it.
 *
 *   rx_r15_perf trial <RES4|RES1|SEQ> <run> <round> <out.jsonl>
 *       one W-EPISODE trial (§4): IDLE, BEFORE, ADAPT, AFTER windows.
 *   rx_r15_perf l1 <A|B|C|D|E|G|W0|W1> <RES1|SEQ> <run> <round> <out.jsonl>
 *       Level 1 substrate measures (§5, C1). W0/W1 = W-PROD with the timing
 *       buffer off/on (C1 item 6, instrumentation overhead).
 *   rx_r15_perf l2 <RES1|SEQ> <run> <round> <out.jsonl>
 *       Level 2 CPU/GPU cooperation through the resident seat (§5).
 *
 * The configuration name recorded is RES-1-NODIGEST when this binary was
 * built with -DRX_MEASURE_NO_CAUSAL_DIGEST (spec §3). Exit status 0 means the
 * observation completed and its own correctness checks held; anything else
 * is a failed trial (§10: it is recorded, never rerun away).
 */
#include "rx_r15_rig.h"
#include "r15_measure.h"
#include "r15_seat_diag.h"
#include "runtime/rx_generation.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum { EXTERNAL = 100, ISSUER = 3, L1_SUBJ = 2 };

#define WARM_NS   2000000000ull
#define WINDOW_NS 5000000000ull
#define IDLE_NS   5000000000ull

static R15Out g_out;
static R15Energy g_energy;
static R15Pmu g_pmu;
static int g_pmu_ok;
static char g_config[24];

static void sleep_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) != 0) {}
}

static void rec_str(const char *k, const char *v) {
    fprintf(g_out.f, ",\"%s\":\"", k);
    for (const char *p = v; p && *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', g_out.f);
        if ((unsigned char)*p >= 0x20) fputc(*p, g_out.f);
    }
    fputc('"', g_out.f);
}
static void rec_u(const char *k, uint64_t v) { fprintf(g_out.f, ",\"%s\":%" PRIu64, k, v); }
static void rec_i(const char *k, int64_t v) { fprintf(g_out.f, ",\"%s\":%" PRId64, k, v); }
static void rec_arr(const char *k, const uint64_t *v, uint64_t n) {
    fprintf(g_out.f, ",\"%s\":[", k);
    for (uint64_t i = 0; i < n; i++) fprintf(g_out.f, "%s%" PRIu64, i ? "," : "", v[i]);
    fputc(']', g_out.f);
}

/* ---- machine state (§9, §13) ------------------------------------------- */

static int max_temp_mc(void) {
    int hot = 0;
    for (int z = 0; z < 64; z++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        FILE *f = fopen(p, "r");
        if (!f) break;
        int t = 0;
        if (fscanf(f, "%d", &t) == 1 && t > hot) hot = t;
        fclose(f);
    }
    return hot;
}

static void rec_machine(const char *when) {
    r15_rec_begin(&g_out, "machine");
    rec_str("when", when);
    fputs(",\"thermal_mc\":[", g_out.f);
    for (int z = 0; z < 64; z++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        FILE *f = fopen(p, "r");
        if (!f) break;
        int t = 0;
        if (fscanf(f, "%d", &t) != 1) t = -1;
        fclose(f);
        fprintf(g_out.f, "%s%d", z ? "," : "", t);
    }
    fputs("],\"cur_khz\":[", g_out.f);
    for (int c = 0; c < 256; c++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        FILE *f = fopen(p, "r");
        if (!f) break;
        long k = 0;
        if (fscanf(f, "%ld", &k) != 1) k = -1;
        fclose(f);
        fprintf(g_out.f, "%s%ld", c ? "," : "", k);
    }
    fputc(']', g_out.f);
    double la[3] = {0};
    if (getloadavg(la, 3) == 3) fprintf(g_out.f, ",\"loadavg\":[%.2f,%.2f,%.2f]", la[0], la[1], la[2]);
    rec_i("causal_digest", rx_world_causal_digest_enabled);
    r15_rec_end(&g_out);
}

/* §9: wait up to 120 s for every zone <= 55 C; run and flag otherwise. */
static void thermal_wait(void) {
    uint64_t t0 = r15_now_ns();
    int hot;
    while ((hot = max_temp_mc()) > 55000 && r15_now_ns() - t0 < 120000000000ull) sleep_ns(1000000000ull);
    r15_rec_begin(&g_out, "thermal_wait");
    rec_u("waited_ns", r15_now_ns() - t0);
    rec_i("max_mc", hot);
    rec_i("flag_hot", hot > 55000);
    r15_rec_end(&g_out);
}

/* ---- GPU residency and R5 slot sampler (1 ms; §6.10, §6.11) ------------- */

/* One accumulator per open window (windows may overlap), plus whole-process
 * residency totals that are never reset. */
typedef struct {
    uint64_t samples, live, slot_sum, slot_max, inflight_sum, inflight_max, claims_max;
    SeatDiag d;   /* sampler lateness and not-live runs (r15_seat_diag.h); does not feed G15 */
} SampSnap;
#define SAMP_ACC 4

typedef struct {
    pthread_t t;
    volatile int stop, armed;
    RxWorld *volatile w;
    volatile int silicon;
    pthread_mutex_t mu;
    uint64_t last, have_last;
    SampSnap acc[SAMP_ACC];
    int active[SAMP_ACC];
    SampSnap total;
} Sampler;
static Sampler g_samp;

static void *sampler_main(void *arg) {
    Sampler *s = arg;
    while (!s->stop) {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
        if (!s->armed || !s->w) continue;
        RxWorld *w = s->w;
        uint64_t slots = __atomic_load_n(&w->used_slots, __ATOMIC_RELAXED);
        uint64_t infl = __atomic_load_n(&w->in_flight, __ATOMIC_RELAXED);
        uint64_t open = __atomic_load_n(&w->stats.resident_claims, __ATOMIC_RELAXED) -
                        __atomic_load_n(&w->stats.resident_closed, __ATOMIC_RELAXED);
        uint64_t hb = 0;
        if (s->silicon && w->coherent)
            hb = __atomic_load_n((uint32_t *)(w->coherent + rx_world_off_heartbeat() + RX_SEAT_HB_LIVE),
                                 __ATOMIC_ACQUIRE);
        /* live = the seat's counter moved since the previous 1 ms sample */
        int live = s->silicon && s->have_last && hb != s->last;
        int counted = s->have_last;
        s->last = hb;
        s->have_last = 1;
        uint64_t t_now = r15_now_ns();
        if (!counted) continue;
        pthread_mutex_lock(&s->mu);
        for (int i = 0; i <= SAMP_ACC; i++) {
            SampSnap *a = i == SAMP_ACC ? &s->total : &s->acc[i];
            if (i < SAMP_ACC && !s->active[i]) continue;
            a->samples++;
            a->live += (uint64_t)live;
            seat_diag_sample(&a->d, t_now, live, (uint32_t)hb);
            a->slot_sum += slots;
            if (slots > a->slot_max) a->slot_max = slots;
            a->inflight_sum += infl;
            if (infl > a->inflight_max) a->inflight_max = infl;
            if (open > a->claims_max) a->claims_max = open;
        }
        pthread_mutex_unlock(&s->mu);
    }
    return NULL;
}

static int samp_open(void) {
    pthread_mutex_lock(&g_samp.mu);
    int k = -1;
    for (int i = 0; i < SAMP_ACC && k < 0; i++)
        if (!g_samp.active[i]) { k = i; memset(&g_samp.acc[i], 0, sizeof g_samp.acc[i]); g_samp.active[i] = 1; }
    pthread_mutex_unlock(&g_samp.mu);
    return k;
}

static SampSnap samp_close(int k) {
    SampSnap x;
    memset(&x, 0, sizeof x);
    if (k < 0) return x;
    pthread_mutex_lock(&g_samp.mu);
    x = g_samp.acc[k];
    g_samp.active[k] = 0;
    pthread_mutex_unlock(&g_samp.mu);
    return x;
}

static void rec_samp(const char *key, const SampSnap *x) {
    fprintf(g_out.f, ",\"%s\":{\"silicon\":%d,\"intervals\":%" PRIu64 ",\"seat_live\":%" PRIu64
                     ",\"slot_sum\":%" PRIu64 ",\"slot_max\":%" PRIu64 ",\"inflight_sum\":%" PRIu64
                     ",\"inflight_max\":%" PRIu64 ",\"claims_max\":%" PRIu64
                     ",\"diag\":{\"late_2ms\":%" PRIu64 ",\"late_10ms\":%" PRIu64 ",\"max_gap_ns\":%" PRIu64
                     ",\"dead_runs\":%" PRIu64 ",\"dead_longest\":%" PRIu64 ",\"dead_at_end\":%" PRIu64
                     ",\"t_first\":%" PRIu64 ",\"t_last\":%" PRIu64 ",\"first_dead_t\":%" PRIu64
                     ",\"last_live_t\":%" PRIu64 ",\"stale_max_ns\":%" PRIu64 "}}",
            key, g_samp.silicon, x->samples, x->live, x->slot_sum, x->slot_max, x->inflight_sum,
            x->inflight_max, x->claims_max, x->d.late_2ms, x->d.late_10ms, x->d.max_gap_ns, x->d.dead_runs,
            x->d.dead_longest, x->d.dead_cur, x->d.t_first, x->d.last_t, x->d.first_dead_t, x->d.last_live_t,
            x->d.stale_max_ns);
}

static void rec_residency(void) {
    pthread_mutex_lock(&g_samp.mu);
    SampSnap t = g_samp.total;
    pthread_mutex_unlock(&g_samp.mu);
    r15_rec_begin(&g_out, "residency");
    rec_samp("total", &t);
    r15_rec_end(&g_out);
}

/* Observers are created before the PMU is opened, so their own work is not
 * counted as the body's (C1 item 8: inherit covers threads created later). */
static void observers_start(void) {
    char why[200] = "";
    int e = r15_energy_open(&g_energy, why, sizeof why);
    r15_energy_start_sampler(&g_energy, 64000);
    pthread_mutex_init(&g_samp.mu, NULL);
#ifdef R15_SILICON
    g_samp.silicon = 1;
#endif
    pthread_create(&g_samp.t, NULL, sampler_main, &g_samp);
    char pwhy[200] = "";
    g_pmu_ok = r15_pmu_open(&g_pmu, pwhy, sizeof pwhy) == 0;
    r15_rec_begin(&g_out, "observers");
    rec_i("spbm", e == 0);
    rec_str("spbm_why", why);
    rec_str("hwmon", g_energy.hwmon);
    rec_i("nvml", g_energy.nvml_dev != NULL);
    rec_i("pmu", g_pmu_ok);
    rec_str("pmu_why", pwhy);
    rec_i("silicon", g_samp.silicon);
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    int par = -99;
    if (f) { if (fscanf(f, "%d", &par) != 1) par = -99; fclose(f); }
    rec_i("perf_event_paranoid", par);
    r15_rec_end(&g_out);
}

static void observers_stop(uint64_t t0) {
    g_samp.stop = 1;
    pthread_join(g_samp.t, NULL);
    /* 10 Hz telemetry over the whole process (§7) */
    static R15EnergySample tel[64000];
    uint64_t n = r15_energy_samples(&g_energy, t0, UINT64_MAX, tel, 64000);
    for (uint64_t i = 0; i < n; i++) {
        r15_rec_begin(&g_out, "telemetry");
        r15_json_energy(g_out.f, "e", &tel[i]);
        r15_rec_end(&g_out);
    }
    r15_energy_close(&g_energy);
    if (g_pmu_ok) r15_pmu_close(&g_pmu);
}

/* ---- live windows ------------------------------------------------------- */

#define STAT_FIELDS(X) \
    X(activations) X(externals) X(wakes) X(wakes_accepted) X(ready_inserts)               \
    X(subscriptions_checked) X(subscriptions_matched) X(coalesced_wakes) X(commits)         \
    X(invalidations) X(rejected) X(noops) X(failed) X(blocked_authority) X(blocked_resource) \
    X(quarantines) X(suppressed_wakes) X(deferred_wakes) X(snapshot_bytes) X(stage_bytes)   \
    X(crumb_bytes) X(proj_bytes) X(c2g_write_bytes) X(c2g_read_bytes) X(g2c_write_bytes)    \
    X(g2c_read_bytes) X(window_move_bytes) X(setup_copy_bytes) X(sched_wall_ns)             \
    X(sched_cpu_ns) X(gpu_results_taken) X(gpu_polls_empty) X(gpu_host_waits) X(seq_pulses) \
    X(seq_polls) X(seq_runs) X(resident_claims) X(resident_closed) X(crumb_overflow)        \
    X(illegal_transitions) X(desc_published)

typedef struct {
    uint64_t t0, t1, served0, served1;
    RxStats s0, s1;
    struct rusage ru0, ru1;
    R15EnergySample e0, e1;
    R15PmuRead pmu;
    int pmu_used;
    uint64_t gio_b0, gio_s0, gio_b1, gio_s1, pio_b0, pio_s0, pio_b1, pio_s1;
    char threads0[4096], threads1[4096];
    int acc;
    SampSnap samp;
} Win;

static RxStats stats_of(RxWorld *w) {
    pthread_mutex_lock(&w->mu);
    RxStats s = w->stats;
    pthread_mutex_unlock(&w->mu);
    return s;
}

/* Per-thread CPU (utime+stime, clock ticks) and name, "tid:comm:ticks;..." */
static void thread_cpu(char *buf, size_t n) {
    buf[0] = 0;
    size_t at = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && at + 64 < n) {
        if (e->d_name[0] == '.') continue;
        char p[128], line[512];
        snprintf(p, sizeof p, "/proc/self/task/%.20s/stat", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        size_t got = fread(line, 1, sizeof line - 1, f);
        fclose(f);
        line[got] = 0;
        char *l = strchr(line, '('), *r = strrchr(line, ')');
        if (!l || !r) continue;
        *r = 0;
        unsigned long ut = 0, st = 0;
        if (sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
        for (char *c = l + 1; *c; c++) if (*c == ';' || *c == ':' || *c == '"' || *c == '\\') *c = '_';
        at += (size_t)snprintf(buf + at, n - at, "%s:%.15s:%lu;", e->d_name, l + 1, ut + st);
    }
    closedir(d);
}

static void win_begin(Win *x, RxWorld *w, R15Rig *r, int pmu) {
    memset(x, 0, sizeof *x);
    thread_cpu(x->threads0, sizeof x->threads0);
    if (r) rx_gen_store_io(r->gen, &x->gio_b0, &x->gio_s0);
    rx_gen_io_counters(&x->pio_b0, &x->pio_s0);
    x->served0 = r ? __atomic_load_n(&r->served, __ATOMIC_RELAXED) : 0;
    x->s0 = stats_of(w);
    getrusage(RUSAGE_SELF, &x->ru0);
    x->acc = samp_open();
    r15_energy_read(&g_energy, &x->e0);
    x->pmu_used = pmu && g_pmu_ok;
    x->t0 = r15_now_ns();
    if (x->pmu_used) r15_pmu_start(&g_pmu);
}

static void win_end(Win *x, RxWorld *w, R15Rig *r) {
    if (x->pmu_used) r15_pmu_stop(&g_pmu, &x->pmu);
    x->t1 = r15_now_ns();
    x->samp = samp_close(x->acc);
    r15_energy_read(&g_energy, &x->e1);
    getrusage(RUSAGE_SELF, &x->ru1);
    x->s1 = stats_of(w);
    x->served1 = r ? __atomic_load_n(&r->served, __ATOMIC_RELAXED) : 0;
    rx_gen_io_counters(&x->pio_b1, &x->pio_s1);
    if (r) rx_gen_store_io(r->gen, &x->gio_b1, &x->gio_s1);
    thread_cpu(x->threads1, sizeof x->threads1);
}

static uint64_t tv_ns(struct timeval t) { return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_usec * 1000ull; }

static void rec_window(const char *name, const Win *x) {
    r15_rec_begin(&g_out, "window");
    rec_str("window", name);
    rec_u("t0", x->t0);
    rec_u("t1", x->t1);
    rec_u("wall_ns", x->t1 - x->t0);
    rec_u("ops", x->served1 - x->served0);
    rec_u("cpu_user_ns", tv_ns(x->ru1.ru_utime) - tv_ns(x->ru0.ru_utime));
    rec_u("cpu_sys_ns", tv_ns(x->ru1.ru_stime) - tv_ns(x->ru0.ru_stime));
    fputs(",\"stats\":{", g_out.f);
    int first = 1;
#define X(fld) fprintf(g_out.f, "%s\"" #fld "\":%" PRIu64, first ? "" : ",", x->s1.fld - x->s0.fld); first = 0;
    STAT_FIELDS(X)
#undef X
    fputc('}', g_out.f);
    r15_json_energy(g_out.f, "e0", &x->e0);
    r15_json_energy(g_out.f, "e1", &x->e1);
    if (x->pmu_used) r15_json_pmu(g_out.f, "pmu", &g_pmu, &x->pmu);
    rec_u("gen_store_bytes", x->gio_b1 - x->gio_b0);
    rec_u("gen_store_syncs", x->gio_s1 - x->gio_s0);
    rec_u("gen_process_bytes", x->pio_b1 - x->pio_b0);
    rec_u("gen_process_syncs", x->pio_s1 - x->pio_s0);
    rec_str("threads0", x->threads0);
    rec_str("threads1", x->threads1);
    rec_samp("sampler", &x->samp);
    r15_rec_end(&g_out);
}

/* ---- crumb-derived windows (§5 L3, §6.2, §6.12-15) ---------------------- */

typedef struct {
    uint64_t t0, t1;
    uint64_t act, kind[16], replays, serve_commits, externals, consequential;
    uint64_t cost_ns;           /* Σ(t_end - t_start), non-production activations */
    R15Hist lat;                /* activation latency: t_start - cause t_end */
} CWin;

static int writes_obj(const RxCrumb *k, uint32_t id) {
    for (uint32_t i = 0; i < k->n_outputs; i++) if (k->outputs[i].obj.id == id) return 1;
    return 0;
}

/* Crumbs whose activation started in [t0, t1]. cause_has_commit[e] = 1 when
 * some reaction commit belongs to causal episode e. */
static void cwin_scan(RxWorld *w, uint32_t r_serve, const uint8_t *ep_commit, uint64_t n, CWin *c) {
    static uint64_t last_inv[RX_MAX_REACTIONS];
    memset(last_inv, 0, sizeof last_inv);
    /* n: the crumb count ep_commit was sized for */
    for (uint64_t id = 1; id <= n; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (!k) continue;
        if (k->kind == RX_CRUMB_INVALIDATED && k->reaction < RX_MAX_REACTIONS)
            last_inv[k->reaction] = k->wake_cause;
        if (k->t_start_ns < c->t0 || k->t_start_ns > c->t1) continue;
        if (k->kind == RX_CRUMB_EXTERNAL) {
            c->externals++;
            if (ep_commit[id]) c->consequential++;
            continue;
        }
        if (k->reaction == UINT32_MAX || k->kind == RX_CRUMB_CREATE || k->kind == RX_CRUMB_RETIRE) continue;
        c->act++;
        if ((unsigned)k->kind < 16) c->kind[k->kind]++;
        if (k->kind != RX_CRUMB_INVALIDATED && k->reaction < RX_MAX_REACTIONS &&
            last_inv[k->reaction] && last_inv[k->reaction] == k->wake_cause) {
            c->replays++;
            last_inv[k->reaction] = 0;
        }
        if (k->reaction == r_serve) {
            if (k->kind == RX_CRUMB_COMMIT) c->serve_commits++;
        } else {
            c->cost_ns += k->t_end_ns - k->t_start_ns;
        }
        const RxCrumb *cause = k->wake_cause ? rx_world_crumb(w, k->wake_cause) : NULL;
        if (cause && k->t_start_ns >= cause->t_end_ns) r15_hist_add(&c->lat, k->t_start_ns - cause->t_end_ns);
    }
}

static void rec_cwin(const char *name, const CWin *c) {
    r15_rec_begin(&g_out, "cwindow");
    rec_str("window", name);
    rec_u("t0", c->t0);
    rec_u("t1", c->t1);
    rec_u("activations", c->act);
    fprintf(g_out.f, ",\"kinds\":{\"commit\":%" PRIu64 ",\"invalidated\":%" PRIu64
                     ",\"blocked_authority\":%" PRIu64 ",\"rejected\":%" PRIu64 ",\"failed\":%" PRIu64
                     ",\"noop\":%" PRIu64 ",\"quarantine\":%" PRIu64 "}",
            c->kind[RX_CRUMB_COMMIT], c->kind[RX_CRUMB_INVALIDATED], c->kind[RX_CRUMB_BLOCKED_AUTHORITY],
            c->kind[RX_CRUMB_REJECTED], c->kind[RX_CRUMB_FAILED], c->kind[RX_CRUMB_NOOP],
            c->kind[RX_CRUMB_QUARANTINE]);
    rec_u("replays", c->replays);
    rec_u("serve_commits", c->serve_commits);
    rec_u("externals", c->externals);
    rec_u("consequential", c->consequential);
    rec_u("adapt_cost_ns", c->cost_ns);
    fputs(",\"latency\":", g_out.f);
    r15_hist_json(g_out.f, &c->lat);
    r15_rec_end(&g_out);
}

/* ---- trial (§4) ----------------------------------------------------------- */

typedef struct { Win idle, before, span, after; int idle_ok, before_ok, after_ok; uint64_t t_met; } TrialWins;

static int hook_idle(R15Hooks *h, R15Rig *r) {
    TrialWins *t = h->ctx;
    if (rx_world_wait_quiescent(&r->w, 10000) != RX_OK) return -1;
    win_begin(&t->idle, &r->w, r, 1);
    sleep_ns(IDLE_NS);
    win_end(&t->idle, &r->w, r);
    t->idle_ok = 1;
    return 0;
}

static int hook_before(R15Hooks *h, R15Rig *r) {
    TrialWins *t = h->ctx;
    sleep_ns(WARM_NS);
    win_begin(&t->before, &r->w, r, 1);
    sleep_ns(WINDOW_NS);
    win_end(&t->before, &r->w, r);
    t->before_ok = 1;
    win_begin(&t->span, &r->w, r, 0);    /* goal .. end of AFTER (G12) */
    return 0;
}

static int hook_after(R15Hooks *h, R15Rig *r) {
    TrialWins *t = h->ctx;
    t->t_met = r15_now_ns();   /* goal MET observed on the in-force record */
    sleep_ns(WARM_NS);
    win_begin(&t->after, &r->w, r, 1);
    sleep_ns(WINDOW_NS);
    win_end(&t->after, &r->w, r);
    win_end(&t->span, &r->w, r);
    t->after_ok = 1;
    return 0;
}

/* First crumb after `from` by reaction `rx` (UINT32_MAX = any) that commits
 * a write to object `obj` (UINT32_MAX = any). */
static const RxCrumb *find_commit(RxWorld *w, uint64_t from, uint32_t rx, uint32_t obj) {
    for (uint64_t id = from + 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (!k || k->kind != RX_CRUMB_COMMIT) continue;
        if (rx != UINT32_MAX && k->reaction != rx) continue;
        if (obj != UINT32_MAX && !writes_obj(k, obj)) continue;
        return k;
    }
    return NULL;
}

static uint32_t reaction_named(RxWorld *w, const char *name) {
    for (uint32_t i = 0; i < w->n_reactions; i++)
        if (w->reactions[i].desc.name && strcmp(w->reactions[i].desc.name, name) == 0) return i;
    return UINT32_MAX;
}

static void rec_outcome(const R15Outcome *o, int rc) {
    r15_rec_begin(&g_out, "episode");
    rec_i("rc", rc);
    rec_i("ok", o->ok);
    rec_str("why", o->why);
#define U(f) rec_u(#f, o->f);
    U(target_ns) U(goal_seq) U(goal_regime) U(goal_status) U(goal_status_final) U(goal_class) U(plan_action)
    U(plan_regime) U(plan_condition) U(plan_reason) U(plan_goal) U(search_epoch)
    U(selection_epoch) U(selection_regime) U(selected_verdict) U(gen_before) U(gen_after)
    U(lineage_before) U(lineage_after) U(inforce_epoch) U(inforce_regime) U(inforce_generation)
    U(candidate_id) U(promotion_candidate) U(promotion_active) U(aegis_woken) U(root_woken)
    U(gpu_claims) U(seat_commits) U(crumbs_checked) U(crumb_overflow) U(illegal) U(wrong)
    U(unpromoted_use) U(lost_triggers) U(incumbent_ns) U(selected_ps) U(reference_ps)
    U(inforce_ps) U(final_expected_ns) U(plan_seq) U(served) U(crumbs)
#undef U
    rec_i("promote_result", o->promote_result);
    rec_i("crumbs_verified", o->crumbs_verified);
    rec_arr("selection_id", o->selection_id, 4);
    rec_arr("inforce_id", o->inforce_id, 4);
    rec_arr("evidence", o->evidence, 5);
    rec_arr("belief", o->belief, 4);
    rec_arr("slot_verdict", o->slot_verdict, RX_OMEGA_SLOTS);
    rec_arr("slot_measure", o->slot_measure, RX_OMEGA_SLOTS);
    rec_arr("slot_cps", o->slot_cps, RX_OMEGA_SLOTS);
    rec_arr("slot_rps", o->slot_rps, RX_OMEGA_SLOTS);
    r15_rec_end(&g_out);
}

/* Diagnostic only (never part of the raw evidence): with R15_BARRIER_TRACE
 * set to a file name, write every crumb around the trial's generation barrier
 * and the barrier's inner phases, one line each. */
static void barrier_trace(RxWorld *w, const RxGenPhases *ph) {
    const char *path = getenv("R15_BARRIER_TRACE");
    if (!path || !ph->enter_ns) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    uint64_t lo = ph->enter_ns - 20000000ull;
    uint64_t hi = (ph->receipt_ns ? ph->receipt_ns : ph->enter_ns) + 20000000ull;
    fprintf(f, "phase enter %" PRIu64 " barrier %" PRIu64 " verified %" PRIu64 " blobs %" PRIu64
            " candidate %" PRIu64 " reachable %" PRIu64 " flip %" PRIu64 " flipped %" PRIu64
            " receipt_file %" PRIu64 " event %" PRIu64 " receipt %" PRIu64 " result %d\n",
            ph->enter_ns, ph->barrier_ns, ph->verified_ns, ph->blobs_ns, ph->candidate_ns,
            ph->reachable_ns, ph->flip_ns, ph->flipped_ns, ph->receipt_file_ns, ph->event_ns,
            ph->receipt_ns, ph->result);
    for (uint64_t id = 1; id <= w->n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (!k || k->t_end_ns < lo || k->t_start_ns > hi) continue;
        const char *name = k->reaction < w->n_reactions ? w->reactions[k->reaction].desc.name
                                                        : "(outside)";
        fprintf(f, "crumb %" PRIu64 " kind %d worker %u start %" PRIu64 " end %" PRIu64 " %s\n",
                k->id, (int)k->kind, k->worker, k->t_start_ns, k->t_end_ns, name);
    }
    fclose(f);
}

static int run_trial(R15Config cfg) {
    thermal_wait();
    rec_machine("before");
    uint64_t t_proc = r15_now_ns();
    observers_start();
    static R15Rig rig;
    R15Rig *r = &rig;
    TrialWins tw;
    memset(&tw, 0, sizeof tw);
    R15Hooks hooks = {hook_idle, hook_before, hook_after, &tw};
    if (r15_start(r, cfg) != 0) {
        r15_rec_begin(&g_out, "error");
        rec_str("what", "build");
        rec_i("stage", r->stage);
        rec_str("stage_why", r->stage_why ? r->stage_why : "");
        r15_rec_end(&g_out);
        observers_stop(t_proc);
        return 2;
    }
    g_samp.w = &r->w;
    g_samp.armed = 1;
    r->hooks = &hooks;
    R15Outcome out;
    uint64_t t_ep = r15_now_ns();
    int rc = r15_episode(r, 0, &out);
    uint64_t t_done = r15_now_ns();
    /* A failed episode returns with production still running: stop it and let
     * the body go quiet so the crumb log no longer grows under the analysis. */
    r15_producer_stop(r);
    rx_world_wait_quiescent(&r->w, 10000);
    rec_outcome(&out, rc);
    if (tw.idle_ok) rec_window("IDLE", &tw.idle);
    if (tw.before_ok) rec_window("BEFORE", &tw.before);
    if (tw.after_ok) { rec_window("AFTER", &tw.after); rec_window("ADAPT_AFTER", &tw.span); }

    /* crumb-derived ADAPT (goal crumb .. promotion crumb) and L3 intervals */
    RxWorld *w = &r->w;
    uint32_t r_serve = r->omega.r_serve;
    const uint64_t n_crumbs = w->n_crumbs;   /* one snapshot bounds every scan below */
    uint8_t *ep = calloc(n_crumbs + 2, 1);
    for (uint64_t id = 1; ep && id <= n_crumbs; id++) {
        const RxCrumb *k = rx_world_crumb(w, id);
        if (k && k->kind == RX_CRUMB_COMMIT && k->reaction != UINT32_MAX && k->episode &&
            k->episode <= n_crumbs)
            ep[k->episode] = 1;
    }
    const RxCrumb *goal = r->goal_crumb > 0 ? rx_world_crumb(w, (uint64_t)r->goal_crumb) : NULL;
    uint64_t g = goal ? (uint64_t)r->goal_crumb : 0;
    const RxCrumb *plan = goal ? find_commit(w, g, reaction_named(w, "aien.plan"), UINT32_MAX) : NULL;
    const RxCrumb *sel = plan ? find_commit(w, plan->id, reaction_named(w, "omega.select"), UINT32_MAX) : NULL;
    const RxCrumb *claim = NULL, *evid = NULL;
    const RxCrumb *cand = goal ? find_commit(w, g, UINT32_MAX, r->living.o.candidate.id) : NULL;
    const RxCrumb *promo = goal ? find_commit(w, g, UINT32_MAX, r->living.o.promotion.id) : NULL;
    const RxCrumb *inforce = goal ? find_commit(w, g, UINT32_MAX, r->living.o.inforce.id) : NULL;
    if (sel)
        for (uint64_t id = sel->id + 1; id <= w->n_crumbs; id++) {
            const RxCrumb *k = rx_world_crumb(w, id);
            if (!k || k->reaction != r->living.r_seat) continue;
            claim = k;
            break;
        }
    if (claim && promo)
        for (uint64_t id = claim->id; id < promo->id; id++) {
            const RxCrumb *k = rx_world_crumb(w, id);
            if (k && k->kind == RX_CRUMB_COMMIT && k->reaction == r->living.r_evidence &&
                writes_obj(k, r->living.o.evidence.id))
                evid = k;
        }
    r15_rec_begin(&g_out, "adapt");
    rec_u("t_episode_start", t_ep);
    rec_u("t_episode_end", t_done);
    rec_u("goal_t", goal ? goal->t_end_ns : 0);
    rec_u("plan_t", plan ? plan->t_end_ns : 0);
    rec_u("select_t", sel ? sel->t_end_ns : 0);
    rec_u("claim_t", claim ? claim->t_start_ns : 0);
    rec_u("evidence_t", evid ? evid->t_end_ns : 0);
    rec_u("candidate_t", cand ? cand->t_end_ns : 0);
    rec_u("promotion_t", promo ? promo->t_end_ns : 0);
    rec_u("inforce_t", inforce ? inforce->t_end_ns : 0);
    rec_u("met_t", tw.t_met);
    RxGenPhases ph;
    memset(&ph, 0, sizeof ph);
    rx_gen_last_phases(r->gen, &ph);
    rec_u("barrier_enter", ph.enter_ns);
    rec_u("barrier_begin", ph.barrier_ns);
    rec_u("barrier_flip", ph.flip_ns);
    rec_u("barrier_receipt", ph.receipt_ns);
    rec_i("barrier_result", ph.result);
    uint64_t during = 0;
    if (ph.barrier_ns && ph.receipt_ns)
        for (uint64_t id = 1; id <= w->n_crumbs; id++) {
            const RxCrumb *k = rx_world_crumb(w, id);
            if (k && k->reaction == r_serve && k->kind == RX_CRUMB_COMMIT &&
                k->t_end_ns >= ph.barrier_ns && k->t_end_ns <= ph.receipt_ns)
                during++;
        }
    rec_u("barrier_production_commits", during);
    barrier_trace(w, &ph);
    rec_u("selected_ps", out.selected_ps);
    rec_u("reference_ps", out.reference_ps);
    r15_rec_end(&g_out);
    if (goal && promo && inforce && ep) {
        static CWin adapt;
        memset(&adapt, 0, sizeof adapt);
        adapt.t0 = goal->t_end_ns;
        adapt.t1 = promo->t_end_ns;
        cwin_scan(w, r_serve, ep, n_crumbs, &adapt);
        rec_cwin("ADAPT", &adapt);
        static CWin cost;                       /* goal .. in-force (§5 L3) */
        memset(&cost, 0, sizeof cost);
        cost.t0 = goal->t_end_ns;
        cost.t1 = inforce->t_end_ns;
        cwin_scan(w, r_serve, ep, n_crumbs, &cost);
        rec_cwin("ADAPT_COST", &cost);
    }
    if (tw.before_ok && ep) {
        static CWin b;
        memset(&b, 0, sizeof b);
        b.t0 = tw.before.t0; b.t1 = tw.before.t1;
        cwin_scan(w, r_serve, ep, n_crumbs, &b);
        rec_cwin("BEFORE", &b);
    }
    if (tw.after_ok && ep) {
        static CWin a, s;
        memset(&a, 0, sizeof a);
        a.t0 = tw.after.t0; a.t1 = tw.after.t1;
        cwin_scan(w, r_serve, ep, n_crumbs, &a);
        rec_cwin("AFTER", &a);
        memset(&s, 0, sizeof s);
        s.t0 = tw.span.t0; s.t1 = tw.span.t1;
        cwin_scan(w, r_serve, ep, n_crumbs, &s);
        rec_cwin("ADAPT_AFTER", &s);
    }
    free(ep);
    g_samp.armed = 0;
    rec_residency();
    r15_stop(r);
    observers_stop(t_proc);
    rec_machine("after");
    int complete = rc == 0 && tw.idle_ok && tw.before_ok && tw.after_ok && goal && promo && inforce;
    r15_rec_begin(&g_out, "result");
    rec_i("complete", complete);
    {   /* where the teardown spent its time (CAND-1 window 2: SEQ-06 and RES4-11 waited ~102 s here); reported only */
        char tb[768];
        if (r15_stage_json(&r->stop_clock, tb, sizeof tb) > 0) fputs(tb, g_out.f);
        if (r->leave_valid)
            fprintf(g_out.f, ",\"seat_leave\":{\"rc\":%d,\"marker\":%u,\"sem\":%u}", r->leave_rc, r->leave_marker, r->leave_sem);
    }
    r15_rec_end(&g_out);
    return complete ? 0 : 1;
}

/* ---- Level 1 (§5, pinned X925 cpu 7 driver / cpu 8 worker) --------------- */

#define L1_WARM 1000u
#define L1_N    10000u

typedef struct {
    AienosCapAdmin *admin;
    AienosCapView *view;
    RxWorld w;
    int seq;
    pthread_t orch;
    atomic_int stop, err;
    uint32_t order[RX_MAX_REACTIONS];
    RxSeqPlan plan;
    uint32_t plan_for;
} L1;

static int pin_cpu(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof s, &s);
}

static RxCapRef l1_mint(L1 *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office, ref = {UINT32_MAX, 0};
    aienos_cap_office(e->admin, &office);
    AienosCapMint m = {ISSUER, subject, resource, rights, 0, {UINT32_MAX, 0}, office};
    aienos_cap_mint(e->admin, &m, &ref);
    return (RxCapRef){ref.cap_id, ref.generation};
}

static RxObjRef l1_obj(L1 *e, uint64_t res) {
    uint64_t z[RX_MAX_FIELDS] = {0};
    RxObjRef r = {UINT32_MAX, 0};
    rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, res, z, &r);
    return r;
}

static void *l1_orch(void *arg) {
    L1 *e = arg;
    while (!atomic_load(&e->stop)) {
        pthread_mutex_lock(&e->w.mu);
        if (e->plan_for != e->w.n_reactions) {
            for (uint32_t i = 0; i < e->w.n_reactions; i++) e->order[i] = i;
            e->plan.order = e->order;
            e->plan.n = e->w.n_reactions;
            e->plan_for = e->w.n_reactions;
        }
        pthread_mutex_unlock(&e->w.mu);
        uint32_t ran = 0;
        if (rx_seq_pulse(&e->w, &e->plan, &ran) != RX_OK) { atomic_store(&e->err, 1); break; }
        if (!ran) sched_yield();
    }
    return NULL;
}

static int l1_start(L1 *e, int seq) {
    memset(e, 0, sizeof *e);
    e->seq = seq;
    if (pin_cpu(8) != 0) return -1;       /* workers / orchestrator inherit cpu 8 */
    if (aienos_cap_start(&e->admin, &e->view) != 0) return -1;
    int rc = seq ? rx_world_init_native_sequential_reference(&e->w, e->view, 1u << 24)
                 : rx_world_init_native(&e->w, e->view, 1, 1u << 24);
    if (rc != RX_OK) return -1;
    e->w.external_subject = EXTERNAL;
    RxResourceBudget b = {0};
    b.slots = RX_MAX_REACTIONS;   /* admission never limits L1 fanout (B inserts up to 256) */
    b.memory_bytes = UINT64_MAX;
    b.energy_budget = UINT64_MAX;
    b.offered_locality = UINT32_MAX;
    b.compute_mask = UINT32_MAX;
    rx_world_set_resources(&e->w, &b);
    if (seq) {
        e->plan.gpu_timeout_ms = 5000;
        if (pthread_create(&e->orch, NULL, l1_orch, e) != 0) return -1;
    }
    return pin_cpu(7);
}

static void l1_stop(L1 *e) {
    if (e->seq) { atomic_store(&e->stop, 1); pthread_join(e->orch, NULL); }
    rx_world_destroy(&e->w);
    aienos_cap_stop(e->admin, e->view);
}

static int fn_quiet(RxCtx *c) { (void)c; return 0; }

typedef struct { RxObjRef in, out; } Copy;
static int fn_copy(RxCtx *c) {
    Copy *k = c->user;
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == k->in.id)
            c->out[c->n_out++] = (RxMutation){k->out, 0, c->in[i].field[0]};
    return 0;
}

static volatile uint64_t g_spin_ns;
static int fn_spin(RxCtx *c) {
    (void)c;
    uint64_t until = r15_now_ns() + g_spin_ns;
    while (r15_now_ns() < until) {}
    return 0;
}

static uint32_t l1_reaction(L1 *e, const char *name, RxFn fn, void *user, uint32_t prio,
                            RxObjRef trig, RxCapRef rd, uint64_t res) {
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = L1_SUBJ;
    d.priority = prio;
    d.fn = fn;
    d.user = user;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){trig, RX_FIELD(0)};
    d.n_caps = 1;
    d.caps[0] = (RxCapNeed){rd, res, RX_RIGHT_READ};
    uint32_t id = UINT32_MAX;
    return rx_world_add_reaction(&e->w, &d, &id) == RX_OK ? id : UINT32_MAX;
}

static uint64_t acts_of(RxWorld *w, uint32_t rid) {
    return __atomic_load_n(&w->reactions[rid].activations, __ATOMIC_ACQUIRE);
}

static int wait_acts(RxWorld *w, uint32_t rid, uint64_t want) {
    uint64_t t = r15_now_ns();
    while (acts_of(w, rid) < want) {
        if (r15_now_ns() - t > 5000000000ull) return -1;
    }
    return 0;
}

static void rec_raw(const char *measure, const char *field, const uint64_t *v, uint64_t n,
                    uint64_t extra_k, uint64_t extra_v) {
    r15_rec_begin(&g_out, "l1");
    rec_str("measure", measure);
    rec_str("field", field);
    if (extra_k) rec_u("param", extra_v);
    rec_u("n", n);
    rec_arr("v", v, n);
    r15_rec_end(&g_out);
}

static int timing_ok(RxWorld *w, const char *measure) {
    uint64_t st = 0, at = 0;
    int ok = rx_world_timing_status(w, &st, &at) == RX_OK;
    r15_rec_begin(&g_out, "timing_status");
    rec_str("measure", measure);
    rec_u("stored", st);
    rec_u("attempted", at);
    rec_i("ok", ok);
    r15_rec_end(&g_out);
    return ok;
}

/* A: external publication (EXTERNAL crumb t_end, inside the lock) -> run_one
 * start of the one dependent. C (uncontended) from the same activations. */
static int l1_a(L1 *e, int with_c) {
    const uint64_t RES = 0x150;
    RxObjRef src = l1_obj(e, RES);
    RxCapRef ext = l1_mint(e, EXTERNAL, RES, RX_RIGHT_WRITE);
    RxCapRef rd = l1_mint(e, L1_SUBJ, RES, RX_RIGHT_READ);
    uint32_t rid = l1_reaction(e, "l1.a", fn_quiet, NULL, RX_PRIO_FOREGROUND, src, rd, RES);
    if (rid == UINT32_MAX) return -1;
    uint32_t total = L1_WARM + L1_N;
    RxTiming *tb = calloc(total + 16, sizeof *tb);
    uint64_t *lat = calloc(total, 8), *sch = calloc(total, 8), *scpu = calloc(total, 8),
             *rr = calloc(total, 8), *dr = calloc(total, 8);
    if (!tb || !lat || !sch || !scpu || !rr || !dr) return -1;
    rx_world_set_timing(&e->w, tb, total + 16);
    uint64_t base = acts_of(&e->w, rid);
    for (uint32_t i = 0; i < total; i++) {
        RxMutation m = {src, 0, i + 1};
        if (rx_world_publish_external(&e->w, ext, &m, 1) <= 0) return -1;
        if (wait_acts(&e->w, rid, base + i + 1) != 0) return -1;
    }
    if (rx_world_wait_quiescent(&e->w, 5000) != RX_OK) return -1;
    int ok = timing_ok(&e->w, with_c ? "C" : "A");
    uint64_t stored = 0, att = 0;
    rx_world_timing_status(&e->w, &stored, &att);
    uint64_t n = 0;
    for (uint64_t j = 0; j < stored; j++) {
        if (tb[j].reaction != rid) continue;
        if (n >= total) break;
        const RxCrumb *c = rx_world_crumb(&e->w, tb[j].cause);
        lat[n] = c && tb[j].t_run >= c->t_end_ns ? tb[j].t_run - c->t_end_ns : UINT64_MAX;
        sch[n] = tb[j].sched_ns;
        scpu[n] = tb[j].sched_cpu_ns;
        rr[n] = tb[j].t_run - tb[j].t_ready;
        dr[n] = tb[j].t_ready - tb[j].t_demand;
        n++;
    }
    rx_world_set_timing(&e->w, NULL, 0);
    if (n != total) ok = 0;
    if (!with_c) {
        rec_raw("A", "latency_ns", lat + L1_WARM, n > L1_WARM ? n - L1_WARM : 0, 0, 0);
    } else {
        uint64_t m = n > L1_WARM ? n - L1_WARM : 0;
        rec_raw("C_uncontended", "sched_wall_ns", sch + L1_WARM, m, 0, 0);
        rec_raw("C_uncontended", "sched_cpu_ns", scpu + L1_WARM, m, 0, 0);
        rec_raw("C_uncontended", "ready_to_run_ns", rr + L1_WARM, m, 0, 0);
        rec_raw("C_uncontended", "demand_to_ready_ns", dr + L1_WARM, m, 0, 0);
    }
    free(tb); free(lat); free(sch); free(scpu); free(rr); free(dr);
    return ok ? 0 : -1;
}

/* B: one committed field -> n dependents inserted (RES only). */
static int l1_b(L1 *e) {
    static const uint32_t fan[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    RxTiming *tb = calloc(512, sizeof *tb);
    uint64_t *wall = calloc(L1_N, 8), *cpu = calloc(L1_N, 8);
    if (!tb || !wall || !cpu) return -1;
    int bad = 0;
    for (uint32_t f = 0; f < sizeof fan / sizeof fan[0]; f++) {
        uint64_t RES = 0x160 + f;
        RxObjRef src = l1_obj(e, RES);
        RxCapRef ext = l1_mint(e, EXTERNAL, RES, RX_RIGHT_WRITE);
        RxCapRef rd = l1_mint(e, L1_SUBJ, RES, RX_RIGHT_READ);
        for (uint32_t i = 0; i < fan[f]; i++)
            if (l1_reaction(e, "l1.b", fn_quiet, NULL, RX_PRIO_FOREGROUND, src, rd, RES) == UINT32_MAX)
                return -1;
        uint64_t failed = 0, n = 0;
        for (uint32_t i = 0; i < L1_WARM + L1_N; i++) {
            rx_world_set_timing(&e->w, tb, 512);
            RxMutation m = {src, 0, i + 1};
            if (rx_world_publish_external(&e->w, ext, &m, 1) <= 0) return -1;
            if (rx_world_wait_quiescent(&e->w, 5000) != RX_OK) return -1;
            pthread_mutex_lock(&e->w.mu);
            RxPropWave p = e->w.last_prop;
            pthread_mutex_unlock(&e->w.mu);
            if (i < L1_WARM) continue;
            if (p.ready_inserts != fan[f]) {   /* C1 item 2 */
                if (!failed) fprintf(stderr, "B fanout %u: inspected %llu matched %llu attempts %llu accepted %llu inserts %llu coalesced %llu deferred %llu suppressed %llu\n", fan[f], (unsigned long long)p.inspected, (unsigned long long)p.matched, (unsigned long long)p.wake_attempts, (unsigned long long)p.wakes_accepted, (unsigned long long)p.ready_inserts, (unsigned long long)p.coalesced, (unsigned long long)p.deferred, (unsigned long long)p.suppressed);
                failed++;
                continue;
            }
            wall[n] = p.wall_ns;
            cpu[n] = p.cpu_ns;
            n++;
        }
        rx_world_set_timing(&e->w, NULL, 0);
        rec_raw("B", "wave_wall_ns", wall, n, 1, fan[f]);
        rec_raw("B", "wave_cpu_ns", cpu, n, 1, fan[f]);
        r15_rec_begin(&g_out, "l1_b_failed");
        rec_u("fanout", fan[f]);
        rec_u("failed_samples", failed);
        r15_rec_end(&g_out);
        if (failed) bad = 1;
    }
    free(tb); free(wall); free(cpu);
    return bad ? -1 : 0;
}

/* C contended: resource-blocked (slots = 1, a holder releases) and priority
 * contention (7 classes, starvation bound 4). */
static int l1_c_contended(L1 *e) {
    RxResourceBudget b = {0};
    b.memory_bytes = UINT64_MAX;
    b.energy_budget = UINT64_MAX;
    b.offered_locality = UINT32_MAX;
    b.compute_mask = UINT32_MAX;
    int ok = 1;
    const uint32_t N = 2000;           /* stimuli per case; every activation recorded */
    for (int kase = 0; kase < 2; kase++) {
        b.slots = kase == 0 ? 1 : 8;
        b.starvation_bound = kase == 0 ? 0 : 4;
        rx_world_set_resources(&e->w, &b);
        uint64_t RES = 0x170 + (uint64_t)kase;
        RxObjRef src = l1_obj(e, RES);
        RxCapRef ext = l1_mint(e, EXTERNAL, RES, RX_RIGHT_WRITE);
        RxCapRef rd = l1_mint(e, L1_SUBJ, RES, RX_RIGHT_READ);
        uint32_t nr = kase == 0 ? 2 : RX_PRIORITY_CLASSES;
        uint32_t ids[RX_PRIORITY_CLASSES];
        g_spin_ns = 20000;
        for (uint32_t i = 0; i < nr; i++)
            if ((ids[i] = l1_reaction(e, kase == 0 ? "l1.c.blocked" : "l1.c.prio", fn_spin, NULL,
                                      kase == 0 ? RX_PRIO_FOREGROUND : i, src, rd, RES)) == UINT32_MAX)
                return -1;
        uint64_t cap = (uint64_t)N * nr + 64;
        RxTiming *tb = calloc(cap, sizeof *tb);
        uint64_t *dr = calloc(cap, 8), *rr = calloc(cap, 8), *sc = calloc(cap, 8), *sw = calloc(cap, 8),
                 *cls = calloc(cap, 8);
        if (!tb || !dr || !rr || !sc || !sw || !cls) return -1;
        rx_world_set_timing(&e->w, tb, cap);
        for (uint32_t i = 0; i < N; i++) {
            RxMutation m = {src, 0, i + 1};
            if (rx_world_publish_external(&e->w, ext, &m, 1) <= 0) return -1;
            if (rx_world_wait_quiescent(&e->w, 5000) != RX_OK) return -1;
        }
        if (!timing_ok(&e->w, kase == 0 ? "C_blocked" : "C_priority")) ok = 0;
        uint64_t st = 0, at = 0;
        rx_world_timing_status(&e->w, &st, &at);
        uint64_t n = 0;
        for (uint64_t j = 0; j < st; j++) {
            uint32_t k;
            for (k = 0; k < nr && ids[k] != tb[j].reaction; k++) {}
            if (k == nr) continue;
            dr[n] = tb[j].t_ready - tb[j].t_demand;
            rr[n] = tb[j].t_run - tb[j].t_ready;
            sc[n] = tb[j].sched_cpu_ns;
            sw[n] = tb[j].sched_ns;
            cls[n] = k;
            n++;
        }
        rx_world_set_timing(&e->w, NULL, 0);
        const char *m = kase == 0 ? "C_blocked" : "C_priority";
        rec_raw(m, "demand_to_ready_ns", dr, n, 0, 0);
        rec_raw(m, "ready_to_run_ns", rr, n, 0, 0);
        rec_raw(m, "sched_cpu_ns", sc, n, 0, 0);
        rec_raw(m, "sched_wall_ns", sw, n, 0, 0);
        rec_raw(m, "class", cls, n, 0, 0);
        free(tb); free(dr); free(rr); free(sc); free(sw); free(cls);
    }
    g_spin_ns = 0;
    return ok ? 0 : -1;
}

/* D: reaction return -> publication committed -> dependents visible, per
 * commit; E(activation): the whole activation of a reaction holding existing
 * grants (run_one -> visible). */
static int l1_d(L1 *e) {
    const uint64_t RI = 0x180, RO = 0x181;
    RxObjRef in = l1_obj(e, RI), outo = l1_obj(e, RO);
    RxCapRef ext = l1_mint(e, EXTERNAL, RI, RX_RIGHT_WRITE);
    RxCapRef rdi = l1_mint(e, L1_SUBJ, RI, RX_RIGHT_READ);
    RxCapRef rwo = l1_mint(e, L1_SUBJ, RO, RX_RIGHT_READ | RX_RIGHT_WRITE);
    static Copy k;
    k.in = in; k.out = outo;
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "l1.d.copy";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = L1_SUBJ;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_copy;
    d.user = &k;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){in, RX_FIELD(0)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){outo, RX_FIELD(0)};
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){rdi, RI, RX_RIGHT_READ};
    d.caps[1] = (RxCapNeed){rwo, RO, RX_RIGHT_WRITE};
    uint32_t rid;
    if (rx_world_add_reaction(&e->w, &d, &rid) != RX_OK) return -1;
    RxCapRef rdo = l1_mint(e, L1_SUBJ, RO, RX_RIGHT_READ);
    if (l1_reaction(e, "l1.d.listener", fn_quiet, NULL, RX_PRIO_FOREGROUND, outo, rdo, RO) == UINT32_MAX)
        return -1;
    uint32_t total = L1_WARM + L1_N;
    uint64_t cap = (uint64_t)total * 2 + 16;
    RxTiming *tb = calloc(cap, sizeof *tb);
    uint64_t *pub = calloc(total, 8), *act = calloc(total, 8);
    if (!tb || !pub || !act) return -1;
    rx_world_set_timing(&e->w, tb, cap);
    for (uint32_t i = 0; i < total; i++) {
        RxMutation m = {in, 0, i + 1};
        if (rx_world_publish_external(&e->w, ext, &m, 1) <= 0) return -1;
        if (rx_world_wait_quiescent(&e->w, 5000) != RX_OK) return -1;
    }
    int ok = timing_ok(&e->w, "D");
    uint64_t st = 0, at = 0, n = 0;
    rx_world_timing_status(&e->w, &st, &at);
    for (uint64_t j = 0; j < st && n < total; j++) {
        if (tb[j].reaction != rid || tb[j].outcome != RX_CRUMB_COMMIT) continue;
        pub[n] = tb[j].t_visible - tb[j].t_fn_end;
        act[n] = tb[j].t_visible - tb[j].t_run;
        n++;
    }
    rx_world_set_timing(&e->w, NULL, 0);
    if (n != total) ok = 0;
    uint64_t m = n > L1_WARM ? n - L1_WARM : 0;
    rec_raw("D", "publish_to_visible_ns", pub + L1_WARM, m, 0, 0);
    rec_raw("E_activation", "run_to_visible_ns", act + L1_WARM, m, 0, 0);
    free(tb); free(pub); free(act);
    return ok ? 0 : -1;
}

/* E: rx_world_validate_cap on an existing valid grant, batches of 100. */
static int l1_e(L1 *e) {
    const uint64_t RES = 0x190;
    RxCapRef rd = l1_mint(e, L1_SUBJ, RES, RX_RIGHT_READ);
    uint64_t *v = calloc(L1_N, 8);
    if (!v) return -1;
    int ok = 1;
    for (uint32_t b = 0; b < L1_WARM + L1_N; b++) {
        uint64_t t0 = r15_now_ns();
        for (int i = 0; i < 100; i++) {
            RxCapEntry ent;
            if (rx_world_validate_cap(&e->w, rd, L1_SUBJ, RES, RX_RIGHT_READ, &ent) != RX_CAP_OK) ok = 0;
        }
        uint64_t dt = r15_now_ns() - t0;
        if (b >= L1_WARM) v[b - L1_WARM] = dt;
    }
    rec_raw("E", "batch100_ns", v, L1_N, 0, 0);
    free(v);
    return ok ? 0 : -1;
}

/* The same check the living promoter uses (rx_living.c native_promotion). */
static int native_promotion(void *ctx, uint32_t cap_id, uint64_t generation, uint32_t subject,
                            uint64_t resource, uint32_t rights) {
    AienosCapEntry entry;
    return aienos_cap_validate(ctx, (AienosCapRef){cap_id, generation}, subject, resource, rights,
                               &entry);
}

/* G: R9 barrier on the living body while production keeps publishing.
 * W0/W1: W-PROD throughput with the timing buffer off/on. Both use the rig. */
static int l1_rig(R15Config cfg, const char *measure) {
    static R15Rig rig;
    R15Rig *r = &rig;
    if (r15_start(r, cfg) != 0) return -1;
    if (r15_move_class(R15_CLASS_X925) != 0 || r15_placement(r, R15_CLASS_X925) != 0) {
        r15_stop(r);
        return -1;
    }
    int ok = 1;
    if (strcmp(measure, "G") == 0) {
        if (r15_producer_start(r) != 0) { r15_stop(r); return -1; }
        sleep_ns(WARM_NS);
        RxObject slot;
        AienosCapEntry grant;
        if (rx_world_read(&r->w, r->aegis.o[0].slot[0], &slot) != RX_OK ||
            aienos_cap_inspect(r->view, (AienosCapRef){(uint32_t)slot.field[0],
                                                       slot.field[1]}, &grant) != 0)
            ok = 0;
        static uint8_t blob[4096];
        for (int b = 0; b < 30 && ok; b++) {
            for (size_t i = 0; i < sizeof blob; i++) blob[i] = (uint8_t)(i * 31u + (unsigned)b);
            RxGenDraft d;
            memset(&d, 0, sizeof d);
            d.authority_epoch = grant.epoch;
            d.authority_generation = grant.generation;
            d.proofs_ok = 1;
            d.evidence = blob; d.evidence_len = 256;
            d.model = blob + 256; d.model_len = 128;
            d.realization = blob; d.realization_len = sizeof blob;
            d.config = blob + 512; d.config_len = 64;
            d.provenance = blob + 1024; d.provenance_len = 128;
            uint64_t id = 0, b0, s0, b1, s1, pb0, ps0, pb1, ps1;
            rx_gen_store_io(r->gen, &b0, &s0);
            rx_gen_io_counters(&pb0, &ps0);
            uint64_t prep = r15_now_ns();
            int prc = rx_gen_propose_as(r->gen, RX_LIVING_PREPARE_SUBJ,
                rx_caller_find(&r->keys_living, RX_LIVING_PREPARE_SUBJ), &d, &id);
            RxPromotionRequest req = {id, RX_LIVING_PROMOTE_SUBJ,
                r->promoter.promotion_authority.cap_id, r->promoter.promotion_authority.generation,
                RX_GEN_RES_PROMOTION, RX_GEN_RIGHT_PROMOTE, {0, {0}}};
            req.caller = *rx_caller_find(&r->keys_promoter, RX_LIVING_PROMOTE_SUBJ);
            uint64_t n0 = r->w.n_crumbs;
            int rc = prc == RX_GEN_OK
                ? rx_gen_promote(r->gen, &req, native_promotion, (void *)r->view, NULL, NULL, NULL, NULL) : prc;
            RxGenPhases ph;
            memset(&ph, 0, sizeof ph);
            rx_gen_last_phases(r->gen, &ph);
            sleep_ns(20000000ull);   /* live work continues */
            rx_gen_store_io(r->gen, &b1, &s1);
            rx_gen_io_counters(&pb1, &ps1);
            uint64_t during = 0, during_flip = 0, after = 0;
            uint64_t n1 = r->w.n_crumbs;
            for (uint64_t cid = n0 > 4096 ? n0 - 4096 : 1; cid <= n1; cid++) {
                const RxCrumb *k = rx_world_crumb(&r->w, cid);
                if (!k || k->reaction != r->omega.r_serve || k->kind != RX_CRUMB_COMMIT) continue;
                if (k->t_end_ns >= ph.barrier_ns && k->t_end_ns <= ph.receipt_ns) during++;
                if (k->t_end_ns >= ph.barrier_ns && k->t_end_ns <= ph.flip_ns) during_flip++;
                if (k->t_end_ns > ph.receipt_ns) after++;
            }
            r15_rec_begin(&g_out, "l1_g");
            rec_i("barrier", b);
            rec_i("propose_rc", prc);
            rec_i("promote_rc", rc);
            rec_u("prepared", prep);
            rec_u("enter", ph.enter_ns);
            rec_u("begin", ph.barrier_ns);
            rec_u("flip", ph.flip_ns);
            rec_u("receipt", ph.receipt_ns);
            rec_u("production_commits_barrier", during);
            rec_u("production_commits_to_flip", during_flip);
            rec_u("production_commits_after", after);
            rec_u("store_bytes", b1 - b0);
            rec_u("store_syncs", s1 - s0);
            rec_u("process_bytes", pb1 - pb0);
            rec_u("process_syncs", ps1 - ps0);
            r15_rec_end(&g_out);
            if (rc != RX_GEN_OK) ok = 0;
            sleep_ns(100000000ull);
        }
        if (r15_producer_stop(r) != 0) ok = 0;
    } else {
        int timing = strcmp(measure, "W1") == 0;
        uint64_t cap = 8ull << 20;
        RxTiming *tb = timing ? calloc(cap, sizeof *tb) : NULL;
        if (timing && !tb) ok = 0;
        if (tb) rx_world_set_timing(&r->w, tb, cap);
        if (r15_producer_start(r) != 0) ok = 0;
        sleep_ns(WARM_NS);
        Win x;
        win_begin(&x, &r->w, r, 0);
        sleep_ns(WINDOW_NS);
        win_end(&x, &r->w, r);
        if (r15_producer_stop(r) != 0) ok = 0;
        rec_window(timing ? "W_TIMING_ON" : "W_TIMING_OFF", &x);
        if (tb) {
            timing_ok(&r->w, "W1");
            rx_world_set_timing(&r->w, NULL, 0);
        }
        r15_rec_begin(&g_out, "w_correct");
        rec_u("wrong", r->wrong);
        r15_rec_end(&g_out);
        if (r->wrong) ok = 0;
        rx_world_wait_quiescent(&r->w, 10000);
        free(tb);
    }
    r15_stop(r);
    return ok ? 0 : -1;
}

static int run_l1(const char *measure, R15Config cfg) {
    rec_machine("before");
    if (strcmp(measure, "G") == 0 || measure[0] == 'W') {
        int rc = l1_rig(cfg, measure);
        rec_machine("after");
        return rc == 0 ? 0 : 1;
    }
    L1 e;
    if (l1_start(&e, cfg == R15_SEQ) != 0) return 2;
    int rc;
    switch (measure[0]) {
    case 'A': rc = l1_a(&e, 0); break;
    case 'B': rc = cfg == R15_SEQ ? -1 : l1_b(&e); break;
    case 'C': rc = l1_a(&e, 1); if (rc == 0) rc = l1_c_contended(&e); break;
    case 'D': rc = l1_d(&e); break;
    case 'E': rc = l1_e(&e); break;
    default: rc = -1;
    }
    if (e.seq && atomic_load(&e.err)) rc = -1;
    l1_stop(&e);
    rec_machine("after");
    return rc == 0 ? 0 : 1;
}

/* ---- Level 2: CPU <-> GPU through the resident seat ----------------------- */

static int run_l2(R15Config cfg) {
    rec_machine("before");
    uint64_t t_proc = r15_now_ns();
    observers_start();
    static R15Rig rig;
    R15Rig *r = &rig;
    if (r15_start(r, cfg) != 0) { observers_stop(t_proc); return 2; }
    g_samp.w = &r->w;
    g_samp.armed = 1;
    if (r15_move_class(R15_CLASS_X925) != 0) { r15_stop(r); observers_stop(t_proc); return 2; }
    RxCapRef ext = r15_mint(r, EXTERNAL, RX_LIVING_RES_BASE + RX_LIVING_RES_INPUT, RX_RIGHT_WRITE);
    RxWorld *w = &r->w;
    int ok = 1;
    const uint32_t CLAIMS = 256;
    Win x;
    win_begin(&x, w, r, 0);
    for (uint32_t i = 0; i < CLAIMS && ok; i++) {
        uint64_t a = 1000u + i * 7u, b = 3u + i * 3u, want = (uint32_t)(a + b);
        RxMutation m[2] = {{r->living.o.input, 0, a}, {r->living.o.input, 1, b}};
        int64_t ec = rx_world_publish_external(w, ext, m, 2);
        if (ec <= 0) { ok = 0; break; }
        uint64_t t = r15_now_ns();
        RxObject o;
        while (!(rx_world_read(w, r->living.o.output, &o) == RX_OK && o.field[0] == want)) {
            if (r15_now_ns() - t > 5000000000ull) { ok = 0; break; }
        }
        if (!ok || rx_world_wait_quiescent(w, 5000) != RX_OK) { ok = 0; break; }
        uint32_t pick = 0, done = 0, claim = 0;
#ifdef R15_SILICON
        const uint8_t *hb = w->coherent + rx_world_off_heartbeat();
        pick = __atomic_load_n((const uint32_t *)(hb + RX_SEAT_HB_T_PICK), __ATOMIC_ACQUIRE);
        done = __atomic_load_n((const uint32_t *)(hb + RX_SEAT_HB_T_DONE), __ATOMIC_ACQUIRE);
        claim = __atomic_load_n((const uint32_t *)(hb + RX_SEAT_HB_CLAIM), __ATOMIC_ACQUIRE);
#endif
        const RxCrumb *ek = rx_world_crumb(w, (uint64_t)ec), *sk = NULL, *dk = NULL;
        for (uint64_t id = (uint64_t)ec + 1; id <= w->n_crumbs; id++) {
            const RxCrumb *k = rx_world_crumb(w, id);
            if (!k) continue;
            if (!sk && k->reaction == r->living.r_seat && k->kind == RX_CRUMB_COMMIT) sk = k;
            else if (sk && !dk && k->reaction == r->living.r_evidence) dk = k;
        }
        r15_rec_begin(&g_out, "l2_claim");
        rec_u("i", i);
        rec_u("input_t", ek ? ek->t_end_ns : 0);
        rec_u("seat_start_t", sk ? sk->t_start_ns : 0);
        rec_u("seat_commit_t", sk ? sk->t_end_ns : 0);
        rec_u("dependent_start_t", dk ? dk->t_start_ns : 0);
        rec_u("chip_pick", pick);
        rec_u("chip_done", done);
        rec_u("chip_claim", claim);
        r15_rec_end(&g_out);
        if (!sk || !dk) ok = 0;
    }
    win_end(&x, w, r);
    rec_window("L2", &x);
    g_samp.armed = 0;
    rec_residency();
    r15_stop(r);
    observers_stop(t_proc);
    rec_machine("after");
    return ok ? 0 : 1;
}

/* ---- main ------------------------------------------------------------------ */

static int parse_cfg(const char *s, R15Config *c) {
    if (strcmp(s, "RES4") == 0) *c = R15_RES4;
    else if (strcmp(s, "RES1") == 0) *c = R15_RES1;
    else if (strcmp(s, "SEQ") == 0) *c = R15_SEQ;
    else return -1;
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: rx_r15_perf trial <RES4|RES1|SEQ> <run> <round> <out.jsonl>\n"
                    "       rx_r15_perf l1 <A|B|C|D|E|G|W0|W1> <RES1|SEQ> <run> <round> <out.jsonl>\n"
                    "       rx_r15_perf l2 <RES1|SEQ> <run> <round> <out.jsonl>\n");
}

int main(int argc, char **argv) {
    if (argc < 6) { usage(); return 64; }
    R15Config cfg;
    const char *mode = argv[1], *measure = "", *run, *outp;
    int round;
    if (strcmp(mode, "l1") == 0) {
        if (argc != 7 || parse_cfg(argv[3], &cfg) != 0) { usage(); return 64; }
        measure = argv[2]; run = argv[4]; round = atoi(argv[5]); outp = argv[6];
    } else {
        if (argc != 6 || parse_cfg(argv[2], &cfg) != 0) { usage(); return 64; }
        run = argv[3]; round = atoi(argv[4]); outp = argv[5];
    }
    snprintf(g_config, sizeof g_config, "%s%s", r15_config_name(cfg),
             !rx_world_causal_digest_enabled ? "-NODIGEST" : "");
    if (!rx_world_causal_digest_enabled && cfg != R15_RES1) {
        fprintf(stderr, "the NODIGEST build measures RES-1 only (spec §3)\n");
        return 64;
    }
    char level[16];
    snprintf(level, sizeof level, "%s%s%s", mode, measure[0] ? "-" : "", measure);
    if (r15_out_open(&g_out, outp, run, level, g_config, round, 0) != 0) {
        fprintf(stderr, "cannot create %s (raw evidence is never overwritten)\n", outp);
        return 65;
    }
    r15_rec_begin(&g_out, "start");
    rec_str("mode", mode);
    rec_str("measure", measure);
    rec_i("pid", getpid());
#ifdef R15_SILICON
    rec_i("silicon", 1);
#else
    rec_i("silicon", 0);
#endif
    r15_rec_end(&g_out);
    int rc;
    if (strcmp(mode, "trial") == 0) rc = run_trial(cfg);
    else if (strcmp(mode, "l1") == 0) rc = run_l1(measure, cfg);
    else if (strcmp(mode, "l2") == 0) rc = run_l2(cfg);
    else { usage(); rc = 64; }
    r15_rec_begin(&g_out, "end");
    rec_i("exit", rc);
    r15_rec_end(&g_out);
    r15_out_close(&g_out);
    fprintf(stderr, "%s %s %s round %d: %s\n", level, g_config, run, round, rc == 0 ? "ok" : "FAILED");
    return rc;
}
