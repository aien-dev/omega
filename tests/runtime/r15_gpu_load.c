/*
 * r15_gpu_load.c -- bounded GPU load for SPBM sensor validation (R15 C2).
 *
 * Starts the R12 resident seat on GB10 and, for the given number of seconds,
 * feeds it claims back to back (publish A -> chip add -> B), checking every
 * result. "idle" mode starts the seat and posts nothing (the seat's own
 * polling loop only). No power limit, clock or firmware setting is touched.
 *
 *   r15_gpu_load claims|idle SECONDS
 *
 * Prints one JSON line: mode, seconds, claims completed, wrong results.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_resident_gpu.h"
#include "runtime/rx_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { SUBJ_SEAT = 3, SUBJ_EXTERNAL = 100, ISSUER = 3 };
#define RES_A 0x10u
#define RES_B 0x20u

static AienosCapAdmin *g_admin;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static RxCapRef mint(uint32_t subject, uint64_t resource, uint32_t rights) {
    AienosCapRef office, r = {UINT32_MAX, 0};
    aienos_cap_office(g_admin, &office);
    AienosCapMint m = {ISSUER, subject, resource, rights, 0, {UINT32_MAX, 0}, office};
    if (aienos_cap_mint(g_admin, &m, &r) != 0) r = (AienosCapRef){UINT32_MAX, 0};
    return (RxCapRef){r.cap_id, r.generation};
}

static int fn_never(RxCtx *c) { (void)c; return -1; }  /* the seat is not a CPU call */

static uint64_t field(RxWorld *w, RxObjRef r, uint32_t f) {
    RxObject o;
    return rx_world_read(w, r, &o) == RX_OK ? o.field[f] : UINT64_MAX;
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "claims") && strcmp(argv[1], "idle"))) {
        fprintf(stderr, "usage: r15_gpu_load claims|idle SECONDS\n");
        return 2;
    }
    long seconds = atol(argv[2]);
    if (seconds < 1 || seconds > 60) return 2;
    int claims = !strcmp(argv[1], "claims");
    AienosCapView *view = NULL;
    static RxWorld w;
    if (aienos_cap_start(&g_admin, &view) != 0) return 1;
    if (rx_world_init_native(&w, view, 1, 1u << 20) != RX_OK) return 1;
    w.external_subject = SUBJ_EXTERNAL;
    uint64_t zero[RX_MAX_FIELDS] = {0};
    RxObjRef A, B;
    if (rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_A, zero, &A) != RX_OK ||
        rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_B, zero, &B) != RX_OK ||
        rx_world_attach_physical(&w, A) != RX_OK || rx_world_attach_physical(&w, B) != RX_OK)
        return 1;
    RxCapRef in = mint(SUBJ_SEAT, RES_A, RX_RIGHT_READ);
    RxCapRef out = mint(SUBJ_SEAT, RES_B, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef ext = mint(SUBJ_EXTERNAL, RES_A, RX_RIGHT_WRITE);
    if (rx_world_bind_capability(&w, A, in) != RX_OK ||
        rx_world_bind_capability(&w, B, out) != RX_OK || rx_world_enable_resident(&w) != RX_OK)
        return 1;
    RxResourceBudget b;
    memset(&b, 0, sizeof b);
    b.slots = 8;
    b.memory_bytes = UINT64_MAX;
    b.energy_budget = UINT64_MAX;
    b.offered_locality = UINT32_MAX;
    b.offered_accel = RX_ACCEL_BLACKWELL;
    b.compute_mask = UINT32_MAX;
    rx_world_set_resources(&w, &b);
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "resident.seat.add";
    d.faculty = RX_FACULTY_AEGIS;
    d.subject = SUBJ_SEAT;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_never;
    d.need.accelerator_features = RX_ACCEL_BLACKWELL;
    d.n_triggers = 1;
    d.triggers[0] = (RxDep){A, RX_FIELD(0) | RX_FIELD(1)};
    d.n_writes = 1;
    d.writes[0] = (RxDep){B, RX_FIELD(0)};
    d.n_caps = 2;
    d.caps[0] = (RxCapNeed){in, RES_A, RX_RIGHT_READ};
    d.caps[1] = (RxCapNeed){out, RES_B, RX_RIGHT_WRITE};
    uint32_t seat_id = 0;
    if (rx_world_add_reaction(&w, &d, &seat_id) != RX_OK) return 1;
    RxGpuSeat *seat = NULL;
    if (rx_gpu_seat_begin(&w, &seat) != 0 || !seat) return 1;

    uint64_t done = 0, wrong = 0, start = now_ns(), end = start + (uint64_t)seconds * 1000000000ull;
    int rc = 0;
    while (now_ns() < end) {
        if (!claims) {
            struct timespec ts = {0, 10000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        uint64_t a0 = (done * 2654435761u) & 0x7fffffffu, a1 = (done + 7) & 0x7fffffffu;
        RxMutation m[2] = {{A, 0, a0}, {A, 1, a1}};
        if (rx_world_publish_external(&w, ext, m, 2) <= 0) { rc = 1; break; }
        uint64_t want = (uint32_t)(a0 + a1), t0 = now_ns();
        for (;;) {
            int arc = rx_resident_accept(&w);
            if (arc != RX_OK && arc != RX_ERR_NOT_FOUND) { rc = 1; break; }
            if (rx_world_wait_quiescent(&w, 0) == RX_OK) break;
            if (now_ns() - t0 > 2000000000ull) { rc = 1; break; }
        }
        if (rc) break;
        if (field(&w, B, 0) != want) wrong++;
        done++;
    }
    uint64_t took = now_ns() - start;
    (void)rx_resident_shutdown(&w);
    (void)rx_gpu_seat_finish(seat);
    printf("{\"mode\":\"%s\",\"seconds\":%.3f,\"claims\":%llu,\"wrong\":%llu,\"error\":%d}\n",
           argv[1], (double)took / 1e9, (unsigned long long)done, (unsigned long long)wrong, rc);
    rx_world_destroy(&w);
    aienos_cap_stop(g_admin, view);
    return rc || wrong ? 1 : 0;
}
