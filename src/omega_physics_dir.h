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

#endif
