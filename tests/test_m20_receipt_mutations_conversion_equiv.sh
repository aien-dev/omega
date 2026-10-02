#!/bin/sh
# Proof that tools/m20_receipt_mutations.sh (now a thin adapter over
# tools/mutation_runner.sh) behaves exactly like the pre-conversion script
# from pinned commit 2dc9dd8: same stdout bytes, same exit code, same
# per-mutant verdicts. Host only, CPU only, light. Run from anywhere.
#
# The old script is extracted into tools/ (it does cd dirname/..) under a
# temporary name and removed by a trap. Both sweeps run on the same tree.
# Nothing is normalised: the sweep prints no temp paths or timings, so the
# stdout is compared byte for byte. This sweep writes no JSON.
# A negative check proves the comparison can fail.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
PIN=2dc9dd8
tmp=$(mktemp -d "${TMPDIR:-/tmp}/m20-equiv.XXXXXX") || exit 2
old=tools/.m20_receipt_mutations_old.$$.sh
trap 'rm -rf "$tmp" "$root/$old"' EXIT INT TERM

git show "$PIN:tools/m20_receipt_mutations.sh" > "$old" || { echo "FAIL cannot extract old script from $PIN"; exit 2; }
chmod +x "$old"

sh "$old" > "$tmp/old.out" 2>&1; echo $? > "$tmp/old.rc"
sh tools/m20_receipt_mutations.sh > "$tmp/new.out" 2>&1; echo $? > "$tmp/new.rc"

# verdict list: "<name> KILLED|SURVIVED|ERROR" from the MUTATION lines
verdicts() {
    sed -n 's/^MUTATION \([A-Z0-9_]*\): caught.*/\1 KILLED/p;
            s/^MUTATION \([A-Z0-9_]*\): NOT CAUGHT.*/\1 SURVIVED/p;
            s/^MUTATION \([A-Z0-9_]*\): NOT APPLIED.*/\1 ERROR/p' "$1"
}
# compare OLDOUT OLDRC NEWOUT NEWRC: returns 0 only if identical
compare() {
    cmp -s "$1" "$3" || return 1
    [ "$(cat "$2")" = "$(cat "$4")" ] || return 1
    [ "$(verdicts "$1")" = "$(verdicts "$3")" ] || return 1
    return 0
}

fail=0
n=$(verdicts "$tmp/old.out" | wc -l)
if [ "$n" -lt 1 ]; then echo "FAIL old sweep produced no mutant verdicts"; fail=1; fi
if compare "$tmp/old.out" "$tmp/old.rc" "$tmp/new.out" "$tmp/new.rc"; then
    echo "PASS old and new sweeps identical (stdout, exit code $(cat "$tmp/new.rc"), $n mutants compared)"
else
    echo "FAIL old and new sweeps differ"; diff "$tmp/old.out" "$tmp/new.out" | head -10; fail=1
fi

# Negative check: one verdict line altered must be reported different.
sed '0,/: caught/s//: NOT CAUGHT/' "$tmp/new.out" > "$tmp/bad.out"
if compare "$tmp/old.out" "$tmp/old.rc" "$tmp/bad.out" "$tmp/new.rc"; then
    echo "FAIL negative check: altered verdict was not detected"; fail=1
else
    echo "PASS negative check: altered verdict line is reported different"
fi
# Negative check 2: a different exit code must be reported different.
echo 1 > "$tmp/bad.rc"
if compare "$tmp/old.out" "$tmp/old.rc" "$tmp/new.out" "$tmp/bad.rc" && [ "$(cat "$tmp/old.rc")" != 1 ]; then
    echo "FAIL negative check: altered exit code was not detected"; fail=1
else
    echo "PASS negative check: altered exit code is reported different"
fi
[ $fail -eq 0 ] && echo "m20 conversion equivalence: PASS" || echo "m20 conversion equivalence: FAIL"
exit $fail
