#!/bin/sh
# ESTIMATION v5 (docs/estimation/protocols/est-v5.md) data collection under the declared
# est_load schedule. The v4 collector (est4_collect.sh) with an ENFORCED WINDOW: it runs only
# while the caller holds the machine-wide quiet flag for this gate (the flag is enforced by
# the quiet-guard hook), it never creates or removes the flag, it refuses to start when a
# foreign build/test/gate process is already running, and it records a process monitor
# (process-monitor.log, one sample every 2 s) whose verdict est5_window_check.sh gives.
#   tools/estimation/est5_collect.sh <fit|heldout|smoke> <seconds> <seed> <raw-root> <est_load-binary>
# Writes <raw-root>/<UTC>-est5-<role>-silicon/ with machine-state.ndjson (1 Hz),
# machine-state-marks.txt, schedule.txt, machine-state-start.txt, process-monitor.log,
# WINDOW verdict and SHA256SUMS. Prints no thermal statistic.
set -u
role=${1:?fit, heldout or smoke}
secs=${2:?seconds}
seed=${3:?seed}
root=${4:?raw root}
load=${5:?est_load binary}
here=$(cd "$(dirname "$0")" && pwd)
quiet=$HOME/workspace/.spark-quiet
if ! grep -q '^gate est-v5$' "$quiet" 2> /dev/null; then
    echo "est5_collect: $quiet missing or not held for gate est-v5; the window must be held first" >&2
    exit 3
fi
if pgrep -x est_load > /dev/null 2>&1; then
    echo "est5_collect: an est_load process is running; refusing" >&2
    exit 3
fi
pre=$(sh "$here/est5_window_scan.sh" 0)
if [ -n "$pre" ]; then
    echo "est5_collect: foreign build/test/gate process already running; refusing:" >&2
    echo "$pre" | head -5 >&2
    exit 3
fi
lpid=
mpid=
cleanup() {
    [ -n "$lpid" ] && kill "$lpid" 2> /dev/null
    [ -n "$mpid" ] && kill "$mpid" 2> /dev/null
}
trap cleanup EXIT
trap 'exit 4' HUP INT TERM
mkdir -p "$root" || exit 5
d=$root/$(date -u +%Y%m%dT%H%M%SZ)-est5-$role-silicon
# Plain mkdir: refuses an existing folder, so a prior collection is never truncated.
mkdir "$d" || { echo "est5_collect: $d already exists; refusing" >&2; exit 5; }
date -u +%FT%TZ > "$d/machine-state-start.txt"
: > "$d/machine-state-marks.txt"
: > "$d/process-monitor.log"
"$load" "$seed" "$secs" "$d/machine-state-marks.txt" > "$d/schedule.txt" &
lpid=$!
# process monitor: a sample line every 2 s, FOREIGN lines for builds/tests/gates, and
# informational HIGHCPU lines (any process above 25 % CPU other than the load generator).
(
    while :; do
        ts=$(date -u +%FT%TZ)
        echo "$ts S $(ps -e --no-headers | wc -l)"
        sh "$here/est5_window_scan.sh" "$lpid" | sed "s/^/$ts /"
        ps -eo pid=,pcpu=,comm= | awk -v own="$lpid" -v ts="$ts" '$1 != own && $2 > 25 { print ts, "HIGHCPU", $1, $2, $3 }'
        sleep 2
    done
) >> "$d/process-monitor.log" 2> /dev/null &
mpid=$!
end=$(( $(date +%s) + secs ))
while [ "$(date +%s)" -lt "$end" ]; do
    t=$(date +%s.%N)
    th=$(cat /sys/class/thermal/thermal_zone*/temp | tr '\n' ' ')
    la=$(cut -d' ' -f1-3 /proc/loadavg)
    cpu=$(head -1 /proc/stat | cut -d' ' -f3-10)
    printf '{"t":%s,"thermal_mc":"%s","loadavg":"%s","cpu":"%s"}\n' "$t" "$th" "$la" "$cpu" >> "$d/machine-state.ndjson"
    if ! kill -0 "$lpid" 2> /dev/null && [ "$(( end - $(date +%s) ))" -gt 5 ]; then
        echo "est5_collect: est_load exited early; collection is INVALID" >&2
        lpid=; echo INVALID_EARLY_LOAD_EXIT > "$d/INVALID"; exit 6
    fi
    sleep 1
done
wait "$lpid"; rc=$?; lpid=
kill "$mpid" 2> /dev/null; mpid=
if [ "$rc" -ne 0 ]; then
    echo "est5_collect: est_load exit status $rc; collection is INVALID" >&2
    echo "INVALID_LOAD_EXIT $rc" > "$d/INVALID"; exit 6
fi
wrc=0
sh "$here/est5_window_check.sh" "$d" "$secs" > "$d/WINDOW" || wrc=$?
(cd "$d" && sha256sum machine-state.ndjson machine-state-marks.txt machine-state-start.txt schedule.txt process-monitor.log WINDOW > SHA256SUMS)
echo "$d"
cat "$d/WINDOW" >&2
[ "$wrc" -eq 0 ] || exit 7
