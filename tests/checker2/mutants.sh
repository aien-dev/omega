#!/bin/sh
# Negative controls for checker2 (omega#446): every line tagged GUARD:<name> in a checker2 source
# is removed, one at a time, in a scratch copy; the matching test is rebuilt against that copy and
# MUST fail. A mutant that survives means the test cannot see that check, and this script fails.
# Also requires the unmutated build to pass first (otherwise "fails" proves nothing).
# Usage: sh tests/checker2/mutants.sh <cc> <scratch-dir>
set -u
CC=${1:-cc}
OUT=${2:-build/checker2/mutants}
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O1 -Isrc -Isrc/checker2"
SIG="src/searchtrace/sig/ed25519.c src/searchtrace/sig/sha512.c src/searchtrace/sig/ct.c"
mkdir -p "$OUT"
total=0
killed=0
survived=""

# run_family <source under test> <test .c> <other sources> <test args>
run_family() {
    src=$1; test_c=$2; others=$3; targs=$4
    base="$OUT/$(basename "$src" .c)"
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS -o "$base.orig" "$test_c" "$src" $others 2>"$base.orig.log"; then
        echo "mutants: baseline build failed for $src"; cat "$base.orig.log"; exit 1
    fi
    # shellcheck disable=SC2086
    if ! "$base.orig" $targs >/dev/null 2>&1; then
        echo "mutants: baseline test fails for $src; negative controls are meaningless"; exit 1
    fi
    for name in $(grep -o 'GUARD:[a-z0-9-]*' "$src" | sed 's/GUARD://' | sort -u); do
        total=$((total + 1))
        m="$base.$name.c"
        # drop the tagged line; a tagged "if (...)" line without ";" also drops its one-line body
        awk -v tag="GUARD:$name" '
            skip { skip = 0; next }
            index($0, tag) && substr($0, index($0, tag) + length(tag)) ~ /^([^a-z0-9-]|$)/ { if ($0 !~ /;/) skip = 1; next }
            { print }' "$src" > "$m"
        # shellcheck disable=SC2086
        if ! $CC $CFLAGS -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
            -Wno-error -o "$base.$name" "$test_c" "$m" $others 2>"$base.$name.log"; then
            echo "mutants: $src GUARD:$name does not build (counts as killed)"
            killed=$((killed + 1))
            continue
        fi
        # shellcheck disable=SC2086
        if timeout 120 "$base.$name" $targs >/dev/null 2>&1; then
            survived="$survived $src:$name"
        else
            killed=$((killed + 1))
        fi
    done
}

run_family src/checker2/c2_receipt.c tests/checker2/test_c2_receipt.c "src/sha256.c $SIG" tests/checker2/fixtures

echo "checker2 mutants: $killed of $total killed"
if [ -n "$survived" ]; then
    echo "SURVIVED (a test cannot see this check):$survived"
    exit 1
fi
[ "$total" -gt 0 ] || { echo "mutants: no GUARD lines found"; exit 1; }
exit 0
