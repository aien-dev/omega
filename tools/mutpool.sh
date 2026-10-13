#!/bin/sh
# Bounded parallel pool for mutation sweeps. SOURCE this file (". tools/mutpool.sh"):
#   mp_init              once, before the first job
#   mp_run cmd args...   queue one mutant job (a shell function or command)
#   mp_wait              wait for all jobs, print each job's output in the order
#                        the jobs were queued, return 1 if any job failed
# Each job runs in its own subshell (inheriting the caller's set -e) with its own output and exit-code files, so a
# job's "exit 1" ends only that job. Pool size: MUTANT_JOBS (default nproc);
# MUTANT_JOBS=1 is the old serial behaviour. Output and verdicts are identical to
# running the jobs one after another, except that after a failure the remaining
# jobs still run and are reported.
mp_init() {
	MP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mutpool.XXXXXX") || return 1
	MP_N=0; MP_PIDS=""; MP_RUNNING=0
	MP_MAX=${MUTANT_JOBS:-$(nproc 2>/dev/null || echo 1)}
	case $MP_MAX in ''|*[!0-9]*|0) MP_MAX=1;; esac
}
mp_run() {
	MP_N=$((MP_N + 1)); _mp_i=$MP_N
	if [ "$MP_RUNNING" -ge "$MP_MAX" ]; then
		_mp_first=${MP_PIDS%% *}; MP_PIDS=${MP_PIDS#"$_mp_first"}; MP_PIDS=${MP_PIDS# }
		wait "$_mp_first" 2>/dev/null; MP_RUNNING=$((MP_RUNNING - 1))
	fi
	case $- in *e*) _mp_e=-e;; *) _mp_e=+e;; esac
	( set +e; ( set $_mp_e; "$@" ) > "$MP_DIR/$_mp_i.out" 2>&1; echo "$?" > "$MP_DIR/$_mp_i.rc" ) &
	MP_PIDS="$MP_PIDS${MP_PIDS:+ }$!"; MP_RUNNING=$((MP_RUNNING + 1))
}
mp_wait() {
	wait; _mp_bad=0; _mp_i=1
	while [ "$_mp_i" -le "$MP_N" ]; do
		cat "$MP_DIR/$_mp_i.out"
		[ "$(cat "$MP_DIR/$_mp_i.rc" 2>/dev/null || echo 1)" = 0 ] || _mp_bad=1
		_mp_i=$((_mp_i + 1))
	done
	rm -rf "$MP_DIR"
	[ "$_mp_bad" = 0 ]
}
