/* EXP-001 producer test: TY-2 models (.tym, decoded) + committed CTR1 fixtures
 * -> TPS1 + TSY1 -> both coders -> byte-exact decode. The producer's ideal
 * length must equal ty_model_score (checked inside tc_produce), and the model
 * digests must be the ones the TY-2 held-out receipt paid for.
 * argv[1] = output prefix (<prefix>.det, compared across plain and ASan builds).
 */
#include "turing/tc_produce.h"
#include "turing/tc_range.h"
#include "turing/tc_rans.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...)                                \
    do {                                                \
        if (cond) {                                     \
            ++g_pass;                                   \
        } else {                                        \
            ++g_fail;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static char why[512];
static const char *FIX[2] = {"tests/turing/fixtures/ctr1_seed1_crumbs05-07.ctr",
                             "tests/turing/fixtures/ctr1_seed1_crumbs10-12.ctr"};
static const struct {
    const char *path, *digest;
} MODELS[2] = {
    {"evidence/TURING_YIELD/ty2_baseline.tym", "59ae9398da24e809d92ab39e7456382a0a4b7e93e203862be6d2ca55450dfd04"},
    {"evidence/TURING_YIELD/ty2_candidate.tym", "64a57ba9cf6f8b6e4558751ef2145ae2aa729c5214bfa29373eabfa220a57bc7"},
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char path[600];
    snprintf(path, sizeof path, "%s.det", argv[1]);
    FILE *det = fopen(path, "w");
    if (!det) return 2;
    uint8_t profile[32];
    memset(profile, 0, sizeof profile); /* dev placeholder; lane A's sidecar digest goes here at freeze */
    for (int f = 0; f < 2; ++f) {
        ty_stream s;
        ty_stream_init(&s);
        CHECK(ty_ctr1_read(FIX[f], &s, why, sizeof why) > 0, "read %s: %s", FIX[f], why);
        uint8_t dd[32];
        CHECK(tc_file_sha256(FIX[f], dd) == TC_OK, "dataset digest");
        for (int mi = 0; mi < 3; ++mi) {
            ty_model m;
            uint8_t md[32];
            uint64_t lm = 0;
            char mh[65];
            if (mi < 2) {
                CHECK(tc_load_model(MODELS[mi].path, &m, md, &lm, why, sizeof why) == TC_OK, "load %s: %s",
                      MODELS[mi].path, why);
                tc_hex(md, mh);
                CHECK(strcmp(mh, MODELS[mi].digest) == 0, "model digest %s != receipt %s", mh, MODELS[mi].digest);
            } else {
                ty_model_uniform(&m, TY_OUT_K);
                uint8_t *b;
                size_t n;
                ty_model_encode(&m, &b, &n, &lm);
                ty_model_digest(b, n, md);
                free(b);
                tc_hex(md, mh);
            }
            tc_pstream p;
            tc_symbols sy;
            int rc = tc_produce(&m, &s, profile, md, dd, &p, &sy, why, sizeof why);
            CHECK(rc == TC_OK, "produce: %s %s", tc_err_name(rc), why);
            if (rc != TC_OK) continue;
            uint8_t *pb, *sb;
            size_t pn, sn;
            tc_pstream P;
            tc_symbols S;
            CHECK(tc_ps_serialize(&p, &pb, &pn, why, sizeof why) == TC_OK &&
                      tc_sy_serialize(&sy, &sb, &sn, why, sizeof why) == TC_OK &&
                      tc_ps_parse(pb, pn, &P, why, sizeof why) == TC_OK && tc_sy_parse(sb, sn, &S, why, sizeof why) == TC_OK,
                  "canonical round trip: %s", why);
            CHECK(tc_ps_expect(&P, profile, md, dd, why, sizeof why) == TC_OK, "expect digests: %s", why);
            uint8_t other[32];
            memcpy(other, md, 32);
            other[0] ^= 1;
            CHECK(tc_ps_expect(&P, profile, other, dd, why, sizeof why) == TC_E_MODEL, "wrong model digest accepted");
            int64_t ub;
            tc_ideal_ub(&P, &S, 0, P.n, &ub);
            for (int c = 1; c <= 2; ++c) {
                uint8_t *cb;
                size_t cn;
                tc_symbols d;
                rc = c == 1 ? tc_range_encode(&P, &S, &cb, &cn, why, sizeof why) : tc_rans_encode(&P, &S, &cb, &cn, why, sizeof why);
                CHECK(rc == TC_OK, "encode %d: %s", c, why);
                if (rc != TC_OK) continue;
                rc = c == 1 ? tc_range_decode(&P, cb, cn, &d, why, sizeof why) : tc_rans_decode(&P, cb, cn, &d, why, sizeof why);
                CHECK(rc == TC_OK && memcmp(d.sym, S.sym, S.n) == 0, "decode %d: %s", c, why);
                if (rc == TC_OK) tc_sy_free(&d);
                char th[65];
                tc_hex(P.digest, th);
                fprintf(det, "fixture=%d model=%.16s n=%" PRIu64 " tps1=%s ideal_ub=%" PRId64 " coder=%d bytes=%zu\n", f, mh,
                        P.n, th, ub, c, cn);
                free(cb);
            }
            free(pb), free(sb);
            tc_ps_free(&p), tc_sy_free(&sy), tc_ps_free(&P), tc_sy_free(&S), ty_model_free(&m);
        }
        ty_stream_free(&s);
    }
    fclose(det);
    printf("test_tc_produce: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
