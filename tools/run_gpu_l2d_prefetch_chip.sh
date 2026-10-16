#!/bin/bash
# run_gpu_l2d_prefetch_chip.sh EVIDENCE_DIR [PHYSICS_DIR]   (OWN-30B W-L2d, 2026-10-14; run under flock /tmp/aien-gb10.lock)
# One variable: the o_proj weight prefetch launch beside the attention (decode graph --prefetch), on GPU-cached weights.
# Synthetic 2-layer token (no model load). Interleaved A/B: cached base, cached prefetch, cached base, cached prefetch;
# control: uncached + prefetch (no L2 keep possible: expect no o_proj gain); red: --prefetch-red MUST FAIL;
# cmdlist chip test (regression of the cmdlist change). Every graph run checks B == C bit for bit + mutants and prints the
# per-kind TIMELINE and the logits hash. Verdict line last: "GPU_L2D_PREFETCH_CHIP: PASS|FAIL".
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{ echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2; uptime; } > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_decode_graph_test build/gpu_cmdlist_test > "$EVID/build.log" 2>&1 || { echo "GPU_L2D_PREFETCH_CHIP: FAIL build"; exit 1; }
D=./build/gpu_decode_graph_test
sha256sum "$D" build/gpu_cmdlist_test build/libomega_gpu.a > "$EVID/binaries.sha256"
fail=0
run() { local name="$1"; shift
  $D "$@" --reps "${REPS:-10}" --out "$EVID/receipt-$name.json" > "$EVID/$name.log" 2>&1 || fail=1
  echo "$name ($*): $(grep -E 'TIMELINE|PER TOKEN|^chip:' "$EVID/$name.log" | tr '\n' ' ')"
  grep -o '"C0_logits_fnv":"[0-9a-f]*"' "$EVID/receipt-$name.json"; }
run 1-cached-base --cached-weights
run 2-cached-prefetch --cached-weights --prefetch
run 3-cached-base --cached-weights
run 4-cached-prefetch --cached-weights --prefetch
run 5-uncached-prefetch --prefetch
$D --cached-weights --prefetch-red --reps 3 --out "$EVID/receipt-red.json" > "$EVID/red.log" 2>&1; rc=$?
echo "red (prefetch-red): exit $rc ($(tail -1 "$EVID/red.log"))"; [ "$rc" != 0 ] || { echo "red UNEXPECTED_PASS"; fail=1; }
./build/gpu_cmdlist_test > "$EVID/cmdlist.log" 2>&1 || fail=1; echo "cmdlist: $(tail -1 "$EVID/cmdlist.log")"
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum ./* 2>/dev/null | grep -v SHA256SUMS > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_L2D_PREFETCH_CHIP: PASS"; else echo "GPU_L2D_PREFETCH_CHIP: FAIL"; exit 1; fi
