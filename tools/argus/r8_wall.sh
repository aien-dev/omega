#!/bin/bash
# R8 wall clock, interleaved RX_ARGUS=0 vs RX_ARGUS=2 (ingest consumer).
# Usage: r8_wall.sh OUT_DIR CPUS ROUNDS > out.jsonl  (run under the bench flock)
set -u
D=${1:-build}; CPUS=${2:-5-9,15-19}; ROUNDS=${3:-30}
TIMEFORMAT='%3R %3U %3S'
for r in $(seq 1 "$ROUNDS"); do
  for cfg in 0 2; do
    b=$D/argus$cfg/rx_r8_aegis_test; log=$(mktemp); tf=$(mktemp)
    if [ "$cfg" = 2 ]; then
      { time env RX_ARGUS_CONSUMER=ingest taskset -c "$CPUS" "./$b" >"$log" 2>&1; } 2>"$tf"; rc=$?
    else
      { time taskset -c "$CPUS" "./$b" >"$log" 2>&1; } 2>"$tf"; rc=$?
    fi
    read -r real user sys < "$tf"
    pass=$(grep -q "failures 0" "$log" && echo true || echo false)
    printf '{"cfg":"%s","round":%d,"cpus":"%s","load1":%s,"wall_s":%s,"user_s":%s,"sys_s":%s,"rc":%d,"passed":%s}\n' \
      "$cfg" "$r" "$CPUS" "$(cut -d' ' -f1 /proc/loadavg)" "$real" "$user" "$sys" "$rc" "$pass"
    rm -f "$log" "$tf"
  done
done
