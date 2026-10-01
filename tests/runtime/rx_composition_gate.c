/*
 * COMPOSITION-2 gate (WP-E): the whole causal path in one process, in fresh
 * directories, against the real World, J-Space, Cortex, Capability Graph and
 * AIENOS authority (nothing mocked). Writes a JSON receipt with one verdict
 * per step; the wrapper (tools/composition_gate.sh) passes the commit and
 * stores the receipt under its content hash. A step is PASS only when its
 * check held; the gate verdict is PASS only when every step of both runs did.
 *
 *   usage: rx_composition_gate <git-commit> <receipt-path>
 */
#include "rx_compose_fixture.h"

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#define N_STEPS 14

typedef struct {
    int pass;
    char detail[200];
} Step;

typedef struct {
    Step st[N_STEPS];
    uint8_t record_digest[32], winner_digest[32];
    JsBranchRef old_ref, new_ref, cand[RXC_K];
    uint64_t claim[RXC_K], evidence, promotion, goal_crumb;
} Run;

static const char *step_name[N_STEPS] = {
    "canonical_machine_id", "register_capability_and_pinned_skill", "create_world",
    "goal_needs_capability", "fork_alternatives", "one_fails_contract", "select_survivor",
    "commit_world", "loser_not_externalized", "seal_winner_reclaim_losers",
    "recorded_in_cortex", "close_reopen_replay", "recall_and_provenance", "deterministic_rerun"
};

static Fx g_fx;
static RxCompose g_c;

static int rm_one(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(p);
}
static void rmtree(const char *p) { nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }

#define STEP(r, i, cond, ...) do {                                         \
        (r)->st[(i) - 1].pass = !!(cond);                                  \
        snprintf((r)->st[(i) - 1].detail, sizeof (r)->st[0].detail, __VA_ARGS__); \
    } while (0)

static int ref_eq(JsBranchRef a, JsBranchRef b) { return a.id == b.id && a.gen == b.gen; }

static int all_pass(const Run *r, int upto) {
    for (int i = 0; i < upto; i++)
        if (!r->st[i].pass) return 0;
    return 1;
}

static void run_once(const char *dir, Run *r) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < N_STEPS; i++) snprintf(r->st[i].detail, sizeof r->st[i].detail, "not reached");

    /* 1. canonical machine identity: stored with the composition, a foreign
     * identity is refused. */
    memset(&g_c, 0, sizeof g_c);
    int rc = fx_open(&g_fx, &g_c, dir, 1);
    if (rc != RX_OK) { STEP(r, 1, 0, "open failed %d", rc); return; }
    fx_close(&g_fx, &g_c);
    char p[256];
    snprintf(p, sizeof p, "%s/machine.id", dir);
    AienMachineId stored, mine = g_fx.self;
    int loaded = aien_mid_load(p, &stored) == AIEN_MID_OK && aien_mid_equal(&stored, &mine);
    g_fx.self = fx_mid(2);
    memset(&g_c, 0, sizeof g_c);
    int foreign = fx_open(&g_fx, &g_c, dir, 1);
    g_fx.self = mine;
    if (foreign == RX_OK) fx_close(&g_fx, &g_c);
    STEP(r, 1, loaded && foreign == RX_ERR_IDENTITY, "stored=%d foreign_open=%d", loaded, foreign);

    /* 2. local capability + digest-pinned Skill in the Capability Graph. */
    CqKey keys[4];
    uint32_t na = sr_skill_capabilities(&g_fx.cat, FX_SKILL_A, 1, keys, 4);
    uint32_t nb = sr_skill_capabilities(&g_fx.cat, FX_SKILL_B, 1, keys, 4);
    STEP(r, 2, na == 1 && nb == 1, "skill_a_caps=%u skill_b_caps=%u", na, nb);

    /* 3. the World. */
    memset(&g_c, 0, sizeof g_c);
    rc = fx_open(&g_fx, &g_c, dir, 1);
    if (rc != RX_OK) { STEP(r, 3, 0, "open failed %d", rc); return; }
    r->old_ref = rx_compose_state(&g_c);
    JsBranchInfo bi;
    int root_ok = js_branch_info(&g_c.js, r->old_ref, &bi) == JS_OK && !bi.staged;
    STEP(r, 3, root_ok && g_c.attached && g_c.state.id == RXC_SLOT_STATE,
         "state_ref=%u/%u attached=%d", r->old_ref.id, r->old_ref.gen, g_c.attached);

    /* 4..8: one goal; Skill A breaks the contract, Skill B passes. */
    fx_a_bad = 1;
    RxcResult o;
    rc = fx_run(&g_fx, &g_c, 5, &o);
    fx_a_bad = 0;
    if (rc != RX_OK) { STEP(r, 4, 0, "run failed %d", rc); fx_close(&g_fx, &g_c); return; }
    r->goal_crumb = o.goal_crumb;
    int routed = o.n_alternatives == 2 && o.route[0].verdict == SR_OK &&
                 o.route[1].verdict == SR_OK &&
                 o.route[0].chosen.local && o.route[1].chosen.local &&
                 o.route[0].chosen.capability_id == 1 && o.route[1].chosen.capability_id == 2;
    STEP(r, 4, routed && o.goal_crumb, "alternatives=%u skills=%u,%u goal_crumb=%llu",
         o.n_alternatives, o.route[0].chosen.skill_id, o.route[1].chosen.skill_id,
         (unsigned long long)o.goal_crumb);
    r->cand[0] = o.cand_ref[0];
    r->cand[1] = o.cand_ref[1];
    int forked = (o.cand_ref[0].id || o.cand_ref[0].gen) && (o.cand_ref[1].id || o.cand_ref[1].gen) &&
                 !ref_eq(o.cand_ref[0], o.cand_ref[1]) && !ref_eq(o.cand_ref[0], r->old_ref) &&
                 !ref_eq(o.cand_ref[1], r->old_ref);
    STEP(r, 5, forked, "cand0=%u/%u cand1=%u/%u", o.cand_ref[0].id, o.cand_ref[0].gen,
         o.cand_ref[1].id, o.cand_ref[1].gen);
    const CxObject *ev = cx_get(&g_c.cx, o.cx_evidence);
    uint64_t passmask = ev && ev->n > RXC_EP_PASSMASK ? cx_payload(&g_c.cx, ev)[RXC_EP_PASSMASK] : 99;
    STEP(r, 6, passmask == 2, "passmask=%llu (bit k = candidate k met the contract)",
         (unsigned long long)passmask);
    STEP(r, 7, o.winner == 1 && o.result == 16, "winner=%u result=%llu", o.winner,
         (unsigned long long)o.result);
    r->new_ref = o.new_ref;
    STEP(r, 8, o.outcome == RXC_OUT_COMMITTED && ref_eq(rx_compose_state(&g_c), o.new_ref) &&
                   ref_eq(o.new_ref, o.cand_ref[1]),
         "outcome=%d new_ref=%u/%u", o.outcome, o.new_ref.id, o.new_ref.gen);

    /* 9. the loser is not externalized: its branch is gone and its claim is
     * admitted as a loser, never promoted. */
    uint32_t lp = 0;
    for (uint64_t id = 1; id <= g_c.cx.n; id++) {
        const CxObject *x = cx_get(&g_c.cx, id);
        if (x && x->kind == CX_K_PROMOTION && x->links[0] == o.cx_candidate[0]) lp++;
    }
    const CxObject *la = cx_get(&g_c.cx, o.cx_admission[0]);
    int loser_ok = js_branch_check(&g_c.js, o.cand_ref[0]) == JS_ERR_STALE && lp == 0 && la &&
                   la->tag == RXC_ADMIT_LOSER && la->links[0] == o.cx_candidate[0];
    STEP(r, 9, loser_ok, "loser_stale=%d loser_promotions=%u loser_admission=%llu",
         js_branch_check(&g_c.js, o.cand_ref[0]) == JS_ERR_STALE, lp,
         (unsigned long long)o.cx_admission[0]);

    /* 10. winner sealed (not staged, no owner), losers reclaimed, the
     * superseded branch released. */
    int winfo = js_branch_info(&g_c.js, o.new_ref, &bi) == JS_OK;
    int sealed = winfo && !bi.staged && bi.owner == 0 && bi.locality == JS_HOME_LOCAL;
    int old_stale = js_branch_check(&g_c.js, r->old_ref) == JS_ERR_STALE;
    STEP(r, 10, sealed && old_stale && o.reclaimed >= 1 && fx_live_branches(&g_c.js) == 1,
         "winner_sealed=%d old_stale=%d reclaimed=%u live=%u", sealed, old_stale, o.reclaimed,
         fx_live_branches(&g_c.js));

    /* 11. recorded in Cortex. */
    const CxObject *pr = cx_get(&g_c.cx, o.cx_promotion);
    const CxObject *wc = cx_get(&g_c.cx, o.cx_candidate[1]);
    int rec = pr && wc && ev && pr->kind == CX_K_PROMOTION && pr->links[0] == o.cx_candidate[1] &&
              pr->links[1] == o.cx_evidence && ev->protect == CX_PROT_VERIFY_EVIDENCE &&
              cx_payload(&g_c.cx, wc)[RXC_CP_REF] == fx_pack(o.new_ref);
    r->claim[0] = o.cx_candidate[0];
    r->claim[1] = o.cx_candidate[1];
    r->evidence = o.cx_evidence;
    r->promotion = o.cx_promotion;
    STEP(r, 11, rec, "claims=%llu,%llu evidence=%llu promotion=%llu",
         (unsigned long long)o.cx_candidate[0], (unsigned long long)o.cx_candidate[1],
         (unsigned long long)o.cx_evidence, (unsigned long long)o.cx_promotion);
    fx_close(&g_fx, &g_c);

    /* 12. close + reopen: the journal replays and verifies, the World names
     * the durable winner, the loser is not recoverable. */
    memset(&g_c, 0, sizeof g_c);
    rc = fx_open(&g_fx, &g_c, dir, 1);
    if (rc != RX_OK) { STEP(r, 12, 0, "reopen failed %d", rc); return; }
    uint8_t dg[32];
    int same = js_branch_content_digest(&g_c.js, r->new_ref.id, dg) == JS_OK &&
               memcmp(dg, o.winner_digest, 32) == 0;
    int replay = cx_verify_chain(&g_c.cx) == CX_OK && ref_eq(rx_compose_state(&g_c), r->new_ref) &&
                 same && js_branch_check(&g_c.js, r->cand[0]) == JS_ERR_STALE &&
                 g_c.rolled_back == 0 && g_c.recovered_completed == 0;
    STEP(r, 12, replay, "records=%llu state=%u/%u content_match=%d rolled_back=%u",
         (unsigned long long)g_c.cx.n, rx_compose_state(&g_c).id, rx_compose_state(&g_c).gen,
         same, g_c.rolled_back);
    memcpy(r->winner_digest, dg, 32);

    /* 13. recall + provenance chain: promotion -> claim -> candidate World
     * record (branch ref) and goal World record (outside crumb). */
    CxFilter f = { CX_CLAIM, CX_K_CANDIDATE, 0, 1 };
    CxRecord recs[16];
    uint32_t nr = cx_recall(&g_c.cx, RXC_CX_SUBJECT(RXC_SLOT_STATE), 0, UINT64_MAX, &f, recs, 16);
    int recalled = 0;
    for (uint32_t i = 0; i < nr; i++)
        if (recs[i].hdr.id == r->claim[1] && recs[i].verified &&
            recs[i].payload[RXC_CP_REF] == fx_pack(r->new_ref) &&
            (uint32_t)recs[i].payload[RXC_CP_SKILL] == FX_SKILL_B)
            recalled = 1;
    uint64_t prov[64];
    uint32_t np = cx_provenance(&g_c.cx, r->promotion, prov, 64);
    int goal_ok = 0, cand_ok = 0;
    for (uint32_t i = 0; i < np; i++) {
        const CxObject *x = cx_get(&g_c.cx, prov[i]);
        if (!x) continue;
        const uint64_t *xp = cx_payload(&g_c.cx, x);
        if (x->kind == CX_K_WORK_ACCEPTED && xp[CX_WREC_CRUMB] == r->goal_crumb) goal_ok = 1;
        if (x->kind == CX_K_EXEC_COMMIT && x->subject == RXC_CX_SUBJECT(RXC_SLOT_CAND1) &&
            xp[CX_WREC_FIELD0 + RXC_C_REF] == fx_pack(r->new_ref))
            cand_ok = 1;
    }
    STEP(r, 13, recalled && goal_ok && cand_ok, "recalled=%d provenance=%u goal=%d candidate=%d",
         recalled, np, goal_ok, cand_ok);
    rx_compose_record_digest(&g_c.cx, r->record_digest);
    fx_close(&g_fx, &g_c);
}

static void hex(const uint8_t *d, char *out) { fx_hex(d, 32, out); }

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <git-commit> <receipt-path>\n", argv[0]);
        return 2;
    }
    const char *commit = argv[1], *outp = argv[2];
    char base[128];
    snprintf(base, sizeof base, "/tmp/rx_composition_gate.XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
    if (fx_init(&g_fx) != 0) { fprintf(stderr, "fixture init failed\n"); return 2; }

    static Run run[2];
    for (int i = 0; i < 2; i++) {
        char d[200];
        snprintf(d, sizeof d, "%s/run%d", base, i);
        run_once(d, &run[i]);
    }
    int same_rec = memcmp(run[0].record_digest, run[1].record_digest, 32) == 0;
    int same_win = memcmp(run[0].winner_digest, run[1].winner_digest, 32) == 0;
    int both = all_pass(&run[0], N_STEPS - 1) && all_pass(&run[1], N_STEPS - 1);
    char h0[65], h1[65], w0[65], w1[65];
    hex(run[0].record_digest, h0);
    hex(run[1].record_digest, h1);
    hex(run[0].winner_digest, w0);
    hex(run[1].winner_digest, w1);
    for (int i = 0; i < 2; i++)
        STEP(&run[i], 14, both && same_rec && same_win && (run[0].winner_digest[0] | run[0].winner_digest[1]) != 0,
             "record_digest_equal=%d winner_digest_equal=%d both_runs_pass=%d", same_rec, same_win,
             both);
    int pass = all_pass(&run[0], N_STEPS) && all_pass(&run[1], N_STEPS);

    char mid_hex[2 * AIEN_MID_ID_BYTES + 1];
    fx_hex(g_fx.self.id, AIEN_MID_ID_BYTES, mid_hex);
    char sk_a[65], sk_b[65];
    fx_hex(g_fx.skills.skill[0].identity, 32, sk_a);
    fx_hex(g_fx.skills.skill[1].identity, 32, sk_b);

    FILE *f = fopen(outp, "w");
    if (!f) { perror(outp); return 2; }
    fprintf(f, "{\n  \"gate\": \"COMPOSITION-2\",\n  \"commit\": \"%s\",\n", commit);
    fprintf(f, "  \"verdict\": \"%s\",\n  \"machine_id\": \"%s\",\n", pass ? "PASS" : "FAIL", mid_hex);
    fprintf(f, "  \"skills\": [{\"id\": %u, \"version\": 1, \"digest\": \"%s\"}, "
               "{\"id\": %u, \"version\": 1, \"digest\": \"%s\"}],\n",
            FX_SKILL_A, sk_a, FX_SKILL_B, sk_b);
    fprintf(f, "  \"record_digest\": [\"%s\", \"%s\"],\n", h0, h1);
    fprintf(f, "  \"winner_digest\": [\"%s\", \"%s\"],\n", w0, w1);
    fprintf(f, "  \"runs\": [\n");
    for (int i = 0; i < 2; i++) {
        Run *r = &run[i];
        fprintf(f, "    {\"run\": %d, \"old_ref\": \"%u/%u\", \"new_ref\": \"%u/%u\", "
                   "\"candidate_refs\": [\"%u/%u\", \"%u/%u\"], \"goal_crumb\": %llu, "
                   "\"claims\": [%llu, %llu], \"evidence\": %llu, \"promotion\": %llu,\n"
                   "     \"steps\": [\n",
                i, r->old_ref.id, r->old_ref.gen, r->new_ref.id, r->new_ref.gen, r->cand[0].id,
                r->cand[0].gen, r->cand[1].id, r->cand[1].gen, (unsigned long long)r->goal_crumb,
                (unsigned long long)r->claim[0], (unsigned long long)r->claim[1],
                (unsigned long long)r->evidence, (unsigned long long)r->promotion);
        for (int s = 0; s < N_STEPS; s++)
            fprintf(f, "       {\"step\": %d, \"name\": \"%s\", \"result\": \"%s\", \"detail\": \"%s\"}%s\n",
                    s + 1, step_name[s], r->st[s].pass ? "PASS" : "FAIL", r->st[s].detail,
                    s + 1 < N_STEPS ? "," : "");
        fprintf(f, "     ]}%s\n", i == 0 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);

    for (int s = 0; s < N_STEPS; s++)
        printf("  step %2d %-38s %s / %s  %s\n", s + 1, step_name[s], run[0].st[s].pass ? "PASS" : "FAIL",
               run[1].st[s].pass ? "PASS" : "FAIL", run[0].st[s].detail);
    printf("COMPOSITION-2 gate: %s\n", pass ? "PASS" : "FAIL");
    fx_free(&g_fx);
    rmtree(base);
    return pass ? 0 : 1;
}
