/*
 * test_osc_model.c -- OSC-0B exit gate (OMEGA_SYSTEMS_CORE_CODE_AUDIT.md
 * II.11, III.8): the II.11 executable model must reject every named invalid
 * transition, with the exact name, across >= 10^6 seeded random sequences,
 * with zero false accepts, zero wrong-name rejects and zero false rejects.
 *
 * usage: test_osc_model [seed_hex [sequences]]
 *   default seed 0x05C0B5EED0010001, default sequences 1000000.
 * Final line: OSC0B_MODEL_PASS or OSC0B_MODEL_FAIL. A run with fewer than
 * 10^6 sequences (e.g. under ASan) can at best print OSC0B_MODEL_SMOKE_PASS.
 */
#include "osc_model.h"
#include "osc_model_gen.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GATE_MIN_SEQUENCES 1000000ull
#define GATE_MIN_PER_CLASS 10000ull
#define DEFAULT_SEED       0x05C0B5EED0010001ull

static int g_unit_fail, g_unit_cases;

#define EV(...) ((OscModelEvent){ __VA_ARGS__ })

/* Run evs[0..n-1]; all but the last must be accepted; the last must give
 * `last` (OSC_REJ_NONE = accepted too). */
static void seq_case(const char *name, uint64_t base, uint64_t max,
                     const OscModelEvent *evs, int n, OscModelReject last)
{
    OscModel m;
    osc_model_init(&m, base, max);
    g_unit_cases++;
    for (int i = 0; i < n; i++) {
        OscModelReject r;
        OscModelVerdict v = osc_model_step(&m, &evs[i], &r);
        OscModelReject want = i == n - 1 ? last : OSC_REJ_NONE;
        if (r != want || (v == OSC_MODEL_ACCEPT) != (r == OSC_REJ_NONE)) {
            printf("UNIT FAIL %s: step %d (%s) got %s want %s\n", name, i,
                   osc_model_event_name(evs[i].kind), osc_model_reject_name(r), osc_model_reject_name(want));
            g_unit_fail++;
            return;
        }
    }
}
#define CASE(name, last, ...) do { const OscModelEvent e_[] = { __VA_ARGS__ }; \
    seq_case(name, 0, UINT64_MAX, e_, (int)(sizeof e_ / sizeof e_[0]), last); } while (0)
#define CASEG(name, base, max, last, ...) do { const OscModelEvent e_[] = { __VA_ARGS__ }; \
    seq_case(name, base, max, e_, (int)(sizeof e_ / sizeof e_[0]), last); } while (0)

#define ALLOC(o, r)      EV(.kind = OSC_EV_ALLOC, .obj = (o), .region = (r))
#define MOVE(o, d)       EV(.kind = OSC_EV_MOVE, .obj = (o), .obj2 = (d))
#define BSH(o, b, v)     EV(.kind = OSC_EV_BORROW_SHARED, .obj = (o), .borrow = (b), .via = (v))
#define BMUT(o, b, v)    EV(.kind = OSC_EV_BORROW_MUT, .obj = (o), .borrow = (b), .via = (v))
#define END(b)           EV(.kind = OSC_EV_END_BORROW, .borrow = (b))
#define RD(o, v)         EV(.kind = OSC_EV_USE_READ, .obj = (o), .via = (v))
#define WR(o, v)         EV(.kind = OSC_EV_USE_WRITE, .obj = (o), .via = (v))
#define REL(o)           EV(.kind = OSC_EV_RELEASE, .obj = (o))
#define ROPEN(r)         EV(.kind = OSC_EV_REGION_OPEN, .region = (r))
#define RDEST(r)         EV(.kind = OSC_EV_REGION_DESTROY, .region = (r))
#define SALLOC(s, h, rt) EV(.kind = OSC_EV_SLOT_ALLOC, .slot = (s), .handle = (h), .rights = (rt))
#define SFREE(h)         EV(.kind = OSC_EV_SLOT_FREE, .handle = (h))
#define SFREEG(s, g)     EV(.kind = OSC_EV_SLOT_FREE, .slot = (s), .gen = (g))
#define DERIVE(h, p, rt) EV(.kind = OSC_EV_HANDLE_DERIVE, .handle = (h), .handle2 = (p), .rights = (rt))
#define HUSE(h, rt)      EV(.kind = OSC_EV_HANDLE_USE, .handle = (h), .rights = (rt))
#define PERSIST(h, s)    EV(.kind = OSC_EV_PERSIST, .handle = (h), .semid = (s))
#define DUR(k, i)        EV(.kind = OSC_EV_DURABLE_WRITE, .vkind = (k), .id = (i))
#define CW(c)            EV(.kind = OSC_EV_CELL_WRITE, .cell = (c))
#define CPUB(c, o)       EV(.kind = OSC_EV_CELL_PUBLISH, .cell = (c), .order = (o))
#define COBS(c, o)       EV(.kind = OSC_EV_CELL_OBSERVE, .cell = (c), .order = (o))
#define CRD(c)           EV(.kind = OSC_EV_CELL_READ, .cell = (c))
#define QUI(t, c)        EV(.kind = OSC_EV_QUIESCE, .token = (t), .cell = (c))
#define RECL(c, t)       EV(.kind = OSC_EV_RECLAIM, .cell = (c), .token = (t))
#define REL_ OSC_ORD_RELEASE
#define ACQ_ OSC_ORD_ACQUIRE
#define RLX_ OSC_ORD_RELAXED
#define DRAINED(c, t)    CW(c), CPUB(c, REL_), COBS(c, ACQ_), QUI(t, c)

static void unit_tests(void)
{
    /* ---- valid sequences ---- */
    CASE("valid: owner lifecycle", OSC_REJ_NONE,
         ALLOC(1, 0), WR(1, 0), RD(1, 0), BSH(1, 1, 0), BSH(1, 2, 0), RD(1, 1), RD(1, 0), RD(1, 2),
         BSH(1, 3, 1), END(3), END(1), END(2), BMUT(1, 4, 0), WR(1, 4), RD(1, 4), BMUT(1, 5, 4), WR(1, 5),
         END(5), BSH(1, 6, 4), BSH(1, 7, 4), RD(1, 6), RD(1, 4), END(6), END(7), WR(1, 4), END(4),
         WR(1, 0), MOVE(1, 2), RD(2, 0), MOVE(2, 0), ALLOC(3, 0), REL(3));
    CASE("valid: region destroy releases owners, nothing escapes", OSC_REJ_NONE,
         ROPEN(1), ALLOC(1, 1), ALLOC(2, 1), MOVE(2, 3), BSH(1, 1, 0), RD(1, 1), END(1), REL(1), RDEST(1),
         ALLOC(4, 0), REL(4));
    CASE("valid: region-destroyed object is released", OSC_REJ_USE_AFTER_RELEASE,
         ROPEN(1), ALLOC(1, 1), RDEST(1), RD(1, 0));
    CASE("valid: publish/observe pairs, drain, reclaim", OSC_REJ_NONE,
         CW(1), CPUB(1, REL_), COBS(1, ACQ_), CRD(1), CRD(1), CW(1), CPUB(1, REL_), COBS(1, ACQ_), CRD(1),
         QUI(1, 1), RECL(1, 1), CW(2), CPUB(2, REL_));
    CASEG("valid: generation advances to retirement", 0, 2, OSC_REJ_NONE,
          SALLOC(1, 1, 3), SFREE(1), SALLOC(1, 2, 3), SFREE(2), SALLOC(1, 3, 3), HUSE(3, 1), SFREE(3));
    CASE("valid: handles narrow, persist, durable semid and value", OSC_REJ_NONE,
         SALLOC(1, 1, 0xF), DERIVE(2, 1, 0x5), DERIVE(3, 2, 0x4), HUSE(3, 0x4), HUSE(1, 0xF), PERSIST(3, 1),
         DUR(OSC_VK_SEMID, 1), ALLOC(1, 0), DUR(OSC_VK_OBJECT, 1), BSH(1, 1, 0), DUR(OSC_VK_OBJECT, 1),
         SFREEG(1, 0), SALLOC(1, 4, 1));

    /* ---- 1 use after move ---- */
    CASE("uam: read", OSC_REJ_USE_AFTER_MOVE, ALLOC(1, 0), MOVE(1, 2), RD(1, 0));
    CASE("uam: release", OSC_REJ_USE_AFTER_MOVE, ALLOC(1, 0), MOVE(1, 0), REL(1));
    CASE("uam: borrow", OSC_REJ_USE_AFTER_MOVE, ALLOC(1, 0), MOVE(1, 2), BSH(1, 1, 0));
    CASE("uam: move again", OSC_REJ_USE_AFTER_MOVE, ALLOC(1, 0), MOVE(1, 2), MOVE(1, 3));
    /* ---- 2 use after release ---- */
    CASE("uar: read", OSC_REJ_USE_AFTER_RELEASE, ALLOC(1, 0), REL(1), RD(1, 0));
    CASE("uar: ended borrow", OSC_REJ_USE_AFTER_RELEASE, ALLOC(1, 0), BSH(1, 1, 0), END(1), RD(1, 1));
    CASE("uar: alloc into destroyed region", OSC_REJ_USE_AFTER_RELEASE, ROPEN(1), RDEST(1), ALLOC(1, 1));
    CASE("uar: reclaimed cell", OSC_REJ_USE_AFTER_RELEASE, DRAINED(1, 1), RECL(1, 1), CW(1));
    /* ---- 3 double release ---- */
    CASE("dbl: object", OSC_REJ_DOUBLE_RELEASE, ALLOC(1, 0), REL(1), REL(1));
    CASE("dbl: borrow", OSC_REJ_DOUBLE_RELEASE, ALLOC(1, 0), BSH(1, 1, 0), END(1), END(1));
    CASE("dbl: region", OSC_REJ_DOUBLE_RELEASE, ROPEN(1), RDEST(1), RDEST(1));
    CASE("dbl: cell reclaim", OSC_REJ_DOUBLE_RELEASE, DRAINED(1, 1), RECL(1, 1), RECL(1, 1));
    CASE("dbl: object in destroyed region", OSC_REJ_DOUBLE_RELEASE, ROPEN(1), ALLOC(1, 1), RDEST(1), REL(1));
    /* ---- 4 stale generation ---- */
    CASE("stale: use after free", OSC_REJ_STALE_GENERATION, SALLOC(1, 1, 1), SFREE(1), HUSE(1, 1));
    CASE("stale: slot reused", OSC_REJ_STALE_GENERATION, SALLOC(1, 1, 1), SFREE(1), SALLOC(1, 2, 1), HUSE(1, 0));
    CASE("stale: second free", OSC_REJ_STALE_GENERATION, SALLOC(1, 1, 1), SFREE(1), SFREE(1));
    CASE("stale: explicit gen", OSC_REJ_STALE_GENERATION, SALLOC(1, 1, 1), SFREE(1), SALLOC(1, 2, 1), SFREEG(1, 0));
    CASE("stale: derive", OSC_REJ_STALE_GENERATION, SALLOC(1, 1, 1), SFREE(1), DERIVE(2, 1, 0));
    CASE("stale before rights", OSC_REJ_STALE_GENERATION, SALLOC(1, 1, 1), SFREE(1), HUSE(1, 0xFF));
    /* ---- 5 generation wrap ---- */
    CASEG("wrap: small max", 0, 1, OSC_REJ_GENERATION_WRAP,
          SALLOC(1, 1, 0), SFREE(1), SALLOC(1, 2, 0), SFREE(2), SALLOC(1, 3, 0));
    CASEG("wrap: u64 boundary", UINT64_MAX - 1, UINT64_MAX, OSC_REJ_GENERATION_WRAP,
          SALLOC(1, 1, 0), SFREEG(1, UINT64_MAX - 1), SALLOC(1, 2, 0), SFREEG(1, UINT64_MAX), SALLOC(1, 0, 0));
    CASEG("wrap: retired handle is stale", UINT64_MAX, UINT64_MAX, OSC_REJ_STALE_GENERATION,
          SALLOC(1, 1, 0), SFREE(1), HUSE(1, 0));
    /* ---- 6 mutable alias ---- */
    CASE("alias: mut after shared", OSC_REJ_MUTABLE_ALIAS, ALLOC(1, 0), BSH(1, 1, 0), BMUT(1, 2, 0));
    CASE("alias: shared after mut", OSC_REJ_MUTABLE_ALIAS, ALLOC(1, 0), BMUT(1, 1, 0), BSH(1, 2, 0));
    CASE("alias: second mut", OSC_REJ_MUTABLE_ALIAS, ALLOC(1, 0), BMUT(1, 1, 0), BMUT(1, 2, 0));
    CASE("alias: direct write while shared", OSC_REJ_MUTABLE_ALIAS, ALLOC(1, 0), BSH(1, 1, 0), WR(1, 0));
    CASE("alias: direct read while mut", OSC_REJ_MUTABLE_ALIAS, ALLOC(1, 0), BMUT(1, 1, 0), RD(1, 0));
    CASE("alias: move while borrowed", OSC_REJ_MUTABLE_ALIAS, ALLOC(1, 0), BSH(1, 1, 0), MOVE(1, 2));
    CASE("alias: parent used under mut reborrow", OSC_REJ_MUTABLE_ALIAS,
         ALLOC(1, 0), BMUT(1, 1, 0), BMUT(1, 2, 1), RD(1, 1));
    CASE("alias: second mut reborrow", OSC_REJ_MUTABLE_ALIAS,
         ALLOC(1, 0), BMUT(1, 1, 0), BSH(1, 2, 1), BMUT(1, 3, 1));
    /* ---- 7 borrow outlives owner ---- */
    CASE("boo: release while borrowed", OSC_REJ_BORROW_OUTLIVES_OWNER, ALLOC(1, 0), BSH(1, 1, 0), REL(1));
    CASE("boo: parent ends before reborrow", OSC_REJ_BORROW_OUTLIVES_OWNER,
         ALLOC(1, 0), BMUT(1, 1, 0), BSH(1, 2, 1), END(1));
    /* ---- 8 arena escape ---- */
    CASE("escape: borrow into destroyed region", OSC_REJ_ARENA_ESCAPE,
         ROPEN(1), ALLOC(1, 1), ALLOC(2, 0), BSH(1, 1, 0), RDEST(1));
    CASE("escape: reborrow chain", OSC_REJ_ARENA_ESCAPE,
         ROPEN(2), ALLOC(1, 2), BMUT(1, 1, 0), BSH(1, 2, 1), END(2), RDEST(2));
    /* ---- 9 reclaim without Quiesced ---- */
    CASE("reclaim: no token", OSC_REJ_RECLAIM_WITHOUT_QUIESCED, CW(1), CPUB(1, REL_), COBS(1, ACQ_), RECL(1, 0));
    CASE("reclaim: other cell's token", OSC_REJ_RECLAIM_WITHOUT_QUIESCED, DRAINED(2, 1), CW(1), RECL(1, 1));
    CASE("reclaim: consumed token", OSC_REJ_RECLAIM_WITHOUT_QUIESCED, DRAINED(1, 1), RECL(1, 1), CW(2), RECL(2, 1));
    /* ---- 10 read before observe ---- */
    CASE("rbo: read published", OSC_REJ_READ_BEFORE_OBSERVE, CW(1), CPUB(1, REL_), CRD(1));
    CASE("rbo: read written", OSC_REJ_READ_BEFORE_OBSERVE, CW(1), CRD(1));
    CASE("rbo: relaxed poll", OSC_REJ_READ_BEFORE_OBSERVE, CW(1), CPUB(1, REL_), COBS(1, RLX_));
    CASE("rbo: new round", OSC_REJ_READ_BEFORE_OBSERVE, CW(1), CPUB(1, REL_), COBS(1, ACQ_), CRD(1), CW(1), CRD(1));
    /* ---- 11 publish without release ---- */
    CASE("pwr: relaxed", OSC_REJ_PUBLISH_WITHOUT_RELEASE, CW(1), CPUB(1, RLX_));
    CASE("pwr: acquire", OSC_REJ_PUBLISH_WITHOUT_RELEASE, CW(1), CPUB(1, ACQ_));
    /* ---- 12 forged rights ---- */
    CASE("forged: derive wider", OSC_REJ_FORGED_RIGHTS, SALLOC(1, 1, 0x3), DERIVE(2, 1, 0x7));
    CASE("forged: grandchild wider than parent", OSC_REJ_FORGED_RIGHTS,
         SALLOC(1, 1, 0xF), DERIVE(2, 1, 0x3), DERIVE(3, 2, 0x4));
    CASE("forged: use wider", OSC_REJ_FORGED_RIGHTS, SALLOC(1, 1, 0x3), HUSE(1, 0x4));
    CASE("forged: write through shared borrow", OSC_REJ_FORGED_RIGHTS, ALLOC(1, 0), BSH(1, 1, 0), WR(1, 1));
    CASE("forged: &mut through shared borrow", OSC_REJ_FORGED_RIGHTS, ALLOC(1, 0), BSH(1, 1, 0), BMUT(1, 2, 1));
    /* ---- 13 live handle to a durable encoder ---- */
    CASE("durable: live handle", OSC_REJ_LIVE_HANDLE_DURABLE, SALLOC(1, 1, 1), DUR(OSC_VK_HANDLE, 1));
    CASE("durable: stale handle", OSC_REJ_LIVE_HANDLE_DURABLE, SALLOC(1, 1, 1), SFREE(1), DUR(OSC_VK_HANDLE, 1));
    CASE("durable: borrow", OSC_REJ_LIVE_HANDLE_DURABLE, ALLOC(1, 0), BSH(1, 1, 0), DUR(OSC_VK_BORROW, 1));

    /* ---- non-named refusals ---- */
    CASE("malformed: never allocated", OSC_REJ_MALFORMED, RD(1, 0));
    CASE("malformed: id reused", OSC_REJ_MALFORMED, ALLOC(1, 0), REL(1), ALLOC(1, 0));
    CASE("malformed: id 0", OSC_REJ_MALFORMED, ALLOC(0, 0));
    CASE("malformed: via names another object", OSC_REJ_MALFORMED, ALLOC(1, 0), ALLOC(2, 0), BSH(1, 1, 0), RD(2, 1));
    CASE("capacity: object id", OSC_REJ_CAPACITY, ALLOC(OSC_MODEL_MAX_OBJECTS + 1, 0));
    CASE("capacity: slot id", OSC_REJ_CAPACITY, SALLOC(OSC_MODEL_MAX_SLOTS + 1, 1, 0));
    CASE("protocol: observe unpublished", OSC_REJ_PROTOCOL, CW(1), COBS(1, ACQ_));
    CASE("protocol: alloc live slot", OSC_REJ_PROTOCOL, SALLOC(1, 1, 0), SALLOC(1, 2, 0));
    CASE("protocol: unknown kind", OSC_REJ_PROTOCOL, EV(.kind = 999));

    /* reject leaves state unchanged */
    {
        OscModel a, b;
        osc_model_init(&a, 0, UINT64_MAX);
        OscModelEvent e1[] = { ALLOC(1, 0), BSH(1, 1, 0) };
        for (int i = 0; i < 2; i++) osc_model_step(&a, &e1[i], NULL);
        b = a;
        OscModelEvent badev = REL(1);
        OscModelReject r;
        osc_model_step(&a, &badev, &r);
        a.rejected = b.rejected;
        g_unit_cases++;
        if (r != OSC_REJ_BORROW_OUTLIVES_OWNER || memcmp(&a, &b, sizeof a) != 0) {
            printf("UNIT FAIL reject-unchanged\n");
            g_unit_fail++;
        }
    }
    /* names */
    g_unit_cases++;
    if (strcmp(osc_model_reject_name(OSC_REJ_LIVE_HANDLE_DURABLE), "live-handle-durable") != 0 ||
        strcmp(osc_model_reject_name(OSC_REJ_NONE), "accept") != 0 ||
        strcmp(osc_model_reject_name((OscModelReject)99), "?") != 0 || OSC_REJ_LIVE_HANDLE_DURABLE != 13) {
        printf("UNIT FAIL names\n");
        g_unit_fail++;
    }
}

typedef struct { uint64_t injected, correct, wrong, accepted, gen_fail; } ClassCount;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++) { h ^= c[i]; h *= 0x100000001B3ull; }
    return h;
}

int main(int argc, char **argv)
{
    uint64_t seed = DEFAULT_SEED, nseq = GATE_MIN_SEQUENCES;
    if (argc > 1) seed = strtoull(argv[1], NULL, 16);
    if (argc > 2) nseq = strtoull(argv[2], NULL, 10);

    unit_tests();
    printf("OSC0B_MODEL_UNIT cases=%d failed=%d\n", g_unit_cases, g_unit_fail);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    static OscGenSeq q;
    ClassCount cc[OSC_MODEL_NAMED_COUNT + 1];
    memset(cc, 0, sizeof cc);
    uint64_t steps = 0, valid_seqs = 0, false_rejects = 0, false_accepts = 0, wrong = 0;
    uint64_t gen_fail = 0, state_changed = 0, wrong_shown = 0;
    uint64_t digest = 0xCBF29CE484222325ull;
    uint64_t crng = seed ^ 0xA5A5C3C35A5A3C3Cull;

    for (uint64_t i = 0; i < nseq; i++) {
        uint64_t x = osc_gen_splitmix64(&crng);
        OscModelReject cls = (x & 3) == 0 ? OSC_REJ_NONE : (OscModelReject)(1 + (x >> 8) % OSC_MODEL_NAMED_COUNT);
        if (osc_gen_sequence(seed, i, cls, &q) != 0) {
            gen_fail++;
            if (cls) cc[cls].gen_fail++;
            continue;
        }
        if (cls == OSC_REJ_NONE) valid_seqs++; else cc[cls].injected++;

        OscModel m, snap;
        osc_model_init(&m, q.gen_base, q.gen_max);
        for (int k = 0; k < q.n; k++) {
            OscModelReject r;
            if (k == q.inject_at) snap = m;
            osc_model_step(&m, &q.ev[k], &r);
            steps++;
            digest = fnv(digest, &r, sizeof r);
            if (k == q.inject_at) {
                if (r == OSC_REJ_NONE) {
                    false_accepts++; cc[cls].accepted++;
                    if (wrong_shown++ < 5)
                        printf("FALSE ACCEPT seq=%" PRIu64 " class=%s variant=%d step=%d\n", i,
                               osc_model_reject_name(cls), q.variant, k);
                    break;
                }
                if (r == q.expect) cc[cls].correct++;
                else {
                    wrong++; cc[cls].wrong++;
                    if (wrong_shown++ < 5)
                        printf("WRONG NAME seq=%" PRIu64 " class=%s variant=%d got=%s event=%s\n", i,
                               osc_model_reject_name(cls), q.variant, osc_model_reject_name(r),
                               osc_model_event_name(q.ev[k].kind));
                }
                snap.rejected = m.rejected;
                if (memcmp(&snap, &m, sizeof m) != 0) state_changed++;
            } else if (r != OSC_REJ_NONE) {
                false_rejects++;
                if (wrong_shown++ < 5)
                    printf("FALSE REJECT seq=%" PRIu64 " class=%s step=%d event=%s got=%s\n", i,
                           osc_model_reject_name(cls), k, osc_model_event_name(q.ev[k].kind),
                           osc_model_reject_name(r));
                break;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    int ok = g_unit_fail == 0 && false_accepts == 0 && wrong == 0 && false_rejects == 0 &&
             gen_fail == 0 && state_changed == 0;
    int gate = nseq >= GATE_MIN_SEQUENCES;
    for (int c = 1; c <= OSC_MODEL_NAMED_COUNT; c++) {
        printf("OSC0B_MODEL_CLASS %-26s injected=%" PRIu64 " rejected_correct=%" PRIu64
               " rejected_wrong_name=%" PRIu64 " accepted=%" PRIu64 "\n",
               osc_model_reject_name((OscModelReject)c), cc[c].injected, cc[c].correct, cc[c].wrong, cc[c].accepted);
        if (gate && cc[c].injected < GATE_MIN_PER_CLASS) ok = 0;
        if (cc[c].correct != cc[c].injected) ok = 0;
    }
    printf("OSC0B_MODEL_VALID sequences=%" PRIu64 " gen_failures=%" PRIu64 " state_changed_on_reject=%" PRIu64
           " digest=%016" PRIx64 " seconds=%.2f\n", valid_seqs, gen_fail, state_changed, digest, secs);
    printf("OSC0B_MODEL_SEED=%016" PRIx64 " SEQUENCES=%" PRIu64 " STEPS=%" PRIu64 " FALSE_ACCEPTS=%" PRIu64
           " WRONG_NAME=%" PRIu64 " FALSE_REJECTS=%" PRIu64 "\n", seed, nseq, steps, false_accepts, wrong, false_rejects);
    if (!ok) { printf("OSC0B_MODEL_FAIL\n"); return 1; }
    if (!gate) { printf("OSC0B_MODEL_SMOKE_PASS\n"); return 0; }
    printf("OSC0B_MODEL_PASS\n");
    return 0;
}
