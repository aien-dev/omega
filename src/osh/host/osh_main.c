/*
 * osh_main.c -- the osh program (aien-architecture#158).
 *   osh                         read commands from standard input (prompts only when it is a terminal)
 *   osh -c 'string' [arg0 [args...]]
 *   osh script [args...]
 * A leading `--caps POLICYFILE` enforces the launch policy (osh_caps.h): the host starts omega's capability root, mints
 * the grants, seals the admin handle, and every spawn / redirection open / cd is checked at the moment of the effect.
 * Without it the policy is a principal with no grants: every such effect is denied (fail closed). Builtins that make
 * no effect (printf, pwd, export, unset, exit) still run.
 * The effect journal (osh_journal.h, ABI section 10) is MANDATORY and always on: before each external spawn or write-class
 * redirection an intent is fsynced, after it an outcome. Default file: $XDG_STATE_HOME/osh/effects.journal, else
 * $HOME/.local/state/osh/effects.journal (directories 0700, file 0600); `--journal FILE` (before or after --caps) names another.
 * At startup every intent left without an outcome by an earlier run is reported on stderr as UNKNOWN (never re-run) and
 * closed. There is no opt-out. If the journal cannot be opened or recovered the shell says so on stderr and refuses every
 * effect (status 1, nothing launched); builtins without a write redirection still run.
 * OSH_INTERP=1 in the environment runs the shell core in the reference interpreter instead of native code.
 * Never runs a command through another shell: programs are started by the host adapter with execve().
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osh_caps.h"
#include "osh_journal.h"
#include "osh_shell.h"

extern char **environ;

static const char *g_journal_file;
static OshJournal g_journal = {.fd = -1};

static char g_journal_err[4700];

/* The effect journal is MANDATORY (ABI section 10 has no off switch). Open it (default location when no --journal was
 * given), report what a previous run left unfinished, attach it. If it cannot be opened, recovered or written, the shell
 * still starts but says so loudly and refuses every effect (spawn, write-class open) until it is run with a working one. */
static void journal_unavailable(OshShell *sh, const char *what)
{
    snprintf(g_journal_err, sizeof g_journal_err, "%s", what);
    sh->s.journal_error = g_journal_err;
    osh_journal_close(&g_journal);
    sh->s.journal = NULL;
    fprintf(stderr, "osh: effect journal unavailable (%s): every command that starts a program or writes a file will be refused\n", what);
}

static int journal_attach(OshShell *sh)
{
    char def[4096], why[4300];
    const char *path = g_journal_file;
    sh->s.journal_required = 1;
    if (!path) {
        if (osh_journal_default_path(getenv("XDG_STATE_HOME"), getenv("HOME"), def, sizeof def) != 0) {
            journal_unavailable(sh, "neither XDG_STATE_HOME nor HOME names an absolute directory; use --journal FILE");
            return 0;
        }
        int mk = osh_journal_mkparents(def);
        if (mk) { snprintf(why, sizeof why, "%s: %s", def, strerror(-mk)); journal_unavailable(sh, why); return 0; }
        path = def;
    }
    /* default or explicit: the directory holding the journal must be ours and private */
    {
        char dir[4200];
        snprintf(dir, sizeof dir, "%s", path);
        char *sl = strrchr(dir, '/');
        if (!sl) strcpy(dir, ".");
        else if (sl == dir) dir[1] = 0;
        else *sl = 0;
        if (osh_journal_check_dir(&g_journal, dir)) { journal_unavailable(sh, g_journal.why); return 0; }
    }
    int rc = osh_journal_open(&g_journal, path);
    if (rc) { journal_unavailable(sh, g_journal.why); return 0; }
    OshJournalUnknown u[16];
    int bad = 0, n = osh_journal_recover(&g_journal, path, u, 16, &bad);
    if (n < 0) { snprintf(why, sizeof why, "%s: recovery failed: %s", path, strerror(-n)); journal_unavailable(sh, why); return 0; }
    for (int i = 0; i < n && i < 16; i++)
        fprintf(stderr, "osh: journal: effect %d/%d of request %.16s... (record %llu, argv0 %s) has outcome UNKNOWN; it is not re-run\n", u[i].idx + 1, u[i].ncmds, u[i].digest, (unsigned long long)u[i].intent_rec, u[i].argv0);
    if (n > 16) fprintf(stderr, "osh: journal: and %d more UNKNOWN\n", n - 16);
    if (bad) fprintf(stderr, "osh: journal: %d unreadable line(s) ignored\n", bad);
    sh->s.journal = &g_journal;
    return 0;
}

static char *slurp_fd(int fd, size_t *n)
{
    size_t cap = 4096, len = 0;
    char *b = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        if (!b) return NULL;
        ssize_t r = read(fd, b + len, cap - len);
        if (r < 0) { free(b); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
    }
    *n = len;
    return b;
}

/* With no --caps the policy is DEFAULT_POLICY: a principal and no grants, so every spawn, redirection open and cd is
 * denied (fail closed). Authority comes only from a policy file named on the command line by whoever starts osh. */
static const char DEFAULT_POLICY[] = "principal 1\n";

/* Read the policy (or take the default), start the root, mint, bind the session, then seal: nothing in this process
 * can mint or revoke after. */
static int arm(OshShell *sh, OshCaps *caps, const char *file)
{
    char *z = NULL;
    if (journal_attach(sh) != 0) return -1;
    if (file) {
        int fd = open(file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) { fprintf(stderr, "osh: --caps %s: cannot open\n", file); return -1; }
        size_t n = 0;
        char *text = slurp_fd(fd, &n);
        close(fd);
        if (!text || n > (1u << 16)) { fprintf(stderr, "osh: --caps %s: unreadable or too large\n", file); free(text); return -1; }
        z = realloc(text, n + 1);
        if (!z) { free(text); return -1; }
        z[n] = 0;
        if (strlen(z) != n) { fprintf(stderr, "osh: --caps %s: contains a NUL byte\n", file); free(z); return -1; }
    } else {
        z = strdup(DEFAULT_POLICY);
        if (!z) return -1;
    }
    char err[256];
    int rc = osh_caps_start(caps);
    if (rc != 0) fprintf(stderr, "osh: capability root did not start (%s)\n", rx_cap_strerror(rc));
    else if (osh_caps_load(caps, z, err, sizeof err) != 0) { fprintf(stderr, "osh: --caps %s: %s\n", file ? file : "(default)", err); rc = -1; }
    free(z);
    if (rc != 0) return -1;
    osh_caps_attach(caps, &sh->s);
    osh_caps_seal(caps);
    return 0;
}

int main(int argc, char **argv)
{
    int native = 1;
    const char *ei = getenv("OSH_INTERP");
    if (ei && strcmp(ei, "1") == 0) native = 0;
    const char *caps_file = NULL;
    while (argc >= 3 && (strcmp(argv[1], "--caps") == 0 || strcmp(argv[1], "--journal") == 0)) {
        if (argv[1][2] == 'c') caps_file = argv[2];
        else g_journal_file = argv[2];
        argv[2] = argv[0];
        argv += 2;
        argc -= 2;
    }
    OshShell sh;
    OshCaps caps = {0};
    int rc;
    if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
        if (argc < 3) { fprintf(stderr, "osh: -c needs a command string\n"); return 2; }
        const char *arg0 = argc > 3 ? argv[3] : "osh";
        if (osh_shell_init(&sh, environ, native, 0, arg0, argc > 4 ? argv + 4 : NULL, argc > 4 ? argc - 4 : 0) != 0) return 70;
        if (arm(&sh, &caps, caps_file) != 0) { osh_shell_free(&sh); osh_caps_stop(&caps); return 70; }
        rc = osh_shell_run_string(&sh, argv[2], strlen(argv[2]));
    } else if (argc >= 2) {
        int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
        if (fd < 0) { fprintf(stderr, "osh: %s: cannot open\n", argv[1]); return 127; }
        size_t n = 0;
        char *text = slurp_fd(fd, &n);
        close(fd);
        if (!text) { fprintf(stderr, "osh: %s: cannot read\n", argv[1]); return 127; }
        if (osh_shell_init(&sh, environ, native, 0, argv[1], argc > 2 ? argv + 2 : NULL, argc > 2 ? argc - 2 : 0) != 0) return 70;
        if (arm(&sh, &caps, caps_file) != 0) { osh_shell_free(&sh); osh_caps_stop(&caps); return 70; }
        rc = osh_shell_run_script(&sh, text, n);
        free(text);
    } else {
        int tty = isatty(0) && isatty(2);
        if (osh_shell_init(&sh, environ, native, tty, "osh", NULL, 0) != 0) return 70;
        if (arm(&sh, &caps, caps_file) != 0) { osh_shell_free(&sh); osh_caps_stop(&caps); return 70; }
        rc = osh_shell_run_fd(&sh, 0);
    }
    osh_shell_free(&sh);
    osh_caps_stop(&caps);
    osh_journal_close(&g_journal);
    return rc & 255;
}
