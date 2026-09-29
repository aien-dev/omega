/*
 * aienos_cap.h -- C view of the native AIENOS capability authority.
 *
 * The view can check a reference. The admin can change the table. The
 * reaction world is given the view. Cognition has no path to the admin.
 * The Linux mint process is a separate oracle and is not started here.
 * Layout matches aienos native/capability/aienos_capability.h: generations
 * are 64 bits.
 */
#ifndef AIENOS_CAP_H
#define AIENOS_CAP_H

#include <stdint.h>

typedef struct AienosCapAdmin AienosCapAdmin;
typedef struct AienosCapView AienosCapView;

typedef struct {
    uint32_t cap_id;
    uint64_t generation;
} AienosCapRef;

typedef struct {
    uint32_t cap_id;
    uint64_t generation;
    uint32_t state;
    uint32_t issuer;
    uint32_t subject;
    uint32_t rights;
    uint64_t resource;
    uint64_t epoch;
    uint64_t lease_expiry;
    uint32_t parent_id;
    uint64_t parent_generation;
    uint32_t minted_by_id;
    uint64_t minted_by_generation;
} AienosCapEntry;

typedef struct {
    uint32_t issuer;
    uint32_t subject;
    uint64_t resource;
    uint32_t rights;
    uint64_t lease_ticks;
    AienosCapRef parent;
    AienosCapRef authority;
} AienosCapMint;

int aienos_cap_start(AienosCapAdmin **admin, AienosCapView **view);
void aienos_cap_stop(AienosCapAdmin *admin, AienosCapView *view);
int aienos_cap_office(const AienosCapAdmin *admin, AienosCapRef *out);
int aienos_cap_mint(AienosCapAdmin *admin, const AienosCapMint *request, AienosCapRef *out);
int aienos_cap_revoke(AienosCapAdmin *admin, AienosCapRef authority, AienosCapRef target);
int aienos_cap_reclaim(AienosCapAdmin *admin, AienosCapRef authority, uint32_t cap_id);
int aienos_cap_advance_clock(AienosCapAdmin *admin, AienosCapRef authority, uint64_t ticks);
int aienos_cap_bump_epoch(AienosCapAdmin *admin, AienosCapRef authority);
int aienos_cap_kill(AienosCapAdmin *admin);
int aienos_cap_restart(AienosCapAdmin *admin);
int aienos_cap_validate(const AienosCapView *view, AienosCapRef cap, uint32_t subject,
                       uint64_t resource, uint32_t rights, AienosCapEntry *out);
int aienos_cap_inspect(const AienosCapView *view, AienosCapRef cap, AienosCapEntry *out);
uint64_t aienos_cap_clock(const AienosCapView *view);
int aienos_cap_cognition_mint(const AienosCapView *view, const AienosCapMint *request,
                             const uint8_t *token);
int aienos_cap_cognition_admin(const AienosCapView *view, uint32_t op, AienosCapRef authority,
                              AienosCapRef target);
int aienos_cap_force_generation(AienosCapAdmin *admin, uint32_t cap_id, uint64_t generation);
int aienos_cap_generation_advance(uint64_t generation, uint64_t *out);

/* Authority observer (aienos native/capability, 12add16, on aienos main from
 * d39dd5b). Set on the admin handle; the view has no path to it and
 * aienos_cap_validate never calls it. One call per state change, with a copy
 * of the entry after the change, made after the table lock is released:
 *   MINT new entry (refused: entry NULL + code); REVOKE target, then each
 *   descendant, ancestors first; RECLAIM freed slot; EPOCH/CLOCK the acting
 *   authority's entry; KILL entry NULL; RESTART the new office entry.
 * VALIDATE_DENIED is reserved and never sent. With no observer set, each admin
 * operation pays one NULL check. */
#define AIENOS_CAP_OBS_MINT 1u
#define AIENOS_CAP_OBS_REVOKE 2u
#define AIENOS_CAP_OBS_RECLAIM 3u
#define AIENOS_CAP_OBS_EPOCH 4u
#define AIENOS_CAP_OBS_CLOCK 5u
#define AIENOS_CAP_OBS_KILL 6u
#define AIENOS_CAP_OBS_RESTART 7u
#define AIENOS_CAP_OBS_VALIDATE_DENIED 8u

typedef void (*AienosCapObserver)(void *ctx, uint32_t op, const AienosCapEntry *entry,
                                  int result);

int aienos_cap_set_observer(AienosCapAdmin *admin, AienosCapObserver fn, void *ctx);

/* Office token check, constant time (not called by the library itself). */
int aienos_cap_authorize(const AienosCapAdmin *admin, const uint8_t *presented);

#endif
