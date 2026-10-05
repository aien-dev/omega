/* Run-time physics resolution (src/omega_physics_dir.h): env wins, ../physics default,
 * and clear failure (no guessing) when unresolvable. Host only, no chip. */
#include "omega_physics_dir.h"
#include <sys/stat.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void mkphys(const char *root) {
    char p[2 * PATH_MAX];
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/m16", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/m16/m16_native.h", root);
    FILE *f = fopen(p, "w"); if (f) { fputs("/* fake */\n", f); fclose(f); }
}

int main(void) {
    char base[] = "/tmp/omega_physdir_XXXXXX", out[PATH_MAX], err[OMEGA_PHYSICS_ERR_SIZE], p[2 * PATH_MAX];
    if (!mkdtemp(base)) return 2;
    char want[PATH_MAX]; if (!realpath(base, want)) return 2;

    /* 1. env set to a valid checkout */
    snprintf(p, sizeof p, "%s/phys", base); mkphys(p);
    setenv("PHYSICS_DIR", p, 1);
    CHECK(omega_physics_dir_resolve(out, sizeof out, err, sizeof err) == 1);
    snprintf(p, sizeof p, "%s/phys", want); CHECK(strcmp(out, p) == 0);

    /* 2. env set to a missing dir: refuse, message names the value and the variable */
    setenv("PHYSICS_DIR", "/nonexistent/physics-xyz", 1);
    CHECK(omega_physics_dir_resolve(out, sizeof out, err, sizeof err) == 0);
    CHECK(strstr(err, "/nonexistent/physics-xyz") && strstr(err, "PHYSICS_DIR"));

    /* 3. env set to a dir that is not physics: refuse, never fall back to ../physics */
    snprintf(p, sizeof p, "%s/notphys", base); mkdir(p, 0755);
    setenv("PHYSICS_DIR", p, 1);
    CHECK(omega_physics_dir_resolve(out, sizeof out, err, sizeof err) == 0);
    CHECK(strstr(err, "m16/m16_native.h"));

    /* 4. env empty or unset: default ../physics relative to cwd; absent -> refuse */
    snprintf(p, sizeof p, "%s/omega", base); mkdir(p, 0755);
    CHECK(chdir(p) == 0);
    setenv("PHYSICS_DIR", "", 1);
    CHECK(omega_physics_dir_resolve(out, sizeof out, err, sizeof err) == 0);
    CHECK(strstr(err, "../physics"));
    unsetenv("PHYSICS_DIR");
    CHECK(omega_physics_dir_resolve(out, sizeof out, err, sizeof err) == 0);
    snprintf(p, sizeof p, "%s/physics", base); mkphys(p);
    CHECK(omega_physics_dir_resolve(out, sizeof out, err, sizeof err) == 1);
    snprintf(p, sizeof p, "%s/physics", want); CHECK(strcmp(out, p) == 0);

    char cmd[2 * PATH_MAX]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); (void)!system(cmd);
    puts(fails ? "test-omegatool-path-runtime: FAIL" : "test-omegatool-path-runtime: PASS");
    return fails != 0;
}
