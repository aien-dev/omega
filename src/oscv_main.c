/*
 * oscv_main.c -- `oscv`: the VERIFIED OSC driver (VC1 stage 4, ADR 0029).
 *
 *   oscv [--domain dev|build] [--lock FILE] [--store FILE] [--receipts DIR] [--blobs DIR]
 *        [--meta-out FILE] <file.osc>
 *
 * Compiles an OSC unit exactly as oscc does and ALSO resolves its `import NAME;` lines, only
 * through omega.lock -> semantic id -> Verified Crumb Store -> receipt check -> closure. Defaults
 * sit next to the source file: omega.lock, omega.vcstore, receipts/ and (build domain only)
 * blobs/. There is no option that skips a check, and nothing is read from the environment. In
 * the build domain the source/IR store is MANDATORY (SPEC 6 step 3): every imported record's
 * blob must be there, hash to its digest and, for an IR digest, recompute to its semantic id,
 * else the import is refused (UNVERIFIED_DEPENDENCY). The dev domain may leave --blobs out; its
 * output is tainted either way. There is no genesis option: a BOOTSTRAP record satisfies an
 * import only if its id is a member of the pinned set VC-GENESIS-1 compiled into the resolver
 * (docs/osc/VC-GENESIS-1.md). The only way to change what is legal is to change the closure.
 *
 * Output (success): ir_sha256= code_sha256= funcs= domain= tainted= imports= closure_entries=
 * closure_sha256= build_id=   (build_id folds the domain and the closure digest into the identity).
 * Refusal: the oscc "error kind=..." lines, plus resolve_code=<CODE> for an import refusal.
 * Exit 0 ok, 1 refused, 2 usage or I/O.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_resolve.h"
#include "omega_resolve_osc.h"
#include "osc_cg.h"
#include "osc_front.h"
#include "sha256.h"

static void hexs(const uint8_t *d, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

static char *slurp(const char *path, size_t *len, int *missing)
{
    FILE *fp = fopen(path, "rb");
    *missing = 0;
    if (!fp) { *missing = 1; return NULL; }
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    for (;;) {
        if (!b) { fclose(fp); return NULL; }
        n += fread(b + n, 1, cap - n, fp);
        if (n < cap) break;
        if (cap >= (1u << 24)) { fclose(fp); free(b); return NULL; }
        cap *= 2;
        char *t = realloc(b, cap);
        if (!t) free(b);
        b = t;
    }
    fclose(fp);
    *len = n;
    return b;
}

static void join_dir(char *out, size_t cap, const char *file, const char *leaf)
{
    const char *slash = strrchr(file, '/');
    if (slash) snprintf(out, cap, "%.*s/%s", (int)(slash - file), file, leaf);
    else snprintf(out, cap, "%s", leaf);
}

static int refuse_resolve(const OmegaResolveError *e)
{
    printf("error kind=IMPORT_REFUSED line=0 col=0 object=%s origin_line=0 other= transition=%s\n", e->subject, omega_resolve_code_name(e->code));
    printf("message=%s\n", e->message);
    printf("resolve_code=%s\n", omega_resolve_code_name(e->code));
    return 1;
}

int main(int argc, char **argv)
{
    OmegaDomain domain = OMEGA_DOMAIN_BUILD;
    const char *lockp = NULL, *storep = NULL, *recdir = NULL, *blobdir = NULL, *metaout = NULL, *file = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--domain") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "dev") == 0) domain = OMEGA_DOMAIN_DEV;
            else if (strcmp(v, "build") == 0) domain = OMEGA_DOMAIN_BUILD;
            else { fprintf(stderr, "oscv: --domain must be dev or build\n"); return 2; }
        } else if (strcmp(a, "--lock") == 0 && i + 1 < argc) lockp = argv[++i];
        else if (strcmp(a, "--store") == 0 && i + 1 < argc) storep = argv[++i];
        else if (strcmp(a, "--receipts") == 0 && i + 1 < argc) recdir = argv[++i];
        else if (strcmp(a, "--blobs") == 0 && i + 1 < argc) blobdir = argv[++i];
        else if (strcmp(a, "--meta-out") == 0 && i + 1 < argc) metaout = argv[++i];
        else if (a[0] == '-' || file) { fprintf(stderr, "oscv: unknown or repeated argument '%s'\n", a); return 2; }
        else file = a;
    }
    if (!file) {
        fprintf(stderr, "usage: oscv [--domain dev|build] [--lock F] [--store F] [--receipts D] [--blobs D] [--meta-out F] <file.osc>\n");
        return 2;
    }
    char lockd[4096], stored[4096], recd[4096], blobd[4096];
    if (!lockp) { join_dir(lockd, sizeof lockd, file, "omega.lock"); lockp = lockd; }
    if (!storep) { join_dir(stored, sizeof stored, file, "omega.vcstore"); storep = stored; }
    if (!recdir) { join_dir(recd, sizeof recd, file, "receipts"); recdir = recd; }
    if (!blobdir && domain == OMEGA_DOMAIN_BUILD) { join_dir(blobd, sizeof blobd, file, "blobs"); blobdir = blobd; }

    size_t slen = 0;
    int missing;
    char *src = slurp(file, &slen, &missing);
    if (!src) { fprintf(stderr, "oscv: cannot read %s\n", file); return 2; }

    OmegaResolveError err;
    OmegaLock lock = { NULL, 0 };
    int have_lock = 0;
    size_t llen = 0;
    char *ltext = slurp(lockp, &llen, &missing);
    if (ltext) {
        if (omega_lock_parse(ltext, llen, &lock, &err)) { free(ltext); free(src); return refuse_resolve(&err); }
        have_lock = 1;
    } else if (!missing) { free(src); fprintf(stderr, "oscv: cannot read %s\n", lockp); return 2; }
    free(ltext);

    OmegaVcStore store;
    omega_vcstore_init(&store);
    FILE *probe = fopen(storep, "rb");
    if (probe) {
        fclose(probe);
        int lrc = omega_vcstore_load(&store, storep);
        if (lrc) {
            memset(&err, 0, sizeof err);
            err.code = OMEGA_RES_UNVERIFIED_DEPENDENCY;
            err.store_code = lrc;
            snprintf(err.subject, sizeof err.subject, "store");
            snprintf(err.message, sizeof err.message, "the Verified Crumb Store file is not intact (store says %s)", omega_vcstore_code_name(lrc));
            omega_vcstore_destroy(&store); omega_lock_free(&lock); free(src);
            return refuse_resolve(&err);
        }
    }

    OmegaOscImports h;
    memset(&h, 0, sizeof h);
    h.resolver.store = &store;
    h.resolver.domain = domain;
    h.resolver.fetch_receipt = omega_receipt_dir_fetch;
    h.resolver.fetch_ctx = (void *)recdir;
    if (blobdir) { h.resolver.fetch_blob = omega_blob_dir_fetch; h.resolver.blob_ctx = (void *)blobdir; }
    /* a BOOTSTRAP record is accepted only if the resolver's pinned VC-GENESIS-1 lists it: no option, file or variable turns that on */
    h.lock = have_lock ? &lock : NULL;

    OscUnit *u = malloc(sizeof *u);
    OscDiag d;
    int rc = 1;
    if (!u) { fprintf(stderr, "oscv: out of memory\n"); rc = 2; goto out; }
    if (osc_compile_imports(src, slen, u, &d, NULL, omega_osc_import_hook, &h)) {
        printf("error kind=%s line=%u col=%u object=%s origin_line=%u other=%s transition=%s\n",
               osc_diag_kind_name(d.kind), d.line, d.col, d.object, d.origin_line, d.other, d.transition);
        printf("message=%s\n", d.message);
        if (d.kind == OSC_DIAG_IMPORT_REFUSED) printf("resolve_code=%s\n", d.transition);
        goto out;
    }
    {
        uint8_t dig[32], codedig[32];
        char hx[65], err2[160];
        OscCode code;
        memset(&code, 0, sizeof code);
        if (osc_ir_digest(u, dig) || osc_cg_compile(u, &code, err2, sizeof err2)) {
            printf("error kind=backend message=%s\n", err2);
            goto out;
        }
        sha256_hash(code.code, code.len, codedig);
        OmegaArtifactMeta meta;
        omega_artifact_meta_make(&meta, domain, dig, h.closure.digest);
        hexs(dig, hx);
        printf("ir_sha256=%s\n", hx);
        hexs(codedig, hx);
        printf("code_sha256=%s\n", hx);
        printf("funcs=%u\n", (unsigned)u->nfuncs);
        printf("domain=%s\n", omega_domain_name(domain));
        printf("tainted=%d\n", meta.tainted);
        printf("imports=%u\n", (unsigned)h.n_imports);
        printf("closure_entries=%zu\n", h.closure.n);
        hexs(h.closure.digest, hx);
        printf("closure_sha256=%s\n", hx);
        hexs(meta.build_id, hx);
        printf("build_id=%s\n", hx);
        osc_cg_free(&code);
        rc = 0;
        if (metaout) {
            char text[512];
            FILE *mf = omega_artifact_meta_text(&meta, text, sizeof text) > 0 ? fopen(metaout, "wb") : NULL;
            if (!mf || fputs(text, mf) < 0 || fclose(mf)) { fprintf(stderr, "oscv: cannot write %s\n", metaout); rc = 2; }
        }
    }
out:
    omega_closure_free(&h.closure);
    omega_vcstore_destroy(&store);
    omega_lock_free(&lock);
    free(u);
    free(src);
    return rc;
}
