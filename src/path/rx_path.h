/* BOOTSTRAP / REFERENCE: PATH-1 native identity and construction
 * (spec/path-semantic-object.md, SPEC-OMEGA-PATH-M16, sections 4 to 7 and 15).
 * Host C, test harness only. Not linked into any runtime, physics or GPU target.
 *
 * Classification (spec 17.1): this header and rx_path.c are BOOTSTRAP; the
 * canonical encoder is the REFERENCE oracle. Native route: lower these records
 * to native Omega memory frames and check the native encoder byte for byte
 * against this oracle.
 *
 * A PATH IS EVIDENCE, NEVER AUTHORITY (spec 8.2, invariant I1). This module
 * builds, encodes, decodes and names paths. It has no function that executes,
 * dispatches, promotes, authorizes or validates a capability. Recording a
 * capability step (family 0x0004) grants nothing.
 *
 * Memory: caller-allocated fixed storage only. No heap, no threads, no system
 * calls. Steps live in a caller-owned append-only step arena; paths refer to
 * arena slots by index. A forked child stores its parent reference, the
 * divergence index K and only its own tail steps (spec 7.1).
 *
 * Byte order: all multi-byte integers are big-endian (spec 6).
 */
#ifndef RX_PATH_H
#define RX_PATH_H

#include "omega_types.h"
#include <stddef.h>
#include <stdint.h>

/* Canonical header (spec 6.1). KIND_PATH is not yet in omega_types.h: ADR-0021
 * (spec 16) must ratify it first, so it is defined here only. */
#define RX_PATH_VERSION 0x01u
#define RX_PATH_KIND 0x0Cu
#define RX_PATH_HEADER_BYTES 0x6Au

/* Identity domain tags (spec 5.1, 5.3). Each is hashed followed by one 0x00. */
#define RX_PATH_SEMANTIC_TAG "omega.path.v1"
#define RX_PATH_REALIZATION_TAG "omega.path.realization.v1"

/* Architectural limits (spec 6.3). */
#define RX_PATH_MAX_STEPS 512u
#define RX_PATH_MAX_INPUTS_PER_STEP 8u
#define RX_PATH_MAX_OUTPUTS_PER_STEP 8u
#define RX_PATH_MAX_PARAM_LEN 1024u
#define RX_PATH_MAX_ATTR_COUNT 32u
#define RX_PATH_MAX_CONSTRAINT_COUNT 16u
#define RX_PATH_MAX_TOTAL_SERIALIZATION 65536u

/* Bootstrap storage bounds (not in the spec). Attribute and constraint storage
 * reuse OmegaAttribute and OmegaConstraint from omega_types.h, so a key holds
 * at most OMEGA_MAX_KEY_LEN - 1 bytes, a value at most OMEGA_MAX_VAL_LEN bytes
 * and a constraint payload at most 128 bytes. */
#define RX_PATH_MAX_KEY_BYTES (OMEGA_MAX_KEY_LEN - 1)
#define RX_PATH_MAX_CONSTRAINT_PAYLOAD 128u
#define RX_PATH_ARENA_STEPS 2048u
#define RX_PATH_MAX_FORK_DEPTH 64u

/* Error codes named by spec 6.3. */
#define RX_PATH_OK 0
#define RX_PATH_ERR_STEP_LIMIT -80
#define RX_PATH_ERR_INPUT_LIMIT -81
#define RX_PATH_ERR_OUTPUT_LIMIT -82
#define RX_PATH_ERR_PARAM_LIMIT -83
#define RX_PATH_ERR_ATTR_LIMIT -84
#define RX_PATH_ERR_CONST_LIMIT -85
#define RX_PATH_ERR_BUFFER_OVERFLOW -86
/* Additional refusal codes. The spec names none for these cases. */
#define RX_PATH_ERR_MALFORMED -87   /* non-canonical or structurally invalid */
#define RX_PATH_ERR_ID_MISMATCH -88 /* recomputed identity differs from expected */
#define RX_PATH_ERR_FROZEN -89      /* path is a fork parent; spec 7.1 rule 3 */
#define RX_PATH_ERR_ARG -90         /* null pointer or out of range argument */
#define RX_PATH_ERR_CAPACITY -91    /* bootstrap arena or fork depth exhausted */

/* Step families (spec 4.1). */
#define STEP_FAM_RELATION 0x0001u
#define STEP_FAM_EXECUTION 0x0002u
#define STEP_FAM_JSPACE 0x0003u
#define STEP_FAM_CAPABILITY 0x0004u
#define STEP_FAM_DISCOVERY 0x0005u

/* Step roles (spec 4.1). */
#define ROLE_REL_ENTITY 0x0101u
#define ROLE_REL_CLAIM 0x0102u
#define ROLE_REL_EVIDENCE 0x0103u
#define ROLE_REL_INFERENCE 0x0104u
#define ROLE_EXEC_GOAL 0x0201u
#define ROLE_EXEC_OPERATION 0x0202u
#define ROLE_EXEC_VERIFICATION 0x0203u
#define ROLE_EXEC_RESULT 0x0204u
#define ROLE_JS_WORLD 0x0301u
#define ROLE_JS_FORK 0x0302u
#define ROLE_JS_PROPOSAL 0x0303u
#define ROLE_JS_ACTION 0x0304u
#define ROLE_JS_OBSERVATION 0x0305u
#define ROLE_JS_VERIFIER 0x0306u
#define ROLE_JS_CANDIDATE 0x0307u
#define ROLE_CAP_INTENT 0x0401u
#define ROLE_CAP_OPERATION 0x0402u
#define ROLE_CAP_REQUIREMENT 0x0403u
#define ROLE_CAP_IMPLEMENTATION 0x0404u
#define ROLE_CAP_MACHINE 0x0405u
#define ROLE_CAP_AUTH_VERIFY 0x0406u
#define ROLE_CAP_EFFECT 0x0407u
#define ROLE_DISC_OBSERVATION 0x0501u
#define ROLE_DISC_HYPOTHESIS 0x0502u
#define ROLE_DISC_EXPERIMENT 0x0503u
#define ROLE_DISC_MEASUREMENT 0x0504u
#define ROLE_DISC_UPDATE 0x0505u
#define ROLE_DISC_EXPLANATION 0x0506u

/* One step (spec 6.2). step_index is not stored: it is the position in the
 * logical sequence and is written by the encoder, checked by the decoder. */
typedef struct {
    uint16_t step_family;
    uint16_t step_role;
    uint16_t input_count;
    uint16_t output_count;
    SemanticId input_ids[RX_PATH_MAX_INPUTS_PER_STEP];
    SemanticId operator_id;
    SemanticId output_ids[RX_PATH_MAX_OUTPUTS_PER_STEP];
    uint32_t param_len;
    uint8_t param_bytes[RX_PATH_MAX_PARAM_LEN];
} RxPathStep;

/* Append-only step arena. A stored step is never modified. */
typedef struct {
    uint32_t used;
    RxPathStep steps[RX_PATH_ARENA_STEPS];
} RxPathStepArena;

/* A semantic path. Attributes are kept sorted by key and constraints by
 * (kind, payload) at insertion time, so builder call order never reaches the
 * encoding (spec 5.2 rule 3). A child made by rx_path_fork reads its logical
 * steps 0..K-1 through parent and holds only steps K..N-1 in tail. */
typedef struct RxPath {
    RxPathStepArena *arena;
    const struct RxPath *parent;
    SemanticId parent_path_id;
    uint16_t divergence_step_index;
    uint16_t tail_count;
    uint32_t tail[RX_PATH_MAX_STEPS];
    SemanticId start_anchor_id;
    SemanticId end_anchor_id;
    SemanticId context_id;
    uint16_t attr_count;
    uint16_t constraint_count;
    OmegaAttribute attributes[RX_PATH_MAX_ATTR_COUNT];
    OmegaConstraint constraints[RX_PATH_MAX_CONSTRAINT_COUNT];
    uint8_t frozen;
} RxPath;

/* Physical realization inputs (spec 5.3). The semantic path id is not stored
 * here: rx_path_realization_id derives it from the path. schedule_kind is an
 * opaque u16; the spec names SEQUENTIAL and PRELOAD but gives no values. */
typedef struct {
    SemanticId machine_id;
    SemanticId engine_profile_id;
    SemanticId code_digest;
    uint16_t schedule_kind;
    SemanticId argus_observation_digest;
} RxPathRealization;

void rx_path_arena_init(RxPathStepArena *arena);

/* Construction. Every mutator refuses a frozen path with RX_PATH_ERR_FROZEN
 * and leaves the path and arena unchanged on any failure. */
int rx_path_init(RxPath *p, RxPathStepArena *arena, const SemanticId *start_anchor,
                 const SemanticId *end_anchor, const SemanticId *context);
int rx_path_set_end_anchor(RxPath *p, const SemanticId *end_anchor);
int rx_path_add_attribute(RxPath *p, const char *key, const uint8_t *value,
                          uint16_t value_len);
int rx_path_add_constraint(RxPath *p, uint16_t kind, const uint8_t *payload,
                           uint16_t payload_len);
int rx_path_append(RxPath *p, const RxPathStep *step);

/* Fork at divergence step K (0 <= K <= parent step count). The parent is frozen
 * from this point on. The child copies the parent's anchors, attributes and
 * constraints, shares steps 0..K-1 by reference and starts with no tail. */
int rx_path_fork(RxPath *child, RxPath *parent, uint16_t divergence_step_index);

/* Inspection. rx_path_get_step copies logical step i. */
uint32_t rx_path_step_count(const RxPath *p);
int rx_path_get_step(const RxPath *p, uint32_t i, RxPathStep *out);
int rx_path_encoded_size(const RxPath *p, size_t *size);
int rx_path_equal(const RxPath *a, const RxPath *b, int *equal);

/* Identity. For a forked path every ancestor is re-hashed and must match the
 * parent_path_id recorded at fork time, else RX_PATH_ERR_ID_MISMATCH. */
int rx_path_semantic_id(const RxPath *p, SemanticId *out);
int rx_path_realization_id(const RxPath *p, const RxPathRealization *r, SemanticId *out);

/* Canonical encoding (spec 6.1, 6.2). The encoding is always the fully
 * reconstructed logical sequence; fork structure never appears on the wire. */
int rx_path_encode(const RxPath *p, uint8_t *out, size_t capacity, size_t *length);

/* Decode into a fresh base path. Refuses anything that is not exactly one
 * canonical encoding, then refuses unless the recomputed SEMANTIC_PATH_ID
 * equals *expected_id. On failure the arena is restored and *p is cleared. */
int rx_path_decode(RxPath *p, RxPathStepArena *arena, const uint8_t *bytes,
                   size_t length, const SemanticId *expected_id);

#endif
