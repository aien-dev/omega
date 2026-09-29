/*
 * visor_console.h -- Omega Visor console / REPL (lane 2).
 *
 * The console parses a line, classifies it, dispatches it to a hook in
 * VisorConsoleOps, and prints the result in a fixed shape. It never executes
 * anything itself and carries no authority. Wiring of the hooks to the other
 * lanes lives in tools/omega.c (visor_console_default_ops), so this module
 * stays dependency-free. A NULL hook prints "not wired in this build".
 *
 * Output classes (every command has exactly one):
 *   inspection      -- reads session / graph / files; changes nothing semantic
 *   pure-execution  -- evaluates or runs pure code (no effects, no authority)
 *   simulation      -- estimates / compares without executing
 *   effect-request  -- would need authority; routed as an EffectIntent, NEVER executed here
 *
 * Human mode: results go to `out`; errors go to `err` as "error: <command>: <msg>".
 *   A successful pure-execution / simulation command result (not a source line,
 *   which prints only its value) is preceded by one tag line
 *   "[<class>]" on `out` (inspection results are untagged). An effect request
 *   prints on `err`, without the "error:" prefix:
 *   "EFFECT REQUEST: requires authority; routed via EffectIntent -> AEGIS/PHYSICS (not executed)[; reason: <r>]".
 *   Script mode echoes each non-empty line as "Ω> <line>" on `out` first.
 * JSON mode: exactly one object per command on `out`, one line, no spaces, key order fixed:
 *   {"command":"<name>","status":"ok"|"error","class":"<class>","result":<json>,"error":<string>|null}
 *   `result` is null on error. JSON is a serialization of a view, never canonical bytes.
 * Blank lines and '#' comments produce no output in either mode.
 */
#ifndef OMEGA_VISOR_CONSOLE_H
#define OMEGA_VISOR_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "visor.h"
#include "visor_parse_command.h"

typedef enum {
    VISOR_CLASS_INSPECTION = 0,
    VISOR_CLASS_PURE_EXECUTION = 1,
    VISOR_CLASS_SIMULATION = 2,
    VISOR_CLASS_EFFECT_REQUEST = 3
} VisorClass;

const char *visor_class_name(VisorClass c);   /* "inspection", "pure-execution", ... */

/* Everything a view hook gets. `subject` is args[0] resolved via visor_resolve
 * (NULL if the command takes no subject or none was given); `subject2` is
 * args[1] resolved for `compare`. Raw tokens are in cmd->args. */
typedef struct {
    VisorSession *session;
    const VisorCommand *cmd;
    const VisorBinding *subject;
    const VisorBinding *subject2;
    bool json;                    /* true: write ONE JSON value; false: human text lines */
    const char *evidence_root;    /* from --evidence-root, may be NULL */
    void *ctx;                    /* VisorConsoleOps.ctx */
} VisorViewCall;

/* View hook: write the result to `out` (JSON value in json mode, text otherwise).
 * Return 0 on success; nonzero on error with a message in err (out is discarded). */
typedef int (*VisorViewFn)(const VisorViewCall *call, FILE *out, char *err, size_t errn);

/* Source-line hook (`let ...`, `fn ...`, bare expressions). On success fill
 * `out` (the result binding; kind NONE = nothing to remember) and `value_text`
 * (what to print, e.g. "18"; may be empty) and return 0. The console then sets
 * `_` via visor_set_last when out->kind != VISOR_BIND_NONE. Named bindings
 * (`let x = ...`) are created by the hook itself, not by the console. */
typedef int (*VisorEvalLineFn)(VisorSession *s, const char *line, VisorBinding *out,
                               char *value_text, size_t n, char *err, size_t errn);

/* Effect classifier used by `run` and `effects`. Set *cls to
 * VISOR_CLASS_PURE_EXECUTION or VISOR_CLASS_EFFECT_REQUEST and a one-line reason. */
typedef int (*VisorClassifyFn)(const VisorViewCall *call, VisorClass *cls,
                               char *reason, size_t reasonn, char *err, size_t errn);

typedef struct {
    VisorViewFn inspect;          /* inspect <x>            inspection */
    VisorViewFn type_of;          /* type <x>               inspection */
    VisorViewFn id_of;            /* (unused: `id` is built in; reserved) */
    VisorViewFn graph_text;       /* graph [x]              inspection */
    VisorEvalLineFn eval_line;    /* source lines           pure-execution */
    VisorViewFn verify;           /* verify <x>             pure-execution */
    VisorViewFn machine;          /* machine                inspection */
    VisorViewFn realize;          /* realize <x> [for current.machine]  pure-execution */
    VisorViewFn cost;             /* cost <x>               simulation */
    VisorViewFn run_pure;         /* run <x> [args..] after classify says pure   pure-execution */
    VisorViewFn evidence;         /* evidence [name]        inspection */
    VisorViewFn world;            /* world [cap]            inspection */
    VisorClassifyFn effects_classify; /* effects <x>; gate for `run`   inspection */
    VisorViewFn alternatives;     /* alternatives <x>       simulation */
    VisorViewFn compare;          /* compare <a> <b>        simulation */
    VisorViewFn why;              /* why <x>                inspection */
    void *ctx;                    /* passed through in VisorViewCall.ctx */
} VisorConsoleOps;

typedef struct {
    VisorSession *session;
    bool json;
    FILE *out;
    FILE *err;
    bool quit;
    int last_status;              /* 0 ok, 1 error, for the last executed command */
    size_t error_count;           /* errors since init */
    VisorConsoleOps ops;          /* zeroed by init; install with visor_console_set_ops */
    const char *evidence_root;    /* may be NULL */
} VisorConsole;

int  visor_console_init(VisorConsole *c, VisorSession *s, FILE *out, FILE *err, bool json);
void visor_console_set_ops(VisorConsole *c, const VisorConsoleOps *ops);

/* One line -> parse -> dispatch -> print. 0 ok, 1 command error. Never exits. */
int  visor_console_exec_line(VisorConsole *c, const char *line);

/* Batch mode: each non-empty line echoed as "Ω> <line>" on out (human mode
 * only), then its output. Stops after `quit`. Returns 0 if every command was
 * ok, 1 otherwise. */
int  visor_console_run_script(VisorConsole *c, FILE *script);

/* Interactive loop. Prints "Ω> " before each read when `in` is a tty and not
 * in JSON mode. Stops at EOF or `quit`. Same return convention as run_script. */
int  visor_console_repl(VisorConsole *c, FILE *in);

/* Implemented in tools/omega.c (the main), not in the console: fills the
 * hooks with the other lanes' functions. Phase A: all NULL. */
void visor_console_default_ops(VisorConsoleOps *ops);

/* Write `s` as a JSON string literal (quotes + escapes). Exposed for hooks. */
void visor_json_string(FILE *out, const char *s);

#endif /* OMEGA_VISOR_CONSOLE_H */
