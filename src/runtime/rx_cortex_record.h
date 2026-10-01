/*
 * rx_cortex_record.h -- World execution recorded into canonical Cortex (M20).
 *
 * The one Cortex writer for World. Attached to a world, it turns every
 * causal crumb, as the World commits it, into one Cortex object:
 *
 *   EXTERNAL                         -> OBSERVATION / CX_K_WORK_ACCEPTED
 *   CREATE, RETIRE                   -> ENTITY / CX_K_ENTITY_CREATED, _RETIRED
 *   COMMIT                           -> EXECUTION / CX_K_EXEC_COMMIT (the result)
 *   NOOP                             -> EXECUTION / CX_K_EXEC_NOOP
 *   FAILED, REJECTED, BLOCKED_AUTHORITY,
 *   INVALIDATED, QUARANTINE          -> FAILURE / CX_K_EXEC_FAILED
 *
 * Payload layout CX_WREC_* (rx_cortex.h): session, crumb id and digest
 * (World evidence reference), reaction, faculty, episode, the object and
 * its field values after the crumb (execution observation / result).
 * Provenance: links[0] is the record of the crumb that woke this work,
 * links[1..3] records of its parent crumbs (writers of what it read).
 *
 * Subject = World object slot + 1 (0 = no object). Slots are runtime
 * indices, not identity. Logical time t = the store's next id, so it keeps
 * increasing across sessions on one journal.
 *
 * Attaching claims the store as its single writer: no other code may append
 * to it while attached (cx_append returns CX_ERR_WRITER), and a store held by
 * another writer cannot be attached.
 */
#ifndef RX_CORTEX_RECORD_H
#define RX_CORTEX_RECORD_H

#include "rx_cortex.h"
#include "rx_world.h"

#define RX_CORTEX_SUBJECTS ((uint64_t)RX_MAX_OBJECTS + 1u)

/* Attach `s` (n_subjects >= RX_CORTEX_SUBJECTS) to `w` under `session`.
 * Crumbs already in the log are recorded first (their field values are the
 * objects' values at attach time, their digests the crumbs'). Returns RX_OK,
 * RX_ERR_EXISTS (world already has a recorder), RX_ERR_IDENTITY (store held
 * by another writer), RX_ERR_ARG or RX_ERR_FULL (no memory). */
int rx_cortex_attach(RxWorld *w, CxStore *s, uint64_t session);

/* Detach and release the store's writer claim. Also done by
 * rx_world_destroy. */
int rx_cortex_detach(RxWorld *w);

/* Records written and append failures (a failure never fails the commit;
 * it is counted here and the crumb stays in the World log). */
int rx_cortex_status(RxWorld *w, uint64_t *records, uint64_t *errors);

/* Cortex id of the record for crumb `crumb` of the attached session, 0 if none. */
uint64_t rx_cortex_record_of(RxWorld *w, uint64_t crumb);

/* Newest committed result for an object slot: Cortex id (0 = none). */
uint64_t rx_cortex_recall_result(CxStore *s, uint32_t obj_slot, CxWorldRecord *out);

/* COMPOSITION-2: append a composition record (candidate, evidence,
 * admission) through the attached single writer, under the world mutex.
 * RX_ERR_NOT_FOUND when no store is attached; RX_ERR_FULL when Cortex
 * refused it (counted in errors). */
int rx_cortex_append(RxWorld *w, const CxHeader *h, const uint64_t *payload, uint32_t n,
                     uint64_t *out_id);
/* COMPOSITION-2: cx_promote through the attached writer (RX_ERR_ARG when
 * Cortex refuses the pair). The only way a candidate becomes fact. */
int rx_cortex_promote(RxWorld *w, uint64_t candidate, uint64_t evidence, uint64_t *out_id);
/* Logical time the next attached append gets (0 = not attached). */
uint64_t rx_cortex_next_t(RxWorld *w);

#endif /* RX_CORTEX_RECORD_H */
