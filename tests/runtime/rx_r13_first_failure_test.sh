#!/bin/bash
# Host test: the R13 receipt carries the FIRST failure message (omega #313 item 3).
# Builds the R13 host test build twice into its own scratch OUT_DIR (never build/): the normal program
# (receipt first_failure is null) and one compiled with an unreachable goal (-DTARGET_PCT=1u, so the goal
# check fails). The failing run must exit 1, write a FAIL receipt, and record the goal message in
# first_failure; the stderr line and the receipt agree. No GPU. Runs from a scratch folder so no receipt
# is written under the real build/qual-runs.
# Usage: tests/runtime/rx_r13_first_failure_test.sh   (environment as for `make test-r13-testbuild-host`)
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/rxr13ff.XXXXXX"); trap 'rm -rf "$T"' EXIT
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }
BASE_CFLAGS=$(make -C "$HERE" -pn 2>/dev/null | sed -n 's/^CFLAGS = //p' | head -1)
mk() { make -C "$HERE" OUT_DIR="$T/out" CFLAGS="$BASE_CFLAGS $1" "$T/out/rx_r13_living_testbuild_host" > "$T/make.log" 2>&1; }
runit() { # name; runs the built program in its own folder
    mkdir -p "$T/$1" && chmod 700 "$T/$1" && mkdir -m 700 "$T/$1/state"
    (cd "$T/$1" && "$T/out/rx_r13_living_testbuild_host" --state-dir "$T/$1/state" > stdout.log 2> stderr.log; echo $? > rc)
    receipt=$(find "$T/$1" -name rx_living_test_build_receipt.json | head -1)
}
mk "" || { echo "NOT_RUN: normal build failed (see environment: PHYSICS_DIR, ARGUS_REPO, AIENOS_LOCK_REPO)"; tail -3 "$T/make.log"; exit 3; }
runit pass
check "normal run: receipt written" '[ -s "$receipt" ]'
check "normal run: first_failure is null" '[ "$(jq -c .first_failure "$receipt")" = null ]'
rm -f "$T/out/rx_r13_living_testbuild_host"
mk "-DTARGET_PCT=1u" || { echo "FAIL variant build"; tail -3 "$T/make.log"; exit 1; }
runit fail
check "unreachable goal: exit 1" '[ "$(cat "$T/fail/rc")" = 1 ]'
check "unreachable goal: receipt written" '[ -s "$receipt" ]'
check "unreachable goal: gate is FAIL" '[ "$(jq -r ".gate | to_entries[0].value" "$receipt")" = FAIL ]'
check "first_failure names the goal check" 'jq -r .first_failure "$receipt" | grep -q "AIEN.s belief does not show the goal met"'
check "first_failure is the first stderr R13 failure line" '[ "$(jq -r .first_failure "$receipt")" = "$(grep -m1 "^R13 mode" "$T/fail/stderr.log")" ]'
check "nothing written under the real build/qual-runs by this test" '[ -z "$(find "$HERE/build/qual-runs" -newer "$T/make.log" 2>/dev/null | head -1)" ]'
echo "rx_r13_first_failure_test: $FAILS failures"
[ "$FAILS" = 0 ]
