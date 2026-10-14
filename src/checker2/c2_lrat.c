/* c2_lrat.c: DIMACS reader and RUP-only LRAT checker (see c2_lrat.h for the format and rules). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_lrat.h"

#define C2_LRAT_MAX_VARS 4000000
#define C2_LRAT_MAX_ID 100000000L

/* ------------------------------------------------------------------ tokens */

typedef struct {
    const char *p;
    size_t n, i;
    long line;
} Lx;

static void skip_ws(Lx *x)
{
    for (;;) {
        while (x->i < x->n && (x->p[x->i] == ' ' || x->p[x->i] == '\t' || x->p[x->i] == '\r' || x->p[x->i] == '\n')) {
            if (x->p[x->i] == '\n') x->line++;
            x->i++;
        }
        if (x->i < x->n && x->p[x->i] == 'c') { /* comment to end of line */
            while (x->i < x->n && x->p[x->i] != '\n') x->i++;
            continue;
        }
        return;
    }
}

/* signed decimal integer; -1 on malformed/overflow, 1 at end of input, 0 ok */
static int rd_int(Lx *x, long *out)
{
    skip_ws(x);
    if (x->i >= x->n) return 1;
    int neg = 0;
    if (x->p[x->i] == '-') { neg = 1; x->i++; }
    if (x->i >= x->n || x->p[x->i] < '0' || x->p[x->i] > '9') return -1;
    long v = 0;
    while (x->i < x->n && x->p[x->i] >= '0' && x->p[x->i] <= '9') {
        v = v * 10 + (x->p[x->i] - '0');
        if (v > C2_LRAT_MAX_ID) return -1;
        x->i++;
    }
    if (x->i < x->n && !(x->p[x->i] == ' ' || x->p[x->i] == '\t' || x->p[x->i] == '\n' || x->p[x->i] == '\r')) return -1;
    *out = neg ? -v : v;
    return 0;
}

/* ------------------------------------------------------------------ CNF */

void c2_cnf_free(C2Cnf *c)
{
    if (!c) return;
    free(c->lits);
    free(c->start);
    memset(c, 0, sizeof *c);
}

int c2_cnf_add(C2Cnf *c, const int32_t *lits, int n)
{
    size_t need = c->nlits + (size_t)n + 1;
    if (need > c->lcap) {
        size_t nc = c->lcap ? c->lcap : 1024;
        while (nc < need) nc *= 2;
        int32_t *nl = realloc(c->lits, nc * sizeof *nl);
        if (!nl) return -1;
        c->lits = nl;
        c->lcap = nc;
    }
    if ((size_t)c->nclauses + 1 > c->scap) {
        size_t nc = c->scap ? c->scap * 2 : 1024;
        size_t *ns = realloc(c->start, nc * sizeof *ns);
        if (!ns) return -1;
        c->start = ns;
        c->scap = nc;
    }
    c->start[c->nclauses++] = c->nlits;
    for (int k = 0; k < n; k++) {
        int32_t l = lits[k];
        int v = l < 0 ? -l : l;
        if (l == 0 || v > C2_LRAT_MAX_VARS) return -1;
        if (v > c->nvars) c->nvars = v;
        c->lits[c->nlits++] = l;
    }
    c->lits[c->nlits++] = 0;
    return 0;
}

int c2_cnf_parse(const char *text, size_t len, C2Cnf *c, const char **why)
{
    memset(c, 0, sizeof *c);
    Lx x = {text, len, 0, 1};
    skip_ws(&x);
    if (len - x.i < 6 || memcmp(text + x.i, "p cnf ", 6) != 0) { *why = "CNF-HEADER"; return -1; }
    x.i += 6;
    long nv, nc;
    if (rd_int(&x, &nv) || rd_int(&x, &nc) || nv < 0 || nc < 0 || nv > C2_LRAT_MAX_VARS) { *why = "CNF-HEADER"; return -1; }
    int32_t buf[4096];
    int n = 0;
    for (;;) {
        long l;
        int r = rd_int(&x, &l);
        if (r == 1) break;
        if (r < 0 || l > nv || -l > nv) { *why = "CNF-LITERAL"; c2_cnf_free(c); return -1; }
        if (l == 0) {
            if (c2_cnf_add(c, buf, n)) { *why = "CNF-RESOURCE"; c2_cnf_free(c); return -1; }
            n = 0;
            continue;
        }
        if (n == 4096) { *why = "CNF-CLAUSE-TOO-LONG"; c2_cnf_free(c); return -1; }
        buf[n++] = (int32_t)l;
    }
    if (n != 0 || c->nclauses != nc) { *why = "CNF-COUNT"; c2_cnf_free(c); return -1; } /* GUARD:cnf-count */
    c->nvars = (int)nv;
    return 0;
}

char *c2_cnf_print(const C2Cnf *c, size_t *len)
{
    size_t cap = 64 + c->nlits * 12, n = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    n += (size_t)snprintf(s + n, cap - n, "p cnf %d %d\n", c->nvars, c->nclauses);
    for (size_t i = 0; i < c->nlits; i++)
        n += (size_t)snprintf(s + n, cap - n, c->lits[i] ? "%d " : "%d\n", c->lits[i]);
    *len = n;
    return s;
}

/* ------------------------------------------------------------------ LRAT */

typedef struct {
    int32_t **cl;   /* by id; NULL = absent or deleted; each ends with 0 */
    long cap;
} Db;

static int db_put(Db *d, long id, int32_t *lits)
{
    if (id >= d->cap) {
        long nc = d->cap ? d->cap : 1024;
        while (nc <= id) nc *= 2;
        int32_t **n = realloc(d->cl, (size_t)nc * sizeof *n);
        if (!n) return -1;
        memset(n + d->cap, 0, (size_t)(nc - d->cap) * sizeof *n);
        d->cl = n;
        d->cap = nc;
    }
    d->cl[id] = lits;
    return 0;
}

static int32_t *dup_clause(const int32_t *l, int n)
{
    int32_t *c = malloc(((size_t)n + 1) * sizeof *c);
    if (!c) return NULL;
    memcpy(c, l, (size_t)n * sizeof *c);
    c[n] = 0;
    return c;
}

static int fail(C2LratResult *r, const char *rule, long line)
{
    r->ok = 0;
    r->rule = rule;
    r->line = line;
    return -1;
}

int c2_lrat_check(const C2Cnf *cnf, const char *proof, size_t len, C2LratResult *r)
{
    memset(r, 0, sizeof *r);
    r->rule = "LRAT-OK";
    Db d = {NULL, 0};
    int nv = cnf->nvars;
    int8_t *val = calloc((size_t)nv + 1, 1); /* +1 true, -1 false, 0 unassigned (by variable) */
    int32_t *assigned = malloc(((size_t)nv + 1) * sizeof *assigned);
    int32_t *lits = malloc(((size_t)nv + 2) * sizeof *lits);
    int rc = -1;
    if (!val || !assigned || !lits) { fail(r, "LRAT-RESOURCE", 0); goto out; }
    for (int i = 0; i < cnf->nclauses; i++) {
        const int32_t *c = cnf->lits + cnf->start[i];
        int n = 0;
        while (c[n]) n++;
        int32_t *k = dup_clause(c, n);
        if (!k || db_put(&d, i + 1, k)) { free(k); fail(r, "LRAT-RESOURCE", 0); goto out; }
    }
    Lx x = {proof, len, 0, 1};
    long last_id = cnf->nclauses;
    for (;;) {
        long id;
        int t = rd_int(&x, &id);
        if (t == 1) break;
        long line = x.line;
        if (t < 0 || id < 1) { fail(r, "LRAT-SYNTAX", line); goto out; }
        skip_ws(&x);
        if (x.i < x.n && x.p[x.i] == 'd') { /* deletion */
            x.i++;
            for (;;) {
                long del;
                if (rd_int(&x, &del)) { fail(r, "LRAT-SYNTAX", line); goto out; }
                if (del == 0) break;
                if (del < 0 || del >= d.cap || !d.cl[del]) { fail(r, "LRAT-DELETE-UNKNOWN", line); goto out; } /* GUARD:delete-unknown */
                free(d.cl[del]);
                d.cl[del] = NULL;
                r->deletions++;
            }
            continue;
        }
        if (id <= last_id) { fail(r, "LRAT-ID-ORDER", line); goto out; } /* GUARD:id-order */
        last_id = id;
        /* clause literals */
        int n = 0, na = 0, ok = 0;
        for (;;) {
            long l;
            if (rd_int(&x, &l)) { fail(r, "LRAT-SYNTAX", line); goto out; }
            if (l == 0) break;
            if (l > nv || -l > nv) { fail(r, "LRAT-LITERAL", line); goto out; } /* GUARD:lrat-literal */
            if (n > nv) { fail(r, "LRAT-SYNTAX", line); goto out; }
            lits[n++] = (int32_t)l;
        }
        /* assign every literal of the new clause false */
        int taut = 0;
        for (int k = 0; k < n; k++) {
            int v = lits[k] < 0 ? -lits[k] : lits[k];
            int8_t want = lits[k] < 0 ? 1 : -1;
            if (val[v] == -want) taut = 1;
            if (!val[v]) { val[v] = want; assigned[na++] = v; }
        }
        ok = taut; /* a tautology is implied by anything */
        /* hints */
        for (;;) {
            long h;
            if (rd_int(&x, &h)) { fail(r, "LRAT-SYNTAX", line); goto undo_fail; }
            if (h == 0) break;
            if (ok) continue; /* trailing hints after the conflict are allowed and ignored */
            if (h < 0) { fail(r, "LRAT-RAT-UNSUPPORTED", line); goto undo_fail; } /* GUARD:rat-refused */
            if (h >= d.cap || !d.cl[h]) { fail(r, "LRAT-HINT-UNKNOWN", line); goto undo_fail; } /* GUARD:hint-unknown */
            const int32_t *c = d.cl[h];
            int32_t unit = 0;
            int nfree = 0, sat = 0;
            for (int k = 0; c[k]; k++) {
                int v = c[k] < 0 ? -c[k] : c[k];
                int8_t s = val[v] == 0 ? 0 : ((c[k] > 0) == (val[v] > 0) ? 1 : -1);
                if (s == 1) { sat = 1; break; }
                if (s == 0) { nfree++; unit = c[k]; }
            }
            if (sat || nfree > 1) { fail(r, "LRAT-HINT-NOT-UNIT", line); goto undo_fail; } /* GUARD:hint-not-unit */
            if (nfree == 0) { ok = 1; continue; } /* falsified: the step is proved */
            int v = unit < 0 ? -unit : unit;
            val[v] = unit > 0 ? 1 : -1;
            assigned[na++] = v;
        }
        for (int k = 0; k < na; k++) val[assigned[k]] = 0;
        if (!ok) { fail(r, "LRAT-NO-CONFLICT", line); goto out; } /* GUARD:no-conflict */
        int32_t *k = dup_clause(lits, n);
        if (!k || db_put(&d, id, k)) { free(k); fail(r, "LRAT-RESOURCE", line); goto out; }
        r->steps++;
        if (n == 0) { r->ok = 1; rc = 0; goto out; } /* empty clause: unsatisfiable */
        continue;
    undo_fail:
        for (int k = 0; k < na; k++) val[assigned[k]] = 0;
        goto out;
    }
    fail(r, "LRAT-NO-EMPTY-CLAUSE", x.line); /* GUARD:no-empty-clause */
out:
    for (long i = 0; i < d.cap; i++) free(d.cl[i]);
    free(d.cl);
    free(val);
    free(assigned);
    free(lits);
    return rc;
}
