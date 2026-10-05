/*
 * rx_dual_jspace_tap.c -- DUAL-3a parity tests for the read-only observation
 * tap at decision site "jspace.residency" (js_forge_enforce, rx_jspace.c).
 *
 * Every scenario builds the same deterministic J-Space workload three times:
 * once with no observer (today's behaviour), twice with a recording observer
 * (legs A and B). Fixed JsCosts replace calibration so the chooser's inputs are
 * identical run to run. Checked, per scenario:
 *
 *   - every js_forge_enforce return code and every JsPolicyReport identical
 *     with and without the observer;
 *   - JsStats (actions, residency, peak, spilled, live, derives, copies) and
 *     the final space digest (every branch, unit, placement, extent, content
 *     check, then full content digests) identical with and without;
 *   - the record stream of leg A is byte-identical to leg B (determinism);
 *   - one record per considered candidate (count == JsPolicyReport.considered),
 *     seq strictly increasing from 1, canonical length JS_DUAL_OBS_BYTES, digest
 *     recomputed and equal;
 *   - each record's chosen action equals the decision the chooser rule gives on
 *     the record's own alternatives (first cheapest priced alternative, keep when
 *     its score is at or above pressure * retain);
 *   - each record's chosen action equals the placement transition that very
 *     realization underwent in the NO-OBSERVER run (slot + generation keyed), so
 *     the observer run took the same decision per realization as the silent run;
 *   - pressure value and case agree with the budget/residency bytes recorded.
 *
 * Pure-extraction check: choose() was split into decide() (working set exposed)
 * plus a wrapper. reference_choose_pre_tap() below is the verbatim pre-change
 * body (a class D reference oracle, test only). On every live realization of a
 * fan-out workload, for both pressure values and both allow_move values, the
 * two must agree.
 *
 * Negative control: built with -DDUAL_TAP_HOSTILE_TEST the leg-A observer casts
 * its ctx to the space and raises retain_ns_per_byte on its third record. The
 * decisions that follow differ from the silent run and this program MUST exit 1.
 * The production source never defines or tests that macro (checked by grep in
 * mk/dual_jspace_tap.mk).
 *
 * This file includes rx_jspace.c (not linked with it) so the reference oracle
 * can call the static cost helpers and the digest can walk the index.
 */
#include "runtime/rx_jspace.c"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNIT 4096u
#define U(x) ((unsigned long long)(x))

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- deterministic realizer -------------------------------------------------- */

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Root: pseudo-random fill. Derive: one word in eight changes, so a delta
 * against the parent is compressible (COMPRESS becomes a real alternative). */
static void derive_sparse(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    uint64_t x = token;
    if (!prev) {
        for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t v = splitmix(&x); memcpy(out + i, &v, 8); }
        return;
    }
    memcpy(out, prev, n);
    for (size_t i = (size_t)(token % 8) * 8; i + 8 <= n; i += 64) {
        uint64_t v = splitmix(&x);
        memcpy(out + i, &v, 8);
    }
}

static const JsRealizer RZ = { JS_REAL_LATENT_CHECKPOINT, "sparse", UNIT, derive_sparse };

/* Fixed costs. With n = UNIT, e = 1, parent HOT:
 *   MOVE 0.25   COMPRESS (0.5 + 0.5) / 0.75 = 1.333   SPILL 2 + 4 = 6   EVICT 1.0
 * Soft threshold 1e3 * retain = 0.9765625: MOVE is taken, nothing else.
 * Capacity threshold 1e9 * retain: EVICT (1.0) beats COMPRESS (1.333). */
static void costs_plain(JsSpace *s) {
    JsCosts c = { 0 };
    c.derive_ns = (double)UNIT;
    c.copy_ns_per_byte = 0.125;
    c.move_ns_per_byte = 0.125;
    c.compress_ns_per_byte = 0.5;
    c.decompress_ns_per_byte = 0.5;
    c.compress_ratio = 0.25;
    c.spill_write_ns_per_byte = 2.0;
    c.spill_read_ns_per_byte = 4.0;
    c.retain_ns_per_byte = 1.0 / 1024.0;
    s->costs = c;
}

/* Tie costs: COMPRESS = (0.25 + 0.25) / 0.5 = 1.0 and EVICT = 1.0 exactly
 * (parent HOT, e = 1). The chooser's strict "<" keeps the first built. */
static void costs_tie(JsSpace *s) {
    costs_plain(s);
    s->costs.compress_ns_per_byte = 0.25;
    s->costs.decompress_ns_per_byte = 0.25;
    s->costs.compress_ratio = 0.5;
}

/* Spill-cheap costs: SPILL (0.125 + 0.125) = 0.25 beats EVICT 1.0 and COMPRESS. */
static void costs_spill(JsSpace *s) {
    costs_plain(s);
    s->costs.spill_write_ns_per_byte = 0.125;
    s->costs.spill_read_ns_per_byte = 0.125;
}

/* ---- record store ------------------------------------------------------------ */

typedef struct {
    uint8_t *bytes;            /* JS_DUAL_OBS_BYTES + 32 digest per record */
    size_t n, cap;
    JsDualObservation last;
    int bad_len, bad_digest, bad_seq, bad_rule;
    uint64_t next_seq;
    JsSpace *space;            /* hostile build only: the observer's way in */
    int tamper_record;         /* normal build: write into the const record */
} Rec;

#define REC_STRIDE (JS_DUAL_OBS_BYTES + 32u)

/* The chooser rule restated over a record's alternatives. */
static uint8_t rule_of(const JsDualObservation *o) {
    uint8_t act = JS_ACT_COUNT;
    double best = 0;
    for (unsigned i = 0; i < JS_DUAL_OBS_N_ALT; i++) {
        if (o->alt[i].status != JS_DUAL_ALT_PRICED) continue;
        if (act == JS_ACT_COUNT || o->alt[i].score < best) { best = o->alt[i].score; act = o->alt[i].action; }
    }
    if (act != JS_ACT_COUNT && best >= o->keep_threshold) return JS_ACT_COUNT;
    return act;
}

static void record(const JsDualObservation *o, const void *ctx) {
    Rec *r = (Rec *)ctx;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->bytes = realloc(r->bytes, r->cap * REC_STRIDE);
    }
    uint8_t *dst = r->bytes + r->n * REC_STRIDE;
    size_t len = js_dual_observation_encode(o, dst);
    if (len != JS_DUAL_OBS_BYTES) r->bad_len++;
    sha256_ctx h;
    uint8_t d[32];
    sha256_init(&h);
    sha256_update(&h, (const uint8_t *)JS_DUAL_OBS_DOMAIN, sizeof JS_DUAL_OBS_DOMAIN);
    sha256_update(&h, dst, len);
    sha256_final(&h, d);
    if (memcmp(d, o->digest, 32)) r->bad_digest++;
    memcpy(dst + JS_DUAL_OBS_BYTES, o->digest, 32);
    if (o->seq != r->next_seq) r->bad_seq++;
    r->next_seq = o->seq + 1;
    if (rule_of(o) != o->chosen) r->bad_rule++;
    r->last = *o;
    r->n++;
    if (r->tamper_record) {
        /* A hostile observer writes into the record it was handed. The chooser's
         * result is a separate local: the production decision must not move. */
        JsDualObservation *w = (JsDualObservation *)o;
        w->chosen = JS_ACT_EVICT;
        w->pressure = 0;
    }
#ifdef DUAL_TAP_HOSTILE_TEST
    /* retain price 0 makes every priced alternative "at or above" the keep
     * threshold: from here on the chooser keeps everything. */
    if (r->n == 3 && r->space) r->space->costs.retain_ns_per_byte = 0.0;
#endif
}

static void rec_free(Rec *r) { free(r->bytes); memset(r, 0, sizeof *r); }

/* ---- workload ---------------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*costs)(JsSpace *);
    unsigned fanout, depth;
    uint64_t max_spill;            /* 0 = unlimited */
    int rounds;
    /* budget per round: resident_bytes * num / den + add, computed when the round starts */
    struct { unsigned num, den; int64_t add; } budget[4];
    int rep_null;                  /* pass rep = NULL to enforce */
    int tamper;                    /* observer writes into the record */
} Scenario;

static unsigned g_spill_seq;

static void space_open(JsSpace *s, const Scenario *sc) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/jstap_%ld_%u", (long)getpid(), g_spill_seq++);
    JsLimits l;
    js_limits_default(&l);
    l.max_spill_bytes = sc->max_spill;
    CHECK(js_space_init_limits(s, path, &l) == JS_OK, "%s: init", sc->name);
    sc->costs(s);
}

/* One root with `depth` derived units, then `fanout` forks each deriving one
 * more unit: fanout + depth + 1 realizations, all HOT. */
static void program(JsSpace *s, const Scenario *sc, uint32_t *root_out) {
    uint32_t root;
    CHECK(js_branch_root(s, &RZ, 7, &root) == JS_OK, "%s: root", sc->name);
    for (unsigned i = 0; i < sc->depth; i++) CHECK(js_branch_derive(s, root, 100 + i) == JS_OK, "%s: derive", sc->name);
    for (unsigned k = 0; k < sc->fanout; k++) {
        uint32_t c;
        CHECK(js_branch_fork(s, root, &c) == JS_OK, "%s: fork %u", sc->name, k);
        CHECK(js_branch_derive(s, c, 1000 + k) == JS_OK, "%s: derive %u", sc->name, k);
    }
    *root_out = root;
}

/* ---- state view -------------------------------------------------------------- */

typedef struct { uint32_t slot; uint64_t gen; uint8_t placement; } Place;

static int place_cmp(const void *a, const void *b) {
    const Place *x = a, *y = b;
    return (x->slot > y->slot) - (x->slot < y->slot);
}

static uint32_t placements(JsSpace *s, Place **out) {
    Cand *cs;
    uint32_t n = collect(s, &cs);
    Place *p = calloc(n ? n : 1, sizeof *p);
    for (uint32_t i = 0; i < n; i++) {
        p[i].slot = cs[i].r->slot; p[i].gen = cs[i].r->gen; p[i].placement = (uint8_t)cs[i].r->placement;
    }
    free(cs);
    qsort(p, n, sizeof *p, place_cmp);
    *out = p;
    return n;
}

static void space_digest(JsSpace *s, uint8_t out[32]) {
    sha256_ctx h;
    sha256_init(&h);
    for (uint32_t b = 0; b < JS_MAX_BRANCHES; b++) {
        JsBranch *br = s->branches[b];
        if (!br) continue;
        sha256_update(&h, (const uint8_t *)&b, 4);
        sha256_update(&h, (const uint8_t *)&br->n_units, 4);
        for (uint32_t i = 0; i < br->n_units; i++) {
            const JsReal *r = br->units[i];
            uint64_t v[5] = { r->slot, r->gen, r->placement, r->packed_len,
                              r->placement == JS_PLACE_SPILLED ? r->spill_off : 0 };
            sha256_update(&h, (const uint8_t *)v, sizeof v);
            sha256_update(&h, r->content, 32);
            sha256_update(&h, r->semantic_state_id.b, 32);
        }
    }
    sha256_update(&h, (const uint8_t *)&s->stats, sizeof s->stats);
    sha256_update(&h, (const uint8_t *)&s->spill_end, sizeof s->spill_end);
    /* Full content digests last: they restore bytes, identically in every leg. */
    for (uint32_t b = 0; b < JS_MAX_BRANCHES; b++) {
        if (!s->branches[b]) continue;
        uint8_t d[32] = { 0 };
        int rc = js_branch_content_digest(s, b, d);
        sha256_update(&h, (const uint8_t *)&rc, sizeof rc);
        sha256_update(&h, d, 32);
    }
    sha256_final(&h, out);
}

/* ---- one leg ----------------------------------------------------------------- */

typedef struct {
    int rc[4];
    JsPolicyReport rep[4];
    JsStats stats;
    uint8_t digest[32];
    /* per round, the silent run's placement before and after enforce */
    Place *before[4], *after[4];
    uint32_t n_place[4];
    uint64_t resident_before[4], budget[4];
} Leg;

static uint64_t budget_of(const Scenario *sc, int round, uint64_t resident) {
    int64_t b = (int64_t)(resident * sc->budget[round].num / sc->budget[round].den) + sc->budget[round].add;
    return b < 0 ? 0 : (uint64_t)b;
}

static void run_leg(const Scenario *sc, Rec *rec, Leg *leg, int keep_places) {
    JsSpace s;
    uint32_t root;
    memset(leg, 0, sizeof *leg);
    space_open(&s, sc);
    program(&s, sc, &root);
    if (rec) { rec->space = &s; js_dual_set_observer(&s, record, rec); }
    for (int k = 0; k < sc->rounds; k++) {
        /* Make everything resident again between rounds (COLD stays COLD) so
         * each round decides again. */
        if (k) for (uint32_t b = 0; b < JS_MAX_BRANCHES; b++) {
            JsBranch *br = s.branches[b];
            if (!br) continue;
            for (uint32_t i = 0; i < br->n_units; i++) js_real_restore(&s, br->units[i]);
        }
        leg->resident_before[k] = s.stats.resident_bytes;
        leg->budget[k] = budget_of(sc, k, s.stats.resident_bytes);
        if (keep_places) leg->n_place[k] = placements(&s, &leg->before[k]);
        memset(&leg->rep[k], 0, sizeof leg->rep[k]);
        leg->rc[k] = js_forge_enforce(&s, leg->budget[k], sc->rep_null ? NULL : &leg->rep[k]);
        if (keep_places) placements(&s, &leg->after[k]);
    }
    leg->stats = s.stats;
    space_digest(&s, leg->digest);
    js_space_destroy(&s);
}

static void leg_free(Leg *l) {
    for (int k = 0; k < 4; k++) { free(l->before[k]); free(l->after[k]); }
}

static uint8_t transition(uint8_t from, uint8_t to) {
    if (from == to) return JS_ACT_COUNT;
    if (from == JS_PLACE_HOT && to == JS_PLACE_COLD) return JS_ACT_MOVE;
    if (to == JS_PLACE_COMPRESSED) return JS_ACT_COMPRESS;
    if (to == JS_PLACE_SPILLED) return JS_ACT_SPILL;
    if (to == JS_PLACE_EVICTED) return JS_ACT_EVICT;
    return 0xff;
}

/* ---- scenario ---------------------------------------------------------------- */

static int same_rep(const JsPolicyReport *a, const JsPolicyReport *b) { return !memcmp(a, b, sizeof *a); }

static void run_scenario(const Scenario *sc) {
    Leg silent, la, lb;
    Rec ra = { 0 }, rb = { 0 };
    ra.next_seq = rb.next_seq = 1;
    ra.tamper_record = rb.tamper_record = sc->tamper;
    run_leg(sc, NULL, &silent, 1);
    run_leg(sc, &ra, &la, 0);
    run_leg(sc, &rb, &lb, 0);

    uint64_t considered = 0;
    for (int k = 0; k < sc->rounds; k++) {
        CHECK(silent.rc[k] == la.rc[k] && silent.rc[k] == lb.rc[k],
              "%s round %d: rc silent %d A %d B %d", sc->name, k, silent.rc[k], la.rc[k], lb.rc[k]);
        CHECK(same_rep(&silent.rep[k], &la.rep[k]) && same_rep(&silent.rep[k], &lb.rep[k]),
              "%s round %d: JsPolicyReport differs with observer", sc->name, k);
        CHECK(silent.budget[k] == la.budget[k] && silent.resident_before[k] == la.resident_before[k],
              "%s round %d: budget/residency differ before enforce", sc->name, k);
        considered += silent.rep[k].considered;
    }
    CHECK(!memcmp(&silent.stats, &la.stats, sizeof silent.stats) && !memcmp(&silent.stats, &lb.stats, sizeof silent.stats),
          "%s: JsStats differ with observer (resident silent %llu A %llu B %llu)", sc->name,
          U(silent.stats.resident_bytes), U(la.stats.resident_bytes), U(lb.stats.resident_bytes));
    CHECK(!memcmp(silent.digest, la.digest, 32) && !memcmp(silent.digest, lb.digest, 32),
          "%s: final space digest differs with observer", sc->name);
    CHECK(ra.n == rb.n && (ra.n == 0 || !memcmp(ra.bytes, rb.bytes, ra.n * REC_STRIDE)),
          "%s: record streams A (%zu) and B (%zu) differ", sc->name, ra.n, rb.n);
    if (!sc->rep_null)
        CHECK(ra.n == considered, "%s: %zu records for %llu considered candidates", sc->name, ra.n, U(considered));
    CHECK(!ra.bad_len && !ra.bad_digest && !ra.bad_seq, "%s: records malformed (len %d digest %d seq %d)",
          sc->name, ra.bad_len, ra.bad_digest, ra.bad_seq);
    CHECK(!ra.bad_rule, "%s: %d records whose chosen action is not the chooser rule on their alternatives",
          sc->name, ra.bad_rule);

    /* Per-realization parity with the silent run: walk leg A's records round by
     * round (records are in enforce order; a round emits rep.considered of them)
     * and compare chosen with the placement transition the silent run shows. */
    size_t at = 0;
    for (int k = 0; k < sc->rounds && !sc->rep_null; k++) {
        uint64_t n = silent.rep[k].considered;
        for (uint64_t j = 0; j < n && at < ra.n; j++, at++) {
            const uint8_t *enc = ra.bytes + at * REC_STRIDE;
            /* fixed offsets of the canonical layout */
            uint8_t chosen = enc[37];
            uint8_t pcase = enc[35];
            double pressure; memcpy(&pressure, enc + 27, 8);
            uint32_t slot; memcpy(&slot, enc + 199, 4);
            uint64_t gen; memcpy(&gen, enc + 203, 8);
            uint64_t budget, resident; memcpy(&budget, enc + 278, 8); memcpy(&resident, enc + 286, 8);
            CHECK(budget == silent.budget[k], "%s round %d rec %llu: budget %llu != %llu", sc->name, k, U(j), U(budget), U(silent.budget[k]));
            CHECK((pcase == JS_DUAL_PRESSURE_CAPACITY) == (resident > budget),
                  "%s round %d rec %llu: pressure case %u with resident %llu budget %llu", sc->name, k, U(j), pcase, U(resident), U(budget));
            CHECK(pressure == (pcase == JS_DUAL_PRESSURE_CAPACITY ? 1e9 : 1e3),
                  "%s round %d rec %llu: pressure %g for case %u", sc->name, k, U(j), pressure, pcase);
            Place key = { slot, 0, 0 }, *pb, *pa;
            pb = bsearch(&key, silent.before[k], silent.n_place[k], sizeof key, place_cmp);
            pa = bsearch(&key, silent.after[k], silent.n_place[k], sizeof key, place_cmp);
            CHECK(pb && pa && pb->gen == gen, "%s round %d rec %llu: realization slot %u gen %llu unknown to the silent run",
                  sc->name, k, U(j), slot, U(gen));
            if (!pb || !pa) continue;
            uint8_t tr = transition(pb->placement, pa->placement);
            /* The last record of a round that ended in an error names an action
             * that was chosen but failed; its placement did not change. */
            int failed_last = silent.rc[k] != JS_OK && j == n - 1;
            if (!failed_last)
                CHECK(tr == chosen, "%s round %d rec %llu: observer saw %s, silent run did %s on slot %u",
                      sc->name, k, U(j), js_action_name(chosen), js_action_name(tr), slot);
            else
                CHECK(tr == JS_ACT_COUNT && chosen != JS_ACT_COUNT,
                      "%s round %d: failed last decision not visible as expected", sc->name, k);
        }
    }

    printf("  %-22s rounds %d records %zu considered %llu rc[0] %d resident %llu digest %02x%02x%02x%02x %s\n",
           sc->name, sc->rounds, ra.n, U(considered), silent.rc[0], U(silent.stats.resident_bytes),
           silent.digest[0], silent.digest[1], silent.digest[2], silent.digest[3],
           g_fail ? "(failures above)" : "ok");
    rec_free(&ra); rec_free(&rb);
    leg_free(&silent); leg_free(&la); leg_free(&lb);
}

/* ---- scenario-specific assertions on record content -------------------------- */

static void check_cases(void) {
    /* Soft case: all HOT, total under budget, hot over half: every record is
     * SOFT with pressure 1e3, MOVE allowed and chosen (0.25 < 0.9765625). */
    Scenario soft = { "soft-case", costs_plain, 16, 4, 0, 1, { { 3, 2, 0 } }, 0, 0 };
    Rec r = { 0 }; r.next_seq = 1;
    Leg l;
    run_leg(&soft, &r, &l, 0);
    CHECK(r.n > 0, "soft: no records");
    int all_soft = 1, all_move = 1;
    for (size_t i = 0; i < r.n; i++) {
        const uint8_t *e = r.bytes + i * REC_STRIDE;
        if (e[35] != JS_DUAL_PRESSURE_SOFT || e[36] != 1) all_soft = 0;
        if (e[37] != JS_ACT_MOVE) all_move = 0;
    }
    CHECK(all_soft, "soft: a record was not the SOFT case with MOVE allowed");
    CHECK(all_move, "soft: a record chose something other than MOVE");
    CHECK(r.last.alt[0].status == JS_DUAL_ALT_PRICED && r.last.alt[0].score == 0.25,
          "soft: MOVE score %g", r.last.alt[0].score);
    CHECK(r.last.alt[3].status == JS_DUAL_ALT_PRICED && r.last.alt[3].score == 1.0,
          "soft: EVICT score %g", r.last.alt[3].score);
    CHECK(r.last.keep_threshold == 1e3 / 1024.0, "soft: keep threshold %g", r.last.keep_threshold);
    CHECK(r.last.kind == JS_DUAL_OBS_KIND && r.last.version == JS_DUAL_OBS_VERSION, "soft: kind/version");
    CHECK(r.last.unit_bytes == UNIT && r.last.held_bytes == UNIT, "soft: unit/held bytes");
    rec_free(&r); leg_free(&l);

    /* Capacity case: total over budget: MOVE inadmissible, EVICT chosen. */
    Scenario cap = { "capacity-case", costs_plain, 16, 4, 0, 1, { { 1, 2, 0 } }, 0, 0 };
    memset(&r, 0, sizeof r); r.next_seq = 1;
    run_leg(&cap, &r, &l, 0);
    /* Once total residency is back under budget the same enforce call goes on
     * with the hot-arena SOFT case, so a run holds both cases: every CAPACITY
     * record must have MOVE inadmissible and allow_move 0; the first record is
     * CAPACITY with EVICT chosen under the 1e9 threshold. */
    CHECK(r.n > 0, "capacity: no records");
    int n_cap = 0, n_soft = 0, bad = 0;
    for (size_t i = 0; i < r.n; i++) {
        const uint8_t *e = r.bytes + i * REC_STRIDE;
        if (e[35] == JS_DUAL_PRESSURE_CAPACITY) {
            n_cap++;
            if (e[36] != 0 || e[39] != JS_ACT_MOVE || e[40] != JS_DUAL_ALT_INADMISSIBLE) bad++;
        } else n_soft++;
    }
    CHECK(n_cap > 0 && !bad, "capacity: %d CAPACITY records, %d with MOVE admissible", n_cap, bad);
    {
        const uint8_t *e0 = r.bytes;
        double thr; memcpy(&thr, e0 + 183, 8);
        CHECK(e0[35] == JS_DUAL_PRESSURE_CAPACITY && e0[37] == JS_ACT_EVICT && thr == 1e9 / 1024.0,
              "capacity: first record case %u chose %s threshold %g", e0[35], js_action_name(e0[37]), thr);
    }
    printf("  capacity: %d CAPACITY then %d SOFT records in one enforce call\n", n_cap, n_soft);
    rec_free(&r); leg_free(&l);

    /* Ties: COMPRESS and EVICT price to exactly 1.0; the first built wins. */
    Scenario tie = { "tie-case", costs_tie, 16, 4, 0, 1, { { 1, 2, 0 } }, 0, 0 };
    memset(&r, 0, sizeof r); r.next_seq = 1;
    run_leg(&tie, &r, &l, 0);
    int ties = 0, tie_ok = 1;
    for (size_t i = 0; i < r.n; i++) {
        const uint8_t *e = r.bytes + i * REC_STRIDE;
        double sc_c, sc_e, best; memcpy(&sc_c, e + 73 + 26, 8); memcpy(&sc_e, e + 141 + 26, 8);
        memcpy(&best, e + 175, 8);
        /* a tie between COMPRESS and EVICT that is also the cheapest price */
        if (e[74] == JS_DUAL_ALT_PRICED && e[142] == JS_DUAL_ALT_PRICED && sc_c == sc_e && best == sc_c) {
            ties++;
            if (e[37] != JS_ACT_COMPRESS) tie_ok = 0;
        }
    }
    CHECK(ties > 0, "tie: no record where COMPRESS == EVICT is the cheapest price");
    printf("  ties: %d records with COMPRESS and EVICT tied at the cheapest price\n", ties);
    CHECK(tie_ok, "tie: a tied record did not choose the first-built alternative");
    rec_free(&r); leg_free(&l);
}

/* ---- pure extraction: decide()/choose() against the pre-tap body ------------- */

/* Verbatim copy of choose() as it was before DUAL-3a (reference oracle). */
static JsAction reference_choose_pre_tap(const JsSpace *s, const JsReal *r, double pressure, bool allow_move) {
    const JsCosts *c = &s->costs;
    double n = (double)r->realizer->unit_bytes;
    double e = expected_reads(r);
    double best = 0;
    JsAction act = JS_ACT_COUNT;   /* keep */
    if (r->placement != JS_PLACE_HOT && r->placement != JS_PLACE_COLD &&
        r->placement != JS_PLACE_COMPRESSED) return JS_ACT_COUNT;
    double held = r->placement == JS_PLACE_COMPRESSED ? (double)r->packed_len : n;
    struct { JsAction a; double now, later, freed; } opt[4];
    int k = 0;
    if (r->placement == JS_PLACE_HOT && allow_move) {
        /* MOVE frees hot-arena bytes, not total residency. */
        opt[k++] = (typeof(opt[0])){ JS_ACT_MOVE, n * c->move_ns_per_byte, 0, n * 0.5 };
    }
    if (r->placement != JS_PLACE_COMPRESSED) {
        double p = predicted_packed(s, r);
        if (p < n)
            /* A delta is decoded against its parent's bytes, so restoring it
             * also pays for producing those. */
            opt[k++] = (typeof(opt[0])){ JS_ACT_COMPRESS, n * c->compress_ns_per_byte,
                                         n * c->decompress_ns_per_byte +
                                         (r->parent_realization ? restore_cost(s, r->parent_realization) : 0),
                                         n - p };
    }
    opt[k++] = (typeof(opt[0])){ JS_ACT_SPILL, n * c->spill_write_ns_per_byte,
                                 n * c->spill_read_ns_per_byte, held };
    opt[k++] = (typeof(opt[0])){ JS_ACT_EVICT, 0, recompute_cost(s, r), held };
    for (int i = 0; i < k; i++) {
        if (opt[i].freed <= 0) continue;
        /* nanoseconds of expected work per byte of residency given back */
        double cost = (opt[i].now + e * opt[i].later) / opt[i].freed;
        if (act == JS_ACT_COUNT || cost < best) { best = cost; act = opt[i].a; }
    }
    /* Keep when holding the bytes is cheaper than the best way to give them up. */
    if (act != JS_ACT_COUNT && best >= pressure * c->retain_ns_per_byte) return JS_ACT_COUNT;
    return act;
}

static void check_extraction(void) {
    static void (*const cost_sets[3])(JsSpace *) = { costs_plain, costs_tie, costs_spill };
    static const double pressures[4] = { 1e3, 1e9, 0.0, 1e-3 };
    unsigned compared = 0, mismatches = 0;
    for (int cs = 0; cs < 3; cs++) {
        Scenario sc = { "extraction", cost_sets[cs], 64, 8, 0, 1, { { 1, 1, 0 } }, 0, 0 };
        JsSpace s;
        uint32_t root;
        space_open(&s, &sc);
        program(&s, &sc, &root);
        /* Mixed placements: some COLD, COMPRESSED, SPILLED, EVICTED. */
        Cand *cands;
        uint32_t n = collect(&s, &cands);
        for (uint32_t i = 0; i < n; i++) {
            JsReal *r = cands[i].r;
            switch (i % 5) {
            case 1: js_real_move(&s, r); break;
            case 2: js_real_compress(&s, r); break;
            case 3: js_real_spill(&s, r); break;
            case 4: js_real_evict(&s, r); break;
            default: break;
            }
        }
        for (uint32_t i = 0; i < n; i++)
            for (int p = 0; p < 4; p++)
                for (int am = 0; am < 2; am++) {
                    JsAction a = choose(&s, cands[i].r, pressures[p], am);
                    JsAction b = reference_choose_pre_tap(&s, cands[i].r, pressures[p], am);
                    compared++;
                    if (a != b) mismatches++;
                    /* and the public pure entry point stays the allow_move = true decision */
                    if (am && js_forge_choose(&s, cands[i].r, pressures[p]) != b) mismatches++;
                }
        free(cands);
        js_space_destroy(&s);
    }
    CHECK(mismatches == 0, "extraction: %u of %u decisions differ from the pre-tap chooser", mismatches, compared);
    printf("  extraction: %u decisions compared with the pre-tap chooser, %u mismatches\n", compared, mismatches);
}

/* ---- encoding ----------------------------------------------------------------- */

static void check_encoding(void) {
    JsDualObservation o;
    memset(&o, 0, sizeof o);
    o.kind = JS_DUAL_OBS_KIND; o.version = JS_DUAL_OBS_VERSION; o.seq = 0x0102030405060708ull;
    o.pressure = 1e3; o.pressure_case = JS_DUAL_PRESSURE_SOFT; o.allow_move = 1; o.chosen = JS_ACT_MOVE;
    o.real_slot = 0xAABBCCDDu; o.budget_bytes = 0x1122334455667788ull;
    uint8_t enc[JS_DUAL_OBS_BYTES];
    size_t len = js_dual_observation_encode(&o, enc);
    CHECK(len == JS_DUAL_OBS_BYTES, "encode: %zu bytes, declared %u", len, JS_DUAL_OBS_BYTES);
    CHECK(enc[0] == JS_DUAL_OBS_KIND && enc[1] == JS_DUAL_OBS_VERSION && enc[2] == JS_DUAL_SITE_ID_LEN,
          "encode: kind/version/site length header");
    CHECK(!memcmp(enc + 3, JS_DUAL_SITE_ID, JS_DUAL_SITE_ID_LEN), "encode: site id");
    CHECK(enc[19] == 0x08 && enc[26] == 0x01, "encode: seq little-endian");
    CHECK(enc[199] == 0xDD && enc[202] == 0xAA, "encode: slot little-endian");
    CHECK(enc[278] == 0x88 && enc[285] == 0x11, "encode: budget little-endian at offset 278");
    uint8_t enc2[JS_DUAL_OBS_BYTES];
    js_dual_observation_encode(&o, enc2);
    CHECK(!memcmp(enc, enc2, len), "encode: not deterministic");
    o.digest[0] = 1;
    js_dual_observation_encode(&o, enc2);
    CHECK(!memcmp(enc, enc2, len), "encode: digest field leaked into the canonical bytes");
    printf("  encoding: %zu canonical bytes, kind 0x%02x version %u, domain \"%s\"\n",
           len, JS_DUAL_OBS_KIND, JS_DUAL_OBS_VERSION, JS_DUAL_OBS_DOMAIN);
}

/* ---- observer lifecycle ------------------------------------------------------- */

static void check_lifecycle(void) {
    Scenario sc = { "lifecycle", costs_plain, 8, 2, 0, 1, { { 1, 2, 0 } }, 0, 0 };
    JsSpace s;
    uint32_t root;
    space_open(&s, &sc);
    program(&s, &sc, &root);
    CHECK(s.dual_observer == NULL && s.dual_ctx == NULL && s.dual_seq == 0, "lifecycle: fresh space has an observer");
    JsPolicyReport rep = { 0 };
    CHECK(js_forge_enforce(&s, s.stats.resident_bytes / 2, &rep) == JS_OK, "lifecycle: enforce");
    CHECK(s.dual_seq == 0, "lifecycle: seq advanced without an observer");
    Rec r = { 0 }; r.next_seq = 1;
    js_dual_set_observer(&s, record, &r);
    for (uint32_t b = 0; b < JS_MAX_BRANCHES; b++) {
        JsBranch *br = s.branches[b];
        if (!br) continue;
        for (uint32_t i = 0; i < br->n_units; i++) js_real_restore(&s, br->units[i]);
    }
    memset(&rep, 0, sizeof rep);
    CHECK(js_forge_enforce(&s, s.stats.resident_bytes / 2, &rep) == JS_OK, "lifecycle: enforce observed");
    CHECK(r.n == rep.considered && s.dual_seq == r.n, "lifecycle: %zu records, %llu considered, seq %llu",
          r.n, U(rep.considered), U(s.dual_seq));
    js_dual_set_observer(&s, NULL, &r);
    CHECK(s.dual_observer == NULL && s.dual_ctx == NULL, "lifecycle: clearing the observer left ctx behind");
    size_t before = r.n;
    for (uint32_t b = 0; b < JS_MAX_BRANCHES; b++) {
        JsBranch *br = s.branches[b];
        if (!br) continue;
        for (uint32_t i = 0; i < br->n_units; i++) js_real_restore(&s, br->units[i]);
    }
    CHECK(js_forge_enforce(&s, s.stats.resident_bytes / 2, NULL) == JS_OK, "lifecycle: enforce after clear");
    CHECK(r.n == before, "lifecycle: records emitted after the observer was cleared");
    uint64_t seq = s.dual_seq;
    rec_free(&r);
    js_space_destroy(&s);
    printf("  lifecycle: observer set, counted, cleared; seq %llu\n", U(seq));
}

/* ---- main ---------------------------------------------------------------------- */

int main(void) {
    printf("DUAL-3a jspace.residency tap: parity%s\n",
#ifdef DUAL_TAP_HOSTILE_TEST
           " [HOSTILE NEGATIVE CONTROL: the observer mutates the space; this run must FAIL]"
#else
           ""
#endif
    );
    check_encoding();
    check_lifecycle();
    check_extraction();
    check_cases();

    const Scenario scenarios[] = {
        /* name, costs, fanout, depth, max_spill, rounds, budgets {num, den, add}, rep NULL, tamper */
        { "under-budget",        costs_plain, 16,  4, 0, 1, { { 1000, 1, 0 } }, 0, 0 },
        { "soft-hot-arena",      costs_plain, 32,  4, 0, 2, { { 3, 2, 0 }, { 3, 2, 0 } }, 0, 0 },
        { "over-total-capacity", costs_plain, 32,  4, 0, 2, { { 1, 2, 0 }, { 1, 4, 0 } }, 0, 0 },
        { "ties",                costs_tie,   32,  4, 0, 2, { { 1, 2, 0 }, { 3, 2, 0 } }, 0, 0 },
        { "exactly-at-budget",   costs_plain, 32,  4, 0, 1, { { 1, 1, 0 } }, 0, 0 },
        { "one-byte-below",      costs_plain, 32,  4, 0, 1, { { 1, 1, +1 } }, 0, 0 },
        { "one-byte-above",      costs_plain, 32,  4, 0, 1, { { 1, 1, -1 } }, 0, 0 },
        { "spill-exhaustion",    costs_spill, 32,  4, 2 * UNIT, 2, { { 1, 2, 0 }, { 1, 2, 0 } }, 0, 0 },
        { "evict-drain",         costs_plain, 32,  4, 0, 3, { { 0, 1, 0 }, { 0, 1, 0 }, { 1, 2, 0 } }, 0, 0 },
        { "fanout-500",          costs_plain, 500, 4, 0, 3, { { 3, 2, 0 }, { 1, 2, 0 }, { 1, 8, 0 } }, 0, 0 },
        { "rep-null",            costs_plain, 32,  4, 0, 2, { { 1, 2, 0 }, { 3, 2, 0 } }, 1, 0 },
        { "record-tamper",       costs_plain, 32,  4, 0, 2, { { 1, 2, 0 }, { 3, 2, 0 } }, 0, 1 },
    };
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) run_scenario(&scenarios[i]);

    printf("DUAL_3A_JSPACE_TAP_PARITY=%s (failures=%d)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
