#!/bin/bash
# r15_machine_state.sh -- machine physical state for R15 runs
# (spec/r15-performance-proof.md §18, clarification C3 of 2026-09-28,
# before any qualification data). Observability only: nothing here is a gate, and no trial is
# dropped or rerun because of it.
#
#   tools/r15_machine_state.sh preflight <out>  exit 0 if the X925 sustains
#                                               its clock, 3 if it does not
#   tools/r15_machine_state.sh energy-preflight exit 0 if the SPBM package-energy
#                                               reader gives valid samples, 5 if not
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


# energy_preflight: is the spec §7/§17 C2 package-energy reader present and
# returning valid samples? The binding source is the owner-key signed
# read-only SPBM reader (hwmon name "aien_spbm", energy1 labelled "pkg", the
# same match tests/runtime/r15_measure.c uses). Spec §7: "If package energy
# cannot be read by a physical hardware source at qualification time, metric
# 17 is incomplete and R15 cannot PASS. GPU-domain energy alone is not
# accepted", and §17 says GPM and NVML are "never substituted". So no other
# source is tried here. Exit 0 = reader valid; 5 = INSTRUMENT_UNAVAILABLE
# (reason=absent or reason=invalid). That is a blocked run, not a measured
# FAIL: no trial is started, no receipt is written.
# Env (tests): R15_HWMON_ROOT (default /sys/class/hwmon), R15_ENERGY_PREFLIGHT_SLEEP (0.1 s).
energy_preflight() {
    local root=${R15_HWMON_ROOT:-/sys/class/hwmon} nap=${R15_ENERGY_PREFLIGHT_SLEEP:-0.1}
    local h="" d s v prev="" first="" n=0 ov
    for d in "$root"/hwmon*; do
        [ "$(cat "$d/name" 2>/dev/null)" = aien_spbm ] && { h=$d; break; }
    done
    if [ -z "$h" ]; then
        echo "INSTRUMENT_UNAVAILABLE reason=absent: no hwmon named aien_spbm (the signed SPBM reader is not loaded); package energy cannot be read, R15 cannot PASS (spec §7)"
        return 5
    fi
    if [ "$(cat "$h/energy1_label" 2>/dev/null)" != pkg ]; then
        echo "INSTRUMENT_UNAVAILABLE reason=invalid: energy1_label is not pkg at $h"
        return 5
    fi
    ov=$(cat "$h/energy1_overflow_raw" 2>/dev/null)
    if [ -n "$ov" ] && [ "$ov" != 0 ]; then
        echo "INSTRUMENT_UNAVAILABLE reason=invalid: energy1 overflow indicator is $ov"
        return 5
    fi
    for s in 1 2 3; do
        v=$(cat "$h/energy1_input" 2>/dev/null)
        case "$v" in ''|*[!0-9]*) echo "INSTRUMENT_UNAVAILABLE reason=invalid: sample $s of energy1_input is not a number ('$v')"; return 5;; esac
        if [ "$v" -le 0 ]; then echo "INSTRUMENT_UNAVAILABLE reason=invalid: sample $s reads $v uJ (a running package counter is above zero)"; return 5; fi
        if [ -n "$prev" ] && [ "$v" -lt "$prev" ]; then echo "INSTRUMENT_UNAVAILABLE reason=invalid: counter went backwards ($prev -> $v uJ)"; return 5; fi
        [ -z "$first" ] && first=$v
        prev=$v; n=$((n + 1))
        [ "$s" -lt 3 ] && sleep "$nap"
    done
    if [ "$prev" -le "$first" ]; then
        echo "INSTRUMENT_UNAVAILABLE reason=invalid: counter did not advance over $n samples ($first uJ)"
        return 5
    fi
    echo "energy preflight ok: $h pkg $first -> $prev uJ over $n samples"
    return 0
}
if [ "${1:-}" = energy-preflight ]; then energy_preflight; exit $?; fi
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
    # perf's workload ends by itself when stop drops .perf.stop (cap 24 h). A
    # bare `sleep 86400` outlived pkill of perf (root, reparented to init) and
    # kept the caller's quietlock hold alive for a day (2026-10-05 13:35Z).
    rm -f "$OUT/.perf.stop"
    sudo -n perf stat -a -A -e cycles -I 1000 -x, -o "$OUT/machine-perf.csv" -- \
        sh -c 'i=0; while [ ! -e "$1/.perf.stop" ] && [ $i -lt 86400 ]; do sleep 1; i=$((i + 1)); done' \
        r15-sampler "$OUT" >/dev/null 2>&1 &
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
    # Let perf's workload end on its own so perf exits and flushes its last
    # interval; the pkill below is only the fallback.
    touch "$OUT/.perf.stop"
    if [ -f "$OUT/.perf.pid" ]; then
        w=0; while [ -d "/proc/$(cat "$OUT/.perf.pid")" ] && [ $w -lt 5 ]; do sleep 1; w=$((w + 1)); done
    fi
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
