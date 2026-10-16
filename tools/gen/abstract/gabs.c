/*
 * gabs.c -- C helpers of the GEN-3 abstraction extractor (tools/gen/abstract/extract, verify; aien-spark/omega#478).
 * Deterministic, no clock, no randomness. Only the corpus files and the repo's own tools' outputs are read.
 *
 *   gabs split <solution.omegese> <out prefix>
 *       splits a program into its functions: writes <prefix>.<k>.omegese (k = 0, 1, ... in program order) and
 *       prints one line per function "fn <k> <name> <calls> <nlines>", <calls> = the earlier functions its body
 *       names, comma-separated, or "-". Refuses blank lines, tabs, CR and text outside a function.
 *   gabs canon <in.json> <out.json>
 *       the omegese op list with every function name replaced by fn<k>_ (k in program order) in the "name"
 *       and "func" keys (RECORD_SCHEMA.md section 6)
 *   gabs inputs <count> <type>...
 *       <count> deterministic input lines for parameters of these IR types (bool u8 .. i64): boundary values
 *       first, then a fixed 64-bit LCG mixed over each type's range (small values weighted in)
 *   gabs run <ola_runner> <scratch dir> <ir file> <entry> <ret> <inputs> <expected|-> [<param type>...]
 *       runs the IR in the harness's sandboxed runner (tests/one_layer/acceptance/ola_runner, called, not
 *       copied) on <inputs> and compares with <expected> (decimal per line, "TRAP" or "TRAP <code>"), the
 *       way ola.c compares (values normalised at the return type's width). Prints one line:
 *       "ACCEPT n=<lines>" (exit 0) or "REJECT <reason> [line=<k> got=<x> want=<y>]" (exit 1).
 *       With expected "-" it prints the observed result per line instead ("<value>" or "TRAP <code>").
 * Exit 2: usage or I/O error.
 *
 * Copied, because ola.c is a program, not a library (its main is static and it scores whole cases): the
 * value grammar (strict decimal, parse_val) and the width normalisation (at_width), about 30 lines.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *m, const char *a)
{
    fprintf(stderr, "gabs: %s%s%s\n", m, a ? " " : "", a ? a : "");
    exit(2);
}

static char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0, r;
    char *b = malloc(cap + 1);
    while (b && (r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) {
            if (cap >= (16u << 20)) { free(b); b = NULL; break; }
            char *nb = realloc(b, (cap *= 2) + 1);
            if (!nb) { free(b); b = NULL; break; }
            b = nb;
        }
    }
    fclose(f);
    if (b) { b[len] = 0; *n = len; }
    return b;
}

/* ---- split ---------------------------------------------------------------------------------------------- */
#define MAXF 32
static int split(const char *in, const char *prefix)
{
    size_t n;
    char *s = slurp(in, &n);
    if (!s) die("cannot read", in);
    if (!n || s[n - 1] != '\n') die("program does not end with a newline:", in);
    char *names[MAXF];
    int start[MAXF], nf = 0, line = 0, open = 0;
    char *lines[4096];
    int nl = 0;
    for (char *p = s; *p;) {
        char *e = strchr(p, '\n');
        *e = 0;
        if (nl == 4096) die("too many lines", in);
        for (char *q = p; *q; q++) if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7e) die("byte outside 0x20..0x7E in", in);
        if (!*p) die("blank line in", in);
        lines[nl++] = p;
        p = e + 1;
    }
    for (line = 0; line < nl; line++) {
        char *l = lines[line];
        if (!strncmp(l, "fn ", 3)) {
            if (open) die("fn inside a function in", in);
            if (nf == MAXF) die("too many functions in", in);
            char nm[64];
            if (sscanf(l + 3, "%63s", nm) != 1) die("fn without a name in", in);
            names[nf] = strdup(nm);
            start[nf++] = line;
            open = 1;
        } else if (!open) die("text outside a function in", in);
        /* a function ends at the `end` that closes it: track block depth */
        if (!strncmp(l, "fn ", 3)) { open = 1; continue; }
        if (!strncmp(l, "if ", 3) || !strncmp(l, "while ", 6) || !strncmp(l, "for ", 4)) open++;
        else if (!strcmp(l, "end")) open--;
    }
    if (open) die("unclosed function in", in);
    for (int k = 0; k < nf; k++) {
        int a = start[k], b = k + 1 < nf ? start[k + 1] : nl;
        char path[4096];
        snprintf(path, sizeof path, "%s.%d.omegese", prefix, k);
        FILE *f = fopen(path, "wb");
        if (!f) die("cannot write", path);
        char calls[1024] = "";
        for (int i = a; i < b; i++) {
            fprintf(f, "%s\n", lines[i]);
            if (i == a) continue;
            /* words of the line (brackets are separate words) naming an earlier function */
            char tmp[1100];
            snprintf(tmp, sizeof tmp, "%s", lines[i]);
            for (char *q = tmp; *q; q++) if (*q == '(' || *q == ')') *q = ' ';
            for (char *w = strtok(tmp, " "); w; w = strtok(NULL, " "))
                for (int j = 0; j < k; j++)
                    if (!strcmp(w, names[j])) {
                        char pat[80];
                        snprintf(pat, sizeof pat, ",%s,", w);
                        char have[1100];
                        snprintf(have, sizeof have, ",%s,", calls);
                        if (!strstr(have, pat)) {
                            if (calls[0]) strncat(calls, ",", sizeof calls - strlen(calls) - 1);
                            strncat(calls, w, sizeof calls - strlen(calls) - 1);
                        }
                    }
        }
        if (fclose(f)) die("cannot write", path);
        printf("fn %d %s %s %d\n", k, names[k], calls[0] ? calls : "-", b - a);
    }
    return 0;
}

/* ---- canon ---------------------------------------------------------------------------------------------- */
static int canon(const char *in, const char *out)
{
    size_t n;
    char *s = slurp(in, &n);
    if (!s) die("cannot read", in);
    char *names[MAXF];
    int nf = 0;
    const char *K1 = "\"name\": \"";
    for (char *p = strstr(s, K1); p; p = strstr(p + 1, K1)) {
        char *a = p + strlen(K1), *e = strchr(a, '"');
        if (!e || e - a > 63 || nf == MAXF) die("bad function name in", in);
        names[nf] = strndup(a, (size_t)(e - a));
        nf++;
    }
    if (!nf) die("no function in", in);
    FILE *f = fopen(out, "wb");
    if (!f) die("cannot write", out);
    for (char *p = s; *p;) {
        const char *keys[2] = {"\"name\": \"", "\"func\": \""};
        int hit = -1;
        for (int k = 0; k < 2; k++) if (!strncmp(p, keys[k], strlen(keys[k]))) hit = k;
        if (hit < 0) { fputc(*p++, f); continue; }
        char *a = p + strlen(keys[hit]), *e = strchr(a, '"');
        if (!e) die("unterminated name in", in);
        int idx = -1;
        for (int k = 0; k < nf; k++) if ((size_t)(e - a) == strlen(names[k]) && !strncmp(a, names[k], (size_t)(e - a))) idx = k;
        if (idx < 0) die("call to an unknown function in", in);
        fprintf(f, "%sfn%d_\"", keys[hit], idx);
        p = e + 1;
    }
    if (fclose(f)) die("cannot write", out);
    return 0;
}

/* ---- types and values (parse_val / at_width copied from tests/one_layer/acceptance/ola.c) -------------- */
enum { K_BOOL, K_U8, K_U16, K_U32, K_U64, K_I8, K_I16, K_I32, K_I64 };
static const char *const KN[] = {"bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"};
static int kind_of(const char *s) { for (int i = 0; i < 9; i++) if (!strcmp(s, KN[i])) return i; return -1; }
static unsigned kwidth(int k) { static const unsigned w[] = {8, 8, 16, 32, 64, 8, 16, 32, 64}; return w[k]; }
static int ksigned(int k) { return k >= K_I8 && k <= K_I64; }

static int parse_val(const char *s, int k, uint64_t *out)
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
    uint64_t lim = (uint64_t)1 << (w - 1);
    if (neg ? v > lim : v >= lim) return -1;
    *out = neg ? (uint64_t)(-(int64_t)(v - 1) - 1) : v;
    return 0;
}

static uint64_t at_width(uint64_t r, int k)
{
    unsigned w = kwidth(k);
    if (w == 64) return r;
    uint64_t m = ((uint64_t)1 << w) - 1;
    r &= m;
    if (ksigned(k) && (r >> (w - 1))) r |= ~m;
    return r;
}

static void put_val(FILE *f, uint64_t v, int k)
{
    if (ksigned(k)) fprintf(f, "%" PRId64, (int64_t)v);
    else fprintf(f, "%" PRIu64, v);
}

/* ---- inputs --------------------------------------------------------------------------------------------- */
static uint64_t kmin(int k) { return ksigned(k) ? (~(uint64_t)0 << (kwidth(k) - 1)) : 0; }
static uint64_t kmax(int k) { if (k == K_BOOL) return 1; unsigned w = kwidth(k) - (unsigned)ksigned(k); return w == 64 ? UINT64_MAX : ((uint64_t)1 << w) - 1; }

static uint64_t pick(int k, uint64_t r, int line)
{
    static const int64_t small[] = {0, 1, 2, 3, 5, 7, 9, 10, 11, 99, 100, 255, 256, 1000};
    uint64_t lo = kmin(k), hi = kmax(k);
    if (k == K_BOOL) return r & 1;
    if (line < 4) { /* boundaries */
        uint64_t b[4] = {lo, hi, 0, 1};
        if (ksigned(k)) b[3] = (uint64_t)-1;
        return b[line];
    }
    switch (r % 4) {
    case 0: case 1: { int64_t v = small[(r >> 8) % 14]; if (ksigned(k) && (r >> 20) & 1) v = -v; return at_width((uint64_t)v, k); }
    case 2: return at_width(r >> 11, k);
    default: { uint64_t v = (r >> 13) % 100000; if (ksigned(k) && (r >> 40) & 1) v = (uint64_t)-(int64_t)v; return at_width(v, k); }
    }
    (void)hi;
}

static int inputs(int argc, char **argv)
{
    int count = atoi(argv[0]), np = argc - 1, ks[6];
    if (count < 1 || count > 4096 || np < 1 || np > 6) die("usage: gabs inputs <count 1..4096> <type 1..6>...", NULL);
    for (int i = 0; i < np; i++) if ((ks[i] = kind_of(argv[1 + i])) < 0) die("unknown type", argv[1 + i]);
    uint64_t x = 0x9e3779b97f4a7c15ull;
    for (int l = 0; l < count; l++) {
        for (int i = 0; i < np; i++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            uint64_t v = pick(ks[i], x ^ (x >> 29), l < 4 ? (l + i) % 4 : l);
            if (i) putchar(' ');
            put_val(stdout, v, ks[i]);
        }
        putchar('\n');
    }
    return 0;
}

/* ---- run ------------------------------------------------------------------------------------------------ */
static int memfd(const char *name, const void *p, size_t n)
{
    int fd = (int)syscall(SYS_memfd_create, name, 0);
    if (fd < 0) die("memfd_create failed", NULL);
    if (n && write(fd, p, n) != (ssize_t)n) die("memfd write failed", NULL);
    if (lseek(fd, 0, SEEK_SET) != 0) die("memfd seek failed", NULL);
    return fd;
}

static char *read_fd(int fd, size_t *n)
{
    struct stat st;
    if (fstat(fd, &st)) die("fstat failed", NULL);
    size_t len = (size_t)st.st_size;
    if (len > (1u << 20)) len = 1u << 20;
    char *b = malloc(len + 1);
    if (!b || lseek(fd, 0, SEEK_SET) != 0) die("read memfd failed", NULL);
    size_t got = 0;
    while (got < len) { ssize_t r = read(fd, b + got, len - got); if (r <= 0) break; got += (size_t)r; }
    b[got] = 0;
    *n = got;
    return b;
}

static int run(int argc, char **argv)
{
    if (argc < 7) die("usage: gabs run <ola_runner> <scratch> <ir> <entry> <ret> <inputs> <expected|-> [<type>...]", NULL);
    const char *runner = argv[0], *scratch = argv[1], *ir = argv[2], *entry = argv[3], *rets = argv[4], *inp = argv[5], *exp = argv[6];
    int ret = kind_of(rets), np = argc - 7, ks[6];
    if (ret < 0) die("unknown return type", rets);
    if (np > 6) die("more than 6 parameters", NULL);
    for (int i = 0; i < np; i++) if ((ks[i] = kind_of(argv[7 + i])) < 0) die("unknown type", argv[7 + i]);
    size_t in_n, ir_n;
    char *in = slurp(inp, &in_n);
    char *irb = slurp(ir, &ir_n);
    if (!in) die("cannot read", inp);
    if (!irb) { printf("REJECT no_ir\n"); return 1; }
    /* inputs: u32 LE count, then n * np u64 LE (ola_runner load_inputs) */
    size_t cap = 4 + 4096u * 6u * 8u, bl = 4;
    uint8_t *bin = calloc(1, cap);
    uint32_t nlines = 0;
    for (char *p = in; *p;) {
        char *e = strchr(p, '\n');
        if (!e) die("inputs do not end with a newline:", inp);
        *e = 0;
        int k = 0;
        for (char *t = strtok(p, " "); t; t = strtok(NULL, " ")) {
            uint64_t v;
            if (k == np || parse_val(t, ks[k], &v)) die("bad input value in", inp);
            for (int b = 0; b < 8; b++) bin[bl++] = (uint8_t)(v >> (8 * b));
            k++;
        }
        if (k != np) die("input arity differs from the parameter list in", inp);
        if (++nlines > 4096) die("more than 4096 input lines in", inp);
        p = e + 1;
    }
    if (!nlines) die("no input lines in", inp);
    for (int b = 0; b < 4; b++) bin[b] = (uint8_t)(nlines >> (8 * b));
    char *av[16];
    int ac = 0;
    av[ac++] = (char *)runner; av[ac++] = "oscir"; av[ac++] = "gabs"; av[ac++] = (char *)scratch;
    av[ac++] = (char *)entry; av[ac++] = (char *)rets;
    for (int i = 0; i < np; i++) av[ac++] = argv[7 + i];
    av[ac] = NULL;
    int fin = memfd("gabs-in", bin, bl), fout = memfd("gabs-out", NULL, 0), ferr = memfd("gabs-err", NULL, 0), fcand = memfd("gabs-cand", irb, ir_n);
    pid_t pid = fork();
    if (pid < 0) die("fork failed", NULL);
    if (pid == 0) {
        if (dup2(fin, 0) < 0 || dup2(fout, 1) < 0 || dup2(ferr, 2) < 0 || dup2(fcand, 3) < 0) _exit(127);
        syscall(SYS_close_range, 4, ~0u, 0);
        struct rlimit rl;
        rl.rlim_cur = 10; rl.rlim_max = 11; setrlimit(RLIMIT_CPU, &rl);
        rl.rlim_cur = rl.rlim_max = (rlim_t)1024 << 20; setrlimit(RLIMIT_AS, &rl);
        rl.rlim_cur = rl.rlim_max = 0; setrlimit(RLIMIT_CORE, &rl);
        char *envp[] = {NULL};
        execve(runner, av, envp);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) die("waitpid failed", NULL);
    size_t on, en;
    char *out = read_fd(fout, &on), *err = read_fd(ferr, &en);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) die("cannot exec", runner);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 96) die("the sandbox could not be built:", err);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        if (WIFSIGNALED(st)) printf("REJECT crash signal=%d\n", WTERMSIG(st));
        else printf("REJECT runner_exit code=%d\n", WEXITSTATUS(st));
        return 1;
    }
    /* transcript: OLA1 gabs / REFUSE r d | R v / T c lines / END gabs n */
    char *lines[4100];
    int nl = 0;
    for (char *p = out; *p && nl < 4100;) { char *e = strchr(p, '\n'); if (!e) break; *e = 0; lines[nl++] = p; p = e + 1; }
    if (nl < 2 || strcmp(lines[0], "OLA1 gabs")) { printf("REJECT bad_transcript\n"); return 1; }
    if (!strncmp(lines[1], "REFUSE ", 7)) { printf("REJECT %s\n", lines[1] + 7); return 1; }
    char endl[64];
    snprintf(endl, sizeof endl, "END gabs %u", nlines);
    if ((uint32_t)nl != nlines + 2 || strcmp(lines[nl - 1], endl)) { printf("REJECT bad_transcript\n"); return 1; }
    if (!strcmp(exp, "-")) {
        for (uint32_t i = 0; i < nlines; i++) {
            const char *l = lines[1 + i];
            if (l[0] == 'T') printf("TRAP %s\n", l + 2);
            else { uint64_t v = strtoull(l + 2, NULL, 10); put_val(stdout, at_width(v, ret), ret); putchar('\n'); }
        }
        return 0;
    }
    size_t xn;
    char *xs = slurp(exp, &xn);
    if (!xs) die("cannot read", exp);
    uint32_t i = 0;
    for (char *p = xs; *p; i++) {
        char *e = strchr(p, '\n');
        if (!e) die("expected does not end with a newline:", exp);
        *e = 0;
        if (i >= nlines) { printf("REJECT expected_line_count\n"); return 1; }
        const char *l = lines[1 + i];
        int got_trap = l[0] == 'T';
        long got_code = got_trap ? strtol(l + 2, NULL, 10) : 0;
        uint64_t got = got_trap ? 0 : at_width(strtoull(l + 2, NULL, 10), ret);
        char gs[32];
        if (got_trap) snprintf(gs, sizeof gs, "TRAP_%ld", got_code);
        else if (ksigned(ret)) snprintf(gs, sizeof gs, "%" PRId64, (int64_t)got);
        else snprintf(gs, sizeof gs, "%" PRIu64, got);
        if (!strncmp(p, "TRAP", 4) && (p[4] == 0 || p[4] == ' ')) {
            if (!got_trap) { printf("REJECT missing_trap line=%u got=%s want=%s\n", i + 1, gs, p); return 1; }
            if (p[4] == ' ' && atol(p + 5) != got_code) { printf("REJECT wrong_trap line=%u got=%s want=%s\n", i + 1, gs, p); return 1; }
        } else {
            uint64_t want;
            if (parse_val(p, ret, &want)) die("bad expected value in", exp);
            if (got_trap) { printf("REJECT unexpected_trap line=%u got=%s want=%s\n", i + 1, gs, p); return 1; }
            if (got != want) { printf("REJECT wrong_result line=%u got=%s want=%s\n", i + 1, gs, p); return 1; }
        }
        p = e + 1;
    }
    if (i != nlines) { printf("REJECT expected_line_count\n"); return 1; }
    printf("ACCEPT n=%u\n", nlines);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "split")) return split(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "canon")) return canon(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "inputs")) return inputs(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "run")) return run(argc - 2, argv + 2);
    fprintf(stderr, "usage: gabs split <prog> <prefix> | canon <in.json> <out.json> | inputs <count> <type>... | run ...\n");
    return 2;
}
