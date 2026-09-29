/*
 * rx_route.c -- Omega cognitive compute routing. See rx_route.h and
 * spec/cognitive-routing.md.
 */
#include "rx_route.h"

#include "sha256.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MODEL_MAGIC   0x4d52434fu /* "OCRM" */
#define MODEL_VERSION 1u
#define MODEL_MAX     65536u
#define PLAN_CACHE    512u
#define PLAN_PROBES   4u
#define NS_KEEP       4096u

typedef struct {
    RxCogDeclaration decl;
    RxCogEngineFn fn;
    void *ctx;
    uint32_t reference_ops;
} Engine;

/* Evidence sample for one operation: every record holds, for each engine in
 * eng[], one byte: confidence bin (low 4 bits) and correct (bit 7). */
typedef struct {
    uint32_t n_records;
    uint32_t n_eng;
    uint32_t eng[RX_COG_MAX_ENGINES]; /* engine indices */
    uint8_t *rec;
} Sample;

typedef struct {
    int used;
    uint64_t stamp;
    uint64_t generation;
    RxCogRequirement req;
    int rc;
    RxCogLadder ladder;
} PlanEntry;

struct RxCogRouter {
    Engine eng[RX_COG_MAX_ENGINES];
    uint32_t n;
    int sealed;
    uint64_t generation;
    int have_profile;
    RxCogRealization real[RX_COG_MAX_ENGINES];
    Sample sample[RX_COG_OP_COUNT];
    PlanEntry cache[PLAN_CACHE];
    uint32_t cache_next;
};

/* ---- small helpers ---- */

static uint32_t conf_bin(uint32_t conf) {
    if (conf >= RX_COG_CONF_ONE) return RX_COG_BINS - 1;
    return (uint32_t)(((uint64_t)conf * RX_COG_BINS) / RX_COG_CONF_ONE);
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Wilson score interval at 95%. */
static double wilson_lower(uint64_t k, uint64_t n) {
    if (n == 0) return 0.0;
    const double z = 1.959964;
    double p = (double)k / (double)n, nn = (double)n;
    double c = p + z * z / (2 * nn);
    double r = z * sqrt(p * (1 - p) / nn + z * z / (4 * nn * nn));
    return (c - r) / (1 + z * z / nn);
}

static double wilson_upper(uint64_t k, uint64_t n) {
    if (n == 0) return 1.0;
    return 1.0 - wilson_lower(n - k, n);
}

static int index_of(const RxCogRouter *r, uint32_t id) {
    for (uint32_t i = 0; i < r->n; i++)
        if (r->eng[i].decl.cognitive_engine_id == id) return (int)i;
    return -1;
}

static void put32(uint8_t **p, uint32_t v) {
    for (int i = 0; i < 4; i++) *(*p)++ = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) *(*p)++ = (uint8_t)(v >> (8 * i));
}

typedef struct {
    const uint8_t *p, *end;
    int bad;
} Reader;

static uint32_t get32(Reader *r) {
    if (r->end - r->p < 4) {
        r->bad = 1;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r->p[i] << (8 * i);
    r->p += 4;
    return v;
}
static uint64_t get64(Reader *r) {
    uint64_t lo = get32(r), hi = get32(r);
    return lo | hi << 32;
}

static void decl_bytes(const RxCogDeclaration *d, uint8_t out[36]) {
    uint8_t *p = out;
    put32(&p, d->cognitive_engine_id);
    put32(&p, d->realization_class);
    put32(&p, d->supported_operations);
    put32(&p, d->hardware_requirements);
    put64(&p, d->memory_capacity);
    put32(&p, d->temporal_horizon);
    put32(&p, d->max_planning_depth);
    put32(&p, d->precision | d->verifiable << 16);
}

static void registry_digest(const Engine *eng, uint32_t n, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    uint8_t b[36];
    for (uint32_t i = 0; i < n; i++) {
        decl_bytes(&eng[i].decl, b);
        sha256_update(&c, b, sizeof b);
        uint8_t ref[4] = {(uint8_t)eng[i].reference_ops, 0, 0, 0};
        sha256_update(&c, ref, 4);
    }
    sha256_final(&c, out);
}

/* ---- router ---- */

int rx_route_create(RxCogRouter **out) {
    if (!out) return RX_COG_ERR_ARG;
    *out = calloc(1, sizeof(RxCogRouter));
    return *out ? RX_COG_OK : RX_COG_ERR_FULL;
}

static void drop_profile(RxCogRouter *r) {
    for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) {
        free(r->sample[o].rec);
        memset(&r->sample[o], 0, sizeof r->sample[o]);
    }
    for (uint32_t i = 0; i < r->n; i++) {
        memset(&r->real[i], 0, sizeof r->real[i]);
        r->real[i].cognitive_engine_id = r->eng[i].decl.cognitive_engine_id;
        r->real[i].decl = r->eng[i].decl;
    }
    r->have_profile = 0;
}

void rx_route_destroy(RxCogRouter *r) {
    if (!r) return;
    drop_profile(r);
    free(r);
}

int rx_route_register(RxCogRouter *r, const RxCogDeclaration *decl, RxCogEngineFn fn, void *ctx,
                      uint32_t reference_ops) {
    if (!r || !decl || !fn || r->sealed) return RX_COG_ERR_ARG;
    if (r->n >= RX_COG_MAX_ENGINES) return RX_COG_ERR_FULL;
    if (index_of(r, decl->cognitive_engine_id) >= 0) return RX_COG_ERR_ARG;
    uint32_t all_ops = (1u << RX_COG_OP_COUNT) - 1;
    if (!decl->supported_operations || (decl->supported_operations & ~all_ops)) return RX_COG_ERR_ARG;
    if ((reference_ops & ~decl->supported_operations) != 0) return RX_COG_ERR_ARG;
    for (uint32_t i = 0; i < r->n; i++)
        if (r->eng[i].reference_ops & reference_ops) return RX_COG_ERR_ARG;
    Engine *e = &r->eng[r->n];
    e->decl = *decl;
    e->fn = fn;
    e->ctx = ctx;
    e->reference_ops = reference_ops;
    r->real[r->n].cognitive_engine_id = decl->cognitive_engine_id;
    r->real[r->n].decl = *decl;
    r->n++;
    return RX_COG_OK;
}

void rx_route_registry_digest(const RxCogRouter *r, uint8_t out[32]) {
    registry_digest(r->eng, r->n, out);
}

uint64_t rx_route_generation(const RxCogRouter *r) { return r ? r->generation : 0; }

int rx_route_realization(const RxCogRouter *r, uint32_t engine_id, RxCogRealization *out) {
    if (!r || !out) return RX_COG_ERR_ARG;
    int i = index_of(r, engine_id);
    if (i < 0) return RX_COG_ERR_ARG;
    *out = r->real[i];
    return RX_COG_OK;
}

/* Model layout (little endian):
 *   magic, version, registry digest[32], n engines
 *   per engine: id, then per op: quality (3 x u32), calibration (2 x 16 x u32),
 *               cost (calls, mean, p99, nj: u64; measured: u32)
 *   per op: n_records, n_eng, engine ids[n_eng], records[n_records * n_eng]
 * An empty model means "no evidence". */
static int parse_model(RxCogRouter *r, uint64_t gen, const uint8_t *buf, size_t len) {
    RxCogRealization real[RX_COG_MAX_ENGINES];
    Sample sample[RX_COG_OP_COUNT];
    memset(real, 0, sizeof real);
    memset(sample, 0, sizeof sample);
    Reader rd = {buf, buf + len, 0};
    if (get32(&rd) != MODEL_MAGIC || get32(&rd) != MODEL_VERSION) return RX_COG_ERR_MODEL;
    uint8_t want[32];
    registry_digest(r->eng, r->n, want);
    if (rd.end - rd.p < 32 || memcmp(rd.p, want, 32) != 0) return RX_COG_ERR_MODEL;
    rd.p += 32;
    if (get32(&rd) != r->n) return RX_COG_ERR_MODEL;
    for (uint32_t i = 0; i < r->n; i++) {
        RxCogRealization *x = &real[i];
        x->cognitive_engine_id = get32(&rd);
        if (x->cognitive_engine_id != r->eng[i].decl.cognitive_engine_id) return RX_COG_ERR_MODEL;
        x->decl = r->eng[i].decl;
        x->generation = gen;
        for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) {
            x->quality_profile[o].observations = get32(&rd);
            x->quality_profile[o].correct = get32(&rd);
            x->quality_profile[o].failures = get32(&rd);
            for (uint32_t b = 0; b < RX_COG_BINS; b++) x->uncertainty_profile[o].n[b] = get32(&rd);
            for (uint32_t b = 0; b < RX_COG_BINS; b++) x->uncertainty_profile[o].correct[b] = get32(&rd);
            x->resource_cost_model[o].calls = get64(&rd);
            x->resource_cost_model[o].mean_ns = get64(&rd);
            x->resource_cost_model[o].p99_ns = get64(&rd);
            x->resource_cost_model[o].nj_per_call = get64(&rd);
            x->resource_cost_model[o].energy_measured = get32(&rd);
            if (x->quality_profile[o].correct > x->quality_profile[o].observations) rd.bad = 1;
            if (!(x->decl.supported_operations >> o & 1) && x->quality_profile[o].observations) rd.bad = 1;
        }
    }
    const uint8_t *sample_start = rd.p;
    int rc = RX_COG_ERR_MODEL;
    for (uint32_t o = 0; o < RX_COG_OP_COUNT && !rd.bad; o++) {
        Sample *s = &sample[o];
        s->n_records = get32(&rd);
        s->n_eng = get32(&rd);
        if (s->n_records > RX_COG_MAX_SAMPLE || s->n_eng > r->n) goto out;
        for (uint32_t k = 0; k < s->n_eng; k++) {
            int ix = index_of(r, get32(&rd));
            if (ix < 0 || !(r->eng[ix].decl.supported_operations >> o & 1)) goto out;
            for (uint32_t j = 0; j < k; j++)
                if (s->eng[j] == (uint32_t)ix) goto out;
            s->eng[k] = (uint32_t)ix;
        }
        size_t bytes = (size_t)s->n_records * s->n_eng;
        if (rd.bad || (size_t)(rd.end - rd.p) < bytes) goto out;
        if (bytes) {
            s->rec = malloc(bytes);
            if (!s->rec) goto out;
            memcpy(s->rec, rd.p, bytes);
            for (size_t k = 0; k < bytes; k++)
                if (s->rec[k] & 0x70) goto out;
        }
        rd.p += bytes;
    }
    if (rd.bad || rd.p != rd.end) goto out;
    uint8_t ev[32];
    sha256_hash(sample_start, (size_t)(rd.end - sample_start), ev);
    drop_profile(r);
    for (uint32_t i = 0; i < r->n; i++) {
        memcpy(real[i].evidence, ev, 32);
        r->real[i] = real[i];
    }
    memcpy(r->sample, sample, sizeof sample);
    r->have_profile = 1;
    return RX_COG_OK;
out:
    for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) free(sample[o].rec);
    return rc;
}

int rx_route_load(RxCogRouter *r, const RxGenStore *store) {
    if (!r || !store) return RX_COG_ERR_ARG;
    r->sealed = 1;
    uint64_t id = 0, lineage = 0;
    if (rx_gen_active(store, &id, &lineage) != RX_GEN_OK) return RX_COG_ERR_MODEL;
    if (id == 0) {
        drop_profile(r);
        r->generation = 0;
        memset(r->cache, 0, sizeof r->cache);
        return RX_COG_OK;
    }
    uint8_t *buf = malloc(MODEL_MAX);
    if (!buf) return RX_COG_ERR_FULL;
    size_t len = 0;
    int grc = rx_gen_read_blob(store, id, "model", buf, MODEL_MAX, &len);
    int rc;
    if (grc != RX_GEN_OK) {
        rc = RX_COG_ERR_MODEL;
    } else if (len == 0) {
        drop_profile(r);
        rc = RX_COG_OK;
    } else {
        rc = parse_model(r, id, buf, len);
    }
    free(buf);
    if (rc == RX_COG_OK) {
        r->generation = id;
        for (uint32_t i = 0; i < r->n; i++) r->real[i].generation = id;
        memset(r->cache, 0, sizeof r->cache);
    }
    return rc;
}

/* ---- choosing ---- */

static int eligible(const RxCogDeclaration *d, const RxCogRequirement *q) {
    if (!(d->supported_operations >> q->operation_class & 1)) return 0;
    if (d->hardware_requirements & ~q->hardware_constraints) return 0;
    if (d->memory_capacity < q->memory_requirement) return 0;
    if (d->temporal_horizon < q->temporal_requirement) return 0;
    if (d->max_planning_depth < q->planning_depth) return 0;
    if (q->precision_requirement == RX_COG_PREC_EXACT && d->precision != RX_COG_PREC_EXACT) return 0;
    return 1;
}

static int sample_slot(const Sample *s, uint32_t ix) {
    for (uint32_t k = 0; k < s->n_eng; k++)
        if (s->eng[k] == ix) return (int)k;
    return -1;
}

/* Smallest bin from which answers may be accepted: the pooled answers at
 * and above it have a 95% upper error bound within the budget. RX_COG_BINS
 * when no bin qualifies.
 *
 * A verifiable engine's checked answer (top bin) is accepted on the check,
 * not on statistics: a sound check has no error to bound. The evidence can
 * still refute the claim: one checked answer observed wrong, or none observed
 * at all, and the engine's answers are never accepted early. */
static uint32_t accept_bin(const RxCogDeclaration *d, const RxCogCalibration *c, uint32_t budget_ppm) {
    if (d->verifiable) {
        uint32_t top = RX_COG_BINS - 1;
        return c->n[top] && c->correct[top] == c->n[top] ? top : RX_COG_BINS;
    }
    uint64_t n = 0, k = 0;
    uint32_t best = RX_COG_BINS;
    double budget = (double)budget_ppm / 1e6;
    for (int b = (int)RX_COG_BINS - 1; b >= 0; b--) {
        n += c->n[b];
        k += c->correct[b];
        if (n && wilson_upper(n - k, n) <= budget) best = (uint32_t)b;
    }
    return best;
}

static int use_energy(const RxCogRouter *r, const uint32_t *ix, uint32_t n, uint32_t op) {
    for (uint32_t i = 0; i < n; i++)
        if (!r->real[ix[i]].resource_cost_model[op].energy_measured) return 0;
    return 1;
}

/* Ladder over engine indices ix[0..n) (cost order), evaluated on the evidence sample. */
static void evaluate(const RxCogRouter *r, const RxCogRequirement *q, const uint32_t *ix,
                     uint32_t n, RxCogLadder *out) {
    uint32_t op = q->operation_class;
    const Sample *s = &r->sample[op];
    int slot[RX_COG_MAX_LADDER];
    memset(out, 0, sizeof *out);
    out->n = n;
    for (uint32_t i = 0; i < n; i++) {
        out->engine[i] = r->eng[ix[i]].decl.cognitive_engine_id;
        out->accept_min_bin[i] = accept_bin(&r->eng[ix[i]].decl, &r->real[ix[i]].uncertainty_profile[op],
                                            q->uncertainty_budget_ppm);
        slot[i] = sample_slot(s, ix[i]);
    }
    uint64_t correct = 0;
    double ns = 0, nj = 0;
    for (uint32_t k = 0; k < s->n_records; k++) {
        const uint8_t *rec = s->rec + (size_t)k * s->n_eng;
        for (uint32_t i = 0; i < n; i++) {
            const RxCogCost *c = &r->real[ix[i]].resource_cost_model[op];
            ns += (double)c->mean_ns;
            nj += (double)c->nj_per_call;
            uint8_t v = rec[slot[i]];
            if (i + 1 == n || (uint32_t)(v & 0x0f) >= out->accept_min_bin[i]) {
                correct += v >> 7;
                break;
            }
        }
    }
    uint64_t m = s->n_records ? s->n_records : 1;
    out->expected_ns = (uint64_t)(ns / (double)m);
    out->expected_nj = use_energy(r, ix, n, op) ? (uint64_t)(nj / (double)m) : 0;
    out->expected_quality_ppm = (uint32_t)(1e6 * (double)correct / (double)m);
    out->quality_lower_ppm = (uint32_t)(1e6 * wilson_lower(correct, s->n_records));
    out->reason = RX_COG_WHY_EVIDENCE;
}

static double ladder_cost(const RxCogLadder *l) {
    return l->expected_nj ? (double)l->expected_nj : (double)l->expected_ns * 1e-3;
}

static int engine_cost_less(const RxCogRouter *r, uint32_t op, uint32_t a, uint32_t b) {
    const RxCogCost *x = &r->real[a].resource_cost_model[op], *y = &r->real[b].resource_cost_model[op];
    if (x->energy_measured && y->energy_measured && x->nj_per_call != y->nj_per_call)
        return x->nj_per_call < y->nj_per_call;
    return x->mean_ns < y->mean_ns;
}

/* Eligible engines with enough evidence for the operation, cheapest first. */
static uint32_t candidates(const RxCogRouter *r, const RxCogRequirement *q, uint32_t *ix) {
    uint32_t n = 0, op = q->operation_class;
    if (!r->have_profile) return 0;
    for (uint32_t i = 0; i < r->n; i++) {
        if (!eligible(&r->eng[i].decl, q)) continue;
        if (r->real[i].quality_profile[op].observations < RX_COG_MIN_EVIDENCE) continue;
        if (!r->real[i].resource_cost_model[op].calls) continue;
        if (sample_slot(&r->sample[op], i) < 0) continue;
        ix[n++] = i;
    }
    for (uint32_t a = 1; a < n; a++)
        for (uint32_t b = a; b > 0 && engine_cost_less(r, op, ix[b], ix[b - 1]); b--) {
            uint32_t t = ix[b];
            ix[b] = ix[b - 1];
            ix[b - 1] = t;
        }
    return n;
}

static int reference_ladder(const RxCogRouter *r, const RxCogRequirement *q, RxCogLadder *out) {
    for (uint32_t i = 0; i < r->n; i++)
        if ((r->eng[i].reference_ops >> q->operation_class & 1) && eligible(&r->eng[i].decl, q)) {
            memset(out, 0, sizeof *out);
            out->n = 1;
            out->engine[0] = r->eng[i].decl.cognitive_engine_id;
            out->accept_min_bin[0] = 0;
            out->reason = RX_COG_WHY_NO_EVIDENCE;
            return RX_COG_OK;
        }
    return RX_COG_ERR_UNSATISFIABLE;
}

static RxCogRequirement normalize(const RxCogRequirement *q) {
    RxCogRequirement z;
    memset(&z, 0, sizeof z);
    z.operation_class = q->operation_class;
    z.minimum_quality_ppm = q->minimum_quality_ppm;
    z.uncertainty_budget_ppm = q->uncertainty_budget_ppm;
    z.memory_requirement = q->memory_requirement;
    z.temporal_requirement = q->temporal_requirement;
    z.planning_depth = q->planning_depth;
    z.precision_requirement = q->precision_requirement;
    z.latency_budget_ns = q->latency_budget_ns;
    z.energy_budget_nj = q->energy_budget_nj;
    z.hardware_constraints = q->hardware_constraints;
    z.escalation_allowed = q->escalation_allowed;
    return z;
}

static int choose(const RxCogRouter *r, const RxCogRequirement *q, RxCogLadder *out) {
    uint32_t ix[RX_COG_MAX_ENGINES];
    uint32_t n = candidates(r, q, ix);
    if (n == 0) {
        /* Without evidence only the reference may run, and only when the
         * operation has no evidence at all: evidence that disqualifies every
         * engine is a refusal, not a bootstrap. */
        if (r->have_profile && r->sample[q->operation_class].n_records) return RX_COG_ERR_UNSATISFIABLE;
        return reference_ladder(r, q, out);
    }
    uint32_t max_len = q->escalation_allowed + 1;
    if (max_len > RX_COG_MAX_LADDER) max_len = RX_COG_MAX_LADDER;
    int found = 0;
    RxCogLadder best, cur;
    memset(&best, 0, sizeof best);
    for (uint32_t mask = 1; mask < (1u << n); mask++) {
        uint32_t pick[RX_COG_MAX_LADDER], len = 0;
        if ((uint32_t)__builtin_popcount(mask) > max_len) continue;
        for (uint32_t i = 0; i < n; i++)
            if (mask >> i & 1) pick[len++] = ix[i];
        evaluate(r, q, pick, len, &cur);
        if (cur.quality_lower_ppm < q->minimum_quality_ppm) continue;
        if (q->latency_budget_ns && cur.expected_ns > q->latency_budget_ns) continue;
        if (q->energy_budget_nj && cur.expected_nj > q->energy_budget_nj) continue;
        if (q->energy_budget_nj && !cur.expected_nj) continue; /* budget cannot be checked */
        if (!found || ladder_cost(&cur) < ladder_cost(&best) ||
            (ladder_cost(&cur) == ladder_cost(&best) && cur.n < best.n)) {
            best = cur;
            found = 1;
        }
    }
    if (!found) return RX_COG_ERR_UNSATISFIABLE;
    *out = best;
    return RX_COG_OK;
}

/* The ladder for a requirement is fixed for a generation, so it is cached:
 * direct-mapped on a hash of the requirement with a few probes; the oldest
 * probed entry is replaced. */
static uint32_t req_hash(const RxCogRequirement *q) {
    const uint8_t *p = (const uint8_t *)q;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *q; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

int rx_route_plan(RxCogRouter *r, const RxCogRequirement *req, RxCogLadder *out) {
    if (!r || !req || !out || req->operation_class >= RX_COG_OP_COUNT) return RX_COG_ERR_ARG;
    r->sealed = 1;
    RxCogRequirement q = normalize(req);
    uint32_t h = req_hash(&q);
    PlanEntry *victim = NULL;
    for (uint32_t k = 0; k < PLAN_PROBES; k++) {
        PlanEntry *e = &r->cache[(h + k) % PLAN_CACHE];
        int live = e->used && e->generation == r->generation;
        if (live && memcmp(&e->req, &q, sizeof q) == 0) {
            *out = e->ladder;
            return e->rc;
        }
        int victim_live = victim && victim->used && victim->generation == r->generation;
        if (!victim || (victim_live && (!live || e->stamp < victim->stamp))) victim = e;
    }
    RxCogLadder l;
    memset(&l, 0, sizeof l);
    int rc = choose(r, &q, &l);
    victim->used = 1;
    victim->stamp = ++r->cache_next;
    victim->generation = r->generation;
    victim->req = q;
    victim->rc = rc;
    victim->ladder = l;
    *out = l;
    return rc;
}

int rx_route_most_expensive(RxCogRouter *r, const RxCogRequirement *req, uint32_t *engine_id) {
    if (!r || !req || !engine_id || req->operation_class >= RX_COG_OP_COUNT) return RX_COG_ERR_ARG;
    RxCogRequirement q = normalize(req);
    uint32_t ix[RX_COG_MAX_ENGINES];
    uint32_t n = candidates(r, &q, ix);
    if (n == 0) {
        RxCogLadder l;
        int rc = reference_ladder(r, &q, &l);
        if (rc == RX_COG_OK) *engine_id = l.engine[0];
        return rc;
    }
    *engine_id = r->eng[ix[n - 1]].decl.cognitive_engine_id;
    return RX_COG_OK;
}

int rx_route_run(RxCogRouter *r, const RxCogRequirement *req, const void *input, void *output,
                 size_t output_size, RxCogTrace *trace) {
    if (!r || !req || !output || !output_size || !trace) return RX_COG_ERR_ARG;
    memset(trace, 0, sizeof *trace);
    RxCogLadder l;
    int rc = rx_route_plan(r, req, &l);
    if (rc != RX_COG_OK) return rc;
    uint32_t op = req->operation_class;
    int last_ok = 0;
    /* The ladder is bounded before it runs: at most escalation_allowed + 1
     * rungs (and RX_COG_MAX_LADDER), and admitted only if its expected cost
     * fits the latency and energy budgets. Nothing measured while it runs
     * changes which rungs run; only the answers do. The times in the trace
     * are observations. */
    for (uint32_t i = 0; i < l.n; i++) {
        int ix = index_of(r, l.engine[i]);
        uint32_t conf = 0;
        uint64_t t0 = mono_ns();
        int erc = r->eng[ix].fn(r->eng[ix].ctx, op, input, output, &conf);
        uint64_t dt = mono_ns() - t0;
        trace->engine[i] = l.engine[i];
        trace->confidence[i] = erc ? 0 : conf;
        trace->ns[i] = dt;
        trace->rungs_run = i + 1;
        trace->escalations = i;
        trace->final_engine = l.engine[i];
        last_ok = erc == 0;
        if (erc == 0 && conf_bin(conf) >= l.accept_min_bin[i]) {
            trace->accepted = 1;
            return RX_COG_OK;
        }
    }
    return last_ok ? RX_COG_ERR_EXHAUSTED : RX_COG_ERR_ENGINE;
}

/* ---- learning ---- */

typedef struct {
    RxCogQuality q;
    RxCogCalibration c;
    uint64_t calls, sum_ns;
    uint64_t *ns;
    uint32_t n_ns;
    uint64_t e_calls, e_nj;
    uint64_t b_calls, b_ns; /* batch timing: mean cost without per-call clock reads */
} Acc;

struct RxCogLedger {
    uint32_t n;
    Engine eng[RX_COG_MAX_ENGINES];
    Acc acc[RX_COG_MAX_ENGINES][RX_COG_OP_COUNT];
    Sample sample[RX_COG_OP_COUNT];
};

int rx_route_ledger_create(const RxCogRouter *r, RxCogLedger **out) {
    if (!r || !out) return RX_COG_ERR_ARG;
    RxCogLedger *l = calloc(1, sizeof *l);
    if (!l) return RX_COG_ERR_FULL;
    l->n = r->n;
    memcpy(l->eng, r->eng, sizeof l->eng);
    *out = l;
    return RX_COG_OK;
}

void rx_route_ledger_destroy(RxCogLedger *l) {
    if (!l) return;
    for (uint32_t i = 0; i < RX_COG_MAX_ENGINES; i++)
        for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) free(l->acc[i][o].ns);
    for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) free(l->sample[o].rec);
    free(l);
}

static int ledger_index(const RxCogLedger *l, uint32_t id) {
    for (uint32_t i = 0; i < l->n; i++)
        if (l->eng[i].decl.cognitive_engine_id == id) return (int)i;
    return -1;
}

int rx_route_observe(RxCogLedger *l, uint32_t op, uint32_t n, const uint32_t *engines,
                     const uint32_t *conf, const uint8_t *correct, const uint64_t *ns,
                     const uint8_t *failed) {
    if (!l || op >= RX_COG_OP_COUNT || !n || n > l->n || !engines || !conf || !correct || !ns ||
        !failed)
        return RX_COG_ERR_ARG;
    int ix[RX_COG_MAX_ENGINES];
    for (uint32_t k = 0; k < n; k++) {
        ix[k] = ledger_index(l, engines[k]);
        if (ix[k] < 0 || !(l->eng[ix[k]].decl.supported_operations >> op & 1)) return RX_COG_ERR_ARG;
        for (uint32_t j = 0; j < k; j++)
            if (ix[j] == ix[k]) return RX_COG_ERR_ARG;
    }
    Sample *s = &l->sample[op];
    if (s->n_eng == 0) {
        s->n_eng = n;
        for (uint32_t k = 0; k < n; k++) s->eng[k] = (uint32_t)ix[k];
        s->rec = malloc((size_t)RX_COG_MAX_SAMPLE * n);
        if (!s->rec) {
            s->n_eng = 0;
            return RX_COG_ERR_FULL;
        }
    } else {
        if (s->n_eng != n) return RX_COG_ERR_ARG;
        for (uint32_t k = 0; k < n; k++)
            if (s->eng[k] != (uint32_t)ix[k]) return RX_COG_ERR_ARG;
    }
    for (uint32_t k = 0; k < n; k++) {
        Acc *a = &l->acc[ix[k]][op];
        uint32_t c = failed[k] ? 0 : conf[k];
        int ok = !failed[k] && correct[k];
        uint32_t b = conf_bin(c);
        a->q.observations++;
        a->q.correct += (uint32_t)ok;
        a->q.failures += failed[k] ? 1u : 0u;
        a->c.n[b]++;
        a->c.correct[b] += (uint32_t)ok;
        a->calls++;
        a->sum_ns += ns[k];
        if (!a->ns) a->ns = malloc(NS_KEEP * sizeof(uint64_t));
        if (a->ns && a->n_ns < NS_KEEP) a->ns[a->n_ns++] = ns[k];
    }
    if (s->n_records < RX_COG_MAX_SAMPLE) {
        uint8_t *rec = s->rec + (size_t)s->n_records * n;
        for (uint32_t k = 0; k < n; k++) {
            int ok = !failed[k] && correct[k];
            rec[k] = (uint8_t)(conf_bin(failed[k] ? 0 : conf[k]) | (ok ? 0x80 : 0));
        }
        s->n_records++;
    }
    return RX_COG_OK;
}

int rx_route_observe_batch(RxCogLedger *l, uint32_t engine_id, uint32_t op, uint64_t calls,
                           uint64_t cpu_ns, int energy_measured, uint64_t nj) {
    if (!l || op >= RX_COG_OP_COUNT || !calls) return RX_COG_ERR_ARG;
    int i = ledger_index(l, engine_id);
    if (i < 0 || !(l->eng[i].decl.supported_operations >> op & 1)) return RX_COG_ERR_ARG;
    Acc *a = &l->acc[i][op];
    a->b_calls += calls;
    a->b_ns += cpu_ns;
    if (energy_measured) {
        a->e_calls += calls;
        a->e_nj += nj;
    }
    return RX_COG_OK;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int rx_route_ledger_model(const RxCogLedger *l, uint8_t **out, size_t *out_len) {
    if (!l || !out || !out_len) return RX_COG_ERR_ARG;
    size_t cap = 48 + (size_t)l->n * (4 + RX_COG_OP_COUNT * (12 + 128 + 36));
    for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++)
        cap += 8 + 4 * l->sample[o].n_eng + (size_t)l->sample[o].n_records * l->sample[o].n_eng;
    if (cap > MODEL_MAX) return RX_COG_ERR_FULL;
    uint8_t *buf = malloc(cap), *p = buf;
    if (!buf) return RX_COG_ERR_FULL;
    put32(&p, MODEL_MAGIC);
    put32(&p, MODEL_VERSION);
    registry_digest(l->eng, l->n, p);
    p += 32;
    put32(&p, l->n);
    for (uint32_t i = 0; i < l->n; i++) {
        put32(&p, l->eng[i].decl.cognitive_engine_id);
        for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) {
            const Acc *a = &l->acc[i][o];
            put32(&p, a->q.observations);
            put32(&p, a->q.correct);
            put32(&p, a->q.failures);
            for (uint32_t b = 0; b < RX_COG_BINS; b++) put32(&p, a->c.n[b]);
            for (uint32_t b = 0; b < RX_COG_BINS; b++) put32(&p, a->c.correct[b]);
            uint64_t p99 = 0;
            if (a->n_ns) {
                uint64_t *tmp = malloc(a->n_ns * sizeof(uint64_t));
                if (!tmp) {
                    free(buf);
                    return RX_COG_ERR_FULL;
                }
                memcpy(tmp, a->ns, a->n_ns * sizeof(uint64_t));
                qsort(tmp, a->n_ns, sizeof(uint64_t), cmp_u64);
                p99 = tmp[(a->n_ns * 99) / 100 < a->n_ns ? (a->n_ns * 99) / 100 : a->n_ns - 1];
                free(tmp);
            }
            put64(&p, a->calls);
            put64(&p, a->b_calls ? a->b_ns / a->b_calls : a->calls ? a->sum_ns / a->calls : 0);
            put64(&p, p99);
            put64(&p, a->e_calls ? a->e_nj / a->e_calls : 0);
            put32(&p, a->e_calls ? 1u : 0u);
        }
    }
    for (uint32_t o = 0; o < RX_COG_OP_COUNT; o++) {
        const Sample *s = &l->sample[o];
        put32(&p, s->n_records);
        put32(&p, s->n_eng);
        for (uint32_t k = 0; k < s->n_eng; k++) put32(&p, l->eng[s->eng[k]].decl.cognitive_engine_id);
        size_t bytes = (size_t)s->n_records * s->n_eng;
        if (bytes) memcpy(p, s->rec, bytes);
        p += bytes;
    }
    *out = buf;
    *out_len = (size_t)(p - buf);
    return RX_COG_OK;
}

int rx_route_propose(const RxCogLedger *l, RxGenStore *store, uint32_t proposer,
                     uint64_t authority_epoch, uint64_t authority_generation, uint64_t *candidate) {
    if (!l || !store || !candidate) return RX_COG_ERR_ARG;
    uint8_t *model = NULL;
    size_t model_len = 0;
    int rc = rx_route_ledger_model(l, &model, &model_len);
    if (rc != RX_COG_OK) return rc;
    uint8_t reg[32];
    registry_digest(l->eng, l->n, reg);
    static const uint8_t config[] = "rx_route model v1";
    uint8_t evidence[32];
    sha256_hash(model, model_len, evidence);
    RxGenDraft d;
    memset(&d, 0, sizeof d);
    d.authority_epoch = authority_epoch;
    d.authority_generation = authority_generation;
    d.proofs_ok = 1;
    d.evidence = evidence;
    d.evidence_len = sizeof evidence;
    d.model = model;
    d.model_len = model_len;
    d.realization = reg;
    d.realization_len = sizeof reg;
    d.config = config;
    d.config_len = sizeof config - 1;
    int grc = rx_gen_propose(store, proposer, &d, candidate);
    free(model);
    return grc == RX_GEN_OK ? RX_COG_OK : RX_COG_ERR_MODEL;
}
