/*
 * osh_main.c -- the osh program (aien-architecture#158).
 *   osh                         read commands from standard input (prompts only when it is a terminal)
 *   osh -c 'string' [arg0 [args...]]
 *   osh script [args...]
 * OSH_INTERP=1 in the environment runs the shell core in the reference interpreter instead of native code.
 * Never runs a command through another shell: programs are started by the host adapter with execve().
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    int native = 1;
    const char *ei = getenv("OSH_INTERP");
    if (ei && strcmp(ei, "1") == 0) native = 0;
    OshShell sh;
    int rc;
    if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
        if (argc < 3) { fprintf(stderr, "osh: -c needs a command string\n"); return 2; }
        const char *arg0 = argc > 3 ? argv[3] : "osh";
        if (osh_shell_init(&sh, environ, native, 0, arg0, argc > 4 ? argv + 4 : NULL, argc > 4 ? argc - 4 : 0) != 0) return 70;
        rc = osh_shell_run_string(&sh, argv[2], strlen(argv[2]));
    } else if (argc >= 2) {
        int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
        if (fd < 0) { fprintf(stderr, "osh: %s: cannot open\n", argv[1]); return 127; }
        size_t n = 0;
        char *text = slurp_fd(fd, &n);
        close(fd);
        if (!text) { fprintf(stderr, "osh: %s: cannot read\n", argv[1]); return 127; }
        if (osh_shell_init(&sh, environ, native, 0, argv[1], argc > 2 ? argv + 2 : NULL, argc > 2 ? argc - 2 : 0) != 0) return 70;
        rc = osh_shell_run_script(&sh, text, n);
        free(text);
    } else {
        int tty = isatty(0) && isatty(2);
        if (osh_shell_init(&sh, environ, native, tty, "osh", NULL, 0) != 0) return 70;
        rc = osh_shell_run_fd(&sh, 0);
    }
    osh_shell_free(&sh);
    return rc & 255;
}
