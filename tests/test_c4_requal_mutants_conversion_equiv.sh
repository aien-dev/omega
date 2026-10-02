#!/bin/sh
# Conversion proof: tools/c4_requal_mutants.sh (now an adapter over
# tools/mutation_runner.sh) must behave exactly like the pinned old script
# (commit e6298806f1c7ccd874d123350d639ad8bba6edc7, omega#216, the newest main
# commit that still holds the unconverted script with M03 retired and the
# rewritten M13). Host only, CPU only. PREREQUISITE: build/aienos-authority
# must exist (run 'make test-c4-requal' first); both sweeps build 15 mutants
# (M01 to M16 minus the retired M03), so this is slow. Env ONLY="M01 M05"
# narrows both sweeps to the same ids.
#
# Compared: stdout, exit code, the JSON receipt (byte for byte), and the
# per-mutant verdict list (id + KILLED/SURVIVED*/ERROR from the stderr lines).
# Normalised (volatile only): the receipt path in the stdout summary line
# (each sweep writes to its own file) and /tmp/c4mut.* or /tmp/mutrun.* scratch
# directory names.
#
# ONE more field is masked, the failing-case COUNT: the N in a KILLED note
# "N failing case(s), first: ..." becomes <n> in BOTH old and new, in the stderr
# lines and the JSON receipt, and only when N is an integer >= 1. Evidence that
# the count is not stable even for the OLD script alone: the C4 harness has 24
# SIGKILL cases (tests/runtime/rx_c4_requal.c:645 kill_sweep(24); the kill delay
# and nanosleep/kill are at :305-308) whose outcome depends on timing. Forge
# diagnostic log test-queue-logs/DEEP6-conv-c4-diag-light-072723.log lines
# 21-36: raw harness fail counts per run were 9 10 10 9 9 (old-style dir) and
# 10 10 9 12 12 (runner-style dir), old script M06 gave 10 and 10 while the
# converted one gave 11 and 8, and the sorted non-kill FAIL lines were identical
# in all 10 runs. The count is descriptive: KILLED is decided by the exit code
# and nf>0 (tools/mutation_runner.sh lines 150-155), never by the value of N.
# A 0 count, a missing count, a non-number and a changed first failing case stay
# visible differences; so does every other field (status, id, file, checks text,
# order, row count, exit code, summary line). The negatives below prove it.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 2
old=tools/_c4_requal_mutants_old.$$.sh
tmp=$(mktemp -d "${TMPDIR:-/tmp}/c4equiv.XXXXXX") || exit 2
trap 'rm -rf "$tmp" "$root/$old"' EXIT INT TERM
git show e6298806f1c7ccd874d123350d639ad8bba6edc7:tools/c4_requal_mutants.sh > "$old" || { echo "FAIL cannot extract pinned old script"; exit 2; }
chmod +x "$old"

norm() { # file -> normalised on stdout
    sed -e "s#$tmp/[A-Za-z0-9_.]*json#OUT.json#g" -e 's#/tmp/c4mut\.[A-Za-z0-9]*#TMP#g' \
        -e 's#/tmp/mutrun\.[A-Za-z0-9]*#TMP#g' "$1"
}
# stdin -> stdout: the one masked field. The count must follow ( or " or a space
# (stderr "(9 failing case(s), first: " and JSON "note": "9 failing case(s), first: "),
# start with 1-9 (so 0, an empty count and a non-number are not masked) and be
# followed by ", first: " (the KILLED-with-FAIL-lines note shape).
mask() {
    sed -e 's/\([(" ]\)[1-9][0-9]* failing case(s), first: /\1<n> failing case(s), first: /'
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
norm "$tmp/old.err" | mask > "$tmp/old.err.n"; norm "$tmp/new.err" | mask > "$tmp/new.err.n"
mask < "$tmp/old.json" > "$tmp/old.json.m"; mask < "$tmp/new.json" > "$tmp/new.json.m"

fails=0
n=$(wc -l < "$tmp/old.v")
[ "$n" -gt 0 ] || { echo "FAIL old sweep produced no verdicts"; fails=$((fails + 1)); }
same "$tmp/old.out.n" "$tmp/new.out.n" "stdout" || fails=$((fails + 1))
same "$tmp/old.rc" "$tmp/new.rc" "exit code" || fails=$((fails + 1))
same "$tmp/old.json.m" "$tmp/new.json.m" "JSON receipt (failing-case count masked)" || fails=$((fails + 1))
same "$tmp/old.v" "$tmp/new.v" "per-mutant verdict list" || fails=$((fails + 1))
same "$tmp/old.err.n" "$tmp/new.err.n" "stderr lines (failing-case count masked)" || fails=$((fails + 1))

# Negative checks: the comparison must be able to fail.
neg=0
{ cat "$tmp/old.v"; echo 'M00 KILLED'; } > "$tmp/neg.v"
same "$tmp/old.v" "$tmp/neg.v" "neg-verdict" >/dev/null 2>&1 && { echo "FAIL negative: altered verdict not detected"; neg=1; }
{ cat "$tmp/old.json"; echo x; } | mask > "$tmp/neg.json.m"
same "$tmp/old.json.m" "$tmp/neg.json.m" "neg-json" >/dev/null 2>&1 && { echo "FAIL negative: altered JSON not detected"; neg=1; }
echo 1 > "$tmp/neg.rc"; echo 0 > "$tmp/neg0.rc"
same "$tmp/neg.rc" "$tmp/neg0.rc" "neg-rc" >/dev/null 2>&1 && { echo "FAIL negative: altered exit code not detected"; neg=1; }

# The mask is narrow. Synthetic lines in the real stderr and JSON shapes (the
# sweep may not contain a counted note when ONLY narrows it). se/js print one
# line from <status> <count> <first failing case>; mdiff succeeds when the two
# lines are identical AFTER the mask.
se() { printf 'M06 %s  choose the NEWEST durable state record (%s failing case(s), first: %s)\n' "$1" "$2" "$3"; }
js() { printf '    {"id": "M06", "file": "src/runtime/rx_compose.c", "checks": "x", "status": "%s", "note": "%s failing case(s), first: %s"},\n' "$1" "$2" "$3"; }
mdiff() { printf '%s\n' "$1" | mask > "$tmp/ma"; printf '%s\n' "$2" | mask > "$tmp/mb"; cmp -s "$tmp/ma" "$tmp/mb"; }
# controls: a differing count of >= 1 IS masked (else the mask is dead), also across digit lengths
mdiff "$(se KILLED 9 A)" "$(se KILLED 8 A)" || { echo "FAIL control: stderr count 9 vs 8 not masked"; neg=1; }
mdiff "$(js KILLED 10 A)" "$(js KILLED 12 A)" || { echo "FAIL control: JSON count 10 vs 12 not masked"; neg=1; }
# (a) a changed first failing case is still different
mdiff "$(se KILLED 9 A)" "$(se KILLED 9 B)" && { echo "FAIL negative: changed first failing case (stderr) not detected"; neg=1; }
mdiff "$(js KILLED 9 A)" "$(js KILLED 8 B)" && { echo "FAIL negative: changed first failing case (JSON) not detected"; neg=1; }
# (b) 0 failing case(s), a missing count and a non-number are still different from 8
mdiff "$(se KILLED 0 A)" "$(se KILLED 8 A)" && { echo "FAIL negative: 0 vs 8 failing case(s) (stderr) not detected"; neg=1; }
mdiff "$(js KILLED 0 A)" "$(js KILLED 8 A)" && { echo "FAIL negative: 0 vs 8 failing case(s) (JSON) not detected"; neg=1; }
mdiff "$(se KILLED '' A)" "$(se KILLED 8 A)" && { echo "FAIL negative: missing count not detected"; neg=1; }
mdiff "$(se KILLED x A)" "$(se KILLED 8 A)" && { echo "FAIL negative: non-number count not detected"; neg=1; }
# (c) a changed status is still different (the verdict list negative above covers the list; these cover the masked stderr and JSON compares)
mdiff "$(se KILLED 9 A)" "$(se SURVIVED 9 A)" && { echo "FAIL negative: changed status (stderr) not detected"; neg=1; }
mdiff "$(js KILLED 9 A)" "$(js SURVIVED 9 A)" && { echo "FAIL negative: changed status (JSON) not detected"; neg=1; }
[ $neg -eq 0 ] && echo "ok   negative checks (altered verdict, JSON, exit code; mask controls; changed first case, 0 / missing / non-number count and changed status each reported different)"
fails=$((fails + neg))

if [ $fails -eq 0 ]; then
    echo "c4 conversion equivalence: PASS ($n mutants compared; stdout, exit code, JSON, verdicts identical, failing-case counts masked)"
    exit 0
fi
echo "c4 conversion equivalence: FAIL ($fails difference(s), $n mutants compared)"
exit 1
