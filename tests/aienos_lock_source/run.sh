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
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=advice.detachedHead GIT_CONFIG_VALUE_0=false

# Fixture aienos: commit GOOD has a buildable native/capability; commit A lacks aienos_cap_mint.
LK=$SCR/aienos
lkg() { git -C "$LK" -c user.name=t -c user.email=t@invalid -c commit.gpgsign=false "$@"; }
mkdir -p "$LK/native/capability" "$LK/native/store"
printf 'out/\n' > "$LK/native/capability/.gitignore"
printf 'int aienos_cap_validate(int v)\n{ return v; }\nint aienos_cap_mint(int a)\n{ return a; }\n' > "$LK/native/capability/aienos_capability.c"
printf 'OUT ?= out\n$(OUT)/libaienos_capability.a: aienos_capability.c\n\tmkdir -p $(OUT) && $(CC) -c -o $(OUT)/aienos_capability.o aienos_capability.c && ar rcs $@ $(OUT)/aienos_capability.o\n' > "$LK/native/capability/Makefile"
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
check "proven cache verifies: exit 0" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 0 ]'
make -s -C "$D/native/capability" >/dev/null 2>&1
check "in-tree build outputs (ignored out/, could hold planted objects) refused: exit 1" '[ -f "$D/native/capability/out/libaienos_capability.a" ] && [ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
rm -rf "$D/native/capability/out"
echo 'int evil;' > "$D/native/capability/evil.c"; printf '*.c\n' > "$D/.gitignore"; printf 'evil.c\n' > "$D/native/.gitignore"
check "extra file hidden by ignore files above the checked subpath: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
rm -f "$D/native/capability/evil.c" "$D/.gitignore" "$D/native/.gitignore"
chmod +x "$D/native/capability/aienos_capability.c"
check "file mode changed: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
chmod -x "$D/native/capability/aienos_capability.c"
# Attribute and config filters must not decide what "matches" means: a planted
# "* text eol=lf" (root, above the subpath, or in the lock repo's info/attributes)
# plus core.autocrlf would make git normalize CRLF edits away.
sed -i 's/$/\r/' "$D/native/capability/aienos_capability.c"
printf '* text eol=lf\n' > "$D/.gitattributes"; printf '* text eol=lf\n' > "$D/native/.gitattributes"
mkdir -p "$(git -C "$LK" rev-parse --absolute-git-dir)/info"; printf '* text eol=lf\n' > "$(git -C "$LK" rev-parse --absolute-git-dir)/info/attributes"
check "CRLF edit hidden by planted .gitattributes and info/attributes: exit 1" '[ "$(rc L GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.autocrlf GIT_CONFIG_VALUE_0=input AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
rm -f "$D/.gitattributes" "$D/native/.gitattributes" "$(git -C "$LK" rev-parse --absolute-git-dir)/info/attributes"
git -C "$LK" show "$GOOD:native/capability/aienos_capability.c" > "$D/native/capability/aienos_capability.c"
check "restored file verifies again: exit 0" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 0 ]'
cp "$D/native/capability/aienos_capability.c" "$SCR/same-bytes.c"; rm "$D/native/capability/aienos_capability.c"
ln -s "$SCR/same-bytes.c" "$D/native/capability/aienos_capability.c"
check "file replaced by a symlink to identical bytes: exit 1" '[ "$(rc L AIENOS_LOCK=$GOOD bash "$T" verify-dir "$D" native/capability)" = 1 ]'
rm "$D/native/capability/aienos_capability.c"; cp "$SCR/same-bytes.c" "$D/native/capability/aienos_capability.c"
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
check "make: default cache materialized and library built" '[ "$(M build/aienos-cap/$GOOD/libaienos_capability.a)" = 0 ] && [ -f "$W/build/aienos-cap/$GOOD/libaienos_capability.a" ] && [ ! -e "$W/build/aienos-authority/$GOOD/native/capability/out" ]'
L1=$(stat -c %Y.%s "$W/build/aienos-cap/$GOOD/libaienos_capability.a"); sleep 1
check "make: second build replaces nothing (no relink)" '[ "$(M build/aienos-cap/$GOOD/libaienos_capability.a)" = 0 ] && [ "$(stat -c %Y.%s "$W/build/aienos-cap/$GOOD/libaienos_capability.a")" = "$L1" ]'
printf 'junk' > "$W/build/aienos-cap/$GOOD/libaienos_capability.a"
check "make: planted library replaced by the fresh build from the proven source" '[ "$(M build/aienos-cap/$GOOD/libaienos_capability.a)" = 0 ] && ar t "$W/build/aienos-cap/$GOOD/libaienos_capability.a" | grep -q aienos_capability.o'
echo 'int injected;' >> "$W/build/aienos-authority/$GOOD/native/capability/aienos_capability.c"
check "make: cache edited after extraction stops the next build" '[ "$(M build/aienos-cap/$GOOD/libaienos_capability.a)" != 0 ] && grep -q "not the aienos.lock commit" "$SCR/make.log"'
check "make: unrelated AIENOS_R7_DIR override refused" '[ "$(M AIENOS_R7_DIR="$UNREL" build/aienos-cap/$GOOD/libaienos_capability.a)" != 0 ]'
git -C "$CL" checkout -q -- .
check "make: clean checkout of a different commit as override refused" '[ "$(cd "$CL" && git -c advice.detachedHead=false checkout -q "$A"; M AIENOS_R7_DIR="$CL" build/aienos-cap/$GOOD/libaienos_capability.a)" != 0 ]'
git -C "$CL" checkout -q "$GOOD"
mkdir -p "$CL/native/capability/out"; echo planted > "$CL/native/capability/out/aienos_capability.o"
check "make: locked checkout with a planted out/ object refused" '[ "$(M AIENOS_R7_DIR="$CL" build/aienos-cap/$GOOD/libaienos_capability.a)" != 0 ]'
rm -rf "$CL/native/capability/out"
check "make: clean checkout of the locked commit as override builds" '[ "$(M AIENOS_R7_DIR="$CL" build/aienos-cap/$GOOD/libaienos_capability.a)" = 0 ]'
check "make: lock repository missing the commit refused" '[ "$(make -s -C "$W" AIENOS_LOCK_REPO=/nonexistent AIENOS_R7_DIR="$UNREL" build/aienos-cap/$GOOD/libaienos_capability.a >/dev/null 2>&1; echo $?)" != 0 ]'

if [ "$FAILS" -ne 0 ]; then echo "aienos lock source test: $FAILS FAILED"; exit 1; fi
echo "aienos lock source test: PASS"
