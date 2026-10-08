/* osh_caps.c -- see osh_caps.h. */
#include "osh_caps.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int osh_caps_start(OshCaps *c)
{
    memset(c, 0, sizeof *c);
    int rc = rx_caproot_start(&c->root, &c->admin);
    if (rc == RX_CAP_OK) c->started = 1;
    return rc;
}

static void set_err(char *err, size_t n, const char *fmt, int line, const char *a)
{
    if (err && n) snprintf(err, n, fmt, line, a ? a : "");
}

static int op_right(int op)
{
    switch (op) {
    case OSH_OP_SPAWN: return RX_RIGHT_EFFECT;
    case OSH_OP_OPEN_WRITE: return RX_RIGHT_WRITE;
    default: return RX_RIGHT_READ; /* read, chdir */
    }
}

/* Canonical absolute form of a target: realpath(), or realpath(parent) + "/" + base when the target does not exist yet.
 * A dangling symlink is refused (open(O_CREAT) would create its target somewhere else). 0 ok, -1 refuse. */
static int canon_target(const char *path, char *out, size_t cap)
{
    if (!path || !*path) return -1;
    char rp[PATH_MAX];
    if (realpath(path, rp)) {
        if (strlen(rp) >= cap) return -1;
        strcpy(out, rp);
        return 0;
    }
    if (errno != ENOENT) return -1;
    struct stat st;
    if (lstat(path, &st) == 0) return -1; /* dangling symlink */
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');
    const char *base;
    if (!slash) { strcpy(dir, "."); base = path; }
    else if (slash == path) { strcpy(dir, "/"); base = path + 1; }
    else {
        if ((size_t)(slash - path) >= sizeof dir) return -1;
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = 0;
        base = slash + 1;
    }
    if (!*base || !strcmp(base, ".") || !strcmp(base, "..")) return -1;
    char rd[PATH_MAX];
    if (!realpath(dir, rd)) return -1;
    size_t n = strlen(rd);
    if (n + 1 + strlen(base) + 1 > cap) return -1;
    memcpy(out, rd, n);
    if (n == 1 && rd[0] == '/') n = 0;
    out[n] = '/';
    strcpy(out + n + 1, base);
    return 0;
}

static int under(const char *prefix, const char *p)
{
    size_t n = strlen(prefix);
    if (n == 1 && prefix[0] == '/') return 1;
    return strncmp(prefix, p, n) == 0 && (p[n] == 0 || p[n] == '/');
}

int osh_caps_load(OshCaps *c, const char *text, char *err, size_t errsz)
{
    if (!c->started || c->sealed) { set_err(err, errsz, "line %d: capability root not available%s", 0, ""); return -1; }
    int have_principal = 0, line = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char buf[PATH_MAX + 64];
        line++;
        if (len >= sizeof buf) { set_err(err, errsz, "line %d: too long%s", line, ""); return -1; }
        memcpy(buf, p, len);
        buf[len] = 0;
        p = e ? e + 1 : p + len;
        char *h = strchr(buf, '#');
        if (h) *h = 0;
        char verb[16], kind[16], path[PATH_MAX], junk[2];
        unsigned long pr;
        int n;
        if ((n = sscanf(buf, " %15s", verb)) != 1) continue; /* blank */
        if (!strcmp(verb, "principal")) {
            if (have_principal || sscanf(buf, " principal %lu %1s", &pr, junk) != 1 || pr > 0xFFFFFFFFul) {
                set_err(err, errsz, "line %d: bad or repeated principal%s", line, "");
                return -1;
            }
            c->principal = (uint32_t)pr;
            RxCapMint m = {.issuer = 0, .subject = c->principal, .resource = 0, .rights = RX_RIGHT_READ,
                           .parent = {UINT32_MAX, 0}, .authority = c->admin.office};
            int rc = rx_capadmin_mint(&c->admin, &m, &c->principal_ref);
            if (rc != RX_CAP_OK) { set_err(err, errsz, "line %d: mint refused: %s", line, rx_cap_strerror(rc)); return -1; }
            have_principal = 1;
        } else if (!strcmp(verb, "allow")) {
            if (!have_principal || sscanf(buf, " allow %15s %4095s %1s", kind, path, junk) != 2) {
                set_err(err, errsz, "line %d: expected 'allow spawn|read|write|chdir <path>' after principal%s", line, "");
                return -1;
            }
            int op = !strcmp(kind, "spawn") ? OSH_OP_SPAWN : !strcmp(kind, "read") ? OSH_OP_OPEN_READ
                   : !strcmp(kind, "write") ? OSH_OP_OPEN_WRITE : !strcmp(kind, "chdir") ? OSH_OP_CHDIR : 0;
            char canon[PATH_MAX];
            if (!op || path[0] != '/' || !realpath(path, canon) || c->ngrants >= OSH_CAPS_MAX_GRANTS) {
                set_err(err, errsz, "line %d: bad grant (verb, absolute existing path, or table full): %s", line, path);
                return -1;
            }
            OshGrant *g = &c->grants[c->ngrants];
            RxCapMint m = {.issuer = 0, .subject = c->principal, .resource = (uint64_t)c->ngrants + 1,
                           .rights = (uint32_t)op_right(op), .parent = {UINT32_MAX, 0}, .authority = c->admin.office};
            int rc = rx_capadmin_mint(&c->admin, &m, &g->ref);
            if (rc != RX_CAP_OK) { set_err(err, errsz, "line %d: mint refused: %s", line, rx_cap_strerror(rc)); return -1; }
            g->op = op;
            g->prefix = strdup(canon);
            g->resource = m.resource;
            if (!g->prefix) { set_err(err, errsz, "line %d: out of memory%s", line, ""); return -1; }
            c->ngrants++;
        } else {
            set_err(err, errsz, "line %d: unknown directive: %s", line, verb);
            return -1;
        }
        (void)n;
    }
    if (!have_principal) { set_err(err, errsz, "line %d: no principal line%s", line, ""); return -1; }
    return 0;
}

void osh_caps_attach(OshCaps *c, OshSession *s)
{
    memset(&s->binding, 0, sizeof s->binding);
    s->binding.principal_id[0] = (uint8_t)c->principal;
    s->binding.principal_id[1] = (uint8_t)(c->principal >> 8);
    s->binding.principal_id[2] = (uint8_t)(c->principal >> 16);
    s->binding.principal_id[3] = (uint8_t)(c->principal >> 24);
    s->binding.domain = 2; /* hosted capability authority: 64-bit generation, never narrowed */
    s->binding.cap_index = c->principal_ref.cap_id;
    s->binding.cap_generation = c->principal_ref.generation;
    s->binding.valid = 1;
    s->effect_hook = osh_caps_effect;
    s->effect_ctx = c;
}

static int map_rc(int rc)
{
    switch (rc) {
    case RX_CAP_OK: return OSH_E_OK;
    case RX_CAP_ERR_STALE_GEN: return OSH_E_STALE;
    case RX_CAP_ERR_REVOKED: case RX_CAP_ERR_EPOCH: case RX_CAP_ERR_EXPIRED: case RX_CAP_ERR_CHAIN: return OSH_E_REVOKED;
    default: return OSH_E_DENIED;
    }
}

int osh_caps_effect(void *ctx, const OshBinding *b, int op, const char *path)
{
    OshCaps *c = ctx;
    if (!c || !c->started || !b || !b->valid) return OSH_E_DENIED;
    if (b->domain != 2) return OSH_E_CAP_DOMAIN_MISMATCH; /* no bridge in v1; never narrow or reinterpret */
    for (int i = 4; i < 32; i++)
        if (b->principal_id[i]) return OSH_E_DENIED;
    uint32_t subject = (uint32_t)b->principal_id[0] | (uint32_t)b->principal_id[1] << 8 |
                       (uint32_t)b->principal_id[2] << 16 | (uint32_t)b->principal_id[3] << 24;
    /* 1. the session's own principal capability, with the generation exactly as bound (64 bits) */
    RxCapRef pref = {b->cap_index, b->cap_generation};
    int rc = rx_caproot_validate(&c->root, pref, subject, 0, RX_RIGHT_READ, NULL);
    if (rc != RX_CAP_OK) return map_rc(rc);
    /* 2. a grant for this operation whose prefix contains the canonical target, validated at this moment */
    char canon[PATH_MAX];
    if (canon_target(path, canon, sizeof canon) != 0) return OSH_E_DENIED;
    int result = OSH_E_DENIED;
    for (int i = 0; i < c->ngrants; i++) {
        const OshGrant *g = &c->grants[i];
        if (g->op != op || !under(g->prefix, canon)) continue;
        int e = map_rc(rx_caproot_validate(&c->root, g->ref, subject, g->resource, (uint32_t)op_right(op), NULL));
        if (e == OSH_E_OK) return OSH_E_OK;
        result = e;
    }
    return result;
}

void osh_caps_seal(OshCaps *c)
{
    if (!c->started || c->sealed) return;
    memset(c->admin.token, 0, sizeof c->admin.token);
    if (c->admin.ctl_fd >= 0) close(c->admin.ctl_fd); /* the root sees EOF and exits */
    c->admin.ctl_fd = -1;
    c->admin.running = false;
    if (c->admin.root_pid > 0) waitpid(c->admin.root_pid, NULL, 0);
    c->admin.root_pid = 0;
    c->sealed = 1;
}

void osh_caps_stop(OshCaps *c)
{
    if (!c->started) return;
    rx_caproot_stop(&c->root, &c->admin);
    for (int i = 0; i < c->ngrants; i++) free(c->grants[i].prefix);
    c->ngrants = 0;
    c->started = 0;
}
