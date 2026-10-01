/* M20 production J-Space mechanics.
 *
 * Covers only what M20 added to rx_jspace: slab allocation and reclamation,
 * generation-safe branch and realization references, deterministic failure at
 * every limit, concurrent use through the space mutex, durable checkpoints
 * that reopen to a valid state (including after a crash), refusal of torn or
 * corrupt metadata, placement metadata and remote refusal, and World commit
 * compatibility (a World field names a packed branch reference; writers stage
 * privately and publish through the World's own commit).
 *
 * The 4-minute branch-reuse benchmark (make test-branch-reuse) remains the
 * check that J-Space semantics did not change. */
#include "runtime/rx_caproot.h"
#include "runtime/rx_world.h"
#include "runtime/rx_jspace.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

#define UNIT 4096u

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Pure: same (prev, token) gives the same bytes. */
static void derive_t(const uint8_t *prev, uint64_t token, uint8_t *out, size_t n) {
    uint64_t x = token;
    if (!prev) {
        for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t v = splitmix(&x); memcpy(out + i, &v, 8); }
        return;
    }
    const uint64_t *p = (const uint64_t *)prev;
    uint64_t *o = (uint64_t *)out;
    size_t w = n / 8;
    for (size_t j = 0; j < w; j++) o[j] = (p[j] ^ splitmix(&x)) + p[(j + 1) % w];
}

static const JsRealizer RZ = { JS_REAL_WORLD_PROJECTION, "test_projection", UNIT, derive_t };
static const JsRealizer RZ2 = { JS_REAL_KV_STATE, "test_kv", UNIT, derive_t };

static JsReal *unit_of(JsSpace *s, uint32_t b, uint32_t i) { return s->branches[b]->units[i]; }

static void digest(JsSpace *s, uint32_t b, uint8_t out[32]) {
    CHECK(js_branch_content_digest(s, b, out) == JS_OK, "digest of branch %u", b);
}

static JsLimits lim(uint32_t reals, uint32_t branches, uint64_t spill, uint64_t resident) {
    JsLimits l = { reals, branches, spill, resident };
    return l;
}

/* ---- allocation and reclamation ------------------------------------------ */

static void test_alloc_reclaim(void) {
    static JsSpace s;
    JsLimits l = lim(64, 4, 8 * UNIT, 0);
    CHECK(js_space_init_limits(&s, "/tmp/jspace_prod_spill", &l) == JS_OK, "init");
    uint32_t root;
    CHECK(js_branch_root(&s, &RZ, 1, &root) == JS_OK, "root");
    for (int i = 0; i < 4; i++) CHECK(js_branch_derive(&s, root, 10 + i) == JS_OK, "derive");
    uint64_t base = s.stats.live_reals;
    /* Ten thousand fork/derive/edit/spill/release cycles in a space with room
     * for 64 realizations, 4 branches and 8 spilled units: only works if every
     * slot, branch and extent comes back. */
    for (int k = 0; k < 10000; k++) {
        uint32_t c;
        int rc = js_branch_fork(&s, root, &c);
        if (rc) { CHECK(0, "fork %d rc %d", k, rc); break; }
        uint8_t p[16];
        memset(p, k & 0xff, sizeof p);
        CHECK(js_branch_derive(&s, c, 1000 + (uint64_t)k) == JS_OK, "derive %d", k);
        CHECK(js_branch_edit(&s, c, 5, 64, p, sizeof p) == JS_OK, "edit %d", k);
        CHECK(js_real_spill(&s, unit_of(&s, c, 5)) == JS_OK, "spill %d", k);
        CHECK(js_branch_release(&s, c) == JS_OK, "release %d", k);
        if (g_fail) break;
    }
    CHECK(s.stats.live_reals == base, "live realizations back to %lu (now %lu)",
          (unsigned long)base, (unsigned long)s.stats.live_reals);
    CHECK(s.real_hw <= 8, "slab high-water %u: slots were not reused", s.real_hw);
    CHECK(s.n_branches <= 2, "branch high-water %u: slots were not reused", s.n_branches);
    CHECK(s.spill_end <= UNIT, "spill file grew to %lu", (unsigned long)s.spill_end);
    CHECK(s.stats.spilled_bytes == 0, "spilled bytes leaked");
    js_space_destroy(&s);
}

/* ---- generation safety ---------------------------------------------------- */

static void test_generations(void) {
    static JsSpace s;
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");
    uint32_t root, c;
    js_branch_root(&s, &RZ, 2, &root);
    js_branch_derive(&s, root, 3);
    JsBranchRef rr, cr;
    CHECK(js_branch_ref(&s, root, &rr) == JS_OK, "root ref");
    CHECK(js_branch_fork_staged(&s, rr, 42, &cr) == JS_OK, "staged fork");
    c = cr.id;
    js_branch_derive(&s, c, 4);
    JsRealId rid;
    CHECK(js_real_id(&s, c, 2, &rid) == JS_OK, "real id");
    CHECK(js_real_lookup(&s, rid) == unit_of(&s, c, 2), "lookup resolves");

    CHECK(js_branch_release_ref(&s, cr, 7) == JS_ERR_OWNER, "foreign subject released a branch");
    CHECK(js_branch_check(&s, cr) == JS_OK, "owner refusal changed state");
    CHECK(js_branch_release_ref(&s, cr, 42) == JS_OK, "owner release");
    CHECK(js_branch_check(&s, cr) == JS_ERR_STALE, "released ref still valid");
    CHECK(js_branch_seal(&s, cr) == JS_ERR_STALE, "seal of stale ref");
    CHECK(js_branch_release_ref(&s, cr, 42) == JS_ERR_STALE, "double release");
    CHECK(js_real_lookup(&s, rid) == NULL, "reclaimed realization still resolves");

    /* The slots come back with new generations: old refs never alias. */
    JsBranchRef nr;
    CHECK(js_branch_fork_staged(&s, rr, 0, &nr) == JS_OK, "refork");
    CHECK(nr.id == cr.id && nr.gen != cr.gen, "slot %u reused without a new generation", nr.id);
    CHECK(js_branch_check(&s, cr) == JS_ERR_STALE, "old ref aliases new occupant");
    js_branch_derive(&s, nr.id, 5);
    JsRealId nid;
    js_real_id(&s, nr.id, 2, &nid);
    CHECK(nid.slot == rid.slot && nid.gen != rid.gen, "realization slot reuse kept generation");
    CHECK(js_real_lookup(&s, rid) == NULL, "old realization id aliases new one");

    /* Packed form for World fields round-trips. */
    JsBranchRef u = js_branch_ref_unpack(js_branch_ref_pack(nr));
    CHECK(u.id == nr.id && u.gen == nr.gen, "pack round trip");
    CHECK(js_space_reclaim_staged(&s) == 1, "one staged branch to reclaim");
    CHECK(js_branch_check(&s, nr) == JS_ERR_STALE, "reclaimed staged branch still valid");
    js_space_destroy(&s);
}

/* ---- limits: deterministic failure, nothing changes ------------------------ */

static void test_limits(void) {
    static JsSpace s;
    uint8_t d0[32], d1[32];
    JsLimits l = lim(6, 3, 2 * UNIT, 4 * UNIT);
    CHECK(js_space_init_limits(&s, "/tmp/jspace_prod_spill", &l) == JS_OK, "init");
    uint32_t root, c1, c2, c3;
    js_branch_root(&s, &RZ, 9, &root);
    for (int i = 0; i < 3; i++) CHECK(js_branch_derive(&s, root, 20 + i) == JS_OK, "derive");
    /* resident limit: 4 units resident */
    digest(&s, root, d0);
    uint64_t live = s.stats.live_reals, res = s.stats.resident_bytes;
    CHECK(js_branch_derive(&s, root, 99) == JS_ERR_FULL, "resident limit not enforced");
    CHECK(s.stats.live_reals == live && s.stats.resident_bytes == res &&
          s.branches[root]->n_units == 4, "failed derive changed state");
    digest(&s, root, d1);
    CHECK(!memcmp(d0, d1, 32), "failed derive changed content");

    /* spill limit: two units fit, the third fails and stays where it was */
    CHECK(js_real_spill(&s, unit_of(&s, root, 0)) == JS_OK, "spill 0");
    CHECK(js_real_spill(&s, unit_of(&s, root, 1)) == JS_OK, "spill 1");
    CHECK(js_real_spill(&s, unit_of(&s, root, 2)) == JS_ERR_FULL, "spill limit not enforced");
    CHECK(unit_of(&s, root, 2)->placement == JS_PLACE_HOT, "failed spill moved the unit");

    /* realization limit: 6 slots, 4 used */
    CHECK(js_branch_derive(&s, root, 30) == JS_OK, "derive 5");
    CHECK(js_branch_derive(&s, root, 31) == JS_OK, "derive 6");
    live = s.stats.live_reals;
    CHECK(js_branch_derive(&s, root, 32) == JS_ERR_FULL, "realization limit not enforced");
    CHECK(s.stats.live_reals == live && s.branches[root]->n_units == 6, "failed derive changed state");

    /* branch limit: 3 slots */
    CHECK(js_branch_fork(&s, root, &c1) == JS_OK && js_branch_fork(&s, root, &c2) == JS_OK, "forks");
    uint32_t nb = s.n_branches;
    CHECK(js_branch_fork(&s, root, &c3) == JS_ERR_FULL, "branch limit not enforced");
    CHECK(s.n_branches == nb, "failed fork took a slot");
    /* The same failures repeat identically. */
    CHECK(js_branch_fork(&s, root, &c3) == JS_ERR_FULL, "branch limit not deterministic");
    CHECK(js_branch_derive(&s, c1, 32) == JS_ERR_FULL, "realization limit not deterministic");
    js_space_destroy(&s);

    JsLimits bad = lim(0, 3, 0, 0);
    CHECK(js_space_init_limits(&s, "/tmp/jspace_prod_spill", &bad) == JS_ERR_ARG, "zero limit accepted");
    js_space_destroy(&s);
}

/* ---- concurrency ----------------------------------------------------------- */

#define NTHREADS 8
#define STEPS 60

typedef struct {
    JsSpace *s;
    JsBranchRef root;
    unsigned t;
    uint8_t dig[32];
    int rc;
} Worker;

static int script(JsSpace *s, uint32_t b, unsigned t) {
    for (unsigned k = 0; k < STEPS; k++) {
        int rc;
        if (k % 7 == 3) {
            uint8_t p[24];
            memset(p, (int)(t * 31 + k), sizeof p);
            rc = js_branch_edit(s, b, k % 9, (k * 64) % (UNIT - 24), p, sizeof p);
        } else {
            rc = js_branch_derive(s, b, (uint64_t)t * 100000 + k);
        }
        if (rc) return rc;
        uint8_t buf[UNIT];
        if ((rc = js_branch_read(s, b, k % 9, buf))) return rc;
    }
    return JS_OK;
}

static void *worker(void *arg) {
    Worker *w = arg;
    JsBranchRef me;
    w->rc = js_branch_fork_staged(w->s, w->root, w->t + 1, &me);
    if (!w->rc) w->rc = script(w->s, me.id, w->t);
    if (!w->rc) w->rc = js_branch_content_digest(w->s, me.id, w->dig);
    if (!w->rc) w->rc = js_branch_release_ref(w->s, me, w->t + 1);
    return NULL;
}

static volatile int g_stop;
static void *pressure(void *arg) {
    JsSpace *s = arg;
    JsPolicyReport rep;
    while (!g_stop) js_forge_enforce(s, 6 * UNIT, &rep);
    return NULL;
}

static void test_concurrency(void) {
    static JsSpace s, ref;
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");
    CHECK(js_space_init(&ref, "/tmp/jspace_prod_spill2") == JS_OK, "init ref");
    uint32_t root, rroot;
    js_branch_root(&s, &RZ, 77, &root);
    js_branch_root(&ref, &RZ, 77, &rroot);
    for (unsigned i = 0; i < 8; i++) {
        js_branch_derive(&s, root, 500 + i);
        js_branch_derive(&ref, rroot, 500 + i);
    }
    Worker w[NTHREADS];
    pthread_t th[NTHREADS], pt;
    g_stop = 0;
    pthread_create(&pt, NULL, pressure, &s);
    for (unsigned t = 0; t < NTHREADS; t++) {
        w[t] = (Worker){ .s = &s, .t = t };
        js_branch_ref(&s, root, &w[t].root);
        pthread_create(&th[t], NULL, worker, &w[t]);
    }
    for (unsigned t = 0; t < NTHREADS; t++) pthread_join(th[t], NULL);
    g_stop = 1;
    pthread_join(pt, NULL);
    /* Same scripts, one thread, no pressure: same content. */
    for (unsigned t = 0; t < NTHREADS; t++) {
        CHECK(w[t].rc == JS_OK, "worker %u rc %d", t, w[t].rc);
        uint32_t c;
        js_branch_fork(&ref, rroot, &c);
        CHECK(script(&ref, c, t) == JS_OK, "reference script %u", t);
        uint8_t d[32];
        digest(&ref, c, d);
        CHECK(!memcmp(d, w[t].dig, 32), "worker %u content differs from serial reference", t);
        js_branch_release(&ref, c);
    }
    CHECK(s.stats.corrupt_restores == 0, "corrupt restores under concurrency");
    CHECK(s.n_free_branch + 1 == s.n_branches, "worker branches not all reclaimed");
    uint8_t a[32], b[32];
    digest(&s, root, a);
    digest(&ref, rroot, b);
    CHECK(!memcmp(a, b, 32), "shared root changed under concurrent use");
    js_space_destroy(&s);
    js_space_destroy(&ref);
}

/* ---- placement metadata, remote refusal ------------------------------------ */

static void test_remote(void) {
    static JsSpace s;
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");
    uint32_t root;
    js_branch_root(&s, &RZ, 5, &root);
    js_branch_derive(&s, root, 6);
    JsBranchRef r;
    js_branch_ref(&s, root, &r);
    JsHome h = { .locality = JS_HOME_REMOTE_OWNED };
    memset(h.machine, 0xab, sizeof h.machine);
    CHECK(js_branch_set_home(&s, r, &h) == JS_OK, "set home");
    JsHome got;
    CHECK(js_branch_home(&s, r, &got) == JS_OK && !memcmp(&got, &h, sizeof h), "home round trip");
    uint32_t c;
    uint8_t p[4] = { 1, 2, 3, 4 }, buf[UNIT];
    CHECK(js_branch_derive(&s, root, 7) == JS_ERR_REMOTE, "derive on remote-owned branch");
    CHECK(js_branch_edit(&s, root, 0, 0, p, 4) == JS_ERR_REMOTE, "edit on remote-owned branch");
    CHECK(js_branch_fork(&s, root, &c) == JS_ERR_REMOTE, "fork of remote-owned branch");
    CHECK(js_branch_read(&s, root, 1, buf) == JS_OK, "resident read of remote-owned branch");
    js_real_evict(&s, unit_of(&s, root, 1));
    CHECK(js_branch_read(&s, root, 1, buf) == JS_ERR_REMOTE, "non-resident remote read without transport");
    h.locality = JS_HOME_REPLICA;
    js_branch_set_home(&s, r, &h);
    CHECK(js_branch_derive(&s, root, 7) == JS_ERR_REMOTE, "derive on replica");
    CHECK(js_branch_read(&s, root, 1, buf) == JS_OK, "replica rebuilds from recipe");
    h.locality = 9;
    CHECK(js_branch_set_home(&s, r, &h) == JS_ERR_ARG, "bad locality accepted");
    js_space_destroy(&s);
}

/* ---- durability: reopen, crash, torn metadata ------------------------------ */

static char g_dir[96];
static const JsRealizer *const RZS[] = { &RZ, &RZ2 };

static void path(char *out, const char *name) { snprintf(out, 128, "%s/%s", g_dir, name); }

static int copy_file(const char *from, const char *to) {
    int a = open(from, O_RDONLY), b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int rc = a < 0 || b < 0 ? -1 : 0;
    char buf[65536];
    ssize_t n;
    while (!rc && (n = read(a, buf, sizeof buf)) > 0) if (write(b, buf, (size_t)n) != n) rc = -1;
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    return rc;
}

static void flip(const char *file, off_t at) {
    int fd = open(file, O_RDWR);
    uint8_t c;
    if (fd < 0 || pread(fd, &c, 1, at) != 1) { CHECK(0, "flip %s", file); if (fd >= 0) close(fd); return; }
    c ^= 0x5a;
    if (pwrite(fd, &c, 1, at) != 1) CHECK(0, "flip write");
    close(fd);
}

typedef struct {
    JsBranchRef a, b, staged;
    uint8_t da[32], db[32];
    uint32_t na, nb;
} Saved;

/* Build a durable state, commit it, then keep changing it without committing. */
static void build_and_crash(Saved *sv, int crash) {
    static JsSpace s;
    JsHome me = { .locality = JS_HOME_LOCAL };
    memset(me.machine, 0x11, sizeof me.machine);
    CHECK(js_space_open(&s, g_dir, RZS, 2, NULL, &me) == JS_OK, "fresh open");
    uint32_t a, b, k;
    js_branch_root(&s, &RZ, 123, &a);
    for (unsigned i = 0; i < 12; i++) js_branch_derive(&s, a, 40 + i);
    js_branch_fork(&s, a, &b);
    uint8_t p[32];
    memset(p, 0xc3, sizeof p);
    js_branch_edit(&s, b, 3, 100, p, sizeof p);
    js_branch_edit(&s, b, 9, 4000, p, 32);
    for (unsigned i = 0; i < 4; i++) js_branch_derive(&s, b, 900 + i);
    js_branch_root(&s, &RZ2, 55, &k);           /* a second representation */
    js_branch_derive(&s, k, 56);
    /* mixed placements: spilled, compressed, evicted, hot */
    js_real_spill(&s, unit_of(&s, a, 2));
    js_real_spill(&s, unit_of(&s, b, 3));
    js_real_compress(&s, unit_of(&s, a, 5));
    js_real_evict(&s, unit_of(&s, b, 14));
    js_branch_ref(&s, a, &sv->a);
    js_branch_ref(&s, b, &sv->b);
    JsHome far = { .locality = JS_HOME_REPLICA };
    memset(far.machine, 0x77, sizeof far.machine);
    js_branch_set_home(&s, sv->b, &far);
    js_branch_set_owner(&s, sv->b, 9);
    CHECK(js_branch_fork_staged(&s, sv->b, 9, &sv->staged) == JS_ERR_REMOTE, "fork of replica");
    /* staged fork of a: never named by a commit, never persisted */
    CHECK(js_branch_fork_staged(&s, sv->a, 3, &sv->staged) == JS_OK, "staged fork");
    js_branch_derive(&s, sv->staged.id, 1);
    digest(&s, a, sv->da);
    digest(&s, b, sv->db);
    sv->na = s.branches[a]->n_units;
    sv->nb = s.branches[b]->n_units;
    CHECK(js_space_commit(&s) == JS_OK, "commit");
    if (!crash) { js_space_destroy(&s); return; }
    /* After the commit: overwrite-prone work that must not damage it. Units
     * named by the checkpoint are freed, respilled and edited; nothing is
     * committed, then the process dies. */
    js_real_restore(&s, unit_of(&s, a, 2));      /* frees a named spill extent */
    js_real_restore(&s, unit_of(&s, b, 3));
    js_branch_release(&s, k);
    for (unsigned i = 0; i < 6; i++) {
        js_branch_derive(&s, sv->staged.id, 2 + i);
        js_real_spill(&s, unit_of(&s, sv->staged.id, s.branches[sv->staged.id]->n_units - 1));
    }
    _exit(0);
}

static void verify_reopen(const Saved *sv, const char *what) {
    static JsSpace s;
    int rc = js_space_open(&s, g_dir, RZS, 2, NULL, NULL);
    CHECK(rc == JS_OK, "%s: reopen rc %d", what, rc);
    if (rc) return;
    CHECK(js_branch_check(&s, sv->a) == JS_OK && js_branch_check(&s, sv->b) == JS_OK,
          "%s: committed refs not valid after reopen", what);
    CHECK(js_branch_check(&s, sv->staged) == JS_ERR_STALE, "%s: staged branch survived", what);
    CHECK(s.branches[sv->a.id]->n_units == sv->na && s.branches[sv->b.id]->n_units == sv->nb,
          "%s: unit counts", what);
    uint8_t d[32];
    digest(&s, sv->a.id, d);
    CHECK(!memcmp(d, sv->da, 32), "%s: branch a content after reopen", what);
    digest(&s, sv->b.id, d);
    CHECK(!memcmp(d, sv->db, 32), "%s: branch b content after reopen", what);
    CHECK(s.stats.corrupt_restores == 0, "%s: corrupt restores", what);
    JsHome h;
    js_branch_home(&s, sv->b, &h);
    CHECK(h.locality == JS_HOME_REPLICA && h.machine[0] == 0x77, "%s: placement metadata lost", what);
    CHECK(js_branch_release_ref(&s, sv->b, 1) == JS_ERR_OWNER, "%s: owner lost", what);
    /* Reopened state is fully usable, and a new staged slot never aliases. */
    JsBranchRef n;
    CHECK(js_branch_fork_staged(&s, sv->a, 0, &n) == JS_OK, "%s: fork after reopen", what);
    CHECK(!(n.id == sv->staged.id && n.gen == sv->staged.gen), "%s: stale ref aliases", what);
    CHECK(js_branch_derive(&s, n.id, 5) == JS_OK, "%s: derive after reopen", what);
    js_space_destroy(&s);
}

static void test_durable(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/jspace_prod_XXXXXX");
    if (!mkdtemp(g_dir)) { CHECK(0, "mkdtemp"); return; }
    char meta[128], data[128], tmp[128], keep_meta[128], keep_data[128];
    path(meta, "jspace.meta"); path(data, "jspace.data"); path(tmp, "jspace.meta.tmp");
    path(keep_meta, "keep.meta"); path(keep_data, "keep.data");
    Saved sv;

    /* Crash after commit: a child builds, commits, keeps working, dies. */
    pid_t pid = fork();
    if (pid == 0) { g_fail = 0; build_and_crash(&sv, 1); _exit(1); }
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "crash child");
    /* Rebuild the same state in-process (deterministic) for the expectations. */
    char dir2[96];
    memcpy(dir2, g_dir, sizeof dir2);
    strcat(g_dir, "/ref");   /* g_dir is a 24-byte mkdtemp name in a 96-byte buffer */
    build_and_crash(&sv, 0);
    memcpy(g_dir, dir2, sizeof g_dir);
    verify_reopen(&sv, "after crash");
    verify_reopen(&sv, "second reopen");   /* reopen without commit is idempotent */

    copy_file(meta, keep_meta);
    copy_file(data, keep_data);
    struct stat ms;
    stat(meta, &ms);
    static JsSpace s;
    struct { const char *what; int mode; off_t at; int want; } cases[] = {
        { "torn metadata (half written)", 1, 0, JS_ERR_CORRUPT },
        { "torn metadata (header only)", 2, 0, JS_ERR_CORRUPT },
        { "flipped header byte", 3, 20, JS_ERR_CORRUPT },
        { "flipped body byte", 3, 300, JS_ERR_CORRUPT },
        { "flipped last byte", 3, -1, JS_ERR_CORRUPT },
        { "data file truncated", 4, 0, JS_ERR_CORRUPT },
        { "empty metadata", 5, 0, JS_ERR_CORRUPT },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        copy_file(keep_meta, meta);
        copy_file(keep_data, data);
        switch (cases[i].mode) {
        case 1: CHECK(truncate(meta, ms.st_size / 2) == 0, "truncate"); break;
        case 2: CHECK(truncate(meta, 128) == 0, "truncate"); break;
        case 3: flip(meta, cases[i].at < 0 ? ms.st_size - 1 : cases[i].at); break;
        case 4: CHECK(truncate(data, 1) == 0, "truncate"); break;
        case 5: CHECK(truncate(meta, 0) == 0, "truncate"); break;
        }
        int rc = js_space_open(&s, g_dir, RZS, 2, NULL, NULL);
        CHECK(rc == cases[i].want, "%s: open rc %d want %d", cases[i].what, rc, cases[i].want);
        if (rc == JS_OK) js_space_destroy(&s);
    }
    /* Corrupt spilled bytes: metadata is fine, the content check refuses the bytes. */
    copy_file(keep_meta, meta);
    copy_file(keep_data, data);
    {
        CHECK(js_space_open(&s, g_dir, RZS, 2, NULL, NULL) == JS_OK, "open");
        JsReal *r = unit_of(&s, sv.a.id, 2);
        CHECK(r->placement == JS_PLACE_SPILLED, "spilled placement persisted");
        uint64_t off = r->spill_off;
        js_space_destroy(&s);
        flip(data, (off_t)off + 7);
        CHECK(js_space_open(&s, g_dir, RZS, 2, NULL, NULL) == JS_OK, "open");
        uint8_t buf[UNIT];
        CHECK(js_branch_read(&s, sv.a.id, 2, buf) == JS_ERR_CORRUPT, "corrupt spilled bytes served");
        js_space_destroy(&s);
    }
    /* Missing representation: the caller did not supply a persisted type. */
    copy_file(keep_meta, meta);
    copy_file(keep_data, data);
    CHECK(js_space_open(&s, g_dir, RZS, 1, NULL, NULL) == JS_ERR_ARG, "missing realizer accepted");
    /* A leftover temporary checkpoint is ignored and removed. */
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && write(fd, "OMJSPC01garbage", 15) == 15, "tmp write");
    if (fd >= 0) close(fd);
    verify_reopen(&sv, "leftover temp file");
    CHECK(access(tmp, F_OK) != 0, "temp checkpoint not removed");

    /* Commit, reopen, commit again: sequence advances, state holds; released
     * extents are reused only after the commit that stops naming them. */
    CHECK(js_space_open(&s, g_dir, RZS, 2, NULL, NULL) == JS_OK, "open");
    uint64_t seq = s.commit_seq;
    JsReal *r = unit_of(&s, sv.a.id, 2);
    uint64_t named = r->spill_off;
    js_real_restore(&s, r);
    CHECK(s.n_ext_pending >= 1, "freed named extent not quarantined");
    js_real_spill(&s, unit_of(&s, sv.a.id, 7));
    CHECK(unit_of(&s, sv.a.id, 7)->spill_off != named, "named extent reused before commit");
    CHECK(js_space_commit(&s) == JS_OK && s.commit_seq == seq + 1, "second commit");
    CHECK(s.n_ext_pending == 0, "quarantine not released by commit");
    js_space_destroy(&s);
    verify_reopen(&sv, "after second commit");

    /* Volatile spaces have no checkpoint. */
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");
    CHECK(js_space_commit(&s) == JS_ERR_ARG, "commit of volatile space");
    js_space_destroy(&s);

    char cmd[160];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd)) { /* best effort */ }
}

/* ---- World commit compatibility --------------------------------------------- */

enum { SUBJ_WRITER = 1, SUBJ_EXTERNAL = 100, ISSUER_AEGIS_POLICY = 3 };
enum { RES_STATE = 0x90, RES_TICK = 0x91 };
#define STEP_TOKEN 7u

typedef struct {
    JsSpace *s;
    RxObjRef state, tick;
    unsigned ran;
} WorldUser;

/* A writer: read the committed reference, fork it privately, advance it one
 * step, propose the new reference. The World's commit decides whether it
 * becomes visible; a refused proposal stays staged and is reclaimed. */
static int writer_fn(RxCtx *c) {
    WorldUser *u = c->user;
    const RxSnapshotDep *in = NULL;
    for (uint32_t i = 0; i < c->n_in; i++) if (c->in[i].obj.id == u->state.id) in = &c->in[i];
    if (!in) return 0;
    JsBranchRef cur = js_branch_ref_unpack(in->field[0]), next;
    if (js_branch_fork_staged(u->s, cur, SUBJ_WRITER, &next)) return 0;
    if (js_branch_derive(u->s, next.id, STEP_TOKEN)) return 0;
    __atomic_add_fetch(&u->ran, 1, __ATOMIC_RELAXED);
    c->out[c->n_out++] = (RxMutation){ u->state, 0, js_branch_ref_pack(next) };
    return 0;
}

static RxCapRef mint(RxCapAdmin *admin, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = ISSUER_AEGIS_POLICY;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(admin);
    RxCapRef r = { UINT32_MAX, 0 };
    CHECK(rx_capadmin_mint(admin, &m, &r) == RX_CAP_OK, "mint");
    return r;
}

static void test_world(void) {
    static RxCapRoot root;
    static RxCapAdmin admin;
    static RxWorld w;
    static JsSpace s;
    if (rx_caproot_start(&root, &admin) != RX_CAP_OK) { CHECK(0, "caproot"); return; }
    if (rx_world_init(&w, &root, 4, 1u << 16) != RX_OK) { CHECK(0, "world"); rx_caproot_stop(&root, &admin); return; }
    w.external_subject = SUBJ_EXTERNAL;
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");

    uint32_t base;
    js_branch_root(&s, &RZ, 31, &base);
    JsBranchRef b0;
    js_branch_ref(&s, base, &b0);
    uint64_t init_state[RX_MAX_FIELDS] = { js_branch_ref_pack(b0) }, init_tick[RX_MAX_FIELDS] = { 0 };
    WorldUser u = { .s = &s };
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_STATE, init_state, &u.state) == RX_OK, "state obj");
    CHECK(rx_world_create(&w, 1, RX_PERSIST_RESIDENT, RES_TICK, init_tick, &u.tick) == RX_OK, "tick obj");
    RxCapRef c_state = mint(&admin, SUBJ_WRITER, RES_STATE, RX_RIGHT_READ | RX_RIGHT_WRITE);
    RxCapRef c_tick = mint(&admin, SUBJ_WRITER, RES_TICK, RX_RIGHT_READ);
    RxCapRef c_ext = mint(&admin, SUBJ_EXTERNAL, RES_TICK, RX_RIGHT_WRITE);

    /* Two writers race on the same state field: World re-validation must make
     * every committed step build on the previously committed one. */
    for (int k = 0; k < 2; k++) {
        RxReactionDesc d;
        memset(&d, 0, sizeof d);
        d.name = k ? "jspace_writer_b" : "jspace_writer_a";
        d.subject = SUBJ_WRITER;
        d.priority = RX_PRIO_FOREGROUND;
        d.fn = writer_fn;
        d.user = &u;
        d.triggers[d.n_triggers++] = (RxDep){ u.tick, 1 };
        d.reads[d.n_reads++] = (RxDep){ u.state, 1 };
        d.writes[d.n_writes++] = (RxDep){ u.state, 1 };
        d.caps[d.n_caps++] = (RxCapNeed){ c_state, RES_STATE, RX_RIGHT_READ | RX_RIGHT_WRITE };
        d.caps[d.n_caps++] = (RxCapNeed){ c_tick, RES_TICK, RX_RIGHT_READ };
        uint32_t id;
        CHECK(rx_world_add_reaction(&w, &d, &id) == RX_OK, "add reaction %d", k);
    }

    JsBranchRef first = { UINT32_MAX, 0 };
    uint8_t first_dig[32];
    for (unsigned t = 1; t <= 40; t++) {
        RxMutation m = { u.tick, 0, t };
        CHECK(rx_world_publish_external(&w, c_ext, &m, 1) > 0, "tick %u", t);
        if (t % 8 == 0 || t == 1) {
            CHECK(rx_world_wait_quiescent(&w, 10000) == RX_OK, "quiesce");
            RxObject o;
            CHECK(rx_world_read(&w, u.state, &o) == RX_OK, "read state");
            JsBranchRef named = js_branch_ref_unpack(o.field[0]);
            /* What the World names is a live branch; seal it (the commit hook
             * requested from the World owner does this on every commit). */
            CHECK(js_branch_seal(&s, named) == JS_OK, "World names a dead branch");
            js_space_reclaim_staged(&s);
            CHECK(js_branch_check(&s, named) == JS_OK, "reclaim took the committed branch");
            if (first.id == UINT32_MAX && named.id != b0.id) {
                first = named;
                digest(&s, first.id, first_dig);
            }
        }
    }
    CHECK(rx_world_wait_quiescent(&w, 10000) == RX_OK, "quiesce");
    RxObject o;
    rx_world_read(&w, u.state, &o);
    JsBranchRef fin = js_branch_ref_unpack(o.field[0]);
    CHECK(js_branch_seal(&s, fin) == JS_OK, "final state names a dead branch");
    js_space_reclaim_staged(&s);

    /* No lost update: the committed state is exactly base + n steps. */
    uint32_t n = s.branches[fin.id]->n_units - 1;
    CHECK(n >= 1, "no write committed");
    static JsSpace ref;
    js_space_init(&ref, "/tmp/jspace_prod_spill2");
    uint32_t rb;
    js_branch_root(&ref, &RZ, 31, &rb);
    for (uint32_t i = 0; i < n; i++) js_branch_derive(&ref, rb, STEP_TOKEN);
    uint8_t a[32], b[32];
    digest(&s, fin.id, a);
    digest(&ref, rb, b);
    CHECK(!memcmp(a, b, 32), "committed J-Space state is not a serial history (%u steps)", n);
    js_space_destroy(&ref);

    /* An older World value still reads the bytes it named. */
    if (first.id != UINT32_MAX) {
        CHECK(js_branch_check(&s, first) == JS_OK, "older committed branch reclaimed");
        digest(&s, first.id, a);
        CHECK(!memcmp(a, first_dig, 32), "older committed bytes changed");
        uint8_t p[4] = { 9, 9, 9, 9 };
        CHECK(js_branch_edit(&s, first.id, 0, 0, p, 4) == JS_ERR_FROZEN, "committed branch mutable in place");
        /* Superseded and released: a stale World value is refused, never aliased. */
        CHECK(js_branch_release_ref(&s, first, SUBJ_WRITER) == JS_OK, "release superseded");
        CHECK(js_branch_check(&s, first) == JS_ERR_STALE, "stale World value accepted");
    }
    rx_world_destroy(&w);
    rx_caproot_stop(&root, &admin);
    js_space_destroy(&s);
    printf("  world: %u writer activations, %u committed steps\n", u.ran, n);
}

/* ---- Lane 32: generations never wrap (no ABA) ------------------------------
 *
 * A slot is driven to the last live generation its type can hold (standing in
 * for ~2^64 realization reuses or ~2^32 branch reuses) by writing the public
 * struct field, then released and reused. The exhausted slot must never be
 * handed out again and no older reference may validate. The top is computed
 * from the field's own type so this test also compiles against 32-bit
 * realization generations, where it fails (the slot wraps to generation 0 and
 * the first occupant's reference validates again). */
#define GEN_TOP(T) ((T)~(T)0)
typedef __typeof__(((JsReal *)0)->gen) RealGen;
typedef __typeof__(((JsBranchRef *)0)->gen) BranchGen;

static void test_gen_exhaustion(void) {
    static JsSpace s;
    /* Realization slot. */
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");
    uint32_t root;
    js_branch_root(&s, &RZ, 77, &root);
    JsBranchRef rr, c;
    js_branch_ref(&s, root, &rr);
    CHECK(js_branch_fork_staged(&s, rr, 0, &c) == JS_OK, "fork");
    CHECK(js_branch_derive(&s, c.id, 700) == JS_OK, "derive");
    uint32_t last = s.branches[c.id]->n_units - 1;
    JsRealId first, top;
    js_real_id(&s, c.id, last, &first);
    CHECK(first.gen == 0, "first occupant generation %llu", (unsigned long long)first.gen);
    JsReal *r = unit_of(&s, c.id, last);
    r->gen = GEN_TOP(RealGen) - 1;            /* the last live generation */
    js_real_id(&s, c.id, last, &top);
    CHECK(js_real_lookup(&s, top) == r, "top-generation reference resolves while live");
    CHECK(js_branch_release_ref(&s, c, 0) == JS_OK, "release at top generation");
    CHECK(js_real_lookup(&s, top) == NULL, "released top-generation reference resolves");
    for (int k = 0; k < 4; k++) {
        CHECK(js_branch_fork_staged(&s, rr, 0, &c) == JS_OK, "refork %d", k);
        CHECK(js_branch_derive(&s, c.id, 701 + (uint64_t)k) == JS_OK, "rederive %d", k);
        JsRealId now;
        js_real_id(&s, c.id, s.branches[c.id]->n_units - 1, &now);
        CHECK(now.slot != first.slot, "cycle %d: exhausted realization slot %u handed out again (gen %llu)",
              k, now.slot, (unsigned long long)now.gen);
        CHECK(js_real_lookup(&s, first) == NULL, "cycle %d: first occupant's reference validates (ABA)", k);
        CHECK(js_real_lookup(&s, top) == NULL, "cycle %d: top-generation reference validates", k);
        CHECK(js_branch_release_ref(&s, c, 0) == JS_OK, "release %d", k);
    }
    js_space_destroy(&s);

    /* Branch slot: 32-bit generations (a JsBranchRef packs into a World field). */
    CHECK(js_space_init(&s, "/tmp/jspace_prod_spill") == JS_OK, "init");
    js_branch_root(&s, &RZ, 78, &root);
    js_branch_ref(&s, root, &rr);
    JsBranchRef b0, btop;
    CHECK(js_branch_fork_staged(&s, rr, 0, &b0) == JS_OK, "fork b0");
    CHECK(b0.gen == 0, "first branch occupant generation %u", b0.gen);
    s.branch_gen[b0.id] = GEN_TOP(BranchGen) - 1;
    s.branches[b0.id]->gen = GEN_TOP(BranchGen) - 1;
    btop = (JsBranchRef){ b0.id, GEN_TOP(BranchGen) - 1 };
    CHECK(js_branch_check(&s, btop) == JS_OK, "top-generation branch ref valid while live");
    CHECK(js_branch_check(&s, b0) == JS_ERR_STALE, "older branch ref valid");
    CHECK(js_branch_release_ref(&s, btop, 0) == JS_OK, "release branch at top generation");
    for (int k = 0; k < 4; k++) {
        JsBranchRef nb;
        CHECK(js_branch_fork_staged(&s, rr, 0, &nb) == JS_OK, "branch refork %d", k);
        CHECK(nb.id != b0.id, "cycle %d: exhausted branch slot %u handed out again (gen %u)", k, nb.id, nb.gen);
        CHECK(js_branch_check(&s, b0) == JS_ERR_STALE, "cycle %d: first branch reference validates (ABA)", k);
        CHECK(js_branch_check(&s, btop) == JS_ERR_STALE, "cycle %d: top branch reference validates", k);
        CHECK(js_branch_release_ref(&s, nb, 0) == JS_OK, "branch release %d", k);
    }
    /* A full space whose only free slot retired refuses a new branch. */
    js_space_destroy(&s);
    JsLimits l = lim(64, 2, 0, 0);
    CHECK(js_space_init_limits(&s, "/tmp/jspace_prod_spill", &l) == JS_OK, "init 2 branches");
    js_branch_root(&s, &RZ, 79, &root);
    js_branch_ref(&s, root, &rr);
    CHECK(js_branch_fork_staged(&s, rr, 0, &b0) == JS_OK, "fork into the last slot");
    s.branch_gen[b0.id] = GEN_TOP(BranchGen) - 1;
    s.branches[b0.id]->gen = GEN_TOP(BranchGen) - 1;
    btop = (JsBranchRef){ b0.id, GEN_TOP(BranchGen) - 1 };
    CHECK(js_branch_release_ref(&s, btop, 0) == JS_OK, "release last slot at top generation");
    JsBranchRef nb;
    CHECK(js_branch_fork_staged(&s, rr, 0, &nb) == JS_ERR_FULL, "retired slot reused instead of JS_ERR_FULL");
    js_space_destroy(&s);
}

/* Durable: a retired branch slot stays retired after reopen, a realization
 * generation above 2^32 comes back whole, and a version-1 checkpoint (32-bit
 * realization generations) is refused with JS_ERR_VERSION. */
static void test_gen_durable(void) {
    char dir[64], meta[128], data[128];
    snprintf(dir, sizeof dir, "/tmp/jspace_gen_XXXXXX");
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    snprintf(meta, sizeof meta, "%s/jspace.meta", dir);
    snprintf(data, sizeof data, "%s/jspace.data", dir);
    static JsSpace s;
    JsHome me = { .locality = JS_HOME_LOCAL };
    memset(me.machine, 0x11, sizeof me.machine);
    CHECK(js_space_open(&s, dir, RZS, 2, NULL, &me) == JS_OK, "fresh open");
    uint32_t a, b, c;
    js_branch_root(&s, &RZ, 90, &a);
    for (unsigned i = 0; i < 3; i++) js_branch_derive(&s, a, 900 + i);
    CHECK(js_branch_fork(&s, a, &b) == JS_OK, "fork b");
    CHECK(js_branch_derive(&s, b, 950) == JS_OK, "derive b");
    uint32_t bl = s.branches[b]->n_units - 1;
    const RealGen wide = sizeof(RealGen) == 8 ? (RealGen)((1ull << 32) + 5) : (RealGen)5;
    CHECK(sizeof(RealGen) == 8, "realization generations are %zu bytes, not 8", sizeof(RealGen));
    unit_of(&s, b, bl)->gen = wide;
    JsRealId wid;
    js_real_id(&s, b, bl, &wid);
    CHECK(js_branch_fork(&s, a, &c) == JS_OK, "fork c");
    s.branch_gen[c] = GEN_TOP(BranchGen) - 1;
    s.branches[c]->gen = GEN_TOP(BranchGen) - 1;
    CHECK(js_branch_release(&s, c) == JS_OK, "release c at top generation");
    CHECK(js_space_commit(&s) == JS_OK, "commit");
    js_space_destroy(&s);

    CHECK(js_space_open(&s, dir, RZS, 2, NULL, &me) == JS_OK, "reopen");
    JsRealId got;
    CHECK(js_real_id(&s, b, bl, &got) == JS_OK && got.gen == wid.gen && got.slot == wid.slot,
          "wide realization generation did not survive reopen (%llu vs %llu)",
          (unsigned long long)got.gen, (unsigned long long)wid.gen);
    JsRealId low = { wid.slot, (RealGen)(uint32_t)wid.gen };
    CHECK(js_real_lookup(&s, low) == NULL, "truncated generation resolves");
    CHECK(s.branch_gen[c] == GEN_TOP(BranchGen), "retired branch slot generation %u after reopen", s.branch_gen[c]);
    JsBranchRef ar;
    js_branch_ref(&s, a, &ar);
    for (int k = 0; k < 3; k++) {
        JsBranchRef nb;
        CHECK(js_branch_fork_staged(&s, ar, 0, &nb) == JS_OK, "fork after reopen %d", k);
        CHECK(nb.id != c, "retired branch slot %u handed out after reopen", c);
        CHECK(js_branch_check(&s, (JsBranchRef){ c, 0 }) == JS_ERR_STALE, "first ref of retired slot valid");
        js_branch_release_ref(&s, nb, 0);
    }
    js_space_destroy(&s);

    /* Rewrite the header as version 1 (header hash recomputed): refused. */
    int fd = open(meta, O_RDWR);
    uint8_t h[128];
    CHECK(fd >= 0 && pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h, "read header");
    h[8] = 1; h[9] = h[10] = h[11] = 0;
    sha256_hash(h, 96, h + 96);
    CHECK(fd >= 0 && pwrite(fd, h, sizeof h, 0) == (ssize_t)sizeof h, "write v1 header");
    if (fd >= 0) close(fd);
    int rc = js_space_open(&s, dir, RZS, 2, NULL, &me);
    CHECK(rc == JS_ERR_VERSION, "version-1 checkpoint: open rc %d, want JS_ERR_VERSION", rc);
    if (rc == JS_OK) js_space_destroy(&s);
    unlink(meta); unlink(data);
    rmdir(dir);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } t[] = {
        { "alloc_reclaim", test_alloc_reclaim },
        { "generations", test_generations },
        { "limits", test_limits },
        { "concurrency", test_concurrency },
        { "remote", test_remote },
        { "durable", test_durable },
        { "world_commit", test_world },
        { "gen_exhaustion", test_gen_exhaustion },
        { "gen_durable", test_gen_durable },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        int before = g_fail;
        t[i].fn();
        printf("%-14s %s\n", t[i].name, g_fail == before ? "PASS" : "FAIL");
    }
    unlink("/tmp/jspace_prod_spill");
    unlink("/tmp/jspace_prod_spill2");
    printf("checks=%d failed=%d\n", g_checks, g_fail);
    printf("M20_JSPACE_PROD=%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
