#!/bin/sh
# run_replay_suite.sh OUTDIR -- lane LD replay suite (host only).
# Needs REPLAY_BIN (dir with the four tools) and REPLAY_SUFFIX ("" or _asan).
# Every expectation is checked from tool output; the verdict is computed,
# never written in. Exit 0 only if every expectation held.
set -u
out=${1:?usage: run_replay_suite.sh OUTDIR}
bin=${REPLAY_BIN:?}
sfx=${REPLAY_SUFFIX:-}
V="$bin/rx_replay$sfx"
M="$bin/rxlog_mutate$sfx"
W="$bin/rx_world_replay$sfx"
T="$bin/m22_dispatch_run$sfx"
rm -rf "$out"
mkdir -p "$out/world" "$out/m22"
rec="$out/results.txt"
: > "$rec"
fails=0
checks=0

# expect NAME WANT(MATCH|DIVERGENCE) [SUBSYSTEM-PREFIX] -- cmd...
expect() {
    name=$1; want=$2; sub=$3; shift 4
    checks=$((checks + 1))
    "$@" > "$out/last.txt" 2>&1
    rc=$?
    line=$(grep -E '^(MATCH|DIVERGENCE|REFUSED|refuse|ok |TRN1_)' "$out/last.txt" | head -n 1)
    got=${line%% *}
    ok=1
    if [ "$want" = MATCH ]; then
        [ "$got" = MATCH ] && [ $rc -eq 0 ] || ok=0
    elif [ "$want" = OK ]; then
        [ $rc -eq 0 ] || ok=0
    elif [ "$want" = DETECT ]; then
        { [ "$got" = DIVERGENCE ] || [ "$got" = refuse ]; } && [ $rc -eq 1 ] || ok=0
    else
        [ "$got" = DIVERGENCE ] && [ $rc -eq 1 ] || ok=0
        if [ $ok -eq 1 ] && [ -n "$sub" ]; then
            case "$line" in *"subsystem=$sub"*) ;; *) ok=0 ;; esac
        fi
    fi
    if [ $ok -eq 1 ]; then st=ok; else st=FAIL; fails=$((fails + 1)); fi
    printf '%-4s %-34s want=%-10s %s\n' "$st" "$name" "$want" "${line:-<no verdict, rc=$rc>}" >> "$rec"
    [ $ok -eq 1 ] || { echo "FAIL $name: wanted $want ${sub:+($sub)}"; cat "$out/last.txt"; }
}

run() { "$@" > "$out/last.txt" 2>&1 || { echo "FAIL setup: $*"; cat "$out/last.txt"; fails=$((fails + 1)); }; }

echo "== World: record, verify, replay from the log =="
ref="$out/world/ref.rxlog"
run "$W" record "$ref" 1 24
expect world-verify-recorded MATCH "" -- "$V" verify "$ref"
run "$W" record "$out/world/ref2.rxlog" 1 24
expect world-record-twice MATCH "" -- "$V" compare "$ref" "$out/world/ref2.rxlog"
run "$W" replay "$ref" "$out/world/replay1.rxlog" 1
expect world-replay-1-worker MATCH "" -- "$V" compare "$ref" "$out/world/replay1.rxlog"
if cmp -s "$ref" "$out/world/replay1.rxlog"; then raw=identical; else raw=different; fi
echo "raw-bytes ref vs replay: $raw (clocks differ; the compared digest must not)" >> "$rec"

echo "== World: schedule independence (4 workers, 5 replays) =="
sched_match=0
for i in 1 2 3 4 5; do
    run "$W" replay "$ref" "$out/world/replay4-$i.rxlog" 4
    if "$V" compare "$ref" "$out/world/replay4-$i.rxlog" > "$out/world/replay4-$i.txt" 2>&1; then
        sched_match=$((sched_match + 1))
    fi
done
echo "schedule: 4-worker replays matching the 1-worker recording: $sched_match/5" >> "$rec"
checks=$((checks + 1))
if [ $sched_match -eq 5 ]; then
    echo "ok   world-replay-4-workers              5/5 MATCH" >> "$rec"
else
    fails=$((fails + 1))
    echo "FAIL world-replay-4-workers              $sched_match/5 MATCH" >> "$rec"
    head -n 2 "$out"/world/replay4-*.txt
fi

echo "== World: negative controls (replay that really differs) =="
run "$W" replay "$ref" "$out/world/neg-value.rxlog" 1 value
expect control-value-only DIVERGENCE world.state -- "$V" compare "$ref" "$out/world/neg-value.rxlog"
run "$W" replay "$ref" "$out/world/neg-structure.rxlog" 1 structure
expect control-structure DIVERGENCE world.crumb -- "$V" compare "$ref" "$out/world/neg-structure.rxlog"
run "$W" replay "$ref" "$out/world/neg-input.rxlog" 1 input
expect control-input DIVERGENCE world.input -- "$V" compare "$ref" "$out/world/neg-input.rxlog"

echo "== World: mutation suite =="
wk=0; ws=0; wx=0
for m in $("$M" list-world); do
    f="$out/world/mut-$m.rxlog"
    run "$M" world "$ref" "$f" "$m"
    "$V" verify "$f" > "$out/world/mut-$m.verify.txt" 2>&1
    sv=$(head -n 1 "$out/world/mut-$m.verify.txt" | cut -d' ' -f1)
    case $m in
    excluded-*)
        wx=$((wx + 1))
        expect "mutant:$m (verify=$sv)" MATCH "" -- "$V" compare "$ref" "$f" ;;
    *)
        before=$fails
        expect "mutant:$m (verify=$sv)" DIVERGENCE "" -- "$V" compare "$ref" "$f"
        if [ $fails -eq $before ]; then wk=$((wk + 1)); else ws=$((ws + 1)); fi ;;
    esac
done
echo "world mutants: killed=$wk survived=$ws excluded-field controls=$wx" >> "$rec"

echo "== M22 dispatch.log =="
run "$T" "$out/m22/a" 12
run "$T" "$out/m22/b" 12
run "$T" "$out/m22/lr" 12 12345 lr
expect m22-verify MATCH "" -- "$V" verify-dispatch "$out/m22/a"
expect m22-replay-run MATCH "" -- "$V" compare-dispatch "$out/m22/a" "$out/m22/b"
expect control-m22-lr DIVERGENCE m22.dispatch -- "$V" compare-dispatch "$out/m22/a" "$out/m22/lr"
dk=0; ds=0
for m in $("$M" list-dispatch); do
    d="$out/m22/mut-$m"
    rm -rf "$d"; cp -r "$out/m22/a" "$d"
    run "$M" dispatch "$d" "$m"
    "$V" verify-dispatch "$d" > "$d.verify.txt" 2>&1
    sv=$(head -n 1 "$d.verify.txt" | cut -d' ' -f1)
    before=$fails
    expect "dmutant:$m (verify=$sv)" DIVERGENCE "" -- "$V" compare-dispatch "$out/m22/a" "$d"
    if [ $fails -eq $before ]; then dk=$((dk + 1)); else ds=$((ds + 1)); fi
done
echo "dispatch mutants: killed=$dk survived=$ds" >> "$rec"

echo "== TRN1: export, verify, compare, bit flips, shared corpus =="
mkdir -p "$out/trn1"
tk=0; ts=0
for f in ref replay1 replay4-1 neg-value neg-structure neg-input; do
    run "$V" export-trn1 "$out/world/$f.rxlog" "$out/trn1/$f.trn"
done
expect trn1-verify-recorded OK "" -- "$V" verify-trn1 "$out/trn1/ref.trn"
expect trn1-replay-1-worker MATCH "" -- "$V" compare-trn1 "$out/trn1/ref.trn" "$out/trn1/replay1.trn"
expect trn1-replay-4-workers MATCH "" -- "$V" compare-trn1 "$out/trn1/ref.trn" "$out/trn1/replay4-1.trn"
expect trn1-control-value DIVERGENCE omega-world -- "$V" compare-trn1 "$out/trn1/ref.trn" "$out/trn1/neg-value.trn"
expect trn1-control-structure DIVERGENCE omega-world -- "$V" compare-trn1 "$out/trn1/ref.trn" "$out/trn1/neg-structure.trn"
expect trn1-control-input DIVERGENCE external -- "$V" compare-trn1 "$out/trn1/ref.trn" "$out/trn1/neg-input.trn"
expect trn1-every-bit-flip-refused OK "" -- "$V" trn1-flipall "$out/trn1/ref.trn"
for m in $("$M" list-world); do
    t="$out/trn1/mut-$m.trn"
    run "$V" export-trn1 "$out/world/mut-$m.rxlog" "$t"
    # Not TRN1 identity by spec 5.3 (episode is annotation) or RXCLOG container
    # fields with no TRN1 counterpart (TRN1 has its own chain and END).
    case $m in
    excluded-*|flip-episode|flip-end-head|flip-end-count) expect "trn1-mutant:$m" MATCH "" -- "$V" compare-trn1 "$out/trn1/ref.trn" "$t" ;;
    *)
        before=$fails
        expect "trn1-mutant:$m" DETECT "" -- "$V" compare-trn1 "$out/trn1/ref.trn" "$t"
        if [ $fails -eq $before ]; then tk=$((tk + 1)); else ts=$((ts + 1)); fi ;;
    esac
done
echo "trn1 world mutants: killed=$tk survived=$ts" >> "$rec"
vec=${TRN1_VECTORS:-}
if [ -n "$vec" ] && [ -f "$vec/expected.txt" ]; then
    expect trn1-shared-corpus OK "" -- "$V" trn1-corpus "$vec"
    corpus=$(grep '^TRN1_CONFORMANCE' "$out/last.txt")
    corpus_src="$vec"
else
    corpus="NOT_RUN (no TRN1_VECTORS directory)"
    corpus_src=none
    echo "NOT_RUN trn1-shared-corpus (set TRN1_VECTORS)" >> "$rec"
fi

commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then dirty=true; else dirty=false; fi
if [ $fails -ne 0 ]; then verdict=FAIL; elif [ "$corpus_src" = none ]; then verdict="PASS (trn1_corpus NOT_RUN)"; else verdict=PASS; fi
{
    echo "lane: LD replay suite${sfx:+ ($sfx build)}"
    echo "evidence: host (CPU only; no GPU, no QEMU, no hardware chip run)"
    echo "host: $(uname -srm)"
    echo "commit: $commit"
    echo "tree_dirty: $dirty"
    echo "checks: $checks failed: $fails"
    echo "world_mutants_killed: $wk world_mutants_survived: $ws excluded_field_controls: $wx"
    echo "dispatch_mutants_killed: $dk dispatch_mutants_survived: $ds"
    echo "trn1_world_mutants_killed: $tk trn1_world_mutants_survived: $ts"
    echo "trn1_corpus: $corpus (vectors: $corpus_src)"
    echo "verdict: $verdict"
    echo "--- results"
    cat "$rec"
} > "$out/receipt.txt"
sed -n '1,11p' "$out/receipt.txt"
[ $fails -eq 0 ]
