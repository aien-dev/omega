/*
 * DIRAC-0 independent oracle: exact Clifford relation check.
 * STATUS: NOT_RUN. Not compiled, not executed. No libm, no floating point.
 * Shares no code with Omega. Entries are Gaussian INTEGERS (re,im as long long),
 * which covers the small hand corpus; rational entries (such as 1/2) are for the
 * basis-change check and are out of scope for this first file.
 *
 * Input on stdin (whitespace separated integers):
 *   d n                      generators and matrix size (n <= 4, d <= 4)
 *   m_0 .. m_{d-1}           metric diagonal entries (integers)
 *   then for each generator, n*n entries as pairs "re im", row major.
 * Output: one line "FAIL i j" per failing pair, then "PASS" or "FAIL_COUNT k".
 * Exit status 0 on PASS, 1 on FAIL, 2 on bad input.
 */
#include <stdio.h>

#define MAXD 4
#define MAXN 4

typedef struct { long long re, im; } gi;
typedef struct { gi a[MAXN][MAXN]; } mat;

static gi gmul(gi x, gi y) {
    gi r;
    r.re = x.re * y.re - x.im * y.im;
    r.im = x.re * y.im + x.im * y.re;
    return r;
}

static void matmul(const mat *x, const mat *y, int n, mat *out) {
    int i, j, k;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            gi s = {0, 0};
            for (k = 0; k < n; k++) {
                gi t = gmul(x->a[i][k], y->a[k][j]);
                s.re += t.re;
                s.im += t.im;
            }
            out->a[i][j] = s;
        }
}

int main(void) {
    int d, n, i, j, r, c;
    long long metric[MAXD];
    mat g[MAXD];
    int fails = 0;

    if (scanf("%d %d", &d, &n) != 2) return 2;
    if (d < 1 || d > MAXD || n < 1 || n > MAXN) return 2;
    for (i = 0; i < d; i++)
        if (scanf("%lld", &metric[i]) != 1) return 2;
    for (i = 0; i < d; i++)
        for (r = 0; r < n; r++)
            for (c = 0; c < n; c++)
                if (scanf("%lld %lld", &g[i].a[r][c].re, &g[i].a[r][c].im) != 2) return 2;

    for (i = 0; i < d; i++)
        for (j = i; j < d; j++) {
            mat p, q;
            int bad = 0;
            matmul(&g[i], &g[j], n, &p);
            matmul(&g[j], &g[i], n, &q);
            for (r = 0; r < n; r++)
                for (c = 0; c < n; c++) {
                    long long want = (r == c && i == j) ? 2 * metric[i] : 0;
                    /* off-diagonal generators: target is 2*metric(i,j) I, metric is diagonal so it is 0 */
                    if (p.a[r][c].re + q.a[r][c].re != want) bad = 1;
                    if (p.a[r][c].im + q.a[r][c].im != 0) bad = 1;
                }
            if (bad) {
                printf("FAIL %d %d\n", i, j);
                fails++;
            }
        }
    if (fails == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL_COUNT %d\n", fails);
    return 1;
}
