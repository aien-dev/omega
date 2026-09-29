/* visor_console.c -- Omega Visor console / REPL (lane 2). Dispatch only; no authority. */
#include "visor_console.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define VISOR_PROMPT "\xCE\xA9> "
#define VISOR_EFFECT_REQUEST_MSG \
    "EFFECT REQUEST: requires authority; routed via EffectIntent -> AEGIS/PHYSICS (not executed)"
#define VISOR_NOT_WIRED_MSG "not wired in this build"

/* ---------- command table: usage, class, meaning (source of truth for help + JSON class) ---------- */

typedef struct {
    VisorCmd cmd;
    const char *usage;
    VisorClass cls;
    const char *meaning;
} HelpEntry;

static const HelpEntry k_help[] = {
    { VISOR_CMD_HELP,         "help [command]",                  VISOR_CLASS_INSPECTION,     "list commands, or show one" },
    { VISOR_CMD_QUIT,         "quit",                            VISOR_CLASS_INSPECTION,     "leave the console" },
    { VISOR_CMD_INSPECT,      "inspect <x>",                     VISOR_CLASS_INSPECTION,     "show the object a name or id refers to" },
    { VISOR_CMD_TYPE,         "type <x>",                        VISOR_CLASS_INSPECTION,     "show the type of an object" },
    { VISOR_CMD_ID,           "id <x>",                          VISOR_CLASS_INSPECTION,     "show the semantic id of a name, _ or hex id" },
    { VISOR_CMD_GRAPH,        "graph <x>",                       VISOR_CLASS_INSPECTION,     "show the semantic graph reachable from x" },
    { VISOR_CMD_VERIFY,       "verify <x>",                      VISOR_CLASS_PURE_EXECUTION, "run the verification pipeline on x" },
    { VISOR_CMD_MACHINE,      "machine",                         VISOR_CLASS_INSPECTION,     "show the current machine model" },
    { VISOR_CMD_REALIZE,      "realize <x> [for current.machine]", VISOR_CLASS_PURE_EXECUTION, "build machine code for x (does not run it)" },
    { VISOR_CMD_COST,         "cost <x>",                        VISOR_CLASS_SIMULATION,     "estimate the cost of x" },
    { VISOR_CMD_RUN,          "run <x> [args...]",               VISOR_CLASS_PURE_EXECUTION, "run x if pure; effects become an effect request" },
    { VISOR_CMD_EVIDENCE,     "evidence [name]",                 VISOR_CLASS_INSPECTION,     "show recorded evidence receipts" },
    { VISOR_CMD_BINDINGS,     "bindings",                        VISOR_CLASS_INSPECTION,     "list session names in the order they were made" },
    { VISOR_CMD_CLEAR,        "clear",                           VISOR_CLASS_INSPECTION,     "forget all session names and objects" },
    { VISOR_CMD_WORLD,        "world [cap]",                     VISOR_CLASS_INSPECTION,     "show the runtime world state (read only)" },
    { VISOR_CMD_EFFECTS,      "effects [x]",                     VISOR_CLASS_EFFECT_REQUEST, "show effect objects as requests (never executed)" },
    { VISOR_CMD_ALTERNATIVES, "alternatives <x>",                VISOR_CLASS_SIMULATION,     "list other ways to realize x" },
    { VISOR_CMD_COMPARE,      "compare <a> <b>",                 VISOR_CLASS_SIMULATION,     "compare two objects or realizations" },
    { VISOR_CMD_WHY,          "why <x>",                         VISOR_CLASS_INSPECTION,     "explain where x came from" },
    { VISOR_CMD_SOURCE,       "let x: u64 = <expr> | fn .. | <expr>", VISOR_CLASS_PURE_EXECUTION, "Omega source line, evaluated by the language" },
};
#define K_HELP_N (sizeof(k_help) / sizeof(k_help[0]))

static const HelpEntry *help_for(VisorCmd cmd) {
    for (size_t i = 0; i < K_HELP_N; i++) if (k_help[i].cmd == cmd) return &k_help[i];
    return NULL;
}

static VisorClass class_for(VisorCmd cmd) {
    const HelpEntry *h = help_for(cmd);
    return h ? h->cls : VISOR_CLASS_INSPECTION;
}

const char *visor_class_name(VisorClass c) {
    switch (c) {
        case VISOR_CLASS_INSPECTION: return "inspection";
        case VISOR_CLASS_PURE_EXECUTION: return "pure-execution";
        case VISOR_CLASS_SIMULATION: return "simulation";
        case VISOR_CLASS_EFFECT_REQUEST: return "effect-request";
    }
    return "inspection";
}

static const char *bind_kind_name(VisorBindKind k) {
    switch (k) {
        case VISOR_BIND_OBJECT: return "object";
        case VISOR_BIND_PROGRAM: return "program";
        case VISOR_BIND_REALIZATION: return "realization";
        default: return "none";
    }
}

/* ---------- JSON ---------- */

void visor_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20 || *p == 0x7F) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
        }
    }
    fputc('"', out);
}

/* ---------- output ---------- */

/* body: in JSON mode a JSON value (NULL/empty -> null); in human mode text. */
static int emit_ok_tag(VisorConsole *c, const char *name, VisorClass cls, const char *body, bool tag) {
    if (c->json) {
        fprintf(c->out, "{\"command\":");
        visor_json_string(c->out, name);
        fprintf(c->out, ",\"status\":\"ok\",\"class\":\"%s\",\"result\":%s,\"error\":null}\n",
                visor_class_name(cls), (body && *body) ? body : "null");
    } else {
        if (tag && cls != VISOR_CLASS_INSPECTION) fprintf(c->out, "[%s]\n", visor_class_name(cls));
        if (body && *body) {
            fputs(body, c->out);
            if (body[strlen(body) - 1] != '\n') fputc('\n', c->out);
        }
    }
    c->last_status = 0;
    return 0;
}

static int emit_ok(VisorConsole *c, const char *name, VisorClass cls, const char *body) {
    return emit_ok_tag(c, name, cls, body, true);
}

/* Error; `body` (may be NULL) is a report printed despite the error
 * (human: on out before the error line; JSON: as `result`). */
static int emit_err_body(VisorConsole *c, const char *name, VisorClass cls, const char *msg, const char *body) {
    if (!msg || !*msg) msg = "failed";
    bool has_body = body && *body;
    if (!c->json && has_body) {
        fputs(body, c->out);
        if (body[strlen(body) - 1] != '\n') fputc('\n', c->out);
    }
    fflush(c->out); /* keep out/err ordered when both reach the same terminal or file */
    if (c->json) {
        fprintf(c->out, "{\"command\":");
        visor_json_string(c->out, name);
        fprintf(c->out, ",\"status\":\"error\",\"class\":\"%s\",\"result\":%s,\"error\":", visor_class_name(cls),
                has_body ? body : "null");
        visor_json_string(c->out, msg);
        fputs("}\n", c->out);
    } else if (strncmp(msg, "EFFECT REQUEST", 14) == 0) {
        fprintf(c->err, "%s\n", msg);
    } else {
        fprintf(c->err, "error: %s: %s\n", name, msg);
    }
    c->last_status = 1;
    c->error_count++;
    return 1;
}

static int emit_err(VisorConsole *c, const char *name, VisorClass cls, const char *msg) {
    return emit_err_body(c, name, cls, msg, NULL);
}

/* A growable text buffer backed by open_memstream. */
typedef struct { char *buf; size_t len; FILE *f; } MemOut;

static FILE *mem_open(MemOut *m) {
    m->buf = NULL; m->len = 0;
    m->f = open_memstream(&m->buf, &m->len);
    return m->f;
}
static void mem_close(MemOut *m) { if (m->f) { fclose(m->f); m->f = NULL; } }
static void mem_free(MemOut *m) { mem_close(m); free(m->buf); m->buf = NULL; }

/* ---------- built-in commands ---------- */

static void help_row_human(FILE *f, const HelpEntry *h) {
    fprintf(f, "  %-36s %-15s %s\n", h->usage, visor_class_name(h->cls), h->meaning);
}
static void help_row_json(FILE *f, const HelpEntry *h) {
    fputs("{\"command\":", f); visor_json_string(f, visor_cmd_name(h->cmd));
    fputs(",\"usage\":", f); visor_json_string(f, h->usage);
    fprintf(f, ",\"class\":\"%s\",\"meaning\":", visor_class_name(h->cls));
    visor_json_string(f, h->meaning);
    fputc('}', f);
}

static int cmd_help(VisorConsole *c, const VisorCommand *cmd) {
    const char *name = "help";
    const HelpEntry *one = NULL;
    if (cmd->argc == 1) {
        for (size_t i = 0; i < K_HELP_N; i++) {
            if (strcmp(visor_cmd_name(k_help[i].cmd), cmd->args[0]) == 0) { one = &k_help[i]; break; }
        }
        if (!one) {
            char msg[320];
            snprintf(msg, sizeof(msg), "no such command '%s'", cmd->args[0]);
            return emit_err(c, name, VISOR_CLASS_INSPECTION, msg);
        }
    }
    MemOut m;
    if (!mem_open(&m)) return emit_err(c, name, VISOR_CLASS_INSPECTION, "out of memory");
    if (c->json) {
        fputc('[', m.f);
        for (size_t i = 0, k = 0; i < K_HELP_N; i++) {
            if (one && &k_help[i] != one) continue;
            if (k++) fputc(',', m.f);
            help_row_json(m.f, &k_help[i]);
        }
        fputc(']', m.f);
    } else if (one) {
        help_row_human(m.f, one);
    } else {
        fputs("Omega Visor commands (classes: inspection, pure-execution, simulation, effect-request):\n", m.f);
        for (size_t i = 0; i < K_HELP_N; i++) help_row_human(m.f, &k_help[i]);
        fputs("Names: a session name, _ (last result), or a semantic id (64 hex, optional sha256: prefix).\n", m.f);
    }
    mem_close(&m);
    int rc = emit_ok(c, name, VISOR_CLASS_INSPECTION, m.buf);
    mem_free(&m);
    return rc;
}

static void binding_row(VisorConsole *c, FILE *f, const VisorBinding *b, bool first) {
    char id[72];
    visor_format_id(&b->id, id);
    if (c->json) {
        if (!first) fputc(',', f);
        fputs("{\"name\":", f); visor_json_string(f, b->name);
        fprintf(f, ",\"kind\":\"%s\",\"id\":\"%s\"}", bind_kind_name(b->kind), id);
    } else {
        fprintf(f, "%s  %s  %s\n", b->name, bind_kind_name(b->kind), id);
    }
}

static int cmd_bindings(VisorConsole *c) {
    const VisorSession *s = c->session;
    MemOut m;
    if (!mem_open(&m)) return emit_err(c, "bindings", VISOR_CLASS_INSPECTION, "out of memory");
    if (c->json) fputc('[', m.f);
    size_t shown = 0;
    for (size_t i = 0; i < s->bindings.count; i++, shown++) binding_row(c, m.f, &s->bindings.items[i], shown == 0);
    if (s->has_last) { binding_row(c, m.f, &s->last, shown == 0); shown++; }
    if (c->json) fputc(']', m.f);
    else if (shown == 0) fputs("(no bindings)\n", m.f);
    mem_close(&m);
    int rc = emit_ok(c, "bindings", VISOR_CLASS_INSPECTION, m.buf);
    mem_free(&m);
    return rc;
}

static int unresolved(VisorConsole *c, const char *name, VisorClass cls, const char *tok) {
    char msg[320];
    snprintf(msg, sizeof(msg), "unknown name or id '%s'", tok);
    return emit_err(c, name, cls, msg);
}

static int cmd_id(VisorConsole *c, const VisorCommand *cmd) {
    VisorBinding b;
    if (visor_resolve(c->session, cmd->args[0], &b) != 0) return unresolved(c, "id", VISOR_CLASS_INSPECTION, cmd->args[0]);
    char id[72];
    visor_format_id(&b.id, id);
    MemOut m;
    if (!mem_open(&m)) return emit_err(c, "id", VISOR_CLASS_INSPECTION, "out of memory");
    if (c->json) {
        fputs("{\"name\":", m.f); visor_json_string(m.f, cmd->args[0]);
        fprintf(m.f, ",\"kind\":\"%s\",\"id\":\"%s\"}", bind_kind_name(b.kind), id);
    } else {
        fprintf(m.f, "%s\n", id);
    }
    mem_close(&m);
    int rc = emit_ok(c, "id", VISOR_CLASS_INSPECTION, m.buf);
    mem_free(&m);
    return rc;
}

/* ---------- hook dispatch ---------- */

static int call_view(VisorConsole *c, const char *name, VisorClass cls, VisorViewFn fn,
                     const VisorCommand *cmd, const VisorBinding *subj, const VisorBinding *subj2) {
    if (!fn) return emit_err(c, name, cls, VISOR_NOT_WIRED_MSG);
    VisorViewCall call = { c->session, cmd, subj, subj2, c->json, c->evidence_root, c->ops.ctx };
    MemOut m;
    if (!mem_open(&m)) return emit_err(c, name, cls, "out of memory");
    char err[512] = {0};
    int rc = fn(&call, m.f, err, sizeof(err));
    mem_close(&m);
    if (rc == 0) rc = emit_ok(c, name, cls, m.buf);
    else if (rc == VISOR_VIEW_ERROR_WITH_OUTPUT) rc = emit_err_body(c, name, cls, err, m.buf);
    else rc = emit_err(c, name, cls, err);
    mem_free(&m);
    return rc;
}

/* Resolve args[i] if present. Returns 0 ok (out filled or *have=false), -1 unresolved. */
static int resolve_arg(VisorConsole *c, const VisorCommand *cmd, size_t i, VisorBinding *out, bool *have) {
    *have = false;
    if (cmd->argc <= i) return 0;
    if (visor_resolve(c->session, cmd->args[i], out) != 0) return -1;
    *have = true;
    return 0;
}

static int cmd_subject_view(VisorConsole *c, const VisorCommand *cmd, VisorViewFn fn) {
    const char *name = visor_cmd_name(cmd->cmd);
    VisorClass cls = class_for(cmd->cmd);
    if (!fn) return emit_err(c, name, cls, VISOR_NOT_WIRED_MSG);
    VisorBinding a, b; bool ha, hb;
    if (resolve_arg(c, cmd, 0, &a, &ha) != 0) return unresolved(c, name, cls, cmd->args[0]);
    if (cmd->cmd == VISOR_CMD_COMPARE && resolve_arg(c, cmd, 1, &b, &hb) != 0)
        return unresolved(c, name, cls, cmd->args[1]);
    if (cmd->cmd != VISOR_CMD_COMPARE) hb = false;
    return call_view(c, name, cls, fn, cmd, ha ? &a : NULL, hb ? &b : NULL);
}

/* `run`: classify FIRST; anything that is not provably pure is an effect request
 * and is never executed. No classifier -> not wired (fail closed). */
static int cmd_run(VisorConsole *c, const VisorCommand *cmd) {
    const char *name = "run";
    if (!c->ops.effects_classify) return emit_err(c, name, VISOR_CLASS_PURE_EXECUTION, VISOR_NOT_WIRED_MSG);
    VisorBinding a; bool ha;
    if (resolve_arg(c, cmd, 0, &a, &ha) != 0) return unresolved(c, name, VISOR_CLASS_PURE_EXECUTION, cmd->args[0]);
    VisorViewCall call = { c->session, cmd, &a, NULL, c->json, c->evidence_root, c->ops.ctx };
    VisorClass k = VISOR_CLASS_EFFECT_REQUEST;
    char reason[256] = {0}, err[512] = {0};
    if (c->ops.effects_classify(&call, &k, reason, sizeof(reason), err, sizeof(err)) != 0)
        return emit_err(c, name, VISOR_CLASS_PURE_EXECUTION, err);
    if (k != VISOR_CLASS_PURE_EXECUTION) {
        char msg[512];
        if (reason[0]) snprintf(msg, sizeof(msg), "%s; reason: %s", VISOR_EFFECT_REQUEST_MSG, reason);
        else snprintf(msg, sizeof(msg), "%s", VISOR_EFFECT_REQUEST_MSG);
        return emit_err(c, name, VISOR_CLASS_EFFECT_REQUEST, msg);
    }
    return call_view(c, name, VISOR_CLASS_PURE_EXECUTION, c->ops.run_pure, cmd, &a, NULL);
}

/* A failed source line whose shape is "<word> <word-or-_>..." (e.g. `authorize x`,
 * `execute _`) was most likely meant as a command: report it as unknown.
 * `let`/`fn` lines and expressions keep the language's own error. */
static bool looks_like_command(const char *line, char *word, size_t n) {
    size_t i = 0;
    if (!(isalpha((unsigned char)line[0]) || line[0] == '_')) return false;
    while (isalnum((unsigned char)line[i]) || line[i] == '_') i++;
    if (line[i] != ' ' && line[i] != '\t') return false;
    if ((i == 3 && strncmp(line, "let", 3) == 0) || (i == 2 && strncmp(line, "fn", 2) == 0)) return false;
    size_t j = i;
    while (line[j] == ' ' || line[j] == '\t') j++;
    if (!(isalnum((unsigned char)line[j]) || line[j] == '_')) return false;
    snprintf(word, n, "%.*s", (int)i, line);
    return true;
}

static int cmd_source(VisorConsole *c, const VisorCommand *cmd) {
    const char *name = "source";
    VisorClass cls = VISOR_CLASS_PURE_EXECUTION;
    if (!c->ops.eval_line) return emit_err(c, name, cls, VISOR_NOT_WIRED_MSG);
    VisorBinding b;
    memset(&b, 0, sizeof(b));
    b.kind = VISOR_BIND_NONE;
    b.index = -1;
    char value[1024] = {0}, err[512] = {0};
    if (c->ops.eval_line(c->session, cmd->line, &b, value, sizeof(value), err, sizeof(err)) != 0) {
        char word[VISOR_TOKEN_MAX];
        if (looks_like_command(cmd->line, word, sizeof(word))) {
            char msg[VISOR_TOKEN_MAX + 64];
            snprintf(msg, sizeof(msg), "unknown command or invalid source line: '%s'", word);
            return emit_err(c, "unknown", cls, msg);
        }
        return emit_err(c, name, cls, err);
    }
    value[sizeof(value) - 1] = '\0';
    bool has = b.kind != VISOR_BIND_NONE;
    if (has) visor_set_last(c->session, &b);
    char id[72] = {0};
    if (has) visor_format_id(&b.id, id);
    MemOut m;
    if (!mem_open(&m)) return emit_err(c, name, cls, "out of memory");
    if (c->json) {
        fputs("{\"value\":", m.f); visor_json_string(m.f, value);
        fprintf(m.f, ",\"kind\":\"%s\",\"id\":", bind_kind_name(b.kind));
        if (has) fprintf(m.f, "\"%s\"}", id); else fputs("null}", m.f);
    } else if (value[0]) {
        fprintf(m.f, "%s\n", value);
    } else if (has) {
        fprintf(m.f, "%s\n", id);
    }
    mem_close(&m);
    int rc = emit_ok_tag(c, name, cls, m.buf, false) /* source: value line only */;
    mem_free(&m);
    return rc;
}

/* ---------- public API ---------- */

int visor_console_init(VisorConsole *c, VisorSession *s, FILE *out, FILE *err, bool json) {
    if (!c || !s || !out || !err) return -1;
    memset(c, 0, sizeof(*c));
    c->session = s;
    c->out = out;
    c->err = err;
    c->json = json;
    return 0;
}

void visor_console_set_ops(VisorConsole *c, const VisorConsoleOps *ops) {
    if (!c) return;
    if (ops) c->ops = *ops; else memset(&c->ops, 0, sizeof(c->ops));
}

static int dispatch(VisorConsole *c, const VisorCommand *cmd) {
    const VisorConsoleOps *o = &c->ops;
    switch (cmd->cmd) {
        case VISOR_CMD_EMPTY: c->last_status = 0; return 0;
        case VISOR_CMD_HELP: return cmd_help(c, cmd);
        case VISOR_CMD_QUIT:
            c->quit = true;
            return emit_ok(c, "quit", VISOR_CLASS_INSPECTION, c->json ? "\"bye\"" : "bye");
        case VISOR_CMD_BINDINGS: return cmd_bindings(c);
        case VISOR_CMD_CLEAR:
            visor_session_clear(c->session);
            return emit_ok(c, "clear", VISOR_CLASS_INSPECTION, c->json ? "\"session cleared\"" : "session cleared");
        case VISOR_CMD_ID: return o->id_of ? cmd_subject_view(c, cmd, o->id_of) : cmd_id(c, cmd);
        case VISOR_CMD_INSPECT: return cmd_subject_view(c, cmd, o->inspect);
        case VISOR_CMD_TYPE: return cmd_subject_view(c, cmd, o->type_of);
        case VISOR_CMD_GRAPH: return cmd_subject_view(c, cmd, o->graph_text);
        case VISOR_CMD_VERIFY: return cmd_subject_view(c, cmd, o->verify);
        case VISOR_CMD_REALIZE: return cmd_subject_view(c, cmd, o->realize);
        case VISOR_CMD_COST: return cmd_subject_view(c, cmd, o->cost);
        case VISOR_CMD_ALTERNATIVES: return cmd_subject_view(c, cmd, o->alternatives);
        case VISOR_CMD_COMPARE: return cmd_subject_view(c, cmd, o->compare);
        case VISOR_CMD_WHY: return cmd_subject_view(c, cmd, o->why);
        case VISOR_CMD_MACHINE:
            return call_view(c, "machine", VISOR_CLASS_INSPECTION, o->machine, cmd, NULL, NULL);
        case VISOR_CMD_EVIDENCE: /* raw name, not resolved */
            return call_view(c, "evidence", VISOR_CLASS_INSPECTION, o->evidence, cmd, NULL, NULL);
        case VISOR_CMD_WORLD: /* raw cap name, not resolved */
            return call_view(c, "world", VISOR_CLASS_INSPECTION, o->world, cmd, NULL, NULL);
        case VISOR_CMD_EFFECTS: return cmd_subject_view(c, cmd, o->effects);
        case VISOR_CMD_RUN: return cmd_run(c, cmd);
        case VISOR_CMD_SOURCE: return cmd_source(c, cmd);
        default: return emit_err(c, "unknown", VISOR_CLASS_INSPECTION, "unknown command");
    }
}

int visor_console_exec_line(VisorConsole *c, const char *line) {
    if (!c) return 1;
    VisorCommand *cmd = calloc(1, sizeof(*cmd));
    if (!cmd) {
        fprintf(c->err, "error: out of memory\n");
        c->last_status = 1; c->error_count++;
        return 1;
    }
    char err[512];
    int rc;
    if (visor_parse_command(line, cmd, err, sizeof(err)) != 0)
        rc = emit_err(c, visor_cmd_name(cmd->cmd), class_for(cmd->cmd), err);
    else
        rc = dispatch(c, cmd);
    free(cmd);
    fflush(c->out);
    fflush(c->err);
    return rc;
}

static bool line_is_empty(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\0' || *p == '\n' || *p == '\r' || *p == '#';
}

static int run_loop(VisorConsole *c, FILE *in, bool echo, bool prompt) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int any_err = 0;
    for (;;) {
        if (prompt) { fputs(VISOR_PROMPT, c->out); fflush(c->out); }
        n = getline(&line, &cap, in);
        if (n < 0) { if (prompt) fputc('\n', c->out); break; }
        /* getline keeps embedded NULs; the parser would see a shorter line. Reject. */
        if ((size_t)n != strlen(line)) {
            if (echo) fputs(VISOR_PROMPT "<line with NUL byte>\n", c->out);
            emit_err(c, "unknown", VISOR_CLASS_INSPECTION, "NUL byte in line is not allowed");
            any_err = 1;
            continue;
        }
        if (line_is_empty(line)) continue;
        if (echo) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) l--;
            fprintf(c->out, VISOR_PROMPT "%.*s\n", (int)l, line);
        }
        if (visor_console_exec_line(c, line) != 0) any_err = 1;
        if (c->quit) break;
    }
    free(line);
    fflush(c->out);
    return any_err;
}

int visor_console_run_script(VisorConsole *c, FILE *script) {
    if (!c || !script) return 1;
    return run_loop(c, script, !c->json, false);
}

int visor_console_repl(VisorConsole *c, FILE *in) {
    if (!c || !in) return 1;
    return run_loop(c, in, false, !c->json && isatty(fileno(in)));
}
