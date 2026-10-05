#!/bin/sh
# tools/run_gpu_elementwise_chip.sh [OUT_ROOT]
# FB-1 cut 4 chip gate (rmsnorm, rope, swiglu, EX2 and shared-exchange probes,
# each against the host oracle and then against its mutant kernel), with a
# sealed receipt folder OUT_ROOT/FB1-CUT4-<sha>/ (default ~/workspace/evidence-out):
#   receipt.json  machine-readable verdict per case      run.log    test output
#   device.txt    kernel, driver, omega and physics shas  build.log  compiler output
#   copies of the three source files the verdict depends on, SHA256SUMS over all
# Run it through the heavy queue only (lanes.sh queue --heavy); never kill it.
set -u
cd "$(dirname "$0")/.." || exit 2
P=${PHYSICS_DIR:-../physics}
SHA=$(git rev-parse --short HEAD)
OUT=${1:-$HOME/workspace/evidence-out}/FB1-CUT4-$SHA
mkdir -p "$OUT"
if ! make PHYSICS_DIR="$P" build/gpu_elementwise_test > "$OUT/build.log" 2>&1; then
    echo "FB1-CUT4 BUILD FAIL (see $OUT/build.log)"; exit 1
fi
{
    uname -a
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || echo "nvidia-smi: unavailable"
    echo "omega $(git rev-parse HEAD) ($(git status --porcelain | wc -l) uncommitted paths)"
    echo "physics $(git -C "$P" rev-parse HEAD)"
    date -u +%FT%TZ
} > "$OUT/device.txt"
./build/gpu_elementwise_test --out "$OUT/receipt.json" > "$OUT/run.log" 2>&1
rc=$?
cp src/omega_gpu_elementwise_api.c src/omega_gpu_elementwise_api.h tests/gpu_elementwise_test.c "$OUT/"
(cd "$OUT" && sha256sum -- * > SHA256SUMS)
tail -1 "$OUT/run.log"
echo "FB1-CUT4 chip gate rc=$rc receipt=$OUT/receipt.json"
exit $rc
