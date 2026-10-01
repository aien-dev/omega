/* rxlog_mutate.c -- make one named, realistic fault in a recorded log, for
 * the replay mutation suite (tests/replay/run_replay_suite.sh).
 *
 *   rxlog_mutate world IN OUT NAME      World RXCLOG01 log
 *   rxlog_mutate dispatch DIR NAME      M22 store dir, edited in place
 *   rxlog_mutate list-world | list-dispatch
 *
 * World names starting "forged-" are re-sealed afterwards: every crumb
 * digest, episode and the END record are recomputed, so the log passes its
 * own checks and only a comparison with the replayed run can catch it.
 * Names starting "excluded-" touch fields that are deliberately outside the
 * compared digest (worker, clocks); the suite expects MATCH for them.
 * Dispatch "forged-" names recompute the chain; "forged-store-" also
 * rewrites the committed generation header, its digest and CURRENT. */
#include "replay/rxlog.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *WORLD[] = {
    "omit-crumb", "omit-tail", "reorder-crumbs", "reorder-input", "omit-input",
    "flip-kind", "flip-reaction", "flip-faculty", "flip-wake-cause", "flip-coalesced",
    "flip-input-obj", "flip-input-version", "flip-input-mask", "flip-cap-id", "flip-cap-gen",
    "flip-cap-issuer", "flip-output-obj", "flip-output-version", "flip-output-mask", "flip-reason",
    "flip-parent", "reorder-parents", "drop-parent", "flip-digest", "flip-episode",
    "flip-input-value", "flip-checkpoint", "flip-end-head", "flip-end-count",
    "forged-output-version", "forged-input-value", "forged-omit-stimulus", "forged-reaction",
    "forged-wake-cause",
    "excluded-worker", "excluded-t-start", "excluded-t-end", NULL };

static const char *DISPATCH[] = {
    "omit-record", "omit-last", "reorder-records", "flip-op", "flip-step", "flip-arg0", "flip-arg1",
    "flip-ref-len", "flip-ref-buf", "flip-base-digest", "flip-chain", "flip-seq", "flip-padding",
    "truncate-mid", "forged-arg0", "forged-store-arg0", NULL };

/* Index of the target crumb: the first COMMIT (kind 3) after the second
 * INPUT with inputs, caps, outputs and at least two parents; else the first
 * COMMIT with the most parents. */
static long target_crumb(const rxl_log *l) {
    int inputs = 0;
    long best = -1;
    uint32_t bestp = 0;
    for (size_t i = 0; i < l->n; i++) {
        const rxl_rec *r = &l->recs[i];
        if (r->type == RXL_INPUT) inputs++;
        if (r->type != RXL_CRUMB || r->u.c.kind != 3) continue;
        const rxl_crumb *k = &r->u.c;
        if (inputs >= 2 && k->n_inputs && k->n_caps && k->n_outputs && k->n_parents >= 2) return (long)i;
        if (k->n_parents > bestp) { bestp = k->n_parents; best = (long)i; }
    }
    return best;
}

static long nth_type(const rxl_log *l, uint32_t type, int nth) {
    for (size_t i = 0; i < l->n; i++)
        if (l->recs[i].type == type && nth-- == 0) return (long)i;
    return -1;
}

static void remove_rec(rxl_log *l, size_t i) {
    memmove(&l->recs[i], &l->recs[i + 1], (l->n - i - 1) * sizeof l->recs[0]);
    l->n--;
}

static void swap_rec(rxl_log *l, size_t i, size_t j) {
    rxl_rec t = l->recs[i];
    l->recs[i] = l->recs[j];
    l->recs[j] = t;
}

static int world(const char *in, const char *out, const char *name) {
    rxl_log l;
    char err[160];
    if (rxl_read(in, &l, err, sizeof err) || l.malformed) { fprintf(stderr, "mutate: %s\n", l.malformed ? l.why : err); return 2; }
    long t = target_crumb(&l);
    if (t < 0) { fprintf(stderr, "mutate: no target crumb\n"); return 2; }
    rxl_crumb *k = &l.recs[t].u.c;
    long in2 = nth_type(&l, RXL_INPUT, 2);
    int forged = !strncmp(name, "forged-", 7);
    int ok = 1;
    if (!strcmp(name, "omit-crumb")) remove_rec(&l, (size_t)t);
    else if (!strcmp(name, "omit-tail")) { l.n -= 2; }  /* last record before END and END itself */
    else if (!strcmp(name, "reorder-crumbs")) swap_rec(&l, (size_t)t, (size_t)t + 1);
    else if (!strcmp(name, "reorder-input")) swap_rec(&l, (size_t)in2, (size_t)in2 + 1);
    else if (!strcmp(name, "omit-input")) remove_rec(&l, (size_t)in2);
    else if (!strcmp(name, "flip-kind")) k->kind ^= 1;
    else if (!strcmp(name, "flip-reaction")) k->reaction ^= 1;
    else if (!strcmp(name, "flip-faculty")) k->faculty ^= 1;
    else if (!strcmp(name, "flip-wake-cause")) k->wake_cause ^= 1;
    else if (!strcmp(name, "flip-coalesced")) k->coalesced ^= 1;
    else if (!strcmp(name, "flip-input-obj")) k->inputs[0].id ^= 1;
    else if (!strcmp(name, "flip-input-version")) k->inputs[0].version ^= 1;
    else if (!strcmp(name, "flip-input-mask")) k->inputs[0].mask ^= 2;
    else if (!strcmp(name, "flip-cap-id")) k->caps[0].cap_id ^= 1;
    else if (!strcmp(name, "flip-cap-gen")) k->caps[0].gen ^= 1ull << 40;   /* high word of the 64-bit generation */
    else if (!strcmp(name, "flip-cap-issuer")) k->caps[0].issuer ^= 1;
    else if (!strcmp(name, "flip-output-obj")) k->outputs[0].id ^= 1;
    else if (!strcmp(name, "flip-output-version")) k->outputs[0].version ^= 1;
    else if (!strcmp(name, "flip-output-mask")) k->outputs[0].mask ^= 2;
    else if (!strcmp(name, "flip-reason")) k->reason ^= 1;
    else if (!strcmp(name, "flip-parent")) k->parents[0] ^= 1;
    else if (!strcmp(name, "reorder-parents")) { uint64_t x = k->parents[0]; k->parents[0] = k->parents[1]; k->parents[1] = x; }
    else if (!strcmp(name, "drop-parent")) k->n_parents--;
    else if (!strcmp(name, "flip-digest")) k->digest[7] ^= 0x10;
    else if (!strcmp(name, "flip-episode")) k->episode ^= 1;
    else if (!strcmp(name, "flip-input-value")) l.recs[in2].u.in.m[0].value ^= 1;
    else if (!strcmp(name, "flip-checkpoint")) l.recs[nth_type(&l, RXL_CHECKPOINT, 3)].u.ck.hash[0] ^= 1;
    else if (!strcmp(name, "flip-end-head")) l.recs[l.n - 1].u.end.head[31] ^= 1;
    else if (!strcmp(name, "flip-end-count")) l.recs[l.n - 1].u.end.n_records -= 1;
    else if (!strcmp(name, "forged-output-version")) k->outputs[0].version += 1;
    else if (!strcmp(name, "forged-input-value")) l.recs[in2].u.in.m[0].value += 1;
    else if (!strcmp(name, "forged-omit-stimulus")) {
        /* Drop the last stimulus entirely (INPUT, its crumbs, CHECKPOINT): a
         * shorter but self-consistent history. */
        long last = -1;
        for (size_t i = 0; i < l.n; i++) if (l.recs[i].type == RXL_INPUT) last = (long)i;
        l.n = (size_t)last;
    }
    else if (!strcmp(name, "forged-reaction")) k->reaction ^= 1;
    else if (!strcmp(name, "forged-wake-cause"))
        k->wake_cause = k->parents[0] != k->wake_cause ? k->parents[0] : k->parents[1];
    else if (!strcmp(name, "excluded-worker")) k->worker ^= 3;
    else if (!strcmp(name, "excluded-t-start")) k->t_start += 12345;
    else if (!strcmp(name, "excluded-t-end")) k->t_end += 999;
    else ok = 0;
    if (!ok) { fprintf(stderr, "mutate: unknown world mutant %s\n", name); rxl_free(&l); return 2; }
    if (forged && rxl_seal(&l)) { rxl_free(&l); return 2; }
    int rc = rxl_write(out, &l) ? 2 : 0;
    rxl_free(&l);
    return rc;
}

/* ---- dispatch ----------------------------------------------------------- */


static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}
static int spit(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fwrite(b, 1, n, f) != n;
    return fclose(f) || bad ? -1 : 0;
}

static void rechain(uint8_t *log, size_t n, uint8_t head[32]) {
    memset(head, 0, 32);
    for (size_t i = 0; i + 192 <= n; i += 192) {
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, head, 32);
        sha256_update(&c, log + i, 160);
        sha256_final(&c, log + i + 160);
        memcpy(head, log + i + 160, 32);
    }
}

/* Rewrite the committed generation so it names `head`: header +56, digest
 * at +120 over [0,120) + body, CURRENT's hex digest. */
static int reforge_store(const char *dir, const uint8_t head[32]) {
    char p[4096], gp[4096];
    size_t n;
    snprintf(p, sizeof p, "%s/CURRENT", dir);
    uint8_t *cur = slurp(p, &n);
    if (!cur || n != 95) { free(cur); return -1; }
    char gen[21];
    memcpy(gen, cur + 9, 20); gen[20] = 0;
    snprintf(gp, sizeof gp, "%s/gen-%s.bin", dir, gen);
    size_t gn;
    uint8_t *g = slurp(gp, &gn);
    if (!g || gn < 152) { free(cur); free(g); return -1; }
    memcpy(g + 56, head, 32);
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, g, 120);
    sha256_update(&c, g + 152, gn - 152);
    sha256_final(&c, g + 120);
    rxl_hex(g + 120, 32, (char *)cur + 30);
    cur[94] = '\n';
    int rc = spit(gp, g, gn) || spit(p, cur, n) ? -1 : 0;
    free(cur); free(g);
    return rc;
}

static int dispatch(const char *dir, const char *name) {
    char p[4096];
    snprintf(p, sizeof p, "%s/dispatch.log", dir);
    size_t n;
    uint8_t *b = slurp(p, &n);
    if (!b || n < 192 * 4) { fprintf(stderr, "mutate: dispatch.log missing or under 4 records\n"); free(b); return 2; }
    size_t nr = n / 192, t = nr / 2;   /* target: a middle record */
    uint8_t *r = b + t * 192;
    uint8_t head[32];
    int ok = 1;
    if (!strcmp(name, "omit-record")) { memmove(r, r + 192, n - (t + 1) * 192); n -= 192; }
    else if (!strcmp(name, "omit-last")) n -= 192;
    else if (!strcmp(name, "reorder-records")) { uint8_t x[192]; memcpy(x, r, 192); memcpy(r, r + 192, 192); memcpy(r + 192, x, 192); }
    else if (!strcmp(name, "flip-op")) r[4] ^= 1;
    else if (!strcmp(name, "flip-step")) r[16] ^= 1;
    else if (!strcmp(name, "flip-arg0")) r[32] ^= 1;
    else if (!strcmp(name, "flip-arg1")) r[40] ^= 1;
    else if (!strcmp(name, "flip-ref-len")) r[56 + 16] ^= 1;
    else if (!strcmp(name, "flip-ref-buf")) r[56] ^= 1;
    else if (!strcmp(name, "flip-base-digest")) r[128] ^= 1;
    else if (!strcmp(name, "flip-chain")) r[160] ^= 1;
    else if (!strcmp(name, "flip-seq")) r[8] ^= 1;
    else if (!strcmp(name, "flip-padding")) r[52] ^= 1;
    else if (!strcmp(name, "truncate-mid")) n -= 100;
    else if (!strcmp(name, "forged-arg0")) { r[32] ^= 1; rechain(b, n, head); }
    else if (!strcmp(name, "forged-store-arg0")) {
        r[32] ^= 1;
        rechain(b, n, head);
        if (reforge_store(dir, head)) { fprintf(stderr, "mutate: cannot reforge store\n"); free(b); return 2; }
    }
    else ok = 0;
    if (!ok) { fprintf(stderr, "mutate: unknown dispatch mutant %s\n", name); free(b); return 2; }
    int rc = spit(p, b, n) ? 2 : 0;
    free(b);
    return rc;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "list-world")) { for (int i = 0; WORLD[i]; i++) puts(WORLD[i]); return 0; }
    if (argc == 2 && !strcmp(argv[1], "list-dispatch")) { for (int i = 0; DISPATCH[i]; i++) puts(DISPATCH[i]); return 0; }
    if (argc == 5 && !strcmp(argv[1], "world")) return world(argv[2], argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[1], "dispatch")) return dispatch(argv[2], argv[3]);
    fprintf(stderr, "usage: rxlog_mutate world IN OUT NAME | dispatch DIR NAME | list-world | list-dispatch\n");
    return 2;
}
