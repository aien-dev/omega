#!/bin/bash
# bench/prime_race/tests/run_gb10_chip.sh -- AIEN Prime Drag Race: native GB10 sieve chip test.
#
#   GB10_CHIP_RUN=1 bench/prime_race/tests/run_gb10_chip.sh --physics-dir DIR [--evidence-dir DIR]
#
# Pattern of tests/run_tensor_chip.sh (never creates or touches the quiet flag; the
# operator runs it inside an own quiet window).
# 1. Refuses unless GB10_CHIP_RUN=1, the omega tree is clean, and the physics checkout is
#    clean and at the physics.lock commit.
# 2. Builds libomega_gpu, gb10_native, gb10_native_mutant and the host test (nice, -j1);
#    the Makefile refuses a binary with CUDA symbols (nm, tools/chip_run.sh pattern); this
#    script checks again.
# 3. Runs the host test first (golden words, nvdisasm listing, IR simulator sweep): a
#    kernel that is wrong on the host never gets chip time.
# 4. Takes /tmp/aien-gb10.lock (waits; no timeout) and holds it for the whole sweep. Every
#    limit of the DESIGN sweep (gb10_sieve_host_test --limits) runs in audit mode (K = 2;
#    K = 5 at 1e6), every pass's bitmap compared byte for byte with the host oracle
#    (independent Eratosthenes); then 1e7 (K = 1), one timed run (1e6, 2 s, stdout line
#    and final bitmap checked) and the mutant binary at 1e6, which must be caught.
#    The first failing run stops the sweep (no further launches); nothing is ever killed.
# 5. Writes a content-addressed receipt <evidence-dir>/<sha256>.json (mode 0444, never
#    overwritten) for PASS and FAIL alike, plus the run log as blobs/<sha256>.log.
# Shell + coreutils + git + jq + make + nm + cmp + flock. No Python. Exit 0 PASS, 1 FAIL, 2 refused.
set -u
SELF_DIR=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OMEGA=$(cd "$SELF_DIR/../../.." && pwd)
PHYS=""
EVID=$HOME/workspace/evidence/prime-race/gb10-chip
while [ $# -gt 0 ]; do
    case $1 in
        --physics-dir) PHYS=$2; shift 2 ;;
        --evidence-dir) EVID=$2; shift 2 ;;
        *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
die() { echo "run_gb10_chip: $*" >&2; exit 2; }
[ "${GB10_CHIP_RUN:-}" = 1 ] || die "chip run refused: GB10_CHIP_RUN=1 is not set"
[ -n "$PHYS" ] && [ -d "$PHYS" ] || die "--physics-dir is required"
PHYS=$(cd "$PHYS" && pwd)
[ -z "$(git -C "$OMEGA" status --porcelain)" ] || die "omega tree is dirty; refusing"
COMMIT=$(git -C "$OMEGA" rev-parse HEAD)
LOCKED=$(tr -d ' \n' < "$OMEGA/physics.lock")
PHEAD=$(git -C "$PHYS" rev-parse HEAD)
[ "$PHEAD" = "$LOCKED" ] || die "physics checkout is at $PHEAD, physics.lock pins $LOCKED"
[ -z "$(git -C "$PHYS" status --porcelain)" ] || die "physics checkout is dirty"
command -v jq >/dev/null || die "jq missing"
mkdir -p "$EVID/blobs"
EVID=$(cd "$EVID" && pwd)
case "$EVID/" in "$OMEGA/"*|"$PHYS/"*) die "evidence dir must be outside the omega tree and physics checkout" ;; esac

STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
B=$OMEGA/bench/prime_race/build
BIN=$B/gb10_native MUT=$B/gb10_native_mutant HT=$B/gb10_sieve_host_test
T=$(mktemp -d) || die "no temp dir"
LOG=$T/run.log
FAIL_REASON=""
log() { echo "$*" | tee -a "$LOG"; }

(cd "$OMEGA" && nice -n 19 make -j1 PHYSICS_DIR="$PHYS" prime-race-gb10 bench/prime_race/build/gb10_sieve_host_test) > "$T/build.log" 2>&1 \
    || FAIL_REASON="build failed"
if [ -z "$FAIL_REASON" ]; then
    for x in "$BIN" "$MUT"; do
        nm -u "$x" | grep -Eiq 'cuda|cuInit|cuLaunch|nvrtc|cublas' && FAIL_REASON="CUDA symbols in $(basename "$x")"
    done
fi
BIN_SHA=$( [ -f "$BIN" ] && sha256sum "$BIN" | cut -d' ' -f1 )
MUT_SHA=$( [ -f "$MUT" ] && sha256sum "$MUT" | cut -d' ' -f1 )
HOST_OK=false
if [ -z "$FAIL_REASON" ]; then
    "$HT" >> "$LOG" 2>&1 && HOST_OK=true || FAIL_REASON="host test (simulator) failed"
fi

NRUNS=0 NPASSES=0 MUTANT_CAUGHT=false TIMED_OK=false KERNEL_SHA=""
# run_audit LIMIT K BINARY -> 0 match, 1 mismatch, 2 run failed
run_audit() {
    local L=$1 K=$2 X=$3
    rm -f "$T/got.bin" "$T/want.bin" "$T/rep.json"
    "$X" --limit "$L" --min-seconds 0 --audit-passes "$K" --bitmap-out "$T/got.bin" --report-out "$T/rep.json" \
        > "$T/stdout" 2> "$T/stderr"
    local rc=$?
    NRUNS=$((NRUNS + 1))
    if [ $rc -ne 0 ]; then log "limit $L: $(basename "$X") exit $rc: $(tail -2 "$T/stderr" | tr '\n' ' ')"; return 2; fi
    "$HT" --oracle "$L" "$T/want.bin" "$K" || { log "limit $L: oracle failed"; return 2; }
    if ! cmp -s "$T/got.bin" "$T/want.bin"; then
        log "limit $L: bitmap differs from oracle: $(cmp "$T/got.bin" "$T/want.bin" 2>&1 | head -1)"; return 1
    fi
    NPASSES=$((NPASSES + K))
    return 0
}

if [ -z "$FAIL_REASON" ]; then
    exec 9> /tmp/aien-gb10.lock || FAIL_REASON="cannot open /tmp/aien-gb10.lock"
fi
if [ -z "$FAIL_REASON" ]; then
    # waits for the lock; no timeout; never killed
    flock 9
    LOCK_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    log "GPU lock held from $LOCK_AT"
    for L in $("$HT" --limits) 10000000; do
        K=2; [ "$L" = 1000000 ] && K=5; [ "$L" = 10000000 ] && K=1
        run_audit "$L" "$K" "$BIN"; r=$?
        [ $r -eq 0 ] || { FAIL_REASON="limit $L: $( [ $r = 1 ] && echo mismatch || echo run failed )"; break; }
        [ "$L" = 1000000 ] && KERNEL_SHA=$(jq -r '.detail.kernel_code_sha256 // empty' "$T/rep.json" 2>/dev/null)
    done
    log "audit sweep: $NRUNS runs, $NPASSES passes checked"
    if [ -z "$FAIL_REASON" ]; then
        "$BIN" --limit 1000000 --min-seconds 2 --bitmap-out "$T/got.bin" --report-out "$T/timed.json" > "$T/timed.out" 2> "$T/timed.err"
        rc=$?
        "$HT" --oracle 1000000 "$T/want.bin" 1
        if [ $rc -eq 0 ] && [ "$(wc -l < "$T/timed.out")" = 1 ] \
           && grep -Eq '^aien-gb10-native;[1-9][0-9]*;[0-9]+\.[0-9]{6};[0-9]+;algorithm=other,faithful=no,bits=1$' "$T/timed.out" \
           && cmp -s "$T/got.bin" "$T/want.bin"; then
            TIMED_OK=true; log "timed smoke: $(cat "$T/timed.out")"
        else
            FAIL_REASON="timed run at 1e6: exit $rc, line '$(head -1 "$T/timed.out")'"
        fi
    fi
    if [ -z "$FAIL_REASON" ]; then
        run_audit 1000000 1 "$MUT"; r=$?
        if [ $r -eq 1 ]; then MUTANT_CAUGHT=true; log "mutant caught at 1e6"
        else FAIL_REASON="mutant not caught at 1e6 (status $r)"; fi
    fi
    flock -u 9
fi

VERDICT=PASS; [ -z "$FAIL_REASON" ] || VERDICT=FAIL
LOG_SHA=$(sha256sum "$LOG" 2>/dev/null | cut -d' ' -f1)
[ -n "$LOG_SHA" ] && [ ! -e "$EVID/blobs/$LOG_SHA.log" ] && cp "$LOG" "$EVID/blobs/$LOG_SHA.log" && chmod 0444 "$EVID/blobs/$LOG_SHA.log"
jq -n --arg v "$VERDICT" --arg reason "$FAIL_REASON" --arg commit "$COMMIT" --arg physics "$PHEAD" \
    --arg started "$STARTED" --arg finished "$(date -u +%Y-%m-%dT%H:%M:%SZ)" --arg bin "${BIN_SHA:-}" --arg mut "${MUT_SHA:-}" \
    --arg kernel "$KERNEL_SHA" --arg log "${LOG_SHA:-}" --argjson host "$HOST_OK" --argjson runs "$NRUNS" \
    --argjson passes "$NPASSES" --argjson mutant "$MUTANT_CAUGHT" --argjson timed "$TIMED_OK" \
    --arg gpu "$(nvidia-smi --query-gpu=name,uuid,driver_version --format=csv,noheader 2>/dev/null || echo unavailable)" \
    --arg kern "$(uname -r)" \
    '{schema:"aien-prime-race/gb10-chip-test/v1", verdict:$v, reason:$reason, omega_commit:$commit, physics_commit:$physics,
      started:$started, finished:$finished, binary_sha256:$bin, mutant_binary_sha256:$mut, kernel_code_sha256:$kernel,
      host_test_pass:$host, audit_runs:$runs, audit_passes_checked:$passes, timed_smoke_pass:$timed, mutant_caught:$mutant,
      gpu:$gpu, kernel:$kern, gpu_lock:"/tmp/aien-gb10.lock", quiet_flag:"not touched (operator window)",
      reference:"independent Eratosthenes oracle (gb10_sieve_host_test --oracle), byte-for-byte per pass", log_blob:$log}' \
    > "$T/receipt.json"
RSHA=$(sha256sum "$T/receipt.json" | cut -d' ' -f1)
if [ ! -e "$EVID/$RSHA.json" ]; then cp "$T/receipt.json" "$EVID/$RSHA.json" && chmod 0444 "$EVID/$RSHA.json"; fi
echo "run_gb10_chip: VERDICT $VERDICT ${FAIL_REASON:+($FAIL_REASON) }receipt=$EVID/$RSHA.json"
rm -rf "$T"
[ "$VERDICT" = PASS ] && exit 0 || exit 1
