/*
 * osh_builtin.c -- in-process builtins: cd pwd printf export unset exit (builtin_id 1..6, ABI section 7.1).
 *
 * printf subset, exactly (anything else is refused with a diagnostic and status 1, before any output):
 *   conversions   %s %d %i %u %x %o %c %%       no flags, width, precision, length modifiers; no %b %f %e %g %X
 *   escapes       in the FORMAT only: \\ \a \b \f \n \r \t \v \" and \NNN (1 to 3 octal digits); any other
 *                 backslash sequence is printed verbatim (backslash included). Arguments are never escape-processed.
 *   format reuse  while arguments remain, the whole format is applied again; a format with no conversion is
 *                 printed once. Missing arguments read as "" for %s %c and 0 for numbers.
 *   numbers       strtoimax/strtoumax base 0 (decimal, 0x hex, 0 octal); a leading ' or " gives the next byte's
 *                 value; a bad number prints its numeric prefix (or 0), emits a diagnostic, and the status is 1.
 *   %c            first byte of the argument, nothing for an empty argument.
 * cd: cd [-L|-P] [--] [DIR | -]; DIR defaults to $HOME; -L (default): PWD is the lexical result, "." and ".."
 *   resolved textually; -P: PWD from getcwd; cd "" is a no-op (bash). CDPATH is NOT searched: a relative DIR not
 *   anchored with ./ or ../ is refused while CDPATH is set (omega#344).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "osh_host.h"
#include "osh_priv.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int osh_write_all(int fd, const char *buf, size_t n)
{
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

void osh_diag(int fd, const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "osh: ");
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    n += m;
    if ((size_t)n > sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    int saved = errno;
    (void)osh_write_all(fd, buf, (size_t)n);
    errno = saved;
}

/* ---------------- cd / pwd ---------------- */

/* Lexical canonical form of an absolute path ("." and ".." removed, no trailing slash). */
static int canon(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '/';
    n = 1;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        if (l == 0) break;
        if (l == 1 && p[0] == '.') {
        } else if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 1 && out[n - 1] != '/') n--;
            if (n > 1) n--;           /* drop the slash before the removed component */
        } else {
            if (n + l + 2 > cap) return -1;
            if (n > 1) out[n++] = '/';
            memcpy(out + n, p, l);
            n += l;
        }
        p = e;
    }
    out[n] = 0;
    return 0;
}

static int logical_pwd(const OshSession *s, char *out, size_t cap)
{
    const char *p = osh_var_get(s, "PWD");
    struct stat a, b;
    if (p && p[0] == '/' && strlen(p) < cap && stat(p, &a) == 0 && stat(".", &b) == 0 && a.st_dev == b.st_dev &&
        a.st_ino == b.st_ino) {
        strcpy(out, p);
        return 0;
    }
    return getcwd(out, cap) ? 0 : -1;
}

static int bi_cd(OshSession *s, const OshCmd *c, const int io[3], int in_parent)
{
    int show = 0, physical = 0, i = 1;
    const char *dir;
    /* options as bash: -L (the default) and -P; "--" ends them; anything else is "invalid option", status 2 */
    for (; i < c->nargv && c->argv[i][0] == '-' && c->argv[i][1]; i++) {
        if (strcmp(c->argv[i], "--") == 0) { i++; break; }
        for (const char *o = c->argv[i] + 1; *o; o++) {
            if (*o == 'L') physical = 0;
            else if (*o == 'P') physical = 1;
            else { osh_diag(io[2], "cd: -%c: invalid option\ncd: usage: cd [-L|-P] [dir]", *o); return 2; }
        }
    }
    if (c->nargv - i > 1) { osh_diag(io[2], "cd: too many arguments"); return 1; }
    if (c->nargv == i) {
        dir = osh_var_get(s, "HOME");
        if (!dir || !*dir) { osh_diag(io[2], "cd: HOME not set"); return 1; }
    } else if (strcmp(c->argv[i], "-") == 0) {
        dir = osh_var_get(s, "OLDPWD");
        if (!dir || !*dir) { osh_diag(io[2], "cd: OLDPWD not set"); return 1; }
        show = 1;
    } else {
        dir = c->argv[i];
        if (!*dir) return 0; /* bash 5.2: cd "" changes nothing and succeeds */
        /* CDPATH is not searched. bash would go to $CDPATH/DIR when that exists, so a relative name that is not
         * anchored with ./ or ../ could silently mean a different directory: refuse it while CDPATH is set. */
        const char *cdpath = osh_var_get(s, "CDPATH");
        int anchored = dir[0] == '/' || (dir[0] == '.' && (dir[1] == 0 || dir[1] == '/' || (dir[1] == '.' && (dir[2] == 0 || dir[2] == '/'))));
        if (cdpath && *cdpath && !anchored) {
            osh_diag(io[2], "cd: %s: CDPATH is set and osh does not search it; use ./%s or an absolute path", dir, dir);
            return 1;
        }
    }
    char old[PATH_MAX], joined[2 * PATH_MAX + 2], cur[PATH_MAX];
    if (logical_pwd(s, old, sizeof old)) { osh_diag(io[2], "cd: cannot determine current directory"); return 1; }
    if (dir[0] == '/') {
        if (strlen(dir) >= sizeof joined) { osh_diag(io[2], "cd: %s: File name too long", dir); return 1; }
        strcpy(joined, dir);
    } else {
        if (snprintf(joined, sizeof joined, "%s/%s", old, dir) >= (int)sizeof joined) {
            osh_diag(io[2], "cd: %s: File name too long", dir);
            return 1;
        }
    }
    if (canon(joined, cur, sizeof cur)) { osh_diag(io[2], "cd: %s: File name too long", dir); return 1; }
    if (osh_effect_check(s, OSH_OP_CHDIR, cur, io[2]) != OSH_E_OK) return 1;
    if (chdir(cur) != 0) { osh_diag(io[2], "cd: %s: %s", dir, strerror(errno)); return 1; }
    if (physical) { /* -P: PWD is the physical directory with symbolic links resolved; the lexical one if the kernel cannot say */
        char phys[PATH_MAX];
        if (getcwd(phys, sizeof phys)) strcpy(cur, phys);
    }
    /* A child context keeps its own copy of the table, so setting here is safe in both cases. */
    (void)in_parent;
    osh_var_set(s, "OLDPWD", old);
    osh_var_set(s, "PWD", cur);
    if (show) {
        char line[PATH_MAX + 1];
        int n = snprintf(line, sizeof line, "%s\n", cur);
        if (osh_write_all(io[1], line, (size_t)n)) { osh_diag(io[2], "cd: write error: %s", strerror(errno)); return 1; }
    }
    return 0;
}

static int bi_pwd(OshSession *s, const OshCmd *c, const int io[3])
{
    int physical = 0, i = 1;
    for (; i < c->nargv && c->argv[i][0] == '-' && c->argv[i][1]; i++) {
        if (strcmp(c->argv[i], "-P") == 0) physical = 1;
        else if (strcmp(c->argv[i], "-L") == 0) physical = 0;
        else if (strcmp(c->argv[i], "--") == 0) { i++; break; }
        else { osh_diag(io[2], "pwd: %s: invalid option", c->argv[i]); return 2; }
    }
    if (i < c->nargv) { osh_diag(io[2], "pwd: too many arguments"); return 1; }
    char p[PATH_MAX + 1];
    if (physical ? !getcwd(p, PATH_MAX) : logical_pwd(s, p, PATH_MAX)) {
        osh_diag(io[2], "pwd: %s", strerror(errno));
        return 1;
    }
    strcat(p, "\n");
    if (osh_write_all(io[1], p, strlen(p))) { osh_diag(io[2], "pwd: write error: %s", strerror(errno)); return 1; }
    return 0;
}

/* ---------------- printf ---------------- */

typedef struct {
    int fd, err;
    size_t n;
    char buf[4096];
} Out;

static void out_flush(Out *o)
{
    if (o->n && !o->err && osh_write_all(o->fd, o->buf, o->n)) o->err = errno;
    o->n = 0;
}

static void out_bytes(Out *o, const char *p, size_t n)
{
    while (n) {
        if (o->n == sizeof o->buf) out_flush(o);
        size_t k = sizeof o->buf - o->n;
        if (k > n) k = n;
        memcpy(o->buf + o->n, p, k);
        o->n += k;
        p += k;
        n -= k;
    }
}

static int is_conv(char ch) { return ch && strchr("sdiuxoc", ch) != NULL; }

static int bi_printf(const OshCmd *c, const int io[3])
{
    int ai = 1;
    if (ai < c->nargv && strcmp(c->argv[ai], "--") == 0) ai++;
    /* bash takes any other leading -x as an option (-v sets a variable); none is supported, so none is printed */
    else if (ai < c->nargv && c->argv[ai][0] == '-' && c->argv[ai][1]) {
        osh_diag(io[2], "printf: %s: option not supported", c->argv[ai]);
        return 2;
    }
    if (ai >= c->nargv) { osh_diag(io[2], "printf: usage: printf format [arguments]"); return 2; } /* bash: 2 */
    const char *fmt = c->argv[ai++];
    int first = ai, nspec = 0;
    /* Validate EVERY conversion before anything is printed. The scan and the print loop below agree on what a
     * backslash does: it always takes the next byte with it (a known escape, or an unknown one that is printed
     * verbatim, as coreutils does), so `\%` is two literal bytes and never starts a conversion. A `%` or `\` that is
     * the last byte never reads past the terminator. */
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            if (p[1]) p++;
            continue;
        }
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        if (!*p) {
            osh_diag(io[2], "printf: %%: invalid conversion specification (the format ends after %%)");
            return 1;
        }
        if (!is_conv(*p)) {
            osh_diag(io[2], "printf: %%%c: unsupported conversion (supported: %%s %%d %%i %%u %%x %%o %%c %%%%)", *p);
            return 1;
        }
        nspec++;
    }
    Out o = {.fd = io[1]};
    int status = 0, ci = first;
    do {
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                char e = p[1];
                static const char esc_from[] = "\\abfnrtv\"", esc_to[] = "\\\a\b\f\n\r\t\v\"";
                const char *m = e ? strchr(esc_from, e) : NULL;
                if (m) {
                    out_bytes(&o, &esc_to[m - esc_from], 1);
                    p++;
                } else if (e >= '0' && e <= '7') {
                    int v = 0, k = 0;
                    while (k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (p[1] - '0'); p++; k++; }
                    char b = (char)(v & 0xff);
                    out_bytes(&o, &b, 1);
                } else if (e) {
                    out_bytes(&o, p, 2); /* unknown escape: both bytes verbatim, as coreutils does */
                    p++;
                } else {
                    out_bytes(&o, p, 1); /* lone trailing backslash */
                }
            } else if (*p != '%') {
                out_bytes(&o, p, 1);
            } else {
                p++;
                if (*p == '%') { out_bytes(&o, "%", 1); continue; }
                if (!*p) break; /* unreachable: the scan refuses a final %; never step past the terminator */
                const char *arg = ci < c->nargv ? c->argv[ci++] : NULL;
                if (*p == 's') {
                    if (arg) out_bytes(&o, arg, strlen(arg));
                } else if (*p == 'c') {
                    out_bytes(&o, arg && *arg ? arg : "", 1); /* coreutils: an empty or missing argument prints a NUL byte */
                } else {
                    char num[64];
                    int bad = 0;
                    errno = 0;
                    uintmax_t u = 0;
                    intmax_t sv = 0;
                    if (arg && (arg[0] == '\'' || arg[0] == '"')) {
                        u = (unsigned char)arg[1];
                        sv = (intmax_t)u;
                    } else if (arg && *arg) {
                        char *end;
                        if (*p == 'd' || *p == 'i') sv = strtoimax(arg, &end, 0);
                        else u = strtoumax(arg, &end, 0);
                        if (end == arg || *end || errno == ERANGE) bad = 1;
                        if (bad && end == arg) { u = 0; sv = 0; }
                    }
                    if (bad) {
                        osh_diag(io[2], "printf: '%s': invalid number", arg);
                        status = 1;
                    }
                    int n;
                    switch (*p) {
                    case 'd': case 'i': n = snprintf(num, sizeof num, "%jd", sv); break;
                    case 'u': n = snprintf(num, sizeof num, "%ju", u); break;
                    case 'x': n = snprintf(num, sizeof num, "%jx", u); break;
                    case 'o': n = snprintf(num, sizeof num, "%jo", u); break;
                    default: n = 0; break; /* unreachable: the scan admitted only d i u x o here */
                    }
                    out_bytes(&o, num, (size_t)n);
                }
            }
        }
    } while (nspec > 0 && ci < c->nargv);
    out_flush(&o);
    if (o.err) { osh_diag(io[2], "printf: write error: %s", strerror(o.err)); return 1; }
    return status;
}

/* ---------------- export / unset / exit ---------------- */

static int cmp_var(const void *a, const void *b)
{
    return strcmp((*(const OshVar *const *)a)->name, (*(const OshVar *const *)b)->name);
}

/* `declare -x NAME="value"` quoting as bash 5.2 prints it: double quotes with \ before " \ $ `; when the value holds
 * a control or non-ASCII byte, the $'...' form with \a \b \t \n \v \f \r \E, \\ \' and three-digit octal otherwise. */
static void out_declare_value(Out *o, const char *v)
{
    int ansi = 0;
    for (const unsigned char *q = (const unsigned char *)v; *q; q++)
        if (*q < 0x20 || *q >= 0x7f) { ansi = 1; break; }
    if (!ansi) {
        out_bytes(o, "\"", 1);
        for (const char *q = v; *q; q++) {
            if (*q == '"' || *q == '\\' || *q == '$' || *q == '`') out_bytes(o, "\\", 1);
            out_bytes(o, q, 1);
        }
        out_bytes(o, "\"", 1);
        return;
    }
    out_bytes(o, "$'", 2);
    for (const unsigned char *q = (const unsigned char *)v; *q; q++) {
        const char *esc = NULL;
        switch (*q) {
        case '\a': esc = "\\a"; break; case '\b': esc = "\\b"; break; case '\t': esc = "\\t"; break;
        case '\n': esc = "\\n"; break; case '\v': esc = "\\v"; break; case '\f': esc = "\\f"; break;
        case '\r': esc = "\\r"; break; case 0x1b: esc = "\\E"; break;
        case '\\': esc = "\\\\"; break; case '\'': esc = "\\'"; break;
        }
        if (esc) { out_bytes(o, esc, strlen(esc)); continue; }
        if (*q < 0x20 || *q >= 0x7f) {
            char oct[5];
            snprintf(oct, sizeof oct, "\\%03o", *q);
            out_bytes(o, oct, 4);
            continue;
        }
        out_bytes(o, (const char *)q, 1);
    }
    out_bytes(o, "'", 1);
}

/* export [-fn] [name[=value] ...] | export -p   (bash 5.2 shape, #344 RISK-6)
 * -p with no names lists `declare -x` lines sorted by name; -p with names is a plain export and prints nothing.
 * -n assigns NAME=value if given, then drops the export mark but keeps the variable. -f names functions, which osh
 * does not have, so every -f name is "not a function" (status 1). An unknown option is status 2 with the usage line. */
static int bi_export(OshSession *s, const OshCmd *c, const int io[3])
{
    int i = 1, status = 0, unexport = 0, fn = 0;
    for (; i < c->nargv && c->argv[i][0] == '-' && c->argv[i][1]; i++) {
        if (strcmp(c->argv[i], "--") == 0) { i++; break; }
        for (const char *p = c->argv[i] + 1; *p; p++) {
            if (*p == 'n') unexport = 1;
            else if (*p == 'f') fn = 1;
            else if (*p != 'p') {
                osh_diag(io[2], "export: -%c: invalid option\nexport: usage: export [-fn] [name[=value] ...] or export -p", *p);
                return 2;
            }
        }
    }
    if (i >= c->nargv) {
        const OshVar **v = malloc((s->nvars + 1) * sizeof *v);
        if (!v) return 1;
        size_t n = 0;
        for (size_t k = 0; k < s->nvars; k++)
            if (s->vars[k].exported) v[n++] = &s->vars[k];
        qsort(v, n, sizeof *v, cmp_var);
        Out o = {.fd = io[1]};
        for (size_t k = 0; k < n; k++) {
            out_bytes(&o, "declare -x ", 11);
            out_bytes(&o, v[k]->name, strlen(v[k]->name));
            if (v[k]->value) { out_bytes(&o, "=", 1); out_declare_value(&o, v[k]->value); }
            out_bytes(&o, "\n", 1);
        }
        out_flush(&o);
        free(v);
        return o.err ? 1 : 0;
    }
    for (; i < c->nargv; i++) {
        const char *a = c->argv[i], *eq = strchr(a, '=');
        char name[256];
        size_t nl = eq ? (size_t)(eq - a) : strlen(a);
        if (fn) { osh_diag(io[2], "export: %s: not a function", a); status = 1; continue; }
        if (nl == 0 || nl >= sizeof name) { osh_diag(io[2], "export: '%s': not a valid identifier", a); status = 1; continue; }
        memcpy(name, a, nl);
        name[nl] = 0;
        if (!osh_name_valid(name)) { osh_diag(io[2], "export: '%s': not a valid identifier", a); status = 1; continue; }
        if (unexport) {
            if (eq && osh_var_set(s, name, eq + 1)) { osh_diag(io[2], "export: out of memory"); status = 1; continue; }
            osh_var_unexport(s, name);
        } else if (osh_var_export(s, name, eq ? eq + 1 : NULL)) { osh_diag(io[2], "export: out of memory"); status = 1; }
    }
    return status;
}

static int bi_unset(OshSession *s, const OshCmd *c, const int io[3])
{
    int i = 1, status = 0;
    for (; i < c->nargv && c->argv[i][0] == '-' && c->argv[i][1]; i++) {
        if (strcmp(c->argv[i], "-v") == 0) continue;
        if (strcmp(c->argv[i], "--") == 0) { i++; break; }
        osh_diag(io[2], "unset: %s: invalid option", c->argv[i]);
        return 2;
    }
    for (; i < c->nargv; i++) {
        if (!osh_name_valid(c->argv[i])) continue; /* bash 5.2: unset of a non-identifier is silent and succeeds (omega#344) */
        osh_var_unset(s, c->argv[i]);
    }
    return status;
}

static int bi_exit(OshSession *s, const OshCmd *c, const int io[3], int in_parent)
{
    int st = s->last_status & 0xff;
    if (c->nargv > 2) {
        osh_diag(io[2], "exit: too many arguments");
        if (in_parent) s->abort_list = 1; /* bash: no exit, status 1, the rest of this list is not run */
        return 1;
    }
    if (c->nargv == 2) {
        char *end;
        errno = 0;
        long v = strtol(c->argv[1], &end, 10);
        if (end == c->argv[1] || *end || errno == ERANGE) {
            osh_diag(io[2], "exit: %s: numeric argument required", c->argv[1]);
            st = 2;
        } else {
            st = (int)(v & 0xff);
        }
    }
    if (in_parent) { s->exit_requested = 1; s->exit_status = st; }
    return st;
}

int osh_builtin_run(OshSession *s, const OshCmd *c, const int io[3], int in_parent)
{
    switch (c->builtin_id) {
    case OSH_B_CD: return bi_cd(s, c, io, in_parent);
    case OSH_B_PWD: return bi_pwd(s, c, io);
    case OSH_B_PRINTF: return bi_printf(c, io);
    case OSH_B_EXPORT: return bi_export(s, c, io);
    case OSH_B_UNSET: return bi_unset(s, c, io);
    case OSH_B_EXIT: return bi_exit(s, c, io, in_parent);
    default: return 1;
    }
}
