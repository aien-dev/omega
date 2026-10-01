/*
 * fabric.h -- AIEN Fabric interface F5-0: membership, capability
 * advertisement, leases and loss detection as typed records keyed by
 * AienMachineId (aien_machine_id.h), over an abstract transport.
 *
 * Scope (F5-0). Everything here runs in one process over the loopback
 * transport (fab_loopback.h). No socket, no real network, no placement, no
 * topology/latency measurement, no failure recovery beyond withdrawing a lost
 * machine's capabilities. A real transport (AIENOS M6) plugs in through
 * FabTransport and a real per-machine signature (TRUST-1 owner key) through
 * FabAuth; nothing else changes.
 *
 * Who owns what.
 *   Roster        Which machines may be members at all. Supplied by the caller
 *                 from the AEGIS / owner-key boundary; the Fabric only reads it
 *                 and has no call that adds to it.
 *   Membership    This node's view of each enrolled peer: generation, last
 *                 sequence, lease, state. One writer: fab_poll / fab_tick.
 *   Capabilities  The canonical Capability Graph (rx_capq.h), touched only
 *                 through its public calls: cq_wire_apply to ingest a peer's
 *                 record, cq_machine_advertise_id to set the peer's lease,
 *                 cq_withdraw when the peer is lost or leaves. The Fabric
 *                 never grants authority: an entry's auth_* is a requirement,
 *                 and whether it is held is decided by the World's authority
 *                 view (CqHeld), never here. The build fails if fabric.o
 *                 references an authority or World operation (mk/fabric.mk).
 *
 * Message (FAB_HDR_BYTES + body + FAB_SIG_BYTES; integers little-endian):
 *
 *   off len field
 *     0   4 magic "AFAB"
 *     4   1 version 0x02 (0x01 had a 32-byte tag; refused as FAB_E_FORMAT)
 *     5   1 kind        FAB_MSG_*
 *     6   2 body_len    must equal the kind's body size
 *     8  44 sender      AienMachineId record
 *    52  44 dest        AienMachineId record (the one receiver it is for)
 *    96   8 generation  sender's membership generation, >= 1, rises on rejoin
 *   104   8 seq         sender's sequence within that generation, >= 1, rising
 *   112   8 sent_us     sender's clock (informational; covered by the sig)
 *   120   n body
 *   120+n 64 sig        FabAuth signature over bytes 0 .. 120+n-1, by the sender.
 *                     Fixed width 64 = an Ed25519 signature (RFC 8032), so the
 *                     TRUST-1 owner-key signer fits without a format change.
 *
 * Bodies:
 *   JOIN       40: ontology digest (cq_ontology_digest, 32) + requested lease (u64 us)
 *   RENEW       8: requested lease (u64 us)
 *   ADVERTISE 188: one rx_capq wire record (CQ_WIRE_BYTES) of any capq kind
 *                 (advertise, availability, withdraw); its machine must be the
 *                 sender, so a machine changes or withdraws only its own entries
 *   LEAVE       0
 *
 * Receive rules, in order (first failure is the verdict; nothing changes
 * except that an authenticated message of the held generation that passed the
 * REPLAY check consumes its seq even when refused later, so a refused message
 * can never succeed on replay):
 *   form                        FAB_E_FORMAT
 *   dest != self, sender = self FAB_E_MISMATCH
 *   sender not on the roster    FAB_E_NOT_ENROLLED
 *   sig does not verify         FAB_E_AUTH      (forged identity, altered bytes)
 *   generation < held           FAB_E_STALE_GEN
 *   same generation, seq <= last FAB_E_REPLAY
 *   JOIN with generation <= held FAB_E_STALE_GEN
 *   non-JOIN, not a member, or a
 *     generation it never joined with FAB_E_NOT_MEMBER
 *   non-JOIN after the lease ended FAB_E_LEASE_EXPIRED (must rejoin, new generation)
 *   JOIN with another ontology  FAB_E_ONTOLOGY
 *   ADVERTISE naming another machine FAB_E_MISMATCH (nobody speaks for a peer)
 *   ADVERTISE the graph refuses FAB_E_CAPQ (capq verdict in FabVerdict.capq)
 * Before any check the node applies fab_tick(now), so loss is a function of
 * the clock only and replays are deterministic.
 *
 * Leases. The receiver grants min(requested, FabConfig.max_lease_us) from its
 * own clock and hands the end to the graph (cq_machine_advertise_id). When
 * now >= lease end the member is LOST: every entry it advertised is withdrawn.
 * A lost member comes back only by a JOIN with a higher generation.
 *
 * Generations never move back: a JOIN is accepted only with a generation
 * above the one held, and anything older is FAB_E_STALE_GEN, so old traffic
 * still in flight after a restart cannot roll membership back.
 * Limits (F5-0): the held generation and seq live in memory only, so a
 * receiver restart forgets them (persistence comes with the TRUST-1 / M6
 * identity work), and a JOIN of a new generation refused before admission
 * (ontology, capacity) is not remembered.

 * Capability generations. rx_capq accepts a record only when its generation
 * is newer than the one held. Advertisers number entries with
 * fab_entry_generation(node, rev) = generation << 32 | rev, so a rejoin with a
 * new generation always outranks anything from before.
 */
#ifndef FABRIC_H
#define FABRIC_H

#include "runtime/aien_machine_id.h"
#include "runtime/rx_capq.h"
#include "runtime/rx_jspace.h"

#include <stddef.h>
#include <stdint.h>

#define FAB_VERSION         0x02u
#define FAB_HDR_BYTES       120u
#define FAB_SIG_BYTES       64u     /* Ed25519-sized signature, fixed width */
#define FAB_JOIN_BODY       40u
#define FAB_RENEW_BODY      8u
#define FAB_ADVERTISE_BODY  CQ_WIRE_BYTES
#define FAB_MSG_MAX         (FAB_HDR_BYTES + FAB_ADVERTISE_BODY + FAB_SIG_BYTES)

#define FAB_MAX_MEMBERS     16u     /* peers one node tracks */
#define FAB_MAX_KEYS        64u     /* capability entries one peer may advertise */
#define FAB_EVENT_RING      256u

enum { FAB_MSG_JOIN = 1, FAB_MSG_RENEW = 2, FAB_MSG_ADVERTISE = 3, FAB_MSG_LEAVE = 4 };

/* Member states in this node's view. */
enum { FAB_ST_NONE = 0, FAB_ST_JOINED = 1, FAB_ST_LOST = 2, FAB_ST_LEFT = 3 };

#define FAB_OK                0
#define FAB_E_ARG            -1
#define FAB_E_FORMAT         -2
#define FAB_E_MISMATCH       -3
#define FAB_E_NOT_ENROLLED   -4
#define FAB_E_AUTH           -5
#define FAB_E_STALE_GEN      -6
#define FAB_E_REPLAY         -7
#define FAB_E_LEASE_EXPIRED  -8
#define FAB_E_NOT_MEMBER     -9
#define FAB_E_ONTOLOGY      -10
#define FAB_E_CAPQ          -11
#define FAB_E_FULL          -12
#define FAB_E_TRANSPORT     -13

/* ---- what a real transport and a real signer must provide ---- */

/* Moves opaque messages between machines. send: 0 or FAB_E_TRANSPORT.
 * recv: 1 with a message in buf, 0 when none is waiting, <0 on error. The
 * Fabric authenticates every message itself; the transport is not trusted. */
typedef struct {
    void *ctx;
    int (*send)(void *ctx, const AienMachineId *from, const AienMachineId *to,
                const uint8_t *msg, size_t len);
    int (*recv)(void *ctx, const AienMachineId *self, uint8_t *buf, size_t cap, size_t *len);
} FabTransport;

/* Signs as this node; verifies a signature as coming from `claimed`. 0 = ok.
 * The signature is always FAB_SIG_BYTES (64) bytes; a signer with a shorter
 * native output must still define every byte (the stand-in derives 64). */
typedef struct {
    void *ctx;
    int (*sign)(void *ctx, const uint8_t *msg, size_t len, uint8_t sig[FAB_SIG_BYTES]);
    int (*verify)(void *ctx, const AienMachineId *claimed, const uint8_t *msg, size_t len,
                  const uint8_t sig[FAB_SIG_BYTES]);
} FabAuth;

/* Enrolled machines (from the AEGIS / owner-key boundary). Read-only here. */
typedef struct {
    uint32_t n;
    const AienMachineId *ids;
} FabRoster;

typedef struct {
    AienMachineId self;
    const FabRoster *roster;
    CqCatalog *catalog;             /* canonical catalog (cq_catalog_init_canonical) */
    const FabTransport *transport;
    const FabAuth *auth;
    uint64_t max_lease_us;          /* longest lease this node grants */
    uint64_t generation;            /* this node's membership generation, >= 1 */
} FabConfig;

/* ---- typed records ---- */

typedef struct {
    AienMachineId machine;
    uint32_t state;                 /* FAB_ST_* */
    uint32_t index;                 /* rx_capq machine index (0 = never joined) */
    uint64_t generation;
    uint64_t last_seq;
    uint64_t lease_until_us;
    uint32_t n_keys;                /* entries it advertised and holds live or withdrawn */
    CqKey keys[FAB_MAX_KEYS];
} FabMember;

enum { FAB_EV_JOINED = 1, FAB_EV_RENEWED, FAB_EV_ADVERTISED, FAB_EV_LOST, FAB_EV_LEFT,
       FAB_EV_REFUSED };

typedef struct {
    uint32_t kind;                  /* FAB_EV_* */
    int32_t code;                   /* FAB_OK or the refusal */
    int32_t capq;                   /* rx_capq verdict for ADVERTISE, else 0 */
    uint32_t msg_kind;              /* FAB_MSG_* that caused it, 0 for a tick */
    AienMachineId machine;          /* the peer it is about (zero id if unparsable) */
    uint64_t generation, seq, at_us;
} FabEvent;

typedef struct {
    int code;                       /* FAB_OK or FAB_E_* */
    int capq;                       /* rx_capq verdict (ADVERTISE), else 0 */
    uint32_t msg_kind;
    AienMachineId sender;
} FabVerdict;

typedef struct {
    FabConfig cfg;
    uint64_t seq;                   /* last sequence sent in cfg.generation */
    uint32_t n_members;
    FabMember members[FAB_MAX_MEMBERS];
    uint64_t n_events;
    FabEvent events[FAB_EVENT_RING];
    sha256_ctx event_hash;          /* every event, in order */
    uint64_t counts[16];            /* refusals by -code */
} FabNode;

/* ---- node ---- */

int  fab_node_init(FabNode *n, const FabConfig *cfg);
/* Start a new membership generation (after a restart): must be higher. */
int  fab_node_set_generation(FabNode *n, uint64_t generation);
uint64_t fab_entry_generation(const FabNode *n, uint32_t rev);

/* Seal a message as this node (next sequence) into out (FAB_MSG_MAX). */
int  fab_seal(FabNode *n, const AienMachineId *dest, uint32_t kind, const uint8_t *body,
              size_t body_len, uint64_t now_us, uint8_t *out, size_t *out_len);

/* Send to every other roster machine. */
int  fab_join(FabNode *n, uint64_t lease_us, uint64_t now_us);
int  fab_renew(FabNode *n, uint64_t lease_us, uint64_t now_us);
int  fab_leave(FabNode *n, uint64_t now_us);
/* Advertise one of this node's own entries (capq wire kind CQ_WIRE_*). */
int  fab_advertise(FabNode *n, const CqKey *own, uint32_t wire_kind, uint64_t now_us);

/* Apply the clock: members whose lease ended become LOST and their entries are
 * withdrawn. Returns how many became lost. */
int  fab_tick(FabNode *n, uint64_t now_us);
/* Process one raw message (normally from the transport). */
int  fab_receive(FabNode *n, const uint8_t *msg, size_t len, uint64_t now_us, FabVerdict *v);
/* Receive and process one waiting message: 1 processed, 0 none, <0 transport. */
int  fab_poll(FabNode *n, uint64_t now_us, FabVerdict *v);

/* ---- views ---- */

const FabMember *fab_member(const FabNode *n, const AienMachineId *m);
/* 1 when m is a JOINED member whose lease is live at now_us. */
int  fab_member_live(const FabNode *n, const AienMachineId *m, uint64_t now_us);
/* J-Space placement for m: LOCAL for self, REMOTE_OWNED for a live member;
 * FAB_E_NOT_MEMBER otherwise (J-Space must not place on a lost machine). */
int  fab_home(const FabNode *n, const AienMachineId *m, uint64_t now_us, JsHome *out);
/* Digest of the membership view and the event history (replay check). */
void fab_state_digest(const FabNode *n, uint8_t out[32]);
const char *fab_strerror(int code);

#endif /* FABRIC_H */
