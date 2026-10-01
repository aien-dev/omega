#!/bin/bash
# run_divsqrt_gate.sh -- E1 row 7 chip gate: correctly rounded FP32 DIV and
# SQRT on the GB10, bit-identical to the CPU semantic (omega_math_div/sqrt and
# the AArch64 FDIV/FSQRT, NaN results canonical 0x7fc00000).
#
# Order: refuse a dirty omega tree or a physics checkout that is dirty or not
# at the physics.lock pin; refuse while ~/workspace/.spark-quiet exists, an
# est_load process runs or /tmp/aien-gb10.lock is held; run the host tier and
# the offline nvdisasm provenance check; raise the quiet flag; take the GPU
# lock; build the chip binary (no libm math, no CUDA symbols); run it to the
# end (never killed, never timed out); write a content-addressed receipt
# <evidence-dir>/<sha256>.json (mode 0444, created exclusively, outside the
# tree) plus the log as blobs/<sha256>.log; drop the flag.
#
# Run it detached so a closed terminal cannot cut the chip run:
#   setsid nohup tools/run_divsqrt_gate.sh > /tmp/divsqrt-gate.out 2>&1 < /dev/null &
# Shell + coreutils + git + jq + gcc + nvdisasm. No Python.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
PHYSICS=${PHYSICS_DIR:-$HOME/workspace/hive-worktrees/physics-gate14-e95e3ed}
EVID=${DIVSQRT_EVIDENCE_DIR:-$HOME/workspace/evidence-out/E1-DIVSQRT}
QUIET=$HOME/workspace/.spark-quiet
OWNER="lane2 E1 DIV/SQRT chip run"
RUN=$(mktemp -d /tmp/divsqrt-run.XXXXXX)
FLAG_MINE=0
refuse() { echo "REFUSED: $*"; echo "VERDICT NOT_RUN"; exit 1; }
cleanup() { if [ "$FLAG_MINE" = 1 ] && grep -q "^$OWNER" "$QUIET" 2>/dev/null; then rm -f "$QUIET"; fi; }
trap cleanup EXIT
sha() { sha256sum "$1" | cut -d' ' -f1; }

case "$(realpath -m "$EVID")/" in "$HERE"/*|"$(realpath -m "$PHYSICS")"/*) refuse "evidence dir $EVID is inside a candidate tree";; esac
[ -z "$(git -C "$HERE" status --porcelain)" ] || refuse "omega tree $HERE is dirty"
OMEGA_COMMIT=$(git -C "$HERE" rev-parse HEAD) || refuse "cannot read omega HEAD"
PIN=$(tr -d '[:space:]' < "$HERE/physics.lock")
PHYS_COMMIT=$(git -C "$PHYSICS" rev-parse HEAD) || refuse "cannot read physics HEAD at $PHYSICS"
[ "$PHYS_COMMIT" = "$PIN" ] || refuse "physics checkout is at $PHYS_COMMIT, physics.lock pins $PIN"
[ -z "$(git -C "$PHYSICS" status --porcelain)" ] || refuse "physics checkout $PHYSICS is dirty"
[ -e "$QUIET" ] && refuse "quiet flag is up: $(cat "$QUIET")"
[ -n "$(pgrep est_load)" ] && refuse "an est_load process is running"
fuser /tmp/aien-gb10.lock >/dev/null 2>&1 && refuse "/tmp/aien-gb10.lock is held"

echo "== host tier"
make -s -C "$HERE" build/test_omega_divsqrt_gb10_cpu > "$RUN/host-build.log" 2>&1 || refuse "host build failed"
"$HERE/build/test_omega_divsqrt_gb10_cpu" > "$RUN/host.log" 2>&1; HOST_RC=$?
tail -1 "$RUN/host.log"
echo "== nvdisasm provenance"
"$HERE/tools/divsqrt_nvdisasm_check.sh" > "$RUN/nvdisasm.log" 2>&1; NVD_RC=$?
grep -E '^\[|^VERDICT|^nvdisasm' "$RUN/nvdisasm.log"
[ "$HOST_RC" = 0 ] && [ "$NVD_RC" = 0 ] || refuse "host tier ($HOST_RC) or nvdisasm check ($NVD_RC) failed; no chip run"
KDIG=$("$HERE/build/test_omega_divsqrt_gb10_cpu" --digest)

START=$(date -u +%Y-%m-%dT%H:%M:%SZ)
set -o noclobber
echo "$OWNER start=$START expected_end=$(date -u -d '+3 hours' +%Y-%m-%dT%H:%M:%SZ) pid=$$" > "$QUIET" || refuse "could not create the quiet flag"
set +o noclobber
FLAG_MINE=1
exec 9> /tmp/aien-gb10.lock || refuse "cannot open /tmp/aien-gb10.lock"
flock -x 9 || refuse "cannot take /tmp/aien-gb10.lock"

echo "== chip build"
NV=$PHYSICS/third_party/nvidia-open-580.173.02
BIN=$RUN/test_omega_divsqrt_gb10
(cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -pthread \
    -Isrc -I"$PHYSICS/nvrm" -I"$PHYSICS/m16" \
    -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" \
    -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include" \
    -o "$BIN" tests/test_omega_divsqrt_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric.c \
    src/omega_numeric_provenance.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c \
    src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c \
    "$PHYSICS/nvrm/nvrm.c" "$PHYSICS/m16/m16_native.c") > "$RUN/chip-build.log" 2>&1 || { cat "$RUN/chip-build.log"; refuse "chip build failed"; }
if nm -u "$BIN" | grep -Eq '\b(sqrtf?|expf?|logf?|powf?|fmaf?|sinf?|cosf?|fdiv|divf)\b'; then refuse "libm math symbols in $BIN"; fi
if nm -u "$BIN" | grep -Eiq 'cuda|cuInit|cuLaunch|nvrtc|cublas'; then refuse "CUDA symbols in $BIN"; fi
BIN_SHA=$(sha "$BIN")

echo "== chip run (not killed, not timed out)"
"$BIN" --chip > "$RUN/chip.log" 2>&1; CHIP_RC=$?
END=$(date -u +%Y-%m-%dT%H:%M:%SZ)
exec 9>&-
grep -E '^RESULT|^VERDICT' "$RUN/chip.log"

OMEGA_CLEAN_AFTER=$([ -z "$(git -C "$HERE" status --porcelain)" ] && echo true || echo false)
PHYS_CLEAN_AFTER=$([ -z "$(git -C "$PHYSICS" status --porcelain)" ] && echo true || echo false)
OMEGA_SAME_AFTER=$([ "$(git -C "$HERE" rev-parse HEAD 2>/dev/null)" = "$OMEGA_COMMIT" ] && echo true || echo false)
PHYS_SAME_AFTER=$([ "$(git -C "$PHYSICS" rev-parse HEAD 2>/dev/null)" = "$PHYS_COMMIT" ] && echo true || echo false)
field() { grep "^RESULT chip $1 " "$RUN/chip.log" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }
VERDICT=$(sed -n 's/^VERDICT \([A-Z_]*\).*/\1/p' "$RUN/chip.log" | tail -1)
[ -n "$VERDICT" ] || VERDICT=NOT_RUN
[ "$CHIP_RC" = 0 ] || [ "$VERDICT" != PASS ] || VERDICT=FAIL
[ "$OMEGA_CLEAN_AFTER" = true ] && [ "$PHYS_CLEAN_AFTER" = true ] && [ "$OMEGA_SAME_AFTER" = true ] && [ "$PHYS_SAME_AFTER" = true ] || { [ "$VERDICT" = PASS ] && VERDICT=FAIL; }

mkdir -p "$EVID/blobs"
LOG_SHA=$(sha "$RUN/chip.log")
[ -e "$EVID/blobs/$LOG_SHA.log" ] || { cp "$RUN/chip.log" "$EVID/blobs/$LOG_SHA.log" && chmod 0444 "$EVID/blobs/$LOG_SHA.log"; } || { echo "could not store the chip log blob"; echo "VERDICT FAIL"; exit 1; }
num() { local v; v=$(field "$1" "$2"); echo "${v:-null}"; }
BODY=$(jq -n --arg gate "E1-DIVSQRT" --arg omega "$OMEGA_COMMIT" --arg phys "$PHYS_COMMIT" --arg pin "$PIN" \
    --argjson oclean true --argjson pclean true --argjson oafter "$OMEGA_CLEAN_AFTER" --argjson pafter "$PHYS_CLEAN_AFTER" \
    --argjson osame "$OMEGA_SAME_AFTER" --argjson psame "$PHYS_SAME_AFTER" \
    --arg bin "$BIN_SHA" --arg log "$LOG_SHA" --arg kd "$KDIG" --arg start "$START" --arg end "$END" \
    --arg nvd "$(grep '^nvdisasm' "$RUN/nvdisasm.log")" --arg host "$(tail -1 "$RUN/host.log")" \
    --argjson dchk "$(num DIV checked)" --argjson dedge "$(num DIV edge_and_corpus)" \
    --argjson dbad "$(num DIV mismatches)" --argjson dmath "$(num DIV mismatches_vs_omega_math)" --argjson dfill "$(num DIV unwritten)" \
    --arg dver "$(field DIV verdict)" --arg seed "$(field DIV seed)" \
    --argjson schk "$(num SQRT checked)" --argjson sbad "$(num SQRT mismatches)" \
    --argjson smath "$(num SQRT mismatches_vs_omega_math)" --argjson sfill "$(num SQRT unwritten)" --arg sver "$(field SQRT verdict)" \
    --arg verdict "$VERDICT" --argjson rc "$CHIP_RC" '{
  gate: $gate, row: "E1 gap table row 7: GB10 correctly rounded FP32 DIV and SQRT",
  omega_commit: $omega, omega_tree_clean_before: $oclean, omega_tree_clean_after: $oafter, omega_commit_unchanged_after: $osame,
  physics_commit: $phys, physics_lock_pin: $pin, physics_tree_clean_before: $pclean, physics_tree_clean_after: $pafter, physics_commit_unchanged_after: $psame,
  binary_sha256: $bin, chip_log_sha256: $log, kernels: ($kd | split("\n") | map(select(length > 0) | split(" ") | {op: .[0], instructions: (.[1] | tonumber), sha256: .[2]})),
  nvdisasm_check: $nvd, host_tier: $host,
  div: {checked: $dchk, edge_and_corpus: $dedge, mismatches_vs_fdiv: $dbad, mismatches_vs_omega_math_div: $dmath, mismatches_unwritten: $dfill, rng_seed: $seed, verdict: $dver},
  sqrt: {checked: $schk, exhaustive: ($schk == 4294967296), mismatches_vs_fsqrt: $sbad, mismatches_vs_omega_math_sqrt: $smath, mismatches_unwritten: $sfill, verdict: $sver},
  chip_exit_status: $rc, started_utc: $start, finished_utc: $end, verdict: $verdict }')
[ -n "$BODY" ] && printf '%s\n' "$BODY" > "$RUN/receipt.json" && jq -e . "$RUN/receipt.json" >/dev/null || { echo "receipt JSON could not be built"; echo "VERDICT FAIL"; exit 1; }
RSHA=$(sha "$RUN/receipt.json")
OUT=$EVID/$RSHA.json
if [ -e "$OUT" ]; then echo "receipt $OUT already exists (identical content)"; else
    set -o noclobber; { cat "$RUN/receipt.json" > "$OUT" && chmod 0444 "$OUT"; } || { echo "could not write receipt $OUT"; echo "VERDICT FAIL"; exit 1; }; set +o noclobber; fi
cmp -s "$RUN/receipt.json" "$OUT" || { echo "receipt $OUT does not match what was built"; echo "VERDICT FAIL"; exit 1; }
echo "RECEIPT $OUT"
echo "VERDICT $VERDICT"
[ "$VERDICT" = PASS ] || exit 1
exit 0
