/* PD-0 G0 harness self-test (spec section 11 step 1) plus the world rules of
 * section 2.3 and the SplitMix64 constant check of section 3:
 *  - describe / PD0REC1 / PDLAW1 records round-trip byte for byte;
 *  - the hash chain verifies; a flipped byte, an unknown version and a broken
 *    chain are refused;
 *  - reproducibility: same (level, seed, requests) twice gives identical
 *    record bytes, in-process and through a forked world process over pipes;
 *  - STAND_IN guard: out-of-range step or reset -> REFUSED_RANGE, state kept,
 *    still charged; OUT_OF_BOUNDS hides the post-step state and ends the
 *    episode; EPISODE_END at episode_max_steps (20 for L0, 100 otherwise);
 *    BUDGET_EXHAUSTED once either budget is spent; one channel per step;
 *  - pd0_splitmix64 equals turing_splitmix64 (src/turing/history_selector.c)
 *    for 10^5 outputs from 16 seeds: the "record the check" of section 3;
 *  - NULL world: observations do not depend on the intervention or state.
 * Prints PHYSICS0_G0: PASS|FAIL. Links the world (harness side). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PD0_WORLD_IMPL 1
#include "physics0/pd0_world.h"
#include "physics0/pd0_guard.h"
#include "turing/field.h"
#include "turing/select.h"

/* history_selector.c pulls two symbols from the turing core that this test
 * never reaches (ranking); stub them so only turing_splitmix64 is exercised. */
int turing_digest_eq(const turing_digest *a, const turing_digest *b) { (void)a; (void)b; abort(); }
uint64_t turing_ev_cost(const turing_evidence *e, int pack) { (void)e; (void)pack; abort(); }

static unsigned long failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_wire(void) {
    pd0_desc d, d2;
    pd0_gen_desc(PD0_L3, &d);
    uint8_t buf[PD0_DESC_MAX];
    size_t n = pd0_desc_encode(&d, buf, sizeof buf);
    CHECK(n == 8 + 2 + 8 + 32 + 16 * 4 + 12 && memcmp(buf, "PD0DESC2", 8) == 0, "desc size %zu, magic PD0DESC2", n);
    CHECK(pd0_desc_decode(buf, n, &d2) == 0 && memcmp(&d, &d2, sizeof d) == 0, "desc round trip");
    CHECK(pd0_desc_decode(buf, n - 1, &d2) != 0, "short desc refused");
    pd0_rec r, r2;
    memset(&r, 0, sizeof r);
    r.n_obs = 2; r.kind = PD0_KIND_STEP; r.channel = 0; r.seq = 7; r.episode = 1; r.step_in_episode = 3;
    r.time_micro = 150000; r.requested = -123456; r.applied = -123456;
    r.before[0] = 1; r.before[1] = -2; r.after[0] = 3; r.after[1] = -4;
    memset(r.prev_hash, 0xAB, PD0_HASH);
    uint8_t rb[PD0_REC_MAX];
    size_t rn = pd0_rec_encode(&r, rb, sizeof rb);
    CHECK(rn == PD0_REC_SIZE(2) && rn == 152, "rec size %zu", rn);
    CHECK(pd0_rec_decode(rb, rn, &r2) == 0 && memcmp(&r, &r2, sizeof r) == 0, "rec round trip");
    rb[9] = 2; /* version 2 */
    CHECK(pd0_rec_decode(rb, rn, &r2) != 0, "unknown version refused");
    rb[9] = 0;
    rb[60] ^= 1; /* flip a byte inside vars_before */
    CHECK(pd0_rec_decode(rb, rn, &r2) != 0, "tampered record refused");
    rb[60] ^= 1;
    CHECK(pd0_rec_decode(rb, rn, &r2) == 0, "restored record accepted");
    /* law record */
    pd0_law l, l2;
    memset(&l, 0, sizeof l);
    l.state = PD0_LAW_PROVISIONAL;
    l.rel.n_vars = 2; l.rel.n_channels = 1; l.rel.n_eq = 2;
    l.rel.eq[0].target = 0; l.rel.eq[0].n_terms = 1; l.rel.eq[0].t[0].coef = 50000; l.rel.eq[0].t[0].ex[1] = 1;
    l.rel.eq[1].target = 1; l.rel.eq[1].n_terms = 2; l.rel.eq[1].t[0].coef = -200000; l.rel.eq[1].t[0].ex[0] = 1;
    l.rel.eq[1].t[1].coef = 50000; l.rel.eq[1].t[1].ex[PD0_MAX_VARS] = 1;
    l.dom.n_observations = 2400; l.dom.n_episodes = 60; l.dom.reset_min = -2000000; l.dom.reset_max = 2000000;
    l.dom.episode_len = 100; l.confidence_ppm = pd0_confidence_ppm(34, 0); l.n_experiments = 1; l.exp[0].kind = 1;
    l.n_refutations = 1; l.eps_micro = 20000;
    uint8_t lb[PD0_LAW_MAX];
    size_t ln = pd0_law_encode(&l, lb, sizeof lb);
    CHECK(ln > 0, "law encode");
    CHECK(pd0_confidence_ppm(34, 0) == 972222, "confidence rule example: %u", pd0_confidence_ppm(34, 0));
    CHECK(pd0_relation_description_bits(&l.rel) == 3 * (8 * 3 + 24), "description_bits %u", pd0_relation_description_bits(&l.rel));
    CHECK(pd0_law_decode(lb, ln, &l2) == 0, "law decode");
    CHECK(memcmp(&l.rel, &l2.rel, sizeof l.rel) == 0 && l2.n_refutations == 1 && l2.confidence_ppm == 972222 &&
              memcmp(l.law_id, l2.law_id, PD0_HASH) == 0, "law round trip");
    lb[11 + PD0_HASH + 3] ^= 1; /* description_bits byte */
    CHECK(pd0_law_decode(lb, ln, &l2) != 0, "law with wrong description_bits refused");
    lb[11 + PD0_HASH + 3] ^= 1;
    lb[8] = 2;
    CHECK(pd0_law_decode(lb, ln, &l2) != 0, "law version 2 refused");
    lb[8] = 1;
    lb[ln - 3] ^= 1; /* claim text */
    CHECK(pd0_law_decode(lb, ln, &l2) != 0, "law with tampered claim refused (law_id)");
    char js[2048];
    pd0_law_json(&l, js, sizeof js);
    CHECK(strstr(js, "\"monomial\":\"c0\"") && strstr(js, "PROVISIONAL_LAW"), "law json projection: %s", js);
    char claim[512];
    pd0_claim_text(&l, claim, sizeof claim);
    CHECK(!strstr(claim, "always") && !strstr(claim, "true") && !strstr(claim, "universally"), "forbidden words");
}

/* drive a channel with a fixed script; append record bytes into out */
static size_t script(pd0_chan *c, int level, uint8_t *out, size_t cap, pd0_rec *last) {
    pd0_desc d;
    if (pd0_chan_describe(c, &d) != 0) return 0;
    size_t n = 0;
    pd0_rng r;
    pd0_stream(&r, 77, "script");
    pd0_rec rec;
    for (int ep = 0; ep < 3; ep++) {
        int64_t reset[PD0_MAX_OBS];
        for (int v = 0; v < d.n_obs; v++) {
            int64_t lo, hi;
            pd0_gen_reset_box(level, v, &lo, &hi); /* per-variable reset box (spec 4 rev 3) */
            reset[v] = pd0_const(&r, lo, hi);
        }
        if (pd0_chan_reset(c, d.n_obs, reset, &rec) != 0) return 0;
        n += pd0_rec_encode(&rec, out + n, cap - n);
        for (int i = 0; i < 25; i++) {
            uint8_t ch = (uint8_t)(pd0_next(&r) % d.n_channels);
            if (pd0_chan_step(c, ch, pd0_const(&r, d.chan_min[ch], d.chan_max[ch]), &rec) != 0) return 0;
            n += pd0_rec_encode(&rec, out + n, cap - n);
            if (rec.status != PD0_OK) break;
        }
    }
    *last = rec;
    return n;
}

static int chain_ok(const uint8_t *buf, size_t n) {
    pd0_rec prev, cur;
    int have = 0;
    size_t off = 0;
    while (off < n) {
        if (n - off < 12) return 0;
        size_t sz = PD0_REC_SIZE(buf[off + 10]);
        if (pd0_rec_decode(buf + off, sz, &cur) != 0) return 0;
        if (!pd0_rec_chained(&cur, have ? &prev : NULL)) return 0;
        prev = cur; have = 1; off += sz;
    }
    return 1;
}

static void t_repro_and_process(void) {
    for (int level = PD0_L0; level <= PD0_LEVEL_NULL; level++) {
        static uint8_t a[64 * 1024], b[64 * 1024], p[64 * 1024];
        pd0_world w1, w2;
        pd0_rec la, lb, lp;
        pd0_world_init(&w1, level, 5);
        pd0_world_init(&w2, level, 5);
        pd0_chan c1 = {pd0_world_chan, &w1, -1, -1}, c2 = {pd0_world_chan, &w2, -1, -1};
        size_t na = script(&c1, level, a, sizeof a, &la), nb = script(&c2, level, b, sizeof b, &lb);
        CHECK(na > 0 && na == nb && memcmp(a, b, na) == 0, "%s: same seed, same bytes (%zu vs %zu)", pd0_gen_level_name(level), na, nb);
        CHECK(chain_ok(a, na), "%s: chain verifies", pd0_gen_level_name(level));
        /* break the chain: swap two records */
        if (na >= 2 * PD0_REC_SIZE(2)) {
            memcpy(b, a, na);
            size_t sz = PD0_REC_SIZE(b[10]);
            uint8_t tmp[PD0_REC_MAX];
            memcpy(tmp, b, sz); memcpy(b, b + sz, sz); memcpy(b + sz, tmp, sz);
            CHECK(!chain_ok(b, na), "%s: reordered chain refused", pd0_gen_level_name(level));
        }
        /* the same script through a forked world process over pipes */
        int to_world[2], from_world[2];
        CHECK(pipe(to_world) == 0 && pipe(from_world) == 0, "pipes");
        pid_t pid = fork();
        if (pid == 0) {
            close(to_world[1]); close(from_world[0]);
            pd0_world w;
            pd0_world_init(&w, level, 5);
            int rc = pd0_world_serve(&w, to_world[0], from_world[1]);
            _exit(rc == 0 ? 0 : 1);
        }
        close(to_world[0]); close(from_world[1]);
        pd0_chan cp = {NULL, NULL, from_world[0], to_world[1]};
        size_t np = script(&cp, level, p, sizeof p, &lp);
        close(to_world[1]); close(from_world[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: world process exit %d", pd0_gen_level_name(level), status);
        CHECK(np == na && memcmp(a, p, na) == 0, "%s: pipe transport gives identical bytes", pd0_gen_level_name(level));
        /* a different seed gives different bytes (constants or noise or null) except L0, which has no constants */
        pd0_world w3;
        pd0_world_init(&w3, level, 6);
        pd0_chan c3 = {pd0_world_chan, &w3, -1, -1};
        size_t nc = script(&c3, level, b, sizeof b, &lb);
        if (level != PD0_L0) CHECK(nc != na || memcmp(a, b, na) != 0, "%s: seed changes the run", pd0_gen_level_name(level));
    }
}

static void t_rules(void) {
    pd0_world w;
    pd0_rec r;
    pd0_world_init(&w, PD0_L1, 3);
    int64_t reset[2] = {1000000, -500000};
    pd0_world_reset(&w, reset, 2, &r);
    CHECK(r.status == PD0_OK && r.kind == PD0_KIND_RESET && r.before[0] == 1000000 && r.after[1] == -500000 && r.seq == 0 &&
              r.step_in_episode == 0, "reset record");
    pd0_world_step(&w, 0, 3000000, &r); /* above chan_max 2.0 */
    CHECK(r.status == PD0_REFUSED_RANGE && r.applied == 0 && r.requested == 3000000 && r.before[0] == 1000000 &&
              r.after[0] == 1000000 && w.steps_used == 1, "refused step keeps state and is charged");
    pd0_world_step(&w, 1, 0, &r); /* unknown channel */
    CHECK(r.status == PD0_REFUSED_RANGE && w.steps_used == 2, "unknown channel refused and charged");
    pd0_world_step(&w, PD0_CH_NONE, 0, &r);
    CHECK(r.status == PD0_OK && r.applied == 0 && r.after[0] == 1000000 + pd0_mul(-500000, 50000), "passive step: s0 += s1*dt");
    pd0_world_step(&w, 0, 2000000, &r);
    CHECK(r.status == PD0_OK && r.applied == 2000000, "max bound inclusive");
    uint32_t used = w.episodes_used;
    int64_t bad[2] = {2500000, 0};
    pd0_world_reset(&w, bad, 2, &r);
    CHECK(r.status == PD0_REFUSED_RANGE && w.episodes_used == used + 1, "refused reset charged an episode");
    pd0_world_step(&w, 0, 0, &r);
    CHECK(r.status == PD0_BUDGET_EXHAUSTED, "no episode open after a refused reset: step refused as exhausted");
    /* episode end at 100 for L1, 50 for L0 */
    pd0_world_reset(&w, reset, 2, &r);
    int ends = 0;
    for (int i = 0; i < 100; i++) { pd0_world_step(&w, PD0_CH_NONE, 0, &r); ends += r.status == PD0_EPISODE_END; }
    CHECK(ends == 1 && r.status == PD0_EPISODE_END && r.step_in_episode == 99 && r.time_micro == 99 * 50000, "L1 episode ends at step 100");
    pd0_world w0;
    pd0_world_init(&w0, PD0_L0, 1);
    int64_t z[2] = {0, 0};
    pd0_world_reset(&w0, z, 2, &r);
    for (int i = 0; i < 20; i++) pd0_world_step(&w0, PD0_CH_NONE, 0, &r);
    CHECK(r.status == PD0_EPISODE_END && w0.desc.episode_max_steps == 20 && w0.desc.chan_max[0] == 100000 && w0.desc.budget_episodes == 150 &&
              w0.desc.budget_steps == 3000,
          "L0 rev 3 describe: 20 steps, pushes 0.1, 3000 steps = 150 episodes");
    /* OUT_OF_BOUNDS hides the post-step state */
    int64_t fast[2] = {2000000, 200000}; /* corner of the L0 reset box (s0 [-2,2], s1 [-0.2,0.2]) */
    pd0_world_reset(&w0, fast, 2, &r);
    int saw_oob = 0;
    for (int i = 0; i < 20 && !saw_oob; i++) {
        pd0_world_step(&w0, 0, 100000, &r);
        if (r.status == PD0_OUT_OF_BOUNDS) {
            saw_oob = 1;
            CHECK(r.after[0] == r.before[0] && r.after[1] == r.before[1] && r.before[0] <= PD0_BOUND, "OOB reveals nothing");
            pd0_world_step(&w0, 0, 0, &r);
            CHECK(r.status == PD0_BUDGET_EXHAUSTED, "episode closed after OOB");
        }
    }
    CHECK(saw_oob, "L0 from (2, 0.2) with +0.1 pushes leaves +-10 within 20 steps");
    /* budget: L0 150 episodes */
    pd0_world wb;
    pd0_world_init(&wb, PD0_L0, 2);
    int exhausted = 0;
    for (int e = 0; e < 160; e++) { pd0_world_reset(&wb, z, 2, &r); exhausted += r.status == PD0_BUDGET_EXHAUSTED; }
    CHECK(exhausted == 10, "episode budget: %d exhausted of 160 resets", exhausted);
    /* guard functions directly */
    CHECK(pd0_guard_in_box(PD0_BOUND) && !pd0_guard_in_box(PD0_BOUND + 1) && pd0_guard_in_box(-PD0_BOUND), "box edges");
}

static void t_splitmix(void) {
    unsigned long mism = 0;
    for (uint64_t seed = 0; seed < 16; seed++) {
        uint64_t a = seed * 0x9E3779B97F4A7C15ull + 12345, b = a;
        for (int i = 0; i < 100000; i++) mism += pd0_splitmix64(&a) != turing_splitmix64(&b);
    }
    CHECK(mism == 0, "pd0_splitmix64 vs turing_splitmix64: %lu mismatches", mism);
    pd0_rng r;
    pd0_stream(&r, 0, "const");
    CHECK(r.s == pd0_fnv1a64("const"), "stream seeding is seed XOR fnv1a64(tag)");
    CHECK(pd0_fnv1a64("") == 0xcbf29ce484222325ull, "fnv1a64 offset basis");
    /* uniform in [0, 1e6], const draw inside [lo, hi] */
    int64_t mn = 1 << 30, mx = -1;
    for (int i = 0; i < 100000; i++) { int64_t u = pd0_uniform(&r); if (u < mn) mn = u; if (u > mx) mx = u; }
    CHECK(mn >= 0 && mx <= 1000000, "uniform range %lld..%lld", (long long)mn, (long long)mx);
    for (int i = 0; i < 1000; i++) { int64_t c = pd0_const(&r, -3000000, 7000000); CHECK(c >= -3000000 && c <= 7000000, "const draw range"); }
    /* Irwin-Hall noise: mean ~0, sd ~sigma (loose check on 200k draws) */
    double s = 0, sq = 0;
    for (int i = 0; i < 200000; i++) { double e = (double)pd0_noise(&r, 20000); s += e; sq += e * e; }
    double mean = s / 200000, sd = sqrt(sq / 200000 - mean * mean);
    CHECK(mean > -300 && mean < 300 && sd > 19000 && sd < 21000, "noise mean %.1f sd %.1f (sigma 20000)", mean, sd);
}

static void t_null_world(void) {
    /* the same seed with two different interventions yields the same observations */
    pd0_world a, b;
    pd0_rec ra, rb;
    pd0_world_init(&a, PD0_LEVEL_NULL, 101);
    pd0_world_init(&b, PD0_LEVEL_NULL, 101);
    int64_t z[2] = {0, 0}, o[2] = {1500000, -1500000};
    pd0_world_reset(&a, z, 2, &ra);
    pd0_world_reset(&b, o, 2, &rb);
    int same = 1;
    for (int i = 0; i < 50; i++) {
        pd0_world_step(&a, 0, 2000000, &ra);
        pd0_world_step(&b, 0, -2000000, &rb);
        same &= ra.after[0] == rb.after[0] && ra.after[1] == rb.after[1];
    }
    CHECK(same, "NULL world: observations independent of state and intervention");
    CHECK(a.desc.budget_steps == 3000 && a.desc.n_obs == 2, "NULL world has the L2 interface and budget");
}

/* revision 3 (physics 5bd2b04): per-variable boxes, L0 s1 box, L4 box, L6 order and redraw rule */
static void t_rev3(void) {
    pd0_rec r;
    int64_t lo, hi;
    /* reset boxes: spec 4 table notes */
    pd0_gen_reset_box(PD0_L0, 0, &lo, &hi); CHECK(lo == -2000000 && hi == 2000000, "L0 s0 box [-2,2]");
    pd0_gen_reset_box(PD0_L0, 1, &lo, &hi); CHECK(lo == -200000 && hi == 200000, "L0 s1 box [-0.2,0.2]");
    pd0_gen_reset_box(PD0_L4, 0, &lo, &hi); CHECK(lo == -1000000 && hi == 1000000, "L4 s0 box [-1,1]");
    pd0_gen_reset_box(PD0_L4, 1, &lo, &hi); CHECK(lo == -1000000 && hi == 1000000, "L4 s1 box [-1,1]");
    pd0_gen_reset_box(PD0_L2, 1, &lo, &hi); CHECK(lo == -2000000 && hi == 2000000, "L2 box [-2,2]");
    /* scoring boxes: in = reset box, extrap = 1.5 x */
    pd0_gen_score_box(PD0_L0, 1, 1, &lo, &hi); CHECK(lo == -300000 && hi == 300000, "L0 s1 extrap 1.5x = 0.3");
    pd0_gen_score_box(PD0_L4, 0, 1, &lo, &hi); CHECK(lo == -1500000 && hi == 1500000, "L4 extrap 1.5x = 1.5");
    pd0_gen_score_box(PD0_L1, 0, 1, &lo, &hi); CHECK(lo == -3000000 && hi == 3000000, "L1 extrap 3");
    pd0_gen_score_box(PD0_L1, 0, 0, &lo, &hi); CHECK(lo == -2000000 && hi == 2000000, "L1 in-box 2");
    /* the world enforces the per-variable box: L0 s1 = 0.3 refused, charged, no episode open */
    pd0_world w;
    pd0_world_init(&w, PD0_L0, 1);
    int64_t bad[2] = {0, 300000}, ok[2] = {1999999, -200000};
    pd0_world_reset(&w, bad, 2, &r);
    CHECK(r.status == PD0_REFUSED_RANGE && w.episodes_used == 1 && !w.in_episode, "L0 s1 outside [-0.2,0.2]: refused, charged, no episode");
    pd0_world_step(&w, PD0_CH_NONE, 0, &r);
    CHECK(r.status == PD0_BUDGET_EXHAUSTED, "step after a refused reset has no open episode");
    pd0_world_reset(&w, ok, 2, &r);
    CHECK(r.status == PD0_OK && w.in_episode, "box edge accepted");
    pd0_world w4;
    pd0_world_init(&w4, PD0_L4, 1);
    int64_t b4[2] = {1000001, 0}, g4[2] = {-1000000, 1000000};
    pd0_world_reset(&w4, b4, 2, &r);
    CHECK(r.status == PD0_REFUSED_RANGE, "L4 s0 = 1.000001 refused");
    pd0_world_reset(&w4, g4, 2, &r);
    CHECK(r.status == PD0_OK, "L4 corner (-1, 1) accepted");
    CHECK(w4.desc.reset_min[0] == -1000000 && w4.desc.reset_max[1] == 1000000, "L4 describe box [-1,1]");
    /* L6 declared ranges and draw order k, w, m, q */
    const pd0_crange *t;
    CHECK(pd0_gen_const_table(PD0_L6, &t) == 4 && !strcmp(t[0].name, "k") && !strcmp(t[1].name, "w") && !strcmp(t[2].name, "m") && !strcmp(t[3].name, "q") &&
              t[0].lo == 3000000 && t[0].hi == 5000000 && t[1].lo == 500000 && t[1].hi == 1000000 && t[2].lo == 1000000 && t[2].hi == 2000000 &&
              t[3].lo == 1000000 && t[3].hi == 2000000, "L6 table k[3,5] w[0.5,1] m[1,2] q[1,2] in draw order");
    int saw_redraw = 0, max_redraw = 0;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        pd0_gen g;
        CHECK(pd0_gen_init(&g, PD0_L6, seed) == 0 && !g.invalid, "L6 seed %llu valid", (unsigned long long)seed);
        int in = 1;
        for (int i = 0; i < 4; i++) in &= g.k[i] >= t[i].lo && g.k[i] <= t[i].hi;
        CHECK(in, "L6 seed %llu constants inside the table", (unsigned long long)seed);
        CHECK(10 * g.k[2] * g.k[3] <= 7 * g.k[0] * g.k[1], "L6 seed %llu satisfies m*q/w <= 0.7 k", (unsigned long long)seed);
        saw_redraw |= g.redraws > 0;
        if (g.redraws > max_redraw) max_redraw = g.redraws;
        /* replay by hand: draw k, w, m, q, then redraw m then q while the rule fails */
        pd0_rng c;
        pd0_stream(&c, seed, "const");
        int64_t k = pd0_const(&c, t[0].lo, t[0].hi), ww = pd0_const(&c, t[1].lo, t[1].hi), m = pd0_const(&c, t[2].lo, t[2].hi), q = pd0_const(&c, t[3].lo, t[3].hi);
        int n = 0;
        while (10 * m * q > 7 * k * ww) { m = pd0_const(&c, t[2].lo, t[2].hi); q = pd0_const(&c, t[3].lo, t[3].hi); n++; }
        CHECK(g.k[0] == k && g.k[1] == ww && g.k[2] == m && g.k[3] == q && g.redraws == n, "L6 seed %llu follows the spec draw order and redraw rule", (unsigned long long)seed);
    }
    CHECK(saw_redraw, "some development seed exercises the redraw rule (max redraws %d)", max_redraw);
    /* cap: a seed that needs a redraw is invalid with cap 0, recorded as such; valid with the spec cap */
    for (uint64_t seed = 1; seed <= 200; seed++) {
        pd0_gen g, g0;
        pd0_gen_init(&g, PD0_L6, seed);
        if (!g.redraws) continue;
        CHECK(pd0_gen_init_cap(&g0, PD0_L6, seed, 0) != 0 && g0.invalid && g0.redraws == 0, "redraw cap reached: seed invalid");
        pd0_world wi;
        CHECK(pd0_world_init(&wi, PD0_L6, seed) == 0, "same seed valid at the spec cap 1000");
        break;
    }
    /* the true relation reads the new draw order: m in eq s1, w and q in eq h */
    pd0_gen g;
    pd0_relation rel;
    pd0_gen_init(&g, PD0_L6, 3);
    pd0_gen_true_relation(&g, 50000, &rel);
    CHECK(rel.eq[1].t[1].coef == pd0_mul(g.k[2], 50000) && rel.eq[2].t[0].coef == -pd0_mul(g.k[1], 50000) && rel.eq[2].t[1].coef == pd0_mul(g.k[3], 50000),
          "L6 relation: m = 3rd draw, w = 2nd, q = 4th");
}

/* PD0DESC2: per-variable reset bounds, PD0DESC1 refused, golden bytes, per-variable guard (spec amendment pending, lead drafting) */
static void t_desc2(void) {
    pd0_desc d, d2;
    uint8_t buf[PD0_DESC_MAX], v1[PD0_DESC_MAX];
    pd0_gen_desc(PD0_L0, &d);
    size_t n = pd0_desc_encode(&d, buf, sizeof buf);
    /* golden bytes of the L0 describe record: magic, n_obs 2, n_channels 1, dt 1.0, chan -0.1/0.1, reset_min s0 s1, reset_max s0 s1, 20, 3000, 150 */
    static const uint8_t gold[] = {
        'P','D','0','D','E','S','C','2', 2, 1,
        0x40,0x42,0x0F,0,0,0,0,0,                 /* dt 1000000 */
        0x60,0x79,0xFE,0xFF,0xFF,0xFF,0xFF,0xFF,  /* chan_min -100000 */
        0xA0,0x86,0x01,0,0,0,0,0,                 /* chan_max 100000 */
        0x80,0x7B,0xE1,0xFF,0xFF,0xFF,0xFF,0xFF,  /* reset_min s0 -2000000 */
        0xC0,0xF2,0xFC,0xFF,0xFF,0xFF,0xFF,0xFF,  /* reset_min s1 -200000 */
        0x80,0x84,0x1E,0,0,0,0,0,                 /* reset_max s0 2000000 */
        0x40,0x0D,0x03,0,0,0,0,0,                 /* reset_max s1 200000 */
        20,0,0,0, 0xB8,0x0B,0,0, 150,0,0,0};
    CHECK(n == sizeof gold, "L0 describe length %zu", n);
    CHECK(n == sizeof gold && memcmp(buf, gold, n) == 0, "L0 describe golden bytes");
    CHECK(pd0_desc_decode(buf, n, &d2) == 0 && memcmp(&d, &d2, sizeof d) == 0 && d2.reset_min[1] == -200000 && d2.reset_max[0] == 2000000 &&
              d2.reset_max[1] == 200000, "PD0DESC2 round trip keeps per-variable bounds");
    /* mutation: the old magic is refused with the explicit code, and nothing is decoded */
    memcpy(v1, buf, n);
    v1[7] = '1';
    memset(&d2, 0xAA, sizeof d2);
    CHECK(pd0_desc_decode(v1, n, &d2) == PD0_DESC_REFUSED_V1, "PD0DESC1 refused with PD0_DESC_REFUSED_V1");
    CHECK(pd0_desc_decode(v1, 8 + 2 + 8 + 16 + 16 + 12, &d2) == PD0_DESC_REFUSED_V1, "PD0DESC1 of the old length also refused");
    buf[7] = '3';
    CHECK(pd0_desc_decode(buf, n, &d2) == -1, "unknown magic refused");
    buf[7] = '2';
    CHECK(pd0_desc_decode(buf, n - 1, &d2) != 0, "short PD0DESC2 refused");
    /* guard mutation: L0 s1 = 0.3 refused, s0 anywhere in [-2,2] accepted */
    int64_t ok[2] = {-2000000, 200000}, ok2[2] = {2000000, -200000}, bad1[2] = {0, 300000}, bad0[2] = {2000001, 0};
    CHECK(pd0_guard_reset_ok(&d, ok, 2) && pd0_guard_reset_ok(&d, ok2, 2), "s0 in [-2,2] with s1 in [-0.2,0.2] accepted");
    CHECK(!pd0_guard_reset_ok(&d, bad1, 2), "s1 = 0.3 refused (per-variable violation on L0)");
    CHECK(!pd0_guard_reset_ok(&d, bad0, 2), "s0 = 2.000001 refused");
    /* the same through a world over the channel: the describe the learner reads carries the s1 box */
    pd0_world w;
    pd0_chan c = {pd0_world_chan, &w, -1, -1};
    pd0_world_init(&w, PD0_L0, 1);
    CHECK(pd0_chan_describe(&c, &d2) == 0 && d2.reset_min[1] == -200000 && d2.reset_max[1] == 200000 && d2.reset_max[0] == 2000000,
          "learner-side describe shows both boxes");
}

int main(void) {
    t_wire();
    t_rules();
    t_splitmix();
    t_null_world();
    t_rev3();
    t_desc2();
    t_repro_and_process();
    printf("checks=%lu failures=%lu\n", checks, failures);
    printf("PHYSICS0_G0: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
