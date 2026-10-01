#!/bin/bash
# Self-test for tools/r16_qualify.sh. No chip, no make target, no build: the script runs in
# dry mode (R16_QUALIFY_DRY=1) inside a scratch git repo, reading fixture logs.
# Proves: a dirty tree is refused before anything runs; a gate with no check prints
# NOT_RUN (never PASS); a failing check makes the script exit non-zero; observed fields
# are computed, not literals. Usage: bash tests/r16_qualify/run.sh
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

# 7. the script holds no literal for observed fields and does not touch tracked evidence
check "no literal tree_dirty/candidate_bound/silicon_observed values" \
    '! grep -E "\"(tree_dirty|candidate_bound|silicon_observed)\": (true|false)" "$HERE/tools/r16_qualify.sh"'
check "no copy into tracked evidence/" '! grep -E "cp .*evidence/R16|EVID_RAW_DIR" "$HERE/tools/r16_qualify.sh"'
check "no hardcoded home paths" '! grep -E "/home/[a-z]" "$HERE/tools/r16_qualify.sh"'

if [ "$FAILS" -ne 0 ]; then echo "R16 qualify self-test: $FAILS FAILED"; exit 1; fi
echo "R16 qualify self-test: PASS"
