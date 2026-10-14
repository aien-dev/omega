/*
 * ola_runner.c -- the sandboxed half of the one-layer stage-1 acceptance harness (omega#443).
 *
 * The trusted parent (ola.c) fork+execs this program for every candidate and every isolation probe,
 * with rlimits already set (CPU, address space, file size, core) and only these descriptors open:
 *   fd 0  inputs: u32 LE line count n, then n * nparams u64 LE canonical arguments
 *   fd 1  transcript (a memfd the parent reads after exit)
 *   fd 2  diagnostics (a memfd)
 *   fd 3  candidate bytes (osc / oscir / omegaprog only)
 * It never receives a path to hidden/, LABELS, the corpus tarball, the evidence tree or a key.
 *
 *   ola_runner osc       <nonce> <scratch> <entry> <ret> <param kinds...>
 *   ola_runner oscir     <nonce> <scratch> <entry> <ret> <param kinds...>   (v2: OSC binary IR bytes, osc_ir_decode)
 *   ola_runner omegaprog <nonce> <scratch> <width> <program_id hex | ->
 *   ola_runner probe     <nonce> <scratch> <control 0|1> <name> [target]
 *
 * Sandbox (enter_sandbox): no_new_privs, Landlock (no filesystem access except the scratch
 * directory; no TCP bind/connect where the kernel ABI has it; scoped signals/abstract sockets where
 * it has that), then a seccomp allow-list whose default action traps: the SIGSYS handler writes
 * "SANDBOX syscall=<nr>" to fd 2 and exits OLA_EXIT_SANDBOX. Decoding, compiling and executing the
 * candidate all happen after the sandbox is entered (the compiler sees hostile bytes).
 * A probe with control=1 skips the sandbox: the negative control that proves the probe notices.
 *
 * Transcript: "OLA1 <nonce>", then either "REFUSE <reason> <detail>" or one line per input
 * ("R <decimal u64>" = returned value, raw 64 bits; "T <code>" = OSC trap), then "END <nonce> <n>".
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_native.h"
#include "omega_program.h"
#include "omega_program_ir.h"

#define OLA_EXIT_SANDBOX 97   /* a syscall outside the allow-list was attempted */
#define OLA_EXIT_MEMORY 98    /* an allocation failed (RLIMIT_AS) */
#define OLA_EXIT_UNAVAIL 96   /* the sandbox could not be built: the parent fails closed */
#define OLA_EXIT_USAGE 95

/* The seccomp allow-list below uses aarch64 syscall numbers and admits only this audit arch. */
#define OLA_FILTER_ARCH AUDIT_ARCH_AARCH64 /* OLA:filter-arch */
#if defined(__aarch64__)
#define OLA_BUILD_ARCH AUDIT_ARCH_AARCH64
#elif defined(__x86_64__)
#define OLA_BUILD_ARCH AUDIT_ARCH_X86_64
#else
#define OLA_BUILD_ARCH 0u
#endif

static const char *g_nonce = "-";
static int g_header; /* "OLA1 <nonce>" already printed */

static void sigsys_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    char b[64] = "SANDBOX syscall=";
    size_t n = strlen(b);
    unsigned v = (unsigned)si->si_syscall;
    char d[12]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    while (k) b[n++] = d[--k];
    b[n++] = '\n';
    ssize_t w = write(2, b, n);
    (void)w;
    _exit(OLA_EXIT_SANDBOX);
}

static int landlock_abi(void)
{
    return (int)syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
}

/* Filesystem rights Landlock knows at each ABI (linux/landlock.h). */
static uint64_t fs_rights(int abi)
{
    uint64_t r = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
                 LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                 LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                 LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                 LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) r |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) r |= LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) r |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    return r;
}

static int enter_landlock(const char *scratch)
{
    int abi = landlock_abi();
    if (abi < 1) { dprintf(2, "SANDBOX_UNAVAILABLE landlock abi=%d errno=%d\n", abi, errno); return -1; }
    struct landlock_ruleset_attr ra;
    memset(&ra, 0, sizeof ra);
    size_t rsz = offsetof(struct landlock_ruleset_attr, handled_access_net);
    ra.handled_access_fs = fs_rights(abi);
    if (abi >= 4) { /* OLA:landlock-net */
        ra.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
        rsz = offsetof(struct landlock_ruleset_attr, handled_access_net) + sizeof ra.handled_access_net;
    }
#ifdef LANDLOCK_SCOPE_SIGNAL
    if (abi >= 6) {
        ra.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
        rsz = sizeof ra;
    }
#endif
    int rs = (int)syscall(SYS_landlock_create_ruleset, &ra, rsz, 0);
    if (rs < 0) { dprintf(2, "SANDBOX_UNAVAILABLE landlock_create_ruleset errno=%d\n", errno); return -1; }
    int dfd = open(scratch, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) { dprintf(2, "SANDBOX_UNAVAILABLE scratch errno=%d\n", errno); return -1; }
    struct landlock_path_beneath_attr pb = {
        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                          LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                          (abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0),
        .parent_fd = dfd,
    };
    if (syscall(SYS_landlock_add_rule, rs, LANDLOCK_RULE_PATH_BENEATH, &pb, 0)) {
        dprintf(2, "SANDBOX_UNAVAILABLE landlock_add_rule errno=%d\n", errno);
        return -1;
    }
    close(dfd);
    if (syscall(SYS_landlock_restrict_self, rs, 0)) { /* OLA:landlock-restrict */
        dprintf(2, "SANDBOX_UNAVAILABLE landlock_restrict_self errno=%d\n", errno);
        return -1;
    }
    close(rs);
    return 0;
}

#define ALLOW(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int enter_seccomp(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSYS, &sa, NULL)) return -1;
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, OLA_FILTER_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        ALLOW(__NR_read), ALLOW(__NR_write), ALLOW(__NR_close), ALLOW(__NR_lseek),
        ALLOW(__NR_fstat), ALLOW(__NR_newfstatat), ALLOW(__NR_openat),
        ALLOW(__NR_mmap), ALLOW(__NR_munmap), ALLOW(__NR_mprotect), ALLOW(__NR_mremap), ALLOW(__NR_brk),
        ALLOW(__NR_madvise), ALLOW(__NR_getrandom),
        ALLOW(__NR_rt_sigreturn), ALLOW(__NR_rt_sigprocmask), ALLOW(__NR_clock_gettime),
        ALLOW(__NR_exit), ALLOW(__NR_exit_group),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP), /* OLA:seccomp-default */
    };
    struct sock_fprog prog = { .len = (unsigned short)(sizeof f / sizeof f[0]), .filter = f };
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)) {
        dprintf(2, "SANDBOX_UNAVAILABLE seccomp errno=%d\n", errno);
        return -1;
    }
    return 0;
}

static void enter_sandbox(const char *scratch)
{
    /* A machine the allow-list was not written for would have every syscall killed and every case
     * reported as a sandbox violation: refuse the whole run instead (the parent fails closed). */
    struct utsname un;
    if (OLA_BUILD_ARCH != OLA_FILTER_ARCH || uname(&un) || strcmp(un.machine, "aarch64")) { /* OLA:arch-check */
        dprintf(2, "SANDBOX_UNAVAILABLE arch: the seccomp allow-list is aarch64-only; this machine or build is not aarch64\n");
        _exit(OLA_EXIT_UNAVAIL);
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) { dprintf(2, "SANDBOX_UNAVAILABLE no_new_privs\n"); _exit(OLA_EXIT_UNAVAIL); }
    if (enter_landlock(scratch)) _exit(OLA_EXIT_UNAVAIL); /* OLA:enter-landlock */
    if (enter_seccomp()) _exit(OLA_EXIT_UNAVAIL);          /* OLA:enter-seccomp */
}

/* ---- inputs and candidate ----------------------------------------------------------------- */

static uint8_t *read_all(int fd, size_t max, size_t *len)
{
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap);
    if (!b) return NULL;
    for (;;) {
        if (n == cap) {
            if (cap > max) { free(b); errno = EFBIG; return NULL; }
            uint8_t *nb = realloc(b, cap * 2);
            if (!nb) { free(b); return NULL; }
            b = nb; cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; free(b); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    if (n > max) { free(b); errno = EFBIG; return NULL; }
    *len = n;
    return b;
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

static void nomem(const char *where)
{
    printf("MEM %s\n", where);
    fflush(stdout);
    _exit(OLA_EXIT_MEMORY);
}

static const uint64_t *load_inputs(unsigned nparams, uint32_t *nlines)
{
    size_t len = 0;
    uint8_t *b = read_all(0, (size_t)4 + 4096u * 6u * 8u, &len);
    if (!b) nomem("inputs");
    if (len < 4) { dprintf(2, "inputs: short\n"); _exit(OLA_EXIT_USAGE); }
    uint32_t n = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    if (n < 1 || n > 4096 || len != 4 + (size_t)n * nparams * 8) { dprintf(2, "inputs: bad length\n"); _exit(OLA_EXIT_USAGE); }
    uint64_t *v = malloc(sizeof *v * ((size_t)n * nparams + 1));
    if (!v) nomem("inputs");
    for (size_t i = 0; i < (size_t)n * nparams; i++) v[i] = le64(b + 4 + 8 * i);
    free(b);
    *nlines = n;
    return v;
}

static void refuse(const char *reason, const char *detail)
{
    if (!g_header) printf("OLA1 %s\n", g_nonce);
    printf("REFUSE %s %s\nEND %s 0\n", reason, detail, g_nonce);
    fflush(stdout);
    _exit(0);
}

/* Detail text is one token: no spaces or control bytes (receipts are key=value lines). */
static void token(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 127) *s = '_';
}

static int kind_of(const char *s)
{
    static const char *const nm[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
    static const int k[] = {OSC_T_BOOL, OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_U64, OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64};
    for (int i = 0; i < 9; i++) if (!strcmp(s, nm[i])) return k[i];
    return -1;
}

static int run_osc(int argc, char **argv, int ir)
{
    /* argv: osc|oscir nonce scratch entry ret k... (oscir: the candidate is OSC binary IR bytes, v2) */
    if (argc < 6 || argc > 12) return OLA_EXIT_USAGE;
    const char *entry = argv[4];
    int ret_kind = kind_of(argv[5]);
    unsigned np = (unsigned)(argc - 6);
    int pk[6];
    if (ret_kind < 0) return OLA_EXIT_USAGE;
    for (unsigned i = 0; i < np; i++) if ((pk[i] = kind_of(argv[6 + i])) < 0) return OLA_EXIT_USAGE;
    uint32_t n = 0;
    const uint64_t *in = load_inputs(np, &n);
    size_t slen = 0;
    uint8_t *src = read_all(3, 1u << 20, &slen);
    if (!src) refuse("oversize", "candidate");
    OscUnit *u = malloc(sizeof *u);
    OscRt *rt = malloc(sizeof *rt);
    if (!u || !rt) nomem("unit");
    enter_sandbox(argv[3]);
    printf("OLA1 %s\n", g_nonce);
    g_header = 1;
    if (ir) { /* v2 kind oscir: the strict reader, inside the sandbox; then exactly the osc path */
        int rc = osc_ir_decode(src, slen, u);
        if (rc) { /* OLA:oscir-decode */
            char det[64];
            snprintf(det, sizeof det, "class=%s", osc_ir_refusal_name(rc));
            refuse("decode_refused", det);
        }
    } else {
        OscDiag d;
        memset(&d, 0, sizeof d);
        if (osc_compile((const char *)src, slen, u, &d, NULL)) {
            char det[96];
            snprintf(det, sizeof det, "kind=%s,line=%u", osc_diag_kind_name(d.kind), d.line);
            token(det);
            refuse("compile_refused", det);
        }
    }
    int fi = -1;
    for (unsigned i = 0; i < u->nfuncs; i++)
        if (!strcmp(u->funcs[i].name, entry)) { fi = (int)i; break; }
    if (fi < 0) refuse("signature_mismatch", "no_entry_function");
    const OscFunc *fn = &u->funcs[fi];
    if (fn->nparams != np) refuse("signature_mismatch", "param_count");
    for (unsigned i = 0; i < np; i++)
        if ((int)fn->vtype[i].s != pk[i] || fn->vtype[i].ref != OSC_REF_NONE) refuse("signature_mismatch", "param_kind");
    if ((int)fn->ret.s != ret_kind || fn->ret.ref != OSC_REF_NONE) refuse("signature_mismatch", "ret_kind");
    OscCode code;
    memset(&code, 0, sizeof code);
    char err[160];
    if (osc_cg_compile(u, &code, err, sizeof err)) { token(err); refuse("compile_refused", "backend"); }
    OscNative nm;
    if (osc_native_map(&nm, code.code, code.len)) refuse("realize_refused", "native_map");
    osc_rt_init(rt);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t r = 0;
        osc_rt_reset(rt);
        int t = osc_native_call(u, fi, &nm, code.entry[fi], rt, in + (size_t)i * np, np, &r);
        if (t < 0) refuse("realize_refused", "native_call_refused");
        if (t > 0) printf("T %d\n", t);
        else printf("R %" PRIu64 "\n", r);
    }
    printf("END %s %" PRIu32 "\n", g_nonce, n);
    fflush(stdout);
    return 0;
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int run_omegaprog(int argc, char **argv)
{
    /* argv: omegaprog nonce scratch width program_id|- */
    if (argc != 6) return OLA_EXIT_USAGE;
    unsigned w = (unsigned)atoi(argv[4]);
    if (w != 8 && w != 16 && w != 32 && w != 64) return OLA_EXIT_USAGE;
    uint8_t want_id[32];
    int has_id = strcmp(argv[5], "-") != 0;
    if (has_id) {
        if (strlen(argv[5]) != 64) return OLA_EXIT_USAGE;
        for (int i = 0; i < 32; i++) {
            int a = hexval(argv[5][2 * i]), b = hexval(argv[5][2 * i + 1]);
            if (a < 0 || b < 0) return OLA_EXIT_USAGE;
            want_id[i] = (uint8_t)(a << 4 | b);
        }
    }
    uint32_t n = 0;
    const uint64_t *in = load_inputs(1, &n);
    size_t blen = 0;
    uint8_t *bytes = read_all(3, 1u << 20, &blen);
    if (!bytes) refuse("oversize", "candidate");
    OmegaProgram *p = malloc(sizeof *p);
    if (!p) nomem("program");
    enter_sandbox(argv[3]);
    printf("OLA1 %s\n", g_nonce);
    g_header = 1;
    if (omega_program_ir_decode(bytes, blen, p)) refuse("decode_refused", "omega_program_ir_decode");
    if (p->contract.input_type != TYPE_UNSIGNED_INT || p->contract.output_type != TYPE_UNSIGNED_INT ||
        p->contract.input_width != w || p->contract.output_width != w)
        refuse("signature_mismatch", "type_or_width");
    if (has_id && memcmp(p->program_id.bytes, want_id, 32)) refuse("signature_mismatch", "program_id");
    char why[160] = "";
    if (omega_program_realize_ex(p, why, sizeof why)) { token(why); refuse("realize_refused", why[0] ? why : "-"); }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t y = 0;
        if (omega_program_exec(p, in[i], &y)) refuse("realize_refused", "exec_refused");
        printf("R %" PRIu64 "\n", y);
    }
    printf("END %s %" PRIu32 "\n", g_nonce, n);
    fflush(stdout);
    return 0;
}

/* ---- isolation probes ------------------------------------------------------------------------
 * Each probe attempts one forbidden action and reports what happened:
 *   ESCAPED <what>      the action succeeded (the sandbox did not stop it)
 *   BLOCKED errno=<e>   the kernel refused it
 * or it dies: seccomp (exit 97 "SANDBOX syscall=..."), time limit, memory limit. The parent
 * classifies. scratch_ok is the positive control inside the sandbox (must be ALLOWED). */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int probe_open(const char *path, int flags, const char *what)
{
    int fd = open(path, flags | O_CLOEXEC, 0600);
    if (fd < 0) { printf("BLOCKED errno=%d\n", errno); return 0; }
    if ((flags & O_ACCMODE) == O_RDONLY) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) { printf("BLOCKED errno=%d read\n", errno); return 0; }
    }
    printf("ESCAPED %s\n", what); /* OLA:probe-report */
    close(fd);
    return 0;
}

static int run_probe(int argc, char **argv)
{
    /* argv: probe nonce scratch control name [target] */
    if (argc < 6) return OLA_EXIT_USAGE;
    const char *scratch = argv[3], *name = argv[5], *target = argc > 6 ? argv[6] : "";
    int control = !strcmp(argv[4], "1");
    if (!control) enter_sandbox(scratch); /* OLA:probe-sandbox */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(name, "read_hidden_inputs") || !strcmp(name, "read_expected") || !strcmp(name, "read_labels") ||
        !strcmp(name, "read_corpus_tar") || !strcmp(name, "read_key"))
        return probe_open(target, O_RDONLY, "read");
    if (!strcmp(name, "write_evidence") || !strcmp(name, "write_scorer") || !strcmp(name, "change_rules"))
        return probe_open(target, O_WRONLY, "opened_for_write"); /* no write, no truncation: nothing changes */
    if (!strcmp(name, "write_outside_scratch")) return probe_open(target, O_WRONLY | O_CREAT | O_EXCL, "created");
    if (!strcmp(name, "scratch_ok")) {
        char p[4096];
        snprintf(p, sizeof p, "%s/scratch_ok", scratch);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) { printf("BLOCKED errno=%d\n", errno); return 0; }
        ssize_t w = write(fd, "ok\n", 3);
        close(fd);
        if (w != 3) { printf("BLOCKED errno=%d write\n", errno); return 0; }
        printf("ESCAPED wrote_scratch\n");
        return 0;
    }
    if (!strcmp(name, "network")) {
        int port = atoi(target);
        int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) { printf("BLOCKED errno=%d socket\n", errno); return 0; }
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof a)) { printf("BLOCKED errno=%d connect\n", errno); return 0; }
        printf("ESCAPED connected\n");
        return 0;
    }
    if (!strcmp(name, "outlive_time")) {
        double limit = atof(target), t0 = now_s();
        volatile uint64_t x = 0;
        while (now_s() - t0 < limit + 2.0) x++;
        printf("ESCAPED outlived\n");
        return 0;
    }
    if (!strcmp(name, "outlive_fork")) {
        pid_t c = fork();
        if (c < 0) { printf("BLOCKED errno=%d\n", errno); return 0; }
        if (c == 0) _exit(0);
        waitpid(c, NULL, 0);
        printf("ESCAPED forked\n");
        return 0;
    }
    if (!strcmp(name, "exceed_memory")) {
        size_t mb = (size_t)atol(target);
        void *m = mmap(NULL, mb * 2u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) { printf("BLOCKED errno=%d\n", errno); return 0; }
        munmap(m, mb * 2u << 20);
        printf("ESCAPED mapped\n");
        return 0;
    }
    if (!strcmp(name, "replay_verdict")) {
        /* A replayed transcript: framed with a nonce recorded from an earlier execution (target). */
        printf("OLA1 %s\nR 0\nEND %s 1\n", target, target);
        return 0;
    }
    return OLA_EXIT_USAGE;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: ola_runner osc|omegaprog|probe <nonce> <scratch> ...\n"); return OLA_EXIT_USAGE; }
    g_nonce = argv[2];
    if (!strcmp(argv[1], "osc")) return run_osc(argc, argv, 0);
    if (!strcmp(argv[1], "oscir")) return run_osc(argc, argv, 1); /* v2 */
    if (!strcmp(argv[1], "omegaprog")) return run_omegaprog(argc, argv);
    if (!strcmp(argv[1], "probe")) return run_probe(argc, argv);
    return OLA_EXIT_USAGE;
}
