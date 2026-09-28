#!/bin/bash
# run.sh -- R15 hardware characterization campaign (2026-09-28).
#
#   research/r15-hwchar/run.sh <out-dir>
#
# Quiet-machine protocol: creates ~/workspace/.spark-quiet (every Omega session
# holds heavy work while it exists), waits until the 1-minute load average is
# below 1.0 (max 15 min, then records that it was not quiet), runs, removes the
# marker on every exit path. kernel.perf_event_paranoid 4 -> 2 for the run and
# back to 4 on every exit path. Never reboots, never changes clocks.
set -u
OUT=${1:?out dir}
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/hwchar
mkdir -p "$OUT"
QUIET=$HOME/workspace/.spark-quiet
A725=2      # a Cortex-A725 core that is not cpu 0 (interrupts)
X925=7      # a Cortex-X925 core
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/progress.log"; }
finish() {
    sudo -n sysctl -q kernel.perf_event_paranoid=4 >/dev/null 2>&1
    echo "paranoid restored: $(cat /proc/sys/kernel/perf_event_paranoid)" >> "$OUT/progress.log"
    rm -f "$QUIET"
    say "quiet marker removed; done"
}
trap finish EXIT
echo "R15 hardware characterization $(date -u +%FT%TZ) by the R15 session" > "$QUIET"
say "quiet marker up; waiting for load < 1.0"
for i in $(seq 1 90); do
    l=$(cut -d' ' -f1 /proc/loadavg)
    awk -v l="$l" 'BEGIN{exit !(l < 1.0)}' && break
    sleep 10
done
say "load at start: $(cat /proc/loadavg)"
ps -eo pcpu,etime,args --sort=-pcpu | head -15 > "$OUT/processes-before.txt"
sudo -n sysctl -q kernel.perf_event_paranoid=2 >/dev/null 2>&1
say "paranoid now $(cat /proc/sys/kernel/perf_event_paranoid)"

# ---- machine state ----------------------------------------------------------
{
    echo "== uname"; uname -a
    echo "== cmdline"; cat /proc/cmdline
    echo "== dmi"; for f in bios_vendor bios_version bios_date board_name product_name; do
        echo "$f: $(cat /sys/class/dmi/id/$f 2>/dev/null)"; done
    echo "== cpufreq"; for c in $(seq 0 19); do d=/sys/devices/system/cpu/cpu$c/cpufreq
        echo "cpu$c $(cat $d/scaling_governor) cur=$(cat $d/scaling_cur_freq) min=$(cat $d/scaling_min_freq) max=$(cat $d/scaling_max_freq) hwmax=$(cat $d/cpuinfo_max_freq)"; done
    echo "== thermal"; for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/temp)"; done
    echo "== nvidia-smi"; nvidia-smi -q -d CLOCK,PERFORMANCE,POWER,TEMPERATURE,UTILIZATION 2>&1
    echo "== firmware"; fwupdmgr get-devices 2>/dev/null | grep -E "^[├└]|Current version"
} > "$OUT/machine-state.txt" 2>&1

# ---- effective clock of every core, before ---------------------------------
say "per-core effective clock (before)"
"$BIN" freq > "$OUT/freq-before.jsonl"

# ---- cold and warm, 5 fresh processes each ---------------------------------
for rep in 1 2 3 4 5; do
    for cfg in "$A725 ref" "$X925 ref" "$X925 quad4" "$A725 quad4"; do
        set -- $cfg
        "$BIN" bench "$1" "$2" cold >> "$OUT/cold.jsonl"
        "$BIN" bench "$1" "$2" warm >> "$OUT/warm.jsonl"
    done
done
say "cold/warm x5 done"

# ---- sustained 30 s and 2 min -----------------------------------------------
for secs in 30 120; do
    for cfg in "$A725 ref" "$X925 ref" "$X925 quad4"; do
        set -- $cfg
        say "sustained $secs s: cpu $1 $2"
        "$BIN" sustain "$1" "$2" "$secs" >> "$OUT/sustain-$secs.jsonl"
    done
done

# ---- effective clock of every core, after ----------------------------------
say "per-core effective clock (after)"
"$BIN" freq > "$OUT/freq-after.jsonl"
ps -eo pcpu,etime,args --sort=-pcpu | head -15 > "$OUT/processes-after.txt"
say "load at end: $(cat /proc/loadavg)"
