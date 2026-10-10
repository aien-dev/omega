//! AT0_CASE_V1 codec: byte rules, canonical encodings, exact validity rules
//! in the contract's order (section 4), identities (section 5), kernel pairs.
//! Contract: aien-architecture docs/plans/atemporal/AT0_CASE_V1.md at 044c9d1.

use super::rational::Rat;
use super::sha256::{sha256_hex, tagged_hex};

/// Freeze commit written as `contract_commit`: AT0_FREEZE lists AT0_RESULT_V2 and marks V1
/// superseded (aien-architecture#181). AT0_CASE_V1 itself was frozen earlier at
/// `CASE_CONTRACT_COMMIT` and is unchanged since.
pub const CONTRACT_COMMIT: &str = "fe86e43aae63370b3084f78e971a33b81ce75a9d";
pub const CASE_CONTRACT_COMMIT: &str = "044c9d11256d8642f80eedd42cbae8763faf63f5";
pub const CASE_DOMAIN: &str = "omega.at0.case.v1";
pub const ACCEPTANCE_DOMAIN: &str = "omega.at0.acceptance.v1";
pub const RATIONAL_LIMIT: i128 = 1_048_576;
pub const FAILURE_CODES: [&str; 10] = [
    "BOUND_KIND_INSUFFICIENT",
    "CONDITIONAL_UNDEFINED",
    "CONSTRAINT_RESIDUAL_EXCEEDED",
    "NONFINITE_VALUE",
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

/// Scaled decimal n / 10^k (tolerances and bounds).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scaled {
    pub n: u128,
    pub k: u32,
}

impl Scaled {
    pub fn text(&self) -> String {
        format!("{}@{}", self.n, self.k)
    }
    /// Canonical form: strip trailing zeros of n into k.
    pub fn canonical(mut n: u128, mut k: u32) -> Scaled {
        if n == 0 {
            return Scaled { n: 0, k: 0 };
        }
        while k > 0 && n % 10 == 0 {
            n /= 10;
            k -= 1;
        }
        Scaled { n, k }
    }
}

#[derive(Clone, Debug)]
pub struct Case {
    pub name: String,
    pub clock_dim: usize,
    pub energies: Vec<Rat>,
    pub pauli: [Rat; 4],
    pub ref_label: String,
    pub ref_index: usize,
    pub state: [(Rat, Rat); 2],
    pub tau: Rat,
    pub weight: Rat,
    pub labels: Vec<String>,
    pub control_kind: String,
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
    pub h_norm: Rat,
    pub eigenvalues: [Rat; 2],
}

// ---- token parsers ------------------------------------------------------------

pub fn parse_int(tok: &str) -> Result<i128, Refusal> {
    let digits = tok.strip_prefix('-').unwrap_or(tok);
    let ok = !digits.is_empty()
        && digits.bytes().all(|b| b.is_ascii_digit())
        && (digits == "0" || !digits.starts_with('0'))
        && tok != "-0";
    if !ok {
        return refuse("CASE_NONCANONICAL", format!("integer {}", tok));
    }
    digits.len().le(&38).then(|| ()).ok_or(Refusal { code: "CASE_INVALID_PARAMETER", detail: "integer too long".into() })?;
    let v: i128 = digits.parse().map_err(|_| Refusal { code: "CASE_INVALID_PARAMETER", detail: "integer".into() })?;
    Ok(if tok.starts_with('-') { -v } else { v })
}

pub fn parse_rational(tok: &str) -> Result<Rat, Refusal> {
    let (a, b) = match tok.split_once('/') {
        Some(p) if !p.1.contains('/') => p,
        _ => return refuse("CASE_PARSE_ERROR", format!("rational {}", tok)),
    };
    let (n, d) = (parse_int(a)?, parse_int(b)?);
    if d < 1 {
        return refuse("CASE_NONCANONICAL", format!("rational denominator {}", tok));
    }
    if n.abs() > RATIONAL_LIMIT || d > RATIONAL_LIMIT {
        return refuse("CASE_INVALID_PARAMETER", format!("rational limit {}", tok));
    }
    let r = Rat::new(n, d);
    if r.n != n || r.d != d {
        return refuse("CASE_NONCANONICAL", format!("rational not reduced {}", tok));
    }
    Ok(r)
}

pub fn parse_complex(tok: &str) -> Result<(Rat, Rat), Refusal> {
    let inner = match tok.strip_prefix('(').and_then(|t| t.strip_suffix(')')) {
        Some(i) => i,
        None => return refuse("CASE_PARSE_ERROR", format!("complex {}", tok)),
    };
    match inner.split_once(';') {
        Some((a, b)) if !b.contains(';') => Ok((parse_rational(a)?, parse_rational(b)?)),
        _ => refuse("CASE_PARSE_ERROR", format!("complex {}", tok)),
    }
}

pub fn parse_scaled(tok: &str) -> Result<Scaled, Refusal> {
    let (a, b) = match tok.split_once('@') {
        Some(p) => p,
        None => return refuse("CASE_PARSE_ERROR", format!("scaled {}", tok)),
    };
    let digits_ok = |s: &str| !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit()) && (s == "0" || !s.starts_with('0'));
    if !digits_ok(a) || !digits_ok(b) || a.len() > 38 || b.len() > 2 {
        return refuse("CASE_PARSE_ERROR", format!("scaled {}", tok));
    }
    let (n, k): (u128, u32) = (a.parse().unwrap(), b.parse().unwrap());
    if k > 40 {
        return refuse("CASE_INVALID_PARAMETER", format!("scaled exponent {}", tok));
    }
    if (n == 0 && k != 0) || (n != 0 && k != 0 && n % 10 == 0) {
        return refuse("CASE_NONCANONICAL", format!("scaled {}", tok));
    }
    Ok(Scaled { n, k })
}

pub fn parse_list(tok: &str) -> Result<Vec<String>, Refusal> {
    if tok == "none" {
        return Ok(Vec::new());
    }
    if tok.is_empty() || tok.starts_with(',') || tok.ends_with(',') || tok.contains(",,") {
        return refuse("CASE_PARSE_ERROR", format!("list {}", tok));
    }
    Ok(tok.split(',').map(str::to_string).collect())
}

pub fn is_label(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty()
        && b.len() <= 32
        && b[0].is_ascii_lowercase()
        && b.iter().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == b'_')
}

pub fn is_name(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty()
        && b.len() <= 64
        && b[0].is_ascii_alphanumeric()
        && b.iter().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'.' | b'-'))
}

pub fn is_digest(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

// ---- byte rules ---------------------------------------------------------------

pub fn check_bytes(data: &[u8]) -> Result<Vec<&str>, Refusal> {
    if data.last() != Some(&b'\n') {
        return refuse("CASE_PARSE_ERROR", "missing final LF");
    }
    if let Some(b) = data.iter().find(|b| **b != 0x0A && !(0x20..=0x7E).contains(*b)) {
        return refuse("CASE_PARSE_ERROR", format!("byte 0x{:02x}", b));
    }
    let text = std::str::from_utf8(data).map_err(|_| Refusal { code: "CASE_PARSE_ERROR", detail: "ascii".into() })?;
    let mut lines: Vec<&str> = text.split('\n').collect();
    lines.pop(); // the empty piece after the final LF
    for ln in &lines {
        if ln.is_empty() || ln.starts_with(' ') || ln.ends_with(' ') || ln.contains("  ") {
            return refuse("CASE_PARSE_ERROR", format!("line spacing: {:?}", ln));
        }
    }
    Ok(lines)
}

struct Cursor<'a> {
    lines: Vec<&'a str>,
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn take(&mut self, key: &str) -> Result<&'a str, Refusal> {
        let line = match self.lines.get(self.pos) {
            Some(l) => *l,
            None => return refuse("CASE_PARSE_ERROR", format!("missing {}", key)),
        };
        self.pos += 1;
        if line == key {
            return Ok("");
        }
        match line.strip_prefix(key).and_then(|r| r.strip_prefix(' ')) {
            Some(v) => Ok(v),
            None => refuse("CASE_PARSE_ERROR", format!("expected {}, got {:?}", key, line)),
        }
    }
    fn fixed(&mut self, key: &str, want: &str, code: &'static str) -> Result<(), Refusal> {
        let v = self.take(key)?;
        if v != want {
            return refuse(code, format!("{} {}", key, v));
        }
        Ok(())
    }
    fn enumerated(&mut self, key: &str, allowed: &[&str]) -> Result<String, Refusal> {
        let v = self.take(key)?;
        if !allowed.contains(&v) {
            return refuse("CASE_PARSE_ERROR", format!("{} {}", key, v));
        }
        Ok(v.to_string())
    }
}

pub fn block(text: &str, name: &str) -> String {
    let lines: Vec<&str> = text.split('\n').collect();
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

/// Parse and validate case bytes in the order of AT0_CASE_V1 section 4.
pub fn parse_case(data: &[u8]) -> Result<Case, Refusal> {
    let lines = check_bytes(data)?;
    let mut c = Cursor { lines, pos: 0 };
    c.fixed("OMEGA-AT0-CASE", "v1", "CASE_UNSUPPORTED_VERSION")?;
    c.fixed("domain", "omega.at0.case.v1", "CASE_UNSUPPORTED_VERSION")?;
    c.fixed("contract", "AT0_CASE_V1", "CASE_UNSUPPORTED_VERSION")?;
    let name = c.take("case_name")?.to_string();
    if !is_name(&name) {
        return refuse("CASE_PARSE_ERROR", "case_name");
    }
    c.fixed("begin", "semantic", "CASE_PARSE_ERROR")?;
    c.fixed("model_family", "PAGE_WOOTTERS_FINITE_IDEAL", "CASE_PARSE_ERROR")?;
    c.fixed("energy_unit", "DIMENSIONLESS_HBAR_1", "CASE_PARSE_ERROR")?;
    let clock_dim_i = parse_int(c.take("clock_dim")?)?;
    let energies: Vec<Rat> = parse_list(c.take("clock_energies")?)?.iter().map(|t| parse_rational(t)).collect::<Result<_, _>>()?;
    c.fixed("system_dim", "2", "CASE_PARSE_ERROR")?;
    let pauli_v: Vec<Rat> = parse_list(c.take("system_hamiltonian_pauli")?)?.iter().map(|t| parse_rational(t)).collect::<Result<_, _>>()?;
    c.fixed("interaction", "NONE", "CASE_PARSE_ERROR")?;
    c.fixed("constraint", "SUM_HC_HS", "CASE_PARSE_ERROR")?;
    c.fixed("physical_state", "NULLSPACE_PROJECTION", "CASE_PARSE_ERROR")?;
    let ref_label = c.take("reference_clock_label")?.to_string();
    if !is_label(&ref_label) {
        return refuse("CASE_PARSE_ERROR", "reference_clock_label");
    }
    let state_v: Vec<(Rat, Rat)> = parse_list(c.take("reference_system_state")?)?.iter().map(|t| parse_complex(t)).collect::<Result<_, _>>()?;
    c.fixed("clock_povm", "COVARIANT_DISCRETE", "CASE_PARSE_ERROR")?;
    let tau = parse_rational(c.take("povm_tau_turns")?)?;
    let weight = parse_rational(c.take("povm_weight")?)?;
    let label_count = parse_int(c.take("clock_label_count")?)?;
    let mut labels = Vec::new();
    if (1..=256).contains(&label_count) {
        for k in 0..label_count {
            let v = c.take("clock_label")?;
            let (idx, lb) = match v.split_once(' ') {
                Some(p) => p,
                None => return refuse("CASE_PARSE_ERROR", format!("clock_label {}", v)),
            };
            if parse_int(idx)? != k || !is_label(lb) {
                return refuse("CASE_PARSE_ERROR", format!("clock_label {}", v));
            }
            labels.push(lb.to_string());
        }
    }
    c.fixed("observables", "PAULI_X,PAULI_Y,PAULI_Z", "CASE_PARSE_ERROR")?;
    c.fixed("end", "semantic", "CASE_PARSE_ERROR")?;
    c.fixed("begin", "acceptance", "CASE_PARSE_ERROR")?;
    let control_kind = c.enumerated("control_kind", &["POSITIVE", "NEGATIVE"])?;
    let expected_outcome = c.enumerated("expected_outcome", &["PASS", "FAIL"])?;
    let expected_codes = parse_list(c.take("expected_failure_codes")?)?;
    let min_bound_kind = c.enumerated("min_bound_kind", &["RIGOROUS", "ESTIMATED", "NONE"])?;
    let tol_constraint = parse_scaled(c.take("tol_constraint_residual")?)?;
    let tol_povm = parse_scaled(c.take("tol_povm_residual")?)?;
    let tol_probability = parse_scaled(c.take("tol_probability")?)?;
    let tol_zero = parse_scaled(c.take("tol_zero_probability")?)?;
    let tol_schrodinger = parse_scaled(c.take("tol_schrodinger")?)?;
    c.fixed("end", "acceptance", "CASE_PARSE_ERROR")?;
    let cid = c.take("case_id")?.to_string();
    let aid = c.take("acceptance_id")?.to_string();
    if !is_digest(&cid) || !is_digest(&aid) {
        return refuse("CASE_PARSE_ERROR", "digest");
    }
    c.fixed("end", "", "CASE_PARSE_ERROR")?;
    if c.pos != c.lines.len() {
        return refuse("CASE_PARSE_ERROR", "trailing lines");
    }

    // Rule 3: ranges and fixed values.
    if !(2..=64).contains(&clock_dim_i) {
        return refuse("CASE_INVALID_PARAMETER", "clock_dim");
    }
    let clock_dim = clock_dim_i as usize;
    if energies.len() != clock_dim || energies.windows(2).any(|w| w[0] >= w[1]) {
        return refuse("CASE_INVALID_PARAMETER", "clock_energies");
    }
    if pauli_v.len() != 4 {
        return refuse("CASE_INVALID_PARAMETER", "system_hamiltonian_pauli");
    }
    if state_v.len() != 2 || state_v.iter().all(|(a, b)| a.is_zero() && b.is_zero()) {
        return refuse("CASE_INVALID_PARAMETER", "reference_system_state");
    }
    if tau <= Rat::zero() || weight <= Rat::zero() {
        return refuse("CASE_INVALID_PARAMETER", "povm");
    }
    if !(1..=256).contains(&label_count) || labels.len() != label_count as usize {
        return refuse("CASE_INVALID_PARAMETER", "clock labels");
    }
    let mut sorted = labels.clone();
    sorted.sort();
    sorted.dedup();
    if sorted.len() != labels.len() {
        return refuse("CASE_INVALID_PARAMETER", "duplicate clock label");
    }
    let ref_index = match labels.iter().position(|l| *l == ref_label) {
        Some(i) => i,
        None => return refuse("CASE_INVALID_PARAMETER", "reference_clock_label"),
    };
    // Rule 4: rational spectrum.
    let pauli = [pauli_v[0], pauli_v[1], pauli_v[2], pauli_v[3]];
    let hh = pauli[1].mul(&pauli[1]).add(&pauli[2].mul(&pauli[2])).add(&pauli[3].mul(&pauli[3]));
    let h_norm = match hh.sqrt_exact() {
        Some(r) => r,
        None => return refuse("CASE_IRRATIONAL_SPECTRUM", hh.text()),
    };
    let eigenvalues = [pauli[0].add(&h_norm), pauli[0].sub(&h_norm)];
    // Rule 5: identities.
    let text = std::str::from_utf8(data).unwrap();
    let semantic_block = block(text, "semantic");
    let acceptance_block = block(text, "acceptance");
    if case_id(&semantic_block) != cid || acceptance_id(&acceptance_block) != aid {
        return refuse("CASE_ID_MISMATCH", "");
    }
    // Rule 6: acceptance.
    if expected_outcome == "PASS" {
        if !expected_codes.is_empty() {
            return refuse("CASE_INVALID_PARAMETER", "expected_failure_codes");
        }
    } else {
        let mut s = expected_codes.clone();
        s.sort();
        s.dedup();
        if expected_codes.is_empty() || s != expected_codes || expected_codes.iter().any(|x| !FAILURE_CODES.contains(&x.as_str())) {
            return refuse("CASE_INVALID_PARAMETER", "expected_failure_codes");
        }
    }
    Ok(Case {
        name,
        clock_dim,
        energies,
        pauli,
        ref_label,
        ref_index,
        state: [state_v[0], state_v[1]],
        tau,
        weight,
        labels,
        control_kind,
        expected_outcome,
        expected_codes,
        min_bound_kind,
        tol_constraint,
        tol_povm,
        tol_probability,
        tol_zero,
        tol_schrodinger,
        case_id: cid,
        acceptance_id: aid,
        semantic_block,
        acceptance_block,
        file_sha256: sha256_hex(data),
        h_norm,
        eigenvalues,
    })
}

/// Exact kernel of H_total: pairs (j, s) with E_j + e_s = 0; s = 0 is h0 + |h|.
pub fn kernel_pairs(c: &Case) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    for (j, e) in c.energies.iter().enumerate() {
        for s in 0..2 {
            if e.add(&c.eigenvalues[s]).is_zero() {
                out.push((j, s));
            }
        }
    }
    out
}

/// Acceptance parameters for `build_case`.
#[derive(Clone)]
pub struct Acceptance {
    pub control_kind: &'static str,
    pub expected_outcome: &'static str,
    pub expected_codes: Vec<&'static str>,
    pub min_bound_kind: &'static str,
    pub tol: [&'static str; 5],
}

impl Acceptance {
    pub fn positive() -> Acceptance {
        Acceptance { control_kind: "POSITIVE", expected_outcome: "PASS", expected_codes: vec![], min_bound_kind: "ESTIMATED", tol: ["1@12", "1@12", "1@12", "1@9", "1@12"] }
    }
    pub fn negative(codes: &[&'static str]) -> Acceptance {
        Acceptance { control_kind: "NEGATIVE", expected_outcome: "FAIL", expected_codes: codes.to_vec(), ..Acceptance::positive() }
    }
}

/// Emit canonical case bytes from exact parameters (fixtures and tests).
pub fn build_case(name: &str, energies: &[Rat], pauli: [Rat; 4], ref_label: &str, state: [(Rat, Rat); 2], tau: Rat, weight: Rat, labels: &[&str], acc: &Acceptance) -> Vec<u8> {
    let mut sem = vec![
        "begin semantic".to_string(),
        "model_family PAGE_WOOTTERS_FINITE_IDEAL".into(),
        "energy_unit DIMENSIONLESS_HBAR_1".into(),
        format!("clock_dim {}", energies.len()),
        format!("clock_energies {}", energies.iter().map(Rat::text).collect::<Vec<_>>().join(",")),
        "system_dim 2".into(),
        format!("system_hamiltonian_pauli {}", pauli.iter().map(Rat::text).collect::<Vec<_>>().join(",")),
        "interaction NONE".into(),
        "constraint SUM_HC_HS".into(),
        "physical_state NULLSPACE_PROJECTION".into(),
        format!("reference_clock_label {}", ref_label),
        format!("reference_system_state ({};{}),({};{})", state[0].0.text(), state[0].1.text(), state[1].0.text(), state[1].1.text()),
        "clock_povm COVARIANT_DISCRETE".into(),
        format!("povm_tau_turns {}", tau.text()),
        format!("povm_weight {}", weight.text()),
        format!("clock_label_count {}", labels.len()),
    ];
    for (k, lb) in labels.iter().enumerate() {
        sem.push(format!("clock_label {} {}", k, lb));
    }
    sem.push("observables PAULI_X,PAULI_Y,PAULI_Z".into());
    sem.push("end semantic".into());
    let codes = if acc.expected_codes.is_empty() { "none".to_string() } else { acc.expected_codes.join(",") };
    let accl = [
        "begin acceptance".to_string(),
        format!("control_kind {}", acc.control_kind),
        format!("expected_outcome {}", acc.expected_outcome),
        format!("expected_failure_codes {}", codes),
        format!("min_bound_kind {}", acc.min_bound_kind),
        format!("tol_constraint_residual {}", acc.tol[0]),
        format!("tol_povm_residual {}", acc.tol[1]),
        format!("tol_probability {}", acc.tol[2]),
        format!("tol_zero_probability {}", acc.tol[3]),
        format!("tol_schrodinger {}", acc.tol[4]),
        "end acceptance".into(),
    ];
    let sem_b: String = sem.iter().map(|l| format!("{}\n", l)).collect();
    let acc_b: String = accl.iter().map(|l| format!("{}\n", l)).collect();
    let head = format!("OMEGA-AT0-CASE v1\ndomain omega.at0.case.v1\ncontract AT0_CASE_V1\ncase_name {}\n", name);
    let tail = format!("case_id {}\nacceptance_id {}\nend\n", case_id(&sem_b), acceptance_id(&acc_b));
    format!("{}{}{}{}", head, sem_b, acc_b, tail).into_bytes()
}
