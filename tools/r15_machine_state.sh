#!/bin/bash
# r15_machine_state.sh -- machine physical state for R15 runs
# (spec/r15-performance-proof.md §18, clarification C3 of 2026-09-28,
# before any qualification data). Observability only: nothing here is a gate, and no trial is
# dropped or rerun because of it.
#
#   tools/r15_machine_state.sh preflight <out>  exit 0 if the X925 sustains
#                                               its clock, 3 if it does not
#   tools/r15_machine_state.sh start <out>      begin 1 s sampling
#   tools/r15_machine_state.sh mark <out> <text>
#   tools/r15_machine_state.sh stop <out>       end sampling, write summary
#
# The operating system's reported frequency does not show the sustained X925
# clock cut seen on 2026-09-28 (scaling_cur_freq stayed at 3.9 GHz while the
# core ran at 3.0-3.5 GHz), so the effective clock comes from each core's
# cycle counter (perf, system-wide, read from outside the measured processes)
# divided by the core's busy time from /proc/stat.
set -u
CUT_GHZ=3.75                # the preflight fails below this sustained median

# preflight_ghz <perf csv>: effective GHz per second while the core was fully
# busy (1 s intervals); exit 0 if the median of seconds 13-20 reaches CUT_GHZ,
# 3 below it, 4 with no samples. Counts only the CPU PMU cycles event: newer
# kernels also list SMMU PMUs (smmuv3_pmcg_*/cycles/) in the same interval,
# which are not core clocks.
preflight_ghz() {
    awk -F, -v cut="$CUT_GHZ" '
        $5 == "cycles" || $5 ~ /^armv8_pmuv3(_[0-9]+)?\/cycles\/$/ { n++; g = $3 / ($1 - last) / 1e9; last = $1; if (n > 12 && n <= 20) s[++m] = g; all = all sprintf(" %.3f", g) }
        END {
            if (m == 0) { print "preflight: no samples"; exit 4 }
            asort(s); med = (m % 2) ? s[(m + 1) / 2] : (s[m / 2] + s[m / 2 + 1]) / 2
            printf "per-second GHz:%s\nmedian seconds 13-20: %.3f GHz (cut-off %.2f)\n", all, med, cut
            exit (med < cut) ? 3 : 0
        }' "$1"
}

# preflight-parse <perf csv>: the preflight verdict on a recorded file (tests).
if [ "${1:-}" = preflight-parse ]; then preflight_ghz "${2:?perf csv}"; exit $?; fi

CMD=${1:?preflight, start, mark or stop}
OUT=${2:?out dir}
mkdir -p "$OUT"
X925=7                      # the preflight core (a Cortex-X925)
SAMPLER_CPU=${R15_STATE_CPU:-0}

spbm_energy() {
    for h in /sys/class/hwmon/hwmon*; do
        case "$(cat "$h/name" 2>/dev/null)" in aien_spbm*) cat "$h/energy1_input" 2>/dev/null; return;; esac
    done
    echo -1
}

# busy jiffies per cpu, "cpuN busy" lines
busy() { awk '/^cpu[0-9]/{b=$2+$3+$4+$7+$8; print $1, b}' /proc/stat; }

case "$CMD" in
preflight)
    # 20 s of sustained load on one X925 core while perf counts its cycles.
    F=$OUT/preflight.txt
    { echo "== preflight $(date -u +%FT%TZ): 20 s sustained on cpu $X925"; busy; } > "$F"
    sudo -n perf stat -a -A -C "$X925" -e cycles -I 1000 -x, -o "$OUT/preflight-perf.csv" -- \
        taskset -c "$X925" timeout 20 sha256sum /dev/zero >/dev/null 2>&1
    busy >> "$F"
    preflight_ghz "$OUT/preflight-perf.csv" >> "$F"
    rc=$?
    [ $rc = 0 ] && echo "result: X925 sustains its clock" >> "$F" || echo "result: MACHINE-STATE FAILURE (rc $rc)" >> "$F"
    exit $rc
    ;;
start)
    date -u +%FT%TZ > "$OUT/machine-state-start.txt"
    for c in /sys/devices/system/cpu/cpu[0-9]*; do
        echo "${c##*/} $(cat "$c/cpufreq/scaling_governor" 2>/dev/null) max=$(cat "$c/cpufreq/cpuinfo_max_freq" 2>/dev/null)"
    done >> "$OUT/machine-state-start.txt"
    sudo -n perf stat -a -A -e cycles -I 1000 -x, -o "$OUT/machine-perf.csv" -- sleep 86400 >/dev/null 2>&1 &
    echo $! > "$OUT/.perf.pid"
    (
        n=0
        while :; do
            t=$(date +%s.%N)
            b=$(busy | awk '{printf "%s:%s ", $1, $2}')
            th=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | tr '\n' ' ')
            g=""
            if [ $((n % 5)) = 0 ]; then
                g=$(nvidia-smi --query-gpu=clocks.sm,power.draw,utilization.gpu,temperature.gpu,clocks_event_reasons.active --format=csv,noheader,nounits 2>/dev/null | tr -d ' ')
            fi
            printf '{"t":%s,"spbm_uj":%s,"loadavg":"%s","busy":"%s","thermal_mc":"%s","gpu":"%s"}\n' \
                "$t" "$(spbm_energy)" "$(cut -d' ' -f1-3 /proc/loadavg)" "$b" "$th" "$g"
            n=$((n + 1))
            sleep 1
        done
    ) >> "$OUT/machine-state.ndjson" 2>/dev/null &
    echo $! > "$OUT/.sampler.pid"
    taskset -pc "$SAMPLER_CPU" "$(cat "$OUT/.sampler.pid")" >/dev/null 2>&1
    ;;
mark)
    echo "$(date +%s.%N) ${3:-}" >> "$OUT/machine-state-marks.txt"
    ;;
stop)
    for p in .sampler.pid .perf.pid; do
        [ -f "$OUT/$p" ] || continue
        pid=$(cat "$OUT/$p")
        pkill -P "$pid" 2>/dev/null
        kill "$pid" 2>/dev/null
        sudo -n pkill -f "perf stat -a -A -e cycles -I 1000 -x, -o $OUT/machine-perf.csv" 2>/dev/null
        rm -f "$OUT/$p"
    done
    # Summary (informative; the raw files are authoritative): per-second
    # cycles of each X925 core (cpus 5-9, 15-19) as GHz. A fully busy second
    # reads as the effective clock; a partly idle second reads lower, so the
    # busy jiffies in machine-state.ndjson decide individual seconds.
    awk -F, '$5 == "cycles" || $5 ~ /^armv8_pmuv3(_[0-9]+)?\/cycles\/$/ { split($2, c, "CPU"); cpu = c[2] + 0
        if ((cpu >= 5 && cpu <= 9) || (cpu >= 15 && cpu <= 19)) { g = $3 / 1e9; if (g >= 1.0) v[++n] = g } }
        END {
            if (!n) { print "X925: no second above 1 GHz of cycles"; exit }
            asort(v)
            printf "X925 seconds with >= 1 G cycles: %d; p10 %.3f, median %.3f, max %.3f GHz\n", n, v[int(n * 0.1) + 1], v[int(n / 2) + 1], v[n]
        }' "$OUT/machine-perf.csv" > "$OUT/machine-state-summary.txt" 2>/dev/null
    awk '{ if (match($0, /"t":[0-9.]+/)) t = substr($0, RSTART + 4, RLENGTH - 4)
           if (match($0, /"spbm_uj":-?[0-9]+/)) e = substr($0, RSTART + 10, RLENGTH - 10) + 0
           if (e >= 0) { if (t0 == "") { t0 = t; e0 = e } t1 = t; e1 = e } }
         END { if (t0 != "" && t1 > t0) printf "package energy %.1f J over %.0f s, mean %.2f W\n", (e1 - e0) / 1e6, t1 - t0, (e1 - e0) / 1e6 / (t1 - t0)
               else print "package energy: SPBM reader not loaded" }' "$OUT/machine-state.ndjson" >> "$OUT/machine-state-summary.txt"
    ;;
*) echo "unknown command $CMD" >&2; exit 64;;
esac
