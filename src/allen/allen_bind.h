/* allen_bind.h -- ALLEN inside the resident organism: the three bindings.
 *
 * ARCH-0035 (PROPOSED). ALLEN is the durable subject the AIEN organism
 * sustains. Its state is an AIENOS continuity object (kind 24,
 * aienos native/kernel/svc/continuity_subject.h; OS-0018 PROPOSED). This
 * module is the omega side of that contract and nothing more:
 *
 *   1. identity   : the subject object is read and its id recomputed; the
 *                   World's external subject number is recorded against the
 *                   LogicalAgentId the object carries (the binding record);
 *   2. memory     : the subject's Cortex lineage reference must equal the
 *                   digest of record 1 of the Cortex journal it is run with,
 *                   or nothing is published (fail closed);
 *   3. intent     : each ACTIVE standing intent of kind GOAL_LATENCY is
 *                   turned into the goal mutation the resident AIEN faculty
 *                   already reads (rx_aien.h: goal = {seq, regime, target ns});
 *                   field 3 carries the intent id's first 64 bits so the
 *                   EXTERNAL crumb names the intent it came from.
 *
 * ALLEN publishes state; it never calls a faculty. The World's dependency
 * machinery wakes aien.assess because the goal object changed (ARCH-0016,
 * R16). There is no loop, no wait, no thread, no clock, no capability mint
 * and no scheduling symbol in this translation unit; tests/allen/run.sh
 * checks its symbol table (ALLEN-G5). */
#ifndef ALLEN_BIND_H
#define ALLEN_BIND_H

#include <stddef.h>
#include <stdint.h>

#include "continuity_subject.h"
#include "runtime/rx_cortex.h"
#include "runtime/rx_world.h"

/* Read a subject object file, decode it (every OS-0018 rule) and recompute
 * its logical ObjectId. 0, or -1 with *why set. */
int allen_load(const char *path, struct cs_subject *s, uint8_t id[32], const char **why);

/* Cortex lineage reference: the digest of journal record 1. 0 on success;
 * -1 when the journal holds no record (unbound). */
int allen_lineage(const CxStore *cx, uint8_t out[32]);

/* Fail-closed memory binding: 0 when the subject's lineage reference equals
 * the journal's; -1 (with *why) when the subject is unbound, the journal is
 * empty, or they differ. */
int allen_check_memory(const struct cs_subject *s, const CxStore *cx, const char **why);

/* The identity binding record (v0, host table of one row). */
typedef struct {
    uint32_t external_subject; /* RxWorld.external_subject */
    uint8_t agent[32];         /* LogicalAgentId from the subject object */
    uint8_t subject_id[32];    /* ObjectId of the subject object */
    uint64_t sequence;         /* subject chain sequence */
} AllenBinding;
void allen_bind_identity(const RxWorld *w, const struct cs_subject *s, const uint8_t id[32],
                         AllenBinding *out);

/* The goal mutation for one ACTIVE GOAL_LATENCY intent. `seq` is the goal
 * object's next sequence number (the writer's monotone counter). Returns the
 * number of mutations written into out (4), or 0 if the intent is not an
 * active goal-latency intent. */
uint32_t allen_goal_mutations(const struct cs_intent *a, RxObjRef goal, uint64_t seq,
                              RxMutation out[4]);

/* Hex helpers for the tool and the gate script. */
void allen_hex(const uint8_t *b, size_t n, char *out);
int allen_unhex(const char *s, uint8_t *out, size_t n);

#endif
