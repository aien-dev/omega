/* omega_vc_bridge.c -- see omega_vc_bridge.h. */
#include "omega_vc_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_program_ir.h"
#include "omega_receipt.h"
#include "sha256.h"

#define PROFILE "host-v1"
#define PROFILE_VERSION "1.0.0"

int omega_vc_bridge_init(OmegaVcBridge *b)
{
    if (!b) return -1;
    memset(b, 0, sizeof *b);
    return omega_vcstore_init(&b->store) == 0 ? 0 : -1;
}

void omega_vc_bridge_destroy(OmegaVcBridge *b)
{
    if (!b) return;
    for (size_t i = 0; i < b->n_receipts; i++) free(b->receipts[i].json);
    for (size_t i = 0; i < b->n_blobs; i++) free(b->blobs[i].bytes);
    free(b->receipts);
    free(b->blobs);
    omega_vcstore_destroy(&b->store);
    memset(b, 0, sizeof *b);
}

static int fetch_copy(const uint8_t *p, size_t n, uint8_t **bytes, size_t *len)
{
    *bytes = malloc(n + 1);
    if (!*bytes) return 2;
    memcpy(*bytes, p, n);
    *len = n;
    return 0;
}
int omega_vc_bridge_fetch_receipt(void *ctx, const uint8_t id[32], uint8_t **bytes, size_t *len)
{
    const OmegaVcBridge *b = ctx;
    for (size_t i = 0; b && i < b->n_receipts; i++)
        if (memcmp(b->receipts[i].id, id, 32) == 0) return fetch_copy((const uint8_t *)b->receipts[i].json, b->receipts[i].len, bytes, len);
    return 1;
}
int omega_vc_bridge_fetch_blob(void *ctx, const uint8_t digest[32], uint8_t **bytes, size_t *len)
{
    const OmegaVcBridge *b = ctx;
    for (size_t i = 0; b && i < b->n_blobs; i++)
        if (memcmp(b->blobs[i].digest, digest, 32) == 0) return fetch_copy(b->blobs[i].bytes, b->blobs[i].len, bytes, len);
    return 1;
}

static void hex32(const uint8_t *in, char out[65])
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[in[i] >> 4]; out[2 * i + 1] = h[in[i] & 15]; }
    out[64] = 0;
}

/* the receipt JSON, with idhex in the "id" field. dep_hex is n x 65 chars. Returns bytes written or -1. */
static int emit_receipt(char *o, size_t cap, const char *idhex, const char *sid, const char *dig, const char *root,
                        const char (*dep_hex)[65], size_t nd)
{
    size_t n = 0;
#define PF(...) do { int w_ = snprintf(o + n, cap - n, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= cap - n) return -1; n += (size_t)w_; } while (0)
    PF("{\n\"schema\": \"EvidenceReceiptV1\",\n\"version\": 1,\n\"id\": \"%s\",\n\"kind\": \"" PROFILE "\",\n", idhex);
    PF("\"tier\": \"HOST_TEST\",\n\"result\": \"PASS\",\n\"timestamp\": 1,\n");
    PF("\"repo\": \"omega-vc-bridge\",\n\"commit\": \"0000000000000000000000000000000000000000\",\n\"dirty\": false,\n");
    PF("\"toolchain\": \"omega-vc-bridge\",\n\"procedure\": \"in-process omega_program_verify and program id recompute (self-minted HOST_TEST receipt, not an independent run)\",\n");
    PF("\"machine\": \"host\",\n\"env_class\": \"HOST_TEST\",\n");
    PF("\"input_artifacts\": [\"sha256:%s\", \"sha256:%s\"],\n\"output_artifacts\": [],\n", sid, dig);
    PF("\"assertions\": [{\"id\": \"omega_program_verify\", \"expected\": \"pass\", \"observed\": \"pass\", \"pass\": true, \"source\": \"omega_vc_bridge\", \"note\": \"\"}],\n");
    PF("\"dependencies\": [");
    for (size_t i = 0; i < nd; i++) PF("%s\"%s\"", i ? ", " : "", dep_hex[i]);
    PF("],\n\"declared_mutation\": \"NONE\",\n\"observed_mutation\": \"NONE\",\n\"authority\": \"\",\n\"output_digest\": \"%s\",\n\"external_refs\": [],\n\"ledger\": null,\n\"lease\": null,\n\"required_features\": [],\n\"reserved\": \"\"\n}\n", root);
#undef PF
    return (int)n;
}

typedef struct { uint8_t *p; size_t n, cap; int bad; } Out;
static void oput(Out *o, const void *d, size_t k)
{
    if (o->bad) return;
    if (o->n + k > o->cap) {
        size_t nc = (o->n + k) * 2 + 64;
        uint8_t *t = realloc(o->p, nc);
        if (!t) { o->bad = 1; return; }
        o->p = t; o->cap = nc;
    }
    memcpy(o->p + o->n, d, k);
    o->n += k;
}
static void ou32(Out *o, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; oput(o, t, 4); }
static void ostr(Out *o, const char *s)
{
    size_t n = strlen(s);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)((uint64_t)n >> (56 - 8 * i));
    oput(o, l, 8);
    oput(o, s, n);
}

static int put_receipt(OmegaVcBridge *b, const uint8_t id[32], const char *json, size_t len)
{
    for (size_t i = 0; i < b->n_receipts; i++) if (memcmp(b->receipts[i].id, id, 32) == 0) return 0;
    if (b->n_receipts == b->cap_receipts) {
        size_t nc = b->cap_receipts ? b->cap_receipts * 2 : 16;
        OmegaVcBridgeReceipt *t = realloc(b->receipts, nc * sizeof *t);
        if (!t) return -1;
        b->receipts = t; b->cap_receipts = nc;
    }
    char *c = malloc(len + 1);
    if (!c) return -1;
    memcpy(c, json, len); c[len] = 0;
    memcpy(b->receipts[b->n_receipts].id, id, 32);
    b->receipts[b->n_receipts].json = c;
    b->receipts[b->n_receipts].len = len;
    b->n_receipts++;
    return 0;
}
static int put_blob(OmegaVcBridge *b, const uint8_t digest[32], const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < b->n_blobs; i++) if (memcmp(b->blobs[i].digest, digest, 32) == 0) return 0;
    if (b->n_blobs == b->cap_blobs) {
        size_t nc = b->cap_blobs ? b->cap_blobs * 2 : 16;
        OmegaVcBridgeBlob *t = realloc(b->blobs, nc * sizeof *t);
        if (!t) return -1;
        b->blobs = t; b->cap_blobs = nc;
    }
    uint8_t *c = malloc(len ? len : 1);
    if (!c) return -1;
    memcpy(c, bytes, len);
    memcpy(b->blobs[b->n_blobs].digest, digest, 32);
    b->blobs[b->n_blobs].bytes = c;
    b->blobs[b->n_blobs].len = len;
    b->n_blobs++;
    return 0;
}

static int cmp_dep(const void *a, const void *b) { return memcmp(a, b, 32); }

int omega_vc_bridge_admit(OmegaVcBridge *b, OmegaLibrary *lib, const OmegaProgram *prog,
                          const SemanticId *deps, size_t dep_count, const uint8_t evidence[32])
{
    static const uint8_t zero[32] = { 0 };
    if (!b || !lib || !prog || !evidence || memcmp(evidence, zero, 32) == 0 || dep_count > OMEGA_LIB_MAX_DEPS || (dep_count && !deps)) return -1;
    if (!prog->is_realized || !prog->body.has_body || memcmp(prog->program_id.bytes, zero, 32) == 0) return -1;
    int rc = -1;
    uint8_t *ir = NULL, *vc = NULL;
    size_t ir_len = 0;
    uint8_t digest[32], contract[32], root[32], evid[32];
    uint8_t (*dep_sorted)[32] = NULL, (*dep_con)[32] = NULL, (*dep_rcpt)[32] = NULL;
    char (*dep_hex)[65] = NULL;
    char *json = NULL;
    Out o = { NULL, 0, 0, 0 };
    OmegaVcRecord *rec = NULL;
    OmegaProgram *cp = NULL;

    /* The bridge checks for itself; it never trusts the caller's is_verified flag or program_id. It
     * verifies a private copy (omega_program_verify writes is_verified) and recomputes the id from the
     * IR it is about to store. */
    cp = malloc(sizeof *cp);
    if (!cp) goto done;
    *cp = *prog;
    {   VerifyReport rep;
        if (omega_program_verify(cp, &rep) != 0) goto done; /* VC1B:verify */
    }
    if (omega_program_ir_encode(prog, &ir, &ir_len) != 0) goto done;
    {   uint8_t rid_[32];
        if (omega_program_ir_recompute_id(ir, ir_len, rid_) != 0 || memcmp(rid_, prog->program_id.bytes, 32) != 0) goto done; /* VC1B:id */
    }
    sha256_hash(ir, ir_len, digest);
    if (omega_program_contract_id(prog, contract) != 0) goto done;
    {   /* evidence root binds the caller's evidence digest to this program */
        static const uint8_t dom[] = "aien.vc1.bridge.evidence.v1";
        uint8_t buf[sizeof dom + 64];
        memcpy(buf, dom, sizeof dom);
        memcpy(buf + sizeof dom, evidence, 32);
        memcpy(buf + sizeof dom + 32, prog->program_id.bytes, 32);
        sha256_hash(buf, sizeof dom + 64, root);
        memcpy(evid, root, 32);
    }
    size_t nd = dep_count;
    dep_sorted = malloc((nd ? nd : 1) * 32);
    dep_con = malloc((nd ? nd : 1) * 32);
    dep_rcpt = malloc((nd ? nd : 1) * 32);
    dep_hex = malloc((nd ? nd : 1) * 65);
    rec = malloc(sizeof *rec);
    if (!dep_sorted || !dep_con || !dep_rcpt || !dep_hex || !rec) goto done;
    for (size_t i = 0; i < nd; i++) memcpy(dep_sorted[i], deps[i].bytes, 32);
    qsort(dep_sorted, nd, 32, cmp_dep);
    for (size_t i = 0; i < nd; i++) {
        if (i && memcmp(dep_sorted[i - 1], dep_sorted[i], 32) == 0) goto done;     /* duplicate dependency */
        if (omega_vcstore_get(&b->store, dep_sorted[i], rec) != 0) goto done;     /* must be admitted already */
        memcpy(dep_con[i], rec->vc.contract_id, 32);
        memcpy(dep_rcpt[i], rec->vc.receipt_id, 32);
        hex32(dep_rcpt[i], dep_hex[i]);
    }
    char sid_h[65], dig_h[65], root_h[65], zero_h[65];
    hex32(prog->program_id.bytes, sid_h);
    hex32(digest, dig_h);
    hex32(root, root_h);
    memset(zero_h, '0', 64); zero_h[64] = 0;
    json = malloc(8192);
    if (!json) goto done;
    int n0 = emit_receipt(json, 8192, zero_h, sid_h, dig_h, root_h, (const char (*)[65])dep_hex, nd);
    if (n0 < 0) goto done;
    uint8_t rid[32];
    {
        OmegaReceipt r;
        char err[200];
        if (omega_receipt_parse((const uint8_t *)json, (size_t)n0, &r, err, sizeof err) != 0) goto done;
        int cr = omega_receipt_compute_id(&r, rid);
        omega_receipt_free(&r);
        if (cr != 0) goto done;
    }
    char rid_h[65];
    hex32(rid, rid_h);
    int n1 = emit_receipt(json, 8192, rid_h, sid_h, dig_h, root_h, (const char (*)[65])dep_hex, nd);
    if (n1 < 0) goto done;
    /* the canonical Verified Crumb */
    oput(&o, "AIEN_VERIFIED_CRUMB_V1", 22);
    ou32(&o, OMEGA_VC_FORMAT_VERSION);
    oput(&o, prog->program_id.bytes, 32);
    oput(&o, contract, 32);
    { uint8_t k = OMEGA_VC_DIGEST_IR; oput(&o, &k, 1); }
    oput(&o, digest, 32);
    ou32(&o, 0);                                   /* realizations */
    ou32(&o, (uint32_t)nd);
    for (size_t i = 0; i < nd; i++) { oput(&o, dep_sorted[i], 32); oput(&o, dep_con[i], 32); }
    oput(&o, rid, 32);
    ostr(&o, PROFILE);
    ostr(&o, PROFILE_VERSION);
    oput(&o, evid, 32);
    ou32(&o, 0);                                   /* exports */
    ou32(&o, 1); ostr(&o, OMEGA_BRIDGE_SELFMINTED_CAPABILITY);   /* capabilities: one, so the build domain refuses this record */ /* VC1B:cap */
    if (o.bad) goto done;
    vc = o.p;
    uint8_t vcid[32];
    omega_vc_compute_id(vc, o.n, vcid);
    if (put_receipt(b, rid, json, (size_t)n1) != 0 || put_blob(b, digest, ir, ir_len) != 0) goto done;
    OmegaResolver r;
    memset(&r, 0, sizeof r);
    r.store = &b->store;
    r.domain = OMEGA_DOMAIN_BUILD;
    r.fetch_receipt = omega_vc_bridge_fetch_receipt;
    r.fetch_ctx = b;
    r.fetch_blob = omega_vc_bridge_fetch_blob;
    r.blob_ctx = b;
    OmegaResolveError err;
    if (omega_resolve_admit(&r, &b->store, vc, o.n, vcid, NULL, &err) != 0) goto done;
    /* store first, library second: the library entry carries the receipt id as its evidence hash.
     * Insert the copy the bridge verified itself (the library copies it by value); the caller's
     * is_verified flag is never consulted. */
    rc = omega_library_insert(lib, cp, deps, dep_count, rid);
done:
    free(ir);
    free(vc);
    free(dep_sorted); free(dep_con); free(dep_rcpt); free(dep_hex);
    free(json);
    free(rec);
    free(cp);
    return rc;
}

int omega_vc_bridge_admit_abstraction(OmegaVcBridge *b, OmegaLibrary *lib, const OmegaAbstractionCandidate *cand,
                                      const uint8_t evidence[32])
{
    if (!b || !lib || !cand || !evidence) return -1;
    if (!cand->is_verified || !cand->is_nontrivial || cand->compression_score <= 0) return -1;
    return omega_vc_bridge_admit(b, lib, &cand->abstraction, NULL, 0, evidence);
}
