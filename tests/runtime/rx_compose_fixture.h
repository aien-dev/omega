/*
 * rx_compose_fixture.h -- shared setup for the COMPOSITION-2 tests and gate:
 * a canonical machine, a Capability Graph with two digest-pinned local
 * Skills for one operation, the contract the AEGIS verifier enforces, and
 * open/close of a composition on a fresh AIENOS authority.
 */
#ifndef RX_COMPOSE_FIXTURE_H
#define RX_COMPOSE_FIXTURE_H

#include "runtime/aien_machine_id.h"
#include "runtime/aienos_cap.h"
#include "runtime/rx_capq.h"
#include "runtime/rx_compose.h"
#include "runtime/rx_cortex_record.h"
#include "runtime/rx_graph.h"
#include "runtime/rx_skillroute.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FX_NOW_US 1000000ull
enum { FX_OP_SCALE = 1 };
enum { FX_SKILL_A = 7, FX_SKILL_B = 8 };    /* A is cheaper: ranked first */
#define FX_SESSION 0x5E55ull

/* Contract: result = 3 * input + 1. */
static __attribute__((unused)) int fx_contract(uint64_t input, uint64_t result) { return result == input * 3 + 1; }

static int fx_a_bad, fx_b_bad;   /* make a Skill break the contract */

static __attribute__((unused)) uint64_t fx_sk_a(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) * 3 + 1 + (fx_a_bad ? 1 : 0);
}
static __attribute__((unused)) uint64_t fx_sk_b(const uint64_t *in, uint32_t n, uint32_t attempt, int *failed) {
    (void)attempt;
    *failed = 0;
    return (n ? in[0] : 0) * 3 + 1 + (fx_b_bad ? 5 : 0);
}

static __attribute__((unused)) AienMachineId fx_mid(uint8_t seed) {
    AienMachineId m;
    uint8_t root[8] = { 'c', 'o', 'm', 'p', seed, 0, 0, 0 };
    aien_mid_derive(AIEN_MID_ROOT_PROVISIONED, root, sizeof root, &m);
    return m;
}

static __attribute__((unused)) CqEntry fx_provide(uint32_t cap, uint32_t op, uint32_t real, uint64_t cost) {
    CqEntry e;
    memset(&e, 0, sizeof e);
    e.capability_id = cap;
    e.realization_id = real;
    e.op = op;
    e.effects = CQ_FX_PURE;
    e.in_types = 0x1;
    e.out_types = 0x2;
    e.confidence_ppm = 900000;
    e.reliability_ppm = 990000;
    e.evidence_level = CQ_EV_MEASURED;
    e.evidence_ref = 0xE000u + cap;
    e.cost = cost;
    e.latency_us = 100;
    e.energy_uj = 100;
    e.live = CQ_LIVE_AVAILABLE;
    e.generation = 1;
    return e;
}

static __attribute__((unused)) SrRequirement fx_requirement(void) {
    SrRequirement q;
    memset(&q, 0, sizeof q);
    q.need.semantic_operation = FX_OP_SCALE;
    q.need.accepted_input_types = ~0ull;
    q.need.effect_class = 0x1FF;
    q.need.authority_ceiling.resource_hi = UINT64_MAX;
    q.need.authority_ceiling.rights = RX_RIGHT_READ | RX_RIGHT_WRITE | RX_RIGHT_EFFECT;
    q.t.n_order = 1;
    q.t.order[0] = CQ_DIM_COST;
    q.t.k = CQ_MAX_K;
    q.local_only = 1;
    return q;
}

typedef struct {
    AienMachineId self;
    AienMachineId slots[8];
    AienMachineIndex ix;
    CqCatalog cat;
    AgSkillTable skills;
    SrRouter router;
    AienosCapAdmin *admin;
    AienosCapView *view;
} Fx;

/* Graph: Skill A (cost 10) and Skill B (cost 40), both local, both pinned
 * to the digest of the procedure this machine runs. Returns 0 on success. */
static __attribute__((unused)) int fx_init(Fx *f) {
    memset(f, 0, sizeof *f);
    f->self = fx_mid(1);
    aien_mid_index_init(&f->ix, f->slots, 8);
    if (cq_catalog_init_canonical(&f->cat, &f->ix, &f->self, 8, 8) != CQ_OK) return -1;
    uint32_t all = CQ_SRC(CQ_SRC_GRAPH) | CQ_SRC(CQ_SRC_SKILL) | CQ_SRC(CQ_SRC_FABRIC);
    if (cq_op_define(&f->cat, FX_OP_SCALE, 0, all) != CQ_OK) return -2;
    f->skills.n = 2;
    f->skills.skill[0] = (AgSkill){ FX_SKILL_A, fx_sk_a, { 0xA7, 0x01 } };
    f->skills.skill[1] = (AgSkill){ FX_SKILL_B, fx_sk_b, { 0xB8, 0x02 } };
    for (uint32_t i = 0; i < 2; i++) {
        SrSkill s;
        memset(&s, 0, sizeof s);
        s.skill_id = f->skills.skill[i].id;
        s.version = 1;
        memcpy(s.digest, f->skills.skill[i].identity, 32);
        s.machine = f->cat.self_machine;
        CqEntry e = fx_provide(1 + i, FX_OP_SCALE, 70 + i, i ? 40 : 10);
        if (sr_register_skill(&f->cat, &s, &e, 1) != SR_OK) return -3;
    }
    if (cq_catalog_build(&f->cat) != CQ_OK) return -4;
    f->router = (SrRouter){ &f->cat, &f->skills };
    return 0;
}

static __attribute__((unused)) void fx_free(Fx *f) { cq_catalog_free(&f->cat); }

static __attribute__((unused)) int fx_open(Fx *f, RxCompose *c, const char *dir, uint32_t n_workers) {
    if (aienos_cap_start(&f->admin, &f->view) != 0) return -100;
    int rc = rx_compose_open(c, dir, &f->self, FX_SESSION, &f->router, fx_contract, f->admin,
                             f->view, n_workers);
    if (rc != RX_OK) {
        aienos_cap_stop(f->admin, f->view);
        f->admin = NULL;
    }
    return rc;
}

static __attribute__((unused)) void fx_close(Fx *f, RxCompose *c) {
    rx_compose_close(c);
    if (f->admin) aienos_cap_stop(f->admin, f->view);
    f->admin = NULL;
}

static __attribute__((unused)) int fx_run(Fx *f, RxCompose *c, uint64_t input, RxcResult *out) {
    (void)f;
    SrRequirement q = fx_requirement();
    return rx_compose_run(c, input, &q, NULL, FX_NOW_US, out);
}

static __attribute__((unused)) uint32_t fx_live_branches(JsSpace *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->n_branches; i++) n += s->branches[i] != NULL;
    return n;
}

/* Count objects of `kind` (and tag when tag != UINT64_MAX). */
static __attribute__((unused)) uint32_t fx_count(const CxStore *s, uint32_t kind, uint64_t tag) {
    uint32_t n = 0;
    for (uint64_t id = 1; id <= s->n; id++) {
        const CxObject *o = cx_get(s, id);
        if (o && o->kind == kind && (tag == UINT64_MAX || o->tag == tag)) n++;
    }
    return n;
}

static __attribute__((unused)) uint64_t fx_pack(JsBranchRef r) { return js_branch_ref_pack(r); }

static __attribute__((unused)) void fx_hex(const uint8_t *d, size_t n, char *out) {
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[2 * n] = 0;
}

#endif /* RX_COMPOSE_FIXTURE_H */
