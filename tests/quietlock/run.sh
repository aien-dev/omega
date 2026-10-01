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

# --- T2f refusals carry the fixed marker the forge keys on.
# Mutation killed: dropping "QUIETLOCK_REFUSED" from the cmd_check message.
live_flag
"$Q" check 2>"$T/err2f"; rc=$?
check "T2f refusal prints QUIETLOCK_REFUSED and exits 75" '[ $rc = 75 ] && grep -q QUIETLOCK_REFUSED "$T/err2f"'

# --- T11 no state dir (GitHub CI, fresh machine) is clear, never an error.
# Mutation killed: removing the state_dir_missing() branch in cmd_check (lock_take dies, exit 1).
out=$(QUIETLOCK_DIR=$T/nonexistent $MK quietlock-check 2>&1); rc=$?
check "T11 make passes with no state dir" '[ $rc = 0 ] && echo "$out" | grep -q "clear for this make run" && [ ! -e "$T/nonexistent" ]'
QUIETLOCK_DIR=$T/nonexistent "$Q" release-stale >/dev/null; rc=$?
check "T11b release-stale with no state dir says no flag" '[ $rc = 0 ] && [ ! -e "$T/nonexistent" ]'

# --- T11c/d tool cannot be built: continue with NOTICE when no flag, refuse when a flag exists.
# Mutations killed: always continuing on build failure (T11d); always refusing (T11c).
reset
out=$(make -s -C "$HERE" OUT_DIR="$T/build2" CC=false quietlock-check 2>&1); rc=$?
check "T11c unbuildable tool + no flag: NOTICE and continue" '[ $rc = 0 ] && echo "$out" | grep -q NOTICE'
live_flag
out=$(make -s -C "$HERE" OUT_DIR="$T/build2" CC=false quietlock-check 2>&1); rc=$?
check "T11d unbuildable tool + flag: refused" '[ $rc != 0 ] && echo "$out" | grep -q QUIETLOCK_REFUSED'

# --- T11e on-demand build writes a temp file and renames it (no half-written binary).
# Mutation killed: compiling straight to $(QUIETLOCK_BIN) in mk/quiet.mk.
reset
printf '#!/bin/sh\necho "$@" >> "%s"\nexec %s "$@"\n' "$T/cc.log" "${CC:-gcc}" > "$T/ccspy"; chmod +x "$T/ccspy"
out=$(make -s -C "$HERE" OUT_DIR="$T/build3" CC="$T/ccspy" quietlock-check 2>&1); rc=$?
check "T11e on-demand build goes through a temp name" '[ $rc = 0 ] && grep -q -- "-o $T/build3/quietlock.tmp.[0-9]" "$T/cc.log" && [ -x "$T/build3/quietlock" ] && ! ls "$T/build3" | grep -q tmp'

# --- T12 legacy QUIET_HOLDER=1 passes only a legacy flag (no hold=), logged as deprecated.
# Mutations killed: removing the legacy branch (T12 fails); dropping its `!f.hold[0]` guard (T12b fails).
echo "M18 gate16 desc start=$PAST expected_end=$FUTURE pid=$$" > "$FLAG"
QUIET_HOLDER=1 "$Q" check 2>/dev/null; rc=$?
check "T12 QUIET_HOLDER=1 passes a legacy flag, logged" '[ $rc = 0 ] && grep -q "DEPRECATED QUIET_HOLDER=1" "$HIST"'
live_flag
QUIET_HOLDER=1 "$Q" check 2>/dev/null; rc=$?
check "T12b QUIET_HOLDER=1 does not pass a quietlock hold" '[ $rc = 75 ]'

# --- T13 overrun: a 1-"minute" hold around a longer command stops blocking others at
# expected_end, logs the overrun, and does NOT kill the command.
# (QUIETLOCK_TEST_MINUTE_SECONDS=2 shortens a minute to 2 s; honoured only with QUIETLOCK_TEST=1.)
# Mutations killed: removing the alarm/overrun release (check stays 75); killing the child at overrun.
reset
QUIETLOCK_TEST=1 QUIETLOCK_TEST_MINUTE_SECONDS=2 "$Q" hold --owner A --minutes 1 -- \
	sh -c 'touch "$1/started"; while [ ! -e "$1/go" ]; do sleep 1; done; touch "$1/done"' sh "$T" 2>/dev/null &
hp=$!
i=0; while [ $i -lt 20 ] && ! grep -q "overrun: hold expired while command still running" "$HIST" 2>/dev/null; do sleep 1; i=$((i + 1)); done
"$Q" check 2>/dev/null; crc=$?
check "T13 after expected_end others are not blocked, command still running" \
	'[ $crc = 0 ] && [ ! -e "$FLAG" ] && [ -e "$T/started" ] && [ ! -e "$T/done" ] && kill -0 $hp 2>/dev/null'
touch "$T/go"; wait $hp; rc=$?
check "T13b overrun command finished normally and was logged" '[ $rc = 0 ] && [ -e "$T/done" ] && grep -q "finished after overrun (exit 0)" "$HIST"'
rm -f "$T/started" "$T/go" "$T/done"

# --- T14 expected_end without Z never counts as passed.
# Mutation killed: making the trailing Z optional in parse_iso.
D2=$(dead_pid)
echo "M18 gate16 desc start=$PAST expected_end=2000-01-01T00:00:00 pid=$D2" > "$FLAG"
"$Q" release-stale >/dev/null; rc=$?
check "T14 no-Z expected_end is never stale" '[ "$D2" != 0 ] && [ $rc = 3 ] && [ -e "$FLAG" ]'

# --- T15 malformed flag (no pid=, no expected_end=) blocks and is never stale.
# Mutation killed: treating a missing pid as dead, or a missing end as passed.
echo "qemu_ck_store_test 3033916" > "$FLAG"
"$Q" check 2>/dev/null; crc=$?
"$Q" release-stale >/dev/null; rc=$?
check "T15 malformed flag blocks and is not released" '[ $crc = 75 ] && [ $rc = 3 ] && [ -e "$FLAG" ]'

# --- T16 a hold never deletes a flag that is not its own.
# Mutation killed: release_mine() unlinking without comparing the hold id.
reset
"$Q" hold --owner A --minutes 1 -- sh -c 'rm -f "$1"; echo "FOREIGN x y start=$3 expected_end=$4 pid=$2 hold=qFOREIGN" > "$1"' sh "$FLAG" "$$" "$PAST" "$FUTURE"
check "T16 foreign flag left alone at hold exit" 'grep -q "hold=qFOREIGN" "$FLAG"'

# --- T17 SIGTERM to hold is forwarded to the command and the flag is released.
# Mutations killed: no signal handler (quietlock dies, flag stays); handler not forwarding (rc 0 after 30 s).
reset
"$Q" hold --owner A --minutes 5 -- sh -c 'touch "$1"; exec sleep 30' sh "$T/ready" 2>/dev/null &
hp=$!
i=0; while [ $i -lt 20 ] && [ ! -e "$T/ready" ]; do sleep 1; i=$((i + 1)); done
kill -TERM $hp; wait $hp; rc=$?
check "T17 SIGTERM forwarded, flag released" '[ $rc = 143 ] && [ ! -e "$FLAG" ]'

# ===== Proposed hook tools/quietlock/quiet-guard.sh (stdin = Claude Code hook JSON) =====
HOOK=$HERE/tools/quietlock/quiet-guard.sh
if ! command -v jq >/dev/null 2>&1; then
	check "H0 jq available for hook tests" 'false'
else
hookrun() { # $1 = JSON; extra env via caller
	printf '%s' "$1" | QUIETLOCK_BIN=$Q sh "$HOOK" 2>"$T/hookerr"
}
bash_json() { jq -n --arg c "$1" '{tool_name:"Bash",tool_input:{command:$c}}'; }
reset; live_flag

# H1 Edit/Write calls are never blocked, even with build words in the text.
# Mutation killed: scanning the whole tool_input JSON (old text matching) instead of Bash commands only.
hookrun "$(jq -n '{tool_name:"Edit",tool_input:{file_path:"notes.md",old_string:"a",new_string:"make test\ncargo build"}}')"; rc=$?
check "H1 Edit of an .md mentioning make test allowed" '[ $rc = 0 ]'
hookrun "$(jq -n '{tool_name:"Write",tool_input:{file_path:"x.md",content:"make -C x"}}')"; rc=$?
check "H1b Write mentioning make allowed" '[ $rc = 0 ]'

# H2 writing notes is never blocked.
# Mutations killed: no quote stripping (H2, H2d); matching words anywhere instead of command position (H2b);
# no heredoc stripping (H2c); per-line quote stripping (H2e).
hookrun "$(bash_json 'echo "remember: make test before merge" >> ~/handoffs/x.md')"; rc=$?
check "H2 echo \"... make ...\" >> handoff.md allowed" '[ $rc = 0 ]'
hookrun "$(bash_json 'echo then run make test >> notes.md')"; rc=$?
check "H2b unquoted make as an echo argument allowed" '[ $rc = 0 ]'
hookrun "$(bash_json "$(printf 'cat > notes.md <<%sEOF%s\nmake test\ncargo build\nEOF' "'" "'")")"; rc=$?
check "H2c heredoc into .md allowed" '[ $rc = 0 ]'
hookrun "$(bash_json "sed -i 's/make all/make test/' doc.md")"; rc=$?
check "H2d sed -i on .md allowed" '[ $rc = 0 ]'
hookrun "$(bash_json "$(printf 'echo "first line\nmake test\nlast" >> notes.md')")"; rc=$?
check "H2e multi-line quoted text allowed" '[ $rc = 0 ]'

# H3 real heavy commands are blocked while held.
# Mutations killed: hook always exiting 0 / not consulting quietlock (H3); no segment split (H3b);
# no -c extraction (H3c); no wrapper/assignment skipping (H3d); cargo subcommand rule removed (H3e).
hookrun "$(bash_json 'make -C x')"; rc=$?
check "H3 make -C x blocked while held" '[ $rc = 2 ] && grep -q QUIETLOCK_REFUSED "$T/hookerr"'
hookrun "$(bash_json 'cd x && make test')"; rc=$?
check "H3b cd x && make test blocked" '[ $rc = 2 ]'
hookrun "$(bash_json "bash -c 'make test'")"; rc=$?
check "H3c bash -c 'make test' blocked" '[ $rc = 2 ]'
hookrun "$(bash_json 'env FOO=1 nice -n 5 make all')"; rc=$?
check "H3d env/nice wrapped make blocked" '[ $rc = 2 ]'
hookrun "$(bash_json 'cargo build --release')"; rc=$?
check "H3e cargo build blocked" '[ $rc = 2 ]'

# H4 the holder passes (command prefix or hook env), a wrong id does not.
# Mutation killed: not handing the extracted/env QUIETLOCK_HOLD to quietlock check.
hookrun "$(bash_json 'QUIETLOCK_HOLD=qOTHER-1-0 make -C x')"; rc=$?
check "H4 holder's QUIETLOCK_HOLD prefix allowed" '[ $rc = 0 ]'
hookrun "$(bash_json 'QUIETLOCK_HOLD=qWRONG make -C x')"; rc=$?
check "H4b wrong QUIETLOCK_HOLD blocked" '[ $rc = 2 ]'
printf '%s' "$(bash_json 'make -C x')" | QUIETLOCK_HOLD=qOTHER-1-0 QUIETLOCK_BIN=$Q sh "$HOOK" 2>/dev/null; rc=$?
check "H4c holder's QUIETLOCK_HOLD in hook env allowed" '[ $rc = 0 ]'

# H5 control: clear flag lets heavy commands through.
# Mutation killed: blocking heavy commands without asking quietlock.
reset
hookrun "$(bash_json 'make -C x')"; rc=$?
check "H5 make allowed when clear" '[ $rc = 0 ]'

# H6 stale flag is released through quietlock release-stale, then allowed.
# Mutation killed: dropping the release-stale call (flag stays).
D3=$(dead_pid)
echo "M18 gate16 desc start=$PAST expected_end=$PAST pid=$D3" > "$FLAG"
hookrun "$(bash_json 'make -C x')"; rc=$?
check "H6 stale flag released by the hook via quietlock" '[ $rc = 0 ] && [ ! -e "$FLAG" ] && grep -q "released stale flag: M18 gate16 desc start=$PAST expected_end=$PAST pid=$D3" "$HIST"'

# H7 DEPRECATED transition escapes still work in the hook.
# Mutations killed: removing the QUIET_HOLDER=1 case (H7); removing the allow-line check (H7b); allow line matching everything (H7c).
echo "M18 gate16 desc start=$PAST expected_end=$FUTURE pid=$$" > "$FLAG"
hookrun "$(bash_json 'QUIET_HOLDER=1 make -C x')"; rc=$?
check "H7 QUIET_HOLDER=1 allowed, marked deprecated" '[ $rc = 0 ] && grep -q DEPRECATED "$T/hookerr"'
printf 'allow ^make -C special\n' >> "$FLAG"
hookrun "$(bash_json 'make -C special')"; rc=$?
check "H7b allow line lets a matching command through" '[ $rc = 0 ]'
hookrun "$(bash_json 'make -C other')"; rc=$?
check "H7c allow line does not let others through" '[ $rc = 2 ]'
fi

reset
echo "quietlock tests (host): $N checks, $FAILS failed"
[ "$FAILS" = 0 ] && echo "QUIETLOCK_TESTS_PASS host" && exit 0
echo "QUIETLOCK_TESTS_FAIL host"
exit 1
