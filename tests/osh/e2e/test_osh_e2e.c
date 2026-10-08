/*
 * test_osh_e2e.c -- end-to-end tests for the osh program (aien-architecture#158 cut 4c).
 * Runs build/osh/osh as a child process in a mkdtemp fixture, with argprint as the test program, and compares
 * standard output, standard error and exit status. Every case runs twice, native and OSH_INTERP=1, and the two
 * runs must be identical in all three. No shell is involved: osh is started with execve() through fork.
 * Usage: test_osh_e2e OSH ARGPRINT
 * "@A" in a case is replaced by the argprint path and "@T" by the fixture directory. Final line OSH_E2E_PASS/FAIL.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *OSH, *ARG, *TMP;
/* osh is fail closed without --caps, so every case runs with this explicit policy (all four effects under /): these
 * cases test the language and the execution service; enforcement itself is test_osh_caps.c. */
static char POL[4096];
static const char E2E_POLICY[] = "principal 1\nallow spawn /\nallow read /\nallow write /\nallow chdir /\n";
static unsigned long checks, fails;

static void check(int ok, const char *fmt, ...)
{
    checks++;
    if (ok) return;
    fails++;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static char *subst(const char *s)
{
    size_t cap = strlen(s) * 3 + strlen(ARG) * 8 + strlen(TMP) * 8 + 64, o = 0;
    char *r = malloc(cap + 1);
    for (; *s; s++) {
        if (s[0] == '@' && s[1] == 'A') { o += (size_t)sprintf(r + o, "%s", ARG); s++; }
        else if (s[0] == '@' && s[1] == 'T') { o += (size_t)sprintf(r + o, "%s", TMP); s++; }
        else r[o++] = *s;
        if (o + 4096 > cap) { cap *= 2; r = realloc(r, cap + 1); }
    }
    r[o] = 0;
    return r;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b = calloc(1, 1 << 16);
    if (f) {
        size_t n = fread(b, 1, (1 << 16) - 1, f);
        b[n] = 0;
        fclose(f);
    }
    return b;
}

typedef struct {
    char *out, *err;
    int status;
} Run;

/* mode 0: osh -c TEXT [args]; 1: osh SCRIPT [args] (TEXT written to the file); 2: osh with TEXT on standard input */
static Run run_osh(int mode, const char *text, char *const *args, int interp)
{
    char outp[512], errp[512], inp[512], scr[512];
    snprintf(outp, sizeof outp, "%s/.out", TMP);
    snprintf(errp, sizeof errp, "%s/.err", TMP);
    snprintf(inp, sizeof inp, "%s/.in", TMP);
    snprintf(scr, sizeof scr, "%s/script.osh", TMP);
    if (mode >= 1) {
        const char *dst = mode == 1 ? scr : inp;
        FILE *f = fopen(dst, "wb");
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
    char *av[32];
    int n = 0;
    av[n++] = (char *)OSH;
    av[n++] = "--caps";
    av[n++] = POL;
    if (mode == 0) { av[n++] = "-c"; av[n++] = (char *)text; }
    if (mode == 1) av[n++] = scr;
    for (; args && *args; args++) av[n++] = *args;
    av[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        int fo = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644), fe = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int fi = open(mode == 2 ? inp : "/dev/null", O_RDONLY);
        dup2(fi, 0); dup2(fo, 1); dup2(fe, 2);
        close(fi); close(fo); close(fe);
        if (chdir(TMP) != 0) _exit(126);
        if (interp) setenv("OSH_INTERP", "1", 1);
        else unsetenv("OSH_INTERP");
        execv(OSH, av);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    Run r;
    r.out = slurp(outp);
    r.err = slurp(errp);
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return r;
}

typedef struct {
    const char *name;
    int mode;
    const char *text;
    char *const *args;
    const char *out;      /* exact standard output */
    const char *err;      /* NULL: standard error must be empty; else it must contain this text */
    int status;
} Case;

static char *const A_NAME_AB[] = {"name", "a", "b", NULL};
static char *const A_ONETWO[] = {"zero", "one", "two words", NULL};

static const Case CASES[] = {
    {"plain words", 0, "@A a b c", NULL, "[a]\n[b]\n[c]\n", NULL, 0},
    {"quoting and empty args", 0, "@A a \"b  c\" 'd  e' \"\" '' x\\ y \"q'r\" 's\"t'", NULL, "[a]\n[b  c]\n[d  e]\n[]\n[]\n[x y]\n[q'r]\n[s\"t]\n", NULL, 0},
    {"backslashes", 0, "@A \\$X \"\\$X\" \"a\\\\b\" 'a\\b' \"a\\b\"", NULL, "[$X]\n[$X]\n[a\\b]\n[a\\b]\n[a\\b]\n", NULL, 0},
    {"variable splits unquoted", 0, "X='1  2'; @A $X \"$X\" a$X b${X}c", NULL, "[1]\n[2]\n[1  2]\n[a1]\n[2]\n[b1]\n[2c]\n", NULL, 0},
    {"empty and unset expansions", 0, "E=; @A $E \"$E\" $U \"$U\" '' x", NULL, "[]\n[]\n[]\n[x]\n", NULL, 0},
    {"empty beside text", 0, "E=; @A a${E}b \"$E$E\"", NULL, "[ab]\n[]\n", NULL, 0},
    {"assignment is not split", 0, "X='a  b'; Y=$X; @A \"$Y\"", NULL, "[a  b]\n", NULL, 0},
    {"later assignments see earlier ones", 0, "A=1 B=$A$A; @A $A $B", NULL, "[1]\n[11]\n", NULL, 0},
    {"assignment stays inside the shell", 0, "Z=1; @A -e Z", NULL, "[(unset)]\n", NULL, 0},
    {"export reaches children", 0, "Z=1; export Z; @A -e Z; export Y=two; @A -e Y", NULL, "[1]\n[two]\n", NULL, 0},
    {"prefix assignment is for one command", 0, "W=5 @A -e W; @A -e W", NULL, "[5]\n[(unset)]\n", NULL, 0},
    {"prefix assignment sees old value", 0, "A=old; A=new B=$A @A -e A; @A -e B", NULL, "[new]\n[(unset)]\n", NULL, 0},
    {"unset", 0, "export Q=1; @A -e Q; unset Q; @A -e Q", NULL, "[1]\n[(unset)]\n", NULL, 0},
    {"environment comes in", 0, "@A -e OSH_E2E_PROBE", NULL, "[(unset)]\n", NULL, 0},
    {"semicolon and and-or", 0, "@A a; @A b && @A c || @A d", NULL, "[a]\n[b]\n[c]\n", NULL, 0},
    {"or runs on failure", 0, "@A -x 1 || @A yes; @A -x 0 || @A no; @A -x 2 && @A no2", NULL, "[yes]\n", NULL, 2},
    {"and skips on failure, or resumes", 0, "@A -x 1 && @A no || @A yes", NULL, "[yes]\n", NULL, 0},
    {"exit status of last command", 0, "@A -x 7", NULL, "", NULL, 7},
    {"dollar question", 0, "@A -x 3; printf '%s\\n' $?; @A -x 0; printf '%s\\n' $?", NULL, "3\n0\n", NULL, 0},
    {"pipeline", 0, "@A a b | @A -cat", NULL, "[a]\n[b]\n", NULL, 0},
    {"three stage pipeline", 0, "@A x | @A -cat | @A -cat", NULL, "[x]\n", NULL, 0},
    {"pipeline status is the last command", 0, "@A -x 4 | @A -cat; printf '%s\\n' $?; @A a | @A -x 5", NULL, "0\n", NULL, 5},
    {"redirect out and in", 0, "@A hi > @T/o1; @A -cat < @T/o1; @A -cat <@T/o1", NULL, "[hi]\n[hi]\n", NULL, 0},
    {"redirect truncates", 0, "@A long line > @T/o2; @A s > @T/o2; @A -cat < @T/o2", NULL, "[s]\n", NULL, 0},
    {"redirect append", 0, "@A 1 > @T/o3; @A 2 >> @T/o3; @A -cat < @T/o3", NULL, "[1]\n[2]\n", NULL, 0},
    {"stderr to file", 0, "@A -io 2> @T/e1; @A -cat < @T/e1", NULL, "OE", NULL, 0},
    {"stderr appended", 0, "@A -io 2> @T/e2; @A -io 2>> @T/e2; @A -cat < @T/e2", NULL, "OOEE", NULL, 0},
    {"2>&1 into a pipe", 0, "@A -io 2>&1 | @A -cat", NULL, "OE", NULL, 0},
    {"2>&1 then file: stderr follows the old stdout", 0, "@A -io 2>&1 > @T/f1; @A -cat < @T/f1", NULL, "EO", NULL, 0},
    {"file then 2>&1: both in the file", 0, "@A -io > @T/f2 2>&1; @A -cat < @T/f2", NULL, "OE", NULL, 0},
    {"1>&2 sends output to stderr", 0, "@A a 1>&2", NULL, "", "[a]", 0},
    {"explicit descriptors", 0, "@A -io 1> @T/f3 2> @T/f4; @A -cat 0< @T/f3; @A -cat 0<@T/f4", NULL, "OE", NULL, 0},
    {"redirect target expands", 0, "F=@T/f5; @A v > \"$F\"; @A -cat < $F", NULL, "[v]\n", NULL, 0},
    {"redirect in a pipeline stage", 0, "@A a b > @T/f6 | @A -cat; @A -cat < @T/f6", NULL, "[a]\n[b]\n", NULL, 0},
    {"missing input file", 0, "@A -cat < @T/nonexistent; printf '%s\\n' $?", NULL, "1\n", "nonexistent", 0},
    {"cd and pwd", 0, "cd @T; pwd; cd /; pwd; cd @T; @A -pwd", NULL, "@T\n/\n[@T]\n", NULL, 0},
    {"cd failure", 0, "cd @T/no/such; printf '%s\\n' $?", NULL, "1\n", "no", 0},
    {"printf", 0, "printf 'a%sb\\n' x; printf '%s|%s\\n' 1 2; printf '%d\\n' 42", NULL, "axb\n1|2\n42\n", NULL, 0},
    {"printf with expansions", 0, "X='a b'; printf '[%s]\\n' $X \"$X\"", NULL, "[a]\n[b]\n[a b]\n", NULL, 0},
    {"builtin redirect", 0, "printf hello > @T/f7; @A -cat < @T/f7", NULL, "hello", NULL, 0},
    {"exit status", 0, "exit 7", NULL, "", NULL, 7},
    {"exit stops the list", 0, "@A a; exit 3; @A b", NULL, "[a]\n", NULL, 3},
    {"exit without argument uses last status", 0, "@A -x 9; exit", NULL, "", NULL, 9},
    {"positional parameters", 0, "@A \"$0\" $1 \"$2\" \"$#\" $3 \"$@\"", A_NAME_AB, "[name]\n[a]\n[b]\n[2]\n[a]\n[b]\n", NULL, 0},
    {"quoted at keeps words, star joins", 0, "@A \"$@\"; @A \"$*\"; @A $*", A_ONETWO, "[one]\n[two words]\n[one two words]\n[one]\n[two]\n[words]\n", NULL, 0},
    {"no positionals", 0, "@A x \"$@\" y $@ \"$#\" \"$1\"", NULL, "[x]\n[y]\n[0]\n[]\n", NULL, 0},
    {"default arg0", 0, "@A \"$0\"", NULL, "[osh]\n", NULL, 0},
    {"comments and blank lines", 1, "# comment\n\n@A a # not b\n  \n@A b\n", NULL, "[a]\n[b]\n", NULL, 0},
    {"script with arguments", 1, "@A \"$0\" \"$1\" $#\n", A_NAME_AB, "[@T/script.osh]\n[name]\n[3]\n", NULL, 0},
    {"continuation lines", 1, "@A a \\\nb \\\n  c\n", NULL, "[a]\n[b]\n[c]\n", NULL, 0},
    {"-c: trailing backslash is literal (bash 5.2)", 0, "@A a\\", NULL, "[a\\]\n", NULL, 0},
    {"-c: a list ending in ; at end of input runs", 0, "@A a;", NULL, "[a]\n", NULL, 0},
    {"-c: two lists, the last ending in ;", 0, "@A a; @A b; ", NULL, "[a]\n[b]\n", NULL, 0},
    {"script: trailing backslash with no newline joins nothing", 1, "@A a\\", NULL, "[a]\n", NULL, 0},
    {"stdin: trailing backslash with no newline joins nothing", 2, "@A a\\", NULL, "[a]\n", NULL, 0},
    {"script: last line ends in ; with no newline", 1, "@A a\n@A b;", NULL, "[a]\n[b]\n", NULL, 0},
    {"printf option is refused and prints nothing", 0, "printf -v x '%s' hi; @A \"$x\" $?", NULL, "[]\n[2]\n", "option not supported", 0},
    {"exit with two arguments drops the rest of the list (bash)", 1, "exit 1 2; @A same\n@A next $?\n", NULL, "[next]\n[1]\n", "too many arguments", 0},
    {"export NAME=$v is not split", 0, "Y='a  b'; export A=$Y; @A -e A", NULL, "[a  b]\n", NULL, 0},
    {"quoted empty var then $@ with no arguments gives no argument", 0, "E=; @A x \"$E$@\" y", NULL, "[x]\n[y]\n", NULL, 0},
    {"colon is refused before anything runs", 0, "@A a; : ; @A b", NULL, "[a]\n", "UNSUPPORTED_BUILTIN", 2},
    {"quote across lines", 1, "@A \"a\nb\" 'c\nd'\n", NULL, "[a\nb]\n[c\nd]\n", NULL, 0},
    {"and-or across lines", 1, "@A a &&\n@A b ||\n@A c\n@A -x 1 |\n@A -cat\n", NULL, "[a]\n[b]\n", NULL, 0},
    {"missing newline at end", 1, "@A a\n@A b", NULL, "[a]\n[b]\n", NULL, 0},
    {"state carries between lines", 1, "X=1\nexport X\n@A -e X\ncd @T\n@A -pwd\n", NULL, "[1]\n[@T]\n", NULL, 0},
    {"dollar question carries between lines", 1, "@A -x 6\nprintf '%s\\n' $?\n", NULL, "6\n", NULL, 0},
    {"exit in a script", 1, "@A a\nexit 4\n@A b\n", NULL, "[a]\n", NULL, 4},
    {"script from standard input", 2, "@A a\nX=7\nprintf '%s\\n' $X\n", NULL, "[a]\n7\n", NULL, 0},
    {"standard input leaves the rest to children", 2, "@A -cat\nsecond line\n@A z\n", NULL, "second line\n@A z\n", NULL, 0},
    {"standard input, last status at end", 2, "@A -x 5\n", NULL, "", NULL, 5},
    {"standard input, exit", 2, "exit 8\n@A no\n", NULL, "", NULL, 8},
    {"missing command", 0, "@T/no-such-program; printf '%s\\n' $?", NULL, "127\n", "no-such-program", 0},
    {"not executable", 0, "@T; printf '%s\\n' $?", NULL, "126\n", "Is a directory", 0},
    /* syntax errors: the shell says what and where, exits 2 */
    {"unterminated quote", 0, "@A \"abc", NULL, "", "syntax error: SYNTAX_EOF", 2},
    {"dangling operator", 0, "@A a &&", NULL, "", "syntax error: SYNTAX_EOF", 2},
    {"empty command", 0, "@A a ; ; @A b", NULL, "", "syntax error: SYNTAX_EMPTY_CMD", 2},
    {"missing redirect target", 0, "@A a >", NULL, "", "syntax error", 2},
    {"syntax error stops a script", 1, "@A a\n@A b &&& @A c\n@A d\n", NULL, "[a]\n", "syntax error", 2},
    {"error shows the byte offset", 0, "echo ok > @T/x; $(true)", NULL, "", "at byte", 2},
    /* refused constructs, each with its named code */
    {"refused: glob", 0, "@A *.c", NULL, "", "syntax error: GLOB at byte", 2},
    {"refused: backtick", 0, "@A `x`", NULL, "", "syntax error: BACKTICK at byte", 2},
    {"refused: command substitution", 0, "@A $(x)", NULL, "", "syntax error: CMDSUB at byte", 2},
    {"refused: tilde", 0, "@A ~", NULL, "", "syntax error: TILDE at byte", 2},
    {"refused: parameter operator", 0, "@A ${X:-y}", NULL, "", "syntax error: PARAM_OP at byte", 2},
    {"refused: special parameter", 0, "@A $$", NULL, "", "syntax error: SPECIAL_PARAM at byte", 2},
    {"refused: background", 0, "@A a & @A b", NULL, "", "syntax error: BACKGROUND at byte", 2},
    {"refused: subshell", 0, "(@A a)", NULL, "", "syntax error: SUBSHELL at byte", 2},
    {"refused: here document", 0, "@A -cat << EOF", NULL, "", "syntax error: HEREDOC at byte", 2},
    {"refused: other redirection", 0, "@A a <> f", NULL, "", "syntax error: REDIR_OTHER at byte", 2},
    {"refused: if", 0, "if true", NULL, "", "syntax error: IF_COMPOUND at byte", 2},
    {"refused: loop", 0, "while true", NULL, "", "syntax error: LOOP at byte", 2},
    {"refused: case", 0, "case x", NULL, "", "syntax error: CASE at byte", 2},
    {"refused: group", 0, "{ @A a; }", NULL, "", "syntax error: GROUP at byte", 2},
    {"refused: function", 0, "function f", NULL, "", "syntax error: FUNCTION at byte", 2},
    {"refused: negation", 0, "! @A a", NULL, "", "syntax error: NEGATION at byte", 2},
    {"refused: brace expansion", 0, "@A {a,b}", NULL, "", "syntax error: BRACE_EXPANSION at byte", 2},
    {"refused: positional range", 0, "@A ${10}", NULL, "", "syntax error: POSITIONAL_RANGE at byte", 2},
    {"refused: IFS assignment", 0, "IFS=x", NULL, "", "syntax error: IFS_ASSIGN at byte", 2},
    {"refused: set", 0, "set -e", NULL, "", "refused: UNSUPPORTED_BUILTIN at byte", 2},
    {"refused: eval through a variable", 0, "B=eval; $B x", NULL, "", "refused: UNSUPPORTED_BUILTIN at byte", 2},
    {"refused: glob in a value", 0, "G='a*'; @A $G", NULL, "", "refused: VALUE_GLOB at byte", 2},
    {"quoted glob value is fine", 0, "G='a*'; @A \"$G\" '*' \"?\"", NULL, "[a*]\n[*]\n[?]\n", NULL, 0},
    {"refused: ambiguous redirect", 0, "F='a b'; @A x > $F", NULL, "", "refused: AMBIGUOUS_REDIRECT at byte", 2},
    {"refusal after earlier output keeps it", 1, "@A a\nG='*'; @A $G\n@A b\n", NULL, "[a]\n", "refused: VALUE_GLOB", 2},
};

/* Read the pseudo-terminal into buf until it holds want, for up to secs seconds. 1 found, 0 not. */
static int pty_wait(int fd, char *buf, size_t cap, size_t *n, const char *want, int secs)
{
    for (int i = 0; i < secs * 10; i++) {
        buf[*n] = 0;
        if (strstr(buf, want)) return 1;
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        ssize_t r = read(fd, buf + *n, cap - 1 - *n);
        if (r <= 0) break;
        *n += (size_t)r;
    }
    buf[*n] = 0;
    return strstr(buf, want) != NULL;
}

/* ^C typed at an idle interactive prompt (review of #340, BUG-5): the shell drops the half-typed line, prompts again,
 * keeps running and sets $? to 130, as bash does. Before the fix the shell died of SIGINT. Needs a pseudo-terminal. */
static void test_prompt_ctrl_c(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master) || !ptsname(master)) {
        printf("note: no pseudo-terminal available, prompt ^C test skipped\n");
        return;
    }
    char sname[256];
    snprintf(sname, sizeof sname, "%s", ptsname(master));
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(sname, O_RDWR); /* the new session leader acquires the terminal */
        if (s < 0) _exit(126);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(master);
        if (chdir(TMP) != 0) _exit(126);
        unsetenv("OSH_INTERP");
        char *av[] = {(char *)OSH, "--caps", POL, NULL}; /* fail closed without a policy, as in run_osh() */
        execv(OSH, av);
        _exit(127);
    }
    char buf[16384], line[1200];
    size_t n = 0;
    int ok = pty_wait(master, buf, sizeof buf, &n, "osh$ ", 5);
    check(ok, "prompt ^C: first prompt");
    snprintf(line, sizeof line, "%s half", ARG);
    if (write(master, line, strlen(line)) < 0) {}
    usleep(200000);
    size_t mark = n;
    if (write(master, "\003", 1) < 0) {}
    ok = pty_wait(master, buf + mark, sizeof buf - mark, &(size_t){0}, "osh$ ", 5);
    n = strlen(buf);
    check(ok, "prompt ^C: a new prompt after ^C");
    snprintf(line, sizeof line, "%s \"st=$?\"\n", ARG);
    if (write(master, line, strlen(line)) < 0) {}
    ok = pty_wait(master, buf, sizeof buf, &n, "[st=130]", 5);
    check(ok, "prompt ^C: the shell still runs commands and $? is 130");
    check(strstr(buf, "[half]") == NULL, "prompt ^C: the abandoned line did not run");
    if (write(master, "exit 0\n", 7) < 0) {}
    int st = -1;
    for (int i = 0; i < 50 && waitpid(pid, &st, WNOHANG) == 0; i++) usleep(100000);
    if (st == -1) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "prompt ^C: shell exits 0 on exit 0 (raw status %d)", st);
    if (fails) fprintf(stderr, "  prompt ^C transcript:\n%s\n", buf);
    close(master);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s OSH ARGPRINT\n", argv[0]); return 2; }
    char tmpl[] = "/tmp/osh-e2e-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    OSH = realpath(argv[1], NULL);
    ARG = realpath(argv[2], NULL);
    TMP = realpath(tmpl, NULL);
    if (!OSH || !ARG || !TMP) { fprintf(stderr, "bad path\n"); return 2; }
    snprintf(POL, sizeof POL, "%s.caps", TMP);
    FILE *pf = fopen(POL, "w");
    if (!pf || fputs(E2E_POLICY, pf) == EOF || fclose(pf) != 0) { fprintf(stderr, "cannot write %s\n", POL); return 2; }
    unsigned long ncases = 0;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        const Case *c = &CASES[i];
        char *text = subst(c->text);
        char *want_out = subst(c->out);
        char *args[8];
        int na = 0;
        if (c->args)
            for (char *const *a = c->args; *a; a++) args[na++] = *a;
        args[na] = NULL;
        Run r[2];
        for (int ip = 0; ip < 2; ip++) {
            /* a fresh fixture state for each run */
            char cmd[600];
            (void)cmd;
            r[ip] = run_osh(c->mode, text, args, ip);
            int ok = strcmp(r[ip].out, want_out) == 0 && r[ip].status == c->status;
            if (c->err) ok = ok && strstr(r[ip].err, c->err) != NULL;
            else ok = ok && r[ip].err[0] == 0;
            if (!ok)
                fprintf(stderr, "  case [%s] %s:\n    want status %d out \"%s\" err %s%s\n    got  status %d out \"%s\" err \"%s\"\n", c->name, ip ? "interp" : "native", c->status,
                        want_out, c->err ? "containing " : "empty", c->err ? c->err : "", r[ip].status, r[ip].out, r[ip].err);
            check(ok, "case [%s] %s", c->name, ip ? "OSH_INTERP=1" : "native");
        }
        check(strcmp(r[0].out, r[1].out) == 0 && strcmp(r[0].err, r[1].err) == 0 && r[0].status == r[1].status, "case [%s]: native and OSH_INTERP=1 differ", c->name);
        ncases++;
        free(text);
        free(want_out);
        free(r[0].out); free(r[0].err); free(r[1].out); free(r[1].err);
    }
    test_prompt_ctrl_c();
    printf("e2e cases %lu (each run native and OSH_INTERP=1), checks %lu, failures %lu\n", ncases, checks, fails);
    /* clean the fixture: only files this test created */
    char rm[600];
    snprintf(rm, sizeof rm, "%s", TMP);
    {
        const char *names[] = {".out", ".err", ".in", "script.osh", "o1", "o2", "o3", "e1", "e2", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "x"};
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            char p[700];
            snprintf(p, sizeof p, "%s/%s", TMP, names[i]);
            unlink(p);
        }
        rmdir(TMP);
    }
    unlink(POL);
    printf(fails ? "OSH_E2E_FAIL\n" : "OSH_E2E_PASS\n");
    return fails != 0;
}
