/* Omega ionic-droplet reservoir, CPU reference. The contract (model,
 * precision, update order, refusals) is in omega_reservoir.h; this file
 * follows it line for line. Build with -ffp-contract=off -fno-fast-math. */
#include "omega_reservoir.h"

#include "omega_numeric.h"
#include "sha256.h"

#include <string.h>

/* ------------------------------------------------------------- helpers */

static int fin(float x) { return x == x && x - x == 0.0f; } /* not NaN, not inf */
static float fabs32(float x) { return x < 0.0f ? -x : x; }

/* TwoSum (Knuth): s = fl(x + y) and *err = (x + y) - s exactly, for any
 * magnitudes, in round-to-nearest without contraction. */
static float two_sum(float x, float y, float *err)
{
    const float s = x + y;
    const float bp = s - x;
    const float ap = s - bp;
    *err = (x - ap) + (y - bp);
    return s;
}

/* |carry| <= half an ulp of the value it belongs to: 2^-24 |value|. */
static int carry_ok(float value, float carry) { return fabs32(carry) <= fabs32(value) * 0x1p-24f; }
static uint32_t f32_bits(float x)
{
    uint32_t u;
    memcpy(&u, &x, 4);
    return u == 0x80000000u ? 0u : u; /* -0.0 encodes as +0.0 */
}

/* 1 - e^w, see the header for the declared sequence. */
static float one_minus_exp(float w)
{
    static const float inv_k[11] = {0.0f, 0.0f, 1.0f / 2.0f, 1.0f / 3.0f, 1.0f / 4.0f, 1.0f / 5.0f,
                                    1.0f / 6.0f, 1.0f / 7.0f, 1.0f / 8.0f, 1.0f / 9.0f, 1.0f / 10.0f};
    if (fabs32(w) <= 0.5f) {
        float p = 1.0f;
        for (int k = 10; k >= 2; k--) p = 1.0f + (w * inv_k[k]) * p;
        return -(w * p);
    }
    return 1.0f - omega_math_exp(w);
}

/* ------------------------------------------------------------- topology */

static int edge_less(OmegaResEdge x, OmegaResEdge y) { return x.a < y.a || (x.a == y.a && x.b < y.b); }

OmegaResStatus omega_res_topology_build(uint32_t node_count, const OmegaResEdge *edges, uint32_t edge_count,
                                        const uint32_t *grounds, uint32_t ground_count, OmegaResTopology *out)
{
    if (!edges || !grounds || !out) return OMEGA_RES_ERR_NULL;
    OmegaResTopology t;
    memset(&t, 0, sizeof t);
    if (node_count < 2u || node_count > OMEGA_RES_MAX_NODES) return OMEGA_RES_ERR_TOPOLOGY;
    if (edge_count < 1u || edge_count > OMEGA_RES_MAX_EDGES) return OMEGA_RES_ERR_TOPOLOGY;
    if (ground_count < 1u || ground_count >= node_count) return OMEGA_RES_ERR_TOPOLOGY;
    t.node_count = node_count;
    t.edge_count = edge_count;
    for (uint32_t e = 0; e < edge_count; e++) {
        uint32_t a = edges[e].a, b = edges[e].b;
        if (a >= node_count || b >= node_count || a == b) return OMEGA_RES_ERR_TOPOLOGY;
        OmegaResEdge c = {(uint16_t)(a < b ? a : b), (uint16_t)(a < b ? b : a)};
        uint32_t i = e; /* insertion sort: bounded by OMEGA_RES_MAX_EDGES */
        while (i > 0 && edge_less(c, t.edge[i - 1])) { t.edge[i] = t.edge[i - 1]; i--; }
        t.edge[i] = c;
    }
    for (uint32_t e = 0; e < edge_count; e++) {
        if (e > 0 && t.edge[e].a == t.edge[e - 1].a && t.edge[e].b == t.edge[e - 1].b) return OMEGA_RES_ERR_TOPOLOGY;
        if (++t.degree[t.edge[e].a] > OMEGA_RES_MAX_DEGREE) return OMEGA_RES_ERR_TOPOLOGY;
        if (++t.degree[t.edge[e].b] > OMEGA_RES_MAX_DEGREE) return OMEGA_RES_ERR_TOPOLOGY;
    }
    for (uint32_t i = 0; i < node_count; i++)
        if (t.degree[i] == 0) return OMEGA_RES_ERR_TOPOLOGY; /* isolated: no capacitance */
    for (uint32_t g = 0; g < ground_count; g++) {
        if (grounds[g] >= node_count || t.is_ground[grounds[g]]) return OMEGA_RES_ERR_TOPOLOGY;
        t.is_ground[grounds[g]] = 1;
    }
    /* Every connected component needs a ground, else M is singular. Label
     * components by repeated relaxation (bounded: node_count passes). */
    uint8_t comp[OMEGA_RES_MAX_NODES];
    for (uint32_t i = 0; i < node_count; i++) comp[i] = (uint8_t)i;
    for (uint32_t pass = 0; pass < node_count; pass++) {
        int changed = 0;
        for (uint32_t e = 0; e < edge_count; e++) {
            uint8_t *ca = &comp[t.edge[e].a], *cb = &comp[t.edge[e].b];
            if (*ca < *cb) { *cb = *ca; changed = 1; }
            else if (*cb < *ca) { *ca = *cb; changed = 1; }
        }
        if (!changed) break;
    }
    for (uint32_t i = 0; i < node_count; i++) {
        int grounded = 0;
        for (uint32_t j = 0; j < node_count && !grounded; j++) grounded = comp[j] == comp[i] && t.is_ground[j];
        if (!grounded) return OMEGA_RES_ERR_TOPOLOGY;
    }
    *out = t;
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_topology_chain(uint32_t node_count, uint32_t ground, OmegaResTopology *out)
{
    if (!out) return OMEGA_RES_ERR_NULL;
    if (node_count < 2u || node_count > OMEGA_RES_MAX_NODES) return OMEGA_RES_ERR_TOPOLOGY;
    OmegaResEdge e[OMEGA_RES_MAX_NODES];
    for (uint32_t i = 0; i + 1 < node_count; i++) { e[i].a = (uint16_t)i; e[i].b = (uint16_t)(i + 1); }
    return omega_res_topology_build(node_count, e, node_count - 1, &ground, 1, out);
}

/* ----------------------------------------------------------- parameters */

OmegaResStatus omega_res_params_reference(OmegaResReference which, uint32_t node_count, OmegaResParams *out)
{
    if (!out) return OMEGA_RES_ERR_NULL;
    if (node_count < 1u || node_count > OMEGA_RES_MAX_NODES) return OMEGA_RES_ERR_PARAM;
    if (which != OMEGA_RES_REF_NARMA && which != OMEGA_RES_REF_LORENZ) return OMEGA_RES_ERR_PARAM;
    OmegaResParams p;
    memset(&p, 0, sizeof p);
    p.node_count = node_count;
    p.eps_f_per_m = which == OMEGA_RES_REF_NARMA ? 8.85e-12f : (float)(2.1 * 8.85e-12);
    p.h_m = 4e-9f;
    p.area_m2 = 5e-8f;
    p.g_u_s = 5e-8f;
    p.z = 0.00015f;
    p.rate_scale = 1e4f;
    p.n0_scale = 0.002f;
    p.n0_rate = 3.7919f;
    for (uint32_t i = 0; i < node_count; i++) {
        p.conc_um[i] = (float)(1u + i % 3u);
        p.tau0_s[i] = 0.001f;
        p.v_tau_v[i] = which == OMEGA_RES_REF_NARMA ? 0.045f : 0.0454f;
        p.v_e_v[i] = 0.0055f;
    }
    *out = p;
    return OMEGA_RES_OK;
}

static int pos(float x) { return fin(x) && x > 0.0f; }

static OmegaResStatus params_check(const OmegaResParams *p, uint32_t node_count)
{
    if (p->node_count != node_count) return OMEGA_RES_ERR_PARAM;
    if (!pos(p->eps_f_per_m) || !pos(p->h_m) || !pos(p->area_m2) || !pos(p->g_u_s)) return OMEGA_RES_ERR_PARAM;
    if (!pos(p->z) || p->z > 1.0f || !pos(p->rate_scale) || !pos(p->n0_scale) || !fin(p->n0_rate))
        return OMEGA_RES_ERR_PARAM;
    for (uint32_t i = 0; i < node_count; i++) {
        if (!fin(p->conc_um[i]) || p->conc_um[i] < 0.0f) return OMEGA_RES_ERR_PARAM;
        if (!pos(p->tau0_s[i]) || !pos(p->v_tau_v[i]) || !pos(p->v_e_v[i])) return OMEGA_RES_ERR_PARAM;
    }
    return OMEGA_RES_OK;
}

/* -------------------------------------------------------- configuration */

static void h_u32(sha256_ctx *c, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    sha256_update(c, b, 4);
}
static void h_u64(sha256_ctx *c, uint64_t v)
{
    h_u32(c, (uint32_t)v);
    h_u32(c, (uint32_t)(v >> 32));
}
static void h_u16(sha256_ctx *c, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    sha256_update(c, b, 2);
}
static void h_f32(sha256_ctx *c, float v) { h_u32(c, f32_bits(v)); }
static void h_domain(sha256_ctx *c, const char *domain)
{
    const uint8_t zero = 0;
    sha256_init(c);
    sha256_update(c, (const uint8_t *)domain, strlen(domain));
    sha256_update(c, &zero, 1);
}

static void config_digest(const OmegaResConfig *cfg, uint8_t out[OMEGA_RES_DIGEST_SIZE])
{
    sha256_ctx c;
    const OmegaResTopology *t = &cfg->topo;
    const OmegaResParams *p = &cfg->par;
    h_domain(&c, OMEGA_RES_DOMAIN_CONFIG);
    h_u32(&c, t->node_count);
    h_u32(&c, t->edge_count);
    for (uint32_t e = 0; e < t->edge_count; e++) { h_u16(&c, t->edge[e].a); h_u16(&c, t->edge[e].b); }
    sha256_update(&c, t->is_ground, t->node_count);
    h_f32(&c, p->eps_f_per_m); h_f32(&c, p->h_m); h_f32(&c, p->area_m2); h_f32(&c, p->g_u_s);
    h_f32(&c, p->z); h_f32(&c, p->rate_scale); h_f32(&c, p->n0_scale); h_f32(&c, p->n0_rate);
    for (uint32_t i = 0; i < t->node_count; i++) {
        h_f32(&c, p->conc_um[i]); h_f32(&c, p->tau0_s[i]); h_f32(&c, p->v_tau_v[i]); h_f32(&c, p->v_e_v[i]);
    }
    h_f32(&c, cfg->integ.step_s);
    h_u32(&c, cfg->integ.steps_per_knot);
    sha256_update(&c, (const uint8_t *)OMEGA_RES_NUMERIC_POLICY, strlen(OMEGA_RES_NUMERIC_POLICY));
    sha256_final(&c, out);
}

OmegaResStatus omega_res_config_build(const OmegaResTopology *topo, const OmegaResParams *par,
                                      const OmegaResIntegrator *integ, OmegaResConfig *out)
{
    if (!topo || !par || !integ || !out) return OMEGA_RES_ERR_NULL;
    if (!omega_numeric_fpenv_ok()) return OMEGA_RES_ERR_FPENV;
    /* Re-derive the topology from its own edges and grounds: only a
     * canonical, valid topology is accepted, whoever filled the struct. */
    uint32_t grounds[OMEGA_RES_MAX_NODES], ng = 0;
    if (topo->node_count < 2u || topo->node_count > OMEGA_RES_MAX_NODES) return OMEGA_RES_ERR_TOPOLOGY;
    for (uint32_t i = 0; i < topo->node_count; i++)
        if (topo->is_ground[i]) grounds[ng++] = i;
    OmegaResTopology canon;
    OmegaResStatus st = omega_res_topology_build(topo->node_count, topo->edge, topo->edge_count, grounds, ng, &canon);
    if (st != OMEGA_RES_OK) return st;
    if (memcmp(&canon, topo, sizeof canon) != 0) return OMEGA_RES_ERR_TOPOLOGY;
    st = params_check(par, topo->node_count);
    if (st != OMEGA_RES_OK) return st;
    if (!pos(integ->step_s) || integ->step_s > OMEGA_RES_MAX_STEP_S) return OMEGA_RES_ERR_PARAM;
    if (integ->steps_per_knot < 1u || integ->steps_per_knot > (1u << 20)) return OMEGA_RES_ERR_PARAM;

    OmegaResConfig c;
    memset(&c, 0, sizeof c);
    c.topo = canon;
    c.par = *par;
    c.integ = *integ;
    const uint32_t n = canon.node_count;
    float cap = omega_math_div(par->eps_f_per_m * par->area_m2, par->h_m);
    c.inv_cap = omega_math_div(1.0f, cap);
    c.g_unit = par->g_u_s * par->area_m2;
    c.lambda_unit = omega_math_div(par->g_u_s * par->h_m, par->eps_f_per_m);
    c.half_step = integ->step_s * 0.5f;
    c.sixth_step = omega_math_div(integ->step_s, 6.0f);
    if (!pos(cap) || !pos(c.inv_cap) || !pos(c.g_unit) || !pos(c.lambda_unit) || !pos(c.sixth_step))
        return OMEGA_RES_ERR_PARAM;
    const float ln_n0_scale = omega_math_log(par->n0_scale);
    for (uint32_t i = 0; i < n; i++) {
        c.ln_n0[i] = ln_n0_scale + par->n0_rate * par->conc_um[i];
        c.ln_alpha0[i] = omega_math_log(omega_math_div(par->rate_scale, par->tau0_s[i]));
        c.inv_v_tau[i] = omega_math_div(1.0f, par->v_tau_v[i]);
        c.inv_v_e[i] = omega_math_div(1.0f, par->v_e_v[i]);
        if (!fin(c.ln_n0[i]) || c.ln_n0[i] < OMEGA_RES_Y_MIN || c.ln_n0[i] > OMEGA_RES_Y_MAX) return OMEGA_RES_ERR_PARAM;
        if (!fin(c.ln_alpha0[i]) || !pos(c.inv_v_tau[i]) || !pos(c.inv_v_e[i])) return OMEGA_RES_ERR_PARAM;
    }
    /* Reduced Laplacian (integer entries) and its Cholesky factor R. */
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        c.reduced_index[i] = -1;
        if (!canon.is_ground[i]) { c.reduced_index[i] = (int16_t)m; c.reduced_node[m] = (uint16_t)i; m++; }
    }
    c.reduced_count = m;
    float *R = c.chol; /* holds the Laplacian, overwritten in place by R */
    for (uint32_t u = 0; u < m; u++) R[u * OMEGA_RES_MAX_NODES + u] = (float)canon.degree[c.reduced_node[u]];
    for (uint32_t e = 0; e < canon.edge_count; e++) {
        int ra = c.reduced_index[canon.edge[e].a], rb = c.reduced_index[canon.edge[e].b];
        if (ra < 0 || rb < 0) continue;
        int hi = ra > rb ? ra : rb, lo = ra > rb ? rb : ra;
        R[(uint32_t)hi * OMEGA_RES_MAX_NODES + (uint32_t)lo] = -1.0f;
    }
    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j <= i; j++) {
            float s = R[i * OMEGA_RES_MAX_NODES + j];
            for (uint32_t k = 0; k < j; k++) s = s - R[i * OMEGA_RES_MAX_NODES + k] * R[j * OMEGA_RES_MAX_NODES + k];
            if (i == j) {
                if (!(s > 0.0f)) return OMEGA_RES_ERR_SINGULAR;
                R[i * OMEGA_RES_MAX_NODES + i] = omega_math_sqrt(s);
                c.chol_inv_diag[i] = omega_math_div(1.0f, R[i * OMEGA_RES_MAX_NODES + i]);
            } else {
                R[i * OMEGA_RES_MAX_NODES + j] = s * c.chol_inv_diag[j];
            }
        }
    }
    config_digest(&c, c.id);
    *out = c;
    return OMEGA_RES_OK;
}

const uint8_t *omega_res_config_id(const OmegaResConfig *cfg) { return cfg ? cfg->id : NULL; }

/* ---------------------------------------------------------------- state */

OmegaResStatus omega_res_state_init(const OmegaResConfig *cfg, OmegaResInit kind, float value, OmegaResState *out)
{
    if (!cfg || !out) return OMEGA_RES_ERR_NULL;
    OmegaResState s;
    memset(&s, 0, sizeof s);
    memcpy(s.config_id, cfg->id, OMEGA_RES_DIGEST_SIZE);
    s.node_count = cfg->topo.node_count;
    s.side_count = 2u * cfg->topo.edge_count;
    float y_const = 0.0f;
    if (kind == OMEGA_RES_INIT_CONST) {
        if (!pos(value)) return OMEGA_RES_ERR_PARAM;
        y_const = omega_math_log(value);
        if (!fin(y_const) || y_const < OMEGA_RES_Y_MIN || y_const > OMEGA_RES_Y_MAX) return OMEGA_RES_ERR_RANGE;
    } else if (kind != OMEGA_RES_INIT_N0) {
        return OMEGA_RES_ERR_PARAM;
    }
    for (uint32_t e = 0; e < cfg->topo.edge_count; e++) {
        s.y[2 * e] = kind == OMEGA_RES_INIT_N0 ? cfg->ln_n0[cfg->topo.edge[e].a] : y_const;
        s.y[2 * e + 1] = kind == OMEGA_RES_INIT_N0 ? cfg->ln_n0[cfg->topo.edge[e].b] : y_const;
    }
    *out = s;
    return OMEGA_RES_OK;
}

float omega_res_state_pore(const OmegaResState *s, uint32_t side)
{
    if (!s || side >= s->side_count || side >= OMEGA_RES_MAX_SIDES) {
        const uint32_t qnan = OMEGA_QNAN_BITS;
        float f;
        memcpy(&f, &qnan, 4);
        return f; /* no such side: NaN, never a plausible density */
    }
    return omega_math_exp(s->y[side]);
}

/* Every value finite and in its declared bounds; grounds exactly zero. */
static OmegaResStatus state_check(const OmegaResConfig *cfg, const OmegaResState *s)
{
    if (memcmp(s->config_id, cfg->id, OMEGA_RES_DIGEST_SIZE) != 0) return OMEGA_RES_ERR_MISMATCH;
    if (s->node_count != cfg->topo.node_count || s->side_count != 2u * cfg->topo.edge_count)
        return OMEGA_RES_ERR_MISMATCH;
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (!fin(s->v[i]) || !fin(s->vc[i])) return OMEGA_RES_ERR_NONFINITE;
        if ((cfg->topo.is_ground[i] && s->v[i] != 0.0f) || !carry_ok(s->v[i], s->vc[i])) return OMEGA_RES_ERR_RANGE;
    }
    /* The voltage guard is on each membrane; node voltages are finite only. */
    for (uint32_t e = 0; e < cfg->topo.edge_count; e++) {
        const float dv = s->v[cfg->topo.edge[e].a] - s->v[cfg->topo.edge[e].b];
        if (!(fabs32(dv) <= OMEGA_RES_VM_LIMIT)) return OMEGA_RES_ERR_RANGE; /* also an overflowed difference */
    }
    for (uint32_t k = 0; k < s->side_count; k++) {
        if (!fin(s->y[k]) || !fin(s->yc[k])) return OMEGA_RES_ERR_NONFINITE;
        if (s->y[k] < OMEGA_RES_Y_MIN || s->y[k] > OMEGA_RES_Y_MAX || !carry_ok(s->y[k], s->yc[k]))
            return OMEGA_RES_ERR_RANGE;
    }
    return OMEGA_RES_OK;
}

/* ------------------------------------------------------------ dynamics */

/* y' of the side owned by node i with dv = V_i - V_other; *lam = alpha Z e^w. */
static float side_rate(const OmegaResConfig *cfg, uint32_t i, float dv, float y, float *lam)
{
    float alpha = omega_math_exp(cfg->ln_alpha0[i] - dv * cfg->inv_v_tau[i]);
    float ln_nss = cfg->ln_n0[i] + dv * cfg->inv_v_e[i];
    float w = cfg->par.z * (y - ln_nss);
    float r = one_minus_exp(w);
    if (lam) *lam = (alpha * cfg->par.z) * (1.0f - r);
    return alpha * r;
}

/* f(V, y) at injected currents cur (header: "Order inside one derivative
 * evaluation"). lam is NULL except at stage 1. */
static void derivative(const OmegaResConfig *cfg, OmegaResWorkspace *ws, const float *v, const float *y,
                       const float *cur, float *dv_out, float *dy_out, float *lam)
{
    const OmegaResTopology *t = &cfg->topo;
    float *b = ws->b, *r = ws->r;
    float lmax = 0.0f;
    for (uint32_t i = 0; i < t->node_count; i++) b[i] = cur[i];
    for (uint32_t e = 0; e < t->edge_count; e++) {
        const uint32_t na = t->edge[e].a, nb = t->edge[e].b;
        const float dv = v[na] - v[nb];
        float la = 0.0f, lb = 0.0f;
        dy_out[2 * e] = side_rate(cfg, na, dv, y[2 * e], lam ? &la : NULL);
        dy_out[2 * e + 1] = side_rate(cfg, nb, -dv, y[2 * e + 1], lam ? &lb : NULL);
        const float pores = omega_math_exp(y[2 * e]) + omega_math_exp(y[2 * e + 1]);
        const float q = (cfg->g_unit * pores) * dv;
        b[na] = b[na] - q;
        b[nb] = b[nb] + q;
        if (lam) {
            float lv = cfg->lambda_unit * pores;
            /* written so that NaN propagates into lmax */
            if (!(la <= lmax)) lmax = la;
            if (!(lb <= lmax)) lmax = lb;
            if (!(lv <= lmax)) lmax = lv;
        }
    }
    const uint32_t m = cfg->reduced_count;
    const float *R = cfg->chol;
    for (uint32_t u = 0; u < m; u++) r[u] = b[cfg->reduced_node[u]] * cfg->inv_cap;
    for (uint32_t i = 0; i < m; i++) { /* forward: R z = r (z overwrites r) */
        float s = r[i];
        for (uint32_t k = 0; k < i; k++) s = s - R[i * OMEGA_RES_MAX_NODES + k] * r[k];
        r[i] = s * cfg->chol_inv_diag[i];
    }
    for (uint32_t ii = m; ii-- > 0;) { /* backward: R^T x = z (x overwrites z) */
        float s = r[ii];
        for (uint32_t k = ii + 1; k < m; k++) s = s - R[k * OMEGA_RES_MAX_NODES + ii] * r[k];
        r[ii] = s * cfg->chol_inv_diag[ii];
    }
    for (uint32_t i = 0; i < t->node_count; i++) {
        int u = cfg->reduced_index[i];
        dv_out[i] = u < 0 ? 0.0f : r[u];
    }
    if (lam) *lam = lmax;
}

OmegaResStatus omega_res_stiffness(const OmegaResConfig *cfg, const OmegaResState *s, float *lambda)
{
    if (!cfg || !s || !lambda) return OMEGA_RES_ERR_NULL;
    OmegaResStatus st = state_check(cfg, s);
    if (st != OMEGA_RES_OK) return st;
    float lmax = 0.0f;
    for (uint32_t e = 0; e < cfg->topo.edge_count; e++) {
        const uint32_t na = cfg->topo.edge[e].a, nb = cfg->topo.edge[e].b;
        const float dv = s->v[na] - s->v[nb];
        float la, lb;
        (void)side_rate(cfg, na, dv, s->y[2 * e], &la);
        (void)side_rate(cfg, nb, -dv, s->y[2 * e + 1], &lb);
        float lv = cfg->lambda_unit * (omega_math_exp(s->y[2 * e]) + omega_math_exp(s->y[2 * e + 1]));
        if (!(la <= lmax)) lmax = la;
        if (!(lb <= lmax)) lmax = lb;
        if (!(lv <= lmax)) lmax = lv;
    }
    if (!fin(lmax)) return OMEGA_RES_ERR_NONFINITE;
    *lambda = lmax;
    return OMEGA_RES_OK;
}

/* ---------------------------------------------------------------- input */

static OmegaResStatus knot_check(const OmegaResInput *in, uint32_t k)
{
    const float *row = in->current_a + (size_t)k * in->node_count;
    for (uint32_t i = 0; i < in->node_count; i++) {
        if (!fin(row[i])) return OMEGA_RES_ERR_NONFINITE;
        if (fabs32(row[i]) > OMEGA_RES_MAX_CURRENT_A) return OMEGA_RES_ERR_INPUT;
    }
    return OMEGA_RES_OK;
}

/* Current at stage time t_n + (half_steps / 2) h, half_steps in {0, 1, 2}. */
static void input_at(const OmegaResInput *in, uint32_t k, uint32_t phase, uint32_t K, uint32_t half_steps, float *cur)
{
    const float f = omega_math_div((float)(2u * phase + half_steps), (float)(2u * K));
    const float *i0 = in->current_a + (size_t)k * in->node_count;
    const float *i1 = i0 + in->node_count;
    for (uint32_t i = 0; i < in->node_count; i++) cur[i] = i0[i] + f * (i1[i] - i0[i]);
}

/* ------------------------------------------------------------- stepping */

static int overlaps(const void *p, size_t np, const void *q, size_t nq)
{
    const uint8_t *a = p, *b = q;
    return a < b + nq && b < a + np;
}

/* The scratch part of the workspace is everything before `spare`. */
#define SCRATCH_BYTES offsetof(OmegaResWorkspace, spare)

/* One step, pointers already checked for NULL and aliasing. */
static OmegaResStatus step_impl(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                                const OmegaResState *cur, OmegaResState *next)
{
    if (!in->current_a) return OMEGA_RES_ERR_NULL;
    if (!omega_numeric_fpenv_ok()) return OMEGA_RES_ERR_FPENV;
    OmegaResStatus st = state_check(cfg, cur);
    if (st != OMEGA_RES_OK) return st;
    if (in->node_count != cfg->topo.node_count || in->knot_count < 2u) return OMEGA_RES_ERR_INPUT;
    const uint32_t K = cfg->integ.steps_per_knot;
    const uint64_t k64 = cur->step / K;
    if (k64 + 1u >= in->knot_count) return OMEGA_RES_ERR_INPUT; /* past the last knot */
    const uint32_t k = (uint32_t)k64, phase = (uint32_t)(cur->step % K);
    if ((st = knot_check(in, k)) != OMEGA_RES_OK || (st = knot_check(in, k + 1u)) != OMEGA_RES_OK) return st;

    const uint32_t n = cfg->topo.node_count, ns = cur->side_count;
    const float h = cfg->integ.step_s, h2 = cfg->half_step, h6 = cfg->sixth_step;
    float lam = 0.0f;
    input_at(in, k, phase, K, 0u, ws->cur);
    derivative(cfg, ws, cur->v, cur->y, ws->cur, ws->kv[0], ws->ky[0], &lam);
    if (!(h * lam <= OMEGA_RES_STABILITY_LIMIT)) return fin(lam) ? OMEGA_RES_ERR_STIFF : OMEGA_RES_ERR_NONFINITE;
    for (int s = 1; s < 4; s++) {
        const float c = s == 3 ? h : h2;
        for (uint32_t i = 0; i < n; i++) ws->sv[i] = cur->v[i] + c * ws->kv[s - 1][i];
        for (uint32_t j = 0; j < ns; j++) ws->sy[j] = cur->y[j] + c * ws->ky[s - 1][j];
        input_at(in, k, phase, K, s == 3 ? 2u : 1u, ws->cur);
        derivative(cfg, ws, ws->sv, ws->sy, ws->cur, ws->kv[s], ws->ky[s], NULL);
    }
    OmegaResState out;
    memset(&out, 0, sizeof out);
    memcpy(out.config_id, cur->config_id, OMEGA_RES_DIGEST_SIZE);
    out.step = cur->step + 1u;
    out.node_count = n;
    out.side_count = ns;
    for (uint32_t i = 0; i < n; i++) {
        const float d = h6 * (((ws->kv[0][i] + 2.0f * ws->kv[1][i]) + 2.0f * ws->kv[2][i]) + ws->kv[3][i]);
        out.v[i] = two_sum(cur->v[i], d + cur->vc[i], &out.vc[i]);
    }
    for (uint32_t j = 0; j < ns; j++) {
        const float d = h6 * (((ws->ky[0][j] + 2.0f * ws->ky[1][j]) + 2.0f * ws->ky[2][j]) + ws->ky[3][j]);
        out.y[j] = two_sum(cur->y[j], d + cur->yc[j], &out.yc[j]);
    }
    st = state_check(cfg, &out);
    if (st != OMEGA_RES_OK) return st;
    *next = out;
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_step(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                              const OmegaResState *cur, OmegaResState *next)
{
    if (!cfg || !in || !ws || !cur || !next) return OMEGA_RES_ERR_NULL;
    if (overlaps(cur, sizeof *cur, next, sizeof *next) || overlaps(ws, SCRATCH_BYTES, cur, sizeof *cur) ||
        overlaps(ws, SCRATCH_BYTES, next, sizeof *next) || overlaps(ws, sizeof *ws, cfg, sizeof *cfg) ||
        overlaps(next, sizeof *next, cfg, sizeof *cfg) || overlaps(next, sizeof *next, in, sizeof *in))
        return OMEGA_RES_ERR_ALIAS;
    return step_impl(cfg, in, ws, cur, next);
}

void omega_res_membrane_stats_init(OmegaResMembraneStats *st)
{
    if (st) memset(st, 0, sizeof *st);
}

OmegaResStatus omega_res_membrane_observe(const OmegaResConfig *cfg, const OmegaResState *s,
                                          OmegaResMembraneStats *st)
{
    if (!cfg || !s || !st) return OMEGA_RES_ERR_NULL;
    if (memcmp(s->config_id, cfg->id, OMEGA_RES_DIGEST_SIZE) != 0 || s->node_count != cfg->topo.node_count)
        return OMEGA_RES_ERR_MISMATCH;
    for (uint32_t i = 0; i < s->node_count; i++) {
        const float m = fabs32(s->v[i]);
        if (m > st->max_node_abs_v) st->max_node_abs_v = m;
    }
    int any = 0;
    for (uint32_t e = 0; e < cfg->topo.edge_count; e++) {
        const float dv = fabs32(s->v[cfg->topo.edge[e].a] - s->v[cfg->topo.edge[e].b]);
        if (dv > st->max_membrane_abs_v) st->max_membrane_abs_v = dv;
        if (dv > OMEGA_RES_VM_PLAUSIBLE) {
            any = 1;
            if (!st->edge_flag[e]) {
                st->edge_flag[e] = 1u;
                st->membranes_implausible++;
            }
        }
    }
    st->states_observed++;
    if (any) st->states_implausible++;
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_advance(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                                 OmegaResState *s, uint64_t steps)
{
    return omega_res_advance_observed(cfg, in, ws, s, steps, NULL);
}

OmegaResStatus omega_res_advance_observed(const OmegaResConfig *cfg, const OmegaResInput *in, OmegaResWorkspace *ws,
                                          OmegaResState *s, uint64_t steps, OmegaResMembraneStats *obs)
{
    if (!cfg || !in || !ws || !s) return OMEGA_RES_ERR_NULL;
    if (overlaps(ws, sizeof *ws, s, sizeof *s) || overlaps(ws, sizeof *ws, cfg, sizeof *cfg) ||
        overlaps(s, sizeof *s, cfg, sizeof *cfg) || overlaps(s, sizeof *s, in, sizeof *in))
        return OMEGA_RES_ERR_ALIAS;
    if (obs && (overlaps(obs, sizeof *obs, s, sizeof *s) || overlaps(obs, sizeof *obs, ws, sizeof *ws) ||
                overlaps(obs, sizeof *obs, cfg, sizeof *cfg)))
        return OMEGA_RES_ERR_ALIAS;
    /* Double buffering: the two state buffers are *s and ws->spare. */
    OmegaResState *a = s, *b = &ws->spare;
    OmegaResStatus st = OMEGA_RES_OK;
    for (uint64_t i = 0; i < steps; i++) {
        st = step_impl(cfg, in, ws, a, b);
        if (st != OMEGA_RES_OK) break;
        if (obs) (void)omega_res_membrane_observe(cfg, b, obs); /* b passed state_check: cannot refuse */
        OmegaResState *tmp = a;
        a = b;
        b = tmp;
    }
    if (a != s) *s = *a;
    return st;
}

/* ------------------------------------------------------------- receipts */

OmegaResStatus omega_res_input_digest(const OmegaResInput *in, uint8_t out[OMEGA_RES_DIGEST_SIZE])
{
    if (!in || !in->current_a || !out) return OMEGA_RES_ERR_NULL;
    sha256_ctx c;
    h_domain(&c, OMEGA_RES_DOMAIN_INPUT);
    h_u32(&c, in->node_count);
    h_u32(&c, in->knot_count);
    const size_t total = (size_t)in->node_count * in->knot_count;
    uint8_t buf[1024];
    size_t used = 0;
    for (size_t i = 0; i < total; i++) {
        uint32_t u = f32_bits(in->current_a[i]);
        buf[used++] = (uint8_t)u; buf[used++] = (uint8_t)(u >> 8);
        buf[used++] = (uint8_t)(u >> 16); buf[used++] = (uint8_t)(u >> 24);
        if (used == sizeof buf) { sha256_update(&c, buf, used); used = 0; }
    }
    if (used) sha256_update(&c, buf, used);
    sha256_final(&c, out);
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_state_digest(const OmegaResState *s, uint8_t out[OMEGA_RES_DIGEST_SIZE])
{
    if (!s || !out) return OMEGA_RES_ERR_NULL;
    if (s->node_count > OMEGA_RES_MAX_NODES || s->side_count > OMEGA_RES_MAX_SIDES) return OMEGA_RES_ERR_MISMATCH;
    sha256_ctx c;
    h_domain(&c, OMEGA_RES_DOMAIN_STATE);
    sha256_update(&c, s->config_id, OMEGA_RES_DIGEST_SIZE);
    h_u64(&c, s->step);
    h_u32(&c, s->node_count);
    h_u32(&c, s->side_count);
    for (uint32_t i = 0; i < s->node_count; i++) h_f32(&c, s->v[i]);
    for (uint32_t k = 0; k < s->side_count; k++) h_f32(&c, s->y[k]);
    for (uint32_t i = 0; i < s->node_count; i++) h_f32(&c, s->vc[i]);
    for (uint32_t k = 0; k < s->side_count; k++) h_f32(&c, s->yc[k]);
    sha256_final(&c, out);
    return OMEGA_RES_OK;
}

OmegaResStatus omega_res_run_receipt(const OmegaResConfig *cfg, const OmegaResInput *in,
                                     const OmegaResState *final_state, OmegaResRunReceipt *out)
{
    if (!cfg || !in || !final_state || !out) return OMEGA_RES_ERR_NULL;
    OmegaResStatus st = state_check(cfg, final_state);
    if (st != OMEGA_RES_OK) return st;
    OmegaResRunReceipt r;
    memset(&r, 0, sizeof r);
    memcpy(r.config_id, cfg->id, OMEGA_RES_DIGEST_SIZE);
    if ((st = omega_res_input_digest(in, r.input_digest)) != OMEGA_RES_OK) return st;
    if ((st = omega_res_state_digest(final_state, r.final_state_digest)) != OMEGA_RES_OK) return st;
    r.steps = final_state->step;
    sha256_ctx c;
    h_domain(&c, OMEGA_RES_DOMAIN_RUN);
    sha256_update(&c, r.config_id, OMEGA_RES_DIGEST_SIZE);
    sha256_update(&c, r.input_digest, OMEGA_RES_DIGEST_SIZE);
    h_u64(&c, r.steps);
    sha256_update(&c, r.final_state_digest, OMEGA_RES_DIGEST_SIZE);
    sha256_final(&c, r.run_id);
    *out = r;
    return OMEGA_RES_OK;
}
