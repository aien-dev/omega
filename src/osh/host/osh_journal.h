/*
 * osh_journal.h -- durable effect journal of the osh Linux host adapter (OSH_PLATFORM_ABI.md section 10).
 *
 * An EFFECT is one external TASK_SPAWN or one write-class open (redirection kinds output/append). Before each effect the
 * adapter appends an INTENT record and fsyncs the journal; after the pipeline is reaped it appends one OUTCOME record per
 * effect and fsyncs again. After a crash, an intent with no outcome is OUTCOME_UNKNOWN and is NEVER replayed.
 *
 * This is the MANDATORY effect record (section 10). It is a separate file from any optional history or recording
 * (section 13): it is written whatever the recording mode is, and it holds no argument values.
 *
 * File format (text, one record per line, '\n' terminated, append-only, mode 0600):
 *   OSHJ1 <rec> I <digest64> <idx>/<n> b=<builtin_id> argc=<k> wr=<w> a0=<argv0, %XX-escaped, last 64 bytes, "~" prefix when cut> lens=<l0,l1,..|->
 *   OSHJ1 <rec> O <ref> <digest64> <idx>/<n> <OUTCOME> st=<status> err=<platform error> [recovered=1]
 * <rec> is a journal-wide counter (1, 2, ...), never a pid or any transient id. <ref> is the <rec> of the intent this
 * outcome closes, or 0 for an outcome with no intent (the effect was refused or never reached: nothing was launched).
 * <digest64> is the lowercase hex request digest, HMAC-SHA256 under a random per-journal key kept in <journal>.key (osh_req_digest). OUTCOME is one of NOT_STARTED, COMPLETED,
 * FAILED_NO_EFFECT, CANCELLED, OUTCOME_UNKNOWN.
 *
 * Redaction rule: argument values, assignment values, redirection targets and environment values are never written.
 * Only argv[0], the argument count, argument lengths, the builtin id, the write-redirection count, the status and the
 * request digest are. The digest is keyed (HMAC-SHA256, random per-journal key in <journal>.key, 0600), so a low-entropy secret
 * given as a plain argument cannot be confirmed by hashing guesses against a leaked journal alone; it also replaces the VALUE
 * of any assignment whose NAME is secret (osh_secret_name) by a fixed marker. Whoever holds journal AND key can test guesses.
 */
#ifndef OSH_JOURNAL_H
#define OSH_JOURNAL_H
#include <stddef.h>
#include <stdint.h>

#include "osh_host.h"

typedef struct OshJournal {
    int fd;
    uint64_t next_rec;
    uint8_t key[32];       /* per-journal HMAC key (loaded from <journal>.key) */
    char why[4700];         /* why osh_journal_open or osh_journal_check_dir refused */
    /* test-only fault injection; zero in production */
    int die_after_intent; /* _exit(77) right after the Nth intent became durable (simulates a crash before launch) */
    int n_intent;
} OshJournal;

typedef struct {
    uint64_t intent_rec;
    char digest[65];
    int idx, ncmds;
    char argv0[200];
} OshJournalUnknown;

/* Open (create if needed, 0600) and fsync the directory when created; load or atomically create <path>.key (32 random bytes, 0600).
 * FAIL CLOSED: a journal or key with another owner or a mode other than 0600, a key of the wrong size, an unreadable key, or a
 * missing key beside a journal that has records is refused (-errno, j->why names the reason). 0 ok, -errno. A torn last line (crash during a
 * write) is closed with '\n' so the next record starts on its own line. */
int osh_journal_open(OshJournal *j, const char *path);
void osh_journal_close(OshJournal *j);

/* Default location of the mandatory journal: $XDG_STATE_HOME/osh/effects.journal, else $HOME/.local/state/osh/effects.journal
 * (XDG Base Directory spec; the ABI draft and the osh docs name no location). 0 ok, -1 when neither variable is usable. */
int osh_journal_default_path(const char *xdg_state_home, const char *home, char *out, size_t cap);
/* mkdir -p the parent directories of path with mode 0700. 0 ok, -errno. */
int osh_journal_mkparents(const char *path);

/* Recovery: report every intent with no outcome as UNKNOWN (never replayed; nothing is launched here) and append an
 * OUTCOME_UNKNOWN record (recovered=1) for each so it is reported once. Returns the number found (all counted, at most
 * cap stored in out), or -errno. *bad (optional) gets the number of unparsable (torn) lines. */
int osh_journal_recover(OshJournal *j, const char *path, OshJournalUnknown *out, int cap, int *bad);

/* ---- used by osh_exec.c ---- */
int osh_secret_name(const char *name); /* 1 when the variable name is in the secret class (see osh_journal.c) */
/* Request digest: HMAC-SHA256 under the journal's random per-journal key over a canonical encoding of the request. */
void osh_req_digest(const OshJournal *j, const OshRequest *r, uint8_t out[32]);
/* The same encoding under plain SHA-256. NOT used by the host; it exists so the tests can show that the recorded (keyed)
 * digest cannot be matched by guessing arguments and hashing them. */
void osh_req_digest_unkeyed(const OshRequest *r, uint8_t out[32]);
/* A state directory that already exists must be ours with mode 0700, else refuse (-EACCES, j->why says why). */
int osh_journal_check_dir(OshJournal *j, const char *dir);
/* Append and fsync an intent for command idx. Returns its rec (> 0) or 0 when it is not durable (the effect must not run). */
uint64_t osh_journal_intent(OshJournal *j, const uint8_t dig[32], const OshRequest *r, int idx);

typedef struct {
    int idx;
    uint64_t ref; /* intent rec or 0 */
    int outcome, status, err;
} OshJournalOutcome;
/* Append all outcomes in one write and fsync. 0 ok, -1 not durable. */
int osh_journal_outcomes(OshJournal *j, const uint8_t dig[32], int ncmds, const OshJournalOutcome *o, int n);
#endif
