// at1h.rs: AT-1 Agent 7 hidden-set generator and exact checker.
// Rust std only. No crates, no Cargo, no Python. Built with: rustc -O at1h.rs
// All quantities are exact rationals or Gaussian rationals (own bignum below).
// Modes:
//   at1h selftest <worked_example_file>   digest self-test + spec table self-tests
//   at1h gen <seed_hex> <out_dir>         deterministic hidden set from a 64-bit seed
use std::cmp::Ordering;
use std::io::Write;
use std::process::{Command, Stdio};

// ---------------------------------------------------------------- bignum
#[derive(Clone, PartialEq, Eq, Debug)]
struct Nat(Vec<u32>);

impl Nat {
    fn zero() -> Nat { Nat(Vec::new()) }
    fn from_u64(x: u64) -> Nat { let mut n = Nat(vec![x as u32, (x >> 32) as u32]); n.trim(); n }
    fn trim(&mut self) { while let Some(&0) = self.0.last() { self.0.pop(); } }
    fn is_zero(&self) -> bool { self.0.is_empty() }
    fn cmpn(&self, o: &Nat) -> Ordering {
        if self.0.len() != o.0.len() { return self.0.len().cmp(&o.0.len()); }
        for i in (0..self.0.len()).rev() {
            if self.0[i] != o.0[i] { return self.0[i].cmp(&o.0[i]); }
        }
        Ordering::Equal
    }
    fn add(&self, o: &Nat) -> Nat {
        let n = self.0.len().max(o.0.len());
        let mut r = Vec::with_capacity(n + 1);
        let mut c = 0u64;
        for i in 0..n {
            let s = *self.0.get(i).unwrap_or(&0) as u64 + *o.0.get(i).unwrap_or(&0) as u64 + c;
            r.push(s as u32);
            c = s >> 32;
        }
        if c > 0 { r.push(c as u32); }
        let mut x = Nat(r); x.trim(); x
    }
    fn sub(&self, o: &Nat) -> Nat {
        assert!(self.cmpn(o) != Ordering::Less, "Nat::sub underflow");
        let mut r = Vec::with_capacity(self.0.len());
        let mut b = 0i64;
        for i in 0..self.0.len() {
            let mut d = self.0[i] as i64 - *o.0.get(i).unwrap_or(&0) as i64 - b;
            if d < 0 { d += 1i64 << 32; b = 1; } else { b = 0; }
            r.push(d as u32);
        }
        assert!(b == 0);
        let mut x = Nat(r); x.trim(); x
    }
    fn mul(&self, o: &Nat) -> Nat {
        if self.is_zero() || o.is_zero() { return Nat::zero(); }
        let (n, m) = (self.0.len(), o.0.len());
        let mut r = vec![0u32; n + m + 1];
        for i in 0..n {
            let a = self.0[i] as u64;
            let mut carry = 0u64;
            for j in 0..m {
                let t = a * (o.0[j] as u64) + r[i + j] as u64 + carry;
                r[i + j] = t as u32;
                carry = t >> 32;
            }
            let mut k = i + m;
            while carry > 0 {
                let t = r[k] as u64 + carry;
                r[k] = t as u32;
                carry = t >> 32;
                k += 1;
            }
        }
        let mut x = Nat(r); x.trim(); x
    }
    fn bits(&self) -> usize {
        if self.is_zero() { 0 } else { self.0.len() * 32 - self.0.last().unwrap().leading_zeros() as usize }
    }
    fn bit(&self, i: usize) -> bool { let w = i / 32; w < self.0.len() && (self.0[w] >> (i % 32)) & 1 == 1 }
    fn shl1_or(&self, b: bool) -> Nat {
        let mut r = Vec::with_capacity(self.0.len() + 1);
        let mut c = b as u32;
        for &x in &self.0 { r.push((x << 1) | c); c = x >> 31; }
        if c != 0 { r.push(c); }
        let mut n = Nat(r); n.trim(); n
    }
    fn divrem(&self, d: &Nat) -> (Nat, Nat) {
        assert!(!d.is_zero(), "division by zero");
        if self.cmpn(d) == Ordering::Less { return (Nat::zero(), self.clone()); }
        let nb = self.bits();
        let mut q = vec![0u32; self.0.len()];
        let mut r = Nat::zero();
        for i in (0..nb).rev() {
            r = r.shl1_or(self.bit(i));
            if r.cmpn(d) != Ordering::Less { r = r.sub(d); q[i / 32] |= 1 << (i % 32); }
        }
        let mut qn = Nat(q); qn.trim();
        (qn, r)
    }
    fn gcd(a: &Nat, b: &Nat) -> Nat {
        let (mut x, mut y) = (a.clone(), b.clone());
        while !y.is_zero() { let (_, r) = x.divrem(&y); x = y; y = r; }
        x
    }
    fn pow2(k: usize) -> Nat { let mut v = vec![0u32; k / 32 + 1]; v[k / 32] = 1 << (k % 32); Nat(v) }
    fn isqrt(&self) -> Nat {
        if self.is_zero() { return Nat::zero(); }
        let mut x = Nat::pow2(self.bits() / 2 + 1);
        let two = Nat::from_u64(2);
        loop {
            let (q, _) = self.divrem(&x);
            let (y, _) = x.add(&q).divrem(&two);
            if y.cmpn(&x) != Ordering::Less { return x; }
            x = y;
        }
    }
    fn to_dec(&self) -> String {
        if self.is_zero() { return "0".to_string(); }
        let base = Nat::from_u64(1_000_000_000);
        let mut parts = Vec::new();
        let mut x = self.clone();
        while !x.is_zero() {
            let (q, r) = x.divrem(&base);
            parts.push(if r.is_zero() { 0 } else { r.0[0] });
            x = q;
        }
        let mut s = format!("{}", parts.last().unwrap());
        for p in parts.iter().rev().skip(1) { s.push_str(&format!("{:09}", p)); }
        s
    }
    fn to_f64(&self) -> f64 { self.0.iter().rev().fold(0.0, |acc, &l| acc * 4294967296.0 + l as f64) }
}

#[derive(Clone, PartialEq, Eq, Debug)]
struct Int { neg: bool, mag: Nat }

impl Int {
    fn mk(neg: bool, mag: Nat) -> Int { let neg = neg && !mag.is_zero(); Int { neg, mag } }
    fn from_i64(x: i64) -> Int { Int::mk(x < 0, Nat::from_u64(x.unsigned_abs())) }
    fn pos(m: &Nat) -> Int { Int::mk(false, m.clone()) }
    fn is_zero(&self) -> bool { self.mag.is_zero() }
    fn negate(&self) -> Int { Int::mk(!self.neg, self.mag.clone()) }
    fn add(&self, o: &Int) -> Int {
        if self.neg == o.neg { return Int::mk(self.neg, self.mag.add(&o.mag)); }
        match self.mag.cmpn(&o.mag) {
            Ordering::Less => Int::mk(o.neg, o.mag.sub(&self.mag)),
            _ => Int::mk(self.neg, self.mag.sub(&o.mag)),
        }
    }
    fn mul(&self, o: &Int) -> Int { Int::mk(self.neg != o.neg, self.mag.mul(&o.mag)) }
    fn cmpi(&self, o: &Int) -> Ordering {
        match (self.neg, o.neg) {
            (false, true) => Ordering::Greater,
            (true, false) => Ordering::Less,
            (false, false) => self.mag.cmpn(&o.mag),
            (true, true) => o.mag.cmpn(&self.mag),
        }
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
struct Q { n: Int, d: Nat }

impl Q {
    fn new(n: Int, d: Nat) -> Q {
        assert!(!d.is_zero());
        if n.is_zero() { return Q { n: Int::from_i64(0), d: Nat::from_u64(1) }; }
        let g = Nat::gcd(&n.mag, &d);
        let (nm, _) = n.mag.divrem(&g);
        let (dm, _) = d.divrem(&g);
        Q { n: Int::mk(n.neg, nm), d: dm }
    }
    fn r(a: i64, b: i64) -> Q {
        assert!(b != 0);
        Q::new(Int::mk((a < 0) != (b < 0), Nat::from_u64(a.unsigned_abs())), Nat::from_u64(b.unsigned_abs()))
    }
    fn i(a: i64) -> Q { Q::r(a, 1) }
    fn zero() -> Q { Q::i(0) }
    fn is_zero(&self) -> bool { self.n.is_zero() }
    fn add(&self, o: &Q) -> Q { Q::new(self.n.mul(&Int::pos(&o.d)).add(&o.n.mul(&Int::pos(&self.d))), self.d.mul(&o.d)) }
    fn neg(&self) -> Q { Q { n: self.n.negate(), d: self.d.clone() } }
    fn sub(&self, o: &Q) -> Q { self.add(&o.neg()) }
    fn mul(&self, o: &Q) -> Q { Q::new(self.n.mul(&o.n), self.d.mul(&o.d)) }
    fn div(&self, o: &Q) -> Q {
        assert!(!o.is_zero(), "Q div by zero");
        Q::new(Int::mk(self.n.neg != o.n.neg, self.n.mag.mul(&o.d)), self.d.mul(&o.n.mag))
    }
    fn abs(&self) -> Q { Q { n: Int::pos(&self.n.mag), d: self.d.clone() } }
    fn cmpq(&self, o: &Q) -> Ordering { self.n.mul(&Int::pos(&o.d)).cmpi(&o.n.mul(&Int::pos(&self.d))) }
    fn is_neg(&self) -> bool { self.n.neg }
    // |self| >= num/den
    fn abs_ge(&self, num: u64, den: u64) -> bool {
        self.n.mag.mul(&Nat::from_u64(den)).cmpn(&Nat::from_u64(num).mul(&self.d)) != Ordering::Less
    }
    fn to_s(&self) -> String { format!("{}{}/{}", if self.n.neg { "-" } else { "" }, self.n.mag.to_dec(), self.d.to_dec()) }
    fn to_f64(&self) -> f64 { let v = self.n.mag.to_f64() / self.d.to_f64(); if self.n.neg { -v } else { v } }
    fn in_token_limit(&self) -> bool {
        let lim = Nat::from_u64(1048576);
        self.n.mag.cmpn(&lim) != Ordering::Greater && self.d.cmpn(&lim) != Ordering::Greater
    }
    fn sqrt_exact(&self) -> Option<Q> {
        if self.is_neg() { return None; }
        let a = self.n.mag.isqrt();
        let b = self.d.isqrt();
        if a.mul(&a) != self.n.mag || b.mul(&b) != self.d { return None; }
        Some(Q::new(Int::pos(&a), b))
    }
    // exact integer value if this rational is an integer
    fn as_int(&self) -> Option<i64> {
        if self.d != Nat::from_u64(1) { return None; }
        let m = self.n.mag.to_f64();
        if m > 1e15 { return None; }
        let v = m as i64;
        Some(if self.n.neg { -v } else { v })
    }
}

#[derive(Clone, PartialEq, Eq, Debug)]
struct G { re: Q, im: Q }

impl G {
    fn new(re: Q, im: Q) -> G { G { re, im } }
    fn re(q: Q) -> G { G { re: q, im: Q::zero() } }
    fn zero() -> G { G::re(Q::zero()) }
    fn is_zero(&self) -> bool { self.re.is_zero() && self.im.is_zero() }
    fn add(&self, o: &G) -> G { G::new(self.re.add(&o.re), self.im.add(&o.im)) }
    fn sub(&self, o: &G) -> G { G::new(self.re.sub(&o.re), self.im.sub(&o.im)) }
    fn mul(&self, o: &G) -> G {
        G::new(self.re.mul(&o.re).sub(&self.im.mul(&o.im)), self.re.mul(&o.im).add(&self.im.mul(&o.re)))
    }
    fn conj(&self) -> G { G::new(self.re.clone(), self.im.neg()) }
    fn norm2(&self) -> Q { self.re.mul(&self.re).add(&self.im.mul(&self.im)) }
    fn scale(&self, q: &Q) -> G { G::new(self.re.mul(q), self.im.mul(q)) }
    fn ipow(m: i64) -> G {
        match m.rem_euclid(4) {
            0 => G::re(Q::i(1)),
            1 => G::new(Q::zero(), Q::i(1)),
            2 => G::re(Q::i(-1)),
            _ => G::new(Q::zero(), Q::i(-1)),
        }
    }
    fn to_s(&self) -> String { format!("({};{})", self.re.to_s(), self.im.to_s()) }
}

type V2 = [G; 2];
fn v2_add(a: &V2, b: &V2) -> V2 { [a[0].add(&b[0]), a[1].add(&b[1])] }
fn v2_scale_g(a: &V2, g: &G) -> V2 { [a[0].mul(g), a[1].mul(g)] }
fn v2_norm2(a: &V2) -> Q { a[0].norm2().add(&a[1].norm2()) }
fn v2_inner(a: &V2, b: &V2) -> G { a[0].conj().mul(&b[0]).add(&a[1].conj().mul(&b[1])) } // <a|b>
fn v2_zero() -> V2 { [G::zero(), G::zero()] }
fn v2_is_zero(a: &V2) -> bool { a[0].is_zero() && a[1].is_zero() }

// ---------------------------------------------------------------- sha256 via shasum
fn sha256_hex(bytes: &[u8]) -> String {
    let mut child = Command::new("/usr/bin/shasum").args(["-a", "256"])
        .stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().expect("spawn shasum");
    child.stdin.take().unwrap().write_all(bytes).unwrap();
    let out = child.wait_with_output().expect("shasum");
    let s = String::from_utf8(out.stdout).unwrap();
    let h = s[..64].to_string();
    assert!(h.chars().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()));
    h
}

fn block(lines: &[String], begin: &str, end: &str) -> Vec<u8> {
    let b = lines.iter().position(|l| l == begin).expect("begin");
    let e = lines.iter().position(|l| l == end).expect("end");
    let mut v = Vec::new();
    for l in &lines[b..=e] { v.extend_from_slice(l.as_bytes()); v.push(b'\n'); }
    v
}

fn tagged(tag: &str, body: &[u8]) -> String {
    let mut v = tag.as_bytes().to_vec();
    v.push(0);
    v.extend_from_slice(body);
    sha256_hex(&v)
}

// recompute case_id and acceptance_id lines from the blocks (AT1_CASE_V1 section 5)
fn fix_ids(text: &str) -> String {
    let mut lines: Vec<String> = text.split_terminator('\n').map(|s| s.to_string()).collect();
    let cid = tagged("omega.at1.case.v1", &block(&lines, "begin semantic", "end semantic"));
    let aid = tagged("omega.at1.acceptance.v1", &block(&lines, "begin acceptance", "end acceptance"));
    for l in lines.iter_mut() {
        if l.starts_with("case_id ") { *l = format!("case_id {}", cid); }
        if l.starts_with("acceptance_id ") { *l = format!("acceptance_id {}", aid); }
    }
    let mut s = lines.join("\n");
    s.push('\n');
    s
}

// ---------------------------------------------------------------- case model
#[derive(Clone, Debug)]
struct Case {
    name: String,
    e: Vec<Q>,
    h0: Q,
    h: [Q; 3],
    v: Vec<[Q; 3]>,
    r: usize,
    psi: V2,
    tau: Q,
    w: Q,
    m: usize,
    control: String,
    target: String,
    outcome: String,
    codes: Vec<String>,
}

fn render(c: &Case) -> String {
    let mut s = String::new();
    let mut p = |l: String| { s.push_str(&l); s.push('\n'); };
    p("OMEGA-AT1-CASE v1".into());
    p("domain omega.at1.case.v1".into());
    p("contract AT1_CASE_V1".into());
    p(format!("case_name {}", c.name));
    p("begin semantic".into());
    p("model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG".into());
    p("energy_unit DIMENSIONLESS_HBAR_1".into());
    p(format!("clock_dim {}", c.e.len()));
    p(format!("clock_energies {}", c.e.iter().map(|q| q.to_s()).collect::<Vec<_>>().join(",")));
    p("system_dim 2".into());
    p(format!("system_hamiltonian_pauli {},{},{},{}", c.h0.to_s(), c.h[0].to_s(), c.h[1].to_s(), c.h[2].to_s()));
    p("interaction CLOCK_DIAGONAL_PAULI".into());
    for (j, v) in c.v.iter().enumerate() {
        p(format!("interaction_pauli {} {},{},{}", j, v[0].to_s(), v[1].to_s(), v[2].to_s()));
    }
    p("constraint SUM_HC_HS_V".into());
    p("physical_state NULLSPACE_PROJECTION".into());
    p(format!("reference_clock_label t{}", c.r));
    p(format!("reference_system_state {},{}", c.psi[0].to_s(), c.psi[1].to_s()));
    p("clock_povm COVARIANT_DISCRETE".into());
    p(format!("povm_tau_turns {}", c.tau.to_s()));
    p(format!("povm_weight {}", c.w.to_s()));
    p(format!("clock_label_count {}", c.m));
    for k in 0..c.m { p(format!("clock_label {} t{}", k, k)); }
    p("observables PAULI_X,PAULI_Y,PAULI_Z".into());
    p("end semantic".into());
    p("begin acceptance".into());
    p(format!("control_kind {}", c.control));
    p(format!("prediction_target {}", c.target));
    p(format!("expected_outcome {}", c.outcome));
    p(format!("expected_failure_codes {}", if c.codes.is_empty() { "none".to_string() } else { c.codes.join(",") }));
    p("min_bound_kind ESTIMATED".into());
    p("tol_constraint_residual 1@12".into());
    p("tol_povm_residual 1@12".into());
    p("tol_probability 1@12".into());
    p("tol_zero_probability 1@9".into());
    p("tol_schrodinger 1@12".into());
    p("end acceptance".into());
    p("case_id X".into());
    p("acceptance_id X".into());
    p("end".into());
    fix_ids(&s)
}

fn parse_q(t: &str) -> Result<Q, String> {
    let (a, b) = t.split_once('/').ok_or(format!("not rational: {}", t))?;
    let a: i64 = a.parse().map_err(|_| format!("bad num {}", t))?;
    let b: i64 = b.parse().map_err(|_| format!("bad den {}", t))?;
    let q = Q::r(a, b);
    if q.to_s() != t { return Err(format!("noncanonical {}", t)); }
    Ok(q)
}
fn parse_g(t: &str) -> Result<G, String> {
    let inner = t.strip_prefix('(').and_then(|x| x.strip_suffix(')')).ok_or("bad complex")?;
    let (a, b) = inner.split_once(';').ok_or("bad complex")?;
    Ok(G::new(parse_q(a)?, parse_q(b)?))
}

// strict reader for files of the AT1_CASE_V1 shape written by this tool (and the worked example)
fn parse_case(text: &str) -> Result<Case, String> {
    if !text.ends_with('\n') || text.contains('\r') { return Err("bad line endings".into()); }
    let lines: Vec<&str> = text.split_terminator('\n').collect();
    let mut i = 0usize;
    let mut take = |key: &str| -> Result<String, String> {
        let l = *lines.get(i).ok_or("short file")?;
        i += 1;
        if key.ends_with(' ') {
            l.strip_prefix(key).map(|s| s.to_string()).ok_or(format!("line {}: expected {}", i, key))
        } else if l == key { Ok(String::new()) } else { Err(format!("line {}: expected {}", i, key)) }
    };
    take("OMEGA-AT1-CASE v1")?; take("domain omega.at1.case.v1")?; take("contract AT1_CASE_V1")?;
    let name = take("case_name ")?;
    take("begin semantic")?; take("model_family PAGE_WOOTTERS_FINITE_CLOCKDIAG")?; take("energy_unit DIMENSIONLESS_HBAR_1")?;
    let n: usize = take("clock_dim ")?.parse().map_err(|_| "clock_dim")?;
    let e: Vec<Q> = take("clock_energies ")?.split(',').map(parse_q).collect::<Result<_, _>>()?;
    if e.len() != n { return Err("energies count".into()); }
    take("system_dim 2")?;
    let hs: Vec<Q> = take("system_hamiltonian_pauli ")?.split(',').map(parse_q).collect::<Result<_, _>>()?;
    if hs.len() != 4 { return Err("hs".into()); }
    take("interaction CLOCK_DIAGONAL_PAULI")?;
    let mut v = Vec::new();
    for j in 0..n {
        let rest = take(&format!("interaction_pauli {} ", j))?;
        let c: Vec<Q> = rest.split(',').map(parse_q).collect::<Result<_, _>>()?;
        if c.len() != 3 { return Err("v".into()); }
        v.push([c[0].clone(), c[1].clone(), c[2].clone()]);
    }
    take("constraint SUM_HC_HS_V")?; take("physical_state NULLSPACE_PROJECTION")?;
    let rl = take("reference_clock_label ")?;
    let ps: Vec<G> = take("reference_system_state ")?.split(',').map(parse_g).collect::<Result<_, _>>()?;
    if ps.len() != 2 { return Err("psi".into()); }
    take("clock_povm COVARIANT_DISCRETE")?;
    let tau = parse_q(&take("povm_tau_turns ")?)?;
    let w = parse_q(&take("povm_weight ")?)?;
    let m: usize = take("clock_label_count ")?.parse().map_err(|_| "m")?;
    let mut labels = Vec::new();
    for k in 0..m { labels.push(take(&format!("clock_label {} ", k))?); }
    take("observables PAULI_X,PAULI_Y,PAULI_Z")?; take("end semantic")?; take("begin acceptance")?;
    let control = take("control_kind ")?;
    let target = take("prediction_target ")?;
    let outcome = take("expected_outcome ")?;
    let codes_s = take("expected_failure_codes ")?;
    let codes: Vec<String> = if codes_s == "none" { vec![] } else { codes_s.split(',').map(|s| s.to_string()).collect() };
    take("min_bound_kind ESTIMATED")?;
    take("tol_constraint_residual 1@12")?; take("tol_povm_residual 1@12")?; take("tol_probability 1@12")?;
    take("tol_zero_probability 1@9")?; take("tol_schrodinger 1@12")?; take("end acceptance")?;
    take("case_id ")?; take("acceptance_id ")?; take("end")?;
    if i != lines.len() { return Err("trailing lines".into()); }
    let r = labels.iter().position(|l| *l == rl).ok_or("reference label missing")?;
    Ok(Case { name, e, h0: hs[0].clone(), h: [hs[1].clone(), hs[2].clone(), hs[3].clone()], v, r,
        psi: [ps[0].clone(), ps[1].clone()], tau, w, m, control, target, outcome, codes })
}

// ---------------------------------------------------------------- exact physics (AT1_CASE_V1 s3, AT1_SPEC 13.1)
struct Exact {
    dim: usize,
    psi_zero: bool,
    p: Vec<Q>,                   // empty when trivial
    pauli: Vec<Option<[Q; 3]>>,  // plus-outcome X,Y,Z per label; None when p(k) = 0
    ideal: Vec<[Q; 3]>,
    povm_res2: Q,
}

fn eigvec(n: &[Q; 3], s: i64, rr: &Q) -> V2 {
    let sr = rr.mul(&Q::i(s));
    let a = [G::re(sr.add(&n[2])), G::new(n[0].clone(), n[1].clone())];
    if !v2_is_zero(&a) { return a; }
    [G::new(n[0].clone(), n[1].neg()), G::re(sr.sub(&n[2]))]
}

fn pauli_plus(c: &V2) -> [Q; 3] {
    let nrm = v2_norm2(c);
    let x = c[0].conj().mul(&c[1]);
    let half = Q::r(1, 2);
    [half.add(&x.re.div(&nrm)), half.add(&x.im.div(&nrm)), c[0].norm2().div(&nrm)]
}

// quarter-turn phase exp(2 pi i * turns) as i^(4*turns); error if 4*turns is not an integer
fn qphase(turns: &Q) -> Result<G, String> {
    let m = turns.mul(&Q::i(4)).as_int().ok_or(format!("phase turn count {} not a quarter multiple", turns.to_s()))?;
    Ok(G::ipow(m))
}

fn level_norm(h: &[Q; 3], v: &[Q; 3]) -> ([Q; 3], Q) {
    let n = [h[0].add(&v[0]), h[1].add(&v[1]), h[2].add(&v[2])];
    let r2 = n[0].mul(&n[0]).add(&n[1].mul(&n[1])).add(&n[2].mul(&n[2]));
    (n, r2)
}

fn eval(c: &Case) -> Result<Exact, String> {
    let nn = c.e.len();
    if nn < 2 || nn > 64 || c.v.len() != nn { return Err("clock_dim".into()); }
    for j in 1..nn { if c.e[j].cmpq(&c.e[j - 1]) != Ordering::Greater { return Err("energies not increasing".into()); } }
    if c.m < 1 || c.m > 256 || c.r >= c.m { return Err("labels".into()); }
    if c.tau.is_neg() || c.tau.is_zero() || c.w.is_neg() || c.w.is_zero() { return Err("tau/w".into()); }
    if v2_is_zero(&c.psi) { return Err("psi zero".into()); }
    let mut dim = 0usize;
    let mut u: Vec<V2> = vec![v2_zero(); nn];
    let mut any_nonzero = false;
    for j in 0..nn {
        let (n, r2) = level_norm(&c.h, &c.v[j]);
        let rr = r2.sqrt_exact().ok_or(format!("level {} irrational spectrum", j))?;
        let ej = c.e[j].add(&c.h0);
        if rr.is_zero() {
            if ej.is_zero() { dim += 2; u[j] = c.psi.clone(); any_nonzero = true; }
        } else {
            for s in [1i64, -1] {
                if ej.add(&rr.mul(&Q::i(s))).is_zero() {
                    dim += 1;
                    let f = eigvec(&n, s, &rr);
                    let amp = v2_inner(&f, &c.psi);
                    if !amp.is_zero() { any_nonzero = true; }
                    u[j] = v2_add(&u[j], &v2_scale_g(&f, &amp).map(|g| g.scale(&Q::i(1).div(&v2_norm2(&f)))));
                }
            }
        }
    }
    let psi_zero = dim > 0 && !any_nonzero;
    let trivial = dim == 0 || psi_zero;
    let mut p = Vec::new();
    let mut pauli = Vec::new();
    if !trivial {
        let s_tot = u.iter().fold(Q::zero(), |a, x| a.add(&v2_norm2(x)));
        let a = (0..nn).find(|&j| !v2_is_zero(&u[j])).unwrap();
        for k in 0..c.m {
            let mut chi = v2_zero();
            let kr = Q::i(k as i64 - c.r as i64);
            for j in 0..nn {
                if v2_is_zero(&u[j]) { continue; }
                let ph = qphase(&c.e[j].sub(&c.e[a]).mul(&kr).mul(&c.tau))?;
                chi = v2_add(&chi, &v2_scale_g(&u[j], &ph));
            }
            let pk = c.w.mul(&v2_norm2(&chi)).div(&Q::i(nn as i64).mul(&s_tot));
            pauli.push(if pk.is_zero() { None } else { Some(pauli_plus(&chi)) });
            p.push(pk);
        }
    }
    // ideal reference: exp(-i H_S (t_k - t_r)) psi_0, H_S alone
    let (_, h2) = level_norm(&c.h, &[Q::zero(), Q::zero(), Q::zero()]);
    let hn = h2.sqrt_exact().ok_or("|h| irrational (not used by this generator)")?;
    let mut ideal = Vec::new();
    for k in 0..c.m {
        let st = if hn.is_zero() { c.psi.clone() } else {
            let gp = eigvec(&c.h, 1, &hn);
            let gm = eigvec(&c.h, -1, &hn);
            let pp = v2_scale_g(&gp, &v2_inner(&gp, &c.psi)).map(|g| g.scale(&Q::i(1).div(&v2_norm2(&gp))));
            let pm = v2_scale_g(&gm, &v2_inner(&gm, &c.psi)).map(|g| g.scale(&Q::i(1).div(&v2_norm2(&gm))));
            let turns = hn.mul(&Q::i(-2)).mul(&Q::i(k as i64 - c.r as i64)).mul(&c.tau);
            v2_add(&pm, &v2_scale_g(&pp, &qphase(&turns)?))
        };
        ideal.push(pauli_plus(&st));
    }
    // POVM residual squared: || sum_k F_k - I ||_F^2
    let mut res2 = Q::zero();
    let wn = c.w.div(&Q::i(nn as i64));
    for i in 0..nn {
        for j in 0..nn {
            let mut s = G::zero();
            for k in 0..c.m {
                s = s.add(&qphase(&c.e[i].sub(&c.e[j]).mul(&Q::i(-(k as i64))).mul(&c.tau))?);
            }
            let mut d = s.scale(&wn);
            if i == j { d = d.sub(&G::re(Q::i(1))); }
            res2 = res2.add(&d.norm2());
        }
    }
    Ok(Exact { dim, psi_zero, p, pauli, ideal, povm_res2: res2 })
}

// ---------------------------------------------------------------- judge (AT1_RESULT_V1 s4, s5) with the margin rule of AT1_SPEC 8.3
#[allow(dead_code)]
const CHECKS: [&str; 12] = ["bound_kind_sufficient", "values_finite", "physical_state_nontrivial", "constraint_residual",
    "povm_normalization", "clock_probability_sum", "probability_range", "pauli_pair_sum", "conditional_defined",
    "target_agreement", "oracle_cross_check", "label_status_agreement"];

struct Verdict { checks: Vec<&'static str>, outcome: &'static str, codes: Vec<String> }

// deviation d against tolerance 1e-12: Ok(false) if exactly 0, Ok(true) if >= 1e-9, Err otherwise
fn dev(d: &Q, what: &str) -> Result<bool, String> {
    if d.is_zero() { return Ok(false); }
    if d.abs_ge(1, 1_000_000_000) { return Ok(true); }
    Err(format!("margin rule violated: {} deviation {:e}", what, d.to_f64()))
}

fn judge(c: &Case, x: &Exact) -> Result<Verdict, String> {
    let mut ch: Vec<&'static str> = vec!["PASS"; 12];
    let mut codes: Vec<&str> = Vec::new();
    // 1 bound kind: min ESTIMATED is met by an ESTIMATED or RIGOROUS engine; 2 finite
    // 5 povm (evaluated even for a trivial kernel)
    let povm_fail = if x.povm_res2.is_zero() { false } else if x.povm_res2.abs_ge(1, 1_000_000_000_000_000_000) { true }
        else { return Err("margin rule violated: povm residual".into()); };
    if povm_fail { ch[4] = "FAIL"; codes.push("POVM_NORMALIZATION_EXCEEDED"); }
    let trivial = x.dim == 0 || x.psi_zero;
    if trivial {
        ch[2] = "FAIL"; codes.push("TRIVIAL_PHYSICAL_STATE");
        for i in [3usize, 5, 6, 7, 8, 9, 10, 11] { ch[i] = "NOT_EVALUATED"; }
    } else {
        for pk in &x.p { if !pk.is_zero() && !pk.abs_ge(1, 1000) { return Err(format!("margin rule violated: p(k) = {:e}", pk.to_f64())); } }
        let sum = x.p.iter().fold(Q::zero(), |a, b| a.add(b));
        if dev(&sum.sub(&Q::i(1)), "sum p")? { ch[5] = "FAIL"; codes.push("PROBABILITY_SUM_EXCEEDED"); }
        // 7 range: signed distance outside [0,1]
        let mut range_fail = false;
        let mut vals: Vec<Q> = x.p.clone();
        for pl in x.pauli.iter().flatten() { for q in pl { vals.push(q.clone()); vals.push(Q::i(1).sub(q)); } }
        for q in &vals {
            let out = if q.is_neg() { q.neg() } else if q.cmpq(&Q::i(1)) == Ordering::Greater { q.sub(&Q::i(1)) } else { Q::zero() };
            if dev(&out, "range")? { range_fail = true; }
        }
        if range_fail { ch[6] = "FAIL"; codes.push("PROBABILITY_OUT_OF_RANGE"); }
        // 8 pair sums: PLUS + MINUS = 1 exactly
        // 9 defined
        let defined: Vec<usize> = (0..c.m).filter(|&k| x.pauli[k].is_some()).collect();
        if defined.len() < c.m { ch[8] = "FAIL"; codes.push("CONDITIONAL_UNDEFINED"); }
        if defined.is_empty() { ch[7] = "NOT_EVALUATED"; }
        // 10 target agreement; 11 oracle cross-check (engine equals oracle exactly for a correct engine)
        if defined.is_empty() { ch[9] = "NOT_EVALUATED"; ch[10] = "NOT_EVALUATED"; }
        else if c.target == "IDEAL" {
            let wn = c.w.div(&Q::i(c.e.len() as i64));
            let mut f = false;
            for &k in &defined {
                let pl = x.pauli[k].as_ref().unwrap();
                for a in 0..3 { if dev(&pl[a].sub(&x.ideal[k][a]), "pauli vs ideal")? { f = true; } }
                if dev(&x.p[k].sub(&wn), "p vs w/N")? { f = true; }
            }
            if f { ch[9] = "FAIL"; codes.push("SCHRODINGER_DEVIATION_EXCEEDED"); }
        } else {
            ch[10] = "NOT_EVALUATED";
        }
        if c.target == "INTERACTING" { ch[10] = "NOT_EVALUATED"; }
        // 12 statuses agree (both sides apply the rule to the same exact p(k), margins keep them determinate)
    }
    codes.sort();
    codes.dedup();
    let outcome = if ch.iter().any(|s| *s == "FAIL") { "FAIL" } else { "PASS" };
    Ok(Verdict { checks: ch, outcome, codes: codes.iter().map(|s| s.to_string()).collect() })
}

// ---------------------------------------------------------------- self-tests against the frozen worked example and AT1_SPEC section 13
fn qv(v: &[(i64, i64)]) -> Vec<Q> { v.iter().map(|&(a, b)| Q::r(a, b)).collect() }
fn gq(a: Q, b: Q) -> G { G::new(a, b) }

fn spec_case(e: &[(i64, i64)], h0: (i64, i64), h: [(i64, i64); 3], v: &[[(i64, i64); 3]], psi: [(Q, Q); 2],
             r: usize, tau: (i64, i64), w: (i64, i64), m: usize, target: &str) -> Case {
    Case { name: "spec".into(), e: qv(e), h0: Q::r(h0.0, h0.1), h: [Q::r(h[0].0, h[0].1), Q::r(h[1].0, h[1].1), Q::r(h[2].0, h[2].1)],
        v: v.iter().map(|x| [Q::r(x[0].0, x[0].1), Q::r(x[1].0, x[1].1), Q::r(x[2].0, x[2].1)]).collect(), r,
        psi: [gq(psi[0].0.clone(), psi[0].1.clone()), gq(psi[1].0.clone(), psi[1].1.clone())],
        tau: Q::r(tau.0, tau.1), w: Q::r(w.0, w.1), m, control: "POSITIVE".into(), target: target.into(),
        outcome: "PASS".into(), codes: vec![] }
}

fn expect_table(label: &str, x: &Exact, p: &[(i64, i64)], xs: &[(i64, i64)], ys: &[(i64, i64)], zs: &[(i64, i64)]) -> usize {
    let mut bad = 0;
    for k in 0..p.len() {
        if x.p[k] != Q::r(p[k].0, p[k].1) { println!("FAIL {} p({}) = {} want {}/{}", label, k, x.p[k].to_s(), p[k].0, p[k].1); bad += 1; }
        if xs.is_empty() { continue; }
        let pl = x.pauli[k].as_ref().unwrap();
        let want = [Q::r(xs[k].0, xs[k].1), Q::r(ys[k].0, ys[k].1), Q::r(zs[k].0, zs[k].1)];
        for a in 0..3 { if pl[a] != want[a] { println!("FAIL {} k={} axis {} = {} want {}", label, k, a, pl[a].to_s(), want[a].to_s()); bad += 1; } }
    }
    bad
}
fn expect_ideal(label: &str, x: &Exact, rows: &[[(i64, i64); 3]]) -> usize {
    let mut bad = 0;
    for (k, row) in rows.iter().enumerate() {
        for a in 0..3 { if x.ideal[k][a] != Q::r(row[a].0, row[a].1) { println!("FAIL {} ideal k={} axis {} = {}", label, k, a, x.ideal[k][a].to_s()); bad += 1; } }
    }
    bad
}
fn expect_verdict(label: &str, c: &Case, x: &Exact, outcome: &str, codes: &[&str], checks: &str) -> usize {
    match judge(c, x) {
        Err(e) => { println!("FAIL {} judge error {}", label, e); 1 }
        Ok(v) => {
            let cv = v.checks.iter().map(|s| match *s { "PASS" => "P", "FAIL" => "F", "NOT_EVALUATED" => "N", _ => "I" }).collect::<String>();
            if v.outcome != outcome || v.codes != codes.iter().map(|s| s.to_string()).collect::<Vec<_>>() || cv != checks {
                println!("FAIL {} verdict {} {:?} {} (want {} {:?} {})", label, v.outcome, v.codes, cv, outcome, codes, checks); 1
            } else { println!("ok   {} verdict {} {:?} {}", label, outcome, codes, cv); 0 }
        }
    }
}

fn selftest(worked: &str) -> usize {
    let text = std::fs::read_to_string(worked).expect("read worked example");
    let mut bad = 0;
    let lines: Vec<String> = text.split_terminator('\n').map(|s| s.to_string()).collect();
    let cid = tagged("omega.at1.case.v1", &block(&lines, "begin semantic", "end semantic"));
    let aid = tagged("omega.at1.acceptance.v1", &block(&lines, "begin acceptance", "end acceptance"));
    let fsha = sha256_hex(text.as_bytes());
    println!("worked example: lines {} case_id {} acceptance_id {} file {}", lines.len(), cid, aid, fsha);
    if cid != "890980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1" { println!("FAIL case_id"); bad += 1; }
    if aid != "d63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f" { println!("FAIL acceptance_id"); bad += 1; }
    if fsha != "76e282fecf21025c9dd675378244e90ff192822a4c49d6cb812c9fe84b3f9ba4" { println!("FAIL file sha"); bad += 1; }
    let c = parse_case(&text).expect("parse worked");
    let re = render(&c);
    if re != text { println!("FAIL render(parse(worked)) is not byte-identical"); bad += 1; } else { println!("ok   render(parse(worked)) byte-identical"); }
    let x = eval(&c).unwrap();
    if x.dim != 2 { println!("FAIL P2 dim"); bad += 1; }
    bad += expect_table("P2", &x, &[(5, 28), (1, 4), (9, 28), (1, 4)], &[(49, 50), (29, 70), (1, 10), (29, 70)],
        &[(1, 2), (13, 14), (1, 2), (1, 14)], &[(16, 25), (26, 35), (4, 5), (26, 35)]);
    bad += expect_ideal("P2", &x, &[[(1, 1), (1, 2), (1, 2)], [(1, 2), (1, 1), (1, 2)], [(0, 1), (1, 2), (1, 2)], [(1, 2), (0, 1), (1, 2)]]);
    bad += expect_verdict("P2", &c, &x, "PASS", &[], "PPPPPPPPPPNP");
    let mut c1 = c.clone(); c1.target = "IDEAL".into();
    bad += expect_verdict("N1 (P2 target IDEAL)", &c1, &x, "FAIL", &["SCHRODINGER_DEVIATION_EXCEEDED"], "PPPPPPPPPFPP");

    let one = |a: i64, b: i64| (Q::r(a, b), Q::zero());
    let z = (0i64, 1i64);
    let b_e = [(-3, 2), (-1, 2), (1, 2), (3, 2)];
    let rho = [(3, 10), z, (-1, 10)];
    // P1
    let p1 = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, z]; 4], [one(1, 1), one(1, 1)], 0, (1, 4), (1, 1), 4, "IDEAL");
    let x = eval(&p1).unwrap();
    bad += expect_table("P1", &x, &[(1, 4); 4], &[(1, 1), (1, 2), (0, 1), (1, 2)], &[(1, 2), (1, 1), (1, 2), (0, 1)], &[(1, 2); 4]);
    bad += expect_verdict("P1b IDEAL", &p1, &x, "PASS", &[], "PPPPPPPPPPPP");
    // P1c
    let p1c = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, (1, 2)], [z, z, z], [z, z, z], rho], [one(1, 1), one(1, 1)], 0, (1, 4), (1, 1), 4, "IDEAL");
    let x = eval(&p1c).unwrap();
    bad += expect_verdict("P1c IDEAL", &p1c, &x, "PASS", &[], "PPPPPPPPPPPP");
    // P3
    let p3 = spec_case(&[(-1, 2), (1, 2), (3, 2)], z, [z, z, (1, 2)], &[rho, [z, (3, 10), (-1, 10)], [(6, 5), z, (2, 5)]],
        [one(1, 1), one(1, 1)], 0, (1, 4), (3, 4), 4, "INTERACTING");
    let x = eval(&p3).unwrap();
    if x.dim != 3 { println!("FAIL P3 dim"); bad += 1; }
    bad += expect_table("P3", &x, &[(107, 280), (53, 280), (5, 56), (19, 56)], &[(98, 107), (37, 53), (8, 25), (37, 95)],
        &[(65, 214), (101, 106), (37, 50), (17, 190)], &[(65, 214), (61, 106), (9, 10), (29, 38)]);
    bad += expect_verdict("P3", &p3, &x, "PASS", &[], "PPPPPPPPPPNP");
    let mut p3i = p3.clone(); p3i.target = "IDEAL".into();
    bad += expect_verdict("N1b (P3 IDEAL)", &p3i, &x, "FAIL", &["SCHRODINGER_DEVIATION_EXCEEDED"], "PPPPPPPPPFPP");
    // P3b
    let p3b = spec_case(&[(-1, 2), z, (1, 2)], z, [z, z, (1, 2)], &[[z, z, z], [z, z, (-1, 2)], rho],
        [one(1, 1), (Q::zero(), Q::i(1))], 0, (1, 2), (3, 4), 4, "INTERACTING");
    let x = eval(&p3b).unwrap();
    if x.dim != 4 { println!("FAIL P3b dim"); bad += 1; }
    bad += expect_table("P3b", &x, &[(41, 80), (19, 80), (1, 80), (19, 80)], &[(29, 82), (1, 38), (1, 2), (37, 38)],
        &[(40, 41), (10, 19), (0, 1), (10, 19)], &[(45, 82), (25, 38), (1, 2), (13, 38)]);
    bad += expect_ideal("P3b", &x, &[[(1, 2), (1, 1), (1, 2)], [(1, 2), (0, 1), (1, 2)], [(1, 2), (1, 1), (1, 2)], [(1, 2), (0, 1), (1, 2)]]);
    // P4
    let p4 = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, z], [z, z, z], rho, [z, z, z]],
        [one(1, 1), (Q::r(3, 5), Q::r(4, 5))], 1, (1, 4), (1, 1), 4, "INTERACTING");
    let x = eval(&p4).unwrap();
    bad += expect_table("P4", &x, &[(29, 164), (37, 164), (53, 164), (45, 164)], &[(277, 290), (197, 370), (37, 530), (13, 50)],
        &[(17, 58), (73, 74), (65, 106), (1, 10)], &[(73, 145), (113, 185), (193, 265), (17, 25)]);
    bad += expect_ideal("P4", &x, &[[(9, 10), (1, 5), (1, 2)], [(4, 5), (9, 10), (1, 2)], [(1, 10), (4, 5), (1, 2)], [(1, 5), (1, 10), (1, 2)]]);
    // P5
    let p5 = spec_case(&[(-524289, 1048574), (524285, 1048574)], (1, 524287), [z, z, (1, 2)],
        &[[z, z, z], [(524175, 1048354), (724, 524177), (-1, 2)]], [one(1, 1), one(1, 1)], 0, (1, 4), (1, 2), 4, "INTERACTING");
    let x = eval(&p5).unwrap();
    bad += expect_table("P5", &x, &[(524181, 2096716), (522731, 2096716), (524177, 2096716), (525627, 2096716)],
        &[(274763624041, 549527248074), (274761527333, 548007134774), (274759430625, 549523054658), (274761527333, 551043167958)],
        &[(274004612845, 549527248074), (274004612845, 548007134774), (275520532729, 549523054658), (275520532729, 551043167958)],
        &[(524180, 524181), (522730, 522731), (524176, 524177), (525626, 525627)]);
    bad += expect_verdict("P5", &p5, &x, "PASS", &[], "PPPPPPPPPPNP");
    let mut p5i = p5.clone(); p5i.target = "IDEAL".into();
    bad += expect_verdict("N1e (P5 IDEAL)", &p5i, &x, "FAIL", &["SCHRODINGER_DEVIATION_EXCEEDED"], "PPPPPPPPPFPP");
    // P6
    let p6 = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, (1, 1)], [z, z, (1, 1048576)], rho, [z, z, z]],
        [one(1, 1), one(1, 1)], 0, (1, 4), (1, 1), 4, "INTERACTING");
    let x = eval(&p6).unwrap();
    if x.dim != 2 { println!("FAIL P6 dim {}", x.dim); bad += 1; }
    bad += expect_table("P6", &x, &[(5, 28), (9, 28), (5, 28), (9, 28)], &[(49, 50), (1, 10), (49, 50), (1, 10)],
        &[(1, 2); 4], &[(16, 25), (4, 5), (16, 25), (4, 5)]);
    // N2
    let n2 = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, z], [z, z, (1, 2)], [z, z, (1, 2)], [z, z, z]], [one(1, 1), one(1, 1)], 0, (1, 4), (1, 1), 4, "INTERACTING");
    let x = eval(&n2).unwrap();
    bad += expect_verdict("N2", &n2, &x, "FAIL", &["TRIVIAL_PHYSICAL_STATE"], "PPFNPNNNNNNN");
    // N3
    let n3 = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, z], [z, z, z], [z, z, (-1, 1)], [z, z, z]], [one(1, 1), one(1, 1)], 0, (1, 4), (1, 1), 4, "INTERACTING");
    let x = eval(&n3).unwrap();
    bad += expect_table("N3", &x, &[(1, 2), (1, 4), (0, 1), (1, 4)], &[], &[], &[]);
    bad += expect_verdict("N3", &n3, &x, "FAIL", &["CONDITIONAL_UNDEFINED"], "PPPPPPPPFPNP");
    // N4a
    let mut n4a = c.clone(); n4a.w = Q::r(1, 2);
    let x = eval(&n4a).unwrap();
    bad += expect_table("N4a", &x, &[(5, 56), (1, 8), (9, 56), (1, 8)], &[], &[], &[]);
    if x.povm_res2 != Q::i(1) { println!("FAIL N4a povm_res2 {}", x.povm_res2.to_s()); bad += 1; }
    bad += expect_verdict("N4a", &n4a, &x, "FAIL", &["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"], "PPPPFFPPPPNP");
    // N6
    let n6 = spec_case(&b_e, z, [z, z, (1, 2)], &[[z, z, z], [z, z, (1, 2)], rho, [z, z, z]], [one(3, 1), one(1, 1)], 0, (1, 4), (1, 1), 4, "INTERACTING");
    let x = eval(&n6).unwrap();
    if !(x.dim == 1 && x.psi_zero) { println!("FAIL N6 dim/psi_zero"); bad += 1; }
    bad += expect_ideal("N6", &x, &[[(4, 5), (1, 2), (9, 10)], [(1, 2), (4, 5), (9, 10)], [(1, 5), (1, 2), (9, 10)], [(1, 2), (1, 5), (9, 10)]]);
    bad += expect_verdict("N6", &n6, &x, "FAIL", &["TRIVIAL_PHYSICAL_STATE"], "PPFNPNNNNNNN");
    // R4: per-level irrational spectrum is detected
    let mut r4 = c.clone(); r4.v[2] = [Q::r(1, 2), Q::zero(), Q::r(-1, 10)];
    match eval(&r4) { Err(e) if e.contains("irrational") => println!("ok   R4 irrational level detected"), _ => { println!("FAIL R4"); bad += 1; } }
    println!("selftest: {} failure(s)", bad);
    bad
}

// ---------------------------------------------------------------- deterministic generator
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 { // splitmix64
        self.0 = self.0.wrapping_add(0x9E3779B97F4A7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
        z ^ (z >> 31)
    }
    fn pick(&mut self, n: usize) -> usize { (self.next() % n as u64) as usize }
    fn coin(&mut self, num: u64, den: u64) -> bool { self.next() % den < num }
}

const TAUS: [(i64, i64); 5] = [(1, 4), (1, 8), (3, 4), (1, 2), (3, 8)];
const OFFS: [(i64, i64); 4] = [(0, 1), (1, 2), (-1, 3), (1, 5)];
const H0S: [(i64, i64); 4] = [(0, 1), (1, 3), (-1, 2), (2, 7)];
const DELTAS: [(i64, i64); 4] = [(1, 2), (1, 1), (1, 3), (3, 2)];
// rational unit axes (a, b, c)/d with a^2 + b^2 + c^2 = d^2
const QUADS: [(i64, i64, i64, i64); 14] = [(1, 2, 2, 3), (2, 3, 6, 7), (1, 4, 8, 9), (4, 4, 7, 9), (2, 6, 9, 11), (6, 6, 7, 11),
    (3, 4, 12, 13), (2, 10, 11, 15), (3, 0, 4, 5), (0, 5, 12, 13), (8, 0, 15, 17), (0, 0, 1, 1), (1, 0, 0, 1), (0, 1, 0, 1)];
const PERMS: [[usize; 3]; 6] = [[0, 1, 2], [0, 2, 1], [1, 0, 2], [1, 2, 0], [2, 0, 1], [2, 1, 0]];

fn psi_pool(i: usize) -> V2 {
    let g = |a: i64, b: i64, c: i64, d: i64| G::new(Q::r(a, b), Q::r(c, d));
    match i {
        0 => [g(1, 1, 0, 1), g(1, 1, 0, 1)],
        1 => [g(1, 1, 0, 1), g(0, 1, 1, 1)],
        2 => [g(2, 1, 0, 1), g(1, 1, -1, 1)],
        3 => [g(1, 1, 0, 1), g(3, 5, 4, 5)],
        4 => [g(3, 1, 0, 1), g(-1, 1, 0, 1)],
        5 => [g(1, 1, 1, 1), g(2, 1, 0, 1)],
        6 => [g(1, 1, 0, 1), g(0, 1, -2, 1)],
        _ => [g(5, 1, 0, 1), g(2, 1, 1, 1)],
    }
}
const PSI_COMPLEX: [usize; 6] = [1, 2, 3, 5, 6, 7];

fn axis(rng: &mut Rng, allow_z: bool) -> [Q; 3] {
    loop {
        let (a, b, c, d) = QUADS[rng.pick(QUADS.len())];
        let comps = [a, b, c];
        let p = PERMS[rng.pick(6)];
        let mut out = [Q::zero(), Q::zero(), Q::zero()];
        for k in 0..3 { let s = if rng.coin(1, 2) { -1 } else { 1 }; out[k] = Q::r(s * comps[p[k]], d); }
        let is_z = out[0].is_zero() && out[1].is_zero();
        if allow_z || !is_z { return out; }
    }
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Role { Zero, Rot, Mz, Spec, Deg, Near, Swap }

struct Clock { tau: Q, e: Vec<Q>, m: usize, w: Q }

fn clock(rng: &mut Rng, n: usize, broken: bool, taus: &[(i64, i64)], offs: &[(i64, i64)], m8: bool) -> Option<Clock> {
    let (tn, td) = taus[rng.pick(taus.len())];
    let tau = Q::r(tn, td);
    let u = Q::i(1).div(&tau.mul(&Q::i(4)));
    let mut res = vec![0i64, 1, 2, 3];
    for i in (1..4).rev() { let j = rng.pick(i + 1); res.swap(i, j); }
    let mut rs: Vec<i64> = res[..n].to_vec();
    if broken { rs[n - 1] = rs[0]; }
    let mut q: Vec<i64> = rs.iter().map(|&x| x + 4 * (rng.pick(3) as i64 - 1)).collect();
    q.sort();
    for i in 1..n { if q[i] == q[i - 1] { return None; } }
    let (on, od) = offs[rng.pick(offs.len())];
    let off = Q::r(on, od);
    let e: Vec<Q> = q.iter().map(|&x| u.mul(&Q::i(x)).add(&off)).collect();
    let m = if m8 { 8 } else if rng.coin(1, 2) { 4 } else { 8 };
    let w = Q::r(n as i64, m as i64);
    Some(Clock { tau, e, m, w })
}

// |h| = mh / (8 tau), so 2 |h| (k - r) tau is a quarter-turn multiple and the ideal reference is exact
fn pick_h(rng: &mut Rng, tau: &Q, z_only: bool) -> (Q, [Q; 3]) {
    let mh = 1 + rng.pick(3) as i64;
    let hn = Q::i(mh).div(&tau.mul(&Q::i(8)));
    let ax = if z_only { [Q::zero(), Q::zero(), Q::i(if rng.coin(1, 2) { 1 } else { -1 })] } else { axis(rng, true) };
    (hn.clone(), [ax[0].mul(&hn), ax[1].mul(&hn), ax[2].mul(&hn)])
}

fn level_v(rng: &mut Rng, role: Role, ej: &Q, h0: &Q, h: &[Q; 3]) -> Option<[Q; 3]> {
    let a = ej.add(h0).abs();
    let sub_h = |n: [Q; 3]| [n[0].sub(&h[0]), n[1].sub(&h[1]), n[2].sub(&h[2])];
    match role {
        Role::Zero => Some([Q::zero(), Q::zero(), Q::zero()]),
        Role::Swap => Some([h[0].mul(&Q::i(-2)), h[1].mul(&Q::i(-2)), h[2].mul(&Q::i(-2))]),
        Role::Deg => if ej.add(h0).is_zero() { Some([h[0].neg(), h[1].neg(), h[2].neg()]) } else { None },
        Role::Rot => {
            if a.is_zero() { return None; }
            let ax = axis(rng, false);
            Some(sub_h([ax[0].mul(&a), ax[1].mul(&a), ax[2].mul(&a)]))
        }
        Role::Mz => {
            if a.is_zero() { return None; }
            let s = if rng.coin(1, 2) { a.clone() } else { a.neg() };
            Some(sub_h([Q::zero(), Q::zero(), s]))
        }
        Role::Spec => {
            let (dn, dd) = DELTAS[rng.pick(DELTAS.len())];
            let rr = a.add(&Q::r(dn, dd));
            let ax = axis(rng, true);
            Some(sub_h([ax[0].mul(&rr), ax[1].mul(&rr), ax[2].mul(&rr)]))
        }
        Role::Near => {
            if a.is_zero() { return None; }
            let eps = Q::r(1, 1048576);
            let rr = if rng.coin(1, 2) { a.add(&eps) } else { a.sub(&eps) };
            if rr.is_neg() || rr.is_zero() { return None; }
            let s = if rng.coin(1, 2) { rr } else { rr.neg() };
            Some(sub_h([Q::zero(), Q::zero(), s]))
        }
    }
}

fn tokens_ok(c: &Case) -> bool {
    let mut all: Vec<&Q> = c.e.iter().collect();
    all.push(&c.h0); all.push(&c.tau); all.push(&c.w);
    for q in c.h.iter() { all.push(q); }
    for v in &c.v { for q in v.iter() { all.push(q); } }
    for g in c.psi.iter() { all.push(&g.re); all.push(&g.im); }
    all.iter().all(|q| q.in_token_limit())
}

fn uniform(x: &Exact) -> bool { x.p.windows(2).all(|w| w[0] == w[1]) }

struct Built { c: Case, x: Exact, v: Verdict }

fn finish(mut c: Case, control: &str) -> Option<Built> {
    if !tokens_ok(&c) { return None; }
    let x = eval(&c).ok()?;
    let v = judge(&c, &x).ok()?;
    c.control = control.into();
    c.outcome = v.outcome.into();
    c.codes = v.codes.clone();
    Some(Built { c, x, v })
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Class { P1, P1c, P2, P3, P3b, P4, P6, N2, N3, N4a, N4b, N6 }

fn attempt(rng: &mut Rng, cls: Class) -> Option<Built> {
    use Class::*;
    let n = match cls { P1 | N2 => 2 + rng.pick(3), _ => 3 + rng.pick(2) };
    let (taus, offs): (&[(i64, i64)], &[(i64, i64)]) = if cls == P6 { (&TAUS[..2], &OFFS[..2]) } else { (&TAUS, &OFFS) };
    let ck = clock(rng, n, cls == N4b, if cls == P6 { &[(1, 4), (1, 8), (1, 2)] } else { taus }, offs, cls == P4)?;
    let (hn, h) = pick_h(rng, &ck.tau, cls == P6);
    let mut roles = vec![Role::Zero; n];
    let a = rng.pick(n);
    let s: i64 = if rng.coin(1, 2) { 1 } else { -1 };
    let h0i = rng.pick(4); let mut h0 = Q::r(H0S[h0i].0, H0S[h0i].1);
    let others: Vec<usize> = (0..n).filter(|&j| j != a).collect();
    let b = others[rng.pick(others.len())];
    match cls {
        P1 => { h0 = ck.e[a].neg().sub(&hn.mul(&Q::i(s))); }
        P1c => { h0 = ck.e[a].neg().sub(&hn.mul(&Q::i(s)));
                 for &j in &others { roles[j] = if rng.coin(1, 2) { Role::Spec } else { Role::Zero }; } roles[b] = Role::Spec; }
        P2 | N4a | N4b => { h0 = ck.e[a].neg().sub(&hn.mul(&Q::i(s))); roles[b] = Role::Rot; }
        P3 => { for j in 0..n { roles[j] = if rng.coin(3, 4) { Role::Rot } else { Role::Spec }; } }
        P3b => { h0 = ck.e[a].neg(); roles[a] = Role::Deg; roles[b] = Role::Rot;
                 for &j in &others { if j != b { roles[j] = if rng.coin(1, 2) { Role::Spec } else { Role::Zero }; } } }
        P4 => { roles[a] = Role::Rot; for &j in &others { roles[j] = [Role::Rot, Role::Zero, Role::Spec][rng.pick(3)]; } }
        P6 => { h0 = Q::zero(); roles[a] = Role::Near; roles[b] = Role::Rot;
                for &j in &others { if j != b { roles[j] = if rng.coin(1, 2) { Role::Mz } else { Role::Zero }; } } }
        N2 => { for j in 0..n { roles[j] = Role::Spec; } }
        N3 => { h0 = ck.e[a].neg().sub(&hn.mul(&Q::i(s)));
                for &j in &others { roles[j] = [Role::Swap, Role::Zero, Role::Spec][rng.pick(3)]; } roles[b] = Role::Swap; }
        N6 => { roles[b] = Role::Rot; for &j in &others { if j != b { roles[j] = Role::Spec; } } roles[a] = Role::Spec; }
    }
    let mut v = Vec::new();
    for j in 0..n { v.push(level_v(rng, roles[j], &ck.e[j], &h0, &h)?); }
    let psi_i = if cls == P4 { PSI_COMPLEX[rng.pick(PSI_COMPLEX.len())] } else { rng.pick(8) };
    let mut psi = psi_pool(psi_i);
    let r = if cls == P4 { 1 + rng.pick(ck.m - 1) } else { rng.pick(ck.m) };
    let target = match cls { P1 | P1c => "IDEAL", _ => "INTERACTING" };
    let w = if cls == N4a { ck.w.mul(&Q::r(1, 2)) } else { ck.w.clone() };
    if cls == N6 {
        // psi_0 orthogonal to the single kernel vector of level b
        let (nb, r2) = level_norm(&h, &v[b]);
        let rr = r2.sqrt_exact()?;
        let eb = ck.e[b].add(&h0);
        let sb = if eb.add(&rr).is_zero() { 1 } else if eb.sub(&rr).is_zero() { -1 } else { return None };
        let f = eigvec(&nb, sb, &rr);
        psi = [f[1].conj().scale(&Q::i(-1)), f[0].conj()];
    }
    let c = Case { name: String::new(), e: ck.e.clone(), h0, h, v, r, psi, tau: ck.tau.clone(), w, m: ck.m,
        control: String::new(), target: target.into(), outcome: String::new(), codes: vec![] };
    let positive = matches!(cls, P1 | P1c | P2 | P3 | P3b | P4 | P6);
    let bt = finish(c, if positive { "POSITIVE" } else { "NEGATIVE" })?;
    let (x, vd) = (&bt.x, &bt.v);
    let codes: Vec<&str> = vd.codes.iter().map(|s| s.as_str()).collect();
    let ideal_twin_ok = |c: &Case| -> bool {
        let mut ci = c.clone(); ci.target = "IDEAL".into();
        match judge(&ci, x) { Ok(v2) => v2.outcome == "FAIL" && v2.codes == vec!["SCHRODINGER_DEVIATION_EXCEEDED".to_string()]
            && v2.checks[9] == "FAIL" && v2.checks[10] == "PASS", Err(_) => false }
    };
    let ok = match cls {
        P1 => vd.outcome == "PASS" && x.dim >= 2,
        P1c => vd.outcome == "PASS" && x.dim >= 2 && bt.c.v.iter().any(|q| q.iter().any(|t| !t.is_zero())),
        P2 => vd.outcome == "PASS" && x.dim >= 2 && !uniform(x) && ideal_twin_ok(&bt.c),
        P3 => vd.outcome == "PASS" && x.dim >= 3 && !uniform(x) && bt.c.v.iter().all(|q| q.iter().any(|t| !t.is_zero())),
        P3b => vd.outcome == "PASS" && x.dim >= 3 && !uniform(x),
        P4 => vd.outcome == "PASS" && x.dim >= 2 && !uniform(x) && ideal_twin_ok(&bt.c),
        P6 => vd.outcome == "PASS" && x.dim >= 2 && ideal_twin_ok(&bt.c),
        N2 => codes == ["TRIVIAL_PHYSICAL_STATE"] && x.dim == 0,
        N3 => codes == ["CONDITIONAL_UNDEFINED"],
        N4a => codes == ["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"],
        N4b => codes.contains(&"POVM_NORMALIZATION_EXCEEDED") && codes.iter().all(|c| *c == "POVM_NORMALIZATION_EXCEEDED" || *c == "PROBABILITY_SUM_EXCEEDED"),
        N6 => codes == ["TRIVIAL_PHYSICAL_STATE"] && x.dim >= 1 && x.psi_zero,
    };
    if ok { Some(bt) } else { None }
}

fn draw(rng: &mut Rng, cls: Class) -> (Built, u32) {
    for t in 1..=2_000_000u32 { if let Some(b) = attempt(rng, cls) { return (b, t); } }
    panic!("class {:?}: no accepted draw", cls);
}

fn chk_str(v: &Verdict) -> String { v.checks.iter().map(|s| match *s { "PASS" => "P", "FAIL" => "F", "NOT_EVALUATED" => "N", _ => "I" }).collect() }

fn details(name: &str, b: &Built) -> String {
    let x = &b.x;
    let mut s = format!("== {}\nkernel_dim {} psi_zero {} povm_residual_squared {}\n", name, x.dim, x.psi_zero, x.povm_res2.to_s());
    for k in 0..b.c.m {
        let p = if x.p.is_empty() { "undefined".to_string() } else { x.p[k].to_s() };
        let pl = match x.pauli.get(k) { Some(Some(a)) => format!("{} {} {}", a[0].to_s(), a[1].to_s(), a[2].to_s()), _ => "undefined".into() };
        let id = &x.ideal[k];
        s.push_str(&format!("k {} p {} plusXYZ {} idealXYZ {} {} {}\n", k, p, pl, id[0].to_s(), id[1].to_s(), id[2].to_s()));
    }
    s
}

fn replace_line(text: &str, from_prefix: &str, to: &str) -> String {
    let mut done = false;
    let out: Vec<String> = text.split_terminator('\n').map(|l| {
        if !done && l.starts_with(from_prefix) { done = true; to.to_string() } else { l.to_string() }
    }).collect();
    assert!(done, "line not found: {}", from_prefix);
    let mut s = out.join("\n"); s.push('\n'); s
}
fn remove_line(text: &str, prefix: &str) -> String {
    let lines: Vec<&str> = text.split_terminator('\n').collect();
    let i = lines.iter().position(|l| l.starts_with(prefix)).expect("line");
    let mut out: Vec<&str> = lines.clone(); out.remove(i);
    let mut s = out.join("\n"); s.push('\n'); s
}

fn generate(seed: u64, dir: &str) {
    let mut rng = Rng(seed);
    std::fs::create_dir_all(dir).unwrap();
    let mut expected = String::from("# file\tclass\tkind\texpected_outcome\texpected_codes\tchecks_1_to_12(P=PASS,F=FAIL,N=NOT_EVALUATED)\tcase_id\tacceptance_id\tattempts\n");
    let mut det = String::new();
    let mut files: Vec<(String, String)> = Vec::new();
    let mut built: Vec<(Class, Built)> = Vec::new();
    use Class::*;
    let order = [P1, P1c, P2, P3, P3b, P4, P6, N2, N3, N4a, N4b, N6];
    let mut attempts = Vec::new();
    for &cls in &order { let (b, t) = draw(&mut rng, cls); attempts.push(t); built.push((cls, b)); }
    let tag = |cls: Class| -> &'static str { match cls { P1 => "p1-zero-coupling-ideal", P1c => "p1c-spectator-ideal", P2 => "p2-one-rotated-level",
        P3 => "p3-every-level-coupled", P3b => "p3b-degenerate-level", P4 => "p4-complex-psi-ref-nonzero", P6 => "p6-near-miss-level",
        N2 => "n2-trivial-kernel", N3 => "n3-zero-label", N4a => "n4a-wrong-weight", N4b => "n4b-broken-clock", N6 => "n6-zero-projection" } };
    let mut idx = 0;
    let push_result = |name: String, cls: &str, c: &Case, x: &Exact, v: &Verdict, att: u32, files: &mut Vec<(String, String)>, expected: &mut String, det: &mut String| {
        let mut c = c.clone(); c.name = name.clone();
        let text = render(&c);
        let back = parse_case(&text).expect("reparse");
        let x2 = eval(&back).expect("re-eval");
        let v2 = judge(&back, &x2).expect("re-judge");
        assert!(v2.outcome == v.outcome && v2.codes == v.codes && chk_str(&v2) == chk_str(v));
        assert!(back.outcome == v.outcome && back.codes == v.codes);
        let lines: Vec<String> = text.split_terminator('\n').map(|s| s.to_string()).collect();
        let cid = lines.iter().find(|l| l.starts_with("case_id ")).unwrap()[8..].to_string();
        let aid = lines.iter().find(|l| l.starts_with("acceptance_id ")).unwrap()[14..].to_string();
        let fname = format!("{}.case", name);
        expected.push_str(&format!("{}\t{}\tRESULT\t{}\t{}\t{}\t{}\t{}\t{}\n", fname, cls, v.outcome,
            if v.codes.is_empty() { "none".to_string() } else { v.codes.join(",") }, chk_str(v), cid, aid, att));
        det.push_str(&details(&fname, &Built { c: c.clone(), x: Exact { dim: x.dim, psi_zero: x.psi_zero, p: x.p.clone(), pauli: x.pauli.clone(), ideal: x.ideal.clone(), povm_res2: x.povm_res2.clone() }, v: Verdict { checks: v.checks.clone(), outcome: v.outcome, codes: v.codes.clone() } }));
        files.push((fname, text));
    };
    for (i, (cls, b)) in built.iter().enumerate() {
        idx += 1;
        let name = format!("a7h-{:02}-{}", idx, tag(*cls));
        push_result(name, &format!("{:?}", cls), &b.c, &b.x, &b.v, attempts[i], &mut files, &mut expected, &mut det);
    }
    // N1 family: the same semantic block as P2, P4, P6 with prediction_target IDEAL
    for (src, label) in [(P2, "n1a-p2-question-ideal-target"), (P4, "n1b-p4-question-ideal-target"), (P6, "n1c-p6-question-ideal-target")] {
        let b = &built.iter().find(|(c, _)| *c == src).unwrap().1;
        let mut c = b.c.clone();
        c.target = "IDEAL".into(); c.control = "NEGATIVE".into();
        let v = judge(&c, &b.x).unwrap();
        assert!(v.outcome == "FAIL" && v.codes == vec!["SCHRODINGER_DEVIATION_EXCEEDED".to_string()]);
        c.outcome = v.outcome.into(); c.codes = v.codes.clone();
        idx += 1;
        let name = format!("a7h-{:02}-{}", idx, label);
        push_result(name, "N1", &c, &b.x, &v, 0, &mut files, &mut expected, &mut det);
    }
    // refusal files: one defect each on an accepted case (first failure in AT1_CASE_V1 section 4 order)
    let base = |cls: Class| -> Case { built.iter().find(|(c, _)| *c == cls).unwrap().1.c.clone() };
    let mut refusals: Vec<(String, String, &str)> = Vec::new();
    {
        // R-a: per-level irrational spectrum on one level while |h| stays rational
        let mut c = base(P4);
        let mut found = false;
        'outer: for j in 0..c.v.len() {
            for (dn, dd) in [(1i64, 5i64), (1, 3), (1, 7), (2, 5), (3, 5)] {
                let mut c2 = c.clone();
                c2.v[j][0] = c2.v[j][0].add(&Q::r(dn, dd));
                let (_, r2) = level_norm(&c2.h, &c2.v[j]);
                if r2.sqrt_exact().is_none() && tokens_ok(&c2) { c = c2; found = true; break 'outer; }
            }
        }
        assert!(found);
        c.name = "a7h-R-irrational-level".into();
        refusals.push(("r1-per-level-irrational".into(), render(&c), "CASE_IRRATIONAL_SPECTRUM"));
        // R-b: non-canonical coupling index token 01 at position 1 (charter reading e)
        let mut c = base(P3); c.name = "a7h-R-index-01".into();
        let t = fix_ids(&replace_line(&render(&c), "interaction_pauli 1 ", &render(&c).lines().find(|l| l.starts_with("interaction_pauli 1 ")).unwrap().replacen("interaction_pauli 1 ", "interaction_pauli 01 ", 1)));
        refusals.push(("r2-coupling-index-01".into(), t, "CASE_NONCANONICAL"));
        // R-c: AT0 header on line 1 (charter reading d)
        let mut c = base(P1); c.name = "a7h-R-at0-header".into();
        refusals.push(("r3-at0-header".into(), replace_line(&render(&c), "OMEGA-AT1-CASE v1", "OMEGA-AT0-CASE v1"), "CASE_PARSE_ERROR"));
        // R-d: foreign domain value (charter reading d)
        let mut c = base(P3b); c.name = "a7h-R-domain-at0".into();
        refusals.push(("r4-domain-at0".into(), replace_line(&render(&c), "domain omega.at1.case.v1", "domain omega.at0.case.v1"), "CASE_UNSUPPORTED_VERSION"));
        // R-e: coupling line count not N (last interaction_pauli line removed)
        let mut c = base(P2); c.name = "a7h-R-coupling-count".into();
        let last = format!("interaction_pauli {} ", c.v.len() - 1);
        refusals.push(("r5-coupling-count".into(), fix_ids(&remove_line(&render(&c), &last)), "CASE_PARSE_ERROR"));
        // R-f: canonical clock_label index with the wrong value (2 at position 1)
        let mut c = base(P6); c.name = "a7h-R-label-index-value".into();
        refusals.push(("r6-label-index-value".into(), fix_ids(&replace_line(&render(&c), "clock_label 1 t1", "clock_label 2 t1")), "CASE_PARSE_ERROR"));
    }
    for (tag, text, code) in refusals {
        idx += 1;
        let fname = format!("a7h-{:02}-{}.case", idx, tag);
        let lines: Vec<String> = text.split_terminator('\n').map(|s| s.to_string()).collect();
        let cid = lines.iter().find(|l| l.starts_with("case_id ")).map(|l| l[8..].to_string()).unwrap_or("-".into());
        let aid = lines.iter().find(|l| l.starts_with("acceptance_id ")).map(|l| l[14..].to_string()).unwrap_or("-".into());
        expected.push_str(&format!("{}\tR\tREFUSED\t{}\t-\t-\t{}\t{}\t0\n", fname, code, cid, aid));
        files.push((fname, text));
    }
    for (f, t) in &files { std::fs::write(format!("{}/{}", dir, f), t).unwrap(); }
    std::fs::write(format!("{}/EXPECTED.tsv", dir), &expected).unwrap();
    std::fs::write(format!("{}/DETAILS.txt", dir), &det).unwrap();
    println!("wrote {} case files", files.len());
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    match a.get(1).map(|s| s.as_str()) {
        Some("selftest") => { let bad = selftest(&a[2]); std::process::exit(if bad == 0 { 0 } else { 1 }); }
        Some("gen") => {
            let seed = u64::from_str_radix(a[2].trim(), 16).expect("seed hex");
            generate(seed, &a[3]);
        }
        _ => { eprintln!("usage: at1h selftest <worked> | gen <seed_hex> <dir>"); std::process::exit(2); }
    }
}
