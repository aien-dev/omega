/* BRW-ACT re-analysis of a committed table.tsv (harness side). Reads only the per-world table; runs no world.
 * Usage: brw_act_reanalyse dev0|dev1 <table.tsv>
 * Prints, over the discoverable classes of that profile (DEV0: diffusion, drift, ou; DEV1: drift, ou):
 *  - held-out coverage 50% / 90% with the per-world 95% interval (brw_act_report.h);
 *  - computation charged: extra evaluations (likelihood + quadrature + cdf, counted alike) of active over each
 *    baseline, the paired time saving with a normal 95% interval, and the break-even exchange rate: active stays
 *    ahead only while one time unit of measurement is worth more than that many evaluations.
 * Descriptive, written after the results were read: not a pass rule of any profile. */
#include "brw_act_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NS = 4, NWMAX = 600 };
static const char *sname[NS] = {"active", "random", "fixed8", "cycle"};

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "dev0") && strcmp(argv[1], "dev1"))) {
        fprintf(stderr, "usage: brw_act_reanalyse dev0|dev1 <table.tsv>\n");
        return 64;
    }
    const int dev1 = !strcmp(argv[1], "dev1");
    FILE *f = fopen(argv[2], "r");
    if (!f) { perror(argv[2]); return 1; }
    static double cost[NS][NWMAX], ev[NS][NWMAX], k50[NS][NWMAX], k90[NS][NWMAX];
    int n[NS] = {0}, rows = 0;
    char line[4096];
    if (!fgets(line, sizeof line, f) || strncmp(line, "world\tclass\t", 12)) { fprintf(stderr, "not a BRW-ACT table\n"); return 1; }
    while (fgets(line, sizeof line, f)) {
        char *col[24];
        int nc = 0;
        for (char *p = strtok(line, "\t\n"); p && nc < 24; p = strtok(NULL, "\t\n")) col[nc++] = p;
        if (nc != 24) { fprintf(stderr, "row %d: %d columns, want 24\n", rows + 2, nc); return 1; }
        rows++;
        if (!strcmp(col[1], "noise") || !strcmp(col[1], "mismatch") || (dev1 && !strcmp(col[1], "diffusion"))) continue;
        int s = -1;
        for (int i = 0; i < NS; i++) if (!strcmp(col[4], sname[i])) s = i;
        if (s < 0 || n[s] >= NWMAX) { fprintf(stderr, "row %d: bad strategy or too many worlds\n", rows + 1); return 1; }
        cost[s][n[s]] = atof(col[8]);
        k50[s][n[s]] = atof(col[18]);
        k90[s][n[s]] = atof(col[19]);
        ev[s][n[s]] = atof(col[21]) + atof(col[22]) + atof(col[23]);
        n[s]++;
    }
    fclose(f);
    for (int s = 1; s < NS; s++) if (n[s] != n[0]) { fprintf(stderr, "unpaired: %d vs %d worlds\n", n[s], n[0]); return 1; }
    const int nd = n[0];
    printf("BRW-ACT re-analysis (%s), %d table rows, %d discoverable worlds per strategy\n", argv[1], rows, nd);
    printf("\nHeld-out coverage, per-world 95%% interval (replaces the pooled Wilson interval):\n");
    for (int s = 0; s < NS; s++) {
        double m50, l50, h50, m90, l90, h90;
        brwr_coverage(k50[s], nd, 60, &m50, &l50, &h50);
        brwr_coverage(k90[s], nd, 60, &m90, &l90, &h90);
        printf("  %-7s cov50 %.4f [%.4f,%.4f]  cov90 %.4f [%.4f,%.4f]\n", sname[s], m50, l50, h50, m90, l90, h90);
    }
    printf("\nComputation charged (active minus baseline, per discoverable world):\n");
    printf("  %-7s %14s %22s %26s\n", "vs", "extra evals", "time saved [95%]", "break-even evals/time");
    for (int b = 1; b < NS; b++) {
        double ds = 0, dss = 0, de = 0;
        for (int i = 0; i < nd; i++) ds += cost[b][i] - cost[0][i], de += ev[0][i] - ev[b][i];
        ds /= nd; de /= nd;
        for (int i = 0; i < nd; i++) { double d = cost[b][i] - cost[0][i] - ds; dss += d * d; }
        double h = 1.959964 * sqrt(dss / (nd - 1) / nd);
        printf("  %-7s %14.4g %8.1f [%6.1f,%6.1f] ", sname[b], de, ds, ds - h, ds + h);
        if (ds - h > 0 && de > 0) printf("%11.3g (%.3g at the low end of the saving)\n", de / ds, de / (ds - h));
        else printf("%26s\n", "no saving established");
    }
    return 0;
}
