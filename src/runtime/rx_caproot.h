/*
 * rx_caproot.h -- host reference capability root for the resident reaction
 * runtime (ADR 0016, Part II §3; gate R7 is NOT claimed by this file).
 *
 * The rule: the shared world may hold references to capabilities, it may
 * never invent them. Here that rule is enforced by the operating system, not by
 * convention:
 *
 *   - The capability table lives in a sealed memfd. The only writable mapping
 *     belongs to a separate, non-dumpable root process (the mint).
 *   - The reaction runtime receives a read-only mapping. The memfd carries
 *     F_SEAL_FUTURE_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL, so no
 *     new writable mapping, write(2), or resize can be obtained from any fd.
 *   - Minting, delegation (attenuation only), revocation, slot reclaim, lease
 *     clock and epoch changes are requests over a socket; the root enforces
 *     every rule and the runtime only observes the result.
 *
 * A capability binds: id, slot generation, issuer, subject, resource, rights,
 * epoch, lease expiry, parent (delegation), delegable flag, revocation state.
 * A CapabilityRef in the world is only {cap_id, generation}; validation reads
 * the root's table, so a forged, stale, revoked, expired, wrong-subject,
 * wrong-resource or amplified reference fails.
 *
 * Scope: this is the Linux-hosted development root. On AIENOS the kernel
 * capability authority is the root (ADR 0014); authenticating WHICH principal
 * may ask for a fresh mint (AEGIS policy identity) is R7/R8 work.
 */
#ifndef RX_CAPROOT_H
#define RX_CAPROOT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define RX_CAP_MAX            256u
#define RX_CAP_MAX_DEPTH      8u
#define RX_CAP_TABLE_MAGIC    0x50414358u /* 'XCAP' LE */

/* Rights */
#define RX_RIGHT_READ         0x1u
#define RX_RIGHT_WRITE        0x2u
#define RX_RIGHT_EFFECT       0x4u
#define RX_RIGHT_DELEGATE     0x8u

/* Slot state */
#define RX_CAP_FREE           0u
#define RX_CAP_LIVE           1u
#define RX_CAP_REVOKED        2u

/* Validation / request results */
#define RX_CAP_OK                   0
#define RX_CAP_ERR_BOUNDS          -1
#define RX_CAP_ERR_STALE_GEN       -2
#define RX_CAP_ERR_REVOKED         -3
#define RX_CAP_ERR_EPOCH           -4
#define RX_CAP_ERR_SUBJECT         -5
#define RX_CAP_ERR_RESOURCE        -6
#define RX_CAP_ERR_RIGHTS          -7
#define RX_CAP_ERR_EXPIRED         -8
#define RX_CAP_ERR_CHAIN           -9
#define RX_CAP_ERR_AMPLIFY        -10
#define RX_CAP_ERR_NOT_DELEGABLE  -11
#define RX_CAP_ERR_FULL           -12
#define RX_CAP_ERR_IO             -13
#define RX_CAP_ERR_STATE          -14

typedef struct {
    uint32_t cap_id;
    uint32_t generation;
} RxCapRef;

typedef struct {
    uint32_t cap_id;
    uint32_t generation;
    uint32_t state;
    uint32_t issuer;
    uint32_t subject;
    uint32_t rights;
    uint64_t resource;
    uint64_t epoch;
    uint64_t lease_expiry;      /* logical clock tick; 0 = no lease */
    uint32_t parent_id;         /* UINT32_MAX = root-issued */
    uint32_t parent_generation;
} RxCapEntry;

/* Shared table. Written only by the root process under a seqlock. */
typedef struct {
    uint32_t magic;
    uint32_t capacity;
    _Atomic uint64_t seq;       /* odd while the root is writing */
    _Atomic uint64_t epoch;
    _Atomic uint64_t clock;
    RxCapEntry entries[RX_CAP_MAX];
} RxCapTable;

/* Handle held by the runtime process. */
typedef struct {
    const RxCapTable *table;    /* read-only mapping */
    int ctl_fd;                 /* request socket to the root */
    int ro_fd;                  /* sealed memfd, kept for attack tests */
    pid_t root_pid;
    bool running;
} RxCapRoot;

/* Mint request. parent.cap_id == UINT32_MAX means a root-issued capability
 * (AEGIS policy decision); otherwise it is a delegation that may only
 * attenuate rights and must keep the parent's resource. */
typedef struct {
    uint32_t issuer;
    uint32_t subject;
    uint64_t resource;
    uint32_t rights;
    uint64_t lease_ticks;       /* 0 = no lease */
    RxCapRef parent;
} RxCapMint;

int  rx_caproot_start(RxCapRoot *root);
void rx_caproot_stop(RxCapRoot *root);

int  rx_caproot_mint(RxCapRoot *root, const RxCapMint *req, RxCapRef *out);
int  rx_caproot_revoke(RxCapRoot *root, RxCapRef ref);
/* Return a revoked slot to the free pool; its generation advances, so every
 * outstanding reference to the old occupant becomes stale. */
int  rx_caproot_reclaim(RxCapRoot *root, uint32_t cap_id);
int  rx_caproot_advance_clock(RxCapRoot *root, uint64_t ticks);
int  rx_caproot_bump_epoch(RxCapRoot *root);

/* Validate a reference for (subject, resource, rights). Reads the root table
 * with a seqlock and walks the delegation chain; any revoked ancestor fails.
 * On success, *out_entry (optional) receives the entry snapshot. */
int  rx_caproot_validate(const RxCapRoot *root, RxCapRef ref,
                         uint32_t subject, uint64_t resource, uint32_t rights,
                         RxCapEntry *out_entry);

const char *rx_cap_strerror(int code);

#endif /* RX_CAPROOT_H */
