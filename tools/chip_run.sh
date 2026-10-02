#!/bin/bash
# chip_run.sh MANIFEST [--physics-dir DIR] [--evidence-dir DIR] [-- gate args]
# Run one chip binary under evidence. The one place that knows how (ground truth:
# tools/run_numeric_transc_gate.sh, tools/run_unwritten_trap.sh, tests/run_reduce_chip.sh).
# The gcc line is the transc/unwritten-trap one; reduce differs (see NOT COVERED).
# MANIFEST is shell syntax setting: GATE TEST_SOURCE SOURCES EXTRA_BUILD_SOURCES (relative
#   to the omega root, may use $PHYSICS and $HERE) RUN_ARGS VERDICT_RE PASS_LINE OWNER
#   EVIDENCE_DIR RAISE_QUIET TAKE_GPU_LOCK REFUSE_DIRTY_OMEGA (default 1) REFUSE_EXIT
#   (default 2; must be an integer 1..125, else exit 1 "CHIP_RUN: BAD_MANIFEST") REFUSE_VERDICT_LINE
#   (default "CHIP_RUN: NOT_RUN") REQUIRE_ALL_PASS (default 0; 0 or 1; when 1 EVERY line matching
#   VERDICT_RE must also match PASS_LINE, so set VERDICT_RE to verdict lines only), hooks
#   HOST_TIER_CMD (run first; nonzero refuses) and RECEIPT_EXTRA_JQ (jq filter on the receipt,
#   $log = chip log). The manifest cannot change the test seam variables below, QUIET_FLAG,
#   GPU_LOCK or EST_LOAD_CMD (checked and restored after it is sourced). That is an accident guard,
#   not a boundary: the manifest is sourced into this shell and could still set any other variable.
# Gate args after "--" REPLACE the manifest RUN_ARGS (not appended); RUN_ARGS is split on whitespace.
# receipt host_tier = last line of the HOST_TIER_CMD log (empty if none); only its last 3 lines are
#   printed. HOST_TIER_CMD runs in the omega root with CHIPRUN_RUN_DIR set, before the quiet flag and lock.
# Physics dir, first match wins: --physics-dir, $PHYSICS_DIR, $PHYSICS, ~/workspace/physics.
# Exit: 0 PASS; 1 FAIL (binary ran, verdict not PASS or tree changed); REFUSE_EXIT (default 2,
#   as run_unwritten_trap.sh) after printing "REFUSED: why" and REFUSE_VERDICT_LINE. A gate that
#   keeps transc semantics sets REFUSE_EXIT=1 REFUSE_VERDICT_LINE="VERDICT NOT_RUN".
#   Those two apply only to refusals after the manifest is sourced; usage, argument, seam-before and
#   missing-manifest refusals always exit 2 with "CHIP_RUN: NOT_RUN". Exit 1 is also BAD_MANIFEST and
#   the post-run "fatal" paths (log blob or receipt not written): "CHIP_RUN: FAIL ...", no receipt promised.
# REFUSES (the chip binary is not run; HOST_TIER_CMD, gcc and nm may already have run): bad usage,
#   argument or manifest; missing manifest variables or GATE characters; no evidence dir; test override env without CHIPRUN_SELFTEST=1 (checked before and
#   after the manifest is sourced; a manifest that changes one is refused); no physics checkout;
#   unreadable omega or physics HEAD or physics.lock; physics not at the physics.lock pin;
#   physics dirty; omega dirty (if REFUSE_DIRTY_OMEGA=1); evidence dir inside omega or physics;
#   quiet flag already up or lost in a create race (if RAISE_QUIET=1); an est_load process; the
#   GPU lock unopenable or held (if TAKE_GPU_LOCK=1); no temp dir; HOST_TIER_CMD failing; a
#   missing source; gcc failing; nm unreadable; libm math or CUDA symbols in the binary. The libm check
#   is a fixed list: sqrt exp exp2 log log2 pow fma sin cos tanh erf round fabs and their f variants;
#   floor, ceil, fmod, tan, sinh, atan, log10 and the rest are not caught.
# FAILS (ran to the end, receipt written): nonzero exit; no line matching VERDICT_RE; last such
#   line not matching PASS_LINE (any such line, if REQUIRE_ALL_PASS=1); omega (if
#   REFUSE_DIRTY_OMEGA=1) or physics dirty after the run; either HEAD moved. Never killed, never
#   timed out. Quiet flag dropped only if still ours. The receipt "reason" holds only the first failure.
# RECEIPT <evidence>/<sha256 of its content>.json, mode 0444, noclobber, plus blobs/<sha>.log.
#   Fields: gate owner omega_commit omega_tree_clean_before omega_tree_clean_after
#   omega_commit_unchanged_after physics_commit physics_lock_pin physics_tree_clean_before
#   physics_tree_clean_after physics_commit_unchanged_after binary_sha256 chip_log_sha256
#   run_args host_tier verdict_lines chip_exit_status started_utc finished_utc verdict reason
#   (plus whatever RECEIPT_EXTRA_JQ adds).
# NOT COVERED by this module (a gate that needs one adds it, or does not use this module yet):
#   - transc's KDIG kernel-digest field and its unknown-op check: use RECEIPT_EXTRA_JQ.
#   - transc's nvdisasm check: use HOST_TIER_CMD.
#   - transc locks with fuser (refuses if ANY process holds the lock file); this module uses
#     flock -n (refuses only if another flock holder has it). Not the same test.
#   - reduce WAITS for the lock and the quiet flag; this module REFUSES at once.
#   - binary blob storage: reduce stores blobs/<sha>.bin and refuses a PASS with no digests;
#     this module stores only the log blob, and records binary_sha256 without keeping the binary.
#   - REQUIRE_ALL_PASS checks lines that appear, not that a particular line appears: reduce's
#     SUM, MAX, MIN, MEAN, RED_GB10_PARITY and E1 lines are not each demanded.
#   - receipt reuse (an existing receipt with the same name is compared, not rewritten) has no case.
#   - the build line: reduce (tests/run_reduce_chip.sh) adds -I"$PHYS/forge" (no variable here for it)
#     and has neither -fno-fast-math nor -pthread; this module uses the transc/unwritten-trap flags.
# Test seam (only with CHIPRUN_SELFTEST=1): CHIPRUN_PREBUILT_BIN CHIPRUN_QUIET_FLAG
#   CHIPRUN_GPU_LOCK CHIPRUN_EST_LOAD_CMD. Mutation check: each refusal line ends in
#   "# REFUSAL:<id>"; tests/test_chip_run.sh --mutants deletes each one and needs case <id> to FAIL.
#   The restore of QUIET_FLAG, GPU_LOCK and EST_LOAD_CMD after the manifest is tagged the same way
#   (case restore_vars). --mutants also reverts the jq "x" prefix for the verdict lines and for the
#   run args and needs case dash_verdict or happy to FAIL. Not tagged: the "fatal" exits after the run.
# Shell + GNU coreutils (date -d, realpath -m) + git + jq + gcc + nm + flock. No Python.
set -u
HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NVDIR=third_party/nvidia-open-580.173.02   # the one definition of the NVIDIA header tree
QUIET_FLAG=${CHIPRUN_QUIET_FLAG:-$HOME/workspace/.spark-quiet}
GPU_LOCK=${CHIPRUN_GPU_LOCK:-/tmp/aien-gb10.lock}
EST_LOAD_CMD=${CHIPRUN_EST_LOAD_CMD:-pgrep est_load}
FLAG_MINE=0; FLAG_TEXT=""; FAIL_REASON=""
REFUSE_EXIT=2; REFUSE_VERDICT_LINE="CHIP_RUN: NOT_RUN"
refuse() { echo "REFUSED: $*"; echo "$REFUSE_VERDICT_LINE"; exit "$REFUSE_EXIT"; }
bad_manifest() { echo "CHIP_RUN: BAD_MANIFEST $*"; exit 1; }
fatal() { echo "CHIP_RUN: FAIL $*"; exit 1; }
fail() { FAIL_REASON=${FAIL_REASON:-$*}; }
cleanup() {
    [ "$FLAG_MINE" = 1 ] || return 0
    [ "$(cat "$QUIET_FLAG" 2>/dev/null)" = "$FLAG_TEXT" ] || return 0 # REFUSAL:flag_not_ours
    rm -f "$QUIET_FLAG"; return 0
}
trap cleanup EXIT
sha() { sha256sum "$1" | cut -d' ' -f1; }
clean() { [ -z "$(git -C "$1" status --porcelain 2>/dev/null)" ] && echo true || echo false; }
head_of() { git -C "$1" rev-parse HEAD 2>/dev/null; }

# Test seam: snapshot before the manifest is sourced, check before and after, restore after.
SEAM_VARS=(CHIPRUN_SELFTEST CHIPRUN_PREBUILT_BIN CHIPRUN_QUIET_FLAG CHIPRUN_GPU_LOCK CHIPRUN_EST_LOAD_CMD)
declare -A SEAM0; for v in "${SEAM_VARS[@]}"; do SEAM0[$v]=${!v:-}; done
SELFTEST0=${CHIPRUN_SELFTEST:-}; QUIET0=$QUIET_FLAG; GPU0=$GPU_LOCK; EST0=$EST_LOAD_CMD
seam_check() {
    [ -z "${CHIPRUN_PREBUILT_BIN:-}" ] || [ "$SELFTEST0" = 1 ] || refuse "CHIPRUN_PREBUILT_BIN is a self-test override; set CHIPRUN_SELFTEST=1" # REFUSAL:seam_prebuilt
    [ -z "${CHIPRUN_QUIET_FLAG:-}" ] || [ "$SELFTEST0" = 1 ] || refuse "CHIPRUN_QUIET_FLAG is a self-test override; set CHIPRUN_SELFTEST=1" # REFUSAL:seam_quiet
    [ -z "${CHIPRUN_GPU_LOCK:-}" ] || [ "$SELFTEST0" = 1 ] || refuse "CHIPRUN_GPU_LOCK is a self-test override; set CHIPRUN_SELFTEST=1" # REFUSAL:seam_gpu
    [ -z "${CHIPRUN_EST_LOAD_CMD:-}" ] || [ "$SELFTEST0" = 1 ] || refuse "CHIPRUN_EST_LOAD_CMD is a self-test override; set CHIPRUN_SELFTEST=1" # REFUSAL:seam_est
}

[ $# -ge 1 ] || refuse "usage: chip_run.sh MANIFEST [--physics-dir DIR] [--evidence-dir DIR] [-- gate args]" # REFUSAL:usage
MANIFEST=$1; shift
PHYS_FLAG=""; EVID_FLAG=""; BAD_ARG=""
while [ $# -gt 0 ]; do
    case $1 in --physics-dir|--evidence-dir) [ $# -ge 2 ] || refuse "$1 needs a value" ;; esac # REFUSAL:arg_value
    case $1 in
        --physics-dir) PHYS_FLAG=$2; shift 2 ;;
        --evidence-dir) EVID_FLAG=$2; shift 2 ;;
        --) shift; break ;;
        *) BAD_ARG=$1; break ;;
    esac
done
[ -z "$BAD_ARG" ] || refuse "unknown argument $BAD_ARG" # REFUSAL:bad_arg
GATE_ARGS=("$@")
seam_check # REFUSAL:seam_before

# Physics dir resolution lives here and nowhere else.
PHYSICS=${PHYS_FLAG:-${PHYSICS_DIR:-${PHYSICS:-$HOME/workspace/physics}}}
PHYSICS=$(realpath -m "$PHYSICS")

GATE=""; TEST_SOURCE=""; SOURCES=""; EXTRA_BUILD_SOURCES=""; RUN_ARGS=""; VERDICT_RE=""; PASS_LINE=""
OWNER=""; EVIDENCE_DIR=""; RAISE_QUIET=1; TAKE_GPU_LOCK=1; REFUSE_DIRTY_OMEGA=1; HOST_TIER_CMD=""; RECEIPT_EXTRA_JQ=""; REQUIRE_ALL_PASS=0
[ -f "$MANIFEST" ] || refuse "no manifest at $MANIFEST" # REFUSAL:no_manifest
. "$MANIFEST"; MRC=$?
REX_BAD=""
case $REFUSE_EXIT in ''|*[!0-9]*) REX_BAD=1 ;; *) { [ "${#REFUSE_EXIT}" -le 3 ] && [ "$((10#$REFUSE_EXIT))" -ge 1 ] && [ "$((10#$REFUSE_EXIT))" -le 125 ]; } || REX_BAD=1 ;; esac
[ -z "$REX_BAD" ] || bad_manifest "REFUSE_EXIT=$REFUSE_EXIT is not an integer from 1 to 125" # REFUSAL:refuse_exit
case $REQUIRE_ALL_PASS in 0|1) ;; *) bad_manifest "REQUIRE_ALL_PASS=$REQUIRE_ALL_PASS is not 0 or 1" ;; esac # REFUSAL:require_all_value
seam_check # REFUSAL:seam_after
for v in "${SEAM_VARS[@]}"; do [ "${!v:-}" = "${SEAM0[$v]}" ] || refuse "manifest changed $v, a test seam variable"; done # REFUSAL:seam_changed
QUIET_FLAG=$QUIET0; GPU_LOCK=$GPU0; EST_LOAD_CMD=$EST0 # REFUSAL:restore_vars
[ "$MRC" = 0 ] || refuse "manifest $MANIFEST failed to load" # REFUSAL:manifest_load
for v in GATE TEST_SOURCE VERDICT_RE PASS_LINE OWNER; do [ -n "${!v}" ] || refuse "manifest does not set $v"; done # REFUSAL:manifest_vars
case $GATE in *[!A-Za-z0-9_.-]*) refuse "GATE $GATE has characters outside A-Za-z0-9_.-" ;; esac # REFUSAL:gate_chars
EVID=${EVID_FLAG:-$EVIDENCE_DIR}
[ -n "$EVID" ] || refuse "no evidence dir (manifest EVIDENCE_DIR or --evidence-dir)" # REFUSAL:no_evidence
EVID=$(realpath -m "$EVID")
if [ "${#GATE_ARGS[@]}" -eq 0 ]; then read -ra GATE_ARGS <<< "$RUN_ARGS"; fi

[ -d "$PHYSICS/nvrm" ] || refuse "no physics checkout at $PHYSICS" # REFUSAL:no_physics
case "$EVID/" in "$HERE"/*) refuse "evidence dir $EVID is inside a candidate tree" ;; esac # REFUSAL:evidence_inside
case "$EVID/" in "$PHYSICS"/*) refuse "evidence dir $EVID is inside a candidate tree" ;; esac # REFUSAL:evidence_inside_physics
OMEGA_COMMIT=$(head_of "$HERE") || refuse "cannot read omega HEAD" # REFUSAL:omega_head
[ "$REFUSE_DIRTY_OMEGA" != 1 ] || [ "$(clean "$HERE")" = true ] || refuse "omega tree $HERE is dirty" # REFUSAL:omega_dirty
PIN=$(tr -d '[:space:]' < "$HERE/physics.lock") || refuse "cannot read physics.lock" # REFUSAL:physics_lock
PHYS_COMMIT=$(head_of "$PHYSICS") || refuse "cannot read physics HEAD at $PHYSICS" # REFUSAL:physics_head
[ "$PHYS_COMMIT" = "$PIN" ] || refuse "physics checkout is at $PHYS_COMMIT, physics.lock pins $PIN" # REFUSAL:physics_pin
[ "$(clean "$PHYSICS")" = true ] || refuse "physics checkout $PHYSICS is dirty" # REFUSAL:physics_dirty
OMEGA_CLEAN_BEFORE=$(clean "$HERE"); PHYS_CLEAN_BEFORE=true
[ "$RAISE_QUIET" != 1 ] || [ ! -e "$QUIET_FLAG" ] || refuse "quiet flag is up: $(cat "$QUIET_FLAG")" # REFUSAL:quiet_flag
[ -z "$(bash -c "$EST_LOAD_CMD" 2>/dev/null)" ] || refuse "an est_load process is running" # REFUSAL:est_load

RUN=$(mktemp -d "${TMPDIR:-/tmp}/chip-run.XXXXXX") || refuse "cannot create a run directory" # REFUSAL:mktemp
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
    echo "$FLAG_TEXT" > "$QUIET_FLAG" 2>/dev/null || { set +o noclobber; refuse "could not create the quiet flag"; } # REFUSAL:flag_create
    set +o noclobber
    FLAG_MINE=1
fi
if [ "$TAKE_GPU_LOCK" = 1 ]; then
    { : >> "$GPU_LOCK"; } 2>/dev/null || refuse "cannot open $GPU_LOCK" # REFUSAL:gpu_open
    exec 9>> "$GPU_LOCK"
    flock -n 9 || refuse "GPU lock $GPU_LOCK is held" # REFUSAL:gpu_lock
fi

if [ -n "${CHIPRUN_PREBUILT_BIN:-}" ]; then
    BIN=$CHIPRUN_PREBUILT_BIN
    [ -x "$BIN" ] || refuse "CHIPRUN_PREBUILT_BIN $BIN is not executable" # REFUSAL:prebuilt_not_exec
else
    echo "== chip build"
    BIN=$RUN/$GATE.bin
    read -ra SRCS <<< "$TEST_SOURCE $SOURCES $EXTRA_BUILD_SOURCES"
    for s in "${SRCS[@]}"; do case $s in /*) f=$s ;; *) f=$HERE/$s ;; esac; [ -f "$f" ] || refuse "missing source $s"; done # REFUSAL:missing_source
    NV=$PHYSICS/$NVDIR
    (cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-fast-math -pthread \
        -Isrc -I"$PHYSICS/nvrm" -I"$PHYSICS/m16" \
        -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
        -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
        -o "$BIN" "${SRCS[@]}") > "$RUN/chip-build.log" 2>&1; BRC=$?
    [ "$BRC" = 0 ] || { cat "$RUN/chip-build.log"; refuse "chip build failed"; } # REFUSAL:build_failed
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
if [ "$REQUIRE_ALL_PASS" = 1 ] && [ "${#VLINES[@]}" -gt 0 ] && printf '%s\n' "${VLINES[@]}" | grep -Evq -- "$PASS_LINE"; then fail "a verdict line does not match PASS_LINE (REQUIRE_ALL_PASS=1)"; fi # REFUSAL:fail_any_verdict
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
# The two arrays go to jq with an "x" prefix that jq strips: jq 1.7 reads an --args value that starts with "--" or with "-" and a letter as an option.
BODY=$(jq -n --arg gate "$GATE" --arg owner "$OWNER" --arg omega "$OMEGA_COMMIT" --arg phys "$PHYS_COMMIT" --arg pin "$PIN" \
    --argjson oclean "$OMEGA_CLEAN_BEFORE" --argjson pclean "$PHYS_CLEAN_BEFORE" \
    --argjson oafter "$OMEGA_CLEAN_AFTER" --argjson pafter "$PHYS_CLEAN_AFTER" --argjson osame "$OMEGA_SAME" --argjson psame "$PHYS_SAME" \
    --arg bin "$BIN_SHA" --arg log "$LOG_SHA" --arg host "$HOST_TIER" --argjson rc "$CHIP_RC" \
    --arg start "$START" --arg end "$END" --arg verdict "$VERDICT" --arg reason "$FAIL_REASON" \
    --argjson run_args "$(jq -n '$ARGS.positional | map(.[1:])' --args "${GATE_ARGS[@]/#/x}")" \
    --argjson vlines "$(jq -n '$ARGS.positional | map(.[1:])' --args "${VLINES[@]/#/x}")" '{
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
