#!/bin/bash
# run_gpu_tern_chip.sh PHASE EVIDENCE_DIR [PHYSICS_DIR]
# Ternary engine T0 chip gate (2026-10-15): KT1 W2A8 ternary GEMV (omega_gpu_tern_api.h) plus the BitNet-shape runs of
# the FP8 and bf16 table GEMVs and the decode attention geometry check. Each PHASE is short so it fits one quiet-flag
# window (at most 20 minutes); run each under its own hold, e.g.
#   quietlock hold --owner tern-t0 --minutes 15 --reason "KT1 phase base" -- tools/run_gpu_tern_chip.sh base EVID
# Phases (verdict line last: "GPU_TERN_CHIP <phase>: PASS|FAIL"):
#   base   host proofs (--host-only, --sim; never hardware results), then the baselines before KT1 runs on the chip:
#          launch floor (gpu_launch_cost_test), word table 128256 x 2560 on the bf16 and FP8 GEMVs (each with its
#          mutants), decode attention at 20 / 5 / 128 (a recorded refusal is the expected outcome)
#   red    negative controls: gpu_tern_test with mutants 1, 3 and 2 forced must FAIL
#   main   gpu_tern_test: correctness battery, mutants caught, cold bandwidth and in-list launch floor (gate judged on
#          kernel time, the in-list launch floor taken out; both figures in the receipt)
#   sweep  informational, one variable per run against the default (row-major, rows 1, 8 warps, 160 CTAs): row-major
#          rows 2, block layout rows 4, warps 4 and 16, CTAs 96 and 256
# Speeds are judged only against the physical limit: 273 GB/s spec, 237.2 GB/s practical.
set -u
PHASE="${1:?phase}"; EVID="${2:?evidence dir}"; PHYS="${3:-$HOME/workspace/physics-own30b-k}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "phase $PHASE start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain --untracked-files=no | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -r; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null
} >> "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j8 build/gpu_tern_test build/gpu_bf16_gemv_test build/gpu_fp8_api_test build/gpu_decode_attn_test build/gpu_launch_cost_test > "$EVID/build-$PHASE.log" 2>&1 || { echo "GPU_TERN_CHIP $PHASE: FAIL build"; exit 1; }
T=./build/gpu_tern_test; fail=0
case "$PHASE" in
base)
  $T --host-only 2>&1 | tee "$EVID/host-only.log" | tail -3; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
  $T --sim 2>&1 | tee "$EVID/sim.log" | tail -3; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
  ./build/gpu_launch_cost_test --out "$EVID/receipt-launch-cost.json" > "$EVID/launch-cost.log" 2>&1
  echo "launch-cost exit $? (recorded)" | tee -a "$EVID/launch-cost.log"
  ./build/gpu_bf16_gemv_test --n 128256 --k 2560 --reps 20 --out "$EVID/receipt-table-bf16.json" > "$EVID/table-bf16.log" 2>&1
  r=$?; echo "table-bf16 exit $r" | tee -a "$EVID/table-bf16.log"; [ $r = 0 ] || fail=1
  ./build/gpu_fp8_api_test --n 128256 --k 2560 --reps 20 --out "$EVID/receipt-table-fp8.json" > "$EVID/table-fp8.log" 2>&1
  r=$?; echo "table-fp8 exit $r" | tee -a "$EVID/table-fp8.log"; [ $r = 0 ] || fail=1
  ./build/gpu_decode_attn_test --q-heads 20 --kv-heads 5 --head-dim 128 --out "$EVID/receipt-attn-20-5.json" > "$EVID/attn-20-5.log" 2>&1
  r=$?; echo "attn-20-5 exit $r (3 = recorded refusal)" | tee -a "$EVID/attn-20-5.log"; [ $r = 3 ] || fail=1
  ;;
red)
  for m in 1 3 2; do
    $T --mutant $m --no-bw --out "$EVID/receipt-red-mutant$m.json" > "$EVID/red-mutant$m.log" 2>&1
    r=$?; echo "mutant $m exit $r (must be 1: FAIL)" | tee -a "$EVID/red-mutant$m.log"; [ $r = 1 ] || fail=1
  done
  ;;
main)
  $T --reps 5 --out "$EVID/receipt-main.json" > "$EVID/main.log" 2>&1; r=$?; echo "main exit $r" | tee -a "$EVID/main.log"; [ $r = 0 ] || fail=1
  ;;
sweep)
  for v in "--layout 0 --rows 2:rm-r2" "--layout 1 --block-rows 4:blk-r4" "--warps 4:w4" "--warps 16:w16" "--ctas 96:c96" "--ctas 256:c256"; do
    a="${v%%:*}"; n="${v##*:}"
    $T --bw-only --reps 3 $a --out "$EVID/receipt-sweep-$n.json" > "$EVID/sweep-$n.log" 2>&1; echo "$a exit $? (informational)" | tee -a "$EVID/sweep-$n.log"
  done
  ;;
*) echo "unknown phase $PHASE"; exit 2 ;;
esac
echo "phase $PHASE end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
if [ $fail = 0 ]; then echo "GPU_TERN_CHIP $PHASE: PASS"; else echo "GPU_TERN_CHIP $PHASE: FAIL"; exit 1; fi
