#!/bin/bash
# tools/run_reduce_stall_probe.sh -- E1 stall probe: does the GB10 reduce launch's last release write
# (marker / marker2) reach the CPU late because it sits in GPU L2 (H1) or because the method is not scheduled (H2)?
#
#   tools/run_reduce_stall_probe.sh --physics-dir DIR [--run]
#
# Builds the reduce chip test binary (same build line as tests/run_reduce_chip.sh) in two arms:
#   A: -DOMEGA_STALL_PROBE                             (marker_mem cached in GPU L2, as today)
#   B: -DOMEGA_STALL_PROBE -DOMEGA_MARKER_UNCACHED     (marker_mem allocated with nvrm_alloc_gpu_uncached)
# Without --run it only builds (host only, never opens the GPU). With --run it then runs arm A, then arm B,
# each under flock /tmp/aien-gb10.lock (waits for the lock, never times out or kills the chip test), with
# OMEGA_PROBE_WAIT_MS=5000 so a stall costs 5 s instead of 10 minutes, and writes stdout/stderr per arm to
# ~/workspace/evidence-out/E1-STALL-PROBE/<UTC>/ and prints a summary per arm:
#   launches, median / p99 / max marker2_ms, count of marker2_ms > 1000, timeouts.
# The physics checkout must be at the commit in physics.lock. The omega tree may be dirty (a probe, not a receipt).
# Shell + coreutils + git + gcc + awk. No Python. Exit 0 on success, 2 on refusal or build failure.
set -u
SELF_DIR=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OMEGA=$(cd "$SELF_DIR/.." && pwd)
PHYS=""
DO_RUN=0
EVROOT=$HOME/workspace/evidence-out/E1-STALL-PROBE
while [ $# -gt 0 ]; do
    case $1 in
        --physics-dir) PHYS=$2; shift 2 ;;
        --run) DO_RUN=1; shift ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
die() { echo "run_reduce_stall_probe: $*" >&2; exit 2; }
[ -n "$PHYS" ] && [ -d "$PHYS" ] || die "--physics-dir is required"
PHYS=$(cd "$PHYS" && pwd)
COMMIT=$(git -C "$OMEGA" rev-parse HEAD)
LOCKED=$(tr -d ' \n' < "$OMEGA/physics.lock")
PHEAD=$(git -C "$PHYS" rev-parse HEAD)
[ "$PHEAD" = "$LOCKED" ] || die "physics checkout is at $PHEAD, physics.lock pins $LOCKED"
[ -z "$(git -C "$PHYS" status --porcelain)" ] || die "physics checkout is dirty"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OMEGA/build/stall-probe" || die "cannot create build/stall-probe"
OUT=$(mktemp -d "$OMEGA/build/stall-probe/$STAMP-${COMMIT:0:12}.XXXXXX") || die "cannot create a build directory"
NV=$PHYS/third_party/nvidia-open-580.173.02

build_arm() { # build_arm NAME DEFINES...
    local name=$1; shift
    (cd "$OMEGA" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off "$@" \
        -Isrc -I"$PHYS/forge" -I"$PHYS/nvrm" -I"$PHYS/m16" \
        -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
        -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
        -o "$OUT/test_omega_reduce_gb10_$name" tests/test_omega_reduce.c src/omega_numeric_reduce.c src/omega_numeric_reduce_gb10.c \
        src/omega_numeric.c src/omega_numeric_gb10.c src/omega_numeric_divsqrt_gb10.c \
        src/omega_numeric_provenance.c src/omega_blackwell_codegen.c \
        src/omega_blackwell_encoder.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c \
        src/forge_realization.c src/aegis_verification.c src/sha256.c \
        "$PHYS/forge/forge_descriptor.c" "$PHYS/forge/forge_realize.c" "$PHYS/sha256_clean.c" \
        "$PHYS/nvrm/nvrm.c" "$PHYS/m16/m16_native.c") > "$OUT/build_$name.log" 2>&1 \
        || { cat "$OUT/build_$name.log" >&2; die "arm $name build failed"; }
    nm -u "$OUT/test_omega_reduce_gb10_$name" | grep -Eq 'libcuda(rt)?\.so|\b(cuInit|cuCtx|cuMem|cuStream|cudaMalloc)\b' \
        && die "CUDA symbols in arm $name binary"
    echo "built arm $name: $OUT/test_omega_reduce_gb10_$name"
}
build_arm A -DOMEGA_STALL_PROBE
build_arm B -DOMEGA_STALL_PROBE -DOMEGA_MARKER_UNCACHED
if [ "$DO_RUN" != 1 ]; then
    echo "build only (no --run): nothing was run on the chip. binaries in $OUT"
    exit 0
fi

EVID=$EVROOT/$STAMP
mkdir -p "$EVID" || die "cannot create $EVID"
{ echo "omega_commit=$COMMIT"; echo "physics_commit=$PHEAD"; echo "omega_dirty=$([ -n "$(git -C "$OMEGA" status --porcelain)" ] && echo yes || echo no)"; echo "host=$(uname -n) kernel=$(uname -r)"; echo "probe_wait_ms=5000"; } > "$EVID/meta.txt"

summarize() { # summarize ARM
    local arm=$1 err=$EVID/arm_$1.stderr
    local nl=$(grep -c '^GB10_PROBE n=' "$err")
    local tmo=$(grep -c '^GB10_PROBE_TIMEOUT' "$err")
    local rf=$(grep -c '^GB10_REDUCE_FAIL' "$err")
    local st=$(cat "$EVID/arm_$arm.status")
    grep '^GB10_PROBE n=' "$err" | sed -n 's/.* marker2_ms=\([-0-9.]*\) .*/\1/p' | sort -g > "$EVID/arm_$arm.marker2_ms.sorted"
    awk -v arm="$arm" -v nl="$nl" -v tmo="$tmo" -v rf="$rf" -v st="$st" '
        { v[NR]=$1; if ($1>1000) slow++ }
        END {
            if (NR==0) { printf "arm %s: exit=%s launches=%d marker2 samples=0 timeouts=%d reduce_fail_lines=%d\n", arm, st, nl, tmo, rf; exit }
            m=(NR%2)?v[(NR+1)/2]:(v[NR/2]+v[NR/2+1])/2
            p=int(0.99*NR); if (p<0.99*NR) p++; if (p<1) p=1
            printf "arm %s: exit=%s launches=%d median_marker2_ms=%.3f p99_marker2_ms=%.3f max_marker2_ms=%.3f count_gt_1000ms=%d timeouts=%d reduce_fail_lines=%d\n", arm, st, nl, m, v[p], v[NR], slow+0, tmo, rf
        }' "$EVID/arm_$arm.marker2_ms.sorted"
}

for arm in A B; do
    echo "running arm $arm (waits for /tmp/aien-gb10.lock; never killed)"
    OMEGA_PROBE_WAIT_MS=5000 flock /tmp/aien-gb10.lock "$OUT/test_omega_reduce_gb10_$arm" \
        > "$EVID/arm_$arm.stdout" 2> "$EVID/arm_$arm.stderr"
    echo $? > "$EVID/arm_$arm.status"
done
{ summarize A; summarize B; } | tee "$EVID/summary.txt"
echo "evidence: $EVID"
