/* Component tests for the AT-0 candidate engine.
 *   at0-tests <path-to-tests/cases>
 * Every test is a plain function returning 0 on pass; failures print the
 * test name and the offending value. No randomness, no wall clock. */
#include "at0_model.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); return 1; } } while (0)
#define OK(call) CHECK((call) == AT0_OK, "%s returned %s", #call, at0_status_name(call))

static const char *cases_dir;
static at0_case kat;                 /* the AT0_CASE_V1 section 6 example, parsed once */
static uint8_t kat_bytes[8192]; static size_t kat_len;

static int load_file(const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", cases_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 1;
}

/* build a case from a struct: compute ids, emit, parse back through the full validator */
static at0_status finalize(at0_case *c)
{
    at0_status st = at0_case_identities(c, c->case_id, c->acceptance_id);
    if (st != AT0_OK) return st;
    static char buf[65536];
    long n = at0_case_emit(c, buf, sizeof buf);
    if (n < 0) return AT0_ERR_INTERNAL;
    return at0_case_parse((const uint8_t *)buf, (size_t)n, c);
}

static at0_rat R(int64_t n, int64_t d) { at0_rat r; at0_status st = at0_rat_make(n, d, &r); if (st != AT0_OK) { r.n = 0; r.d = 1; } return r; }
static at0_scaled S(uint64_t n, int k) { at0_scaled s = { n, k }; return s; }

/* a two-level-clock case in the draft AT0_SPEC mapping: omega = 1, N = 2, M labels, w = 2/M */
static void spec_two_level(at0_case *c, int m, int ref)
{
    memset(c, 0, sizeof *c);
    strcpy(c->name, "spec-two-level");
    c->clock_dim = 2; c->clock_energies[0] = R(-1, 1); c->clock_energies[1] = R(0, 1);
    c->h0 = R(1, 2); c->hx = R(0, 1); c->hy = R(0, 1); c->hz = R(-1, 2);
    c->psi0[0].re = R(1, 1); c->psi0[0].im = R(0, 1); c->psi0[1].re = R(1, 1); c->psi0[1].im = R(0, 1);
    c->povm_tau_turns = R(1, m); c->povm_weight = R(2, m);
    c->label_count = m;
    for (int k = 0; k < m; k++) snprintf(c->labels[k], sizeof c->labels[k], "t%d", k);
    strcpy(c->reference_clock_label, c->labels[ref]);
    c->control_kind = AT0_CTRL_POSITIVE; c->expected_outcome = AT0_EXPECT_PASS;
    c->min_bound_kind = AT0_BOUND_ESTIMATED;
    c->tol_constraint_residual = S(1, 12); c->tol_povm_residual = S(1, 12); c->tol_probability = S(1, 12);
    c->tol_zero_probability = S(1, 9); c->tol_schrodinger = S(1, 12);
}

/* test-side Schrodinger reference (not the AT-0 oracle): U = exp(-i H_S t) via the 2x2 identity
 * exp(-i (h0 I + h.sigma) t) = e^{-i h0 t} (cos(|h| t) I - i sin(|h| t) (h.sigma)/|h|) */
static void schrodinger_probs(const at0_case *c, double t, double out[3][2])
{
    double h0 = at0_rat_to_double(c->h0), hx = at0_rat_to_double(c->hx), hy = at0_rat_to_double(c->hy), hz = at0_rat_to_double(c->hz);
    double r = at0_rat_to_double(c->h_norm);
    double complex u[2][2];
    double complex g = cexp(-I * h0 * t);
    if (r == 0.0) { u[0][0] = g; u[0][1] = 0; u[1][0] = 0; u[1][1] = g; }
    else {
        double cs = cos(r * t), sn = sin(r * t);
        u[0][0] = g * (cs - I * sn * hz / r);        u[0][1] = g * (-I * sn * (hx - I * hy) / r);
        u[1][0] = g * (-I * sn * (hx + I * hy) / r); u[1][1] = g * (cs + I * sn * hz / r);
    }
    double complex p0 = at0_rat_to_double(c->psi0[0].re) + I * at0_rat_to_double(c->psi0[0].im);
    double complex p1 = at0_rat_to_double(c->psi0[1].re) + I * at0_rat_to_double(c->psi0[1].im);
    double complex v0 = u[0][0] * p0 + u[0][1] * p1, v1 = u[1][0] * p0 + u[1][1] * p1;
    double n2 = cabs(v0) * cabs(v0) + cabs(v1) * cabs(v1);
    double complex eig[3][2][2] = {
        { { 1 / sqrt(2.0), 1 / sqrt(2.0) }, { 1 / sqrt(2.0), -1 / sqrt(2.0) } },           /* X+, X- */
        { { 1 / sqrt(2.0), I / sqrt(2.0) }, { 1 / sqrt(2.0), -I / sqrt(2.0) } },           /* Y+, Y- */
        { { 1, 0 }, { 0, 1 } } };                                                           /* Z+, Z- */
    for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) {
        double complex ov = conj(eig[a][s][0]) * v0 + conj(eig[a][s][1]) * v1;
        out[a][s] = cabs(ov) * cabs(ov) / n2;
    }
}

static double scaled_to_double(at0_scaled s) { return (double)s.n * pow(10.0, -s.k); }

/* ---- tests ---------------------------------------------------------------- */
static int test_kat_roundtrip(void)
{
    static char buf[8192];
    long n = at0_case_emit(&kat, buf, sizeof buf);
    CHECK(n == (long)kat_len && memcmp(buf, kat_bytes, kat_len) == 0, "re-emitted KAT differs from file");
    CHECK(strcmp(kat.case_id, "3cf4ca4f882b5b9691ddcd905e65e15010855adcd2be200e19ebc3184655b44d") == 0, "case_id");
    CHECK(strcmp(kat.acceptance_id, "a13fb02dd674b8042ef8c0a197f03d58768709f782ba59cd26545eef6d41a228") == 0, "acceptance_id");
    CHECK(strcmp(kat.case_file_sha256, "ed16c95c89bf312b0fbf95a4cd43bc352daa138954fabad78d8a170cd35b4e81") == 0, "case_file_sha256");
    char cid[65], aid[65];
    OK(at0_case_identities(&kat, cid, aid));
    CHECK(strcmp(cid, kat.case_id) == 0 && strcmp(aid, kat.acceptance_id) == 0, "recomputed identities differ");
    CHECK(kat.h_norm.n == 1 && kat.h_norm.d == 2, "|h| = %lld/%lld", (long long)kat.h_norm.n, (long long)kat.h_norm.d);
    return 0;
}

static int test_exact_arithmetic(void)
{
    at0_rat a, b, r; int sq;
    OK(at0_rat_make(6, -4, &a)); CHECK(a.n == -3 && a.d == 2, "reduce");
    OK(at0_rat_make(1, 3, &b)); OK(at0_rat_add(a, b, &r)); CHECK(r.n == -7 && r.d == 6, "add");
    OK(at0_rat_sqrt_exact(R(9, 4), &r, &sq)); CHECK(sq && r.n == 3 && r.d == 2, "sqrt 9/4");
    OK(at0_rat_sqrt_exact(R(2, 1), &r, &sq)); CHECK(!sq, "sqrt 2 must not be rational");
    OK(at0_rat_reduce_turn(R(7, 4), &r)); CHECK(r.n == -1 && r.d == 4, "7/4 mod turn -> -1/4, got %lld/%lld", (long long)r.n, (long long)r.d);
    OK(at0_rat_reduce_turn(R(-1, 2), &r)); CHECK(r.n == -1 && r.d == 2, "-1/2 stays -1/2");
    OK(at0_rat_reduce_turn(R(1, 2), &r)); CHECK(r.n == -1 && r.d == 2, "1/2 -> -1/2");
    OK(at0_rat_make(AT0_RATIONAL_LIMIT + 1, 1, &r)); CHECK(!at0_rat_in_limits(r), "contract limit is a rule-3 property, not an arithmetic one");
    CHECK(at0_rat_make(INT64_MAX, 1, &r) == AT0_ERR_OVERFLOW, "representability limit");
    OK(at0_rat_make(1000, 1, &r)); OK(at0_rat_mul(r, r, &r)); OK(at0_rat_mul(r, r, &r)); CHECK(r.n == 1000000000000LL, "wide product");
    CHECK(at0_rat_parse("2/4", &r) == AT0_CASE_NONCANONICAL, "2/4");
    CHECK(at0_rat_parse("-0/1", &r) == AT0_CASE_NONCANONICAL, "-0/1 is well formed but noncanonical (qualification D2)");
    CHECK(at0_rat_parse("04/1", &r) == AT0_CASE_NONCANONICAL, "leading zero numerator");
    CHECK(at0_rat_parse("1/04", &r) == AT0_CASE_NONCANONICAL, "leading zero denominator");
    CHECK(at0_rat_parse("04/x", &r) == AT0_CASE_PARSE_ERROR, "shape error wins over canonicality inside a token");
    CHECK(at0_rat_parse("0/1", &r) == AT0_OK && r.n == 0, "0/1 canonical");
    at0_crat z;
    CHECK(at0_crat_parse("(-0/1;1/1)", &z) == AT0_CASE_NONCANONICAL, "-0 inside a complex token");
    CHECK(at0_crat_parse("(-0/1;1/x)", &z) == AT0_CASE_PARSE_ERROR, "shape error in the other part wins");
    CHECK(at0_rat_parse("+1/1", &r) == AT0_CASE_PARSE_ERROR, "+1/1");
    CHECK(at0_rat_parse("1/0", &r) == AT0_CASE_NONCANONICAL, "1/0");
    at0_scaled s;
    CHECK(at0_scaled_parse("10@13", &s) == AT0_CASE_NONCANONICAL, "10@13");
    CHECK(at0_scaled_parse("0@3", &s) == AT0_CASE_NONCANONICAL, "0@3");
    CHECK(at0_scaled_parse("04@1", &s) == AT0_CASE_NONCANONICAL, "leading zero scaled");
    CHECK(at0_scaled_parse("1@04", &s) == AT0_CASE_NONCANONICAL, "leading zero exponent");
    CHECK(at0_scaled_parse("-0@0", &s) == AT0_CASE_NONCANONICAL, "-0 scaled");
    /* overflow-free |h| and |h| + hz for in-limit tokens with distinct denominators (qualification D1) */
    {
        at0_rat hn; int issq; at0_i128 A, D;
        OK(at0_rat_norm_exact(R(1, 349524), R(1, 262143), R(0, 1), &hn, &issq));   /* (3, 4) / 1048572 in reduced form */
        CHECK(issq && hn.n == 5 && hn.d == 1048572, "|h| = 5/1048572, got %lld/%lld", (long long)hn.n, (long long)hn.d);
        OK(at0_exact_norm_plus_hz(R(1, 349524), R(1, 262143), R(0, 1), hn, &A, &D));
        CHECK(D == (at0_i128)349524 * 262143 && A == (at0_i128)5 * 87381, "a = |h| + hz over D = dx dy dz: A=%lld D=%lld", (long long)A, (long long)D);
        OK(at0_rat_norm_exact(R(1, 1048573), R(1, 1048571), R(1, 1048569), &hn, &issq));
        CHECK(!issq, "three coprime denominators: irrational, decided without overflow");
        OK(at0_rat_norm_exact(R(2, 1048573), R(3, 1048573), R(6, 1048573), &hn, &issq));
        CHECK(issq && hn.n == 7 && hn.d == 1048573, "P5 norm 7/1048573");
        at0_i128 num[4] = { 1, -1, 0, 0 }, den[4] = { 1048573, 1048573, 1, 1 }; int zero;
        OK(at0_exact_sum_is_zero(4, num, den, &zero)); CHECK(zero, "1/d - 1/d == 0");
        num[1] = -1; den[1] = 1048571;
        OK(at0_exact_sum_is_zero(4, num, den, &zero)); CHECK(!zero, "1/d1 - 1/d2 != 0");
        CHECK(at0_rat_norm_exact(R(AT0_RATIONAL_LIMIT + 1, 1), R(0, 1), R(0, 1), &hn, &issq) == AT0_ERR_OVERFLOW, "out-of-limit input is refused, not wrapped");
    }
    OK(at0_scaled_parse("1@41", &s)); CHECK(!at0_scaled_in_limits(s), "1@41 parses, rule 3 refuses it");
    OK(at0_scaled_parse("25@2", &s)); CHECK(s.n == 25 && s.k == 2, "25@2");
    /* exact comparison: p = 0.25, b = 1e-15, tol = 1e-9 -> defined */
    int le, gt;
    OK(at0_exact_prob_status(0.25, S(1, 15), S(1, 9), &le, &gt)); CHECK(!le && gt, "0.25 defined");
    OK(at0_exact_prob_status(0.0, S(1, 15), S(1, 9), &le, &gt)); CHECK(le && !gt, "0 undefined");
    OK(at0_exact_prob_status(1e-9, S(0, 0), S(1, 9), &le, &gt)); CHECK(!le && gt, "binary64 nearest 1e-9 is above 10^-9, so exactly greater");
    OK(at0_exact_prob_status(5e-324, S(0, 0), S(0, 0), &le, &gt)); CHECK(!le && gt, "subnormal > 0 exactly");
    OK(at0_exact_prob_status(0.5, S(5, 1), S(0, 0), &le, &gt)); CHECK(!le && !gt, "p - b == tol boundary");
    return 0;
}

static int test_state_normalization(void)
{
    at0_system_hamiltonian hs; at0_kernel k; at0_state psi; double n2;
    OK(at0_hamiltonian_system(&kat, &hs));
    OK(at0_constraint_kernel(&kat, &hs, &k));
    OK(at0_constraint_physical_state(&kat, &hs, &k, &psi));
    OK(at0_state_norm2(&psi, &n2));
    CHECK(n2 > 0.0, "physical state is zero");
    OK(at0_state_scale(&psi, 1.0 / sqrt(n2)));
    OK(at0_state_norm2(&psi, &n2));
    CHECK(fabs(n2 - 1.0) < 4e-16, "normalized norm2 = %.17g", n2);
    for (int kk = 0; kk < kat.label_count; kk++) {
        at0_clock_vec t; double acc = 0;
        OK(at0_povm_clock_state(&kat, kk, &t));
        for (int j = 0; j < t.clock_dim; j++) acc += cabs(t.v[j]) * cabs(t.v[j]);
        CHECK(fabs(acc - 1.0) < 4e-16, "clock state %d norm2 = %.17g", kk, acc);
    }
    /* eigenvectors orthonormal for a tilted field */
    at0_case c = kat; c.hx = R(3, 10); c.hz = R(2, 5); c.h0 = R(1, 4);
    OK(finalize(&c));
    OK(at0_hamiltonian_system(&c, &hs));
    double complex ov = conj(hs.vec[0].v[0]) * hs.vec[1].v[0] + conj(hs.vec[0].v[1]) * hs.vec[1].v[1];
    CHECK(cabs(ov) < 4e-16, "eigenvectors not orthogonal: %.3g", cabs(ov));
    return 0;
}

static int test_constraint_satisfaction(void)
{
    at0_system_hamiltonian hs; at0_kernel k; at0_state psi; double r;
    OK(at0_hamiltonian_system(&kat, &hs));
    OK(at0_constraint_kernel(&kat, &hs, &k));
    CHECK(k.kernel_dim == 2, "kernel dim %d", k.kernel_dim);
    OK(at0_constraint_physical_state(&kat, &hs, &k, &psi));
    OK(at0_constraint_residual(&kat, &hs, &psi, &r));
    CHECK(r <= 1e-15, "residual %.3g", r);
    /* stationarity: a kernel vector is invariant under any evolution parameter; here the
     * generator annihilates it, so H_total applied twice is zero too */
    at0_state h; double hn;
    OK(at0_hamiltonian_apply_total(&kat, &hs, &psi, &h)); OK(at0_state_norm2(&h, &hn));
    CHECK(hn <= 1e-30, "H Psi norm2 %.3g", hn);
    /* negative control: a product state outside the kernel */
    at0_state bad; OK(at0_state_init(&bad, kat.clock_dim)); OK(at0_state_set(&bad, 0, 0, 1.0));
    OK(at0_constraint_residual(&kat, &hs, &bad, &r));
    CHECK(fabs(r - 1.0) < 1e-15, "|E_0>|0> residual should be |-3/2 + 1/2| = 1, got %.17g", r);
    /* uncovered spectrum: kernel empty, Psi zero */
    at0_case c = kat; for (int j = 0; j < 4; j++) c.clock_energies[j] = R(j + 1, 1);
    OK(finalize(&c)); OK(at0_hamiltonian_system(&c, &hs)); OK(at0_constraint_kernel(&c, &hs, &k));
    CHECK(k.kernel_dim == 0, "uncovered kernel dim %d", k.kernel_dim);
    OK(at0_constraint_physical_state(&c, &hs, &k, &psi)); double n2; OK(at0_state_norm2(&psi, &n2));
    CHECK(n2 == 0.0, "uncovered Psi must be zero");
    CHECK(at0_constraint_residual(&c, &hs, &psi, &r) == AT0_ERR_ARGUMENT, "residual of zero state must be refused");
    /* exact-zero overlap: psi0 orthogonal to the only matched eigenvector gives Psi = 0 exactly */
    c = kat; c.clock_energies[0] = R(-3, 2); c.clock_energies[1] = R(-1, 2); c.clock_energies[2] = R(3, 2); c.clock_energies[3] = R(5, 2);
    c.psi0[0].re = R(0, 1); c.psi0[1].re = R(1, 1);        /* only e=+1/2 (|0>) matched by E=-1/2; psi0 = |1> */
    OK(finalize(&c)); OK(at0_hamiltonian_system(&c, &hs)); OK(at0_constraint_kernel(&c, &hs, &k));
    CHECK(k.kernel_dim == 1, "half-covered kernel dim %d", k.kernel_dim);
    OK(at0_constraint_physical_state(&c, &hs, &k, &psi)); OK(at0_state_norm2(&psi, &n2));
    CHECK(n2 == 0.0, "orthogonal psi0 must give exactly zero Psi");
    /* degenerate system (|h| = 0): kernel holds both system components of the matched energy */
    c = kat; c.hz = R(0, 1); c.h0 = R(1, 2); OK(finalize(&c));
    OK(at0_hamiltonian_system(&c, &hs)); CHECK(hs.degenerate, "degenerate flag");
    OK(at0_constraint_kernel(&c, &hs, &k)); CHECK(k.kernel_dim == 2, "degenerate kernel dim %d", k.kernel_dim);
    return 0;
}

static int test_povm_normalization(void)
{
    double r;
    OK(at0_povm_residual(&kat, &r));
    CHECK(r <= 1e-15, "KAT povm residual %.3g", r);
    at0_case c = kat; c.povm_weight = R(1, 2); OK(finalize(&c));
    OK(at0_povm_residual(&c, &r));
    CHECK(fabs(r - 1.0) < 1e-14, "w=1/2 residual should be ||I/2||_F = 1, got %.17g", r);
    c = kat; c.label_count = 3; OK(finalize(&c));
    OK(at0_povm_residual(&c, &r));
    CHECK(r > 0.5, "dropped label residual %.3g", r);
    c = kat; c.povm_tau_turns = R(1, 3); OK(finalize(&c));     /* broken clock: D tau M not integer */
    OK(at0_povm_residual(&c, &r));
    CHECK(r > 0.1, "broken clock residual %.3g", r);
    /* N = 8, M = 8, tau = 1/4 with half-integer gaps: still a resolution of the identity */
    memset(&c, 0, sizeof c); c = kat; c.clock_dim = 8;
    for (int j = 0; j < 8; j++) c.clock_energies[j] = R(-3 + 2 * j, 4);
    c.label_count = 8; for (int k = 0; k < 8; k++) snprintf(c.labels[k], sizeof c.labels[k], "t%d", k);
    OK(finalize(&c)); OK(at0_povm_residual(&c, &r));
    CHECK(r <= 1e-14, "N=8 residual %.3g", r);
    return 0;
}

static int test_conditional_density(void)
{
    at0_system_hamiltonian hs; at0_kernel k; at0_state psi; double n2, psum = 0;
    OK(at0_hamiltonian_system(&kat, &hs)); OK(at0_constraint_kernel(&kat, &hs, &k));
    OK(at0_constraint_physical_state(&kat, &hs, &k, &psi)); OK(at0_state_norm2(&psi, &n2));
    for (int kk = 0; kk < kat.label_count; kk++) {
        at0_conditional cd;
        OK(at0_conditional_compute(&kat, &psi, n2, kk, &cd));
        CHECK(cd.defined_numeric, "label %d undefined", kk);
        double tr = creal(cd.rho.m[0][0] + cd.rho.m[1][1]);
        CHECK(fabs(tr - 1.0) < 4e-16, "trace %.17g", tr);
        CHECK(cabs(cd.rho.m[0][1] - conj(cd.rho.m[1][0])) < 4e-16, "not hermitian");
        /* purity: rho^2 == rho */
        for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
            double complex sq = cd.rho.m[a][0] * cd.rho.m[0][b] + cd.rho.m[a][1] * cd.rho.m[1][b];
            CHECK(cabs(sq - cd.rho.m[a][b]) < 8e-16, "rho^2 != rho at %d%d", a, b);
        }
        CHECK(fabs(cd.clock_probability - 0.25) < 4e-16, "p(%d) = %.17g", kk, cd.clock_probability);
        psum += cd.clock_probability;
    }
    CHECK(fabs(psum - 1.0) < 8e-16, "sum p = %.17g", psum);
    CHECK(at0_conditional_compute(&kat, &psi, n2, 4, (at0_conditional *)&hs) == AT0_ERR_ARGUMENT, "label out of range");
    return 0;
}

static int test_pauli_probabilities(void)
{
    /* KAT hand table (AT0_CASE_V1 section 6): P(X+), P(Y+), P(Z+) per label */
    static const double table[4][3] = { {1, .5, .5}, {.5, 1, .5}, {0, .5, .5}, {.5, 0, .5} };
    static at0_engine_result res; at0_engine_result *r = &res; memset(r, 0, sizeof *r);
    OK(at0_engine_run(&kat, r));
    for (int k = 0; k < 4; k++) {
        CHECK(r->label[k].status == AT0_LABEL_DEFINED, "label %d not DEFINED", k);
        for (int a = 0; a < 3; a++) {
            double plus = r->label[k].pauli[a][0], minus = r->label[k].pauli[a][1];
            CHECK(fabs(plus - table[k][a]) <= 1e-15, "k=%d axis=%d P+ = %.17g", k, a, plus);
            CHECK(fabs(plus + minus - 1.0) <= 4e-16, "pair sum k=%d axis=%d", k, a);
            CHECK(fabs(plus - table[k][a]) <= scaled_to_double(r->label[k].pauli_bound[a][0]), "deviation exceeds stated bound");
        }
    }
    /* draft-spec two-level mapping: P(Y+) = (1 - sin theta_k)/2 = 1/2, 0, 1/2, 1 for M = 4 */
    at0_case c; spec_two_level(&c, 4, 0); OK(finalize(&c));
    OK(at0_engine_run(&c, r));
    static const double yplus[4] = { .5, 0, .5, 1 };
    for (int k = 0; k < 4; k++) {
        CHECK(fabs(r->label[k].pauli[1][0] - yplus[k]) <= 1e-15, "spec Y+ k=%d = %.17g", k, r->label[k].pauli[1][0]);
        CHECK(fabs(r->label[k].clock_probability - 0.25) <= 4e-16, "spec p(k)");
    }
    CHECK(r->povm_residual <= 1e-15 && r->constraint_residual <= 1e-15, "spec residuals");
    /* eigenstate control (spec T4): psi0 = |0> is stationary, P(Z+) = 1, P(X+) = P(Y+) = 1/2 */
    c.psi0[1].re = R(0, 1); OK(finalize(&c)); OK(at0_engine_run(&c, r));
    for (int k = 0; k < 4; k++) {
        CHECK(fabs(r->label[k].pauli[2][0] - 1.0) <= 1e-15 && fabs(r->label[k].pauli[0][0] - .5) <= 1e-15 &&
              fabs(r->label[k].pauli[1][0] - .5) <= 1e-15, "eigenstate control k=%d", k);
    }
    return 0;
}

/* varying N, phase offset (reference label), omega (|h|) and tilt, against the test-side Schrodinger reference */
static int test_schrodinger_general(void)
{
    static at0_engine_result res; at0_engine_result *r = &res; memset(r, 0, sizeof *r);
    struct { int n, m, ref; at0_rat tau, w, h0, hx, hy, hz, e0, step; at0_crat p0, p1; } tc[] = {
        /* N=8 tilted field, h0=1/4, |h|=1/2, reference label 3 (phase offset), tau=1/4 */
        { 8, 8, 3, {1,4}, {1,1}, {1,4}, {3,10}, {0,1}, {2,5}, {-3,4}, {1,2}, {{1,1},{0,1}}, {{1,3},{-2,5}} },
        /* N=4 omega: |h|=1 (hz=1), energies -1,0,1,2 step 1, tau=1/8, M=8, w=1/2 */
        { 4, 8, 5, {1,8}, {1,2}, {0,1}, {0,1}, {0,1}, {1,1}, {-1,1}, {1,1}, {{2,1},{1,1}}, {{-1,1},{1,2}} },
        /* N=3 with hy, |h| = 5/13 from (3/13, 4/13, 0): energies -5/13, 0, 5/13 ; gaps 5/13 and 10/13 need tau M = 13/5: tau = 13/30, M = 6, w = 1/2 */
        { 3, 6, 0, {13,30}, {1,2}, {0,1}, {3,13}, {4,13}, {0,1}, {-5,13}, {5,13}, {{1,1},{0,1}}, {{0,1},{1,1}} },
        /* N=5 with M=7 labels, w = 5/7: not a POVM for these gaps, but conditional states are still Schrodinger */
        { 5, 7, 2, {1,7}, {5,7}, {1,2}, {0,1}, {0,1}, {1,2}, {-1,1}, {1,2}, {{1,1},{1,1}}, {{1,1},{-1,1}} },
    };
    for (size_t i = 0; i < sizeof tc / sizeof tc[0]; i++) {
        at0_case c; memset(&c, 0, sizeof c);
        snprintf(c.name, sizeof c.name, "general-%zu", i);
        c.clock_dim = tc[i].n;
        for (int j = 0; j < tc[i].n; j++) { at0_rat e; OK(at0_rat_mul(tc[i].step, R(j, 1), &e)); OK(at0_rat_add(tc[i].e0, e, &c.clock_energies[j])); }
        c.h0 = tc[i].h0; c.hx = tc[i].hx; c.hy = tc[i].hy; c.hz = tc[i].hz;
        c.psi0[0] = tc[i].p0; c.psi0[1] = tc[i].p1;
        c.povm_tau_turns = tc[i].tau; c.povm_weight = tc[i].w; c.label_count = tc[i].m;
        for (int k = 0; k < tc[i].m; k++) snprintf(c.labels[k], sizeof c.labels[k], "lab%d", k);
        strcpy(c.reference_clock_label, c.labels[tc[i].ref]);
        c.expected_outcome = AT0_EXPECT_PASS; c.min_bound_kind = AT0_BOUND_ESTIMATED;
        c.tol_constraint_residual = S(1, 12); c.tol_povm_residual = S(1, 12); c.tol_probability = S(1, 12);
        c.tol_zero_probability = S(1, 9); c.tol_schrodinger = S(1, 12);
        OK(finalize(&c));
        OK(at0_engine_run(&c, r));
        CHECK(r->kernel_dim == 2, "case %zu kernel dim %d", i, r->kernel_dim);
        CHECK(r->constraint_residual <= scaled_to_double(r->constraint_residual_bound), "case %zu residual %.3g", i, r->constraint_residual);
        double tau = at0_rat_to_double(c.povm_tau_turns);
        for (int k = 0; k < c.label_count; k++) {
            CHECK(r->label[k].status == AT0_LABEL_DEFINED, "case %zu label %d status", i, k);
            double ref[3][2];
            schrodinger_probs(&c, 2 * 3.141592653589793238462643383279 * (k - tc[i].ref) * tau, ref);
            for (int a = 0; a < 3; a++) for (int s = 0; s < 2; s++) {
                double dev = fabs(r->label[k].pauli[a][s] - ref[a][s]);
                CHECK(dev <= 2e-15, "case %zu k=%d axis=%d sign=%d engine %.17g ref %.17g", i, k, a, s, r->label[k].pauli[a][s], ref[a][s]);
                CHECK(dev <= scaled_to_double(r->label[k].pauli_bound[a][s]), "case %zu deviation above stated bound", i);
            }
            double expect_p = at0_rat_to_double(c.povm_weight) / c.clock_dim;
            CHECK(fabs(r->label[k].clock_probability - expect_p) <= 4e-16, "case %zu p(k) %.17g", i, r->label[k].clock_probability);
        }
        if (i == 3) CHECK(r->povm_residual > 0.1, "M=7 labels on N=5 must not resolve the identity");
        else CHECK(r->povm_residual <= scaled_to_double(r->povm_residual_bound), "case %zu povm residual %.3g", i, r->povm_residual);
    }
    return 0;
}

static int test_global_phase_invariance(void)
{
    static at0_engine_result ra, rb; at0_engine_result *a = &ra, *b = &rb; memset(a, 0, sizeof *a); memset(b, 0, sizeof *b);
    at0_case base = kat; base.psi0[0].re = R(2, 1); base.psi0[1].re = R(1, 1); base.psi0[1].im = R(-1, 2);
    base.hx = R(3, 10); base.hz = R(2, 5); base.h0 = R(1, 4);
    for (int j = 0; j < 4; j++) base.clock_energies[j] = R(-3 + 2 * j, 4);   /* -3/4,-1/4,1/4,3/4 : covers +-1/2 around 1/4 */
    OK(finalize(&base)); OK(at0_engine_run(&base, a));
    CHECK(a->kernel_dim == 2, "kernel");
    at0_crat phases[2] = { {{3,5},{4,5}}, {{0,1},{1,1}} };     /* (3+4i)/5 and i, both unit modulus */
    for (int p = 0; p < 2; p++) {
        at0_case c = base;
        for (int s = 0; s < 2; s++) {
            at0_rat re, im, t1, t2;
            OK(at0_rat_mul(phases[p].re, base.psi0[s].re, &t1)); OK(at0_rat_mul(phases[p].im, base.psi0[s].im, &t2)); OK(at0_rat_sub(t1, t2, &re));
            OK(at0_rat_mul(phases[p].re, base.psi0[s].im, &t1)); OK(at0_rat_mul(phases[p].im, base.psi0[s].re, &t2)); OK(at0_rat_add(t1, t2, &im));
            c.psi0[s].re = re; c.psi0[s].im = im;
        }
        OK(finalize(&c)); OK(at0_engine_run(&c, b));
        CHECK(strcmp(c.case_id, base.case_id) != 0, "phase-multiplied case must be a different question");
        for (int k = 0; k < 4; k++) {
            CHECK(fabs(a->label[k].clock_probability - b->label[k].clock_probability) <= 4e-16, "p(k) changed under global phase");
            for (int ax = 0; ax < 3; ax++) for (int sg = 0; sg < 2; sg++)
                CHECK(fabs(a->label[k].pauli[ax][sg] - b->label[k].pauli[ax][sg]) <= 8e-16, "pauli changed under global phase %d", p);
        }
    }
    return 0;
}

static int expect_refusal(const char *name, const char *from, const char *to, at0_status want)
{
    static char text[8192]; at0_case c;
    memcpy(text, kat_bytes, kat_len); text[kat_len] = 0;
    char *pos = strstr(text, from);
    CHECK(pos != NULL, "%s: pattern not found", name);
    static char out[8192];
    size_t head = (size_t)(pos - text);
    memcpy(out, text, head); out[head] = 0;
    strcat(out, to); strcat(out, pos + strlen(from));
    at0_status st = at0_case_parse((const uint8_t *)out, strlen(out), &c);
    CHECK(st == want, "%s: got %s, want %s", name, at0_status_name(st), at0_status_name(want));
    return 0;
}

/* two substitutions in sequence */
static int expect_refusal2(const char *name, const char *from1, const char *to1, const char *from2, const char *to2, at0_status want)
{
    static char text[8192]; at0_case c;
    memcpy(text, kat_bytes, kat_len); text[kat_len] = 0;
    const char *from[2] = { from1, from2 }, *to[2] = { to1, to2 };
    for (int i = 0; i < 2; i++) {
        char *pos = strstr(text, from[i]);
        CHECK(pos != NULL, "%s: pattern %d not found", name, i);
        static char out[8192];
        size_t head = (size_t)(pos - text);
        memcpy(out, text, head); out[head] = 0;
        strcat(out, to[i]); strcat(out, pos + strlen(from[i]));
        strcpy(text, out);
    }
    at0_status st = at0_case_parse((const uint8_t *)text, strlen(text), &c);
    CHECK(st == want, "%s: got %s, want %s", name, at0_status_name(st), at0_status_name(want));
    return 0;
}

/* qualification D1: in-limit tokens with large, distinct denominators must run, not overflow */
static int test_large_rationals_within_limits(void)
{
    static at0_engine_result r;
    {
        /* Agent 5's positive case P5 (copied byte for byte into tests/cases) */
        static uint8_t buf[8192]; size_t len; at0_case c;
        CHECK(load_file("p5-large-rationals.case", buf, sizeof buf, &len), "cannot read p5 case");
        OK(at0_case_parse(buf, len, &c));
        OK(at0_engine_run(&c, &r));
        CHECK(r.kernel_dim == 2 && r.psi_nonzero, "P5 kernel %d nonzero %d", r.kernel_dim, r.psi_nonzero);
        double psum = 0;
        for (int k = 0; k < r.label_count; k++) {
            psum += r.label[k].clock_probability;
            for (int a = 0; a < 3; a++)
                CHECK(fabs(r.label[k].pauli[a][0] + r.label[k].pauli[a][1] - 1.0) <= 1e-14, "P5 label %d axis %d not normalized", k, a);
        }
        CHECK(fabs(psum - 1.0) <= 1e-14, "P5 clock probabilities sum %.17g", psum);
    }
    {
        /* N = 2, h = (1/349524, 1/262143, 0) = (3, 4, 0)/1048572: |h| = 5/1048572, three distinct in-limit denominators;
         * psi0 = ((-3 + 4i), 5) / 1048571 is the exact eigenvector v_0, so <v_1|psi0> = 0 exactly
         * (the overlap decision must be exact over 128-bit integers, never a reduced rational).
         * The conditional state is then the stationary eigenstate for every label:
         * P(Z+) = 25/50 = 1/2, P(X+) = (1 - 3/5)/2 = 1/5, P(Y+) = (1 - 4/5)/2 = 1/10. */
        at0_case c; spec_two_level(&c, 4, 0);
        strcpy(c.name, "large-distinct-denominators");
        c.clock_energies[0] = R(-5, 1048572); c.clock_energies[1] = R(5, 1048572);
        c.h0 = R(0, 1); c.hx = R(1, 349524); c.hy = R(1, 262143); c.hz = R(0, 1);
        c.psi0[0].re = R(-3, 1048571); c.psi0[0].im = R(4, 1048571);
        c.psi0[1].re = R(5, 1048571);  c.psi0[1].im = R(0, 1);
        c.povm_tau_turns = R(1048572, 40);     /* tau (E_1 - E_0) = 1/4 turn per label step */
        OK(finalize(&c));
        CHECK(c.h_norm.n == 5 && c.h_norm.d == 1048572, "|h|");
        OK(at0_engine_run(&c, &r));
        CHECK(r.kernel_dim == 2 && r.psi_nonzero, "kernel %d nonzero %d", r.kernel_dim, r.psi_nonzero);
        for (int k = 0; k < r.label_count; k++) {
            CHECK(fabs(r.label[k].pauli[2][0] - 0.5) <= 1e-14, "Z+ label %d = %.17g", k, r.label[k].pauli[2][0]);
            CHECK(fabs(r.label[k].pauli[0][0] - 0.2) <= 1e-14, "X+ label %d = %.17g", k, r.label[k].pauli[0][0]);
            CHECK(fabs(r.label[k].pauli[1][0] - 0.1) <= 1e-14, "Y+ label %d = %.17g", k, r.label[k].pauli[1][0]);
        }
        /* the exact zero decision itself: with energies {-5/1048572, 1/1} only (j = 0, s = 1) is in the
         * kernel, and <v_1|psi0> = 0 exactly, so Psi = 0 and the engine must say so (the old engine
         * overflowed here; a wrong "nonzero" would give a tiny junk state instead) */
        c.clock_energies[1] = R(1, 1);
        OK(finalize(&c));
        OK(at0_engine_run(&c, &r));
        CHECK(r.kernel_dim == 1 && !r.psi_nonzero, "exact zero: kernel %d nonzero %d", r.kernel_dim, r.psi_nonzero);
        c.clock_energies[1] = R(5, 1048572);
        /* and the generic (nonzero overlap) branch with three further denominators */
        c.psi0[0].re = R(1, 1048571); c.psi0[0].im = R(0, 1);
        c.psi0[1].re = R(1, 1048569); c.psi0[1].im = R(1, 1048567);
        OK(finalize(&c));
        OK(at0_engine_run(&c, &r));
        CHECK(r.psi_nonzero, "generic psi0 gives a nonzero state");
        double psum = 0;
        for (int k = 0; k < r.label_count; k++) psum += r.label[k].clock_probability;
        CHECK(fabs(psum - 1.0) <= 1e-14, "clock probabilities sum %.17g", psum);
    }
    return 0;
}

static int test_invalid_input_refusal(void)
{
    int f = 0;
    f |= expect_refusal("noncanonical rational", "-3/2,-1/2", "-6/4,-1/2", AT0_CASE_NONCANONICAL);
    f |= expect_refusal("header version", "OMEGA-AT0-CASE v1", "OMEGA-AT0-CASE v2", AT0_CASE_UNSUPPORTED_VERSION);
    f |= expect_refusal("domain", "domain omega.at0.case.v1", "domain omega.at0.case.v2", AT0_CASE_UNSUPPORTED_VERSION);
    f |= expect_refusal("dimension too small", "clock_dim 4\nclock_energies -3/2,-1/2,1/2,3/2", "clock_dim 1\nclock_energies -3/2", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("energies not increasing", "-3/2,-1/2,1/2,3/2", "-3/2,1/2,-1/2,3/2", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("irrational spectrum", "0/1,0/1,0/1,1/2", "0/1,1/1,0/1,1/1", AT0_CASE_IRRATIONAL_SPECTRUM);
    f |= expect_refusal("case_id mismatch", "case_id 3cf4", "case_id 4cf4", AT0_CASE_ID_MISMATCH);
    f |= expect_refusal("acceptance_id mismatch", "acceptance_id a13f", "acceptance_id b13f", AT0_CASE_ID_MISMATCH);
    f |= expect_refusal("zero tau", "povm_tau_turns 1/4", "povm_tau_turns 0/1", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("negative weight", "povm_weight 1/1", "povm_weight -1/1", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("unknown reference label", "reference_clock_label t0", "reference_clock_label t9", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("duplicate label", "clock_label 3 t3", "clock_label 3 t2", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("zero system state", "(1/1;0/1),(1/1;0/1)", "(0/1;0/1),(0/1;0/1)", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("wrong fixed value", "interaction NONE", "interaction WEAK", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("float text", "tol_probability 1@12", "tol_probability 1e-12", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("noncanonical scaled", "tol_probability 1@12", "tol_probability 10@13", AT0_CASE_NONCANONICAL);
    f |= expect_refusal("double space", "clock_dim 4", "clock_dim  4", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("crlf", "clock_dim 4\n", "clock_dim 4\r\n", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("missing final lf", "end\n", "end", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("unknown key", "interaction NONE", "interactions NONE", AT0_CASE_PARSE_ERROR);
    /* qualification D2: well-formed but noncanonical integers are CASE_NONCANONICAL (rule 1, canonical form) */
    f |= expect_refusal("R17 leading-zero clock_dim", "clock_dim 4\n", "clock_dim 04\n", AT0_CASE_NONCANONICAL);
    f |= expect_refusal("R30 negative zero h0", "0/1,0/1,0/1,1/2", "-0/1,0/1,0/1,1/2", AT0_CASE_NONCANONICAL);
    f |= expect_refusal("leading-zero label count", "clock_label_count 4", "clock_label_count 04", AT0_CASE_NONCANONICAL);
    f |= expect_refusal("leading-zero label index", "clock_label 0 t0", "clock_label 00 t0", AT0_CASE_NONCANONICAL);
    f |= expect_refusal("plus sign is a shape error", "povm_weight 1/1", "povm_weight +1/1", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("long leading-zero count is still noncanonical", "clock_dim 4\n", "clock_dim 0000000004\n", AT0_CASE_NONCANONICAL);
    /* within rule 1: a shape error after a noncanonical token still reports the shape error */
    f |= expect_refusal2("shape error beats canonical form across lines", "clock_dim 4\n", "clock_dim 04\n", "povm_weight 1/1", "povm_weight 1/x", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("bad label chars", "clock_label 1 t1", "clock_label 1 T1", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("trailing lines", "end\n", "end\nextra\n", AT0_CASE_PARSE_ERROR);
    f |= expect_refusal("bad failure code with FAIL", "expected_outcome PASS\nexpected_failure_codes none", "expected_outcome FAIL\nexpected_failure_codes NOT_A_CODE", AT0_CASE_ID_MISMATCH);
    /* rule 6 with correct identities: FAIL with "none", unsorted and duplicate code lists */
    at0_case c = kat; c.expected_outcome = AT0_EXPECT_FAIL; c.expected_failure_count = 0;
    at0_status st = finalize(&c);
    CHECK(st == AT0_CASE_INVALID_PARAMETER, "FAIL with none: %s", at0_status_name(st));
    c = kat; c.expected_outcome = AT0_EXPECT_FAIL; c.expected_failure_count = 2; c.expected_failure_idx[0] = 4; c.expected_failure_idx[1] = 2;
    st = finalize(&c); CHECK(st == AT0_CASE_INVALID_PARAMETER, "unsorted codes: %s", at0_status_name(st));
    c = kat; c.expected_outcome = AT0_EXPECT_FAIL; c.expected_failure_count = 2; c.expected_failure_idx[0] = 4; c.expected_failure_idx[1] = 4;
    st = finalize(&c); CHECK(st == AT0_CASE_INVALID_PARAMETER, "duplicate codes: %s", at0_status_name(st));
    c = kat; c.expected_outcome = AT0_EXPECT_FAIL; c.expected_failure_count = 2; c.expected_failure_idx[0] = 4; c.expected_failure_idx[1] = 7;
    st = finalize(&c); CHECK(st == AT0_OK, "sorted codes should pass: %s", at0_status_name(st));
    /* API argument checking */
    at0_state s; double complex z;
    OK(at0_state_init(&s, 4));
    CHECK(at0_state_get(&s, 4, 0, &z) == AT0_ERR_ARGUMENT, "clock index out of range");
    CHECK(at0_state_get(&s, 0, 2, &z) == AT0_ERR_ARGUMENT, "system index out of range");
    CHECK(at0_state_init(&s, 65) == AT0_ERR_ARGUMENT, "dimension above limit");
    CHECK(at0_state_init(NULL, 4) == AT0_ERR_ARGUMENT, "null state");
    CHECK(at0_case_parse(NULL, 0, &c) == AT0_ERR_ARGUMENT, "null bytes");
    CHECK(at0_engine_run(&kat, NULL) == AT0_ERR_ARGUMENT, "null result");
    at0_qubit_op op; CHECK(at0_pauli_matrix((at0_axis)3, &op) == AT0_ERR_ARGUMENT, "bad axis");
    return f;
}

static int test_repeatability_and_order(void)
{
    static char t1[1 << 16], t2[1 << 16];
    static at0_engine_result ra, rb; at0_engine_result *a = &ra, *b = &rb; memset(a, 0, sizeof *a); memset(b, 0, sizeof *b);
    at0_case c = kat; c.hx = R(3, 10); c.hz = R(2, 5); c.h0 = R(1, 4);
    for (int j = 0; j < 4; j++) c.clock_energies[j] = R(-3 + 2 * j, 4);
    strcpy(c.reference_clock_label, "t2");
    OK(finalize(&c));
    OK(at0_engine_run(&c, a)); long n1 = at0_engine_emit(&c, a, t1, sizeof t1);
    OK(at0_engine_run(&c, b)); long n2 = at0_engine_emit(&c, b, t2, sizeof t2);
    CHECK(n1 > 0 && n1 == n2 && memcmp(t1, t2, (size_t)n1) == 0, "two runs differ");
    OK(at0_engine_run_reversed(&c, b)); n2 = at0_engine_emit(&c, b, t2, sizeof t2);
    CHECK(n1 == n2 && memcmp(t1, t2, (size_t)n1) == 0, "reversed label order changed the values block");
    CHECK(strstr(t1, "OMEGA-AT0-ENGINE v1\ndomain omega.at0.engine.v1\n") == t1, "header");
    CHECK(strstr(t1, "\nthreads 1\n") && strstr(t1, "\nbound_kind ESTIMATED\n"), "numerics block");
    /* f64 token rules */
    char tok[32];
    at0_f64_format(-0.0, tok, sizeof tok); CHECK(strcmp(tok, "f64:0000000000000000") == 0, "-0 folding: %s", tok);
    at0_f64_format(1.0, tok, sizeof tok);  CHECK(strcmp(tok, "f64:3ff0000000000000") == 0, "1.0: %s", tok);
    at0_f64_format(0.0 / 0.0, tok, sizeof tok); CHECK(strcmp(tok, "nonfinite") == 0, "nan: %s", tok);
    /* trivial physical state: values undefined, labels UNDEFINED, kernel 0 */
    at0_case u = kat; for (int j = 0; j < 4; j++) u.clock_energies[j] = R(j + 1, 1);
    OK(finalize(&u)); OK(at0_engine_run(&u, a)); n1 = at0_engine_emit(&u, a, t1, sizeof t1);
    CHECK(n1 > 0 && strstr(t1, "physical_state_kernel_dim 0\nconstraint_residual undefined 0@0\n"), "trivial kernel output");
    CHECK(strstr(t1, "label 0 t0 UNDEFINED\n") && strstr(t1, "pauli 3 Z MINUS undefined 0@0\n"), "trivial labels");
    return 0;
}

static int test_review_findings(void);
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: at0-tests <cases-dir>\n"); return 2; }
    cases_dir = argv[1];
    if (!load_file("kat-ideal-qubit-n4.case", kat_bytes, sizeof kat_bytes, &kat_len)) { fprintf(stderr, "cannot read KAT case\n"); return 2; }
    at0_status st = at0_case_parse(kat_bytes, kat_len, &kat);
    if (st != AT0_OK) { fprintf(stderr, "KAT case refused: %s\n", at0_status_name(st)); return 2; }
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "kat_roundtrip", test_kat_roundtrip },
        { "exact_arithmetic", test_exact_arithmetic },
        { "state_normalization", test_state_normalization },
        { "constraint_satisfaction", test_constraint_satisfaction },
        { "povm_normalization", test_povm_normalization },
        { "conditional_density_matrices", test_conditional_density },
        { "pauli_probabilities", test_pauli_probabilities },
        { "schrodinger_general_n_phase_omega", test_schrodinger_general },
        { "global_phase_invariance", test_global_phase_invariance },
        { "invalid_input_refusal", test_invalid_input_refusal },
        { "repeatability_and_order", test_repeatability_and_order },
        { "review_findings_f1_f5", test_review_findings },
        { "large_rationals_within_limits", test_large_rationals_within_limits },
    };
    int total = (int)(sizeof tests / sizeof tests[0]), passed = 0;
    for (int i = 0; i < total; i++) {
        int before = failures;
        int rc = tests[i].fn();
        int ok = rc == 0 && failures == before;
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        if (ok) passed++;
    }
    printf("at0-tests: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}

/* regression tests for the independent review of PR omega#360 (findings F1..F5) */
static int test_review_findings(void)
{
    static at0_engine_result res; at0_engine_result *r = &res;
    /* F1: tiny |h| = 1/1025 is a valid rational spectrum */
    at0_case c = kat; c.hz = R(1, 1025);
    at0_status st = finalize(&c); CHECK(st == AT0_OK, "F1 refused: %s", at0_status_name(st));
    /* F2: energies and h0 at the contract limit */
    c = kat; c.clock_energies[0] = R(-1048576, 1); c.h0 = R(1048576, 1);
    OK(finalize(&c)); OK(at0_engine_run(&c, r)); CHECK(r->kernel_dim == 0, "F2 kernel %d", r->kernel_dim);
    /* F3: large overlaps in the exact zero test */
    c = kat; c.clock_energies[0] = R(-1000, 1); c.clock_energies[3] = R(1000, 1); c.hx = R(600, 1); c.hz = R(800, 1);
    c.psi0[0].re = R(1000, 1);
    OK(finalize(&c)); OK(at0_engine_run(&c, r)); CHECK(r->kernel_dim == 2 && r->psi_nonzero, "F3 run");
    /* F4: 19-digit tokens never overflow and are refused as out of range */
    int f = 0;
    f |= expect_refusal("F4 long denominator", "povm_weight 1/1", "povm_weight 1/9999999999999999999", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("F4 long scaled", "tol_probability 1@12", "tol_probability 9999999999999999999@0", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("scaled k above 40", "tol_probability 1@12", "tol_probability 1@41", AT0_CASE_INVALID_PARAMETER);
    f |= expect_refusal("rational above limit", "povm_weight 1/1", "povm_weight 2000000/1", AT0_CASE_INVALID_PARAMETER);
    /* F5: version check (rule 2) precedes the limit check (rule 3) */
    f |= expect_refusal("F5 version before limit", "OMEGA-AT0-CASE v1\n", "OMEGA-AT0-CASE v2\n", AT0_CASE_UNSUPPORTED_VERSION);
    {
        static char text[8192]; at0_case d;
        memcpy(text, kat_bytes, kat_len); text[kat_len] = 0;
        char *p = strstr(text, "OMEGA-AT0-CASE v1"); p[15] = '2';
        char *q = strstr(text, "povm_weight 1/1"); memcpy(q, "povm_weight 2/1", 15);   /* same length, over no limit yet */
        at0_status s2 = at0_case_parse((const uint8_t *)text, strlen(text), &d);
        CHECK(s2 == AT0_CASE_UNSUPPORTED_VERSION, "F5: %s", at0_status_name(s2));
    }
    f |= expect_refusal("F5 noncanonical is rule 1, before the limit", "povm_weight 1/1", "povm_weight 2000000/4000000", AT0_CASE_NONCANONICAL);
    return f;
}
