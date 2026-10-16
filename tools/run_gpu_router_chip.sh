#!/bin/bash
# run_gpu_router_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B cut 8 item 2 chip gate for omega_gpu_router_api (one-launch MoE router: RMSNorm, logits, top-8, weights,
# x' row and the grouped FP8 GEMV's group tables written on the device).
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_router_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only (oracle, codegen budgets, refusals) and --sim
#      (warp-simulator parity at 8 warps, both fragment orders, norm_topk on and off, mutants 1-4 caught)
#   1. baseline: the already-proven matmul API chip timing (chip and session path work before the new kernel is judged)
#   2. main: parity against the double oracle on 36 random hidden states (layer 0; norm_topk on and off; x scales
#      0.01 / 1 / 40; out_scale 1 and 2^64), second call bit-identical, mutants 1-4 at the chip, end-to-end on layer 1:
#      the grouped FP8 gate+up GEMV (128 x 1536 x 2048) reading the router-written table and x' equals the host-table
#      call bit for bit and one group matches the FP8 host oracle; chip medians of router + gate+up, host caches evicted
#   3. one variable per run, informational: kernel shape (warps 32 depth 8; 16/8; 8/8) and --no-evict
# Verdict line last: "GPU_ROUTER_CHIP: PASS|FAIL". Steps 0 and 2 decide; 1 and 3 are recorded.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD) physics_lock=$(cat physics.lock 2>/dev/null | head -1)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_router_test build/gpu_matmul_api_test > "$EVID/build.log" 2>&1 || { echo "GPU_ROUTER_CHIP: FAIL build"; exit 1; }
T=./build/gpu_router_test
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
./build/gpu_matmul_api_test --timing --out "$EVID/receipt-baseline-matmul.json" 2>&1 | tee "$EVID/baseline-matmul.log"
echo "baseline exit ${PIPESTATUS[0]} (recorded)" | tee -a "$EVID/baseline-matmul.log"
$T --reps 50 --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
for shape in "32 8" "16 8" "8 8"; do
  set -- $shape
  $T --reps 50 --warps "$1" --depth "$2" --out "$EVID/receipt-w$1-d$2.json" 2>&1 | tee "$EVID/w$1-d$2.log"; echo "warps $1 depth $2 exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/w$1-d$2.log"
done
$T --reps 50 --no-evict --out "$EVID/receipt-noevict.json" 2>&1 | tee "$EVID/noevict.log"; echo "no-evict exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/noevict.log"

echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log device.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_ROUTER_CHIP: PASS"; else echo "GPU_ROUTER_CHIP: FAIL"; exit 1; fi
