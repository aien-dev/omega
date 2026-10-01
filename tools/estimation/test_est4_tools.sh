#!/bin/sh
# ESTIMATION v4 tool tests (synthetic data only; never opens D1 or D2).
#   tools/estimation/test_est4_tools.sh <est4-test-build> <workdir>
# The test build (-DE4_TEST_BUILD) takes the dirty flag, protocol SHA and the
# D1/D2/params identities from E4_TEST_* environment variables.
set -u
T=${1:?test build}
W=${2:?workdir}
pass=0; fail=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
# expect <rc> <stderr-substring> <cmd...>
expect() {
    want=$1; pat=$2; shift 2
    "$@" > "$W/out.txt" 2> "$W/err.txt"; rc=$?
    if [ "$rc" -ne "$want" ]; then bad "rc $rc (want $want): $*"; cat "$W/err.txt" >&2; return; fi
    if [ -n "$pat" ] && ! grep -q -- "$pat" "$W/err.txt" "$W/out.txt"; then bad "missing '$pat': $*"; cat "$W/err.txt" >&2; return; fi
    ok
}
rm -rf "$W"; mkdir -p "$W"
S=$W/syn-a; S2=$W/syn-b
expect 0 "" "$T" make-synthetic "$S" --seed 7 --lines 2400
expect 0 "" "$T" make-synthetic "$S2" --seed 11 --lines 2400
expect 0 "VALID" "$T" precheck --dir "$S2"

# 1. synthetic fit runs, fast and pmf scores agree for every family
expect 0 "EST4_FIT" "$T" fit --raw "$S/machine-state.ndjson" --marks "$S/machine-state-marks.txt" --out "$W/p.txt" --synthetic-test --grid-stride 3
for g in G1 G2 G3; do
    if grep -q "^family $g available 1 .* agree 1$" "$W/p.txt"; then ok; else bad "family $g not available+agreeing"; fi
done
grep -q '^phase_a ' "$W/p.txt" && ok || bad "no phase_a line"
grep -q '^e0_count ' "$W/p.txt" && ok || bad "no E0 histogram"
grep -q '^baseline F1 available 1 ' "$W/p.txt" && ok || bad "no F1 baseline"
# never overwrites
expect 1 "never overwritten" "$T" fit --raw "$S/machine-state.ndjson" --marks "$S/machine-state-marks.txt" --out "$W/p.txt" --synthetic-test --grid-stride 3

# 2. held-out guard: refused before any file is opened (paths do not exist)
for tag in est4-heldout est3c-heldout; do
    H=$W/x-$tag-silicon
    expect 2 "held-out" "$T" fit --raw "$H/machine-state.ndjson" --marks "$S/machine-state-marks.txt" --out "$W/h.txt" --synthetic-test
    expect 2 "held-out" "$T" fit --raw "$S/machine-state.ndjson" --marks "$H/machine-state-marks.txt" --out "$W/h.txt" --synthetic-test
done
# a symlink whose target carries the tag is refused too
mkdir -p "$W/r-est4-heldout-silicon"; cp "$S"/* "$W/r-est4-heldout-silicon/"; ln -s "$(cd "$W" && pwd)/r-est4-heldout-silicon" "$W/innocent"
expect 2 "held-out" "$T" fit --raw "$W/innocent/machine-state.ndjson" --marks "$W/innocent/machine-state-marks.txt" --out "$W/h2.txt" --synthetic-test

# 3. purity: a fit opens only its own inputs (strace, when available)
if command -v strace > /dev/null 2>&1; then
    strace -f -e trace=open,openat -o "$W/trace.txt" "$T" fit --raw "$S/machine-state.ndjson" --marks "$S/machine-state-marks.txt" --out "$W/p2.txt" --synthetic-test --grid-stride 5 > /dev/null 2>&1
    if grep -E 'heldout|syn-b|-est4-fit-|est3c-fit' "$W/trace.txt" > /dev/null; then bad "fit opened a foreign data path"; else ok; fi
    if grep -q 'syn-a/machine-state.ndjson' "$W/trace.txt"; then ok; else bad "strace saw no input open"; fi
    strace -f -e trace=open,openat -o "$W/trace2.txt" "$T" fit --raw "$W/innocent/machine-state.ndjson" --marks "$W/innocent/machine-state-marks.txt" --out "$W/h3.txt" --synthetic-test > /dev/null 2>&1
    if grep -E 'machine-state' "$W/trace2.txt" > /dev/null; then bad "held-out refusal opened data"; else ok; fi
else
    echo "test_est4_tools: strace absent, open-trace purity skipped" >&2
fi

# 4. binding-mode refusals
F1=$W/d-est4-fit-silicon; mkdir -p "$F1"; cp "$S"/* "$F1/"
RS=$(sha256sum "$F1/machine-state.ndjson" | cut -c1-64); MS=$(sha256sum "$F1/machine-state-marks.txt" | cut -c1-64)
PS=$(sha256sum docs/estimation/protocols/est-v4.md 2>/dev/null | cut -c1-64)
fitb() { "$T" fit --raw "$1/machine-state.ndjson" --marks "$1/machine-state-marks.txt" --out "$2"; }
expect 2 "--grid-stride only" "$T" fit --raw "$F1/machine-state.ndjson" --marks "$F1/machine-state-marks.txt" --out "$W/b.txt" --grid-stride 3
expect 2 "clean tree" env E4_TEST_DIRTY=1 "$T" fit --raw "$F1/machine-state.ndjson" --marks "$F1/machine-state-marks.txt" --out "$W/b.txt"
expect 2 "protocol SHA" env E4_TEST_DIRTY=0 E4_TEST_PROTOCOL_SHA=0000 "$T" fit --raw "$F1/machine-state.ndjson" --marks "$F1/machine-state-marks.txt" --out "$W/b.txt"
if [ -n "$PS" ]; then
    expect 2 "no D1 identity" env E4_TEST_DIRTY=0 E4_TEST_PROTOCOL_SHA="$PS" "$T" fit --raw "$F1/machine-state.ndjson" --marks "$F1/machine-state-marks.txt" --out "$W/b.txt"
    expect 2 "-est4-fit-" env E4_TEST_DIRTY=0 E4_TEST_PROTOCOL_SHA="$PS" E4_TEST_D1_RAW_SHA="$RS" E4_TEST_D1_MARKS_SHA="$MS" "$T" fit --raw "$S/machine-state.ndjson" --marks "$S/machine-state-marks.txt" --out "$W/b.txt"
    expect 2 "differs from the compiled D1" env E4_TEST_DIRTY=0 E4_TEST_PROTOCOL_SHA="$PS" E4_TEST_D1_RAW_SHA="$MS" E4_TEST_D1_MARKS_SHA="$MS" "$T" fit --raw "$F1/machine-state.ndjson" --marks "$F1/machine-state-marks.txt" --out "$W/b.txt"
    [ ! -e "$W/b.txt" ] && ok || bad "refused binding fit left an output"
else
    echo "test_est4_tools: protocol doc absent, binding identity refusals skipped" >&2
fi

# 5. recorded: refuses PHASE_A_FAIL params, a held-out dir in synthetic mode, binding without D2 identity
mkdir -p "$W/rc"
if grep -q '^phase_a PHASE_A_FAIL' "$W/p.txt"; then
    expect 2 "NOT_RUN" "$T" recorded --dir "$S2" --params "$W/p.txt" --outdir "$W/rc" --synthetic-test
fi
expect 2 "no D2 identity" env E4_TEST_DIRTY=0 "$T" recorded --dir "$S2" --params "$W/p.txt" --outdir "$W/rc"
expect 2 "never runs on D1" "$T" recorded --dir "$W/r-est4-heldout-silicon" --params "$W/p.txt" --outdir "$W/rc" --synthetic-test
# a params file marked as a G1 screen pass exercises the scored path end to end
sed -e 's/^screen_result G1 pass 0/screen_result G1 pass 1/' -e 's/^selected .*/selected G1/' -e 's/^phase_a .*/phase_a PASS/' "$W/p.txt" > "$W/pp.txt"
expect 0 "EST4_RECORDED" "$T" recorded --dir "$S2" --params "$W/pp.txt" --outdir "$W/rc" --synthetic-test
n=$(ls "$W/rc" | grep -c '^receipt-.*\.json$'); [ "$n" -eq 1 ] && ok || bad "want one receipt, have $n"
r=$(ls "$W/rc"/receipt-*.json | head -1); h=$(sha256sum "$r" | cut -c1-64)
case "$r" in *"receipt-$h.json") ok ;; *) bad "receipt name is not its SHA-256" ;; esac
grep -q '"same_steps": true' "$r" && ok || bad "S, F1, E0 not on the same steps"
expect 2 "already holds a receipt" "$T" recorded --dir "$S2" --params "$W/pp.txt" --outdir "$W/rc" --synthetic-test
# selection mismatch is refused
sed -e 's/^selected .*/selected G3/' "$W/pp.txt" > "$W/pm.txt"; mkdir -p "$W/rc2"
expect 2 "the rule gives" "$T" recorded --dir "$S2" --params "$W/pm.txt" --outdir "$W/rc2" --synthetic-test


# 6. binding recorded end to end (identities from the environment), and the
#    synthetic path refusing D2 bytes under another name
if [ -n "$PS" ]; then
    RD=$W/repo; RO=$RD/docs/estimation/receipts/est-v4; mkdir -p "$RO"
    D2=$W/z-est4-heldout-silicon; mkdir -p "$D2"; cp "$S2"/* "$D2/"
    R2=$(sha256sum "$D2/machine-state.ndjson" | cut -c1-64); M2=$(sha256sum "$D2/machine-state-marks.txt" | cut -c1-64); C2=$(sha256sum "$D2/schedule.txt" | cut -c1-64)
    mkpb() { sed -e 's/^synthetic_test .*/synthetic_test 0/' -e 's/^tool_dirty .*/tool_dirty 0/' -e "s/^protocol_doc_sha256 .*/protocol_doc_sha256 $1/" \
                 -e "s/^fit_raw_sha256 .*/fit_raw_sha256 $RS/" -e "s/^fit_marks_sha256 .*/fit_marks_sha256 $MS/" \
                 -e "s|^fit_raw_path .*|fit_raw_path $F1/machine-state.ndjson|" "$W/pp.txt" > "$2"; }
    mkpb "$PS" "$W/pb.txt"; mkpb 1111 "$W/pbx.txt"
    rec() { env E4_TEST_DIRTY=0 E4_TEST_PROTOCOL_SHA="$PS" E4_TEST_D1_RAW_SHA="$RS" E4_TEST_D1_MARKS_SHA="$MS" \
            E4_TEST_D2_RAW_SHA="$R2" E4_TEST_D2_MARKS_SHA="$M2" E4_TEST_D2_SCHED_SHA="$C2" \
            E4_TEST_PARAMS_SHA="$(sha256sum "$1" | cut -c1-64)" E4_TEST_REPO_ROOT="$RD" \
            "$T" recorded --dir "$D2" --params "$1" --outdir "$RO"; }
    expect 2 "another protocol SHA" rec "$W/pbx.txt"
    expect 2 "receipt goes only" env E4_TEST_DIRTY=0 E4_TEST_PROTOCOL_SHA="$PS" E4_TEST_D2_RAW_SHA="$R2" E4_TEST_D2_MARKS_SHA="$M2" E4_TEST_D2_SCHED_SHA="$C2" \
        E4_TEST_PARAMS_SHA="$(sha256sum "$W/pb.txt" | cut -c1-64)" E4_TEST_REPO_ROOT="$RD" "$T" recorded --dir "$D2" --params "$W/pb.txt" --outdir "$W/rc2"
    expect 0 "EST4_RECORDED" rec "$W/pb.txt"
    n=$(ls "$RO" | grep -c '^receipt-.*\.json$'); [ "$n" -eq 1 ] && ok || bad "binding: want one receipt, have $n"
    grep -q "\"protocol_sha256\": \"$PS\"" "$RO"/receipt-*.json && ok || bad "binding receipt lacks the protocol SHA"
    expect 2 "already holds a receipt" rec "$W/pb.txt"
    mkdir -p "$W/rc3"
    expect 2 "(D2 bytes)" env E4_TEST_D2_RAW_SHA="$R2" "$T" recorded --dir "$S2" --params "$W/pp.txt" --outdir "$W/rc3" --synthetic-test
fi
echo "test_est4_tools: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
