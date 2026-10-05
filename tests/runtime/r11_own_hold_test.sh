#!/bin/sh
# R11 living run inside its own quiet hold (tests/runtime/rx_r11_aien.c
# quiet_flag_is_mine). With a fake HOME whose ~/workspace/.spark-quiet carries
# hold=q1-abc: no QUIETLOCK_HOLD, a prefix and an extension of the id must all
# refuse the living run (someone else's measurement); QUIETLOCK_HOLD=q1-abc
# must run it. usage: r11_own_hold_test.sh <rx_r11_aien_test>
B=${1:?rx_r11_aien_test binary}; H=$(mktemp -d); fail=0
trap 'rm -rf "$H"' EXIT
mkdir -p "$H/workspace"
echo "other quietlock X start=2026-10-05T00:00:00Z expected_end=2099-01-01T00:00:00Z pid=1 hold=q1-abc" > "$H/workspace/.spark-quiet"
for hold in "" q1-ab q1-abcd q1-abc; do
    out=$(HOME=$H QUIETLOCK_HOLD=$hold "$B" 2>&1)
    if printf '%s\n' "$out" | grep -Fq "living run not exercised"; then got=refused; else got=ran; fi
    if [ "$hold" = q1-abc ]; then want=ran; else want=refused; fi
    printf '%s\n' "$out" | grep -Eq '^checks [0-9]+ failures 0$' || { got="$got+failures"; }
    if [ "$got" = "$want" ]; then echo "[+] hold='$hold': $got"; else echo "[-] hold='$hold': $got, want $want"; fail=$((fail + 1)); fi
done
echo "failures $fail"; [ "$fail" = 0 ]
