#!/bin/bash
# run_gpu_l2_stale_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# omega #457 chip gate: after the host rewrites a GPU-cached (CPU_WRITE | GPU_READ) buffer the GPU already read, the next
# launches must read the new bytes. tests/gpu_l2_stale_test.c, 5 rounds of 4 cases:
#   rewrite --cached 1  the case under test (unfixed platform: 4096 of 4096 lines stale on every launch, 5/5)
#   rewrite --cached 0  negative control (uncached mapping, nvos.h:1117-1118): FRESH on any platform
#   realloc --cached 0|1  free + fresh upload: FRESH on any platform
# Every case first proves the GEMV sees A's bytes (+2048 on every row) before judging B.
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_l2_stale_chip.sh EVID
# Verdict line last: "GPU_L2_STALE_CHIP: PASS|FAIL". PASS = all 20 runs FRESH (exit 0).
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD) physics_lock=$(cat physics.lock 2>/dev/null | head -1)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_l2_stale_test > "$EVID/build.log" 2>&1 || { echo "GPU_L2_STALE_CHIP: FAIL build"; exit 1; }
T=./build/gpu_l2_stale_test
fail=0
for r in 1 2 3 4 5; do for m in rewrite realloc; do for c in 0 1; do
  f="$EVID/$m-c$c-r$r.log"
  $T --mode "$m" --cached "$c" --reps 3 > "$f" 2>&1; e=$?
  echo "# exit $e" >> "$f"
  [ "$e" = 0 ] || fail=1
  echo "r$r $(grep -h '^STALE\|INVALID\|^FAIL' "$f" | tail -1) exit=$e"
done; done; done | tee "$EVID/summary.txt"
grep -q 'exit=[^0]' "$EVID/summary.txt" && fail=1
( cd "$EVID" && sha256sum device.txt build.log summary.txt *-r*.log > SHA256SUMS )
if [ $fail = 0 ]; then echo "GPU_L2_STALE_CHIP: PASS"; else echo "GPU_L2_STALE_CHIP: FAIL"; exit 1; fi
