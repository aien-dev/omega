#!/bin/bash
# R8 wall clock, interleaved configurations (speed2). Each config is NAME=DIR:CONSUMER, where
# DIR holds rx_r8_aegis_test and CONSUMER is none|discard|ingest, optionally @CPULIST to pin the
# consumer thread (RX_ARGUS_CONSUMER_CPU), e.g. ingest@2. The order within a round
# rotates by round so no config always runs first. Timer: date +%s%N around the taskset'd
# process (us resolution; includes the same exec overhead for every config).
# Usage: r8_wall_ab.sh CPUS ROUNDS NAME=DIR:CONSUMER ... > out.jsonl   (under the bench flock)
set -u
CPUS=$1; ROUNDS=$2; shift 2
cfgs=("$@"); n=${#cfgs[@]}
for r in $(seq 1 "$ROUNDS"); do
  for j in $(seq 0 $((n - 1))); do
    c=${cfgs[$(( (j + r) % n ))]}; name=${c%%=*}; rest=${c#*=}; dir=${rest%%:*}; mode=${rest#*:}
    pin=""; case $mode in *@*) pin=${mode#*@}; mode=${mode%%@*};; esac
    if [ "$mode" = none ]; then env=(); else env=(RX_ARGUS_CONSUMER="$mode"); fi
    [ -n "$pin" ] && env+=(RX_ARGUS_CONSUMER_CPU="$pin")
    log=$(mktemp -p "${TMPDIR_BENCH:-.}")
    t0=$(date +%s%N); env "${env[@]}" taskset -c "$CPUS" "$dir/rx_r8_aegis_test" >"$log" 2>&1; rc=$?; t1=$(date +%s%N)
    pass=$(grep -q "failures 0" "$log" && echo true || echo false)
    printf '{"cfg":"%s","round":%d,"cpus":"%s","load1":%s,"wall_us":%d,"rc":%d,"passed":%s}\n' \
      "$name" "$r" "$CPUS" "$(cut -d' ' -f1 /proc/loadavg)" $(( (t1 - t0) / 1000 )) "$rc" "$pass"
    rm -f "$log"
  done
done
