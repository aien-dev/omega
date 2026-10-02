/* omega_receipt.c -- see omega_receipt.h.
 * Lines tagged VC1R:<tag> are the ones `make test-resolve` deletes or weakens, one at a time
 * (the mutation proof). */
#include "omega_receipt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_blake3.h"

#define DOMAIN "AIEN_EVIDENCE_RECEIPT_V1"
#define SCHEMA "EvidenceReceiptV1"

/* ---------------------------------------------------------------- small helpers */
static void seterr(char *err, size_t cap, const char *fmt, const char *a)
{
    if (err && cap) snprintf(err, cap, fmt, a ? a : "");
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int is_hex(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (hexval((unsigned char)*s) < 0) return 0;
    return 1;
}
static int is_lower_hex(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}
/* exactly 64 hex digits (any case) to 32 bytes */
static int hex32(const char *s, uint8_t out[32])
{
    if (strlen(s) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = hexval((unsigned char)s[2 * i]), lo = hexval((unsigned char)s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

/* ---------------------------------------------------------------- tiers, verdicts, mutations */
static const struct { const char *name; int rank; } TIERS[] = {
    {"TEST_ONLY_TRUST", 0}, {"HOST_TEST", 1}, {"QEMU", 2}, {"QEMU_SECURITY", 3},
    {"MACHINE1_READ_ONLY", 4}, {"MACHINE1_ATTENDED", 5}, {"MACHINE1_MUTATING", 6}, {"PRODUCTION", 7},
};
int omega_tier_rank(const char *tier)
{
    for (size_t i = 0; i < sizeof TIERS / sizeof TIERS[0]; i++)
        if (strcmp(TIERS[i].name, tier) == 0) return TIERS[i].rank;
    return -1;
}
int omega_tier_satisfies(int need, int have)
{
    if (need < 0 || have < 0) return 0;
    if (have == 0) return need == 0;                  /* test-only trust satisfies only itself */
    if (need >= 4 && have < 4) return 0;              /* physical needs physical (rank >= 4) */
    return have >= need;
}
static const char *const VERDICTS[] = { "PASS", "FAIL", "BLOCKED", "INCOMPLETE", "SKIPPED" };
/* name, wire code, severity */
static const struct { const char *name; uint8_t code, sev; } MUTS[] = {
    {"NONE", 0, 0}, {"VOLATILE_ONLY", 1, 1}, {"REMOVABLE_MEDIA_ONLY", 2, 2},
    {"ONE_TIME_BOOT_SELECTION", 3, 3}, {"BOUNDED_TEST_REGION_WRITE", 4, 4},
    {"BOOT_CONFIGURATION_CHANGE", 5, 5}, {"TRUST_ROOT_CHANGE", 6, 6},
    {"TPM_POLICY_CHANGE", 7, 6}, {"DESTRUCTIVE_STORAGE", 8, 7},
};
#define N_MUTS (sizeof MUTS / sizeof MUTS[0])

/* ---------------------------------------------------------------- JSON DOM */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JK;
typedef struct J {
    JK k;
    int b;
    uint64_t u;
    int is_uint;
    char *s;            /* J_STR, NUL-terminated, no embedded NUL */
    struct J **v;       /* J_ARR items, J_OBJ values */
    char **keys;        /* J_OBJ */
    size_t n;
} J;

typedef struct { const uint8_t *p; size_t n, i; int depth; char err[160]; } JP;

static J *jnew(JK k)
{
    J *j = calloc(1, sizeof *j);
    if (j) j->k = k;
    return j;
}
static void jfree(J *j)
{
    if (!j) return;
    free(j->s);
    for (size_t i = 0; i < j->n; i++) { jfree(j->v ? j->v[i] : NULL); if (j->keys) free(j->keys[i]); }
    free(j->v);
    free(j->keys);
    free(j);
}
static J *jfail(JP *p, const char *what)
{
    if (!p->err[0]) snprintf(p->err, sizeof p->err, "receipt JSON: %s at byte %zu", what, p->i);
    return NULL;
}
static void jws(JP *p)
{
    while (p->i < p->n && (p->p[p->i] == ' ' || p->p[p->i] == '\t' || p->p[p->i] == '\n' || p->p[p->i] == '\r')) p->i++;
}
static int utf8_len(const uint8_t *s, size_t avail)
{
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF && avail >= 2 && (s[1] & 0xC0) == 0x80) return 2;
    if (s[0] >= 0xE0 && s[0] <= 0xEF && avail >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4 && avail >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF) return 4;
    }
    return 0;
}
static int put_utf8(char *o, uint32_t cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
static int hex4(JP *p, uint32_t *out)
{
    if (p->i + 4 > p->n) return -1;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(p->p[p->i + k]);
        if (h < 0) return -1;
        v = v << 4 | (uint32_t)h;
    }
    p->i += 4;
    *out = v;
    return 0;
}
/* p->i is just past the opening quote */
static char *jstring(JP *p)
{
    size_t cap = 16, n = 0;
    char *s = malloc(cap);
    if (!s) { jfail(p, "out of memory"); return NULL; }
    for (;;) {
        if (p->i >= p->n) { free(s); jfail(p, "unterminated string"); return NULL; }
        uint8_t c = p->p[p->i];
        if (n + 8 > cap) { cap *= 2; char *t = realloc(s, cap); if (!t) { free(s); jfail(p, "out of memory"); return NULL; } s = t; }
        if (c == '"') { p->i++; s[n] = 0; return s; }
        if (c < 0x20) { free(s); jfail(p, "control character in string"); return NULL; }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) { free(s); jfail(p, "bad escape"); return NULL; }
            uint8_t e = p->p[p->i++];
            switch (e) {
            case '"': s[n++] = '"'; break;
            case '\\': s[n++] = '\\'; break;
            case '/': s[n++] = '/'; break;
            case 'b': s[n++] = '\b'; break;
            case 'f': s[n++] = '\f'; break;
            case 'n': s[n++] = '\n'; break;
            case 'r': s[n++] = '\r'; break;
            case 't': s[n++] = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (hex4(p, &cp)) { free(s); jfail(p, "bad \\u escape"); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (p->i + 2 > p->n || p->p[p->i] != '\\' || p->p[p->i + 1] != 'u') { free(s); jfail(p, "lone surrogate"); return NULL; }
                    p->i += 2;
                    if (hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF) { free(s); jfail(p, "bad surrogate pair"); return NULL; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) { free(s); jfail(p, "lone surrogate"); return NULL; }
                if (cp == 0) { free(s); jfail(p, "NUL in string"); return NULL; }
                n += (size_t)put_utf8(s + n, cp);
                break;
            }
            default: free(s); jfail(p, "bad escape"); return NULL;
            }
            continue;
        }
        int l = utf8_len(p->p + p->i, p->n - p->i);
        if (!l) { free(s); jfail(p, "invalid UTF-8"); return NULL; }
        memcpy(s + n, p->p + p->i, (size_t)l);
        n += (size_t)l;
        p->i += (size_t)l;
    }
}
static J *jvalue(JP *p);
static J *jcontainer(JP *p, int is_obj)
{
    if (++p->depth > 6) return jfail(p, "nesting too deep");
    J *j = jnew(is_obj ? J_OBJ : J_ARR);
    if (!j) return jfail(p, "out of memory");
    size_t cap = 0;
    jws(p);
    char close = is_obj ? '}' : ']';
    if (p->i < p->n && p->p[p->i] == close) { p->i++; p->depth--; return j; }
    for (;;) {
        jws(p);
        char *key = NULL;
        if (is_obj) {
            if (p->i >= p->n || p->p[p->i] != '"') { jfree(j); return jfail(p, "expected object key"); }
            p->i++;
            key = jstring(p);
            if (!key) { jfree(j); return NULL; }
            for (size_t k = 0; k < j->n; k++)
                if (strcmp(j->keys[k], key) == 0) { free(key); jfree(j); return jfail(p, "duplicate key"); } /* VC1R:json-dup-key */
            jws(p);
            if (p->i >= p->n || p->p[p->i] != ':') { free(key); jfree(j); return jfail(p, "expected ':'"); }
            p->i++;
        }
        J *v = jvalue(p);
        if (!v) { free(key); jfree(j); return NULL; }
        if (j->n == cap) {
            cap = cap ? cap * 2 : 8;
            J **nv = realloc(j->v, cap * sizeof *nv);
            char **nk = is_obj ? realloc(j->keys, cap * sizeof *nk) : NULL;
            if (nv) j->v = nv;
            if (nk) j->keys = nk;
            if (!nv || (is_obj && !nk)) { free(key); jfree(v); jfree(j); return jfail(p, "out of memory"); }
        }
        j->v[j->n] = v;
        if (is_obj) j->keys[j->n] = key;
        j->n++;
        jws(p);
        if (p->i < p->n && p->p[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->p[p->i] == close) { p->i++; break; }
        jfree(j);
        return jfail(p, "expected ',' or closing bracket");
    }
    p->depth--;
    return j;
}
static J *jvalue(JP *p)
{
    jws(p);
    if (p->i >= p->n) return jfail(p, "unexpected end");
    uint8_t c = p->p[p->i];
    if (c == '{') { p->i++; return jcontainer(p, 1); }
    if (c == '[') { p->i++; return jcontainer(p, 0); }
    if (c == '"') {
        p->i++;
        char *s = jstring(p);
        if (!s) return NULL;
        J *j = jnew(J_STR);
        if (!j) { free(s); return jfail(p, "out of memory"); }
        j->s = s;
        return j;
    }
    if (p->n - p->i >= 4 && memcmp(p->p + p->i, "null", 4) == 0) { p->i += 4; return jnew(J_NULL); }
    if (p->n - p->i >= 4 && memcmp(p->p + p->i, "true", 4) == 0) { p->i += 4; J *j = jnew(J_BOOL); if (j) j->b = 1; return j; }
    if (p->n - p->i >= 5 && memcmp(p->p + p->i, "false", 5) == 0) { p->i += 5; return jnew(J_BOOL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        size_t s = p->i;
        if (c == '-') p->i++;
        size_t d0 = p->i;
        while (p->i < p->n && p->p[p->i] >= '0' && p->p[p->i] <= '9') p->i++;
        if (p->i == d0) return jfail(p, "bad number");
        if (p->p[d0] == '0' && p->i - d0 > 1) return jfail(p, "leading zero");
        int plain = (c != '-');
        if (p->i < p->n && (p->p[p->i] == '.' || p->p[p->i] == 'e' || p->p[p->i] == 'E')) {
            plain = 0;
            p->i++;
            while (p->i < p->n && ((p->p[p->i] >= '0' && p->p[p->i] <= '9') || p->p[p->i] == '+' || p->p[p->i] == '-' ||
                                   p->p[p->i] == 'e' || p->p[p->i] == 'E' || p->p[p->i] == '.')) p->i++;
        }
        J *j = jnew(J_NUM);
        if (!j) return jfail(p, "out of memory");
        if (plain) {
            uint64_t v = 0;
            int ok = (p->i - s) <= 20;
            for (size_t k = s; ok && k < p->i; k++) {
                uint64_t dgt = (uint64_t)(p->p[k] - '0');
                if (v > (UINT64_MAX - dgt) / 10) ok = 0; else v = v * 10 + dgt;
            }
            j->is_uint = ok;
            j->u = v;
        }
        return j;
    }
    return jfail(p, "unexpected character");
}

static const J *oget(const J *o, const char *key)
{
    for (size_t i = 0; i < o->n; i++) if (strcmp(o->keys[i], key) == 0) return o->v[i];
    return NULL;
}

/* ---------------------------------------------------------------- receipt extraction */
typedef struct { char *err; size_t cap; } Er;
static int ef(Er *e, const char *fmt, const char *a) { seterr(e->err, e->cap, fmt, a); return -1; }

static int want_str(Er *e, const J *o, const char *key, int required, char **out)
{
    const J *v = oget(o, key);
    if (!v) {
        if (required) return ef(e, "receipt: missing field %s", key);
        *out = strdup("");
        return *out ? 0 : ef(e, "receipt: out of memory%s", "");
    }
    if (v->k != J_STR) return ef(e, "receipt: field %s must be a string", key);
    *out = strdup(v->s);
    return *out ? 0 : ef(e, "receipt: out of memory%s", "");
}
static int want_strlist(Er *e, const J *o, const char *key, int required, char ***out, size_t *n)
{
    const J *v = oget(o, key);
    *out = NULL; *n = 0;
    if (!v) return required ? ef(e, "receipt: missing field %s", key) : 0;
    if (v->k != J_ARR) return ef(e, "receipt: field %s must be a list", key);
    if (v->n > 4096) return ef(e, "receipt: field %s is too long", key);
    if (v->n == 0) return 0;
    char **a = calloc(v->n, sizeof *a);
    if (!a) return ef(e, "receipt: out of memory%s", "");
    *out = a; *n = v->n;
    for (size_t i = 0; i < v->n; i++) {
        if (v->v[i]->k != J_STR) return ef(e, "receipt: list %s must hold strings", key);
        a[i] = strdup(v->v[i]->s);
        if (!a[i]) return ef(e, "receipt: out of memory%s", "");
    }
    return 0;
}
static int want_bool(Er *e, const J *o, const char *key, int *out)
{
    const J *v = oget(o, key);
    if (!v) return ef(e, "receipt: missing field %s", key);
    if (v->k != J_BOOL) return ef(e, "receipt: field %s must be true or false", key);
    *out = v->b;
    return 0;
}
static int want_u64(Er *e, const J *o, const char *key, uint64_t *out)
{
    const J *v = oget(o, key);
    if (!v) return ef(e, "receipt: missing field %s", key);
    if (v->k != J_NUM || !v->is_uint) return ef(e, "receipt: field %s must be an unsigned integer", key);
    *out = v->u;
    return 0;
}
static int want_mut(Er *e, const J *o, const char *key, uint8_t *out)
{
    char *s = NULL;
    if (want_str(e, o, key, 1, &s)) return -1;
    int found = 0;
    for (size_t i = 0; i < N_MUTS; i++) if (strcmp(MUTS[i].name, s) == 0) { *out = (uint8_t)i; found = 1; }
    free(s);
    return found ? 0 : ef(e, "receipt: unknown mutation in %s", key);
}

void omega_receipt_free(OmegaReceipt *r)
{
    if (!r) return;
    char **sv[] = { &r->kind, &r->tier, &r->repo, &r->commit, &r->toolchain, &r->procedure, &r->machine,
                    &r->env_class, &r->authority, &r->lease_resource };
    for (size_t i = 0; i < sizeof sv / sizeof sv[0]; i++) { free(*sv[i]); *sv[i] = NULL; }
    char ***lv[] = { &r->input_artifacts, &r->output_artifacts, &r->dependencies, &r->external_refs, &r->required_features };
    size_t *nv[] = { &r->n_input, &r->n_output, &r->n_dependencies, &r->n_external, &r->n_features };
    for (size_t k = 0; k < 5; k++) {
        if (*lv[k]) for (size_t i = 0; i < *nv[k]; i++) free((*lv[k])[i]);
        free(*lv[k]); *lv[k] = NULL; *nv[k] = 0;
    }
    if (r->assertions) {
        for (size_t i = 0; i < r->n_assertions; i++) {
            free(r->assertions[i].id); free(r->assertions[i].expected); free(r->assertions[i].observed);
            free(r->assertions[i].source); free(r->assertions[i].note);
        }
        free(r->assertions);
    }
    r->assertions = NULL; r->n_assertions = 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int check_digest_list(Er *e, const char *name, char **items, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *it = items[i], *colon = strchr(it, ':');
        if (colon) {
            size_t al = (size_t)(colon - it);
            int ok = (al == 6 && memcmp(it, "blake3", 6) == 0) || (al == 6 && memcmp(it, "sha256", 6) == 0) ||
                     (al == 6 && memcmp(it, "sha512", 6) == 0);
            if (!ok) return ef(e, "receipt: unknown digest algorithm in %s", name);
            const char *hx = colon + 1;
            size_t hl = strlen(hx);
            if (hl < 32 || hl % 2 || !is_hex(hx)) return ef(e, "receipt: bad digest hex in %s", name);
        } else if (!(strlen(it) == 64 && is_hex(it))) {
            return ef(e, "receipt: digest must be <algo>:<hex> or 64 hex digits in %s", name);
        }
    }
    return 0;
}

static int all_known_key(const J *o)
{
    static const char *const keys[] = { "schema", "version", "id", "kind", "tier", "result", "timestamp", "repo", "commit",
        "dirty", "toolchain", "procedure", "machine", "env_class", "input_artifacts", "output_artifacts", "assertions",
        "dependencies", "declared_mutation", "observed_mutation", "authority", "output_digest", "external_refs", "ledger",
        "lease", "required_features", "reserved" };
    for (size_t i = 0; i < o->n; i++) {
        int ok = 0;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) if (strcmp(keys[k], o->keys[i]) == 0) ok = 1;
        if (!ok) return 0;
    }
    return 1;
}

int omega_receipt_parse(const uint8_t *json, size_t len, OmegaReceipt *r, char *err, size_t errcap)
{
    Er e = { err, errcap };
    memset(r, 0, sizeof *r);
    if (!json || len == 0 || len > OMEGA_RECEIPT_MAX_BYTES) return ef(&e, "receipt: empty or oversize input%s", "");
    JP p = { json, len, 0, 0, { 0 } };
    J *root = jvalue(&p);
    if (root) { jws(&p); if (p.i != p.n) { jfree(root); root = NULL; jfail(&p, "trailing bytes after the receipt"); } }
    if (!root) return ef(&e, "%s", p.err);
    int rc = -1;
    char *schema = NULL, *idhex = NULL, *resultstr = NULL, *reserved = NULL, *odigest = NULL;
    if (root->k != J_OBJ) { ef(&e, "receipt: top level must be an object%s", ""); goto done; }
    if (!all_known_key(root)) { ef(&e, "receipt: unknown field%s", ""); goto done; } /* VC1R:deny-unknown */
    uint64_t ver = 0;
    if (want_str(&e, root, "schema", 1, &schema) || want_u64(&e, root, "version", &ver) ||
        want_str(&e, root, "id", 1, &idhex) || want_str(&e, root, "kind", 1, &r->kind) ||
        want_str(&e, root, "tier", 1, &r->tier) || want_str(&e, root, "result", 1, &resultstr) ||
        want_u64(&e, root, "timestamp", &r->timestamp) || want_str(&e, root, "repo", 1, &r->repo) ||
        want_str(&e, root, "commit", 1, &r->commit) || want_bool(&e, root, "dirty", &r->dirty) ||
        want_str(&e, root, "toolchain", 1, &r->toolchain) || want_str(&e, root, "procedure", 1, &r->procedure) ||
        want_str(&e, root, "machine", 1, &r->machine) || want_str(&e, root, "env_class", 1, &r->env_class) ||
        want_strlist(&e, root, "input_artifacts", 1, &r->input_artifacts, &r->n_input) ||
        want_strlist(&e, root, "output_artifacts", 1, &r->output_artifacts, &r->n_output) ||
        want_strlist(&e, root, "dependencies", 1, &r->dependencies, &r->n_dependencies) ||
        want_mut(&e, root, "declared_mutation", &r->declared_mutation) ||
        want_mut(&e, root, "observed_mutation", &r->observed_mutation) ||
        want_str(&e, root, "authority", 0, &r->authority) || want_str(&e, root, "output_digest", 1, &odigest) ||
        want_strlist(&e, root, "external_refs", 0, &r->external_refs, &r->n_external) ||
        want_strlist(&e, root, "required_features", 0, &r->required_features, &r->n_features) ||
        want_str(&e, root, "reserved", 0, &reserved))
        goto done;
    if (ver > 0xFFFFFFFFu) { ef(&e, "receipt: version out of range%s", ""); goto done; }
    r->version = (uint32_t)ver;
    /* ---- the validations of canonical_bytes, in its order ---- */
    if (strcmp(schema, SCHEMA) != 0) { ef(&e, "receipt: unknown receipt schema%s", ""); goto done; } /* VC1R:schema */
    if (r->version != 1) { ef(&e, "receipt: unsupported receipt version%s", ""); goto done; } /* VC1R:version */
    for (size_t i = 0; i < r->n_features; i++)
        if (strcmp(r->required_features[i], "v1-base") != 0) { ef(&e, "receipt: unknown required receipt feature%s", ""); goto done; } /* VC1R:feature */
    if (reserved[0]) { ef(&e, "receipt: reserved field must be empty%s", ""); goto done; } /* VC1R:reserved */
    if (!r->kind[0]) { ef(&e, "receipt: kind must not be empty%s", ""); goto done; }
    if (!r->repo[0]) { ef(&e, "receipt: repo must not be empty%s", ""); goto done; }
    if (!((strlen(r->commit) == 40 || strlen(r->commit) == 64) && is_lower_hex(r->commit))) {
        ef(&e, "receipt: commit must be 40 or 64 lowercase hex digits%s", ""); goto done;
    }
    if (!r->procedure[0]) { ef(&e, "receipt: procedure identity must not be empty%s", ""); goto done; }
    if (!r->machine[0]) { ef(&e, "receipt: machine identity must not be empty%s", ""); goto done; }
    r->tier_rank = omega_tier_rank(r->tier);
    if (r->tier_rank < 0) { ef(&e, "receipt: unknown tier%s", ""); goto done; }
    r->result = 255;
    for (size_t i = 0; i < sizeof VERDICTS / sizeof VERDICTS[0]; i++) if (strcmp(VERDICTS[i], resultstr) == 0) r->result = (uint8_t)i;
    if (r->result == 255) { ef(&e, "receipt: unknown result%s", ""); goto done; }
    if (strcmp(r->env_class, r->tier) != 0) { ef(&e, "receipt: env_class contradicts tier%s", ""); goto done; } /* VC1R:env-class */
    if (check_digest_list(&e, "input_artifacts", r->input_artifacts, r->n_input) ||
        check_digest_list(&e, "output_artifacts", r->output_artifacts, r->n_output)) goto done;
    if (hex32(odigest, r->output_digest)) { ef(&e, "receipt: output_digest must be 64 hex digits%s", ""); goto done; }
    if (hex32(idhex, r->id)) { ef(&e, "receipt: id must be 64 hex digits%s", ""); goto done; }
    /* assertions */
    const J *av = oget(root, "assertions");
    if (!av) { ef(&e, "receipt: missing field %s", "assertions"); goto done; }
    if (av->k != J_ARR || av->n > 4096) { ef(&e, "receipt: assertions must be a list%s", ""); goto done; }
    if (av->n) {
        r->assertions = calloc(av->n, sizeof *r->assertions);
        if (!r->assertions) { ef(&e, "receipt: out of memory%s", ""); goto done; }
        r->n_assertions = av->n;
    }
    for (size_t i = 0; i < av->n; i++) {
        const J *a = av->v[i];
        if (a->k != J_OBJ) { ef(&e, "receipt: an assertion must be an object%s", ""); goto done; }
        for (size_t k = 0; k < a->n; k++) {
            static const char *const ak[] = { "id", "expected", "observed", "pass", "source", "note" };
            int ok = 0;
            for (size_t q = 0; q < 6; q++) if (strcmp(ak[q], a->keys[k]) == 0) ok = 1;
            if (!ok) { ef(&e, "receipt: unknown assertion field%s", ""); goto done; }
        }
        OmegaReceiptAssertion *as = &r->assertions[i];
        if (want_str(&e, a, "id", 1, &as->id) || want_str(&e, a, "expected", 0, &as->expected) ||
            want_str(&e, a, "observed", 0, &as->observed) || want_bool(&e, a, "pass", &as->pass) ||
            want_str(&e, a, "source", 0, &as->source) || want_str(&e, a, "note", 0, &as->note))
            goto done;
    }
    if (r->n_assertions == 0 && r->result == OMEGA_RECEIPT_PASS) { ef(&e, "receipt: PASS receipts must carry at least one assertion%s", ""); goto done; }
    for (size_t i = 0; i < r->n_assertions; i++) {
        if (!r->assertions[i].id[0]) { ef(&e, "receipt: assertion id must not be empty%s", ""); goto done; }
        for (size_t k = 0; k < i; k++)
            if (strcmp(r->assertions[k].id, r->assertions[i].id) == 0) { ef(&e, "receipt: duplicate assertion id%s", ""); goto done; }
    }
    for (size_t i = 0; i < r->n_dependencies; i++) {
        uint8_t tmp[32];
        if (hex32(r->dependencies[i], tmp)) { ef(&e, "receipt: dependency is not a 64 digit receipt hash%s", ""); goto done; }
    }
    if (MUTS[r->observed_mutation].sev > MUTS[r->declared_mutation].sev) { ef(&e, "receipt: observed mutation exceeds declared mutation%s", ""); goto done; } /* VC1R:mutation-order */
    {
        int max = r->tier_rank == 4 ? 2 : r->tier_rank == 5 ? 5 : (r->tier_rank >= 6) ? 255 : 4;
        if (MUTS[r->observed_mutation].sev > max) { ef(&e, "receipt: tier cannot claim that observed mutation%s", ""); goto done; }
        if (r->tier_rank == 4 && r->observed_mutation >= 4) { ef(&e, "receipt: a read-only run cannot claim that mutation%s", ""); goto done; }
    }
    if (r->tier_rank == 7) {
        size_t a = 0, b = strlen(r->authority);
        while (a < b && (r->authority[a] == ' ' || r->authority[a] == '\t' || r->authority[a] == '\n' || r->authority[a] == '\r')) a++;
        if (a == b) { ef(&e, "receipt: PRODUCTION receipts require an authority reference%s", ""); goto done; }
        char *low = strdup(r->authority);
        if (!low) { ef(&e, "receipt: out of memory%s", ""); goto done; }
        for (char *c = low; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        int bad = strstr(low, "test-only") || strstr(low, "test_only") || strstr(low, "testonly");
        free(low);
        if (bad) { ef(&e, "receipt: a TEST_ONLY signer cannot produce a PRODUCTION receipt%s", ""); goto done; }
    }
    /* ledger and lease: null or an object (deny_unknown_fields) */
    const J *lg = oget(root, "ledger");
    if (lg && lg->k != J_NULL) {
        if (lg->k != J_OBJ) { ef(&e, "receipt: ledger must be null or an object%s", ""); goto done; }
        for (size_t k = 0; k < lg->n; k++)
            if (strcmp(lg->keys[k], "index") && strcmp(lg->keys[k], "hash")) { ef(&e, "receipt: unknown ledger field%s", ""); goto done; }
        char *h = NULL;
        if (want_u64(&e, lg, "index", &r->ledger_index) || want_str(&e, lg, "hash", 1, &h)) goto done;
        int bad = hex32(h, r->ledger_hash);
        free(h);
        if (bad) { ef(&e, "receipt: ledger hash must be 64 hex digits%s", ""); goto done; }
        r->has_ledger = 1;
    }
    const J *ls = oget(root, "lease");
    if (ls && ls->k != J_NULL) {
        if (ls->k != J_OBJ) { ef(&e, "receipt: lease must be null or an object%s", ""); goto done; }
        for (size_t k = 0; k < ls->n; k++)
            if (strcmp(ls->keys[k], "hold_id") && strcmp(ls->keys[k], "resource")) { ef(&e, "receipt: unknown lease field%s", ""); goto done; }
        char *h = NULL;
        if (want_str(&e, ls, "hold_id", 1, &h) || want_str(&e, ls, "resource", 1, &r->lease_resource)) { free(h); goto done; }
        int bad = hex32(h, r->lease_hold);
        free(h);
        if (bad) { ef(&e, "receipt: lease hold_id must be 64 hex digits%s", ""); goto done; }
        size_t a = 0, b = strlen(r->lease_resource);
        while (a < b && (r->lease_resource[a] == ' ' || r->lease_resource[a] == '\t' || r->lease_resource[a] == '\n' || r->lease_resource[a] == '\r')) a++;
        if (a == b) { ef(&e, "receipt: lease resource must not be empty%s", ""); goto done; }
        r->has_lease = 1;
    }
    rc = 0;
done:
    free(schema); free(idhex); free(resultstr); free(reserved); free(odigest);
    jfree(root);
    if (rc) omega_receipt_free(r);
    return rc;
}

/* ---------------------------------------------------------------- canonical bytes */
typedef struct { uint8_t *p; size_t n, cap; int oom; } Buf;
static void bput(Buf *b, const void *d, size_t k)
{
    if (b->oom) return;
    if (b->n + k > b->cap) {
        size_t nc = (b->n + k) * 2 + 256;
        uint8_t *t = realloc(b->p, nc);
        if (!t) { b->oom = 1; return; }
        b->p = t; b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, d, k);
    b->n += k;
}
static void bu8(Buf *b, uint8_t v) { bput(b, &v, 1); }
static void bu32(Buf *b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, t, 4); }
static void bu64(Buf *b, uint64_t v) { bu32(b, (uint32_t)(v >> 32)); bu32(b, (uint32_t)v); }
static void bstr(Buf *b, const char *s) { size_t n = strlen(s); bu64(b, n); bput(b, s, n); }

/* sorted, de-duplicated copy of the pointers (BTreeSet<&str>: byte order) */
static char **sorted_unique(char **items, size_t n, size_t *out_n)
{
    char **c = malloc((n ? n : 1) * sizeof *c);
    if (!c) return NULL;
    if (n) memcpy(c, items, n * sizeof *c);
    qsort(c, n, sizeof *c, cmp_str); /* VC1R:canon-sort */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) if (m == 0 || strcmp(c[m - 1], c[i]) != 0) c[m++] = c[i];
    *out_n = m;
    return c;
}
static int cmp_assert(const void *a, const void *b)
{
    return strcmp(((const OmegaReceiptAssertion *)a)->id, ((const OmegaReceiptAssertion *)b)->id);
}

int omega_receipt_canonical(const OmegaReceipt *r, uint8_t **out, size_t *len)
{
    Buf b = { 0 };
    bput(&b, DOMAIN, sizeof DOMAIN - 1);
    bu32(&b, 1);
    bstr(&b, r->kind);
    bstr(&b, r->tier);
    bu8(&b, r->result);
    bstr(&b, r->repo);
    bstr(&b, r->commit);
    bu8(&b, (uint8_t)(r->dirty ? 1 : 0));
    bstr(&b, r->toolchain);
    bstr(&b, r->procedure);
    bstr(&b, r->machine);
    bstr(&b, r->env_class);
    size_t m;
    char **s = sorted_unique(r->input_artifacts, r->n_input, &m);
    if (!s) return -1;
    bu32(&b, (uint32_t)m);
    for (size_t i = 0; i < m; i++) bstr(&b, s[i]);
    free(s);
    s = sorted_unique(r->output_artifacts, r->n_output, &m);
    if (!s) return -1;
    bu32(&b, (uint32_t)m);
    for (size_t i = 0; i < m; i++) bstr(&b, s[i]);
    free(s);
    OmegaReceiptAssertion *as = malloc((r->n_assertions ? r->n_assertions : 1) * sizeof *as);
    if (!as) return -1;
    if (r->n_assertions) memcpy(as, r->assertions, r->n_assertions * sizeof *as);
    qsort(as, r->n_assertions, sizeof *as, cmp_assert);
    bu32(&b, (uint32_t)r->n_assertions);
    for (size_t i = 0; i < r->n_assertions; i++) {
        bstr(&b, as[i].id); bstr(&b, as[i].expected); bstr(&b, as[i].observed);
        bu8(&b, (uint8_t)(as[i].pass ? 1 : 0));
        bstr(&b, as[i].source); bstr(&b, as[i].note);
    }
    free(as);
    s = sorted_unique(r->dependencies, r->n_dependencies, &m);
    if (!s) return -1;
    bu32(&b, (uint32_t)m);
    for (size_t i = 0; i < m; i++) {
        uint8_t raw[32];
        if (hex32(s[i], raw)) { free(s); free(b.p); return -1; }
        bput(&b, raw, 32);
    }
    free(s);
    bu8(&b, MUTS[r->declared_mutation].code);
    bu8(&b, MUTS[r->observed_mutation].code);
    bstr(&b, r->authority);
    bput(&b, r->output_digest, 32);
    s = sorted_unique(r->external_refs, r->n_external, &m);
    if (!s) return -1;
    bu32(&b, (uint32_t)m);
    for (size_t i = 0; i < m; i++) bstr(&b, s[i]);
    free(s);
    if (r->has_ledger) { bu8(&b, 1); bu64(&b, r->ledger_index); bput(&b, r->ledger_hash, 32); }
    else bu8(&b, 0);
    if (r->has_lease) { bu8(&b, 1); bput(&b, r->lease_hold, 32); bstr(&b, r->lease_resource); }
    else bu8(&b, 0);
    bu64(&b, r->timestamp);
    s = sorted_unique(r->required_features, r->n_features, &m);
    if (!s) return -1;
    bu32(&b, (uint32_t)m);
    for (size_t i = 0; i < m; i++) bstr(&b, s[i]);
    free(s);
    bu32(&b, 0);
    if (b.oom) { free(b.p); return -1; }
    *out = b.p;
    *len = b.n;
    return 0;
}

int omega_receipt_compute_id(const OmegaReceipt *r, uint8_t out[32])
{
    uint8_t *c = NULL;
    size_t n = 0;
    if (omega_receipt_canonical(r, &c, &n)) return -1;
    omega_blake3_hash(c, n, out);
    free(c);
    return 0;
}

int omega_receipt_verify_bytes(const uint8_t *json, size_t len, OmegaReceipt *r, uint8_t out_id[32], char *err, size_t errcap)
{
    if (omega_receipt_parse(json, len, r, err, errcap)) return -1;
    if (omega_receipt_compute_id(r, out_id)) { seterr(err, errcap, "receipt: cannot compute canonical identity%s", ""); omega_receipt_free(r); return -1; }
    if (memcmp(out_id, r->id, 32) != 0) { /* VC1R:id-compare */
        seterr(err, errcap, "receipt id mismatch: the file claims one id, its canonical bytes hash to another%s", "");
        omega_receipt_free(r);
        return -1;
    }
    return 0;
}
