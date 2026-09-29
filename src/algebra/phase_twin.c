/* Omega mixed algebra: phase-domain Z3 digital twin. See phase_twin.h. */
#include "algebra/phase_twin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- RNG */
static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void pt_rng_seed(pt_rng *r, uint64_t seed) {
    uint64_t x = seed;
    for (int i = 0; i < 4; i++) r->s[i] = splitmix64(&x);
    r->has_spare = 0;
    r->spare = 0.0;
}

uint64_t pt_rng_next(pt_rng *r) {
    uint64_t *s = r->s;
    uint64_t result = rotl(s[1] * 5u, 7) * 9u;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

double pt_rng_uniform(pt_rng *r) {
    return (double)(pt_rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

double pt_rng_gauss(pt_rng *r) {
    if (r->has_spare) {
        r->has_spare = 0;
        return r->spare;
    }
    double u, v, s;
    do {
        u = 2.0 * pt_rng_uniform(r) - 1.0;
        v = 2.0 * pt_rng_uniform(r) - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    double m = sqrt(-2.0 * log(s) / s);
    r->spare = v * m;
    r->has_spare = 1;
    return u * m;
}

/* ------------------------------------------------------------ helpers */
static double deg2rad(double d) { return d * (PT_PI / 180.0); }
static double rad2deg(double r) { return r * (180.0 / PT_PI); }

typedef struct { double re, im; } cplx;

static cplx cmul(cplx a, cplx b) {
    cplx c = { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re };
    return c;
}
static cplx crot(cplx a, double rad) {
    cplx r = { cos(rad), sin(rad) };
    return cmul(a, r);
}
static double cabs_(cplx a) { return hypot(a.re, a.im); }
static double carg_(cplx a) { return atan2(a.im, a.re); }

/* Mid-tread quantizer over [-1, 1): 2^bits codes, clipped. bits = 0: ideal. */
static double quant(double v, int bits) {
    if (bits <= 0) return v;
    double half = ldexp(1.0, bits - 1);
    double lsb = 1.0 / half;
    double code = floor(v * half + 0.5);
    if (code > half - 1.0) code = half - 1.0;
    if (code < -half) code = -half;
    return code * lsb;
}

double pt_sigma_for_rho(double amplitude, uint32_t n_samples, double rho_linear) {
    /* rho = A^2 N / (4 sigma^2) */
    return amplitude * sqrt((double)n_samples / (4.0 * rho_linear));
}

int pt_decode(double theta_rad, double *dist_deg) {
    const double third = 2.0 * PT_PI / 3.0;
    double q = floor(theta_rad / third + 0.5);
    double dist = theta_rad - q * third;
    if (dist_deg) *dist_deg = fabs(rad2deg(dist));
    long k = (long)q % 3;
    if (k < 0) k += 3;
    return (int)k;
}

/* --------------------------------------------------------------- twin */
int pt_twin_init(pt_twin *tw, const pt_config *cfg, uint64_t seed) {
    if (!tw || !cfg) return PT_E_ARG;
    memset(tw, 0, sizeof *tw);
    const pt_channel_params *c = &cfg->ch;
    if (c->cycle_samples < 8 || c->n_samples == 0 || c->n_samples % c->cycle_samples)
        return PT_E_ARG;
    if (!(c->amplitude > 0.0) || c->amplitude > 1.0 || c->noise_sigma < 0.0) return PT_E_ARG;
    if (c->dac_bits < 0 || c->dac_bits > 24 || c->adc_bits < 0 || c->adc_bits > 24) return PT_E_ARG;
    if (cfg->mode != PT_ADD_CORR_PRODUCT && cfg->mode != PT_ADD_MIXER && cfg->mode != PT_ADD_NCO)
        return PT_E_ARG;
    if (cfg->cal.enabled && (cfg->cal.interval_trials == 0 || cfg->cal.reps == 0)) return PT_E_ARG;
    tw->cfg = *cfg;
    pt_rng_seed(&tw->rng, seed);
    size_t n = c->n_samples, cyc = c->cycle_samples;
    tw->buf_a = calloc(n, sizeof(double));
    tw->buf_b = calloc(n, sizeof(double));
    tw->buf_m = calloc(n, sizeof(double));
    tw->cos_tab = calloc(cyc, sizeof(double));
    tw->sin_tab = calloc(cyc, sizeof(double));
    if (!tw->buf_a || !tw->buf_b || !tw->buf_m || !tw->cos_tab || !tw->sin_tab) {
        pt_twin_free(tw);
        return PT_E_NOMEM;
    }
    for (size_t i = 0; i < cyc; i++) {
        double w = 2.0 * PT_PI * (double)i / (double)cyc;
        tw->cos_tab[i] = cos(w);
        tw->sin_tab[i] = sin(w);
    }
    /* Uncalibrated defaults: zero offset, ideal magnitudes. */
    double A = c->amplitude, N = (double)c->n_samples;
    tw->cal_mag[0] = tw->cal_mag[1] = A * N / 2.0;
    tw->cal_mix_mag = tw->cal_dbl_mag = A * A * N / 4.0;
    return PT_OK;
}

void pt_twin_free(pt_twin *tw) {
    if (!tw) return;
    free(tw->buf_a);
    free(tw->buf_b);
    free(tw->buf_m);
    free(tw->cos_tab);
    free(tw->sin_tab);
    tw->buf_a = tw->buf_b = tw->buf_m = tw->cos_tab = tw->sin_tab = NULL;
}

/* Emit one tone of logical phase `phase_rad` on channel ch into buf.
 * Path phase (offset + drift at the current time + frame jitter) is applied
 * to the tone before the DAC (declared simplification: the analog path is a
 * pure phase shift at f_c). AWGN is added at the ADC input. */
static void emit(pt_twin *tw, int ch, double phase_rad, double *buf, int use_adc) {
    const pt_channel_params *c = &tw->cfg.ch;
    double path_deg = c->offset_deg[ch] + c->drift_deg_per_s[ch] * tw->t_s;
    if (c->jitter_deg > 0.0) path_deg += c->jitter_deg * pt_rng_gauss(&tw->rng);
    double ph = phase_rad + deg2rad(path_deg);
    double cp = cos(ph), sp = sin(ph);
    uint32_t cyc = c->cycle_samples, N = c->n_samples;
    double A = c->amplitude, sg = c->noise_sigma;
    for (uint32_t n = 0, i = 0; n < N; n++) {
        double v = A * (tw->cos_tab[i] * cp - tw->sin_tab[i] * sp);
        v = quant(v, c->dac_bits);
        if (sg > 0.0) v += sg * pt_rng_gauss(&tw->rng);
        if (use_adc) v = quant(v, c->adc_bits);
        buf[n] = v;
        if (++i == cyc) i = 0;
    }
}

static void end_frame(pt_twin *tw) {
    tw->t_s += tw->cfg.ch.frame_s;
    tw->frames++;
}

/* I/Q correlation at harmonic `bin` of f_c: sum x[n] exp(-j 2 pi bin n / C). */
static cplx correlate(const pt_twin *tw, const double *buf, uint32_t bin) {
    uint32_t cyc = tw->cfg.ch.cycle_samples, N = tw->cfg.ch.n_samples;
    double I = 0.0, Q = 0.0;
    for (uint32_t n = 0, i = 0; n < N; n++) {
        I += buf[n] * tw->cos_tab[i];
        Q -= buf[n] * tw->sin_tab[i];
        i += bin;
        if (i >= cyc) i -= cyc;
    }
    cplx z = { I, Q };
    return z;
}

static double k_phase(int k) { return 2.0 * PT_PI * (double)k / 3.0; }

/* Mixer: both tones enter the mixer without their own ADC; the product is
 * digitized by one ADC and correlated at 2 f_c. */
static cplx mixer_frame(pt_twin *tw, double pa, double pb) {
    emit(tw, 0, pa, tw->buf_a, 0);
    emit(tw, 1, pb, tw->buf_b, 0);
    uint32_t N = tw->cfg.ch.n_samples;
    for (uint32_t n = 0; n < N; n++)
        tw->buf_m[n] = quant(tw->buf_a[n] * tw->buf_b[n], tw->cfg.ch.adc_bits);
    cplx z = correlate(tw, tw->buf_m, 2);
    end_frame(tw);
    return z;
}

/* Frequency doubler (squarer) on channel 0, correlated at 2 f_c. */
static cplx doubler_frame(pt_twin *tw, double pa) {
    emit(tw, 0, pa, tw->buf_a, 0);
    uint32_t N = tw->cfg.ch.n_samples;
    for (uint32_t n = 0; n < N; n++)
        tw->buf_m[n] = quant(tw->buf_a[n] * tw->buf_a[n], tw->cfg.ch.adc_bits);
    cplx z = correlate(tw, tw->buf_m, 2);
    end_frame(tw);
    return z;
}

static int check_120(double meas_rad, double est_rad, double tol_deg) {
    double d = rad2deg(atan2(sin(meas_rad - est_rad), cos(meas_rad - est_rad)));
    return fabs(d - 120.0) <= tol_deg;
}

/* Reference-frame calibration (research note, "Timing and calibration"):
 * emit k = 0 on each path `reps` times, average the correlator outputs,
 * store the phase as the path offset and the magnitude as the erasure
 * reference; then emit k = 1 and require 120 +/- tol after correction. */
static void calibrate(pt_twin *tw) {
    const pt_cal_params *cp = &tw->cfg.cal;
    uint32_t R = cp->reps;
    int fault = 0;
    cplx z0 = { 0, 0 }, z1 = { 0, 0 };
    for (uint32_t r = 0; r < R; r++) {
        emit(tw, 0, 0.0, tw->buf_a, 1);
        emit(tw, 1, 0.0, tw->buf_b, 1);
        cplx a = correlate(tw, tw->buf_a, 1), b = correlate(tw, tw->buf_b, 1);
        z0.re += a.re; z0.im += a.im;
        z1.re += b.re; z1.im += b.im;
        end_frame(tw);
    }
    tw->cal_deg[0] = rad2deg(carg_(z0));
    tw->cal_deg[1] = rad2deg(carg_(z1));
    tw->cal_mag[0] = cabs_(z0) / R;
    tw->cal_mag[1] = cabs_(z1) / R;
    cplx y0 = { 0, 0 }, y1 = { 0, 0 };
    for (uint32_t r = 0; r < R; r++) {
        emit(tw, 0, k_phase(1), tw->buf_a, 1);
        emit(tw, 1, k_phase(1), tw->buf_b, 1);
        cplx a = correlate(tw, tw->buf_a, 1), b = correlate(tw, tw->buf_b, 1);
        y0.re += a.re; y0.im += a.im;
        y1.re += b.re; y1.im += b.im;
        end_frame(tw);
    }
    if (!check_120(carg_(y0), deg2rad(tw->cal_deg[0]), cp->check_tol_deg)) fault = 1;
    if (tw->cfg.mode == PT_ADD_CORR_PRODUCT &&
        !check_120(carg_(y1), deg2rad(tw->cal_deg[1]), cp->check_tol_deg)) fault = 1;
    if (tw->cfg.mode == PT_ADD_MIXER) {
        cplx m = { 0, 0 }, m1 = { 0, 0 };
        for (uint32_t r = 0; r < R; r++) {
            cplx z = mixer_frame(tw, 0.0, 0.0);
            m.re += z.re; m.im += z.im;
        }
        for (uint32_t r = 0; r < R; r++) {
            cplx z = mixer_frame(tw, k_phase(1), 0.0);
            m1.re += z.re; m1.im += z.im;
        }
        tw->cal_mix_deg = rad2deg(carg_(m));
        tw->cal_mix_mag = cabs_(m) / R;
        if (!check_120(carg_(m1), carg_(m), cp->check_tol_deg)) fault = 1;
    }
    if (tw->want_dbl) {
        cplx d = { 0, 0 }, d1 = { 0, 0 };
        for (uint32_t r = 0; r < R; r++) {
            cplx z = doubler_frame(tw, 0.0);
            d.re += z.re; d.im += z.im;
        }
        /* k = 1 doubled is 240 degrees = -120: check 120 on the conjugate. */
        for (uint32_t r = 0; r < R; r++) {
            cplx z = doubler_frame(tw, k_phase(1));
            d1.re += z.re; d1.im += z.im;
        }
        tw->cal_dbl_deg = rad2deg(carg_(d));
        tw->cal_dbl_mag = cabs_(d) / R;
        if (!check_120(carg_(d), carg_(d1), cp->check_tol_deg)) fault = 1;
    }
    tw->cal_valid = 1;
    tw->cal_fault = fault;
    tw->cal_count++;
    if (fault) tw->cal_faults++;
    tw->ops_since_cal = 0;
}

static void maybe_calibrate(pt_twin *tw) {
    if (!tw->cfg.cal.enabled) return;
    if (!tw->cal_valid || tw->ops_since_cal >= tw->cfg.cal.interval_trials) calibrate(tw);
}

static void finish(pt_twin *tw, cplx z, double ref_mag, pt_result *out) {
    double dist;
    double th = carg_(z);
    out->decoded = pt_decode(th, &dist);
    out->phase_deg = rad2deg(th);
    const pt_flag_params *f = &tw->cfg.flags;
    if (f->erasure_frac > 0.0 && cabs_(z) < f->erasure_frac * ref_mag) out->flags |= PT_FLAG_ERASURE;
    if (f->reject_deg > 0.0 && dist > f->reject_deg) out->flags |= PT_FLAG_REJECT;
    if (tw->cfg.cal.enabled && tw->cal_fault) out->flags |= PT_FLAG_CALFAULT;
    tw->ops_since_cal++;
}

int pt_twin_add(pt_twin *tw, int a, int b, pt_result *out) {
    if (!tw || !out || a < 0 || a > 2 || b < 0 || b > 2) return PT_E_ARG;
    memset(out, 0, sizeof *out);
    out->decoded_a = out->decoded_b = -1;
    maybe_calibrate(tw);
    const pt_flag_params *f = &tw->cfg.flags;
    cplx z;
    double ref;
    switch (tw->cfg.mode) {
    case PT_ADD_CORR_PRODUCT: {
        emit(tw, 0, k_phase(a), tw->buf_a, 1);
        emit(tw, 1, k_phase(b), tw->buf_b, 1);
        cplx za = crot(correlate(tw, tw->buf_a, 1), -deg2rad(tw->cal_deg[0]));
        cplx zb = crot(correlate(tw, tw->buf_b, 1), -deg2rad(tw->cal_deg[1]));
        end_frame(tw);
        out->decoded_a = pt_decode(carg_(za), NULL);
        out->decoded_b = pt_decode(carg_(zb), NULL);
        if (f->erasure_frac > 0.0 &&
            (cabs_(za) < f->erasure_frac * tw->cal_mag[0] || cabs_(zb) < f->erasure_frac * tw->cal_mag[1]))
            out->flags |= PT_FLAG_ERASURE;
        z = cmul(za, zb);
        ref = 0.0; /* per-channel erasure checked above */
        break;
    }
    case PT_ADD_MIXER:
        z = crot(mixer_frame(tw, k_phase(a), k_phase(b)), -deg2rad(tw->cal_mix_deg));
        ref = tw->cal_mix_mag;
        break;
    case PT_ADD_NCO: {
        uint32_t word = (uint32_t)a * PT_NCO_THIRD + (uint32_t)b * PT_NCO_THIRD; /* wraps mod 2^32 */
        double ph = 2.0 * PT_PI * ((double)word / 4294967296.0);
        emit(tw, 0, ph, tw->buf_a, 1);
        z = crot(correlate(tw, tw->buf_a, 1), -deg2rad(tw->cal_deg[0]));
        end_frame(tw);
        ref = tw->cal_mag[0];
        break;
    }
    default:
        return PT_E_ARG;
    }
    finish(tw, z, ref, out);
    return PT_OK;
}

int pt_twin_mul_const(pt_twin *tw, int a, int b, pt_result *out) {
    if (!tw || !out || a < 0 || a > 2 || b < 0 || b > 2) return PT_E_ARG;
    memset(out, 0, sizeof *out);
    out->decoded_a = out->decoded_b = -1;
    if (!tw->want_dbl) {
        tw->want_dbl = 1;
        tw->cal_valid = 0; /* force a calibration that includes the doubler path */
    }
    maybe_calibrate(tw);
    cplx z;
    double ref;
    if (b == 0) {
        /* constant source: emit the k = 0 tone regardless of a */
        emit(tw, 0, 0.0, tw->buf_a, 1);
        z = crot(correlate(tw, tw->buf_a, 1), -deg2rad(tw->cal_deg[0]));
        end_frame(tw);
        ref = tw->cal_mag[0];
    } else if (b == 1) {
        emit(tw, 0, k_phase(a), tw->buf_a, 1);
        z = crot(correlate(tw, tw->buf_a, 1), -deg2rad(tw->cal_deg[0]));
        end_frame(tw);
        ref = tw->cal_mag[0];
    } else {
        z = crot(doubler_frame(tw, k_phase(a)), -deg2rad(tw->cal_dbl_deg));
        ref = tw->cal_dbl_mag;
    }
    finish(tw, z, ref, out);
    return PT_OK;
}

/* --------------------------------------------------------- statistics */
pt_interval pt_wilson(uint64_t errors, uint64_t n, double z) {
    pt_interval r = { 0.0, 1.0 };
    if (n == 0) return r;
    double nn = (double)n, p = (double)errors / nn, z2 = z * z;
    double den = 1.0 + z2 / nn;
    double ctr = (p + z2 / (2.0 * nn)) / den;
    double half = z * sqrt(p * (1.0 - p) / nn + z2 / (4.0 * nn * nn)) / den;
    r.lo = ctr - half < 0.0 ? 0.0 : ctr - half;
    r.hi = ctr + half > 1.0 ? 1.0 : ctr + half;
    if (errors == 0) r.lo = 0.0;
    return r;
}

double pt_e1_bound(uint64_t n, double alpha) { return -log(alpha) / (double)n; }

double pt_ref_single_craig(double rho) {
    const int M = 200000;
    double lim = 2.0 * PT_PI / 3.0, h = lim / M, s = 0.0;
    for (int i = 0; i <= M; i++) {
        double th = i * h, v;
        double sn = sin(th);
        v = (sn == 0.0) ? 0.0 : exp(-rho * 0.75 / (sn * sn));
        double w = (i == 0 || i == M) ? 1.0 : ((i & 1) ? 4.0 : 2.0);
        s += w * v;
    }
    return s * h / 3.0 / PT_PI;
}

/* Exact density of the phase of A + noise, rho = A^2 / N0. */
static double psk_phase_pdf(double th, double rho) {
    double c = cos(th), s = sin(th);
    return exp(-rho) / (2.0 * PT_PI) +
           0.5 * sqrt(rho / PT_PI) * c * exp(-rho * s * s) * (1.0 + erf(sqrt(rho) * c));
}

double pt_ref_add_exact(double rho, int grid) {
    if (grid < 60 || grid % 6) return -1.0;
    int M = grid;
    double h = 2.0 * PT_PI / M;
    double *p = malloc((size_t)M * sizeof(double));
    double *c = calloc((size_t)M, sizeof(double));
    if (!p || !c) {
        free(p);
        free(c);
        return -1.0;
    }
    double tot = 0.0;
    for (int j = 0; j < M; j++) {
        p[j] = psk_phase_pdf(-PT_PI + (j + 0.5) * h, rho) * h;
        tot += p[j];
    }
    for (int j = 0; j < M; j++) p[j] /= tot;
    /* theta_j + theta_l = -pi + (j + l + 1 - M/2) h: vertex grid index. */
    for (int j = 0; j < M; j++) {
        double pj = p[j];
        int base = j + 1 - M / 2;
        for (int l = 0; l < M; l++) {
            int m = base + l;
            m %= M;
            if (m < 0) m += M;
            c[m] += pj * p[l];
        }
    }
    int b = M / 3; /* vertex index of -pi/3 is M/3, of +pi/3 is 2M/3 */
    double err = 0.0;
    for (int m = 0; m < M; m++) {
        if (m < b || m > 2 * b) err += c[m];
        else if (m == b || m == 2 * b) err += 0.5 * c[m];
    }
    free(p);
    free(c);
    return err;
}

double pt_ref_add_jitter(double s_deg) {
    double x = 60.0 / (sqrt(2.0) * s_deg);
    return erfc(x / sqrt(2.0)); /* 2 Q(x) */
}
