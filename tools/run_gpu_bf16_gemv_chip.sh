#!/bin/bash
# run_gpu_bf16_gemv_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B chip gate for omega_gpu_bf16_gemv_api (bf16 streaming GEMV, the lm_head 151936 x 2048).
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_bf16_gemv_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only and --sim (bit-identical to the f32 emulation, mutants 1-3 caught)
#   1. baseline: the proven matmul API chip timing
#   2. main: all 151936 logits bit-identical to the host emulation, within 1e-5 of the double oracle, second call
#      bit-identical, bf16-x entry, mutants 1-3 caught on the chip, timing medians (evicted host caches)
#   3. informational, one variable per run: --no-evict, --depth 4, --warps 32, --ctas 48; then the report-only
#      fp8-vs-bf16 argmax study on the real checkpoint (nothing is switched)
# Verdict line last: "GPU_BF16_GEMV_CHIP: PASS|FAIL". Steps 0 and 2 decide.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
MODEL="${MODEL_DIR:-$HOME/models/qwen3-30b-a3b-instruct-2507-fp8}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_bf16_gemv_test build/gpu_matmul_api_test build/gpu_lmhead_fp8_study > "$EVID/build.log" 2>&1 || { echo "GPU_BF16_GEMV_CHIP: FAIL build"; exit 1; }
T=./build/gpu_bf16_gemv_test; fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
./build/gpu_matmul_api_test --timing --out "$EVID/receipt-baseline-matmul.json" 2>&1 | tee "$EVID/baseline-matmul.log"
echo "baseline exit ${PIPESTATUS[0]} (recorded)" | tee -a "$EVID/baseline-matmul.log"
$T --reps 50 --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
for v in "--no-evict:noevict" "--depth 4:d4" "--warps 32:w32" "--ctas 48:c48"; do
  a="${v%%:*}"; n="${v##*:}"
  $T --reps 30 $a --out "$EVID/receipt-$n.json" 2>&1 | tee "$EVID/$n.log"; echo "$a exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/$n.log"
done
./build/gpu_lmhead_fp8_study "$MODEL" --out "$EVID/argmax-study.txt" 2>&1 | tee "$EVID/argmax-study.log"; echo "study exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/argmax-study.log"
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_BF16_GEMV_CHIP: PASS"; else echo "GPU_BF16_GEMV_CHIP: FAIL"; exit 1; fi
