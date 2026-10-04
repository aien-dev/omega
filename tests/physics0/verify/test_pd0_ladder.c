/* The ladder checker: an honest oracle-shaped bundle reaches PROVISIONAL_LAW; every dishonest mutant stops for the recorded reason. */
#include "bundle.h"
#include "pd0_receipt.h"
#include "sha256.h"
#include <stdio.h>
static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_CODE(rc, R, want) do { g_checks++; if ((rc) != (want) && (R).code != (want) && (R).stall_code != (want)) { g_fail++; fprintf(stderr, "FAIL %s:%d: rc=%d (%s) R.code=%d (%s) state=%s want %s\n", __FILE__, __LINE__, rc, pd0v_name(rc), (R).code, pd0v_name((R).code), pd0_ladder_state_name((R).state), #want); } } while (0)
static const char *g_dir = "build/tests-physics0-verify/receipts";
static void receipt(const char *kind, const bundle *b, int code) { uint8_t h[32]; sha256_hash(b->buf, b->len, h); char path[1024]; if (pd0_receipt_write(g_dir, kind, h, code, NULL, path, sizeof path)) fprintf(stderr, "receipt write failed for %s\n", kind); }
static int run(int level, uint64_t seed, const bundle_opts *o, pd0_ladder_report *R, uint8_t *law, size_t *law_len, const char *kind)
{
    bundle b; b_init(&b, level, seed); int rc = b_build(&b, o, R); if (law) *law_len = pd0_ladder_emit_law(R, &b.P, law, 8192); receipt(kind, &b, rc ? rc : R->code); b_free(&b); return rc;
}
int main(void)
{
    static uint8_t law[8192], law2[8192]; size_t ln = 0; pd0_ladder_report R; bundle_opts o; pd0_ladder_params P; pd0_ladder_params_default(&P, 2, 1);
    /* honest, three levels and several seeds */
    int levels[] = { 1, 2, 4 };
    for (int li = 0; li < 3; li++) for (uint64_t seed = 1; seed <= 3; seed++) {
        memset(&o, 0, sizeof o); int rc = run(levels[li], seed, &o, &R, law, &ln, "ladder-honest");
        CHECK(rc == 0 && R.state == LS_REPLICATED); CHECK(R.n_refutations == 0 && R.p == 35 && R.f == 0 && R.confidence_ppm == pd0_confidence_ppm(35, 0));
        CHECK(ln > 0 && pd0_ladder_verify_law(&R, &P, law, ln) == PD0V_OK);
        pd0_law *l = calloc(1, sizeof *l); CHECK(pd0_law_parse(law, ln, l) == 0 && l->state == PDLAW_PROVISIONAL_LAW && l->n_experiments == 8 && l->rel.n_refutations == 0);
        if (li == 0 && seed == 1) printf("claim: %.*s\n", (int)l->claim_len, l->claim);
        /* T8 tampering on the honest law */
        pd0_law m = *l; m.confidence_ppm += 1; size_t n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_CONFIDENCE_MISMATCH);
        m = *l; memcpy(m.claim, "This is always true. ", 21); n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_CLAIM_FORBIDDEN_WORD);
        m = *l; m.claim[0] = 'B'; n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_CLAIM_TEXT_MISMATCH);
        m = *l; m.n_experiments--; n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_EXPERIMENT_MISSING);
        m = *l; m.chain_root[0] ^= 1; n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_CHAIN_ROOT_MISMATCH);
        m = *l; m.rel.eq[1].coef[0] += 1; n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_RELATION_MISMATCH);
        m = *l; m.dom.n_observations += 1; n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_DOMAIN_MISMATCH);
        m = *l; m.n_exceptions = 1; n = pd0_law_write(&m, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_EXCEPTIONS_MISMATCH);
        memcpy(law2, law, ln); law2[ln - 1] ^= 1; CHECK(pd0_ladder_verify_law(&R, &P, law2, ln) == PD0V_LAW_ID_MISMATCH);
        free(l); }
    /* mutants: each stops for the recorded reason */
    memset(&o, 0, sizeof o); o.skip_corr = 1; int rc = run(1, 1, &o, &R, law, &ln, "ladder-skip-state"); CHECK_CODE(rc, R, PD0V_T8_STATE_SKIPPED); CHECK(R.state == LS_OBSERVATION);
    memset(&o, 0, sizeof o); o.fake_corr = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-fake-corr"); CHECK_CODE(rc, R, PD0V_T2_CORR_MISMATCH); CHECK(R.state == LS_OBSERVATION);
    memset(&o, 0, sizeof o); o.no_rival = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-no-rival"); CHECK_CODE(rc, R, PD0V_T4_NO_RIVAL); CHECK(R.state == LS_CANDIDATE);
    memset(&o, 0, sizeof o); o.eps_above_bound = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-eps-above-bound"); CHECK_CODE(rc, R, PD0V_T4_EPS_ABOVE_BOUND); CHECK(R.state == LS_CANDIDATE);
    memset(&o, 0, sizeof o); o.reuse_schedule = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-reused-schedule"); CHECK_CODE(rc, R, PD0V_T5_SCHEDULE_REUSED);
    memset(&o, 0, sizeof o); o.trial_without_prereg = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-trial-not-prereg"); CHECK_CODE(rc, R, PD0V_T6_TRIAL_NOT_PREREGISTERED);
    memset(&o, 0, sizeof o); o.rep_before_candidate = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-rep-in-fit"); CHECK_CODE(rc, R, PD0V_REP_INFLUENCED_FIT);
    memset(&o, 0, sizeof o); o.batch_seed_reuse = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-stream-reuse"); CHECK_CODE(rc, R, PD0V_T7_STREAM_REUSED);
    memset(&o, 0, sizeof o); o.all_planner = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-origin-imbalance"); CHECK_CODE(rc, R, PD0V_T7_ORIGIN_IMBALANCE);
    /* wrong constant: the committed predictions miss; the hypothesis is refuted, not promoted */
    memset(&o, 0, sizeof o); o.wrong_k = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-wrong-k"); CHECK((rc == 0 || rc == PD0V_T8_STATE_SKIPPED) && R.n_refutations == 1 && R.state == LS_CANDIDATE && R.n_exceptions == 1 && R.f >= 1 && R.last_nrmse_micro > 20000); /* refuted on trial 1; later preregs without a new T4 are a skipped state */
    CHECK(pd0_ladder_verify_law(&R, &P, law, ln) == 0); { pd0_law *l = calloc(1, sizeof *l); CHECK(pd0_law_parse(law, ln, l) == 0 && l->state != PDLAW_PROVISIONAL_LAW); free(l); }
    /* partial ladders: a PROVISIONAL_LAW record is refused as a skipped state */
    memset(&o, 0, sizeof o); o.stop_after_prereg = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-stop-prereg"); CHECK(rc == 0 && R.state == LS_PREDICTED && R.code == PD0V_T6_TRIAL_MISSING);
    { pd0_law *l = calloc(1, sizeof *l); pd0_law_parse(law, ln, l); l->state = PDLAW_PROVISIONAL_LAW; size_t n = pd0_law_write(l, law2, sizeof law2); CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_T8_STATE_SKIPPED); free(l); }
    memset(&o, 0, sizeof o); o.stop_after_trials = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-stop-trials"); CHECK(rc == 0 && R.state == LS_INTERVENED && R.code == PD0V_T7_TOO_FEW_BATCHES);
    /* fabricated law with no evidence at all (M4) */
    { static uint8_t empty[64]; pd0_ladder_report R0; CHECK(pd0_ladder_check(empty, 0, &P, &R0) == 0 && R0.state == LS_START);
      pd0_law *l = calloc(1, sizeof *l); l->state = PDLAW_PROVISIONAL_LAW; l->rel.n_vars = 2; l->rel.n_channels = 1; l->dom.n_obs = 2; l->dom.n_channels = 1; size_t n = pd0_law_write(l, law2, sizeof law2);
      CHECK(pd0_ladder_verify_law(&R0, &P, law2, n) == PD0V_T8_STATE_SKIPPED); free(l); }
    /* demotion: a later contradicting trial pulls the law back to HYPOTHESIS */
    /* spec rev 6 (b): EPISODE_END records carry an outcome; an honest ladder whose every episode ends with one still replicates */
    memset(&o, 0, sizeof o); o.end_last = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-episode-end"); CHECK(rc == 0 && R.state == LS_REPLICATED && R.n_refutations == 0);
    /* spec rev 6 (a): trial error is normalised by the pooled FIT sd, not the within-trial spread: quiet trials with
     * observation noise (sd 0.02, eps 0.02 x 3 batches eps) must not refute the true relation */
    memset(&o, 0, sizeof o); o.quiet_trials = 1; o.obs_noise = 20000; rc = run(1, 1, &o, &R, law, &ln, "ladder-quiet-noisy-trials"); CHECK(rc == 0 && R.state >= LS_INTERVENED && R.n_refutations == 0);
    { int64_t pred[6] = { 1000, 0, 2000, 0, 3000, 0 }, obs[6] = { 11000, 0, 12000, 0, 13000, 0 }, sd[2] = { 1000000, 1000000 };
      CHECK(pd0_nrmse_micro(pred, obs, 3, 2) > 10000000);                  /* within-trial spread 0.0008: error reads as 12 sd */
      CHECK(pd0_nrmse_pooled_micro(pred, obs, 3, 2, sd) == 10000);          /* pooled sd 1.0: the same error is 0.01 */
      int64_t sd0[2] = { 0, 0 }; CHECK(pd0_nrmse_pooled_micro(pred, obs, 3, 2, sd0) == 10000); }   /* sd 0 falls back to the RMSE */
    /* spec rev 7: void TRIAL/REP episodes (OUT_OF_BOUNDS before the horizon) are replaced and never count toward p or f */
    memset(&o, 0, sizeof o); o.void_rep = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-void-rep"); CHECK(rc == 0 && R.state == LS_REPLICATED && R.n_void_episodes == 1 && R.n_void_batches == 0 && R.p == 35 && R.f == 0);
    memset(&o, 0, sizeof o); o.void_rep_four = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-void-batch"); CHECK(rc == 0 && R.state == LS_REPLICATED && R.n_void_batches == 1 && R.n_void_episodes == 4 && R.p == 35 && R.f == 0);
    memset(&o, 0, sizeof o); o.void_trial = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-void-trial"); CHECK(rc == 0 && R.state == LS_REPLICATED && R.n_void_episodes == 1 && R.p == 35 && R.f == 0 && R.n_experiments == 8);
    memset(&o, 0, sizeof o); o.demote_after_law = 1; rc = run(1, 1, &o, &R, law, &ln, "ladder-demotion"); CHECK(rc == 0 && R.demoted && R.state == LS_HYPOTHESIS && R.n_refutations == 1);
    { bundle b; b_init(&b, 1, 1); bundle_opts h; memset(&h, 0, sizeof h); pd0_ladder_report Rh; b_build(&b, &h, &Rh); size_t n = pd0_ladder_emit_law(&Rh, &b.P, law2, sizeof law2); b_free(&b);
      CHECK(pd0_ladder_verify_law(&R, &P, law2, n) == PD0V_LAW_DEMOTED); }
    printf("test_pd0_ladder: %d checks, %d failed\n", g_checks, g_fail); return g_fail ? 1 : 0;
}
