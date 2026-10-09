#!/bin/sh
# AT-0 Agent 4 qualification runner. Builds the evaluator, runs its own
# red-before-green self-tests, the public corpus, the isolation-gate controls,
# and (when a candidate is supplied) the candidate conformance and verification
# tests. Writes results/qualification.json (machine-readable) and prints a table.
#
#   sh run.sh [--self-only]
# Candidate hooks (environment; each unset hook leaves its tests BLOCKED_NO_CANDIDATE):
#   AT0_CANDIDATE_CASE_TOOL   command that takes a case path and prints AT0_CASE_OK <id>
#                             (exit 0) or AT0_CASE_REFUSED <code> on stderr (exit 2)
#   AT0_CANDIDATE_RUN         command that takes a case path and writes an
#                             AT0_RESULT file to standard output (engine + oracle run)
#   AT0_CANDIDATE_OBJECTS     space-separated compute objects/binaries for the isolation gate
#   AT0_HIDDEN_DIR            directory holding the hidden case files (default ~/at0-private/agent4)
# Statuses: PASS FAIL INCONCLUSIVE NOT_RUN BLOCKED_NO_CANDIDATE. UNISOLATED is a
# property of the whole run (ISOLATION_REPORT.md), never a test status.
set -u
cd "$(dirname "$0")"
T=build/at0-eval
OUT=results
TMP=${TMPDIR:-/tmp}/at0e-run-$$
mkdir -p "$OUT" "$TMP"
trap 'rm -rf "$TMP"' EXIT
JSONL="$TMP/tests.jsonl"; : > "$JSONL"
npass=0; nfail=0; ninc=0; nblk=0; nnr=0
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\n\t' '  ' | cut -c1-400; }
rec() { # id class status reason
    case "$3" in PASS) npass=$((npass+1));; FAIL) nfail=$((nfail+1));; INCONCLUSIVE) ninc=$((ninc+1));; BLOCKED_NO_CANDIDATE) nblk=$((nblk+1));; *) nnr=$((nnr+1));; esac
    printf '{"id":"%s","class":"%s","status":"%s","reason":"%s"}\n' "$1" "$2" "$3" "$(esc "$4")" >> "$JSONL"
    printf '%-44s %-8s %-20s %s\n' "$1" "$2" "$3" "$(printf '%s' "$4" | cut -c1-70)"
}
SELF_ONLY=0; [ "${1:-}" = "--self-only" ] && SELF_ONLY=1

# ---- build ----
if make -s all >"$TMP/build.log" 2>&1; then rec SELF-BUILD build PASS "cc -std=c11 -Wall -Wextra -Werror -pedantic"; else rec SELF-BUILD build FAIL "$(tail -1 "$TMP/build.log")"; cat "$TMP/build.log"; exit 1; fi
if make -s asan >"$TMP/asan.log" 2>&1; then rec SELF-BUILD-ASAN build PASS "ASan/UBSan build"; else rec SELF-BUILD-ASAN build INCONCLUSIVE "$(tail -1 "$TMP/asan.log")"; fi

# ---- known-answer test (AT0_CASE_V1 section 6, AT0_FREEZE.md) ----
K=cases/positive/P1-kat-ideal-qubit-n4.case
fsha=$(sha256sum "$K" | cut -c1-64)
kat=$($T case "$K" --detail 2>&1 | tr '\n' ' ')
if [ "$fsha" = ed16c95c89bf312b0fbf95a4cd43bc352daa138954fabad78d8a170cd35b4e81 ] \
   && printf '%s' "$kat" | grep -q 'AT0_CASE_OK 3cf4ca4f882b5b9691ddcd905e65e15010855adcd2be200e19ebc3184655b44d' \
   && printf '%s' "$kat" | grep -q 'acceptance_id a13fb02dd674b8042ef8c0a197f03d58768709f782ba59cd26545eef6d41a228'; then
    rec SELF-KAT-DIGESTS codec PASS "file, case_id and acceptance_id equal the published digests"
else rec SELF-KAT-DIGESTS codec FAIL "$kat"; fi
if $T gen name=at0-kat-ideal-qubit-n4 | cmp -s - "$K"; then rec SELF-GEN-ROUNDTRIP codec PASS "generator reproduces the example byte for byte"; else rec SELF-GEN-ROUNDTRIP codec FAIL "byte difference"; fi
sh_out=$($T shadow "$K")
if printf '%s\n' "$sh_out" | grep -q '^t0 *0.250000000 1.000000000 0.500000000 0.500000000' \
   && printf '%s\n' "$sh_out" | grep -q '^t1 *0.250000000 0.500000000 1.000000000 0.500000000' \
   && printf '%s\n' "$sh_out" | grep -q '^t2 *0.250000000 0.000000000 0.500000000 0.500000000' \
   && printf '%s\n' "$sh_out" | grep -q '^t3 *0.250000000 0.500000000 0.000000000 0.500000000' \
   && printf '%s\n' "$sh_out" | grep -q 'kernel_dim 2 .* povm_exact_identity 1'; then
    rec SELF-SHADOW-HAND-TABLE shadow PASS "shadow oracle reproduces the section 6 hand table"
else rec SELF-SHADOW-HAND-TABLE shadow FAIL "table differs"; fi

# ---- public corpus: every case gives its manifest response ----
mm=0; mt=0
while IFS="$(printf '\t')" read -r path cls want; do
    case "$path" in '#'*|'') continue;; esac
    mt=$((mt+1))
    got=$($T case "cases/$path" 2>&1 | head -1 | sed 's/ [0-9a-f]\{64\}$//')
    [ "$got" = "$want" ] || { mm=$((mm+1)); rec "CORPUS-$(basename "$path" .case)" "$cls" FAIL "want [$want] got [$got]"; }
done < cases/MANIFEST.tsv
[ $mm = 0 ] && rec SELF-CORPUS-MANIFEST corpus PASS "$mt cases give their expected response" || rec SELF-CORPUS-MANIFEST corpus FAIL "$mm of $mt differ"

# ---- hand derivations agree with shadow + exact checker (honest fixture -> expectation_met YES) ----
bad=0; n=0
for f in cases/positive/*.case cases/negative/*.case; do
    n=$((n+1)); $T synth "$f" > "$TMP/h.result" 2>/dev/null || { bad=$((bad+1)); rec "DERIV-$(basename "$f" .case)" derivation FAIL "synth failed"; continue; }
    em=$(grep '^expectation_met' "$TMP/h.result" | awk '{print $2}')
    v=$($T result "$TMP/h.result" --case "$f" | head -1 | awk '{print $2}')
    [ "$em" = YES ] && [ "$v" = PASS ] || { bad=$((bad+1)); rec "DERIV-$(basename "$f" .case)" derivation FAIL "expectation_met=$em verify=$v codes=$(grep '^failure_codes' "$TMP/h.result" | cut -d' ' -f2)"; }
done
[ $bad = 0 ] && rec SELF-DERIVATIONS derivation PASS "$n positive/negative cases: expected codes equal the derived codes" || rec SELF-DERIVATIONS derivation FAIL "$bad of $n disagree"

# ---- red before green: every planted defect must be caught on a case where it bites ----
# case:mutant pairs; see FAILURE_TAXONOMY.md section 3 for why each pair discriminates
MATRIX="P1-kat-ideal-qubit-n4:axis_swap P1-kat-ideal-qubit-n4:y_sign_swap P1-kat-ideal-qubit-n4:conjugate_bug P1-kat-ideal-qubit-n4:reversed_reference
P1-kat-ideal-qubit-n4:dephased_state P1-kat-ideal-qubit-n4:kernel_dim_wrong P1-kat-ideal-qubit-n4:residual_over_bound P1-kat-ideal-qubit-n4:nonfinite_hidden
P1-kat-ideal-qubit-n4:evidence_corrupt P1-kat-ideal-qubit-n4:verdict_id_corrupt P1-kat-ideal-qubit-n4:case_id_altered P1-kat-ideal-qubit-n4:tolerance_altered
P1-kat-ideal-qubit-n4:binding_rebound P1-kat-ideal-qubit-n4:times_reversed P1-kat-ideal-qubit-n4:oracle_is_engine P1-kat-ideal-qubit-n4:bound_none_nonzero
P1-kat-ideal-qubit-n4:label_status_wrong P1-kat-ideal-qubit-n4:placeholder_with_values P1-kat-ideal-qubit-n4:crlf P1-kat-ideal-qubit-n4:trailing_space
P1e-x-axis-rotation:conjugate_bug P1e-x-axis-rotation:y_sign_swap P1e-x-axis-rotation:axis_swap P1g-complex-psi-yplus:conjugate_bug P2-tilted-h0:axis_swap P5-large-rationals:conjugate_bug P5-large-rationals:axis_swap P5-large-rationals:hardcoded_table P1e-x-axis-rotation:reversed_reference P1g-complex-psi-yplus:reversed_reference
P1b-ideal-n4-tau8-m8:hardcoded_table P1d-ref-offset-t2:hardcoded_table P1c-ideal-n6-m6:hardcoded_table N4-wrong-weight-2:wrong_weight_hidden N4b-wrong-weight-5:wrong_weight_hidden
N2-half-covered:verdict_forged N3-broken-clock-tau3:verdict_forged N1-uncovered-spectrum:verdict_forged N6-zero-kernel-component:verdict_forged N7-unreachable-labels:verdict_forged N7-unreachable-labels:label_claimed_defined"
esc_n=0; tot=0
for pair in $MATRIX; do
    c=${pair%%:*}; m=${pair##*:}; f=$(ls cases/*/"$c".case); tot=$((tot+1))
    $T synth "$f" "$m" > "$TMP/m.result" 2>/dev/null || { esc_n=$((esc_n+1)); rec "MUTANT-$c-$m" mutant FAIL "synth failed"; continue; }
    st=$($T result "$TMP/m.result" --case "$f" | head -1 | awk '{print $2}')
    [ "$st" = FAIL ] || { esc_n=$((esc_n+1)); rec "MUTANT-$c-$m" mutant FAIL "defect not graded FAIL (status ${st:-none})"; }
done
# documented INCONCLUSIVE pairs (the defect is invisible on that case by construction)
for pair in P1-kat-ideal-qubit-n4:hardcoded_table P1-kat-ideal-qubit-n4:wrong_weight_hidden P1-kat-ideal-qubit-n4:verdict_forged P1f-y-axis-rotation:conjugate_bug P2b-h0-shift-control:hardcoded_table P2-tilted-h0:reversed_reference N7-unreachable-labels:label_status_wrong P1-kat-ideal-qubit-n4:label_claimed_defined; do
    c=${pair%%:*}; m=${pair##*:}; f=$(ls cases/*/"$c".case)
    $T synth "$f" "$m" > "$TMP/m.result" 2>/dev/null; st=$($T result "$TMP/m.result" --case "$f" | head -1 | awk '{print $2}')
    rec "MUTANT-BLIND-$c-$m" mutant INCONCLUSIVE "defect indistinguishable from truth on this case by construction (status $st); covered by another pair"
done
[ $esc_n = 0 ] && rec SELF-MUTANTS mutant PASS "$tot discriminating case/mutant pairs all caught" || rec SELF-MUTANTS mutant FAIL "$esc_n of $tot escaped"
# dirty-tree and unknown contract commit are INCONCLUSIVE by design
for m in dirty_tree contract_commit_wrong; do
    $T synth "$K" "$m" > "$TMP/m.result"; st=$($T result "$TMP/m.result" --case "$K" | head -1 | awk '{print $2}')
    [ "$st" = INCONCLUSIVE ] && rec "MUTANT-$m" mutant PASS "graded INCONCLUSIVE as designed (not citable)" || rec "MUTANT-$m" mutant FAIL "graded $st, expected INCONCLUSIVE"
done
# sanitizer run of the same paths
if [ -x build/at0-eval-asan ]; then
    ok=1
    build/at0-eval-asan case "$K" >/dev/null 2>"$TMP/a.err" || ok=0
    for m in evidence_corrupt crlf binding_rebound hardcoded_table; do build/at0-eval-asan synth "$K" "$m" > "$TMP/m.result" 2>>"$TMP/a.err" || ok=0; build/at0-eval-asan result "$TMP/m.result" --case "$K" >/dev/null 2>>"$TMP/a.err"; [ $? -gt 3 ] && ok=0; done
    for f in cases/refuse/*.case; do build/at0-eval-asan case "$f" >/dev/null 2>>"$TMP/a.err"; [ $? -gt 2 ] && ok=0; done
    grep -q 'ERROR: \(AddressSanitizer\|UndefinedBehaviorSanitizer\)\|runtime error' "$TMP/a.err" && ok=0
    [ $ok = 1 ] && rec SELF-ASAN sanitizer PASS "codec, refusals, synth and verify clean under ASan/UBSan" || rec SELF-ASAN sanitizer FAIL "$(grep -m1 'ERROR\|runtime error' "$TMP/a.err")"
else rec SELF-ASAN sanitizer NOT_RUN "no sanitizer binary"; fi

# ---- isolation gate controls ----
gate_build=1
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -c gates/hidden_clock_mutant.c -o build/hidden_clock_mutant.o 2>"$TMP/g.err" || gate_build=0
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pedantic -O2 -c gates/clean_control.c -o build/clean_control.o 2>>"$TMP/g.err" || gate_build=0
if [ $gate_build = 0 ]; then rec GATE-ISOLATION-MUTANT-CAUGHT gate NOT_RUN "control object failed to compile: $(head -1 "$TMP/g.err")"; rec GATE-ISOLATION-CLEAN-PASSES gate NOT_RUN "control object failed to compile"; else
if sh gates/isolation.sh build/hidden_clock_mutant.o >/dev/null 2>&1; then rec GATE-ISOLATION-MUTANT-CAUGHT gate FAIL "hidden clock_gettime mutant passed the gate"; else rec GATE-ISOLATION-MUTANT-CAUGHT gate PASS "hidden-clock mutant rejected"; fi
if sh gates/isolation.sh build/clean_control.o >/dev/null 2>&1; then rec GATE-ISOLATION-CLEAN-PASSES gate PASS "clean arithmetic object accepted"; else rec GATE-ISOLATION-CLEAN-PASSES gate FAIL "clean object rejected"; fi
fi

# ---- hidden set commitment ----
HD=${AT0_HIDDEN_DIR:-$HOME/at0-private/agent4}
if [ -d "$HD" ]; then
    if (cd "$HD" && grep -v '^#' "$OLDPWD/HIDDEN_COMMITMENT.txt" | sha256sum -c --quiet - >/dev/null 2>&1); then rec HIDDEN-COMMITMENT hidden PASS "private files match the committed digests (identity only, not non-access)"; else rec HIDDEN-COMMITMENT hidden FAIL "digest mismatch or missing private file"; fi
else rec HIDDEN-COMMITMENT hidden INCONCLUSIVE "private set not present on this host"; fi

# ---- candidate ----
if [ $SELF_ONLY = 1 ]; then :; else
if [ -n "${AT0_CANDIDATE_CASE_TOOL:-}" ]; then
    mm=0; mt=0
    while IFS="$(printf '\t')" read -r path cls want; do
        case "$path" in '#'*|'') continue;; esac
        mt=$((mt+1)); got=$($AT0_CANDIDATE_CASE_TOOL "cases/$path" 2>&1 | head -1 | sed 's/ [0-9a-f]\{64\}$//')
        [ "$got" = "$want" ] || { mm=$((mm+1)); rec "CAND-CODEC-$(basename "$path" .case)" "$cls" FAIL "want [$want] got [$got]"; }
    done < cases/MANIFEST.tsv
    [ $mm = 0 ] && rec CAND-CODEC-CONFORMANCE candidate PASS "$mt public cases answered exactly as the contract requires" || rec CAND-CODEC-CONFORMANCE candidate FAIL "$mm of $mt differ"
    if [ -d "$HD" ]; then hm=0; for f in "$HD"/H*.case; do got=$($AT0_CANDIDATE_CASE_TOOL "$f" 2>&1 | head -1 | cut -d' ' -f1); [ "$got" = AT0_CASE_OK ] || hm=$((hm+1)); done; [ $hm = 0 ] && rec CAND-CODEC-HIDDEN candidate PASS "hidden cases accepted" || rec CAND-CODEC-HIDDEN candidate FAIL "$hm hidden cases refused"; fi
else rec CAND-CODEC-CONFORMANCE candidate BLOCKED_NO_CANDIDATE "AT0_CANDIDATE_CASE_TOOL unset"; fi
if [ -n "${AT0_CANDIDATE_RUN:-}" ]; then
    for f in cases/positive/*.case cases/negative/*.case $( [ -d "$HD" ] && ls "$HD"/H*.case ); do
        id=$(basename "$f" .case)
        $AT0_CANDIDATE_RUN "$f" > "$TMP/c.result" 2>"$TMP/c.err"; rc=$?
        if [ $rc -ne 0 ]; then rec "CAND-RUN-$id" candidate FAIL "runner exit $rc : $(head -1 "$TMP/c.err")"; continue; fi
        out=$($T result "$TMP/c.result" --case "$f" --json); st=$(printf '%s' "$out" | sed 's/.*"status":"\([A-Z]*\)".*/\1/')
        em=$(grep '^expectation_met' "$TMP/c.result" | awk '{print $2}')
        if [ "$st" = PASS ] && [ "$em" = YES ]; then rec "CAND-VERIFY-$id" candidate PASS "independent verification agrees; expectation met"
        elif [ "$st" = PASS ]; then rec "CAND-VERIFY-$id" candidate FAIL "record is honest but expectation_met=$em: the physics did not do what the case expects"
        else rec "CAND-VERIFY-$id" candidate "$st" "$(printf '%s' "$out" | sed 's/.*"findings":\[\(.*\)\]}/\1/' | cut -c1-200)"; fi
        case "$id" in H*) mkdir -p "$HD/results" && cp "$TMP/c.result" "$HD/results/$id.result";; *) cp "$TMP/c.result" "$OUT/candidate-$id.result";; esac
    done
    # wall-clock / environment independence: same case twice, different TZ and a pause
    f=$K; TZ=UTC $AT0_CANDIDATE_RUN "$f" > "$TMP/r1.result" 2>/dev/null; sleep 2; TZ=Asia/Tokyo AT0_EVALUATOR_NOISE=1 $AT0_CANDIDATE_RUN "$f" > "$TMP/r2.result" 2>/dev/null
    v1=$(grep '^verdict_id' "$TMP/r1.result"); v2=$(grep '^verdict_id' "$TMP/r2.result"); e1=$(grep '^evidence_digest' "$TMP/r1.result"); e2=$(grep '^evidence_digest' "$TMP/r2.result")
    b1=$(sed -n '/^begin values/,/^end values/p' "$TMP/r1.result" | sha256sum); b2=$(sed -n '/^begin values/,/^end values/p' "$TMP/r2.result" | sha256sum)
    if [ -n "$v1" ] && [ "$v1" = "$v2" ] && [ "$b1" = "$b2" ] && [ "$e1" != "$e2" ]; then rec CAND-WALLCLOCK-INDEPENDENCE control PASS "verdict_id and values identical across time/TZ; evidence digests differ"
    elif [ -n "$v1" ] && [ "$e1" = "$e2" ]; then rec CAND-WALLCLOCK-INDEPENDENCE control FAIL "evidence digests equal: run times not recorded or record reused"
    else rec CAND-WALLCLOCK-INDEPENDENCE control FAIL "verdict or values changed with wall clock or environment"; fi
else rec CAND-VERIFY candidate BLOCKED_NO_CANDIDATE "AT0_CANDIDATE_RUN unset"; rec CAND-WALLCLOCK-INDEPENDENCE control BLOCKED_NO_CANDIDATE "AT0_CANDIDATE_RUN unset"; fi
if [ -n "${AT0_CANDIDATE_OBJECTS:-}" ]; then
    if sh gates/isolation.sh $AT0_CANDIDATE_OBJECTS > "$TMP/iso.log" 2>&1; then rec CAND-ISOLATION gate PASS "no clock/random/env/file/net/process/thread symbol in candidate compute objects"; else rec CAND-ISOLATION gate FAIL "$(grep -m1 FAIL "$TMP/iso.log")"; fi
else rec CAND-ISOLATION gate BLOCKED_NO_CANDIDATE "AT0_CANDIDATE_OBJECTS unset"; fi
fi

# ---- summary ----
omega_commit=$(git -C ../../../.. rev-parse HEAD 2>/dev/null || echo unknown)
clean=$(git -C ../../../.. status --porcelain 2>/dev/null | grep -q . && echo NO || echo YES)
now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
overall=PASS; [ $nfail -gt 0 ] && overall=FAIL
{
printf '{\n  "program": "AT-0",\n  "agent": "Agent 4 independent adversarial evaluator",\n  "generated_utc": "%s",\n' "$now"
printf '  "omega_commit": "%s",\n  "omega_tree_clean": "%s",\n  "contract_commit": "044c9d11256d8642f80eedd42cbae8763faf63f5",\n  "charter_commit": "00e9f2308666f74e58f524b177b2a31eccdc69ba",\n  "spec_draft_commit": "0efd1a14cbdf117bc694b556bb61d031cf80c8f9",\n' "$omega_commit" "$clean"
printf '  "result_contract_version_expected": 2,\n  "isolation": "UNISOLATED",\n  "isolation_note": "single host, single user account, shared filesystem; hidden cases are withheld but their non-access cannot be proven (ISOLATION_REPORT.md)",\n'
printf '  "verdict_scope": "software conformance of the AT-0 implementation to the frozen contracts; no scientific discovery verdict is awarded or implied",\n'
printf '  "summary": {"pass": %d, "fail": %d, "inconclusive": %d, "blocked_no_candidate": %d, "not_run": %d, "self_test_overall": "%s"},\n' $npass $nfail $ninc $nblk $nnr "$overall"
printf '  "tests": [\n'; sed '$!s/$/,/' "$JSONL" | sed 's/^/    /'; printf '  ]\n}\n'
} > "$OUT/qualification.json"
echo "---- summary: pass=$npass fail=$nfail inconclusive=$ninc blocked_no_candidate=$nblk not_run=$nnr  (self-test overall $overall; isolation UNISOLATED)"
echo "written $OUT/qualification.json"
[ $nfail = 0 ]
