#!/bin/bash
# tests/test_m20_receipt.sh -- tests for tools/m20_receipt.sh (M20 receipt writer).
# Fixture logs and tables under tests/fixtures/m20_receipt/. Each scenario
# commits a fixture table as docs/tensor/M20_OMEGA_TENSOR.md in a throwaway git
# repo under a temp dir and writes evidence outside it. Covers reproducibility
# (two runs, same hash, byte-identical), every refusal path and every
# NOT QUALIFIED rule. M20_RECEIPT_WRITER selects the writer (used by
# tools/m20_receipt_mutations.sh to run mutants). Host only, no device.
# Shell + coreutils + git + jq. No Python. Exit 0 only if every check passes.
set -u
cd "$(dirname "$0")/.." || exit 2
ROOT=$(pwd)
W=${M20_RECEIPT_WRITER:-$ROOT/tools/m20_receipt.sh}
FX=$ROOT/tests/fixtures/m20_receipt
T=$(mktemp -d "${TMPDIR:-/tmp}/m20-receipt-test.XXXXXX") || exit 2
trap 'chmod -R u+w "$T" 2> /dev/null; rm -rf "$T"' EXIT INT TERM
G="git -c user.name=m20-test -c user.email=m20-test@invalid -c commit.gpgsign=false -c init.defaultBranch=main"
PASSLOG=$FX/tensor_pass.log; FAILLOG=$FX/tensor_fail.log; MUTLOG=$FX/mutations_pass.log
fail=0; total=0
check() { # NAME CONDITION-EXIT-STATUS
    total=$((total + 1))
    if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi
}
mkrepo() { # NAME TABLE (fixture tests/fixtures/m20_receipt/table_<TABLE>) -> prints the repo path
    local r=$T/repo-$1
    mkdir -p "$r/docs/tensor" && cp "$FX/table_$2" "$r/docs/tensor/M20_OMEGA_TENSOR.md" \
        && $G -C "$r" init -q && $G -C "$r" add -A && $G -C "$r" commit -qm fixture \
        || { echo "FATAL: cannot create fixture repo $1"; exit 2; }
    echo "$r"
}
# run NAME REPO EVID TENSOR-LOG [writer args...] -> sets RC, OUT, REC
run() {
    local name=$1 repo=$2 evid=$3 tlog=$4; shift 4
    OUT=$T/$name.out
    "$W" --repo "$repo" --evidence-dir "$evid" --tensor-log "$tlog" --mutations-log "$MUTLOG" "$@" > "$OUT" 2>&1
    RC=$?
    REC=$(sed -n 's/^receipt: \([^ ]*\) .*/\1/p' "$OUT")
}
nojson() { ! ls "$1"/*.json > /dev/null 2>&1; }
jqok() { jq -e "$2" "$1" > /dev/null 2>&1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

# 1. All rows host PASS (+ REFERENCE rows), no GB10 parity row: host-only
#    evidence never qualifies -> NOT QUALIFIED; sealed, content-addressed.
R=$(mkrepo all all_pass.md)
run all1 "$R" "$T/ev-a" "$PASSLOG" --binary "$FX/binary_stub.txt"; A=$REC
[ "$RC" = 1 ]; check "all host PASS: exit 1 (NOT QUALIFIED)" $?
[ -n "$A" ] && jqok "$A" '.verdict == "NOT QUALIFIED" and .hardware == "host" and .gate == "M20_OMEGA_TENSOR" and (.verdict_reasons | any(test("GB10 parity")))'; check "all host PASS: NOT QUALIFIED, hardware host, GB10 parity reason" $?
[ -n "$A" ] && [ "$(basename "$A" .json)" = "$(sha "$A")" ]; check "receipt name is its sha256" $?
[ -n "$A" ] && [ "$(stat -c %a "$A")" = 444 ]; check "receipt mode 0444" $?
[ -f "$T/ev-a/blobs/$(sha "$PASSLOG").log" ] && [ -f "$T/ev-a/blobs/$(sha "$MUTLOG").log" ] \
    && [ -f "$T/ev-a/blobs/$(sha "$FX/binary_stub.txt").bin" ]; check "log and binary blobs stored by digest" $?
[ -n "$A" ] && jqok "$A" "(.logs.test_tensor_sha256 == \"$(sha "$PASSLOG")\") and (.binaries[0].sha256 == \"$(sha "$FX/binary_stub.txt")\") and (.omega_commit == \"$($G -C "$R" rev-parse HEAD)\") and .omega_tree_clean_after and .omega_commit_unchanged_after and ([.rows[] | select(.label == \"host PASS\")] | length) == 3 and ([.rows[] | select(.label == \"REFERENCE\")] | length) == 2 and (has(\"started_utc\") | not)"
check "receipt fields: log/binary/commit hashes, clean after, row labels, no clock" $?

# 2. Reproducibility: same inputs, second evidence dir -> same hash, same bytes.
run all2 "$R" "$T/ev-b" "$PASSLOG" --binary "$FX/binary_stub.txt"; B=$REC
[ "$RC" = 1 ] && [ -n "$A" ] && [ "$(basename "$A")" = "$(basename "$B")" ] && cmp -s "$A" "$B"; check "reproducible: two runs give the same hash and bytes" $?
run ts1 "$R" "$T/ev-ts1" "$PASSLOG" --started-utc 2026-10-01T00:00:00Z --finished-utc 2026-10-01T00:01:00Z; T1=$REC
run ts2 "$R" "$T/ev-ts2" "$PASSLOG" --started-utc 2026-10-01T00:00:00Z --finished-utc 2026-10-01T00:01:00Z; T2=$REC
run ts3 "$R" "$T/ev-ts3" "$PASSLOG" --started-utc 2026-10-01T00:00:01Z --finished-utc 2026-10-01T00:01:00Z; T3=$REC
[ -n "$T1" ] && [ "$(basename "$T1")" = "$(basename "$T2")" ] && [ "$(basename "$T1")" != "$(basename "$T3")" ] \
    && jqok "$T1" '.started_utc == "2026-10-01T00:00:00Z"'; check "timestamps only when passed in, and they are hashed" $?

# 3. Never overwritten: rerun into the same dir, and a squatter with the same name.
run again "$R" "$T/ev-a" "$PASSLOG" --binary "$FX/binary_stub.txt"
[ "$RC" = 2 ] && grep -q 'already exists' "$OUT"; check "rerun into same evidence dir refused" $?
mkdir -p "$T/ev-sq" && [ -n "$A" ] && echo squatter > "$T/ev-sq/$(basename "$A")" && chmod 0644 "$T/ev-sq/$(basename "$A")"
run squat "$R" "$T/ev-sq" "$PASSLOG" --binary "$FX/binary_stub.txt"
[ "$RC" = 2 ] && [ "$(cat "$T/ev-sq/$(basename "$A")" 2> /dev/null)" = squatter ]; check "existing writable receipt name not overwritten" $?

# 4. NOT_RUN / MISSING_IMPLEMENTATION (GB10 parity PASS, so only that rule
#    can fail them) / nothing counted -> NOT QUALIFIED (receipt written).
R=$(mkrepo notrun not_run.md); run notrun "$R" "$T/ev-nr" "$PASSLOG" --chip-evidence "$FX/chip_evidence.json"
[ "$RC" = 1 ] && [ -n "$REC" ] && jqok "$REC" '.verdict == "NOT QUALIFIED" and (.verdict_reasons | any(test("NOT_RUN")))'; check "NOT_RUN row forces NOT QUALIFIED" $?
R=$(mkrepo missing missing.md); run missing "$R" "$T/ev-mi" "$PASSLOG" --chip-evidence "$FX/chip_evidence.json"
[ "$RC" = 1 ] && [ -n "$REC" ] && jqok "$REC" '.verdict == "NOT QUALIFIED" and (.verdict_reasons | any(test("MISSING_IMPLEMENTATION")))'; check "MISSING_IMPLEMENTATION row forces NOT QUALIFIED" $?
R=$(mkrepo refonly reference_only.md); run refonly "$R" "$T/ev-ro" "$PASSLOG"
[ "$RC" = 1 ] && [ -n "$REC" ] && jqok "$REC" '.verdict == "NOT QUALIFIED"'; check "only REFERENCE rows -> NOT QUALIFIED" $?

# 5. Refusals: exit 2 and no receipt.
R=$(mkrepo dirty all_pass.md); echo stray > "$R/stray.txt"; run dirty "$R" "$T/ev-di" "$PASSLOG"
[ "$RC" = 2 ] && nojson "$T/ev-di"; check "dirty tree refused" $?
R=$(mkrepo inside all_pass.md); echo "evidence/" > "$R/.git/info/exclude"; run inside "$R" "$R/evidence" "$PASSLOG"
[ "$RC" = 2 ] && nojson "$R/evidence"; check "evidence dir inside the tree refused" $?
R=$(mkrepo unknown unknown.md); run unknown "$R" "$T/ev-un" "$PASSLOG"
[ "$RC" = 2 ] && nojson "$T/ev-un"; check "unknown row label refused" $?
R=$(mkrepo hostfail all_pass.md); run hostfail "$R" "$T/ev-hf" "$FAILLOG"
[ "$RC" = 2 ] && nojson "$T/ev-hf"; check "host PASS rows with a failing host log refused" $?

# 6. GB10 PASS rows: need the chip evidence file and its hash in the row.
R=$(mkrepo gb10 gb10.md)
run gb10ok "$R" "$T/ev-g1" "$PASSLOG" --chip-evidence "$FX/chip_evidence.json"
[ "$RC" = 0 ] && [ -n "$REC" ] && jqok "$REC" ".verdict == \"QUALIFIED\" and .hardware == \"GB10\" and .chip_evidence_sha256 == \"$(sha "$FX/chip_evidence.json")\"" \
    && [ -f "$T/ev-g1/blobs/$(sha "$FX/chip_evidence.json").json" ]; check "GB10 PASS with matching chip evidence -> QUALIFIED, hardware GB10" $?
run gb10none "$R" "$T/ev-g2" "$PASSLOG"
[ "$RC" = 2 ] && nojson "$T/ev-g2"; check "GB10 PASS without chip evidence refused" $?
run gb10other "$R" "$T/ev-g3" "$PASSLOG" --chip-evidence "$FX/chip_evidence_other.json"
[ "$RC" = 2 ] && nojson "$T/ev-g3"; check "GB10 PASS with non-matching chip evidence refused" $?
R=$(mkrepo gb10nh gb10_nohash.md); run gb10nh "$R" "$T/ev-g4" "$PASSLOG"
[ "$RC" = 2 ] && nojson "$T/ev-g4"; check "GB10 PASS row naming no evidence, no chip file, refused" $?

# 7. Tree changes during the run (test-only hook) -> NOT QUALIFIED with the reason.
R=$(mkrepo middirty all_pass.md)
M20_RECEIPT_TEST_MIDRUN="echo x > '$R/late.txt'" run middirty "$R" "$T/ev-md" "$PASSLOG"
[ "$RC" = 1 ] && [ -n "$REC" ] && jqok "$REC" '(.omega_tree_clean_after | not) and (.verdict_reasons | any(test("dirty after")))'; check "tree dirtied mid-run -> NOT QUALIFIED" $?
R=$(mkrepo midcommit all_pass.md)
M20_RECEIPT_TEST_MIDRUN="$G -C '$R' commit -q --allow-empty -m moved" run midcommit "$R" "$T/ev-mc" "$PASSLOG"
[ "$RC" = 1 ] && [ -n "$REC" ] && jqok "$REC" '(.omega_commit_unchanged_after | not) and (.verdict_reasons | any(test("HEAD moved")))'; check "HEAD moved mid-run -> NOT QUALIFIED" $?

if [ "$fail" -ne 0 ]; then echo "test-m20-receipt: FAIL"; exit 1; fi
echo "test-m20-receipt: PASS ($total of $total checks)"
