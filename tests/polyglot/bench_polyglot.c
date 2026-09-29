/* POLYGLOT-0 benchmark harness (lane F). spec/polyglot-0.md sections 6-8.
 *
 * Workloads S1-S4 x sparsity {0, 0.33, 0.66}; every registered candidate
 * (omx_candidate_get) that accepts the shape is packed once per cell,
 * checked against oma_rz_oracle for every x of the cell, then timed:
 *   - one pinned core (sched_setaffinity), cluster from MIDR (X925/A725);
 *   - candidates interleaved round-robin (A B C A B C), N samples per cell
 *     (N >= 20 unless --smoke);
 *   - a sample loops enough calls to last >= 1 ms (calibrated per candidate
 *     and cell) and sums every y into a checksum that is compared with the
 *     checksum predicted by the oracle, so no call can be hoisted or skipped;
 *   - a sample whose /proc/thread-self/schedstat run delay grew is re-taken
 *     (up to 20 times; then kept and counted as contaminated);
 *   - min, median and MAD of ns per call;
 *   - pack cost (min/median of 5 packs) per weight and break-even calls
 *     against the cheapest-to-pack exact candidate of the cell;
 *   - after timing: compile time (min of 5 wall runs of the manifest command),
 *     object SHA-256 and text bytes, run+pack symbol bytes, peak RSS.
 * One JSON receipt per candidate x workload (the three sparsities are cells
 * inside it) under --out. Refuses to run if ~/workspace/.spark-quiet exists.
 *
 * Usage: bench_polyglot --spec spec/polyglot-0.md --manifest M --out DIR
 *        [--samples N] [--sparsity 0,0.33,0.66] [--workloads S1,S2,S3,S4]
 *        [--cpu C] [--smoke]
 * Environment (set by mk/polyglot.mk): POLYGLOT_COMMIT, POLYGLOT_DIRTY,
 * POLYGLOT_BENCH_SHA, POLYGLOT_CFLAGS_O2, POLYGLOT_CFLAGS_O3, POLYGLOT_CFLAGS_LANE,
 * POLYGLOT_BENCH_CPU.
 */
#define _GNU_SOURCE
#include "polyglot/omx_bench.h"
#include "polyglot/omx_lang.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define SCHEMA "OMEGA_POLYGLOT0_RECEIPT_V1"
#define SEED 0x504f4c5942454e43ULL /* "POLYBENC" */
#define MAXC 64
#define NW 4
#define NSP 3
#define MIN_SAMPLE_NS 1.0e6
#define MAX_RETAKE 20
#define PACK_REPS 5
#define COMPILE_REPS 5

typedef struct {
    const char *id;
    size_t m, n, nx;
    const char *regime;
} workload;

static const workload k_w[NW] = {
    {"S1", 1, 4096, 1, "call overhead"},
    {"S2", 256, 1024, 1, "cache-resident (256 KiB int8, L2)"},
    {"S3", 4096, 4096, 1, "memory bandwidth"},
    {"S4", 64, 1024, 256, "compute-bound, 256 different x per packed W, cache-resident"},
};
static const double k_sp[NSP] = {0.0, 0.33, 0.66};

typedef struct {
    char src[256], flavor[8], obj[256], cmd[1536];
    int used, ctime_done;
    double ctime_min_s;
    char digest[65];
    long text_bytes;
} man_ent;

typedef struct {
    int present;       /* cell was run (workload and sparsity selected) */
    int measured;      /* timed */
    char skip[160];    /* why not timed */
    char input_sha[65];
    unsigned long long checks, failures;
    double pack_min, pack_med;
    long reps;
    double *s;         /* ns per call, one per sample */
    int ns;
    unsigned retakes, contaminated;
    double lat_min, lat_med, lat_mad;
    size_t weight_bytes, footprint, scratch_bytes;
    uint64_t nnz;
    long cur_khz;
    long long break_even; /* -1: never; -2: n/a */
    const char *cheapest_pack;
    oma_rz_plan plan;
} cell;

static size_t g_nc;
static cell g_cell[MAXC][NW][NSP];
static man_ent g_man[256];
static size_t g_nman;
static int g_cpu = -1, g_part = -1;
static volatile int64_t g_sink;

static void die(const char *msg) {
    fprintf(stderr, "bench_polyglot: %s\n", msg);
    exit(2);
}

/* ---- manifest ---- */
static void load_manifest(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open manifest");
    char line[2400];
    while (fgets(line, sizeof line, f) && g_nman < 256) {
        line[strcspn(line, "\n")] = 0;
        char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t"), *d = strtok(NULL, "");
        if (!a || !b || !c) continue;
        man_ent *e = &g_man[g_nman++];
        memset(e, 0, sizeof *e);
        snprintf(e->src, sizeof e->src, "%s", a);
        snprintf(e->flavor, sizeof e->flavor, "%s", b);
        snprintf(e->obj, sizeof e->obj, "%s", c);
        snprintf(e->cmd, sizeof e->cmd, "%s", d ? d : "-");
        e->text_bytes = -1;
    }
    fclose(f);
}

static const char *cand_flavor(const omx_candidate *c) {
    size_t l = strlen(c->impl->id);
    if (l > 3 && strcmp(c->impl->id + l - 3, "@O3") == 0) return "O3";
    return strcmp(c->language, "c") == 0 ? "O2" : "lane";
}

static man_ent *man_for(const omx_candidate *c) {
    const char *fl = cand_flavor(c);
    for (size_t i = 0; i < g_nman; i++)
        if (strcmp(g_man[i].src, c->source) == 0 && strcmp(g_man[i].flavor, fl) == 0) return &g_man[i];
    return NULL;
}

static const char *cand_flags(const omx_candidate *c) {
    const char *fl = cand_flavor(c), *v = NULL;
    if (strcmp(fl, "O2") == 0) v = getenv("POLYGLOT_CFLAGS_O2");
    else if (strcmp(fl, "O3") == 0) v = getenv("POLYGLOT_CFLAGS_O3");
    else {
        man_ent *e = man_for(c);
        return e ? e->cmd : NULL;
    }
    return v;
}

/* ---- inputs ---- */
static void make_inputs(const workload *w, double sp, int8_t *W, int8_t *X, char hex[65]) {
    omx_rng r = {SEED ^ ((uint64_t)(w->id[1] - '0') << 32) ^ (uint64_t)llround(sp * 100.0)};
    for (size_t i = 0; i < w->m * w->n; i++)
        W[i] = omx_rng_01(&r) < sp ? 0 : ((omx_rng_next(&r) & 1) ? 1 : -1);
    for (size_t i = 0; i < w->n * w->nx; i++) {
        uint64_t v = omx_rng_next(&r);
        X[i] = (v % 16 == 0) ? -128 : (v % 16 == 1) ? 127 : (int8_t)(uint8_t)(v >> 8);
    }
    sha256_ctx c;
    uint8_t d[32];
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)W, w->m * w->n);
    sha256_update(&c, (const uint8_t *)X, w->n * w->nx);
    sha256_final(&c, d);
    omx_hex(d, 32, hex);
}

/* One timed sample: reps calls cycling through the nx inputs, every y summed. */
static double sample(const oma_rz_impl *im, const oma_rz_plan *p, const int8_t *X, size_t n, size_t nx,
                     int32_t *y, size_t m, long reps, int64_t *sum, int *rc_out) {
    int64_t s = 0;
    int rc = 0;
    size_t xi = 0;
    double t0 = omx_now_ns();
    for (long k = 0; k < reps; k++) {
        rc |= im->run(p, X + xi * n, y);
        for (size_t i = 0; i < m; i++) s += y[i];
        if (++xi == nx) xi = 0;
    }
    double t1 = omx_now_ns();
    *sum = s;
    *rc_out = rc;
    g_sink += s;
    return t1 - t0;
}

static int64_t expected_sum(const int64_t *ysum_prefix, size_t nx, long reps) {
    long full = reps / (long)nx, rem = reps % (long)nx;
    return (int64_t)full * ysum_prefix[nx] + ysum_prefix[rem];
}

static int parse_list_d(const char *s, double *out, int cap) {
    int n = 0;
    while (s && *s && n < cap) {
        char *e;
        out[n++] = strtod(s, &e);
        if (e == s) return -1;
        s = *e == ',' ? e + 1 : e;
    }
    return n;
}

/* ---- compile time ---- */
static double time_cmd(const char *cmd) {
    pid_t pid;
    char *argv[] = {"/bin/sh", "-c", (char *)cmd, NULL};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    double t0 = omx_now_ns();
    int rc = posix_spawn(&pid, "/bin/sh", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) return -1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    double t1 = omx_now_ns();
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? (t1 - t0) / 1e9 : -1;
}

static void measure_build(man_ent *e, const char *tmpdir) {
    if (e->ctime_done) return;
    e->ctime_done = 1;
    if (omx_sha256_file(e->obj, e->digest)) snprintf(e->digest, sizeof e->digest, "unavailable");
    e->text_bytes = omx_elf_text_bytes(e->obj);
    e->ctime_min_s = -1;
    if (strcmp(e->cmd, "-") == 0) return;
    char cmd[2048], out[512];
    snprintf(out, sizeof out, "%s/ctime.o", tmpdir);
    /* replace every @OUT@ */
    size_t o = 0;
    for (const char *p = e->cmd; *p && o + 1 < sizeof cmd;) {
        if (strncmp(p, "@OUT@", 5) == 0) {
            o += (size_t)snprintf(cmd + o, sizeof cmd - o, "%s", out);
            if (o >= sizeof cmd) return;
            p += 5;
        } else cmd[o++] = *p++;
    }
    cmd[o] = 0;
    for (int r = 0; r < COMPILE_REPS; r++) {
        double t = time_cmd(cmd);
        if (t < 0) { e->ctime_min_s = -1; return; }
        if (e->ctime_min_s < 0 || t < e->ctime_min_s) e->ctime_min_s = t;
    }
}

/* ---- informational ---- */
static void source_info(const char *path, long *lines, long *unsafe, const char **rule) {
    *lines = -1;
    *unsafe = -1;
    *rule = "none";
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t l = strlen(path);
    int is_asm = l > 2 && (strcmp(path + l - 2, ".S") == 0 || strcmp(path + l - 2, ".s") == 0);
    int is_mojo = l > 5 && strcmp(path + l - 5, ".mojo") == 0;
    *rule = is_asm ? "instruction lines (not blank, not comment, not directive, not label-only)"
            : is_mojo ? "occurrences of 'UnsafePointer' plus 'bitcast'"
                      : "occurrences of '*)' (pointer casts)";
    char line[1024];
    long n = 0, u = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        if (is_asm) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            char *colon = strchr(p, ':');
            if (colon && (colon[1] == '\n' || colon[1] == 0 || colon[1] == ' ')) {
                p = colon + 1;
                while (*p == ' ' || *p == '\t') p++;
            }
            if (*p && *p != '\n' && *p != '.' && *p != '/' && *p != '#' && *p != '*' && *p != '@') u++;
        } else if (is_mojo) {
            for (char *p = line; (p = strstr(p, "UnsafePointer")); p++) u++;
            for (char *p = line; (p = strstr(p, "bitcast")); p++) u++;
        } else {
            for (char *p = line; (p = strstr(p, "*)")); p++) u++;
        }
    }
    fclose(f);
    *lines = n;
    *unsafe = u;
}

static void json_num(FILE *f, double v, int prec) {
    if (!isfinite(v)) fputs("null", f);
    else fprintf(f, "%.*f", prec, v);
}

static void sanitize(const char *id, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = id; *p && o + 4 < cap; p++) {
        if (*p == '@') { memcpy(out + o, "_at_", 4); o += 4; }
        else out[o++] = (char)((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                                       *p == '_' || *p == '-' || *p == '.'
                                   ? *p
                                   : '_');
    }
    out[o] = 0;
}

typedef struct {
    char kernel[128], governor[64], load0[64], load1[64], model[160], run_id[96], started[32];
    long khz0, khz1;
    long th0[OMX_MAX_THERMAL], th1[OMX_MAX_THERMAL];
    int nth0, nth1, capacity;
    long peak_rss_kib;
    double idle_frac;
    char cpu_why[96];
} run_info;

static void print_thermal(FILE *f, const long *t, int n) {
    fputc('[', f);
    for (int i = 0; i < n; i++) fprintf(f, "%s%.1f", i ? ", " : "", (double)t[i] / 1000.0);
    fputc(']', f);
}

static int write_receipt(const char *dir, const omx_candidate *c, int wi, const run_info *ri, const char *contract,
                         const char *spec, int samples, int smoke) {
    const oma_rz_impl *im = c->impl;
    const workload *w = &k_w[wi];
    char name[128], path[1024];
    sanitize(im->id, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s__%s.json", dir, name, w->id);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    man_ent *e = man_for(c);
    const char *commit = getenv("POLYGLOT_COMMIT"), *dirty = getenv("POLYGLOT_DIRTY");
    const char *bsha = getenv("POLYGLOT_BENCH_SHA");
    long lines, unsafe;
    const char *urule;
    source_info(c->source, &lines, &unsafe, &urule);
    char sym_run[128], sym_pack[128];
    size_t zr = omx_self_symbol_size((const void *)(uintptr_t)im->run, sym_run, sizeof sym_run);
    size_t zp = omx_self_symbol_size((const void *)(uintptr_t)im->pack, sym_pack, sizeof sym_pack);

    fprintf(f, "{\n  \"schema\": \"%s\",\n  \"semantic_operation\": \"omega-x\",\n", SCHEMA);
    fprintf(f, "  \"operation\": \"y = W.x, W in {-1,0,+1}^(m x n) row-major int8, x int8^n, y int32^m, exact\",\n");
    fprintf(f, "  \"contract\": {\"path\": ");
    omx_json_str(f, spec);
    fprintf(f, ", \"section\": 1, \"sha256\": \"%s\", \"digest_rule\": ", contract);
    omx_json_str(f, OMX_CONTRACT_DIGEST_RULE);
    fprintf(f, "},\n  \"run\": {\"run_id\": ");
    omx_json_str(f, ri->run_id);
    fprintf(f, ", \"started_utc\": \"%s\", \"source_commit\": ", ri->started);
    omx_json_str(f, commit && *commit ? commit : NULL);
    fprintf(f, ", \"tree_dirty_files\": %s, \"bench_binary_sha256\": ", dirty && *dirty ? dirty : "null");
    omx_json_str(f, bsha && *bsha ? bsha : NULL);
    fprintf(f, ", \"smoke\": %s, \"samples_per_cell\": %d, \"gate_eligible\": %s, \"seed\": \"0x%016llx\", "
               "\"min_sample_ns\": %.0f, \"peak_rss_kib\": %ld},\n",
            smoke ? "true" : "false", samples, (!smoke && samples >= 20) ? "true" : "false",
            (unsigned long long)SEED, MIN_SAMPLE_NS, ri->peak_rss_kib);
    fprintf(f, "  \"candidate\": {\"id\": ");
    omx_json_str(f, im->id);
    fprintf(f, ", \"label\": ");
    omx_json_str(f, im->label);
    fprintf(f, ", \"language\": ");
    omx_json_str(f, c->language);
    fprintf(f, ", \"toolchain\": ");
    omx_json_str(f, c->toolchain);
    fprintf(f, ", \"flags\": ");
    omx_json_str(f, cand_flags(c));
    fprintf(f, ", \"representation\": ");
    omx_json_str(f, im->family);
    fprintf(f, ", \"backend\": \"grace-cpu, one pinned core\", \"exact\": %d, \"weak_baseline\": %d, "
               "\"compiler_derived\": %d, \"toolchain_only\": %d, \"max_n\": %zu, \"source\": ",
            im->exact, im->weak_baseline, c->compiler_derived, c->toolchain_only, im->max_n);
    omx_json_str(f, c->source);
    fprintf(f, "},\n  \"workload\": {\"id\": \"%s\", \"m\": %zu, \"n\": %zu, \"x_per_packed_w\": %zu, \"regime\": ",
            w->id, w->m, w->n, w->nx);
    omx_json_str(f, w->regime);
    fprintf(f, "},\n  \"machine\": {\"cpu_model\": ");
    omx_json_str(f, ri->model);
    fprintf(f, ", \"core\": %d, \"core_type\": \"%s\", \"cluster\": \"%s\", \"midr_part\": \"0x%03x\", "
               "\"cpu_capacity\": %d, \"cpu_choice\": ",
            g_cpu, omx_core_name(g_part), omx_cluster_name(g_part), g_part < 0 ? 0 : g_part, ri->capacity);
    omx_json_str(f, ri->cpu_why);
    fprintf(f, ", \"kernel\": ");
    omx_json_str(f, ri->kernel);
    fprintf(f, ", \"governor\": ");
    omx_json_str(f, ri->governor);
    fprintf(f, ", \"cur_khz_before\": %ld, \"cur_khz_after\": %ld, \"thermal_c_before\": ", ri->khz0, ri->khz1);
    print_thermal(f, ri->th0, ri->nth0);
    fprintf(f, ", \"thermal_c_after\": ");
    print_thermal(f, ri->th1, ri->nth1);
    fprintf(f, ", \"loadavg_before\": \"%s\", \"loadavg_after\": \"%s\"},\n", ri->load0, ri->load1);
    fprintf(f, "  \"build\": {\"object\": ");
    omx_json_str(f, e ? e->obj : NULL);
    fprintf(f, ", \"object_sha256\": ");
    omx_json_str(f, e ? e->digest : NULL);
    fprintf(f, ", \"object_text_bytes\": %ld, \"run_symbol\": ", e ? e->text_bytes : -1L);
    omx_json_str(f, zr ? sym_run : NULL);
    fprintf(f, ", \"run_symbol_bytes\": %zu, \"pack_symbol\": ", zr);
    omx_json_str(f, zp ? sym_pack : NULL);
    fprintf(f, ", \"pack_symbol_bytes\": %zu, \"code_size_bytes\": %zu, \"code_size_rule\": "
               "\"run + pack function symbol sizes in the bench executable (callees not inlined are "
               "not counted; object_text_bytes is the whole object)\", \"compile_time_s_min_of_5\": ",
            zp, zr + zp);
    if (e && e->ctime_min_s >= 0) json_num(f, e->ctime_min_s, 4);
    else fputs("null", f);
    fprintf(f, ", \"compile_command\": ");
    omx_json_str(f, e ? e->cmd : NULL);
    fprintf(f, ", \"compile_note\": ");
    omx_json_str(f, !e ? "no manifest entry for this candidate's source" :
                    e->ctime_min_s < 0 ? "not measured (no command, or command failed)" :
                    "wall time, 5 runs, min; the object may hold several realizations");
    fprintf(f, "},\n  \"cells\": [\n");
    int first = 1;
    size_t ci = 0;
    for (; ci < g_nc; ci++)
        if (omx_candidate_get(ci) == c) break;
    for (int si = 0; si < NSP; si++) {
        cell *ce = &g_cell[ci][wi][si];
        if (!ce->present) continue;
        fprintf(f, "%s    {\"sparsity\": %.2f, \"input_sha256\": \"%s\", \"measured\": %s, \"skip_reason\": ",
                first ? "" : ",\n", k_sp[si], ce->input_sha, ce->measured ? "true" : "false");
        first = 0;
        omx_json_str(f, ce->measured ? NULL : ce->skip);
        fprintf(f, ",\n     \"correctness\": {\"checks\": %llu, \"failures\": %llu, \"method\": \"y vs oma_rz_oracle "
                   "for every x of the cell before timing, and every timed sample's y checksum vs the oracle "
                   "checksum\"},\n",
                ce->checks, ce->failures);
        if (!ce->measured) {
            fprintf(f, "     \"latency_ns\": null}");
            continue;
        }
        double m = (double)w->m, n = (double)w->n;
        size_t bytes = ce->weight_bytes + w->n + 4 * w->m + ce->scratch_bytes;
        double cps = 1e9 / ce->lat_med;
        fprintf(f, "     \"calls_per_sample\": %ld, \"samples\": %d, \"retaken_samples\": %u, "
                   "\"contaminated_samples\": %u, \"cur_khz_at_cell_start\": %ld,\n",
                ce->reps, ce->ns, ce->retakes, ce->contaminated, ce->cur_khz);
        fprintf(f, "     \"latency_ns\": {\"min\": %.3f, \"median\": %.3f, \"mad\": %.3f},\n", ce->lat_min,
                ce->lat_med, ce->lat_mad);
        fprintf(f, "     \"samples_ns_per_call\": [");
        for (int k = 0; k < ce->ns; k++) fprintf(f, "%s%.3f", k ? ", " : "", ce->s[k]);
        fprintf(f, "],\n     \"throughput\": {\"calls_per_s\": %.1f, \"weights_per_s\": %.4e, \"bytes_per_s\": %.4e},\n",
                cps, cps * m * n, cps * (double)bytes);
        fprintf(f, "     \"bytes_moved_per_call\": {\"weight_bytes\": %zu, \"x_bytes\": %zu, \"y_bytes\": %zu, "
                   "\"scratch_bytes\": %zu, \"total\": %zu},\n",
                ce->weight_bytes, w->n, 4 * w->m, ce->scratch_bytes, bytes);
        fprintf(f, "     \"footprint_bytes\": %zu, \"nnz\": %llu,\n", ce->footprint, (unsigned long long)ce->nnz);
        fprintf(f, "     \"pack\": {\"ns_min\": %.1f, \"ns_median\": %.1f, \"ns_per_weight\": %.4f, "
                   "\"cheapest_to_pack\": ",
                ce->pack_min, ce->pack_med, ce->pack_med / (m * n));
        omx_json_str(f, ce->cheapest_pack);
        fprintf(f, ", \"break_even_calls\": ");
        if (ce->break_even >= 0) fprintf(f, "%lld", ce->break_even);
        else fputs("null", f);
        fprintf(f, ", \"break_even_note\": ");
        omx_json_str(f, ce->break_even == -1 ? "never: not faster per call than the cheapest-to-pack candidate"
                        : ce->break_even == -2 ? "n/a" : "smallest k with pack + k*median <= cheapest pack + k*its median");
        fprintf(f, "},\n     \"rx_costmodel\": {\"op\": \"RX_CM_OP_MATVEC\", \"M\": %zu, \"N\": %zu, "
                   "\"state_bytes\": %zu, \"core\": \"%s\", \"thermal_c\": %.0f, \"ps_per_call_median\": %.0f, "
                   "\"ps_per_call_min\": %.0f, \"failed\": %d}}",
                w->m, w->n, (w->m * w->n + w->n + w->m) * 8,
                g_part == 0xd85 ? "RX_CM_CORE_X925" : g_part == 0xd87 ? "RX_CM_CORE_A725" : "RX_CM_CORE_OTHER",
                ri->nth0 ? (double)ri->th0[0] / 1000.0 : 0.0, ce->lat_med * 1000.0, ce->lat_min * 1000.0,
                ce->failures ? 1 : 0);
    }
    fprintf(f, "\n  ],\n  \"informational\": {\"source_lines\": %ld, \"unsafe_surface\": %ld, \"unsafe_surface_rule\": ",
            lines, unsafe);
    omx_json_str(f, urule);
    fprintf(f, ", \"source_note\": \"whole source file (may hold several realizations)\", "
               "\"defects_found_while_authoring\": null}\n}\n");
    int bad = ferror(f);
    if (fclose(f) || bad) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *spec = NULL, *manifest = NULL, *out = NULL;
    int samples = 21, smoke = 0, cpu_arg = -1;
    double sps[NSP] = {0.0, 0.33, 0.66};
    int nsp = NSP;
    int wsel[NW] = {1, 1, 1, 1};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spec") && i + 1 < argc) spec = argv[++i];
        else if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--smoke")) smoke = 1;
        else if (!strcmp(argv[i], "--sparsity") && i + 1 < argc) {
            nsp = parse_list_d(argv[++i], sps, NSP);
            if (nsp <= 0) die("bad --sparsity");
        } else if (!strcmp(argv[i], "--workloads") && i + 1 < argc) {
            const char *s = argv[++i];
            for (int k = 0; k < NW; k++) wsel[k] = strstr(s, k_w[k].id) != NULL;
        } else {
            fprintf(stderr, "usage: %s --spec F --manifest F --out DIR [--samples N] [--sparsity a,b] "
                            "[--workloads S1,S2] [--cpu C] [--smoke]\n", argv[0]);
            return 2;
        }
    }
    if (!spec || !manifest || !out) die("--spec, --manifest and --out are required");
    if (samples < 1 || samples > 1000) die("--samples out of range");
    if (samples < 20 && !smoke) die("spec section 7 requires N >= 20 samples per cell (use --smoke for less)");
    int spsel[NSP] = {0, 0, 0};
    for (int k = 0; k < nsp; k++) {
        int hit = 0;
        for (int j = 0; j < NSP; j++)
            if (fabs(sps[k] - k_sp[j]) < 1e-9) { spsel[j] = 1; hit = 1; }
        if (!hit) die("--sparsity values must be among 0, 0.33, 0.66");
    }

    /* quiet flag */
    const char *home = getenv("HOME");
    char quiet[512];
    snprintf(quiet, sizeof quiet, "%s/workspace/.spark-quiet", home ? home : "");
    struct stat qs;
    if (stat(quiet, &qs) == 0) {
        fprintf(stderr, "bench_polyglot: %s exists: the Spark is asked to be quiet; not running\n", quiet);
        return 3;
    }

    char contract[65];
    if (omx_contract_digest(spec, contract)) die("cannot compute contract digest (section 1 missing?)");
    load_manifest(manifest);
    mkdir(out, 0755);

    g_nc = omx_candidate_count();
    if (g_nc == 0 || g_nc > MAXC) die("candidate count out of range");

    run_info ri;
    memset(&ri, 0, sizeof ri);
    /* pin */
    const char *envcpu = getenv("POLYGLOT_BENCH_CPU");
    if (cpu_arg < 0 && envcpu && *envcpu) cpu_arg = atoi(envcpu);
    if (cpu_arg >= 0) {
        g_cpu = cpu_arg;
        snprintf(ri.cpu_why, sizeof ri.cpu_why, "requested (--cpu / POLYGLOT_BENCH_CPU)");
    } else {
        g_cpu = omx_pick_idle_cpu(0xd85, &ri.idle_frac);
        if (g_cpu < 0) g_cpu = 0;
        snprintf(ri.cpu_why, sizeof ri.cpu_why, "most idle Cortex-X925 over 0.3 s (%.0f%% idle)", ri.idle_frac * 100);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(g_cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set)) die("sched_setaffinity failed");
    sched_yield();
    if (sched_getcpu() != g_cpu) die("not running on the pinned cpu");
    g_part = omx_cpu_part(g_cpu);
    ri.capacity = omx_cpu_capacity(g_cpu);
    omx_cpu_governor(g_cpu, ri.governor, sizeof ri.governor);
    omx_kernel(ri.kernel, sizeof ri.kernel);
    {
        char prod[96] = "", vend[64] = "";
        FILE *f = fopen("/sys/devices/virtual/dmi/id/product_name", "r");
        if (f) { if (!fgets(prod, sizeof prod, f)) prod[0] = 0; fclose(f); }
        f = fopen("/sys/devices/virtual/dmi/id/sys_vendor", "r");
        if (f) { if (!fgets(vend, sizeof vend, f)) vend[0] = 0; fclose(f); }
        prod[strcspn(prod, "\n")] = 0;
        vend[strcspn(vend, "\n")] = 0;
        snprintf(ri.model, sizeof ri.model, "%s %s, Grace CPU (%d cpus, pinned core %s)", vend, prod, omx_ncpus(),
                 omx_core_name(g_part));
    }
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(ri.started, sizeof ri.started, "%Y%m%dT%H%M%SZ", &tmv);
    const char *commit = getenv("POLYGLOT_COMMIT");
    snprintf(ri.run_id, sizeof ri.run_id, "%s-%.12s%s", ri.started, commit && *commit ? commit : "nocommit",
             smoke ? "-smoke" : "");
    ri.nth0 = omx_thermal_read(ri.th0);
    ri.khz0 = omx_cpu_cur_khz(g_cpu);
    omx_loadavg(ri.load0, sizeof ri.load0);
    printf("bench_polyglot %s: %zu candidates, cpu %d (%s, capacity %d, %s, %ld kHz), N=%d%s\n", ri.run_id, g_nc,
           g_cpu, omx_core_name(g_part), ri.capacity, ri.governor, ri.khz0, samples, smoke ? " SMOKE" : "");

    double t_start = omx_now_ns();
    for (int wi = 0; wi < NW; wi++) {
        if (!wsel[wi]) continue;
        const workload *w = &k_w[wi];
        size_t m = w->m, n = w->n, nx = w->nx;
        for (int si = 0; si < NSP; si++) {
            if (!spsel[si]) continue;
            int8_t *W = malloc(m * n), *X = malloc(n * nx);
            int32_t *yref = malloc(m * nx * sizeof *yref), *y = malloc(m * sizeof *y);
            int64_t *pref = calloc(nx + 1, sizeof *pref);
            if (!W || !X || !yref || !y || !pref) die("out of memory");
            char dig[65];
            make_inputs(w, k_sp[si], W, X, dig);
            for (size_t k = 0; k < nx; k++) {
                if (oma_rz_oracle(W, m, n, X + k * n, yref + k * m)) die("oracle failed");
                int64_t s = 0;
                for (size_t i = 0; i < m; i++) s += yref[k * m + i];
                pref[k + 1] = pref[k] + s;
            }
            long khz = omx_cpu_cur_khz(g_cpu);
            /* pack + verify + calibrate */
            for (size_t ci = 0; ci < g_nc; ci++) {
                const oma_rz_impl *im = omx_candidate_get(ci)->impl;
                cell *ce = &g_cell[ci][wi][si];
                memset(ce, 0, sizeof *ce);
                ce->present = 1;
                ce->break_even = -2;
                ce->cur_khz = khz;
                snprintf(ce->input_sha, sizeof ce->input_sha, "%s", dig);
                if (n > im->max_n) {
                    snprintf(ce->skip, sizeof ce->skip, "n=%zu above max_n=%zu", n, im->max_n);
                    continue;
                }
                double pt[PACK_REPS];
                int rc = 0;
                for (int r = 0; r < PACK_REPS; r++) {
                    if (r) oma_rz_free(&ce->plan);
                    memset(&ce->plan, 0, sizeof ce->plan);
                    double t0 = omx_now_ns();
                    rc = im->pack(&ce->plan, W, m, n);
                    pt[r] = omx_now_ns() - t0;
                    if (rc) break;
                }
                if (rc) {
                    snprintf(ce->skip, sizeof ce->skip, "pack failed: %s", oma_rz_strerror(rc));
                    ce->checks++;
                    ce->failures++;
                    continue;
                }
                omx_sort_d(pt, PACK_REPS);
                ce->pack_min = pt[0];
                ce->pack_med = omx_median_sorted(pt, PACK_REPS);
                ce->weight_bytes = ce->plan.weight_bytes;
                ce->footprint = ce->plan.footprint_bytes;
                ce->scratch_bytes = ce->plan.scratch_bytes;
                ce->nnz = ce->plan.nnz;
                for (size_t k = 0; k < nx; k++) {
                    for (size_t i = 0; i < m; i++) y[i] = (int32_t)0x5a5a5a5a;
                    rc = im->run(&ce->plan, X + k * n, y);
                    ce->checks++;
                    if (rc || memcmp(y, yref + k * m, m * sizeof *y)) ce->failures++;
                }
                if (ce->failures) {
                    snprintf(ce->skip, sizeof ce->skip, "not bit-exact against oma_rz_oracle (%llu of %llu checks)",
                             ce->failures, ce->checks);
                    continue;
                }
                /* calibrate: min of 3 single calls (after one warm call) */
                int64_t s;
                double one = 1e30;
                sample(im, &ce->plan, X, n, nx, y, m, 1, &s, &rc);
                for (int r = 0; r < 3; r++) {
                    double t = sample(im, &ce->plan, X, n, nx, y, m, 1, &s, &rc);
                    if (t < one) one = t;
                }
                ce->reps = (long)ceil(MIN_SAMPLE_NS * 1.25 / (one > 1 ? one : 1));
                if (ce->reps < 1) ce->reps = 1;
                ce->s = calloc((size_t)samples, sizeof *ce->s);
                if (!ce->s) die("out of memory");
                ce->measured = 1;
            }
            /* round-robin timed samples; sample 0 of each candidate is preceded
             * by one untimed warm-up sample */
            for (size_t ci = 0; ci < g_nc; ci++) {
                cell *ce = &g_cell[ci][wi][si];
                if (!ce->measured) continue;
                int64_t s;
                int rc;
                sample(omx_candidate_get(ci)->impl, &ce->plan, X, n, nx, y, m, ce->reps, &s, &rc);
            }
            for (int k = 0; k < samples; k++)
                for (size_t ci = 0; ci < g_nc; ci++) {
                    cell *ce = &g_cell[ci][wi][si];
                    if (!ce->measured) continue;
                    const oma_rz_impl *im = omx_candidate_get(ci)->impl;
                    double t = 0;
                    for (int tries = 0;; tries++) {
                        int64_t s;
                        int rc;
                        uint64_t d0 = omx_run_delay_ns();
                        t = sample(im, &ce->plan, X, n, nx, y, m, ce->reps, &s, &rc);
                        uint64_t d1 = omx_run_delay_ns();
                        ce->checks++;
                        if (rc || s != expected_sum(pref, nx, ce->reps)) ce->failures++;
                        if (d1 == d0) break;
                        if (tries == MAX_RETAKE) { ce->contaminated++; break; }
                        ce->retakes++;
                    }
                    ce->s[ce->ns++] = t / (double)ce->reps;
                }
            /* statistics, break-even; free plans */
            size_t cheapest = (size_t)-1;
            for (size_t ci = 0; ci < g_nc; ci++) {
                cell *ce = &g_cell[ci][wi][si];
                if (!ce->measured) continue;
                if (ce->failures) {
                    ce->measured = 0;
                    snprintf(ce->skip, sizeof ce->skip, "timed-sample checksum mismatch (%llu failures)", ce->failures);
                    continue;
                }
                double *tmp = malloc((size_t)ce->ns * sizeof *tmp);
                if (!tmp) die("out of memory");
                memcpy(tmp, ce->s, (size_t)ce->ns * sizeof *tmp);
                omx_sort_d(tmp, (size_t)ce->ns);
                ce->lat_min = tmp[0];
                ce->lat_med = omx_median_sorted(tmp, (size_t)ce->ns);
                ce->lat_mad = omx_mad(tmp, (size_t)ce->ns, ce->lat_med);
                free(tmp);
                if (cheapest == (size_t)-1 || ce->pack_med < g_cell[cheapest][wi][si].pack_med) cheapest = ci;
            }
            for (size_t ci = 0; ci < g_nc; ci++) {
                cell *ce = &g_cell[ci][wi][si];
                oma_rz_free(&ce->plan);
                if (!ce->measured || cheapest == (size_t)-1) continue;
                cell *cb = &g_cell[cheapest][wi][si];
                ce->cheapest_pack = omx_candidate_get(cheapest)->impl->id;
                double dp = ce->pack_med - cb->pack_med, dt = cb->lat_med - ce->lat_med;
                if (ci == cheapest || dp <= 0) ce->break_even = 0;
                else if (dt <= 0) ce->break_even = -1;
                else ce->break_even = (long long)ceil(dp / dt);
                printf("  %s sp=%.2f %-16s median %12.1f ns  mad %9.1f  min %12.1f  reps %6ld  pack/w %.3f ns  "
                       "retakes %u\n",
                       w->id, k_sp[si], omx_candidate_get(ci)->impl->id, ce->lat_med, ce->lat_mad, ce->lat_min,
                       ce->reps, ce->pack_med / (double)(m * n), ce->retakes);
            }
            for (size_t ci = 0; ci < g_nc; ci++) {
                cell *ce = &g_cell[ci][wi][si];
                if (!ce->measured && ce->present)
                    printf("  %s sp=%.2f %-16s NOT TIMED: %s\n", w->id, k_sp[si], omx_candidate_get(ci)->impl->id,
                           ce->skip);
            }
            fflush(stdout);
            free(W); free(X); free(yref); free(y); free(pref);
        }
    }
    double t_end = omx_now_ns();
    ri.nth1 = omx_thermal_read(ri.th1);
    ri.khz1 = omx_cpu_cur_khz(g_cpu);
    omx_loadavg(ri.load1, sizeof ri.load1);

    /* build facts, after timing */
    char tmpdir[] = "/tmp/polyglot-ctime-XXXXXX";
    if (!mkdtemp(tmpdir)) die("mkdtemp failed");
    for (size_t ci = 0; ci < g_nc; ci++) {
        man_ent *e = man_for(omx_candidate_get(ci));
        if (e) measure_build(e, tmpdir);
    }
    char rmo[600];
    snprintf(rmo, sizeof rmo, "%s/ctime.o", tmpdir);
    unlink(rmo);
    snprintf(rmo, sizeof rmo, "%s/ctime.o.o", tmpdir);
    unlink(rmo);
    rmdir(tmpdir);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    ri.peak_rss_kib = ru.ru_maxrss;

    int nrec = 0, bad = 0;
    for (size_t ci = 0; ci < g_nc; ci++)
        for (int wi = 0; wi < NW; wi++) {
            if (!wsel[wi]) continue;
            if (write_receipt(out, omx_candidate_get(ci), wi, &ri, contract, spec, samples, smoke)) bad++;
            else nrec++;
        }
    printf("bench_polyglot %s: %d receipts in %s (%d write errors), timed wall %.1f s, peak RSS %ld KiB, "
           "contract %.16s\n",
           bad ? "FAIL" : "DONE", nrec, out, bad, (t_end - t_start) / 1e9, ri.peak_rss_kib, contract);
    return bad ? 1 : 0;
}
