/* PD-0 hidden generators: spec section 4.1 (physics main @ 5bd2b04, revision 3)
 * verbatim in integer micro-units. Discrete maps: the law is the map written
 * here, not the differential equation it resembles.
 * Build with -DPD0_MUTANT_SIGN to flip the sign of the spring term in L1
 * (NC-3 style mutation: the oracle check must then FAIL). -DPD0_MUTANT_LATENT_SIGN flips the sign of m*h in L6. */
#include "physics0/pd0_gen.h"
#include "physics0/pd0_guard.h"

#include <stdlib.h>
#include <string.h>

#define U(x) ((int64_t)((x) * 1000000.0))

static const char *NAMES[PD0_LEVELS] = {"L0", "L1", "L2", "L3", "L4", "L5", "L6", "NULL"};
const char *pd0_gen_level_name(int level) { return (level >= 0 && level < PD0_LEVELS) ? NAMES[level] : "?"; }

/* THE one table of declared constant ranges (spec 4.1), in DRAW ORDER. The
 * generator draws from it and the oracle's declared-range check reads it. */
static const pd0_crange T_L1[] = {{"k", U(1.0), U(4.0)}};
static const pd0_crange T_L2[] = {{"k", U(1.0), U(9.0)}, {"c", U(0.2), U(2.0)}};
static const pd0_crange T_L3[] = {{"k", U(1.0), U(4.0)}, {"g", U(0.5), U(2.0)}};
static const pd0_crange T_L4[] = {{"k", U(1.0), U(4.0)}, {"b", U(0.5), U(2.0)}};
/* rev 3: order k, w, m, q */
static const pd0_crange T_L6[] = {{"k", U(3.0), U(5.0)}, {"w", U(0.5), U(1.0)}, {"m", U(1.0), U(2.0)}, {"q", U(1.0), U(2.0)}};

int pd0_gen_const_table(int level, const pd0_crange **t) {
    switch (level) {
    case PD0_L1: *t = T_L1; return 1;
    case PD0_L2: case PD0_L5: *t = T_L2; return 2;
    case PD0_L3: *t = T_L3; return 2;
    case PD0_L4: *t = T_L4; return 2;
    case PD0_L6: *t = T_L6; return 4;
    default: *t = NULL; return 0;
    }
}

/* reset box per observed variable (spec 4 table notes, rev 3): [-2, 2] except
 * L0 s1 [-0.2, 0.2] and L4 both [-1, 1]. */
void pd0_gen_reset_box(int level, int var, int64_t *lo, int64_t *hi) {
    int64_t b = U(2.0);
    if (level == PD0_L0 && var == 1) b = U(0.2);
    if (level == PD0_L4) b = U(1.0);
    *lo = -b; *hi = b;
}

/* scoring boxes (spec 6.1, rev 3): in-box = the reset box, extrapolation = 1.5 x it */
void pd0_gen_score_box(int level, int var, int extrap, int64_t *lo, int64_t *hi) {
    pd0_gen_reset_box(level, var, lo, hi);
    if (extrap) { *lo = (*lo * 3) / 2; *hi = (*hi * 3) / 2; }
}

int pd0_gen_desc(int level, pd0_desc *d) {
    memset(d, 0, sizeof *d);
    d->n_obs = 2; d->n_channels = 1;
    for (int v = 0; v < PD0_MAX_OBS; v++) pd0_gen_reset_box(level, v, &d->reset_min[v], &d->reset_max[v]); /* trimmed to n_obs below */
    d->episode_max_steps = PD0_EPISODE_STEPS;
    d->budget_steps = 3000; d->budget_episodes = 300;
    d->dt_micro = U(0.05);
    d->chan_min[0] = -U(2.0); d->chan_max[0] = U(2.0);
    switch (level) {
    case PD0_L0: /* rev 3: 20 steps per episode, 3000 steps = 150 episodes, pushes [-0.1, 0.1] */
        d->dt_micro = U(1.0); d->chan_min[0] = -U(0.1); d->chan_max[0] = U(0.1);
        d->episode_max_steps = 20; d->budget_episodes = 150;
        break;
    case PD0_L1: case PD0_L2: case PD0_L4: break;
    case PD0_L3: d->n_obs = 4; d->n_channels = 2; d->chan_min[1] = -U(2.0); d->chan_max[1] = U(2.0); break;
    case PD0_L5: case PD0_L6: d->budget_steps = 8000; break;
    case PD0_LEVEL_NULL: break; /* same interface and budget as L2 (NC-1) */
    default: return -1;
    }
    for (int v = d->n_obs; v < PD0_MAX_OBS; v++) d->reset_min[v] = d->reset_max[v] = 0; /* unused slots stay zero (round trip) */
    return 0;
}

int pd0_gen_init_cap(pd0_gen *g, int level, uint64_t seed, int redraw_cap) {
    memset(g, 0, sizeof *g);
    g->level = level;
    pd0_rng c;
    pd0_stream(&c, seed, "const");
    const pd0_crange *t;
    int n = pd0_gen_const_table(level, &t);
    for (int i = 0; i < n; i++) g->k[i] = pd0_const(&c, t[i].lo, t[i].hi); /* draw order = table order */
    switch (level) {
    case PD0_L0: case PD0_L1: case PD0_L2: case PD0_L3: case PD0_L4: break;
    case PD0_L5: g->sigma_obs = U(0.02); break;
    case PD0_L6:
        g->n_hidden = 1;
        /* spec 4.1 rev 3: while m*q/w > 0.7*k redraw m then q from the same stream.
         * Exact integer test (no rounding): m*q/w > 0.7*k  <=>  10*M*Q > 7*K*W in micro units. */
        while (10 * g->k[2] * g->k[3] > 7 * g->k[0] * g->k[1]) {
            if (g->redraws >= redraw_cap) { g->invalid = 1; return -1; } /* reached the cap: invalid seed */
            g->k[2] = pd0_const(&c, t[2].lo, t[2].hi);
            g->k[3] = pd0_const(&c, t[3].lo, t[3].hi);
            g->redraws++;
        }
        break;
    case PD0_LEVEL_NULL: g->sigma_null = U(0.5); break;
    default: return -1;
    }
    return 0;
}

int pd0_gen_init(pd0_gen *g, int level, uint64_t seed) { return pd0_gen_init_cap(g, level, seed, PD0_L6_REDRAW_CAP); }

static int oob(int64_t v) { return !pd0_guard_in_box(v); }

int pd0_gen_step(const pd0_gen *g, const int64_t *s, int64_t h, const int64_t *u, int64_t dt,
                 int64_t *o, int64_t *h_out, int64_t *obs, pd0_rng *noise, pd0_rng *null) {
    const int64_t k = g->k[0], c = g->k[1];
    int64_t hn = h;
    int n = 2;
    switch (g->level) {
    case PD0_L0:
        o[0] = s[0] + s[1];
        o[1] = s[1] + u[0];
        break;
    case PD0_L1: {
#ifdef PD0_MUTANT_SIGN
        int64_t spring = pd0_mul(k, s[0]);
#else
        int64_t spring = -pd0_mul(k, s[0]);
#endif
        o[0] = s[0] + pd0_mul(s[1], dt);
        o[1] = s[1] + pd0_mul(spring + u[0], dt);
        break;
    }
    case PD0_L2: case PD0_L5:
        o[0] = s[0] + pd0_mul(s[1], dt);
        o[1] = s[1] + pd0_mul(-pd0_mul(k, s[0]) - pd0_mul(c, s[1]) + u[0], dt);
        break;
    case PD0_L3: {
        const int64_t gg = g->k[1];
        n = 4;
        o[0] = s[0] + pd0_mul(s[1], dt);
        o[1] = s[1] + pd0_mul(-pd0_mul(k, s[0]) + pd0_mul(gg, s[2] - s[0]) + u[0], dt);
        o[2] = s[2] + pd0_mul(s[3], dt);
        o[3] = s[3] + pd0_mul(-pd0_mul(k, s[2]) - pd0_mul(gg, s[2] - s[0]) + u[1], dt);
        break;
    }
    case PD0_L4: {
        const int64_t b = g->k[1];
        int64_t cube = pd0_mul(pd0_mul(s[0], s[0]), s[0]);
        o[0] = s[0] + pd0_mul(s[1], dt);
        o[1] = s[1] + pd0_mul(-pd0_mul(k, s[0]) - pd0_mul(b, cube) + u[0], dt);
        break;
    }
    case PD0_L6: {
        const int64_t w = g->k[1], m = g->k[2], q = g->k[3]; /* draw order k, w, m, q */
        o[0] = s[0] + pd0_mul(s[1], dt);
#ifdef PD0_MUTANT_LATENT_SIGN
        const int64_t lat = -pd0_mul(m, h); /* wrong latent sign: the spec says +m*h */
#else
        const int64_t lat = pd0_mul(m, h);
#endif
        o[1] = s[1] + pd0_mul(-pd0_mul(k, s[0]) + lat + u[0], dt);
        hn = h + pd0_mul(-pd0_mul(w, h) + pd0_mul(q, s[0]), dt);
        break;
    }
    case PD0_LEVEL_NULL:
        /* nothing depends on anything: fresh draws, state and intervention ignored */
        o[0] = pd0_noise(null, g->sigma_null);
        o[1] = pd0_noise(null, g->sigma_null);
        break;
    default: return -1;
    }
    *h_out = hn;
    int bad = oob(hn);
    for (int i = 0; i < n; i++) bad |= oob(o[i]);
    for (int i = 0; i < n; i++) obs[i] = o[i] + (g->sigma_obs ? pd0_noise(noise, g->sigma_obs) : 0);
    return bad;
}

void pd0_gen_observe_reset(const pd0_gen *g, const int64_t *s, int64_t *obs, pd0_rng *noise) {
    int n = g->level == PD0_L3 ? 4 : 2;
    for (int i = 0; i < n; i++) obs[i] = s[i] + (g->sigma_obs ? pd0_noise(noise, g->sigma_obs) : 0);
}

/* term helpers: variable index v (observed then latent), channel index ch */
static void add_var(pd0_eq *e, int64_t coef, int v) { pd0_term t = {coef, {0}}; t.ex[v] = 1; e->t[e->n_terms++] = t; }
static void add_ch(pd0_eq *e, int64_t coef, int ch, int n_vars) { (void)n_vars; pd0_term t = {coef, {0}}; t.ex[PD0_MAX_VARS + ch] = 1; e->t[e->n_terms++] = t; }

int pd0_gen_true_relation(const pd0_gen *g, int64_t dt, pd0_relation *r) {
    memset(r, 0, sizeof *r);
    r->n_channels = g->level == PD0_L3 ? 2 : 1;
    r->n_vars = g->level == PD0_L3 ? 4 : 2;
    if (g->level == PD0_L6) { r->n_vars = 3; r->n_latent = 1; }
    if (g->level == PD0_LEVEL_NULL) { r->n_eq = 0; return 0; }
    const int nv = r->n_vars;
    const int64_t k = g->k[0];
    pd0_eq *e0 = &r->eq[0], *e1 = &r->eq[1];
    e0->target = 0; e1->target = 1;
    r->n_eq = 2;
    switch (g->level) {
    case PD0_L0:
        add_var(e0, PD0_MICRO, 1);
        add_ch(e1, PD0_MICRO, 0, nv);
        break;
    case PD0_L1:
        add_var(e0, dt, 1);
#ifdef PD0_MUTANT_SIGN
        add_var(e1, -pd0_mul(k, dt), 0); /* the reference law stays the spec's: that is the point */
#else
        add_var(e1, -pd0_mul(k, dt), 0);
#endif
        add_ch(e1, dt, 0, nv);
        break;
    case PD0_L2: case PD0_L5:
        add_var(e0, dt, 1);
        add_var(e1, -pd0_mul(k, dt), 0); add_var(e1, -pd0_mul(g->k[1], dt), 1); add_ch(e1, dt, 0, nv);
        break;
    case PD0_L3: {
        const int64_t gg = g->k[1];
        pd0_eq *e2 = &r->eq[2], *e3 = &r->eq[3];
        e2->target = 2; e3->target = 3; r->n_eq = 4;
        add_var(e0, dt, 1);
        add_var(e1, -pd0_mul(k + gg, dt), 0); add_var(e1, pd0_mul(gg, dt), 2); add_ch(e1, dt, 0, nv);
        add_var(e2, dt, 3);
        add_var(e3, pd0_mul(gg, dt), 0); add_var(e3, -pd0_mul(k + gg, dt), 2); add_ch(e3, dt, 1, nv);
        break;
    }
    case PD0_L4: {
        add_var(e0, dt, 1);
        add_var(e1, -pd0_mul(k, dt), 0);
        pd0_term cube = {-pd0_mul(g->k[1], dt), {0}}; cube.ex[0] = 3; e1->t[e1->n_terms++] = cube;
        add_ch(e1, dt, 0, nv);
        break;
    }
    case PD0_L6: {
        pd0_eq *eh = &r->eq[2];
        eh->target = 2; r->n_eq = 3;
        add_var(e0, dt, 1);
        add_var(e1, -pd0_mul(k, dt), 0); add_var(e1, pd0_mul(g->k[2], dt), 2); add_ch(e1, dt, 0, nv); /* m = k[2] */
        add_var(eh, -pd0_mul(g->k[1], dt), 2); add_var(eh, pd0_mul(g->k[3], dt), 0); /* w = k[1], q = k[3] */
        break;
    }
    default: return -1;
    }
    return 0;
}

int pd0_gen_true_size(int level) {
    static const int S[PD0_LEVELS] = {2, 3, 4, 8, 4, 4, 7, 0};
    return (level >= 0 && level < PD0_LEVELS) ? S[level] : -1;
}
