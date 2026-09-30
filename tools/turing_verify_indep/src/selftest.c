/* Self tests from the documents' own test vectors, plus the log2 rounding audit (SPEC_GAPS G2). */
#include "core.h"
#include "sha256.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("ok   "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

typedef struct { uint8_t b[4096]; uint64_t pos; } bw;
static void put(bw *w, uint64_t v, int n) { int i; for (i = n - 1; i >= 0; i--) { if ((v >> i) & 1) w->b[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7)); w->pos++; } }

int main(int argc, char **argv) {
    uint8_t d[32]; char hx[65]; is_sha256 c; is_model m; char msg[256]; int r, q;
    const char *docs = argc > 1 ? argv[1] : NULL, *cand = argc > 2 ? argv[2] : NULL;
    is_sha256_init(&c); is_sha256_update(&c, "abc", 3); is_sha256_final(&c, d); is_sha256_hex(d, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc)");
    { static const uint8_t b0[29] = {0x54,0x59,0x4d,0x30,0x00,0x09,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x71};
      FILE *f = fopen("/tmp/is_b0.tym", "wb"); fwrite(b0, 1, 29, f); fclose(f);
      r = is_model_load("/tmp/is_b0.tym", &m, msg, sizeof msg); is_sha256_hex(m.model_digest, hx);
      CHECK(r == 0 && m.lm_bits == 232 && m.def[8] == 7281 && !strcmp(hx, "83c04bdfc10614febd634092355ef03997ed255fde5bdec9a80fe2d8964998ce"), "B0 test vector: L=232, digest %s", hx);
      is_model_free(&m); }
    { bw w; int k, j; memset(&w, 0, sizeof w); FILE *f;
      put(&w, 0x54594D30, 32); put(&w, 0, 8); put(&w, 9, 8); put(&w, 1, 8); put(&w, 16, 8); put(&w, 4, 8); put(&w, 16, 32);
      for (j = 0; j < 7; j++) put(&w, 7282, 16);
      put(&w, 7281, 16);
      for (k = 0; k < 16; k++) { put(&w, (uint64_t)k, 4);
        for (j = 0; j < 8; j++) put(&w, k < 15 ? (j < 7 ? 9362 : 1) : (j < 7 ? 1 : 32765), 16); }
      f = fopen("/tmp/is_b3.tym", "wb"); fwrite(w.b, 1, (w.pos + 7) / 8, f); fclose(f);
      r = is_model_load("/tmp/is_b3.tym", &m, msg, sizeof msg); is_sha256_hex(m.model_digest, hx);
      CHECK(r == 0 && m.lm_bits == 2344 && (w.pos + 7) / 8 == 293 && !strcmp(hx, "e729818b8289263e6819b21fc55f944f9d5113e7cfc4419d0badcc27f589f2ae"),
            "B3 rebuilt from prereg section 0: L=%llu bytes=%llu digest %s", (unsigned long long)m.lm_bits, (unsigned long long)((w.pos+7)/8), hx);
      CHECK(w.b[29] == 0x02 && w.b[30] == 0x49 && w.b[31] == 0x22, "B3 bytes 29..31 = 02 49 22 (MDE 7)");
      is_model_free(&m); }
    /* refusal tests */
    { static uint8_t b0[29] = {0x54,0x59,0x4d,0x30,0x00,0x09,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x72,0x1c,0x71};
      uint8_t t[30]; FILE *f;
      memcpy(t, b0, 29); t[4] = 1; f = fopen("/tmp/is_t.tym","wb"); fwrite(t,1,29,f); fclose(f);
      CHECK(is_model_load("/tmp/is_t.tym", &m, msg, sizeof msg) == -2, "refuse version 1");
      memcpy(t, b0, 29); t[7] = 8; f = fopen("/tmp/is_t.tym","wb"); fwrite(t,1,29,f); fclose(f);
      CHECK(is_model_load("/tmp/is_t.tym", &m, msg, sizeof msg) == -3, "refuse qbits 8");
      memcpy(t, b0, 29); t[29] = 0; f = fopen("/tmp/is_t.tym","wb"); fwrite(t,1,30,f); fclose(f);
      CHECK(is_model_load("/tmp/is_t.tym", &m, msg, sizeof msg) == -5, "refuse trailing byte");
      memcpy(t, b0, 29); t[13] = 0; t[14] = 0; f = fopen("/tmp/is_t.tym","wb"); fwrite(t,1,29,f); fclose(f);
      CHECK(is_model_load("/tmp/is_t.tym", &m, msg, sizeof msg) == -6, "refuse zero entry");
      memcpy(t, b0, 29); t[6] = 2; f = fopen("/tmp/is_t.tym","wb"); fwrite(t,1,29,f); fclose(f);
      CHECK(is_model_load("/tmp/is_t.tym", &m, msg, sizeof msg) == -4, "refuse keybits mismatch (mask 2)");
    }
    if (docs) {
        char p[1024]; const char *pv = is_profile_version(); if (!pv) pv = "1.0"; snprintf(p, sizeof p, "%s/profiles/Turing-profile-v%s.toml", docs, pv);
        is_sha256_file(p, d, NULL); is_sha256_hex(d, hx);
        { char sc[65] = ""; FILE *sf; snprintf(p, sizeof p, "%s/profiles/Turing-profile-v%s.sha256", docs, pv);
          if ((sf = fopen(p, "r")) != NULL) { if (fread(sc, 1, 64, sf) != 64) sc[0] = 0; fclose(sf); }
          CHECK(!strcmp(hx, sc), "profile sha256 = sidecar"); }
    }
    if (cand) {
        const char *nm[2] = {"M_mem", "M_mem_seed1"};
        const char *fs[2] = {"5a2ca3f457cc75b4f49fee99f054a0c32d15e3bfd442886c40e4a5f4945bab81", "34df83a19756d1bd7e73f2b218576cefedad40ac2fc0e7a7a3da10a2a7fd206f"};
        const char *md[2] = {"cbd6944c6618f508e5377e4698b0d472042cdf3b4592cb80e5427767e232c140", "5dfeff7b643178807b547fbd62388319b9a407109e345614679e4bf20ccfd652"};
        uint64_t lm[2] = {607838516ULL, 356335496ULL}, rows[2] = {3706331, 2172776}; int i; char h2[65];
        for (i = 0; i < 2; i++) { char p[1024]; snprintf(p, sizeof p, "%s/%s.tym", cand, nm[i]);
            r = is_model_load(p, &m, msg, sizeof msg); is_sha256_hex(m.file_sha, hx); is_sha256_hex(m.model_digest, h2);
            CHECK(r == 0 && !strcmp(hx, fs[i]) && !strcmp(h2, md[i]) && m.lm_bits == lm[i] && m.nrows == rows[i] && m.keybits == 36,
                  "%s: r=%d L=%llu rows=%llu keybits=%d (%s)", nm[i], r, (unsigned long long)m.lm_bits, (unsigned long long)m.nrows, m.keybits, r ? msg : "");
            is_model_free(&m); }
    }
    /* RNG smoke: splitmix64 from 0 (published reference first output e220a8397b1dcdaf) */
    { uint64_t s = 0; CHECK(is_splitmix64(&s) == 0xe220a8397b1dcdafULL, "splitmix64 reference output"); }
    /* log2 rounding audit against quad-precision log2l */
    is_ub_table_init();
    { int ndiff = 0, nnear = 0; long double worst = 1;
      FILE *nf = fopen("near_tie_q.txt", "w");
      for (q = 1; q <= 65536; q++) {
          long double ex = (16.0L - log2l((long double)q)) * 1000000.0L;
          long double fl = floorl(ex), fr = ex - fl;
          int64_t rn = (int64_t)fl + (fr >= 0.5L ? 1 : 0);
          long double dist = fabsl(fr - 0.5L);
          if (dist < worst && q != 65536) worst = dist;
          if (rn != is_ub_q16[q]) { ndiff++; printf("  table differs from exact rounding at q=%d: %lld vs %lld (frac %.9Lf)\n", q, (long long)is_ub_q16[q], (long long)rn, fr); }
          if (dist < 0.0005L) { nnear++; fprintf(nf, "%d %.12Lf\n", q, fr); }
      }
      fclose(nf);
      CHECK(ndiff == 2 && is_ub_q16[43481] == 591903 && is_ub_q16[46819] == 485194, "ub table (literal 32-step method) differs from correctly rounded exact value at exactly q=43481,46819 (G2: the written integer rule is the definition; differences %d)", ndiff);
      printf("     q whose exact ub fraction is within 5e-4 of .5: %d (listed in near_tie_q.txt); closest distance %.3Le\n", nnear, worst);
      printf("     ub(1)=%lld ub(7282)=%lld ub(32768)=%lld ub(65536)=%lld\n", (long long)is_ub_q16[1], (long long)is_ub_q16[7282], (long long)is_ub_q16[32768], (long long)is_ub_q16[65536]); }
    printf("%s (%d failures)\n", fails ? "SELFTEST FAIL" : "SELFTEST PASS", fails);
    return fails ? 1 : 0;
}
