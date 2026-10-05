/*
 * rx_caller.h -- runtime-issued caller credentials (R16 clarification C5).
 *
 * A subject number (the principal a reaction or a promotion names) is a
 * plain value any caller can write. On its own it proves nothing. A world
 * that has bound its callers (rx_world_bind_callers) accepts a subject only
 * together with the credential the runtime issued for it:
 *
 *   - rx_world_enroll_caller mints one per subject, before the world is
 *     bound: a fresh 32-byte secret from getrandom(2) and a generation.
 *     The secret is handed back once, to the enroller. The world keeps only
 *     SHA-256(domain, subject, generation, secret), never the secret.
 *   - Every authority-bearing call checks it: reaction registration (and
 *     again at each activation and commit: the enrollment must still be
 *     live at the same generation), R9 proposal and promotion (a store bound
 *     with rx_gen_bind_authority).
 *   - Revocation (rx_world_revoke_caller, which needs the credential itself)
 *     retires the enrollment; the old credential never validates again.
 *     Enrollment closes when the world is bound, so a revoked subject stays
 *     revoked for that world's life.
 *
 * Generation 0 is never issued: an all-zero credential is "absent".
 * The limit that remains is the process boundary: code in the same address
 * space can read any memory, including a credential. R16-G3 shows the
 * production binary does not link or execute legacy code at all.
 */
#ifndef RX_CALLER_H
#define RX_CALLER_H

#include <stdint.h>

#define RX_CALLER_SECRET_LEN 32u
#define RX_CALLER_MAX        64u
#define RX_CALLER_KEYRING_MAX 8u

typedef struct {
    uint64_t generation;                      /* 0: absent */
    uint8_t secret[RX_CALLER_SECRET_LEN];
} RxCallerCred;

/* Why a credential was refused (rx_world_check_caller). At the authority
 * boundary every one of these is RX_ERR_IDENTITY (world) or
 * RX_GEN_ERR_IDENTITY (R9). */
#define RX_CALLER_OK            0
#define RX_CALLER_ERR_ABSENT   -1   /* no credential presented */
#define RX_CALLER_ERR_UNKNOWN  -2   /* subject never enrolled in this world */
#define RX_CALLER_ERR_REVOKED  -3   /* enrollment revoked */
#define RX_CALLER_ERR_STALE    -4   /* credential of another generation */
#define RX_CALLER_ERR_FORGED   -5   /* secret does not match this subject */
#define RX_CALLER_ERR_CLOSED   -6   /* enrollment closed (world bound) */
#define RX_CALLER_ERR_EXISTS   -7   /* subject already enrolled */
#define RX_CALLER_ERR_ENTROPY  -8   /* getrandom failed */
#define RX_CALLER_ERR_FULL     -9   /* enrollment table full (RX_CALLER_MAX) */
#define RX_CALLER_ERR_HALTED  -10   /* operator emergency stop in force (rx_world_caller_check_fn):
                                       RX_GEN_ERR_HALTED at the R9 boundary, not an identity fault */

/* What a caller check does (RxGenCallerFn, rx_world_caller_check_fn).
 * HOLD is the full check that, when it passes, keeps the enrollment table
 * locked so no revocation can land until the matching RELEASE: the R9
 * store holds it across the durable flip of the active pointer (R16 C7). */
#define RX_CALLER_OP_CHECK   0
#define RX_CALLER_OP_HOLD    1
#define RX_CALLER_OP_RELEASE 2

/* The credentials one component holds, looked up by subject when it
 * registers a reaction or calls R9. Held by the component, never published
 * into the world. */
typedef struct {
    uint32_t n;
    uint32_t subject[RX_CALLER_KEYRING_MAX];
    RxCallerCred cred[RX_CALLER_KEYRING_MAX];
} RxCallerKeyring;

static inline const RxCallerCred *rx_caller_find(const RxCallerKeyring *k, uint32_t subject) {
    if (!k) return 0;
    for (uint32_t i = 0; i < k->n && i < RX_CALLER_KEYRING_MAX; i++)
        if (k->subject[i] == subject) return &k->cred[i];
    return 0;
}

static inline void rx_caller_wipe(RxCallerCred *c) {
    volatile uint8_t *p = (volatile uint8_t *)c;
    for (unsigned i = 0; i < sizeof *c; i++) p[i] = 0;
}

#endif
