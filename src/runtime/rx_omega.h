/*
 * rx_omega.h -- Omega as a resident realization faculty (ADR 0016 §43, R10).
 *
 * Nothing here is a service. No caller asks Omega for a realization and waits.
 * Omega's work is a set of reactions on the shared world:
 *
 *   workload.serve     request         -> result, demand      (production)
 *   omega.watch        demand window   -> search              (CostEstimate)
 *   omega.synthesize   search          -> candidate[k]        (RealizationCandidate)
 *   omega.verify.k     candidate[k]    -> verdict[k]          (V0 + sandboxed differential)
 *   omega.measure.k    verdict[k]      -> measure[k]          (benchmark against the incumbent)
 *   omega.select       measure[*]      -> selection           (recorded, eligible)
 *
 * Production reads `selection` as an ordinary input. It never waits for it.
 * The first request of a regime runs the semantic reference; a verified,
 * measured realization replaces it only after omega.select publishes one for
 * that regime.
 *
 * The operation is Omega's integer matrix-vector product (M12, omega_matvec):
 * y[i] = sum_j A[i*N+j] * x[j], wrapping mod 2^64. Candidates are the native
 * AArch64 realizations omega_matvec_synthesize emits. Candidate code lives in a
 * content-addressed realization store owned by this faculty; world objects
 * carry the 32-byte realization identity, never code or pointers.
 *
 * Unverified code is only ever run in a forked child. The parent executes a
 * realization only after its verdict passed and its bytes still hash to the
 * identity the candidate object names.
 */
#ifndef RX_OMEGA_H
#define RX_OMEGA_H

#include "rx_world.h"
#include "omega_machine.h"
#include "omega_matvec.h"
#include "omega_matvec_quad.h"

#include <pthread.h>
#include <stdint.h>

#define RX_OMEGA_SLOTS        5u    /* four Omega kinds + one optional test defect */
#define RX_OMEGA_STORE        16u
#define RX_OMEGA_WINDOW       16u   /* calls per published cost window */
#define RX_OMEGA_MAX_ELEMS    (1u << 16)

/* Object types (RxObject.type). */
enum {
    RX_OT_REQUEST = 0x5210u, RX_OT_DEMAND, RX_OT_RESULT, RX_OT_SEARCH,
    RX_OT_CANDIDATE, RX_OT_VERDICT, RX_OT_MEASURE, RX_OT_SELECTION
};

/* request:   0 seq, 1 M, 2 N, 3 seed                                        (external)
 * result:    0 seq, 1 digest, 2 realization used (0 = reference), 3 ns
 * demand:    0 regime calls, 1 regime ns, 2 window seq, 3 window mean ns,
 *            4 M, 5 N, 6 realization in use (id word 0), 7 calls all regimes
 * search:    0 epoch, 1 M, 2 N, 3 window mean ns, 4 regime calls, 5 regime ns,
 *            6 last inquiry sequence taken up (R11)
 * candidate: 0 epoch, 1 kind, 2..5 realization id, 6 code bytes, 7 state
 * verdict:   0 epoch, 1 state, 2 checks, 3 failures, 4 id word 0, 5 reason
 * measure:   0 epoch, 1 state, 2 candidate ps/call, 3 reference ps/call,
 *            4..7 realization id
 * selection: 0 epoch, 1..4 realization id (zero = reference), 5 ps/call,
 *            6 regime, 7 reference ps/call
 * regime = (M << 32) | N. */
enum { RX_OMEGA_SYNTHESIZED = 1 };
enum { RX_OMEGA_PASSED = 1, RX_OMEGA_REFUSED = 2 };
enum { RX_OMEGA_MEASURED = 1, RX_OMEGA_DECLINED = 2 };

/* Why a verdict refused. */
enum {
    RX_OMEGA_WHY_NONE = 0,
    RX_OMEGA_WHY_UNKNOWN,       /* the store has no bytes for this identity */
    RX_OMEGA_WHY_IDENTITY,      /* the bytes do not hash to the named identity */
    RX_OMEGA_WHY_STRUCTURE,     /* Omega V0 structural check failed */
    RX_OMEGA_WHY_DIFFERENTIAL,  /* disagreed with the semantic reference */
    RX_OMEGA_WHY_CRASHED,       /* the sandboxed child died */
    RX_OMEGA_WHY_HUNG,          /* the sandboxed child did not finish */
    RX_OMEGA_WHY_BOUNDS         /* wrote outside y or changed A or x */
};

/* Test-only faults. They add or alter a candidate; they never touch the
 * verifier, the store lookup or the selection rule. */
typedef enum {
    RX_OMEGA_DEFECT_NONE = 0,
    RX_OMEGA_DEFECT_SKIP_REMAINDER,  /* slot 4: unroll4 that ignores N % 4 */
    RX_OMEGA_DEFECT_CRASH,           /* slot 4: loads from address zero */
    RX_OMEGA_DEFECT_TAMPER           /* slot 4: stored bytes differ from the named identity */
} RxOmegaDefect;

typedef struct {
    SemanticId id;
    uint32_t kind;              /* 0..2 M12 kinds, 3 quad4, 4 the test defect */
    size_t code_len;
    uint8_t code[AARCH64_MAX_CODE_BYTES];
    void *page;                 /* read+execute mapping of code */
    int verified;               /* verdict passed for exactly these bytes */
    uint64_t parent_runs;       /* times the parent process executed it */
} RxOmegaRealization;

typedef struct {
    uint32_t n_slots;
    RxOmegaDefect defect;
    uint64_t hot_calls;         /* both must be reached in one regime */
    uint64_t hot_ns;
    uint32_t margin_pct;        /* a realization must beat the reference by this */
    uint32_t sandbox_ms;
} RxOmegaConfig;

typedef struct {
    RxObjRef request, demand, result, search, selection;
    RxObjRef candidate[RX_OMEGA_SLOTS];
    RxObjRef verdict[RX_OMEGA_SLOTS];
    RxObjRef measure[RX_OMEGA_SLOTS];
} RxOmegaObjects;

/* Resources: RX_OMEGA_RES_BASE + object index; the caller mints against them. */
#define RX_OMEGA_RES_BASE 0x5210000ull
enum {
    RX_OMEGA_RES_REQUEST = 0, RX_OMEGA_RES_DEMAND, RX_OMEGA_RES_RESULT, RX_OMEGA_RES_SEARCH,
    RX_OMEGA_RES_SELECTION, RX_OMEGA_RES_CANDIDATE0,
    RX_OMEGA_RES_VERDICT0 = RX_OMEGA_RES_CANDIDATE0 + RX_OMEGA_SLOTS,
    RX_OMEGA_RES_MEASURE0 = RX_OMEGA_RES_VERDICT0 + RX_OMEGA_SLOTS,
    RX_OMEGA_RES_COUNT = RX_OMEGA_RES_MEASURE0 + RX_OMEGA_SLOTS
};

/* Subjects the capabilities are bound to. */
enum { RX_OMEGA_SUBJ_SERVE = 21, RX_OMEGA_SUBJ_OMEGA = 22 };

/* One capability reference per (subject, object resource). Each reaction
 * declares READ for what it observes and READ|WRITE for what it publishes;
 * the root decides at run time whether the reference still grants that.
 * An entry a subject never touches may be left { UINT32_MAX, 0 }. */
typedef struct {
    RxCapRef serve[RX_OMEGA_RES_COUNT];
    RxCapRef omega[RX_OMEGA_RES_COUNT];
} RxOmegaCaps;

typedef struct RxOmegaFaculty {
    RxWorld *w;
    RxOmegaConfig cfg;
    RxOmegaObjects o;
    OmegaMachineGraph machine;
    MatVecSemanticSpec spec;

    pthread_mutex_t mu;         /* guards the store and the counters below */
    RxOmegaRealization store[RX_OMEGA_STORE];
    uint32_t n_store;

    uint32_t r_serve, r_watch, r_select, r_reconsider;
    RxObjRef inquiry;           /* R11: a plan object Omega may react to */
    /* Optional belief barrier for later searches. The first selection
     * establishes the incumbent. A later selection must observe AIEN's
     * belief about physical experiment evidence for its own search epoch. */
    RxObjRef experiment_evidence;
    RxCapRef experiment_evidence_cap;
    int require_experiment_evidence;
    /* R13. When set, production runs the realization this record names,
     * not the selection. The record has the selection layout. Omega's
     * selection is then only eligible; what is in force is decided by
     * whoever writes this record (in R13: the promotion authority). */
    RxObjRef serve_record;
    RxCapRef serve_record_cap;
    int serve_from_record;
    uint32_t r_synth[RX_OMEGA_SLOTS], r_verify[RX_OMEGA_SLOTS], r_measure[RX_OMEGA_SLOTS];
    struct { struct RxOmegaFaculty *f; uint32_t k; } slot[RX_OMEGA_SLOTS];

    /* Observability only; no reaction reads these. */
    uint64_t served_reference, served_realized;
    uint64_t sandbox_runs, sandbox_crashes;
    /* R16 C5: this faculty's caller credentials (rx_caller.h); null in a
     * world without bound callers. Held here, never published. */
    const RxCallerKeyring *keys;
} RxOmegaFaculty;

void rx_omega_default_config(RxOmegaConfig *cfg);

/* Create the objects. Resources are RX_OMEGA_RES_BASE + index. */
int rx_omega_create_objects(RxOmegaFaculty *f, RxWorld *w, const RxOmegaConfig *cfg);

/* Register the reactions with capabilities the caller minted. */
int rx_omega_register(RxOmegaFaculty *f, const RxOmegaCaps *caps);

/* Call after create_objects and before register. Belief field 0 is the
 * search epoch accepted from the resident physical experiment; field 2 is
 * the validity bit set by AIEN's evidence reaction. */
int rx_omega_require_evidence(RxOmegaFaculty *f, RxObjRef evidence, RxCapRef read_cap);

/* R13. Call after create_objects and before register. Production reads the
 * realization in force from `record` (fields 0 epoch, 1..4 realization id,
 * 6 regime) with `read_cap`, held by the serve subject. R10 and R11 never
 * call this; production then follows the selection as before. */
int rx_omega_serve_from(RxOmegaFaculty *f, RxObjRef record, RxCapRef read_cap);

/* Identity of these bytes as an Omega matvec realization on this machine,
 * computed the way the synthesizer names them. 0 on success. */
int rx_omega_identity_of(const RxOmegaFaculty *f, const uint8_t *code, size_t len,
                         SemanticId *out);

void rx_omega_destroy(RxOmegaFaculty *f);

/* Same function the verifier uses as its oracle. */
void rx_omega_fill(uint64_t seed, uint64_t *A, uint64_t *x, uint32_t M, uint32_t N);
uint64_t rx_omega_digest(const uint64_t *y, uint32_t M);

static inline uint64_t rx_omega_regime(uint64_t M, uint64_t N) { return (M << 32) | N; }

/* R11. Let Omega react to a plan another faculty publishes. Omega needs only
 * this layout: field 0 sequence, 1 action (RX_OMEGA_INQ_RESEARCH: search this
 * regime again, on whatever cores Omega runs now), 2 regime. Omega reads the
 * plan with `read_cap` and cannot write it. A plan that arrives while a
 * search is in flight waits for that search's selection. Search field 6
 * records the last plan Omega took up. R10 alone never registers this. */
enum { RX_OMEGA_INQ_RESEARCH = 1 };
int rx_omega_register_reconsider(RxOmegaFaculty *f, RxObjRef plan, RxCapRef read_cap,
                                 RxCapRef search_cap, RxCapRef selection_cap);

/* R14. Admit durable bytes after a restart: store them under the identity
 * they hash to, then run the same verifier omega.verify.k runs (identity, V0
 * structure, sandboxed differential on the regime's shape). 0 when verified;
 * production may then execute them. 1 with *out_why when refused; -1 when the
 * check could not run. It does not select or put anything in force. */
int rx_omega_readmit(RxOmegaFaculty *f, const uint8_t *code, size_t len, uint32_t kind,
                     uint64_t regime, SemanticId *out_id, uint32_t *out_why);

/* Read-only store lookup for tests. Returns a copy; 0 if found. */
int rx_omega_store_find(RxOmegaFaculty *f, uint64_t id_word0, RxOmegaRealization *out);

#endif /* RX_OMEGA_H */
