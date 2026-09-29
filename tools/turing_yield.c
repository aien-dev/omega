/* turing-yield: Turing Yield TY-2 driver (held-out predictive compression).
 *
 *   turing-yield manifest-check MANIFEST ROOT      re-hash every listed file
 *   turing-yield tune MANIFEST ROOT                fit seeds 1-6, score seed 7 (grid)
 *   turing-yield loso MANIFEST ROOT                leave-one-seed-out T inside seeds 1-7
 *   turing-yield heldout MANIFEST ROOT OUTDIR      fit seeds 1-7, score seeds 8-10 ONCE
 *   turing-yield verify MANIFEST ROOT RECEIPT      re-derive a held-out receipt
 *   turing-yield fixture FIT.ctr HELD.ctr          determinism printout (tests)
 *
 * MANIFEST lines: "<sha256>  <bytes>  <path relative to ROOT>", seeds 1..10 in
 * order. tune and loso open only entries 1-7. heldout refuses to run if
 * OUTDIR already holds a receipt: the held-out seeds are scored once.
 */
#include "turing/ty_record.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NSEED 10
#define NFIT 7

typedef struct {
    char hex[65];
    uint64_t size;
    char rel[256];
    char path[1024];
    ty_digest d;
} mentry;

static mentry M[NSEED];

static int die(const char *msg, int rc) {
    fprintf(stderr, "turing-yield: %s (%s)\n", msg, ty_err_name(rc));
    return 2;
}

static int load_manifest(const char *mpath, const char *root, ty_digest *mdig) {
    FILE *f = fopen(mpath, "r");
    if (!f) return TY_E_IO;
    char line[1400];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < NSEED) {
        if (line[0] == '#' || line[0] == '\n') continue;
        mentry *e = &M[n];
        if (sscanf(line, "%64s %" SCNu64 " %255s", e->hex, &e->size, e->rel) != 3) {
            fclose(f);
            return TY_E_FORMAT;
        }
        if (ty_parse_hex(e->hex, &e->d) != TY_OK) {
            fclose(f);
            return TY_E_FORMAT;
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
    if (n != NSEED) return TY_E_FORMAT;
    return ty_file_sha256(mpath, mdig);
}

static void make_split(ty_split *sp, const int *fit, int nf, const int *held, int nh) {
    memset(sp, 0, sizeof *sp);
    for (int i = 0; i < nf; ++i) {
        sp->fit[i] = M[fit[i]].d;
        snprintf(sp->fit_label[i], sizeof sp->fit_label[i], "%s", M[fit[i]].rel);
    }
    for (int i = 0; i < nh; ++i) {
        sp->held[i] = M[held[i]].d;
        snprintf(sp->held_label[i], sizeof sp->held_label[i], "%s", M[held[i]].rel);
    }
    sp->nfit = (size_t)nf;
    sp->nheld = (size_t)nh;
}

static ty_stream S[NSEED];
static int loaded[NSEED];

static int load_seed(int i) {
    if (loaded[i]) return TY_OK;
    struct stat st;
    if (stat(M[i].path, &st) != 0 || (uint64_t)st.st_size != M[i].size) {
        fprintf(stderr, "size mismatch or missing: %s\n", M[i].path);
        return TY_E_IO;
    }
    if (st.st_size % TY_CTR1_BYTES) return TY_E_FORMAT;
    char why[256] = "";
    ty_stream_init(&S[i]);
    int64_t n = ty_ctr1_read(M[i].path, &S[i], why, sizeof why);
    if (n < 0) {
        fprintf(stderr, "%s\n", why);
        return (int)n;
    }
    fprintf(stderr, "read %s: %lld records (size / 247 = %lld), %u crumbs, %llu index gaps\n", M[i].rel,
            (long long)n, (long long)st.st_size / TY_CTR1_BYTES, S[i].ncrumb, (unsigned long long)S[i].gaps);
    loaded[i] = 1;
    return TY_OK;
}

typedef struct {
    const char *name;
    unsigned mask;
    int uniform;
} rung;

static const rung BASE_GRID[] = {{"order-0", 0, 0}, {"order-1", TY_F_PREV1, 0}, {"op-only", TY_F_OP, 0}};
static const rung CAND_GRID[] = {
    {"op+prev1", TY_F_OP | TY_F_PREV1, 0},
    {"op+prev1..2", TY_F_OP | TY_F_PREV1 | TY_F_PREV2, 0},
    {"op+prev1..3", TY_F_OP | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3, 0},
    {"op+prev1..4", TY_F_OP | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4, 0},
    {"op+prev1..5", TY_F_OP | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4 | TY_F_PREV5, 0},
    {"op+depth+prev1", TY_F_OP | TY_F_DEPTH | TY_F_PREV1, 0},
    {"op+depth+prev1..2", TY_F_OP | TY_F_DEPTH | TY_F_PREV1 | TY_F_PREV2, 0},
    {"op+depth+prev1..3", TY_F_OP | TY_F_DEPTH | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3, 0},
    {"op+depth+prev1..4", TY_F_OP | TY_F_DEPTH | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4, 0},
    {"op+depth+prev1..5", TY_F_OP | TY_F_DEPTH | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4 | TY_F_PREV5, 0},
};

/* Fit a rung on the given loaded seeds; returns code length and data length on seed `score`. */
static int fit_score(const rung *r, const int *fit, int nf, const int *score, int ns, uint64_t *lm_bits,
                     int64_t *ld_ub, int64_t *per_file_ub, uint64_t *nsym) {
    ty_model m, d;
    int rc;
    if (r->uniform) {
        rc = ty_model_uniform(&m, TY_OUT_K);
    } else {
        const ty_stream *sv[NSEED];
        for (int i = 0; i < nf; ++i) sv[i] = &S[fit[i]];
        rc = ty_model_fit(&m, sv, (size_t)nf, r->mask, TY_OUT_K, TY_FIT_MDL_PRUNE);
    }
    if (rc != TY_OK) return rc;
    uint8_t *code = NULL;
    size_t nb = 0;
    rc = ty_model_encode(&m, &code, &nb, lm_bits);
    ty_model_free(&m);
    if (rc != TY_OK) return rc;
    rc = ty_model_decode(code, nb, &d, NULL, NULL, 0); /* score the decoded model */
    free(code);
    if (rc != TY_OK) return rc;
    *ld_ub = 0;
    *nsym = 0;
    for (int i = 0; rc == TY_OK && i < ns; ++i) {
        int64_t ub;
        uint64_t n;
        rc = ty_model_score(&d, &S[score[i]], &ub, &n);
        if (rc == TY_OK) rc = ty_add(*ld_ub, ub, ld_ub);
        if (per_file_ub) per_file_ub[i] = ub;
        *nsym += n;
    }
    ty_model_free(&d);
    return rc;
}

static double bits(int64_t ub) { return (double)ub / 1e6; }

static int cmd_tune(void) {
    int fit[6] = {0, 1, 2, 3, 4, 5}, val[1] = {6};
    for (int i = 0; i < NFIT; ++i)
        if (load_seed(i) != TY_OK) return 2;
    printf("# tune: fit seeds 1-6, validation seed 7; DL = L(M) + L(D|M) on seed 7\n");
    printf("%-28s %6s %14s %16s %16s %10s\n", "model", "mask", "L(M) bits", "L(D|M) bits", "DL bits", "bits/sym");
    const rung uni = {"uniform", 0, 1};
    const rung *all[16];
    int na = 0;
    all[na++] = &uni;
    for (size_t i = 0; i < sizeof BASE_GRID / sizeof *BASE_GRID; ++i) all[na++] = &BASE_GRID[i];
    for (size_t i = 0; i < sizeof CAND_GRID / sizeof *CAND_GRID; ++i) all[na++] = &CAND_GRID[i];
    int64_t best_b = INT64_MAX, best_c = INT64_MAX;
    const char *nb = "", *nc = "";
    unsigned mb = 0, mc = 0;
    for (int k = 0; k < na; ++k) {
        uint64_t lm, ns;
        int64_t ld, dl;
        int rc = fit_score(all[k], fit, 6, val, 1, &lm, &ld, NULL, &ns);
        if (rc != TY_OK || ty_dl(lm, ld, &dl) != TY_OK) return die("fit/score", rc);
        printf("%-28s %6u %14" PRIu64 " %16.3f %16.3f %10.6f\n", all[k]->name, all[k]->mask, lm, bits(ld), bits(dl),
               bits(dl) / (double)ns);
        int is_base = k >= 1 && k <= 3, is_cand = k >= 4;
        if (is_base && dl < best_b) best_b = dl, nb = all[k]->name, mb = all[k]->mask;
        if (is_cand && dl < best_c) best_c = dl, nc = all[k]->name, mc = all[k]->mask;
    }
    printf("chosen baseline: %s (mask %u)\nchosen candidate: %s (mask %u)\n", nb, mb, nc, mc);
    printf("validation T (candidate vs baseline) = %.3f bits\n", bits(best_b - best_c));
    return 0;
}

static int cmd_loso(void) {
    ty_yprofile p;
    ty_yprofile_v0(&p);
    for (int i = 0; i < NFIT; ++i)
        if (load_seed(i) != TY_OK) return 2;
    rung b = {"baseline", p.baseline_mask, 0}, c = {"candidate", p.candidate_mask, 0};
    printf("# loso: for each seed s in 1-7 fit B (mask %u) and C (mask %u) on the other six, score s\n",
           p.baseline_mask, p.candidate_mask);
    double t[NFIT], sum = 0;
    for (int s = 0; s < NFIT; ++s) {
        int fit[6], nf = 0, sc[1] = {s};
        for (int i = 0; i < NFIT; ++i)
            if (i != s) fit[nf++] = i;
        uint64_t lb, lc, ns;
        int64_t db, dc, xb, xc, g;
        int rc = fit_score(&b, fit, nf, sc, 1, &lb, &db, NULL, &ns);
        if (rc == TY_OK) rc = fit_score(&c, fit, nf, sc, 1, &lc, &dc, NULL, &ns);
        if (rc == TY_OK) rc = ty_dl(lb, db, &xb);
        if (rc == TY_OK) rc = ty_dl(lc, dc, &xc);
        if (rc == TY_OK) rc = ty_gain(xb, xc, &g);
        if (rc != TY_OK) return die("loso", rc);
        t[s] = bits(g);
        sum += t[s];
        printf("seed %d: nsym %" PRIu64 "  DL(B) %.3f  DL(C) %.3f  T %.3f bits  (%.6f bits/sym)\n", s + 1, ns,
               bits(xb), bits(xc), t[s], t[s] / (double)ns);
    }
    double mean = sum / NFIT, ss = 0;
    for (int s = 0; s < NFIT; ++s) ss += (t[s] - mean) * (t[s] - mean);
    double sd = sqrt(ss / (NFIT - 1));
    double margin = 3.0 * sqrt(3.0) * sd;
    printf("mean T %.3f bits, sample sd %.3f bits\n", mean, sd);
    printf("margin rule: 3 * sqrt(3) * sd = %.3f bits -> margin_ub %lld\n", margin,
           (long long)ceil(margin * 1e6));
    return 0;
}

static void write_kv_digest(FILE *f, const char *k, const ty_digest *d) {
    char h[65];
    ty_hex(d, h);
    fprintf(f, "%s=%s\n", k, h);
}

static int write_file(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return TY_E_IO;
    size_t w = fwrite(b, 1, n, f);
    int e = fclose(f);
    return (w == n && e == 0) ? TY_OK : TY_E_IO;
}

static void profile_with_manifest(ty_yprofile *p, const ty_digest *mdig) {
    ty_yprofile_v0(p);
    p->manifest = *mdig;
}

static int cmd_heldout(const ty_digest *mdig, const char *outdir) {
    char rpath[1200];
    snprintf(rpath, sizeof rpath, "%s/ty2_heldout_receipt.txt", outdir);
    struct stat st;
    if (stat(rpath, &st) == 0) {
        fprintf(stderr, "turing-yield: %s exists; the held-out seeds are scored once\n", rpath);
        return 2;
    }
    ty_yprofile p;
    profile_with_manifest(&p, mdig);
    int fit[NFIT] = {0, 1, 2, 3, 4, 5, 6}, held[3] = {7, 8, 9};
    ty_split sp;
    make_split(&sp, fit, NFIT, held, 3);
    const char *fp[NFIT], *hp[3];
    for (int i = 0; i < NFIT; ++i) fp[i] = M[fit[i]].path;
    for (int i = 0; i < 3; ++i) hp[i] = M[held[i]].path;
    char why[512] = "";
    ty_fit_rec br, cr;
    uint8_t *bc = NULL, *cc = NULL;
    size_t nb = 0, nc = 0;
    int rc = ty_fit_files(&p, &sp, fp, sp.fit, NFIT, TY_ROLE_BASELINE, p.baseline_mask, &br, &bc, &nb, why,
                          sizeof why);
    if (rc == TY_OK)
        rc = ty_fit_files(&p, &sp, fp, sp.fit, NFIT, TY_ROLE_CANDIDATE, p.candidate_mask, &cr, &cc, &nc, why,
                          sizeof why);
    if (rc != TY_OK) {
        fprintf(stderr, "%s\n", why);
        return die("fit", rc);
    }
    ty_gain_rec g;
    rc = ty_gain_compute(&p, &sp, &br, bc, nb, &cr, cc, nc, hp, &g, why, sizeof why);
    if (rc != TY_OK) {
        fprintf(stderr, "%s\n", why);
        return die("gain", rc);
    }
    ty_digest gd, pd, sd, bfd, cfd;
    ty_gain_digest(&g, &gd);
    ty_profile_digest(&p, &pd);
    ty_split_digest(&sp, &sd);
    ty_fit_digest(&br, &bfd);
    ty_fit_digest(&cr, &cfd);
    char mp[1200];
    snprintf(mp, sizeof mp, "%s/ty2_baseline.tym", outdir);
    if (write_file(mp, bc, nb) != TY_OK) return die("write baseline model", TY_E_IO);
    snprintf(mp, sizeof mp, "%s/ty2_candidate.tym", outdir);
    if (write_file(mp, cc, nc) != TY_OK) return die("write candidate model", TY_E_IO);
    free(bc);
    free(cc);

    /* Ladder: every rung fit on seeds 1-7 and scored on the held-out seeds (same single pass). */
    for (int i = 0; i < NSEED; ++i)
        if (load_seed(i) != TY_OK) return 2;
    FILE *f = fopen(rpath, "w");
    if (!f) return die("open receipt", TY_E_IO);
    fprintf(f, "# Turing Yield TY-2 held-out receipt (turing.yield.v0). Units: ub = micro-bits.\n");
    write_kv_digest(f, "manifest_sha256", mdig);
    write_kv_digest(f, "profile", &pd);
    write_kv_digest(f, "split", &sd);
    write_kv_digest(f, "baseline_fit_record", &bfd);
    write_kv_digest(f, "candidate_fit_record", &cfd);
    write_kv_digest(f, "baseline_model", &g.baseline_model);
    write_kv_digest(f, "candidate_model", &g.candidate_model);
    fprintf(f, "baseline_mask=%u\ncandidate_mask=%u\n", p.baseline_mask, p.candidate_mask);
    fprintf(f, "nsym=%" PRIu64 "\nlm_b_bits=%" PRIu64 "\nlm_c_bits=%" PRIu64 "\n", g.nsym, g.lm_b_bits, g.lm_c_bits);
    fprintf(f, "ld_b_ub=%" PRId64 "\nld_c_ub=%" PRId64 "\ndl_b_ub=%" PRId64 "\ndl_c_ub=%" PRId64 "\n", g.ld_b_ub,
            g.ld_c_ub, g.dl_b_ub, g.dl_c_ub);
    fprintf(f, "t_microbits=%" PRId64 "\nnheld=%zu\n", g.t_ub, g.nheld);
    for (size_t i = 0; i < g.nheld; ++i)
        fprintf(f, "file.%02zu=%" PRIu64 " %" PRId64 "\n", i, g.file_nsym[i], g.file_t_ub[i]);
    fprintf(f, "qerr_bound_ub=%" PRId64 "\nmargin_ub=%" PRId64 "\nverdict=%s\n", g.qerr_bound_ub, g.margin_ub,
            g.verdict);
    write_kv_digest(f, "gain_record", &gd);
    fprintf(f, "# ladder: fit seeds 1-7, held-out seeds 8-10; T vs B and vs order-0 charge each model's L(M)\n");
    const rung uni = {"uniform", 0, 1};
    const rung *lad[16];
    int na = 0;
    lad[na++] = &uni;
    for (size_t i = 0; i < sizeof BASE_GRID / sizeof *BASE_GRID; ++i) lad[na++] = &BASE_GRID[i];
    for (size_t i = 0; i < sizeof CAND_GRID / sizeof *CAND_GRID; ++i) lad[na++] = &CAND_GRID[i];
    int64_t dl0 = 0;
    for (int k = 0; k < na; ++k) {
        uint64_t lm, ns;
        int64_t ld, dl, pf[3];
        rc = fit_score(lad[k], fit, NFIT, held, 3, &lm, &ld, pf, &ns);
        if (rc != TY_OK || ty_dl(lm, ld, &dl) != TY_OK) {
            fclose(f);
            return die("ladder", rc);
        }
        if (k == 1) dl0 = dl;
        fprintf(f,
                "ladder.%s mask=%u lm_bits=%" PRIu64 " ld_ub=%" PRId64 " dl_ub=%" PRId64 " t_vs_B_ub=%" PRId64
                " t_vs_order0_ub=%" PRId64 " seed8_ub=%" PRId64 " seed9_ub=%" PRId64 " seed10_ub=%" PRId64 "\n",
                lad[k]->name, lad[k]->mask, lm, ld, dl, g.dl_b_ub - dl, k >= 1 ? dl0 - dl : 0, pf[0], pf[1], pf[2]);
    }
    fclose(f);
    printf("receipt %s\nverdict %s  T = %.3f bits (%.6f bits/symbol over %" PRIu64 " symbols)\n", rpath, g.verdict,
           bits(g.t_ub), bits(g.t_ub) / (double)g.nsym, g.nsym);
    return 0;
}

static int kv(const char *path, const char *key, char *val, size_t vl) {
    FILE *f = fopen(path, "r");
    if (!f) return TY_E_IO;
    char line[1024];
    size_t kl = strlen(key);
    int rc = TY_E_FORMAT;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            line[strcspn(line, "\n")] = 0;
            snprintf(val, vl, "%s", line + kl + 1);
            rc = TY_OK;
            break;
        }
    }
    fclose(f);
    return rc;
}

static int cmd_verify(const ty_digest *mdig, const char *receipt) {
    ty_yprofile p;
    profile_with_manifest(&p, mdig);
    int fit[NFIT] = {0, 1, 2, 3, 4, 5, 6}, held[3] = {7, 8, 9};
    ty_split sp;
    make_split(&sp, fit, NFIT, held, 3);
    ty_gain_rec g;
    memset(&g, 0, sizeof g);
    char v[512];
    ty_digest gd;
    int rc = TY_OK;
#define GETD(k, dst) \
    if (rc == TY_OK && (rc = kv(receipt, k, v, sizeof v)) == TY_OK) rc = ty_parse_hex(v, dst)
#define GETI(k, dst) \
    if (rc == TY_OK && (rc = kv(receipt, k, v, sizeof v)) == TY_OK) dst = strtoll(v, NULL, 10)
    GETD("profile", &g.profile);
    GETD("split", &g.split);
    GETD("baseline_fit_record", &g.baseline_fit);
    GETD("candidate_fit_record", &g.candidate_fit);
    GETD("baseline_model", &g.baseline_model);
    GETD("candidate_model", &g.candidate_model);
    GETD("gain_record", &gd);
    GETI("nsym", g.nsym);
    GETI("lm_b_bits", g.lm_b_bits);
    GETI("lm_c_bits", g.lm_c_bits);
    GETI("ld_b_ub", g.ld_b_ub);
    GETI("ld_c_ub", g.ld_c_ub);
    GETI("dl_b_ub", g.dl_b_ub);
    GETI("dl_c_ub", g.dl_c_ub);
    GETI("t_microbits", g.t_ub);
    GETI("nheld", g.nheld);
    GETI("qerr_bound_ub", g.qerr_bound_ub);
    GETI("margin_ub", g.margin_ub);
    if (rc == TY_OK && (rc = kv(receipt, "verdict", v, sizeof v)) == TY_OK)
        snprintf(g.verdict, sizeof g.verdict, "%.15s", v);
    for (size_t i = 0; rc == TY_OK && i < g.nheld && i < TY_MAX_FILES; ++i) {
        char k[32];
        snprintf(k, sizeof k, "file.%02zu", i);
        if ((rc = kv(receipt, k, v, sizeof v)) == TY_OK &&
            sscanf(v, "%" SCNu64 " %" SCNd64, &g.file_nsym[i], &g.file_t_ub[i]) != 2)
            rc = TY_E_FORMAT;
    }
    if (rc != TY_OK) return die("receipt parse", rc);
    const char *fp[NFIT], *hp[3];
    for (int i = 0; i < NFIT; ++i) fp[i] = M[fit[i]].path;
    for (int i = 0; i < 3; ++i) hp[i] = M[held[i]].path;
    char why[512] = "";
    rc = ty_gain_verify(&p, &sp, fp, hp, &g, &gd, why, sizeof why);
    if (rc != TY_OK) {
        printf("VERIFY FAIL: %s (%s)\n", why, ty_err_name(rc));
        return 1;
    }
    printf("VERIFY OK: receipt re-derived from the manifest files; T = %.3f bits, verdict %s\n", bits(g.t_ub),
           g.verdict);
    return 0;
}

static int cmd_profile(const ty_digest *mdig) {
    ty_yprofile p;
    profile_with_manifest(&p, mdig);
    int fit[NFIT] = {0, 1, 2, 3, 4, 5, 6}, held[3] = {7, 8, 9};
    ty_split sp;
    make_split(&sp, fit, NFIT, held, 3);
    ty_digest pd, sd;
    if (ty_profile_digest(&p, &pd) != TY_OK || ty_split_digest(&sp, &sd) != TY_OK) return die("digest", TY_E_ARG);
    write_kv_digest(stdout, "manifest_sha256", mdig);
    write_kv_digest(stdout, "profile", &pd);
    write_kv_digest(stdout, "split", &sd);
    printf("baseline_mask=%u\ncandidate_mask=%u\nmargin_ub=%lld\n", p.baseline_mask, p.candidate_mask,
           (long long)p.margin_ub);
    return 0;
}

static int cmd_manifest_check(void) {
    int bad = 0;
    for (int i = 0; i < NSEED; ++i) {
        ty_digest d;
        struct stat st;
        int ok = stat(M[i].path, &st) == 0 && (uint64_t)st.st_size == M[i].size && st.st_size % TY_CTR1_BYTES == 0 &&
                 ty_file_sha256(M[i].path, &d) == TY_OK && ty_digest_eq(&d, &M[i].d);
        printf("%s %s records=%llu\n", ok ? "OK  " : "FAIL", M[i].rel,
               (unsigned long long)(M[i].size / TY_CTR1_BYTES));
        bad += !ok;
    }
    return bad ? 1 : 0;
}

/* Deterministic printout on the committed fixture (plain vs ASan must match). */
static int cmd_fixture(const char *a, const char *b) {
    ty_stream fa, fb;
    ty_stream_init(&fa);
    ty_stream_init(&fb);
    char why[256] = "";
    if (ty_ctr1_read(a, &fa, why, sizeof why) < 0 || ty_ctr1_read(b, &fb, why, sizeof why) < 0) {
        fprintf(stderr, "%s\n", why);
        return 2;
    }
    const ty_stream *sv[1] = {&fa};
    const rung *all[16];
    const rung uni = {"uniform", 0, 1}, memo = {"memorizer", TY_F_POS, 0};
    int na = 0;
    all[na++] = &uni;
    for (size_t i = 0; i < sizeof BASE_GRID / sizeof *BASE_GRID; ++i) all[na++] = &BASE_GRID[i];
    for (size_t i = 0; i < sizeof CAND_GRID / sizeof *CAND_GRID; ++i) all[na++] = &CAND_GRID[i];
    all[na++] = &memo;
    printf("fixture fit %zu events %u crumbs; held %zu events %u crumbs\n", fa.n, fa.ncrumb, fb.n, fb.ncrumb);
    for (int k = 0; k < na; ++k) {
        ty_model m;
        int rc = all[k]->uniform ? ty_model_uniform(&m, TY_OUT_K)
                                 : ty_model_fit(&m, sv, 1, all[k]->mask, TY_OUT_K,
                                                all[k]->mask == TY_F_POS ? TY_FIT_KEEP_ALL : TY_FIT_MDL_PRUNE);
        uint8_t *code = NULL;
        size_t nb = 0;
        uint64_t lm = 0;
        if (rc == TY_OK) rc = ty_model_encode(&m, &code, &nb, &lm);
        int64_t in_ub = 0, out_ub = 0;
        if (rc == TY_OK) rc = ty_model_score(&m, &fa, &in_ub, NULL);
        if (rc == TY_OK) rc = ty_model_score(&m, &fb, &out_ub, NULL);
        if (rc != TY_OK) return die("fixture", rc);
        uint8_t d[32];
        ty_model_digest(code, nb, d);
        ty_digest dd;
        memcpy(dd.b, d, 32);
        char h[65];
        ty_hex(&dd, h);
        printf("%-28s rows=%zu lm_bits=%" PRIu64 " ld_fit_ub=%" PRId64 " ld_held_ub=%" PRId64 " model=%s\n",
               all[k]->name, m.nrows, lm, in_ub, out_ub, h);
        free(code);
        ty_model_free(&m);
    }
    ty_stream_free(&fa);
    ty_stream_free(&fb);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "fixture") == 0) return cmd_fixture(argv[2], argv[3]);
    if (argc < 4) {
        fprintf(stderr, "usage: turing-yield manifest-check|tune|loso|heldout|verify MANIFEST ROOT [OUT|RECEIPT]\n"
                        "       turing-yield fixture FIT.ctr HELD.ctr\n");
        return 2;
    }
    ty_digest mdig;
    int rc = load_manifest(argv[2], argv[3], &mdig);
    if (rc != TY_OK) return die("manifest", rc);
    if (strcmp(argv[1], "manifest-check") == 0) return cmd_manifest_check();
    if (strcmp(argv[1], "profile") == 0) return cmd_profile(&mdig);
    if (strcmp(argv[1], "tune") == 0) return cmd_tune();
    if (strcmp(argv[1], "loso") == 0) return cmd_loso();
    if (strcmp(argv[1], "heldout") == 0 && argc == 5) return cmd_heldout(&mdig, argv[4]);
    if (strcmp(argv[1], "verify") == 0 && argc == 5) return cmd_verify(&mdig, argv[4]);
    fprintf(stderr, "turing-yield: unknown command\n");
    return 2;
}
