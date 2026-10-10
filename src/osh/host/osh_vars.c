/*
 * osh_vars.c -- the shell's variable table and the fresh-per-exec environment (ABI section 4.5, 7.2, 9.1).
 * The environment of a child is built from this table only; the process-global environ is read once, at
 * osh_session_init, and never again. Names and values are raw bytes without NUL.
 * Bookkeeping as bash 5.2 does it: PWD per POSIX; SHLVL = inherited level + 1 (non-numeric counts as 0, never below 0,
 * above 1000 resets to 1 with a warning); OLDPWD exported, kept only when the inherited value names a directory and
 * set by cd. `_` (bash's last argument) is not maintained.
 */
#include "osh_host.h"
#include "osh_priv.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static OshVar *find(const OshSession *s, const char *name)
{
    for (size_t i = 0; i < s->nvars; i++)
        if (strcmp(s->vars[i].name, name) == 0) return &s->vars[i];
    return NULL;
}

static OshVar *add(OshSession *s, const char *name)
{
    if (s->nvars == s->capvars) {
        size_t nc = s->capvars ? s->capvars * 2 : 32;
        OshVar *nv = realloc(s->vars, nc * sizeof *nv);
        if (!nv) return NULL;
        s->vars = nv;
        s->capvars = nc;
    }
    char *n = strdup(name);
    if (!n) return NULL;
    OshVar *v = &s->vars[s->nvars++];
    v->name = n;
    v->value = NULL;
    v->exported = 0;
    return v;
}

const char *osh_var_get(const OshSession *s, const char *name)
{
    OshVar *v = find(s, name);
    return v ? v->value : NULL;
}

static int setval(OshVar *v, const char *value)
{
    char *d = strdup(value);
    if (!d) return -1;
    free(v->value);
    v->value = d;
    return 0;
}

int osh_var_set(OshSession *s, const char *name, const char *value)
{
    if (!osh_name_valid(name)) return -1;
    OshVar *v = find(s, name);
    if (!v && !(v = add(s, name))) return -1;
    return setval(v, value);
}

int osh_var_export(OshSession *s, const char *name, const char *value)
{
    if (!osh_name_valid(name)) return -1;
    OshVar *v = find(s, name);
    if (!v && !(v = add(s, name))) return -1;
    if (value && setval(v, value)) return -1;
    v->exported = 1;
    return 0;
}

int osh_var_unexport(OshSession *s, const char *name)
{
    OshVar *v = find(s, name);
    if (v) v->exported = 0;
    return 0; /* bash: `export -n` of an unset name is a silent success */
}

int osh_var_unset(OshSession *s, const char *name)
{
    for (size_t i = 0; i < s->nvars; i++)
        if (strcmp(s->vars[i].name, name) == 0) {
            free(s->vars[i].name);
            free(s->vars[i].value);
            s->vars[i] = s->vars[--s->nvars];
            return 0;
        }
    return 0;
}

static int pwd_names_cwd(const char *p)
{
    struct stat a, b;
    return p && p[0] == '/' && stat(p, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* Keep an imported entry whose name is not an identifier. bash never makes it a variable and never shows it in
 * `export -p`, yet every child still receives it; dropping it would starve tools that read their own odd entries. */
static int raw_keep(OshSession *s, const char *entry)
{
    char **nr = realloc(s->raw_env, (s->nraw + 1) * sizeof *nr);
    if (!nr) return -1;
    s->raw_env = nr;
    if (!(nr[s->nraw] = strdup(entry))) return -1;
    s->nraw++;
    return 0;
}

int osh_session_init(OshSession *s, char *const *envp)
{
    memset(s, 0, sizeof *s);
    s->fd[0] = 0; s->fd[1] = 1; s->fd[2] = 2;
    s->tty_fd = -1;
    s->session_epoch = 1;
    for (; envp && *envp; envp++) {
        const char *eq = strchr(*envp, '=');
        if (!eq || eq == *envp) continue;
        size_t nl = (size_t)(eq - *envp);
        char name[256];
        int ident = nl < sizeof name;
        if (ident) { memcpy(name, *envp, nl); name[nl] = 0; ident = osh_name_valid(name); }
        if (!ident) {
            if (raw_keep(s, *envp)) { osh_session_free(s); return -1; }
            continue;
        }
        if (osh_var_export(s, name, eq + 1)) { osh_session_free(s); return -1; }
    }
    if (!pwd_names_cwd(osh_var_get(s, "PWD"))) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd) || osh_var_export(s, "PWD", cwd)) { osh_session_free(s); return -1; }
    }
    const char *lv = osh_var_get(s, "SHLVL");
    long level = (lv ? strtol(lv, NULL, 10) : 0) + 1; /* atoi semantics: junk counts as 0 */
    if (level < 0) level = 0;
    if (level >= 1000) {
        osh_diag(s->fd[2], "osh: warning: shell level (%ld) too high, resetting to 1", level);
        level = 1;
    }
    char lvbuf[24];
    snprintf(lvbuf, sizeof lvbuf, "%ld", level);
    if (osh_var_export(s, "SHLVL", lvbuf)) { osh_session_free(s); return -1; }
    const char *op = osh_var_get(s, "OLDPWD");
    struct stat st;
    if (!op || stat(op, &st) != 0 || !S_ISDIR(st.st_mode)) {
        osh_var_unset(s, "OLDPWD");
        if (osh_var_export(s, "OLDPWD", NULL)) { osh_session_free(s); return -1; }
    }
    return 0;
}

void osh_session_free(OshSession *s)
{
    for (size_t i = 0; i < s->nvars; i++) { free(s->vars[i].name); free(s->vars[i].value); }
    free(s->vars);
    s->vars = NULL;
    s->nvars = s->capvars = 0;
    for (size_t i = 0; i < s->nraw; i++) free(s->raw_env[i]);
    free(s->raw_env);
    s->raw_env = NULL;
    s->nraw = 0;
}

void osh_envp_free(char **envp)
{
    if (!envp) return;
    for (char **p = envp; *p; p++) free(*p);
    free(envp);
}

char **osh_build_envp(const OshSession *s, const OshAssign *ov, int nov)
{
    char **e = calloc(s->nvars + s->nraw + (size_t)nov + 1, sizeof *e);
    if (!e) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < s->nvars; i++) {
        const OshVar *v = &s->vars[i];
        if (!v->exported || !v->value) continue;
        size_t nl = strlen(v->name), vl = strlen(v->value);
        char *str = malloc(nl + vl + 2);
        if (!str) { osh_envp_free(e); return NULL; }
        memcpy(str, v->name, nl);
        str[nl] = '=';
        memcpy(str + nl + 1, v->value, vl + 1);
        e[n++] = str;
    }
    for (size_t i = 0; i < s->nraw; i++) { /* verbatim; their names are not identifiers so no override can match them */
        if (!(e[n] = strdup(s->raw_env[i]))) { osh_envp_free(e); return NULL; }
        n++;
    }
    for (int k = 0; k < nov; k++) {
        size_t nl = strlen(ov[k].name), vl = strlen(ov[k].value);
        char *str = malloc(nl + vl + 2);
        if (!str) { osh_envp_free(e); return NULL; }
        memcpy(str, ov[k].name, nl);
        str[nl] = '=';
        memcpy(str + nl + 1, ov[k].value, vl + 1);
        size_t j = 0;
        for (; j < n; j++)
            if (strncmp(e[j], ov[k].name, nl) == 0 && e[j][nl] == '=') break;
        if (j < n) { free(e[j]); e[j] = str; }
        else e[n++] = str;
    }
    e[n] = NULL;
    return e;
}
