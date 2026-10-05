/* test_omega_vc_bridge.c: the bridge (src/omega_vc_bridge.c) is not a self-minting escape hatch
 * (VC1 stage 6 fix, from the inspector's hand test on omega PR #224).
 *   1. The bridge runs omega_program_verify itself and recomputes the program id itself. A program
 *      whose real verification fails is refused even when the caller flips is_verified by hand; a
 *      program whose id was forged is refused.
 *   2. A record the bridge mints lists OMEGA_BRIDGE_SELFMINTED_CAPABILITY, so a build-domain import
 *      of it is refused. The dev domain still accepts it.
 * Each guard carries a VC1B tag; the Makefile target test-vc-bridge breaks each tag in a copy and
 * the named check must FAIL. CPU only. Exit 0 only when every check passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_library.h"
#include "omega_program.h"
#include "omega_resolve.h"
#include "omega_vc_bridge.h"
#include "omega_vcstore.h"

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
static OmegaProgram *mkprog(uint64_t imm) {
    OmegaProgram *p = calloc(1, sizeof *p);
    if (!p || omega_program_build_unary_op(p, "bridge-test", OP_ADD, imm) != 0) { fprintf(stderr, "setup failed\n"); exit(2); }
    if (!p->is_realized && omega_program_realize(p) != 0) { fprintf(stderr, "realize failed\n"); exit(2); }
    return p;
}
typedef struct { OmegaVcBridge br; OmegaLibrary lib; } Rig;
static Rig *rig_new(void) {
    Rig *r = calloc(1, sizeof *r);
    if (!r || omega_vc_bridge_init(&r->br) != 0) { fprintf(stderr, "setup failed\n"); exit(2); }
    omega_library_init(&r->lib);
    return r;
}
static void rig_free(Rig *r) { omega_library_destroy(&r->lib); omega_vc_bridge_destroy(&r->br); free(r); }

/* resolve one import of the program in the bridge's own store, in domain d */
static int import_one(Rig *r, const OmegaProgram *p, OmegaDomain d, OmegaClosure *cl, OmegaResolveError *e) {
    uint8_t rcpt[32];
    if (omega_vcstore_receipt_of(&r->br.store, p->program_id.bytes, rcpt) != 0) return -99;
    char sh[65], rh[65], text[400];
    hx(p->program_id.bytes, sh); hx(rcpt, rh);
    snprintf(text, sizeof text, "omega.lock v1\nfoo = semantic %s receipt %s\n", sh, rh);
    OmegaLock lock;
    if (omega_lock_parse(text, strlen(text), &lock, e) != 0) return -98;
    OmegaResolver rv;
    memset(&rv, 0, sizeof rv);
    rv.store = &r->br.store; rv.domain = d;
    rv.fetch_receipt = omega_vc_bridge_fetch_receipt; rv.fetch_ctx = &r->br;
    rv.fetch_blob = omega_vc_bridge_fetch_blob; rv.blob_ctx = &r->br;
    const char *names[1] = { "foo" };
    memset(cl, 0, sizeof *cl);
    int rc = omega_resolve_imports(&rv, &lock, names, 1, cl, e);
    omega_lock_free(&lock);
    return rc;
}

int main(void) {
    static const uint8_t ev[32] = { 7 };

    /* the genuine program is admitted */
    {
        Rig *r = rig_new();
        OmegaProgram *p = mkprog(5);
        check("bridge-admits-a-genuine-program", omega_vc_bridge_admit(&r->br, &r->lib, p, NULL, 0, ev) == 0 &&
              r->lib.count == 1 && omega_vcstore_count(&r->br.store) == 1);
        OmegaClosure cl; OmegaResolveError e;
        memset(&e, 0, sizeof e);
        int rc = import_one(r, p, OMEGA_DOMAIN_BUILD, &cl, &e);
        check("refuse-bridge-record-in-build-domain", rc == OMEGA_RES_UNVERIFIED_DEPENDENCY && cl.n == 0 && strstr(e.message, "bridge") != NULL);
        omega_closure_free(&cl);
        memset(&e, 0, sizeof e);
        rc = import_one(r, p, OMEGA_DOMAIN_DEV, &cl, &e);
        check("accept-bridge-record-in-dev-domain", rc == 0 && cl.n == 1);
        omega_closure_free(&cl);
        omega_program_destroy(p); free(p); rig_free(r);
    }
    /* the caller never needs to set is_verified: the bridge verifies for itself */
    {
        Rig *r = rig_new();
        OmegaProgram *p = mkprog(6);
        p->is_verified = false;
        check("bridge-does-not-need-the-callers-verified-flag", omega_vc_bridge_admit(&r->br, &r->lib, p, NULL, 0, ev) == 0);
        omega_program_destroy(p); free(p); rig_free(r);
    }
    /* the inspector's hand test: corrupt the machine code, flip the flags, ask the bridge */
    {
        Rig *r = rig_new();
        OmegaProgram *p = mkprog(5);
        memset(p->realization.code_bytes, 0xFF, p->realization.code_len);
        OmegaProgram *probe = calloc(1, sizeof *probe);
        *probe = *p;
        VerifyReport rep;
        int premise = omega_program_verify(probe, &rep) != 0;   /* real verification fails on this program */
        free(probe);
        p->is_realized = true; p->is_verified = true;           /* the caller's lie */
        int rc = omega_vc_bridge_admit(&r->br, &r->lib, p, NULL, 0, ev);
        check("premise-corrupted-program-fails-real-verification", premise);
        check("refuse-corrupted-program-even-with-flags-set", rc == -1 && r->lib.count == 0 && omega_vcstore_count(&r->br.store) == 0);
        omega_program_destroy(p); free(p); rig_free(r);
    }
    /* a forged program id with the flags set */
    {
        Rig *r = rig_new();
        OmegaProgram *p = mkprog(5);
        p->is_realized = true; p->is_verified = true;
        p->program_id.bytes[0] ^= 1;
        int rc = omega_vc_bridge_admit(&r->br, &r->lib, p, NULL, 0, ev);
        check("refuse-forged-program-id-at-admit", rc == -1 && r->lib.count == 0 && omega_vcstore_count(&r->br.store) == 0);
        omega_program_destroy(p); free(p); rig_free(r);
    }
    printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed ? 1 : 0;
}
