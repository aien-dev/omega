#ifndef RX_OPERATOR_H
#define RX_OPERATOR_H
/* R16 G6: the operator's outside entry point to a running production world
 * (docs/r16-operator-control.md). An owner-only Unix-domain socket in a
 * validated 0700 directory, a SO_PEERCRED check, and one line per request
 * carrying the operator's runtime-issued caller credential and its
 * RX_WORLD_RES_CONTROL capability. Every decision is the world's
 * (rx_world_emergency_stop / _resume / rx_world_operator_authorize); this
 * module only carries requests and replies. */
#include "rx_world.h"
#include "aienos_cap.h"

#include <stddef.h>
#include <stdint.h>

#define RX_OPERATOR_SUBJ   70u    /* runs no reaction; holds only the control capability */
#define RX_OPERATOR_CRED   "operator.cred"
#define RX_OPERATOR_SOCK   "operator.sock"
#define RX_OPERATOR_LINE   512u

typedef struct RxOperator RxOperator;

typedef struct {
    RxWorld *world;
    AienosCapAdmin *admin;       /* the authority: its office revokes the capability */
    const char *control_dir;     /* validated with rx_operator_dir_check */
    const char *world_key;       /* the mode this world runs, for the credential file */
    /* Program counters appended to a status reply (key=value ...). */
    void (*describe)(void *ctx, char *buf, size_t n);
    /* Called after the world accepted a stop (1) or a resume (0). */
    void (*on_halt)(void *ctx, int halted);
    /* Called after an authorized shutdown of a stopped world. */
    void (*on_shutdown)(void *ctx);
    void *ctx;
} RxOperatorConfig;

/* `dir` must be a real directory (lstat; no symlink), owned by the effective
 * uid, with no group or other permission bits. With `create`, a missing
 * directory is made 0700 first. 0 ok; -1 refused, with the reason in `why`. */
int rx_operator_dir_check(const char *dir, int create, char *why, size_t n);

/* Write the credential file (0600, temporary file, fsync, rename), wipe
 * `cred`, bind the socket and start the listener. The request gate starts
 * HELD by the caller: no request is served until rx_operator_release. */
int rx_operator_open(RxOperator **out, const RxOperatorConfig *cfg, uint32_t subject,
                     RxCallerCred *cred, RxCapRef cap);
/* The request gate: while the program holds it (world setup), requests wait. */
void rx_operator_hold(RxOperator *op);
void rx_operator_release(RxOperator *op);
/* Stop the listener, remove the socket and the credential file. The gate
 * must not be held by the caller. */
void rx_operator_close(RxOperator *op);

#endif
