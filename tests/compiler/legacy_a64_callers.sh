#!/bin/sh
# OSC-2 item 4: the legacy AArch64 writer (src/aarch64_encoder.c) takes no new callers.
# OSC-2 slice; not a general Omega compiler; no self-hosting.
# Lists every C source/header in the tree (tracked plus untracked, not ignored) that
# includes aarch64_encoder.h or names an aarch64_emit_* function and compares the set
# with tests/compiler/legacy_a64_allowlist.txt. Exit 1 with
#   A64_LEGACY_NEW_CALLER <file>        a file not on the frozen list uses the writer
#   A64_LEGACY_ALLOWLIST_STALE <file>   a listed file no longer uses it (drop the line)
# Usage: legacy_a64_callers.sh [--selftest]
#   --selftest first proves both refusals fire on a synthetic tree (a new includer, a
#   new direct caller, a stale list line), then checks the real tree.
# Last line on success: OSC2_LEGACY_CALLERS_PASS callers=<n> [selftest=3/3]
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
cd "$ROOT"
LIST=tests/compiler/legacy_a64_allowlist.txt
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PAT='aarch64_encoder\.h|aarch64_emit_[a-z0-9_]+'

# compare <file-list> <allowlist> <out>: writes the refusal lines to <out>, returns 1 if any
compare() {
    : >"$T/found"
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        if grep -qE "$PAT" "$f"; then echo "$f" >>"$T/found"; fi
    done <"$1"
    LC_ALL=C sort -u "$T/found" >"$T/found.s"
    grep -v '^#' "$2" | grep -v '^[[:space:]]*$' | LC_ALL=C sort -u >"$T/allow.s"
    LC_ALL=C comm -23 "$T/found.s" "$T/allow.s" | sed 's/^/A64_LEGACY_NEW_CALLER /' >"$3"
    LC_ALL=C comm -13 "$T/found.s" "$T/allow.s" | sed 's/^/A64_LEGACY_ALLOWLIST_STALE /' >>"$3"
    [ ! -s "$3" ]
}

ST=
if [ "${1:-}" = --selftest ]; then
    mkdir -p "$T/s"
    printf '#include "aarch64_encoder.h"\n' >"$T/s/new_include.c"
    printf 'int f(void) { return aarch64_emit_ret(0, 0, 0); }\n' >"$T/s/new_call.c"
    printf 'int g(void) { return 0; }\n' >"$T/s/clean.c"
    printf '%s\n' "$T/s/new_include.c" "$T/s/new_call.c" "$T/s/clean.c" >"$T/s/files"
    printf '# test list\n%s\n' "$T/s/clean.c" >"$T/s/allow"
    ok=0
    if compare "$T/s/files" "$T/s/allow" "$T/s/out"; then :; fi
    grep -qx "A64_LEGACY_NEW_CALLER $T/s/new_include.c" "$T/s/out" && ok=$((ok + 1))
    grep -qx "A64_LEGACY_NEW_CALLER $T/s/new_call.c" "$T/s/out" && ok=$((ok + 1))
    grep -qx "A64_LEGACY_ALLOWLIST_STALE $T/s/clean.c" "$T/s/out" && ok=$((ok + 1))
    [ "$(wc -l <"$T/s/out" | tr -d ' ')" = 3 ] || ok=0
    if [ "$ok" != 3 ]; then cat "$T/s/out"; echo "selftest: refusals did not fire ($ok/3)"; echo "OSC2_LEGACY_CALLERS_FAIL"; exit 1; fi
    ST=" selftest=3/3"
fi

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git ls-files --cached --others --exclude-standard -- '*.c' '*.h' >"$T/files"
else
    find . -path ./build -prune -o -type f \( -name '*.c' -o -name '*.h' \) -print | sed 's|^\./||' >"$T/files"
fi
if ! compare "$T/files" "$LIST" "$T/out"; then cat "$T/out"; echo "OSC2_LEGACY_CALLERS_FAIL"; exit 1; fi
echo "OSC2_LEGACY_CALLERS_PASS callers=$(wc -l <"$T/found.s" | tr -d ' ')$ST"
