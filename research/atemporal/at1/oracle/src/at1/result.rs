//! AT1_RESULT_V1 writer and exact judge. Contract: aien-architecture
//! docs/plans/atemporal/AT1_RESULT_V1.md, sha256 3e6efd7e..., frozen at
//! `case::CONTRACT_COMMIT`; AT0_RESULT_V2 (sha256 bd0f9eb8...) sections 2, 4, 5 and 7 are
//! normative by reference.
//!
//! The values block holds the matrix route as the record's own values and the four oracle
//! families (`reference_label`, `reference_clock_probability`, `reference_ideal`,
//! `reference_interacting`; charter reading (c)). The verdict is decided from the written
//! tokens only, in exact rational arithmetic: a binary64 token is an exact dyadic rational,
//! a scaled decimal an exact decimal rational, and the ideal marginal w/N an exact rational.

use super::big::{Nat, Q};
use super::case::{Case, BOUND_KINDS};
use super::closed::{self, IdealRef, LabelRef};
use super::matrix::{self, MatrixValues};
use super::model::{self, Levels};
use super::sha256::tagged_hex;

pub const RESULT_HEADER: &str = "OMEGA-AT1-RESULT v1";
pub const RESULT_DOMAIN: &str = "omega.at1.result.v1";
pub const RESULT_CONTRACT: &str = "AT1_RESULT_V1";
pub const VERDICT_DOMAIN: &str = "omega.at1.verdict.v1";
pub const EVIDENCE_DOMAIN: &str = "omega.at1.evidence.v1";
pub const CHECK_NAMES: [&str; 12] = [
    "bound_kind_sufficient",
    "values_finite",
    "physical_state_nontrivial",
    "constraint_residual",
    "povm_normalization",
    "clock_probability_sum",
    "probability_range",
    "pauli_pair_sum",
    "conditional_defined",
    "target_agreement",
    "oracle_cross_check",
    "label_status_agreement",
];
pub const AXES: [&str; 3] = ["X", "Y", "Z"];
pub const SIGNS: [&str; 2] = ["PLUS", "MINUS"];
pub const PROVENANCE_KEYS: [&str; 13] = [
    "source_repo",
    "source_commit",
    "source_tree_clean",
    "contract_commit",
    "engine_sha256",
    "oracle_repo",
    "oracle_commit",
    "oracle_sha256",
    "build_cc",
    "build_flags",
    "host",
    "run_started_utc",
    "run_finished_utc",
];

// ---- tokens -----------------------------------------------------------------------------

pub fn f64_token(x: f64) -> String {
    if !x.is_finite() {
        return "nonfinite".to_string();
    }
    let x = if x == 0.0 { 0.0 } else { x }; // -0.0 is written as +0.0
    format!("f64:{:016x}", x.to_bits())
}

/// Scaled decimal >= b: the exact binary64 value times 10^20, rounded up in exact integer
/// arithmetic, plus one unit of slack; the canonical form strips trailing zeros. A bound at
/// or above 1e18 (or not finite) is written as 1e18 plus one unit (never reached by a valid
/// case; it would only make every comparison indeterminate). Review finding 10: no float
/// multiply happens before the rounding.
pub fn bound_token(b: f64) -> String {
    if b == 0.0 {
        return "0@0".to_string();
    }
    // exact for every finite positive bound (no clip: a clipped bound would be written below
    // the estimate, confirmation review); a non-finite or negative estimate is written as
    // the largest binary64, which no finite residual can exceed by more than it
    let q = if b.is_finite() && b > 0.0 { Q::from_f64(b).expect("finite") } else { Q::from_f64(f64::MAX).expect("finite") };
    let e20 = Nat::from_decimal("100000000000000000000");
    let (mut n, r) = q.num.mag.mul(&e20).divrem(&q.den);
    if !r.is_zero() {
        n = n.add(&Nat::one());
    }
    n = n.add(&Nat::one());
    let mut digits = n.to_decimal();
    let mut k = 20u32;
    while k > 0 && digits.ends_with('0') {
        digits.pop();
        k -= 1;
    }
    format!("{}@{}", digits, k)
}

/// Exact value of a value token: Ok(Some) for `f64:`, Ok(None) for `undefined`/`nonfinite`.
pub fn value_exact(tok: &str) -> Result<Option<Q>, String> {
    if tok == "undefined" || tok == "nonfinite" {
        return Ok(None);
    }
    let hex = tok.strip_prefix("f64:").filter(|h| h.len() == 16 && h.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))).ok_or_else(|| format!("bad value token {}", tok))?;
    let bits = u64::from_str_radix(hex, 16).map_err(|_| format!("bad value token {}", tok))?;
    Q::from_f64(f64::from_bits(bits)).map(Some).ok_or_else(|| "non-finite bit pattern written as f64:".to_string())
}

pub fn bound_exact(tok: &str) -> Result<Q, String> {
    let (s, canonical) = super::case::parse_scaled(tok).map_err(|e| e.detail)?;
    if !canonical || s.k > 40 {
        return Err(format!("bad bound token {}", tok));
    }
    Ok(s.exact())
}

/// Label status rule (AT1_RESULT_V1 section 3) on exact written value and bound.
pub fn status(p: &Q, b: &Q, tol_zero: &Q) -> &'static str {
    if p.add_raw(b) <= *tol_zero {
        "UNDEFINED"
    } else if p.sub_raw(b) > *tol_zero {
        "DEFINED"
    } else {
        "INDETERMINATE"
    }
}

fn status_of(p: f64, bound_tok: &str, tol_zero: &Q) -> &'static str {
    match (Q::from_f64(p), bound_exact(bound_tok)) {
        (Some(pq), Ok(bq)) => status(&pq, &bq, tol_zero),
        _ => "INDETERMINATE", // a non-finite value cannot be judged defined
    }
}

// ---- values block -------------------------------------------------------------------------

pub struct Computed {
    pub levels: Levels,
    pub matrix: MatrixValues,
    pub interacting: Vec<LabelRef>,
    pub ideal: Vec<IdealRef>,
    pub values: Vec<String>,
    pub verdict: Verdict,
    pub verdict_lines: Vec<String>,
    pub verdict_id: String,
}

pub fn values_block(c: &Case, lv: &Levels, mat: &MatrixValues, inter: &[LabelRef], ideal: &[IdealRef]) -> Vec<String> {
    let m = c.labels.len();
    let tol_zero = c.tol_zero.exact();
    let mut out = vec!["begin values".to_string(), format!("physical_state_kernel_dim {}", lv.kernel_dim)];
    let trivial = lv.trivial;
    match (&mat.constraint, trivial) {
        (Some((v, b)), false) => out.push(format!("constraint_residual {} {}", f64_token(*v), bound_token(*b))),
        _ => out.push("constraint_residual undefined 0@0".to_string()),
    }
    out.push(format!("povm_residual {} {}", f64_token(mat.povm.0), bound_token(mat.povm.1)));
    let eng_status: Vec<&str> = (0..m).map(|k| if trivial { "UNDEFINED" } else { status_of(mat.labels[k].p, &bound_token(mat.labels[k].p_bound), &tol_zero) }).collect();
    let ref_status: Vec<&str> = (0..m).map(|k| if trivial { "UNDEFINED" } else { status_of(inter[k].p, &bound_token(inter[k].p_bound), &tol_zero) }).collect();
    for k in 0..m {
        out.push(format!("label {} {} {}", k, c.labels[k], eng_status[k]));
    }
    for k in 0..m {
        if trivial {
            out.push(format!("clock_probability {} undefined 0@0", k));
        } else {
            out.push(format!("clock_probability {} {} {}", k, f64_token(mat.labels[k].p), bound_token(mat.labels[k].p_bound)));
        }
    }
    let six = |out: &mut Vec<String>, key: &str, k: usize, vals: Option<[[f64; 2]; 3]>, bound: [[f64; 2]; 3], defined: bool| {
        for (a, an) in AXES.iter().enumerate() {
            for (s, sn) in SIGNS.iter().enumerate() {
                if !defined {
                    out.push(format!("{} {} {} {} undefined 0@0", key, k, an, sn));
                } else {
                    // a DEFINED label always has a state; a missing one is written as nonfinite, never invented
                    let v = vals.map(|p| p[a][s]).unwrap_or(f64::NAN);
                    out.push(format!("{} {} {} {} {} {}", key, k, an, sn, f64_token(v), bound_token(bound[a][s])));
                }
            }
        }
    };
    for k in 0..m {
        let (vals, b) = if trivial { (None, [[0.0; 2]; 3]) } else { (mat.labels[k].pauli, mat.labels[k].pauli_bound) };
        six(&mut out, "pauli", k, vals, b, eng_status[k] == "DEFINED");
    }
    for k in 0..m {
        out.push(format!("reference_label {} {} {}", k, c.labels[k], ref_status[k]));
    }
    for k in 0..m {
        if trivial {
            out.push(format!("reference_clock_probability {} undefined 0@0", k));
        } else {
            out.push(format!("reference_clock_probability {} {} {}", k, f64_token(inter[k].p), bound_token(inter[k].p_bound)));
        }
    }
    for (k, idl) in ideal.iter().enumerate() {
        six(&mut out, "reference_ideal", k, Some(idl.pauli), [[idl.bound; 2]; 3], true);
    }
    for k in 0..m {
        let (vals, b) = if trivial { (None, [[0.0; 2]; 3]) } else { (inter[k].pauli, inter[k].pauli_bound) };
        six(&mut out, "reference_interacting", k, vals, b, ref_status[k] == "DEFINED");
    }
    out.push("end values".to_string());
    out
}

// ---- parsed values (a verifier's view of the written block) -------------------------------

type VB = (Option<Q>, Q); // value (None for undefined or nonfinite), bound

#[derive(Default)]
pub struct Values {
    pub kernel_dim: usize,
    pub nonfinite: bool,
    pub constraint_undefined: bool,
    pub constraint: Option<VB>,
    pub povm: Option<VB>,
    pub label: Vec<String>,
    pub clock: Vec<VB>,
    pub pauli: Vec<Vec<VB>>, // [k][axis*2 + sign]
    pub ref_label: Vec<String>,
    pub ref_clock: Vec<VB>,
    pub ref_ideal: Vec<Vec<VB>>,
    pub ref_inter: Vec<Vec<VB>>,
}

const STATUSES: [&str; 3] = ["DEFINED", "UNDEFINED", "INDETERMINATE"];

/// Parse a values block for M labels, enforcing line order and the placement rules of
/// `undefined` (AT1_RESULT_V1 section 1).
pub fn parse_values(lines: &[String], c: &Case) -> Result<Values, String> {
    let m = c.labels.len();
    let mut r = Reader { toks: lines.iter().map(|l| l.split(' ').map(str::to_string).collect()).collect(), pos: 0, nonfinite: false };
    let mut v = Values::default();
    if r.next("begin")? != ["begin", "values"] {
        return Err("begin values".into());
    }
    let t = r.next("physical_state_kernel_dim")?;
    // canonical decimal only: `0` or `[1-9][0-9]*` (no sign, no leading zero)
    let kd = t.get(1).filter(|s| !s.is_empty() && s.len() <= 9 && s.bytes().all(|b| b.is_ascii_digit()) && (s.len() == 1 || !s.starts_with('0'))).ok_or("kernel_dim")?;
    v.kernel_dim = kd.parse().map_err(|_| "kernel_dim")?;
    let t = r.next("constraint_residual")?;
    v.constraint_undefined = t.get(1).map(|s| s == "undefined").unwrap_or(false);
    v.constraint = Some(r.vb(&t[1..])?);
    let t = r.next("povm_residual")?;
    if t.get(1).map(|s| s == "undefined").unwrap_or(false) {
        return Err("povm_residual is never undefined".into());
    }
    v.povm = Some(r.vb(&t[1..])?);
    let trivial = v.kernel_dim == 0 || v.constraint_undefined;
    v.label = r.labels("label", c, trivial)?;
    v.clock = r.clocks("clock_probability", m, trivial)?;
    let lab = v.label.clone();
    v.pauli = r.six("pauli", m, Some(&lab), trivial)?;
    v.ref_label = r.labels("reference_label", c, trivial)?;
    v.ref_clock = r.clocks("reference_clock_probability", m, trivial)?;
    v.ref_ideal = r.six("reference_ideal", m, None, trivial)?;
    let rl = v.ref_label.clone();
    v.ref_inter = r.six("reference_interacting", m, Some(&rl), trivial)?;
    if r.next("end")? != ["end", "values"] {
        return Err("end values".into());
    }
    if r.pos != r.toks.len() {
        return Err("lines after end values".into());
    }
    v.nonfinite = r.nonfinite;
    Ok(v)
}

struct Reader {
    toks: Vec<Vec<String>>,
    pos: usize,
    nonfinite: bool,
}

impl Reader {
    fn next(&mut self, want: &str) -> Result<Vec<String>, String> {
        let t = self.toks.get(self.pos).cloned().ok_or_else(|| format!("missing {}", want))?;
        self.pos += 1;
        if t[0] != want {
            return Err(format!("expected {}, got {:?}", want, t.join(" ")));
        }
        Ok(t)
    }
    fn vb(&mut self, t: &[String]) -> Result<VB, String> {
        if t.len() != 2 {
            return Err("value and bound".into());
        }
        if t[0] == "nonfinite" {
            self.nonfinite = true;
        }
        let val = value_exact(&t[0])?;
        let b = bound_exact(&t[1])?;
        if t[0] == "undefined" && !b.is_zero() {
            return Err("undefined with a nonzero bound".into());
        }
        Ok((val, b))
    }
    fn labels(&mut self, key: &str, c: &Case, trivial: bool) -> Result<Vec<String>, String> {
        let mut out = Vec::new();
        for k in 0..c.labels.len() {
            let t = self.next(key)?;
            if t.len() != 4 || t[1] != k.to_string() || t[2] != c.labels[k] || !STATUSES.contains(&t[3].as_str()) {
                return Err(format!("{} line {}", key, k));
            }
            if trivial && t[3] != "UNDEFINED" {
                return Err("trivial kernel requires UNDEFINED".into());
            }
            out.push(t[3].clone());
        }
        Ok(out)
    }
    fn clocks(&mut self, key: &str, m: usize, trivial: bool) -> Result<Vec<VB>, String> {
        let mut out = Vec::new();
        for k in 0..m {
            let t = self.next(key)?;
            if t.len() != 4 || t[1] != k.to_string() {
                return Err(format!("{} line {}", key, k));
            }
            if (t[2] == "undefined") != trivial {
                return Err(format!("{} {}: undefined exactly on a trivial kernel", key, k));
            }
            out.push(self.vb(&t[2..])?);
        }
        Ok(out)
    }
    /// `statuses` None: reference_ideal (always a value). Some: undefined exactly where the
    /// label is not DEFINED (always on a trivial kernel).
    fn six(&mut self, key: &str, m: usize, statuses: Option<&[String]>, trivial: bool) -> Result<Vec<Vec<VB>>, String> {
        let mut out = Vec::new();
        for k in 0..m {
            let mut row = Vec::new();
            for an in AXES {
                for sn in SIGNS {
                    let t = self.next(key)?;
                    if t.len() != 6 || t[1] != k.to_string() || t[2] != an || t[3] != sn {
                        return Err(format!("{} line {} {} {}", key, k, an, sn));
                    }
                    let must_undef = match statuses {
                        None => false,
                        Some(st) => trivial || st[k] != "DEFINED",
                    };
                    if (t[4] == "undefined") != must_undef {
                        return Err(format!("{} {} {} {}: misplaced or missing undefined", key, k, an, sn));
                    }
                    row.push(self.vb(&t[4..])?);
                }
            }
            out.push(row);
        }
        Ok(out)
    }
}

// ---- checks ------------------------------------------------------------------------------

fn abs_form(v: &Q, b: &Q, tol: &Q) -> &'static str {
    let a = v.abs();
    if a.add_raw(b) <= *tol {
        "PASS"
    } else if a.sub_raw(b) > *tol {
        "FAIL"
    } else {
        "INDETERMINATE"
    }
}

fn signed_form(v: &Q, b: &Q, tol: &Q) -> &'static str {
    if v.add_raw(b) <= *tol {
        "PASS"
    } else if v.sub_raw(b) > *tol {
        "FAIL"
    } else {
        "INDETERMINATE"
    }
}

fn combine(results: &[&'static str]) -> &'static str {
    if results.is_empty() {
        "NOT_EVALUATED"
    } else if results.contains(&"FAIL") {
        "FAIL"
    } else if results.contains(&"INDETERMINATE") {
        "INDETERMINATE"
    } else {
        "PASS"
    }
}

#[derive(Clone, Debug)]
pub struct Verdict {
    pub checks: Vec<(&'static str, &'static str)>,
    pub outcome: &'static str,
    pub codes: Vec<&'static str>,
}

/// Decide the twelve checks of AT1_RESULT_V1 section 4 from a written values block.
pub fn judge(c: &Case, vlines: &[String], bound_kind: &str) -> Result<Verdict, String> {
    let v = parse_values(vlines, c)?;
    let tol_c = c.tol_constraint.exact();
    let tol_p = c.tol_povm.exact();
    let tol_pr = c.tol_probability.exact();
    let tol_s = c.tol_schrodinger.exact();
    let ideal_target = c.prediction_target == "IDEAL";
    let mut checks: Vec<(&'static str, &'static str)> = Vec::new();
    let mut codes: Vec<&'static str> = Vec::new();
    let mut set = |name: &'static str, res: &'static str, code: &'static str| {
        checks.push((name, res));
        if res == "FAIL" && !codes.contains(&code) {
            codes.push(code);
        }
        if res == "INDETERMINATE" && !codes.contains(&"PRECISION_INSUFFICIENT") {
            codes.push("PRECISION_INSUFFICIENT");
        }
    };
    let bk = BOUND_KINDS.iter().position(|b| *b == bound_kind).ok_or("bound_kind")?;
    let mk = BOUND_KINDS.iter().position(|b| *b == c.min_bound_kind).ok_or("min_bound_kind")?;
    set(CHECK_NAMES[0], if bk >= mk { "PASS" } else { "FAIL" }, "BOUND_KIND_INSUFFICIENT");
    set(CHECK_NAMES[1], if v.nonfinite { "FAIL" } else { "PASS" }, "NONFINITE_VALUE");
    let live = v.kernel_dim >= 1 && !v.constraint_undefined;
    set(CHECK_NAMES[2], if live { "PASS" } else { "FAIL" }, "TRIVIAL_PHYSICAL_STATE");
    let cr = v.constraint.clone().unwrap();
    set(CHECK_NAMES[3], if !live { "NOT_EVALUATED" } else { cr.0.as_ref().map(|x| abs_form(x, &cr.1, &tol_c)).unwrap_or("NOT_EVALUATED") }, "CONSTRAINT_RESIDUAL_EXCEEDED");
    let pr = v.povm.clone().unwrap();
    set(CHECK_NAMES[4], pr.0.as_ref().map(|x| abs_form(x, &pr.1, &tol_p)).unwrap_or("NOT_EVALUATED"), "POVM_NORMALIZATION_EXCEEDED");
    if !live {
        for name in &CHECK_NAMES[5..] {
            set(name, "NOT_EVALUATED", "");
        }
    } else {
        // 6: sum of the clock marginal
        let sum = if v.clock.iter().all(|x| x.0.is_some()) {
            let mut s = Q::int(-1);
            let mut sb = Q::zero();
            for (p, b) in &v.clock {
                s = s.add_raw(p.as_ref().unwrap());
                sb = sb.add_raw(b);
            }
            abs_form(&s, &sb, &tol_pr)
        } else {
            "NOT_EVALUATED"
        };
        set(CHECK_NAMES[5], sum, "PROBABILITY_SUM_EXCEEDED");
        // 7: range of every written clock_probability and pauli value (reference lines excluded)
        let one = Q::one();
        let mut rng = Vec::new();
        for (p, b) in v.clock.iter().chain(v.pauli.iter().flatten()) {
            if let Some(p) = p {
                rng.push(signed_form(&p.neg(), b, &tol_pr));
                rng.push(signed_form(&p.sub_raw(&one), b, &tol_pr));
            }
        }
        set(CHECK_NAMES[6], combine(&rng), "PROBABILITY_OUT_OF_RANGE");
        // 8: PLUS + MINUS - 1 per defined label and axis
        let mut pairs = Vec::new();
        for (k, st) in v.label.iter().enumerate() {
            if st != "DEFINED" {
                continue;
            }
            for a in 0..3 {
                if let ((Some(pp), bp), (Some(pm), bm)) = (&v.pauli[k][2 * a], &v.pauli[k][2 * a + 1]) {
                    pairs.push(abs_form(&pp.add_raw(pm).sub_raw(&one), &bp.add_raw(bm), &tol_pr));
                }
            }
        }
        set(CHECK_NAMES[7], combine(&pairs), "PROBABILITY_SUM_EXCEEDED");
        // 9: no label UNDEFINED
        set(CHECK_NAMES[8], if v.label.iter().any(|s| s == "UNDEFINED") { "FAIL" } else { "PASS" }, "CONDITIONAL_UNDEFINED");
        // 10 and 11 on labels DEFINED on both sides
        let both: Vec<usize> = (0..c.labels.len()).filter(|&k| v.label[k] == "DEFINED" && v.ref_label[k] == "DEFINED").collect();
        let w_over_n = c.weight.div(&Q::int(c.clock_dim as i64));
        let compare = |k: usize, refs: &[Vec<VB>], clock_ref: Option<&VB>, out: &mut Vec<&'static str>| {
            for i in 0..6 {
                if let ((Some(p), bp), (Some(r), br)) = (&v.pauli[k][i], &refs[k][i]) {
                    out.push(abs_form(&p.sub_raw(r), &bp.add_raw(br), &tol_s));
                }
            }
            if let (Some(p), bp) = &v.clock[k] {
                match clock_ref {
                    None => out.push(abs_form(&p.sub_raw(&w_over_n), bp, &tol_pr)),
                    Some((Some(r), br)) => out.push(abs_form(&p.sub_raw(r), &bp.add_raw(br), &tol_pr)),
                    Some((None, _)) => {}
                }
            }
        };
        let mut agree = Vec::new();
        for &k in &both {
            if ideal_target {
                compare(k, &v.ref_ideal, None, &mut agree);
            } else {
                compare(k, &v.ref_inter, Some(&v.ref_clock[k]), &mut agree);
            }
        }
        set(CHECK_NAMES[9], combine(&agree), if ideal_target { "SCHRODINGER_DEVIATION_EXCEEDED" } else { "INTERACTING_DEVIATION_EXCEEDED" });
        let cross = if ideal_target {
            let mut cr = Vec::new();
            for &k in &both {
                compare(k, &v.ref_inter, Some(&v.ref_clock[k]), &mut cr);
            }
            combine(&cr)
        } else {
            "NOT_EVALUATED"
        };
        set(CHECK_NAMES[10], cross, "ORACLE_DISAGREEMENT");
        // 12: label status agreement
        let st: Vec<&'static str> = (0..c.labels.len())
            .map(|k| {
                let (a, b) = (v.label[k].as_str(), v.ref_label[k].as_str());
                if a == "INDETERMINATE" || b == "INDETERMINATE" {
                    "INDETERMINATE"
                } else if a != b {
                    "FAIL"
                } else {
                    "PASS"
                }
            })
            .collect();
        set(CHECK_NAMES[11], combine(&st), "ORACLE_DISAGREEMENT");
    }
    if v.label.iter().chain(v.ref_label.iter()).any(|s| s == "INDETERMINATE") && !codes.contains(&"PRECISION_INSUFFICIENT") {
        codes.push("PRECISION_INSUFFICIENT");
    }
    codes.sort();
    let outcome = if codes.is_empty() { "PASS" } else { "FAIL" };
    Ok(Verdict { checks, outcome, codes })
}

pub fn verdict_block(c: &Case, v: &Verdict) -> Vec<String> {
    let mut lines = vec!["begin verdict".to_string()];
    for (name, res) in &v.checks {
        lines.push(format!("check {} {}", name, res));
    }
    lines.push(format!("outcome {}", v.outcome));
    lines.push(format!("failure_codes {}", if v.codes.is_empty() { "none".to_string() } else { v.codes.join(",") }));
    lines.push("error_code none".to_string());
    let exp: Vec<&str> = c.expected_codes.iter().map(String::as_str).collect();
    let met = v.outcome == c.expected_outcome && exp == v.codes;
    lines.push(format!("expectation_met {}", if met { "YES" } else { "NO" }));
    lines.push("end verdict".to_string());
    lines
}

pub fn verdict_id(c: &Case, vlines: &[String]) -> String {
    let mut body = format!("case_id {}\nacceptance_id {}\n", c.case_id, c.acceptance_id);
    for l in vlines {
        body.push_str(l);
        body.push('\n');
    }
    tagged_hex(VERDICT_DOMAIN, body.as_bytes())
}

pub fn evidence_digest(lines_above: &[String]) -> String {
    let body: String = lines_above.iter().map(|l| format!("{}\n", l)).collect();
    tagged_hex(EVIDENCE_DOMAIN, body.as_bytes())
}

pub fn compute(c: &Case) -> Computed {
    let levels = model::levels(c);
    let matrix = matrix::evaluate(c, &levels);
    let interacting = if levels.trivial { Vec::new() } else { closed::interacting(c, &levels) };
    let ideal = closed::ideal(c);
    let values = values_block(c, &levels, &matrix, &interacting, &ideal);
    let verdict = judge(c, &values, "ESTIMATED").expect("own values block parses");
    let verdict_lines = verdict_block(c, &verdict);
    let verdict_id = verdict_id(c, &verdict_lines);
    Computed { levels, matrix, interacting, ideal, values, verdict, verdict_lines, verdict_id }
}

fn provenance_text_ok(val: &str) -> bool {
    !val.is_empty() && val.len() <= 200 && val.trim() == val && val.bytes().all(|b| (0x20..=0x7E).contains(&b))
}

/// Full AT1_RESULT_V1 record. `provenance` holds PROVENANCE_KEYS in order. The writer
/// refuses anything that is not marked as an oracle record (AT1_RESULT_V1 section 1:
/// `build_cc` begins with `oracle ` and `engine_sha256` equals `oracle_sha256`), and
/// refuses blocks that do not reproduce the copied identities.
pub fn result_bytes(c: &Case, computed: &Computed, provenance: &[(&str, String)]) -> Result<Vec<u8>, String> {
    if super::case::case_id(&c.semantic_block) != c.case_id || super::case::acceptance_id(&c.acceptance_block) != c.acceptance_id {
        return Err("blocks do not reproduce the copied identities".into());
    }
    let mut lines = vec![RESULT_HEADER.to_string(), format!("domain {}", RESULT_DOMAIN), format!("contract {}", RESULT_CONTRACT), "case_contract AT1_CASE_V1".to_string(), format!("case_name {}", c.name)];
    lines.extend(c.semantic_block.lines().map(str::to_string));
    lines.extend(c.acceptance_block.lines().map(str::to_string));
    lines.push(format!("case_id {}", c.case_id));
    lines.push(format!("acceptance_id {}", c.acceptance_id));
    lines.push(format!("case_file_sha256 {}", c.file_sha256));
    lines.extend(["begin numerics", "arithmetic BINARY64", "bound_kind ESTIMATED", "threads 1", "end numerics"].iter().map(|s| s.to_string()));
    lines.extend(computed.values.iter().cloned());
    lines.extend(computed.verdict_lines.iter().cloned());
    lines.push(format!("verdict_id {}", computed.verdict_id));
    lines.push("begin provenance".to_string());
    if provenance.len() != PROVENANCE_KEYS.len() {
        return Err("provenance key count".into());
    }
    let find = |k: &str| provenance.iter().find(|(key, _)| *key == k).map(|(_, v)| v.as_str());
    if find("engine_sha256") != find("oracle_sha256") {
        return Err("engine_sha256 must equal oracle_sha256 in an oracle-written record".into());
    }
    for ((key, val), want) in provenance.iter().zip(PROVENANCE_KEYS.iter()) {
        if key != want {
            return Err(format!("provenance order: {} vs {}", key, want));
        }
        if !provenance_text_ok(val) {
            return Err(format!("bad provenance text for {}", key));
        }
        if *key == "build_cc" && !val.starts_with("oracle ") {
            return Err("build_cc must begin with \"oracle \"".into());
        }
        lines.push(format!("{} {}", key, val));
    }
    lines.push("end provenance".to_string());
    let ed = evidence_digest(&lines);
    lines.push(format!("evidence_digest {}", ed));
    lines.push("end".to_string());
    Ok(lines.iter().map(|l| format!("{}\n", l)).collect::<String>().into_bytes())
}

