#!/bin/sh
# ESTIMATION v4 (docs/estimation/protocols/est-v4.md) data collection under the
# declared est_load schedule. Same collector as v3 (est3c_collect.sh); only the
# folder tag, the flag owner text and the est_load process check differ.
#   tools/estimation/est4_collect.sh <fit|heldout|smoke> <seconds> <seed> <raw-root> <est_load-binary>
# Writes <raw-root>/<UTC>-est4-<role>-silicon/ with machine-state.ndjson (1 Hz),
# machine-state-marks.txt, schedule.txt, machine-state-start.txt, SHA256SUMS.
# Coordination: refuses to start while ~/workspace/.spark-quiet exists or any
# est_load process runs; holds the flag (text "lane15 est v4 load period") for
# its own run and removes it afterwards. Prints no thermal statistic.
set -u
role=${1:?fit, heldout or smoke}
secs=${2:?seconds}
seed=${3:?seed}
root=${4:?raw root}
load=${5:?est_load binary}
quiet=$HOME/workspace/.spark-quiet
if pgrep -x est_load > /dev/null 2>&1; then
    echo "est4_collect: an est_load process is running; refusing" >&2
    exit 3
fi
# Take the flag atomically (noclobber): refuse if another run owns the machine.
if ! ( set -C; printf 'owner: lane15 est v4 load period (%s)\nstarted: %s\nexpected end: %s\nplease: no builds or tests until this file is gone\n' \
    "$role" "$(date -u +%FT%TZ)" "$(date -u -d "+$secs seconds" +%FT%TZ)" > "$quiet" ) 2> /dev/null; then
    echo "est4_collect: $quiet exists; another run owns the machine" >&2
    exit 3
fi
lpid=
cleanup() {
    [ -n "$lpid" ] && kill "$lpid" 2> /dev/null
    rm -f "$quiet"
}
trap cleanup EXIT
trap 'exit 4' HUP INT TERM
mkdir -p "$root" || exit 5
d=$root/$(date -u +%Y%m%dT%H%M%SZ)-est4-$role-silicon
# Plain mkdir: refuses an existing folder, so a prior collection is never truncated.
mkdir "$d" || { echo "est4_collect: $d already exists; refusing" >&2; exit 5; }
date -u +%FT%TZ > "$d/machine-state-start.txt"
: > "$d/machine-state-marks.txt"
"$load" "$seed" "$secs" "$d/machine-state-marks.txt" > "$d/schedule.txt" &
lpid=$!
end=$(( $(date +%s) + secs ))
while [ "$(date +%s)" -lt "$end" ]; do
    t=$(date +%s.%N)
    th=$(cat /sys/class/thermal/thermal_zone*/temp | tr '\n' ' ')
    la=$(cut -d' ' -f1-3 /proc/loadavg)
    cpu=$(head -1 /proc/stat | cut -d' ' -f3-10)
    printf '{"t":%s,"thermal_mc":"%s","loadavg":"%s","cpu":"%s"}\n' "$t" "$th" "$la" "$cpu" >> "$d/machine-state.ndjson"
    if ! kill -0 "$lpid" 2> /dev/null && [ "$(( end - $(date +%s) ))" -gt 5 ]; then
        echo "est4_collect: est_load exited early; collection is INVALID" >&2
        lpid=; echo INVALID_EARLY_LOAD_EXIT > "$d/INVALID"; exit 6
    fi
    sleep 1
done
wait "$lpid"; rc=$?; lpid=
if [ "$rc" -ne 0 ]; then
    echo "est4_collect: est_load exit status $rc; collection is INVALID" >&2
    echo "INVALID_LOAD_EXIT $rc" > "$d/INVALID"; exit 6
fi
(cd "$d" && sha256sum machine-state.ndjson machine-state-marks.txt machine-state-start.txt schedule.txt > SHA256SUMS)
echo "$d"
