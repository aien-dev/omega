/*
 * osh_host.h -- Linux host adapter for osh (aien-architecture#158): the execution service.
 *
 * Host code is replaceable scaffolding (ADR 0024). It never tokenizes, parses or expands: it consumes request
 * records laid out as OSH_PLATFORM_ABI.md section 7 (v1 DRAFT, aien-protocols branch osh/platform-abi-v1-draft)
 * and performs them. No shell is ever invoked (no sh -c, system(), popen()); every program is started with
 * execve() on an argv this code built itself.
 *
 * Semantics decided here, where the ABI draft is silent, are listed at the top of osh_exec.c ("DECISIONS").
 */
#ifndef OSH_HOST_H
#define OSH_HOST_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---- ABI constants (section 3, 5.3, 6.1, 7.1, 8.6, 10) ---- */
#define OSH_REQ_MAGIC 0x4F524551u /* "OREQ" */
#define OSH_REQ_VERSION 1
#define OSH_REQ_HDR_CELLS 8
#define OSH_CMD_CELLS 164
#define OSH_MAX_CMDS 8
#define OSH_MAX_ARGV 32
#define OSH_MAX_ASSIGN 16
#define OSH_MAX_REDIR 8
#define OSH_OUT_CAP 8192 /* CAP_OUT */
#define OSH_REQ_FLAG_DIRECT 1u

enum { /* refusal codes, section 5.3 (ABI group) */
    OSH_ABI_MAGIC = 214, OSH_ABI_VERSION = 215, OSH_ABI_LENGTH = 216, OSH_ABI_RESERVED = 217, OSH_REQ_FIELD = 218
};

enum { /* platform error codes, section 8.6 (separate namespace from refusals) */
    OSH_E_OK = 0, OSH_E_DENIED = 1, OSH_E_REVOKED = 2, OSH_E_STALE = 3, OSH_E_NOT_FOUND = 4, OSH_E_UNAVAILABLE = 5,
    OSH_E_INVALID_ARG = 6, OSH_E_LIMIT = 7, OSH_E_INTERRUPTED = 8, OSH_E_BROKEN_PIPE = 9, OSH_E_IO = 10,
    OSH_E_OUTCOME_UNKNOWN = 11, OSH_E_CAP_DOMAIN_MISMATCH = 12, OSH_E_CAP_GEN_NARROW = 13, OSH_E_NOT_SUPPORTED = 14,
    OSH_E_WOULD_BLOCK = 15, OSH_E_PARTIAL_LAUNCH = 16
};

enum { /* section 10 outcomes, per command */
    OSH_OUT_NOT_STARTED = 0, OSH_OUT_COMPLETED, OSH_OUT_FAILED_NO_EFFECT, OSH_OUT_CANCELLED, OSH_OUT_UNKNOWN
};

enum { OSH_B_NONE = 0, OSH_B_CD, OSH_B_PWD, OSH_B_PRINTF, OSH_B_EXPORT, OSH_B_UNSET, OSH_B_EXIT };
enum { OSH_R_IN = 1, OSH_R_OUT = 2, OSH_R_APPEND = 3, OSH_R_DUP = 4 };
enum { OSH_CONN_NONE = 0, OSH_CONN_SEMI = 1, OSH_CONN_AND = 2, OSH_CONN_OR = 3 };
enum { OSH_CLASS_OMEGA_NATIVE_OP = 1, OSH_CLASS_AIENOS_ARTIFACT = 2, OSH_CLASS_AIENOS_SERVICE = 3, OSH_CLASS_LINUX = 4 };

/* ---- decoded request (the one struct both the record path and the direct-argv path produce) ---- */
typedef struct {
    int kind;         /* OSH_R_* */
    int fd;           /* descriptor being redirected, 0..2 */
    int src_fd;       /* OSH_R_DUP: source descriptor M, 0..2 */
    const char *path; /* kinds 1..3: NUL-terminated target (points into OshRequest.arena) */
} OshRedir;

typedef struct {
    const char *name, *value; /* NUL-terminated, point into the arena */
} OshAssign;

typedef struct {
    int builtin_id;
    int nargv, nassign, nredir;
    const char *argv[OSH_MAX_ARGV + 1]; /* NULL terminated */
    OshAssign assign[OSH_MAX_ASSIGN];
    OshRedir redir[OSH_MAX_REDIR];
} OshCmd;

typedef struct {
    int ncmds;
    int connector_after;
    unsigned flags;
    OshCmd cmd[OSH_MAX_CMDS];
    /* Owned copy of the OUT bytes, each string NUL terminated by the decoder (so no pointer into the caller's
     * workspace survives the call: borrow per call, ABI section 4.1). */
    char arena[OSH_OUT_CAP + OSH_MAX_CMDS * (OSH_MAX_ARGV + 2 * OSH_MAX_ASSIGN + OSH_MAX_REDIR) + 8];
    size_t arena_used;
} OshRequest;

/* Decode a request record. rec = cells of the record (header 8 + ncmds*164), out = the OUT area, one byte per
 * cell. Check order: length, magic, version, reserved, then field validation (REQ_FIELD).
 * Returns 0 and fills *r, or a refusal code (214..218); *r is unspecified after a refusal and nothing is partially
 * applied. A pointer-free function of its inputs. */
int osh_req_decode(const uint64_t *rec, size_t nrec, const uint64_t *out, size_t nout, OshRequest *r);

/* ---- record builder (hand-built records in tests, and the direct-argv path) ---- */
typedef struct {
    uint64_t rec[OSH_REQ_HDR_CELLS + OSH_MAX_CMDS * OSH_CMD_CELLS];
    uint64_t out[OSH_OUT_CAP];
    size_t out_used;
    int ncmds, err; /* err: sticky OSH_REQ_FIELD / OSH_ABI_* */
} OshBuilder;

void osh_rb_init(OshBuilder *b, unsigned flags, int connector_after);
void osh_rb_cmd(OshBuilder *b, int builtin_id);                          /* start the next command */
void osh_rb_arg(OshBuilder *b, const char *s);                           /* append argv entry */
void osh_rb_assign(OshBuilder *b, const char *name, const char *value);  /* prefix assignment */
void osh_rb_redir(OshBuilder *b, int kind, int fd, const char *path_or_null, int src_fd);
size_t osh_rb_cells(const OshBuilder *b); /* cells in rec (8 + ncmds*164) */
void osh_rb_seal(OshBuilder *b);          /* writes header cells (out_used, ncmds, version) */

/* ---- session ---- */
typedef struct {
    uint8_t principal_id[32];
    uint8_t domain; /* 1 kernel IPC table, 2 hosted capability authority */
    uint32_t cap_index;
    uint64_t cap_generation;
    uint16_t resource_class, operation;
    int valid;
} OshBinding;

typedef struct {
    char *name, *value; /* value NULL: exported name that has no value yet */
    int exported;
} OshVar;

struct OshSession;
/* RESOLVE class hook: return OSH_CLASS_* for a command name. NULL means every name is OSH_CLASS_LINUX. */
typedef int (*OshClassHook)(void *ctx, const char *name);
/* PERM_CHECK hook: return OSH_E_OK or a denial reason (OSH_E_*). NULL means any valid binding is allowed. */
typedef int (*OshPermHook)(void *ctx, const OshBinding *b, int op);
enum { OSH_OP_SPAWN = 1, OSH_OP_CHDIR = 2, OSH_OP_OPEN_WRITE = 3, OSH_OP_OPEN_READ = 4 };
/* EFFECT hook (ABI section 9.2): called with the canonical-able target path immediately before the syscall that would
 * make the effect (execve/fork of a resolved program, open for a redirection, chdir). Return OSH_E_OK or a denial
 * (OSH_E_DENIED / REVOKED / STALE / CAP_DOMAIN_MISMATCH ...). On denial the effect does not happen. NULL = not gated
 * (embedder-supplied authorization only; the osh program installs one when started with --caps). */
typedef int (*OshEffectHook)(void *ctx, const OshBinding *b, int op, const char *path);

typedef struct OshSession {
    OshVar *vars;
    size_t nvars, capvars;
    int last_status;             /* $? */
    int fd[3];                   /* the shell's own stdin/stdout/stderr (default 0,1,2); never closed by osh */
    int interactive;             /* nonzero: foreground pipelines take the terminal on tty_fd */
    int tty_fd;                  /* controlling terminal descriptor (interactive only) */
    uint64_t session_epoch;
    OshBinding binding;
    OshClassHook class_hook;
    OshPermHook perm_hook;
    void *hook_ctx;
    OshEffectHook effect_hook;   /* per-effect authority check, see OshEffectHook */
    void *effect_ctx;
    int exit_requested, exit_status; /* set by the parent `exit` builtin */
    int abort_list;              /* set by the parent `exit` with too many arguments: bash drops the rest of the list */
    struct OshJournal *journal;  /* durable intent/outcome journal (osh_journal.h, ABI section 10); NULL = none */
    /* test-only fault injection; all zero in production */
    int fail_fork_at;                          /* 1-based index of the command whose fork() is made to fail */
    void (*after_launch_hook)(void *);         /* called after the last command is started, before waiting */
    void *after_launch_ctx;
} OshSession;

/* Fill the variable table from envp (NAME=value strings; entries without '=' or with an invalid name are ignored)
 * and set PWD per POSIX (kept if it names the current directory, else getcwd). All imported variables are exported. */
int osh_session_init(OshSession *s, char *const *envp);
void osh_session_free(OshSession *s);
const char *osh_var_get(const OshSession *s, const char *name);               /* NULL if unset */
int osh_var_set(OshSession *s, const char *name, const char *value);          /* keeps export flag; 0 ok, -1 invalid/oom */
int osh_var_export(OshSession *s, const char *name, const char *value_or_null);
int osh_var_unset(OshSession *s, const char *name);
int osh_name_valid(const char *s);
char **osh_build_envp(const OshSession *s, const OshAssign *ov, int nov);     /* malloc'd, free with osh_envp_free */
void osh_envp_free(char **envp);

/* ---- results ---- */
typedef struct {
    pid_t pid;      /* 0 when no process was started */
    int status;     /* exit status of this command (127, 126, 128+sig, ...) */
    int outcome;    /* OSH_OUT_* */
    int termsig;    /* nonzero when killed by a signal */
    int err;        /* OSH_E_* for this command */
} OshCmdResult;

typedef struct {
    int status;     /* pipeline status = last command's status; also stored in session->last_status */
    int err;        /* OSH_E_*: OK, DENIED, UNAVAILABLE, PARTIAL_LAUNCH, OUTCOME_UNKNOWN ... */
    int ncmds;
    OshCmdResult cmd[OSH_MAX_CMDS];
    int sigint_seen;     /* the shell itself received SIGINT while waiting (forwarded to the pipeline only when interactive; see osh_exec.c) */
    int killed_by_int;   /* the last command died of SIGINT or SIGQUIT: list execution must stop */
    int exit_requested;  /* mirrors session: parent `exit` ran */
    int refusal;         /* decoder refusal code if the request was refused before any effect, else 0 */
    int journal_failed;  /* an outcome record could not be made durable (the intent stays; recovery reports UNKNOWN) */
} OshResult;

/* Execute one decoded request (one pipeline). Returns the pipeline status. */
int osh_exec(OshSession *s, const OshRequest *r, OshResult *res);
/* Decode then execute. A refused record runs nothing: res->refusal is set, status is 2, return value is 2. */
int osh_exec_record(OshSession *s, const uint64_t *rec, size_t nrec, const uint64_t *out, size_t nout, OshResult *res);

/* connector semantics: should the pipeline after `connector_after` run, given $? of what ran? */
int osh_conn_should_run(int connector_after, int last_status);
/* Run a list of already decoded pipelines with && || ; and $? tracking. Returns final $?. nres may be NULL. */
int osh_run_list(OshSession *s, const OshRequest *const *reqs, int n, OshResult *res_out);

/* AIEN direct-argv submission (ABI section 11): builds the SAME record, decodes it through the SAME decoder and
 * runs the SAME osh_exec. No shell string exists. redirs are OshRedir with path or src_fd filled. */
int osh_submit_argv(OshSession *s, const char *const *argv, int nargv, const OshAssign *ov, int nov,
                    const OshRedir *redirs, int nredir, OshResult *res);

/* path resolution exposed for tests: 0 found (out filled), 126, or 127. */
int osh_resolve_linux(const OshSession *s, const char *name, char *out, size_t outsz);

#endif /* OSH_HOST_H */
