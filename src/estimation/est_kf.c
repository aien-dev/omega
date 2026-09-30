/* EST-1 linear Kalman filter. See est_kf.h for the equations and refusals.
 * Row-major dense matrices, at most EST_MAX_DIM per side, all on the stack.
 * No allocation, no globals, no I/O. Outputs are built in locals and copied
 * to the caller only on success, so a refusal never leaves a half-written
 * record. Inputs are const and never modified. */
#include "est_kf.h"

#include <math.h>
#include <string.h>

#define D EST_MAX_DIM
#define MAT (EST_MAX_DIM * EST_MAX_DIM)

/* C(r x c) = A(r x k) * B(k x c) */
static void mm(const double *A, const double *B, double *C, uint32_t r, uint32_t k, uint32_t c)
{
    for (uint32_t i = 0; i < r; i++)
        for (uint32_t j = 0; j < c; j++) {
            double s = 0.0;
            for (uint32_t t = 0; t < k; t++) s += A[i * k + t] * B[t * c + j];
            C[i * c + j] = s;
        }
}

/* C(r x c) = A(r x k) * B^T where B is (c x k) */
static void mmt(const double *A, const double *B, double *C, uint32_t r, uint32_t k, uint32_t c)
{
    for (uint32_t i = 0; i < r; i++)
        for (uint32_t j = 0; j < c; j++) {
            double s = 0.0;
            for (uint32_t t = 0; t < k; t++) s += A[i * k + t] * B[j * k + t];
            C[i * c + j] = s;
        }
}

static void symmetrize(double *A, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < i; j++) {
            double v = 0.5 * (A[i * n + j] + A[j * n + i]);
            A[i * n + j] = v;
            A[j * n + i] = v;
        }
}

/* Cholesky S = L L^T (lower). Refuses a pivot that is not strictly positive
 * or not finite. */
static int chol(const double *S, double *L, uint32_t n)
{
    memset(L, 0, sizeof(double) * MAT);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j <= i; j++) {
            double s = S[i * n + j];
            for (uint32_t k = 0; k < j; k++) s -= L[i * n + k] * L[j * n + k];
            if (i == j) {
                if (!(s > 0.0) || !isfinite(s)) return 0;
                L[i * n + i] = sqrt(s);
            } else {
                L[i * n + j] = s / L[j * n + j];
            }
        }
    return 1;
}

/* Solve L y = b (forward). */
static void fwd(const double *L, const double *b, double *y, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        double s = b[i];
        for (uint32_t k = 0; k < i; k++) s -= L[i * n + k] * y[k];
        y[i] = s / L[i * n + i];
    }
}
/* Solve L^T x = y (backward). */
static void bwd(const double *L, const double *y, double *x, uint32_t n)
{
    for (uint32_t ii = n; ii > 0; ii--) {
        uint32_t i = ii - 1;
        double s = y[i];
        for (uint32_t k = i + 1; k < n; k++) s -= L[k * n + i] * x[k];
        x[i] = s / L[i * n + i];
    }
}

static est_status model_digest_matches(const est_model *mdl, const est_digest *want)
{
    est_digest d;
    est_status st = est_digest_model(mdl, &d);
    if (st != EST_OK) return st;
    return memcmp(d.b, want->b, EST_DIGEST_SIZE) == 0 ? EST_OK : EST_ERR_MODEL;
}

static est_status units_match(const est_unit *a, const est_unit *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i]) return EST_ERR_UNIT;
    return EST_OK;
}

est_status est_kf_prior(const est_model *mdl, const double *x0, const double *P0,
                        int64_t t_ns, est_belief *out)
{
    if (!mdl || !x0 || !P0 || !out) return EST_ERR_NULL;
    est_status st = est_check_model(mdl);
    if (st != EST_OK) return st;
    est_belief b;
    memset(&b, 0, sizeof b);
    b.n = mdl->n;
    for (uint32_t i = 0; i < mdl->n; i++) {
        b.unit[i] = mdl->state_unit[i];
        b.x[i] = x0[i];
    }
    memcpy(b.P, P0, sizeof(double) * mdl->n * mdl->n);
    b.t_ns = t_ns;
    st = est_digest_model(mdl, &b.model);
    if (st != EST_OK) return st;
    st = est_check_belief(&b);
    if (st != EST_OK) return st;
    *out = b;
    return EST_OK;
}

est_status est_kf_predict(const est_model *mdl, const est_belief *bel,
                          const double *Bu, uint32_t horizon, est_prediction *out)
{
    if (!mdl || !bel || !out) return EST_ERR_NULL;
    est_status st = est_check_model(mdl);
    if (st != EST_OK) return st;
    st = est_check_belief(bel);
    if (st != EST_OK) return st;
    uint32_t n = mdl->n, m = mdl->m;
    if (bel->n != n) return EST_ERR_DIM;
    st = units_match(bel->unit, mdl->state_unit, n);
    if (st != EST_OK) return st;
    st = model_digest_matches(mdl, &bel->model);
    if (st != EST_OK) return st;
    if (horizon < 1u) return EST_ERR_TIME;
    if (Bu) {
        for (uint32_t i = 0; i < n; i++)
            if (!isfinite(Bu[i])) return EST_ERR_NONFINITE;
    }
    if (bel->generation == UINT64_MAX) return EST_ERR_STALE;
    /* t = bel.t + horizon * step, refusing overflow */
    if ((int64_t)horizon > INT64_MAX / mdl->step_ns) return EST_ERR_TIME;
    int64_t span = (int64_t)horizon * mdl->step_ns;
    if (bel->t_ns > INT64_MAX - span) return EST_ERR_TIME;

    est_prediction p;
    memset(&p, 0, sizeof p);
    est_digest prior;
    st = est_digest_belief(bel, &prior);
    if (st != EST_OK) return st;

    double x[D] = {0}, xn[D] = {0}, P[MAT] = {0}, T[MAT] = {0}, P2[MAT] = {0};
    memcpy(x, bel->x, sizeof(double) * n);
    memcpy(P, bel->P, sizeof(double) * n * n);
    for (uint32_t h = 0; h < horizon; h++) {
        for (uint32_t i = 0; i < n; i++) {
            double s = 0.0;
            for (uint32_t j = 0; j < n; j++) s += mdl->F[i * n + j] * x[j];
            xn[i] = Bu ? s + Bu[i] : s;
        }
        memcpy(x, xn, sizeof(double) * n);
        mm(mdl->F, P, T, n, n, n);
        mmt(T, mdl->F, P2, n, n, n);
        for (uint32_t i = 0; i < n * n; i++) P[i] = P2[i] + mdl->Q[i];
        symmetrize(P, n);
        for (uint32_t i = 0; i < n; i++) {
            if (!isfinite(x[i])) return EST_ERR_NONFINITE;
        }
    }

    p.prior = prior;
    p.model = bel->model;
    p.horizon = horizon;
    p.generation = bel->generation + 1u;
    p.t_ns = bel->t_ns + span;
    p.n = n;
    p.m = m;
    memcpy(p.x, x, sizeof(double) * n);
    memcpy(p.P, P, sizeof(double) * n * n);
    for (uint32_t i = 0; i < m; i++) {
        double s = 0.0;
        for (uint32_t j = 0; j < n; j++) s += mdl->H[i * n + j] * x[j];
        p.y_mean[i] = s;
    }
    double PHt[MAT] = {0}, S[MAT] = {0};
    mmt(P, mdl->H, PHt, n, n, m);
    mm(mdl->H, PHt, S, m, n, m);
    for (uint32_t i = 0; i < m * m; i++) S[i] += mdl->R[i];
    symmetrize(S, m);
    memcpy(p.S, S, sizeof(double) * m * m);
    st = est_check_prediction(&p);
    if (st != EST_OK) return st;
    *out = p;
    return EST_OK;
}

/* Shared staleness / binding checks for update and coast. */
static est_status bind_checks(const est_model *mdl, const est_belief *prior,
                              const est_prediction *pred, est_digest *pred_digest)
{
    est_status st = est_check_model(mdl);
    if (st != EST_OK) return st;
    st = est_check_belief(prior);
    if (st != EST_OK) return st;
    st = est_check_prediction(pred);
    if (st != EST_OK) return st;
    est_digest pd;
    st = est_digest_belief(prior, &pd);
    if (st != EST_OK) return st;
    if (memcmp(pd.b, pred->prior.b, EST_DIGEST_SIZE) != 0) return EST_ERR_STALE;
    if (pred->generation != prior->generation + 1u || prior->generation == UINT64_MAX) return EST_ERR_STALE;
    st = model_digest_matches(mdl, &prior->model);
    if (st != EST_OK) return st;
    if (memcmp(pred->model.b, prior->model.b, EST_DIGEST_SIZE) != 0) return EST_ERR_MODEL;
    if (pred->n != mdl->n || pred->m != mdl->m || prior->n != mdl->n) return EST_ERR_DIM;
    return est_digest_prediction(pred, pred_digest);
}

est_status est_kf_update(const est_model *mdl, const est_belief *prior_belief,
                         const est_prediction *pred, const est_observation *obs,
                         est_belief *posterior, est_innovation *innovation)
{
    if (!mdl || !prior_belief || !pred || !obs || !posterior) return EST_ERR_NULL;
    est_digest pdig;
    est_status st = bind_checks(mdl, prior_belief, pred, &pdig);
    if (st != EST_OK) return st;
    st = est_check_observation(obs);
    if (st != EST_OK) return st;
    uint32_t n = mdl->n, m = mdl->m;
    if (obs->m != m) return EST_ERR_DIM;
    if (obs->t_ns != pred->t_ns) return EST_ERR_TIME;
    st = units_match(obs->unit, mdl->obs_unit, m);
    if (st != EST_OK) return st;

    est_digest odig, root;
    st = est_digest_observation(obs, &odig);
    if (st != EST_OK) return st;
    st = est_evidence_root_extend(&prior_belief->evidence_root, &odig, &root);
    if (st != EST_OK) return st;

    double nu[D] = {0}, PHt[MAT] = {0}, S[MAT] = {0}, L[MAT] = {0};
    for (uint32_t i = 0; i < m; i++) {
        double s = 0.0;
        for (uint32_t j = 0; j < n; j++) s += mdl->H[i * n + j] * pred->x[j];
        nu[i] = obs->z[i] - s;
    }
    mmt(pred->P, mdl->H, PHt, n, n, m);   /* P' H^T, n x m */
    mm(mdl->H, PHt, S, m, n, m);
    for (uint32_t i = 0; i < m * m; i++) S[i] += obs->R[i];
    symmetrize(S, m);
    if (!chol(S, L, m)) return EST_ERR_NOT_PD;

    double y[D] = {0}, K[MAT] = {0}, row[D] = {0}, krow[D] = {0};
    fwd(L, nu, y, m);
    double nis = 0.0;
    for (uint32_t i = 0; i < m; i++) nis += y[i] * y[i];
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < m; j++) row[j] = PHt[i * m + j];
        fwd(L, row, y, m);
        bwd(L, y, krow, m);
        for (uint32_t j = 0; j < m; j++) K[i * m + j] = krow[j];
    }

    est_belief b;
    memset(&b, 0, sizeof b);
    b.n = n;
    for (uint32_t i = 0; i < n; i++) {
        double s = 0.0;
        for (uint32_t j = 0; j < m; j++) s += K[i * m + j] * nu[j];
        b.x[i] = pred->x[i] + s;
        b.unit[i] = mdl->state_unit[i];
    }
    /* Joseph form: A = I - K H, P = A P' A^T + K R K^T */
    double A[MAT] = {0}, T[MAT] = {0}, P1[MAT] = {0}, KR[MAT] = {0}, P2[MAT] = {0};
    mm(K, mdl->H, T, n, m, n);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < n; j++) A[i * n + j] = (i == j ? 1.0 : 0.0) - T[i * n + j];
    mm(A, pred->P, T, n, n, n);
    mmt(T, A, P1, n, n, n);
    mm(K, obs->R, KR, n, m, m);
    mmt(KR, K, P2, n, m, n);
    for (uint32_t i = 0; i < n * n; i++) b.P[i] = P1[i] + P2[i];
    symmetrize(b.P, n);
    b.generation = pred->generation + 1u;
    b.t_ns = pred->t_ns;
    b.model = prior_belief->model;
    b.parent = pdig;
    b.evidence_root = root;
    st = est_check_belief(&b);
    if (st != EST_OK) return st;

    est_innovation iv;
    memset(&iv, 0, sizeof iv);
    iv.prediction = pdig;
    iv.observation = odig;
    iv.model = prior_belief->model;
    iv.m = m;
    memcpy(iv.nu, nu, sizeof(double) * m);
    memcpy(iv.S, S, sizeof(double) * m * m);
    iv.nis = nis;
    iv.t_ns = pred->t_ns;
    st = est_check_innovation(&iv);
    if (st != EST_OK) return st;

    *posterior = b;
    if (innovation) *innovation = iv;
    return EST_OK;
}

est_status est_kf_coast(const est_model *mdl, const est_prediction *pred,
                        const est_belief *prior_belief, est_belief *out)
{
    if (!mdl || !pred || !prior_belief || !out) return EST_ERR_NULL;
    est_digest pdig;
    est_status st = bind_checks(mdl, prior_belief, pred, &pdig);
    if (st != EST_OK) return st;
    est_belief b;
    memset(&b, 0, sizeof b);
    b.n = pred->n;
    for (uint32_t i = 0; i < b.n; i++) {
        b.unit[i] = mdl->state_unit[i];
        b.x[i] = pred->x[i];
    }
    memcpy(b.P, pred->P, sizeof(double) * b.n * b.n);
    b.generation = pred->generation + 1u;
    b.t_ns = pred->t_ns;
    b.model = prior_belief->model;
    b.parent = pdig;
    b.evidence_root = prior_belief->evidence_root;
    st = est_check_belief(&b);
    if (st != EST_OK) return st;
    *out = b;
    return EST_OK;
}
