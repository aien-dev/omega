#!/bin/bash
# run_numeric_ldst_chip.sh -- E1 row 2 chip run: every spec of the load/store table
# (tests/test_omega_ldst_gb10.c) executed on the GB10 and compared byte for byte with the host model.
# Meant to run only from the heavy forge lane (which serializes chip work); it also takes
# /tmp/aien-gb10.lock. It never raises the quiet flag and never kills or times out the run.
# A FAIL whose RESULT lines show "unwritten" > 0 (bytes still holding the fill pattern where the
# host model wrote) is the C3 unwritten-output signature and must be classified as C3.
# Usage: PHYSICS_DIR=<physics checkout at physics.lock> tools/run_numeric_ldst_chip.sh
# Shell + gcc only. No Python.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
PHYSICS=${PHYSICS_DIR:-$HOME/workspace/hive-worktrees/physics-pin-e95e3ed}
QUIET=$HOME/workspace/.spark-quiet
refuse() { echo "REFUSED: $*"; echo "VERDICT NOT_RUN"; exit 1; }
[ -z "$(git -C "$HERE" status --porcelain)" ] || refuse "omega tree $HERE is dirty"
PIN=$(tr -d '[:space:]' < "$HERE/physics.lock")
[ "$(git -C "$PHYSICS" rev-parse HEAD 2>/dev/null)" = "$PIN" ] || refuse "physics checkout $PHYSICS is not at physics.lock pin $PIN"
[ -e "$QUIET" ] && refuse "quiet flag is up: $(cat "$QUIET")"
[ -n "$(pgrep est_load)" ] && refuse "an est_load process is running"
RUN=$(mktemp -d /tmp/ldst-chip.XXXXXX)
echo "omega $(git -C "$HERE" rev-parse HEAD) physics $PIN"
echo "== host tier"
make -s -C "$HERE" build/test_omega_ldst_gb10_cpu > "$RUN/host-build.log" 2>&1 || refuse "host build failed"
"$HERE/build/test_omega_ldst_gb10_cpu" > "$RUN/host.log" 2>&1 || { tail -5 "$RUN/host.log"; refuse "host tier failed"; }
tail -2 "$RUN/host.log"
echo "== nvdisasm provenance"
"$HERE/tools/ldst_nvdisasm_check.sh" > "$RUN/nvdisasm.log" 2>&1 || { cat "$RUN/nvdisasm.log"; refuse "nvdisasm check failed"; }
tail -3 "$RUN/nvdisasm.log"
exec 9> /tmp/aien-gb10.lock || refuse "cannot open /tmp/aien-gb10.lock"
flock -x 9 || refuse "cannot take /tmp/aien-gb10.lock"
echo "== chip build"
NV=$PHYSICS/third_party/nvidia-open-580.173.02
BIN=$RUN/test_omega_ldst_gb10
(cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -pthread \
    -Isrc -I"$PHYSICS/nvrm" -I"$PHYSICS/m16" \
    -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
    -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
    -o "$BIN" tests/test_omega_ldst_gb10.c src/omega_numeric_ldst_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric.c \
    src/omega_numeric_provenance.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c \
    src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c \
    "$PHYSICS/nvrm/nvrm.c" "$PHYSICS/m16/m16_native.c") > "$RUN/chip-build.log" 2>&1 || { cat "$RUN/chip-build.log"; refuse "chip build failed"; }
echo "== chip run (not killed, not timed out)"
"$BIN" --chip > "$RUN/chip.log" 2>&1; RC=$?
exec 9>&-
grep -c '^RESULT chip .*verdict=PASS' "$RUN/chip.log" | sed 's/^/specs passed: /'
grep '^RESULT chip .*verdict=FAIL' "$RUN/chip.log" | head -20
UNW=$(sed -n 's/.* unwritten=\([0-9]*\) .*/\1/p' "$RUN/chip.log" | awk '{s+=$1} END {print s+0}')
[ "$UNW" = 0 ] || echo "NOTE: $UNW unwritten bytes in total: C3 signature (unwritten output), classify as C3 not as a load/store bug"
grep '^VERDICT' "$RUN/chip.log" | tail -1
echo "log: $RUN/chip.log"
[ "$RC" = 0 ]
