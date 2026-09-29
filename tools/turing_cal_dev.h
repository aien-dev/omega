/* EXP-001 lane A: development-data loader shared by tools/turing_cal_candidates.c
 * and calibration/scripts/power_simulation.c.
 *
 * Reads the TY-2 trace manifest (evidence/TURING_YIELD/trace_manifest_rep10_control.sha256)
 * but only ever opens entries 1-7 (dev seeds 1-7 of exp-20260927-rep10 control).
 * Entries 8-10 are burned held-out seeds: their lines are parsed for count only
 * and their paths are never opened. Every dev file is re-hashed (SHA-256) and
 * compared with the manifest before it is read.
 */
#ifndef TURING_CAL_DEV_H
#define TURING_CAL_DEV_H

#include "turing/ty_record.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TCD_NSEED 10
#define TCD_NDEV 7

typedef struct {
    char hex[65];
    uint64_t size;
    char rel[256];
    char path[1024];
    ty_digest d;
} tcd_entry;

static tcd_entry TCD_M[TCD_NSEED];
static ty_stream TCD_S[TCD_NDEV];
static int TCD_loaded[TCD_NDEV];

static inline int tcd_load_manifest(const char *mpath, const char *root) {
    FILE *f = fopen(mpath, "r");
    if (!f) return TY_E_IO;
    char line[1400];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < TCD_NSEED) {
        if (line[0] == '#' || line[0] == '\n') continue;
        tcd_entry *e = &TCD_M[n];
        if (sscanf(line, "%64s %" SCNu64 " %255s", e->hex, &e->size, e->rel) != 3 || ty_parse_hex(e->hex, &e->d) != TY_OK) {
            fclose(f);
            return TY_E_FORMAT;
        }
        char want[32];
        snprintf(want, sizeof want, "/seed-%d/control/", n + 1);
        if (!strstr(e->rel, want)) {
            fclose(f);
            return TY_E_FORMAT; /* entries must be seeds 1..10 in order */
        }
        size_t lr = strlen(root), ll = strlen(e->rel);
        if (lr + 1 + ll >= sizeof e->path) {
            fclose(f);
            return TY_E_FORMAT;
        }
        memcpy(e->path, root, lr);
        e->path[lr] = '/';
        memcpy(e->path + lr + 1, e->rel, ll + 1);
        ++n;
    }
    fclose(f);
    return n == TCD_NSEED ? TY_OK : TY_E_FORMAT;
}

/* seed is 1..7. Re-hash, then read. Any other seed number is refused. */
static inline int tcd_load_seed(int seed) {
    if (seed < 1 || seed > TCD_NDEV) return TY_E_LEAK;
    int i = seed - 1;
    if (TCD_loaded[i]) return TY_OK;
    struct stat st;
    if (stat(TCD_M[i].path, &st) != 0 || (uint64_t)st.st_size != TCD_M[i].size) return TY_E_IO;
    ty_digest d;
    int rc = ty_file_sha256(TCD_M[i].path, &d);
    if (rc != TY_OK) return rc;
    if (!ty_digest_eq(&d, &TCD_M[i].d)) return TY_E_DIGEST;
    char why[256] = "";
    ty_stream_init(&TCD_S[i]);
    int64_t n = ty_ctr1_read(TCD_M[i].path, &TCD_S[i], why, sizeof why);
    if (n < 0) {
        fprintf(stderr, "seed %d: %s\n", seed, why);
        return (int)n;
    }
    fprintf(stderr, "dev seed %d: %lld events, %u crumbs, sha256 ok\n", seed, (long long)n, TCD_S[i].ncrumb);
    TCD_loaded[i] = 1;
    return TY_OK;
}

static inline void tcd_hex(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

#endif /* TURING_CAL_DEV_H */
