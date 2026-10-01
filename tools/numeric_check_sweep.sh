#!/bin/bash
# numeric_check_sweep.sh -- proves each pre-submission check in
# src/omega_numeric.c is load-bearing. Every refusal line there ends in a
# /* CHECK:<name> */ marker. For each marker this script deletes that one
# line in a scratch copy, rebuilds the CPU-only Gate 5 test binary and runs
# it. The sweep passes only if every copy still builds and every run FAILS
# (a check whose removal no test notices is reported as SURVIVED). Unused-
# variable warnings are allowed in the scratch copies only: deleting a check
# can leave the value it tested unused.
# Host only: opens no device, writes only under a temporary directory.
# Run: make test-numeric-sweep   (or run this file).
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
SRC=$HERE/src/omega_numeric.c
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
OTHER="tests/test_omega_numeric.c src/omega_numeric_provenance.c src/omega_numeric_divsqrt_gb10.c src/omega_blackwell_encoder.c
       src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c"
names=$(grep -o '/\* CHECK:[a-z0-9_]* \*/' "$SRC" | sed 's|/\* CHECK:\([a-z0-9_]*\) \*/|\1|')
total=0 killed=0 survived=0 broken=0
for name in $names; do
    total=$((total + 1))
    mut=$TMP/omega_numeric.c
    grep -v "/\* CHECK:$name \*/" "$SRC" > "$mut"
    if [ "$(wc -l < "$SRC")" -ne $(( $(wc -l < "$mut") + 1 )) ]; then
        echo "  [BROKEN]   $name: marker is not on exactly one line"; broken=$((broken + 1)); continue
    fi
    # shellcheck disable=SC2086
    if ! (cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc \
            -Wno-unused-variable -Wno-unused-but-set-variable \
            -DOMEGA_NUMERIC_CPU_ONLY -o "$TMP/t" "$mut" $OTHER) > "$TMP/build.log" 2>&1; then
        echo "  [BROKEN]   $name: scratch copy does not build"; broken=$((broken + 1)); continue
    fi
    if "$TMP/t" > "$TMP/run.log" 2>&1; then
        echo "  [SURVIVED] $name: removing this check fails no test"; survived=$((survived + 1))
    else
        echo "  [KILLED]   $name: $(grep -m3 '^\[FAIL\]' "$TMP/run.log" | sed 's/^\[FAIL\] //' | tr '\n' ' ')"
        killed=$((killed + 1))
    fi
done
echo "check sweep: $total checks, $killed caught by a failing test, $survived survived, $broken broken"
[ "$total" -gt 0 ] && [ "$killed" -eq "$total" ]
