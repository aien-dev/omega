/* PD-0 Omega-language differential test: compile tests/physics0/osc/
 * pd0_helpers.osc with the OSC-3 front end, run every helper through the
 * reference interpreter AND the native AArch64 path, and compare both with the
 * C implementation (pd0_rng.c, pd0_gen.c via the public relation of each
 * level, pd0_relation_description_bits). Same pattern as
 * tests/compiler/test_osc_compiler.c (golden entries: interp vs native).
 * Prints PHYSICS0_OSC_DIFF: PASS|FAIL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osc_cg.h"
#include "osc_front.h"
#include "osc_interp.h"
#include "osc_native.h"
#include "osc_rt.h"
#include "physics0/pd0_relation.h"
#include "physics0/pd0_rng.h"

static unsigned long failures, runs;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f);
    s[n] = 0;
    *len = (size_t)n;
    return s;
}

static OscUnit *U;
static OscCode C;
static OscNative NM;
static OscRt *RI, *RN;
static int native_ok;

static int func_index(const char *name) {
    for (int k = 0; k < U->nfuncs; k++)
        if (strcmp(U->funcs[k].name, name) == 0) return k;
    return -1;
}

/* run one helper both ways; returns 0 and sets *out if both agree and neither trapped */
static int run(const char *name, const uint64_t *args, unsigned nargs, uint64_t *out) {
    int fi = func_index(name);
    if (fi < 0) { CHECK(0, "%s: not found", name); return -1; }
    uint64_t ri = 0, rn = 0;
    osc_rt_reset(RI);
    int ti = osc_interp_run(U, fi, args, nargs, RI, &ri);
    runs++;
    if (native_ok) {
        osc_rt_reset(RN);
        int tn = osc_rt_call_native(RN, osc_native_at(&NM, C.entry[fi]), args, nargs, &rn);
        CHECK(ti == tn && (ti != 0 || ri == rn), "%s: interp trap %d ret %lld vs native trap %d ret %lld", name, ti,
              (long long)ri, tn, (long long)rn);
    }
    if (ti != 0) { CHECK(0, "%s: trap %d", name, ti); return -1; }
    *out = ri;
    return 0;
}

static int64_t rnd_box(pd0_rng *r, int64_t box) { return pd0_const(r, -box, box); }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "tests/physics0/osc/pd0_helpers.osc";
    unsigned iters = argc > 2 ? (unsigned)atoi(argv[2]) : 20000;
    size_t len;
    char *src = read_file(path, &len);
    if (!src) { printf("cannot read %s\nPHYSICS0_OSC_DIFF: FAIL\n", path); return 1; }
    U = calloc(1, sizeof *U);
    RI = calloc(1, sizeof *RI);
    RN = calloc(1, sizeof *RN);
    osc_rt_init(RI);
    osc_rt_init(RN);
    OscDiag d;
    int rc = osc_compile(src, len, U, &d, NULL);
    if (rc != 0) {
        printf("refused: %s line %u object=%s: %s\nPHYSICS0_OSC_DIFF: FAIL\n", osc_diag_kind_name(d.kind), d.line, d.object, d.message);
        return 1;
    }
    char err[160];
    memset(&C, 0, sizeof C);
    CHECK(osc_cg_compile(U, &C, err, sizeof err) == 0, "codegen refused: %s", err);
    int mr = failures ? -1 : osc_native_map(&NM, C.code, C.len);
    native_ok = mr == 0;
    if (mr == -2) printf("note: not an AArch64 host, native leg skipped\n");
    else CHECK(mr == 0, "native map failed (%d)", mr);

    pd0_rng r;
    pd0_stream(&r, 0x9D0u, "osc-diff");
    unsigned long mism = 0;
    const int64_t BOX = PD0_BOUND, KMAX = 9000000, DT = 50000;
    for (unsigned it = 0; it < iters; it++) {
        uint64_t a[6], o;
        int64_t s0 = rnd_box(&r, BOX), s1 = rnd_box(&r, BOX), u = rnd_box(&r, 2000000), k = pd0_const(&r, 0, KMAX),
                c = pd0_const(&r, 0, 2000000);
        /* mul: both operands inside the world box */
        a[0] = (uint64_t)s0; a[1] = (uint64_t)s1;
        if (run("mul", a, 2, &o) == 0) mism += (int64_t)o != pd0_mul(s0, s1);
        /* const_draw */
        int64_t lo = rnd_box(&r, 5000000), hi = lo + pd0_const(&r, 0, 9000000), uu = pd0_uniform(&r);
        a[0] = (uint64_t)lo; a[1] = (uint64_t)hi; a[2] = (uint64_t)uu;
        if (run("const_draw", a, 3, &o) == 0) mism += (int64_t)o != lo + ((hi - lo) * uu) / PD0_MICRO;
        /* noise_map against pd0_noise: both consume the same four uniforms from a copy of r */
        int64_t sigma = pd0_const(&r, 0, 500000);
        pd0_rng feed = r, feed2 = r;
        int64_t want = pd0_noise(&feed, sigma);
        int64_t u1 = pd0_uniform(&feed2), u2 = pd0_uniform(&feed2), u3 = pd0_uniform(&feed2), u4 = pd0_uniform(&feed2);
        r = feed; /* advance past the four draws */
        a[0] = (uint64_t)u1; a[1] = (uint64_t)u2; a[2] = (uint64_t)u3; a[3] = (uint64_t)u4; a[4] = (uint64_t)sigma;
        if (run("noise_map", a, 5, &o) == 0) mism += (int64_t)o != want;
        /* L0, L1, L2 ticks against the C relation of each level (true coefficients) */
        a[0] = (uint64_t)s0; a[1] = (uint64_t)s1;
        if (run("l0_s0", a, 2, &o) == 0) mism += (int64_t)o != s0 + s1;
        a[0] = (uint64_t)s1; a[1] = (uint64_t)u;
        if (run("l0_s1", a, 2, &o) == 0) mism += (int64_t)o != s1 + u;
        a[0] = (uint64_t)s0; a[1] = (uint64_t)s1; a[2] = (uint64_t)DT;
        if (run("l1_s0", a, 3, &o) == 0) mism += (int64_t)o != s0 + pd0_mul(s1, DT);
        a[0] = (uint64_t)s0; a[1] = (uint64_t)s1; a[2] = (uint64_t)u; a[3] = (uint64_t)k; a[4] = (uint64_t)DT;
        if (run("l1_s1", a, 5, &o) == 0) mism += (int64_t)o != s1 + pd0_mul(-pd0_mul(k, s0) + u, DT);
        a[5] = (uint64_t)DT; a[4] = (uint64_t)c;
        if (run("l2_s1", a, 6, &o) == 0) mism += (int64_t)o != s1 + pd0_mul(-pd0_mul(k, s0) - pd0_mul(c, s1) + u, DT);
        a[0] = (uint64_t)s0;
        if (run("cube", a, 1, &o) == 0) mism += (int64_t)o != pd0_mul(pd0_mul(s0, s0), s0);
        /* description_bits against pd0_relation_description_bits on a random relation shape */
        pd0_relation rel; memset(&rel, 0, sizeof rel);
        rel.n_vars = (uint8_t)(1 + pd0_next(&r) % 4); rel.n_latent = (uint8_t)(pd0_next(&r) % 2);
        if (rel.n_latent >= rel.n_vars) rel.n_latent = 0;
        rel.n_channels = (uint8_t)(1 + pd0_next(&r) % 2); rel.n_eq = 1;
        rel.eq[0].n_terms = (uint8_t)(pd0_next(&r) % 6);
        for (int t = 0; t < rel.eq[0].n_terms; t++) rel.eq[0].t[t].coef = 1;
        a[0] = rel.n_vars; a[1] = rel.n_channels; a[2] = rel.eq[0].n_terms; a[3] = rel.n_latent;
        if (run("description_bits", a, 4, &o) == 0) mism += (uint32_t)o != pd0_relation_description_bits(&rel);
    }
    CHECK(mism == 0, "%lu mismatches against C", mism);
    printf("osc differential: runs=%lu mismatches=%lu native=%s\n", runs, mism, native_ok ? "yes" : "skipped");
    if (native_ok) osc_native_unmap(&NM);
    osc_cg_free(&C);
    free(src);
    printf("PHYSICS0_OSC_DIFF: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
