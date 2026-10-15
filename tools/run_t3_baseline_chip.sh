#!/bin/bash
# run_t3_baseline_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B T3 baseline: the existing FFMA FP8 grouped GEMM (omega_gpu_fp8gemm_api, commit 14ffa824 lineage) at the
# largest M its API accepts (1024 rows = 128 tokens x top-8), gate|up (n 1536, k 2048) and down (n 2048, k 768),
# 5 independent process runs per shape (each: upload, parity vs the per-row GEMV, medians of 9, caches evicted).
# Kernel only, no model load. Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_t3_baseline_chip.sh EVID
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"; OUT="${OUT_DIR:-/tmp/t3-omega}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make OUT_DIR="$OUT" PHYSICS_DIR="$PHYS" -j10 "$OUT/gpu_fp8gemm_test" > "$EVID/build.log" 2>&1 || { echo "T3_BASELINE: FAIL build"; exit 1; }
T="$OUT/gpu_fp8gemm_test"; fail=0
for shape in gateup down; do
  for r in 1 2 3 4 5; do
    $T --shape $shape --tokens 128 --reps 9 --out "$EVID/receipt-$shape-r$r.json" > "$EVID/$shape-r$r.log" 2>&1
    rc=$?; echo "$shape r$r exit $rc" >> "$EVID/exits.txt"; [ $rc = 0 ] || fail=1
    grep -h "GEMM (medians" "$EVID/$shape-r$r.log" | sed "s/^/$shape r$r: /"
  done
done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "T3_BASELINE: PASS"; else echo "T3_BASELINE: FAIL"; exit 1; fi
