#!/bin/bash
# run_gpu_tcgemm_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B T3 cut 1 chip gate: tensor-core FP8 grouped GEMM (omega_gpu_tcgemm_api). Per point: 5 independent process runs,
# each = upload, bit-identity vs the C emulation on sampled rows, second call identical, mutants 1-4 (run 1 only), medians of 9.
# Points: gate|up and down at 128 tokens (the FFMA baseline's M=1024, evidence/OWN30B-T3-BASELINE-*) and at 3600 tokens,
# dense qkv and o at 3600 tokens. Kernel only, no model load. Caller holds the lock: flock /tmp/aien-gb10.lock tools/run_gpu_tcgemm_chip.sh EVID
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"; OUT="${OUT_DIR:-/tmp/t3-omega}"
"$(dirname "$0")/require_physics_pin.sh" "$PHYS" || exit 2  # omega#456: refuse a Physics checkout other than physics.lock
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"; cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2
} > "$EVID/device.txt"
make OUT_DIR="$OUT" PHYSICS_DIR="$PHYS" -j10 "$OUT/gpu_tcgemm_test" > "$EVID/build.log" 2>&1 || { echo "TCGEMM_CHIP: FAIL build"; exit 1; }
T="$OUT/gpu_tcgemm_test"; fail=0
for pt in "gateup 128" "down 128" "gateup 3600" "down 3600" "qkv 3600" "o 3600"; do
  set -- $pt; shape=$1; tok=$2
  for r in 1 2 3 4 5; do
    extra="--no-mutants"; [ $r = 1 ] && extra=""
    $T --chip --shape $shape --tokens $tok --reps 9 --check-rows 256 $extra --out "$EVID/receipt-$shape-$tok-r$r.json" > "$EVID/$shape-$tok-r$r.log" 2>&1
    rc=$?; echo "$shape $tok r$r exit $rc" >> "$EVID/exits.txt"; [ $rc = 0 ] || fail=1
    grep -h "TCGEMM \|bit-identity\|mutant\|FAIL" "$EVID/$shape-$tok-r$r.log" | sed "s/^/$shape $tok r$r: /"
  done
done
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "TCGEMM_CHIP: PASS"; else echo "TCGEMM_CHIP: FAIL"; exit 1; fi
