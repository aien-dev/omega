/*
 * rx_dual_jspace_tap_bench.c -- overhead of the DUAL-3a observation tap at
 * decision site "jspace.residency". Public API only.
 *
 * Built twice by mk/dual_jspace_tap.mk: against the base commit's rx_jspace.c
 * (-DDUAL_TAP_BENCH_BASE: observer-off leg only, the pre-change timing) and
 * against the current one (observer off, then a recording observer that
 * encodes and keeps every record). Same workload, fixed costs, trials
 * interleaved; medians reported. Thresholds are pre-registered in
 * evidence/DUAL/3a-jspace-tap/receipt.md, not here.
 *
 * Workload per round: make every unit resident again (untimed), then two timed
 * js_forge_enforce calls: hot-arena SOFT case (budget 3/2 of residency) and
 * over-total CAPACITY case (budget 1/2 of residency).
 */
#include "runtime/rx_jspace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UNIT 4096u
#define FANOUT 256u
#define DEPTH 4u
#define ROUNDS 40
#define TRIALS 7

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void derive_sparse(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    uint64_t x = token;
    if (!prev) {
        for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t v = splitmix(&x); memcpy(out + i, &v, 8); }
        return;
    }
    memcpy(out, prev, n);
    for (size_t i = (size_t)(token % 8) * 8; i + 8 <= n; i += 64) {
        uint64_t v = splitmix(&x);
        memcpy(out + i, &v, 8);
    }
}

static const JsRealizer RZ = { JS_REAL_LATENT_CHECKPOINT, "sparse", UNIT, derive_sparse };

static void costs_plain(JsSpace *s) {
    JsCosts c = { 0 };
    c.derive_ns = (double)UNIT;
    c.copy_ns_per_byte = 0.125;
    c.move_ns_per_byte = 0.125;
    c.compress_ns_per_byte = 0.5;
    c.decompress_ns_per_byte = 0.5;
    c.compress_ratio = 0.25;
    c.spill_write_ns_per_byte = 2.0;
    c.spill_read_ns_per_byte = 4.0;
    c.retain_ns_per_byte = 1.0 / 1024.0;
    s->costs = c;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifndef DUAL_TAP_BENCH_BASE
typedef struct { uint64_t records, bytes; uint8_t xor_digest[32]; } Sink;
static void sink(const JsDualObservation *o, const void *ctx) {
    Sink *k = (Sink *)ctx;
    uint8_t enc[JS_DUAL_OBS_BYTES];
    k->bytes += js_dual_observation_encode(o, enc);
    for (int i = 0; i < 32; i++) k->xor_digest[i] ^= o->digest[i];
    k->records++;
}
#endif

typedef struct { uint64_t wall_ns, decisions, records, bytes; } Trial;

static Trial trial(int observe) {
    Trial t = { 0, 0, 0, 0 };
    JsSpace s;
    char path[96];
    snprintf(path, sizeof path, "/tmp/jstap_bench_%ld", (long)getpid());
    if (js_space_init(&s, path) != JS_OK) { fprintf(stderr, "init failed\n"); exit(2); }
    costs_plain(&s);
    uint32_t root;
    js_branch_root(&s, &RZ, 7, &root);
    for (unsigned i = 0; i < DEPTH; i++) js_branch_derive(&s, root, 100 + i);
    for (unsigned k = 0; k < FANOUT; k++) {
        uint32_t c;
        js_branch_fork(&s, root, &c);
        js_branch_derive(&s, c, 1000 + k);
    }
#ifndef DUAL_TAP_BENCH_BASE
    Sink snk;
    memset(&snk, 0, sizeof snk);
    if (observe) js_dual_set_observer(&s, sink, &snk);
#else
    (void)observe;
#endif
    for (int round = 0; round < ROUNDS; round++) {
        for (uint32_t b = 0; b < JS_MAX_BRANCHES; b++) {
            JsBranch *br = s.branches[b];
            if (!br) continue;
            for (uint32_t i = 0; i < br->n_units; i++) js_real_restore(&s, br->units[i]);
        }
        JsPolicyReport rep;
        uint64_t res = s.stats.resident_bytes;
        memset(&rep, 0, sizeof rep);
        uint64_t t0 = now_ns();
        js_forge_enforce(&s, res * 3 / 2, &rep);
        uint64_t t1 = now_ns();
        t.wall_ns += t1 - t0;
        t.decisions += rep.considered;
        res = s.stats.resident_bytes;
        memset(&rep, 0, sizeof rep);
        t0 = now_ns();
        js_forge_enforce(&s, res / 2, &rep);
        t1 = now_ns();
        t.wall_ns += t1 - t0;
        t.decisions += rep.considered;
    }
#ifndef DUAL_TAP_BENCH_BASE
    t.records = snk.records;
    t.bytes = snk.bytes;
#endif
    js_space_destroy(&s);
    return t;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t median(uint64_t *v, int n) { qsort(v, n, sizeof *v, cmp_u64); return v[n / 2]; }

int main(void) {
#ifdef DUAL_TAP_BENCH_BASE
    const char *build = "base";
#else
    const char *build = "tap";
#endif
    uint64_t off[TRIALS], on[TRIALS];
    Trial last_off = { 0, 0, 0, 0 }, last_on = { 0, 0, 0, 0 };
    for (int i = 0; i < TRIALS; i++) {
        last_off = trial(0); off[i] = last_off.wall_ns;
#ifndef DUAL_TAP_BENCH_BASE
        last_on = trial(1); on[i] = last_on.wall_ns;
#else
        on[i] = 0;
#endif
    }
    uint64_t m_off = median(off, TRIALS);
#ifdef DUAL_TAP_BENCH_BASE
    (void)on; (void)last_on;
#endif
    printf("bench[%s] observer=NULL  median_wall_ns=%" PRIu64 " decisions=%" PRIu64 " records=0 bytes=0 "
           "(rounds %d, trials %d, fanout %u, unit %u)\n", build, m_off, last_off.decisions, ROUNDS, TRIALS, FANOUT, UNIT);
#ifndef DUAL_TAP_BENCH_BASE
    uint64_t m_on = median(on, TRIALS);
    printf("bench[%s] observer=record median_wall_ns=%" PRIu64 " decisions=%" PRIu64 " records=%" PRIu64 " bytes=%" PRIu64 "\n",
           build, m_on, last_on.decisions, last_on.records, last_on.bytes);
    double per_record = last_on.records ? ((double)m_on - (double)m_off) / (double)last_on.records : 0;
    printf("bench[%s] ratio_on_off=%.3f added_ns_per_record=%.1f records_equal_decisions=%s\n",
           build, m_off ? (double)m_on / (double)m_off : 0, per_record,
           last_on.records == last_on.decisions ? "yes" : "NO");
#endif
    return 0;
}
