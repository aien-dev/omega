/*
 * rx_operator_authority_misuse.c -- the R16 operator control capability
 * against the locked native AIENOS authority (aienos#266, aienos#272).
 *
 * The operator's capability is minted as the R13 program mints it
 * (tests/runtime/rx_r13_living.c: RX_OPERATOR_SUBJ, RX_WORLD_RES_CONTROL,
 * RX_WORLD_RIGHT_HALT, issued by the office). It is the epoch right on the
 * control resource (docs/r16-operator-control.md section 6.1). This test
 * proves, by execution against $(AIENOS_CAP_LIB):
 *   (a) the authority's office still mints, revokes, reclaims, advances the
 *       clock and bumps the epoch;
 *   (b) the control capability, and one carrying every privileged right on
 *       the control resource, are refused by every authority operation with
 *       RX_CAP_ERR_RESOURCE, and nothing changes;
 *   (c) the world still accepts the control capability for the halt check
 *       (rx_world_validate_cap on RX_WORLD_RES_CONTROL with
 *       RX_WORLD_RIGHT_HALT), the check stop/status/resume run through.
 * Against an aienos.lock without the resource check (bbad5e4) part (b) fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/runtime/aienos_cap.h"
#include "../../src/runtime/rx_caproot.h"
#include "../../src/runtime/rx_operator.h"
#include "../../src/runtime/rx_world.h"

static int g_fail, g_pass;

static void expect(const char *what, int got, int want) {
    if (got == want) {
        g_pass++;
        printf("ok   %s (%d)\n", what, got);
    } else {
        g_fail++;
        printf("FAIL %s: got %d, want %d\n", what, got, want);
    }
}

static AienosCapRef office_of(AienosCapAdmin *admin) {
    AienosCapRef o = {UINT32_MAX, 0};
    aienos_cap_office(admin, &o);
    return o;
}

static int mint(AienosCapAdmin *admin, uint32_t subject, uint64_t resource, uint32_t rights,
                AienosCapRef *out) {
    AienosCapMint m = {3, subject, resource, rights, 0, {UINT32_MAX, 0}, office_of(admin)};
    return aienos_cap_mint(admin, &m, out);
}

static RxCapRef rx(AienosCapRef r) { return (RxCapRef){r.cap_id, r.generation}; }

int main(void) {
    AienosCapAdmin *admin = NULL;
    AienosCapView *view = NULL;
    if (aienos_cap_start(&admin, &view) != RX_CAP_OK) {
        printf("FAIL native authority start\n");
        return 1;
    }
    RxWorld world;
    if (rx_world_init_native(&world, view, 2, 1u << 12) != RX_OK) {
        printf("FAIL native world init\n");
        aienos_cap_stop(admin, view);
        return 1;
    }
    const uint32_t all_priv = RX_RIGHT_MINT | RX_RIGHT_REVOKE | RX_RIGHT_RECLAIM |
                              RX_RIGHT_EPOCH | RX_RIGHT_CLOCK;
    AienosCapRef halt, wide, victim, victim2, minted;
    expect("mint operator control capability (as R13)",
           mint(admin, RX_OPERATOR_SUBJ, RX_WORLD_RES_CONTROL, RX_WORLD_RIGHT_HALT, &halt), RX_CAP_OK);
    expect("mint every privileged right on the control resource",
           mint(admin, RX_OPERATOR_SUBJ, RX_WORLD_RES_CONTROL, all_priv, &wide), RX_CAP_OK);
    expect("mint a victim", mint(admin, 2, 0x77, RX_RIGHT_READ, &victim), RX_CAP_OK);

    /* (b) misuse: the control capability operates no authority entry point.
     * The halt cap holds only the epoch right, so its epoch bump is the hole
     * aienos#266 named (refused on the resource since aienos#272); its other
     * operations lack the right and stay refused as before. The wide cap holds
     * every right each operation needs, so every refusal is the resource. */
    expect("halt cap: bump epoch refused on the resource", aienos_cap_bump_epoch(admin, halt),
           RX_CAP_ERR_RESOURCE);
    expect("halt cap: advance clock refused (no clock right)",
           aienos_cap_advance_clock(admin, halt, 1), RX_CAP_ERR_UNAUTHORIZED);
    expect("halt cap: revoke refused (no revoke right)", aienos_cap_revoke(admin, halt, victim),
           RX_CAP_ERR_UNAUTHORIZED);
    AienosCapMint mh = {3, 2, 0x78, RX_RIGHT_READ, 0, {UINT32_MAX, 0}, halt};
    expect("halt cap: mint refused (no mint right)", aienos_cap_mint(admin, &mh, &minted),
           RX_CAP_ERR_UNAUTHORIZED);
    expect("wide control cap: bump epoch refused on the resource",
           aienos_cap_bump_epoch(admin, wide), RX_CAP_ERR_RESOURCE);
    expect("wide control cap: advance clock refused on the resource",
           aienos_cap_advance_clock(admin, wide, 1), RX_CAP_ERR_RESOURCE);
    expect("wide control cap: revoke refused on the resource",
           aienos_cap_revoke(admin, wide, victim), RX_CAP_ERR_RESOURCE);
    expect("wide control cap: reclaim refused on the resource",
           aienos_cap_reclaim(admin, wide, victim.cap_id), RX_CAP_ERR_RESOURCE);
    AienosCapMint mw = {3, 2, 0x78, RX_RIGHT_READ, 0, {UINT32_MAX, 0}, wide};
    expect("wide control cap: mint refused on the resource", aienos_cap_mint(admin, &mw, &minted),
           RX_CAP_ERR_RESOURCE);
    /* Nothing moved: the victim and both control capabilities are still live
     * at the same epoch (a bump would have staled every one of them). */
    expect("victim still valid", rx_world_validate_cap(&world, rx(victim), 2, 0x77, RX_RIGHT_READ, NULL),
           RX_CAP_OK);

    /* (c) the halt check the operator commands run through still passes. */
    expect("world accepts control cap for the halt check",
           rx_world_validate_cap(&world, rx(halt), RX_OPERATOR_SUBJ, RX_WORLD_RES_CONTROL,
                                 RX_WORLD_RIGHT_HALT, NULL), RX_CAP_OK);
    expect("world refuses control cap on another resource",
           rx_world_validate_cap(&world, rx(halt), RX_OPERATOR_SUBJ, 0x77, RX_WORLD_RIGHT_HALT, NULL),
           RX_CAP_ERR_RESOURCE);

    /* (a) legitimate authority operations by the office still succeed. */
    AienosCapRef office = office_of(admin);
    expect("office advances the clock", aienos_cap_advance_clock(admin, office, 1), RX_CAP_OK);
    expect("office revokes", aienos_cap_revoke(admin, office, victim), RX_CAP_OK);
    expect("office reclaims", aienos_cap_reclaim(admin, office, victim.cap_id), RX_CAP_OK);
    expect("office mints", mint(admin, 2, 0x79, RX_RIGHT_READ, &victim2), RX_CAP_OK);
    AienosCapRef clock_cap;
    expect("authority-resource clock cap minted",
           mint(admin, RX_OPERATOR_SUBJ, 0 /* AIENOS_CAP_RES_AUTHORITY */, RX_RIGHT_CLOCK, &clock_cap),
           RX_CAP_OK);
    expect("authority-resource clock cap advances the clock",
           aienos_cap_advance_clock(admin, clock_cap, 1), RX_CAP_OK);
    expect("office bumps the epoch", aienos_cap_bump_epoch(admin, office), RX_CAP_OK);
    expect("control cap stale after the office's epoch bump",
           rx_world_validate_cap(&world, rx(halt), RX_OPERATOR_SUBJ, RX_WORLD_RES_CONTROL,
                                 RX_WORLD_RIGHT_HALT, NULL), RX_CAP_ERR_EPOCH);

    rx_world_destroy(&world);
    aienos_cap_stop(admin, view);
    printf("rx_operator_authority_misuse: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) return 1;
    printf("OMEGA_OPERATOR_AUTHORITY_MISUSE_REFUSED PASS\n");
    return 0;
}
