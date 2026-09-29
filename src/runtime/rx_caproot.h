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
 * Scope: this is the Linux-hosted development root. It is not the permanent
 * root and it is not R7. On AIENOS the kernel capability authority is the root
 * (ADR 0014). This host root only refuses to treat the control socket as
 * authority:
 *   - mint, revoke, reclaim, epoch, clock and shutdown each require a
 *     capability this mint previously handed to this socket, carrying the
 *     matching right;
 *   - every such request must also present the office token delivered once at
 *     start. That token is not stored in the readable table, so reading the
 *     table and speaking on the socket is not enough;
 *   - revoke reaches a target only when the target's privileged rights are
 *     already held by the presented authority, and it marks descendants
 *     revoked (AIENOS-style cascade);
 *   - privileged rights cannot be delegated;
 *   - counter wrap and a slot whose generation is exhausted fail closed;
 *   - a new root starts above every generation an earlier root in this
 *     process could have reached (see take_boot_gen).
 */

#ifndef RX_CAPROOT_H
#define RX_CAPROOT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define RX_CAP_MAX            256u
#define RX_CAP_MAX_DEPTH      8u
#define RX_CAP_TOKEN_LEN      32u
#define RX_CAP_TABLE_MAGIC    0x50414358u /* 'XCAP' LE */

/* Control operations. The socket carries these; it does not authorize them. */
enum {
    RX_OP_MINT = 1,
    RX_OP_REVOKE,
    RX_OP_RECLAIM,
    RX_OP_CLOCK,
    RX_OP_EPOCH,
    RX_OP_SHUTDOWN
};

/* Rights */
#define RX_RIGHT_READ         0x1u
#define RX_RIGHT_WRITE        0x2u
#define RX_RIGHT_EFFECT       0x4u
#define RX_RIGHT_DELEGATE     0x8u
/* Privileged rights. Never delegable. A root-issued cap may carry a subset of
 * the presented authority's privileged rights, and never DELEGATE as well. */
#define RX_RIGHT_MINT         0x10u
#define RX_RIGHT_REVOKE       0x20u
#define RX_RIGHT_RECLAIM      0x40u
#define RX_RIGHT_EPOCH        0x80u
#define RX_RIGHT_CLOCK        0x100u
/* Lineage transition. The proposer of a candidate does not hold this.
 * It cannot be combined with DELEGATE or passed on to a child. */
#define RX_RIGHT_PROMOTE      0x200u
#define RX_RIGHT_PRIVILEGED   (RX_RIGHT_MINT | RX_RIGHT_REVOKE | RX_RIGHT_RECLAIM | \
                               RX_RIGHT_EPOCH | RX_RIGHT_CLOCK | RX_RIGHT_PROMOTE)
#define RX_RIGHT_KNOWN        (RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT | \
                               RX_RIGHT_DELEGATE | RX_RIGHT_PRIVILEGED)

/* Resource id of the bootstrap authority office. Not a world object. */
#define RX_CAP_RES_AUTHORITY  0ull

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
#define RX_CAP_ERR_UNAUTHORIZED   -15
#define RX_CAP_ERR_OVERFLOW       -16
#define RX_CAP_ERR_EXHAUSTED      -17

typedef struct {
    uint32_t cap_id;
    uint64_t generation;
} RxCapRef;

typedef struct {
    uint32_t cap_id;
    uint64_t generation;
    uint32_t state;
    uint32_t issuer;
    uint32_t subject;
    uint32_t rights;
    uint64_t resource;
    uint64_t epoch;
    uint64_t lease_expiry;      /* logical clock tick; 0 = no lease */
    uint32_t parent_id;         /* UINT32_MAX = root-issued */
    uint64_t parent_generation;
    uint32_t minted_by_id;      /* capability that was allowed to create this one */
    uint64_t minted_by_generation;
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

/* Runtime validation handle. It intentionally contains no mint token, office
 * capability, or control socket. Passing this to RxWorld cannot confer
 * administrative authority. */
typedef struct {
    const RxCapTable *table;    /* read-only mapping */
    int ro_fd;                  /* sealed memfd, kept for attack tests */
    pid_t root_pid;
    bool running;
} RxCapRoot;

/* Privileged host-reference administration handle. Policy/setup code may hold
 * this; the resident reaction world must not. This split is the host analogue
 * of keeping intelligence separate from the authority root. */
typedef struct {
    int ctl_fd;
    pid_t root_pid;
    bool running;
    RxCapRef office;
    uint8_t token[RX_CAP_TOKEN_LEN];
} RxCapAdmin;

/* Mint request. parent.cap_id == UINT32_MAX means a root-issued capability.
 * authority must be a capability this mint handed to this socket:
 *   root-issued: authority carries MINT
 *   delegation:  authority is the parent itself
 * The socket is transport. Holding it does not mint anything. */
typedef struct {
    uint32_t issuer;
    uint32_t subject;
    uint64_t resource;
    uint32_t rights;
    uint64_t lease_ticks;       /* 0 = no lease */
    RxCapRef parent;
    RxCapRef authority;
} RxCapMint;

/* Wire request. Tests may forge one. The library fills `token` from the
 * secret delivered at start; a caller who only read the table cannot. */
typedef struct {
    uint32_t op;
    uint32_t pad;
    RxCapMint mint;
    RxCapRef ref;
    RxCapRef authority;
    uint64_t arg;
    uint8_t token[RX_CAP_TOKEN_LEN];
} RxCapRequest;

/* Fail closed. A sum that would wrap is refused and *out is left unchanged. */
static inline int rx_cap_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return RX_CAP_ERR_OVERFLOW;
    if (out) *out = a + b;
    return RX_CAP_OK;
}

/* Match AIENOS: generations are 64 bits and a slot whose generation is
 * already UINT64_MAX is retired. The old handle is not revived by wrapping. */
static inline int rx_cap_generation_advance(uint64_t generation, uint64_t *out) {
    if (generation == UINT64_MAX) return RX_CAP_ERR_EXHAUSTED;
    if (out) *out = generation + 1u;
    return RX_CAP_OK;
}

int  rx_caproot_start(RxCapRoot *root, RxCapAdmin *admin);
void rx_caproot_stop(RxCapRoot *root, RxCapAdmin *admin);
RxCapRef rx_capadmin_office(const RxCapAdmin *admin);

int  rx_capadmin_mint(RxCapAdmin *admin, const RxCapMint *req, RxCapRef *out);
int  rx_capadmin_revoke(RxCapAdmin *admin, RxCapRef authority, RxCapRef ref);
/* Return a revoked slot to the free pool; its generation advances, so every
 * outstanding reference to the old occupant becomes stale. Generation
 * UINT32_MAX refuses and leaves the slot revoked. */
int  rx_capadmin_reclaim(RxCapAdmin *admin, RxCapRef authority, uint32_t cap_id);
int  rx_capadmin_advance_clock(RxCapAdmin *admin, RxCapRef authority, uint64_t ticks);
int  rx_capadmin_bump_epoch(RxCapAdmin *admin, RxCapRef authority);

/* Copy one table entry if the reference generation matches. Does not grant
 * rights. Used to record who issued a capability into a causal crumb. */
int  rx_caproot_inspect(const RxCapRoot *root, RxCapRef ref, RxCapEntry *out);

/* Validate a reference for (subject, resource, rights). Reads the root table
 * with a seqlock and walks the delegation chain; any revoked ancestor fails.
 * On success, *out_entry (optional) receives the entry snapshot. */
int  rx_caproot_validate(const RxCapRoot *root, RxCapRef ref,
                         uint32_t subject, uint64_t resource, uint32_t rights,
                         RxCapEntry *out_entry);

const char *rx_cap_strerror(int code);

#endif /* RX_CAPROOT_H */
