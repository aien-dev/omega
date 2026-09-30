#!/bin/bash
# test_gate14_combine.sh -- host-only tests (no GPU) for tools/gate14_combine.sh.
# Feeds it synthetic leg receipts (digested with tools/json_canon.c) that are
# good, carry mismatched commits, come from dirty trees, miss a leg, or are
# tampered, and expects accept or refuse. Run: make test-gate14-combine.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
COMBINE=$HERE/tools/gate14_combine.sh
# shellcheck source=tools/gate14_combine.sh
. "$COMBINE"
g14_build_canon || { echo "FAIL: cannot build tools/json_canon.c"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP" "$G14_TMP"' EXIT
fails=0 passes=0
ok() { echo "  [PASS] $1"; passes=$((passes + 1)); }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OC=62f5ba5b8c2d3bf135a5db9132d8fb6a92e51741
PC=f63a6ef4c5dfa27fc32ee6e4f893bd2c42ec8a12
OC2=8b719ff000000000000000000000000000000000
DESC=1ce23d57112901c414ddc75ba35b65bc8c38fc0bb5e85f3a995634af456bf24b
BINH=4e07408562bedb8b60ce05c1decfe3ad16b72230967de01f640b7e4729b49fce
COMMON="\"candidate_git_commit\":\"$OC\",\"physics_candidate_git_commit\":\"$PC\",\"candidate_trees_clean\":{\"omega\":true,\"physics\":true}"
ACCEL=$(for i in $(seq 1 18); do printf '{"suite":"m19r","id":"OMEGA_ACCEL_RESIDENT_%02d","status":"PASS"},' "$i"; done)
M19R_BODY="{\"schema\":\"AIEN_M19R_QUALIFICATION_V1\",$COMMON,\"observed_gate_results_count\":20,\"observed_pass_count\":20,\"observed_fail_count\":0,\"test_results\":[${ACCEL}{\"suite\":\"m19\",\"id\":\"A_PASS\",\"status\":\"PASS\"},{\"suite\":\"m19\",\"id\":\"B_PASS\",\"status\":\"PASS\"}],\"soak\":{\"passed\":true,\"cycles\":100000,\"bytes_churned\":419430400000,\"physical_memory_bytes\":130661117952},\"m19_observations\":{\"m19_gates_completed\":18,\"m19_gates_passed\":18,\"regression_gates_completed\":157,\"regression_gates_passed\":157}}"
TR2='"test_results":[{"suite":"s","id":"X_PASS","status":"PASS"},{"suite":"s","id":"Y_PASS","status":"PASS"}],"observed_test_count":2,"observed_pass_count":2'
FORGE_BODY="{\"schema\":\"AIEN_M19R_FORGE_GATES_V1\",\"status\":\"PASS\",$COMMON,\"gates\":{\"GATE_3_FORGE_0\":{\"status\":\"PASS\",\"gate_binary_exit_status\":0,\"candidate_binary_sha256\":\"$BINH\"},\"GATE_4_FORGE_HWID\":{\"status\":\"PASS\",\"gate_binary_exit_status\":0,\"candidate_binary_sha256\":\"$BINH\",\"hardware_descriptor_digest\":\"$DESC\"}},\"hardware_descriptor_digest\":\"$DESC\",\"observed_fail_count\":0,$TR2}"
NUM_BODY="{\"schema\":\"AIEN_OMEGA_NUMERIC_0_V1\",\"status\":\"PASS\",$COMMON,\"run_git_commit\":\"$OC\",\"physics_lock\":\"$PC\",\"gate_binary_exit_status\":0,\"observed_fail_count\":0,$TR2,\"hardware_descriptor_digest\":\"$DESC\",\"timestamp_utc\":\"2026-09-30T18:42:38.311108Z\"}"

# mk NAME BODY [JQ_FILTER] -- write a leg receipt (pretty, digest over the
# canonical body after the filter) to TMP/NAME and print the path.
mk() {
    local b d
    b=$(printf '%s' "$2" | jq -c "${3:-.}") && d=$(printf '%s' "$b" | "$JSON_CANON" --sha256) &&
        printf '{"receipt_digest":"%s",%s' "$d" "${b#\{}" | "$JSON_CANON" --pretty > "$TMP/$1" &&
        echo "$TMP/$1"
}
run() { "$COMBINE" "$@" > "$TMP/out" 2> "$TMP/err"; }
refused() { ! run "$@" && grep -q "^REFUSED: .*$REASON" "$TMP/err"; }

M=$(mk m19r.json "$M19R_BODY"); F=$(mk forge.json "$FORGE_BODY"); N=$(mk num.json "$NUM_BODY")

echo "good receipts accepted"
EV=$TMP/evidence/GATE14-FOUNDATION
check "three good legs accepted" 'run --evidence-dir "$EV" "$M" "$F" "$N"'
out=$(ls "$EV"/*.json 2>/dev/null | head -n 1)
check "combined receipt written as <digest>.json" '[ -f "$out" ] && [ "$(basename "$out" .json)" = "$(jq -r .receipt_digest "$out")" ]'
check "combined receipt is 0444" '[ "$(stat -c %a "$out")" = 444 ]'
check "combined receipt is 0444 under umask 077" '( umask 077; run --evidence-dir "$TMP/ev077" "$M" "$F" "$N" ) && [ "$(stat -c %a "$TMP"/ev077/*.json)" = 444 ]'
check "combined digest recomputes" '[ "$(jq -c "del(.receipt_digest)" "$out" | "$JSON_CANON" --sha256)" = "$(jq -r .receipt_digest "$out")" ]'
check "eight criteria, all PASS" '[ "$(jq "[.criteria[] | select(.status == \"PASS\")] | length" "$out")" = 8 ]'
check "names the one commit pair" '[ "$(jq -r "[.candidate_git_commit, .physics_candidate_git_commit] | join(\" \")" "$out")" = "$OC $PC" ]'
check "references the three leg digests" '[ "$(jq -r "[.constituent_receipts[].receipt_digest] | sort | join(\" \")" "$out")" = "$(for r in "$M" "$F" "$N"; do jq -r .receipt_digest "$r"; done | sort | paste -sd" ")" ]'
check "FORGE_BOUNDARY backed by the Gate 3/4 receipt" '[ "$(jq -r .criteria.FORGE_BOUNDARY.receipt_digest "$out")" = "$(jq -r .receipt_digest "$F")" ]'
check "leg order does not matter" 'run "$N" "$M" "$F"'
check "expected pair given and matching" 'run --omega-candidate "$OC" --physics-candidate "${PC^^}" "$M" "$F" "$N"'
check "digest-named leg file accepted" 'cp "$F" "$TMP/$(jq -r .receipt_digest "$F").json" && run "$M" "$TMP/$(jq -r .receipt_digest "$F").json" "$N"'

echo "mismatched commits refused"
REASON="omega commit mismatch"
check "Gate 5 leg on another omega commit" 'refused "$M" "$F" "$(mk n2.json "$NUM_BODY" ".candidate_git_commit = \"$OC2\" | .run_git_commit = \"$OC2\"")"'
REASON="physics commit mismatch"
check "Gate 3/4 leg on another physics commit" 'refused "$M" "$(mk f2.json "$FORGE_BODY" ".physics_candidate_git_commit = \"$OC2\"")" "$N"'
REASON="not the expected"
check "all legs agree but not on the expected omega" 'refused --omega-candidate "$OC2" "$M" "$F" "$N"'
REASON="not a full 40-hex id"
check "short commit id" 'refused "$(mk m2.json "$M19R_BODY" ".candidate_git_commit = \"62f5ba5\"")" "$F" "$N"'

echo "dirty trees refused"
REASON="not made from clean omega and physics trees"
check "physics tree dirty on Gate 1/2 leg" 'refused "$(mk m3.json "$M19R_BODY" ".candidate_trees_clean.physics = false")" "$F" "$N"'
check "omega tree dirty on Gate 3/4 leg" 'refused "$M" "$(mk f3.json "$FORGE_BODY" ".candidate_trees_clean.omega = false")" "$N"'
check "clean flag missing" 'refused "$M" "$F" "$(mk n3.json "$NUM_BODY" "del(.candidate_trees_clean.physics)")"'
check "clean flag as a string" 'refused "$M" "$F" "$(mk n4.json "$NUM_BODY" ".candidate_trees_clean.omega = \"true\"")"'

echo "missing legs refused"
REASON="missing leg: no receipt backs FORGE_BOUNDARY"
check "no Gate 3/4 receipt" 'refused "$M" "$N"'
REASON="missing leg: no receipt backs EVIDENCE_IMMUTABLE"
check "no Gate 1/2 receipt" 'refused "$F" "$N"'
REASON="missing leg: no receipt backs FP32_CPU_GB10_PARITY"
check "no Gate 5 receipt" 'refused "$M" "$F"'
REASON="cannot read receipt"
check "named file does not exist" 'refused "$M" "$F" "$TMP/nope.json"'
check "refusals wrote nothing" '[ "$(ls "$EV" | wc -l)" = 1 ]'

echo "tampered or failing legs refused"
REASON="receipt_digest does not match its content"
check "edited after digesting" 'sed "s/\"observed_pass_count\": 2/\"observed_pass_count\": 3/" "$F" > "$TMP/t.json" && refused "$M" "$TMP/t.json" "$N"'
REASON="named for a different digest"
check "file named for another digest" 'cp "$F" "$TMP/$(jq -r .receipt_digest "$N").json" && refused "$M" "$TMP/$(jq -r .receipt_digest "$N").json" "$N"'
REASON="does not record a PASS"
check "Gate 1/2 with a failed gate" 'refused "$(mk m4.json "$M19R_BODY" ".observed_fail_count = 1 | .observed_pass_count = 1 | .test_results[1].status = \"FAIL\"")" "$F" "$N"'
check "Gate 1/2 with a failed soak" 'refused "$(mk m5.json "$M19R_BODY" ".soak.passed = false")" "$F" "$N"'
check "Gate 1/2 counts that do not add up" 'refused "$(mk m6.json "$M19R_BODY" ".observed_pass_count = 1")" "$F" "$N"'
check "Gate 3/4 with gate 4 FAIL" 'refused "$M" "$(mk f4.json "$FORGE_BODY" ".status = \"FAIL\" | .gates.GATE_4_FORGE_HWID.status = \"FAIL\"")" "$N"'
check "Gate 3/4 status PASS but gate 3 exit 1" 'refused "$M" "$(mk f5.json "$FORGE_BODY" ".gates.GATE_3_FORGE_0.gate_binary_exit_status = 1")" "$N"'
check "Gate 5 status FAIL" 'refused "$M" "$F" "$(mk n5.json "$NUM_BODY" ".status = \"FAIL\"")"'
check "Gate 3/4 status PASS but one test result FAIL" 'refused "$M" "$(mk f6.json "$FORGE_BODY" ".test_results[1].status = \"FAIL\"")" "$N"'
check "Gate 3/4 without test results" 'refused "$M" "$(mk f7.json "$FORGE_BODY" "del(.test_results)")" "$N"'
check "Gate 5 status PASS but one test result FAIL" 'refused "$M" "$F" "$(mk n8.json "$NUM_BODY" ".test_results[0].status = \"FAIL\"")"'
check "Gate 5 test count does not match results" 'refused "$M" "$F" "$(mk n9.json "$NUM_BODY" ".observed_test_count = 3")"'
check "Gate 5 ran on another omega commit" 'refused "$M" "$F" "$(mk n10.json "$NUM_BODY" ".run_git_commit = \"$OC2\"")"'
check "Gate 5 physics.lock names another physics commit" 'refused "$M" "$F" "$(mk n11.json "$NUM_BODY" ".physics_lock = \"$OC2\"")"'
check "Gate 1/2 soak too short" 'refused "$(mk m7.json "$M19R_BODY" ".soak.cycles = 99999")" "$F" "$N"'
check "Gate 1/2 soak churned too little memory" 'refused "$(mk m8.json "$M19R_BODY" ".soak.bytes_churned = 1000")" "$F" "$N"'
check "Gate 1/2 missing an OMEGA_ACCEL_RESIDENT_ result" 'refused "$(mk m9.json "$M19R_BODY" ".test_results |= .[1:] | .observed_gate_results_count = 19 | .observed_pass_count = 19")" "$F" "$N"'
check "Gate 1/2 with one resident id repeated 18 times" 'refused "$(mk m11.json "$M19R_BODY" ".test_results |= (map(select(.id | startswith(\"OMEGA_ACCEL_RESIDENT_\") | not)) + [range(18) | {suite:\"m19r\",id:\"OMEGA_ACCEL_RESIDENT_01\",status:\"PASS\"}]) | .observed_gate_results_count = 20 | .observed_pass_count = 20")" "$F" "$N"'
check "Gate 1/2 with a regression gate failure" 'refused "$(mk m12.json "$M19R_BODY" ".m19_observations.regression_gates_passed = 156")" "$F" "$N"'
check "Gate 1/2 without m19 observations" 'refused "$(mk m13.json "$M19R_BODY" "del(.m19_observations)")" "$F" "$N"'
check "Gate 3/4 with an empty binary hash" 'refused "$M" "$(mk f9.json "$FORGE_BODY" ".gates.GATE_3_FORGE_0.candidate_binary_sha256 = \"\"")" "$N"'
check "Gate 3/4 gate 4 descriptor differs from top level" 'refused "$M" "$(mk f10.json "$FORGE_BODY" ".gates.GATE_4_FORGE_HWID.hardware_descriptor_digest = \"$(printf "%064d" 1)\"")" "$N"'
check "Gate 3/4 without a descriptor digest" 'refused "$M" "$(mk f11.json "$FORGE_BODY" "del(.hardware_descriptor_digest) | del(.gates.GATE_4_FORGE_HWID.hardware_descriptor_digest)")" "$N"'
check "Gate 5 without a descriptor digest" 'refused "$M" "$F" "$(mk n13.json "$NUM_BODY" "del(.hardware_descriptor_digest)")"'
REASON="hardware_descriptor_digest is not 64 hex"
check "descriptor digest carrying JSON is refused" 'refused "$(mk m10.json "$M19R_BODY" ".hardware_descriptor_digest = \"x\\\",\\\"candidate_git_commit\\\":\\\"$OC2\"")" "$(mk f8.json "$FORGE_BODY" "del(.hardware_descriptor_digest)")" "$(mk n12.json "$NUM_BODY" "del(.hardware_descriptor_digest)")"'
check "all legs agreeing on an all-zero descriptor" 'z=$(printf "%064d" 0); refused "$M" "$(mk f12.json "$FORGE_BODY" ".hardware_descriptor_digest = \"$z\" | .gates.GATE_4_FORGE_HWID.hardware_descriptor_digest = \"$z\"")" "$(mk n14.json "$NUM_BODY" ".hardware_descriptor_digest = \"$z\"")"'
REASON="named for a different digest"
check "file named for another digest in upper case" 'u=$(jq -r .receipt_digest "$N"); cp "$F" "$TMP/${u^^}.json" && refused "$M" "$TMP/${u^^}.json" "$N"'
REASON="two receipts for AIEN_OMEGA_NUMERIC_0_V1"
check "same leg twice" 'refused "$M" "$F" "$N" "$(mk n6.json "$NUM_BODY" ".timestamp_utc = \"x\"")"'
REASON="unknown schema"
check "unknown schema" 'refused "$M" "$F" "$N" "$(mk u.json "$NUM_BODY" ".schema = \"AIEN_SOMETHING_V1\"")"'
REASON="is not valid JSON"
check "not JSON" 'echo "{" > "$TMP/bad.json" && refused "$M" "$F" "$TMP/bad.json"'
REASON="hardware_descriptor_digest differs"
check "legs on different hardware descriptors" 'refused "$M" "$F" "$(mk n7.json "$NUM_BODY" ".hardware_descriptor_digest = \"$(printf "%064d" 1)\"")"'
REASON="cannot write"
check "second write of the same combined receipt refused" 'cp -r "$EV" "$TMP/ev2" && chmod u+w "$TMP/ev2" && t=$(jq -r .timestamp_utc "$out") && g14_combine "" "" "$t" "$M" "$F" "$N" && ! g14_write "$TMP/ev2" && [ "$G14_DIGEST" = "$(basename "$out" .json)" ]'

echo "bad arguments"
check "no receipts is a usage error" '"$COMBINE" > /dev/null 2>&1; [ $? = 2 ]'
check "short expected candidate is a usage error" '"$COMBINE" --omega-candidate 62f5ba5 "$M" > /dev/null 2>&1; [ $? = 2 ]'

echo "Gate 14 combiner tests: $passes passed, $fails failed"
[ "$fails" -eq 0 ]
