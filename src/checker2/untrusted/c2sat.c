/* c2sat: small DPLL SAT solver that writes LRAT proofs (omega#446). UNTRUSTED.
 *
 * Not part of checker2's trusted base and not in its identity hash: every UNSAT answer is only
 * believed after checker2's own LRAT checker (src/checker2/c2_lrat.c) accepts the proof. Shares no
 * code with that checker (own parser, own data structures).
 *
 * Method: DPLL with unit propagation over occurrence lists, decisions on the lowest unassigned
 * variable, conflict-directed backjumping. Every refuted subtree yields a clause that is the
 * negation of the decisions that mattered:
 *   leaf    : RUP with hints = the propagation reasons the conflict depends on, in trail order,
 *             then the conflicting clause;
 *   inner   : if the first branch's clause does not mention the decision it is returned as is,
 *             likewise the second; otherwise their resolvent on the decision, hints [first, second].
 * At the root the clause is empty. No learned clause is reused for propagation (simple, slow).
 *
 * Usage: c2sat <in.cnf> <out.lrat> [max-decisions]
 * Exit: 20 UNSAT (proof written), 10 SAT (model on stdout as "v ..."), 1 limit hit, 2 usage/I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int *l; int n; } Cl;

static int nv, ncl;
static Cl *cls;              /* original clauses, then learned ones (index = id - 1) */
static int ncls, capcls;
static int **occ, *nocc, *capocc; /* occ[lit + nv] */
static signed char *val;
static int *reason, *trail, ntrail, *seen;
static long decisions, max_decisions;
static FILE *out;

static int lit_true(int l) { return val[abs(l)] == (l > 0 ? 1 : -1); }
static int lit_false(int l) { return val[abs(l)] == (l > 0 ? -1 : 1); }

static int add_clause(const int *l, int n)
{
    if (ncls == capcls) {
        capcls = capcls ? capcls * 2 : 1024;
        cls = realloc(cls, (size_t)capcls * sizeof *cls);
        if (!cls) exit(2);
    }
    cls[ncls].l = malloc(((size_t)n + 1) * sizeof(int));
    if (!cls[ncls].l) exit(2);
    memcpy(cls[ncls].l, l, (size_t)n * sizeof(int));
    cls[ncls].n = n;
    return ++ncls; /* id */
}

static void assign(int l, int why)
{
    val[abs(l)] = l > 0 ? 1 : -1;
    reason[abs(l)] = why;
    trail[ntrail++] = l;
}

/* propagate from trail position q; returns the id of a falsified clause or 0 */
static int propagate(int q)
{
    while (q < ntrail) {
        int t = trail[q++];
        int f = -t; /* clauses containing f may now be unit or false */
        int *o = occ[f + nv];
        for (int k = 0; k < nocc[f + nv]; k++) {
            Cl *c = &cls[o[k] - 1];
            int nfree = 0, last = 0, sat = 0;
            for (int j = 0; j < c->n; j++) {
                if (lit_true(c->l[j])) { sat = 1; break; }
                if (!lit_false(c->l[j])) { nfree++; last = c->l[j]; }
            }
            if (sat) continue;
            if (nfree == 0) return o[k];
            if (nfree == 1) assign(last, o[k]);
        }
    }
    return 0;
}

static void undo_to(int n)
{
    while (ntrail > n) {
        int l = trail[--ntrail];
        val[abs(l)] = 0;
        reason[abs(l)] = 0;
    }
}

static int emit(const int *l, int n, const int *hints, int nh)
{
    int id = add_clause(l, n);
    fprintf(out, "%d ", id);
    for (int k = 0; k < n; k++) fprintf(out, "%d ", l[k]);
    fprintf(out, "0 ");
    for (int k = 0; k < nh; k++) fprintf(out, "%d ", hints[k]);
    fprintf(out, "0\n");
    return id;
}

/* clause from a conflict: negated relevant decisions, hints = relevant reasons in trail order */
static int analyze(int conflict)
{
    int *lits = malloc(((size_t)ntrail + 1) * sizeof(int));
    int *hints = malloc(((size_t)ntrail + 2) * sizeof(int));
    int nl = 0, nh = 0;
    Cl *c = &cls[conflict - 1];
    for (int j = 0; j < c->n; j++) seen[abs(c->l[j])] = 1;
    for (int i = ntrail - 1; i >= 0; i--) {
        int v = abs(trail[i]);
        if (!seen[v]) continue;
        seen[v] = 0;
        if (reason[v] == 0) { lits[nl++] = -trail[i]; continue; }
        hints[nh++] = reason[v];
        Cl *r = &cls[reason[v] - 1];
        for (int j = 0; j < r->n; j++)
            if (abs(r->l[j]) != v) seen[abs(r->l[j])] = 1;
    }
    for (int a = 0, b = nh - 1; a < b; a++, b--) { int t = hints[a]; hints[a] = hints[b]; hints[b] = t; }
    hints[nh++] = conflict;
    int id = emit(lits, nl, hints, nh);
    free(lits);
    free(hints);
    return id;
}

static int mentions(int id, int l)
{
    Cl *c = &cls[id - 1];
    for (int j = 0; j < c->n; j++) if (c->l[j] == l) return 1;
    return 0;
}

/* returns the id of a clause refuting the current trail, 0 if SAT, -1 if the limit was hit */
static int solve(int q)
{
    int conflict = propagate(q);
    if (conflict) return analyze(conflict);
    int v = 0;
    for (int i = 1; i <= nv; i++) if (!val[i]) { v = i; break; }
    if (!v) return 0;
    if (++decisions > max_decisions) return -1;
    int mark = ntrail, id[2];
    for (int side = 0; side < 2; side++) {
        int d = side ? -v : v;
        assign(d, 0);
        id[side] = solve(ntrail - 1);
        if (id[side] <= 0) return id[side]; /* SAT keeps its model on the trail */
        undo_to(mark);
        if (!mentions(id[side], -d)) return id[side]; /* backjump: the decision did not matter */
    }
    /* resolve on v */
    Cl *a = &cls[id[0] - 1], *b = &cls[id[1] - 1];
    int *r = malloc(((size_t)a->n + (size_t)b->n) * sizeof(int)), n = 0;
    for (int j = 0; j < a->n; j++) if (a->l[j] != -v) r[n++] = a->l[j];
    for (int j = 0; j < b->n; j++) {
        if (b->l[j] == v) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= r[k] == b->l[j];
        if (!dup) r[n++] = b->l[j];
    }
    int h[2] = {id[0], id[1]};
    int rid = emit(r, n, h, 2);
    free(r);
    return rid;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: c2sat <in.cnf> <out.lrat> [max-decisions]\n"); return 2; }
    max_decisions = argc > 3 ? atol(argv[3]) : 2000000;
    FILE *in = fopen(argv[1], "r");
    if (!in) return 2;
    int ch;
    while ((ch = fgetc(in)) == 'c') while ((ch = fgetc(in)) != '\n' && ch != EOF) {}
    ungetc(ch, in);
    if (fscanf(in, " p cnf %d %d", &nv, &ncl) != 2 || nv < 0 || ncl < 0) return 2;
    int *buf = malloc(((size_t)nv * 2 + 2) * sizeof(int)), n = 0, x;
    while (fscanf(in, "%d", &x) == 1) {
        if (x == 0) { add_clause(buf, n); n = 0; }
        else if (n < 2 * nv + 1) buf[n++] = x;
    }
    fclose(in);
    if (ncls != ncl) return 2;
    occ = calloc((size_t)nv * 2 + 1, sizeof *occ);
    nocc = calloc((size_t)nv * 2 + 1, sizeof *nocc);
    capocc = calloc((size_t)nv * 2 + 1, sizeof *capocc);
    val = calloc((size_t)nv + 1, 1);
    reason = calloc((size_t)nv + 1, sizeof *reason);
    seen = calloc((size_t)nv + 1, sizeof *seen);
    trail = malloc(((size_t)nv + 1) * sizeof *trail);
    for (int i = 0; i < ncls; i++)
        for (int j = 0; j < cls[i].n; j++) {
            int s = cls[i].l[j] + nv;
            if (nocc[s] == capocc[s]) { capocc[s] = capocc[s] ? capocc[s] * 2 : 4; occ[s] = realloc(occ[s], (size_t)capocc[s] * sizeof(int)); }
            occ[s][nocc[s]++] = i + 1;
        }
    out = fopen(argv[2], "w");
    if (!out) return 2;
    /* level 0: empty and unit clauses */
    int conflict = 0;
    for (int i = 0; i < ncl && !conflict; i++) {
        if (cls[i].n == 0) conflict = i + 1;
        else if (cls[i].n == 1) {
            if (lit_false(cls[i].l[0])) conflict = i + 1;
            else if (!lit_true(cls[i].l[0])) assign(cls[i].l[0], i + 1);
        }
    }
    int r = conflict ? analyze(conflict) : solve(0);
    fclose(out);
    if (r > 0) { printf("s UNSATISFIABLE\n"); return 20; }
    if (r < 0) { printf("s UNKNOWN (decision limit)\n"); return 1; }
    printf("s SATISFIABLE\nv");
    for (int i = 1; i <= nv; i++) printf(" %d", val[i] >= 0 ? i : -i);
    printf(" 0\n");
    return 10;
}
