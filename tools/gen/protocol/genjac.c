/*
 * genjac.c -- word-3-gram Jaccard near-duplicate check for GEN held-out prompts (aien-spark/omega#476,
 * docs/one_layer/GEN_PROTOCOL_V1.md section 2). Deterministic: no clock, no randomness, no environment.
 *
 *   genjac pair <fileA> <fileB>
 *       print "grams_a=<n> grams_b=<m> inter=<i> union=<u> jaccard=<6 decimals>"
 *   genjac scan [--threshold <t>] [--show] <heldout-list> <corpus-list>
 *       every held-out file against every corpus file; a list file names one path per line (no blank
 *       lines, no comments). Prints "heldout=<n> corpus=<m> threshold=<t> max_jaccard=<6 decimals>
 *       above=<k>" where above = the number of held-out files with at least one corpus file whose Jaccard
 *       is strictly greater than t. Nothing about any held-out file is printed unless --show is given
 *       (custodian only): then one "above <heldout path> <corpus path> <jaccard>" line per such pair.
 *       Exit 0 when above = 0, 1 otherwise, 2 on usage or read errors. Default t = 0.5.
 *
 * Words: bytes A-Z are lowercased; a word is a maximal run of [a-z0-9]; every other byte separates (the
 * genret tokenization, tools/gen/retrieval/CONTRACT.md section 4). Grams: the SET of consecutive word
 * triples. Jaccard = |A n B| / |A u B|; 0 when both sets are empty (a file with fewer than 3 words has no
 * gram). The comparison "> t" is exact: t is a decimal with at most 6 fraction digits, read as an integer
 * ratio, and compared by integer cross-multiplication.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char **g; int n; } Grams;

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "genjac: cannot read %s\n", path); exit(2); }
    size_t cap = 4096, n = 0, r;
    char *b = malloc(cap + 1);
    while (b && (r = fread(b + n, 1, cap - n, fp)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; b = realloc(b, cap + 1); }
    }
    fclose(fp);
    if (!b) { perror("genjac"); exit(2); }
    b[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (!b[i]) { fprintf(stderr, "genjac: %s: NUL byte\n", path); exit(2); }
    return b;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static Grams grams(const char *path)
{
    char *s = slurp(path);
    size_t len = strlen(s);
    char **w = malloc((len / 2 + 2) * sizeof *w);
    int nw = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') s[i] = (char)(c + 32), c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            size_t j = i;
            while (j < len) {
                unsigned char d = (unsigned char)s[j];
                if (d >= 'A' && d <= 'Z') s[j] = (char)(d + 32), d = (unsigned char)s[j];
                if (!((d >= 'a' && d <= 'z') || (d >= '0' && d <= '9'))) break;
                j++;
            }
            w[nw++] = strndup(s + i, j - i);
            i = j;
        } else i++;
    }
    Grams G = {NULL, 0};
    if (nw >= 3) {
        G.g = malloc((size_t)(nw - 2) * sizeof *G.g);
        for (int i = 0; i + 2 < nw; i++) {
            size_t l = strlen(w[i]) + strlen(w[i + 1]) + strlen(w[i + 2]) + 3;
            G.g[i] = malloc(l);
            snprintf(G.g[i], l, "%s %s %s", w[i], w[i + 1], w[i + 2]);
        }
        qsort(G.g, (size_t)(nw - 2), sizeof *G.g, cmp);
        int u = 0;
        for (int i = 0; i < nw - 2; i++)
            if (u == 0 || strcmp(G.g[i], G.g[u - 1])) G.g[u++] = G.g[i];
        G.n = u;
    }
    for (int i = 0; i < nw; i++) free(w[i]);
    free(w);
    free(s);
    return G;
}

static void jac(const Grams *a, const Grams *b, int64_t *inter, int64_t *uni)
{
    int i = 0, j = 0;
    int64_t in = 0;
    while (i < a->n && j < b->n) {
        int c = strcmp(a->g[i], b->g[j]);
        if (c == 0) { in++; i++; j++; } else if (c < 0) i++; else j++;
    }
    *inter = in;
    *uni = a->n + b->n - in;
}

/* t = num / den, den = 10^digits */
static int parse_t(const char *s, int64_t *num, int64_t *den)
{
    int64_t n = 0, d = 1;
    int dot = 0, digits = 0, any = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.' && !dot) { dot = 1; continue; }
        if (*p < '0' || *p > '9') return -1;
        any = 1;
        n = n * 10 + (*p - '0');
        if (dot) { d *= 10; if (++digits > 6) return -1; }
        if (n > 10000000) return -1;
    }
    if (!any || n > d) return -1;   /* 0 <= t <= 1 */
    *num = n;
    *den = d;
    return 0;
}

static char **read_list(const char *path, int *n)
{
    char *s = slurp(path);
    size_t len = strlen(s);
    if (len == 0 || s[len - 1] != '\n') { fprintf(stderr, "genjac: %s: empty or no final newline\n", path); exit(2); }
    char **v = malloc((len + 1) * sizeof *v);
    int k = 0;
    for (char *p = s; *p;) {
        char *e = strchr(p, '\n');
        *e = 0;
        if (!*p) { fprintf(stderr, "genjac: %s: blank line\n", path); exit(2); }
        v[k++] = p;
        p = e + 1;
    }
    *n = k;
    return v;
}

static void print6(int64_t in, int64_t un)
{
    /* exact rounding of in/un to 6 decimals, half up */
    if (un == 0) { printf("0.000000"); return; }
    int64_t q = (in * 2000000 + un) / (2 * un);
    printf("%lld.%06lld", (long long)(q / 1000000), (long long)(q % 1000000));
}

static int usage(void)
{
    fprintf(stderr, "usage: genjac pair <fileA> <fileB> | scan [--threshold <t>] [--show] <heldout-list> <corpus-list>\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "pair")) {
        Grams a = grams(argv[2]), b = grams(argv[3]);
        int64_t in, un;
        jac(&a, &b, &in, &un);
        printf("grams_a=%d grams_b=%d inter=%lld union=%lld jaccard=", a.n, b.n, (long long)in, (long long)un);
        print6(in, un);
        putchar('\n');
        return 0;
    }
    if (argc < 4 || strcmp(argv[1], "scan")) return usage();
    const char *ts = "0.5";
    int show = 0, i = 2;
    for (; i < argc - 2; i++) {
        if (!strcmp(argv[i], "--threshold") && i + 1 < argc - 2) ts = argv[++i];
        else if (!strcmp(argv[i], "--show")) show = 1;
        else return usage();
    }
    if (i != argc - 2) return usage();
    int64_t tn, td;
    if (parse_t(ts, &tn, &td)) { fprintf(stderr, "genjac: threshold must be a decimal in [0,1] with at most 6 fraction digits\n"); return 2; }
    int nh, nc;
    char **hl = read_list(argv[argc - 2], &nh), **cl = read_list(argv[argc - 1], &nc);
    Grams *cg = malloc((size_t)nc * sizeof *cg);
    for (int c = 0; c < nc; c++) cg[c] = grams(cl[c]);
    int64_t bi = 0, bu = 1;   /* max Jaccard as a ratio, starts at 0/1 */
    int above = 0;
    for (int h = 0; h < nh; h++) {
        Grams g = grams(hl[h]);
        int hit = 0;
        for (int c = 0; c < nc; c++) {
            int64_t in, un;
            jac(&g, &cg[c], &in, &un);
            if (un == 0) continue;
            if (in * bu > bi * un) { bi = in; bu = un; }
            if (in * td > tn * un) {   /* in/un > tn/td, exactly */
                hit = 1;
                if (show) { printf("above %s %s ", hl[h], cl[c]); print6(in, un); putchar('\n'); }
            }
        }
        above += hit;
    }
    printf("heldout=%d corpus=%d threshold=%s max_jaccard=", nh, nc, ts);
    print6(bi, bu);
    printf(" above=%d\n", above);
    return above ? 1 : 0;
}
