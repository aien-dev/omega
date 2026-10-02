/* test_omega_genesis.c: the REAL Genesis Set VC-GENESIS-1 (src/omega_genesis.h), pinned.
 *
 * Changing the set (adding, removing or editing a member, or the set's name) is an ADR-level
 * change. This test makes it impossible to do by accident: it fails until the pinned count and
 * the audit record docs/osc/VC-GENESIS-1.md name exactly the same members as the table.
 * What a LISTED member does is test_omega_genesis_set.c (a variant build with one member).
 * Exit 0 only when every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_genesis.h"
#include "vc_fixture.h"

#define PINNED_COUNT 0u   /* the audit result; edit only together with docs/osc/VC-GENESIS-1.md */

static int g_total, g_failed;
static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    g_total++;
    if (!ok) g_failed++;
}
static void hx(const uint8_t *d, char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}
static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *b = malloc(cap + 1);
    size_t r;
    while (b && (r = fread(b + len, 1, cap - len, f)) > 0) { len += r; if (len == cap) { cap *= 2; b = realloc(b, cap + 1); } }
    fclose(f);
    if (!b) return NULL;
    b[len] = 0; *n = len;
    return b;
}

int main(void) {
    check("set-name-is-vc-genesis-1", strcmp(OMEGA_GENESIS_SET_NAME, "VC-GENESIS-1") == 0);
    check("member-count-is-the-pinned-audit-result", omega_genesis_count() == PINNED_COUNT && OMEGA_GENESIS_1_COUNT == PINNED_COUNT);
    check("member-access-is-bounded", omega_genesis_member(omega_genesis_count()) == NULL && omega_genesis_member((size_t)-1) == NULL);

    int sorted = 1, nonzero = 1, listed = 1;
    static const uint8_t zero[32] = { 0 };
    for (size_t i = 0; i < omega_genesis_count(); i++) {
        if (memcmp(omega_genesis_member(i), zero, 32) == 0) nonzero = 0;
        if (i && memcmp(omega_genesis_member(i - 1), omega_genesis_member(i), 32) >= 0) sorted = 0;
        if (!omega_genesis_contains(omega_genesis_member(i))) listed = 0;
    }
    check("table-rows-are-strictly-ascending-so-no-duplicates", sorted);
    check("no-member-is-the-zero-id", nonzero);
    check("every-member-is-contained", listed);
    check("the-terminator-row-after-the-last-member-is-zero", memcmp(OMEGA_GENESIS_1[OMEGA_GENESIS_1_COUNT], zero, 32) == 0);
    check("zero-id-and-null-are-never-members", omega_genesis_contains(zero) == 0 && omega_genesis_contains(NULL) == 0);

    int none = 1;
    for (int n = 0; n < 40; n++) if (omega_genesis_contains(vcfx((uint8_t)n)->id)) none = 0;
    uint8_t junk[32]; memset(junk, 0xA5, 32);
    check("no-program-we-know-and-no-junk-id-is-a-member", none && omega_genesis_contains(junk) == 0);

    /* the audit record names exactly the table */
    size_t dn = 0;
    char *doc = slurp("docs/osc/VC-GENESIS-1.md", &dn);
    check("audit-record-exists", doc != NULL);
    if (doc) {
        char want[64];
        snprintf(want, sizeof want, "\nMembers: %zu\n", omega_genesis_count());
        check("audit-record-states-the-same-member-count", strstr(doc, want) != NULL && strstr(doc, "\nMembers: ") == strstr(doc, want));
        int all = 1;
        for (size_t i = 0; i < omega_genesis_count(); i++) { char h[65]; hx(omega_genesis_member(i), h); if (!strstr(doc, h)) all = 0; }
        check("audit-record-names-every-member-by-its-id", all);
        /* no 64-hex id in the record that is not in the table: count them */
        size_t hexruns = 0, run = 0;
        for (size_t i = 0; i <= dn; i++) {
            char c = doc[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) run++;
            else { if (run == 64) hexruns++; run = 0; }
        }
        check("audit-record-lists-no-id-that-is-not-in-the-table", hexruns == omega_genesis_count());
        free(doc);
    }

    /* the header cannot be steered from outside: no environment, no files, no switches */
    size_t hn = 0;
    char *hdr = slurp("src/omega_genesis.h", &hn);
    check("header-is-readable", hdr != NULL);
    if (hdr) {
        static const char *const bad[] = { "getenv", "fopen", "#ifdef", "#ifndef OMEGA_GENESIS_1", "#if ", "#elif", "#include \"", "extern ", "argv", "secure_getenv" };
        int clean = 1;
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) if (strstr(hdr, bad[i])) { printf("note: header contains %s\n", bad[i]); clean = 0; }
        check("header-has-no-environment-file-or-preprocessor-switch", clean);
        free(hdr);
    }

    printf("%d checks, %d failed\n", g_total, g_failed);
    if (g_failed) return 1;
    printf("test-genesis-real: PASS\n");
    return 0;
}
