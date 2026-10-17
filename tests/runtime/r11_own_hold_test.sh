#!/bin/sh
# R11 living run inside its own quiet hold (tests/runtime/rx_r11_aien.c
# quiet_flag_is_mine). With a fake HOME whose ~/workspace/.spark-quiet carries
# hold=q1-abc: no QUIETLOCK_HOLD, a prefix and an extension of the id must all
# refuse the living run because of the flag (someone else's measurement);
# QUIETLOCK_HOLD=q1-abc must run it.
#
# The binary also refuses the living run when the machine is loaded (1-minute
# load average above 2, or an R15 program alive): "SKIPPED-LOADED: <why>".
# Only the flag reason counts as the refusal this test is about. When the
# q1-abc case is refused for load, the test waits for the machine to quiet down
# and runs it again, for at most R11_QUIET_WAIT seconds (default 1800). If the
# machine never quiets, the case stays a failure: a skipped living run is never
# a pass. usage: r11_own_hold_test.sh <rx_r11_aien_test>
B=${1:?rx_r11_aien_test binary}; H=$(mktemp -d); fail=0
WAIT=${R11_QUIET_WAIT:-1800}
trap 'rm -rf "$H"' EXIT
mkdir -p "$H/workspace"
echo "other quietlock X start=2026-10-05T00:00:00Z expected_end=2099-01-01T00:00:00Z pid=1 hold=q1-abc" > "$H/workspace/.spark-quiet"
FLAG_WHY='holds ~/workspace/.spark-quiet'

load_ok() { awk '{ exit !($1 <= 2.0) }' /proc/loadavg; }

for hold in "" q1-ab q1-abcd q1-abc; do
    if [ "$hold" = q1-abc ]; then want=ran; else want=refused; fi
    waited=0
    while :; do
        out=$(HOME=$H QUIETLOCK_HOLD=$hold "$B" 2>&1)
        if ! printf '%s\n' "$out" | grep -Fq "living run not exercised"; then got=ran
        elif printf '%s\n' "$out" | grep -Fq "$FLAG_WHY"; then got=refused
        else got=loaded; fi
        [ "$got" = loaded ] && [ "$want" = ran ] && [ "$waited" -lt "$WAIT" ] || break
        why=$(printf '%s\n' "$out" | sed -n 's/^\[-\] SKIPPED-LOADED: living run not started: //p' | head -n 1)
        echo "[~] hold='$hold': living run refused for load ($why), waiting for a quiet machine (${waited}s of ${WAIT}s)"
        while [ "$waited" -lt "$WAIT" ]; do
            sleep 15; waited=$((waited + 15))
            load_ok && break
        done
    done
    printf '%s\n' "$out" | grep -Eq '^checks [0-9]+ failures 0$' || { got="$got+failures"; }
    if [ "$got" = "$want" ]; then echo "[+] hold='$hold': $got"; else echo "[-] hold='$hold': $got, want $want"; fail=$((fail + 1)); fi
    [ "$got" = loaded ] && printf '%s\n' "$out" | grep -F "SKIPPED-LOADED" | head -n 1
done
echo "failures $fail"; [ "$fail" = 0 ]
