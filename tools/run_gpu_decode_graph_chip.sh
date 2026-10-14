#!/bin/bash
# run_gpu_decode_graph_chip.sh EVIDENCE_DIR [main|red|cached] [PHYSICS_DIR]
#   cached: main with --cached-weights (FP8 tensors in GPU-cached memory); verdict "GPU_DECODE_GRAPH_CHIP_CACHED: PASS|FAIL"
# OWN-30B W-L2a chip gate for omega_gpu_decode_graph (one Qwen3-30B-A3B decode token as one command list).
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_decode_graph_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only
#   1. main: A (today's path, host glue) vs C (one list) vs B (one launch per submission), synthetic 2-layer step on
#      random weights, real vocab 151936; argmax identical, logits within tolerance, B == C bit for bit, negative
#      controls (SWIGLU / COMBINE mutants) caught; per-token medians over REPS (20).
#   red: the same with the wrong-wiring mutant in C (must FAIL: red-before-green receipt).
# Verdict line last: "GPU_DECODE_GRAPH_CHIP: PASS|FAIL" (red: "GPU_DECODE_GRAPH_CHIP_RED: FAILED_AS_EXPECTED|UNEXPECTED_PASS").
set -u
EVID="${1:?evidence dir}"; MODE="${2:-main}"; PHYS="${3:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) mode=$MODE omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
git diff HEAD > "$EVID/worktree.diff"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_decode_graph_test > "$EVID/build.log" 2>&1 || { echo "GPU_DECODE_GRAPH_CHIP: FAIL build"; exit 1; }
T=./build/gpu_decode_graph_test
sha256sum "$T" build/libomega_gpu.a > "$EVID/binaries.sha256"
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
if [ "$MODE" = cached ]; then
  $T --cached-weights --reps "${REPS:-20}" --out "$EVID/receipt-cached.json" 2>&1 | tee "$EVID/cached.log"; rc=${PIPESTATUS[0]}
  echo "end $(date -u +%FT%TZ) cached_exit=$rc" >> "$EVID/device.txt"
  (cd "$EVID" && sha256sum *.json *.log *.txt *.diff *.sha256 > SHA256SUMS)
  if [ "$rc" = 0 ] && [ $fail = 0 ]; then echo "GPU_DECODE_GRAPH_CHIP_CACHED: PASS"; exit 0; else echo "GPU_DECODE_GRAPH_CHIP_CACHED: FAIL"; exit 1; fi
fi
if [ "$MODE" = red ]; then
  $T --red --reps "${REPS:-3}" --out "$EVID/receipt-red.json" 2>&1 | tee "$EVID/red.log"; rc=${PIPESTATUS[0]}
  echo "end $(date -u +%FT%TZ) red_exit=$rc" >> "$EVID/device.txt"
  (cd "$EVID" && sha256sum *.json *.log *.txt *.diff *.sha256 > SHA256SUMS)
  if [ "$rc" != 0 ]; then echo "GPU_DECODE_GRAPH_CHIP_RED: FAILED_AS_EXPECTED"; else echo "GPU_DECODE_GRAPH_CHIP_RED: UNEXPECTED_PASS"; exit 1; fi
  exit 0
fi
$T --reps "${REPS:-20}" --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.txt *.diff *.sha256 > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_DECODE_GRAPH_CHIP: PASS"; else echo "GPU_DECODE_GRAPH_CHIP: FAIL"; exit 1; fi
