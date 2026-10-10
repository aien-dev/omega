//! Hidden-set generator (documented, deterministic, seeded) and the commitment
//! procedure of AT1_SPEC 12.3. The procedure is public; the 256-bit seed is
//! not, so the cases cannot be reproduced before the reveal.
//!
//! Stream: block i = SHA-256("omega.at1.agent4.hidden.v1" || 0x00 || seed || be64(i)).
//! 27 cases, family = index mod 9, from the AT1_SPEC 13.1 item 7 families:
//!   F0 rotated level (INTERACTING, PASS)      F1 rotated level, target IDEAL (N1)
//!   F2 every level coupled (INTERACTING)      F3 degenerate level + rotated level
//!   F4 spectator couplings, target IDEAL      F5 every level out (N2)
//!   F6 swapped level v_j = -2h (N3)           F7 psi_0 orthogonal to the kernel (N6)
//!   F8 rotated level with half weight (N4a)
//! Axes come from Pythagorean quadruples, signs and component order drawn from
//! the stream; |h| = 1/2 always. Expected outcome and codes are the exact
//! verdict of a correct implementation (honest synthetic run, re-derived
//! exactly); a draw is rejected and redrawn unless the family's intended code
//! set results and the AT1_SPEC 8.3 margin rule holds.

use crate::case::{validate_bytes, Case, Spec};
use crate::model::{shadow, Mods};
use crate::rat::{Cq, Q};
use crate::sha256::{sha256, sha256_hex};
use crate::synth::{engine_result, oracle_record, splice, EngineMut, OracleMut};

const DOMAIN: &[u8] = b"omega.at1.agent4.hidden.v1";
pub const COUNT: usize = 27;

struct Rng {
    seed: Vec<u8>,
    block: u64,
    buf: Vec<u8>,
}
impl Rng {
    fn u64(&mut self) -> u64 {
        if self.buf.len() < 8 {
            let mut m = DOMAIN.to_vec();
            m.push(0);
            m.extend_from_slice(&self.seed);
            m.extend_from_slice(&self.block.to_be_bytes());
            self.block += 1;
            self.buf.extend_from_slice(&sha256(&m));
        }
        let b: Vec<u8> = self.buf.drain(..8).collect();
        u64::from_be_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]])
    }
    /// Uniform in [0, n) by rejection.
    fn below(&mut self, n: u64) -> u64 {
        let lim = u64::MAX - u64::MAX % n;
        loop {
            let x = self.u64();
            if x < lim {
                return x % n;
            }
        }
    }
    fn pick<T: Clone>(&mut self, v: &[T]) -> T {
        v[self.below(v.len() as u64) as usize].clone()
    }
}

const QUADS: [[i64; 4]; 11] = [[1, 2, 2, 3], [2, 3, 6, 7], [1, 4, 8, 9], [4, 4, 7, 9], [2, 6, 9, 11], [6, 6, 7, 11], [3, 4, 12, 13], [2, 5, 14, 15], [2, 10, 11, 15], [1, 12, 12, 17], [8, 9, 12, 17]];

/// A random rational unit axis (a, b, c)/d.
fn axis(g: &mut Rng) -> [Q; 3] {
    let q = g.pick(&QUADS);
    let perm = g.pick(&[[0, 1, 2], [0, 2, 1], [1, 0, 2], [1, 2, 0], [2, 0, 1], [2, 1, 0]]);
    let mut out = [Q::zero(), Q::zero(), Q::zero()];
    for i in 0..3 {
        let s = if g.below(2) == 0 { 1 } else { -1 };
        out[i] = Q::frac(s * q[perm[i]], q[3]);
    }
    out
}
fn scale(a: &[Q; 3], r: &Q) -> [Q; 3] {
    [a[0].mul(r), a[1].mul(r), a[2].mul(r)]
}
fn sub3(a: &[Q; 3], b: &[Q; 3]) -> [Q; 3] {
    [a[0].sub(&b[0]), a[1].sub(&b[1]), a[2].sub(&b[2])]
}
fn small_c(g: &mut Rng) -> Cq {
    let d = g.pick(&[1i64, 1, 2, 3, 5]);
    Cq::new(Q::frac(g.below(7) as i64 - 3, d), Q::frac(g.below(7) as i64 - 3, d))
}

/// The margin rule of AT1_SPEC 8.3 for a correct implementation of this case.
pub fn margin_ok(c: &Case) -> Result<(), String> {
    let sh = shadow(c, &Mods::default(), false);
    let tiny = Q::frac(1, 100_000_000_000_000_000);
    let near0 = |x: &Q, exact: bool| if exact { x.is_zero() } else { x.abs().cmp(&tiny) != std::cmp::Ordering::Greater };
    let big = |x: &Q, tol: &Q| x.abs().cmp(&tol.mul(&Q::int(1000))) != std::cmp::Ordering::Less;
    let tol_prob = Q::from_scaled(&c.tol_prob);
    let tol_schro = Q::from_scaled(&c.tol_schro);
    let tol_povm = Q::from_scaled(&c.tol_povm);
    let pv = &sh.povm_residual;
    if !(near0(&pv.q, pv.exact) || big(&pv.q, &tol_povm)) {
        return Err(format!("povm_residual {} inside the margin", pv.q.to_f64()));
    }
    if sh.trivial {
        return Ok(());
    }
    let milli = Q::frac(1, 1000);
    let mut sum = Q::zero();
    let mut sum_exact = true;
    let wn = c.w.div(&Q::int(c.n as i64));
    for k in 0..c.m {
        let p = &sh.p[k];
        sum = sum.add(&p.q);
        sum_exact &= p.exact;
        if !(near0(&p.q, p.exact) || p.q.cmp(&milli) != std::cmp::Ordering::Less) {
            return Err(format!("p({}) = {} is neither 0 nor >= 1e-3", k, p.q.to_f64()));
        }
        let over = p.q.sub(&Q::one());
        if over.signum() > 0 && !big(&over, &tol_prob) {
            return Err(format!("p({}) - 1 inside the margin", k));
        }
        if c.target_ideal {
            if let Some(pa) = &sh.pauli[k] {
                for a in 0..3 {
                    for s in 0..2 {
                        let d = pa[a][s].q.sub(&sh.ideal[k][a][s].q);
                        if !(near0(&d, pa[a][s].exact && sh.ideal[k][a][s].exact) || big(&d, &tol_schro)) {
                            return Err(format!("ideal deviation at k={} inside the margin", k));
                        }
                    }
                }
                let d = p.q.sub(&wn);
                if !(near0(&d, p.exact) || big(&d, &tol_prob)) {
                    return Err(format!("marginal deviation from w/N at k={} inside the margin", k));
                }
            }
        }
    }
    let d = sum.sub(&Q::one());
    if !(near0(&d, sum_exact) || big(&d, &tol_prob)) {
        return Err("sum of p(k) - 1 inside the margin".into());
    }
    Ok(())
}

/// One draw of family f; None when the draw is not usable.
fn draw(g: &mut Rng, f: usize, name: &str) -> Option<Spec> {
    let mut s = Spec::base(name);
    let half = Q::frac(1, 2);
    let h = scale(&axis(g), &half);
    let degen = f == 3;
    // clock grid: gap 1 with tau 1/4, or gap 1/2 with tau 1/2 (degenerate family); M = 4 complete
    let n = if degen { g.pick(&[3usize, 5]) } else { g.pick(&[2usize, 3, 4, 5, 6]) };
    let gap = if degen { Q::frac(1, 2) } else { Q::one() };
    let mid = Q::frac(n as i64 - 1, 2);
    let mut e: Vec<Q> = (0..n).map(|j| Q::int(j as i64).sub(&mid).mul(&gap)).collect();
    // h0 puts two adjacent levels at -1/2 and +1/2 (odd N, gap 1: h0 = 1/2); an optional common shift delta keeps that
    let mut h0 = if !degen && n % 2 == 1 { half.clone() } else { Q::zero() };
    if g.below(3) == 0 {
        let delta = Q::frac(1 + g.below(97) as i64, 1009);
        for x in e.iter_mut() {
            *x = x.add(&delta);
        }
        h0 = h0.sub(&delta);
    }
    s.e = e.clone();
    s.h = [h0.clone(), h[0].clone(), h[1].clone(), h[2].clone()];
    s.v = vec![[Q::zero(), Q::zero(), Q::zero()]; n];
    s.tau = if degen { Q::frac(1, 2) } else { Q::frac(1, 4) };
    s.w = Q::frac(n as i64, 4);
    s.psi = [small_c(g), small_c(g)];
    if s.psi[0].is_zero() && s.psi[1].is_zero() {
        return None;
    }
    let r = g.below(4) as usize;
    s.ref_label = format!("t{}", r);
    let off: Vec<Q> = e.iter().map(|x| x.add(&h0)).collect();
    let matched: Vec<usize> = (0..n).filter(|&j| off[j].abs() == half).collect();
    let others: Vec<usize> = (0..n).filter(|&j| off[j].abs() != half).collect();
    if matched.is_empty() {
        return None;
    }
    let rotate = |g: &mut Rng, j: usize| -> [Q; 3] { sub3(&scale(&axis(g), &off[j].abs()), &h) };
    match f {
        0 | 1 | 8 => {
            let j = g.pick(&matched);
            s.v[j] = rotate(g, j);
            if f == 1 {
                s.target_ideal = true;
            }
            if f == 8 {
                s.w = Q::frac(n as i64, 8);
            }
        }
        2 => {
            for j in 0..n {
                if off[j].is_zero() {
                    return None;
                }
                s.v[j] = rotate(g, j);
            }
        }
        3 => {
            let z = (0..n).find(|&j| off[j].is_zero())?;
            s.v[z] = [h[0].neg(), h[1].neg(), h[2].neg()];
            let j = g.pick(&matched);
            s.v[j] = rotate(g, j);
        }
        4 => {
            if others.is_empty() {
                return None;
            }
            for &j in &others {
                // norm |E_j + h0| + 1/2 never matches the level
                s.v[j] = sub3(&scale(&axis(g), &off[j].abs().add(&half)), &h);
            }
            s.target_ideal = true;
        }
        5 => {
            for &j in &matched {
                s.v[j] = sub3(&scale(&axis(g), &Q::one()), &h);
            }
        }
        6 => {
            let j = *matched.iter().find(|&&j| off[j].signum() > 0)?;
            s.v[j] = [h[0].mul(&Q::int(-2)), h[1].mul(&Q::int(-2)), h[2].mul(&Q::int(-2))];
        }
        7 => {
            if matched.len() < 2 {
                return None;
            }
            let keep = g.pick(&matched);
            for &j in &matched {
                if j != keep {
                    s.v[j] = sub3(&scale(&axis(g), &Q::one()), &h);
                }
            }
            let nn = scale(&axis(g), &off[keep].abs());
            s.v[keep] = sub3(&nn, &h);
            // kernel vector of the kept level and a psi_0 orthogonal to it
            let sgn = if off[keep].signum() < 0 { 1 } else { -1 };
            let rr = off[keep].abs();
            let sr = if sgn > 0 { rr.clone() } else { rr.neg() };
            let (f0, f1) = if sr.add(&nn[2]).is_zero() {
                (Cq::new(nn[0].clone(), nn[1].neg()), Cq::new(sr.sub(&nn[2]), Q::zero()))
            } else {
                (Cq::new(sr.add(&nn[2]), Q::zero()), Cq::new(nn[0].clone(), nn[1].clone()))
            };
            s.psi = [f1.conj().scale(&Q::int(-1)), f0.conj()];
        }
        _ => return None,
    }
    Some(s)
}

fn intended(f: usize) -> Vec<&'static str> {
    match f {
        1 => vec!["SCHRODINGER_DEVIATION_EXCEEDED"],
        5 | 7 => vec!["TRIVIAL_PHYSICAL_STATE"],
        6 => vec!["CONDITIONAL_UNDEFINED"],
        8 => vec!["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"],
        _ => vec![],
    }
}

/// Builds case number i (deterministic in the seed).
fn build(g: &mut Rng, i: usize) -> (String, Vec<u8>) {
    let f = i % 9;
    let name = format!("at1-agent4-hidden-{:02}", i + 1);
    for _attempt in 0..10_000 {
        let s = match draw(g, f, &name) {
            Some(s) => s,
            None => continue,
        };
        let c = match validate_bytes(&s.bytes()) {
            Ok(c) => c,
            Err(_) => continue,
        };
        let r = splice(&engine_result(&c, &EngineMut::default(), "h"), &oracle_record(&c, &OracleMut::default(), "o")).unwrap();
        let codes: Vec<&str> = r.verdict.failure_codes.iter().map(|x| x.as_str()).collect();
        if codes != intended(f) {
            continue;
        }
        let s2 = if codes.is_empty() { s } else { s.negative(&codes) };
        let c2 = match validate_bytes(&s2.bytes()) {
            Ok(c) => c,
            Err(_) => continue,
        };
        if margin_ok(&c2).is_err() {
            continue;
        }
        return (format!("H{:02}.case", i + 1), s2.bytes());
    }
    panic!("hidden-gen: family {} produced no usable draw", f);
}

fn parse_seed(hex: &str) -> Option<Vec<u8>> {
    if hex.len() != 64 || !hex.bytes().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    (0..32).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).ok()).collect()
}

pub fn generate(seed_hex: &str, outdir: &str) -> i32 {
    let seed = match parse_seed(seed_hex.trim()) {
        Some(s) => s,
        None => {
            eprintln!("hidden-gen: the seed must be 64 hex digits (256 bits)");
            return 64;
        }
    };
    let mut g = Rng { seed, block: 0, buf: Vec::new() };
    let _ = std::fs::create_dir_all(outdir);
    for i in 0..COUNT {
        let (name, bytes) = build(&mut g, i);
        if let Err(e) = std::fs::write(format!("{}/{}", outdir, name), &bytes) {
            eprintln!("hidden-gen: {}", e);
            return 73;
        }
    }
    commit(outdir)
}

/// Manifest `<sha256><two spaces><name>` per .case file, sorted bytewise by name,
/// LF-terminated; the commitment is SHA-256 of the manifest bytes (AT1_SPEC 12.3).
pub fn manifest(dir: &str) -> Result<String, String> {
    let mut names: Vec<String> = std::fs::read_dir(dir)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok().map(|e| e.file_name().to_string_lossy().into_owned()))
        .filter(|n| n.ends_with(".case"))
        .collect();
    names.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    let mut m = String::new();
    for n in names {
        let b = std::fs::read(format!("{}/{}", dir, n)).map_err(|e| e.to_string())?;
        m.push_str(&format!("{}  {}\n", sha256_hex(&b), n));
    }
    Ok(m)
}

pub fn commit(dir: &str) -> i32 {
    match manifest(dir) {
        Ok(m) => {
            print!("{}", m);
            println!("commitment {}", sha256_hex(m.as_bytes()));
            0
        }
        Err(e) => {
            eprintln!("hidden-commit: {}", e);
            66
        }
    }
}
