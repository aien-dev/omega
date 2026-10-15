#!/bin/bash
# run_gpu_decode_attn_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B cut 8 chip gate for omega_gpu_decode_attn_api (one-launch GQA decode attention, in-kernel KV append).
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_decode_attn_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only (bf16 rounding, RoPE rows, reference sanity, codegen
#      budgets, refusals) and --sim (warp-simulator parity on 8 contexts incl. multi-split and 2 chunks per CTA, mutants 1-4)
#   1. baseline: the already-proven matmul API chip timing (chip and session path work before the new kernel is judged)
#   2. main: parity against the double reference at contexts 1, 7, 256, 301 (sharp q), 1024, 4096, 8192, 32768; the appended
#      K/V rows read back; second call bit-identical; out_scale 2^64 path; mutants 1-4 at context 301; chip medians and
#      GB/s on the K+V bytes streamed, host caches evicted before each timing loop
#   3. one variable per run, informational: --no-evict (host caches warm: the flattering number), --depth 1 (shallowest
#      prefetch), --per-split 1024 (4 chunks per CTA, a quarter of the CTAs)
# Verdict line last: "GPU_DECODE_ATTN_CHIP: PASS|FAIL". Steps 0 and 2 decide; 1 and 3 are recorded.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD) physics_lock=$(cat physics.lock 2>/dev/null | head -1)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_decode_attn_test build/gpu_matmul_api_test > "$EVID/build.log" 2>&1 || { echo "GPU_DECODE_ATTN_CHIP: FAIL build"; exit 1; }
T=./build/gpu_decode_attn_test
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
./build/gpu_matmul_api_test --timing --out "$EVID/receipt-baseline-matmul.json" 2>&1 | tee "$EVID/baseline-matmul.log"
echo "baseline exit ${PIPESTATUS[0]} (recorded)" | tee -a "$EVID/baseline-matmul.log"
$T --reps 20 --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps 20 --no-evict --out "$EVID/receipt-noevict.json" 2>&1 | tee "$EVID/noevict.log"; echo "no-evict exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/noevict.log"
$T --reps 20 --depth 1 --out "$EVID/receipt-depth1.json" 2>&1 | tee "$EVID/depth1.log"; echo "depth 1 exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/depth1.log"
$T --reps 20 --per-split 1024 --out "$EVID/receipt-persplit1024.json" 2>&1 | tee "$EVID/persplit1024.log"; echo "per-split 1024 exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/persplit1024.log"

echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log device.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_DECODE_ATTN_CHIP: PASS"; else echo "GPU_DECODE_ATTN_CHIP: FAIL"; exit 1; fi
