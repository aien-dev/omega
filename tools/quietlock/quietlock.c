/* quietlock: the Spark quiet flag as a real lock (HD-13).
 *
 * The quiet flag is the file $QUIETLOCK_DIR/.spark-quiet (default
 * $HOME/workspace/.spark-quiet). Its first line keeps the format that
 * ~/.claude/hooks/quiet-guard.sh and orchestrate-lanes/lanes.sh already read:
 *
 *   <f1> <f2> <f3> ... start=<iso> expected_end=<iso UTC> pid=<n> [hold=<id>]
 *
 * (first three fields = holder; pid= and expected_end= tokens anywhere).
 * quietlock adds a hold=<id> token; old lines without it still parse.
 *
 * Subcommands
 *   hold --owner ID --minutes N [--reason TEXT] -- CMD...
 *       Acquire the flag (flock on .spark-quiet.lock around the
 *       read-modify-write, flag published with link(2) so it never clobbers),
 *       run CMD with QUIETLOCK_HOLD=<id>, release on exit. Exit = CMD's exit.
 *       N > 20 needs the approval token (see below). Exit 75 if already held,
 *       77 if the approval token is missing or does not cover this hold.
 *   check
 *       Exit 0 if no flag, if the flag is stale (holder pid dead AND
 *       expected_end passed), or if $QUIETLOCK_HOLD equals the live hold id.
 *       Otherwise exit 75 with the holder on stderr.
 *   run -- CMD...
 *       check, then exec CMD.
 *   release-stale
 *       Remove the flag only when the holder pid is dead AND expected_end has
 *       passed (the quiet-guard.sh rule). Never touches a live hold (exit 3).
 *
 * Approval token ($QUIETLOCK_DIR/.spark-quiet-approval), key=value lines:
 *   owner=<ID>  max_minutes=<N>  expires_at=<iso UTC>
 * THIS IS A POLICY FILE, NOT CRYPTOGRAPHIC. Anyone who can write the
 * directory can write a token. It stops accidental or unthinking long holds
 * and leaves an audit line in .spark-quiet.history for every use; it does not
 * stop a determined local user. A signed token is a separate follow-up.
 *
 * Where the lock is enforced: at real entry points only (mk/quiet.mk for every
 * omega make goal, scripts run under `quietlock run --` or `quietlock hold`,
 * and lanes.sh queue/flush/forge once wired to call quietlock). The decision
 * is never made by pattern-matching command text; the Claude hook
 * (tools/quietlock/quiet-guard.sh) is only a courtesy filter in front of it.
 *
 * Rules worth knowing
 * - No state dir (GitHub CI, another machine): check is clear (exit 0).
 * - Refusals print the fixed marker QUIETLOCK_REFUSED and exit 75.
 * - Overrun: expected_end is ADVISORY (Drake 2026-10-01). If the command is
 *   still running when it passes, `hold` KEEPS the flag and logs one
 *   "overrun:" warning; the flag is released only when the command exits.
 *   The command is never killed.
 * - Legacy flags (no hold= token) let their holder through with
 *   QUIET_HOLDER=1, DEPRECATED, logged, for the transition only.
 * - QUIETLOCK_DIR moves the state dir (tests use it). Anyone can point it at an
 *   empty dir and pass check: this is a cooperative lock against accidents,
 *   not a security boundary, like the token below.
 * - Times must end in Z (UTC). A time without Z never counts as passed.
 *
 * Plain C + POSIX, plus flock(2) (Linux/BSD) as the brief requires.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXIT_HELD 75
#define EXIT_NOAPPROVAL 77
#define EXIT_LIVE 3
#define MAX_UNAPPROVED_MINUTES 20L
#define MAX_MINUTES (7L * 24L * 60L)

#define PBUF (PATH_MAX + 64)
static char g_dir[PATH_MAX];
static char g_flag[PBUF];
static char g_lock[PBUF];
static char g_hist[PBUF];
static char g_appr[PBUF];

struct flag {
	int present;
	char line[1024];
	char holder[256];
	long pid;
	int has_pid;
	time_t end;
	int has_end;
	char end_text[64];
	char hold[128];
};

static void die(const char *fmt, ...)
{
	va_list ap;
	fputs("quietlock: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void mkpath(char *dst, size_t n, const char *name)
{
	int r = snprintf(dst, n, "%s/%s", g_dir, name);
	if (r < 0 || (size_t)r >= n)
		die("state path too long");
}

static void paths_init(void)
{
	const char *d = getenv("QUIETLOCK_DIR");
	int r;
	if (d && *d) {
		r = snprintf(g_dir, sizeof g_dir, "%s", d);
	} else {
		const char *h = getenv("HOME");
		if (!h || !*h)
			die("neither QUIETLOCK_DIR nor HOME is set");
		r = snprintf(g_dir, sizeof g_dir, "%s/workspace", h);
	}
	if (r < 0 || (size_t)r >= sizeof g_dir)
		die("state dir path too long");
	mkpath(g_flag, sizeof g_flag, ".spark-quiet");
	mkpath(g_lock, sizeof g_lock, ".spark-quiet.lock");
	mkpath(g_hist, sizeof g_hist, ".spark-quiet.history");
	mkpath(g_appr, sizeof g_appr, ".spark-quiet-approval");
}

static void iso_utc(time_t t, char *buf, size_t n)
{
	struct tm tm;
	gmtime_r(&t, &tm);
	if (strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0 && n)
		buf[0] = '\0';
}

/* Accepts YYYY-MM-DDTHH:MM:SSZ and YYYY-MM-DDTHH:MMZ only. The trailing Z is
 * REQUIRED: quiet-guard.sh and lanes.sh read a no-Z time with `date -d` as local
 * time, so a no-Z (or +00:00) time is treated as unreadable, which never counts
 * as passed (fails safe: such a flag is never stale). */
static int parse_iso(const char *s, time_t *out)
{
	static const char *fmts[] = { "%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M" };
	size_t i;
	for (i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
		struct tm tm;
		char *e;
		memset(&tm, 0, sizeof tm);
		e = strptime(s, fmts[i], &tm);
		if (!e)
			continue;
		if (*e != 'Z' || e[1] != '\0')
			continue;
		*out = timegm(&tm);
		return *out == (time_t)-1 ? -1 : 0;
	}
	return -1;
}

static void history(const char *fmt, ...)
{
	char msg[2048], ts[32], out[2200];
	va_list ap;
	int fd, n;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	iso_utc(time(NULL), ts, sizeof ts);
	n = snprintf(out, sizeof out, "[%s] quietlock: %s\n", ts, msg);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof out)
		n = (int)sizeof out - 1;
	fd = open(g_hist, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "quietlock: cannot append history %s: %s\n", g_hist, strerror(errno));
		return;
	}
	if (write(fd, out, (size_t)n) != n)
		fprintf(stderr, "quietlock: short write to history\n");
	close(fd);
}

static int lock_take(int op)
{
	int fd = open(g_lock, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		die("cannot open lock file %s: %s", g_lock, strerror(errno));
	while (flock(fd, op) != 0) {
		if (errno != EINTR)
			die("flock %s: %s", g_lock, strerror(errno));
	}
	return fd;
}

static void lock_drop(int fd)
{
	flock(fd, LOCK_UN);
	close(fd);
}

static void read_flag(struct flag *f)
{
	FILE *fp;
	char copy[1024], *tok, *save = NULL;
	int field = 0;
	size_t hl = 0;

	memset(f, 0, sizeof *f);
	fp = fopen(g_flag, "r");
	if (!fp) {
		if (errno != ENOENT)
			die("cannot read flag %s: %s", g_flag, strerror(errno));
		return;
	}
	f->present = 1;
	if (!fgets(f->line, sizeof f->line, fp))
		f->line[0] = '\0';
	fclose(fp);
	f->line[strcspn(f->line, "\r\n")] = '\0';

	memcpy(copy, f->line, sizeof copy);
	for (tok = strtok_r(copy, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (field < 3) {
			size_t tl = strlen(tok);
			if (hl + tl + 2 < sizeof f->holder) {
				if (hl)
					f->holder[hl++] = ' ';
				memcpy(f->holder + hl, tok, tl);
				hl += tl;
				f->holder[hl] = '\0';
			}
			field++;
		}
		if (!f->has_pid && strncmp(tok, "pid=", 4) == 0) {
			char *e;
			long v;
			errno = 0;
			v = strtol(tok + 4, &e, 10);
			if (e != tok + 4 && errno == 0 && v > 0) {
				f->pid = v;
				f->has_pid = 1;
			}
		} else if (!f->has_end && strncmp(tok, "expected_end=", 13) == 0) {
			snprintf(f->end_text, sizeof f->end_text, "%s", tok + 13);
			if (parse_iso(tok + 13, &f->end) == 0)
				f->has_end = 1;
		} else if (!f->hold[0] && strncmp(tok, "hold=", 5) == 0) {
			snprintf(f->hold, sizeof f->hold, "%s", tok + 5);
		}
	}
}

static int pid_alive(long pid)
{
	if (pid <= 0)
		return 0;
	if (kill((pid_t)pid, 0) == 0)
		return 1;
	return errno == EPERM; /* exists, owned by someone else */
}

/* Stale = holder pid known and dead AND expected_end known and passed. */
static int flag_stale(const struct flag *f)
{
	if (!f->has_pid || pid_alive(f->pid))
		return 0;
	if (!f->has_end)
		return 0;
	return time(NULL) > f->end;
}

static void describe(const struct flag *f, char *buf, size_t n)
{
	char pid[32];
	if (f->has_pid)
		snprintf(pid, sizeof pid, "%ld", f->pid);
	else
		snprintf(pid, sizeof pid, "?");
	snprintf(buf, n, "holder='%s' pid=%s alive=%s expected_end=%s%s", f->holder[0] ? f->holder : "unknown",
		 pid, f->has_pid && pid_alive(f->pid) ? "yes" : "no", f->end_text[0] ? f->end_text : "?",
		 f->has_pid && pid_alive(f->pid) && f->has_end && time(NULL) > f->end ? " overrun=yes" : "");
}

/* No state dir (GitHub CI, another machine, fresh account): nothing can be held. */
static int state_dir_missing(void)
{
	struct stat st;
	return stat(g_dir, &st) != 0 && errno == ENOENT;
}

static int cmd_check(void)
{
	struct flag f;
	const char *mine = getenv("QUIETLOCK_HOLD");
	const char *legacy = getenv("QUIET_HOLDER");
	char d[512];
	int fd;
	if (state_dir_missing()) {
		fprintf(stderr, "quietlock: no state dir %s, so no quiet flag: clear\n", g_dir);
		return 0;
	}
	fd = lock_take(LOCK_SH);
	read_flag(&f);
	lock_drop(fd);
	if (!f.present)
		return 0;
	if (f.hold[0] && mine && strcmp(mine, f.hold) == 0)
		return 0;
	describe(&f, d, sizeof d);
	if (flag_stale(&f)) {
		fprintf(stderr, "quietlock: stale quiet flag (%s): holder dead and past expected_end, treating as clear; "
				"'quietlock release-stale' removes it\n", d);
		return 0;
	}
	/* DEPRECATED transition escape: a legacy flag (written before quietlock, so no
	 * hold= token) lets its holder through with QUIET_HOLDER=1, as quiet-guard.sh
	 * did. Never applies to a quietlock hold. Every use is logged. */
	if (!f.hold[0] && legacy && strcmp(legacy, "1") == 0) {
		history("DEPRECATED QUIET_HOLDER=1 passed legacy flag: %s", f.line);
		fprintf(stderr, "quietlock: legacy flag passed with QUIET_HOLDER=1 (deprecated; use 'quietlock hold')\n");
		return 0;
	}
	fprintf(stderr, "quietlock: QUIETLOCK_REFUSED Spark quiet flag HELD (%s) at %s. Builds and tests wait until it "
			"is released. The holder runs its commands under 'quietlock hold'.\n", d, g_flag);
	return EXIT_HELD;
}

static int valid_owner(const char *s)
{
	size_t n = strlen(s), i;
	if (n == 0 || n > 64)
		return 0;
	for (i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		      c == '_' || c == '.'))
			return 0;
	}
	return 1;
}

static int parse_long(const char *s, long *out)
{
	char *e;
	long v;
	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtol(s, &e, 10);
	if (*e != '\0' || errno)
		return -1;
	*out = v;
	return 0;
}

/* 0 = token covers (owner, minutes); else -1 with why. Writes no history. */
static int approval_ok(const char *owner, long minutes, char *why, size_t wn, char *info, size_t in)
{
	FILE *fp = fopen(g_appr, "r");
	char line[512], tok_owner[128] = "", tok_max[64] = "", tok_exp[64] = "";
	long maxm;
	time_t exp;
	if (!fp) {
		snprintf(why, wn, "no approval token at %s", g_appr);
		return -1;
	}
	while (fgets(line, sizeof line, fp)) {
		char *k = line, *v;
		line[strcspn(line, "\r\n")] = '\0';
		while (*k == ' ' || *k == '\t')
			k++;
		if (*k == '#' || *k == '\0')
			continue;
		v = strchr(k, '=');
		if (!v)
			continue;
		*v++ = '\0';
		if (strcmp(k, "owner") == 0)
			snprintf(tok_owner, sizeof tok_owner, "%s", v);
		else if (strcmp(k, "max_minutes") == 0)
			snprintf(tok_max, sizeof tok_max, "%s", v);
		else if (strcmp(k, "expires_at") == 0)
			snprintf(tok_exp, sizeof tok_exp, "%s", v);
	}
	fclose(fp);
	snprintf(info, in, "token owner=%s max_minutes=%s expires_at=%s", tok_owner, tok_max, tok_exp);
	if (strcmp(tok_owner, owner) != 0) {
		snprintf(why, wn, "approval token names owner '%s', not '%s'", tok_owner, owner);
		return -1;
	}
	if (parse_long(tok_max, &maxm) != 0 || maxm < minutes) {
		snprintf(why, wn, "approval token max_minutes '%s' does not cover %ld minutes", tok_max, minutes);
		return -1;
	}
	if (parse_iso(tok_exp, &exp) != 0) {
		snprintf(why, wn, "approval token expires_at '%s' is unreadable", tok_exp);
		return -1;
	}
	if (time(NULL) >= exp) {
		snprintf(why, wn, "approval token expired at %s", tok_exp);
		return -1;
	}
	return 0;
}

static volatile sig_atomic_t g_child;
static volatile sig_atomic_t g_sig;
static const int g_fwd[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };

static void on_signal(int sig)
{
	g_sig = sig;
	if (g_child > 0)
		kill((pid_t)g_child, sig);
}

static void make_hold_id(char *buf, size_t n)
{
	unsigned int r = 0;
	struct timespec ts;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (read(fd, &r, sizeof r) != (ssize_t)sizeof r)
			r = 0;
		close(fd);
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	r ^= (unsigned int)ts.tv_nsec;
	snprintf(buf, n, "q%ld-%lld-%08x", (long)getpid(), (long long)ts.tv_sec, r);
}

static void on_alarm(int sig)
{
	(void)sig; /* only interrupts waitpid() */
}

/* Remove the flag only if it still carries our hold id (never a foreign flag). */
static void release_mine(const char *id, int rc, int overrun)
{
	struct flag f;
	int fd = lock_take(LOCK_EX);
	read_flag(&f);
	if (f.present && strcmp(f.hold, id) == 0) {
		if (unlink(g_flag) != 0)
			fprintf(stderr, "quietlock: cannot remove flag %s: %s\n", g_flag, strerror(errno));
		if (overrun)
			history("hold %s released when its command exited after overrun (command exit %d)", id, rc);
		else
			history("hold %s released (command exit %d)", id, rc);
	} else {
		history("hold %s: flag already gone or replaced at exit, left alone (command exit %d)", id, rc);
	}
	lock_drop(fd);
}

/* Seconds per --minutes unit. 60, except a test build may shorten it with
 * QUIETLOCK_TEST=1 QUIETLOCK_TEST_MINUTE_SECONDS=N (only ever SHORTER holds). */
static long minute_seconds(void)
{
	const char *t = getenv("QUIETLOCK_TEST"), *s = getenv("QUIETLOCK_TEST_MINUTE_SECONDS");
	long v;
	if (t && strcmp(t, "1") == 0 && parse_long(s, &v) == 0 && v >= 1 && v < 60)
		return v;
	return 60;
}

static int cmd_hold(const char *owner, const char *mins, const char *reason_in, char **argv)
{
	long minutes;
	char reason[65], why[PBUF + 128], info[512], id[96], start[32], end[32], line[512], tmp[PBUF + 32], d[512];
	struct flag f;
	struct sigaction sa;
	size_t i;
	int fd, tfd, n, status = 0, rc;
	pid_t child;
	time_t now, end_t;
	int overrun = 0;

	if (!owner || !valid_owner(owner)) {
		fprintf(stderr, "quietlock: --owner must be 1-64 chars of A-Z a-z 0-9 . _ -\n");
		return 2;
	}
	if (parse_long(mins, &minutes) != 0 || minutes < 1 || minutes > MAX_MINUTES) {
		fprintf(stderr, "quietlock: --minutes must be an integer from 1 to %ld\n", MAX_MINUTES);
		return 2;
	}
	snprintf(reason, sizeof reason, "%s", reason_in && *reason_in ? reason_in : "hold");
	for (i = 0; reason[i]; i++) {
		char c = reason[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		      c == '_' || c == '.'))
			reason[i] = '_';
	}

	if (state_dir_missing() && mkdir(g_dir, 0755) != 0 && errno != EEXIST)
		die("cannot create state dir %s: %s", g_dir, strerror(errno));
	fd = lock_take(LOCK_EX);
	read_flag(&f);
	if (f.present) {
		describe(&f, d, sizeof d);
		lock_drop(fd);
		fprintf(stderr, "quietlock: QUIETLOCK_REFUSED hold refused, quiet flag already held (%s)%s\n", d,
			flag_stale(&f) ? "; it is stale, run 'quietlock release-stale' first" : "");
		return EXIT_HELD;
	}
	if (minutes > MAX_UNAPPROVED_MINUTES) {
		info[0] = '\0';
		if (approval_ok(owner, minutes, why, sizeof why, info, sizeof info) != 0) {
			history("REFUSED %ld-minute hold for %s: %s", minutes, owner, why);
			lock_drop(fd);
			fprintf(stderr, "quietlock: refused: holds over %ld minutes need Drake's approval token: %s\n",
				MAX_UNAPPROVED_MINUTES, why);
			return EXIT_NOAPPROVAL;
		}
		history("approval token USED for %ld-minute hold by %s (%s)", minutes, owner, info);
	}

	make_hold_id(id, sizeof id);
	now = time(NULL);
	iso_utc(now, start, sizeof start);
	end_t = now + minutes * minute_seconds();
	iso_utc(end_t, end, sizeof end);
	n = snprintf(line, sizeof line, "%s quietlock %s start=%s expected_end=%s pid=%ld hold=%s\n", owner, reason,
		     start, end, (long)getpid(), id);
	if (n < 0 || (size_t)n >= sizeof line) {
		lock_drop(fd);
		die("flag line too long");
	}
	snprintf(tmp, sizeof tmp, "%s.tmp.%ld", g_flag, (long)getpid());
	unlink(tmp); /* leftover of a crashed run with the same pid; we hold the flock */
	tfd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (tfd < 0) {
		lock_drop(fd);
		die("cannot create %s: %s", tmp, strerror(errno));
	}
	if (write(tfd, line, (size_t)n) != n || fsync(tfd) != 0) {
		close(tfd);
		unlink(tmp);
		lock_drop(fd);
		die("cannot write %s", tmp);
	}
	close(tfd);
	/* link(2) publishes the complete line and fails if a flag appeared meanwhile. */
	if (link(tmp, g_flag) != 0) {
		int e = errno;
		unlink(tmp);
		lock_drop(fd);
		if (e == EEXIST) {
			fprintf(stderr, "quietlock: refused: quiet flag appeared while acquiring\n");
			return EXIT_HELD;
		}
		die("cannot publish flag %s: %s", g_flag, strerror(e));
	}
	unlink(tmp);
	line[n - 1] = '\0';
	history("hold acquired: %s", line);
	lock_drop(fd);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	for (i = 0; i < sizeof g_fwd / sizeof g_fwd[0]; i++)
		sigaction(g_fwd[i], &sa, NULL);
	sa.sa_handler = on_alarm; /* no SA_RESTART: interrupts waitpid at expected_end */
	sigaction(SIGALRM, &sa, NULL);

	child = fork();
	if (child < 0) {
		fprintf(stderr, "quietlock: fork: %s\n", strerror(errno));
		release_mine(id, 1, 0);
		return 1;
	}
	if (child == 0) {
		struct sigaction dfl;
		memset(&dfl, 0, sizeof dfl);
		dfl.sa_handler = SIG_DFL;
		sigemptyset(&dfl.sa_mask);
		for (i = 0; i < sizeof g_fwd / sizeof g_fwd[0]; i++)
			sigaction(g_fwd[i], &dfl, NULL);
		if (setenv("QUIETLOCK_HOLD", id, 1) != 0)
			_exit(127);
		execvp(argv[0], argv);
		fprintf(stderr, "quietlock: cannot run %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	g_child = child;
	if (g_sig)
		kill(child, g_sig);
	/* Drake 2026-10-01: expected_end is ADVISORY. When it passes and the command
	 * is still running, the hold is KEPT and one overrun warning is logged. The
	 * flag is released only when the command exits (below). The command is NEVER
	 * killed (standing rule: chip tests are never killed). */
	for (;;) {
		pid_t w;
		if (!overrun) {
			time_t t = time(NULL);
			if (t >= end_t) {
				overrun = 1;
				history("overrun: hold %s passed expected_end while command still running; flag KEPT "
					"until the command exits, command NOT killed", id);
				fprintf(stderr, "quietlock: WARNING overrun: hold %s passed expected_end; flag kept until "
						"the command exits\n", id);
			} else {
				alarm((unsigned)(end_t - t));
			}
		}
		w = waitpid(child, &status, 0);
		if (w == child)
			break;
		if (w < 0 && errno != EINTR) {
			status = 1 << 8;
			break;
		}
	}
	alarm(0);
	if (WIFEXITED(status))
		rc = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		rc = 128 + WTERMSIG(status);
	else
		rc = 1;
	release_mine(id, rc, overrun);
	return rc;
}

static int cmd_release_stale(void)
{
	struct flag f;
	char d[512];
	int fd;
	if (state_dir_missing()) {
		printf("no flag\n");
		return 0;
	}
	fd = lock_take(LOCK_EX);
	read_flag(&f);
	if (!f.present) {
		lock_drop(fd);
		printf("no flag\n");
		return 0;
	}
	describe(&f, d, sizeof d);
	if (!flag_stale(&f)) {
		lock_drop(fd);
		printf("flag is LIVE (%s); not touching it\n", d);
		return EXIT_LIVE;
	}
	if (unlink(g_flag) != 0) {
		int e = errno;
		lock_drop(fd);
		die("cannot remove flag %s: %s", g_flag, strerror(e));
	}
	history("released stale flag: %s", f.line);
	lock_drop(fd);
	printf("released stale flag (holder dead, past expected_end)\n");
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: quietlock hold --owner ID --minutes N [--reason TEXT] -- CMD...\n"
			"       quietlock check\n"
			"       quietlock run -- CMD...\n"
			"       quietlock release-stale\n"
			"state dir: $QUIETLOCK_DIR (default $HOME/workspace)\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 2;
	}
	paths_init();
	if (strcmp(argv[1], "check") == 0 && argc == 2)
		return cmd_check();
	if (strcmp(argv[1], "release-stale") == 0 && argc == 2)
		return cmd_release_stale();
	if (strcmp(argv[1], "run") == 0) {
		int rc;
		if (argc < 4 || strcmp(argv[2], "--") != 0) {
			usage();
			return 2;
		}
		rc = cmd_check();
		if (rc != 0)
			return rc;
		execvp(argv[3], argv + 3);
		fprintf(stderr, "quietlock: cannot run %s: %s\n", argv[3], strerror(errno));
		return 127;
	}
	if (strcmp(argv[1], "hold") == 0) {
		const char *owner = NULL, *mins = NULL, *reason = NULL;
		int i;
		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--") == 0)
				break;
			if (i + 1 >= argc) {
				usage();
				return 2;
			}
			if (strcmp(argv[i], "--owner") == 0)
				owner = argv[++i];
			else if (strcmp(argv[i], "--minutes") == 0)
				mins = argv[++i];
			else if (strcmp(argv[i], "--reason") == 0)
				reason = argv[++i];
			else {
				usage();
				return 2;
			}
		}
		if (i >= argc - 1) {
			usage();
			return 2;
		}
		return cmd_hold(owner, mins, reason, argv + i + 1);
	}
	usage();
	return 2;
}
