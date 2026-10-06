#!/bin/bash
# R16 G6 operator control by execution (docs/r16-operator-control.md §7).
#
#   rx_operator_host.sh <host|silicon> <production R13 binary> <rx_operator client>
#
# Starts the PRODUCTION R13 program (built without AIEN_TEST_BUILD) and drives
# it only from outside: the rx_operator client over the owner-only socket, and
# signals. Every check reads a reply, a file the world wrote in its state
# directory, the program's exit status or the program's crumb-log audit line.
# A case that cannot run here prints SKIPPED with its reason and the gate line
# is NOT_RUN, never a pass. Silicon: the program is never killed (no SIGKILL
# phase; on a failed check the world is resumed and the program is waited for).
#
# Host only, deterministic holds: the first program start of the control phase
# runs under gdb in non-stop mode (only the thread at a breakpoint pauses; the
# operator socket keeps serving). Three temporary breakpoints hold one point
# each until the test has sent its request, then the test resumes the thread:
#   1 rx_resident_accept      the seat acceptor holding a computed seat result
#                             (an activation in flight): the stop must cancel it
#   2 rx_world_verify_crumbs  the positive episode's work is done, its world is
#                             not yet torn down: a stop here must keep it up
#   3 rx_operator_open (2nd)  the next world's open, after the positive world's
#                             entry point closed: what close removed is visible
# Without gdb those checks are SKIPPED (gate NOT_RUN). Silicon runs without
# gdb: its in-flight count is reported, not checked.
#
# Environment: RX_OP_PHASES (default "startup control restart kill") runs a
# subset (mutants); RX_OP_FAILFAST=1 stops at the first failed check (host);
# RX_OP_OUT (default build/r16-operator) receives the receipt and the logs of
# a failed run.
set -u
SEAT=${1:?host or silicon}; BIN=${2:?binary}; CLI=${3:?client}
case "$SEAT" in host|silicon) ;; *) echo "seat must be host or silicon" >&2; exit 2 ;; esac
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
CLI=$(cd "$(dirname "$CLI")" && pwd)/$(basename "$CLI")
PHASES=${RX_OP_PHASES:-"startup control restart kill"}
FAILFAST=${RX_OP_FAILFAST:-0}
OUT=${RX_OP_OUT:-$HERE/build/r16-operator}
W=$(mktemp -d "${TMPDIR:-/tmp}/rx-op.XXXXXX"); chmod 700 "$W"
if [ "$SEAT" = silicon ]; then WORLD_WAIT=900; EXIT_WAIT=3600; else WORLD_WAIT=120; EXIT_WAIT=300; fi
CHECKS=0; FAILS=0; SKIPS=0; SKIP_WHY=(); PID=""; GPID=""
SUDO=0; if sudo -n true 2>/dev/null; then SUDO=1; fi
HOLD=0; if [ "$SEAT" = host ] && command -v gdb >/dev/null 2>&1; then HOLD=1; fi

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        if [ "$SEAT" = host ]; then kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
        else "$CLI" --control "$S/control" resume >/dev/null 2>&1; wait "$PID" 2>/dev/null; fi
    fi
    if [ -n "$GPID" ]; then echo quit 2>/dev/null >&7; sleep 0.2; kill -9 "$GPID" 2>/dev/null; wait "$GPID" 2>/dev/null; fi
    [ "$SUDO" = 1 ] && sudo -n rm -rf "$W" 2>/dev/null || rm -rf "$W"
}
trap cleanup EXIT

receipt() {
    local commit dirty sum why="" r
    mkdir -p "$OUT"
    commit=$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo unknown)
    if [ -n "$(git -C "$HERE" status --porcelain 2>/dev/null)" ]; then dirty=true; else dirty=false; fi
    sum=$(sha256sum "$BIN" | cut -d' ' -f1)
    for r in "${SKIP_WHY[@]+"${SKIP_WHY[@]}"}"; do why="$why${why:+, }\"$r\""; done
    cat > "$OUT/operator_${SEAT}_receipt.json" <<EOF
{
  "schema": "AIEN_R16_G6_OPERATOR_CONTROL_V1",
  "seat": "$SEAT",
  "seat_description": "$([ "$SEAT" = silicon ] && echo "production GB10 program, resident Blackwell seat" || echo "production host program, R12 processor stand-in (not silicon)")",
  "candidate_commit": "$commit",
  "candidate_env": "${OMEGA_CANDIDATE_COMMIT:-}",
  "tree_dirty": $dirty,
  "binary": "$(basename "$BIN")",
  "binary_sha256": "$sum",
  "phases": "$PHASES",
  "held_by_gdb": $([ "$HOLD" = 1 ] && echo true || echo false),
  "checks": $CHECKS,
  "failures": $FAILS,
  "skipped": $SKIPS,
  "skip_reasons": [$why],
  "gate": {"R16_G6_OPERATOR": "$1"}
}
EOF
    echo "R16 operator receipt: $OUT/operator_${SEAT}_receipt.json"
}

finish() {
    local gate
    if [ "$FAILS" -gt 0 ]; then gate=FAIL
    elif [ "$SKIPS" -gt 0 ]; then gate="NOT_RUN ($SKIPS SKIPPED)"
    elif [ "$SEAT" = silicon ]; then gate=PASS
    else gate=HOST_PASS_NON_SILICON; fi
    receipt "$gate"
    if [ "$FAILS" -gt 0 ]; then   # keep the program logs of a failed run for diagnosis
        local keep="$OUT/failed-$SEAT-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$keep" && cp "$W"/*.log "$keep"/ 2>/dev/null
        echo "R16 operator: program logs kept in $keep"
    fi
    echo "R16 operator: $CHECKS checks, $FAILS failures, $SKIPS skipped ($SEAT, phases: $PHASES)"
    echo "R16 operator gate: R16_G6_OPERATOR=$gate"
    [ "$FAILS" = 0 ]
    exit $?
}

check() {   # check <label> <shell condition>
    CHECKS=$((CHECKS + 1))
    if eval "$2"; then echo "[+] $1"
    else
        echo "[-] $1"; FAILS=$((FAILS + 1))
        echo "    last reply (client exit $OPRC): $(printf '%s' "$REPLY_" | head -c 300)"
        if [ "$FAILFAST" = 1 ]; then finish; fi
    fi
}
skip() { echo "[SKIPPED] $1"; SKIPS=$((SKIPS + 1)); SKIP_WHY+=("$1"); }

REPLY_=""; OPRC=0
op() { REPLY_=$("$CLI" "$@" 2>&1); OPRC=$?; return $OPRC; }   # op <client args>: sets REPLY_, OPRC
kv() { printf '%s\n' "$REPLY_" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
has() { printf '%s\n' "$REPLY_" | grep -Fq -- "$1"; }

start_prog() {   # start_prog <state dir> <log>
    "$BIN" --state-dir "$1" > "$2" 2>&1 < /dev/null &
    PID=$!
}
# start_held <state dir> <log>: the program under gdb (host), PID = the program itself.
start_held() {
    local g="$W/hold.gdb" f="$W/hold.fifo" i=0
    cat > "$g" <<'EOF'
set debuginfod enabled off
set pagination off
set confirm off
set non-stop on
set startup-with-shell off
set print thread-events off
set print inferior-events on
handle SIGPIPE nostop noprint pass
tbreak rx_resident_accept
tbreak rx_world_verify_crumbs
tbreak rx_operator_open
ignore 3 1
run
EOF
    mkfifo -m 600 "$f" && exec 7<>"$f"
    gdb -nx -q -x "$g" --args "$BIN" --state-dir "$1" <&7 > "$2" 2>&1 &
    GPID=$!; PID=""
    while [ $i -lt $((WORLD_WAIT * 20)) ]; do
        PID=$(sed -n 's/^R13 operator: state .* pid \([0-9][0-9]*\))$/\1/p' "$2" | head -1)
        [ -n "$PID" ] && return 0
        kill -0 "$GPID" 2>/dev/null || return 1
        sleep 0.05; i=$((i + 1))
    done
    return 1
}
held() {         # held <n>: the program's thread is paused at temporary breakpoint n
    local i=0
    while [ $i -lt $((WORLD_WAIT * 20)) ]; do
        grep -Eq "hit Temporary breakpoint $1, " "$LA" && return 0
        alive || return 1
        sleep 0.05; i=$((i + 1))
    done
    return 1
}
release() { echo "continue -a" >&7; }   # resume every paused thread
alive() { [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; }
# wait_world <state> <mode-key>: this program start's credential file for that world is
# there (a file left by a killed earlier start names another pid) and its socket exists.
wait_world() {
    local i=0 lim=$((WORLD_WAIT * 50))
    while [ $i -lt $lim ]; do
        if grep -q "^world $2 $PID " "$1/control/operator.cred" 2>/dev/null && [ -S "$1/control/operator.sock" ]; then
            return 0
        fi
        alive || return 1
        sleep 0.02; i=$((i + 1))
    done
    return 1
}
wait_line() {    # wait_line <log> <fixed text> [seconds]: until the program printed it (or exited)
    local i=0 lim=$((${3:-$WORLD_WAIT} * 20))
    while [ $i -lt $lim ]; do
        grep -Fq -- "$2" "$1" && return 0
        alive || { grep -Fq -- "$2" "$1"; return $?; }
        sleep 0.05; i=$((i + 1))
    done
    return 1
}
# wait_exit: the program ends by itself. A program still running after EXIT_WAIT
# seconds (a world left stopped by a failed check) is a failure: on host it is
# killed, on silicon it is resumed and waited for (a chip program is never killed).
# Under gdb the exit status is the one gdb reports for the program.
wait_exit() {
    local i=0 lim=$((EXIT_WAIT * 20)) late=0 code
    while alive && [ $i -lt $lim ]; do sleep 0.05; i=$((i + 1)); done
    if alive; then
        late=1; echo "program still running after ${EXIT_WAIT}s"
        if [ "$SEAT" = host ]; then kill -9 "$PID" 2>/dev/null
        else "$CLI" --control "$S/control" resume >/dev/null 2>&1; fi
    fi
    if [ -n "$GPID" ]; then
        i=0; while [ $i -lt 200 ] && ! grep -Eq '^\[Inferior 1 \(process [0-9]+\) exited|terminated with signal' "$LA"; do sleep 0.05; i=$((i + 1)); done
        if grep -Eq '^\[Inferior 1 \(process [0-9]+\) exited normally\]' "$LA"; then RC=0
        else
            code=$(sed -n 's/^\[Inferior 1 (process [0-9]*) exited with code \([0-7]*\)\]$/\1/p' "$LA" | head -1)
            if [ -n "$code" ]; then RC=$((8#$code)); else RC=137; fi
        fi
        echo quit >&7; wait "$GPID" 2>/dev/null; exec 7>&-; GPID=""
    else
        wait "$PID"; RC=$?
    fi
    PID=""
    [ $late = 0 ] || RC=124
}
# dup_run <state dir> <log>: start a second program and wait up to 20 s for it to exit;
# sets rc (124 if it was still running: the lock did not refuse it). Host: it is then
# killed. Silicon: it is stopped and shut down through its own entry point (a chip
# program is never killed).
dup_run() {
    "$BIN" --state-dir "$1" > "$2" 2>&1 < /dev/null &
    local d=$! i=0
    while kill -0 "$d" 2>/dev/null && [ $i -lt 400 ]; do sleep 0.05; i=$((i + 1)); done
    if kill -0 "$d" 2>/dev/null; then
        echo "a second program on $1 is still running after 20 s"
        if [ "$SEAT" = host ]; then kill -9 "$d" 2>/dev/null
        else "$CLI" --control "$1/control" stop >/dev/null 2>&1; "$CLI" --control "$1/control" shutdown >/dev/null 2>&1; fi
        wait "$d" 2>/dev/null; rc=124; return
    fi
    wait "$d"; rc=$?
}
sealed() {       # sealed <file>: last line is "sha256 <64 hex>"
    [ -f "$1" ] && tail -n 1 "$1" | grep -Eq '^sha256 [0-9a-f]{64}$'
}
forge() {        # forge <cred in> <cred out> <sed expression>
    sed -e "$3" "$1" > "$2"; chmod 600 "$2"
}
audit_ok() {     # audit_ok <log> <mode-key> <stops> <resumes> <restored>
    grep -Eq "^R13 operator $2: stops $3 resumes $4 restored $5 open 0; under stop: commits 0 externals 0 cancelled [0-9]+; order ok$" "$1"
}
# raw <cred file> <command> [fields | -]: a request line with that credential and a
# deadline the client fills in (@deadline@), or the given fields, or no field (-).
raw() {
    local c=$1 cmd=$2; shift 2
    local f; f=$(sed -n 's/^cred \([0-9]*\) \([0-9a-f]*\)$/gen=\1 secret=\2/p' "$c")
    local k; k=$(sed -n 's/^cap \([0-9]*\) \([0-9]*\)$/cap=\1:\2/p' "$c")
    local s; s=$(sed -n 's/^subject \([0-9]*\)$/subject=\1/p' "$c")
    local x="${*:-deadline=@deadline@}"; [ "$x" = - ] && x=""
    echo "aien-operator v1 $cmd $s $f $k${x:+ $x}"
}

echo "R16 operator control ($SEAT): binary $BIN"

# ---- startup: the state directory is validated before any world starts -------------
if [[ " $PHASES " == *" startup "* ]]; then
    mkdir -m 755 "$W/open755"
    "$BIN" --state-dir "$W/open755" > "$W/s1.log" 2>&1; rc=$?
    check "startup: a group/other-readable state directory is refused (exit 2) before any world" \
        '[ $rc = 2 ] && grep -q "state directory refused" "$W/s1.log" && [ ! -e "$W/open755/control" ]'
    mkdir -m 700 "$W/real"; ln -s "$W/real" "$W/link"
    "$BIN" --state-dir "$W/link" > "$W/s2.log" 2>&1; rc=$?
    check "startup: a symlinked state directory is refused" '[ $rc = 2 ] && grep -q "is a symlink" "$W/s2.log"'
    mkdir -m 700 "$W/ctl"; mkdir -m 750 "$W/ctl/control"
    "$BIN" --state-dir "$W/ctl" > "$W/s3.log" 2>&1; rc=$?
    check "startup: a group-readable control directory is refused" '[ $rc = 2 ] && grep -q "control directory refused" "$W/s3.log"'
    if [ "$SUDO" = 1 ]; then
        mkdir -m 700 "$W/foreign"; sudo -n chown root "$W/foreign"
        "$BIN" --state-dir "$W/foreign" > "$W/s4.log" 2>&1; rc=$?
        check "startup: a state directory owned by another uid is refused" '[ $rc = 2 ] && grep -q "owned by uid 0" "$W/s4.log"'
    else
        skip "startup: foreign-owned state directory needs sudo -n to chown (no passwordless sudo here)"
    fi
fi

# ---- control: one program start driven through four of its worlds --------------------
S="$W/state-a"; mkdir -m 700 "$S"; LA="$W/run-a.log"; SEQ=0
if [[ " $PHASES " == *" control "* ]] || [[ " $PHASES " == *" restart "* ]]; then
    if [ "$HOLD" = 1 ] && [[ " $PHASES " == *" control "* ]]; then
        check "control: the production program starts under gdb (non-stop holds)" 'start_held "$S" "$LA"'
    else
        start_prog "$S" "$LA"
    fi
    check "control: the positive world opens the operator entry point" 'wait_world "$S" positive'
    C="$S/control"; cp "$C/operator.cred" "$W/cred-positive"
    forge "$W/cred-positive" "$W/forged" 's/^\(cred [0-9]* \)\(.\)\(.*\)$/\1\3\2/'   # rotated secret: right length, wrong bytes
    forge "$W/cred-positive" "$W/wrongcap" 's/^cap \([0-9]*\) /cap 1\1 /'
    forge "$W/cred-positive" "$W/wrongsubj" 's/^subject 70$/subject 61/'
fi
if [[ " $PHASES " == *" control "* ]]; then
    [ "$SEAT" = host ] && [ "$HOLD" = 0 ] && skip "control: the deterministic in-flight, end-of-episode and close holds need gdb (not installed)"
    # Authorized stop, sent the moment the world exists: the program answers it as soon
    # as setup is done, before the episode has done its work, so the promotion is still
    # ahead and cannot happen under the stop.
    op --control "$C" stop 7; SEQ=1
    check "stop: authorized stop accepted, durable, the first stop of this world (client exit 0)" \
        '[ $OPRC = 0 ] && has "OK state=stopped" && [ "$(kv seq)" = 1 ] && [ "$(kv durable)" = 1 ]'
    M="$S/gen-positive/OPERATOR_HALT"
    check "stop: sealed mark in the generation store directory" \
        'head -n 1 "$M" | grep -qx "aien-operator-halt v1" && grep -qx "reason 7" "$M" && grep -qx "subject 70" "$M" && sealed "$M"'
    op --control "$C" status
    sv1=$(kv served); pc1=$(kv production_commits); ag1=$(kv active_generation); pr1=$(kv promotion); if1=$(kv inforce)
    check "stop: status says stopped, the promotion has not happened yet" 'has "state=stopped" && [ "$pr1" = 0 ] && [ "$if1" = 0 ] && [ -n "$sv1" ]'

    # One program per state directory (S1): a second program on the same state, or on
    # this program's generation (halt) directory, is refused before it touches the
    # socket or the credential, and the first program stays controllable.
    ino1=$(stat -c %i "$C/operator.sock"); cr1=$(sha256sum < "$C/operator.cred")
    dup_run "$S" "$W/dup1.log"
    check "lock: a second program on the same state directory is refused (exit 2, names the holder)" \
        '[ $rc = 2 ] && grep -q "state directory refused: .* is in use by another program (pid $PID)" "$W/dup1.log"'
    dup_run "$S/gen-positive" "$W/dup2.log"
    check "lock: a second program on the first one's generation directory is refused" \
        '[ $rc = 2 ] && grep -q "in use by another program (pid $PID)" "$W/dup2.log" && [ ! -e "$S/gen-positive/control" ]'
    op --control "$C" status
    check "lock: the first program keeps its socket and credential and stays controllable" \
        'has "OK state=stopped" && [ "$(kv seq)" = 1 ] && [ "$(stat -c %i "$C/operator.sock")" = "$ino1" ] && [ "$(sha256sum < "$C/operator.cred")" = "$cr1" ]'

    # Unauthorized requests: each refused, nothing changes, no state revealed.
    op --cred "$W/forged" --control "$C" stop
    check "unauthorized: forged secret, stop REFUSED identity" 'has "REFUSED reason=identity" && ! has state='
    op --cred "$W/forged" --control "$C" status
    check "unauthorized: forged secret, status REFUSED identity (no state revealed)" 'has "REFUSED reason=identity" && ! has state='
    op --cred "$W/wrongcap" --control "$C" stop
    check "unauthorized: another capability, stop REFUSED authority" 'has "REFUSED reason=authority"'
    op --cred "$W/wrongsubj" --control "$C" stop
    check "unauthorized: a reaction subject (61) with the operator secret, REFUSED identity" 'has "REFUSED reason=identity"'
    op --control "$C" --raw "aien-operator v1 stop"
    check "unauthorized: request without credential fields is BAD_REQUEST" 'has "BAD_REQUEST"'
    # S3: the socket routes the operator capability only to stop, resume and the
    # authorized status/shutdown/revoke commands. Anything else, with a full valid
    # credential, is refused before any authority call.
    bad=""
    for c in explode bump-epoch bump_epoch mint grant epoch STOP Stop stop2 resume2 halt; do
        op --control "$C" --raw "$(raw "$W/cred-positive" "$c")"
        has "aien-operator v1 BAD_REQUEST" || bad="$bad $c"
    done
    check "unknown commands with a full credential are BAD_REQUEST (11 commands${bad:+, accepted:$bad})" '[ -z "$bad" ]'
    op --control "$C" --raw "$(raw "$W/cred-positive" resume reason=1 deadline=@deadline@)"
    check "unauthorized: a field the command does not take (resume reason=) is BAD_REQUEST" 'has "BAD_REQUEST"'
    if [ "$SUDO" = 1 ]; then
        REPLY_=$(sudo -n "$CLI" --control "$C" stop 2>&1)
        check "unauthorized: a peer of another uid (root) is REFUSED peer, even with the credential" 'has "REFUSED reason=peer"'
    else
        skip "unauthorized: a peer of another uid needs sudo -n (no passwordless sudo here)"
    fi
    op --cred "$W/forged" --control "$C" resume
    check "unauthorized: a forged resume is REFUSED" 'has "REFUSED reason=identity"'
    op --cred "$W/wrongcap" --control "$C" resume
    check "unauthorized: a resume with another capability is REFUSED" 'has "REFUSED reason=authority"'
    op --cred "$W/wrongsubj" --control "$C" resume
    check "unauthorized: a resume by a reaction subject is REFUSED" 'has "REFUSED reason=identity"'
    # S4: a request whose client already gave up is never run late (resume), and a
    # request without a deadline, or with one beyond the client's wait, is malformed.
    op --control "$C" --raw "$(raw "$C/operator.cred" resume deadline=1)"
    check "expired: a resume whose client deadline passed answers EXPIRED and does nothing" 'has "aien-operator v1 EXPIRED"'
    op --control "$C" --raw "$(raw "$C/operator.cred" resume deadline=18000000000000000000)"
    check "expired: a deadline beyond the client's wait is BAD_REQUEST" 'has "BAD_REQUEST"'
    op --control "$C" --raw "$(raw "$C/operator.cred" resume -)"
    check "expired: a resume without a deadline is BAD_REQUEST" 'has "BAD_REQUEST"'
    sleep 2
    op --control "$C" status
    check "stop: after every refusal and two seconds, still the same stop; nothing served, committed or promoted" \
        'has "state=stopped" && [ "$(kv seq)" = 1 ] && [ "$(kv served)" = "$sv1" ] && [ "$(kv production_commits)" = "$pc1" ] && [ "$(kv active_generation)" = "$ag1" ] && [ "$(kv promotion)" = 0 ] && [ "$(kv inforce)" = 0 ] && [ -e "$M" ]'
    op --control "$C" stop 8
    check "repeat: a second stop answers ALREADY, same stop" 'has "ALREADY state=stopped" && [ "$(kv seq)" = 1 ]'
    op --control "$C" resume
    check "resume: authorized resume accepted" 'has "OK state=running" && [ "$(kv seq)" = 1 ]'
    check "resume: the mark is gone and the sealed resumed record kept" \
        '[ ! -e "$M" ] && R=$(ls "$S"/gen-positive/OPERATOR_HALT.resumed.1.* 2>/dev/null | head -1) && [ -n "$R" ] && grep -qx "resumed_by 70" "$R" && grep -qx "reason 7" "$R" && sealed "$R"'
    op --control "$C" resume
    check "repeat: resume of a running world answers NOT_STOPPED" 'has "NOT_STOPPED"'
    op --control "$C" shutdown
    check "shutdown of a running world answers NOT_STOPPED (the program goes on)" 'has "NOT_STOPPED"'

    # S2: a stop that cannot be written to disk is in force in memory but would not
    # survive a restart. Its reply and the client's exit code say so.
    if [ "$(id -u)" != 0 ]; then
        chmod 500 "$S/gen-positive"
        op --control "$C" stop 12; SEQ=2
        chmod 700 "$S/gen-positive"
        check "not durable: a stop whose mark cannot be written answers STOPPED_NOT_DURABLE errno=13, client exit 3" \
            '[ $OPRC = 3 ] && has "STOPPED_NOT_DURABLE state=stopped" && [ "$(kv seq)" = 2 ] && [ "$(kv durable)" = -13 ] && [ "$(kv errno)" = 13 ] && [ ! -e "$M" ]'
        op --control "$C" status
        check "not durable: the stop is in force (status stopped, durable -13)" 'has "OK state=stopped" && [ "$(kv durable)" = -13 ]'
        op --control "$C" resume
        check "not durable: resume accepted" 'has "OK state=running" && [ "$(kv seq)" = 2 ]'
    else
        skip "not durable: an unwritable halt directory cannot be made as root"
    fi
    STOPS=$SEQ

    # In flight (G6), host: the acceptor thread is paused holding a computed seat
    # result, the stop is accepted, then the thread goes on: the result must be
    # refused at commit and the seat claim end halted, every run.
    if [ "$HOLD" = 1 ]; then
        check "in-flight: the seat acceptor is paused holding a computed seat result (breakpoint 1)" 'held 1'
        op --cred "$W/cred-positive" --control "$C" stop 9; SEQ=$((SEQ + 1)); STOPS=$SEQ
        check "in-flight: the stop is accepted while that activation is in flight" 'has "OK state=stopped" && [ "$(kv seq)" = $SEQ ]'
        release
        ok=0; i=0
        while [ $i -lt 200 ]; do   # waits for the released thread, never repeats the stop
            op --cred "$W/cred-positive" --control "$C" status
            [ "$(kv seat_halted)" -ge 1 ] 2>/dev/null && { ok=1; break; }; sleep 0.05; i=$((i + 1))
        done
        check "in-flight: the held result was refused at commit (cancelled $(kv cancelled), seat_halted $(kv seat_halted))" \
            '[ $ok = 1 ] && [ "$(kv cancelled)" -ge 1 ] && has "state=stopped"'
        op --cred "$W/cred-positive" --control "$C" resume
        check "in-flight: resume accepted" 'has "OK state=running"'
    fi
    ok=0
    for i in $(seq 1 200); do op --control "$C" status; [ "$(kv served)" != "$sv1" ] && { ok=1; break; }; sleep 0.05; done
    check "resume: production serves again after the resume" '[ $ok = 1 ]'
    op --cred "$W/forged" --control "$C" stop
    check "unauthorized: a forged stop of the running world is REFUSED identity" 'has "REFUSED reason=identity"'
    op --cred "$W/wrongcap" --control "$C" stop
    check "unauthorized: a stop of the running world with another capability is REFUSED authority" 'has "REFUSED reason=authority"'
    op --control "$C" status
    check "unauthorized: after the refused stops the world still runs, no new stop, no mark" \
        'has "OK state=running" && [ "$(kv seq)" = $SEQ ] && [ ! -e "$M" ]'

    # Under production load: 12 stop/resume cycles. Every request carries the positive
    # world's own credential, so a stop can never land in the next world. On host the
    # positive world cannot end during them (breakpoint 2 holds its end), so all 12 run.
    cycles=0
    for i in $(seq 1 12); do
        op --cred "$W/cred-positive" --control "$C" stop 9 || break
        has "OK state=stopped" || break
        sleep 0.05
        op --cred "$W/cred-positive" --control "$C" resume || break
        has "OK state=running" || break
        cycles=$((cycles + 1))
    done
    STOPS=$((STOPS + cycles))
    if [ "$HOLD" = 1 ]; then
        check "load: 12 of 12 stop/resume cycles while the positive world runs ($cycles)" '[ $cycles = 12 ]'
    else
        check "load: at least 6 of 12 stop/resume cycles while the positive world runs ($cycles)" '[ $cycles -ge 6 ]'
    fi

    if [ "$HOLD" = 1 ]; then
        # End of episode (G5 M-a): the positive episode's work is done and its world is
        # paused before teardown. A stop there must keep the world up, stopped, until
        # the operator resumes it.
        check "end of episode: the positive world's work is done, the world is paused before teardown (breakpoint 2)" 'held 2'
        op --cred "$W/cred-positive" --control "$C" stop 10; STOPS=$((STOPS + 1))
        check "end of episode: the stop is accepted" 'has "OK state=stopped"'
        release
        check "end of episode: the program says it waits for an operator resume" \
            'wait_line "$LA" "R13 operator positive: stopped after its episode; waiting for an operator resume" 10'
        sleep 2
        op --cred "$W/cred-positive" --control "$C" status
        check "end of episode: two seconds later the positive world is still up and stopped, not audited, not ended" \
            'has "OK state=stopped" && has "world=positive" && ! grep -q "^R13 operator positive: stops" "$LA"'
        # Close removes only what this instance made: a credential file another program
        # put in its place must still be there after the positive world closed.
        printf 'aien-operator-credential v1\nforeign %s\n' "$$" > "$C/.plant"; chmod 600 "$C/.plant"
        mv -f "$C/.plant" "$C/operator.cred"
        op --cred "$W/cred-positive" --control "$C" resume
        check "end of episode: resume accepted" 'has "OK state=running"'
        check "close: the next world's open is paused after the positive world closed (breakpoint 3)" 'held 3'
        check "close: the positive world's close left the credential file it did not create" \
            'grep -qx "foreign $$" "$C/operator.cred"'
        release
    fi
    check "positive world: the promotion happens after the resume (generation advanced)" 'wait_line "$LA" "R13 positive: generation"'
    check "positive world: crumb log shows $STOPS stops, as many resumes, nothing committed or published under a stop" \
        'audit_ok "$LA" positive $STOPS $STOPS 0'
    cancelled=$(sed -n 's/^R13 operator positive: .*cancelled \([0-9]*\);.*$/\1/p' "$LA")
    if [ "$HOLD" = 1 ]; then
        check "in-flight: the crumb log counts the cancelled activations (${cancelled:-?})" '[ "${cancelled:-0}" -ge 1 ]'
    else
        echo "in-flight ($SEAT, informational): activations cancelled by a stop: ${cancelled:-?}"
    fi

    # Next world (A no AIEN): stale credential, then the capability revoked by the authority.
    check "rotation: the next world opens with a new credential" 'wait_world "$S" no-aien'
    check "rotation: the new credential differs from the earlier world's" '! cmp -s "$W/cred-positive" "$C/operator.cred"'
    op --cred "$W/cred-positive" --control "$C" status
    check "stale: the earlier world's credential is REFUSED identity" 'has "REFUSED reason=identity"'
    op --control "$C" revoke-cap
    check "revoke-cap: the authority revokes the control capability" 'has "OK revoked=capability"'
    op --control "$C" stop
    check "revoke-cap: a stop with the revoked capability is REFUSED authority" 'has "REFUSED reason=authority"'
    op --control "$C" status
    check "revoke-cap: status with it is REFUSED authority" 'has "REFUSED reason=authority"'
    check "revoke-cap: the world runs on without operator control (control A passes, no stop)" \
        'wait_line "$LA" "R13 control A:" && audit_ok "$LA" no-aien 0 0 0 && [ ! -e "$S/gen-no-aien/OPERATOR_HALT" ]'

    # Next world (B no promotion authority): the credential revoked.
    check "revoke: world B opens" 'wait_world "$S" no-promotion'
    cp "$C/operator.cred" "$W/cred-b"
    op --control "$C" revoke
    check "revoke: capability and credential revoked, credential file removed" 'has "OK revoked=capability,credential" && [ ! -e "$C/operator.cred" ]'
    op --cred "$W/cred-b" --control "$C" stop
    check "revoke: a stop with the revoked credential is REFUSED identity" 'has "REFUSED reason=identity"'
    check "revoke: world B completes without a stop" 'wait_line "$LA" "R13 control B:" && audit_ok "$LA" no-promotion 0 0 0'
fi
if [[ " $PHASES " == *" control "* ]] || [[ " $PHASES " == *" restart "* ]]; then
    # World C: stop, then shut the program down while stopped.
    check "shutdown: world C opens" 'wait_world "$S" revoked-experiment'
    cp "$C/operator.cred" "$W/cred-c"
    op --control "$C" stop 11
    check "shutdown: stop world C" 'has "OK state=stopped"'
    op --cred "$W/forged" --control "$C" shutdown
    check "shutdown: a forged shutdown is REFUSED" 'has "REFUSED reason=identity"'
    op --control "$C" shutdown
    check "shutdown: authorized shutdown of the stopped world" 'has "OK state=stopped shutdown=1"'
    wait_exit
    check "shutdown: the program exits 3, writes no receipt, the stop stays on disk" \
        '[ $RC = 3 ] && grep -q "shutdown while stopped in revoked-experiment" "$LA" && ! grep -q "R13 gate:" "$LA" && sealed "$S/gen-revoked-experiment/OPERATOR_HALT" && [ ! -e "$C/operator.sock" ]'
fi

# ---- restart while stopped (after the shutdown) -----------------------------------------
if [[ " $PHASES " == *" restart "* ]]; then
    LB="$W/run-b.log"
    start_prog "$S" "$LB"
    check "restart: world C reopens" 'wait_world "$S" revoked-experiment'
    op --control "$C" status
    check "restart: world C comes up STOPPED, restored, before any work (no reaction registered)" \
        'has "OK state=stopped" && [ "$(kv restored)" = 1 ] && [ "$(kv reactions)" = 0 ] && has "setup=pending"'
    k1=$(kv crumbs)
    check "restart: the program says the stop was restored" 'grep -q "^R13 operator revoked-experiment: restored STOPPED" "$LB"'
    op --cred "$W/cred-c" --control "$C" status
    check "stale: the credential of the earlier program start is REFUSED identity" 'has "REFUSED reason=identity"'
    sleep 1
    op --control "$C" status
    check "restart: one second later still stopped, nothing built, no crumb added" \
        'has "state=stopped" && [ "$(kv reactions)" = 0 ] && [ "$(kv crumbs)" = "$k1" ]'
    op --control "$C" resume
    check "recovery: resume of the restored stop" 'has "OK state=running"'
    check "recovery: the mark is gone, the resumed record kept" \
        '[ ! -e "$S/gen-revoked-experiment/OPERATOR_HALT" ] && ls "$S"/gen-revoked-experiment/OPERATOR_HALT.resumed.* >/dev/null 2>&1'
    wait_exit
    # Host: the stand-in is never silicon. Silicon: only a full PASS (bound to the
    # candidate commit, clean tree, silicon observed) counts; the spec allows no other.
    if [ "$SEAT" = host ]; then want="R13 gate: R13_LIVING_SYSTEM=HOST_PASS_NON_SILICON"; else want="R13 gate: R13_LIVING_SYSTEM=PASS"; fi
    check "recovery: the program completes every mode and its R13 gate is ${want#R13 gate: } (exit 0)" \
        '[ $RC = 0 ] && grep -q "R13 control C:" "$LB" && grep -Eq "^${want}( |$)" "$LB"'
    check "recovery: crumb log of world C shows the restored stop and its resume, nothing under it" \
        'audit_ok "$LB" revoked-experiment 1 1 1'
fi

# ---- crash while stopped (host only: a chip program is never killed) ------------------
if [[ " $PHASES " == *" kill "* ]]; then
    if [ "$SEAT" = host ]; then
        S="$W/state-k"; mkdir -m 700 "$S"; C="$S/control"; LC="$W/run-c.log"; LD="$W/run-d.log"
        start_prog "$S" "$LC"
        check "crash: positive world opens" 'wait_world "$S" positive'
        op --control "$C" stop 13
        check "crash: stop accepted" 'has "OK state=stopped"'
        kill -9 "$PID"; wait "$PID" 2>/dev/null; PID=""
        check "crash: after SIGKILL the sealed mark is on disk" 'sealed "$S/gen-positive/OPERATOR_HALT" && grep -qx "reason 13" "$S/gen-positive/OPERATOR_HALT"'
        start_prog "$S" "$LD"
        check "crash restart: positive world reopens (the dead program's lock is gone with it)" 'wait_world "$S" positive'
        op --control "$C" status
        check "crash restart: comes up STOPPED before any work" 'has "OK state=stopped" && [ "$(kv restored)" = 1 ] && [ "$(kv reactions)" = 0 ]'
        op --control "$C" resume
        check "crash recovery: resume accepted" 'has "OK state=running"'
        wait_exit
        check "crash recovery: positive promotes after the restart, every mode passes (exit 0)" \
            '[ $RC = 0 ] && grep -q "R13 positive: generation" "$LD" && grep -Fq "R13 gate: R13_LIVING_SYSTEM=HOST_PASS_NON_SILICON" "$LD" && audit_ok "$LD" positive 1 1 1'
    else
        echo "kill phase: host only by design (a chip program is never killed); restart on silicon is proven by the shutdown phase"
    fi
fi

finish
