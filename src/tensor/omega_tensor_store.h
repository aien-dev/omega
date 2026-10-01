#ifndef OMEGA_TENSOR_STORE_H
#define OMEGA_TENSOR_STORE_H

/*
 * M20 OMEGA_TENSOR crash-safe storage lifetime (docs/tensor/M20_OMEGA_TENSOR.md,
 * row "Crash-safe storage lifetime"). Owned by src/tensor; does not link or
 * call src/runtime (the commit pattern is copied from rx_generation.c).
 *
 * A store is a directory that keeps generation-checked tensor storage across
 * process exits and crashes:
 *   <dir>/<value-id-hex>.bin  content-addressed payload: the tensor's elements
 *                             in logical row-major order, dtype width,
 *                             little-endian, every NaN stored as the canonical
 *                             quiet NaN (NaN payload is not semantic in M20, so
 *                             a value id names exactly one byte string).
 *   <dir>/journal             header + fixed-size records (PUT / RELEASE),
 *                             each with slot, generation, sequence number,
 *                             dtype, shape, value id and payload SHA-256, and
 *                             its own SHA-256 checksum.
 *
 * Commit (put and release): the file is written to a temp name, fsynced,
 * renamed over the final name, then the directory is fsynced. A put commits
 * its payload first and its journal record second; the journal rename is the
 * commit point. A crash anywhere leaves either the old or the new state.
 *
 * Recovery (open) replays the journal and refuses, with a typed error and
 * without returning any data, when:
 *   - the journal is shorter or longer than its header says, a record or the
 *     header fails its checksum, or a record does not follow from the state
 *     before it (wrong sequence, wrong generation, release of a free slot,
 *     put on a live slot, slot or shape out of range): OMEGA_TSTORE_ERR_TORN;
 *   - a live record's payload is missing, short, long, or does not hash to
 *     the recorded payload SHA-256 and value id: OMEGA_TSTORE_ERR_CORRUPT;
 *   - the journal is missing while payload files exist (a lost journal would
 *     reset generations): OMEGA_TSTORE_ERR_TORN.
 * Leftover temp files and payloads no live record refers to are removed.
 *
 * Generations: every slot starts at generation 1. A release bumps it, and the
 * bump is journaled, so after any recovery a released handle stays released:
 * its generation never comes back and it gets OMEGA_TENSOR_ERR_STALE. A slot
 * whose generation reaches UINT64_MAX is retired, never wrapped.
 *
 * One opener at a time (flock on the directory; a second open gets
 * OMEGA_TSTORE_ERR_BUSY). Not thread safe. The journal grows by one record
 * per put/release and is rewritten whole on each commit (no compaction yet).
 * Not covered: rollback of the whole directory to an older valid copy.
 *
 * Status codes: OMEGA_TENSOR_* from omega_tensor.h (OK, BAD_ARGS, STALE,
 * CAPACITY, and any code of the tensor layer) plus the codes below.
 */

#include <stdbool.h>
#include <stdint.h>

#include "omega_tensor.h"

#define OMEGA_TSTORE_ERR_IO      (-20)  /* a file operation failed            */
#define OMEGA_TSTORE_ERR_TORN    (-21)  /* journal short/torn/inconsistent    */
#define OMEGA_TSTORE_ERR_CORRUPT (-22)  /* payload missing or hash mismatch   */
#define OMEGA_TSTORE_ERR_BUSY    (-23)  /* store already open elsewhere       */

#define OMEGA_TSTORE_MAX_CAPACITY (1u << 20)

typedef struct OmegaTensorStore OmegaTensorStore;

/* Opens <dir> (created if absent, with `capacity` slots), or recovers an
 * existing store, whose recorded capacity must equal `capacity`. */
int  omega_tensor_store_open(const char *dir, uint32_t capacity, OmegaTensorStore **out);
void omega_tensor_store_close(OmegaTensorStore *st);

/* Persists the value of t (any tensor or view of ctx) into a free slot and
 * returns its handle once the commit is durable. */
int omega_tensor_store_put(OmegaTensorStore *st, const OmegaTensorCtx *ctx, OmegaTensor t,
                           OmegaStorageHandle *out);
/* Loads the stored value into a new tensor of ctx. The payload hash and value
 * id are checked again on every load. Stale handle: OMEGA_TENSOR_ERR_STALE. */
int omega_tensor_store_get(OmegaTensorStore *st, OmegaStorageHandle h, OmegaTensorCtx *ctx,
                           OmegaTensor *out);
/* Releases the slot durably; every handle of it turns stale. Releasing twice,
 * or through a stale handle, returns OMEGA_TENSOR_ERR_STALE and changes nothing. */
int  omega_tensor_store_release(OmegaTensorStore *st, OmegaStorageHandle h);
bool omega_tensor_store_valid(const OmegaTensorStore *st, OmegaStorageHandle h);
int  omega_tensor_store_value_id(const OmegaTensorStore *st, OmegaStorageHandle h, uint8_t id[32]);
uint32_t omega_tensor_store_live_count(const OmegaTensorStore *st);

/* Crash points, in commit order. A put passes 1..8, a release 5..8. */
enum {
    OMEGA_TSTORE_CRASH_NONE = 0,
    OMEGA_TSTORE_CRASH_PAYLOAD_PARTIAL,   /* half the payload temp written      */
    OMEGA_TSTORE_CRASH_PAYLOAD_WRITTEN,   /* payload temp fsynced, not renamed  */
    OMEGA_TSTORE_CRASH_PAYLOAD_RENAMED,   /* renamed, directory not fsynced     */
    OMEGA_TSTORE_CRASH_PAYLOAD_SYNCED,    /* payload durable, journal untouched */
    OMEGA_TSTORE_CRASH_JOURNAL_PARTIAL,   /* half the journal temp written      */
    OMEGA_TSTORE_CRASH_JOURNAL_WRITTEN,   /* journal temp fsynced, not renamed  */
    OMEGA_TSTORE_CRASH_JOURNAL_RENAMED,   /* commit point passed                */
    OMEGA_TSTORE_CRASH_JOURNAL_SYNCED,    /* directory fsynced                  */
    OMEGA_TSTORE_CRASH_COUNT
};

#ifdef OMEGA_TENSOR_TEST_HOOKS
/* Test-only: the next commit that reaches `step` calls _exit(86) there.
 * Compiled only with OMEGA_TENSOR_TEST_HOOKS; test-tensor-no-hooks checks with
 * nm that the default build has neither this symbol nor a reference to _exit. */
int omega_tensor_store_test_set_crash_step(OmegaTensorStore *st, int step);
#endif

#endif /* OMEGA_TENSOR_STORE_H */
