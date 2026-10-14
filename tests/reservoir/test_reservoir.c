/* omega#440 Gate 1 tests for src/reservoir. Expected values come from hand
 * derivations in the comments, from closed forms evaluated here in
 * binary64, from the independent binary64 oracle (res_oracle64.c), or from
 * counts taken off the reference archive by tools outside this repo (noted
 * where used); never from calling the code under test. Every CHECK is one
 * test case; any failure makes the program exit nonzero. */
#include "omega_reservoir.h"
#include "omega_reservoir_readout.h"
#include "res_fixtures.h"
#include "res_oracle64.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_ST(expr, want) do { OmegaResStatus s_ = (expr); g_checks++; if (s_ != (want)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s -> %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, (int)(want)); } } while (0)
#define NEAR(a, b, rel) do { double a_ = (a), b_ = (b); g_checks++; \
    if (!(fabs(a_ - b_) <= (rel) * fmax(fabs(b_), 1e-300))) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.17g, want %.17g\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define NEARABS(a, b, ab) do { double a_ = (a), b_ = (b); g_checks++; \
    if (!(fabs(a_ - b_) <= (ab))) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s = %.17g, want %.17g\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* Shorter oracle scenarios under ASan (mk/reservoir.mk passes -DRES_QUICK). */
#ifdef RES_QUICK
#define ORACLE_SAMPLES 20u
#else
#define ORACLE_SAMPLES 100u
#endif

static OmegaResWorkspace g_ws;
static OmegaResConfig g_cfg, g_cfg2;
static OmegaResState g_s, g_s2;
#define MAX_KNOTS 4096u
static float g_tab[MAX_KNOTS * OMEGA_RES_MAX_NODES];
static double g_vref[MAX_KNOTS * OMEGA_RES_MAX_NODES];

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static double runit(void) { return (double)(rnd() >> 11) / 9007199254740992.0; }

static OmegaResIntegrator integ(float h, uint32_t K) { OmegaResIntegrator i = {h, K}; return i; }

/* Symmetrized edge list of a 1-based reference neighbour table. */
static uint32_t fixture_edges(const uint8_t (*nb)[7], uint32_t n, OmegaResEdge *e)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 1; k <= nb[i][0]; k++) {
            uint32_t a = i, b = (uint32_t)nb[i][k] - 1u, lo = a < b ? a : b, hi = a < b ? b : a, dup = 0;
            for (uint32_t q = 0; q < m && !dup; q++) dup = e[q].a == lo && e[q].b == hi;
            if (!dup) { e[m].a = (uint16_t)lo; e[m].b = (uint16_t)hi; m++; }
        }
    return m;
}

static OmegaResStatus fixture_topology(const uint8_t (*nb)[7], uint32_t n, uint32_t ground1, OmegaResTopology *t)
{
    OmegaResEdge e[OMEGA_RES_MAX_EDGES];
    uint32_t m = fixture_edges(nb, n, e), g = ground1 - 1u;
    return omega_res_topology_build(n, e, m, &g, 1, t);
}

static void fill_const(float *tab, uint32_t knots, uint32_t n, const float *per_node)
{
    for (uint32_t k = 0; k < knots; k++)
        for (uint32_t i = 0; i < n; i++) tab[k * n + i] = per_node[i];
}

/* NARMA-style masked drive (Sci. Adv. Fig. 4A; reference condition 7):
 * u ~ U[0, 0.5), I = u * hi / 0.5, sub-pulses of 1 ms (10 knots of 0.1 ms),
 * mask bit 1 -> I, bit 0 -> hi - I (the reference "inverter" with lo = 0).
 * Masks here come from this file's own generator, not MATLAB's. */
static uint32_t fill_masked(float *tab, uint32_t n, uint32_t samples, uint32_t mask_len, float hi)
{
    uint8_t mask[OMEGA_RES_MAX_NODES][8];
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < mask_len; j++) mask[i][j] = (uint8_t)(rnd() & 1u);
    const uint32_t knots = samples * mask_len * 10u + 1u;
    double seq = 0.0;
    for (uint32_t k = 0; k < knots; k++) {
        uint32_t sub = k / 10u, smp = sub / mask_len, mi = sub % mask_len;
        if (k % (10u * mask_len) == 0 && smp < samples) seq = runit() * 0.5 * (double)hi / 0.5;
        for (uint32_t i = 0; i < n; i++) tab[k * n + i] = mask[i][mi] ? (float)seq : (float)((double)hi - seq);
    }
    return knots;
}

/* ------------------------------------------------------------ topology */

static void test_topology(void)
{
    OmegaResTopology t, t2;
    CHECK_ST(omega_res_topology_chain(5, 4, &t), OMEGA_RES_OK);
    CHECK(t.node_count == 5 && t.edge_count == 4);
    CHECK(t.degree[0] == 1 && t.degree[1] == 2 && t.degree[2] == 2 && t.degree[3] == 2 && t.degree[4] == 1);
    CHECK(t.is_ground[4] == 1 && t.is_ground[0] == 0);
    CHECK(t.edge[0].a == 0 && t.edge[0].b == 1 && t.edge[3].a == 3 && t.edge[3].b == 4);

    /* Same membranes listed shuffled and reversed: identical canonical struct. */
    OmegaResEdge sh[4] = {{4, 3}, {1, 0}, {2, 3}, {2, 1}};
    uint32_t g4 = 4;
    CHECK_ST(omega_res_topology_build(5, sh, 4, &g4, 1, &t2), OMEGA_RES_OK);
    CHECK(memcmp(&t, &t2, sizeof t) == 0);

    uint32_t g0 = 0, gg[2] = {0, 0}, gall[2] = {0, 1};
    OmegaResEdge self[1] = {{1, 1}}, dupl[2] = {{0, 1}, {1, 0}}, oob[1] = {{0, 5}};
    CHECK_ST(omega_res_topology_build(2, self, 1, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_build(2, dupl, 2, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_build(5, oob, 1, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_build(2, dupl, 1, gg, 2, &t2), OMEGA_RES_ERR_TOPOLOGY);    /* duplicate ground */
    CHECK_ST(omega_res_topology_build(2, dupl, 1, gall, 2, &t2), OMEGA_RES_ERR_TOPOLOGY);  /* every node ground */
    CHECK_ST(omega_res_topology_build(2, dupl, 1, &g0, 0, &t2), OMEGA_RES_ERR_TOPOLOGY);   /* no ground */
    CHECK_ST(omega_res_topology_build(2, dupl, 0, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);   /* no edge */
    CHECK_ST(omega_res_topology_build(1, dupl, 1, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_build(OMEGA_RES_MAX_NODES + 1, dupl, 1, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    uint32_t g9 = 9;
    CHECK_ST(omega_res_topology_build(3, dupl, 1, &g9, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);   /* ground out of range */
    /* Isolated node 2 (no membrane, so no capacitance). */
    CHECK_ST(omega_res_topology_build(3, dupl, 1, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    /* Star with 7 leaves: centre degree 7 > 6. */
    OmegaResEdge star[7];
    for (uint16_t i = 0; i < 7; i++) { star[i].a = 0; star[i].b = (uint16_t)(i + 1); }
    CHECK_ST(omega_res_topology_build(8, star, 7, &g0, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_build(7, star, 6, &g0, 1, &t2), OMEGA_RES_OK);              /* degree 6 is fine */
    CHECK(t2.degree[0] == 6);
    /* Two components, only one grounded: M would be singular. */
    OmegaResEdge two[3] = {{0, 1}, {1, 2}, {3, 4}};
    uint32_t g2 = 2, g24[2] = {2, 4};
    CHECK_ST(omega_res_topology_build(5, two, 3, &g2, 1, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_build(5, two, 3, g24, 2, &t2), OMEGA_RES_OK);
    CHECK_ST(omega_res_topology_build(5, NULL, 3, g24, 2, &t2), OMEGA_RES_ERR_NULL);
    CHECK_ST(omega_res_topology_chain(1, 0, &t2), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_topology_chain(4, 4, &t2), OMEGA_RES_ERR_TOPOLOGY);
}

/* Edge counts and degree histograms below were counted off the .m tables
 * with awk (outside this repo) after the reference's own symmetrization. */
static void degree_histogram(const OmegaResTopology *t, uint32_t h[8])
{
    memset(h, 0, 8 * sizeof h[0]);
    for (uint32_t i = 0; i < t->node_count; i++) h[t->degree[i]]++;
}

static void test_reference_lattices(void)
{
    OmegaResTopology t;
    uint32_t h[8];
    CHECK_ST(fixture_topology(RES_FIX_HEX13_NB, 13, RES_FIX_HEX13_GROUND, &t), OMEGA_RES_OK);
    CHECK(t.edge_count == 21);
    degree_histogram(&t, h);
    CHECK(h[1] == 3 && h[2] == 0 && h[3] == 3 && h[4] == 6 && h[5] == 0 && h[6] == 1);
    CHECK(t.degree[RES_FIX_HEX13_GROUND - 1] == 6 && t.is_ground[RES_FIX_HEX13_GROUND - 1]);
    for (int k = 0; k < 3; k++) CHECK(t.degree[RES_FIX_HEX13_INPUTS[k] - 1] == 1);

    CHECK_ST(fixture_topology(RES_FIX_HEX25_NB, 25, RES_FIX_HEX25_GROUND, &t), OMEGA_RES_OK);
    CHECK(t.edge_count == 51);
    degree_histogram(&t, h);
    CHECK(h[1] == 3 && h[2] == 0 && h[3] == 6 && h[4] == 6 && h[5] == 3 && h[6] == 7);
    CHECK(t.degree[RES_FIX_HEX25_GROUND - 1] == 6);
    for (int k = 0; k < 3; k++) CHECK(t.degree[RES_FIX_HEX25_INPUTS[k] - 1] == 1);

    CHECK_ST(fixture_topology(RES_FIX_HEX43_NB, 43, RES_FIX_HEX43_GROUND, &t), OMEGA_RES_OK);
    CHECK(t.edge_count == 99);
    degree_histogram(&t, h);
    CHECK(h[1] == 3 && h[2] == 0 && h[3] == 7 && h[4] == 10 && h[5] == 4 && h[6] == 19);
    CHECK(t.degree[RES_FIX_HEX43_GROUND - 1] == 6);
    for (int k = 0; k < 3; k++) CHECK(t.degree[RES_FIX_HEX43_INPUTS[k] - 1] == 1);
    /* Boundary nodes of the six-neighbour lattice: degree < 6 (43 - 19). */
    uint32_t boundary = 0;
    for (uint32_t i = 0; i < t.node_count; i++) boundary += t.degree[i] < 6;
    CHECK(boundary == 24);

    /* Length50.mat: one path, leaves 1 and 2, ground 50 interior. */
    OmegaResEdge e[49];
    for (int i = 0; i < 49; i++) {
        e[i].a = (uint16_t)(RES_FIX_CHAIN50_EDGES[i][0] - 1);
        e[i].b = (uint16_t)(RES_FIX_CHAIN50_EDGES[i][1] - 1);
    }
    uint32_t g = RES_FIX_CHAIN50_GROUND - 1;
    CHECK_ST(omega_res_topology_build(50, e, 49, &g, 1, &t), OMEGA_RES_OK);
    degree_histogram(&t, h);
    CHECK(h[1] == 2 && h[2] == 48 && t.degree[0] == 1 && t.degree[1] == 1 && t.degree[49] == 2);
}

/* ---------------------------------------------------------- parameters */

static void test_params(void)
{
    OmegaResParams p;
    CHECK_ST(omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p), OMEGA_RES_OK);
    CHECK(p.eps_f_per_m == 8.85e-12f && p.h_m == 4e-9f && p.area_m2 == 5e-8f && p.g_u_s == 5e-8f);
    CHECK(p.z == 0.00015f && p.rate_scale == 1e4f && p.n0_scale == 0.002f && p.n0_rate == 3.7919f);
    CHECK(p.conc_um[0] == 1.0f && p.conc_um[1] == 2.0f && p.conc_um[2] == 3.0f && p.conc_um[3] == 1.0f);
    CHECK(p.tau0_s[4] == 0.001f && p.v_tau_v[4] == 0.045f && p.v_e_v[4] == 0.0055f);
    CHECK_ST(omega_res_params_reference(OMEGA_RES_REF_LORENZ, 43, &p), OMEGA_RES_OK);
    CHECK(p.eps_f_per_m == (float)(2.1 * 8.85e-12) && p.v_tau_v[42] == 0.0454f);
    CHECK(p.conc_um[41] == 3.0f && p.conc_um[42] == 1.0f); /* 1 + (i mod 3), 0-based node i */
    CHECK_ST(omega_res_params_reference((OmegaResReference)7, 5, &p), OMEGA_RES_ERR_PARAM);
    CHECK_ST(omega_res_params_reference(OMEGA_RES_REF_NARMA, 0, &p), OMEGA_RES_ERR_PARAM);

    OmegaResTopology t;
    omega_res_topology_chain(5, 4, &t);
    OmegaResIntegrator ig = integ(1e-5f, 10);
    const float nan = (float)NAN;
    struct { float *field; float bad; } cases[] = {
        {&p.eps_f_per_m, nan}, {&p.eps_f_per_m, -1.0f}, {&p.h_m, 0.0f}, {&p.area_m2, -5e-8f},
        {&p.g_u_s, (float)INFINITY}, {&p.z, 0.0f}, {&p.z, 1.5f}, {&p.rate_scale, 0.0f}, {&p.n0_scale, -1.0f},
        {&p.n0_rate, nan}, {&p.conc_um[2], -1.0f}, {&p.tau0_s[1], 0.0f}, {&p.v_tau_v[0], nan}, {&p.v_e_v[3], -0.0055f},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
        *cases[c].field = cases[c].bad;
        CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_ERR_PARAM);
    }
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    p.n0_rate = 40.0f; /* ln N0 = ln 0.002 + 40 * 3 = 113.8 > OMEGA_RES_Y_MAX */
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_ERR_PARAM);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 4, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_ERR_PARAM); /* node count mismatch */
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    OmegaResIntegrator bad[] = {integ(0.0f, 10), integ(-1e-5f, 10), integ(2e-3f, 1), integ(nan, 10), integ(1e-5f, 0)};
    for (size_t c = 0; c < sizeof bad / sizeof bad[0]; c++)
        CHECK_ST(omega_res_config_build(&t, &p, &bad[c], &g_cfg), OMEGA_RES_ERR_PARAM);
    /* A hand-edited (non-canonical) topology is refused. */
    OmegaResTopology t2 = t;
    t2.degree[0] = 3;
    CHECK_ST(omega_res_config_build(&t2, &p, &ig, &g_cfg), OMEGA_RES_ERR_TOPOLOGY);
    t2 = t;
    OmegaResEdge tmp = t2.edge[0]; t2.edge[0] = t2.edge[1]; t2.edge[1] = tmp;
    CHECK_ST(omega_res_config_build(&t2, &p, &ig, &g_cfg), OMEGA_RES_ERR_TOPOLOGY);
    CHECK_ST(omega_res_config_build(NULL, &p, &ig, &g_cfg), OMEGA_RES_ERR_NULL);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
}

/* ------------------------------------------------------------ identity */

static void test_identity(void)
{
    OmegaResTopology t, t2;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    omega_res_topology_chain(5, 4, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg2), OMEGA_RES_OK);
    CHECK(memcmp(omega_res_config_id(&g_cfg), omega_res_config_id(&g_cfg2), 32) == 0);
    CHECK(omega_res_config_id(NULL) == NULL);
    uint8_t zero[32] = {0};
    CHECK(memcmp(g_cfg.id, zero, 32) != 0);
    /* Edge listing order does not change the identity. */
    OmegaResEdge sh[4] = {{3, 4}, {2, 1}, {0, 1}, {3, 2}};
    uint32_t g4 = 4;
    omega_res_topology_build(5, sh, 4, &g4, 1, &t2);
    CHECK_ST(omega_res_config_build(&t2, &p, &ig, &g_cfg2), OMEGA_RES_OK);
    CHECK(memcmp(g_cfg.id, g_cfg2.id, 32) == 0);
    /* -0.0 and +0.0 encode alike (here: concentration of node 0). */
    OmegaResParams pz = p, pn = p;
    pz.conc_um[0] = 0.0f; pn.conc_um[0] = -0.0f;
    CHECK_ST(omega_res_config_build(&t, &pz, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_config_build(&t, &pn, &ig, &g_cfg2), OMEGA_RES_OK);
    CHECK(memcmp(g_cfg.id, g_cfg2.id, 32) == 0);
    /* One ulp in any parameter, a different ground, step or K: new identity. */
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    float *fields[] = {&p.eps_f_per_m, &p.h_m, &p.area_m2, &p.g_u_s, &p.z, &p.rate_scale, &p.n0_scale,
                       &p.n0_rate, &p.conc_um[3], &p.tau0_s[0], &p.v_tau_v[2], &p.v_e_v[4]};
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
        float keep = *fields[f];
        *fields[f] = nextafterf(keep, INFINITY);
        CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg2), OMEGA_RES_OK);
        CHECK(memcmp(g_cfg.id, g_cfg2.id, 32) != 0);
        *fields[f] = keep;
    }
    OmegaResIntegrator ig2 = integ(nextafterf(1e-5f, 1.0f), 10), ig3 = integ(1e-5f, 11);
    CHECK_ST(omega_res_config_build(&t, &p, &ig2, &g_cfg2), OMEGA_RES_OK);
    CHECK(memcmp(g_cfg.id, g_cfg2.id, 32) != 0);
    CHECK_ST(omega_res_config_build(&t, &p, &ig3, &g_cfg2), OMEGA_RES_OK);
    CHECK(memcmp(g_cfg.id, g_cfg2.id, 32) != 0);
    omega_res_topology_chain(5, 0, &t2);
    CHECK_ST(omega_res_config_build(&t2, &p, &ig, &g_cfg2), OMEGA_RES_OK);
    CHECK(memcmp(g_cfg.id, g_cfg2.id, 32) != 0);
}

/* ------------------------------------------------- dynamics: closed forms */

/* Zero input from N = N0 on every side, V = 0: dV = 0, ln Nss = ln N0, w = 0,
 * so 1 - e^w = -0 and every derivative is zero. The state must not move. */
static void test_zero_input_fixed_point(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    omega_res_topology_chain(5, 4, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    /* N0 = 0.002 e^(3.7919 c): c = 1 -> 0.08871, c = 2 -> 3.9343 (binary64 by hand). */
    NEAR(omega_res_state_pore(&g_s, 0), 0.002 * exp(3.7919), 1e-5);
    NEAR(omega_res_state_pore(&g_s, 1), 0.002 * exp(3.7919 * 2), 1e-5);
    CHECK(isnan(omega_res_state_pore(&g_s, 8)));
    memset(g_tab, 0, sizeof(float) * 5 * 401);
    OmegaResInput in = {5, 401, g_tab};
    g_s2 = g_s;
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 4000), OMEGA_RES_OK);
    CHECK(g_s.step == 4000);
    CHECK(memcmp(g_s.v, g_s2.v, sizeof g_s.v) == 0);
    CHECK(memcmp(g_s.y, g_s2.y, sizeof g_s.y) == 0);
}

/* Decay. One membrane, V = 0 (no input), every side starts at N = 1000.
 * With u = y - ln N0 and V = 0: u' = -alpha0 (e^(Z u) - 1), alpha0 =
 * rate_scale / tau0 = 1e7 /s. s = e^(-Z u) obeys s' = Z alpha0 (1 - s), so
 * s(t) = 1 - (1 - s0) e^(-Z alpha0 t) and u(t) = -ln(s(t)) / Z. */
static void test_decay(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    omega_res_topology_chain(2, 1, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 2, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1000.0f, &g_s), OMEGA_RES_OK);
    memset(g_tab, 0, sizeof(float) * 2 * 101);
    OmegaResInput in = {2, 101, g_tab};
    const double Z = 0.00015, a0 = 1e7;
    double prev[2] = {1e9, 1e9};
    for (int ms = 1; ms <= 8; ms++) {
        CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 100), OMEGA_RES_OK);
        CHECK(g_s.v[0] == 0.0f && g_s.v[1] == 0.0f);
        for (int side = 0; side < 2; side++) {
            const double ln_n0 = log(0.002) + 3.7919 * (side ? 2.0 : 1.0); /* side 1 belongs to node 1 (c = 2) */
            const double u0 = log(1000.0) - ln_n0, s0 = exp(-Z * u0);
            const double s = 1.0 - (1.0 - s0) * exp(-Z * a0 * ms * 1e-3);
            const double y = ln_n0 - log(s) / Z;
            NEARABS(g_s.y[side], y, 2e-4);
            CHECK(g_s.y[side] < prev[side] && g_s.y[side] > ln_n0); /* monotone, from above */
            prev[side] = g_s.y[side];
        }
    }
}

/* Charging. Pore kinetics frozen (rate_scale tiny) and conductance
 * negligible (N = 1e-30), so V' = I / C_node exactly: a node with one
 * membrane has C = eps A / h = 8.85e-12 * 5e-8 / 4e-9 = 1.10625e-10 F;
 * a node between two grounded neighbours has 2 C. RK4 integrates a
 * constant derivative exactly, up to rounding. */
static void test_charging(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    omega_res_topology_chain(2, 1, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 2, &p);
    p.rate_scale = 1e-12f;
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1e-30f, &g_s), OMEGA_RES_OK);
    const float cur[2] = {1e-10f, 5e-10f}; /* current at the ground is ignored */
    fill_const(g_tab, 11, 2, cur);
    OmegaResInput in = {2, 11, g_tab};
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 100), OMEGA_RES_OK);
    const double C = 8.85e-12 * 5e-8 / 4e-9;
    NEAR(g_s.v[0], 1e-10 * 1e-3 / C, 1e-5); /* 9.0395e-4 V after 1 ms */
    CHECK(g_s.v[1] == 0.0f);

    OmegaResEdge e[2] = {{0, 1}, {1, 2}};
    uint32_t gr[2] = {0, 2};
    CHECK_ST(omega_res_topology_build(3, e, 2, gr, 2, &t), OMEGA_RES_OK);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 3, &p);
    p.rate_scale = 1e-12f;
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1e-30f, &g_s), OMEGA_RES_OK);
    const float cur3[3] = {0.0f, 1e-10f, 0.0f};
    fill_const(g_tab, 11, 3, cur3);
    OmegaResInput in3 = {3, 11, g_tab};
    CHECK_ST(omega_res_advance(&g_cfg, &in3, &g_ws, &g_s, 100), OMEGA_RES_OK);
    NEAR(g_s.v[1], 1e-10 * 1e-3 / (2.0 * C), 1e-5);
    CHECK(g_s.v[0] == 0.0f && g_s.v[2] == 0.0f);
}

/* Saturation. One membrane, constant 0.1 nA into node 0 (c = 1), ground
 * node 1 (c = 2). At rest N_01 = Nss_01 = N0(1) e^(V/v_e) and N_10 =
 * N0(2) e^(-V/v_e), and the injected current equals the membrane current:
 *   I = G_u A (N0(1) e^(V/v_e) + N0(2) e^(-V/v_e)) V.
 * V* is found here by bisection in binary64 (about 0.085 V). */
static void test_saturation(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 1000);
    omega_res_topology_chain(2, 1, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 2, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    const float cur[2] = {1e-10f, 0.0f};
    fill_const(g_tab, 301, 2, cur);
    OmegaResInput in = {2, 301, g_tab};
    const double I = (double)1e-10f, gua = 5e-8 * 5e-8, ve = 0.0055;
    const double n01 = 0.002 * exp(3.7919), n10 = 0.002 * exp(3.7919 * 2);
    double lo = 0.0, hi = 0.5;
    for (int it = 0; it < 200; it++) {
        double mid = 0.5 * (lo + hi);
        double f = gua * (n01 * exp(mid / ve) + n10 * exp(-mid / ve)) * mid - I;
        if (f > 0) hi = mid; else lo = mid;
    }
    const double vstar = 0.5 * (lo + hi);
    CHECK(vstar > 0.08 && vstar < 0.09);
    /* The pores lag the voltage, so V may overshoot V* before it settles;
     * after 1.5 s every 10 ms sample must sit at V* and stay there. */
    double worst_late = 0.0;
    for (int k = 0; k < 300; k++) { /* 3 s in 10 ms chunks */
        CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 1000), OMEGA_RES_OK);
        if (k >= 150 && fabs(g_s.v[0] - vstar) > worst_late) worst_late = fabs(g_s.v[0] - vstar);
    }
    CHECK(worst_late <= 2e-4 * vstar);
    NEAR(g_s.v[0], vstar, 2e-5);
    float lam;
    CHECK_ST(omega_res_stiffness(&g_cfg, &g_s, &lam), OMEGA_RES_OK);
    CHECK(lam > 0.0f && 1e-5f * lam < 0.1f); /* far from the RK4 limit */
}

/* ------------------------------------------------ connected, disconnected */

static void test_connected_and_disconnected(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    /* Chain 0-1-2 grounded at 2, current into node 0 only: node 1 must
     * respond through the membrane (V0 > V1 > 0 = V2). */
    omega_res_topology_chain(3, 2, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 3, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    const float cur3[3] = {1e-10f, 0.0f, 0.0f};
    fill_const(g_tab, 101, 3, cur3);
    OmegaResInput in3 = {3, 101, g_tab};
    CHECK_ST(omega_res_advance(&g_cfg, &in3, &g_ws, &g_s, 1000), OMEGA_RES_OK);
    CHECK(g_s.v[0] > g_s.v[1] && g_s.v[1] > 0.0f && g_s.v[2] == 0.0f);

    /* Two components {0,1,2} (ground 2) and {3,4} (ground 4): driving node 0
     * leaves the second component exactly at rest. */
    OmegaResEdge e[3] = {{0, 1}, {1, 2}, {3, 4}};
    uint32_t gr[2] = {2, 4};
    CHECK_ST(omega_res_topology_build(5, e, 3, gr, 2, &t), OMEGA_RES_OK);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    g_s2 = g_s;
    const float cur5[5] = {1e-10f, 0.0f, 0.0f, 0.0f, 0.0f};
    fill_const(g_tab, 101, 5, cur5);
    OmegaResInput in5 = {5, 101, g_tab};
    CHECK_ST(omega_res_advance(&g_cfg, &in5, &g_ws, &g_s, 1000), OMEGA_RES_OK);
    CHECK(g_s.v[0] > 0.0f && g_s.v[1] > 0.0f);
    CHECK(g_s.v[3] == 0.0f && g_s.v[4] == 0.0f);
    CHECK(g_s.y[4] == g_s2.y[4] && g_s.y[5] == g_s2.y[5]); /* sides of membrane 3-4 */
    CHECK(g_s.y[0] != g_s2.y[0]);

    /* 43-node reference lattice, Lorenz constants, N(0) = 1, constant drive
     * at the three tail nodes: everything finite, centre held at 0, each
     * degree-1 injection node above its only neighbour. */
    CHECK_ST(fixture_topology(RES_FIX_HEX43_NB, 43, RES_FIX_HEX43_GROUND, &t), OMEGA_RES_OK);
    omega_res_params_reference(OMEGA_RES_REF_LORENZ, 43, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1.0f, &g_s), OMEGA_RES_OK);
    float cur43[43] = {0};
    for (int k = 0; k < 3; k++) cur43[RES_FIX_HEX43_INPUTS[k] - 1] = 1.1e-10f;
    fill_const(g_tab, 201, 43, cur43);
    OmegaResInput in43 = {43, 201, g_tab};
    CHECK_ST(omega_res_advance(&g_cfg, &in43, &g_ws, &g_s, 2000), OMEGA_RES_OK);
    CHECK(g_s.v[RES_FIX_HEX43_GROUND - 1] == 0.0f);
    CHECK(g_s.v[0] > g_s.v[2] && g_s.v[27] > g_s.v[22] && g_s.v[41] > g_s.v[38]); /* 1-3, 28-23, 42-39 */
}

/* ------------------------------------------------------- reproducibility */

static void test_reproducibility(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    omega_res_topology_chain(5, 4, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    g_rng = 42;
    const uint32_t knots = fill_masked(g_tab, 5, 30, 2, 1e-10f);
    OmegaResInput in = {5, knots, g_tab};
    OmegaResRunReceipt r1, r2;
    for (int run = 0; run < 2; run++) {
        if (run) memset(&g_ws, 0xFF, sizeof g_ws); /* a dirty (all-NaN) workspace changes nothing */
        CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
        CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
        CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, (uint64_t)(knots - 1) * 10u), OMEGA_RES_OK);
        CHECK_ST(omega_res_run_receipt(&g_cfg, &in, &g_s, run ? &r2 : &r1), OMEGA_RES_OK);
        if (run == 0) g_s2 = g_s;
    }
    CHECK(memcmp(&g_s, &g_s2, sizeof g_s) == 0);
    CHECK(memcmp(&r1, &r2, sizeof r1) == 0);
    CHECK(r1.steps == (uint64_t)(knots - 1) * 10u);
    CHECK(g_s.v[0] != 0.0f);
    /* Step by step with omega_res_step equals one advance call, bit for bit. */
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    OmegaResState a, b;
    a = g_s;
    for (uint64_t i = 0; i < (uint64_t)(knots - 1) * 10u; i++) {
        if (omega_res_step(&g_cfg, &in, &g_ws, &a, &b) != OMEGA_RES_OK) { CHECK(0); break; }
        a = b;
    }
    CHECK(memcmp(&a, &g_s2, sizeof a) == 0);
    /* One ulp in one input value: different input digest and final state. */
    uint8_t d1[32], d2[32];
    CHECK_ST(omega_res_input_digest(&in, d1), OMEGA_RES_OK);
    g_tab[7] = nextafterf(g_tab[7], 1.0f);
    CHECK_ST(omega_res_input_digest(&in, d2), OMEGA_RES_OK);
    CHECK(memcmp(d1, d2, 32) != 0);
    CHECK(memcmp(d1, r1.input_digest, 32) == 0);
    /* A one-ulp current change can vanish in rounding; 1 % cannot. */
    g_tab[7] = g_tab[7] * 1.01f + 1e-12f;
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, (uint64_t)(knots - 1) * 10u), OMEGA_RES_OK);
    CHECK(memcmp(g_s.v, g_s2.v, sizeof g_s.v) != 0);
    OmegaResRunReceipt r3;
    CHECK_ST(omega_res_run_receipt(&g_cfg, &in, &g_s, &r3), OMEGA_RES_OK);
    CHECK(memcmp(r3.run_id, r1.run_id, 32) != 0 && memcmp(r3.config_id, r1.config_id, 32) == 0);
}

/* ------------------------------------------------------------- refusals */

static void test_refusals(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    omega_res_topology_chain(3, 2, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 3, &p);
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_N0, 0.0f, &g_s), OMEGA_RES_OK);
    const float cur[3] = {1e-10f, 0.0f, 0.0f};
    fill_const(g_tab, 2, 3, cur);
    OmegaResInput in = {3, 2, g_tab};
    OmegaResState keep = g_s, nx;

    /* Past the last knot: 2 knots support 10 steps. */
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 25), OMEGA_RES_ERR_INPUT);
    CHECK(g_s.step == 10);
    g_s = keep;
    /* NaN and out-of-bound currents; the state is left as it was. */
    g_tab[1] = (float)NAN;
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 1), OMEGA_RES_ERR_NONFINITE);
    CHECK(memcmp(&g_s, &keep, sizeof g_s) == 0);
    g_tab[1] = 2e-6f;
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_INPUT);
    g_tab[1] = 0.0f;
    OmegaResInput bad_in = {2, 2, g_tab}, one_knot = {3, 1, g_tab}, no_tab = {3, 2, NULL};
    CHECK_ST(omega_res_step(&g_cfg, &bad_in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_INPUT);
    CHECK_ST(omega_res_step(&g_cfg, &one_knot, &g_ws, &g_s, &nx), OMEGA_RES_ERR_INPUT);
    CHECK_ST(omega_res_step(&g_cfg, &no_tab, &g_ws, &g_s, &nx), OMEGA_RES_ERR_NULL);
    /* Corrupted states. */
    g_s.v[0] = (float)NAN;
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_NONFINITE);
    g_s = keep; g_s.v[2] = 1e-3f; /* ground not at zero */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s = keep; g_s.v[0] = 1.5f; /* membrane 0-1 at 1.5 V */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s = keep; g_s.y[1] = 100.0f;
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s = keep; g_s.yc[1] = 1e-3f; /* carry larger than half an ulp of its value */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s = keep; g_s.vc[2] = 1e-30f; /* any carry at a ground */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s = keep; g_s.vc[0] = (float)INFINITY;
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_NONFINITE);
    g_s = keep; g_s.config_id[5] ^= 1u;
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_MISMATCH);
    g_s = keep; g_s.side_count = 2;
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_MISMATCH);
    g_s = keep;
    /* Aliasing and NULL. */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &g_s), OMEGA_RES_ERR_ALIAS);
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_ws.spare, &nx), OMEGA_RES_OK); /* spare is not scratch */
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_ws.spare, 1), OMEGA_RES_ERR_ALIAS);
    CHECK_ST(omega_res_step(NULL, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_NULL);
    CHECK_ST(omega_res_advance(&g_cfg, NULL, &g_ws, &g_s, 1), OMEGA_RES_ERR_NULL);
    CHECK_ST(omega_res_state_init(&g_cfg, (OmegaResInit)9, 0.0f, &g_s2), OMEGA_RES_ERR_PARAM);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, -1.0f, &g_s2), OMEGA_RES_ERR_PARAM);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1e-38f, &g_s2), OMEGA_RES_ERR_RANGE);
    CHECK(memcmp(&g_s, &keep, sizeof g_s) == 0);

    /* Stiffness: N = e^70 on every side gives lambda >= G_u h_m 2 e^70 / eps
     * ~ 1e26 /s, far above 2 / h. */
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, (float)exp(70.0), &g_s), OMEGA_RES_OK);
    float lam;
    CHECK_ST(omega_res_stiffness(&g_cfg, &g_s, &lam), OMEGA_RES_OK);
    CHECK(lam > 1e25f);
    keep = g_s;
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &g_s, 1), OMEGA_RES_ERR_STIFF);
    CHECK(memcmp(&g_s, &keep, sizeof g_s) == 0);

    /* Overflow of the model's voltage range: 1 uA into a node whose
     * membranes neither conduct nor adapt charges it at 9e3 V/s; the run
     * stops at the first step past 1 V and keeps the last valid state. */
    omega_res_topology_chain(2, 1, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 2, &p);
    p.rate_scale = 1e-12f;
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1e-30f, &g_s), OMEGA_RES_OK);
    const float big[2] = {1e-6f, 0.0f};
    fill_const(g_tab, 101, 2, big);
    OmegaResInput inb = {2, 101, g_tab};
    CHECK_ST(omega_res_advance(&g_cfg, &inb, &g_ws, &g_s, 1000), OMEGA_RES_ERR_RANGE);
    CHECK(g_s.v[0] <= 1.0f && g_s.v[0] > 0.99f && g_s.step == 11); /* 9.04 V/s * 1e-5 s per step */
}


/* Voltage guard on membranes, not nodes (orchestrator decision 2026-10-14,
 * RESERVOIR_REFERENCE.md section 12). A 30-node chain grounded at node 29
 * whose membranes neither conduct nor adapt is a series of equal
 * capacitors: 10 nA into node 0 charges every membrane at I / C = 90.4 V/s
 * and node 0 at 29 times that. Node 0 passes 1 V after about 38 steps,
 * where the earlier node-voltage guard refused; the run must go on, flag
 * every membrane once it passes 0.3 V, and refuse only when a membrane
 * passes 1 V (about step 1106). */
static float max_membrane(const OmegaResConfig *c, const OmegaResState *s)
{
    float m = 0.0f;
    for (uint32_t e = 0; e < c->topo.edge_count; e++) {
        float d = s->v[c->topo.edge[e].a] - s->v[c->topo.edge[e].b];
        if (d < 0.0f) d = -d;
        if (d > m) m = d;
    }
    return m;
}

static void test_membrane_guard(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    OmegaResIntegrator ig = integ(1e-5f, 10);
    CHECK_ST(omega_res_topology_chain(30, 29, &t), OMEGA_RES_OK);
    CHECK_ST(omega_res_params_reference(OMEGA_RES_REF_NARMA, 30, &p), OMEGA_RES_OK);
    p.rate_scale = 1e-12f;
    CHECK_ST(omega_res_config_build(&t, &p, &ig, &g_cfg), OMEGA_RES_OK);
    CHECK_ST(omega_res_state_init(&g_cfg, OMEGA_RES_INIT_CONST, 1e-30f, &g_s), OMEGA_RES_OK);
    const OmegaResState rest = g_s;
    float cur[30] = {0};
    cur[0] = 1e-8f;
    fill_const(g_tab, 201, 30, cur);
    OmegaResInput in = {30, 201, g_tab};
    OmegaResMembraneStats ms;
    omega_res_membrane_stats_init(&ms);

    /* Node voltages far above 1 V, every membrane below 0.3 V: runs, no flag. */
    CHECK_ST(omega_res_advance_observed(&g_cfg, &in, &g_ws, &g_s, 300, &ms), OMEGA_RES_OK);
    CHECK(g_s.step == 300);
    CHECK(g_s.v[0] > 7.5f && g_s.v[0] < 8.2f);              /* 29 * 90.4 V/s * 3 ms = 7.86 V */
    CHECK(max_membrane(&g_cfg, &g_s) > 0.26f && max_membrane(&g_cfg, &g_s) < 0.28f);
    CHECK(ms.states_observed == 300 && ms.states_implausible == 0 && ms.membranes_implausible == 0);
    CHECK(ms.max_node_abs_v == g_s.v[0] && ms.max_membrane_abs_v == max_membrane(&g_cfg, &g_s));

    /* Past 0.3 V every membrane is flagged (recorded, not refused); past 1 V
     * the step is refused and the last accepted state is kept. */
    CHECK_ST(omega_res_advance_observed(&g_cfg, &in, &g_ws, &g_s, 1900, &ms), OMEGA_RES_ERR_RANGE);
    CHECK(g_s.step > 1090 && g_s.step < 1120);
    CHECK(ms.states_observed == g_s.step);
    CHECK(ms.membranes_implausible == 29);
    CHECK(ms.states_implausible > 700 && ms.states_implausible < ms.states_observed);
    CHECK(ms.max_membrane_abs_v <= OMEGA_RES_VM_LIMIT && ms.max_membrane_abs_v > 0.99f);
    CHECK(g_s.v[0] > 28.0f);
    /* Without the observer the same run stops at the same step. */
    OmegaResState plain = rest;
    CHECK_ST(omega_res_advance(&g_cfg, &in, &g_ws, &plain, 1900), OMEGA_RES_ERR_RANGE);
    CHECK(plain.step == g_s.step && memcmp(plain.v, g_s.v, sizeof plain.v) == 0);

    /* Hand-set states: a 0.09 V ramp (node 0 at 2.61 V) steps; one membrane
     * over 1 V with every node below 2.8 V is refused. */
    OmegaResState nx;
    g_s = rest;
    for (uint32_t i = 0; i < 29; i++) g_s.v[i] = 0.09f * (float)(29u - i);
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_OK);
    g_s.v[10] += 1.05f; /* membranes 9-10 and 10-11 at 0.96 V and 1.14 V */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s.v[10] -= 1.05f;
    g_s.v[28] = 1.2f; /* the membrane to the ground */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    /* The bound is on |V_a - V_b|: a negative membrane voltage is refused too
     * (catches a one-sided guard dv > limit). */
    g_s = rest;
    g_s.v[0] = -1.5f; /* membrane 0-1 at -1.5 V */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);
    g_s = rest;
    g_s.v[28] = -1.2f; /* the membrane to the ground, negative */
    CHECK_ST(omega_res_step(&g_cfg, &in, &g_ws, &g_s, &nx), OMEGA_RES_ERR_RANGE);

    /* Observer refusals. */
    CHECK_ST(omega_res_membrane_observe(&g_cfg, &rest, NULL), OMEGA_RES_ERR_NULL);
    g_s = rest;
    g_s.config_id[0] ^= 1u;
    CHECK_ST(omega_res_membrane_observe(&g_cfg, &g_s, &ms), OMEGA_RES_ERR_MISMATCH);
    g_s = rest;
    CHECK_ST(omega_res_advance_observed(&g_cfg, &in, &g_ws, &g_s, 1, (OmegaResMembraneStats *)(void *)&g_ws),
             OMEGA_RES_ERR_ALIAS);
}

/* --------------------------------------- agreement with the binary64 oracle */

typedef struct { double max_err, vmax; } cmp_result;

/* Runs the module at (h, K) on the knot table and compares node voltages at
 * every knot with vref (knot-major, as res64_run writes it). */
static int compare_oracle(const OmegaResTopology *t, const OmegaResParams *p, float h, uint32_t K, OmegaResInit init,
                          float init_value, uint32_t knots, const double *vref, cmp_result *out)
{
    OmegaResIntegrator ig = integ(h, K);
    if (omega_res_config_build(t, p, &ig, &g_cfg) != OMEGA_RES_OK) return -1;
    if (omega_res_state_init(&g_cfg, init, init_value, &g_s) != OMEGA_RES_OK) return -1;
    const uint32_t n = t->node_count;
    OmegaResInput in = {n, knots, g_tab};
    out->max_err = 0.0;
    out->vmax = 0.0;
    for (uint32_t k = 1; k < knots; k++) {
        if (omega_res_advance(&g_cfg, &in, &g_ws, &g_s, K) != OMEGA_RES_OK) return -1;
        for (uint32_t i = 0; i < n; i++) {
            const double r = vref[(size_t)k * n + i], e = fabs((double)g_s.v[i] - r);
            if (e > out->max_err) out->max_err = e;
            if (fabs(r) > out->vmax) out->vmax = fabs(r);
        }
    }
    return 0;
}

static double g_vtight[MAX_KNOTS * OMEGA_RES_MAX_NODES];

/* Gate 1 agreement contract (declared in RESERVOIR_REFERENCE.md): at every
 * knot of every scenario and for every step size from 1e-4 s down to
 * 5e-6 s, |V_omega - V_oracle(rtol 1e-10)| <= 1e-6 max|V|. The oracle at
 * the reference's own tolerances (rtol 1e-6, atol 1e-8) is reported beside
 * it for the convergence record. */
#define AGREE_REL 1e-6

static void oracle_case(const char *name, const OmegaResTopology *t, const OmegaResParams *p, OmegaResInit init,
                        float init_value, uint32_t knots)
{
    static const float hs[5] = {1e-4f, 5e-5f, 2e-5f, 1e-5f, 5e-6f};
    static const uint32_t ks[5] = {1, 2, 5, 10, 20};
    cmp_result r[5], reftol = {0.0, 0.0};
    double ninit[OMEGA_RES_MAX_SIDES];
    for (uint32_t e = 0; e < t->edge_count; e++)
        for (int side = 0; side < 2; side++) {
            uint32_t o = side ? t->edge[e].b : t->edge[e].a;
            ninit[2 * e + (uint32_t)side] = init == OMEGA_RES_INIT_N0
                ? (double)p->n0_scale * exp((double)p->n0_rate * (double)p->conc_um[o]) : (double)init_value;
        }
    res64_opts ot = {1e-10, 1e-14, 0, 0}, orf = {1e-6, 1e-8, 0, 0};
    CHECK(res64_run(t, p, ninit, g_tab, knots, 1e-4, &ot, g_vtight, NULL) == 0);
    CHECK(res64_run(t, p, ninit, g_tab, knots, 1e-4, &orf, g_vref, NULL) == 0);
    for (size_t q = (size_t)t->node_count; q < (size_t)knots * t->node_count; q++) {
        double e = fabs(g_vref[q] - g_vtight[q]);
        if (e > reftol.max_err) reftol.max_err = e;
        if (fabs(g_vtight[q]) > reftol.vmax) reftol.vmax = fabs(g_vtight[q]);
    }
    for (int c = 0; c < 5; c++) CHECK(compare_oracle(t, p, hs[c], ks[c], init, init_value, knots, g_vtight, &r[c]) == 0);
    printf("oracle %-26s max|V| %.4e V; max|V err| fp32 rk4 h=1e-4 %.2e, 5e-5 %.2e, 2e-5 %.2e, 1e-5 %.2e,"
           " 5e-6 %.2e; binary64 at ref tol %.2e (%lu steps tight)\n", name, r[3].vmax, r[0].max_err,
           r[1].max_err, r[2].max_err, r[3].max_err, r[4].max_err, reftol.max_err, ot.steps_accepted);
    CHECK(r[3].vmax > 0.0);
    for (int c = 0; c < 5; c++) CHECK(r[c].max_err <= AGREE_REL * r[3].vmax);
    CHECK(reftol.max_err <= 1e-9 * reftol.vmax); /* the reference's tolerances suffice */
}

static void test_oracle_agreement(void)
{
    OmegaResTopology t;
    OmegaResParams p;
    /* (a) one membrane, constant 0.1 nA, 200 ms */
    omega_res_topology_chain(2, 1, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 2, &p);
    const float c2[2] = {1e-10f, 0.0f};
    fill_const(g_tab, 2001, 2, c2);
    oracle_case("one-node const 0.1nA", &t, &p, OMEGA_RES_INIT_N0, 0.0f, 2001);
    /* (b) 5-node chain, NARMA-style masked drive (mask 2, 0.1 nA) */
    omega_res_topology_chain(5, 4, &t);
    omega_res_params_reference(OMEGA_RES_REF_NARMA, 5, &p);
    g_rng = 7;
    uint32_t knots = fill_masked(g_tab, 5, ORACLE_SAMPLES, 2, 1e-10f);
    oracle_case("chain5 masked NARMA-style", &t, &p, OMEGA_RES_INIT_N0, 0.0f, knots);
    /* (c) 13-node reference lattice, Lorenz constants, N(0) = 1, smooth
     * drive (I = (x * 1.05e-10 + 1e-9) / 10 with x a 20 Hz sine of
     * amplitude 15) at the three injection nodes. */
    CHECK_ST(fixture_topology(RES_FIX_HEX13_NB, 13, RES_FIX_HEX13_GROUND, &t), OMEGA_RES_OK);
    omega_res_params_reference(OMEGA_RES_REF_LORENZ, 13, &p);
    knots = 10u * ORACLE_SAMPLES + 1u;
    memset(g_tab, 0, sizeof(float) * 13 * knots);
    for (uint32_t k = 0; k < knots; k++)
        for (int q = 0; q < 3; q++) {
            double x = 15.0 * sin(2.0 * M_PI * 20.0 * k * 1e-4 + q);
            g_tab[k * 13 + RES_FIX_HEX13_INPUTS[q] - 1] = (float)((x * 1.05e-10 + 1e-9) / 10.0);
        }
    oracle_case("hex13 Lorenz-style", &t, &p, OMEGA_RES_INIT_CONST, 1.0f, knots);
}

/* -------------------------------------------------------------- readout */

static OmegaResReadoutWorkspace g_rws;

static void test_readout(void)
{
    enum { NS = 60, NF = 3 };
    static double x[NS * NF], y[NS * 2];
    g_rng = 99;
    for (int s = 0; s < NS; s++) {
        for (int f = 0; f < NF; f++) x[s * NF + f] = runit() * 2.0 - 1.0;
        y[s * 2 + 0] = 0.5 + 2.0 * x[s * NF] - 1.0 * x[s * NF + 1] + 0.25 * x[s * NF + 2];
        y[s * 2 + 1] = -3.0 + 0.125 * x[s * NF + 1];
    }
    OmegaResReadout r;
    CHECK_ST(omega_res_readout_fit(x, y, NS, NF, 2, 0.0, &g_rws, &r), OMEGA_RES_OK);
    const double want0[4] = {0.5, 2.0, -1.0, 0.25}, want1[4] = {-3.0, 0.0, 0.125, 0.0};
    for (int i = 0; i < 4; i++) {
        NEARABS(r.w[i * OMEGA_RES_READOUT_MAX_OUTPUTS + 0], want0[i], 1e-12);
        NEARABS(r.w[i * OMEGA_RES_READOUT_MAX_OUTPUTS + 1], want1[i], 1e-12);
    }
    double out[2];
    CHECK_ST(omega_res_readout_apply(&r, &x[5 * NF], out), OMEGA_RES_OK);
    NEARABS(out[0], y[10], 1e-12);
    /* Very large lambda: weights -> 0, the unpenalized bias -> mean(y). */
    double mean = 0.0;
    for (int s = 0; s < NS; s++) mean += y[s * 2];
    mean /= NS;
    CHECK_ST(omega_res_readout_fit(x, y, NS, NF, 2, 1e12, &g_rws, &r), OMEGA_RES_OK);
    NEARABS(r.w[0], mean, 1e-6);
    NEARABS(r.w[1 * OMEGA_RES_READOUT_MAX_OUTPUTS], 0.0, 1e-9);
    CHECK(r.lambda == 1e12);
    /* Duplicate feature column with lambda = 0: singular. With lambda > 0: fine. */
    static double xd[NS * 2];
    for (int s = 0; s < NS; s++) { xd[s * 2] = x[s * NF]; xd[s * 2 + 1] = x[s * NF]; }
    CHECK_ST(omega_res_readout_fit(xd, y, NS, 2, 2, 0.0, &g_rws, &r), OMEGA_RES_ERR_SINGULAR);
    CHECK_ST(omega_res_readout_fit(xd, y, NS, 2, 2, 1e-3, &g_rws, &r), OMEGA_RES_OK);
    CHECK_ST(omega_res_readout_fit(x, y, NS, NF, 2, -1.0, &g_rws, &r), OMEGA_RES_ERR_PARAM);
    CHECK_ST(omega_res_readout_fit(x, y, NS, OMEGA_RES_READOUT_MAX_FEATURES + 1, 2, 0.0, &g_rws, &r),
             OMEGA_RES_ERR_PARAM);
    CHECK_ST(omega_res_readout_fit(x, y, NS, NF, 0, 0.0, &g_rws, &r), OMEGA_RES_ERR_PARAM);
    x[7] = NAN;
    CHECK_ST(omega_res_readout_fit(x, y, NS, NF, 2, 0.0, &g_rws, &r), OMEGA_RES_ERR_NONFINITE);
    CHECK_ST(omega_res_readout_apply(&r, &x[6], out), OMEGA_RES_ERR_NONFINITE);
    CHECK_ST(omega_res_readout_fit(NULL, y, NS, NF, 2, 0.0, &g_rws, &r), OMEGA_RES_ERR_NULL);

    /* NRMSE (Text S6): ref 1,2,3,4 and pred 1,2,3,5: sum e^2 = 1, sum
     * (ref - 2.5)^2 = 5, NRMSE = sqrt(1/5) = 0.44721359549995793. */
    const double ref[4] = {1, 2, 3, 4}, pred[4] = {1, 2, 3, 5}, flat[4] = {2.5, 2.5, 2.5, 2.5}, c4[4] = {7, 7, 7, 7};
    double v;
    CHECK_ST(omega_res_nrmse(ref, pred, 4, &v), OMEGA_RES_OK);
    NEAR(v, 0.44721359549995793, 1e-15);
    CHECK_ST(omega_res_nrmse(ref, ref, 4, &v), OMEGA_RES_OK);
    CHECK(v == 0.0);
    CHECK_ST(omega_res_nrmse(ref, flat, 4, &v), OMEGA_RES_OK);
    CHECK(v == 1.0);
    CHECK_ST(omega_res_nrmse(c4, ref, 4, &v), OMEGA_RES_ERR_SINGULAR);
    CHECK_ST(omega_res_nrmse(ref, pred, 1, &v), OMEGA_RES_ERR_PARAM);
    const double withnan[4] = {1, NAN, 3, 4};
    CHECK_ST(omega_res_nrmse(withnan, pred, 4, &v), OMEGA_RES_ERR_NONFINITE);
}

int main(void)
{
    test_topology();
    test_reference_lattices();
    test_params();
    test_identity();
    test_zero_input_fixed_point();
    test_decay();
    test_charging();
    test_saturation();
    test_connected_and_disconnected();
    test_reproducibility();
    test_refusals();
    test_membrane_guard();
    test_oracle_agreement();
    test_readout();
    printf("test_reservoir: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail) { printf("test_reservoir: FAIL\n"); return 1; }
    printf("test_reservoir: PASS\n");
    return 0;
}
