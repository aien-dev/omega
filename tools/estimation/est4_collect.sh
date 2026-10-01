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
if [ -e "$quiet" ]; then
    echo "est4_collect: $quiet exists; another run owns the machine" >&2
    exit 3
fi
if pgrep -x est_load > /dev/null 2>&1; then
    echo "est4_collect: an est_load process is running; refusing" >&2
    exit 3
fi
printf 'owner: lane15 est v4 load period (%s)\nstarted: %s\nexpected end: %s\nplease: no builds or tests until this file is gone\n' \
    "$role" "$(date -u +%FT%TZ)" "$(date -u -d "+$secs seconds" +%FT%TZ)" > "$quiet"
d=$root/$(date -u +%Y%m%dT%H%M%SZ)-est4-$role-silicon
mkdir -p "$d"
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
    sleep 1
done
wait "$lpid"
(cd "$d" && sha256sum machine-state.ndjson machine-state-marks.txt machine-state-start.txt schedule.txt > SHA256SUMS)
rm -f "$quiet"
echo "$d"
