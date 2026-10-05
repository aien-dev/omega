#!/bin/sh
# tools/run_gpu_cut4b_chip.sh [OUT_ROOT] [PROBE_N]
# FB-1 cut 4b chip gate: the three native GPU APIs on ONE persistent session.
#   1. elementwise parity battery (cut 4, unchanged cases) + --timing
#      (rmsnorm 1x2048 and swiglu 5632: median of 100 calls under 0.5 ms, one open)
#   2. attention: host simulator first, then the chip parity battery (cut 5, unchanged
#      cases) + --timing (1 query x 32 heads x 2048 ctx: median of 100 calls under 3 ms)
#   3. matmul sweep + --timing (cut 1b regression on the shared session)
#   4. fresh-process device-open probe (flake investigation, tools/probe_gpu_open.sh)
# Sealed receipt folder OUT_ROOT/FB1-CUT4B-<sha>/ (default ~/workspace/evidence-out):
#   elementwise.json elementwise_timing.json attention.json attention_timing.json
#   matmul.json matmul_timing.json  + the matching *.log, probe/ (probe.log, failures.txt),
#   device.txt build.log, copies of the sources the verdict depends on, SHA256SUMS.
# Every step runs even when an earlier one fails (the receipt records all of them).
# Run it through the heavy queue only (lanes.sh queue --heavy); never kill it.
set -u
cd "$(dirname "$0")/.." || exit 2
P=${PHYSICS_DIR:-../physics}
SHA=$(git rev-parse --short HEAD)
OUT=${1:-$HOME/workspace/evidence-out}/FB1-CUT4B-$SHA
PROBE_N=${2:-30}
mkdir -p "$OUT"
if ! make PHYSICS_DIR="$P" build/gpu_elementwise_test build/gpu_attention_test build/gpu_matmul_api_test build/gpu_session_probe > "$OUT/build.log" 2>&1; then
    echo "FB1-CUT4B BUILD FAIL (see $OUT/build.log)"; exit 1
fi
{
    uname -a
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || echo "nvidia-smi: unavailable"
    echo "omega $(git rev-parse HEAD) ($(git status --porcelain | wc -l) uncommitted paths)"
    echo "physics $(git -C "$P" rev-parse HEAD)"
    date -u +%FT%TZ
} > "$OUT/device.txt"
fail=0
step() { # name command...
    name=$1; shift
    "$@" > "$OUT/$name.log" 2>&1; rc=$?
    echo "$name rc=$rc :: $(grep -E '^(PASS|FAIL)' "$OUT/$name.log" | tail -1)"
    [ "$rc" -eq 0 ] || fail=$((fail+1))
}
step elementwise        ./build/gpu_elementwise_test --out "$OUT/elementwise.json"
step elementwise_timing ./build/gpu_elementwise_test --timing --out "$OUT/elementwise_timing.json"
step attention_sim      ./build/gpu_attention_test --sim
step attention          ./build/gpu_attention_test --out "$OUT/attention.json"
step attention_timing   ./build/gpu_attention_test --timing --out "$OUT/attention_timing.json"
step matmul             ./build/gpu_matmul_api_test --out "$OUT/matmul.json"
step matmul_timing      ./build/gpu_matmul_api_test --timing --out "$OUT/matmul_timing.json"
./tools/probe_gpu_open.sh "$OUT/probe" "$PROBE_N" > "$OUT/probe.log" 2>&1; prc=$?
tail -1 "$OUT/probe.log"
echo "end $(date -u +%FT%TZ)" >> "$OUT/device.txt"
cp src/omega_gpu_session.c src/omega_gpu_session.h src/omega_gpu_matmul_api.c src/omega_gpu_elementwise_api.c src/omega_gpu_elementwise_api.h \
   src/omega_gpu_attention_api.c src/omega_gpu_attention_api.h tests/gpu_elementwise_test.c tests/gpu_attention_test.c tests/gpu_matmul_api_test.c "$OUT/"
(cd "$OUT" && sha256sum -- * probe/* 2>/dev/null > SHA256SUMS)
echo "FB1-CUT4B chip gate failed_steps=$fail probe_rc=$prc receipt=$OUT"
[ "$fail" -eq 0 ] && [ "$prc" -ne 3 ]
