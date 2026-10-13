#!/bin/bash
# run_gpu_fp8_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B Cut 6 / Cut 5 chip gate for omega_gpu_fp8_api (FP8 e4m3 block-scaled GEMV, grouped experts).
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_fp8_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only (exhaustive e4m3 conversion, pack refusals,
#      codegen budgets, nvdisasm listing) and --sim (warp simulator parity + mutants)
#   1. baseline: the already-proven matmul API chip timing (the chip and session path work before the new kernel is judged)
#   2. main: parity battery at the automatic shape (one-hot conversion on the chip, 14 shapes, grouped experts,
#      cache hit bit-identical, mutants caught), bandwidth per shape in receipt-main.json
#   3. one variable per run, informational: --no-evict (host caches warm after upload: the flattering number),
#      rows per warp 2/4/8 (default 1), warps 4, CTA budget 128/512
# Verdict line last: "GPU_FP8_CHIP: PASS|FAIL". Steps 0 and 2 decide; 1 and 3 are recorded.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD) physics_lock=$(cat physics.lock 2>/dev/null | head -1)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_fp8_api_test build/gpu_matmul_api_test > "$EVID/build.log" 2>&1 || { echo "GPU_FP8_CHIP: FAIL build"; exit 1; }
T=./build/gpu_fp8_api_test
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
./build/gpu_matmul_api_test --timing --out "$EVID/receipt-baseline-matmul.json" 2>&1 | tee "$EVID/baseline-matmul.log"
echo "baseline exit ${PIPESTATUS[0]} (recorded)" | tee -a "$EVID/baseline-matmul.log"
$T --reps 50 --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps 50 --no-evict --out "$EVID/receipt-noevict.json" 2>&1 | tee "$EVID/noevict.log"; echo "no-evict exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/noevict.log"
for r in 2 4 8; do
  $T --reps 20 --rows $r --out "$EVID/receipt-rows$r.json" 2>&1 | tee "$EVID/rows$r.log"
  echo "rows $r exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/rows$r.log"
done
$T --reps 20 --warps 4 --out "$EVID/receipt-warps4.json" 2>&1 | tee "$EVID/warps4.log"; echo "warps 4 exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/warps4.log"
for b in 128 512; do
  $T --reps 20 --cta-budget $b --out "$EVID/receipt-cta$b.json" 2>&1 | tee "$EVID/cta$b.log"
  echo "cta budget $b exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/cta$b.log"
done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log device.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_FP8_CHIP: PASS"; else echo "GPU_FP8_CHIP: FAIL"; exit 1; fi
