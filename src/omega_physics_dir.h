#ifndef OMEGA_PHYSICS_DIR_H
#define OMEGA_PHYSICS_DIR_H

/* Run-time resolution of the physics checkout used by the omegatool gate runners
 * (M17 physics authority, M18 gate 16, M19 clean-clone qualification).
 *
 * The physics location is NOT compiled into the binary (it used to be
 * -DOMEGA_PHYSICS_DIR, which made omegatool's bytes depend on where physics was
 * checked out). Rule, in order:
 *   1. the PHYSICS_DIR environment variable, if set and non-empty (make exports
 *      command-line variables, so `make PHYSICS_DIR=/x test-m17` passes it down);
 *   2. otherwise the project default "../physics", relative to the current directory
 *      (the same default as Makefile `PHYSICS_DIR ?= ../physics`, run from the omega root).
 * The chosen directory must contain m16/m16_native.h; its realpath is returned.
 * On failure nothing is guessed: the function returns 0 and writes a message that names
 * the value tried, where it came from, and how to fix it. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define OMEGA_PHYSICS_ERR_SIZE (2 * PATH_MAX + 256)

static inline int omega_physics_dir_resolve(char *out, size_t out_size, char *err, size_t err_size) {
    const char *env = getenv("PHYSICS_DIR");
    const char *src = (env && env[0] != '\0') ? "PHYSICS_DIR environment variable" : "default ../physics";
    const char *pd = (env && env[0] != '\0') ? env : "../physics";
    char real[PATH_MAX];
    char probe[PATH_MAX + 32];
    if (!realpath(pd, real)) {
        snprintf(err, err_size,
                 "physics checkout not found: %s (%s) does not resolve; set PHYSICS_DIR to the physics checkout",
                 pd, src);
        return 0;
    }
    if (strchr(real, '\'') != NULL || strlen(real) + 1 > out_size) {
        snprintf(err, err_size, "physics checkout path unusable (quote or too long): %s (%s)", real, src);
        return 0;
    }
    snprintf(probe, sizeof(probe), "%s/m16/m16_native.h", real);
    if (access(probe, R_OK) != 0) {
        snprintf(err, err_size,
                 "not a physics checkout: %s (%s) has no m16/m16_native.h; set PHYSICS_DIR to the physics checkout",
                 real, src);
        return 0;
    }
    snprintf(out, out_size, "%s", real);
    return 1;
}


/* Authority check for a resolved physics dir (the run-time dir may differ from the one make checked at
 * build time, so the gate must re-check it). `lock_path` holds the pinned commit: first line, trimmed,
 * full 40 hex digits (malformed is refused). `git -C <pd> rev-parse HEAD` must equal it and, when
 * `require_clean`, `git -C <pd> status --porcelain` must succeed and print nothing. Any git failure
 * (not a repository, git missing) is a refusal; nothing is hidden. */
static inline int omega_physics_dir_verify(const char *pd, const char *lock_path, int require_clean,
                                           char *err, size_t err_size) {
    char want[128] = {0}, got[256] = {0}, cmd[PATH_MAX + 128], line[256];
    FILE *f = fopen(lock_path, "r");
    if (!f) {
        snprintf(err, err_size, "cannot read %s (cwd-relative; run from the omega root): physics commit pin unknown", lock_path);
        return 0;
    }
    if (!fgets(want, sizeof(want), f)) want[0] = '\0';
    fclose(f);
    size_t n = strlen(want);
    while (n > 0 && (want[n - 1] == '\n' || want[n - 1] == '\r' || want[n - 1] == ' ' || want[n - 1] == '\t')) want[--n] = '\0';
    int ok = (n == 40);
    for (size_t i = 0; ok && i < 40; i++) {
        char c = want[i];
        ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    if (!ok) {
        snprintf(err, err_size, "%s is malformed: first line must be one full 40-hex commit", lock_path);
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse HEAD 2>&1", pd);
    FILE *p = popen(cmd, "r");
    int rc = -1;
    if (p) {
        if (!fgets(got, sizeof(got), p)) got[0] = '\0';
        rc = pclose(p);
    }
    n = strlen(got);
    while (n > 0 && (got[n - 1] == '\n' || got[n - 1] == '\r')) got[--n] = '\0';
    if (rc != 0 || strcasecmp(got, want) != 0) {
        snprintf(err, err_size, "physics dir %s is not the pinned checkout: expected commit %s, observed %s",
                 pd, want, rc != 0 ? "(not a git checkout or git failed)" : got);
        return 0;
    }
    if (require_clean) {
        snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>&1", pd);
        p = popen(cmd, "r");
        int dirty = 1;
        rc = -1;
        if (p) {
            dirty = (fgets(line, sizeof(line), p) != NULL);
            rc = pclose(p);
        }
        if (rc != 0 || dirty) {
            snprintf(err, err_size, "physics dir %s at pinned commit %s is %s", pd, want,
                     rc != 0 ? "unreadable by git status" : "dirty (git status --porcelain not empty)");
            return 0;
        }
    }
    return 1;
}

/* resolve + verify against ./physics.lock (cwd-relative, like src/omega_evidence.c). */
static inline int omega_physics_dir_resolve_pinned(char *out, size_t out_size, int require_clean,
                                                   char *err, size_t err_size) {
    return omega_physics_dir_resolve(out, out_size, err, err_size) &&
           omega_physics_dir_verify(out, "physics.lock", require_clean, err, err_size);
}

#endif
