#!/bin/sh
# Self-test of tools/mutation_runner.sh. Host only, CPU only, light. Builds a
# tiny C project in a temp dir, runs the runner over fake mutants and checks
# every verdict, the exit code and the JSON bytes. Each check can fail: the
# always-passing test run and the corrupted expected file prove the self-test
# notices a runner that reports the wrong thing.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
runner=$root/tools/mutation_runner.sh
expected=$root/tests/fixtures/mutation_runner/expected.json
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mutrun-selftest.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM
proj=$tmp/proj
mkdir -p "$proj"
cat > "$proj/add.c" <<'CEOF'
int add(int a, int b) { return a + b; } /* MUT:ADD_PLUS */
int clamp(int x) { return x>100?100:x; }
int twice(int x) { return x; }
CEOF
cat > "$proj/check.c" <<'CEOF'
#include <stdio.h>
int add(int a, int b);
int main(void) {
    if (add(2, 3) != 5) { puts("FAIL add"); return 1; }
    puts("PASS");
    return 0;
}
CEOF
BUILD='cc -o check check.c add.c'
TEST=./check
fails=0; checks=0
ok() { checks=$((checks + 1)); if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
has() { grep -q -- "$2" "$1"; }   # file pattern

# run <rowsfile-content> <extra args...>: sets rc, $tmp/err, $tmp/outp, $tmp/j.json
run() {
    rows=$1; shift
    printf '%s\n' "$rows" > "$tmp/rows"
    rm -f "$tmp/j.json"
    "$runner" -m "$tmp/rows" -d "$proj" -o "$tmp/j.json" "$@" > "$tmp/outp" 2> "$tmp/err"
    rc=$?
}
before=$(find "$proj" -type f | sort | xargs sha256sum | sha256sum)

# (1) a mutant that breaks the code is KILLED, exit 0
run 'K~add.c~s/a + b/a - b/~addition' -k table -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^K KILLED'; ok "1 breaking mutant is KILLED, exit 0" $?

# (2) a mutant the test cannot see SURVIVES, exit nonzero
run 'S~add.c~s/x>100/x>101/~clamp' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^S SURVIVED ' && ! has "$tmp/err" 'SURVIVED_'; ok "2 unseen mutant is SURVIVED, exit nonzero" $?

# (3) same mutant marked redundant (new column form and legacy prose form)
run 'S~add.c~s/x>100/x>101/~clamp~redundant~covered elsewhere' -k table -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^S SURVIVED_REDUNDANT' && has "$tmp/j.json" '"note": "redundant: covered elsewhere"'; ok "3a redundant (class column) is SURVIVED_REDUNDANT, exit 0" $?
run 'S~add.c~s/x>100/x>101/~clamp~redundant: covered elsewhere' -k table -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^S SURVIVED_REDUNDANT' && has "$tmp/j.json" '"note": "redundant: covered elsewhere"'; ok "3b redundant (legacy prose) is SURVIVED_REDUNDANT, exit 0" $?
run 'S~add.c~s/x>100/x>101/~clamp~gap~no test reaches it' -k table -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^S SURVIVED_GAP'; ok "3c gap is SURVIVED_GAP, exit 0" $?
run 'S~add.c~s/x>100/x>101/~clamp~none~' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^S SURVIVED '; ok "3d class none is a plain SURVIVED, exit nonzero" $?

# (4) a no-op edit is ERROR
run 'N~add.c~s/NO_SUCH_TEXT/y/~noop' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^N ERROR' && has "$tmp/j.json" '"note": "edit did not apply"'; ok "4a unmatched sed is ERROR edit did not apply" $?
run 'N~add.c~R:return a + b;@>@return a + b;~noop replace' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^N ERROR' && has "$tmp/j.json" '"note": "edit did not apply"'; ok "4b replace with identical text is ERROR" $?
run 'N~missing.c~s/a/b/~no file' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^N ERROR'; ok "4c missing file is ERROR" $?
run 'B~add.c~s/int add/int add_/~breaks the build' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^B ERROR' && has "$tmp/j.json" 'build failed'; ok "4d build failure is ERROR" $?

# (5) replace edit: exactly once works, twice is ERROR, @NL@ is a newline
run 'R1~add.c~R:return a + b;@>@return a - b;~replace once' -k table -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^R1 KILLED'; ok "5a replace matching once is applied (KILLED)" $?
run 'R2~add.c~R:return@>@return 0 +~replace twice' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^R2 ERROR' && has "$tmp/j.json" 'not found exactly once'; ok "5b replace matching twice is ERROR" $?
run 'R3~add.c~R:return a + b;@>@return a - b;@NL@int zzz(void) { return 1; }~newline escape' -k table -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^R3 KILLED'; ok "5c replace with @NL@ builds and is KILLED" $?

# (6) marker reader
run 'ADD_PLUS|add.c|s/a + b/a * b/' -k marker -t "$TEST" -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^ADD_PLUS KILLED'; ok "6a marker row is KILLED" $?
run 'NO_MARKER|add.c|s/a + b/a * b/' -k marker -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^NO_MARKER ERROR'; ok "6b marker not on any line is ERROR (edit does not apply)" $?
run 'ADD_PLUS|add.c|s/a + b/a * b/' -k marker -t "$TEST" -b "$BUILD" -B
[ $rc -eq 0 ]; ok "6c marker baseline passing is accepted" $?
run 'ADD_PLUS|add.c|s/a + b/a * b/' -k marker -t 'exit 1' -b "$BUILD" -B
[ $rc -eq 1 ] && has "$tmp/outp" 'baseline does not pass'; ok "6d failing baseline FAILS the sweep" $?

# (7) sed-table row with two files and @@ separated expressions
cp "$proj/add.c" "$proj/other.c"
run 'T~add.c,other.c~s/a + b/a - b/@@s/return x;/return x + 1;/~two files' -k table -t "$TEST" -b "$BUILD" -f FAIL
[ $rc -eq 0 ] && has "$tmp/err" '^T KILLED' && has "$tmp/j.json" '"file": "add.c,other.c"' && has "$tmp/j.json" 'first: add'; ok "7 two-file sed row is applied per file and KILLED" $?
rm -f "$proj/other.c"

# (8) JSON byte-compare against the committed expected receipt
cat > "$tmp/rows8" <<'REOF'
K1~add.c~s/a + b/a - b/~addition is computed
S1~add.c~s/x>100/x>101/~clamp upper bound
S2~add.c~s/x>100/x>101/~clamp upper bound again~redundant~covered by the caller, UNVERIFIED
L1~add.c~s/x>100/x>101/~legacy gap row~gap: legacy prose with "quotes" and \back
E1~add.c~s/NO_SUCH_TEXT/y/~no-op edit
REOF
"$runner" -k table -m "$tmp/rows8" -d "$proj" -o "$tmp/j8.json" -t "$TEST" -b "$BUILD" -f FAIL >/dev/null 2>"$tmp/err8"
rc8=$?
[ $rc8 -ne 0 ]; ok "8a sweep with a plain SURVIVED and an ERROR exits nonzero" $?
cmp -s "$tmp/j8.json" "$expected"; ok "8b JSON is byte-identical to tests/fixtures/mutation_runner/expected.json" $?
sed 's/KILLED/SURVIVED/' "$expected" > "$tmp/bad.json"
! cmp -s "$tmp/j8.json" "$tmp/bad.json"; ok "8c the byte compare notices a wrong receipt" $?
ONLY='K1 S2' "$runner" -k table -m "$tmp/rows8" -d "$proj" -o "$tmp/j9.json" -t "$TEST" -b "$BUILD" >/dev/null 2>&1
[ "$(grep -c '"id"' "$tmp/j9.json")" -eq 2 ]; ok "8d ONLY restricts the sweep" $?

# (9) deliberately wrong run: a test that always passes must make the breaking
# mutant SURVIVE, never KILLED, and the sweep must fail.
run 'K~add.c~s/a + b/a - b/~addition' -k table -t true -b "$BUILD"
[ $rc -ne 0 ] && has "$tmp/err" '^K SURVIVED' && ! has "$tmp/err" '^K KILLED'; ok "9a always-passing test reports SURVIVED, not KILLED" $?
run 'K~add.c~s/a + b/a - b/~addition' -k table -t false -b "$BUILD"
[ $rc -eq 0 ] && has "$tmp/err" '^K KILLED'; ok "9b always-failing test reports KILLED (the runner trusts the exit code)" $?
run '' -k table -t "$TEST" -b "$BUILD"
[ $rc -ne 0 ]; ok "9c an empty sweep fails instead of passing vacuously" $?

# (10) the source tree is never edited
after=$(find "$proj" -type f | sort | xargs sha256sum | sha256sum)
[ "$before" = "$after" ]; ok "10 source tree unchanged (no build artifacts, no edits)" $?

"$runner" >/dev/null 2>&1; [ $? -eq 2 ]; ok "11 missing arguments exit 2" $?

if [ $fails -ne 0 ]; then echo "mutation runner self-test: FAIL ($fails of $checks)"; exit 1; fi
echo "mutation runner self-test: PASS ($checks checks)"
