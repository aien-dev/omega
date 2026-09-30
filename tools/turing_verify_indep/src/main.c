/* indep-scorer: lane D independent scorer for EXP-001.
 *
 * Usage: indep-scorer --docs DOCS_DIR --bundle-root R --cand-dir DIR [--cand-dir DIR ...]
 *                     --out scorer_independent.json --details details.json
 *
 * R is the run root (calibration/docs/DATA_FORMAT.md section 4): R/dataset_manifest.json names the CTR1 files,
 * R/bundle holds the primary bundle and its INDEX files, R/work the large streams. dry_run in the output is
 * false only when the dataset manifest split is sealed_test.
 * Candidates (names, files, file SHA-256, model digest, L(M)) come from DOCS/candidate_manifest.json;
 * the baseline is B2_order1 (profile baseline_family). Everything else is recomputed from the artifacts. */
#include "core.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXC 16
#define MAXF 16
typedef struct { char name[64], file[128], file_sha[65], model_digest[65]; uint64_t lm_bits, bytes; } cand_t;
typedef struct { int g, index; char seed[64], path[1024], sha[65]; is_events ev; } file_t;
typedef struct { char tps[1024], tsy[1024], tcr[1024], tca[1024]; int present; } stream_t;
typedef struct {                      /* per (candidate, file) */
    int64_t ideal_ub, ideal_alt_ub; uint64_t range_bytes, rans_bytes;
    int tps_match, tsy_match, range_ok, rans_ok, binding_ok; char tps_digest[65], tsy_digest[65], tsy_note[160], tps_note[256];
    uint64_t alt_diff_events, rows_hit;
    int64_t *crumb_ub;                /* per crumb of the file */
} res_t;

static cand_t C[MAXC]; static int nc;
static file_t F[MAXF]; static int nf;
static stream_t ST[MAXC][MAXF];
static res_t R[MAXC][MAXF];
static int64_t ub_alt[65537];
static char profile_hex[65]; static uint8_t profile_dig[32];
static int problems = 0;

static void die(const char *m) { fprintf(stderr, "indep-scorer: %s\n", m); exit(2); }
static void note(const char *fmt, const char *a, const char *b) { fprintf(stderr, "PROBLEM: "); fprintf(stderr, fmt, a, b); fprintf(stderr, "\n"); problems++; }

static int jstr(const char *line, const char *key, char *out, size_t n) {
    char pat[80]; const char *p, *e; snprintf(pat, sizeof pat, "\"%s\": \"", key);
    if (!(p = strstr(line, pat))) return -1;
    p += strlen(pat); e = strchr(p, '"'); if (!e || (size_t)(e - p) >= n) return -1;
    memcpy(out, p, (size_t)(e - p)); out[e - p] = 0; return 0;
}
static int jnum(const char *line, const char *key, uint64_t *v) {
    char pat[80]; const char *p; snprintf(pat, sizeof pat, "\"%s\": ", key);
    if (!(p = strstr(line, pat))) return -1;
    *v = strtoull(p + strlen(pat), NULL, 10); return 0;
}
static void load_manifest(const char *docs) {
    char p[1024], line[4096]; FILE *f; snprintf(p, sizeof p, "%s/candidate_manifest.json", docs);
    if (!(f = fopen(p, "r"))) die("cannot open candidate_manifest.json");
    while (fgets(line, sizeof line, f)) {
        cand_t c; memset(&c, 0, sizeof c);
        if (!strstr(line, "\"lm_bits\"") || jstr(line, "name", c.name, sizeof c.name)) continue;
        if (jstr(line, "file", c.file, sizeof c.file) || jstr(line, "file_sha256", c.file_sha, sizeof c.file_sha) ||
            jstr(line, "model_digest", c.model_digest, sizeof c.model_digest) || jnum(line, "lm_bits", &c.lm_bits) || jnum(line, "bytes", &c.bytes))
            die("malformed candidate line");
        if (nc == MAXC) die("too many candidates");
        C[nc++] = c;
    }
    fclose(f);
}
static int cand_index(const char *n) { int i; for (i = 0; i < nc; i++) if (!strcmp(C[i].name, n)) return i; return -1; }
static int file_index(int g, int idx) { int i; for (i = 0; i < nf; i++) if (F[i].g == g && F[i].index == idx) return i; return -1; }

/* Bundle loading (EVALUATOR.md section 6: the scorer reads the bundle directory and the CTR1 files named in
 * its dataset manifest). Files come from dataset_manifest.json "files"; TPS1 paths and expected trailer
 * digests from bundle/probability_streams/INDEX; TCR1/TCA1 paths from bundle/encoded_artifacts/INDEX.
 * TSY1 paths are not listed in any INDEX: work/symbols/g<g>_j<j>_<C>.tsy is the observed naming (SPEC_GAPS). */
static char idx_tps_trailer[MAXC][MAXF][65], idx_tsy_trailer[MAXC][MAXF][65];
static void load_bundle(const char *root) {
    char p[2048], line[4096]; FILE *f;
    snprintf(p, sizeof p, "%s/dataset_manifest.json", root);
    if (!(f = fopen(p, "r"))) die("cannot open dataset_manifest.json");
    while (fgets(line, sizeof line, f)) {
        file_t *x; uint64_t gg, ii, ss;
        if (!strstr(line, "\"group\"")) continue;
        if (nf == MAXF) die("too many files");
        x = &F[nf]; memset(x, 0, sizeof *x);
        if (jnum(line, "group", &gg) || jnum(line, "index", &ii) || jnum(line, "seed", &ss) ||
            jstr(line, "sha256", x->sha, sizeof x->sha) || jstr(line, "path", x->path, sizeof x->path)) die("malformed dataset file line");
        x->g = (int)gg; x->index = (int)ii; snprintf(x->seed, sizeof x->seed, "%llu", (unsigned long long)ss);
        nf++;
    }
    fclose(f);
    /* sort by group then index: that is the pool order */
    { int a, b; for (a = 0; a < nf; a++) for (b = a + 1; b < nf; b++)
        if (F[b].g < F[a].g || (F[b].g == F[a].g && F[b].index < F[a].index)) { file_t t = F[a]; F[a] = F[b]; F[b] = t; } }
    snprintf(p, sizeof p, "%s/bundle/probability_streams/INDEX", root);
    if (!(f = fopen(p, "r"))) die("cannot open probability_streams/INDEX");
    while (fgets(line, sizeof line, f)) {
        char sha[65], tr[65], ts[65], cn[64], path[1024]; int g, j, ci, fi; unsigned long long by;
        if (line[0] == '#') continue;
        if (sscanf(line, "%64s %64s %64s %d %d %63s %llu %1023s", sha, tr, ts, &g, &j, cn, &by, path) != 8) die("bad TPS INDEX line");
        if ((ci = cand_index(cn)) < 0 || (fi = file_index(g, j)) < 0) die("TPS INDEX names unknown candidate or file");
        snprintf(ST[ci][fi].tps, 1024, "%s/%s", root, path);
        snprintf(ST[ci][fi].tsy, 1024, "%s/work/symbols/g%d_j%d_%s.tsy", root, g, j, cn);
        strcpy(idx_tps_trailer[ci][fi], tr); strcpy(idx_tsy_trailer[ci][fi], ts);
        ST[ci][fi].present |= 1;
    }
    fclose(f);
    snprintf(p, sizeof p, "%s/bundle/encoded_artifacts/INDEX", root);
    if (!(f = fopen(p, "r"))) die("cannot open encoded_artifacts/INDEX");
    while (fgets(line, sizeof line, f)) {
        char sha[65], coder[16], cn[64], path[1024]; int g, j, ci, fi; unsigned long long by;
        if (line[0] == '#') continue;
        if (sscanf(line, "%64s %15s %d %d %63s %llu %1023s", sha, coder, &g, &j, cn, &by, path) != 7) die("bad coded INDEX line");
        if ((ci = cand_index(cn)) < 0 || (fi = file_index(g, j)) < 0) die("coded INDEX names unknown candidate or file");
        if (!strcmp(coder, "range")) { snprintf(ST[ci][fi].tcr, 1024, "%s/%s", root, path); ST[ci][fi].present |= 2; }
        else if (!strcmp(coder, "rans")) { snprintf(ST[ci][fi].tca, 1024, "%s/%s", root, path); ST[ci][fi].present |= 4; }
        else die("unknown coder in INDEX");
    }
    fclose(f);
    { int ci, fi; for (ci = 0; ci < nc; ci++) for (fi = 0; fi < nf; fi++) if (ST[ci][fi].present != 7) die("bundle lacks a TPS1/TCR1/TCA1 for some (candidate, file)"); }
}

static void le(uint8_t *p, uint64_t v, int n) { int i; for (i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t rdle(const uint8_t *p, int n) { uint64_t v = 0; int i; for (i = n - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* checks the 56-byte coded header except the binding, returns payload pointer or NULL */
static const uint8_t *coded_header(const uint8_t *b, uint64_t L, const char *magic, int id, uint64_t count, char *why) {
    if (L < 56 || memcmp(b, magic, 4)) { strcpy(why, "HEADER magic/length"); return NULL; }
    if (b[4] != 1 || b[5] != id || b[6] || b[7]) { strcpy(why, "HEADER version/coder_id/reserved"); return NULL; }
    if (rdle(b + 8, 8) != count) { strcpy(why, "COUNT"); return NULL; }
    if (L < 56 + rdle(b + 48, 8)) { strcpy(why, "TRUNC"); return NULL; }
    if (L > 56 + rdle(b + 48, 8)) { strcpy(why, "TRAIL"); return NULL; }
    return b + 56;
}

static void score_one(int ci, int fi, const is_model *m) {
    const is_events *ev = &F[fi].ev; stream_t *st = &ST[ci][fi]; res_t *rs = &R[ci][fi];
    uint8_t hdr[128], rec[24 + 2 * IS_K], tpsd[32], tsyd[32], *tcr = NULL, *tca = NULL; uint64_t tcrL = 0, tcaL = 0, t, crumb = 0;
    is_sha256 tc, sc; is_walk w; is_rdec rd; is_adec ad; FILE *tf = NULL; uint8_t fhdr[128], frec[24 + 2 * IS_K];
    const uint8_t *tcrp = NULL, *tcap = NULL; char why[128]; int j, k; int64_t cub = 0;
    uint8_t *syms;
    memset(rs, 0, sizeof *rs);
    rs->crumb_ub = calloc(ev->crumbs ? ev->crumbs : 1, sizeof(int64_t));
    syms = malloc(ev->n ? ev->n : 1);
    /* my TPS1 header */
    memset(hdr, 0, sizeof hdr); memcpy(hdr, "TPS1", 4); le(hdr + 4, 1, 2); hdr[6] = IS_K; hdr[7] = 16; le(hdr + 12, ev->n, 8);
    le(hdr + 20, 24 + 2 * IS_K, 4); memcpy(hdr + 28, profile_dig, 32); memcpy(hdr + 60, m->model_digest, 32); memcpy(hdr + 92, ev->sha, 32);
    is_domain_digest_init(&tc, "turing.tc.pstream.v1"); is_sha256_update(&tc, hdr, 128);
    rs->tps_match = -1; rs->tsy_match = -1;
    if (st->present && strcmp(st->tps, "-")) {
        tf = fopen(st->tps, "rb");
        if (!tf || fread(fhdr, 1, 128, tf) != 128) { snprintf(rs->tps_note, sizeof rs->tps_note, "cannot read TPS1"); rs->tps_match = 0; if (tf) fclose(tf); tf = NULL; }
        else {
            rs->tps_match = 1;
            if (memcmp(fhdr, hdr, 128)) {
                int diff = -1; for (j = 0; j < 128; j++) if (fhdr[j] != hdr[j]) { diff = j; break; }
                snprintf(rs->tps_note, sizeof rs->tps_note, "TPS1 header differs first at byte %d", diff); rs->tps_match = 0;
            }
        }
    }
    if (st->present) {
        if (is_read_file(st->tcr, &tcr, &tcrL)) die("cannot read TCR1");
        if (is_read_file(st->tca, &tca, &tcaL)) die("cannot read TCA1");
        rs->range_bytes = tcrL; rs->rans_bytes = tcaL;
        if (!(tcrp = coded_header(tcr, tcrL, "TCR1", 1, ev->n, why))) { note("TCR1 header: %s %s", why, C[ci].name); rs->range_ok = 0; }
        if (!(tcap = coded_header(tca, tcaL, "TCA1", 2, ev->n, why))) { note("TCA1 header: %s %s", why, C[ci].name); rs->rans_ok = 0; }
        if (tcrp) is_rdec_init(&rd, tcrp, tcrL - 56);
        if (tcap) is_adec_init(&ad, tcap, tcaL - 56);
    }
    for (k = 0; k < 5; k++) w.p[k] = IS_K;
    w.crumb = 0;
    for (t = 0; t < ev->n; t++) {
        const uint16_t *q; int found, s = ev->sym[t]; uint64_t key;
        if (ev->first[t]) {
            if (t > 0) { rs->crumb_ub[crumb++] = cub; cub = 0; }
            for (k = 0; k < 5; k++) w.p[k] = IS_K;
            w.crumb = crumb;
        }
        key = is_key(m, ev, t, &w);
        q = is_model_row(m, key, &found); rs->rows_hit += (uint64_t)found;
        le(rec, t, 8); le(rec + 8, key, 8); le(rec + 16, crumb, 4); le(rec + 20, 65536, 4);
        for (j = 0; j < IS_K; j++) le(rec + 24 + 2 * j, q[j], 2);
        is_sha256_update(&tc, rec, sizeof rec);
        if (tf && rs->tps_match >= 0) {
            if (fread(frec, 1, sizeof frec, tf) != sizeof frec) { if (rs->tps_match) snprintf(rs->tps_note, sizeof rs->tps_note, "TPS1 short at record %llu", (unsigned long long)t); rs->tps_match = 0; fclose(tf); tf = NULL; }
            else if (memcmp(frec, rec, sizeof rec)) {
                if (rs->tps_match == 1 || !rs->tps_note[0]) {
                    int diff = -1; for (j = 0; j < (int)sizeof rec; j++) if (frec[j] != rec[j]) { diff = j; break; }
                    snprintf(rs->tps_note, sizeof rs->tps_note, "TPS1 record %llu differs first at record byte %d", (unsigned long long)t, diff);
                }
                rs->tps_match = 0;
            }
        }
        rs->ideal_ub += is_ub_q16[q[s]]; rs->ideal_alt_ub += ub_alt[q[s]]; cub += is_ub_q16[q[s]];
        if (ub_alt[q[s]] != is_ub_q16[q[s]]) rs->alt_diff_events++;
        syms[t] = (uint8_t)s;
        if (tcrp) { int x = is_rdec_step(&rd, q, IS_K); if (x != s) { if (x >= 0) { note("range decode symbol mismatch %s %s", C[ci].name, F[fi].seed); rd.err = -116; } tcrp = NULL; } }
        if (tcap) { int x = is_adec_step(&ad, q, IS_K); if (x != s) { if (x >= 0) { note("rANS decode symbol mismatch %s %s", C[ci].name, F[fi].seed); ad.err = -116; } tcap = NULL; } }
        for (k = 4; k > 0; k--) w.p[k] = w.p[k - 1];
        w.p[0] = s;
    }
    if (ev->n) rs->crumb_ub[crumb++] = cub;
    if (crumb != ev->crumbs) die("crumb count mismatch");
    { int64_t sum = 0; uint64_t i; for (i = 0; i < crumb; i++) sum += rs->crumb_ub[i];
      if (sum != rs->ideal_ub) die("sum over crumbs != file ideal (UNCERTAINTY 1): run void"); }
    is_sha256_final(&tc, tpsd); is_sha256_hex(tpsd, rs->tps_digest);
    if (tf) {
        uint8_t trl[32], extra;
        if (fread(trl, 1, 32, tf) != 32) { rs->tps_match = 0; snprintf(rs->tps_note, sizeof rs->tps_note, "TPS1 trailer missing"); }
        else if (memcmp(trl, tpsd, 32)) { if (rs->tps_match) snprintf(rs->tps_note, sizeof rs->tps_note, "TPS1 trailer != my digest"); rs->tps_match = 0; }
        if (fread(&extra, 1, 1, tf) == 1) { rs->tps_match = 0; snprintf(rs->tps_note, sizeof rs->tps_note, "TPS1 has trailing bytes"); }
        fclose(tf);
    }
    if (st->present) {
        rs->binding_ok = tcrL >= 56 && tcaL >= 56 && !memcmp(tcr + 16, tpsd, 32) && !memcmp(tca + 16, tpsd, 32);
        if (tcrp) rs->range_ok = is_rdec_finish(&rd) == 0 && rs->binding_ok; else rs->range_ok = 0;
        if (tcap) rs->rans_ok = is_adec_finish(&ad) == 0 && rs->binding_ok; else rs->rans_ok = 0;
        if (!rs->range_ok) note("range decode/termination/binding failed: %s %s", C[ci].name, F[fi].seed);
        if (!rs->rans_ok) note("rANS decode/termination/binding failed: %s %s", C[ci].name, F[fi].seed);
    }
    /* TSY1 */
    { uint8_t h[56]; memset(h, 0, 56); memcpy(h, "TSY1", 4); le(h + 4, 1, 2); h[6] = IS_K; le(h + 8, ev->n, 8); memcpy(h + 24, ev->sha, 32);
      is_domain_digest_init(&sc, "turing.tc.symbols.v1"); is_sha256_update(&sc, h, 56); is_sha256_update(&sc, syms, ev->n); is_sha256_final(&sc, tsyd); is_sha256_hex(tsyd, rs->tsy_digest);
      if (st->present && strcmp(st->tsy, "-")) {
          uint8_t *b; uint64_t L;
          if (is_read_file(st->tsy, &b, &L)) { rs->tsy_match = 0; snprintf(rs->tsy_note, sizeof rs->tsy_note, "cannot read"); }
          else {
              rs->tsy_match = L == 56 + ev->n + 32 && !memcmp(b, h, 56) && !memcmp(b + 56, syms, ev->n) && !memcmp(b + 56 + ev->n, tsyd, 32);
              if (!rs->tsy_match) snprintf(rs->tsy_note, sizeof rs->tsy_note, "TSY1 bytes differ from my symbols/header/trailer");
              free(b);
          }
      } }
    free(syms); free(tcr); free(tca);
}

static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return x < y ? -1 : x > y; }

static FILE *out, *det;
static int first_val = 1;
static void val(const char *k, int64_t v) { fprintf(out, "%s    {\"key\": \"%s\", \"value\": %lld}", first_val ? "" : ",\n", k, (long long)v); first_val = 0; }

int main(int argc, char **argv) {
    const char *docs = NULL, *root = NULL, *outp = NULL, *detp = NULL, *cdirs[8]; int ncd = 0, i, ci, fi, g, B2;
    char msg[256], hx[65], cm_hex[65], dm_hex[65], pd_hex[65], p[2048]; int dry = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--docs") && i + 1 < argc) docs = argv[++i];
        else if (!strcmp(argv[i], "--bundle-root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "--details") && i + 1 < argc) detp = argv[++i];
        else if (!strcmp(argv[i], "--cand-dir") && i + 1 < argc && ncd < 8) cdirs[ncd++] = argv[++i];
        else die("usage: --docs D --bundle-root R [--cand-dir D...] --out F --details F");
    }
    if (!docs || !root || !outp || !detp) die("missing argument");
    if (!ncd) { static char cd[2048]; snprintf(cd, sizeof cd, "%s/candidates", root); cdirs[ncd++] = cd; }
    is_ub_table_init();
    { /* alternative table: exact value correctly rounded, via the two q the literal method rounds differently (selftest) */
      uint32_t q; for (q = 0; q <= 65536; q++) ub_alt[q] = is_ub_q16[q]; ub_alt[43481] -= 1; ub_alt[46819] -= 1; }
    snprintf(p, sizeof p, "%s/profiles/Turing-profile-v1.0.toml", docs);
    if (is_sha256_file(p, profile_dig, NULL)) die("cannot hash profile");
    is_sha256_hex(profile_dig, profile_hex);
    { uint8_t *b; uint64_t L; snprintf(p, sizeof p, "%s/bundle/profile.digest", root);
      if (is_read_file(p, &b, &L) || L < 64) die("cannot read bundle/profile.digest");
      memcpy(pd_hex, b, 64); pd_hex[64] = 0; free(b);
      if (strcmp(pd_hex, profile_hex)) die("bundle profile.digest differs from SHA-256 of the docs profile"); }
    { uint8_t d[32], d2[32]; snprintf(p, sizeof p, "%s/candidate_manifest.json", root);
      if (is_sha256_file(p, d, NULL)) die("cannot hash candidate manifest");
      is_sha256_hex(d, cm_hex);
      snprintf(p, sizeof p, "%s/candidate_manifest.json", docs);
      if (is_sha256_file(p, d2, NULL) || memcmp(d, d2, 32)) die("bundle candidate manifest differs from the docs copy");
      snprintf(p, sizeof p, "%s/dataset_manifest.json", root);
      if (is_sha256_file(p, d, NULL)) die("cannot hash dataset manifest");
      is_sha256_hex(d, dm_hex);
      { uint8_t *b; uint64_t L; if (is_read_file(p, &b, &L)) die("cannot read dataset manifest");
        { const char *k = "\"split\": \"sealed_test\""; size_t kl = strlen(k); uint64_t i; for (i = 0; i + kl <= L; i++) if (!memcmp(b + i, k, kl)) { dry = 0; break; } } free(b); } }
    load_manifest(root);
    if ((B2 = cand_index("B2_order1")) < 0) die("no B2_order1 in manifest");
    load_bundle(root);
    for (fi = 0; fi < nf; fi++) {
        if (is_ctr1_load(F[fi].path, &F[fi].ev, msg, sizeof msg)) die(msg);
        is_sha256_hex(F[fi].ev.sha, hx);
        if (strcmp(F[fi].sha, "-") && strcmp(F[fi].sha, hx)) die("trace sha256 differs from plan/dataset manifest");
        fprintf(stderr, "file g%d idx%d seed %s: %llu events, %llu crumbs, gaps %llu, sha %s\n", F[fi].g, F[fi].index, F[fi].seed,
                (unsigned long long)F[fi].ev.n, (unsigned long long)F[fi].ev.crumbs, (unsigned long long)F[fi].ev.gaps, hx);
    }
    for (ci = 0; ci < nc; ci++) {
        is_model m; int r = -100, d; char path[2048];
        for (d = 0; d < ncd && r == -100; d++) { snprintf(path, sizeof path, "%s/%s", cdirs[d], C[ci].file); r = is_model_load(path, &m, msg, sizeof msg); }
        if (r) { fprintf(stderr, "candidate %s: %s\n", C[ci].name, msg); die("candidate model refused or missing"); }
        is_sha256_hex(m.file_sha, hx); if (strcmp(hx, C[ci].file_sha)) die("candidate file sha256 differs from manifest");
        is_sha256_hex(m.model_digest, hx); if (strcmp(hx, C[ci].model_digest)) die("candidate model digest differs from manifest");
        if (m.lm_bits != C[ci].lm_bits) die("candidate L(M) differs from manifest");
        if (m.K != IS_K) die("K != 9 is not a candidate under this profile");
        for (fi = 0; fi < nf; fi++) {
            score_one(ci, fi, &m);
            fprintf(stderr, "  %-13s g%d idx%d ideal %lld ub, range %llu B, rans %llu B, tps %d tsy %d range_ok %d rans_ok %d %s\n", C[ci].name, F[fi].g, F[fi].index,
                    (long long)R[ci][fi].ideal_ub, (unsigned long long)R[ci][fi].range_bytes, (unsigned long long)R[ci][fi].rans_bytes,
                    R[ci][fi].tps_match, R[ci][fi].tsy_match, R[ci][fi].range_ok, R[ci][fi].rans_ok, R[ci][fi].tps_note);
        }
        is_model_free(&m);
    }
    { /* per-crumb dump for cross-checking ideal_lengths.json: g index crumb n candidate ideal_ub */
      char cp[4200]; FILE *cf; snprintf(cp, sizeof cp, "%s.crumbs.txt", detp);
      if (!(cf = fopen(cp, "w"))) die("cannot write crumb dump");
      for (fi = 0; fi < nf; fi++) {
          uint64_t *cn = calloc(F[fi].ev.crumbs + 1, sizeof(uint64_t)), t, k = 0;
          for (t = 0; t < F[fi].ev.n; t++) { if (F[fi].ev.first[t] && t > 0) k++; cn[k]++; }
          for (k = 0; k < F[fi].ev.crumbs; k++) for (ci = 0; ci < nc; ci++)
              fprintf(cf, "%d %d %llu %llu %s %lld\n", F[fi].g, F[fi].index, (unsigned long long)k, (unsigned long long)cn[k], C[ci].name, (long long)R[ci][fi].crumb_ub[k]);
          free(cn);
      }
      fclose(cf); }
    if (!(out = fopen(outp, "w")) || !(det = fopen(detp, "w"))) die("cannot write output");
    fprintf(out, "{\n  \"schema\": \"turing.cal.scorer.v1\",\n  \"scorer\": \"lane D independent scorer (indep-scorer, C, written from calibration docs only)\",\n  \"dry_run\": %s,\n  \"profile_sha256\": \"%s\",\n  \"candidate_manifest_sha256\": \"%s\",\n  \"dataset_manifest_sha256\": \"%s\",\n  \"values\": [\n", dry ? "true" : "false", pd_hex, cm_hex, dm_hex);
    fprintf(det, "{\n  \"schema\": \"lane_d.indep_scorer.details.v0\",\n  \"profile_digest\": \"%s\",\n  \"problems\": PROBLEMS_PLACEHOLDER,\n  \"files\": [\n", profile_hex);
    for (fi = 0; fi < nf; fi++) {
        is_sha256_hex(F[fi].ev.sha, hx);
        fprintf(det, "    {\"g\": %d, \"index\": %d, \"seed\": \"%s\", \"sha256\": \"%s\", \"events\": %llu, \"crumbs\": %llu, \"event_index_gaps\": %llu, \"candidates\": [\n",
                F[fi].g, F[fi].index, F[fi].seed, hx, (unsigned long long)F[fi].ev.n, (unsigned long long)F[fi].ev.crumbs, (unsigned long long)F[fi].ev.gaps);
        for (ci = 0; ci < nc; ci++) {
            res_t *r = &R[ci][fi]; int64_t N = (int64_t)F[fi].ev.n;
            int64_t ovr = (int64_t)r->range_bytes * 8 * IS_UB - r->ideal_ub, ova = (int64_t)r->rans_bytes * 8 * IS_UB - r->ideal_ub;
            int64_t band = 64000000LL + 1000LL * N;
            { char k2[8192];
              snprintf(k2, sizeof k2, "g%d.f%d.%s.ideal_ub", F[fi].g, F[fi].index, C[ci].name); val(k2, r->ideal_ub);
              snprintf(k2, sizeof k2, "g%d.f%d.%s.range_bytes", F[fi].g, F[fi].index, C[ci].name); val(k2, (int64_t)r->range_bytes);
              snprintf(k2, sizeof k2, "g%d.f%d.%s.rans_bytes", F[fi].g, F[fi].index, C[ci].name); val(k2, (int64_t)r->rans_bytes);
              if (strcmp(r->tps_digest, idx_tps_trailer[ci][fi])) note("my TPS1 digest != INDEX trailer digest: %s %s", C[ci].name, F[fi].seed);
              if (strcmp(r->tsy_digest, idx_tsy_trailer[ci][fi])) note("my TSY1 digest != INDEX digest: %s %s", C[ci].name, F[fi].seed); }
            fprintf(det, "      {\"name\": \"%s\", \"ideal_ub\": %lld, \"ideal_ub_exact_rounding_variant\": %lld, \"events_with_undetermined_q\": %llu, \"rows_hit\": %llu, "
                    "\"tps1_digest\": \"%s\", \"tps1_equal\": %d, \"tps1_note\": \"%s\", \"tsy1_equal\": %d, \"range_bytes\": %llu, \"rans_bytes\": %llu, "
                    "\"range_decoded_ok\": %d, \"rans_decoded_ok\": %d, \"binding_ok\": %d, \"range_overhead_ub\": %lld, \"rans_overhead_ub\": %lld, "
                    "\"range_envelope_ok\": %d, \"rans_envelope_ok\": %d}%s\n",
                    C[ci].name, (long long)r->ideal_ub, (long long)r->ideal_alt_ub, (unsigned long long)r->alt_diff_events, (unsigned long long)r->rows_hit,
                    r->tps_digest, r->tps_match, r->tps_note, r->tsy_match, (unsigned long long)r->range_bytes, (unsigned long long)r->rans_bytes,
                    r->range_ok, r->rans_ok, r->binding_ok, (long long)ovr, (long long)ova,
                    llabs(ovr - 448000000LL) <= band, llabs(ova - 448000000LL) <= band, ci + 1 < nc ? "," : "");
        }
        fprintf(det, "    ]}%s\n", fi + 1 < nf ? "," : "");
    }
    fprintf(det, "  ],\n  \"groups\": [\n");
    for (g = 1; g <= 2; g++) {
        uint64_t Cn = 0, events = 0, b, s; int64_t *dvec, *Tb; uint32_t *idx; char key[8192]; int any = 0;
        int64_t ideal[MAXC], rbits[MAXC], abits[MAXC];
        for (fi = 0; fi < nf; fi++) if (F[fi].g == g) { Cn += F[fi].ev.crumbs; events += F[fi].ev.n; any = 1; }
        if (!any) continue;
        if (!Cn) die("VOID: a group has no crumbs");
        /* pool: files in index order (plan order must already be group then index), then crumb ordinal */
        idx = malloc(sizeof(uint32_t) * 10000 * Cn); dvec = malloc(sizeof(int64_t) * Cn); Tb = malloc(sizeof(int64_t) * 10000);
        if (!idx || !dvec || !Tb) die("oom");
        { uint64_t st = 0x4558503030315543ULL + (uint64_t)g; for (b = 0; b < 10000 * Cn; b++) idx[b] = (uint32_t)is_draw(&st, Cn); }
        snprintf(key, sizeof key, "g%d.crumbs", g); val(key, (int64_t)Cn);
        snprintf(key, sizeof key, "g%d.events", g); val(key, (int64_t)events);
        for (ci = 0; ci < nc; ci++) {
            ideal[ci] = 0; rbits[ci] = 0; abits[ci] = 0;
            for (fi = 0; fi < nf; fi++) if (F[fi].g == g) { ideal[ci] += R[ci][fi].ideal_ub; rbits[ci] += (int64_t)R[ci][fi].range_bytes * 8; abits[ci] += (int64_t)R[ci][fi].rans_bytes * 8; }
        }
        fprintf(det, "    {\"g\": %d, \"crumbs\": %llu, \"events\": %llu, \"candidates\": [\n", g, (unsigned long long)Cn, (unsigned long long)events);
        for (ci = 0; ci < nc; ci++) {
            int64_t lmd = ((int64_t)C[ci].lm_bits - (int64_t)C[B2].lm_bits) * IS_UB, T, lo, hi, oR, oA; uint64_t p = 0, gt0 = 0;
            for (fi = 0; fi < nf; fi++) if (F[fi].g == g) { uint64_t c; for (c = 0; c < F[fi].ev.crumbs; c++) dvec[p++] = R[B2][fi].crumb_ub[c] - R[ci][fi].crumb_ub[c]; }
            T = -lmd; for (s = 0; s < Cn; s++) T += dvec[s];
            for (b = 0; b < 10000; b++) { int64_t sum = -lmd; const uint32_t *ix = idx + b * Cn; for (s = 0; s < Cn; s++) sum += dvec[ix[s]]; Tb[b] = sum; if (sum > 0) gt0++; }
            qsort(Tb, 10000, sizeof(int64_t), cmp64);
            lo = Tb[250]; hi = Tb[9749];
            oR = (rbits[ci] * IS_UB - ideal[ci]) - (rbits[B2] * IS_UB - ideal[B2]);
            oA = (abits[ci] * IS_UB - ideal[ci]) - (abits[B2] * IS_UB - ideal[B2]);
#define V(suffix, v) do { snprintf(key, sizeof key, "g%d.%s.%s", g, C[ci].name, (const char *)suffix); val(key, v); } while (0)
            V("lm_bits", (int64_t)C[ci].lm_bits); V("ideal_ub", ideal[ci]); V("range_bits", rbits[ci]); V("rans_bits", abits[ci]);
            V("T_ideal_vs_B2.point_ub", T); V("T_ideal_vs_B2.lo_ub", lo); V("T_ideal_vs_B2.hi_ub", hi);
            V("T_range_vs_B2.point_ub", T - oR); V("T_range_vs_B2.lo_ub", lo - oR); V("T_range_vs_B2.hi_ub", hi - oR);
            V("T_rans_vs_B2.point_ub", T - oA); V("T_rans_vs_B2.lo_ub", lo - oA); V("T_rans_vs_B2.hi_ub", hi - oA);
            fprintf(det, "      {\"name\": \"%s\", \"bootstrap_replicates_T_ideal_vs_B2_gt0\": %llu, \"range_overhead_ub\": %lld, \"rans_overhead_ub\": %lld}%s\n",
                    C[ci].name, (unsigned long long)gt0, (long long)(rbits[ci] * IS_UB - ideal[ci]), (long long)(abits[ci] * IS_UB - ideal[ci]), ci + 1 < nc ? "," : "");
        }
        fprintf(det, "    ]}%s\n", g == 1 ? "," : "");
        free(idx); free(dvec); free(Tb);
    }
    fprintf(det, "  ]\n}\n"); fprintf(out, "\n  ]\n}\n");
    fclose(out); fclose(det);
    /* patch problems count */
    { uint8_t *b; uint64_t L; char num[32]; FILE *f; char *p;
      if (!is_read_file(detp, &b, &L)) { snprintf(num, sizeof num, "%d", problems);
        p = strstr((char *)b, "PROBLEMS_PLACEHOLDER"); f = fopen(detp, "w");
        fwrite(b, 1, (size_t)(p - (char *)b), f); fputs(num, f); fwrite(p + 20, 1, L - (uint64_t)(p - (char *)b) - 20, f); fclose(f); free(b); } }
    fprintf(stderr, "done, %d problems\n", problems);
    return problems ? 3 : 0;
}
