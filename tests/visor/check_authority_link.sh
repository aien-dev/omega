#!/bin/sh
# Omega Visor V1, lane 7: authority isolation link check.
# "The Visor can ask. The Visor cannot grant."
#
# Fails (exit 1) when any Visor-side object, or the omega binary, references
# an authority-mutating symbol from forbidden_symbols.txt, references any
# runtime / AIENOS / publish-gate symbol at all (^rx_ ^aienos_cap_ ^rc_), or
# DEFINES a forbidden name (a local reimplementation). Also checks that
# visor_effect_request.o exports only its four public functions and that no
# Visor source assigns `authorized` anything but false.
# Usage: tests/visor/check_authority_link.sh [build_dir]   (default: build)
set -u
BUILD=${1:-build}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LIST="$HERE/forbidden_symbols.txt"
NM=${NM:-nm}
fail=0
checked=0

[ -f "$LIST" ] || { echo "FAIL: missing $LIST"; exit 1; }
forbidden=$(sed -e 's/#.*//' -e 's/[[:space:]]//g' "$LIST" | grep -v '^$')
[ -n "$forbidden" ] || { echo "FAIL: forbidden list is empty"; exit 1; }

set -- "$BUILD"/visor/*.o
if [ ! -e "$1" ]; then
    echo "FAIL: no objects in $BUILD/visor (build the omega target first)"
    echo "OMEGA_VISOR_AUTHORITY_ISOLATION_FAIL"
    exit 1
fi

check_file() {
    f=$1
    if [ ! -e "$f" ]; then
        echo "skip: $f (not built)"
        return 0
    fi
    checked=$((checked + 1))
    undef=$($NM -u "$f" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//')
    defs=$($NM --defined-only "$f" 2>/dev/null | awk '{print $NF}')
    for s in $forbidden; do
        if printf '%s\n' "$undef" | grep -qx "$s"; then
            echo "FAIL: $f references forbidden symbol $s"; fail=1
        fi
        if printf '%s\n' "$defs" | grep -qx "$s"; then
            echo "FAIL: $f defines forbidden symbol $s"; fail=1
        fi
    done
    bad=$(printf '%s\n' "$undef" | grep -E '^(rx_|aienos_cap_|rc_)' || true)
    if [ -n "$bad" ]; then
        for s in $bad; do echo "FAIL: $f references runtime/authority symbol $s"; done
        fail=1
    fi
}

for f in "$BUILD"/visor/*.o "$BUILD"/language/*.o "$BUILD"/omega_main.o "$BUILD"/omega; do
    check_file "$f"
done

# The adapter exports exactly its four public functions: no setter, no submit.
ER="$BUILD/visor/visor_effect_request.o"
if [ -e "$ER" ]; then
    exports=$($NM --defined-only -g "$ER" | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort | tr '\n' ' ')
    want="visor_effect_request_build visor_effect_request_classify visor_effect_request_format_json visor_effect_request_format_text "
    if [ "$exports" != "$want" ]; then
        echo "FAIL: $ER exports [$exports], expected [$want]"; fail=1
    fi
else
    echo "FAIL: $ER missing (lane 7 adapter not built)"; fail=1
fi

# No Visor source assigns `authorized` anything but false.
hits=$(grep -rnE '(\.|->)authorized[[:space:]]*=[^=]' "$ROOT/src/visor" "$ROOT/src/language" "$ROOT/tools/omega.c" 2>/dev/null \
        | grep -vE 'authorized[[:space:]]*=[[:space:]]*false[[:space:]]*;' || true)
if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "FAIL: a Visor source assigns 'authorized' other than false"; fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "OMEGA_VISOR_AUTHORITY_ISOLATION_FAIL"
    exit 1
fi
echo "checked $checked file(s) against $(printf '%s\n' $forbidden | wc -l) forbidden symbols"
echo "OMEGA_VISOR_AUTHORITY_ISOLATION_PASS"
exit 0
