/*
 * rx_cortex_journal.c -- the journal backend seam (native v1 P06 cut 1).
 *
 * Links the CX_NO_POSIX build of rx_cortex.c (no file, lock or libc
 * allocation symbols; see the nm check in the Makefile) against a memory
 * backend and proves the contract a backend is held to: one writer, every
 * record either wholly in the journal or not at all (memory never runs
 * ahead), replay and chain identical on reopen, a torn tail refused or
 * repaired only on request, a changed byte never repaired.
 */
#include "runtime/rx_cortex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(cond, ...) do {                                              \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_fail++;                                                      \
            fprintf(stderr, "  FAIL %s:%d ", __FILE__, __LINE__);          \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

/* The allocation hooks named by cx_config_freestanding.h. */
static long g_live;
void *cx_fs_alloc(size_t n) { g_live++; return malloc(n); }
void *cx_fs_realloc(void *p, size_t n) { if (!p) g_live++; return realloc(p, n); }
void cx_fs_free(void *p) { if (p) g_live--; free(p); }

/* A memory journal: one byte buffer, a writer flag, fault switches. */
typedef struct {
    uint8_t *b;
    size_t len, cap;
    int writer;          /* 1 while a writable handle is open */
    int fail_append;     /* next append fails after a partial copy (undone) */
    int fail_truncate;
    int opens, appends, truncates, syncs;
} MemJournal;

static int mj_open(void *ctx, const char *name, int ro, void **h) {
    MemJournal *j = ctx;
    (void)name;
    j->opens++;
    if (!ro) {
        if (j->writer) return CX_ERR_WRITER;
        j->writer = 1;
    }
    *h = ro ? (void *)&j->len : (void *)j;   /* distinct handles, same journal */
    return CX_OK;
}
static int mj_size(void *ctx, void *h, size_t *len) { (void)h; *len = ((MemJournal *)ctx)->len; return CX_OK; }
static int mj_read(void *ctx, void *h, size_t off, uint8_t *out, size_t n) {
    MemJournal *j = ctx;
    (void)h;
    if (off + n > j->len) return CX_ERR_IO;
    memcpy(out, j->b + off, n);
    return CX_OK;
}
static int mj_append(void *ctx, void *h, const uint8_t *b, size_t n, int sync) {
    MemJournal *j = ctx;
    if (h != (void *)j) return CX_ERR_IO;   /* read-only handle */
    j->appends++;
    if (j->len + n > j->cap) {
        size_t c = j->cap ? j->cap * 2 : 4096;
        while (c < j->len + n) c *= 2;
        uint8_t *q = realloc(j->b, c);
        if (!q) return CX_ERR_IO;
        j->b = q;
        j->cap = c;
    }
    if (j->fail_append) {
        /* A partial write happened; the backend undoes it before returning. */
        memcpy(j->b + j->len, b, n / 2);
        memset(j->b + j->len, 0xee, n / 2);
        j->fail_append = 0;
        return CX_ERR_IO;
    }
    memcpy(j->b + j->len, b, n);
    j->len += n;
    if (sync) j->syncs++;
    return CX_OK;
}
static int mj_truncate(void *ctx, void *h, size_t len) {
    MemJournal *j = ctx;
    if (h != (void *)j) return CX_ERR_IO;
    j->truncates++;
    if (j->fail_truncate) return CX_ERR_IO;
    if (len > j->len) return CX_ERR_IO;
    j->len = len;
    return CX_OK;
}
static void mj_close(void *ctx, void *h) {
    MemJournal *j = ctx;
    if (h == (void *)j) j->writer = 0;
}
static const CxJournalOps mem_ops = { mj_open, mj_size, mj_read, mj_append, mj_truncate, mj_close };

static CxHeader hdr(uint32_t cls, uint32_t kind, uint64_t subject, uint64_t t) {
    CxHeader h;
    memset(&h, 0, sizeof h);
    h.cls = cls;
    h.kind = kind;
    h.subject = subject;
    h.t = t;
    return h;
}

int main(void) {
    MemJournal j;
    memset(&j, 0, sizeof j);
    CxStore s, t;

    /* 1. create: a header is written durably; a read-only open of nothing is refused */
    CHECK(cx_open_with(&t, &mem_ops, &j, "m", 16, CX_OPEN_READONLY) == CX_ERR_FORMAT, "ro open of empty");
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 16, CX_OPEN_SYNC) == CX_OK, "create");
    CHECK(j.len == 32 && j.syncs == 1 && j.writer == 1, "header len=%zu syncs=%d", j.len, j.syncs);
    CHECK(cx_open_with(&t, &mem_ops, &j, "m", 16, 0) == CX_ERR_WRITER, "second writer admitted");
    CHECK(cx_open_with(&t, &mem_ops, &j, "m", 0, CX_OPEN_READONLY) == CX_OK && t.n == 0, "ro while written");
    cx_close(&t);
    CHECK(j.writer == 1, "ro close dropped the writer");

    /* 2. append: each record is one atomic unit, synced under CX_OPEN_SYNC */
    for (uint64_t k = 1; k <= 5; k++) {
        CxHeader h = hdr(CX_CLAIM, CX_K_CLAIM, k % 3, k);
        uint64_t pay[3] = { k, k + 100, k + 200 };
        CHECK(cx_append(&s, &h, pay, 3, NULL) == CX_OK, "append %llu", (unsigned long long)k);
    }
    CHECK(s.n == 5 && j.appends == 6 && j.syncs == 6, "appends=%d syncs=%d", j.appends, j.syncs);
    size_t len5 = j.len;
    uint8_t chain5[32];
    memcpy(chain5, s.chain, 32);

    /* 3. a failed append leaves memory and journal exactly as before */
    j.fail_append = 1;
    CxHeader h = hdr(CX_OBSERVATION, CX_K_EVIDENCE_REF, 1, 6);
    uint64_t pay[1] = { 42 };
    CHECK(cx_append(&s, &h, pay, 1, NULL) == CX_ERR_IO, "failed append not reported");
    CHECK(s.n == 5 && j.len == len5 && memcmp(s.chain, chain5, 32) == 0, "memory ran ahead of the journal");
    CHECK(cx_append(&s, &h, pay, 1, NULL) == CX_OK && s.n == 6, "append after failure");
    uint8_t chain6[32];
    memcpy(chain6, s.chain, 32);
    cx_close(&s);
    CHECK(j.writer == 0, "close kept the writer lock");

    /* 4. reopen replays every record; the chain is rebuilt, not read */
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 0, 0) == CX_OK, "reopen");
    CHECK(s.n == 6 && s.n_subjects == 16 && memcmp(s.chain, chain6, 32) == 0 &&
          cx_verify_chain(&s) == CX_OK, "replayed state differs");
    CxRecord r;
    CHECK(cx_recall_id(&s, 4, &r) == CX_OK && r.payload[2] == 204, "recalled record differs");
    CHECK(cx_open_with(&t, &mem_ops, &j, "m", 17, CX_OPEN_READONLY) == CX_ERR_FORMAT, "subject mismatch");
    cx_close(&s);

    /* 5. a torn tail is refused, repaired only on request, and only by the backend */
    size_t full = j.len;
    j.len = full - 5;
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 16, 0) == CX_ERR_TORN, "torn tail accepted");
    CHECK(j.writer == 0, "failed open kept the writer lock");
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 16, CX_OPEN_READONLY) == CX_ERR_TORN, "ro torn tail accepted");
    j.fail_truncate = 1;
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 16, CX_OPEN_REPAIR_TAIL) == CX_ERR_IO, "repair without truncate");
    j.fail_truncate = 0;
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 16, CX_OPEN_REPAIR_TAIL) == CX_OK && s.n == 5 &&
          j.truncates == 2 && j.len < full - 5, "repair tail");
    CHECK(memcmp(s.chain, chain5, 32) == 0, "repaired chain differs");
    cx_close(&s);

    /* 6. a complete record with a changed byte is never repaired */
    j.b[32 + 8 * 14] ^= 1;   /* first record, payload word 1 */
    CHECK(cx_open_with(&s, &mem_ops, &j, "m", 16, CX_OPEN_REPAIR_TAIL) == CX_ERR_DIGEST, "tampered journal opened");
    j.b[32 + 8 * 14] ^= 1;

    /* 7. memory-only stores never touch a backend; nothing leaks */
    int appends_before = j.appends;
    CHECK(cx_init(&s, 4) == CX_OK && s.jops == NULL, "memory-only init");
    h = hdr(CX_CLAIM, CX_K_CLAIM, 1, 1);
    CHECK(cx_append(&s, &h, pay, 1, NULL) == CX_OK && j.appends == appends_before,
          "memory-only append hit the backend");
    cx_close(&s);

    /* 8. a subject count too large to index is refused, never wrapped into a small allocation */
    const uint64_t wraps = (uint64_t)(SIZE_MAX / sizeof(CxIdList)) + 1;   /* * size wraps to one entry */
    CHECK(cx_init(&t, UINT64_MAX) == CX_ERR_NOMEM, "oversized subject count initialised");
    CHECK(cx_init(&t, wraps) == CX_ERR_NOMEM, "wrapping subject count initialised");
    uint8_t saved[8];
    memcpy(saved, j.b + 16, 8);
    const uint64_t hostile[2] = { UINT64_MAX, wraps };
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 8; i++) j.b[16 + i] = (uint8_t)(hostile[k] >> (8 * i));   /* header word 2 */
        CHECK(cx_open_with(&t, &mem_ops, &j, "m", 0, CX_OPEN_READONLY) == CX_ERR_FORMAT,
              "journal with subject count %llu opened", (unsigned long long)hostile[k]);
    }
    memcpy(j.b + 16, saved, 8);
    CHECK(g_live == 0, "live allocations after close: %ld", g_live);

    free(j.b);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "M20 CORTEX JOURNAL SEAM: FAIL\n" : "M20 CORTEX JOURNAL SEAM: PASS\n");
    return g_fail ? 1 : 0;
}
