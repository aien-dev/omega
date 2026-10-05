#!/bin/sh
# R15 energy-instrument preflight (tools/r15_machine_state.sh energy-preflight).
# Spec §7: if package energy cannot be read by the physical SPBM source, R15
# cannot PASS. The preflight must refuse EARLY with INSTRUMENT_UNAVAILABLE
# (exit 5) whenever the reader would not satisfy r15_energy_open/r15_energy_read
# (tests/runtime/r15_measure.c): all 5 energy + 5 power labels exact, every
# energyN_input / energyN_overflow_raw / powerN_input readable, overflow 0,
# package energy advancing, counter headroom. The preflight reads its label
# tables from r15_measure.c, and this test builds its fixtures from the same
# tables, so the two cannot drift. Fixtures are fake hwmon trees in a temp dir.
cd "$(dirname "$0")/.." || exit 1
T=tools/r15_machine_state.sh; fail=0; C=tests/runtime/r15_measure.c
W=$(mktemp -d) || exit 1; trap 'rm -rf "$W"' EXIT
export R15_ENERGY_PREFLIGHT_SLEEP=0
# fixture uptime: 100 s, so a 10 W floor is 1 kJ (1,000,000,000 uJ); fixtures hold 5 kJ unless a case says otherwise
printf "100.00 90.00\n" > "$W/uptime"; export R15_UPTIME_FILE="$W/uptime"
EL=$(sed -n 's/^static const char \*const k_elabel\[[A-Z_0-9]*\] = {\(.*\)};/\1/p' $C | tr -d '" ' | tr , ' ')
PL=$(sed -n 's/^static const char \*const k_plabel\[[A-Z_0-9]*\] = {\(.*\)};/\1/p' $C | tr -d '" ' | tr , ' ')
[ "$(echo $EL | wc -w)" = 5 ] && [ "$(echo $PL | wc -w)" = 5 ] || { echo "[-] cannot read label tables from $C"; exit 1; }
check() { # name expected-exit expected-text
    out=$(R15_HWMON_ROOT="$W/$1" "$T" energy-preflight 2>&1); rc=$?
    if [ "$rc" = "$2" ] && printf '%s\n' "$out" | grep -Fq -- "$3"; then echo "[+] $1: exit $rc"
    else echo "[-] $1: exit $rc (want $2), output: $out"; fail=$((fail + 1)); fi
}
mk() { # case: a complete, valid-looking reader tree (static counters)
    H=$W/$1/hwmon1; mkdir -p "$W/$1/hwmon0" "$H"; echo acpitz > "$W/$1/hwmon0/name"; echo aien_spbm > "$H/name"
    i=1; for l in $EL; do echo "$l" > "$H/energy${i}_label"; echo 5000000000 > "$H/energy${i}_input"; echo 0 > "$H/energy${i}_overflow_raw"; i=$((i + 1)); done
    i=1; for l in $PL; do echo "$l" > "$H/power${i}_label"; echo 20000000 > "$H/power${i}_input"; i=$((i + 1)); done
}
# advance <case> <start-uJ>: package counter rises 0.5 J then 1 J in the background while the preflight samples
put() { echo "$2" > "$1.tmp" && mv "$1.tmp" "$1"; } # atomic, so the preflight never reads a half-written file
advance() { ( sleep 0.05; put "$W/$1/hwmon1/energy1_input" $(( $2 + 500000 )); sleep 0.1; put "$W/$1/hwmon1/energy1_input" $(( $2 + 1000000 )) ) & }
mkdir -p "$W/absent/hwmon0"; echo acpitz > "$W/absent/hwmon0/name"; check absent 5 "reason=absent"
mkdir -p "$W/empty"; check empty 5 "reason=absent"
mk substitute; echo nvml > "$W/substitute/hwmon1/name"; check substitute 5 "reason=absent"
mk zero; echo 0 > "$W/zero/hwmon1/energy1_input"; check zero 5 "reads 0 uJ"
mk unreadable; rm "$W/unreadable/hwmon1/energy1_input"; check unreadable 5 "energy1_input"
mk garbage; echo -22 > "$W/garbage/hwmon1/energy1_input"; check garbage 5 "not a number"
mk stuck; check stuck 5 "did not advance"
mk wrongelabel; echo gpm > "$W/wrongelabel/hwmon1/energy1_label"; check wrongelabel 5 "energy1_label"
mk wrongelabel4; echo x > "$W/wrongelabel4/hwmon1/energy4_label"; check wrongelabel4 5 "energy4_label"
mk wrongplabel; echo x > "$W/wrongplabel/hwmon1/power3_label"; check wrongplabel 5 "power3_label"
mk missingplabel; rm "$W/missingplabel/hwmon1/power5_label"; check missingplabel 5 "power5_label"
mk noovf; rm "$W/noovf/hwmon1/energy2_overflow_raw"; check noovf 5 "energy2_overflow_raw unreadable"
mk ovf1; echo 1 > "$W/ovf1/hwmon1/energy1_overflow_raw"; check ovf1 5 "energy1_overflow_raw is '1'"
mk ovf5; echo 1 > "$W/ovf5/hwmon1/energy5_overflow_raw"; check ovf5 5 "energy5_overflow_raw is '1'"
mk nopower; rm "$W/nopower/hwmon1/power2_input"; check nopower 5 "power2_input"
mk badpower; echo abc > "$W/badpower/hwmon1/power4_input"; check badpower 5 "power4_input"
mk noenergy3; rm "$W/noenergy3/hwmon1/energy3_input"; check noenergy3 5 "energy3_input"
mk lowhead; echo 4294967000000000 > "$W/lowhead/hwmon1/energy3_input"; check lowhead 5 "headroom"
# whole-run headroom: 200 kJ left passes the old 300 s x 600 W (180 kJ) window check but is
# less than the planned run (2400 s x 100 W = 240 kJ), so a wrap could land mid-run
mk shortrun; echo 4094967295000 > "$W/shortrun/hwmon1/energy1_input"
advance shortrun 4094967295000
R15_ENERGY_PREFLIGHT_SLEEP=0.08 check shortrun 5 "planned run"; wait
# wrapped / reset counter: uptime 200000 s but only 1 kJ on the counter (floor 10 W x 200000 s = 2 MJ)
mk wrapped; echo 1000000000 > "$W/wrapped/hwmon1/energy1_input"; printf "200000.00 1.00\n" > "$W/wrapped-uptime"
advance wrapped 1000000000
R15_UPTIME_FILE="$W/wrapped-uptime" R15_ENERGY_PREFLIGHT_SLEEP=0.08 check wrapped 5 "wrapped or was reset"; wait
mk nouptime; advance nouptime 5000000000; R15_UPTIME_FILE="$W/does-not-exist" R15_ENERGY_PREFLIGHT_SLEEP=0.08 check nouptime 5 "uptime"; wait
# a decrease on any channel (not only package) invalidates the reader
mk decr3; echo 5000000000 > "$W/decr3/hwmon1/energy1_input"
( k=0; while [ $k -lt 60 ]; do put "$W/decr3/hwmon1/energy3_input" $((5000000000 - k * 1000)); k=$((k + 1)); sleep 0.01; done ) &  # steadily falling cpu_p counter
advance decr3 5000000000
R15_ENERGY_PREFLIGHT_SLEEP=0.08 check decr3 5 "energy3 counter went backwards"
wait
# valid: package counter advances between samples
mk valid
put "$W/valid/hwmon1/energy1_input" 5000100000
advance valid 5000100000
R15_ENERGY_PREFLIGHT_SLEEP=0.08 check valid 0 "energy preflight ok"
wait
# blocked run: receipt refuses (exit 4), writes nothing
R=$W/blocked; mkdir -p "$R"; printf 'BLOCKED_INSTRUMENT\nINSTRUMENT_UNAVAILABLE reason=absent\n' > "$R/instrument-unavailable.txt"
out=$(tools/r15_receipt.sh "$R" "$W/receipt-out" 2>&1); rc=$?
if [ "$rc" = 4 ] && printf '%s' "$out" | grep -Fq BLOCKED_INSTRUMENT && [ -z "$(ls "$W/receipt-out" 2>/dev/null)" ]; then
    echo "[+] receipt refuses blocked run: exit $rc, nothing written"
else echo "[-] receipt on blocked run: exit $rc (want 4): $out"; fail=$((fail + 1)); fi
# the qualify path seals a blocked directory with SHA256SUMS (same command as r15_qualify.sh)
( cd "$R" && find . -type f ! -name SHA256SUMS -printf "%P\n" | LC_ALL=C sort | xargs sha256sum > SHA256SUMS )
if ( cd "$R" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ) && grep -q instrument-unavailable.txt "$R/SHA256SUMS"; then echo "[+] blocked dir sealed"; else echo "[-] seal"; fail=$((fail + 1)); fi
grep -q 'printf "%P\\n" | LC_ALL=C sort | xargs sha256sum > SHA256SUMS' tools/r15_qualify.sh && echo "[+] qualify.sh uses the same seal command" || { echo "[-] qualify.sh seal missing"; fail=$((fail + 1)); }
echo "failures $fail"; [ "$fail" = 0 ]
