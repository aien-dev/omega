/* test_c2_blast: checker2's bit-blaster without any SAT solver (omega#446).
 *   A. Every function of every corpus unit that has a non-scalar register (slice, handle, arena,
 *      struct) is refused as BLAST-UNSUPPORTED "not a scalar", never approximated.
 *   B. Every program of the LRAT gate (tests/checker2/lrat, IR written by lrat.sh): the CNF is
 *      evaluated input by input with unit propagation alone (the encoding is full Tseitin, so fixing
 *      the argument bits fixes every gate). p* programs ("holds") must conflict on every input;
 *      n* programs ("trap") must have an input that satisfies it. Exhaustive up to 16 argument bits.
 *      This cross-checks the CNF against the expectation independently of c2sat and of the proof.
 * Usage: test_c2_blast <corpus-ir-dir> <lrat-work-dir> */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_blast.h"

static int fails;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static int load(const char *path, C2Unit *u)
{
    size_t n;
    uint8_t *b = slurp(path, &n);
    C2Verdict v;
    int rc = !b || c2_decode(b, n, u, &v) || c2_check_unit(u, &v);
    free(b);
    return rc ? -1 : 0;
}

/* ------------------------------------------------------------------ propagation evaluator */

typedef struct {
    const C2Cnf *c;
    int **occ, *nocc; /* clauses containing literal l at occ[l + nvars] */
    signed char *val;
    int *trail, ntrail;
} Ev;

static void ev_init(Ev *e, const C2Cnf *c)
{
    int nv = c->nvars;
    e->c = c;
    e->occ = calloc((size_t)nv * 2 + 1, sizeof *e->occ);
    e->nocc = calloc((size_t)nv * 2 + 1, sizeof *e->nocc);
    int *cap = calloc((size_t)nv * 2 + 1, sizeof *cap);
    e->val = calloc((size_t)nv + 1, 1);
    e->trail = malloc(((size_t)nv + 1) * sizeof *e->trail);
    for (int i = 0; i < c->nclauses; i++)
        for (const int32_t *l = c->lits + c->start[i]; *l; l++) {
            int s = *l + nv;
            if (e->nocc[s] == cap[s]) { cap[s] = cap[s] ? cap[s] * 2 : 4; e->occ[s] = realloc(e->occ[s], (size_t)cap[s] * sizeof(int)); }
            e->occ[s][e->nocc[s]++] = i;
        }
    free(cap);
}

static void ev_free(Ev *e)
{
    for (int i = 0; i < e->c->nvars * 2 + 1; i++) free(e->occ[i]);
    free(e->occ);
    free(e->nocc);
    free(e->val);
    free(e->trail);
}

static int lval(const Ev *e, int l) { int v = e->val[abs(l)]; return l > 0 ? v : -v; }
static void set(Ev *e, int l) { e->val[abs(l)] = l > 0 ? 1 : -1; e->trail[e->ntrail++] = l; }

/* 1 conflict, 0 every variable assigned without conflict, -1 propagation left variables open */
static int ev_run(Ev *e, int inputs, unsigned long a)
{
    const C2Cnf *c = e->c;
    while (e->ntrail) e->val[abs(e->trail[--e->ntrail])] = 0;
    for (int i = 0; i < c->nclauses; i++) /* unit and empty input clauses */
        if (c->lits[c->start[i]] == 0) return 1;
    for (int i = 0; i < c->nclauses; i++) {
        const int32_t *l = c->lits + c->start[i];
        if (l[1] == 0) {
            if (lval(e, l[0]) < 0) return 1;
            if (!lval(e, l[0])) set(e, l[0]);
        }
    }
    for (int k = 0; k < inputs; k++) {
        int v = 2 + k, l = (a >> k) & 1 ? v : -v;
        if (lval(e, l) < 0) return 1;
        if (!lval(e, l)) set(e, l);
    }
    for (int q = 0; q < e->ntrail; q++) {
        int f = -e->trail[q];
        for (int k = 0; k < e->nocc[f + c->nvars]; k++) {
            const int32_t *l = c->lits + c->start[e->occ[f + c->nvars][k]];
            int open = 0, last = 0, sat = 0;
            for (; *l; l++) {
                int s = lval(e, *l);
                if (s > 0) { sat = 1; break; }
                if (!s) { open++; last = *l; }
            }
            if (sat) continue;
            if (!open) return 1;
            if (open == 1) set(e, last);
        }
    }
    return e->ntrail == c->nvars ? 0 : -1;
}

/* ------------------------------------------------------------------ main */

static int has_nonscalar(const C2Func *f)
{
    for (int r = 0; r < f->nvregs; r++)
        if (!(f->vt[r].s >= S_BOOL && f->vt[r].s <= S_I64)) return 1;
    return 0;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list(const char *dir, const char *suffix, char ***out)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0, cap = 0;
    char **v = NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name), s = strlen(suffix);
        if (l <= s || strcmp(de->d_name + l - s, suffix)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; v = realloc(v, (size_t)cap * sizeof *v); }
        v[n] = malloc(strlen(dir) + l + 2);
        sprintf(v[n++], "%s/%s", dir, de->d_name);
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, by_name);
    *out = v;
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: test_c2_blast <corpus-ir-dir> <lrat-work-dir>\n"); return 2; }
    char **files;
    int nf = list(argv[1], ".ir", &files), refused_nonscalar = 0, scalar_ok = 0, scalar_refused = 0;
    if (nf <= 0) { printf("FAIL no corpus IR in %s\n", argv[1]); return 1; }
    for (int i = 0; i < nf; i++) {
        C2Unit u;
        if (load(files[i], &u)) { printf("FAIL %s: not accepted\n", files[i]); fails++; free(files[i]); continue; }
        for (int fi = 0; fi < u.nfuncs; fi++) {
            C2Cnf cnf;
            C2BlastInfo bi;
            memset(&cnf, 0, sizeof cnf);
            int rc = c2_blast_notrap(&u, fi, &cnf, &bi);
            if (rc == 0) c2_cnf_free(&cnf);
            if (has_nonscalar(&u.fn[fi])) {
                if (rc == 0 || strcmp(bi.rule, "BLAST-UNSUPPORTED") || !strstr(bi.detail, "not a scalar")) {
                    printf("FAIL %s fn %d: non-scalar function not refused as such (rc=%d rule=%s detail=%s)\n",
                           files[i], fi, rc, rc ? bi.rule : "-", rc ? bi.detail : "-");
                    fails++;
                } else refused_nonscalar++;
            } else if (rc == 0) scalar_ok++;
            else scalar_refused++;
        }
        c2_unit_free(&u);
        free(files[i]);
    }
    free(files);
    if (refused_nonscalar == 0) { printf("FAIL corpus has no non-scalar function to refuse\n"); fails++; }
    printf("test_c2_blast: corpus %d units: %d scalar functions blasted, %d scalar refused (calls, division, memory), %d non-scalar refused\n",
           nf, scalar_ok, scalar_refused, refused_nonscalar);

    int np = list(argv[2], ".ir", &files), holds = 0, traps = 0, checked = 0;
    if (np <= 0) { printf("FAIL no LRAT gate IR in %s\n", argv[2]); return 1; }
    for (int i = 0; i < np; i++) {
        const char *base = strrchr(files[i], '/') + 1;
        int want_trap = base[0] == 'n';
        if (base[0] != 'n' && base[0] != 'p') { free(files[i]); continue; }
        C2Unit u;
        C2Cnf cnf;
        C2BlastInfo bi;
        memset(&cnf, 0, sizeof cnf);
        if (load(files[i], &u) || u.nfuncs != 1 || c2_blast_notrap(&u, 0, &cnf, &bi)) {
            printf("FAIL %s: not blasted\n", base);
            fails++;
            free(files[i]);
            continue;
        }
        Ev e;
        memset(&e, 0, sizeof e);
        ev_init(&e, &cnf);
        int ok = 1;
        unsigned long total = bi.inputs <= 16 ? 1UL << bi.inputs : 65536, sat = 0, open = 0;
        unsigned long x = 99;
        for (unsigned long a = 0; a < total; a++) {
            unsigned long in = a;
            if (bi.inputs > 16) { x = x * 6364136223846793005UL + 1442695040888963407UL; in = x >> 7; }
            int r = ev_run(&e, bi.inputs, in);
            if (r == 0) sat++;
            if (r < 0) open++;
        }
        if (open) { printf("FAIL %s: %lu inputs left gates undetermined\n", base, open); ok = 0; }
        if (want_trap && !sat) { printf("FAIL %s: expected a trapping input, none of %lu traps\n", base, total); ok = 0; }
        if (!want_trap && sat) { printf("FAIL %s: %lu of %lu inputs trap but the program is expected to hold\n", base, sat, total); ok = 0; }
        if (ok) { checked++; if (want_trap) traps++; else holds++; } else fails++;
        ev_free(&e);
        c2_cnf_free(&cnf);
        c2_unit_free(&u);
        free(files[i]);
    }
    free(files);
    if (holds < 20) { printf("FAIL only %d holding programs checked (need 20)\n", holds); fails++; }
    printf("test_c2_blast: %d gate programs evaluated input by input (%d hold, %d trap), %d failures\n", checked, holds, traps, fails);
    return fails ? 1 : 0;
}
