#!/bin/bash
# ARGUS_PERFORMANCE micro-op: interleaved RX_ARGUS=0/1/2(discard)/2(ingest) runs of
# bench_rx_argus pinned with taskset. Usage: bench_micro.sh OUT_DIR CPUS ROUNDS N S > out.jsonl
# CFGS (env) selects configs; 2dn/2in = discard/ingest with RX_ARGUS_CONSUMER_CPU=none
# (consumer unpinned, i.e. sharing the taskset CPUs; the default is auto placement).
# Run under: flock ~/workspace/.argus-bench.lock tools/argus/bench_micro.sh ...
set -u
D=${1:-build}; CPUS=${2:-7}; ROUNDS=${3:-7}; N=${4:-20000000}; S=${5:-200000}
for r in $(seq 1 "$ROUNDS"); do
  for cfg in ${CFGS:-0 1 2d 2i}; do
    case $cfg in
      0) b=$D/argus0/bench_rx_argus; env=();;
      1) b=$D/argus1/bench_rx_argus; env=();;
      2d) b=$D/argus2/bench_rx_argus; env=(RX_ARGUS_CONSUMER=discard);;
      2i) b=$D/argus2/bench_rx_argus; env=(RX_ARGUS_CONSUMER=ingest);;
      2dn) b=$D/argus2/bench_rx_argus; env=(RX_ARGUS_CONSUMER=discard RX_ARGUS_CONSUMER_CPU=none);;
      2in) b=$D/argus2/bench_rx_argus; env=(RX_ARGUS_CONSUMER=ingest RX_ARGUS_CONSUMER_CPU=none);;
    esac
    out=$(env "${env[@]}" taskset -c "$CPUS" "./$b" "$N" "$S" 2>/dev/null | tail -1)
    printf '{"cfg":"%s","round":%d,"cpus":"%s","load1":%s,"res":%s}\n' "$cfg" "$r" "$CPUS" \
      "$(cut -d' ' -f1 /proc/loadavg)" "${out:-null}"
  done
done
