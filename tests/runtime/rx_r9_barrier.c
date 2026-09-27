/*
 * Qualification for the generation barrier. The important cases are a stop
 * after every persistence step, and the refusals around promotion authority.
 * The Linux checker and the native authority must agree on the promotion right.
 */
#include "runtime/aienos_cap.h"
#include "runtime/rx_caproot.h"
#include "runtime/rx_generation.h"
#include "runtime/rx_world.h"
#include "omega_evidence.h"
#include "sha256.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define SUBJ_PROPOSER 8u
#define SUBJ_AUTHORITY 4u
#define RES_LIVE 0x10ull

static int g_fail;
static int g_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);                \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

typedef struct {
    const char *name;
    int linux_rc;
    int native_rc;
} DiffRow;

static DiffRow g_diff[16];
static int g_ndiff;

static void expect_eq(const char *name, int linux_rc, int native_rc) {
    CHECK(linux_rc == native_rc, "%s linux %d native %d", name, linux_rc, native_rc);
    if (linux_rc == native_rc && g_ndiff < (int)(sizeof g_diff / sizeof g_diff[0])) {
        g_diff[g_ndiff].name = name;
        g_diff[g_ndiff].linux_rc = linux_rc;
        g_diff[g_ndiff].native_rc = native_rc;
        g_ndiff++;
    }
}

typedef struct {
    int step;
    int coherent;
    uint64_t lineage;
    int receipt_after;
    char evidence[32];
} CrashRow;

static CrashRow g_crash[8];
static int g_ncrash;

typedef struct {
    RxCapRoot root;
    RxCapAdmin admin;
    AienosCapAdmin *native_admin;
    AienosCapView *native_view;
} Auth;

static int auth_start(Auth *a) {
    memset(a, 0, sizeof(*a));
    if (rx_caproot_start(&a->root, &a->admin) != RX_CAP_OK) return -1;
    if (aienos_cap_start(&a->native_admin, &a->native_view) != 0) {
        rx_caproot_stop(&a->root, &a->admin);
        return -1;
    }
    return 0;
}

static void auth_stop(Auth *a) {
    aienos_cap_stop(a->native_admin, a->native_view);
    rx_caproot_stop(&a->root, &a->admin);
}

static int mint_both(Auth *a, uint32_t subject, uint32_t rights, RxCapRef *linux_out,
                     AienosCapRef *native_out) {
    RxCapMint m;
    memset(&m, 0, sizeof m);
    m.issuer = 3;
    m.subject = subject;
    m.resource = RX_GEN_RES_PROMOTION;
    m.rights = rights;
    m.parent = (RxCapRef){UINT32_MAX, 0};
    m.authority = rx_capadmin_office(&a->admin);
    int lrc = rx_capadmin_mint(&a->admin, &m, linux_out);
    AienosCapMint n;
    memset(&n, 0, sizeof n);
    n.issuer = 3;
    n.subject = subject;
    n.resource = RX_GEN_RES_PROMOTION;
    n.rights = rights;
    n.parent.cap_id = UINT32_MAX;
    if (aienos_cap_office(a->native_admin, &n.authority) != 0) return -1;
    int nrc = aienos_cap_mint(a->native_admin, &n, native_out);
    if (lrc != nrc) return -1;
    return lrc;
}

static int native_auth(void *ctx, uint32_t cap_id, uint32_t cap_generation, uint32_t subject,
                       uint64_t resource, uint32_t rights) {
    Auth *a = ctx;
    AienosCapRef ref = {cap_id, cap_generation};
    AienosCapEntry entry;
    return aienos_cap_validate(a->native_view, ref, subject, resource, rights, &entry);
}

static void draft_init(RxGenDraft *d, const uint8_t *evidence, size_t n) {
    memset(d, 0, sizeof(*d));
    d->authority_epoch = 1;
    d->authority_generation = 1;
    d->proofs_ok = 1;
    d->evidence = evidence;
    d->evidence_len = n;
    static const uint8_t model[] = "model-unused";
    static const uint8_t realization[] = "realization";
    static const uint8_t config[] = "config";
    static const uint8_t provenance[] = "host-proof";
    d->model = model;
    d->model_len = sizeof model - 1;
    d->realization = realization;
    d->realization_len = sizeof realization - 1;
    d->config = config;
    d->config_len = sizeof config - 1;
    d->provenance = provenance;
    d->provenance_len = sizeof provenance - 1;
}

static int read_text(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = 0;
    return (int)got;
}

static int evidence_of(const char *dir, uint64_t id, char *buf, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/g/%llu/evidence", dir, (unsigned long long)id);
    return read_text(path, buf, n);
}

static void rm_tree(const char *dir) {
    char cmd[640];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    int rc = system(cmd);
    (void)rc;
}

static int fresh_dir(char *out, size_t n) {
    char tmpl[] = "/tmp/r9-barrier-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return -1;
    if (strlen(dir) + 1 > n) return -1;
    memcpy(out, dir, strlen(dir) + 1);
    return 0;
}

static RxPromotionRequest request_for(uint64_t id, uint32_t subject, AienosCapRef cap) {
    RxPromotionRequest r;
    memset(&r, 0, sizeof r);
    r.candidate_id = id;
    r.subject = subject;
    r.cap_id = cap.cap_id;
    r.cap_generation = cap.generation;
    r.resource = RX_GEN_RES_PROMOTION;
    r.rights = RX_GEN_RIGHT_PROMOTE;
    return r;
}

static void authority_differential(void) {
    Auth a;
    CHECK(auth_start(&a) == 0, "authority did not start");
    RxCapRef linux_cap = {0, 0};
    AienosCapRef native_cap = {0, 0};
    int mrc = mint_both(&a, SUBJ_AUTHORITY, RX_RIGHT_PROMOTE, &linux_cap, &native_cap);
    CHECK(mrc == RX_CAP_OK, "promote mint failed %d", mrc);
    RxCapEntry lentry;
    AienosCapEntry nentry;
    int lrc = rx_caproot_validate(&a.root, linux_cap, SUBJ_AUTHORITY, RX_GEN_RES_PROMOTION,
                                  RX_RIGHT_PROMOTE, &lentry);
    int nrc = aienos_cap_validate(a.native_view, native_cap, SUBJ_AUTHORITY, RX_GEN_RES_PROMOTION,
                                  RX_RIGHT_PROMOTE, &nentry);
    expect_eq("promote right validates", lrc, nrc);

    RxCapRef bad_l = {0, 0};
    AienosCapRef bad_n = {0, 0};
    RxCapMint badm;
    memset(&badm, 0, sizeof badm);
    badm.issuer = 3;
    badm.subject = SUBJ_AUTHORITY;
    badm.resource = RX_GEN_RES_PROMOTION;
    badm.rights = RX_RIGHT_PROMOTE | RX_RIGHT_DELEGATE;
    badm.parent = (RxCapRef){UINT32_MAX, 0};
    badm.authority = rx_capadmin_office(&a.admin);
    lrc = rx_capadmin_mint(&a.admin, &badm, &bad_l);
    AienosCapMint nbad;
    memset(&nbad, 0, sizeof nbad);
    nbad.issuer = 3;
    nbad.subject = SUBJ_AUTHORITY;
    nbad.resource = RX_GEN_RES_PROMOTION;
    nbad.rights = RX_RIGHT_PROMOTE | RX_RIGHT_DELEGATE;
    nbad.parent.cap_id = UINT32_MAX;
    aienos_cap_office(a.native_admin, &nbad.authority);
    nrc = aienos_cap_mint(a.native_admin, &nbad, &bad_n);
    expect_eq("promote cannot be combined with hand-off", lrc, nrc);

    RxCapMint child;
    memset(&child, 0, sizeof child);
    child.issuer = SUBJ_AUTHORITY;
    child.subject = 5;
    child.resource = RX_GEN_RES_PROMOTION;
    child.rights = RX_RIGHT_PROMOTE;
    child.parent = linux_cap;
    child.authority = linux_cap;
    RxCapRef cout = {0, 0};
    lrc = rx_capadmin_mint(&a.admin, &child, &cout);
    AienosCapMint nchild;
    memset(&nchild, 0, sizeof nchild);
    nchild.issuer = SUBJ_AUTHORITY;
    nchild.subject = 5;
    nchild.resource = RX_GEN_RES_PROMOTION;
    nchild.rights = RX_RIGHT_PROMOTE;
    nchild.parent = native_cap;
    nchild.authority = native_cap;
    AienosCapRef nout = {0, 0};
    nrc = aienos_cap_mint(a.native_admin, &nchild, &nout);
    expect_eq("promote right cannot be handed on", lrc, nrc);

    lrc = rx_capadmin_revoke(&a.admin, rx_capadmin_office(&a.admin), linux_cap);
    AienosCapRef office;
    aienos_cap_office(a.native_admin, &office);
    nrc = aienos_cap_revoke(a.native_admin, office, native_cap);
    expect_eq("revoke promotion right", lrc, nrc);
    lrc = rx_caproot_validate(&a.root, linux_cap, SUBJ_AUTHORITY, RX_GEN_RES_PROMOTION,
                              RX_RIGHT_PROMOTE, &lentry);
    nrc = aienos_cap_validate(a.native_view, native_cap, SUBJ_AUTHORITY, RX_GEN_RES_PROMOTION,
                              RX_RIGHT_PROMOTE, &nentry);
    expect_eq("revoked promotion right", lrc, nrc);

    AienosCapMint forged;
    memset(&forged, 0, sizeof forged);
    forged.subject = SUBJ_PROPOSER;
    forged.resource = RX_GEN_RES_PROMOTION;
    forged.rights = RX_RIGHT_PROMOTE;
    forged.parent.cap_id = UINT32_MAX;
    uint8_t token[32];
    memset(token, 0, sizeof token);
    nrc = aienos_cap_cognition_mint(a.native_view, &forged, token);
    expect_eq("reaction cannot mint a promotion", RX_CAP_ERR_UNAUTHORIZED, nrc);
    auth_stop(&a);
}

static int child_crash(const char *dir, int step) {
    Auth a;
    if (auth_start(&a) != 0) return 2;
    RxCapRef ignore = {0, 0};
    AienosCapRef cap = {0, 0};
    if (mint_both(&a, SUBJ_AUTHORITY, RX_RIGHT_PROMOTE, &ignore, &cap) != RX_CAP_OK) return 3;
    RxGenStore *store = NULL;
    if (rx_gen_open(dir, &store) != RX_GEN_OK) return 4;
    const uint8_t evidence[] = "CANDIDATE";
    RxGenObject obj;
    memset(&obj, 0, sizeof obj);
    obj.id = 1;
    obj.generation = 1;
    obj.digest[0] = 9;
    RxGenDraft draft;
    draft_init(&draft, evidence, sizeof evidence - 1);
    draft.objects = &obj;
    draft.n_objects = 1;
    uint64_t id = 0;
    if (rx_gen_propose(store, SUBJ_PROPOSER, &draft, &id) != RX_GEN_OK) return 5;
    RxGenWork effect = {42, RX_WORK_EXTERNAL, RX_WORK_ISSUED, 0};
    if (rx_gen_add_work(store, id, &effect) != RX_GEN_OK) return 6;
    rx_gen_set_crash(store, step);
    RxPromotionRequest req = request_for(id, SUBJ_AUTHORITY, cap);
    int rc = rx_gen_promote(store, &req, native_auth, &a, NULL, NULL, NULL, NULL);
    auth_stop(&a);
    rx_gen_close(store);
    return rc == RX_GEN_OK ? 0 : 7;
}

static int child_busy(const char *dir) {
    Auth a;
    if (auth_start(&a) != 0) return 21;
    RxCapRef ignore = {0, 0};
    AienosCapRef cap = {0, 0};
    if (mint_both(&a, SUBJ_AUTHORITY, RX_RIGHT_PROMOTE, &ignore, &cap) != RX_CAP_OK) return 21;
    RxGenStore *store = NULL;
    if (rx_gen_open(dir, &store) != RX_GEN_OK) return 21;
    const uint8_t evidence[] = "OTHER";
    RxGenDraft draft;
    draft_init(&draft, evidence, sizeof evidence - 1);
    uint64_t id = 0;
    if (rx_gen_propose(store, SUBJ_PROPOSER, &draft, &id) != RX_GEN_OK) return 21;
    RxPromotionRequest req = request_for(id, SUBJ_AUTHORITY, cap);
    int rc = rx_gen_promote(store, &req, native_auth, &a, NULL, NULL, NULL, NULL);
    auth_stop(&a);
    rx_gen_close(store);
    return rc == RX_GEN_ERR_BUSY ? 20 : 21;
}

static int run_child(const char *argv0, const char *dir, const char *mode, int step) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char step_text[16];
        snprintf(step_text, sizeof step_text, "%d", step);
        execl(argv0, argv0, mode, dir, step_text, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static void check_crash(const char *argv0, int step, uint64_t want_lineage, const char *want_evidence,
                        int receipt_before_recover) {
    char dir[64];
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    int code = run_child(argv0, dir, "--crash", step);
    CHECK(code == 86, "crash step %d exited %d", step, code);
    uint64_t id_guess = want_lineage == 1 ? 1 : 2;
    char path[512];
    snprintf(path, sizeof path, "%s/g/%llu/receipt", dir, (unsigned long long)id_guess);
    int receipt_before = access(path, R_OK) == 0;
    if (want_lineage == 1) {
        CHECK(!receipt_before, "step %d wrote a receipt for a generation that is not active", step);
    } else if (!receipt_before_recover) {
        CHECK(!receipt_before, "step %d wrote the receipt before the receipt step", step);
    } else {
        CHECK(receipt_before, "step %d was missing the receipt", step);
    }
    RxRecoveryRecord rec;
    int rc = rx_gen_recover(dir, &rec);
    CHECK(rc == RX_GEN_OK, "step %d recovery %d", step, rc);
    CHECK(rec.coherent, "step %d incoherent", step);
    CHECK(rec.lineage == want_lineage, "step %d lineage %llu", step,
          (unsigned long long)rec.lineage);
    char evidence[32];
    CHECK(evidence_of(dir, rec.active_id, evidence, sizeof evidence) > 0, "step %d evidence missing",
          step);
    CHECK(strcmp(evidence, want_evidence) == 0, "step %d evidence '%s'", step, evidence);
    CHECK(strcmp(evidence, "GENESIS") == 0 || strcmp(evidence, "CANDIDATE") == 0,
          "step %d mixed evidence", step);
    if (want_lineage == 2) {
        CHECK(rec.receipt_present, "step %d receipt not completed", step);
        CHECK(rec.event_present, "step %d change was not published", step);
        CHECK(rx_gen_reject_replay(dir, 42) == RX_GEN_ERR_REPLAY, "step %d replayed an effect", step);
    } else {
        CHECK(rx_gen_reject_replay(dir, 42) == RX_GEN_OK, "step %d recorded an effect that was not committed",
              step);
    }
    if (g_ncrash < 8) {
        g_crash[g_ncrash].step = step;
        g_crash[g_ncrash].coherent = rec.coherent;
        g_crash[g_ncrash].lineage = rec.lineage;
        g_crash[g_ncrash].receipt_after = rec.receipt_present;
        snprintf(g_crash[g_ncrash].evidence, sizeof g_crash[0].evidence, "%s", evidence);
        g_ncrash++;
    }
    rm_tree(dir);
}

static RxWorld g_world;
static RxCapRoot g_root;
static RxCapAdmin g_admin;
static RxCapRef g_live_cap;
static RxObjRef g_live_obj;
static volatile int g_stop;
static volatile int g_live_ok;
static volatile int g_live_bad;
static RxGenStore *g_store;
static uint64_t g_cand;
static int g_closed_mutation;

static void *live_thread(void *arg) {
    (void)arg;
    uint64_t value = 1;
    while (!g_stop) {
        RxMutation m = {g_live_obj, 0, value++};
        int64_t id = rx_world_publish_external(&g_world, g_live_cap, &m, 1);
        if (id >= 0) g_live_ok++;
        else g_live_bad++;
    }
    return NULL;
}

static void on_live(void *ctx) {
    (void)ctx;
    const uint8_t nope[] = "nope";
    g_closed_mutation = rx_gen_set_evidence(g_store, g_cand, nope, sizeof nope - 1);
    RxMutation m = {g_live_obj, 0, 99};
    int64_t id = rx_world_publish_external(&g_world, g_live_cap, &m, 1);
    if (id >= 0) g_live_ok++;
    else g_live_bad++;
}

typedef struct {
    RxGenStore *store;
    uint64_t candidate;
} DrainCtx;

static int on_drain(uint64_t work_id, void *ctx) {
    DrainCtx *d = ctx;
    const uint8_t text[] = "DRAINED";
    if (rx_gen_set_evidence(d->store, d->candidate, text, sizeof text - 1) != RX_GEN_OK) return -1;
    return rx_gen_finish_work(d->store, d->candidate, work_id);
}

static void flip_evidence(const char *generation_dir, void *ctx) {
    (void)ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/evidence", generation_dir);
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    int byte = fgetc(f);
    if (byte >= 0) {
        fseek(f, 0, SEEK_SET);
        fputc(byte ^ 0xff, f);
    }
    fclose(f);
}

static void drop_objects(const char *generation_dir, void *ctx) {
    (void)ctx;
    char path[512];
    snprintf(path, sizeof path, "%s/objects", generation_dir);
    unlink(path);
}

static int promote_ok(Auth *a, RxGenStore *store, uint64_t id, AienosCapRef cap, RxGenDrainFn drain,
                      void *drain_ctx, RxGenLiveFn live) {
    RxPromotionRequest req = request_for(id, SUBJ_AUTHORITY, cap);
    return rx_gen_promote(store, &req, native_auth, a, drain, drain_ctx, live, NULL);
}

static void negative_and_live(void) {
    Auth a;
    CHECK(auth_start(&a) == 0, "authority did not start");
    CHECK(rx_caproot_start(&g_root, &g_admin) == RX_CAP_OK, "live world authority");
    CHECK(rx_world_init(&g_world, &g_root, 1, 1u << 16) == RX_OK, "live world");
    g_world.external_subject = 100;
    uint64_t init[RX_MAX_FIELDS] = {0};
    CHECK(rx_world_create(&g_world, 1, RX_PERSIST_RESIDENT, RES_LIVE, init, &g_live_obj) == RX_OK,
          "live object");
    RxCapMint wm;
    memset(&wm, 0, sizeof wm);
    wm.issuer = 3;
    wm.subject = 100;
    wm.resource = RES_LIVE;
    wm.rights = RX_RIGHT_WRITE;
    wm.parent = (RxCapRef){UINT32_MAX, 0};
    wm.authority = rx_capadmin_office(&g_admin);
    CHECK(rx_capadmin_mint(&g_admin, &wm, &g_live_cap) == RX_CAP_OK, "live write right");

    RxCapRef linux_cap = {0, 0};
    AienosCapRef cap = {0, 0};
    CHECK(mint_both(&a, SUBJ_AUTHORITY, RX_RIGHT_PROMOTE, &linux_cap, &cap) == RX_CAP_OK,
          "authority promote right");
    RxCapRef self_l = {0, 0};
    AienosCapRef self_n = {0, 0};
    CHECK(mint_both(&a, SUBJ_PROPOSER, RX_RIGHT_PROMOTE, &self_l, &self_n) == RX_CAP_OK,
          "proposer was given a promotion right for the refusal test");

    char dir[64];
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    RxGenStore *store = NULL;
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    g_store = store;
    const uint8_t evidence[] = "CANDIDATE";
    RxGenObject obj;
    memset(&obj, 0, sizeof obj);
    obj.id = 7;
    obj.generation = 1;
    obj.digest[0] = 3;
    RxGenDraft draft;
    draft_init(&draft, evidence, sizeof evidence - 1);
    draft.objects = &obj;
    draft.n_objects = 1;
    uint64_t first = 0, second = 0;
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "first candidate");
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &second) == RX_GEN_OK, "second candidate");
    g_cand = first;
    RxGenWork ephemeral = {1, RX_WORK_EPHEMERAL, RX_WORK_PENDING, 0};
    RxGenWork optional = {7, RX_WORK_EVIDENCE, RX_WORK_PENDING, 0};
    RxGenWork required = {3, RX_WORK_EVIDENCE, RX_WORK_PENDING, 1};
    RxGenWork effect = {11, RX_WORK_EXTERNAL, RX_WORK_ISSUED, 0};
    CHECK(rx_gen_add_work(store, first, &ephemeral) == RX_GEN_OK, "ephemeral");
    CHECK(rx_gen_add_work(store, first, &optional) == RX_GEN_OK, "optional evidence");
    CHECK(rx_gen_add_work(store, first, &required) == RX_GEN_OK, "required evidence");
    CHECK(rx_gen_add_work(store, first, &effect) == RX_GEN_OK, "external effect");

    RxPromotionRequest self = request_for(first, SUBJ_PROPOSER, self_n);
    CHECK(rx_gen_promote(store, &self, native_auth, &a, NULL, NULL, NULL, NULL) ==
              RX_GEN_ERR_AUTHORITY,
          "candidate authorized its own promotion");
    uint64_t active = 0, lineage = 0;
    rx_gen_active(store, &active, &lineage);
    CHECK(lineage == 1, "self-promotion changed the active generation");

    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "reopen");
    g_store = store;
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "candidate");
    g_cand = first;
    AienosCapRef office;
    aienos_cap_office(a.native_admin, &office);
    CHECK(aienos_cap_revoke(a.native_admin, office, cap) == 0, "revoke");
    CHECK(promote_ok(&a, store, first, cap, NULL, NULL, NULL) == RX_GEN_ERR_AUTHORITY,
          "revoked authority promoted");
    rx_gen_active(store, &active, &lineage);
    CHECK(lineage == 1, "revoked promotion changed the generation");

    /* A fresh authority, because the previous one was revoked. */
    auth_stop(&a);
    CHECK(auth_start(&a) == 0, "restart authority");
    CHECK(mint_both(&a, SUBJ_AUTHORITY, RX_RIGHT_PROMOTE, &linux_cap, &cap) == RX_CAP_OK, "remint");
    CHECK(mint_both(&a, SUBJ_PROPOSER, RX_RIGHT_PROMOTE, &self_l, &self_n) == RX_CAP_OK, "remint self");
    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open for the live promotion");
    g_store = store;
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "live candidate");
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &second) == RX_GEN_OK, "sibling candidate");
    g_cand = first;
    CHECK(rx_gen_add_work(store, first, &ephemeral) == RX_GEN_OK, "ephemeral");
    CHECK(rx_gen_add_work(store, first, &optional) == RX_GEN_OK, "optional");
    CHECK(rx_gen_add_work(store, first, &required) == RX_GEN_OK, "required");
    CHECK(rx_gen_add_work(store, first, &effect) == RX_GEN_OK, "effect");
    self = request_for(first, SUBJ_PROPOSER, self_n);
    CHECK(rx_gen_promote(store, &self, native_auth, &a, NULL, NULL, NULL, NULL) == RX_GEN_ERR_AUTHORITY,
          "candidate authorized itself");

    g_stop = 0;
    g_live_ok = 0;
    g_live_bad = 0;
    g_closed_mutation = 0;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, live_thread, NULL) == 0, "live thread");
    DrainCtx drain = {store, first};
    int prc = promote_ok(&a, store, first, cap, on_drain, &drain, on_live);
    g_stop = 1;
    pthread_join(thread, NULL);
    CHECK(prc == RX_GEN_OK, "promotion failed %d", prc);
    CHECK(g_closed_mutation == RX_GEN_ERR_CLOSING, "candidate accepted a change while closing");
    CHECK(g_live_ok > 0 && g_live_bad == 0, "live reactions stopped or failed (%d ok, %d bad)",
          g_live_ok, g_live_bad);
    rx_gen_active(store, &active, &lineage);
    CHECK(lineage == 2 && active == first, "active lineage %llu", (unsigned long long)lineage);
    char ev[32];
    CHECK(evidence_of(dir, active, ev, sizeof ev) > 0, "evidence missing");
    CHECK(strcmp(ev, "DRAINED") == 0, "evidence was '%s'", ev);
    RxRecoveryRecord rec;
    CHECK(rx_gen_recover(dir, &rec) == RX_GEN_OK, "recover after promotion");
    CHECK(rec.n_excluded == 1 && rec.excluded_ids[0] == 7, "optional evidence was not excluded");
    CHECK(rec.n_external == 1 && rec.external_ids[0] == 11, "external effect was not frozen");
    CHECK(rx_gen_reject_replay(dir, 11) == RX_GEN_ERR_REPLAY, "effect was replayed");
    int events = 0;
    char epath[512];
    snprintf(epath, sizeof epath, "%s/events", dir);
    FILE *ef = fopen(epath, "r");
    unsigned long long eid = 0, elin = 0;
    if (ef) {
        while (fscanf(ef, "%llu %llu", &eid, &elin) == 2) events++;
        fclose(ef);
    }
    CHECK(events == 1, "generation change was published %d times", events);
    CHECK(promote_ok(&a, store, second, cap, NULL, NULL, NULL) == RX_GEN_ERR_STALE,
          "second candidate from the same parent was promoted");

    /* Stale object generation, before any commit. */
    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "stale candidate");
    CHECK(rx_gen_observe_object(store, first, 0, 1) == RX_GEN_OK, "observe");
    uint8_t changed[32];
    memset(changed, 4, sizeof changed);
    CHECK(rx_gen_mutate_object(store, first, 0, 2, changed) == RX_GEN_OK, "mutate");
    CHECK(promote_ok(&a, store, first, cap, NULL, NULL, NULL) == RX_GEN_ERR_STALE,
          "stale object generation was promoted");
    rx_gen_active(store, &active, &lineage);
    CHECK(lineage == 1, "stale object changed the generation");

    /* Missing proof. */
    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    draft.proofs_ok = 0;
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "unproven");
    CHECK(promote_ok(&a, store, first, cap, NULL, NULL, NULL) == RX_GEN_ERR_VERIFY,
          "missing proof was promoted");
    draft.proofs_ok = 1;

    /* Evidence changed after the freeze, and a missing object. */
    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "tamper candidate");
    rx_gen_set_disk_hook(store, flip_evidence, NULL);
    CHECK(promote_ok(&a, store, first, cap, NULL, NULL, NULL) == RX_GEN_ERR_TORN,
          "changed evidence was promoted");
    rx_gen_active(store, &active, &lineage);
    CHECK(lineage == 1, "tampered evidence changed the generation");
    char genesis[32];
    CHECK(evidence_of(dir, 1, genesis, sizeof genesis) > 0, "genesis evidence");
    CHECK(strcmp(genesis, "GENESIS") == 0, "genesis evidence changed");

    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "missing object");
    rx_gen_set_disk_hook(store, drop_objects, NULL);
    CHECK(promote_ok(&a, store, first, cap, NULL, NULL, NULL) == RX_GEN_ERR_MISSING,
          "missing object was promoted");

    /* Corrupt the committed root. Recovery must refuse rather than mix. */
    rx_gen_close(store);
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    CHECK(rx_gen_propose(store, SUBJ_PROPOSER, &draft, &first) == RX_GEN_OK, "corrupt candidate");
    rx_gen_set_disk_hook(store, NULL, NULL);
    CHECK(promote_ok(&a, store, first, cap, NULL, NULL, NULL) == RX_GEN_OK, "commit before corruption");
    rx_gen_close(store);
    char root_path[512];
    snprintf(root_path, sizeof root_path, "%s/g/%llu/root", dir, (unsigned long long)first);
    FILE *rf = fopen(root_path, "r+b");
    CHECK(rf != NULL, "root file");
    if (rf) {
        fseek(rf, 100, SEEK_SET);
        int byte = fgetc(rf);
        fseek(rf, 100, SEEK_SET);
        fputc(byte ^ 0xff, rf);
        fclose(rf);
    }
    CHECK(rx_gen_recover(dir, &rec) == RX_GEN_ERR_TORN, "corrupt checkpoint was accepted");
    CHECK(evidence_of(dir, 1, genesis, sizeof genesis) > 0, "parent evidence");
    CHECK(strcmp(genesis, "GENESIS") == 0, "parent was rewritten after corruption");

    /* Two processes. One holds the barrier; the other must wait outside it. */
    rm_tree(dir);
    CHECK(fresh_dir(dir, sizeof dir) == 0, "temp directory");
    CHECK(rx_gen_open(dir, &store) == RX_GEN_OK, "open");
    CHECK(rx_gen_hold_barrier(store) == RX_GEN_OK, "hold");
    char exe[512];
    ssize_t nread = readlink("/proc/self/exe", exe, sizeof exe - 1);
    CHECK(nread > 0, "exe");
    if (nread > 0) exe[nread] = 0;
    int busy = run_child(exe, dir, "--busy", 0);
    CHECK(busy == 20, "concurrent promotion returned %d", busy);
    CHECK(rx_gen_release_barrier(store) == RX_GEN_OK, "release");
    rx_gen_close(store);
    rm_tree(dir);

    rx_world_destroy(&g_world);
    rx_caproot_stop(&g_root, &g_admin);
    auth_stop(&a);
}

static void binary_digest(char out[65]) {
    strcpy(out, "unavailable");
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) return;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, n);
    fclose(f);
    uint8_t d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static void write_receipt(void) {
    char path[512];
    if (omega_evidence_path("R9/rx_generation_barrier_receipt.json", path, sizeof path) != 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    char commit[41];
    memset(commit, 0, sizeof commit);
    if (!omega_evidence_run_commit(commit)) memcpy(commit, "unknown", 8);
    const char *candidate = getenv("OMEGA_CANDIDATE_COMMIT");
    const char *aienos = getenv("AIENOS_COMMIT");
    int bound = candidate && candidate[0] && strcmp(candidate, commit) == 0 &&
                !omega_evidence_tree_dirty();
    char digest[65];
    binary_digest(digest);
    struct utsname u;
    memset(&u, 0, sizeof u);
    uname(&u);
    fprintf(f,
            "{\n"
            "  \"schema\": \"AIEN_RX_R9_GENERATION_BARRIER_V1\",\n"
            "  \"run_id\": \"%s\",\n"
            "  \"candidate_commit\": %s%s%s,\n"
            "  \"candidate_bound\": %s,\n"
            "  \"run_commit\": \"%s\",\n"
            "  \"tree_dirty\": %s,\n"
            "  \"aienos_commit\": %s%s%s,\n"
            "  \"checks\": %d,\n"
            "  \"failures\": %d,\n"
            "  \"test_binary_sha256\": \"%s\",\n"
            "  \"host\": {\"sysname\": \"%s\", \"release\": \"%s\", \"machine\": \"%s\"},\n"
            "  \"hardware_scope\": \"host CPU only; no graphics processor; Linux authority checker retained\",\n"
            "  \"gates\": {\n"
            "    \"R9_GENERATION_BARRIER\": \"PASS (host; one coherent generation after every injected stop)\",\n"
            "    \"not_claimed\": [\"R8\", \"R10\", \"R11\", \"R12 silicon\", \"R13\"]\n"
            "  },\n"
            "  \"differential\": [\n",
            omega_evidence_run_id(), candidate ? "\"" : "", candidate ? candidate : "null",
            candidate ? "\"" : "", bound ? "true" : "false", commit,
            omega_evidence_tree_dirty() ? "true" : "false", aienos ? "\"" : "",
            aienos ? aienos : "null", aienos ? "\"" : "", g_checks, g_fail, digest, u.sysname,
            u.release, u.machine);
    for (int i = 0; i < g_ndiff; i++) {
        fprintf(f, "    {\"name\": \"%s\", \"linux\": %d, \"native\": %d}%s\n", g_diff[i].name,
                g_diff[i].linux_rc, g_diff[i].native_rc, i + 1 < g_ndiff ? "," : "");
    }
    fprintf(f, "  ],\n  \"crashes\": [\n");
    for (int i = 0; i < g_ncrash; i++) {
        fprintf(f,
                "    {\"step\": %d, \"coherent\": %s, \"lineage\": %llu, \"evidence\": \"%s\", "
                "\"receipt\": %s}%s\n",
                g_crash[i].step, g_crash[i].coherent ? "true" : "false",
                (unsigned long long)g_crash[i].lineage, g_crash[i].evidence,
                g_crash[i].receipt_after ? "true" : "false", i + 1 < g_ncrash ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("receipt: %s\n", path);
    printf("candidate bound: %s\n", bound ? "yes" : "no");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 4 && strcmp(argv[1], "--crash") == 0) return child_crash(argv[2], atoi(argv[3]));
    if (argc >= 3 && strcmp(argv[1], "--busy") == 0) return child_busy(argv[2]);

    char exe[512];
    ssize_t nread = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (nread < 0) return 1;
    exe[nread] = 0;

    authority_differential();
    const int steps[] = {
        RX_CRASH_BEFORE_CANDIDATE_WRITE, RX_CRASH_DURING_CANDIDATE_WRITE,
        RX_CRASH_AFTER_CANDIDATE_WRITE,   RX_CRASH_BEFORE_ROOT_FLIP,
        RX_CRASH_DURING_ROOT_FLIP,        RX_CRASH_AFTER_ROOT_FLIP,
        RX_CRASH_BEFORE_RECEIPT,          RX_CRASH_AFTER_RECEIPT};
    const uint64_t want[] = {1, 1, 1, 1, 1, 2, 2, 2};
    const char *text[] = {"GENESIS", "GENESIS", "GENESIS", "GENESIS",
                          "GENESIS", "CANDIDATE", "CANDIDATE", "CANDIDATE"};
    const int receipt_already[] = {0, 0, 0, 0, 0, 0, 0, 1};
    for (int i = 0; i < 8; i++)
        check_crash(exe, steps[i], want[i], text[i], receipt_already[i]);
    negative_and_live();
    if (g_fail) {
        fprintf(stderr, "%d generation-barrier failures\n", g_fail);
        return 1;
    }
    write_receipt();
    printf("generation barrier kept a single coherent generation through every injected stop\n");
    return 0;
}
