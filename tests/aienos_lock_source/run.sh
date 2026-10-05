#!/bin/bash
# Test for tools/aienos_lock_source.sh and the Makefile rule that builds the trusted
# capability root from it. Host only, no chip. Fixture git repositories stand in for aienos.
# Every counterexample must be refused with the right exit status: 1 = verified mismatch or
# absence, 2 = the locked commit cannot be read (never reported as absent, never as proven).
# Usage: bash tests/aienos_lock_source/run.sh
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
SCR=$(mktemp -d "${TMPDIR:-/tmp}/aienos-lock-src.XXXXXX") || exit 2
trap 'rm -rf "$SCR"' EXIT INT TERM
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }
T="$HERE/tools/aienos_lock_source.sh"

# Fixture aienos: commit GOOD has a buildable native/capability; commit A lacks aienos_cap_mint.
LK=$SCR/aienos
lkg() { git -C "$LK" -c user.name=t -c user.email=t@invalid -c commit.gpgsign=false "$@"; }
mkdir -p "$LK/native/capability" "$LK/native/store"
printf 'out/\n' > "$LK/native/capability/.gitignore"
printf 'int aienos_cap_validate(int v)\n{ return v; }\nint aienos_cap_mint(int a)\n{ return a; }\n' > "$LK/native/capability/aienos_capability.c"
printf 'all:\n\tmkdir -p out && $(CC) -c -o out/aienos_capability.o aienos_capability.c && ar rcs out/libaienos_capability.a out/aienos_capability.o\n' > "$LK/native/capability/Makefile"
printf 'int store_v1;\n' > "$LK/native/store/store_v1.c"
lkg init -q && lkg add -A && lkg commit -qm good
GOOD=$(git -C "$LK" rev-parse HEAD)
sed -i 's/aienos_cap_mint(/aienos_cap_other(/' "$LK/native/capability/aienos_capability.c"; lkg commit -qam "A: no mint"
A=$(git -C "$LK" rev-parse HEAD)
lkg checkout -q "$GOOD"
L() { env AIENOS_LOCK_REPO="$LK" AIENOS_R7_DIR= "$@"; }
rc() { "$@" >/dev/null 2>&1; echo $?; }

# 1. reading blobs of the locked commit
check "show: locked file read (exit 0)" '[ "$(L AIENOS_LOCK=$GOOD bash "$T" show native/capability/aienos_capability.c | grep -c aienos_cap_mint)" = 1 ]'
check "blob: equals the commit's blob id" '[ "$(L AIENOS_LOCK=$GOOD bash "$T" blob native/capability/aienos_capability.c)" = "$(git -C "$LK" rev-parse "$GOOD:native/capability/aienos_capability.c")" ]'
check "show: path absent at the locked commit is exit 1 (verified absence)" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" show native/capability/none.c)" = 1 ]'
check "show: lock A is read as A although HEAD is GOOD" '! L AIENOS_LOCK=$A bash "$T" show native/capability/aienos_capability.c | grep -q aienos_cap_mint'
check "identity: names the full lock and its tree" '[ "$(L AIENOS_LOCK=$GOOD bash "$T" identity | jq -r .commit_tree)" = "$(git -C "$LK" rev-parse "$GOOD^{tree}")" ]'
check "unavailable repository: exit 2, not 1" '[ "$(rc env AIENOS_LOCK_REPO=/nonexistent AIENOS_LOCK=$GOOD bash "$T" show native/capability/aienos_capability.c)" = 2 ]'
check "repository without the locked commit: exit 2" '[ "$(rc env AIENOS_LOCK_REPO="$LK" AIENOS_LOCK=0123456789abcdef0123456789abcdef01234567 bash "$T" identity)" = 2 ]'
check "abbreviated lock refused: exit 2" '[ "$(rc L AIENOS_LOCK=${GOOD:0:7} bash "$T" identity)" = 2 ]'
check "uppercase or padded lock refused: exit 2" '[ "$(rc L AIENOS_LOCK="${GOOD^^}" bash "$T" identity)" = 2 ] && [ "$(rc L AIENOS_LOCK="$GOOD " bash "$T" identity)" = 2 ]'
lkg replace "$A" "$GOOD"
check "replace ref A->GOOD ignored: A still lacks mint" '! L AIENOS_LOCK=$A bash "$T" show native/capability/aienos_capability.c | grep -q aienos_cap_mint'
lkg replace -d "$A" >/dev/null
# a loose object whose bytes were replaced on disk (here: by another object's bytes) no
# longer hashes to its id
CORR=$SCR/corrupt; cp -r "$LK" "$CORR"
B=$(git -C "$CORR" rev-parse "$A:native/capability/aienos_capability.c")
X=$(printf 'int aienos_cap_mint(int a)\n{ return a; }\n' | git -C "$CORR" hash-object --stdin -w)
obj="$CORR/.git/objects/${B:0:2}/${B:2}"
chmod u+w "$obj" && cp "$CORR/.git/objects/${X:0:2}/${X:2}" "$obj"
check "altered object bytes: refused (exit 2), never read as content" '[ "$(rc env AIENOS_LOCK_REPO="$CORR" AIENOS_R7_DIR= AIENOS_LOCK=$A bash "$T" show native/capability/aienos_capability.c)" = 2 ]'

# 2. directories: proven only against the locked commit
D=$SCR/cache
check "materialize into an empty cache: exit 0" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" materialize "$D" native/capability)" = 0 ]'
make -s -C "$D/native/capability" >/dev/null 2>&1
check "build outputs under an ignored dir do not break the proof" '[ -f "$D/native/capability/out/libaienos_capability.a" ] && [ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 0 ]'
echo 'int injected;' >> "$D/native/capability/aienos_capability.c"
check "modified cached source: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
check "materialize does not re-extract over a modified cache: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" materialize "$D" native/capability)" = 1 ]'
git -C "$LK" show "$GOOD:native/capability/aienos_capability.c" > "$D/native/capability/aienos_capability.c"
echo 'int extra;' > "$D/native/capability/extra.c"
check "extra unignored file beside the locked sources: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
rm "$D/native/capability/extra.c" "$D/native/capability/Makefile"
check "missing locked file: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
UNREL=$SCR/unrelated; mkdir -p "$UNREL/native/capability"; cp "$LK/native/capability/"* "$UNREL/native/capability/"
check "unrelated directory with plausible functions, no lock repo: exit 2" '[ "$(rc env AIENOS_LOCK_REPO=/nonexistent AIENOS_R7_DIR="$UNREL" AIENOS_LOCK=$GOOD bash "$T" verify-dir "$UNREL" native/capability)" = 2 ]'
check "unrelated directory checked against lock A: exit 1" '[ "$(rc L AIENOS_LOCK=$A bash "$T" verify-dir "$UNREL" native/capability)" = 1 ]'
CL=$SCR/clone; git clone -q "$LK" "$CL"; git -C "$CL" -c advice.detachedHead=false checkout -q "$GOOD"
check "clean checkout of a different commit (GOOD) against lock A: exit 1" '[ "$(rc env AIENOS_LOCK_REPO=/nonexistent AIENOS_R7_DIR="$CL" AIENOS_LOCK=$A bash "$T" verify-dir "$CL" native/capability)" = 1 ]'
check "clean checkout of the locked commit, proven through its own repository: exit 0" '[ "$(rc env AIENOS_LOCK_REPO=/nonexistent AIENOS_R7_DIR="$CL" AIENOS_LOCK=$GOOD bash "$T" verify-dir "$CL" native/capability)" = 0 ]'
echo 'int dirty;' >> "$CL/native/capability/aienos_capability.c"
check "dirty checkout of the locked commit: exit 1" '[ "$(rc env AIENOS_LOCK_REPO=/nonexistent AIENOS_R7_DIR="$CL" AIENOS_LOCK=$GOOD bash "$T" verify-dir "$CL" native/capability)" = 1 ]'

# 3. the Makefile rule: the capability library is built only from a proven source, on every build
W=$SCR/omega; mkdir -p "$W"
cp -r "$HERE/Makefile" "$HERE/mk" "$HERE/tools" "$W/"
cp "$HERE/physics.lock" "$W/" 2>/dev/null || true
echo "$GOOD" > "$W/aienos.lock"
M() { make -s -C "$W" AIENOS_LOCK_REPO="$LK" "$@" >"$SCR/make.log" 2>&1; echo $?; }
check "make: default cache materialized and library built" '[ "$(M build/aienos-authority/$GOOD/native/capability/out/libaienos_capability.a)" = 0 ] && [ -f "$W/build/aienos-authority/$GOOD/native/capability/out/libaienos_capability.a" ]'
echo 'int injected;' >> "$W/build/aienos-authority/$GOOD/native/capability/aienos_capability.c"
check "make: cache edited after extraction stops the next build" '[ "$(M build/aienos-authority/$GOOD/native/capability/out/libaienos_capability.a)" != 0 ] && grep -q "not the aienos.lock commit" "$SCR/make.log"'
check "make: unrelated AIENOS_R7_DIR override refused" '[ "$(M AIENOS_R7_DIR="$UNREL" "$UNREL/native/capability/out/libaienos_capability.a")" != 0 ]'
git -C "$CL" checkout -q -- .
check "make: clean checkout of a different commit as override refused" '[ "$(cd "$CL" && git checkout -q "$A"; M AIENOS_R7_DIR="$CL" "$CL/native/capability/out/libaienos_capability.a")" != 0 ]'
git -C "$CL" checkout -q "$GOOD"
check "make: clean checkout of the locked commit as override builds" '[ "$(M AIENOS_R7_DIR="$CL" "$CL/native/capability/out/libaienos_capability.a")" = 0 ]'
check "make: lock repository missing the commit refused" '[ "$(make -s -C "$W" AIENOS_LOCK_REPO=/nonexistent AIENOS_R7_DIR="$UNREL" "$UNREL/native/capability/out/libaienos_capability.a" >/dev/null 2>&1; echo $?)" != 0 ]'

if [ "$FAILS" -ne 0 ]; then echo "aienos lock source test: $FAILS FAILED"; exit 1; fi
echo "aienos lock source test: PASS"
