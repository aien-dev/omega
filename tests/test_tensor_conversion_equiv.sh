#!/bin/sh
# Proof that tools/tensor_mutations.sh, now an adapter over
# tools/mutation_runner.sh, behaves like the legacy sweep. Host only, CPU only.
# Runs the OLD script (pinned commit 2dc9dd8) and the NEW script on the same
# tree and requires identical stdout, identical exit code and an identical
# per-mutant verdict list (name + KILLED/SURVIVED/ERROR).
#
# Normalised text: none. The sweeps print no temp paths or timings on stdout
# (scratch dirs are only used internally), so stdout is compared byte for byte.
# (The sweep has no JSON output; the c4 sweep is the only one that does.)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
PIN=2dc9dd8
old=tools/tensor_mutations_legacy.$$.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tensor-equiv.XXXXXX") || exit 2
trap 'rm -rf "$tmp" "$root/$old"' EXIT INT TERM
git show "$PIN:tools/tensor_mutations.sh" > "$old" || { echo "FAIL cannot extract $PIN:tools/tensor_mutations.sh"; exit 2; }
chmod +x "$old"
fails=0
ok() { if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

# verdicts FILE: "name VERDICT" per MUTATION line, in order.
verdicts() {
    sed -n 's/^MUTATION \([A-Za-z0-9_ ]*\): caught.*/\1 KILLED/p;
            s/^MUTATION \([A-Za-z0-9_ ]*\): NOT CAUGHT.*/\1 SURVIVED/p;
            s/^MUTATION \([A-Za-z0-9_ ]*\): \(NOT APPLIED\|does not build\).*/\1 ERROR/p;
            s/^MUTATION \(store baseline\): .*/\1 ERROR/p' "$1"
}

sh "$old" > "$tmp/old.out" 2>/dev/null; echo $? > "$tmp/old.rc"
tools/tensor_mutations.sh > "$tmp/new.out" 2>/dev/null; echo $? > "$tmp/new.rc"
verdicts "$tmp/old.out" > "$tmp/old.v"
verdicts "$tmp/new.out" > "$tmp/new.v"
n=$(wc -l < "$tmp/new.v")

cmp -s "$tmp/old.out" "$tmp/new.out"; ok "stdout identical old vs new" $?
cmp -s "$tmp/old.rc" "$tmp/new.rc"; ok "exit code identical old vs new ($(cat "$tmp/old.rc") vs $(cat "$tmp/new.rc"))" $?
cmp -s "$tmp/old.v" "$tmp/new.v"; ok "per-mutant verdict list identical" $?
[ "$n" -ge 26 ]; ok "compared at least 26 mutants (20 main + 6 store), got $n" $?
[ "$(wc -l < "$tmp/old.v")" = "$n" ]; ok "old and new list the same number of mutants" $?

# Negative checks: the comparison must be able to fail.
sed '0,/: caught/s//: NOT CAUGHT/' "$tmp/new.out" > "$tmp/alt.out"
cmp -s "$tmp/old.out" "$tmp/alt.out"; [ $? -ne 0 ]; ok "negative: altered stdout is reported different" $?
verdicts "$tmp/alt.out" > "$tmp/alt.v"
cmp -s "$tmp/old.v" "$tmp/alt.v"; [ $? -ne 0 ]; ok "negative: altered verdict list is reported different" $?
echo $(( $(cat "$tmp/old.rc") + 1 )) > "$tmp/alt.rc"
cmp -s "$tmp/old.rc" "$tmp/alt.rc"; [ $? -ne 0 ]; ok "negative: altered exit code is reported different" $?

if [ "$fails" -ne 0 ]; then echo "tensor conversion equivalence: FAIL ($fails)"; exit 1; fi
echo "tensor conversion equivalence: PASS ($n mutants compared)"
