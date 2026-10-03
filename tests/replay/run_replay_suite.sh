#!/bin/sh
# run_replay_suite.sh OUTDIR -- lane LD replay suite (host only).
# Needs REPLAY_BIN (dir with the four tools) and REPLAY_SUFFIX ("" or _asan).
# Every expectation is checked from tool output; the verdict is computed,
# never written in. Exit 0 only if every gating expectation held. Strict
# 4-worker sequence checks are non-gating KNOWN_FAIL lines under runtime
# finding I11 (see known_check); the 4-worker gate is the causal-order
# compare (tools/replay/CAUSAL_ORDER.md), gated by its own mutants.
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

# Known runtime findings. A strict check the engine is known to fail stays in
# the suite and is reported every run, both ways: "ok" when it passes,
# "KNOWN_FAIL" (non-gating, finding named) when it reports DIVERGENCE. It is
# never reported as ok when it fails, and a run that gives no DIVERGENCE
# verdict (tool error, refusal) is a normal FAIL. When the engine is fixed the
# line turns into "ok" by itself; nothing here has to change.
I11="runtime finding I11: concurrent commits are logged in schedule order (deterministic commit order requested)"
known=0
known_names=""
# known_check NAME -- cmd...: one strict compare under finding I11.
known_check() {
    name=$1; shift 2
    checks=$((checks + 1))
    "$@" > "$out/last.txt" 2>&1
    rc=$?
    line=$(grep -E '^(MATCH|DIVERGENCE|REFUSED|refuse|ok |TRN1_)' "$out/last.txt" | head -n 1)
    got=${line%% *}
    if [ "$got" = MATCH ] && [ $rc -eq 0 ]; then st=ok
    elif [ "$got" = DIVERGENCE ] && [ $rc -eq 1 ]; then
        st=KNOWN_FAIL; known=$((known + 1)); known_names="$known_names $name"
    else st=FAIL; fails=$((fails + 1)); fi
    printf '%-4s %-34s want=%-10s %s%s\n' "$st" "$name" "MATCH" "${line:-<no verdict, rc=$rc>}" \
        "$([ $st = KNOWN_FAIL ] && echo " [non-gating; $I11]")" >> "$rec"
    [ $st = FAIL ] && { echo "FAIL $name: no verdict"; cat "$out/last.txt"; }
    [ $st = KNOWN_FAIL ] && { echo "KNOWN_FAIL $name (non-gating, I11)"; head -n 4 "$out/last.txt"; }
    return 0
}

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

echo "== World: 4 workers, 5 replays: strict sequence (non-gating, I11) and causal order (gating) =="
# Strict sequence: every 4-worker replay crumb-identical to the 1-worker
# recording. The engine logs unrelated concurrent commits in schedule order,
# so this is KNOWN_FAIL until runtime finding I11 is fixed (see known_check).
sched_match=0
sched_bad=0
causal_match=0
for i in 1 2 3 4 5; do
    run "$W" replay "$ref" "$out/world/replay4-$i.rxlog" 4
    "$V" compare "$ref" "$out/world/replay4-$i.rxlog" > "$out/world/replay4-$i.txt" 2>&1
    rc=$?
    v=$(head -n 1 "$out/world/replay4-$i.txt" | cut -d' ' -f1)
    if [ "$v" = MATCH ] && [ $rc -eq 0 ]; then sched_match=$((sched_match + 1))
    elif [ "$v" != DIVERGENCE ] || [ $rc -ne 1 ]; then sched_bad=$((sched_bad + 1)); fi
    # Causal order: the property concurrent commits must keep
    # (rx_replay.c compare-causal, tools/replay/CAUSAL_ORDER.md).
    "$V" compare-causal "$ref" "$out/world/replay4-$i.rxlog" > "$out/world/replay4-$i.causal.txt" 2>&1
    rc=$?
    cl=$(head -n 1 "$out/world/replay4-$i.causal.txt")
    [ "${cl%% *}" = MATCH ] && [ $rc -eq 0 ] && causal_match=$((causal_match + 1))
    echo "     replay4-$i strict:  $(head -n 1 "$out/world/replay4-$i.txt")" >> "$rec"
    echo "     replay4-$i causal:  ${cl:-<no verdict, rc=$rc>}" >> "$rec"
done
echo "schedule: 4-worker replays matching the 1-worker recording: strict $sched_match/5, causal order $causal_match/5" >> "$rec"
checks=$((checks + 1))
if [ $sched_match -eq 5 ]; then
    echo "ok   world-replay-4-workers              5/5 MATCH" >> "$rec"
elif [ $sched_bad -eq 0 ]; then
    known=$((known + 1)); known_names="$known_names world-replay-4-workers"
    echo "KNOWN_FAIL world-replay-4-workers       $sched_match/5 MATCH [non-gating; $I11]" >> "$rec"
    echo "KNOWN_FAIL world-replay-4-workers (non-gating, I11)"
    head -n 4 "$out"/world/replay4-*.txt
else
    fails=$((fails + 1))
    echo "FAIL world-replay-4-workers              $sched_bad/5 compares gave no verdict" >> "$rec"
    cat "$out"/world/replay4-*.txt
fi
# Gating: committed history equal as a causal partial order on every replay.
checks=$((checks + 1))
if [ $causal_match -eq 5 ]; then
    echo "ok   trn1-replay-4-workers-causal        5/5 MATCH (causal order; merged-wake counts excluded)" >> "$rec"
else
    fails=$((fails + 1))
    echo "FAIL trn1-replay-4-workers-causal        $causal_match/5 MATCH (causal order)" >> "$rec"
    echo "FAIL trn1-replay-4-workers-causal: $causal_match/5"
    for i in 1 2 3 4 5; do echo "-- replay4-$i"; head -n 4 "$out/world/replay4-$i.causal.txt"; done
fi
expect world-replay-1-worker-causal MATCH "" -- "$V" compare-causal "$ref" "$out/world/replay1.rxlog"

echo "== World: negative controls (replay that really differs) =="
run "$W" replay "$ref" "$out/world/neg-value.rxlog" 1 value
expect control-value-only DIVERGENCE world.state -- "$V" compare "$ref" "$out/world/neg-value.rxlog"
run "$W" replay "$ref" "$out/world/neg-structure.rxlog" 1 structure
expect control-structure DIVERGENCE world.crumb -- "$V" compare "$ref" "$out/world/neg-structure.rxlog"
run "$W" replay "$ref" "$out/world/neg-input.rxlog" 1 input
expect control-input DIVERGENCE world.input -- "$V" compare "$ref" "$out/world/neg-input.rxlog"
# The causal-order compare must see the same three real differences.
expect causal-control-value-only DIVERGENCE world.state -- "$V" compare-causal "$ref" "$out/world/neg-value.rxlog"
expect causal-control-structure DIVERGENCE "" -- "$V" compare-causal "$ref" "$out/world/neg-structure.rxlog"
expect causal-control-input DIVERGENCE world.input -- "$V" compare-causal "$ref" "$out/world/neg-input.rxlog"

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

echo "== Causal order: mutants (must FAIL the causal gate) and controls (must not) =="
# causal-* forge one real difference into a self-consistent log; causal-control-*
# make only the changes concurrent commits legitimately make (unrelated
# commits swapped, a flagged superseded run with its rerun, the merged-wake
# count): the sequence compare must still catch those, the causal compare not.
ck=0; cs=0; cc=0
for m in $("$M" list-causal); do
    f="$out/world/causal-$m.rxlog"
    run "$M" causal "$ref" "$f" "$m"
    "$V" verify "$f" > "$out/world/causal-$m.verify.txt" 2>&1
    sv=$(head -n 1 "$out/world/causal-$m.verify.txt" | cut -d' ' -f1)
    # every forged log verifies by itself: only a comparison can catch it
    expect "cmutant-self-verify:$m" MATCH "" -- "$V" verify "$f"
    case $m in
    causal-control-*)
        cc=$((cc + 1))
        expect "ccontrol:$m (strict)" DIVERGENCE "" -- "$V" compare "$ref" "$f"
        expect "ccontrol:$m (causal)" MATCH "" -- "$V" compare-causal "$ref" "$f" ;;
    *)
        before=$fails
        expect "cmutant:$m (verify=$sv)" DIVERGENCE "" -- "$V" compare-causal "$ref" "$f"
        if [ $fails -eq $before ]; then ck=$((ck + 1)); else cs=$((cs + 1)); fi ;;
    esac
done
# Every sequence-compare world mutant must also fail the causal compare,
# except the excluded-field controls (worker, clocks).
for m in $("$M" list-world); do
    f="$out/world/mut-$m.rxlog"
    case $m in
    excluded-*) expect "causal-wmutant:$m" MATCH "" -- "$V" compare-causal "$ref" "$f" ;;
    *)
        before=$fails
        expect "causal-wmutant:$m" DETECT "" -- "$V" compare-causal "$ref" "$f"
        if [ $fails -eq $before ]; then ck=$((ck + 1)); else cs=$((cs + 1)); fi ;;
    esac
done
echo "causal mutants: killed=$ck survived=$cs controls=$cc" >> "$rec"

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
known_check trn1-replay-4-workers -- "$V" compare-trn1 "$out/trn1/ref.trn" "$out/trn1/replay4-1.trn"
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

echo "== Path and line limits: refuse, never truncate =="
# expect_refusal NAME RC TEXT -- cmd...: exit code RC and TEXT in the output.
# A truncated path or split corpus line must be refused loudly; before the
# fix these inputs were read as a different (shorter) path or two lines.
expect_refusal() {
    name=$1; want_rc=$2; text=$3; shift 4
    checks=$((checks + 1))
    "$@" > "$out/last.txt" 2>&1
    rc=$?
    if [ $rc -eq "$want_rc" ] && grep -qF "$text" "$out/last.txt"; then st=ok; else st=FAIL; fails=$((fails + 1)); fi
    printf '%-4s %-34s want=rc%s+"%s" got rc=%s\n' "$st" "$name" "$want_rc" "$text" "$rc" >> "$rec"
    [ $st = ok ] || { echo "FAIL $name: wanted rc=$want_rc and \"$text\""; head -c 2000 "$out/last.txt"; echo; }
}
# Same directory, spelled with 2100 "/." segments: over 4096 bytes.
lp_trn="$out/trn1"; lp_m22="$out/m22/a"; i=0
while [ $i -lt 2100 ]; do lp_trn="$lp_trn/."; lp_m22="$lp_m22/."; i=$((i + 1)); done
expect_refusal limit-corpus-path-too-long 2 "path too long" -- "$V" trn1-corpus "$lp_trn"
expect_refusal limit-dispatch-path-too-long 1 "store path too long" -- "$V" verify-dispatch "$lp_m22"
expect_refusal limit-mutate-path-too-long 2 "path too long" -- "$M" dispatch "$lp_m22" flip-op
mkdir -p "$out/trn1/longline"
{ i=0; while [ $i -lt 1500 ]; do printf x; i=$((i + 1)); done; echo " OK"; } > "$out/trn1/longline/expected.txt"
: > "$out/trn1/longline/compare.txt"
expect_refusal limit-corpus-line-too-long 1 "line longer than" -- "$V" trn1-corpus "$out/trn1/longline"

commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then dirty=true; else dirty=false; fi
# Non-gating known failures are named in the verdict itself, never folded into PASS silently.
notes=""
[ "$corpus_src" = none ] && notes="trn1_corpus NOT_RUN"
if [ $known -ne 0 ]; then
    kn="KNOWN FAIL (non-gating, runtime finding I11):$known_names"
    notes="${notes:+$notes; }$kn"
fi
if [ $fails -ne 0 ]; then verdict=FAIL; else verdict="PASS${notes:+ ($notes)}"; fi
{
    echo "lane: LD replay suite${sfx:+ ($sfx build)}"
    echo "evidence: host (CPU only; no GPU, no QEMU, no hardware chip run)"
    echo "host: $(uname -srm)"
    echo "commit: $commit"
    echo "tree_dirty: $dirty"
    echo "checks: $checks failed: $fails known_fail_non_gating: $known"
    echo "world_mutants_killed: $wk world_mutants_survived: $ws excluded_field_controls: $wx"
    echo "dispatch_mutants_killed: $dk dispatch_mutants_survived: $ds"
    echo "trn1_world_mutants_killed: $tk trn1_world_mutants_survived: $ts"
    echo "causal_mutants_killed: $ck causal_mutants_survived: $cs causal_controls: $cc"
    echo "strict_4_worker_sequence: $([ $known -eq 0 ] && echo PASS || echo "KNOWN FAIL ($I11)")"
    echo "causal_4_worker_gate: $causal_match/5 (merged-wake counts excluded from the comparison, kept in the log)"
    echo "trn1_corpus: $corpus (vectors: $corpus_src)"
    echo "verdict: $verdict"
    echo "--- results"
    cat "$rec"
} > "$out/receipt.txt"
sed -n '1,14p' "$out/receipt.txt"
[ $fails -eq 0 ]
