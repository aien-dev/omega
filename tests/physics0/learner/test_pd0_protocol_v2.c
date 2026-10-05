/* Unit tests for PD0 protocol v2 revision 2 (docs/physics0/PD0_PROTOCOL_V2.md): the final gate, the refusal counters,
 * shape and score values. In-process against the omega world (test-side; links the generators). */
#define PD0_WORLD_IMPL 1
#include "physics0/pd0_world.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint8_t resp[4096];
static size_t call(pd0_world *w, const uint8_t *req, size_t n) { return pd0_world_handle(w, req, n, resp, sizeof resp); }
static size_t do_score(pd0_world *w, const int64_t *reset, int no, int n, const uint8_t *ch, const int64_t *val) {
    uint8_t q[512]; size_t o = 0; q[o++] = 3; q[o++] = (uint8_t)no;
    for (int j = 0; j < no; j++, o += 8) pd0_put_u64(q + o, (uint64_t)reset[j]);
    q[o++] = (uint8_t)n; for (int s = 0; s < n; s++, o += 8) { q[o++] = ch[s]; pd0_put_u64(q + o, (uint64_t)val[s]); }
    return call(w, q, o);
}
static void audit(pd0_world *w, uint32_t *c, int *fin) {
    uint8_t q[1] = { 6 }; size_t n = call(w, q, 1); CHECK(n == PD0_AUDIT_RESP_SIZE && !memcmp(resp, "PD0AUDT1", 8));
    *fin = resp[8]; for (int i = 0; i < 4; i++) c[i] = pd0_get_u32(resp + 9 + 4 * i);
}

static void run_level(int level) {
    pd0_world w; CHECK(pd0_world_init(&w, level, 1) == 0);
    uint8_t dq[1] = { 0 }, dd[PD0_DESC_MAX]; pd0_desc d; pd0_world_describe(&w, &d);
    size_t dn = pd0_desc_encode(&d, dd, sizeof dd);
    size_t n = call(&w, dq, 1); CHECK(n == dn && !memcmp(resp, dd, dn));            /* describe free */
    uint32_t c[4]; int fin;
    /* before final: score and shape refused and counted; play allowed; describe free */
    int64_t reset[PD0_MAX_OBS] = { 1000000, 0, 0, 0 }; uint8_t ch[2] = { 0, PD0_CH_NONE }; int64_t val[2] = { 100000, 0 };
    CHECK(do_score(&w, reset, d.n_obs, 2, ch, val) == 0);
    uint8_t sq[1] = { 4 };   /* shape before final: dimensions only, zero terms, not a refusal */
    for (int k = 0; k < 2; k++) { n = call(&w, sq, 1); CHECK(n == 22 && !memcmp(resp, "PD0SHAP1", 8) && resp[11] == 0 && pd0_get_u16(resp + 20) == 0 && (int)pd0_get_u32(resp + 12) == pd0_gen_true_size(level) && pd0_get_u32(resp + 16) == 0); }
    { pd0_gen g0; pd0_relation r0; CHECK(pd0_gen_init(&g0, level, 1) == 0); pd0_gen_true_relation(&g0, d.dt_micro, &r0); CHECK(resp[8] == r0.n_vars && resp[9] == r0.n_latent && resp[10] == r0.n_channels); }
    audit(&w, c, &fin); CHECK(!fin && c[0] == 1 && c[1] == 0 && c[2] == 0 && c[3] == 0);
    uint8_t rq[2 + 8 * PD0_MAX_OBS]; size_t ro = 0; rq[ro++] = 1; rq[ro++] = d.n_obs; for (int j = 0; j < d.n_obs; j++, ro += 8) pd0_put_u64(rq + ro, (uint64_t)reset[j]);
    pd0_rec r; n = call(&w, rq, ro); CHECK(n > 0 && pd0_rec_decode(resp, n, &r) == 0 && r.status == PD0_OK);
    uint8_t tq[10] = { 2, 0 }; pd0_put_u64(tq + 2, 100000); n = call(&w, tq, 10); CHECK(n > 0 && pd0_rec_decode(resp, n, &r) == 0);
    n = call(&w, dq, 1); CHECK(n == dn);
    /* malformed final: not counted, gate stays shut */
    uint8_t fq[1 + PD0_HASH]; memset(fq, 0xA5, sizeof fq); fq[0] = 5; CHECK(call(&w, fq, 5) == 0);
    audit(&w, c, &fin); CHECK(!fin);
    /* final once */
    n = call(&w, fq, sizeof fq); CHECK(n == 1 && resp[0] == 0);
    /* duplicate final (different hash): refused, counted, first hash kept */
    uint8_t f2[1 + PD0_HASH]; memset(f2, 0x5A, sizeof f2); f2[0] = 5; n = call(&w, f2, sizeof f2); CHECK(n == 1 && resp[0] == 1);
    audit(&w, c, &fin); CHECK(fin && c[3] == 1 && !memcmp(resp + 25, fq + 1, PD0_HASH));
    /* play after final refused and counted */
    CHECK(call(&w, rq, ro) == 0); CHECK(call(&w, tq, 10) == 0);
    /* shape now correct */
    pd0_gen g; pd0_relation rel; CHECK(pd0_gen_init(&g, level, 1) == 0); pd0_gen_true_relation(&g, d.dt_micro, &rel);
    n = call(&w, sq, 1); unsigned nt = 0; CHECK(resp[11] == rel.n_eq); for (int e = 0; e < rel.n_eq; e++) nt += rel.eq[e].n_terms;
    CHECK(n == 22 + (size_t)nt * (9 + rel.n_vars + rel.n_channels) && !memcmp(resp, "PD0SHAP1", 8));
    CHECK(resp[8] == rel.n_vars && resp[9] == rel.n_latent && resp[10] == rel.n_channels && resp[11] == rel.n_eq);
    CHECK((int)pd0_get_u32(resp + 12) == pd0_gen_true_size(level) && pd0_get_u16(resp + 20) == nt);
    if (nt) { CHECK(resp[22] == rel.eq[0].target && (int64_t)pd0_get_u64(resp + 23) == rel.eq[0].t[0].coef); }
    /* score now correct: compare with the generator step by step */
    n = do_score(&w, reset, d.n_obs, 2, ch, val); CHECK(n == 2 + 8u * d.n_obs * 2 && resp[0] == 0 && resp[1] == 2);
    { int64_t s[PD0_MAX_OBS] = { 0 }, h = 0, ns[PD0_MAX_OBS], nh, obs[PD0_MAX_OBS]; pd0_rng a, b; pd0_stream(&a, 1, "x"); pd0_stream(&b, 1, "y");
      for (int j = 0; j < d.n_obs; j++) s[j] = reset[j];
      for (int st = 0; st < 2; st++) { int64_t u[PD0_MAX_CH] = { 0 }; if (ch[st] != PD0_CH_NONE) u[ch[st]] = val[st];
          CHECK(pd0_gen_step(&g, s, h, u, d.dt_micro, ns, &nh, obs, &a, &b) == 0); memcpy(s, ns, sizeof s); h = nh;
          for (int j = 0; j < d.n_obs; j++) CHECK((int64_t)pd0_get_u64(resp + 2 + 8 * (st * d.n_obs + j)) == s[j]); } }
    { uint8_t bad[1] = { 7 }; int64_t vz[1] = { 0 }; CHECK(do_score(&w, reset, d.n_obs, 1, bad, vz) == 0); CHECK(do_score(&w, reset, d.n_obs + 1, 1, bad, vz) == 0); }
    /* describe free after final; final counters */
    n = call(&w, dq, 1); CHECK(n == dn && !memcmp(resp, dd, dn));
    audit(&w, c, &fin); CHECK(fin && c[0] == 1 && c[1] == 0 && c[2] == 2 && c[3] == 1);
    /* ops 0..2 unchanged: unknown op refused */
    uint8_t uq[1] = { 9 }; CHECK(call(&w, uq, 1) == 0);
}

int main(void) {
    for (int level = 0; level <= 7; level++) run_level(level);
    printf("test_pd0_protocol_v2: %d checks, %d failed\n", checks, fails);
    printf(fails ? "PD0_PROTOCOL_V2: FAIL\n" : "PD0_PROTOCOL_V2: PASS\n");
    return fails != 0;
}
