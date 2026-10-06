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
 *   T5 torn tail (cut 2): the default open refuses a torn journal
 *      (RXC_HOST_E_TORN) or one cut behind its anchor (RXC_HOST_E_REPLAY),
 *      stably and without writing; rxc_host_recover repairs, records the cut
 *      (byte range, dropped count, cause) and re-anchors; the next open
 *      succeeds with the old records an unchanged prefix and a goal commits
 *   T6 recover is refused while a handle holds the home
 *   T7 host records (constraint, authorization, effect) through the
 *      composition writer, recalled with the same ids after restart
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

/* Byte offset where record `id` starts (journal: 32-byte header, then per
 * record 13 header words + payload + 4 digest words). */
static uint64_t record_start(const RxcHostRecord *all, uint32_t nall, uint64_t id) {
    uint64_t off = 32;
    for (uint32_t i = 0; i < nall && all[i].id < id; i++) off += (17u + all[i].n_payload) * 8u;
    return off;
}

static uint64_t word_digest_nonzero(const uint64_t *w) { return w[0] | w[1] | w[2] | w[3]; }

/* `all[0..nall)`: every record of the closed home, read just before close.
 * Cut the journal to `cut` bytes (torn: inside the last record; boundary:
 * at a record start), then: the default open refuses with a named reason
 * and leaves the journal untouched, stably; another machine may not
 * repair; rxc_host_recover repairs and records the cut; a second recover
 * is a no-op; the next open succeeds with records 1..kept an unchanged
 * prefix, the repair record readable, and a goal commits. 0 = all held. */
static int torn_case(const char *home, const char *label, const RxcHostRecord *all, uint32_t nall,
                     uint64_t cut, int torn, uint64_t next_task, RxcHostRepair *rep_out) {
    int f0 = fails, rc;
    char cx[400];
    snprintf(cx, sizeof cx, "%s/cortex.cx", home);
    struct stat st;
    CHECK(stat(cx, &st) == 0 && (uint64_t)st.st_size > cut, "%s: journal size", label);
    const uint64_t size0 = (uint64_t)st.st_size;
    CHECK(truncate(cx, (off_t)cut) == 0, "%s: cut %llu -> %llu bytes", label,
          (unsigned long long)size0, (unsigned long long)cut);

    for (int pass = 0; pass < 2; pass++) {   /* the refusal is stable */
        RxcHostInfo it, it2;
        RxcHost *s = NULL;
        int orc = rxc_host_open(home, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, 0x5E55ull,
                                0, &s, &it);
        if (torn) {
            CHECK(orc == RXC_HOST_E_TORN && s == NULL && it.tail_torn == 1,
                  "%s: open %d refuses the torn tail -> %d", label, pass, orc);
        } else {
            CHECK(orc == RXC_HOST_OK && s && !it.tail_torn, "%s: open %d handle -> %d", label, pass,
                  orc);
            CHECK(s && rxc_host_register_skill(s, "np1.echo-proposal", NULL, 10, skill, NULL) == 0,
                  "%s: register on the refused home", label);
            int irc = s ? rxc_host_info(s, &it2) : -99;
            CHECK(irc == RXC_HOST_E_REPLAY && it2.open_rc == -21 && !it2.opened,
                  "%s: open %d refuses a journal behind its anchor (rc %d open_rc %d)", label, pass,
                  irc, it2.open_rc);
            RxcHostResult rq;
            int qrc = s ? rxc_host_run(s, next_task, 6000000, &rq) : -99;
            CHECK(qrc == RXC_HOST_E_REPLAY, "%s: no run on a refused home -> %d", label, qrc);
            rxc_host_close(s);
        }
        CHECK(stat(cx, &st) == 0 && (uint64_t)st.st_size == cut,
              "%s: refusal %d left the journal untouched (%lld bytes)", label, pass,
              (long long)st.st_size);
    }

    RxcHostRepair rep, rep2;
    CHECK(rxc_host_recover(home, RXC_HOST_ROOT_PROVISIONED, OTHER, sizeof OTHER - 1, &rep) ==
              RXC_HOST_E_IDENTITY && !rep.repaired, "%s: another machine may not repair", label);
    CHECK(stat(cx, &st) == 0 && (uint64_t)st.st_size == cut, "%s: identity refusal wrote nothing",
          label);
    rc = rxc_host_recover(home, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, &rep);
    printf("%s: recover rc %d repaired %u torn %u cause %d cut [%llu,%llu) partial %llu anchor %llu "
           "kept %llu dropped %llu event %llu cut_bytes_kept %llu rolled_back %u recovered_completed %u\n", label, rc, rep.repaired,
           rep.tail_torn, rep.cause, (unsigned long long)rep.cut_lo, (unsigned long long)rep.cut_hi,
           (unsigned long long)rep.partial_records, (unsigned long long)rep.anchor_records,
           (unsigned long long)rep.records_kept, (unsigned long long)rep.dropped_records,
           (unsigned long long)rep.event_id, (unsigned long long)rep.cut_bytes_kept, rep.rolled_back,
           rep.recovered_completed);
    CHECK(rc == RXC_HOST_OK && rep.repaired == 1 && rep.event_id == rep.records_kept + 1,
          "%s: recover repaired -> %d", label, rc);
    CHECK(rep.opens == 1 && rep.open_rc == 0, "%s: recover's trial open succeeded", label);
    CHECK(rep.tail_torn == (uint32_t)torn && rep.cause == (torn ? -8 : -21), "%s: cause named",
          label);
    CHECK(rep.cut_hi == cut && rep.cut_lo <= rep.cut_hi && rep.cut_bytes_kept == rep.cut_hi - rep.cut_lo,
          "%s: byte range recorded", label);
    CHECK(rep.dropped_records >= 1 && rep.records_kept < nall, "%s: dropped %llu records", label,
          (unsigned long long)rep.dropped_records);
    CHECK(rep.records_kept + rep.dropped_records == nall || rep.anchor_records < nall,
          "%s: kept + dropped = records before the cut (%llu + %llu vs %u)", label,
          (unsigned long long)rep.records_kept, (unsigned long long)rep.dropped_records, nall);
    if (torn) CHECK(rep.cut_lo == record_start(all, nall, rep.records_kept + 1),
                    "%s: cut starts at record %llu", label, (unsigned long long)rep.records_kept + 1);
    CHECK(rxc_host_recover(home, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, &rep2) ==
              RXC_HOST_OK && rep2.repaired == 0, "%s: a second recover is a no-op", label);

    RxcHostInfo ir, ir2;
    RxcHost *h = open_home(home, 0, &ir, &rc);
    CHECK(h && rc == RXC_HOST_OK && !ir.tail_torn, "%s: open after recover -> %d", label, rc);
    if (!h) return -1;
    int irc = rxc_host_info(h, &ir2);
    printf("%s: after recover info rc %d records %llu rolled_back %u recovered_completed %u\n",
           label, irc, (unsigned long long)ir2.records, ir2.rolled_back, ir2.recovered_completed);
    CHECK(irc == RXC_HOST_OK && ir2.opened, "%s: the composition opens after recover", label);
    int prefix = 1;
    for (uint32_t i = 0; i < rep.records_kept && i < nall; i++) {
        RxcHostRecord x;
        if (rxc_host_record(h, all[i].id, &x) != RXC_HOST_OK || x.id != all[i].id ||
            memcmp(x.digest, all[i].digest, 32) != 0) { prefix = 0; break; }
    }
    CHECK(prefix, "%s: records 1..%llu are an unchanged prefix", label,
          (unsigned long long)rep.records_kept);
    RxcHostRecord ev;
    uint64_t w[24];
    CHECK(rxc_host_record(h, rep.event_id, &ev) == RXC_HOST_OK &&
              ev.subject == RXC_HOST_SUBJECT_HOST && ev.tag == RXC_HOST_TAG_REPAIR_TAIL &&
              ev.links[0] == rep.records_kept, "%s: repair record %llu in the journal", label,
          (unsigned long long)rep.event_id);
    int pn = rxc_host_payload(h, rep.event_id, w, 24);
    CHECK(pn >= (int)RXC_HOST_RP_BYTES && w[RXC_HOST_RP_CUT_LO] == rep.cut_lo &&
              w[RXC_HOST_RP_CUT_HI] == rep.cut_hi && w[RXC_HOST_RP_DROPPED] == rep.dropped_records &&
              (int64_t)w[RXC_HOST_RP_CAUSE] == rep.cause &&
              (!rep.cut_bytes_kept || word_digest_nonzero(w + RXC_HOST_RP_CUT_SHA)),
          "%s: repair payload names range, count, cause", label);
    RxcHostResult r3;
    rc = rxc_host_run(h, next_task, 5000000 + next_task % 1000, &r3);
    CHECK(rc == RXC_HOST_OK && r3.committed, "%s: run after recover -> %d outcome %d", label, rc,
          r3.outcome);
    rxc_host_close(h);
    if (rep_out) *rep_out = rep;
    return fails == f0 ? 0 : -1;
}

#define MAXR 64
int main(void) {
    char dir[256];
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/rxc_host_abi.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    char home[300];
    snprintf(home, sizeof home, "%s/home", dir);

    uint32_t lay[4];
    CHECK(rxc_host_abi_layout(lay) == RXC_HOST_ABI_VERSION && lay[0] == sizeof(RxcHostInfo) &&
          lay[1] == sizeof(RxcHostResult) && lay[2] == sizeof(RxcHostRecord) &&
          lay[3] == sizeof(RxcHostRepair), "abi layout");
    printf("abi layout: info %u result %u record %u repair %u\n", lay[0], lay[1], lay[2], lay[3]);

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

    /* ---- T5 torn tail, strict by default, repaired on request ----
     * (a) a goal commits and its own tail is torn (the coordinator's case);
     * (b) the torn record lies past the J-Space anchor (written at reopen);
     * (c) the journal is cut exactly at a record boundary inside the
     *     anchored prefix (cx_open sees a shorter valid journal). */
    static RxcHostRecord all[256];
    uint32_t nall;
    RxcHostRepair rp;
    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T5a open -> %d", rc);
    if (!h) return 1;
    RxcHostResult rb;
    CHECK(rxc_host_run(h, 0x4E50310000000005ull, 7000000, &rb) == RXC_HOST_OK && rb.committed,
          "T5a run");
    nall = collect(h, all, 256);
    rxc_host_close(h);
    {
        struct stat s5;
        char cx5[400];
        snprintf(cx5, sizeof cx5, "%s/cortex.cx", home);
        CHECK(stat(cx5, &s5) == 0, "T5a stat");
        CHECK(torn_case(home, "T5a torn after a run", all, nall, (uint64_t)s5.st_size - 5, 1,
                        0x4E50310000000006ull, &rp) == 0, "T5a held");
    }

    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T5b open -> %d", rc);
    if (!h) return 1;
    nall = collect(h, all, 256);
    rxc_host_close(h);
    h = open_home(home, 0, NULL, &rc);   /* this reopen appends past the anchor */
    if (!h) return 1;
    nall = collect(h, all, 256);
    rxc_host_close(h);
    {
        struct stat s5;
        char cx5[400];
        snprintf(cx5, sizeof cx5, "%s/cortex.cx", home);
        CHECK(stat(cx5, &s5) == 0, "T5b stat");
        CHECK(torn_case(home, "T5b torn past anchor", all, nall, (uint64_t)s5.st_size - 5, 1,
                        0x4E50310000000007ull, &rp) == 0, "T5b held");
    }

    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T5c open -> %d", rc);
    if (!h) return 1;
    CHECK(rxc_host_run(h, 0x4E50310000000008ull, 8000000, &rb) == RXC_HOST_OK && rb.committed,
          "T5c run");
    nall = collect(h, all, 256);
    rxc_host_close(h);
    CHECK(torn_case(home, "T5c cut at a record boundary", all, nall,
                    record_start(all, nall, all[nall - 1].id), 0, 0x4E50310000000009ull, &rp) == 0,
          "T5c held");

    /* (d) a whole run's records cut at a boundary (fresh home): everything
     *     the last goal wrote, its state commit included, is gone while
     *     J-Space holds NEW. MEASURED limit: recover records the cut and
     *     reports that the composition still refuses (it neither adopts a
     *     state no record names nor overwrites committed J-Space history);
     *     the refusal is named and stable and the kept prefix is unchanged. */
    {
        char home2[320];
        snprintf(home2, sizeof home2, "%s/home2", dir);
        h = open_home(home2, 0, NULL, &rc);
        CHECK(h != NULL, "T5d open -> %d", rc);
        if (!h) return 1;
        CHECK(rxc_host_run(h, 0x4E5031000000000Bull, 8500000, &rb) == RXC_HOST_OK && rb.committed,
              "T5d run 1");
        uint32_t npre = collect(h, all, 256);
        CHECK(rxc_host_run(h, 0x4E5031000000000Cull, 8600000, &rb) == RXC_HOST_OK && rb.committed,
              "T5d run 2");
        nall = collect(h, all, 256);
        rxc_host_close(h);
        uint64_t cut = record_start(all, nall, npre + 1);
        char cx[400];
        snprintf(cx, sizeof cx, "%s/cortex.cx", home2);
        static uint8_t keep[1 << 16], now[1 << 16];
        FILE *f = fopen(cx, "rb");
        size_t got = f && cut <= sizeof keep ? fread(keep, 1, cut, f) : 0;
        if (f) fclose(f);
        CHECK(got == cut && truncate(cx, (off_t)cut) == 0, "T5d cut run 2 (records %u..%u)", npre + 1,
              nall);
        rc = rxc_host_recover(home2, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, &rp);
        printf("T5d whole run cut: recover rc %d repaired %u cause %d anchor %llu kept %llu dropped %llu "
               "event %llu opens %u open_rc %d\n", rc, rp.repaired, rp.cause,
               (unsigned long long)rp.anchor_records, (unsigned long long)rp.records_kept,
               (unsigned long long)rp.dropped_records, (unsigned long long)rp.event_id, rp.opens,
               rp.open_rc);
        CHECK(rc == RXC_HOST_E_REPLAY && rp.repaired == 1 && !rp.opens && rp.open_rc == -21 &&
              rp.event_id == npre + 1 && rp.dropped_records == nall - npre,
              "T5d recover records the cut and names the refusal");
        RxcHostInfo i5d;
        h = open_home(home2, 0, NULL, &rc);
        int qrc = h ? rxc_host_info(h, &i5d) : -99;
        CHECK(qrc == RXC_HOST_E_REPLAY, "T5d the refusal is stable and named -> %d", qrc);
        rxc_host_close(h);
        f = fopen(cx, "rb");
        got = f ? fread(now, 1, cut, f) : 0;
        if (f) fclose(f);
        CHECK(got == cut && memcmp(keep, now, cut) == 0, "T5d records 1..%u unchanged on disk", npre);
    }

    /* ---- T6 recover needs the journal: a live handle blocks it ---- */
    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T6 open -> %d", rc);
    if (!h) return 1;
    RxcHostInfo i6;
    CHECK(rxc_host_info(h, &i6) == RXC_HOST_OK, "T6 info");
    CHECK(rxc_host_recover(home, RXC_HOST_ROOT_PROVISIONED, ROOT, sizeof ROOT - 1, &rp) ==
              RXC_HOST_E_OPEN && !rp.repaired, "T6 recover refused while a handle holds the home");

    /* ---- T7 host records through the composition writer, recalled after restart ---- */
    RxcHostResult r7;
    CHECK(rxc_host_run(h, 0x4E5031000000000Aull, 9000000, &r7) == RXC_HOST_OK && r7.committed,
          "T7 run");
    static const char C7[] = "constraint: change only files under the workspace";
    static const char A7[] = "authorize: write notes.txt (expected approval 1 of 1)";
    static const char E7[] = "effect: wrote notes.txt sha256=...";
    uint64_t c7 = 0, a7 = 0, e7 = 0;
    uint64_t lk[4] = { r7.cx_promotion, r7.cx_evidence, 0, 0 };
    CHECK(rxc_host_note(h, RXC_HOST_NOTE_CONSTRAINT, NULL, (const uint8_t *)C7, sizeof C7 - 1, &c7) ==
              RXC_HOST_OK && c7, "T7 constraint note %llu", (unsigned long long)c7);
    lk[2] = c7;
    CHECK(rxc_host_note(h, RXC_HOST_NOTE_AUTHORIZATION, lk, (const uint8_t *)A7, sizeof A7 - 1, &a7) ==
              RXC_HOST_OK && a7 > c7, "T7 authorization note %llu", (unsigned long long)a7);
    lk[3] = a7;
    CHECK(rxc_host_note(h, RXC_HOST_NOTE_EFFECT, lk, (const uint8_t *)E7, sizeof E7 - 1, &e7) ==
              RXC_HOST_OK && e7 > a7, "T7 effect note %llu", (unsigned long long)e7);
    uint64_t bad[4] = { 1u << 30, 0, 0, 0 };
    uint64_t x7 = 0;
    CHECK(rxc_host_note(h, RXC_HOST_NOTE_EFFECT, bad, (const uint8_t *)E7, 3, &x7) ==
              RXC_HOST_E_NOT_FOUND && x7 == 0, "T7 a link to a missing record is refused");
    CHECK(rxc_host_note(h, 99, NULL, (const uint8_t *)E7, 3, &x7) == RXC_HOST_E_ARG,
          "T7 unknown note kind refused");
    static RxcHostRecord hb[MAXR], ha[MAXR];
    uint32_t nhb = 0, nha = 0;
    uint64_t thb = 0, tha = 0;
    CHECK(rxc_host_recall(h, RXC_HOST_SUBJECT_HOST, hb, MAXR, &nhb, &thb) == RXC_HOST_OK,
          "T7 recall host subject");
    RxcHostInfo i7;
    rxc_host_info(h, &i7);
    rxc_host_close(h);
    h = open_home(home, 0, NULL, &rc);
    CHECK(h != NULL, "T7 reopen -> %d", rc);
    if (!h) return 1;
    RxcHostInfo i7b;
    CHECK(rxc_host_info(h, &i7b) == RXC_HOST_OK && memcmp(i7b.machine_id, mid, 32) == 0,
          "T7 same AienMachineId after restart");
    CHECK(rxc_host_recall(h, RXC_HOST_SUBJECT_HOST, ha, MAXR, &nha, &tha) == RXC_HOST_OK &&
              nha == nhb, "T7 recall after restart: %u host records (were %u)", nha, nhb);
    int same7 = nha == nhb;
    for (uint32_t i = 0; same7 && i < nha; i++)
        same7 = ha[i].id == hb[i].id && !memcmp(ha[i].digest, hb[i].digest, 32) && ha[i].verified;
    CHECK(same7, "T7 host records keep ids and digests across restart");
    uint64_t pw[RXC_HOST_NP_BYTES + 16];
    int pn = rxc_host_payload(h, c7, pw, RXC_HOST_NP_BYTES + 16);
    char txt[129] = { 0 };
    if (pn > (int)RXC_HOST_NP_BYTES && pw[RXC_HOST_NP_LEN] < sizeof txt)
        for (uint64_t i = 0; i < pw[RXC_HOST_NP_LEN]; i++)
            txt[i] = (char)(pw[RXC_HOST_NP_BYTES + i / 8] >> (8 * (i % 8)));
    CHECK(strcmp(txt, C7) == 0, "T7 constraint text round-trips: '%s'", txt);
    RxcHostRecord ra;
    CHECK(rxc_host_record(h, a7, &ra) == RXC_HOST_OK && ra.tag == RXC_HOST_NOTE_AUTHORIZATION &&
              ra.links[0] == r7.cx_promotion && ra.links[2] == c7, "T7 authorization cites its ids");
    printf("T7 host records: constraint %llu authorization %llu effect %llu; host subject %u\n",
           (unsigned long long)c7, (unsigned long long)a7, (unsigned long long)e7, nha);
    rxc_host_close(h);

    char m[17];
    hex8(mid, m);
    printf("machine %s... records-before-restart %u\n", m, nb);
    printf("rxc_host_abi_test: %d checks, %d failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (!fails && system(cmd) != 0) printf("note: could not remove %s\n", dir);
    return fails ? 1 : 0;
}
