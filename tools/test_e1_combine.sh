#!/bin/bash
# test_e1_combine.sh -- host-only tests (no GPU) for tools/e1_combine.sh.
# Builds a scratch git repository with a "candidate" and a "main" commit, a
# full synthetic constituent set (Gate 5 receipt, reduce and transc receipts,
# MEAN-mutant log, campaign log, LDST chip log, host rerun, equivalence
# manifest), then expects accept for the good set and refuse for each hostile
# mutant: child digest, candidate SHA, Physics SHA, PASS->FAIL, input count,
# mismatch count, equivalence digest, chip file differing, missing child,
# duplicate child, uncertain completion, mutant not killed, host line failing,
# rebuilt binary not the chip binary, prior transc kernel digest changed. Run:
# make test-e1-combine.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
COMBINE=$HERE/tools/e1_combine.sh
# shellcheck source=tools/e1_combine.sh
. "$COMBINE"
e1_build_canon || { echo "FAIL: cannot build tools/json_canon.c"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP" "$E1_TMP"' EXIT
fails=0 passes=0
ok() { echo "  [PASS] $1"; passes=$((passes + 1)); }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# ---- scratch repository: candidate -> main, one host file (Makefile) differs ----
R=$TMP/repo; mkdir -p "$R/src/numeric" "$R/tests"
git -C "$R" init -q; git -C "$R" config user.email t@t; git -C "$R" config user.name t
echo 'kernel' > "$R/src/numeric/k.c"; echo 'gate' > "$R/tests/run_numeric_gates.sh"; echo 'all:' > "$R/Makefile"
git -C "$R" add -A; git -C "$R" commit -qm cand; CAND=$(git -C "$R" rev-parse HEAD)
echo 'all: more' > "$R/Makefile"; git -C "$R" commit -qam main; MAIN=$(git -C "$R" rev-parse HEAD)
export E1_GIT_DIR=$R
PHYS=e95e3ed2a86fe4bffe4d954fa94c27dfb5284280
OTHER=1111111111111111111111111111111111111111
sha() { sha256sum -- "$1" | cut -c1-64; }
gsha() { git -C "$R" show "$1:$2" | sha256sum | cut -c1-64; }
H() { printf '%064d' "$1"; }   # fake 64-hex digests
G5BIN=$(H 5) REDBIN=$(H 6) TRBIN=$(H 7) LDBIN=$(H 8) DESC=$(H 9) LOGH=$(H 10)

# content-named file: write BODY to TMP/DIR/<sha>.EXT, print path
named() { local d=$TMP/$1 f; mkdir -p "$d"; f=$(mktemp "$d/x.XXXX"); printf '%s' "$3" > "$f"; mv "$f" "$d/$(sha "$f").$2"; echo "$d/$(sha "$d/$(sha "$f").$2" 2>/dev/null || true)" > /dev/null; ls "$d"/*."$2" | tail -n1; }
named() { local d=$TMP/$1 f s; mkdir -p "$d"; f=$(mktemp "$d/x.XXXX"); printf '%s' "$3" > "$f"; s=$(sha "$f"); mv "$f" "$d/$s.$2"; echo "$d/$s.$2"; }
# json receipts: pretty and content-named
njson() { named "$1" json "$(printf '%s' "$2" | jq -c "${3:-.}" | "$JSON_CANON" --pretty)"; }

# Gate 5: receipt_digest over the body without it; named by that digest
G5_BODY="{\"schema\":\"AIEN_OMEGA_NUMERIC_0_V1\",\"status\":\"PASS\",\"candidate_git_commit\":\"$CAND\",\"run_git_commit\":\"$CAND\",\"physics_candidate_git_commit\":\"$PHYS\",\"physics_lock\":\"$PHYS\",\"candidate_trees_clean\":{\"omega\":true,\"physics\":true},\"gate_binary_exit_status\":0,\"observed_fail_count\":0,\"observed_test_count\":2,\"observed_pass_count\":2,\"test_results\":[{\"suite\":\"s\",\"id\":\"A_PASS\",\"status\":\"PASS\"},{\"suite\":\"s\",\"id\":\"B_PASS\",\"status\":\"PASS\"}],\"hardware_descriptor_digest\":\"$DESC\",\"candidate_binary_sha256\":\"$G5BIN\"}"
mkg5() { # [JQ_FILTER] [RENAME_DIGEST]
    local b d dir; b=$(printf '%s' "$G5_BODY" | jq -c "${1:-.}") && d=$(printf '%s' "$b" | "$JSON_CANON" --sha256) || return 1
    dir=$(mktemp -d "$TMP/g5.XXXX"); printf '{"receipt_digest":"%s",%s' "${2:-$d}" "${b#\{}" | "$JSON_CANON" --pretty > "$dir/${2:-$d}.json"; echo "$dir/${2:-$d}.json"
}
G5=$(mkg5); G5D=$(basename "$G5" .json)

RED_BODY="{\"suite\":\"E1_REDUCE_GB10_PARITY\",\"status\":\"PASS\",\"exit_status\":\"0\",\"omega_commit\":\"$CAND\",\"physics_commit\":\"$PHYS\",\"omega_clean_before\":true,\"omega_clean_after\":true,\"ops\":[\"SUM\",\"MAX\",\"MIN\",\"MEAN\"],\"parity_line\":\"RED_GB10_PARITY: PASS ops=SUM,MAX,MIN,MEAN cases=380 mismatches=0\",\"parity_by_op\":{\"SUM\":\"RED_GB10_PARITY_SUM: PASS op=SUM cases=95 mismatches=0\",\"MAX\":\"RED_GB10_PARITY_MAX: PASS op=MAX cases=95 mismatches=0\",\"MIN\":\"RED_GB10_PARITY_MIN: PASS op=MIN cases=95 mismatches=0\",\"MEAN\":\"RED_GB10_PARITY_MEAN: PASS op=MEAN cases=95 mismatches=0\"},\"binary_sha256\":\"$REDBIN\",\"log_sha256\":\"$LOGH\"}"
RED=$(njson red "$RED_BODY"); REDD=$(basename "$RED" .json)

ops_json() { local o j=; for o in SIN COS ERF GELU RSQRT; do j+="{\"op\":\"$o\",\"checked\":${1:-4294967296},\"exhaustive\":true,\"mismatches\":${2:-0},\"unwritten\":0,\"verdict\":\"PASS\"},"; done; echo "[${j%,}]"; }
KERN4() { local o j= i=1; for o in EXP2 LOG2 SIGMOID TANH; do j+="{\"op\":\"$o\",\"instructions\":$((i*8)),\"sha256\":\"${1:-$(H $((20+i)))}\"},"; i=$((i+1)); done; echo "${j%,}"; }
TR_BODY="{\"gate\":\"E1-TRANSC-GB10\",\"verdict\":\"PASS\",\"chip_exit_status\":0,\"omega_commit\":\"$CAND\",\"physics_commit\":\"$PHYS\",\"physics_lock_pin\":\"$PHYS\",\"omega_tree_clean_before\":true,\"omega_tree_clean_after\":true,\"omega_commit_unchanged_after\":true,\"physics_tree_clean_before\":true,\"physics_tree_clean_after\":true,\"physics_commit_unchanged_after\":true,\"ops_requested\":[\"SIN\",\"COS\",\"ERF\",\"GELU\",\"RSQRT\"],\"ops\":$(ops_json),\"kernels\":[$(KERN4),{\"op\":\"SIN\",\"instructions\":120,\"sha256\":\"$(H 31)\"}],\"binary_sha256\":\"$TRBIN\",\"chip_log_sha256\":\"$LOGH\"}"
TR=$(njson tr "$TR_BODY"); TRD=$(basename "$TR" .json)
ops4_json() { local o j=; for o in EXP2 LOG2 SIGMOID TANH; do j+="{\"op\":\"$o\",\"checked\":4294967296,\"exhaustive\":true,\"mismatches\":${1:-0},\"unwritten\":0,\"verdict\":\"PASS\"},"; done; echo "[${j%,}]"; }
TRP_BODY="{\"gate\":\"E1-TRANSC-GB10\",\"verdict\":\"PASS\",\"chip_exit_status\":0,\"omega_commit\":\"$(H 1 | cut -c1-40)\",\"physics_commit\":\"$PHYS\",\"physics_lock_pin\":\"$PHYS\",\"omega_tree_clean_before\":true,\"omega_tree_clean_after\":true,\"ops_requested\":[\"EXP2\",\"LOG2\",\"SIGMOID\",\"TANH\"],\"ops\":$(ops4_json),\"kernels\":[$(KERN4)],\"binary_sha256\":\"$(H 32)\"}"
TRP=$(njson trp "$TRP_BODY")

MUT_TXT=$'RED_GB10_PARITY_SUM: PASS op=SUM cases=95 mismatches=0\nRED_GB10_PARITY_MAX: PASS op=MAX cases=95 mismatches=0\nRED_GB10_PARITY_MIN: PASS op=MIN cases=95 mismatches=0\nRED_GB10_PARITY_MEAN: FAIL op=MEAN cases=95 mismatches=95\nE1 Reduce Verdict: FAIL\n'
MUT=$(named mut log "$MUT_TXT"); MUTD=$(basename "$MUT" .log)

camp() { # [sed expression]
    local f; f=$(mktemp "$TMP/campaign.XXXX.log")
    printf 'omega %s physics %s\nGate LDST host: PASSED=148 FAILED=0\n[PASS] ldst: 148 of 148 kernels decode\nspecs passed: 3\nGate 5 PASS: receipt digest %s\nreceipt: /x/%s.json status=PASS\nRECEIPT /x/%s.json\nlog sha256: %s\nE1 MEAN MUTANT: KILLED\nVERDICT PASS\n' \
        "$CAND" "$PHYS" "$G5D" "$REDD" "$TRD" "$MUTD" | sed "${1:-}" > "$f"; echo "$f"
}
CAMP=$(camp)
LDST_TXT=$'RESULT chip spec=1 a count=1000 rc=0 mismatches=0 unwritten=0 verdict=PASS\nRESULT chip spec=2 b count=1000 rc=0 mismatches=0 unwritten=0 verdict=PASS\nRESULT chip spec=3 c count=1000 rc=0 mismatches=0 unwritten=0 verdict=PASS\nVERDICT PASS\n'
printf '%s' "$LDST_TXT" > "$TMP/ldst.log"; LDST=$TMP/ldst.log

HOST_BODY="{\"schema\":\"AIEN_E1_HOST_RERUN_V1\",\"omega_commit\":\"$MAIN\",\"tracked_tree_clean_before\":true,\"tracked_tree_clean_after\":true,\"lines\":{\"GB10-COMPILE\":\"PASS\",\"NUMERIC-HOST\":\"PASS\",\"PROGRAM\":\"PASS\",\"TRANSC\":\"PASS\",\"MANIFEST\":\"PASS\",\"HOSTALL\":\"PASS\"}}"
HOST=$(njson host "$HOST_BODY")

mf() { local p=$1 c=$2 a b; a=$(gsha "$CAND" "$p"); b=$(gsha "$MAIN" "$p"); printf '{"path":"%s","class":"%s","sha256_candidate":"%s","sha256_main":"%s","equal":%s}' "$p" "$c" "$a" "$b" "$([ "$a" = "$b" ] && echo true || echo false)"; }
MAN_BODY="{\"schema\":\"AIEN_E1_EQUIVALENCE_MANIFEST_V1\",\"candidate_git_commit\":\"$CAND\",\"main_git_commit\":\"$MAIN\",\"whole_tree_identical\":false,\"files_equal\":2,\"files_differ\":1,\"chip_files_differ\":0,\"files\":[$(mf src/numeric/k.c chip),$(mf tests/run_numeric_gates.sh chip),$(mf Makefile host)],\"rebuilt_chip_binaries\":[{\"gate\":\"GATE5\",\"sha256_main\":\"$G5BIN\"},{\"gate\":\"REDUCE\",\"sha256_main\":\"$REDBIN\"},{\"gate\":\"TRANSC\",\"sha256_main\":\"$TRBIN\"},{\"gate\":\"LDST\",\"sha256_main\":\"$LDBIN\"}]}"
MAN=$(njson man "$MAN_BODY")

ARGS=(--candidate "$CAND" --main "$MAIN" --physics "$PHYS")
good() { echo --manifest "${MANX:-$MAN}" --gate5 "${G5X:-$G5}" --reduce "${REDX:-$RED}" --transc "${TRX:-$TR}" --mutant-log "${MUTX:-$MUT}" --campaign-log "${CAMPX:-$CAMP}" --ldst-chip-log "${LDSTX:-$LDST}" --host-rerun "${HOSTX:-$HOST}" --transc-prior "${TRPX:-$TRP}"; }
run() { "$COMBINE" "$@" > "$TMP/out" 2> "$TMP/err"; }
refused() { ! run "$@" && grep -q "^REFUSED: .*$REASON" "$TMP/err" || { echo "    err: $(head -c 300 "$TMP/err") out: $(head -c 100 "$TMP/out")"; false; }; }
# with VAR=path overridden for one call
with() { local v=$1 p=$2; eval "$v=\$p"; refused "${ARGS[@]}" $(good); local rc=$?; unset "$v"; return $rc; }

echo "== accept =="
EV=$TMP/ev; export E1_TIMESTAMP_UTC=2026-10-02T00:00:00.000000Z
check "good constituent set accepted and written" 'run "${ARGS[@]}" $(good) --evidence-dir "$EV" && grep -q "^E1 CLOSURE PASS" "$TMP/out"'
OUT=$(ls "$EV"/*.json 2>/dev/null | head -n1)
check "written receipt is named for its receipt_digest" '[ -n "$OUT" ] && [ "$(jq -r .receipt_digest "$OUT")" = "$(basename "$OUT" .json)" ]'
check "receipt_digest = sha256(canonical body without it)" '[ "$(jq -c "del(.receipt_digest)" "$OUT" | "$JSON_CANON" --sha256)" = "$(basename "$OUT" .json)" ]'
check "receipt is read-only and says the chip ran on the candidate" '[ ! -w "$OUT" ] && [ "$(jq -r .chip_ran_on "$OUT")" = candidate ] && [ "$(jq -r .merged_main_git_commit "$OUT")" = "$MAIN" ]'
check "receipt records one differing host file and host rerun PASS" '[ "$(jq -r .e1_artifact_set.host_class_files_differ "$OUT")" = 1 ] && [ "$(jq -r .constituents.host_rerun.status "$OUT")" = PASS ]'
check "second write of the same receipt is refused (exclusive)" 'REASON="cannot write" refused "${ARGS[@]}" $(good) --evidence-dir "$EV"'
check "deterministic digest apart from timestamp" 'run "${ARGS[@]}" $(good) && [ "$(jq -c "del(.receipt_digest,.timestamp_utc)" "$OUT")" = "$(jq -c "del(.receipt_digest,.timestamp_utc)" "$OUT")" ] && grep -q "receipt digest" "$TMP/out"'

echo "== refuse: child digest =="
X=$(mkg5 . "$(H 42)"); check "gate5 receipt_digest not matching content" 'REASON="receipt_digest does not match" with G5X "$X"'
cp "$RED" "$TMP/red-tampered.json"; sed -i 's/cases=380/cases=380 /' "$TMP/red-tampered.json"; mv "$TMP/red-tampered.json" "$TMP/$REDD.tampered"; mkdir -p "$TMP/rt"; cp "$TMP/$REDD.tampered" "$TMP/rt/$REDD.json"
check "reduce receipt whose content no longer matches its name" 'REASON="not named for its SHA-256" with REDX "$TMP/rt/$REDD.json"'
mkdir -p "$TMP/tt"; cp "$TR" "$TMP/tt/$(H 43).json"
check "transc receipt renamed to a different digest" 'REASON="not named for its SHA-256" with TRX "$TMP/tt/$(H 43).json"'
mkdir -p "$TMP/mt"; printf '%s' "$MUT_TXT" > "$TMP/mt/$(H 44).log"
check "mutant log renamed" 'REASON="not named for its SHA-256" with MUTX "$TMP/mt/$(H 44).log"'
X=$(njson man "$MAN_BODY" ".files[0].sha256_main=\"$(H 45)\"")
check "equivalence manifest digest not matching git" 'REASON="do not match git" with MANX "$X"'

echo "== refuse: SHAs =="
check "candidate SHA on the command line differs from the receipts" 'REASON="candidate" refused --candidate "$OTHER" --main "$MAIN" --physics "$PHYS" $(good)'
X=$(mkg5 ".candidate_git_commit=\"$OTHER\""); check "gate5 names another candidate" 'REASON="gate5" with G5X "$X"'
X=$(njson red "$RED_BODY" ".physics_commit=\"$OTHER\""); check "reduce names another Physics commit" 'REASON="physics" with REDX "$X"'
X=$(njson tr "$TR_BODY" ".physics_lock_pin=\"$OTHER\""); check "transc pin differs from Physics" 'REASON="transc" with TRX "$X"'
check "Physics SHA on the command line differs" 'REASON="physics" refused --candidate "$CAND" --main "$MAIN" --physics "$OTHER" $(good)'
X=$(njson host "$HOST_BODY" ".omega_commit=\"$CAND\""); check "host rerun on the candidate, not main" 'REASON="host rerun" with HOSTX "$X"'
X=$(njson man "$MAN_BODY" ".main_git_commit=\"$CAND\""); check "manifest for other commits" 'REASON="manifest" with MANX "$X"'

echo "== refuse: PASS->FAIL =="
X=$(mkg5 '.status="FAIL"'); check "gate5 FAIL" 'REASON="gate5" with G5X "$X"'
X=$(mkg5 '.test_results[1].status="FAIL"'); check "gate5 one failing test" 'REASON="gate5" with G5X "$X"'
X=$(njson red "$RED_BODY" '.status="FAIL"'); check "reduce FAIL" 'REASON="reduce" with REDX "$X"'
X=$(njson tr "$TR_BODY" '.ops[2].verdict="FAIL"'); check "transc one op FAIL" 'REASON="transc" with TRX "$X"'
X=$(njson host "$HOST_BODY" '.lines.TRANSC="FAIL(rc=2)"'); check "host rerun line FAIL" 'REASON="TRANSC is not PASS" with HOSTX "$X"'
X=$(njson host "$HOST_BODY" 'del(.lines.HOSTALL)'); check "host rerun line missing" 'REASON="HOSTALL is not PASS" with HOSTX "$X"'
X=$(camp 's/^VERDICT PASS/VERDICT FAIL/'); check "campaign VERDICT FAIL" 'REASON="VERDICT PASS" with CAMPX "$X"'
printf '%s' "${LDST_TXT/verdict=PASS/verdict=FAIL}" > "$TMP/ldst-f.log"; check "ldst one spec FAIL" 'REASON="ldst" with LDSTX "$TMP/ldst-f.log"'

echo "== refuse: counts =="
X=$(njson tr "$TR_BODY" ".ops=$(ops_json 4294967295)"); check "transc input count short of 2^32" 'REASON="transc" with TRX "$X"'
X=$(njson tr "$TR_BODY" '.ops |= .[0:4]'); check "transc only four ops" 'REASON="transc" with TRX "$X"'
X=$(njson red "$RED_BODY" '.parity_by_op.MIN="RED_GB10_PARITY_MIN: PASS op=MIN cases=94 mismatches=0"'); check "reduce per-op case count sums short" 'REASON="reduce" with REDX "$X"'
X=$(mkg5 '.observed_test_count=3'); check "gate5 test count disagrees with results" 'REASON="gate5" with G5X "$X"'
X=$(camp 's/^specs passed: 3/specs passed: 4/'); check "campaign spec count disagrees with ldst log" 'REASON="PASS results" with CAMPX "$X"'
X=$(njson man "$MAN_BODY" '.files_differ=0'); check "manifest differ count wrong" 'REASON="files_differ" with MANX "$X"'

echo "== refuse: mismatches =="
X=$(njson red "$RED_BODY" '.parity_by_op.MEAN="RED_GB10_PARITY_MEAN: PASS op=MEAN cases=95 mismatches=1"'); check "reduce one mismatch in one op" 'REASON="reduce" with REDX "$X"'
X=$(njson tr "$TR_BODY" ".ops=$(ops_json 4294967296 1)"); check "transc one mismatch" 'REASON="transc" with TRX "$X"'
X=$(mkg5 '.observed_fail_count=1'); check "gate5 nonzero fail count" 'REASON="gate5" with G5X "$X"'
printf '%s' "${LDST_TXT/mismatches=0 unwritten=0 verdict=PASS/mismatches=0 unwritten=0 verdict=PASS
OMEGA_DEVERR x}" > "$TMP/ldst-e.log"; check "ldst device error" 'REASON="device error" with LDSTX "$TMP/ldst-e.log"'

echo "== refuse: equivalence =="
echo 'changed' > "$R/src/numeric/k.c"; git -C "$R" commit -qam chip; MAIN2=$(git -C "$R" rev-parse HEAD)
mf2() { local p=$1 c=$2 a b; a=$(gsha "$CAND" "$p"); b=$(gsha "$MAIN2" "$p"); printf '{"path":"%s","class":"%s","sha256_candidate":"%s","sha256_main":"%s","equal":%s}' "$p" "$c" "$a" "$b" "$([ "$a" = "$b" ] && echo true || echo false)"; }
MAN2="{\"schema\":\"AIEN_E1_EQUIVALENCE_MANIFEST_V1\",\"candidate_git_commit\":\"$CAND\",\"main_git_commit\":\"$MAIN2\",\"whole_tree_identical\":false,\"files_equal\":1,\"files_differ\":2,\"chip_files_differ\":0,\"files\":[$(mf2 src/numeric/k.c chip),$(mf2 tests/run_numeric_gates.sh chip),$(mf2 Makefile host)],\"rebuilt_chip_binaries\":[{\"gate\":\"GATE5\",\"sha256_main\":\"$G5BIN\"},{\"gate\":\"REDUCE\",\"sha256_main\":\"$REDBIN\"},{\"gate\":\"TRANSC\",\"sha256_main\":\"$TRBIN\"},{\"gate\":\"LDST\",\"sha256_main\":\"$LDBIN\"}]}"
X=$(njson man "$MAN2"); HOST2=$(njson host "$HOST_BODY" ".omega_commit=\"$MAIN2\"")
check "a chip-class file differs between candidate and main (needs requalification)" 'REASON="chip artifact" refused --candidate "$CAND" --main "$MAIN2" --physics "$PHYS" --manifest "$X" --gate5 "$G5" --reduce "$RED" --transc "$TR" --mutant-log "$MUT" --campaign-log "$CAMP" --ldst-chip-log "$LDST" --host-rerun "$HOST2" --transc-prior "$TRP"'
X=$(njson man "$MAN_BODY" ".rebuilt_chip_binaries[1].sha256_main=\"$(H 46)\""); check "rebuilt REDUCE binary is not the chip binary" 'REASON="rebuilt REDUCE" with MANX "$X"'
X=$(njson man "$MAN_BODY" '.rebuilt_chip_binaries |= .[0:3]'); check "no rebuilt LDST binary" 'REASON="LDST" with MANX "$X"'
X=$(njson man "$MAN_BODY" '.files[2].equal=true'); check "manifest equal flag lies" 'REASON="equal flag" with MANX "$X"'
X=$(njson man "$MAN_BODY" '.files[0].class="host" | .files[1].class="host"'); check "chip file relabelled host (digests still match, still accepted only if equal)" 'run "${ARGS[@]}" --manifest "$X" --gate5 "$G5" --reduce "$RED" --transc "$TR" --mutant-log "$MUT" --campaign-log "$CAMP" --ldst-chip-log "$LDST" --host-rerun "$HOST" --transc-prior "$TRP"'

echo "== refuse: campaign / mutant / children =="
X=$(camp 's/^E1 MEAN MUTANT: KILLED/E1 MEAN MUTANT: SURVIVED/'); check "campaign mutant not killed" 'REASON="KILLED" with CAMPX "$X"'
X=$(camp '$a GB10_COMPLETION_UNCERTAIN'); check "campaign has an uncertain completion" 'REASON="uncertain" with CAMPX "$X"'
X=$(camp "s/$G5D/$(H 47)/"); check "campaign names a different Gate 5 receipt" 'REASON="Gate 5" with CAMPX "$X"'
X=$(named mut log "${MUT_TXT/MEAN: FAIL op=MEAN cases=95 mismatches=95/MEAN: PASS op=MEAN cases=95 mismatches=0}"); check "mutant log where MEAN passed (mutant survived)" 'REASON="MEAN failing" with MUTX "$X"'
check "missing child (no host rerun)" 'REASON="not given" refused "${ARGS[@]}" --manifest "$MAN" --gate5 "$G5" --reduce "$RED" --transc "$TR" --mutant-log "$MUT" --campaign-log "$CAMP" --ldst-chip-log "$LDST" --transc-prior "$TRP"'
check "missing child (no gate5)" 'REASON="not given" refused "${ARGS[@]}" --manifest "$MAN" --reduce "$RED" --transc "$TR" --mutant-log "$MUT" --campaign-log "$CAMP" --ldst-chip-log "$LDST" --host-rerun "$HOST" --transc-prior "$TRP"'
check "missing child (no prior transc receipt)" 'REASON="not given" refused "${ARGS[@]}" --manifest "$MAN" --gate5 "$G5" --reduce "$RED" --transc "$TR" --mutant-log "$MUT" --campaign-log "$CAMP" --ldst-chip-log "$LDST" --host-rerun "$HOST"'
X=$(njson trp "$TRP_BODY" ".kernels[1].sha256=\"$(H 50)\""); check "prior transc LOG2 kernel digest differs from the candidate kernel" 'REASON="LOG2 kernel digest" with TRPX "$X"'
X=$(njson trp "$TRP_BODY" ".ops=$(ops4_json 1)"); check "prior transc with one mismatch" 'REASON="transc prior" with TRPX "$X"'
X=$(njson trp "$TRP_BODY" ".physics_commit=\"$OTHER\""); check "prior transc on another Physics commit" 'REASON="transc prior" with TRPX "$X"'
X=$(njson tr "$TR_BODY" ".kernels[0].sha256=\"$(H 51)\""); check "candidate transc EXP2 kernel differs from the chip-run prior" 'REASON="EXP2 kernel digest" with TRX "$X"'
check "duplicate child (gate5 twice)" 'REASON="duplicate" refused "${ARGS[@]}" $(good) --gate5 "$G5"'
check "nonexistent child path" 'REASON="cannot read" with TRX "$TMP/nope.json"'
mkdir -p "$TMP/nj"; echo 'not json' > "$TMP/nj/x.json"; X=$TMP/nj/$(sha "$TMP/nj/x.json").json; mv "$TMP/nj/x.json" "$X"
check "child is not JSON" 'REASON="not valid JSON" with REDX "$X"'
check "nothing written on any refusal" '[ "$(ls "$EV" | wc -l)" = 1 ]'
check "short SHA is a usage error" '"$COMBINE" --candidate abc --main "$MAIN" --physics "$PHYS" $(good) > /dev/null 2>&1; [ $? = 2 ]'

echo "e1_combine tests: $passes passed, $fails failed"
[ "$fails" = 0 ]
