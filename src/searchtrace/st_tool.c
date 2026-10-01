/* searchtrace: M23 G1 search-trace corpus tool and G3 holdout commitments.
 *   searchtrace run <taskset> <out_corpus>     generate, self-verify, write (refuses to replace different bytes)
 *   searchtrace verify <corpus> [<taskset>]    strict format check (+ task-set binding)
 *   searchtrace replay <taskset> <corpus>      regenerate and compare byte for byte
 *   searchtrace hookcheck <taskset>            hook installed vs not: identical search results
 *   searchtrace holdout <commit|reveal|verify> ...   G3 sealed-holdout commitment format
 * Exit: 0 ok, 2 refused/failed, 1 usage. */
#include "searchtrace/st_corpus.h"
#include "searchtrace/st_holdout.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE (256u << 20)

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            if (cap > MAX_FILE + 1) { free(buf); fclose(f); return NULL; }
            char *p = realloc(buf, cap + 1);
            if (!p) { free(buf); fclose(f); return NULL; }
            buf = p;
        }
        size_t k = fread(buf + n, 1, cap - n, f);
        n += k;
        if (k == 0) break;
    }
    int err = ferror(f);
    fclose(f);
    if (err || n > MAX_FILE) { free(buf); return NULL; }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int load_taskset(const char *path, StTaskSet *ts) {
    size_t n;
    char why[192];
    char *d = read_file(path, &n);
    if (!d) { fprintf(stderr, "searchtrace: cannot read task set %s\n", path); return -1; }
    int rc = st_taskset_parse(d, n, ts, why, sizeof why);
    free(d);
    if (rc != 0) fprintf(stderr, "searchtrace: REFUSED task set %s: %s\n", path, why);
    return rc;
}

static int write_exact(const char *path, const char *data, size_t n) {
    size_t on;
    char *old = read_file(path, &on);
    if (old) {
        int same = on == n && memcmp(old, data, n) == 0;
        free(old);
        if (!same) { fprintf(stderr, "searchtrace: REFUSED: %s exists with different bytes\n", path); return -1; }
        return 0;
    }
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "searchtrace: cannot write %s\n", tmp); return -1; }
    int ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); fprintf(stderr, "searchtrace: write failed %s\n", path); return -1; }
    return 0;
}

static int cmd_run(const char *tsp, const char *outp, int replay) {
    StTaskSet *ts = malloc(sizeof *ts);
    if (!ts || load_taskset(tsp, ts) != 0) { free(ts); return 2; }
    StBuf b = {0};
    char why[192];
    uint8_t dg[32];
    uint64_t steps = 0;
    int ret = 2;
    if (st_corpus_generate(ts, &b, why, sizeof why) != 0) { fprintf(stderr, "searchtrace: REFUSED generate: %s\n", why); goto out; }
    if (st_corpus_verify(b.p, b.n, ts, dg, &steps, why, sizeof why) != 0) { fprintf(stderr, "searchtrace: FAIL self-verify: %s\n", why); goto out; }
    char hx[65];
    st_hex(dg, 32, hx);
    if (replay) {
        size_t n;
        char *d = read_file(outp, &n);
        if (!d) { fprintf(stderr, "searchtrace: cannot read corpus %s\n", outp); goto out; }
        int same = n == b.n && memcmp(d, b.p, n) == 0;
        free(d);
        printf("replay %s digest=%s steps=%" PRIu64 " bytes=%zu\n", same ? "PASS" : "FAIL", hx, steps, b.n);
        ret = same ? 0 : 2;
    } else {
        if (write_exact(outp, b.p, b.n) != 0) goto out;
        printf("corpus digest=%s steps=%" PRIu64 " tasks=%u bytes=%zu path=%s\n", hx, steps, ts->n_tasks, b.n, outp);
        ret = 0;
    }
out:
    st_buf_free(&b);
    free(ts);
    return ret;
}

static int cmd_verify(const char *cp, const char *tsp) {
    StTaskSet *ts = NULL;
    if (tsp) {
        ts = malloc(sizeof *ts);
        if (!ts || load_taskset(tsp, ts) != 0) { free(ts); return 2; }
    }
    size_t n;
    char *d = read_file(cp, &n);
    if (!d) { fprintf(stderr, "searchtrace: cannot read corpus %s\n", cp); free(ts); return 2; }
    char why[192], hx[65];
    uint8_t dg[32];
    uint64_t steps = 0;
    int rc = st_corpus_verify(d, n, ts, dg, &steps, why, sizeof why);
    free(d);
    free(ts);
    if (rc != 0) { printf("verify REFUSED: %s\n", why); return 2; }
    st_hex(dg, 32, hx);
    printf("verify PASS digest=%s steps=%" PRIu64 "\n", hx, steps);
    return 0;
}

static int cmd_hookcheck(const char *tsp) {
    StTaskSet *ts = malloc(sizeof *ts);
    if (!ts || load_taskset(tsp, ts) != 0) { free(ts); return 2; }
    char why[192];
    int rc = st_hook_equivalence(ts, why, sizeof why);
    printf("hookcheck %s tasks=%u%s%s\n", rc == 0 ? "PASS" : "FAIL", ts->n_tasks, rc ? ": " : "", rc ? why : "");
    free(ts);
    return rc == 0 ? 0 : 2;
}

static int usage(void) {
    fprintf(stderr, "usage: searchtrace run <taskset> <out> | verify <corpus> [<taskset>] | replay <taskset> <corpus>\n"
                    "       searchtrace hookcheck <taskset> | holdout <commit|reveal|verify> ...\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "run") && argc == 4) return cmd_run(argv[2], argv[3], 0);
    if (!strcmp(c, "replay") && argc == 4) return cmd_run(argv[2], argv[3], 1);
    if (!strcmp(c, "verify") && (argc == 3 || argc == 4)) return cmd_verify(argv[2], argc == 4 ? argv[3] : NULL);
    if (!strcmp(c, "hookcheck") && argc == 3) return cmd_hookcheck(argv[2]);
    if (!strcmp(c, "holdout") && argc >= 3) return st_holdout_cli(argc - 2, argv + 2);
    return usage();
}
