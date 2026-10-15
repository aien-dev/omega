/*
 * examples_expected.c -- independent C computation of the expected results of the exp445 worked
 * examples (omega#445, docs/one_layer/EXP445_PROTOCOL.md section 13.2; tools/exp445/arms/examples/).
 * Written from each example's prompt, not from its two programs, so a program that disagrees with
 * its task text is caught when make test-exp445 runs both forms through the sealed harness.
 *   examples_expected <entry> < inputs > expected
 * One input line -> one result line (decimal; bool as 0/1) or "TRAP <code>".
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 6

/* reads one line of up to MAXV integers; signed and unsigned views of each */
static int row(int n, int64_t *s, uint64_t *u)
{
    char buf[512], *p = buf, *e;
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    for (int i = 0; i < n; i++) {
        while (*p == ' ') p++;
        if (*p == '-') { s[i] = strtoll(p, &e, 10); u[i] = (uint64_t)s[i]; }
        else { u[i] = strtoull(p, &e, 10); s[i] = (int64_t)u[i]; }
        if (e == p) { fprintf(stderr, "examples_expected: bad input line\n"); exit(2); }
        p = e;
    }
    return 1;
}

static int fits(int64_t v, int bits) { return v >= -(INT64_C(1) << (bits - 1)) && v < (INT64_C(1) << (bits - 1)); }

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: examples_expected <entry> < inputs\n"); return 2; }
    const char *t = argv[1];
    int64_t s[MAXV];
    uint64_t u[MAXV];
    if (!strcmp(t, "clamp_sub")) {
        while (row(2, s, u)) printf("%" PRIu64 "\n", u[0] >= u[1] ? u[0] - u[1] : 0);
    } else if (!strcmp(t, "hash_step")) {
        while (row(2, s, u)) printf("%" PRIu32 "\n", (uint32_t)((uint32_t)u[0] * 31u + (uint32_t)u[1]));
    } else if (!strcmp(t, "sum_squares3")) {
        while (row(3, s, u)) {
            int64_t a = s[0] * s[0], b = s[1] * s[1], c = s[2] * s[2];
            if (!fits(a, 16) || !fits(b, 16) || !fits(c, 16) || !fits(a + b, 16) || !fits(a + b + c, 16)) printf("TRAP 1\n");
            else printf("%" PRId64 "\n", a + b + c);
        }
    } else if (!strcmp(t, "nibble_mirror")) {
        while (row(1, s, u)) { unsigned x = (unsigned)u[0] & 0xffffu; printf("%d\n", (x & 15u) == (x >> 12) && ((x >> 4) & 15u) == ((x >> 8) & 15u)); }
    } else if (!strcmp(t, "halve_or_dec_steps")) {
        while (row(1, s, u)) { uint32_t x = (uint32_t)u[0], k = 0; while (x > 1) { x = x % 2 ? x - 1 : x / 2; k++; } printf("%" PRIu32 "\n", k); }
    } else if (!strcmp(t, "count_digits")) {
        while (row(1, s, u)) { char b[32]; printf("%d\n", snprintf(b, sizeof b, "%" PRIu64, u[0])); }
    } else if (!strcmp(t, "sum_range")) {
        while (row(2, s, u)) {
            int64_t lo = s[0], hi = s[1], sum = 0;
            if (lo <= hi) sum = (lo + hi) * (hi - lo + 1) / 2;
            printf("%" PRId64 "\n", sum);
        }
    } else if (!strcmp(t, "cube_xor_pair")) {
        while (row(1, s, u)) {
            if (u[0] >= 15) { printf("TRAP 3\n"); continue; }
            uint32_t a = (uint32_t)(u[0] * u[0] * u[0]), b = (uint32_t)((u[0] + 1) * (u[0] + 1) * (u[0] + 1));
            printf("%" PRIu32 "\n", (a ^ b) & 0xffffu);
        }
    } else if (!strcmp(t, "count_above")) {
        while (row(5, s, u)) { int k = 0; for (int i = 0; i < 4; i++) k += s[i] > s[4]; printf("%d\n", k); }
    } else if (!strcmp(t, "dot3")) {
        while (row(6, s, u)) printf("%" PRId64 "\n", s[0] * s[3] + s[1] * s[4] + s[2] * s[5]);
    } else if (!strcmp(t, "power_mod")) {
        while (row(3, s, u)) {
            uint64_t b = u[0], e = u[1], m = u[2];
            if (m == 0) { printf("TRAP 2\n"); continue; }
            /* left-to-right square-and-multiply in 128 bits (the programs go right to left in 64 bits) */
            uint64_t r = 1 % m, x = b % m;
            unsigned __int128 acc = r;
            for (int bit = 63; bit >= 0; bit--) {
                acc = acc * acc % m;
                if (e >> bit & 1) acc = acc * x % m;
            }
            printf("%" PRIu64 "\n", (uint64_t)acc);
        }
    } else if (!strcmp(t, "narrow_sum")) {
        while (row(2, s, u)) {
            int64_t v = s[0] + s[1];
            if (!fits(v, 16)) printf("TRAP 1\n");
            else if (!fits(v, 8)) printf("TRAP 5\n");
            else printf("%" PRId64 "\n", v);
        }
    } else if (!strcmp(t, "rotl8")) {
        while (row(2, s, u)) {
            unsigned x = (unsigned)u[0] & 255u, k = (unsigned)(u[1] % 8), r = x;
            for (unsigned i = 0; i < k; i++) r = ((r << 1) | (r >> 7)) & 255u;
            printf("%u\n", r);
        }
    } else if (!strcmp(t, "has_digit")) {
        while (row(2, s, u)) {
            char b[32];
            snprintf(b, sizeof b, "%" PRIu64, u[0]);
            printf("%d\n", u[1] <= 9 && strchr(b, (int)('0' + u[1])) != NULL);
        }
    } else if (!strcmp(t, "tri_kind")) {
        while (row(3, s, u)) {
            uint64_t a = u[0], b = u[1], c = u[2];
            int k;
            if (a + b <= c || a + c <= b || b + c <= a) k = 0;
            else if (a == b && b == c) k = 1;
            else if (a == b || b == c || a == c) k = 2;
            else k = 3;
            printf("%d\n", k);
        }
    } else { fprintf(stderr, "examples_expected: unknown example %s\n", t); return 2; }
    return 0;
}
