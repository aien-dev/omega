/* test_c2_blast_native: checker2's bit-blaster against the producer's compiled code (omega#446).
 *
 * Independent cross-check of the blaster's semantics. The other side is NOT checker2 code and NOT
 * a hand-written label: it is the AArch64 machine code oscc emits (`oscc file.osc OUT` writes
 * OUT.ir, OUT.code, OUT.entries), mapped executable and called as a black box per the documented
 * native ABI (docs/osc/OSC-1-DESIGN.md section 7: params in x0..x5, return in x0, runtime pointer
 * in x7). Every runtime entry the code reaches is a trap here (the blastable subset has no calls,
 * memory or handles), and the trap code is read from the second argument register. Nothing of
 * src/compiler is linked or read; only oscc's output bytes are used.
 *
 * The CNF side is the formula checker2 builds from oscc's own IR bytes, evaluated with unit
 * propagation after fixing the argument bits as units (full Tseitin encoding: fixing the inputs
 * fixes every gate, so the answer is exact; an undetermined gate fails the test).
 *
 * Two kinds of program, by file name:
 *   op_*   fn f(<operands>, c: T) -> T ensures result != c { return <one operation>; }
 *          The ensures clause exposes the operation's value to the CNF. For each operand vector:
 *          run the code with c = 0. If it returns r (or traps ENSURES with c = 0, then r = 0, and a
 *          rerun with c = 1 must return 0), the CNF must be satisfiable at c = r (the ensures trap)
 *          and unsatisfiable at c = r ^ 1. If the code traps with any other code, the operation
 *          itself traps and the CNF must be satisfiable at c = 0 and at c = 1.
 *   other  (the LRAT gate programs): the code traps with a code other than REQUIRES (9) exactly on
 *          the inputs at which the CNF is satisfiable.
 * Inputs: exhaustive up to 16 operand bits; otherwise edge values of each operand type (0, 1, 2,
 * -1, -2, MIN, MIN+1, MAX, MAX-1, the sign bit, every shift amount 0..w+1) in all combinations,
 * plus 3000 pseudo-random vectors.
 *
 * Negative controls (file "controls" in the work directory, lines "<ir-name> <code-name>"): the
 * CNF of one program checked against the code of a different operation of the same signature must
 * disagree on at least one input, so the comparison can tell the operations apart.
 *
 * AArch64 Linux only (the documented native ABI); the harness refuses to run elsewhere.
 * Usage: test_c2_blast_native <work-dir>   (written by tests/checker2/blast_native.sh) */
#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "c2_blast.h"

#if !defined(__aarch64__) || !defined(__linux__)
int main(void)
{
    printf("test_c2_blast_native: FAIL not an AArch64 Linux host; the native cross-check cannot run\n");
    return 1;
}
#else

#define TRAP_REQUIRES 9
#define TRAP_ENSURES 10

static int fails;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) b[len] = 0;
    *n = (size_t)len;
    return b;
}

/* ------------------------------------------------------------------ CNF side */

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
    e->ntrail = 0;
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

/* bits[k] is argument bit k (CNF variable 2 + k). 1 satisfiable (a trap is reachable), 0 conflict,
 * -1 propagation left gates undetermined. */
static int ev_sat(Ev *e, const uint8_t *bits, int inputs)
{
    const C2Cnf *c = e->c;
    while (e->ntrail) e->val[abs(e->trail[--e->ntrail])] = 0;
    for (int i = 0; i < c->nclauses; i++)
        if (c->lits[c->start[i]] == 0) return 0;
    for (int i = 0; i < c->nclauses; i++) {
        const int32_t *l = c->lits + c->start[i];
        if (l[1] == 0) {
            if (lval(e, l[0]) < 0) return 0;
            if (!lval(e, l[0])) set(e, l[0]);
        }
    }
    for (int k = 0; k < inputs; k++) {
        int l = bits[k] ? 2 + k : -(2 + k);
        if (lval(e, l) < 0) return 0;
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
            if (!open) return 0;
            if (open == 1) set(e, last);
        }
    }
    return e->ntrail == c->nvars ? 1 : -1;
}

/* ------------------------------------------------------------------ native side */

static sigjmp_buf jb;
static volatile uint64_t trap_code;
static void *rt_table[512];

static void rt_entry(uint64_t x0, uint64_t x1)
{
    (void)x0;
    trap_code = x1;
    siglongjmp(jb, 1);
}

typedef uint64_t (*NativeFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void *);

typedef struct {
    uint8_t *map;
    size_t maplen;
    NativeFn fn;
} Native;

static int native_load(const char *code_path, const char *entries_path, const char *fname, Native *nt)
{
    size_t n, en;
    uint8_t *code = slurp(code_path, &n);
    uint8_t *ent = slurp(entries_path, &en);
    long off = -1;
    if (code && ent) {
        for (char *line = strtok((char *)ent, "\n"); line; line = strtok(NULL, "\n")) {
            char *name = strrchr(line, ' ');
            if (name && strcmp(name + 1, fname) == 0) off = strtol(line, NULL, 10);
        }
    }
    free(ent);
    if (!code || off < 0 || (size_t)off >= n) { free(code); return -1; }
    nt->maplen = (n + 4095) & ~(size_t)4095;
    nt->map = mmap(NULL, nt->maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nt->map == MAP_FAILED) { free(code); return -1; }
    memcpy(nt->map, code, n);
    free(code);
    if (mprotect(nt->map, nt->maplen, PROT_READ | PROT_EXEC)) { munmap(nt->map, nt->maplen); return -1; }
    __builtin___clear_cache((char *)nt->map, (char *)nt->map + n);
    nt->fn = (NativeFn)(void *)(nt->map + off);
    return 0;
}

/* 0 returned (*ret), 1 trapped (*code) */
static int native_run(const Native *nt, const uint64_t *a, int na, uint64_t *ret, uint64_t *code)
{
    uint64_t x[6] = {0};
    for (int i = 0; i < na && i < 6; i++) x[i] = a[i];
    if (sigsetjmp(jb, 0) == 0) {
        *ret = nt->fn(x[0], x[1], x[2], x[3], x[4], x[5], 0, rt_table);
        return 0;
    }
    *code = trap_code;
    return 1;
}

/* ------------------------------------------------------------------ values */

static int width(uint8_t s)
{
    static const int w[] = {0, 1, 8, 16, 32, 64, 8, 16, 32, 64};
    return s <= S_I64 ? w[s] : 0;
}
static int is_signed(uint8_t s) { return s >= S_I8 && s <= S_I64; }
static uint64_t mask(int w) { return w == 64 ? ~0ULL : (1ULL << w) - 1; }

/* canonical 64-bit register value of the low w bits of v */
static uint64_t canon(uint64_t v, uint8_t s)
{
    int w = width(s);
    v &= mask(w);
    if (is_signed(s) && w < 64 && (v >> (w - 1)) & 1) v |= ~mask(w);
    return v;
}

/* edge values of a type, as low-w-bit patterns */
static int edges(uint8_t s, uint64_t *out)
{
    int w = width(s), n = 0;
    if (w == 1) { out[0] = 0; out[1] = 1; return 2; }
    uint64_t m = mask(w), msb = 1ULL << (w - 1);
    uint64_t base[] = {0, 1, 2, 3, m, m - 1, msb, msb + 1, msb - 1, msb - 2};
    for (unsigned i = 0; i < sizeof base / sizeof *base; i++) out[n++] = base[i] & m;
    for (int k = 4; k <= w + 1; k++) out[n++] = (uint64_t)k & m; /* every shift amount */
    return n;
}

static uint64_t rng = 88172645463325252ULL;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* operand vectors for params 0..np-1: exhaustive up to 16 bits, else edges x edges + random */
typedef struct {
    uint64_t (*v)[6];
    long n, cap;
} Vecs;

static void push(Vecs *vs, const uint64_t *x, int np)
{
    if (vs->n == vs->cap) { vs->cap = vs->cap ? vs->cap * 2 : 1024; vs->v = realloc(vs->v, (size_t)vs->cap * sizeof *vs->v); }
    for (int i = 0; i < 6; i++) vs->v[vs->n][i] = i < np ? x[i] : 0;
    vs->n++;
}

static void vectors(const C2Func *f, int np, Vecs *vs)
{
    int bits = 0;
    for (int i = 0; i < np; i++) bits += width(f->vt[i].s);
    uint64_t x[6] = {0};
    if (bits <= 16) {
        for (uint64_t a = 0; a < (1ULL << bits); a++) {
            int sh = 0;
            for (int i = 0; i < np; i++) { int w = width(f->vt[i].s); x[i] = (a >> sh) & mask(w); sh += w; }
            push(vs, x, np);
        }
        return;
    }
    uint64_t e[6][80];
    int ne[6], idx[6] = {0};
    long combos = 1;
    for (int i = 0; i < np; i++) { ne[i] = edges(f->vt[i].s, e[i]); combos *= ne[i]; }
    if (combos <= 20000)
        for (long c = 0; c < combos; c++) {
            long r = c;
            for (int i = 0; i < np; i++) { idx[i] = (int)(r % ne[i]); r /= ne[i]; x[i] = e[i][idx[i]]; }
            push(vs, x, np);
        }
    for (int k = 0; k < 3000; k++) {
        for (int i = 0; i < np; i++) {
            /* mix: sometimes an edge value, sometimes a uniform pattern */
            uint64_t r = rnd();
            x[i] = (r & 3) == 0 ? e[i][(r >> 8) % (uint64_t)ne[i]] : rnd() & mask(width(f->vt[i].s));
        }
        push(vs, x, np);
    }
}

static void put_bits(uint8_t *bits, int *at, uint64_t v, int w)
{
    for (int k = 0; k < w; k++) bits[(*at)++] = (uint8_t)((v >> k) & 1);
}

/* ------------------------------------------------------------------ one program */

typedef struct {
    long vectors, native_traps, mismatches, undetermined;
    char first[200];
} Stat;

static void note(Stat *st, const char *what, const uint64_t *x, int np, uint64_t extra)
{
    st->mismatches++;
    if (st->first[0]) return;
    int k = snprintf(st->first, sizeof st->first, "%s at", what);
    for (int i = 0; i < np && k < (int)sizeof st->first; i++)
        k += snprintf(st->first + k, sizeof st->first - (size_t)k, " %#llx", (unsigned long long)x[i]);
    if (k < (int)sizeof st->first) snprintf(st->first + k, sizeof st->first - (size_t)k, " (%#llx)", (unsigned long long)extra);
}

/* compare the CNF of irfile with the code of codebase; value mode for op_* programs */
static int compare(const char *dir, const char *irname, const char *codename, Stat *st)
{
    char p[1024], q[1024];
    memset(st, 0, sizeof *st);
    snprintf(p, sizeof p, "%s/%s.ir", dir, irname);
    size_t n;
    uint8_t *b = slurp(p, &n);
    C2Unit *u = calloc(1, sizeof *u);
    C2Verdict v;
    if (!b || !u || c2_decode(b, n, u, &v) || c2_check_unit(u, &v) || u->nfuncs != 1) {
        printf("FAIL %s: IR not accepted by checker2\n", irname);
        free(b);
        free(u);
        return -1;
    }
    free(b);
    C2Cnf cnf;
    C2BlastInfo bi;
    memset(&cnf, 0, sizeof cnf);
    if (c2_blast_notrap(u, 0, &cnf, &bi)) {
        printf("FAIL %s: not blasted (%s %s)\n", irname, bi.rule, bi.detail);
        c2_unit_free(u);
        free(u);
        return -1;
    }
    Native nt;
    snprintf(p, sizeof p, "%s/%s.code", dir, codename);
    snprintf(q, sizeof q, "%s/%s.entries", dir, codename);
    const C2Func *f = &u->fn[0];
    int rc = -1;
    if (native_load(p, q, f->name, &nt)) {
        printf("FAIL %s: cannot load native code %s\n", irname, codename);
        goto out;
    }
    Ev e;
    ev_init(&e, &cnf);
    int value_mode = strncmp(irname, "op_", 3) == 0;
    int np = f->nparams, nops = value_mode ? np - 1 : np;
    uint8_t cs = f->vt[np - 1].s;
    int cw = width(cs);
    Vecs vs = {0};
    vectors(f, nops, &vs);
    uint8_t *bits = malloc((size_t)bi.inputs + 1);
    for (long i = 0; i < vs.n; i++) {
        uint64_t a[6], ret = 0, code = 0;
        int at = 0;
        for (int k = 0; k < nops; k++) { a[k] = canon(vs.v[i][k], f->vt[k].s); put_bits(bits, &at, vs.v[i][k], width(f->vt[k].s)); }
        st->vectors++;
        if (!value_mode) {
            int t = native_run(&nt, a, np, &ret, &code);
            int native_trap = t && code != TRAP_REQUIRES;
            st->native_traps += native_trap;
            int s = ev_sat(&e, bits, bi.inputs);
            if (s < 0) { st->undetermined++; continue; }
            if (s != native_trap) note(st, native_trap ? "code traps, CNF holds" : "CNF traps, code holds", vs.v[i], nops, code);
            continue;
        }
        /* value mode: last parameter c of the result type */
        a[np - 1] = 0;
        int t = native_run(&nt, a, np, &ret, &code);
        int optrap = 0;
        uint64_t r = 0;
        if (!t) r = ret & mask(cw);
        else if (code == TRAP_ENSURES) {
            a[np - 1] = canon(1, cs);
            if (native_run(&nt, a, np, &ret, &code) || (ret & mask(cw)) != 0) { note(st, "ENSURES trap at c=0 not confirmed at c=1", vs.v[i], nops, code); continue; }
            r = 0;
        } else optrap = 1;
        st->native_traps += optrap;
        uint64_t cvals[2] = {optrap ? 0 : r, optrap ? 1 : (r ^ 1) & mask(cw)};
        int want[2] = {1, optrap};
        for (int j = 0; j < 2; j++) {
            int at2 = at;
            put_bits(bits, &at2, cvals[j], cw);
            int s = ev_sat(&e, bits, bi.inputs);
            if (s < 0) { st->undetermined++; continue; }
            if (s != want[j])
                note(st, optrap ? "code traps, CNF does not" : (j ? "CNF disagrees: traps at c = r^1" : "CNF disagrees: no trap at c = r"),
                     vs.v[i], nops, optrap ? code : r);
        }
    }
    free(bits);
    free(vs.v);
    ev_free(&e);
    munmap(nt.map, nt.maplen);
    rc = 0;
out:
    c2_cnf_free(&cnf);
    c2_unit_free(u);
    free(u);
    return rc;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: test_c2_blast_native <work-dir>\n"); return 2; }
    const char *dir = argv[1];
    for (unsigned i = 0; i < sizeof rt_table / sizeof *rt_table; i++) rt_table[i] = (void *)rt_entry;
    DIR *d = opendir(dir);
    if (!d) { printf("FAIL cannot open %s\n", dir); return 1; }
    char **names = NULL;
    int nn = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l <= 3 || strcmp(de->d_name + l - 3, ".ir")) continue;
        if (nn == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, (size_t)cap * sizeof *names); }
        names[nn] = strndup(de->d_name, l - 3);
        nn++;
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof *names, by_name);
    long vec = 0, ntraps = 0;
    int ops = 0, gates = 0;
    for (int i = 0; i < nn; i++) {
        Stat st;
        if (compare(dir, names[i], names[i], &st)) { fails++; free(names[i]); continue; }
        vec += st.vectors;
        ntraps += st.native_traps;
        if (st.vectors <= 0) {
            /* omega#454: a program that ran no vectors proves nothing; it must not count as agreeing */
            printf("FAIL %s: no vectors were run\n", names[i]);
            fails++;
        } else if (st.mismatches || st.undetermined) {
            printf("FAIL %s: %ld of %ld vectors disagree, %ld undetermined; first: %s\n", names[i], st.mismatches, st.vectors,
                   st.undetermined, st.first);
            fails++;
        } else if (strncmp(names[i], "op_", 3) == 0) ops++;
        else gates++;
        free(names[i]);
    }
    free(names);
    /* negative controls: the CNF of one operation against the code of another must disagree */
    char cp[1024];
    snprintf(cp, sizeof cp, "%s/controls", dir);
    FILE *cf = fopen(cp, "r");
    int controls = 0;
    char a[256], b[256];
    while (cf && fscanf(cf, "%255s %255s", a, b) == 2) {
        Stat st;
        if (compare(dir, a, b, &st) == 0 && st.mismatches > 0) controls++;
        else { printf("FAIL control %s vs code of %s: no disagreement found\n", a, b); fails++; }
    }
    if (cf) fclose(cf);
    if (ops < 180 || gates < 29 || controls < 8) { printf("FAIL too little coverage: %d operations, %d gate programs, %d controls\n", ops, gates, controls); fails++; }
    printf("test_c2_blast_native: %d operation programs and %d gate programs agree with oscc's code on %ld vectors "
           "(%ld native traps); %d controls disagree as required; %d failures\n", ops, gates, vec, ntraps, controls, fails);
    return fails ? 1 : 0;
}
#endif
