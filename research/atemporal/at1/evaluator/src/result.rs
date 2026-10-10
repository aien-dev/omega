//! AT1_RESULT_V1 strict parser and canonical writer (section 1 line order,
//! section 2 number tokens, section 6 identities).

use crate::case::{validate_lines, BoundKind, Case};
use crate::model::Status;
use crate::rat::{parse_int, parse_scaled, scaled_text, Scaled, Tok, Q};
use crate::sha256::tagged_hex;
use crate::text::{is_digest, is_hex40, is_path, is_repo, is_text200, is_utc, join_lf, lines_checked, FAILURE_CODES, RUN_ERROR_CODES};
use crate::big::Int;

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

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Kind {
    Value(u64),
    Nonfinite,
    Undefined,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RVal {
    pub kind: Kind,
    pub bound: Scaled,
}
impl RVal {
    pub fn undefined() -> RVal {
        RVal { kind: Kind::Undefined, bound: zero_scaled() }
    }
    pub fn of(x: f64, bound: &Scaled) -> RVal {
        if !x.is_finite() {
            return RVal { kind: Kind::Nonfinite, bound: bound.clone() };
        }
        let bits = if x == 0.0 { 0 } else { x.to_bits() };
        RVal { kind: Kind::Value(bits), bound: bound.clone() }
    }
    pub fn q(&self) -> Option<Q> {
        match self.kind {
            Kind::Value(b) => Some(Q::from_f64_bits(b)),
            _ => None,
        }
    }
    pub fn bq(&self) -> Q {
        Q::from_scaled(&self.bound)
    }
    pub fn text(&self) -> String {
        let v = match self.kind {
            Kind::Value(b) => format!("f64:{:016x}", b),
            Kind::Nonfinite => "nonfinite".into(),
            Kind::Undefined => "undefined".into(),
        };
        format!("{} {}", v, scaled_text(&self.bound))
    }
}
pub fn zero_scaled() -> Scaled {
    Scaled { n: Int::zero(), k: 0 }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ck {
    Pass,
    Fail,
    Indet,
    NotEval,
}
impl Ck {
    pub fn text(self) -> &'static str {
        match self {
            Ck::Pass => "PASS",
            Ck::Fail => "FAIL",
            Ck::Indet => "INDETERMINATE",
            Ck::NotEval => "NOT_EVALUATED",
        }
    }
    fn parse(s: &str) -> Option<Ck> {
        match s {
            "PASS" => Some(Ck::Pass),
            "FAIL" => Some(Ck::Fail),
            "INDETERMINATE" => Some(Ck::Indet),
            "NOT_EVALUATED" => Some(Ck::NotEval),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Verdict {
    pub checks: [Ck; 12],
    pub outcome: String,
    pub failure_codes: Vec<String>,
    pub error_code: String,
    pub expectation: String,
}
impl Verdict {
    pub fn lines(&self) -> Vec<String> {
        let mut v = vec!["begin verdict".to_string()];
        for i in 0..12 {
            v.push(format!("check {} {}", CHECK_NAMES[i], self.checks[i].text()));
        }
        v.push(format!("outcome {}", self.outcome));
        v.push(format!("failure_codes {}", if self.failure_codes.is_empty() { "none".to_string() } else { self.failure_codes.join(",") }));
        v.push(format!("error_code {}", self.error_code));
        v.push(format!("expectation_met {}", self.expectation));
        v.push("end verdict".into());
        v
    }
}

#[derive(Clone, Debug)]
pub struct Prov {
    pub source_repo: String,
    pub source_commit: String,
    pub source_tree_clean: String,
    pub contract_commit: String,
    pub engine_sha256: String,
    pub oracle_repo: String,
    pub oracle_commit: String,
    pub oracle_sha256: String,
    pub build_cc: String,
    pub build_flags: String,
    pub host: String,
    pub run_started: String,
    pub run_finished: String,
    pub artifacts: Vec<(String, String)>,
}

pub type Six = [[RVal; 2]; 3];

#[derive(Clone, Debug)]
pub struct ResultFile {
    pub case: Case,
    pub case_file_sha256: String,
    pub arithmetic: String,
    pub bound_kind: BoundKind,
    pub kernel_dim: i64,
    pub constraint: RVal,
    pub povm: RVal,
    pub label: Vec<Status>,
    pub clock: Vec<RVal>,
    pub pauli: Vec<Six>,
    pub ref_label: Vec<Status>,
    pub ref_clock: Vec<RVal>,
    pub ref_ideal: Vec<Six>,
    pub ref_inter: Vec<Six>,
    pub verdict: Verdict,
    pub verdict_id: String,
    pub prov: Prov,
    pub evidence_digest: String,
}

fn six_lines(key: &str, k: usize, s: &Six, out: &mut Vec<String>) {
    for a in 0..3 {
        for g in 0..2 {
            out.push(format!("{} {} {} {} {}", key, k, AXES[a], SIGNS[g], s[a][g].text()));
        }
    }
}

impl ResultFile {
    pub fn values_lines(&self) -> Vec<String> {
        let m = self.case.m;
        let mut v = vec!["begin values".to_string(), format!("physical_state_kernel_dim {}", self.kernel_dim)];
        v.push(format!("constraint_residual {}", self.constraint.text()));
        v.push(format!("povm_residual {}", self.povm.text()));
        for k in 0..m {
            v.push(format!("label {} {} {}", k, self.case.labels[k], self.label[k].text()));
        }
        for k in 0..m {
            v.push(format!("clock_probability {} {}", k, self.clock[k].text()));
        }
        for k in 0..m {
            six_lines("pauli", k, &self.pauli[k], &mut v);
        }
        for k in 0..m {
            v.push(format!("reference_label {} {} {}", k, self.case.labels[k], self.ref_label[k].text()));
        }
        for k in 0..m {
            v.push(format!("reference_clock_probability {} {}", k, self.ref_clock[k].text()));
        }
        for k in 0..m {
            six_lines("reference_ideal", k, &self.ref_ideal[k], &mut v);
        }
        for k in 0..m {
            six_lines("reference_interacting", k, &self.ref_inter[k], &mut v);
        }
        v.push("end values".into());
        v
    }
    pub fn compute_verdict_id(&self) -> String {
        let mut b = format!("case_id {}\nacceptance_id {}\n", self.case.case_id, self.case.acceptance_id).into_bytes();
        b.extend(join_lf(&self.verdict.lines()));
        tagged_hex(VERDICT_DOMAIN, &b)
    }
    /// Every line above evidence_digest.
    pub fn pre_evidence_lines(&self) -> Vec<String> {
        let mut v = vec![
            "OMEGA-AT1-RESULT v1".to_string(),
            "domain omega.at1.result.v1".into(),
            "contract AT1_RESULT_V1".into(),
            "case_contract AT1_CASE_V1".into(),
        ];
        v.extend(self.case.body_lines[..self.case.body_lines.len() - 2].iter().cloned());
        v.push(format!("case_id {}", self.case.case_id));
        v.push(format!("acceptance_id {}", self.case.acceptance_id));
        v.push(format!("case_file_sha256 {}", self.case_file_sha256));
        v.push("begin numerics".into());
        v.push(format!("arithmetic {}", self.arithmetic));
        v.push(format!("bound_kind {}", self.bound_kind.text()));
        v.push("threads 1".into());
        v.push("end numerics".into());
        v.extend(self.values_lines());
        v.extend(self.verdict.lines());
        v.push(format!("verdict_id {}", self.verdict_id));
        let p = &self.prov;
        v.push("begin provenance".into());
        v.push(format!("source_repo {}", p.source_repo));
        v.push(format!("source_commit {}", p.source_commit));
        v.push(format!("source_tree_clean {}", p.source_tree_clean));
        v.push(format!("contract_commit {}", p.contract_commit));
        v.push(format!("engine_sha256 {}", p.engine_sha256));
        v.push(format!("oracle_repo {}", p.oracle_repo));
        v.push(format!("oracle_commit {}", p.oracle_commit));
        v.push(format!("oracle_sha256 {}", p.oracle_sha256));
        v.push(format!("build_cc {}", p.build_cc));
        v.push(format!("build_flags {}", p.build_flags));
        v.push(format!("host {}", p.host));
        v.push(format!("run_started_utc {}", p.run_started));
        v.push(format!("run_finished_utc {}", p.run_finished));
        for (a, d) in &p.artifacts {
            v.push(format!("artifact {} {}", a, d));
        }
        v.push("end provenance".into());
        v
    }
    pub fn compute_evidence(&self) -> String {
        tagged_hex(EVIDENCE_DOMAIN, &join_lf(&self.pre_evidence_lines()))
    }
    /// Recomputes verdict_id and evidence_digest (writer role).
    pub fn seal(&mut self) {
        self.verdict_id = self.compute_verdict_id();
        self.evidence_digest = self.compute_evidence();
    }
    pub fn lines(&self) -> Vec<String> {
        let mut v = self.pre_evidence_lines();
        v.push(format!("evidence_digest {}", self.evidence_digest));
        v.push("end".into());
        v
    }
    pub fn bytes(&self) -> Vec<u8> {
        join_lf(&self.lines())
    }
}

struct Rd<'a> {
    l: &'a [String],
    i: usize,
}
type Pr<T> = Result<T, String>;
impl<'a> Rd<'a> {
    fn line(&mut self) -> Pr<&'a str> {
        let s = self.l.get(self.i).ok_or_else(|| format!("line {}: unexpected end of file", self.i + 1))?;
        self.i += 1;
        Ok(s)
    }
    fn exact(&mut self, t: &str) -> Pr<()> {
        let s = self.line()?;
        if s != t {
            return Err(format!("line {}: expected `{}`", self.i, t));
        }
        Ok(())
    }
    fn toks(&mut self, key: &str, n: usize) -> Pr<Vec<&'a str>> {
        let s = self.line()?;
        let t: Vec<&str> = s.split(' ').collect();
        if t[0] != key || t.len() != n + 1 {
            return Err(format!("line {}: expected `{}` with {} token(s)", self.i, key, n));
        }
        Ok(t[1..].to_vec())
    }
    /// `<key> <text>` where text may contain spaces.
    fn text(&mut self, key: &str) -> Pr<&'a str> {
        let s = self.line()?;
        let pre = format!("{} ", key);
        match s.strip_prefix(&pre) {
            Some(t) => Ok(t),
            None => Err(format!("line {}: expected `{}`", self.i, key)),
        }
    }
    fn err<T>(&self, m: &str) -> Pr<T> {
        Err(format!("line {}: {}", self.i, m))
    }
}

fn parse_val(rd: &Rd, v: &str, b: &str) -> Pr<RVal> {
    let kind = if v == "nonfinite" {
        Kind::Nonfinite
    } else if v == "undefined" {
        Kind::Undefined
    } else if let Some(h) = v.strip_prefix("f64:") {
        if h.len() != 16 || !h.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) {
            return rd.err("f64 token must be 16 lowercase hex digits");
        }
        let bits = u64::from_str_radix(h, 16).map_err(|_| "bad hex".to_string())?;
        if (bits >> 52) & 0x7ff == 0x7ff {
            return rd.err("non-finite bit pattern written as f64");
        }
        if bits == 0x8000_0000_0000_0000 {
            return rd.err("-0.0 must be written as f64:0000000000000000");
        }
        Kind::Value(bits)
    } else {
        return rd.err("value token is not f64:, nonfinite or undefined");
    };
    match parse_scaled(b) {
        Some((s, Tok::Ok)) => {
            if kind == Kind::Undefined && !s.n.is_zero() {
                return rd.err("undefined must carry bound 0@0");
            }
            Ok(RVal { kind, bound: s })
        }
        _ => rd.err("bound is not a canonical scaled decimal"),
    }
}

fn parse_status(rd: &Rd, s: &str) -> Pr<Status> {
    match Status::parse(s) {
        Some(x) => Ok(x),
        None => rd.err("label status outside its set"),
    }
}

fn parse_six(rd: &mut Rd, key: &str, k: usize) -> Pr<Six> {
    let mut out: Vec<RVal> = Vec::new();
    for a in 0..3 {
        for g in 0..2 {
            let t = rd.toks(key, 5)?;
            if t[0] != k.to_string() || t[1] != AXES[a] || t[2] != SIGNS[g] {
                return rd.err(&format!("{} line out of order (want {} {} {})", key, k, AXES[a], SIGNS[g]));
            }
            out.push(parse_val(rd, t[3], t[4])?);
        }
    }
    Ok([[out[0].clone(), out[1].clone()], [out[2].clone(), out[3].clone()], [out[4].clone(), out[5].clone()]])
}

/// Strict parse. Err carries a line-numbered reason (the evaluator reports E4-PARSE).
pub fn parse(bytes: &[u8]) -> Pr<ResultFile> {
    let lines = lines_checked(bytes)?;
    let mut rd = Rd { l: &lines, i: 0 };
    rd.exact("OMEGA-AT1-RESULT v1")?;
    rd.exact("domain omega.at1.result.v1")?;
    rd.exact("contract AT1_RESULT_V1")?;
    rd.exact("case_contract AT1_CASE_V1")?;
    let (case, end) = match validate_lines(&lines, 4, None) {
        Ok(x) => x,
        Err((code, m)) => return Err(format!("embedded case refused {}: {}", code.code(), m)),
    };
    rd.i = end;
    let sha = rd.toks("case_file_sha256", 1)?[0];
    if !is_digest(sha) {
        return rd.err("case_file_sha256 is not a digest");
    }
    rd.exact("begin numerics")?;
    let arith = rd.toks("arithmetic", 1)?[0];
    if !["BINARY64", "BINARY64_INTERVAL", "BINARY64_COMPENSATED", "NONE"].contains(&arith) {
        return rd.err("arithmetic outside its set");
    }
    let bk = match BoundKind::parse(rd.toks("bound_kind", 1)?[0]) {
        Some(b) => b,
        None => return rd.err("bound_kind outside its set"),
    };
    rd.exact("threads 1")?;
    rd.exact("end numerics")?;
    rd.exact("begin values")?;
    let kd = rd.toks("physical_state_kernel_dim", 1)?[0];
    let kernel_dim = match parse_int(kd) {
        Some((v, Tok::Ok)) if !v.is_neg() && v.bits() < 31 => v.low_u64() as i64,
        _ => return rd.err("physical_state_kernel_dim is not a canonical non-negative integer"),
    };
    let t = rd.toks("constraint_residual", 2)?;
    let constraint = parse_val(&rd, t[0], t[1])?;
    let t = rd.toks("povm_residual", 2)?;
    let povm = parse_val(&rd, t[0], t[1])?;
    let m = case.m;
    let mut label = Vec::new();
    for k in 0..m {
        let t = rd.toks("label", 3)?;
        if t[0] != k.to_string() || t[1] != case.labels[k] {
            return rd.err("label line index or label text differs from the case");
        }
        label.push(parse_status(&rd, t[2])?);
    }
    let mut clock = Vec::new();
    for k in 0..m {
        let t = rd.toks("clock_probability", 3)?;
        if t[0] != k.to_string() {
            return rd.err("clock_probability out of order");
        }
        clock.push(parse_val(&rd, t[1], t[2])?);
    }
    let mut pauli = Vec::new();
    for k in 0..m {
        pauli.push(parse_six(&mut rd, "pauli", k)?);
    }
    let mut ref_label = Vec::new();
    for k in 0..m {
        let t = rd.toks("reference_label", 3)?;
        if t[0] != k.to_string() || t[1] != case.labels[k] {
            return rd.err("reference_label line index or label text differs from the case");
        }
        ref_label.push(parse_status(&rd, t[2])?);
    }
    let mut ref_clock = Vec::new();
    for k in 0..m {
        let t = rd.toks("reference_clock_probability", 3)?;
        if t[0] != k.to_string() {
            return rd.err("reference_clock_probability out of order");
        }
        ref_clock.push(parse_val(&rd, t[1], t[2])?);
    }
    let mut ref_ideal = Vec::new();
    for k in 0..m {
        ref_ideal.push(parse_six(&mut rd, "reference_ideal", k)?);
    }
    let mut ref_inter = Vec::new();
    for k in 0..m {
        ref_inter.push(parse_six(&mut rd, "reference_interacting", k)?);
    }
    rd.exact("end values")?;
    rd.exact("begin verdict")?;
    let mut checks = [Ck::NotEval; 12];
    for i in 0..12 {
        let t = rd.toks("check", 2)?;
        if t[0] != CHECK_NAMES[i] {
            return rd.err(&format!("check {} expected in position {}", CHECK_NAMES[i], i + 1));
        }
        checks[i] = match Ck::parse(t[1]) {
            Some(c) => c,
            None => return rd.err("check status outside its set"),
        };
    }
    let outcome = rd.toks("outcome", 1)?[0].to_string();
    if !["PASS", "FAIL", "ERROR", "NOT_RUN"].contains(&outcome.as_str()) {
        return rd.err("outcome outside its set");
    }
    let fc = rd.toks("failure_codes", 1)?[0];
    let failure_codes: Vec<String> = if fc == "none" { Vec::new() } else { fc.split(',').map(|s| s.to_string()).collect() };
    for c in &failure_codes {
        if !FAILURE_CODES.contains(&c.as_str()) {
            return rd.err("failure code outside the closed set");
        }
    }
    for i in 1..failure_codes.len() {
        if failure_codes[i - 1].as_bytes() >= failure_codes[i].as_bytes() {
            return rd.err("failure_codes not sorted bytewise or not unique");
        }
    }
    let ec = rd.toks("error_code", 1)?[0].to_string();
    if ec != "none" && !RUN_ERROR_CODES.contains(&ec.as_str()) {
        return rd.err("error_code outside the closed set");
    }
    let em = rd.toks("expectation_met", 1)?[0].to_string();
    if !["YES", "NO", "NOT_APPLICABLE"].contains(&em.as_str()) {
        return rd.err("expectation_met outside its set");
    }
    rd.exact("end verdict")?;
    let vid = rd.toks("verdict_id", 1)?[0];
    if !is_digest(vid) {
        return rd.err("verdict_id is not a digest");
    }
    let placeholder_ok = outcome == "ERROR" || outcome == "NOT_RUN";
    rd.exact("begin provenance")?;
    let field = |rd: &mut Rd, key: &str, check: &dyn Fn(&str) -> bool, may_none: bool| -> Pr<String> {
        let t = rd.text(key)?;
        if (may_none && placeholder_ok && t == "none") || check(t) {
            Ok(t.to_string())
        } else {
            rd.err(&format!("{} grammar", key))
        }
    };
    let source_repo = field(&mut rd, "source_repo", &|s| is_repo(s), false)?;
    let source_commit = field(&mut rd, "source_commit", &|s| is_hex40(s), false)?;
    let source_tree_clean = field(&mut rd, "source_tree_clean", &|s| s == "YES" || s == "NO", false)?;
    let contract_commit = field(&mut rd, "contract_commit", &|s| is_hex40(s), false)?;
    let engine_sha256 = field(&mut rd, "engine_sha256", &|s| is_digest(s), true)?;
    let oracle_repo = field(&mut rd, "oracle_repo", &|s| is_repo(s), true)?;
    let oracle_commit = field(&mut rd, "oracle_commit", &|s| is_hex40(s), true)?;
    let oracle_sha256 = field(&mut rd, "oracle_sha256", &|s| is_digest(s), true)?;
    let build_cc = field(&mut rd, "build_cc", &|s| is_text200(s), true)?;
    let build_flags = field(&mut rd, "build_flags", &|s| is_text200(s), true)?;
    let host = field(&mut rd, "host", &|s| is_text200(s), true)?;
    let run_started = field(&mut rd, "run_started_utc", &|s| is_utc(s), true)?;
    let run_finished = field(&mut rd, "run_finished_utc", &|s| is_utc(s), true)?;
    let mut artifacts: Vec<(String, String)> = Vec::new();
    loop {
        let s = rd.line()?;
        if s == "end provenance" {
            break;
        }
        let t: Vec<&str> = s.split(' ').collect();
        if t.len() != 3 || t[0] != "artifact" || !is_path(t[1]) || !is_digest(t[2]) {
            return rd.err("expected `artifact <path> <digest>` or `end provenance`");
        }
        if let Some((prev, _)) = artifacts.last() {
            if prev.as_bytes() >= t[1].as_bytes() {
                return rd.err("artifact paths not unique and strictly increasing");
            }
        }
        artifacts.push((t[1].to_string(), t[2].to_string()));
    }
    let ed = rd.toks("evidence_digest", 1)?[0];
    if !is_digest(ed) {
        return rd.err("evidence_digest is not a digest");
    }
    rd.exact("end")?;
    if rd.i != lines.len() {
        return rd.err("content after `end`");
    }
    Ok(ResultFile {
        case,
        case_file_sha256: sha.to_string(),
        arithmetic: arith.to_string(),
        bound_kind: bk,
        kernel_dim,
        constraint,
        povm,
        label,
        clock,
        pauli,
        ref_label,
        ref_clock,
        ref_ideal,
        ref_inter,
        verdict: Verdict { checks, outcome, failure_codes, error_code: ec, expectation: em },
        verdict_id: vid.to_string(),
        prov: Prov {
            source_repo,
            source_commit,
            source_tree_clean,
            contract_commit,
            engine_sha256,
            oracle_repo,
            oracle_commit,
            oracle_sha256,
            build_cc,
            build_flags,
            host,
            run_started,
            run_finished,
            artifacts,
        },
        evidence_digest: ed.to_string(),
    })
}
