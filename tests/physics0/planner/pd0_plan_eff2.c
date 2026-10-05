/* PD-0 planner information-efficiency comparison, cut 2 (Direction 5), development evidence.
 *
 * Cut 1 (pd0_plan_eff.c) used rivals so weak that one episode refuted them
 * under either strategy. This cut asks the same question with hard rivals and
 * per-step accounting, so a tie at one episode cannot hide a difference.
 *
 * Rival sets per (level, seed), all built mechanically from the true relation
 * (oracle-shaped, harness side; calibration, NOT a learner result):
 *   A1  truth vs spring coefficient x1.01
 *   A2  truth vs spring coefficient x1.02
 *   B   truth vs a term whose effect is small inside the visited domain:
 *       L4 cubic coefficient x0.9 (visible near the box edge), L2 damping
 *       coefficient x0.95, L1 push coefficient x0.95 (L1 has no small term;
 *       the push term is the only one whose effect scales with what the
 *       planner chooses)
 *   C   truth vs x1.02 vs the small term removed (L1: push coefficient x0.9,
 *       since removing the push term is not a small difference)
 *
 * Metric per set and strategy: total OK steps and total |intervention| cost
 * (micro units) until EVERY rival is refuted. As in spec T6 a rival is refuted
 * by any single trial (episode) whose 20-step rollout error exceeds eps = 0.02,
 * the L0-L4 in-box bound (spec section 6.1); the error is checked after every
 * step of the trial over the steps of that trial so far, normalised by the
 * declared reset-box scale (uniform spread) because the spec normalisation
 * (trajectory sd) is undefined for the first steps; the spec-normalised value
 * at the refuting step is recorded as a cross-check. Refuted rivals leave the
 * live hypothesis set, so the planner aims the next trial at what is left. Episodes are 20
 * steps; the planner commits a whole schedule before it runs (preregistered),
 * cost counts steps up to and including the refuting step. Also recorded for
 * the active strategy: the divergence D the planner chose for the refuting
 * episode and whether that proposal was flagged by behaviour 5 (starts or
 * reaches outside the middle 50% of the visited range). The truth's own
 * rollout NRMSE must stay within eps or the row is INVALID.
 *
 * PD-0 development evidence. NOT an EXP-003 claim: no blinding, no seal, no
 * preregistered pass criterion.
 *
 * usage: pd0_plan_eff2 <receipt-path> [commit]  (receipt created with O_EXCL; never overwrites) */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pd0_planner.h"
#include "pd0p_bridge.h"

#define EPS_PPM 20000            /* level bound, L0-L4 in-box 20-step NRMSE */
#define EP_CAP 40
#define MIN_POOLED 2
#define RESID_SD_MICRO 10000
#define PLAN_SEED 2026100411ull
#define PASSIVE_SEED 2026100412ull
#define N_SETS 4
static const char *set_name[N_SETS] = { "A1 x1.01", "A2 x1.02", "B small-term", "C 3-way" };

typedef struct { double se[PD0_MAX_OBS], ss[PD0_MAX_OBS], mean[PD0_MAX_OBS]; uint64_t n; } acc;
typedef struct {
    int steps[2]; int64_t cost[2]; int episodes[2]; int refuted[2]; int declined[2]; int64_t spec_ppm[2];
    int64_t d_first, d_refuting; int b5_refuting, b5_any;
    int valid, no_disc;
} row;

static void rel_from_pb(const pb_rel *in, pd0_rel *out) {
    memset(out, 0, sizeof *out);
    out->n_vars = in->n_vars; out->n_latent = in->n_latent; out->n_channels = in->n_channels; out->n_equations = in->n_eq;
    for (int e = 0; e < in->n_eq; e++) {
        out->eq[e].target = in->eq[e].target; out->eq[e].n_terms = in->eq[e].n_terms;
        for (int t = 0; t < in->eq[e].n_terms; t++) { out->eq[e].coef[t] = in->eq[e].coef[t]; memcpy(out->eq[e].expo[t], in->eq[e].expo[t], PD0_MAX_VARS + PD0_MAX_CHAN); }
    }
    out->description_bits = pd0_rel_bits(out);
}

/* term kinds in equation 1 (target s1), by exponent pattern */
enum { T_SPRING, T_DAMP, T_CUBIC, T_PUSH };
static int find_term(const pd0_rel *r, int kind) {
    int e = r->n_equations > 1 ? 1 : 0;
    for (int t = 0; t < r->eq[e].n_terms; t++) {
        const uint8_t *x = r->eq[e].expo[t];
        int push = 0; for (int c = 0; c < r->n_channels; c++) push |= x[r->n_vars + c];
        if (kind == T_PUSH && push) return t;
        if (push) continue;
        if (kind == T_SPRING && x[0] == 1 && x[1] == 0) return t;
        if (kind == T_DAMP && x[0] == 0 && x[1] == 1) return t;
        if (kind == T_CUBIC && x[0] == 3) return t;
    }
    return -1;
}
/* scale coefficient of a term by num/den; returns 0 if the term is absent */
static int scaled(const pd0_rel *truth, int kind, int64_t num, int64_t den, pd0_rel *out) {
    memcpy(out, truth, sizeof *out);
    int e = truth->n_equations > 1 ? 1 : 0, t = find_term(truth, kind);
    if (t < 0) return 0;
    out->eq[e].coef[t] = (out->eq[e].coef[t] * num) / den;
    return 1;
}
static int removed(const pd0_rel *truth, int kind, pd0_rel *out) {
    memcpy(out, truth, sizeof *out);
    int e = truth->n_equations > 1 ? 1 : 0, t = find_term(truth, kind);
    if (t < 0) return 0;
    for (int u = t; u + 1 < out->eq[e].n_terms; u++) { out->eq[e].coef[u] = out->eq[e].coef[u + 1]; memcpy(out->eq[e].expo[u], out->eq[e].expo[u + 1], PD0_MAX_VARS + PD0_MAX_CHAN); }
    out->eq[e].n_terms--; out->description_bits = pd0_rel_bits(out);
    return 1;
}
/* small-term kind per level */
static int small_kind(int level) { return level == 4 ? T_CUBIC : level == 2 ? T_DAMP : T_PUSH; }

static void acc_add(acc *a, int n_obs, const int64_t *pred, const int64_t *obs) {
    for (int i = 0; i < n_obs; i++) { double d = (double)(pred[i] - obs[i]) / 1e6, o = (double)obs[i] / 1e6; a->se[i] += d * d; a->ss[i] += o * o; a->mean[i] += o; }
    a->n++;
}
/* per-step criterion: RMSE over the box scale (uniform spread of the declared reset box),
 * defined from the first step; the spec normalisation (running trajectory sd) is undefined
 * for the first steps of a trajectory and is recorded alongside as a cross-check */
static double acc_nrmse_box(const acc *a, int n_obs, const double *box_sd) {
    double worst = 0;
    if (a->n < MIN_POOLED) return 0;
    for (int i = 0; i < n_obs; i++) { double v = sqrt(a->se[i] / (double)a->n) / box_sd[i]; if (v > worst) worst = v; }
    return worst;
}
static double acc_nrmse(const acc *a, int n_obs) {
    double worst = 0;
    if (a->n < MIN_POOLED) return 0;
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

/* strategy 0 = active planner, 1 = passive baseline; per-step accounting */
static void run_strategy(int level, uint64_t seed, int strategy, const pd0_rel *truth, const pd0_rel *rv[2], int n_rv, row *out) {
    pb_bounds pb; pb_rel pbt;
    pb_world *w = pb_open(level, seed, &pb, &pbt);
    if (!w) { out->valid = 0; return; }
    pd0p_bounds b; memset(&b, 0, sizeof b);
    b.n_obs = pb.n_obs; b.n_channels = pb.n_channels; b.episode_max_steps = pb.episode_max_steps;
    for (int i = 0; i < pb.n_channels; i++) { b.chan_min[i] = pb.chan_min[i]; b.chan_max[i] = pb.chan_max[i]; }
    for (int i = 0; i < pb.n_obs; i++) { b.reset_min[i] = pb.reset_min[i]; b.reset_max[i] = pb.reset_max[i]; }
    pd0p_domain dom; double box_sd[PD0_MAX_OBS];
    for (int i = 0; i < pb.n_obs; i++) { dom.var_min[i] = pb.reset_min[i]; dom.var_max[i] = pb.reset_max[i]; box_sd[i] = (double)(pb.reset_max[i] - pb.reset_min[i]) / 1e6 / sqrt(12.0); }
    pd0p_hyps h; h.n_hyp = (uint8_t)(1 + n_rv); h.hyp[0] = truth; for (int k = 0; k < n_rv; k++) h.hyp[1 + k] = rv[k]; h.residual_sd_micro = RESID_SD_MICRO;
    static pd0p_prior prior; memset(&prior, 0, sizeof prior);
    acc a_truth, a_rv[2];
    int refuted[2] = { 0, 0 }, steps = 0, done = 0; int64_t cost = 0; int ep;
    uint32_t budget = pb.budget_steps;
    for (ep = 0; ep < EP_CAP && !done; ep++) {
        pd0_exp e; int st; int64_t d_here = 0; int b5 = 0;
        memset(&a_truth, 0, sizeof a_truth); memset(a_rv, 0, sizeof a_rv);   /* per trial, spec T6 */
        h.n_hyp = 1; for (int k = 0; k < n_rv; k++) if (!refuted[k]) h.hyp[h.n_hyp++] = rv[k];   /* live hypotheses only */
        if (strategy == 0) {
            static pd0p_proposal p;
            st = pd0p_propose(&b, &dom, &h, budget, &prior, PLAN_SEED + (uint64_t)ep, &p);
            if (st == PD0P_NO_DISCRIMINATING_EXPERIMENT) { out->no_disc = 1; out->declined[0] = 1; break; }
            if (st != PD0P_OK) { out->valid = 0; break; }
            memcpy(&e, &p.exp, sizeof e); d_here = p.divergence_micro; b5 = p.outside_middle;
            if (ep == 0) out->d_first = d_here;
            out->b5_any |= b5;
        } else if (pd0p_passive(&b, budget, PASSIVE_SEED, (uint32_t)ep, &e) != PD0P_OK) { out->valid = 0; break; }
        if (pd0p_validate(&b, &prior, &e) != PD0P_OK) { out->valid = 0; break; }
        pd0p_prior_add(&prior, e.schedule_hash);
        pb_sched s; memset(&s, 0, sizeof s); s.n_obs = e.n_obs; s.n_steps = e.n_steps;
        for (int i = 0; i < e.n_obs; i++) s.reset[i] = e.reset[i];
        for (int k = 0; k < e.n_steps; k++) { s.channel[k] = e.steps[k].channel; s.value[k] = e.steps[k].value; }
        int64_t obs[PB_STEPS][PB_MAX_OBS]; memset(obs, 0, sizeof obs);
        int n_ok = pb_run(w, &s, obs);
        if (n_ok < 0) { out->valid = 0; break; }
        budget = pb.budget_steps > pb_steps_used(w) ? pb.budget_steps - pb_steps_used(w) : 0;
        int64_t pt[PD0_MAX_STEPS][PD0_MAX_OBS], pr[2][PD0_MAX_STEPS][PD0_MAX_OBS];
        rollout(truth, &e, n_ok, pt);
        for (int r = 0; r < n_rv; r++) rollout(rv[r], &e, n_ok, pr[r]);
        for (int k = 0; k < n_ok && !done; k++) {
            steps++; cost += e.steps[k].value < 0 ? -e.steps[k].value : e.steps[k].value;
            acc_add(&a_truth, e.n_obs, pt[k], obs[k]);
            if (acc_nrmse_box(&a_truth, e.n_obs, box_sd) * 1e6 > EPS_PPM) out->valid = 0;   /* truth must pass every trial */
            int all = 1;
            for (int r = 0; r < n_rv; r++) {
                acc_add(&a_rv[r], e.n_obs, pr[r][k], obs[k]);
                if (!refuted[r] && acc_nrmse_box(&a_rv[r], e.n_obs, box_sd) * 1e6 > EPS_PPM) refuted[r] = 1;
                if (!refuted[r]) all = 0;
            }
            if (all) {
                done = 1;
                if (strategy == 0) { out->d_refuting = d_here; out->b5_refuting = b5; }
                double sp = 0; for (int r = 0; r < n_rv; r++) { double v = acc_nrmse(&a_rv[r], e.n_obs); if (v > sp) sp = v; }
                out->spec_ppm[strategy] = (int64_t)(sp * 1e6);
            }
        }
    }
    out->steps[strategy] = steps; out->cost[strategy] = cost; out->episodes[strategy] = ep; out->refuted[strategy] = done;
    pb_close(w);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <receipt-path> [commit]\n", argv[0]); return 2; }
    const char *commit = argc > 2 ? argv[2] : "unknown";
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { fprintf(stderr, "receipt exists or cannot be created: %s (%s)\n", argv[1], strerror(errno)); return 2; }
    FILE *f = fdopen(fd, "w");
    const int levels[3] = { 1, 2, 4 };
    fprintf(f, "{\n  \"receipt\": \"PD0_PLANNER_EFFICIENCY_DEV_CUT2\",\n  \"claim\": \"PD-0 development evidence only; NOT an EXP-003 claim\",\n"
               "  \"commit\": \"%s\",\n  \"eps_ppm\": %d,\n  \"eps_source\": \"L0-L4 in-box 20-step NRMSE bound, spec 6.1\",\n  \"min_pooled_steps\": %d,\n  \"episode_cap\": %d,\n  \"residual_sd_micro\": %d,\n  \"recorder\": \"STAND_IN\",\n"
               "  \"sets\": [\"A1 truth vs spring x1.01\", \"A2 truth vs spring x1.02\", \"B truth vs small term (L4 cubic x0.9, L2 damping x0.95, L1 push x0.95)\", \"C truth vs x1.02 vs small term removed (L1: push x0.9)\"],\n  \"rows\": [\n",
            commit, EPS_PPM, MIN_POOLED, EP_CAP, RESID_SD_MICRO);
    int first = 1, invalid = 0;
    int a_fewer[N_SETS] = {0}, ties[N_SETS] = {0}, p_fewer[N_SETS] = {0}, unref[N_SETS] = {0}, b5_sep[N_SETS] = {0}, declined[N_SETS] = {0}, spec_low[N_SETS] = {0};
    long long s_act[N_SETS] = {0}, s_pas[N_SETS] = {0}, c_act[N_SETS] = {0}, c_pas[N_SETS] = {0};
    printf("level seed set | active steps (episodes, cost units) | passive steps (episodes, cost units) | D chosen (refuting) | b5 | spec NRMSE at refute (active / passive)\n");
    for (int li = 0; li < 3; li++) for (uint64_t seed = 1; seed <= 5; seed++) {
        int level = levels[li];
        pb_bounds pb; pb_rel pbt; pb_world *w = pb_open(level, seed, &pb, &pbt);
        if (!w) { fprintf(stderr, "cannot open L%d seed %llu\n", level, (unsigned long long)seed); return 1; }
        pb_close(w);
        static pd0_rel truth, x101, x102, small, gone;
        rel_from_pb(&pbt, &truth);
        if (!scaled(&truth, T_SPRING, 101, 100, &x101) || !scaled(&truth, T_SPRING, 102, 100, &x102)) { fprintf(stderr, "no spring term L%d\n", level); return 1; }
        int sk = small_kind(level);
        if (!scaled(&truth, sk, level == 4 ? 90 : 95, 100, &small)) { fprintf(stderr, "no small term L%d\n", level); return 1; }
        if (level == 1) { if (!scaled(&truth, T_PUSH, 90, 100, &gone)) return 1; } else if (!removed(&truth, sk, &gone)) return 1;
        const pd0_rel *sets[N_SETS][2] = { { &x101, NULL }, { &x102, NULL }, { &small, NULL }, { &x102, &gone } };
        const int n_rv[N_SETS] = { 1, 1, 1, 2 };
        for (int si = 0; si < N_SETS; si++) {
            row r; memset(&r, 0, sizeof r); r.valid = 1;
            run_strategy(level, seed, 0, &truth, sets[si], n_rv[si], &r);
            run_strategy(level, seed, 1, &truth, sets[si], n_rv[si], &r);
            printf("L%d %llu %-12s | %3d (%2d, %6.2f)%s | %3d (%2d, %6.2f)%s | %.2f | %s | %.3f / %.3f%s%s\n", level, (unsigned long long)seed, set_name[si],
                   r.steps[0], r.episodes[0], (double)r.cost[0] / 1e6, r.refuted[0] ? "" : " unrefuted",
                   r.steps[1], r.episodes[1], (double)r.cost[1] / 1e6, r.refuted[1] ? "" : " unrefuted",
                   (double)r.d_refuting / 1e6, r.b5_refuting ? "yes" : "no", (double)r.spec_ppm[0] / 1e6, (double)r.spec_ppm[1] / 1e6,
                   r.valid ? "" : " INVALID(truth outside eps)", r.no_disc ? " NO_DISCRIMINATING" : "");
            fprintf(f, "%s    {\"level\": %d, \"seed\": %llu, \"set\": %d, \"active_steps\": %d, \"active_episodes\": %d, \"active_cost_micro\": %lld, \"active_refuted\": %s, "
                       "\"passive_steps\": %d, \"passive_episodes\": %d, \"passive_cost_micro\": %lld, \"passive_refuted\": %s, "
                       "\"d_first_micro\": %lld, \"d_refuting_micro\": %lld, \"b5_refuting\": %s, \"b5_any\": %s, \"spec_nrmse_at_refute_ppm\": [%lld, %lld], \"valid\": %s, \"planner_declined\": %s}",
                    first ? "" : ",\n", level, (unsigned long long)seed, si, r.steps[0], r.episodes[0], (long long)r.cost[0], r.refuted[0] ? "true" : "false",
                    r.steps[1], r.episodes[1], (long long)r.cost[1], r.refuted[1] ? "true" : "false",
                    (long long)r.d_first, (long long)r.d_refuting, r.b5_refuting ? "true" : "false", r.b5_any ? "true" : "false", (long long)r.spec_ppm[0], (long long)r.spec_ppm[1], r.valid ? "true" : "false", r.no_disc ? "true" : "false");
            first = 0;
            if (!r.valid) { invalid++; continue; }
            if (r.declined[0]) { declined[si]++; continue; }   /* planner said no experiment separates them: not a win, not a loss */
            if (!r.refuted[0] || !r.refuted[1]) unref[si]++;
            if ((r.refuted[0] && r.spec_ppm[0] <= EPS_PPM) || (r.refuted[1] && r.spec_ppm[1] <= EPS_PPM)) spec_low[si]++;
            s_act[si] += r.steps[0]; s_pas[si] += r.steps[1]; c_act[si] += r.cost[0]; c_pas[si] += r.cost[1];
            if (r.steps[0] < r.steps[1]) { a_fewer[si]++; if (r.b5_refuting) b5_sep[si]++; } else if (r.steps[0] == r.steps[1]) ties[si]++; else p_fewer[si]++;
        }
    }
    fprintf(f, "\n  ],\n  \"summary\": [\n");
    printf("summary per set (15 seeds): active fewer steps / ties / passive fewer / planner declined; total steps active vs passive (declined rows excluded); total cost active vs passive; rows unrefuted at cap; b5 flagged among active wins; refutations below spec eps\n");
    for (int si = 0; si < N_SETS; si++) {
        printf("  %-12s | %2d / %2d / %2d / %2d | %4lld vs %4lld | %8.2f vs %8.2f | %d | %d | %d\n", set_name[si], a_fewer[si], ties[si], p_fewer[si], declined[si], s_act[si], s_pas[si], (double)c_act[si] / 1e6, (double)c_pas[si] / 1e6, unref[si], b5_sep[si], spec_low[si]);
        fprintf(f, "    {\"set\": %d, \"active_fewer\": %d, \"ties\": %d, \"passive_fewer\": %d, \"total_steps_active\": %lld, \"total_steps_passive\": %lld, \"total_cost_active_micro\": %lld, \"total_cost_passive_micro\": %lld, \"planner_declined\": %d, \"unrefuted_rows\": %d, \"b5_among_active_wins\": %d, \"refutations_below_spec_eps\": %d}%s\n",
                si, a_fewer[si], ties[si], p_fewer[si], s_act[si], s_pas[si], c_act[si], c_pas[si], declined[si], unref[si], b5_sep[si], spec_low[si], si + 1 < N_SETS ? "," : "");
    }
    fprintf(f, "  ],\n  \"invalid\": %d\n}\n", invalid);
    fclose(f);
    printf("PD0_PLANNER_EFF2: %s (invalid %d)\n", invalid == 0 ? "PASS" : "FAIL", invalid);
    return invalid == 0 ? 0 : 1;
}
