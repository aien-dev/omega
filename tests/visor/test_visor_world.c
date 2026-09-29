/*
 * test_visor_world.c -- lane 6: read-only World view.
 *
 * The FIXTURE builds a World exactly as tests/runtime/rx_heartbeat_test.c does
 * (rx_caproot_start, rx_world_init, rx_world_create, rx_world_retire). Those
 * calls belong to the test harness, not to the visor. The code under test
 * (visor_world_snapshot / formatters) calls no mutator.
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"
#include "visor_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_total;
#define CHECK(cond, msg) do { g_total++; if (cond) g_pass++; \
    else fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } while (0)

typedef struct { RxCapRoot root; RxCapAdmin admin; RxWorld w; } Env;

static int env_start(Env *e) {
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, 2, 1u << 12) != RX_OK) {
        rx_caproot_stop(&e->root, &e->admin);
        return -1;
    }
    e->w.external_subject = 100;
    return 0;
}

static void env_stop(Env *e) {
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
}

static RxObjRef mkobj(Env *e, RxPersist p, uint64_t resource, uint64_t v0) {
    uint64_t init[RX_MAX_FIELDS] = { v0 };
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&e->w, 1, p, resource, init, &r);
    return r;
}

static char t1[8192], t2[8192], j1[8192];

int main(void) {
    VisorWorldView v, v2;

    /* 1. NULL world fails closed. */
    memset(&v, 0x5a, sizeof(v));
    CHECK(visor_world_snapshot(NULL, &v) == -1, "NULL world accepted");
    CHECK(!v.attached && v.object_count == 0 && v.resident_object_count == 0,
          "NULL world left a claimed view");
    CHECK(strstr(v.gap_note, "fail closed") != NULL, "NULL world gap note missing");
    CHECK(visor_world_snapshot(NULL, NULL) == -1, "NULL out accepted");
    CHECK(visor_world_format_text(NULL, t1, sizeof t1) == -1, "format NULL view accepted");

    /* 2. Unattached view formats, deterministically, claiming nothing. */
    visor_world_unattached(&v);
    int a = visor_world_format_text(&v, t1, sizeof t1);
    int b = visor_world_format_text(&v, t2, sizeof t2);
    CHECK(a > 0 && a == b && strcmp(t1, t2) == 0, "unattached text not deterministic");
    CHECK(strstr(t1, "world: unattached") && strstr(t1, "does not link the runtime"),
          "unattached text does not say why");
    CHECK(visor_world_format_json(&v, j1, sizeof j1) > 0 &&
          strstr(j1, "\"attached\":false") && strstr(j1, "\"authority\":false"),
          "unattached json wrong");
    CHECK(!v.attached && !v.staged_effects_visible && v.generation == 0,
          "unattached view claims state");

    /* 3. A real World, built like rx_heartbeat_test. */
    Env e;
    memset(&e, 0, sizeof(e));
    if (env_start(&e) != 0) {
        fprintf(stderr, "FAIL: could not start capability root / world\n");
        printf("FAIL %d/%d\n", g_pass, g_total + 1);
        return 1;
    }
    RxObjRef o1 = mkobj(&e, RX_PERSIST_RESIDENT, 0x10, 7);
    RxObjRef o2 = mkobj(&e, RX_PERSIST_EPHEMERAL, 0x20, 8);
    RxObjRef o3 = mkobj(&e, RX_PERSIST_DURABLE, 0x30, 9);
    CHECK(o1.id != UINT32_MAX && o2.id != UINT32_MAX && o3.id != UINT32_MAX,
          "fixture could not create objects");
    CHECK(rx_world_wait_quiescent(&e.w, 2000) == RX_OK, "world not quiescent");

    uint64_t crumbs_before = e.w.n_crumbs;
    uint8_t d_before[32];
    rx_world_digest(&e.w, d_before);

    CHECK(visor_world_snapshot(&e.w, &v) == 0, "snapshot failed");
    CHECK(visor_world_snapshot(&e.w, &v2) == 0, "second snapshot failed");
    a = visor_world_format_text(&v, t1, sizeof t1);
    b = visor_world_format_text(&v2, t2, sizeof t2);
    CHECK(a > 0 && a == b && strcmp(t1, t2) == 0, "attached text not deterministic");
    a = visor_world_format_text(&v, t2, sizeof t2);
    CHECK(strcmp(t1, t2) == 0, "same view formats differently twice");

    CHECK(v.attached && v.object_count == 3 && v.resident_object_count == 3,
          "object counts wrong");
    CHECK(v.reaction_count == 0 && v.active_work == 0, "reaction/work counts wrong");

    /* 4. Each object matches rx_world_read (generation and persistence). */
    const RxPersist want[3] = { RX_PERSIST_RESIDENT, RX_PERSIST_EPHEMERAL, RX_PERSIST_DURABLE };
    const char *want_kind[3] = { "RESIDENT", "EPHEMERAL", "DURABLE" };
    for (size_t i = 0; i < v.object_count && i < 3; i++) {
        unsigned id = 0, gen = 0;
        CHECK(sscanf(v.objects[i].id, "rx:%u@%u", &id, &gen) == 2, "object id unparsable");
        RxObject ro;
        RxObjRef ref = { id, gen };
        CHECK(rx_world_read(&e.w, ref, &ro) == RX_OK, "view names an object rx_world_read rejects");
        CHECK(ro.generation == gen && ro.id == id, "generation differs from rx_world_read");
        CHECK(ro.persist == want[i] && strcmp(v.objects[i].kind, want_kind[i]) == 0,
              "kind differs from runtime persistence");
        CHECK(strcmp(v.objects[i].state, ro.placed ? "live,placed" : "live") == 0,
              "state differs from runtime placement");
    }

    /* 5. Digest and tail match the public read calls. */
    char dhex[72];
    memcpy(dhex, "sha256:", 7);
    for (int i = 0; i < 32; i++) snprintf(dhex + 7 + 2 * i, 3, "%02x", d_before[i]);
    CHECK(strcmp(v.digest, dhex) == 0, "digest differs from rx_world_digest");
    CHECK(v.publication_tail == rx_world_publication_tail(&e.w), "tail differs");

    /* 6. No field claims visibility the runtime does not grant. */
    CHECK(!v.staged_effects_visible && v.staged_effects == 0, "staged effects claimed");
    CHECK(v.generation == 0 && strstr(v.gap_note, "generation"), "generation claimed");
    CHECK(v.published_results == 0 && strstr(v.gap_note, "published results"),
          "published results claimed");
    CHECK(strstr(t1, "(not visible)") && strstr(t1, "generation:         (not observable)"),
          "text claims unobservable fields");
    CHECK(visor_world_format_json(&v, j1, sizeof j1) > 0 &&
          strstr(j1, "\"staged_effects_visible\":false") &&
          strstr(j1, "\"generation_observable\":false") && strstr(j1, "\"authority\":false"),
          "json claims visibility");

    /* 7. Snapshot mutated nothing. */
    uint8_t d_after[32];
    rx_world_digest(&e.w, d_after);
    CHECK(memcmp(d_before, d_after, 32) == 0 && e.w.n_crumbs == crumbs_before,
          "snapshot changed the World");

    /* 8. A retired object disappears; its stale ref is refused by the runtime. */
    CHECK(rx_world_retire(&e.w, o2) == RX_OK, "fixture retire failed");
    CHECK(visor_world_snapshot(&e.w, &v) == 0 && v.object_count == 2, "retired object shown");
    char stale[72];
    snprintf(stale, sizeof stale, "rx:%u@%u", o2.id, o2.generation);
    int seen = 0;
    for (size_t i = 0; i < v.object_count; i++) seen |= strcmp(v.objects[i].id, stale) == 0;
    CHECK(!seen, "stale generation still listed");

    /* 9. Bounded: more than 32 live objects truncates honestly. */
    for (int i = 0; i < 40; i++) mkobj(&e, RX_PERSIST_EPHEMERAL, 0x40, (uint64_t)i);
    CHECK(visor_world_snapshot(&e.w, &v) == 0 && v.object_count == 32 &&
          v.resident_object_count == 42 && strstr(v.gap_note, "truncated"),
          "truncation not bounded/declared");
    CHECK(strstr(v.gap_note, "quiescent") != NULL, "gap note itself was cut off");

    /* 10. Output that does not fit fails closed. */
    char tiny[16];
    CHECK(visor_world_format_text(&v, tiny, sizeof tiny) == -1 && tiny[0] == '\0',
          "text overflow not refused");
    CHECK(visor_world_format_json(&v, tiny, sizeof tiny) == -1 && tiny[0] == '\0',
          "json overflow not refused");

    env_stop(&e);
    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
