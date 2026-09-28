/*
 * rx_cortex.h -- Cortex, AIEN's durable typed memory (host reference).
 *
 * Cortex holds what the machine has seen, done, claimed and proven. It is
 * append-only: an object, once recorded, never changes, and its digest says
 * so. Nothing in Cortex is text. Every object is a fixed header plus a
 * payload of 64-bit words.
 *
 * Two axes, never merged:
 *
 *   content class  what the object IS: one of the nine memory classes of
 *                  doctrine AIEN.md §8.2 (entity, claim, observation, ...).
 *   treatment      how one cognitive operation may use it (pinned, live,
 *                  compressible, ...). Treatment is not stored here. It is
 *                  decided per operation by the state projection
 *                  (rx_projection.h).
 *
 * Protection bits are the one treatment fact Cortex does own: authority,
 * commit receipts, verification evidence, generation identity and effect
 * receipts may never be summarized, whatever an operation asks for.
 *
 * Cortex does not hand out its history. It answers typed queries: one
 * subject, one time range, one class and kind. The indexes are per subject
 * and time ordered, so a query costs the objects in its range, not the size
 * of the store. `examined` counts every object a query touched.
 *
 * Scope of this reference: one in-memory tier. Doctrine's L1/L2/L3 tiers
 * (accelerator memory, host memory, NVMe) are not implemented here.
 */
#ifndef RX_CORTEX_H
#define RX_CORTEX_H

#include <stddef.h>
#include <stdint.h>

/* Content axis: the nine memory classes of doctrine AIEN.md §8.2. */
typedef enum {
    CX_ENTITY = 1, CX_CLAIM, CX_OBSERVATION, CX_EXECUTION, CX_FAILURE,
    CX_EVIDENCE, CX_PLAN, CX_REALIZATION, CX_RELATIONSHIP,
    CX_CLASS_END
} CxClass;

/* State that may never be summarized, derived away or dropped for budget. */
enum {
    CX_PROT_AUTHORITY       = 1u << 0,
    CX_PROT_COMMIT_RECEIPT  = 1u << 1,
    CX_PROT_VERIFY_EVIDENCE = 1u << 2,
    CX_PROT_GENERATION_ID   = 1u << 3,
    CX_PROT_EFFECT_RECEIPT  = 1u << 4,
    CX_PROT_ALL             = 0x1fu
};

#define CX_LINKS 4u

typedef struct {
    uint64_t id;             /* 1-based and dense: object id == index + 1 */
    uint32_t cls;            /* CxClass */
    uint32_t kind;           /* fact type within the class; numbering belongs to the writer */
    uint64_t subject;        /* the entity the object is about */
    uint64_t t;              /* logical time of the fact */
    uint64_t generation;     /* generation the fact was recorded under */
    uint32_t branch;         /* 0 = the main line; else a hypothetical branch */
    uint32_t protect;        /* CX_PROT_* */
    uint64_t tag;            /* one small header value (cause code, condition key, ...) */
    uint64_t links[CX_LINKS];/* ids of related objects, 0 = none */
    uint32_t n;              /* payload words */
    uint64_t off;            /* payload offset in the arena */
    uint8_t digest[32];      /* SHA-256 of the canonical encoding */
} CxObject;

typedef struct {
    uint64_t *ids;
    uint32_t n, cap;
} CxIdList;

typedef struct {
    CxObject *obj;
    uint64_t n, cap;
    uint64_t *arena;
    uint64_t arena_n, arena_cap;
    CxIdList *by_subject;    /* per subject, in append (= time) order */
    uint64_t n_subjects;
    uint8_t chain[32];       /* running hash over every digest in append order */
    uint64_t examined;       /* objects touched by queries, cumulative */
} CxStore;

/* What a writer supplies; id, offset and digest are Cortex's. */
typedef struct {
    uint32_t cls, kind;
    uint64_t subject, t, generation;
    uint32_t branch, protect;
    uint64_t tag;
    uint64_t links[CX_LINKS];
} CxHeader;

enum { CX_OK = 0, CX_ERR_ARG = -1, CX_ERR_NOMEM = -2, CX_ERR_ORDER = -3, CX_ERR_DIGEST = -4 };

int  cx_init(CxStore *s, uint64_t n_subjects);
void cx_free(CxStore *s);

/* Append one object. Per subject, time may not go backwards (CX_ERR_ORDER). */
int  cx_append(CxStore *s, const CxHeader *h, const uint64_t *payload, uint32_t n, uint64_t *out_id);

const CxObject *cx_get(const CxStore *s, uint64_t id);
const uint64_t *cx_payload(const CxStore *s, const CxObject *o);

/* Recompute an object's digest and compare: CX_OK or CX_ERR_DIGEST. */
int  cx_verify(const CxStore *s, uint64_t id);

/* Replay the running hash from the objects: CX_OK if it matches `chain`. */
int  cx_verify_chain(const CxStore *s);

/* Canonical digest of a header plus payload. */
void cx_digest(const CxObject *o, const uint64_t *payload, uint8_t out[32]);

/* ---- typed queries (every touched object is counted in s->examined) ---- */

/* Matches cls/kind (0 = any), and branch unless branch_any. */
typedef struct {
    uint32_t cls, kind;
    uint32_t branch;
    int branch_any;
} CxFilter;

typedef int (*CxVisit)(void *user, const CxObject *o);

/* Objects of `subject` with t in [t0, t1], oldest first. The visitor returns
 * nonzero to stop. Returns the number visited. */
uint64_t cx_range(CxStore *s, uint64_t subject, uint64_t t0, uint64_t t1,
                  const CxFilter *f, CxVisit visit, void *user);

/* The newest matching object of `subject` with t <= t_max, or 0. */
uint64_t cx_latest(CxStore *s, uint64_t subject, uint64_t t_max, const CxFilter *f);

/* Count of `subject`'s objects with t < t0 (index arithmetic, no scan). */
uint64_t cx_count_before(const CxStore *s, uint64_t subject, uint64_t t0);
uint64_t cx_count_subject(const CxStore *s, uint64_t subject);

/* Test hook: overwrite one payload word without updating the digest. */
void cx_tamper(CxStore *s, uint64_t id, uint32_t word, uint64_t value);

#endif /* RX_CORTEX_H */
