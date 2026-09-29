/* Turing calibration (CAL-0 / EXP-001): development vs sealed CTR1 overlap audit.
 *
 * usage: turing-cal-overlap DEV_LIST SEALED_LIST
 *   DEV_LIST / SEALED_LIST: text files, one CTR1 trace.ctr path per line.
 *
 * Every record is checked for the CTR1 framing (magic "CTR1", version 1, and
 * event_index rising inside a crumb; a crumb starts at event_index 0), and a
 * malformed file fails closed (exit 2). The full value-set check of
 * ty_ctr1_decode is NOT applied: development data includes learning-condition
 * traces (library ops, op_origin 1) that the TY-2 control reader refuses, and
 * the overlap audit must still compare against them. Three comparisons, all over record
 * bytes 0..214 (the record body; bytes 215..246 are the BLAKE3 chain digest,
 * which depends on file position and is excluded):
 *
 *   crumb_block_overlap  crumbs (a record run starting at event_index 0) whose
 *                        SHA-256 over the concatenated bodies occurs on both
 *                        sides. GATE: must be 0.
 *   first_state_overlap  crumbs whose first record's state_digest (offset 63)
 *                        occurs on both sides. INFORMATIONAL: distinct crumbs of
 *                        small families can start from the same state (a trial
 *                        seed vs dev seeds 1-7 showed 1 such crumb with a
 *                        different block digest).
 *   record_body_overlap  sealed records whose body fingerprint (first 8 bytes of
 *                        SHA-256 of the body) also occurs in development data.
 *                        INFORMATIONAL: search events on different crumbs can be
 *                        byte-identical; reported with its count only.
 *
 * Output: one JSON object on stdout. Exit 0 = gates pass, 1 = a gate failed,
 * 2 = usage / I/O / format error.
 */
#include "sha256.h"
#include "turing/ty_ctr1.h" /* TY_CTR1_BYTES only */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BODY 215
#define CHUNK_RECS 4096

typedef struct {
    uint8_t (*d)[32];
    size_t n, cap;
} dvec;

typedef struct {
    uint64_t *v;
    size_t n, cap;
} uvec;

typedef struct {
    uint64_t files, records, crumbs;
    dvec blocks, first_states;
    uvec fps;
} side;

static int dpush(dvec *a, const uint8_t d[32]) {
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 1024;
        void *p = realloc(a->d, nc * 32);
        if (!p) return -1;
        a->d = p;
        a->cap = nc;
    }
    memcpy(a->d[a->n++], d, 32);
    return 0;
}

static int upush(uvec *a, uint64_t x) {
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 1u << 20;
        void *p = realloc(a->v, nc * sizeof(uint64_t));
        if (!p) return -1;
        a->v = p;
        a->cap = nc;
    }
    a->v[a->n++] = x;
    return 0;
}

static int cmp32(const void *a, const void *b) { return memcmp(a, b, 32); }
static int cmpu(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int read_file(const char *path, side *s) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "turing-cal-overlap: cannot open %s\n", path);
        return -1;
    }
    static uint8_t buf[CHUNK_RECS * TY_CTR1_BYTES];
    int64_t prev = -1;
    int open_crumb = 0;
    sha256_ctx blk;
    size_t got;
    int rc = 0;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (got % TY_CTR1_BYTES) {
            /* short read inside a record: only legal at EOF, and then a format error */
            fprintf(stderr, "turing-cal-overlap: %s: size not a multiple of %d\n", path, TY_CTR1_BYTES);
            rc = -1;
            break;
        }
        for (size_t off = 0; off < got; off += TY_CTR1_BYTES) {
            const uint8_t *r = buf + off;
            uint32_t idx = (uint32_t)r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16 | (uint32_t)r[11] << 24;
            int first = idx == 0;
            if (memcmp(r, "CTR1", 4) != 0 || r[4] != 1 || r[5] != 0 || (!first && (int64_t)idx <= prev)) {
                fprintf(stderr, "turing-cal-overlap: %s record %llu: bad CTR1 framing\n", path,
                        (unsigned long long)s->records);
                rc = -1;
                goto done;
            }
            if (first) {
                if (open_crumb) {
                    uint8_t d[32];
                    sha256_final(&blk, d);
                    if (dpush(&s->blocks, d)) { rc = -1; goto done; }
                }
                sha256_init(&blk);
                open_crumb = 1;
                s->crumbs++;
                if (dpush(&s->first_states, r + 63)) { rc = -1; goto done; }
            } else if (!open_crumb) {
                fprintf(stderr, "turing-cal-overlap: %s: first record does not open a crumb\n", path);
                rc = -1;
                goto done;
            }
            sha256_update(&blk, r, BODY);
            {
                uint8_t h[32];
                sha256_hash(r, BODY, h);
                uint64_t fp = 0;
                for (int i = 0; i < 8; i++) fp = (fp << 8) | h[i];
                if (upush(&s->fps, fp)) { rc = -1; goto done; }
            }
            prev = idx;
            s->records++;
        }
    }
    if (ferror(f)) rc = -1;
    if (rc == 0 && open_crumb) {
        uint8_t d[32];
        sha256_final(&blk, d);
        if (dpush(&s->blocks, d)) rc = -1;
    }
done:
    fclose(f);
    s->files++;
    return rc;
}

static int read_list(const char *list, side *s) {
    FILE *f = fopen(list, "r");
    if (!f) {
        fprintf(stderr, "turing-cal-overlap: cannot open list %s\n", list);
        return -1;
    }
    char line[4096];
    int rc = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (read_file(line, s)) { rc = -1; break; }
    }
    fclose(f);
    if (rc == 0 && s->files == 0) {
        fprintf(stderr, "turing-cal-overlap: list %s names no files\n", list);
        rc = -1;
    }
    return rc;
}

static uint64_t count_shared32(dvec *a, dvec *b) {
    qsort(a->d, a->n, 32, cmp32);
    uint64_t k = 0;
    for (size_t i = 0; i < b->n; i++)
        if (bsearch(b->d[i], a->d, a->n, 32, cmp32)) k++;
    return k;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: turing-cal-overlap DEV_LIST SEALED_LIST\n");
        return 2;
    }
    side dev = {0}, sea = {0};
    if (read_list(argv[1], &dev) || read_list(argv[2], &sea)) return 2;
    uint64_t blk = count_shared32(&dev.blocks, &sea.blocks);
    uint64_t fst = count_shared32(&dev.first_states, &sea.first_states);
    qsort(dev.fps.v, dev.fps.n, sizeof(uint64_t), cmpu);
    uint64_t rec = 0;
    for (size_t i = 0; i < sea.fps.n; i++)
        if (bsearch(&sea.fps.v[i], dev.fps.v, dev.fps.n, sizeof(uint64_t), cmpu)) rec++;
    int pass = blk == 0;
    printf("{\"dev_files\":%llu,\"dev_records\":%llu,\"dev_crumbs\":%llu,"
           "\"sealed_files\":%llu,\"sealed_records\":%llu,\"sealed_crumbs\":%llu,"
           "\"crumb_block_overlap\":%llu,\"first_state_overlap\":%llu,"
           "\"record_body_overlap\":%llu,\"gated\":[\"crumb_block_overlap\"],"
           "\"ctr1_gates_pass\":%s}\n",
           (unsigned long long)dev.files, (unsigned long long)dev.records, (unsigned long long)dev.crumbs,
           (unsigned long long)sea.files, (unsigned long long)sea.records, (unsigned long long)sea.crumbs,
           (unsigned long long)blk, (unsigned long long)fst, (unsigned long long)rec, pass ? "true" : "false");
    free(dev.blocks.d); free(dev.first_states.d); free(dev.fps.v);
    free(sea.blocks.d); free(sea.first_states.d); free(sea.fps.v);
    return pass ? 0 : 1;
}
