/*
 * test_osc_alloc.c -- OSC explicit-allocation gate (src/compiler/osc_alloc.h,
 * docs/osc/OSC-1-DESIGN.md "Explicit allocation").
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 *
 * For every golden program (tests/compiler/progs/NAME.osc) plus one generated unit big enough to
 * grow the back end's word and fixup buffers (the only realloc sites), compile + emit through
 * osc_compile_a / osc_cg_compile_a and check:
 *  1. metered run over libc: succeeds, live == 0 after osc_cg_free (no leak), no wrong-size
 *     free or realloc (bad == 0), nothing refused; N = counted allocation calls, P = peak bytes.
 *  2. determinism: the IR (osc_ir_encode) and code bytes are identical to the unwrapped libc entry
 *     points, to osc_alloc_libc() passed explicitly, to a NULL allocator, and to an allocator that
 *     fills new blocks with 0xA5 and freed blocks with 0xDD (so nothing may rely on zeroed or
 *     stale memory; the checker and lowerer used to calloc).
 *  3. failure sweep: for every k in 1..N the k-th allocation call is refused. Compile + emit must
 *     fail cleanly (front end: OSC_DIAG_CAPACITY; validator / back end: -1 with "out of memory"
 *     in err), exactly one refusal, live == 0 afterwards (no leak on any failure path), bad == 0.
 *  4. budget: a budget of P-1 bytes fails cleanly, a budget of P succeeds with the same bytes and
 *     a peak of exactly P; the same pair for the back end alone (peak Pe, unit already built).
 * Also: osc_ir_validate_a swept the same way (-2 + "out of memory"), and osc_cg_free on a zeroed
 * OscCode (a == NULL) releases a libc-compiled unit.
 *
 * Usage: test_osc_alloc [tests dir (default tests/compiler)]
 * Final line: `PASS <programs> <fail-points>`, or `FAIL ...` and exit 1.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_alloc.h"
#include "osc_cg.h"
#include "osc_front.h"
#include "osc_ir.h"

static unsigned long checks, failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            if (failures <= 40) {                           \
                printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                printf(__VA_ARGS__);                        \
                printf("\n");                               \
            }                                               \
        }                                                   \
    } while (0)

/* ------------------------------------------------------------ files */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap + 1);
    while (b) {
        n += fread(b + n, 1, cap - n, f);
        if (n < cap) break;
        cap *= 2;
        char *nb = realloc(b, cap + 1);
        if (!nb) { free(b); b = NULL; break; }
        b = nb;
    }
    fclose(f);
    if (b) { b[n] = 0; *len = n; }
    return b;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_osc(const char *dir, char ***out)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0, cap = 64;
    char **v = malloc(sizeof(char *) * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".osc")) continue;
        if (n == cap) { cap *= 2; v = realloc(v, sizeof(char *) * (size_t)cap); }
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof(char *), cmpstr);
    *out = v;
    return n;
}

/* Twelve functions of 200 `s = s + b;` statements: about 17000 code words (the word buffer starts
 * at 4096 and doubles) and 2400 trap fixups (starts at 256), so the growing-realloc paths of the
 * back end run. Under the 480-vreg and 16384-node limits of one function / one unit. */
static char *gen_big(size_t *len)
{
    size_t cap = 12 * 200 * 16 + 4096, n = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
    for (int f = 0; f < 12; f++) {
        n += (size_t)snprintf(b + n, cap - n, "fn big%d(a: u64, b: u64) -> u64 {\n    let mut s: u64 = a;\n", f);
        for (int i = 0; i < 200; i++) n += (size_t)snprintf(b + n, cap - n, "    s = s + b;\n");
        n += (size_t)snprintf(b + n, cap - n, "    return s;\n}\n");
    }
    *len = n;
    return b;
}

/* ------------------------------------------------------------ allocators */
/* fills what it hands out with 0xA5 and what it takes back with 0xDD */
static void *scr_alloc(void *ctx, size_t n)
{
    (void)ctx;
    void *p = malloc(n ? n : 1);
    if (p) memset(p, 0xA5, n);
    return p;
}
static void *scr_realloc(void *ctx, void *p, size_t old_n, size_t new_n)
{
    (void)ctx;
    void *q = realloc(p, new_n ? new_n : 1);
    if (q && new_n > old_n) memset((uint8_t *)q + old_n, 0xA5, new_n - old_n);
    return q;
}
static void scr_free(void *ctx, void *p, size_t n)
{
    (void)ctx;
    if (p) memset(p, 0xDD, n);
    free(p);
}
static const OscAlloc k_scribble = {scr_alloc, scr_realloc, scr_free, NULL};

/* ------------------------------------------------------------ one compile + emit */
typedef struct {
    int stage;        /* 0 ok, 1 front end refused, 2 validator / back end refused */
    OscDiag d;
    char err[160];
} Res;

typedef struct {
    uint8_t *ir, *code;
    size_t irn, coden;
} Out;

static int run(const OscAlloc *a, const char *src, size_t len, OscUnit *u, OscCode *c, Res *r)
{
    memset(r, 0, sizeof *r);
    memset(c, 0, sizeof *c);
    if (osc_compile_a(a, src, len, u, &r->d, NULL)) { r->stage = 1; return -1; }
    if (osc_cg_compile_a(a, u, c, r->err, sizeof r->err)) { r->stage = 2; return -1; }
    return 0;
}

static int snapshot(const OscUnit *u, const OscCode *c, Out *o)
{
    memset(o, 0, sizeof *o);
    if (osc_ir_encode(u, NULL, 0, &o->irn)) return -1;
    o->ir = malloc(o->irn);
    o->code = malloc(c->len ? c->len : 1);
    if (!o->ir || !o->code || osc_ir_encode(u, o->ir, o->irn, &o->irn)) return -1;
    memcpy(o->code, c->code, c->len);
    o->coden = c->len;
    return 0;
}

static void out_free(Out *o) { free(o->ir); free(o->code); memset(o, 0, sizeof *o); }

static int same(const Out *a, const Out *b)
{
    return a->irn == b->irn && a->coden == b->coden && !memcmp(a->ir, b->ir, a->irn) && !memcmp(a->code, b->code, a->coden);
}

/* A refusal is reported the documented way: front end CAPACITY, validator / back end "out of memory". */
static int clean_oom(const Res *r)
{
    if (r->stage == 1) return r->d.kind == OSC_DIAG_CAPACITY;
    if (r->stage == 2) return strstr(r->err, "out of memory") != NULL;
    return 0;
}

static unsigned long n_programs, n_calls, n_failpoints, n_front, n_emit, n_pairs, n_validate_points;
static size_t peak_max, emit_peak_max;

static void program(const char *label, const char *src, size_t len, OscUnit *u)
{
    OscCode c;
    Res r;
    Out ref, o;
    unsigned long f0 = failures;

    /* reference bytes: the unwrapped libc entry points (what every existing caller uses) */
    OscDiag d;
    char err[160];
    int rc = osc_compile(src, len, u, &d, NULL) || osc_cg_compile(u, &c, err, sizeof err);
    CHECK(rc == 0, "%s: reference compile failed (%s)", label, d.message);
    if (rc) return;
    CHECK(snapshot(u, &c, &ref) == 0, "%s: snapshot", label);
    osc_cg_free(&c);

    /* 1. metered run over libc */
    OscAllocMeter m;
    osc_alloc_meter_init(&m, NULL, 0, 0);
    OscAlloc ma = osc_alloc_meter(&m);
    rc = run(&ma, src, len, u, &c, &r);
    CHECK(rc == 0, "%s: metered compile failed: %s%s", label, r.d.message, r.err);
    CHECK(rc != 0 || (snapshot(u, &c, &o) == 0 && same(&o, &ref)), "%s: metered output differs from libc", label);
    if (rc == 0) out_free(&o);
    CHECK(c.a == &ma || rc, "%s: OscCode.a not set to the allocator", label);
    osc_cg_free(&c);
    CHECK(m.live == 0, "%s: leak: live=%zu after free", label, m.live);
    CHECK(m.bad == 0, "%s: %zu wrong-size free/realloc", label, m.bad);
    CHECK(m.refused == 0, "%s: %zu refusals with no limit set", label, m.refused);
    size_t N = m.calls, P = m.peak;
    CHECK(N >= 6 && P > 0, "%s: implausible meter N=%zu P=%zu", label, N, P);
    if (rc || failures != f0) { out_free(&ref); return; }

    /* 2. determinism across allocators: libc passed explicitly, NULL (= libc), and the scribbler under a meter */
    OscAllocMeter sm;
    osc_alloc_meter_init(&sm, &k_scribble, 0, 0);
    OscAlloc sa = osc_alloc_meter(&sm);
    const OscAlloc *variants[3] = {osc_alloc_libc(), NULL, &sa};
    const char *vname[3] = {"libc", "NULL", "scribble"};
    for (int v = 0; v < 3; v++) {
        rc = run(variants[v], src, len, u, &c, &r);
        CHECK(rc == 0, "%s: %s allocator: compile failed", label, vname[v]);
        if (rc == 0) {
            CHECK(snapshot(u, &c, &o) == 0 && same(&o, &ref), "%s: %s allocator: output differs", label, vname[v]);
            out_free(&o);
        }
        osc_cg_free(&c);
    }
    CHECK(sm.live == 0 && sm.bad == 0 && sm.calls == N, "%s: scribble run: live=%zu bad=%zu calls=%zu (want %zu)", label,
          sm.live, sm.bad, sm.calls, N);
    /* a zeroed OscCode (a == NULL) frees a libc-compiled unit */
    rc = run(osc_alloc_libc(), src, len, u, &c, &r);
    if (rc == 0) { c.a = NULL; osc_cg_free(&c); CHECK(c.code == NULL && c.len == 0, "%s: osc_cg_free left the code", label); }

    /* 3. failure sweep: the k-th allocation call is refused */
    for (size_t k = 1; k <= N; k++) {
        OscAllocMeter fm;
        osc_alloc_meter_init(&fm, NULL, 0, k);
        OscAlloc fa = osc_alloc_meter(&fm);
        rc = run(&fa, src, len, u, &c, &r);
        osc_cg_free(&c);
        CHECK(rc != 0, "%s: fail_at=%zu: compile + emit succeeded", label, k);
        CHECK(rc == 0 || clean_oom(&r), "%s: fail_at=%zu: stage %d refusal is not a clean OOM (kind %d, err '%s')", label, k,
              r.stage, (int)r.d.kind, r.err);
        CHECK(fm.refused == 1, "%s: fail_at=%zu: %zu refusals", label, k, fm.refused);
        CHECK(fm.live == 0, "%s: fail_at=%zu: leak: live=%zu", label, k, fm.live);
        CHECK(fm.bad == 0, "%s: fail_at=%zu: %zu wrong-size free/realloc", label, k, fm.bad);
        n_failpoints++;
        if (r.stage == 1) n_front++;
        if (r.stage == 2) n_emit++;
    }

    /* 4. budget: P-1 fails, P succeeds with the same bytes and a peak of exactly P */
    OscAllocMeter bm;
    osc_alloc_meter_init(&bm, NULL, P - 1, 0);
    OscAlloc ba = osc_alloc_meter(&bm);
    rc = run(&ba, src, len, u, &c, &r);
    osc_cg_free(&c);
    CHECK(rc != 0 && clean_oom(&r), "%s: budget %zu (P-1) did not fail cleanly", label, P - 1);
    CHECK(bm.live == 0 && bm.bad == 0 && bm.peak <= P - 1, "%s: budget P-1: live=%zu bad=%zu peak=%zu", label, bm.live, bm.bad, bm.peak);
    osc_alloc_meter_init(&bm, NULL, P, 0);
    ba = osc_alloc_meter(&bm);
    rc = run(&ba, src, len, u, &c, &r);
    CHECK(rc == 0, "%s: budget %zu (P) failed: %s%s", label, P, r.d.message, r.err);
    if (rc == 0) {
        CHECK(snapshot(u, &c, &o) == 0 && same(&o, &ref), "%s: budget P: output differs", label);
        out_free(&o);
    }
    osc_cg_free(&c);
    CHECK(bm.live == 0 && bm.bad == 0 && bm.peak == P && bm.refused == 0, "%s: budget P: live=%zu bad=%zu peak=%zu (want %zu) refused=%zu", label,
          bm.live, bm.bad, bm.peak, P, bm.refused);
    n_pairs++;

    /* 4b. the back end alone (the unit already exists): its own peak Pe, Pe-1 fails, Pe succeeds with the same code */
    OscAllocMeter em;
    osc_alloc_meter_init(&em, NULL, 0, 0);
    OscAlloc ea = osc_alloc_meter(&em);
    rc = osc_cg_compile_a(&ea, u, &c, err, sizeof err);
    CHECK(rc == 0, "%s: emit-only compile failed: %s", label, err);
    osc_cg_free(&c);
    size_t Pe = em.peak;
    CHECK(em.live == 0 && Pe > 0, "%s: emit-only: live=%zu peak=%zu", label, em.live, Pe);
    osc_alloc_meter_init(&em, NULL, Pe - 1, 0);
    ea = osc_alloc_meter(&em);
    rc = osc_cg_compile_a(&ea, u, &c, err, sizeof err);
    osc_cg_free(&c);
    CHECK(rc != 0 && strstr(err, "out of memory") && em.live == 0 && em.bad == 0, "%s: emit budget %zu (Pe-1): rc=%d err='%s' live=%zu", label,
          Pe - 1, rc, err, em.live);
    osc_alloc_meter_init(&em, NULL, Pe, 0);
    ea = osc_alloc_meter(&em);
    rc = osc_cg_compile_a(&ea, u, &c, err, sizeof err);
    CHECK(rc == 0 && c.len == ref.coden && !memcmp(c.code, ref.code, c.len), "%s: emit budget %zu (Pe): rc=%d err='%s'", label, Pe, rc, err);
    osc_cg_free(&c);
    CHECK(em.live == 0 && em.bad == 0 && em.peak == Pe, "%s: emit budget Pe: live=%zu peak=%zu (want %zu)", label, em.live, em.peak, Pe);
    if (Pe > emit_peak_max) emit_peak_max = Pe;

    /* osc_ir_validate_a swept on its own: -2 and "out of memory", no leak */
    OscAllocMeter vm;
    osc_alloc_meter_init(&vm, NULL, 0, 0);
    OscAlloc va = osc_alloc_meter(&vm);
    CHECK(osc_ir_validate_a(&va, u, err, sizeof err) == 0 && vm.live == 0, "%s: validate_a over the meter", label);
    size_t vn = vm.calls;
    for (size_t k = 1; k <= vn; k++) {
        osc_alloc_meter_init(&vm, NULL, 0, k);
        va = osc_alloc_meter(&vm);
        int vr = osc_ir_validate_a(&va, u, err, sizeof err);
        CHECK(vr == -2 && strstr(err, "out of memory") && vm.live == 0 && vm.bad == 0 && vm.refused == 1,
              "%s: validate_a fail_at=%zu: rc=%d err='%s' live=%zu", label, k, vr, err, vm.live);
        n_validate_points++;
    }

    n_programs++;
    n_calls += N;
    if (P > peak_max) peak_max = P;
    printf("%-28s calls=%-4zu peak=%-8zu fail-points=%zu\n", label, N, P, N);
    out_free(&ref);
}

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "tests/compiler";
    char pdir[512];
    snprintf(pdir, sizeof pdir, "%s/progs", root);
    OscUnit *u = malloc(sizeof *u);
    if (!u) { printf("FAIL out of memory\n"); return 1; }

    char **pv = NULL;
    int np = list_osc(pdir, &pv);
    if (np < 0) { printf("FAIL cannot list %s\n", pdir); free(u); return 1; }
    CHECK(np >= 20, "need >= 20 golden programs, found %d", np);
    for (int i = 0; i < np; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", pdir, pv[i]);
        size_t len = 0;
        char *src = slurp(path, &len);
        CHECK(src != NULL, "cannot read %s", path);
        if (!src) continue;
        program(pv[i], src, len, u);
        free(src);
    }
    size_t blen = 0;
    char *big = gen_big(&blen);
    CHECK(big != NULL, "cannot build the generated unit");
    if (big) {
        program("(generated: 12 x 200 adds)", big, blen, u);
        free(big);
    }
    free(u);
    for (int i = 0; i < np; i++) free(pv[i]);
    free(pv);

    printf("alloc sweep: programs=%lu (golden files %d + 1 generated) calls=%lu peak_max=%zu emit_peak_max=%zu fail_points=%lu (front=%lu emit=%lu) "
           "budget_pairs=%lu validate_points=%lu checks=%lu failed=%lu\n",
           n_programs, np, n_calls, peak_max, emit_peak_max, n_failpoints, n_front, n_emit, n_pairs, n_validate_points, checks, failures);
    if (failures || n_programs != (unsigned long)np + 1 || n_failpoints != n_calls) {
        printf("FAIL %lu checks failed (programs run %lu of %d)\n", failures, n_programs, np + 1);
        return 1;
    }
    printf("PASS %lu %lu\n", n_programs, n_failpoints);
    return 0;
}
