/* m22_dispatch_run.c -- make an M22 store with a real dispatch.log through
 * the public tg_store/tg_sgd API (host only), so tools/replay/rx_replay can
 * verify it and compare two runs.
 *
 *   m22_dispatch_run DIR [steps] [seed] [perturb]
 *
 * Deterministic: quadratic loss, seeded parameters, momentum SGD, every
 * third step applies two dispatches in one generation. perturb "lr" uses a
 * different learning rate from step 4 on (the negative control for
 * compare-dispatch). DIR must not exist or be empty. */
#include "train/tg_sgd.h"
#include "train/tg_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N 64

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static float urand(uint32_t *s) { return (float)(lcg(s) >> 8) / 16777216.0f - 0.5f; }
static int accept(const tg_shadow *sh, const tg_snapshot *c, void *ctx) { (void)sh; (void)c; (void)ctx; return 0; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: m22_dispatch_run DIR [steps] [seed] [lr]\n"); return 2; }
    const char *dir = argv[1];
    int steps = argc > 2 ? atoi(argv[2]) : 12;
    uint32_t seed = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 12345u;
    int perturb = argc > 4 && !strcmp(argv[4], "lr");
    mkdir(dir, 0755);
    float p[N], v[N], t[N], g[N];
    for (int i = 0; i < N; i++) { p[i] = 4.0f * urand(&seed); v[i] = 0; t[i] = urand(&seed); }
    int r = tg_create(dir, p, sizeof p, v, sizeof v);
    if (r) { fprintf(stderr, "tg_create: %s\n", tg_err_name(r)); return 2; }
    tg_store *st;
    if ((r = tg_open(dir, &st, NULL))) { fprintf(stderr, "tg_open: %s\n", tg_err_name(r)); return 2; }
    for (int s = 0; s < steps; s++) {
        tg_shadow sh;
        if ((r = tg_shadow_begin(st, &sh))) break;
        int reps = s % 3 == 2 ? 2 : 1;
        for (int k = 0; k < reps && !r; k++) {
            const float *cur = (const float *)(const void *)sh.params;
            for (int i = 0; i < N; i++) g[i] = cur[i] - t[i];
            float lr = perturb && s >= 4 ? 0.06f : 0.05f;
            r = tg_sgd_step(st, &sh, g, N, lr, 0.9f, (uint64_t)(s * 4 + k));
        }
        if (r) { tg_shadow_discard(&sh); break; }
        if ((r = tg_commit(st, &sh, accept, NULL))) break;
    }
    if (r) { fprintf(stderr, "step failed: %s\n", tg_err_name(r)); tg_close(st); return 2; }
    const tg_snapshot *c = tg_committed(st);
    char hx[2 * TG_DIGEST + 1];
    tg_hex(c->dispatch_head, hx);
    printf("m22 store %s: gen %llu, %llu dispatch records, head %s\n", dir, (unsigned long long)c->gen,
           (unsigned long long)(c->dispatch_len / 192u), hx);
    tg_close(st);
    return 0;
}
