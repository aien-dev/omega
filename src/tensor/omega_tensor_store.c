/*
 * M20 OMEGA_TENSOR crash-safe storage lifetime. See omega_tensor_store.h.
 * Commit pattern (temp write, fsync, rename, fsync directory; phased crash
 * points behind a test-only hook that calls _exit) follows
 * src/runtime/rx_generation.c; nothing from src/runtime is linked.
 */
#include "omega_tensor_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sha256.h"

#define JRN_MAGIC "OTSJRN1"          /* 7 chars + NUL = 8 bytes */
#define REC_MAGIC "OTSREC1"
#define JRN_VERSION 1u
#define HDR_BYTES 56u                /* magic 8, version 4, capacity 4, count 8, sum 32 */
#define HDR_SUM   24u
#define REC_BYTES 200u
#define REC_SUM   168u
#define REC_PUT     1u
#define REC_RELEASE 2u
#define PAYLOAD_TMP "payload.tmp"
#define JOURNAL_TMP "journal.tmp"
#define JOURNAL     "journal"
#define PATH_MAX_LEN 600

typedef struct {
    bool       live;
    uint64_t   gen;
    OmegaDType dtype;
    uint32_t   rank;
    uint64_t   shape[OMEGA_TENSOR_MAX_RANK];
    uint8_t    value_id[32];
    uint8_t    payload_sha[32];
} StoreSlot;

typedef struct {
    uint32_t   kind;
    uint32_t   slot;
    uint64_t   gen;
    uint64_t   seq;
    uint32_t   dtype;
    uint32_t   rank;
    uint64_t   shape[OMEGA_TENSOR_MAX_RANK];
    uint8_t    value_id[32];
    uint8_t    payload_sha[32];
} Record;

struct OmegaTensorStore {
    char           dir[PATH_MAX_LEN - 100];
    int            lock_fd;
    uint32_t       cap;
    StoreSlot     *slots;
    uint8_t       *jbuf;       /* journal file image: header + records */
    size_t         jlen, jcap;
    uint64_t       count;      /* records in jbuf */
    OmegaTensorCtx *vctx;      /* private context used to recompute value ids */
    int            crash_step;
};

/* ---- little-endian fields --------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* ---- crash points (test builds only) ---------------------------------- */

#ifdef OMEGA_TENSOR_TEST_HOOKS
#define CRASH_POINT(st, step) do { if ((st)->crash_step == (step)) _exit(86); } while (0)
int omega_tensor_store_test_set_crash_step(OmegaTensorStore *st, int step) {
    if (!st || step < OMEGA_TSTORE_CRASH_NONE || step >= OMEGA_TSTORE_CRASH_COUNT)
        return OMEGA_TENSOR_ERR_BAD_ARGS;
    st->crash_step = step;
    return OMEGA_TENSOR_OK;
}
#else
#define CRASH_POINT(st, step) ((void)(st), (void)(step))
#endif

/* ---- files ------------------------------------------------------------- */

static int path_join(char *out, size_t n, const char *a, const char *b) {
    int w = snprintf(out, n, "%s/%s", a, b);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

static int write_full(int fd, const uint8_t *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int fsync_path(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd) == 0 ? 0 : -1;
    close(fd);
    return rc;
}

/* Reads at most cap bytes of path into buf; *n = bytes read. A file longer
 * than cap reads as cap bytes plus *longer = true. */
static int read_upto(const char *path, uint8_t *buf, size_t cap, size_t *n, bool *longer) {
    *n = 0;
    *longer = false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    for (;;) {
        uint8_t extra;
        uint8_t *dst = *n < cap ? buf + *n : &extra;
        size_t want = *n < cap ? cap - *n : 1;
        ssize_t r = read(fd, dst, want);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) break;
        if (*n >= cap) { *longer = true; break; }
        *n += (size_t)r;
    }
    close(fd);
    return 0;
}

/*
 * Durable replace of <dir>/<name> with buf: write a temp file, fsync it,
 * rename it over the final name, fsync the directory. step0 is the first of
 * four crash points (PARTIAL, WRITTEN, RENAMED, SYNCED).
 */
static int commit_file(OmegaTensorStore *st, const char *tmp_name, const char *name,
                       const uint8_t *buf, size_t n, int step0) {
    char tmp[PATH_MAX_LEN], final[PATH_MAX_LEN];
    if (path_join(tmp, sizeof tmp, st->dir, tmp_name) || path_join(final, sizeof final, st->dir, name))
        return OMEGA_TSTORE_ERR_IO;
    const char *wpath = tmp; /* MUT:TSTORE_RENAME_BEFORE_WRITE (final name only after the bytes are durable) */
    int fd = open(wpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return OMEGA_TSTORE_ERR_IO;
#ifdef OMEGA_TENSOR_TEST_HOOKS
    if (st->crash_step == step0) {
        size_t half = n / 2 ? n / 2 : (n ? 1 : 0);
        if (write_full(fd, buf, half) == 0) (void)fsync(fd);
        close(fd);
        _exit(86);
    }
#endif
    if (write_full(fd, buf, n) != 0 || fsync(fd) != 0) {
        close(fd);
        return OMEGA_TSTORE_ERR_IO;
    }
    if (close(fd) != 0) return OMEGA_TSTORE_ERR_IO;
    CRASH_POINT(st, step0 + 1);
    if (rename(wpath, final) != 0) return OMEGA_TSTORE_ERR_IO;
    CRASH_POINT(st, step0 + 2);
    if (fsync_path(st->dir) != 0) return OMEGA_TSTORE_ERR_IO;
    CRASH_POINT(st, step0 + 3);
    return OMEGA_TENSOR_OK;
}

static void hex_name(const uint8_t id[32], char out[69]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hx[id[i] >> 4];
        out[2 * i + 1] = hx[id[i] & 15];
    }
    memcpy(out + 64, ".bin", 5);
}

/* A payload file name: 64 lowercase hex digits + ".bin". */
static bool is_payload_name(const char *s) {
    if (strlen(s) != 68 || strcmp(s + 64, ".bin") != 0) return false;
    for (int i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

/* ---- shapes and payload bytes ----------------------------------------- */

/* Element count of a shape; 0 when the shape is invalid. */
static uint64_t shape_elems(uint32_t dtype, uint32_t rank, const uint64_t *shape) {
    if (omega_dtype_size((OmegaDType)dtype) == 0 || rank > OMEGA_TENSOR_MAX_RANK) return 0;
    uint64_t e = 1;
    for (uint32_t d = 0; d < rank; d++) {
        if (shape[d] == 0 || shape[d] > OMEGA_TENSOR_MAX_ELEMS) return 0;
        e *= shape[d];
        if (e > OMEGA_TENSOR_MAX_ELEMS) return 0;
    }
    return e;
}

/* Every NaN becomes the canonical quiet NaN of its dtype (same rule as the
 * value id), so a value id names exactly one payload byte string. */
static void canonicalize(OmegaDType dt, uint8_t *buf, uint64_t elems) {
    for (uint64_t e = 0; e < elems; e++) {
        if (dt == OMEGA_DT_F32) {
            uint8_t *p = buf + e * 4;
            uint32_t u = get_u32(p);
            if ((u & 0x7f800000U) == 0x7f800000U && (u & 0x007fffffU)) put_u32(p, 0x7fc00000U);
        } else {
            uint8_t *p = buf + e * 2;
            uint16_t h = (uint16_t)(p[0] | (p[1] << 8));
            uint16_t c = h;
            if (dt == OMEGA_DT_F16 && (h & 0x7c00U) == 0x7c00U && (h & 0x03ffU)) c = 0x7e00U;
            if (dt == OMEGA_DT_BF16 && (h & 0x7f80U) == 0x7f80U && (h & 0x007fU)) c = 0x7fc0U;
            p[0] = (uint8_t)c;
            p[1] = (uint8_t)(c >> 8);
        }
    }
}

/* Value id of a row-major payload, recomputed through the tensor layer. */
static int payload_value_id(OmegaTensorStore *st, const StoreSlot *sl, const uint8_t *buf,
                            uint8_t vid[32]) {
    OmegaTensor t;
    int rc = omega_tensor_from_data(st->vctx, sl->dtype, sl->rank, sl->shape, buf, &t);
    if (rc) return rc;
    rc = omega_tensor_value_id(st->vctx, t, vid);
    int rr = omega_tensor_release(st->vctx, t);
    return rc ? rc : rr;
}

/*
 * Reads and verifies the payload of a live slot. On success *out (if not
 * NULL) receives a malloc'd buffer of exactly the expected size.
 */
static int load_payload(OmegaTensorStore *st, const StoreSlot *sl, uint8_t **out) {
    uint64_t elems = shape_elems((uint32_t)sl->dtype, sl->rank, sl->shape);
    size_t expect = (size_t)elems * omega_dtype_size(sl->dtype);
    if (elems == 0) return OMEGA_TSTORE_ERR_CORRUPT;
    char name[69], path[PATH_MAX_LEN];
    hex_name(sl->value_id, name);
    if (path_join(path, sizeof path, st->dir, name)) return OMEGA_TSTORE_ERR_IO;
    uint8_t *buf = calloc(1, expect);
    if (!buf) return OMEGA_TENSOR_ERR_CAPACITY;
    size_t n;
    bool longer;
    if (read_upto(path, buf, expect, &n, &longer) != 0) {
        int err = errno;
        free(buf);
        return err == ENOENT ? OMEGA_TSTORE_ERR_CORRUPT : OMEGA_TSTORE_ERR_IO;
    }
    /* The payload digest covers the bytes actually read; a short file keeps
     * zeros past its end for the value id recomputation. */
    uint8_t dig[32], vid[32];
    sha256_hash(buf, n, dig);
    int rc = payload_value_id(st, sl, buf, vid);
    if (rc) { free(buf); return rc; }
    if (longer || n != expect) { free(buf); return OMEGA_TSTORE_ERR_CORRUPT; }
    if (memcmp(dig, sl->payload_sha, 32) != 0 || memcmp(vid, sl->value_id, 32) != 0) { /* MUT:TSTORE_HASH_VERIFY */
        free(buf);
        return OMEGA_TSTORE_ERR_CORRUPT;
    }
    if (out) *out = buf;
    else free(buf);
    return OMEGA_TENSOR_OK;
}

/* ---- journal encoding -------------------------------------------------- */

static void encode_header(uint32_t cap, uint64_t count, uint8_t out[HDR_BYTES]) {
    memset(out, 0, HDR_BYTES);
    memcpy(out, JRN_MAGIC, 8);
    put_u32(out + 8, JRN_VERSION);
    put_u32(out + 12, cap);
    put_u64(out + 16, count);
    sha256_hash(out, HDR_SUM, out + HDR_SUM);
}

static void encode_record(const Record *r, uint8_t out[REC_BYTES]) {
    memset(out, 0, REC_BYTES);
    memcpy(out, REC_MAGIC, 8);
    put_u32(out + 8, r->kind);
    put_u32(out + 12, r->slot);
    put_u64(out + 16, r->gen);
    put_u64(out + 24, r->seq);
    put_u32(out + 32, r->dtype);
    put_u32(out + 36, r->rank);
    for (uint32_t d = 0; d < OMEGA_TENSOR_MAX_RANK; d++) put_u64(out + 40 + 8 * d, r->shape[d]);
    memcpy(out + 104, r->value_id, 32);
    memcpy(out + 136, r->payload_sha, 32);
    sha256_hash(out, REC_SUM, out + REC_SUM);
}

static int decode_record(const uint8_t in[REC_BYTES], Record *r) {
    uint8_t sum[32];
    if (memcmp(in, REC_MAGIC, 8) != 0) return OMEGA_TSTORE_ERR_TORN;
    sha256_hash(in, REC_SUM, sum);
    if (memcmp(sum, in + REC_SUM, 32) != 0) return OMEGA_TSTORE_ERR_TORN; /* MUT:TSTORE_RECORD_SUM */
    r->kind = get_u32(in + 8);
    r->slot = get_u32(in + 12);
    r->gen = get_u64(in + 16);
    r->seq = get_u64(in + 24);
    r->dtype = get_u32(in + 32);
    r->rank = get_u32(in + 36);
    for (uint32_t d = 0; d < OMEGA_TENSOR_MAX_RANK; d++) r->shape[d] = get_u64(in + 40 + 8 * d);
    memcpy(r->value_id, in + 104, 32);
    memcpy(r->payload_sha, in + 136, 32);
    return OMEGA_TENSOR_OK;
}

/* Applies one record to the slot table, refusing anything that does not
 * follow from the state before it. Used by replay and by live commits. */
static int apply_record(OmegaTensorStore *st, const Record *r, uint64_t expect_seq) {
    if (r->seq != expect_seq || r->slot >= st->cap) return OMEGA_TSTORE_ERR_TORN;
    StoreSlot *sl = &st->slots[r->slot];
    if (r->kind == REC_PUT) {
        if (sl->live || sl->gen == UINT64_MAX || r->gen != sl->gen) return OMEGA_TSTORE_ERR_TORN;
        if (shape_elems(r->dtype, r->rank, r->shape) == 0) return OMEGA_TSTORE_ERR_TORN;
        for (uint32_t d = r->rank; d < OMEGA_TENSOR_MAX_RANK; d++)
            if (r->shape[d] != 0) return OMEGA_TSTORE_ERR_TORN;
        sl->live = true;
        sl->dtype = (OmegaDType)r->dtype;
        sl->rank = r->rank;
        memcpy(sl->shape, r->shape, sizeof sl->shape);
        memcpy(sl->value_id, r->value_id, 32);
        memcpy(sl->payload_sha, r->payload_sha, 32);
        return OMEGA_TENSOR_OK;
    }
    if (r->kind == REC_RELEASE) {
        if (!sl->live || r->gen != sl->gen) return OMEGA_TSTORE_ERR_TORN;
        sl->live = false;
        sl->gen = r->gen + 1; /* MUT:TSTORE_GEN_RESET (released generation never returns) */
        return OMEGA_TENSOR_OK;
    }
    return OMEGA_TSTORE_ERR_TORN;
}

static int jbuf_reserve(OmegaTensorStore *st, size_t need) {
    if (need <= st->jcap) return 0;
    size_t c = st->jcap ? st->jcap : 4096;
    while (c < need) c *= 2;
    uint8_t *p = realloc(st->jbuf, c);
    if (!p) return -1;
    st->jbuf = p;
    st->jcap = c;
    return 0;
}

/* Appends r (seq = count) and commits the whole journal durably. On any
 * failure the in-memory journal and slot table are unchanged. */
static int commit_record(OmegaTensorStore *st, Record *r) {
    if (r->slot >= st->cap) return OMEGA_TENSOR_ERR_BAD_ARGS;
    r->seq = st->count;
    StoreSlot saved = st->slots[r->slot];
    int rc = apply_record(st, r, st->count);
    if (rc) return rc;
    if (jbuf_reserve(st, st->jlen + REC_BYTES) != 0) {
        st->slots[r->slot] = saved;
        return OMEGA_TENSOR_ERR_CAPACITY;
    }
    size_t old_len = st->jlen;
    encode_record(r, st->jbuf + st->jlen);
    st->jlen += REC_BYTES;
    encode_header(st->cap, st->count + 1, st->jbuf);
    rc = commit_file(st, JOURNAL_TMP, JOURNAL, st->jbuf, st->jlen, OMEGA_TSTORE_CRASH_JOURNAL_PARTIAL);
    if (rc) {
        st->jlen = old_len;
        encode_header(st->cap, st->count, st->jbuf);
        st->slots[r->slot] = saved;
        return rc;
    }
    st->count++;
    return OMEGA_TENSOR_OK;
}

/* ---- recovery ---------------------------------------------------------- */

static bool payload_referenced(const OmegaTensorStore *st, const char *name) {
    char n[69];
    for (uint32_t i = 0; i < st->cap; i++) {
        if (!st->slots[i].live) continue;
        hex_name(st->slots[i].value_id, n);
        if (strcmp(n, name) == 0) return true;
    }
    return false;
}

/* Removes temp files and payloads no live slot refers to. When only_check is
 * set, removes nothing and returns whether any payload file exists. */
static int sweep_dir(OmegaTensorStore *st, bool only_check, bool *any_payload) {
    DIR *d = opendir(st->dir);
    if (!d) return OMEGA_TSTORE_ERR_IO;
    struct dirent *e;
    bool removed = false;
    if (any_payload) *any_payload = false;
    while ((e = readdir(d)) != NULL) {
        bool is_tmp = strcmp(e->d_name, PAYLOAD_TMP) == 0 || strcmp(e->d_name, JOURNAL_TMP) == 0;
        bool is_pay = is_payload_name(e->d_name);
        if (is_pay && any_payload) *any_payload = true;
        if (only_check) continue;
        if (is_tmp || (is_pay && !payload_referenced(st, e->d_name))) {
            char p[PATH_MAX_LEN];
            if (path_join(p, sizeof p, st->dir, e->d_name) == 0 && unlink(p) == 0) removed = true;
        }
    }
    closedir(d);
    if (removed && fsync_path(st->dir) != 0) return OMEGA_TSTORE_ERR_IO;
    return OMEGA_TENSOR_OK;
}

static int load_journal(OmegaTensorStore *st, const char *jpath, uint32_t capacity) {
    struct stat sb;
    int fd = open(jpath, O_RDONLY);
    if (fd < 0) return OMEGA_TSTORE_ERR_IO;
    if (fstat(fd, &sb) != 0 || sb.st_size < 0) { close(fd); return OMEGA_TSTORE_ERR_IO; }
    size_t len = (size_t)sb.st_size;
    if (jbuf_reserve(st, len + REC_BYTES) != 0) { close(fd); return OMEGA_TENSOR_ERR_CAPACITY; }
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, st->jbuf + got, len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    len = got;
    if (len < HDR_BYTES || memcmp(st->jbuf, JRN_MAGIC, 8) != 0) return OMEGA_TSTORE_ERR_TORN;
    uint8_t sum[32];
    sha256_hash(st->jbuf, HDR_SUM, sum);
    if (memcmp(sum, st->jbuf + HDR_SUM, 32) != 0) return OMEGA_TSTORE_ERR_TORN; /* MUT:TSTORE_HEADER_SUM */
    if (get_u32(st->jbuf + 8) != JRN_VERSION) return OMEGA_TSTORE_ERR_TORN;
    if (get_u32(st->jbuf + 12) != capacity) return OMEGA_TENSOR_ERR_BAD_ARGS;
    uint64_t count = get_u64(st->jbuf + 16);
    if (count > (len - HDR_BYTES) / REC_BYTES + 1) count = (len - HDR_BYTES) / REC_BYTES + 1;
    if (len != HDR_BYTES + count * REC_BYTES) return OMEGA_TSTORE_ERR_TORN; /* MUT:TSTORE_SHORT_RECORD */
    uint64_t i = 0;
    for (; i < count && HDR_BYTES + (i + 1) * REC_BYTES <= len; i++) {
        Record r;
        int rc = decode_record(st->jbuf + HDR_BYTES + i * REC_BYTES, &r);
        if (!rc) rc = apply_record(st, &r, i);
        if (rc) return rc;
    }
    /* Keep exactly the records that were replayed (equal to the header count
     * unless the length check above was bypassed). */
    st->count = i;
    st->jlen = HDR_BYTES + i * REC_BYTES;
    encode_header(st->cap, st->count, st->jbuf);
    return OMEGA_TENSOR_OK;
}

int omega_tensor_store_open(const char *dir, uint32_t capacity, OmegaTensorStore **out) {
    if (!out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    *out = NULL;
    if (!dir || capacity == 0 || capacity > OMEGA_TSTORE_MAX_CAPACITY ||
        strlen(dir) + 80 >= sizeof(((OmegaTensorStore *)0)->dir))
        return OMEGA_TENSOR_ERR_BAD_ARGS;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return OMEGA_TSTORE_ERR_IO;
    OmegaTensorStore *st = calloc(1, sizeof(*st));
    if (!st) return OMEGA_TENSOR_ERR_CAPACITY;
    snprintf(st->dir, sizeof st->dir, "%s", dir);
    st->cap = capacity;
    st->lock_fd = -1;
    int rc = OMEGA_TENSOR_OK;
    st->slots = calloc(capacity, sizeof(StoreSlot));
    if (!st->slots) { rc = OMEGA_TENSOR_ERR_CAPACITY; goto fail; }
    for (uint32_t i = 0; i < capacity; i++) st->slots[i].gen = 1;
    rc = omega_tensor_ctx_create(2, omega_tensor_cpu_realization(), &st->vctx);
    if (rc) goto fail;
    st->lock_fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (st->lock_fd < 0) { rc = OMEGA_TSTORE_ERR_IO; goto fail; }
    if (flock(st->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        rc = errno == EWOULDBLOCK ? OMEGA_TSTORE_ERR_BUSY : OMEGA_TSTORE_ERR_IO;
        goto fail;
    }
    char jpath[PATH_MAX_LEN];
    if (path_join(jpath, sizeof jpath, dir, JOURNAL)) { rc = OMEGA_TSTORE_ERR_IO; goto fail; }
    if (access(jpath, F_OK) != 0) {
        if (errno != ENOENT) { rc = OMEGA_TSTORE_ERR_IO; goto fail; }
        /* No journal: a fresh store, unless payloads exist (a lost journal
         * would silently reset every generation). */
        bool any = false;
        rc = sweep_dir(st, true, &any);
        if (rc) goto fail;
        if (any) { rc = OMEGA_TSTORE_ERR_TORN; goto fail; }
        if (jbuf_reserve(st, HDR_BYTES + REC_BYTES) != 0) { rc = OMEGA_TENSOR_ERR_CAPACITY; goto fail; }
        encode_header(capacity, 0, st->jbuf);
        st->jlen = HDR_BYTES;
        rc = commit_file(st, JOURNAL_TMP, JOURNAL, st->jbuf, st->jlen, OMEGA_TSTORE_CRASH_COUNT);
        if (rc) goto fail;
    } else {
        rc = load_journal(st, jpath, capacity);
        if (rc) goto fail;
        for (uint32_t i = 0; i < capacity; i++) {
            if (!st->slots[i].live) continue;
            rc = load_payload(st, &st->slots[i], NULL);
            if (rc) goto fail;
        }
    }
    rc = sweep_dir(st, false, NULL);
    if (rc) goto fail;
    *out = st;
    return OMEGA_TENSOR_OK;
fail:
    omega_tensor_store_close(st);
    return rc;
}

void omega_tensor_store_close(OmegaTensorStore *st) {
    if (!st) return;
    if (st->vctx) omega_tensor_ctx_destroy(st->vctx);
    if (st->lock_fd >= 0) close(st->lock_fd);   /* drops the flock */
    free(st->slots);
    free(st->jbuf);
    free(st);
}

/* ---- live operations --------------------------------------------------- */

static StoreSlot *live_slot(const OmegaTensorStore *st, OmegaStorageHandle h) {
    if (!st || h.slot >= st->cap) return NULL;
    StoreSlot *sl = &st->slots[h.slot];
    if (!sl->live || sl->gen != h.generation) return NULL;
    return sl;
}

bool omega_tensor_store_valid(const OmegaTensorStore *st, OmegaStorageHandle h) {
    return live_slot(st, h) != NULL;
}

int omega_tensor_store_value_id(const OmegaTensorStore *st, OmegaStorageHandle h, uint8_t id[32]) {
    if (!st || !id) return OMEGA_TENSOR_ERR_BAD_ARGS;
    StoreSlot *sl = live_slot(st, h);
    if (!sl) return OMEGA_TENSOR_ERR_STALE;
    memcpy(id, sl->value_id, 32);
    return OMEGA_TENSOR_OK;
}

uint32_t omega_tensor_store_live_count(const OmegaTensorStore *st) {
    uint32_t n = 0;
    if (!st) return 0;
    for (uint32_t i = 0; i < st->cap; i++) n += st->slots[i].live ? 1u : 0u;
    return n;
}

int omega_tensor_store_put(OmegaTensorStore *st, const OmegaTensorCtx *ctx, OmegaTensor t,
                           OmegaStorageHandle *out) {
    if (!st || !ctx || !out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    OmegaTensorInfo info;
    int rc = omega_tensor_info(ctx, t, &info);
    if (rc) return rc;
    uint32_t slot = st->cap;
    for (uint32_t i = 0; i < st->cap; i++)
        if (!st->slots[i].live && st->slots[i].gen != UINT64_MAX) { slot = i; break; }
    if (slot == st->cap) return OMEGA_TENSOR_ERR_CAPACITY;
    size_t es = omega_dtype_size(info.dtype);
    size_t bytes = (size_t)info.elements * es;
    uint8_t *buf = malloc(bytes);
    if (!buf) return OMEGA_TENSOR_ERR_CAPACITY;
    rc = omega_tensor_read(ctx, t, buf, bytes);
    if (rc) { free(buf); return rc; }
    canonicalize(info.dtype, buf, info.elements);
    Record r;
    memset(&r, 0, sizeof r);
    r.kind = REC_PUT;
    r.slot = slot;
    r.gen = st->slots[slot].gen;
    r.dtype = (uint32_t)info.dtype;
    r.rank = info.rank;
    for (uint32_t d = 0; d < info.rank; d++) r.shape[d] = info.shape[d];
    rc = omega_tensor_value_id(ctx, t, r.value_id);
    if (rc) { free(buf); return rc; }
    sha256_hash(buf, bytes, r.payload_sha);
    char name[69];
    hex_name(r.value_id, name);
    /* Payload first, journal second: the journal rename is the commit point. */
    rc = commit_file(st, PAYLOAD_TMP, name, buf, bytes, OMEGA_TSTORE_CRASH_PAYLOAD_PARTIAL);
    free(buf);
    if (rc) return rc;
    rc = commit_record(st, &r);
    if (rc) return rc;
    out->slot = slot;
    out->generation = r.gen;
    return OMEGA_TENSOR_OK;
}

int omega_tensor_store_get(OmegaTensorStore *st, OmegaStorageHandle h, OmegaTensorCtx *ctx,
                           OmegaTensor *out) {
    if (!st || !ctx || !out) return OMEGA_TENSOR_ERR_BAD_ARGS;
    StoreSlot *sl = live_slot(st, h);
    if (!sl) return OMEGA_TENSOR_ERR_STALE;
    uint8_t *buf = NULL;
    int rc = load_payload(st, sl, &buf);
    if (rc) return rc;
    rc = omega_tensor_from_data(ctx, sl->dtype, sl->rank, sl->shape, buf, out);
    free(buf);
    return rc;
}

int omega_tensor_store_release(OmegaTensorStore *st, OmegaStorageHandle h) {
    if (!st) return OMEGA_TENSOR_ERR_BAD_ARGS;
    StoreSlot *sl = live_slot(st, h);
    if (!sl) return OMEGA_TENSOR_ERR_STALE;
    uint8_t vid[32];
    memcpy(vid, sl->value_id, 32);
    Record r;
    memset(&r, 0, sizeof r);
    r.kind = REC_RELEASE;
    r.slot = h.slot;
    r.gen = h.generation;
    int rc = commit_record(st, &r);
    if (rc) return rc;
    /* Committed. Drop the payload if no other live slot shares it; a crash
     * before this leaves an orphan that the next open removes. */
    char name[69], path[PATH_MAX_LEN];
    hex_name(vid, name);
    if (!payload_referenced(st, name) && path_join(path, sizeof path, st->dir, name) == 0)
        (void)unlink(path);
    return OMEGA_TENSOR_OK;
}
