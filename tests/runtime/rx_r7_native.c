/*
 * rx_r7_native.c -- same attacks against the Linux mint and the native
 * AIENOS authority. The world is then pointed at the native view only.
 * The Linux oracle is not removed.
 */
#include "omega_evidence.h"
#include "runtime/aienos_cap.h"
#include "runtime/rx_world.h"
#include "sha256.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;

typedef struct {
    char name[48];   /* copied: some names are built on the caller's stack */
    int code;
} DiffResult;

static DiffResult g_diff[96];
static int g_ndiff;

static void expect_eq(const char *name, int linux_rc, int native_rc) {
    if (linux_rc != native_rc) {
        fprintf(stderr, "FAIL %s: linux %s (%d), native %s (%d)\n", name,
                rx_cap_strerror(linux_rc), linux_rc, rx_cap_strerror(native_rc), native_rc);
        g_fail++;
        return;
    }
    if (g_ndiff < (int)(sizeof g_diff / sizeof g_diff[0])) {
        snprintf(g_diff[g_ndiff].name, sizeof g_diff[g_ndiff].name, "%s", name);
        g_diff[g_ndiff].code = linux_rc;
        g_ndiff++;
    }
}

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    AienosCapAdmin *native_admin;
    AienosCapView *native_view;
    /* Each authority starts its table at its own boot generation (time
     * seeded), so the two agree on generations relative to their office. */
    uint64_t linux_base;
    uint64_t native_base;
} Pair;

static int refs_match(const Pair *p, RxCapRef linux_ref, AienosCapRef native_ref) {
    return linux_ref.cap_id == native_ref.cap_id &&
           linux_ref.generation - p->linux_base == native_ref.generation - p->native_base;
}

/* The native reference for a Linux one: same slot, same offset from boot.
 * Forged generations map one to one, so they stay forged. */
static AienosCapRef to_native(const Pair *p, RxCapRef ref) {
    return (AienosCapRef){ ref.cap_id, ref.generation - p->linux_base + p->native_base };
}

static int pair_start(Pair *p) {
    memset(p, 0, sizeof(*p));
    int linux_rc = rx_caproot_start(&p->root, &p->admin);
    int native_rc = aienos_cap_start(&p->native_admin, &p->native_view);
    expect_eq("start", linux_rc, native_rc);
    if (linux_rc == RX_CAP_OK && native_rc == RX_CAP_OK) {
        AienosCapRef office;
        aienos_cap_office(p->native_admin, &office);
        p->linux_base = p->admin.office.generation;
        p->native_base = office.generation;
    }
    return linux_rc == RX_CAP_OK && native_rc == RX_CAP_OK ? 0 : -1;
}

static void pair_stop(Pair *p) {
    rx_caproot_stop(&p->root, &p->admin);
    aienos_cap_stop(p->native_admin, p->native_view);
    p->native_admin = NULL;
    p->native_view = NULL;
}

static void fill_mint(const Pair *p, RxCapMint *linux_mint, AienosCapMint *native_mint,
                      uint32_t subject,
                      uint64_t resource, uint32_t rights, uint64_t lease, RxCapRef parent) {
    memset(linux_mint, 0, sizeof(*linux_mint));
    memset(native_mint, 0, sizeof(*native_mint));
    linux_mint->issuer = 3;
    native_mint->issuer = 3;
    linux_mint->subject = subject;
    native_mint->subject = subject;
    linux_mint->resource = resource;
    native_mint->resource = resource;
    linux_mint->rights = rights;
    native_mint->rights = rights;
    linux_mint->lease_ticks = lease;
    native_mint->lease_ticks = lease;
    linux_mint->parent = parent;
    native_mint->parent = parent.cap_id == UINT32_MAX ? (AienosCapRef){ UINT32_MAX, 0 }
                                                      : to_native(p, parent);
}

static int mint_both(Pair *p, const char *name, uint32_t subject, uint64_t resource,
                     uint32_t rights, uint64_t lease, RxCapRef parent, int parent_is_authority,
                     RxCapRef *out) {
    RxCapMint linux_mint;
    AienosCapMint native_mint;
    fill_mint(p, &linux_mint, &native_mint, subject, resource, rights, lease, parent);
    if (parent_is_authority) {
        linux_mint.authority = parent;
        native_mint.authority = native_mint.parent;
    } else {
        linux_mint.authority = p->admin.office;
        AienosCapRef office;
        aienos_cap_office(p->native_admin, &office);
        native_mint.authority = office;
    }
    RxCapRef linux_out = { UINT32_MAX, 0 };
    AienosCapRef native_out = { UINT32_MAX, 0 };
    int linux_rc = rx_capadmin_mint(&p->admin, &linux_mint, &linux_out);
    int native_rc = aienos_cap_mint(p->native_admin, &native_mint, &native_out);
    expect_eq(name, linux_rc, native_rc);
    if (linux_rc == RX_CAP_OK || native_rc == RX_CAP_OK) {
        if (!refs_match(p, linux_out, native_out)) {
            fprintf(stderr, "FAIL %s: ref linux %u/%llu native %u/%llu\n", name,
                    linux_out.cap_id, (unsigned long long)linux_out.generation,
                    native_out.cap_id, (unsigned long long)native_out.generation);
            g_fail++;
            return -1;
        }
    }
    if (out && linux_rc == RX_CAP_OK) *out = linux_out;
    return linux_rc == native_rc ? 0 : -1;
}

static void validate_both(Pair *p, const char *name, RxCapRef cap, uint32_t subject,
                          uint64_t resource, uint32_t rights) {
    AienosCapRef native = to_native(p, cap);
    int linux_rc = rx_caproot_validate(&p->root, cap, subject, resource, rights, NULL);
    int native_rc = aienos_cap_validate(p->native_view, native, subject, resource, rights, NULL);
    expect_eq(name, linux_rc, native_rc);
}

static int corpus(void) {
    Pair p;
    if (pair_start(&p) != 0) return -1;
    RxCapRef linux_office = p.admin.office;
    AienosCapRef native_office;
    aienos_cap_office(p.native_admin, &native_office);
    if (!refs_match(&p, linux_office, native_office)) {
        fprintf(stderr, "FAIL office identity diverged\n");
        g_fail++;
    }

    RxCapRef none = { UINT32_MAX, 0 };
    RxCapRef read_cap;
    if (mint_both(&p, "mint read", 1, 0x10, RX_RIGHT_READ, 0, none, 0, &read_cap) != 0) {
        pair_stop(&p);
        return -1;
    }
    validate_both(&p, "valid read", read_cap, 1, 0x10, RX_RIGHT_READ);
    validate_both(&p, "forged handle", (RxCapRef){ 200, 7 }, 1, 0x10, RX_RIGHT_READ);
    validate_both(&p, "out of range handle", (RxCapRef){ 300, 1 }, 1, 0x10, RX_RIGHT_READ);
    validate_both(&p, "wrong subject", read_cap, 7, 0x10, RX_RIGHT_READ);
    validate_both(&p, "wrong resource", read_cap, 1, 0x30, RX_RIGHT_READ);
    validate_both(&p, "missing rights", read_cap, 1, 0x10, RX_RIGHT_WRITE);

    uint64_t wide = (1ull << 32) | 0x51u;
    RxCapRef wide_cap;
    mint_both(&p, "mint wide resource", 1, wide, RX_RIGHT_READ, 0, none, 0, &wide_cap);
    validate_both(&p, "wide resource kept", wide_cap, 1, wide, RX_RIGHT_READ);
    validate_both(&p, "truncated resource refused", wide_cap, 1, 0x51, RX_RIGHT_READ);

    RxCapRef revoked;
    mint_both(&p, "mint to revoke", 1, 0x20, RX_RIGHT_WRITE, 0, none, 0, &revoked);
    expect_eq("revoke", rx_capadmin_revoke(&p.admin, linux_office, revoked),
              aienos_cap_revoke(p.native_admin, native_office,
                                to_native(&p, revoked)));
    validate_both(&p, "revoked", revoked, 1, 0x20, RX_RIGHT_WRITE);
    expect_eq("reclaim", rx_capadmin_reclaim(&p.admin, linux_office, revoked.cap_id),
              aienos_cap_reclaim(p.native_admin, native_office, revoked.cap_id));
    RxCapRef reused;
    mint_both(&p, "remint reclaimed", 7, 0x20, RX_RIGHT_WRITE, 0, none, 0, &reused);
    if (reused.cap_id != revoked.cap_id || reused.generation != revoked.generation + 1) {
        fprintf(stderr, "FAIL reclaim did not advance generation\n");
        g_fail++;
    }
    validate_both(&p, "stale generation", revoked, 1, 0x20, RX_RIGHT_WRITE);

    RxCapRef parent;
    mint_both(&p, "delegable parent", 1, 0x20, RX_RIGHT_READ | RX_RIGHT_DELEGATE, 0, none, 0,
              &parent);
    mint_both(&p, "rights amplification", 2, 0x20, RX_RIGHT_READ | RX_RIGHT_WRITE, 0, parent, 1,
              NULL);
    mint_both(&p, "resource change", 2, 0x30, RX_RIGHT_READ, 0, parent, 1, NULL);
    mint_both(&p, "not delegable", 2, 0x10, RX_RIGHT_READ, 0, read_cap, 1, NULL);
    RxCapRef child;
    mint_both(&p, "attenuated child", 2, 0x20, RX_RIGHT_READ, 0, parent, 1, &child);
    validate_both(&p, "child valid", child, 2, 0x20, RX_RIGHT_READ);
    expect_eq("revoke ancestor", rx_capadmin_revoke(&p.admin, linux_office, parent),
              aienos_cap_revoke(p.native_admin, native_office,
                                to_native(&p, parent)));
    validate_both(&p, "revoked ancestor", child, 2, 0x20, RX_RIGHT_READ);

    RxCapRef leased;
    mint_both(&p, "leased", 1, 0x10, RX_RIGHT_READ, 5, none, 0, &leased);
    expect_eq("advance clock", rx_capadmin_advance_clock(&p.admin, linux_office, 10),
              aienos_cap_advance_clock(p.native_admin, native_office, 10));
    validate_both(&p, "expired lease", leased, 1, 0x10, RX_RIGHT_READ);

    RxCapRef before_epoch;
    mint_both(&p, "before epoch", 1, 0x10, RX_RIGHT_READ, 0, none, 0, &before_epoch);
    expect_eq("bump epoch", rx_capadmin_bump_epoch(&p.admin, linux_office),
              aienos_cap_bump_epoch(p.native_admin, native_office));
    validate_both(&p, "stale epoch", before_epoch, 1, 0x10, RX_RIGHT_READ);
    expect_eq("stale office epoch", rx_capadmin_bump_epoch(&p.admin, linux_office),
              aienos_cap_bump_epoch(p.native_admin, native_office));

    pair_stop(&p);
    if (pair_start(&p) != 0) return -1;
    linux_office = p.admin.office;
    aienos_cap_office(p.native_admin, &native_office);
    mint_both(&p, "privileged plus delegate", 1, 0, RX_RIGHT_REVOKE | RX_RIGHT_DELEGATE, 0, none,
              0, NULL);
    RxCapRef revoker;
    mint_both(&p, "limited revoker", 1, 0, RX_RIGHT_REVOKE, 0, none, 0, &revoker);
    mint_both(&p, "delegate admin right", 2, 0, RX_RIGHT_REVOKE, 0, revoker, 1, NULL);
    expect_eq("narrow revoker vs office",
              rx_capadmin_revoke(&p.admin, revoker, linux_office),
              aienos_cap_revoke(p.native_admin, to_native(&p, revoker),
                                native_office));
    validate_both(&p, "office still live", linux_office, 0, 0, RX_RIGHT_MINT);

    RxCapRef cur;
    mint_both(&p, "depth base", 1, 0x20, RX_RIGHT_READ | RX_RIGHT_DELEGATE, 0, none, 0, &cur);
    for (int i = 0; i < 8; i++) {
        char name[32];
        snprintf(name, sizeof(name), "depth %d", i + 1);
        if (mint_both(&p, name, 2, 0x20, RX_RIGHT_READ | RX_RIGHT_DELEGATE, 0, cur, 1, &cur) != 0)
            break;
    }
    mint_both(&p, "delegation too deep", 2, 0x20, RX_RIGHT_READ | RX_RIGHT_DELEGATE, 0, cur, 1,
              NULL);

    uint64_t advanced = 7;
    expect_eq("generation wrap", rx_cap_generation_advance(UINT64_MAX, &advanced),
              aienos_cap_generation_advance(UINT64_MAX, &advanced));
    if (advanced != 7) {
        fprintf(stderr, "FAIL generation wrap wrote a new generation\n");
        g_fail++;
    }
    expect_eq("clock near the top",
              rx_capadmin_advance_clock(&p.admin, linux_office, UINT64_MAX - 8),
              aienos_cap_advance_clock(p.native_admin, native_office, UINT64_MAX - 8));
    expect_eq("clock wrap", rx_capadmin_advance_clock(&p.admin, linux_office, 100),
              aienos_cap_advance_clock(p.native_admin, native_office, 100));

    uint64_t before_clock = aienos_cap_clock(p.native_view);
    AienosCapMint sneak;
    memset(&sneak, 0, sizeof(sneak));
    sneak.issuer = 1;
    sneak.subject = 1;
    sneak.resource = 1;
    sneak.rights = RX_RIGHT_MINT;
    sneak.parent.cap_id = UINT32_MAX;
    sneak.authority = native_office;
    uint8_t guessed[32];
    memset(guessed, 0, sizeof(guessed));
    int native_cog = aienos_cap_cognition_mint(p.native_view, &sneak, guessed);
    RxCapRequest forged;
    memset(&forged, 0, sizeof(forged));
    forged.op = 1; /* mint */
    forged.mint.issuer = 1;
    forged.mint.subject = 1;
    forged.mint.resource = 1;
    forged.mint.rights = RX_RIGHT_MINT;
    forged.mint.parent.cap_id = UINT32_MAX;
    forged.mint.authority = linux_office;
    forged.authority = linux_office;
    int wrote = write(p.admin.ctl_fd, &forged, sizeof(forged)) == (ssize_t)sizeof(forged);
    struct { int32_t status; uint32_t pad; RxCapRef ref; } reply;
    memset(&reply, 0, sizeof(reply));
    int got = read(p.admin.ctl_fd, &reply, sizeof(reply)) == (ssize_t)sizeof(reply);
    if (!wrote || !got) {
        fprintf(stderr, "FAIL could not complete the forged linux mint\n");
        g_fail++;
    } else {
        expect_eq("cognition mint", reply.status, native_cog);
    }
    int native_admin_try = aienos_cap_cognition_admin(p.native_view, 2, native_office, native_office);
    expect_eq("cognition admin", RX_CAP_ERR_UNAUTHORIZED, native_admin_try);
    if (aienos_cap_clock(p.native_view) != before_clock) {
        fprintf(stderr, "FAIL cognition moved the clock\n");
        g_fail++;
    }

    RxCapRef old;
    mint_both(&p, "before restart", 1, 0x10, RX_RIGHT_READ, 0, none, 0, &old);
    RxCapRef old_office = linux_office;
    if (kill(p.root.root_pid, SIGKILL) != 0) {
        fprintf(stderr, "FAIL could not stop the linux mint\n");
        g_fail++;
    }
    int status = 0;
    waitpid(p.root.root_pid, &status, 0);
    expect_eq("kill native writer", RX_CAP_OK, aienos_cap_kill(p.native_admin));
    RxCapMint dead;
    memset(&dead, 0, sizeof(dead));
    dead.issuer = 3;
    dead.subject = 1;
    dead.resource = 0x10;
    dead.rights = RX_RIGHT_READ;
    dead.parent = none;
    dead.authority = old_office;
    AienosCapMint dead_native;
    memset(&dead_native, 0, sizeof(dead_native));
    dead_native.issuer = 3;
    dead_native.subject = 1;
    dead_native.resource = 0x10;
    dead_native.rights = RX_RIGHT_READ;
    dead_native.parent.cap_id = UINT32_MAX;
    dead_native.authority = native_office;
    expect_eq("mint after death", rx_capadmin_mint(&p.admin, &dead, NULL),
              aienos_cap_mint(p.native_admin, &dead_native, NULL));
    rx_caproot_stop(&p.root, &p.admin);
    int linux_restart = rx_caproot_start(&p.root, &p.admin);
    int native_restart = aienos_cap_restart(p.native_admin);
    expect_eq("restart", linux_restart, native_restart);
    validate_both(&p, "restart drops old capability", old, 1, 0x10, RX_RIGHT_READ);
    validate_both(&p, "restart drops old office", old_office, 0, 0, RX_RIGHT_MINT);
    pair_stop(&p);
    return 0;
}

static int native_exhaustion(void) {
    AienosCapAdmin *admin = NULL;
    AienosCapView *view = NULL;
    if (aienos_cap_start(&admin, &view) != RX_CAP_OK) {
        fprintf(stderr, "FAIL native exhaustion start\n");
        g_fail++;
        return -1;
    }
    AienosCapRef office;
    aienos_cap_office(admin, &office);
    AienosCapMint mint;
    memset(&mint, 0, sizeof(mint));
    mint.issuer = 3;
    mint.subject = 1;
    mint.resource = 0x10;
    mint.rights = RX_RIGHT_READ;
    mint.parent.cap_id = UINT32_MAX;
    mint.authority = office;
    AienosCapRef cap;
    int rc = aienos_cap_mint(admin, &mint, &cap);
    if (rc != RX_CAP_OK) {
        fprintf(stderr, "FAIL exhaustion mint\n");
        g_fail++;
    }
    rc = aienos_cap_revoke(admin, office, cap);
    if (rc != RX_CAP_OK) {
        fprintf(stderr, "FAIL exhaustion revoke\n");
        g_fail++;
    }
    expect_eq("force exhausted generation", RX_CAP_OK,
              aienos_cap_force_generation(admin, cap.cap_id, UINT64_MAX));
    expect_eq("reclaim refuses generation wrap", RX_CAP_ERR_EXHAUSTED,
              aienos_cap_reclaim(admin, office, cap.cap_id));
    expect_eq("wrapped generation does not validate", RX_CAP_ERR_STALE_GEN,
              aienos_cap_validate(view, cap, 1, 0x10, RX_RIGHT_READ, NULL));
    aienos_cap_stop(admin, view);
    return 0;
}

static int table_full(void) {
    Pair p;
    if (pair_start(&p) != 0) return -1;
    RxCapRef none = { UINT32_MAX, 0 };
    int linux_full = -1;
    int native_full = -1;
    int n = 0;
    for (uint32_t i = 0; i < RX_CAP_MAX + 2; i++) {
        RxCapMint linux_mint;
        AienosCapMint native_mint;
        fill_mint(&p, &linux_mint, &native_mint, 1, 0x70, RX_RIGHT_READ, 0, none);
        linux_mint.authority = p.admin.office;
        AienosCapRef office;
        aienos_cap_office(p.native_admin, &office);
        native_mint.authority = office;
        int linux_rc = rx_capadmin_mint(&p.admin, &linux_mint, NULL);
        int native_rc = aienos_cap_mint(p.native_admin, &native_mint, NULL);
        if (linux_rc != RX_CAP_OK && linux_full < 0) linux_full = linux_rc;
        if (native_rc != RX_CAP_OK && native_full < 0) native_full = native_rc;
        if (linux_rc == RX_CAP_OK && native_rc == RX_CAP_OK) n++;
        if (linux_rc != RX_CAP_OK && native_rc != RX_CAP_OK) break;
    }
    expect_eq("table full", linux_full, native_full);
    if (n != (int)RX_CAP_MAX - 1) {
        fprintf(stderr, "FAIL filled %d slots\n", n);
        g_fail++;
    }
    pair_stop(&p);
    return 0;
}

/* The old defect: a reclaim moved a slot to boot + 1 and the next root also
 * started at boot + 1, so a reference from a stopped root validated against
 * a fresh mint of the same slot in the next one. */
static int restart_generations(void) {
    RxCapRef none = { UINT32_MAX, 0 };
    Pair a;
    if (pair_start(&a) != 0) return -1;
    RxCapRef first, second;
    mint_both(&a, "restart: first", 1, 0x40, RX_RIGHT_READ, 0, none, 0, &first);
    AienosCapRef native_office;
    aienos_cap_office(a.native_admin, &native_office);
    expect_eq("restart: revoke", rx_capadmin_revoke(&a.admin, a.admin.office, first),
              aienos_cap_revoke(a.native_admin, native_office, to_native(&a, first)));
    expect_eq("restart: reclaim", rx_capadmin_reclaim(&a.admin, a.admin.office, first.cap_id),
              aienos_cap_reclaim(a.native_admin, native_office, first.cap_id));
    mint_both(&a, "restart: second", 1, 0x40, RX_RIGHT_READ, 0, none, 0, &second);
    AienosCapRef native_second = to_native(&a, second);
    if (second.cap_id != first.cap_id || second.generation != first.generation + 1) {
        fprintf(stderr, "FAIL restart: reclaim did not reuse the slot one generation up\n");
        g_fail++;
    }
    if (a.linux_base <= UINT32_MAX || a.native_base <= UINT32_MAX) {
        fprintf(stderr, "FAIL restart: boot generation still fits in 32 bits\n");
        g_fail++;
    }
    uint64_t a_linux = a.linux_base, a_native = a.native_base;
    pair_stop(&a);

    Pair b;
    if (pair_start(&b) != 0) return -1;
    if (b.linux_base <= second.generation || b.native_base <= native_second.generation ||
        b.linux_base <= a_linux || b.native_base <= a_native) {
        fprintf(stderr, "FAIL restart: new root did not start above the old one\n");
        g_fail++;
    }
    RxCapRef fresh;
    mint_both(&b, "restart: fresh", 1, 0x40, RX_RIGHT_READ, 0, none, 0, &fresh);
    if (fresh.cap_id != second.cap_id) {
        fprintf(stderr, "FAIL restart: fresh mint used another slot\n");
        g_fail++;
    }
    expect_eq("restart: old ref stale (linux)", RX_CAP_ERR_STALE_GEN,
              rx_caproot_validate(&b.root, second, 1, 0x40, RX_RIGHT_READ, NULL));
    expect_eq("restart: old ref stale (native)", RX_CAP_ERR_STALE_GEN,
              aienos_cap_validate(b.native_view, native_second, 1, 0x40, RX_RIGHT_READ, NULL));
    validate_both(&b, "restart: fresh valid", fresh, 1, 0x40, RX_RIGHT_READ);
    RxCapRef truncated = { fresh.cap_id, (uint32_t)fresh.generation };
    validate_both(&b, "restart: truncated generation refused", truncated, 1, 0x40,
                  RX_RIGHT_READ);
    expect_eq("restart: truncated generation stale", RX_CAP_ERR_STALE_GEN,
              rx_caproot_validate(&b.root, truncated, 1, 0x40, RX_RIGHT_READ, NULL));

    /* In-process restart of the native table: same promise. */
    AienosCapRef native_fresh = to_native(&b, fresh);
    expect_eq("restart: native restart", RX_CAP_OK, aienos_cap_restart(b.native_admin));
    AienosCapRef after;
    aienos_cap_office(b.native_admin, &after);
    if (after.generation <= native_fresh.generation) {
        fprintf(stderr, "FAIL restart: native restart did not move above the old table\n");
        g_fail++;
    }
    expect_eq("restart: pre-restart native ref stale", RX_CAP_ERR_STALE_GEN,
              aienos_cap_validate(b.native_view, native_fresh, 1, 0x40, RX_RIGHT_READ, NULL));
    pair_stop(&b);
    return 0;
}

static AienosCapView *g_view;
static int g_cognition;

static int reaction_tries_to_mint(RxCtx *ctx) {
    (void)ctx;
    AienosCapMint mint;
    memset(&mint, 0, sizeof(mint));
    mint.subject = 1;
    mint.resource = 1;
    mint.rights = RX_RIGHT_MINT | RX_RIGHT_REVOKE | RX_RIGHT_CLOCK | RX_RIGHT_EPOCH;
    mint.parent.cap_id = UINT32_MAX;
    g_cognition = aienos_cap_cognition_mint(g_view, &mint, NULL);
    AienosCapRef none = { UINT32_MAX, 0 };
    int admin = aienos_cap_cognition_admin(g_view, 2, none, none);
    if (g_cognition == RX_CAP_ERR_UNAUTHORIZED && admin == RX_CAP_ERR_UNAUTHORIZED)
        g_cognition = RX_CAP_ERR_UNAUTHORIZED;
    else
        g_cognition = -100;
    return 0;
}

static int world_uses_native(void) {
    AienosCapAdmin *admin = NULL;
    AienosCapView *view = NULL;
    if (aienos_cap_start(&admin, &view) != RX_CAP_OK) {
        fprintf(stderr, "FAIL world authority start\n");
        g_fail++;
        return -1;
    }
    g_view = view;
    RxWorld world;
    if (rx_world_init_native(&world, view, 2, 1u << 12) != RX_OK) {
        fprintf(stderr, "FAIL native world init\n");
        g_fail++;
        aienos_cap_stop(admin, view);
        return -1;
    }
    world.external_subject = 100;
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef sensor;
    if (rx_world_create(&world, 1, RX_PERSIST_RESIDENT, 0x10, init, &sensor) != RX_OK) {
        fprintf(stderr, "FAIL create sensor\n");
        g_fail++;
    }
    AienosCapRef office;
    aienos_cap_office(admin, &office);
    AienosCapMint mint;
    memset(&mint, 0, sizeof(mint));
    mint.issuer = 3;
    mint.subject = 100;
    mint.resource = 0x10;
    mint.rights = RX_RIGHT_WRITE;
    mint.parent.cap_id = UINT32_MAX;
    mint.authority = office;
    AienosCapRef ext;
    if (aienos_cap_mint(admin, &mint, &ext) != RX_CAP_OK) {
        fprintf(stderr, "FAIL external mint\n");
        g_fail++;
    }
    RxCapRef forged = { 200, 7 };
    RxMutation mutation = { sensor, 0, 5 };
    if (rx_world_publish_external(&world, forged, &mutation, 1) != RX_ERR_AUTHORITY) {
        fprintf(stderr, "FAIL world accepted a forged handle\n");
        g_fail++;
    }
    RxObject object;
    rx_world_read(&world, sensor, &object);
    if (object.field[0] != 0) {
        fprintf(stderr, "FAIL forged write changed the object\n");
        g_fail++;
    }
    RxCapRef good = { ext.cap_id, ext.generation };
    if (rx_world_publish_external(&world, good, &mutation, 1) < 0) {
        fprintf(stderr, "FAIL world rejected a good native capability\n");
        g_fail++;
    }
    mint.subject = 1;
    mint.rights = RX_RIGHT_READ;
    mint.resource = 0x10;
    AienosCapRef reader;
    aienos_cap_mint(admin, &mint, &reader);
    mint.resource = 0x20;
    mint.rights = RX_RIGHT_WRITE;
    AienosCapRef writer;
    aienos_cap_mint(admin, &mint, &writer);
    RxObjRef belief;
    uint64_t belief_init[RX_MAX_FIELDS] = { 0 };
    rx_world_create(&world, 1, RX_PERSIST_RESIDENT, 0x20, belief_init, &belief);
    RxReactionDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = "cognition.tries.mint";
    desc.faculty = RX_FACULTY_AIEN;
    desc.subject = 1;
    desc.fn = reaction_tries_to_mint;
    desc.n_triggers = 1;
    desc.triggers[0].obj = sensor;
    desc.triggers[0].mask = RX_FIELD(0);
    desc.n_writes = 1;
    desc.writes[0].obj = belief;
    desc.writes[0].mask = RX_FIELD(0);
    desc.n_caps = 2;
    desc.caps[0].ref = (RxCapRef){ reader.cap_id, reader.generation };
    desc.caps[0].resource = 0x10;
    desc.caps[0].rights = RX_RIGHT_READ;
    desc.caps[1].ref = (RxCapRef){ writer.cap_id, writer.generation };
    desc.caps[1].resource = 0x20;
    desc.caps[1].rights = RX_RIGHT_WRITE;
    uint32_t id = 0;
    if (rx_world_add_reaction(&world, &desc, &id) != RX_OK) {
        fprintf(stderr, "FAIL add cognition reaction\n");
        g_fail++;
    }
    uint64_t clock_before = aienos_cap_clock(view);
    mutation.value = 9;
    if (rx_world_publish_external(&world, good, &mutation, 1) < 0) {
        fprintf(stderr, "FAIL stimulus\n");
        g_fail++;
    }
    rx_world_wait_quiescent(&world, 5000);
    expect_eq("reaction cannot mint or administer", RX_CAP_ERR_UNAUTHORIZED, g_cognition);
    if (aienos_cap_clock(view) != clock_before) {
        fprintf(stderr, "FAIL reaction moved the authority clock\n");
        g_fail++;
    }
    if (aienos_cap_validate(view, office, 0, 0, RX_RIGHT_MINT, NULL) != RX_CAP_OK) {
        fprintf(stderr, "FAIL office changed after cognition\n");
        g_fail++;
    }
    rx_world_destroy(&world);
    aienos_cap_stop(admin, view);
    g_view = NULL;
    return 0;
}

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n);
    fclose(f);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(&out[i * 2], "%02x", d[i]);
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("R7/rx_native_authority_receipt.json", path, sizeof path) != 0)
        return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    const char *aienos = getenv("AIENOS_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    fprintf(f,
            "{\n"
            "  \"schema\": \"AIEN_RX_R7_NATIVE_AUTHORITY_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"aienos_commit\": %s%s%s,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"hardware_scope\": \"host CPU only; Linux mint kept as the oracle; no graphics processor\",\n"
            "  \"gates\": {\n"
            "    \"R7_NATIVE_AUTHORITY\": \"PASS (host; same outcomes as the Linux oracle; oracle retained)\",\n"
            "    \"not_claimed\": [\"R8\", \"R9\", \"R10\", \"R11\", \"R12 silicon\", \"R13\"]\n"
            "  },\n"
            "  \"differential\": [\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "",
            aienos ? aienos : "null", aienos ? "\"" : "", digest, u.sysname, u.release, u.machine);
    for (int i = 0; i < g_ndiff; i++) {
        fprintf(f, "    {\"name\": \"%s\", \"linux\": %d, \"native\": %d}%s\n", g_diff[i].name,
                g_diff[i].code, g_diff[i].code, i + 1 < g_ndiff ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("receipt: %s\n", path);
    printf("candidate bound: %s\n", bound ? "yes" : "no");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    corpus();
    table_full();
    native_exhaustion();
    restart_generations();
    world_uses_native();
    if (g_fail) {
        fprintf(stderr, "%d native-authority mismatches\n", g_fail);
        return 1;
    }
    write_receipt();
    printf("native authority matched the linux oracle, and the world checks the native view\n");
    return 0;
}
