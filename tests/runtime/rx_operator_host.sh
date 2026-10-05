#!/bin/bash
# R16 G6 operator control by execution (docs/r16-operator-control.md §7).
#
#   rx_operator_host.sh <host|silicon> <production R13 binary> <rx_operator client>
#
# Starts the PRODUCTION R13 program (built without AIEN_TEST_BUILD) and drives
# it only from outside: the rx_operator client over the owner-only socket, and
# signals. Every check reads a reply, a file the world wrote in its state
# directory, the program's exit status or the program's crumb-log audit line.
# A case that cannot run here prints SKIPPED and the gate line is NOT_RUN,
# never a pass. Silicon: the program is never killed (no SIGKILL phase; on a
# failed check the world is resumed and the program is waited for).
#
# Environment: RX_OP_PHASES (default "startup control restart kill") runs a
# subset (mutants); RX_OP_FAILFAST=1 stops at the first failed check (host).
set -u
SEAT=${1:?host or silicon}; BIN=${2:?binary}; CLI=${3:?client}
case "$SEAT" in host|silicon) ;; *) echo "seat must be host or silicon" >&2; exit 2 ;; esac
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
CLI=$(cd "$(dirname "$CLI")" && pwd)/$(basename "$CLI")
PHASES=${RX_OP_PHASES:-"startup control restart kill"}
FAILFAST=${RX_OP_FAILFAST:-0}
W=$(mktemp -d "${TMPDIR:-/tmp}/rx-op.XXXXXX"); chmod 700 "$W"
if [ "$SEAT" = silicon ]; then WORLD_WAIT=900; EXIT_WAIT=3600; else WORLD_WAIT=120; EXIT_WAIT=300; fi
CHECKS=0; FAILS=0; SKIPS=0; PID=""
SUDO=0; if sudo -n true 2>/dev/null; then SUDO=1; fi

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        if [ "$SEAT" = host ]; then kill -9 "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
        else "$CLI" --control "$S/control" resume >/dev/null 2>&1; wait "$PID" 2>/dev/null; fi
    fi
    [ "$SUDO" = 1 ] && sudo -n rm -rf "$W" 2>/dev/null || rm -rf "$W"
}
trap cleanup EXIT

receipt() {
    local dir="$HERE/build/r16-operator" commit dirty sum
    mkdir -p "$dir"
    commit=$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo unknown)
    if [ -n "$(git -C "$HERE" status --porcelain 2>/dev/null)" ]; then dirty=true; else dirty=false; fi
    sum=$(sha256sum "$BIN" | cut -d' ' -f1)
    cat > "$dir/operator_${SEAT}_receipt.json" <<EOF
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
  "checks": $CHECKS,
  "failures": $FAILS,
  "skipped": $SKIPS,
  "gate": {"R16_G6_OPERATOR": "$1"}
}
EOF
    echo "R16 operator receipt: $dir/operator_${SEAT}_receipt.json"
}

finish() {
    local gate
    if [ "$FAILS" -gt 0 ]; then gate=FAIL
    elif [ "$SKIPS" -gt 0 ]; then gate="NOT_RUN ($SKIPS SKIPPED)"
    elif [ "$SEAT" = silicon ]; then gate=PASS
    else gate=HOST_PASS_NON_SILICON; fi
    receipt "$gate"
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
        if [ "$FAILFAST" = 1 ]; then finish; fi
    fi
}
skip() { echo "[SKIPPED] $1"; SKIPS=$((SKIPS + 1)); }

REPLY_=""
op() { REPLY_=$("$CLI" "$@" 2>&1); return $?; }       # op <client args>: sets REPLY_
kv() { printf '%s\n' "$REPLY_" | tr ' ' '\n' | sed -n "s/^$1=//p" | head -1; }
has() { printf '%s\n' "$REPLY_" | grep -Fq -- "$1"; }

start_prog() {   # start_prog <state dir> <log>
    "$BIN" --state-dir "$1" > "$2" 2>&1 < /dev/null &
    PID=$!
}
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
wait_line() {    # wait_line <log> <fixed text>: until the program printed it (or exited)
    local i=0 lim=$((WORLD_WAIT * 20))
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
wait_exit() {
    local i=0 lim=$((EXIT_WAIT * 20)) late=0
    while alive && [ $i -lt $lim ]; do sleep 0.05; i=$((i + 1)); done
    if alive; then
        late=1; echo "program still running after ${EXIT_WAIT}s"
        if [ "$SEAT" = host ]; then kill -9 "$PID" 2>/dev/null
        else "$CLI" --control "$S/control" resume >/dev/null 2>&1; fi
    fi
    wait "$PID"; RC=$?; PID=""
    [ $late = 0 ] || RC=124
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
        skip "startup: foreign-owned state directory (needs sudo -n to chown)"
    fi
fi

# ---- control: one program start driven through four of its worlds --------------------
S="$W/state-a"; mkdir -m 700 "$S"; LA="$W/run-a.log"
if [[ " $PHASES " == *" control "* ]] || [[ " $PHASES " == *" restart "* ]]; then
    start_prog "$S" "$LA"
    check "control: the positive world opens the operator entry point" 'wait_world "$S" positive'
    C="$S/control"; cp "$C/operator.cred" "$W/cred-positive"
    forge "$W/cred-positive" "$W/forged" 's/^\(cred [0-9]* \)\(.\)\(.*\)$/\1\3\2/'   # rotated secret: right length, wrong bytes
    op --control "$C" status
    check "control: authorized status, world running, no stop yet" 'has "OK state=running" && [ "$(kv seq)" = 0 ]'
fi
if [[ " $PHASES " == *" control "* ]]; then
    # Unauthorized requests: each refused, nothing changes, no state revealed.
    op --cred "$W/forged" --control "$C" stop
    check "unauthorized: forged secret, stop REFUSED identity" 'has "REFUSED reason=identity" && ! has state='
    op --cred "$W/forged" --control "$C" status
    check "unauthorized: forged secret, status REFUSED identity (no state revealed)" 'has "REFUSED reason=identity" && ! has state='
    forge "$W/cred-positive" "$W/wrongcap" 's/^cap \([0-9]*\) /cap 1\1 /'
    op --cred "$W/wrongcap" --control "$C" stop
    check "unauthorized: another capability, stop REFUSED authority" 'has "REFUSED reason=authority"'
    forge "$W/cred-positive" "$W/wrongsubj" 's/^subject 70$/subject 61/'
    op --cred "$W/wrongsubj" --control "$C" stop
    check "unauthorized: a reaction subject (61) with the operator secret, REFUSED identity" 'has "REFUSED reason=identity"'
    op --control "$C" --raw "aien-operator v1 stop"
    check "unauthorized: request without credential fields is BAD_REQUEST" 'has "BAD_REQUEST"'
    op --control "$C" --raw "$(sed -n 's/^cred \([0-9]*\) \([0-9a-f]*\)$/aien-operator v1 explode subject=70 gen=\1 secret=\2 cap=0:0/p' "$W/cred-positive")"
    check "unauthorized: unknown command with a full credential is BAD_REQUEST" 'has "BAD_REQUEST"'
    if [ "$SUDO" = 1 ]; then
        REPLY_=$(sudo -n "$CLI" --control "$C" stop 2>&1)
        check "unauthorized: a peer of another uid (root) is REFUSED peer, even with the credential" 'has "REFUSED reason=peer"'
    else
        skip "unauthorized: peer of another uid (needs sudo -n)"
    fi
    op --control "$C" status
    check "unauthorized: after every refusal the world still runs, no stop taken, no mark" \
        'has "OK state=running" && [ "$(kv seq)" = 0 ] && [ ! -e "$S/gen-positive/OPERATOR_HALT" ]'

    # Authorized stop: the world freezes; the promotion cannot happen under it.
    op --control "$C" stop 7
    check "stop: authorized stop accepted, durable" 'has "OK state=stopped" && [ "$(kv seq)" = 1 ] && [ "$(kv durable)" = 1 ]'
    M="$S/gen-positive/OPERATOR_HALT"
    check "stop: sealed mark in the generation store directory" \
        'head -n 1 "$M" | grep -qx "aien-operator-halt v1" && grep -qx "reason 7" "$M" && grep -qx "subject 70" "$M" && sealed "$M"'
    op --control "$C" status; S1=$REPLY_
    sv1=$(kv served); pc1=$(kv production_commits); ag1=$(kv active_generation); pr1=$(kv promotion); if1=$(kv inforce)
    check "stop: status says stopped, the promotion has not happened yet" 'has "state=stopped" && [ "$pr1" = 0 ] && [ "$if1" = 0 ] && [ -n "$sv1" ]'
    sleep 2
    op --control "$C" status
    check "stop: two seconds later nothing was served, committed or promoted" \
        '[ "$(kv served)" = "$sv1" ] && [ "$(kv production_commits)" = "$pc1" ] && [ "$(kv active_generation)" = "$ag1" ] && [ "$(kv promotion)" = 0 ] && [ "$(kv inforce)" = 0 ]'
    op --cred "$W/forged" --control "$C" resume
    check "stop: a forged resume is REFUSED and the world stays stopped" 'has "REFUSED reason=identity"'
    op --cred "$W/wrongcap" --control "$C" resume
    check "stop: a resume with another capability is REFUSED" 'has "REFUSED reason=authority"'
    op --control "$C" status
    check "stop: still stopped after the refused resumes" 'has "state=stopped" && [ "$(kv served)" = "$sv1" ]'
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
    ok=0
    for i in $(seq 1 200); do op --control "$C" status; [ "$(kv served)" != "$sv1" ] && { ok=1; break; }; sleep 0.05; done
    check "resume: production serves again after the resume" '[ $ok = 1 ]'

    # In-flight work under production load: stop and resume while requests run.
    cycles=0
    for i in $(seq 1 12); do
        op --control "$C" stop 9 || break
        has "OK state=stopped" || break
        sleep 0.05
        op --control "$C" resume || break
        has "OK state=running" || break
        cycles=$((cycles + 1))
    done
    check "in-flight: at least 6 stop/resume cycles while the positive world runs ($cycles)" '[ $cycles -ge 6 ]'
    check "positive world: the promotion happens after the resume (generation advanced)" 'wait_line "$LA" "R13 positive: generation"'
    check "positive world: crumb log shows $((1 + cycles)) stops, as many resumes, nothing committed or published under a stop" \
        'audit_ok "$LA" positive $((1 + cycles)) $((1 + cycles)) 0'
    cancelled=$(sed -n 's/^R13 operator positive: .*cancelled \([0-9]*\);.*$/\1/p' "$LA")
    check "in-flight: activations caught computing by a stop were refused at commit (cancelled ${cancelled:-?})" '[ "${cancelled:-0}" -ge 1 ]'

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
    if [ "$SEAT" = host ]; then want="R13 gate: R13_LIVING_SYSTEM=HOST_PASS_NON_SILICON"; else want="R13 gate: R13_LIVING_SYSTEM="; fi
    check "recovery: the program completes every mode and passes its R13 checks (exit 0)" \
        '[ $RC = 0 ] && grep -q "R13 control C:" "$LB" && grep -Fq "$want" "$LB" && ! grep -q "R13_LIVING_SYSTEM=FAIL" "$LB"'
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
        check "crash restart: positive world reopens" 'wait_world "$S" positive'
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
