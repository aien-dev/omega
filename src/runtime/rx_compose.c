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

/* One fault point. Fires once; candidate-side points fire only for fault_k.
 * Returns 1 when the caller must fail in process. */
static int fault(RxCompose *c, int point, uint32_t k) {
    if (c->fault_point != point) return 0;
    if ((point == RXC_FP_BEFORE_FORK || point == RXC_FP_CANDIDATE) && k != c->fault_k) return 0;
    if (__atomic_exchange_n(&c->fault_hit, 1, __ATOMIC_ACQ_REL)) return 0;
    if (c->fault_crash) _exit(RXC_CRASH_EXIT);
    return 1;
}

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
    uint64_t ref = 0, result = 0;
    if (k < c->n_routes && skill_word != 0 && c->run_route[k].verdict == SR_OK) {
        const AgSkill *s = skill_of(c, (uint32_t)skill_word);
        const uint8_t *want = c->run_route[k].skill_digest;
        if (s && s->fn && (digest_zero(want) || memcmp(want, s->identity, 32) == 0)) {
            if (fault(c, RXC_FP_BEFORE_FORK, k)) return -1;
            JsBranchRef base = ref_unpack(st->field[RXC_S_REF]), next;
            uint32_t subj = k == 0 ? RXC_SUBJ_CAND0 : RXC_SUBJ_CAND1;
            if (js_branch_fork_staged(&c->js, base, subj, &next) != JS_OK) return -1;
            uint64_t in = g->field[RXC_G_INPUT];
            int failed = 0;
            uint64_t r = s->fn(&in, 1, 0, &failed);
            if (failed || js_branch_derive(&c->js, next.id, r) != JS_OK) {
                js_branch_release_ref(&c->js, next, subj);
            } else {
                if (fault(c, RXC_FP_CANDIDATE, k)) return -1;
                ref = ref_pack(next);
                result = r;
                if (k == 0 && c->test_rogue_candidate)
                    out_put(x, c->state, RXC_S_REF, ref);   /* refused: not in its write set */
            }
        }
    }
    RxObjRef me = c->cand[k];
    out_put(x, me, RXC_C_REF, ref);
    out_put(x, me, RXC_C_RESULT, result);
    out_put(x, me, RXC_C_SKILL, skill_word);
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
    if (subject != RXC_SUBJ_COMMIT) return RX_ERR_BINDING;
    JsBranchRef old = ref_unpack(old_value), nw = ref_unpack(new_value);
    JsBranchInfo bi;
    if (js_branch_info(&c->js, nw, &bi) != JS_OK) return RX_ERR_BINDING;
    if (!bi.staged || bi.locality != JS_HOME_LOCAL) return RX_ERR_BINDING;
    if (bi.owner != RXC_SUBJ_CAND0 && bi.owner != RXC_SUBJ_CAND1) return RX_ERR_BINDING;
    if (bi.parent != old.id || bi.parent_gen != old.gen) return RX_ERR_BINDING;
    if (fault(c, RXC_FP_AFTER_VALIDATION, 0)) return RX_ERR_BINDING;
    return RX_OK;
}

static int bind_fn(void *ctx, RxObjRef obj, uint32_t field, uint64_t old_value,
                   uint64_t new_value, uint32_t subject) {
    (void)obj; (void)field; (void)old_value; (void)subject;
    RxCompose *c = ctx;
    JsBranchRef nw = ref_unpack(new_value);
    if (c->fault_point == RXC_FP_SEAL &&
        !__atomic_exchange_n(&c->fault_hit, 1, __ATOMIC_ACQ_REL)) {
        if (!c->fault_crash) return RX_ERR_BINDING;   /* seal refused */
        js_branch_seal(&c->js, nw);
        js_branch_set_owner(&c->js, nw, 0);
        _exit(RXC_CRASH_EXIT);                        /* crash right after the seal */
    }
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
        h->t = rx_cortex_next_t(&c->w);
        return rx_cortex_append(&c->w, h, p, n, id) == RX_OK ? 0 : -1;
    }
    h->t = c->cx.n + 1;
    return cx_append(&c->cx, h, p, n, id) == CX_OK ? 0 : -1;
}

static int cx_prom(RxCompose *c, uint64_t cand, uint64_t ev, uint64_t *id) {
    if (c->attached) return rx_cortex_promote(&c->w, cand, ev, id) == RX_OK ? 0 : -1;
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
            /* The procedure that produced it: its executable identity, which
             * the router admitted only when it equals the graph's digest. */
            const AgSkill *sk = skill_of(c, (uint32_t)cf[k][RXC_C_SKILL]);
            if (sk && cf[k][RXC_C_SKILL]) words_from_digest(sk->identity, p + RXC_CP_SKILLDIG0);
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
        p[RXC_EP_VERIFIER] = RXC_SUBJ_AEGIS;
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

static int build_world(RxCompose *c, AienosCapView *view, uint32_t n_workers) {
    int rc = rx_world_init_native(&c->w, view, n_workers, 1u << 14);
    if (rc != RX_OK) return rc;
    c->w.external_subject = RXC_SUBJ_EXTERNAL;
    uint64_t z[RX_MAX_FIELDS] = { 0 }, st[RX_MAX_FIELDS] = { 0 };
    st[RXC_S_REF] = ref_pack(c->recovered);
    if (c->recovered_record) {
        const uint64_t *p = payload_of(&c->cx, c->recovered_record, CX_WREC_WORDS);
        if (p) {
            st[RXC_S_RESULT] = p[CX_WREC_FIELD0 + RXC_S_RESULT];
            st[RXC_S_GOAL] = p[CX_WREC_FIELD0 + RXC_S_GOAL];
        }
    }
    static const uint64_t res[5] = { RXC_RES_GOAL, RXC_RES_CAND0, RXC_RES_CAND1, RXC_RES_VERDICT,
                                     RXC_RES_STATE };
    RxObjRef o[5];
    for (uint32_t i = 0; i < 5; i++) {
        rc = rx_world_create(&c->w, 1, RX_PERSIST_RESIDENT, res[i], i == RXC_SLOT_STATE ? st : z,
                             &o[i]);
        if (rc != RX_OK) return rc;
        if (o[i].id != i) return RX_ERR_BAD_DESC;   /* slot layout is part of the record */
    }
    c->goal = o[RXC_SLOT_GOAL];
    c->cand[0] = o[RXC_SLOT_CAND0];
    c->cand[1] = o[RXC_SLOT_CAND1];
    c->verdict = o[RXC_SLOT_VERDICT];
    c->state = o[RXC_SLOT_STATE];

    /* Each step holds only the rights it needs. */
    if ((rc = mint(c, RXC_SUBJ_EXTERNAL, RXC_RES_GOAL, RX_RIGHT_WRITE, &c->cap_ext))) return rc;
    for (uint32_t k = 0; k < RXC_K; k++) {
        uint32_t subj = k ? RXC_SUBJ_CAND1 : RXC_SUBJ_CAND0;
        if ((rc = mint(c, subj, RXC_RES_GOAL, RX_RIGHT_READ, &c->cap_cand[k][0])) ||
            (rc = mint(c, subj, RXC_RES_STATE, RX_RIGHT_READ, &c->cap_cand[k][1])) ||
            (rc = mint(c, subj, res[RXC_SLOT_CAND0 + k], RX_RIGHT_WRITE, &c->cap_cand[k][2])))
            return rc;
    }
    if ((rc = mint(c, RXC_SUBJ_AEGIS, RXC_RES_CAND0, RX_RIGHT_READ, &c->cap_verify[0])) ||
        (rc = mint(c, RXC_SUBJ_AEGIS, RXC_RES_CAND1, RX_RIGHT_READ, &c->cap_verify[1])) ||
        (rc = mint(c, RXC_SUBJ_AEGIS, RXC_RES_GOAL, RX_RIGHT_READ, &c->cap_verify[2])) ||
        (rc = mint(c, RXC_SUBJ_AEGIS, RXC_RES_VERDICT, RX_RIGHT_WRITE, &c->cap_verify[3])) ||
        (rc = mint(c, RXC_SUBJ_COMMIT, RXC_RES_VERDICT, RX_RIGHT_READ, &c->cap_commit[0])) ||
        (rc = mint(c, RXC_SUBJ_COMMIT, RXC_RES_STATE, RX_RIGHT_WRITE, &c->cap_commit[1])))
        return rc;

    for (uint32_t k = 0; k < RXC_K; k++) {
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = k ? "compose.candidate.1" : "compose.candidate.0";
        d.faculty = RX_FACULTY_OMEGA;
        d.subject = k ? RXC_SUBJ_CAND1 : RXC_SUBJ_CAND0;
        d.priority = RX_PRIO_FOREGROUND;
        d.triggers[d.n_triggers++] = (RxDep){ c->goal, RX_ALL_FIELDS };
        d.reads[d.n_reads++] = (RxDep){ c->state, RX_FIELD(RXC_S_REF) };
        d.writes[d.n_writes++] = (RxDep){ c->cand[k], RX_ALL_FIELDS };
        d.caps[d.n_caps++] = (RxCapNeed){ c->cap_cand[k][0], RXC_RES_GOAL, RX_RIGHT_READ };
        d.caps[d.n_caps++] = (RxCapNeed){ c->cap_cand[k][1], RXC_RES_STATE, RX_RIGHT_READ };
        d.caps[d.n_caps++] = (RxCapNeed){ c->cap_cand[k][2], res[RXC_SLOT_CAND0 + k], RX_RIGHT_WRITE };
        c->cand_user[k] = (struct RxcCandUser){ c, k };
        d.fn = candidate_fn;
        d.user = &c->cand_user[k];
        if ((rc = rx_world_add_reaction(&c->w, &d, &c->rx_cand[k])) != RX_OK) return rc;
    }
    RxReactionDesc v;
    memset(&v, 0, sizeof v);
    v.name = "compose.verify";
    v.faculty = RX_FACULTY_AEGIS;
    v.subject = RXC_SUBJ_AEGIS;
    v.priority = RX_PRIO_FOREGROUND;
    v.triggers[v.n_triggers++] = (RxDep){ c->cand[0], RX_ALL_FIELDS };
    v.triggers[v.n_triggers++] = (RxDep){ c->cand[1], RX_ALL_FIELDS };
    v.reads[v.n_reads++] = (RxDep){ c->goal, RX_ALL_FIELDS };
    v.writes[v.n_writes++] = (RxDep){ c->verdict, RX_ALL_FIELDS };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[0], RXC_RES_CAND0, RX_RIGHT_READ };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[1], RXC_RES_CAND1, RX_RIGHT_READ };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[2], RXC_RES_GOAL, RX_RIGHT_READ };
    v.caps[v.n_caps++] = (RxCapNeed){ c->cap_verify[3], RXC_RES_VERDICT, RX_RIGHT_WRITE };
    v.fn = verify_fn;
    v.user = c;
    if ((rc = rx_world_add_reaction(&c->w, &v, &c->rx_verify)) != RX_OK) return rc;

    RxReactionDesc m;
    memset(&m, 0, sizeof m);
    m.name = "compose.commit";
    m.faculty = RX_FACULTY_OMEGA;
    m.subject = RXC_SUBJ_COMMIT;
    m.priority = RX_PRIO_FOREGROUND;
    m.triggers[m.n_triggers++] = (RxDep){ c->verdict, RX_ALL_FIELDS };
    m.writes[m.n_writes++] = (RxDep){ c->state, RX_ALL_FIELDS };
    m.caps[m.n_caps++] = (RxCapNeed){ c->cap_commit[0], RXC_RES_VERDICT, RX_RIGHT_READ };
    m.caps[m.n_caps++] = (RxCapNeed){ c->cap_commit[1], RXC_RES_STATE, RX_RIGHT_WRITE };
    m.fn = commit_fn;
    m.user = c;
    if ((rc = rx_world_add_reaction(&c->w, &m, &c->rx_commit)) != RX_OK) return rc;

    if ((rc = rx_world_set_binder(&c->w, bind_check, bind_fn, bind_abort, c)) != RX_OK) return rc;
    if ((rc = rx_world_bind_field(&c->w, c->state, RXC_S_REF)) != RX_OK) return rc;
    if ((rc = rx_cortex_attach(&c->w, &c->cx, c->session)) != RX_OK) return rc;
    c->attached = 1;
    return RX_OK;
}

/* ---- public API ------------------------------------------------------------ */

int rx_compose_open(RxCompose *c, const char *dir, const AienMachineId *self, uint64_t session,
                    const SrRouter *router, RxcContract contract, AienosCapAdmin *admin,
                    AienosCapView *view, uint32_t n_workers) {
    if (!c || !dir || !self || !admin || !view || strlen(dir) >= sizeof c->dir - 32)
        return RX_ERR_ARG;
    /* Keep only the caller's test settings across the reset. */
    int fp = c->fault_point, fc = c->fault_crash, rogue = c->test_rogue_candidate;
    uint32_t fk = c->fault_k;
    memset(c, 0, sizeof *c);
    c->fault_point = fp; c->fault_crash = fc; c->fault_k = fk; c->test_rogue_candidate = rogue;
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
    if (rc == RX_OK) rc = build_world(c, view, n_workers ? n_workers : 1);
    if (rc != RX_OK) {
        if (c->w.n_workers) rx_world_destroy(&c->w);
        js_space_destroy(&c->js);
        cx_close(&c->cx);
        return rc;
    }
    return RX_OK;
}

JsBranchRef rx_compose_state(RxCompose *c) {
    RxObject o;
    if (rx_world_read(&c->w, c->state, &o) != RX_OK) return (JsBranchRef){ UINT32_MAX, 0 };
    return ref_unpack(o.field[RXC_S_REF]);
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
    uint64_t sk[RXC_K] = { 0, 0 };
    for (uint32_t k = 0; k < (uint32_t)n && k < RXC_K; k++)
        sk[k] = ((uint64_t)routes[k].skill_version << 32) | routes[k].chosen.skill_id;
    RxMutation g[5] = {
        { c->goal, RXC_G_INPUT, input }, { c->goal, RXC_G_OP, req->need.semantic_operation },
        { c->goal, RXC_G_SKILL0, sk[0] }, { c->goal, RXC_G_SKILL1, sk[1] },
        { c->goal, RXC_G_SEQ, seq } };
    int64_t crumb = rx_world_publish_external(&c->w, c->cap_ext, g, 5);
    if (crumb < 0) return (int)crumb;
    out->goal_crumb = (uint64_t)crumb;
    int rc = rx_world_wait_quiescent(&c->w, 30000);
    if (rc != RX_OK) return rc;

    /* settle */
    RxObject so, vo, co[RXC_K];
    if (rx_world_read(&c->w, c->state, &so) != RX_OK || rx_world_read(&c->w, c->verdict, &vo) ||
        rx_world_read(&c->w, c->cand[0], &co[0]) || rx_world_read(&c->w, c->cand[1], &co[1]))
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
        uint64_t S = rx_cortex_record_of(&c->w, rx_world_explain(&c->w, c->state, RXC_S_REF));
        uint64_t V = rx_cortex_record_of(&c->w, rx_world_explain(&c->w, c->verdict, RXC_V_GOAL));
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
        uint64_t V = rx_cortex_record_of(&c->w, rx_world_explain(&c->w, c->verdict, RXC_V_GOAL));
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
    if (!c) return;
    rx_world_destroy(&c->w);     /* detaches the recorder */
    c->attached = 0;
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
