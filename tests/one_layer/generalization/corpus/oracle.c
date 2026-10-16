/* tests/one_layer/generalization/corpus/oracle.c -- independent C oracle for the GEN-3 public corpus
 * (aien-spark/omega#478). `oracle <task_id> < inputs > expected`: one input line (decimal arguments, as the
 * harness reads them) gives one expected line, the decimal result or `TRAP 2` (division by zero, the only
 * trap these tasks have). Written from each task's prompt, separately from the Omegese solutions and the OSC
 * references, so a shared mistake has to be made three times. Unsigned 32-bit arithmetic wraps modulo 2^32, as
 * in OSC-1 and Omegese (docs/osc/OSC-2-DESIGN.md: wrap for unsigned types); it is computed in 64 bits and masked. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M32 0xffffffffull
typedef uint64_t u;
static int trap; /* 0, or the trap code of the current case */
static u chk(u v) { return v & M32; } /* unsigned 32-bit wrap */

static u clampv(u x, u lo, u hi) { return x < lo ? lo : x > hi ? hi : x; }
static u adiff(u a, u b) { return a > b ? a - b : b - a; }
static u sat8(u x) { return x > 255 ? 255 : x; }
static u dsum(u n) { u s = 0; do { s += n % 10; n /= 10; } while (n); return s; }
static u dcount(u n) { u c = 1; while (n >= 10) { c++; n /= 10; } return c; }
static u pcount(u x) { u c = 0; for (; x; x >>= 1) c += x & 1; return c; }
static u gcdv(u a, u b) { while (b) { u t = a % b; a = b; b = t; } return a; }
static u max4(const u *v) { u m = v[0]; for (int i = 1; i < 4; i++) if (v[i] > m) m = v[i]; return m; }
static u sum4(const u *v) { return v[0] + v[1] + v[2] + v[3]; }
static u avg2(u a, u b) { return (a + b) / 2; } /* exact in 64 bits */

static u task(const char *t, const u *a)
{
#define T(name) if (!strcmp(t, name))
    T("clamp_sum")        return clampv(chk(a[0] + a[1]), 10, 100);
    T("clamp_triple")     return clampv(chk(3 * a[0]), 50, 1000);
    T("gap_inc")          return chk(adiff(a[0], a[1]) + 1);
    T("gap_half")         return adiff(a[0], a[1]) / 2;
    T("sat_sum")          return sat8(chk(a[0] + a[1]));
    T("sat_prod")         return sat8(chk(a[0] * a[1]));
    T("digsum_mod9")      return dsum(a[0]) % 9;
    T("digsum_scaled")    return chk(dsum(a[0]) * a[1]);
    T("digcount_nibbles") return 4 * dcount(a[0]);
    T("popcnt_zero")      return 32 - pcount(a[0]);
    T("popcnt_odd")       return pcount(a[0]) & 1;
    T("lcm32")            return (a[0] == 0 || a[1] == 0) ? 0 : chk(a[0] / gcdv(a[0], a[1]) * a[1]);
    T("ds_plus_dc")       return dsum(a[0]) + dcount(a[0]);
    T("pc_times_dc")      return pcount(a[0]) * dcount(a[0]);
    T("gcd_plus_ds")      return chk(gcdv(a[0], a[1]) + dsum(a[0]));
    T("gcd_times_pc")     return chk(gcdv(a[0], a[1]) * pcount(a[1]));
    T("sum4_avg")         return sum4(a) / 4;
    T("sum4_mod")         { if (a[4] == 0) { trap = 2; return 0; } return sum4(a) % a[4]; }
    T("sum4_low")         return sum4(a) & 65535;
    T("max4_inc")         return chk(max4(a) + 1);
    T("max4_half")        return max4(a) / 2;
    T("sum_clamped")      { u s = 0; for (u i = 0; i < a[0]; i++) s = chk(s + clampv(i, a[1], a[2])); return s; }
    T("sum_dists")        { u s = 0; for (u i = 0; i < a[0]; i++) s = chk(s + adiff(i, a[1])); return s; }
    T("sum_sat")          { u s = 0; for (u i = 0; i < a[0]; i++) s = chk(s + sat8(chk(i * a[1]))); return s; }
    T("even_ds_odd_dc")   return a[0] % 2 == 0 ? dsum(a[0]) : dcount(a[0]);
    T("big_pc_small_ds")  return a[0] >= 1000 ? pcount(a[0]) : dsum(a[0]);
    T("gcd_or_dc")        return a[0] > a[1] ? gcdv(a[0], a[1]) : dcount(a[1]);
    T("avg_inc")          return chk(avg2(a[0], a[1]) + 1);
    T("avg_double")       return chk(2 * avg2(a[0], a[1]));
    T("cube_inc")         return chk(chk(chk(a[0] * a[0]) * a[0]) + 1);
    T("gcd_only")         return gcdv(a[0], a[1]);
    T("ds2_inc")          return dsum(dsum(a[0])) + 1;
    T("ds2_double")       return 2 * dsum(dsum(a[0]));
    /* transfer probes (withheld from extraction) */
    T("clamp_gap")        return clampv(adiff(a[0], a[1]), a[2], a[3]);
    T("sat_affine")       return sat8(chk(chk(a[0] * a[1]) + a[2]));
    T("even_digsums")     { u c = 0; for (u i = 0; i < a[0]; i++) c += dsum(i) % 2 == 0; return c; }
    T("hamming")          return pcount(a[0] ^ a[1]);
    T("spread4")          return 4 * max4(a) - sum4(a);
    T("avg4")             return avg2(avg2(a[0], a[1]), avg2(a[2], a[3]));
    T("digits_both")      return dcount(a[0]) + dcount(a[1]);
    T("wrap_range")       { u d = chk(a[2] - a[1]); if (d == 0) { trap = 2; return 0; } return chk(a[1] + a[0] % d); }
    T("wrap_diff")        return chk(a[0] - a[1]);
    T("wrap8")            return a[0] & 255;
    T("digit_product")    { if (a[0] == 0) return 0; u p = 1, n = a[0]; while (n) { p *= n % 10; n /= 10; } return p; }
    T("hex_digits")       { u c = 1, n = a[0]; while (n >= 16) { c++; n /= 16; } return c; }
    T("zero_bits8")       return 8 - pcount(a[0] & 255);
    T("weighted4")        return a[0] + 2 * a[1] + 3 * a[2] + 4 * a[3];
    T("second_max")       { u v[4]; memcpy(v, a, sizeof v);
                            for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) if (v[j] > v[i]) { u x = v[i]; v[i] = v[j]; v[j] = x; }
                            return v[1]; }
    T("ceil_avg")         return (a[0] + a[1] + 1) / 2;
#undef T
    fprintf(stderr, "oracle: unknown task '%s'\n", t);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: oracle <task_id> < inputs > expected\n"); return 2; }
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        u a[8] = {0};
        int n = 0;
        char *p = line, *e;
        while (n < 8) {
            while (*p == ' ') p++;
            if (*p == '\n' || !*p) break;
            a[n++] = strtoull(p, &e, 10);
            if (e == p || a[n - 1] > M32) { fprintf(stderr, "oracle: bad input line\n"); return 2; }
            p = e;
        }
        trap = 0;
        u r = task(argv[1], a);
        if (trap) printf("TRAP %d\n", trap); else printf("%llu\n", (unsigned long long)r);
    }
    return 0;
}
