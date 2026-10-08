/* osh_priv.h -- internal to the Linux host adapter; not part of the adapter interface. */
#ifndef OSH_PRIV_H
#define OSH_PRIV_H
#include "osh_host.h"

/* Write "osh: <fmt>\n" to fd (best effort, full-write loop). */
void osh_diag(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Write all n bytes; 0 ok, -1 with errno set. */
int osh_write_all(int fd, const char *buf, size_t n);
/* Run builtin c->builtin_id with io[0..2] as its stdin/out/err. in_parent: effects on the session are real.
 * Returns the builtin's exit status. May set s->exit_requested (parent only). */
int osh_builtin_run(OshSession *s, const OshCmd *c, const int io[3], int in_parent);
#endif
