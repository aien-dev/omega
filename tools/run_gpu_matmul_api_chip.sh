#!/bin/bash
# run_gpu_matmul_api_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# FB-1 cut 1b chip gate for omega_gpu_matmul_api (run through lanes.sh queue --heavy, never killed).
#   1. parity sweep (cut-1 shapes + Llama shapes), host double reference, rel < 1e-5
#   2. mutant sweep: wrong kernel (last K step dropped) must be caught on every shape
#   3. resident GEMV timing: 1x2048x8192 and 1x8192x2048, median of 100 calls < 5 ms
#   4. CTA budget experiment (I42): the same sweep subset at 128 and 256 CTAs, informational
# Verdict line last: "GPU_MATMUL_API_CHIP: PASS|FAIL". Steps 1-3 decide, step 4 is recorded only.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_matmul_api_test > "$EVID/build.log" 2>&1 || { echo "GPU_MATMUL_API_CHIP: FAIL build"; exit 1; }
T=./build/gpu_matmul_api_test
fail=0
$T --out "$EVID/receipt-sweep.json" 2>&1 | tee "$EVID/sweep.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --mutant --out "$EVID/receipt-mutant.json" 2>&1 | tee "$EVID/mutant.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --timing --out "$EVID/receipt-timing.json" 2>&1 | tee "$EVID/timing.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
for b in 128 256; do
  OMEGA_GPU_MATMUL_SHAPES="256,256,256;1,2048,8192;1,2048,128256" $T --cta-budget $b --out "$EVID/receipt-cta$b.json" 2>&1 | tee "$EVID/cta$b.log"
  echo "cta budget $b exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/cta$b.log"
done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log device.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_MATMUL_API_CHIP: PASS"; else echo "GPU_MATMUL_API_CHIP: FAIL"; exit 1; fi
