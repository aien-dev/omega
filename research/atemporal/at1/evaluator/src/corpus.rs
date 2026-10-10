//! The public AT-1 case corpus: one file per charter case class and per
//! AT1_SPEC section 13 row, generated deterministically (no shell edits, so it
//! regenerates byte-identically on Linux and macOS). Positive and negative
//! controls come from the generator; refusal controls are single-defect edits
//! of the worked example with identities recomputed (except identity rows).

use crate::case::Spec;
use crate::rat::{Cq, Q};
use crate::text::join_lf;

pub struct Row {
    pub path: String,
    pub class: String,
    /// `AT1_CASE_OK` or `AT1_CASE_REFUSED <code>`
    pub expect: String,
    pub bytes: Vec<u8>,
}

fn q(n: i64, d: i64) -> Q {
    Q::frac(n, d)
}
fn z() -> Q {
    Q::zero()
}
fn rho() -> [Q; 3] {
    [q(3, 10), z(), q(-1, 10)]
}
fn c(re: Q, im: Q) -> Cq {
    Cq::new(re, im)
}

/// The worked example `at1-kat-rotated-level-n4` (AT1_CASE_V1 section 6).
pub fn kat() -> Spec {
    let mut s = Spec::base("at1-kat-rotated-level-n4");
    s.v[2] = rho();
    s
}
fn p2(name: &str) -> Spec {
    let mut s = kat();
    s.name = name.into();
    s
}
fn p3(name: &str) -> Spec {
    let mut s = Spec::base(name);
    s.e = vec![q(-1, 2), q(1, 2), q(3, 2)];
    s.v = vec![rho(), [z(), q(3, 10), q(-1, 10)], [q(6, 5), z(), q(2, 5)]];
    s.w = q(3, 4);
    s
}
fn p3b(name: &str) -> Spec {
    let mut s = Spec::base(name);
    s.e = vec![q(-1, 2), z(), q(1, 2)];
    s.v = vec![[z(), z(), z()], [z(), z(), q(-1, 2)], rho()];
    s.psi = [c(Q::one(), z()), c(z(), Q::one())];
    s.tau = q(1, 2);
    s.w = q(3, 4);
    s
}
fn p4(name: &str) -> Spec {
    let mut s = p2(name);
    s.psi = [c(Q::one(), z()), c(q(3, 5), q(4, 5))];
    s.ref_label = "t1".into();
    s
}
fn p5(name: &str) -> Spec {
    let mut s = Spec::base(name);
    s.e = vec![q(-524289, 1048574), q(524285, 1048574)];
    s.h = [q(1, 524287), z(), z(), q(1, 2)];
    s.v = vec![[z(), z(), z()], [q(524175, 1048354), q(724, 524177), q(-1, 2)]];
    s.w = q(1, 2);
    s
}
fn p6(name: &str) -> Spec {
    let mut s = Spec::base(name);
    s.v = vec![[z(), z(), Q::one()], [z(), z(), q(1, 1048576)], rho(), [z(), z(), z()]];
    s
}
fn ideal_neg(mut s: Spec) -> Spec {
    s.target_ideal = true;
    s.negative(&["SCHRODINGER_DEVIATION_EXCEEDED"])
}

fn ok(path: &str, class: &str, s: &Spec) -> Row {
    Row { path: path.into(), class: class.into(), expect: "AT1_CASE_OK".into(), bytes: s.bytes() }
}

/// Valid cases: (path, class, spec).
pub fn valid_specs() -> Vec<(String, String, Spec)> {
    let mut v: Vec<(&str, &str, Spec)> = Vec::new();
    // ---- positive (expected PASS, codes none) ----
    let mut s = Spec::base("at1-p1a-zero-coupling-interacting");
    v.push(("positive/P1a-zero-coupling-interacting.case", "P1", s.clone()));
    s.name = "at1-p1b-zero-coupling-ideal".into();
    s.target_ideal = true;
    v.push(("positive/P1b-zero-coupling-ideal.case", "P1", s));
    let mut s = Spec::base("at1-p1c-spectator-coupling-ideal");
    s.v[0] = [z(), z(), q(1, 2)];
    s.v[3] = rho();
    s.target_ideal = true;
    v.push(("positive/P1c-spectator-coupling-ideal.case", "P1", s));
    v.push(("positive/P2-kat-rotated-level-n4.case", "P2", kat()));
    let mut s = p2("at1-p2-t4-global-phase-i");
    s.psi = [c(z(), Q::one()), c(z(), Q::one())];
    v.push(("positive/P2t4-global-phase-i.case", "P2", s));
    let mut s = p2("at1-p2-t5-scale-2");
    s.psi = [c(q(2, 1), z()), c(q(2, 1), z())];
    v.push(("positive/P2t5-scale-2.case", "P2", s));
    v.push(("positive/P3-every-level-coupled.case", "P3", p3("at1-p3-every-level-coupled")));
    v.push(("positive/P3b-degenerate-level.case", "P3", p3b("at1-p3b-degenerate-level")));
    v.push(("positive/P4-complex-psi-ref-t1.case", "P4", p4("at1-p4-complex-psi-ref-t1")));
    let mut s = p4("at1-p4-t7-ref-t0");
    s.ref_label = "t0".into();
    v.push(("positive/P4t7-complex-psi-ref-t0.case", "P4", s));
    v.push(("positive/P5-large-rationals.case", "P5", p5("at1-p5-large-rationals")));
    v.push(("positive/P6-near-miss-energies.case", "P6", p6("at1-p6-near-miss-energies")));
    // ---- negative (expected FAIL, exact code sets) ----
    v.push(("negative/N1-p2-target-ideal.case", "N1", ideal_neg(p2("at1-n1-p2-target-ideal"))));
    v.push(("negative/N1b-p3-target-ideal.case", "N1", ideal_neg(p3("at1-n1b-p3-target-ideal"))));
    v.push(("negative/N1c-p3b-target-ideal.case", "N1", ideal_neg(p3b("at1-n1c-p3b-target-ideal"))));
    v.push(("negative/N1d-p4-target-ideal.case", "N1", ideal_neg(p4("at1-n1d-p4-target-ideal"))));
    v.push(("negative/N1e-p5-target-ideal.case", "N1", ideal_neg(p5("at1-n1e-p5-target-ideal"))));
    v.push(("negative/N1f-p6-target-ideal.case", "N1", ideal_neg(p6("at1-n1f-p6-target-ideal"))));
    let mut s = Spec::base("at1-n2-every-level-out");
    s.v[1] = [z(), z(), q(1, 2)];
    s.v[2] = [z(), z(), q(1, 2)];
    v.push(("negative/N2-every-level-out.case", "N2", s.negative(&["TRIVIAL_PHYSICAL_STATE"])));
    let mut s = Spec::base("at1-n3-zero-label");
    s.v[2] = [z(), z(), q(-1, 1)];
    v.push(("negative/N3-zero-label.case", "N3", s.negative(&["CONDITIONAL_UNDEFINED"])));
    let mut s = p2("at1-n4a-wrong-weight");
    s.w = q(1, 2);
    v.push(("negative/N4a-wrong-weight.case", "N4", s.negative(&["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"])));
    let mut s = p2("at1-n4b-broken-clock-tau3");
    s.tau = q(1, 3);
    v.push(("negative/N4b-broken-clock-tau3.case", "N4", s.negative(&["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"])));
    let mut s = p2("at1-n5-precision-demand");
    s.min_bk = "RIGOROUS".into();
    v.push(("negative/N5-precision-demand.case", "N5", s.negative(&["BOUND_KIND_INSUFFICIENT"])));
    let mut s = Spec::base("at1-n6-zero-projection");
    s.v[1] = [z(), z(), q(1, 2)];
    s.v[2] = rho();
    s.psi = [c(q(3, 1), z()), c(Q::one(), z())];
    v.push(("negative/N6-zero-projection.case", "N6", s.negative(&["TRIVIAL_PHYSICAL_STATE"])));
    v.into_iter().map(|(a, b, s)| (a.to_string(), b.to_string(), s)).collect()
}

fn kat_lines() -> Vec<String> {
    kat().lines()
}

/// Edits the worked example line by line, then recomputes both identities.
fn edit(f: impl FnOnce(&mut Vec<String>)) -> Vec<u8> {
    let mut l = kat_lines();
    f(&mut l);
    crate::case::recompute_ids(&mut l);
    join_lf(&l)
}
/// Same without recomputing identities (identity rows and version rows that do not touch the blocks).
fn edit_raw(f: impl FnOnce(&mut Vec<String>)) -> Vec<u8> {
    let mut l = kat_lines();
    f(&mut l);
    join_lf(&l)
}
fn sub(l: &mut Vec<String>, from: &str, to: &str) {
    let i = l.iter().position(|x| x == from).unwrap_or_else(|| panic!("corpus edit: line `{}` not found", from));
    l[i] = to.to_string();
}
fn sub_prefix(l: &mut Vec<String>, prefix: &str, to: &str) {
    let i = l.iter().position(|x| x.starts_with(prefix)).unwrap_or_else(|| panic!("corpus edit: prefix `{}` not found", prefix));
    l[i] = to.to_string();
}
fn del(l: &mut Vec<String>, line: &str) {
    let i = l.iter().position(|x| x == line).unwrap_or_else(|| panic!("corpus edit: line `{}` not found", line));
    l.remove(i);
}
fn ins_after(l: &mut Vec<String>, after: &str, new: &str) {
    let i = l.iter().position(|x| x == after).unwrap_or_else(|| panic!("corpus edit: line `{}` not found", after));
    l.insert(i + 1, new.to_string());
}
fn flip_hex(l: &mut Vec<String>, key: &str) {
    let i = l.iter().position(|x| x.starts_with(key)).unwrap();
    let mut b = l[i].clone().into_bytes();
    let p = key.len();
    b[p] = if b[p] == b'0' { b'1' } else { b'0' };
    l[i] = String::from_utf8(b).unwrap();
}

pub const V1: &str = "interaction_pauli 1 0/1,0/1,0/1";
pub const V2: &str = "interaction_pauli 2 3/10,0/1,-1/10";
pub const V3: &str = "interaction_pauli 3 0/1,0/1,0/1";

pub fn refusal_rows() -> Vec<Row> {
    let mut v: Vec<(&str, &str, &str, Vec<u8>)> = Vec::new();
    let pe = "CASE_PARSE_ERROR";
    let nc = "CASE_NONCANONICAL";
    let uv = "CASE_UNSUPPORTED_VERSION";
    let ip = "CASE_INVALID_PARAMETER";
    let ir = "CASE_IRRATIONAL_SPECTRUM";
    let id = "CASE_ID_MISMATCH";
    // ---- AT1_SPEC 13.5 rows ----
    v.push(("R0a-line-removed", "R0", pe, edit(|l| del(l, "povm_weight 1/1"))));
    let mut b = Vec::new();
    for (i, s) in kat_lines().iter().enumerate() {
        b.extend_from_slice(s.as_bytes());
        if i == 6 {
            b.push(b'\r');
        }
        b.push(b'\n');
    }
    v.push(("R0b-one-crlf-line", "R0", pe, b));
    v.push(("R1-noncanonical-tau", "R1", nc, edit(|l| sub(l, "povm_tau_turns 1/4", "povm_tau_turns 2/8"))));
    v.push(("R2-header-v2", "R2", uv, edit_raw(|l| sub(l, "OMEGA-AT1-CASE v1", "OMEGA-AT1-CASE v2"))));
    v.push(("R2b-domain-v2", "R2", uv, edit_raw(|l| sub(l, "domain omega.at1.case.v1", "domain omega.at1.case.v2"))));
    v.push(("R2c-contract-v2", "R2", uv, edit_raw(|l| sub(l, "contract AT1_CASE_V1", "contract AT1_CASE_V2"))));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = (0..65).map(|i| Q::int(i)).collect();
    s.v = (0..65).map(|j| if j == 2 { rho() } else { [z(), z(), z()] }).collect();
    v.push(("R3-clock-dim-65", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = vec![q(-1, 2)];
    s.v = vec![[z(), z(), z()]];
    v.push(("R3b-clock-dim-1", "R3", ip, s.bytes()));
    v.push(("R4-irrational-level", "R4", ir, edit(|l| sub(l, V2, "interaction_pauli 2 1/2,0/1,-1/10"))));
    v.push(("R4b-irrational-h", "R4", ir, edit(|l| sub(l, "system_hamiltonian_pauli 0/1,0/1,0/1,1/2", "system_hamiltonian_pauli 0/1,1/1,0/1,1/1"))));
    v.push(("R5-case-id-digit", "R5", id, edit_raw(|l| flip_hex(l, "case_id "))));
    v.push(("R5b-acceptance-id-digit", "R5", id, edit_raw(|l| flip_hex(l, "acceptance_id "))));
    v.push(("R6-fail-without-codes", "R6", ip, edit(|l| sub(l, "expected_outcome PASS", "expected_outcome FAIL"))));
    v.push(("R6b-code-outside-set", "R6", ip, edit(|l| {
        sub(l, "control_kind POSITIVE", "control_kind NEGATIVE");
        sub(l, "expected_outcome PASS", "expected_outcome FAIL");
        sub(l, "expected_failure_codes none", "expected_failure_codes ORACLE_UNAVAILABLE");
    })));
    v.push(("R7-coupling-count", "R7", pe, edit(|l| del(l, V3))));
    v.push(("R8-coupling-order", "R8", pe, edit(|l| {
        let a = l.iter().position(|x| x == V1).unwrap();
        l.swap(a, a + 1);
    })));
    v.push(("R9-interaction-none", "R9", pe, edit(|l| sub(l, "interaction CLOCK_DIAGONAL_PAULI", "interaction NONE"))));
    v.push(("R10-at0-model-family", "R10", pe, edit(|l| sub(l, "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG", "model_family PAGE_WOOTTERS_FINITE_IDEAL"))));
    v.push(("R11-at0-constraint", "R11", pe, edit(|l| sub(l, "constraint SUM_HC_HS_V", "constraint SUM_HC_HS"))));
    v.push(("R12-target-both", "R12", pe, edit(|l| sub(l, "prediction_target INTERACTING", "prediction_target BOTH"))));
    v.push(("R12b-target-missing", "R12", pe, edit(|l| del(l, "prediction_target INTERACTING"))));
    v.push(("R13-noncanonical-coupling", "R13", nc, edit(|l| sub(l, V2, "interaction_pauli 2 6/20,0/1,-1/10"))));
    v.push(("R14-coupling-over-limit", "R14", ip, edit(|l| sub(l, V3, "interaction_pauli 3 0/1,0/1,1048577/1"))));
    // ---- AT-0 manifest rows re-expressed on P2 (AT1_SPEC 13.5 second table) ----
    v.push(("R01-noncanonical-rational", "R1", nc, edit(|l| sub(l, "povm_weight 1/1", "povm_weight 2/2"))));
    v.push(("R02-header-v2", "R2", uv, edit_raw(|l| sub(l, "OMEGA-AT1-CASE v1", "OMEGA-AT1-CASE v2"))));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = vec![q(-1, 2)];
    s.v = vec![[z(), z(), z()]];
    v.push(("R03-clock-dim-1", "R3", ip, s.bytes()));
    v.push(("R04-irrational-spectrum", "R4", ir, edit(|l| sub(l, "system_hamiltonian_pauli 0/1,0/1,0/1,1/2", "system_hamiltonian_pauli 0/1,1/1,0/1,1/1"))));
    v.push(("R05-wrong-case-id", "R5", id, edit_raw(|l| flip_hex(l, "case_id "))));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.codes = vec!["TRIVIAL_PHYSICAL_STATE".into()];
    v.push(("R06-codes-with-pass", "R6", ip, s.bytes()));
    let s = p2("at1-kat-rotated-level-n4").negative(&["PROBABILITY_SUM_EXCEEDED", "POVM_NORMALIZATION_EXCEEDED"]);
    v.push(("R06b-codes-unsorted", "R6", ip, s.bytes()));
    let s = p2("at1-kat-rotated-level-n4").negative(&["FOO_BAR"]);
    v.push(("R06c-codes-unknown", "R6", ip, s.bytes()));
    let s = p2("at1-kat-rotated-level-n4").negative(&[]);
    v.push(("R06d-fail-without-codes", "R6", ip, s.bytes()));
    let mut b = Vec::new();
    for s in kat_lines() {
        b.extend_from_slice(s.as_bytes());
        b.extend_from_slice(b"\r\n");
    }
    v.push(("R07-crlf", "R0", pe, b));
    v.push(("R08-tab", "R0", pe, edit(|l| sub(l, "clock_dim 4", "clock_dim\t4"))));
    v.push(("R09-blank-line", "R0", pe, edit(|l| ins_after(l, "begin semantic", ""))));
    v.push(("R10-trailing-space", "R0", pe, edit(|l| sub(l, "system_dim 2", "system_dim 2 "))));
    v.push(("R11-double-space", "R0", pe, edit(|l| sub(l, "system_dim 2", "system_dim  2"))));
    v.push(("R12-missing-key", "R0", pe, edit(|l| del(l, "interaction CLOCK_DIAGONAL_PAULI"))));
    v.push(("R13-unknown-key", "R0", pe, edit(|l| ins_after(l, "interaction CLOCK_DIAGONAL_PAULI", "foo bar"))));
    v.push(("R14-duplicate-key", "R0", pe, edit(|l| ins_after(l, "interaction CLOCK_DIAGONAL_PAULI", "interaction CLOCK_DIAGONAL_PAULI"))));
    v.push(("R15-out-of-order", "R0", pe, edit(|l| {
        del(l, "interaction CLOCK_DIAGONAL_PAULI");
        ins_after(l, "constraint SUM_HC_HS_V", "interaction CLOCK_DIAGONAL_PAULI");
    })));
    v.push(("R16-float-text", "R0", pe, edit(|l| sub(l, "povm_tau_turns 1/4", "povm_tau_turns 0.25"))));
    v.push(("R17-noncanonical-integer", "R1", nc, edit(|l| sub(l, "clock_dim 4", "clock_dim 04"))));
    v.push(("R18-noncanonical-scaled", "R1", nc, edit(|l| sub(l, "tol_probability 1@12", "tol_probability 10@13"))));
    v.push(("R19-label-uppercase", "R0", pe, edit(|l| {
        sub(l, "clock_label 0 t0", "clock_label 0 T0");
        sub(l, "reference_clock_label t0", "reference_clock_label T0");
    })));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = vec![q(-1, 2), q(-3, 2), q(1, 2), q(3, 2)];
    v.push(("R20-energies-not-increasing", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.psi = [c(z(), z()), c(z(), z())];
    v.push(("R21-psi-zero", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.tau = z();
    v.push(("R22-tau-zero", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.w = q(-1, 1);
    v.push(("R23-weight-negative", "R3", ip, s.bytes()));
    v.push(("R24-wrong-domain", "R2", uv, edit_raw(|l| sub(l, "domain omega.at1.case.v1", "domain omega.at1.case.v2"))));
    v.push(("R25-wrong-contract", "R2", uv, edit_raw(|l| sub(l, "contract AT1_CASE_V1", "contract AT1_CASE_V2"))));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = (0..65).map(|i| Q::int(i)).collect();
    s.v = (0..65).map(|j| if j == 2 { rho() } else { [z(), z(), z()] }).collect();
    v.push(("R26-clock-dim-65", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = vec![q(-1048577, 1), q(-1, 2), q(1, 2), q(3, 2)];
    v.push(("R27-rational-over-limit", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.labels = vec!["t0".into(), "t1".into(), "t1".into(), "t3".into()];
    v.push(("R28-duplicate-label", "R3", ip, s.bytes()));
    let mut s = p2("at1-kat-rotated-level-n4");
    s.ref_label = "t9".into();
    v.push(("R29-ref-label-missing", "R3", ip, s.bytes()));
    v.push(("R30-negative-zero", "R1", nc, edit(|l| sub(l, "system_hamiltonian_pauli 0/1,0/1,0/1,1/2", "system_hamiltonian_pauli -0/1,0/1,0/1,1/2"))));
    v.push(("R31-control-kind-bad", "R0", pe, edit(|l| sub(l, "control_kind POSITIVE", "control_kind MAYBE"))));
    v.push(("R32-case-name-two-tokens", "R0", pe, edit(|l| sub(l, "case_name at1-kat-rotated-level-n4", "case_name at1 kat"))));
    let mut b = kat().bytes();
    b.pop();
    v.push(("R33-missing-final-lf", "R0", pe, b));
    v.push(("R34-empty", "R0", pe, Vec::new()));
    v.push(("R35-trailing-line", "R0", pe, edit_raw(|l| l.push("extra".into()))));
    v.push(("R36-label-count-mismatch", "R0", pe, edit(|l| sub(l, "clock_label_count 4", "clock_label_count 5"))));
    v.push(("R37-model-family-bad", "R0", pe, edit(|l| sub(l, "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG", "model_family PAGE_WOOTTERS_INTERACTING"))));
    v.push(("R38-acceptance-id-wrong", "R5", id, edit_raw(|l| flip_hex(l, "acceptance_id "))));
    v.push(("R39-huge-digits-tau", "R3", ip, edit(|l| sub(l, "povm_tau_turns 1/4", "povm_tau_turns 340282366920938463463374607431768211457/4"))));
    // ---- charter section 8 readings d and e ----
    v.push(("X1-at0-header", "Rd", pe, edit_raw(|l| sub(l, "OMEGA-AT1-CASE v1", "OMEGA-AT0-CASE v1"))));
    v.push(("X2-at0-domain", "Rd", uv, edit_raw(|l| sub(l, "domain omega.at1.case.v1", "domain omega.at0.case.v1"))));
    v.push(("X3-coupling-index-01", "Re", nc, edit(|l| sub(l, V1, "interaction_pauli 01 0/1,0/1,0/1"))));
    v.push(("X4-coupling-index-a", "Re", pe, edit(|l| sub(l, V1, "interaction_pauli a 0/1,0/1,0/1"))));
    v.push(("X5-coupling-index-wrong", "Re", pe, edit(|l| sub(l, V1, "interaction_pauli 2 0/1,0/1,0/1"))));
    v.push(("X6-clock-label-index-01", "Re", nc, edit(|l| sub_prefix(l, "clock_label 1 ", "clock_label 01 t1"))));
    // review finding 5: `none` is the empty list, so clock_dim 0 with no energies is a range error
    let mut s = p2("at1-kat-rotated-level-n4");
    s.e = Vec::new();
    s.v = Vec::new();
    v.push(("X7-clock-dim-0-none", "R3", ip, s.bytes()));
    v.into_iter()
        .map(|(n, cl, code, b)| Row { path: format!("refuse/{}.case", n), class: cl.into(), expect: format!("AT1_CASE_REFUSED {}", code), bytes: b })
        .collect()
}

pub fn public_rows() -> Vec<Row> {
    let mut v: Vec<Row> = valid_specs().iter().map(|(p, c, s)| ok(p, c, s)).collect();
    v.extend(refusal_rows());
    v
}

/// Exact hand tables of AT1_SPEC section 13.2 (p, P(X+), P(Y+), P(Z+), ideal X+, Y+, Z+),
/// transcribed for the shadow self-test. Values are "n/d".
pub fn hand_tables() -> Vec<(&'static str, Vec<[&'static str; 7]>)> {
    let p1 = vec![
        ["1/4", "1", "1/2", "1/2", "1", "1/2", "1/2"],
        ["1/4", "1/2", "1", "1/2", "1/2", "1", "1/2"],
        ["1/4", "0", "1/2", "1/2", "0", "1/2", "1/2"],
        ["1/4", "1/2", "0", "1/2", "1/2", "0", "1/2"],
    ];
    vec![
        ("positive/P1a-zero-coupling-interacting.case", p1.clone()),
        ("positive/P1b-zero-coupling-ideal.case", p1.clone()),
        ("positive/P1c-spectator-coupling-ideal.case", p1),
        ("positive/P2-kat-rotated-level-n4.case", vec![
            ["5/28", "49/50", "1/2", "16/25", "1", "1/2", "1/2"],
            ["1/4", "29/70", "13/14", "26/35", "1/2", "1", "1/2"],
            ["9/28", "1/10", "1/2", "4/5", "0", "1/2", "1/2"],
            ["1/4", "29/70", "1/14", "26/35", "1/2", "0", "1/2"],
        ]),
        ("positive/P3-every-level-coupled.case", vec![
            ["107/280", "98/107", "65/214", "65/214", "1", "1/2", "1/2"],
            ["53/280", "37/53", "101/106", "61/106", "1/2", "1", "1/2"],
            ["5/56", "8/25", "37/50", "9/10", "0", "1/2", "1/2"],
            ["19/56", "37/95", "17/190", "29/38", "1/2", "0", "1/2"],
        ]),
        ("positive/P3b-degenerate-level.case", vec![
            ["41/80", "29/82", "40/41", "45/82", "1/2", "1", "1/2"],
            ["19/80", "1/38", "10/19", "25/38", "1/2", "0", "1/2"],
            ["1/80", "1/2", "0", "1/2", "1/2", "1", "1/2"],
            ["19/80", "37/38", "10/19", "13/38", "1/2", "0", "1/2"],
        ]),
        ("positive/P4-complex-psi-ref-t1.case", vec![
            ["29/164", "277/290", "17/58", "73/145", "9/10", "1/5", "1/2"],
            ["37/164", "197/370", "73/74", "113/185", "4/5", "9/10", "1/2"],
            ["53/164", "37/530", "65/106", "193/265", "1/10", "4/5", "1/2"],
            ["45/164", "13/50", "1/10", "17/25", "1/5", "1/10", "1/2"],
        ]),
        ("positive/P5-large-rationals.case", vec![
            ["524181/2096716", "274763624041/549527248074", "274004612845/549527248074", "524180/524181", "1", "1/2", "1/2"],
            ["522731/2096716", "274761527333/548007134774", "274004612845/548007134774", "522730/522731", "1/2", "1", "1/2"],
            ["524177/2096716", "274759430625/549523054658", "275520532729/549523054658", "524176/524177", "0", "1/2", "1/2"],
            ["525627/2096716", "274761527333/551043167958", "275520532729/551043167958", "525626/525627", "1/2", "0", "1/2"],
        ]),
        ("positive/P6-near-miss-energies.case", vec![
            ["5/28", "49/50", "1/2", "16/25", "1", "1/2", "1/2"],
            ["9/28", "1/10", "1/2", "4/5", "1/2", "1", "1/2"],
            ["5/28", "49/50", "1/2", "16/25", "0", "1/2", "1/2"],
            ["9/28", "1/10", "1/2", "4/5", "1/2", "0", "1/2"],
        ]),
    ]
}
