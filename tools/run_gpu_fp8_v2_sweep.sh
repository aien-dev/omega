#!/bin/bash
# run_gpu_fp8_v2_sweep.sh EVIDENCE_DIR: one-variable chip sweep of the FP8 GEMV on the production shapes (kernel-only,
# under the GB10 lock), variant 1 controls first and last in the same run. Each line: label, then the test flags.
set -u
EVID="${1:?evidence dir}"; HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE" || exit 2; mkdir -p "$EVID"
{ echo "start $(date -u +%FT%TZ) omega=$(git rev-parse HEAD) dirty=$(git status --porcelain | wc -l)"; uptime; } > "$EVID/device.txt"
T=./build/gpu_fp8_api_test
run() { local label=$1; shift; for pat in "8 experts" "o_proj" "qkv fused"; do
  $T --reps 20 --only "$pat" --out "$EVID/receipt-$label-${pat// /_}.json" "$@" 2>&1 | grep -E "^chip .*variant=|FAIL|latched" | sed "s/^/[$label] /"; done; }
run v1-control
run v2-default --variant 2
run v2-w8 --variant 2 --warps 8
run v2-w8-b24 --variant 2 --warps 8 --cta-budget 24
run v2-w8-b48 --variant 2 --warps 8 --cta-budget 48
run v2-r2-w8 --variant 2 --rows 2 --warps 8
run v2-r4-w8 --variant 2 --rows 4 --warps 8
run v1-r2 --rows 2
run v1-r4 --rows 4
run v1-w16 --warps 16
run v1-control-2
echo "end $(date -u +%FT%TZ)" >> "$EVID/device.txt"
(cd "$EVID" && sha256sum *.json *.txt > SHA256SUMS)
echo "GPU_FP8_V2_SWEEP: DONE"
