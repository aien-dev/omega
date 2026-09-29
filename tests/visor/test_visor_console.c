/* test_visor_console.c -- lane 2: parser + console framework tests (Omega Visor V1). */
#include "visor.h"
#include "visor_console.h"
#include "visor_parse_command.h"
#include "omega_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_total;

#define CHECK(cond, name) do { \
    g_total++; \
    if (cond) g_pass++; \
    else fprintf(stderr, "FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
} while (0)

static const char ZERO_ID[] = "sha256:0000000000000000000000000000000000000000000000000000000000000000";

/* ---------------- parser ---------------- */

typedef struct {
    const char *line;
    int rc;
    VisorCmd cmd;
    size_t argc;
    const char *a0, *a1;
    bool for_cur;
} ParseCase;

static void test_parser(void) {
    static const ParseCase cases[] = {
        { "help", 0, VISOR_CMD_HELP, 0, NULL, NULL, false },
        { "help id\n", 0, VISOR_CMD_HELP, 1, "id", NULL, false },
        { "quit\r\n", 0, VISOR_CMD_QUIT, 0, NULL, NULL, false },
        { "  inspect   x  ", 0, VISOR_CMD_INSPECT, 1, "x", NULL, false },
        { "type _", 0, VISOR_CMD_TYPE, 1, "_", NULL, false },
        { "id _", 0, VISOR_CMD_ID, 1, "_", NULL, false },
        { "graph", 0, VISOR_CMD_GRAPH, 0, NULL, NULL, false },
        { "graph y", 0, VISOR_CMD_GRAPH, 1, "y", NULL, false },
        { "verify f", 0, VISOR_CMD_VERIFY, 1, "f", NULL, false },
        { "machine", 0, VISOR_CMD_MACHINE, 0, NULL, NULL, false },
        { "realize x", 0, VISOR_CMD_REALIZE, 1, "x", NULL, false },
        { "realize x for current.machine", 0, VISOR_CMD_REALIZE, 1, "x", NULL, true },
        { "cost x", 0, VISOR_CMD_COST, 1, "x", NULL, false },
        { "run f 3 4", 0, VISOR_CMD_RUN, 3, "f", "3", false },
        { "evidence", 0, VISOR_CMD_EVIDENCE, 0, NULL, NULL, false },
        { "evidence VISOR", 0, VISOR_CMD_EVIDENCE, 1, "VISOR", NULL, false },
        { "bindings", 0, VISOR_CMD_BINDINGS, 0, NULL, NULL, false },
        { "clear", 0, VISOR_CMD_CLEAR, 0, NULL, NULL, false },
        { "world", 0, VISOR_CMD_WORLD, 0, NULL, NULL, false },
        { "effects x", 0, VISOR_CMD_EFFECTS, 1, "x", NULL, false },
        { "alternatives x", 0, VISOR_CMD_ALTERNATIVES, 1, "x", NULL, false },
        { "compare a b", 0, VISOR_CMD_COMPARE, 2, "a", "b", false },
        { "why\tx", 0, VISOR_CMD_WHY, 1, "x", NULL, false },
        { "let x = 3 * 6", 0, VISOR_CMD_SOURCE, 0, NULL, NULL, false },
        { "fn f(x) = x + 1", 0, VISOR_CMD_SOURCE, 0, NULL, NULL, false },
        { "x + 1", 0, VISOR_CMD_SOURCE, 0, NULL, NULL, false },
        { "helpp", 0, VISOR_CMD_SOURCE, 0, NULL, NULL, false },
        { "let \xCE\xA9 = 1", 0, VISOR_CMD_SOURCE, 0, NULL, NULL, false },
        { "", 0, VISOR_CMD_EMPTY, 0, NULL, NULL, false },
        { "   \t ", 0, VISOR_CMD_EMPTY, 0, NULL, NULL, false },
        { "# comment", 0, VISOR_CMD_EMPTY, 0, NULL, NULL, false },
        /* malformed */
        { "id", -1, VISOR_CMD_ID, 0, NULL, NULL, false },
        { "id a b", -1, VISOR_CMD_ID, 0, NULL, NULL, false },
        { "quit now", -1, VISOR_CMD_QUIT, 0, NULL, NULL, false },
        { "run a b c d e", -1, VISOR_CMD_RUN, 0, NULL, NULL, false },
        { "run a b c d e f g", -1, VISOR_CMD_RUN, 0, NULL, NULL, false },
        { "realize x for mars", -1, VISOR_CMD_REALIZE, 0, NULL, NULL, false },
        { "inspect x\x01", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false },
        { "let x = 1\x1b[2J", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false },
        { "let x = \x7f", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false },
        { "id \xff", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false },
        { "id \xC0\xAF", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false },     /* overlong '/' */
        { "id \xED\xA0\x80", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false }, /* surrogate */
        { "id \xCE", -1, VISOR_CMD_UNKNOWN, 0, NULL, NULL, false },         /* truncated */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ParseCase *pc = &cases[i];
        VisorCommand cmd;
        char err[256];
        int rc = visor_parse_command(pc->line, &cmd, err, sizeof(err));
        char name[96];
        snprintf(name, sizeof(name), "parse case %zu", i);
        bool ok = (rc == pc->rc) && (cmd.cmd == pc->cmd);
        if (ok && rc == 0) {
            ok = cmd.argc == pc->argc && cmd.for_current_machine == pc->for_cur;
            if (pc->a0) ok = ok && strcmp(cmd.args[0], pc->a0) == 0;
            if (pc->a1) ok = ok && strcmp(cmd.args[1], pc->a1) == 0;
        }
        if (ok && rc != 0) ok = err[0] != '\0';
        CHECK(ok, name);
    }

    /* SOURCE keeps the whole trimmed line, untokenized. */
    VisorCommand cmd;
    char err[256];
    CHECK(visor_parse_command("  let x = 3 * 6  \n", &cmd, err, sizeof(err)) == 0 &&
          strcmp(cmd.line, "let x = 3 * 6") == 0 && cmd.argc == 0, "source line kept");

    /* Oversized token: 256 bytes rejected (not truncated), 255 accepted. */
    char big[300];
    memcpy(big, "id ", 3);
    memset(big + 3, 'a', 256);
    big[3 + 256] = '\0';
    CHECK(visor_parse_command(big, &cmd, err, sizeof(err)) == -1 &&
          strstr(err, "too long") != NULL, "oversized token rejected");
    big[3 + 255] = '\0';
    CHECK(visor_parse_command(big, &cmd, err, sizeof(err)) == 0 && strlen(cmd.args[0]) == 255,
          "255-byte token accepted");

    /* Over-long line rejected, not truncated. */
    char *longline = malloc(VISOR_LINE_MAX + 8);
    memset(longline, 'x', VISOR_LINE_MAX + 4);
    longline[VISOR_LINE_MAX + 4] = '\0';
    CHECK(visor_parse_command(longline, &cmd, err, sizeof(err)) == -1 && strstr(err, "line too long"),
          "oversized line rejected");
    free(longline);

    /* Deterministic: same input, identical struct bytes. */
    VisorCommand c1, c2;
    visor_parse_command("run f 1 2", &c1, err, sizeof(err));
    visor_parse_command("run f 1 2", &c2, err, sizeof(err));
    CHECK(memcmp(&c1, &c2, sizeof(c1)) == 0, "parser deterministic");

    CHECK(strcmp(visor_cmd_name(VISOR_CMD_ALTERNATIVES), "alternatives") == 0 &&
          strcmp(visor_cmd_name(VISOR_CMD_SOURCE), "source") == 0, "cmd names");
}

/* ---------------- console harness ---------------- */

typedef struct {
    VisorSession s;
    VisorConsole c;
    FILE *f;
    char *buf;
    size_t len;
} Harness;

static void h_open(Harness *h, bool json) {
    memset(h, 0, sizeof(*h));
    if (visor_session_init(&h->s) != 0) { fprintf(stderr, "session init failed\n"); exit(1); }
    h->f = open_memstream(&h->buf, &h->len);
    visor_console_init(&h->c, &h->s, h->f, h->f, json);   /* out and err share one stream */
}

/* Returns the output of this one line (caller frees). */
static char *h_exec(Harness *h, const char *line, int *rc) {
    size_t before = h->len;
    fflush(h->f);
    before = h->len;
    int r = visor_console_exec_line(&h->c, line);
    fflush(h->f);
    if (rc) *rc = r;
    return strndup(h->buf + before, h->len - before);
}

static void h_close(Harness *h) {
    fclose(h->f);
    free(h->buf);
    visor_session_destroy(&h->s);
}

static void expect(Harness *h, const char *line, int want_rc, const char *want, const char *name) {
    int rc;
    char *got = h_exec(h, line, &rc);
    bool ok = rc == want_rc && strcmp(got, want) == 0;
    if (!ok) fprintf(stderr, "--- %s: rc=%d want %d\n--- got:\n%s--- want:\n%s", name, rc, want_rc, got, want);
    CHECK(ok, name);
    free(got);
}

static void fake_id(SemanticId *id, unsigned char seed) {
    for (size_t i = 0; i < sizeof(id->bytes); i++) id->bytes[i] = (unsigned char)(seed + i);
}

static void test_console_human(void) {
    Harness h;
    h_open(&h, false);

    /* help: exact first/last lines + one entry per command word. */
    int rc;
    char *help = h_exec(&h, "help", &rc);
    CHECK(rc == 0 && strncmp(help, "Omega Visor commands (classes: inspection, pure-execution, simulation, effect-request):\n", 87) == 0,
          "help header");
    static const char *words[] = { "help", "quit", "inspect", "type", "id", "graph", "verify", "machine", "realize",
                                   "cost", "run", "evidence", "bindings", "clear", "world", "effects", "alternatives",
                                   "compare", "why", "let x" };
    size_t lines = 0;
    for (const char *p = help; *p; p++) if (*p == '\n') lines++;
    CHECK(lines == 22, "help line count");
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "\n  %s", words[i]);
        CHECK(strstr(help, needle) != NULL, "help lists command");
    }
    CHECK(strstr(help, "  run <x> [args...]                    pure-execution  run x if pure; effects become an effect request\n") != NULL,
          "help row exact");
    free(help);
    expect(&h, "help id", 0,
           "  id <x>                               inspection      show the semantic id of a name, _ or hex id\n",
           "help one");
    expect(&h, "help nope", 1, "error: help: no such command 'nope'\n", "help unknown");

    /* bindings empty, then after visor_binding_set (insertion order). */
    expect(&h, "bindings", 0, "(no bindings)\n", "bindings empty");
    SemanticId a, b;
    fake_id(&a, 0x10);
    fake_id(&b, 0xA0);
    CHECK(visor_binding_set(&h.s, "zeta", VISOR_BIND_OBJECT, &a, -1) == 0, "bind zeta");
    CHECK(visor_binding_set(&h.s, "alpha", VISOR_BIND_PROGRAM, &b, 0) == 0, "bind alpha");
    char ida[72], idb[72];
    visor_format_id(&a, ida);
    visor_format_id(&b, idb);
    char want[512];
    snprintf(want, sizeof(want), "zeta  object  %s\nalpha  program  %s\n", ida, idb);
    expect(&h, "bindings", 0, want, "bindings insertion order");

    /* id of a binding, unknown name, hex not in graph (fail closed), no `_` yet. */
    snprintf(want, sizeof(want), "%s\n", ida);
    expect(&h, "id zeta", 0, want, "id binding");
    expect(&h, "id nope", 1, "error: id: unknown name or id 'nope'\n", "id unknown");
    snprintf(want, sizeof(want), "error: id: unknown name or id '%s'\n", ZERO_ID);
    expect(&h, ZERO_ID + 0, 1, "error: source: not wired in this build\n", "bare hex is source");
    char line[128];
    snprintf(line, sizeof(line), "id %s", ZERO_ID);
    expect(&h, line, 1, want, "id hex not in graph");
    expect(&h, "id _", 1, "error: id: unknown name or id '_'\n", "id _ unset");

    /* hex id of an object that IS in the graph resolves. */
    OmegaObject *t = omega_build_type_uint(h.s.graph, 64);
    CHECK(t != NULL, "build type");
    if (t) {
        char tid[72];
        visor_format_id(&t->id, tid);
        snprintf(line, sizeof(line), "id %s", tid);
        snprintf(want, sizeof(want), "%s\n", tid);
        expect(&h, line, 0, want, "id hex in graph");
    }

    /* not-wired shapes (phase A default ops). */
    expect(&h, "inspect zeta", 1, "error: inspect: not wired in this build\n", "inspect not wired");
    expect(&h, "let y = 1", 1, "error: source: not wired in this build\n", "source not wired");
    expect(&h, "run zeta", 1, "error: run: not wired in this build\n", "run not wired");
    expect(&h, "machine", 1, "error: machine: not wired in this build\n", "machine not wired");

    /* malformed / empty */
    expect(&h, "", 0, "", "empty line");
    expect(&h, "# note", 0, "", "comment line");
    expect(&h, "compare a", 1, "error: compare: expected 2 arguments, got 1\n", "arity error");
    expect(&h, "id \x01", 1, "error: unknown: control character 0x01 at byte 3 is not allowed\n", "control char");

    /* clear then quit */
    expect(&h, "clear", 0, "session cleared\n", "clear");
    expect(&h, "bindings", 0, "(no bindings)\n", "bindings after clear");
    CHECK(!h.c.quit, "not quit yet");
    expect(&h, "quit", 0, "bye\n", "quit");
    CHECK(h.c.quit, "quit sets flag");
    h_close(&h);
}

/* ---------------- stub ops (phase-B contract exercised now) ---------------- */

static int stub_eval(VisorSession *s, const char *line, VisorBinding *out, char *value, size_t n,
                     char *err, size_t errn) {
    if (strcmp(line, "bad") == 0) { snprintf(err, errn, "parse error at 1:1"); return 1; }
    SemanticId id;
    fake_id(&id, 0x33);
    if (strncmp(line, "let ", 4) == 0) visor_binding_set(s, "x", VISOR_BIND_OBJECT, &id, -1);
    memset(out, 0, sizeof(*out));
    out->kind = VISOR_BIND_OBJECT;
    out->id = id;
    out->index = -1;
    snprintf(value, n, "18");
    return 0;
}

static int stub_classify(const VisorViewCall *call, VisorClass *cls, char *reason, size_t rn,
                         char *err, size_t errn) {
    (void)err; (void)errn;
    if (strcmp(call->cmd->args[0], "x") == 0) { *cls = VISOR_CLASS_PURE_EXECUTION; snprintf(reason, rn, "no effects"); }
    else { *cls = VISOR_CLASS_EFFECT_REQUEST; snprintf(reason, rn, "writes a file"); }
    return 0;
}

static int g_run_calls;
static int stub_run(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    (void)err; (void)errn;
    g_run_calls++;
    if (call->json) fputs("{\"value\":\"18\"}", out); else fputs("18\n", out);
    return 0;
}

static int stub_cost(const VisorViewCall *call, FILE *out, char *err, size_t errn) {
    (void)call; (void)out;
    snprintf(err, errn, "no \"estimate\"");
    return 1;
}

static void install_stubs(Harness *h) {
    VisorConsoleOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.eval_line = stub_eval;
    ops.effects_classify = stub_classify;
    ops.run_pure = stub_run;
    ops.cost = stub_cost;
    visor_console_set_ops(&h->c, &ops);
}

static void test_console_stubs(void) {
    Harness h;
    h_open(&h, false);
    install_stubs(&h);
    expect(&h, "let x = 3 * 6", 0, "18\n", "source ok");
    CHECK(h.s.has_last && strcmp(h.s.last.name, "_") == 0, "source sets _");
    SemanticId id;
    fake_id(&id, 0x33);
    char want[256], idt[72];
    visor_format_id(&id, idt);
    snprintf(want, sizeof(want), "%s\n", idt);
    expect(&h, "id _", 0, want, "id _ after source");
    snprintf(want, sizeof(want), "x  object  %s\n_  object  %s\n", idt, idt);
    expect(&h, "bindings", 0, want, "bindings shows _ last");
    expect(&h, "bad", 1, "error: source: parse error at 1:1\n", "source error");

    g_run_calls = 0;
    expect(&h, "run x", 0, "[pure-execution]\n18\n", "run pure");
    CHECK(g_run_calls == 1, "run_pure called once");
    SemanticId e;
    fake_id(&e, 0x44);
    visor_binding_set(&h.s, "w", VISOR_BIND_OBJECT, &e, -1);
    expect(&h, "run w", 1,
           "EFFECT REQUEST: requires authority; routed via EffectIntent -> AEGIS/PHYSICS (not executed); reason: writes a file\n",
           "run effect request");
    CHECK(g_run_calls == 1, "effect request never executed");
    expect(&h, "run nope", 1, "error: run: unknown name or id 'nope'\n", "run unresolved");
    CHECK(g_run_calls == 1, "unresolved never executed");
    expect(&h, "effects w", 0, "effect-request: writes a file\n", "effects view");
    expect(&h, "cost x", 1, "error: cost: no \"estimate\"\n", "hook error");
    h_close(&h);

    /* Classifier missing but run_pure present: fail closed. */
    h_open(&h, false);
    VisorConsoleOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.run_pure = stub_run;
    visor_console_set_ops(&h.c, &ops);
    g_run_calls = 0;
    expect(&h, "run _", 1, "error: run: not wired in this build\n", "run without classifier");
    CHECK(g_run_calls == 0, "run without classifier not executed");
    h_close(&h);
}

static void test_console_json(void) {
    Harness h;
    h_open(&h, true);
    install_stubs(&h);
    expect(&h, "bindings", 0, "{\"command\":\"bindings\",\"status\":\"ok\",\"class\":\"inspection\",\"result\":[],\"error\":null}\n",
           "json bindings empty");
    expect(&h, "help quit", 0,
           "{\"command\":\"help\",\"status\":\"ok\",\"class\":\"inspection\",\"result\":[{\"command\":\"quit\",\"usage\":\"quit\","
           "\"class\":\"inspection\",\"meaning\":\"leave the console\"}],\"error\":null}\n", "json help one");
    SemanticId id;
    fake_id(&id, 0x33);
    char idt[72], want[1024];
    visor_format_id(&id, idt);
    snprintf(want, sizeof(want),
             "{\"command\":\"source\",\"status\":\"ok\",\"class\":\"pure-execution\",\"result\":{\"value\":\"18\",\"kind\":\"object\",\"id\":\"%s\"},\"error\":null}\n", idt);
    expect(&h, "let x = 3 * 6", 0, want, "json source");
    snprintf(want, sizeof(want),
             "{\"command\":\"id\",\"status\":\"ok\",\"class\":\"inspection\",\"result\":{\"name\":\"x\",\"kind\":\"object\",\"id\":\"%s\"},\"error\":null}\n", idt);
    expect(&h, "id x", 0, want, "json id");
    snprintf(want, sizeof(want),
             "{\"command\":\"bindings\",\"status\":\"ok\",\"class\":\"inspection\",\"result\":[{\"name\":\"x\",\"kind\":\"object\",\"id\":\"%s\"},"
             "{\"name\":\"_\",\"kind\":\"object\",\"id\":\"%s\"}],\"error\":null}\n", idt, idt);
    expect(&h, "bindings", 0, want, "json bindings");
    expect(&h, "id nope", 1,
           "{\"command\":\"id\",\"status\":\"error\",\"class\":\"inspection\",\"result\":null,\"error\":\"unknown name or id 'nope'\"}\n",
           "json id error");
    expect(&h, "inspect x", 1,
           "{\"command\":\"inspect\",\"status\":\"error\",\"class\":\"inspection\",\"result\":null,\"error\":\"not wired in this build\"}\n",
           "json not wired");
    expect(&h, "run x", 0,
           "{\"command\":\"run\",\"status\":\"ok\",\"class\":\"pure-execution\",\"result\":{\"value\":\"18\"},\"error\":null}\n",
           "json run pure");
    SemanticId e;
    fake_id(&e, 0x44);
    visor_binding_set(&h.s, "w", VISOR_BIND_OBJECT, &e, -1);
    expect(&h, "run w", 1,
           "{\"command\":\"run\",\"status\":\"error\",\"class\":\"effect-request\",\"result\":null,\"error\":\"EFFECT REQUEST: requires authority; "
           "routed via EffectIntent -> AEGIS/PHYSICS (not executed); reason: writes a file\"}\n", "json effect request");
    expect(&h, "cost x", 1,
           "{\"command\":\"cost\",\"status\":\"error\",\"class\":\"simulation\",\"result\":null,\"error\":\"no \\\"estimate\\\"\"}\n",
           "json escaping");
    expect(&h, "quit", 0, "{\"command\":\"quit\",\"status\":\"ok\",\"class\":\"inspection\",\"result\":\"bye\",\"error\":null}\n", "json quit");
    h_close(&h);
}

/* ---------------- script mode ---------------- */

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0;
    FILE *m = open_memstream(&buf, &cap);
    int ch;
    while ((ch = fgetc(f)) != EOF) fputc(ch, m);
    fclose(f);
    fclose(m);
    *n = cap;
    return buf;
}

static void test_script(void) {
    const char *spath = "tests/visor/sessions/basic.omega-session";
    const char *epath = "tests/visor/sessions/basic.expected";
    size_t en = 0;
    char *expected = slurp(epath, &en);
    FILE *script = fopen(spath, "r");
    CHECK(expected && script, "session files present (run from repo root)");
    if (!expected || !script) { free(expected); if (script) fclose(script); return; }

    Harness h;
    h_open(&h, false);
    int rc = visor_console_run_script(&h.c, script);
    fflush(h.f);
    fclose(script);
    bool same = h.len == en && memcmp(h.buf, expected, en) == 0;
    if (!same) fprintf(stderr, "--- script got:\n%.*s--- expected:\n%s", (int)h.len, h.buf, expected);
    CHECK(same, "basic.omega-session matches basic.expected exactly");
    CHECK(rc == 1, "script with errors returns 1");
    CHECK(h.c.quit, "script stopped at quit");
    h_close(&h);

    /* JSON mode: no echo, exactly one object per executed command (12 before quit + quit). */
    script = fopen(spath, "r");
    h_open(&h, true);
    visor_console_run_script(&h.c, script);
    fflush(h.f);
    fclose(script);
    size_t objs = 0;
    bool shape = true;
    for (char *p = h.buf; p && *p;) {
        char *nl = strchr(p, '\n');
        if (!nl) { shape = false; break; }
        if (strncmp(p, "{\"command\":\"", 12) != 0 || nl[-1] != '}' || memchr(p, '\n', (size_t)(nl - p)) != NULL) shape = false;
        objs++;
        p = nl + 1;
    }
    CHECK(shape && objs == 12, "json script: one object per line");
    CHECK(strstr(h.buf, "\xCE\xA9>") == NULL, "json script: no echo");
    h_close(&h);

    free(expected);
}

int main(void) {
    test_parser();
    test_console_human();
    test_console_stubs();
    test_console_json();
    test_script();
    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
