/*
 * rx_cortex_canon.c -- M20 Cortex canonicalization tests.
 *
 * Six checks, one per behavior the M20 brief names: canonical append,
 * provenance relation, typed recall, restart persistence, execution to
 * Cortex through the real World, duplicate-writer prevention.
 */
#include "runtime/rx_caproot.h"
#include "runtime/rx_cortex.h"
#include "runtime/rx_cortex_record.h"
#include "runtime/rx_world.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail, g_checks;
static const char *g_test;

#define CHECK(cond, ...) do {                                                   \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_fail++;                                                           \
            fprintf(stderr, "  FAIL %s:%d [%s] ", __FILE__, __LINE__, g_test);  \
            fprintf(stderr, __VA_ARGS__);                                       \
            fputc('\n', stderr);                                                \
        }                                                                       \
    } while (0)

static char g_dir[256];

static void path_of(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_dir, name);
}

static CxHeader hdr(uint32_t cls, uint32_t kind, uint64_t subject, uint64_t t) {
    CxHeader h;
    memset(&h, 0, sizeof h);
    h.cls = cls;
    h.kind = kind;
    h.subject = subject;
    h.t = t;
    return h;
}

/* 1. canonical append ------------------------------------------------------ */
static void test_append(void) {
    g_test = "canonical append";
    printf("[*] %s\n", g_test);
    CxStore s;
    CHECK(cx_init(&s, 8) == CX_OK, "init");
    uint64_t pay[3] = { 7, 8, 9 }, id1 = 0, id2 = 0;
    CxHeader h = hdr(CX_CLAIM, CX_K_CLAIM, 2, 10);
    CHECK(cx_append(&s, &h, pay, 3, &id1) == CX_OK && id1 == 1, "first append id %llu",
          (unsigned long long)id1);
    h = hdr(CX_OBSERVATION, CX_K_EVIDENCE_REF, 2, 11);
    CHECK(cx_append(&s, &h, pay, 1, &id2) == CX_OK && id2 == 2, "second append");
    h = hdr(CX_CLAIM, CX_K_CLAIM, 2, 5);
    CHECK(cx_append(&s, &h, pay, 1, NULL) == CX_ERR_ORDER, "time went backwards");
    h = hdr(0, 0, 2, 20);
    CHECK(cx_append(&s, &h, NULL, 0, NULL) == CX_ERR_ARG, "class 0 accepted");
    h = hdr(CX_CLAIM, 0, 8, 20);
    CHECK(cx_append(&s, &h, NULL, 0, NULL) == CX_ERR_ARG, "subject out of range accepted");
    CHECK(s.n == 2, "append-only count %llu", (unsigned long long)s.n);
    CHECK(cx_verify(&s, 1) == CX_OK && cx_verify_chain(&s) == CX_OK, "digests");
    cx_tamper(&s, 1, 0, 99);
    CHECK(cx_verify(&s, 1) == CX_ERR_DIGEST && cx_verify_chain(&s) == CX_ERR_DIGEST,
          "tamper not detected");
    cx_free(&s);
}

/* 2. provenance relation ---------------------------------------------------- */
static void test_provenance(void) {
    g_test = "provenance relation";
    printf("[*] %s\n", g_test);
    CxStore s;
    cx_init(&s, 4);
    uint64_t obs = 0, ev = 0, cand = 0, prom = 0, w = 1;
    CxHeader h = hdr(CX_OBSERVATION, CX_K_EVIDENCE_REF, 1, 1);
    cx_append(&s, &h, &w, 1, &obs);
    h = hdr(CX_EVIDENCE, CX_K_EVIDENCE_REF, 1, 2);
    h.links[0] = obs;
    cx_append(&s, &h, &w, 1, &ev);
    h = hdr(CX_CLAIM, CX_K_CANDIDATE, 1, 3);
    cx_append(&s, &h, &w, 1, &cand);
    CHECK(cx_promote(&s, 0, ev, cand, 4, NULL) == CX_ERR_ARG, "promoted evidence as candidate");
    CHECK(cx_promote(&s, 0, cand, obs, 4, NULL) == CX_ERR_ARG, "observation accepted as evidence");
    CHECK(cx_promote(&s, 0, cand, ev, 4, &prom) == CX_OK && prom == 4, "promote");
    const CxObject *p = cx_get(&s, prom);
    CHECK(p && p->cls == CX_EVIDENCE && p->kind == CX_K_PROMOTION && p->links[0] == cand &&
          p->links[1] == ev && (p->protect & CX_PROT_VERIFY_EVIDENCE), "promotion record");
    uint64_t anc[8];
    uint32_t n = cx_provenance(&s, prom, anc, 8);
    CHECK(n == 3 && anc[0] == cand && anc[1] == ev && anc[2] == obs,
          "provenance walk n=%u", n);
    cx_free(&s);
}

/* 3. typed recall ----------------------------------------------------------- */
static void test_recall(void) {
    g_test = "typed recall";
    printf("[*] %s\n", g_test);
    CxStore s;
    cx_init(&s, 4);
    for (uint64_t t = 1; t <= 6; t++) {
        CxHeader h = hdr(t % 2 ? CX_CLAIM : CX_OBSERVATION, t % 2 ? CX_K_CLAIM : CX_K_EVIDENCE_REF,
                         t <= 4 ? 1 : 2, t);
        uint64_t pay[2] = { t, t * 10 };
        cx_append(&s, &h, pay, 2, NULL);
    }
    CxRecord r[8];
    CxFilter f = { CX_CLAIM, CX_K_CLAIM, 0, 0 };
    uint32_t n = cx_recall(&s, 1, 0, UINT64_MAX, &f, r, 8);
    CHECK(n == 2 && r[0].hdr.t == 1 && r[1].hdr.t == 3, "typed filter n=%u", n);
    CHECK(n == 2 && r[0].verified && r[0].payload[1] == 10 && r[1].payload[1] == 30, "payload");
    n = cx_recall(&s, 1, 2, 3, NULL, r, 8);
    CHECK(n == 2 && r[0].hdr.t == 2 && r[1].hdr.t == 3, "time range");
    n = cx_recall(&s, 1, 0, UINT64_MAX, NULL, r, 1);
    CHECK(n == 1, "max bound");
    cx_tamper(&s, 3, 0, 0);
    n = cx_recall(&s, 1, 3, 3, NULL, r, 8);
    CHECK(n == 1 && !r[0].verified, "tampered record reported verified");
    CxWorldRecord wr;
    CHECK(cx_recall_id(&s, 1, &r[0]) == CX_OK && cx_world_decode(&r[0], &wr) == CX_ERR_ARG,
          "non-World record decoded as World record");
    cx_free(&s);
}

/* 4. restart persistence ---------------------------------------------------- */
static void test_persistence(void) {
    g_test = "restart persistence";
    printf("[*] %s\n", g_test);
    char path[512];
    path_of(path, sizeof path, "persist.cx");
    CxStore s;
    CHECK(cx_open(&s, path, 16, CX_OPEN_SYNC) == CX_OK, "create journal");
    for (uint64_t t = 1; t <= 5; t++) {
        CxHeader h = hdr(CX_CLAIM, CX_K_CLAIM, t % 3, t);
        h.links[0] = t > 1 ? t - 1 : 0;
        uint64_t pay[3] = { t, t + 100, t + 200 };
        CHECK(cx_append(&s, &h, pay, 3, NULL) == CX_OK, "append %llu", (unsigned long long)t);
    }
    uint8_t chain[32];
    memcpy(chain, s.chain, 32);
    cx_close(&s);

    CHECK(cx_open(&s, path, 16, 0) == CX_OK, "reopen");
    CHECK(s.n == 5 && memcmp(s.chain, chain, 32) == 0 && cx_verify_chain(&s) == CX_OK,
          "replayed state differs");
    CxRecord r;
    CHECK(cx_recall_id(&s, 4, &r) == CX_OK && r.payload[2] == 204 && r.hdr.links[0] == 3,
          "recalled record differs");
    cx_close(&s);
    CxStore t;
    CHECK(cx_open(&t, path, 17, 0) == CX_ERR_FORMAT, "subject count mismatch accepted");

    /* A torn trailing record: refused, or dropped only on request. */
    struct stat st;
    stat(path, &st);
    CHECK(truncate(path, st.st_size - 5) == 0, "truncate");
    CHECK(cx_open(&t, path, 16, 0) == CX_ERR_TORN, "torn tail accepted silently");
    CHECK(cx_open(&t, path, 16, CX_OPEN_REPAIR_TAIL) == CX_OK && t.n == 4, "repair tail");
    cx_close(&t);

    /* A complete record with a changed byte is never repaired. */
    int fd = open(path, O_RDWR);
    uint8_t b = 0;
    off_t at = 32 + 8 * 14;   /* first record, payload word 1 */
    CHECK(pread(fd, &b, 1, at) == 1, "read");
    b ^= 1;
    CHECK(pwrite(fd, &b, 1, at) == 1, "write");
    close(fd);
    CHECK(cx_open(&t, path, 16, CX_OPEN_REPAIR_TAIL) == CX_ERR_DIGEST, "tampered journal opened");
}

/* 5. execution -> Cortex through the real World ------------------------------ */

enum { SUBJ_WORKER = 1, SUBJ_EXTERNAL = 100, ISSUER = 3, RES_IN = 0x10, RES_OUT = 0x20 };

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    RxWorld w;
    RxObjRef in, out;
    RxCapRef c_ext;
} Env;

static RxCapRef mint(Env *e, uint32_t subject, uint64_t resource, uint32_t rights) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = ISSUER;
    m.subject = subject;
    m.resource = resource;
    m.rights = rights;
    m.parent = (RxCapRef){ UINT32_MAX, 0 };
    m.authority = rx_capadmin_office(&e->admin);
    RxCapRef r = { UINT32_MAX, 0 };
    rx_capadmin_mint(&e->admin, &m, &r);
    return r;
}

/* The work: out.field0 = 2 * in.field0. */
static int fn_double(RxCtx *c) {
    const RxObjRef *out = c->user;
    c->out[c->n_out++] = (RxMutation){ *out, 0, c->in[0].field[0] * 2 };
    return 0;
}

static int env_start(Env *e) {
    memset(e, 0, sizeof *e);
    if (rx_caproot_start(&e->root, &e->admin) != RX_CAP_OK) return -1;
    if (rx_world_init(&e->w, &e->root, 2, 1u << 12) != RX_OK) return -1;
    e->w.external_subject = SUBJ_EXTERNAL;
    uint64_t init[RX_MAX_FIELDS] = { 0 };
    rx_world_create(&e->w, 1, RX_PERSIST_DURABLE, RES_IN, init, &e->in);
    rx_world_create(&e->w, 1, RX_PERSIST_DURABLE, RES_OUT, init, &e->out);
    e->c_ext = mint(e, SUBJ_EXTERNAL, RES_IN, RX_RIGHT_WRITE);
    RxReactionDesc d;
    memset(&d, 0, sizeof d);
    d.name = "work.double";
    d.faculty = RX_FACULTY_OMEGA;
    d.subject = SUBJ_WORKER;
    d.priority = RX_PRIO_FOREGROUND;
    d.fn = fn_double;
    d.user = &e->out;
    d.triggers[d.n_triggers++] = (RxDep){ e->in, RX_FIELD(0) };
    d.writes[d.n_writes++] = (RxDep){ e->out, RX_FIELD(0) };
    d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_WORKER, RES_IN, RX_RIGHT_READ), RES_IN,
                                      RX_RIGHT_READ };
    d.caps[d.n_caps++] = (RxCapNeed){ mint(e, SUBJ_WORKER, RES_OUT, RX_RIGHT_WRITE), RES_OUT,
                                      RX_RIGHT_WRITE };
    uint32_t rid;
    return rx_world_add_reaction(&e->w, &d, &rid) == RX_OK ? 0 : -1;
}

static void env_stop(Env *e) {
    rx_world_destroy(&e->w);
    rx_caproot_stop(&e->root, &e->admin);
}

static int64_t submit(Env *e, uint64_t v) {
    RxMutation m = { e->in, 0, v };
    return rx_world_publish_external(&e->w, e->c_ext, &m, 1);
}

static void test_execution(void) {
    g_test = "execution to Cortex";
    printf("[*] %s\n", g_test);
    char path[512];
    path_of(path, sizeof path, "world.cx");
    CxStore s;
    CHECK(cx_open(&s, path, RX_CORTEX_SUBJECTS, 0) == CX_OK, "open journal");
    Env e;
    CHECK(env_start(&e) == 0, "world start");
    CHECK(rx_cortex_attach(&e.w, &s, 1) == RX_OK, "attach");
    int64_t ext = submit(&e, 21);
    CHECK(ext > 0, "submit");
    CHECK(rx_world_wait_quiescent(&e.w, 10000) == RX_OK, "quiesce");
    uint64_t recs = 0, errs = 0;
    rx_cortex_status(&e.w, &recs, &errs);
    CHECK(errs == 0 && recs == e.w.n_crumbs, "records %llu crumbs %llu errors %llu",
          (unsigned long long)recs, (unsigned long long)e.w.n_crumbs, (unsigned long long)errs);

    /* Accepted work. */
    uint64_t acc_id = rx_cortex_record_of(&e.w, (uint64_t)ext);
    CxRecord r;
    CxWorldRecord acc, res;
    CHECK(acc_id && cx_recall_id(&s, acc_id, &r) == CX_OK && cx_world_decode(&r, &acc) == CX_OK &&
          acc.kind == CX_K_WORK_ACCEPTED && r.hdr.cls == CX_OBSERVATION &&
          acc.obj == e.in.id && acc.field[0] == 21, "accepted-work record");

    /* Result, with World evidence and provenance back to the accepted work. */
    uint64_t res_id = rx_cortex_recall_result(&s, e.out.id, &res);
    CHECK(res_id && res.field[0] == 42 && res.reaction != UINT32_MAX &&
          res.faculty == RX_FACULTY_OMEGA && res.session == 1, "result record");
    const RxCrumb *k = rx_world_crumb(&e.w, res.crumb);
    CHECK(k && k->kind == RX_CRUMB_COMMIT && memcmp(k->digest, res.crumb_digest, 32) == 0,
          "result does not reference the World commit crumb");
    CHECK(res.cause == acc_id && res.episode == (uint64_t)ext, "provenance to accepted work");
    uint64_t anc[8];
    uint32_t n = cx_provenance(&s, res_id, anc, 8);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) found |= anc[i] == acc_id;
    CHECK(found, "accepted work not in provenance");
    CHECK(cx_get(&s, res_id)->protect & CX_PROT_COMMIT_RECEIPT, "commit receipt not protected");
    env_stop(&e);             /* releases the writer claim */
    uint64_t n_first = s.n;
    cx_close(&s);

    /* Restart: a new World session on the same journal keeps the old
     * evidence recallable and keeps appending in order. */
    CHECK(cx_open(&s, path, 0, 0) == CX_OK && s.n == n_first, "reopen after restart");
    CHECK(rx_cortex_recall_result(&s, e.out.id, &res) == res_id && res.field[0] == 42,
          "result lost across restart");
    CHECK(env_start(&e) == 0 && rx_cortex_attach(&e.w, &s, 2) == RX_OK, "second session");
    CHECK(submit(&e, 5) > 0 && rx_world_wait_quiescent(&e.w, 10000) == RX_OK, "second submit");
    rx_cortex_status(&e.w, &recs, &errs);
    CHECK(errs == 0, "second session append errors %llu", (unsigned long long)errs);
    CHECK(rx_cortex_recall_result(&s, e.out.id, &res) > res_id && res.field[0] == 10 &&
          res.session == 2, "second session result");
    env_stop(&e);
    CHECK(cx_verify_chain(&s) == CX_OK, "chain");
    cx_close(&s);
}

/* 6. duplicate-writer prevention ------------------------------------------- */
static void test_single_writer(void) {
    g_test = "duplicate-writer prevention";
    printf("[*] %s\n", g_test);
    char path[512];
    path_of(path, sizeof path, "single.cx");
    CxStore a, b;
    CHECK(cx_open(&a, path, RX_CORTEX_SUBJECTS, 0) == CX_OK, "first writer");
    CHECK(cx_open(&b, path, RX_CORTEX_SUBJECTS, 0) == CX_ERR_WRITER, "second open in process");
    pid_t pid = fork();
    if (pid == 0) {
        CxStore c;
        _exit(cx_open(&c, path, RX_CORTEX_SUBJECTS, 0) == CX_ERR_WRITER ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "second process opened the journal for write");
    CHECK(cx_open(&b, path, 0, CX_OPEN_READONLY) == CX_OK, "read-only inspection");
    CxHeader h = hdr(CX_CLAIM, CX_K_CLAIM, 1, 1);
    CHECK(cx_append(&b, &h, NULL, 0, NULL) == CX_ERR_WRITER, "read-only store appended");
    cx_close(&b);

    Env e1, e2;
    CHECK(env_start(&e1) == 0 && env_start(&e2) == 0, "worlds");
    CHECK(rx_cortex_attach(&e1.w, &a, 1) == RX_OK, "attach one");
    CHECK(rx_cortex_attach(&e2.w, &a, 2) == RX_ERR_IDENTITY, "second World attached");
    CHECK(cx_append(&a, &h, NULL, 0, NULL) == CX_ERR_WRITER, "bypass write while attached");
    CHECK(cx_claim_writer(&a, 12345) == CX_ERR_WRITER, "second claim");
    CHECK(rx_cortex_attach(&e1.w, &a, 3) == RX_ERR_EXISTS, "double attach");
    CHECK(rx_cortex_detach(&e1.w) == RX_OK, "detach");
    CHECK(rx_cortex_attach(&e2.w, &a, 2) == RX_OK, "attach after detach");
    env_stop(&e1);
    env_stop(&e2);           /* destroy releases */
    CHECK(a.writer == 0, "claim not released by destroy");
    h.t = a.n + 1;
    CHECK(cx_append(&a, &h, NULL, 0, NULL) == CX_OK, "append after release");
    cx_close(&a);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/rx_cortex_canon.XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 2; }
    test_append();
    test_provenance();
    test_recall();
    test_persistence();
    test_execution();
    test_single_writer();
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "cleanup of %s failed\n", g_dir);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "M20 CORTEX TESTS: FAIL\n" : "M20 CORTEX TESTS: PASS\n");
    return g_fail ? 1 : 0;
}
