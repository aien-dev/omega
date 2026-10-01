#!/bin/bash
# tests/run_reduce_chip.sh -- E1 WP-D reduction chip parity run (GB10).
#
#   tests/run_reduce_chip.sh --physics-dir DIR [--evidence-dir DIR] [--quiet-flag]
#
# 1. The omega tree (this repo) must be clean; the run commit is its HEAD.
#    The physics checkout must be clean and at the commit in physics.lock.
# 2. Builds the chip test binary (tests/test_omega_reduce.c with the GB10
#    executor) with -ffp-contract=off into build/reduce-runs/<run-id>/ and
#    refuses a binary with libm math or CUDA symbols.
# 3. Runs it under /tmp/aien-gb10.lock (waits for the lock; never times out
#    or kills the chip test). --quiet-flag also creates ~/workspace/.spark-quiet
#    (only when no flag exists) and removes it afterwards.
# 4. PASS needs exit status 0, "RED_GB10_PARITY_<OP>: PASS" for each of
#    SUM, MAX, MIN and MEAN, "RED_GB10_PARITY: PASS" and "E1 Reduce Verdict:
#    PASS" in the log, and a clean tree afterwards. MEAN is chip SUM levels
#    plus one declared host division (omega_math_div); the receipt says so.
# 5. Writes a content-addressed receipt <evidence-dir>/<sha256>.json (mode
#    0444, never overwritten) for PASS and FAIL alike, plus the log and binary
#    as blobs/<sha256>.{log,bin}. The evidence dir must lie outside the tree.
# Shell + coreutils + git + jq + gcc. No Python. Exit 0 PASS, 1 FAIL, 2 refused.
set -u
SELF_DIR=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OMEGA=$(cd "$SELF_DIR/.." && pwd)
PHYS=""
EVID=$HOME/workspace/evidence-out/E1-REDUCE
QUIET=0
FLAG=$HOME/workspace/.spark-quiet
FLAG_OWNER="lane2 E1 WP-D reduce chip run"
while [ $# -gt 0 ]; do
    case $1 in
        --physics-dir) PHYS=$2; shift 2 ;;
        --evidence-dir) EVID=$2; shift 2 ;;
        --quiet-flag) QUIET=1; shift ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
die() { echo "run_reduce_chip: $*" >&2; exit 2; }
[ -n "$PHYS" ] && [ -d "$PHYS" ] || die "--physics-dir is required"
PHYS=$(cd "$PHYS" && pwd)
[ -z "$(git -C "$OMEGA" status --porcelain)" ] || die "omega tree is dirty; refusing"
COMMIT=$(git -C "$OMEGA" rev-parse HEAD)
LOCKED=$(tr -d ' \n' < "$OMEGA/physics.lock")
PHEAD=$(git -C "$PHYS" rev-parse HEAD)
[ "$PHEAD" = "$LOCKED" ] || die "physics checkout is at $PHEAD, physics.lock pins $LOCKED"
[ -z "$(git -C "$PHYS" status --porcelain)" ] || die "physics checkout is dirty"
mkdir -p "$EVID/blobs"
EVID=$(cd "$EVID" && pwd)
case "$EVID/" in "$OMEGA/"*|"$PHYS/"*) die "evidence dir must be outside the omega tree and physics checkout" ;; esac

RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-${COMMIT:0:12}
OUT=$OMEGA/build/reduce-runs/$RUN_ID
mkdir -p "$OUT"
BIN=$OUT/test_omega_reduce_gb10
NV=$PHYS/third_party/nvidia-open-580.173.02
FAIL_REASON=""
(cd "$OMEGA" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
    -Isrc -I"$PHYS/forge" -I"$PHYS/nvrm" -I"$PHYS/m16" \
    -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
    -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
    -o "$BIN" tests/test_omega_reduce.c src/omega_numeric_reduce.c src/omega_numeric_reduce_gb10.c \
    src/omega_numeric.c src/omega_numeric_gb10.c src/omega_numeric_divsqrt_gb10.c \
    src/omega_numeric_provenance.c src/omega_blackwell_codegen.c \
    src/omega_blackwell_encoder.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c \
    src/forge_realization.c src/aegis_verification.c src/sha256.c \
    "$PHYS/forge/forge_descriptor.c" "$PHYS/forge/forge_realize.c" "$PHYS/sha256_clean.c" \
    "$PHYS/nvrm/nvrm.c" "$PHYS/m16/m16_native.c") > "$OUT/build.log" 2>&1 || FAIL_REASON="build failed"
if [ -z "$FAIL_REASON" ]; then
    nm -u "$BIN" | grep -Eq '\b(sqrtf?|expf?|logf?|powf?|fmaf?|sinf?|cosf?|roundf?|fabsf?)\b' && FAIL_REASON="libm symbols in binary"
    nm -u "$BIN" | grep -Eq 'libcuda(rt)?\.so|\b(cuInit|cuCtx|cuMem|cuStream|cudaMalloc)\b' && FAIL_REASON="CUDA symbols in binary"
fi
STATUS=-1
if [ -z "$FAIL_REASON" ]; then
    CREATED_FLAG=0
    if [ "$QUIET" = 1 ]; then
        # wait until no other flag exists, then create ours atomically
        until (set -o noclobber; echo "$FLAG_OWNER" > "$FLAG") 2>/dev/null; do sleep 60; done
        # remove our flag (never anyone else's) however this script ends
        trap '[ "$(cat "$FLAG" 2>/dev/null)" = "$FLAG_OWNER" ] && rm -f "$FLAG"' EXIT
        CREATED_FLAG=1
    fi
    flock /tmp/aien-gb10.lock "$BIN" > "$OUT/reduce.log" 2> "$OUT/reduce.stderr"
    STATUS=$?
    if [ "$CREATED_FLAG" = 1 ] && [ "$(cat "$FLAG" 2>/dev/null)" = "$FLAG_OWNER" ]; then rm -f "$FLAG"; fi
    echo "$STATUS" > "$OUT/reduce.status"
    [ "$STATUS" = 0 ] || FAIL_REASON="test binary exit status $STATUS"
    for op in SUM MAX MIN MEAN; do
        grep -q "^RED_GB10_PARITY_${op}: PASS" "$OUT/reduce.log" || FAIL_REASON="${FAIL_REASON:-no RED_GB10_PARITY_${op} PASS line}"
    done
    grep -q '^RED_GB10_PARITY: PASS' "$OUT/reduce.log" || FAIL_REASON="${FAIL_REASON:-no RED_GB10_PARITY PASS line}"
    grep -qx 'E1 Reduce Verdict: PASS' "$OUT/reduce.log" || FAIL_REASON="${FAIL_REASON:-verdict line is not PASS}"
fi
[ -z "$(git -C "$OMEGA" status --porcelain)" ] && CLEAN_AFTER=true || { CLEAN_AFTER=false; FAIL_REASON="${FAIL_REASON:-tree dirty after run}"; }
[ "$(git -C "$OMEGA" rev-parse HEAD)" = "$COMMIT" ] || FAIL_REASON="${FAIL_REASON:-HEAD moved during run}"

BIN_SHA=""; LOG_SHA=""
if [ -f "$BIN" ]; then BIN_SHA=$(sha256sum "$BIN" | cut -d' ' -f1); [ -n "$BIN_SHA" ] || die "cannot hash binary"; [ -e "$EVID/blobs/$BIN_SHA.bin" ] || cp "$BIN" "$EVID/blobs/$BIN_SHA.bin" || die "cannot store binary blob"; chmod 0444 "$EVID/blobs/$BIN_SHA.bin" || die "cannot seal binary blob"; fi
if [ -f "$OUT/reduce.log" ]; then LOG_SHA=$(sha256sum "$OUT/reduce.log" | cut -d' ' -f1); [ -n "$LOG_SHA" ] || die "cannot hash log"; [ -e "$EVID/blobs/$LOG_SHA.log" ] || cp "$OUT/reduce.log" "$EVID/blobs/$LOG_SHA.log" || die "cannot store log blob"; chmod 0444 "$EVID/blobs/$LOG_SHA.log" || die "cannot seal log blob"; fi
VERDICT=PASS; [ -n "$FAIL_REASON" ] && VERDICT=FAIL
PARITY=$(grep '^RED_GB10_PARITY:' "$OUT/reduce.log" 2>/dev/null | head -1)
PSUM=$(grep '^RED_GB10_PARITY_SUM:' "$OUT/reduce.log" 2>/dev/null | head -1)
PMAX=$(grep '^RED_GB10_PARITY_MAX:' "$OUT/reduce.log" 2>/dev/null | head -1)
PMIN=$(grep '^RED_GB10_PARITY_MIN:' "$OUT/reduce.log" 2>/dev/null | head -1)
PMEAN=$(grep '^RED_GB10_PARITY_MEAN:' "$OUT/reduce.log" 2>/dev/null | head -1)
TMP=$OUT/receipt.json
jq -n --arg suite E1_REDUCE_GB10_PARITY --arg status "$VERDICT" --arg reason "$FAIL_REASON" \
    --arg run_id "$RUN_ID" --arg commit "$COMMIT" --arg physics "$PHEAD" --argjson clean_after "$CLEAN_AFTER" \
    --arg order "RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL" \
    --arg bin "$BIN_SHA" --arg log "$LOG_SHA" --arg parity "$PARITY" --arg psum "$PSUM" --arg pmax "$PMAX" --arg pmin "$PMIN" --arg pmean "$PMEAN" --arg exit_status "$STATUS" \
    --arg host "$(uname -n)" --arg kernel "$(uname -r)" \
    '{suite:$suite,status:$status,reason:$reason,run_id:$run_id,omega_commit:$commit,omega_clean_before:true,
      omega_clean_after:$clean_after,physics_commit:$physics,declared_order:$order,binary_sha256:$bin,
      log_sha256:$log,exit_status:$exit_status,parity_line:$parity,
      ops:["SUM","MAX","MIN","MEAN"],parity_by_op:{SUM:$psum,MAX:$pmax,MIN:$pmin,MEAN:$pmean},
      chip_kernels:{SUM:"REDUCE_SUM (WP-C patch, SHFL.DOWN+FADD)",MAX:"reduce minmax patch SHFL.DOWN+FMNMX !PT",MIN:"reduce minmax patch SHFL.DOWN+FMNMX PT",MEAN:"chip SUM levels"},
      mean_final_division:"HOST_DECLARED_STEP omega_math_div(sum,u2f(n)); GB10 DIV kernel (omega#141) not merged",host:$host,kernel:$kernel}' > "$TMP" || die "receipt json (jq) failed, no receipt written"
[ -s "$TMP" ] || die "empty receipt, not written"
[ "$VERDICT" != PASS ] || { [ -n "$BIN_SHA" ] && [ -n "$LOG_SHA" ]; } || die "PASS without binary and log digests refused"
DIG=$(sha256sum "$TMP" | cut -d' ' -f1)
(set -o noclobber; cat "$TMP" > "$EVID/$DIG.json") || die "receipt $DIG.json already exists"
chmod 0444 "$EVID/$DIG.json"
echo "receipt: $EVID/$DIG.json status=$VERDICT ${FAIL_REASON}"
[ "$VERDICT" = PASS ]
