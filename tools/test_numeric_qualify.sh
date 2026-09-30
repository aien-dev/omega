#!/bin/bash
# test_numeric_qualify.sh -- host-only tests of the Gate 5 (OMEGA-NUMERIC-0)
# qualifier in tests/run_numeric_gates.sh: physics pin checks, gate log
# checks and the immutable digest-named receipt writer. No GPU, no device.
# Everything is written under a temporary directory, never into evidence/.
# The "hardware" descriptors here are made up for the test and say so
# (UNIT_TEST_FAKE_NOT_HARDWARE); they are never from a device.
# Run: make test-numeric-qualify   (or run this file).
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
# shellcheck source=tests/run_numeric_gates.sh
. "$HERE/tests/run_numeric_gates.sh"
m19r_build_canon || { echo "FAIL: cannot build tools/json_canon.c"; exit 1; }
TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP"; rm -rf "$TMP" "$M19R_TMP"' EXIT
fails=0
ok() { echo "  [PASS] $1"; }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
gitc() { git -C "$1" -c user.name=Test -c user.email=test@example.invalid "${@:2}"; }

echo "physics dependency comes from physics.lock"
phys=$TMP/physics
git init -q "$phys"
echo old > "$phys/README"
gitc "$phys" add README && gitc "$phys" commit -qm "no forge yet"
old=$(git -C "$phys" rev-parse HEAD)
omega=$TMP/omega
git init -q "$omega"
echo "$old" > "$omega/physics.lock"
gitc "$omega" add physics.lock && gitc "$omega" commit -qm lock
check "pinned commit without forge/ refused" '! num_check_physics "$omega" "$phys" "$old"'
check "  message names forge and the owner decision" '[[ $M19R_ERR == *"has no forge/forge_descriptor.h"*"owner decision"* ]]'
mkdir -p "$phys/forge" "$phys/nvrm" "$phys/m16"
for f in forge/forge_descriptor.h forge/forge_descriptor.c forge/forge_realize.c sha256_clean.c nvrm/nvrm.c m16/m16_native.c; do
    echo "/* stub */" > "$phys/$f"
done
gitc "$phys" add -A && gitc "$phys" commit -qm "forge"
new=$(git -C "$phys" rev-parse HEAD)
check "candidate differing from physics.lock refused" '! num_check_physics "$omega" "$phys" "$new"'
check "  message shows both commits" '[ "$M19R_ERR" = "physics.lock pins $old but --physics-candidate is $new" ]'
check "checkout not at the pinned commit refused" '! num_check_physics "$omega" "$phys" "$old"'
check "  message shows both commits" '[ "$M19R_ERR" = "physics checkout $phys is at $new but physics.lock pins $old" ]'
check "missing physics checkout refused" '! num_check_physics "$omega" "$TMP/nowhere" "$old"'
echo "$new" > "$omega/physics.lock"
gitc "$omega" commit -qam "move pin"
check "matching pin with forge/ accepted" 'num_check_physics "$omega" "$phys" "$new"'
echo dirty >> "$phys/README"
check "dirty physics tree refused" '! num_check_physics "$omega" "$phys" "$new"'
gitc "$phys" checkout -q README
echo "not-a-sha" > "$omega/physics.lock"
check "malformed physics.lock refused" '! num_check_physics "$omega" "$phys" "$new"'
echo "$new" > "$omega/physics.lock"

echo "gate log checks"
fake_digest=$(printf 'UNIT_TEST_FAKE_NOT_HARDWARE' | sha256sum | cut -d' ' -f1)
good_log() {
    local id op
    for id in $NUM_EXPECTED_IDS; do echo "[PASS] $id"; done
    echo "OMEGA_NUMERIC_HWDESC_JSON:{\"source\":\"FORGE_PROBE\",\"descriptor_digest\":\"$fake_digest\",\"derived_alias\":\"UNIT_TEST_FAKE_NOT_HARDWARE\"}"
    for op in $NUM_ENCODED_OPS; do
        case $op in
            MUFU_*) echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"$op\",\"tier\":\"gb10\",\"compare\":\"SEED_BOUND\",\"n\":4096,\"checked\":3000,\"out_of_bound\":0,\"skipped\":1096}";;
            *) echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"$op\",\"tier\":\"gb10\",\"compare\":\"BIT_EXACT\",\"n\":4096,\"checked\":4096,\"mismatches\":0}";;
        esac
        echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"$op\",\"tier\":\"cpu\",\"compare\":\"BIT_EXACT\",\"n\":4096,\"checked\":4096,\"mismatches\":0}"
    done
}
good_log > "$TMP/good.log"
check "complete passing log accepted" 'num_check_log "$TMP/good.log"'
check "  hardware digest read from the log" '[ "$NUM_HWDIGEST" = "$fake_digest" ]'
check "  13 GB10 parity lines kept" '[ "$(printf "%s" "$NUM_PARITY" | jq length)" = 13 ]'
sed 's/"source":"FORGE_PROBE"/"source":"FAKE_NON_HARDWARE_CPU_ONLY","fake":true/' "$TMP/good.log" > "$TMP/fake.log"
check "CPU-only fake descriptor refused" '! num_check_log "$TMP/fake.log"'
check "  as not a device probe" '[[ $M19R_ERR == "hardware descriptor is not a device probe"* ]]'
sed 's/"derived_alias"/"fake":true,"derived_alias"/' "$TMP/good.log" > "$TMP/fake2.log"
check "descriptor marked fake refused" '! num_check_log "$TMP/fake2.log"'
sed "s/$fake_digest/$(printf '0%.0s' {1..64})/" "$TMP/good.log" > "$TMP/zero.log"
check "all-zero descriptor digest refused" '! num_check_log "$TMP/zero.log"'
grep -v '"op":"SHFL_DOWN","tier":"gb10"' "$TMP/good.log" > "$TMP/noshfl.log"
check "missing GB10 parity line refused" '! num_check_log "$TMP/noshfl.log"'
check "  naming the op" '[ "$M19R_ERR" = "no GB10 parity line for SHFL_DOWN" ]'
sed 's/"op":"FFMA","tier":"gb10","compare":"BIT_EXACT","n":4096,"checked":4096,"mismatches":0/"op":"FFMA","tier":"gb10","compare":"BIT_EXACT","n":4096,"checked":4096,"mismatches":1/' "$TMP/good.log" > "$TMP/onebit.log"
check "one GB10 mismatch refused" '! num_check_log "$TMP/onebit.log"'
sed 's/"op":"FADD","tier":"gb10","compare":"BIT_EXACT","n":4096,"checked":4096,"mismatches":0/"op":"FADD","tier":"gb10","error":-4/' "$TMP/good.log" > "$TMP/err.log"
check "GB10 launch error refused" '! num_check_log "$TMP/err.log"'
sed 's/"out_of_bound":0/"out_of_bound":3/' "$TMP/good.log" > "$TMP/mufu.log"
check "MUFU seed out of bound refused" '! num_check_log "$TMP/mufu.log"'
sed 's/^\[PASS\] FP32_SIMT_OPCODES_ENCODED/[FAIL] FP32_SIMT_OPCODES_ENCODED/' "$TMP/good.log" > "$TMP/fail.log"
check "a failed test refused" '! num_check_log "$TMP/fail.log"'
grep -v 'NEG_FTZ_DETECTED_AND_REJECTED' "$TMP/good.log" > "$TMP/miss.log"
check "missing expected test refused" '! num_check_log "$TMP/miss.log"'
{ cat "$TMP/good.log"; echo "[PASS] CPU_GB10_BIT_PARITY"; } > "$TMP/dup.log"
check "duplicated test refused" '! num_check_log "$TMP/dup.log"'
{ cat "$TMP/good.log"; echo "[SKIP] CPU_GB10_BIT_PARITY (needs GB10)"; } > "$TMP/skip.log"
check "any SKIP refused" '! num_check_log "$TMP/skip.log"'
{ cat "$TMP/good.log"; echo "[PASS] EXTRA_UNLISTED_TEST"; } > "$TMP/extra.log"
check "unlisted extra test refused" '! num_check_log "$TMP/extra.log"'
cpu_bin=$HERE/build/test_omega_numeric_cpu
if [ -x "$cpu_bin" ]; then
    "$cpu_bin" > "$TMP/cpu.log" 2>&1
    check "real CPU-only run log refused (no receipt without the chip)" '! num_check_log "$TMP/cpu.log"'
fi

echo "receipt writer (temporary repo only)"
mkdir -p "$omega/evidence"
echo '{"historical":true}' > "$omega/evidence/m19r_gate5_omega_numeric_evidence.json"
NUM_OMEGA_CAND=$(git -C "$omega" rev-parse HEAD) NUM_PHYSICS_CAND=$new
NUM_RUN_COMMIT=$NUM_OMEGA_CAND NUM_BINARY_SHA=$(printf 'bin' | sha256sum | cut -d' ' -f1)
NUM_RUN_ID=unit-test NUM_TS=$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ) NUM_RUN_DIR=$TMP/run NUM_RECORD=1
mkdir -p "$NUM_RUN_DIR"
NUM_RUN_COMMIT=0000000000000000000000000000000000000000
check "run commit differing from candidate refused" '! num_receipt "$omega" "$TMP/good.log"'
check "  and nothing written" '[ ! -e "$omega/evidence/OMEGA-NUMERIC-0" ]'
NUM_RUN_COMMIT=$NUM_OMEGA_CAND
check "fake descriptor log writes no receipt" '! num_receipt "$omega" "$TMP/fake.log" && [ ! -e "$omega/evidence/OMEGA-NUMERIC-0" ]'
check "passing log writes a receipt" 'num_receipt "$omega" "$TMP/good.log"'
rec=$omega/evidence/OMEGA-NUMERIC-0/$NUM_DIGEST.json
check "  file named by its digest" '[ -f "$rec" ] && [ "$(jq "del(.receipt_digest)" "$rec" | "$JSON_CANON" --sha256)" = "$NUM_DIGEST" ]'
check "  mode 0444" '[ "$(stat -c %a "$rec")" = 444 ]'
check "  observed counts 19/19/0" '[ "$(jq -c "[.observed_test_count,.observed_pass_count,.observed_fail_count]" "$rec")" = "[19,19,0]" ]'
check "  candidate == run commit, trees clean" '[ "$(jq -r ".candidate_git_commit == .run_git_commit and .candidate_trees_clean.omega and .candidate_trees_clean.physics" "$rec")" = true ]'
check "  binary, hardware digest, UTC time, predecessor recorded" '[ "$(jq -r "(.candidate_binary_sha256|length)==64 and .hardware_descriptor_digest==\"$fake_digest\" and (.timestamp_utc|endswith(\"Z\")) and (.predecessor_historical_gate5_sha256|length)==64" "$rec")" = true ]'
check "  historical gate5 file untouched" '[ "$(cat "$omega/evidence/m19r_gate5_omega_numeric_evidence.json")" = "{\"historical\":true}" ]'
orig=$(sha256sum < "$rec")
check "second write of the same receipt refused" '! num_receipt "$omega" "$TMP/good.log"'
check "  original bytes unchanged" '[ "$(sha256sum < "$rec")" = "$orig" ]'
NUM_RECORD=0
NUM_TS=2000-01-01T00:00:00.000000Z
check "without --record only the preview is written" 'num_receipt "$omega" "$TMP/good.log" && [ "$(ls "$omega/evidence/OMEGA-NUMERIC-0" | wc -l)" = 1 ] && [ -f "$NUM_RUN_DIR/receipt-preview.json" ]'

if [ "$fails" -ne 0 ]; then
    echo "Gate 5 qualifier tests: $fails FAILED"
    exit 1
fi
echo "Gate 5 qualifier tests: all passed"
