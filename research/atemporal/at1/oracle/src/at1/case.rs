//! AT1_CASE_V1 codec: byte rules, canonical encodings, exact validity rules in the order of
//! section 4, identities (section 5). Contract: aien-architecture
//! docs/plans/atemporal/AT1_CASE_V1.md, sha256 e62018d8..., frozen at `CONTRACT_COMMIT`.
//!
//! Readings followed (AT1_CHARTER.md section 8, ruled on omega#371):
//! - (b, inherited) shape before range: fixed literals, enumerations, counts, list arity and
//!   index order are shape (`CASE_PARSE_ERROR`); `CASE_NONCANONICAL` is decided after the
//!   whole shape pass; `CASE_INVALID_PARAMETER` is for the ranges of section 3.
//! - (d) the first token of each version-bearing line is its key (shape, step 1); the rest
//!   is its value (step 2, `CASE_UNSUPPORTED_VERSION`).
//! - (e) index tokens are compared with their position by parsed value in the shape pass.
//!
//! Token readings this oracle declares where the frozen text leaves room (README, section
//! "Declared readings"), matching the qualified AT-0 engine: an integer token is `-?[0-9]+`
//! (so `+4` does not parse: shape); a scaled token is `<int>@<int>` and a negative part is
//! non-canonical; a wrong arity in a fixed-arity list is shape.

use super::big::{Int, Nat, Q, GQ};
use super::sha256::{sha256_hex, tagged_hex};

/// aien-architecture commit that froze AT1_CASE_V1 and AT1_RESULT_V1 (gate AT1-G0, PR 184).
pub const CONTRACT_COMMIT: &str = "cbe4c8ed88d28bb96d5209327ecd30aa9cddaf7a";
pub const CASE_DOMAIN: &str = "omega.at1.case.v1";
pub const ACCEPTANCE_DOMAIN: &str = "omega.at1.acceptance.v1";
pub const RATIONAL_LIMIT: u64 = 1_048_576;
pub const FAILURE_CODES: [&str; 12] = [
    "BOUND_KIND_INSUFFICIENT",
    "CONDITIONAL_UNDEFINED",
    "CONSTRAINT_RESIDUAL_EXCEEDED",
    "INTERACTING_DEVIATION_EXCEEDED",
    "NONFINITE_VALUE",
    "ORACLE_DISAGREEMENT",
    "POVM_NORMALIZATION_EXCEEDED",
    "PRECISION_INSUFFICIENT",
    "PROBABILITY_OUT_OF_RANGE",
    "PROBABILITY_SUM_EXCEEDED",
    "SCHRODINGER_DEVIATION_EXCEEDED",
    "TRIVIAL_PHYSICAL_STATE",
];
pub const BOUND_KINDS: [&str; 3] = ["NONE", "ESTIMATED", "RIGOROUS"];

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refusal {
    pub code: &'static str,
    pub detail: String,
}

fn refuse<T>(code: &'static str, detail: impl Into<String>) -> Result<T, Refusal> {
    Err(Refusal { code, detail: detail.into() })
}
fn shape<T>(detail: impl Into<String>) -> Result<T, Refusal> {
    refuse("CASE_PARSE_ERROR", detail)
}

/// Scaled decimal n / 10^k as written (tolerances and bounds).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Scaled {
    pub n: Nat,
    pub k: u64,
}

impl Scaled {
    pub fn exact(&self) -> Q {
        Q::from_scaled(&self.n, self.k.min(40) as u32)
    }
}

#[derive(Clone, Debug)]
pub struct Case {
    pub name: String,
    pub clock_dim: usize,
    pub energies: Vec<Q>,
    pub h0: Q,
    pub h: [Q; 3],
    pub v: Vec<[Q; 3]>,
    pub ref_label: String,
    pub ref_index: usize,
    pub psi: [GQ; 2],
    pub tau: Q,
    pub weight: Q,
    pub labels: Vec<String>,
    pub control_kind: String,
    pub prediction_target: String,
    pub expected_outcome: String,
    pub expected_codes: Vec<String>,
    pub min_bound_kind: String,
    pub tol_constraint: Scaled,
    pub tol_povm: Scaled,
    pub tol_probability: Scaled,
    pub tol_zero: Scaled,
    pub tol_schrodinger: Scaled,
    pub case_id: String,
    pub acceptance_id: String,
    pub semantic_block: String,
    pub acceptance_block: String,
    pub file_sha256: String,
    /// Level norms R_j = |h + v_j|, exact rationals (validity rule 4).
    pub level_norm: Vec<Q>,
}

impl Case {
    /// n_j = h + v_j.
    pub fn level_vector(&self, j: usize) -> [Q; 3] {
        [self.h[0].add(&self.v[j][0]), self.h[1].add(&self.v[j][1]), self.h[2].add(&self.v[j][2])]
    }
}

// ---- token parsers: Ok((value, canonical)) or a shape refusal -------------------------

/// Integer grammar `-?[0-9]+`. Canonical: no leading zero (except `0`), not `-0`.
pub fn parse_int(tok: &str) -> Result<(Int, bool), Refusal> {
    let (neg, digits) = match tok.strip_prefix('-') {
        Some(d) => (true, d),
        None => (false, tok),
    };
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return shape(format!("integer {:?}", tok));
    }
    let canonical = (digits == "0" || !digits.starts_with('0')) && !(neg && digits.starts_with('0'));
    Ok((Int::from_nat(neg, Nat::from_decimal(digits)), canonical))
}

/// Small integer value for counts and indices: None when it does not fit (or is negative),
/// which can then never equal a count or a position.
fn small(i: &Int) -> Option<usize> {
    if i.neg {
        return None;
    }
    i.mag.to_u64().filter(|v| *v <= 1 << 40).map(|v| v as usize)
}

/// Rational `n/d`. Canonical: both integers canonical, d >= 1, gcd(|n|, d) = 1.
pub fn parse_rational(tok: &str) -> Result<(Q, bool), Refusal> {
    let mut parts = tok.split('/');
    let (a, b) = match (parts.next(), parts.next(), parts.next()) {
        (Some(a), Some(b), None) => (a, b),
        _ => return shape(format!("rational {:?}", tok)),
    };
    let (n, cn) = parse_int(a)?;
    let (d, cd) = parse_int(b)?;
    if d.neg || d.is_zero() {
        return Ok((Q::zero(), false)); // d < 1: non-canonical; value never used
    }
    let reduced = n.mag.gcd(&d.mag) == Nat::one();
    Ok((Q::raw(n, d.mag), cn && cd && reduced))
}

/// Complex rational `(re;im)`.
pub fn parse_complex(tok: &str) -> Result<(GQ, bool), Refusal> {
    let inner = match tok.strip_prefix('(').and_then(|t| t.strip_suffix(')')) {
        Some(i) => i,
        None => return shape(format!("complex {:?}", tok)),
    };
    let mut parts = inner.split(';');
    match (parts.next(), parts.next(), parts.next()) {
        (Some(a), Some(b), None) => {
            let (re, ca) = parse_rational(a)?;
            let (im, cb) = parse_rational(b)?;
            Ok((GQ::new(re, im), ca && cb))
        }
        _ => shape(format!("complex {:?}", tok)),
    }
}

/// Scaled decimal `N@k`. Canonical: both parts canonical and nonnegative; zero is `0@0`;
/// N not divisible by 10 unless k = 0. k > 40 is a range error (rule 3), checked later.
pub fn parse_scaled(tok: &str) -> Result<(Scaled, bool), Refusal> {
    let mut parts = tok.split('@');
    let (a, b) = match (parts.next(), parts.next(), parts.next()) {
        (Some(a), Some(b), None) => (a, b),
        _ => return shape(format!("scaled {:?}", tok)),
    };
    let (n, cn) = parse_int(a)?;
    let (k, ck) = parse_int(b)?;
    let k_val = if k.neg { 0 } else { k.mag.to_u64().unwrap_or(u64::MAX) };
    let ten = Nat::from_u64(10);
    let n_div10 = !n.is_zero() && n.mag.divrem(&ten).1.is_zero();
    let canonical = cn && ck && !n.neg && !k.neg && !(n.is_zero() && k_val != 0) && !(n_div10 && k_val != 0);
    Ok((Scaled { n: n.mag, k: k_val }, canonical))
}

/// Comma list without spaces; items nonempty.
fn split_list(tok: &str) -> Result<Vec<&str>, Refusal> {
    let items: Vec<&str> = tok.split(',').collect();
    if items.iter().any(|s| s.is_empty()) {
        return shape(format!("list {:?}", tok));
    }
    Ok(items)
}

pub fn is_label(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty() && b.len() <= 32 && b[0].is_ascii_lowercase() && b.iter().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == b'_')
}

pub fn is_name(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty() && b.len() <= 64 && b[0].is_ascii_alphanumeric() && b.iter().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'.' | b'-'))
}

pub fn is_digest(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

// ---- byte rules -----------------------------------------------------------------------

pub fn check_bytes(data: &[u8]) -> Result<Vec<&str>, Refusal> {
    if data.last() != Some(&b'\n') {
        return shape("missing final LF");
    }
    if let Some(b) = data.iter().find(|b| **b != 0x0A && !(0x20..=0x7E).contains(*b)) {
        return shape(format!("byte 0x{:02x}", b));
    }
    let text = std::str::from_utf8(data).map_err(|_| Refusal { code: "CASE_PARSE_ERROR", detail: "ascii".into() })?;
    let mut lines: Vec<&str> = text.split('\n').collect();
    lines.pop();
    for ln in &lines {
        if ln.is_empty() || ln.starts_with(' ') || ln.ends_with(' ') || ln.contains("  ") {
            return shape(format!("line spacing {:?}", ln));
        }
    }
    Ok(lines)
}

struct Cursor<'a> {
    lines: Vec<&'a str>,
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn peek_key(&self) -> Option<&'a str> {
        self.lines.get(self.pos).map(|l| l.split(' ').next().unwrap_or(""))
    }
    /// Next line must be `<key> <value>` with a nonempty value; returns the value.
    fn keyed(&mut self, key: &str) -> Result<&'a str, Refusal> {
        let line = match self.lines.get(self.pos) {
            Some(l) => *l,
            None => return shape(format!("missing {}", key)),
        };
        self.pos += 1;
        match line.strip_prefix(key).and_then(|r| r.strip_prefix(' ')) {
            Some(v) if !v.is_empty() => Ok(v),
            _ => shape(format!("expected {}, got {:?}", key, line)),
        }
    }
    /// Next line must equal `text` exactly (fixed literal of the section 1 grammar).
    fn fixed(&mut self, text: &str) -> Result<(), Refusal> {
        match self.lines.get(self.pos) {
            Some(l) if *l == text => {
                self.pos += 1;
                Ok(())
            }
            other => shape(format!("expected {:?}, got {:?}", text, other)),
        }
    }
    fn enumerated(&mut self, key: &str, allowed: &[&str]) -> Result<String, Refusal> {
        let v = self.keyed(key)?;
        if !allowed.contains(&v) {
            return shape(format!("{} {}", key, v));
        }
        Ok(v.to_string())
    }
}

/// Lines from `begin <name>` through `end <name>` inclusive, each followed by LF.
pub fn block(lines: &[&str], name: &str) -> String {
    let (b, e) = (format!("begin {}", name), format!("end {}", name));
    let start = lines.iter().position(|l| *l == b).unwrap_or(0);
    let end = lines.iter().position(|l| *l == e).unwrap_or(start);
    lines[start..=end].iter().map(|l| format!("{}\n", l)).collect()
}

pub fn case_id(sem_block: &str) -> String {
    tagged_hex(CASE_DOMAIN, sem_block.as_bytes())
}

pub fn acceptance_id(acc_block: &str) -> String {
    tagged_hex(ACCEPTANCE_DOMAIN, acc_block.as_bytes())
}

fn in_limits(q: &Q) -> bool {
    let lim = Nat::from_u64(RATIONAL_LIMIT);
    q.num.mag <= lim && q.den <= lim
}

/// Parse and validate case bytes in the order of AT1_CASE_V1 section 4.
pub fn parse_case(data: &[u8]) -> Result<Case, Refusal> {
    // ---- step 1: shape (and token parse), canonical form decided after the whole pass ----
    let lines = check_bytes(data)?;
    let mut c = Cursor { lines: lines.clone(), pos: 0 };
    let mut canonical = true;
    let mut note = |ok: bool| canonical &= ok;
    let header_v = c.keyed("OMEGA-AT1-CASE")?;
    let domain_v = c.keyed("domain")?;
    let contract_v = c.keyed("contract")?;
    let name = c.keyed("case_name")?.to_string();
    if !is_name(&name) {
        return shape("case_name");
    }
    c.fixed("begin semantic")?;
    c.fixed("model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG")?;
    c.fixed("energy_unit DIMENSIONLESS_HBAR_1")?;
    let (clock_dim_i, ok) = parse_int(c.keyed("clock_dim")?)?;
    note(ok);
    let clock_dim = small(&clock_dim_i);
    let energy_toks = split_list(c.keyed("clock_energies")?)?;
    if Some(energy_toks.len()) != clock_dim {
        return shape("clock_energies count differs from clock_dim");
    }
    let mut energies = Vec::new();
    for t in energy_toks {
        let (q, ok) = parse_rational(t)?;
        note(ok);
        energies.push(q);
    }
    c.fixed("system_dim 2")?;
    let hp = split_list(c.keyed("system_hamiltonian_pauli")?)?;
    if hp.len() != 4 {
        return shape("system_hamiltonian_pauli arity");
    }
    let mut hv = Vec::new();
    for t in hp {
        let (q, ok) = parse_rational(t)?;
        note(ok);
        hv.push(q);
    }
    c.fixed("interaction CLOCK_DIAGONAL_PAULI")?;
    let mut v = Vec::new();
    while c.peek_key() == Some("interaction_pauli") {
        let rest = c.keyed("interaction_pauli")?;
        let mut parts = rest.split(' ');
        let (it, vt) = match (parts.next(), parts.next(), parts.next()) {
            (Some(a), Some(b), None) => (a, b),
            _ => return shape(format!("interaction_pauli {:?}", rest)),
        };
        let (idx, ok) = parse_int(it)?;
        note(ok);
        if small(&idx) != Some(v.len()) {
            return shape(format!("interaction_pauli index {:?} at position {}", it, v.len()));
        }
        let comps = split_list(vt)?;
        if comps.len() != 3 {
            return shape("interaction_pauli arity");
        }
        let mut vec3 = Vec::new();
        for t in comps {
            let (q, ok) = parse_rational(t)?;
            note(ok);
            vec3.push(q);
        }
        v.push([vec3[0].clone(), vec3[1].clone(), vec3[2].clone()]);
    }
    if Some(v.len()) != clock_dim {
        return shape("interaction_pauli count differs from clock_dim");
    }
    c.fixed("constraint SUM_HC_HS_V")?;
    c.fixed("physical_state NULLSPACE_PROJECTION")?;
    let ref_label = c.keyed("reference_clock_label")?.to_string();
    if !is_label(&ref_label) {
        return shape("reference_clock_label");
    }
    let st = split_list(c.keyed("reference_system_state")?)?;
    if st.len() != 2 {
        return shape("reference_system_state arity");
    }
    let (psi0, ok0) = parse_complex(st[0])?;
    let (psi1, ok1) = parse_complex(st[1])?;
    note(ok0 && ok1);
    c.fixed("clock_povm COVARIANT_DISCRETE")?;
    let (tau, ok) = parse_rational(c.keyed("povm_tau_turns")?)?;
    note(ok);
    let (weight, ok) = parse_rational(c.keyed("povm_weight")?)?;
    note(ok);
    let (label_count_i, ok) = parse_int(c.keyed("clock_label_count")?)?;
    note(ok);
    let mut labels = Vec::new();
    while c.peek_key() == Some("clock_label") {
        let rest = c.keyed("clock_label")?;
        let mut parts = rest.split(' ');
        let (it, lb) = match (parts.next(), parts.next(), parts.next()) {
            (Some(a), Some(b), None) => (a, b),
            _ => return shape(format!("clock_label {:?}", rest)),
        };
        let (idx, ok) = parse_int(it)?;
        note(ok);
        if small(&idx) != Some(labels.len()) || !is_label(lb) {
            return shape(format!("clock_label {:?} at position {}", rest, labels.len()));
        }
        labels.push(lb.to_string());
    }
    if Some(labels.len()) != small(&label_count_i) {
        return shape("clock_label count differs from clock_label_count");
    }
    c.fixed("observables PAULI_X,PAULI_Y,PAULI_Z")?;
    c.fixed("end semantic")?;
    c.fixed("begin acceptance")?;
    let control_kind = c.enumerated("control_kind", &["POSITIVE", "NEGATIVE"])?;
    let prediction_target = c.enumerated("prediction_target", &["IDEAL", "INTERACTING"])?;
    let expected_outcome = c.enumerated("expected_outcome", &["PASS", "FAIL"])?;
    let codes_tok = c.keyed("expected_failure_codes")?;
    let expected_codes: Vec<String> = if codes_tok == "none" { Vec::new() } else { split_list(codes_tok)?.iter().map(|s| s.to_string()).collect() };
    let min_bound_kind = c.enumerated("min_bound_kind", &["RIGOROUS", "ESTIMATED", "NONE"])?;
    let mut tols = Vec::new();
    for key in ["tol_constraint_residual", "tol_povm_residual", "tol_probability", "tol_zero_probability", "tol_schrodinger"] {
        let (s, ok) = parse_scaled(c.keyed(key)?)?;
        note(ok);
        tols.push(s);
    }
    c.fixed("end acceptance")?;
    let cid = c.keyed("case_id")?.to_string();
    if !is_digest(&cid) {
        return shape("case_id");
    }
    let aid = c.keyed("acceptance_id")?.to_string();
    if !is_digest(&aid) {
        return shape("acceptance_id");
    }
    c.fixed("end")?;
    if c.pos != c.lines.len() {
        return shape("trailing lines");
    }
    if !canonical {
        return refuse("CASE_NONCANONICAL", "a token parses but is not canonical");
    }

    // ---- step 2: values of the version-bearing lines ----
    if header_v != "v1" || domain_v != CASE_DOMAIN || contract_v != "AT1_CASE_V1" {
        return refuse("CASE_UNSUPPORTED_VERSION", format!("{} / {} / {}", header_v, domain_v, contract_v));
    }

    // ---- step 3: ranges and fixed values of section 3 ----
    let n = clock_dim.unwrap_or(0);
    if !(2..=64).contains(&n) {
        return refuse("CASE_INVALID_PARAMETER", "clock_dim");
    }
    let mut all_rats: Vec<&Q> = energies.iter().chain(hv.iter()).collect();
    for vj in &v {
        all_rats.extend(vj.iter());
    }
    all_rats.extend([&psi0.re, &psi0.im, &psi1.re, &psi1.im, &tau, &weight]);
    if all_rats.iter().any(|q| !in_limits(q)) {
        return refuse("CASE_INVALID_PARAMETER", "rational token limit");
    }
    if tols.iter().any(|s| s.k > 40) {
        return refuse("CASE_INVALID_PARAMETER", "scaled exponent above 40");
    }
    if energies.windows(2).any(|w| w[0] >= w[1]) {
        return refuse("CASE_INVALID_PARAMETER", "clock_energies not strictly increasing");
    }
    if psi0.is_zero() && psi1.is_zero() {
        return refuse("CASE_INVALID_PARAMETER", "reference_system_state zero");
    }
    if weight <= Q::zero() || tau <= Q::zero() {
        return refuse("CASE_INVALID_PARAMETER", "povm_weight or povm_tau_turns not positive");
    }
    if !(1..=256).contains(&labels.len()) {
        return refuse("CASE_INVALID_PARAMETER", "clock_label_count");
    }
    let mut sorted = labels.clone();
    sorted.sort();
    sorted.dedup();
    if sorted.len() != labels.len() {
        return refuse("CASE_INVALID_PARAMETER", "duplicate clock label");
    }
    let ref_index = match labels.iter().position(|l| *l == ref_label) {
        Some(i) => i,
        None => return refuse("CASE_INVALID_PARAMETER", "reference_clock_label not a clock label"),
    };

    // ---- step 4: rational spectrum per level, exact ----
    let h = [hv[1].clone(), hv[2].clone(), hv[3].clone()];
    let mut level_norm = Vec::with_capacity(n);
    for (j, vj) in v.iter().enumerate() {
        let nj = [h[0].add(&vj[0]), h[1].add(&vj[1]), h[2].add(&vj[2])];
        let nn = nj[0].mul(&nj[0]).add(&nj[1].mul(&nj[1])).add(&nj[2].mul(&nj[2]));
        match nn.sqrt_exact() {
            Some(r) => level_norm.push(r),
            None => return refuse("CASE_IRRATIONAL_SPECTRUM", format!("level {}", j)),
        }
    }

    // ---- step 5: identities ----
    let semantic_block = block(&lines, "semantic");
    let acceptance_block = block(&lines, "acceptance");
    if case_id(&semantic_block) != cid || acceptance_id(&acceptance_block) != aid {
        return refuse("CASE_ID_MISMATCH", "");
    }

    // ---- step 6: acceptance ----
    if expected_outcome == "PASS" {
        if !expected_codes.is_empty() {
            return refuse("CASE_INVALID_PARAMETER", "expected_failure_codes with PASS");
        }
    } else {
        let mut s = expected_codes.clone();
        s.sort();
        s.dedup();
        if expected_codes.is_empty() || s != expected_codes || expected_codes.iter().any(|x| !FAILURE_CODES.contains(&x.as_str())) {
            return refuse("CASE_INVALID_PARAMETER", "expected_failure_codes");
        }
    }
    let mut it = tols.into_iter();
    Ok(Case {
        name,
        clock_dim: n,
        energies: energies.into_iter().map(|q| Q::new(q.num, q.den)).collect(),
        h0: hv[0].clone(),
        h,
        v,
        ref_label,
        ref_index,
        psi: [psi0, psi1],
        tau,
        weight,
        labels,
        control_kind,
        prediction_target,
        expected_outcome,
        expected_codes,
        min_bound_kind,
        tol_constraint: it.next().unwrap(),
        tol_povm: it.next().unwrap(),
        tol_probability: it.next().unwrap(),
        tol_zero: it.next().unwrap(),
        tol_schrodinger: it.next().unwrap(),
        case_id: cid,
        acceptance_id: aid,
        semantic_block,
        acceptance_block,
        file_sha256: sha256_hex(data),
        level_norm,
    })
}

// ---- case builder (fixtures and tests) ----------------------------------------------------

pub fn qtext(q: &Q) -> String {
    let r = Q::new(q.num.clone(), q.den.clone());
    format!("{}/{}", r.num.to_decimal(), r.den.to_decimal())
}

/// Exact parameters of one case, for emitting canonical case bytes.
#[derive(Clone)]
pub struct Spec {
    pub name: String,
    pub energies: Vec<Q>,
    pub hpauli: [Q; 4],
    pub v: Vec<[Q; 3]>,
    pub ref_label: usize,
    pub psi: [GQ; 2],
    pub tau: Q,
    pub weight: Q,
    pub labels: usize,
    pub control_kind: String,
    pub target: String,
    pub expected_outcome: String,
    pub expected_codes: Vec<String>,
    pub min_bound_kind: String,
    pub tol: [String; 5],
}

impl Spec {
    pub fn semantic_lines(&self) -> Vec<String> {
        let mut sem = vec![
            "begin semantic".to_string(),
            "model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG".into(),
            "energy_unit DIMENSIONLESS_HBAR_1".into(),
            format!("clock_dim {}", self.energies.len()),
            format!("clock_energies {}", self.energies.iter().map(qtext).collect::<Vec<_>>().join(",")),
            "system_dim 2".into(),
            format!("system_hamiltonian_pauli {}", self.hpauli.iter().map(qtext).collect::<Vec<_>>().join(",")),
            "interaction CLOCK_DIAGONAL_PAULI".into(),
        ];
        for (j, vj) in self.v.iter().enumerate() {
            sem.push(format!("interaction_pauli {} {}", j, vj.iter().map(qtext).collect::<Vec<_>>().join(",")));
        }
        sem.push("constraint SUM_HC_HS_V".into());
        sem.push("physical_state NULLSPACE_PROJECTION".into());
        sem.push(format!("reference_clock_label t{}", self.ref_label));
        sem.push(format!("reference_system_state ({};{}),({};{})", qtext(&self.psi[0].re), qtext(&self.psi[0].im), qtext(&self.psi[1].re), qtext(&self.psi[1].im)));
        sem.push("clock_povm COVARIANT_DISCRETE".into());
        sem.push(format!("povm_tau_turns {}", qtext(&self.tau)));
        sem.push(format!("povm_weight {}", qtext(&self.weight)));
        sem.push(format!("clock_label_count {}", self.labels));
        for k in 0..self.labels {
            sem.push(format!("clock_label {} t{}", k, k));
        }
        sem.push("observables PAULI_X,PAULI_Y,PAULI_Z".into());
        sem.push("end semantic".into());
        sem
    }
    pub fn acceptance_lines(&self) -> Vec<String> {
        let codes = if self.expected_codes.is_empty() { "none".to_string() } else { self.expected_codes.join(",") };
        vec![
            "begin acceptance".to_string(),
            format!("control_kind {}", self.control_kind),
            format!("prediction_target {}", self.target),
            format!("expected_outcome {}", self.expected_outcome),
            format!("expected_failure_codes {}", codes),
            format!("min_bound_kind {}", self.min_bound_kind),
            format!("tol_constraint_residual {}", self.tol[0]),
            format!("tol_povm_residual {}", self.tol[1]),
            format!("tol_probability {}", self.tol[2]),
            format!("tol_zero_probability {}", self.tol[3]),
            format!("tol_schrodinger {}", self.tol[4]),
            "end acceptance".into(),
        ]
    }
    pub fn bytes(&self) -> Vec<u8> {
        lines_to_case(&self.name, &self.semantic_lines(), &self.acceptance_lines())
    }
}

/// Assemble a case file from its blocks, with identities recomputed.
pub fn lines_to_case(name: &str, sem: &[String], acc: &[String]) -> Vec<u8> {
    let sem_b: String = sem.iter().map(|l| format!("{}\n", l)).collect();
    let acc_b: String = acc.iter().map(|l| format!("{}\n", l)).collect();
    let head = format!("OMEGA-AT1-CASE v1\ndomain {}\ncontract AT1_CASE_V1\ncase_name {}\n", CASE_DOMAIN, name);
    let tail = format!("case_id {}\nacceptance_id {}\nend\n", case_id(&sem_b), acceptance_id(&acc_b));
    format!("{}{}{}{}", head, sem_b, acc_b, tail).into_bytes()
}

/// Recompute both identities of an edited case text (one defect, identities refreshed).
pub fn refresh_ids(text: &str) -> String {
    let lines: Vec<&str> = text.split('\n').collect();
    let sem = block(&lines, "semantic");
    let acc = block(&lines, "acceptance");
    let (cid, aid) = (case_id(&sem), acceptance_id(&acc));
    lines
        .iter()
        .map(|l| {
            if l.starts_with("case_id ") {
                format!("case_id {}", cid)
            } else if l.starts_with("acceptance_id ") {
                format!("acceptance_id {}", aid)
            } else {
                l.to_string()
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}
