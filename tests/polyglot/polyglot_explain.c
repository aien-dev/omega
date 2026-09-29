/* POLYGLOT-0 explainer (lane F). spec/polyglot-0.md section 9.
 *
 * Reads ONLY the contract file and the receipt directory (no registry, no
 * hard-coded winners) and prints, per workload and sparsity: the selected
 * candidate, the runner-up, the measured margin and its noise band, the pack
 * cost with break-even against the cheapest-to-pack eligible candidate, and
 * every other candidate with the reason it was not selected.
 *
 * Selection policy (printed in the output):
 *   current run = --run RUN_ID, or the only non-smoke run in the directory
 *              (several non-smoke runs and no --run: nothing is selected);
 *   eligible = receipt contract digest equals the digest of the contract file
 *              AND not a smoke receipt AND from the current run AND
 *              gate_eligible (N >= 20) AND tree_dirty_files == 0 AND the cell
 *              was measured AND correctness checks > 0 with 0 failures AND
 *              0 < min <= median AND no kept sample under 1 ms AND not
 *              toolchain_only (spec section 4) AND not a labelled weak
 *              baseline;
 *   selected = lowest median ns per call (pack once, amortized); ties on the
 *              exact median are broken by id (byte order);
 *   verdict  = CHOSEN if (runner-up median / selected median - 1) exceeds the
 *              noise band max(3 * max(MAD/median) of the two, RUN_FLOOR),
 *              else TIE. RUN_FLOOR (5%) is derived below from measured
 *              run-to-run spread.
 * Deterministic: receipts are read in byte order of file name, numbers are
 * printed with fixed precision, nothing depends on time or environment.
 *
 * Usage: polyglot_explain --spec spec/polyglot-0.md --receipts evidence/POLYGLOT [--run RUN_ID]
 */
#define _GNU_SOURCE
#include "polyglot/omx_bench.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHEMA "OMEGA_POLYGLOT0_RECEIPT_V1"

/* Run-to-run floor of the noise band (review G-S7). Within-run MAD does not
 * see run-to-run, core-to-core or boot-state spread, so the band is never
 * narrower than RUN_FLOOR. Derivation (measured, not assumed): the two MA-3
 * bench runs evidence/MIXED_ALGEBRA/ma3_bench_run1.json and _run2.json (same
 * code, same X925 core, one minute apart) give 360 paired medians. Their
 * run-to-run shift |ln(median2 / median1)| has p50 0.27%, p90 2.3%, p95
 * 3.6%, p99 10.4%, max 18.8%; 22 of 360 (6.1%) exceed 3 x the within-run
 * noise, i.e. a within-run band alone misjudges about one comparison in 16.
 * Rule: RUN_FLOOR is the smallest whole percent at which at most 2.5% of the
 * 360 shifts exceed max(3 x within-run noise, floor): 4% leaves 10 (2.8%),
 * 5% leaves 8 (2.2%). A CHOSEN therefore needs a margin that one run-to-run
 * shift explains in at most about 1 case in 40; smaller margins are TIE.
 * Re-derive when more paired runs exist (two full runs on different X925
 * cores would replace this estimate). */
#define RUN_FLOOR 0.05

/* ---- minimal JSON reader ---- */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct jv {
    int t;
    double num;
    char *str;           /* J_STR */
    struct jv **kids;    /* J_ARR, J_OBJ values */
    char **keys;         /* J_OBJ */
    size_t n;
} jv;

typedef struct { const char *p, *e; int err; } jp;

static void ws(jp *s) {
    while (s->p < s->e && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) s->p++;
}

static jv *jnew(int t) {
    jv *v = calloc(1, sizeof *v);
    if (!v) { fprintf(stderr, "out of memory\n"); exit(2); }
    v->t = t;
    return v;
}

static char *jparse_str(jp *s) {
    if (s->p >= s->e || *s->p != '"') { s->err = 1; return NULL; }
    s->p++;
    size_t cap = 32, n = 0;
    char *o = malloc(cap);
    while (o && s->p < s->e && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->e) {
            char d = *s->p++;
            if (d == 'n') c = '\n';
            else if (d == 't') c = '\t';
            else if (d == 'u' && s->e - s->p >= 4) {
                char h[5] = {s->p[0], s->p[1], s->p[2], s->p[3], 0};
                c = (char)strtol(h, NULL, 16);
                s->p += 4;
            } else c = d;
        }
        if (n + 2 > cap) {
            char *no = realloc(o, cap *= 2);
            if (!no) { free(o); o = NULL; break; }
            o = no;
        }
        o[n++] = c;
    }
    if (!o || s->p >= s->e) { s->err = 1; free(o); return NULL; }
    s->p++;
    o[n] = 0;
    return o;
}

static jv *jparse(jp *s, int depth) {
    ws(s);
    if (s->p >= s->e || depth > 64) { s->err = 1; return NULL; }
    char c = *s->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        jv *v = jnew(obj ? J_OBJ : J_ARR);
        s->p++;
        ws(s);
        if (s->p < s->e && *s->p == (obj ? '}' : ']')) { s->p++; return v; }
        for (;;) {
            char *k = NULL;
            if (obj) {
                ws(s);
                k = jparse_str(s);
                ws(s);
                if (!k || s->p >= s->e || *s->p != ':') { s->err = 1; free(k); return v; }
                s->p++;
            }
            jv *kid = jparse(s, depth + 1);
            if (s->err) { free(k); return v; }
            v->kids = realloc(v->kids, (v->n + 1) * sizeof *v->kids);
            if (obj) v->keys = realloc(v->keys, (v->n + 1) * sizeof *v->keys);
            if (!v->kids || (obj && !v->keys)) { fprintf(stderr, "out of memory\n"); exit(2); }
            v->kids[v->n] = kid;
            if (obj) v->keys[v->n] = k;
            v->n++;
            ws(s);
            if (s->p < s->e && *s->p == ',') { s->p++; continue; }
            if (s->p < s->e && *s->p == (obj ? '}' : ']')) { s->p++; return v; }
            s->err = 1;
            return v;
        }
    }
    if (c == '"') {
        jv *v = jnew(J_STR);
        v->str = jparse_str(s);
        return v;
    }
    if (!strncmp(s->p, "true", 4) || !strncmp(s->p, "false", 5)) {
        jv *v = jnew(J_BOOL);
        v->num = c == 't';
        s->p += c == 't' ? 4 : 5;
        return v;
    }
    if (!strncmp(s->p, "null", 4)) {
        s->p += 4;
        return jnew(J_NULL);
    }
    char *end;
    double d = strtod(s->p, &end);
    if (end == s->p) { s->err = 1; return NULL; }
    s->p = end;
    jv *v = jnew(J_NUM);
    v->num = d;
    return v;
}

static const jv *jget(const jv *o, const char *k) {
    if (!o || o->t != J_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++)
        if (!strcmp(o->keys[i], k)) return o->kids[i];
    return NULL;
}
static double jnum(const jv *o, const char *k, double dflt) {
    const jv *v = jget(o, k);
    return v && (v->t == J_NUM || v->t == J_BOOL) ? v->num : dflt;
}
static const char *jstr(const jv *o, const char *k) {
    const jv *v = jget(o, k);
    return v && v->t == J_STR && v->str ? v->str : "";
}
static int jtrue(const jv *o, const char *k) {
    const jv *v = jget(o, k);
    return v && v->t == J_BOOL && v->num != 0;
}

/* ---- receipts ---- */
typedef struct {
    char file[256];
    char id[64], language[32], toolchain[160], family[32], run_id[96], contract[65], wid[8], regime[96];
    int weak, derived, tonly, smoke, gate_ok;
    double dirty; /* tree_dirty_files, -1 when null or missing */
    size_t m, n;
    int ncells;
    struct {
        double sp, med, mad, min, pack_med, pack_pw, short_kept;
        int measured;
        double checks, failures;
        char skip[160];
    } cell[8];
} rec;

static rec *g_rec;
static size_t g_nrec;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int cmp_arr(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (l < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)l + 1);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[l] = 0; *len = (size_t)l; }
    return b;
}

static void cpy(char *d, size_t cap, const char *s) { snprintf(d, cap, "%s", s ? s : ""); }

static int load_receipt(const char *dir, const char *name, rec *r) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len = 0;
    char *b = slurp(path, &len);
    if (!b) return -1;
    jp s = {b, b + len, 0};
    jv *root = jparse(&s, 0);
    free(b);
    if (s.err || !root || strcmp(jstr(root, "schema"), SCHEMA)) return -1; /* tree leaked: short-lived tool */
    memset(r, 0, sizeof *r);
    cpy(r->file, sizeof r->file, name);
    const jv *c = jget(root, "candidate"), *w = jget(root, "workload"), *run = jget(root, "run"),
             *ct = jget(root, "contract");
    cpy(r->id, sizeof r->id, jstr(c, "id"));
    cpy(r->language, sizeof r->language, jstr(c, "language"));
    cpy(r->toolchain, sizeof r->toolchain, jstr(c, "toolchain"));
    cpy(r->family, sizeof r->family, jstr(c, "representation"));
    r->weak = (int)jnum(c, "weak_baseline", 0);
    r->derived = (int)jnum(c, "compiler_derived", 0);
    r->tonly = (int)jnum(c, "toolchain_only", 0);
    cpy(r->run_id, sizeof r->run_id, jstr(run, "run_id"));
    r->smoke = jtrue(run, "smoke");
    r->gate_ok = jtrue(run, "gate_eligible");
    r->dirty = jnum(run, "tree_dirty_files", -1);
    cpy(r->contract, sizeof r->contract, jstr(ct, "sha256"));
    cpy(r->wid, sizeof r->wid, jstr(w, "id"));
    cpy(r->regime, sizeof r->regime, jstr(w, "regime"));
    r->m = (size_t)jnum(w, "m", 0);
    r->n = (size_t)jnum(w, "n", 0);
    const jv *cells = jget(root, "cells");
    for (size_t i = 0; cells && cells->t == J_ARR && i < cells->n && r->ncells < 8; i++) {
        const jv *ce = cells->kids[i];
        int k = r->ncells++;
        r->cell[k].sp = jnum(ce, "sparsity", -1);
        r->cell[k].measured = jtrue(ce, "measured");
        const jv *cor = jget(ce, "correctness"), *lat = jget(ce, "latency_ns"), *pk = jget(ce, "pack");
        r->cell[k].checks = jnum(cor, "checks", 0);
        r->cell[k].failures = jnum(cor, "failures", 0);
        r->cell[k].med = jnum(lat, "median", NAN);
        r->cell[k].mad = jnum(lat, "mad", NAN);
        r->cell[k].min = jnum(lat, "min", NAN);
        r->cell[k].short_kept = jnum(ce, "short_samples_kept", -1);
        r->cell[k].pack_med = jnum(pk, "ns_median", NAN);
        r->cell[k].pack_pw = jnum(pk, "ns_per_weight", NAN);
        cpy(r->cell[k].skip, sizeof r->cell[k].skip, jstr(ce, "skip_reason"));
    }
    return 0;
}

/* ---- selection ---- */
typedef struct {
    const rec *r;
    int k; /* cell index */
    int eligible;
    char why[200];
} entry;

static int cmp_entry(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->eligible != y->eligible) return y->eligible - x->eligible;
    if (x->eligible) {
        double mx = x->r->cell[x->k].med, my = y->r->cell[y->k].med;
        if (mx != my) return mx < my ? -1 : 1;
    }
    return strcmp(x->r->id, y->r->id);
}

static double relmad(const entry *e) {
    double med = e->r->cell[e->k].med;
    return med > 0 ? e->r->cell[e->k].mad / med : 0;
}

int main(int argc, char **argv) {
    const char *spec = NULL, *dir = NULL, *want_run = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--spec")) spec = argv[i + 1];
        else if (!strcmp(argv[i], "--receipts")) dir = argv[i + 1];
        else if (!strcmp(argv[i], "--run")) want_run = argv[i + 1];
    }
    if (!spec || !dir) {
        fprintf(stderr, "usage: %s --spec spec/polyglot-0.md --receipts DIR [--run RUN_ID]\n", argv[0]);
        return 2;
    }
    char contract[65];
    if (omx_contract_digest(spec, contract)) {
        fprintf(stderr, "polyglot_explain: cannot read contract section 1 from %s\n", spec);
        return 2;
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "polyglot_explain: cannot open %s\n", dir);
        return 2;
    }
    char **names = NULL;
    size_t nn = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 5 && !strcmp(de->d_name + l - 5, ".json")) {
            names = realloc(names, (nn + 1) * sizeof *names);
            if (!names || !(names[nn] = strdup(de->d_name))) return 2;
            nn++;
        }
    }
    closedir(d);
    if (nn) qsort(names, nn, sizeof *names, cmp_str);
    g_rec = calloc(nn ? nn : 1, sizeof *g_rec);
    size_t ignored = 0;
    for (size_t i = 0; i < nn; i++) {
        if (load_receipt(dir, names[i], &g_rec[g_nrec]) == 0) g_nrec++;
        else ignored++;
    }

    size_t stale = 0, smoke = 0, notgate = 0;
    char runs[64][96];
    size_t nruns = 0, nfull = 0;
    for (size_t i = 0; i < g_nrec; i++) {
        stale += strcmp(g_rec[i].contract, contract) != 0;
        smoke += g_rec[i].smoke;
        notgate += !g_rec[i].gate_ok;
        size_t j = 0;
        for (; j < nruns; j++)
            if (!strcmp(runs[j], g_rec[i].run_id)) break;
        if (j == nruns && nruns < 64) cpy(runs[nruns++], sizeof runs[0], g_rec[i].run_id);
    }
    qsort(runs, nruns, sizeof runs[0], cmp_arr);
    /* Current run (review G-S6): --run, or the only non-smoke run present.
     * With several non-smoke runs and no --run, nothing is selected. */
    const char *cur = want_run;
    for (size_t j = 0; j < nruns; j++) {
        int full = 0;
        for (size_t i = 0; i < g_nrec; i++)
            if (!strcmp(g_rec[i].run_id, runs[j]) && !g_rec[i].smoke) full = 1;
        if (full) {
            nfull++;
            if (!want_run && nfull == 1) cur = runs[j];
        }
    }
    if (!want_run && nfull > 1) cur = NULL;

    printf("POLYGLOT-0 explanation (spec/polyglot-0.md section 9)\n");
    printf("contract section 1 sha256: %s\n", contract);
    printf("receipts: %zu read, %zu ignored (not %s), %zu with a stale contract digest, %zu smoke, %zu not "
           "gate-eligible\n", g_nrec, ignored, SCHEMA, stale, smoke, notgate);
    for (size_t j = 0; j < nruns; j++) printf("run: %s\n", runs[j]);
    printf("current run: %s\n", cur ? cur : "none");
    printf("policy: eligible = contract digest matches, not smoke, from the current run, gate-eligible (N >= 20),\n"
           "        built from a clean tree, measured, checks > 0 and 0 failures, 0 < min <= median, no kept\n"
           "        sample under 1 ms, not toolchain-only, not a weak baseline; select lowest median ns/call\n"
           "        (pack once, amortized); CHOSEN if the runner-up is slower by more than the band\n"
           "        max(3 x max(MAD/median) of the two, %.0f%% run-to-run floor), else TIE.\n",
           RUN_FLOOR * 100.0);
    if (smoke || notgate) printf("NOTE: smoke or N<20 receipts present: they are never selected.\n");
    if (!cur && nfull > 1)
        printf("NOTE: %zu non-smoke runs present and no --run given: nothing is selected (pass --run RUN_ID).\n",
               nfull);
    else if (!cur)
        printf("NOTE: no non-smoke run present: nothing is selected.\n");

    /* workloads in byte order of id */
    char wids[16][8];
    size_t nw = 0;
    for (size_t i = 0; i < g_nrec; i++) {
        size_t j = 0;
        for (; j < nw; j++)
            if (!strcmp(wids[j], g_rec[i].wid)) break;
        if (j == nw && nw < 16) cpy(wids[nw++], sizeof wids[0], g_rec[i].wid);
    }
    qsort(wids, nw, sizeof wids[0], cmp_arr);

    entry *es = calloc(g_nrec ? g_nrec : 1, sizeof *es);
    for (size_t wi = 0; wi < nw; wi++) {
        const rec *any = NULL;
        double sps[8];
        int nsp = 0;
        for (size_t i = 0; i < g_nrec; i++) {
            if (strcmp(g_rec[i].wid, wids[wi])) continue;
            if (!any) any = &g_rec[i];
            for (int k = 0; k < g_rec[i].ncells; k++) {
                int f = 0;
                for (int j = 0; j < nsp; j++) f |= fabs(sps[j] - g_rec[i].cell[k].sp) < 1e-9;
                if (!f && nsp < 8) sps[nsp++] = g_rec[i].cell[k].sp;
            }
        }
        for (int a = 0; a < nsp; a++) /* sort sparsities */
            for (int b = a + 1; b < nsp; b++)
                if (sps[b] < sps[a]) { double t = sps[a]; sps[a] = sps[b]; sps[b] = t; }
        printf("\n%s  m=%zu n=%zu  (%s)\n", wids[wi], any->m, any->n, any->regime);
        for (int si = 0; si < nsp; si++) {
            size_t ne = 0;
            for (size_t i = 0; i < g_nrec; i++) {
                const rec *r = &g_rec[i];
                if (strcmp(r->wid, wids[wi])) continue;
                for (int k = 0; k < r->ncells; k++) {
                    if (fabs(r->cell[k].sp - sps[si]) > 1e-9) continue;
                    entry *e = &es[ne++];
                    e->r = r;
                    e->k = k;
                    e->eligible = 0;
                    const double cmin = r->cell[k].min, cmed = r->cell[k].med;
                    if (strcmp(r->contract, contract))
                        snprintf(e->why, sizeof e->why, "stale receipt: contract digest %.12s differs", r->contract);
                    else if (r->smoke)
                        snprintf(e->why, sizeof e->why, "smoke receipt (never gate evidence)");
                    else if (!cur || strcmp(r->run_id, cur))
                        snprintf(e->why, sizeof e->why, "not from the current run (run %.60s)", r->run_id);
                    else if (!r->gate_ok)
                        snprintf(e->why, sizeof e->why, "not gate-eligible (N < 20 samples)");
                    else if (r->dirty != 0)
                        snprintf(e->why, sizeof e->why, "built from a dirty tree (tree_dirty_files %.0f)", r->dirty);
                    else if (!r->cell[k].measured)
                        snprintf(e->why, sizeof e->why, "not measured: %s", r->cell[k].skip);
                    else if (r->cell[k].checks <= 0 || r->cell[k].failures > 0)
                        snprintf(e->why, sizeof e->why, "not bit-exact: %.0f of %.0f checks failed",
                                 r->cell[k].failures, r->cell[k].checks);
                    else if (!(isfinite(cmin) && isfinite(cmed) && cmin > 0 && cmin <= cmed))
                        snprintf(e->why, sizeof e->why, "inconsistent latency (min %.1f, median %.1f)", cmin, cmed);
                    else if (r->cell[k].short_kept != 0)
                        snprintf(e->why, sizeof e->why, "%s",
                                 r->cell[k].short_kept < 0 ? "no sample-length record (receipt predates it)"
                                                           : "kept samples shorter than 1 ms");
                    else if (r->tonly)
                        snprintf(e->why, sizeof e->why, "toolchain-only (own-encoder rule, spec section 4)");
                    else if (r->weak)
                        snprintf(e->why, sizeof e->why, "labelled weak baseline (reference only)");
                    else
                        e->eligible = 1;
                    break;
                }
            }
            qsort(es, ne, sizeof *es, cmp_entry);
            printf("  sparsity %.2f:\n", sps[si]);
            if (ne == 0 || !es[0].eligible) {
                printf("    no eligible candidate\n");
            } else {
                const entry *s = &es[0];
                const rec *r = s->r;
                printf("    SELECT    %-16s %-12s %-9s median %12.1f ns/call  MAD %8.1f  min %12.1f\n", r->id,
                       r->language, r->family, r->cell[s->k].med, r->cell[s->k].mad, r->cell[s->k].min);
                if (ne > 1 && es[1].eligible) {
                    const entry *u = &es[1];
                    double margin = u->r->cell[u->k].med / r->cell[s->k].med - 1.0;
                    double band = fmax(3.0 * fmax(relmad(s), relmad(u)), RUN_FLOOR);
                    printf("    runner-up %-16s %-12s %-9s median %12.1f ns/call  margin %+.1f%%  band %.1f%%  -> %s\n",
                           u->r->id, u->r->language, u->r->family, u->r->cell[u->k].med, margin * 100.0,
                           band * 100.0, margin > band ? "CHOSEN" : "TIE (within noise)");
                } else {
                    printf("    runner-up none eligible -> CHOSEN by default\n");
                }
                /* pack cost and break-even vs cheapest-to-pack eligible */
                const entry *cheap = NULL;
                for (size_t i = 0; i < ne; i++)
                    if (es[i].eligible && (!cheap || es[i].r->cell[es[i].k].pack_med < cheap->r->cell[cheap->k].pack_med))
                        cheap = &es[i];
                double pp = r->cell[s->k].pack_med, cp = cheap->r->cell[cheap->k].pack_med;
                double dt = cheap->r->cell[cheap->k].med - r->cell[s->k].med;
                printf("    pack      %.4f ns/weight (%.0f ns); cheapest to pack %s %.4f ns/weight; ", r->cell[s->k].pack_pw,
                       pp, cheap->r->id, cheap->r->cell[cheap->k].pack_pw);
                if (cheap == s || pp <= cp) printf("break-even 0 calls\n");
                else if (dt <= 0) printf("break-even never\n");
                else printf("break-even %.0f calls\n", ceil((pp - cp) / dt));
                /* best per language (eligible only) */
                printf("    by language:");
                char seen[16][32];
                int nseen = 0;
                for (size_t i = 0; i < ne; i++) {
                    if (!es[i].eligible) continue;
                    int f = 0;
                    for (int j = 0; j < nseen; j++) f |= !strcmp(seen[j], es[i].r->language);
                    if (f || nseen >= 16) continue;
                    cpy(seen[nseen++], sizeof seen[0], es[i].r->language);
                    printf(" %s=%s %.1f ns%s;", es[i].r->language, es[i].r->id, es[i].r->cell[es[i].k].med,
                           es[i].r->derived ? " (compiler-derived)" : "");
                }
                printf("\n");
            }
            for (size_t i = ne && es[0].eligible ? 1 : 0; i < ne; i++) {
                const entry *e = &es[i];
                if (e->eligible) {
                    double margin = e->r->cell[e->k].med / es[0].r->cell[es[0].k].med - 1.0;
                    if (i == 1) continue; /* runner-up printed above */
                    printf("    rejected  %-16s %-12s slower: %+.1f%% median vs selected\n", e->r->id,
                           e->r->language, margin * 100.0);
                } else {
                    printf("    rejected  %-16s %-12s %s", e->r->id, e->r->language, e->why);
                    if (e->r->cell[e->k].measured && isfinite(e->r->cell[e->k].med) && es[0].eligible)
                        printf(" (median %+.1f%% vs selected)",
                               (e->r->cell[e->k].med / es[0].r->cell[es[0].k].med - 1.0) * 100.0);
                    printf("\n");
                }
            }
        }
    }
    return 0;
}
