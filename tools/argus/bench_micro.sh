#!/bin/bash
# ARGUS_PERFORMANCE micro-op: interleaved RX_ARGUS=0/1/2(discard)/2(ingest) runs of
# bench_rx_argus pinned with taskset. Usage: bench_micro.sh OUT_DIR CPUS ROUNDS N S > out.jsonl
# Run under: flock ~/workspace/.argus-bench.lock tools/argus/bench_micro.sh ...
set -u
D=${1:-build}; CPUS=${2:-7}; ROUNDS=${3:-7}; N=${4:-20000000}; S=${5:-200000}
for r in $(seq 1 "$ROUNDS"); do
  for cfg in 0 1 2d 2i; do
    case $cfg in
      0) b=$D/argus0/bench_rx_argus; env=();;
      1) b=$D/argus1/bench_rx_argus; env=();;
      2d) b=$D/argus2/bench_rx_argus; env=(RX_ARGUS_CONSUMER=discard);;
      2i) b=$D/argus2/bench_rx_argus; env=(RX_ARGUS_CONSUMER=ingest);;
    esac
    out=$(env "${env[@]}" taskset -c "$CPUS" "./$b" "$N" "$S" 2>/dev/null | tail -1)
    printf '{"cfg":"%s","round":%d,"cpus":"%s","load1":%s,"res":%s}\n' "$cfg" "$r" "$CPUS" \
      "$(cut -d' ' -f1 /proc/loadavg)" "${out:-null}"
  done
done
