#!/bin/sh
# R15 preflight parser regression (tools/r15_machine_state.sh preflight-parse).
# kernel-with-smmu-pmus.csv is the 2026-10-05 12:53Z preflight file whose SMMU
# PMU "cycles" lines made the old parser divide by zero; cpu-pmu-only.csv is the
# 2026-09-29 PASS run's file. Both must read the X925 at ~3.9 GHz (exit 0); a
# 3 GHz core must fail the 3.75 GHz cut (exit 3); a file with no CPU cycles
# event must report no samples (exit 4), never a clock.
cd "$(dirname "$0")/.." || exit 1
T=tools/r15_machine_state.sh; D=tests/r15_preflight; fail=0
check() { # file expected-exit expected-text
    out=$("$T" preflight-parse "$D/$1" 2>&1); rc=$?
    if [ "$rc" = "$2" ] && printf '%s\n' "$out" | grep -Fq -- "$3"; then echo "[+] $1: exit $rc"
    else echo "[-] $1: exit $rc (want $2), output: $(printf '%s' "$out" | tail -1)"; fail=$((fail + 1)); fi
}
check kernel-with-smmu-pmus.csv 0 "median seconds 13-20: 3.896 GHz"
check cpu-pmu-only.csv 0 "median seconds 13-20: 3.896 GHz"
check slow-core-3ghz.csv 3 "median seconds 13-20: 3.000 GHz"
check no-cpu-cycles.csv 4 "preflight: no samples"
echo "failures $fail"; [ "$fail" = 0 ]
