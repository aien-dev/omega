/*
 * visor_semantic.h -- Omega Visor V1, lane 1: read-only semantic inspection API.
 *
 * A stable, structured, READ-ONLY view over canonical objects stored in an
 * OmegaGraph. The console, the --json output and any future GUI consume
 * these views and never touch OmegaObject internals themselves.
 *
 * Contract:
 *  - const everywhere: nothing here mutates the graph or any object, never
 *    recomputes/stores an id, never mints or uses authority.
 *  - Fixed storage only (no heap). A VisorObjectView is roughly 30 KB; the
 *    caller owns it (stack, static or session storage).
 *  - Everything is derived from the canonical object (kind, payload,
 *    attributes, relations, constraints). No duplicated metadata.
 *  - Deterministic: fields and lists keep stored order; the same object always
 *    yields byte-identical text and JSON.
 *  - Return codes: 0 ok; -1 invalid argument / id not in graph / unsupported;
 *    -2 malformed object (reported in the view, its payload never trusted);
 *    -3 (eval only) the existing evaluator refused (overflow FAIL_CLOSED, div by 0).
 *
 * "Apply" objects: omega_build_apply stores an ApplyPayload under
 * KIND_OPERATION. As in omega_validate.c, an OPERATION whose payload_len is
 * >= sizeof(OperationPayload) is an operation; otherwise >= sizeof(ApplyPayload)
 * is an apply; shorter is malformed. The view reports kind "OPERATION" and
 * shape "APPLY" for those.
 */
#ifndef OMEGA_VISOR_SEMANTIC_H
#define OMEGA_VISOR_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_types.h"

#define VISOR_SEM_TYPE_TEXT   96
#define VISOR_SEM_REASON_LEN  128
#define VISOR_SEM_NAME_LEN    24
#define VISOR_SEM_MAX_DEPS    (OMEGA_MAX_RELATIONS + 8)
#define VISOR_SEM_EVAL_DEPTH  64
#define VISOR_SEM_GRAPH_DEPTH 64

typedef enum {
    VISOR_SHAPE_NONE = 0,
    VISOR_SHAPE_TYPE = 1,
    VISOR_SHAPE_VALUE = 2,
    VISOR_SHAPE_OPERATION = 3,
    VISOR_SHAPE_APPLY = 4,
    VISOR_SHAPE_OPAQUE = 5      /* any other kind: payload shown as length + hash only */
} VisorShape;

typedef struct {
    char key[OMEGA_MAX_KEY_LEN];
    uint16_t len;
    uint8_t value[OMEGA_MAX_VAL_LEN];
    bool is_utf8;               /* every byte printable ASCII: formatters show it as text too */
} VisorAttrView;

typedef struct {
    uint16_t kind;
    char kind_name[VISOR_SEM_NAME_LEN];
    SemanticId target;
    bool resolved;              /* target present in the graph */
} VisorRelView;

typedef struct {
    uint16_t kind;
    char kind_name[VISOR_SEM_NAME_LEN];
    uint16_t len;
    uint8_t payload[128];
} VisorConstraintView;

typedef struct {
    SemanticId id;
    char role[VISOR_SEM_NAME_LEN];  /* "rel:DEPENDS_ON", "type", "input0", "output", "op", "operand1", "elem" */
    bool resolved;
} VisorDepView;

typedef struct VisorObjectView {
    int status;                               /* 0 ok, -2 malformed */
    char malformed_reason[VISOR_SEM_REASON_LEN];

    SemanticId id;
    char id_text[72];                         /* "sha256:<64 hex>" */
    uint8_t kind;
    char kind_name[VISOR_SEM_NAME_LEN];       /* "VALUE", "OPERATION", ... */
    VisorShape shape;
    char shape_name[VISOR_SEM_NAME_LEN];      /* "TYPE", "VALUE", "OPERATION", "APPLY", "OPAQUE" */
    char type_text[VISOR_SEM_TYPE_TEXT];      /* "u64", "bool", "bitvector<32>", "seq<u8,16>", "fn(u64,u64)->u64" */
    bool has_type_id;                         /* VALUE type / OPERATION+APPLY result type */
    SemanticId type_id;

    /* TYPE */
    uint8_t type_tag;
    uint16_t type_width;
    uint32_t type_length;
    bool has_elem_type;
    SemanticId elem_type;

    /* VALUE */
    uint16_t value_byte_len;
    uint8_t value_bytes[64];
    bool has_u64;                              /* decoded when integer/bool type and width <= 64 */
    uint64_t value_u64;
    bool value_is_bool;

    /* OPERATION */
    uint8_t opcode;
    char opcode_name[VISOR_SEM_NAME_LEN];
    uint8_t overflow;
    char overflow_name[VISOR_SEM_NAME_LEN];
    uint8_t arity;
    SemanticId op_type_id;
    SemanticId input_types[4];
    SemanticId output_type;

    /* APPLY */
    SemanticId apply_op;
    uint8_t operand_count;
    SemanticId operands[4];

    /* Attributes / relations / constraints, in stored order. */
    uint16_t attr_count;
    VisorAttrView attrs[OMEGA_MAX_ATTRIBUTES];
    uint16_t rel_count;
    VisorRelView rels[OMEGA_MAX_RELATIONS];
    uint16_t const_count;
    VisorConstraintView consts[OMEGA_MAX_CONSTRAINTS];

    /* Canonical encoding (omega_canonical_encode) of the stored object. */
    size_t canonical_len;
    char canonical_sha256[65];
    bool canonical_id_matches;                /* sha256(encoding) == stored id */

    /* Dependencies: relation targets (stored order), then payload ids; deduped, zero ids skipped. */
    uint16_t dep_count;
    VisorDepView deps[VISOR_SEM_MAX_DEPS];
    uint16_t dangling_count;                  /* deps not present in the graph (reported, not an error) */
} VisorObjectView;

/* Inspect one object. 0 ok; -1 bad args or id not in graph; -2 malformed
 * (out->status == -2, out->malformed_reason set, id/kind still filled). */
int visor_semantic_inspect(const OmegaGraph *g, const SemanticId *id, VisorObjectView *out);

/* Render a view. Returns bytes written (excluding NUL) or -1 if it does not fit
 * (never a silently truncated result). Text is human; JSON is a view of the
 * view (valid JSON, fixed key order), NOT canonical bytes. */
int visor_semantic_format_text(const VisorObjectView *v, char *out, size_t n);
int visor_semantic_format_json(const VisorObjectView *v, char *out, size_t n);

/* Canonical type of a VALUE, result type of an OPERATION or APPLY.
 * Either output may be NULL. 0 ok; -1 none (TYPE/opaque kinds, zero or
 * unresolved type, missing id); -2 malformed. */
int visor_semantic_type_of(const OmegaGraph *g, const SemanticId *id, SemanticId *out_type_id,
                           char *type_text, size_t n);

/* Reachable subgraph from root, DFS pre-order following dependencies in view
 * order, deduped, two-space indent per depth. Line: "<KIND|APPLY> sha256:<hex> <summary>".
 * Unresolved targets print as "MISSING sha256:<hex>"; malformed objects as
 * "MALFORMED sha256:<hex> <reason>" (not followed). Returns bytes written,
 * -1 bad args / root missing / does not fit / depth > VISOR_SEM_GRAPH_DEPTH. */
int visor_semantic_graph_text(const OmegaGraph *g, const SemanticId *root, char *out, size_t n);

/* Reference evaluation of a pure graph: VALUE -> its u64; APPLY of a binary
 * OPERATION -> omega_eval_pure_binary_uint(opcode, overflow, width, a, b)
 * with width from the OPERATION's type (uint/bitvector/byte, 1..64), operands
 * evaluated recursively and type-checked against the OPERATION's input types.
 * Depth > VISOR_SEM_EVAL_DEPTH fails closed.
 * 0 ok; -1 unsupported/invalid/missing; -2 malformed; -3 evaluator refused. */
int visor_semantic_eval_u64(const OmegaGraph *g, const SemanticId *id, uint64_t *out);

/* Parts of an APPLY. out_operands must hold 4 entries. 0 ok; -1 not an apply/missing; -2 malformed. */
int visor_semantic_apply_parts(const OmegaGraph *g, const SemanticId *apply_id, SemanticId *out_op,
                               SemanticId *out_operands, size_t *out_count);

/* u64 of a VALUE whose type is uint/bitvector/byte/bool with width <= 64. 0 ok; -1 otherwise; -2 malformed. */
int visor_semantic_value_u64(const OmegaGraph *g, const SemanticId *value_id, uint64_t *out);

/* Name helpers (static strings; "UNKNOWN" for out-of-range). */
const char *visor_semantic_opcode_name(unsigned op);
const char *visor_semantic_overflow_name(unsigned ov);
const char *visor_semantic_relation_name(unsigned kind);
const char *visor_semantic_constraint_name(unsigned kind);
const char *visor_semantic_shape_name(VisorShape s);

#endif /* OMEGA_VISOR_SEMANTIC_H */
