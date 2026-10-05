#!/bin/bash
# DIAGNOSTIC ONLY (not qualifying): G15 seat-liveness rate over 30 back-to-back SEQ trials,
# CAND-1 executables (overlay 81efb09, rx_r15_perf_silicon b44b3320...), co-resident atlas python present.
set -u
O=$HOME/workspace/cand2-campaign/wt-cand1-overlay; OUT=${OUT:?}; mkdir -p "$OUT"; cd "$O" || exit 2
snap() { d=$OUT/machine-$1; mkdir -p $d; date -u +%FT%TZ > $d/time.txt; nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv > $d/gpu-processes.csv 2>&1
  nvidia-smi --query-gpu=utilization.gpu,clocks.sm,temperature.gpu,power.draw --format=csv > $d/gpu.csv 2>&1; cat /proc/loadavg > $d/loadavg.txt
  ps -eo pid,user,pcpu,etime,args --sort=-pcpu | head -25 | cut -c1-180 > $d/top.txt; uname -r > $d/kernel.txt; cat /proc/driver/nvidia/version > $d/driver.txt 2>&1; }
snap before; sha256sum build/rx_r15_perf_silicon > $OUT/executable.sha256; git rev-parse HEAD > $OUT/commit.txt
RUN=diag-g15-$(date -u +%Y%m%dT%H%M%SZ)
for i in $(seq 1 30); do
  f=$OUT/trial-SEQ-$(printf %02d $i).jsonl
  ./build/rx_r15_perf_silicon trial SEQ "$RUN" "$i" "$f" > $OUT/trial-$i.log 2>&1; rc=$?
  r=$(grep '"kind":"residency"' "$f" | grep -o '"intervals":[0-9]*,"seat_live":[0-9]*' | head -1)
  echo "trial $i rc=$rc $r" | tee -a $OUT/SUMMARY.txt
done
snap after; echo "diag done" >> $OUT/SUMMARY.txt
