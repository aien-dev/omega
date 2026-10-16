#!/bin/bash
# run_gpu_tgemm_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# Trainer lane E1 chip gate for the device-resident bf16 training GEMM (omega_gpu_tgemm_api.h, cut 1: NN, F32 overwrite).
# Caller holds the GB10 quiet lock: quietlock run -- tools/run_gpu_tgemm_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only (instruction / register budgets) and --sim (bit-identical to the
#      HMMA-chain emulation in both fragment orders, tile K ranges, mutants 1-4 caught)
#   1. yardstick: tests/gpu_tc_peak_test (HMMA.16816.F32.BF16 dense throughput ceiling); the best "peak ..." line's TFLOPS is the
#      100% every percentage below is measured against
#   2. gate: 8192x768x{2304,768,3072} and 8192x3072x768, uncached then GPU-cached buffers: every output within 1e-5 of the
#      double reference, repeats bit-identical, poison scan clean, mutants 1 and 4 caught, chip-time TFLOPS >= 25% of the yardstick
#   3. informational, one variable per run: 8192x2304x768, 1024x768x50304 (lm_head gradient width, warps 8), the gate shape at
#      warps 8 and at rt 2
# Verdict line last: "GPU_TGEMM_CHIP: PASS|FAIL". Steps 0 to 2 decide.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version,clocks.sm,clocks.max.sm,temperature.gpu --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_tgemm_test build/gpu_tc_peak_test > "$EVID/build.log" 2>&1 || { echo "GPU_TGEMM_CHIP: FAIL build"; exit 1; }
T=./build/gpu_tgemm_test; fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
timeout 300 ./build/gpu_tc_peak_test --chip 2>&1 | tee "$EVID/peak.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
PEAK=$(grep '^peak ' "$EVID/peak.log" | sed 's/.*us, \([0-9.]*\) TFLOPS.*/\1/' | sort -g | tail -1)
echo "yardstick: bf16 HMMA dense peak $PEAK TFLOPS (best 'peak' line of peak.log)" | tee "$EVID/yardstick.txt"
if [ -z "$PEAK" ]; then echo "GPU_TGEMM_CHIP: FAIL no yardstick"; exit 1; fi
$T --chip --reps 15 --gate 25 --peak "$PEAK" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
for v in "--shape 8192x2304x768:n2304" "--shape 1024x768x50304:k50304" "--shape 8192x768x2304 --shape-knob 4 8 1:warps8" "--shape 8192x768x2304 --shape-knob 2 8 1:rt2"; do
  a="${v%%:*}"; n="${v##*:}"
  $T --chip --reps 9 --peak "$PEAK" $a 2>&1 | tee "$EVID/$n.log"; echo "$a exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/$n.log"
done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.log *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_TGEMM_CHIP: PASS" | tee -a "$EVID/main.log"; else echo "GPU_TGEMM_CHIP: FAIL" | tee -a "$EVID/main.log"; exit 1; fi
