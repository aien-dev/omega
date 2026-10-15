#!/bin/sh
# tools/run_gpu_reconv_chip.sh [OUT_ROOT] [-- extra gate args]
# omega #308 chip gate: structured warp reconvergence. Runs the host part first (encoder
# goldens, region bookkeeping, refusals, nvdisasm, every probe through the SIMT warp
# simulator, mutation controls); a kernel the simulator rejects never gets chip time. Then
# every probe (R3 diamond, R4 loop break, R5 nested, R6 lanes past the end + BAR/SHFL, 16-deep
# barrier bound) on the chip against the host oracle, bit-exact, then each math mutant must be
# caught. Sealed receipt folder OUT_ROOT/GPU-RECONV-<sha>/ (default ~/workspace/evidence-out):
#   receipt.json  machine-readable verdict per case      run.log    test output
#   device.txt    kernel, driver, omega and physics shas  build.log  compiler output
#   copies of the source files the verdict depends on, SHA256SUMS over all
# Pattern of tools/run_gpu_elementwise_chip.sh. Never creates or touches the quiet flag (the
# operator runs it inside an own quiet window) and never kills or times out the run.
# PHYSICS_DIR must be at the physics.lock commit (the Makefile refuses otherwise).
set -u
cd "$(dirname "$0")/.." || exit 2
P=${PHYSICS_DIR:-../physics}
OUT_ROOT=$HOME/workspace/evidence-out
EXTRA=""
if [ $# -gt 0 ] && [ "$1" != "--" ]; then OUT_ROOT=$1; shift; fi
if [ $# -gt 0 ] && [ "$1" = "--" ]; then shift; EXTRA="$*"; fi
SHA=$(git rev-parse --short HEAD)
OUT=$OUT_ROOT/GPU-RECONV-$SHA
mkdir -p "$OUT"
if ! make PHYSICS_DIR="$P" build/gpu_reconv_test > "$OUT/build.log" 2>&1; then
    echo "GPU-RECONV BUILD FAIL (see $OUT/build.log)"; exit 1
fi
{
    uname -a
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || echo "nvidia-smi: unavailable"
    echo "omega $(git rev-parse HEAD) ($(git status --porcelain | wc -l) uncommitted paths)"
    echo "physics $(git -C "$P" rev-parse HEAD)"
    date -u +%FT%TZ
} > "$OUT/device.txt"
# shellcheck disable=SC2086
./build/gpu_reconv_test --out "$OUT/receipt.json" $EXTRA > "$OUT/run.log" 2>&1
rc=$?
cp src/omega_gpu_elementwise_api.c src/omega_gpu_elementwise_api.h src/omega_bw_reconv.h src/omega_blackwell_codegen.c \
   tests/gpu_reconv_test.c tests/bw_warp_sim.h "$OUT/"
(cd "$OUT" && sha256sum -- * > SHA256SUMS)
tail -1 "$OUT/run.log"
echo "GPU-RECONV chip gate rc=$rc receipt=$OUT/receipt.json"
exit $rc
