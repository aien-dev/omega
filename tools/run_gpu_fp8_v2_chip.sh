#!/bin/bash
# run_gpu_fp8_v2_chip.sh EVIDENCE_DIR [PHYSICS_DIR]
# OWN-30B W-L2c chip gate for the FP8 GEMV variant 2 (omega_blackwell_codegen_moe2.c), run as a whole under the GB10 lock:
#   flock /tmp/aien-gb10.lock tools/run_gpu_fp8_v2_chip.sh EVID
#   0. host proofs (never hardware results): --host-only (v2 budgets, register file, listing) and --sim (v2 bit-identical to v1, mutants)
#   1. baseline: the fp8 battery on variant 1 (parity, mutants, GB/s per shape)        -> receipt-v1.json
#   2. the same battery on variant 2 (--variant 2): parity, chip mutants, GB/s          -> receipt-v2.json
#   3. decode graph (synthetic 2 layers, real shapes): v1, v2, v1, v2 interleaved, REPS 5, timeline per kind + logits hash.
#      The logits hash must be the same on every run (bit-identical token and logits on the chip).
# Verdict line last: "GPU_FP8_V2_CHIP: PASS|FAIL". Steps 0, 2 and the hash equality decide; GB/s are recorded.
set -u
EVID="${1:?evidence dir}"; PHYS="${2:-$HOME/workspace/physics-own30b-k}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$EVID"
cd "$HERE" || exit 2
{
  echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) branch=$(git rev-parse --abbrev-ref HEAD) dirty=$(git status --porcelain | wc -l) physics=$PHYS physics_commit=$(git -C "$PHYS" rev-parse HEAD)"
  uname -a; nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null; free -g | head -2; uptime
} > "$EVID/device.txt"
make PHYSICS_DIR="$PHYS" -j10 build/gpu_fp8_api_test build/gpu_decode_graph_test > "$EVID/build.log" 2>&1 || { echo "GPU_FP8_V2_CHIP: FAIL build"; exit 1; }
T=./build/gpu_fp8_api_test; D=./build/gpu_decode_graph_test
fail=0
$T --host-only 2>&1 | tee "$EVID/host-only.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --sim 2>&1 | tee "$EVID/sim.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
$T --reps 50 --out "$EVID/receipt-v1.json" 2>&1 | tee "$EVID/v1.log"; echo "v1 exit ${PIPESTATUS[0]} (baseline)" | tee -a "$EVID/v1.log"
$T --reps 50 --variant 2 --out "$EVID/receipt-v2.json" 2>&1 | tee "$EVID/v2.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
for r in 1 2; do
  for v in 1 2; do
    $D --reps 5 --variant $v --out "$EVID/receipt-graph-v$v-r$r.json" 2>&1 | tee "$EVID/graph-v$v-r$r.log"; [ "${PIPESTATUS[0]}" = 0 ] || fail=1
  done
done
hashes=$(grep -ho '"C0_logits_fnv":"[0-9a-f]*"' "$EVID"/receipt-graph-*.json | sort -u | wc -l)
echo "logits hashes across the 4 graph runs: $hashes distinct (must be 1)" | tee "$EVID/hash.txt"
[ "$hashes" = 1 ] || fail=1
grep -h "TIMELINE\|fp8 knobs" "$EVID"/graph-v*.log | tee "$EVID/timelines.txt"
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.log *.txt > SHA256SUMS)
if [ $fail = 0 ]; then echo "GPU_FP8_V2_CHIP: PASS"; else echo "GPU_FP8_V2_CHIP: FAIL"; exit 1; fi
