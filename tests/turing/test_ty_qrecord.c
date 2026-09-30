/* Tests for the four companion records and the sidecar manifests. All data is
 * neutral dummy text and pseudo-random digests. */
#include "turing/ty_qrecord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint64_t lcg = 0x243f6a8885a308d3ULL;
static ty_digest rnd(void) {
    ty_digest d;
    for (int i = 0; i < 32; ++i) {
        lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
        d.b[i] = (uint8_t)(lcg >> 56);
    }
    return d;
}

static void S(char *dst, const char *s) { snprintf(dst, TYQR_VAL, "%s", s); }

static tyqr_profile mk_profile(void) {
    tyqr_profile p;
    memset(&p, 0, sizeof p);
    S(p.profile, "dummy.profile.v9");
    p.spec = rnd(); p.packet = rnd();
    S(p.protocol, "dummy.line.v9"); S(p.quantizer, "dummyq.v9");
    p.delta_log2 = -7;
    S(p.edge_rule, "dummy.edge");
    p.sd_min_log2 = -3;
    S(p.floor_rule, "dummy.floor.rule");
    S(p.families, "0 1");
    p.kmax = 3; p.n_points = 64; p.split = 32;
    p.cells = rnd();
    S(p.rider, "dummy.rider"); S(p.lm_rule, "dummy.lm.rule.id");
    S(p.baselines, "X1:0x01 X2:0x02");
    p.thresholds = rnd();
    S(p.thresholds_format, "dummy.thr.v9"); S(p.gen_version, "dummy.gen.v9");
    p.bootstrap_b = 100;
    S(p.unit, "micro-bits (1 T = 1 bit = 1000000 ub)");
    return p;
}

static tyqr_world mk_world(const ty_digest *prof, const ty_digest *frz) {
    tyqr_world w;
    memset(&w, 0, sizeof w);
    w.qprofile = *prof;
    S(w.mode, "certification"); S(w.parent_kind, "sealed");
    w.qfreeze = *frz;
    S(w.cell, "cellA"); w.block = 4;
    S(w.label, "dummy-label-bytes");
    S(w.gen_version, "dummy.gen.v9");
    w.generator = rnd();
    w.rep_first = 0; w.rep_count = 20;
    w.traj_manifest = rnd();
    S(w.traj_format, "dummy.traj.v9");
    return w;
}

static tyqr_freeze mk_freeze(const ty_digest *prof, int disc) {
    tyqr_freeze f;
    memset(&f, 0, sizeof f);
    S(f.mode, disc ? "discovery" : "certification");
    f.artifact = rnd();
    S(f.artifact_kind, disc ? "cand.c" : "ref.c");
    if (disc) f.artifact_bin = rnd();
    f.qprofile = *prof; f.dev_manifest = rnd();
    S(f.b2_emerging_label, "AMBIGUOUS"); S(f.bayes_plugin_check, "AGREE");
    S(f.diag_fw_rate_ppm, "cellA:10 cellB:20");
    S(f.lengthening_test, "PASS"); S(f.stability_test, "FAIL");
    S(f.calibration_extra, "-");
    memcpy(f.run_commit, "0123456789abcdef0123456789abcdef01234567", 41);
    f.tree_dirty = 0;
    S(f.base_commits, "A=0123456789abcdef0123456789abcdef01234567 B=89abcdef0123456789abcdef0123456789abcdef");
    S(f.producer_tool, disc ? "tool 1.0" : "none");
    S(f.producer_model, disc ? "model-x" : "none");
    if (disc) { f.prompt_digest = rnd(); f.transcript_digest = rnd(); }
    f.n_lineage = 2;
    f.lineage[0] = rnd(); f.lineage[1] = rnd();
    return f;
}

static tyqr_gain mk_gain(const ty_digest *prof, const ty_digest *wd, const ty_digest *fd, const ty_digest *cand) {
    tyqr_gain g;
    memset(&g, 0, sizeof g);
    g.qprofile = *prof; g.qworld = *wd; g.qfreeze = *fd;
    S(g.mode, "certification"); S(g.cell, "cellA"); S(g.gen_version, "dummy.gen.v9");
    g.baseline_opcode = 4; g.baseline_artifact = rnd();
    g.candidate = *cand; g.candidate_opcode = 2;
    g.prd_b = rnd(); g.prd_m = rnd();
    g.official = 1; S(g.ladder_item, "-");
    int64_t L[6] = {100, 2000, 90, 1900, 2100, 1990};
    memcpy(g.lengths, L, sizeof L);
    g.t_ub = 110; g.t_lo_ub = 10; g.t_hi_ub = 200;
    g.numeric_bound_ub = 3; g.mc_err_ub = 4;
    g.counts[0] = 5; g.counts[1] = 6; g.counts[2] = 7; g.counts[3] = 8;
    g.floor_hits = 1; g.protocol_failures = 0;
    S(g.leak_flag, "0");
    g.coverage[0] = 900000; g.coverage[1] = -1; g.coverage[2] = 5;
    S(g.diag_warnings, "-"); S(g.classification, "POSITIVE"); S(g.status, "PASS");
    S(g.unit, "micro-bits (1 T = 1 bit = 1000000 ub)");
    return g;
}

/* Known-answer digests, computed once from the fixed generator above and pinned. */
static const char *KAT_PROFILE = "b38414c1d33f8c93545fb9ba44e6cbb4340ed86492aae50c06a2137696924bd2";
static const char *KAT_WORLD = "f3f6c539d7882949feb094951d6216adab5c1ded8787658f46d7e27b836e1cef";
static const char *KAT_FREEZE_C = "6d789fc90cfcaf23435f68ed0876e4b0fb013e1556c0d39cb4b039bf0a3c925b";
static const char *KAT_FREEZE_D = "a2955bbd58d0b956b0d33411f2496e61edc6cd34457b4b3bc24670963514e754";
static const char *KAT_GAIN = "d95b64763b483519d45c2ae22a3fcac04c326205e09c5a588948314667ce973f";

static int show;
static void kat(const char *name, const ty_digest *d, const char *pinned) {
    char h[65];
    ty_hex(d, h);
    if (show) printf("KAT %s %s\n", name, h);
    else CHECK(strcmp(h, pinned) == 0);
}

#define FLIP_REFUSED(rec, fn, mut) do { __typeof__(rec) t_ = (rec); ty_digest d_; mut; CHECK(fn(&t_, &d_) == TY_E_FORMAT); } while (0)

static void test_records(void) {
    lcg = 0x243f6a8885a308d3ULL;
    tyqr_profile p = mk_profile();
    ty_digest pd, pd2;
    CHECK(tyqr_profile_digest(&p, &pd) == TY_OK);
    CHECK(tyqr_profile_digest(&p, &pd2) == TY_OK && ty_digest_eq(&pd, &pd2));
    CHECK(tyqr_profile_verify(&p, &pd, NULL, 0) == TY_OK);
    kat("profile", &pd, KAT_PROFILE);

    tyqr_freeze fc = mk_freeze(&pd, 0), fd = mk_freeze(&pd, 1);
    ty_digest fcd, fdd, wd, gd;
    CHECK(tyqr_freeze_digest(&fc, &fcd) == TY_OK);
    CHECK(tyqr_freeze_digest(&fd, &fdd) == TY_OK);
    kat("freeze_cert", &fcd, KAT_FREEZE_C);
    kat("freeze_disc", &fdd, KAT_FREEZE_D);
    CHECK(!ty_digest_eq(&fcd, &fdd));

    tyqr_world w = mk_world(&pd, &fcd);
    CHECK(tyqr_world_digest(&w, &wd) == TY_OK);
    kat("world", &wd, KAT_WORLD);
    tyqr_gain g = mk_gain(&pd, &wd, &fcd, &fc.artifact);
    CHECK(tyqr_gain_digest(&g, &gd) == TY_OK);
    kat("gain", &gd, KAT_GAIN);
    CHECK(tyqr_gain_bind(&g, &gd, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_OK);

    /* every field change moves the digest (spot checks across kinds) */
    { tyqr_profile t = p; t.kmax++; ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &pd)); }
    { tyqr_profile t = p; t.delta_log2--; ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &pd)); }
    { tyqr_profile t = p; S(t.families, "0 2"); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &pd)); }
    { tyqr_profile t = p; t.thresholds.b[5] ^= 1; ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &pd)); }
    { tyqr_world t = w; t.block++; ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &wd)); }
    { tyqr_gain t = g; t.mc_err_ub++; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &gd)); }
    { tyqr_gain t = g; t.coverage[1] = 0; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK && !ty_digest_eq(&d, &gd)); }

    /* a dev world: mode dev, parent dev, zero freeze */
    { tyqr_world t = mk_world(&pd, &fcd); S(t.mode, "dev"); S(t.parent_kind, "dev"); memset(&t.qfreeze, 0, 32);
      ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_OK); }

    /* ---- verify: tamper of a field is a digest mismatch ---- */
    { tyqr_profile t = p; t.bootstrap_b++; char why[160]; CHECK(tyqr_profile_verify(&t, &pd, why, sizeof why) == TY_E_DIGEST); }
    { tyqr_world t = w; t.rep_count++; CHECK(tyqr_world_verify(&t, &wd, NULL, 0) == TY_E_DIGEST); }
    { tyqr_freeze t = fc; t.n_lineage = 1; CHECK(tyqr_freeze_verify(&t, &fcd, NULL, 0) == TY_E_DIGEST); }
    { tyqr_gain t = g; t.t_hi_ub++; CHECK(tyqr_gain_verify(&t, &gd, NULL, 0) == TY_E_DIGEST); }
    { tyqr_gain t = g; ty_digest bad = gd; bad.b[0] ^= 1; CHECK(tyqr_gain_verify(&t, &bad, NULL, 0) == TY_E_DIGEST); }
    CHECK(tyqr_gain_verify(&g, NULL, NULL, 0) == TY_E_ARG);
    CHECK(tyqr_gain_digest(NULL, &gd) == TY_E_ARG);

    /* ---- refusals: profile ---- */
    { tyqr_profile t = p; memset(t.rider, 'x', TYQR_VAL); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); } /* 513, no NUL */
    { tyqr_profile t = p; memset(t.rider, 'x', 512); t.rider[512] = 0; ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_OK); } /* 512 ok */
    { tyqr_profile t = p; t.rider[0] = 0; ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); } /* empty */
    { tyqr_profile t = p; S(t.rider, "a\tb"); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_profile t = p; S(t.rider, "a\xc3\xa9"); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_profile t = p; S(t.families, "0  2"); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_profile t = p; S(t.families, " 0 2"); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_profile t = p; S(t.baselines, "a b "); ty_digest d; CHECK(tyqr_profile_digest(&t, &d) == TY_E_FORMAT); }

    /* ---- refusals: world ---- */
    { tyqr_world t = w; S(t.mode, "other"); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_world t = w; S(t.parent_kind, "x"); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_world t = w; S(t.mode, "dev"); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); }         /* dev mode, sealed parent */
    { tyqr_world t = w; S(t.parent_kind, "dev"); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); }   /* dev parent, cert mode */
    { tyqr_world t = w; memset(&t.qfreeze, 0, 32); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); } /* sealed, no freeze */
    { tyqr_world t = mk_world(&pd, &fcd); S(t.mode, "dev"); S(t.parent_kind, "dev"); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); } /* dev with freeze */
    { tyqr_world t = w; S(t.cell, "two words"); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_E_FORMAT); }

    /* ---- refusals: freeze ---- */
    { tyqr_freeze t = fc; S(t.mode, "dev"); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; t.artifact_bin = rnd(); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fd; memset(&t.artifact_bin, 0, 32); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; S(t.b2_emerging_label, "MAYBE"); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; S(t.bayes_plugin_check, "agree"); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; S(t.lengthening_test, "OK"); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; S(t.stability_test, "pass"); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; t.run_commit[3] = 'G'; ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; t.run_commit[3] = 'A'; ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); } /* uppercase */
    { tyqr_freeze t = fc; t.run_commit[39] = 0; ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }  /* short */
    { tyqr_freeze t = fc; t.tree_dirty = 2; ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; S(t.producer_tool, "tool"); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; t.prompt_digest = rnd(); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fd; memset(&t.transcript_digest, 0, 32); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; t.n_lineage = 9; ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_freeze t = fc; t.n_lineage = 8; for (int i = 0; i < 8; ++i) t.lineage[i] = rnd(); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_OK); } /* 8 ok: 29 attributes */
    { tyqr_freeze t = fc; t.n_lineage = 0; ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_OK); }

    /* ---- refusals: gain ---- */
    { tyqr_gain t = g; S(t.mode, "dev"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.baseline_opcode = 0; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.baseline_opcode = 8; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.candidate_opcode = 255; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }   /* cert with 255 */
    { tyqr_gain t = g; t.candidate_opcode = 0; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.candidate_opcode = 8; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; S(t.mode, "discovery"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }    /* discovery needs 255 */
    { tyqr_gain t = g; S(t.mode, "discovery"); t.candidate_opcode = 255; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK); }
    { tyqr_gain t = g; t.official = 2; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; S(t.ladder_item, "I-1"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }   /* official with item */
    { tyqr_gain t = g; t.official = 0; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }             /* ladder without item */
    { tyqr_gain t = g; t.official = 0; S(t.ladder_item, "I-1"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK); }
    { tyqr_gain t = g; t.t_ub++; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.lengths[4] = INT64_MAX; t.lengths[5] = -1; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); } /* overflow */
    { tyqr_gain t = g; t.t_lo_ub = 300; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.protocol_failures = 1; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.protocol_failures = 1; S(t.status, "STOPPED"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK); }
    { tyqr_gain t = g; S(t.status, "FAIL_X"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK); }
    { tyqr_gain t = g; S(t.status, "FAILED"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; S(t.status, "OK"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; S(t.leak_flag, "2"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; S(t.leak_flag, "INCONCLUSIVE"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK); }
    { tyqr_gain t = g; S(t.classification, "GOOD"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; t.coverage[0] = -2; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }
    { tyqr_gain t = g; S(t.diag_warnings, "d1  d2"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_E_FORMAT); }

    /* ---- gain binding ---- */
    { ty_digest other = rnd(); CHECK(tyqr_gain_bind(&g, &gd, &w, &wd, &fc, &fcd, &other, NULL, 0) == TY_E_PROFILE); } /* different P */
    { tyqr_gain t = g; t.qprofile = rnd(); ty_digest d, wrong = t.qprofile; CHECK(tyqr_gain_digest(&t, &d) == TY_OK);
      char why[160]; CHECK(tyqr_gain_bind(&t, &d, &w, &wd, &fc, &fcd, &pd, why, sizeof why) == TY_E_PROFILE); (void)wrong; }
    { tyqr_world t = w; t.qprofile = rnd(); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&g, &gd, &t, &d, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    { tyqr_freeze t = fc; t.qprofile = rnd(); ty_digest d; CHECK(tyqr_freeze_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&g, &gd, &w, &wd, &t, &d, &pd, NULL, 0) == TY_E_PROFILE); }
    { tyqr_gain t = g; S(t.cell, "cellB"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&t, &d, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    { tyqr_gain t = g; S(t.gen_version, "other.v9"); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&t, &d, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    { tyqr_gain t = g; t.candidate = rnd(); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&t, &d, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    { tyqr_gain t = g; t.qworld = rnd(); ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&t, &d, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    { tyqr_world t = w; t.qfreeze = rnd(); ty_digest d; CHECK(tyqr_world_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&g, &gd, &t, &d, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    { /* mode disagreement: discovery gain over a certification world */
      tyqr_gain t = g; S(t.mode, "discovery"); t.candidate_opcode = 255; ty_digest d; CHECK(tyqr_gain_digest(&t, &d) == TY_OK);
      CHECK(tyqr_gain_bind(&t, &d, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_E_PROFILE); }
    /* a stale digest is caught before binding */
    { tyqr_gain t = g; t.mc_err_ub++; CHECK(tyqr_gain_bind(&t, &gd, &w, &wd, &fc, &fcd, &pd, NULL, 0) == TY_E_DIGEST); }
}

static void test_files(void) {
    lcg = 0x13198a2e03707344ULL;
    char dir[] = "/tmp/tyqrXXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    tyqr_profile p = mk_profile();
    ty_digest d, d2;
    char path[1024], path2[1024];
    CHECK(tyqr_profile_write(dir, &p, &d, path, sizeof path) == TY_OK);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0444 && st.st_size > 0);
    char h[65];
    ty_hex(&d, h);
    CHECK(strstr(path, h) != NULL && strstr(path, "turing.qprofile.") != NULL);
    CHECK(tyqr_file_verify(path, TYQR_DOMAIN_PROFILE, &d) == TY_OK);
    CHECK(tyqr_file_verify(path, TYQR_DOMAIN_WORLD, &d) == TY_E_DIGEST); /* wrong domain */
    /* second write of the same record is refused and leaves the file alone */
    CHECK(tyqr_profile_write(dir, &p, &d2, path2, sizeof path2) == TYQR_E_EXISTS);
    CHECK(ty_digest_eq(&d, &d2) && strcmp(path, path2) == 0);
    /* refusal writes nothing */
    { tyqr_profile t = p; S(t.unit, ""); CHECK(tyqr_profile_write(dir, &t, NULL, NULL, 0) == TY_E_FORMAT); }
    /* tamper: flip one byte of a copy -> verify fails */
    FILE *f = fopen(path, "rb");
    uint8_t buf[TYQR_CANON_CAP];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    CHECK(n > 10);
    for (size_t pos = 0; pos < n; pos += (n / 7) + 1) {
        char tp[1100];
        snprintf(tp, sizeof tp, "%s/tamper.bin", dir);
        buf[pos] ^= 0x01;
        f = fopen(tp, "wb");
        CHECK(f && fwrite(buf, 1, n, f) == n);
        if (f) fclose(f);
        buf[pos] ^= 0x01;
        CHECK(tyqr_file_verify(tp, TYQR_DOMAIN_PROFILE, &d) == TY_E_DIGEST);
        unlink(tp);
    }
    CHECK(tyqr_file_verify("/nonexistent/qq", TYQR_DOMAIN_PROFILE, &d) == TY_E_IO);
    /* the other three writers */
    tyqr_freeze fr = mk_freeze(&d, 1);
    ty_digest fd, wd, gd;
    CHECK(tyqr_freeze_write(dir, &fr, &fd, path, sizeof path) == TY_OK && tyqr_file_verify(path, TYQR_DOMAIN_FREEZE, &fd) == TY_OK);
    tyqr_world w = mk_world(&d, &fd);
    CHECK(tyqr_world_write(dir, &w, &wd, path, sizeof path) == TY_OK && tyqr_file_verify(path, TYQR_DOMAIN_WORLD, &wd) == TY_OK);
    tyqr_gain g = mk_gain(&d, &wd, &fd, &fr.artifact);
    S(g.mode, "certification");
    CHECK(tyqr_gain_write(dir, &g, &gd, path, sizeof path) == TY_OK && tyqr_file_verify(path, TYQR_DOMAIN_GAIN, &gd) == TY_OK);
    CHECK(tyqr_gain_write(dir, &g, NULL, NULL, 0) == TYQR_E_EXISTS);
    CHECK(tyqr_gain_write("/nonexistent/dir", &g, NULL, NULL, 0) == TY_E_IO);
    /* cleanup */
    char cmd[1100];
    snprintf(cmd, sizeof cmd, "chmod -R u+w %s && rm -rf %s", dir, dir);
    CHECK(system(cmd) == 0);
}

static void test_manifests(void) {
    lcg = 0xa4093822299f31d0ULL;
    tyqr_repline e[3] = {{0, rnd()}, {1, rnd()}, {10, rnd()}}, o[4];
    char buf[1024];
    size_t len = 0, n = 0;
    CHECK(tyqr_repman_build(e, 3, buf, sizeof buf, &len) == TY_OK);
    CHECK(buf[len - 1] == '\n' && buf[0] == '0' && buf[1] == ' ');
    CHECK(tyqr_repman_parse(buf, len, o, 4, &n) == TY_OK && n == 3 && o[2].rep == 10 && ty_digest_eq(&o[1].d, &e[1].d));
    ty_digest sd, sd2;
    tyqr_sidecar_digest(buf, len, &sd);
    tyqr_sidecar_digest(buf, len - 1, &sd2);
    CHECK(!ty_digest_eq(&sd, &sd2));
    /* known answer for the sidecar digest: SHA-256("abc\n") style check via fixed line */
    {
        static const char one[] = "0 0000000000000000000000000000000000000000000000000000000000000000\n";
        ty_digest k; char h[65];
        tyqr_sidecar_digest(one, sizeof one - 1, &k);
        ty_hex(&k, h);
        if (show) printf("KAT sidecar %s\n", h); else CHECK(strcmp(h, "364442899a250207173c7169918c4d5d49a9c60073f5be62111926b33144c2fc") == 0);
    }
    /* build refusals */
    { tyqr_repline b[2] = {{1, rnd()}, {1, rnd()}}; CHECK(tyqr_repman_build(b, 2, buf, sizeof buf, &len) == TY_E_FORMAT); }
    { tyqr_repline b[2] = {{2, rnd()}, {1, rnd()}}; CHECK(tyqr_repman_build(b, 2, buf, sizeof buf, &len) == TY_E_FORMAT); }
    CHECK(tyqr_repman_build(e, 3, buf, 100, &len) == TY_E_FORMAT); /* buffer too small */
    CHECK(tyqr_repman_build(e, 0, buf, sizeof buf, &len) == TY_E_ARG);
    /* parse refusals */
    tyqr_repline q[8];
    char good[1024];
    CHECK(tyqr_repman_build(e, 2, good, sizeof good, &len) == TY_OK);
    { char t[1024]; memcpy(t, good, len); CHECK(tyqr_repman_parse(t, len - 1, q, 8, &n) == TY_E_FORMAT); }      /* no final LF */
    { char t[1024]; memcpy(t, good, len); t[len] = '\n'; CHECK(tyqr_repman_parse(t, len + 1, q, 8, &n) == TY_E_FORMAT); } /* blank line */
    { char t[1024]; memcpy(t, good, len); t[1] = '\t'; CHECK(tyqr_repman_parse(t, len, q, 8, &n) == TY_E_FORMAT); }
    { char t[1024]; memcpy(t, good, len); t[10] = 'G'; CHECK(tyqr_repman_parse(t, len, q, 8, &n) == TY_E_FORMAT); }
    { char t[1024]; memcpy(t, good, len); t[10] = 'A'; CHECK(tyqr_repman_parse(t, len, q, 8, &n) == TY_E_FORMAT); } /* uppercase hex */
    { char t[1024]; memcpy(t, good, len); t[len - 2] = '\r'; t[len - 1] = '\n'; CHECK(tyqr_repman_parse(t, len, q, 8, &n) == TY_E_FORMAT); }
    { char t[1024]; int k = snprintf(t, sizeof t, "01 %s\n", "0000000000000000000000000000000000000000000000000000000000000000"); CHECK(tyqr_repman_parse(t, (size_t)k, q, 8, &n) == TY_E_FORMAT); } /* leading zero */
    { char t[1024]; int k = snprintf(t, sizeof t, "0  %s\n", "0000000000000000000000000000000000000000000000000000000000000000"); CHECK(tyqr_repman_parse(t, (size_t)k, q, 8, &n) == TY_E_FORMAT); } /* double space */
    { char t[1024]; int k = snprintf(t, sizeof t, "0 %s \n", "0000000000000000000000000000000000000000000000000000000000000000"); CHECK(tyqr_repman_parse(t, (size_t)k, q, 8, &n) == TY_E_FORMAT); } /* trailing space */
    { char t[1024]; int k = snprintf(t, sizeof t, "99999999999999999999999 %s\n", "0000000000000000000000000000000000000000000000000000000000000000"); CHECK(tyqr_repman_parse(t, (size_t)k, q, 8, &n) == TY_E_FORMAT); }
    { char t[2048]; memcpy(t, good, len); memcpy(t + len, good, len); CHECK(tyqr_repman_parse(t, 2 * len, q, 8, &n) == TY_E_FORMAT); } /* rep repeats */
    CHECK(tyqr_repman_parse(good, len, q, 1, &n) == TY_E_FORMAT); /* more lines than capacity */
    CHECK(tyqr_repman_parse(good, 0, q, 8, &n) == TY_E_FORMAT);

    /* devman */
    tyqr_devline dv[3], dq[4];
    memset(dv, 0, sizeof dv);
    snprintf(dv[0].cell, sizeof dv[0].cell, "cellA"); dv[0].block = 0; dv[0].d = rnd();
    snprintf(dv[1].cell, sizeof dv[1].cell, "cellA"); dv[1].block = 2; dv[1].d = rnd();
    snprintf(dv[2].cell, sizeof dv[2].cell, "cellB"); dv[2].block = 0; dv[2].d = rnd();
    CHECK(tyqr_devman_build(dv, 3, buf, sizeof buf, &len) == TY_OK);
    CHECK(strncmp(buf, "cellA 0 ", 8) == 0 && buf[len - 1] == '\n');
    CHECK(tyqr_devman_parse(buf, len, dq, 4, &n) == TY_OK && n == 3 && strcmp(dq[2].cell, "cellB") == 0 && dq[1].block == 2);
    { tyqr_devline b[2]; memcpy(b, dv, sizeof b); b[1].block = 0; CHECK(tyqr_devman_build(b, 2, buf, sizeof buf, &len) == TY_E_FORMAT); }
    { tyqr_devline b[2]; memcpy(b, dv, sizeof b); b[0] = dv[2]; b[1] = dv[0]; CHECK(tyqr_devman_build(b, 2, buf, sizeof buf, &len) == TY_E_FORMAT); }
    { tyqr_devline b[1]; memcpy(b, dv, sizeof b); snprintf(b[0].cell, sizeof b[0].cell, "a b"); CHECK(tyqr_devman_build(b, 1, buf, sizeof buf, &len) == TY_E_FORMAT); }
    { tyqr_devline b[1]; memcpy(b, dv, sizeof b); memset(b[0].cell, 'x', sizeof b[0].cell); CHECK(tyqr_devman_build(b, 1, buf, sizeof buf, &len) == TY_E_FORMAT); }
    { /* "cell10" sorts before "cell2" in byte order */
      tyqr_devline b[2]; memset(b, 0, sizeof b);
      snprintf(b[0].cell, sizeof b[0].cell, "cell2"); snprintf(b[1].cell, sizeof b[1].cell, "cell10");
      CHECK(tyqr_devman_build(b, 2, buf, sizeof buf, &len) == TY_E_FORMAT);
      snprintf(b[0].cell, sizeof b[0].cell, "cell10"); snprintf(b[1].cell, sizeof b[1].cell, "cell2");
      CHECK(tyqr_devman_build(b, 2, buf, sizeof buf, &len) == TY_OK); }
    { const char *s = "cellA 0\n"; CHECK(tyqr_devman_parse(s, strlen(s), dq, 4, &n) == TY_E_FORMAT); }

    /* cell table and thresholds format checks */
    { const char *s = "c1 a=1 b=2\nc2 a=3\n"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_OK); }
    { const char *s = "c2 a=1\nc1 a=3\n"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "c1 a=1\nc1 a=3\n"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "c1\n"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "c1 a1\n"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "c1 a=1"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "c1 a=1\nc10 a=1\n"; CHECK(tyqr_celltab_check(s, strlen(s)) == TY_OK); }
    { const char *s = "i1 f 0.5\ni2 f 7\n"; CHECK(tyqr_thrtab_check(s, strlen(s)) == TY_OK); }
    { const char *s = "i2 f 7\ni1 f 0.5\n"; CHECK(tyqr_thrtab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "i1 f\n"; CHECK(tyqr_thrtab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "i1 f 1 2\n"; CHECK(tyqr_thrtab_check(s, strlen(s)) == TY_E_FORMAT); }
    { const char *s = "i1 f 1\ni1 f 1\n"; CHECK(tyqr_thrtab_check(s, strlen(s)) == TY_E_FORMAT); }
    CHECK(tyqr_thrtab_check("", 0) == TY_E_FORMAT);
    CHECK(tyqr_thrtab_check(NULL, 5) == TY_E_FORMAT);
}

int main(int argc, char **argv) {
    show = argc > 1 && strcmp(argv[1], "--print") == 0;
    test_records();
    test_files();
    test_manifests();
    if (show) return 0;
    printf("test_ty_qrecord: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
