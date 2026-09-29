/* Omega Visor V1 - lane 4: evidence (receipt) inspection.
 *
 * Reads receipt files under an evidence root directly (never includes
 * omega_evidence.h, which pulls in physics). The on-disk tree holds JSON
 * receipts in two shapes, both supported by one strict fixed-buffer JSON scanner:
 *   - milestone receipts  (evidence/omega_*_receipt.json): git_commit, host{os,arch},
 *     status, qualification_gates{total,passed,gates{}}, nested semantic_id/realization_id
 *   - run receipts, content-addressed (evidence/<LANE>/<sha256>.json): schema, run_id,
 *     candidate_commit/run_commit, tree_dirty, host{sysname,release,machine} or
 *     machine_identity{node,...}, hardware_scope|scope, gates{}|gate{}, silicon_observed,
 *     physics_commit|physics_forge_commit, selected_realization_identity
 * Everything else (txt/log/bin/omg/SHA256SUMS, malformed or oversize JSON) is
 * counted UNPARSED with its path.
 *
 * Scope law: qualification scope is NEVER upgraded. It is inferred only from
 * explicit receipt fields, and when several signals exist the LOWEST wins:
 *   silicon_observed:true, gpu_uuid, compute_class           -> SILICON
 *   silicon_observed:false                                   -> HOST (cap)
 *   a "qemu_target" key (any depth), or hardware_scope/scope naming qemu -> QEMU
 *     (gate names like *_QEMU_VIRT_PASS or modeled QEMU machine profiles do not count)
 *   hardware_scope/scope starting "host" / "no silicon claim"  -> HOST
 *   hardware_scope/scope containing "simulat"                -> SIMULATED
 *   none of the above (a bare host{} build-machine block included) -> UNKNOWN
 *
 * Fixed storage, not reentrant (one static read buffer + token pool). */
#ifndef VISOR_EVIDENCE_H
#define VISOR_EVIDENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omega_types.h"

#define VISOR_EVIDENCE_MAX_ITEMS 16
#define VISOR_EVIDENCE_MAX_UNPARSED_PATHS 8
#define VISOR_EVIDENCE_MAX_FILE_BYTES (256u * 1024u)

typedef enum {
    VISOR_QUAL_UNKNOWN, VISOR_QUAL_SIMULATED, VISOR_QUAL_HOST, VISOR_QUAL_QEMU, VISOR_QUAL_SILICON
} VisorQualScope;

typedef struct {
    char path[256];
    char receipt_id[72];        /* "sha256:<hex>" of the exact file bytes */
    char commit[41];            /* candidate_commit | run_commit | git_commit | candidate_git_commit | qualified_implementation_commit */
    char physics_commit[41];    /* physics_commit | physics_forge_commit */
    char run_id[64];
    char machine[96];           /* from machine_identity{} or host{}; "" if absent */
    char realization_id[72];    /* "sha256:<hex>" from realization_id | selected_/promoted_realization_identity | compiler_realization_id */
    VisorQualScope scope;
    const char *scope_name;     /* "UNKNOWN" / "SIMULATED" / "HOST" / "QEMU" / "SILICON" */
    bool tree_dirty;
    uint32_t gates_run, gates_passed;
    char claim[128];
    /* extensions (lane 4) */
    char scope_basis[96];       /* which explicit field(s) set the scope, "" when UNKNOWN */
    bool tree_state_known;      /* false: receipt says nothing about tree cleanliness */
    int name_hash_match;        /* 1: <sha256>.json name == content hash, 0: mismatch, -1: not hash-named */
} VisorEvidenceRecord;

typedef struct {
    VisorEvidenceRecord items[VISOR_EVIDENCE_MAX_ITEMS];
    size_t count;               /* items filled (<= 16, sorted by path) */
    size_t scanned;             /* files visited */
    size_t unparsed;            /* non-JSON, malformed or oversize files */
    /* extensions (lane 4) */
    size_t matched;             /* records that qualified (may exceed count: truncated) */
    bool truncated;
    char unparsed_paths[VISOR_EVIDENCE_MAX_UNPARSED_PATHS][256];
    size_t unparsed_listed;
} VisorEvidenceView;

const char *visor_qual_scope_name(VisorQualScope s);

/* All receipts under root, depth-first, sorted by path. -1 if root unreadable. */
int visor_evidence_scan(const char *evidence_root, VisorEvidenceView *out);
/* Receipts whose bytes mention the id's 64-hex form (semantic, realization or
 * machine id, any nesting). count==0 means "no evidence" and returns 0. */
int visor_evidence_for_id(const char *evidence_root, const SemanticId *id, VisorEvidenceView *out);
/* One receipt. -1 (fail closed) if unreadable, oversize, not strict JSON, or not an object. */
int visor_evidence_load(const char *path, VisorEvidenceRecord *out);

/* Deterministic renderings. Return bytes written (excl. NUL) or -1 if truncated/invalid. */
int visor_evidence_format_text(const VisorEvidenceView *v, char *out, size_t n);
int visor_evidence_format_json(const VisorEvidenceView *v, char *out, size_t n);

#endif /* VISOR_EVIDENCE_H */
