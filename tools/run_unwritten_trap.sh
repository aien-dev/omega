#!/bin/bash
# C3b: build and run the GB10 unwritten-output trap. Forge queue only; the chip
# run is never killed and never timed out. No Python.
#   tools/run_unwritten_trap.sh [harness args]   (default: --chip --repeats 3000)
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
PHYSICS=${PHYSICS:-$HOME/workspace/physics}
refuse() { echo "REFUSED: $*"; echo "OMEGA_UNWRITTEN_TRAP: NOT_RUN"; exit 2; }
[ -d "$PHYSICS/nvrm" ] || refuse "no physics checkout at $PHYSICS"
PIN=$(tr -d '[:space:]' < "$HERE/physics.lock")
[ "$(git -C "$PHYSICS" rev-parse HEAD)" = "$PIN" ] || refuse "physics HEAD is not the physics.lock pin $PIN"
NV=$PHYSICS/third_party/nvidia-open-580.173.02
BIN=$HERE/build/test_omega_unwritten_trap_gb10
mkdir -p "$HERE/build"
(cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-fast-math -pthread \
    -Isrc -I"$PHYSICS/nvrm" -I"$PHYSICS/m16" \
    -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
    -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
    -o "$BIN" tests/test_omega_unwritten_trap_gb10.c src/omega_unwritten_trap.c \
    src/omega_numeric_divsqrt_gb10.c src/omega_numeric_transc.c src/omega_numeric.c src/omega_numeric_provenance.c \
    src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c \
    src/sha256.c "$PHYSICS/nvrm/nvrm.c" "$PHYSICS/m16/m16_native.c") || refuse "chip build failed"
exec 9> /tmp/aien-gb10.lock || refuse "cannot open /tmp/aien-gb10.lock"
flock -x 9 || refuse "cannot take /tmp/aien-gb10.lock"
[ $# -gt 0 ] || set -- --chip --repeats 3000
"$BIN" "$@"
RC=$?
exec 9>&-
exit $RC
