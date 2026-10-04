#!/bin/sh
# tools/run_gpu_attention_chip.sh [OUT_ROOT]
# FB-1 cut 5 chip gate: gqa_attention (f32 KV) and paged_attention (+ batch, bf16 KV)
# for the TinyLlama (32q/4kv) and Llama-3.2-1B (32q/8kv) head shapes at context
# 1, 17, 256 and 2048, each against the host oracle (the ReferenceCpuBackend math),
# then against every mutant kernel (NO_MAX, KV_HEAD, SLOT, NO_RESCALE, Q_ROW, OUT_ROW), with a
# negative control and a cache-hit repeat. Sealed receipt folder
# OUT_ROOT/FB1-CUT5-<sha>/ (default ~/workspace/evidence-out):
#   receipt.json  machine-readable verdict per case      run.log    test output
#   device.txt    kernel, driver, omega and physics shas  build.log  compiler output
#   sim.log       the same battery through the host IR simulator, run first
#   copies of the three source files the verdict depends on, SHA256SUMS over all
# Run it through the heavy queue only (lanes.sh queue --heavy); never kill it.
set -u
cd "$(dirname "$0")/.." || exit 2
P=${PHYSICS_DIR:-../physics}
SHA=$(git rev-parse --short HEAD)
OUT=${1:-$HOME/workspace/evidence-out}/FB1-CUT5-$SHA
mkdir -p "$OUT"
if ! make PHYSICS_DIR="$P" build/gpu_attention_test > "$OUT/build.log" 2>&1; then
    echo "FB1-CUT5 BUILD FAIL (see $OUT/build.log)"; exit 1
fi
{
    uname -a
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || echo "nvidia-smi: unavailable"
    echo "omega $(git rev-parse HEAD) ($(git status --porcelain | wc -l) uncommitted paths)"
    echo "physics $(git -C "$P" rev-parse HEAD)"
    date -u +%FT%TZ
} > "$OUT/device.txt"
# host simulator first: a kernel that is wrong on the host never gets chip time
./build/gpu_attention_test --sim > "$OUT/sim.log" 2>&1
sim_rc=$?
if [ "$sim_rc" -ne 0 ]; then
    echo "FB1-CUT5 SIMULATOR FAIL rc=$sim_rc (see $OUT/sim.log)"; exit 1
fi
./build/gpu_attention_test --out "$OUT/receipt.json" > "$OUT/run.log" 2>&1
rc=$?
cp src/omega_gpu_attention_api.c src/omega_gpu_attention_api.h tests/gpu_attention_test.c "$OUT/"
(cd "$OUT" && sha256sum -- * > SHA256SUMS)
tail -1 "$OUT/run.log"
echo "FB1-CUT5 chip gate rc=$rc receipt=$OUT/receipt.json"
exit $rc
