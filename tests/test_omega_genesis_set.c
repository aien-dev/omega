/* test_omega_genesis_set.c: what a LISTED Genesis Set member does (VC1 stage 6).
 *
 * The real VC-GENESIS-1 is empty (src/omega_genesis.h, docs/osc/VC-GENESIS-1.md), so the listed
 * path cannot run against it. This test is built from copies of omega_vcstore.c and
 * omega_resolve.c whose single genesis include line is rewritten to
 * tests/genesis_variant/omega_genesis.h (one member: the id of fixture 0x10). No hook exists in
 * production source: the variant is a different file chosen by the build, never by a flag.
 *
 * It proves, with a member on the list: the store inserts, saves and loads it; omega_resolve_admit_genesis
 * admits it; the resolver lets it satisfy an import in the build domain (with the mandatory source/IR
 * recheck); and with the SAME list an id that is not a member is refused at every one of those doors.
 * Exit 0 only when every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "genesis_variant/omega_genesis.h"
#include "omega_resolve.h"
#include "omega_vcstore.h"
#include "omega_vcstore_priv.h"
#include "vc_fixture.h"

static int g_total, g_failed;
static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    g_total++;
    if (!ok) g_failed++;
}
static void die(const char *what) { fprintf(stderr, "setup failed: %s\n", what); exit(2); }

typedef struct { uint8_t *p; size_t n, cap; } B;
static void bput(B *b, const void *d, size_t k) {
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) die("oom"); }
    memcpy(b->p + b->n, d, k); b->n += k;
}
static void bu32(B *b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, t, 4); }
static void bu64(B *b, uint64_t v) { bu32(b, (uint32_t)(v >> 32)); bu32(b, (uint32_t)v); }
static void bstr(B *b, const char *s) { size_t n = strlen(s); bu64(b, n); bput(b, s, n); }
static void hx(const uint8_t *d, char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

/* a record for fixture n: real program, IR digest, audit-style nonzero receipt_id, no dependencies */
static void make_record(uint8_t n, const uint8_t *dep64, uint32_t n_dep, B *o) {
    const VcFx *f = vcfx(n);
    uint8_t con[32], rcpt[32], root[32], kind = OMEGA_VC_DIGEST_IR;
    memcpy(con, f->id, 32); con[0] ^= 0x80;
    memset(rcpt, 0xA0, 32); rcpt[0] = n;       /* for BOOTSTRAP this is the audit record hash */
    memset(root, 0x66, 32);
    o->n = 0;
    bput(o, "AIEN_VERIFIED_CRUMB_V1", 22);
    bu32(o, 1);
    bput(o, f->id, 32); bput(o, con, 32); bput(o, &kind, 1); bput(o, f->digest, 32);
    bu32(o, 0);
    bu32(o, n_dep); if (n_dep) bput(o, dep64, 64 * (size_t)n_dep);
    bput(o, rcpt, 32); bstr(o, "host-v1"); bstr(o, "1.0.0"); bput(o, root, 32);
    bu32(o, 0);
    bu32(o, 0);
}
static int raw_boot(OmegaVcStore *s, uint8_t n) {
    B b = { 0, 0, 0 }; uint8_t id[32];
    make_record(n, NULL, 0, &b);
    omega_vc_compute_id(b.p, b.n, id);
    int rc = omega_vcstore_insert_bootstrap(s, b.p, b.n, id);
    free(b.p);
    return rc;
}
static int raw_verified(OmegaVcStore *s, uint8_t n) {
    B b = { 0, 0, 0 }; uint8_t id[32];
    make_record(n, NULL, 0, &b);
    omega_vc_compute_id(b.p, b.n, id);
    int rc = omega_vcstore_insert(s, b.p, b.n, id);
    free(b.p);
    return rc;
}
static int admit_genesis(OmegaVcStore *s, uint8_t n, OmegaResolveError *e) {
    B b = { 0, 0, 0 }; uint8_t id[32];
    make_record(n, NULL, 0, &b);
    omega_vc_compute_id(b.p, b.n, id);
    int rc = omega_resolve_admit_genesis(s, b.p, b.n, id, NULL, e);
    free(b.p);
    return rc;
}
static void flip(OmegaVcStore *s, uint8_t n, uint8_t kind) {
    for (size_t i = 0; i < s->count; i++)
        if (memcmp(s->objs[i].semantic_id, vcfx(n)->id, 32) == 0) { s->objs[i].admission_kind = kind; return; }
    die("flip: no such object");
}
static int kind_of(const OmegaVcStore *s, uint8_t n) {
    uint8_t k = 0;
    return omega_vcstore_admission_kind(s, vcfx(n)->id, &k) == 0 ? (int)k : -1;
}

static void t_list(void) {
    check("variant-list-has-one-member", omega_genesis_count() == 1 && omega_genesis_member(0) != NULL && omega_genesis_member(1) == NULL);
    check("variant-member-is-the-program-id-of-fixture-0x10", memcmp(omega_genesis_member(0), vcfx(0x10)->id, 32) == 0);
    check("listed-id-is-contained", omega_genesis_contains(vcfx(0x10)->id) == 1);
    check("other-real-programs-are-not-contained", omega_genesis_contains(vcfx(0x11)->id) == 0 && omega_genesis_contains(vcfx(0x00)->id) == 0);
    uint8_t x[32]; int all = 1;
    for (int i = 0; i < 32; i++) {   /* no prefix or partial match: one bit anywhere makes it a non-member */
        memcpy(x, vcfx(0x10)->id, 32); x[i] ^= 1;
        if (omega_genesis_contains(x)) all = 0;
    }
    check("one-bit-different-id-is-not-contained-at-any-position", all);
    uint8_t z[32] = { 0 };
    check("zero-id-and-null-are-never-members", omega_genesis_contains(z) == 0 && omega_genesis_contains(NULL) == 0);
}

static void t_store(void) {
    OmegaVcStore s; if (omega_vcstore_init(&s)) die("init");
    check("store-inserts-a-listed-bootstrap-record", raw_boot(&s, 0x10) == 0 && kind_of(&s, 0x10) == (int)OMEGA_VCS_ADMISSION_BOOTSTRAP);
    check("store-refuses-an-unlisted-bootstrap-record", raw_boot(&s, 0x11) == OMEGA_VCS_GENESIS_NOT_LISTED && omega_vcstore_count(&s) == 1);
    check("store-still-inserts-an-unlisted-verified-record", raw_verified(&s, 0x11) == 0 && kind_of(&s, 0x11) == (int)OMEGA_VCS_ADMISSION_VERIFIED);
    check("store-refuses-a-bootstrap-record-over-a-verified-one", raw_boot(&s, 0x11) == OMEGA_VCS_GENESIS_NOT_LISTED);
    char path[] = "/tmp/omega-gset-XXXXXX";
    int fd = mkstemp(path); if (fd < 0) die("mkstemp"); close(fd);
    OmegaVcStore l;
    check("store-saves-listed-bootstrap-and-verified-and-loads-them", omega_vcstore_save(&s, path) == 0 && omega_vcstore_init(&l) == 0 &&
          omega_vcstore_load(&l, path) == 0 && omega_vcstore_count(&l) == 2 && kind_of(&l, 0x10) == (int)OMEGA_VCS_ADMISSION_BOOTSTRAP);
    omega_vcstore_destroy(&l);
    flip(&s, 0x11, OMEGA_VCS_ADMISSION_BOOTSTRAP);       /* an unlisted id marked BOOTSTRAP, written to a file */
    int sv = omega_vcstore_save(&s, path);
    int init = omega_vcstore_init(&l);
    int ld = omega_vcstore_load(&l, path);
    check("store-load-refuses-a-file-with-an-unlisted-bootstrap-record", sv == 0 && init == 0 && ld == OMEGA_VCS_GENESIS_NOT_LISTED && omega_vcstore_count(&l) == 0);
    omega_vcstore_destroy(&l);
    unlink(path);
    omega_vcstore_destroy(&s);
}

static void t_admit(void) {
    OmegaVcStore s; OmegaResolveError e;
    if (omega_vcstore_init(&s)) die("init");
    memset(&e, 0, sizeof e);
    check("admit-genesis-admits-a-listed-member", admit_genesis(&s, 0x10, &e) == 0 && kind_of(&s, 0x10) == (int)OMEGA_VCS_ADMISSION_BOOTSTRAP);
    memset(&e, 0, sizeof e);
    int rc = admit_genesis(&s, 0x11, &e);
    check("admit-genesis-refuses-a-record-that-is-not-a-member", rc == OMEGA_RES_UNVERIFIED_DEPENDENCY && e.store_code == OMEGA_VCS_GENESIS_NOT_LISTED && strstr(e.message, "pinned genesis set") != NULL &&
          omega_vcstore_count(&s) == 1);
    omega_vcstore_destroy(&s);
}

static const char *g_lock_name = "gen";
static void lock_for(const OmegaVcStore *s, uint8_t n, char *buf, size_t cap) {
    uint8_t rid[32]; char a[65], b[65];
    if (omega_vcstore_receipt_of(s, vcfx(n)->id, rid) != 0) die("receipt_of");
    hx(vcfx(n)->id, a); hx(rid, b);
    snprintf(buf, cap, "omega.lock v1\n%s = semantic %s receipt %s\n", g_lock_name, a, b);
}
static int resolve_one(const OmegaVcStore *s, OmegaDomain d, int blobs, char *lock_text, OmegaResolveError *e) {
    OmegaResolver r; memset(&r, 0, sizeof r);
    r.store = s; r.domain = d;
    if (blobs) r.fetch_blob = vcfx_fetch_blob;
    OmegaLock lock; OmegaResolveError le;
    if (omega_lock_parse(lock_text, strlen(lock_text), &lock, &le)) die("lock");
    const char *names[1] = { g_lock_name };
    OmegaClosure c; memset(&c, 0, sizeof c);
    memset(e, 0, sizeof *e);
    int rc = omega_resolve_imports(&r, &lock, names, 1, &c, e);
    int closure_ok = rc == 0 && c.n == 1 && c.entries[0].admission_kind == OMEGA_VCS_ADMISSION_BOOTSTRAP &&
                     memcmp(c.entries[0].semantic_id, vcfx(0x10)->id, 32) == 0;
    omega_closure_free(&c);
    omega_lock_free(&lock);
    return rc == 0 ? (closure_ok ? 0 : -1) : rc;
}
static void t_resolve(void) {
    OmegaVcStore s; OmegaResolveError e; char lock[400];
    if (omega_vcstore_init(&s)) die("init");
    if (admit_genesis(&s, 0x10, &e) != 0) die("admit listed");
    lock_for(&s, 0x10, lock, sizeof lock);
    check("resolver-lets-a-listed-bootstrap-record-satisfy-an-import-in-the-build-domain", resolve_one(&s, OMEGA_DOMAIN_BUILD, 1, lock, &e) == 0);
    check("a-listed-bootstrap-record-still-needs-the-blob-store-in-the-build-domain", resolve_one(&s, OMEGA_DOMAIN_BUILD, 0, lock, &e) == OMEGA_RES_UNVERIFIED_DEPENDENCY);
    check("resolver-lets-a-listed-bootstrap-record-satisfy-an-import-in-the-dev-domain", resolve_one(&s, OMEGA_DOMAIN_DEV, 0, lock, &e) == 0);
    omega_vcstore_destroy(&s);

    /* the same list, an id that is not on it, kind BOOTSTRAP made by tampering: refused by the resolver itself */
    if (omega_vcstore_init(&s)) die("init");
    if (raw_verified(&s, 0x10) != 0) die("insert verified 0x10");
    flip(&s, 0x10, OMEGA_VCS_ADMISSION_BOOTSTRAP);     /* listed id: fine */
    lock_for(&s, 0x10, lock, sizeof lock);
    check("resolver-accepts-a-listed-id-whatever-route-put-it-in-the-store", resolve_one(&s, OMEGA_DOMAIN_BUILD, 1, lock, &e) == 0);
    omega_vcstore_destroy(&s);

    if (omega_vcstore_init(&s)) die("init");
    if (raw_verified(&s, 0x11) != 0) die("insert verified 0x11");
    flip(&s, 0x11, OMEGA_VCS_ADMISSION_BOOTSTRAP);
    lock_for(&s, 0x11, lock, sizeof lock);
    OmegaResolver r; memset(&r, 0, sizeof r); r.store = &s; r.domain = OMEGA_DOMAIN_BUILD; r.fetch_blob = vcfx_fetch_blob;
    OmegaLock lk; OmegaResolveError le;
    if (omega_lock_parse(lock, strlen(lock), &lk, &le)) die("lock");
    const char *names[1] = { g_lock_name };
    OmegaClosure c; memset(&c, 0, sizeof c); memset(&e, 0, sizeof e);
    int rc = omega_resolve_imports(&r, &lk, names, 1, &c, &e);
    check("resolver-refuses-an-unlisted-bootstrap-record-even-with-the-list-non-empty",
          rc == OMEGA_RES_UNVERIFIED_DEPENDENCY && e.store_code == OMEGA_VCS_GENESIS_NOT_LISTED && c.n == 0);
    omega_closure_free(&c); omega_lock_free(&lk);
    omega_vcstore_destroy(&s);
}

int main(void) {
    t_list();
    t_store();
    t_admit();
    t_resolve();
    printf("%d checks, %d failed\n", g_total, g_failed);
    if (g_failed) return 1;
    printf("test-genesis-set: PASS\n");
    return 0;
}
