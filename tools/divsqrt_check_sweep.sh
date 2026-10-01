#!/bin/bash
# divsqrt_check_sweep.sh -- proves each pre-submission check in
# src/omega_numeric_divsqrt_gb10.c is load-bearing. Every check is one line
# ending in a /* CHECK:<name> */ marker. For each marker this script deletes
# that line in a scratch copy, rebuilds the CPU-only test and runs the host
# tier. The sweep passes only if every copy builds and every run FAILS.
# Unused-variable and unused-parameter warnings are allowed in the scratch
# copies only: deleting a check can leave the value it tested unused.
# Host only: opens no device, writes only under a temporary directory.
# Run: make test-divsqrt-sweep   (or run this file).
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
SRC=$HERE/src/omega_numeric_divsqrt_gb10.c
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
OTHER="tests/test_omega_divsqrt_gb10.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c
       src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c"
names=$(grep -o '/\* CHECK:[a-z0-9_]* \*/' "$SRC" | sed 's|/\* CHECK:\([a-z0-9_]*\) \*/|\1|')
total=0 killed=0 survived=0 broken=0
for name in $names; do
    total=$((total + 1))
    mut=$TMP/omega_numeric_divsqrt_gb10.c
    grep -v "/\* CHECK:$name \*/" "$SRC" > "$mut"
    if [ "$(wc -l < "$SRC")" -ne $(( $(wc -l < "$mut") + 1 )) ]; then
        echo "  [BROKEN]   $name: marker is not on exactly one line"; broken=$((broken + 1)); continue
    fi
    # shellcheck disable=SC2086
    if ! (cd "$HERE" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc -pthread \
            -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter \
            -DOMEGA_NUMERIC_CPU_ONLY -o "$TMP/t" "$mut" $OTHER) > "$TMP/build.log" 2>&1; then
        echo "  [BROKEN]   $name: scratch copy does not build"; broken=$((broken + 1)); continue
    fi
    if "$TMP/t" > "$TMP/run.log" 2>&1; then
        echo "  [SURVIVED] $name: removing this check fails no test"; survived=$((survived + 1))
    else
        echo "  [KILLED]   $name: $(grep -m2 '^\[FAIL\]' "$TMP/run.log" | sed 's/^\[FAIL\] //' | tr '\n' ' ')"
        killed=$((killed + 1))
    fi
done
echo "SWEEP total=$total killed=$killed survived=$survived broken=$broken"
[ "$total" -gt 0 ] && [ "$killed" = "$total" ] && echo "VERDICT PASS" && exit 0
echo "VERDICT FAIL"; exit 1
