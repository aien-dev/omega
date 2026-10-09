#!/bin/sh
# Regenerates the public AT-0 case corpus of the Agent 4 evaluator from the
# generator (positive and negative controls) and from byte-level edits of the
# contract example (refusal controls). Hand derivations: ../README.md section 4.
# Usage: sh cases/generate.sh <at0-eval binary>   (run from evaluator/)
set -eu
T=${1:-build/at0-eval}
P=cases/positive; N=cases/negative; R=cases/refuse
K=$P/P1-kat-ideal-qubit-n4.case          # the contract's own example, kept verbatim
G() { out=$1; shift; "$T" gen "$@" > "$out"; }
# ---- positive controls (expected PASS) ----
G $P/P1b-ideal-n4-tau8-m8.case  name=at0-p1b-ideal-n4-tau8-m8 tau=1/8 w=1/2 M=8
G $P/P1c-ideal-n6-m6.case       name=at0-p1c-ideal-n6-m6 E=-5/2,-3/2,-1/2,1/2,3/2,5/2 tau=1/6 w=1/1 M=6
G $P/P1d-ref-offset-t2.case     name=at0-p1d-ref-offset-t2 ref=t2
G $P/P1e-x-axis-rotation.case   name=at0-p1e-x-axis-rotation h=0/1,1/2,0/1,0/1 'psi=(1/1;0/1),(0/1;0/1)'
G $P/P1f-y-axis-rotation.case   name=at0-p1f-y-axis-rotation h=0/1,0/1,1/2,0/1 'psi=(1/1;0/1),(0/1;0/1)'
G $P/P1g-complex-psi-yplus.case name=at0-p1g-complex-psi-yplus 'psi=(1/1;0/1),(0/1;1/1)'
G $P/P2-tilted-h0.case          name=at0-p2-tilted-h0 E=-3/4,-1/4,1/4,3/4 h=1/4,3/10,0/1,2/5 tau=1/2 w=1/1 M=4
G $P/P2b-h0-shift-control.case  name=at0-p2b-h0-shift-control h=1/1,0/1,0/1,1/2
G $P/P3-spec-reference-model.case name=at0-p3-spec-reference-model E=-1/1,0/1 h=1/2,0/1,0/1,-1/2 tau=1/4 w=1/2 M=4
G $P/P4-degenerate-identity-h.case name=at0-p4-degenerate-identity-h E=-1/2,1/2,3/2,5/2 h=1/2,0/1,0/1,0/1 tau=1/4 w=1/1 M=4
G $P/P5-large-rationals.case name=at0-p5-large-rationals E=-7/1048573,7/1048573 h=0/1,2/1048573,3/1048573,6/1048573 'psi=(1/1048571;0/1),(1/1048569;1/1048567)' tau=1048573/28 w=1/1 M=2
# ---- negative controls (expected FAIL with exactly these codes) ----
G $N/N1-uncovered-spectrum.case name=at0-n1-uncovered-spectrum E=1/1,2/1,3/1,4/1 control=NEGATIVE expected=FAIL codes=TRIVIAL_PHYSICAL_STATE
G $N/N2-half-covered.case       name=at0-n2-half-covered E=-3/2,1/2,3/2,7/2 control=NEGATIVE expected=FAIL codes=SCHRODINGER_DEVIATION_EXCEEDED
G $N/N3-broken-clock-tau3.case  name=at0-n3-broken-clock-tau3 tau=1/3 control=NEGATIVE expected=FAIL codes=POVM_NORMALIZATION_EXCEEDED
G $N/N4-wrong-weight-2.case     name=at0-n4-wrong-weight-2 w=2/1 control=NEGATIVE expected=FAIL codes=POVM_NORMALIZATION_EXCEEDED,PROBABILITY_SUM_EXCEEDED
G $N/N4b-wrong-weight-5.case    name=at0-n4b-wrong-weight-5 w=5/1 control=NEGATIVE expected=FAIL codes=POVM_NORMALIZATION_EXCEEDED,PROBABILITY_OUT_OF_RANGE,PROBABILITY_SUM_EXCEEDED
G $N/N5-precision-demand.case   name=at0-n5-precision-demand minbk=RIGOROUS control=NEGATIVE expected=FAIL codes=BOUND_KIND_INSUFFICIENT
G $N/N6-zero-kernel-component.case name=at0-n6-zero-kernel-component E=-3/2,1/2,3/2,7/2 'psi=(1/1;0/1),(0/1;0/1)' control=NEGATIVE expected=FAIL codes=TRIVIAL_PHYSICAL_STATE
G $N/N7-unreachable-labels.case  name=at0-n7-unreachable-labels tolz=1@0 control=NEGATIVE expected=FAIL codes=CONDITIONAL_UNDEFINED
# ---- refusal controls: byte edits of the example; expected code in MANIFEST.tsv ----
E() { out=$1; shift; sed "$@" "$K" > "$out"; }
E $R/R01-noncanonical-rational.case    's|^povm_weight 1/1$|povm_weight 2/2|'
E $R/R02-header-v2.case                's|^OMEGA-AT0-CASE v1$|OMEGA-AT0-CASE v2|'
"$T" gen name=at0-r03-clock-dim-1 E=-1/2 > $R/R03-clock-dim-1.case
"$T" gen name=at0-r04-irrational h=0/1,1/1,0/1,1/1 > $R/R04-irrational-spectrum.case
E $R/R05-wrong-case-id.case            's|^case_id 3cf4|case_id 4cf4|'
"$T" gen name=at0-r06-codes-with-pass expected=PASS codes=TRIVIAL_PHYSICAL_STATE > $R/R06-codes-with-pass.case
"$T" gen name=at0-r06b-codes-unsorted expected=FAIL control=NEGATIVE codes=PROBABILITY_SUM_EXCEEDED,POVM_NORMALIZATION_EXCEEDED > $R/R06b-codes-unsorted.case
"$T" gen name=at0-r06c-codes-unknown expected=FAIL control=NEGATIVE codes=FOO_BAR > $R/R06c-codes-unknown.case
"$T" gen name=at0-r06d-fail-without-codes expected=FAIL control=NEGATIVE codes=none > $R/R06d-fail-without-codes.case
sed 's|$|\r|' "$K" > $R/R07-crlf.case
E $R/R08-tab.case                      's|^clock_dim 4$|clock_dim\t4|'
sed 's|^begin semantic$|begin semantic\n|' "$K" > $R/R09-blank-line.case
E $R/R10-trailing-space.case           's|^system_dim 2$|system_dim 2 |'
E $R/R11-double-space.case             's|^system_dim 2$|system_dim  2|'
E $R/R12-missing-key.case              '/^interaction NONE$/d'
sed 's|^interaction NONE$|interaction NONE\nfoo bar|' "$K" > $R/R13-unknown-key.case
sed 's|^interaction NONE$|interaction NONE\ninteraction NONE|' "$K" > $R/R14-duplicate-key.case
awk '/^interaction NONE$/{h=$0; next} /^constraint SUM_HC_HS$/{print; print h; next} {print}' "$K" > $R/R15-out-of-order.case
E $R/R16-float-text.case               's|^povm_tau_turns 1/4$|povm_tau_turns 0.25|'
E $R/R17-noncanonical-integer.case     's|^clock_dim 4$|clock_dim 04|'
E $R/R18-noncanonical-scaled.case      's|^tol_probability 1@12$|tol_probability 10@13|'
E $R/R19-label-uppercase.case          's|^clock_label 0 t0$|clock_label 0 T0|; s|^reference_clock_label t0$|reference_clock_label T0|'
"$T" gen name=at0-r20-energies-not-increasing E=-1/2,-3/2,1/2,3/2 > $R/R20-energies-not-increasing.case
"$T" gen name=at0-r21-psi-zero 'psi=(0/1;0/1),(0/1;0/1)' > $R/R21-psi-zero.case
"$T" gen name=at0-r22-tau-zero tau=0/1 > $R/R22-tau-zero.case
"$T" gen name=at0-r23-weight-negative w=-1/1 > $R/R23-weight-negative.case
E $R/R24-wrong-domain.case             's|^domain omega.at0.case.v1$|domain omega.at0.case.v2|'
E $R/R25-wrong-contract.case           's|^contract AT0_CASE_V1$|contract AT0_CASE_V2|'
E65=$(i=0; s=""; while [ $i -lt 65 ]; do s="$s${s:+,}$i/1"; i=$((i+1)); done; printf "%s" "$s")
E $R/R26-clock-dim-65.case "s|^clock_dim 4$|clock_dim 65|; s|^clock_energies .*$|clock_energies $E65|"
E $R/R39-huge-digits-tau.case          's|^povm_tau_turns .*$|povm_tau_turns 340282366920938463463374607431768211457/4|'
"$T" gen name=at0-r27-rational-over-limit E=-1048577/1,-1/2,1/2,3/2 > $R/R27-rational-over-limit.case
"$T" gen name=at0-r28-duplicate-label labels=t0,t1,t1,t3 > $R/R28-duplicate-label.case
"$T" gen name=at0-r29-ref-label-missing ref=t9 > $R/R29-ref-label-missing.case
E $R/R30-negative-zero.case            's|^system_hamiltonian_pauli 0/1,0/1,0/1,1/2$|system_hamiltonian_pauli -0/1,0/1,0/1,1/2|'
E $R/R31-control-kind-bad.case         's|^control_kind POSITIVE$|control_kind MAYBE|'
E $R/R32-case-name-two-tokens.case     's|^case_name at0-kat-ideal-qubit-n4$|case_name at0 kat|'
head -c -1 "$K" > $R/R33-missing-final-lf.case
: > $R/R34-empty.case
sed 's|^end$|end\nextra|' "$K" > $R/R35-trailing-line.case
E $R/R36-label-count-mismatch.case     's|^clock_label_count 4$|clock_label_count 5|'
E $R/R37-model-family-bad.case         's|^model_family PAGE_WOOTTERS_FINITE_IDEAL$|model_family PAGE_WOOTTERS_INTERACTING|'
E $R/R38-acceptance-id-wrong.case      's|^acceptance_id a13f|acceptance_id b13f|'
echo "generated: $(ls $P $N $R | grep -c '\.case$') case files"
