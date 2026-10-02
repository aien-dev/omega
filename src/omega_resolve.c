/* omega_resolve.c -- see omega_resolve.h.
 * Lines tagged VC1R:<tag> are the ones `make test-resolve` deletes or weakens, one at a time
 * (the mutation proof). */
#include "omega_resolve.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "omega_genesis.h"
#include "omega_program_ir.h"
#include "omega_receipt.h"
#include "omega_vcstore_priv.h"
#include "sha256.h"

/* the shared codes must keep the store's numbers */
#define SAME_CODE(n) ((int)OMEGA_RES_##n == (int)OMEGA_VCS_##n)
_Static_assert(SAME_CODE(UNVERIFIED_DEPENDENCY) && SAME_CODE(MISSING_RECEIPT) && SAME_CODE(RECEIPT_HASH_MISMATCH) &&
               SAME_CODE(DEPENDENCY_NOT_PINNED) && SAME_CODE(DEPENDENCY_CYCLE) && SAME_CODE(STALE_RECEIPT) &&
               SAME_CODE(UNDECLARED_IMPORT) && SAME_CODE(TAINTED_ARTIFACT) && SAME_CODE(UNKNOWN_VERIFIER_PROFILE),
               "resolve codes must match the store reserved codes");

#define MAX_DEPTH 128
#define MAX_FILE (4u * 1024u * 1024u)

const char *omega_resolve_code_name(int code)
{
    switch (code) {
    case OMEGA_RES_OK: return "OK";
    case OMEGA_RES_UNVERIFIED_DEPENDENCY: return "UNVERIFIED_DEPENDENCY";
    case OMEGA_RES_MISSING_RECEIPT: return "MISSING_RECEIPT";
    case OMEGA_RES_RECEIPT_HASH_MISMATCH: return "RECEIPT_HASH_MISMATCH";
    case OMEGA_RES_DEPENDENCY_NOT_PINNED: return "DEPENDENCY_NOT_PINNED";
    case OMEGA_RES_DEPENDENCY_CYCLE: return "DEPENDENCY_CYCLE";
    case OMEGA_RES_STALE_RECEIPT: return "STALE_RECEIPT";
    case OMEGA_RES_UNDECLARED_IMPORT: return "UNDECLARED_IMPORT";
    case OMEGA_RES_TAINTED_ARTIFACT: return "TAINTED_ARTIFACT";
    case OMEGA_RES_UNKNOWN_VERIFIER_PROFILE: return "UNKNOWN_VERIFIER_PROFILE";
    case OMEGA_RES_VERIFIER_TOO_OLD: return "VERIFIER_TOO_OLD";
    case OMEGA_RES_BAD_ARGUMENT: return "BAD_ARGUMENT";
    case OMEGA_RES_NOMEM: return "NOMEM";
    default: return "?";
    }
}
const char *omega_domain_name(OmegaDomain d) { return d == OMEGA_DOMAIN_DEV ? "dev" : d == OMEGA_DOMAIN_BUILD ? "build" : "?"; }

static void hex(const uint8_t *in, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = h[in[i] >> 4]; out[2 * i + 1] = h[in[i] & 15]; }
    out[2 * n] = 0;
}
static int unhex32(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = s[2 * i] <= '9' ? s[2 * i] - '0' : s[2 * i] - 'a' + 10;
        int lo = s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}
static int is_zero32(const uint8_t *p) { static const uint8_t z[32] = { 0 }; return memcmp(p, z, 32) == 0; }

static int fail(OmegaResolveError *e, int code, int store_code, const char *subject, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static int fail(OmegaResolveError *e, int code, int store_code, const char *subject, const char *fmt, ...)
{
    if (e) {
        memset(e, 0, sizeof *e);
        e->code = code;
        e->store_code = store_code;
        if (subject) snprintf(e->subject, sizeof e->subject, "%s", subject);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->message, sizeof e->message, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* ---------------------------------------------------------------- profiles */
static const OmegaVerifierProfile DEFAULT_PROFILES[] = {
    { "host-v1", "1.0.0", "HOST_TEST" },
    { "qemu-v1", "1.0.0", "QEMU" },
    { "production-v1", "1.0.0", "PRODUCTION" },
};
const OmegaVerifierProfile *omega_resolve_default_profiles(size_t *n)
{
    if (n) *n = sizeof DEFAULT_PROFILES / sizeof DEFAULT_PROFILES[0];
    return DEFAULT_PROFILES;
}

#define MAX_VER_PARTS 8
static int parse_version(const char *s, uint32_t out[MAX_VER_PARTS], int *n)
{
    *n = 0;
    if (!*s) return -1;
    for (;;) {
        if (*n >= MAX_VER_PARTS) return -1;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { if (++digits > 9) return -1; v = v * 10 + (uint32_t)(*s - '0'); s++; }
        if (!digits) return -1;
        out[(*n)++] = v;
        if (*s == '.') { s++; continue; }
        if (*s == 0) return 0;
        return -1;
    }
}

static const OmegaVerifierProfile *find_profile(const OmegaVerifierProfile *t, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) if (strcmp(t[i].name, name) == 0) return &t[i];
    return NULL;
}

int omega_resolve_check_profile(const OmegaVerifierProfile *table, size_t n, const char *profile,
                                const char *version, OmegaResolveError *err)
{
    if (!table && n == 0) table = omega_resolve_default_profiles(&n);
    const OmegaVerifierProfile *p = table ? find_profile(table, n, profile) : NULL;
    if (!p) return fail(err, OMEGA_RES_UNKNOWN_VERIFIER_PROFILE, 0, profile, "verifier profile '%s' is not in the profile table", profile); /* VC1R:profile-known */
    uint32_t have[MAX_VER_PARTS], need[MAX_VER_PARTS];
    int nh, nn;
    if (parse_version(version, have, &nh) || parse_version(p->min_version, need, &nn))
        return fail(err, OMEGA_RES_VERIFIER_TOO_OLD, 0, profile, "verifier version '%s' is not a dotted number, so it cannot be shown to meet %s", version, p->min_version);
    int cmp = 0;
    for (int i = 0; i < MAX_VER_PARTS && cmp == 0; i++) {
        uint32_t a = i < nh ? have[i] : 0, b = i < nn ? need[i] : 0;
        cmp = a < b ? -1 : a > b ? 1 : 0;
    }
    if (cmp < 0) /* VC1R:profile-version */
        return fail(err, OMEGA_RES_VERIFIER_TOO_OLD, 0, profile, "verifier version %s is older than the minimum %s for profile %s", version, p->min_version, profile);
    return 0;
}

/* ---------------------------------------------------------------- lock */
void omega_lock_free(OmegaLock *l) { if (l) { free(l->e); l->e = NULL; l->n = 0; } }
const OmegaLockEntry *omega_lock_find(const OmegaLock *l, const char *name)
{
    if (!l) return NULL;
    for (size_t i = 0; i < l->n; i++) if (strcmp(l->e[i].name, name) == 0) return &l->e[i];
    return NULL;
}

static int lock_bad(OmegaResolveError *e, size_t line, const char *why)
{
    char subj[32];
    snprintf(subj, sizeof subj, "omega.lock:%zu", line);
    return fail(e, OMEGA_RES_DEPENDENCY_NOT_PINNED, 0, subj, "malformed omega.lock, line %zu: %s", line, why);
}

int omega_lock_parse(const char *text, size_t len, OmegaLock *out, OmegaResolveError *err)
{
    out->e = NULL; out->n = 0;
    if (!text || len == 0 || len > (1u << 20)) return lock_bad(err, 1, "empty or oversize file");
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\t' || text[i] == '\r' || text[i] == 0) return lock_bad(err, 1, "tab, CR or NUL byte (LF line endings only)"); /* VC1R:lock-bytes */
    if (text[len - 1] != '\n') return lock_bad(err, 1, "the last line must end with a line feed");
    size_t cap = 0, line = 0, pos = 0;
    OmegaLockEntry *ents = NULL;
    size_t n = 0;
    int header = 0;
    while (pos < len) {
        size_t end = pos;
        while (text[end] != '\n') end++;
        const char *ln = text + pos;
        size_t ll = end - pos;
        pos = end + 1;
        line++;
        if (!header) {
            if (ll != 13 || memcmp(ln, "omega.lock v1", 13) != 0) { free(ents); return lock_bad(err, line, "line 1 must be exactly 'omega.lock v1'"); } /* VC1R:lock-header */
            header = 1;
            continue;
        }
        if (ll == 0 || ln[0] == '#') continue;
        /* NAME */
        size_t k = 0;
        if (!((ln[0] >= 'A' && ln[0] <= 'Z') || (ln[0] >= 'a' && ln[0] <= 'z') || ln[0] == '_')) { free(ents); return lock_bad(err, line, "unknown line (expected NAME = semantic HEX64 receipt HEX64)"); } /* VC1R:lock-unknown-line */
        while (k < ll && ((ln[k] >= 'A' && ln[k] <= 'Z') || (ln[k] >= 'a' && ln[k] <= 'z') || (ln[k] >= '0' && ln[k] <= '9') ||
                          ln[k] == '_' || ln[k] == '.' || ln[k] == '-')) k++;
        if (k > 128) { free(ents); return lock_bad(err, line, "name longer than 128 characters"); }
        static const char mid[] = " = semantic ", mid2[] = " receipt ";
        size_t need = k + (sizeof mid - 1) + 64 + (sizeof mid2 - 1) + 64;
        if (ll != need || memcmp(ln + k, mid, sizeof mid - 1) != 0 ||
            memcmp(ln + k + sizeof mid - 1 + 64, mid2, sizeof mid2 - 1) != 0) { free(ents); return lock_bad(err, line, "unknown line (expected NAME = semantic HEX64 receipt HEX64)"); }
        char h1[65], h2[65];
        memcpy(h1, ln + k + sizeof mid - 1, 64); h1[64] = 0;
        memcpy(h2, ln + k + sizeof mid - 1 + 64 + sizeof mid2 - 1, 64); h2[64] = 0;
        OmegaLockEntry en;
        memset(&en, 0, sizeof en);
        memcpy(en.name, ln, k);
        if (unhex32(h1, en.semantic) || unhex32(h2, en.receipt)) { free(ents); return lock_bad(err, line, "digest must be 64 lowercase hex digits"); } /* VC1R:lock-hex */
        if (is_zero32(en.semantic) || is_zero32(en.receipt)) { free(ents); return lock_bad(err, line, "an all-zero digest pins nothing"); }
        if (n > 0 && strcmp(ents[n - 1].name, en.name) >= 0) { /* VC1R:lock-order */
            int dup = strcmp(ents[n - 1].name, en.name) == 0;
            free(ents);
            return lock_bad(err, line, dup ? "duplicate name" : "names must be sorted ascending");
        }
        if (n >= OMEGA_LOCK_MAX_ENTRIES) { free(ents); return lock_bad(err, line, "too many entries"); }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            OmegaLockEntry *t = realloc(ents, cap * sizeof *t);
            if (!t) { free(ents); return fail(err, OMEGA_RES_NOMEM, 0, "omega.lock", "out of memory"); }
            ents = t;
        }
        ents[n++] = en;
    }
    if (!header) return lock_bad(err, 1, "missing header");
    out->e = ents;
    out->n = n;
    return 0;
}

/* ---------------------------------------------------------------- file sources */
static int read_file(const char *path, size_t maxn, uint8_t **bytes, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap);
    if (!b) { fclose(f); return -1; }
    for (;;) {
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (n < cap) break;
        if (cap >= maxn) { free(b); fclose(f); return -1; }
        cap *= 2;
        uint8_t *t = realloc(b, cap);
        if (!t) { free(b); fclose(f); return -1; }
        b = t;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad || n > maxn) { free(b); return -1; }
    *bytes = b;
    *len = n;
    return 0;
}
static int dir_fetch(void *dir, const uint8_t id[32], const char *ext, size_t maxn, uint8_t **bytes, size_t *len)
{
    if (!dir) return -1;
    char hx[65], path[4096];
    hex(id, 32, hx);
    if (snprintf(path, sizeof path, "%s/%s.%s", (const char *)dir, hx, ext) >= (int)sizeof path) return -1;
    return read_file(path, maxn, bytes, len);
}
int omega_receipt_dir_fetch(void *dir, const uint8_t id[32], uint8_t **bytes, size_t *len)
{
    return dir_fetch(dir, id, "json", MAX_FILE, bytes, len);
}
int omega_blob_dir_fetch(void *dir, const uint8_t digest[32], uint8_t **bytes, size_t *len)
{
    return dir_fetch(dir, digest, "blob", 16u << 20, bytes, len);
}

/* ---------------------------------------------------------------- receipt check (SPEC 5.1) */
static int cmp32(const void *a, const void *b) { return memcmp(a, b, 32); }

static int has_artifact(const OmegaReceipt *rc, const char *want)
{
    for (size_t i = 0; i < rc->n_input; i++) if (strcasecmp(rc->input_artifacts[i], want) == 0) return 1;
    return 0;
}

int omega_resolve_check_record_receipt(const OmegaResolver *r, const OmegaVcView *vc, OmegaResolveError *err)
{
    char sid[65], rid[65];
    hex(vc->semantic_id, 32, sid);
    hex(vc->receipt_id, 32, rid);
    char prof[OMEGA_VC_MAX_STRING + 1], ver[OMEGA_VC_MAX_STRING + 1];
    memcpy(prof, vc->verifier_profile.p, vc->verifier_profile.len); prof[vc->verifier_profile.len] = 0;
    memcpy(ver, vc->verifier_version.p, vc->verifier_version.len); ver[vc->verifier_version.len] = 0;
    /* 5: profile */
    int rc = omega_resolve_check_profile(r->profiles, r->n_profiles, prof, ver, err);
    if (rc && err) snprintf(err->subject, sizeof err->subject, "%s", sid);
    if (rc) return rc;
    size_t nprof = r->n_profiles;
    const OmegaVerifierProfile *tab = r->profiles ? r->profiles : omega_resolve_default_profiles(&nprof);
    const OmegaVerifierProfile *pf = find_profile(tab, nprof, prof);
    if (!pf) return fail(err, OMEGA_RES_UNKNOWN_VERIFIER_PROFILE, 0, sid, "verifier profile '%s' is not in the profile table", prof);
    int need_rank = pf ? omega_tier_rank(pf->min_tier) : -1;
    /* 6: the receipt itself */
    uint8_t *bytes = NULL;
    size_t blen = 0;
    int fr = r->fetch_receipt ? r->fetch_receipt(r->fetch_ctx, vc->receipt_id, &bytes, &blen) : 1;
    if (fr != 0) return fail(err, OMEGA_RES_MISSING_RECEIPT, 0, sid, "receipt %s is %s", rid, fr == 1 ? "not in the receipt store" : "unreadable"); /* VC1R:missing-receipt */
    OmegaReceipt rcpt;
    uint8_t got_id[32];
    char why[200];
    if (omega_receipt_verify_bytes(bytes, blen, &rcpt, got_id, why, sizeof why)) {
        free(bytes);
        return fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "receipt %s: %s", rid, why);
    }
    free(bytes);
    rc = 0;
    char want[80];
    uint8_t (*deps)[32] = NULL, (*rdeps)[32] = NULL;
    size_t nd = 0, nrd = 0;
    if (memcmp(got_id, vc->receipt_id, 32) != 0) { /* VC1R:rule1-name */
        rc = fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "the file stored under receipt %s holds another receipt", rid);
        goto done;
    }
    /* A digest authenticates receipt bytes, not a clean source checkout.
     * DEV may inspect this evidence, but its output remains tainted. */
    if (r->domain == OMEGA_DOMAIN_BUILD && rcpt.dirty) { /* VC1R:rule-clean */
        rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt %s was produced from a dirty checkout", rid);
        goto done;
    }
    /* rule 4: the receipt names this program (semantic id), then this source (digest) */
    snprintf(want, sizeof want, "sha256:%s", sid);
    if (!has_artifact(&rcpt, want)) { /* VC1R:rule4-semantic */
        rc = fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "receipt %s does not name program %s", rid, sid);
        goto done;
    }
    {
        char dh[65];
        hex(vc->source_or_ir_digest, 32, dh);
        snprintf(want, sizeof want, "sha256:%s", dh);
        if (!has_artifact(&rcpt, want)) { /* VC1R:rule4-source */
            rc = fail(err, OMEGA_RES_STALE_RECEIPT, 0, sid, "receipt %s names the program but not its current source digest %s: it covers an older source", rid, dh);
            goto done;
        }
    }
    /* rule 3: output digest is the evidence root */
    if (memcmp(rcpt.output_digest, vc->evidence_root, 32) != 0) { /* VC1R:rule3 */
        rc = fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "receipt %s output_digest is not the record's evidence_root", rid);
        goto done;
    }
    /* rule 5: receipt kind is the verifier profile */
    if (strcmp(rcpt.kind, prof) != 0) { /* VC1R:rule5 */
        rc = fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "receipt %s kind '%s' is not the verifier profile '%s'", rid, rcpt.kind, prof);
        goto done;
    }
    /* rule 6: receipt dependencies == the receipt ids of the record's dependencies (as sets) */
    nd = vc->n_dependencies;
    deps = malloc((nd ? nd : 1) * 32);
    rdeps = malloc((rcpt.n_dependencies ? rcpt.n_dependencies : 1) * 32);
    if (!deps || !rdeps) { rc = fail(err, OMEGA_RES_NOMEM, 0, sid, "out of memory"); goto done; }
    for (size_t i = 0; i < nd; i++) {
        if (omega_vcstore_receipt_of(r->store, vc->dependencies + 64 * i, deps[i]) != 0) {
            rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "a dependency of %s is not in the store", sid);
            goto done;
        }
    }
    for (size_t i = 0; i < rcpt.n_dependencies; i++) {
        char *s = rcpt.dependencies[i];
        char low[65];
        size_t k = 0;
        for (; s[k] && k < 64; k++) low[k] = (char)((s[k] >= 'A' && s[k] <= 'F') ? s[k] + 32 : s[k]);
        low[k] = 0;
        if (unhex32(low, rdeps[i])) { rc = fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "receipt %s has a malformed dependency", rid); goto done; }
    }
    qsort(deps, nd, 32, cmp32);
    qsort(rdeps, rcpt.n_dependencies, 32, cmp32);
    {
        size_t a = 0, b = 0;
        for (size_t i = 0; i < nd; i++) if (a == 0 || memcmp(deps[a - 1], deps[i], 32) != 0) memcpy(deps[a++], deps[i], 32);
        for (size_t i = 0; i < rcpt.n_dependencies; i++) if (b == 0 || memcmp(rdeps[b - 1], rdeps[i], 32) != 0) memcpy(rdeps[b++], rdeps[i], 32);
        nrd = b;
        if (a != nrd || (a && memcmp(deps, rdeps, a * 32) != 0)) { /* VC1R:rule6 */
            rc = fail(err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, sid, "receipt %s dependencies are not the receipts of the record's dependencies", rid);
            goto done;
        }
    }
    /* rule 2: a passing receipt of enough tier */
    if (rcpt.result != OMEGA_RECEIPT_PASS) { /* VC1R:rule2-pass */
        rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt %s does not record PASS", rid);
        goto done;
    }
    for (size_t i = 0; i < rcpt.n_assertions; i++)
        if (!rcpt.assertions[i].pass) { /* VC1R:rule2-assert */
            rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt %s says PASS but an assertion failed", rid);
            goto done;
        }
    if (rcpt.tier_rank >= 4 && rcpt.tier_rank <= 6 && !rcpt.has_lease) {
        rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt %s is a hardware receipt with no lease binding", rid);
        goto done;
    }
    if (rcpt.tier_rank == 0) { /* VC1R:rule2-testonly */
        rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt %s is TEST_ONLY_TRUST, which satisfies no profile", rid);
        goto done;
    }
    if (!omega_tier_satisfies(need_rank, rcpt.tier_rank)) { /* VC1R:rule2-tier */
        rc = fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt %s tier %s is below the minimum %s of profile %s", rid, rcpt.tier, pf->min_tier, prof);
        goto done;
    }
done:
    free(deps);
    free(rdeps);
    omega_receipt_free(&rcpt);
    return rc;
}

/* ---------------------------------------------------------------- closure */
void omega_closure_free(OmegaClosure *c) { if (c) { free(c->entries); c->entries = NULL; c->n = 0; memset(c->digest, 0, 32); } }

void omega_closure_digest(const OmegaClosureEntry *e, size_t n, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    static const uint8_t dom[] = "OMEGA.CLOSURE.V1";
    sha256_update(&c, dom, sizeof dom); /* includes the NUL after the tag */
    uint8_t cnt[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    sha256_update(&c, cnt, 4);
    for (size_t i = 0; i < n; i++) {
        sha256_update(&c, e[i].semantic_id, 32);
        sha256_update(&c, e[i].receipt_id, 32); /* VC1R:closure-receipt */
        sha256_update(&c, &e[i].admission_kind, 1);
    }
    sha256_final(&c, out);
}

typedef struct { uint8_t id[32], receipt[32], contract[32]; uint8_t kind; int state; } WNode;
typedef struct {
    const OmegaResolver *r;
    WNode *nodes;
    size_t n, cap;
    OmegaResolveError *err;
} Walk;

static long wfind(const Walk *w, const uint8_t id[32])
{
    for (size_t i = 0; i < w->n; i++) if (memcmp(w->nodes[i].id, id, 32) == 0) return (long)i;
    return -1;
}

static int lists_selfminted_cap(const OmegaVcView *v)
{
    for (uint32_t i = 0; i < v->n_capabilities; i++)
        if (v->capabilities[i].len == sizeof OMEGA_BRIDGE_SELFMINTED_CAPABILITY - 1 &&
            memcmp(v->capabilities[i].p, OMEGA_BRIDGE_SELFMINTED_CAPABILITY, sizeof OMEGA_BRIDGE_SELFMINTED_CAPABILITY - 1) == 0) return 1;
    return 0;
}

static int resolve_node(Walk *w, const uint8_t id[32], const uint8_t *lock_receipt, const char *name, size_t depth)
{
    const OmegaResolver *r = w->r;
    char sid[65];
    hex(id, 32, sid);
    const char *subj = name ? name : sid;
    long at = wfind(w, id);
    if (at >= 0) {
        if (w->nodes[at].state == 1) return fail(w->err, OMEGA_RES_DEPENDENCY_CYCLE, 0, sid, "dependency cycle through %s", sid); /* VC1R:cycle */
        if (lock_receipt && memcmp(lock_receipt, w->nodes[at].receipt, 32) != 0) /* VC1R:visited-lock-receipt */
            return fail(w->err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, subj, "the lock pins another receipt than the record carries for %s", sid);
        return 0;
    }
    if (depth >= MAX_DEPTH) return fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "dependency chain deeper than %d", MAX_DEPTH); /* VC1R:max-depth */
    OmegaVcRecord *rec = malloc(sizeof *rec);
    if (!rec) return fail(w->err, OMEGA_RES_NOMEM, 0, sid, "out of memory");
    int rc = omega_vcstore_get(r->store, id, rec);
    if (rc) {
        int out = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, rc, subj, "%s is not an intact record in the Verified Crumb Store (store says %s)", sid, omega_vcstore_code_name(rc));
        free(rec);
        return out;
    }
    const OmegaVcView *vc = &rec->vc;
    rc = 0;
    if (memcmp(vc->semantic_id, id, 32) != 0) {
        rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "the record fetched for %s names a different semantic id", sid);
        goto out;
    }
    if (!r->fetch_blob && r->domain == OMEGA_DOMAIN_BUILD) { /* VC1R:build-needs-blobs */
        rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "the build domain needs the source or IR store to recompute %s (SPEC 6 step 3), and none was given", sid);
        goto out;
    }
    if (r->fetch_blob) {
        uint8_t *bb = NULL;
        size_t bl = 0;
        uint8_t dg[32];
        int fr = r->fetch_blob(r->blob_ctx, vc->source_or_ir_digest, &bb, &bl);
        if (fr != 0) { rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "the source or IR named by %s is not available to recompute its digest", sid); goto out; }
        sha256_hash(bb, bl, dg);
        if (memcmp(dg, vc->source_or_ir_digest, 32) != 0) { /* VC1R:blob-digest */
            free(bb);
            rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "the stored source or IR of %s does not hash to its source_or_ir_digest", sid);
            goto out;
        }
        if (vc->digest_kind == OMEGA_VC_DIGEST_IR) {
            uint8_t pid[32];
            if (omega_program_ir_recompute_id(bb, bl, pid) != 0 || memcmp(pid, id, 32) != 0) { /* VC1R:program-id */
                free(bb);
                rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "the stored IR of %s does not recompute to its semantic id", sid);
                goto out;
            }
        } else if (r->domain == OMEGA_DOMAIN_BUILD) { /* VC1R:ir-kind */
            free(bb);
            rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "%s was recorded from OSC source, and only an IR digest can be recomputed to its program id in the build domain", sid);
            goto out;
        }
        free(bb);
    }
    if (lock_receipt && memcmp(lock_receipt, vc->receipt_id, 32) != 0) { /* VC1R:lock-receipt */
        rc = fail(w->err, OMEGA_RES_RECEIPT_HASH_MISMATCH, 0, subj, "the lock pins another receipt than the record carries for %s", sid);
        goto out;
    }
    if (rec->admission_kind != OMEGA_VCS_ADMISSION_VERIFIED && rec->admission_kind != OMEGA_VCS_ADMISSION_BOOTSTRAP) {
        rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "%s has an unknown admission kind", sid);
        goto out;
    }
    for (uint32_t i = 0; i < vc->n_capabilities; i++)
        if (vc->capabilities[i].len == sizeof OMEGA_TAINT_CAPABILITY - 1 &&
            memcmp(vc->capabilities[i].p, OMEGA_TAINT_CAPABILITY, sizeof OMEGA_TAINT_CAPABILITY - 1) == 0) { /* VC1R:taint-cap */
            rc = fail(w->err, OMEGA_RES_TAINTED_ARTIFACT, 0, subj, "%s lists the capability %s: an omega-dev output can never satisfy an import", sid, OMEGA_TAINT_CAPABILITY);
            goto out;
        }
    if (r->domain == OMEGA_DOMAIN_BUILD && lists_selfminted_cap(vc)) { rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, subj, "%s was minted by the in-process bridge (capability %s) with no independent qualification run, so it cannot satisfy a build import", sid, OMEGA_BRIDGE_SELFMINTED_CAPABILITY); goto out; } /* VC1B:build-refuses-selfminted */
    if (rec->admission_kind == OMEGA_VCS_ADMISSION_VERIFIED) { /* VC1R:verified-needs-receipt */
        rc = omega_resolve_check_record_receipt(r, vc, w->err);
        if (rc) { if (name && w->err) snprintf(w->err->subject, sizeof w->err->subject, "%s", name); goto out; }
    }
    if (w->n == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 16;
        WNode *t = realloc(w->nodes, nc * sizeof *t);
        if (!t) { rc = fail(w->err, OMEGA_RES_NOMEM, 0, sid, "out of memory"); goto out; }
        w->nodes = t;
        w->cap = nc;
    }
    size_t me = w->n++;
    memcpy(w->nodes[me].id, id, 32);
    memcpy(w->nodes[me].receipt, vc->receipt_id, 32);
    memcpy(w->nodes[me].contract, vc->contract_id, 32);
    w->nodes[me].kind = rec->admission_kind;
    w->nodes[me].state = 1;
    for (uint32_t i = 0; i < vc->n_dependencies; i++) {
        const uint8_t *dep = vc->dependencies + 64 * (size_t)i, *req = dep + 32;
        rc = resolve_node(w, dep, NULL, NULL, depth + 1);
        if (rc) goto out;
        long di = wfind(w, dep);
        if (di < 0) { rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "internal: dependency vanished"); goto out; }
        if (memcmp(w->nodes[di].contract, req, 32) != 0) { /* VC1R:edge-contract */
            char dh[65];
            hex(dep, 32, dh);
            rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "dependency %s of %s has a different contract than required", dh, sid);
            goto out;
        }
        if (w->nodes[me].kind == OMEGA_VCS_ADMISSION_BOOTSTRAP && w->nodes[di].kind != OMEGA_VCS_ADMISSION_BOOTSTRAP) { /* VC1R:boot-dep-verified */
            rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "genesis record %s depends on a non-genesis record: the audited base must be closed over itself", sid);
            goto out;
        }
    }
    /* THE genesis gate: reached only after the whole subtree resolved, and refusing any BOOTSTRAP record whose id the pinned
     * VC-GENESIS-1 does not list. Nothing but list membership can satisfy it (no flag, no variable, nothing from the store). */
    if (rec->admission_kind == OMEGA_VCS_ADMISSION_BOOTSTRAP && !omega_genesis_contains(vc->semantic_id)) { /* VC1R:genesis-gate */
        rc = fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, OMEGA_VCS_GENESIS_NOT_LISTED, subj, "%s is a genesis (bootstrap) record whose id is not in the pinned set %s", sid, OMEGA_GENESIS_SET_NAME);
        goto out;
    }
    w->nodes[me].state = 2;
out:
    free(rec);
    return rc;
}

static int cmp_entry(const void *a, const void *b) { return memcmp(a, b, 32); }

int omega_resolve_imports(const OmegaResolver *r, const OmegaLock *lock, const char *const *names,
                          size_t n_names, OmegaClosure *out, OmegaResolveError *err)
{
    out->entries = NULL; out->n = 0;
    memset(out->digest, 0, 32);
    if (!r || !r->store || (n_names && !names)) return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, "resolver", "bad argument");
    OmegaLock empty = { NULL, 0 };
    if (!lock) lock = &empty;
    for (size_t i = 0; i < n_names; i++)
        for (size_t k = 0; k < i; k++)
            if (strcmp(names[i], names[k]) == 0) return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, names[i], "import '%s' is listed twice", names[i]); /* VC1R:dup-import */
    /* declaration check: every import is pinned, every pin is imported */
    for (size_t i = 0; i < n_names; i++)
        if (!omega_lock_find(lock, names[i])) /* VC1R:not-pinned */
            return fail(err, OMEGA_RES_DEPENDENCY_NOT_PINNED, 0, names[i], "import '%s' has no line in omega.lock", names[i]);
    for (size_t i = 0; i < lock->n; i++) {
        int used = 0;
        for (size_t k = 0; k < n_names; k++) if (strcmp(names[k], lock->e[i].name) == 0) used = 1;
        if (!used) /* VC1R:undeclared */
            return fail(err, OMEGA_RES_UNDECLARED_IMPORT, 0, lock->e[i].name, "omega.lock pins '%s' but the source never imports it", lock->e[i].name);
    }
    Walk w = { r, NULL, 0, 0, err };
    int rc = 0;
    for (size_t i = 0; i < n_names && !rc; i++) {
        const OmegaLockEntry *le = omega_lock_find(lock, names[i]);
        if (!le) { rc = fail(err, OMEGA_RES_BAD_ARGUMENT, 0, names[i], "internal error: the pin for %s vanished after the declaration check", names[i]); break; }
        rc = resolve_node(&w, le->semantic, le->receipt, names[i], 0);
    }
    if (rc) { free(w.nodes); return rc; }
    OmegaClosureEntry *ents = malloc((w.n ? w.n : 1) * sizeof *ents);
    if (!ents) { free(w.nodes); return fail(err, OMEGA_RES_NOMEM, 0, "closure", "out of memory"); }
    for (size_t i = 0; i < w.n; i++) {
        memcpy(ents[i].semantic_id, w.nodes[i].id, 32);
        memcpy(ents[i].receipt_id, w.nodes[i].receipt, 32);
        ents[i].admission_kind = w.nodes[i].kind;
    }
    free(w.nodes);
    _Static_assert(offsetof(OmegaClosureEntry, semantic_id) == 0, "entries sort by their first 32 bytes");
    qsort(ents, w.n, sizeof *ents, cmp_entry); /* VC1R:closure-sort */
    out->entries = ents;
    out->n = w.n;
    omega_closure_digest(ents, w.n, out->digest);
    return 0;
}

/* ---------------------------------------------------------------- build identity and the taint mark */
void omega_build_id(const uint8_t ir_digest[32], OmegaDomain d, const uint8_t closure_digest[32], uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    static const uint8_t dom[] = "OSC1.BUILD.V1";
    sha256_update(&c, dom, sizeof dom);
    sha256_update(&c, ir_digest, 32);
    uint8_t db = (uint8_t)d; /* VC1R:build-domain */
    sha256_update(&c, &db, 1);
    sha256_update(&c, closure_digest, 32); /* VC1R:build-closure */
    sha256_final(&c, out);
}

void omega_artifact_meta_make(OmegaArtifactMeta *m, OmegaDomain d, const uint8_t ir_digest[32], const uint8_t closure_digest[32])
{
    memset(m, 0, sizeof *m);
    m->domain = d;
    m->tainted = (uint8_t)(d == OMEGA_DOMAIN_DEV); /* VC1R:taint-mark */
    memcpy(m->ir_digest, ir_digest, 32);
    memcpy(m->closure_digest, closure_digest, 32);
    omega_build_id(ir_digest, d, closure_digest, m->build_id);
}

int omega_artifact_meta_text(const OmegaArtifactMeta *m, char *buf, size_t cap)
{
    char a[65], b[65], c[65];
    hex(m->ir_digest, 32, a); hex(m->closure_digest, 32, b); hex(m->build_id, 32, c);
    int n = snprintf(buf, cap, "OMEGA-ARTIFACT v1\ndomain %s\ntainted %d\nir_sha256 %s\nclosure_sha256 %s\nbuild_id %s\n",
                     omega_domain_name(m->domain), m->tainted ? 1 : 0, a, b, c);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int omega_artifact_meta_parse(const char *text, size_t len, OmegaArtifactMeta *m, OmegaResolveError *err)
{
    memset(m, 0, sizeof *m);
    if (!text || len == 0 || len > 4096 || text[len - 1] != '\n') return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, "artifact", "malformed artifact header");
    char *t = malloc(len + 1);
    if (!t) return fail(err, OMEGA_RES_NOMEM, 0, "artifact", "out of memory");
    memcpy(t, text, len);
    t[len] = 0;
    static const char *const keys[] = { "OMEGA-ARTIFACT v1", "domain ", "tainted ", "ir_sha256 ", "closure_sha256 ", "build_id " };
    char *line = t, *vals[6] = { 0 };
    int ok = 1;
    for (int i = 0; i < 6 && ok; i++) {
        char *nl = strchr(line, '\n');
        if (!nl) { ok = 0; break; }
        *nl = 0;
        size_t kl = strlen(keys[i]);
        if (strncmp(line, keys[i], kl) != 0) { ok = 0; break; }
        vals[i] = line + kl;
        if (i == 0 && vals[i][0]) ok = 0;
        line = nl + 1;
    }
    if (ok && *line) ok = 0;
    uint8_t build[32];
    int domain = 0, tainted = -1;
    if (ok) {
        if (strcmp(vals[1], "dev") == 0) domain = OMEGA_DOMAIN_DEV; else if (strcmp(vals[1], "build") == 0) domain = OMEGA_DOMAIN_BUILD; else ok = 0;
        if (strcmp(vals[2], "1") == 0) tainted = 1; else if (strcmp(vals[2], "0") == 0) tainted = 0; else ok = 0;
        if (unhex32(vals[3], m->ir_digest) || unhex32(vals[4], m->closure_digest) || unhex32(vals[5], build)) ok = 0;
    }
    free(t);
    if (!ok) return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, "artifact", "malformed artifact header");
    m->domain = (OmegaDomain)domain;
    m->tainted = (uint8_t)tainted;
    if (tainted != (domain == OMEGA_DOMAIN_DEV)) /* VC1R:meta-taint-consistent */
        return fail(err, OMEGA_RES_TAINTED_ARTIFACT, 0, "artifact", "the taint mark does not match the domain: an omega-dev artifact is always tainted");
    omega_build_id(m->ir_digest, m->domain, m->closure_digest, m->build_id);
    if (memcmp(m->build_id, build, 32) != 0) /* VC1R:meta-build-id */
        return fail(err, OMEGA_RES_TAINTED_ARTIFACT, 0, "artifact", "the build id does not match the header: the artifact was edited");
    return 0;
}

/* ---------------------------------------------------------------- the one door into the store */
static int map_store(OmegaResolveError *err, int vcs, const char *subject)
{
    int code = (vcs >= OMEGA_VCS_UNVERIFIED_DEPENDENCY && vcs <= OMEGA_VCS_UNKNOWN_VERIFIER_PROFILE) ? vcs : OMEGA_RES_UNVERIFIED_DEPENDENCY;
    return fail(err, code, vcs, subject, "the store refused the record: %s", omega_vcstore_code_name(vcs));
}

static int has_taint_cap(const OmegaVcView *v)
{
    for (uint32_t i = 0; i < v->n_capabilities; i++)
        if (v->capabilities[i].len == sizeof OMEGA_TAINT_CAPABILITY - 1 &&
            memcmp(v->capabilities[i].p, OMEGA_TAINT_CAPABILITY, sizeof OMEGA_TAINT_CAPABILITY - 1) == 0) return 1;
    return 0;
}

int omega_resolve_admit(const OmegaResolver *r, OmegaVcStore *s, const uint8_t *canonical, size_t len,
                        const uint8_t claimed_vc_id[32], const OmegaArtifactMeta *origin, OmegaResolveError *err)
{
    if (!r || !s || !canonical || !claimed_vc_id) return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, "admit", "bad argument");
    if (origin && (origin->tainted || origin->domain == OMEGA_DOMAIN_DEV)) /* VC1R:admit-origin */
        return fail(err, OMEGA_RES_TAINTED_ARTIFACT, 0, "admit", "an omega-dev artifact is tainted and can never enter the store");
    OmegaVcView *view = malloc(sizeof *view);
    if (!view) return fail(err, OMEGA_RES_NOMEM, 0, "admit", "out of memory");
    int rc = omega_vc_decode(canonical, len, view);
    if (rc) { free(view); return map_store(err, rc, "admit"); }
    char sid[65];
    hex(view->semantic_id, 32, sid);
    if (has_taint_cap(view)) { /* VC1R:admit-cap */
        free(view);
        return fail(err, OMEGA_RES_TAINTED_ARTIFACT, 0, sid, "the record lists %s and can never enter the store", OMEGA_TAINT_CAPABILITY);
    }
    OmegaResolver rr = *r;
    rr.store = s;
    rc = omega_resolve_check_record_receipt(&rr, view, err); /* VC1R:admit-receipt */
    free(view);
    if (rc) return rc;
    rc = omega_vcstore_insert(s, canonical, len, claimed_vc_id);
    if (rc) return map_store(err, rc, sid);
    return 0;
}

int omega_resolve_admit_genesis(OmegaVcStore *s, const uint8_t *canonical, size_t len,
                                const uint8_t claimed_vc_id[32], const OmegaArtifactMeta *origin, OmegaResolveError *err)
{
    if (!s || !canonical || !claimed_vc_id) return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, "genesis", "bad argument");
    if (origin && (origin->tainted || origin->domain == OMEGA_DOMAIN_DEV))
        return fail(err, OMEGA_RES_TAINTED_ARTIFACT, 0, "genesis", "an omega-dev artifact is tainted and can never enter the store");
    OmegaVcView *view = malloc(sizeof *view);
    if (!view) return fail(err, OMEGA_RES_NOMEM, 0, "genesis", "out of memory");
    int rc = omega_vc_decode(canonical, len, view);
    if (rc) { free(view); return map_store(err, rc, "genesis"); }
    char sid[65];
    hex(view->semantic_id, 32, sid);
    if (has_taint_cap(view)) {
        free(view);
        return fail(err, OMEGA_RES_TAINTED_ARTIFACT, 0, sid, "the record lists %s and can never enter the store", OMEGA_TAINT_CAPABILITY);
    }
    for (uint32_t i = 0; i < view->n_dependencies; i++) {
        uint8_t k = 0;
        if (omega_vcstore_admission_kind(s, view->dependencies + 64 * (size_t)i, &k) != 0 || k != OMEGA_VCS_ADMISSION_BOOTSTRAP) { /* VC1R:genesis-dep */
            free(view);
            return fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "a genesis record may depend only on genesis records already in the store");
        }
    }
    if (!omega_genesis_contains(view->semantic_id)) { /* VC1R:admit-genesis-list */
        free(view);
        return fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, OMEGA_VCS_GENESIS_NOT_LISTED, sid, "%s is not a member of the pinned genesis set %s", sid, OMEGA_GENESIS_SET_NAME);
    }
    free(view);
    rc = omega_vcstore_insert_bootstrap(s, canonical, len, claimed_vc_id);
    if (rc) return map_store(err, rc, sid);
    return 0;
}
