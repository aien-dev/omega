/*
 * omega_evidence.h -- immutable run-evidence plumbing shared by every gate suite.
 *
 * Problem this closes: gate suites used to fopen() straight into the
 * committed evidence/ tree, silently rewriting evidence that had already
 * been reviewed and merged, and receipts asserted literal pass counts
 * instead of reporting what actually ran. This header gives every gate
 * suite:
 *
 *   (a) omega_evidence_path()   -- a namespaced, run-scoped output path
 *       that never collides with (or overwrites) a legacy evidence/ file.
 *   (b) accessor functions for every suite's gate counters, plus callable
 *       entry points for the M4..M15 suites (previously inlined in
 *       tools/omegatool.c) so a regression gate can actually execute them
 *       and sum real counts instead of asserting a literal.
 *   (c) git commit identity (run_commit / tree_dirty) so a receipt cannot
 *       misrepresent the commit it ran on as "the qualified candidate".
 *   (d) observed hardware identity (compute_class / sm_version / uuid)
 *       read from a live Nvrm handle instead of a hard-coded "sm_121".
 */
#ifndef OMEGA_EVIDENCE_H
#define OMEGA_EVIDENCE_H

#include <stddef.h>
#include <stdbool.h>
#include "nvrm.h"

/* ---- (a) run-scoped, non-clobbering evidence paths -------------------- */

/*
 * Resolve `relpath` to a run-scoped output path and create any parent
 * directories needed to write it.
 *
 *  - Default: "build/qual-runs/<run_id>/<relpath>".
 *  - Even when OMEGA_QUAL_RECORD=1, intermediate gate output remains under
 *    build/qual-runs. Only tools/m19r_qualify.py writes permanent evidence,
 *    after all gates finish, using an exclusive content-addressed file.
 *  - This function never returns a legacy "evidence/<relpath>" path
 *    (i.e. never a path that collides with a previously committed,
 *    reviewed evidence file).
 *
 * `run_id` is computed once per process as UTC yyyymmddThhmmssZ + "-" +
 * the short HEAD sha, and is stable for the lifetime of the process.
 *
 * Returns 0 on success (out/n filled with a NUL-terminated path), -1 on
 * failure or refusal.
 */
int omega_evidence_path(const char *relpath, char *out, size_t n);

/* The run_id used by omega_evidence_path(), computed lazily on first use. */
const char *omega_evidence_run_id(void);

/* ---- (c) commit identity ------------------------------------------------ */

/* HEAD sha of the working tree the current process is running from. */
bool omega_evidence_run_commit(char out[41]);

/* True if the full relevant git tree (including untracked files) is dirty. */
bool omega_evidence_tree_dirty(void);

/* Reads physics.lock at the repo root; trims whitespace/newline. Returns
 * false if the file cannot be read. */
bool omega_evidence_physics_commit(char *out, size_t n);

/* ---- (d) observed hardware identity ------------------------------------- */

typedef struct {
    unsigned compute_class;   /* raw Nvrm.compute_class */
    unsigned rm_sm_version;   /* raw Nvrm.sm_version */
    char gpu_uuid_hex[33];    /* Nvrm.gpu_uuid[16], hex-encoded */
    const char *alias;        /* human alias only, e.g. "sm_121" -- never the bound identity */
} OmegaEvidenceHardware;

void omega_evidence_hardware_from_nvrm(const Nvrm *rm, OmegaEvidenceHardware *out);

/* ---- (b) per-suite gate counters & callable suite entry points --------- */

/* M4..M15 used to be inlined directly in tools/omegatool.c's main(). They
 * are now callable functions so a regression gate can invoke them and read
 * back how many gates each suite actually ran/passed. All twelve share one
 * counter pair internally (matching the previous inlined behavior); read
 * the snapshot immediately after each call, before invoking the next. */
int omega_run_m4_gates(void);
int omega_run_m5_gates(void);
int omega_run_m6_gates(void);
int omega_run_m7_gates(void);
int omega_run_m8_gates(void);
int omega_run_m9_gates(void);
int omega_run_m10_gates(void);
int omega_run_m11_gates(void);
int omega_run_m12_gates(void);
int omega_run_m13_gates(void);
int omega_run_m14_gates(void);
int omega_run_m15_gates(void);
void omega_get_gate_snapshot(int *count, int *passed);

/* M17/M18/M19 already track their own static counters; these expose them. */
void omega_get_m17_gate_snapshot(int *count, int *passed);
void omega_get_m18_gate_snapshot(int *count, int *passed);
void omega_get_m19_gate_snapshot(int *count, int *passed);

#endif /* OMEGA_EVIDENCE_H */
