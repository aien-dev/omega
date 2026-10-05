/*
 * M20 OMEGA_TENSOR crash-safe storage lifetime tests
 * (src/tensor/omega_tensor_store.h, docs/tensor/M20_OMEGA_TENSOR.md row
 * "Crash-safe storage lifetime"). Host only, no device.
 *
 *   round trip      put/get/release across close and reopen, views, F16,
 *                   rank 0, NaN canonical form, dedup, orphan sweep, busy
 *                   lock, lost journal refused, generation monotonicity
 *   crash           fork a child that crashes (_exit(86) via the test-only
 *                   hook) at every commit phase of a put and of a release;
 *                   the parent recovers and must see exactly the old state
 *                   (before the journal rename) or the new state (after it)
 *   torn writes     the journal truncated at every byte and with every byte
 *                   flipped, the payload truncated at every byte, extended,
 *                   and flipped: open must refuse with a typed error; a
 *                   payload corrupted after open must be refused by get
 *   crafted journal an independent encoder of the documented format: the
 *                   implementation's bytes match it, and records that do not
 *                   follow from the state before them are refused
 * Every refusal returns no data. Expected values come from the test's own
 * tensors, never from the store.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef OMEGA_TENSOR_TEST_HOOKS
#error "test_omega_tensor_store.c needs -DOMEGA_TENSOR_TEST_HOOKS (set by mk/tensor.mk test build)"
#endif
#include "omega_numeric.h"
#include "omega_tensor.h"
#include "omega_tensor_store.h"
#include "sha256.h"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static OmegaTensorCtx *g;
static char g_root[256];
static int g_ndir;

/* ---- files ------------------------------------------------------------- */

static void new_dir(char *out, size_t n) { snprintf(out, n, "%s/s%d", g_root, g_ndir++); }

static void rm_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static int read_file(const char *path, uint8_t **buf, size_t *n) {
    struct stat sb;
    *buf = NULL;
    *n = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &sb) != 0) { close(fd); return -1; }
    *buf = malloc((size_t)sb.st_size + 1);
    size_t got = 0;
    while (*buf && got < (size_t)sb.st_size) {
        ssize_t r = read(fd, *buf + got, (size_t)sb.st_size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    *n = got;
    return (*buf && got == (size_t)sb.st_size) ? 0 : -1;
}

static int write_file(const char *path, const uint8_t *buf, size_t n) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static bool exists(const char *dir, const char *name) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static void payload_name(const uint8_t id[32], char out[69]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", id[i]);
    snprintf(out + 64, 5, ".bin");
}

/* ---- tensors ----------------------------------------------------------- */

static OmegaTensor mk_bits(uint32_t rank, const uint64_t *shape, const uint32_t *bits) {
    uint64_t n = 1;
    for (uint32_t d = 0; d < rank; d++) n *= shape[d];
    float *f = malloc(n * sizeof(float));
    memcpy(f, bits, n * sizeof(float));
    OmegaTensor t = {0, 0};
    int rc = omega_tensor_from_f32(g, rank, shape, f, &t);
    CHECK(rc == OMEGA_TENSOR_OK, "setup: from_f32 rc %d", rc);
    free(f);
    return t;
}

static OmegaTensor mk_vals(uint32_t rank, const uint64_t *shape, uint32_t seed) {
    uint32_t bits[64];
    uint64_t n = 1;
    for (uint32_t d = 0; d < rank; d++) n *= shape[d];
    for (uint64_t i = 0; i < n && i < 64; i++) bits[i] = 0x3f800000U + seed * 0x10000U + (uint32_t)i * 0x100U;
    return mk_bits(rank, shape, bits);
}

/* Loads h and compares with expect (value equality and exact bytes). */
static bool got_equals(OmegaTensorStore *st, OmegaStorageHandle h, OmegaTensor expect) {
    OmegaTensor t;
    if (omega_tensor_store_get(st, h, g, &t) != OMEGA_TENSOR_OK) return false;
    bool eq = false;
    int rc = omega_tensor_value_equal(g, t, expect, &eq);
    omega_tensor_release(g, t);
    return rc == OMEGA_TENSOR_OK && eq;
}

static void release_t(OmegaTensor t) { omega_tensor_release(g, t); }

/* ---- round trip -------------------------------------------------------- */

static void test_round_trip(void) {
    char dir[512];
    new_dir(dir, sizeof dir);
    OmegaTensorStore *st = NULL, *st2 = NULL;
    CHECK(omega_tensor_store_open(NULL, 4, &st) == OMEGA_TENSOR_ERR_BAD_ARGS, "open NULL dir refused");
    CHECK(omega_tensor_store_open(dir, 0, &st) == OMEGA_TENSOR_ERR_BAD_ARGS, "open capacity 0 refused");
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK, "open fresh store");
    if (!st) return;
    CHECK(omega_tensor_store_open(dir, 8, &st2) == OMEGA_TSTORE_ERR_BUSY && !st2, "second opener refused (busy)");

    uint64_t s23[2] = {2, 3}, s5[1] = {5};
    uint32_t abits[6] = {0x3f800000U, 0x80000000U, 0x7fa00001U, 0xff800000U, 0x00000001U, 0x40490fdbU};
    OmegaTensor a = mk_bits(2, s23, abits);
    uint16_t hbits[5] = {0x3c00U, 0x8000U, 0x7d01U, 0xfc00U, 0x0001U};
    OmegaTensor b;
    CHECK(omega_tensor_from_data(g, OMEGA_DT_F16, 1, s5, hbits, &b) == OMEGA_TENSOR_OK, "F16 tensor");
    uint32_t cbits[1] = {0xc0000000U};
    OmegaTensor c = mk_bits(0, NULL, cbits);
    OmegaTensor at;
    CHECK(omega_tensor_transpose(g, a, &at) == OMEGA_TENSOR_OK, "transpose view");

    OmegaStorageHandle ha, hb, hc, hat;
    CHECK(omega_tensor_store_put(st, g, a, &ha) == OMEGA_TENSOR_OK, "put F32 [2,3]");
    CHECK(omega_tensor_store_put(st, g, b, &hb) == OMEGA_TENSOR_OK, "put F16 [5]");
    CHECK(omega_tensor_store_put(st, g, c, &hc) == OMEGA_TENSOR_OK, "put rank 0");
    CHECK(omega_tensor_store_put(st, g, at, &hat) == OMEGA_TENSOR_OK, "put transposed view");
    CHECK(ha.slot == 0 && ha.generation == 1 && hat.slot == 3 && hat.generation == 1,
          "first handles are slot 0..3 at generation 1");
    CHECK(got_equals(st, ha, a) && got_equals(st, hb, b) && got_equals(st, hc, c) && got_equals(st, hat, at),
          "every stored value reads back equal");

    /* NaN payload is not semantic: stored as the canonical quiet NaN. */
    OmegaTensor ga;
    float out[6];
    if (omega_tensor_store_get(st, ha, g, &ga) == OMEGA_TENSOR_OK) {
        uint32_t ob[6];
        omega_tensor_read_f32(g, ga, out, 6);
        memcpy(ob, out, sizeof ob);
        CHECK(ob[2] == 0x7fc00000U && ob[1] == 0x80000000U && ob[4] == 0x00000001U,
              "NaN canonical, -0 and subnormal kept (got %08x %08x %08x)", ob[2], ob[1], ob[4]);
        release_t(ga);
    } else CHECK(0, "get a");
    uint8_t vid[32], svid[32];
    omega_tensor_value_id(g, a, vid);
    CHECK(omega_tensor_store_value_id(st, ha, svid) == OMEGA_TENSOR_OK && !memcmp(vid, svid, 32),
          "store value id equals the tensor value id");
    char pname[69];
    payload_name(vid, pname);
    CHECK(exists(dir, pname), "payload file is named by the value id");

    omega_tensor_store_close(st);
    st = NULL;
    CHECK(omega_tensor_store_open(dir, 9, &st) == OMEGA_TENSOR_ERR_BAD_ARGS && !st, "reopen with other capacity refused");
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK, "reopen");
    if (!st) return;
    CHECK(omega_tensor_store_live_count(st) == 4, "4 live after reopen");
    CHECK(got_equals(st, ha, a) && got_equals(st, hb, b) && got_equals(st, hc, c) && got_equals(st, hat, at),
          "every value reads back equal after reopen");

    /* release: stale now and after recovery; generation never returns */
    CHECK(omega_tensor_store_release(st, ha) == OMEGA_TENSOR_OK, "release a");
    OmegaTensor tmp;
    CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE, "get after release: stale");
    CHECK(omega_tensor_store_release(st, ha) == OMEGA_TENSOR_ERR_STALE, "double release: stale");
    CHECK(!omega_tensor_store_valid(st, ha), "released handle invalid");
    CHECK(!exists(dir, pname), "unshared payload removed on release");
    OmegaStorageHandle wrong = hb;
    wrong.generation++;
    CHECK(omega_tensor_store_get(st, wrong, g, &tmp) == OMEGA_TENSOR_ERR_STALE, "wrong generation: stale");
    omega_tensor_store_close(st);
    st = NULL;
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK, "reopen after release");
    if (!st) return;
    CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE, "released handle stale after recovery");
    OmegaTensor d = mk_vals(1, s5, 7);
    OmegaStorageHandle hd;
    CHECK(omega_tensor_store_put(st, g, d, &hd) == OMEGA_TENSOR_OK, "put d");
    CHECK(hd.slot == ha.slot && hd.generation == ha.generation + 1,
          "slot reuse gets the next generation (slot %u gen %llu)", hd.slot, (unsigned long long)hd.generation);
    CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE, "old handle stale after slot reuse");
    CHECK(got_equals(st, hd, d), "d reads back");

    /* dedup: two slots, one payload; releasing one keeps the other */
    OmegaStorageHandle hd2;
    CHECK(omega_tensor_store_put(st, g, d, &hd2) == OMEGA_TENSOR_OK && hd2.slot != hd.slot, "put d twice");
    CHECK(omega_tensor_store_release(st, hd) == OMEGA_TENSOR_OK, "release first copy");
    CHECK(got_equals(st, hd2, d), "shared payload kept for the other copy");

    /* capacity */
    OmegaStorageHandle hx;
    int n = 0;
    while (omega_tensor_store_put(st, g, d, &hx) == OMEGA_TENSOR_OK && n < 20) n++;
    CHECK(n > 0 && omega_tensor_store_live_count(st) == 8,
          "store fills to capacity then refuses");
    CHECK(omega_tensor_store_put(st, g, d, &hx) == OMEGA_TENSOR_ERR_CAPACITY, "full store: capacity");
    omega_tensor_store_close(st);
    st = NULL;

    /* orphan sweep and leftover temp files */
    char orphan[69];
    uint8_t zid[32];
    memset(zid, 0xab, 32);
    payload_name(zid, orphan);
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, orphan);
    write_file(p, (const uint8_t *)"x", 1);
    snprintf(p, sizeof p, "%s/payload.tmp", dir);
    write_file(p, (const uint8_t *)"junk", 4);
    snprintf(p, sizeof p, "%s/journal.tmp", dir);
    write_file(p, (const uint8_t *)"junk", 4);
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK, "open with leftovers");
    CHECK(!exists(dir, orphan) && !exists(dir, "payload.tmp") && !exists(dir, "journal.tmp"),
          "orphan payload and temp files removed");
    CHECK(got_equals(st, hd2, d) && got_equals(st, hb, b), "values intact after sweep");
    omega_tensor_store_close(st);
    st = NULL;

    /* lost journal: refused, never a silent fresh store */
    char jp[1024], jp2[1024];
    snprintf(jp, sizeof jp, "%s/journal", dir);
    snprintf(jp2, sizeof jp2, "%s.moved", dir);
    CHECK(rename(jp, jp2) == 0, "move journal away");
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TSTORE_ERR_TORN && !st, "missing journal with payloads refused");
    CHECK(rename(jp2, jp) == 0, "restore journal");
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK, "open after restore");
    CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE && got_equals(st, hd2, d),
          "state intact after restore");
    omega_tensor_store_close(st);
    release_t(at);
    release_t(a);
    release_t(b);
    release_t(c);
    release_t(d);
    rm_dir(dir);
}

/* ---- crash at every phase ---------------------------------------------- */

static int run_child_crash(const char *dir, int step, int release, OmegaStorageHandle h, OmegaTensor v) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        OmegaTensorStore *st = NULL;
        if (omega_tensor_store_open(dir, 8, &st) != OMEGA_TENSOR_OK) _exit(3);
        if (omega_tensor_store_test_set_crash_step(st, step) != OMEGA_TENSOR_OK) _exit(4);
        OmegaStorageHandle out;
        int rc = release ? omega_tensor_store_release(st, h) : omega_tensor_store_put(st, g, v, &out);
        _exit(rc == OMEGA_TENSOR_OK ? 0 : 5);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* variant 0: put a new value; 1: put a value equal to a live one (shared
 * payload); 2: release. */
static void crash_case(int variant, int step) {
    char dir[512];
    new_dir(dir, sizeof dir);
    uint64_t s3[1] = {3};
    OmegaTensor a = mk_vals(1, s3, 1), b = mk_vals(1, s3, 2), c = mk_vals(1, s3, 3);
    OmegaStorageHandle ha, hb;
    OmegaTensorStore *st = NULL;
    if (omega_tensor_store_open(dir, 8, &st) != OMEGA_TENSOR_OK) { CHECK(0, "crash setup open"); return; }
    omega_tensor_store_put(st, g, a, &ha);
    omega_tensor_store_put(st, g, b, &hb);
    if (variant != 2) omega_tensor_store_release(st, ha);   /* slot 0 free at generation 2 */
    omega_tensor_store_close(st);
    st = NULL;

    OmegaTensor v = variant == 1 ? b : c;
    int ex = run_child_crash(dir, step, variant == 2, ha, v);
    CHECK(ex == 86, "variant %d step %d: child crashed at the hook (exit %d)", variant, step, ex);
    int rc = omega_tensor_store_open(dir, 8, &st);
    CHECK(rc == OMEGA_TENSOR_OK, "variant %d step %d: recovery opens (rc %d)", variant, step, rc);
    if (!st) { release_t(a); release_t(b); release_t(c); rm_dir(dir); return; }
    bool committed = step >= OMEGA_TSTORE_CRASH_JOURNAL_RENAMED;
    CHECK(got_equals(st, hb, b), "variant %d step %d: untouched value intact", variant, step);
    CHECK(!exists(dir, "payload.tmp") && !exists(dir, "journal.tmp"), "variant %d step %d: temp files swept",
          variant, step);
    OmegaTensor tmp;
    if (variant == 2) {
        if (committed) {
            CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE &&
                  omega_tensor_store_live_count(st) == 1, "release step %d: committed, handle stale", step);
        } else {
            CHECK(got_equals(st, ha, a) && omega_tensor_store_live_count(st) == 2,
                  "release step %d: not committed, value intact", step);
        }
    } else {
        OmegaStorageHandle hv = {0, 2};
        CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE, "put step %d: released a stays stale", step);
        if (committed) {
            CHECK(got_equals(st, hv, v) && omega_tensor_store_live_count(st) == 2,
                  "put v%d step %d: committed, new value at slot 0 gen 2", variant, step);
        } else {
            CHECK(!omega_tensor_store_valid(st, hv) && omega_tensor_store_live_count(st) == 1,
                  "put v%d step %d: not committed, nothing new", variant, step);
        }
    }
    /* the recovered store keeps working, and its next handle never reuses a
     * released generation */
    OmegaStorageHandle hn;
    CHECK(omega_tensor_store_put(st, g, c, &hn) == OMEGA_TENSOR_OK && got_equals(st, hn, c),
          "variant %d step %d: put after recovery", variant, step);
    CHECK(!(hn.slot == ha.slot && hn.generation <= ha.generation), "variant %d step %d: no generation reuse",
          variant, step);
    omega_tensor_store_close(st);
    st = NULL;
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK && got_equals(st, hn, c) && got_equals(st, hb, b),
          "variant %d step %d: second recovery stable", variant, step);
    omega_tensor_store_close(st);
    release_t(a);
    release_t(b);
    release_t(c);
    rm_dir(dir);
}

static void test_crash(void) {
    for (int step = OMEGA_TSTORE_CRASH_PAYLOAD_PARTIAL; step < OMEGA_TSTORE_CRASH_COUNT; step++) {
        crash_case(0, step);
        crash_case(1, step);
    }
    for (int step = OMEGA_TSTORE_CRASH_JOURNAL_PARTIAL; step < OMEGA_TSTORE_CRASH_COUNT; step++)
        crash_case(2, step);
}

/* ---- torn writes ------------------------------------------------------- */

/* Store with history: put a, put b, release a. Returns the handles. */
static void torn_setup(const char *dir, OmegaTensor a, OmegaTensor b, OmegaStorageHandle *ha,
                       OmegaStorageHandle *hb) {
    OmegaTensorStore *st = NULL;
    if (omega_tensor_store_open(dir, 8, &st) != OMEGA_TENSOR_OK) { CHECK(0, "torn setup"); return; }
    omega_tensor_store_put(st, g, a, ha);
    omega_tensor_store_put(st, g, b, hb);
    omega_tensor_store_release(st, *ha);
    omega_tensor_store_close(st);
}

static void test_torn(void) {
    char dir[512], jp[1024], pp[1024];
    new_dir(dir, sizeof dir);
    uint64_t s3[1] = {3};
    OmegaTensor a = mk_vals(1, s3, 4), b = mk_vals(1, s3, 5);
    OmegaStorageHandle ha = {0, 0}, hb = {0, 0};
    torn_setup(dir, a, b, &ha, &hb);
    snprintf(jp, sizeof jp, "%s/journal", dir);
    uint8_t vid[32];
    char pname[69];
    omega_tensor_value_id(g, b, vid);
    payload_name(vid, pname);
    snprintf(pp, sizeof pp, "%s/%s", dir, pname);
    uint8_t *jr = NULL, *pr = NULL;
    size_t jn = 0, pn = 0;
    CHECK(read_file(jp, &jr, &jn) == 0 && jn == 56 + 3 * 200, "journal is header + 3 records (%zu bytes)", jn);
    CHECK(read_file(pp, &pr, &pn) == 0 && pn == 12, "payload is 12 bytes (%zu)", pn);
    if (!jr || !pr || jn != 656 || pn != 12) { free(jr); free(pr); return; }
    OmegaTensorStore *st = NULL;
    uint32_t lt0, ls0, lt1, ls1;
    omega_tensor_live_counts(g, &lt0, &ls0);

    /* journal truncated at every byte boundary: refuse */
    int bad = 0;
    for (size_t L = 0; L < jn; L++) {
        write_file(jp, jr, L);
        int rc = omega_tensor_store_open(dir, 8, &st);
        if (rc != OMEGA_TSTORE_ERR_TORN || st) {
            if (bad++ < 3) printf("  journal cut at %zu: rc %d\n", L, rc);
            omega_tensor_store_close(st);
            st = NULL;
        }
    }
    CHECK(bad == 0, "journal truncated at all %zu lengths: all refused TORN (%d not)", jn, bad);
    /* journal extended by one byte and by one full zero record: refuse */
    uint8_t *big = calloc(1, jn + 200);
    memcpy(big, jr, jn);
    write_file(jp, big, jn + 1);
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TSTORE_ERR_TORN && !st, "journal + 1 byte refused");
    write_file(jp, big, jn + 200);
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TSTORE_ERR_TORN && !st, "journal + zero record refused");
    free(big);

    /* every byte of the journal flipped: refuse */
    bad = 0;
    for (size_t i = 0; i < jn; i++) {
        jr[i] ^= (uint8_t)(1u << (i % 8));
        write_file(jp, jr, jn);
        jr[i] ^= (uint8_t)(1u << (i % 8));
        int rc = omega_tensor_store_open(dir, 8, &st);
        if ((rc != OMEGA_TSTORE_ERR_TORN && rc != OMEGA_TSTORE_ERR_CORRUPT) || st) {
            if (bad++ < 3) printf("  journal byte %zu flipped: rc %d\n", i, rc);
            omega_tensor_store_close(st);
            st = NULL;
        }
    }
    CHECK(bad == 0, "journal bit flip at each of %zu bytes: all refused (%d not)", jn, bad);
    write_file(jp, jr, jn);

    /* payload truncated at every byte, extended, flipped: refuse */
    bad = 0;
    for (size_t L = 0; L < pn; L++) {
        write_file(pp, pr, L);
        int rc = omega_tensor_store_open(dir, 8, &st);
        if (rc != OMEGA_TSTORE_ERR_CORRUPT || st) {
            if (bad++ < 3) printf("  payload cut at %zu: rc %d\n", L, rc);
            omega_tensor_store_close(st);
            st = NULL;
        }
    }
    CHECK(bad == 0, "payload truncated at all %zu lengths: all refused CORRUPT (%d not)", pn, bad);
    uint8_t ext[13];
    memcpy(ext, pr, 12);
    ext[12] = 0;
    write_file(pp, ext, 13);
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TSTORE_ERR_CORRUPT && !st, "payload + 1 byte refused");
    bad = 0;
    for (size_t i = 0; i < pn * 8; i++) {
        pr[i / 8] ^= (uint8_t)(1u << (i % 8));
        write_file(pp, pr, pn);
        pr[i / 8] ^= (uint8_t)(1u << (i % 8));
        int rc = omega_tensor_store_open(dir, 8, &st);
        if (rc != OMEGA_TSTORE_ERR_CORRUPT || st) {
            if (bad++ < 3) printf("  payload bit %zu flipped: rc %d\n", i, rc);
            omega_tensor_store_close(st);
            st = NULL;
        }
    }
    CHECK(bad == 0, "payload: each of %zu bits flipped refused (%d not)", pn * 8, bad);
    unlink(pp);
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TSTORE_ERR_CORRUPT && !st, "missing payload refused");
    write_file(pp, pr, pn);

    /* restored files: the committed state comes back exactly */
    CHECK(omega_tensor_store_open(dir, 8, &st) == OMEGA_TENSOR_OK, "restored store opens");
    if (st) {
        OmegaTensor tmp;
        CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE && got_equals(st, hb, b),
              "restored: released stays stale, live value intact");
        /* corruption after open: get re-verifies and returns nothing */
        pr[5] ^= 0x10;
        write_file(pp, pr, pn);
        pr[5] ^= 0x10;
        omega_tensor_live_counts(g, &lt1, &ls1);
        tmp.slot = 0xffffffffU;
        CHECK(omega_tensor_store_get(st, hb, g, &tmp) == OMEGA_TSTORE_ERR_CORRUPT, "payload corrupted after open: get refuses");
        uint32_t lt2, ls2;
        omega_tensor_live_counts(g, &lt2, &ls2);
        CHECK(lt2 == lt1 && ls2 == ls1 && tmp.slot == 0xffffffffU, "refused get created no tensor");
        omega_tensor_store_close(st);
    }
    omega_tensor_live_counts(g, &lt1, &ls1);
    CHECK(lt1 == lt0 && ls1 == ls0, "torn checks leaked no tensors");
    free(jr);
    free(pr);
    release_t(a);
    release_t(b);
    rm_dir(dir);
}

/* ---- crafted journals (independent encoder of the documented format) ---- */

static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void craft_header(uint8_t *h, uint32_t cap, uint64_t count) {
    memset(h, 0, 56);
    memcpy(h, "OTSJRN1", 8);
    le32(h + 8, 1);
    le32(h + 12, cap);
    le64(h + 16, count);
    sha256_hash(h, 24, h + 24);
}

static void craft_record(uint8_t *r, uint32_t kind, uint32_t slot, uint64_t gen, uint64_t seq,
                         uint32_t dtype, uint32_t rank, const uint64_t *shape, const uint8_t *vid,
                         const uint8_t *psha) {
    memset(r, 0, 200);
    memcpy(r, "OTSREC1", 8);
    le32(r + 8, kind);
    le32(r + 12, slot);
    le64(r + 16, gen);
    le64(r + 24, seq);
    le32(r + 32, dtype);
    le32(r + 36, rank);
    for (uint32_t d = 0; d < rank; d++) le64(r + 40 + 8 * d, shape[d]);
    if (vid) memcpy(r + 104, vid, 32);
    if (psha) memcpy(r + 136, psha, 32);
    sha256_hash(r, 168, r + 168);
}

static void test_crafted(void) {
    char dir[512], jp[1024];
    new_dir(dir, sizeof dir);
    snprintf(jp, sizeof jp, "%s/journal", dir);
    uint64_t s3[1] = {3};
    uint32_t bits[3] = {0x3f800000U, 0x7fa00001U, 0x80000000U};
    OmegaTensor a = mk_bits(1, s3, bits);
    OmegaStorageHandle ha;
    OmegaTensorStore *st = NULL;
    if (omega_tensor_store_open(dir, 4, &st) != OMEGA_TENSOR_OK) { CHECK(0, "crafted open"); return; }
    CHECK(omega_tensor_store_put(st, g, a, &ha) == OMEGA_TENSOR_OK, "crafted put");
    omega_tensor_store_close(st);
    st = NULL;

    /* expected bytes from the test's own encoder */
    uint8_t vid[32], psha[32], canon[12];
    omega_tensor_value_id(g, a, vid);
    uint32_t cb[3] = {0x3f800000U, 0x7fc00000U, 0x80000000U};
    for (int i = 0; i < 3; i++) le32(canon + 4 * i, cb[i]);
    sha256_hash(canon, 12, psha);
    uint8_t want[56 + 200 * 2];
    craft_header(want, 4, 1);
    craft_record(want + 56, 1, 0, 1, 0, OMEGA_DT_F32, 1, s3, vid, psha);
    uint8_t *jr = NULL;
    size_t jn = 0;
    CHECK(read_file(jp, &jr, &jn) == 0 && jn == 256 && !memcmp(jr, want, 256),
          "journal bytes match the documented format exactly");
    free(jr);

    struct { const char *what; uint32_t kind, slot; uint64_t gen, seq; int expect; } cases[] = {
        {"release of a free slot",        2, 1, 1, 1, OMEGA_TSTORE_ERR_TORN},
        {"release with wrong generation", 2, 0, 2, 1, OMEGA_TSTORE_ERR_TORN},
        {"record out of sequence",        2, 0, 1, 5, OMEGA_TSTORE_ERR_TORN},
        {"put on a live slot",            1, 0, 1, 1, OMEGA_TSTORE_ERR_TORN},
        {"slot beyond capacity",          2, 4, 1, 1, OMEGA_TSTORE_ERR_TORN},
        {"unknown record kind",           3, 0, 1, 1, OMEGA_TSTORE_ERR_TORN},
        {"valid release (control)",       2, 0, 1, 1, OMEGA_TENSOR_OK},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        craft_header(want, 4, 2);
        craft_record(want + 256, cases[i].kind, cases[i].slot, cases[i].gen, cases[i].seq,
                     OMEGA_DT_F32, 1, s3, vid, psha);
        write_file(jp, want, sizeof want);
        int rc = omega_tensor_store_open(dir, 4, &st);
        CHECK(rc == cases[i].expect, "crafted %s: rc %d, want %d", cases[i].what, rc, cases[i].expect);
        if (st) {
            OmegaTensor tmp;
            CHECK(omega_tensor_store_get(st, ha, g, &tmp) == OMEGA_TENSOR_ERR_STALE, "crafted release took effect");
            omega_tensor_store_close(st);
            st = NULL;
        }
    }
    /* a bad shape inside a correctly checksummed put record */
    uint64_t bad_shape[1] = {0};
    craft_header(want, 4, 1);
    craft_record(want + 56, 1, 0, 1, 0, OMEGA_DT_F32, 1, bad_shape, vid, psha);
    write_file(jp, want, 256);
    CHECK(omega_tensor_store_open(dir, 4, &st) == OMEGA_TSTORE_ERR_TORN && !st, "crafted zero dimension refused");
    craft_record(want + 56, 1, 0, 1, 0, 9, 1, s3, vid, psha);
    write_file(jp, want, 256);
    CHECK(omega_tensor_store_open(dir, 4, &st) == OMEGA_TSTORE_ERR_TORN && !st, "crafted unknown dtype refused");
    release_t(a);
    rm_dir(dir);
}

int main(void) {
    if (!omega_numeric_fpenv_ok()) { printf("FPCR not RNE/no-FTZ: refusing to run\n"); return 2; }
    if (omega_tensor_ctx_create(256, omega_tensor_cpu_realization(), &g)) { printf("ctx\n"); return 2; }
    snprintf(g_root, sizeof g_root, "%s/tensor-store.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_root)) { printf("mkdtemp failed\n"); return 2; }
    test_round_trip();
    test_crash();
    test_torn();
    test_crafted();
    uint32_t lt, ls;
    omega_tensor_live_counts(g, &lt, &ls);
    CHECK(lt == 0 && ls == 0, "no leaked tensors (%u) or storage (%u)", lt, ls);
    omega_tensor_ctx_destroy(g);
    rmdir(g_root);
    printf("M20 OMEGA_TENSOR store tests: %d pass, %d fail\n", g_pass, g_fail);
    printf("M20 verdict: NOT QUALIFIED (crash-safe storage lifetime host tier only)\n");
    return g_fail ? 1 : 0;
}
