/* rx_world_replay.c -- record a World run to an RXCLOG01 log, and replay a
 * recorded run from its log alone (host only, CPU, no GPU).
 *
 *   rx_world_replay record OUT [workers] [stimuli] [seed]
 *   rx_world_replay replay IN OUT [workers] [perturb]
 *
 * The World is the R3 heartbeat organism (tests/runtime/rx_heartbeat_test.c):
 * sensor -> aien.hypothesis -> belief -> omega.realize -> plan, and
 * omega.risk fed by sensor humidity and belief. Recording publishes seeded
 * outside stimuli; each goes into the log as an INPUT record (with values),
 * then the crumbs it caused, then a CHECKPOINT holding a hash of every
 * object's fields, versions and writers.
 *
 * Replay rebuilds the same World and re-publishes exactly the INPUT records
 * read from the log (nothing else comes from the recording), waiting for
 * quiescence after each, and writes a new log. tools/replay/rx_replay
 * compare then names the first divergent event.
 *
 * perturb (negative controls, so a passing compare means something):
 *   value      omega.realize adds 1001 instead of 1000 (same crumbs, other values)
 *   structure  omega.risk is not registered (different crumbs)
 *   input      the third INPUT is replayed with value + 1 */
#include "replay/rx_crumb_export.h"
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SUBJ_AIEN = 1, SUBJ_OMEGA = 2, SUBJ_EXTERNAL = 100, ISSUER_AEGIS_POLICY = 3 };
enum { RES_SENSOR = 0x10, RES_BELIEF = 0x20, RES_PLAN = 0x30, RES_RISK = 0x40 };
enum { F_TEMP = 0, F_HUMIDITY = 1 };
enum { P_NONE = 0, P_VALUE, P_STRUCTURE, P_INPUT };

typedef struct { RxObjRef sensor, belief, plan, risk; int perturb; } HB;

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    HB h;
    RxCapRef c_ext;
} Env;

static const RxSnapshotDep *in_of(const RxCtx *c, RxObjRef o) {
    for (uint32_t i = 0; i < c->n_in; i++)
        if (c->in[i].obj.id == o.id) return &c->in[i];
    return NULL;
}

static int fn_hypothesis(RxCtx *c) {
    HB *h = c->user;
    const RxSnapshotDep *s = in_of(c, h->sensor);
    if (!s) return -1;
    c->out[c->n_out++] = (RxMutation){ h->belief, 0, s->field[F_TEMP] * 2 + 1 };
    return 0;
}

static int fn_realize(RxCtx *c) {
    HB *h = c->user;
    const RxSnapshotDep *b = in_of(c, h->belief);
    if (!b) return -1;
    c->out[c->n_out++] = (RxMutation){ h->plan, 0, b->field[0] + (h->perturb == P_VALUE ? 1001 : 1000) };
    return 0;
}

static int fn_risk(RxCtx *c) {
    HB *h = c->user;
    const RxSnapshotDep *s = in_of(c, h->sensor);
    const RxSnapshotDep *b = in_of(c, h->belief);
    if (!s || !b) return -1;
    c->out[c->n_out++] = (RxMutation){ h->risk, 0, s->field[F_HUMIDITY] * 100000 + b->field[0] };
    return 0;
}

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = ISSUER_AEGIS_POLICY;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(&e->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    if (rx_capadmin_mint(&e->admin, &m, &r) != RX_CAP_OK) fprintf(stderr, "mint failed\n");
    return r;
}

static RxObjRef mkobj(Env *e, uint64_t resource) {
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    RxObjRef r = { UINT32_MAX, 0 };
    rx_world_create(&e->w, 1, RX_PERSIST_RESIDENT, resource, init, &r);
    return r;
}

static void desc(RxReactionDesc *d, const char *name, uint32_t fac, uint32_t subj, RxFn fn, void *u) {
    memset(d, 0, sizeof *d);
    d->name = name;
    d->faculty = fac;
    d->subject = subj;
    d->priority = RX_PRIO_FOREGROUND;
    d->fn = fn;
    d->user = u;
}
static void trig(RxReactionDesc *d, RxObjRef o, uint64_t m) { d->triggers[d->n_triggers++] = (RxDep){ o, m }; }
static void wr(RxReactionDesc *d, RxObjRef o, uint64_t m) { d->writes[d->n_writes++] = (RxDep){ o, m }; }
static void cap(RxReactionDesc *d, RxCapRef c, uint64_t res, uint32_t rights) {
    d->caps[d->n_caps++] = (RxCapNeed){ c, res, rights };
}

static int setup(Env *e, uint32_t workers, int perturb) {
    memset(e, 0, sizeof *e);
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, workers, 1u << 16) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    e->h.perturb = perturb;
    e->h.sensor = mkobj(e, RES_SENSOR);
    e->h.belief = mkobj(e, RES_BELIEF);
    e->h.plan = mkobj(e, RES_PLAN);
    e->h.risk = mkobj(e, RES_RISK);
    e->c_ext = mint(e, SUBJ_EXTERNAL, RES_SENSOR, RX_RIGHT_WRITE);
    RxCapRef a_rs = mint(e, SUBJ_AIEN, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef a_wb = mint(e, SUBJ_AIEN, RES_BELIEF, RX_RIGHT_WRITE);
    RxCapRef o_rb = mint(e, SUBJ_OMEGA, RES_BELIEF, RX_RIGHT_READ);
    RxCapRef o_wp = mint(e, SUBJ_OMEGA, RES_PLAN, RX_RIGHT_WRITE);
    RxCapRef o_rs = mint(e, SUBJ_OMEGA, RES_SENSOR, RX_RIGHT_READ);
    RxCapRef o_wr = mint(e, SUBJ_OMEGA, RES_RISK, RX_RIGHT_WRITE);
    RxReactionDesc d;
    uint32_t id;
    desc(&d, "aien.hypothesis", RX_FACULTY_AIEN, SUBJ_AIEN, fn_hypothesis, &e->h);
    trig(&d, e->h.sensor, RX_FIELD(F_TEMP));
    wr(&d, e->h.belief, RX_FIELD(0));
    cap(&d, a_rs, RES_SENSOR, RX_RIGHT_READ);
    cap(&d, a_wb, RES_BELIEF, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &id) != RX_OK) return -1;
    desc(&d, "omega.realize", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_realize, &e->h);
    trig(&d, e->h.belief, RX_FIELD(0));
    wr(&d, e->h.plan, RX_FIELD(0));
    cap(&d, o_rb, RES_BELIEF, RX_RIGHT_READ);
    cap(&d, o_wp, RES_PLAN, RX_RIGHT_WRITE);
    if (rx_world_add_reaction(&e->w, &d, &id) != RX_OK) return -1;
    if (perturb != P_STRUCTURE) {
        desc(&d, "omega.risk", RX_FACULTY_OMEGA, SUBJ_OMEGA, fn_risk, &e->h);
        trig(&d, e->h.sensor, RX_FIELD(F_HUMIDITY));
        trig(&d, e->h.belief, RX_FIELD(0));
        wr(&d, e->h.risk, RX_FIELD(0));
        cap(&d, o_rs, RES_SENSOR, RX_RIGHT_READ);
        cap(&d, o_rb, RES_BELIEF, RX_RIGHT_READ);
        cap(&d, o_wr, RES_RISK, RX_RIGHT_WRITE);
        if (rx_world_add_reaction(&e->w, &d, &id) != RX_OK) return -1;
    }
    return 0;
}

static void teardown(Env *e) {
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
}

static void put32(sha256_ctx *c, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    sha256_update(c, b, 4);
}
static void put64(sha256_ctx *c, uint64_t v) { put32(c, (uint32_t)v); put32(c, (uint32_t)(v >> 32)); }

/* State checkpoint: every object's identity, version, values, field
 * versions and field writers (values are what crumbs do not carry). */
static int checkpoint(Env *e, rxl_log *log) {
    const RxObjRef objs[4] = { e->h.sensor, e->h.belief, e->h.plan, e->h.risk };
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"RXCLOG01-STATE", 14);
    for (int i = 0; i < 4; i++) {
        RxObject o;
        if (rx_world_read(&e->w, objs[i], &o) != RX_OK) return -1;
        put32(&c, o.id); put32(&c, o.generation); put32(&c, o.type); put64(&c, o.version);
        for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
            put64(&c, o.field[f]); put64(&c, o.field_version[f]); put64(&c, o.field_writer[f]);
        }
    }
    rxl_rec r;
    memset(&r, 0, sizeof r);
    r.type = RXL_CHECKPOINT;
    r.u.ck.through_crumb = e->w.n_crumbs;
    r.u.ck.subsystem = 1;
    sha256_final(&c, r.u.ck.hash);
    return rxl_push(log, &r);
}

/* Publish one INPUT record, log it and everything it caused. */
static int drive(Env *e, rxl_log *log, uint64_t *next, const rxl_input *in) {
    rxl_rec r;
    memset(&r, 0, sizeof r);
    r.type = RXL_INPUT;
    r.u.in = *in;
    r.u.in.after_crumb = e->w.n_crumbs;
    if (rxl_push(log, &r)) return -1;
    RxMutation m[RXL_MAX_MUTS];
    for (uint32_t i = 0; i < in->n; i++)
        m[i] = (RxMutation){ { in->m[i].id, in->m[i].gen }, in->m[i].field, in->m[i].value };
    int64_t rc = rx_world_publish_external(&e->w, (RxCapRef){ in->cap_id, in->cap_gen }, m, in->n);
    if (rc <= 0) { fprintf(stderr, "publish refused: %lld\n", (long long)rc); return -1; }
    if (rx_world_wait_quiescent(&e->w, 10000) != RX_OK) { fprintf(stderr, "no quiescence\n"); return -1; }
    if (rxx_append_crumbs(log, &e->w, next)) return -1;
    return checkpoint(e, log);
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static int record(const char *out, uint32_t workers, uint32_t stimuli, uint32_t seed) {
    Env e;
    if (setup(&e, workers, P_NONE)) { fprintf(stderr, "setup failed\n"); return 2; }
    rxl_log log;
    rxl_init(&log, RXL_FLAG_INPUTS);
    uint64_t next = 1;
    int bad = rxx_append_crumbs(&log, &e.w, &next) || checkpoint(&e, &log);
    for (uint32_t k = 0; k < stimuli && !bad; k++) {
        rxl_input in;
        memset(&in, 0, sizeof in);
        in.cap_id = e.c_ext.cap_id;
        in.cap_gen = e.c_ext.generation;
        uint32_t pick = lcg(&seed) % 3;     /* temp, humidity, or both */
        if (pick != 1) in.m[in.n++] = (rxl_mut){ e.h.sensor.id, e.h.sensor.generation, F_TEMP, lcg(&seed) % 8 };
        if (pick != 0) in.m[in.n++] = (rxl_mut){ e.h.sensor.id, e.h.sensor.generation, F_HUMIDITY, lcg(&seed) % 8 };
        bad = drive(&e, &log, &next, &in);
    }
    teardown(&e);
    if (bad || rxl_finish(&log) || rxl_write(out, &log)) { fprintf(stderr, "record failed\n"); rxl_free(&log); return 2; }
    printf("recorded %zu events (%llu crumbs, %u stimuli, %u workers) to %s\n", log.n - 1,
           (unsigned long long)(next - 1), stimuli, workers, out);
    rxl_free(&log);
    return 0;
}

static int replay(const char *in_path, const char *out, uint32_t workers, int perturb) {
    rxl_log in;
    char err[160];
    if (rxl_read(in_path, &in, err, sizeof err)) { fprintf(stderr, "replay: %s\n", err); return 2; }
    Env e;
    if (setup(&e, workers, perturb)) { fprintf(stderr, "setup failed\n"); rxl_free(&in); return 2; }
    rxl_log log;
    rxl_init(&log, RXL_FLAG_INPUTS);
    uint64_t next = 1;
    int bad = rxx_append_crumbs(&log, &e.w, &next) || checkpoint(&e, &log);
    uint32_t n_in = 0;
    for (size_t i = 0; i < in.n && !bad; i++) {
        if (in.recs[i].type != RXL_INPUT) continue;
        rxl_input x = in.recs[i].u.in;
        if (x.cap_id != e.c_ext.cap_id || x.cap_gen != e.c_ext.generation) {
            fprintf(stderr, "replay: recorded capability %u/%llu is not the rebuilt one\n", x.cap_id,
                    (unsigned long long)x.cap_gen);
            bad = 1;
            break;
        }
        if (perturb == P_INPUT && n_in == 2) x.m[0].value += 1;
        n_in++;
        bad = drive(&e, &log, &next, &x);
    }
    teardown(&e);
    rxl_free(&in);
    if (bad || rxl_finish(&log) || rxl_write(out, &log)) { fprintf(stderr, "replay failed\n"); rxl_free(&log); return 2; }
    printf("replayed %u inputs into %zu events (%u workers) to %s\n", n_in, log.n - 1, workers, out);
    rxl_free(&log);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "record"))
        return record(argv[2], argc > 3 ? (uint32_t)atoi(argv[3]) : 1, argc > 4 ? (uint32_t)atoi(argv[4]) : 24,
                      argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 0) : 0xA11CEu);
    if (argc >= 4 && !strcmp(argv[1], "replay")) {
        int p = P_NONE;
        if (argc > 5) {
            if (!strcmp(argv[5], "value")) p = P_VALUE;
            else if (!strcmp(argv[5], "structure")) p = P_STRUCTURE;
            else if (!strcmp(argv[5], "input")) p = P_INPUT;
            else { fprintf(stderr, "unknown perturbation %s\n", argv[5]); return 2; }
        }
        return replay(argv[2], argv[3], argc > 4 ? (uint32_t)atoi(argv[4]) : 1, p);
    }
    fprintf(stderr, "usage: rx_world_replay record OUT [workers] [stimuli] [seed]\n"
                    "       rx_world_replay replay IN OUT [workers] [value|structure|input]\n");
    return 2;
}
