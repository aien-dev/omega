#!/bin/sh
# Compile real wrappers with one safety check removed. Compilation failure is
# not a kill: each mutated binary must run and reject a named test scenario.
set -eu
ulimit -c 0
out=${1:?output directory required}
cc=${CC:-cc}
mkdir -p "$out"
for mutant in poison retain exact; do
    dir="$out/$mutant"
    mkdir -p "$dir"
    cp src/omega_numeric_lifecycle.h "$dir/"
    case "$mutant" in
      poison) sed 's/omega_numeric_native_poison();/(void)0;/g' src/omega_numeric_native.h > "$dir/omega_numeric_native.h" ;;
      retain) sed 's/if (omega_numeric_native_uncertain()) return -1;/(void)0;/' src/omega_numeric_native.h > "$dir/omega_numeric_native.h" ;;
      exact) sed 's/if (rc == 0 \&\& (!word || \*word != want)) rc = -1;/(void)0;/' src/omega_numeric_native.h > "$dir/omega_numeric_native.h" ;;
    esac
    if cmp -s src/omega_numeric_native.h "$dir/omega_numeric_native.h"; then
        echo "FAIL: mutation $mutant changed nothing"; exit 1
    fi
    $cc -std=c11 -O2 -Wall -Wextra -Werror -I"$dir" -Isrc -Itests/numeric_native tests/test_numeric_lifecycle.c tests/numeric_native/other_launcher.c -o "$dir/test"
    rc=0
    "$dir/test" > "$dir/output" 2>&1 || rc=$?
    if [ "$rc" -ne 1 ] || ! grep -q '^FAIL numeric lifecycle scenario ' "$dir/output"; then
        echo "FAIL: mutation $mutant survived or did not run"; cat "$dir/output"; exit 1
    fi
    echo "numeric lifecycle mutant $mutant: killed"
done
