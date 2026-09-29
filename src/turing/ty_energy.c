/* TURING Yield TY-4/TY-5 records and checks. See ty_energy.h. */
#include "turing/ty_energy.h"

#include "omega_canonical.h"
#include "sha256.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TY_CANON_CAP 40000
__extension__ typedef unsigned __int128 ty_energy_u128;

static const char *const k_conf[TY_CONF__COUNT] = {"DIRECT", "COUNTER_DERIVED", "CAUSAL_RUNTIME",
                                                   "COUNTERFACTUAL", "MODELLED", "UNATTRIBUTED"};
static const uint32_t k_forge[TY_CONF__COUNT] = {
    TY_FORGE_ENERGY_MEASURED,  TY_FORGE_ENERGY_MEASURED,  TY_FORGE_ENERGY_ESTIMATED,
    TY_FORGE_ENERGY_ESTIMATED, TY_FORGE_ENERGY_ESTIMATED, TY_FORGE_ENERGY_NOT_MEASURED};
static const char *const k_status[TYE_STATUS__COUNT] = {
    "OK", "SENSOR_UNAVAILABLE", "COUNTER_WRAP", "WRAP_HEADROOM", "SAMPLING_GAP", "PARTIAL_RUN",
    "INTERVAL_MISMATCH", "PMU_MULTIPLEXED", "OVERLAPPING_INTERVALS", "MISSING_IDLE_BASELINE", "DOUBLE_COUNT",
    "ALLOCATION_EXCEEDS_TOTAL", "NEGATIVE_SHARE", "MISSING_PARTICIPANT", "UNKNOWN_PARTICIPANT",
    "CONDITION_MISMATCH", "BAD_ARGUMENT"};

const char *ty_conf_name(ty_conf c) { return (unsigned)c < TY_CONF__COUNT ? k_conf[c] : "INVALID"; }
uint32_t ty_conf_forge(ty_conf c) { return (unsigned)c < TY_CONF__COUNT ? k_forge[c] : TY_FORGE_ENERGY_NOT_MEASURED; }
const char *ty_energy_status_name(ty_energy_status s) { return (unsigned)s < TYE_STATUS__COUNT ? k_status[s] : "INVALID"; }

void ty_energy_hex(const turing_digest *d, char out[65]) {
    for (int i = 0; i < TURING_DIGEST_BYTES; i++) snprintf(out + 2 * i, 3, "%02x", d->b[i]);
}

/* ---- window validity ---- */

static int text_ok(const char *text, size_t size) {
    const char *end = memchr(text, 0, size);
    return end && end != text && !memchr(text, '|', (size_t)(end - text));
}

static int interval_shape_ok(const ty_interval *w) {
    if (!w || w->npart > TY_MAX_PART || !text_ok(w->run_id, sizeof w->run_id) ||
        !text_ok(w->config, sizeof w->config) || !text_ok(w->cond, sizeof w->cond)) return 0;
    for (size_t i = 0; i < w->npart; i++) {
        const ty_participant *p = &w->part[i];
        if (p->tag[0] < 'A' || p->tag[0] > 'D' || p->tag[1] != 0 ||
            !text_ok(p->rz, sizeof p->rz) || !text_ok(p->cpus, sizeof p->cpus)) return 0;
        for (size_t j = 0; j < i; j++) if (p->tag[0] == w->part[j].tag[0]) return 0;
    }
    return 1;
}

static int has_tag(const ty_interval *w, const char *tag) {
    for (size_t i = 0; i < w->npart; i++)
        if (!strcmp(w->part[i].tag, tag)) return 1;
    return 0;
}

static ty_energy_status check_condition(const ty_interval *w) {
    if (!strcmp(w->cond, "IDLE")) return w->npart == 0 ? TYE_OK : TYE_E_CONDITION;
    size_t want = strlen(w->cond);
    if (want != w->npart) return TYE_E_CONDITION;
    for (size_t i = 0; i < want; i++) {
        if (w->cond[i] < 'A' || w->cond[i] > 'D') return TYE_E_CONDITION;
        for (size_t j = 0; j < i; j++) if (w->cond[i] == w->cond[j]) return TYE_E_CONDITION;
        char t[2] = {w->cond[i], 0};
        if (!has_tag(w, t)) return TYE_E_CONDITION;
    }
    return TYE_OK;
}

ty_energy_status ty_interval_check(const ty_interval *w) {
    if (!interval_shape_ok(w) || w->planned_ns == 0 || w->t1_ns <= w->t0_ns) return TYE_E_ARG;
    if (!w->spbm_ok0 || !w->spbm_ok1) return TYE_E_SENSOR;
    if (w->overflow) return TYE_E_WRAP;
    for (int c = 0; c < TY_NCH; c++)
        if (w->e1_uj[c] < w->e0_uj[c]) return TYE_E_WRAP;
    for (int c = 0; c < TY_NCH; c++)
        if (w->e1_uj[c] >= TY_WRAP_UJ || TY_WRAP_UJ - w->e1_uj[c] <= w->e1_uj[c] - w->e0_uj[c]) return TYE_E_HEADROOM;
    uint64_t dt = w->t1_ns - w->t0_ns;
    if (w->max_gap_ns > TY_MAX_GAP_NS ||
        (ty_energy_u128)w->n_samples * 10u * TY_SAMPLE_PERIOD_NS < (ty_energy_u128)dt * 9u ||
        w->n_samples > dt / TY_SAMPLE_PERIOD_NS + 2u) return TYE_E_SAMPLING_GAP;
    ty_energy_status c = check_condition(w);
    if (c != TYE_OK) return c;
    if (dt < w->planned_ns || dt - w->planned_ns > w->planned_ns / 20u) return TYE_E_INTERVAL_MISMATCH;
    for (size_t i = 0; i < w->npart; i++) {
        const ty_participant *p = &w->part[i];
        if (!p->exited_ok || !p->oracle_ok || !p->pinned || p->calls == 0) return TYE_E_PARTIAL_RUN;
        if (p->planned_ns != w->planned_ns) return TYE_E_INTERVAL_MISMATCH;
        if (p->go_ns < w->t0_ns || p->t0_ns < p->go_ns || p->t1_ns < p->t0_ns || p->done_ns < p->t1_ns ||
            p->done_ns > w->t1_ns)
            return TYE_E_INTERVAL_MISMATCH;
        if ((ty_energy_u128)(p->t1_ns - p->t0_ns) * 50u < (ty_energy_u128)w->planned_ns * 49u) return TYE_E_PARTIAL_RUN;
        if (!p->pmu_ok) return TYE_E_SENSOR;
        if (p->multiplexed) return TYE_E_MULTIPLEXED;
    }
    return TYE_OK;
}

ty_energy_status ty_interval_energy(const ty_interval *w, int ch, int64_t *uj) {
    if (!uj || ch < 0 || ch >= TY_NCH) return TYE_E_ARG;
    ty_energy_status s = ty_interval_check(w);
    if (s != TYE_OK) return s;
    uint64_t de = w->e1_uj[ch] - w->e0_uj[ch], dt_us = (w->t1_ns - w->t0_ns) / 1000u;
    if (dt_us == 0 || de > 1000000000000ull) return TYE_E_ARG; /* > 1 MJ in one window: not physical here */
    ty_energy_u128 scaled = (ty_energy_u128)de * (w->planned_ns / 1000u) / dt_us;
    if (scaled > INT64_MAX) return TYE_E_ARG;
    *uj = (int64_t)scaled;
    return TYE_OK;
}

ty_energy_status ty_intervals_disjoint(const ty_interval *const *w, size_t n, size_t *a, size_t *b) {
    if (n && !w) return TYE_E_ARG;
    for (size_t i = 0; i < n; i++) if (!w[i] || w[i]->t1_ns <= w[i]->t0_ns) return TYE_E_ARG;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (w[i]->t0_ns < w[j]->t1_ns && w[j]->t0_ns < w[i]->t1_ns) {
                if (a) *a = i;
                if (b) *b = j;
                return TYE_E_OVERLAP;
            }
    return TYE_OK;
}

ty_energy_status ty_interaction(const ty_interval *idle, const ty_interval *a, const ty_interval *b,
                         const ty_interval *ab, int ch, int64_t *I) {
    if (!a || !b || !ab || !I) return TYE_E_ARG;
    if (!idle) return TYE_E_MISSING_IDLE;
    const ty_interval *w[4] = {idle, a, b, ab};
    static const char *const want[4] = {"IDLE", "A", "B", "AB"};
    int64_t e[4];
    for (int k = 0; k < 4; k++) {
        ty_energy_status s = ty_interval_energy(w[k], ch, &e[k]);
        if (s != TYE_OK) return s;
        if (strcmp(w[k]->cond, want[k]) || w[k]->round != ab->round || strcmp(w[k]->config, ab->config) ||
            strcmp(w[k]->run_id, ab->run_id) || w[k]->planned_ns != ab->planned_ns)
            return k == 0 && strcmp(w[k]->cond, "IDLE") ? TYE_E_MISSING_IDLE : TYE_E_CONDITION;
    }
    ty_energy_status s = ty_intervals_disjoint(w, 4, NULL, NULL);
    if (s != TYE_OK) return s;
    *I = e[3] - e[1] - e[2] + e[0];
    return TYE_OK;
}

/* ---- attribution ---- */

ty_energy_status ty_attribution_check(ty_attribution *at, const ty_interval *w) {
    if (at) at->verdict = TYE_E_ARG;
    if (!at || !w || at->nshare > TY_MAX_SHARE || at->nsrc > TY_MAX_SRC || at->ch < 0 || at->ch >= TY_NCH)
        return TYE_E_ARG;
    if (!text_ok(at->method, sizeof at->method) || at->tolerance_uj > INT64_MAX) return TYE_E_ARG;
    for (size_t i = 0; i < at->nshare; i++)
        if (!text_ok(at->share[i].who, sizeof at->share[i].who)) return TYE_E_ARG;
    at->allocated_uj = at->remainder_uj = at->measured_uj = 0;
    at->overall = TY_UNATTRIBUTED;
    ty_energy_status s = ty_interval_energy(w, at->ch, &at->measured_uj);
    if (s == TYE_OK && ty_interval_digest(w, &at->measured) != 0) s = TYE_E_ARG;
    if (s != TYE_OK) return at->verdict = s;
    int64_t tol = (int64_t)at->tolerance_uj;
    for (size_t i = 0; i < at->nsrc; i++) {
        if (!memcmp(at->src[i].b, at->measured.b, TURING_DIGEST_BYTES)) return at->verdict = TYE_E_DOUBLE_COUNT;
        for (size_t j = i + 1; j < at->nsrc; j++)
            if (!memcmp(at->src[i].b, at->src[j].b, TURING_DIGEST_BYTES)) return at->verdict = TYE_E_DOUBLE_COUNT;
    }
    if (at->all_unattributed) {
        if (at->nshare) return at->verdict = TYE_E_ARG;
        at->remainder_uj = at->measured_uj;
        return at->verdict = TYE_OK;
    }
    int idle = 0;
    ty_conf weakest = TY_DIRECT;
    for (size_t i = 0; i < at->nshare; i++) {
        const ty_share *sh = &at->share[i];
        for (size_t j = i + 1; j < at->nshare; j++)
            if (!strcmp(sh->who, at->share[j].who)) return at->verdict = TYE_E_DOUBLE_COUNT;
        if ((unsigned)sh->conf >= TY_UNATTRIBUTED) return at->verdict = TYE_E_ARG; /* the remainder is the only UNATTRIBUTED */
        if (!strcmp(sh->who, TY_WHO_IDLE)) idle = 1;
        else if (!has_tag(w, sh->who)) return at->verdict = TYE_E_UNKNOWN_PARTICIPANT;
        if (sh->uj < -tol) return at->verdict = TYE_E_NEGATIVE_SHARE;
        if (sh->conf > weakest) weakest = sh->conf;
        if (__builtin_add_overflow(at->allocated_uj, sh->uj, &at->allocated_uj)) return at->verdict = TYE_E_ARG;
    }
    if (!idle) return at->verdict = TYE_E_MISSING_IDLE;
    for (size_t i = 0; i < w->npart; i++) {
        int found = 0;
        for (size_t j = 0; j < at->nshare; j++) found |= !strcmp(at->share[j].who, w->part[i].tag);
        if (!found) return at->verdict = TYE_E_MISSING_PARTICIPANT;
    }
    if (__builtin_sub_overflow(at->measured_uj, at->allocated_uj, &at->remainder_uj)) return at->verdict = TYE_E_ARG;
    at->overall = weakest;
    if (at->remainder_uj < -tol) return at->verdict = TYE_E_OVER_ALLOCATED;
    return at->verdict = TYE_OK;
}

/* ---- canonical records (field.c convention, own builder) ---- */

typedef struct {
    OmegaObject *o;
    int err;
} tyb;

static void b_text(tyb *b, const char *key, const char *val) {
    if (b->err) return;
    size_t kl = strlen(key), vl = strlen(val);
    if (b->o->attr_count >= OMEGA_MAX_ATTRIBUTES || kl >= OMEGA_MAX_KEY_LEN || vl > OMEGA_MAX_VAL_LEN) {
        b->err = 1;
        return;
    }
    OmegaAttribute *a = &b->o->attributes[b->o->attr_count++];
    memcpy(a->key, key, kl + 1);
    memcpy(a->value, val, vl);
    a->val_len = (uint16_t)vl;
}
static void b_u64(tyb *b, const char *key, uint64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%" PRIu64, v);
    b_text(b, key, t);
}
static void b_i64(tyb *b, const char *key, int64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%" PRId64, v);
    b_text(b, key, t);
}
static void b_rel(tyb *b, uint16_t kind, const turing_digest *d) {
    if (b->err) return;
    if (b->o->rel_count >= OMEGA_MAX_RELATIONS) {
        b->err = 1;
        return;
    }
    OmegaRelation *r = &b->o->relations[b->o->rel_count++];
    r->kind = kind;
    memcpy(r->target_id.bytes, d->b, TURING_DIGEST_BYTES);
}
static int b_finish(tyb *b, const char *domain, turing_digest *out) {
    int rc = -1;
    size_t n = 0;
    uint8_t *buf = malloc(TY_CANON_CAP);
    if (buf && !b->err && omega_canonical_encode(b->o, buf, TY_CANON_CAP, &n) == 0) {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)domain, strlen(domain) + 1);
        sha256_update(&c, buf, n);
        sha256_final(&c, out->b);
        rc = 0;
    }
    free(buf);
    free(b->o);
    b->o = NULL;
    return rc;
}
static int b_new(tyb *b, const char *record) {
    b->err = 0;
    b->o = calloc(1, sizeof(OmegaObject));
    if (!b->o) return -1;
    b->o->kind = KIND_EVIDENCE;
    b_text(b, "record", record);
    return 0;
}

int ty_interval_digest(const ty_interval *w, turing_digest *out) {
    tyb b;
    if (!interval_shape_ok(w) || !out || b_new(&b, "turing.resource_interval") != 0) return -1;
    char t[512];
    b_text(&b, "run", w->run_id);
    b_text(&b, "config", w->config);
    b_text(&b, "cond", w->cond);
    b_u64(&b, "round", w->round);
    b_u64(&b, "pos", w->pos);
    b_u64(&b, "t0_ns", w->t0_ns);
    b_u64(&b, "t1_ns", w->t1_ns);
    b_u64(&b, "planned_ns", w->planned_ns);
    snprintf(t, sizeof t, "%d,%d,%d", w->spbm_ok0, w->spbm_ok1, w->overflow);
    b_text(&b, "sensor_ok0_ok1_overflow", t);
    snprintf(t, sizeof t, "%" PRIu64 ",%" PRIu64 ",%" PRIu64, w->e0_uj[0], w->e0_uj[1], w->e0_uj[2]);
    b_text(&b, "e0_uj_pkg_cpue_cpup", t);
    snprintf(t, sizeof t, "%" PRIu64 ",%" PRIu64 ",%" PRIu64, w->e1_uj[0], w->e1_uj[1], w->e1_uj[2]);
    b_text(&b, "e1_uj_pkg_cpue_cpup", t);
    b_u64(&b, "n_samples", w->n_samples);
    b_u64(&b, "max_gap_ns", w->max_gap_ns);
    b_u64(&b, "resolution_uj", TY_RESOLUTION_UJ);
    b_u64(&b, "sample_period_ns", TY_SAMPLE_PERIOD_NS);
    b_u64(&b, "foreign_busy_ms", w->foreign_busy_ms);
    b_u64(&b, "max_temp_mc", w->max_temp_mc);
    for (size_t i = 0; i < w->npart; i++) {
        const ty_participant *p = &w->part[i];
        char key[8] = "part0";
        key[4] = (char)('0' + i);
        snprintf(t, sizeof t,
                 "%s|%s|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64
                 "|%" PRIu64 "|%" PRIu64 "|%d|%d|%d|%d|%d",
                 p->tag, p->rz, p->cpus, p->go_ns, p->t0_ns, p->t1_ns, p->done_ns, p->planned_ns, p->calls,
                 p->cycles, p->inst, p->l2refill, p->pinned, p->oracle_ok, p->exited_ok, p->pmu_ok, p->multiplexed);
        b_text(&b, key, t);
    }
    ty_energy_status status = ty_interval_check(w);
    ty_conf confidence = status == TYE_OK ? TY_DIRECT : TY_UNATTRIBUTED;
    b_text(&b, "status", ty_energy_status_name(status));
    b_text(&b, "confidence", ty_conf_name(confidence));
    b_u64(&b, "forge_energy_source", ty_conf_forge(confidence));
    return b_finish(&b, TY_DOMAIN_INTERVAL, out);
}

int ty_attribution_digest(const ty_attribution *at, turing_digest *out) {
    tyb b;
    if (!at || !text_ok(at->method, sizeof at->method) || at->nshare > TY_MAX_SHARE) return -1;
    for (size_t i = 0; i < at->nshare; i++)
        if (!text_ok(at->share[i].who, sizeof at->share[i].who)) return -1;
    if (!at || !out || at->nshare > TY_MAX_SHARE || at->nsrc > TY_MAX_SRC ||
        b_new(&b, "turing.resource_attribution") != 0)
        return -1;
    static const char *const ch[TY_NCH] = {"pkg", "cpu_e", "cpu_p"};
    char t[512] = "";
    size_t k = 0;
    b_text(&b, "method", at->method);
    b_text(&b, "channel", at->ch >= 0 && at->ch < TY_NCH ? ch[at->ch] : "invalid");
    b_u64(&b, "tolerance_uj", at->tolerance_uj);
    b_u64(&b, "all_unattributed", (uint64_t)(at->all_unattributed != 0));
    b_i64(&b, "measured_uj", at->measured_uj);
    b_i64(&b, "allocated_uj", at->allocated_uj);
    b_i64(&b, "unattributed_uj", at->remainder_uj);
    for (size_t i = 0; i < at->nshare && k < sizeof t; i++)
        k += (size_t)snprintf(t + k, sizeof t - k, "%s%s:%s:%" PRId64 ":%u", i ? ";" : "", at->share[i].who,
                              ty_conf_name(at->share[i].conf), at->share[i].uj, ty_conf_forge(at->share[i].conf));
    b_text(&b, "shares_who_conf_uj_forge", t);
    b_text(&b, "verdict", ty_energy_status_name(at->verdict));
    ty_conf confidence = at->all_unattributed || at->verdict != TYE_OK ? TY_UNATTRIBUTED : at->overall;
    b_text(&b, "overall_confidence", ty_conf_name(confidence));
    b_u64(&b, "overall_forge_energy_source",
          ty_conf_forge(confidence));
    b_rel(&b, REL_DERIVED_FROM, &at->measured);
    for (size_t i = 0; i < at->nsrc; i++) b_rel(&b, REL_DEPENDS_ON, &at->src[i]);
    return b_finish(&b, TY_DOMAIN_ATTRIBUTION, out);
}
