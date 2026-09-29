/*
 * omega -- Omega Visor V1 terminal console.
 *
 *   omega [--json] [--command "<line>"]... [--script <file>|-] [--evidence-root <dir>]
 *
 * No arguments: interactive REPL on stdin. Exit code: 0 if every command was
 * ok, 1 if any command errored, 2 on usage error. The console only dispatches;
 * the hooks it dispatches to are installed here (visor_console_default_ops).
 */
#include "visor.h"
#include "visor_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Phase A: nothing wired yet. Phase B fills these with the other lanes' functions. */
void visor_console_default_ops(VisorConsoleOps *ops) {
    if (!ops) return;
    memset(ops, 0, sizeof(*ops));
}

static void usage(FILE *f) {
    fprintf(f, "usage: omega [--json] [--command \"<line>\"]... [--script <file>|-] [--evidence-root <dir>]\n");
}

int main(int argc, char **argv) {
    bool json = false;
    const char *script = NULL;
    const char *evidence_root = NULL;
    const char **commands = calloc((size_t)argc + 1, sizeof(*commands));
    size_t ncommands = 0;
    if (!commands) { fprintf(stderr, "omega: out of memory\n"); return 2; }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0) {
            json = true;
        } else if (strcmp(a, "--command") == 0 || strcmp(a, "--script") == 0 || strcmp(a, "--evidence-root") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "omega: %s needs a value\n", a); usage(stderr); free(commands); return 2; }
            const char *v = argv[++i];
            if (strcmp(a, "--command") == 0) commands[ncommands++] = v;
            else if (strcmp(a, "--script") == 0) {
                if (script) { fprintf(stderr, "omega: only one --script allowed\n"); free(commands); return 2; }
                script = v;
            } else evidence_root = v;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            free(commands);
            return 0;
        } else {
            fprintf(stderr, "omega: unknown argument '%s'\n", a);
            usage(stderr);
            free(commands);
            return 2;
        }
    }

    FILE *script_f = NULL;
    if (script) {
        if (strcmp(script, "-") == 0) script_f = stdin;
        else if (!(script_f = fopen(script, "r"))) {
            fprintf(stderr, "omega: cannot open script '%s'\n", script);
            free(commands);
            return 2;
        }
    }

    VisorSession *session = calloc(1, sizeof(*session));
    if (!session || visor_session_init(session) != 0) {
        fprintf(stderr, "omega: cannot create session\n");
        free(session); free(commands);
        if (script_f && script_f != stdin) fclose(script_f);
        return 2;
    }
    VisorConsole con;
    visor_console_init(&con, session, stdout, stderr, json);
    VisorConsoleOps ops;
    visor_console_default_ops(&ops);
    visor_console_set_ops(&con, &ops);
    con.evidence_root = evidence_root;

    int any_err = 0;
    /* --command lines run in order; an error does not stop later ones; quit does. */
    for (size_t i = 0; i < ncommands && !con.quit; i++) {
        if (visor_console_exec_line(&con, commands[i]) != 0) any_err = 1;
    }
    if (script_f && !con.quit) {
        if (visor_console_run_script(&con, script_f) != 0) any_err = 1;
    }
    if (!script_f && ncommands == 0) {
        if (!json && isatty(fileno(stdin)))
            printf("Omega Visor V1 -- type `help` for commands, `quit` to leave.\n");
        if (visor_console_repl(&con, stdin) != 0) any_err = 1;
    }

    if (script_f && script_f != stdin) fclose(script_f);
    visor_session_destroy(session);
    free(session);
    free(commands);
    return any_err ? 1 : 0;
}
