//! Test suite of the AT-1 reference oracle. Expectations come from the frozen contracts
//! (AT1_CASE_V1, AT1_RESULT_V1 at aien-architecture cbe4c8e) and from AT1_SPEC.md section
//! 13 (81047f5); none is copied from a run of this oracle.

use super::at1::big::Q;
use super::at1::case::{parse_case, refresh_ids, Spec};
use super::at1::fixtures::{self, calibrate, q, qi, Fixture};
use super::at1::result::{bound_exact, bound_token, compute, evidence_digest, judge, parse_values, result_bytes, value_exact, Computed, CHECK_NAMES, PROVENANCE_KEYS};

const KAT_FILE: &str = "76e282fecf21025c9dd675378244e90ff192822a4c49d6cb812c9fe84b3f9ba4";
const KAT_CASE_ID: &str = "890980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1";
const KAT_ACC_ID: &str = "d63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f";

fn fixture(name: &str) -> Fixture {
    fixtures::all().into_iter().find(|f| f.name == name).unwrap_or_else(|| panic!("no fixture {}", name))
}
fn p2_text() -> String {
    String::from_utf8(fixture("at1-kat-rotated-level-n4").spec.bytes()).unwrap()
}
fn run(spec: &Spec) -> (super::at1::case::Case, Computed) {
    let c = parse_case(&spec.bytes()).expect("case parses");
    let comp = compute(&c);
    (c, comp)
}
fn check_result<'a>(comp: &'a Computed, name: &str) -> &'a str {
    comp.verdict.checks.iter().find(|(n, _)| *n == name).map(|(_, r)| *r).unwrap()
}
fn provenance(started: &str) -> Vec<(&'static str, String)> {
    let exe = "ab".repeat(32);
    let commit = "cd".repeat(20);
    let vals = [
        "aien-dev/omega".to_string(),
        commit.clone(),
        "YES".to_string(),
        "cbe4c8ed88d28bb96d5209327ecd30aa9cddaf7a".to_string(),
        exe.clone(),
        "aien-dev/omega".to_string(),
        commit,
        exe,
        "oracle rustc test".to_string(),
        "rustc test src/main.rs".to_string(),
        "testhost".to_string(),
        started.to_string(),
        "2026-10-10T00:00:09Z".to_string(),
    ];
    PROVENANCE_KEYS.iter().cloned().zip(vals).collect()
}

// ---------------------------------------------------------------- identities (KAT)

#[test]
fn worked_example_identities_are_byte_exact() {
    let text = p2_text();
    let c = parse_case(text.as_bytes()).expect("worked example parses");
    assert_eq!(c.file_sha256, KAT_FILE);
    assert_eq!(c.case_id, KAT_CASE_ID);
    assert_eq!(c.acceptance_id, KAT_ACC_ID);
    let comp = compute(&c);
    assert_eq!(comp.levels.kernel_dim, 2);
    assert!(!comp.levels.trivial);
    // the AT1_SPEC worked example table: p = 5/28, 1/4, 9/28, 1/4
    let want = [5.0 / 28.0, 0.25, 9.0 / 28.0, 0.25];
    for k in 0..4 {
        assert!((comp.interacting[k].p - want[k]).abs() < 1e-15);
        assert!((comp.matrix.labels[k].p - want[k]).abs() < 1e-15);
    }
    assert_eq!(comp.verdict.outcome, "PASS");
}

// ---------------------------------------------------------------- section 13 tables

#[test]
fn every_section13_table_matches_on_both_routes_within_bounds() {
    let all = fixtures::all();
    assert_eq!(all.len(), 21, "P1a, P1b, P1c, P2, P3, P3b, P4, P5, P6, N1 to N1f, N2, N3, N4a, N4b, N5, N6");
    for f in &all {
        let (_c, comp) = run(&f.spec);
        assert_eq!(comp.levels.kernel_dim, f.kernel_dim, "{} kernel dim", f.name);
        assert_eq!(comp.levels.trivial, f.trivial, "{} trivial", f.name);
        assert_eq!(comp.matrix.trivial, f.trivial, "{} matrix trivial", f.name);
        let cal = calibrate(f, &comp);
        assert_eq!(cal.bound_violations, 0, "{}: an actual error exceeds its written bound", f.name);
        assert!(cal.max_dev_matrix <= 1e-14 && cal.max_dev_closed <= 1e-14 && cal.max_dev_ideal <= 1e-14, "{}: deviation", f.name);
        assert!(cal.max_bound < 1e-12, "{}: a bound reaches the tolerance", f.name);
        assert!(cal.values_checked >= 25, "{}", f.name);
    }
}

#[test]
fn every_case_meets_its_own_expectation() {
    for f in fixtures::all() {
        let (_c, comp) = run(&f.spec);
        let exp: Vec<&str> = f.spec.expected_codes.iter().map(String::as_str).collect();
        assert_eq!(comp.verdict.outcome, f.spec.expected_outcome, "{}", f.name);
        assert_eq!(comp.verdict.codes, exp, "{}", f.name);
        assert!(comp.verdict_lines.contains(&"expectation_met YES".to_string()), "{}", f.name);
    }
}

#[test]
fn n1_family_fails_check_10_and_passes_check_11() {
    for name in ["at1-n1-p2-ideal", "at1-n1b-p3-ideal", "at1-n1c-p3b-ideal", "at1-n1d-p4-ideal", "at1-n1e-p5-ideal", "at1-n1f-p6-ideal"] {
        let (_c, comp) = run(&fixture(name).spec);
        assert_eq!(check_result(&comp, "target_agreement"), "FAIL", "{}", name);
        assert_eq!(check_result(&comp, "oracle_cross_check"), "PASS", "{}", name);
        assert_eq!(check_result(&comp, "label_status_agreement"), "PASS", "{}", name);
        assert_eq!(comp.verdict.codes, vec!["SCHRODINGER_DEVIATION_EXCEEDED"], "{}", name);
    }
    // under INTERACTING check 11 is not run
    let (_c, comp) = run(&fixture("at1-kat-rotated-level-n4").spec);
    assert_eq!(check_result(&comp, "oracle_cross_check"), "NOT_EVALUATED");
}

#[test]
fn trivial_kernel_rule() {
    for name in ["at1-n2-all-levels-out", "at1-n6-zero-projection"] {
        let (_c, comp) = run(&fixture(name).spec);
        for (i, (n, r)) in comp.verdict.checks.iter().enumerate() {
            let evaluated = matches!(i + 1, 1 | 2 | 3 | 5);
            assert_eq!(*n, CHECK_NAMES[i]);
            assert_eq!(*r == "NOT_EVALUATED", !evaluated, "{} check {} {}", name, i + 1, r);
        }
        assert_eq!(check_result(&comp, "physical_state_nontrivial"), "FAIL");
        for l in &comp.values {
            let t: Vec<&str> = l.split(' ').collect();
            match t[0] {
                "label" | "reference_label" => assert_eq!(t[3], "UNDEFINED"),
                "constraint_residual" => assert_eq!(&t[1..], ["undefined", "0@0"]),
                "clock_probability" | "reference_clock_probability" => assert_eq!(&t[2..], ["undefined", "0@0"]),
                "pauli" | "reference_interacting" => assert_eq!(&t[4..], ["undefined", "0@0"]),
                "reference_ideal" => assert!(t[4].starts_with("f64:")),
                _ => {}
            }
        }
    }
}

#[test]
fn n3_zero_label_is_undefined_with_zero_bound_lines() {
    let (_c, comp) = run(&fixture("at1-n3-swapped-level-zero-label").spec);
    assert!(comp.values.contains(&"label 2 t2 UNDEFINED".to_string()));
    assert!(comp.values.contains(&"reference_label 2 t2 UNDEFINED".to_string()));
    assert!(comp.values.contains(&"pauli 2 Y MINUS undefined 0@0".to_string()));
    assert!(comp.values.contains(&"reference_interacting 2 X PLUS undefined 0@0".to_string()));
    assert_eq!(check_result(&comp, "conditional_defined"), "FAIL");
}

// ---------------------------------------------------------------- refusals

fn edit(text: &str, from: &str, to: &str) -> String {
    assert_eq!(text.matches(from).count(), 1, "defect anchor {:?} must occur once", from);
    text.replacen(from, to, 1)
}
fn refused(bytes: &[u8]) -> &'static str {
    match parse_case(bytes) {
        Ok(_) => "ACCEPTED",
        Err(r) => r.code,
    }
}
fn refused_fresh(text: &str) -> &'static str {
    refused(refresh_ids(text).as_bytes())
}

#[test]
fn spec_13_5_refusal_classes() {
    let t = p2_text();
    assert_eq!(refused(t.as_bytes()), "ACCEPTED");
    let pe = "CASE_PARSE_ERROR";
    let nc = "CASE_NONCANONICAL";
    let uv = "CASE_UNSUPPORTED_VERSION";
    let ip = "CASE_INVALID_PARAMETER";
    // R0
    assert_eq!(refused_fresh(&edit(&t, "system_dim 2\n", "")), pe);
    assert_eq!(refused(t.replace('\n', "\r\n").as_bytes()), pe);
    // R1
    assert_eq!(refused_fresh(&edit(&t, "povm_tau_turns 1/4", "povm_tau_turns 2/8")), nc);
    // R2, R2b, R2c
    assert_eq!(refused_fresh(&edit(&t, "OMEGA-AT1-CASE v1", "OMEGA-AT1-CASE v2")), uv);
    assert_eq!(refused_fresh(&edit(&t, "domain omega.at1.case.v1", "domain omega.at1.case.v2")), uv);
    assert_eq!(refused_fresh(&edit(&t, "contract AT1_CASE_V1", "contract AT1_CASE_V2")), uv);
    // R3: 65 levels with 65 couplings; R3b: one level with one coupling
    let mut s = fixture("at1-kat-rotated-level-n4").spec;
    s.energies = (0..65).map(qi).collect();
    s.v = vec![[Q::zero(), Q::zero(), Q::zero()]; 65];
    assert_eq!(refused(&s.bytes()), ip);
    s.energies = vec![q(-1, 2)];
    s.v = vec![[Q::zero(), Q::zero(), Q::zero()]];
    assert_eq!(refused(&s.bytes()), ip);
    // R4: per-level irrational spectrum while |h| is rational
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 2 3/10,0/1,-1/10", "interaction_pauli 2 1/2,0/1,-1/10")), "CASE_IRRATIONAL_SPECTRUM");
    // R5, R5b: identities not refreshed
    assert_eq!(refused(edit(&t, &format!("case_id {}", KAT_CASE_ID), &format!("case_id 0{}", &KAT_CASE_ID[1..])).as_bytes()), "CASE_ID_MISMATCH");
    assert_eq!(refused(edit(&t, &format!("acceptance_id {}", KAT_ACC_ID), &format!("acceptance_id 0{}", &KAT_ACC_ID[1..])).as_bytes()), "CASE_ID_MISMATCH");
    // R6, R6b
    assert_eq!(refused_fresh(&edit(&t, "expected_outcome PASS", "expected_outcome FAIL")), ip);
    let neg = edit(&edit(&t, "control_kind POSITIVE", "control_kind NEGATIVE"), "expected_outcome PASS", "expected_outcome FAIL");
    assert_eq!(refused_fresh(&edit(&neg, "expected_failure_codes none", "expected_failure_codes NOT_AN_AT1_CODE")), ip);
    // R7 coupling count, R8 coupling order
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 3 0/1,0/1,0/1\n", "")), pe);
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 0/1,0/1,0/1\ninteraction_pauli 2 3/10,0/1,-1/10\n", "interaction_pauli 2 3/10,0/1,-1/10\ninteraction_pauli 1 0/1,0/1,0/1\n")), pe);
    // R9 to R12 fixed literals and enumeration
    assert_eq!(refused_fresh(&edit(&t, "interaction CLOCK_DIAGONAL_PAULI", "interaction NONE")), pe);
    assert_eq!(refused_fresh(&edit(&t, "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG", "model_family PAGE_WOOTTERS_FINITE_IDEAL")), pe);
    assert_eq!(refused_fresh(&edit(&t, "constraint SUM_HC_HS_V", "constraint SUM_HC_HS")), pe);
    assert_eq!(refused_fresh(&edit(&t, "prediction_target INTERACTING", "prediction_target BOTH")), pe);
    assert_eq!(refused_fresh(&edit(&t, "prediction_target INTERACTING\n", "")), pe);
    // R13 non-canonical coupling component, R14 over the token limit
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 0/1,0/1,0/1", "interaction_pauli 1 2/10,0/1,0/1")), nc);
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 0/1,0/1,0/1", "interaction_pauli 1 1048577/1,0/1,0/1")), ip);
    // two defects: the earlier rule wins (R1 noncanonical before R4 spectrum)
    let two = edit(&edit(&t, "povm_tau_turns 1/4", "povm_tau_turns 2/8"), "interaction_pauli 2 3/10,0/1,-1/10", "interaction_pauli 2 1/2,0/1,-1/10");
    assert_eq!(refused_fresh(&two), nc);
}

#[test]
fn inherited_at0_refusal_rows_on_p2() {
    let t = p2_text();
    let pe = "CASE_PARSE_ERROR";
    let nc = "CASE_NONCANONICAL";
    let uv = "CASE_UNSUPPORTED_VERSION";
    let ip = "CASE_INVALID_PARAMETER";
    let rows: Vec<(&str, String, &str)> = vec![
        ("R07 crlf", t.replace('\n', "\r\n"), pe),
        ("R08 tab", edit(&t, "clock_dim 4", "clock_dim\t4"), pe),
        ("R09 blank-line", edit(&t, "system_dim 2\n", "system_dim 2\n\n"), pe),
        ("R10 trailing-space", edit(&t, "system_dim 2\n", "system_dim 2 \n"), pe),
        ("R11 double-space", edit(&t, "system_dim 2", "system_dim  2"), pe),
        ("R12 missing-key", edit(&t, "interaction CLOCK_DIAGONAL_PAULI\n", ""), pe),
        ("R13 unknown-key", edit(&t, "system_dim 2\n", "system_dim 2\nfoo bar\n"), pe),
        ("R14 duplicate-key", edit(&t, "interaction CLOCK_DIAGONAL_PAULI\n", "interaction CLOCK_DIAGONAL_PAULI\ninteraction CLOCK_DIAGONAL_PAULI\n"), pe),
        ("R15 out-of-order", edit(&t, "system_hamiltonian_pauli 0/1,0/1,0/1,1/2\ninteraction CLOCK_DIAGONAL_PAULI\n", "interaction CLOCK_DIAGONAL_PAULI\nsystem_hamiltonian_pauli 0/1,0/1,0/1,1/2\n"), pe),
        ("R16 float-text", edit(&t, "povm_tau_turns 1/4", "povm_tau_turns 0.25"), pe),
        ("R19 label-uppercase", edit(&edit(&t, "reference_clock_label t0", "reference_clock_label T0"), "clock_label 0 t0", "clock_label 0 T0"), pe),
        ("R31 control-kind-bad", edit(&t, "control_kind POSITIVE", "control_kind MAYBE"), pe),
        ("R32 case-name-two-tokens", edit(&t, "case_name at1-kat-rotated-level-n4", "case_name at1 kat"), pe),
        ("R35 trailing-line", format!("{}extra\n", t), pe),
        ("R36 label-count-mismatch", edit(&t, "clock_label_count 4", "clock_label_count 5"), pe),
        ("R37 model-family-bad", edit(&t, "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG", "model_family PAGE_WOOTTERS_INTERACTING"), pe),
        ("R01 noncanonical-rational", edit(&t, "povm_weight 1/1", "povm_weight 2/2"), nc),
        ("R17 noncanonical-integer", edit(&t, "clock_dim 4", "clock_dim 04"), nc),
        ("R18 noncanonical-scaled", edit(&t, "tol_probability 1@12", "tol_probability 10@13"), nc),
        ("R30 negative-zero", edit(&t, "system_hamiltonian_pauli 0/1,", "system_hamiltonian_pauli -0/1,"), nc),
        ("R02 header-v2", edit(&t, "OMEGA-AT1-CASE v1", "OMEGA-AT1-CASE v2"), uv),
        ("R24 wrong-domain", edit(&t, "domain omega.at1.case.v1", "domain omega.at1.case.v2"), uv),
        ("R25 wrong-contract", edit(&t, "contract AT1_CASE_V1", "contract AT1_CASE_V2"), uv),
        ("R20 energies-not-increasing", edit(&t, "clock_energies -3/2,-1/2,", "clock_energies -1/2,-3/2,"), ip),
        ("R21 psi-zero", edit(&t, "reference_system_state (1/1;0/1),(1/1;0/1)", "reference_system_state (0/1;0/1),(0/1;0/1)"), ip),
        ("R22 tau-zero", edit(&t, "povm_tau_turns 1/4", "povm_tau_turns 0/1"), ip),
        ("R23 weight-negative", edit(&t, "povm_weight 1/1", "povm_weight -1/1"), ip),
        ("R27 rational-over-limit", edit(&t, "clock_energies -3/2,", "clock_energies -1048577/1,"), ip),
        ("R28 duplicate-label", edit(&t, "clock_label 2 t2", "clock_label 2 t1"), ip),
        ("R29 ref-label-missing", edit(&t, "reference_clock_label t0", "reference_clock_label t9"), ip),
        ("R39 huge-digits-tau", edit(&t, "povm_tau_turns 1/4", "povm_tau_turns 340282366920938463463374607431768211457/4"), ip),
        ("R06 codes-with-pass", edit(&t, "expected_failure_codes none", "expected_failure_codes TRIVIAL_PHYSICAL_STATE"), ip),
        ("R04 irrational-spectrum", edit(&t, "system_hamiltonian_pauli 0/1,0/1,0/1,1/2", "system_hamiltonian_pauli 0/1,1/1,0/1,1/1"), "CASE_IRRATIONAL_SPECTRUM"),
    ];
    for (name, text, code) in &rows {
        let got = if name.starts_with("R07") { refused(text.as_bytes()) } else { refused_fresh(text) };
        assert_eq!(got, *code, "{}", name);
    }
    let neg = edit(&edit(&t, "control_kind POSITIVE", "control_kind NEGATIVE"), "expected_outcome PASS", "expected_outcome FAIL");
    assert_eq!(refused_fresh(&edit(&neg, "expected_failure_codes none", "expected_failure_codes PROBABILITY_SUM_EXCEEDED,POVM_NORMALIZATION_EXCEEDED")), ip, "R06b codes-unsorted");
    assert_eq!(refused_fresh(&edit(&neg, "expected_failure_codes none", "expected_failure_codes FOO_BAR")), ip, "R06c codes-unknown");
    assert_eq!(refused_fresh(&neg), ip, "R06d fail-without-codes");
    assert_eq!(refused(t.trim_end_matches('\n').as_bytes()), pe, "R33 missing-final-lf");
    assert_eq!(refused(b""), pe, "R34 empty");
}

#[test]
fn charter_readings_d_and_e_and_declared_token_readings() {
    let t = p2_text();
    // (d) the first token of a version line is its key: an AT-0 header is a parse error
    assert_eq!(refused_fresh(&edit(&t, "OMEGA-AT1-CASE v1", "OMEGA-AT0-CASE v1")), "CASE_PARSE_ERROR");
    assert_eq!(refused_fresh(&edit(&t, "contract AT1_CASE_V1", "contract AT0_CASE_V1")), "CASE_UNSUPPORTED_VERSION");
    assert_eq!(refused_fresh(&edit(&t, "domain omega.at1.case.v1", "domain omega.at0.case.v1")), "CASE_UNSUPPORTED_VERSION");
    // (e) index tokens by parsed value: 01 at position 1 is non-canonical, x or 5 is shape
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 ", "interaction_pauli 01 ")), "CASE_NONCANONICAL");
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 ", "interaction_pauli x ")), "CASE_PARSE_ERROR");
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 ", "interaction_pauli 5 ")), "CASE_PARSE_ERROR");
    assert_eq!(refused_fresh(&edit(&t, "clock_label 1 t1", "clock_label 01 t1")), "CASE_NONCANONICAL");
    // (b) shape before range, non-canonical decided after the whole shape pass
    let shape_late = edit(&edit(&t, "povm_weight 1/1", "povm_weight 2/2"), "observables PAULI_X,PAULI_Y,PAULI_Z", "observables PAULI_X");
    assert_eq!(refused_fresh(&shape_late), "CASE_PARSE_ERROR");
    // declared readings (README): integer grammar -?[0-9]+, fixed arities, d < 1
    assert_eq!(refused_fresh(&edit(&t, "clock_dim 4", "clock_dim +4")), "CASE_PARSE_ERROR");
    // `clock_dim 0` against a non-empty clock_energies line is a count mismatch (shape)
    assert_eq!(refused_fresh(&edit(&t, "clock_dim 4", "clock_dim 0")), "CASE_PARSE_ERROR");
    // X7 (Agent 0 ruling, omega#371 comment 6093816127): `none` is the empty list, so
    // `clock_dim 0` + `clock_energies none` + no interaction_pauli lines passes the shape pass
    // and the range rule 2 <= N <= 64 refuses it
    {
        let x7: String = edit(&edit(&t, "clock_dim 4", "clock_dim 0"), "clock_energies -3/2,-1/2,1/2,3/2", "clock_energies none")
            .lines()
            .filter(|l| !l.starts_with("interaction_pauli "))
            .map(|l| format!("{}\n", l))
            .collect();
        assert_eq!(refused_fresh(&x7), "CASE_INVALID_PARAMETER", "X7 clock-dim-0-none");
    }
    assert_eq!(refused_fresh(&edit(&t, "interaction_pauli 1 0/1,0/1,0/1", "interaction_pauli 1 0/1,0/1")), "CASE_PARSE_ERROR");
    assert_eq!(refused_fresh(&edit(&t, "system_hamiltonian_pauli 0/1,0/1,0/1,1/2", "system_hamiltonian_pauli 0/1,0/1,1/2")), "CASE_PARSE_ERROR");
    assert_eq!(refused_fresh(&edit(&t, "povm_weight 1/1", "povm_weight 1/0")), "CASE_NONCANONICAL");
    assert_eq!(refused_fresh(&edit(&t, "tol_probability 1@12", "tol_probability 1@41")), "CASE_INVALID_PARAMETER");
}

// ---------------------------------------------------------------- judge and mutants

/// Values block whose engine lines (label, clock_probability, pauli and the residuals)
/// come from `mutant` and whose reference lines come from `orig`.
fn splice(orig: &[String], mutant: &[String]) -> Vec<String> {
    assert_eq!(orig.len(), mutant.len());
    orig.iter()
        .zip(mutant)
        .map(|(o, m)| {
            let key = o.split(' ').next().unwrap();
            if matches!(key, "label" | "clock_probability" | "pauli") {
                m.clone()
            } else {
                o.clone()
            }
        })
        .collect()
}

fn mutant_verdict(orig: &Spec, mutant: &Spec) -> super::at1::result::Verdict {
    let (c, comp) = run(orig);
    let (_m, mcomp) = run(mutant);
    judge(&c, &splice(&comp.values, &mcomp.values), "ESTIMATED").expect("spliced block parses")
}

fn result_of(v: &super::at1::result::Verdict, name: &str) -> &'static str {
    v.checks.iter().find(|(n, _)| *n == name).map(|(_, r)| *r).unwrap()
}

#[test]
fn mutant_m1_drop_v() {
    let p2 = fixture("at1-kat-rotated-level-n4").spec;
    let mut m1 = p2.clone();
    m1.v = vec![[Q::zero(), Q::zero(), Q::zero()]; 4];
    let v = mutant_verdict(&p2, &m1);
    assert_eq!(v.codes, vec!["INTERACTING_DEVIATION_EXCEEDED"]);
    // under IDEAL (N1): check 10 passes, check 11 fails
    let n1 = fixture("at1-n1-p2-ideal").spec;
    let mut m1i = n1.clone();
    m1i.v = vec![[Q::zero(), Q::zero(), Q::zero()]; 4];
    let v = mutant_verdict(&n1, &m1i);
    assert_eq!(result_of(&v, "target_agreement"), "PASS");
    assert_eq!(result_of(&v, "oracle_cross_check"), "FAIL");
    assert_eq!(v.codes, vec!["ORACLE_DISAGREEMENT"]);
    // blind on P1c (coupling outside the kernel); P1 itself has V = 0, so dropping V is a no-op there
    let p1c = fixture("at1-p1c-spectator-coupling").spec;
    let mut m1c = p1c.clone();
    m1c.v = vec![[Q::zero(), Q::zero(), Q::zero()]; 4];
    assert_eq!(mutant_verdict(&p1c, &m1c).outcome, "PASS");
}

#[test]
fn mutants_m3_m4_are_visible_on_p2() {
    let p2 = fixture("at1-kat-rotated-level-n4").spec;
    // M2 (sign of V flipped) is not expressible as a valid case on P2: h - v_2 has the
    // irrational |n|^2 = 9/20, so it stays with Agent 4's engine mutants.
    // M3 v_j applied to level j + 1
    let mut m3 = p2.clone();
    m3.v.rotate_right(1);
    assert_eq!(mutant_verdict(&p2, &m3).codes, vec!["INTERACTING_DEVIATION_EXCEEDED"]);
    // M4 pre-freeze phase sign: with r = 0 and a complete clock, label k of the mutant is
    // label (M - k) mod M of the correct engine
    let (c, comp) = run(&p2);
    let m = 4;
    let permuted: Vec<String> = comp
        .values
        .iter()
        .map(|l| {
            let t: Vec<&str> = l.split(' ').collect();
            if matches!(t[0], "clock_probability" | "pauli") {
                let k: usize = t[1].parse().unwrap();
                let src = format!("{} {} ", t[0], (m - k) % m);
                let found = comp.values.iter().find(|x| x.starts_with(&src) && (t[0] == "clock_probability" || x.split(' ').nth(2) == Some(t[2]) && x.split(' ').nth(3) == Some(t[3]))).unwrap();
                let ft: Vec<&str> = found.split(' ').collect();
                let mut out = t[..2].to_vec();
                out.extend_from_slice(&ft[2..]);
                out.join(" ")
            } else {
                l.clone()
            }
        })
        .collect();
    let v = judge(&c, &permuted, "ESTIMATED").unwrap();
    assert_eq!(v.codes, vec!["INTERACTING_DEVIATION_EXCEEDED"]);
}

#[test]
fn label_status_mismatch_fails_check_12() {
    let (c, comp) = run(&fixture("at1-kat-rotated-level-n4").spec);
    let lines: Vec<String> = comp
        .values
        .iter()
        .map(|l| {
            if l == "label 2 t2 DEFINED" {
                "label 2 t2 UNDEFINED".to_string()
            } else if l.starts_with("pauli 2 ") {
                let t: Vec<&str> = l.split(' ').collect();
                format!("pauli 2 {} {} undefined 0@0", t[2], t[3])
            } else {
                l.clone()
            }
        })
        .collect();
    let v = judge(&c, &lines, "ESTIMATED").unwrap();
    assert_eq!(result_of(&v, "label_status_agreement"), "FAIL");
    assert!(v.codes.contains(&"ORACLE_DISAGREEMENT"));
}

#[test]
fn values_parser_enforces_undefined_placement() {
    let (c, comp) = run(&fixture("at1-kat-rotated-level-n4").spec);
    let bad: Vec<String> = comp.values.iter().map(|l| if l.starts_with("clock_probability 1 ") { "clock_probability 1 undefined 0@0".to_string() } else { l.clone() }).collect();
    assert!(parse_values(&bad, &c).is_err());
    let bad: Vec<String> = comp.values.iter().map(|l| if l.starts_with("reference_ideal 0 X PLUS ") { "reference_ideal 0 X PLUS undefined 0@0".to_string() } else { l.clone() }).collect();
    assert!(parse_values(&bad, &c).is_err());
    let bad: Vec<String> = comp.values.iter().map(|l| if l.starts_with("povm_residual ") { "povm_residual undefined 0@0".to_string() } else { l.clone() }).collect();
    assert!(parse_values(&bad, &c).is_err());
    let mut swapped = comp.values.clone();
    let i = swapped.iter().position(|l| l.starts_with("pauli 0 X PLUS")).unwrap();
    swapped.swap(i, i + 1);
    assert!(parse_values(&swapped, &c).is_err());
    for tok in ["+2", "02", "-2", "2.0", ""] {
        let bad: Vec<String> = comp.values.iter().map(|l| if l.starts_with("physical_state_kernel_dim ") { format!("physical_state_kernel_dim {}", tok) } else { l.clone() }).collect();
        assert!(parse_values(&bad, &c).is_err(), "kernel_dim token {:?}", tok);
    }
    assert!(parse_values(&comp.values, &c).is_ok());
}

#[test]
fn nonfinite_value_fails_check_2() {
    let (c, comp) = run(&fixture("at1-kat-rotated-level-n4").spec);
    let lines: Vec<String> = comp.values.iter().map(|l| if l.starts_with("pauli 1 Z PLUS ") { "pauli 1 Z PLUS nonfinite 1@20".to_string() } else { l.clone() }).collect();
    let v = judge(&c, &lines, "ESTIMATED").unwrap();
    assert_eq!(result_of(&v, "values_finite"), "FAIL");
    assert!(v.codes.contains(&"NONFINITE_VALUE"));
}

// ---------------------------------------------------------------- invariance tests (spec 9.1)

fn table(comp: &Computed) -> Vec<(f64, [[f64; 2]; 3])> {
    comp.interacting.iter().map(|l| (l.p, l.pauli.unwrap())).collect()
}
fn close(a: &[(f64, [[f64; 2]; 3])], b: &[(f64, [[f64; 2]; 3])], tol: f64) -> bool {
    a.len() == b.len()
        && a.iter().zip(b).all(|(x, y)| (x.0 - y.0).abs() <= tol && (0..3).all(|i| (0..2).all(|s| (x.1[i][s] - y.1[i][s]).abs() <= tol)))
}

#[test]
fn t4_t5_global_phase_and_scale() {
    let p4 = fixture("at1-p4-complex-state-ref-t1").spec;
    let (_c, base) = run(&p4);
    let i = super::at1::big::GQ::new(Q::zero(), qi(1));
    let mut ph = p4.clone();
    ph.psi = [p4.psi[0].mul(&i), p4.psi[1].mul(&i)];
    let (_c, phc) = run(&ph);
    let mut sc = p4.clone();
    sc.psi = [p4.psi[0].scale(&qi(2)), p4.psi[1].scale(&qi(2))];
    let (_c, scc) = run(&sc);
    assert!(close(&table(&base), &table(&phc), 1e-15));
    assert!(close(&table(&base), &table(&scc), 1e-15));
    assert_eq!(base.verdict_lines, phc.verdict_lines);
    assert_eq!(base.verdict_lines, scc.verdict_lines);
}

#[test]
fn t6_purity_of_every_defined_label() {
    for f in fixtures::all() {
        let (_c, comp) = run(&f.spec);
        for l in &comp.matrix.labels {
            if let Some(p) = l.pauli {
                let s: f64 = (0..3).map(|a| (2.0 * p[a][0] - 1.0).powi(2)).sum();
                assert!((s - 1.0).abs() <= 8e-12, "{}", f.name);
            }
        }
        for l in &comp.interacting {
            if let Some(p) = l.pauli {
                let s: f64 = (0..3).map(|a| (2.0 * p[a][0] - 1.0).powi(2)).sum();
                assert!((s - 1.0).abs() <= 8e-12, "{}", f.name);
            }
        }
    }
}

#[test]
fn t7_reference_shift_relabels() {
    let p4 = fixture("at1-p4-complex-state-ref-t1").spec; // r = 1
    let mut r0 = p4.clone();
    r0.ref_label = 0;
    let (_c, a) = run(&p4);
    let (_c, b) = run(&r0);
    let (ta, tb) = (table(&a), table(&b));
    for k in 0..4 {
        let shifted = [tb[(k + 3) % 4]];
        assert!(close(&[ta[k]], &shifted, 1e-15), "label {}", k);
    }
}

#[test]
fn t9_wall_clock_independence_and_record_shape() {
    let f = fixture("at1-kat-rotated-level-n4");
    let (c, a) = run(&f.spec);
    let (_c, b) = run(&f.spec);
    assert_eq!(a.values, b.values);
    assert_eq!(a.verdict_id, b.verdict_id);
    let r1 = String::from_utf8(result_bytes(&c, &a, &provenance("2026-10-10T00:00:01Z")).unwrap()).unwrap();
    let r2 = String::from_utf8(result_bytes(&c, &b, &provenance("2026-10-10T00:00:02Z")).unwrap()).unwrap();
    let l1: Vec<&str> = r1.lines().collect();
    let l2: Vec<&str> = r2.lines().collect();
    let vid = |l: &[&str]| l.iter().find(|x| x.starts_with("verdict_id ")).unwrap().to_string();
    let ed = |l: &[&str]| l.iter().find(|x| x.starts_with("evidence_digest ")).unwrap().to_string();
    assert_eq!(vid(&l1), vid(&l2));
    assert_ne!(ed(&l1), ed(&l2));
    // shape: header lines, copied blocks, numerics, values, verdict, provenance, digest, end
    assert_eq!(&l1[..5], ["OMEGA-AT1-RESULT v1", "domain omega.at1.result.v1", "contract AT1_RESULT_V1", "case_contract AT1_CASE_V1", "case_name at1-kat-rotated-level-n4"]);
    assert_eq!(*l1.last().unwrap(), "end");
    assert!(r1.ends_with("end\n") && !r1.contains('\r'));
    let at = |p: &str| l1.iter().position(|x| x.starts_with(p)).unwrap();
    assert!(at("begin semantic") < at("begin acceptance") && at("begin acceptance") < at("case_id ") && at("case_id ") < at("acceptance_id "));
    assert!(at("case_file_sha256 ") < at("begin numerics") && at("begin numerics") < at("begin values") && at("end values") < at("begin verdict"));
    assert!(at("verdict_id ") < at("begin provenance") && at("end provenance") < at("evidence_digest "));
    assert_eq!(l1[at("case_file_sha256 ")], format!("case_file_sha256 {}", KAT_FILE));
    let above: Vec<String> = l1[..at("evidence_digest ")].iter().map(|s| s.to_string()).collect();
    assert_eq!(ed(&l1), format!("evidence_digest {}", evidence_digest(&above)));
    // values block: 4 + 4 M + 6 M + 4 M + ... lines for M = 4, in contract order
    let m = 4;
    let nv = at("end values") - at("begin values") + 1;
    assert_eq!(nv, 2 + 3 + m + m + 6 * m + m + m + 6 * m + 6 * m);
}

#[test]
fn oracle_record_marking_is_enforced() {
    let f = fixture("at1-kat-rotated-level-n4");
    let (c, a) = run(&f.spec);
    let mut p = provenance("2026-10-10T00:00:01Z");
    p[8].1 = "rustc test".to_string();
    assert!(result_bytes(&c, &a, &p).is_err(), "build_cc must begin with oracle");
    let mut p = provenance("2026-10-10T00:00:01Z");
    p[4].1 = "ef".repeat(32);
    assert!(result_bytes(&c, &a, &p).is_err(), "engine_sha256 must equal oracle_sha256");
    let mut p = provenance("2026-10-10T00:00:01Z");
    p.swap(0, 1);
    assert!(result_bytes(&c, &a, &p).is_err(), "provenance order");
}

// ---------------------------------------------------------------- tokens

#[test]
fn bound_tokens_never_round_down() {
    for b in [1e-13, 3.3e-16, 2.5e-14, 1.0e-20, 7.77e-17, 0.5, 1.0, 1e18, 3.7e25, 1e300, f64::MAX] {
        let tok = bound_token(b);
        let exact = bound_exact(&tok).unwrap();
        assert!(exact >= Q::from_f64(b).unwrap(), "{} -> {}", b, tok);
    }
    assert_eq!(bound_token(0.0), "0@0");
    // non-finite estimates are written as the largest binary64, never below a finite value
    for b in [f64::INFINITY, f64::NAN] {
        assert!(bound_exact(&bound_token(b)).unwrap() >= Q::from_f64(f64::MAX).unwrap());
    }
    assert_eq!(value_exact("f64:3fd0000000000000").unwrap(), Some(q(1, 4)));
    assert_eq!(value_exact("undefined").unwrap(), None);
    assert!(value_exact("f64:3FD0000000000000").is_err());
}

// ---------------------------------------------------------------- review cases (PR body, findings 1 and 2)

fn review_case(name: &str, energies: Vec<Q>, hpauli: [Q; 4], v: Vec<[Q; 3]>, tau: Q, weight: Q, labels: usize) -> Spec {
    let mut s = fixture("at1-p1a-zero-coupling").spec;
    s.name = name.to_string();
    s.energies = energies;
    s.hpauli = hpauli;
    s.v = v;
    s.tau = tau;
    s.weight = weight;
    s.labels = labels;
    s
}

fn assert_review_pass(s: &Spec, kernel_dim: usize) {
    let (_c, comp) = run(s);
    let (val, bound) = comp.matrix.constraint.expect("nontrivial kernel");
    let (pv, pb) = comp.matrix.povm;
    eprintln!("{}: constraint {:e} bound {:e}; povm {:e} bound {:e}; outcome {} {:?}", s.name, val, bound, pv, pb, comp.verdict.outcome, comp.verdict.codes);
    assert_eq!(comp.matrix.kernel_dim, kernel_dim, "{}", s.name);
    assert!(val <= bound, "{}: constraint residual {:e} above its bound {:e}", s.name, val, bound);
    assert!(pv <= pb, "{}: povm residual {:e} above its bound {:e}", s.name, pv, pb);
    assert_eq!(comp.verdict.outcome, "PASS", "{}: {:?}", s.name, comp.verdict.codes);
}

#[test]
fn review_finding_1_large_field_cancelling_large_coupling() {
    // reviewer's case: h_z = 1048573/15 and v_j = (3/10, 0, -1048567/15) on both levels, so
    // n_j = (3/10, 0, 2/5), R_j = 1/2; E = (-1/2, 1/2) puts one kernel vector on each level
    let vj = [q(3, 10), Q::zero(), q(-1048567, 15)];
    let s = review_case("at1-review-f1-cancellation", vec![q(-1, 2), q(1, 2)], [Q::zero(), Q::zero(), Q::zero(), q(1048573, 15)], vec![vj.clone(), vj], q(1, 2), qi(1), 2);
    assert_review_pass(&s, 2);
}

#[test]
fn review_finding_2_large_clocks() {
    // P1-type kernel on E_j = j, h0 = -1/2, h_z = 1/2 (kernel vectors on levels 0 and 1)
    let h = [q(-1, 2), Q::zero(), Q::zero(), q(1, 2)];
    let e64: Vec<Q> = (0..64).map(qi).collect();
    let zero_v = vec![[Q::zero(), Q::zero(), Q::zero()]; 64];
    let s = review_case("at1-review-f2-n64-m64", e64.clone(), h.clone(), zero_v.clone(), q(1, 64), qi(1), 64);
    assert_review_pass(&s, 2);
    let s = review_case("at1-review-f2-n64-m256", e64, h, zero_v, q(1, 256), q(1, 4), 256);
    assert_review_pass(&s, 2);
}
