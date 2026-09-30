/*
 * One active generation on disk. A candidate lives beside it until the
 * pointer file is replaced by a rename. Recovery trusts a complete pointer
 * and a complete root, or it refuses. It does not splice the two.
 */
#include "runtime/rx_generation.h"
#include "runtime/rx_argus.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ROOT_BYTES 512
#define ROOT_SUM   480
#define ROOT_AUTH_GEN_HI 432   /* after the excluded ids, before the sum */
#define PTR_BYTES  128
#define PTR_SUM    96
#define JRN_BYTES  128
#define JRN_SUM    96
#define RCT_BYTES  256
#define RCT_SUM    224
#define MAX_BLOB   65536u
#define MAX_CAND   4

enum {
    PHASE_NONE = 0,
    PHASE_CANDIDATE = 1,
    PHASE_FLIPPED = 2,
    PHASE_RECEIPT = 3
};

enum {
    DIG_OBJECT = 0,
    DIG_EVIDENCE,
    DIG_MODEL,
    DIG_REALIZATION,
    DIG_CONFIG,
    DIG_PROVENANCE,
    DIG_COUNT
};

typedef struct {
    uint64_t id;
    uint64_t parent;
    uint64_t lineage;
    uint32_t state;
    uint32_t n_objects;
    uint64_t authority_epoch;
    uint64_t authority_generation;
    uint32_t n_external;
    uint32_t n_excluded;
    uint32_t flags;
    uint64_t lengths[DIG_COUNT];
    uint8_t digest[DIG_COUNT][32];
    uint64_t external_ids[RX_GEN_MAX_EXTERNAL];
    uint64_t excluded_ids[RX_GEN_MAX_EXCLUDED];
} RootView;

typedef struct {
    int used;
    uint64_t id;
    uint64_t parent_id;
    uint32_t proposer;
    int closing;
    int draining;
    int proofs_ok;
    uint64_t authority_epoch;
    uint64_t authority_generation;
    RxGenObject objects[RX_GEN_MAX_OBJECTS];
    uint32_t n_objects;
    int observed_set[RX_GEN_MAX_OBJECTS];
    uint32_t observed_gen[RX_GEN_MAX_OBJECTS];
    uint8_t *blob[DIG_COUNT];
    size_t blob_len[DIG_COUNT];
    RxGenWork work[RX_GEN_MAX_WORK];
    uint32_t n_work;
} Candidate;

struct RxGenStore {
    char dir[384];
    uint64_t active_id;
    uint64_t active_lineage;
    uint64_t next_id;
    int crash_step;
    int lock_fd;
    Candidate cand[MAX_CAND];
    RxGenDiskHook disk_hook;
    void *disk_ctx;
    RxGenPhases phases;
    uint64_t io_bytes, io_syncs;   /* this store's writes and syncs (R15) */
    /* Guards active_id/active_lineage, which the durable executor moves while
     * a reaction may read them. Held only for the copy. */
    pthread_mutex_t active_mu;
    struct RxGenExec *exec;
    /* R16 C5 (rx_gen_bind_authority): set once, never cleared. */
    int bound;
    RxGenCallerFn caller;
    void *caller_ctx;
    RxGenAuthFn bound_auth;
    void *bound_auth_ctx;
};

/* R16 C5: bound once (release), read with acquire by every caller. */
static int store_bound(const RxGenStore *store) {
    return __atomic_load_n(&store->bound, __ATOMIC_ACQUIRE);
}

static void set_active(RxGenStore *store, uint64_t id, uint64_t lineage) {
    pthread_mutex_lock(&store->active_mu);
    store->active_id = id;
    store->active_lineage = lineage;
    pthread_mutex_unlock(&store->active_mu);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int rx_gen_last_phases(const RxGenStore *store, RxGenPhases *out) {
    if (!store || !out) return RX_GEN_ERR_ARG;
    *out = store->phases;
    return store->phases.enter_ns ? RX_GEN_OK : RX_GEN_ERR_MISSING;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void checksum(const uint8_t *p, size_t n, uint8_t out[32]) {
    sha256_hash(p, n, out);
}

/* R15: bytes written and sync calls issued by every store in the process. */
static uint64_t g_io_bytes, g_io_syncs;
void rx_gen_io_counters(uint64_t *bytes, uint64_t *syncs) {
    if (bytes) *bytes = __atomic_load_n(&g_io_bytes, __ATOMIC_RELAXED);
    if (syncs) *syncs = __atomic_load_n(&g_io_syncs, __ATOMIC_RELAXED);
}

int rx_gen_store_io(const RxGenStore *store, uint64_t *bytes, uint64_t *syncs) {
    if (!store) return RX_GEN_ERR_ARG;
    if (bytes) *bytes = __atomic_load_n(&store->io_bytes, __ATOMIC_RELAXED);
    if (syncs) *syncs = __atomic_load_n(&store->io_syncs, __ATOMIC_RELAXED);
    return RX_GEN_OK;
}

/* Every write and sync is charged to the store that issued it and to the
 * process total. */
static int write_full(RxGenStore *store, int fd, const uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
        __atomic_add_fetch(&g_io_bytes, (uint64_t)w, __ATOMIC_RELAXED);
        __atomic_add_fetch(&store->io_bytes, (uint64_t)w, __ATOMIC_RELAXED);
    }
    return 0;
}

static int read_full(const char *path, uint8_t *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)r;
    }
    close(fd);
    return 0;
}

static int fsync_fd(RxGenStore *store, int fd) {
    __atomic_add_fetch(&g_io_syncs, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&store->io_syncs, 1, __ATOMIC_RELAXED);
    return fsync(fd) == 0 ? 0 : -1;
}

static int fsync_path(RxGenStore *store, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync_fd(store, fd);
    close(fd);
    return rc;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
}

static void crash_now(void) { _exit(86); }

static int path_join(char *out, size_t n, const char *a, const char *b) {
    int wrote = snprintf(out, n, "%s/%s", a, b);
    if (wrote < 0 || (size_t)wrote >= n) return -1;
    return 0;
}

static Candidate *find_cand(RxGenStore *store, uint64_t id) {
    for (int i = 0; i < MAX_CAND; i++)
        if (store->cand[i].used && store->cand[i].id == id) return &store->cand[i];
    return NULL;
}

static int copy_blob(uint8_t **dst, size_t *dst_n, const uint8_t *src, size_t n) {
    if (n > MAX_BLOB) return RX_GEN_ERR_ARG;
    uint8_t *p = NULL;
    if (n) {
        p = malloc(n);
        if (!p) return RX_GEN_ERR_IO;
        memcpy(p, src, n);
    }
    free(*dst);
    *dst = p;
    *dst_n = n;
    return RX_GEN_OK;
}

static void encode_root(const RootView *v, uint8_t out[ROOT_BYTES]) {
    memset(out, 0, ROOT_BYTES);
    memcpy(out, "R9ROOT2", 7);
    put_u64(out + 8, v->id);
    put_u64(out + 16, v->parent);
    put_u64(out + 24, v->lineage);
    put_u32(out + 32, v->state);
    put_u32(out + 36, v->n_objects);
    put_u64(out + 40, v->authority_epoch);
    put_u32(out + 48, (uint32_t)v->authority_generation);
    put_u32(out + 52, v->n_external);
    put_u32(out + 56, v->n_excluded);
    put_u32(out + 60, v->flags);
    for (int i = 0; i < DIG_COUNT; i++) put_u64(out + 64 + (size_t)i * 8, v->lengths[i]);
    for (int i = 0; i < DIG_COUNT; i++)
        memcpy(out + 112 + (size_t)i * 32, v->digest[i], 32);
    for (uint32_t i = 0; i < v->n_external && i < RX_GEN_MAX_EXTERNAL; i++)
        put_u64(out + 304 + (size_t)i * 8, v->external_ids[i]);
    for (uint32_t i = 0; i < v->n_excluded && i < RX_GEN_MAX_EXCLUDED; i++)
        put_u64(out + 368 + (size_t)i * 8, v->excluded_ids[i]);
    put_u32(out + ROOT_AUTH_GEN_HI, (uint32_t)(v->authority_generation >> 32));
    checksum(out, ROOT_SUM, out + ROOT_SUM);
}

static int decode_root(const uint8_t in[ROOT_BYTES], RootView *v) {
    /* Version 2 keeps the high half of the 64-bit authority generation at
     * ROOT_AUTH_GEN_HI. Version 1 had none; its high half reads as zero. */
    int v2 = memcmp(in, "R9ROOT2", 7) == 0;
    if (!v2 && memcmp(in, "R9ROOT1", 7) != 0) return RX_GEN_ERR_TORN;
    uint8_t sum[32];
    checksum(in, ROOT_SUM, sum);
    if (memcmp(sum, in + ROOT_SUM, 32) != 0) return RX_GEN_ERR_TORN;
    memset(v, 0, sizeof(*v));
    v->id = get_u64(in + 8);
    v->parent = get_u64(in + 16);
    v->lineage = get_u64(in + 24);
    v->state = get_u32(in + 32);
    v->n_objects = get_u32(in + 36);
    v->authority_epoch = get_u64(in + 40);
    v->authority_generation = get_u32(in + 48);
    if (v2) v->authority_generation |= (uint64_t)get_u32(in + ROOT_AUTH_GEN_HI) << 32;
    v->n_external = get_u32(in + 52);
    v->n_excluded = get_u32(in + 56);
    v->flags = get_u32(in + 60);
    if (v->n_external > RX_GEN_MAX_EXTERNAL || v->n_excluded > RX_GEN_MAX_EXCLUDED)
        return RX_GEN_ERR_TORN;
    for (int i = 0; i < DIG_COUNT; i++) v->lengths[i] = get_u64(in + 64 + (size_t)i * 8);
    for (int i = 0; i < DIG_COUNT; i++)
        memcpy(v->digest[i], in + 112 + (size_t)i * 32, 32);
    for (uint32_t i = 0; i < v->n_external; i++)
        v->external_ids[i] = get_u64(in + 304 + (size_t)i * 8);
    for (uint32_t i = 0; i < v->n_excluded; i++)
        v->excluded_ids[i] = get_u64(in + 368 + (size_t)i * 8);
    return RX_GEN_OK;
}

static void encode_pointer(uint64_t id, uint64_t lineage, const uint8_t digest[32],
                           uint8_t out[PTR_BYTES]) {
    memset(out, 0, PTR_BYTES);
    memcpy(out, "R9PTR01", 7);
    put_u64(out + 8, id);
    put_u64(out + 16, lineage);
    memcpy(out + 24, digest, 32);
    checksum(out, PTR_SUM, out + PTR_SUM);
}

static int decode_pointer(const uint8_t in[PTR_BYTES], uint64_t *id, uint64_t *lineage,
                          uint8_t digest[32]) {
    if (memcmp(in, "R9PTR01", 7) != 0) return RX_GEN_ERR_TORN;
    uint8_t sum[32];
    checksum(in, PTR_SUM, sum);
    if (memcmp(sum, in + PTR_SUM, 32) != 0) return RX_GEN_ERR_TORN;
    *id = get_u64(in + 8);
    *lineage = get_u64(in + 16);
    memcpy(digest, in + 24, 32);
    return RX_GEN_OK;
}

static void encode_journal(uint32_t phase, uint64_t parent, uint64_t candidate,
                           uint64_t parent_lineage, const uint8_t digest[32],
                           uint8_t out[JRN_BYTES]) {
    memset(out, 0, JRN_BYTES);
    memcpy(out, "R9JRN01", 7);
    put_u32(out + 8, phase);
    put_u64(out + 16, parent);
    put_u64(out + 24, candidate);
    put_u64(out + 32, parent_lineage);
    memcpy(out + 40, digest, 32);
    checksum(out, JRN_SUM, out + JRN_SUM);
}

static int decode_journal(const uint8_t in[JRN_BYTES], uint32_t *phase, uint64_t *parent,
                          uint64_t *candidate, uint64_t *parent_lineage, uint8_t digest[32]) {
    if (memcmp(in, "R9JRN01", 7) != 0) return RX_GEN_ERR_TORN;
    uint8_t sum[32];
    checksum(in, JRN_SUM, sum);
    if (memcmp(sum, in + JRN_SUM, 32) != 0) return RX_GEN_ERR_TORN;
    *phase = get_u32(in + 8);
    *parent = get_u64(in + 16);
    *candidate = get_u64(in + 24);
    *parent_lineage = get_u64(in + 32);
    memcpy(digest, in + 40, 32);
    return RX_GEN_OK;
}

static int commit_file(RxGenStore *store, const char *final_path, const uint8_t *buf,
                       size_t n, int during_step) {
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s/tmp/write.partial", store->dir) >= (int)sizeof tmp)
        return RX_GEN_ERR_IO;
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return RX_GEN_ERR_IO;
    if (during_step != RX_CRASH_NONE && store->crash_step == during_step) {
        size_t half = n / 2;
        if (half == 0) half = 1;
        if (half > n) half = n;
        if (write_full(store, fd, buf, half) != 0) {
            close(fd);
            return RX_GEN_ERR_IO;
        }
        fsync_fd(store, fd);
        close(fd);
        crash_now();
    }
    if (write_full(store, fd, buf, n) != 0 || fsync_fd(store, fd) != 0) {
        close(fd);
        return RX_GEN_ERR_IO;
    }
    close(fd);
    if (rename(tmp, final_path) != 0) return RX_GEN_ERR_IO;
    char parent[512];
    snprintf(parent, sizeof parent, "%s", final_path);
    char *slash = strrchr(parent, '/');
    if (!slash) return RX_GEN_ERR_IO;
    *slash = 0;
    if (fsync_path(store, parent) != 0) return RX_GEN_ERR_IO;
    return RX_GEN_OK;
}

static int gen_dir(const char *dir, uint64_t id, char *out, size_t n) {
    int wrote = snprintf(out, n, "%s/g/%llu", dir, (unsigned long long)id);
    if (wrote < 0 || (size_t)wrote >= n) return -1;
    return 0;
}

static int load_root_file(const char *dir, uint64_t id, RootView *view, uint8_t digest[32]) {
    char folder[512], path[512];
    if (gen_dir(dir, id, folder, sizeof folder) != 0) return RX_GEN_ERR_IO;
    if (path_join(path, sizeof path, folder, "root") != 0) return RX_GEN_ERR_IO;
    uint8_t raw[ROOT_BYTES];
    if (read_full(path, raw, sizeof raw) != 0) return RX_GEN_ERR_MISSING;
    int rc = decode_root(raw, view);
    if (rc != RX_GEN_OK) return rc;
    checksum(raw, ROOT_BYTES, digest);
    return RX_GEN_OK;
}

static int file_matches(const char *path, const uint8_t expect[32], uint64_t expect_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return RX_GEN_ERR_MISSING;
    sha256_ctx ctx;
    sha256_init(&ctx);
    uint64_t total = 0;
    uint8_t buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return RX_GEN_ERR_IO;
        }
        if (r == 0) break;
        sha256_update(&ctx, buf, (size_t)r);
        total += (uint64_t)r;
    }
    close(fd);
    uint8_t got[32];
    sha256_final(&ctx, got);
    if (total != expect_len || memcmp(got, expect, 32) != 0) return RX_GEN_ERR_TORN;
    return RX_GEN_OK;
}

static const char *BLOB_NAME[DIG_COUNT] = {
    "objects", "evidence", "model", "realization", "config", "provenance"
};

static int root_reachable(const char *dir, const RootView *view) {
    char folder[512], path[512];
    if (gen_dir(dir, view->id, folder, sizeof folder) != 0) return RX_GEN_ERR_IO;
    for (int i = 0; i < DIG_COUNT; i++) {
        if (path_join(path, sizeof path, folder, BLOB_NAME[i]) != 0) return RX_GEN_ERR_IO;
        int rc = file_matches(path, view->digest[i], view->lengths[i]);
        if (rc != RX_GEN_OK) return rc;
    }
    return RX_GEN_OK;
}

static int read_pointer(const char *dir, uint64_t *id, uint64_t *lineage, uint8_t digest[32]) {
    char path[512];
    if (path_join(path, sizeof path, dir, "active") != 0) return RX_GEN_ERR_IO;
    uint8_t raw[PTR_BYTES];
    if (read_full(path, raw, sizeof raw) != 0) return RX_GEN_ERR_MISSING;
    return decode_pointer(raw, id, lineage, digest);
}

static int read_journal(const char *dir, uint32_t *phase, uint64_t *parent, uint64_t *candidate,
                        uint64_t *parent_lineage, uint8_t digest[32]) {
    char path[512];
    if (path_join(path, sizeof path, dir, "journal") != 0) return RX_GEN_ERR_IO;
    uint8_t raw[JRN_BYTES];
    if (read_full(path, raw, sizeof raw) != 0) return RX_GEN_ERR_MISSING;
    return decode_journal(raw, phase, parent, candidate, parent_lineage, digest);
}

static int event_has(const char *dir, uint64_t id) {
    char path[512];
    if (path_join(path, sizeof path, dir, "events") != 0) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long seen = 0, lineage = 0;
    int found = 0;
    while (fscanf(f, "%llu %llu", &seen, &lineage) == 2) {
        if (seen == (unsigned long long)id) found = 1;
    }
    fclose(f);
    return found;
}

static int append_event(RxGenStore *store, uint64_t id, uint64_t lineage) {
    if (event_has(store->dir, id)) return RX_GEN_OK;
    char path[512];
    if (path_join(path, sizeof path, store->dir, "events") != 0) return RX_GEN_ERR_IO;
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) return RX_GEN_ERR_IO;
    char line[64];
    int n = snprintf(line, sizeof line, "%llu %llu\n", (unsigned long long)id,
                     (unsigned long long)lineage);
    if (n < 0 || write_full(store, fd, (const uint8_t *)line, (size_t)n) != 0 || fsync_fd(store, fd) != 0) {
        close(fd);
        return RX_GEN_ERR_IO;
    }
    close(fd);
    return fsync_path(store, store->dir) == 0 ? RX_GEN_OK : RX_GEN_ERR_IO;
}

static void fill_record(const RootView *view, int receipt, int event, RxRecoveryRecord *out) {
    memset(out, 0, sizeof(*out));
    out->coherent = 1;
    out->active_id = view->id;
    out->lineage = view->lineage;
    out->parent_id = view->parent;
    out->receipt_present = receipt;
    out->event_present = event;
    out->n_external = view->n_external;
    out->n_excluded = view->n_excluded;
    memcpy(out->external_ids, view->external_ids, sizeof view->external_ids);
    memcpy(out->excluded_ids, view->excluded_ids, sizeof view->excluded_ids);
}

static int receipt_exists(const char *dir, uint64_t id) {
    char folder[512], path[512];
    if (gen_dir(dir, id, folder, sizeof folder) != 0) return 0;
    if (path_join(path, sizeof path, folder, "receipt") != 0) return 0;
    return access(path, R_OK) == 0;
}

static int write_receipt_file(RxGenStore *store, const RootView *view, uint32_t subject,
                              uint32_t cap_id, uint64_t cap_gen) {
    uint8_t raw[RCT_BYTES];
    memset(raw, 0, sizeof raw);
    memcpy(raw, "R9RCT02", 7);
    put_u64(raw + 8, view->id);
    put_u64(raw + 16, view->lineage);
    put_u64(raw + 24, view->parent);
    uint8_t root_digest[32];
    char folder[512], root_path[512], path[512];
    if (gen_dir(store->dir, view->id, folder, sizeof folder) != 0) return RX_GEN_ERR_IO;
    if (path_join(root_path, sizeof root_path, folder, "root") != 0) return RX_GEN_ERR_IO;
    uint8_t root_raw[ROOT_BYTES];
    if (read_full(root_path, root_raw, sizeof root_raw) != 0) return RX_GEN_ERR_MISSING;
    checksum(root_raw, ROOT_BYTES, root_digest);
    memcpy(raw + 32, root_digest, 32);
    put_u32(raw + 64, subject);
    put_u32(raw + 68, cap_id);
    put_u32(raw + 72, (uint32_t)cap_gen);
    put_u32(raw + 76, (uint32_t)(cap_gen >> 32));
    checksum(raw, RCT_SUM, raw + RCT_SUM);
    if (path_join(path, sizeof path, folder, "receipt") != 0) return RX_GEN_ERR_IO;
    return commit_file(store, path, raw, sizeof raw, RX_CRASH_NONE);
}

static int write_journal(RxGenStore *store, uint32_t phase, uint64_t parent, uint64_t candidate,
                         uint64_t parent_lineage, const uint8_t digest[32]) {
    uint8_t raw[JRN_BYTES];
    encode_journal(phase, parent, candidate, parent_lineage, digest, raw);
    char path[512];
    if (path_join(path, sizeof path, store->dir, "journal") != 0) return RX_GEN_ERR_IO;
    return commit_file(store, path, raw, sizeof raw, RX_CRASH_NONE);
}

static int write_named_blob(RxGenStore *store, const char *folder, const char *name,
                            const uint8_t *bytes, size_t n, int during) {
    char path[512];
    if (path_join(path, sizeof path, folder, name) != 0) return RX_GEN_ERR_IO;
    return commit_file(store, path, bytes ? bytes : (const uint8_t *)"", n, during);
}

static int objects_bytes(const Candidate *c, uint8_t **out, size_t *out_n) {
    size_t n = 4u + (size_t)c->n_objects * 40u;
    uint8_t *p = malloc(n ? n : 1);
    if (!p) return RX_GEN_ERR_IO;
    put_u32(p, c->n_objects);
    for (uint32_t i = 0; i < c->n_objects; i++) {
        uint8_t *row = p + 4 + (size_t)i * 40;
        put_u32(row, c->objects[i].id);
        put_u32(row + 4, c->objects[i].generation);
        memcpy(row + 8, c->objects[i].digest, 32);
    }
    *out = p;
    *out_n = n;
    return RX_GEN_OK;
}

static void hash_buf(const uint8_t *p, size_t n, uint8_t out[32]) {
    sha256_hash(p ? p : (const uint8_t *)"", n, out);
}

static int release_lock(RxGenStore *store) {
    if (store->lock_fd >= 0) {
        close(store->lock_fd);
        store->lock_fd = -1;
    }
    char path[512];
    if (path_join(path, sizeof path, store->dir, "barrier.lock") != 0) return RX_GEN_ERR_IO;
    if (unlink(path) != 0 && errno != ENOENT) return RX_GEN_ERR_IO;
    return RX_GEN_OK;
}

static int take_lock(RxGenStore *store) {
    char path[512];
    if (path_join(path, sizeof path, store->dir, "barrier.lock") != 0) return RX_GEN_ERR_IO;
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return RX_GEN_ERR_BUSY;
    char pid[32];
    int n = snprintf(pid, sizeof pid, "%ld\n", (long)getpid());
    if (n > 0) write_full(store, fd, (const uint8_t *)pid, (size_t)n);
    fsync_fd(store, fd);
    store->lock_fd = fd;
    return RX_GEN_OK;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static int install_genesis(RxGenStore *store) {
    const uint8_t evidence[] = "GENESIS";
    uint8_t empty_obj[4];
    put_u32(empty_obj, 0);
    const uint8_t *blobs[DIG_COUNT];
    size_t lens[DIG_COUNT];
    blobs[DIG_OBJECT] = empty_obj;
    lens[DIG_OBJECT] = 4;
    blobs[DIG_EVIDENCE] = evidence;
    lens[DIG_EVIDENCE] = sizeof evidence - 1;
    for (int i = DIG_MODEL; i < DIG_COUNT; i++) {
        blobs[i] = (const uint8_t *)"";
        lens[i] = 0;
    }
    char folder[512];
    if (gen_dir(store->dir, 1, folder, sizeof folder) != 0) return RX_GEN_ERR_IO;
    if (ensure_dir(folder) != 0) return RX_GEN_ERR_IO;
    RootView view;
    memset(&view, 0, sizeof view);
    view.id = 1;
    view.parent = 0;
    view.lineage = 1;
    view.state = 1;
    view.flags = 1;
    for (int i = 0; i < DIG_COUNT; i++) {
        if (write_named_blob(store, folder, BLOB_NAME[i], blobs[i], lens[i], RX_CRASH_NONE) !=
            RX_GEN_OK)
            return RX_GEN_ERR_IO;
        view.lengths[i] = lens[i];
        hash_buf(blobs[i], lens[i], view.digest[i]);
    }
    uint8_t root[ROOT_BYTES];
    encode_root(&view, root);
    char root_path[512];
    if (path_join(root_path, sizeof root_path, folder, "root") != 0) return RX_GEN_ERR_IO;
    if (commit_file(store, root_path, root, sizeof root, RX_CRASH_NONE) != RX_GEN_OK)
        return RX_GEN_ERR_IO;
    uint8_t root_digest[32];
    checksum(root, ROOT_BYTES, root_digest);
    uint8_t pointer[PTR_BYTES];
    encode_pointer(1, 1, root_digest, pointer);
    char active[512];
    if (path_join(active, sizeof active, store->dir, "active") != 0) return RX_GEN_ERR_IO;
    if (commit_file(store, active, pointer, sizeof pointer, RX_CRASH_NONE) != RX_GEN_OK)
        return RX_GEN_ERR_IO;
    if (write_journal(store, PHASE_RECEIPT, 0, 1, 0, root_digest) != RX_GEN_OK)
        return RX_GEN_ERR_IO;
    store->active_id = 1;
    store->active_lineage = 1;
    store->next_id = 2;
    char nextp[512];
    if (path_join(nextp, sizeof nextp, store->dir, "nextid") != 0) return RX_GEN_ERR_IO;
    char num[32];
    int n = snprintf(num, sizeof num, "2\n");
    return commit_file(store, nextp, (const uint8_t *)num, (size_t)n, RX_CRASH_NONE);
}

int rx_gen_open(const char *dir, RxGenStore **out) {
    if (!dir || !out) return RX_GEN_ERR_ARG;
    RxGenStore *store = calloc(1, sizeof(*store));
    if (!store) return RX_GEN_ERR_IO;
    pthread_mutex_init(&store->active_mu, NULL);
    if (snprintf(store->dir, sizeof store->dir, "%s", dir) >= (int)sizeof store->dir) {
        free(store);
        return RX_GEN_ERR_ARG;
    }
    store->lock_fd = -1;
    char gdir[512], tmp[512];
    if (path_join(gdir, sizeof gdir, store->dir, "g") != 0 ||
        path_join(tmp, sizeof tmp, store->dir, "tmp") != 0) {
        free(store);
        return RX_GEN_ERR_IO;
    }
    if (ensure_dir(store->dir) != 0 || ensure_dir(gdir) != 0 || ensure_dir(tmp) != 0) {
        free(store);
        return RX_GEN_ERR_IO;
    }
    char active[512];
    path_join(active, sizeof active, store->dir, "active");
    if (access(active, R_OK) != 0) {
        int rc = install_genesis(store);
        if (rc != RX_GEN_OK) {
            free(store);
            return rc;
        }
    } else {
        uint8_t digest[32];
        int rc = read_pointer(store->dir, &store->active_id, &store->active_lineage, digest);
        if (rc != RX_GEN_OK) {
            free(store);
            return rc;
        }
        FILE *f = NULL;
        char nextp[512];
        path_join(nextp, sizeof nextp, store->dir, "nextid");
        f = fopen(nextp, "r");
        unsigned long long next = store->active_id + 1;
        if (f) {
            if (fscanf(f, "%llu", &next) != 1) next = store->active_id + 1;
            fclose(f);
        }
        store->next_id = (uint64_t)next;
    }
    *out = store;
    return RX_GEN_OK;
}

static void exec_stop(RxGenStore *store);

void rx_gen_close(RxGenStore *store) {
    if (!store) return;
    exec_stop(store);
    for (int i = 0; i < MAX_CAND; i++) {
        for (int b = 0; b < DIG_COUNT; b++) free(store->cand[i].blob[b]);
    }
    if (store->lock_fd >= 0) close(store->lock_fd);
    pthread_mutex_destroy(&store->active_mu);
    free(store);
}

int rx_gen_active(const RxGenStore *store, uint64_t *id, uint64_t *lineage) {
    if (!store || !id || !lineage) return RX_GEN_ERR_ARG;
    RxGenStore *s = (RxGenStore *)store;
    pthread_mutex_lock(&s->active_mu);
    *id = s->active_id;
    *lineage = s->active_lineage;
    pthread_mutex_unlock(&s->active_mu);
    return RX_GEN_OK;
}

int rx_gen_read_blob(const RxGenStore *store, uint64_t id, const char *name, uint8_t *buf,
                     size_t cap, size_t *out_len) {
    if (!store || !name || !out_len || (!buf && cap)) return RX_GEN_ERR_ARG;
    int which = -1;
    for (int i = 0; i < DIG_COUNT; i++)
        if (strcmp(name, BLOB_NAME[i]) == 0) which = i;
    if (which < 0) return RX_GEN_ERR_ARG;
    RootView view;
    uint8_t root_digest[32];
    int rc = load_root_file(store->dir, id, &view, root_digest);
    if (rc != RX_GEN_OK) return rc;
    if (view.id != id) return RX_GEN_ERR_TORN;
    if (view.lengths[which] > cap) return RX_GEN_ERR_ARG;
    char folder[512], path[512];
    if (gen_dir(store->dir, id, folder, sizeof folder) != 0 ||
        path_join(path, sizeof path, folder, name) != 0) return RX_GEN_ERR_IO;
    rc = file_matches(path, view.digest[which], view.lengths[which]);
    if (rc != RX_GEN_OK) return rc;
    if (view.lengths[which] && read_full(path, buf, (size_t)view.lengths[which]) != 0)
        return RX_GEN_ERR_IO;
    /* The bytes read must be the bytes the root names, not whatever the file
     * became after the check above. */
    uint8_t got[32];
    hash_buf(buf, (size_t)view.lengths[which], got);
    if (memcmp(got, view.digest[which], 32) != 0) return RX_GEN_ERR_TORN;
    *out_len = (size_t)view.lengths[which];
    return RX_GEN_OK;
}

int rx_gen_bind_authority(RxGenStore *store, RxGenCallerFn caller, void *caller_ctx,
                          RxGenAuthFn auth, void *auth_ctx) {
    if (!store || !caller || !auth) return RX_GEN_ERR_ARG;
    if (store_bound(store)) return RX_GEN_ERR_BUSY;
    store->caller = caller;
    store->caller_ctx = caller_ctx;
    store->bound_auth = auth;
    store->bound_auth_ctx = auth_ctx;
    __atomic_store_n(&store->bound, 1, __ATOMIC_RELEASE);
    return RX_GEN_OK;
}

int rx_gen_propose(RxGenStore *store, uint32_t proposer, const RxGenDraft *draft,
                   uint64_t *out_id) {
    return rx_gen_propose_as(store, proposer, NULL, draft, out_id);
}

int rx_gen_propose_as(RxGenStore *store, uint32_t proposer, const RxCallerCred *cred,
                      const RxGenDraft *draft, uint64_t *out_id) {
    if (!store || !draft || !out_id) return RX_GEN_ERR_ARG;
    /* R16 C5: the proposer is who the credential says, or nobody. */
    if (store_bound(store) && store->caller(store->caller_ctx, proposer, cred, RX_CALLER_OP_CHECK) != 0)
        return RX_GEN_ERR_IDENTITY;
    if (draft->n_objects > RX_GEN_MAX_OBJECTS) return RX_GEN_ERR_ARG;
    if (draft->n_objects && !draft->objects) return RX_GEN_ERR_ARG;
    Candidate *slot = NULL;
    for (int i = 0; i < MAX_CAND; i++)
        if (!store->cand[i].used) {
            slot = &store->cand[i];
            break;
        }
    if (!slot) return RX_GEN_ERR_BUSY;
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->id = store->next_id++;
    slot->parent_id = store->active_id;
    slot->proposer = proposer;
    slot->proofs_ok = draft->proofs_ok;
    slot->authority_epoch = draft->authority_epoch;
    slot->authority_generation = draft->authority_generation;
    slot->n_objects = draft->n_objects;
    if (draft->n_objects) memcpy(slot->objects, draft->objects, draft->n_objects * sizeof(RxGenObject));
    const uint8_t *src[DIG_COUNT] = {NULL, draft->evidence, draft->model, draft->realization,
                                     draft->config, draft->provenance};
    size_t lens[DIG_COUNT] = {0, draft->evidence_len, draft->model_len, draft->realization_len,
                              draft->config_len, draft->provenance_len};
    uint8_t *obj = NULL;
    size_t obj_n = 0;
    int rc = objects_bytes(slot, &obj, &obj_n);
    if (rc != RX_GEN_OK) {
        slot->used = 0;
        return rc;
    }
    src[DIG_OBJECT] = obj;
    lens[DIG_OBJECT] = obj_n;
    for (int i = 0; i < DIG_COUNT; i++) {
        rc = copy_blob(&slot->blob[i], &slot->blob_len[i], src[i], lens[i]);
        if (rc != RX_GEN_OK) {
            free(obj);
            for (int b = 0; b < DIG_COUNT; b++) free(slot->blob[b]);
            memset(slot, 0, sizeof(*slot));
            return rc;
        }
    }
    free(obj);
    char nextp[512], num[32];
    if (path_join(nextp, sizeof nextp, store->dir, "nextid") != 0) return RX_GEN_ERR_IO;
    int n = snprintf(num, sizeof num, "%llu\n", (unsigned long long)store->next_id);
    rc = commit_file(store, nextp, (const uint8_t *)num, (size_t)n, RX_CRASH_NONE);
    if (rc != RX_GEN_OK) return rc;
    *out_id = slot->id;
    return RX_GEN_OK;
}

int rx_gen_mutate_object(RxGenStore *store, uint64_t candidate, uint32_t index,
                         uint32_t generation, const uint8_t digest[32]) {
    if (!store || !digest) return RX_GEN_ERR_ARG;
    /* R16 C7: these edits carry no credential; a bound store refuses them. */
    if (store_bound(store)) return RX_GEN_ERR_IDENTITY;
    Candidate *c = find_cand(store, candidate);
    if (!c || index >= c->n_objects) return RX_GEN_ERR_ARG;
    if (c->closing) return RX_GEN_ERR_CLOSING;
    c->objects[index].generation = generation;
    memcpy(c->objects[index].digest, digest, 32);
    uint8_t *obj = NULL;
    size_t obj_n = 0;
    int rc = objects_bytes(c, &obj, &obj_n);
    if (rc != RX_GEN_OK) return rc;
    rc = copy_blob(&c->blob[DIG_OBJECT], &c->blob_len[DIG_OBJECT], obj, obj_n);
    free(obj);
    return rc;
}

int rx_gen_set_evidence(RxGenStore *store, uint64_t candidate, const uint8_t *bytes, size_t n) {
    if (!store) return RX_GEN_ERR_ARG;
    if (store_bound(store)) return RX_GEN_ERR_IDENTITY;
    Candidate *c = find_cand(store, candidate);
    if (!c) return RX_GEN_ERR_ARG;
    if (c->closing && !c->draining) return RX_GEN_ERR_CLOSING;
    return copy_blob(&c->blob[DIG_EVIDENCE], &c->blob_len[DIG_EVIDENCE], bytes, n);
}

int rx_gen_observe_object(RxGenStore *store, uint64_t candidate, uint32_t index,
                          uint32_t generation) {
    Candidate *c = find_cand(store, candidate);
    if (!c || index >= c->n_objects) return RX_GEN_ERR_ARG;
    c->observed_set[index] = 1;
    c->observed_gen[index] = generation;
    return RX_GEN_OK;
}

int rx_gen_add_work(RxGenStore *store, uint64_t candidate, const RxGenWork *work) {
    /* R16 C7: no credential, so a bound store refuses it (identity error). */
    return rx_gen_add_work_as(store, 0, NULL, candidate, work);
}

int rx_gen_add_work_as(RxGenStore *store, uint32_t subject, const RxCallerCred *cred,
                       uint64_t candidate, const RxGenWork *work) {
    if (!store || !work) return RX_GEN_ERR_ARG;
    /* R16 C7: on a bound store the caller is who the credential says. */
    if (store_bound(store) && store->caller(store->caller_ctx, subject, cred, RX_CALLER_OP_CHECK) != 0)
        return RX_GEN_ERR_IDENTITY;
    Candidate *c = find_cand(store, candidate);
    if (!c) return RX_GEN_ERR_ARG;
    if (c->closing) return RX_GEN_ERR_CLOSING;
    if (c->n_work >= RX_GEN_MAX_WORK) return RX_GEN_ERR_BUSY;
    if (work->class != RX_WORK_EPHEMERAL && work->class != RX_WORK_EVIDENCE &&
        work->class != RX_WORK_EXTERNAL)
        return RX_GEN_ERR_ARG;
    /* R16 C7: work starts pending or issued; only rx_gen_finish_work(_as)
     * marks it done, and only classify cancels it. */
    int starts_open = work->state == RX_WORK_PENDING || work->state == RX_WORK_ISSUED;
    if (!starts_open) return RX_GEN_ERR_ARG;
    c->work[c->n_work++] = *work;
    return RX_GEN_OK;
}

int rx_gen_finish_work(RxGenStore *store, uint64_t candidate, uint64_t work_id) {
    /* R16 C7: no credential, so a bound store refuses it (identity error). */
    return rx_gen_finish_work_as(store, 0, NULL, candidate, work_id);
}

int rx_gen_finish_work_as(RxGenStore *store, uint32_t subject, const RxCallerCred *cred,
                          uint64_t candidate, uint64_t work_id) {
    if (!store) return RX_GEN_ERR_ARG;
    if (store_bound(store) && store->caller(store->caller_ctx, subject, cred, RX_CALLER_OP_CHECK) != 0)
        return RX_GEN_ERR_IDENTITY;
    Candidate *c = find_cand(store, candidate);
    if (!c) return RX_GEN_ERR_ARG;
    for (uint32_t i = 0; i < c->n_work; i++) {
        if (c->work[i].id == work_id) {
            c->work[i].state = RX_WORK_DONE;
            return RX_GEN_OK;
        }
    }
    return RX_GEN_ERR_MISSING;
}

void rx_gen_set_crash(RxGenStore *store, int step) {
    if (store) store->crash_step = step;
}

void rx_gen_set_disk_hook(RxGenStore *store, RxGenDiskHook fn, void *ctx) {
    if (!store) return;
    store->disk_hook = fn;
    store->disk_ctx = ctx;
}

int rx_gen_hold_barrier(RxGenStore *store) {
    if (!store) return RX_GEN_ERR_ARG;
    if (store->lock_fd >= 0) return RX_GEN_OK;
    return take_lock(store);
}

int rx_gen_release_barrier(RxGenStore *store) {
    if (!store) return RX_GEN_ERR_ARG;
    return release_lock(store);
}

static int classify(RxGenStore *store, Candidate *c, RxGenDrainFn drain, void *drain_ctx,
                    uint64_t *external, uint32_t *n_external, uint64_t *excluded,
                    uint32_t *n_excluded) {
    *n_external = 0;
    *n_excluded = 0;
    c->draining = 1;
    for (uint32_t i = 0; i < c->n_work; i++) {
        RxGenWork *w = &c->work[i];
        if (w->class == RX_WORK_EPHEMERAL) {
            if (w->state != RX_WORK_DONE) w->state = RX_WORK_CANCELLED;
            continue;
        }
        if (w->class == RX_WORK_EVIDENCE) {
            if (w->state != RX_WORK_DONE && w->required) {
                int drained = drain && drain(w->id, drain_ctx) == 0 && w->state == RX_WORK_DONE;
                if (!drained) {
                    c->draining = 0;
                    return RX_GEN_ERR_VERIFY;
                }
            }
            if (w->state != RX_WORK_DONE) {
                if (*n_excluded >= RX_GEN_MAX_EXCLUDED) {
                    c->draining = 0;
                    return RX_GEN_ERR_VERIFY;
                }
                excluded[(*n_excluded)++] = w->id;
            } else if (c->blob_len[DIG_EVIDENCE] == 0) {
                c->draining = 0;
                return RX_GEN_ERR_VERIFY;
            }
            continue;
        }
        if (w->state == RX_WORK_PENDING) {
            w->state = RX_WORK_CANCELLED;
            continue;
        }
        if (*n_external >= RX_GEN_MAX_EXTERNAL) {
            c->draining = 0;
            return RX_GEN_ERR_VERIFY;
        }
        external[(*n_external)++] = w->id;
    }
    c->draining = 0;
    qsort(external, *n_external, sizeof(uint64_t), compare_u64);
    qsort(excluded, *n_excluded, sizeof(uint64_t), compare_u64);
    (void)store;
    return RX_GEN_OK;
}

static void release_candidate(Candidate *c) {
    for (int b = 0; b < DIG_COUNT; b++) free(c->blob[b]);
    memset(c, 0, sizeof(*c));
}

int rx_gen_promote(RxGenStore *store, const RxPromotionRequest *request, RxGenAuthFn auth,
                   void *auth_ctx, RxGenDrainFn drain, void *drain_ctx, RxGenLiveFn live,
                   void *live_ctx) {
    if (!store || !request) return RX_GEN_ERR_ARG;
    /* R16 C5: the request's subject is the caller's only with the credential
     * the runtime issued for it; and a bound store validates the promotion
     * right with its own authority, never the caller's callback. */
    if (store_bound(store)) {
        if (store->caller(store->caller_ctx, request->subject, &request->caller, RX_CALLER_OP_CHECK) != 0)
            return RX_GEN_ERR_IDENTITY;
        auth = store->bound_auth;
        auth_ctx = store->bound_auth_ctx;
    }
    if (!auth) return RX_GEN_ERR_ARG;
    Candidate *c = find_cand(store, request->candidate_id);
    if (!c) return RX_GEN_ERR_ARG;
    int locked_here = 0;
    if (store->lock_fd < 0) {
        int rc = take_lock(store);
        if (rc != RX_GEN_OK) return rc;
        locked_here = 1;
    } else {
        return RX_GEN_ERR_BUSY;
    }
    memset(&store->phases, 0, sizeof store->phases);
    store->phases.candidate_id = request->candidate_id;
    store->phases.enter_ns = monotonic_ns();
    int held = 0;   /* R16 C7: the caller check is held across the flip */
    int rc = RX_GEN_OK;
    uint64_t disk_id = 0, disk_lineage = 0;
    uint8_t disk_digest[32];
    rc = read_pointer(store->dir, &disk_id, &disk_lineage, disk_digest);
    if (rc != RX_GEN_OK) goto done;
    set_active(store, disk_id, disk_lineage);
    if (c->parent_id != store->active_id) {
        rc = RX_GEN_ERR_STALE;
        goto done;
    }
    if (request->subject == c->proposer) {
        rc = RX_GEN_ERR_AUTHORITY;
        goto done;
    }
    if (request->resource != RX_GEN_RES_PROMOTION || request->rights != RX_GEN_RIGHT_PROMOTE) {
        rc = RX_GEN_ERR_AUTHORITY;
        goto done;
    }
#if RX_ARGUS
    /* ARGUS: the promotion authority check (tick unknown here: the view is opaque). */
    uint64_t argus_key = rx_argus_use_begin();
#endif
    int auth_rc = auth(auth_ctx, request->cap_id, request->cap_generation, request->subject,
                       request->resource, request->rights);
#if RX_ARGUS
    rx_argus_use_end(argus_key, NULL, request->subject, request->cap_id,
                     (uint64_t)request->cap_generation, request->resource, auth_rc);
#endif
    if (auth_rc != 0) {
        rc = RX_GEN_ERR_AUTHORITY;
        goto done;
    }
    c->closing = 1;
    if (live) live(live_ctx);
    store->phases.barrier_ns = monotonic_ns();
    uint64_t external[RX_GEN_MAX_EXTERNAL];
    uint64_t excluded[RX_GEN_MAX_EXCLUDED];
    uint32_t n_external = 0, n_excluded = 0;
    rc = classify(store, c, drain, drain_ctx, external, &n_external, excluded, &n_excluded);
    if (rc != RX_GEN_OK) goto done;
    for (uint32_t i = 0; i < c->n_objects; i++) {
        if (c->observed_set[i] && c->observed_gen[i] != c->objects[i].generation) {
            rc = RX_GEN_ERR_STALE;
            goto done;
        }
    }
    if (!c->proofs_ok) {
        rc = RX_GEN_ERR_VERIFY;
        goto done;
    }
    store->phases.verified_ns = monotonic_ns();
    if (store->crash_step == RX_CRASH_BEFORE_CANDIDATE_WRITE) crash_now();

    char folder[512];
    if (gen_dir(store->dir, c->id, folder, sizeof folder) != 0) {
        rc = RX_GEN_ERR_IO;
        goto done;
    }
    if (ensure_dir(folder) != 0) {
        rc = RX_GEN_ERR_IO;
        goto done;
    }
    RootView view;
    memset(&view, 0, sizeof view);
    view.id = c->id;
    view.parent = c->parent_id;
    view.lineage = store->active_lineage + 1;
    view.state = 2;
    view.n_objects = c->n_objects;
    view.authority_epoch = c->authority_epoch;
    view.authority_generation = c->authority_generation;
    view.n_external = n_external;
    view.n_excluded = n_excluded;
    view.flags = 1;
    memcpy(view.external_ids, external, sizeof external);
    memcpy(view.excluded_ids, excluded, sizeof excluded);
    for (int i = 0; i < DIG_COUNT; i++) {
        int during = (i == DIG_OBJECT) ? RX_CRASH_DURING_CANDIDATE_WRITE : RX_CRASH_NONE;
        rc = write_named_blob(store, folder, BLOB_NAME[i], c->blob[i], c->blob_len[i], during);
        if (rc != RX_GEN_OK) goto done;
        view.lengths[i] = c->blob_len[i];
        hash_buf(c->blob[i], c->blob_len[i], view.digest[i]);
    }
    store->phases.blobs_ns = monotonic_ns();
    uint8_t root[ROOT_BYTES];
    encode_root(&view, root);
    char root_path[512];
    if (path_join(root_path, sizeof root_path, folder, "root") != 0) {
        rc = RX_GEN_ERR_IO;
        goto done;
    }
    rc = commit_file(store, root_path, root, sizeof root, RX_CRASH_NONE);
    if (rc != RX_GEN_OK) goto done;
    uint8_t root_digest[32];
    checksum(root, ROOT_BYTES, root_digest);
    rc = write_journal(store, PHASE_CANDIDATE, c->parent_id, c->id, store->active_lineage,
                       root_digest);
    if (rc != RX_GEN_OK) goto done;
    if (store->crash_step == RX_CRASH_AFTER_CANDIDATE_WRITE) crash_now();
    store->phases.candidate_ns = monotonic_ns();
    if (store->disk_hook) store->disk_hook(folder, store->disk_ctx);
    rc = root_reachable(store->dir, &view);
    if (rc != RX_GEN_OK) goto done;
    store->phases.reachable_ns = monotonic_ns();
    if (store->crash_step == RX_CRASH_BEFORE_ROOT_FLIP) crash_now();

    uint8_t pointer[PTR_BYTES];
    encode_pointer(view.id, view.lineage, root_digest, pointer);
    char active_path[512];
    if (path_join(active_path, sizeof active_path, store->dir, "active") != 0) {
        rc = RX_GEN_ERR_IO;
        goto done;
    }
    /* R16 C7: identity again at the durable commit point. The promoter may
     * have been revoked since entry (in the live barrier, during the disk
     * writes); refuse before the pointer moves. HOLD keeps any revocation out
     * until the flip is in memory too. rx_gen_bind_authority is startup-only
     * and lock-free: by contract no bind races an in-flight promotion. */
    int recheck = store_bound(store);
    if (recheck) {
        if (store->caller(store->caller_ctx, request->subject, &request->caller, RX_CALLER_OP_HOLD) != 0) {
            rc = RX_GEN_ERR_IDENTITY;
            goto done;
        }
        held = 1;
    }
    rc = commit_file(store, active_path, pointer, sizeof pointer, RX_CRASH_DURING_ROOT_FLIP);
    if (rc == RX_GEN_OK) {
        store->phases.flip_ns = monotonic_ns();
        set_active(store, view.id, view.lineage);
    }
    if (held) {
        store->caller(store->caller_ctx, request->subject, NULL, RX_CALLER_OP_RELEASE);
        held = 0;
    }
    if (rc != RX_GEN_OK) goto done;
    /* ARGUS: the World is committed at the flip. object_id = this store's identity
     * (hash of its directory: a reopened store keeps it), world_generation = lineage
     * (exactly parent+1 within a store). */
    RX_ARGUS_EMIT(rx_argus_emit_world_committed(rx_argus_store_id(store->dir), request->subject,
                                                request->cap_id, (uint64_t)request->cap_generation,
                                                view.lineage, root_digest, 0));
    if (store->crash_step == RX_CRASH_AFTER_ROOT_FLIP) crash_now();
    rc = write_journal(store, PHASE_FLIPPED, c->parent_id, c->id, view.lineage - 1, root_digest);
    if (rc != RX_GEN_OK) goto done;
    store->phases.flipped_ns = monotonic_ns();
    if (store->crash_step == RX_CRASH_BEFORE_RECEIPT) crash_now();
    rc = write_receipt_file(store, &view, request->subject, request->cap_id,
                            request->cap_generation);
    if (rc != RX_GEN_OK) goto done;
    store->phases.receipt_file_ns = monotonic_ns();
    rc = append_event(store, view.id, view.lineage);
    if (rc != RX_GEN_OK) goto done;
    store->phases.event_ns = monotonic_ns();
    if (store->crash_step == RX_CRASH_AFTER_RECEIPT) crash_now();
    rc = write_journal(store, PHASE_RECEIPT, c->parent_id, c->id, view.lineage - 1, root_digest);
    if (rc == RX_GEN_OK) store->phases.receipt_ns = monotonic_ns();

done:
    store->phases.result = rc;
    if (held) store->caller(store->caller_ctx, request->subject, NULL, RX_CALLER_OP_RELEASE);
    /* The draft is finished: it is the active generation now, or it can never
     * be promoted (its parent is gone or an object it observed moved). Its
     * slot is free for the next proposal; the committed bytes live on disk. */
    if (store->active_id == c->id || rc == RX_GEN_ERR_STALE) release_candidate(c);
    if (locked_here) release_lock(store);
    return rc;
}

int rx_gen_recover(const char *dir, RxRecoveryRecord *out) {
    if (!dir || !out) return RX_GEN_ERR_ARG;
    memset(out, 0, sizeof(*out));
    char lock[512];
    if (path_join(lock, sizeof lock, dir, "barrier.lock") == 0) unlink(lock);

    uint64_t id = 0, lineage = 0;
    uint8_t pointer_digest[32];
    int pointer_rc = read_pointer(dir, &id, &lineage, pointer_digest);
    uint32_t phase = PHASE_NONE;
    uint64_t parent = 0, candidate = 0, parent_lineage = 0;
    uint8_t journal_digest[32];
    int journal_rc =
        read_journal(dir, &phase, &parent, &candidate, &parent_lineage, journal_digest);

    if (pointer_rc != RX_GEN_OK && journal_rc == RX_GEN_OK && phase >= PHASE_FLIPPED) {
        RootView repaired;
        uint8_t got[32];
        int rc = load_root_file(dir, candidate, &repaired, got);
        if (rc != RX_GEN_OK) return rc;
        if (memcmp(got, journal_digest, 32) != 0) return RX_GEN_ERR_TORN;
        if (root_reachable(dir, &repaired) != RX_GEN_OK) return RX_GEN_ERR_TORN;
        RxGenStore scratch;
        memset(&scratch, 0, sizeof scratch);
        scratch.lock_fd = -1;
        snprintf(scratch.dir, sizeof scratch.dir, "%s", dir);
        uint8_t pointer[PTR_BYTES];
        encode_pointer(repaired.id, repaired.lineage, got, pointer);
        char active[512];
        if (path_join(active, sizeof active, dir, "active") != 0) return RX_GEN_ERR_IO;
        rc = commit_file(&scratch, active, pointer, sizeof pointer, RX_CRASH_NONE);
        if (rc != RX_GEN_OK) return rc;
        id = repaired.id;
        lineage = repaired.lineage;
        memcpy(pointer_digest, got, 32);
        pointer_rc = RX_GEN_OK;
    }
    if (pointer_rc != RX_GEN_OK) return pointer_rc;

    RootView view;
    uint8_t root_digest[32];
    int rc = load_root_file(dir, id, &view, root_digest);
    if (rc != RX_GEN_OK) return rc;
    if (view.id != id || view.lineage != lineage || memcmp(root_digest, pointer_digest, 32) != 0)
        return RX_GEN_ERR_TORN;
    rc = root_reachable(dir, &view);
    if (rc != RX_GEN_OK) return rc;

    int receipt = receipt_exists(dir, id);
    if (view.parent != 0 && journal_rc == RX_GEN_OK && candidate == id && !receipt) {
        RxGenStore scratch;
        memset(&scratch, 0, sizeof scratch);
        scratch.lock_fd = -1;
        scratch.crash_step = RX_CRASH_NONE;
        snprintf(scratch.dir, sizeof scratch.dir, "%s", dir);
        rc = write_receipt_file(&scratch, &view, 0, 0, 0);
        if (rc != RX_GEN_OK) return rc;
        rc = append_event(&scratch, view.id, view.lineage);
        if (rc != RX_GEN_OK) return rc;
        receipt = 1;
    }
    fill_record(&view, receipt, event_has(dir, id) || view.parent == 0, out);
    if (view.parent != 0 && !out->event_present) {
        RxGenStore scratch;
        memset(&scratch, 0, sizeof scratch);
        scratch.lock_fd = -1;
        snprintf(scratch.dir, sizeof scratch.dir, "%s", dir);
        if (append_event(&scratch, view.id, view.lineage) != RX_GEN_OK) return RX_GEN_ERR_IO;
        out->event_present = 1;
    }
    return RX_GEN_OK;
}

int rx_gen_reject_replay(const char *dir, uint64_t effect_id) {
    RxRecoveryRecord rec;
    int rc = rx_gen_recover(dir, &rec);
    if (rc != RX_GEN_OK) return rc;
    for (uint32_t i = 0; i < rec.n_external; i++)
        if (rec.external_ids[i] == effect_id) return RX_GEN_ERR_REPLAY;
    return RX_GEN_OK;
}

/* ---- durable executor --------------------------------------------------- */

typedef struct {
    int state;
    uint64_t seq;               /* post order; the lower pending seq runs first */
    uint64_t key;
    RxGenDoneFn done;
    void *done_ctx;
    RxGenJobResult result;
    /* proposal: an owned copy of the draft */
    uint32_t proposer;
    RxCallerCred proposer_cred;   /* R16 C5; wiped when the job has run */
    RxGenDraft draft;
    RxGenObject *objects;
    uint8_t *bytes;
    /* promotion */
    RxPromotionRequest request;
    RxGenAuthFn auth;
    void *auth_ctx;
} Job;

struct RxGenExec {
    RxGenStore *store;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int stop;
    uint64_t seq;
    Job job[RX_GEN_JOB_KINDS];
};

static void job_free(Job *j) {
    free(j->objects);
    free(j->bytes);
    j->objects = NULL;
    j->bytes = NULL;
}

static void *exec_main(void *arg) {
    struct RxGenExec *x = arg;
    pthread_mutex_lock(&x->mu);
    for (;;) {
        Job *next = NULL;
        for (int k = 0; k < RX_GEN_JOB_KINDS; k++)
            if (x->job[k].state == RX_GEN_JOB_PENDING && (!next || x->job[k].seq < next->seq))
                next = &x->job[k];
        if (!next) {
            if (x->stop) break;
            pthread_cond_wait(&x->cv, &x->mu);
            continue;
        }
        pthread_mutex_unlock(&x->mu);
        RxGenJobResult r;
        memset(&r, 0, sizeof r);
        r.key = next->key;
        uint64_t t0 = monotonic_ns();
        if (next == &x->job[RX_GEN_JOB_PROPOSE])
            r.rc = rx_gen_propose_as(x->store, next->proposer, &next->proposer_cred,
                                     &next->draft, &r.id);
        else
            r.rc = rx_gen_promote(x->store, &next->request, next->auth, next->auth_ctx,
                                  NULL, NULL, NULL, NULL);
        r.ns = monotonic_ns() - t0;
        rx_gen_active(x->store, &r.active, &r.lineage);
        rx_caller_wipe(&next->proposer_cred);
        rx_caller_wipe(&next->request.caller);
        job_free(next);
        pthread_mutex_lock(&x->mu);
        next->result = r;
        next->state = RX_GEN_JOB_DONE;
        RxGenDoneFn done = next->done;
        void *ctx = next->done_ctx;
        pthread_mutex_unlock(&x->mu);
        if (done) done(ctx);
        pthread_mutex_lock(&x->mu);
    }
    pthread_mutex_unlock(&x->mu);
    return NULL;
}

int rx_gen_exec_start(RxGenStore *store) {
    if (!store) return RX_GEN_ERR_ARG;
    if (store->exec) return RX_GEN_OK;
    struct RxGenExec *x = calloc(1, sizeof *x);
    if (!x) return RX_GEN_ERR_IO;
    x->store = store;
    pthread_mutex_init(&x->mu, NULL);
    pthread_cond_init(&x->cv, NULL);
    if (pthread_create(&x->thread, NULL, exec_main, x) != 0) {
        pthread_cond_destroy(&x->cv);
        pthread_mutex_destroy(&x->mu);
        free(x);
        return RX_GEN_ERR_IO;
    }
    store->exec = x;
    return RX_GEN_OK;
}

int rx_gen_exec_running(const RxGenStore *store) {
    return store && store->exec != NULL;
}

/* Runs every job already posted, then stops. A posted job is a promise the
 * caller is waiting on; it is never dropped. */
static void exec_stop(RxGenStore *store) {
    struct RxGenExec *x = store->exec;
    if (!x) return;
    pthread_mutex_lock(&x->mu);
    x->stop = 1;
    pthread_cond_signal(&x->cv);
    pthread_mutex_unlock(&x->mu);
    pthread_join(x->thread, NULL);
    for (int k = 0; k < RX_GEN_JOB_KINDS; k++) job_free(&x->job[k]);
    pthread_cond_destroy(&x->cv);
    pthread_mutex_destroy(&x->mu);
    free(x);
    store->exec = NULL;
}

static Job *claim_slot(struct RxGenExec *x, int kind) {
    Job *j = &x->job[kind];
    if (x->stop || j->state != RX_GEN_JOB_IDLE) return NULL;
    return j;
}

static void post_locked(struct RxGenExec *x, Job *j, uint64_t key, RxGenDoneFn done,
                        void *done_ctx) {
    j->key = key;
    j->done = done;
    j->done_ctx = done_ctx;
    j->seq = ++x->seq;
    memset(&j->result, 0, sizeof j->result);
    j->state = RX_GEN_JOB_PENDING;
    pthread_cond_signal(&x->cv);
}

int rx_gen_post_propose(RxGenStore *store, uint64_t key, uint32_t proposer,
                        const RxGenDraft *draft, RxGenDoneFn done, void *done_ctx) {
    return rx_gen_post_propose_as(store, key, proposer, NULL, draft, done, done_ctx);
}

int rx_gen_post_propose_as(RxGenStore *store, uint64_t key, uint32_t proposer,
                           const RxCallerCred *cred, const RxGenDraft *draft,
                           RxGenDoneFn done, void *done_ctx) {
    if (!store || !store->exec || !draft) return RX_GEN_ERR_ARG;
    if (draft->n_objects > RX_GEN_MAX_OBJECTS || (draft->n_objects && !draft->objects))
        return RX_GEN_ERR_ARG;
    const uint8_t *src[5] = {draft->evidence, draft->model, draft->realization,
                             draft->config, draft->provenance};
    size_t len[5] = {draft->evidence_len, draft->model_len, draft->realization_len,
                     draft->config_len, draft->provenance_len};
    size_t total = 0;
    for (int i = 0; i < 5; i++) {
        if (len[i] && !src[i]) return RX_GEN_ERR_ARG;
        total += len[i];
    }
    RxGenObject *objs = NULL;
    uint8_t *bytes = NULL;
    if (draft->n_objects) {
        objs = malloc(draft->n_objects * sizeof *objs);
        if (!objs) return RX_GEN_ERR_IO;
        memcpy(objs, draft->objects, draft->n_objects * sizeof *objs);
    }
    if (total) {
        bytes = malloc(total);
        if (!bytes) {
            free(objs);
            return RX_GEN_ERR_IO;
        }
    }
    RxGenDraft copy = *draft;
    copy.objects = objs;
    const uint8_t **dst[5] = {&copy.evidence, &copy.model, &copy.realization, &copy.config,
                              &copy.provenance};
    size_t at = 0;
    for (int i = 0; i < 5; i++) {
        *dst[i] = len[i] ? bytes + at : NULL;
        if (len[i]) memcpy(bytes + at, src[i], len[i]);
        at += len[i];
    }
    struct RxGenExec *x = store->exec;
    pthread_mutex_lock(&x->mu);
    Job *j = claim_slot(x, RX_GEN_JOB_PROPOSE);
    if (!j) {
        pthread_mutex_unlock(&x->mu);
        free(objs);
        free(bytes);
        return RX_GEN_ERR_BUSY;
    }
    j->proposer = proposer;
    if (cred) j->proposer_cred = *cred;
    else memset(&j->proposer_cred, 0, sizeof j->proposer_cred);
    j->draft = copy;
    j->objects = objs;
    j->bytes = bytes;
    post_locked(x, j, key, done, done_ctx);
    pthread_mutex_unlock(&x->mu);
    return RX_GEN_OK;
}

int rx_gen_post_promote(RxGenStore *store, uint64_t key, const RxPromotionRequest *request,
                        RxGenAuthFn auth, void *auth_ctx, RxGenDoneFn done, void *done_ctx) {
    if (!store || !store->exec || !request || (!auth && !store_bound(store))) return RX_GEN_ERR_ARG;
    struct RxGenExec *x = store->exec;
    pthread_mutex_lock(&x->mu);
    Job *j = claim_slot(x, RX_GEN_JOB_PROMOTE);
    if (!j) {
        pthread_mutex_unlock(&x->mu);
        return RX_GEN_ERR_BUSY;
    }
    j->request = *request;
    j->auth = auth;
    j->auth_ctx = auth_ctx;
    post_locked(x, j, key, done, done_ctx);
    pthread_mutex_unlock(&x->mu);
    return RX_GEN_OK;
}

int rx_gen_job_state(RxGenStore *store, int kind, RxGenJobResult *out) {
    if (!store || !store->exec || kind < 0 || kind >= RX_GEN_JOB_KINDS) return RX_GEN_ERR_ARG;
    struct RxGenExec *x = store->exec;
    pthread_mutex_lock(&x->mu);
    int st = x->job[kind].state;
    if (st == RX_GEN_JOB_DONE && out) *out = x->job[kind].result;
    pthread_mutex_unlock(&x->mu);
    return st;
}

void rx_gen_job_take(RxGenStore *store, int kind) {
    if (!store || !store->exec || kind < 0 || kind >= RX_GEN_JOB_KINDS) return;
    struct RxGenExec *x = store->exec;
    pthread_mutex_lock(&x->mu);
    if (x->job[kind].state == RX_GEN_JOB_DONE) x->job[kind].state = RX_GEN_JOB_IDLE;
    pthread_mutex_unlock(&x->mu);
}
