/*
 * osh_caps.h -- capability enforcement for the Linux host adapter (aien-architecture#158, ABI section 9).
 *
 * Authority: omega's own host capability root (src/runtime/rx_caproot.{h,c}), no new authority. The root runs in a
 * separate process and is the only writer of the capability table; the osh process holds a read-only view. This file
 * is HOST code: it parses a launch policy, asks the root to mint, and answers the effect hook. The shell core (.osc)
 * never sees it, and no shell syntax, variable, environment entry, command name or argv[0] reaches it.
 *
 * Policy file (parsed here, by the host only):
 *     principal <u32>                      once, before any grant
 *     allow spawn|read|write|chdir <absolute path prefix>
 * '#' starts a comment. One minted capability per grant: subject = principal, resource = the grant's number (1..N,
 * assigned here, never a hash), rights EFFECT (spawn) / READ (read, chdir) / WRITE (write). The session itself holds a
 * principal capability (resource 0, READ); revoking it revokes every effect at once. No policy: every effect is denied.
 *
 * WHAT THIS DOES NOT DO. It gates what osh itself does: the spawn of a program it resolved, the opens it performs
 * for redirections, and cd. A child that was allowed to start runs with the full authority of the Linux user (any file
 * that user may open, any program, the network). A prefix allowlist is not a sandbox and no containment is claimed:
 * nothing here uses seccomp, Landlock or namespaces. The check and the syscall are two steps (a path swapped in between
 * is not prevented). The root is the Linux development root (rx_caproot.h); on AIENOS the kernel authority is the root.
 */
#ifndef OSH_CAPS_H
#define OSH_CAPS_H
#include <stddef.h>

#include "osh_host.h"
#include "rx_caproot.h"

#define OSH_CAPS_MAX_GRANTS 200

typedef struct {
    int op;              /* OSH_OP_* */
    char *prefix;        /* canonical absolute prefix */
    uint64_t resource;   /* grant number */
    RxCapRef ref;
} OshGrant;

typedef struct OshCaps {
    RxCapRoot root;      /* read-only view: the only thing the effect check touches */
    RxCapAdmin admin;    /* host/embedder only; wiped by osh_caps_seal() */
    int started, sealed;
    uint32_t principal;
    RxCapRef principal_ref;
    OshGrant grants[OSH_CAPS_MAX_GRANTS];
    int ngrants;
} OshCaps;

/* Start the root. 0 ok, else a negative RX_CAP_ERR_*. */
int osh_caps_start(OshCaps *c);
/* Parse the policy text and have the root mint one capability per grant. 0 ok; else -1 and a message in err. */
int osh_caps_load(OshCaps *c, const char *text, char *err, size_t errsz);
/* Bind the session: effect hook + principal binding (domain 2, full 64-bit generation). */
void osh_caps_attach(OshCaps *c, OshSession *s);
/* The hook installed by osh_caps_attach (exposed for tests). */
int osh_caps_effect(void *ctx, const OshBinding *b, int op, const char *path);
/* Drop the admin handle: wipe the token, close the control socket. After this nothing in this process can mint or
 * revoke; the root exits and the table is frozen. The osh program seals before running the first command. */
void osh_caps_seal(OshCaps *c);
void osh_caps_stop(OshCaps *c);
#endif
