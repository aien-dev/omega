/*
 * ola.c -- trusted half of the one-layer stage-1 hostile acceptance harness (omega#443).
 * Contract: tests/one_layer/acceptance/HARNESS_INTERFACE.md (v1, sha256 in RULES).
 *
 *   ola untar  <corpus.tar> <SEALED.sha256> <destdir> cases|labels
 *        check the tarball hash against the sealed file, validate every member (regular files and
 *        directories only, no absolute paths, no "..", only the contract layout), then extract
 *        either everything except corpus/LABELS ("cases") or only corpus/LABELS ("labels").
 *        Prints "corpus_sha256=<hex>".
 *   ola pass   <corpus_dir> <verdicts.txt> <receipts_dir> <scratch_root> <ola_runner> <meta>
 *        one verdict line + one receipt per case (sorted by case id); never reads LABELS.
 *   ola probes <ola_runner> <scratch_root> <probes.log> <time_ms> <mem_mb> name=path...
 *        every isolation probe sandboxed (must be blocked) and as a negative control without the
 *        defence (must escape); "PROBES PASS" only if every probe is blocked AND its control escaped.
 *   ola tally  <verdicts.txt> <LABELS> <replay_identical 0|1> <probes_pass 0|1> <clean 0|1>
 *        prints the score.txt lines.
 *   ola cmp    <a> <b>                      byte-identical files (exit 0) or not (exit 1)
 *
 * Hosted C (glibc, Linux syscalls) is a temporary dependency: see evidence/ONE-LAYER-1/INVENTORY.txt.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sha256.h"

#define MAX_TAR (64u << 20)
#define MAX_FILE (2u << 20)
#define MAX_CAND (1u << 20)
#define MAX_LINES 4096
#define OLA_EXIT_UNAVAIL 96
#define OLA_EXIT_SANDBOX 97
#define OLA_EXIT_MEMORY 98

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ola: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void hex32(const uint8_t d[32], char out[65])
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

static void sha_hex(const void *p, size_t n, char out[65])
{
    uint8_t d[32];
    sha256_hash((const uint8_t *)p, n, d);
    hex32(d, out);
}

/* Read a whole regular file (no symlink following), at most max bytes. NULL on any failure. */
static uint8_t *slurp_at(int dirfd, const char *path, size_t max, size_t *len)
{
    int fd = openat(dirfd, path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > max) { close(fd); errno = EFBIG; return NULL; }
    size_t n = (size_t)st.st_size, got = 0;
    uint8_t *b = malloc(n + 1);
    if (!b) { close(fd); return NULL; }
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; free(b); close(fd); return NULL; }
        got += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    *len = n;
    return b;
}
static uint8_t *slurp(const char *path, size_t max, size_t *len) { return slurp_at(AT_FDCWD, path, max, len); }

/* ======================================================================================== untar */

static uint64_t octal(const char *p, size_t n, int *bad)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ') i++;
    int digits = 0;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++, digits++) v = v * 8 + (uint64_t)(p[i] - '0');
    for (; i < n; i++) if (p[i] != ' ' && p[i] != 0) *bad = 1;
    if (!digits) *bad = 1;
    return v;
}

/* The contract layout (HARNESS_INTERFACE.md section 1). 1 directory, 2 file, 0 refused. */
static int case_id_ok(const char *s, size_t n)
{
    if (n < 1 || n > 40) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

static int layout(const char *p)
{
    if (!strcmp(p, "corpus") || !strcmp(p, "corpus/cases")) return 1;
    if (!strcmp(p, "corpus/LABELS")) return 2;
    if (strncmp(p, "corpus/cases/", 13)) return 0;
    const char *id = p + 13, *sl = strchr(id, '/');
    if (!sl) return case_id_ok(id, strlen(id)) ? 1 : 0;
    if (!case_id_ok(id, (size_t)(sl - id))) return 0;
    const char *rest = sl + 1;
    if (!strcmp(rest, "hidden")) return 1;
    if (!strcmp(rest, "requirement") || !strcmp(rest, "candidate.osc") || !strcmp(rest, "candidate.opir") ||
        !strcmp(rest, "hidden/inputs") || !strcmp(rest, "hidden/expected"))
        return 2;
    return 0;
}

/* Only mtime/atime/ctime records are accepted in a pax extended header (GNU tar --format=posix). */
static int pax_ok(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t len = 0, j = i;
        while (j < n && p[j] >= '0' && p[j] <= '9') len = len * 10 + (size_t)(p[j++] - '0');
        if (j >= n || p[j] != ' ' || len == 0 || i + len > n || p[i + len - 1] != '\n') return 0;
        const char *k = (const char *)p + j + 1;
        size_t kl = i + len - (j + 1);
        if (!((kl > 6 && !memcmp(k, "mtime=", 6)) || (kl > 6 && !memcmp(k, "atime=", 6)) || (kl > 6 && !memcmp(k, "ctime=", 6))))
            return 0;
        i += len;
    }
    return 1;
}

static int mkdirs_at(int root, const char *path)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; ; s++) {
        if (*s == '/' || *s == 0) {
            char c = *s;
            *s = 0;
            if (mkdirat(root, buf, 0700) && errno != EEXIST) return -1;
            struct stat st;
            if (fstatat(root, buf, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)) return -1;
            *s = c;
            if (!c) break;
        }
    }
    return 0;
}

static int cmd_untar(int argc, char **argv)
{
    if (argc != 6) die("usage: ola untar <tar> <sealed> <destdir> cases|labels");
    int want_labels = !strcmp(argv[5], "labels");
    if (!want_labels && strcmp(argv[5], "cases")) die("untar: mode must be cases or labels");
    size_t tn = 0, sn = 0;
    uint8_t *tar = slurp(argv[2], MAX_TAR, &tn);
    if (!tar) die("untar: cannot read %s (%s)", argv[2], strerror(errno));
    uint8_t *sealed = slurp(argv[3], 4096, &sn);
    if (!sealed) die("untar: SEALED file %s is missing: the corpus is not sealed yet; refusing to score", argv[3]);
    char h[65];
    sha_hex(tar, tn, h);
    if (sn != 64 + 2 + strlen("corpus.tar") + 1 || memcmp(sealed, h, 64) || memcmp(sealed + 64, "  corpus.tar\n", 13)) /* OLA:seal-check */
        die("untar: corpus sha256 %s does not match the sealed hash file; refusing to score", h);
    int root = open(argv[4], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) die("untar: destdir %s", argv[4]);
    /* pass 0 validates everything, pass 1 extracts: a refused corpus leaves nothing behind */
    for (int pass = 0; pass < 2; pass++) {
        size_t off = 0, members = 0;
        int ended = 0, pax_pending = 0;
        char seen[1024][96];
        size_t nseen = 0;
        while (off + 512 <= tn) {
            const uint8_t *hd = tar + off;
            int zero = 1;
            for (int i = 0; i < 512; i++) if (hd[i]) { zero = 0; break; }
            if (zero) { ended = 1; break; }
            int bad = 0;
            uint64_t chk = octal((const char *)hd + 148, 8, &bad), sum = 0;
            for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : hd[i];
            if (bad || chk != sum) die("untar: refused: bad header checksum at offset %zu", off);
            if (memcmp(hd + 257, "ustar", 5)) die("untar: refused: not a POSIX ustar member at offset %zu", off);
            uint64_t size = octal((const char *)hd + 124, 12, &bad);
            if (bad || size > MAX_FILE) die("untar: refused: bad or oversized member size at offset %zu", off);
            char type = (char)hd[156];
            const uint8_t *data = hd + 512;
            size_t span = 512 + (size_t)((size + 511) / 512) * 512;
            if (off + span > tn) die("untar: refused: truncated member at offset %zu", off);
            if (type == 'x') {
                if (!pax_ok(data, (size_t)size)) die("untar: refused: pax header with a key other than mtime/atime/ctime");
                pax_pending = 1;
                off += span;
                continue;
            }
            pax_pending = 0;
            char name[160 + 2 + 101];
            char pre[156], nm[101];
            memcpy(pre, hd + 345, 155); pre[155] = 0;
            memcpy(nm, hd, 100); nm[100] = 0;
            if (pre[0]) snprintf(name, sizeof name, "%s/%s", pre, nm);
            else snprintf(name, sizeof name, "%s", nm);
            size_t L = strlen(name);
            if (type == '5' && L && name[L - 1] == '/') name[--L] = 0;
            if (!L || name[0] == '/' || strstr(name, "//") || !strcmp(name, "..") || !strncmp(name, "../", 3) ||
                strstr(name, "/../") || (L >= 3 && !strcmp(name + L - 3, "/..")) || !strncmp(name, "./", 2) ||
                strstr(name, "/./"))
                die("untar: refused: unsafe member name '%s'", name);
            int kind = (type == '0' || type == 0) ? 2 : type == '5' ? 1 : 0;
            if (!kind) die("untar: refused: member '%s' has type '%c' (only regular files and directories)", name, type ? type : '0');
            if (kind == 1 && size) die("untar: refused: directory '%s' with data", name);
            int lay = layout(name);
            if (lay != kind) die("untar: refused: member '%s' is not in the contract layout", name); /* OLA:layout */
            for (size_t i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) die("untar: refused: duplicate member '%s'", name);
            if (nseen >= 1024 || L >= sizeof seen[0]) die("untar: refused: too many members or long name");
            snprintf(seen[nseen++], sizeof seen[0], "%s", name);
            members++;
            if (pass == 1) {
                int is_labels = !strcmp(name, "corpus/LABELS");
                if (kind == 1 && !want_labels) {
                    if (mkdirs_at(root, name)) die("untar: mkdir %s", name);
                } else if (kind == 2 && is_labels == want_labels) {
                    char *sl = strrchr(name, '/');
                    *sl = 0;
                    if (mkdirs_at(root, name)) die("untar: mkdir %s", name);
                    *sl = '/';
                    int fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                    if (fd < 0) die("untar: create %s: %s", name, strerror(errno));
                    if (size && write(fd, data, (size_t)size) != (ssize_t)size) die("untar: write %s", name);
                    if (close(fd)) die("untar: close %s", name);
                }
            }
            off += span;
        }
        if (!ended || pax_pending) die("untar: refused: no end-of-archive marker");
        if (!members) die("untar: refused: empty corpus");
    }
    printf("corpus_sha256=%s\n", h);
    free(tar);
    return 0;
}

/* ===================================================================== sandboxed execution */

typedef struct {
    int timed_out;
    int exited, code, signaled, sig;
    char *out; size_t outn;
    char *err; size_t errn;
} Run;

typedef struct {
    unsigned time_ms, mem_mb;
    int limits;        /* 0 = negative control: no rlimits (wall limit 3x) */
} Limits;

static int memfd_with(const char *name, const void *p, size_t n)
{
    int fd = (int)syscall(SYS_memfd_create, name, 0);
    if (fd < 0) die("memfd_create: %s", strerror(errno));
    if (n && write(fd, p, n) != (ssize_t)n) die("memfd write");
    if (lseek(fd, 0, SEEK_SET) != 0) die("memfd seek");
    return fd;
}

static char *read_fd(int fd, size_t max, size_t *n)
{
    struct stat st;
    if (fstat(fd, &st)) die("fstat memfd");
    size_t len = (size_t)st.st_size;
    if (len > max) len = max;
    char *b = malloc(len + 1);
    if (!b) die("out of memory");
    if (lseek(fd, 0, SEEK_SET) != 0) die("seek memfd");
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, b + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    b[got] = 0;
    *n = got;
    return b;
}

static void run_free(Run *r) { free(r->out); free(r->err); }

static void spawn(const char *runner, char *const argv[], int cand_fd, const void *in, size_t inn, const Limits *lim, Run *r)
{
    memset(r, 0, sizeof *r);
    int fin = memfd_with("ola-in", in, inn), fout = memfd_with("ola-out", NULL, 0), ferr = memfd_with("ola-err", NULL, 0);
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (dup2(fin, 0) < 0 || dup2(fout, 1) < 0 || dup2(ferr, 2) < 0) _exit(127);
        if (cand_fd >= 0) { if (dup2(cand_fd, 3) < 0) _exit(127); }
        else { int nul = open("/dev/null", O_RDONLY); if (nul < 0 || dup2(nul, 3) < 0) _exit(127); }
        syscall(SYS_close_range, 4, ~0u, 0);
        setpgid(0, 0);
        struct rlimit rl;
        unsigned cpu = (lim->time_ms + 999) / 1000 + 1;
        if (lim->limits) {
            rl.rlim_cur = cpu; rl.rlim_max = cpu + 1;                      setrlimit(RLIMIT_CPU, &rl);  /* OLA:rlimit-cpu */
            rl.rlim_cur = rl.rlim_max = (rlim_t)lim->mem_mb << 20;          setrlimit(RLIMIT_AS, &rl);   /* OLA:rlimit-as */
            rl.rlim_cur = rl.rlim_max = 8u << 20;                           setrlimit(RLIMIT_FSIZE, &rl);
            rl.rlim_cur = rl.rlim_max = 0;                                  setrlimit(RLIMIT_CORE, &rl);
            rl.rlim_cur = rl.rlim_max = 32;                                 setrlimit(RLIMIT_NOFILE, &rl);
        }
        char *envp[] = {NULL};
        execve(runner, argv, envp);
        _exit(127);
    }
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd < 0) die("pidfd_open: %s", strerror(errno));
    struct pollfd p = {.fd = pfd, .events = POLLIN};
    int wall = (int)(lim->limits ? lim->time_ms : lim->time_ms * 3 + 5000); /* OLA:wall-limit */
    int pr;
    do pr = poll(&p, 1, wall); while (pr < 0 && errno == EINTR);
    if (pr == 0) {
        r->timed_out = 1;
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) die("waitpid: %s", strerror(errno));
    kill(-pid, SIGKILL); /* nothing in its process group outlives it */
    close(pfd);
    if (WIFEXITED(st)) { r->exited = 1; r->code = WEXITSTATUS(st); }
    if (WIFSIGNALED(st)) { r->signaled = 1; r->sig = WTERMSIG(st); }
    r->out = read_fd(fout, 1u << 20, &r->outn);
    r->err = read_fd(ferr, 4096, &r->errn);
    close(fin); close(fout); close(ferr);
    if (r->exited && r->code == OLA_EXIT_UNAVAIL)
        die("the sandbox could not be built (%s); refusing to score", r->err); /* fail closed */
    if (r->exited && r->code == 127) die("cannot exec %s", runner);
}

static void new_nonce(char out[33])
{
    uint8_t b[16];
    if (getrandom(b, sizeof b, 0) != sizeof b) die("getrandom");
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = h[b[i] >> 4]; out[2 * i + 1] = h[b[i] & 15]; }
    out[32] = 0;
}

static int rm_tree_at(int dirfd, const char *name)
{
    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return unlinkat(dirfd, name, 0);
    DIR *d = fdopendir(fd);
    if (!d) { close(fd); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_type == DT_DIR) rm_tree_at(fd, e->d_name);
        else unlinkat(fd, e->d_name, 0);
    }
    closedir(d);
    return unlinkat(dirfd, name, AT_REMOVEDIR);
}

/* Fresh empty scratch directory <root>/<name>. */
static void fresh_scratch(const char *root, const char *name, char *path, size_t n)
{
    snprintf(path, n, "%s/%s", root, name);
    int rfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rfd < 0) die("scratch root %s", root);
    rm_tree_at(rfd, name);
    if (mkdirat(rfd, name, 0700)) die("mkdir scratch %s", path);
    close(rfd);
}

/* ================================================================================ case model */

typedef enum { K_BOOL, K_U8, K_U16, K_U32, K_U64, K_I8, K_I16, K_I32, K_I64, K_NONE } Kind;
static const char *const kname[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};

static Kind parse_kind(const char *s)
{
    for (int i = 0; i < 9; i++) if (!strcmp(s, kname[i])) return (Kind)i;
    return K_NONE;
}
/* compare width: bool is one byte under AAPCS64 */
static unsigned kwidth(Kind k) { static const unsigned w[] = {8, 8, 16, 32, 64, 8, 16, 32, 64}; return w[k]; }
static int ksigned(Kind k) { return k >= K_I8 && k <= K_I64; }

/* Strict decimal: [-]digits, no '+', no leading zeros except "0", range of kind k. */
static int parse_val(const char *s, Kind k, uint64_t *out)
{
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!*s || (s[0] == '0' && s[1])) return -1;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        if (v > (UINT64_MAX - (uint64_t)(*s - '0')) / 10) return -1;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    unsigned w = kwidth(k);
    if (k == K_BOOL) { if (neg || v > 1) return -1; *out = v; return 0; }
    if (!ksigned(k)) {
        if (neg && v) return -1;
        if (w < 64 && v >> w) return -1;
        *out = v;
        return 0;
    }
    uint64_t lim = (uint64_t)1 << (w - 1); /* |min| */
    if (neg ? v > lim : v >= lim) return -1;
    *out = neg ? (uint64_t)(-(int64_t)(v - 1) - 1) : v; /* canonical: sign-extended to 64 bits */
    return 0;
}

/* Normalize a raw 64-bit return at the declared width (contract section 2). */
static uint64_t at_width(uint64_t r, Kind k)
{
    unsigned w = kwidth(k);
    if (w == 64) return r;
    uint64_t m = ((uint64_t)1 << w) - 1;
    r &= m;
    if (ksigned(k) && (r >> (w - 1))) r |= ~m; /* OLA:sign-extend */
    return r;
}

typedef struct {
    char id[48];
    int osc;                 /* 1 osc, 0 omegaprog */
    char entry[64];
    Kind params[6]; unsigned np;
    Kind ret;
    unsigned width;
    char pid[65];            /* "-" if absent */
    uint32_t n;              /* lines */
    uint64_t *in;            /* n * np canonical */
    char sha_cand[65], sha_req[65], sha_in[65], sha_exp[65];
    const char *cand_name;
    size_t cand_len;
} Case;

/* Next logical line (comments and blank lines skipped, trailing spaces trimmed). */
static char *next_line(char **cur)
{
    while (**cur) {
        char *s = *cur, *e = strchr(s, '\n');
        if (e) { *e = 0; *cur = e + 1; } else *cur = s + strlen(s);
        char *h = strchr(s, '#');
        if (h) *h = 0;
        size_t L = strlen(s);
        while (L && (s[L - 1] == ' ' || s[L - 1] == '\t' || s[L - 1] == '\r')) s[--L] = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (*s) return s;
    }
    return NULL;
}

static int ascii_ok(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) if (b[i] == 0 || b[i] > 126) return 0;
    return 1;
}

static int name_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 63 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return 1;
}

/* Parse requirement text; "" on success, else the malformation. */
static const char *parse_requirement(char *txt, Case *c)
{
    int have_kind = 0, have_entry = 0, have_params = 0, have_ret = 0, have_width = 0, have_pid = 0;
    char *cur = txt, *ln;
    snprintf(c->pid, sizeof c->pid, "-");
    while ((ln = next_line(&cur))) {
        char *tok[10]; int nt = 0;
        for (char *t = strtok(ln, " \t"); t; t = strtok(NULL, " \t")) { if (nt == 10) return "requirement_too_many_tokens"; tok[nt++] = t; }
        if (!strcmp(tok[0], "kind")) {
            if (have_kind++ || nt != 2) return "requirement_kind";
            if (!strcmp(tok[1], "osc")) c->osc = 1;
            else if (!strcmp(tok[1], "omegaprog")) c->osc = 0;
            else return "requirement_kind";
        } else if (!strcmp(tok[0], "entry")) {
            if (have_entry++ || nt != 2 || !name_ok(tok[1])) return "requirement_entry";
            snprintf(c->entry, sizeof c->entry, "%s", tok[1]);
        } else if (!strcmp(tok[0], "params")) {
            if (have_params++ || nt > 7) return "requirement_params";
            c->np = (unsigned)(nt - 1);
            for (int i = 1; i < nt; i++) if ((c->params[i - 1] = parse_kind(tok[i])) == K_NONE) return "requirement_params";
        } else if (!strcmp(tok[0], "ret")) {
            if (have_ret++ || nt != 2 || (c->ret = parse_kind(tok[1])) == K_NONE) return "requirement_ret";
        } else if (!strcmp(tok[0], "width")) {
            if (have_width++ || nt != 2) return "requirement_width";
            if (!strcmp(tok[1], "8")) c->width = 8; else if (!strcmp(tok[1], "16")) c->width = 16;
            else if (!strcmp(tok[1], "32")) c->width = 32; else if (!strcmp(tok[1], "64")) c->width = 64;
            else return "requirement_width";
        } else if (!strcmp(tok[0], "program_id")) {
            if (have_pid++ || nt != 2 || strlen(tok[1]) != 64) return "requirement_program_id";
            for (int i = 0; i < 64; i++) if (!((tok[1][i] >= '0' && tok[1][i] <= '9') || (tok[1][i] >= 'a' && tok[1][i] <= 'f'))) return "requirement_program_id";
            snprintf(c->pid, sizeof c->pid, "%s", tok[1]);
        } else return "requirement_unknown_key";
    }
    if (!have_kind) return "requirement_kind";
    if (c->osc) {
        if (!have_entry || !have_params || !have_ret || have_width || have_pid) return "requirement_keys_for_osc";
    } else {
        if (!have_width || have_entry || have_params || have_ret) return "requirement_keys_for_omegaprog";
        c->np = 1;
        c->ret = c->width == 8 ? K_U8 : c->width == 16 ? K_U16 : c->width == 32 ? K_U32 : K_U64;
        c->params[0] = c->ret;
    }
    return "";
}

static const char *parse_inputs(char *txt, Case *c)
{
    char *cur = txt, *ln;
    c->in = calloc((size_t)MAX_LINES * 6 + 1, sizeof *c->in);
    if (!c->in) die("out of memory");
    c->n = 0;
    while ((ln = next_line(&cur))) {
        if (c->n == MAX_LINES) return "inputs_too_many_lines";
        unsigned k = 0;
        for (char *t = strtok(ln, " \t"); t; t = strtok(NULL, " \t")) {
            if (k == c->np) return "inputs_arity";
            if (parse_val(t, c->params[k], &c->in[(size_t)c->n * c->np + k])) return "inputs_value";
            k++;
        }
        if (k != c->np) return "inputs_arity";
        c->n++;
    }
    if (c->n < 1) return "inputs_empty";
    return "";
}

typedef struct { int trap; int code; uint64_t v; } Want; /* trap: 0 value, 1 any trap, 2 that code */

static const char *parse_expected(char *txt, const Case *c, Want *w)
{
    char *cur = txt, *ln;
    uint32_t n = 0;
    while ((ln = next_line(&cur))) {
        if (n == c->n) return "expected_line_count";
        Want *x = &w[n];
        memset(x, 0, sizeof *x);
        if (!strncmp(ln, "TRAP", 4) && (ln[4] == 0 || ln[4] == ' ' || ln[4] == '\t')) {
            char *a = ln + 4;
            while (*a == ' ' || *a == '\t') a++;
            if (!*a) x->trap = 1;
            else {
                uint64_t code;
                if (parse_val(a, K_U8, &code) || code == 0) return "expected_trap_code";
                x->trap = 2; x->code = (int)code;
            }
        } else if (parse_val(ln, c->ret, &x->v)) return "expected_value";
        n++;
    }
    if (n != c->n) return "expected_line_count";
    return "";
}

/* ========================================================================= verdict + receipt */

typedef struct {
    char commit[64], harness[65], rules[65], corpus[65];
    unsigned time_ms, mem_mb;
} Meta;

static void read_meta(const char *path, Meta *m)
{
    size_t n;
    char *t = (char *)slurp(path, 4096, &n);
    if (!t) die("meta %s", path);
    memset(m, 0, sizeof *m);
    char *cur = t, *ln;
    while ((ln = next_line(&cur))) {
        char *eq = strchr(ln, '=');
        if (!eq) die("meta line without '='");
        *eq++ = 0;
        if (!strcmp(ln, "commit")) snprintf(m->commit, sizeof m->commit, "%s", eq);
        else if (!strcmp(ln, "harness_sha256")) snprintf(m->harness, sizeof m->harness, "%s", eq);
        else if (!strcmp(ln, "rules_sha256")) snprintf(m->rules, sizeof m->rules, "%s", eq);
        else if (!strcmp(ln, "corpus_sha256")) snprintf(m->corpus, sizeof m->corpus, "%s", eq);
        else if (!strcmp(ln, "time_ms")) m->time_ms = (unsigned)atoi(eq);
        else if (!strcmp(ln, "mem_mb")) m->mem_mb = (unsigned)atoi(eq);
        else die("meta: unknown key %s", ln);
    }
    if (!m->commit[0] || strlen(m->harness) != 64 || strlen(m->rules) != 64 || strlen(m->corpus) != 64 || !m->time_ms || !m->mem_mb)
        die("meta: incomplete");
    free(t);
}

/* Write receipt bytes under receipts/<sha256>; never rewrite. Returns the hash in h. */
static void put_receipt(int rdir, const char *body, size_t n, char h[65])
{
    sha_hex(body, n, h);
    char tmp[128];
    snprintf(tmp, sizeof tmp, ".tmp.%s.%d", h, (int)getpid());
    unlinkat(rdir, tmp, 0);
    int fd = openat(rdir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0444);
    if (fd < 0 || write(fd, body, n) != (ssize_t)n || fsync(fd) || close(fd)) die("receipt write");
    if (linkat(rdir, tmp, rdir, h, 0)) {
        if (errno != EEXIST) die("receipt link: %s", strerror(errno));
        size_t on;
        int ofd = openat(rdir, h, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        uint8_t *old = ofd >= 0 ? slurp_at(rdir, h, 1u << 16, &on) : NULL;
        if (ofd >= 0) close(ofd);
        if (!old || on != n || memcmp(old, body, n)) die("receipt %s exists with different bytes: evidence tampered", h); /* OLA:receipt-immutable */
        free(old);
    }
    unlinkat(rdir, tmp, 0);
}

static const char *judge(const Case *c, const Want *w, const Run *r, const char *nonce, char *det, size_t dn)
{
    snprintf(det, dn, "-");
    if (r->timed_out || (r->signaled && (r->sig == SIGXCPU || r->sig == SIGKILL))) return "timeout";
    if ((r->exited && r->code == OLA_EXIT_SANDBOX) || (r->signaled && r->sig == SIGSYS)) {
        const char *s = strstr(r->err, "syscall=");
        int nr = s ? atoi(s + 8) : -1;
        snprintf(det, dn, "syscall=%d", nr);
        return "sandbox_violation";
    }
    if (r->exited && r->code == OLA_EXIT_MEMORY) { snprintf(det, dn, "rlimit_as"); return "memory"; }
    if (r->signaled) { snprintf(det, dn, "signal=%d", r->sig); return "crash"; }
    if (!r->exited || r->code) { snprintf(det, dn, "exit=%d", r->code); return "crash"; }
    /* strict transcript */
    char *t = strdup(r->out), *save = NULL;
    if (!t) die("out of memory");
    const char *reason = "ok";
    char hdr[64];
    snprintf(hdr, sizeof hdr, "OLA1 %s", nonce);
    char *ln = strtok_r(t, "\n", &save);
    if (!ln || strcmp(ln, hdr)) { /* OLA:nonce-check */
        snprintf(det, dn, "stale_or_forged_transcript");
        free(t);
        return "sandbox_violation";
    }
    uint32_t i = 0;
    int ended = 0;
    while ((ln = strtok_r(NULL, "\n", &save))) {
        if (ended) { reason = "crash"; snprintf(det, dn, "trailing_transcript"); break; }
        if (!strncmp(ln, "REFUSE ", 7) && i == 0) {
            char rs[40] = "", dd[100] = "";
            if (sscanf(ln + 7, "%39s %99s", rs, dd) != 2) { reason = "crash"; snprintf(det, dn, "malformed_transcript"); break; }
            static const char *const ok[] = {"compile_refused", "decode_refused", "realize_refused", "signature_mismatch", "oversize"};
            reason = NULL;
            for (int k = 0; k < 5; k++) if (!strcmp(rs, ok[k])) reason = ok[k];
            if (!reason) { reason = "crash"; snprintf(det, dn, "malformed_transcript"); break; }
            snprintf(det, dn, "%s", dd);
            char end[64];
            snprintf(end, sizeof end, "END %s 0", nonce);
            ln = strtok_r(NULL, "\n", &save);
            if (!ln || strcmp(ln, end) || strtok_r(NULL, "\n", &save)) { reason = "crash"; snprintf(det, dn, "malformed_transcript"); }
            free(t);
            return reason;
        }
        if (!strncmp(ln, "END ", 4)) {
            char end[64];
            snprintf(end, sizeof end, "END %s %" PRIu32, nonce, c->n);
            if (strcmp(ln, end) || i != c->n) { reason = "crash"; snprintf(det, dn, "malformed_transcript"); break; }
            ended = 1;
            continue;
        }
        if (i >= c->n) { reason = "crash"; snprintf(det, dn, "malformed_transcript"); break; }
        const Want *x = &w[i];
        if (ln[0] == 'T' && ln[1] == ' ') {
            int code = atoi(ln + 2);
            if (!x->trap) { reason = "unexpected_trap"; snprintf(det, dn, "line=%u,trap=%d", i + 1, code); break; }
            if (x->trap == 2 && code != x->code) { reason = "wrong_trap"; snprintf(det, dn, "line=%u,want=%d,got=%d", i + 1, x->code, code); break; } /* OLA:trap-code */
        } else if (ln[0] == 'R' && ln[1] == ' ') {
            char *e;
            errno = 0;
            uint64_t v = strtoull(ln + 2, &e, 10);
            if (*e || errno) { reason = "crash"; snprintf(det, dn, "malformed_transcript"); break; }
            if (x->trap) { reason = "missing_trap"; snprintf(det, dn, "line=%u", i + 1); break; }
            if (at_width(v, c->ret) != x->v) { /* OLA:value-compare */
                reason = "wrong_result";
                snprintf(det, dn, "line=%u", i + 1);
                break;
            }
        } else { reason = "crash"; snprintf(det, dn, "malformed_transcript"); break; }
        i++;
    }
    if (!strcmp(reason, "ok") && !ended) { reason = "crash"; snprintf(det, dn, "truncated_transcript"); }
    free(t);
    return reason;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void hash_file_at(int dfd, const char *p, char out[65], size_t *len)
{
    size_t n = 0;
    uint8_t *b = slurp_at(dfd, p, MAX_FILE, &n);
    if (!b) { snprintf(out, 65, "-"); if (len) *len = 0; return; }
    sha_hex(b, n, out);
    if (len) *len = n;
    free(b);
}

static int cmd_pass(int argc, char **argv)
{
    if (argc != 8) die("usage: ola pass <corpus_dir> <verdicts> <receipts_dir> <scratch_root> <ola_runner> <meta>");
    const char *corpus = argv[2], *runner = argv[6];
    Meta m;
    read_meta(argv[7], &m);
    char cases_path[4096];
    snprintf(cases_path, sizeof cases_path, "%s/cases", corpus);
    struct stat lst;
    char lpath[4096];
    snprintf(lpath, sizeof lpath, "%s/LABELS", corpus);
    if (!lstat(lpath, &lst)) die("LABELS is present before verdicts are written; refusing"); /* OLA:labels-late */
    int vfd = open(argv[3], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (vfd < 0) die("verdicts %s exists or cannot be created: never overwritten", argv[3]);
    FILE *vf = fdopen(vfd, "w");
    int rdir = open(argv[4], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rdir < 0) die("receipts dir %s", argv[4]);
    DIR *d = opendir(cases_path);
    if (!d) die("no cases directory");
    char *ids[1024];
    size_t nid = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (nid == 1024) die("too many cases");
        ids[nid++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(ids, nid, sizeof ids[0], cmpstr);
    int casesfd = open(cases_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (casesfd < 0) die("cases dir");
    for (size_t ci = 0; ci < nid; ci++) {
        Case c;
        memset(&c, 0, sizeof c);
        snprintf(c.id, sizeof c.id, "%s", ids[ci]);
        int cfd = openat(casesfd, c.id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (cfd < 0) die("case dir %s", c.id);
        const char *verdict = "ERROR", *reason = "case_malformed";
        char det[160] = "-";
        size_t rn = 0, inn = 0, en = 0;
        uint8_t *req = slurp_at(cfd, "requirement", MAX_FILE, &rn);
        uint8_t *inp = slurp_at(cfd, "hidden/inputs", MAX_FILE, &inn);
        uint8_t *exp = NULL;
        hash_file_at(cfd, "requirement", c.sha_req, NULL);
        hash_file_at(cfd, "hidden/inputs", c.sha_in, NULL);
        hash_file_at(cfd, "hidden/expected", c.sha_exp, NULL);
        struct stat so, sp;
        int has_osc = !fstatat(cfd, "candidate.osc", &so, AT_SYMLINK_NOFOLLOW);
        int has_op = !fstatat(cfd, "candidate.opir", &sp, AT_SYMLINK_NOFOLLOW);
        snprintf(c.sha_cand, sizeof c.sha_cand, "-");
        const char *bad = "";
        Want *want = NULL;
        if (!req) bad = "missing_requirement";
        else if (!inp) bad = "missing_inputs";
        else if (!ascii_ok(req, rn) || !ascii_ok(inp, inn)) bad = "not_ascii";
        const char *kinds = "-";
        if (!*bad) bad = parse_requirement((char *)req, &c);
        if (!*bad) kinds = c.osc ? "osc" : "omegaprog";
        if (!*bad && has_osc + has_op != 1) bad = "candidate_count";
        if (!*bad && c.osc != has_osc) bad = "candidate_kind";
        if (!*bad) {
            c.cand_name = c.osc ? "candidate.osc" : "candidate.opir";
            const struct stat *cs = c.osc ? &so : &sp;
            if (!S_ISREG(cs->st_mode)) bad = "candidate_not_regular";
            else { c.cand_len = (size_t)cs->st_size; hash_file_at(cfd, c.cand_name, c.sha_cand, NULL); }
        }
        if (!*bad) bad = parse_inputs((char *)inp, &c);
        if (*bad) snprintf(det, sizeof det, "%s", bad);
        else if (c.cand_len > MAX_CAND) { verdict = "REJECT"; reason = "oversize"; snprintf(det, sizeof det, "bytes=%zu", c.cand_len); } /* OLA:oversize */
        else {
            /* the candidate runs BEFORE hidden/expected is even read */
            size_t inb = 4 + (size_t)c.n * c.np * 8;
            uint8_t *ib = malloc(inb);
            if (!ib) die("out of memory");
            ib[0] = (uint8_t)c.n; ib[1] = (uint8_t)(c.n >> 8); ib[2] = (uint8_t)(c.n >> 16); ib[3] = (uint8_t)(c.n >> 24);
            for (size_t k = 0; k < (size_t)c.n * c.np; k++)
                for (int b = 0; b < 8; b++) ib[4 + 8 * k + b] = (uint8_t)(c.in[k] >> (8 * b));
            int cand = openat(cfd, c.cand_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (cand < 0) die("open candidate");
            char nonce[33], scratch[4096];
            new_nonce(nonce);
            fresh_scratch(argv[5], "case", scratch, sizeof scratch);
            char *av[16];
            int ac = 0;
            char wbuf[8];
            av[ac++] = (char *)"ola_runner";
            av[ac++] = (char *)(c.osc ? "osc" : "omegaprog");
            av[ac++] = nonce;
            av[ac++] = scratch;
            if (c.osc) {
                av[ac++] = c.entry;
                av[ac++] = (char *)kname[c.ret];
                for (unsigned k = 0; k < c.np; k++) av[ac++] = (char *)kname[c.params[k]];
            } else {
                snprintf(wbuf, sizeof wbuf, "%u", c.width);
                av[ac++] = wbuf;
                av[ac++] = c.pid;
            }
            av[ac] = NULL;
            Limits lim = {m.time_ms, m.mem_mb, 1};
            Run r;
            spawn(runner, av, cand, ib, inb, &lim, &r);
            close(cand);
            free(ib);
            exp = slurp_at(cfd, "hidden/expected", MAX_FILE, &en);
            want = calloc(MAX_LINES + 1, sizeof *want);
            if (!want) die("out of memory");
            const char *eb = !exp ? "missing_expected" : !ascii_ok(exp, en) ? "not_ascii" : parse_expected((char *)exp, &c, want);
            if (*eb) snprintf(det, sizeof det, "%s", eb);
            else {
                reason = judge(&c, want, &r, nonce, det, sizeof det);
                verdict = !strcmp(reason, "ok") ? "ACCEPT" : "REJECT";
            }
            run_free(&r);
        }
        for (char *p = det; *p; p++) if ((unsigned char)*p <= ' ' || *p == '=') *p = '_';
        char body[2048];
        int bn = snprintf(body, sizeof body,
                          "schema=omega.one_layer.verdict.v1\ncommit=%s\nharness_sha256=%s\nrules_sha256=%s\ncorpus_sha256=%s\n"
                          "case=%s\nkind=%s\ncandidate_sha256=%s\nrequirement_sha256=%s\ninputs_sha256=%s\nexpected_sha256=%s\n"
                          "verdict=%s\nreason=%s\ndetail=%s\n",
                          m.commit, m.harness, m.rules, m.corpus, c.id, kinds,
                          c.sha_cand, c.sha_req, c.sha_in, c.sha_exp, verdict, reason, det);
        if (bn <= 0 || (size_t)bn >= sizeof body) die("receipt too long");
        char rh[65];
        put_receipt(rdir, body, (size_t)bn, rh);
        fprintf(vf, "%s %s %s %s\n", c.id, verdict, reason, rh);
        fprintf(stderr, "  %-40s %s %s %s\n", c.id, verdict, reason, det);
        free(req); free(inp); free(exp); free(want); free(c.in);
        close(cfd);
        free(ids[ci]);
    }
    if (fflush(vf) || fsync(fileno(vf)) || fclose(vf)) die("verdicts write");
    close(rdir);
    return 0;
}

/* ===================================================================================== probes */

typedef struct { const char *name; const char *target_key; } Probe;

static const Probe k_probes[] = {
    {"read_hidden_inputs", "hidden_inputs"}, {"read_expected", "expected"}, {"read_labels", "labels"},
    {"read_corpus_tar", "tar"}, {"read_key", "key"}, {"write_evidence", "evidence"}, {"write_scorer", "scorer"},
    {"change_rules", "rules"}, {"network", NULL}, {"outlive_time", NULL}, {"outlive_fork", NULL},
    {"exceed_memory", NULL}, {"write_outside_scratch", "outside"}, {"replay_verdict", NULL},
};

/* Outcome of one probe execution: "ESCAPED:<what>" or "BLOCKED:<how>". */
static void classify(const Run *r, const char *nonce_or_null, char *out, size_t n)
{
    if (r->timed_out || (r->signaled && (r->sig == SIGXCPU || r->sig == SIGKILL))) { snprintf(out, n, "BLOCKED:time_limit"); return; }
    if ((r->exited && r->code == OLA_EXIT_SANDBOX) || (r->signaled && r->sig == SIGSYS)) {
        const char *s = strstr(r->err, "syscall=");
        snprintf(out, n, "BLOCKED:seccomp_syscall=%d", s ? atoi(s + 8) : -1);
        return;
    }
    if (nonce_or_null) { /* replay: the transcript check is the defence */
        char hdr[64];
        snprintf(hdr, sizeof hdr, "OLA1 %s\n", nonce_or_null);
        if (strncmp(r->out, hdr, strlen(hdr))) { snprintf(out, n, "BLOCKED:stale_or_forged_transcript"); return; }
        snprintf(out, n, "ESCAPED:transcript_accepted");
        return;
    }
    if (r->signaled) { snprintf(out, n, "BLOCKED:signal=%d", r->sig); return; }
    if (!strncmp(r->out, "ESCAPED ", 8)) {
        snprintf(out, n, "ESCAPED:%.*s", (int)strcspn(r->out + 8, "\n"), r->out + 8);
        return;
    }
    if (!strncmp(r->out, "BLOCKED ", 8)) {
        snprintf(out, n, "BLOCKED:%.*s", (int)strcspn(r->out + 8, "\n"), r->out + 8);
        for (char *p = out; *p; p++) if (*p == ' ') *p = '_';
        return;
    }
    snprintf(out, n, "UNKNOWN:exit=%d", r->exited ? r->code : -1);
}

static int cmd_probes(int argc, char **argv)
{
    if (argc < 7) die("usage: ola probes <ola_runner> <scratch_root> <probes.log> <time_ms> <mem_mb> key=path...");
    const char *runner = argv[2], *sroot = argv[3];
    unsigned time_ms = (unsigned)atoi(argv[5]), mem_mb = (unsigned)atoi(argv[6]);
    const char *only = getenv("OLA_PROBE_ONLY"); /* selftest: run a subset */
    FILE *log = fopen(argv[4], "wx");
    if (!log) die("probes log %s exists or cannot be created", argv[4]);
    /* a local listener so the network probe's control has something real to reach */
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8) || getsockname(ls, (struct sockaddr *)&a, &al))
        die("probe listener: %s", strerror(errno));
    char port[16], tbuf[16], mbuf[16];
    snprintf(port, sizeof port, "%u", (unsigned)ntohs(a.sin_port));
    snprintf(tbuf, sizeof tbuf, "%.3f", time_ms / 1000.0);
    snprintf(mbuf, sizeof mbuf, "%u", mem_mb);
    int all = 1, nrun = 0;
    /* positive control inside the sandbox: scratch must stay writable, else "blocked" means nothing */
    for (size_t pi = 0; pi < sizeof k_probes / sizeof k_probes[0] + 1; pi++) {
        int is_pos = pi == sizeof k_probes / sizeof k_probes[0];
        const char *name = is_pos ? "scratch_ok" : k_probes[pi].name;
        if (only && !strstr(only, name)) continue;
        const char *target = "";
        if (!is_pos && k_probes[pi].target_key) {
            size_t kl = strlen(k_probes[pi].target_key);
            for (int i = 7; i < argc; i++)
                if (!strncmp(argv[i], k_probes[pi].target_key, kl) && argv[i][kl] == '=') target = argv[i] + kl + 1;
            if (!*target) die("probe %s: no target %s=", name, k_probes[pi].target_key);
        }
        if (!strcmp(name, "network")) target = port;
        if (!strcmp(name, "outlive_time")) target = tbuf;
        if (!strcmp(name, "exceed_memory")) target = mbuf;
        char res[2][160];
        for (int control = 0; control < 2; control++) {
            char nonce[33], stale[33], scratch[4096];
            new_nonce(nonce);
            new_nonce(stale); /* the "recorded earlier" nonce a replayed transcript carries */
            fresh_scratch(sroot, "probe", scratch, sizeof scratch);
            if (!strcmp(name, "replay_verdict")) target = stale;
            char *av[] = {(char *)"ola_runner", (char *)"probe", nonce, scratch, (char *)(control ? "1" : "0"),
                          (char *)name, (char *)target, NULL};
            Limits lim = {time_ms, mem_mb, !control};
            Run r;
            spawn(runner, av, -1, "\0\0\0\0", 4, &lim, &r);
            /* replay control: the defence removed = the transcript is checked against the nonce it carries */
            const char *chk = !strcmp(name, "replay_verdict") ? (control ? stale : nonce) : NULL;
            classify(&r, chk, res[control], sizeof res[control]);
            run_free(&r);
            if (!strcmp(name, "write_outside_scratch")) unlink(target); /* the control really created it */
        }
        int pass = is_pos ? !strncmp(res[0], "ESCAPED:", 8) && !strncmp(res[1], "ESCAPED:", 8)
                          : !strncmp(res[0], "BLOCKED:", 8) && !strncmp(res[1], "ESCAPED:", 8); /* OLA:probe-verdict */
        fprintf(log, "probe=%s expect=%s sandboxed=%s control=%s result=%s\n", name, is_pos ? "ALLOWED" : "BLOCKED",
                res[0], res[1], pass ? "PASS" : "FAIL");
        fprintf(stderr, "  probe %-22s sandboxed=%-40s control=%-28s %s\n", name, res[0], res[1], pass ? "PASS" : "FAIL");
        all &= pass;
        nrun++;
    }
    close(ls);
    fprintf(log, "PROBES %s count=%d\n", all && nrun ? "PASS" : "FAIL", nrun);
    if (fclose(log)) die("probes log write");
    return all && nrun ? 0 : 1;
}

/* ====================================================================================== tally */

static int cmd_tally(int argc, char **argv)
{
    if (argc != 7) die("usage: ola tally <verdicts> <LABELS> <replay_identical> <probes_pass> <clean>");
    size_t vn, ln;
    char *v = (char *)slurp(argv[2], MAX_FILE, &vn), *l = (char *)slurp(argv[3], MAX_FILE, &ln);
    if (!v || !l) die("tally: cannot read verdicts or LABELS");
    char *ids[1024], *lab[1024];
    size_t nl = 0;
    char *cur = l, *line;
    while ((line = next_line(&cur))) {
        char id[64], cls[16], ac[64];
        if (sscanf(line, "%63s %15s %63s", id, cls, ac) != 3 || (strcmp(cls, "DEFECT") && strcmp(cls, "CONTROL"))) die("tally: bad LABELS line");
        if (nl == 1024) die("tally: too many labels");
        for (size_t i = 0; i < nl; i++) if (!strcmp(ids[i], id)) die("tally: duplicate label %s", id);
        ids[nl] = strdup(id); lab[nl] = strdup(cls); nl++;
    }
    unsigned defects = 0, controls = 0, drej = 0, cacc = 0, errors = 0, seen = 0, unlabelled = 0;
    for (size_t i = 0; i < nl; i++) { if (!strcmp(lab[i], "DEFECT")) defects++; else controls++; }
    cur = v;
    while ((line = next_line(&cur))) {
        char id[64], verdict[16], reason[40], rh[80];
        if (sscanf(line, "%63s %15s %39s %79s", id, verdict, reason, rh) != 4) die("tally: bad verdict line");
        if (!strcmp(verdict, "ERROR")) errors++;
        size_t i;
        for (i = 0; i < nl; i++) if (!strcmp(ids[i], id)) break;
        if (i == nl) { unlabelled++; continue; }
        seen++;
        if (!strcmp(lab[i], "DEFECT") && !strcmp(verdict, "REJECT")) drej++; /* ERROR never counts as a rejection */
        if (!strcmp(lab[i], "CONTROL") && !strcmp(verdict, "ACCEPT")) cacc++;
    }
    int replay = !strcmp(argv[4], "1"), probes = !strcmp(argv[5], "1"), clean = !strcmp(argv[6], "1");
    int gate = defects == 30 && controls == 10 && drej == 30 && cacc == 10 && errors == 0 && seen == nl && !unlabelled &&
               replay && probes && clean; /* OLA:gate */
    printf("defects_rejected=%u/30 controls_accepted=%u/10 errors=%u\n", drej, cacc, errors);
    printf("labelled_defects=%u labelled_controls=%u verdicts_matched=%u unlabelled_verdicts=%u\n", defects, controls, seen, unlabelled);
    printf("replay_identical=%s probes=%s clean_tree=%s\n", replay ? "yes" : "no", probes ? "PASS" : "FAIL", clean ? "yes" : "no");
    printf("GATE %s\n", gate ? "PASS" : "FAIL");
    return 0;
}

static int cmd_cmp(int argc, char **argv)
{
    if (argc != 4) die("usage: ola cmp <a> <b>");
    size_t an, bn;
    uint8_t *a = slurp(argv[2], MAX_TAR, &an), *b = slurp(argv[3], MAX_TAR, &bn);
    return a && b && an == bn && !memcmp(a, b, an) ? 0 : 1;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) die("usage: ola untar|pass|probes|tally|cmp ...");
    if (!strcmp(argv[1], "untar")) return cmd_untar(argc, argv);
    if (!strcmp(argv[1], "pass")) return cmd_pass(argc, argv);
    if (!strcmp(argv[1], "probes")) return cmd_probes(argc, argv);
    if (!strcmp(argv[1], "tally")) return cmd_tally(argc, argv);
    if (!strcmp(argv[1], "cmp")) return cmd_cmp(argc, argv);
    die("unknown command %s", argv[1]);
    return 2;
}
