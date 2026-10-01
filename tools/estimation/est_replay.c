#include "est_replay.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

#define NS 1000000000ll

void est_hex(const uint8_t *d, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[2 * n] = 0;
}

int est_sha_file_hex(const char *path, char out[65])
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    sha256_ctx c; sha256_init(&c);
    uint8_t buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, n);
    int bad = ferror(fp);
    fclose(fp);
    if (bad) return 1;
    uint8_t d[32]; sha256_final(&c, d);
    est_hex(d, 32, out);
    return 0;
}

static void setmsg(char *err, size_t cap, const char *fmt, const char *a, const char *b)
{
    if (err && cap) snprintf(err, cap, fmt, a, b);
}

/* Find "<hex>  <name>" in dir/SHA256SUMS. Returns 0 when found. */
int est_sums_lookup(const char *dir, const char *name, char hex[65])
{
    char p[1100];
    snprintf(p, sizeof p, "%s/SHA256SUMS", dir);
    FILE *fp = fopen(p, "r");
    if (!fp) return 1;
    char line[1024]; int found = 1;
    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l > 66 && line[64] == ' ' && line[65] == ' ' && strcmp(line + 66, name) == 0) {
            memcpy(hex, line, 64); hex[64] = 0; found = 0; break;
        }
    }
    fclose(fp);
    return found;
}

int est_allow_run_b = 0;
int est_allow_heldout = 0;

int est_path_is_heldout(const char *path)
{
    char rp[PATH_MAX];
    if (strstr(path, EST_V2_HELDOUT_TAG)) return 1;
    if (realpath(path, rp) && strstr(rp, EST_V2_HELDOUT_TAG)) return 1;
    return 0;
}

const char *est_run_b_id(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_B_ID"); if (e && *e) return e;
#endif
    return EST_RUN_B_ID;
}
const char *est_run_b_raw_sha(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_B_RAW_SHA"); if (e && *e) return e;
#endif
    return EST_RUN_B_RAW_SHA_DEFAULT;
}
const char *est_run_b_marks_sha(void)
{
#ifdef EST_TEST_BUILD
    const char *e = getenv("EST_TEST_B_MARKS_SHA"); if (e && *e) return e;
#endif
    return EST_RUN_B_MARKS_SHA_DEFAULT;
}

/* Resolve symlinks and relative parts, then look for run B's id in any path
 * component. The unresolved string is checked too. A path that cannot be
 * resolved is judged on its raw text only (the open then fails anyway). */
int est_path_is_run_b(const char *path)
{
    char rp[PATH_MAX];
    if (strstr(path, est_run_b_id())) return 1;
    if (realpath(path, rp) && strstr(rp, est_run_b_id())) return 1;
    return 0;
}

static int hex_is_run_b(const char *hex)
{
    return strcmp(hex, est_run_b_raw_sha()) == 0 || strcmp(hex, est_run_b_marks_sha()) == 0;
}

static int read_index(const char *path, est_file *f, char *err, size_t errcap);

int est_file_load(const char *path, est_file *f, char *err, size_t errcap)
{
    memset(f, 0, sizeof *f);
    if (strlen(path) >= sizeof f->path) { setmsg(err, errcap, "%s: path too long%s", path, ""); return 1; }
    char dir[1024]; const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (slash) { size_t n = (size_t)(slash - path); memcpy(dir, path, n); dir[n] = 0; }
    else snprintf(dir, sizeof dir, ".");
    if (!est_allow_heldout && est_path_is_heldout(path)) {
        setmsg(err, errcap, "%s: resolves into the held-out run; only est_eval --protocol-v2 --recorded may open it%s", path, "");
        return 1;
    }
    if (!est_allow_run_b && est_path_is_run_b(path)) {
        setmsg(err, errcap, "%s: resolves into run B; only est_eval --recorded may open run B%s", path, "");
        return 1;
    }
    char want[65];
    if (est_sums_lookup(dir, name, want)) { setmsg(err, errcap, "%s: no SHA256SUMS line for %s", dir, name); return 1; }
    if (!est_allow_run_b && hex_is_run_b(want)) {
        setmsg(err, errcap, "%s: SHA256SUMS names run B's digest; refused before opening%s", path, "");
        return 1;
    }
    if (read_index(path, f, err, errcap)) return 1;
    if (strcmp(f->sha_hex, want) != 0) {
        setmsg(err, errcap, "SHA-256 mismatch for %s (SHA256SUMS says %s)", path, want);
        est_file_free(f);
        return 1;
    }
    if (!est_allow_run_b && hex_is_run_b(f->sha_hex)) {
        setmsg(err, errcap, "%s: content is run B; refused%s", path, "");
        est_file_free(f);
        return 1;
    }
    return 0;
}

int est_file_load_raw(const char *path, est_file *f, char *err, size_t errcap)
{
    memset(f, 0, sizeof *f);
    if (strlen(path) >= sizeof f->path) { setmsg(err, errcap, "%s: path too long%s", path, ""); return 1; }
    return read_index(path, f, err, errcap);
}

static int read_index(const char *path, est_file *f, char *err, size_t errcap)
{
    snprintf(f->path, sizeof f->path, "%s", path);
    FILE *fp = fopen(path, "rb");
    if (!fp) { setmsg(err, errcap, "cannot open %s%s", path, ""); return 1; }
    size_t cap = 1u << 20, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(fp); return 1; }
    for (;;) {
        if (len == cap) { cap *= 2; uint8_t *nb = realloc(buf, cap); if (!nb) { free(buf); fclose(fp); return 1; } buf = nb; }
        size_t n = fread(buf + len, 1, cap - len, fp);
        if (n == 0) break;
        len += n;
    }
    fclose(fp);
    uint8_t d[32]; sha256_hash(buf, len, d);
    est_hex(d, 32, f->sha_hex);
    f->data = buf; f->len = len;
    size_t nl = 0;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') nl++;
    if (len && buf[len - 1] != '\n') nl++;
    f->off = malloc((nl + 1) * sizeof(size_t)); f->llen = malloc((nl + 1) * sizeof(size_t));
    if (!f->off || !f->llen) { est_file_free(f); return 1; }
    size_t k = 0, start = 0;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') { f->off[k] = start; f->llen[k] = i - start; k++; start = i + 1; }
    if (start < len) { f->off[k] = start; f->llen[k] = len - start; k++; }
    f->nlines = k;
    return 0;
}

void est_file_free(est_file *f)
{
    free(f->data); free(f->off); free(f->llen);
    memset(f, 0, sizeof *f);
}

int est_parse_time_prefix(const char *s, size_t len, int64_t *t_ns, size_t *used)
{
    size_t i = 0; int64_t sec = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (i >= 10) return 2;               /* more than 10 seconds digits */
        sec = sec * 10 + (s[i] - '0'); i++;
    }
    if (i == 0 || i >= len || s[i] != '.') return 1;
    i++;
    int64_t frac = 0;
    for (int k = 0; k < 9; k++, i++) {
        if (i >= len || s[i] < '0' || s[i] > '9') return 1;
        frac = frac * 10 + (s[i] - '0');
    }
    if (i < len && s[i] >= '0' && s[i] <= '9') return 1; /* more than nine digits */
    if (sec > 9223372035ll) return 2;        /* sec * 1e9 must fit int64 */
    *t_ns = sec * NS + frac;
    if (used) *used = i;
    return 0;
}

int est_parse_t(const char *line, size_t len, int64_t *t_ns)
{
    static const char pre[] = "{\"t\":";
    if (len < sizeof pre - 1 || memcmp(line, pre, sizeof pre - 1) != 0) return 1;
    size_t used;
    int pr = est_parse_time_prefix(line + 5, len - 5, t_ns, &used);
    if (pr) return pr;
    size_t e = 5 + used;
    if (e >= len || (line[e] != ',' && line[e] != '}')) return 1;
    return 0;
}

int est_parse_thermal0(const char *line, size_t len, double *value)
{
    static const char key[] = "\"thermal_mc\":\"";
    size_t kl = sizeof key - 1;
    if (len < kl) return 1;
    size_t at = (size_t)-1;
    for (size_t i = 0; i + kl <= len; i++) if (memcmp(line + i, key, kl) == 0) { at = i + kl; break; }
    if (at == (size_t)-1) return 1;
    char tok[40]; size_t n = 0;
    while (at + n < len && line[at + n] != ' ' && line[at + n] != '"') {
        char c = line[at + n];
        if (!(isdigit((unsigned char)c) || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E')) return 1;
        if (n >= sizeof tok - 1) return 1;
        tok[n] = c; n++;
    }
    if (n == 0) return 1;
    tok[n] = 0;
    char *end; double v = strtod(tok, &end);
    if (*end != 0 || !isfinite(v)) return 1;
    *value = v;
    return 0;
}

double est_grid_q(int i) { return pow(10.0, -3.0 + 0.5 * (double)i); }
double est_grid_r(int j) { return pow(10.0, 0.25 * (double)j); }

int est_make_model(int model_id, double q, double r, est_model *m)
{
    memset(m, 0, sizeof *m);
    m->estimator = EST_ESTIMATOR_LINEAR_KALMAN;
    m->meaning = EST_UNCERTAINTY_GAUSSIAN_COVARIANCE;
    m->step_ns = NS;
    m->m = 1;
    m->obs_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m->state_unit[0] = EST_UNIT_MILLI_CELSIUS;
    m->R[0] = r;
    if (model_id == 0 || model_id == 2) {   /* M2 (protocol v2): M0 structure, r = 1e-12 */
        m->n = 1; m->F[0] = 1.0; m->Q[0] = q; m->H[0] = 1.0;
    } else if (model_id == 1) {
        m->n = 2;
        m->state_unit[1] = EST_UNIT_MILLI_CELSIUS_PER_S;
        m->F[0] = 1.0; m->F[1] = 1.0; m->F[2] = 0.0; m->F[3] = 1.0;
        m->Q[0] = q / 3.0; m->Q[1] = q / 2.0; m->Q[2] = q / 2.0; m->Q[3] = q;
        m->H[0] = 1.0; m->H[1] = 0.0;
    } else return 1;
    return 0;
}

static int digest_eq(const est_digest *a, const est_digest *b) { return memcmp(a->b, b->b, EST_DIGEST_SIZE) == 0; }

static int fail(est_replay *rp, const char *what, size_t line, est_status st)
{
    rp->error = 1;
    snprintf(rp->errmsg, sizeof rp->errmsg, "%s at line %zu (status %d)", what, line, (int)st);
    return 1;
}

int est_replay_run(const est_file *f, size_t limit, int model_id, double q, double r, est_replay *rp)
{
    est_step *keep = rp->steps; size_t cap = rp->cap;
    memset(rp, 0, sizeof *rp);
    rp->steps = keep; rp->cap = cap;
    size_t nl = f->nlines < limit ? f->nlines : limit;
    rp->lines_used = nl;
    if (est_make_model(model_id, q, r, &rp->model)) return fail(rp, "bad model", 0, EST_ERR_MODEL);
    est_status st = est_digest_model(&rp->model, &rp->model_d);
    if (st != EST_OK) return fail(rp, "model digest", 0, st);
    if (rp->cap < nl + 1) {
        est_step *ns = realloc(rp->steps, (nl + 1) * sizeof *ns);
        if (!ns) return fail(rp, "out of memory", 0, EST_OK);
        rp->steps = ns; rp->cap = nl + 1;
    }
    uint8_t src[32]; est_digest source;
    sha256_hash((const uint8_t *)EST_SOURCE_TEXT, strlen(EST_SOURCE_TEXT), src);
    memcpy(source.b, src, 32);

    est_belief bel; int have = 0;
    int64_t prev_wall = 0; int prev_ok = 0;
    size_t first = 0;
    for (size_t li = 0; li < nl; li++) {
        const char *ln = (const char *)f->data + f->off[li];
        size_t ll = f->llen[li];
        double z = 0; int64_t wall = 0;
        int tr = est_parse_t(ln, ll, &wall);
        int t_ok = (tr == 0);
        int v_parsed = (est_parse_thermal0(ln, ll, &z) == 0);
        /* a line whose t fails to parse is a missing observation even when the value parses */
        int v_ok = v_parsed && t_ok;
        if (!t_ok) { rp->bad_t++; if (tr == 2) rp->bad_t_overflow++; }
        if (!v_parsed) rp->bad_value++;
        uint8_t ev[32]; sha256_hash((const uint8_t *)ln, ll, ev);
        if (!have) {
            if (!v_ok) { rp->leading_missing++; if (t_ok) { prev_wall = wall; prev_ok = 1; } continue; }
            double x0[2] = { z, 0.0 }, P0[4] = { 0 };
            if (model_id != 1) P0[0] = r; else { P0[0] = r; P0[3] = 1e6; }
            st = est_kf_prior(&rp->model, x0, P0, 0, &bel);
            if (st != EST_OK) return fail(rp, "prior", li, st);
            first = li; have = 1;
            est_step *s = &rp->steps[rp->nsteps++];
            memset(s, 0, sizeof *s);
            s->line = li; s->L = 0; s->prior = 1; s->t_ok = t_ok; s->wall_ns = t_ok ? wall : 0;
            s->horizon = 0; s->t_ns = 0; s->z = z; s->post = bel; s->chain_ok = 1;
            memcpy(s->evidence.b, ev, 32);
            st = est_digest_belief(&bel, &s->belief_d);
            if (st != EST_OK) return fail(rp, "belief digest", li, st);
            if (t_ok) { prev_wall = wall; prev_ok = 1; } else { prev_ok = 0; }
            continue;
        }
        /* logical-grid horizon from the raw wall gap */
        uint32_t h = 1;
        if (t_ok && prev_ok) {
            int64_t gap = wall - prev_wall;
            if (gap == 0) rp->gap_zero++;
            else if (gap < 0) rp->gap_backward++;
            if (gap > 0) {
                int64_t hr = (gap + NS / 2) / NS; /* round half up */
                if (hr < 1) hr = 1;
                if (hr > 1000000) hr = 1000000;
                h = (uint32_t)hr;
            }
        }
        if (h > 1) rp->multi_horizon++;
        if (t_ok) { prev_wall = wall; prev_ok = 1; }
        else if (prev_ok) prev_wall += NS; /* unparsable t: assume one nominal step */

        est_step *s = &rp->steps[rp->nsteps++];
        memset(s, 0, sizeof *s);
        s->line = li; s->L = li - first; s->t_ok = t_ok; s->wall_ns = t_ok ? wall : 0; s->horizon = h;
        memcpy(s->evidence.b, ev, 32);
        est_prediction pred;
        st = est_kf_predict(&rp->model, &bel, NULL, h, &pred);
        if (st != EST_OK) return fail(rp, "predict", li, st);
        st = est_digest_prediction(&pred, &s->pred_d);
        if (st != EST_OK) return fail(rp, "prediction digest", li, st);
        s->t_ns = pred.t_ns;
        est_belief post;
        if (!v_ok) {
            s->coast = 1; rp->coasts++;
            st = est_kf_coast(&rp->model, &pred, &bel, &post);
            if (st != EST_OK) return fail(rp, "coast", li, st);
            s->chain_ok = 1;
        } else {
            est_observation o; memset(&o, 0, sizeof o);
            o.m = 1; o.unit[0] = EST_UNIT_MILLI_CELSIUS; o.z[0] = z; o.R[0] = r;
            o.t_ns = pred.t_ns; o.seq = (uint64_t)li; o.source = source;
            memcpy(o.evidence.b, ev, 32);
            est_innovation iv;
            st = est_kf_update(&rp->model, &bel, &pred, &o, &post, &iv);
            if (st != EST_OK) return fail(rp, "update", li, st);
            est_status s1 = est_digest_observation(&o, &s->obs_d);
            est_status s2 = est_digest_innovation(&iv, &s->innov_d);
            if (s1 != EST_OK || s2 != EST_OK) return fail(rp, "digest", li, s1 != EST_OK ? s1 : s2);
            s->z = z; s->y_mean = pred.y_mean[0]; s->S = iv.S[0]; s->nu = iv.nu[0]; s->nis = iv.nis;
            s->chain_ok = digest_eq(&iv.prediction, &s->pred_d) && digest_eq(&iv.observation, &s->obs_d)
                          && digest_eq(&o.evidence, &s->evidence) && (iv.nu[0] == z - pred.y_mean[0]);
        }
        s->post = post;
        st = est_digest_belief(&post, &s->belief_d);
        if (st != EST_OK) return fail(rp, "belief digest", li, st);
        bel = post;
    }
    if (!have) return fail(rp, "no valid observation in the file", 0, EST_OK);
    return 0;
}

void est_replay_free(est_replay *rp) { free(rp->steps); memset(rp, 0, sizeof *rp); }

void est_replay_write_stream(FILE *fp, const est_replay *rp, int model_id)
{
    char a[65], b[65], c[65], d[65];
    for (size_t i = 0; i < rp->nsteps; i++) {
        const est_step *s = &rp->steps[i];
        est_hex(s->obs_d.b, 32, a); est_hex(s->pred_d.b, 32, b);
        est_hex(s->innov_d.b, 32, c); est_hex(s->belief_d.b, 32, d);
        fprintf(fp, "M%d line=%zu L=%zu %s t_ns=%lld h=%u z=%.17g y=%.17g S=%.17g nu=%.17g nis=%.17g obs=%s pred=%s innov=%s belief=%s\n",
                model_id, s->line, s->L, s->prior ? "prior" : (s->coast ? "coast" : "obs"),
                (long long)s->t_ns, s->horizon, s->z, s->y_mean, s->S, s->nu, s->nis, a, b, c, d);
    }
}

int est_params_write(const char *path, const est_params *p)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 1;
    fprintf(fp, "est_params_v1\nprotocol_commit %s\nfit_run_path %s\nfit_file_sha256 %s\nfit_lines %zu\n",
            EST_PROTOCOL_COMMIT, p->fit_path, p->fit_sha, p->fit_lines);
    for (int m = 0; m < 2; m++)
        fprintf(fp, "M%d q %.17g r %.17g loglik %.17g\n", m, p->q[m], p->r[m], p->ll[m]);
    return fclose(fp) != 0;
}

int est_fit_pick(const double ll[EST_NQ][EST_NR], int *bi, int *bj)
{
    double best = -INFINITY; int pi = -1, pj = -1;
    for (int i = 0; i < EST_NQ; i++)
        for (int j = 0; j < EST_NR; j++) {
            if (!isfinite(ll[i][j])) continue;
            if (ll[i][j] > best) { best = ll[i][j]; pi = i; pj = j; }   /* strict: ties keep the earlier */
        }
    if (pi < 0) return 1;
    *bi = pi; *bj = pj;
    return 0;
}

static int is_hex64(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

/* Exactly: header, protocol_commit, fit_run_path, fit_file_sha256, fit_lines,
 * M0, M1, in that order, nothing else. */
int est_params_read(const char *path, est_params *p)
{
    memset(p, 0, sizeof *p);
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    char line[1300]; int idx = 0, bad = 0;
    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        if (l == 0 || line[l - 1] != '\n') { bad = 1; break; }
        line[--l] = 0;
        switch (idx) {
        case 0: if (strcmp(line, "est_params_v1") != 0) bad = 1; break;
        case 1: if (strcmp(line, "protocol_commit " EST_PROTOCOL_COMMIT) != 0) bad = 1; break;
        case 2:
            if (strncmp(line, "fit_run_path ", 13) != 0 || l - 13 == 0 || l - 13 >= sizeof p->fit_path) bad = 1;
            else snprintf(p->fit_path, sizeof p->fit_path, "%.1023s", line + 13);
            break;
        case 3:
            if (strncmp(line, "fit_file_sha256 ", 16) != 0 || !is_hex64(line + 16)) bad = 1;
            else memcpy(p->fit_sha, line + 16, 65);
            break;
        case 4: {
            char *end;
            if (strncmp(line, "fit_lines ", 10) != 0) { bad = 1; break; }
            p->fit_lines = (size_t)strtoull(line + 10, &end, 10);
            if (*end || end == line + 10) bad = 1;
            break; }
        case 5: case 6: {
            int m = idx - 5, used = 0;
            char tag[4]; snprintf(tag, sizeof tag, "M%d", m);
            if (strncmp(line, tag, 2) != 0 ||
                sscanf(line + 2, " q %lf r %lf loglik %lf%n", &p->q[m], &p->r[m], &p->ll[m], &used) != 3 ||
                line[2 + used] != 0) bad = 1;
            break; }
        default: bad = 1;
        }
        if (bad) break;
        idx++;
    }
    fclose(fp);
    return (!bad && idx == 7) ? 0 : 1;
}

int est_params_validate(const est_params *p, char *err, size_t cap)
{
    for (int m = 0; m < 2; m++) {
        double q = p->q[m], r = p->r[m];
        if (!isfinite(q) || !isfinite(r) || q <= 0 || r <= 0) { snprintf(err, cap, "M%d: q or r not finite and positive", m); return 1; }
        int i = (int)floor((log10(q) + 3.0) / 0.5 + 0.5), j = (int)floor(log10(r) / 0.25 + 0.5);
        if (i < 0 || i >= EST_NQ || fabs(q - est_grid_q(i)) > 1e-9 * est_grid_q(i)) { snprintf(err, cap, "M%d: q %.17g is not on the protocol grid", m, q); return 1; }
        if (j < 0 || j >= EST_NR || fabs(r - est_grid_r(j)) > 1e-9 * est_grid_r(j)) { snprintf(err, cap, "M%d: r %.17g is not on the protocol grid", m, r); return 1; }
    }
    return 0;
}

/* ---- protocol v2 ---- */
size_t est_m2_changes(const est_replay *rp, double *e, size_t cap)
{
    size_t n = 0; double prev = 0; int have = 0;
    for (size_t i = 0; i < rp->nsteps; i++) {
        const est_step *s = &rp->steps[i];
        if (s->coast) continue;
        if (!s->prior && have && memcmp(&s->y_mean, &prev, sizeof prev) != 0 && s->L >= EST_BURN_IN) return (size_t)-1;
        if (!s->prior && s->L >= EST_BURN_IN && s->horizon == 1 && n < cap) e[n++] = s->nu;
        prev = s->z; have = 1;
    }
    return n;
}

int est_params2_write(const char *path, const est_params2 *p)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 1;
    fprintf(fp, "est_params_v2\nv1_params_path %s\nv1_params_sha256 %s\nfit_run_path %s\nfit_file_sha256 %s\nfit_lines %zu\nfit_changes %zu\n",
            p->v1_params_path, p->v1_params_sha, p->fit_path, p->fit_sha, p->fit_lines, p->fit_n);
    fprintf(fp, "M2 q %.17g r %.17g k %u loglik %.17g\n", p->q, p->r, p->mix.k, p->ll);
    for (uint32_t j = 0; j < p->mix.k; j++) fprintf(fp, "c%u w %.17g v %.17g\n", j, p->mix.w[j], p->mix.v[j]);
    return fclose(fp) != 0;
}

static int str_field(const char *line, const char *key, char *dst, size_t cap)
{
    size_t kl = strlen(key), l = strlen(line);
    if (strncmp(line, key, kl) != 0 || line[kl] != ' ' || l - kl - 1 == 0 || l - kl - 1 >= cap) return 1;
    memcpy(dst, line + kl + 1, l - kl); /* includes the terminator */
    return 0;
}
static int size_field(const char *line, const char *key, size_t *out)
{
    size_t kl = strlen(key); char *end;
    if (strncmp(line, key, kl) != 0 || line[kl] != ' ' || !line[kl + 1]) return 1;
    *out = (size_t)strtoull(line + kl + 1, &end, 10);
    return *end != 0;
}

int est_params2_read(const char *path, est_params2 *p)
{
    memset(p, 0, sizeof *p);
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    char line[1300]; int idx = 0, bad = 0, total = 8;
    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        if (l == 0 || line[l - 1] != '\n') { bad = 1; break; }
        line[--l] = 0;
        int used = 0;
        switch (idx) {
        case 0: bad = strcmp(line, "est_params_v2") != 0; break;
        case 1: bad = str_field(line, "v1_params_path", p->v1_params_path, sizeof p->v1_params_path); break;
        case 2: bad = str_field(line, "v1_params_sha256", p->v1_params_sha, sizeof p->v1_params_sha) || !is_hex64(p->v1_params_sha); break;
        case 3: bad = str_field(line, "fit_run_path", p->fit_path, sizeof p->fit_path); break;
        case 4: bad = str_field(line, "fit_file_sha256", p->fit_sha, sizeof p->fit_sha) || !is_hex64(p->fit_sha); break;
        case 5: bad = size_field(line, "fit_lines", &p->fit_lines); break;
        case 6: bad = size_field(line, "fit_changes", &p->fit_n); break;
        case 7:
            if (sscanf(line, "M2 q %lf r %lf k %u loglik %lf%n", &p->q, &p->r, &p->mix.k, &p->ll, &used) != 4 || line[used] != 0
                || p->mix.k < 1 || p->mix.k > EST_MIX_MAX) bad = 1;
            else total = 8 + (int)p->mix.k;
            break;
        default: {
            unsigned j;
            if (idx >= total || sscanf(line, "c%u w %lf v %lf%n", &j, &p->mix.w[idx - 8], &p->mix.v[idx - 8], &used) != 3
                || line[used] != 0 || j != (unsigned)(idx - 8)) bad = 1;
            break; }
        }
        if (bad) break;
        idx++;
    }
    fclose(fp);
    return (!bad && idx == total && idx > 8) ? 0 : 1;
}

#ifdef EST_REPLAY_MAIN
/* est_replay <model 0|1|2> <q> <r> <machine-state.ndjson>
 * Prints the full observation/prediction/innovation stream, one line per step.
 * Never opens run B (est_eval --recorded writes run B's streams itself). */
int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: est_replay model q r file\n"); return 2; }
    char *e1, *e2, *e3;
    long mid = strtol(argv[1], &e1, 10);
    double q = strtod(argv[2], &e2), r = strtod(argv[3], &e3);
    if (*e1 || *e2 || *e3 || (mid < 0 || mid > 2)) { fprintf(stderr, "bad model, q or r\n"); return 2; }
    est_file f; char err[300];
    if (est_file_load(argv[4], &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    static est_replay rp;
    if (est_replay_run(&f, (size_t)-1, (int)mid, q, r, &rp)) {
        fprintf(stderr, "replay failed: %s\n", rp.errmsg); return 1;
    }
    est_replay_write_stream(stdout, &rp, (int)mid);
    fprintf(stderr, "steps=%zu coasts=%zu leading_missing=%zu bad_value=%zu bad_t=%zu\n",
            rp.nsteps, rp.coasts, rp.leading_missing, rp.bad_value, rp.bad_t);
    return 0;
}
#endif
