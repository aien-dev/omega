#include "est_replay.h"

#include <ctype.h>
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
static int sums_lookup(const char *dir, const char *name, char hex[65])
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

int est_file_load(const char *path, est_file *f, char *err, size_t errcap)
{
    memset(f, 0, sizeof *f);
    if (strlen(path) >= sizeof f->path) { setmsg(err, errcap, "%s: path too long%s", path, ""); return 1; }
    snprintf(f->path, sizeof f->path, "%s", path);
    char dir[1024]; const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (slash) { size_t n = (size_t)(slash - path); memcpy(dir, path, n); dir[n] = 0; }
    else snprintf(dir, sizeof dir, ".");
    char want[65];
    if (sums_lookup(dir, name, want)) { setmsg(err, errcap, "%s: no SHA256SUMS line for %s", dir, name); return 1; }
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
    if (strcmp(f->sha_hex, want) != 0) {
        free(buf);
        setmsg(err, errcap, "SHA-256 mismatch for %s (SHA256SUMS says %s)", path, want);
        return 1;
    }
    f->data = buf; f->len = len;
    size_t nl = 0;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') nl++;
    if (len && buf[len - 1] != '\n') nl++;
    f->off = malloc((nl + 1) * sizeof(size_t)); f->llen = malloc((nl + 1) * sizeof(size_t));
    if (!f->off || !f->llen) return 1;
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
        if (i >= 12) return 1;
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
    *t_ns = sec * NS + frac;
    if (used) *used = i;
    return 0;
}

int est_parse_t(const char *line, size_t len, int64_t *t_ns)
{
    static const char pre[] = "{\"t\":";
    if (len < sizeof pre - 1 || memcmp(line, pre, sizeof pre - 1) != 0) return 1;
    size_t used;
    if (est_parse_time_prefix(line + 5, len - 5, t_ns, &used)) return 1;
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
    if (model_id == 0) {
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
        int t_ok = (est_parse_t(ln, ll, &wall) == 0);
        int v_ok = (est_parse_thermal0(ln, ll, &z) == 0);
        if (!t_ok) rp->bad_t++;
        if (!v_ok) rp->bad_value++;
        uint8_t ev[32]; sha256_hash((const uint8_t *)ln, ll, ev);
        if (!have) {
            if (!v_ok) { rp->leading_missing++; if (t_ok) { prev_wall = wall; prev_ok = 1; } continue; }
            double x0[2] = { z, 0.0 }, P0[4] = { 0 };
            if (model_id == 0) P0[0] = r; else { P0[0] = r; P0[3] = 1e6; }
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
            if (gap > 0) {
                int64_t hr = (gap + NS / 2) / NS; /* round half up */
                if (hr < 1) hr = 1;
                if (hr > 1000000) hr = 1000000;
                h = (uint32_t)hr;
            }
        }
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

int est_params_read(const char *path, est_params *p)
{
    memset(p, 0, sizeof *p);
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    char line[1300]; int ok = 0, got = 0;
    while (fgets(line, sizeof line, fp)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (strcmp(line, "est_params_v1") == 0) ok = 1;
        else if (strncmp(line, "fit_run_path ", 13) == 0) { snprintf(p->fit_path, sizeof p->fit_path, "%.1023s", line + 13); got |= 1; }
        else if (strncmp(line, "fit_file_sha256 ", 16) == 0 && l == 16 + 64) { memcpy(p->fit_sha, line + 16, 65); got |= 2; }
        else if (strncmp(line, "fit_lines ", 10) == 0) { p->fit_lines = (size_t)strtoull(line + 10, NULL, 10); }
        else if (line[0] == 'M' && (line[1] == '0' || line[1] == '1')) {
            int m = line[1] - '0';
            if (sscanf(line + 2, " q %lf r %lf loglik %lf", &p->q[m], &p->r[m], &p->ll[m]) == 3) got |= (4 << m);
        }
    }
    fclose(fp);
    return (ok && got == 15) ? 0 : 1;
}

#ifdef EST_REPLAY_MAIN
/* est_replay [--recorded] <model 0|1> <q> <r> <machine-state.ndjson>
 * Prints the full observation/prediction/innovation stream, one line per step. */
int main(int argc, char **argv)
{
    int rec = 0, a = 1;
    if (a < argc && strcmp(argv[a], "--recorded") == 0) { rec = 1; a++; }
    if (argc - a != 4) { fprintf(stderr, "usage: est_replay [--recorded] model q r file\n"); return 2; }
    if (strstr(argv[a + 3], EST_RUN_B_ID) && !rec) {
        fprintf(stderr, "refusing run B without --recorded (protocol section 6)\n"); return 2;
    }
    est_file f; char err[300];
    if (est_file_load(argv[a + 3], &f, err, sizeof err)) { fprintf(stderr, "refuse: %s\n", err); return 1; }
    static est_replay rp;
    if (est_replay_run(&f, (size_t)-1, atoi(argv[a]), atof(argv[a + 1]), atof(argv[a + 2]), &rp)) {
        fprintf(stderr, "replay failed: %s\n", rp.errmsg); return 1;
    }
    est_replay_write_stream(stdout, &rp, atoi(argv[a]));
    fprintf(stderr, "steps=%zu coasts=%zu leading_missing=%zu bad_value=%zu bad_t=%zu\n",
            rp.nsteps, rp.coasts, rp.leading_missing, rp.bad_value, rp.bad_t);
    return 0;
}
#endif
