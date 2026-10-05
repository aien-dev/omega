#!/bin/sh
# R15 energy-instrument preflight (tools/r15_machine_state.sh energy-preflight).
# Spec §7: if package energy cannot be read by the physical SPBM source, R15
# cannot PASS. The preflight must refuse EARLY with INSTRUMENT_UNAVAILABLE
# (exit 5) when the reader is absent or gives invalid samples, and pass (exit 0)
# when it gives a valid advancing counter. Fixtures are fake hwmon trees built
# in a temp dir; nothing touches the machine. The receipt writer must refuse a
# BLOCKED_INSTRUMENT run directory (exit 4) and write nothing.
cd "$(dirname "$0")/.." || exit 1
T=tools/r15_machine_state.sh; fail=0
W=$(mktemp -d) || exit 1; trap 'rm -rf "$W"' EXIT
export R15_ENERGY_PREFLIGHT_SLEEP=0
check() { # name expected-exit expected-text
    out=$(R15_HWMON_ROOT="$W/$1" "$T" energy-preflight 2>&1); rc=$?
    if [ "$rc" = "$2" ] && printf '%s\n' "$out" | grep -Fq -- "$3"; then echo "[+] $1: exit $rc"
    else echo "[-] $1: exit $rc (want $2), output: $out"; fail=$((fail + 1)); fi
}
mk() { # case name label first-value-file-content
    mkdir -p "$W/$1/hwmon0" "$W/$1/hwmon1"
    echo acpitz > "$W/$1/hwmon0/name"
    echo "$2" > "$W/$1/hwmon1/name"
    echo "$3" > "$W/$1/hwmon1/energy1_label"
}
# reader absent: only unrelated hwmon devices
mkdir -p "$W/absent/hwmon0"; echo acpitz > "$W/absent/hwmon0/name"
check absent 5 "INSTRUMENT_UNAVAILABLE reason=absent"
mkdir -p "$W/empty"
check empty 5 "INSTRUMENT_UNAVAILABLE reason=absent"
# reader present, invalid samples
mk zero aien_spbm pkg; echo 0 > "$W/zero/hwmon1/energy1_input"
check zero 5 "reason=invalid"
mk unreadable aien_spbm pkg      # energy1_input missing (kernel returned an error)
check unreadable 5 "reason=invalid"
mk garbage aien_spbm pkg; echo "-22" > "$W/garbage/hwmon1/energy1_input"
check garbage 5 "reason=invalid"
mk stuck aien_spbm pkg; echo 5000000 > "$W/stuck/hwmon1/energy1_input"
check stuck 5 "counter did not advance"
mk wronglabel aien_spbm gpm; echo 5000000 > "$W/wronglabel/hwmon1/energy1_input"
check wronglabel 5 "energy1_label is not pkg"
mk overflow aien_spbm pkg; echo 5000000 > "$W/overflow/hwmon1/energy1_input"; echo 1 > "$W/overflow/hwmon1/energy1_overflow_raw"
check overflow 5 "overflow indicator"
# another energy source must not be accepted in place of the reader
mk substitute nvml pkg; echo 5000000 > "$W/substitute/hwmon1/energy1_input"
check substitute 5 "reason=absent"
# valid reader: a counter that advances between samples
mk valid aien_spbm pkg; echo 100000 > "$W/valid/hwmon1/energy1_input"; echo 0 > "$W/valid/hwmon1/energy1_overflow_raw"
# a static file cannot advance; emulate with a FIFO-free trick: a shell function is not
# possible across processes, so use sleep-based writer for the valid case
( sleep 0.05; echo 100500 > "$W/valid/hwmon1/energy1_input"; sleep 0.05; echo 101000 > "$W/valid/hwmon1/energy1_input" ) &
export R15_ENERGY_PREFLIGHT_SLEEP=0.08
check valid 0 "energy preflight ok"
wait
export R15_ENERGY_PREFLIGHT_SLEEP=0
# receipt writer: a blocked run is refused with its own exit code, no output written
R=$W/blocked; mkdir -p "$R"; printf 'BLOCKED_INSTRUMENT\nINSTRUMENT_UNAVAILABLE reason=absent\n' > "$R/instrument-unavailable.txt"
out=$(tools/r15_receipt.sh "$R" "$W/receipt-out" 2>&1); rc=$?
if [ "$rc" = 4 ] && printf '%s' "$out" | grep -Fq BLOCKED_INSTRUMENT && [ -z "$(ls "$W/receipt-out" 2>/dev/null)" ]; then
    echo "[+] receipt refuses blocked run: exit $rc, nothing written"
else echo "[-] receipt on blocked run: exit $rc (want 4): $out"; fail=$((fail + 1)); fi
echo "failures $fail"; [ "$fail" = 0 ]
