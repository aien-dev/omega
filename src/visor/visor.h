/*
 * visor.h -- Omega Visor V1 shared session contract (lead-owned).
 *
 * The Visor is a viewport over existing canonical Omega objects. This header
 * holds ONLY the per-session bookkeeping every lane shares: one OmegaGraph,
 * named bindings, the `_` last result, and bounded tables of programs and
 * realizations. It carries no authority and no semantic meaning of its own.
 *
 * Rules (see spec/visor.md):
 *  - OmegaGraph is the only semantic store. Never duplicate object contents.
 *  - Nothing here may mint capabilities, publish, promote or mutate runtime state.
 *  - Every lookup fails closed: unknown names / ids return an error, never a guess.
 *  - Field/iteration order is deterministic so output can be tested byte-for-byte.
 */
#ifndef OMEGA_VISOR_H
#define OMEGA_VISOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_types.h"
#include "omega_machine.h"
#include "omega_program.h"
#include "omega_realize.h"

#define VISOR_MAX_BINDINGS     64
#define VISOR_MAX_PROGRAMS     16
#define VISOR_MAX_REALIZATIONS 16
#define VISOR_NAME_LEN         64

/* What a binding names. */
typedef enum {
    VISOR_BIND_NONE = 0,
    VISOR_BIND_OBJECT = 1,      /* a SemanticId present in session->graph */
    VISOR_BIND_PROGRAM = 2,     /* index into session->programs */
    VISOR_BIND_REALIZATION = 3  /* index into session->realizations */
} VisorBindKind;

typedef struct {
    char name[VISOR_NAME_LEN];
    VisorBindKind kind;
    SemanticId id;      /* object id, program_id, or realization_id */
    int index;          /* table index for PROGRAM / REALIZATION, else -1 */
} VisorBinding;

typedef struct {
    VisorBinding items[VISOR_MAX_BINDINGS];
    size_t count;
} VisorBindings;

/* A realization the session built. `subject_id` is the semantic object the
 * user asked to realize (e.g. an APPLY); `real.semantic_id` is whatever the
 * existing realizer keyed on (for omega_realize_pure_binary that is the
 * OPERATION id). Both are shown; neither is invented. */
typedef struct {
    RealizationObject real;
    SemanticId subject_id;
    bool subject_is_program;
    int program_index;          /* when subject_is_program */
    SemanticId machine_id;
    bool has_machine_id;
    char target_name[32];       /* e.g. "aarch64-v8a", "qemu-virt", "dgx-spark" */
    uint32_t estimated_cycles;  /* from synthesis/machine estimate; 0 = none */
    bool has_estimate;
} VisorRealizationEntry;

typedef struct {
    OmegaGraph *graph;                      /* the single semantic store */
    VisorBindings bindings;
    bool has_last;
    VisorBinding last;                      /* `_` */
    OmegaProgram programs[VISOR_MAX_PROGRAMS];
    size_t program_count;
    VisorRealizationEntry realizations[VISOR_MAX_REALIZATIONS];
    size_t realization_count;
    OmegaMachineGraph machine;              /* current machine model */
    bool has_machine;
    bool machine_is_observed;               /* false = assumed profile, not measured */
} VisorSession;

/* Lifecycle. init returns 0 on success, -1 on allocation failure. */
int  visor_session_init(VisorSession *s);
void visor_session_destroy(VisorSession *s);
void visor_session_clear(VisorSession *s);     /* `clear`: drop bindings + tables, fresh graph */

/* Bindings. set replaces an existing name. Returns 0 / -1 (full or bad name). */
int  visor_binding_set(VisorSession *s, const char *name, VisorBindKind kind,
                       const SemanticId *id, int index);
const VisorBinding *visor_binding_get(const VisorSession *s, const char *name);
void visor_set_last(VisorSession *s, const VisorBinding *b);

/* Resolve a user token: "_" | binding name | 64-hex SemanticId | "sha256:<hex>".
 * A hex id resolves to VISOR_BIND_OBJECT only if it exists in the graph,
 * otherwise to a PROGRAM/REALIZATION whose id matches. Returns 0 on success,
 * -1 if nothing matches (fail closed). */
int  visor_resolve(const VisorSession *s, const char *token, VisorBinding *out);

/* Table insertion. Return the new index or -1 when full. */
int  visor_program_add(VisorSession *s, const OmegaProgram *p);
int  visor_realization_add(VisorSession *s, const VisorRealizationEntry *e);

/* Small shared formatting helpers so every lane prints ids the same way. */
void visor_format_id(const SemanticId *id, char out[72]);   /* "sha256:<64 hex>" */
const char *visor_kind_name(SemanticKind k);                /* "VALUE", "TYPE", ... */

#endif /* OMEGA_VISOR_H */
