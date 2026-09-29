/* visor_parse_command.c -- Omega Visor console line parser (lane 2). No semantics. */
#include "visor_parse_command.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *word;
    VisorCmd cmd;
    size_t min_args, max_args;
} ParseEntry;

static const ParseEntry k_words[] = {
    { "help",         VISOR_CMD_HELP,         0, 1 },
    { "quit",         VISOR_CMD_QUIT,         0, 0 },
    { "inspect",      VISOR_CMD_INSPECT,      1, 1 },
    { "type",         VISOR_CMD_TYPE,         1, 1 },
    { "id",           VISOR_CMD_ID,           1, 1 },
    { "graph",        VISOR_CMD_GRAPH,        0, 1 },
    { "verify",       VISOR_CMD_VERIFY,       1, 1 },
    { "machine",      VISOR_CMD_MACHINE,      0, 0 },
    { "realize",      VISOR_CMD_REALIZE,      1, 1 },
    { "cost",         VISOR_CMD_COST,         1, 1 },
    { "run",          VISOR_CMD_RUN,          1, 4 },
    { "evidence",     VISOR_CMD_EVIDENCE,     0, 1 },
    { "bindings",     VISOR_CMD_BINDINGS,     0, 0 },
    { "clear",        VISOR_CMD_CLEAR,        0, 0 },
    { "world",        VISOR_CMD_WORLD,        0, 1 },
    { "effects",      VISOR_CMD_EFFECTS,      1, 1 },
    { "alternatives", VISOR_CMD_ALTERNATIVES, 1, 1 },
    { "compare",      VISOR_CMD_COMPARE,      2, 2 },
    { "why",          VISOR_CMD_WHY,          1, 1 },
};
#define K_WORDS_N (sizeof(k_words) / sizeof(k_words[0]))

const char *visor_cmd_name(VisorCmd c) {
    for (size_t i = 0; i < K_WORDS_N; i++) if (k_words[i].cmd == c) return k_words[i].word;
    switch (c) {
        case VISOR_CMD_SOURCE: return "source";
        case VISOR_CMD_EMPTY: return "empty";
        default: return "unknown";
    }
}

int visor_cmd_arity(VisorCmd c, size_t *min_args, size_t *max_args) {
    for (size_t i = 0; i < K_WORDS_N; i++) {
        if (k_words[i].cmd == c) {
            if (min_args) *min_args = k_words[i].min_args;
            if (max_args) *max_args = k_words[i].max_args;
            return 0;
        }
    }
    return -1;
}

/* Validate UTF-8 (RFC 3629: no overlongs, no surrogates, <= U+10FFFF) and
 * reject control bytes other than tab. Returns 0 ok, -1 bad (err set). */
static int check_bytes(const unsigned char *p, size_t n, char *err, size_t errn) {
    size_t i = 0;
    while (i < n) {
        unsigned char b = p[i];
        if (b < 0x80) {
            if ((b < 0x20 && b != '\t') || b == 0x7F) {
                if (err && errn) snprintf(err, errn, "control character 0x%02X at byte %zu is not allowed", b, i);
                return -1;
            }
            i++;
            continue;
        }
        size_t len; uint32_t cp, min;
        if ((b & 0xE0) == 0xC0) { len = 2; cp = b & 0x1F; min = 0x80; }
        else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; min = 0x800; }
        else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; min = 0x10000; }
        else goto bad;
        if (i + len > n) goto bad;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) goto bad;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) goto bad;
        i += len;
        continue;
    bad:
        if (err && errn) snprintf(err, errn, "invalid UTF-8 at byte %zu", i);
        return -1;
    }
    return 0;
}

static bool is_blank(char ch) { return ch == ' ' || ch == '\t'; }

int visor_parse_command(const char *line, VisorCommand *out, char *err, size_t errn) {
    if (err && errn) err[0] = '\0';
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->cmd = VISOR_CMD_UNKNOWN;
    if (!line) {
        if (err && errn) snprintf(err, errn, "no input");
        return -1;
    }

    size_t n = strlen(line);
    if (n > 0 && line[n - 1] == '\n') n--;
    if (n > 0 && line[n - 1] == '\r') n--;
    if (n >= VISOR_LINE_MAX) {
        if (err && errn) snprintf(err, errn, "line too long (%zu bytes, max %d)", n, VISOR_LINE_MAX - 1);
        return -1;
    }
    if (check_bytes((const unsigned char *)line, n, err, errn) != 0) return -1;

    size_t a = 0, b = n;
    while (a < b && is_blank(line[a])) a++;
    while (b > a && is_blank(line[b - 1])) b--;
    memcpy(out->line, line + a, b - a);
    out->line[b - a] = '\0';

    if (out->line[0] == '\0' || out->line[0] == '#') { out->cmd = VISOR_CMD_EMPTY; return 0; }

    /* First word. */
    const char *s = out->line;
    size_t wl = 0;
    while (s[wl] && !is_blank(s[wl])) wl++;
    const ParseEntry *e = NULL;
    for (size_t i = 0; i < K_WORDS_N; i++) {
        if (strlen(k_words[i].word) == wl && memcmp(k_words[i].word, s, wl) == 0) { e = &k_words[i]; break; }
    }
    if (!e) { out->cmd = VISOR_CMD_SOURCE; return 0; }
    out->cmd = e->cmd;

    /* Tokenize the arguments. Room for MAX_ARGS+2 so `for current.machine` fits. */
    char toks[VISOR_MAX_ARGS + 2][VISOR_TOKEN_MAX];
    size_t nt = 0;
    const char *p = s + wl;
    for (;;) {
        while (*p && is_blank(*p)) p++;
        if (!*p) break;
        size_t tl = 0;
        while (p[tl] && !is_blank(p[tl])) tl++;
        if (tl >= VISOR_TOKEN_MAX) {
            if (err && errn) snprintf(err, errn, "argument %zu too long (%zu bytes, max %d)",
                                      nt + 1, tl, VISOR_TOKEN_MAX - 1);
            return -1;
        }
        if (nt >= VISOR_MAX_ARGS + 2) {
            if (err && errn) snprintf(err, errn, "too many arguments (max %zu)", e->max_args);
            return -1;
        }
        memcpy(toks[nt], p, tl);
        toks[nt][tl] = '\0';
        nt++;
        p += tl;
    }

    if (e->cmd == VISOR_CMD_REALIZE && nt >= 2 && strcmp(toks[nt - 2], "for") == 0) {
        if (strcmp(toks[nt - 1], "current.machine") != 0) {
            if (err && errn) snprintf(err, errn, "only `for current.machine` is supported (got `for %s`)",
                                      toks[nt - 1]);
            return -1;
        }
        out->for_current_machine = true;
        nt -= 2;
    }

    if (nt < e->min_args || nt > e->max_args) {
        if (err && errn) {
            if (e->min_args == e->max_args)
                snprintf(err, errn, "expected %zu argument%s, got %zu", e->min_args,
                         e->min_args == 1 ? "" : "s", nt);
            else
                snprintf(err, errn, "expected %zu to %zu arguments, got %zu",
                         e->min_args, e->max_args, nt);
        }
        return -1;
    }
    for (size_t i = 0; i < nt; i++) memcpy(out->args[i], toks[i], VISOR_TOKEN_MAX);
    out->argc = nt;
    return 0;
}
