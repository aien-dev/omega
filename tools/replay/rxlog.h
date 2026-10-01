/* rxlog.h -- RXCLOG01: a portable file form of a World causal crumb log
 * (src/runtime/rx_world.h RxCrumb), plus the outside inputs that drove the
 * run and state checkpoints, so a run can be verified and replayed by a
 * program that does not link the runtime.
 *
 * This header and rxlog.c depend on libc and src/sha256.h only. They never
 * include rx_world.h: the verifier is an independent implementation of the
 * crumb digest (rx_world.c crumb_hash, "AIEN_RX_CAUSAL_V1") and of the
 * rx_world_verify_crumbs rules. The exporter (rx_crumb_export.c) is the only
 * file that sees both layouts, and it static-asserts the limits match.
 *
 * File layout, all integers little-endian:
 *   header  "RXCLOG01" | u32 version (1) | u32 flags
 *   record* u32 type | u32 payload_len | payload
 *   the last record is END (type 0xFF): u64 n_records | 32-byte head
 * Payload encodings are fixed by rxl_encode(); counts bound every array and
 * a payload must be exactly as long as its counts imply.
 *
 * Compared digest (what "the same run" means):
 *   CRUMB       the crumb digest, recomputed from the fields the runtime
 *               hashes: id, kind, reaction, faculty, wake_cause,
 *               coalesced_wakes, inputs, caps (+issuer), outputs, reason,
 *               parents and the parents' digests.
 *               NOT compared: worker, t_start_ns, t_end_ns (where and when a
 *               reaction ran is placement and timing, not causal identity;
 *               the runtime itself leaves them out so the same history
 *               hashes the same on any schedule), and episode (derived from
 *               wake_cause, so it is checked by rule, not hashed).
 *               Capability generations (crumb caps and the INPUT capability)
 *               are stored relative to the run's capability office
 *               generation: the root seeds generations from boot time, so
 *               raw ones never repeat across runs. Stored crumb digests are
 *               over that relative form; the exporter first checks each
 *               runtime digest against rxl_crumb_digest over the raw fields
 *               (rx_crumb_export.h).
 *   INPUT       SHA-256 of the encoded payload (capability + mutations with
 *               their values: the crumb log alone does not carry values).
 *   CHECKPOINT  SHA-256 of the encoded payload: the state hash of every
 *               object and, since the causal-order compare, the state table
 *               it was hashed from (object id, generation, type, version and
 *               per field value, field version and writer crumb id). The
 *               verifier recomputes the state hash from the table, so the
 *               table cannot differ from what the recorder hashed.
 * END.head chains the compared digests of every record before it, so a
 * dropped tail or a forged middle is caught without trusting the file. */
#ifndef OMEGA_RXLOG_H
#define OMEGA_RXLOG_H

#include <stddef.h>
#include <stdint.h>

#define RXL_MAGIC "RXCLOG01"
#define RXL_VERSION 1u
#define RXL_FLAG_INPUTS 0x1u   /* every EXTERNAL crumb is preceded by its INPUT record */

#define RXL_MAX_DEPS 8u
#define RXL_MAX_WRITES 8u
#define RXL_MAX_CAPS 8u
#define RXL_MAX_PARENTS 65u    /* RX_MAX_DEPS * RX_MAX_FIELDS + 1 */
#define RXL_MAX_MUTS 16u
#define RXL_MAX_PAYLOAD 4096u
#define RXL_MAX_FIELDS 8u     /* RX_MAX_FIELDS (static-asserted by the recorder) */
#define RXL_MAX_OBJS 16u

/* Crumb kinds, as numbered in rx_world.h RxCrumbKind. */
enum { RXL_K_CREATE = 1, RXL_K_EXTERNAL = 2, RXL_K_COMMIT = 3, RXL_K_INVALIDATED = 4, RXL_K_MAX = 10 };

enum { RXL_CRUMB = 1, RXL_INPUT = 2, RXL_CHECKPOINT = 3, RXL_END = 0xFF };

typedef struct { uint32_t id, gen; uint64_t version, mask; } rxl_io;
typedef struct { uint32_t cap_id; uint64_t gen; uint32_t issuer; } rxl_cap;

typedef struct {
    uint64_t id;
    uint32_t kind, reaction, faculty, worker;
    uint64_t wake_cause, coalesced;
    uint32_t n_inputs;
    rxl_io inputs[RXL_MAX_DEPS];
    uint32_t n_caps;
    rxl_cap caps[RXL_MAX_CAPS];
    uint32_t n_outputs;
    rxl_io outputs[RXL_MAX_WRITES];
    int32_t reason;
    uint32_t n_parents;
    uint64_t parents[RXL_MAX_PARENTS];
    uint64_t t_start, t_end, episode;
    uint8_t digest[32];
} rxl_crumb;

typedef struct { uint32_t id, gen, field; uint64_t value; } rxl_mut;
typedef struct {
    uint64_t after_crumb;      /* crumbs in the log when the input was published */
    uint32_t cap_id;
    uint64_t cap_gen;
    uint32_t n;
    rxl_mut m[RXL_MAX_MUTS];
} rxl_input;

/* One object of a CHECKPOINT state table, in the order the state hash
 * covers it. writer[f] is the crumb id of the field's last writer (0 = none). */
typedef struct {
    uint32_t id, gen, type;
    uint64_t version;
    uint64_t value[RXL_MAX_FIELDS], fversion[RXL_MAX_FIELDS], writer[RXL_MAX_FIELDS];
} rxl_obj;

typedef struct {
    uint64_t through_crumb;
    uint32_t subsystem;        /* 1 = world object state */
    uint8_t hash[32];
    uint32_t n_obj;            /* 0 = hash only (no state table) */
    rxl_obj obj[RXL_MAX_OBJS];
} rxl_checkpoint;

typedef struct { uint64_t n_records; uint8_t head[32]; } rxl_end;

typedef struct {
    uint32_t type;
    union { rxl_crumb c; rxl_input in; rxl_checkpoint ck; rxl_end end; } u;
} rxl_rec;

typedef struct {
    uint32_t version, flags;
    rxl_rec *recs;
    size_t n, cap;
    /* Set by rxl_read when the file stops being well formed: index of the
     * record that could not be read (== n), and why. */
    int malformed;
    char why[128];
} rxl_log;

void rxl_init(rxl_log *l, uint32_t flags);
void rxl_free(rxl_log *l);
int  rxl_push(rxl_log *l, const rxl_rec *r);
/* Read a file. Returns 0 if the header is readable (records may still be
 * malformed: see l->malformed), -1 if the file or header is unusable. */
int  rxl_read(const char *path, rxl_log *l, char *err, size_t errlen);
int  rxl_write(const char *path, const rxl_log *l);
/* Encode one record payload; returns its length (0 for an unknown type). */
size_t rxl_encode(const rxl_rec *r, uint8_t *buf);
/* Crumb digest exactly as rx_world.c crumb_hash. digest_by_id[i] is the
 * digest of crumb id i+1, for the n_known crumbs before k. */
void rxl_crumb_digest(const rxl_crumb *k, const uint8_t (*digest_by_id)[32], uint64_t n_known,
                      uint8_t out[32]);
/* Compared digest of a non-END record (crumb: its stored digest). */
void rxl_event_digest(const rxl_rec *r, uint8_t out[32]);
/* One step of the END head chain. */
void rxl_head_step(uint8_t head[32], const rxl_rec *r);
/* Append the END record over the records as they are (stored digests are
 * used, not recomputed): what the exporter writes. */
int  rxl_finish(rxl_log *l);
/* Recompute every crumb digest, episode and the END record in place: the
 * mutation tool uses it to build forged logs that are internally consistent. */
int  rxl_seal(rxl_log *l);
void rxl_hex(const uint8_t *d, size_t n, char *out);
/* State hash of a CHECKPOINT table, exactly as the recorder hashes the
 * objects (tests/replay/rx_world_replay.c checkpoint()). */
void rxl_state_hash(const rxl_checkpoint *ck, uint8_t out[32]);

#endif
