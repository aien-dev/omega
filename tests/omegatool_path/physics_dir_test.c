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

/* The M17 check before the pin was added (d70bc4a): header exists and git status is empty. */
static int legacy_m17(const char *dir) {
    char c[4 * PATH_MAX], b[256];
    snprintf(c, sizeof c, "cd '%s' && git status --porcelain 2>/dev/null", dir);
    FILE *p = popen(c, "r"); if (!p) return 0;
    int lines = 0; while (fgets(b, sizeof b, p)) lines++;
    pclose(p); return lines == 0;
}

static void sh(const char *fmt, const char *a) {
    char c[4 * PATH_MAX]; snprintf(c, sizeof c, fmt, a, a, a);
    if (system(c) != 0) { fprintf(stderr, "setup failed: %s\n", c); fails++; }
}

/* Pin + clean checks (omega_physics_dir_verify). */
static void verify_cases(const char *base) {
    char good[2 * PATH_MAX], other[2 * PATH_MAX], nogit[2 * PATH_MAX], lock[2 * PATH_MAX], sha[64] = {0}, err[OMEGA_PHYSICS_ERR_SIZE], c[4 * PATH_MAX];
    snprintf(good, sizeof good, "%s/pgood", base); snprintf(other, sizeof other, "%s/pother", base);
    snprintf(nogit, sizeof nogit, "%s/pnogit", base); snprintf(lock, sizeof lock, "%s/physics.lock", base);
    const char *mk = "mkdir -p '%s/m16' && echo x > '%s/m16/m16_native.h' && cd '%s' && git init -q . && git add . && git -c user.name=t -c user.email=t@t commit -q -m c";
    sh(mk, good);
    sh("mkdir -p '%s/m16' && echo y > '%s/m16/m16_native.h' && cd '%s' && git init -q . && git add . && git -c user.name=t -c user.email=t@t commit -q -m c", other);
    sh("mkdir -p '%s/m16' && echo x > '%s/m16/m16_native.h' && true", nogit);
    snprintf(c, sizeof c, "git -C '%s' rev-parse HEAD", good);
    FILE *p = popen(c, "r"); if (!p || !fgets(sha, sizeof sha, p)) { fails++; return; } pclose(p); sha[40] = 0;
    FILE *l = fopen(lock, "w"); fprintf(l, "%s\n", sha); fclose(l);
    /* red on the old check: an unrelated clean repo and a non-repo both pass it */
    CHECK(legacy_m17(other) == 1);
    CHECK(legacy_m17(nogit) == 1);
    /* new check fails closed */
    CHECK(omega_physics_dir_verify(other, lock, 1, err, sizeof err) == 0 && strstr(err, "expected commit") && strstr(err, other));
    CHECK(omega_physics_dir_verify(nogit, lock, 1, err, sizeof err) == 0 && strstr(err, nogit));
    CHECK(omega_physics_dir_verify(good, lock, 1, err, sizeof err) == 1);
    snprintf(c, sizeof c, "echo z >> '%s/m16/m16_native.h'", good); CHECK(system(c) == 0);
    CHECK(omega_physics_dir_verify(good, lock, 1, err, sizeof err) == 0 && strstr(err, "dirty"));
    CHECK(omega_physics_dir_verify(good, lock, 0, err, sizeof err) == 1); /* pin-only mode (M18/M19) */
    l = fopen(lock, "w"); fputs("deadbeef\n", l); fclose(l);
    CHECK(omega_physics_dir_verify(good, lock, 0, err, sizeof err) == 0 && strstr(err, "malformed"));
    CHECK(omega_physics_dir_verify(good, "/nonexistent/physics.lock", 0, err, sizeof err) == 0);
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

    verify_cases(base);
    char cmd[2 * PATH_MAX]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); (void)!system(cmd);
    puts(fails ? "test-omegatool-path-runtime: FAIL" : "test-omegatool-path-runtime: PASS");
    return fails != 0;
}
