#!/bin/sh
# Proof that tools/autodiff_mutations.sh (now an adapter over
# tools/mutation_runner.sh) behaves like the pinned legacy script. Host, CPU
# only. Runs the OLD script (git show 2dc9dd8:tools/autodiff_mutations.sh,
# placed under tools/ because it cds to dirname/..; removed by the trap) and
# the NEW script on the same tree, then compares stdout and exit code, and the
# per-mutant verdict list (name + KILLED/SURVIVED/ERROR, derived from the
# legacy "MUTATION <name>: ..." lines). Nothing is normalised: both scripts'
# stdout carries no paths or timings (scratch dirs are never printed on the
# pass path). A negative check proves the comparison can fail.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
PIN=2dc9dd8
old=tools/.autodiff_mutations_old.$$.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/autodiff-equiv.XXXXXX") || exit 2
trap 'rm -rf "$tmp" "$root/$old"' EXIT INT TERM
git show "$PIN:tools/autodiff_mutations.sh" > "$old" || { echo "FAIL cannot extract $PIN"; exit 1; }
chmod +x "$old"
sh "$old" > "$tmp/old.out" 2>/dev/null; echo $? > "$tmp/old.rc"
sh tools/autodiff_mutations.sh > "$tmp/new.out" 2>/dev/null; echo $? > "$tmp/new.rc"

verdicts() { # stdout file -> "name VERDICT" lines
    sed -n 's/^MUTATION \([A-Z_0-9]*\): caught.*/\1 KILLED/p;
            s/^MUTATION \([A-Z_0-9]*\): NOT CAUGHT.*/\1 SURVIVED/p;
            s/^MUTATION \([A-Z_0-9]*\): \(NOT APPLIED\|does not build\).*/\1 ERROR/p' "$1"
}
verdicts "$tmp/old.out" > "$tmp/old.v"
verdicts "$tmp/new.out" > "$tmp/new.v"
count=$(wc -l < "$tmp/old.v")

fails=0
chk() { if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
[ "$count" -ge 5 ]; chk "old script produced at least 5 mutant verdicts ($count)" $?
cmp -s "$tmp/old.rc" "$tmp/new.rc"; chk "exit code identical (old $(cat "$tmp/old.rc"), new $(cat "$tmp/new.rc"))" $?
cmp -s "$tmp/old.out" "$tmp/new.out"; chk "stdout byte-identical" $?
cmp -s "$tmp/old.v" "$tmp/new.v"; chk "per-mutant verdict list identical" $?

# Negative check: an altered verdict line must be reported as different.
sed '1s/KILLED/SURVIVED/' "$tmp/new.v" > "$tmp/alt.v"
if cmp -s "$tmp/old.v" "$tmp/alt.v"; then chk "negative: altered verdict is detected" 1; else chk "negative: altered verdict is detected" 0; fi
sed '1s/^MUTATION/MUTATIONX/' "$tmp/new.out" > "$tmp/alt.out"
if cmp -s "$tmp/old.out" "$tmp/alt.out"; then chk "negative: altered stdout is detected" 1; else chk "negative: altered stdout is detected" 0; fi
echo $(( $(cat "$tmp/old.rc") + 1 )) > "$tmp/alt.rc"
if cmp -s "$tmp/old.rc" "$tmp/alt.rc"; then chk "negative: altered exit code is detected" 1; else chk "negative: altered exit code is detected" 0; fi

echo "compared $count mutants"
if [ "$fails" -ne 0 ]; then echo "autodiff conversion equivalence: FAIL"; exit 1; fi
echo "autodiff conversion equivalence: PASS"
