#!/bin/bash
# chip_run.sh MANIFEST [--physics-dir DIR] [--evidence-dir DIR] [-- gate args]
# Run one chip binary under evidence. The one place that knows how (ground truth:
# tools/run_numeric_transc_gate.sh, tools/run_unwritten_trap.sh, tests/run_reduce_chip.sh).
# MANIFEST is shell syntax setting: GATE TEST_SOURCE SOURCES EXTRA_BUILD_SOURCES (relative
#   to the omega root, may use $PHYSICS and $HERE) RUN_ARGS VERDICT_RE PASS_LINE OWNER
#   EVIDENCE_DIR RAISE_QUIET TAKE_GPU_LOCK REFUSE_DIRTY_OMEGA (default 1) REFUSE_EXIT
#   (default 2) REFUSE_VERDICT_LINE (default "CHIP_RUN: NOT_RUN"), hooks HOST_TIER_CMD
#   (run first; nonzero refuses) and RECEIPT_EXTRA_JQ (jq filter on the receipt, $log = chip log).
# Physics dir, first match wins: --physics-dir, $PHYSICS_DIR, $PHYSICS, ~/workspace/physics.
# Exit: 0 PASS; 1 FAIL (binary ran, verdict not PASS or tree changed); REFUSE_EXIT (default 2,
#   as run_unwritten_trap.sh) after printing "REFUSED: why" and REFUSE_VERDICT_LINE. A gate that
#   keeps transc semantics sets REFUSE_EXIT=1 REFUSE_VERDICT_LINE="VERDICT NOT_RUN".
# REFUSES (nothing is run): bad usage or manifest; test override env without CHIPRUN_SELFTEST=1;
#   no physics checkout; physics not at the physics.lock pin; physics dirty; omega dirty (if
#   REFUSE_DIRTY_OMEGA=1); evidence dir inside omega or physics; quiet flag already up (if
#   RAISE_QUIET=1); an est_load process; the GPU lock held (if TAKE_GPU_LOCK=1); HOST_TIER_CMD
#   failing; a missing source; gcc failing; nm unreadable; libm math or CUDA symbols in the binary.
# FAILS (ran to the end, receipt written): nonzero exit; no line matching VERDICT_RE; last such
#   line not matching PASS_LINE; omega (if REFUSE_DIRTY_OMEGA=1) or physics dirty after the run;
#   either HEAD moved. Never killed, never timed out. Quiet flag dropped only if still ours.
# RECEIPT <evidence>/<sha256 of its content>.json, mode 0444, noclobber, plus blobs/<sha>.log.
#   Fields: gate owner omega_commit omega_tree_clean_before omega_tree_clean_after
#   omega_commit_unchanged_after physics_commit physics_lock_pin physics_tree_clean_before
#   physics_tree_clean_after physics_commit_unchanged_after binary_sha256 chip_log_sha256
#   run_args host_tier verdict_lines chip_exit_status started_utc finished_utc verdict reason
#   (plus whatever RECEIPT_EXTRA_JQ adds).
# Test seam (only with CHIPRUN_SELFTEST=1): CHIPRUN_PREBUILT_BIN CHIPRUN_QUIET_FLAG
#   CHIPRUN_GPU_LOCK CHIPRUN_EST_LOAD_CMD. Mutation check: each refusal line ends in
#   "# REFUSAL:<id>"; tests/test_chip_run.sh --mutants deletes each one and needs case <id> to FAIL.
# Shell + coreutils + git + jq + gcc + nm + flock. No Python.
set -u
HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NVDIR=third_party/nvidia-open-580.173.02   # the one definition of the NVIDIA header tree
QUIET_FLAG=${CHIPRUN_QUIET_FLAG:-$HOME/workspace/.spark-quiet}
GPU_LOCK=${CHIPRUN_GPU_LOCK:-/tmp/aien-gb10.lock}
EST_LOAD_CMD=${CHIPRUN_EST_LOAD_CMD:-pgrep est_load}
FLAG_MINE=0; FLAG_TEXT=""; FAIL_REASON=""
REFUSE_EXIT=2; REFUSE_VERDICT_LINE="CHIP_RUN: NOT_RUN"
refuse() { echo "REFUSED: $*"; echo "$REFUSE_VERDICT_LINE"; exit "$REFUSE_EXIT"; }
fatal() { echo "CHIP_RUN: FAIL $*"; exit 1; }
fail() { FAIL_REASON=${FAIL_REASON:-$*}; }
cleanup() { [ "$FLAG_MINE" = 1 ] && [ "$(cat "$QUIET_FLAG" 2>/dev/null)" = "$FLAG_TEXT" ] && rm -f "$QUIET_FLAG"; return 0; }
trap cleanup EXIT
sha() { sha256sum "$1" | cut -d' ' -f1; }
clean() { [ -z "$(git -C "$1" status --porcelain 2>/dev/null)" ] && echo true || echo false; }
head_of() { git -C "$1" rev-parse HEAD 2>/dev/null; }

[ $# -ge 1 ] || refuse "usage: chip_run.sh MANIFEST [--physics-dir DIR] [--evidence-dir DIR] [-- gate args]"
MANIFEST=$1; shift
PHYS_FLAG=""; EVID_FLAG=""
while [ $# -gt 0 ]; do
    case $1 in
        --physics-dir) [ $# -ge 2 ] || refuse "--physics-dir needs a value"; PHYS_FLAG=$2; shift 2 ;;
        --evidence-dir) [ $# -ge 2 ] || refuse "--evidence-dir needs a value"; EVID_FLAG=$2; shift 2 ;;
        --) shift; break ;;
        *) refuse "unknown argument $1" ;;
    esac
done
GATE_ARGS=("$@")
for v in CHIPRUN_PREBUILT_BIN CHIPRUN_QUIET_FLAG CHIPRUN_GPU_LOCK CHIPRUN_EST_LOAD_CMD; do [ -z "${!v:-}" ] || [ "${CHIPRUN_SELFTEST:-}" = 1 ] || refuse "$v is a self-test override; set CHIPRUN_SELFTEST=1"; done # REFUSAL:selftest_override

# Physics dir resolution lives here and nowhere else.
PHYSICS=${PHYS_FLAG:-${PHYSICS_DIR:-${PHYSICS:-$HOME/workspace/physics}}}
PHYSICS=$(realpath -m "$PHYSICS")

GATE=""; TEST_SOURCE=""; SOURCES=""; EXTRA_BUILD_SOURCES=""; RUN_ARGS=""; VERDICT_RE=""; PASS_LINE=""
OWNER=""; EVIDENCE_DIR=""; RAISE_QUIET=1; TAKE_GPU_LOCK=1; REFUSE_DIRTY_OMEGA=1; HOST_TIER_CMD=""; RECEIPT_EXTRA_JQ=""
[ -f "$MANIFEST" ] || refuse "no manifest at $MANIFEST"
. "$MANIFEST" || refuse "manifest $MANIFEST failed to load"
for v in GATE TEST_SOURCE VERDICT_RE PASS_LINE OWNER; do [ -n "${!v}" ] || refuse "manifest does not set $v"; done
case $GATE in *[!A-Za-z0-9_.-]*) refuse "GATE $GATE has characters outside A-Za-z0-9_.-" ;; esac
EVID=${EVID_FLAG:-$EVIDENCE_DIR}
[ -n "$EVID" ] || refuse "no evidence dir (manifest EVIDENCE_DIR or --evidence-dir)"
EVID=$(realpath -m "$EVID")
if [ "${#GATE_ARGS[@]}" -eq 0 ]; then read -ra GATE_ARGS <<< "$RUN_ARGS"; fi

[ -d "$PHYSICS/nvrm" ] || refuse "no physics checkout at $PHYSICS" # REFUSAL:no_physics
case "$EVID/" in "$HERE"/*|"$PHYSICS"/*) refuse "evidence dir $EVID is inside a candidate tree" ;; esac # REFUSAL:evidence_inside
OMEGA_COMMIT=$(head_of "$HERE") || refuse "cannot read omega HEAD"
[ "$REFUSE_DIRTY_OMEGA" != 1 ] || [ "$(clean "$HERE")" = true ] || refuse "omega tree $HERE is dirty" # REFUSAL:omega_dirty
PIN=$(tr -d '[:space:]' < "$HERE/physics.lock") || refuse "cannot read physics.lock"
PHYS_COMMIT=$(head_of "$PHYSICS") || refuse "cannot read physics HEAD at $PHYSICS"
[ "$PHYS_COMMIT" = "$PIN" ] || refuse "physics checkout is at $PHYS_COMMIT, physics.lock pins $PIN" # REFUSAL:physics_pin
[ "$(clean "$PHYSICS")" = true ] || refuse "physics checkout $PHYSICS is dirty" # REFUSAL:physics_dirty
OMEGA_CLEAN_BEFORE=$(clean "$HERE"); PHYS_CLEAN_BEFORE=true
[ "$RAISE_QUIET" != 1 ] || [ ! -e "$QUIET_FLAG" ] || refuse "quiet flag is up: $(cat "$QUIET_FLAG")" # REFUSAL:quiet_flag
[ -z "$(bash -c "$EST_LOAD_CMD" 2>/dev/null)" ] || refuse "an est_load process is running" # REFUSAL:est_load

RUN=$(mktemp -d "${TMPDIR:-/tmp}/chip-run.XXXXXX") || refuse "cannot create a run directory"
if [ -n "$HOST_TIER_CMD" ]; then
    (cd "$HERE" && CHIPRUN_RUN_DIR="$RUN" bash -c "$HOST_TIER_CMD") > "$RUN/host.log" 2>&1; HRC=$?
    tail -n 3 "$RUN/host.log"
    [ "$HRC" = 0 ] || refuse "host tier failed (exit $HRC); no chip run" # REFUSAL:host_tier
fi
HOST_TIER=$(tail -n 1 "$RUN/host.log" 2>/dev/null)

START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
if [ "$RAISE_QUIET" = 1 ]; then
    FLAG_TEXT="$OWNER start=$START expected_end=$(date -u -d '+2 hours' +%Y-%m-%dT%H:%M:%SZ) pid=$$"
    set -o noclobber
    echo "$FLAG_TEXT" > "$QUIET_FLAG" 2>/dev/null || { set +o noclobber; refuse "could not create the quiet flag"; }
    set +o noclobber
    FLAG_MINE=1
fi
if [ "$TAKE_GPU_LOCK" = 1 ]; then
    exec 9> "$GPU_LOCK" || refuse "cannot open $GPU_LOCK"
    flock -n 9 || refuse "GPU lock $GPU_LOCK is held" # REFUSAL:gpu_lock
fi

if [ -n "${CHIPRUN_PREBUILT_BIN:-}" ]; then
    BIN=$CHIPRUN_PREBUILT_BIN
    [ -x "$BIN" ] || refuse "CHIPRUN_PREBUILT_BIN $BIN is not executable"
else
    echo "== chip build"
    BIN=$RUN/$GATE.bin
    read -ra SRCS <<< "$TEST_SOURCE $SOURCES $EXTRA_BUILD_SOURCES"
    for s in "${SRCS[@]}"; do case $s in /*) f=$s ;; *) f=$HERE/$s ;; esac; [ -f "$f" ] || refuse "missing source $s"; done
    NV=$PHYSICS/$NVDIR
    (cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-fast-math -pthread \
        -Isrc -I"$PHYSICS/nvrm" -I"$PHYSICS/m16" \
        -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
        -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
        -o "$BIN" "${SRCS[@]}") > "$RUN/chip-build.log" 2>&1 || { cat "$RUN/chip-build.log"; refuse "chip build failed"; }
fi
SYMS=$(nm -u "$BIN" 2>&1) || refuse "nm cannot read $BIN" # REFUSAL:nm_unreadable
if printf '%s\n' "$SYMS" | grep -Eq '\b(sqrtf?|expf?|exp2f?|logf?|log2f?|powf?|fmaf?|sinf?|cosf?|tanhf?|erff?|roundf?|fabsf?)\b'; then refuse "libm math symbols in $BIN"; fi # REFUSAL:libm
if printf '%s\n' "$SYMS" | grep -Eiq 'cuda|cuInit|cuLaunch|nvrtc|cublas'; then refuse "CUDA symbols in $BIN"; fi # REFUSAL:cuda
BIN_SHA=$(sha "$BIN")

echo "== chip run (not killed, not timed out): ${GATE_ARGS[*]}"
(cd "$HERE" && "$BIN" "${GATE_ARGS[@]}") > "$RUN/chip.log" 2>&1; CHIP_RC=$?
END=$(date -u +%Y-%m-%dT%H:%M:%SZ)
exec 9>&-
mapfile -t VLINES < <(grep -E -- "$VERDICT_RE" "$RUN/chip.log")
printf '%s\n' "${VLINES[@]}"

OMEGA_CLEAN_AFTER=$(clean "$HERE"); PHYS_CLEAN_AFTER=$(clean "$PHYSICS")
OMEGA_SAME=$([ "$(head_of "$HERE")" = "$OMEGA_COMMIT" ] && echo true || echo false)
PHYS_SAME=$([ "$(head_of "$PHYSICS")" = "$PHYS_COMMIT" ] && echo true || echo false)
[ "$CHIP_RC" = 0 ] || fail "binary exit status $CHIP_RC" # REFUSAL:rc_nonzero
[ "${#VLINES[@]}" -gt 0 ] || fail "no verdict line matching $VERDICT_RE" # REFUSAL:no_verdict
[ "${#VLINES[@]}" -eq 0 ] || printf '%s\n' "${VLINES[$((${#VLINES[@]} - 1))]}" | grep -Eq -- "$PASS_LINE" || fail "last verdict line does not match PASS_LINE" # REFUSAL:fail_verdict
[ "$REFUSE_DIRTY_OMEGA" != 1 ] || [ "$OMEGA_CLEAN_AFTER" = true ] || fail "omega tree dirty after run" # REFUSAL:omega_dirty_after
[ "$PHYS_CLEAN_AFTER" = true ] || fail "physics tree dirty after run" # REFUSAL:physics_dirty_after
[ "$OMEGA_SAME" = true ] && [ "$PHYS_SAME" = true ] || fail "a HEAD moved during the run" # REFUSAL:head_moved
VERDICT=PASS; [ -z "$FAIL_REASON" ] || VERDICT=FAIL

LOG_SHA=$(sha "$RUN/chip.log")
mkdir -p "$EVID/blobs" || fatal "cannot create $EVID/blobs"
if [ ! -e "$EVID/blobs/$LOG_SHA.log" ]; then
    TMPB=$(mktemp "$EVID/blobs/.tmp.XXXXXX") && cp "$RUN/chip.log" "$TMPB" && chmod 0444 "$TMPB" && mv -n "$TMPB" "$EVID/blobs/$LOG_SHA.log" || fatal "could not store the log blob"
    rm -f "$TMPB"
fi
[ "$(sha "$EVID/blobs/$LOG_SHA.log")" = "$LOG_SHA" ] || fatal "log blob does not match its digest"
BODY=$(jq -n --arg gate "$GATE" --arg owner "$OWNER" --arg omega "$OMEGA_COMMIT" --arg phys "$PHYS_COMMIT" --arg pin "$PIN" \
    --argjson oclean "$OMEGA_CLEAN_BEFORE" --argjson pclean "$PHYS_CLEAN_BEFORE" \
    --argjson oafter "$OMEGA_CLEAN_AFTER" --argjson pafter "$PHYS_CLEAN_AFTER" --argjson osame "$OMEGA_SAME" --argjson psame "$PHYS_SAME" \
    --arg bin "$BIN_SHA" --arg log "$LOG_SHA" --arg host "$HOST_TIER" --argjson rc "$CHIP_RC" \
    --arg start "$START" --arg end "$END" --arg verdict "$VERDICT" --arg reason "$FAIL_REASON" \
    --argjson run_args "$(jq -n '$ARGS.positional' --args "${GATE_ARGS[@]}")" \
    --argjson vlines "$(jq -n '$ARGS.positional' --args "${VLINES[@]}")" '{
  gate: $gate, owner: $owner, omega_commit: $omega, omega_tree_clean_before: $oclean, omega_tree_clean_after: $oafter,
  omega_commit_unchanged_after: $osame, physics_commit: $phys, physics_lock_pin: $pin, physics_tree_clean_before: $pclean,
  physics_tree_clean_after: $pafter, physics_commit_unchanged_after: $psame, binary_sha256: $bin, chip_log_sha256: $log,
  run_args: $run_args, host_tier: $host, verdict_lines: $vlines, chip_exit_status: $rc, started_utc: $start,
  finished_utc: $end, verdict: $verdict, reason: $reason }') || fatal "receipt JSON could not be built"
if [ -n "$RECEIPT_EXTRA_JQ" ]; then BODY=$(printf '%s' "$BODY" | jq --rawfile log "$RUN/chip.log" "$RECEIPT_EXTRA_JQ") || fatal "RECEIPT_EXTRA_JQ failed"; fi
printf '%s\n' "$BODY" > "$RUN/receipt.json"
jq -e '.gate and .verdict and .binary_sha256' "$RUN/receipt.json" > /dev/null || fatal "receipt lost its required fields"
RSHA=$(sha "$RUN/receipt.json"); OUT=$EVID/$RSHA.json
if [ ! -e "$OUT" ]; then
    set -o noclobber; { cat "$RUN/receipt.json" > "$OUT" && chmod 0444 "$OUT"; } || { set +o noclobber; fatal "could not write receipt $OUT"; }; set +o noclobber
fi
cmp -s "$RUN/receipt.json" "$OUT" || fatal "receipt $OUT does not match what was built"
echo "RECEIPT $OUT"
echo "CHIP_RUN: $VERDICT${FAIL_REASON:+ $FAIL_REASON}"
[ "$VERDICT" = PASS ]
