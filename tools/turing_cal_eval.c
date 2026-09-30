/* turing-cal-eval: the EXP-001 primary evaluator (Turing-profile-v1.0).
 * Normative description: calibration/docs/EVALUATOR.md. Statistics:
 * calibration/docs/UNCERTAINTY_PROTOCOL.md. Coders: calibration/docs/CODER_SPEC.md.
 *
 *   turing-cal-eval run [--dry-run] --repo DIR --manifest CAND_MANIFEST --cand-dir DIR [--cand-dir DIR ...]
 *                       --dataset DATASET_MANIFEST [--overlap OVERLAP_AUDIT] --out DIR --work DIR
 *                       [--only NAME,NAME,...]   (dry run only)
 *   turing-cal-eval gate [--dry-run] --bundle DIR --independent SCORER_INDEPENDENT_JSON
 *
 * run: checks the freeze order and every digest (refuses, exit 2, void_receipt_<n>.json, on any mismatch),
 * then for each candidate and sealed file: produce TPS1 + TSY1, write them, re-read and parse them, encode
 * with coder A (TCR1 range) and coder B (TCA1 rANS) from the same parsed TPS1, write the coded files,
 * re-read them, decode, compare. Per crumb: ideal length, both coders on the crumb alone (+56-byte header),
 * round trip. Then the per-crumb bootstrap, envelope (per file and per crumb), reversal audit, S1-S7 and S9.
 * Writes ideal_lengths.json, scorer_primary.json, uncertainty.json, probability_streams/INDEX,
 * arithmetic/results.json, ans/results.json, encoded_artifacts/INDEX, decoder_receipts/, and the pending
 * final receipt + report. gate: compares scorer_independent.json with scorer_primary.json exactly (S8),
 * fixes the verdict and writes final_receipt.json + REPORT.md exactly once.
 *
 * All decisions use int64 micro-bits (ub, 1 bit = 1e6 ub). C and POSIX only. */
#include "sha256.h"
#include "turing/tc_produce.h"
#include "turing/tc_pstream.h"
#include "turing/tc_range.h"
#include "turing/tc_rans.h"
#include "turing/ty_ctr1.h"
#include "turing/ty_model.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UB INT64_C(1000000)
#define ENV_CENTER INT64_C(448000000)
#define ENV_A INT64_C(64000000)
#define ENV_B INT64_C(1000)
#define BOOT_B 10000
#define BOOT_LO 250
#define BOOT_HI 9749
#define BOOT_SEED 0x4558503030315543ULL
#define MAXC 16
#define MAXF 16
#define BASELINE "B2_order1"
#define CANDIDATE "M_candidate"
#define MEM1 "M_mem"
#define MEM2 "M_mem_seed1"
#define DRY_FORBIDDEN "calibration/experiments/EXP-001"

static char why[1024];
static const char *g_out;
static int g_dry;
/* g_scored: set once the first probability stream has been produced. From then on the verdict is final
 * (FAILURE_REPORTING.md section 2): a failure is written as a final FAIL receipt, never as a void. */
static int g_scored;
static char g_cmd[4096];     /* the command line, recorded in every void receipt (identical-retry check) */
static char g_inputs[1024];  /* "<name> <sha256>" pairs of the input files, same purpose */
#define MAX_ATTEMPTS 3
/* Receipt binding (every void and terminal receipt carries these three; CAL-0 review 2 Q5). Filled by a pre-pass
 * before any receipt can be written: run hashes the profile and candidate manifest it was given and reads
 * freeze_commit from the dataset manifest ("DRY_RUN" in a dry run); gate copies them from the pending receipt. */
static char g_rprof[65], g_rman[65], g_rfreeze[48];

/* ---------- small helpers ---------- */

static void utc_now(char out[32]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void jclean(char *s) {
    for (; *s; ++s)
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) *s = '\'';
}

/* Exclusive create of <out>/<name>; NULL if it exists or cannot be made. */
static FILE *excl_open(const char *name) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", g_out, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
    return fd >= 0 ? fdopen(fd, "w") : NULL;
}

/* Terminal failure after scoring started: final_receipt.json with verdict FAIL, the failing criterion FAIL and
 * every other criterion NOT_REACHED (prereg section 5a: NOT_REACHED counts as FAIL). Exit status 1. */
static int fail_final(const char *crit, const char *step, const char *code, const char *msg) {
    fprintf(stderr, "FAILED %s: %s: %s (%s; the verdict is final)\n", step, code, msg, crit);
    if (!g_out) return 1;
    FILE *f = excl_open("final_receipt.json");
    if (!f) return 1;
    char t[32], m[1024];
    utc_now(t);
    snprintf(m, sizeof m, "%s", msg);
    jclean(m);
    fprintf(f, "{\n  \"schema\": \"turing.cal.terminal_receipt.v1\",\n  \"experiment\": \"EXP-001\",\n  \"kind\": \"terminal_fail\",\n"
               "  \"dry_run\": %s,\n  \"created_utc\": \"%s\",\n  \"verdict\": \"FAIL\",\n  \"criteria\": {",
            g_dry ? "true" : "false", t);
    for (int i = 1; i <= 9; ++i) {
        char nm[4];
        snprintf(nm, sizeof nm, "S%d", i);
        fprintf(f, "%s\"%s\": \"%s\"", i > 1 ? ", " : "", nm, strcmp(nm, crit) ? "NOT_REACHED" : "FAIL");
    }
    fprintf(f, "},\n  \"failed_criterion\": \"%s\",\n  \"step\": \"%s\",\n  \"code\": \"%s\",\n  \"reason\": \"%s\",\n"
               "  \"profile_digest\": \"%s\",\n  \"freeze_commit\": \"%s\",\n  \"candidate_manifest_sha256\": \"%s\",\n"
               "  \"EXP_001_COMPRESSION_BRIDGE\": \"%s\"\n}\n",
            crit, step, code, m, g_rprof, g_rfreeze, g_rman, g_dry ? "DRY_RUN_NOT_EVIDENCE" : "FAIL");
    fclose(f);
    return 1;
}

/* Refusal before any score exists: the attempt is VOID. Writes <out>/void_receipt_<n>.json (n = attempt number, the
 * first free one, exclusive create, never overwritten) when the out dir exists. In sealed mode the third void
 * attempt also writes final_receipt.json with verdict INCONCLUSIVE, reason INFRA (FAILURE_REPORTING.md section 2).
 * After scoring started, a refusal is a terminal FAIL of the named criterion (see fail_final). */
static int refuse_c(const char *crit, const char *step, const char *code, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_scored) return fail_final(crit, step, code, msg);
    fprintf(stderr, "REFUSED %s: %s: %s\n", step, code, msg);
    if (g_out) {
        char t[32], name[64];
        FILE *f = NULL;
        int n = 1;
        for (; n < 1000 && !f; ++n) {
            snprintf(name, sizeof name, "void_receipt_%d.json", n);
            f = excl_open(name);
            if (!f && errno != EEXIST) break;
        }
        --n;
        if (f) {
            utc_now(t);
            jclean(msg);
            fprintf(f,
                    "{\n  \"schema\": \"turing.cal.void_receipt.v1\",\n  \"experiment\": \"EXP-001\",\n"
                    "  \"kind\": \"void\",\n  \"stage\": \"evaluation\",\n  \"dry_run\": %s,\n  \"created_utc\": \"%s\",\n"
                    "  \"attempt\": %d,\n  \"step\": \"%s\",\n  \"code\": \"%s\",\n  \"reason\": \"%s\",\n  \"command\": \"%s\",\n"
                    "  \"inputs\": \"%s\",\n  \"profile_digest\": \"%s\",\n  \"freeze_commit\": \"%s\",\n"
                    "  \"candidate_manifest_sha256\": \"%s\"\n}\n",
                    g_dry ? "true" : "false", t, n, step, code, msg, g_cmd, g_inputs, g_rprof, g_rfreeze, g_rman);
            fclose(f);
            if (!g_dry && n >= MAX_ATTEMPTS && (f = excl_open("final_receipt.json"))) {
                fprintf(f,
                        "{\n  \"schema\": \"turing.cal.terminal_receipt.v1\",\n  \"experiment\": \"EXP-001\",\n"
                        "  \"kind\": \"inconclusive_infra\",\n  \"dry_run\": false,\n  \"created_utc\": \"%s\",\n"
                        "  \"verdict\": \"INCONCLUSIVE\",\n  \"reason\": \"INFRA\",\n  \"void_attempts\": %d,\n"
                        "  \"last_step\": \"%s\",\n  \"last_code\": \"%s\",\n"
                        "  \"profile_digest\": \"%s\",\n  \"freeze_commit\": \"%s\",\n  \"candidate_manifest_sha256\": \"%s\",\n"
                        "  \"EXP_001_COMPRESSION_BRIDGE\": \"INCONCLUSIVE\"\n}\n",
                        t, n, step, code, g_rprof, g_rfreeze, g_rman);
                fclose(f);
                fprintf(stderr, "attempt %d of %d was void: verdict INCONCLUSIVE (INFRA), final_receipt.json written\n", n,
                        MAX_ATTEMPTS);
            }
        }
    }
    return 2;
}
#define refuse(...) refuse_c("NONE", __VA_ARGS__)

/* A freeze-order violation fixed by the frozen and published inputs (overlap audit FAIL, candidate manifest or
 * profile not the ones at C_f, freeze after release, seed not derived from C_f) is S2 FAIL, final, in sealed mode
 * (FAILURE_REPORTING.md section 2): retrying with the same inputs cannot change it. In a dry run it is a void. */
static int s2_fail(const char *step, const char *code, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_dry) return refuse(step, code, "%s", msg);
    return fail_final("S2", step, code, msg);
}

static void hexs(const uint8_t d[32], char o[65]) { tc_hex(d, o); }

/* Run argv (no shell), capture up to n-1 bytes of stdout (trailing newline stripped). Returns the exit status,
 * -1 if it could not run. */
static int run_capture(char *const argv[], char *buf, size_t n) {
    int fd[2];
    buf[0] = 0;
    if (pipe(fd)) return -1;
    pid_t pid = fork();
    if (pid < 0) return close(fd[0]), close(fd[1]), -1;
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]), close(fd[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    size_t k = 0;
    char tmp[4096];
    ssize_t r;
    while ((r = read(fd[0], tmp, sizeof tmp)) > 0)
        for (ssize_t i = 0; i < r; ++i)
            if (k + 1 < n) buf[k++] = tmp[i];
    buf[k] = 0;
    close(fd[0]);
    int st;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return -1;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    return WEXITSTATUS(st);
}

/* Run argv (no shell) and SHA-256 its exact stdout bytes. Returns the exit status, -1 if it could not run. */
static int run_sha(char *const argv[], char o[65]) {
    int fd[2];
    o[0] = 0;
    if (pipe(fd)) return -1;
    pid_t pid = fork();
    if (pid < 0) return close(fd[0]), close(fd[1]), -1;
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]), close(fd[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    sha256_ctx c;
    sha256_init(&c);
    uint8_t tmp[4096], d[32];
    ssize_t r;
    while ((r = read(fd[0], tmp, sizeof tmp)) > 0) sha256_update(&c, tmp, (size_t)r);
    close(fd[0]);
    sha256_final(&c, d);
    tc_hex(d, o);
    int st;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

/* "YYYY-MM-DDTHH:MM:SSZ" -> seconds since 1970 (UTC), -1 if malformed. */
static int64_t utc_epoch(const char *s) {
    int y, mo, d, h, mi, se;
    if (strlen(s) != 20 || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6 || mo < 1 || mo > 12)
        return -1;
    y -= mo <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + se;
}

/* Sealed seed rule turing.cal.sealed.v1 (generate_sealed_data.sh): first 16 hex of
 * SHA-256("turing.cal.sealed.v1|<commit>|<profile digest>|g<g>|<j>"), top bit cleared. */
static int64_t sealed_seed(const char *commit, const char *digest, int g, int j) {
    char m[256];
    uint8_t d[32];
    snprintf(m, sizeof m, "turing.cal.sealed.v1|%s|%s|g%d|%d", commit, digest, g, j);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)m, strlen(m));
    sha256_final(&c, d);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | d[i];
    return (int64_t)(v & 0x7fffffffffffffffULL);
}

/* Notebook record (lab protocol, notebook record): who ran it, where, from which commit, clean tree or not. */
typedef struct {
    char operator_[128], host[256], kernel[128], commit[64];
    int dirty; /* 0 clean, 1 dirty, -1 unknown (not a git checkout) */
} notebook_t;

static void json_clean(char *s) {
    for (; *s; ++s)
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20) *s = '_';
}

static void notebook_of(const char *repo, notebook_t *nb) {
    char out[8192];
    char *a1[] = {"git", "-C", (char *)repo, "config", "user.name", NULL};
    if (run_capture(a1, nb->operator_, sizeof nb->operator_) != 0 || !nb->operator_[0]) snprintf(nb->operator_, sizeof nb->operator_, "UNKNOWN");
    char *a2[] = {"git", "-C", (char *)repo, "rev-parse", "HEAD", NULL};
    if (run_capture(a2, nb->commit, sizeof nb->commit) != 0 || strlen(nb->commit) != 40) snprintf(nb->commit, sizeof nb->commit, "UNKNOWN");
    char *a3[] = {"git", "-C", (char *)repo, "status", "--porcelain", NULL};
    int rc = run_capture(a3, out, sizeof out);
    nb->dirty = rc != 0 ? -1 : (out[0] != 0);
    /* --repo must be the top of its own checkout, not a folder inside another one. */
    char *a4[] = {"git", "-C", (char *)repo, "rev-parse", "--show-toplevel", NULL};
    char top[PATH_MAX], rr[PATH_MAX], rt[PATH_MAX];
    if (run_capture(a4, top, sizeof top) != 0 || !realpath(repo, rr) || !realpath(top, rt) || strcmp(rr, rt)) nb->dirty = -1;
    if (gethostname(nb->host, sizeof nb->host)) snprintf(nb->host, sizeof nb->host, "UNKNOWN");
    nb->host[sizeof nb->host - 1] = 0;
    struct utsname u;
    if (uname(&u)) snprintf(nb->kernel, sizeof nb->kernel, "UNKNOWN");
    else snprintf(nb->kernel, sizeof nb->kernel, "%s %s %s", u.sysname, u.release, u.machine);
    json_clean(nb->operator_), json_clean(nb->host), json_clean(nb->kernel);
}

static int sha_file_hex(const char *path, char o[65]) {
    uint8_t d[32];
    if (tc_file_sha256(path, d) != TC_OK) return -1;
    hexs(d, o);
    return 0;
}

static char *slurp(const char *path, size_t *len) {
    uint8_t *b;
    size_t n;
    if (tc_read_file(path, &b, &n) != TC_OK) return NULL;
    char *s = realloc(b, n + 1);
    if (!s) return free(b), NULL;
    s[n] = 0;
    if (len) *len = n;
    return s;
}

/* Line-oriented JSON field access (our scripts write one entry per line). */
static int jstr(const char *line, const char *key, char *out, size_t n) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') ++p;
    if (*p != '"') return -1;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return *p == '"' ? 0 : -1;
}
static int jint(const char *line, const char *key, int64_t *v) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') ++p;
    char *e;
    errno = 0;
    long long x = strtoll(p, &e, 10);
    if (e == p || errno) return -1;
    if (*e != ',' && *e != '}' && *e != ' ' && *e != '\n' && *e != '\r' && *e != 0) return -1; /* integers only */
    *v = x;
    return 0;
}
/* First line of text containing key (as "key":), copied into buf. */
static int jtop(const char *text, const char *key, char *out, size_t n) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(text, pat);
    if (!p) return -1;
    const char *ls = p;
    while (ls > text && ls[-1] != '\n') --ls;
    const char *le = strchr(p, '\n');
    size_t l = le ? (size_t)(le - ls) : strlen(ls);
    char line[4096];
    if (l >= sizeof line) return -1;
    memcpy(line, ls, l);
    line[l] = 0;
    return jstr(line, key, out, n);
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    return (mkdir(tmp, 0755) && errno != EEXIST) ? -1 : 0;
}

static void fmt_bits(int64_t ub, char *o, size_t n) {
    uint64_t a = ub < 0 ? (uint64_t)(-(ub + 1)) + 1 : (uint64_t)ub;
    snprintf(o, n, "%s%" PRIu64 ".%06" PRIu64, ub < 0 ? "-" : "", a / 1000000u, a % 1000000u);
}

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static int64_t env_band(uint64_t n) { return ENV_A + ENV_B * (int64_t)n; }
static int64_t iabs64(int64_t x) { return x < 0 ? -x : x; }

/* ---------- inputs ---------- */

typedef struct {
    char name[64], file[128], file_sha[65], model_digest[65], path[PATH_MAX];
    int64_t lm_bits;
} cand_t;

typedef struct {
    int g, j;
    int64_t seed, bytes;
    char sha[65], path[PATH_MAX];
    ty_stream s;
    uint8_t dd[32];
} dfile_t;

typedef struct { /* one crumb of one file */
    uint32_t ord;
    uint64_t lo, hi;
} crumb_t;

/* Per (candidate, file) results. */
typedef struct {
    int64_t ideal_ub, crumb_sum_ub;
    uint64_t nrange, nrans; /* coded file bytes (header included) */
    int lossless;
    int64_t *crumb_ub;       /* per crumb ideal */
    uint64_t *crumb_range, *crumb_rans; /* per crumb coded bytes (+56) */
    int env_file_fail[2];
    int env_crumb_fail[2];
    int64_t worst_crumb_margin[2]; /* min over crumbs of band - |ovh - 448e6| */
    char tps[65], tsy[65], range_sha[65], rans_sha[65];
} res_t;

static cand_t C[MAXC];
static int nc;
static dfile_t F[MAXF];
static int nf;
static crumb_t *CR[MAXF];
static size_t ncr[MAXF];
static res_t R[MAXC][MAXF];

static int cand_index(const char *name) {
    for (int i = 0; i < nc; ++i)
        if (!strcmp(C[i].name, name)) return i;
    return -1;
}

/* ---------- run ---------- */

typedef struct {
    int64_t point, lo, hi;
} iv_t;

static iv_t interval_of(int64_t *tb, int64_t point) {
    qsort(tb, BOOT_B, sizeof *tb, cmp_i64);
    iv_t v = {point, tb[BOOT_LO], tb[BOOT_HI]};
    return v;
}

static int under_forbidden(const char *repo, const char *path) {
    char rr[PATH_MAX], rp[PATH_MAX], forb[PATH_MAX];
    if (!realpath(repo, rr) || !realpath(path, rp)) return 1;
    snprintf(forb, sizeof forb, "%s/%s", rr, DRY_FORBIDDEN);
    size_t l = strlen(forb);
    return !strncmp(rp, forb, l) && (rp[l] == 0 || rp[l] == '/');
}

static int write_text(const char *dir, const char *name, const char *text) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fputs(text, f);
    return fclose(f);
}

static FILE *open_out(const char *rel) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", g_out, rel);
    return fopen(p, "w");
}

/* Root over a set of files: SHA-256 over sorted "<sha256>  <relpath>\n" lines (like sha256sum). */
static int root_of(const char *dir, const char **rels, int n, char o[65]) {
    char lines[64][PATH_MAX + 80];
    for (int i = 0; i < n; ++i) {
        char p[PATH_MAX], h[65];
        snprintf(p, sizeof p, "%s/%s", dir, rels[i]);
        if (sha_file_hex(p, h)) return -1;
        snprintf(lines[i], sizeof lines[i], "%s  %s\n", h, rels[i]);
    }
    for (int i = 1; i < n; ++i) /* insertion sort by line */
        for (int k = i; k > 0 && strcmp(lines[k - 1], lines[k]) > 0; --k) {
            char t[PATH_MAX + 80];
            strcpy(t, lines[k]), strcpy(lines[k], lines[k - 1]), strcpy(lines[k - 1], t);
        }
    sha256_ctx c;
    sha256_init(&c);
    for (int i = 0; i < n; ++i) sha256_update(&c, (const uint8_t *)lines[i], strlen(lines[i]));
    uint8_t d[32];
    sha256_final(&c, d);
    hexs(d, o);
    return 0;
}

static int self_sha(char o[65]) {
    char p[PATH_MAX];
    ssize_t l = readlink("/proc/self/exe", p, sizeof p - 1);
    if (l <= 0) return -1;
    p[l] = 0;
    return sha_file_hex(p, o);
}

static int cmd_run(int argc, char **argv) {
    const char *repo = NULL, *manifest = NULL, *dataset = NULL, *overlap = NULL, *work = NULL, *only = NULL;
    const char *cdirs[8];
    int ncd = 0;
    for (int i = 2; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--dry-run")) g_dry = 1;
        else if (!v) return refuse("args", "ARG", "%s needs a value", a);
        else if (!strcmp(a, "--repo")) repo = v, ++i;
        else if (!strcmp(a, "--manifest")) manifest = v, ++i;
        else if (!strcmp(a, "--dataset")) dataset = v, ++i;
        else if (!strcmp(a, "--overlap")) overlap = v, ++i;
        else if (!strcmp(a, "--out")) g_out = v, ++i;
        else if (!strcmp(a, "--work")) work = v, ++i;
        else if (!strcmp(a, "--only")) only = v, ++i;
        else if (!strcmp(a, "--cand-dir") && ncd < 8) cdirs[ncd++] = v, ++i;
        else return refuse("args", "ARG", "unknown argument %s", a);
    }
    /* Command and input record for the identical-retry rule (FAILURE_REPORTING.md section 2). */
    for (int i = 0, k = 0; i < argc && k < (int)sizeof g_cmd - 2; ++i)
        k += snprintf(g_cmd + k, sizeof g_cmd - (size_t)k, "%s%s", i ? " " : "", argv[i]);
    {
        const char *nm[] = {"turing-cal-eval", "manifest", "dataset", "overlap"};
        char self[PATH_MAX] = "/proc/self/exe";
        const char *pp[] = {self, manifest, dataset, overlap};
        for (int i = 0, k = 0; i < 4; ++i) {
            char h[65] = "NONE";
            if (pp[i] && sha_file_hex(pp[i], h)) snprintf(h, sizeof h, "UNREADABLE");
            k += snprintf(g_inputs + k, sizeof g_inputs - (size_t)k, "%s%s %s", i ? " " : "", nm[i], h);
        }
        jclean(g_cmd);
    }
    const char *outp = g_out;
    g_out = NULL; /* no void receipt until the out dir is checked */
    if (!repo || !manifest || !dataset || !outp || !work || !ncd)
        return refuse("args", "ARG", "need --repo --manifest --cand-dir --dataset --out --work");
    if (only && !g_dry) return refuse("args", "ARG", "--only is allowed in a dry run only");
    /* Receipt-binding pre-pass (no receipt yet: a failure here is not an attempt, like a bad argument). */
    {
        char pp[PATH_MAX];
        snprintf(pp, sizeof pp, "%s/calibration/profiles/Turing-profile-v1.0.toml", repo);
        if (sha_file_hex(pp, g_rprof)) return refuse("args", "IO", "cannot read %s", pp);
        if (sha_file_hex(manifest, g_rman)) return refuse("args", "IO", "cannot read %s", manifest);
        snprintf(g_rfreeze, sizeof g_rfreeze, "DRY_RUN");
        if (!g_dry) {
            char *d0 = slurp(dataset, NULL);
            char fc[64] = "";
            if (!d0) return refuse("args", "IO", "cannot read %s", dataset);
            jtop(d0, "freeze_commit", fc, sizeof fc);
            free(d0);
            int ok = strlen(fc) == 40;
            for (int i = 0; ok && i < 40; ++i) ok = (fc[i] >= '0' && fc[i] <= '9') || (fc[i] >= 'a' && fc[i] <= 'f');
            if (!ok) return refuse("args", "FREEZE_COMMIT", "dataset freeze_commit '%s' is not a 40-hex commit", fc);
            snprintf(g_rfreeze, sizeof g_rfreeze, "%s", fc);
        }
    }
    if (mkdir_p(outp) || mkdir_p(work)) return refuse("args", "IO", "cannot create out/work dirs");
    /* One VOID counter for all of EXP-001 (FAILURE_REPORTING.md section 2): in sealed mode the bundle must be
     * <eval root>/<C_f>/run/bundle, the directory where sealed generation and the frozen-tree tests also write their
     * void receipts, so every void of the experiment lands in one numbered sequence. */
    if (!g_dry) {
        char rp[PATH_MAX], suf[128];
        snprintf(suf, sizeof suf, "/%s/run/bundle", g_rfreeze);
        size_t lr, ls = strlen(suf);
        if (!realpath(outp, rp) || (lr = strlen(rp)) < ls || strcmp(rp + lr - ls, suf))
            return refuse("args", "OUT_PATH", "--out must resolve to <eval root>/%s/run/bundle (one void counter per experiment)",
                          g_rfreeze);
        int nv = 0;
        for (int k = 1; k < 1000; ++k) {
            char p[PATH_MAX];
            struct stat st;
            snprintf(p, sizeof p, "%s/void_receipt_%d.json", outp, k);
            if (stat(p, &st)) break;
            ++nv;
        }
        if (nv >= MAX_ATTEMPTS)
            return refuse("args", "ATTEMPTS", "%d void attempts already recorded: EXP-001 is INCONCLUSIVE (INFRA)", nv);
    }
    if (g_dry && (under_forbidden(repo, outp) || under_forbidden(repo, work)))
        return refuse("dry run", "DRY_RUN_TARGET", "a dry run may not write under %s", DRY_FORBIDDEN);
    {
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof p, "%s/final_receipt.json", outp);
        if (!stat(p, &st)) return refuse("write once", "EXISTS", "%s already exists; a verdict is never replaced", p);
    }
    g_out = outp;
    /* A sealed retry after a void evaluation attempt must be the byte-identical command on byte-identical inputs:
     * it is compared with the first void receipt of stage "evaluation" (generation and test voids share the
     * numbering but record other commands). */
    if (!g_dry) {
        for (int k = 1; k < 1000; ++k) {
            char p[PATH_MAX], stg[32] = "";
            snprintf(p, sizeof p, "%s/void_receipt_%d.json", g_out, k);
            char *v1 = slurp(p, NULL);
            if (!v1) break;
            jtop(v1, "stage", stg, sizeof stg);
            if (strcmp(stg, "evaluation")) {
                free(v1);
                continue;
            }
            char c1[sizeof g_cmd] = "", i1[sizeof g_inputs] = "";
            jtop(v1, "command", c1, sizeof c1);
            jtop(v1, "inputs", i1, sizeof i1);
            free(v1);
            if (strcmp(c1, g_cmd) || strcmp(i1, g_inputs))
                return refuse("retry", "RETRY_DIFFERS", "a retry must repeat the first void evaluation attempt byte for byte (command and input digests)");
            break;
        }
    }
    /* Notebook record; a non-dry run refuses a dirty or unverifiable tree. */
    notebook_t nb;
    notebook_of(repo, &nb);
    if (!g_dry && nb.dirty != 0)
        return refuse("notebook", "DIRTY_TREE", "%s", nb.dirty < 0 ? "--repo is not a git checkout (clean state cannot be verified)" : "--repo has uncommitted changes (git status --porcelain is not empty)");
    char t0[32];
    utc_now(t0);

    /* --- profile --- */
    char prof_path[PATH_MAX], side_path[PATH_MAX], prof_sha[65], side_sha[65] = "";
    snprintf(prof_path, sizeof prof_path, "%s/calibration/profiles/Turing-profile-v1.0.toml", repo);
    snprintf(side_path, sizeof side_path, "%s/calibration/profiles/Turing-profile-v1.0.sha256", repo);
    if (sha_file_hex(prof_path, prof_sha)) return refuse("profile", "IO", "cannot read %s", prof_path);
    char *side = slurp(side_path, NULL);
    if (!side || sscanf(side, "%64s", side_sha) != 1 || strcmp(side_sha, prof_sha))
        return refuse("profile", "PROFILE_DIGEST", "profile sha256 %s does not match the sidecar", prof_sha);
    free(side);
    char *prof = slurp(prof_path, NULL);
    if (!prof) return refuse("profile", "IO", "profile");
    int prof_fill = strstr(prof, "FILL_AT_FREEZE") != NULL;
    free(prof);
    if (prof_fill && !g_dry) return refuse("freeze order", "NOT_FROZEN", "profile still contains FILL_AT_FREEZE");
    uint8_t pd[32];
    tc_unhex(prof_sha, pd);

    /* --- candidate manifest --- */
    char man_sha[65];
    char *man = slurp(manifest, NULL);
    if (!man || sha_file_hex(manifest, man_sha)) return refuse("manifest", "IO", "cannot read %s", manifest);
    char status[32] = "", frozen_at[40] = "", m_prof[65] = "";
    jtop(man, "status", status, sizeof status);
    jtop(man, "frozen_at", frozen_at, sizeof frozen_at);
    jtop(man, "profile_sha256", m_prof, sizeof m_prof);
    if (strcmp(m_prof, prof_sha))
        return refuse("manifest", "PROFILE_DIGEST", "manifest profile_sha256 %.16s differs from profile %.16s", m_prof, prof_sha);
    if (!g_dry && strcmp(status, "frozen"))
        return refuse("freeze order", "NOT_FROZEN", "candidate manifest status is '%s', not 'frozen'", status);
    if (!g_dry && !frozen_at[0]) return refuse("freeze order", "NOT_FROZEN", "candidate manifest has no frozen_at");
    if (!g_dry) { /* the independent scorer source in --repo is the frozen one (it may not be revised after C_f) */
        char m_ind[80] = "", got[128] = "", scr[PATH_MAX];
        jtop(man, "independent_scorer_source_sha256", m_ind, sizeof m_ind);
        snprintf(scr, sizeof scr, "%s/calibration/scripts/indep_source_digest.sh", repo);
        char *ai[] = {"sh", scr, NULL};
        if (run_capture(ai, got, sizeof got) != 0 || strlen(m_ind) != 64 || strcmp(got, m_ind))
            return refuse("independent scorer", "INDEP_SOURCE", "tools/turing_verify_indep digest %.16s is not the frozen %.16s", got,
                          m_ind);
    }
    char self_hex[65] = "", m_eval[65] = "";
    int n_bg = 0;
    {
        int sect = 0; /* 1 = shared_background, 2 = runtime */
        for (char *l = man; l && *l;) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            if (strstr(l, "\"shared_background_sha256\": {")) sect = 1;
            else if (strstr(l, "\"runtime_sha256\": {")) sect = 2;
            else if (sect && strchr(l, '}') && !strchr(l, ':')) sect = 0;
            else if (strstr(l, "\"name\":") && strstr(l, "\"model_digest\":")) {
                if (nc == MAXC) return refuse("manifest", "FORMAT", "too many candidates");
                cand_t *c = &C[nc];
                if (jstr(l, "name", c->name, sizeof c->name) || jstr(l, "file", c->file, sizeof c->file) ||
                    jstr(l, "file_sha256", c->file_sha, sizeof c->file_sha) ||
                    jstr(l, "model_digest", c->model_digest, sizeof c->model_digest) || jint(l, "lm_bits", &c->lm_bits))
                    return refuse("manifest", "FORMAT", "bad candidate line");
                if (only) {
                    char lst[512];
                    snprintf(lst, sizeof lst, ",%s,", only);
                    char pat[80];
                    snprintf(pat, sizeof pat, ",%s,", c->name);
                    if (!strstr(lst, pat)) goto next;
                }
                ++nc;
            } else if (sect == 1) {
                char k[PATH_MAX], v[80], *q1 = strchr(l, '"');
                if (q1 && sscanf(q1, "\"%[^\"]\": \"%64[0-9a-f]\"", k, v) == 2) {
                    char p[PATH_MAX], h[65];
                    snprintf(p, sizeof p, "%s/%s", repo, k);
                    if (sha_file_hex(p, h) || strcmp(h, v))
                        return refuse("shared background", "DIGEST", "%s does not match the candidate manifest", k);
                    ++n_bg;
                }
            } else if (sect == 2) {
                jstr(l, "turing-cal-eval", m_eval, sizeof m_eval);
            }
        next:
            l = e ? e + 1 : NULL;
        }
    }
    free(man);
    if (n_bg == 0) return refuse("manifest", "FORMAT", "no shared_background_sha256 entries");
    if (self_sha(self_hex)) return refuse("runtime", "IO", "cannot hash own binary");
    if (!g_dry && strcmp(self_hex, m_eval))
        return refuse("runtime", "RUNTIME_DIGEST", "turing-cal-eval sha256 %.16s is not the frozen one", self_hex);
    if (cand_index(BASELINE) < 0) return refuse("manifest", "FORMAT", "baseline %s missing", BASELINE);
    if (!g_dry && (cand_index(CANDIDATE) < 0 || cand_index(MEM1) < 0 || cand_index(MEM2) < 0))
        return refuse("manifest", "FORMAT", "candidate or memorizer missing");
    for (int i = 0; i < nc; ++i) {
        cand_t *c = &C[i];
        int found = 0;
        for (int d = 0; d < ncd && !found; ++d) {
            char cp_[sizeof c->path];
            snprintf(cp_, sizeof cp_, "%s/%s", cdirs[d], c->file);
            memcpy(c->path, cp_, sizeof cp_);
            found = access(c->path, R_OK) == 0;
        }
        if (!found) return refuse("candidate", "IO", "%s not found in any --cand-dir", c->file);
        char h[65];
        if (sha_file_hex(c->path, h) || strcmp(h, c->file_sha))
            return refuse("candidate", "DIGEST", "%s file sha256 differs from the manifest", c->name);
    }

    /* --- dataset manifest --- */
    char ds_sha[65];
    char *ds = slurp(dataset, NULL);
    if (!ds || sha_file_hex(dataset, ds_sha)) return refuse("dataset", "IO", "cannot read %s", dataset);
    char split[32] = "", d_prof[65] = "", d_commit[64] = "", released[40] = "", payload[65] = "", dsid[160] = "";
    char sc_sha[80] = "", sm_sha[80] = "";
    jtop(ds, "split", split, sizeof split);
    jtop(ds, "profile_digest", d_prof, sizeof d_prof);
    jtop(ds, "freeze_commit", d_commit, sizeof d_commit);
    jtop(ds, "released_utc", released, sizeof released);
    jtop(ds, "payload_digest", payload, sizeof payload);
    jtop(ds, "dataset_id", dsid, sizeof dsid);
    jtop(ds, "seed_commitment_sha256", sc_sha, sizeof sc_sha);
    jtop(ds, "sealed_manifest_sha256", sm_sha, sizeof sm_sha);
    if (strcmp(d_prof, prof_sha)) return refuse("dataset", "PROFILE_DIGEST", "dataset manifest profile_digest differs");
    if (g_dry && strcmp(split, "development")) return refuse("dataset", "SPLIT", "a dry run scores development data only");
    if (!g_dry && strcmp(split, "sealed_test")) return refuse("dataset", "SPLIT", "split '%s' is not sealed_test", split);
    {
        sha256_ctx pc;
        sha256_init(&pc);
        for (char *l = ds; l && *l;) {
            char *e = strchr(l, '\n');
            if (e) *e = 0;
            if (strstr(l, "\"group\":") && strstr(l, "\"sha256\":")) {
                if (nf == MAXF) return refuse("dataset", "FORMAT", "too many files");
                dfile_t *f = &F[nf];
                int64_t g, j;
                if (jint(l, "group", &g) || jint(l, "index", &j) || jint(l, "seed", &f->seed) ||
                    jstr(l, "sha256", f->sha, sizeof f->sha) || jint(l, "bytes", &f->bytes) ||
                    jstr(l, "path", f->path, sizeof f->path) || (g != 1 && g != 2))
                    return refuse("dataset", "FORMAT", "bad file line");
                f->g = (int)g, f->j = (int)j;
                char pl[200];
                snprintf(pl, sizeof pl, "%d %d %" PRId64 " %s %" PRId64 "\n", f->g, f->j, f->seed, f->sha, f->bytes);
                sha256_update(&pc, (const uint8_t *)pl, strlen(pl));
                ++nf;
            }
            l = e ? e + 1 : NULL;
        }
        uint8_t d[32];
        char h[65];
        sha256_final(&pc, d);
        hexs(d, h);
        if (strcmp(h, payload)) return refuse("dataset", "DATASET_DIGEST", "payload_digest does not match the file list");
    }
    free(ds);
    for (int i = 0; i < nf; ++i) /* group then index order, and both groups present */
        if (i && (F[i].g < F[i - 1].g || (F[i].g == F[i - 1].g && F[i].j != F[i - 1].j + 1)))
            return refuse("dataset", "FORMAT", "files not in group/index order");
    if (!nf || F[0].g != 1 || F[nf - 1].g != 2) return refuse("dataset", "FORMAT", "need group 1 and group 2");

    /* --- freeze order (two-step freeze, BLINDING_PROTOCOL.md section 2) ---
     * C_f = the dataset manifest's freeze_commit: the commit that holds the frozen candidate manifest, profile and
     * sidecar. C_f names no commit of its own; the sealed seeds are derived from it. */
    char ov_result[16] = "", ov_commit[64] = "", origin_main[64] = "NONE";
    if (!g_dry) {
        if (!overlap) return refuse("freeze order", "OVERLAP", "--overlap overlap_audit.json is required");
        char *ov = slurp(overlap, NULL);
        if (!ov) return refuse("freeze order", "OVERLAP", "cannot read %s", overlap);
        jtop(ov, "result", ov_result, sizeof ov_result);
        jtop(ov, "freeze_commit", ov_commit, sizeof ov_commit);
        free(ov);
        if (strcmp(ov_result, "PASS")) return s2_fail("freeze order", "OVERLAP", "overlap audit result is '%s'", ov_result);
        if (strlen(d_commit) != 40 || strspn(d_commit, "0123456789abcdef") != 40)
            return refuse("freeze order", "FREEZE_COMMIT", "dataset freeze_commit '%s' is not a 40-hex commit", d_commit);
        if (strcmp(ov_commit, d_commit))
            return refuse("freeze order", "FREEZE_COMMIT", "overlap audit commit differs from the sealed freeze_commit");
        char spec[128], out[256];
        char *a1[] = {"git", "-C", (char *)repo, "rev-parse", "--verify", "origin/main^{commit}", NULL};
        if (run_capture(a1, origin_main, sizeof origin_main) != 0 || strlen(origin_main) != 40)
            return refuse("freeze order", "FREEZE_COMMIT", "origin/main is not available in --repo (fetch first)");
        char *a2[] = {"git", "-C", (char *)repo, "merge-base", "--is-ancestor", d_commit, "origin/main", NULL};
        if (run_capture(a2, out, sizeof out) != 0)
            return refuse("freeze order", "FREEZE_COMMIT", "freeze commit %.12s is not an ancestor of origin/main %.12s", d_commit,
                          origin_main);
        char h[65];
        snprintf(spec, sizeof spec, "%s:calibration/experiments/EXP-001/candidate_manifest.json", d_commit);
        char *a3[] = {"git", "-C", (char *)repo, "show", spec, NULL};
        if (run_sha(a3, h) != 0 || strcmp(h, man_sha))
            return s2_fail("freeze order", "FREEZE_COMMIT", "candidate manifest differs from the one at freeze commit %.12s", d_commit);
        snprintf(spec, sizeof spec, "%s:calibration/profiles/Turing-profile-v1.0.toml", d_commit);
        char *a4[] = {"git", "-C", (char *)repo, "show", spec, NULL};
        if (run_sha(a4, h) != 0 || strcmp(h, prof_sha))
            return s2_fail("freeze order", "FREEZE_COMMIT", "profile differs from the one at freeze commit %.12s", d_commit);
        char *a5[] = {"git", "-C", (char *)repo, "show", "-s", "--format=%ct", d_commit, NULL};
        long long ct = -1;
        if (run_capture(a5, out, sizeof out) != 0 || sscanf(out, "%lld", &ct) != 1)
            return refuse("freeze order", "FREEZE_COMMIT", "cannot read the commit time of %.12s", d_commit);
        int64_t rel = utc_epoch(released);
        if (rel < 0) return refuse("freeze order", "FREEZE_AFTER_RELEASE", "no data release time");
        if (ct >= rel)
            return s2_fail("freeze order", "FREEZE_AFTER_RELEASE", "freeze commit time %lld is not before the data release %s", ct,
                          released);
        for (int i = 0; i < nf; ++i)
            if (F[i].seed != sealed_seed(d_commit, prof_sha, F[i].g, F[i].j))
                return s2_fail("freeze order", "SEED_DERIVATION", "g%d/%d seed %" PRId64 " is not derived from freeze commit %.12s",
                              F[i].g, F[i].j, F[i].seed, d_commit);
    }
    /* Both modes: a freeze receipt newer than the data release is refused when both times exist. */
    if (frozen_at[0] && released[0] && released[0] >= '0' && released[0] <= '9' && strcmp(frozen_at, released) >= 0)
        return s2_fail("freeze order", "FREEZE_AFTER_RELEASE", "candidate frozen_at %s is not before data release %s", frozen_at,
                      released);
    if (!g_dry && (!released[0] || released[0] < '0' || released[0] > '9'))
        return refuse("freeze order", "FREEZE_AFTER_RELEASE", "no data release time");

    /* --- load and re-hash every trace --- */
    for (int i = 0; i < nf; ++i) {
        char h[65];
        struct stat st;
        if (sha_file_hex(F[i].path, h) || strcmp(h, F[i].sha) || stat(F[i].path, &st) || st.st_size != F[i].bytes)
            return refuse("dataset", "DATASET_DIGEST", "g%d/%d %s differs from the dataset manifest", F[i].g, F[i].j,
                          F[i].path);
        tc_unhex(h, F[i].dd);
        ty_stream_init(&F[i].s);
        if (ty_ctr1_read(F[i].path, &F[i].s, why, sizeof why) < 0) return refuse("dataset", "FORMAT", "%s", why);
    }

    /* --- bundle dirs --- */
    char wd[PATH_MAX];
    const char *sub[] = {"probability_streams", "arithmetic", "ans", "encoded_artifacts", "decoder_receipts"};
    for (int i = 0; i < 5; ++i) {
        snprintf(wd, sizeof wd, "%s/%s", g_out, sub[i]);
        if (mkdir_p(wd)) return refuse("out", "IO", "mkdir %s", wd);
    }
    const char *wsub[] = {"probability_streams", "symbols", "arithmetic", "ans"};
    for (int i = 0; i < 4; ++i) {
        snprintf(wd, sizeof wd, "%s/%s", work, wsub[i]);
        if (mkdir_p(wd)) return refuse("work", "IO", "mkdir %s", wd);
    }
    FILE *ps_idx = open_out("probability_streams/INDEX"), *enc_idx = open_out("encoded_artifacts/INDEX");
    FILE *ar_res = open_out("arithmetic/results.json"), *an_res = open_out("ans/results.json");
    if (!ps_idx || !enc_idx || !ar_res || !an_res) return refuse("out", "IO", "bundle files");
    fprintf(ar_res, "{\n  \"schema\": \"turing.cal.coder_results.v1\",\n  \"coder\": \"A TCR1 v1 range\",\n  \"results\": [\n");
    fprintf(an_res, "{\n  \"schema\": \"turing.cal.coder_results.v1\",\n  \"coder\": \"B TCA1 v1 rANS L=2^23\",\n  \"results\": [\n");
    fprintf(ps_idx, "# <TPS1 file sha256> <TPS1 trailer digest> <TSY1 trailer digest> <g> <index> <candidate> <bytes> <path>\n");
    fprintf(enc_idx, "# <coded file sha256> <coder> <g> <index> <candidate> <bytes> <path>\n");
    int first_res = 1;

    int S1 = 1, S3 = 1;
    char tsy_digest[MAXF][65];
    memset(tsy_digest, 0, sizeof tsy_digest);

    /* --- every candidate model is parsed and checked before the first score (one at a time, freed) --- */
    for (int ci = 0; ci < nc; ++ci) {
        cand_t *c = &C[ci];
        ty_model m;
        uint8_t md[32];
        uint64_t lm;
        int rc = tc_load_model(c->path, &m, md, &lm, why, sizeof why);
        if (rc != TC_OK) return refuse("candidate", tc_err_name(rc), "%s: %s", c->name, why);
        char mh[65];
        hexs(md, mh);
        ty_model_free(&m);
        if (strcmp(mh, c->model_digest)) return refuse("candidate", "MODEL_DIGEST", "%s model digest differs", c->name);
        if ((int64_t)lm != c->lm_bits) return refuse("candidate", "LM_BITS", "%s L(M) %" PRIu64 " differs", c->name, lm);
    }
    /* From here on a score exists: the verdict is final and nothing is VOID any more. */
    g_scored = 1;

    /* --- per candidate, per file --- */
    for (int ci = 0; ci < nc; ++ci) {
        cand_t *c = &C[ci];
        ty_model m;
        uint8_t md[32];
        uint64_t lm;
        int rc = tc_load_model(c->path, &m, md, &lm, why, sizeof why);
        if (rc != TC_OK) return refuse("candidate", tc_err_name(rc), "%s: %s", c->name, why);
        char mh[65];
        hexs(md, mh);
        if (strcmp(mh, c->model_digest)) return refuse("candidate", "MODEL_DIGEST", "%s model digest differs", c->name);
        if ((int64_t)lm != c->lm_bits) return refuse("candidate", "LM_BITS", "%s L(M) %" PRIu64 " differs", c->name, lm);
        fprintf(stderr, "eval %s (L(M) = %" PRIu64 " bits)\n", c->name, lm);
        for (int fi = 0; fi < nf; ++fi) {
            dfile_t *f = &F[fi];
            res_t *r = &R[ci][fi];
            tc_pstream p0, p;
            tc_symbols s0, s;
            if ((rc = tc_produce(&m, &f->s, pd, md, f->dd, &p0, &s0, why, sizeof why)) != TC_OK)
                return refuse_c("S3", "produce", tc_err_name(rc), "%s g%d/%d: %s", c->name, f->g, f->j, why);
            uint8_t *pb, *sb;
            size_t pn, sn;
            if (tc_ps_serialize(&p0, &pb, &pn, why, sizeof why) != TC_OK || tc_sy_serialize(&s0, &sb, &sn, why, sizeof why) != TC_OK)
                return refuse_c("S3", "serialize", "FORMAT", "%s", why);
            tc_ps_free(&p0), tc_sy_free(&s0);
            char psp[PATH_MAX], syp[PATH_MAX];
            snprintf(psp, sizeof psp, "%s/probability_streams/g%d_j%d_%s.tps", work, f->g, f->j, c->name);
            snprintf(syp, sizeof syp, "%s/symbols/g%d_j%d_%s.tsy", work, f->g, f->j, c->name);
            if (tc_write_file(psp, pb, pn) != TC_OK || tc_write_file(syp, sb, sn) != TC_OK)
                return refuse("write", "IO", "%s", psp);
            free(pb), free(sb);
            /* The coders consume the stream as re-read from disk. */
            if (tc_read_file(psp, &pb, &pn) != TC_OK || tc_read_file(syp, &sb, &sn) != TC_OK) return refuse("read", "IO", "%s", psp);
            if ((rc = tc_ps_parse(pb, pn, &p, why, sizeof why)) != TC_OK || (rc = tc_sy_parse(sb, sn, &s, why, sizeof why)) != TC_OK)
                return refuse_c("S3", "parse", tc_err_name(rc), "%s", why);
            if ((rc = tc_ps_expect(&p, pd, md, f->dd, why, sizeof why)) != TC_OK || (rc = tc_pair_check(&p, &s, why, sizeof why)) != TC_OK)
                return refuse_c("S3", "stream binding", tc_err_name(rc), "%s", why);
            free(pb), free(sb);
            char fsha[65];
            sha_file_hex(psp, fsha);
            hexs(p.digest, r->tps);
            hexs(s.digest, r->tsy);
            /* S3: one symbol stream per sealed file, identical for every candidate. */
            if (!tsy_digest[fi][0]) memcpy(tsy_digest[fi], r->tsy, 65);
            else if (strcmp(tsy_digest[fi], r->tsy)) S3 = 0;
            fprintf(ps_idx, "%s %s %s %d %d %s %zu %s\n", fsha, r->tps, r->tsy, f->g, f->j, c->name, pn, psp);
            /* Crumb list: identical for every candidate (it comes from the data). */
            size_t k = 0;
            for (uint64_t lo = 0; lo < p.n;) {
                uint64_t hi = lo + 1;
                while (hi < p.n && p.crumb[hi] == p.crumb[lo]) ++hi;
                ++k;
                lo = hi;
            }
            if (!CR[fi]) {
                CR[fi] = calloc(k ? k : 1, sizeof(crumb_t));
                ncr[fi] = k;
                size_t q = 0;
                for (uint64_t lo = 0; lo < p.n;) {
                    uint64_t hi = lo + 1;
                    while (hi < p.n && p.crumb[hi] == p.crumb[lo]) ++hi;
                    CR[fi][q].ord = p.crumb[lo], CR[fi][q].lo = lo, CR[fi][q].hi = hi, ++q;
                    lo = hi;
                }
            } else {
                if (k != ncr[fi]) S3 = 0;
                for (size_t q = 0; q < k && q < ncr[fi]; ++q)
                    if (p.crumb[CR[fi][q].lo] != CR[fi][q].ord) S3 = 0;
            }
            if (!S3) return refuse_c("S3", "S3", "SYMBOL_STREAM", "g%d/%d: candidates see different symbol streams", f->g, f->j);
            tc_ideal_ub(&p, &s, 0, p.n, &r->ideal_ub);
            /* Coder A and coder B: same parsed TPS1, written, re-read, decoded. */
            const char *cname[2] = {"range", "rans"};
            const char *cext[2] = {"tcr", "tca"};
            uint64_t *nbytes[2] = {&r->nrange, &r->nrans};
            char *csha[2] = {r->range_sha, r->rans_sha};
            int ok_c[2] = {0, 0};
            char dec_digest[2][65] = {"", ""};
            for (int x = 0; x < 2; ++x) {
                uint8_t *cb;
                size_t cn;
                rc = x == 0 ? tc_range_encode(&p, &s, &cb, &cn, why, sizeof why) : tc_rans_encode(&p, &s, &cb, &cn, why, sizeof why);
                if (rc != TC_OK) return refuse_c("S1", "encode", tc_err_name(rc), "%s %s: %s", cname[x], c->name, why);
                if (cn < TC_CODED_HEADER || memcmp(cb + 16, p.digest, 32))
                    return refuse_c("S3", "encode", "BINDING", "%s header is not bound to the TPS1 digest", cname[x]);
                char cp[PATH_MAX];
                snprintf(cp, sizeof cp, "%s/%s/g%d_j%d_%s.%s", work, x ? "ans" : "arithmetic", f->g, f->j, c->name, cext[x]);
                if (tc_write_file(cp, cb, cn) != TC_OK) return refuse("write", "IO", "%s", cp);
                free(cb);
                if (tc_read_file(cp, &cb, &cn) != TC_OK) return refuse("read", "IO", "%s", cp);
                *nbytes[x] = cn;
                sha_file_hex(cp, csha[x]);
                tc_symbols dsy;
                rc = x == 0 ? tc_range_decode(&p, cb, cn, &dsy, why, sizeof why) : tc_rans_decode(&p, cb, cn, &dsy, why, sizeof why);
                if (rc == TC_OK) {
                    ok_c[x] = dsy.n == s.n && !memcmp(dsy.sym, s.sym, s.n);
                    uint8_t *db;
                    size_t dn;
                    memcpy(dsy.dataset, s.dataset, 32);
                    if (tc_sy_serialize(&dsy, &db, &dn, why, sizeof why) == TC_OK) hexs(dsy.digest, dec_digest[x]), free(db);
                    tc_sy_free(&dsy);
                }
                if (!ok_c[x]) S1 = 0;
                free(cb);
                fprintf(enc_idx, "%s %s %d %d %s %zu %s\n", csha[x], cname[x], f->g, f->j, c->name, cn, cp);
                FILE *cr = x ? an_res : ar_res;
                fprintf(cr,
                        "%s    {\"g\": %d, \"index\": %d, \"candidate\": \"%s\", \"tps1_digest\": \"%s\", \"symbols_digest\": \"%s\", \"source_dataset_digest\": \"%s\", "
                        "\"count\": %" PRIu64 ", \"coded_bytes\": %zu, \"header_bits\": 448, \"payload_bits\": %zu, "
                        "\"side_information_bits\": 0, \"coded_bits\": %zu, \"encoded_sha256\": \"%s\", \"decoded_digest\": \"%s\", "
                        "\"lossless_roundtrip\": %s}",
                        first_res ? "" : ",\n", f->g, f->j, c->name, r->tps, r->tsy, f->sha, s.n, cn, (cn - TC_CODED_HEADER) * 8, cn * 8,
                        csha[x], dec_digest[x][0] ? dec_digest[x] : "NONE", ok_c[x] ? "true" : "false");
            }
            first_res = 0;
            r->lossless = ok_c[0] && ok_c[1];
            /* Decoder receipt. */
            {
                char rel[PATH_MAX];
                snprintf(rel, sizeof rel, "decoder_receipts/g%d_j%d_%s.json", f->g, f->j, c->name);
                FILE *dr = open_out(rel);
                if (!dr) return refuse("out", "IO", "%s", rel);
                fprintf(dr,
                        "{\n  \"schema\": \"turing.cal.decoder_receipt.v1\",\n  \"g\": %d,\n  \"index\": %d,\n  \"candidate\": \"%s\",\n"
                        "  \"profile_digest\": \"%s\",\n  \"model_digest\": \"%s\",\n  \"dataset_digest\": \"%s\",\n"
                        "  \"tps1_digest\": \"%s\",\n  \"symbols_digest\": \"%s\",\n  \"count\": %" PRIu64 ",\n"
                        "  \"range\": {\"coded_sha256\": \"%s\", \"coded_bytes\": %" PRIu64 ", \"decoded_digest\": \"%s\", \"lossless\": %s},\n"
                        "  \"rans\": {\"coded_sha256\": \"%s\", \"coded_bytes\": %" PRIu64 ", \"decoded_digest\": \"%s\", \"lossless\": %s},\n"
                        "  \"same_tps1_for_both_coders\": true\n}\n",
                        f->g, f->j, c->name, prof_sha, mh, f->sha, r->tps, r->tsy, s.n, r->range_sha, r->nrange,
                        dec_digest[0][0] ? dec_digest[0] : "NONE", ok_c[0] ? "true" : "false", r->rans_sha, r->nrans,
                        dec_digest[1][0] ? dec_digest[1] : "NONE", ok_c[1] ? "true" : "false");
                fclose(dr);
            }
            /* Per crumb: ideal, both coders on the crumb alone, round trip, envelope. */
            size_t K = ncr[fi];
            r->crumb_ub = calloc(K, sizeof(int64_t));
            r->crumb_range = calloc(K, sizeof(uint64_t));
            r->crumb_rans = calloc(K, sizeof(uint64_t));
            uint8_t *tmp = malloc(s.n ? s.n : 1);
            r->worst_crumb_margin[0] = r->worst_crumb_margin[1] = INT64_MAX;
            r->crumb_sum_ub = 0;
            for (size_t q = 0; q < K; ++q) {
                uint64_t lo = CR[fi][q].lo, hi = CR[fi][q].hi, n = hi - lo;
                tc_ideal_ub(&p, &s, lo, hi, &r->crumb_ub[q]);
                r->crumb_sum_ub += r->crumb_ub[q];
                uint8_t *x, *y;
                size_t nx, ny;
                if (tc_range_encode_raw(p.q + lo * p.K, p.K, s.sym + lo, n, &x, &nx, NULL) != TC_OK ||
                    tc_rans_encode_raw(p.q + lo * p.K, p.K, s.sym + lo, n, &y, &ny) != TC_OK)
                    return refuse_c("S1", "crumb encode", "CODER", "%s g%d/%d crumb %u", c->name, f->g, f->j, CR[fi][q].ord);
                if (tc_range_decode_raw(p.q + lo * p.K, p.K, x, nx, n, tmp, why, sizeof why) != TC_OK || memcmp(tmp, s.sym + lo, n) ||
                    tc_rans_decode_raw(p.q + lo * p.K, p.K, y, ny, n, tmp, why, sizeof why) != TC_OK || memcmp(tmp, s.sym + lo, n))
                    S1 = 0, r->lossless = 0;
                r->crumb_range[q] = nx + TC_CODED_HEADER;
                r->crumb_rans[q] = ny + TC_CODED_HEADER;
                free(x), free(y);
                for (int z = 0; z < 2; ++z) {
                    uint64_t cb = z ? r->crumb_rans[q] : r->crumb_range[q];
                    int64_t ovh = (int64_t)cb * 8 * UB - r->crumb_ub[q];
                    int64_t margin = env_band(n) - iabs64(ovh - ENV_CENTER);
                    if (margin < 0) r->env_crumb_fail[z]++;
                    if (margin < r->worst_crumb_margin[z]) r->worst_crumb_margin[z] = margin;
                }
            }
            free(tmp);
            if (r->crumb_sum_ub != r->ideal_ub)
                return refuse_c("S4", "per-crumb sum", "CRUMB_SUM", "%s g%d/%d: crumb sum %" PRId64 " != file %" PRId64, c->name, f->g, f->j,
                              r->crumb_sum_ub, r->ideal_ub);
            for (int z = 0; z < 2; ++z) {
                uint64_t cb = z ? r->nrans : r->nrange;
                int64_t ovh = (int64_t)cb * 8 * UB - r->ideal_ub;
                if (iabs64(ovh - ENV_CENTER) > env_band(s.n)) r->env_file_fail[z] = 1;
            }
            tc_ps_free(&p), tc_sy_free(&s);
        }
        ty_model_free(&m);
    }
    fprintf(ar_res, "\n  ]\n}\n");
    fprintf(an_res, "\n  ]\n}\n");
    fclose(ar_res), fclose(an_res), fclose(ps_idx), fclose(enc_idx);

    /* --- group aggregates and bootstrap --- */
    int b2 = cand_index(BASELINE);
    int64_t lm[MAXC], ideal[3][MAXC], coded[3][MAXC][2], events[3] = {0}, band_sum[3] = {0};
    size_t pool_n[3] = {0};
    iv_t Tv[3][MAXC][3];   /* [g][cand][ideal, range, rans] vs B2 */
    iv_t Tb[3][MAXC][MAXC]; /* [g][cand][baseline] ideal */
    int pgt0[3][MAXC] = {{0}}; /* bootstrap replicates with T_ideal vs B2 > 0 (reported only) */
    int64_t *Sb[3][MAXC];
    memset(ideal, 0, sizeof ideal), memset(coded, 0, sizeof coded);
    for (int ci = 0; ci < nc; ++ci) lm[ci] = C[ci].lm_bits;
    for (int g = 1; g <= 2; ++g) {
        for (int fi = 0; fi < nf; ++fi)
            if (F[fi].g == g) {
                pool_n[g] += ncr[fi];
                events[g] += (int64_t)F[fi].s.n;
                band_sum[g] += env_band(F[fi].s.n);
                for (int ci = 0; ci < nc; ++ci) {
                    ideal[g][ci] += R[ci][fi].ideal_ub;
                    coded[g][ci][0] += (int64_t)R[ci][fi].nrange * 8;
                    coded[g][ci][1] += (int64_t)R[ci][fi].nrans * 8;
                }
            }
        size_t Cn = pool_n[g];
        if (Cn == 0) return refuse_c(g == 2 ? "S9" : "S6", "bootstrap", "NO_CRUMBS", "group %d has no crumbs", g);
        /* Pool: (seed order, crumb ordinal); per candidate crumb ideal. */
        int64_t *pool[MAXC];
        for (int ci = 0; ci < nc; ++ci) {
            pool[ci] = malloc(Cn * sizeof(int64_t));
            size_t q = 0;
            for (int fi = 0; fi < nf; ++fi)
                if (F[fi].g == g)
                    for (size_t z = 0; z < ncr[fi]; ++z) pool[ci][q++] = R[ci][fi].crumb_ub[z];
            Sb[g][ci] = malloc(BOOT_B * sizeof(int64_t));
            memset(Sb[g][ci], 0, BOOT_B * sizeof(int64_t));
        }
        uint64_t st = BOOT_SEED + (uint64_t)g;
        uint64_t lim = UINT64_MAX - ((UINT64_MAX % Cn) + 1) % Cn;
        for (int b = 0; b < BOOT_B; ++b)
            for (size_t k = 0; k < Cn; ++k) {
                uint64_t rr;
                do rr = splitmix64(&st);
                while (rr > lim);
                size_t idx = (size_t)(rr % Cn);
                for (int ci = 0; ci < nc; ++ci) Sb[g][ci][b] += pool[ci][idx];
            }
        int64_t *tb = malloc(BOOT_B * sizeof(int64_t));
        for (int ci = 0; ci < nc; ++ci) {
            for (int bi = 0; bi < nc; ++bi) {
                int64_t shift = (lm[ci] - lm[bi]) * UB;
                for (int b = 0; b < BOOT_B; ++b) tb[b] = Sb[g][bi][b] - Sb[g][ci][b] - shift;
                if (bi == b2)
                    for (int b = 0; b < BOOT_B; ++b) pgt0[g][ci] += tb[b] > 0;
                Tb[g][ci][bi] = interval_of(tb, ideal[g][bi] - ideal[g][ci] - shift);
            }
            Tv[g][ci][0] = Tb[g][ci][b2];
            for (int x = 0; x < 2; ++x) {
                int64_t o_m = coded[g][ci][x] * UB - ideal[g][ci], o_b = coded[g][b2][x] * UB - ideal[g][b2];
                int64_t sh = o_m - o_b;
                iv_t v = {Tv[g][ci][0].point - sh, Tv[g][ci][0].lo - sh, Tv[g][ci][0].hi - sh};
                Tv[g][ci][1 + x] = v;
            }
        }
        free(tb);
        for (int ci = 0; ci < nc; ++ci) free(pool[ci]), free(Sb[g][ci]);
    }

    /* --- S4 envelope --- */
    int S4 = 1;
    for (int ci = 0; ci < nc; ++ci)
        for (int fi = 0; fi < nf; ++fi)
            for (int z = 0; z < 2; ++z)
                if (R[ci][fi].env_file_fail[z] || R[ci][fi].env_crumb_fail[z]) S4 = 0;

    /* --- S5 reversals (prereg section 5, FAILURE_REPORTING section 5) --- */
    int S5 = 1, n_rev = 0, n_rev_expl = 0;
    static char revlog[65536]; revlog[0] = 0; /* up to 84 DL + 36 T-sign entries, ~200 bytes each */
    size_t rl = 0;
    for (int g = 1; g <= 2; ++g)
        for (int x = 0; x < 2; ++x)
            for (int a = 0; a < nc; ++a)
                for (int b = a + 1; b < nc; ++b) {
                    int64_t di = (lm[a] * UB + ideal[g][a]) - (lm[b] * UB + ideal[g][b]);
                    int64_t dc = (lm[a] - lm[b]) * UB + (coded[g][a][x] - coded[g][b][x]) * UB;
                    int si = (di > 0) - (di < 0), sc = (dc > 0) - (dc < 0);
                    if (si == sc) continue;
                    ++n_rev;
                    int expl = iabs64(di) <= 2 * band_sum[g];
                    n_rev_expl += expl;
                    if (!expl) S5 = 0;
                    if (rl < sizeof revlog - 200)
                        rl += (size_t)snprintf(revlog + rl, sizeof revlog - rl,
                                               "    {\"g\": %d, \"coder\": \"%s\", \"a\": \"%s\", \"b\": \"%s\", \"dl_diff_ideal_ub\": %" PRId64
                                               ", \"dl_diff_coded_ub\": %" PRId64 ", \"explained\": %s},\n",
                                               g, x ? "rans" : "range", C[a].name, C[b].name, di, dc, expl ? "true" : "false");
                }
    /* Sign reversals of T: ideal vs coded (same group); group 1 vs group 2 (same measure). Material when the two
     * intervals exclude each other's point estimates. Ideal vs coded is explained when |o(M)-o(B2)| is within
     * the two groups' summed envelope band. */
    for (int ci = 0; ci < nc; ++ci) {
        if (ci == b2) continue;
        for (int g = 1; g <= 2; ++g)
            for (int x = 1; x <= 2; ++x) {
                iv_t u = Tv[g][ci][0], v = Tv[g][ci][x];
                int su = (u.point > 0) - (u.point < 0), sv = (v.point > 0) - (v.point < 0);
                int excl = (u.point < v.lo || u.point > v.hi) && (v.point < u.lo || v.point > u.hi);
                if (su == sv || !excl) continue;
                ++n_rev;
                int expl = iabs64(u.point - v.point) <= 2 * band_sum[g];
                n_rev_expl += expl;
                if (!expl) S5 = 0;
                if (rl < sizeof revlog - 200)
                    rl += (size_t)snprintf(revlog + rl, sizeof revlog - rl,
                                           "    {\"g\": %d, \"kind\": \"T sign ideal vs %s\", \"candidate\": \"%s\", \"explained\": %s},\n", g,
                                           x == 1 ? "range" : "rans", C[ci].name, expl ? "true" : "false");
            }
        for (int x = 0; x < 3; ++x) {
            iv_t u = Tv[1][ci][x], v = Tv[2][ci][x];
            int su = (u.point > 0) - (u.point < 0), sv = (v.point > 0) - (v.point < 0);
            int excl = (u.point < v.lo || u.point > v.hi) && (v.point < u.lo || v.point > u.hi);
            if (su == sv || !excl) continue;
            ++n_rev, S5 = 0;
            if (rl < sizeof revlog - 200)
                rl += (size_t)snprintf(revlog + rl, sizeof revlog - rl,
                                       "    {\"kind\": \"T sign group 1 vs group 2\", \"measure\": \"%s\", \"candidate\": \"%s\", \"explained\": false},\n",
                                       x == 0 ? "ideal" : x == 1 ? "range" : "rans", C[ci].name);
        }
    }
    if (rl >= 2 && revlog[rl - 2] == ',') revlog[rl - 2] = '\n', revlog[rl - 1] = 0;
    /* Spearman rho of the DL ordering, ideal vs each coder (reported only). */
    double rho[3][2];
    for (int g = 1; g <= 2; ++g)
        for (int x = 0; x < 2; ++x) {
            double d2 = 0;
            for (int a = 0; a < nc; ++a) {
                int ra = 0, rb = 0;
                for (int b = 0; b < nc; ++b) {
                    if (lm[b] * UB + ideal[g][b] < lm[a] * UB + ideal[g][a]) ++ra;
                    if (lm[b] + coded[g][b][x] < lm[a] + coded[g][a][x]) ++rb;
                }
                d2 += (double)(ra - rb) * (ra - rb);
            }
            rho[g][x] = nc > 1 ? 1.0 - 6.0 * d2 / ((double)nc * ((double)nc * nc - 1)) : 1.0;
        }

    /* --- S6, S7, S9 (three-way: PASS, FAIL = interval entirely on the wrong side, INCONCLUSIVE = straddles 0) --- */
    const char *crit[10] = {0};
    int mi = cand_index(CANDIDATE), m1 = cand_index(MEM1), m2 = cand_index(MEM2);
    int s6[3] = {0}, s7[3] = {0}; /* 1 PASS, 0 INCONCLUSIVE, -1 FAIL, -2 not reached */
    for (int g = 1; g <= 2; ++g) {
        s6[g] = mi < 0 ? -2 : 1;
        for (int x = 0; mi >= 0 && x < 3; ++x) {
            iv_t v = Tv[g][mi][x];
            int e = v.lo > 0 ? 1 : v.hi < 0 ? -1 : 0;
            if (e < s6[g]) s6[g] = e;
        }
        s7[g] = (m1 < 0 && m2 < 0) ? -2 : 1;
        int mm[2] = {m1, m2};
        for (int k = 0; k < 2; ++k)
            for (int x = 0; mm[k] >= 0 && x < 3; ++x) {
                iv_t v = Tv[g][mm[k]][x];
                int e = v.hi < 0 ? 1 : v.lo > 0 ? -1 : 0;
                if (e < s7[g]) s7[g] = e;
            }
    }
    static const char *tri[] = {"FAIL", "INCONCLUSIVE", "PASS"};
#define TRI(v) ((v) == -2 ? "NOT_REACHED" : tri[(v) + 1])
    int s9 = (s6[2] == -2 || s7[2] == -2) ? -2 : (s6[2] < s7[2] ? s6[2] : s7[2]);
    crit[1] = S1 ? "PASS" : "FAIL";
    crit[2] = g_dry ? "NOT_CHECKED" : "PASS"; /* non-dry: every freeze-order check above refused on failure */
    crit[3] = S3 ? "PASS" : "FAIL";
    crit[4] = S4 ? "PASS" : "FAIL";
    crit[5] = S5 ? "PASS" : "FAIL";
    crit[6] = TRI(s6[1]);
    crit[7] = TRI(s7[1]);
    crit[9] = TRI(s9);

    /* --- ideal_lengths.json --- */
    FILE *il = open_out("ideal_lengths.json");
    if (!il) return refuse("out", "IO", "ideal_lengths.json");
    fprintf(il, "{\n  \"schema\": \"turing.cal.ideal_lengths.v1\",\n  \"unit\": \"ub (1 bit = 1000000 ub)\",\n  \"files\": [\n");
    for (int fi = 0; fi < nf; ++fi)
        for (int ci = 0; ci < nc; ++ci)
            fprintf(il, "    {\"g\": %d, \"index\": %d, \"seed\": %" PRId64 ", \"candidate\": \"%s\", \"events\": %zu, \"crumbs\": %zu, \"ideal_ub\": %" PRId64
                    ", \"range_bytes\": %" PRIu64 ", \"rans_bytes\": %" PRIu64 "}%s\n",
                    F[fi].g, F[fi].j, F[fi].seed, C[ci].name, F[fi].s.n, ncr[fi], R[ci][fi].ideal_ub, R[ci][fi].nrange,
                    R[ci][fi].nrans, (fi == nf - 1 && ci == nc - 1) ? "" : ",");
    fprintf(il, "  ],\n  \"crumbs\": [\n");
    for (int fi = 0; fi < nf; ++fi)
        for (size_t q = 0; q < ncr[fi]; ++q)
            for (int ci = 0; ci < nc; ++ci)
                fprintf(il, "    {\"g\": %d, \"index\": %d, \"crumb\": %u, \"n\": %" PRIu64 ", \"candidate\": \"%s\", \"ideal_ub\": %" PRId64
                        ", \"range_bytes\": %" PRIu64 ", \"rans_bytes\": %" PRIu64 "}%s\n",
                        F[fi].g, F[fi].j, CR[fi][q].ord, CR[fi][q].hi - CR[fi][q].lo, C[ci].name, R[ci][fi].crumb_ub[q],
                        R[ci][fi].crumb_range[q], R[ci][fi].crumb_rans[q],
                        (fi == nf - 1 && q == ncr[fi] - 1 && ci == nc - 1) ? "" : ",");
    fprintf(il, "  ]\n}\n");
    fclose(il);

    /* --- scorer_primary.json: the values the independent scorer must reproduce exactly --- */
    FILE *sp = open_out("scorer_primary.json");
    if (!sp) return refuse("out", "IO", "scorer_primary.json");
    fprintf(sp, "{\n  \"schema\": \"turing.cal.scorer.v1\",\n  \"scorer\": \"turing-cal-eval (primary)\",\n  \"dry_run\": %s,\n",
            g_dry ? "true" : "false");
    fprintf(sp, "  \"profile_sha256\": \"%s\",\n  \"candidate_manifest_sha256\": \"%s\",\n  \"dataset_manifest_sha256\": \"%s\",\n",
            prof_sha, man_sha, ds_sha);
    fprintf(sp, "  \"values\": [\n");
    int firstv = 1;
#define V(fmt, val, ...)                                                                                              \
    do {                                                                                                                  \
        fprintf(sp, "%s    {\"key\": \"" fmt "\", \"value\": %" PRId64 "}", firstv ? "" : ",\n", __VA_ARGS__, (int64_t)(val)); \
        firstv = 0;                                                                                                       \
    } while (0)
    static const char *mname[3] = {"ideal", "range", "rans"};
    for (int g = 1; g <= 2; ++g) {
        V("g%d.crumbs", pool_n[g], g);
        V("g%d.events", events[g], g);
        for (int ci = 0; ci < nc; ++ci) {
            V("g%d.%s.lm_bits", lm[ci], g, C[ci].name);
            V("g%d.%s.ideal_ub", ideal[g][ci], g, C[ci].name);
            V("g%d.%s.range_bits", coded[g][ci][0], g, C[ci].name);
            V("g%d.%s.rans_bits", coded[g][ci][1], g, C[ci].name);
            for (int x = 0; x < 3; ++x) {
                V("g%d.%s.T_%s_vs_B2.point_ub", Tv[g][ci][x].point, g, C[ci].name, mname[x]);
                V("g%d.%s.T_%s_vs_B2.lo_ub", Tv[g][ci][x].lo, g, C[ci].name, mname[x]);
                V("g%d.%s.T_%s_vs_B2.hi_ub", Tv[g][ci][x].hi, g, C[ci].name, mname[x]);
            }
        }
    }
    for (int fi = 0; fi < nf; ++fi)
        for (int ci = 0; ci < nc; ++ci) {
            V("g%d.f%d.%s.ideal_ub", R[ci][fi].ideal_ub, F[fi].g, F[fi].j, C[ci].name);
            V("g%d.f%d.%s.range_bytes", R[ci][fi].nrange, F[fi].g, F[fi].j, C[ci].name);
            V("g%d.f%d.%s.rans_bytes", R[ci][fi].nrans, F[fi].g, F[fi].j, C[ci].name);
        }
    fprintf(sp, "\n  ],\n  \"criteria\": {\"S1\": \"%s\", \"S2\": \"%s\", \"S3\": \"%s\", \"S4\": \"%s\", \"S5\": \"%s\", \"S6\": \"%s\", "
                "\"S7\": \"%s\", \"S9\": \"%s\"}\n}\n",
            crit[1], crit[2], crit[3], crit[4], crit[5], crit[6], crit[7], crit[9]);
    fclose(sp);

    /* --- uncertainty.json (intervals, envelope detail, sensitivity) --- */
    FILE *un = open_out("uncertainty.json");
    if (!un) return refuse("out", "IO", "uncertainty.json");
    fprintf(un, "{\n  \"schema\": \"turing.cal.uncertainty.v1\",\n  \"method\": \"crumb percentile bootstrap, UNCERTAINTY_PROTOCOL.md\",\n"
                "  \"resamples\": %d,\n  \"lo_index\": %d,\n  \"hi_index\": %d,\n  \"groups\": [\n",
            BOOT_B, BOOT_LO, BOOT_HI);
    for (int g = 1; g <= 2; ++g) {
        fprintf(un, "    {\"g\": %d, \"bootstrap_seed\": \"0x%016" PRIX64 "\", \"crumbs\": %zu, \"events\": %" PRId64 ",\n     \"candidates\": [\n", g,
                (uint64_t)(BOOT_SEED + (uint64_t)g), pool_n[g], events[g]);
        for (int ci = 0; ci < nc; ++ci) {
            iv_t v = Tv[g][ci][0];
            int64_t il_ = v.point - ((v.point - v.lo) * 204 + 50) / 100, ih_ = v.point + ((v.hi - v.point) * 204 + 50) / 100;
            int64_t lmr = (lm[ci] + 7) / 8 * 8, lb2r = (lm[b2] + 7) / 8 * 8;
            int64_t t_round = v.point - ((lmr - lm[ci]) - (lb2r - lm[b2])) * UB;
            int64_t t_double = v.point - (lm[ci] - lm[b2]) * UB;
            int64_t t_data = v.point + (lm[ci] - lm[b2]) * UB;
            fprintf(un, "      {\"candidate\": \"%s\"", C[ci].name);
            for (int x = 0; x < 3; ++x)
                fprintf(un, ", \"T_%s_vs_B2\": [%" PRId64 ", %" PRId64 ", %" PRId64 "]", mname[x], Tv[g][ci][x].point, Tv[g][ci][x].lo,
                        Tv[g][ci][x].hi);
            fprintf(un, ", \"T_ideal_vs_B2_inflated_x2.04\": [%" PRId64 ", %" PRId64 ", %" PRId64 "]", v.point, il_, ih_);
            for (int bi = 0; bi < nc; ++bi)
                if (bi != b2 && C[bi].name[0] == 'B')
                    fprintf(un, ", \"T_ideal_vs_%s\": [%" PRId64 ", %" PRId64 ", %" PRId64 "]", C[bi].name, Tb[g][ci][bi].point,
                            Tb[g][ci][bi].lo, Tb[g][ci][bi].hi);
            fprintf(un, ", \"T_ideal_lm_byte_rounded\": %" PRId64 ", \"T_ideal_lm_doubled\": %" PRId64 ", \"data_only_gain\": %" PRId64,
                    t_round, t_double, t_data);
            fprintf(un, ", \"bootstrap_replicates_T_ideal_vs_B2_gt0\": %d", pgt0[g][ci]);
            int ef[2] = {0, 0}, ec[2] = {0, 0};
            int64_t wm[2] = {INT64_MAX, INT64_MAX};
            for (int fi = 0; fi < nf; ++fi)
                if (F[fi].g == g)
                    for (int z = 0; z < 2; ++z) {
                        ef[z] += R[ci][fi].env_file_fail[z], ec[z] += R[ci][fi].env_crumb_fail[z];
                        if (R[ci][fi].worst_crumb_margin[z] < wm[z]) wm[z] = R[ci][fi].worst_crumb_margin[z];
                    }
            fprintf(un, ", \"envelope\": {\"range_files_out\": %d, \"range_crumbs_out\": %d, \"range_worst_crumb_margin_ub\": %" PRId64
                        ", \"rans_files_out\": %d, \"rans_crumbs_out\": %d, \"rans_worst_crumb_margin_ub\": %" PRId64 "}}%s\n",
                    ef[0], ec[0], wm[0], ef[1], ec[1], wm[1], ci == nc - 1 ? "" : ",");
        }
        fprintf(un, "     ],\n     \"spearman_rho_dl_ideal_vs_range\": %.6f, \"spearman_rho_dl_ideal_vs_rans\": %.6f}%s\n", rho[g][0], rho[g][1],
                g == 2 ? "" : ",");
    }
    fprintf(un, "  ],\n  \"reversals_total\": %d,\n  \"reversals_explained\": %d,\n  \"reversals\": [\n%s  ]\n}\n", n_rev, n_rev_expl, revlog);
    fclose(un);

    /* --- profile.digest --- */
    {
        char pdl[128];
        snprintf(pdl, sizeof pdl, "%s  Turing-profile-v1.0.toml\n", prof_sha);
        if (write_text(g_out, "profile.digest", pdl)) return refuse("out", "IO", "profile.digest");
    }

    /* --- roots --- */
    char prob_root[65], coding_root[65];
    const char *pr[] = {"probability_streams/INDEX"};
    const char *cr_[] = {"arithmetic/results.json", "ans/results.json", "encoded_artifacts/INDEX"};
    if (root_of(g_out, pr, 1, prob_root) || root_of(g_out, cr_, 3, coding_root)) return refuse("out", "IO", "roots");

    /* --- pending final receipt (verdict and S8 fixed by `gate`) --- */
    char t1[32];
    utc_now(t1);
    char ev_sha[65];
    self_sha(ev_sha);

    FILE *fr = open_out("final_receipt.pending.json");
    if (!fr) return refuse("out", "IO", "pending receipt");
    fprintf(fr, "{\n  \"schema\": \"turing.cal.measurement_receipt.v1\",\n  \"experiment\": \"EXP-001\",\n  \"kind\": \"%s\",\n",
            g_dry ? "dry_run" : "final");
    fprintf(fr, "  \"EXP_001_COMPRESSION_BRIDGE\": \"@BRIDGE@\",\n  \"verdict\": \"@VERDICT@\",\n");
    fprintf(fr, "  \"run_id\": \"EXP-001-%s-%s\",\n  \"created_utc\": \"@CREATED@\",\n  \"started_utc\": \"%s\",\n  \"scored_utc\": \"%s\",\n",
            g_dry ? "dry" : "sealed", t0, t0, t1);
    fprintf(fr, "  \"profile\": {\"path\": \"calibration/profiles/Turing-profile-v1.0.toml\", \"sha256\": \"%s\", \"sidecar_matches\": true},\n",
            prof_sha);
    {
        char pj[PATH_MAX], prj[65] = "";
        snprintf(pj, sizeof pj, "%s/calibration/experiments/EXP-001/preregistration.json", repo);
        if (sha_file_hex(pj, prj)) return refuse("prereg", "IO", "%s", pj);
        fprintf(fr, "  \"freeze\": {\"commit\": \"%s\", \"origin_main_checked\": \"%s\", \"status\": \"%s\", \"frozen_at\": \"%s\", "
                    "\"candidate_manifest_sha256\": \"%s\", \"preregistration_sha256\": \"%s\"},\n",
                g_dry ? "NONE" : d_commit, origin_main, status, frozen_at[0] ? frozen_at : "NONE", man_sha, prj);
    }
    fprintf(fr, "  \"notebook\": {\"operator\": \"%s\", \"host\": \"%s\", \"kernel\": \"%s\", \"commit\": \"%s\", \"tree_clean\": %s},\n",
            nb.operator_, nb.host, nb.kernel, nb.commit, nb.dirty == 0 ? "true" : "false");
    fprintf(fr, "  \"roots\": {\"candidate_root\": \"%s\", \"dataset_root\": \"%s\", \"probability_root\": \"%s\", \"coding_root\": \"%s\", "
                "\"analysis_root\": \"@ANALYSIS_ROOT@\"},\n",
            man_sha, payload, prob_root, coding_root);
    fprintf(fr, "  \"runtime_sha256\": {\"turing-cal-eval\": \"%s\"},\n", ev_sha);
    fprintf(fr, "  \"sealed\": {\"dataset_id\": \"%s\", \"split\": \"%s\", \"dataset_manifest_sha256\": \"%s\", \"seed_commitment_sha256\": \"%s\", "
                "\"manifest_sha256\": \"%s\", \"complete_utc\": \"%s\", \"overlap_audit_pass\": %s},\n",
            dsid, split, ds_sha, sc_sha, sm_sha, released, g_dry ? "false" : "true");
    fprintf(fr, "  \"groups\": [\n");
    for (int g = 1; g <= 2; ++g) {
        fprintf(fr, "    {\"g\": %d, \"seeds\": [", g);
        int fs = 1;
        for (int fi = 0; fi < nf; ++fi)
            if (F[fi].g == g) fprintf(fr, "%s%" PRId64, fs ? "" : ", ", F[fi].seed), fs = 0;
        fprintf(fr, "], \"stream_sha256\": [");
        fs = 1;
        for (int fi = 0; fi < nf; ++fi)
            if (F[fi].g == g) fprintf(fr, "%s\"%s\"", fs ? "" : ", ", tsy_digest[fi]), fs = 0;
        fprintf(fr, "], \"crumbs\": %zu, \"events\": %" PRId64 ", \"bootstrap_seed\": \"0x%016" PRIX64 "\", \"resamples\": %d,\n      \"candidates\": [\n",
                pool_n[g], events[g], (uint64_t)(BOOT_SEED + (uint64_t)g), BOOT_B);
        for (int ci = 0; ci < nc; ++ci) {
            int ok_sum = 1, dec[2] = {1, 1}, inenv[2] = {1, 1};
            for (int fi = 0; fi < nf; ++fi)
                if (F[fi].g == g) {
                    ok_sum &= R[ci][fi].crumb_sum_ub == R[ci][fi].ideal_ub;
                    for (int z = 0; z < 2; ++z) {
                        dec[z] &= R[ci][fi].lossless;
                        inenv[z] &= !R[ci][fi].env_file_fail[z] && !R[ci][fi].env_crumb_fail[z];
                    }
                }
            fprintf(fr, "        {\"name\": \"%s\", \"model_digest\": \"%s\", \"lm_bits\": %" PRId64 ", \"ld_ideal_ub\": %" PRId64
                        ", \"per_crumb_sum_equals_file\": %s, \"t_vs_b2_ideal\": {\"point_ub\": %" PRId64 ", \"lo_ub\": %" PRId64 ", \"hi_ub\": %" PRId64 "},\n"
                        "         \"coded\": [",
                    C[ci].name, C[ci].model_digest, lm[ci], ideal[g][ci], ok_sum ? "true" : "false", Tv[g][ci][0].point, Tv[g][ci][0].lo,
                    Tv[g][ci][0].hi);
            for (int x = 0; x < 2; ++x) {
                int64_t bits = coded[g][ci][x];
                fprintf(fr, "%s{\"coder\": \"%s\", \"coded_bytes\": %" PRId64 ", \"coded_bits\": %" PRId64 ", \"decoded_ok\": %s, \"overhead_ub\": %" PRId64
                            ", \"inside_envelope\": %s, \"t_vs_b2\": {\"point_ub\": %" PRId64 ", \"lo_ub\": %" PRId64 ", \"hi_ub\": %" PRId64 "}}",
                        x ? ", " : "", x ? "TCA1 rANS" : "TCR1 range", bits / 8, bits, dec[x] ? "true" : "false", bits * UB - ideal[g][ci],
                        inenv[x] ? "true" : "false", Tv[g][ci][1 + x].point, Tv[g][ci][1 + x].lo, Tv[g][ci][1 + x].hi);
            }
            fprintf(fr, "]}%s\n", ci == nc - 1 ? "" : ",");
        }
        fprintf(fr, "      ]}%s\n", g == 2 ? "" : ",");
    }
    fprintf(fr, "  ],\n  \"criteria\": {\"S1\": \"%s\", \"S2\": \"%s\", \"S3\": \"%s\", \"S4\": \"%s\", \"S5\": \"%s\", \"S6\": \"%s\", \"S7\": \"%s\", "
                "\"S8\": \"@S8@\", \"S9\": \"%s\"},\n",
            crit[1], crit[2], crit[3], crit[4], crit[5], crit[6], crit[7], crit[9]);
    fprintf(fr, "  \"refusals\": [],\n  \"deviations\": [\"see calibration/docs/PROTOCOL_CONFORMANCE.md (deviations D1-D9)\"%s],\n",
            g_dry ? ", \"DRY RUN on development data: not evidence, S2 not checked\"" : "");
    fprintf(fr, "  \"signed_by\": \"none (unsigned; git commit is the record)\",\n  \"notes\": \"@NOTES@\"\n}\n");
    fclose(fr);

    /* --- pending REPORT.md --- */
    FILE *rp_ = open_out("REPORT.pending.md");
    if (!rp_) return refuse("out", "IO", "pending report");
    fprintf(rp_, "# EXP-001 compression bridge: %s\n\nEXP_001_COMPRESSION_BRIDGE = @BRIDGE@\n\n", g_dry ? "DRY RUN (development data, not evidence)" : "report");
    fprintf(rp_, "Profile sha256 %s. Candidate manifest sha256 %s (status %s). Dataset %s (%s), payload %s.\n\n", prof_sha, man_sha, status,
            dsid, split, payload);
    fprintf(rp_, "Criteria: S1 %s, S2 %s, S3 %s, S4 %s, S5 %s, S6 %s, S7 %s, S8 @S8@, S9 %s.\n\n", crit[1], crit[2], crit[3], crit[4], crit[5],
            crit[6], crit[7], crit[9]);
    for (int g = 1; g <= 2; ++g) {
        fprintf(rp_, "## Group %d (%s): seeds", g, g == 1 ? "primary" : "replication");
        for (int fi = 0; fi < nf; ++fi)
            if (F[fi].g == g) fprintf(rp_, " %" PRId64, F[fi].seed);
        fprintf(rp_, ", %zu crumbs, %" PRId64 " events\n\n", pool_n[g], events[g]);
        fprintf(rp_, "All values in bits. T against B2; uncertainty = 95%% crumb-bootstrap interval of T_ideal (T_A, T_B intervals are the same interval shifted).\n\n");
        fprintf(rp_, "| candidate | L(M) | ideal bits | coder-A bits | coder-B bits | T_ideal | T_A | T_B | uncertainty | result |\n");
        fprintf(rp_, "|---|---:|---:|---:|---:|---:|---:|---:|---|---|\n");
        for (int ci = 0; ci < nc; ++ci) {
            char a[40], b[40], c_[40], d[40], lo_[40], hi_[40];
            fmt_bits(ideal[g][ci], a, sizeof a);
            fmt_bits(Tv[g][ci][0].point, b, sizeof b);
            fmt_bits(Tv[g][ci][1].point, c_, sizeof c_);
            fmt_bits(Tv[g][ci][2].point, d, sizeof d);
            fmt_bits(Tv[g][ci][0].lo, lo_, sizeof lo_);
            fmt_bits(Tv[g][ci][0].hi, hi_, sizeof hi_);
            const char *res;
            /* Same three-way rule as S6 / S7 (prereg section 5a): all three measures decide PASS, any one decides FAIL. */
            int lop = Tv[g][ci][0].lo > 0 && Tv[g][ci][1].lo > 0 && Tv[g][ci][2].lo > 0;
            int hin = Tv[g][ci][0].hi < 0 && Tv[g][ci][1].hi < 0 && Tv[g][ci][2].hi < 0;
            int anylo = Tv[g][ci][0].lo > 0 || Tv[g][ci][1].lo > 0 || Tv[g][ci][2].lo > 0;
            int anyhi = Tv[g][ci][0].hi < 0 || Tv[g][ci][1].hi < 0 || Tv[g][ci][2].hi < 0;
            if (ci == b2) res = "baseline";
            else if (ci == mi) res = lop ? "wins (S6 PASS rule)" : anyhi ? "loses (S6 FAIL rule)" : "straddles 0 (S6 INCONCLUSIVE rule)";
            else if (ci == m1 || ci == m2) res = hin ? "loses (S7 PASS rule)" : anylo ? "wins (S7 FAIL rule)" : "straddles 0 (S7 INCONCLUSIVE rule)";
            else res = lop ? "beats B2 (reported)" : hin ? "loses to B2 (reported)" : "mixed or ties B2 (reported)";
            fprintf(rp_, "| %s | %" PRId64 " | %s | %" PRId64 " | %" PRId64 " | %s | %s | %s | [%s, %s] | %s |\n", C[ci].name, lm[ci], a,
                    coded[g][ci][0], coded[g][ci][1], b, c_, d, lo_, hi_, res);
        }
        fprintf(rp_, "\nEnvelope (two-sided, per file and per crumb, |overhead - 448| <= 64 + 0.001 N bits):\n\n");
        for (int ci = 0; ci < nc; ++ci) {
            int out_[2] = {0, 0};
            int64_t wm[2] = {INT64_MAX, INT64_MAX};
            for (int fi = 0; fi < nf; ++fi)
                if (F[fi].g == g)
                    for (int z = 0; z < 2; ++z) {
                        out_[z] += R[ci][fi].env_file_fail[z] + R[ci][fi].env_crumb_fail[z];
                        if (R[ci][fi].worst_crumb_margin[z] < wm[z]) wm[z] = R[ci][fi].worst_crumb_margin[z];
                    }
            char w0[40], w1[40], o0[40], o1[40];
            fmt_bits(wm[0], w0, sizeof w0), fmt_bits(wm[1], w1, sizeof w1);
            fmt_bits(coded[g][ci][0] * UB - ideal[g][ci], o0, sizeof o0);
            fmt_bits(coded[g][ci][1] * UB - ideal[g][ci], o1, sizeof o1);
            fprintf(rp_, "- %s: overhead A %s bits, B %s bits over the group; units outside the band A %d, B %d; worst crumb margin A %s, B %s bits\n",
                    C[ci].name, o0, o1, out_[0], out_[1], w0, w1);
        }
        fprintf(rp_, "\nSpearman rho of the description-length order, ideal vs A %.4f, ideal vs B %.4f (reported only).\n\n", rho[g][0], rho[g][1]);
    }
    fprintf(rp_, "Reversals: %d found, %d explained by the coder envelope (detail in uncertainty.json).\n\n", n_rev, n_rev_expl);
    fprintf(rp_, "Sensitivity (T against B0, B1, B3; L(M) byte-rounded and doubled; data-only gain; interval inflated x2.04) is in uncertainty.json; it is reported, not part of the verdict.\n\n");
    /* Required report sections (laboratory execution protocol, EXP-001 final report). */
    fprintf(rp_, "## Report sections\n\n");
    fprintf(rp_, "- Preregistration: calibration/preregistration/EXP-001.md and preregistration.json (sha256 in final_receipt.json freeze.preregistration_sha256).\n");
    fprintf(rp_, "- Blinding: calibration/docs/BLINDING_PROTOCOL.md; overlap audit %s.\n", g_dry ? "not used (dry run)" : "PASS (overlap_audit.json)");
    fprintf(rp_, "- Candidate freeze: candidate_manifest.json sha256 %s, status %s, frozen_at %s, freeze commit C_f %s.\n", man_sha, status,
            frozen_at[0] ? frozen_at : "NONE", g_dry ? "NONE (dry run)" : d_commit);
    fprintf(rp_, "- Model-cost accounting: L(M) = exact TYM0 bits (calibration/docs/MODEL_DESCRIPTION_ENCODING.md), re-checked against the manifest for every candidate.\n");
    fprintf(rp_, "- Probability stream: one TPS1 per candidate and file, probability_root %s (probability_streams/INDEX).\n", prob_root);
    fprintf(rp_, "- Ideal codelength: ideal_lengths.json (per file and per crumb, int64 ub).\n");
    fprintf(rp_, "- Actual coder results: arithmetic/results.json, ans/results.json, encoded_artifacts/INDEX, decoder_receipts/; coding_root %s.\n", coding_root);
    fprintf(rp_, "- Memorizer result: rows %s and %s in the tables above (criterion S7, S9).\n", MEM1, MEM2);
    fprintf(rp_, "- Independent verification: S8 = @S8@ (scorer_independent.json vs scorer_primary.json).\n");
    fprintf(rp_, "- Uncertainty: uncertainty.json (calibration/docs/UNCERTAINTY_PROTOCOL.md).\n");
    fprintf(rp_, "- Failures/deviations: calibration/docs/PROTOCOL_CONFORMANCE.md (D1-D9)%s.\n", g_dry ? "; DRY RUN on development data" : "");
    fprintf(rp_, "- Gate decision: EXP_001_COMPRESSION_BRIDGE = @BRIDGE@ (rule: prereg section 5a).\n\n");
    fprintf(rp_, "@NOTES@\n");
    fclose(rp_);

    for (int i = 0; i < nf; ++i) ty_stream_free(&F[i].s);
    fprintf(stderr, "run complete: S1 %s S2 %s S3 %s S4 %s S5 %s S6 %s S7 %s S9 %s; next: turing-cal-eval gate\n", crit[1], crit[2], crit[3],
            crit[4], crit[5], crit[6], crit[7], crit[9]);
    return 0;
}

/* ---------- gate ---------- */

typedef struct {
    char key[160];
    int64_t v;
} kv_t;

static int load_values(const char *path, kv_t **out, int *n) {
    char *t = slurp(path, NULL);
    if (!t) return -1;
    int cap = 1024;
    kv_t *a = malloc(cap * sizeof *a);
    *n = 0;
    for (char *l = t; l && *l;) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        char k[160];
        int64_t v;
        if (!jstr(l, "key", k, sizeof k) && !jint(l, "value", &v)) {
            if (*n == cap) a = realloc(a, (cap *= 2) * sizeof *a);
            snprintf(a[*n].key, sizeof a[*n].key, "%s", k);
            a[*n].v = v;
            ++*n;
        }
        l = e ? e + 1 : NULL;
    }
    free(t);
    *out = a;
    return 0;
}

static char *replace_all(const char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *p = strstr(s, from); p; p = strstr(p + fl, from)) ++n;
    char *o = malloc(strlen(s) + n * (tl + 1) + 1), *w = o;
    for (const char *p = s;;) {
        const char *q = strstr(p, from);
        if (!q) {
            strcpy(w, p);
            break;
        }
        memcpy(w, p, (size_t)(q - p)), w += q - p;
        memcpy(w, to, tl), w += tl;
        p = q + fl;
    }
    return o;
}

static int crit_of(const char *text, const char *k, char *o, size_t n) {
    char pat[16];
    snprintf(pat, sizeof pat, "\"%s\": \"", k);
    const char *p = strstr(text, "\"criteria\"");
    if (!p || !(p = strstr(p, pat))) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) o[i++] = *p++;
    o[i] = 0;
    return 0;
}

static int cmd_gate(int argc, char **argv) {
    const char *bundle = NULL, *indep = NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--dry-run")) g_dry = 1;
        else if (!strcmp(argv[i], "--bundle") && i + 1 < argc) bundle = argv[++i];
        else if (!strcmp(argv[i], "--independent") && i + 1 < argc) indep = argv[++i];
        else return refuse("args", "ARG", "unknown argument %s", argv[i]);
    }
    if (!bundle || !indep) return refuse("args", "ARG", "need --bundle and --independent");
    char p[PATH_MAX], q[PATH_MAX];
    struct stat st;
    snprintf(p, sizeof p, "%s/final_receipt.json", bundle);
    if (!stat(p, &st)) return refuse("write once", "EXISTS", "%s already exists; a verdict is never replaced", p);
    /* Gate refusals before the comparison are operator errors: no receipt of any kind is written. */
    snprintf(p, sizeof p, "%s/final_receipt.pending.json", bundle);
    char *rec = slurp(p, NULL);
    snprintf(q, sizeof q, "%s/REPORT.pending.md", bundle);
    char *rep = slurp(q, NULL);
    if (!rec || !rep) return refuse("gate", "IO", "pending receipt or report missing; run first");
    int rec_dry = strstr(rec, "\"kind\": \"dry_run\"") != NULL;
    if (rec_dry != g_dry) return refuse("gate", "MODE", "bundle dry-run flag and --dry-run disagree");
    /* Receipt binding for a terminal receipt written by the gate: taken from the pending receipt of the run. */
    {
        char fc[48] = "";
        if (jtop(rec, "sha256", g_rprof, sizeof g_rprof) || jtop(rec, "candidate_manifest_sha256", g_rman, sizeof g_rman) ||
            jtop(rec, "commit", fc, sizeof fc) || strlen(g_rprof) != 64 || strlen(g_rman) != 64)
            return refuse("gate", "FORMAT", "pending receipt lacks profile, freeze commit or candidate manifest digest");
        snprintf(g_rfreeze, sizeof g_rfreeze, "%s", g_dry ? "DRY_RUN" : fc);
    }
    char prim[PATH_MAX], ph[65], ih[65];
    snprintf(prim, sizeof prim, "%s/scorer_primary.json", bundle);
    if (sha_file_hex(prim, ph)) return refuse("gate", "IO", "scorer_primary.json");
    int indep_ok = !sha_file_hex(indep, ih);
    int self_cmp = indep_ok && !strcmp(ph, ih);
    if (self_cmp && !g_dry) return refuse("S8", "NOT_INDEPENDENT", "scorer_independent.json is byte-identical to scorer_primary.json");
    /* The run already produced scores: from here on any failure is a final FAIL, never a void. A lane D crash, a
     * missing, unreadable or unparsable scorer_independent.json, or one that reports problems of its own, is S8
     * FAIL with no retry (EVALUATOR.md section 6). */
    g_out = bundle;
    g_scored = 1;
    if (!indep_ok) return refuse_c("S8", "S8", "INDEP_MISSING", "independent scorer file %s is missing or unreadable", indep);
    /* S8: same key set, same int64 values. */
    kv_t *a, *b;
    int na, nb;
    if (load_values(prim, &a, &na)) return refuse("gate", "IO", "values of scorer_primary.json");
    if (load_values(indep, &b, &nb)) return refuse_c("S8", "S8", "INDEP_FORMAT", "values of %s cannot be parsed", indep);
    int s8 = na > 0 && na == nb, mism = 0;
    if (!self_cmp) { /* lane D records its own mismatches in "problems"; anything but 0 (or no field) fails S8 */
        char *ti = slurp(indep, NULL);
        int64_t pr = -1;
        if (!ti || jint(ti, "problems", &pr) || pr != 0) ++mism;
        free(ti);
    }
    /* The independent file must name the same inputs and schema, and may not repeat a key. */
    {
        static const char *hk[] = {"schema", "profile_sha256", "candidate_manifest_sha256", "dataset_manifest_sha256"};
        char *ta = slurp(prim, NULL), *tb_ = slurp(indep, NULL);
        for (int i = 0; i < 4; ++i) {
            char va[128] = "", vb[128] = "";
            if (!ta || !tb_ || jstr(ta, hk[i], va, sizeof va) || jstr(tb_, hk[i], vb, sizeof vb) || strcmp(va, vb)) ++mism;
        }
        free(ta), free(tb_);
        for (int i = 0; i < nb; ++i)
            for (int j = i + 1; j < nb; ++j)
                if (!strcmp(b[i].key, b[j].key)) ++mism;
    }
    for (int i = 0; i < na; ++i) {
        int found = 0;
        for (int j = 0; j < nb && !found; ++j)
            if (!strcmp(a[i].key, b[j].key)) found = 1, mism += a[i].v != b[j].v;
        if (!found) ++mism;
    }
    if (mism) s8 = 0;
    char c[10][32];
    const char *names[] = {"", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "", "S9"};
    for (int i = 1; i <= 9; ++i)
        if (i != 8 && crit_of(rec, names[i], c[i], sizeof c[i])) return refuse("gate", "FORMAT", "criterion %s missing", names[i]);
    snprintf(c[8], sizeof c[8], "%s", s8 ? "PASS" : "FAIL");
    /* Verdict (prereg section 5a). */
    const char *verdict = "PASS";
    int fail = 0, inc = 0;
    for (int i = 1; i <= 9; ++i) {
        if (!strcmp(c[i], "FAIL")) fail = 1;
        else if (!strcmp(c[i], "INCONCLUSIVE")) inc = 1;
        else if (strcmp(c[i], "PASS") && !(g_dry && i == 2)) fail = 1; /* NOT_REACHED counts against PASS */
    }
    if (fail) verdict = "FAIL";
    else if (inc) verdict = "INCONCLUSIVE";
    char bridge[64], notes[512], created[32], s8s[64];
    utc_now(created);
    snprintf(bridge, sizeof bridge, "%s", g_dry ? "DRY_RUN_NOT_EVIDENCE" : verdict);
    snprintf(s8s, sizeof s8s, "%s", c[8]);
    snprintf(notes, sizeof notes, "%s%s S8 compared %d values, %d mismatches.",
             g_dry ? "Dry run on development data; the verdict line below is what these numbers would give, not a result. " : "",
             self_cmp ? "S8 was a self-comparison (dry run only, not evidence)." : "S8 compared against an independent scorer file.", na, mism);
    /* analysis root over the scoring outputs, independent file included. */
    char ind_copy[PATH_MAX];
    snprintf(ind_copy, sizeof ind_copy, "%s/scorer_independent.json", bundle);
    if (strcmp(indep, ind_copy)) {
        uint8_t *buf;
        size_t len;
        if (tc_read_file(indep, &buf, &len) != TC_OK || tc_write_file(ind_copy, buf, len) != TC_OK)
            return refuse("gate", "IO", "copy independent scorer");
        free(buf);
    }
    char aroot[65];
    const char *ar[] = {"ideal_lengths.json", "scorer_primary.json", "scorer_independent.json", "uncertainty.json"};
    if (root_of(bundle, ar, 4, aroot)) return refuse("gate", "IO", "analysis root");
    char *r1 = replace_all(rec, "@BRIDGE@", bridge), *r2 = replace_all(r1, "@VERDICT@", verdict), *r3 = replace_all(r2, "@S8@", s8s),
         *r4 = replace_all(r3, "@ANALYSIS_ROOT@", aroot), *r5 = replace_all(r4, "@CREATED@", created), *r6 = replace_all(r5, "@NOTES@", notes);
    char vline[256];
    snprintf(vline, sizeof vline, "%s\n\nVerdict rule result: %s.", notes, verdict);
    char *q1 = replace_all(rep, "@BRIDGE@", bridge), *q2 = replace_all(q1, "@S8@", s8s), *q3 = replace_all(q2, "@NOTES@", vline);
    snprintf(p, sizeof p, "%s/final_receipt.json", bundle);
    int fd_ok = 0;
    {
        FILE *f = fopen(p, "wx"); /* exclusive create: write once */
        if (f) fd_ok = fputs(r6, f) >= 0, fd_ok &= fclose(f) == 0;
    }
    if (!fd_ok) return refuse("write once", "EXISTS", "cannot create %s exclusively", p);
    if (write_text(bundle, "REPORT.md", q3)) return refuse("gate", "IO", "REPORT.md");
    snprintf(p, sizeof p, "%s/final_receipt.pending.json", bundle), unlink(p);
    unlink(q);
    printf("EXP_001_COMPRESSION_BRIDGE = %s (verdict rule: %s; S1 %s S2 %s S3 %s S4 %s S5 %s S6 %s S7 %s S8 %s S9 %s)\n", bridge, verdict, c[1],
           c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]);
    free(r1), free(r2), free(r3), free(r4), free(r5), free(r6), free(q1), free(q2), free(q3), free(rec), free(rep), free(a), free(b);
    return 0;
}

int main(int argc, char **argv) {
    int rc = -1;
    if (argc >= 2 && !strcmp(argv[1], "run")) rc = cmd_run(argc, argv);
    else if (argc >= 2 && !strcmp(argv[1], "gate")) rc = cmd_gate(argc, argv);
    /* A refusal ends the process at once: buffers of the abandoned run are not unwound (exit 2 stays exit 2
     * under the leak checker of the ASan test build). */
    if (rc == 2) {
        fflush(NULL);
        _exit(2);
    }
    if (rc >= 0) return rc;
    fprintf(stderr, "usage: turing-cal-eval run|gate ... (see calibration/docs/EVALUATOR.md)\n");
    return 2;
}
