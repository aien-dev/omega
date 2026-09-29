#!/bin/bash
# Per-thread perf counters for R8, interleaved RX_ARGUS=0 / 2 discard / 2 ingest.
# Counters are counted, not sampled (period 1e12), plus one 2 ms cpu-clock sampler so
# perf report has samples. Per-thread totals via `perf record -s` + `perf report -T`,
# classified by r8_perthread.awk. pmu1 = X925 PMU, pmu0 = A725 PMU on the GB10.
# Usage: r8_perthread.sh BUILD_DIR CPUS ROUNDS WORKDIR > out.jsonl   (under the bench flock)
set -u
D=$(realpath "${1:-build}"); CPUS=${2:-5-9,15-19}; ROUNDS=${3:-20}; W=${4:-.}
AWK=$(realpath "$(dirname "$0")/r8_perthread.awk")
cd "$W" || exit 1
EV="cycles,instructions,task-clock,context-switches,cpu-migrations,cpu-clock/period=2000000/"
for r in $(seq 1 "$ROUNDS"); do
  for cfg in ${CFGS:-0 2d 2i}; do
    case $cfg in
      0) b=$D/argus0/rx_r8_aegis_test; env=();;
      2d) b=$D/argus2/rx_r8_aegis_test; env=(RX_ARGUS_CONSUMER=discard);;
      2i) b=$D/argus2/rx_r8_aegis_test; env=(RX_ARGUS_CONSUMER=ingest);;
    esac
    t0=$(date +%s%N)
    env "${env[@]}" perf record -q -s -c 1000000000000 -e "$EV" -o pt.data -- taskset -c "$CPUS" "$b" >pt.log 2>&1
    t1=$(date +%s%N)
    pass=$(grep -q "failures 0" pt.log && echo true || echo false)
    # main thread only: --no-inherit counts the exec'd leader, not its threads or children
    env "${env[@]}" perf stat --no-inherit -x, -e cycles,instructions,task-clock,context-switches,cpu-migrations \
      -o st.txt -- taskset -c "$CPUS" "$b" >/dev/null 2>&1
    mainj=$(awk -F, '$1 ~ /^[0-9.]+$/ { k = $3; gsub(/armv8_pmuv3_/, "pmu", k); gsub(/\//, "_", k); sub(/_$/, "", k)
      if (k in s) k = k "_b"; s[k] = 1; o = o sprintf(",\"%s\":%s", k, $1) } END { print "{\"n\":1" o "}" }' st.txt)
    perf report -i pt.data -T --stdio 2>/dev/null | awk -v cfg="$cfg" -v r="$r" -v wall=$(( (t1-t0)/1000 )) -v pass="$pass" -v mainj="$mainj" -f "$AWK"
  done
done
rm -f pt.data pt.log st.txt
