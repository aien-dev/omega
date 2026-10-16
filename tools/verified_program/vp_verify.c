/*
 * vp_verify.c -- the independent verifier for verified-program receipts (omega#505,
 * docs/one_layer/VERIFIED_PROGRAM.md section 6).
 *
 *   vp_verify --receipt-id HEX --store DIR --objective-text TEXT --oracle NAME --checker2 PATH
 *             --epoch N [--host-check]
 *
 * The caller (the Pi, or anyone holding the receipt id it was told) states what it asked and which
 * receipt it was given. This program believes nothing the runner wrote down. It re-hashes every
 * artifact in the content-addressed store, recomputes the objective id from the text, recomputes the
 * effects the IR needs with checker2's own decoder (no compiler code is linked), rebuilds the binding
 * object, runs checker2 again on the stored IR and verifies the signed checker2 receipt with the
 * checker2 binary, reads the receipt's requirements and input fields itself, compares the host, and
 * finally recomputes every result with its own reference function (the oracle) for the task.
 *
 * Output: "VERIFIED receipt=<id> result_sha256=<hex> oracle=<name> calls=<n>" and exit 0, or
 * "FAIL <CODE> <detail>" and exit 1 (2 on usage error). The first failing check is reported.
 *
 * Linked: src/sha256.c and checker2's decoder (src/checker2/c2_ir.c). Not linked: src/compiler.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "c2_ir.h"
#include "sha256.h"

/* Paths come from a store directory checked to be at most 1024 bytes (main), so the 4096-byte path
 * buffers cannot truncate; GCC cannot see that bound. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define RCPT_SCHEMA "omega.verified_program.receipt.v1"
#define GRANT_SCHEMA "aien.verified_program.grant.v1"
#define OBJ_TAG "AIEN_E2E_OBJECTIVE_V1"
#define TIER_HOST "host-cpu-aarch64"

static void fail(const char *code, const char *fmt, ...)
{
    va_list ap;
    printf("FAIL %s ", code);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(1);
}

static void hex32(const uint8_t d[32], char out[65])
{
    static const char *x = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = x[d[i] >> 4]; out[2 * i + 1] = x[d[i] & 15]; }
    out[64] = 0;
}

static void sha_hex(const void *b, size_t n, char out[65])
{
    uint8_t d[32];
    sha256_hash(b, n, d);
    hex32(d, out);
}

static int is_hex64(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

static uint8_t *slurp(const char *path, size_t max, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap + 1);
    while (b) {
        if (n == cap) {
            if (cap > max) break;
            uint8_t *nb = realloc(b, cap * 2 + 1);
            if (!nb) break;
            b = nb; cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0) {
            close(fd);
            if (n > max) { free(b); return NULL; }
            b[n] = 0;
            *len = n;
            return b;
        }
        n += (size_t)r;
    }
    free(b);
    close(fd);
    return NULL;
}

/* argv run with stdout+stderr captured in full. Exit code or 128+signal, -1 if not started. */
static int run_all(char *const argv[], char *out, size_t cap)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) { dup2(p[1], 1); dup2(p[1], 2); execv(argv[0], argv); _exit(127); }
    close(p[1]);
    size_t n = 0;
    for (;;) {
        char t[4096];
        ssize_t r = read(p[0], t, sizeof t);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        size_t k = (size_t)r;
        if (n + k > cap - 1) k = cap - 1 - n;
        memcpy(out + n, t, k);
        n += k;
    }
    out[n] = 0;
    close(p[0]);
    int st = 0;
    while (waitpid(c, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* the same, first line only */
static int run_line(char *const argv[], char *out, size_t cap)
{
    int rc = run_all(argv, out, cap);
    out[strcspn(out, "\n")] = 0;
    return rc;
}

/* ---- the receipt ------------------------------------------------------------------------------ */

static const char *const RK[] = {
    "schema", "verdict", "objective_id", "objective_text_sha256", "grant_sha256", "binding_sha256", "program_format",
    "program_sha256", "oplist_sha256", "ir_sha256", "code_sha256", "entry", "params", "ret", "inputs_sha256",
    "outputs_sha256", "calls", "effects_required", "effects_granted", "checker2_verdict", "checker2_id",
    "checker2_receipt_id", "checker2_signer", "omegese_sha256", "omega_emit_sha256", "checker2_bin_sha256",
    "runner_sha256", "commit", "tier", "host_cpu", "machine_id", "gpu_used", "sandbox", "epoch"};
enum {
    R_SCHEMA, R_VERDICT, R_OBJ, R_OBJTXT, R_GRANT, R_BIND, R_FMT, R_PROG, R_OPLIST, R_IR, R_CODE, R_ENTRY, R_PARAMS,
    R_RET, R_IN, R_OUT, R_CALLS, R_EREQ, R_EGRANT, R_C2V, R_C2ID, R_C2R, R_C2SIGNER, R_OMEGESE, R_OME, R_C2BIN,
    R_RUNNER, R_COMMIT, R_TIER, R_HOST, R_MACHINE, R_GPU, R_SANDBOX, R_EPOCH, NR
};

static char *rv[NR];

static void parse_receipt(char *t, size_t n)
{
    if (strncmp(t, "schema=", 7) == 0 && strncmp(t + 7, RCPT_SCHEMA "\n", strlen(RCPT_SCHEMA) + 1))
        fail("UNKNOWN_VERSION", "receipt schema is not %s", RCPT_SCHEMA);
    if (memchr(t, 0, n) || !n || t[n - 1] != '\n') fail("RECEIPT_MALFORMED", "not newline-terminated text");
    char *p = t;
    for (int i = 0; i < NR; i++) {
        char *nl = strchr(p, '\n');
        size_t kl = strlen(RK[i]);
        if (!nl || strncmp(p, RK[i], kl) || p[kl] != '=') fail("RECEIPT_MALFORMED", "line %d: want %s=", i + 1, RK[i]);
        *nl = 0;
        rv[i] = p + kl + 1;
        p = nl + 1;
    }
    if (*p) fail("RECEIPT_MALFORMED", "extra lines");
    if (strcmp(rv[R_VERDICT], "EXECUTED") || strcmp(rv[R_C2V], "ACCEPT")) fail("RECEIPT_MALFORMED", "verdict");
}

/* ---- store ------------------------------------------------------------------------------------ */

static const char *g_store;

static uint8_t *store_get(const char *hex, size_t *len, const char *what)
{
    char path[4096];
    if (!is_hex64(hex)) fail("RECEIPT_MALFORMED", "%s is not a digest", what);
    snprintf(path, sizeof path, "%s/store/%s", g_store, hex);
    uint8_t *b = slurp(path, 64u << 20, len);
    if (!b) fail("STORE_MISSING", "%s %s", what, hex);
    char h[65];
    sha_hex(b, *len, h);
    if (strcmp(h, hex)) fail("STORE_TAMPERED", "%s: the bytes stored under %s hash to %s", what, hex, h);
    return b;
}

/* ---- grant and effects ------------------------------------------------------------------------ */

enum { EFF_PURE = 1, EFF_HEAP = 2, EFF_CALLER = 4 };

static void effects_text(unsigned e, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", e & EFF_PURE ? "pure" : "", e & EFF_HEAP ? ",heap" : "", e & EFF_CALLER ? ",caller_memory" : "");
}

/* What the IR needs, recomputed from checker2's decoding of the stored bytes. */
static unsigned effects_of(const C2Unit *u)
{
    unsigned e = EFF_PURE;
    for (unsigned f = 0; f < u->nfuncs; f++) {
        const C2Func *fn = &u->fn[f];
        for (unsigned v = 0; v < fn->nvregs; v++)
            if (fn->vt[v].s == S_BYTES || fn->vt[v].s == S_CELLS) e |= EFF_CALLER;
        for (uint32_t i = 0; i < fn->ninsns; i++) {
            unsigned op = fn->in[i].op;
            if (op == C2_ALLOC || op == C2_RELEASE || (op >= C2_AOPEN && op <= C2_HSTORE)) e |= EFF_HEAP;
            if (op == C2_SLOAD || op == C2_SSTORE) e |= EFF_CALLER;
        }
    }
    return e;
}

static int parse_u64(const char *s, uint64_t *v)
{
    if (!*s || (*s == '0' && s[1])) return -1;
    uint64_t x = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned d = (unsigned)(*s - '0');
        if (x > (UINT64_MAX - d) / 10) return -1;
        x = x * 10 + d;
    }
    *v = x;
    return 0;
}

/* ---- host ------------------------------------------------------------------------------------- */

static void host_cpu(char *out, size_t n)
{
    struct utsname un;
    char kinds[16][64], imp[16] = "?", var[16] = "?", part[16] = "?", rev[16] = "?", line[256];
    int nk = 0, more = 1;
    if (uname(&un)) snprintf(un.machine, sizeof un.machine, "unknown");
    FILE *f = fopen("/proc/cpuinfo", "re");
    while (f && more) {
        more = fgets(line, sizeof line, f) != NULL;
        char *c = more ? strchr(line, ':') : NULL;
        if (c) {
            char *v = c + 1;
            while (*v == ' ' || *v == '\t') v++;
            v[strcspn(v, "\n")] = 0;
            if (!strncmp(line, "CPU implementer", 15)) snprintf(imp, sizeof imp, "%s", v);
            else if (!strncmp(line, "CPU variant", 11)) snprintf(var, sizeof var, "%s", v);
            else if (!strncmp(line, "CPU part", 8)) snprintf(part, sizeof part, "%s", v);
            else if (!strncmp(line, "CPU revision", 12)) snprintf(rev, sizeof rev, "%s", v);
        }
        if ((!more || line[0] == '\n') && strcmp(part, "?")) {
            char k[64];
            int seen = 0;
            snprintf(k, sizeof k, "%s:%s:%s:%s", imp, var, part, rev);
            for (int i = 0; i < nk; i++) seen |= !strcmp(kinds[i], k);
            if (!seen && nk < 16) snprintf(kinds[nk++], sizeof kinds[0], "%s", k);
            snprintf(part, sizeof part, "?");
        }
    }
    if (f) fclose(f);
    for (int i = 0; i < nk; i++)
        for (int j = i + 1; j < nk; j++)
            if (strcmp(kinds[j], kinds[i]) < 0) { char t[64]; memcpy(t, kinds[i], 64); memcpy(kinds[i], kinds[j], 64); memcpy(kinds[j], t, 64); }
    size_t o = (size_t)snprintf(out, n, "%s/", un.machine);
    for (int i = 0; i < nk && o < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? "," : "", kinds[i]);
    if (!nk && o < n) snprintf(out + o, n - o, "unknown");
}

/* ---- oracles: reference functions written here, from the task statements ----------------------- */

typedef struct {
    const char *name, *params, *ret;
    unsigned np;
    /* 0: expected line written; -1: input outside the oracle's domain */
    int (*f)(const int64_t *a, char *out, size_t n);
} Oracle;

static int o_gcd(const int64_t *a, char *out, size_t n)
{
    uint64_t x = (uint64_t)a[0], y = (uint64_t)a[1];
    while (y) { uint64_t t = x % y; x = y; y = t; }
    snprintf(out, n, "R %" PRIu64, x);
    return 0;
}

static int o_collatz(const int64_t *a, char *out, size_t n)
{
    uint64_t x = (uint64_t)a[0], steps = 0;
    if (x == 0) return -1;
    while (x != 1) {
        if (steps >= 999 || x > (UINT64_MAX - 1) / 3) return -1;
        x = (x & 1) ? 3 * x + 1 : x / 2;
        steps++;
    }
    snprintf(out, n, "R %" PRIu64, steps);
    return 0;
}

/* overflow of the 16-bit signed product is refused: trap code 1 (OSC arithmetic overflow) */
static int o_mul16(const int64_t *a, char *out, size_t n)
{
    int64_t p = a[0] * a[1];
    if (p < -32768 || p > 32767) snprintf(out, n, "T 1");
    else snprintf(out, n, "R %" PRId64, p);
    return 0;
}

static const Oracle ORACLES[] = {
    {"gcd_u32", "u32,u32", "u32", 2, o_gcd},
    {"collatz_steps_u32", "u32", "u32", 1, o_collatz},
    {"checked_mul_i16", "i16,i16", "i16", 2, o_mul16},
};

/* ---- main ------------------------------------------------------------------------------------- */

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv)
{
    const char *rid = opt(argc, argv, "--receipt-id"), *text = opt(argc, argv, "--objective-text"),
               *oname = opt(argc, argv, "--oracle"), *c2 = opt(argc, argv, "--checker2"), *ep = opt(argc, argv, "--epoch");
    int host_check = 0;
    for (int i = 1; i < argc; i++) host_check |= !strcmp(argv[i], "--host-check");
    g_store = opt(argc, argv, "--store");
    uint64_t now = 0;
    if (!rid || !text || !oname || !c2 || !ep || !g_store || parse_u64(ep, &now)) {
        fprintf(stderr, "usage: vp_verify --receipt-id HEX --store DIR --objective-text TEXT --oracle NAME --checker2 PATH --epoch N [--host-check]\n");
        return 2;
    }
    const Oracle *orc = NULL;
    for (size_t i = 0; i < sizeof ORACLES / sizeof ORACLES[0]; i++) if (!strcmp(ORACLES[i].name, oname)) orc = &ORACLES[i];
    if (!orc) { fprintf(stderr, "vp_verify: unknown oracle %s\n", oname); return 2; }
    if (strlen(g_store) > 1024) { fprintf(stderr, "vp_verify: store path longer than 1024 bytes\n"); return 2; }

    /* 1. the receipt the caller was given, by id */
    char path[4096], h[65];
    if (!is_hex64(rid)) fail("RECEIPT_MISSING", "not a receipt id");
    snprintf(path, sizeof path, "%s/receipts/%s.vpr", g_store, rid);
    size_t rn = 0;
    char *rt = (char *)slurp(path, 65536, &rn);
    if (!rt) fail("RECEIPT_MISSING", "no receipt %s", rid);
    sha_hex(rt, rn, h);
    if (strcmp(h, rid)) fail("RECEIPT_ID_MISMATCH", "the receipt stored as %s hashes to %s", rid, h);
    parse_receipt(rt, rn);

    /* 2. the objective the caller asked */
    char *ob = malloc(strlen(text) + 32);
    if (!ob) return 2;
    int obl = sprintf(ob, "%s\n%s", OBJ_TAG, text);
    sha_hex(ob, (size_t)obl, h);
    if (strcmp(h, rv[R_OBJ])) fail("OBJECTIVE_MISMATCH", "receipt is for objective %.16s..., the asked objective is %.16s...", rv[R_OBJ], h);
    sha_hex(text, strlen(text), h);
    if (strcmp(h, rv[R_OBJTXT])) fail("OBJECTIVE_MISMATCH", "objective text digest");

    /* 3. every artifact, re-hashed */
    size_t n_obj, n_grant, n_bind, n_prog, n_ir, n_code, n_in, n_out, n_c2r, n_ol = 0;
    free(store_get(rv[R_OBJTXT], &n_obj, "objective_text"));
    char *grant = (char *)store_get(rv[R_GRANT], &n_grant, "grant");
    char *bind = (char *)store_get(rv[R_BIND], &n_bind, "binding");
    free(store_get(rv[R_PROG], &n_prog, "program"));
    if (strcmp(rv[R_OPLIST], "-")) free(store_get(rv[R_OPLIST], &n_ol, "oplist"));
    uint8_t *ir = store_get(rv[R_IR], &n_ir, "ir");
    free(store_get(rv[R_CODE], &n_code, "code"));
    char *inputs = (char *)store_get(rv[R_IN], &n_in, "inputs");
    char *outputs = (char *)store_get(rv[R_OUT], &n_out, "outputs");
    uint8_t *c2r = store_get(rv[R_C2R], &n_c2r, "checker2_receipt");

    /* 4. the grant: same objective, tier, epoch window, call limit; effects recomputed from the IR */
    static const char *const GK[] = {"schema", "objective_id", "tier", "effects", "epoch_first", "epoch_last", "max_calls"};
    char *gv[7], *p = grant;
    for (int i = 0; i < 7; i++) {
        char *nl = strchr(p, '\n');
        size_t kl = strlen(GK[i]);
        if (!nl || strncmp(p, GK[i], kl) || p[kl] != ' ') fail("GRANT_MALFORMED", "line %d", i + 1);
        *nl = 0;
        gv[i] = p + kl + 1;
        p = nl + 1;
    }
    uint64_t e_first, e_last, max_calls, calls, epoch;
    if (*p || strcmp(gv[0], GRANT_SCHEMA) || parse_u64(gv[4], &e_first) || parse_u64(gv[5], &e_last) ||
        parse_u64(gv[6], &max_calls) || parse_u64(rv[R_CALLS], &calls) || parse_u64(rv[R_EPOCH], &epoch))
        fail("GRANT_MALFORMED", "grant or receipt numbers");
    if (strcmp(gv[1], rv[R_OBJ])) fail("GRANT_OBJECTIVE", "the grant names another objective");
    if (strcmp(gv[2], rv[R_TIER])) fail("GRANT_TIER", "grant tier %s, receipt tier %s", gv[2], rv[R_TIER]);
    if (epoch < e_first || epoch > e_last || now < e_first || now > e_last)
        fail("GRANT_EPOCH", "epoch %" PRIu64 " (now %" PRIu64 ") outside %" PRIu64 "..%" PRIu64, epoch, now, e_first, e_last);
    if (calls == 0 || calls > max_calls) fail("GRANT_LIMIT", "%" PRIu64 " calls, grant allows %" PRIu64, calls, max_calls);
    unsigned granted = 0;
    for (char *s = strtok(gv[3], " "); s; s = strtok(NULL, " ")) {
        if (!strcmp(s, "pure")) granted |= EFF_PURE;
        else if (!strcmp(s, "heap")) granted |= EFF_HEAP;
        else if (!strcmp(s, "caller_memory")) granted |= EFF_CALLER;
        else fail("GRANT_MALFORMED", "effect %s", s);
    }
    C2Unit *u = calloc(1, sizeof *u);
    C2Verdict cv;
    if (!u) return 2;
    if (c2_decode(ir, n_ir, u, &cv)) fail("CHECKER_REJECT", "checker2 decoder: rule=%s %s", cv.rule, cv.detail);
    unsigned need = effects_of(u);
    char need_t[64], grant_t[64];
    effects_text(need, need_t, sizeof need_t);
    effects_text(granted, grant_t, sizeof grant_t);
    if ((need & ~granted) || !(granted & EFF_PURE)) fail("UNAUTHORIZED_EFFECT", "the IR needs %s, the grant allows %s", need_t, grant_t);
    if (strcmp(need_t, rv[R_EREQ]) || strcmp(grant_t, rv[R_EGRANT])) fail("BINDING_MISMATCH", "effects recorded in the receipt");

    /* 5. the binding object (what checker2's receipt signed as requirements) and the signature */
    char want_bind[1024];
    int bl = snprintf(want_bind, sizeof want_bind,
                      "schema=omega.verified_program.binding.v1\nobjective_id=%s\ngrant_sha256=%s\nentry=%s\nparams=%s\n"
                      "ret=%s\ntier=%s\nepoch=%s\n",
                      rv[R_OBJ], rv[R_GRANT], rv[R_ENTRY], rv[R_PARAMS], rv[R_RET], rv[R_TIER], rv[R_EPOCH]);
    if ((size_t)bl != n_bind || memcmp(bind, want_bind, n_bind)) fail("BINDING_MISMATCH", "binding object");
    static const char *const SN[] = {"void", "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    int fi = -1;
    for (unsigned i = 0; i < u->nfuncs; i++) if (!strcmp(u->fn[i].name, rv[R_ENTRY])) fi = (int)i;
    if (fi < 0) fail("BINDING_MISMATCH", "no function %s in the IR", rv[R_ENTRY]);
    const C2Func *fn = &u->fn[fi];
    char sig[128] = "";
    for (unsigned i = 0; i < fn->nparams; i++) {
        unsigned s = fn->vt[i].s;
        size_t l = strlen(sig);
        snprintf(sig + l, sizeof sig - l, "%s%s", i ? "," : "", s >= S_BOOL && s <= S_I64 && fn->vt[i].kind == K_NONE ? SN[s] : "ref");
    }
    if (!fn->nparams) snprintf(sig, sizeof sig, "-");
    if (strcmp(sig, rv[R_PARAMS]) || fn->ret.s < S_BOOL || fn->ret.s > S_I64 || strcmp(SN[fn->ret.s], rv[R_RET]))
        fail("BINDING_MISMATCH", "the IR entry signature is %s -> %s", sig, fn->ret.s <= S_I64 ? SN[fn->ret.s] : "ref");
    c2_unit_free(u);

    /* 6. checker2, again, on the stored bytes; and its identity */
    char out[8192], irp[4096], codep[4096], c2rp[4096], want[128];
    snprintf(irp, sizeof irp, "%s/store/%s", g_store, rv[R_IR]);
    snprintf(codep, sizeof codep, "%s/store/%s", g_store, rv[R_CODE]);
    snprintf(c2rp, sizeof c2rp, "%s/store/%s", g_store, rv[R_C2R]);
    {
        char *av[] = {(char *)c2, "check", irp, NULL};
        int rc = run_line(av, out, sizeof out);
        snprintf(want, sizeof want, "checker2=ACCEPT ir_sha256=%s", rv[R_IR]);
        if (rc || strcmp(out, want)) fail("CHECKER_REJECT", "%s", out);
        char *av2[] = {(char *)c2, "version", NULL};
        rc = run_all(av2, out, sizeof out);
        char *idp = strstr(out, "checker_id=");
        if (rc || !idp || strncmp(idp + 11, rv[R_C2ID], 64)) fail("CHECKER_ID", "this checker2 is not the one named in the receipt");
    }

    /* 7. the signed checker2 receipt: signature, program, executable, target, epoch (checker2), then
     *    the requirements and input fields, read here from the 416 bytes */
    {
        char *av[] = {(char *)c2, "receipt", "verify", c2rp, irp, codep, "--target", rv[R_TIER], "--epoch", (char *)ep,
                      "--allow-test-key", NULL};
        int rc = run_line(av, out, sizeof out);
        if (rc || strcmp(out, "receipt=VALID")) fail("C2_RECEIPT_INVALID", "%s", out);
        if (n_c2r != 416) fail("C2_RECEIPT_INVALID", "size %zu", n_c2r);
        hex32(c2r + 16, h);
        if (strcmp(h, rv[R_BIND])) fail("C2_BINDING", "the signed requirements digest is not this binding");
        hex32(c2r + 272, h);
        if (strcmp(h, rv[R_IN])) fail("C2_INPUT", "the signed input digest is not these inputs");
        uint16_t signer = (uint16_t)(c2r[10] | c2r[11] << 8);
        if ((signer == 1 && strcmp(rv[R_C2SIGNER], "TEST")) || (signer == 2 && strcmp(rv[R_C2SIGNER], "OWNER")) || signer < 1 || signer > 2)
            fail("C2_RECEIPT_INVALID", "signer class");
    }

    /* 8. tier and host */
    if (strcmp(rv[R_TIER], TIER_HOST) || strcmp(rv[R_GPU], "no")) fail("HOST_MISMATCH", "tier %s gpu_used %s", rv[R_TIER], rv[R_GPU]);
    if (host_check) {
        char cpu[512];
        host_cpu(cpu, sizeof cpu);
        if (strcmp(cpu, rv[R_HOST])) fail("HOST_MISMATCH", "receipt host %s, this host %s", rv[R_HOST], cpu);
    }

    /* 9. the results, recomputed */
    if (strcmp(orc->params, rv[R_PARAMS]) || strcmp(orc->ret, rv[R_RET]))
        fail("ORACLE_SIGNATURE", "oracle %s is for (%s) -> %s, the receipt is for (%s) -> %s", orc->name, orc->params, orc->ret,
             rv[R_PARAMS], rv[R_RET]);
    if (!n_out || outputs[n_out - 1] != '\n' || memchr(outputs, 0, n_out)) fail("OUTPUT_MALFORMED", "outputs text");
    char *ip = inputs, *op = outputs;
    uint64_t line = 0;
    while (*ip) {
        char *inl = strchr(ip, '\n'), *onl = strchr(op, '\n');
        if (!inl) fail("OUTPUT_MALFORMED", "inputs text");
        if (!onl) fail("OUTPUT_MALFORMED", "fewer result lines than calls");
        *inl = 0;
        *onl = 0;
        line++;
        int64_t a[6];
        unsigned k = 0;
        for (char *s = strtok(ip, " "); s; s = strtok(NULL, " ")) {
            if (k == orc->np) fail("OUTPUT_MALFORMED", "input line %" PRIu64, line);
            char *end;
            errno = 0;
            a[k++] = strtoll(s, &end, 10);
            if (errno || *end) fail("OUTPUT_MALFORMED", "input line %" PRIu64, line);
        }
        if (k != orc->np) fail("OUTPUT_MALFORMED", "input line %" PRIu64, line);
        char exp[64];
        if (orc->f(a, exp, sizeof exp)) fail("ORACLE_DOMAIN", "call %" PRIu64 " is outside what oracle %s decides", line, orc->name);
        if (strcmp(exp, op)) fail("WRONG_RESULT", "call %" PRIu64 ": got \"%s\", the task's answer is \"%s\"", line, op, exp);
        ip = inl + 1;
        op = onl + 1;
    }
    if (*op) fail("OUTPUT_MALFORMED", "more result lines than calls");
    if (line != calls) fail("OUTPUT_MALFORMED", "%" PRIu64 " calls, receipt says %" PRIu64, line, calls);
    printf("VERIFIED receipt=%s result_sha256=%s oracle=%s calls=%" PRIu64 "\n", rid, rv[R_OUT], orc->name, calls);
    return 0;
}
