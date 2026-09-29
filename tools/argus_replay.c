/*
 * argus_replay.c -- replay a raw ARGUS runtime stream (128-byte records, as
 * written by rx_argus RX_ARGUS_STREAM) into a fresh argus_core, twice, and
 * print per-kind counts, findings and the determinism check.
 *
 *   argus_replay FILE [-v]     -v also prints every finding's triggering event
 */
#include "argus_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static _Alignas(64) uint8_t core_mem[1 << 20];

typedef struct {
    uint64_t by_kind[ARGUS_EV_KIND_MAX + 1], by_code[ARGUS_F_MAX + 1];
    uint64_t events, decode_fail, findings;
    uint8_t state[ARGUS_DIGEST_LEN];
    ArgusCoreHealth h;
    uint64_t ingest_ns;
} Replay;

static void print_event(const char *tag, const ArgusEvent *e) {
    printf("%s seq=%llu kind=%u class=%u outcome=%u code=%d principal=%u cap=%u/%llu "
           "obj=%u res=%llu world=%llu flags=%u tick=%llu\n",
           tag, (unsigned long long)e->sequence, e->kind, e->class_, e->outcome, e->code,
           e->principal, e->cap_id, (unsigned long long)e->cap_generation, e->object_id,
           (unsigned long long)e->resource, (unsigned long long)e->world_generation, e->flags,
           (unsigned long long)e->tick);
}

static int replay(const uint8_t *buf, size_t n, Replay *r, int verbose) {
    memset(r, 0, sizeof *r);
    ArgusCore *core;
    if (argus_core_footprint() > sizeof core_mem ||
        argus_core_init(&core, core_mem, sizeof core_mem) != ARGUS_OK)
        return -1;
    for (size_t i = 0; i + ARGUS_EVENT_SIZE <= n; i += ARGUS_EVENT_SIZE) {
        ArgusEvent ev;
        if (argus_event_decode(buf + i, &ev) != ARGUS_OK) {
            r->decode_fail++;
            continue;
        }
        r->events++;
        if (ev.kind <= ARGUS_EV_KIND_MAX) r->by_kind[ev.kind]++;
        ArgusFinding f[32];
        size_t nf = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        argus_core_ingest(core, &ev, f, 32, &nf);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        r->ingest_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        for (size_t j = 0; j < nf; j++) {
            r->findings++;
            if (f[j].code <= ARGUS_F_MAX) r->by_code[f[j].code]++;
            if (verbose) {
                printf("finding code=%u sev=%u seq=%llu prior=%llu | ", f[j].code, f[j].severity,
                       (unsigned long long)f[j].sequence, (unsigned long long)f[j].prior_sequence);
                print_event("event", &ev);
            }
        }
        if (verbose > 1) print_event("  ", &ev);
    }
    argus_core_state_digest(core, r->state);
    argus_core_health(core, &r->h);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE [-v|-vv]\n", argv[0]);
        return 2;
    }
    int verbose = argc > 2 ? (strcmp(argv[2], "-vv") == 0 ? 2 : 1) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len > 0 ? (size_t)len : 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); return 2; }
    fclose(f);
    Replay a, b;
    if (replay(buf, (size_t)len, &a, verbose) || replay(buf, (size_t)len, &b, 0)) return 2;
    printf("records %ld (%s), decoded %llu, decode failures %llu\n", len / ARGUS_EVENT_SIZE,
           len % ARGUS_EVENT_SIZE ? "TRAILING BYTES" : "whole", (unsigned long long)a.events,
           (unsigned long long)a.decode_fail);
    for (unsigned k = 0; k <= ARGUS_EV_KIND_MAX; k++)
        if (a.by_kind[k]) printf("  kind %u: %llu\n", k, (unsigned long long)a.by_kind[k]);
    printf("core ingest: %.0f ns/event mean (second pass)\n", b.events ? (double)b.ingest_ns / (double)b.events : 0.0);
    printf("findings %llu\n", (unsigned long long)a.findings);
    for (unsigned c = 0; c <= ARGUS_F_MAX; c++)
        if (a.by_code[c]) printf("  code %u: %llu\n", c, (unsigned long long)a.by_code[c]);
    printf("core: received %llu rejected %llu not_applied %llu incidents %llu\n",
           (unsigned long long)a.h.events_received, (unsigned long long)a.h.events_rejected,
           (unsigned long long)a.h.events_not_applied, (unsigned long long)a.h.incidents_open);
    int same = memcmp(a.state, b.state, ARGUS_DIGEST_LEN) == 0 &&
               memcmp(a.h.chain, b.h.chain, ARGUS_DIGEST_LEN) == 0 && a.findings == b.findings;
    printf("state digest ");
    for (unsigned i = 0; i < ARGUS_DIGEST_LEN; i++) printf("%02x", a.state[i]);
    printf("\ndeterministic replay: %s\n", same ? "yes" : "NO");
    free(buf);
    return same ? 0 : 1;
}
