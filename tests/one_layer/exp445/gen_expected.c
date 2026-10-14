/*
 * gen_expected.c -- independent C computation of hidden/expected for the PUBLIC exp445 dev tasks
 * (omega#445; tests/one_layer/exp445/dev/). Written from each task's prompt, not from its
 * reference.osc, so a reference that disagrees with the prompt is caught by `ola.sh tasks-check`.
 *   gen_expected <task_id> < hidden/inputs > hidden/expected
 * Trap lines name the code the prompt names (1 overflow, 2 division by zero, 5 conversion).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int line(int64_t *a, int64_t *b, int two)
{
    char buf[256];
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return two ? sscanf(buf, "%" SCNd64 " %" SCNd64, a, b) == 2 : sscanf(buf, "%" SCNd64, a) == 1;
}

static int lineu(uint64_t *a, uint64_t *b, int two)
{
    char buf[256];
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return two ? sscanf(buf, "%" SCNu64 " %" SCNu64, a, b) == 2 : sscanf(buf, "%" SCNu64, a) == 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: gen_expected <task_id> < inputs\n"); return 2; }
    const char *t = argv[1];
    int64_t a, b;
    uint64_t x, y;
    if (!strcmp(t, "dev01")) while (lineu(&x, &y, 1)) printf("%" PRIu64 "\n", x + y > 255 ? 255 : x + y);
    else if (!strcmp(t, "dev02")) while (line(&a, &b, 1)) printf("%" PRId64 "\n", a > b ? a - b : b - a);
    else if (!strcmp(t, "dev03")) while (lineu(&x, &y, 0)) printf("%d\n", __builtin_popcountll(x));
    else if (!strcmp(t, "dev04")) while (lineu(&x, &y, 1)) { while (y) { uint64_t r = x % y; x = y; y = r; } printf("%" PRIu64 "\n", x); }
    else if (!strcmp(t, "dev05")) while (line(&a, &b, 1)) {
        int64_t p = a * b;
        if (p < -32768 || p > 32767) printf("TRAP 1\n"); else printf("%" PRId64 "\n", p);
    }
    else if (!strcmp(t, "dev06")) while (lineu(&x, &y, 1)) { if (!y) printf("TRAP 2\n"); else printf("%" PRIu64 "\n", x / y); }
    else if (!strcmp(t, "dev07")) while (lineu(&x, &y, 0)) { uint64_t r = 0; while ((r + 1) * (r + 1) <= x) r++; printf("%" PRIu64 "\n", r); }
    else if (!strcmp(t, "dev08")) while (lineu(&x, &y, 0)) {
        unsigned s = 0;
        while (x != 1) { x = (x % 2) ? 3 * x + 1 : x / 2; s++; if (s >= 1000) { fprintf(stderr, "dev08: input needs >= 1000 steps\n"); return 1; } }
        printf("%u\n", s);
    }
    else if (!strcmp(t, "dev09")) while (lineu(&x, &y, 0)) { unsigned r = 0; for (int i = 0; i < 8; i++) if (x >> i & 1) r |= 1u << (7 - i); printf("%u\n", r); }
    else if (!strcmp(t, "dev10")) while (line(&a, &b, 0)) { if (a < -128 || a > 127) printf("TRAP 5\n"); else printf("%" PRId64 "\n", a); }
    else { fprintf(stderr, "gen_expected: unknown task %s\n", t); return 2; }
    return 0;
}
