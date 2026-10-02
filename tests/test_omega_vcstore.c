/* test_omega_vcstore.c -- VC1 stage 3: the Verified Crumb Store.
 *
 * Usage (run from the repository root; golden vectors are read from tests/vcstore/golden/):
 *   test_omega_vcstore            run every check; exit 0 only if all pass
 *   test_omega_vcstore <mutant>   the binary was linked against a deliberately broken copy of
 *                                 src/omega_vcstore.c (built by `make test-vcstore`); run every
 *                                 check and require the mapped check to FAIL. Exit 1 = mutant
 *                                 killed (what the Makefile asserts), 3 = survived, 2 = unknown.
 *
 * Nothing here is hardcoded PASS: accepted ids come from golden/expected.txt (computed by
 * aien-protocols with sha256sum, independent of this code), the state digest and the store file
 * are re-derived here by a second, independent implementation, and graphs are built with a
 * test-side encoder that does not share code with the decoder under test.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "omega_vcstore.h"
#include "sha256.h"

#define GOLDEN "tests/vcstore/golden"

static const struct { const char *mutant, *check; } MUTANTS[] = {
    /* decoder */
    { "refuse-valid",            "accept-v01_minimal" },
    { "refuse-existing-dep",     "accept-v02_deps" },
    { "caps-reversed",           "accept-v03_capabilities" },
    { "kind-source-only",        "accept-v04_ir_kind" },
    { "id-hash",                 "accept-ids-match-golden" },
    { "dec-dep-dup",             "refuse-r01_duplicate_dep" },
    { "dec-dep-order",           "refuse-r02_unsorted_deps" },
    { "dec-receipt",             "refuse-r03_zero_receipt_id" },
    { "dec-dep-self",            "refuse-r04_self_dependency" },
    { "dec-trailing",            "refuse-r05_trailing_byte" },
    { "dec-version",             "refuse-r06_unknown_version" },
    { "dec-truncated",           "refuse-r07_truncated" },
    { "dec-tag",                 "refuse-r08_bad_domain_tag" },
    { "dec-exports-order",       "refuse-r09_unsorted_exports" },
    { "dec-kind",                "refuse-r10_bad_digest_kind" },
    { "dec-zero-ids",            "refuse-r11_zero_id" },
    { "dec-string-char",         "refuse-r12_bad_string" },
    { "dec-count",               "refuse-r13_too_many_entries" },
    { "dec-real-order",          "refuse-unsorted-realizations" },
    { "dec-caps-order",          "refuse-unsorted-capabilities" },
    { "dec-dep-zero",            "refuse-zero-dependency-id" },
    { "dec-evroot",              "refuse-zero-evidence-root" },
    { "dec-string-empty",        "refuse-bad-string-empty" },
    { "dec-string-max",          "refuse-bad-string-too-long" },
    /* insert */
    { "claimed-required",        "null-claimed-id-refused" },
    { "insert-idcheck",          "claimed-id-mismatch-refused" },
    { "idempotent",              "identical-insert-is-noop" },
    { "idempotent-ignores-kind", "kind-switch-refused" },
    { "immutable",               "immutable-conflict-refused" },
    { "dep-exists",              "missing-dep-refused" },
    { "dep-contract",            "dep-contract-mismatch-refused" },
    { "no-cap",                  "store-grows-past-old-caps" },
    { "sorted-insert",           "digest-independent-of-insert-order" },
    { "kind-bootstrap",          "bootstrap-kind-recorded" },
    /* get, receipt */
    { "get-recompute",           "get-detects-corrupt-bytes" },
    { "get-key",                 "get-detects-key-mismatch" },
    { "receipt-of",              "receipt-of-returns-receipt-id" },
    /* closure */
    { "walk-order",              "closure-order-deterministic" },
    { "walk-visited",            "closure-visits-each-id-once" },
    { "walk-capacity",           "closure-capacity-refused" },
    { "walk-dep-exists",         "closure-missing-dep-refused" },
    { "walk-contract",           "closure-contract-mismatch-refused" },
    { "walk-cycle",              "closure-cycle-refused" },
    { "walk-names",              "names-do-not-affect-resolution" },
    /* digests */
    { "digest-magic",            "digest-matches-reference" },
    { "digest-kind",             "bootstrap-vs-verified-digest-differ" },
    { "digest-bytes",            "digest-covers-bytes" },
    { "digest-names",            "name-index-never-changes-object-digest" },
    { "namedigest-id",           "name-index-digest-covers-id" },
    { "name-sorted",             "name-index-digest-order-independent" },
    /* names */
    { "name-many",               "name-bind-many-to-one" },
    { "name-no-silent-rebind",   "name-no-silent-rebind" },
    { "rebind-write",            "name-rebind-explicit" },
    { "rebind-exists",           "name-rebind-unbound-refused" },
    { "rebind-id-exists",        "name-rebind-unknown-id-refused" },
    { "name-id-exists",          "name-bind-unknown-id-refused" },
    { "name-valid",              "name-invalid-refused" },
    { "resolve",                 "resolve-name-returns-id" },
    /* persistence */
    { "load-names",              "save-load-round-trip" },
    { "load-fail",               "load-failure-leaves-store-unchanged" },
    { "load-magic",              "tamper-every-byte-and-prefix-refused" },
    { "load-digest-obj",         "tamper-object-digest-refused" },
    { "load-digest-names",       "tamper-name-digest-refused" },
    { "load-trailing",           "trailing-byte-refused" },
    { "load-kind",               "load-bad-kind-refused" },
    { "load-order",              "load-unsorted-objects-refused" },
    { "load-graph",              "load-cycle-refused" },
    { "load-graph-missing",      "load-missing-dep-refused" },
    { "load-name-id",            "load-name-unknown-id-refused" },
    { "save-verify",             "save-refuses-corrupt-store" },
};
#define N_MUT (sizeof MUTANTS / sizeof MUTANTS[0])

static const char *g_mutant, *g_expect_check;
static int g_failed, g_expect_failed;

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        g_failed++;
        if (g_expect_check && strcmp(name, g_expect_check) == 0) g_expect_failed = 1;
    }
}
/* A setup step that cannot complete is a hard error. Under a mutant it is accepted only if the
 * mapped check had already failed (the Makefile accepts exactly "KILLED by", exit 1). */
static void setup_fail(const char *what) {
    if (g_mutant && g_expect_failed) { printf("MUTANT %s KILLED by %s (a later fixture then failed: %s)\n", g_mutant, g_expect_check, what); exit(1); }
    if (g_mutant) { printf("MUTANT %s SETUP-BROKEN before check %s ran (%s)\n", g_mutant, g_expect_check, what); exit(4); }
    fprintf(stderr, "setup failed: %s\n", what);
    exit(2);
}

/* ---- byte helpers ---- */
typedef struct { uint8_t *p; size_t n, cap; } B;
static void bput(B *b, const void *d, size_t k) {
    if (b->n + k > b->cap) { b->cap = (b->n + k) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) setup_fail("oom"); }
    if (k) memcpy(b->p + b->n, d, k);
    b->n += k;
}
static void bu32(B *b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, t, 4); }
static void bu64(B *b, uint64_t v) { bu32(b, (uint32_t)(v >> 32)); bu32(b, (uint32_t)v); }
static void bstr(B *b, const char *s) { size_t n = strlen(s); bu64(b, n); bput(b, s, n); }

static void mkid(uint8_t out[32], uint8_t b0) { memset(out, 0x77, 32); out[0] = b0; }
static void mkid2(uint8_t out[32], uint8_t b0, uint8_t b1) { memset(out, 0x77, 32); out[0] = b0; out[1] = b1; }
/* the contract id of a program is its id with the top bit of byte 0 flipped */
static void con_of(const uint8_t sid[32], uint8_t out[32]) { memcpy(out, sid, 32); out[0] ^= 0x80; }

/* ---- test-side encoder (writes lists exactly as given: no sorting, no validation) ---- */
typedef struct {
    uint32_t ver;
    uint8_t sem[32], con[32], src[32], rcpt[32], root[32];
    uint8_t kind;
    uint32_t n_real; const uint8_t (*real)[32];
    uint32_t n_dep;  const uint8_t (*dep)[64];
    const char *prof, *pver;
    uint32_t n_exp; const char *const *exp;
    uint32_t n_cap; const char *const *cap;
} Spec;

static void spec_default(Spec *s, const uint8_t sem[32]) {
    memset(s, 0, sizeof *s);
    s->ver = 1; s->kind = 1;
    memcpy(s->sem, sem, 32);
    con_of(sem, s->con);
    memset(s->src, 0x33, 32);
    memset(s->rcpt, 0x55, 32); s->rcpt[0] = sem[0]; s->rcpt[1] = sem[1];
    memset(s->root, 0x66, 32);
    s->prof = "test-profile"; s->pver = "1.0.0";
}
static void encode(B *o, const Spec *s) {
    o->n = 0;
    bput(o, "AIEN_VERIFIED_CRUMB_V1", 22);
    bu32(o, s->ver);
    bput(o, s->sem, 32); bput(o, s->con, 32); bput(o, &s->kind, 1); bput(o, s->src, 32);
    bu32(o, s->n_real); for (uint32_t i = 0; i < s->n_real; i++) bput(o, s->real[i], 32);
    bu32(o, s->n_dep);  for (uint32_t i = 0; i < s->n_dep; i++) bput(o, s->dep[i], 64);
    bput(o, s->rcpt, 32); bstr(o, s->prof); bstr(o, s->pver); bput(o, s->root, 32);
    bu32(o, s->n_exp); for (uint32_t i = 0; i < s->n_exp; i++) bstr(o, s->exp[i]);
    bu32(o, s->n_cap); for (uint32_t i = 0; i < s->n_cap; i++) bstr(o, s->cap[i]);
}
static int ins_kind(OmegaVcStore *st, const Spec *sp, int bootstrap) {
    B b = { 0, 0, 0 };
    encode(&b, sp);
    uint8_t id[32];
    omega_vc_compute_id(b.p, b.n, id);
    int rc = bootstrap ? omega_vcstore_insert_bootstrap(st, b.p, b.n, id) : omega_vcstore_insert(st, b.p, b.n, id);
    free(b.p);
    return rc;
}
static int ins(OmegaVcStore *st, const Spec *sp) { return ins_kind(st, sp, 0); }

/* a node whose dependencies are given by id; each required contract is the dependency's own contract */
static int node_kind(OmegaVcStore *st, const uint8_t sid[32], const uint8_t (*deps)[32], uint32_t nd, int bootstrap) {
    Spec sp;
    spec_default(&sp, sid);
    uint8_t (*d)[64] = calloc(nd ? nd : 1, 64);
    if (!d) setup_fail("oom");
    for (uint32_t i = 0; i < nd; i++) { memcpy(d[i], deps[i], 32); con_of(deps[i], d[i] + 32); }
    sp.n_dep = nd; sp.dep = (const uint8_t (*)[64])d;
    int rc = ins_kind(st, &sp, bootstrap);
    free(d);
    return rc;
}
static int node(OmegaVcStore *st, const uint8_t sid[32], const uint8_t (*deps)[32], uint32_t nd) { return node_kind(st, sid, deps, nd, 0); }

static OmegaVcStore *new_store(void) {
    OmegaVcStore *s = calloc(1, sizeof *s);
    if (!s || omega_vcstore_init(s) != 0) setup_fail("store");
    return s;
}
static void free_store(OmegaVcStore *s) { omega_vcstore_destroy(s); free(s); }
static void dig(const OmegaVcStore *s, uint8_t out[32]) { if (omega_vcstore_digest(s, out) != 0) setup_fail("digest"); }
static int same_digest(const OmegaVcStore *a, const OmegaVcStore *b) {
    uint8_t x[32], y[32]; dig(a, x); dig(b, y); return memcmp(x, y, 32) == 0;
}

/* the diamond: R needs B and C, both need D. Ids ascend with the letters' order below. */
static uint8_t D[32], Bn[32], C[32], R[32];
static void diamond_ids(void) { mkid(D, 0x10); mkid(Bn, 0x20); mkid(C, 0x30); mkid(R, 0x40); }
static int build_diamond(OmegaVcStore *s, int swap_bc) {
    uint8_t dD[1][32], dR[2][32];
    memcpy(dD[0], D, 32);
    memcpy(dR[0], Bn, 32); memcpy(dR[1], C, 32);
    int rc = node(s, D, NULL, 0);
    if (swap_bc) { rc |= node(s, C, dD, 1); rc |= node(s, Bn, dD, 1); }
    else         { rc |= node(s, Bn, dD, 1); rc |= node(s, C, dD, 1); }
    rc |= node(s, R, dR, 2);
    return rc;
}

/* ---- golden corpus ---- */
static uint8_t *read_hex(const char *path, size_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0; char *t = malloc(cap); int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n' || ch == '\r' || ch == ' ') continue;
        if (n + 1 > cap) { cap *= 2; t = realloc(t, cap); }
        t[n++] = (char)ch;
    }
    fclose(f);
    uint8_t *b = malloc(n / 2 + 1);
    for (size_t i = 0; i + 1 < n; i += 2) {
        int hi = t[i] <= '9' ? t[i] - '0' : t[i] - 'a' + 10, lo = t[i + 1] <= '9' ? t[i + 1] - '0' : t[i + 1] - 'a' + 10;
        b[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    free(t); *n_out = n / 2;
    return b;
}

static void golden(void) {
    char path[512], line[512];
    snprintf(path, sizeof path, "%s/expected.txt", GOLDEN);
    FILE *f = fopen(path, "r");
    if (!f) setup_fail("golden/expected.txt (run from the repository root)");
    int n_acc = 0, n_ref = 0, ids_ok = 1;
    while (fgets(line, sizeof line, f)) {
        char name[256], verb[16], arg[128], cname[300];
        if (sscanf(line, "%255s %15s %127s", name, verb, arg) != 3) continue;
        snprintf(path, sizeof path, "%s/%s.hex", GOLDEN, name);
        size_t n; uint8_t *b = read_hex(path, &n);
        if (!b) setup_fail("golden vector file");
        OmegaVcStore *s = new_store();
        uint8_t claimed[32];
        omega_vc_compute_id(b, n, claimed);
        if (strcmp(verb, "ACCEPT") == 0) {
            n_acc++;
            /* the expected id is the one aien-protocols computed with sha256sum */
            for (int i = 0; i < 32; i++) { unsigned v; sscanf(arg + 2 * i, "%2x", &v); claimed[i] = (uint8_t)v; }
            /* dependencies the vector names must exist first: synthesize them from the vector itself */
            OmegaVcView *v = malloc(sizeof *v);
            if (omega_vc_decode(b, n, v) == 0) {
                for (uint32_t i = 0; i < v->n_dependencies; i++) {
                    Spec sp;
                    spec_default(&sp, v->dependencies + 64 * (size_t)i);
                    memcpy(sp.con, v->dependencies + 64 * (size_t)i + 32, 32);
                    if (ins(s, &sp) != 0) setup_fail("golden dependency");
                }
            }
            free(v);
            int rc = omega_vcstore_insert(s, b, n, claimed);
            snprintf(cname, sizeof cname, "accept-%s", name);
            check(cname, rc == 0);
            OmegaVcRecord *rec = malloc(sizeof *rec);
            uint8_t sid[32];
            OmegaVcView *v2 = malloc(sizeof *v2);
            if (omega_vc_decode(b, n, v2) == 0) memcpy(sid, v2->semantic_id, 32); else memset(sid, 0x99, 32);
            free(v2);
            if (omega_vcstore_get(s, sid, rec) != 0 || memcmp(rec->vc_id, claimed, 32) != 0) ids_ok = 0;
            free(rec);
        } else {
            n_ref++;
            int rc = omega_vcstore_insert(s, b, n, claimed);
            snprintf(cname, sizeof cname, "refuse-%s", name);
            check(cname, rc != 0 && strcmp(omega_vcstore_code_name(rc), arg) == 0 && omega_vcstore_count(s) == 0);
        }
        free(b);
        free_store(s);
    }
    fclose(f);
    if (n_acc != 4 || n_ref != 13) setup_fail("golden corpus is not 4 accept + 13 refuse vectors");
    check("accept-ids-match-golden", ids_ok);
}

/* ---- synthetic refusals ---- */
static int refuse_code(const Spec *sp) {
    OmegaVcStore *s = new_store();
    int rc = ins(s, sp);
    if (omega_vcstore_count(s) != 0 && rc != 0) rc = -1;   /* a refusal must leave the store empty */
    free_store(s);
    return rc;
}

static void synthetic_refusals(void) {
    uint8_t sem[32]; mkid(sem, 0x11);
    Spec sp;
    uint8_t reals[2][32]; mkid(reals[0], 0x90); mkid(reals[1], 0x80);   /* descending */
    spec_default(&sp, sem); sp.n_real = 2; sp.real = (const uint8_t (*)[32])reals;
    check("refuse-unsorted-realizations", refuse_code(&sp) == OMEGA_VCS_NONCANONICAL_SET);
    const char *caps[2] = { "b-cap", "a-cap" };
    spec_default(&sp, sem); sp.n_cap = 2; sp.cap = caps;
    check("refuse-unsorted-capabilities", refuse_code(&sp) == OMEGA_VCS_NONCANONICAL_SET);
    uint8_t dz[1][64]; mkid(dz[0], 0x22); memset(dz[0] + 32, 0, 32);
    spec_default(&sp, sem); sp.n_dep = 1; sp.dep = (const uint8_t (*)[64])dz;
    check("refuse-zero-dependency-id", refuse_code(&sp) == OMEGA_VCS_ZERO_ID);
    spec_default(&sp, sem); memset(sp.root, 0, 32);
    check("refuse-zero-evidence-root", refuse_code(&sp) == OMEGA_VCS_ZERO_ID);
    spec_default(&sp, sem); sp.prof = "";
    check("refuse-bad-string-empty", refuse_code(&sp) == OMEGA_VCS_BAD_STRING);
    char longs[258]; memset(longs, 'a', 257); longs[257] = 0;
    spec_default(&sp, sem); sp.pver = longs;
    check("refuse-bad-string-too-long", refuse_code(&sp) == OMEGA_VCS_BAD_STRING);
}

/* ---- store behaviour ---- */
static const uint8_t ZERO[32];

static void insert_checks(void) {
    uint8_t sem[32]; mkid(sem, 0x11);
    Spec sp; spec_default(&sp, sem);
    B b = { 0, 0, 0 };
    encode(&b, &sp);
    uint8_t id[32]; omega_vc_compute_id(b.p, b.n, id);

    OmegaVcStore *s = new_store();
    check("null-claimed-id-refused",
          omega_vcstore_insert(s, b.p, b.n, NULL) == OMEGA_VCS_VCSTORE_MALFORMED && omega_vcstore_count(s) == 0);
    uint8_t wrong[32]; memcpy(wrong, id, 32); wrong[7] ^= 1;
    check("claimed-id-mismatch-refused",
          omega_vcstore_insert(s, b.p, b.n, wrong) == OMEGA_VCS_VCSTORE_ID_MISMATCH && omega_vcstore_count(s) == 0);

    /* identical bytes twice: success, one object, same digest */
    int r1 = omega_vcstore_insert(s, b.p, b.n, id);
    uint8_t d1[32], d2[32]; dig(s, d1);
    int r2 = omega_vcstore_insert(s, b.p, b.n, id);
    dig(s, d2);
    check("identical-insert-is-noop", r1 == 0 && r2 == 0 && omega_vcstore_count(s) == 1 && memcmp(d1, d2, 32) == 0);

    /* same program id, different record (other receipt and exports): refused, original untouched */
    Spec sp2; spec_default(&sp2, sem); sp2.rcpt[5] ^= 0xFF;
    const char *ex[1] = { "other" }; sp2.n_exp = 1; sp2.exp = ex;
    int rc = ins(s, &sp2);
    OmegaVcRecord *rec = malloc(sizeof *rec);
    int g = omega_vcstore_get(s, sem, rec);
    dig(s, d2);
    check("immutable-conflict-refused",
          rc == OMEGA_VCS_VCSTORE_IMMUTABLE_CONFLICT && g == 0 && memcmp(rec->vc_id, id, 32) == 0 &&
          memcmp(d1, d2, 32) == 0 && omega_vcstore_count(s) == 1);
    free(rec);
    /* same bytes, other admission kind: also a conflict, never a silent relabel */
    uint8_t kind = 0;
    rc = omega_vcstore_insert_bootstrap(s, b.p, b.n, id);
    int g2 = omega_vcstore_admission_kind(s, sem, &kind);
    check("kind-switch-refused", rc == OMEGA_VCS_VCSTORE_IMMUTABLE_CONFLICT && g2 == 0 && kind == OMEGA_VCS_ADMISSION_VERIFIED);
    free_store(s);

    /* receipt_of: the record's receipt_id, nothing else; absent id is refused */
    s = new_store();
    if (ins(s, &sp) != 0) setup_fail("receipt_of insert");
    uint8_t out[32]; memset(out, 0xEE, 32);
    int ro = omega_vcstore_receipt_of(s, sem, out);
    uint8_t missing[32]; mkid(missing, 0xF1);
    uint8_t out2[32]; memset(out2, 0xEE, 32);
    int ro2 = omega_vcstore_receipt_of(s, missing, out2);
    check("receipt-of-returns-receipt-id", ro == 0 && memcmp(out, sp.rcpt, 32) == 0 &&
          ro2 == OMEGA_VCS_UNVERIFIED_DEPENDENCY && out2[0] == 0xEE);
    free_store(s);
    free(b.p);

    /* missing dependency: the golden v02 on its own */
    {
        char path[512]; size_t n;
        snprintf(path, sizeof path, "%s/v02_deps.hex", GOLDEN);
        uint8_t *v2 = read_hex(path, &n);
        if (!v2) setup_fail("v02");
        uint8_t cid[32]; omega_vc_compute_id(v2, n, cid);
        s = new_store();
        rc = omega_vcstore_insert(s, v2, n, cid);
        check("missing-dep-refused", rc == OMEGA_VCS_UNVERIFIED_DEPENDENCY && omega_vcstore_count(s) == 0);
        free_store(s); free(v2);
    }

    /* dependency present but under another contract */
    s = new_store();
    uint8_t dd[32]; mkid(dd, 0x10);
    if (node(s, dd, NULL, 0) != 0) setup_fail("dep node");
    Spec sp3; spec_default(&sp3, sem);
    uint8_t depw[1][64]; memcpy(depw[0], dd, 32); mkid(depw[0] + 32, 0x01);   /* wrong contract */
    sp3.n_dep = 1; sp3.dep = (const uint8_t (*)[64])depw;
    check("dep-contract-mismatch-refused", ins(s, &sp3) == OMEGA_VCS_UNVERIFIED_DEPENDENCY && omega_vcstore_count(s) == 1);
    free_store(s);
}

static void growth_checks(void) {
    /* more objects than the old library cap (128) and one record with the maximum 256 dependencies */
    OmegaVcStore *s = new_store();
    int ok = 1;
    for (int i = 999; i >= 0; i--) {
        uint8_t id[32]; mkid2(id, (uint8_t)(1 + i / 250), (uint8_t)(i % 250 + 1));
        if (node(s, id, NULL, 0) != 0) { ok = 0; break; }
    }
    size_t cnt = omega_vcstore_count(s);
    int sorted = 1;
    for (size_t i = 1; i < s->count; i++) if (memcmp(s->objs[i - 1].semantic_id, s->objs[i].semantic_id, 32) >= 0) sorted = 0;
    check("store-grows-past-old-caps", ok && cnt == 1000 && sorted);
    free_store(s);

    s = new_store();
    uint8_t (*deps)[32] = calloc(256, 32);
    if (!deps) setup_fail("oom");
    for (int i = 0; i < 256; i++) {
        mkid2(deps[i], 0x20, (uint8_t)i);
        if (node(s, deps[i], NULL, 0) != 0) setup_fail("leaf");
    }
    uint8_t top[32]; mkid(top, 0x50);
    int rc = node(s, top, (const uint8_t (*)[32])deps, 256);
    uint8_t *out = malloc(32 * 300); size_t n = 0;
    int rc2 = omega_vcstore_closure(s, top, out, 300, &n);
    check("closure-keeps-all-256-dependencies", rc == 0 && rc2 == 0 && n == 257 && memcmp(out + 32 * 256, top, 32) == 0);
    free(out); free(deps); free_store(s);
}

static void closure_checks(void) {
    diamond_ids();
    uint8_t want[4][32];
    memcpy(want[0], D, 32); memcpy(want[1], Bn, 32); memcpy(want[2], C, 32); memcpy(want[3], R, 32);

    OmegaVcStore *a = new_store(), *b = new_store();
    if (build_diamond(a, 0) != 0 || build_diamond(b, 1) != 0) setup_fail("diamond");
    uint8_t oa[4 * 32], ob[4 * 32]; size_t na = 99, nb = 99;
    int ra = omega_vcstore_closure(a, R, oa, 4, &na), rb = omega_vcstore_closure(b, R, ob, 4, &nb);
    check("closure-order-deterministic",
          ra == 0 && rb == 0 && na == 4 && nb == 4 && memcmp(oa, want, sizeof want) == 0 && memcmp(ob, want, sizeof want) == 0);
    check("closure-visits-each-id-once", ra == 0 && na == 4);   /* D appears once although B and C both need it */

    uint8_t two[2 * 32]; size_t n2 = 0;
    int rcap = omega_vcstore_closure(a, R, two, 2, &n2);
    size_t n0 = 0;
    int rcap0 = omega_vcstore_closure(a, R, NULL, 0, &n0);
    uint8_t exact[4 * 32]; size_t n4 = 0;
    int rex = omega_vcstore_closure(a, R, exact, 4, &n4);
    check("closure-capacity-refused",
          rcap == OMEGA_VCS_VCSTORE_CAPACITY && n2 == 4 && rcap0 == OMEGA_VCS_VCSTORE_CAPACITY && n0 == 4 && rex == 0 && n4 == 4);

    /* the closure of a leaf is itself; an unknown id is refused */
    uint8_t one[32]; size_t n1 = 0;
    int rleaf = omega_vcstore_closure(a, D, one, 1, &n1);
    uint8_t unknown[32]; mkid(unknown, 0xF7); size_t nu = 5;
    int run = omega_vcstore_closure(a, unknown, one, 1, &nu);
    check("closure-leaf-and-unknown", rleaf == 0 && n1 == 1 && memcmp(one, D, 32) == 0 && run == OMEGA_VCS_UNVERIFIED_DEPENDENCY && nu == 0);

    /* names never take part in resolution */
    uint8_t before[4 * 32], after[4 * 32]; size_t nbf = 0, naf = 0;
    int r0 = omega_vcstore_closure(a, R, before, 4, &nbf);
    int ok = omega_vcstore_name_bind(a, "alpha", D) == 0 && omega_vcstore_name_bind(a, "beta", R) == 0 &&
             omega_vcstore_name_rebind(a, "alpha", C) == 0;
    OmegaVcRecord *g1 = malloc(sizeof *g1), *g2 = malloc(sizeof *g2);
    int gr1 = omega_vcstore_get(b, R, g1);
    int r1 = omega_vcstore_closure(a, R, after, 4, &naf);
    int gr2 = omega_vcstore_get(a, R, g2);
    check("names-do-not-affect-resolution",
          ok && r0 == 0 && r1 == 0 && nbf == naf && memcmp(before, after, sizeof before) == 0 &&
          gr1 == 0 && gr2 == 0 && memcmp(g1->vc_id, g2->vc_id, 32) == 0);
    free(g1); free(g2);

    /* damage the graph in memory: drop D */
    {
        OmegaVcStore *m = new_store();
        if (build_diamond(m, 0) != 0) setup_fail("diamond");
        free(m->objs[0].bytes);
        memmove(&m->objs[0], &m->objs[1], (m->count - 1) * sizeof m->objs[0]);
        m->count--;
        uint8_t o[4 * 32]; size_t n = 7;
        int rc = omega_vcstore_closure(m, R, o, 4, &n);
        check("closure-missing-dep-refused", rc == OMEGA_VCS_UNVERIFIED_DEPENDENCY && n == 0);
        free_store(m);
    }
    /* damage the graph in memory: B is replaced by a record with another contract id */
    {
        OmegaVcStore *m = new_store();
        if (build_diamond(m, 0) != 0) setup_fail("diamond");
        Spec sp; spec_default(&sp, Bn);
        mkid(sp.con, 0x0B);   /* not the contract R requires */
        uint8_t dd[1][64]; memcpy(dd[0], D, 32); con_of(D, dd[0] + 32);
        sp.n_dep = 1; sp.dep = (const uint8_t (*)[64])dd;
        B enc = { 0, 0, 0 }; encode(&enc, &sp);
        size_t bi = 1;   /* sorted: D, B, C, R */
        free(m->objs[bi].bytes);
        m->objs[bi].bytes = enc.p; m->objs[bi].len = enc.n;
        omega_vc_compute_id(enc.p, enc.n, m->objs[bi].vc_id);
        uint8_t o[4 * 32]; size_t n = 7;
        int rc = omega_vcstore_closure(m, R, o, 4, &n);
        check("closure-contract-mismatch-refused", rc == OMEGA_VCS_UNVERIFIED_DEPENDENCY && n == 0);
        free_store(m);
    }
    /* damage the graph in memory: Y is replaced by a record that needs X, which needs Y */
    {
        OmegaVcStore *m = new_store();
        uint8_t X[32], Y[32], dy[1][32];
        mkid(X, 0x50); mkid(Y, 0x60); memcpy(dy[0], Y, 32);
        if (node(m, Y, NULL, 0) != 0 || node(m, X, (const uint8_t (*)[32])dy, 1) != 0) setup_fail("xy");
        Spec sp; spec_default(&sp, Y);
        uint8_t dd[1][64]; memcpy(dd[0], X, 32); con_of(X, dd[0] + 32);
        sp.n_dep = 1; sp.dep = (const uint8_t (*)[64])dd;
        B enc = { 0, 0, 0 }; encode(&enc, &sp);
        free(m->objs[1].bytes);                 /* sorted: X, Y */
        m->objs[1].bytes = enc.p; m->objs[1].len = enc.n;
        omega_vc_compute_id(enc.p, enc.n, m->objs[1].vc_id);
        uint8_t o[2 * 32]; size_t n = 7;
        int rc = omega_vcstore_closure(m, X, o, 2, &n);
        check("closure-cycle-refused", rc == OMEGA_VCS_DEPENDENCY_CYCLE && n == 0);
        free_store(m);
    }
    free_store(a); free_store(b);
}

static void integrity_checks(void) {
    uint8_t sem[32]; mkid(sem, 0x11);
    Spec sp; spec_default(&sp, sem);
    OmegaVcStore *s = new_store();
    if (ins(s, &sp) != 0) setup_fail("insert");
    /* flip one bit of the source digest: still a well-formed record, but not the bytes that were hashed */
    s->objs[0].bytes[91] ^= 1;
    OmegaVcRecord *rec = malloc(sizeof *rec);
    check("get-detects-corrupt-bytes", omega_vcstore_get(s, sem, rec) == OMEGA_VCS_VCSTORE_ID_MISMATCH);
    free(rec);
    free_store(s);

    s = new_store();
    if (ins(s, &sp) != 0) setup_fail("insert");
    s->objs[0].semantic_id[0] ^= 1;   /* the key no longer matches the record inside */
    rec = malloc(sizeof *rec);
    check("get-detects-key-mismatch", omega_vcstore_get(s, s->objs[0].semantic_id, rec) == OMEGA_VCS_VCSTORE_ID_MISMATCH);
    free(rec);
    free_store(s);
}

/* ---- digest: second implementation ---- */
static void ref_u64(sha256_ctx *c, uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); sha256_update(c, b, 8); }
typedef struct { const uint8_t *bytes; size_t len; uint8_t kind; } FObj;
typedef struct { const char *name; const uint8_t *id; } FName;
static int fobj_cmp(const void *x, const void *y) { return memcmp(((const FObj *)x)->bytes + 26, ((const FObj *)y)->bytes + 26, 32); }
static void ref_digests(const FObj *objs, size_t n, const FName *names, size_t nn, uint8_t d1[32], uint8_t d2[32]) {
    FObj *sorted = malloc((n ? n : 1) * sizeof *sorted);
    memcpy(sorted, objs, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, fobj_cmp);
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"VCS1", 4); ref_u64(&c, n);
    for (size_t i = 0; i < n; i++) {
        sha256_update(&c, sorted[i].bytes + 26, 32);          /* semantic_id sits at offset 22 + 4 */
        sha256_update(&c, &sorted[i].kind, 1);
        ref_u64(&c, sorted[i].len);
        sha256_update(&c, sorted[i].bytes, sorted[i].len);
    }
    sha256_final(&c, d1);
    free(sorted);
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"VCN1", 4); ref_u64(&c, nn);
    for (size_t i = 0; i < nn; i++) {
        size_t l = strlen(names[i].name);
        ref_u64(&c, l); sha256_update(&c, (const uint8_t *)names[i].name, l); sha256_update(&c, names[i].id, 32);
    }
    sha256_final(&c, d2);
}
/* write a store file by hand (ascending names expected); trailer digests are the reference ones */
static void write_file(const char *path, const FObj *objs, size_t n, const FName *names, size_t nn) {
    B b = { 0, 0, 0 };
    bput(&b, "AIEN_VCSTORE_V1", 15);
    bu64(&b, n);
    for (size_t i = 0; i < n; i++) { bput(&b, &objs[i].kind, 1); bu64(&b, objs[i].len); bput(&b, objs[i].bytes, objs[i].len); }
    bu64(&b, nn);
    for (size_t i = 0; i < nn; i++) { bu64(&b, strlen(names[i].name)); bput(&b, names[i].name, strlen(names[i].name)); bput(&b, names[i].id, 32); }
    uint8_t d1[32], d2[32];
    ref_digests(objs, n, names, nn, d1, d2);
    bput(&b, d1, 32); bput(&b, d2, 32);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b.p, 1, b.n, f) != b.n) setup_fail("write file");
    fclose(f);
    free(b.p);
}
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) setup_fail("slurp");
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)l + 1);
    if (fread(p, 1, (size_t)l, f) != (size_t)l) setup_fail("read");
    fclose(f); *n = (size_t)l;
    return p;
}
static void spit(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || (n && fwrite(p, 1, n, f) != n)) setup_fail("spit");
    fclose(f);
}

static void order_checks(void) {
    diamond_ids();
    /* same set, different insert orders: same digest */
    OmegaVcStore *a = new_store(), *b = new_store(), *c = new_store();
    uint8_t leaf[6][32];
    for (int i = 0; i < 6; i++) mkid2(leaf[i], (uint8_t)(0x80 + i * 7), (uint8_t)(i + 1));
    int ok = build_diamond(a, 0) == 0;
    for (int i = 0; i < 6; i++) ok &= node(a, leaf[i], NULL, 0) == 0;
    for (int i = 5; i >= 0; i--) ok &= node(b, leaf[i], NULL, 0) == 0;
    ok &= build_diamond(b, 1) == 0;
    ok &= node(c, leaf[3], NULL, 0) == 0;
    ok &= build_diamond(c, 0) == 0;
    for (int i = 0; i < 6; i++) if (i != 3) ok &= node(c, leaf[i], NULL, 0) == 0;
    check("digest-independent-of-insert-order", ok && same_digest(a, b) && same_digest(a, c) && omega_vcstore_count(a) == 10);
    free_store(a); free_store(b); free_store(c);
}

static void digest_checks(void) {
    diamond_ids();
    /* reference digest equals the library digest (verified and bootstrap mixed) */
    OmegaVcStore *s = new_store();
    uint8_t dD[1][32], dR[2][32]; memcpy(dD[0], D, 32); memcpy(dR[0], Bn, 32); memcpy(dR[1], C, 32);
    int rc = node_kind(s, D, NULL, 0, 1);
    rc |= node(s, Bn, dD, 1); rc |= node(s, C, dD, 1); rc |= node(s, R, dR, 2);
    if (rc) setup_fail("digest store");
    FObj objs[4]; Spec sp; B enc[4] = { {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0} };
    const uint8_t *ids[4] = { D, Bn, C, R };
    for (int i = 0; i < 4; i++) {
        spec_default(&sp, ids[i]);
        uint8_t (*d)[64] = calloc(2, 64);
        if (i == 1 || i == 2) { memcpy(d[0], D, 32); con_of(D, d[0] + 32); sp.n_dep = 1; }
        if (i == 3) { memcpy(d[0], Bn, 32); con_of(Bn, d[0] + 32); memcpy(d[1], C, 32); con_of(C, d[1] + 32); sp.n_dep = 2; }
        sp.dep = (const uint8_t (*)[64])d;
        encode(&enc[i], &sp);
        free(d);
        objs[i].bytes = enc[i].p; objs[i].len = enc[i].n; objs[i].kind = i == 0 ? 2 : 1;
    }
    uint8_t r1[32], r2[32], l1[32];
    ref_digests(objs, 4, NULL, 0, r1, r2);
    dig(s, l1);
    check("digest-matches-reference", memcmp(r1, l1, 32) == 0);

    /* the digest covers the bytes of every object */
    OmegaVcStore *m = new_store();
    if (build_diamond(m, 0) != 0) setup_fail("diamond");
    uint8_t x1[32], x2[32];
    dig(m, x1);
    m->objs[2].bytes[91] ^= 1;
    dig(m, x2);
    check("digest-covers-bytes", memcmp(x1, x2, 32) != 0);
    free_store(m);


    /* the same record admitted as BOOTSTRAP and as VERIFIED never shares a digest */
    OmegaVcStore *v = new_store(), *bo = new_store();
    int rv = node_kind(v, D, NULL, 0, 0), rb = node_kind(bo, D, NULL, 0, 1);
    uint8_t kv = 0, kb = 0;
    omega_vcstore_admission_kind(v, D, &kv); omega_vcstore_admission_kind(bo, D, &kb);
    check("bootstrap-kind-recorded", rv == 0 && rb == 0 && kv == OMEGA_VCS_ADMISSION_VERIFIED && kb == OMEGA_VCS_ADMISSION_BOOTSTRAP);
    check("bootstrap-vs-verified-digest-differ", rv == 0 && rb == 0 && !same_digest(v, bo));
    free_store(v); free_store(bo);

    for (int i = 0; i < 4; i++) free(enc[i].p);
    free_store(s);
}

static void name_checks(void) {
    diamond_ids();
    OmegaVcStore *s = new_store();
    if (build_diamond(s, 0) != 0) setup_fail("diamond");
    uint8_t od0[32], od1[32], od2[32];
    dig(s, od0);

    /* many names, one id */
    int r1 = omega_vcstore_name_bind(s, "first", D), r2 = omega_vcstore_name_bind(s, "second", D), r3 = omega_vcstore_name_bind(s, "third.v-2", D);
    uint8_t g1[32], g2[32], g3[32];
    int q1 = omega_vcstore_resolve_name(s, "first", g1), q2 = omega_vcstore_resolve_name(s, "second", g2), q3 = omega_vcstore_resolve_name(s, "third.v-2", g3);
    check("name-bind-many-to-one", r1 == 0 && r2 == 0 && r3 == 0 && q1 == 0 && q2 == 0 && q3 == 0 &&
          memcmp(g1, D, 32) == 0 && memcmp(g2, D, 32) == 0 && memcmp(g3, D, 32) == 0 && s->n_names == 3 &&
          omega_vcstore_name_bind(s, "first", D) == 0);   /* same name, same id: no-op */
    dig(s, od1);
    check("name-index-never-changes-object-digest", memcmp(od0, od1, 32) == 0);

    /* a bound name cannot be taken over by a plain bind */
    int rx = omega_vcstore_name_bind(s, "first", R);
    uint8_t gx[32];
    int qx = omega_vcstore_resolve_name(s, "first", gx);
    check("name-no-silent-rebind", rx == OMEGA_VCS_VCSTORE_NAME_EXISTS && qx == 0 && memcmp(gx, D, 32) == 0);

    /* explicit rebind works, changes the name digest, still not the object digest */
    uint8_t n1[32], n2[32];
    omega_vcstore_name_index_digest(s, n1);
    int rr = omega_vcstore_name_rebind(s, "first", R);
    int qr = omega_vcstore_resolve_name(s, "first", gx);
    omega_vcstore_name_index_digest(s, n2);
    dig(s, od2);
    check("name-rebind-explicit", rr == 0 && qr == 0 && memcmp(gx, R, 32) == 0);
    check("name-index-digest-covers-id", memcmp(n1, n2, 32) != 0 && memcmp(od0, od2, 32) == 0);

    uint8_t nobody[32]; mkid(nobody, 0xF3);
    size_t before = s->n_names;
    check("name-rebind-unbound-refused", omega_vcstore_name_rebind(s, "never-bound", D) == OMEGA_VCS_VCSTORE_NOT_FOUND && s->n_names == before);
    check("name-rebind-unknown-id-refused", omega_vcstore_name_rebind(s, "first", nobody) == OMEGA_VCS_UNVERIFIED_DEPENDENCY &&
          omega_vcstore_resolve_name(s, "first", gx) == 0 && memcmp(gx, R, 32) == 0);
    check("name-bind-unknown-id-refused", omega_vcstore_name_bind(s, "ghost", nobody) == OMEGA_VCS_UNVERIFIED_DEPENDENCY &&
          s->n_names == before && omega_vcstore_resolve_name(s, "ghost", gx) == OMEGA_VCS_VCSTORE_NOT_FOUND);

    char long129[130]; memset(long129, 'a', 129); long129[129] = 0;
    char long128[129]; memset(long128, 'a', 128); long128[128] = 0;
    int bad = 1;
    const char *bads[] = { "", "1abc", "a b", "-x", ".x", "a/b", "caf\xc3\xa9", long129 };
    for (size_t i = 0; i < sizeof bads / sizeof *bads; i++) bad &= omega_vcstore_name_bind(s, bads[i], D) == OMEGA_VCS_VCSTORE_MALFORMED;
    check("name-invalid-refused", bad && omega_vcstore_name_bind(s, long128, D) == 0 && omega_vcstore_name_bind(s, "_ok.Name-9", D) == 0);

    uint8_t rg[32]; memset(rg, 0xEE, 32);
    int none = omega_vcstore_resolve_name(s, "does-not-exist", rg);
    uint8_t rg2[32];
    int found = omega_vcstore_resolve_name(s, "second", rg2);
    check("resolve-name-returns-id", none == OMEGA_VCS_VCSTORE_NOT_FOUND && rg[0] == 0xEE && found == 0 && memcmp(rg2, D, 32) == 0);
    free_store(s);

    /* the name digest does not depend on the order names were bound */
    OmegaVcStore *a = new_store(), *b = new_store();
    if (build_diamond(a, 0) != 0 || build_diamond(b, 0) != 0) setup_fail("diamond");
    omega_vcstore_name_bind(a, "zeta", D); omega_vcstore_name_bind(a, "alpha", R); omega_vcstore_name_bind(a, "mid", C);
    omega_vcstore_name_bind(b, "mid", C); omega_vcstore_name_bind(b, "alpha", R); omega_vcstore_name_bind(b, "zeta", D);
    uint8_t da[32], db[32];
    omega_vcstore_name_index_digest(a, da); omega_vcstore_name_index_digest(b, db);
    check("name-index-digest-order-independent", memcmp(da, db, 32) == 0);
    free_store(a); free_store(b);
}

/* ---- persistence ---- */
static char g_path[256];

static void persistence_checks(void) {
    diamond_ids();
    snprintf(g_path, sizeof g_path, "/tmp/omega_vcstore_test_%d.bin", (int)getpid());
    char g_path2[300]; snprintf(g_path2, sizeof g_path2, "%s.t", g_path);
    OmegaVcStore *s = new_store();
    uint8_t dD[1][32]; memcpy(dD[0], D, 32);
    uint8_t dR[2][32]; memcpy(dR[0], Bn, 32); memcpy(dR[1], C, 32);
    int rc = node_kind(s, D, NULL, 0, 1);
    rc |= node(s, Bn, dD, 1); rc |= node(s, C, dD, 1); rc |= node(s, R, dR, 2);
    rc |= omega_vcstore_name_bind(s, "root", R); rc |= omega_vcstore_name_bind(s, "leaf", D);
    if (rc) setup_fail("persist store");

    /* round trip into a store that already holds something else */
    int sv = omega_vcstore_save(s, g_path);
    OmegaVcStore *t = new_store();
    uint8_t other[32]; mkid(other, 0xC4);
    node(t, other, NULL, 0);
    int ld = omega_vcstore_load(t, g_path);
    uint8_t o1[32], o2[32], n1[32], n2[32];
    dig(s, o1); dig(t, o2); omega_vcstore_name_index_digest(s, n1); omega_vcstore_name_index_digest(t, n2);
    uint8_t rc1[32], rc2[32], res[32]; uint8_t kind = 0;
    int a1 = omega_vcstore_receipt_of(s, R, rc1), a2 = omega_vcstore_receipt_of(t, R, rc2);
    int a3 = omega_vcstore_resolve_name(t, "root", res);
    int a4 = omega_vcstore_admission_kind(t, D, &kind);
    uint8_t cl[4 * 32]; size_t ncl = 0;
    int a5 = omega_vcstore_closure(t, R, cl, 4, &ncl);
    check("save-load-round-trip", sv == 0 && ld == 0 && omega_vcstore_count(t) == 4 && memcmp(o1, o2, 32) == 0 && memcmp(n1, n2, 32) == 0 &&
          a1 == 0 && a2 == 0 && memcmp(rc1, rc2, 32) == 0 && a3 == 0 && memcmp(res, R, 32) == 0 &&
          a4 == 0 && kind == OMEGA_VCS_ADMISSION_BOOTSTRAP && a5 == 0 && ncl == 4 && memcmp(cl, D, 32) == 0);
    free_store(t);

    size_t fn; uint8_t *file = slurp(g_path, &fn);

    /* a failed load leaves the target as it was */
    uint8_t bad1[1] = { 0 };
    spit(g_path2, bad1, 1);
    t = new_store();
    node(t, other, NULL, 0);
    uint8_t t1[32], t2[32]; dig(t, t1);
    int lf = omega_vcstore_load(t, g_path2);
    dig(t, t2);
    check("load-failure-leaves-store-unchanged", lf != 0 && memcmp(t1, t2, 32) == 0 && omega_vcstore_count(t) == 1);
    free_store(t);

    /* every single-bit change and every proper prefix is refused, and refusal leaves nothing behind */
    int all = 1;
    for (size_t i = 0; i < fn && all; i++) {
        file[i] ^= 1;
        spit(g_path2, file, fn);
        file[i] ^= 1;
        t = new_store();
        if (omega_vcstore_load(t, g_path2) == 0 || omega_vcstore_count(t) != 0) all = 0;
        free_store(t);
    }
    for (size_t k = 0; k < fn && all; k++) {
        spit(g_path2, file, k);
        t = new_store();
        if (omega_vcstore_load(t, g_path2) == 0 || omega_vcstore_count(t) != 0) all = 0;
        free_store(t);
    }
    check("tamper-every-byte-and-prefix-refused", all);

    /* digest trailers: flip one byte of each; the object trailer also when the record stays well-formed */
    file[fn - 64] ^= 1; spit(g_path2, file, fn); file[fn - 64] ^= 1;
    t = new_store();
    int e1 = omega_vcstore_load(t, g_path2);
    free_store(t);
    check("tamper-object-digest-refused", e1 == OMEGA_VCS_VCSTORE_ID_MISMATCH);
    file[fn - 1] ^= 1; spit(g_path2, file, fn); file[fn - 1] ^= 1;
    t = new_store();
    int e2 = omega_vcstore_load(t, g_path2);
    free_store(t);
    check("tamper-name-digest-refused", e2 == OMEGA_VCS_VCSTORE_ID_MISMATCH);

    uint8_t *plus = malloc(fn + 1);
    memcpy(plus, file, fn); plus[fn] = 0;
    spit(g_path2, plus, fn + 1);
    t = new_store();
    int e3 = omega_vcstore_load(t, g_path2);
    free_store(t);
    check("trailing-byte-refused", e3 == OMEGA_VCS_VCSTORE_MALFORMED);
    free(plus);
    free(file);

    /* a corrupt store is never written out */
    OmegaVcStore *c = new_store();
    uint8_t sem[32]; mkid(sem, 0x11);
    Spec sp; spec_default(&sp, sem);
    ins(c, &sp);
    c->objs[0].bytes[91] ^= 1;
    remove(g_path2);
    int sc = omega_vcstore_save(c, g_path2);
    check("save-refuses-corrupt-store", sc == OMEGA_VCS_VCSTORE_ID_MISMATCH && access(g_path2, F_OK) != 0);
    free_store(c);

    /* hand-built files: each checks one load refusal with correct reference digests */
    Spec spX, spY, spYc, spA, spB;
    uint8_t X[32], Y[32]; mkid(X, 0x50); mkid(Y, 0x60);
    uint8_t dxy[1][64]; memcpy(dxy[0], Y, 32); con_of(Y, dxy[0] + 32);
    uint8_t dyx[1][64]; memcpy(dyx[0], X, 32); con_of(X, dyx[0] + 32);
    spec_default(&spX, X); spX.n_dep = 1; spX.dep = (const uint8_t (*)[64])dxy;
    spec_default(&spYc, Y); spYc.n_dep = 1; spYc.dep = (const uint8_t (*)[64])dyx;
    spec_default(&spY, Y);
    spec_default(&spA, other);
    uint8_t other2[32]; mkid(other2, 0xC9); spec_default(&spB, other2);
    B bx = { 0, 0, 0 }, by = { 0, 0, 0 }, byc = { 0, 0, 0 }, ba = { 0, 0, 0 }, bb = { 0, 0, 0 };
    encode(&bx, &spX); encode(&by, &spY); encode(&byc, &spYc); encode(&ba, &spA); encode(&bb, &spB);

    { FObj o[2] = { { bx.p, bx.n, 1 }, { byc.p, byc.n, 1 } };   /* X -> Y -> X */
      write_file(g_path2, o, 2, NULL, 0);
      t = new_store(); int r = omega_vcstore_load(t, g_path2);
      check("load-cycle-refused", r == OMEGA_VCS_DEPENDENCY_CYCLE && omega_vcstore_count(t) == 0); free_store(t); }
    { FObj o[1] = { { bx.p, bx.n, 1 } };                        /* X needs Y, Y absent */
      write_file(g_path2, o, 1, NULL, 0);
      t = new_store(); int r = omega_vcstore_load(t, g_path2);
      check("load-missing-dep-refused", r == OMEGA_VCS_UNVERIFIED_DEPENDENCY && omega_vcstore_count(t) == 0); free_store(t); }
    { FObj o[2] = { { ba.p, ba.n, 3 }, { bb.p, bb.n, 1 } };     /* admission kind 3 does not exist */
      write_file(g_path2, o, 2, NULL, 0);
      t = new_store(); int r = omega_vcstore_load(t, g_path2);
      check("load-bad-kind-refused", r == OMEGA_VCS_VCSTORE_MALFORMED); free_store(t); }
    { FObj o[2] = { { bb.p, bb.n, 1 }, { ba.p, ba.n, 1 } };     /* 0xC9 before 0xC4: not ascending */
      write_file(g_path2, o, 2, NULL, 0);
      t = new_store(); int r = omega_vcstore_load(t, g_path2);
      check("load-unsorted-objects-refused", r == OMEGA_VCS_VCSTORE_MALFORMED); free_store(t); }
    { FObj o[1] = { { ba.p, ba.n, 1 } };                        /* name bound to an id that is not stored */
      uint8_t ghost[32]; mkid(ghost, 0xF9);
      FName nm[1] = { { "ghost", ghost } };
      write_file(g_path2, o, 1, nm, 1);
      t = new_store(); int r = omega_vcstore_load(t, g_path2);
      check("load-name-unknown-id-refused", r == OMEGA_VCS_UNVERIFIED_DEPENDENCY); free_store(t); }
    /* and a valid hand-built file does load (the reference writer and the loader agree) */
    { FObj o[2] = { { ba.p, ba.n, 1 }, { bb.p, bb.n, 2 } };
      write_file(g_path2, o, 2, NULL, 0);
      t = new_store(); int r = omega_vcstore_load(t, g_path2);
      check("load-reference-written-file", r == 0 && omega_vcstore_count(t) == 2); free_store(t); }
    free(bx.p); free(by.p); free(byc.p); free(ba.p); free(bb.p);

    remove(g_path); remove(g_path2);
    free_store(s);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_mutant = argv[1];
        for (size_t i = 0; i < N_MUT; i++)
            if (strcmp(MUTANTS[i].mutant, g_mutant) == 0) g_expect_check = MUTANTS[i].check;
        if (!g_expect_check) { fprintf(stderr, "unknown mutant '%s'\n", g_mutant); return 2; }
    }
    (void)ZERO;
    order_checks();
    golden();
    synthetic_refusals();
    insert_checks();
    growth_checks();
    closure_checks();
    integrity_checks();
    digest_checks();
    name_checks();
    persistence_checks();

    if (!g_mutant) {
        printf("%s: %d check(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
        return g_failed ? 1 : 0;
    }
    if (g_expect_failed) { printf("MUTANT %s KILLED by %s\n", g_mutant, g_expect_check); return 1; }
    printf("MUTANT %s SURVIVED (check %s did not fail)\n", g_mutant, g_expect_check);
    return 3;
}
