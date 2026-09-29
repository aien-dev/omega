/*
 * visor_parse_command.h -- Omega Visor console line parser (lane 2).
 *
 * Pure tokenizer: no semantic meaning, no session access, deterministic.
 *
 * Rules:
 *  - A trailing "\n" / "\r\n" is stripped; leading/trailing blanks are ignored.
 *  - Blank lines and lines whose first non-blank byte is '#' are EMPTY.
 *  - Bytes must be valid UTF-8; control bytes (< 0x20 except tab, and 0x7F) are rejected.
 *  - Lines longer than VISOR_LINE_MAX-1 bytes are rejected (never truncated).
 *  - If the first word is a command word (help, quit, inspect, ...) the rest of
 *    the line is split on blanks into at most VISOR_MAX_ARGS arguments, each at
 *    most VISOR_TOKEN_MAX-1 bytes; arity is checked against the command table.
 *    `realize <x> for current.machine` sets for_current_machine and leaves
 *    args = { x }. Any other `for <target>` is rejected.
 *  - Anything else (e.g. `let x = 3 * 6`, `fn f(x) = x + 1`, `x + 1`) is SOURCE:
 *    the whole trimmed line is kept in `line` and NOT tokenized.
 *  - Malformed input returns rc -1 with an error string (no command-name prefix):
 *    control bytes, invalid UTF-8 or an over-long line -> cmd = UNKNOWN;
 *    a recognised command word with a bad/over-long argument or wrong arity ->
 *    cmd keeps that command so the console can name it. A mistyped command word
 *    ("helpp") is SOURCE, because it is not a command word.
 *  - `_` is passed through as an ordinary token; resolution happens in the console.
 */
#ifndef OMEGA_VISOR_PARSE_COMMAND_H
#define OMEGA_VISOR_PARSE_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#define VISOR_MAX_ARGS  4
#define VISOR_TOKEN_MAX 256     /* max token length 255 bytes + NUL */
#define VISOR_LINE_MAX  4096    /* max line length 4095 bytes + NUL */

typedef enum {
    VISOR_CMD_HELP = 0,
    VISOR_CMD_QUIT,
    VISOR_CMD_INSPECT,
    VISOR_CMD_TYPE,
    VISOR_CMD_ID,
    VISOR_CMD_GRAPH,
    VISOR_CMD_VERIFY,
    VISOR_CMD_MACHINE,
    VISOR_CMD_REALIZE,
    VISOR_CMD_COST,
    VISOR_CMD_RUN,
    VISOR_CMD_EVIDENCE,
    VISOR_CMD_BINDINGS,
    VISOR_CMD_CLEAR,
    VISOR_CMD_WORLD,
    VISOR_CMD_EFFECTS,
    VISOR_CMD_ALTERNATIVES,
    VISOR_CMD_COMPARE,
    VISOR_CMD_WHY,
    VISOR_CMD_SOURCE,   /* not a command word: a source line for the language lane */
    VISOR_CMD_EMPTY,    /* blank or comment */
    VISOR_CMD_UNKNOWN,  /* malformed (see rules above) */
    VISOR_CMD__COUNT
} VisorCmd;

typedef struct {
    VisorCmd cmd;
    char args[VISOR_MAX_ARGS][VISOR_TOKEN_MAX];
    size_t argc;
    bool for_current_machine;       /* `realize x for current.machine` */
    char line[VISOR_LINE_MAX];      /* trimmed line (always set when rc == 0) */
} VisorCommand;

/* Parse one line. Returns 0 on success; -1 on malformed input, in which case
 * out->cmd is VISOR_CMD_UNKNOWN unless a command word was recognised (then it
 * keeps that command so the error can name it) and `err` holds a message. */
int visor_parse_command(const char *line, VisorCommand *out, char *err, size_t errn);

/* Command word for a VisorCmd ("help", ..., "source", "empty", "unknown"). */
const char *visor_cmd_name(VisorCmd c);

/* Arity bounds (number of args after the command word). Returns -1 for
 * SOURCE/EMPTY/UNKNOWN. */
int visor_cmd_arity(VisorCmd c, size_t *min_args, size_t *max_args);

#endif /* OMEGA_VISOR_PARSE_COMMAND_H */
