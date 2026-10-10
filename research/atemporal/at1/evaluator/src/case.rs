//! Independent AT1_CASE_V1 codec: byte rules, fixed line grammar, canonical
//! encodings and the six validity rules in contract order (section 4), with
//! arbitrary-precision exact arithmetic for rules 3 and 4 (no implementation
//! refusal, no wrap-around). Charter readings c, d, e are applied.

use crate::rat::{parse_crat, parse_int, parse_rat, parse_scaled, Cq, Scaled, Tok, Q};
use crate::sha256::{sha256_hex, tagged_hex};
use crate::text::{is_code, is_digest, is_label, is_name, join_lf, lines_checked, FAILURE_CODES};

pub const CASE_DOMAIN: &str = "omega.at1.case.v1";
pub const ACC_DOMAIN: &str = "omega.at1.acceptance.v1";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    ParseError,
    NonCanonical,
    UnsupportedVersion,
    InvalidParameter,
    IrrationalSpectrum,
    IdMismatch,
}

impl Refusal {
    pub fn code(self) -> &'static str {
        match self {
            Refusal::ParseError => "CASE_PARSE_ERROR",
            Refusal::NonCanonical => "CASE_NONCANONICAL",
            Refusal::UnsupportedVersion => "CASE_UNSUPPORTED_VERSION",
            Refusal::InvalidParameter => "CASE_INVALID_PARAMETER",
            Refusal::IrrationalSpectrum => "CASE_IRRATIONAL_SPECTRUM",
            Refusal::IdMismatch => "CASE_ID_MISMATCH",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum BoundKind {
    None = 0,
    Estimated = 1,
    Rigorous = 2,
}
impl BoundKind {
    pub fn parse(s: &str) -> Option<BoundKind> {
        match s {
            "NONE" => Some(BoundKind::None),
            "ESTIMATED" => Some(BoundKind::Estimated),
            "RIGOROUS" => Some(BoundKind::Rigorous),
            _ => None,
        }
    }
    pub fn text(self) -> &'static str {
        match self {
            BoundKind::None => "NONE",
            BoundKind::Estimated => "ESTIMATED",
            BoundKind::Rigorous => "RIGOROUS",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Case {
    pub name: String,
    pub n: usize,
    pub e: Vec<Q>,
    pub h0: Q,
    pub h: [Q; 3],
    pub v: Vec<[Q; 3]>,
    pub ref_label: String,
    pub ref_index: usize,
    pub psi: [Cq; 2],
    pub tau: Q,
    pub w: Q,
    pub m: usize,
    pub labels: Vec<String>,
    pub control_positive: bool,
    pub target_ideal: bool,
    pub expected_pass: bool,
    pub expected_codes: Vec<String>,
    pub min_bound_kind: BoundKind,
    pub tol_constraint: Scaled,
    pub tol_povm: Scaled,
    pub tol_prob: Scaled,
    pub tol_zero: Scaled,
    pub tol_schro: Scaled,
    pub case_id: String,
    pub acceptance_id: String,
    /// lines from case_name through acceptance_id (what a result copies)
    pub body_lines: Vec<String>,
    pub sem_lines: Vec<String>,
    pub acc_lines: Vec<String>,
    pub file_sha256: String,
    /// exact level norms |h + v_j| (rule 4 guarantees rational)
    pub level_r: Vec<Q>,
}

struct P<'a> {
    lines: &'a [String],
    i: usize,
    noncanon: Option<String>,
    bad_param: Option<String>,
    bad_version: Option<String>,
}

type R<T> = Result<T, (Refusal, String)>;

fn perr<T>(msg: String) -> R<T> {
    Err((Refusal::ParseError, msg))
}

impl<'a> P<'a> {
    fn next(&mut self) -> R<&'a str> {
        if self.i >= self.lines.len() {
            return perr("unexpected end of file".into());
        }
        let l = &self.lines[self.i];
        self.i += 1;
        Ok(l)
    }
    /// A `key value...` line with exactly `ntok` tokens after the key.
    fn key(&mut self, key: &str, ntok: usize) -> R<Vec<&'a str>> {
        let ln = self.i + 1;
        let l = self.next()?;
        let t: Vec<&str> = l.split(' ').collect();
        if t[0] != key {
            return perr(format!("line {}: expected key {}, found {}", ln, key, t[0]));
        }
        if t.len() != ntok + 1 {
            return perr(format!("line {}: {} takes {} token(s)", ln, key, ntok));
        }
        Ok(t[1..].to_vec())
    }
    fn exact(&mut self, text: &str) -> R<()> {
        let ln = self.i + 1;
        let l = self.next()?;
        if l != text {
            return perr(format!("line {}: expected `{}`", ln, text));
        }
        Ok(())
    }
    fn tok(&mut self, t: Tok, what: &str) {
        if t == Tok::NonCanonical && self.noncanon.is_none() {
            self.noncanon = Some(format!("line {}: non-canonical {}", self.i, what));
        }
    }
    fn param(&mut self, msg: String) {
        if self.bad_param.is_none() {
            self.bad_param = Some(msg);
        }
    }
    fn rat(&mut self, s: &str, what: &str) -> R<Q> {
        match parse_rat(s) {
            None => perr(format!("line {}: {} `{}` is not a rational", self.i, what, s)),
            Some((q, t)) => {
                self.tok(t, what);
                if !q.within_limits() {
                    self.param(format!("line {}: {} outside the token limits", self.i, what));
                }
                Ok(q)
            }
        }
    }
    fn list(&self, s: &str) -> R<Vec<String>> {
        let v: Vec<String> = s.split(',').map(|x| x.to_string()).collect();
        if v.iter().any(|x| x.is_empty()) {
            return perr(format!("line {}: empty list item", self.i));
        }
        Ok(v)
    }
    fn count(&mut self, s: &str, what: &str) -> R<i64> {
        match parse_int(s) {
            None => perr(format!("line {}: {} is not an integer", self.i, what)),
            Some((v, t)) => {
                self.tok(t, what);
                if v.is_neg() || v.bits() > 40 {
                    return perr(format!("line {}: {} cannot be a count", self.i, what));
                }
                Ok(v.low_u64() as i64)
            }
        }
    }
    fn index(&mut self, s: &str, pos: usize, what: &str) -> R<()> {
        // reading (e): parsed value compared with the position; canonicality after the shape pass
        match parse_int(s) {
            None => perr(format!("line {}: {} index `{}` is not an integer", self.i, what, s)),
            Some((v, t)) => {
                if v.is_neg() || v.bits() > 40 || v.low_u64() as usize != pos {
                    return perr(format!("line {}: {} index {} is not its position {}", self.i, what, s, pos));
                }
                self.tok(t, what);
                Ok(())
            }
        }
    }
    fn scaled(&mut self, key: &str) -> R<Scaled> {
        let t = self.key(key, 1)?;
        match parse_scaled(t[0]) {
            None => perr(format!("line {}: {} is not a scaled decimal", self.i, key)),
            Some((s, tk)) => {
                self.tok(tk, key);
                Ok(s)
            }
        }
    }
}

/// Parses the lines from `case_name` through `acceptance_id` (shape pass plus token
/// canonicality and range flags). Used for standalone files and inside results.
fn parse_body(p: &mut P) -> R<Case> {
    let body_start = p.i;
    let name = p.key("case_name", 1)?[0];
    if !is_name(name) {
        return perr("case_name grammar".into());
    }
    let sem_start = p.i;
    p.exact("begin semantic")?;
    p.exact("model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG")?;
    p.exact("energy_unit DIMENSIONLESS_HBAR_1")?;
    let t = p.key("clock_dim", 1)?[0];
    let n = p.count(t, "clock_dim")?;
    let et = p.key("clock_energies", 1)?;
    let items = p.list(et[0])?;
    if items.len() as i64 != n {
        return perr(format!("clock_energies has {} entries, clock_dim {}", items.len(), n));
    }
    let mut e = Vec::new();
    for it in &items {
        e.push(p.rat(it, "clock energy")?);
    }
    p.exact("system_dim 2")?;
    let ht = p.key("system_hamiltonian_pauli", 1)?;
    let hi = p.list(ht[0])?;
    if hi.len() != 4 {
        return perr("system_hamiltonian_pauli needs 4 rationals".into());
    }
    let mut hv = Vec::new();
    for it in &hi {
        hv.push(p.rat(it, "system_hamiltonian_pauli component")?);
    }
    p.exact("interaction CLOCK_DIAGONAL_PAULI")?;
    let mut v = Vec::new();
    for j in 0..n as usize {
        let t = p.key("interaction_pauli", 2)?;
        p.index(t[0], j, "interaction_pauli")?;
        let comps = p.list(t[1])?;
        if comps.len() != 3 {
            return perr("interaction_pauli needs 3 rationals".into());
        }
        let a = p.rat(&comps[0], "coupling component")?;
        let b = p.rat(&comps[1], "coupling component")?;
        let c = p.rat(&comps[2], "coupling component")?;
        v.push([a, b, c]);
    }
    p.exact("constraint SUM_HC_HS_V")?;
    p.exact("physical_state NULLSPACE_PROJECTION")?;
    let rl = p.key("reference_clock_label", 1)?[0];
    if !is_label(rl) {
        return perr("reference_clock_label grammar".into());
    }
    let st = p.key("reference_system_state", 1)?;
    let sv = p.list(st[0])?;
    if sv.len() != 2 {
        return perr("reference_system_state needs 2 complex rationals".into());
    }
    let mut psi = Vec::new();
    for it in &sv {
        match parse_crat(it) {
            None => return perr("reference_system_state component is not a complex rational".into()),
            Some((z, t)) => {
                p.tok(t, "reference_system_state");
                if !z.re.within_limits() || !z.im.within_limits() {
                    p.param("reference_system_state outside the token limits".into());
                }
                psi.push(z);
            }
        }
    }
    p.exact("clock_povm COVARIANT_DISCRETE")?;
    let tau = {
        let t = p.key("povm_tau_turns", 1)?;
        p.rat(t[0], "povm_tau_turns")?
    };
    let w = {
        let t = p.key("povm_weight", 1)?;
        p.rat(t[0], "povm_weight")?
    };
    let t = p.key("clock_label_count", 1)?[0];
    let m = p.count(t, "clock_label_count")?;
    let mut labels = Vec::new();
    for k in 0..m as usize {
        let t = p.key("clock_label", 2)?;
        p.index(t[0], k, "clock_label")?;
        if !is_label(t[1]) {
            return perr(format!("clock_label {} grammar", k));
        }
        labels.push(t[1].to_string());
    }
    p.exact("observables PAULI_X,PAULI_Y,PAULI_Z")?;
    p.exact("end semantic")?;
    let sem_end = p.i;
    p.exact("begin acceptance")?;
    let control_positive = match p.key("control_kind", 1)?[0] {
        "POSITIVE" => true,
        "NEGATIVE" => false,
        _ => return perr("control_kind outside its set".into()),
    };
    let target_ideal = match p.key("prediction_target", 1)?[0] {
        "IDEAL" => true,
        "INTERACTING" => false,
        _ => return perr("prediction_target outside its set".into()),
    };
    let expected_pass = match p.key("expected_outcome", 1)?[0] {
        "PASS" => true,
        "FAIL" => false,
        _ => return perr("expected_outcome outside its set".into()),
    };
    let ct = p.key("expected_failure_codes", 1)?[0];
    let expected_codes: Vec<String> = if ct == "none" {
        Vec::new()
    } else {
        let l = p.list(ct)?;
        if l.iter().any(|c| !is_code(c)) {
            return perr("expected_failure_codes token grammar".into());
        }
        l
    };
    let min_bound_kind = match BoundKind::parse(p.key("min_bound_kind", 1)?[0]) {
        Some(b) => b,
        None => return perr("min_bound_kind outside its set".into()),
    };
    let tol_constraint = p.scaled("tol_constraint_residual")?;
    let tol_povm = p.scaled("tol_povm_residual")?;
    let tol_prob = p.scaled("tol_probability")?;
    let tol_zero = p.scaled("tol_zero_probability")?;
    let tol_schro = p.scaled("tol_schrodinger")?;
    p.exact("end acceptance")?;
    let acc_end = p.i;
    let cid = p.key("case_id", 1)?[0];
    if !is_digest(cid) {
        return perr("case_id is not a digest".into());
    }
    let aid = p.key("acceptance_id", 1)?[0];
    if !is_digest(aid) {
        return perr("acceptance_id is not a digest".into());
    }
    let lines = p.lines;
    Ok(Case {
        name: name.to_string(),
        n: n as usize,
        e,
        h0: hv[0].clone(),
        h: [hv[1].clone(), hv[2].clone(), hv[3].clone()],
        v,
        ref_label: rl.to_string(),
        ref_index: 0,
        psi: [psi[0].clone(), psi[1].clone()],
        tau,
        w,
        m: m as usize,
        labels,
        control_positive,
        target_ideal,
        expected_pass,
        expected_codes,
        min_bound_kind,
        tol_constraint,
        tol_povm,
        tol_prob,
        tol_zero,
        tol_schro,
        case_id: cid.to_string(),
        acceptance_id: aid.to_string(),
        body_lines: lines[body_start..p.i].to_vec(),
        sem_lines: lines[sem_start..sem_end].to_vec(),
        acc_lines: lines[sem_end..acc_end].to_vec(),
        file_sha256: String::new(),
        level_r: Vec::new(),
    })
}

/// Rules 3 to 6 on a shape-valid, canonical, version-valid case.
fn rules_3_to_6(c: &mut Case, bad_param: Option<String>) -> R<()> {
    let ip = |m: String| Err((Refusal::InvalidParameter, m));
    if let Some(m) = bad_param {
        return ip(m);
    }
    if c.n < 2 || c.n > 64 {
        return ip(format!("clock_dim {} outside 2..64", c.n));
    }
    for j in 1..c.n {
        if c.e[j].cmp(&c.e[j - 1]) != std::cmp::Ordering::Greater {
            return ip("clock_energies not strictly increasing".into());
        }
    }
    if c.psi[0].is_zero() && c.psi[1].is_zero() {
        return ip("reference_system_state is zero".into());
    }
    if c.w.signum() <= 0 {
        return ip("povm_weight <= 0".into());
    }
    if c.tau.signum() <= 0 {
        return ip("povm_tau_turns <= 0".into());
    }
    if c.m < 1 || c.m > 256 {
        return ip(format!("clock_label_count {} outside 1..256", c.m));
    }
    let mut ref_index = None;
    for k in 0..c.m {
        if c.labels[..k].contains(&c.labels[k]) {
            return ip("duplicate clock label".into());
        }
        if c.labels[k] == c.ref_label {
            ref_index = Some(k);
        }
    }
    match ref_index {
        None => return ip("reference_clock_label not among the clock labels".into()),
        Some(r) => c.ref_index = r,
    }
    // rule 4, per level, exact
    let mut rs = Vec::new();
    for j in 0..c.n {
        let nx = c.h[0].add(&c.v[j][0]);
        let ny = c.h[1].add(&c.v[j][1]);
        let nz = c.h[2].add(&c.v[j][2]);
        let r2 = nx.mul(&nx).add(&ny.mul(&ny)).add(&nz.mul(&nz));
        match r2.sqrt_exact() {
            Some(r) => rs.push(r),
            None => return Err((Refusal::IrrationalSpectrum, format!("level {}: |h + v_j|^2 = {} is not a rational square", j, r2.text()))),
        }
    }
    c.level_r = rs;
    // rule 5
    let cid = tagged_hex(CASE_DOMAIN, &join_lf(&c.sem_lines));
    let aid = tagged_hex(ACC_DOMAIN, &join_lf(&c.acc_lines));
    if cid != c.case_id {
        return Err((Refusal::IdMismatch, format!("case_id recomputes to {}", cid)));
    }
    if aid != c.acceptance_id {
        return Err((Refusal::IdMismatch, format!("acceptance_id recomputes to {}", aid)));
    }
    // rule 6
    if c.expected_pass {
        if !c.expected_codes.is_empty() {
            return ip("expected_failure_codes must be none when expected_outcome PASS".into());
        }
    } else {
        if c.expected_codes.is_empty() {
            return ip("expected_failure_codes empty with expected_outcome FAIL".into());
        }
        for i in 1..c.expected_codes.len() {
            if c.expected_codes[i - 1].as_bytes() >= c.expected_codes[i].as_bytes() {
                return ip("expected_failure_codes not sorted or not unique".into());
            }
        }
        for code in &c.expected_codes {
            if !FAILURE_CODES.contains(&code.as_str()) {
                return ip(format!("{} is not an AT1_RESULT_V1 failure code", code));
            }
        }
    }
    Ok(())
}

/// Parses and validates the lines of a body (case_name .. acceptance_id) that
/// sit after the three version lines (already checked by the caller).
pub fn validate_lines(lines: &[String], start: usize, bad_version: Option<String>) -> R<(Case, usize)> {
    let mut p = P { lines, i: start, noncanon: None, bad_param: None, bad_version };
    let mut c = parse_body(&mut p)?;
    let end = p.i;
    if let Some(m) = p.noncanon.take() {
        return Err((Refusal::NonCanonical, m));
    }
    if let Some(m) = p.bad_version.take() {
        return Err((Refusal::UnsupportedVersion, m));
    }
    rules_3_to_6(&mut c, p.bad_param.take())?;
    Ok((c, end))
}

/// Full case file validation in contract order.
pub fn validate_bytes(bytes: &[u8]) -> R<Case> {
    let lines = lines_checked(bytes).map_err(|m| (Refusal::ParseError, m))?;
    let mut bad_version = None;
    // reading (d): first token is the key (shape, step 1); the rest is the value (step 2)
    let ver = [("OMEGA-AT1-CASE", "v1"), ("domain", CASE_DOMAIN), ("contract", "AT1_CASE_V1")];
    for (i, (k, val)) in ver.iter().enumerate() {
        let l = lines.get(i).ok_or((Refusal::ParseError, "file too short".to_string()))?;
        let t: Vec<&str> = l.split(' ').collect();
        if t[0] != *k || t.len() != 2 {
            return Err((Refusal::ParseError, format!("line {}: expected version line with key {}", i + 1, k)));
        }
        if t[1] != *val && bad_version.is_none() {
            bad_version = Some(format!("line {}: {} {} is not {}", i + 1, k, t[1], val));
        }
    }
    // shape of the remainder must be checked before the version value is reported
    let mut p = P { lines: &lines, i: 3, noncanon: None, bad_param: None, bad_version };
    let mut c = parse_body(&mut p)?;
    p.exact("end")?;
    if p.i != lines.len() {
        return perr(format!("line {}: content after `end`", p.i + 1));
    }
    if let Some(m) = p.noncanon.take() {
        return Err((Refusal::NonCanonical, m));
    }
    if let Some(m) = p.bad_version.take() {
        return Err((Refusal::UnsupportedVersion, m));
    }
    rules_3_to_6(&mut c, p.bad_param.take())?;
    c.file_sha256 = sha256_hex(bytes);
    Ok(c)
}

/// Rebuilds the exact bytes of the case file a result was made from (the result
/// copies every line except the three version lines and `end`).
pub fn case_bytes_from_body(body: &[String]) -> Vec<u8> {
    let mut lines = vec!["OMEGA-AT1-CASE v1".to_string(), format!("domain {}", CASE_DOMAIN), "contract AT1_CASE_V1".to_string()];
    lines.extend_from_slice(body);
    lines.push("end".into());
    join_lf(&lines)
}

/// Case generator: canonical text with both identities computed.
#[derive(Clone, Debug)]
pub struct Spec {
    pub name: String,
    pub e: Vec<Q>,
    pub h: [Q; 4],
    pub v: Vec<[Q; 3]>,
    pub ref_label: String,
    pub psi: [Cq; 2],
    pub tau: Q,
    pub w: Q,
    pub labels: Vec<String>,
    pub control_positive: bool,
    pub target_ideal: bool,
    pub expected_pass: bool,
    pub codes: Vec<String>,
    pub min_bk: String,
    pub tols: [String; 5],
}

impl Spec {
    /// The base clock B of AT1_SPEC section 13 with zero coupling.
    pub fn base(name: &str) -> Spec {
        let z = Q::zero;
        Spec {
            name: name.into(),
            e: vec![Q::frac(-3, 2), Q::frac(-1, 2), Q::frac(1, 2), Q::frac(3, 2)],
            h: [z(), z(), z(), Q::frac(1, 2)],
            v: vec![[z(), z(), z()], [z(), z(), z()], [z(), z(), z()], [z(), z(), z()]],
            ref_label: "t0".into(),
            psi: [Cq::real(Q::one()), Cq::real(Q::one())],
            tau: Q::frac(1, 4),
            w: Q::one(),
            labels: (0..4).map(|k| format!("t{}", k)).collect(),
            control_positive: true,
            target_ideal: false,
            expected_pass: true,
            codes: Vec::new(),
            min_bk: "ESTIMATED".into(),
            tols: ["1@12".into(), "1@12".into(), "1@12".into(), "1@9".into(), "1@12".into()],
        }
    }
    pub fn negative(mut self, codes: &[&str]) -> Spec {
        self.control_positive = false;
        self.expected_pass = false;
        self.codes = codes.iter().map(|s| s.to_string()).collect();
        self
    }
    pub fn lines(&self) -> Vec<String> {
        let qs = |v: &[Q]| v.iter().map(|q| q.text()).collect::<Vec<_>>().join(",");
        let cs = |z: &Cq| format!("({};{})", z.re.text(), z.im.text());
        let mut sem = vec!["begin semantic".to_string(), "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG".into(), "energy_unit DIMENSIONLESS_HBAR_1".into()];
        sem.push(format!("clock_dim {}", self.e.len()));
        sem.push(format!("clock_energies {}", qs(&self.e)));
        sem.push("system_dim 2".into());
        sem.push(format!("system_hamiltonian_pauli {}", qs(&self.h)));
        sem.push("interaction CLOCK_DIAGONAL_PAULI".into());
        for (j, v) in self.v.iter().enumerate() {
            sem.push(format!("interaction_pauli {} {}", j, qs(v)));
        }
        sem.push("constraint SUM_HC_HS_V".into());
        sem.push("physical_state NULLSPACE_PROJECTION".into());
        sem.push(format!("reference_clock_label {}", self.ref_label));
        sem.push(format!("reference_system_state {},{}", cs(&self.psi[0]), cs(&self.psi[1])));
        sem.push("clock_povm COVARIANT_DISCRETE".into());
        sem.push(format!("povm_tau_turns {}", self.tau.text()));
        sem.push(format!("povm_weight {}", self.w.text()));
        sem.push(format!("clock_label_count {}", self.labels.len()));
        for (k, l) in self.labels.iter().enumerate() {
            sem.push(format!("clock_label {} {}", k, l));
        }
        sem.push("observables PAULI_X,PAULI_Y,PAULI_Z".into());
        sem.push("end semantic".into());
        let acc = vec![
            "begin acceptance".to_string(),
            format!("control_kind {}", if self.control_positive { "POSITIVE" } else { "NEGATIVE" }),
            format!("prediction_target {}", if self.target_ideal { "IDEAL" } else { "INTERACTING" }),
            format!("expected_outcome {}", if self.expected_pass { "PASS" } else { "FAIL" }),
            format!("expected_failure_codes {}", if self.codes.is_empty() { "none".to_string() } else { self.codes.join(",") }),
            format!("min_bound_kind {}", self.min_bk),
            format!("tol_constraint_residual {}", self.tols[0]),
            format!("tol_povm_residual {}", self.tols[1]),
            format!("tol_probability {}", self.tols[2]),
            format!("tol_zero_probability {}", self.tols[3]),
            format!("tol_schrodinger {}", self.tols[4]),
            "end acceptance".to_string(),
        ];
        let mut out = vec!["OMEGA-AT1-CASE v1".to_string(), format!("domain {}", CASE_DOMAIN), "contract AT1_CASE_V1".into(), format!("case_name {}", self.name)];
        out.extend(sem);
        out.extend(acc);
        out.push("case_id 0".into());
        out.push("acceptance_id 0".into());
        out.push("end".into());
        recompute_ids(&mut out);
        out
    }
    pub fn bytes(&self) -> Vec<u8> {
        join_lf(&self.lines())
    }
}

/// Rewrites the case_id and acceptance_id lines of a (possibly defective) case
/// text from its current semantic and acceptance blocks, when they can be found.
pub fn recompute_ids(lines: &mut [String]) {
    let find = |s: &str| lines.iter().position(|l| l == s);
    if let (Some(a), Some(b)) = (find("begin semantic"), find("end semantic")) {
        if a < b {
            let cid = tagged_hex(CASE_DOMAIN, &join_lf(&lines[a..=b]));
            if let Some(i) = lines.iter().position(|l| l.starts_with("case_id ")) {
                lines[i] = format!("case_id {}", cid);
            }
        }
    }
    let find = |s: &str| lines.iter().position(|l| l == s);
    if let (Some(a), Some(b)) = (find("begin acceptance"), find("end acceptance")) {
        if a < b {
            let aid = tagged_hex(ACC_DOMAIN, &join_lf(&lines[a..=b]));
            if let Some(i) = lines.iter().position(|l| l.starts_with("acceptance_id ")) {
                lines[i] = format!("acceptance_id {}", aid);
            }
        }
    }
}
