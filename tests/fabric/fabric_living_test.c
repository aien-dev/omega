/*
 * fabric_living_test.c -- Fabric F5-0 used by the COMPOSITION-2 causal path
 * (Lane 13). Runs the scenario of fab_living_phase.h on a composition that
 * owns its World and authority, twice in fresh directories, and requires
 * equal Cortex record and Fabric state digests (deterministic).
 * The same scenario runs inside the R13 living World (rx_r13_living.c).
 * One worker, as the COMPOSITION-2 gate: with more, the candidates finish in
 * varying order and the record order (not its content) varies.
 * Prints FABRIC_LIVING_PASS on success.
 */
#include "fab_living_phase.h"

#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static Fx g_fx;                 /* only for its authority start/stop */

static int rm_one(const char *p, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(p);
}

static int own_attach(void *ctx, RxCompose *c, const char *dir, const AienMachineId *self,
                      const SrRouter *router) {
    Fx *f = ctx;
    f->self = *self;
    if (aienos_cap_start(&f->admin, &f->view) != 0) return -100;
    f->owns_authority = 1;
    int rc = rx_compose_open(c, dir, self, FX_SESSION, router, fx_contract, f->admin, f->view, 1);
    if (rc != RX_OK) {
        aienos_cap_stop(f->admin, f->view);
        f->admin = NULL;
    }
    return rc;
}

static void own_close(void *ctx, RxCompose *c) { fx_close(ctx, c); }

int main(void) {
    char base[64] = "/tmp/fabric_living_test.XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }
    AienMachineId self = fx_mid(1);
    FlReceipt g[2];
    unsigned checks = 0, failures = 0;
    for (int run = 0; run < 2; run++) {
        char dir[128];
        snprintf(dir, sizeof dir, "%s/run%d", base, run);
        memset(&g_fx, 0, sizeof g_fx);
        int rc = fl_run(&self, dir, own_attach, own_close, &g_fx, &g[run]);
        checks += g[run].checks;
        failures += g[run].failures;
        printf("run %d: %s (%u checks, %u failed; remote wins %u, dispatched %llu)\n", run,
               rc == 0 ? "PASS" : "FAIL", g[run].checks, g[run].failures, g[run].remote_wins,
               (unsigned long long)g[run].dispatched);
    }
    int same = memcmp(g[0].record_digest, g[1].record_digest, 32) == 0 &&
               memcmp(g[0].fabric_digest, g[1].fabric_digest, 32) == 0 &&
               memcmp(g[0].result, g[1].result, sizeof g[0].result) == 0 &&
               memcmp(g[0].winner, g[1].winner, sizeof g[0].winner) == 0;
    checks++;
    if (!same) {
        failures++;
        fprintf(stderr, "FABRIC-LIVING FAIL: the two runs differ\n");
    }
    char hx[65];
    for (int run = 0; run < 2; run++) {
        fx_hex(g[run].record_digest, 32, hx);
        printf("run %d record digest %s\n", run, hx);
        fx_hex(g[run].fabric_digest, 32, hx);
        printf("run %d fabric digest %s\n", run, hx);
    }
    nftw(base, rm_one, 16, FTW_DEPTH | FTW_PHYS);
    printf("checks %u, failures %u\n", checks, failures);
    if (failures) { printf("FABRIC_LIVING_FAIL\n"); return 1; }
    printf("FABRIC_LIVING_PASS\n");
    return 0;
}
