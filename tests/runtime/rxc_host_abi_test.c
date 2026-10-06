/*
 * rxc_host_abi_test.c -- NEXT-PHASE-1 cut 1a: the host ABI facade over
 * COMPOSITION-2, through build/librx_compose.a only (no test hooks).
 *
 *   T1 open a fresh home, register one Skill that records its task handle,
 *      run one goal: committed, AEGIS pass, Cortex record ids present
 *   T2 close, REOPEN the same dir: same AienMachineId, recall returns the
 *      same record ids with the same digests (restart + recall); a second
 *      goal commits after the restart
 *   T3 another machine is refused on that home (identity)
 *   T4 containment: only machine.id, cortex.cx and jspace* on disk (staged
 *      J-Space branches leave no file)
 *   T5 torn tail (cortex.cx cut by a few bytes): strict open refuses
 *      explicitly; default open reports tail_torn and either repairs (every
 *      record before the torn one keeps id and digest, run-1 evidence intact)
 *      or refuses explicitly and stably (a second open refuses again)
 */
#include "runtime/rxc_host_abi.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static _Atomic uint64_t seen_task, calls;
#define PROPOSE(t) ((t) * 3u + 1u)

static int skill(void *ctx, uint64_t task, uint64_t *result) {
    (void)ctx;
    atomic_store(&seen_task, task);
    atomic_fetch_add(&calls, 1);
    *result = PROPOSE(task);
    return 0;
}
static int verify(void *ctx, uint64_t task, uint64_t result) {
    (void)ctx;
    return result == PROPOSE(task);
}

static const uint8_t ROOT[] = "np1-host-abi-test-machine";
static const uint8_t OTHER[] = "np1-some-other-machine";

static void hex8(const uint8_t *d, char *o) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { o[2 * i] = h[d[i] >> 4]; o[2 * i + 1] = h[d[i] & 15]; }
    o[16] = 0;
}

static RxcHost *open_home(const char *dir, uint32_t flags, RxcHostInfo *info, int *rc_out) {
    RxcHost *h = NULL;
    int rc = rxc_host_open(dir, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, 0x5E55ull, flags,
                           &h, info);
    if (rc_out) *rc_out = rc;
    if (rc != RXC_HOST_OK) return NULL;
    int k = rxc_host_register_skill(h, "np1.echo-proposal", NULL, 10, skill, NULL);
    CHECK(k == 0, "register_skill -> %d", k);
    CHECK(rxc_host_set_verify(h, verify, NULL) == RXC_HOST_OK, "set_verify");
    return h;
}

/* Cut the journal by 5 bytes and reopen. Returns 1 if the default open
 * repaired and ran on, 0 if it refused (explicitly, and again on a second
 * open), -1 on a check failure. Either way: the strict open refuses, the
 * default open reports tail_torn, and nothing is lost while reported fine. */
static uint32_t collect(RxcHost *h, RxcHostRecord *all, uint32_t max) {
    RxcHostInfo in;
    if (rxc_host_info(h, &in) != RXC_HOST_OK) return 0;
    uint32_t n = 0;
    for (uint64_t id = 1; id <= in.records && n < max; id++)
        if (rxc_host_record(h, id, &all[n]) == RXC_HOST_OK) n++;
    CHECK(n == in.records && n > 0, "every record readable (%u of %llu)", n,
          (unsigned long long)in.records);
    return n;
}

/* `all[0..nall)`: every record of the closed home, read just before close. */
static int torn_case(const char *home, const char *label, const RxcHostRecord *all, uint32_t nall,
                     const uint64_t *cited, const RxcHostRecord *cited_rec, uint64_t next_task) {
    int f0 = fails, rc;
    uint64_t n0 = nall;
    RxcHost *h;

    char cx[400];
    snprintf(cx, sizeof cx, "%s/cortex.cx", home);
    struct stat st;
    CHECK(stat(cx, &st) == 0 && st.st_size > 16, "%s: journal size", label);
    off_t cut = st.st_size - 5;
    CHECK(truncate(cx, cut) == 0, "%s: truncate by 5 bytes", label);

    RxcHostInfo it;
    RxcHost *s = NULL;
    int src = rxc_host_open(home, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, 0x5E55ull,
                            RXC_HOST_OPEN_REFUSE_TORN, &s, &it);
    CHECK(src == RXC_HOST_E_TORN && s == NULL && it.tail_torn == 1,
          "%s: strict open refuses the torn tail -> %d", label, src);
    CHECK(stat(cx, &st) == 0 && st.st_size == cut, "%s: strict refusal left the journal untouched",
          label);

    RxcHostInfo ir, ir2;
    h = open_home(home, 0, &ir, &rc);
    CHECK(h && rc == RXC_HOST_OK && ir.tail_torn == 1, "%s: default open reports tail_torn -> %d",
          label, rc);
    if (!h) return -1;
    int irc = rxc_host_info(h, &ir2);
    printf("%s: info rc %d open_rc %d records %llu (were %llu) recovered_completed %u "
           "rolled_back %u\n", label, irc, ir2.open_rc, (unsigned long long)ir2.records,
           (unsigned long long)n0, ir2.recovered_completed, ir2.rolled_back);
    int repaired = irc == RXC_HOST_OK;
    if (repaired) {
        /* Repaired: everything before the torn record is unchanged and the
         * torn record (the last one) is gone or re-written; any re-written
         * record is new history appended after the repair. */
        int prefix = 1;
        for (uint32_t i = 0; i + 1 < nall; i++) {
            RxcHostRecord x;
            if (rxc_host_record(h, all[i].id, &x) != RXC_HOST_OK ||
                memcmp(x.digest, all[i].digest, 32) != 0) { prefix = 0; break; }
        }
        CHECK(prefix, "%s: records 1..%u keep ids and digests after repair", label, nall - 1);
        CHECK(ir2.records >= n0 - 1, "%s: at most the torn record is missing", label);
        for (int i = 0; i < 4; i++) {
            RxcHostRecord x;
            CHECK(rxc_host_record(h, cited[i], &x) == RXC_HOST_OK &&
                  memcmp(x.digest, cited_rec[i].digest, 32) == 0,
                  "%s: cited record %llu intact", label, (unsigned long long)cited[i]);
        }
        RxcHostResult r3;
        rc = rxc_host_run(h, next_task, 5000000 + next_task % 1000, &r3);
        CHECK(rc == RXC_HOST_OK && r3.committed, "%s: run after repair -> %d outcome %d", label, rc,
              r3.outcome);
        rxc_host_close(h);
    } else {
        CHECK(ir2.open_rc != 0 && !ir2.opened, "%s: refusal names its cause", label);
        rxc_host_close(h);
        CHECK(stat(cx, &st) == 0, "%s: journal still present", label);
        printf("%s: refused explicitly (open_rc %d); journal now %lld bytes (cut to %lld)\n", label,
               ir2.open_rc, (long long)st.st_size, (long long)cut);
        /* Stable: a second open must not "succeed" now that the first one
         * may have truncated the torn record (CX_OPEN_REPAIR_TAIL). */
        RxcHostInfo iq, iq2;
        h = open_home(home, 0, &iq, &rc);
        CHECK(h && rc == RXC_HOST_OK, "%s: second open handle -> %d", label, rc);
        int qrc = h ? rxc_host_info(h, &iq2) : -99;
        CHECK(qrc == RXC_HOST_E_OPEN && iq2.open_rc != 0 && !iq2.opened,
              "%s: second open refuses again (rc %d open_rc %d)", label, qrc, iq2.open_rc);
        RxcHostResult rq;
        CHECK(h && rxc_host_run(h, next_task, 6000000, &rq) == RXC_HOST_E_OPEN,
              "%s: no run on a refused home", label);
        rxc_host_close(h);
    }
    return fails == f0 ? repaired : -1;
}

#define MAXR 64
int main(void) {
    char dir[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rxc_host_abi.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    char home[300];
    snprintf(home, sizeof home, "%s/home", dir);

    uint32_t lay[3];
    CHECK(rxc_host_abi_layout(lay) == RXC_HOST_ABI_VERSION && lay[0] == sizeof(RxcHostInfo) &&
          lay[1] == sizeof(RxcHostResult) && lay[2] == sizeof(RxcHostRecord), "abi layout");
    printf("abi layout: info %u result %u record %u\n", lay[0], lay[1], lay[2]);

    /* ---- T1 ---- */
    RxcHostInfo i1;
    int rc;
    RxcHost *h = open_home(home, 0, &i1, &rc);
    CHECK(h && rc == RXC_HOST_OK, "open fresh -> %d", rc);
    if (!h) return 1;
    CHECK(i1.abi_version == RXC_HOST_ABI_VERSION && !i1.machine_id_was_stored && !i1.tail_torn,
          "fresh info");
    uint64_t task1 = 0x4E50310000000001ull;
    RxcHostResult r1;
    rc = rxc_host_run(h, task1, 1000000, &r1);
    CHECK(rc == RXC_HOST_OK, "run 1 -> %d (run_rc %d)", rc, r1.run_rc);
    CHECK(r1.outcome == RXC_HOST_OUT_COMMITTED && r1.committed == 1, "run 1 outcome %d", r1.outcome);
    CHECK(atomic_load(&seen_task) == task1 && atomic_load(&calls) >= 1,
          "Skill saw task %llx", (unsigned long long)atomic_load(&seen_task));
    CHECK(r1.result == PROPOSE(task1), "committed result");
    CHECK(r1.n_branches == 1 && r1.winner == 0 && r1.winner_skill == 0, "branches %u winner %u",
          r1.n_branches, r1.winner);
    CHECK(r1.aegis_pass_mask & 1u, "AEGIS pass mask %x", r1.aegis_pass_mask);
    CHECK(r1.cx_goal && r1.cx_candidate[0] && r1.cx_evidence && r1.cx_promotion,
          "cortex ids goal %llu cand %llu ev %llu promo %llu", (unsigned long long)r1.cx_goal,
          (unsigned long long)r1.cx_candidate[0], (unsigned long long)r1.cx_evidence,
          (unsigned long long)r1.cx_promotion);
    uint64_t cited[4] = { r1.cx_goal, r1.cx_candidate[0], r1.cx_evidence, r1.cx_promotion };
    RxcHostRecord cited_rec[4];
    for (int i = 0; i < 4; i++)
        CHECK(rxc_host_record(h, cited[i], &cited_rec[i]) == RXC_HOST_OK && cited_rec[i].verified,
              "record %llu", (unsigned long long)cited[i]);
    static RxcHostRecord before[MAXR], after[MAXR];
    uint32_t nb = 0, na = 0;
    uint64_t tb = 0, ta = 0;
    CHECK(rxc_host_recall(h, RXC_HOST_SUBJECT_STATE, before, MAXR, &nb, &tb) == RXC_HOST_OK &&
          nb > 0 && nb == tb, "recall before: %u of %llu", nb, (unsigned long long)tb);
    RxcHostInfo i1b;
    CHECK(rxc_host_info(h, &i1b) == RXC_HOST_OK && i1b.records > 0, "journal has records");
    uint8_t mid[32];
    memcpy(mid, i1b.machine_id, 32);
    rxc_host_close(h);

    /* ---- T2 restart + recall ---- */
    RxcHostInfo i2;
    h = open_home(home, 0, &i2, &rc);
    CHECK(h && rc == RXC_HOST_OK, "reopen -> %d", rc);
    if (!h) return 1;
    CHECK(i2.machine_id_was_stored && memcmp(i2.machine_id, mid, 32) == 0 && !i2.tail_torn,
          "same AienMachineId after restart");
    CHECK(rxc_host_recall(h, RXC_HOST_SUBJECT_STATE, after, MAXR, &na, &ta) == RXC_HOST_OK,
          "recall after");
    /* Reopen appends (the World's objects come to life again at every open):
     * the pre-restart records must be an unchanged PREFIX of the recall. */
    CHECK(na >= nb, "recall count %u vs %u", na, nb);
    int same = na >= nb;
    for (uint32_t i = 0; same && i < nb; i++)
        same = after[i].id == before[i].id && memcmp(after[i].digest, before[i].digest, 32) == 0 &&
               after[i].verified;
    CHECK(same, "recall after restart returns the same ids and digests");
    for (uint32_t i = nb; i < na; i++)
        printf("appended at reopen: id %llu kind 0x%x tag %llu\n", (unsigned long long)after[i].id,
               after[i].kind, (unsigned long long)after[i].tag);
    for (int i = 0; i < 4; i++) {
        RxcHostRecord x;
        CHECK(rxc_host_record(h, cited[i], &x) == RXC_HOST_OK &&
              memcmp(x.digest, cited_rec[i].digest, 32) == 0, "cited record %llu after restart",
              (unsigned long long)cited[i]);
    }
    RxcHostInfo i2b;
    rxc_host_info(h, &i2b);
    CHECK(i2b.recovered_record != 0 && i2b.rolled_back == 0, "recovered state record %llu",
          (unsigned long long)i2b.recovered_record);
    RxcHostResult r2;
    uint64_t task2 = 0x4E50310000000002ull;
    rc = rxc_host_run(h, task2, 2000000, &r2);
    CHECK(rc == RXC_HOST_OK && r2.committed && r2.result == PROPOSE(task2),
          "run 2 after restart -> %d outcome %d", rc, r2.outcome);

    /* ---- T3 identity ---- */
    RxcHost *o = NULL;
    RxcHostInfo io;
    int orc = rxc_host_open(home, RXC_HOST_ROOT_PROVISIONED, OTHER, sizeof OTHER - 1, 1, 0, &o, &io);
    CHECK(orc == RXC_HOST_E_IDENTITY && o == NULL, "other machine refused -> %d", orc);
    rxc_host_close(h);

    /* ---- T4 containment ---- */
    DIR *d = opendir(home);
    int stray = 0, nfiles = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        nfiles++;
        if (strcmp(e->d_name, "machine.id") && strcmp(e->d_name, "cortex.cx") &&
            strncmp(e->d_name, "jspace", 6)) {
            stray++;
            printf("stray file: %s\n", e->d_name);
        }
    }
    if (d) closedir(d);
    CHECK(stray == 0 && nfiles >= 3, "home holds only machine.id, cortex.cx, jspace* (%d files)",
          nfiles);

    /* ---- T5 torn tail ----
     * (a) the torn record lies past the J-Space anchor (a reopen appends
     *     World records after the last commit): repaired, runs on;
     * (b) a run commits and its own tail is torn: repaired or refused
     *     explicitly; measured below, never silent. */
    static RxcHostRecord all[256];
    uint32_t nall;
    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T5a open -> %d", rc);
    if (!h) return 1;
    nall = collect(h, all, 256);
    rxc_host_close(h);
    int ta_r = torn_case(home, "T5a torn past anchor", all, nall, cited, cited_rec,
                         0x4E50310000000003ull);
    CHECK(ta_r == 1, "T5a expected repair, got %d", ta_r);

    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T5b open -> %d", rc);
    if (!h) return 1;
    RxcHostResult rb;
    CHECK(rxc_host_run(h, 0x4E50310000000005ull, 7000000, &rb) == RXC_HOST_OK && rb.committed,
          "T5b run");
    nall = collect(h, all, 256);
    rxc_host_close(h);
    int tb_r = torn_case(home, "T5b torn after a run", all, nall, cited, cited_rec,
                         0x4E50310000000006ull);
    CHECK(tb_r >= 0, "T5b repaired or refused explicitly (%d)", tb_r);
    printf("T5b outcome: %s\n", tb_r == 1 ? "repaired" : tb_r == 0 ? "refused" : "check failure");

    char m[17];
    hex8(mid, m);
    printf("machine %s... records-before-restart %u\n", m, nb);
    printf("rxc_host_abi_test: %d checks, %d failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (!fails && system(cmd) != 0) printf("note: could not remove %s\n", dir);
    return fails ? 1 : 0;
}
