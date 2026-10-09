//! AT0_RESULT writer and exact checker.
//!
//! Result version: AT0_RESULT_V2 per Agent 0's ruling on omega#358 (comment
//! 6090404007, 2026-10-09): header `OMEGA-AT0-RESULT v2`, domain
//! `omega.at0.result.v2`, verdict tag `omega.at0.verdict.v2`, evidence tag
//! `omega.at0.evidence.v2`; `undefined` only for a trivial kernel. Case files
//! stay AT0_CASE_V1. Line shape, checks and codes otherwise follow
//! AT0_RESULT_V1 sections 1 to 7 at aien-architecture 044c9d1.
//! Every check is decided in exact arithmetic on the written tokens (exact.rs).

use super::case::{Case, Scaled, BOUND_KINDS};
use super::exact::Exact;
use super::matrix_path::MatrixValues;
use super::reference::RefValues;
use super::sha256::tagged_hex;

pub const RESULT_HEADER: &str = "OMEGA-AT0-RESULT v2";
pub const RESULT_DOMAIN: &str = "omega.at0.result.v2";
pub const RESULT_CONTRACT: &str = "AT0_RESULT_V2";
pub const VERDICT_DOMAIN: &str = "omega.at0.verdict.v2";
pub const EVIDENCE_DOMAIN: &str = "omega.at0.evidence.v2";
pub const CHECK_NAMES: [&str; 10] = [
    "bound_kind_sufficient",
    "values_finite",
    "physical_state_nontrivial",
    "constraint_residual",
    "povm_normalization",
    "clock_probability_sum",
    "probability_range",
    "pauli_pair_sum",
    "conditional_defined",
    "schrodinger_agreement",
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

// ---- tokens --------------------------------------------------------------------

pub fn f64_token(x: f64) -> String {
    if !x.is_finite() {
        return "nonfinite".to_string();
    }
    let x = if x == 0.0 { 0.0 } else { x }; // folds -0.0
    format!("f64:{:016x}", x.to_bits())
}

/// Exact value of a token; Ok(None) for `undefined` / `nonfinite`.
pub fn value_exact(tok: &str) -> Result<Option<Exact>, String> {
    if tok == "undefined" || tok == "nonfinite" {
        return Ok(None);
    }
    let hex = tok.strip_prefix("f64:").filter(|h| h.len() == 16).ok_or_else(|| format!("bad value token {}", tok))?;
    let bits = u64::from_str_radix(hex, 16).map_err(|_| format!("bad value token {}", tok))?;
    Exact::from_f64(f64::from_bits(bits)).map(Some).ok_or_else(|| "nonfinite bit pattern".to_string())
}

/// Scaled decimal >= b: twenty decimal places, rounded up, plus one unit of slack.
pub fn bound_token(b: f64) -> String {
    if b == 0.0 {
        return "0@0".to_string();
    }
    let n = (b * 1e20).ceil() as u128 + 1;
    Scaled::canonical(n, 20).text()
}

fn scaled_exact(s: &Scaled) -> Exact {
    Exact::from_scaled(s.n, s.k)
}

// ---- values block --------------------------------------------------------------

pub fn label_status(p: Option<&Exact>, b: &Exact, tol_zero: &Exact) -> &'static str {
    match p {
        None => "UNDEFINED",
        Some(p) => {
            if p.add(b).le(tol_zero) {
                "UNDEFINED"
            } else if p.sub(b).gt(tol_zero) {
                "DEFINED"
            } else {
                "INDETERMINATE"
            }
        }
    }
}

pub fn values_block(c: &Case, mat: &MatrixValues, refv: &[RefValues]) -> Vec<String> {
    let m = c.labels.len();
    let tol_zero = scaled_exact(&c.tol_zero);
    let btok = bound_token(mat.bound);
    let bex = Exact::from_f64(mat.bound).unwrap();
    let live = mat.live();
    let mut lines = vec!["begin values".to_string(), format!("physical_state_kernel_dim {}", mat.kernel_dim)];
    match (live, mat.constraint_residual) {
        (true, Some(r)) => lines.push(format!("constraint_residual {} {}", f64_token(r), btok)),
        _ => lines.push("constraint_residual undefined 0@0".to_string()),
    }
    lines.push(format!("povm_residual {} {}", f64_token(mat.povm_residual), btok));
    let mut statuses = Vec::with_capacity(m);
    for k in 0..m {
        let st = if live {
            let pe = Exact::from_f64(mat.clock_probability[k]);
            label_status(pe.as_ref(), &bex, &tol_zero)
        } else {
            "UNDEFINED"
        };
        statuses.push(st);
        lines.push(format!("label {} {} {}", k, c.labels[k], st));
    }
    for k in 0..m {
        if live {
            lines.push(format!("clock_probability {} {} {}", k, f64_token(mat.clock_probability[k]), btok));
        } else {
            lines.push(format!("clock_probability {} undefined 0@0", k));
        }
    }
    for k in 0..m {
        for (ax, axn) in AXES.iter().enumerate() {
            for (si, sgn) in SIGNS.iter().enumerate() {
                if !live {
                    lines.push(format!("pauli {} {} {} undefined 0@0", k, axn, sgn));
                } else {
                    // A numerically zero phi_k makes rho_k = 0/0: written as nonfinite, never invented.
                    let v = mat.pauli[k].map(|p| p[ax][si]).unwrap_or(f64::NAN);
                    lines.push(format!("pauli {} {} {} {} {}", k, axn, sgn, f64_token(v), btok));
                }
            }
        }
    }
    for (k, r) in refv.iter().enumerate() {
        let rb = bound_token(r.bound);
        for (ax, axn) in AXES.iter().enumerate() {
            for (si, sgn) in SIGNS.iter().enumerate() {
                lines.push(format!("reference {} {} {} {} {}", k, axn, sgn, f64_token(r.schrodinger[ax][si]), rb));
            }
        }
    }
    lines.push("end values".to_string());
    lines
}

// ---- parsed values (a verifier's view) -------------------------------------------

#[derive(Default)]
pub struct Values {
    pub kernel_dim: usize,
    pub nonfinite: bool,
    pub constraint_residual: Option<(Option<Exact>, Exact)>,
    pub povm_residual: Option<(Option<Exact>, Exact)>,
    pub label: Vec<(usize, String)>,
    pub clock_probability: Vec<(usize, Option<Exact>, Exact)>,
    pub pauli: Vec<(usize, usize, usize, Option<Exact>, Exact)>,
    pub reference: Vec<(usize, usize, usize, Option<Exact>, Exact)>,
}

fn parse_vb(tok: &str) -> Result<(Option<Exact>, Exact), String> {
    Ok((value_exact(tok)?, Exact::zero()))
}

/// Parse a values block back into exact numbers. Enforces the V2 rule that
/// `undefined` appears only when the kernel is trivial.
pub fn parse_values(lines: &[String]) -> Result<Values, String> {
    let mut v = Values::default();
    let scaled = |t: &str| -> Result<Exact, String> {
        let s = super::case::parse_scaled(t).map_err(|e| e.detail)?;
        Ok(scaled_exact(&s))
    };
    let axis = |t: &str| AXES.iter().position(|a| *a == t).ok_or_else(|| format!("axis {}", t));
    let sign = |t: &str| SIGNS.iter().position(|a| *a == t).ok_or_else(|| format!("sign {}", t));
    for ln in &lines[1..lines.len() - 1] {
        let t: Vec<&str> = ln.split(' ').collect();
        if t.contains(&"nonfinite") {
            v.nonfinite = true;
        }
        match t[0] {
            "physical_state_kernel_dim" => v.kernel_dim = t[1].parse().map_err(|_| "kernel_dim".to_string())?,
            "constraint_residual" => v.constraint_residual = Some((parse_vb(t[1])?.0, scaled(t[2])?)),
            "povm_residual" => v.povm_residual = Some((parse_vb(t[1])?.0, scaled(t[2])?)),
            "label" => v.label.push((t[1].parse().map_err(|_| "label".to_string())?, t[3].to_string())),
            "clock_probability" => v.clock_probability.push((t[1].parse().map_err(|_| "k".to_string())?, value_exact(t[2])?, scaled(t[3])?)),
            "pauli" => v.pauli.push((t[1].parse().map_err(|_| "k".to_string())?, axis(t[2])?, sign(t[3])?, value_exact(t[4])?, scaled(t[5])?)),
            "reference" => v.reference.push((t[1].parse().map_err(|_| "k".to_string())?, axis(t[2])?, sign(t[3])?, value_exact(t[4])?, scaled(t[5])?)),
            _ => return Err(format!("unknown values line {}", ln)),
        }
    }
    let trivial = v.kernel_dim == 0 || v.constraint_residual.as_ref().map(|r| r.0.is_none()).unwrap_or(true);
    let has_undef = lines.iter().any(|l| l.contains(" undefined ") || l.ends_with(" undefined"));
    if has_undef && !trivial {
        return Err("undefined token outside a trivial kernel".to_string());
    }
    if v.reference.iter().any(|r| r.3.is_none() && !v.nonfinite) {
        return Err("reference value undefined".to_string());
    }
    Ok(v)
}

// ---- checks ------------------------------------------------------------------------

fn abs_form(v: &Exact, b: &Exact, tol: &Exact) -> &'static str {
    if v.abs().add(b).le(tol) {
        "PASS"
    } else if v.abs().sub(b).gt(tol) {
        "FAIL"
    } else {
        "INDETERMINATE"
    }
}

fn signed_form(v: &Exact, b: &Exact, tol: &Exact) -> &'static str {
    if v.add(b).le(tol) {
        "PASS"
    } else if v.sub(b).gt(tol) {
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

pub struct Verdict {
    pub checks: Vec<(&'static str, &'static str)>,
    pub outcome: &'static str,
    pub codes: Vec<&'static str>,
}

/// Decide the ten checks of section 4 from the written values block.
pub fn judge(c: &Case, vlines: &[String], bound_kind: &str) -> Result<Verdict, String> {
    let v = parse_values(vlines)?;
    let (tol_c, tol_p, tol_pr, tol_s) = (scaled_exact(&c.tol_constraint), scaled_exact(&c.tol_povm), scaled_exact(&c.tol_probability), scaled_exact(&c.tol_schrodinger));
    let mut checks: Vec<(&'static str, &'static str)> = Vec::new();
    let mut codes: Vec<&'static str> = Vec::new();
    let set = |name: &'static str, res: &'static str, code: &'static str, checks: &mut Vec<(&'static str, &'static str)>, codes: &mut Vec<&'static str>| {
        checks.push((name, res));
        if res == "FAIL" && !codes.contains(&code) {
            codes.push(code);
        }
        if res == "INDETERMINATE" && !codes.contains(&"PRECISION_INSUFFICIENT") {
            codes.push("PRECISION_INSUFFICIENT");
        }
    };
    let bk = BOUND_KINDS.iter().position(|b| *b == bound_kind).unwrap_or(0);
    let mk = BOUND_KINDS.iter().position(|b| *b == c.min_bound_kind).unwrap_or(0);
    set("bound_kind_sufficient", if bk >= mk { "PASS" } else { "FAIL" }, "BOUND_KIND_INSUFFICIENT", &mut checks, &mut codes);
    set("values_finite", if v.nonfinite { "FAIL" } else { "PASS" }, "NONFINITE_VALUE", &mut checks, &mut codes);
    let cr = v.constraint_residual.as_ref().ok_or("missing constraint_residual")?;
    let live = v.kernel_dim >= 1 && cr.0.is_some();
    set("physical_state_nontrivial", if live { "PASS" } else { "FAIL" }, "TRIVIAL_PHYSICAL_STATE", &mut checks, &mut codes);
    set("constraint_residual", match &cr.0 { Some(x) => abs_form(x, &cr.1, &tol_c), None => "NOT_EVALUATED" }, "CONSTRAINT_RESIDUAL_EXCEEDED", &mut checks, &mut codes);
    let pr = v.povm_residual.as_ref().ok_or("missing povm_residual")?;
    set("povm_normalization", match &pr.0 { Some(x) => abs_form(x, &pr.1, &tol_p), None => "NOT_EVALUATED" }, "POVM_NORMALIZATION_EXCEEDED", &mut checks, &mut codes);
    let all_defined = !v.clock_probability.is_empty() && v.clock_probability.iter().all(|p| p.1.is_some());
    let sum_res = if all_defined {
        let mut s = Exact::from_int(-1);
        let mut sb = Exact::zero();
        for (_, p, b) in &v.clock_probability {
            s = s.add(p.as_ref().unwrap());
            sb = sb.add(b);
        }
        abs_form(&s, &sb, &tol_pr)
    } else {
        "NOT_EVALUATED"
    };
    set("clock_probability_sum", sum_res, "PROBABILITY_SUM_EXCEEDED", &mut checks, &mut codes);
    let mut written: Vec<(&Exact, &Exact)> = Vec::new();
    written.extend(v.clock_probability.iter().filter_map(|(_, p, b)| p.as_ref().map(|p| (p, b))));
    written.extend(v.pauli.iter().filter_map(|(_, _, _, p, b)| p.as_ref().map(|p| (p, b))));
    written.extend(v.reference.iter().filter_map(|(_, _, _, p, b)| p.as_ref().map(|p| (p, b))));
    let one = Exact::from_int(1);
    let mut rng = Vec::new();
    for (p, b) in &written {
        rng.push(signed_form(&p.neg(), b, &tol_pr));
        rng.push(signed_form(&p.sub(&one), b, &tol_pr));
    }
    set("probability_range", combine(&rng), "PROBABILITY_OUT_OF_RANGE", &mut checks, &mut codes);
    let defined: Vec<usize> = v.label.iter().filter(|(_, st)| st == "DEFINED").map(|(k, _)| *k).collect();
    let find = |list: &[(usize, usize, usize, Option<Exact>, Exact)], k: usize, ax: usize, si: usize| -> Option<(Option<Exact>, Exact)> {
        list.iter().find(|e| e.0 == k && e.1 == ax && e.2 == si).map(|e| (e.3.clone(), e.4.clone()))
    };
    let mut pairs = Vec::new();
    let mut agree = Vec::new();
    for &k in &defined {
        for ax in 0..3 {
            let (pp, pm) = (find(&v.pauli, k, ax, 0), find(&v.pauli, k, ax, 1));
            if let (Some((Some(a), ba)), Some((Some(b), bb))) = (pp, pm) {
                pairs.push(abs_form(&a.add(&b).sub(&one), &ba.add(&bb), &tol_pr));
            }
            for si in 0..2 {
                if let (Some((Some(a), ba)), Some((Some(r), br))) = (find(&v.pauli, k, ax, si), find(&v.reference, k, ax, si)) {
                    agree.push(abs_form(&a.sub(&r), &ba.add(&br), &tol_s));
                }
            }
        }
    }
    set("pauli_pair_sum", combine(&pairs), "PROBABILITY_SUM_EXCEEDED", &mut checks, &mut codes);
    let cond = if live { if v.label.iter().any(|(_, st)| st == "UNDEFINED") { "FAIL" } else { "PASS" } } else { "NOT_EVALUATED" };
    set("conditional_defined", cond, "CONDITIONAL_UNDEFINED", &mut checks, &mut codes);
    set("schrodinger_agreement", combine(&agree), "SCHRODINGER_DEVIATION_EXCEEDED", &mut checks, &mut codes);
    if v.label.iter().any(|(_, st)| st == "INDETERMINATE") && !codes.contains(&"PRECISION_INSUFFICIENT") {
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
    let mut exp: Vec<&str> = c.expected_codes.iter().map(String::as_str).collect();
    exp.sort();
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

pub struct Computed {
    pub matrix: MatrixValues,
    pub reference: Vec<RefValues>,
    pub values: Vec<String>,
    pub verdict: Verdict,
    pub verdict_lines: Vec<String>,
    pub verdict_id: String,
}

pub fn compute(c: &Case, dephased: bool) -> Computed {
    let matrix = super::matrix_path::evaluate(c, dephased);
    let reference = super::reference::evaluate(c);
    let values = values_block(c, &matrix, &reference);
    let verdict = judge(c, &values, "ESTIMATED").expect("own values block parses");
    let verdict_lines = verdict_block(c, &verdict);
    let verdict_id = verdict_id(c, &verdict_lines);
    Computed { matrix, reference, values, verdict, verdict_lines, verdict_id }
}

/// Full result record. `provenance` holds PROVENANCE_KEYS in order.
pub fn result_bytes(c: &Case, computed: &Computed, provenance: &[(&str, String)]) -> Result<Vec<u8>, String> {
    let mut lines = vec![RESULT_HEADER.to_string(), format!("domain {}", RESULT_DOMAIN), format!("contract {}", RESULT_CONTRACT), "case_contract AT0_CASE_V1".to_string(), format!("case_name {}", c.name)];
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
        return Err("provenance key count".to_string());
    }
    for ((key, val), want) in provenance.iter().zip(PROVENANCE_KEYS.iter()) {
        if key != want {
            return Err(format!("provenance order: {} vs {}", key, want));
        }
        let ok = !val.is_empty() && val.len() <= 200 && val.trim() == val && val.bytes().all(|b| (0x20..=0x7E).contains(&b));
        if !ok {
            return Err(format!("bad provenance text for {}", key));
        }
        lines.push(format!("{} {}", key, val));
    }
    lines.push("end provenance".to_string());
    let ed = evidence_digest(&lines);
    lines.push(format!("evidence_digest {}", ed));
    lines.push("end".to_string());
    Ok(lines.iter().map(|l| format!("{}\n", l)).collect::<String>().into_bytes())
}
