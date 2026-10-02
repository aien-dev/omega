/*
 * rx_compose.c -- COMPOSITION-2: one World, one causal path (rx_compose.h).
 *
 * Persistent layout in the composition directory:
 *   machine.id     canonical AienMachineId the composition belongs to
 *   cortex.cx      Cortex journal (World records + composition records)
 *   jspace/        durable J-Space (sealed state branches only)
 */
#include "rx_compose.h"
#include "rx_cortex_record.h"
#include "sha256.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RXC_ISSUER 3u                 /* AIENOS issuer: system owner policy office */
#define RXC_STATE_SUBJECT RXC_CX_SUBJECT(RXC_SLOT_STATE)

/* ---- deterministic realizer of the state unit ---------------------------- */

static uint64_t rxc_mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static void rxc_derive(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    uint64_t h = rxc_mix(token ^ 0xC0A1E5CEull);
    for (size_t i = 0; i < n; i += 8) {
        uint64_t p = 0;
        if (prev) memcpy(&p, prev + i, n - i < 8 ? n - i : 8);
        h = rxc_mix(h + p + i);
        memcpy(out + i, &h, n - i < 8 ? n - i : 8);
    }
}

const JsRealizer RXC_REALIZER = { JS_REAL_WORLD_PROJECTION, "rxc_state", RXC_UNIT, rxc_derive };

/* ---- small helpers -------------------------------------------------------- */

static void path_in(char *out, size_t n, const char *dir, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static uint64_t ref_pack(JsBranchRef r) { return js_branch_ref_pack(r); }
static JsBranchRef ref_unpack(uint64_t v) { return js_branch_ref_unpack(v); }
static int ref_eq(JsBranchRef a, JsBranchRef b) { return a.id == b.id && a.gen == b.gen; }

/* One fault point (test builds only, -DAIEN_TEST_BUILD=1). Fires once;
 * candidate-side points fire only for fault_k. Returns 1 when the caller must
 * fail in process. Without the test build it never fires. */
#ifdef RXC_TEST_HOOKS
/* Link-map marker: the production hygiene check (tools/r16_prod_hygiene.sh)
 * refuses any binary that carries an aien_test_build_* symbol. */
__attribute__((used)) const char aien_test_build_rx_compose_hooks[] =
    "AIEN_TEST_BUILD piece: rx_compose test hooks (fault points, rogue candidate, hold point)";
static int fault(RxCompose *c, int point, uint32_t k) {
    if (c->test.fault_point != point) return 0;
    if ((point == RXC_FP_BEFORE_FORK || point == RXC_FP_CANDIDATE) && k != c->test.fault_k)
        return 0;
    if (__atomic_exchange_n(&c->test.fault_hit, 1, __ATOMIC_ACQ_REL)) return 0;
    if (c->test.fault_crash) _exit(RXC_CRASH_EXIT);
    return 1;
}
#define RXC_ROGUE(c) ((c)->test.rogue_candidate)
#else
static int fault(RxCompose *c, int point, uint32_t k) {
    (void)c; (void)point; (void)k;
    return 0;
}
#define RXC_ROGUE(c) 0
#endif

/* Hold point (test builds only): see RxcTestHooks.hold_k1. */
#ifdef RXC_TEST_HOOKS
static void hold_point(RxCompose *c, uint32_t k) {
    if (c->test.hold_k1 != k + 1u) return;
    if (__atomic_exchange_n(&c->test.held, 1, __ATOMIC_ACQ_REL)) return;
    struct timespec ts = { 0, 100000 };
    while (!__atomic_load_n(&c->test.release, __ATOMIC_ACQUIRE)) nanosleep(&ts, NULL);
    __atomic_store_n(&c->test.hold_done, 1, __ATOMIC_RELEASE);
}
#else
static void hold_point(RxCompose *c, uint32_t k) { (void)c; (void)k; }
#endif

static const RxSnapshotDep *dep_of(const RxCtx *x, RxObjRef o) {
    for (uint32_t i = 0; i < x->n_in; i++)
        if (x->in[i].obj.id == o.id && x->in[i].obj.generation == o.generation) return &x->in[i];
    return NULL;
}

static void out_put(RxCtx *x, RxObjRef o, uint32_t f, uint64_t v) {
    if (x->n_out < RX_MAX_MUTATIONS) x->out[x->n_out++] = (RxMutation){ o, f, v };
}

static const AgSkill *skill_of(const RxCompose *c, uint32_t id) {
    const AgSkillTable *t = c->router ? c->router->skills : NULL;
    for (uint32_t i = 0; t && i < t->n; i++)
        if (t->skill[i].id == id) return &t->skill[i];
    return NULL;
}

static int digest_zero(const uint8_t d[32]) {
    uint8_t a = 0;
    for (int i = 0; i < 32; i++) a |= d[i];
    return a == 0;
}

static void words_from_digest(const uint8_t d[32], uint64_t *w) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) v |= (uint64_t)d[8 * i + b] << (8 * b);
        w[i] = v;
    }
}

/* ---- reactions ------------------------------------------------------------ */

/* compose.candidate.k: fork a staged branch from the committed state, run the
 * routed Skill, derive the branch with the result, propose it in cand[k]. */
static int candidate_fn(RxCtx *x) {
    struct RxcCandUser *u = x->user;
    RxCompose *c = u->c;
    uint32_t k = u->k;
    const RxSnapshotDep *g = dep_of(x, c->goal), *st = dep_of(x, c->state);
    if (!g || !st) return 0;
    uint64_t seq = g->field[RXC_G_SEQ];
    if (seq == 0) return 0;
    uint64_t skill_word = g->field[k == 0 ? RXC_G_SKILL0 : RXC_G_SKILL1];
    uint64_t ref = 0, result = 0, home = RXC_HOME_LOCAL;
    const SrRoute *rt = k < c->n_routes ? &c->run_route[k] : NULL;
    const AgSkill *s = NULL;
    int runnable = 0;
    if (rt && skill_word != 0 && rt->verdict == SR_OK) {
        s = skill_of(c, (uint32_t)skill_word);
        const uint8_t *want = rt->skill_digest;
        runnable = s && s->fn && (digest_zero(want) || memcmp(want, s->identity, 32) == 0);
    } else if (rt && skill_word != 0 && rt->verdict == SR_E_REMOTE) {
        /* The provider is on another machine: the Fabric runs it there (the
         * hook revalidates the route and the machine's lease first). The
         * claim names that machine's advertised procedure whether it ran,
         * was refused or no hook is set (never a local identity). */
        home = RXC_HOME_FABRIC;
        memcpy(c->remote.digest[k], rt->skill_digest, 32);
        c->remote.seq[k] = seq;
        runnable = c->remote.run != NULL;
    }
    if (runnable) {
        if (fault(c, RXC_FP_BEFORE_FORK, k)) return -1;
        JsBranchRef base = ref_unpack(st->field[RXC_S_REF]), next;
        uint32_t subj = c->subj_cand[k];
        if (js_branch_fork_staged(&c->js, base, subj, &next) != JS_OK) return -1;
        uint64_t in = g->field[RXC_G_INPUT];
        int failed = 0;
        uint64_t r = 0;
        if (home == RXC_HOME_LOCAL) {
            r = s->fn(&in, 1, 0, &failed);
        } else if (c->remote.run(c->remote.ctx, c->router, rt, in, c->run_now, &r) != 0) {
            failed = 1;
            __atomic_add_fetch(&c->remote.refused, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_add_fetch(&c->remote.ran, 1, __ATOMIC_RELAXED);
        }
        if (failed || js_branch_derive(&c->js, next.id, r) != JS_OK) {
            js_branch_release_ref(&c->js, next, subj);
        } else {
            if (fault(c, RXC_FP_CANDIDATE, k)) return -1;
            ref = ref_pack(next);
            result = r;
            if (k == 0 && RXC_ROGUE(c))
                out_put(x, c->state, RXC_S_REF, ref);   /* refused: not in its write set */
        }
    }
    hold_point(c, k);
    RxObjRef me = c->cand[k];
    out_put(x, me, RXC_C_REF, ref);
    out_put(x, me, RXC_C_RESULT, result);
    out_put(x, me, RXC_C_SKILL, skill_word);
    out_put(x, me, RXC_C_HOME, home);
    out_put(x, me, RXC_C_GOAL, seq);
    out_put(x, me, RXC_C_DONE, seq);
    return 0;
}

/* compose.verify (AEGIS): the only subject that can write the verdict. */
static int verify_fn(RxCtx *x) {
    RxCompose *c = x->user;
    const RxSnapshotDep *g = dep_of(x, c->goal);
    const RxSnapshotDep *cd[RXC_K] = { dep_of(x, c->cand[0]), dep_of(x, c->cand[1]) };
    if (!g || !cd[0] || !cd[1]) return 0;
    uint64_t seq = g->field[RXC_G_SEQ];
    if (seq == 0) return 0;
    for (uint32_t k = 0; k < RXC_K; k++)
        if (cd[k]->field[RXC_C_DONE] != seq) return 0;   /* wait for every candidate */
    uint64_t input = g->field[RXC_G_INPUT];
    uint32_t winner = RXC_NONE, passmask = 0;
    for (uint32_t k = 0; k < RXC_K; k++) {
        int pass = cd[k]->field[RXC_C_REF] != 0 && cd[k]->field[RXC_C_GOAL] == seq &&
                   c->contract && c->contract(input, cd[k]->field[RXC_C_RESULT]);
        if (pass) {
            passmask |= 1u << k;
            if (winner == RXC_NONE) winner = k;
        }
    }
    uint64_t wref = winner != RXC_NONE ? cd[winner]->field[RXC_C_REF] : 0;
    uint64_t lref = cd[winner == 0 ? 1 : 0]->field[RXC_C_REF];
    uint64_t res = winner != RXC_NONE ? cd[winner]->field[RXC_C_RESULT] : 0;
    out_put(x, c->verdict, RXC_V_WINNER, winner);
    out_put(x, c->verdict, RXC_V_WREF, wref);
    out_put(x, c->verdict, RXC_V_LREF, lref);
    out_put(x, c->verdict, RXC_V_PASSMASK, passmask);
    out_put(x, c->verdict, RXC_V_GOAL, seq);
    out_put(x, c->verdict, RXC_V_RESULT, res);
    return 0;
}

/* compose.commit: names the winner in the state object. */
static int commit_fn(RxCtx *x) {
    RxCompose *c = x->user;
    const RxSnapshotDep *v = dep_of(x, c->verdict);
    if (!v || v->field[RXC_V_GOAL] == 0 || v->field[RXC_V_WINNER] == RXC_NONE) return 0;
    if (fault(c, RXC_FP_BEFORE_COMMIT, 0)) return -1;
    out_put(x, c->state, RXC_S_REF, v->field[RXC_V_WREF]);
    out_put(x, c->state, RXC_S_RESULT, v->field[RXC_V_RESULT]);
    out_put(x, c->state, RXC_S_GOAL, v->field[RXC_V_GOAL]);
    return 0;
}

/* ---- World commit binder: state.REF names a J-Space branch ---------------- */

static int bind_check(void *ctx, RxObjRef obj, uint32_t field, uint64_t old_value,
                      uint64_t new_value, uint32_t subject) {
    RxCompose *c = ctx;
    if (obj.id != c->state.id || field != RXC_S_REF) return RX_ERR_BINDING;
    if (subject != c->subj_commit) return RX_ERR_BINDING;
    JsBranchRef old = ref_unpack(old_value), nw = ref_unpack(new_value);
    JsBranchInfo bi;
    if (js_branch_info(&c->js, nw, &bi) != JS_OK) return RX_ERR_BINDING;
    if (!bi.staged || bi.locality != JS_HOME_LOCAL) return RX_ERR_BINDING;
    if (bi.owner != c->subj_cand[0] && bi.owner != c->subj_cand[1]) return RX_ERR_BINDING;
    if (bi.parent != old.id || bi.parent_gen != old.gen) return RX_ERR_BINDING;
    if (fault(c, RXC_FP_AFTER_VALIDATION, 0)) return RX_ERR_BINDING;
    return RX_OK;
}

static int bind_fn(void *ctx, RxObjRef obj, uint32_t field, uint64_t old_value,
                   uint64_t new_value, uint32_t subject) {
    (void)obj; (void)field; (void)old_value; (void)subject;
    RxCompose *c = ctx;
    JsBranchRef nw = ref_unpack(new_value);
#ifdef RXC_TEST_HOOKS
    if (c->test.fault_point == RXC_FP_SEAL &&
        !__atomic_exchange_n(&c->test.fault_hit, 1, __ATOMIC_ACQ_REL)) {
        if (!c->test.fault_crash) return RX_ERR_BINDING;   /* seal refused */
        js_branch_seal(&c->js, nw);
        js_branch_set_owner(&c->js, nw, 0);
        _exit(RXC_CRASH_EXIT);                             /* crash right after the seal */
    }
#endif
    if (js_branch_seal(&c->js, nw) != JS_OK) return RX_ERR_BINDING;
    js_branch_set_owner(&c->js, nw, 0);
    return RX_OK;
}

static void bind_abort(void *ctx, RxObjRef obj, uint32_t field, uint64_t value,
                       uint32_t subject, int was_bound) {
    (void)obj; (void)field; (void)subject;
    RxCompose *c = ctx;
    if (was_bound) js_branch_release_ref(&c->js, ref_unpack(value), 0);
    /* not bound: still staged, reclaimed at settle */
}

/* ---- Cortex helpers -------------------------------------------------------- */

static int cx_put(RxCompose *c, CxHeader *h, const uint64_t *p, uint32_t n, uint64_t *id) {
    if (c->attached) {
        h->t = rx_cortex_next_t_in(c->world, &c->cx);
        return rx_cortex_append_in(c->world, &c->cx, h, p, n, id) == RX_OK ? 0 : -1;
    }
    h->t = c->cx.n + 1;
    return cx_append(&c->cx, h, p, n, id) == CX_OK ? 0 : -1;
}

static int cx_prom(RxCompose *c, uint64_t cand, uint64_t ev, uint64_t *id) {
    if (c->attached)
        return rx_cortex_promote_in(c->world, &c->cx, cand, ev, id) == RX_OK ? 0 : -1;
    return cx_promote(&c->cx, 0, cand, ev, c->cx.n + 1, id) == CX_OK ? 0 : -1;
}

static const uint64_t *payload_of(const CxStore *s, uint64_t id, uint32_t min_words) {
    const CxObject *o = cx_get(s, id);
    if (!o || o->n < min_words) return NULL;
    return cx_payload(s, o);
}

/* Newest record of `kind` about `slot` with id < before (0 = none). */
static uint64_t latest_before(CxStore *s, uint32_t slot, uint32_t cls, uint32_t kind,
                              uint64_t before) {
    CxFilter f = { cls, kind, 0, 1 };
    return cx_latest(s, RXC_CX_SUBJECT(slot), before > 1 ? before - 1 : 0, &f);
}

/* C4: 1 if a Cortex promotion names a winner claim whose branch is `packed_ref`.
 * A promotion is written only after its state is durable (settle order), so a
 * promoted state that J-Space no longer holds is a regressed checkpoint, not a
 * crash window: recovery must refuse it rather than roll it back. */
static int state_ref_promoted(const CxStore *s, uint64_t packed_ref) {
    for (uint64_t id = 1; id <= s->n; id++) {
        const CxObject *o = cx_get(s, id);
        if (!o || o->kind != CX_K_PROMOTION) continue;
        const CxObject *claim = cx_get(s, o->links[0]);
        if (!claim || claim->kind != CX_K_CANDIDATE) continue;
        const uint64_t *pl = payload_of(s, claim->id, RXC_CP_WORDS);
        if (pl && pl[RXC_CP_REF] == packed_ref) return 1;
    }
    return 0;
}

static int has_admission(CxStore *s, uint64_t tag, uint64_t link0) {
    const CxIdList *l = &s->by_subject[RXC_STATE_SUBJECT];
    for (uint32_t i = 0; i < l->n; i++) {
        const CxObject *o = cx_get(s, l->ids[i]);
        if (o && o->kind == CX_K_ADMISSION && o->tag == tag && o->links[0] == link0) return 1;
    }
    return 0;
}

/* The composition record of one goal. S = state World record of the commit (0
 * when no winner), V = the verdict World record. Records are written in a
 * fixed order (claim 0, claim 1, evidence, [promotion], loser admissions);
 * the ones already present after S/V are reused, so recovery can complete an
 * interrupted record. `cdig` (may be NULL) holds candidate branch digests
 * taken before the reclaim. Returns records written, -1 when an append failed,
 * or -2 when its inputs are absent (nothing can be, or was, written). */
static int compose_records(RxCompose *c, uint64_t S, uint64_t V, const uint8_t (*cdig)[32],
                           int may_fault, RxcResult *out) {
    CxStore *s = &c->cx;
    /* Appends may move the Cortex arena: copy every input value out first
     * and hold no payload or object pointer across an append. */
    uint64_t vf[8], cf[RXC_K][8], g_input;
    const uint64_t *vp = payload_of(s, V, CX_WREC_WORDS);
    if (!vp) return -2;
    memcpy(vf, vp + CX_WREC_FIELD0, sizeof vf);
    uint64_t G = latest_before(s, RXC_SLOT_GOAL, CX_OBSERVATION, CX_K_WORK_ACCEPTED, V);
    const uint64_t *gp = payload_of(s, G, CX_WREC_WORDS);
    if (!gp) return -2;
    g_input = gp[CX_WREC_FIELD0 + RXC_G_INPUT];
    uint64_t Ck[RXC_K];
    for (uint32_t k = 0; k < RXC_K; k++) {
        Ck[k] = latest_before(s, RXC_SLOT_CAND0 + k, CX_EXECUTION, CX_K_EXEC_COMMIT, V);
        const uint64_t *cp = payload_of(s, Ck[k], CX_WREC_WORDS);
        if (!cp) return -2;
        memcpy(cf[k], cp + CX_WREC_FIELD0, sizeof cf[k]);
    }
    uint32_t winner = (uint32_t)vf[RXC_V_WINNER];
    if ((winner == RXC_NONE) != (S == 0)) return -2;

    /* Existing composition records of this goal (claims and evidence carry
     * tag G; promotion and loser admissions link one of its claims), in
     * append order, after max(S, V) on the state subject. */
    uint64_t after = S > V ? S : V, have[2 + 2 + RXC_K], mine[RXC_K] = { 0, 0 };
    uint32_t n_have = 0, n_mine = 0;
    const CxIdList *l = &s->by_subject[RXC_STATE_SUBJECT];
    for (uint32_t i = 0; i < l->n && n_have < 2 + 2 + RXC_K; i++) {
        const CxObject *o = cx_get(s, l->ids[i]);
        if (!o || o->id <= after) continue;
        int ours = 0;
        if (o->kind == CX_K_CANDIDATE && o->tag == G) {
            ours = 1;
            if (n_mine < RXC_K) mine[n_mine++] = o->id;
        } else if (o->kind == CX_K_EVIDENCE_REF && o->tag == G) {
            ours = 1;
        } else if (o->kind == CX_K_PROMOTION ||
                   (o->kind == CX_K_ADMISSION && o->tag == RXC_ADMIT_LOSER)) {
            for (uint32_t m = 0; m < n_mine; m++) ours |= o->links[0] == mine[m];
        }
        if (ours) have[n_have++] = o->id;
    }
    uint32_t step = 0;
    int written = 0;
#define RXC_STEP(expr_id, ...)                                                    \
    do {                                                                          \
        if (step < n_have) { (expr_id) = have[step]; }                            \
        else {                                                                    \
            if (may_fault && written == 1 && fault(c, RXC_FP_CORTEX, 0)) return -1; \
            __VA_ARGS__                                                           \
            written++;                                                            \
        }                                                                         \
        step++;                                                                   \
    } while (0)

    uint64_t claim[RXC_K] = { 0, 0 }, ev = 0, prom = 0, adm[RXC_K] = { 0, 0 };
    uint8_t wdig[32];
    memset(wdig, 0, sizeof wdig);
    if (winner != RXC_NONE) {
        JsBranchRef w = ref_unpack(vf[RXC_V_WREF]);
        if (js_branch_check(&c->js, w) == JS_OK) js_branch_content_digest(&c->js, w.id, wdig);
    }
    for (uint32_t k = 0; k < RXC_K; k++) {
        RXC_STEP(claim[k], {
            uint64_t p[RXC_CP_WORDS];
            memset(p, 0, sizeof p);
            p[RXC_CP_K] = k;
            p[RXC_CP_REF] = cf[k][RXC_C_REF];
            p[RXC_CP_RESULT] = cf[k][RXC_C_RESULT];
            p[RXC_CP_SKILL] = cf[k][RXC_C_SKILL];
            p[RXC_CP_INPUT] = g_input;
            p[RXC_CP_GOALSEQ] = cf[k][RXC_C_GOAL];
            p[RXC_CP_PASS] = (vf[RXC_V_PASSMASK] >> k) & 1u;
            uint8_t d[32];
            memset(d, 0, sizeof d);
            JsBranchRef r = ref_unpack(cf[k][RXC_C_REF]);
            if (cdig) memcpy(d, cdig[k], 32);
            else if (r.id || r.gen) {
                if (js_branch_check(&c->js, r) == JS_OK) js_branch_content_digest(&c->js, r.id, d);
            }
            words_from_digest(d, p + RXC_CP_DIGEST0);
            /* The procedure that produced it: locally its executable
             * identity, which the router admitted only when it equals the
             * graph's digest; on a Fabric machine the digest that machine
             * advertised (and ran, if the claim has a branch). It is held in
             * memory per goal; a pending record is completed only within the
             * same open, before the next goal's candidates run. */
            p[RXC_CP_HOME] = cf[k][RXC_C_HOME];
            if (cf[k][RXC_C_HOME] == RXC_HOME_FABRIC) {
                if (c->remote.seq[k] && c->remote.seq[k] == cf[k][RXC_C_GOAL])
                    words_from_digest(c->remote.digest[k], p + RXC_CP_SKILLDIG0);
            } else {
                const AgSkill *sk = skill_of(c, (uint32_t)cf[k][RXC_C_SKILL]);
                if (sk && cf[k][RXC_C_SKILL]) words_from_digest(sk->identity, p + RXC_CP_SKILLDIG0);
            }
            CxHeader h;
            memset(&h, 0, sizeof h);
            h.cls = CX_CLAIM;
            h.kind = CX_K_CANDIDATE;
            h.subject = RXC_STATE_SUBJECT;
            h.tag = G;
            h.links[0] = Ck[k];
            h.links[1] = G;
            if (cx_put(c, &h, p, RXC_CP_WORDS, &claim[k])) return -1;
        });
    }
    RXC_STEP(ev, {
        uint64_t p[RXC_EP_WORDS];
        memset(p, 0, sizeof p);
        p[RXC_EP_WINNER] = winner;
        p[RXC_EP_WREF] = vf[RXC_V_WREF];
        p[RXC_EP_LREF] = vf[RXC_V_LREF];
        p[RXC_EP_PASSMASK] = vf[RXC_V_PASSMASK];
        p[RXC_EP_RESULT] = vf[RXC_V_RESULT];
        p[RXC_EP_GOALSEQ] = vf[RXC_V_GOAL];
        p[RXC_EP_VERIFIER] = c->subj_aegis;
        words_from_digest(wdig, p + RXC_EP_WDIGEST0);
        CxHeader h;
        memset(&h, 0, sizeof h);
        h.cls = CX_EVIDENCE;
        h.kind = CX_K_EVIDENCE_REF;
        h.protect = CX_PROT_VERIFY_EVIDENCE;
        h.subject = RXC_STATE_SUBJECT;
        h.tag = G;
        h.links[0] = V;
        h.links[1] = claim[0];
        h.links[2] = claim[1];
        h.links[3] = S;
        if (cx_put(c, &h, p, RXC_EP_WORDS, &ev)) return -1;
    });
    if (winner != RXC_NONE)
        RXC_STEP(prom, { if (cx_prom(c, claim[winner], ev, &prom)) return -1; });
    for (uint32_t k = 0; k < RXC_K; k++) {
        if (k == winner) continue;
        RXC_STEP(adm[k], {
            uint64_t p[RXC_AP_WORDS] = { cf[k][RXC_C_REF], vf[RXC_V_GOAL], k };
            CxHeader h;
            memset(&h, 0, sizeof h);
            h.cls = CX_EVIDENCE;
            h.kind = CX_K_ADMISSION;
            h.subject = RXC_STATE_SUBJECT;
            h.tag = RXC_ADMIT_LOSER;
            h.links[0] = claim[k];
            h.links[1] = ev;
            if (cx_put(c, &h, p, RXC_AP_WORDS, &adm[k])) return -1;
        });
    }
#undef RXC_STEP
    if (out) {
        for (uint32_t k = 0; k < RXC_K; k++) {
            out->cx_candidate[k] = claim[k];
            out->cx_admission[k] = adm[k];
        }
        out->cx_evidence = ev;
        out->cx_promotion = prom;
        memcpy(out->winner_digest, wdig, 32);
    }
    return written;
}

/* ---- recovery (OLD-or-NEW), before the World exists ----------------------- */

static int recover(RxCompose *c) {
    CxStore *s = &c->cx;
    const CxIdList *l = &s->by_subject[RXC_STATE_SUBJECT];
    uint64_t chosen = 0;
    JsBranchRef ref = { 0, 0 };
    for (uint32_t i = l->n; i-- > 0;) {
        const CxObject *o = cx_get(s, l->ids[i]);
        if (!o || (o->kind != CX_K_ENTITY_CREATED && o->kind != CX_K_EXEC_COMMIT)) continue;
        if (has_admission(s, RXC_ADMIT_ROLLBACK, o->id)) continue;
        const uint64_t *p = payload_of(s, o->id, CX_WREC_WORDS);
        if (!p) return RX_ERR_REPLAY;
        JsBranchRef r = ref_unpack(p[CX_WREC_FIELD0 + RXC_S_REF]);
        JsBranchInfo bi;
        if (js_branch_info(&c->js, r, &bi) == JS_OK && !bi.staged) {
            chosen = o->id;
            ref = r;
            break;
        }
    }
    /* State was recorded but nothing it names is durable: refuse, never
     * invent a genesis over recorded history (the root is made durable
     * before the World first records it). */
    if (!chosen)
        for (uint32_t i = 0; i < l->n; i++) {
            const CxObject *o = cx_get(s, l->ids[i]);
            if (o && (o->kind == CX_K_ENTITY_CREATED || o->kind == CX_K_EXEC_COMMIT))
                return RX_ERR_REPLAY;
        }
    /* Every newer state record did not become durable: answer it. */
    for (uint32_t i = 0; i < l->n; i++) {
        const CxObject *o = cx_get(s, l->ids[i]);
        if (!o || o->id <= chosen) continue;
        if (o->kind != CX_K_ENTITY_CREATED && o->kind != CX_K_EXEC_COMMIT) continue;
        if (has_admission(s, RXC_ADMIT_ROLLBACK, o->id)) continue;
        {
            const uint64_t *pp = payload_of(s, o->id, CX_WREC_WORDS);
            if (pp && state_ref_promoted(s, pp[CX_WREC_FIELD0 + RXC_S_REF])) return RX_ERR_REPLAY;
        }
        const uint64_t *p = payload_of(s, o->id, CX_WREC_WORDS);
        uint64_t ap[RXC_AP_WORDS] = { p ? p[CX_WREC_FIELD0 + RXC_S_REF] : 0,
                                      p ? p[CX_WREC_FIELD0 + RXC_S_GOAL] : 0, RXC_NONE };
        CxHeader h;
        memset(&h, 0, sizeof h);
        h.cls = CX_EVIDENCE;
        h.kind = CX_K_ADMISSION;
        h.subject = RXC_STATE_SUBJECT;
        h.tag = RXC_ADMIT_ROLLBACK;
        h.links[0] = o->id;
        uint64_t id;
        if (cx_put(c, &h, ap, RXC_AP_WORDS, &id)) return RX_ERR_REPLAY;
        c->rolled_back++;
    }
    if (!chosen) {
        /* Genesis: drop anything durable, root seeded by the machine identity. */
        /* C4: a lost or emptied Cortex journal must not silently reset a World
         * whose J-Space holds committed history. The genesis root is one unit;
         * every committed goal derives more. Durable history with no state
         * record is refused, never overwritten with a fresh genesis. */
        for (uint32_t b = 0; b < c->js.n_branches; b++)
            if (c->js.branches[b] && c->js.branches[b]->n_units > 1) return RX_ERR_REPLAY;
        for (uint32_t b = 0; b < c->js.n_branches; b++)
            if (c->js.branches[b]) js_branch_release(&c->js, b);
        uint32_t root;
        if (js_branch_root(&c->js, &RXC_REALIZER, aien_mid_hash(&c->self), &root) != JS_OK ||
            js_branch_ref(&c->js, root, &ref) != JS_OK)
            return RX_ERR_REPLAY;
    } else {
        for (uint32_t b = 0; b < c->js.n_branches; b++)
            if (c->js.branches[b] && b != ref.id) js_branch_release(&c->js, b);
    }
    if (js_space_commit(&c->js) != JS_OK) return RX_ERR_REPLAY;
    c->recovered = ref;
    c->recovered_record = chosen;

    /* Complete every interrupted composition record, not only the newest:
     * each recorded verdict whose goal had no winner (S = 0), or whose winner
     * was committed by a state record that was not rolled back. A crash or
     * failure anywhere inside a record leaves it completable here. */
    const uint64_t vsubj = RXC_CX_SUBJECT(RXC_SLOT_VERDICT);
    uint32_t done = 0;
    for (uint32_t i = 0; i < s->by_subject[vsubj].n; i++) {
        uint64_t V = s->by_subject[vsubj].ids[i];
        const CxObject *vo = cx_get(s, V);
        if (!vo || vo->kind != CX_K_EXEC_COMMIT) continue;
        const uint64_t *vp = payload_of(s, V, CX_WREC_WORDS);
        if (!vp || vp[CX_WREC_N_OUTPUTS] == 0) continue;   /* the verifier's own write */
        const uint64_t goal = vp[CX_WREC_FIELD0 + RXC_V_GOAL];
        const uint64_t winner = vp[CX_WREC_FIELD0 + RXC_V_WINNER];
        if (goal == 0) continue;
        uint64_t S = 0, sref = 0;
        if (winner != RXC_NONE) {
            /* Its commit: the first state record of the same goal after V,
             * before the next verdict write and the next World. */
            uint64_t bound = UINT64_MAX;
            for (uint32_t j = i + 1; j < s->by_subject[vsubj].n; j++) {
                uint64_t id = s->by_subject[vsubj].ids[j];
                const CxObject *n2 = cx_get(s, id);
                const uint64_t *np = payload_of(s, id, CX_WREC_WORDS);
                if (n2 && (n2->kind == CX_K_ENTITY_CREATED ||
                           (n2->kind == CX_K_EXEC_COMMIT && np && np[CX_WREC_N_OUTPUTS]))) {
                    bound = id;
                    break;
                }
            }
            for (uint32_t j = 0; j < l->n; j++) {
                const CxObject *o = cx_get(s, l->ids[j]);
                if (!o || o->id <= V) continue;
                if (o->id >= bound || o->kind == CX_K_ENTITY_CREATED) break;
                if (o->kind != CX_K_EXEC_COMMIT) continue;
                const uint64_t *p = payload_of(s, o->id, CX_WREC_WORDS);
                if (p && p[CX_WREC_FIELD0 + RXC_S_GOAL] == goal) {
                    S = o->id;
                    sref = p[CX_WREC_FIELD0 + RXC_S_REF];
                }
                break;
            }
            if (!S || has_admission(s, RXC_ADMIT_ROLLBACK, S)) continue;   /* never NEW */
        }
        RxcResult tmp;
        memset(&tmp, 0, sizeof tmp);
        int n = compose_records(c, S, V, NULL, 0, &tmp);
        if (n == -2 && S != chosen) continue;   /* inputs absent: the run wrote nothing either */
        if (n < 0) return RX_ERR_REPLAY;
        if (n > 0) {
            uint64_t ap[RXC_AP_WORDS] = { sref, goal, winner };
            CxHeader h;
            memset(&h, 0, sizeof h);
            h.cls = CX_EVIDENCE;
            h.kind = CX_K_ADMISSION;
            h.subject = RXC_STATE_SUBJECT;
            h.tag = RXC_ADMIT_RECOVERED;
            h.links[0] = S ? S : V;
            h.links[1] = tmp.cx_evidence;
            uint64_t id;
            if (cx_put(c, &h, ap, RXC_AP_WORDS, &id)) return RX_ERR_REPLAY;
            done += (uint32_t)n;
        }
    }
    c->recovered_completed = done;
    return RX_OK;
}

/* ---- authority ------------------------------------------------------------ */

static int mint(RxCompose *c, uint32_t subject, uint64_t resource, uint32_t rights, RxCapRef *out) {
    AienosCapRef office, ref = { UINT32_MAX, 0 };
    if (aienos_cap_office(c->admin, &office) != 0) return RX_ERR_AUTHORITY;
    AienosCapMint m = { RXC_ISSUER, subject, resource, rights, 0, { UINT32_MAX, 0 }, office };
    if (aienos_cap_mint(c->admin, &m, &ref) != 0) return RX_ERR_AUTHORITY;
    *out = (RxCapRef){ ref.cap_id, ref.generation };
    return RX_OK;
}

static const uint64_t RXC_RES[5] = { RXC_RES_GOAL, RXC_RES_CAND0, RXC_RES_CAND1, RXC_RES_VERDICT,
                                     RXC_RES_STATE };
#define RXC_N_CAPS (1u + RXC_K * 3u + 4u + 2u)

/* Instance `inst` of a World's composition slots: its own subjects and its
 * own resources (rx_compose.h, RXC_SUBJ_OF / RXC_RES_OF). */
static void set_instance(RxCompose *c, uint32_t inst) {
    c->inst = inst;
    c->subj_cand[0] = RXC_SUBJ_OF(inst, RXC_SUBJ_CAND0);
    c->subj_cand[1] = RXC_SUBJ_OF(inst, RXC_SUBJ_CAND1);
    c->subj_aegis = RXC_SUBJ_OF(inst, RXC_SUBJ_AEGIS);
    c->subj_commit = RXC_SUBJ_OF(inst, RXC_SUBJ_COMMIT);
    for (uint32_t i = 0; i < 5; i++) c->res[i] = RXC_RES_OF(inst, RXC_RES[i]);
}

/* The capabilities minted at open/attach, in mint order. */
static RxCapRef *cap_at(RxCompose *c, uint32_t i) {
    if (i == 0) return &c->cap_ext;
    i -= 1;
    if (i < RXC_K * 3u) return &c->cap_cand[i / 3u][i % 3u];
    i -= RXC_K * 3u;
    if (i < 4u) return &c->cap_verify[i];
    return &c->cap_commit[i - 4u];
}

static int mint_next(RxCompose *c, uint32_t subject, uint64_t resource, uint32_t rights) {
    if (c->n_minted >= RXC_N_CAPS) return RX_ERR_FULL;   /* cap_at has RXC_N_CAPS slots */
    int rc = mint(c, subject, resource, rights, cap_at(c, c->n_minted));
    if (rc == RX_OK) c->n_minted++;
    return rc;
}

static int add_reaction(RxCompose *c, const RxReactionDesc *d, uint32_t *id) {
    int rc = c->keys ? rx_world_add_reaction_keyed(c->world, c->keys, d, id)
                     : rx_world_add_reaction(c->world, d, id);
    if (rc == RX_OK) c->n_rx++;
    return rc;
}

/* Objects, authority, reactions, binder and Cortex link of the composition
 * in c->world. `own`: the World was just built for it (open): its object ids
 * must be 0..4 and its whole-World recorder is the composition journal, as
 * before attach existed. Otherwise (attach) ids are whatever the World hands
 * out and a scoped Cortex link maps them to the composition subjects. Every
 * subject and resource is the instance's own (set_instance), so two
 * compositions in one World hold no right on each other's objects. */
static int build_in_world(RxCompose *c, int own) {
    int rc;
    /* The binder first: it claims the instance's slot in the World. */
    if ((rc = rx_world_set_binder(c->world, bind_check, bind_fn, bind_abort, c)) != RX_OK)
        return rc;
    c->has_binder = 1;
    uint64_t z[RX_MAX_FIELDS] = { 0 }, st[RX_MAX_FIELDS] = { 0 };
    st[RXC_S_REF] = ref_pack(c->recovered);
    if (c->recovered_record) {
        const uint64_t *p = payload_of(&c->cx, c->recovered_record, CX_WREC_WORDS);
        if (p) {
            st[RXC_S_RESULT] = p[CX_WREC_FIELD0 + RXC_S_RESULT];
            st[RXC_S_GOAL] = p[CX_WREC_FIELD0 + RXC_S_GOAL];
        }
    }
    RxObjRef o[5];
    for (uint32_t i = 0; i < 5; i++) {
        rc = rx_world_create(c->world, 1, RX_PERSIST_RESIDENT, c->res[i],
                             i == RXC_SLOT_STATE ? st : z, &o[i]);
        if (rc != RX_OK) return rc;
        c->obj[c->n_objs++] = o[i];
        if (own && o[i].id != i) return RX_ERR_BAD_DESC;   /* slot layout is part of the record */
    }
    c->goal = o[RXC_SLOT_GOAL];
    c->cand[0] = o[RXC_SLOT_CAND0];
    c->cand[1] = o[RXC_SLOT_CAND1];
    c->verdict = o[RXC_SLOT_VERDICT];
    c->state = o[RXC_SLOT_STATE];
    const uint64_t *R = c->res;

    /* Each step holds only the rights it needs. Outside input comes in under
     * the World's own external subject, on the goal resource only. */
    if ((rc = mint_next(c, c->world->external_subject, R[RXC_SLOT_GOAL], RX_RIGHT_WRITE)))
        return rc;
    for (uint32_t k = 0; k < RXC_K; k++) {
        uint32_t subj = c->subj_cand[k];
        if ((rc = mint_next(c, subj, R[RXC_SLOT_GOAL], RX_RIGHT_READ)) ||
            (rc = mint_next(c, subj, R[RXC_SLOT_STATE], RX_RIGHT_READ)) ||
            (rc = mint_next(c, subj, R[RXC_SLOT_CAND0 + k], RX_RIGHT_WRITE)))
            return rc;
    }
    if ((rc = mint_next(c, c->subj_aegis, R[RXC_SLOT_CAND0], RX_RIGHT_READ)) ||
        (rc = mint_next(c, c->subj_aegis, R[RXC_SLOT_CAND1], RX_RIGHT_READ)) ||
        (rc = mint_next(c, c->subj_aegis, R[RXC_SLOT_GOAL], RX_RIGHT_READ)) ||
        (rc = mint_next(c, c->subj_aegis, R[RXC_SLOT_VERDICT], RX_RIGHT_WRITE)) ||
        (rc = mint_next(c, c->subj_commit, R[RXC_SLOT_VERDICT], RX_RIGHT_READ)) ||
        (rc = mint_next(c, c->subj_commit, R[RXC_SLOT_STATE], RX_RIGHT_WRITE)))
        return rc;

    for (uint32_t k = 0; k < RXC_K; k++) {
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = k ? "compose.candidate.1" : "compose.candidate.0";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = c->subj_cand[k];
        d.priority = RX_PRIO_FOREGROUND;
        d.triggers[d.n_triggers++] = (RxDep){ c->goal, RX_ALL_FIELDS };
        d.reads[d.n_reads++] = (RxDep){ c->state, RX_FIELD(RXC_S_REF) };
        d.writes[d.n_writes++] = (RxDep){ c->cand[k], RX_ALL_FIELDS };
        d.caps[d.n_caps++] = (RxCapNeed){ c->cap_cand[k][0], R[RXC_SLOT_GOAL], RX_RIGHT_READ };
        d.caps[d.n_caps++] = (RxCapNeed){ c->cap_cand[k][1], R[RXC_SLOT_STATE], RX_RIGHT_READ };
        d.caps[d.n_caps++] = (RxCapNeed){ c->cap_cand[k][2], R[RXC_SLOT_CAND0 + k],
                                          RX_RIGHT_WRITE };
        c->cand_user[k] = (struct RxcCandUser){ c, k };
        d.fn = candidate_fn;
        d.user = &c->cand_user[k];
        if ((rc = add_reaction(c, &d, &c->rx_cand[k])) != RX_OK) return rc;
    }
    RxReactionDesc v;
    memset(&v, 0, sizeof v);
    v.name = "compose.verify";
    v.faculty = RX_FACULTY_AEGIS;
    v.subject = c->subj_aegis;
    v.priority = RX_PRIO_FOREGROUND;
    v.triggers[v.n_triggers++] = (RxDep){ c->cand[0], RX_ALL_FIELDS };
    v.triggers[v.n_triggers++] = (RxDep){ c->cand[1], RX_ALL_FIELDS };
    v.reads[v.n_reads++] = (RxDep){ c->goal, RX_ALL_FIELDS };
    v.writes[v.n_writes++] = (RxDep){ c->verdict, RX_ALL_FIELDS };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[0], R[RXC_SLOT_CAND0], RX_RIGHT_READ };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[1], R[RXC_SLOT_CAND1], RX_RIGHT_READ };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[2], R[RXC_SLOT_GOAL], RX_RIGHT_READ };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[3], R[RXC_SLOT_VERDICT], RX_RIGHT_WRITE };
    v.fn = verify_fn;
    v.user = c;
    if ((rc = add_reaction(c, &v, &c->rx_verify)) != RX_OK) return rc;

    RxReactionDesc m;
    memset(&m, 0, sizeof m);
    m.name = "compose.commit";
    m.faculty = RX_FACULTY_OMEGA;
    m.subject = c->subj_commit;
    m.priority = RX_PRIO_FOREGROUND;
    m.triggers[m.n_triggers++] = (RxDep){ c->verdict, RX_ALL_FIELDS };
    m.writes[m.n_writes++] = (RxDep){ c->state, RX_ALL_FIELDS };
    m.caps[m.n_caps++] = (RxCapNeed){ c->cap_commit[0], R[RXC_SLOT_VERDICT], RX_RIGHT_READ };
    m.caps[m.n_caps++] = (RxCapNeed){ c->cap_commit[1], R[RXC_SLOT_STATE], RX_RIGHT_WRITE };
    m.fn = commit_fn;
    m.user = c;
    if ((rc = add_reaction(c, &m, &c->rx_commit)) != RX_OK) return rc;

    if ((rc = rx_world_bind_field(c->world, c, c->state, RXC_S_REF)) != RX_OK) return rc;
    if (own) {
        rc = rx_cortex_attach(c->world, &c->cx, c->session);
    } else {
        uint64_t subj[5];
        for (uint32_t i = 0; i < 5; i++) subj[i] = RXC_CX_SUBJECT(i);
        rc = rx_cortex_attach_scoped(c->world, &c->cx, c->session, o, subj, 5);
    }
    if (rc != RX_OK) return rc;
    c->attached = 1;
    return RX_OK;
}

static int build_world(RxCompose *c, AienosCapView *view, uint32_t n_workers) {
    int rc = rx_world_init_native(&c->w, view, n_workers, 1u << 14);
    if (rc != RX_OK) return rc;
    c->world = &c->w;
    c->owns_world = 1;
    c->w.external_subject = RXC_SUBJ_EXTERNAL;
    set_instance(c, 0);
    return build_in_world(c, 1);
}

/* ---- public API ------------------------------------------------------------ */

/* Shared by open and attach: reset (keeping the caller's test settings,
 * but not whether a fault already fired), machine identity, Cortex journal,
 * durable J-Space, OLD-or-NEW recovery. */
static int open_home(RxCompose *c, const char *dir, const AienMachineId *self, uint64_t session,
                     const SrRouter *router, RxcContract contract, AienosCapAdmin *admin) {
    struct RxcTestHooks hooks = c->test;
    hooks.fault_hit = 0;
    hooks.held = hooks.release = hooks.hold_done = 0;
    memset(c, 0, sizeof *c);
    c->test = hooks;
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    c->self = *self;
    c->session = session;
    c->router = router;
    c->contract = contract;
    c->admin = admin;
    if (mkdir(dir, 0700) && errno != EEXIST) return RX_ERR_ARG;

    char p[256];
    path_in(p, sizeof p, dir, "machine.id");
    if (access(p, F_OK) == 0) {
        AienMachineId stored;
        if (aien_mid_load(p, &stored) != AIEN_MID_OK || !aien_mid_equal(&stored, self))
            return RX_ERR_IDENTITY;
    } else if (aien_mid_store(p, self) != AIEN_MID_OK) {
        return RX_ERR_IDENTITY;
    }

    path_in(p, sizeof p, dir, "cortex.cx");
    if (cx_open(&c->cx, p, RX_CORTEX_SUBJECTS, CX_OPEN_SYNC | CX_OPEN_REPAIR_TAIL) != CX_OK)
        return RX_ERR_REPLAY;
    if (cx_verify_chain(&c->cx) != CX_OK) { cx_close(&c->cx); return RX_ERR_REPLAY; }

    JsHome home;
    memset(&home, 0, sizeof home);
    aien_mid_to_slot(self, home.machine);
    home.locality = JS_HOME_LOCAL;
    const JsRealizer *rz[1] = { &RXC_REALIZER };
    path_in(p, sizeof p, dir, "jspace");
    if (js_space_open(&c->js, p, rz, 1, NULL, &home) != JS_OK) {
        cx_close(&c->cx);
        return RX_ERR_REPLAY;
    }
    int rc = recover(c);
    if (rc != RX_OK) {
        js_space_destroy(&c->js);
        cx_close(&c->cx);
    }
    return rc;
}

/* 1 while any of the composition's own reactions may still call into it:
 * admitted (READY, BLOCKED_RESOURCE), running or publishing, or due to run
 * again (re-armed, parked, deferred, resume or sequential pulse pending,
 * holding). Read under the World lock. */
static int own_busy(RxCompose *c) {
    uint32_t ids[4] = { c->rx_cand[0], c->rx_cand[1], c->rx_verify, c->rx_commit };
    int busy = 0;
    pthread_mutex_lock(&c->world->mu);
    for (uint32_t i = 0; i < c->n_rx && i < 4 && !busy; i++) {
        if (ids[i] >= c->world->n_reactions) continue;
        const RxReaction *r = &c->world->reactions[ids[i]];
        busy = r->state == RX_READY || r->state == RX_RUNNING || r->state == RX_PUBLISHING ||
               r->state == RX_BLOCKED_RESOURCE || r->rearm || r->parked || r->deferred ||
               r->resume_pending || r->holding || r->seq_pending;
    }
    /* A wake held back by the World's fan-out limit waits in its backlog
     * with the reaction still DORMANT: that is a pending run as well. */
    for (uint32_t j = 0; j < c->world->deferred_len && !busy; j++) {
        uint32_t rid = c->world->deferred[c->world->deferred_head + j].reaction;
        for (uint32_t i = 0; i < c->n_rx && i < 4; i++)
            if (rid == ids[i]) busy = 1;
    }
    pthread_mutex_unlock(&c->world->mu);
    return busy;
}

/* Wait until none of the composition's own steps can still run (attach
 * mode: the rest of a living World need never be quiet). RX_ERR_TIMEOUT
 * after timeout_ms. */
static int wait_own(RxCompose *c, int timeout_ms) {
    struct timespec ts = { 0, 100000 }, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (own_busy(c)) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int64_t ms = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= timeout_ms) return RX_ERR_TIMEOUT;
        nanosleep(&ts, NULL);
    }
    return RX_OK;
}

/* Undo what attach put into the caller's World; the World keeps running
 * (rx_compose.h, close in attach mode, steps 1-5). */
static void leave_world(RxCompose *c) {
    /* 1. Revoke: from here on no composition step can publish anything. */
    AienosCapRef office;
    int have_office = aienos_cap_office(c->admin, &office) == 0;
    if (have_office)
        for (uint32_t i = 0; i < c->n_minted; i++) {
            RxCapRef *r = cap_at(c, i);
            (void)aienos_cap_revoke(c->admin, office, (AienosCapRef){ r->cap_id, r->generation });
        }
    /* 2. The composition journal takes no further World records. */
    if (c->attached) rx_cortex_detach_store(c->world, &c->cx);
    c->attached = 0;
    /* 3. Its own steps still use c: wait until none can run. No cutoff. */
    struct timespec ts = { 0, 200000 };
    while (own_busy(c)) nanosleep(&ts, NULL);
    /* 4. Binder, objects, capability slots. */
    if (c->has_binder) rx_world_clear_binder(c->world, c);
    c->has_binder = 0;
    for (uint32_t i = 0; i < c->n_objs; i++) rx_world_retire(c->world, c->obj[i]);
    c->n_objs = 0;
    if (have_office)
        for (uint32_t i = 0; i < c->n_minted; i++)
            (void)aienos_cap_reclaim(c->admin, office, cap_at(c, i)->cap_id);
    c->n_minted = 0;
    /* 5. Its reactions leave the World (rx_world_remove_reaction): their
     * subscriptions go and their slots are reused by the next attach of the
     * same instance. Every object they name is retired, so nothing can wake
     * them now; a BUSY answer is a step that is just finishing. */
    uint32_t ids[4] = { c->rx_cand[0], c->rx_cand[1], c->rx_verify, c->rx_commit };
    for (uint32_t i = 0; i < c->n_rx && i < 4; i++)
        while (rx_world_remove_reaction(c->world, ids[i]) == RX_ERR_BUSY) nanosleep(&ts, NULL);
    c->n_rx = 0;
}

int rx_compose_open(RxCompose *c, const char *dir, const AienMachineId *self, uint64_t session,
                    const SrRouter *router, RxcContract contract, AienosCapAdmin *admin,
                    AienosCapView *view, uint32_t n_workers) {
    if (!c || !dir || !self || !admin || !view || strlen(dir) >= sizeof c->dir - 32)
        return RX_ERR_ARG;
    int rc = open_home(c, dir, self, session, router, contract, admin);
    if (rc != RX_OK) return rc;
    c->world = &c->w;
    rc = build_world(c, view, n_workers ? n_workers : 1);
    if (rc != RX_OK) {
        if (c->w.n_workers) rx_world_destroy(&c->w);
        c->world = NULL;
        js_space_destroy(&c->js);
        cx_close(&c->cx);
        return rc;
    }
    return RX_OK;
}

int rx_compose_enroll_callers(RxWorld *w, RxCallerKeyring *keys) {
    if (!w || !keys) return RX_ERR_ARG;
    static const uint32_t role[4] = { RXC_SUBJ_CAND0, RXC_SUBJ_CAND1, RXC_SUBJ_AEGIS,
                                      RXC_SUBJ_COMMIT };
    _Static_assert(RXC_MAX_ACTIVE * 4u <= RX_CALLER_KEYRING_MAX,
                   "one keyring holds every instance's composition subjects");
    memset(keys, 0, sizeof *keys);
    for (uint32_t inst = 0; inst < RXC_MAX_ACTIVE; inst++)
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t s = RXC_SUBJ_OF(inst, role[i]);
            if (keys->n >= RX_CALLER_KEYRING_MAX ||
                rx_world_enroll_caller(w, s, &keys->cred[keys->n]) != RX_CALLER_OK) {
                for (unsigned b = 0; b < sizeof *keys; b++) ((volatile uint8_t *)keys)[b] = 0;
                return RX_ERR_IDENTITY;   /* bound already, enrolled, full or no entropy */
            }
            keys->subject[keys->n++] = s;
        }
    return RX_OK;
}

/* Attaches are serialized process-wide, so choosing an instance slot and
 * claiming it (installing the binder) is one step; runs and closes are not
 * serialized. */
static pthread_mutex_t g_attach_mu = PTHREAD_MUTEX_INITIALIZER;

/* Under w->mu: the instance slots taken in `w` by attached compositions
 * (bit i = instance i), and whether one of them uses `dir`. */
static uint32_t taken_instances(RxWorld *w, const char *dir, int *same_dir) {
    uint32_t taken = 0;
    *same_dir = 0;
    for (uint32_t i = 0; i < RX_MAX_BINDERS; i++) {
        if (w->binder[i].check != bind_check || !w->binder[i].ctx) continue;
        const RxCompose *o = w->binder[i].ctx;
        if (o->inst < RXC_MAX_ACTIVE) taken |= 1u << o->inst;
        if (strcmp(o->dir, dir) == 0) *same_dir = 1;
    }
    return taken;
}

/* Under w->mu: table slots the four reactions of `inst` still need (a removed
 * slot of the same subject and faculty is reused, rx_world_remove_reaction). */
static uint32_t slots_needed(const RxWorld *w, uint32_t inst) {
    const uint32_t subj[4] = { RXC_SUBJ_OF(inst, RXC_SUBJ_CAND0), RXC_SUBJ_OF(inst, RXC_SUBJ_CAND1),
                               RXC_SUBJ_OF(inst, RXC_SUBJ_AEGIS), RXC_SUBJ_OF(inst, RXC_SUBJ_COMMIT) };
    const uint32_t fac[4] = { RX_FACULTY_OMEGA, RX_FACULTY_OMEGA, RX_FACULTY_AEGIS,
                              RX_FACULTY_OMEGA };
    uint32_t need = 0;
    for (uint32_t k = 0; k < 4; k++) {
        int found = 0;
        for (uint32_t i = 0; i < w->n_reactions && !found; i++)
            found = w->reactions[i].removed && w->reactions[i].desc.subject == subj[k] &&
                    w->reactions[i].desc.faculty == fac[k];
        if (!found) need++;
    }
    return need;
}

int rx_compose_attach(RxCompose *c, RxWorld *w, const RxCallerKeyring *keys, const char *dir,
                      const AienMachineId *self, uint64_t session, const SrRouter *router,
                      RxcContract contract, AienosCapAdmin *admin) {
    if (!c || !w || !dir || !self || !admin || strlen(dir) >= sizeof c->dir - 32)
        return RX_ERR_ARG;
    /* The World's outside subject must not be a composition subject. */
    if (w->external_subject >= RXC_SUBJ_EXTERNAL &&
        w->external_subject < RXC_SUBJ_OF(RXC_MAX_ACTIVE, RXC_SUBJ_EXTERNAL))
        return RX_ERR_ARG;
    pthread_mutex_lock(&g_attach_mu);
    /* A free instance slot (RXC_MAX_ACTIVE per World), no composition of this
     * World already on `dir`, and room for its four reactions, read together
     * under the World lock. */
    char d[sizeof c->dir];
    snprintf(d, sizeof d, "%s", dir);
    int same_dir;
    pthread_mutex_lock(&w->mu);
    uint32_t taken = taken_instances(w, d, &same_dir), inst = RXC_MAX_ACTIVE;
    for (uint32_t i = 0; i < RXC_MAX_ACTIVE && inst == RXC_MAX_ACTIVE; i++)
        if (!(taken & (1u << i))) inst = i;
    int full = inst < RXC_MAX_ACTIVE && w->n_reactions + slots_needed(w, inst) > RX_MAX_REACTIONS;
    pthread_mutex_unlock(&w->mu);
    int rc = same_dir || inst == RXC_MAX_ACTIVE ? RX_ERR_EXISTS : full ? RX_ERR_FULL : RX_OK;
    if (rc == RX_OK) rc = open_home(c, dir, self, session, router, contract, admin);
    if (rc != RX_OK) {
        pthread_mutex_unlock(&g_attach_mu);
        return rc;
    }
    c->world = w;
    c->owns_world = 0;
    c->keys = keys;
    set_instance(c, inst);
    rc = build_in_world(c, 0);
    if (rc != RX_OK) {
        leave_world(c);
        c->world = NULL;
        js_space_destroy(&c->js);
        cx_close(&c->cx);
    }
    pthread_mutex_unlock(&g_attach_mu);
    return rc;
}

int rx_compose_set_remote(RxCompose *c, RxcRemoteRun run, void *ctx) {
    if (!c || !c->world) return RX_ERR_ARG;
    c->remote.run = run;
    c->remote.ctx = run ? ctx : NULL;
    return RX_OK;
}

JsBranchRef rx_compose_state(RxCompose *c) {
    RxObject o;
    if (rx_world_read(c->world, c->state, &o) != RX_OK) return (JsBranchRef){ UINT32_MAX, 0 };
    return ref_unpack(o.field[RXC_S_REF]);
}

/* Cortex id of the record of the publication that last wrote obj.field. */
static uint64_t record_of(RxCompose *c, RxObjRef obj, uint32_t field) {
    return rx_cortex_record_in(c->world, &c->cx, rx_world_explain(c->world, obj, field));
}

/* Remember an incomplete record so the next run completes it first; without
 * the World records naming it, refuse further runs until reopened. */
static void set_pending(RxCompose *c, uint64_t S, uint64_t V, int committed, JsBranchRef old,
                        int released, const uint8_t (*cdig)[32]) {
    if (!V || (committed && !S)) {
        c->pending = 2;
        return;
    }
    c->pending = 1;
    c->pend_S = S;
    c->pend_V = V;
    c->pend_old = old;
    c->pend_released = released;
    memcpy(c->pend_cdig, cdig, sizeof c->pend_cdig);
}

int rx_compose_run(RxCompose *c, uint64_t input, const SrRequirement *req, const CqHeld *held,
                   uint64_t now_us, RxcResult *out) {
    if (!c || !out) return RX_ERR_ARG;
    memset(out, 0, sizeof *out);
    out->winner = RXC_NONE;
    /* A previous run left its record incomplete or its state not durable:
     * never start another goal over it. */
    if (c->pending == 2) return RX_ERR_REPLAY;
    if (c->pending == 1) {
        int m = compose_records(c, c->pend_S, c->pend_V,
                                (const uint8_t (*)[32])c->pend_cdig, 0, NULL);
        if (m == -1 || (m == -2 && c->pend_S)) return RX_ERR_REPLAY;
        if (c->pend_S && !c->pend_released) {
            js_branch_release_ref(&c->js, c->pend_old, 0);
            c->pend_released = 1;
        }
        if (c->pend_S && js_space_commit(&c->js) != JS_OK) return RX_ERR_REPLAY;
        out->prior_completed = m > 0 ? (uint32_t)m : 0;
        c->pending = 0;
    }
    /* Routing discovers the alternatives; it mints nothing. */
    SrRoute routes[RXC_K];
    int n = sr_route_alternatives(c->router, req, held, now_us, routes, RXC_K);
    if (n < 0) return n;
    c->n_routes = (uint32_t)n;
    memcpy(c->run_route, routes, sizeof routes);
    out->n_alternatives = (uint32_t)n;
    memcpy(out->route, routes, sizeof routes);

    JsBranchRef old = rx_compose_state(c);
    out->old_ref = old;
    uint64_t seq = ++c->seq;
    c->run_input = input;
    c->run_now = now_us;
    uint64_t sk[RXC_K] = { 0, 0 };
    for (uint32_t k = 0; k < (uint32_t)n && k < RXC_K; k++)
        sk[k] = ((uint64_t)routes[k].skill_version << 32) | routes[k].chosen.skill_id;
    RxMutation g[5] = {
        { c->goal, RXC_G_INPUT, input }, { c->goal, RXC_G_OP, req->need.semantic_operation },
        { c->goal, RXC_G_SKILL0, sk[0] }, { c->goal, RXC_G_SKILL1, sk[1] },
        { c->goal, RXC_G_SEQ, seq } };
    int64_t crumb = rx_world_publish_external(c->world, c->cap_ext, g, 5);
    if (crumb < 0) return (int)crumb;
    out->goal_crumb = (uint64_t)crumb;
    /* Its own World: wait for all of it. Attached: only for its own steps (the
     * rest of a living World, or another composition, need never be quiet). */
    int rc = c->owns_world ? rx_world_wait_quiescent(c->world, 30000) : wait_own(c, 30000);
    if (rc != RX_OK) return rc;

    /* settle */
    RxObject so, vo, co[RXC_K];
    if (rx_world_read(c->world, c->state, &so) != RX_OK ||
        rx_world_read(c->world, c->verdict, &vo) ||
        rx_world_read(c->world, c->cand[0], &co[0]) || rx_world_read(c->world, c->cand[1], &co[1]))
        return RX_ERR_NOT_FOUND;
    uint8_t cdig[RXC_K][32];
    memset(cdig, 0, sizeof cdig);
    for (uint32_t k = 0; k < RXC_K; k++) {
        out->cand_ref[k] = co[k].field[RXC_C_GOAL] == seq ? ref_unpack(co[k].field[RXC_C_REF])
                                                         : (JsBranchRef){ 0, 0 };
        if (co[k].field[RXC_C_GOAL] == seq && co[k].field[RXC_C_REF] &&
            js_branch_check(&c->js, out->cand_ref[k]) == JS_OK)
            js_branch_content_digest(&c->js, out->cand_ref[k].id, cdig[k]);
    }
    JsBranchRef nw = ref_unpack(so.field[RXC_S_REF]);
    int committed = !ref_eq(nw, old);
    out->reclaimed = js_space_reclaim_staged(&c->js);
    if (vo.field[RXC_V_GOAL] == seq) out->winner = (uint32_t)vo.field[RXC_V_WINNER];

    if (committed) {
        out->new_ref = nw;
        out->result = so.field[RXC_S_RESULT];
        if (fault(c, RXC_FP_RECLAIM, 0) || js_space_commit(&c->js) != JS_OK) {
            c->pending = 2;
            out->outcome = RXC_OUT_NOT_DURABLE;
            return RX_OK;
        }
        js_branch_content_digest(&c->js, nw.id, out->winner_digest);
        uint64_t S = record_of(c, c->state, RXC_S_REF);
        uint64_t V = record_of(c, c->verdict, RXC_V_GOAL);
        if (!S || !V || compose_records(c, S, V, (const uint8_t (*)[32])cdig, 1, out) < 0) {
            set_pending(c, S, V, 1, old, 0, (const uint8_t (*)[32])cdig);
            out->outcome = RXC_OUT_RECORD_FAILED;
            return RX_OK;
        }
        js_branch_release_ref(&c->js, old, 0);   /* the superseded branch */
        if (js_space_commit(&c->js) != JS_OK) {
            set_pending(c, S, V, 1, old, 1, (const uint8_t (*)[32])cdig);
            out->outcome = RXC_OUT_RECORD_FAILED;
            return RX_OK;
        }
        out->outcome = RXC_OUT_COMMITTED;
        return RX_OK;
    }
    out->new_ref = old;
    if (vo.field[RXC_V_GOAL] == seq && vo.field[RXC_V_WINNER] == RXC_NONE) {
        uint64_t V = record_of(c, c->verdict, RXC_V_GOAL);
        if (!V || compose_records(c, 0, V, (const uint8_t (*)[32])cdig, 1, out) < 0) {
            set_pending(c, 0, V, 0, old, 1, (const uint8_t (*)[32])cdig);
            out->outcome = RXC_OUT_RECORD_FAILED;
            return RX_OK;
        }
        out->outcome = RXC_OUT_NO_WINNER;
        return RX_OK;
    }
    out->outcome = RXC_OUT_NOT_COMMITTED;
    return RX_OK;
}

void rx_compose_close(RxCompose *c) {
    if (!c || !c->world) return;
    if (c->owns_world) {
        rx_world_destroy(&c->w);     /* detaches the recorder */
        c->attached = 0;
        c->has_binder = 0;
        c->n_objs = 0;
    } else {
        /* Leave the caller's World running; returns once none of the
         * composition's own steps can run (rx_compose.h). */
        leave_world(c);
    }
    c->world = NULL;
    js_space_destroy(&c->js);    /* never commits: durable state is what settle made durable */
    cx_close(&c->cx);
}

void rx_compose_record_digest(const CxStore *s, uint8_t out[32]) {
    sha256_ctx h;
    sha256_init(&h);
    for (uint64_t id = 1; s && id <= s->n; id++) {
        const CxObject *o = cx_get(s, id);
        if (!o) break;
        uint64_t hw[13] = { o->cls, o->kind, o->subject, o->t, o->generation, o->branch,
                            o->protect, o->tag, o->links[0], o->links[1], o->links[2],
                            o->links[3], o->n };
        sha256_update(&h, (const uint8_t *)hw, sizeof hw);
        const uint64_t *p = cx_payload(s, o);
        int world = o->kind >= CX_K_WORK_ACCEPTED && o->kind <= CX_K_EXEC_FAILED;
        for (uint32_t i = 0; i < o->n; i++) {
            uint64_t v = p[i];
            /* Timing, and the crumb digest (it binds capability generations,
             * which the authority seeds per start), are not record content. */
            if (world && ((i >= CX_WREC_DIGEST0 && i < CX_WREC_DIGEST0 + 4) ||
                          i == CX_WREC_T_START_NS || i == CX_WREC_T_END_NS))
                v = 0;
            sha256_update(&h, (const uint8_t *)&v, 8);
        }
    }
    sha256_final(&h, out);
}
