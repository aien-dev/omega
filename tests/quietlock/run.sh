#!/bin/sh
# tests/quietlock/run.sh -- negative tests for tools/quietlock (HD-13).
# POSIX sh. Uses a private temp state dir via QUIETLOCK_DIR; never touches the
# real ~/workspace/.spark-quiet. Each test lists the code change ("mutation")
# that makes it fail, so a removed guard cannot pass silently.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/quietlock-test.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT INT TERM
S=$T/state
mkdir -p "$S"
QUIETLOCK_DIR=$S
export QUIETLOCK_DIR
unset QUIETLOCK_HOLD
case "$S" in "$HOME"/workspace|"$HOME"/workspace/) echo "FAIL safety: state dir is the real one"; exit 1;; esac

FLAG=$S/.spark-quiet
HIST=$S/.spark-quiet.history
APPR=$S/.spark-quiet-approval
PAST=2000-01-01T00:00:00Z
FUTURE=2099-01-01T00:00:00Z
Q=$T/quietlock
MK="make -s -C $HERE OUT_DIR=$T/build"

${CC:-gcc} -std=gnu11 -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -o "$Q" "$HERE/tools/quietlock/quietlock.c" \
	|| { echo "FAIL build quietlock"; exit 1; }

FAILS=0
N=0
check() {
	N=$((N + 1))
	if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi
}
reset() { rm -f "$FLAG" "$APPR" "$T"/ran*; }
dead_pid() {
	sh -c 'exit 0' &
	p=$!
	wait "$p"
	if kill -0 "$p" 2>/dev/null; then echo 0; else echo "$p"; fi
}
live_flag() { # someone else holds, pid = this shell (alive)
	echo "OTHER quietlock test start=$PAST expected_end=$FUTURE pid=$$ hold=qOTHER-1-0" > "$FLAG"
}
token() { printf '# POLICY FILE, NOT CRYPTOGRAPHIC (test)\nowner=%s\nmax_minutes=%s\nexpires_at=%s\n' "$1" "$2" "$3" > "$APPR"; }

# --- T0 bootstrap: `make quietlock` builds the tool even while someone else holds.
# Mutation killed: drop the QUIETLOCK_BOOT_GOALS filter in mk/quiet.mk (check runs for every goal).
reset; live_flag
out=$($MK quietlock 2>&1); rc=$?
check "T0 make quietlock bootstraps while held by another" '[ $rc = 0 ] && [ -x "$T/build/quietlock" ]'

# --- T1 second hold refused while the first is live (nested hold).
# Mutation killed: in cmd_hold, skip the `if (f.present)` refusal AND publish with rename() instead of link().
reset
"$Q" hold --owner A --minutes 1 -- "$Q" hold --owner B --minutes 1 -- touch "$T/ran1" 2>"$T/err1"; rc=$?
check "T1 second hold refused while first live (exit 75)" '[ $rc = 75 ] && [ ! -e "$T/ran1" ]'
check "T1 first hold released its flag on exit" '[ ! -e "$FLAG" ]'
# T1b same against an old-format flag held by a live pid.
# Mutation killed: same as T1.
echo "M18 gate16 desc start=$PAST expected_end=$FUTURE pid=$$" > "$FLAG"
"$Q" hold --owner B --minutes 1 -- touch "$T/ran1b" 2>/dev/null; rc=$?
check "T1b hold refused over an old-format live flag" '[ $rc = 75 ] && [ ! -e "$T/ran1b" ] && grep -q "pid=$$" "$FLAG"'

# --- T2 make goal refused while held by someone else.
# Mutation killed: delete the $(error ...) in mk/quiet.mk, delete mk/quiet.mk, or make cmd_check return 0.
reset; live_flag
out=$($MK quietlock-check 2>&1); rc=$?
check "T2 make goal refused while held by another" '[ $rc != 0 ] && echo "$out" | grep -q "HELD" && ! echo "$out" | grep -q "clear for this make run"'
# T2b a wrong QUIETLOCK_HOLD does not open the lock.
# Mutation killed: cmd_check accepting any non-empty QUIETLOCK_HOLD.
out=$(QUIETLOCK_HOLD=qWRONG $MK quietlock-check 2>&1); rc=$?
check "T2b make refused with a wrong QUIETLOCK_HOLD" '[ $rc != 0 ] && ! echo "$out" | grep -q "clear for this make run"'
# T2c control: clear flag lets make through (proves T2 is not an unconditional failure).
# Mutation killed: cmd_check always returning 75.
reset
out=$($MK quietlock-check 2>&1); rc=$?
check "T2c make passes when no flag" '[ $rc = 0 ] && echo "$out" | grep -q "clear for this make run"'
# T2d run -- CMD refused while held, runs when clear.
# Mutation killed: cmd `run` exec'ing without calling cmd_check.
live_flag
"$Q" run -- touch "$T/ran2d" 2>/dev/null; rc=$?
check "T2d run refused while held" '[ $rc = 75 ] && [ ! -e "$T/ran2d" ]'
reset
"$Q" run -- touch "$T/ran2e"; rc=$?
check "T2e run executes when clear" '[ $rc = 0 ] && [ -e "$T/ran2e" ]'

# --- T3 the holder's own command passes.
# Mutation killed: drop setenv("QUIETLOCK_HOLD") in the child, or drop the hold-id match in cmd_check.
reset
out=$("$Q" hold --owner A --minutes 1 -- $MK quietlock-check 2>&1); rc=$?
check "T3 holder's own make passes" '[ $rc = 0 ] && echo "$out" | grep -q "clear for this make run"'
"$Q" hold --owner A --minutes 1 -- "$Q" check; rc=$?
check "T3b holder's own check exits 0" '[ $rc = 0 ]'
"$Q" hold --owner A --minutes 1 -- sh -c 'exit 7'; rc=$?
check "T3c hold returns the command exit status and releases" '[ $rc = 7 ] && [ ! -e "$FLAG" ]'
# T3d flag written by hold stays readable by quiet-guard.sh / lanes.sh (same shell parsing).
# Mutation killed: changing the flag line (drop pid= / expected_end= / move owner out of field 1).
"$Q" hold --owner A --minutes 5 --reason "two words" -- sh -c 'head -1 "$1" > "$2"; echo $PPID > "$3"' sh "$FLAG" "$T/line" "$T/ppid"
l=$(cat "$T/line"); p=$(echo "$l" | grep -o "pid=[0-9]*" | cut -d= -f2); e=$(echo "$l" | grep -o "expected_end=[^ ]*" | cut -d= -f2)
o=$(echo "$l" | awk '{print $1" "$2" "$3}')
check "T3d flag line compatible with quiet-guard parsing" '[ "$p" = "$(cat "$T/ppid")" ] && [ "$o" = "A quietlock two_words" ] && [ $(( $(date -u -d "$e" +%s) - $(date -u +%s) )) -gt 200 ]'

# --- T4 21-minute hold refused without a token; 20 accepted.
# Mutations killed: removing the minutes > 20 check (21 runs); changing it to >= 20 (20 refused).
reset
"$Q" hold --owner A --minutes 21 -- touch "$T/ran4" 2>/dev/null; rc=$?
check "T4 21-minute hold refused without token (exit 77)" '[ $rc = 77 ] && [ ! -e "$T/ran4" ] && [ ! -e "$FLAG" ]'
check "T4 refusal logged to history" 'grep -q "REFUSED 21-minute hold for A" "$HIST"'
"$Q" hold --owner A --minutes 20 -- touch "$T/ran4b"; rc=$?
check "T4b 20-minute hold needs no token" '[ $rc = 0 ] && [ -e "$T/ran4b" ]'

# --- T5 refused with expired / wrong-owner / too-small token.
# Mutations killed: drop the expires_at test; drop the owner compare; drop the max_minutes compare.
reset; token A 30 "$PAST"
"$Q" hold --owner A --minutes 25 -- touch "$T/ran5a" 2>/dev/null; rc=$?
check "T5a expired token refused" '[ $rc = 77 ] && [ ! -e "$T/ran5a" ]'
token B 30 "$FUTURE"
"$Q" hold --owner A --minutes 25 -- touch "$T/ran5b" 2>/dev/null; rc=$?
check "T5b wrong-owner token refused" '[ $rc = 77 ] && [ ! -e "$T/ran5b" ]'
token A 24 "$FUTURE"
"$Q" hold --owner A --minutes 25 -- touch "$T/ran5c" 2>/dev/null; rc=$?
check "T5c too-small token refused" '[ $rc = 77 ] && [ ! -e "$T/ran5c" ]'

# --- T6 accepted with a valid token; the use is logged.
# Mutations killed: approval_ok always failing; removing the history() "USED" line.
token A 30 "$FUTURE"
"$Q" hold --owner A --minutes 25 -- touch "$T/ran6"; rc=$?
check "T6 valid token accepted" '[ $rc = 0 ] && [ -e "$T/ran6" ] && [ ! -e "$FLAG" ]'
check "T6 token use appended to history" 'grep -q "approval token USED for 25-minute hold by A" "$HIST"'

# --- T7 release-stale refuses when the pid is alive (even past expected_end).
# Mutation killed: flag_stale() ignoring pid_alive().
reset
echo "M18 gate16 desc start=$PAST expected_end=$PAST pid=$$" > "$FLAG"
"$Q" release-stale >/dev/null; rc=$?
check "T7 release-stale refuses live pid" '[ $rc = 3 ] && [ -e "$FLAG" ]'

# --- T8 release-stale refuses when pid dead but expected_end not passed.
# Mutation killed: flag_stale() ignoring expected_end.
D=$(dead_pid)
echo "M18 gate16 desc start=$PAST expected_end=$FUTURE pid=$D" > "$FLAG"
"$Q" release-stale >/dev/null; rc=$?
check "T8 release-stale refuses dead pid before end" '[ "$D" != 0 ] && [ $rc = 3 ] && [ -e "$FLAG" ]'

# --- T9 release-stale releases when pid dead AND end passed.
# Mutation killed: release-stale never unlinking / flag_stale always 0.
echo "M18 gate16 desc start=$PAST expected_end=$PAST pid=$D" > "$FLAG"
"$Q" release-stale >/dev/null; rc=$?
check "T9 release-stale releases dead + past" '[ $rc = 0 ] && [ ! -e "$FLAG" ] && grep -q "released stale flag: M18 gate16 desc" "$HIST"'

# --- T10 old-format pid line still parsed (no hold= token, pid= last).
# Mutation killed: parsing pid only at a fixed field, or only when hold= is present.
echo "M18 gate16 desc start=$PAST expected_end=$FUTURE pid=$$" > "$FLAG"
"$Q" check 2>"$T/err10"; rc=$?
check "T10 old-format live flag blocks check with parsed pid" '[ $rc = 75 ] && grep -q "pid=$$ alive=yes" "$T/err10" && grep -q "holder=.M18 gate16 desc." "$T/err10"'
echo "M18 gate16 desc start=$PAST expected_end=$PAST pid=$D" > "$FLAG"
"$Q" check 2>/dev/null; rc=$?
check "T10b old-format stale flag counts as clear for check" '[ $rc = 0 ]'

reset
echo "quietlock tests: $N checks, $FAILS failed"
[ "$FAILS" = 0 ] && echo "QUIETLOCK_TESTS_PASS" && exit 0
echo "QUIETLOCK_TESTS_FAIL"
exit 1
