#!/bin/bash
# run_gpu_cmdlist_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B W-L2 chip gate for omega_gpu_cmdlist (many launches in one submission) + the decode glue kernels.
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_gpu_cmdlist_chip.sh EVID
#   0. host proofs first (never hardware results): --host-only, --sim
#   1. main: baseline (session launch loop) vs one list (PCAS, INLINE) at L = 64, 289, 1024, bit-identical, negative
#      control (dropped launch caught), no-barrier control (informational), glue kernels in one list + mutants caught
#   2. one variable, informational: --spin-us 0 (platform poll only)
# Verdict line last: "GPU_CMDLIST_CHIP: PASS|FAIL".
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
make PHYSICS_DIR="$PHYS" -j10 build/gpu_cmdlist_test > "$EVID/build.log" 2>&1 || { echo "GPU_CMDLIST_CHIP: FAIL build"; exit 1; }
T=./build/gpu_cmdlist_test
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps "${REPS:-20}" --out "$EVID/receipt-main.json" 2>&1 | tee "$EVID/main.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps "${REPS:-20}" --spin-us 0 --out "$EVID/receipt-spin0.json" 2>&1 | tee "$EVID/spin0.log"; echo "spin-us 0 exit ${PIPESTATUS[0]} (informational)" | tee -a "$EVID/spin0.log"
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log device.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_CMDLIST_CHIP: PASS"; else echo "GPU_CMDLIST_CHIP: FAIL"; exit 1; fi
