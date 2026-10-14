/* binary64 Dormand-Prince oracle; see res_oracle64.h. */
#include "res_oracle64.h"

#include <math.h>
#include <string.h>

#define NMAX OMEGA_RES_MAX_NODES
#define YMAX (OMEGA_RES_MAX_NODES + OMEGA_RES_MAX_SIDES)

typedef struct {
    unsigned n, ne, m;              /* nodes, undirected edges, unknown nodes */
    unsigned a[OMEGA_RES_MAX_EDGES], b[OMEGA_RES_MAX_EDGES];
    int unk[NMAX];                  /* node -> unknown index or -1 */
    unsigned unk_node[NMAX];
    double lu[NMAX][NMAX];          /* LU of the reduced capacitance matrix */
    unsigned piv[NMAX];
    double gu_a, z, rate, n0s, n0r;
    double c[NMAX], tau0[NMAX], vtau[NMAX], ve[NMAX];
} model;

static int lu_factor(model *md)
{
    for (unsigned k = 0; k < md->m; k++) {
        unsigned p = k;
        for (unsigned i = k + 1; i < md->m; i++)
            if (fabs(md->lu[i][k]) > fabs(md->lu[p][k])) p = i;
        if (md->lu[p][k] == 0.0) return -1;
        md->piv[k] = p;
        if (p != k)
            for (unsigned j = 0; j < md->m; j++) { double t = md->lu[k][j]; md->lu[k][j] = md->lu[p][j]; md->lu[p][j] = t; }
        for (unsigned i = k + 1; i < md->m; i++) {
            md->lu[i][k] /= md->lu[k][k];
            for (unsigned j = k + 1; j < md->m; j++) md->lu[i][j] -= md->lu[i][k] * md->lu[k][j];
        }
    }
    return 0;
}

static void lu_solve(const model *md, double *x)
{
    for (unsigned k = 0; k < md->m; k++) {
        unsigned p = md->piv[k];
        if (p != k) { double t = x[k]; x[k] = x[p]; x[p] = t; }
        for (unsigned i = k + 1; i < md->m; i++) x[i] -= md->lu[i][k] * x[k];
    }
    for (unsigned i = md->m; i-- > 0;) {
        for (unsigned j = i + 1; j < md->m; j++) x[i] -= md->lu[i][j] * x[j];
        x[i] /= md->lu[i][i];
    }
}

/* y = [V (n), N (2 ne)] */
static void rhs(const model *md, const double *cur, const double *y, double *dy)
{
    double B[NMAX], x[NMAX];
    const double *V = y, *N = y + md->n;
    double *dN = dy + md->n;
    for (unsigned i = 0; i < md->n; i++) B[i] = cur[i];
    for (unsigned e = 0; e < md->ne; e++) {
        const unsigned a = md->a[e], b = md->b[e];
        const double q = md->gu_a * (N[2 * e] + N[2 * e + 1]) * (V[a] - V[b]);
        B[a] -= q;
        B[b] += q;
        for (int side = 0; side < 2; side++) {
            const unsigned o = side ? b : a, p = side ? a : b;
            const double dv = V[o] - V[p];
            const double alpha = md->rate / (md->tau0[o] * exp(dv / md->vtau[o]));
            const double nss = md->n0s * exp(md->n0r * md->c[o]) * exp(dv / md->ve[o]);
            const double Nk = N[2 * e + (unsigned)side];
            dN[2 * e + (unsigned)side] = alpha * Nk * (1.0 - pow(Nk / nss, md->z));
        }
    }
    for (unsigned u = 0; u < md->m; u++) x[u] = B[md->unk_node[u]];
    lu_solve(md, x);
    for (unsigned i = 0; i < md->n; i++) dy[i] = md->unk[i] < 0 ? 0.0 : x[md->unk[i]];
}

int res64_run(const OmegaResTopology *topo, const OmegaResParams *par, const double *n_init,
              const float *current_a, unsigned knot_count, double dt_s, res64_opts *opt, double *v_out,
              double *n_final)
{
    static const double c2 = 1.0 / 5, c3 = 3.0 / 10, c4 = 4.0 / 5, c5 = 8.0 / 9;
    static const double a21 = 1.0 / 5, a31 = 3.0 / 40, a32 = 9.0 / 40, a41 = 44.0 / 45, a42 = -56.0 / 15,
                        a43 = 32.0 / 9, a51 = 19372.0 / 6561, a52 = -25360.0 / 2187, a53 = 64448.0 / 6561,
                        a54 = -212.0 / 729, a61 = 9017.0 / 3168, a62 = -355.0 / 33, a63 = 46732.0 / 5247,
                        a64 = 49.0 / 176, a65 = -5103.0 / 18656, b1 = 35.0 / 384, b3 = 500.0 / 1113,
                        b4 = 125.0 / 192, b5 = -2187.0 / 6784, b6 = 11.0 / 84;
    static const double e1 = 35.0 / 384 - 5179.0 / 57600, e3 = 500.0 / 1113 - 7571.0 / 16695,
                        e4 = 125.0 / 192 - 393.0 / 640, e5 = -2187.0 / 6784 + 92097.0 / 339200,
                        e6 = 11.0 / 84 - 187.0 / 2100, e7 = -1.0 / 40;
    static model md;
    memset(&md, 0, sizeof md);
    md.n = topo->node_count;
    md.ne = topo->edge_count;
    for (unsigned e = 0; e < md.ne; e++) { md.a[e] = topo->edge[e].a; md.b[e] = topo->edge[e].b; }
    md.gu_a = (double)par->g_u_s * (double)par->area_m2;
    md.z = par->z; md.rate = par->rate_scale; md.n0s = par->n0_scale; md.n0r = par->n0_rate;
    for (unsigned i = 0; i < md.n; i++) {
        md.c[i] = par->conc_um[i]; md.tau0[i] = par->tau0_s[i]; md.vtau[i] = par->v_tau_v[i]; md.ve[i] = par->v_e_v[i];
        md.unk[i] = -1;
        if (!topo->is_ground[i]) { md.unk[i] = (int)md.m; md.unk_node[md.m++] = i; }
    }
    const double cap = (double)par->eps_f_per_m * (double)par->area_m2 / (double)par->h_m;
    for (unsigned e = 0; e < md.ne; e++) {
        const int ua = md.unk[md.a[e]], ub = md.unk[md.b[e]];
        if (ua >= 0) md.lu[ua][ua] += cap;
        if (ub >= 0) md.lu[ub][ub] += cap;
        if (ua >= 0 && ub >= 0) { md.lu[ua][ub] -= cap; md.lu[ub][ua] -= cap; }
    }
    if (lu_factor(&md)) return -1;

    const unsigned ny = md.n + 2 * md.ne;
    double y[YMAX], yn[YMAX], yt[YMAX], k[7][YMAX], cur[NMAX];
    memset(y, 0, sizeof y);
    for (unsigned s = 0; s < 2 * md.ne; s++) y[md.n + s] = n_init[s];
    for (unsigned i = 0; i < md.n; i++) v_out[i] = 0.0;
    double h = dt_s;
    opt->steps_accepted = opt->steps_rejected = 0;
    for (unsigned kn = 0; kn + 1 < knot_count; kn++) {
        const float *i0 = current_a + (size_t)kn * md.n, *i1 = i0 + md.n;
        double t = 0.0; /* time within this knot interval */
        while (t < dt_s) {
            if (h > dt_s - t) h = dt_s - t;
            if (h < 1e-18) return -1;
            const double cs[7] = {0.0, c2, c3, c4, c5, 1.0, 1.0};
            for (int st = 0; st < 7; st++) {
                for (unsigned j = 0; j < ny; j++) {
                    double acc = y[j];
                    switch (st) {
                    case 0: break;
                    case 1: acc += h * (a21 * k[0][j]); break;
                    case 2: acc += h * (a31 * k[0][j] + a32 * k[1][j]); break;
                    case 3: acc += h * (a41 * k[0][j] + a42 * k[1][j] + a43 * k[2][j]); break;
                    case 4: acc += h * (a51 * k[0][j] + a52 * k[1][j] + a53 * k[2][j] + a54 * k[3][j]); break;
                    case 5: acc += h * (a61 * k[0][j] + a62 * k[1][j] + a63 * k[2][j] + a64 * k[3][j] + a65 * k[4][j]); break;
                    default: acc += h * (b1 * k[0][j] + b3 * k[2][j] + b4 * k[3][j] + b5 * k[4][j] + b6 * k[5][j]); break;
                    }
                    yt[j] = acc;
                }
                if (st == 6) memcpy(yn, yt, sizeof(double) * ny);
                const double f = (t + cs[st] * h) / dt_s;
                for (unsigned i = 0; i < md.n; i++) cur[i] = (double)i0[i] + f * ((double)i1[i] - (double)i0[i]);
                rhs(&md, cur, yt, k[st]);
            }
            double err = 0.0;
            int bad = 0;
            for (unsigned j = 0; j < ny; j++) {
                const double ej = h * (e1 * k[0][j] + e3 * k[2][j] + e4 * k[3][j] + e5 * k[4][j] + e6 * k[5][j] + e7 * k[6][j]);
                const double sc = opt->atol + opt->rtol * fmax(fabs(y[j]), fabs(yn[j]));
                const double r = fabs(ej) / sc;
                if (!isfinite(r) || !isfinite(yn[j])) bad = 1;
                else if (r > err) err = r;
                if (j >= md.n && !(yn[j] > 0.0)) bad = 1; /* densities stay positive */
            }
            if (bad) err = INFINITY;
            if (err <= 1.0) {
                memcpy(y, yn, sizeof(double) * ny);
                t += h;
                opt->steps_accepted++;
                h *= fmin(5.0, fmax(0.2, 0.9 * pow(err > 1e-30 ? err : 1e-30, -0.2)));
            } else {
                opt->steps_rejected++;
                h *= err < INFINITY ? fmax(0.1, 0.9 * pow(err, -0.2)) : 0.1;
            }
        }
        for (unsigned i = 0; i < md.n; i++) v_out[(size_t)(kn + 1) * md.n + i] = y[i];
    }
    for (unsigned j = 0; j < ny; j++)
        if (!isfinite(y[j])) return -1;
    if (n_final)
        for (unsigned s = 0; s < 2 * md.ne; s++) n_final[s] = y[md.n + s];
    return 0;
}
