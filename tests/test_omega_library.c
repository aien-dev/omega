/* test_omega_library.c -- VC1-LIB: the verified program library as the only admission gate.
 *
 * Usage:
 *   test_omega_library            run every check; exit 0 only if all pass
 *   test_omega_library <mutant>   the binary was linked against a deliberately broken copy of
 *                                 src/omega_library.c (built by `make test-library`); run every
 *                                 check and require the mapped check to FAIL. Exit 1 = mutant
 *                                 killed (what the Makefile asserts), 3 = mutant survived,
 *                                 2 = unknown mutant name.
 *
 * Programs are built the way tests/program/test_program_id.c builds them:
 * omega_program_build_unary_op(), then is_verified = true. Physics-free, host-portable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "omega_core.h"
#include "omega_program.h"
#include "omega_library.h"

/* check -> mutant that must kill it. Each mutant is one deleted/weakened guard line in
 * omega_library.c (see the sed table in the Makefile test-library target). */
static const struct { const char *mutant, *check; } MUTANTS[] = {
    { "allow-null-receipt",    "null-receipt-refused" },
    { "allow-zero-receipt",    "zero-receipt-refused" },
    { "allow-unknown-dep",     "unknown-dep-refused" },
    { "truncate-deps",         "dep-overflow-refused" },
    { "allow-duplicate",       "duplicate-refused" },
    { "allow-unverified",      "unverified-refused" },
    { "digest-ignores-kind",   "bootstrap-vs-verified-digest-differ" },
    { "bootstrap-as-verified", "bootstrap-kind-recorded" },
    { "refuse-valid",          "valid-verified-accepted" },
    { "refuse-deps",           "existing-dep-accepted" },
    { "max-deps-off-by-one",   "max-deps-accepted" },
    { "bootstrap-null-audit",  "bootstrap-null-audit-refused" },
    { "bootstrap-zero-audit",  "bootstrap-zero-audit-refused" },
};
#define N_MUT (sizeof MUTANTS / sizeof MUTANTS[0])

static const char *g_mutant;         /* NULL in normal mode */
static const char *g_expect_check;   /* check that must fail under the mutant */
static int g_failed, g_expect_failed;

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        g_failed++;
        if (g_expect_check && strcmp(name, g_expect_check) == 0) g_expect_failed = 1;
    }
}

static const uint8_t RECEIPT[32] = { 0xA5, 0x5A, 0xC3, 0x3C, 1 };
static const uint8_t ZERO[32];

static OmegaLibrary *new_lib(void) {
    OmegaLibrary *l = calloc(1, sizeof *l);
    if (!l || omega_library_init(l) != 0) { fprintf(stderr, "setup failed\n"); exit(2); }
    return l;
}

static OmegaLibrary *reset(OmegaLibrary *l) {
    omega_library_destroy(l);
    free(l);
    return new_lib();
}

static void mk(OmegaProgram *p, const char *name, OpCode op, uint64_t imm) {
    if (omega_program_build_unary_op(p, name, op, imm) != 0) { fprintf(stderr, "build failed\n"); exit(2); }
    p->is_verified = true;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_mutant = argv[1];
        for (size_t i = 0; i < N_MUT; i++)
            if (strcmp(MUTANTS[i].mutant, g_mutant) == 0) g_expect_check = MUTANTS[i].check;
        if (!g_expect_check) { fprintf(stderr, "unknown mutant '%s'\n", g_mutant); return 2; }
    }

    OmegaLibrary *lib = new_lib();
    OmegaProgram a, b, unv;
    mk(&a, "add7", OP_ADD, 7);
    mk(&b, "add8", OP_ADD, 8);
    mk(&unv, "add9", OP_ADD, 9);
    unv.is_verified = false;

    /* Evidence is mandatory. Each refusal runs on a fresh library so a weakened guard in one
     * path cannot hide behind state left by another (count must stay 0). */
    check("null-receipt-refused", omega_library_insert(lib, &a, NULL, 0, NULL) != 0 && lib->count == 0);
    lib = reset(lib);
    check("zero-receipt-refused", omega_library_insert(lib, &a, NULL, 0, ZERO) != 0 && lib->count == 0);
    lib = reset(lib);
    check("bootstrap-null-audit-refused", omega_library_insert_bootstrap(lib, &a, NULL, 0, NULL) != 0 && lib->count == 0);
    lib = reset(lib);
    check("bootstrap-zero-audit-refused", omega_library_insert_bootstrap(lib, &a, NULL, 0, ZERO) != 0 && lib->count == 0);
    lib = reset(lib);
    check("unverified-refused", omega_library_insert(lib, &unv, NULL, 0, RECEIPT) != 0 && lib->count == 0);
    lib = reset(lib);

    /* valid verified insert */
    int rc = omega_library_insert(lib, &a, NULL, 0, RECEIPT);
    check("valid-verified-accepted", rc == 0 && lib->count == 1 &&
          lib->entries[0].admission_kind == OMEGA_LIB_ADMISSION_VERIFIED &&
          memcmp(lib->entries[0].evidence_receipt_hash, RECEIPT, 32) == 0);

    /* duplicate */
    size_t before = lib->count;
    check("duplicate-refused", omega_library_insert(lib, &a, NULL, 0, RECEIPT) != 0 && lib->count == before);

    /* dependencies must exist */
    SemanticId unknown;
    memset(&unknown, 0xEE, sizeof unknown);
    before = lib->count;
    check("unknown-dep-refused", omega_library_insert(lib, &b, &unknown, 1, RECEIPT) != 0 && lib->count == before);
    SemanticId dep = a.program_id;
    rc = omega_library_insert(lib, &b, &dep, 1, RECEIPT);
    check("existing-dep-accepted", rc == 0 && lib->count == before + 1 && lib->entries[before].dep_count == 1 &&
          memcmp(lib->entries[before].dependency_ids[0].bytes, dep.bytes, OMEGA_ID_BYTES) == 0);

    /* dep_count boundary: MAX accepted, MAX+1 refused (not truncated). OMEGA_LIB_MAX_DEPS distinct
     * base programs + 2 composites; the deps array repeats existing ids, which is legal input. */
    OmegaLibrary *l2 = new_lib();
    SemanticId ids[OMEGA_LIB_MAX_DEPS + 1];
    for (int i = 0; i < OMEGA_LIB_MAX_DEPS; i++) {
        OmegaProgram p;
        mk(&p, "base", OP_ADD, 100 + (uint64_t)i);
        ids[i] = p.program_id;
        if (omega_library_insert(l2, &p, NULL, 0, RECEIPT) != 0) { fprintf(stderr, "setup insert failed\n"); return 2; }
    }
    ids[OMEGA_LIB_MAX_DEPS] = ids[0];
    OmegaProgram top;
    mk(&top, "top", OP_MUL, 3);
    size_t c2 = l2->count;
    check("dep-overflow-refused",
          omega_library_insert(l2, &top, ids, OMEGA_LIB_MAX_DEPS + 1, RECEIPT) != 0 && l2->count == c2);
    rc = omega_library_insert(l2, &top, ids, OMEGA_LIB_MAX_DEPS, RECEIPT);
    check("max-deps-accepted", rc == 0 && l2->count == c2 + 1 && l2->entries[c2].dep_count == OMEGA_LIB_MAX_DEPS);

    /* bootstrap vs verified: same program bytes, same hash bytes, different admission kind */
    OmegaLibrary *lv = new_lib(), *lb = new_lib();
    int rv = omega_library_insert(lv, &a, NULL, 0, RECEIPT);
    int rb = omega_library_insert_bootstrap(lb, &a, NULL, 0, RECEIPT);
    check("bootstrap-kind-recorded", rv == 0 && rb == 0 &&
          lb->entries[0].admission_kind == OMEGA_LIB_ADMISSION_BOOTSTRAP &&
          lv->entries[0].admission_kind == OMEGA_LIB_ADMISSION_VERIFIED);
    check("bootstrap-vs-verified-digest-differ", rv == 0 && rb == 0 &&
          memcmp(lv->state_digest, lb->state_digest, 32) != 0);

    omega_library_destroy(lib); free(lib);
    omega_library_destroy(l2); free(l2);
    omega_library_destroy(lv); free(lv);
    omega_library_destroy(lb); free(lb);

    if (!g_mutant) {
        printf("%s: %d check(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
        return g_failed ? 1 : 0;
    }
    if (g_expect_failed) { printf("MUTANT %s KILLED by %s\n", g_mutant, g_expect_check); return 1; }
    printf("MUTANT %s SURVIVED (check %s did not fail)\n", g_mutant, g_expect_check);
    return 3;
}
