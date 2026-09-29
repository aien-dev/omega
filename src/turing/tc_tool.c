/* turing-coder: EXP-001 probability-stream producer, reference coders, verifier
 * and overhead-envelope measurement. See calibration/docs/CODER_SPEC.md.
 *
 *   pstream  <model.tym|uniform:K> <profile_hex> <trace.ctr> <out.tps> <out.tsy>
 *   encode   <range|rans> <in.tps> <in.tsy> <out.coded> [expect...]
 *   decode   <range|rans> <in.tps> <in.coded> <out.tsy> [expect...]
 *   verify   <range|rans> <in.tps> <in.tsy> <in.coded> [expect...]
 *   envelope <profile_hex> <out.csv> <trace.ctr> <model.tym|uniform:K>...
 * expect = --profile HEX | --model HEX | --dataset HEX (refuse on mismatch).
 * Exit status 0 = accepted; 2 = refused (reason on stderr); 1 = usage.
 */
#define _POSIX_C_SOURCE 200809L
#include "turing/tc_produce.h"
#include "turing/tc_range.h"
#include "turing/tc_rans.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char why[512];

static int refuse(const char *what, int rc) {
    fprintf(stderr, "REFUSED %s: %s: %s\n", what, tc_err_name(rc), why[0] ? why : "(no detail)");
    return 2;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int get_model(const char *arg, ty_model *m, uint8_t dig[TC_DIGEST], uint64_t *lm) {
    if (strncmp(arg, "uniform:", 8) == 0) {
        unsigned K = (unsigned)atoi(arg + 8);
        if (ty_model_uniform(m, K) != TY_OK) return TC_E_ARG;
        uint8_t *b;
        size_t n;
        if (ty_model_encode(m, &b, &n, lm) != TY_OK) return TC_E_IO;
        ty_model_digest(b, n, dig);
        free(b);
        return TC_OK;
    }
    return tc_load_model(arg, m, dig, lm, why, sizeof why);
}

static int load_ps(const char *path, tc_pstream *p) {
    uint8_t *b;
    size_t n;
    if (tc_read_file(path, &b, &n) != TC_OK) {
        snprintf(why, sizeof why, "cannot read %s", path);
        return TC_E_IO;
    }
    int rc = tc_ps_parse(b, n, p, why, sizeof why);
    free(b);
    return rc;
}

static int load_sy(const char *path, tc_symbols *s, uint8_t **raw, size_t *rawn) {
    uint8_t *b;
    size_t n;
    if (tc_read_file(path, &b, &n) != TC_OK) {
        snprintf(why, sizeof why, "cannot read %s", path);
        return TC_E_IO;
    }
    int rc = tc_sy_parse(b, n, s, why, sizeof why);
    if (raw && rc == TC_OK) {
        *raw = b;
        *rawn = n;
    } else {
        free(b);
    }
    return rc;
}

/* Parse trailing --profile/--model/--dataset options. */
static int parse_expect(int argc, char **argv, int i, uint8_t pr[32], uint8_t mo[32], uint8_t da[32], int have[3]) {
    have[0] = have[1] = have[2] = 0;
    for (; i + 1 < argc; i += 2) {
        uint8_t *d = !strcmp(argv[i], "--profile") ? pr : !strcmp(argv[i], "--model") ? mo
                   : !strcmp(argv[i], "--dataset") ? da : NULL;
        if (!d || tc_unhex(argv[i + 1], d) != TC_OK) return -1;
        have[d == pr ? 0 : d == mo ? 1 : 2] = 1;
    }
    return i == argc ? 0 : -1;
}

static int coder_id(const char *s) { return !strcmp(s, "range") ? 1 : !strcmp(s, "rans") ? 2 : 0; }

static int cmd_pstream(char **a) {
    ty_model m;
    uint8_t md[32], pd[32], dd[32];
    uint64_t lm = 0;
    int rc = get_model(a[0], &m, md, &lm);
    if (rc != TC_OK) return refuse("model", rc);
    if (tc_unhex(a[1], pd) != TC_OK) return refuse("profile digest", TC_E_ARG);
    if (tc_file_sha256(a[2], dd) != TC_OK) return refuse("dataset", TC_E_IO);
    ty_stream s;
    ty_stream_init(&s);
    if (ty_ctr1_read(a[2], &s, why, sizeof why) < 0) return refuse("trace", TC_E_FORMAT);
    tc_pstream p;
    tc_symbols sy;
    if ((rc = tc_produce(&m, &s, pd, md, dd, &p, &sy, why, sizeof why)) != TC_OK) return refuse("produce", rc);
    uint8_t *b1, *b2;
    size_t n1, n2;
    if ((rc = tc_ps_serialize(&p, &b1, &n1, why, sizeof why)) != TC_OK) return refuse("serialize", rc);
    if ((rc = tc_sy_serialize(&sy, &b2, &n2, why, sizeof why)) != TC_OK) return refuse("serialize", rc);
    if (tc_write_file(a[3], b1, n1) != TC_OK || tc_write_file(a[4], b2, n2) != TC_OK) return refuse("write", TC_E_IO);
    int64_t ub;
    tc_ideal_ub(&p, &sy, 0, p.n, &ub);
    char h[4][65];
    tc_hex(md, h[0]), tc_hex(dd, h[1]), tc_hex(p.digest, h[2]), tc_hex(sy.digest, h[3]);
    printf("model_digest=%s\nlm_bits=%" PRIu64 "\ndataset_digest=%s\ntps1_digest=%s\ntsy1_digest=%s\n"
           "count=%" PRIu64 "\nideal_ub=%" PRId64 "\n",
           h[0], lm, h[1], h[2], h[3], p.n, ub);
    free(b1), free(b2), tc_ps_free(&p), tc_sy_free(&sy), ty_stream_free(&s), ty_model_free(&m);
    return 0;
}

static int check_expect(const tc_pstream *p, int argc, char **argv, int i) {
    uint8_t pr[32], mo[32], da[32];
    int have[3];
    if (parse_expect(argc, argv, i, pr, mo, da, have) != 0) {
        snprintf(why, sizeof why, "bad --profile/--model/--dataset option");
        return TC_E_ARG;
    }
    return tc_ps_expect(p, have[0] ? pr : NULL, have[1] ? mo : NULL, have[2] ? da : NULL, why, sizeof why);
}

static int cmd_coder(const char *cmd, int argc, char **argv) {
    int id = coder_id(argv[2]);
    if (!id) return 1;
    tc_pstream p;
    tc_symbols s, d;
    memset(&p, 0, sizeof p), memset(&s, 0, sizeof s), memset(&d, 0, sizeof d);
    uint8_t *cb = NULL, *db = NULL, *raw = NULL;
    size_t cn = 0, dn = 0, rawn = 0;
    int ret = 0, rc;
    const char *stage = "probability stream";
#define STOP(st, r)      \
    do {                 \
        stage = (st);    \
        rc = (r);        \
        goto refused;    \
    } while (0)
    if ((rc = load_ps(argv[3], &p)) != TC_OK) STOP("probability stream", rc);
    if ((rc = check_expect(&p, argc, argv, 6)) != TC_OK) STOP("probability stream", rc);
    if (!strcmp(cmd, "encode")) {
        if ((rc = load_sy(argv[4], &s, NULL, NULL)) != TC_OK) STOP("symbols", rc);
        rc = id == 1 ? tc_range_encode(&p, &s, &cb, &cn, why, sizeof why) : tc_rans_encode(&p, &s, &cb, &cn, why, sizeof why);
        if (rc != TC_OK) STOP("encode", rc);
        if (tc_write_file(argv[5], cb, cn) != TC_OK) STOP("write", TC_E_IO);
        printf("coded_bytes=%zu\ncoded_bits=%zu\n", cn, 8 * cn);
        goto done;
    }
    if (tc_read_file(!strcmp(cmd, "decode") ? argv[4] : argv[5], &cb, &cn) != TC_OK) STOP("coded file", TC_E_IO);
    rc = id == 1 ? tc_range_decode(&p, cb, cn, &d, why, sizeof why) : tc_rans_decode(&p, cb, cn, &d, why, sizeof why);
    if (rc != TC_OK) STOP("decode", rc);
    if ((rc = tc_sy_serialize(&d, &db, &dn, why, sizeof why)) != TC_OK) STOP("decode", rc);
    if (!strcmp(cmd, "decode")) {
        if (tc_write_file(argv[5], db, dn) != TC_OK) STOP("write", TC_E_IO);
        goto done;
    }
    if ((rc = load_sy(argv[4], &s, &raw, &rawn)) != TC_OK) STOP("symbols", rc);
    if ((rc = tc_pair_check(&p, &s, why, sizeof why)) != TC_OK) STOP("symbols", rc);
    if (rawn != dn || memcmp(raw, db, dn) != 0) {
        snprintf(why, sizeof why, "decoded symbol file is not byte-identical to %s", argv[4]);
        STOP("verify", TC_E_SYMBOL);
    }
    int64_t ub;
    tc_ideal_ub(&p, &s, 0, p.n, &ub);
    char h[65];
    tc_hex(p.digest, h);
    printf("coder=%s\ntps1_digest=%s\ncount=%" PRIu64 "\ncoded_bytes=%zu\ncoded_bits=%zu\nideal_ub=%" PRId64
           "\noverhead_ub=%" PRId64 "\nroundtrip=byte-exact\n",
           argv[2], h, p.n, cn, 8 * cn, ub, (int64_t)(8 * cn) * 1000000 - ub);
    goto done;
refused:
    ret = refuse(stage, rc);
done:
#undef STOP
    free(cb), free(db), free(raw);
    tc_ps_free(&p), tc_sy_free(&s), tc_sy_free(&d);
    return ret;
}

/* Envelope: per file and per crumb, both coders, with full round trip. */
static int cmd_envelope(int argc, char **argv) {
    uint8_t pd[32], dd[32];
    if (tc_unhex(argv[2], pd) != TC_OK) return refuse("profile digest", TC_E_ARG);
    FILE *csv = fopen(argv[3], "w");
    if (!csv) return refuse("csv", TC_E_IO);
    const char *trace = argv[4];
    if (tc_file_sha256(trace, dd) != TC_OK) return refuse("dataset", TC_E_IO);
    ty_stream s;
    ty_stream_init(&s);
    if (ty_ctr1_read(trace, &s, why, sizeof why) < 0) return refuse("trace", TC_E_FORMAT);
    fprintf(csv, "unit,model,index,n,ideal_ub,range_bytes,rans_bytes\n");
    for (int mi = 5; mi < argc; ++mi) {
        ty_model m;
        uint8_t md[32];
        uint64_t lm;
        int rc = get_model(argv[mi], &m, md, &lm);
        if (rc != TC_OK) return refuse("model", rc);
        char mh[65];
        tc_hex(md, mh);
        tc_pstream p0, p;
        tc_symbols sy0, sy;
        if ((rc = tc_produce(&m, &s, pd, md, dd, &p0, &sy0, why, sizeof why)) != TC_OK) return refuse("produce", rc);
        /* Through the canonical bytes: what the coders see is the parsed file. */
        uint8_t *pb, *sb;
        size_t pn, sn;
        if (tc_ps_serialize(&p0, &pb, &pn, why, sizeof why) != TC_OK || tc_sy_serialize(&sy0, &sb, &sn, why, sizeof why) != TC_OK ||
            tc_ps_parse(pb, pn, &p, why, sizeof why) != TC_OK || tc_sy_parse(sb, sn, &sy, why, sizeof why) != TC_OK)
            return refuse("canonical round trip", TC_E_FORMAT);
        tc_ps_free(&p0), tc_sy_free(&sy0), free(pb), free(sb);
        int64_t ub;
        tc_ideal_ub(&p, &sy, 0, p.n, &ub);
        uint8_t *ca, *cb;
        size_t na, nb;
        double t0 = now_s();
        if ((rc = tc_range_encode(&p, &sy, &ca, &na, why, sizeof why)) != TC_OK) return refuse("range encode", rc);
        double t1 = now_s();
        tc_symbols da, db;
        if ((rc = tc_range_decode(&p, ca, na, &da, why, sizeof why)) != TC_OK) return refuse("range decode", rc);
        double t2 = now_s();
        if ((rc = tc_rans_encode(&p, &sy, &cb, &nb, why, sizeof why)) != TC_OK) return refuse("rans encode", rc);
        double t3 = now_s();
        if ((rc = tc_rans_decode(&p, cb, nb, &db, why, sizeof why)) != TC_OK) return refuse("rans decode", rc);
        double t4 = now_s();
        if (memcmp(da.sym, sy.sym, sy.n) || memcmp(db.sym, sy.sym, sy.n)) {
            snprintf(why, sizeof why, "whole-file round trip differs");
            return refuse("round trip", TC_E_SYMBOL);
        }
        double ms = (double)sy.n / 1e6;
        printf("file %s model=%.16s mask=%u K=%u lm_bits=%" PRIu64 " n=%" PRIu64 " ideal_ub=%" PRId64
               " range_bytes=%zu rans_bytes=%zu ovh_range_bits=%.3f ovh_rans_bits=%.3f"
               " range_enc_Msym_s=%.1f range_dec_Msym_s=%.1f rans_enc_Msym_s=%.1f rans_dec_Msym_s=%.1f\n",
               trace, mh, m.mask, m.K, lm, sy.n, ub, na, nb, (8.0 * na * 1e6 - ub) / 1e6, (8.0 * nb * 1e6 - ub) / 1e6,
               ms / (t1 - t0), ms / (t2 - t1), ms / (t3 - t2), ms / (t4 - t3));
        fprintf(csv, "file,%.16s,0,%" PRIu64 ",%" PRId64 ",%zu,%zu\n", mh, sy.n, ub, na, nb);
        tc_sy_free(&da), tc_sy_free(&db), free(ca), free(cb);
        /* Per crumb: each crumb as its own coded file (header counted). */
        uint64_t lo = 0;
        uint8_t *tmp = malloc(sy.n ? sy.n : 1);
        while (lo < p.n) {
            uint64_t hi = lo + 1;
            while (hi < p.n && p.crumb[hi] == p.crumb[lo]) ++hi;
            int64_t cu;
            tc_ideal_ub(&p, &sy, lo, hi, &cu);
            uint8_t *x, *y;
            size_t nx, ny;
            if (tc_range_encode_raw(p.q + lo * p.K, p.K, sy.sym + lo, hi - lo, &x, &nx, NULL) != TC_OK ||
                tc_rans_encode_raw(p.q + lo * p.K, p.K, sy.sym + lo, hi - lo, &y, &ny) != TC_OK)
                return refuse("crumb encode", TC_E_IO);
            if (tc_range_decode_raw(p.q + lo * p.K, p.K, x, nx, hi - lo, tmp, why, sizeof why) != TC_OK ||
                memcmp(tmp, sy.sym + lo, hi - lo) ||
                tc_rans_decode_raw(p.q + lo * p.K, p.K, y, ny, hi - lo, tmp, why, sizeof why) != TC_OK ||
                memcmp(tmp, sy.sym + lo, hi - lo))
                return refuse("crumb round trip", TC_E_SYMBOL);
            fprintf(csv, "crumb,%.16s,%u,%" PRIu64 ",%" PRId64 ",%zu,%zu\n", mh, p.crumb[lo], hi - lo, cu,
                    nx + TC_CODED_HEADER, ny + TC_CODED_HEADER);
            free(x), free(y);
            lo = hi;
        }
        free(tmp);
        tc_ps_free(&p), tc_sy_free(&sy), ty_model_free(&m);
    }
    ty_stream_free(&s);
    return fclose(csv) == 0 ? 0 : refuse("csv", TC_E_IO);
}

int main(int argc, char **argv) {
    if (argc == 7 && !strcmp(argv[1], "pstream")) return cmd_pstream(argv + 2);
    if (argc >= 6 && (!strcmp(argv[1], "encode") || !strcmp(argv[1], "decode") || !strcmp(argv[1], "verify")) &&
        coder_id(argv[2]) && (argc - 6) % 2 == 0)
        return cmd_coder(argv[1], argc, argv);
    if (argc >= 6 && !strcmp(argv[1], "envelope")) return cmd_envelope(argc, argv);
    fprintf(stderr,
            "usage:\n  turing-coder pstream <model.tym|uniform:K> <profile_hex> <trace.ctr> <out.tps> <out.tsy>\n"
            "  turing-coder encode|decode|verify <range|rans> <in.tps> <in.tsy|in.coded> <out|in> "
            "[--profile H] [--model H] [--dataset H]\n"
            "  turing-coder envelope <profile_hex> <out.csv> <trace.ctr> <model.tym|uniform:K>...\n");
    return 1;
}
