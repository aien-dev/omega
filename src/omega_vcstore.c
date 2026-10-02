#include "omega_vcstore.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Every refusal guard sits on one line carrying a VC1S:<tag> marker. The mutation test
 * (Makefile test-vcstore) copies this file, deletes or weakens exactly that line, and
 * requires tests/test_omega_vcstore.c to fail. A comment-only marker line is a hook where a
 * mutant injects a forbidden behaviour. Keep one guard per line when editing. */

#define TAG "AIEN_VERIFIED_CRUMB_V1"
#define TAG_LEN 22
#define FILE_MAGIC "AIEN_VCSTORE_V1"
#define FILE_MAGIC_LEN 15
#define MAX_NAME 128

static const uint8_t ZERO32[32];

const char *omega_vcstore_code_name(int code) {
    switch (code) {
    case OMEGA_VCS_OK: return "OK";
    case OMEGA_VCS_UNVERIFIED_DEPENDENCY: return "UNVERIFIED_DEPENDENCY";
    case OMEGA_VCS_MISSING_RECEIPT: return "MISSING_RECEIPT";
    case OMEGA_VCS_RECEIPT_HASH_MISMATCH: return "RECEIPT_HASH_MISMATCH";
    case OMEGA_VCS_DEPENDENCY_NOT_PINNED: return "DEPENDENCY_NOT_PINNED";
    case OMEGA_VCS_DEPENDENCY_CYCLE: return "DEPENDENCY_CYCLE";
    case OMEGA_VCS_STALE_RECEIPT: return "STALE_RECEIPT";
    case OMEGA_VCS_UNDECLARED_IMPORT: return "UNDECLARED_IMPORT";
    case OMEGA_VCS_TAINTED_ARTIFACT: return "TAINTED_ARTIFACT";
    case OMEGA_VCS_UNKNOWN_VERIFIER_PROFILE: return "UNKNOWN_VERIFIER_PROFILE";
    case OMEGA_VCS_BAD_DOMAIN_TAG: return "BAD_DOMAIN_TAG";
    case OMEGA_VCS_UNKNOWN_FORMAT_VERSION: return "UNKNOWN_FORMAT_VERSION";
    case OMEGA_VCS_TRUNCATED: return "TRUNCATED";
    case OMEGA_VCS_TRAILING_BYTES: return "TRAILING_BYTES";
    case OMEGA_VCS_DUPLICATE_DEPENDENCY: return "DUPLICATE_DEPENDENCY";
    case OMEGA_VCS_UNSORTED_DEPENDENCIES: return "UNSORTED_DEPENDENCIES";
    case OMEGA_VCS_NONCANONICAL_SET: return "NONCANONICAL_SET";
    case OMEGA_VCS_BAD_STRING: return "BAD_STRING";
    case OMEGA_VCS_ZERO_ID: return "ZERO_ID";
    case OMEGA_VCS_TOO_MANY_ENTRIES: return "TOO_MANY_ENTRIES";
    case OMEGA_VCS_BAD_DIGEST_KIND: return "BAD_DIGEST_KIND";
    case OMEGA_VCS_VCSTORE_ID_MISMATCH: return "VCSTORE_ID_MISMATCH";
    case OMEGA_VCS_VCSTORE_IMMUTABLE_CONFLICT: return "VCSTORE_IMMUTABLE_CONFLICT";
    case OMEGA_VCS_VCSTORE_MALFORMED: return "VCSTORE_MALFORMED";
    case OMEGA_VCS_VCSTORE_NOT_FOUND: return "VCSTORE_NOT_FOUND";
    case OMEGA_VCS_VCSTORE_NAME_EXISTS: return "VCSTORE_NAME_EXISTS";
    case OMEGA_VCS_VCSTORE_CAPACITY: return "VCSTORE_CAPACITY";
    case OMEGA_VCS_VCSTORE_NOMEM: return "VCSTORE_NOMEM";
    case OMEGA_VCS_VCSTORE_IO: return "VCSTORE_IO";
    }
    return "UNKNOWN_CODE";
}

/* ---- decoder (SPEC 3; refusal order is field order, the first failure wins) ---- */

typedef struct { const uint8_t *b; size_t n, pos; int err; } cur_t;

static void cfail(cur_t *c, int code) { if (!c->err) c->err = code; }

static int need(cur_t *c, size_t k) {
    if (c->err) return 0;
    if (c->n - c->pos < k) { cfail(c, OMEGA_VCS_TRUNCATED); return 0; } /* VC1S:dec-truncated */
    return 1;
}
static uint32_t rd32(cur_t *c) {
    if (!need(c, 4)) return 0;
    const uint8_t *p = c->b + c->pos; c->pos += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t rd64(cur_t *c) {
    uint64_t hi = rd32(c), lo = rd32(c);
    return hi << 32 | lo;
}
static const uint8_t *rd_idp(cur_t *c) {
    if (!need(c, 32)) return ZERO32;
    const uint8_t *p = c->b + c->pos; c->pos += 32;
    return p;
}
static int is_zero32(const uint8_t *p) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= p[i];
    return acc == 0;
}
static uint32_t rd_count(cur_t *c) {
    uint32_t n = rd32(c);
    if (!c->err && n > OMEGA_VC_MAX_ENTRIES) { cfail(c, OMEGA_VCS_TOO_MANY_ENTRIES); return 0; } /* VC1S:dec-count */
    return n;
}
static void rd_str(cur_t *c, OmegaVcStr *out) {
    uint64_t len = rd64(c);
    out->p = (const uint8_t *)""; out->len = 0;
    if (c->err) return;
    if (len == 0 || len > OMEGA_VC_MAX_STRING) { cfail(c, OMEGA_VCS_BAD_STRING); return; } /* VC1S:dec-string-len */
    if (!need(c, (size_t)len)) return;
    for (uint64_t i = 0; i < len; i++) {
        uint8_t ch = c->b[c->pos + i];
        if (ch < 0x21 || ch > 0x7e) { cfail(c, OMEGA_VCS_BAD_STRING); return; } /* VC1S:dec-string-char */
    }
    out->p = c->b + c->pos; out->len = (size_t)len; c->pos += (size_t)len;
}
static int str_cmp(const OmegaVcStr *a, const OmegaVcStr *b) {
    size_t m = a->len < b->len ? a->len : b->len;
    int r = memcmp(a->p, b->p, m);
    if (r) return r;
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

int omega_vc_decode(const uint8_t *b, size_t n, OmegaVcView *v) {
    if (!v || (!b && n)) return OMEGA_VCS_VCSTORE_MALFORMED;
    cur_t c = { b ? b : ZERO32, n, 0, 0 };
    memset(v, 0, sizeof *v);
    if (!need(&c, TAG_LEN)) return c.err;
    if (memcmp(b, TAG, TAG_LEN) != 0) return OMEGA_VCS_BAD_DOMAIN_TAG; /* VC1S:dec-tag */
    c.pos = TAG_LEN;
    v->format_version = rd32(&c);
    if (!c.err && v->format_version != OMEGA_VC_FORMAT_VERSION) cfail(&c, OMEGA_VCS_UNKNOWN_FORMAT_VERSION); /* VC1S:dec-version */
    v->semantic_id = rd_idp(&c);
    v->contract_id = rd_idp(&c);
    if (need(&c, 1)) {
        v->digest_kind = c.b[c.pos++];
        if (v->digest_kind != OMEGA_VC_DIGEST_SOURCE && v->digest_kind != OMEGA_VC_DIGEST_IR) cfail(&c, OMEGA_VCS_BAD_DIGEST_KIND); /* VC1S:dec-kind */
    }
    v->source_or_ir_digest = rd_idp(&c);
    if (!c.err && (is_zero32(v->semantic_id) || is_zero32(v->contract_id) || is_zero32(v->source_or_ir_digest))) cfail(&c, OMEGA_VCS_ZERO_ID); /* VC1S:dec-zero-ids */
    v->n_realizations = rd_count(&c);
    v->realization_ids = c.b + c.pos;
    const uint8_t *prev = NULL;
    for (uint32_t i = 0; i < v->n_realizations && !c.err; i++) {
        const uint8_t *id = rd_idp(&c);
        if (!c.err && prev && memcmp(prev, id, 32) >= 0) cfail(&c, OMEGA_VCS_NONCANONICAL_SET); /* VC1S:dec-real-order */
        prev = id;
    }
    v->n_dependencies = rd_count(&c);
    v->dependencies = c.b + c.pos;
    prev = NULL;
    for (uint32_t i = 0; i < v->n_dependencies && !c.err; i++) {
        const uint8_t *sid = rd_idp(&c);
        const uint8_t *req = rd_idp(&c);
        if (c.err) break;
        if (memcmp(sid, v->semantic_id, 32) == 0) { cfail(&c, OMEGA_VCS_DEPENDENCY_CYCLE); break; } /* VC1S:dec-dep-self */
        if (is_zero32(sid) || is_zero32(req)) { cfail(&c, OMEGA_VCS_ZERO_ID); break; } /* VC1S:dec-dep-zero */
        if (prev) {
            int cmp = memcmp(prev, sid, 32);
            if (cmp == 0) { cfail(&c, OMEGA_VCS_DUPLICATE_DEPENDENCY); break; } /* VC1S:dec-dep-dup */
            if (cmp > 0) { cfail(&c, OMEGA_VCS_UNSORTED_DEPENDENCIES); break; } /* VC1S:dec-dep-order */
        }
        prev = sid;
    }
    v->receipt_id = rd_idp(&c);
    if (!c.err && is_zero32(v->receipt_id)) cfail(&c, OMEGA_VCS_MISSING_RECEIPT); /* VC1S:dec-receipt */
    rd_str(&c, &v->verifier_profile);
    rd_str(&c, &v->verifier_version);
    v->evidence_root = rd_idp(&c);
    if (!c.err && is_zero32(v->evidence_root)) cfail(&c, OMEGA_VCS_ZERO_ID); /* VC1S:dec-evroot */
    v->n_exports = rd_count(&c);
    for (uint32_t i = 0; i < v->n_exports && !c.err; i++) {
        rd_str(&c, &v->exports[i]);
        if (!c.err && i > 0 && str_cmp(&v->exports[i - 1], &v->exports[i]) >= 0) cfail(&c, OMEGA_VCS_NONCANONICAL_SET); /* VC1S:dec-exports-order */
    }
    v->n_capabilities = rd_count(&c);
    for (uint32_t i = 0; i < v->n_capabilities && !c.err; i++) {
        rd_str(&c, &v->capabilities[i]);
        if (!c.err && i > 0 && str_cmp(&v->capabilities[i - 1], &v->capabilities[i]) >= 0) cfail(&c, OMEGA_VCS_NONCANONICAL_SET); /* VC1S:dec-caps-order */
    }
    if (!c.err && c.pos != c.n) cfail(&c, OMEGA_VCS_TRAILING_BYTES); /* VC1S:dec-trailing */
    return c.err;
}

void omega_vc_compute_id(const uint8_t *bytes, size_t len, uint8_t out[OMEGA_VC_ID_BYTES]) {
    sha256_hash(bytes, len, out); /* VC1S:id-hash */
}

/* ---- store ---- */

int omega_vcstore_init(OmegaVcStore *s) {
    if (!s) return OMEGA_VCS_VCSTORE_MALFORMED;
    memset(s, 0, sizeof *s);
    return OMEGA_VCS_OK;
}

void omega_vcstore_destroy(OmegaVcStore *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) free(s->objs[i].bytes);
    for (size_t i = 0; i < s->n_names; i++) free(s->names[i].name);
    free(s->objs);
    free(s->names);
    memset(s, 0, sizeof *s);
}

size_t omega_vcstore_count(const OmegaVcStore *s) { return s ? s->count : 0; }

/* lower bound by semantic_id; returns 1 if found at *pos, else 0 with *pos the insert point */
static int vcs_find(const OmegaVcStore *s, const uint8_t *id, size_t *pos) {
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = memcmp(s->objs[mid].semantic_id, id, 32);
        if (c == 0) { *pos = mid; return 1; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    *pos = lo;
    return 0;
}

/* Fetch object idx with every integrity check: the VC id is recomputed from the stored bytes,
 * the bytes are decoded again, and the decoded semantic_id must be the key. */
static int vcs_fetch(const OmegaVcStore *s, size_t idx, OmegaVcRecord *out) {
    const OmegaVcObject *o = &s->objs[idx];
    uint8_t id[32];
    omega_vc_compute_id(o->bytes, o->len, id);
    if (memcmp(id, o->vc_id, 32) != 0) return OMEGA_VCS_VCSTORE_ID_MISMATCH; /* VC1S:get-recompute */
    int rc = omega_vc_decode(o->bytes, o->len, &out->vc);
    if (rc) return rc;
    if (memcmp(out->vc.semantic_id, o->semantic_id, 32) != 0) return OMEGA_VCS_VCSTORE_ID_MISMATCH; /* VC1S:get-key */
    out->canonical = o->bytes;
    out->canonical_len = o->len;
    memcpy(out->vc_id, o->vc_id, 32);
    out->admission_kind = o->admission_kind;
    return OMEGA_VCS_OK;
}

static int vcs_insert(OmegaVcStore *s, const uint8_t *canon, size_t len, const uint8_t *claimed, uint8_t kind) {
    if (!s || !canon || len == 0) return OMEGA_VCS_VCSTORE_MALFORMED;
    if (!claimed) return OMEGA_VCS_VCSTORE_MALFORMED; /* VC1S:claimed-required */
    /* VC1S:no-cap (the store has no fixed capacity: it grows) */
    OmegaVcRecord *rec = malloc(sizeof *rec);
    if (!rec) return OMEGA_VCS_VCSTORE_NOMEM;
    int rc = omega_vc_decode(canon, len, &rec->vc);
    if (rc) { free(rec); return rc; }
    uint8_t id[32];
    omega_vc_compute_id(canon, len, id);
    if (memcmp(id, claimed, 32) != 0) { free(rec); return OMEGA_VCS_VCSTORE_ID_MISMATCH; } /* VC1S:insert-idcheck */
    size_t pos;
    if (vcs_find(s, rec->vc.semantic_id, &pos)) {
        const OmegaVcObject *o = &s->objs[pos];
        free(rec);
        if (o->len == len && o->admission_kind == kind && memcmp(o->bytes, canon, len) == 0) return OMEGA_VCS_OK; /* VC1S:idempotent */
        return OMEGA_VCS_VCSTORE_IMMUTABLE_CONFLICT; /* VC1S:immutable */
    }
    /* every dependency must already be stored, under the contract this record requires */
    OmegaVcRecord *dr = malloc(sizeof *dr);
    if (!dr) { free(rec); return OMEGA_VCS_VCSTORE_NOMEM; }
    for (uint32_t i = 0; i < rec->vc.n_dependencies; i++) {
        const uint8_t *dep = rec->vc.dependencies + 64 * (size_t)i;
        size_t di;
        if (!vcs_find(s, dep, &di)) { rc = OMEGA_VCS_UNVERIFIED_DEPENDENCY; break; } /* VC1S:dep-exists */
        rc = vcs_fetch(s, di, dr);
        if (rc) break;
        if (memcmp(dr->vc.contract_id, dep + 32, 32) != 0) { rc = OMEGA_VCS_UNVERIFIED_DEPENDENCY; break; } /* VC1S:dep-contract */
    }
    free(dr);
    if (rc) { free(rec); return rc; }
    uint8_t sid[32];
    memcpy(sid, rec->vc.semantic_id, 32);
    free(rec);

    uint8_t *cp = malloc(len);
    if (!cp) return OMEGA_VCS_VCSTORE_NOMEM;
    memcpy(cp, canon, len);
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        if (nc > SIZE_MAX / sizeof *s->objs) { free(cp); return OMEGA_VCS_VCSTORE_NOMEM; }
        OmegaVcObject *n = realloc(s->objs, nc * sizeof *s->objs);
        if (!n) { free(cp); return OMEGA_VCS_VCSTORE_NOMEM; }
        s->objs = n; s->cap = nc;
    }
    size_t at = pos; /* VC1S:sorted-insert */
    memmove(&s->objs[at + 1], &s->objs[at], (s->count - at) * sizeof *s->objs);
    OmegaVcObject *o = &s->objs[at];
    memcpy(o->semantic_id, sid, 32);
    memcpy(o->vc_id, id, 32);
    o->admission_kind = kind;
    o->bytes = cp;
    o->len = len;
    s->count++;
    return OMEGA_VCS_OK;
}

int omega_vcstore_insert(OmegaVcStore *s, const uint8_t *canonical, size_t len, const uint8_t claimed_vc_id[32]) {
    return vcs_insert(s, canonical, len, claimed_vc_id, OMEGA_VCS_ADMISSION_VERIFIED);
}

int omega_vcstore_insert_bootstrap(OmegaVcStore *s, const uint8_t *canonical, size_t len, const uint8_t claimed_vc_id[32]) {
    return vcs_insert(s, canonical, len, claimed_vc_id, OMEGA_VCS_ADMISSION_BOOTSTRAP); /* VC1S:kind-bootstrap */
}

int omega_vcstore_get(const OmegaVcStore *s, const uint8_t semantic_id[32], OmegaVcRecord *out) {
    if (!s || !semantic_id || !out) return OMEGA_VCS_VCSTORE_MALFORMED;
    size_t idx;
    if (!vcs_find(s, semantic_id, &idx)) return OMEGA_VCS_UNVERIFIED_DEPENDENCY;
    return vcs_fetch(s, idx, out);
}

int omega_vcstore_receipt_of(const OmegaVcStore *s, const uint8_t semantic_id[32], uint8_t out_receipt[32]) {
    if (!out_receipt) return OMEGA_VCS_VCSTORE_MALFORMED;
    OmegaVcRecord *rec = malloc(sizeof *rec);
    if (!rec) return OMEGA_VCS_VCSTORE_NOMEM;
    int rc = omega_vcstore_get(s, semantic_id, rec);
    if (rc == OMEGA_VCS_OK) memcpy(out_receipt, rec->vc.receipt_id, 32); /* VC1S:receipt-of */
    free(rec);
    return rc;
}

int omega_vcstore_admission_kind(const OmegaVcStore *s, const uint8_t semantic_id[32], uint8_t *out_kind) {
    if (!out_kind) return OMEGA_VCS_VCSTORE_MALFORMED;
    OmegaVcRecord *rec = malloc(sizeof *rec);
    if (!rec) return OMEGA_VCS_VCSTORE_NOMEM;
    int rc = omega_vcstore_get(s, semantic_id, rec);
    if (rc == OMEGA_VCS_OK) *out_kind = rec->admission_kind;
    free(rec);
    return rc;
}

/* ---- dependency walk: post-order, shared by closure and the load-time graph check ---- */

typedef struct { size_t idx; uint32_t n_dep, next; const uint8_t *deps; } frame_t;

/* state: 0 unseen, 1 on the current path, 2 done. Visits root and everything it needs that is
 * still unseen. Emits newly finished ids into out_ids while *emitted < cap, always counts. */
static int vcs_walk(const OmegaVcStore *s, size_t root, uint8_t *state, frame_t *stack, OmegaVcRecord *rec,
                    uint8_t *out_ids, size_t cap, size_t *emitted) {
    /* VC1S:walk-names (no resolution step may read the name index) */
    if (state[root] == 2) return OMEGA_VCS_OK;
    int rc = vcs_fetch(s, root, rec);
    if (rc) return rc;
    size_t sp = 0;
    state[root] = 1;
    stack[sp++] = (frame_t){ root, rec->vc.n_dependencies, 0, rec->vc.dependencies };
    while (sp) {
        frame_t *f = &stack[sp - 1];
        if (f->next < f->n_dep) {
            const uint8_t *dep = f->deps + 64 * (size_t)f->next++; /* VC1S:walk-order */
            size_t di;
            if (!vcs_find(s, dep, &di)) return OMEGA_VCS_UNVERIFIED_DEPENDENCY; /* VC1S:walk-dep-exists */
            rc = vcs_fetch(s, di, rec);
            if (rc) return rc;
            if (memcmp(rec->vc.contract_id, dep + 32, 32) != 0) return OMEGA_VCS_UNVERIFIED_DEPENDENCY; /* VC1S:walk-contract */
            if (state[di] == 1) return OMEGA_VCS_DEPENDENCY_CYCLE; /* VC1S:walk-cycle */
            if (state[di] == 2) continue; /* VC1S:walk-visited */
            state[di] = 1;
            stack[sp++] = (frame_t){ di, rec->vc.n_dependencies, 0, rec->vc.dependencies };
        } else {
            state[f->idx] = 2;
            if (*emitted < cap) memcpy(out_ids + 32 * *emitted, s->objs[f->idx].semantic_id, 32);
            (*emitted)++;
            sp--;
        }
    }
    return OMEGA_VCS_OK;
}

int omega_vcstore_closure(const OmegaVcStore *s, const uint8_t id[32], uint8_t *out_ids, size_t cap, size_t *out_n) {
    if (!out_n) return OMEGA_VCS_VCSTORE_MALFORMED;
    *out_n = 0;
    if (!s || !id || (cap && !out_ids)) return OMEGA_VCS_VCSTORE_MALFORMED;
    size_t root;
    if (!vcs_find(s, id, &root)) return OMEGA_VCS_UNVERIFIED_DEPENDENCY;
    uint8_t *state = calloc(s->count, 1);
    frame_t *stack = malloc(s->count * sizeof *stack);
    OmegaVcRecord *rec = malloc(sizeof *rec);
    int rc;
    size_t emitted = 0;
    if (!state || !stack || !rec) { rc = OMEGA_VCS_VCSTORE_NOMEM; goto out; }
    rc = vcs_walk(s, root, state, stack, rec, out_ids, cap, &emitted);
    if (rc) goto out;
    *out_n = emitted;
    if (emitted > cap) rc = OMEGA_VCS_VCSTORE_CAPACITY; /* VC1S:walk-capacity */
out:
    free(state); free(stack); free(rec);
    return rc;
}

/* Every dependency present with its contract, no cycle, over the whole store. */
static int vcs_graph_check(const OmegaVcStore *s) {
    if (s->count == 0) return OMEGA_VCS_OK;
    uint8_t *state = calloc(s->count, 1);
    frame_t *stack = malloc(s->count * sizeof *stack);
    OmegaVcRecord *rec = malloc(sizeof *rec);
    int rc = OMEGA_VCS_OK;
    size_t emitted = 0;
    if (!state || !stack || !rec) rc = OMEGA_VCS_VCSTORE_NOMEM;
    for (size_t i = 0; !rc && i < s->count; i++) rc = vcs_walk(s, i, state, stack, rec, NULL, 0, &emitted);
    free(state); free(stack); free(rec);
    return rc;
}

/* ---- digests ---- */

static void u32be(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    sha256_update(c, b, 4);
}
static void u64be(sha256_ctx *c, uint64_t v) { u32be(c, (uint32_t)(v >> 32)); u32be(c, (uint32_t)v); }

int omega_vcstore_digest(const OmegaVcStore *s, uint8_t out[32]) {
    if (!s || !out) return OMEGA_VCS_VCSTORE_MALFORMED;
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"VCS1", 4); /* VC1S:digest-magic */
    u64be(&ctx, s->count);
    for (size_t i = 0; i < s->count; i++) {
        const OmegaVcObject *o = &s->objs[i];
        sha256_update(&ctx, o->semantic_id, 32);
        uint8_t kind = o->admission_kind; /* VC1S:digest-kind */
        sha256_update(&ctx, &kind, 1);
        u64be(&ctx, o->len);
        sha256_update(&ctx, o->bytes, o->len); /* VC1S:digest-bytes */
    }
    /* VC1S:digest-names (the name index is never part of the object digest) */
    sha256_final(&ctx, out);
    return OMEGA_VCS_OK;
}

int omega_vcstore_name_index_digest(const OmegaVcStore *s, uint8_t out[32]) {
    if (!s || !out) return OMEGA_VCS_VCSTORE_MALFORMED;
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"VCN1", 4);
    u64be(&ctx, s->n_names);
    for (size_t i = 0; i < s->n_names; i++) {
        const OmegaVcName *nm = &s->names[i];
        size_t l = strlen(nm->name);
        u64be(&ctx, l);
        sha256_update(&ctx, (const uint8_t *)nm->name, l);
        sha256_update(&ctx, nm->id, 32); /* VC1S:namedigest-id */
    }
    sha256_final(&ctx, out);
    return OMEGA_VCS_OK;
}

/* ---- name index ---- */

static int name_valid(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > MAX_NAME) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = name[i];
        int alpha = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
        int rest = alpha || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
        if (i == 0 ? !alpha : !rest) return 0;
    }
    return 1;
}

static int name_find(const OmegaVcStore *s, const char *name, size_t *pos) {
    size_t lo = 0, hi = s->n_names;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(s->names[mid].name, name);
        if (c == 0) { *pos = mid; return 1; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    *pos = lo;
    return 0;
}

static int name_insert_at(OmegaVcStore *s, size_t pos, const char *name, const uint8_t *id) {
    size_t l = strlen(name);
    char *cp = malloc(l + 1);
    if (!cp) return OMEGA_VCS_VCSTORE_NOMEM;
    memcpy(cp, name, l + 1);
    if (s->n_names == s->cap_names) {
        size_t nc = s->cap_names ? s->cap_names * 2 : 8;
        if (nc > SIZE_MAX / sizeof *s->names) { free(cp); return OMEGA_VCS_VCSTORE_NOMEM; }
        OmegaVcName *n = realloc(s->names, nc * sizeof *s->names);
        if (!n) { free(cp); return OMEGA_VCS_VCSTORE_NOMEM; }
        s->names = n; s->cap_names = nc;
    }
    memmove(&s->names[pos + 1], &s->names[pos], (s->n_names - pos) * sizeof *s->names);
    s->names[pos].name = cp;
    memcpy(s->names[pos].id, id, 32);
    s->n_names++;
    return OMEGA_VCS_OK;
}

int omega_vcstore_name_bind(OmegaVcStore *s, const char *name, const uint8_t id[32]) {
    if (!s || !name || !id) return OMEGA_VCS_VCSTORE_MALFORMED;
    if (!name_valid(name)) return OMEGA_VCS_VCSTORE_MALFORMED; /* VC1S:name-valid */
    size_t di;
    if (!vcs_find(s, id, &di)) return OMEGA_VCS_UNVERIFIED_DEPENDENCY; /* VC1S:name-id-exists */
    /* VC1S:name-many (many names may point at one id) */
    size_t np;
    if (name_find(s, name, &np)) {
        if (memcmp(s->names[np].id, id, 32) == 0) return OMEGA_VCS_OK;
        return OMEGA_VCS_VCSTORE_NAME_EXISTS; /* VC1S:name-no-silent-rebind */
    }
    return name_insert_at(s, np, name, id); /* VC1S:name-sorted */
}

int omega_vcstore_name_rebind(OmegaVcStore *s, const char *name, const uint8_t id[32]) {
    if (!s || !name || !id) return OMEGA_VCS_VCSTORE_MALFORMED;
    size_t di, np;
    if (!vcs_find(s, id, &di)) return OMEGA_VCS_UNVERIFIED_DEPENDENCY; /* VC1S:rebind-id-exists */
    if (!name_find(s, name, &np)) return OMEGA_VCS_VCSTORE_NOT_FOUND; /* VC1S:rebind-exists */
    memcpy(s->names[np].id, id, 32); /* VC1S:rebind-write */
    return OMEGA_VCS_OK;
}

int omega_vcstore_resolve_name(const OmegaVcStore *s, const char *name, uint8_t out_id[32]) {
    if (!s || !name || !out_id) return OMEGA_VCS_VCSTORE_MALFORMED;
    size_t np;
    if (!name_find(s, name, &np)) return OMEGA_VCS_VCSTORE_NOT_FOUND;
    memcpy(out_id, s->names[np].id, 32); /* VC1S:resolve */
    return OMEGA_VCS_OK;
}

/* ---- persistence ---- */

typedef struct { uint8_t *p; size_t n, cap; int err; } wbuf_t;

static void w_put(wbuf_t *w, const void *d, size_t k) {
    if (w->err) return;
    if (w->n + k > w->cap) {
        size_t nc = (w->n + k) * 2 + 256;
        uint8_t *np = realloc(w->p, nc);
        if (!np) { w->err = OMEGA_VCS_VCSTORE_NOMEM; return; }
        w->p = np; w->cap = nc;
    }
    memcpy(w->p + w->n, d, k);
    w->n += k;
}
static void w_u64(wbuf_t *w, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i));
    w_put(w, b, 8);
}

int omega_vcstore_save(const OmegaVcStore *s, const char *path) {
    if (!s || !path) return OMEGA_VCS_VCSTORE_MALFORMED;
    OmegaVcRecord *rec = malloc(sizeof *rec);
    if (!rec) return OMEGA_VCS_VCSTORE_NOMEM;
    for (size_t i = 0; i < s->count; i++) {
        int rc = vcs_fetch(s, i, rec); /* VC1S:save-verify */
        if (rc) { free(rec); return rc; }
    }
    free(rec);
    wbuf_t w = { 0, 0, 0, 0 };
    w_put(&w, FILE_MAGIC, FILE_MAGIC_LEN);
    w_u64(&w, s->count);
    for (size_t i = 0; i < s->count; i++) {
        w_put(&w, &s->objs[i].admission_kind, 1);
        w_u64(&w, s->objs[i].len);
        w_put(&w, s->objs[i].bytes, s->objs[i].len);
    }
    w_u64(&w, s->n_names);
    for (size_t i = 0; i < s->n_names; i++) {
        size_t l = strlen(s->names[i].name);
        w_u64(&w, l);
        w_put(&w, s->names[i].name, l);
        w_put(&w, s->names[i].id, 32);
    }
    uint8_t d1[32], d2[32];
    omega_vcstore_digest(s, d1);
    omega_vcstore_name_index_digest(s, d2);
    w_put(&w, d1, 32);
    w_put(&w, d2, 32);
    if (w.err) { free(w.p); return w.err; }

    size_t pl = strlen(path);
    char *tmp = malloc(pl + 5);
    if (!tmp) { free(w.p); return OMEGA_VCS_VCSTORE_NOMEM; }
    memcpy(tmp, path, pl); memcpy(tmp + pl, ".tmp", 5);
    int rc = OMEGA_VCS_VCSTORE_IO;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = fwrite(w.p, 1, w.n, f) == w.n && fflush(f) == 0 && fsync(fileno(f)) == 0;
        ok = (fclose(f) == 0) && ok;
        if (ok && rename(tmp, path) == 0) rc = OMEGA_VCS_OK;
        else remove(tmp);
    }
    free(tmp); free(w.p);
    return rc;
}

typedef struct { const uint8_t *b; size_t n, pos; int bad; } rdr_t;

static const uint8_t *r_take(rdr_t *r, size_t k) {
    if (r->bad || r->n - r->pos < k) { r->bad = 1; return NULL; }
    const uint8_t *p = r->b + r->pos; r->pos += k;
    return p;
}
static uint64_t r_u64(rdr_t *r) {
    const uint8_t *p = r_take(r, 8);
    if (!p) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

/* Append an object that is already known to sort after every stored one. */
static int vcs_append(OmegaVcStore *s, const OmegaVcRecord *rec, uint8_t kind, const uint8_t id[32]) {
    uint8_t *cp = malloc(rec->canonical_len);
    if (!cp) return OMEGA_VCS_VCSTORE_NOMEM;
    memcpy(cp, rec->canonical, rec->canonical_len);
    if (s->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        if (nc > SIZE_MAX / sizeof *s->objs) { free(cp); return OMEGA_VCS_VCSTORE_NOMEM; }
        OmegaVcObject *n = realloc(s->objs, nc * sizeof *s->objs);
        if (!n) { free(cp); return OMEGA_VCS_VCSTORE_NOMEM; }
        s->objs = n; s->cap = nc;
    }
    OmegaVcObject *o = &s->objs[s->count++];
    memcpy(o->semantic_id, rec->vc.semantic_id, 32);
    memcpy(o->vc_id, id, 32);
    o->admission_kind = kind;
    o->bytes = cp;
    o->len = rec->canonical_len;
    return OMEGA_VCS_OK;
}

static int parse_store(OmegaVcStore *t, const uint8_t *b, size_t n) {
    rdr_t r = { b, n, 0, 0 };
    const uint8_t *m = r_take(&r, FILE_MAGIC_LEN);
    if (!m || memcmp(m, FILE_MAGIC, FILE_MAGIC_LEN) != 0) return OMEGA_VCS_VCSTORE_MALFORMED; /* VC1S:load-magic */
    uint64_t count = r_u64(&r);
    if (r.bad || count > (n - r.pos) / 9) return OMEGA_VCS_VCSTORE_MALFORMED;
    OmegaVcRecord *rec = malloc(sizeof *rec);
    if (!rec) return OMEGA_VCS_VCSTORE_NOMEM;
    int rc = OMEGA_VCS_OK;
    for (uint64_t i = 0; i < count && !rc; i++) {
        const uint8_t *kp = r_take(&r, 1);
        uint64_t len = r_u64(&r);
        if (r.bad || len > n - r.pos) { rc = OMEGA_VCS_VCSTORE_MALFORMED; break; }
        uint8_t kind = *kp;
        if (kind != OMEGA_VCS_ADMISSION_VERIFIED && kind != OMEGA_VCS_ADMISSION_BOOTSTRAP) { rc = OMEGA_VCS_VCSTORE_MALFORMED; break; } /* VC1S:load-kind */
        const uint8_t *ob = r_take(&r, (size_t)len);
        rc = omega_vc_decode(ob, (size_t)len, &rec->vc);
        if (rc) break;
        if (t->count > 0 && memcmp(t->objs[t->count - 1].semantic_id, rec->vc.semantic_id, 32) >= 0) { rc = OMEGA_VCS_VCSTORE_MALFORMED; break; } /* VC1S:load-order */
        rec->canonical = ob;
        rec->canonical_len = (size_t)len;
        uint8_t id[32];
        omega_vc_compute_id(ob, (size_t)len, id);
        rc = vcs_append(t, rec, kind, id);
    }
    free(rec);
    if (rc) return rc;
    uint64_t nn = r_u64(&r);
    if (r.bad || nn > (n - r.pos) / 41) return OMEGA_VCS_VCSTORE_MALFORMED;
    for (uint64_t i = 0; i < nn && !rc; i++) {
        uint64_t l = r_u64(&r);
        if (r.bad || l == 0 || l > MAX_NAME) return OMEGA_VCS_VCSTORE_MALFORMED;
        const uint8_t *nb = r_take(&r, (size_t)l);
        const uint8_t *id = r_take(&r, 32);
        if (!nb || !id) return OMEGA_VCS_VCSTORE_MALFORMED;
        char name[MAX_NAME + 1];
        memcpy(name, nb, (size_t)l);
        name[l] = 0;
        if (strlen(name) != l || !name_valid(name)) return OMEGA_VCS_VCSTORE_MALFORMED;
        size_t di;
        if (!vcs_find(t, id, &di)) return OMEGA_VCS_UNVERIFIED_DEPENDENCY; /* VC1S:load-name-id */
        if (t->n_names > 0 && strcmp(t->names[t->n_names - 1].name, name) >= 0) return OMEGA_VCS_VCSTORE_MALFORMED;
        rc = name_insert_at(t, t->n_names, name, id); /* VC1S:load-names */
    }
    if (rc) return rc;
    const uint8_t *tr = r_take(&r, 64);
    if (!tr) return OMEGA_VCS_VCSTORE_MALFORMED;
    if (r.pos != r.n) return OMEGA_VCS_VCSTORE_MALFORMED; /* VC1S:load-trailing */
    rc = vcs_graph_check(t); /* VC1S:load-graph */
    if (rc) return rc;
    uint8_t d1[32], d2[32];
    omega_vcstore_digest(t, d1);
    omega_vcstore_name_index_digest(t, d2);
    if (memcmp(d1, tr, 32) != 0) return OMEGA_VCS_VCSTORE_ID_MISMATCH; /* VC1S:load-digest-obj */
    if (memcmp(d2, tr + 32, 32) != 0) return OMEGA_VCS_VCSTORE_ID_MISMATCH; /* VC1S:load-digest-names */
    return OMEGA_VCS_OK;
}

int omega_vcstore_load(OmegaVcStore *s, const char *path) {
    if (!s || !path) return OMEGA_VCS_VCSTORE_MALFORMED;
    FILE *f = fopen(path, "rb");
    if (!f) return OMEGA_VCS_VCSTORE_IO;
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    int rc = OMEGA_VCS_OK;
    for (;;) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 4096;
            uint8_t *nb = realloc(buf, nc);
            if (!nb) { rc = OMEGA_VCS_VCSTORE_NOMEM; break; }
            buf = nb; cap = nc;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) { if (ferror(f)) rc = OMEGA_VCS_VCSTORE_IO; break; }
    }
    fclose(f);
    if (rc) { free(buf); return rc; }
    OmegaVcStore t;
    omega_vcstore_init(&t);
    rc = parse_store(&t, buf ? buf : ZERO32, n);
    free(buf);
    if (rc) {
        omega_vcstore_destroy(&t); /* VC1S:load-fail */
        return rc;
    }
    omega_vcstore_destroy(s);
    *s = t;
    return OMEGA_VCS_OK;
}
