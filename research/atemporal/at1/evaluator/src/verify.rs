//! Verification of one AT1_RESULT_V1 file: structure, identities, binding to
//! the case, provenance, token discipline, exact re-derivation of the verdict,
//! and comparison of every engine and oracle value with the shadow model.
//! Status: FAIL if any FAIL finding, else INCONCLUSIVE if any INCONCLUSIVE
//! finding, else PASS. PASS means contract conformance and agreement with the
//! shadow within the case tolerances; it says nothing about physics.

use crate::case::{case_bytes_from_body, validate_bytes, BoundKind};
use crate::judge::{label_from, rederive};
use crate::model::{shadow, Mods, Shadow, Status, Val};
use crate::rat::Q;
use crate::result::{parse, Ck, Kind, RVal, ResultFile, Six, CHECK_NAMES, EVIDENCE_DOMAIN};
use crate::sha256::{sha256_hex, tagged_hex};
use crate::text::{join_lf, lines_checked};
use std::cmp::Ordering;

/// aien-architecture commits whose three contract files were verified by digest
/// (AT1_CASE_V1 e62018d8.., AT1_RESULT_V1 3e6efd7e.., AT0_RESULT_V2 bd0f9eb8..).
pub const CONTRACT_COMMITS: [&str; 3] = [
    "cbe4c8ed88d28bb96d5209327ecd30aa9cddaf7a",
    "81047f52850f4bd14c2fc5772ec2ac833ac694b9",
    "ea91d7c06507c8e43fdcef2b685f29d88a9e7d81",
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sev {
    Fail,
    Inconclusive,
}

#[derive(Clone, Debug)]
pub struct Finding {
    pub code: String,
    pub detail: String,
    pub sev: Sev,
}

#[derive(Clone, Debug, Default)]
pub struct Report {
    pub findings: Vec<Finding>,
    pub derived_outcome: String,
    pub derived_codes: Vec<String>,
    pub derived_expectation: String,
    pub any_indeterminate: bool,
}

impl Report {
    fn add(&mut self, sev: Sev, code: &str, detail: String) {
        // keep the report readable: at most 4 findings per code
        let n = self.findings.iter().filter(|f| f.code == code).count();
        if n < 4 {
            self.findings.push(Finding { code: code.into(), detail, sev });
        } else if n == 4 {
            self.findings.push(Finding { code: code.into(), detail: "(further findings with this code suppressed)".into(), sev });
        }
    }
    pub fn fail(&mut self, code: &str, detail: String) {
        self.add(Sev::Fail, code, detail);
    }
    pub fn inconclusive(&mut self, code: &str, detail: String) {
        self.add(Sev::Inconclusive, code, detail);
    }
    pub fn status(&self) -> &'static str {
        if self.findings.iter().any(|f| f.sev == Sev::Fail) {
            "FAIL"
        } else if !self.findings.is_empty() {
            "INCONCLUSIVE"
        } else {
            "PASS"
        }
    }
    pub fn codes(&self) -> Vec<String> {
        let mut v: Vec<String> = self.findings.iter().map(|f| f.code.clone()).collect();
        v.sort();
        v.dedup();
        v
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    /// a spliced candidate result (engine values + oracle references)
    Candidate,
    /// a record written by the oracle itself (G3)
    Oracle,
}

fn is_placeholder(r: &ResultFile) -> bool {
    r.verdict.outcome == "ERROR" || r.verdict.outcome == "NOT_RUN"
}

/// The file's own claim of a trivial kernel.
pub fn claims_trivial(r: &ResultFile) -> bool {
    r.kernel_dim == 0 || r.constraint.kind == Kind::Undefined
}

/// Tri-state agreement of a written value with the shadow: Some(true) when the
/// value is beyond its tolerance for certain, Some(false) when it is only beyond
/// its own bound, None when it agrees within its own bound.
fn compare(v: &RVal, s: &Val, tol: &Q) -> Option<bool> {
    let x = v.q()?;
    let d = x.sub(&s.q).abs();
    let b = v.bq().add(&s.bound());
    if d.sub(&b).cmp(tol) == Ordering::Greater {
        Some(true)
    } else if d.cmp(&b) == Ordering::Greater {
        Some(false)
    } else {
        None
    }
}

struct Cmp<'a> {
    rep: &'a mut Report,
    bk: BoundKind,
}
impl<'a> Cmp<'a> {
    fn one(&mut self, code: &str, what: String, v: &RVal, s: &Val, tol: &Q) {
        match compare(v, s, tol) {
            Some(true) => self.rep.fail(code, format!("{} = {:.12e}, shadow {:.12e} (beyond tolerance)", what, v.q().unwrap().to_f64(), s.q.to_f64())),
            Some(false) => self.bound_claim(&what, v, s),
            None => {}
        }
    }
    fn bound_claim(&mut self, what: &str, v: &RVal, s: &Val) {
        let msg = format!("{} = {:.17e} differs from shadow {:.17e} by more than its own bound {}", what, v.q().unwrap().to_f64(), s.q.to_f64(), crate::rat::scaled_text(&v.bound));
        match self.bk {
            BoundKind::Rigorous => self.rep.fail("E4-BOUND-CLAIM-FALSE", msg),
            BoundKind::Estimated => self.rep.inconclusive("E4-BOUND-ESTIMATE-LOW", msg),
            BoundKind::None => {}
        }
    }
    fn six(&mut self, code: &str, what: &str, k: usize, v: &Six, s: &[[Val; 2]; 3], tol: &Q) {
        let ax = ["X", "Y", "Z"];
        let sg = ["PLUS", "MINUS"];
        for a in 0..3 {
            for g in 0..2 {
                self.one(code, format!("{} {} {} {}", what, k, ax[a], sg[g]), &v[a][g], &s[a][g], tol);
            }
        }
    }
}

fn six_all(s: &Six, f: impl Fn(&RVal) -> bool) -> bool {
    s.iter().all(|a| a.iter().all(|v| f(v)))
}

/// Verifies one result file. `case_bytes`, when given, must be the case file the
/// result claims to come from. `sh_override` lets a caller reuse a shadow.
pub fn verify(bytes: &[u8], case_bytes: Option<&[u8]>, role: Role) -> Report {
    let mut rep = Report::default();
    let r = match parse(bytes) {
        Ok(r) => r,
        Err(m) => {
            rep.fail("E4-PARSE", m);
            return rep;
        }
    };
    let c = &r.case;
    // ---- identities ----
    let vid = r.compute_verdict_id();
    if vid != r.verdict_id {
        rep.fail("E4-VERDICT-ID", format!("verdict_id does not recompute (expected {})", vid));
    }
    let lines = lines_checked(bytes).unwrap_or_default();
    let ed = tagged_hex(EVIDENCE_DOMAIN, &join_lf(&lines[..lines.len().saturating_sub(2)]));
    if ed != r.evidence_digest {
        rep.fail("E4-EVIDENCE-DIGEST", format!("evidence_digest does not recompute (expected {})", ed));
    }
    // ---- binding: the case file is fully determined by the copied lines ----
    let rebuilt = case_bytes_from_body(&c.body_lines);
    let rsha = sha256_hex(&rebuilt);
    if rsha != r.case_file_sha256 {
        rep.fail("E4-CASE-SHA", format!("case_file_sha256 {} is not the digest {} of the copied case", r.case_file_sha256, rsha));
    }
    if let Some(cb) = case_bytes {
        match validate_bytes(cb) {
            Err((code, m)) => rep.fail("E4-CASE-BINDING", format!("supplied case file is refused: {} ({})", code.code(), m)),
            Ok(_) => {
                if cb != rebuilt.as_slice() {
                    rep.fail("E4-CASE-BINDING", "the copied case_name, blocks or identities differ from the supplied case file".into());
                }
            }
        }
    }
    // ---- provenance ----
    let p = &r.prov;
    if !CONTRACT_COMMITS.contains(&p.contract_commit.as_str()) {
        rep.inconclusive("E4-PROV-CONTRACT-UNKNOWN", format!("contract_commit {} is not in the verified allowlist (src/verify.rs)", p.contract_commit));
    }
    if p.source_tree_clean != "YES" {
        rep.inconclusive("E4-PROV-DIRTY-TREE", "source_tree_clean NO: not citable for a gate (AT0_RESULT_V2 section 7)".into());
    }
    let oracle_marked = p.build_cc.starts_with("oracle ") && p.engine_sha256 == p.oracle_sha256;
    match role {
        Role::Candidate => {
            if p.build_cc.starts_with("oracle ") {
                rep.fail("E4-PROV-ORACLE-RECORD", "an oracle-written record is never a candidate result (AT1_RESULT_V1 section 1)".into());
            } else if p.engine_sha256 == p.oracle_sha256 && p.engine_sha256 != "none" {
                rep.fail("E4-PROV-ORACLE-IS-ENGINE", "engine_sha256 equals oracle_sha256: the references did not come from a separate executable".into());
            }
        }
        Role::Oracle => {
            if !oracle_marked {
                rep.fail("E4-PROV-ORACLE-RECORD", "oracle record must have build_cc beginning `oracle ` and engine_sha256 equal to oracle_sha256".into());
            }
        }
    }
    if is_placeholder(&r) {
        let mut bad = r.kernel_dim != 0 || !r.verdict.failure_codes.is_empty() || r.verdict.expectation != "NOT_APPLICABLE";
        bad |= r.verdict.checks.iter().any(|c| *c != Ck::NotEval);
        bad |= crate::judge::all_values(&r).iter().any(|v| v.kind != Kind::Undefined);
        bad |= r.label.iter().chain(r.ref_label.iter()).any(|s| *s != Status::Undefined);
        bad |= (r.verdict.outcome == "ERROR") == (r.verdict.error_code == "none");
        if bad {
            rep.fail("E4-PLACEHOLDER-SHAPE", "ERROR/NOT_RUN result violates the shape of AT0_RESULT_V2 section 5".into());
        }
        rep.fail("E4-RUN-NOT-COMPLETED", format!("outcome {} error_code {}", r.verdict.outcome, r.verdict.error_code));
        return rep;
    }
    for (k, v) in [("engine_sha256", &p.engine_sha256), ("oracle_repo", &p.oracle_repo), ("oracle_commit", &p.oracle_commit), ("oracle_sha256", &p.oracle_sha256), ("build_cc", &p.build_cc), ("build_flags", &p.build_flags), ("host", &p.host), ("run_started_utc", &p.run_started), ("run_finished_utc", &p.run_finished)] {
        if v.as_str() == "none" {
            rep.fail("E4-PROV-NONE", format!("completed run carries `{} none`", k));
        }
    }
    if p.run_finished < p.run_started {
        rep.fail("E4-PROV-TIME-ORDER", format!("run_finished_utc {} precedes run_started_utc {}", p.run_finished, p.run_started));
    }
    if r.verdict.error_code != "none" {
        rep.fail("E4-UNDEFINED-DISCIPLINE", format!("completed run carries error_code {}", r.verdict.error_code));
    }
    // ---- shadow (always computable: AT-1 spectra are rational by rule 4) ----
    let sh: Shadow = shadow(c, &Mods::default(), false);
    let nontrivial = !sh.trivial;
    let tolz = Q::from_scaled(&c.tol_zero);
    // ---- token discipline (section 1) ----
    if nontrivial {
        let mut u = vec![];
        if r.constraint.kind == Kind::Undefined {
            u.push("constraint_residual".to_string());
        }
        if r.povm.kind == Kind::Undefined {
            u.push("povm_residual".into());
        }
        for k in 0..c.m {
            if r.clock[k].kind == Kind::Undefined {
                u.push(format!("clock_probability {}", k));
            }
            if r.ref_clock[k].kind == Kind::Undefined {
                u.push(format!("reference_clock_probability {}", k));
            }
            if !six_all(&r.ref_ideal[k], |v| v.kind != Kind::Undefined) {
                u.push(format!("reference_ideal {}", k));
            }
            let pd = r.label[k] == Status::Defined;
            if pd != six_all(&r.pauli[k], |v| v.kind != Kind::Undefined) || (!pd && !six_all(&r.pauli[k], |v| v.kind == Kind::Undefined)) {
                u.push(format!("pauli {} (undefined exactly when label is not DEFINED)", k));
            }
            let rd = r.ref_label[k] == Status::Defined;
            if rd != six_all(&r.ref_inter[k], |v| v.kind != Kind::Undefined) || (!rd && !six_all(&r.ref_inter[k], |v| v.kind == Kind::Undefined)) {
                u.push(format!("reference_interacting {} (undefined exactly when reference_label is not DEFINED)", k));
            }
        }
        if !u.is_empty() {
            rep.fail("E4-UNDEFINED-DISCIPLINE", format!("undefined-token rule of AT1_RESULT_V1 section 1 broken on: {}", u.join("; ")));
        }
    } else {
        let mut bad = r.constraint.kind != Kind::Undefined || r.povm.kind == Kind::Undefined;
        let mut obad = false;
        for k in 0..c.m {
            bad |= r.label[k] != Status::Undefined || r.clock[k].kind != Kind::Undefined;
            bad |= !six_all(&r.pauli[k], |v| v.kind == Kind::Undefined);
            obad |= r.ref_label[k] != Status::Undefined || r.ref_clock[k].kind != Kind::Undefined;
            obad |= !six_all(&r.ref_inter[k], |v| v.kind == Kind::Undefined) || !six_all(&r.ref_ideal[k], |v| v.kind != Kind::Undefined);
        }
        if bad {
            rep.fail("E4-TRIVIAL-SHAPE", "trivial kernel: labels UNDEFINED, clock_probability, pauli and constraint_residual undefined 0@0, povm_residual a value (AT1_RESULT_V1 section 1)".into());
        }
        if obad {
            rep.fail("E4-ORACLE-TRIVIAL", "trivial kernel: reference_label UNDEFINED, reference_clock_probability and reference_interacting undefined 0@0, reference_ideal values (AT1_RESULT_V1 section 1)".into());
        }
    }
    // ---- label rule: each status follows from its own clock value and bound ----
    for k in 0..c.m {
        if label_from(&r.clock[k], &tolz) != r.label[k] {
            rep.fail("E4-LABEL-RULE", format!("label {} is {} but its clock_probability gives {}", k, r.label[k].text(), label_from(&r.clock[k], &tolz).text()));
        }
        if label_from(&r.ref_clock[k], &tolz) != r.ref_label[k] {
            rep.fail("E4-LABEL-RULE", format!("reference_label {} is {} but its reference_clock_probability gives {}", k, r.ref_label[k].text(), label_from(&r.ref_clock[k], &tolz).text()));
        }
    }
    if r.bound_kind == BoundKind::None {
        let mut nz = !r.constraint.bound.n.is_zero() || !r.povm.bound.n.is_zero();
        for k in 0..c.m {
            nz |= !r.clock[k].bound.n.is_zero() || !six_all(&r.pauli[k], |v| v.bound.n.is_zero());
        }
        if nz {
            rep.fail("E4-BOUND-NONE-NONZERO", "bound_kind NONE but an engine line carries a nonzero bound".into());
        }
    }
    // ---- exact re-derivation of the verdict ----
    let d = rederive(&r, nontrivial);
    for i in 0..12 {
        if d.checks[i] != r.verdict.checks[i] {
            rep.fail("E4-CHECK-MISMATCH", format!("check {} reported {}, re-derived {}", CHECK_NAMES[i], r.verdict.checks[i].text(), d.checks[i].text()));
        }
    }
    if d.outcome != r.verdict.outcome {
        rep.fail("E4-OUTCOME-MISMATCH", format!("outcome reported {}, re-derived {}", r.verdict.outcome, d.outcome));
    }
    if d.failure_codes != r.verdict.failure_codes {
        rep.fail("E4-CODES-MISMATCH", format!("failure_codes reported {:?}, re-derived {:?}", r.verdict.failure_codes, d.failure_codes));
    }
    if d.expectation != r.verdict.expectation {
        rep.fail("E4-EXPECTATION-MISMATCH", format!("expectation_met reported {}, re-derived {}", r.verdict.expectation, d.expectation));
    }
    rep.any_indeterminate = d.checks.iter().any(|c| *c == Ck::Indet) || r.label.iter().any(|s| *s == Status::Indeterminate);
    if role == Role::Candidate && d.expectation != "YES" {
        let exp = if c.expected_codes.is_empty() { "none".to_string() } else { c.expected_codes.join(",") };
        let got = if d.failure_codes.is_empty() { "none".to_string() } else { d.failure_codes.join(",") };
        let msg = format!("re-derived outcome {} codes {} vs expected {} codes {}", d.outcome, got, if c.expected_pass { "PASS" } else { "FAIL" }, exp);
        let only_precision = d.failure_codes.iter().filter(|x| !c.expected_codes.contains(x)).all(|x| x == "PRECISION_INSUFFICIENT")
            && c.expected_codes.iter().all(|x| d.failure_codes.contains(x));
        if only_precision && rep.any_indeterminate {
            rep.inconclusive("E4-EXPECTATION-INDETERMINATE", msg);
        } else {
            rep.fail("E4-EXPECTATION-NOT-MET", msg);
        }
    }
    rep.derived_outcome = d.outcome.clone();
    rep.derived_codes = d.failure_codes.clone();
    rep.derived_expectation = d.expectation.clone();
    // ---- shadow agreement ----
    let tol_prob = Q::from_scaled(&c.tol_prob);
    let tol_schro = Q::from_scaled(&c.tol_schro);
    let tol_povm = Q::from_scaled(&c.tol_povm);
    let mut cm = Cmp { rep: &mut rep, bk: r.bound_kind };
    {
        if r.kernel_dim as usize != sh.kernel_dim {
            cm.rep.fail("E4-SHADOW-KERNEL-DIM", format!("physical_state_kernel_dim {}, exact value {}", r.kernel_dim, sh.kernel_dim));
        }
        if claims_trivial(&r) != sh.trivial {
            cm.rep.fail("E4-SHADOW-TRIVIAL", format!("the file {} a trivial kernel; the exact decision is {}", if claims_trivial(&r) { "claims" } else { "denies" }, if sh.trivial { "trivial" } else { "nontrivial" }));
        }
        if nontrivial {
            if let Some(x) = r.constraint.q() {
                // the exact residual of a vector built from exact kernel pairs is 0
                if x.abs().cmp(&r.constraint.bq()) == Ordering::Greater {
                    let z = Val::exact(Q::zero());
                    cm.bound_claim("constraint_residual", &r.constraint, &z);
                }
            }
        }
        cm.one("E4-SHADOW-POVM", "povm_residual".into(), &r.povm, &sh.povm_residual, &tol_povm);
        if nontrivial {
            for k in 0..c.m {
                cm.one("E4-SHADOW-CLOCK", format!("clock_probability {}", k), &r.clock[k], &sh.p[k], &tol_prob);
                let (es, ss) = (r.label[k], sh.status[k]);
                if es != Status::Indeterminate && ss != Status::Indeterminate && es != ss {
                    cm.rep.fail("E4-SHADOW-LABEL", format!("label {} is {}, exact status {}", k, es.text(), ss.text()));
                }
                if let Some(sp) = &sh.pauli[k] {
                    if es == Status::Defined {
                        cm.six("E4-SHADOW-PAULI", "pauli", k, &r.pauli[k], sp, &tol_schro);
                    }
                }
                // oracle families (gate G3 when the role is Oracle; always reported)
                cm.one("E4-ORACLE-CLOCK", format!("reference_clock_probability {}", k), &r.ref_clock[k], &sh.p[k], &tol_prob);
                let rs = r.ref_label[k];
                if rs != Status::Indeterminate && ss != Status::Indeterminate && rs != ss {
                    cm.rep.fail("E4-ORACLE-LABEL", format!("reference_label {} is {}, exact status {}", k, rs.text(), ss.text()));
                }
                if let Some(sp) = &sh.pauli[k] {
                    if rs == Status::Defined {
                        cm.six("E4-ORACLE-INTERACTING", "reference_interacting", k, &r.ref_inter[k], sp, &tol_schro);
                    }
                }
            }
        }
        for k in 0..c.m {
            cm.six("E4-ORACLE-IDEAL", "reference_ideal", k, &r.ref_ideal[k], &sh.ideal[k], &tol_schro);
        }
    }
    rep
}

/// Per-case candidate status for the table: PASS, FAIL or INDETERMINATE.
pub fn case_status(rep: &Report) -> &'static str {
    match rep.status() {
        "PASS" => "PASS",
        "FAIL" => "FAIL",
        _ => "INDETERMINATE",
    }
}
