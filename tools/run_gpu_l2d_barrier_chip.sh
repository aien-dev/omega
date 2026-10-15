#!/bin/bash
# run_gpu_l2d_barrier_chip.sh EVIDENCE_DIR [PHYSICS_DIR]   (OWN-30B W-L2d, 2026-10-14; run under flock /tmp/aien-gb10.lock)
# One variable: the barrier between the launches of one command list. Kernel-only (no model load).
#   1. launch-cost repro (33 MB FP8 GEMV split 1..16 ways): wfi, wfi_only, wfi_data, flush_data, none, wfi (drift control)
#   2. decode graph (synthetic 2-layer token): barrier 0 (baseline), 2, 3, 4, 0 again; each run checks B (per-launch submits)
#      == C (one list) bit for bit, the mutants, and prints the per-kind TIMELINE + logits hash
#   3. negative control: decode graph with barrier 1 (none) MUST FAIL
# Verdict line last: "GPU_L2D_BARRIER_CHIP: PASS|FAIL".
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2; uptime
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_decode_graph_test build/gpu_launch_cost_test > "$EVID/build.log" 2>&1 || { echo "GPU_L2D_BARRIER_CHIP: FAIL build"; exit 1; }
D=./build/gpu_decode_graph_test; L=./build/gpu_launch_cost_test
sha256sum "$D" "$L" build/libomega_gpu.a > "$EVID/binaries.sha256"
fail=0
if [ "${SKIP_COST:-0}" != 1 ]; then
  $L --reps "${REPS_COST:-20}" --out "$EVID/receipt-cost.json" > "$EVID/cost.log" 2>&1 || fail=1
  grep -E "^COST|PASS|FAIL" "$EVID/cost.log" | tail -40
fi
i=0
for b in 0 2 3 4 0; do
  i=$((i + 1))
  $D --barrier "$b" --reps "${REPS:-10}" --out "$EVID/receipt-graph-$i-b$b.json" > "$EVID/graph-$i-b$b.log" 2>&1 || fail=1
  echo "graph $i barrier $b: $(grep -E '^fp8 knobs|TIMELINE|PER TOKEN|^chip:' "$EVID/graph-$i-b$b.log" | tr '\n' ' ')"
  grep -o '"C0_logits_fnv":"[0-9a-f]*"' "$EVID/receipt-graph-$i-b$b.json"
done
$D --barrier 1 --reps 3 --out "$EVID/receipt-graph-red-b1.json" > "$EVID/graph-red-b1.log" 2>&1; rc=$?
echo "negative control barrier none: exit $rc ($(tail -1 "$EVID/graph-red-b1.log"))"
[ "$rc" != 0 ] || { echo "negative control UNEXPECTED_PASS"; fail=1; }
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum ./* 2>/dev/null | grep -v SHA256SUMS > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_L2D_BARRIER_CHIP: PASS"; else echo "GPU_L2D_BARRIER_CHIP: FAIL"; exit 1; fi
