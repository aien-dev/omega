#!/bin/sh
# Conversion proof: tools/c4_requal_mutants.sh (now an adapter over
# tools/mutation_runner.sh) must behave exactly like the pinned old script
# (commit 2dc9dd8). Host only, CPU only. PREREQUISITE: build/aienos-authority
# must exist (run 'make test-c4-requal' first); both sweeps build 16 mutants,
# so this is slow. Env ONLY="M01 M05" narrows both sweeps to the same ids.
#
# Compared: stdout, exit code, the JSON receipt (byte for byte), and the
# per-mutant verdict list (id + KILLED/SURVIVED*/ERROR from the stderr lines).
# Normalised (volatile only): the receipt path in the stdout summary line
# (each sweep writes to its own file) and /tmp/c4mut.* or /tmp/mutrun.* scratch
# directory names. Nothing else is rewritten.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
old=tools/_c4_requal_mutants_old.$$.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/c4equiv.XXXXXX") || exit 2
trap 'rm -rf "$tmp" "$root/$old"' EXIT INT TERM
git show 2dc9dd8:tools/c4_requal_mutants.sh > "$old" || { echo "FAIL cannot extract pinned old script"; exit 2; }
chmod +x "$old"

norm() { # file -> normalised on stdout
    sed -e "s#$tmp/[A-Za-z0-9_.]*json#OUT.json#g" -e 's#/tmp/c4mut\.[A-Za-z0-9]*#TMP#g' \
        -e 's#/tmp/mutrun\.[A-Za-z0-9]*#TMP#g' "$1"
}
verdicts() { # stderr file -> "id STATUS" per mutant
    sed -n 's/^\(M[0-9][0-9]*\) \(KILLED\|SURVIVED_REDUNDANT\|SURVIVED_GAP\|SURVIVED\|ERROR\) .*/\1 \2/p' "$1"
}
# same <fileA> <fileB> <label>: succeeds only if identical
same() { if cmp -s "$1" "$2"; then return 0; else echo "DIFF $3"; diff "$1" "$2" | head -5; return 1; fi; }

sh tools/c4_requal_mutants.sh "$tmp/new.json" > "$tmp/new.out" 2> "$tmp/new.err"; echo $? > "$tmp/new.rc"
sh "$old" "$tmp/old.json" > "$tmp/old.out" 2> "$tmp/old.err"; echo $? > "$tmp/old.rc"
norm "$tmp/old.out" > "$tmp/old.out.n"; norm "$tmp/new.out" > "$tmp/new.out.n"
verdicts "$tmp/old.err" > "$tmp/old.v"; verdicts "$tmp/new.err" > "$tmp/new.v"
norm "$tmp/old.err" > "$tmp/old.err.n"; norm "$tmp/new.err" > "$tmp/new.err.n"

fails=0
n=$(wc -l < "$tmp/old.v")
[ "$n" -gt 0 ] || { echo "FAIL old sweep produced no verdicts"; fails=$((fails + 1)); }
same "$tmp/old.out.n" "$tmp/new.out.n" "stdout" || fails=$((fails + 1))
same "$tmp/old.rc" "$tmp/new.rc" "exit code" || fails=$((fails + 1))
same "$tmp/old.json" "$tmp/new.json" "JSON receipt" || fails=$((fails + 1))
same "$tmp/old.v" "$tmp/new.v" "per-mutant verdict list" || fails=$((fails + 1))
same "$tmp/old.err.n" "$tmp/new.err.n" "stderr lines" || fails=$((fails + 1))

# Negative checks: the comparison must be able to fail.
neg=0
{ cat "$tmp/old.v"; echo 'M00 KILLED'; } > "$tmp/neg.v"
same "$tmp/old.v" "$tmp/neg.v" "neg-verdict" >/dev/null 2>&1 && { echo "FAIL negative: altered verdict not detected"; neg=1; }
{ cat "$tmp/old.json"; echo x; } > "$tmp/neg.json"
same "$tmp/old.json" "$tmp/neg.json" "neg-json" >/dev/null 2>&1 && { echo "FAIL negative: altered JSON not detected"; neg=1; }
echo 1 > "$tmp/neg.rc"; echo 0 > "$tmp/neg0.rc"
same "$tmp/neg.rc" "$tmp/neg0.rc" "neg-rc" >/dev/null 2>&1 && { echo "FAIL negative: altered exit code not detected"; neg=1; }
[ $neg -eq 0 ] && echo "ok   negative checks (altered verdict, JSON, exit code each reported different)"
fails=$((fails + neg))

if [ $fails -eq 0 ]; then
    echo "c4 conversion equivalence: PASS ($n mutants compared; stdout, exit code, JSON, verdicts identical)"
    exit 0
fi
echo "c4 conversion equivalence: FAIL ($fails difference(s), $n mutants compared)"
exit 1
