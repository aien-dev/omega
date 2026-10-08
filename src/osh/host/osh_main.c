/*
 * osh_main.c -- the osh program (aien-architecture#158).
 *   osh                         read commands from standard input (prompts only when it is a terminal)
 *   osh -c 'string' [arg0 [args...]]
 *   osh script [args...]
 * A leading `--caps POLICYFILE` enforces the launch policy (osh_caps.h): the host starts omega's capability root, mints
 * the grants, seals the admin handle, and every spawn / redirection open / cd is checked at the moment of the effect.
 * Without it the policy is a principal with no grants: every such effect is denied (fail closed). Builtins that make
 * no effect (printf, pwd, export, unset, exit) still run.
 * OSH_INTERP=1 in the environment runs the shell core in the reference interpreter instead of native code.
 * Never runs a command through another shell: programs are started by the host adapter with execve().
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "osh_caps.h"
#include "osh_shell.h"

extern char **environ;

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
    if (argc >= 3 && strcmp(argv[1], "--caps") == 0) {
        caps_file = argv[2];
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
        rc = osh_shell_run_string(&sh, text, n);
        free(text);
    } else {
        int tty = isatty(0) && isatty(2);
        if (osh_shell_init(&sh, environ, native, tty, "osh", NULL, 0) != 0) return 70;
        if (arm(&sh, &caps, caps_file) != 0) { osh_shell_free(&sh); osh_caps_stop(&caps); return 70; }
        rc = osh_shell_run_fd(&sh, 0);
    }
    osh_shell_free(&sh);
    osh_caps_stop(&caps);
    return rc & 255;
}
