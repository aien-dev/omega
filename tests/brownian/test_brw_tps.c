/* Tests for the TPS1 binariser. Streams come from xoshiro256** seeded through
 * splitmix64 with fixed constants in this file; nothing is read from disk. */
#include "brownian/brw_tps_adapter.h"
#include "turing/ty_qcont.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint64_t rs[4];
static uint64_t sm(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static void rseed(uint64_t s) { for (int i = 0; i < 4; i++) rs[i] = sm(&s); }
static uint64_t rot(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
static uint64_t rnext(void)
{
    uint64_t r = rot(rs[1] * 5, 7) * 9, t = rs[1] << 17;
    rs[2] ^= rs[0]; rs[3] ^= rs[1]; rs[1] ^= rs[2]; rs[0] ^= rs[3];
    rs[2] ^= t; rs[3] = rot(rs[3], 45);
    return r;
}
static double runif(void) { return (double)(rnext() >> 11) * 0x1p-53; }

/* Normal draw by Box-Muller from the fixed generator. */
static double rnorm(void)
{
    double u = runif(), v = runif();
    if (u < 1e-300) u = 1e-300;
    return sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v);
}

#define N 100000u
static double mu[N], sd[N];
static int64_t bb[N];

static void fill(size_t i, double m, double s, double z)
{
    mu[i] = m; sd[i] = s;
    bb[i] = (int64_t)floor((m + z * s) * 1048576.0);
}

static double worst_excess = -1e300;
static void run(const char *name, size_t n)
{
    size_t bad = 0;
    double mx = 0, mq = 0, me = 0;
    int rc = brw_tps_check_stream(mu, sd, bb, n, &bad, &mx, &mq, &me);
    if (me > worst_excess) worst_excess = me;
    printf("%-32s n=%zu rc=%d max|steps-exact|=%.3g max|steps-qint|=%.3g max(dev-bound)=%.3g\n", name, n, rc, mx, mq, me);
    CHECK(rc == 0);
}

int main(void)
{
    rseed(0x0123456789abcdefull);

    /* A: mixed scales, bin drawn from the predictive itself. Above the qint
     * normalisation band, so the qint gate is 1e-9 as well. */
    for (size_t i = 0; i < 40000; i++) {
        double s = 0.05 * exp(6.0 * runif());          /* ~0.05 .. ~20 */
        fill(i, 200.0 * (runif() - 0.5), s, rnorm());
    }
    run("A mixed scales", 40000);

    /* B: centre bin and small offsets, both signs, including boundaries. */
    for (size_t i = 0; i < 20000; i++) {
        double s = 0.02 + runif();
        double m = (double)(int64_t)((runif() - 0.5) * 2e5) * 0x1p-20;   /* exactly on a bin edge */
        if (i & 1) m += 0.5 * 0x1p-20;                                   /* mid-bin */
        mu[i] = m; sd[i] = s;
        int64_t c = (int64_t)floor(m * 1048576.0);
        bb[i] = c + (int64_t)(rnext() % 9) - 4;
    }
    run("B centre and small offsets", 20000);

    /* C: far tails, 20..40 sd either side. */
    for (size_t i = 0; i < 10000; i++) {
        double z = (20.0 + 20.0 * runif()) * ((i & 1) ? 1.0 : -1.0);
        fill(i, 10.0 * (runif() - 0.5), 0.1 + 3.0 * runif(), z);
    }
    mu[0] = 0.0; sd[0] = 1.0; bb[0] = (int64_t)(40.0 * 1048576.0);
    run("C far tails", 10000);

    /* D: sd at and near sd_min. qint.v1 is unnormalised here; gate on exact
     * mass, and bound the qint gap. */
    for (size_t i = 0; i < 10000; i++) {
        double s = (i % 3 == 0) ? TYQ_SD_MIN : (i % 3 == 1) ? TYQ_SD_MIN * 0.25 : TYQ_SD_MIN * (1.0 + runif());
        fill(i, 4.0 * (runif() - 0.5), s, 6.0 * rnorm() / 3.0);
    }
    run("D sd at sd_min", 10000);
    { size_t bad; double mx, mq, me;
      CHECK(brw_tps_check_stream(mu, sd, bb, 10000, &bad, &mx, &mq, &me) == 0);
      CHECK(mq > 1e-9);   /* the qint gap is real at sd_min, and inside its bound */
    }

    /* G: sd = sd_min, bin offset from mu of 0, 1, 5, 30, 1448, 2048, 4096 bins
     * (ty_qcont's z_c is in sd units; here the offset is counted in bins),
     * both signs, several fractional mu positions. */
    {
        static const int64_t offs[] = { 0, 1, 5, 30, 1448, 2048, 4096 };
        static const double fr[] = { 0.0, 0.25, 0.5, 0.75, 0.999 };
        size_t n = 0;
        for (size_t o = 0; o < sizeof offs / sizeof offs[0]; o++)
            for (int sg = -1; sg <= 1; sg += 2)
                for (size_t f = 0; f < sizeof fr / sizeof fr[0]; f++)
                    for (int m = 0; m < 4; m++) {
                        double mm = ((double)(int64_t)(rnext() % 2000001 - 1000000) + fr[f]) * 0x1p-20 * 1.0 + m * 0.37;
                        mu[n] = mm; sd[n] = TYQ_SD_MIN;
                        bb[n] = (int64_t)floor(mm * 1048576.0) + sg * offs[o];
                        n++;
                    }
        run("G sd_min offsets in bins", n);
    }

    /* H: sd = sd_min, z_c in sd units (as ty_qcont defines it), both signs,
     * several fractional positions. At z = 1448 the exact bin mass is about
     * e^-1e6, far below any floating range; everything runs on log masses. */
    {
        static const double zs[] = { 0, 1, 5, 30, 1448, 2048, 4096 };
        static const double fr[] = { 0.0, 0.3, 0.5, 0.9 };
        size_t n = 0;
        for (size_t o = 0; o < sizeof zs / sizeof zs[0]; o++)
            for (int sg = -1; sg <= 1; sg += 2)
                for (size_t f = 0; f < sizeof fr / sizeof fr[0]; f++)
                    for (int m = 0; m < 5; m++) {
                        double mm = (double)(int64_t)(rnext() % 200001 - 100000) * 0x1p-10 + fr[f] * 0x1p-20;
                        mu[n] = mm; sd[n] = TYQ_SD_MIN;
                        bb[n] = (int64_t)floor(mm * 1048576.0) + sg * (int64_t)(zs[o] * 1024.0) + m;
                        n++;
                    }
        run("H sd_min, z_c in sd units", n);
        tyq_pred p = { 0, TYQ_FAM_GAUSS, 0.0, TYQ_SD_MIN, 0, { { 0, 0, 0 } } };
        double qb = 0; int qr = ty_qcont_bits(&p, (int64_t)(4096 * 1024), &qb, NULL);
        printf("ty_qcont_bits at z_c=4096 sd: rc=%d bits=%.17g\n", qr, qb);
    }

    /* I: reference-style stream: y_prev to y steps are N(0,1), predictor is
     * Normal(y_prev, sd_min), so observations sit about 1000..4000 sd out. */
    {
        double y = 0.0; size_t n = 3000;
        for (size_t i = 0; i < n; i++) {
            double y2 = y + rnorm();
            mu[i] = y; sd[i] = TYQ_SD_MIN;
            bb[i] = (int64_t)floor(y2 * 1048576.0);
            y = y2;
        }
        run("I y_prev predictor, N(0,1) steps", n);
    }

    /* Largest supported offset: |b - centre| < 2^52 bins (2^42 sd at sd_min),
     * with |mu| < 2^31 and |b| < 2^51; beyond that is refused. */
    {
        brw_tps_step st[BRW_TPS_MAX_STEPS]; size_t ns;
        int64_t lim = (INT64_C(1) << 51) - 1;
        CHECK(brw_tps_binarise(0.0, TYQ_SD_MIN, lim, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) == 0);
        CHECK(brw_tps_binarise(0.0, TYQ_SD_MIN, -lim, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) == 0);
        CHECK(brw_tps_binarise(0.0, TYQ_SD_MIN, lim + 1, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
        CHECK(brw_tps_binarise(2147483647.0, TYQ_SD_MIN, -lim, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) == 0);
        CHECK(brw_tps_binarise(-2147483647.0, TYQ_SD_MIN, lim, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) == 0);
    }

    /* E: huge sd. */
    for (size_t i = 0; i < 10000; i++) {
        double s = (i & 1) ? 1e6 : 1e9 * (1.0 + runif());
        fill(i, 1e4 * (runif() - 0.5), s, rnorm());
        if (fabs(mu[i] + 5.0 * s) >= 2147483648.0 / 2) { double z = 0.2 * rnorm(); fill(i, mu[i], s, z); }
        int64_t lim = (INT64_C(1) << 51) - 1;
        if (bb[i] > lim) bb[i] = lim;
        if (bb[i] < -lim) bb[i] = -lim;
    }
    run("E huge sd", 10000);

    /* F: mu at non-integer bin boundaries, fractions of a bin. */
    for (size_t i = 0; i < 10000; i++) {
        double s = 0.01 + 5.0 * runif();
        double frac = (double)(rnext() % 1000) / 1000.0;
        double m = ((double)(int64_t)((runif() - 0.5) * 1e6) + frac) * 0x1p-20;
        mu[i] = m; sd[i] = s;
        bb[i] = (int64_t)floor(m * 1048576.0) + (int64_t)(rnorm() * s * 1048576.0);
    }
    run("F fractional bin position", 10000);

    /* Frequency rows. */
    { uint16_t f[2];
      double ps[] = { 0.0, 1e-300, 5e-6, 0.5, 1.0 - 1e-9, 1.0 };
      for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++) {
          CHECK(brw_tps_p1_to_freq(ps[i], f) == 0);
          CHECK(f[0] >= 1 && f[1] >= 1 && (unsigned)f[0] + f[1] == 65536u);
      }
      CHECK(brw_tps_p1_to_freq(NAN, f) < 0);
    }

    /* Hostile inputs never crash and return negative codes. */
    { brw_tps_step st[BRW_TPS_MAX_STEPS]; size_t ns;
      const int64_t big = INT64_C(1) << 51;
      CHECK(brw_tps_binarise(NAN, 1.0, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, NAN, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, 0.0, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, -1.0, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, INFINITY, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(INFINITY, 1.0, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(1e300, 1.0, 0, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, 1.0, big, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, 1.0, -big, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, 1.0, INT64_MIN, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, 1.0, INT64_MAX, BRW_TPS_MAX_STEPS, st, &ns, NULL, NULL) < 0);
      CHECK(brw_tps_binarise(0.0, 1.0, 5000, 2, st, &ns, NULL, NULL) < 0);   /* cap too small */
      CHECK(brw_tps_binarise(0.0, 1.0, 0, 4, NULL, &ns, NULL, NULL) < 0);
      double m1[1] = { NAN }, s1[1] = { 1.0 }; int64_t b1[1] = { 0 };
      size_t bad; double x, y, z;
      CHECK(brw_tps_check_stream(m1, s1, b1, 1, &bad, &x, &y, &z) < 0);
      CHECK(brw_tps_check_stream(NULL, s1, b1, 1, &bad, &x, &y, &z) < 0);
    }

    printf("worst max(|steps-qint| - bound) over all streams = %.3g\n", worst_excess);
    CHECK(worst_excess <= 1e-9);

    printf(fails ? "test_brw_tps: FAIL (%d)\n" : "test_brw_tps: PASS\n", fails);
    return fails ? 1 : 0;
}
