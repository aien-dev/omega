#!/bin/bash
# Self-test for tools/r16_qualify.sh. No chip, no make target, no build: the script runs in
# dry mode (R16_QUALIFY_DRY=1) inside a scratch git repo, reading fixture logs.
# Proves: a dirty tree is refused before anything runs; a gate with no check prints
# NOT_RUN (never PASS); a failing check makes the script exit non-zero; observed fields
# are computed, not literals; G1/G2 follow every count, repo head and result in the
# inventory JSON; the G7 R15 receipt check fails on each broken field. Each check has a
# passing fixture first and then a counterexample that must fail.
# Usage: bash tests/r16_qualify/run.sh
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
SCR=$(mktemp -d "${TMPDIR:-/tmp}/r16qual.XXXXXX") || exit 2
trap 'rm -rf "$SCR"' EXIT INT TERM
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }

W=$SCR/w
mkdir -p "$W/tools"
cp "$HERE/tools/r16_qualify.sh" "$W/tools/"
printf 'build/\n' > "$W/.gitignore"
g() { git -C "$W" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false "$@"; }
g init -q && g add -A && g commit -qm fixture || exit 2
Q() { env R16_QUALIFY_DRY=1 "$@" bash "$W/tools/r16_qualify.sh"; }

# 1. dirty tree refused before anything runs
echo x > "$W/stray.txt"
out=$(Q 2>&1); rc=$?
check "dirty tree: exit 2" '[ $rc = 2 ]'
check "dirty tree: says REFUSED" 'echo "$out" | grep -q "^REFUSED: working tree is dirty"'
check "dirty tree: nothing created (no build dir)" '[ ! -e "$W/build" ]'
check "dirty tree: no gate lines printed" '! echo "$out" | grep -q "R16-G"'
rm -f "$W/stray.txt"
echo y >> "$W/.gitignore"
out=$(Q 2>&1); rc=$?
check "modified tracked file: exit 2" '[ $rc = 2 ]'
g checkout -q -- .gitignore

# 2. wrong expected commit refused
out=$(Q R16_EXPECT_COMMIT=0000000000000000000000000000000000000000 2>&1); rc=$?
check "unexpected HEAD: exit 2" '[ $rc = 2 ]'

# 3. output under tracked evidence/ refused
out=$(Q R16_OUT_DIR="$W/evidence/R16/raw" 2>&1); rc=$?
check "output under evidence/: exit 2" '[ $rc = 2 ]'

# 4. no fixtures: every gate NOT_RUN, nothing PASS, exit 3
out=$(Q R16_RUN_ID=none 2>&1); rc=$?
R=$W/build/r16-raw/none/DRY-RUN-receipt.json
check "no fixtures: exit 3 (nothing ran)" '[ $rc = 3 ]'
check "no fixtures: G6 prints NOT_RUN" 'echo "$out" | grep -q "R16-G6: NOT_RUN"'
check "no fixtures: summary all NOT_RUN" 'echo "$out" | grep -q "^Gates: G1=NOT_RUN G2=NOT_RUN G3=NOT_RUN G4=NOT_RUN G5=NOT_RUN G6=NOT_RUN G7=NOT_RUN G8=NOT_RUN$"'
check "no fixtures: receipt gate NOT_RUN" 'grep -q "\"gate\": \"NOT_RUN\"" "$R"'
check "no fixtures: receipt has no PASS value" '! grep -q "\"PASS\"" "$R"'
check "no fixtures: silicon_observed false (computed)" 'grep -q "\"silicon_observed\": false" "$R"'
check "no fixtures: tree_dirty false and candidate_bound true observed" 'grep -q "\"tree_dirty\": false" "$R" && grep -q "\"candidate_bound\": true" "$R"'
check "receipt marks schema revision 2" 'grep -q "\"schema_revision\": 2" "$R"'
check "receipt keeps schema name" 'grep -q "\"schema\": \"AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1\"" "$R"'
check "tree still clean after run" '[ -z "$(g status --porcelain)" ]'

# 5. fixture logs: G4 good, G5 log lacks the required line -> FAIL -> exit 1
F=$W/build/r16-raw/fx
mkdir -p "$F"
echo "R16 gate: R16_G4_LEGACY_REFUSED=PASS" > "$F/r16_negative.log"; echo 0 > "$F/r16_negative.log.rc"
echo "R16 gate: R16_G4_GUARDS_LOAD_BEARING=PASS" > "$F/r16_negative_mutants.log"; echo 0 > "$F/r16_negative_mutants.log.rc"
echo "surface: something else" > "$F/r16_surface.log"; echo 0 > "$F/r16_surface.log.rc"
out=$(Q R16_RUN_ID=fx 2>&1); rc=$?
R=$W/build/r16-raw/fx/DRY-RUN-receipt.json
check "failing grep: exit non-zero (1)" '[ $rc = 1 ]'
check "failing grep: G5 FAIL" 'echo "$out" | grep -q "G5=FAIL"'
check "failing grep: G4 PASS from its own checks" 'echo "$out" | grep -q "G4=PASS"'
check "failing grep: receipt gate FAIL" 'grep -q "\"gate\": \"FAIL\"" "$R"'

# 6. target exit status counts even when the pass line is present
echo 1 > "$F/r16_surface.log.rc"; echo "R16 gate: R16_G5_SURFACE=PASS" > "$F/r16_surface.log"
out=$(Q R16_RUN_ID=fx 2>&1); rc=$?
check "bad exit status with pass line: G5 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G5=FAIL"'
echo 0 > "$F/r16_surface.log.rc"
out=$(Q R16_RUN_ID=fx 2>&1); rc=$?
check "good G4/G5 fixtures: those gates PASS" 'echo "$out" | grep -q "G4=PASS" && echo "$out" | grep -q "G5=PASS"'
check "all-good G4/G5 still exit 3 (G6 has no check)" '[ $rc = 3 ] && echo "$out" | grep -q "G6=NOT_RUN"'

# 6b. R11 that skipped its living run (load > 2) exits 0 with "failures 0": NOT_RUN, never PASS
printf 'checks 400 failures 0\n' > "$F/r11_aien.log"; echo 0 > "$F/r11_aien.log.rc"
out=$(Q R16_RUN_ID=fx 2>&1)
check "R11 rules-only log with living run: PASS" 'echo "$out" | grep -q "^    R11: PASS$"'
printf '[-] SKIPPED-LOADED: living run not started: load average 15.85 is above 2\n[-] living run not exercised: SKIPPED-LOADED: load average 15.85 is above 2\nchecks 400 failures 0\n' > "$F/r11_aien.log"
out=$(Q R16_RUN_ID=fx 2>&1)
check "R11 living run skipped under load: NOT_RUN" 'echo "$out" | grep -q "^    R11: NOT_RUN$"'
rm -f "$F/r11_aien.log" "$F/r11_aien.log.rc"


# 7. G1/G2 from the inventory JSON. A good fixture passes first (so each failure below is
# caused by the one value changed), then every G1/G2 check is broken on its own.
I=$W/build/r16-raw/inv
mkdir -p "$I"
# inv_fixture [key=value ...]: writes inventory.json in the tool's layout with all counts 0,
# result PASS and five recorded heads; a key=value pair overrides one field
# (head_<repo>=<sha> overrides one repo head; an empty value records no head).
inv_fixture() {
    local unclassified=0 question_rows=0 bad_class=0 a_reachable=0 stale_rows=0 map_errors=0
    local skipped_repos=0 result=PASS kv k v r
    local head_omega=1111111111111111111111111111111111111111
    local head_aien_sovereign_core=2222222222222222222222222222222222222222
    local head_aegis_runtime=3333333333333333333333333333333333333333
    local head_aienos=4444444444444444444444444444444444444444
    local head_physics=5555555555555555555555555555555555555555
    for kv in "$@"; do k=${kv%%=*}; v=${kv#*=}; k=${k//-/_}; printf -v "$k" '%s' "$v"; done
    {
        printf '{\n  "schema": "AIEN_R16_LOOP_INVENTORY_V1",\n  "map": "spec/r16-orchestrator-retirement-map.md",\n  "repos": [\n'
        for r in omega aien-sovereign-core aegis-runtime aienos physics; do
            k=head_${r//-/_}
            printf '    {"name": "%s", "path": "/x/%s", "head": "%s", "map_sha": "", "status": "SCANNED", "sites_by_class": {"A": 0}}%s\n' \
                "$r" "$r" "${!k}" "$([ "$r" = physics ] || echo ,)"
        done
        printf '  ],\n  "sites": [\n  ],\n  "manual_rows": 5,\n  "sites_total": 328,\n'
        printf '  "unclassified": %s,\n  "question_rows": %s,\n  "bad_class": %s,\n  "a_reachable": %s,\n' \
            "$unclassified" "$question_rows" "$bad_class" "$a_reachable"
        printf '  "stale_rows": %s,\n  "map_errors": %s,\n  "skipped_repos": %s,\n  "class_n_sites": 0,\n  "result": "%s"\n}\n' \
            "$stale_rows" "$map_errors" "$skipped_repos" "$result"
    } > "$I/inventory.json"
}
inv_logs() {   # inv_logs <inventory rc> <self-test rc> <self-test last line>
    echo "R16 loop inventory: sites=328 -> fixture" > "$I/r16_inventory.log"; echo "$1" > "$I/r16_inventory.log.rc"
    echo "$3" > "$I/test_r16_inventory.log"; echo "$2" > "$I/test_r16_inventory.log.rc"
}
inv_logs 0 0 "R16 inventory self-test: PASS"
inv_fixture
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
R=$I/DRY-RUN-receipt.json
check "good inventory: G1 PASS and G2 PASS" 'echo "$out" | grep -q "G1=PASS G2=PASS"'
check "good inventory: no FAIL, exit 3 (other gates NOT_RUN)" '[ $rc = 3 ]'
check "good inventory: counts read from the JSON (0 and 0)" 'grep -q "\"remaining_unclassified_semantic_loop_count\": 0" "$R" && grep -q "\"remaining_central_loop_count\": 0" "$R"'
check "good inventory: repo heads read from the JSON" 'grep -q "\"aien_sovereign_core_commit\": \"2222222222222222222222222222222222222222\"" "$R" && grep -q "\"aegis_runtime_commit\": \"3333333333333333333333333333333333333333\"" "$R"'
for k in unclassified question_rows bad_class stale_rows map_errors skipped_repos; do
    inv_fixture "$k=1"
    out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
    check "bad inventory $k=1: G1 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=FAIL"'
done
inv_fixture unclassified=7
out=$(Q R16_RUN_ID=inv 2>&1)
check "bad inventory unclassified=7: count carried into receipt" 'grep -q "\"remaining_unclassified_semantic_loop_count\": 7" "$R"'
for r in omega aien-sovereign-core aegis-runtime aienos physics; do
    inv_fixture "head_$r="
    out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
    check "inventory with no head for $r: G1 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=FAIL"'
done
inv_fixture a_reachable=1
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "bad inventory a_reachable=1: G2 FAIL (G1 PASS), exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=PASS G2=FAIL"'
check "bad inventory a_reachable=1: count carried into receipt" 'grep -q "\"remaining_central_loop_count\": 1" "$R"'
inv_fixture result=FAIL
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory result FAIL: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_fixture; inv_logs 1 0 "R16 inventory self-test: PASS"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory target exit 1: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_logs 0 0 "R16 inventory self-test: 1 FAILED"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory self-test log lacks pass line: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_logs 0 1 "R16 inventory self-test: PASS"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory self-test exit 1 with pass line: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_logs 0 0 "R16 inventory self-test: PASS"; rm -f "$I/inventory.json"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory ran but wrote no JSON: G1 FAIL and G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=FAIL G2=FAIL"'

# 8. G7 R15 acceptance receipt (R16_R15_RECEIPT). A good receipt (named by its own
# SHA-256, bound to this HEAD) passes first; then each check is broken on its own.
CAND=$(g rev-parse HEAD)
RD=$SCR/r15
mkdir -p "$RD"
# r15_receipt [field=value ...]: writes a receipt named by its own SHA-256, prints its path.
r15_receipt() {
    local outcome=PASS candidate_commit=$CAND candidate_bound=true tree_dirty=false silicon_observed=true kv
    for kv in "$@"; do printf -v "${kv%%=*}" '%s' "${kv#*=}"; done
    printf '{\n  "schema": "AIEN_RX_R15_FIXTURE",\n  "outcome": "%s",\n  "candidate_commit": "%s",\n  "candidate_bound": %s,\n  "tree_dirty": %s,\n  "silicon_observed": %s\n}\n' \
        "$outcome" "$candidate_commit" "$candidate_bound" "$tree_dirty" "$silicon_observed" > "$RD/tmp.json"
    local s; s=$(sha256sum "$RD/tmp.json" | cut -d' ' -f1)
    mv "$RD/tmp.json" "$RD/$s.json"
    echo "$RD/$s.json"
}
GOOD=$(r15_receipt)
out=$(Q R16_RUN_ID=r15 R16_R15_RECEIPT="$GOOD" 2>&1); rc=$?
R=$W/build/r16-raw/r15/DRY-RUN-receipt.json
check "good R15 receipt: acceptance PASS" 'echo "$out" | grep -q "R15 acceptance receipt: PASS"'
check "good R15 receipt: field true, exit 3 (ladder NOT_RUN)" '[ $rc = 3 ] && grep -q "\"r15_acceptance_still_passing\": true" "$R"'
r15_bad() {   # r15_bad <label> <receipt path>
    out=$(Q R16_RUN_ID=r15 R16_R15_RECEIPT="$2" 2>&1); rc=$?
    check "R15 receipt $1: acceptance FAIL, G7 FAIL, exit 1" \
        '[ $rc = 1 ] && echo "$out" | grep -q "R15 acceptance receipt: FAIL" && echo "$out" | grep -q "G7=FAIL" && grep -q "\"r15_acceptance_still_passing\": false" "$R"'
}
cp "$GOOD" "$RD/not-its-sha.json"
r15_bad "not named by its SHA-256" "$RD/not-its-sha.json"
r15_bad "missing file" "$RD/absent.json"
: > "$RD/empty.json"
r15_bad "empty file" "$RD/empty.json"
r15_bad "for another commit" "$(r15_receipt candidate_commit=0000000000000000000000000000000000000000)"
r15_bad "outcome FAIL" "$(r15_receipt outcome=FAIL)"
r15_bad "candidate_bound false" "$(r15_receipt candidate_bound=false)"
r15_bad "tree_dirty true" "$(r15_receipt tree_dirty=true)"
r15_bad "silicon_observed false" "$(r15_receipt silicon_observed=false)"

# 9. the script holds no literal for observed fields and does not touch tracked evidence
check "no literal tree_dirty/candidate_bound/silicon_observed values" \
    '! grep -E "\"(tree_dirty|candidate_bound|silicon_observed)\": (true|false)" "$HERE/tools/r16_qualify.sh"'
check "no copy into tracked evidence/" '! grep -E "cp .*evidence/R16|EVID_RAW_DIR" "$HERE/tools/r16_qualify.sh"'
check "no hardcoded home paths" '! grep -E "/home/[a-z]" "$HERE/tools/r16_qualify.sh"'

if [ "$FAILS" -ne 0 ]; then echo "R16 qualify self-test: $FAILS FAILED"; exit 1; fi
echo "R16 qualify self-test: PASS"
