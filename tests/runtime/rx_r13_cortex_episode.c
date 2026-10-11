/* G1: the R13 living body journals one positive episode into canonical Cortex.
 *
 * Reuses the whole R13 rig by including rx_r13_living.c (its main renamed).
 * Attaches the existing World->Cortex recorder (rx_cortex_record.h) to the
 * living world on a journal file, runs the POSITIVE episode, finds the lesson
 * lesson and evidence reference the BODY wrote (generation.prepare/promote), then proves
 * the provenance chain, the digest-gated promotion, tamper refusal and restart. */
#define main r13_main_unused
#include "rx_r13_living.c"
#undef main

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) { g_pass++; printf("  ok   "); } else { g_fail++; printf("  FAIL "); } \
    printf(__VA_ARGS__); printf("\n"); } while (0)

static void words(const uint8_t d[32], uint64_t w[4]) {
    for (int i = 0; i < 4; i++) { w[i] = 0; for (int b = 0; b < 8; b++) w[i] |= (uint64_t)d[8 * i + b] << (8 * b); }
}

enum { N_STEP = 12 };
static const char *const g_step[N_STEP] = {"observation", "belief", "prediction", "hypothesis", "plan",
    "search", "gpu-input", "experiment", "evidence", "exp-belief", "candidate", "promotion"};

/* Cortex record of the newest crumb that wrote any field of `o`. */
static uint64_t rec_of(Rig *r, RxObjRef o) {
    uint64_t best = 0;
    for (uint32_t fi = 0; fi < RX_MAX_FIELDS; fi++) {
        uint64_t id = rx_cortex_record_of(&r->w, rx_world_explain(&r->w, o, fi));
        if (id > best) best = id;
    }
    return best;
}
/* Nearest ancestor of `from` (provenance, nearest first) written for object slot `slot`. */
static uint64_t nearest(const CxStore *s, uint64_t from, uint32_t slot) {
    static uint64_t p[4096];
    uint32_t n = cx_provenance(s, from, p, 4096);
    for (uint32_t i = 0; i < n; i++) if (cx_get(s, p[i])->subject == (uint64_t)slot + 1u && cx_get(s, p[i])->kind != CX_K_ENTITY_CREATED) return p[i];
    return 0;
}
/* Protected records, copied for the byte-identical comparison. */
typedef struct { uint32_t n; uint64_t id[8192]; CxObject o[8192]; uint64_t pl[8192][CX_WREC_WORDS + 8]; } Prot;
static void snap(CxStore *s, Prot *p) {
    p->n = 0;
    for (uint64_t i = 1; i <= s->n && p->n < 8192; i++) {
        const CxObject *o = cx_get(s, i);
        if (!o->protect || o->n > CX_WREC_WORDS + 8) continue;
        p->id[p->n] = i; p->o[p->n] = *o;
        memcpy(p->pl[p->n], cx_payload(s, o), o->n * sizeof(uint64_t));
        p->n++;
    }
}
static int corrupt_copy(const char *src, const char *dst, long off) {
    FILE *a = fopen(src, "rb"), *b = fopen(dst, "wb");
    if (!a || !b) return -1;
    int c; long i = 0;
    while ((c = fgetc(a)) != EOF) { if (i++ == off) c ^= 0x01; fputc(c, b); }
    fclose(a); fclose(b);
    return 0;
}


/* ---- C2: a second life after a restart ----
 * A fresh body (new World and faculties, same R9 store) starts from the
 * journal. Before AIEN plans, its explored-condition memory is seeded from
 * the PROMOTED lessons in Cortex. `to_x`: the body moves to the X925 cores
 * (the condition the first life explored); otherwise it stays on the A725. */
typedef struct { uint32_t seeded; uint64_t status, plan, mem_count, mem_key; } Outcome;
static int episode(CxStore *src, int to_x, Outcome *o) {
    memset(o, 0, sizeof *o);
    Rig *r = calloc(1, sizeof *r);
    cpu_set_t a, x;
    if (!r || start(r, POSITIVE) != 0 || core_sets(&a, &x) != 0 || move_threads(&a) <= 0) return -1;
    uint64_t keys[RX_AIEN_MEMORY_SLOTS];
    o->seeded = src ? rx_living_explored_keys(src, (uint64_t)r->living.o.evidence.id + 1u, keys, RX_AIEN_MEMORY_SLOTS) : 0;
    if (o->seeded) {
        RxCapRef mc = mint(r, EXTERNAL, RX_AIEN_RES_BASE + RX_AIEN_RES_MEMORY, RX_RIGHT_WRITE);
        if (rx_aien_seed_explored(&r->aien, mc, keys, o->seeded) != RX_OK) return -1;
    }
    int bad = publish_placement(r, CLASS_A725) != 0;
    uint64_t limit = run_clock(r) + 60000000000ull;
    while (!bad && field(r, r->omega.o.selection, 0) < 1 && run_clock(r) < limit) bad = serve(r) != 0;
    while (!bad && field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED && run_clock(r) < limit) bad = serve(r) != 0;
    if (bad || field(r, r->aien.o.prediction, 6) != RX_AIEN_PRED_CONFIRMED) { stop(r); free(r); return -2; }
    uint64_t target = field(r, r->aien.o.prediction, 5) * TARGET_PCT / 100u;
    if (start_producer(r) != 0) { stop(r); free(r); return -1; }
    if (to_x && (move_threads(&x) <= 0 || publish_placement(r, CLASS_X925) != 0)) bad = 1;
    if (bad || publish_goal(r, target) != 0) { stop_producer(r); stop(r); free(r); return -1; }
    limit = run_clock(r) + 60000000000ull;
    while (run_clock(r) < limit && !atomic_load(&r->producer_error)) {
        pause_us(1000);
        o->plan = field(r, r->aien.o.plan, 0);
        o->status = field(r, r->aien.o.assessment, 4);
        if (o->plan || o->status == RX_AIEN_GOAL_UNMET_EXPLORED) break;
    }
    o->mem_count = field(r, r->aien.o.memory, 0);
    o->mem_key = field(r, r->aien.o.plan, 3);
    stop_producer(r);
    stop(r);
    free(r);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (operator_state(NULL) != 0 || seen_fit(1u << 17) != 0) return 2;
    char dir[192], jr[256], jr2[256];
    if (scratch_dir(dir, sizeof dir, "r13-cortex") != 0) return 2;
    snprintf(jr, sizeof jr, "%s/episode.cxj", dir);
    snprintf(jr2, sizeof jr2, "%s/episode-flipped.cxj", dir);
    Rig *r = calloc(1, sizeof *r);
    static CxStore s, s2;
    if (!r) return 2;
    if (start(r, POSITIVE) != 0) { fprintf(stderr, "setup failed at stage %d\n", g_stage); return 1; }
    int rc = cx_open(&s, jr, RX_CORTEX_SUBJECTS, 0);
    CHECK(rc == CX_OK, "journal opened (rc %d)", rc);
    rc = rx_cortex_attach(&r->w, &s, 1);
    CHECK(rc == RX_OK, "recorder attached to the living world (rc %d)", rc);
    rc = run(r, POSITIVE);
    CHECK(rc == 0, "R13 POSITIVE episode ran to promotion and in force (rc %d)", rc);
    if (rc != 0 || g_fail) { printf("R13_CORTEX_EPISODE FAIL %d pass %d fail %d\n", 1, g_pass, g_fail); stop(r); return 1; }
    rx_world_wait_quiescent(&r->w, 5000);
    uint64_t recs = 0, errs = 0;
    rx_cortex_status(&r->w, &recs, &errs);
    CHECK(recs > 0 && errs == 0, "recorder wrote %llu records, %llu errors", U(recs), U(errs));

    /* 1. a Cortex record for each crumb of the episode, and the chain */
    RxObjRef ref[N_STEP] = {r->aien.o.placement, r->aien.o.belief, r->aien.o.prediction,
        r->aien.o.hypothesis, r->aien.o.plan, r->omega.o.search, r->living.o.input,
        r->living.o.output, r->living.o.evidence, r->aien.o.experiment_belief,
        r->living.o.candidate, r->living.o.promotion};
    uint64_t id[N_STEP];
    for (int i = 0; i < N_STEP; i++) {
        id[i] = rec_of(r, ref[i]);
        const CxObject *o = id[i] ? cx_get(&s, id[i]) : NULL;
        CHECK(o != NULL, "%-12s -> cortex record %llu kind 0x%x", g_step[i], U(id[i]), o ? o->kind : 0);
    }
    /* Ground truth of the POSITIVE run (see the report): two causal chains meet in the same world.
     * A (action): goal -> assessment -> plan -> experiment -> evidence -> experiment belief -> candidate -> promotion.
     * B (sense):  observation -> belief -> prediction (failed) -> hypothesis.
     * Each link below is the nearest cx_provenance ancestor of the one before. */
    uint64_t as = rec_of(r, r->aien.o.assessment);
    uint64_t ca[6], nca = 0;
    uint64_t cur = id[8];
    RxObjRef chainA[] = {r->living.o.output, r->aien.o.plan, r->aien.o.assessment, r->aien.o.goal};
    char line[400]; int ln = snprintf(line, sizeof line, "A: evidence %llu", U(id[8]));
    for (unsigned i = 0; i < 4; i++) {
        uint64_t nx = cur ? nearest(&s, cur, chainA[i].id) : 0;
        ca[nca++] = nx;
        ln += snprintf(line + ln, sizeof line - ln, " <- %s %llu", i == 0 ? "experiment" : i == 1 ? "plan" : i == 2 ? "assessment" : "goal", U(nx));
        if (nx) cur = nx;
    }
    printf("CHAIN %s\n", line);
    CHECK(ca[0] && ca[1] && ca[2] && ca[3] && cx_get(&s, ca[3])->kind == CX_K_WORK_ACCEPTED && as,
          "chain A: evidence <- experiment <- plan <- assessment <- goal (outside input) is in the Cortex provenance");
    CxFilter fc = {CX_EXECUTION, CX_K_EXEC_COMMIT, 0, 1};
    CxRecord hr;
    uint64_t hyp = cx_recall(&s, (uint64_t)r->aien.o.hypothesis.id + 1u, 0, UINT64_MAX, &fc, &hr, 1) ? hr.hdr.id : 0;
    uint64_t pr = hyp ? nearest(&s, hyp, r->aien.o.prediction.id) : 0;
    uint64_t be = pr ? nearest(&s, pr, r->aien.o.belief.id) : 0;
    uint64_t ob = 0;
    if (be) { uint64_t p[4096]; uint32_t n = cx_provenance(&s, be, p, 4096);
        for (uint32_t i = 0; i < n && !ob; i++) if (cx_get(&s, p[i])->kind == CX_K_WORK_ACCEPTED) ob = p[i]; }
    printf("CHAIN B: hypothesis %llu <- prediction %llu <- belief %llu <- observation %llu\n", U(hyp), U(pr), U(be), U(ob));
    CHECK(hyp && pr && be && ob, "chain B: hypothesis <- prediction <- belief <- observation (outside input) is in the Cortex provenance");
    CHECK(!ob || cx_get(&s, ob)->subject != 0, "observation record is an outside input about object slot %llu", U(ob ? cx_get(&s, ob)->subject - 1 : 0));
    {   /* the lesson side: later records descend from evidence */
        uint64_t p[4096]; uint32_t n = cx_provenance(&s, id[10], p, 4096), has = 0, has_eb = 0;
        for (uint32_t i = 0; i < n; i++) { has |= p[i] == id[8]; has_eb |= p[i] == id[9]; }
        printf("candidate %llu has evidence %llu in its provenance: %u; experiment belief %llu: %u\n", U(id[10]), U(id[8]), has, U(id[9]), has_eb);
        uint64_t q[4096]; uint32_t m = cx_provenance(&s, id[11], q, 4096), hc = 0;
        for (uint32_t i = 0; i < m; i++) hc |= q[i] == id[10];
        CHECK(hc, "promotion %llu descends from candidate %llu", U(id[11]), U(id[10]));
    }
    CHECK(cx_verify_chain(&s) == CX_OK, "cx_verify_chain passes (%llu records)", U(s.n));

    /* 2. lesson candidate and digest-bound evidence */
    RxObject ev;
    rx_world_read(&r->w, r->living.o.evidence, &ev);
    uint8_t dg[32];
    rx_living_object_digest(ev.type, ev.field, dg);
    CHECK(memcmp(dg, ev.digest, 32) == 0, "rx_living_object_digest equals the World's digest of the evidence object");
    uint64_t sub = (uint64_t)r->living.o.evidence.id + 1u;
    uint64_t dw[4]; words(dg, dw);
    /* The lesson records are written by the body (generation.prepare and
     * generation.promote); the test only finds them. */
    uint64_t ev_ref = 0, cand = 0, prom = 0, n_ref = 0, n_cand = 0, n_prom = 0;
    for (uint64_t i = 1; i <= s.n; i++) {
        const CxObject *o = cx_get(&s, i);
        if (o->subject != sub) continue;
        if (o->kind == CX_K_EVIDENCE_REF) { ev_ref = i; n_ref++; }
        if (o->kind == CX_K_CANDIDATE) { cand = i; n_cand++; }
        if (o->kind == CX_K_PROMOTION) { prom = i; n_prom++; }
    }
    CHECK(n_ref == 1 && n_cand == 1 && n_prom == 1, "body wrote one evidence-ref (%llu), one lesson candidate (%llu), one promotion (%llu)",
          U(ev_ref), U(cand), U(prom));
    if (n_ref != 1 || n_cand != 1 || n_prom != 1) { printf("R13_CORTEX_EPISODE FAIL %d pass %d fail %d\n", 1, g_pass, g_fail); stop(r); return 1; }
    const CxObject *co = cx_get(&s, cand), *eo = cx_get(&s, ev_ref), *po = cx_get(&s, prom);
    CHECK(co->links[0] == id[9] && co->links[1] == ev_ref && co->links[2] == id[8],
          "lesson links {experiment belief %llu, evidence ref %llu, evidence %llu}", U(co->links[0]), U(co->links[1]), U(co->links[2]));
    CHECK(eo->links[0] == id[8] && eo->links[1] == id[9] && (eo->protect & CX_PROT_VERIFY_EVIDENCE) && eo->tag == co->tag,
          "evidence ref links {evidence %llu, experiment belief %llu}, protected, same epoch tag %llu", U(eo->links[0]), U(eo->links[1]), U(eo->tag));
    const uint64_t *cp = cx_payload(&s, co), *ep = cx_payload(&s, eo);
    CHECK(eo->n == RX_LIVING_EV_WORDS && memcmp(ep, dw, sizeof dw) == 0 && ep[RX_LIVING_EV_CRUMB] == rx_world_explain(&r->w, r->living.o.evidence, 0),
          "evidence ref holds the World digest of the evidence object and its crumb");
    CHECK(co->n == RX_LIVING_LESSON_WORDS && cp[1] == ev.field[0] &&
          cp[RX_LIVING_LESSON_KEY] == rx_aien_key(field(r, r->aien.o.plan, 3), field(r, r->aien.o.plan, 2)) &&
          cp[RX_LIVING_LESSON_REGIME] == field(r, r->aien.o.plan, 2),
          "lesson payload: epoch %llu, regime %llx, condition key %llx", U(cp[1]), U(cp[RX_LIVING_LESSON_REGIME]), U(cp[RX_LIVING_LESSON_KEY]));
    CHECK(po->links[0] == cand && po->links[1] == ev_ref && (po->protect & CX_PROT_VERIFY_EVIDENCE),
          "body promotion %llu links {candidate %llu, evidence ref %llu}", U(prom), U(po->links[0]), U(po->links[1]));
    uint8_t other[32];
    RxObject goal; rx_world_read(&r->w, r->aien.o.goal, &goal);
    rx_living_object_digest(goal.type, goal.field, other);
    uint64_t out = 0, n_before = s.n;
    int rc2 = rx_living_lesson_promote(&r->w, &s, cand, 999999, dg, &out);
    CHECK(rc2 == RX_ERR_NOT_FOUND, "promotion with a missing evidence record refused (rc %d)", rc2);
    rc2 = rx_living_lesson_promote(&r->w, &s, cand, ev_ref, other, &out);
    CHECK(rc2 == RX_ERR_ARG, "promotion against a different object digest (wrong-digest evidence) refused (rc %d)", rc2);
    CHECK(s.n == n_before, "no record was written by the refusals");
    CHECK(cx_verify_chain(&s) == CX_OK, "cx_verify_chain passes after promotion (%llu records)", U(s.n));
    static Prot before, after;
    snap(&s, &before);
    uint32_t n_auth = 0, n_commit = 0;
    for (uint32_t i = 0; i < before.n; i++) { n_auth += !!(before.o[i].protect & CX_PROT_AUTHORITY); n_commit += !!(before.o[i].protect & CX_PROT_COMMIT_RECEIPT); }
    printf("protected records: %u (authority %u, commit receipt %u)\n", before.n, n_auth, n_commit);
    CHECK(before.n > 0 && n_auth > 0, "authority and receipt evidence exist to compare");

    stop(r);
    free(r);
    uint64_t n_records = s.n;
    cx_close(&s);
    CHECK(g_first_failure[0] == 0, "no World failure noted while stopping");

    /* tampering: flip one byte of a journal copy, inside the evidence-ref record */
    struct stat st; stat(jr, &st);
    int refused = 0;
    for (long off = st.st_size - 400; off < st.st_size - 300 && !refused; off += 7) {
        corrupt_copy(jr, jr2, off);
        CxStore t; memset(&t, 0, sizeof t);
        int trc = cx_open(&t, jr2, RX_CORTEX_SUBJECTS, CX_OPEN_READONLY);
        if (trc != CX_OK) refused = trc; else cx_close(&t);
    }
    CHECK(refused != 0, "journal with one flipped byte is refused at open (rc %d)", refused);

    /* 3. restart: a new body opens the same journal */
    rc = cx_open(&s2, jr, RX_CORTEX_SUBJECTS, 0);
    CHECK(rc == CX_OK && s2.n == n_records, "journal reopened: %llu records (was %llu)", U(s2.n), U(n_records));
    CHECK(cx_verify_chain(&s2) == CX_OK, "cx_verify_chain passes after restart");
    CxRecord a[64], b[64];
    CxFilter f = {0, 0, 0, 1};
    uint32_t na = cx_recall(&s2, sub, 0, UINT64_MAX, &f, a, 64), nb = cx_recall(&s2, sub, 0, UINT64_MAX, &f, b, 64);
    int same = na == nb && na > 0;
    for (uint32_t i = 0; same && i < na; i++) same = a[i].hdr.id == b[i].hdr.id && a[i].verified;
    for (uint32_t i = 1; same && i < na; i++) same = a[i].hdr.id > a[i - 1].hdr.id;
    CHECK(same, "cx_recall by the evidence subject returns %u records, ordered and repeatable", na);
    CHECK(cx_get(&s2, cand)->kind == CX_K_CANDIDATE && cx_get(&s2, prom)->kind == CX_K_PROMOTION && cx_get(&s2, prom)->links[0] == cand,
          "after restart lesson %llu is a promoted fact (promotion %llu)", U(cand), U(prom));
    CHECK(rx_living_lesson_evidence_ok(&s2, ev_ref, dg) == RX_OK && rx_living_lesson_evidence_ok(&s2, ev_ref, other) == RX_ERR_ARG,
          "after restart the evidence gate still accepts the match and refuses the wrong digest");
    /* tampered evidence record: refused, chain fails; restored value passes again */
    uint64_t keep = cx_payload(&s2, cx_get(&s2, ev_ref))[0];
    cx_tamper(&s2, ev_ref, 0, keep ^ 1u);
    CHECK(cx_verify_chain(&s2) != CX_OK && rx_living_lesson_evidence_ok(&s2, ev_ref, dg) == RX_ERR_IDENTITY,
          "tampered evidence record: cx_verify_chain fails and the gate refuses");
    cx_tamper(&s2, ev_ref, 0, keep);
    CHECK(cx_verify_chain(&s2) == CX_OK, "restored record verifies again");
    /* what is not in Cortex */
    uint64_t kinds[16] = {0}; uint32_t kid[16] = {0}; int nk = 0;
    for (uint64_t i = 1; i <= s2.n; i++) {
        uint32_t k = cx_get(&s2, i)->kind; int j = 0;
        while (j < nk && kid[j] != k) j++;
        if (j == nk && nk < 16) kid[nk++] = k;
        if (j < 16) kinds[j]++;
    }
    printf("kinds in Cortex:");
    for (int j = 0; j < nk; j++) printf(" 0x%x x%llu", kid[j], U(kinds[j]));
    printf("\nnot in Cortex: live World objects and their field versions, reaction state, capabilities and the crumb log itself (only each crumb's result is kept)\n");
    int ephemeral = 1;
    for (int j = 0; j < nk; j++) ephemeral &= (kid[j] >= CX_K_BASE);
    CHECK(ephemeral, "every record is a canonical kind; there is no kind for an object table or reaction state");

    /* 4. protected evidence byte-identical */
    snap(&s2, &after);
    int ident = after.n == before.n;
    for (uint32_t i = 0; ident && i < before.n; i++)
        ident = before.id[i] == after.id[i] && memcmp(&before.o[i], &after.o[i], sizeof(CxObject)) == 0 &&
                memcmp(before.pl[i], after.pl[i], before.o[i].n * sizeof(uint64_t)) == 0;
    CHECK(ident, "%u protected records (authority, receipts, verification evidence, promotion) byte-identical after restart", after.n);

    /* ---- C2: related-problem retrieval after a restart ---- */
    uint64_t want = rx_aien_key(CLASS_X925, rx_omega_regime(M, N));
    uint64_t k_prom[RX_AIEN_MEMORY_SLOTS];
    uint32_t nk_prom = rx_living_explored_keys(&s2, sub, k_prom, RX_AIEN_MEMORY_SLOTS);
    CHECK(nk_prom == 1 && k_prom[0] == want, "Cortex holds %u promoted condition (key %llx; X925 at this regime = %llx)", nk_prom, U(k_prom[0]), U(want));
    /* the same journal with the promotion left out: the lesson is only a candidate */
    static CxStore sn;
    int cp_ok = cx_init(&sn, RX_CORTEX_SUBJECTS) == CX_OK;
    for (uint64_t i = 1; cp_ok && i < prom; i++) {
        const CxObject *q = cx_get(&s2, i);
        CxHeader h = {q->cls, q->kind, q->subject, q->t, q->generation, q->branch, q->protect, q->tag, {q->links[0], q->links[1], q->links[2], q->links[3]}};
        cp_ok = cx_append(&sn, &h, cx_payload(&s2, q), q->n, NULL) == CX_OK;
    }
    uint64_t k_open[RX_AIEN_MEMORY_SLOTS];
    CHECK(cp_ok && sn.n == prom - 1 && cx_get(&sn, cand)->kind == CX_K_CANDIDATE && rx_living_explored_keys(&sn, sub, k_open, RX_AIEN_MEMORY_SLOTS) == 0,
          "journal without the promotion: lesson %llu is a candidate and yields no memory", U(cand));
    Outcome oe, on, ou;
    int e1 = episode(&s2, 1, &oe), e2 = episode(&s2, 0, &on), e3 = episode(&sn, 1, &ou);
    printf("C2 explored-not-replanned: seeded %u key, assessment %s, plan seq %llu, memory count %llu\n", oe.seeded,
           oe.status == RX_AIEN_GOAL_UNMET_EXPLORED ? "UNMET_EXPLORED" : "other", U(oe.plan), U(oe.mem_count));
    printf("C2 new-condition-planned: seeded %u key, core class A725 (not seeded), assessment status %llu, plan seq %llu, memory count %llu\n", on.seeded, U(on.status), U(on.plan), U(on.mem_count));
    printf("C2 unpromoted-replanned: seeded %u keys, X925 condition, plan seq %llu for class %llx, memory count %llu\n", ou.seeded, U(ou.plan), U(ou.mem_key), U(ou.mem_count));
    CHECK(e1 == 0 && oe.seeded == 1 && oe.status == RX_AIEN_GOAL_UNMET_EXPLORED && oe.plan == 0 && oe.mem_count == 1,
          "second life, same condition: explored, NOT re-planned (rc %d)", e1);
    CHECK(e2 == 0 && on.seeded == 1 && on.plan != 0 && on.mem_count == 2 && on.mem_key == CLASS_A725,
          "second life, new core class: still gets a plan (rc %d)", e2);
    CHECK(e3 == 0 && ou.seeded == 0 && ou.plan != 0 && ou.mem_key == CLASS_X925,
          "second life, lesson never promoted: the condition IS re-explored (rc %d)", e3);
    cx_free(&sn);
    cx_close(&s2);
    free(g_seen); free(g_stack);
    printf("R13_CORTEX_EPISODE %s: pass %d fail %d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
