/*
 * fab_loopback.h -- in-process loopback transport and HMAC authenticator for
 * Fabric F5-0 (fabric.h). Test and simulation only.
 *
 * Transport: one FIFO mailbox per registered machine. Delivery order is send
 * order, so a scenario replays bit for bit. Every send is folded into a
 * transcript digest. Hooks let a test copy the last message, inject raw
 * bytes (replay, forgery, misdelivery) and silence a machine.
 *
 * Authenticator: HMAC-SHA256 with one 32-byte key per machine, stretched to
 * the 64-byte FAB_SIG_BYTES field (fab_hmac_sig64: two domain-separated
 * HMAC-SHA256 halves). This is a
 * stand-in: a verifier holding a symmetric key could also sign with it, so it
 * proves only that the sender knew the key. The real transport replaces it
 * with the machine's owner-key signature (TRUST-1) behind the same FabAuth.
 */
#ifndef FAB_LOOPBACK_H
#define FAB_LOOPBACK_H

#include "fabric.h"

#define FAB_LOOP_NODES   8u
#define FAB_LOOP_DEPTH   64u

typedef struct {
    uint32_t len;
    uint8_t b[FAB_MSG_MAX];
} FabLoopMsg;

typedef struct {
    AienMachineId id;
    int silent;                     /* 1: messages it sends are dropped */
    uint32_t head, count;
    FabLoopMsg q[FAB_LOOP_DEPTH];
} FabLoopBox;

typedef struct {
    uint32_t n;
    FabLoopBox box[FAB_LOOP_NODES];
    sha256_ctx transcript;
    uint64_t sent, dropped;
    FabLoopMsg last;                /* copy of the last message sent (or dropped) */
    FabTransport transport;         /* points back at this loop */
} FabLoop;

void fab_loop_init(FabLoop *l);
int  fab_loop_add(FabLoop *l, const AienMachineId *id);
void fab_loop_silence(FabLoop *l, const AienMachineId *id, int silent);
/* Put raw bytes in `to`'s mailbox, bypassing the sender (attack hook). */
int  fab_loop_inject(FabLoop *l, const AienMachineId *to, const uint8_t *msg, size_t len);
uint32_t fab_loop_pending(const FabLoop *l);
void fab_loop_transcript(const FabLoop *l, uint8_t out[32]);

typedef struct {
    AienMachineId self;
    uint8_t self_key[32];
    uint32_t n;
    const AienMachineId *ids;
    const uint8_t (*keys)[32];      /* keys[i] belongs to ids[i] */
    FabAuth auth;                   /* points back at this authenticator */
} FabHmacAuth;

/* Peer tables hold at most FAB_HMAC_MAX_PEERS entries; ids and keys must each
 * have n elements. Returns FAB_OK, or FAB_E_ARG (nothing usable) when an
 * argument is NULL or n is 0 or above the cap. */
#define FAB_HMAC_MAX_PEERS 64
int fab_hmac_auth_init(FabHmacAuth *a, const AienMachineId *self, const uint8_t self_key[32],
                        uint32_t n, const AienMachineId *ids, const uint8_t (*keys)[32]);

#endif
