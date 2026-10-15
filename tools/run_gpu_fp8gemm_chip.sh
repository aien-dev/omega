#!/bin/bash
# run_gpu_fp8gemm_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B chip gate for omega_gpu_fp8gemm_api (prefill FP8 grouped GEMM: each distinct expert streamed once for all its rows).
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_fp8gemm_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only (emulation vs double oracle, permutation, budgets) and
#      --sim (bit-identical to the host emulation in both fragment orders, mutants 1-3 caught)
#   1. main: gateup (128x1536x2048), down (128x2048x768), qkv (5120x2048), o (2048x4096), 64 tokens: every output
#      bit-identical to the per-row FP8 GEMV, host emulation + oracle on sampled outputs, second call identical,
#      scales-dropped / pointer / row-mix mutants caught on the chip, timing medians (host caches evicted) vs the
#      per-token grouped GEMV path
#   2. informational, one variable per run: --no-evict, --mt 8 / --mt 2 on gateup, --tokens 128 (M=1024) on gateup
# Verdict line last: "GPU_FP8GEMM_CHIP: PASS|FAIL". Steps 0 and 1 decide.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_fp8gemm_test > "$EVID/build.log" 2>&1 || { echo "GPU_FP8GEMM_CHIP: FAIL build"; exit 1; }
T=./build/gpu_fp8gemm_test; fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps 15 --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
for v in "--no-evict:noevict" "--shape gateup --mt 8:gateup-mt8" "--shape gateup --mt 2:gateup-mt2" "--shape gateup --tokens 128:gateup-m1024" "--shape down --tokens 128:down-m1024"; do
  a="${v%%:*}"; n="${v##*:}"
  $T --reps 9 $a --out "$EVID/receipt-$n.json" 2>&1 | tee "$EVID/$n.log"; echo "$a exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/$n.log"
done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_FP8GEMM_CHIP: PASS"; else echo "GPU_FP8GEMM_CHIP: FAIL"; exit 1; fi
