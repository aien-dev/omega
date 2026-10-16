/*
 * vp.c -- the verified-program entrypoint (omega#505, docs/one_layer/VERIFIED_PROGRAM.md).
 *
 *   verified_program run <request> <outdir> --omegese P --omega-emit P --checker2 P [--commit SHA]
 *
 * One request (program bytes, declared signature, bounded inputs, objective, grant, pins, tier) goes
 * through the existing chain and nothing else:
 *   omegese emit -> omega_emit emit -> canonical OSC IR -> checker2 check -> checker2 receipt issue
 *   (TEST class, binds IR, code image, objective binding and inputs) -> preflight (IR and code bytes
 *   re-read and re-hashed, checker2 run again on the exact bytes, code image word self-check and an
 *   independent recompile that must give the same bytes) -> execution in a child under seccomp strict
 *   mode -> an execution receipt that names every artifact by its SHA-256 in a content-addressed store.
 *
 * Authority. The grant is an input written by the runtime authority (AIENOS / sovereign-core). This tool
 * only checks that the grant names the same objective, tier and epoch window and covers the effects
 * the checked IR needs; it never mints or widens a grant. checker2 judges the IR; it grants nothing.
 *
 * Exit: 0 EXECUTED (result.txt verdict=EXECUTED), 1 REFUSED (result.txt names one rejection code),
 * 2 usage or I/O error. A refusal writes no receipt. EXECUTED says the program ran under its grant; it
 * does NOT say the result is right: that is the independent verifier's job (vp_verify.c).
 *
 * Canary builds (negative controls only): -DVP_TAMPER=1 changes one IR byte in the store after the
 * receipt is issued and before the preflight; -DVP_TAMPER=2 does the same to the code image. The
 * preflight must refuse both. -DVP_TAMPER=3 makes one forbidden system call inside the sandbox; the
 * kernel must kill the child and the run must be refused (EXEC_FAILED).
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "osc_cg.h"
#include "osc_ir.h"
#include "osc_native.h"
#include "osc_rt.h"
#include "sha256.h"

/* Every path is built from an outdir and request path checked to be at most 1024 bytes (main), so the
 * 4096-byte path buffers cannot truncate; GCC cannot see that bound. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#ifndef VP_TAMPER
#define VP_TAMPER 0
#endif

#define REQ_SCHEMA "omega.verified_program.request.v1"
#define GRANT_SCHEMA "aien.verified_program.grant.v1"
#define TIER_HOST "host-cpu-aarch64"
#define OBJ_TAG "AIEN_E2E_OBJECTIVE_V1"
#define MAX_CALLS 4096u
#define MAX_FILE (1u << 20)

/* ---- small helpers ---------------------------------------------------------------------------- */

static const char *g_out;          /* outdir */
static char g_objective_id[65] = "-";
static char g_ir_sha[65] = "-";

static void hex32(const uint8_t d[32], char out[65])
{
    static const char *x = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = x[d[i] >> 4]; out[2 * i + 1] = x[d[i] & 15]; }
    out[64] = 0;
}

static void sha_hex(const uint8_t *b, size_t n, char out[65])
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

static void die_io(const char *what)
{
    fprintf(stderr, "verified_program: %s: %s\n", what, strerror(errno));
    exit(2);
}

static uint8_t *read_file(const char *path, size_t max, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap + 1);
    if (!b) { close(fd); return NULL; }
    for (;;) {
        if (n == cap) {
            if (cap > max) { free(b); close(fd); errno = EFBIG; return NULL; }
            uint8_t *nb = realloc(b, cap * 2 + 1);
            if (!nb) { free(b); close(fd); return NULL; }
            b = nb; cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; free(b); close(fd); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (n > max) { free(b); errno = EFBIG; return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}

static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

/* Exclusive create; identical bytes already there is a no-op, any other content is an error. */
static int put_file(const char *path, const void *p, size_t n, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) {
        if (errno != EEXIST) return -1;
        size_t ol = 0;
        uint8_t *old = read_file(path, MAX_FILE * 4u, &ol);
        int same = old && ol == n && !memcmp(old, p, n);
        free(old);
        if (same) return 0;
        errno = EEXIST;
        return -1;
    }
    int rc = write_all(fd, p, n);
    if (close(fd) || rc) return -1;
    return 0;
}

/* The content-addressed store: <outdir>/store/<sha256 hex>, read-only files. */
static void store_put(const void *p, size_t n, char hex[65])
{
    char path[4096];
    sha_hex(p, n, hex);
    snprintf(path, sizeof path, "%s/store/%s", g_out, hex);
    if (put_file(path, p, n, 0444)) die_io(path);
}

/* One space-free token for result fields (key=value lines). */
static void token(char *s)
{
    for (; *s; s++) if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 127 || *s == '=') *s = '_';
}

static void write_result(const char *verdict, const char *rejection, const char *stage, const char *detail,
                         const char *outputs, const char *receipt, const char *c2r)
{
    char d[256], path[4096], buf[2048];
    snprintf(d, sizeof d, "%s", detail && *detail ? detail : "-");
    token(d);
    int n = snprintf(buf, sizeof buf,
                     "schema=omega.verified_program.result.v1\nverdict=%s\nrejection=%s\nstage=%s\ndetail=%s\n"
                     "objective_id=%s\nir_sha256=%s\noutputs_sha256=%s\nreceipt_id=%s\nchecker2_receipt_id=%s\n",
                     verdict, rejection, stage, d, g_objective_id, g_ir_sha, outputs, receipt, c2r);
    snprintf(path, sizeof path, "%s/result.txt", g_out);
    if (put_file(path, buf, (size_t)n, 0644)) die_io(path);
}

static void refuse(const char *code, const char *stage, const char *fmt, ...)
{
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    write_result("REFUSED", code, stage, detail, "-", "-", "-");
    printf("REFUSED %s %s: %s\n", code, stage, detail);
    exit(1);
}

/* Run argv; capture stdout and stderr together (at most cap-1 bytes, NUL-terminated). Exit code, or
 * 128+signal; -1 when it could not start. */
static int run_capture(char *const argv[], char *out, size_t cap)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t c = fork();
    if (c < 0) { close(p[0]); close(p[1]); return -1; }
    if (c == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    for (;;) {
        char tmp[4096];
        ssize_t r = read(p[0], tmp, sizeof tmp);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        size_t k = (size_t)r;
        if (n + k > cap - 1) k = cap - 1 - n;
        memcpy(out + n, tmp, k);
        n += k;
    }
    out[n] = 0;
    close(p[0]);
    int st = 0;
    while (waitpid(c, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void first_line(char *s)
{
    char *nl = strchr(s, '\n');
    if (nl) *nl = 0;
}

static int file_sha(const char *path, char hex[65])
{
    size_t n = 0;
    uint8_t *b = read_file(path, 256u << 20, &n);
    if (!b) return -1;
    sha_hex(b, n, hex);
    free(b);
    return 0;
}

/* ---- request and grant ------------------------------------------------------------------------ */

static const char *const REQ_KEYS[] = {"schema", "objective_id", "objective_text", "program_format", "program",
                                       "entry", "params", "ret", "inputs", "tier", "grant", "epoch",
                                       "pin_checker2_id", "pin_omegese_sha256", "pin_omega_emit_sha256"};
#define NREQ (sizeof REQ_KEYS / sizeof REQ_KEYS[0])
static const char *const GRANT_KEYS[] = {"schema", "objective_id", "tier", "effects", "epoch_first", "epoch_last",
                                         "max_calls"};
#define NGRANT (sizeof GRANT_KEYS / sizeof GRANT_KEYS[0])

/* Split text into exactly nk lines "key value" in the fixed key order. vals[i] points into text. A
 * value is everything after the first space (may be empty only where allowed by the caller). Returns
 * 0, or -1 with why filled. The schema line is checked by the caller (unknown version first). */
static int parse_kv(char *text, size_t len, const char *const *keys, size_t nk, char **vals, char *why, size_t wn)
{
    if (memchr(text, 0, len)) { snprintf(why, wn, "NUL byte"); return -1; }
    if (len == 0 || text[len - 1] != '\n') { snprintf(why, wn, "must end with a newline"); return -1; }
    char *p = text;
    for (size_t i = 0; i < nk; i++) {
        char *nl = strchr(p, '\n');
        if (!nl) { snprintf(why, wn, "missing line %s", keys[i]); return -1; }
        *nl = 0;
        if (strchr(p, '\r')) { snprintf(why, wn, "carriage return in line %zu", i + 1); return -1; }
        size_t kl = strlen(keys[i]);
        if (strncmp(p, keys[i], kl) || (p[kl] != ' ' && p[kl] != 0)) {
            snprintf(why, wn, "line %zu: want key %s", i + 1, keys[i]);
            return -1;
        }
        vals[i] = p[kl] ? p + kl + 1 : p + kl;
        p = nl + 1;
    }
    if (*p) { snprintf(why, wn, "extra lines after %s", keys[nk - 1]); return -1; }
    return 0;
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

static int kind_of(const char *s)
{
    static const char *const nm[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    static const int k[] = {OSC_T_BOOL, OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_U64, OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64};
    for (int i = 0; i < 9; i++) if (!strcmp(s, nm[i])) return k[i];
    return -1;
}

enum { EFF_PURE = 1, EFF_HEAP = 2, EFF_CALLER = 4 };

static void effects_text(unsigned e, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", e & EFF_PURE ? "pure" : "", e & EFF_HEAP ? ",heap" : "", e & EFF_CALLER ? ",caller_memory" : "");
}

/* What the checked IR needs, read from every instruction and register type of every function. */
static unsigned effects_of(const OscUnit *u)
{
    unsigned e = EFF_PURE;
    for (unsigned f = 0; f < u->nfuncs; f++) {
        const OscFunc *fn = &u->funcs[f];
        for (unsigned v = 0; v < fn->nvregs; v++)
            if (fn->vtype[v].s == OSC_T_BYTES || fn->vtype[v].s == OSC_T_CELLS) e |= EFF_CALLER;
        for (uint32_t i = 0; i < fn->ninsns; i++) switch (fn->insns[i].op) {
            case OSC_I_ALLOC: case OSC_I_RELEASE: case OSC_I_AOPEN: case OSC_I_AALLOC: case OSC_I_ADESTROY:
            case OSC_I_POPEN: case OSC_I_PCLOSE: case OSC_I_HALLOC: case OSC_I_HGEN: case OSC_I_HFREE:
            case OSC_I_HLOAD: case OSC_I_HSTORE:
                e |= EFF_HEAP; break;
            case OSC_I_SLOAD: case OSC_I_SSTORE:
                e |= EFF_CALLER; break;
            default: break;
            }
    }
    return e;
}

/* ---- hardware --------------------------------------------------------------------------------- */

/* "aarch64/<implementer>:<variant>:<part>:<revision>,..." over the distinct core kinds, sorted. */
static void host_cpu(char *out, size_t n)
{
    struct utsname un;
    char kinds[16][64];
    int nk = 0;
    char imp[16] = "?", var[16] = "?", part[16] = "?", rev[16] = "?";
    FILE *f = fopen("/proc/cpuinfo", "re");
    if (uname(&un)) snprintf(un.machine, sizeof un.machine, "unknown");
    if (f) {
        char line[256];
        int more = 1;
        while (more) {
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
                snprintf(k, sizeof k, "%s:%s:%s:%s", imp, var, part, rev);
                int seen = 0;
                for (int i = 0; i < nk; i++) if (!strcmp(kinds[i], k)) seen = 1;
                if (!seen && nk < 16) snprintf(kinds[nk++], sizeof kinds[0], "%s", k);
                snprintf(part, sizeof part, "?");
            }
        }
        fclose(f);
    }
    for (int i = 0; i < nk; i++)
        for (int j = i + 1; j < nk; j++)
            if (strcmp(kinds[j], kinds[i]) < 0) { char t[64]; memcpy(t, kinds[i], 64); memcpy(kinds[i], kinds[j], 64); memcpy(kinds[j], t, 64); }
    size_t o = (size_t)snprintf(out, n, "%s/", un.machine);
    for (int i = 0; i < nk && o < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? "," : "", kinds[i]);
    if (!nk && o < n) snprintf(out + o, n - o, "unknown");
}

/* ---- execution -------------------------------------------------------------------------------- */

typedef struct {
    uint64_t wall_ns, user_us, sys_us;
    long maxrss_kb;
} Measure;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t canon(uint64_t v, int kind)
{
    unsigned w = osc_scalar_width((OscScalar)kind);
    if (kind == OSC_T_BOOL) return v & 1;
    if (w >= 64) return v;
    uint64_t m = (1ull << w) - 1;
    v &= m;
    if (osc_scalar_signed((OscScalar)kind) && (v >> (w - 1)) & 1) v |= ~m;
    return v;
}

static void fmt_value(char *o, size_t n, uint64_t v, int kind)
{
    if (osc_scalar_signed((OscScalar)kind)) snprintf(o, n, "%" PRId64, (int64_t)v);
    else snprintf(o, n, "%" PRIu64, v);
}

/* Child: map the exact code bytes, enter seccomp strict mode (read, write, _exit, sigreturn only),
 * run every call, write the transcript to fd w, _exit. Native code that tries anything else is killed
 * by the kernel. The transcript is formatted without stdio (no hidden syscalls). */
static void child_exec(int w, const OscUnit *u, int fi, const uint8_t *code, size_t clen, uint32_t entry,
                       const uint64_t *in, uint32_t ncalls, unsigned np, int ret_kind)
{
    struct rlimit cpu = {10, 10};
    setrlimit(RLIMIT_CPU, &cpu);
    OscNative nm;
    OscRt *rt = malloc(sizeof *rt);
    size_t cap = (size_t)ncalls * 32u + 16u;
    char *buf = malloc(cap);
    if (!rt || !buf || osc_native_map(&nm, code, clen)) _exit(3);
    osc_rt_init(rt);
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0)) _exit(4);
#if VP_TAMPER == 3
    syscall(SYS_getpid); /* canary: any call outside read, write, _exit, sigreturn must kill the child */
#endif
    size_t o = 0;
    for (uint32_t i = 0; i < ncalls; i++) {
        uint64_t r = 0;
        osc_rt_reset(rt);
        int t = osc_native_call(u, fi, &nm, entry, rt, in + (size_t)i * np, np, &r);
        if (t < 0) syscall(SYS_exit, 5);
        char v[32];
        if (t > 0) snprintf(v, sizeof v, "%d", t);
        else fmt_value(v, sizeof v, canon(r, ret_kind), ret_kind);
        o += (size_t)snprintf(buf + o, cap - o, "%s %s\n", t > 0 ? "T" : "R", v);
    }
    size_t k = 0;
    while (k < o) {
        long r = syscall(SYS_write, w, buf + k, o - k);
        if (r <= 0) syscall(SYS_exit, 6);
        k += (size_t)r;
    }
    syscall(SYS_exit, 0);
}

/* ---- main ------------------------------------------------------------------------------------- */

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 4; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static void resolve(const char *base_dir, const char *p, char *out, size_t n)
{
    if (p[0] == '/') snprintf(out, n, "%s", p);
    else snprintf(out, n, "%s/%s", base_dir, p);
}

int main(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[1], "run") || (argc - 4) % 2) {
        fprintf(stderr, "usage: verified_program run <request> <outdir> --omegese P --omega-emit P --checker2 P [--commit SHA]\n");
        return 2;
    }
    const char *reqp = argv[2];
    const char *t_omegese = opt(argc, argv, "--omegese"), *t_ome = opt(argc, argv, "--omega-emit"),
               *t_c2 = opt(argc, argv, "--checker2"), *commit = opt(argc, argv, "--commit");
    if (!t_omegese || !t_ome || !t_c2) { fprintf(stderr, "verified_program: --omegese, --omega-emit and --checker2 are required\n"); return 2; }
    if (!commit) commit = "-";
    g_out = argv[3];
    char path[4096];
    if (strlen(g_out) > 1024 || strlen(reqp) > 1024) { fprintf(stderr, "verified_program: paths longer than 1024 bytes\n"); return 2; }
    if (mkdir(g_out, 0755) && errno != EEXIST) die_io(g_out);
    snprintf(path, sizeof path, "%s/result.txt", g_out);
    if (access(path, F_OK) == 0) { fprintf(stderr, "verified_program: %s exists; one run per outdir\n", path); return 2; }
    snprintf(path, sizeof path, "%s/store", g_out);
    if (mkdir(path, 0755) && errno != EEXIST) die_io(path);
    snprintf(path, sizeof path, "%s/receipts", g_out);
    if (mkdir(path, 0755) && errno != EEXIST) die_io(path);
    snprintf(path, sizeof path, "%s/work", g_out);
    if (mkdir(path, 0755) && errno != EEXIST) die_io(path);

    /* 1. request: schema first (an unknown version is refused as such), then the fixed layout */
    size_t rlen = 0;
    char *rtext = (char *)read_file(reqp, 65536, &rlen);
    if (!rtext) die_io(reqp);
    if (strncmp(rtext, "schema ", 7) == 0) {
        size_t sl = strcspn(rtext + 7, "\n");
        if (sl != strlen(REQ_SCHEMA) || strncmp(rtext + 7, REQ_SCHEMA, sl))
            refuse("UNKNOWN_VERSION", "request", "request schema is not %s", REQ_SCHEMA);
    }
    char *rv[NREQ], why[160];
    if (parse_kv(rtext, rlen, REQ_KEYS, NREQ, rv, why, sizeof why)) refuse("REQUEST_MALFORMED", "request", "%s", why);
    const char *objective_id = rv[1], *objective_text = rv[2], *fmt = rv[3], *entry = rv[5], *tier = rv[9];
    char req_dir[4096];
    snprintf(req_dir, sizeof req_dir, "%s", reqp);
    char *slash = strrchr(req_dir, '/');
    if (slash) *slash = 0; else snprintf(req_dir, sizeof req_dir, ".");
    char prog_path[4096], in_path[4096], grant_path[4096];
    resolve(req_dir, rv[4], prog_path, sizeof prog_path);
    resolve(req_dir, rv[8], in_path, sizeof in_path);
    resolve(req_dir, rv[10], grant_path, sizeof grant_path);
    uint64_t epoch = 0;
    if (parse_u64(rv[11], &epoch) || epoch == 0) refuse("REQUEST_MALFORMED", "request", "epoch is not a positive integer");
    int fmt_k = !strcmp(fmt, "omegese-v1") ? 0 : !strcmp(fmt, "oplist-v1") ? 1 : !strcmp(fmt, "oscir-v1") ? 2 : -1;
    if (fmt_k < 0) refuse("UNKNOWN_VERSION", "request", "program_format %s", fmt);
    if (strcmp(tier, TIER_HOST))
        refuse("TIER_UNSUPPORTED", "request", "tier %s: only %s executes here; a GPU tier needs its own lowering and device receipt", tier, TIER_HOST);
    struct utsname un;
    if (uname(&un) || strcmp(un.machine, "aarch64")) refuse("TIER_UNAVAILABLE", "request", "this host is not aarch64");
    unsigned np = 0;
    int pk[OSC_MAX_PARAMS], ret_kind = kind_of(rv[7]);
    {
        char ptxt[256];
        snprintf(ptxt, sizeof ptxt, "%s", rv[6]);
        for (char *s = strtok(ptxt, " "); s; s = strtok(NULL, " ")) {
            if (np == OSC_MAX_PARAMS || (pk[np] = kind_of(s)) < 0) refuse("REQUEST_MALFORMED", "request", "params: bad kind or more than 6");
            np++;
        }
    }
    if (ret_kind < 0) refuse("REQUEST_MALFORMED", "request", "ret: bad kind");

    /* 2. objective: the id is SHA-256 of the tag, LF, the text (sovereign-core objective.rs) */
    if (!*objective_text) refuse("REQUEST_MALFORMED", "objective", "empty objective text");
    for (const char *c = objective_text; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7f) refuse("REQUEST_MALFORMED", "objective", "control byte in the objective text");
    char obj_buf[70000];
    int obl = snprintf(obj_buf, sizeof obj_buf, "%s\n%s", OBJ_TAG, objective_text);
    char want_id[65];
    sha_hex((const uint8_t *)obj_buf, (size_t)obl, want_id);
    if (!is_hex64(objective_id) || strcmp(objective_id, want_id))
        refuse("OBJECTIVE_MISMATCH", "objective", "objective_id is not SHA-256 of the tag and this objective text");
    snprintf(g_objective_id, sizeof g_objective_id, "%s", objective_id);

    /* 3. grant (the runtime authority's record; checked, never minted here) */
    size_t glen = 0;
    char *gbytes = (char *)read_file(grant_path, 4096, &glen);
    if (!gbytes) refuse("GRANT_MISSING", "grant", "no readable grant");
    char *gcopy = malloc(glen + 1);
    if (!gcopy) die_io("malloc");
    memcpy(gcopy, gbytes, glen + 1);
    char *gv[NGRANT];
    if (strncmp(gcopy, "schema ", 7) == 0 && strncmp(gcopy + 7, GRANT_SCHEMA "\n", strlen(GRANT_SCHEMA) + 1))
        refuse("UNKNOWN_VERSION", "grant", "grant schema is not %s", GRANT_SCHEMA);
    if (parse_kv(gcopy, glen, GRANT_KEYS, NGRANT, gv, why, sizeof why)) refuse("GRANT_MALFORMED", "grant", "%s", why);
    uint64_t e_first = 0, e_last = 0, max_calls = 0;
    if (parse_u64(gv[4], &e_first) || parse_u64(gv[5], &e_last) || parse_u64(gv[6], &max_calls) || e_first > e_last ||
        max_calls == 0 || max_calls > MAX_CALLS)
        refuse("GRANT_MALFORMED", "grant", "epochs or max_calls");
    unsigned granted = 0;
    {
        char etxt[128];
        snprintf(etxt, sizeof etxt, "%s", gv[3]);
        for (char *s = strtok(etxt, " "); s; s = strtok(NULL, " ")) {
            if (!strcmp(s, "pure")) granted |= EFF_PURE;
            else if (!strcmp(s, "heap")) granted |= EFF_HEAP;
            else if (!strcmp(s, "caller_memory")) granted |= EFF_CALLER;
            else refuse("GRANT_MALFORMED", "grant", "unknown effect %s", s);
        }
    }
    if (strcmp(gv[1], objective_id)) refuse("GRANT_OBJECTIVE", "grant", "the grant names another objective");
    if (strcmp(gv[2], tier)) refuse("GRANT_TIER", "grant", "the grant is for tier %s", gv[2]);
    if (epoch < e_first || epoch > e_last) refuse("GRANT_EPOCH", "grant", "epoch %" PRIu64 " outside the grant window", epoch);

    /* 4. pins: the tool builds this run uses */
    char sha_omegese[65], sha_ome[65], sha_c2bin[65], sha_runner[65], c2id[65] = "";
    if (file_sha(t_omegese, sha_omegese) || file_sha(t_ome, sha_ome) || file_sha(t_c2, sha_c2bin) ||
        file_sha("/proc/self/exe", sha_runner))
        die_io("tool digest");
    static char cap[1u << 16];
    {
        char *av[] = {(char *)t_c2, "version", NULL};
        if (run_capture(av, cap, sizeof cap)) refuse("PIN_MISMATCH", "pin", "checker2 version failed");
        char *p = strstr(cap, "checker_id=");
        if (!p) refuse("PIN_MISMATCH", "pin", "checker2 printed no checker_id");
        snprintf(c2id, sizeof c2id, "%.64s", p + 11);
    }
    if (strcmp(rv[12], "-") && strcmp(rv[12], c2id)) refuse("PIN_MISMATCH", "pin", "checker2 identity %s is not the pinned one", c2id);
    if (strcmp(rv[13], "-") && strcmp(rv[13], sha_omegese)) refuse("PIN_MISMATCH", "pin", "omegese binary is not the pinned one");
    if (strcmp(rv[14], "-") && strcmp(rv[14], sha_ome)) refuse("PIN_MISMATCH", "pin", "omega_emit binary is not the pinned one");

    /* 5. program bytes -> op list -> canonical IR */
    size_t plen = 0;
    uint8_t *pbytes = read_file(prog_path, MAX_FILE, &plen);
    if (!pbytes) refuse("PROGRAM_MISSING", "program", "no readable program (max 1 MiB)");
    char sha_prog[65], sha_oplist[65] = "-";
    store_put(pbytes, plen, sha_prog);
    char json_path[4096], ir_path[4096];
    snprintf(json_path, sizeof json_path, "%s/work/program.json", g_out);
    snprintf(ir_path, sizeof ir_path, "%s/work/program.oscir", g_out);
    if (fmt_k == 0) {
        char sp[4096];
        snprintf(sp, sizeof sp, "%s/store/%s", g_out, sha_prog);
        char *av[] = {(char *)t_omegese, "emit", sp, json_path, NULL};
        int rc = run_capture(av, cap, sizeof cap);
        first_line(cap);
        if (rc == 1) refuse("EMIT_REFUSED", "emit", "%s", cap);
        if (rc) refuse("EMIT_REFUSED", "emit", "omegese exit %d", rc);
    } else if (fmt_k == 1) {
        if (put_file(json_path, pbytes, plen, 0644)) die_io(json_path);
    } else if (put_file(ir_path, pbytes, plen, 0644)) die_io(ir_path);
    if (fmt_k <= 1) {
        size_t jl = 0;
        uint8_t *jb = read_file(json_path, MAX_FILE, &jl);
        if (!jb) die_io(json_path);
        store_put(jb, jl, sha_oplist);
        free(jb);
        char *av[] = {(char *)t_ome, "emit", json_path, ir_path, NULL};
        int rc = run_capture(av, cap, sizeof cap);
        first_line(cap);
        if (rc) refuse("LOWER_REFUSED", "lower", "%s", cap);
    }
    size_t irlen = 0;
    uint8_t *ir = read_file(ir_path, MAX_FILE, &irlen);
    if (!ir) die_io(ir_path);
    store_put(ir, irlen, g_ir_sha);
    char ir_store[4096];
    snprintf(ir_store, sizeof ir_store, "%s/store/%s", g_out, g_ir_sha);

    /* 6. checker2 on the exact bytes in the store */
    {
        char *av[] = {(char *)t_c2, "check", ir_store, NULL};
        int rc = run_capture(av, cap, sizeof cap);
        first_line(cap);
        char want[128];
        snprintf(want, sizeof want, "checker2=ACCEPT ir_sha256=%s", g_ir_sha);
        if (rc || strcmp(cap, want)) refuse("CHECKER_REJECT", "checker2", "%s", cap);
    }
    OscUnit *u = calloc(1, sizeof *u);
    if (!u) die_io("malloc");
    int drc = osc_ir_decode(ir, irlen, u);
    if (drc) refuse("DECODE_REFUSED", "decode", "class=%s", osc_ir_refusal_name(drc));

    /* 7. signature and effects */
    int fi = -1;
    for (unsigned i = 0; i < u->nfuncs; i++) if (!strcmp(u->funcs[i].name, entry)) { fi = (int)i; break; }
    if (fi < 0) refuse("SIGNATURE_MISMATCH", "signature", "no function %s", entry);
    const OscFunc *fn = &u->funcs[fi];
    if (fn->nparams != np) refuse("SIGNATURE_MISMATCH", "signature", "param count %u, declared %u", fn->nparams, np);
    for (unsigned i = 0; i < np; i++)
        if ((int)fn->vtype[i].s != pk[i] || fn->vtype[i].ref != OSC_REF_NONE) refuse("SIGNATURE_MISMATCH", "signature", "param %u kind", i);
    if ((int)fn->ret.s != ret_kind || fn->ret.ref != OSC_REF_NONE) refuse("SIGNATURE_MISMATCH", "signature", "ret kind");
    unsigned need = effects_of(u);
    char need_txt[64], granted_txt[64];
    effects_text(need, need_txt, sizeof need_txt);
    effects_text(granted, granted_txt, sizeof granted_txt);
    if ((need & ~granted) || !(granted & EFF_PURE))
        refuse("UNAUTHORIZED_EFFECT", "effects", "program needs %s, grant allows %s", need_txt, granted_txt);

    /* 8. bounded inputs, canonical text */
    size_t ilen = 0;
    char *itext = (char *)read_file(in_path, 1u << 20, &ilen);
    if (!itext) refuse("INPUT_MALFORMED", "inputs", "no readable inputs");
    uint64_t *in = malloc(sizeof *in * (MAX_CALLS * OSC_MAX_PARAMS + 1));
    char *canon_in = malloc(MAX_CALLS * OSC_MAX_PARAMS * 24u + 16u);
    if (!in || !canon_in) die_io("malloc");
    uint32_t ncalls = 0;
    size_t co = 0;
    for (char *line = itext; *line;) {
        char *nl = strchr(line, '\n');
        if (!nl) refuse("INPUT_MALFORMED", "inputs", "last line has no newline");
        *nl = 0;
        if (ncalls == MAX_CALLS || ncalls >= max_calls) refuse("INPUT_LIMIT", "inputs", "more than %" PRIu64 " calls", max_calls);
        unsigned k = 0;
        for (char *s = strtok(line, " "); s; s = strtok(NULL, " ")) {
            if (k == np) refuse("INPUT_MALFORMED", "inputs", "line %u: more than %u values", ncalls + 1, np);
            int neg = *s == '-';
            uint64_t mag = 0;
            if (parse_u64(s + neg, &mag) || (neg && mag == 0)) refuse("INPUT_MALFORMED", "inputs", "line %u: not a decimal", ncalls + 1);
            unsigned wbits = osc_scalar_width((OscScalar)pk[k]);
            uint64_t v;
            if (pk[k] == OSC_T_BOOL) {
                if (neg || mag > 1) refuse("INPUT_RANGE", "inputs", "line %u: bool is 0 or 1", ncalls + 1);
                v = mag;
            } else if (osc_scalar_signed((OscScalar)pk[k])) {
                uint64_t lim = 1ull << (wbits - 1);
                if ((!neg && mag > lim - 1) || (neg && mag > lim)) refuse("INPUT_RANGE", "inputs", "line %u: value %u out of range", ncalls + 1, k + 1);
                v = neg ? (uint64_t)0 - mag : mag;
            } else {
                if (neg || (wbits < 64 && mag >> wbits)) refuse("INPUT_RANGE", "inputs", "line %u: value %u out of range", ncalls + 1, k + 1);
                v = mag;
            }
            in[(size_t)ncalls * np + k] = v;
            char t[32];
            fmt_value(t, sizeof t, v, pk[k]);
            co += (size_t)sprintf(canon_in + co, "%s%s", k ? " " : "", t);
            k++;
        }
        if (k != np) refuse("INPUT_MALFORMED", "inputs", "line %u: %u values, want %u", ncalls + 1, k, np);
        canon_in[co++] = '\n';
        ncalls++;
        line = nl + 1;
    }
    if (ncalls == 0) refuse("INPUT_LIMIT", "inputs", "no calls");
    char sha_in[65];
    store_put(canon_in, co, sha_in);

    /* 9. code image */
    OscCode code;
    memset(&code, 0, sizeof code);
    char err[200];
    if (osc_cg_compile(u, &code, err, sizeof err)) refuse("BACKEND_REFUSED", "backend", "%s", err);
    char sha_code[65];
    store_put(code.code, code.len, sha_code);
    uint32_t entry_off = code.entry[fi];

    /* 10. objective binding, grant, objective text into the store; checker2 signed receipt */
    char sha_obj[65], sha_grant[65], sha_bind[65], params_txt[128] = "";
    store_put(objective_text, strlen(objective_text), sha_obj);
    store_put(gbytes, glen, sha_grant);
    for (unsigned i = 0; i < np; i++) {
        size_t l = strlen(params_txt);
        snprintf(params_txt + l, sizeof params_txt - l, "%s%s", i ? "," : "", osc_scalar_name((OscScalar)pk[i]));
    }
    if (!np) snprintf(params_txt, sizeof params_txt, "-");
    char bind[1024];
    int bl = snprintf(bind, sizeof bind,
                      "schema=omega.verified_program.binding.v1\nobjective_id=%s\ngrant_sha256=%s\nentry=%s\nparams=%s\n"
                      "ret=%s\ntier=%s\nepoch=%" PRIu64 "\n",
                      objective_id, sha_grant, entry, params_txt, osc_scalar_name((OscScalar)ret_kind), tier, epoch);
    store_put(bind, (size_t)bl, sha_bind);
    char c2_id[65] = "";
    {
        char rdir[4096], exe[4096], bindp[4096], inp[4096], ep[32], ex[32];
        snprintf(rdir, sizeof rdir, "%s/work/c2r", g_out);
        if (mkdir(rdir, 0755) && errno != EEXIST) die_io(rdir);
        snprintf(exe, sizeof exe, "%s/store/%s", g_out, sha_code);
        snprintf(bindp, sizeof bindp, "%s/store/%s", g_out, sha_bind);
        snprintf(inp, sizeof inp, "%s/store/%s", g_out, sha_in);
        snprintf(ep, sizeof ep, "%" PRIu64, epoch);
        snprintf(ex, sizeof ex, "%" PRIu64, e_last);
        char *av[] = {(char *)t_c2, "receipt", "issue", ir_store, exe, "--target", (char *)tier, "--epoch", ep,
                      "--expiry", ex, "--out-dir", rdir, "--requirements", bindp, "--input", inp, NULL};
        int rc = run_capture(av, cap, sizeof cap);
        first_line(cap);
        if (rc) refuse("CHECKER_REJECT", "checker2-receipt", "%s", cap);
        /* checker2 prints "receipt=<dir>/<id>.rcpt" (id = SHA-256 of the 416 receipt bytes) */
        char *p = strncmp(cap, "receipt=", 8) ? NULL : strrchr(cap, '/');
        if (!p || strlen(p + 1) < 69 || strncmp(p + 65, ".rcpt", 5)) refuse("CHECKER_REJECT", "checker2-receipt", "unexpected output: %s", cap);
        snprintf(c2_id, sizeof c2_id, "%.64s", p + 1);
        if (!is_hex64(c2_id)) refuse("CHECKER_REJECT", "checker2-receipt", "unexpected output: %s", cap);
        snprintf(path, sizeof path, "%s/%s.rcpt", rdir, c2_id);
        size_t cl = 0;
        uint8_t *cr = read_file(path, 4096, &cl);
        if (!cr) die_io(path);
        char h[65];
        store_put(cr, cl, h);
        if (strcmp(h, c2_id)) refuse("CHECKER_REJECT", "checker2-receipt", "receipt id is not the hash of its bytes");
        free(cr);
    }

    /* 11. preflight, immediately before execution */
#if VP_TAMPER == 1 || VP_TAMPER == 2
    {
        snprintf(path, sizeof path, "%s/store/%s", g_out, VP_TAMPER == 1 ? g_ir_sha : sha_code);
        chmod(path, 0644);
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        uint8_t x = 0x5a;
        if (fd < 0 || pwrite(fd, &x, 1, VP_TAMPER == 1 ? 12 : 4) != 1) die_io("tamper");
        close(fd);
    }
#endif
    size_t ir2len = 0, code2len = 0;
    uint8_t *ir2 = read_file(ir_store, MAX_FILE, &ir2len);
    snprintf(path, sizeof path, "%s/store/%s", g_out, sha_code);
    uint8_t *code2 = read_file(path, MAX_FILE * 4u, &code2len);
    if (!ir2 || !code2) die_io("preflight read");
    char h[65];
    sha_hex(ir2, ir2len, h);
    if (strcmp(h, g_ir_sha)) refuse("PREFLIGHT_IR_CHANGED", "preflight", "IR bytes in the store changed after the check");
    {
        char *av[] = {(char *)t_c2, "check", ir_store, NULL};
        int rc = run_capture(av, cap, sizeof cap);
        first_line(cap);
        char want[128];
        snprintf(want, sizeof want, "checker2=ACCEPT ir_sha256=%s", g_ir_sha);
        if (rc || strcmp(cap, want)) refuse("PREFLIGHT_IR_CHANGED", "preflight", "checker2 on the bytes to run: %s", cap);
    }
    sha_hex(code2, code2len, h);
    if (strcmp(h, sha_code)) refuse("PREFLIGHT_CODE_CHANGED", "preflight", "code image in the store changed after the receipt");
    OscCode chk;
    memset(&chk, 0, sizeof chk);
    chk.code = code2; chk.len = code2len; chk.nfuncs = code.nfuncs;
    if (osc_cg_verify(&chk, err, sizeof err)) refuse("PREFLIGHT_CODE_INVALID", "preflight", "%s", err);
    OscUnit *u2 = calloc(1, sizeof *u2);
    OscCode code3;
    memset(&code3, 0, sizeof code3);
    if (!u2 || osc_ir_decode(ir2, ir2len, u2) || osc_cg_compile(u2, &code3, err, sizeof err) || code3.len != code2len ||
        memcmp(code3.code, code2, code2len) || code3.entry[fi] != entry_off)
        refuse("PREFLIGHT_CODE_CHANGED", "preflight", "the code image is not what the back end gives for the checked IR");

    /* 12. execution */
    int p[2];
    if (pipe2(p, O_CLOEXEC)) die_io("pipe");
    fflush(NULL);
    Measure m = {0, 0, 0, 0};
    uint64_t t0 = now_ns();
    pid_t c = fork();
    if (c < 0) die_io("fork");
    if (c == 0) {
        close(p[0]);
        child_exec(p[1], u2, fi, code2, code2len, entry_off, in, ncalls, np, ret_kind);
    }
    close(p[1]);
    size_t ocap = (size_t)ncalls * 32u + 16u, on = 0;
    char *outb = malloc(ocap + 1);
    if (!outb) die_io("malloc");
    for (;;) {
        ssize_t r = read(p[0], outb + on, ocap - on);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0 || (on += (size_t)r) == ocap) break;
    }
    close(p[0]);
    int st = 0;
    struct rusage ru;
    while (wait4(c, &st, 0, &ru) < 0) if (errno != EINTR) die_io("wait4");
    m.wall_ns = now_ns() - t0;
    m.user_us = (uint64_t)ru.ru_utime.tv_sec * 1000000u + (uint64_t)ru.ru_utime.tv_usec;
    m.sys_us = (uint64_t)ru.ru_stime.tv_sec * 1000000u + (uint64_t)ru.ru_stime.tv_usec;
    m.maxrss_kb = ru.ru_maxrss;
    if (WIFSIGNALED(st)) refuse("EXEC_FAILED", "execute", "killed by signal %d (seccomp or CPU limit)", WTERMSIG(st));
    if (!WIFEXITED(st) || WEXITSTATUS(st)) refuse("EXEC_FAILED", "execute", "executor exit %d", WEXITSTATUS(st));
    outb[on] = 0;
    uint32_t lines = 0;
    for (size_t i = 0; i < on; i++) if (outb[i] == '\n') lines++;
    if (lines != ncalls) refuse("EXEC_FAILED", "execute", "%u result lines for %u calls", lines, ncalls);
    char sha_out[65];
    store_put(outb, on, sha_out);

    /* 13. the execution receipt (deterministic: no time, no pid, no path) */
    char cpu[512], rbuf[8192], rid[65];
    host_cpu(cpu, sizeof cpu);
    int rn = snprintf(rbuf, sizeof rbuf,
                      "schema=omega.verified_program.receipt.v1\nverdict=EXECUTED\nobjective_id=%s\nobjective_text_sha256=%s\n"
                      "grant_sha256=%s\nbinding_sha256=%s\nprogram_format=%s\nprogram_sha256=%s\noplist_sha256=%s\n"
                      "ir_sha256=%s\ncode_sha256=%s\nentry=%s\nparams=%s\nret=%s\ninputs_sha256=%s\noutputs_sha256=%s\n"
                      "calls=%u\neffects_required=%s\neffects_granted=%s\nchecker2_verdict=ACCEPT\nchecker2_id=%s\n"
                      "checker2_receipt_id=%s\nchecker2_signer=TEST\nomegese_sha256=%s\nomega_emit_sha256=%s\n"
                      "checker2_bin_sha256=%s\nrunner_sha256=%s\ncommit=%s\ntier=%s\nhost_cpu=%s\nmachine_id=absent\n"
                      "gpu_used=no\nsandbox=seccomp-strict\nepoch=%" PRIu64 "\n",
                      objective_id, sha_obj, sha_grant, sha_bind, fmt, sha_prog, sha_oplist, g_ir_sha, sha_code, entry,
                      params_txt, osc_scalar_name((OscScalar)ret_kind), sha_in, sha_out, ncalls, need_txt, granted_txt,
                      c2id, c2_id, fmt_k == 0 ? sha_omegese : "-", fmt_k <= 1 ? sha_ome : "-", sha_c2bin, sha_runner,
                      commit, tier, cpu, epoch);
    sha_hex((const uint8_t *)rbuf, (size_t)rn, rid);
    snprintf(path, sizeof path, "%s/receipts/%s.vpr", g_out, rid);
    if (put_file(path, rbuf, (size_t)rn, 0444)) die_io(path);
    char mbuf[1024];
    int mn = snprintf(mbuf, sizeof mbuf,
                      "schema=omega.verified_program.measure.v1\nreceipt_id=%s\nwall_ns=%" PRIu64 "\nuser_us=%" PRIu64
                      "\nsys_us=%" PRIu64 "\nmaxrss_kb=%ld\ncalls=%u\nir_bytes=%zu\ncode_bytes=%zu\n",
                      rid, m.wall_ns, m.user_us, m.sys_us, m.maxrss_kb, ncalls, irlen, code2len);
    snprintf(path, sizeof path, "%s/measure.txt", g_out);
    if (put_file(path, mbuf, (size_t)mn, 0644)) die_io(path);
    write_result("EXECUTED", "-", "-", "-", sha_out, rid, c2_id);
    printf("EXECUTED receipt=%s outputs_sha256=%s calls=%u\n", rid, sha_out, ncalls);
    return 0;
}
