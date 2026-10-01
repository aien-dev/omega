#!/bin/bash
# tests/run_tensor_chip.sh -- M20 OMEGA_TENSOR GB10 parity chip run.
#
#   GB10_CHIP_RUN=1 tests/run_tensor_chip.sh --physics-dir DIR [--evidence-dir DIR]
#
# Pattern of tests/run_reduce_chip.sh, without any quiet-flag option (this
# script never creates or touches ~/workspace/.spark-quiet).
# 1. Refuses unless GB10_CHIP_RUN=1. The omega tree (this repo) must be clean;
#    the run commit is its HEAD. The physics checkout must be clean and at the
#    commit in physics.lock.
# 2. Builds tests/test_omega_tensor_gb10.c with the CPU table, the GB10 table
#    and the E1 GB10 executors, -ffp-contract=off, into
#    build/tensor-runs/<run-id>/, and refuses a binary with libm math or CUDA
#    symbols.
# 3. Runs it with --chip under /tmp/aien-gb10.lock (waits for the lock; no
#    timeout; the chip test is never killed).
# 4. PASS needs exit status 0 and the lines "TENSOR_GB10_HOST: PASS",
#    "TENSOR_GB10_PARITY: PASS", "TENSOR_GB10_MUTANTS: PASS" and
#    "M20 Tensor GB10 Verdict: PASS", and clean trees with unmoved HEADs after.
# 5. Writes a content-addressed receipt <evidence-dir>/<sha256>.json (mode
#    0444, never overwritten) for PASS and FAIL alike, plus the log and binary
#    as blobs/<sha256>.{log,bin}, and the chip run stderr (GB10_CALL per-call
#    timing, GB10_DEVFAIL device step diagnostics) as blobs/<sha256>.stderr. The evidence dir must lie outside the trees.
# Shell + coreutils + git + jq + gcc. No Python. Exit 0 PASS, 1 FAIL, 2 refused.
set -u
SELF_DIR=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OMEGA=$(cd "$SELF_DIR/.." && pwd)
PHYS=""
EVID=$HOME/workspace/evidence-out/M20-TENSOR-GB10
while [ $# -gt 0 ]; do
    case $1 in
        --physics-dir) PHYS=$2; shift 2 ;;
        --evidence-dir) EVID=$2; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
die() { echo "run_tensor_chip: $*" >&2; exit 2; }
[ "${GB10_CHIP_RUN:-}" = 1 ] || die "chip run refused: GB10_CHIP_RUN=1 is not set"
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

STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-${COMMIT:0:12}
mkdir -p "$OMEGA/build/tensor-runs" || die "cannot create build/tensor-runs"
# one fresh directory per invocation, so concurrent runs never share files
OUT=$(mktemp -d "$OMEGA/build/tensor-runs/$RUN_ID.XXXXXX") || die "cannot create a run directory"
RUN_ID=$(basename "$OUT")
BIN=$OUT/test_omega_tensor_gb10
NV=$PHYS/third_party/nvidia-open-580.173.02
FAIL_REASON=""
(cd "$OMEGA" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -ffp-contract=off \
    -Isrc -Isrc/tensor -I"$PHYS/forge" -I"$PHYS/nvrm" -I"$PHYS/m16" \
    -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
    -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
    -o "$BIN" tests/test_omega_tensor_gb10.c src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c \
    src/tensor/omega_tensor_reduce_seam.c src/tensor/omega_tensor_gb10.c \
    src/omega_numeric_reduce.c src/omega_numeric_transc.c src/omega_numeric_reduce_gb10.c \
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
BIN_SHA_BUILT=""
[ -z "$FAIL_REASON" ] && { BIN_SHA_BUILT=$(sha256sum "$BIN" | cut -d' ' -f1); [ -n "$BIN_SHA_BUILT" ] || FAIL_REASON="cannot hash binary after build"; }
if [ -z "$FAIL_REASON" ]; then
    # waits for the lock; no timeout; never killed
    GB10_CHIP_RUN=1 flock /tmp/aien-gb10.lock "$BIN" --chip > "$OUT/tensor.log" 2> "$OUT/tensor.stderr"
    STATUS=$?
    echo "$STATUS" > "$OUT/tensor.status"
    [ "$STATUS" = 0 ] || FAIL_REASON="test binary exit status $STATUS"
    for k in TENSOR_GB10_HOST TENSOR_GB10_PARITY TENSOR_GB10_MUTANTS; do
        grep -q "^$k: PASS" "$OUT/tensor.log" || FAIL_REASON="${FAIL_REASON:-no $k PASS line}"
    done
    grep -qx 'M20 Tensor GB10 Verdict: PASS' "$OUT/tensor.log" || FAIL_REASON="${FAIL_REASON:-verdict line is not PASS}"
fi
FINISHED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
[ -z "$(git -C "$OMEGA" status --porcelain)" ] && CLEAN_AFTER=true || { CLEAN_AFTER=false; FAIL_REASON="${FAIL_REASON:-omega tree dirty after run}"; }
[ "$(git -C "$OMEGA" rev-parse HEAD)" = "$COMMIT" ] && HEAD_SAME=true || { HEAD_SAME=false; FAIL_REASON="${FAIL_REASON:-HEAD moved during run}"; }
[ -z "$(git -C "$PHYS" status --porcelain)" ] && PCLEAN_AFTER=true || { PCLEAN_AFTER=false; FAIL_REASON="${FAIL_REASON:-physics dirty after run}"; }
[ "$(git -C "$PHYS" rev-parse HEAD)" = "$PHEAD" ] && PHEAD_SAME=true || { PHEAD_SAME=false; FAIL_REASON="${FAIL_REASON:-physics HEAD moved during run}"; }

# store_blob SRC EXT -> prints the digest. An existing blob is trusted only if
# its content hashes to its name; a new one is copied to a temp name, checked,
# sealed and moved into place without overwriting.
store_blob() {
    local src=$1 ext=$2 d dst tmp have
    d=$(sha256sum "$src" | cut -d' ' -f1); [ -n "$d" ] || die "cannot hash $src"
    dst=$EVID/blobs/$d.$ext
    if [ -e "$dst" ]; then
        have=$(sha256sum "$dst" | cut -d' ' -f1)
        [ "$have" = "$d" ] || die "existing blob $dst does not match its digest"
    else
        tmp=$(mktemp "$EVID/blobs/.tmp.XXXXXX") || die "cannot create temp blob"
        cp "$src" "$tmp" || { rm -f "$tmp"; die "cannot store $ext blob"; }
        have=$(sha256sum "$tmp" | cut -d' ' -f1)
        [ "$have" = "$d" ] || { rm -f "$tmp"; die "stored $ext blob does not match its digest"; }
        chmod 0444 "$tmp" || { rm -f "$tmp"; die "cannot seal $ext blob"; }
        mv -n "$tmp" "$dst"; rm -f "$tmp"
        have=$(sha256sum "$dst" | cut -d' ' -f1)
        [ "$have" = "$d" ] || die "blob $dst does not match its digest after publish"
    fi
    echo "$d"
}
BIN_SHA=""; LOG_SHA=""; ERR_SHA=""
if [ -f "$BIN" ]; then BIN_SHA=$(store_blob "$BIN" bin) || exit 2; fi
[ -z "$BIN_SHA_BUILT" ] || [ "$BIN_SHA" = "$BIN_SHA_BUILT" ] || FAIL_REASON="${FAIL_REASON:-binary changed between build and receipt}"
if [ -f "$OUT/tensor.log" ]; then LOG_SHA=$(store_blob "$OUT/tensor.log" log) || exit 2; fi
if [ -f "$OUT/tensor.stderr" ]; then ERR_SHA=$(store_blob "$OUT/tensor.stderr" stderr) || exit 2; fi
DEVFAIL_N=0; FAILED_CALLS_N=0
if [ -f "$OUT/tensor.stderr" ]; then
    DEVFAIL_N=$(grep -c "^GB10_DEVFAIL " "$OUT/tensor.stderr")
    FAILED_CALLS_N=$(grep -c "^GB10_CALL .* FAILED$" "$OUT/tensor.stderr")
fi
VERDICT=PASS; [ -n "$FAIL_REASON" ] && VERDICT=FAIL
line() { grep "^$1" "$OUT/tensor.log" 2>/dev/null | head -1; }
TMP=$OUT/receipt.json
jq -n --arg suite M20_TENSOR_GB10_PARITY --arg status "$VERDICT" --arg reason "$FAIL_REASON" \
    --arg run_id "$RUN_ID" --arg commit "$COMMIT" --arg physics "$PHEAD" --arg pin "$LOCKED" \
    --argjson clean_after "$CLEAN_AFTER" --argjson head_same "$HEAD_SAME" \
    --argjson pclean_after "$PCLEAN_AFTER" --argjson phead_same "$PHEAD_SAME" \
    --arg started "$STARTED" --arg finished "$FINISHED" \
    --arg order "RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL" \
    --arg mm_order "MATMUL_V1_FMUL_RNE_PRODUCTS_THEN_E1_REDUCE_SUM_RECURSIVE_TILE32_PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1_PAD_IDENTITY_MIN_ONE_LEVEL" \
    --arg bin "$BIN_SHA" --arg log "$LOG_SHA" --arg errlog "$ERR_SHA" \
    --argjson devfail_n "$DEVFAIL_N" --argjson failed_calls_n "$FAILED_CALLS_N" --arg exit_status "$STATUS" \
    --arg host_line "$(line TENSOR_GB10_HOST:)" --arg parity "$(line TENSOR_GB10_PARITY:)" \
    --arg mutants "$(line TENSOR_GB10_MUTANTS:)" --arg cases "$(line 'cases:')" \
    --arg verdict_line "$(line 'M20 Tensor GB10 Verdict:')" \
    --arg host "$(uname -n)" --arg kernel "$(uname -r)" \
    '{suite:$suite,status:$status,reason:$reason,run_id:$run_id,started_utc:$started,finished_utc:$finished,
      omega_commit:$commit,omega_tree_clean_before:true,omega_tree_clean_after:$clean_after,
      omega_commit_unchanged_after:$head_same,physics_commit:$physics,physics_lock_pin:$pin,
      physics_tree_clean_before:true,physics_tree_clean_after:$pclean_after,physics_commit_unchanged_after:$phead_same,
      reduce_declared_order:$order,matmul_declared_order:$mm_order,binary_sha256:$bin,chip_log_sha256:$log,
      chip_stderr_sha256:$errlog,devfail_lines:$devfail_n,failed_gb10_calls:$failed_calls_n,
      chip_exit_status:$exit_status,cases_line:$cases,host_line:$host_line,parity_line:$parity,
      mutants_line:$mutants,verdict_line:$verdict_line,
      realization:{elementwise:"E1 omega_gb10_execute_simt_op (chunks <= 65536)",
        reduce:"E1 omega_reduce_gb10 (MEAN final division is its declared host step)",
        reduce_rows_sum:"E1 REDUCE_SUM warp kernel via omega_gb10_execute_simt_op, all rows per level",
        matmul:"FMUL_RNE products + E1 tree; no Tensor Core, no FFMA"},
      mutants:["WRONG_REDUCE_ORDER","FFMA_MATMUL","SWAPPED_OPERAND"],quiet_flag:"not used",
      host:$host,kernel:$kernel}' > "$TMP" || die "receipt json (jq) failed, no receipt written"
[ -s "$TMP" ] || die "empty receipt, not written"
[ "$VERDICT" != PASS ] || { [ -n "$BIN_SHA" ] && [ -n "$LOG_SHA" ]; } || die "PASS without binary and log digests refused"
DIG=$(sha256sum "$TMP" | cut -d' ' -f1)
(set -o noclobber; cat "$TMP" > "$EVID/$DIG.json") || die "receipt $DIG.json already exists"
chmod 0444 "$EVID/$DIG.json"
echo "receipt: $EVID/$DIG.json status=$VERDICT ${FAIL_REASON}"
[ "$VERDICT" = PASS ]
