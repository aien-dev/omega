/* test_omega_resolve.c -- VC1 stage 4: import resolution in the Omega OSC compiler.
 *
 * Usage (run from the repository root):
 *   test_omega_resolve            run every check; exit 0 only if all pass
 *   test_omega_resolve <mutant>   the binary was linked against a deliberately broken copy of
 *                                 src/omega_resolve.c, src/omega_receipt.c or src/omega_blake3.c
 *                                 (built by `make test-resolve`); run every check and require the
 *                                 mapped check to FAIL. Exit 1 = killed (what the Makefile
 *                                 asserts), 3 = survived, 2 = unknown mutant.
 *
 * What is NOT hardcoded: BLAKE3 outputs come from the official test vectors
 * (tests/resolve/blake3_vectors.txt) and from three receipts written by the Rust implementation
 * (tests/resolve/receipts, copied from aien-sovereign-core fixtures); Verified Crumbs are built by
 * a test-side encoder that shares no code with the decoder; the closure digest and the build id
 * are re-derived here by a second implementation of the formulas; every refusal code is provoked
 * by a real compile of an OSC fixture program through the front end and the resolver.
 * Receipts built here are emitted as JSON text and their ids are computed by the library under
 * test, which is anchored by the Rust receipts above.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "omega_blake3.h"
#include "omega_receipt.h"
#include "omega_resolve.h"
#include "omega_resolve_osc.h"
#include "omega_vcstore.h"
#include "osc_front.h"
#include "osc_cg.h"
#include "sha256.h"

#ifndef OSCV_PATH
#define OSCV_PATH "build/compiler/oscv"
#endif

static const char *g_mutant, *g_expect_check;
static int g_failed, g_expect_failed, g_total;

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    g_total++;
    if (!ok) {
        g_failed++;
        if (g_expect_check && strcmp(name, g_expect_check) == 0) g_expect_failed = 1;
    }
}
static void setup_fail(const char *what) {
    if (g_mutant && g_expect_failed) { printf("MUTANT %s KILLED by %s (a later fixture then failed: %s)\n", g_mutant, g_expect_check, what); exit(1); }
    if (g_mutant) { printf("MUTANT %s SETUP-BROKEN before check %s ran (%s)\n", g_mutant, g_expect_check, what); exit(4); }
    fprintf(stderr, "setup failed: %s\n", what);
    exit(2);
}

/* ---- byte and hex helpers ---- */
typedef struct { uint8_t *p; size_t n, cap; } B;
static void bput(B *b, const void *d, size_t k) {
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) setup_fail("oom"); }
    if (k) memcpy(b->p + b->n, d, k);
    b->n += k;
}
static void bu32(B *b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, t, 4); }
static void bu64(B *b, uint64_t v) { bu32(b, (uint32_t)(v >> 32)); bu32(b, (uint32_t)v); }
static void bstr(B *b, const char *s) { size_t n = strlen(s); bu64(b, n); bput(b, s, n); }

static void hx(const uint8_t *d, char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}
static void mkid(uint8_t out[32], uint8_t b0) { memset(out, 0x77, 32); out[0] = b0; }
static void con_of(const uint8_t sid[32], uint8_t out[32]) { memcpy(out, sid, 32); out[0] ^= 0x80; }

/* ---- test-side Verified Crumb encoder (writes lists exactly as given) ---- */
typedef struct {
    uint32_t ver;
    uint8_t sem[32], con[32], src[32], rcpt[32], root[32];
    uint8_t kind;
    uint32_t n_dep;  const uint8_t (*dep)[64];
    const char *prof, *pver;
    uint32_t n_cap; const char *const *cap;
} Spec;
static void encode(B *o, const Spec *s) {
    o->n = 0;
    bput(o, "AIEN_VERIFIED_CRUMB_V1", 22);
    bu32(o, s->ver);
    bput(o, s->sem, 32); bput(o, s->con, 32); bput(o, &s->kind, 1); bput(o, s->src, 32);
    bu32(o, 0);
    bu32(o, s->n_dep);  for (uint32_t i = 0; i < s->n_dep; i++) bput(o, s->dep[i], 64);
    bput(o, s->rcpt, 32); bstr(o, s->prof); bstr(o, s->pver); bput(o, s->root, 32);
    bu32(o, 0);
    bu32(o, s->n_cap); for (uint32_t i = 0; i < s->n_cap; i++) bstr(o, s->cap[i]);
}

/* ---- receipt JSON emitter ---- */
typedef struct {
    const char *kind, *tier, *env_class, *result, *schema, *decl_mut, *obs_mut, *procedure, *reserved, *feature;
    int version;
    const char *inputs[4]; int n_in;
    const char *deps[4]; int n_dep;
    char root[65];
    int assert_pass, extra_key, dup_key, lease, no_assertion, dirty;
    const char *authority;
} RJ;
static void rj_default(RJ *j) {
    memset(j, 0, sizeof *j);
    j->kind = "host-v1"; j->tier = "HOST_TEST"; j->env_class = NULL; j->result = "PASS";
    j->schema = "EvidenceReceiptV1"; j->decl_mut = "NONE"; j->obs_mut = "NONE";
    j->procedure = "fixture"; j->authority = ""; j->reserved = ""; j->feature = NULL; j->version = 1; j->assert_pass = 1;
    memset(j->root, '6', 64); j->root[64] = 0;
}
static char *rj_emit(const RJ *j, const char *idhex, size_t *len) {
    size_t cap = 16384, n = 0;
    char *o = malloc(cap);
    if (!o) setup_fail("oom");
#define PF(...) do { n += (size_t)snprintf(o + n, cap - n, __VA_ARGS__); if (n >= cap - 64) setup_fail("receipt too long"); } while (0)
    PF("{\n\"schema\": \"%s\",\n\"version\": %d,\n\"id\": \"%s\",\n\"kind\": \"%s\",\n", j->schema, j->version, idhex, j->kind);
    if (j->dup_key) PF("\"kind\": \"%s\",\n", j->kind);
    if (j->extra_key) PF("\"surprise\": 1,\n");
    PF("\"tier\": \"%s\",\n\"result\": \"%s\",\n\"timestamp\": 1786000000,\n", j->tier, j->result);
    PF("\"repo\": \"fixture\",\n\"commit\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n\"dirty\": %s,\n", j->dirty ? "true" : "false");
    PF("\"toolchain\": \"fixture\",\n\"procedure\": \"%s\",\n\"machine\": \"host\",\n\"env_class\": \"%s\",\n", j->procedure, j->env_class ? j->env_class : j->tier);
    PF("\"input_artifacts\": [");
    for (int i = 0; i < j->n_in; i++) PF("%s\"%s\"", i ? ", " : "", j->inputs[i]);
    PF("],\n\"output_artifacts\": [],\n");
    if (j->no_assertion) PF("\"assertions\": [],\n");
    else PF("\"assertions\": [{\"id\": \"fixture_check\", \"expected\": \"pass\", \"observed\": \"%s\", \"pass\": %s, \"source\": \"test\", \"note\": \"\"}],\n",
            j->assert_pass ? "pass" : "fail", j->assert_pass ? "true" : "false");
    PF("\"dependencies\": [");
    for (int i = 0; i < j->n_dep; i++) PF("%s\"%s\"", i ? ", " : "", j->deps[i]);
    PF("],\n\"declared_mutation\": \"%s\",\n\"observed_mutation\": \"%s\",\n\"authority\": \"%s\",\n\"output_digest\": \"%s\",\n\"external_refs\": [],\n\"ledger\": null,\n",
       j->decl_mut, j->obs_mut, j->authority, j->root);
    if (j->lease) PF("\"lease\": {\"hold_id\": \"%s\", \"resource\": \"fixture-lease\"},\n", "1111111111111111111111111111111111111111111111111111111111111111");
    else PF("\"lease\": null,\n");
    if (j->feature) PF("\"required_features\": [\"%s\"],\n", j->feature); else PF("\"required_features\": [],\n");
    PF("\"reserved\": \"%s\"\n}\n", j->reserved);
#undef PF
    *len = n;
    return o;
}
/* emit with a placeholder id, let the library compute the id, emit again with the real id */
static int rj_make(const RJ *j, uint8_t id[32], char **json, size_t *len) {
    char zero[65]; memset(zero, '0', 64); zero[64] = 0;
    size_t l0; char *t = rj_emit(j, zero, &l0);
    OmegaReceipt r; char err[200];
    if (omega_receipt_parse((const uint8_t *)t, l0, &r, err, sizeof err)) { free(t); return -1; }
    free(t);
    int rc = omega_receipt_compute_id(&r, id);
    omega_receipt_free(&r);
    if (rc) return -1;
    char h[65]; hx(id, h);
    *json = rj_emit(j, h, len);
    return 0;
}

/* ---- in-memory receipt store ---- */
typedef struct { uint8_t id[32]; char *p; size_t n; } Rb;
typedef struct { Rb *v; size_t n; } RTab;
static void tab_put(RTab *t, const uint8_t id[32], const char *p, size_t n) {
    t->v = realloc(t->v, (t->n + 1) * sizeof *t->v);
    if (!t->v) setup_fail("oom");
    memcpy(t->v[t->n].id, id, 32);
    t->v[t->n].p = malloc(n + 1);
    if (!t->v[t->n].p) setup_fail("oom");
    memcpy(t->v[t->n].p, p, n);
    t->v[t->n].n = n;
    t->n++;
}
static int tab_fetch(void *ctx, const uint8_t id[32], uint8_t **bytes, size_t *len) {
    RTab *t = ctx;
    for (size_t i = 0; i < t->n; i++)
        if (memcmp(t->v[i].id, id, 32) == 0) {
            *bytes = malloc(t->v[i].n + 1);
            if (!*bytes) return 2;
            memcpy(*bytes, t->v[i].p, t->v[i].n);
            *len = t->v[i].n;
            return 0;
        }
    return 1;
}

/* ---- the world: a store, receipts, and a resolver over them ---- */
typedef struct {
    OmegaVcStore *st;
    RTab rt;
    uint8_t last_rid[32];
    int allow_genesis;
} W;
static void w_init(W *w) {
    memset(w, 0, sizeof *w);
    w->st = calloc(1, sizeof *w->st);
    if (!w->st || omega_vcstore_init(w->st) != 0) setup_fail("store");
}
static void w_free(W *w) {
    omega_vcstore_destroy(w->st); free(w->st);
    for (size_t i = 0; i < w->rt.n; i++) free(w->rt.v[i].p);
    free(w->rt.v);
}

typedef struct {
    const char *prof, *ver, *tier, *result;
    int assert_fail, skip_sid, stale_src, wrong_root, extra_rdep, wrong_kind, no_store, swap, taint, boot, lease, dirty;
} Opt;
static Opt opt0(void) {
    Opt o; memset(&o, 0, sizeof o);
    o.prof = "host-v1"; o.ver = "1.0.0"; o.tier = "HOST_TEST"; o.result = "PASS";
    return o;
}
static int ins_spec(OmegaVcStore *st, const Spec *sp, int boot) {
    B b = { 0, 0, 0 };
    encode(&b, sp);
    uint8_t id[32];
    omega_vc_compute_id(b.p, b.n, id);
    int rc = boot ? omega_vcstore_insert_bootstrap(st, b.p, b.n, id) : omega_vcstore_insert(st, b.p, b.n, id);
    free(b.p);
    return rc;
}
static void spec_for(Spec *sp, uint8_t b0, const uint8_t (*deps)[32], uint32_t nd, uint8_t (*dbuf)[64]) {
    memset(sp, 0, sizeof *sp);
    sp->ver = 1; sp->kind = 1;
    mkid(sp->sem, b0); con_of(sp->sem, sp->con);
    memset(sp->src, 0x33, 32); sp->src[0] = b0;
    memset(sp->root, 0x66, 32); sp->root[0] = b0;
    memset(sp->rcpt, 0x55, 32);
    for (uint32_t i = 0; i < nd; i++) { memcpy(dbuf[i], deps[i], 32); con_of(deps[i], dbuf[i] + 32); }
    sp->n_dep = nd; sp->dep = (const uint8_t (*)[64])dbuf;
    sp->prof = "host-v1"; sp->pver = "1.0.0";
}
/* add one component; returns the insert result. Its receipt id is left in w->last_rid. */
static int add_comp(W *w, uint8_t b0, const uint8_t (*deps)[32], uint32_t nd, const Opt *o) {
    uint8_t dbuf[8][64];
    Spec sp;
    spec_for(&sp, b0, deps, nd, dbuf);
    sp.prof = o->prof; sp.pver = o->ver;
    const char *taintcap[1] = { OMEGA_TAINT_CAPABILITY };
    if (o->taint) { sp.n_cap = 1; sp.cap = taintcap; }
    if (o->boot) {
        memset(sp.rcpt, 0xA0, 32); sp.rcpt[0] = b0;
        memcpy(w->last_rid, sp.rcpt, 32);
        return ins_spec(w->st, &sp, 1);
    }
    char sid_h[65], src_h[65], oth_h[65], root_h[65], inp[4][80], dep_h[8][65];
    hx(sp.sem, sid_h); hx(sp.src, src_h);
    uint8_t other[32]; memset(other, 0x99, 32); hx(other, oth_h);
    uint8_t root[32]; memcpy(root, sp.root, 32); if (o->wrong_root) root[5] ^= 1;
    hx(root, root_h);
    RJ j; rj_default(&j);
    j.kind = o->wrong_kind ? "some-other-kind" : o->prof;
    j.dirty = o->dirty;
    j.tier = o->tier; j.result = o->result; j.assert_pass = !o->assert_fail; j.lease = o->lease;
    memcpy(j.root, root_h, 65);
    if (!o->skip_sid) { snprintf(inp[j.n_in], 80, "sha256:%s", sid_h); j.inputs[j.n_in] = inp[j.n_in]; j.n_in++; }
    snprintf(inp[j.n_in], 80, "sha256:%s", o->stale_src ? oth_h : src_h); j.inputs[j.n_in] = inp[j.n_in]; j.n_in++;
    for (uint32_t i = 0; i < nd; i++) {
        uint8_t r[32];
        if (omega_vcstore_receipt_of(w->st, deps[i], r) != 0) setup_fail("dependency not in store");
        hx(r, dep_h[j.n_dep]); j.deps[j.n_dep] = dep_h[j.n_dep]; j.n_dep++;
    }
    char extra[65]; memset(extra, 'a', 64); extra[64] = 0;
    if (o->extra_rdep) j.deps[j.n_dep++] = extra;
    uint8_t rid[32]; char *json; size_t jl;
    if (rj_make(&j, rid, &json, &jl)) setup_fail("receipt fixture rejected by the library");
    memcpy(w->last_rid, rid, 32);
    memcpy(sp.rcpt, rid, 32);
    if (o->swap) {            /* the file stored under rid holds another, internally valid receipt */
        RJ k = j; k.procedure = "swapped"; uint8_t rid2[32]; char *json2; size_t jl2;
        if (rj_make(&k, rid2, &json2, &jl2)) setup_fail("swap receipt");
        tab_put(&w->rt, rid, json2, jl2); free(json2);
    } else if (!o->no_store) tab_put(&w->rt, rid, json, jl);
    free(json);
    return ins_spec(w->st, &sp, 0);
}
/* replace the stored bytes of an existing object (tampering; mirrors test_omega_vcstore.c) */
static void replace_obj(W *w, uint8_t b0, const Spec *sp) {
    uint8_t sid[32]; mkid(sid, b0);
    for (size_t i = 0; i < w->st->count; i++)
        if (memcmp(w->st->objs[i].semantic_id, sid, 32) == 0) {
            B b = { 0, 0, 0 };
            encode(&b, sp);
            free(w->st->objs[i].bytes);
            w->st->objs[i].bytes = b.p; w->st->objs[i].len = b.n;
            omega_vc_compute_id(b.p, b.n, w->st->objs[i].vc_id);
            return;
        }
    setup_fail("replace_obj: no such object");
}

/* ---- lock text and compiling a fixture through the real front end ---- */
typedef struct { const char *name; uint8_t b0; int wrong_receipt; } Pin;
static char g_lock[8192];
static const char *lock_text(const W *w, const Pin *pins, size_t n) {
    size_t at = (size_t)snprintf(g_lock, sizeof g_lock, "omega.lock v1\n");
    for (size_t i = 0; i < n; i++) {
        uint8_t sid[32], rid[32]; char a[65], b[65];
        mkid(sid, pins[i].b0);
        if (omega_vcstore_receipt_of(w->st, sid, rid) != 0) memset(rid, 0x42, 32);
        if (pins[i].wrong_receipt) rid[3] ^= 1;
        hx(sid, a); hx(rid, b);
        at += (size_t)snprintf(g_lock + at, sizeof g_lock - at, "%s = semantic %s receipt %s\n", pins[i].name, a, b);
    }
    return g_lock;
}
typedef struct {
    OscUnit *u;
    OscDiag dg;
    OmegaOscImports h;
    OmegaLock lock;
    int rc, lock_rc;
} Run;
static const char *FN = "fn add(a: u32, b: u32) -> u32 { return a + b; }\n";
static void run_compile(W *w, const char *src, OmegaDomain d, const char *locktext, Run *r) {
    memset(r, 0, sizeof *r);
    r->u = malloc(sizeof *r->u);
    if (!r->u) setup_fail("oom");
    OmegaResolveError e;
    if (locktext) {
        r->lock_rc = omega_lock_parse(locktext, strlen(locktext), &r->lock, &e);
        if (r->lock_rc) setup_fail("fixture lock did not parse");
        r->h.lock = &r->lock;
    }
    r->h.resolver.store = w->st;
    r->h.resolver.domain = d;
    r->h.resolver.fetch_receipt = tab_fetch;
    r->h.resolver.fetch_ctx = &w->rt;
    r->h.resolver.allow_genesis = w->allow_genesis;
    r->rc = osc_compile_imports(src, strlen(src), r->u, &r->dg, NULL, omega_osc_import_hook, &r->h);
}
static void run_free(Run *r) {
    omega_closure_free(&r->h.closure);
    omega_lock_free(&r->lock);
    free(r->u);
}
static int refused(const Run *r, const char *code, int code_enum) {
    return r->rc != 0 && r->dg.kind == OSC_DIAG_IMPORT_REFUSED && strcmp(r->dg.transition, code) == 0 && r->h.err.code == code_enum;
}
static int refused_msg(const Run *r, const char *code, int code_enum, const char *needle) {
    return refused(r, code, code_enum) && strstr(r->h.err.message, needle) != NULL;
}
static char g_src[512];
static const char *src_imports(const char *const *names, size_t n) {
    size_t at = 0;
    g_src[0] = 0;
    for (size_t i = 0; i < n; i++) at += (size_t)snprintf(g_src + at, sizeof g_src - at, "import %s;\n", names[i]);
    snprintf(g_src + at, sizeof g_src - at, "%s", FN);
    return g_src;
}
/* one component "foo" built with o, pinned correctly, imported by a fixture program */
static int one_foo(const Opt *o, OmegaDomain d, Run *r, W *w) {
    w_init(w);
    if (add_comp(w, 0x51, NULL, 0, o) != 0) setup_fail("one_foo insert");
    Pin p[1] = { { "foo", 0x51, 0 } };
    const char *nm[1] = { "foo" };
    const char *lk = lock_text(w, p, 1);
    run_compile(w, src_imports(nm, 1), d, lk, r);
    return r->rc;
}

/* ======================================================================== checks */
static uint8_t *slurp_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 14, k = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        k += fread(b + k, 1, cap - k, f);
        if (k < cap) break;
        cap *= 2; b = realloc(b, cap);
    }
    fclose(f);
    *n = k;
    return b;
}

/* ---- BLAKE3 ---- */
static void t_blake3(void) {
    FILE *f = fopen("tests/resolve/blake3_vectors.txt", "r");
    if (!f) setup_fail("tests/resolve/blake3_vectors.txt (run from the repository root)");
    size_t len; char want[80];
    int n = 0, bad = 0;
    while (fscanf(f, "%zu %64s", &len, want) == 2) {
        uint8_t *in = malloc(len + 1), out[32]; char got[65];
        if (!in) setup_fail("oom");
        for (size_t i = 0; i < len; i++) in[i] = (uint8_t)(i % 251);
        omega_blake3_hash(in, len, out);
        hx(out, got);
        n++;
        if (strcmp(got, want) != 0) bad++;
        free(in);
    }
    fclose(f);
    check("b3-official-vectors", n == 35 && bad == 0);
}

/* ---- receipts ---- */
static int rj_refused(const RJ *j) {
    char zero[65]; memset(zero, '0', 64); zero[64] = 0;
    size_t l; char *t = rj_emit(j, zero, &l);
    OmegaReceipt r; char err[200];
    int rc = omega_receipt_parse((const uint8_t *)t, l, &r, err, sizeof err);
    if (rc == 0) omega_receipt_free(&r);
    free(t);
    return rc != 0;
}
static void t_receipts(void) {
    static const char *const files[3] = { "54f597d22b3753e47d6bd7706b6a7a422c189024912ec4626347e860b5f44a8c",
        "7fdff014cb20a0af5b5a5b377532292be29fc15da36d434944ad4521e69de5b3",
        "8e885832de2e7123555c47803547161c02196646b270d41078d8b16d4beaf765" };
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        char path[256]; snprintf(path, sizeof path, "tests/resolve/receipts/%s.json", files[i]);
        size_t n; uint8_t *b = slurp_file(path, &n);
        if (!b) setup_fail("rust receipt fixture");
        OmegaReceipt r; uint8_t id[32]; char err[200], h[65];
        if (omega_receipt_verify_bytes(b, n, &r, id, err, sizeof err) == 0) {
            hx(id, h);
            if (strcmp(h, files[i]) == 0) ok++;
            omega_receipt_free(&r);
        }
        free(b);
    }
    check("receipt-rust-ids", ok == 3);

    uint8_t x[32], y[32]; char xh[80], yh[80];
    memset(x, 1, 32); memset(y, 2, 32);
    snprintf(xh, sizeof xh, "sha256:"); hx(x, xh + 7);
    snprintf(yh, sizeof yh, "sha256:"); hx(y, yh + 7);
    RJ a, b; rj_default(&a); rj_default(&b);
    a.inputs[0] = xh; a.inputs[1] = yh; a.n_in = 2;
    b.inputs[0] = yh; b.inputs[1] = xh; b.n_in = 2;
    uint8_t ida[32], idb[32]; char *ja, *jb; size_t la, lb;
    int mk = rj_make(&a, ida, &ja, &la) == 0 && rj_make(&b, idb, &jb, &lb) == 0;
    RJ c = a; c.procedure = "another procedure"; uint8_t idc[32]; char *jc; size_t lc;
    int mk2 = rj_make(&c, idc, &jc, &lc) == 0;
    check("receipt-canon-order-independent", mk && mk2 && memcmp(ida, idb, 32) == 0 && memcmp(ida, idc, 32) != 0);

    /* a receipt whose id field is not the hash of its content */
    if (!mk) setup_fail("receipt fixture");
    {
        char h[65]; hx(ida, h);
        OmegaReceipt r; uint8_t id[32]; char err[200];
        int good = omega_receipt_verify_bytes((const uint8_t *)ja, la, &r, id, err, sizeof err);
        if (good == 0) omega_receipt_free(&r);
        h[0] = h[0] == '0' ? '1' : '0';
        char *bad = rj_emit(&a, h, &lb);
        int rc = omega_receipt_verify_bytes((const uint8_t *)bad, lb, &r, id, err, sizeof err);
        if (rc == 0) omega_receipt_free(&r);
        free(bad);
        check("receipt-refuses-wrong-id", good == 0 && rc != 0);
    }
    RJ j;
    rj_default(&j); j.dup_key = 1;                         check("receipt-refuses-duplicate-key", rj_refused(&j));
    rj_default(&j); j.extra_key = 1;                       check("receipt-refuses-unknown-field", rj_refused(&j));
    rj_default(&j); j.schema = "EvidenceReceiptV2";        check("receipt-refuses-unknown-schema", rj_refused(&j));
    rj_default(&j); j.version = 2;                         check("receipt-refuses-unknown-version", rj_refused(&j));
    rj_default(&j); j.feature = "v2-extension";            check("receipt-refuses-unknown-feature", rj_refused(&j));
    rj_default(&j); j.reserved = "x";                      check("receipt-refuses-reserved-field", rj_refused(&j));
    rj_default(&j); j.env_class = "QEMU";                  check("receipt-refuses-env-class-mismatch", rj_refused(&j));
    rj_default(&j); j.obs_mut = "VOLATILE_ONLY";           check("receipt-refuses-observed-over-declared", rj_refused(&j));
    rj_default(&j); j.decl_mut = "VOLATILE_ONLY"; j.obs_mut = "VOLATILE_ONLY";
    check("receipt-control-valid-variant-parses", !rj_refused(&j));
    rj_default(&j); j.no_assertion = 1;                    check("receipt-refuses-pass-without-assertion", rj_refused(&j));
    rj_default(&j); j.no_assertion = 1; j.result = "FAIL"; check("receipt-control-fail-without-assertion-parses", !rj_refused(&j));
    rj_default(&j); j.tier = "PRODUCTION"; j.kind = "production-v1"; j.authority = "release-key-1";
    check("receipt-control-production-with-authority-parses", !rj_refused(&j));
    {
        int blank = 1, testonly = 1;
        static const char *const blanks[] = { "", " ", "   ", "\t" };
        static const char *const marks[] = { "TEST-ONLY signer", "a test_only key", "TestOnly", "test-only", "TEST_ONLY" };
        for (size_t i = 0; i < sizeof blanks / sizeof blanks[0]; i++) { rj_default(&j); j.tier = "PRODUCTION"; j.kind = "production-v1"; j.authority = blanks[i]; if (!rj_refused(&j)) blank = 0; }
        for (size_t i = 0; i < sizeof marks / sizeof marks[0]; i++) { rj_default(&j); j.tier = "PRODUCTION"; j.kind = "production-v1"; j.authority = marks[i]; if (!rj_refused(&j)) testonly = 0; }
        check("receipt-refuses-production-blank-authority", blank);
        check("receipt-refuses-production-test-only-authority", testonly);
    }
    free(ja); free(jb); free(jc);

    int tab_ok = 1;
    for (int need = -1; need <= 7; need++)
        for (int have = -1; have <= 7; have++) {
            int want = need >= 0 && have >= 0 && have >= need;
            if (omega_tier_satisfies(need, have) != want) tab_ok = 0;
        }
    check("tier-satisfies-table", tab_ok && omega_tier_rank("PRODUCTION") == 7 && omega_tier_rank("HOST_TEST") == 1 &&
                                  omega_tier_rank("TEST_ONLY_TRUST") == 0 && omega_tier_rank("nonsense") == -1);
}

/* ---- profiles ---- */
static void t_profiles(void) {
    size_t n; const OmegaVerifierProfile *t = omega_resolve_default_profiles(&n);
    OmegaResolveError e;
    check("profile-table-contents", n == 3 && strcmp(t[0].name, "host-v1") == 0 && strcmp(t[0].min_tier, "HOST_TEST") == 0 &&
          strcmp(t[1].name, "qemu-v1") == 0 && strcmp(t[1].min_tier, "QEMU") == 0 &&
          strcmp(t[2].name, "production-v1") == 0 && strcmp(t[2].min_tier, "PRODUCTION") == 0);
    check("profile-known-accepted", omega_resolve_check_profile(t, n, "host-v1", "1.0.0", &e) == 0);
    int rc = omega_resolve_check_profile(t, n, "mystery-v9", "1.0.0", &e);
    check("profile-unknown-refused", rc == OMEGA_RES_UNKNOWN_VERIFIER_PROFILE && e.code == rc);
    rc = omega_resolve_check_profile(t, n, "", "1.0.0", &e);
    check("profile-empty-name-refused", rc == OMEGA_RES_UNKNOWN_VERIFIER_PROFILE);
    static const char *const old[] = { "0.9.9", "0.99.99", "0", "0.0.0", "", "abc", "1.0.0-rc", "1..0", "1.0.", ".1", "1.0.x" };
    int old_ok = 1;
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
        if (omega_resolve_check_profile(t, n, "host-v1", old[i], &e) != OMEGA_RES_VERIFIER_TOO_OLD) old_ok = 0;
    check("profile-version-too-old-refused", old_ok);
    static const char *const fine[] = { "1.0.0", "1.0.1", "1.1", "1.0", "1", "2.0.0", "10.0.0", "1.0.0.0" };
    int fine_ok = 1;
    for (size_t i = 0; i < sizeof fine / sizeof fine[0]; i++)
        if (omega_resolve_check_profile(t, n, "host-v1", fine[i], &e) != 0) fine_ok = 0;
    check("profile-version-boundaries-accepted", fine_ok);
}

/* ---- the lock ---- */
static const char *H1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *H2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static int lock_refused(const char *text, size_t len) {
    OmegaLock l; OmegaResolveError e;
    int rc = omega_lock_parse(text, len, &l, &e);
    if (rc == 0) { omega_lock_free(&l); return 0; }
    return rc == OMEGA_RES_DEPENDENCY_NOT_PINNED && e.code == rc;
}
static void t_lock(void) {
    char buf[1024];
    OmegaLock l; OmegaResolveError e;
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s\nbeta.x-1_y = semantic %s receipt %s\n", H1, H2, H2, H1);
    int rc = omega_lock_parse(buf, strlen(buf), &l, &e);
    int ok = rc == 0 && l.n == 2 && strcmp(l.e[0].name, "alpha") == 0 && l.e[0].semantic[0] == 0xaa && l.e[0].receipt[0] == 0xbb &&
             omega_lock_find(&l, "beta.x-1_y") != NULL && omega_lock_find(&l, "gamma") == NULL && omega_lock_find(&l, "alph") == NULL;
    if (rc == 0) omega_lock_free(&l);
    check("lock-parses-valid", ok);
    rc = omega_lock_parse("omega.lock v1\n", 14, &l, &e);
    check("lock-parses-empty", rc == 0 && l.n == 0);
    if (rc == 0) omega_lock_free(&l);

    snprintf(buf, sizeof buf, "omega.lock v2\nalpha = semantic %s receipt %s\n", H1, H2);
    check("lock-refuses-wrong-header", lock_refused(buf, strlen(buf)));
    check("lock-refuses-empty-file", lock_refused("", 0));
    check("lock-refuses-missing-final-newline", lock_refused("omega.lock v1", 13));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s", H1, H2);
    check("lock-refuses-no-trailing-lf", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s\n", H1, H2);
    { char t[1024]; size_t k = strlen(buf); memcpy(t, buf, k + 1); t[22] = '\t';
      check("lock-refuses-tab", lock_refused(t, k)); }
    { char t[1024]; size_t k = strlen(buf); memcpy(t, buf, k + 1); t[k - 1] = '\r'; t[k] = '\n'; t[k + 1] = 0;
      check("lock-refuses-cr", lock_refused(t, k + 1)); }
    { char t[1024]; size_t k = strlen(buf); memcpy(t, buf, k + 1); t[k - 1] = 0; t[k] = '\n';
      check("lock-refuses-nul", lock_refused(t, k + 1)); }
    { const char *c1 = "omega.lock v1\n# a\tcomment\n"; check("lock-refuses-tab-in-comment", lock_refused(c1, strlen(c1)));
      const char *c2 = "omega.lock v1\r\n"; check("lock-refuses-crlf-header", lock_refused(c2, strlen(c2)));
      const char *c3 = "omega.lock v1\n# a comment\r\n"; check("lock-refuses-cr-in-comment", lock_refused(c3, strlen(c3)));
      static const char c4[] = "omega.lock v1\n# a\0comment\n"; check("lock-refuses-nul-in-comment", lock_refused(c4, sizeof c4 - 1)); }
    check("lock-refuses-unknown-line", lock_refused("omega.lock v1\nhello world\n", 26));
    snprintf(buf, sizeof buf, "omega.lock v1\n# a comment\n\nalpha = semantic %s receipt %s\n\n# another\n", H1, H2);
    rc = omega_lock_parse(buf, strlen(buf), &l, &e);
    check("lock-allows-comment-and-blank-lines", rc == 0 && l.n == 1);
    if (rc == 0) omega_lock_free(&l);
    check("lock-refuses-indented-comment", lock_refused("omega.lock v1\n # not a comment\n", 30));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s\n", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", H2);
    check("lock-refuses-all-uppercase-hex", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %.63sA receipt %s\n", H1, H2);
    check("lock-refuses-uppercase-hex", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %.63sg\n", H1, H2);
    check("lock-refuses-non-hex-character", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %.63s receipt %s\n", H1, H2);
    check("lock-refuses-short-hex", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s0 receipt %s\n", H1, H2);
    check("lock-refuses-long-hex", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s\n", "0000000000000000000000000000000000000000000000000000000000000000", H2);
    check("lock-refuses-zero-digest", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nbeta = semantic %s receipt %s\nalpha = semantic %s receipt %s\n", H1, H2, H1, H2);
    check("lock-refuses-unsorted", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s\nalpha = semantic %s receipt %s\n", H1, H2, H2, H1);
    check("lock-refuses-duplicate-name", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha  = semantic %s receipt %s\n", H1, H2);
    check("lock-refuses-extra-space", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s receipt %s \n", H1, H2);
    check("lock-refuses-trailing-space", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = receipt %s semantic %s\n", H1, H2);
    check("lock-refuses-swapped-keywords", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\n1alpha = semantic %s receipt %s\n", H1, H2);
    check("lock-refuses-bad-name-start", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nal/pha = semantic %s receipt %s\n", H1, H2);
    check("lock-refuses-bad-name-char", lock_refused(buf, strlen(buf)));
    snprintf(buf, sizeof buf, "omega.lock v1\nalpha = semantic %s\n", H1);
    check("lock-refuses-missing-receipt-field", lock_refused(buf, strlen(buf)));
}

/* ---- OSC front end: the `import` grammar ---- */
static int stub_hook(void *ctx, const OscImport *imps, uint32_t n, uint32_t *bad, char *why, size_t cap) {
    (void)bad; (void)why; (void)cap;
    uint32_t *c = ctx;
    c[0]++; c[1] = n;
    c[2] = (n > 0 && strcmp(imps[0].name, "foo") == 0) ? 1 : 0;
    return 0;
}
static int refuse_second_hook(void *ctx, const OscImport *imps, uint32_t n, uint32_t *bad, char *why, size_t cap) {
    (void)ctx; (void)imps;
    *bad = n - 1;
    snprintf(why, cap, "TEST_CODE: nope");
    return -1;
}
static int front(const char *src, OscImportResolver h, void *ctx, OscDiag *d) {
    OscUnit *u = malloc(sizeof *u);
    if (!u) setup_fail("oom");
    int rc = osc_compile_imports(src, strlen(src), u, d, NULL, h, ctx);
    free(u);
    return rc;
}
static void t_frontend(void) {
    OscDiag d; uint32_t c[3] = { 0, 0, 0 };
    char s[8192];
    snprintf(s, sizeof s, "import foo;\nimport bar;\n%s", FN);
    int rc = front(s, stub_hook, c, &d);
    check("osc-import-grammar-accepted", rc == 0 && c[0] == 1 && c[1] == 2 && c[2] == 1);
    c[0] = 0;
    rc = front(FN, stub_hook, c, &d);
    check("osc-resolver-runs-with-zero-imports", rc == 0 && c[0] == 1 && c[1] == 0);
    snprintf(s, sizeof s, "import foo\n%s", FN);
    rc = front(s, stub_hook, c, &d);
    check("osc-import-needs-semicolon", rc != 0 && d.kind == OSC_DIAG_SYNTAX);
    snprintf(s, sizeof s, "%simport foo;\n", FN);
    rc = front(s, stub_hook, c, &d);
    check("osc-import-after-function-refused", rc != 0 && d.kind == OSC_DIAG_SYNTAX);
    snprintf(s, sizeof s, "import foo;\nimport foo;\n%s", FN);
    rc = front(s, stub_hook, c, &d);
    check("osc-import-duplicate-refused", rc != 0 && d.kind == OSC_DIAG_REDEFINED_NAME);
    snprintf(s, sizeof s, "import 7;\n%s", FN);
    rc = front(s, stub_hook, c, &d);
    check("osc-import-number-refused", rc != 0 && d.kind == OSC_DIAG_SYNTAX);
    snprintf(s, sizeof s, "import fn;\n%s", FN);
    rc = front(s, stub_hook, c, &d);
    check("osc-import-keyword-refused", rc != 0 && d.kind == OSC_DIAG_SYNTAX);
    {
        size_t at = 0;
        for (int i = 0; i < 65; i++) at += (size_t)snprintf(s + at, sizeof s - at, "import m%d;\n", i);
        snprintf(s + at, sizeof s - at, "%s", FN);
        rc = front(s, stub_hook, c, &d);
        check("osc-too-many-imports-refused", rc != 0 && d.kind == OSC_DIAG_CAPACITY);
    }
    snprintf(s, sizeof s, "import foo;\n%s", FN);
    OscUnit *u = malloc(sizeof *u);
    if (!u) setup_fail("oom");
    rc = osc_compile(s, strlen(s), u, &d, NULL);
    check("osc-plain-compile-refuses-imports", rc != 0 && d.kind == OSC_DIAG_UNSUPPORTED);
    rc = osc_compile_imports(s, strlen(s), u, &d, NULL, NULL, NULL);
    check("osc-no-resolver-refuses-imports", rc != 0 && d.kind == OSC_DIAG_UNSUPPORTED);
    uint8_t d1[32], d2[32];
    OscUnit *u2 = malloc(sizeof *u2);
    if (!u2) setup_fail("oom");
    c[0] = 0;
    int r1 = osc_compile(FN, strlen(FN), u, &d, NULL);
    int r2 = osc_compile_imports(FN, strlen(FN), u2, &d, NULL, stub_hook, c);
    int g = r1 == 0 && r2 == 0 && osc_ir_digest(u, d1) == 0 && osc_ir_digest(u2, d2) == 0;
    check("osc-no-import-unit-identical-to-plain-compile", g && memcmp(d1, d2, 32) == 0);
    free(u); free(u2);
    snprintf(s, sizeof s, "import foo;\nimport bar;\n%s", FN);
    rc = front(s, refuse_second_hook, NULL, &d);
    check("osc-hook-refusal-points-at-the-import", rc != 0 && d.kind == OSC_DIAG_IMPORT_REFUSED && d.line == 2 &&
          strcmp(d.transition, "TEST_CODE") == 0);
}

/* ---- reference formulas (independent of the library under test) ---- */
static void ref_closure_digest(const OmegaClosureEntry *e, size_t n, uint8_t out[32]) {
    B b = { 0, 0, 0 };
    bput(&b, "OMEGA.CLOSURE.V1", 16);
    uint8_t z = 0; bput(&b, &z, 1);
    bu32(&b, (uint32_t)n);
    for (size_t i = 0; i < n; i++) { bput(&b, e[i].semantic_id, 32); bput(&b, e[i].receipt_id, 32); bput(&b, &e[i].admission_kind, 1); }
    sha256_hash(b.p, b.n, out);
    free(b.p);
}
static void ref_build_id(const uint8_t ir[32], int dom, const uint8_t cl[32], uint8_t out[32]) {
    B b = { 0, 0, 0 };
    bput(&b, "OSC1.BUILD.V1", 13);
    uint8_t z = 0, d = (uint8_t)dom; bput(&b, &z, 1);
    bput(&b, ir, 32); bput(&b, &d, 1); bput(&b, cl, 32);
    sha256_hash(b.p, b.n, out);
    free(b.p);
}
static int ir_of(const Run *r, uint8_t out[32]) { return osc_ir_digest(r->u, out); }

/* ---- the refusal codes, each from a real compile ---- */
static void t_refusals(void) {
    W w; Run r; Opt o;

    /* the good path first: a record that passes every check resolves */
    o = opt0();
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("resolve-accepts-verified-record", r.rc == 0 && r.h.resolved && r.h.closure.n == 1 && r.h.n_imports == 1);
    run_free(&r); w_free(&w);

    o = opt0(); o.dirty = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-dirty-receipt-in-build", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY));
    run_free(&r); w_free(&w);
    one_foo(&o, OMEGA_DOMAIN_DEV, &r, &w);
    check("allow-dirty-receipt-in-dev", r.rc == 0);
    { uint8_t ir[32]; OmegaArtifactMeta meta;
      check("dirty-dev-ir-digest", ir_of(&r, ir) == 0);
      omega_artifact_meta_make(&meta, OMEGA_DOMAIN_DEV, ir, r.h.closure.digest);
      check("dirty-dev-output-remains-tainted", meta.tainted == 1); }
    run_free(&r); w_free(&w);

    /* UNVERIFIED_DEPENDENCY: the pinned id is not in the store */
    w_init(&w);
    { Opt a = opt0(); add_comp(&w, 0x51, NULL, 0, &a); }
    { Pin p[1] = { { "foo", 0x77, 0 } }; const char *nm[1] = { "foo" };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-unverified-id-not-in-store", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY));
    run_free(&r); w_free(&w);

    /* MISSING_RECEIPT: the record names a receipt that is not in the receipt store */
    o = opt0(); o.no_store = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-missing-receipt", refused(&r, "MISSING_RECEIPT", OMEGA_RES_MISSING_RECEIPT));
    { const char *names[] = { "foo" }; OmegaClosure out = {0};
      int rc = omega_resolve_imports(&r.h.resolver, &r.lock, names, 1, &out, NULL);
      check("missing-receipt-null-error-safe", rc == OMEGA_RES_MISSING_RECEIPT && out.n == 0);
      omega_closure_free(&out); }
    run_free(&r); w_free(&w);

    /* RECEIPT_HASH_MISMATCH, one cause per check */
    o = opt0(); o.swap = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-file-holds-another-receipt", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r); w_free(&w);
    o = opt0(); o.skip_sid = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-does-not-name-program", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r); w_free(&w);
    o = opt0(); o.wrong_root = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-output-digest-is-not-evidence-root", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r); w_free(&w);
    o = opt0(); o.wrong_kind = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-kind-is-not-profile", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r); w_free(&w);
    o = opt0(); o.extra_rdep = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-has-extra-dependency", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r); w_free(&w);

    /* the lock pins another receipt than the record carries */
    w_init(&w);
    o = opt0(); add_comp(&w, 0x51, NULL, 0, &o);
    { Pin p[1] = { { "foo", 0x51, 1 } }; const char *nm[1] = { "foo" };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-lock-pins-another-receipt", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r); w_free(&w);

    /* the same, but the pinned name was already reached as a dependency of an earlier import */
    w_init(&w);
    o = opt0(); add_comp(&w, 0x61, NULL, 0, &o);
    { uint8_t d[1][32]; mkid(d[0], 0x61); add_comp(&w, 0x62, d, 1, &o); }
    { Pin p[2] = { { "alpha", 0x62, 0 }, { "beta", 0x61, 1 } }; const char *nm[2] = { "alpha", "beta" };
      run_compile(&w, src_imports(nm, 2), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 2), &r); }
    check("refuse-lock-pins-another-receipt-for-visited-node", refused(&r, "RECEIPT_HASH_MISMATCH", OMEGA_RES_RECEIPT_HASH_MISMATCH));
    run_free(&r);
    { Pin p[2] = { { "alpha", 0x62, 0 }, { "beta", 0x61, 0 } }; const char *nm[2] = { "alpha", "beta" };
      run_compile(&w, src_imports(nm, 2), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 2), &r); }
    check("accept-visited-node-with-the-right-pin", r.rc == 0 && r.h.closure.n == 2);
    run_free(&r); w_free(&w);

    /* STALE_RECEIPT: the receipt covers an older source digest */
    o = opt0(); o.stale_src = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-stale-receipt", refused(&r, "STALE_RECEIPT", OMEGA_RES_STALE_RECEIPT));
    run_free(&r); w_free(&w);

    /* DEPENDENCY_NOT_PINNED / UNDECLARED_IMPORT, both directions */
    w_init(&w);
    o = opt0(); add_comp(&w, 0x51, NULL, 0, &o);
    { const char *nm[1] = { "foo" }; run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, NULL, &r); }
    check("refuse-not-pinned-without-lock", refused(&r, "DEPENDENCY_NOT_PINNED", OMEGA_RES_DEPENDENCY_NOT_PINNED));
    run_free(&r);
    { const char *nm[2] = { "foo", "bar" }; Pin p[1] = { { "foo", 0x51, 0 } };
      run_compile(&w, src_imports(nm, 2), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-not-pinned-second-import", refused(&r, "DEPENDENCY_NOT_PINNED", OMEGA_RES_DEPENDENCY_NOT_PINNED) && strcmp(r.h.err.subject, "bar") == 0);
    run_free(&r);
    { Pin p[1] = { { "foo", 0x51, 0 } };
      run_compile(&w, FN, OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-undeclared-lock-line-without-import", refused(&r, "UNDECLARED_IMPORT", OMEGA_RES_UNDECLARED_IMPORT) && strcmp(r.h.err.subject, "foo") == 0);
    run_free(&r);
    { const char *nm[1] = { "foo" }; Pin p[2] = { { "bar", 0x51, 0 }, { "foo", 0x51, 0 } };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 2), &r); }
    check("refuse-undeclared-extra-lock-line", refused(&r, "UNDECLARED_IMPORT", OMEGA_RES_UNDECLARED_IMPORT) && strcmp(r.h.err.subject, "bar") == 0);
    run_free(&r);
    { run_compile(&w, FN, OMEGA_DOMAIN_BUILD, NULL, &r); }
    check("accept-no-imports-no-lock", r.rc == 0 && r.h.closure.n == 0);
    run_free(&r); w_free(&w);

    /* TAINTED_ARTIFACT: a record carrying the dev taint capability, in either domain */
    o = opt0(); o.taint = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-tainted-record-in-build-domain", refused(&r, "TAINTED_ARTIFACT", OMEGA_RES_TAINTED_ARTIFACT));
    run_free(&r); w_free(&w);
    one_foo(&o, OMEGA_DOMAIN_DEV, &r, &w);
    check("refuse-tainted-record-in-dev-domain", refused(&r, "TAINTED_ARTIFACT", OMEGA_RES_TAINTED_ARTIFACT));
    run_free(&r); w_free(&w);

    /* UNKNOWN_VERIFIER_PROFILE and VERIFIER_TOO_OLD, from a real record */
    o = opt0(); o.prof = "mystery-v9";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-unknown-verifier-profile", refused(&r, "UNKNOWN_VERIFIER_PROFILE", OMEGA_RES_UNKNOWN_VERIFIER_PROFILE));
    run_free(&r); w_free(&w);
    o = opt0(); o.ver = "0.9.0";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-verifier-too-old", refused(&r, "VERIFIER_TOO_OLD", OMEGA_RES_VERIFIER_TOO_OLD));
    run_free(&r); w_free(&w);

    /* UNVERIFIED_DEPENDENCY from the receipt's own content (SPEC 5.1 rule 2) */
    o = opt0(); o.result = "FAIL";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-result-is-not-pass", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY));
    run_free(&r); w_free(&w);
    o = opt0(); o.assert_fail = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-assertion-failed", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY));
    run_free(&r); w_free(&w);
    o = opt0(); o.tier = "TEST_ONLY_TRUST";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-test-only-trust", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "satisfies no profile"));
    run_free(&r); w_free(&w);
    o = opt0(); o.prof = "production-v1";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-receipt-tier-below-profile", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "below the minimum"));
    run_free(&r); w_free(&w);
    o = opt0(); o.prof = "qemu-v1";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-host-receipt-for-qemu-profile", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "below the minimum"));
    run_free(&r); w_free(&w);
    o = opt0(); o.prof = "qemu-v1"; o.tier = "QEMU";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("accept-qemu-receipt-for-qemu-profile", r.rc == 0 && r.h.closure.n == 1);
    run_free(&r); w_free(&w);
    o = opt0(); o.prof = "host-v1"; o.tier = "QEMU";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("accept-higher-tier-receipt", r.rc == 0);
    run_free(&r); w_free(&w);
    o = opt0(); o.prof = "host-v1"; o.tier = "MACHINE1_READ_ONLY";
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("refuse-hardware-receipt-without-lease", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "lease"));
    run_free(&r); w_free(&w);
    o = opt0(); o.prof = "host-v1"; o.tier = "MACHINE1_READ_ONLY"; o.lease = 1;
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r, &w);
    check("accept-hardware-receipt-with-lease", r.rc == 0);
    run_free(&r); w_free(&w);

    /* blob check (optional source/IR store of stage 6): a blob that does not hash to the digest */
    {
        w_init(&w);
        o = opt0(); add_comp(&w, 0x51, NULL, 0, &o);
        Pin p[1] = { { "foo", 0x51, 0 } }; const char *nm[1] = { "foo" };
        const char *lk = lock_text(&w, p, 1);
        char dir[] = "/tmp/omega-resolve-blobs-XXXXXX";
        if (!mkdtemp(dir)) setup_fail("mkdtemp");
        uint8_t src[32]; memset(src, 0x33, 32); src[0] = 0x51;
        char hh[65], path[300]; hx(src, hh);
        snprintf(path, sizeof path, "%s/%s.blob", dir, hh);
        FILE *f = fopen(path, "wb"); fputs("not the source", f); fclose(f);
        (void)src_imports(nm, 1);
        memset(&r, 0, sizeof r);
        r.u = malloc(sizeof *r.u);
        OmegaResolveError le; omega_lock_parse(lk, strlen(lk), &r.lock, &le);
        r.h.lock = &r.lock; r.h.resolver.store = w.st; r.h.resolver.domain = OMEGA_DOMAIN_BUILD;
        r.h.resolver.fetch_receipt = tab_fetch; r.h.resolver.fetch_ctx = &w.rt;
        r.h.resolver.fetch_blob = omega_blob_dir_fetch; r.h.resolver.blob_ctx = dir;
        r.rc = osc_compile_imports(g_src, strlen(g_src), r.u, &r.dg, NULL, omega_osc_import_hook, &r.h);
        check("refuse-blob-does-not-hash-to-digest", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY));
        run_free(&r);
        /* a blob that does hash to the digest passes */
        uint8_t blobdig[32]; const char *content = "the real source";
        sha256_hash((const uint8_t *)content, strlen(content), blobdig);
        /* record 0x52 whose source_or_ir_digest is the digest of the blob we write */
        {
            uint8_t dbuf[1][64]; Spec sp; spec_for(&sp, 0x52, NULL, 0, dbuf);
            memcpy(sp.src, blobdig, 32);
            char sid_h[65], src_h[65], root_h[65], inp0[80], inp1[80]; hx(sp.sem, sid_h); hx(sp.src, src_h); hx(sp.root, root_h);
            RJ j; rj_default(&j);
            snprintf(inp0, sizeof inp0, "sha256:%s", sid_h); snprintf(inp1, sizeof inp1, "sha256:%s", src_h);
            j.inputs[0] = inp0; j.inputs[1] = inp1; j.n_in = 2; memcpy(j.root, root_h, 65);
            uint8_t rid[32]; char *json; size_t jl;
            if (rj_make(&j, rid, &json, &jl)) setup_fail("blob receipt");
            memcpy(sp.rcpt, rid, 32); tab_put(&w.rt, rid, json, jl); free(json);
            if (ins_spec(w.st, &sp, 0) != 0) setup_fail("blob record");
            snprintf(path, sizeof path, "%s/%s.blob", dir, src_h);
            f = fopen(path, "wb"); fputs(content, f); fclose(f);
        }
        Pin p2[1] = { { "foo", 0x52, 0 } };
        const char *lk2 = lock_text(&w, p2, 1);
        memset(&r, 0, sizeof r);
        r.u = malloc(sizeof *r.u);
        omega_lock_parse(lk2, strlen(lk2), &r.lock, &le);
        r.h.lock = &r.lock; r.h.resolver.store = w.st; r.h.resolver.domain = OMEGA_DOMAIN_BUILD;
        r.h.resolver.fetch_receipt = tab_fetch; r.h.resolver.fetch_ctx = &w.rt;
        r.h.resolver.fetch_blob = omega_blob_dir_fetch; r.h.resolver.blob_ctx = dir;
        r.rc = osc_compile_imports(g_src, strlen(g_src), r.u, &r.dg, NULL, omega_osc_import_hook, &r.h);
        check("accept-blob-that-hashes-to-digest", r.rc == 0);
        run_free(&r); w_free(&w);
        char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        if (system(cmd) != 0) setup_fail("cleanup");
    }
}

/* ---- graphs: closure, cycle, edges, genesis ---- */
static void t_graphs(void) {
    W w; Run r; Opt o = opt0();

    /* the diamond: root needs b and c, both need d */
    w_init(&w);
    { uint8_t d[1][32], rr[2][32];
      add_comp(&w, 0x10, NULL, 0, &o);
      mkid(d[0], 0x10);
      add_comp(&w, 0x20, d, 1, &o); add_comp(&w, 0x30, d, 1, &o);
      mkid(rr[0], 0x20); mkid(rr[1], 0x30);
      add_comp(&w, 0x40, rr, 2, &o); }
    Pin pr[1] = { { "root", 0x40, 0 } }; const char *nr[1] = { "root" };
    run_compile(&w, src_imports(nr, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, pr, 1), &r);
    int ok = r.rc == 0 && r.h.closure.n == 4;
    OmegaClosureEntry want[4];
    static const uint8_t ids[4] = { 0x10, 0x20, 0x30, 0x40 };
    for (int i = 0; i < 4 && ok; i++) {
        mkid(want[i].semantic_id, ids[i]);
        omega_vcstore_receipt_of(w.st, want[i].semantic_id, want[i].receipt_id);
        want[i].admission_kind = 1;
        if (memcmp(r.h.closure.entries[i].semantic_id, want[i].semantic_id, 32) || memcmp(r.h.closure.entries[i].receipt_id, want[i].receipt_id, 32) ||
            r.h.closure.entries[i].admission_kind != 1) ok = 0;
    }
    uint8_t rd[32];
    if (ok) ref_closure_digest(want, 4, rd);
    check("closure-diamond-has-each-node-once-in-id-order", ok);
    check("closure-digest-matches-reference", ok && memcmp(rd, r.h.closure.digest, 32) == 0);
    {
        OmegaClosureEntry tweak[4]; memcpy(tweak, want, sizeof tweak);
        tweak[2].receipt_id[0] ^= 1;
        uint8_t td[32]; ref_closure_digest(tweak, 4, td);
        tweak[2].receipt_id[0] ^= 1; tweak[2].admission_kind = 2;
        uint8_t td2[32]; ref_closure_digest(tweak, 4, td2);
        check("closure-digest-binds-receipt-ids-and-kinds", ok && memcmp(td, rd, 32) != 0 && memcmp(td2, rd, 32) != 0);
    }
    run_free(&r);

    /* resolution order differs from id order: names alpha -> 0x90, zeta -> 0x10 */
    w_free(&w); w_init(&w);
    add_comp(&w, 0x90, NULL, 0, &o); add_comp(&w, 0x10, NULL, 0, &o);
    { Pin p[2] = { { "alpha", 0x90, 0 }, { "zeta", 0x10, 0 } }; const char *nm[2] = { "alpha", "zeta" };
      run_compile(&w, src_imports(nm, 2), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 2), &r); }
    int sorted = r.rc == 0 && r.h.closure.n == 2 && memcmp(r.h.closure.entries[0].semantic_id, r.h.closure.entries[1].semantic_id, 32) < 0 &&
                 r.h.closure.entries[0].semantic_id[0] == 0x10;
    check("closure-sorted-ascending-by-semantic-id", sorted);
    run_free(&r); w_free(&w);

    /* DEPENDENCY_CYCLE: only a tampered store can hold one (insert refuses it), shown with genesis records */
    w_init(&w); w.allow_genesis = 1;
    { Opt b = opt0(); b.boot = 1; uint8_t d[1][32];
      add_comp(&w, 0x71, NULL, 0, &b);
      mkid(d[0], 0x71); add_comp(&w, 0x72, d, 1, &b);
      uint8_t db[1][64]; uint8_t dep72[1][32]; mkid(dep72[0], 0x72);
      Spec sp; spec_for(&sp, 0x71, dep72, 1, db);
      memset(sp.rcpt, 0xA0, 32); sp.rcpt[0] = 0x71;
      replace_obj(&w, 0x71, &sp); }
    { Pin p[1] = { { "cyc", 0x71, 0 } }; const char *nm[1] = { "cyc" };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-dependency-cycle", refused(&r, "DEPENDENCY_CYCLE", OMEGA_RES_DEPENDENCY_CYCLE));
    run_free(&r); w_free(&w);

    /* an edge whose required contract differs from the dependency's contract */
    w_init(&w);
    add_comp(&w, 0x81, NULL, 0, &o);
    { uint8_t d[1][32]; mkid(d[0], 0x81); add_comp(&w, 0x82, d, 1, &o);
      uint8_t db[1][64]; Spec sp; spec_for(&sp, 0x82, d, 1, db);
      db[0][32] ^= 0x01;                                   /* required_contract now wrong */
      omega_vcstore_receipt_of(w.st, sp.sem, sp.rcpt);
      replace_obj(&w, 0x82, &sp); }
    { Pin p[1] = { { "top", 0x82, 0 } }; const char *nm[1] = { "top" };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-edge-contract-mismatch", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY) && strstr(r.h.err.message, "contract") != NULL);
    run_free(&r); w_free(&w);

    /* genesis (BOOTSTRAP) records */
    { Opt b = opt0(); b.boot = 1;
      w_init(&w);
      add_comp(&w, 0x91, NULL, 0, &b);
      Pin p[1] = { { "base", 0x91, 0 } }; const char *nm[1] = { "base" };
      const char *lk = lock_text(&w, p, 1);
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lk, &r);
      check("refuse-genesis-record-without-permission", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "genesis"));
      run_free(&r);
      w.allow_genesis = 1;
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lk, &r);
      check("accept-genesis-record-with-permission", r.rc == 0 && r.h.closure.n == 1 && r.h.closure.entries[0].admission_kind == 2);
      run_free(&r);
      /* a verified record on top of a genesis record needs the permission too */
      uint8_t d[1][32]; mkid(d[0], 0x91);
      add_comp(&w, 0x92, d, 1, &o);
      Pin p2[1] = { { "top", 0x92, 0 } }; const char *nm2[1] = { "top" };
      const char *lk2 = lock_text(&w, p2, 1);
      run_compile(&w, src_imports(nm2, 1), OMEGA_DOMAIN_BUILD, lk2, &r);
      check("accept-verified-on-genesis-with-permission", r.rc == 0 && r.h.closure.n == 2);
      run_free(&r);
      w.allow_genesis = 0;
      run_compile(&w, src_imports(nm2, 1), OMEGA_DOMAIN_BUILD, lk2, &r);
      check("refuse-verified-on-genesis-without-permission", refused(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY));
      run_free(&r);
      /* a genesis record may NOT depend on a verified record */
      w.allow_genesis = 1;
      mkid(d[0], 0x92);
      add_comp(&w, 0x93, d, 1, &b);
      Pin p3[1] = { { "bad", 0x93, 0 } }; const char *nm3[1] = { "bad" };
      run_compile(&w, src_imports(nm3, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p3, 1), &r);
      check("refuse-genesis-record-depending-on-verified", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "closed over itself"));
      run_free(&r); w_free(&w);
    }
}

/* ---- admission into the store ---- */
static int take(W *w, uint8_t b0, uint8_t **bytes, size_t *len, uint8_t id[32]) {
    uint8_t sid[32]; mkid(sid, b0);
    for (size_t i = 0; i < w->st->count; i++)
        if (memcmp(w->st->objs[i].semantic_id, sid, 32) == 0) {
            *bytes = malloc(w->st->objs[i].len); if (!*bytes) return -1;
            memcpy(*bytes, w->st->objs[i].bytes, w->st->objs[i].len);
            *len = w->st->objs[i].len;
            memcpy(id, w->st->objs[i].vc_id, 32);
            return 0;
        }
    return -1;
}
static OmegaResolver resolver_of(W *dst, RTab *rt) {
    OmegaResolver r; memset(&r, 0, sizeof r);
    r.store = dst->st; r.domain = OMEGA_DOMAIN_BUILD; r.fetch_receipt = tab_fetch; r.fetch_ctx = rt;
    return r;
}
static void t_admit(void) {
    OmegaResolveError e;
    /* source world: builds the records and receipts, a second empty store is the target */
    {
        W src, dst; Opt o = opt0();
        w_init(&src); w_init(&dst);
        add_comp(&src, 0x51, NULL, 0, &o);
        uint8_t *b; size_t n; uint8_t id[32];
        if (take(&src, 0x51, &b, &n, id)) setup_fail("take");
        OmegaResolver r = resolver_of(&dst, &src.rt);
        int rc = omega_resolve_admit(&r, dst.st, b, n, id, NULL, &e);
        uint8_t kind = 0; uint8_t sid[32]; mkid(sid, 0x51);
        check("admit-verified-record-with-receipt", rc == 0 && omega_vcstore_count(dst.st) == 1 &&
              omega_vcstore_admission_kind(dst.st, sid, &kind) == 0 && kind == OMEGA_VCS_ADMISSION_VERIFIED);
        /* wrong claimed id */
        uint8_t bad[32]; memcpy(bad, id, 32); bad[0] ^= 1;
        W dst2; w_init(&dst2); OmegaResolver r2 = resolver_of(&dst2, &src.rt);
        rc = omega_resolve_admit(&r2, dst2.st, b, n, bad, NULL, &e);
        check("admit-refuses-wrong-claimed-id", rc != 0 && omega_vcstore_count(dst2.st) == 0);
        /* dev-domain origin */
        OmegaArtifactMeta m; uint8_t ir[32], cl[32]; memset(ir, 1, 32); memset(cl, 2, 32);
        omega_artifact_meta_make(&m, OMEGA_DOMAIN_DEV, ir, cl);
        rc = omega_resolve_admit(&r2, dst2.st, b, n, id, &m, &e);
        check("admit-refuses-record-from-tainted-origin", rc == OMEGA_RES_TAINTED_ARTIFACT && omega_vcstore_count(dst2.st) == 0);
        omega_artifact_meta_make(&m, OMEGA_DOMAIN_BUILD, ir, cl);
        rc = omega_resolve_admit(&r2, dst2.st, b, n, id, &m, &e);
        check("admit-accepts-record-from-build-origin", rc == 0 && omega_vcstore_count(dst2.st) == 1);
        free(b); w_free(&dst2); w_free(&dst); w_free(&src);
    }
    {   /* a record carrying the dev taint capability never enters */
        W src, dst; Opt o = opt0(); o.taint = 1;
        w_init(&src); w_init(&dst);
        add_comp(&src, 0x52, NULL, 0, &o);
        uint8_t *b; size_t n; uint8_t id[32];
        if (take(&src, 0x52, &b, &n, id)) setup_fail("take");
        OmegaResolver r = resolver_of(&dst, &src.rt);
        int rc = omega_resolve_admit(&r, dst.st, b, n, id, NULL, &e);
        check("admit-refuses-taint-capability", rc == OMEGA_RES_TAINTED_ARTIFACT && omega_vcstore_count(dst.st) == 0);
        rc = omega_resolve_admit_genesis(dst.st, b, n, id, NULL, &e);
        check("admit-genesis-refuses-taint-capability", rc == OMEGA_RES_TAINTED_ARTIFACT && omega_vcstore_count(dst.st) == 0);
        free(b); w_free(&dst); w_free(&src);
    }
    {   /* admission demands the receipt to qualify the record */
        static const char *const why[3] = { "no receipt", "receipt does not match the record", "receipt records FAIL" };
        for (int k = 0; k < 3; k++) {
            W src, dst; Opt o = opt0();
            if (k == 0) o.no_store = 1;
            if (k == 1) o.wrong_root = 1;
            if (k == 2) o.result = "FAIL";
            w_init(&src); w_init(&dst);
            add_comp(&src, 0x53, NULL, 0, &o);
            uint8_t *b; size_t n; uint8_t id[32];
            if (take(&src, 0x53, &b, &n, id)) setup_fail("take");
            OmegaResolver r = resolver_of(&dst, &src.rt);
            int rc = omega_resolve_admit(&r, dst.st, b, n, id, NULL, &e);
            char nm[80]; snprintf(nm, sizeof nm, "admit-refuses-%s", k == 0 ? "missing-receipt" : k == 1 ? "mismatched-receipt" : "failing-receipt");
            (void)why;
            check(nm, rc != 0 && omega_vcstore_count(dst.st) == 0);
            free(b); w_free(&dst); w_free(&src);
        }
    }
    {   /* genesis loading: no receipt, but it may depend only on genesis records */
        W src, dst; Opt b = opt0(); b.boot = 1; Opt v = opt0();
        w_init(&src); w_init(&dst);
        add_comp(&src, 0x61, NULL, 0, &b);
        uint8_t *g; size_t gn; uint8_t gid[32];
        if (take(&src, 0x61, &g, &gn, gid)) setup_fail("take");
        int rc = omega_resolve_admit_genesis(dst.st, g, gn, gid, NULL, &e);
        uint8_t kind = 0; uint8_t sid[32]; mkid(sid, 0x61);
        check("admit-genesis-records-bootstrap-kind", rc == 0 && omega_vcstore_admission_kind(dst.st, sid, &kind) == 0 && kind == OMEGA_VCS_ADMISSION_BOOTSTRAP);
        /* depends on a genesis record already loaded: fine */
        uint8_t d[1][32]; mkid(d[0], 0x61);
        add_comp(&src, 0x62, d, 1, &b);
        uint8_t *g2; size_t g2n; uint8_t g2id[32];
        if (take(&src, 0x62, &g2, &g2n, g2id)) setup_fail("take");
        rc = omega_resolve_admit_genesis(dst.st, g2, g2n, g2id, NULL, &e);
        check("admit-genesis-may-depend-on-genesis", rc == 0 && omega_vcstore_count(dst.st) == 2);
        /* depends on a verified record: refused */
        add_comp(&dst, 0x63, NULL, 0, &v);               /* verified record in the target store */
        add_comp(&src, 0x63, NULL, 0, &v);
        mkid(d[0], 0x63);
        add_comp(&src, 0x64, d, 1, &b);
        uint8_t *g3; size_t g3n; uint8_t g3id[32];
        if (take(&src, 0x64, &g3, &g3n, g3id)) setup_fail("take");
        size_t before = omega_vcstore_count(dst.st);
        rc = omega_resolve_admit_genesis(dst.st, g3, g3n, g3id, NULL, &e);
        check("admit-genesis-refuses-verified-dependency", rc == OMEGA_RES_UNVERIFIED_DEPENDENCY && omega_vcstore_count(dst.st) == before);
        OmegaArtifactMeta m; uint8_t ir[32], cl[32]; memset(ir, 1, 32); memset(cl, 2, 32);
        omega_artifact_meta_make(&m, OMEGA_DOMAIN_DEV, ir, cl);
        rc = omega_resolve_admit_genesis(dst.st, g, gn, gid, &m, &e);
        check("admit-genesis-refuses-tainted-origin", rc == OMEGA_RES_TAINTED_ARTIFACT);
        free(g); free(g2); free(g3); w_free(&dst); w_free(&src);
    }
}

/* ---- guards: chain depth, duplicate imports through the API ---- */
static void build_chain(W *w, int n) {
    Opt o = opt0();
    for (int k = 1; k <= n; k++) {
        uint8_t d[1][32];
        if (k > 1) mkid(d[0], (uint8_t)(k - 1));
        if (add_comp(w, (uint8_t)k, k > 1 ? d : NULL, k > 1 ? 1 : 0, &o) != 0) setup_fail("chain insert");
    }
}
static void t_guards(void) {
    W w; Run r;
    w_init(&w); build_chain(&w, 128);
    { Pin p[1] = { { "top", 128, 0 } }; const char *nm[1] = { "top" };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("accept-dependency-chain-at-the-limit", r.rc == 0 && r.h.closure.n == 128);
    run_free(&r); w_free(&w);
    w_init(&w); build_chain(&w, 129);
    { Pin p[1] = { { "top", 129, 0 } }; const char *nm[1] = { "top" };
      run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w, p, 1), &r); }
    check("refuse-dependency-chain-too-deep", refused_msg(&r, "UNVERIFIED_DEPENDENCY", OMEGA_RES_UNVERIFIED_DEPENDENCY, "deeper than") && r.h.closure.n == 0);
    run_free(&r); w_free(&w);

    /* the same import name listed twice, straight into the resolver (the parser cannot produce it) */
    w_init(&w);
    { Opt o = opt0(); add_comp(&w, 0x51, NULL, 0, &o); }
    { Pin p[1] = { { "foo", 0x51, 0 } };
      OmegaLock lock; OmegaResolveError e; OmegaClosure cl;
      const char *lk = lock_text(&w, p, 1);
      if (omega_lock_parse(lk, strlen(lk), &lock, &e)) setup_fail("lock");
      OmegaResolver rv = resolver_of(&w, &w.rt);
      const char *twice[2] = { "foo", "foo" };
      memset(&cl, 0, sizeof cl);
      int rc = omega_resolve_imports(&rv, &lock, twice, 2, &cl, &e);
      check("refuse-duplicate-import-names-through-the-api", rc == OMEGA_RES_BAD_ARGUMENT && cl.n == 0 && cl.entries == NULL);
      const char *once[1] = { "foo" };
      rc = omega_resolve_imports(&rv, &lock, once, 1, &cl, &e);
      check("accept-single-import-name-through-the-api", rc == 0 && cl.n == 1);
      omega_closure_free(&cl); omega_lock_free(&lock); }
    w_free(&w);
}

/* ---- build identity, domains, artifact header ---- */
static void t_identity(void) {
    uint8_t ir[32], cl[32], got[32], ref[32];
    memset(ir, 0x11, 32); memset(cl, 0x22, 32);
    int ok = 1;
    for (int dom = 1; dom <= 2; dom++) {
        omega_build_id(ir, (OmegaDomain)dom, cl, got);
        ref_build_id(ir, dom, cl, ref);
        if (memcmp(got, ref, 32) != 0) ok = 0;
    }
    check("build-id-matches-reference", ok);
    uint8_t b1[32], b2[32], b3[32], b4[32];
    omega_build_id(ir, OMEGA_DOMAIN_BUILD, cl, b1);
    memset(cl, 0x23, 32); omega_build_id(ir, OMEGA_DOMAIN_BUILD, cl, b2);
    memset(cl, 0x22, 32); omega_build_id(ir, OMEGA_DOMAIN_DEV, cl, b3);
    memset(ir, 0x12, 32); omega_build_id(ir, OMEGA_DOMAIN_BUILD, cl, b4);
    check("build-id-changes-with-closure-domain-and-ir", memcmp(b1, b2, 32) && memcmp(b1, b3, 32) && memcmp(b1, b4, 32) && memcmp(b2, b3, 32));

    memset(ir, 0x11, 32);
    OmegaArtifactMeta m, p; OmegaResolveError e; char text[600];
    omega_artifact_meta_make(&m, OMEGA_DOMAIN_DEV, ir, cl);
    OmegaArtifactMeta mb; omega_artifact_meta_make(&mb, OMEGA_DOMAIN_BUILD, ir, cl);
    check("artifact-dev-is-tainted-build-is-not", m.tainted == 1 && mb.tainted == 0 && m.domain == OMEGA_DOMAIN_DEV);
    int tl = omega_artifact_meta_text(&m, text, sizeof text);
    check("artifact-header-round-trips", tl > 0 && omega_artifact_meta_parse(text, (size_t)tl, &p, &e) == 0 &&
          p.domain == OMEGA_DOMAIN_DEV && p.tainted == 1 && memcmp(p.build_id, m.build_id, 32) == 0 && strstr(text, "tainted 1\n") != NULL);
    char t2[800]; memcpy(t2, text, (size_t)tl + 1);
    char *at = strstr(t2, "tainted 1");
    if (at) at[8] = '0';
    check("artifact-refuses-dev-header-marked-untainted", at && omega_artifact_meta_parse(t2, (size_t)tl, &p, &e) == OMEGA_RES_TAINTED_ARTIFACT);
    memcpy(t2, text, (size_t)tl + 1);
    at = strstr(t2, "domain dev");
    if (at) memcpy(at, "domain bui", 10), memmove(at + 10, at + 10, 0);
    /* a header edited to the other domain: the mark and the build id no longer fit */
    check("artifact-refuses-edited-domain", at && omega_artifact_meta_parse(t2, (size_t)tl, &p, &e) != 0);
    memcpy(t2, text, (size_t)tl + 1);
    t2[tl - 2] = t2[tl - 2] == '0' ? '1' : '0';
    check("artifact-refuses-edited-build-id", omega_artifact_meta_parse(t2, (size_t)tl, &p, &e) == OMEGA_RES_TAINTED_ARTIFACT);
    snprintf(t2, sizeof t2, "%sextra line\n", text);
    check("artifact-refuses-extra-line", omega_artifact_meta_parse(t2, strlen(t2), &p, &e) != 0);
    check("artifact-refuses-truncated-header", omega_artifact_meta_parse(text, (size_t)tl - 3, &p, &e) != 0);

    /* through real compiles: same IR, different closure or domain, different build id */
    W w1, w2; Run r1, r2, r3; Opt o = opt0();
    one_foo(&o, OMEGA_DOMAIN_BUILD, &r1, &w1);
    w_init(&w2);
    add_comp(&w2, 0x52, NULL, 0, &o);       /* a different record behind the same name */
    { Pin pn[1] = { { "foo", 0x52, 0 } }; const char *nm[1] = { "foo" };
      run_compile(&w2, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lock_text(&w2, pn, 1), &r2); }
    { Pin pn[1] = { { "foo", 0x51, 0 } }; const char *nm[1] = { "foo" };
      W w3; w_init(&w3); add_comp(&w3, 0x51, NULL, 0, &o);
      run_compile(&w3, src_imports(nm, 1), OMEGA_DOMAIN_DEV, lock_text(&w3, pn, 1), &r3);
      uint8_t i1[32], i2[32], i3[32], bb1[32], bb2[32], bb3[32];
      int g = r1.rc == 0 && r2.rc == 0 && r3.rc == 0 && ir_of(&r1, i1) == 0 && ir_of(&r2, i2) == 0 && ir_of(&r3, i3) == 0;
      if (g) {
          omega_build_id(i1, OMEGA_DOMAIN_BUILD, r1.h.closure.digest, bb1);
          omega_build_id(i2, OMEGA_DOMAIN_BUILD, r2.h.closure.digest, bb2);
          omega_build_id(i3, OMEGA_DOMAIN_DEV, r3.h.closure.digest, bb3);
      }
      check("identity-same-ir-different-closure-differs", g && memcmp(i1, i2, 32) == 0 && memcmp(r1.h.closure.digest, r2.h.closure.digest, 32) != 0 && memcmp(bb1, bb2, 32) != 0);
      check("identity-same-ir-same-closure-dev-vs-build-differs", g && memcmp(i1, i3, 32) == 0 && memcmp(r1.h.closure.digest, r3.h.closure.digest, 32) == 0 && memcmp(bb1, bb3, 32) != 0);
      check("dev-domain-resolves-and-marks-tainted", g && r3.h.resolved);
      run_free(&r3); w_free(&w3);
    }
    run_free(&r1); run_free(&r2); w_free(&w1); w_free(&w2);
}

/* ---- no escape hatch ---- */
static const char *const FORBIDDEN[] = { "unsafe import", "unsafe_import", "skip-verify", "skip_verify", "SKIP_VERIFY", "no-verify", "no_verify",
                                         "--insecure", "getenv", "secure_getenv", "putenv" };
#define N_FORBIDDEN (sizeof FORBIDDEN / sizeof FORBIDDEN[0])
static int scan_buf(const char *buf, size_t n, const char **hit) {
    for (size_t i = 0; i < N_FORBIDDEN; i++) {
        size_t k = strlen(FORBIDDEN[i]);
        for (size_t a = 0; a + k <= n; a++)
            if (memcmp(buf + a, FORBIDDEN[i], k) == 0) { *hit = FORBIDDEN[i]; return 1; }
    }
    return 0;
}
static int scan_file(const char *path, const char **hit) {
    size_t n; uint8_t *b = slurp_file(path, &n);
    if (!b) setup_fail("cannot read a scanned source file");
    int r = scan_buf((const char *)b, n, hit);
    free(b);
    return r;
}
static void t_no_escape(void) {
    static const char *const files[] = { "src/omega_resolve.c", "src/omega_resolve.h", "src/omega_receipt.c", "src/omega_receipt.h",
        "src/omega_blake3.c", "src/omega_blake3.h", "src/omega_resolve_osc.c", "src/omega_resolve_osc.h", "src/oscv_main.c" };
    const char *hit = NULL;
    int found = 0, nfiles = 0;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) { nfiles++; if (scan_file(files[i], &hit)) { found = 1; printf("note: %s contains \"%s\"\n", files[i], hit); } }
    DIR *d = opendir("src/compiler");
    if (!d) setup_fail("src/compiler");
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 3 || (strcmp(de->d_name + l - 2, ".c") && strcmp(de->d_name + l - 2, ".h"))) continue;
        char path[300]; snprintf(path, sizeof path, "src/compiler/%s", de->d_name);
        nfiles++;
        if (scan_file(path, &hit)) { found = 1; printf("note: %s contains \"%s\"\n", path, hit); }
    }
    closedir(d);
    check("no-escape-hatch-in-compiler-and-resolver-sources", !found && nfiles > 20);
    /* the driver and its adapter: no genesis switch, and no option beyond the allow-list */
    {
        static const char *const cli_files[] = { "src/oscv_main.c", "src/omega_resolve_osc.c", "src/omega_resolve_osc.h" };
        static const char *const cli_words[] = { "allow-genesis", "allow_genesis" };
        static const char *const allowed[] = { "--domain", "--lock", "--store", "--receipts", "--blobs", "--meta-out" };
        int clean = 1, opts_ok = 1, n_opts = 0;
        for (size_t f = 0; f < sizeof cli_files / sizeof cli_files[0]; f++) {
            size_t n; uint8_t *b = slurp_file(cli_files[f], &n);
            if (!b) setup_fail("cannot read a driver source");
            for (size_t w = 0; w < sizeof cli_words / sizeof cli_words[0]; w++) {
                size_t k = strlen(cli_words[w]);
                for (size_t a = 0; a + k <= n; a++) if (memcmp(b + a, cli_words[w], k) == 0) clean = 0;
            }
            if (strcmp(cli_files[f], "src/oscv_main.c") == 0)
                for (size_t a = 0; a + 3 <= n; a++)
                    if (b[a] == '"' && b[a + 1] == '-' && b[a + 2] == '-') {
                        size_t e = a + 1;
                        while (e < n && b[e] != '"' && b[e] != ' ') e++;
                        int known = 0;
                        for (size_t o = 0; o < sizeof allowed / sizeof allowed[0]; o++)
                            if (strlen(allowed[o]) == e - (a + 1) && memcmp(allowed[o], b + a + 1, e - (a + 1)) == 0) known = 1;
                        n_opts++;
                        if (!known) opts_ok = 0;
                    }
            free(b);
        }
        check("no-genesis-switch-in-the-driver-sources", clean);
        check("driver-options-are-exactly-the-allow-list", opts_ok && n_opts >= 6);
    }
    const char *h2;
    int control = 1;
    for (size_t i = 0; i < N_FORBIDDEN; i++) {
        char probe[128]; snprintf(probe, sizeof probe, "int x; /* %s */", FORBIDDEN[i]);
        if (!scan_buf(probe, strlen(probe), &h2)) control = 0;
    }
    check("no-escape-scanner-detects-every-forbidden-word", control && !scan_buf("clean text", 10, &h2));
}

/* ---- the real driver, end to end ---- */
static int sh(const char *cmd, char *out, size_t cap) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    int st = pclose(p);
    return st == -1 ? -1 : (st >> 8) & 0xff;
}
static int wfile(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = fwrite(d, 1, n, f) == n ? 0 : -1;
    return fclose(f) == 0 && rc == 0 ? 0 : -1;
}
static int kv(const char *out, const char *key, char *val, size_t cap) {
    size_t k = strlen(key);
    const char *p = out;
    while (p && *p) {
        if (strncmp(p, key, k) == 0 && p[k] == '=') {
            const char *e = strchr(p + k + 1, '\n');
            size_t l = e ? (size_t)(e - (p + k + 1)) : strlen(p + k + 1);
            if (l >= cap) l = cap - 1;
            memcpy(val, p + k + 1, l); val[l] = 0;
            return 0;
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return -1;
}
static void write_receipts(const W *w, const char *dir) {
    char path[700], h[65];
    for (size_t i = 0; i < w->rt.n; i++) {
        hx(w->rt.v[i].id, h);
        snprintf(path, sizeof path, "%.200s/%s.json", dir, h);
        if (wfile(path, w->rt.v[i].p, w->rt.v[i].n)) setup_fail("write receipt");
    }
}
static void t_cli(void) {
    if (access(OSCV_PATH, X_OK) != 0) setup_fail("build/compiler/oscv is missing (make test-resolve builds it)");
    char dir[] = "/tmp/omega-resolve-cli-XXXXXX";
    if (!mkdtemp(dir)) setup_fail("mkdtemp");
    char path[700], rdir[700], cmd[2400], out[4096], v[200];
    snprintf(rdir, sizeof rdir, "%s/receipts", dir);
    if (mkdir(rdir, 0700)) setup_fail("mkdir");
    W w; w_init(&w); Opt o = opt0();
    add_comp(&w, 0x51, NULL, 0, &o);
    Opt t = opt0(); t.taint = 1; add_comp(&w, 0x52, NULL, 0, &t);
    write_receipts(&w, rdir);
    snprintf(path, sizeof path, "%s/omega.vcstore", dir);
    if (omega_vcstore_save(w.st, path)) setup_fail("save store");
    Pin pin[1] = { { "foo", 0x51, 0 } };
    const char *lk = lock_text(&w, pin, 1);
    snprintf(path, sizeof path, "%s/omega.lock", dir);
    wfile(path, lk, strlen(lk));
    char src[400]; snprintf(src, sizeof src, "import foo;\n%s", FN);
    snprintf(path, sizeof path, "%s/a.osc", dir);
    wfile(path, src, strlen(src));
    snprintf(path, sizeof path, "%s/plain.osc", dir);
    wfile(path, FN, strlen(FN));

    /* expected identities, derived in this process */
    uint8_t ir[32], cd[32], bid[32], biddev[32]; char irh[65], cdh[65], bidh[65], biddevh[65];
    Run r; { const char *nm[1] = { "foo" }; run_compile(&w, src_imports(nm, 1), OMEGA_DOMAIN_BUILD, lk, &r); }
    if (r.rc != 0 || ir_of(&r, ir)) setup_fail("cli expectation compile");
    memcpy(cd, r.h.closure.digest, 32);
    omega_build_id(ir, OMEGA_DOMAIN_BUILD, cd, bid); omega_build_id(ir, OMEGA_DOMAIN_DEV, cd, biddev);
    hx(ir, irh); hx(cd, cdh); hx(bid, bidh); hx(biddev, biddevh);
    run_free(&r);

    snprintf(cmd, sizeof cmd, "%s %s/a.osc 2>&1", OSCV_PATH, dir);
    int rc = sh(cmd, out, sizeof out);
    int ok = rc == 0;
    ok = ok && kv(out, "ir_sha256", v, sizeof v) == 0 && strcmp(v, irh) == 0;
    ok = ok && kv(out, "closure_sha256", v, sizeof v) == 0 && strcmp(v, cdh) == 0;
    ok = ok && kv(out, "build_id", v, sizeof v) == 0 && strcmp(v, bidh) == 0;
    check("cli-build-resolves-with-expected-identity", ok);
    snprintf(cmd, sizeof cmd, "%s --domain dev %s/a.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    ok = rc == 0 && kv(out, "build_id", v, sizeof v) == 0 && strcmp(v, biddevh) == 0 && strcmp(v, bidh) != 0 &&
         kv(out, "tainted", v, sizeof v) == 0 && strcmp(v, "1") == 0;
    check("cli-dev-domain-is-tainted-with-its-own-build-id", ok);
    snprintf(cmd, sizeof cmd, "%s %s/a.osc 2>&1", OSCV_PATH, dir);
    sh(cmd, out, sizeof out);
    kv(out, "tainted", v, sizeof v);
    check("cli-build-domain-is-not-tainted", strcmp(v, "0") == 0);
    /* meta header file */
    snprintf(cmd, sizeof cmd, "%s --domain dev --meta-out %s/meta.txt %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    snprintf(path, sizeof path, "%s/meta.txt", dir);
    size_t mn; uint8_t *mb = slurp_file(path, &mn);
    OmegaArtifactMeta pm; OmegaResolveError e;
    check("cli-meta-header-is-parsable-and-tainted", rc == 0 && mb && omega_artifact_meta_parse((const char *)mb, mn, &pm, &e) == 0 && pm.tainted == 1 && memcmp(pm.build_id, biddev, 32) == 0);
    free(mb);
    /* no-import unit: identical to the plain compiler's identity */
    snprintf(cmd, sizeof cmd, "%s --lock %s/missing.lock %s/plain.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    OscUnit *u = malloc(sizeof *u); OscDiag dg; uint8_t pir[32]; char pirh[65];
    int pc = osc_compile(FN, strlen(FN), u, &dg, NULL) == 0 && osc_ir_digest(u, pir) == 0;
    hx(pir, pirh);
    free(u);
    check("cli-plain-unit-has-the-plain-compilers-ir-digest", rc == 0 && pc && kv(out, "ir_sha256", v, sizeof v) == 0 && strcmp(v, pirh) == 0);
    /* refusals */
    snprintf(path, sizeof path, "%s/none.lock", dir);
    snprintf(cmd, sizeof cmd, "%s --lock %s/missing.lock %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-import-without-lock", rc == 1 && strstr(out, "resolve_code=DEPENDENCY_NOT_PINNED") != NULL && strstr(out, "ir_sha256") == NULL);
    snprintf(path, sizeof path, "%s/tainted.osc", dir);
    snprintf(src, sizeof src, "import foo;\n%s", FN); wfile(path, src, strlen(src));
    { Pin pt[1] = { { "foo", 0x52, 0 } }; const char *lt = lock_text(&w, pt, 1);
      snprintf(path, sizeof path, "%s/t.lock", dir); wfile(path, lt, strlen(lt)); }
    snprintf(cmd, sizeof cmd, "%s --lock %s/t.lock %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-tainted-record", rc == 1 && strstr(out, "resolve_code=TAINTED_ARTIFACT") != NULL);
    snprintf(cmd, sizeof cmd, "%s --lock %s/t.lock --domain dev %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-tainted-record-in-dev-domain-too", rc == 1 && strstr(out, "resolve_code=TAINTED_ARTIFACT") != NULL);
    snprintf(path, sizeof path, "%s/u.osc", dir);
    wfile(path, FN, strlen(FN));
    snprintf(cmd, sizeof cmd, "%s %s/u.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-lock-line-with-no-import", rc == 1 && strstr(out, "resolve_code=UNDECLARED_IMPORT") != NULL);
    /* a bad receipt file */
    snprintf(path, sizeof path, "%s/rcpt2", dir);
    if (mkdir(path, 0700)) setup_fail("mkdir");
    snprintf(cmd, sizeof cmd, "%s --receipts %s/rcpt2 %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-missing-receipt", rc == 1 && strstr(out, "resolve_code=MISSING_RECEIPT") != NULL);
    /* a store file that does not load is refused, never treated as empty */
    snprintf(path, sizeof path, "%s/omega.vcstore", dir);
    { size_t sn; uint8_t *sb = slurp_file(path, &sn); sb[sn / 2] ^= 1; snprintf(path, sizeof path, "%s/broken.vcstore", dir); wfile(path, sb, sn); free(sb); }
    snprintf(cmd, sizeof cmd, "%s --store %s/broken.vcstore %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-a-damaged-store-file", rc == 1 && strstr(out, "resolve_code=UNVERIFIED_DEPENDENCY") != NULL);
    /* no switch, no environment variable relaxes anything */
    snprintf(cmd, sizeof cmd, "%s --skip-verify %s/a.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-has-no-skip-switch", rc == 2);
    snprintf(cmd, sizeof cmd, "%s --insecure %s/a.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-has-no-insecure-switch", rc == 2);
    snprintf(cmd, sizeof cmd, "OMEGA_SKIP_VERIFY=1 SKIP_VERIFY=1 OMEGA_UNSAFE=1 %s --lock %s/missing.lock %s/a.osc 2>&1", OSCV_PATH, dir, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-ignores-environment-variables", rc == 1 && strstr(out, "resolve_code=DEPENDENCY_NOT_PINNED") != NULL);
    /* genesis: no switch, no way round it from the driver */
    snprintf(cmd, sizeof cmd, "%s --allow-genesis %s/a.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-allow-genesis-as-unknown-option", rc == 2 && strstr(out, "unknown or repeated argument") != NULL && strstr(out, "ir_sha256") == NULL);
    snprintf(cmd, sizeof cmd, "%s --allow_genesis %s/a.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-allow_genesis-spelling-too", rc == 2);
    {
        W g; w_init(&g);
        Opt b = opt0(); b.boot = 1;
        add_comp(&g, 0x91, NULL, 0, &b);
        Pin gp[1] = { { "base", 0x91, 0 } };
        const char *glk = lock_text(&g, gp, 1);
        snprintf(path, sizeof path, "%s/g.vcstore", dir);
        if (omega_vcstore_save(g.st, path)) setup_fail("save genesis store");
        snprintf(path, sizeof path, "%s/g.lock", dir); wfile(path, glk, strlen(glk));
        snprintf(src, sizeof src, "import base;\n%s", FN);
        snprintf(path, sizeof path, "%s/g.osc", dir); wfile(path, src, strlen(src));
        static const char *const flags[3] = { "", "--domain dev", "--domain build" };
        static const char *const envs[2] = { "", "OMEGA_ALLOW_GENESIS=1 ALLOW_GENESIS=1 " };
        int all = 1;
        for (int f = 0; f < 3; f++)
            for (int e2 = 0; e2 < 2; e2++) {
                snprintf(cmd, sizeof cmd, "%s%s %s --store %s/g.vcstore --lock %s/g.lock --receipts %s %s/g.osc 2>&1", envs[e2], OSCV_PATH, flags[f], dir, dir, rdir, dir);
                rc = sh(cmd, out, sizeof out);
                if (!(rc == 1 && strstr(out, "resolve_code=UNVERIFIED_DEPENDENCY") != NULL && strstr(out, "genesis") != NULL && strstr(out, "build_id") == NULL)) all = 0;
            }
        check("cli-genesis-only-store-is-refused-in-every-mode", all);
        w_free(&g);
    }
    snprintf(cmd, sizeof cmd, "%s --domain prod %s/a.osc 2>&1", OSCV_PATH, dir);
    rc = sh(cmd, out, sizeof out);
    check("cli-refuses-unknown-domain", rc == 2);
    /* plain oscc-style refusal of an import, no resolver in the loop is covered in t_frontend */
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) setup_fail("cleanup");
    w_free(&w);
}

/* ======================================================================== main */
static const struct { const char *mutant, *check; } MUTANTS[] = {
    { "b3-merge", "b3-official-vectors" },
    { "b3-root", "b3-official-vectors" },
    { "profile-known", "profile-unknown-refused" },
    { "profile-version", "profile-version-too-old-refused" },
    { "lock-bytes-tab", "lock-refuses-tab-in-comment" },
    { "lock-bytes-cr", "lock-refuses-cr-in-comment" },
    { "lock-bytes-nul", "lock-refuses-nul-in-comment" },
    { "lock-header", "lock-refuses-wrong-header" },
    { "lock-unknown-line", "lock-refuses-bad-name-start" },
    { "lock-hex", "lock-refuses-uppercase-hex" },
    { "lock-order-duplicate", "lock-refuses-duplicate-name" },
    { "lock-order-sorted", "lock-refuses-unsorted" },
    { "missing-receipt", "refuse-missing-receipt" },
    { "rule1-name", "refuse-receipt-file-holds-another-receipt" },
    { "rule-clean", "refuse-dirty-receipt-in-build" },
    { "rule4-semantic", "refuse-receipt-does-not-name-program" },
    { "rule4-source", "refuse-stale-receipt" },
    { "rule3", "refuse-receipt-output-digest-is-not-evidence-root" },
    { "rule5", "refuse-receipt-kind-is-not-profile" },
    { "rule6", "refuse-receipt-has-extra-dependency" },
    { "rule2-pass", "refuse-receipt-result-is-not-pass" },
    { "rule2-assert", "refuse-receipt-assertion-failed" },
    { "rule2-testonly", "refuse-receipt-test-only-trust" },
    { "rule2-tier", "refuse-receipt-tier-below-profile" },
    { "closure-receipt", "closure-digest-matches-reference" },
    { "cycle", "refuse-dependency-cycle" },
    { "lock-receipt", "refuse-lock-pins-another-receipt" },
    { "lock-receipt-visited", "refuse-lock-pins-another-receipt-for-visited-node" },
    { "genesis-gate", "refuse-genesis-record-without-permission" },
    { "taint-cap", "refuse-tainted-record-in-build-domain" },
    { "verified-needs-receipt", "refuse-missing-receipt" },
    { "edge-contract", "refuse-edge-contract-mismatch" },
    { "boot-dep-verified", "refuse-genesis-record-depending-on-verified" },
    { "not-pinned", "refuse-not-pinned-without-lock" },
    { "undeclared", "refuse-undeclared-lock-line-without-import" },
    { "closure-sort", "closure-sorted-ascending-by-semantic-id" },
    { "build-domain", "build-id-matches-reference" },
    { "build-closure", "build-id-matches-reference" },
    { "taint-mark", "artifact-dev-is-tainted-build-is-not" },
    { "meta-taint-consistent", "artifact-refuses-dev-header-marked-untainted" },
    { "meta-build-id", "artifact-refuses-edited-build-id" },
    { "admit-origin", "admit-refuses-record-from-tainted-origin" },
    { "admit-cap", "admit-refuses-taint-capability" },
    { "admit-receipt", "admit-refuses-mismatched-receipt" },
    { "genesis-dep", "admit-genesis-refuses-verified-dependency" },
    { "blob-digest", "refuse-blob-does-not-hash-to-digest" },
    { "json-dup-key", "receipt-refuses-duplicate-key" },
    { "deny-unknown", "receipt-refuses-unknown-field" },
    { "schema", "receipt-refuses-unknown-schema" },
    { "version", "receipt-refuses-unknown-version" },
    { "feature", "receipt-refuses-unknown-feature" },
    { "reserved", "receipt-refuses-reserved-field" },
    { "env-class", "receipt-refuses-env-class-mismatch" },
    { "mutation-order", "receipt-refuses-observed-over-declared" },
    { "canon-sort", "receipt-canon-order-independent" },
    { "id-compare", "receipt-refuses-wrong-id" },
    { "max-depth-removed", "refuse-dependency-chain-too-deep" },
    { "max-depth-one-too-many", "refuse-dependency-chain-too-deep" },
    { "max-depth-one-too-few", "accept-dependency-chain-at-the-limit" },
    { "dup-import", "refuse-duplicate-import-names-through-the-api" },
    { "pass-needs-assertion", "receipt-refuses-pass-without-assertion" },
    { "prod-authority-empty", "receipt-refuses-production-blank-authority" },
    { "prod-authority-trim", "receipt-refuses-production-blank-authority" },
    { "prod-spelling", "receipt-refuses-production-test-only-authority" },
    { "prod-bad-refused", "receipt-refuses-production-test-only-authority" },
};
#define N_MUT (sizeof MUTANTS / sizeof MUTANTS[0])

int main(int argc, char **argv) {
    if (argc > 1) {
        g_mutant = argv[1];
        for (size_t i = 0; i < N_MUT; i++) if (strcmp(MUTANTS[i].mutant, g_mutant) == 0) g_expect_check = MUTANTS[i].check;
        if (!g_expect_check) { fprintf(stderr, "unknown mutant %s\n", g_mutant); return 2; }
    }
    t_blake3();
    t_receipts();
    t_profiles();
    t_lock();
    t_frontend();
    t_refusals();
    t_graphs();
    t_admit();
    t_guards();
    t_identity();
    t_no_escape();
    t_cli();
    if (g_mutant) {
        if (g_expect_failed) { printf("MUTANT %s KILLED by %s\n", g_mutant, g_expect_check); return 1; }
        printf("MUTANT %s SURVIVED (check %s did not fail; %d other checks failed)\n", g_mutant, g_expect_check, g_failed);
        return 3;
    }
    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
