#!/bin/sh
# EST-3c (protocol v3, docs/estimation/EST3C_PROTOCOL_V3.md) data collection
# under a declared CPU load schedule.
#   tools/estimation/est3c_collect.sh <fit|heldout|smoke> <seconds> <seed> <raw-root> <est_load-binary>
# Writes <raw-root>/<UTC>-est3c-<role>-silicon/ with
#   machine-state.ndjson  1 Hz lines {"t":<s.ns>,"thermal_mc":"<z0> <z1> ...",
#                         "loadavg":"<1m 5m 15m>","cpu":"<user nice system idle iowait irq softirq steal>"}
#                         (same thermal shape as R15 / EST-3b; extra keys are ignored by est_replay)
#   machine-state-marks.txt  load trials written by est_load (R15 marks shape)
#   schedule.txt          the schedule est_load derived from <seed>, printed before it started
#   machine-state-start.txt, SHA256SUMS
# Coordination: refuses to start while ~/workspace/.spark-quiet exists (another
# timed run owns the machine); creates it for the duration of its own run and
# removes it afterwards. No root, no perf, no /dev/null, no Python.
set -u
role=${1:?fit, heldout or smoke}
secs=${2:?seconds}
seed=${3:?seed}
root=${4:?raw root}
load=${5:?est_load binary}
quiet=$HOME/workspace/.spark-quiet
if [ -e "$quiet" ]; then
    echo "est3c_collect: $quiet exists; another timed run owns the machine" >&2
    exit 3
fi
printf 'owner: lane4 ESTIMATION-2 est3c %s collection\nstarted: %s\nexpected end: %s\nplease: no builds or tests until this file is gone\n' \
    "$role" "$(date -u +%FT%TZ)" "$(date -u -d "+$secs seconds" +%FT%TZ)" > "$quiet"
d=$root/$(date -u +%Y%m%dT%H%M%SZ)-est3c-$role-silicon
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
