/* PD-0 planner information-efficiency comparison (Direction 5), development evidence.
 *
 * Question: does active experimental choice separate competing explanations
 * with fewer observations or less intervention cost than a preregistered
 * passive (uniform random) schedule?
 *
 * For L1, L2 and L4, seeds 1..5: the hypothesis set is the true relation
 * (oracle-shaped, from the harness side; this is calibration, NOT a learner
 * result) plus two rivals built mechanically from it: the first non-channel
 * coefficient of the second equation scaled by 1.1, and the last non-channel
 * term of the last equation removed. Each strategy runs episodes of 20 steps
 * on the real world and stops when the pooled 20-step rollout NRMSE of EVERY
 * rival against the observed trajectories exceeds the declared eps (0.02, the
 * L0-L4 in-box bound), or at the episode cap. The truth's own NRMSE must stay
 * within eps (sanity; otherwise the row is marked INVALID).
 *
 * This is PD-0 development evidence. It is NOT an EXP-003 claim and must not
 * be cited as one: no blinding, no seal, no preregistered pass criterion.
 *
 * usage: pd0_plan_eff <receipt-path> [commit]  (receipt is created with O_EXCL; never overwrites) */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pd0_planner.h"
#include "pd0p_bridge.h"

#define EPS_PPM 20000
#define EP_CAP 40
#define RESID_SD_MICRO 10000     /* declared r for D: 0.01 units (noise-free worlds) */
#define PLAN_SEED 2026100401ull
#define PASSIVE_SEED 2026100402ull

typedef struct { double se[PD0_MAX_OBS], ss[PD0_MAX_OBS], mean[PD0_MAX_OBS]; uint64_t n; } acc;
typedef struct { int episodes[2]; int64_t cost[2]; int refuted_at[2][2]; int valid; int no_disc; } row;

static void rel_from_pb(const pb_rel *in, pd0_rel *out) {
    memset(out, 0, sizeof *out);
    out->n_vars = in->n_vars; out->n_latent = in->n_latent; out->n_channels = in->n_channels; out->n_equations = in->n_eq;
    for (int e = 0; e < in->n_eq; e++) {
        out->eq[e].target = in->eq[e].target; out->eq[e].n_terms = in->eq[e].n_terms;
        for (int t = 0; t < in->eq[e].n_terms; t++) {
            out->eq[e].coef[t] = in->eq[e].coef[t];
            memcpy(out->eq[e].expo[t], in->eq[e].expo[t], PD0_MAX_VARS + PD0_MAX_CHAN);
        }
    }
    out->description_bits = pd0_rel_bits(out);
}

static int is_channel_term(const pd0_rel *r, int e, int t) {
    for (int c = 0; c < r->n_channels; c++) if (r->eq[e].expo[t][r->n_vars + c]) return 1;
    return 0;
}

/* rival A: first non-channel coefficient of equation 1 (or 0) scaled by 1.1 */
static void rival_scale(const pd0_rel *truth, pd0_rel *out) {
    memcpy(out, truth, sizeof *out);
    int e = truth->n_equations > 1 ? 1 : 0;
    for (int t = 0; t < out->eq[e].n_terms; t++)
        if (!is_channel_term(out, e, t)) { out->eq[e].coef[t] = (out->eq[e].coef[t] * 11) / 10; return; }
}

/* rival B: last non-channel term of the last equation removed (if it leaves one term) */
static int rival_missing(const pd0_rel *truth, pd0_rel *out) {
    memcpy(out, truth, sizeof *out);
    int e = truth->n_equations - 1;
    for (int t = out->eq[e].n_terms - 1; t >= 0; t--)
        if (!is_channel_term(out, e, t) && out->eq[e].n_terms > 1) {
            for (int u = t; u + 1 < out->eq[e].n_terms; u++) {
                out->eq[e].coef[u] = out->eq[e].coef[u + 1];
                memcpy(out->eq[e].expo[u], out->eq[e].expo[u + 1], PD0_MAX_VARS + PD0_MAX_CHAN);
            }
            out->eq[e].n_terms--;
            out->description_bits = pd0_rel_bits(out);
            return 1;
        }
    return 0;
}

static void acc_add(acc *a, int n_obs, const int64_t *pred, const int64_t *obs) {
    for (int i = 0; i < n_obs; i++) {
        double d = (double)(pred[i] - obs[i]) / 1e6, o = (double)obs[i] / 1e6;
        a->se[i] += d * d; a->ss[i] += o * o; a->mean[i] += o;
    }
    a->n++;
}

static double acc_nrmse(const acc *a, int n_obs) {
    double worst = 0;
    if (a->n < 2) return 0;
    for (int i = 0; i < n_obs; i++) {
        double m = a->mean[i] / (double)a->n, var = a->ss[i] / (double)a->n - m * m;
        double sd = var > 0 ? sqrt(var) : 0, rmse = sqrt(a->se[i] / (double)a->n);
        double v = sd > 1e-9 ? rmse / sd : (rmse > 1e-9 ? 1e9 : 0);
        if (v > worst) worst = v;
    }
    return worst;
}

static void rollout(const pd0_rel *r, const pd0_exp *e, int n_ok, int64_t out[PD0_MAX_STEPS][PD0_MAX_OBS]) {
    int64_t st[PD0_MAX_VARS], nx[PD0_MAX_VARS]; memset(st, 0, sizeof st);
    for (int i = 0; i < e->n_obs; i++) st[i] = e->reset[i];
    for (int s = 0; s < n_ok; s++) { pd0_rel_step(r, st, e->steps[s].channel, e->steps[s].value, nx); for (int i = 0; i < e->n_obs; i++) out[s][i] = nx[i]; memcpy(st, nx, sizeof st); }
}

/* strategy 0 = active planner, 1 = passive baseline */
static void run_strategy(int level, uint64_t seed, int strategy, const pd0_rel *truth, const pd0_rel *rv[2], int n_rv, row *out) {
    pb_bounds pb; pb_rel pbt;
    pb_world *w = pb_open(level, seed, &pb, &pbt);
    if (!w) { out->valid = 0; return; }
    pd0p_bounds b; memset(&b, 0, sizeof b);
    b.n_obs = pb.n_obs; b.n_channels = pb.n_channels; b.episode_max_steps = pb.episode_max_steps;
    for (int i = 0; i < pb.n_channels; i++) { b.chan_min[i] = pb.chan_min[i]; b.chan_max[i] = pb.chan_max[i]; }
    for (int i = 0; i < pb.n_obs; i++) { b.reset_min[i] = pb.reset_min[i]; b.reset_max[i] = pb.reset_max[i]; }
    pd0p_domain dom; for (int i = 0; i < pb.n_obs; i++) { dom.var_min[i] = pb.reset_min[i]; dom.var_max[i] = pb.reset_max[i]; }
    pd0p_hyps h; h.n_hyp = (uint8_t)(1 + n_rv); h.hyp[0] = truth; for (int k = 0; k < n_rv; k++) h.hyp[1 + k] = rv[k]; h.residual_sd_micro = RESID_SD_MICRO;
    static pd0p_prior prior; memset(&prior, 0, sizeof prior);
    acc a_truth, a_rv[2]; memset(&a_truth, 0, sizeof a_truth); memset(a_rv, 0, sizeof a_rv);
    int refuted[2] = { 0, 0 }; int64_t cost = 0; int ep;
    uint32_t budget = pb.budget_steps;
    for (ep = 0; ep < EP_CAP; ep++) {
        pd0_exp e; int st;
        if (strategy == 0) {
            static pd0p_proposal p;
            st = pd0p_propose(&b, &dom, &h, budget, &prior, PLAN_SEED + (uint64_t)ep, &p);
            if (st == PD0P_NO_DISCRIMINATING_EXPERIMENT) { out->no_disc = 1; break; }
            if (st != PD0P_OK) { out->valid = 0; break; }
            memcpy(&e, &p.exp, sizeof e);
        } else {
            if (pd0p_passive(&b, budget, PASSIVE_SEED, (uint32_t)ep, &e) != PD0P_OK) { out->valid = 0; break; }
        }
        if (pd0p_validate(&b, &prior, &e) != PD0P_OK) { out->valid = 0; break; }
        pd0p_prior_add(&prior, e.schedule_hash);
        pb_sched s; memset(&s, 0, sizeof s); s.n_obs = e.n_obs; s.n_steps = e.n_steps;
        for (int i = 0; i < e.n_obs; i++) s.reset[i] = e.reset[i];
        for (int k = 0; k < e.n_steps; k++) { s.channel[k] = e.steps[k].channel; s.value[k] = e.steps[k].value; cost += e.steps[k].value < 0 ? -e.steps[k].value : e.steps[k].value; }
        int64_t obs[PB_STEPS][PB_MAX_OBS]; memset(obs, 0, sizeof obs);
        int n_ok = pb_run(w, &s, obs);
        if (n_ok < 0) { out->valid = 0; break; }
        budget = pb.budget_steps > pb_steps_used(w) ? pb.budget_steps - pb_steps_used(w) : 0;
        int64_t pred[PD0_MAX_STEPS][PD0_MAX_OBS];
        rollout(truth, &e, n_ok, pred); for (int k = 0; k < n_ok; k++) acc_add(&a_truth, e.n_obs, pred[k], obs[k]);        for (int r = 0; r < n_rv; r++) { rollout(rv[r], &e, n_ok, pred); for (int k = 0; k < n_ok; k++) acc_add(&a_rv[r], e.n_obs, pred[k], obs[k]); }
        int all = 1;
        for (int r = 0; r < n_rv; r++) {
            if (!refuted[r] && acc_nrmse(&a_rv[r], e.n_obs) * 1e6 > EPS_PPM) { refuted[r] = 1; out->refuted_at[strategy][r] = ep + 1; }
            if (!refuted[r]) all = 0;
        }
        if (all) { ep++; break; }
    }
    if (acc_nrmse(&a_truth, pb.n_obs) * 1e6 > EPS_PPM) out->valid = 0;
    out->episodes[strategy] = ep; out->cost[strategy] = cost;
    for (int r = 0; r < n_rv; r++) if (!refuted[r]) out->refuted_at[strategy][r] = 0;
    pb_close(w);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <receipt-path> [commit]\n", argv[0]); return 2; }
    const char *commit = argc > 2 ? argv[2] : "unknown";
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { fprintf(stderr, "receipt exists or cannot be created: %s (%s)\n", argv[1], strerror(errno)); return 2; }
    FILE *f = fdopen(fd, "w");
    const int levels[3] = { 1, 2, 4 };
    fprintf(f, "{\n  \"receipt\": \"PD0_PLANNER_EFFICIENCY_DEV\",\n  \"claim\": \"PD-0 development evidence only; NOT an EXP-003 claim\",\n"
               "  \"commit\": \"%s\",\n  \"eps_ppm\": %d,\n  \"episode_cap\": %d,\n  \"residual_sd_micro\": %d,\n  \"recorder\": \"STAND_IN\",\n  \"rows\": [\n", commit, EPS_PPM, EP_CAP, RESID_SD_MICRO);
    int first = 1, active_wins = 0, ties = 0, passive_wins = 0, invalid = 0;
    long long sum_act = 0, sum_pas = 0;
    printf("level seed | active episodes (cost units) | passive episodes (cost units) | rival refuted at (active A,B / passive A,B)\n");
    for (int li = 0; li < 3; li++) for (uint64_t seed = 1; seed <= 5; seed++) {
        pb_bounds pb; pb_rel pbt; pb_world *w = pb_open(levels[li], seed, &pb, &pbt);
        if (!w) { fprintf(stderr, "cannot open L%d seed %llu\n", levels[li], (unsigned long long)seed); return 1; }
        pb_close(w);
        static pd0_rel truth, ra, rb; rel_from_pb(&pbt, &truth); rival_scale(&truth, &ra);
        int n_rv = 1 + rival_missing(&truth, &rb);
        const pd0_rel *rv[2] = { &ra, &rb };
        row r; memset(&r, 0, sizeof r); r.valid = 1;
        run_strategy(levels[li], seed, 0, &truth, rv, n_rv, &r);
        run_strategy(levels[li], seed, 1, &truth, rv, n_rv, &r);
        printf("L%d %llu | %d (%.2f) | %d (%.2f) | %d,%d / %d,%d%s%s\n", levels[li], (unsigned long long)seed,
               r.episodes[0], (double)r.cost[0] / 1e6, r.episodes[1], (double)r.cost[1] / 1e6,
               r.refuted_at[0][0], r.refuted_at[0][1], r.refuted_at[1][0], r.refuted_at[1][1],
               r.valid ? "" : " INVALID(truth outside eps)", r.no_disc ? " NO_DISCRIMINATING" : "");
        fprintf(f, "%s    {\"level\": %d, \"seed\": %llu, \"active_episodes\": %d, \"active_cost_micro\": %lld, \"passive_episodes\": %d, \"passive_cost_micro\": %lld, "
                   "\"active_refuted_at\": [%d, %d], \"passive_refuted_at\": [%d, %d], \"valid\": %s, \"no_discriminating\": %s}",
                first ? "" : ",\n", levels[li], (unsigned long long)seed, r.episodes[0], (long long)r.cost[0], r.episodes[1], (long long)r.cost[1],
                r.refuted_at[0][0], r.refuted_at[0][1], r.refuted_at[1][0], r.refuted_at[1][1], r.valid ? "true" : "false", r.no_disc ? "true" : "false");
        first = 0;
        if (!r.valid) invalid++;
        else { sum_act += r.episodes[0]; sum_pas += r.episodes[1]; if (r.episodes[0] < r.episodes[1]) active_wins++; else if (r.episodes[0] == r.episodes[1]) ties++; else passive_wins++; }
    }
    fprintf(f, "\n  ],\n  \"summary\": {\"active_fewer\": %d, \"ties\": %d, \"passive_fewer\": %d, \"invalid\": %d, \"total_active_episodes\": %lld, \"total_passive_episodes\": %lld}\n}\n",
            active_wins, ties, passive_wins, invalid, sum_act, sum_pas);
    fclose(f);
    printf("summary: active fewer %d, ties %d, passive fewer %d, invalid %d; total episodes active %lld vs passive %lld\n", active_wins, ties, passive_wins, invalid, sum_act, sum_pas);
    printf("PD0_PLANNER_EFF: %s\n", invalid == 0 ? "PASS" : "FAIL");
    return invalid == 0 ? 0 : 1;
}
