#!/bin/bash
# run_l1g_chip.sh EVIDENCE_DIR [what...]  (W-L1G, OWN-30B L1 GEMM, 2026-10-14)
# what: qmma (QMMA arithmetic probe + peak, then a confirmation seed), pfgemm (GEMM v2 chip gate, 4 shapes at 3600 tokens),
# qgemm (native FP8 path: quantiser + QMMA GEMM chip gate, 4 shapes at 3600 tokens, then a wait-0 probe on gateup).
# GEMM v2 runs at rt 4 warps 16 aqn 1: rt 8 needs 32768 shared bytes, over the QMD cap of 16384. Default: qmma pfgemm. Kernel tests only, no model load.
# Caller holds the GB10 lock: flock /tmp/aien-gb10.lock tools/run_l1g_chip.sh EVID ...
set -u
EVID="${1:?evidence dir}"; shift; WHAT="${*:-qmma pfgemm}"
PHYS="${PHYS:-$HOME/workspace/physics-own30b-k}"; OUT="${OUT_DIR:-/tmp/wl1g-omega}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain --untracked-files=no | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD) what=$WHAT"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make OUT_DIR="$OUT" PHYSICS_DIR="$PHYS" -j10 "$OUT/gpu_qmma_probe_test" "$OUT/gpu_pfgemm_test" "$OUT/gpu_qgemm_test" > "$EVID/build.log" 2>&1 || { echo "L1G_CHIP: FAIL build"; exit 1; }
fail=0
run() { local name="$1"; shift; "$@" > "$EVID/$name.log" 2>&1; local rc=$?; echo "$name exit $rc: $*" >> "$EVID/exits.txt"; [ $rc = 0 ] || fail=1; tail -1 "$EVID/$name.log" | sed "s/^/$name: /"; }
for w in $WHAT; do case $w in
  qmma) run qmma-probe "$OUT/gpu_qmma_probe_test" --save "$EVID/qmma-words.bin"
        QMMA_PROBE_SEED=0x1234567 run qmma-probe-seed2 "$OUT/gpu_qmma_probe_test" --no-peak ;;
  pfgemm) run pfgemm-chip "$OUT/gpu_pfgemm_test" --shape-knob 4 16 1 --chip --tokens 3600 --reps 7 ;;
  qgemm) run qgemm-chip "$OUT/gpu_qgemm_test" --chip --tokens 3600 --reps 7
         run qgemm-cached "$OUT/gpu_qgemm_test" --chip --cached --tokens 3600 --reps 7
         for s in gateup down; do run qgemm-cached-wait0-$s "$OUT/gpu_qgemm_test" --chip --cached --shape $s --wait 0 --tokens 3600 --reps 7; done ;;
esac; done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum * 2>/dev/null | grep -v SHA256SUMS > SHA256SUMS)
if [ $fail = 0 ]; then echo "L1G_CHIP: PASS"; else echo "L1G_CHIP: FAIL"; exit 1; fi
