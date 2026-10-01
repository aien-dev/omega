#!/bin/sh
# OSC-1 determinism gate (DESIGN section 8): compile every golden program
# twice in separate processes and compare the printed IR digest, machine-code
# digest and function count byte for byte.
# Usage: tests/compiler/determinism.sh [path/to/oscc]
# Without an argument oscc is built single core under /tmp/l22c.
# Prints OSC1_DETERMINISM_PASS or OSC1_DETERMINISM_FAIL.
# OSC-1 slice; not a general Omega compiler; no self-hosting.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=/tmp/l22c/det
mkdir -p "$out" || exit 1
if [ $# -ge 1 ]; then
    oscc=$1
else
    oscc=/tmp/l22c/oscc
    srcs=""
    for f in "$root"/src/compiler/*.c; do
        case "$f" in
            */oscc_main.c) ;;
            *) srcs="$srcs $f" ;;
        esac
    done
    # shellcheck disable=SC2086
    nice cc -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 \
        -I"$root/src" -I"$root/src/compiler" -I"$root/src/compiler/model" \
        -o "$oscc" "$root/src/compiler/oscc_main.c" $srcs "$root/src/sha256.c" || {
        echo "OSC1_DETERMINISM_FAIL build"
        exit 1
    }
fi
n=0
bad=0
for p in "$root"/tests/compiler/progs/*.osc; do
    b=$(basename "$p" .osc)
    nice "$oscc" "$p" > "$out/$b.1" 2>&1; r1=$?
    nice "$oscc" "$p" > "$out/$b.2" 2>&1; r2=$?
    n=$((n + 1))
    if [ "$r1" -ne 0 ] || [ "$r2" -ne 0 ]; then
        echo "FAIL $b: exit $r1 / $r2"
        bad=$((bad + 1))
    elif ! cmp -s "$out/$b.1" "$out/$b.2"; then
        echo "FAIL $b: outputs differ"
        bad=$((bad + 1))
    elif ! grep -q '^ir_sha256=' "$out/$b.1" || ! grep -q '^code_sha256=' "$out/$b.1"; then
        echo "FAIL $b: digests missing"
        bad=$((bad + 1))
    fi
done
if [ "$n" -ge 1 ] && [ "$bad" -eq 0 ]; then
    echo "programs=$n identical=$n"
    echo "OSC1_DETERMINISM_PASS"
    exit 0
fi
echo "programs=$n failed=$bad"
echo "OSC1_DETERMINISM_FAIL"
exit 1
