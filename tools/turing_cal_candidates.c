/* turing-cal-candidates: EXP-001 candidate set (lane A), dev seeds 1-7 only.
 *
 *   turing-cal-candidates write MANIFEST ROOT EVIDENCE_DIR OUTDIR
 *       Re-hash dev seeds 1-7 against MANIFEST, then write the seven TYM0 model
 *       codes to OUTDIR and print L(M) and the in-sample (dev, seeds 1-7) ideal
 *       L(D|M) of each:
 *         B0_uniform.tym     ty_model_uniform, K = 9
 *         B1_order0.tym      ty_model_fit mask 0 (order-0 frequencies)
 *         B2_order1.tym      EVIDENCE_DIR/ty2_baseline.tym, bytes unchanged
 *         B3_heuristic.tym   hand-set rows declared in calibration/preregistration/EXP-001.md
 *         M_candidate.tym    EVIDENCE_DIR/ty2_candidate.tym, bytes unchanged
 *         M_mem.tym          ty_model_fit mask TY_F_POS, TY_FIT_KEEP_ALL (memorization control)
 *         M_mem_seed1.tym    same fit on dev seed 1 alone, each row sharpened to its argmax
 *                            (65528, others 1): exact memorizer of seed 1
 *       B2 and M_candidate are also refit from seeds 1-7 (mask 4 and 93, MDL prune)
 *       and must be byte-identical to the evidence files, and their digests must
 *       equal the TY-2 receipt values; otherwise the tool refuses.
 *   turing-cal-candidates consts
 *       Print the code constants the profile TOML must agree with
 *       (read by calibration/scripts/check_profile.sh).
 */
#include "turing_cal_dev.h"

#define TXA_B2_MASK TY_F_PREV1
#define TXA_MC_MASK (TY_F_OP | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4)
#define TXA_MEM_MASK TY_F_POS
#define TXA_B2_DIGEST "59ae9398da24e809d92ab39e7456382a0a4b7e93e203862be6d2ca55450dfd04"
#define TXA_MC_DIGEST "64a57ba9cf6f8b6e4558751ef2145ae2aa729c5214bfa29373eabfa220a57bc7"

/* B3 literals, exactly as declared in the preregistration (commit fe32be5). */
static const uint32_t B3_EXPAND[TY_OUT_K] = {9362, 9362, 9362, 9362, 9362, 9362, 9362, 1, 1};
static const uint32_t B3_SUBMIT[TY_OUT_K] = {1, 1, 1, 1, 1, 1, 1, 32765, 32764};
static const uint32_t B3_DEFAULT[TY_OUT_K] = {7282, 7282, 7282, 7282, 7282, 7282, 7282, 7281, 7281};

static int fail(const char *what, int rc) {
    fprintf(stderr, "turing-cal-candidates: %s (%s)\n", what, ty_err_name(rc));
    return 2;
}

static int read_all(const char *path, uint8_t **buf, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return TY_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return TY_E_IO;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) {
        fclose(f);
        return TY_E_IO;
    }
    *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!*buf) {
        fclose(f);
        return TY_E_IO;
    }
    *n = fread(*buf, 1, (size_t)sz, f);
    fclose(f);
    return *n == (size_t)sz ? TY_OK : TY_E_IO;
}

static int write_all(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return TY_E_IO;
    size_t w = fwrite(b, 1, n, f);
    return (fclose(f) == 0 && w == n) ? TY_OK : TY_E_IO;
}

static int build_b3(ty_model *m) {
    memset(m, 0, sizeof *m);
    m->K = TY_OUT_K;
    m->mask = TY_F_OP;
    unsigned kb;
    int rc = ty_key_space(TY_F_OP, TY_OUT_K, NULL, &kb);
    if (rc != TY_OK) return rc;
    m->keybits = kb;
    /* The declared default row must equal the profile's quantized uniform row. */
    ty_model u;
    if ((rc = ty_model_uniform(&u, TY_OUT_K)) != TY_OK) return rc;
    if (memcmp(u.def, B3_DEFAULT, sizeof B3_DEFAULT) != 0) return TY_E_PROFILE;
    memcpy(m->def, B3_DEFAULT, sizeof B3_DEFAULT);
    m->rows = calloc(16, sizeof *m->rows);
    if (!m->rows) return TY_E_IO;
    for (unsigned k = 0; k < 16; ++k) {
        m->rows[k].key = k;
        memcpy(m->rows[k].q, k == TY_OP_SUBMIT ? B3_SUBMIT : B3_EXPAND, sizeof B3_EXPAND);
        uint32_t s = 0;
        for (unsigned x = 0; x < TY_OUT_K; ++x) s += m->rows[k].q[x];
        if (s != TY_QONE) return TY_E_NORM;
    }
    m->nrows = 16;
    return TY_OK;
}

typedef struct {
    const char *name, *file;
    uint8_t *code;
    size_t ncode;
    uint64_t lm_bits;
    int64_t ld_ub;
    uint64_t nsym;
    int64_t ld1_ub; /* seed 1 alone */
    uint64_t nsym1;
    uint64_t nrows;
    char digest[65];
} cand;

/* Decode the code (so the scored model is the paid model), score dev seeds 1-7. */
static int score_code(cand *c) {
    ty_model m;
    char why[256] = "";
    int rc = ty_model_decode(c->code, c->ncode, &m, &c->lm_bits, why, sizeof why);
    if (rc != TY_OK) {
        fprintf(stderr, "%s: %s\n", c->name, why);
        return rc;
    }
    c->nrows = m.nrows;
    c->ld_ub = 0;
    c->nsym = 0;
    for (int s = 1; s <= TCD_NDEV; ++s) {
        int64_t ub;
        uint64_t n;
        if ((rc = ty_model_score(&m, &TCD_S[s - 1], &ub, &n)) != TY_OK || (rc = ty_add(c->ld_ub, ub, &c->ld_ub)) != TY_OK) {
            ty_model_free(&m);
            return rc;
        }
        c->nsym += n;
        if (s == 1) {
            c->ld1_ub = ub;
            c->nsym1 = n;
        }
    }
    ty_model_free(&m);
    uint8_t d[32];
    ty_model_digest(c->code, c->ncode, d);
    tcd_hex(d, c->digest);
    return TY_OK;
}

static int encode(const ty_model *m, cand *c) {
    uint64_t bits;
    return ty_model_encode(m, &c->code, &c->ncode, &bits);
}

static int fit_encode(unsigned mask, int rule, uint8_t **code, size_t *n) {
    const ty_stream *st[TCD_NDEV];
    for (int i = 0; i < TCD_NDEV; ++i) st[i] = &TCD_S[i];
    ty_model m;
    int rc = ty_model_fit(&m, st, TCD_NDEV, mask, TY_OUT_K, rule);
    if (rc != TY_OK) return rc;
    uint64_t bits;
    rc = ty_model_encode(&m, code, n, &bits);
    ty_model_free(&m);
    return rc;
}

/* Evidence file bytes unchanged; digest must match the receipt; refit must match bytes. */
static int evidence_model(const char *dir, const char *file, const char *want_digest, unsigned mask, cand *c) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    int rc = read_all(path, &c->code, &c->ncode);
    if (rc != TY_OK) return rc;
    uint8_t d[32];
    char hex[65];
    ty_model_digest(c->code, c->ncode, d);
    tcd_hex(d, hex);
    if (strcmp(hex, want_digest) != 0) return TY_E_DIGEST;
    uint8_t *re;
    size_t nre;
    if ((rc = fit_encode(mask, TY_FIT_MDL_PRUNE, &re, &nre)) != TY_OK) return rc;
    int same = nre == c->ncode && memcmp(re, c->code, nre) == 0;
    free(re);
    if (!same) {
        fprintf(stderr, "%s: refit from dev seeds 1-7 is not byte-identical to the evidence file\n", file);
        return TY_E_PROVENANCE;
    }
    return TY_OK;
}

/* M_mem_seed1: position table fit on dev seed 1 ALONE (KEEP_ALL), each row then sharpened to
 * 65528 on its most likely symbol (lowest index on ties), floor 1 elsewhere. Every row has one
 * count, so this stores seed 1 exactly: an exact memorizer of one dev seed. */
static int sharpen(const cand *mem, cand *out) {
    ty_model m;
    uint64_t bits;
    int rc = ty_model_decode(mem->code, mem->ncode, &m, &bits, NULL, 0);
    if (rc != TY_OK) return rc;
    for (size_t i = 0; i < m.nrows; ++i) {
        unsigned best = 0;
        for (unsigned x = 1; x < m.K; ++x)
            if (m.rows[i].q[x] > m.rows[i].q[best]) best = x;
        for (unsigned x = 0; x < m.K; ++x) m.rows[i].q[x] = x == best ? TY_QONE - (m.K - 1) : 1;
    }
    rc = encode(&m, out);
    ty_model_free(&m);
    return rc;
}

static void fmt_bits(int64_t ub, char *out, size_t n) {
    int neg = ub < 0;
    uint64_t a = neg ? (uint64_t)(-(ub + 1)) + 1 : (uint64_t)ub;
    snprintf(out, n, "%s%" PRIu64 ".%06" PRIu64, neg ? "-" : "", a / 1000000u, a % 1000000u);
}

static int cmd_write(const char *mpath, const char *root, const char *evdir, const char *outdir) {
    int rc = tcd_load_manifest(mpath, root);
    if (rc != TY_OK) return fail("manifest", rc);
    for (int s = 1; s <= TCD_NDEV; ++s)
        if ((rc = tcd_load_seed(s)) != TY_OK) return fail("dev seed", rc);
    cand C[7] = {{"B0_uniform", "B0_uniform.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""},
                 {"B1_order0", "B1_order0.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""},
                 {"B2_order1", "B2_order1.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""},
                 {"B3_heuristic", "B3_heuristic.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""},
                 {"M_candidate", "M_candidate.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""},
                 {"M_mem", "M_mem.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""},
                 {"M_mem_seed1", "M_mem_seed1.tym", 0, 0, 0, 0, 0, 0, 0, 0, ""}};
    ty_model m;
    if ((rc = ty_model_uniform(&m, TY_OUT_K)) != TY_OK || (rc = encode(&m, &C[0])) != TY_OK) return fail("B0", rc);
    if ((rc = fit_encode(0, TY_FIT_MDL_PRUNE, &C[1].code, &C[1].ncode)) != TY_OK) return fail("B1", rc);
    if ((rc = evidence_model(evdir, "ty2_baseline.tym", TXA_B2_DIGEST, TXA_B2_MASK, &C[2])) != TY_OK) return fail("B2", rc);
    if ((rc = build_b3(&m)) != TY_OK || (rc = encode(&m, &C[3])) != TY_OK) return fail("B3", rc);
    ty_model_free(&m);
    if ((rc = evidence_model(evdir, "ty2_candidate.tym", TXA_MC_DIGEST, TXA_MC_MASK, &C[4])) != TY_OK)
        return fail("M_candidate", rc);
    if ((rc = fit_encode(TXA_MEM_MASK, TY_FIT_KEEP_ALL, &C[5].code, &C[5].ncode)) != TY_OK) return fail("M_mem", rc);
    {
        cand one = {"s1", "", 0, 0, 0, 0, 0, 0, 0, 0, ""};
        const ty_stream *s1 = &TCD_S[0];
        if ((rc = ty_model_fit(&m, &s1, 1, TXA_MEM_MASK, TY_OUT_K, TY_FIT_KEEP_ALL)) != TY_OK || (rc = encode(&m, &one)) != TY_OK)
            return fail("M_mem_seed1 fit", rc);
        ty_model_free(&m);
        rc = sharpen(&one, &C[6]);
        free(one.code);
        if (rc != TY_OK) return fail("M_mem_seed1", rc);
    }
    printf("# EXP-001 candidate set, dev seeds 1-7 (in-sample). L(M) exact bits; L(D|M) ideal bits (micro-bit exact).\n");
    printf("# name rows L(M)_bits L(D|M)_bits DL_bits bits_per_event code_bytes digest(turing.ymodel.v0)\n");
    for (int i = 0; i < 7; ++i) {
        if ((rc = score_code(&C[i])) != TY_OK) return fail(C[i].name, rc);
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", outdir, C[i].file);
        if ((rc = write_all(path, C[i].code, C[i].ncode)) != TY_OK) return fail(path, rc);
        int64_t dl;
        if ((rc = ty_dl(C[i].lm_bits, C[i].ld_ub, &dl)) != TY_OK) return fail("dl", rc);
        char ld[40], dls[40];
        fmt_bits(C[i].ld_ub, ld, sizeof ld);
        fmt_bits(dl, dls, sizeof dls);
        printf("%s rows=%" PRIu64 " lm_bits=%" PRIu64 " ld_bits=%s dl_bits=%s bpe=%.6f nsym=%" PRIu64
               " seed1_bpe=%.6f bytes=%zu digest=%s\n",
               C[i].name, C[i].nrows, C[i].lm_bits, ld, dls, (double)C[i].ld_ub / 1e6 / (double)C[i].nsym, C[i].nsym,
               (double)C[i].ld1_ub / 1e6 / (double)C[i].nsym1,
               C[i].ncode, C[i].digest);
        free(C[i].code);
    }
    return 0;
}

static int cmd_consts(void) {
    unsigned kb_b3, kb_mem, kb_mc;
    uint64_t sp;
    ty_key_space(TY_F_OP, TY_OUT_K, &sp, &kb_b3);
    ty_key_space(TXA_MEM_MASK, TY_OUT_K, &sp, &kb_mem);
    ty_key_space(TXA_MC_MASK, TY_OUT_K, &sp, &kb_mc);
    printf("K=%d\n", TY_OUT_K);
    printf("kmax=%d\n", TY_KMAX);
    printf("qbits=%d\n", TY_QBITS);
    printf("qone=%u\n", TY_QONE);
    printf("floor_q=1\n");
    printf("model_header_bits=%d\n", TY_MODEL_HEADER_BITS);
    printf("model_version=%d\n", TY_MODEL_VERSION);
    printf("model_domain=%s\n", TY_MODEL_DOMAIN);
    printf("ub_per_bit=%lld\n", (long long)TY_UB_PER_BIT);
    printf("qerr_milli_ub_per_symbol=%d\n", TY_QERR_MILLI_UB_PER_SYMBOL);
    printf("ctr1_record_bytes=%d\n", TY_CTR1_BYTES);
    printf("depth_clip=%d\n", TY_DEPTH_CLIP);
    printf("op_submit=%d\n", TY_OP_SUBMIT);
    printf("b2_mask=%d\n", TXA_B2_MASK);
    printf("m_candidate_mask=%d\n", TXA_MC_MASK);
    printf("m_mem_mask=%d\n", TXA_MEM_MASK);
    printf("b3_mask=%d\n", TY_F_OP);
    printf("b3_keybits=%u\n", kb_b3);
    printf("m_mem_keybits=%u\n", kb_mem);
    printf("m_candidate_keybits=%u\n", kb_mc);
    printf("b2_digest=%s\n", TXA_B2_DIGEST);
    printf("m_candidate_digest=%s\n", TXA_MC_DIGEST);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "consts") == 0) return cmd_consts();
    if (argc == 6 && strcmp(argv[1], "write") == 0) return cmd_write(argv[2], argv[3], argv[4], argv[5]);
    fprintf(stderr, "usage: turing-cal-candidates write MANIFEST ROOT EVIDENCE_DIR OUTDIR | consts\n");
    return 2;
}
