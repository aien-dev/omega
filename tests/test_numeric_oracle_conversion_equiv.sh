#!/bin/bash
# Proof that tools/numeric_oracle_mutations.sh, now an adapter over
# tools/mutation_runner.sh, behaves exactly like the old standalone sweep from
# pinned commit 2dc9dd8: same stdout, same exit code, same per-mutant verdicts.
# Host only, CPU only. Runs both full sweeps (13 mutants each) on the same tree.
# The old script is extracted into tools/ (it cd's to dirname/..) and removed by
# a trap. Normalised, and only this: temp directory paths such as /tmp/tmp.AbC123
# (replaced by TMPDIR). No timings appear in either script's output.
# The sweep is not a JSON writer, so there is no JSON to compare.
set -u
ROOT=$(cd -P "$(dirname "$0")/.." && pwd)
PIN=2dc9dd8
OLD=$ROOT/tools/.old_numeric_oracle_mutations.$$.sh
W=$(mktemp -d)
trap 'rm -f "$OLD"; rm -rf "$W"' EXIT INT TERM
git -C "$ROOT" show "$PIN:tools/numeric_oracle_mutations.sh" > "$OLD" || { echo "FAIL: cannot extract old script from $PIN"; exit 1; }
chmod +x "$OLD"

norm() { sed -E 's#/[^ :]*/tmp\.[A-Za-z0-9]+#TMPDIR#g' "$1"; }
verdicts() { norm "$1" | sed -n -E 's/^  \[(KILLED|SURVIVED|BROKEN)\] +([A-Za-z0-9_]+).*/\2 \1/p' | sed 's/BROKEN$/ERROR/'; }
# differs <stdoutA> <rcA> <stdoutB> <rcB>: 0 if any difference in stdout, exit code or verdict list.
differs() {
    ! { diff <(norm "$1") <(norm "$3") >/dev/null && [ "$2" = "$4" ] && diff <(verdicts "$1") <(verdicts "$3") >/dev/null; }
}

bash "$OLD" > "$W/old.out" 2>&1; old_rc=$?
bash "$ROOT/tools/numeric_oracle_mutations.sh" > "$W/new.out" 2>&1; new_rc=$?

fails=0
n=$(verdicts "$W/old.out" | wc -l)
if [ "$n" -lt 1 ]; then echo "FAIL: old sweep produced no verdicts"; fails=$((fails + 1)); fi
if differs "$W/old.out" "$old_rc" "$W/new.out" "$new_rc"; then
    echo "FAIL: old and new sweeps differ"; diff <(norm "$W/old.out") <(norm "$W/new.out") | head -20
    echo "exit old=$old_rc new=$new_rc"; fails=$((fails + 1))
else
    echo "ok   stdout, exit code ($new_rc) and verdict list identical"
fi
if [ "$(verdicts "$W/old.out")" != "$(verdicts "$W/new.out")" ]; then echo "FAIL: verdict lists differ"; fails=$((fails + 1)); fi

# Negative checks: the comparison must be able to fail.
sed '0,/\[KILLED\]/s//[SURVIVED]/' "$W/new.out" > "$W/alt.out"
if differs "$W/old.out" "$old_rc" "$W/alt.out" "$new_rc"; then echo "ok   altered verdict line is reported different"
else echo "FAIL: comparison missed an altered verdict line"; fails=$((fails + 1)); fi
if differs "$W/old.out" "$old_rc" "$W/new.out" "$((new_rc + 1))"; then echo "ok   altered exit code is reported different"
else echo "FAIL: comparison missed an altered exit code"; fails=$((fails + 1)); fi

echo "mutants compared: $n"
if [ "$fails" -eq 0 ]; then echo "numeric oracle conversion equivalence: PASS ($n mutants compared)"; exit 0; fi
echo "numeric oracle conversion equivalence: FAIL"; exit 1
