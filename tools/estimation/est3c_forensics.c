/* est3c_forensics.c: read-only forensics of the EST-3 v1 and v2 FAILs
 * (ESTIMATION-2, protocol v3 preparation). Deterministic; writes nothing.
 *
 * Inputs (paths relative to the omega repo root, the working directory):
 *   A  = fit run of v1, B = held-out run of v1 (now a development artifact),
 *   C1 = v2 idle fit run, the v1 M0/M1 step streams and the v1 receipt.json.
 * Nothing under evidence/EST3C/ is read.
 *
 * Definitions (printed again in the output):
 *   line index L counts from the first line of the file (L = 0).
 *   one-step change d_L = z_L - z_{L-1}, z = first field of thermal_mc (mC),
 *   kept when L >= 30 and the rounded wall gap is exactly 1 s (the v2
 *   pre-check rule). Gap rounding: (gap_ns + 5e8) / 1e9, t parsed as
 *   integer seconds plus nine fractional digits.
 *
 * Build: cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
 *          -o /tmp/est3c_forensics tools/estimation/est3c_forensics.c -lm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 8192
#define MAXM 1024
#define BURN 30
#define NS 1000000000LL
#define CELL 50.0           /* half width of a 100 mC reading cell */

static const char *PATH_A = "evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/machine-state.ndjson";
static const char *PATH_B = "evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/machine-state.ndjson";
static const char *MARK_A = "evidence/R15/raw/20260929T020536Z-ad8e1f2ea4e4-silicon/machine-state-marks.txt";
static const char *MARK_B = "evidence/R15/raw/20260929T025735Z-3e9e53be3358-silicon/machine-state-marks.txt";
static const char *PATH_C1 = "evidence/EST3B/raw/20261001T020412Z-est3b-fit-silicon/machine-state.ndjson";
static const char *STREAM[2] = { "docs/estimation/receipts/est23-v1/receipt.json.M0.stream",
                                 "docs/estimation/receipts/est23-v1/receipt.json.M1.stream" };
static const char *RECEIPT = "docs/estimation/receipts/est23-v1/receipt.json";

static void die(const char *m, const char *p) { fprintf(stderr, "est3c_forensics: %s: %s\n", m, p); exit(2); }

/* "1790647557.392107561" -> ns; returns 0 on failure */
static int parse_t(const char *s, int64_t *out) {
    char *e; long long sec = strtoll(s, &e, 10);
    if (e == s || *e != '.') return 0;
    int64_t frac = 0; int k = 0;
    for (e++; k < 9; k++, e++) { if (*e < '0' || *e > '9') return 0; frac = frac * 10 + (*e - '0'); }
    *out = (int64_t)sec * NS + frac; return 1;
}

typedef struct { size_t n, bad; int64_t t[MAXN]; double z[MAXN]; } series;

static void read_series(const char *path, series *s) {
    FILE *f = fopen(path, "r"); if (!f) die("cannot open", path);
    char buf[8192]; s->n = 0; s->bad = 0;
    while (fgets(buf, sizeof buf, f)) {
        if (!strchr(buf, '\n')) die("line too long or missing newline", path);
        const char *pt = strstr(buf, "\"t\":"), *pz = strstr(buf, "\"thermal_mc\":\"");
        int64_t t; char *e;
        if (!pt || !pz || !parse_t(pt + 4, &t)) { s->bad++; continue; }
        double z = strtod(pz + 14, &e);
        if (e == pz + 14 || !isfinite(z)) { s->bad++; continue; }
        if (s->n >= MAXN) die("too many lines", path);
        s->t[s->n] = t; s->z[s->n] = z; s->n++;
    }
    fclose(f);
}

typedef struct { size_t n; int64_t t[MAXM]; int begin[MAXM]; } marks;

static void read_marks(const char *path, marks *m) {
    FILE *f = fopen(path, "r"); if (!f) die("cannot open", path);
    char buf[1024]; m->n = 0;
    while (fgets(buf, sizeof buf, f)) {
        int64_t t; if (!parse_t(buf, &t)) die("bad mark time", path);
        if (m->n >= MAXM) die("too many marks", path);
        m->t[m->n] = t; m->begin[m->n] = strstr(buf, " begin ") != NULL; m->n++;
    }
    fclose(f);
}

typedef struct { size_t n, multi; double d[MAXN]; size_t L[MAXN]; } changes;

static void make_changes(const series *s, changes *c) {
    c->n = 0; c->multi = 0;
    for (size_t L = 1; L < s->n; L++) {
        int64_t h = (s->t[L] - s->t[L - 1] + NS / 2) / NS;
        if (h < 1) h = 1;
        if (L < BURN) continue;
        if (h != 1) { c->multi++; continue; }
        c->d[c->n] = s->z[L] - s->z[L - 1]; c->L[c->n] = L; c->n++;
    }
}

typedef struct { size_t n, zero, big; double mean, sd, exkurt, maxabs; } dstat;

static dstat describe(const double *x, size_t n) {
    dstat r; memset(&r, 0, sizeof r); r.n = n; if (n < 4) return r;
    double m = 0; for (size_t i = 0; i < n; i++) m += x[i]; m /= (double)n;
    double m2 = 0, m4 = 0;
    for (size_t i = 0; i < n; i++) {
        double e = x[i] - m; m2 += e * e; m4 += e * e * e * e;
        if (x[i] == 0.0) r.zero++;
        if (fabs(x[i]) >= 1000.0) r.big++;
        if (fabs(x[i]) > r.maxabs) r.maxabs = fabs(x[i]);
    }
    m2 /= (double)n; m4 /= (double)n;
    r.mean = m; r.sd = sqrt(m2); r.exkurt = m2 > 0 ? m4 / (m2 * m2) - 3.0 : 0.0;
    return r;
}

/* autocorrelation r_1..r_10 and Ljung-Box Q10 of the mean-subtracted series */
static double acf10(const double *x, size_t n, double r[11]) {
    double m = 0; for (size_t i = 0; i < n; i++) m += x[i]; m /= (double)n;
    double den = 0; for (size_t i = 0; i < n; i++) den += (x[i] - m) * (x[i] - m);
    double lb = 0, dn = (double)n;
    for (size_t k = 1; k <= 10; k++) {
        double num = 0; for (size_t i = 0; i + k < n; i++) num += (x[i] - m) * (x[i + k] - m);
        r[k] = den > 0 ? num / den : 0; lb += r[k] * r[k] / (dn - (double)k);
    }
    return dn * (dn + 2.0) * lb;
}

static double Phi(double x) { return 0.5 * erfc(-x / sqrt(2.0)); }

static void print_dstat(const char *tag, const dstat *r) {
    double pz = r->sd > 0 ? erf(CELL / (r->sd * sqrt(2.0))) : 1.0;
    printf("  %-22s n=%5zu sd=%7.1f exkurt=%7.2f max|d|=%6.0f zero=%zu (%.4f; N(0,sd) P|d|<=50: %.4f) |d|>=1000: %.4f\n",
           tag, r->n, r->sd, r->exkurt, r->maxabs, r->zero, r->n ? (double)r->zero / (double)r->n : 0.0, pz,
           r->n ? (double)r->big / (double)r->n : 0.0);
}

/* ---------- v1 stream reproduction ---------- */
typedef struct { size_t L; int prior, coast; long h; double z, y, S, nu, nis; } step;
static step ST[MAXN];

static double kv(const char *line, const char *key, int *ok) {
    char pat[32]; snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(line, pat); if (!p) { *ok = 0; return 0; }
    char *e; double v = strtod(p + strlen(pat), &e); if (e == p + strlen(pat)) *ok = 0;
    return v;
}

static size_t read_stream(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) die("cannot open", path);
    char buf[2048]; size_t n = 0;
    while (fgets(buf, sizeof buf, f)) {
        if (!strchr(buf, '\n')) die("stream line too long", path);
        if (n >= MAXN) die("stream too long", path);
        int ok = 1; step *s = &ST[n];
        s->L = (size_t)kv(buf, "L", &ok); s->h = (long)kv(buf, "h", &ok);
        s->z = kv(buf, "z", &ok); s->y = kv(buf, "y", &ok); s->S = kv(buf, "S", &ok);
        s->nu = kv(buf, "nu", &ok); s->nis = kv(buf, "nis", &ok);
        s->prior = strstr(buf, " prior ") != NULL; s->coast = strstr(buf, " coast ") != NULL;
        if (!ok) die("bad stream line", path);
        n++;
    }
    fclose(f); return n;
}

/* first number after "key": {"value": or "key": inside the model block */
static double rec(const char *blk, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(blk, pat); if (!p) die("receipt key missing", key);
    p += strlen(pat); while (*p == ' ') p++;
    if (*p == '{') { p = strstr(p, "\"value\":"); if (!p) die("receipt value missing", key); p += 8; }
    return strtod(p, NULL);
}

static char RC[65536];

static void cmp(const char *name, double recd, double mine) {
    printf("  %-14s recorded=%.17g recomputed=%.17g |diff|=%.3g%s\n", name, recd, mine, fabs(recd - mine),
           recd == mine ? " EXACT" : "");
}

static void v1_model(int m) {
    size_t n_all = read_stream(STREAM[m]);
    static double w[MAXN];
    size_t n = 0, hit[3] = { 0, 0, 0 }, zero = 0;
    double snis = 0, sse = 0, ssp = 0, prev_z = 0, numu = 0, nus[MAXN];
    const double zc[3] = { 0.674490, 1.281552, 1.959964 }, lv[3] = { 0.50, 0.80, 0.95 };
    double frac[3] = { 0, 0, 0 }, pit[10] = { 0 };
    for (size_t i = 0; i < n_all; i++) {
        const step *s = &ST[i];
        if (s->prior) { prev_z = s->z; continue; }
        if (s->coast) continue;
        if (s->L >= BURN) {
            double sd = sqrt(s->S), a = fabs(s->nu);
            for (int k = 0; k < 3; k++) if (a <= zc[k] * sd) hit[k]++;
            snis += s->nis; sse += s->nu * s->nu; numu += s->nu; nus[n] = s->nu;
            if (s->nu == 0.0) zero++;
            double e = s->z - prev_z; ssp += e * e;
            w[n] = s->nu / sd;
            /* fractional (randomized-PIT) coverage: outcome cell (z-50, z+50] */
            double Flo = Phi((s->nu - CELL) / sd), Fhi = Phi((s->nu + CELL) / sd), wd = Fhi - Flo;
            for (int k = 0; k < 3; k++) {
                double lo = (1.0 - lv[k]) / 2.0, hi = 1.0 - lo;
                if (wd > 0) { double ov = fmin(hi, Fhi) - fmax(lo, Flo); frac[k] += ov > 0 ? ov / wd : 0; }
                else frac[k] += (Flo >= lo && Flo <= hi) ? 1.0 : 0.0;
            }
            for (int b = 0; b < 10; b++) {
                double lo = b / 10.0, hi = (b + 1) / 10.0;
                if (wd > 0) { double ov = fmin(hi, Fhi) - fmax(lo, Flo); pit[b] += ov > 0 ? ov / wd : 0; }
                else if (Flo >= lo && (Flo < hi || b == 9)) pit[b] += 1.0;
            }
            n++;
        }
        prev_z = s->z;
    }
    double dn = (double)n, r[11];
    double q10 = acf10(w, n, r);
    double mn = numu / dn, v = 0; for (size_t i = 0; i < n; i++) v += (nus[i] - mn) * (nus[i] - mn);
    const char *mod = strstr(RC, "\"models\""); if (!mod) die("models block", "receipt");
    const char *blk = strstr(mod, m == 0 ? "\"M0\": {" : "\"M1\": {"); if (!blk) die("model block", "receipt");
    printf("v1 %s on B (stream, L>=%d): n=%zu (recorded %.0f)\n", m ? "M1" : "M0", BURN, n, rec(blk, "samples"));
    cmp("coverage_50", rec(blk, "coverage_50"), hit[0] / dn);
    cmp("coverage_80", rec(blk, "coverage_80"), hit[1] / dn);
    cmp("coverage_95", rec(blk, "coverage_95"), hit[2] / dn);
    cmp("mean_nis", rec(blk, "mean_nis"), snis / dn);
    cmp("std_bias", rec(blk, "standardized_bias"), mn / sqrt(v / (dn - 1.0)));
    cmp("lag1", rec(blk, "lag1_autocorr"), r[1]);
    cmp("LB_Q10", rec(blk, "ljung_box_q10_reported_only"), q10);
    cmp("rmse", rec(blk, "rmse_one_step"), sqrt(sse / dn));
    cmp("persist_rmse", rec(blk, "persistence_rmse"), sqrt(ssp / dn));
    cmp("zero_innov", rec(blk, "zero_innovation_fraction_extra"), (double)zero / dn);
    printf("  fractional coverage (cell z+-50): 50%%=%.4f 80%%=%.4f 95%%=%.4f  (point: %.4f %.4f %.4f)\n",
           frac[0] / dn, frac[1] / dn, frac[2] / dn, hit[0] / dn, hit[1] / dn, hit[2] / dn);
    printf("  randomized PIT deciles (expected mass, ideal 0.100):");
    for (int b = 0; b < 10; b++) printf(" %.3f", pit[b] / dn);
    printf("\n");
}

/* ---------- per-run dependence, tails, regime split, EWMA ---------- */
static void run_report(const char *name, const char *path, const char *mpath, int ewma) {
    static series s; static changes c; static marks mk;
    read_series(path, &s); make_changes(&s, &c);
    printf("\n%s: lines=%zu unparsed=%zu kept changes (L>=30, gap=1s)=%zu excluded multi-horizon=%zu\n",
           name, s.n + s.bad, s.bad, c.n, c.multi);
    dstat all = describe(c.d, c.n);
    print_dstat("all changes", &all);
    if (mpath) {
        read_marks(mpath, &mk);
        static double din[MAXN], dsp[MAXN]; size_t nin = 0, nsp = 0, outside = 0, begins = 0;
        for (size_t j = 0; j < mk.n; j++) begins += (size_t)mk.begin[j];
        for (size_t L = 0; L < s.n; L++) {      /* sample membership in any begin..end pair */
            int in = 0;
            for (size_t j = 0; j + 1 < mk.n; j++)
                if (mk.begin[j] && !mk.begin[j + 1] && s.t[L] >= mk.t[j] && s.t[L] <= mk.t[j + 1]) { in = 1; break; }
            if (!in) outside++;
        }
        for (size_t i = 0; i < c.n; i++) {
            int64_t t0 = s.t[c.L[i] - 1], t1 = s.t[c.L[i]]; int span = 0;
            for (size_t j = 0; j < mk.n; j++) if (mk.t[j] > t0 && mk.t[j] <= t1) { span = 1; break; }
            if (span) dsp[nsp++] = c.d[i]; else din[nin++] = c.d[i];
        }
        printf("  marks: %zu lines, %zu begins; samples outside every begin..end pair: %zu of %zu\n",
               mk.n, begins, outside, s.n);
        dstat a = describe(din, nin), b = describe(dsp, nsp);
        print_dstat("within one trial", &a);
        print_dstat("spans a trial mark", &b);
    }
    double r[11], ra[11]; static double ad[MAXN];
    for (size_t i = 0; i < c.n; i++) ad[i] = fabs(c.d[i]);
    double q = acf10(c.d, c.n, r), qa = acf10(ad, c.n, ra);
    printf("  acf d   lags1-10:"); for (int k = 1; k <= 10; k++) printf(" %+.3f", r[k]); printf("  LB Q10=%.1f\n", q);
    printf("  acf |d| lags1-10:"); for (int k = 1; k <= 10; k++) printf(" %+.3f", ra[k]); printf("  LB Q10=%.1f\n", qa);
    if (!ewma) return;
    /* EWMA scale, strictly causal: sigma2 at L uses changes up to L-1.
       Init = mean d^2 over gap-1 changes with 1 <= L < 30. Scale floor 100^2/12
       (uniform quantization variance) or 100^2 (one reading step). */
    const double lam[2] = { 0.9, 0.97 }, fl = 10000.0 / 12.0;
    double init = 0; size_t ni = 0;
    for (size_t L = 1; L < BURN && L < s.n; L++) {
        int64_t h = (s.t[L] - s.t[L - 1] + NS / 2) / NS;
        if (h == 1) { double d = s.z[L] - s.z[L - 1]; init += d * d; ni++; }
    }
    init = ni ? init / (double)ni : fl; if (init < fl) init = fl;
    {
        size_t h50 = 0, h95 = 0; for (size_t i = 0; i < c.n; i++) { double a = fabs(c.d[i]) / all.sd; h50 += a <= 0.674490; h95 += a <= 1.959964; }
        printf("  fixed scale (in-sample sd):  exkurt=%7.2f cov50=%.4f cov95=%.4f\n", all.exkurt, h50 / (double)c.n, h95 / (double)c.n);
    }
    for (int k = 0; k < 4; k++) { const double flk = (k < 2) ? fl : 10000.0; const double lk = lam[k % 2];
        static double zz[MAXN]; size_t m = 0, h50 = 0, h95 = 0; double s2 = init;
        for (size_t L = 1; L < s.n; L++) {
            int64_t h = (s.t[L] - s.t[L - 1] + NS / 2) / NS; double d = s.z[L] - s.z[L - 1];
            if (L >= BURN && h == 1) {
                double sc = sqrt(s2 < flk ? flk : s2); zz[m] = d / sc;
                h50 += fabs(zz[m]) <= 0.674490; h95 += fabs(zz[m]) <= 1.959964; m++;
            }
            if (h == 1) s2 = lk * s2 + (1.0 - lk) * d * d;
        }
        dstat z = describe(zz, m);
        printf("  EWMA lambda=%.2f floor=%5.0f mC^2 (init %.0f): exkurt=%7.2f cov50=%.4f cov95=%.4f sd(z)=%.3f max|z|=%.1f n=%zu\n",
               lk, flk, init, z.exkurt, h50 / (double)m, h95 / (double)m, z.sd, z.maxabs, m);
    }
}

int main(void) {
    FILE *f = fopen(RECEIPT, "r"); if (!f) die("cannot open", RECEIPT);
    size_t got = fread(RC, 1, sizeof RC - 1, f); RC[got] = 0; fclose(f);
    if (got == sizeof RC - 1) die("receipt too large", RECEIPT);
    printf("est3c_forensics v1. change d_L = z_L - z_{L-1}, kept for L>=30 and rounded gap = 1 s.\n");
    printf("exkurt = m4/m2^2 - 3 (population moments). LB Q10 = n(n+2) sum_{k=1..10} r_k^2/(n-k).\n\n");
    v1_model(0);
    v1_model(1);
    run_report("A (v1 fit, loaded)", PATH_A, MARK_A, 1);
    run_report("B (v1 held-out, loaded)", PATH_B, MARK_B, 1);
    run_report("C1 (v2 fit, idle; no marks)", PATH_C1, NULL, 0);
    return 0;
}
