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

/* `dir` must be a real directory (opened O_DIRECTORY|O_NOFOLLOW, then
 * fstat on that descriptor: no symlink, no path re-resolution), owned by the
 * effective uid, with no group or other permission bits. With `create`, a
 * missing directory is made 0700 first. 0 ok; -1 refused, with the reason in
 * `why`. rx_operator_dir_open also hands back the held descriptor. */
int rx_operator_dir_check(const char *dir, int create, char *why, size_t n);
int rx_operator_dir_open(const char *dir, int create, int *fd_out, char *why, size_t n);

/* Exclusive lock on `dir` (validated as above): flock(LOCK_EX|LOCK_NB) on
 * RX_OPERATOR_LOCK, opened with openat(O_CLOEXEC|O_NOFOLLOW) in the held
 * directory. Held until the returned descriptor is closed (or the process
 * ends). -1 refused (held by another program: its pid is in `why`). */
#define RX_OPERATOR_LOCK   "operator.lock"
int rx_operator_lock_dir(const char *dir, int create, int *lock_fd, char *why, size_t n);

/* Lock the control directory (refused if another program holds it), write
 * the credential file (0600, temporary file, fsync, rename, all relative to
 * the held directory descriptor), wipe `cred`, bind the socket inside the
 * held directory and start the listener. The request gate starts
 * HELD by the caller: no request is served until rx_operator_release. */
int rx_operator_open(RxOperator **out, const RxOperatorConfig *cfg, uint32_t subject,
                     RxCallerCred *cred, RxCapRef cap);
/* The request gate: while the program holds it (world setup), requests wait. */
void rx_operator_hold(RxOperator *op);
void rx_operator_release(RxOperator *op);
/* Stop the listener and remove the socket and the credential file, each only
 * if it is still the file this instance created (same device and inode),
 * then release the control directory lock. The gate must not be held by the
 * caller. */
void rx_operator_close(RxOperator *op);

#endif
