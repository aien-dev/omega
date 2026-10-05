/* G0-style self-tests for the verifier's byte layouts: round trip, version refusal, chain detection. */
#include "pd0_fmt.h"
#include "pd0_codes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
int golden_check(const char *path);
int main(void)
{
    CHECK(golden_check(GOLDEN_PATH) == 0);
    uint8_t buf[4096], buf2[4096]; pd0_rec r, r2; memset(&r, 0, sizeof r); r.n_obs = 2; r.kind = 1; r.channel = 0; r.seq = 7; r.episode = 1; r.step_in_episode = 3; r.applied = r.requested = 123456; r.before[0] = -5; r.after[0] = 9; r.after[1] = 1000000;
    size_t n = pd0_rec_write(&r, buf, sizeof buf); CHECK(n == pd0_rec_size(2)); size_t used = 0;
    CHECK(pd0_rec_parse(buf, n, &r2, &used) == PD0V_OK && used == n && r2.after[1] == 1000000 && r2.seq == 7);
    CHECK(pd0_rec_parse(buf, n - 1, &r2, &used) == PD0V_TRUNCATED);
    memcpy(buf2, buf, n); buf2[8] = 2; CHECK(pd0_rec_parse(buf2, n, &r2, &used) == PD0V_BAD_VERSION);
    memcpy(buf2, buf, n); buf2[0] = 'X'; CHECK(pd0_rec_parse(buf2, n, &r2, &used) == PD0V_BAD_MAGIC);
    memcpy(buf2, buf, n); buf2[60] ^= 1; CHECK(pd0_rec_parse(buf2, n, &r2, &used) == PD0V_HASH_MISMATCH);
    /* same bytes in, same bytes out */
    size_t n2 = pd0_rec_write(&r, buf2, sizeof buf2); CHECK(n2 == n && !memcmp(buf, buf2, n));
    /* fixed-point product */
    CHECK(pd0_mul(2000000, 3000000) == 6000000 && pd0_mul(-1500000, 1000000) == -1500000 && pd0_mul(1, 1) == 0 && pd0_mul(-1, 1) == 0);
    /* relation round trip + size/bits */
    pd0_rel rel; memset(&rel, 0, sizeof rel); rel.n_vars = 3; rel.n_latent = 1; rel.n_channels = 1; rel.n_equations = 2; rel.eq[0].target = 0; rel.eq[0].n_terms = 1; rel.eq[0].coef[0] = 50000; rel.eq[0].expo[0][1] = 1;
    rel.eq[1].target = 1; rel.eq[1].n_terms = 2; rel.eq[1].coef[0] = -100000; rel.eq[1].expo[0][0] = 1; rel.eq[1].coef[1] = 50000; rel.eq[1].expo[1][3] = 1; rel.description_bits = pd0_rel_bits(&rel);
    n = pd0_rel_write(&rel, buf, sizeof buf); CHECK(n == 10 + 3 + 12 + 3 + 24); pd0_rel rel2; CHECK(pd0_rel_parse(buf, n, &rel2, &used) == 0 && used == n);
    CHECK(pd0_rel_size(&rel) == 4 && pd0_rel_bits(&rel) == 3 * (8 * 4 + 24) + 8 && pd0_rel_max_degree(&rel) == 1);
    int64_t st[3] = { 1000000, 2000000, 0 }, nx[3]; pd0_rel_step(&rel, st, 0, 1000000, nx); CHECK(nx[0] == 1100000 && nx[1] == 2000000 - 100000 + 50000 && nx[2] == 0);
    /* PDLAW1 round trip, law_id verified, tamper detected, version refused */
    pd0_law *l = calloc(1, sizeof *l); l->state = PDLAW_PROVISIONAL_LAW; l->rel = rel; l->dom.n_obs = 2; l->dom.n_channels = 1; l->dom.var_min[0] = -1; l->dom.var_max[0] = 1; l->confidence_ppm = 900000; l->n_exceptions = 1; l->exc[0].record_seq = 5; l->n_experiments = 1; l->exp[0].kind = 1; l->exp[0].first_seq = 1; l->exp[0].last_seq = 2; l->claim_len = 5; memcpy(l->claim, "Acros", 5);
    n = pd0_law_write(l, buf, sizeof buf); CHECK(n > 0); pd0_law *l2 = calloc(1, sizeof *l2); CHECK(pd0_law_parse(buf, n, l2) == 0 && l2->confidence_ppm == 900000 && l2->exc[0].record_seq == 5 && l2->exp[0].last_seq == 2 && l2->claim_len == 5);
    memcpy(buf2, buf, n); buf2[n - 1] ^= 1; CHECK(pd0_law_parse(buf2, n, l2) == PD0V_LAW_ID_MISMATCH);
    memcpy(buf2, buf, n); buf2[8] = 9; CHECK(pd0_law_parse(buf2, n, l2) == PD0V_BAD_VERSION);
    /* PD0EXP1 */
    pd0_exp *e = calloc(1, sizeof *e); e->n_obs = 2; e->n_hyp = 2; e->n_steps = 20; e->reset[0] = 1; for (int s = 0; s < 20; s++) { e->steps[s].channel = 0; e->steps[s].value = s; } e->expected[1][3][1] = 42;
    uint8_t *eb = malloc(16384); n = pd0_exp_write(e, eb, 16384); pd0_exp *e2 = calloc(1, sizeof *e2); CHECK(pd0_exp_parse(eb, n, e2, &used) == 0 && used == n && e2->expected[1][3][1] == 42 && !memcmp(e->schedule_hash, e2->schedule_hash, 32));
    eb[9] = 3; CHECK(pd0_exp_parse(eb, n, e2, &used) == PD0V_BAD_VERSION);
    /* ledger: chain and tamper */
    uint8_t *led = malloc(1 << 16); uint8_t last[32] = { 0 }; size_t ll = 0; uint8_t p1[4] = { 1, 2, 3, 4 };
    ll = pd0_ledg_append(led, ll, 1 << 16, LEDG_TAG, p1, 4, last); ll = pd0_ledg_append(led, ll, 1 << 16, LEDG_TAG, p1, 4, last); CHECK(ll == 2 * (16 + 4 + 64));
    size_t off = 0; uint8_t prev[32] = { 0 }; pd0_entry en; CHECK(pd0_ledg_next(led, ll, &off, prev, &en) == 0); memcpy(prev, en.entry_hash, 32); CHECK(pd0_ledg_next(led, ll, &off, prev, &en) == 0 && off == ll);
    off = 0; memset(prev, 0, 32); led[17] ^= 1; CHECK(pd0_ledg_next(led, ll, &off, prev, &en) == PD0V_HASH_MISMATCH); led[17] ^= 1;
    off = 0; memset(prev, 1, 32); CHECK(pd0_ledg_next(led, ll, &off, prev, &en) == PD0V_CHAIN_BROKEN);
    CHECK(strcmp(pd0v_name(PD0V_T5_SCHEDULE_REUSED), "PD0V_T5_SCHEDULE_REUSED") == 0);
    free(l); free(l2); free(e); free(e2); free(eb); free(led);
    printf("test_pd0_fmt: %d checks, %d failed\n", g_checks, g_fail); return g_fail ? 1 : 0;
}
/* golden: a PD0REC1 record written by the substrate lane's encoder (omega PR #243, tests/physics0/verify/golden_pd0rec1_pr243.bin) parses here byte for byte */
int golden_check(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "golden missing: %s\n", path); return 1; }
    uint8_t b[512]; size_t n = fread(b, 1, sizeof b, f); fclose(f); pd0_rec r; size_t used = 0;
    int rc = pd0_rec_parse(b, n, &r, &used);
    if (rc || used != 152 || r.n_obs != 2 || r.seq != 3 || r.episode != 1 || r.step_in_episode != 2 || r.time_micro != 100000 || r.applied != -250000 || r.before[1] != -2000000 || r.after[0] != 900000 || r.prev_hash[0] != 0xAB) { fprintf(stderr, "golden parse rc=%d used=%zu\n", rc, used); return 1; }
    uint8_t out[512]; size_t m = pd0_rec_write(&r, out, sizeof out); return (m == n && !memcmp(out, b, n)) ? 0 : 2;
}
