#!/bin/bash
# run_gpu_prefill_attn_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B L1 attention chip gate for omega_gpu_prefill_attn_api (tensor-core causal prefill attention, chunked, in-kernel
# K/V append). Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_prefill_attn_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only (budgets, placeholder patches) and --sim (simulator parity,
#      rows bit-equal to the emulation, mutants 1-4)
#   1. baseline: the already-proven decode attention chip gate binary at one quick point (the session path and the
#      decode kernel the gate compares against work before the new kernel is judged)
#   2. main (warps 8): parity vs the double reference, gate 2 (rows bit-identical to m decode steps on a twin cache,
#      outputs within tolerance), second call identical, out_scale 2^64, mutants 1-4, timing medians vs the decode loop
#   3. one variable, informational: warps 4
#   cut 3 (2026-10-13): main runs the default split path (APPEND + ATTEND launches) and the device-resident entry;
#   main-fused (--fused --quick) re-proves the cut-1 path; 4. spread study at 2048 tokens (informational): an SM clock
#   camera (nvidia-smi sampled every 100 ms, outside the measured path) over: fused cold (baseline), fused warm 5,
#   split cold, split warm 5, 15 reps each, per-rep and per-launch times printed.
# Verdict line last: "GPU_PREFILL_ATTN_CHIP: PASS|FAIL". Steps 0 and 2 decide; 1 and 3 are recorded.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_prefill_attn_test build/gpu_decode_attn_test > "$EVID/build.log" 2>&1 || { echo "GPU_PREFILL_ATTN_CHIP: FAIL build"; exit 1; }
T=./build/gpu_prefill_attn_test
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
./build/gpu_decode_attn_test --reps 3 --only 1024 --out "$EVID/receipt-baseline-decode.json" 2>&1 | tail -20 | tee "$EVID/baseline-decode.log"
echo "baseline exit ${PIPESTATUS[0]} (recorded)" | tee -a "$EVID/baseline-decode.log"
$T --reps 9 --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps 9 --warps 4 --quick --out "$EVID/receipt-warps4.json" 2>&1 | tee "$EVID/warps4.log"; echo "warps 4 exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/warps4.log"
$T --reps 9 --fused --quick --out "$EVID/receipt-main-fused.json" 2>&1 | tee "$EVID/main-fused.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
nvidia-smi --query-gpu=timestamp,clocks.sm,clocks.mem,utilization.gpu,power.draw,pstate --format=csv -lms 100 > "$EVID/clocks.csv" 2>&1 &
CAM=$!
for v in "fused-cold:--fused" "fused-warm5:--fused --warm 5" "split-cold:" "split-warm5:--warm 5"; do
  name=${v%%:*}; flags=${v#*:}
  echo "variant $name start $(date -u +%FT%T.%3NZ)" >> "$EVID/spread-marks.txt"
  $T --timing-only --reps 15 $flags 2>&1 | tee "$EVID/spread-$name.log"; echo "$name exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/spread-$name.log"
  echo "variant $name end $(date -u +%FT%T.%3NZ)" >> "$EVID/spread-marks.txt"
done
kill $CAM 2>/dev/null; wait $CAM 2>/dev/null
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.csv *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_PREFILL_ATTN_CHIP: PASS"; else echo "GPU_PREFILL_ATTN_CHIP: FAIL"; exit 1; fi
