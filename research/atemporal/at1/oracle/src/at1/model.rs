//! Exact level structure of the clock-diagonal model (AT1_SPEC.md sections 1 to 4).
//!
//! Everything here is exact Gaussian-rational arithmetic: level norms, kernel pairs,
//! degenerate-matched levels, the unnormalized level eigenvectors of AT1_CASE_V1 section 3,
//! the level vectors u_j = Pi_j psi_0, S = sum ||u_j||^2 and the exact zero test of Psi.
//! No floating-point value enters any decision made here (AT1_SPEC Remark 1.4).
//! Also: exact reduction of phase turn counts modulo one (AT1_SPEC section 8.4 item 1).

use super::big::{Q, GQ};
use super::case::Case;
use super::complex::C;

#[derive(Clone, Debug, PartialEq)]
pub enum LevelKind {
    /// R_j > 0 and E_j + h0 + s R_j = 0; `f` the unnormalized eigenvector for h0 + s R_j.
    Pair { s: i32, f: [GQ; 2] },
    /// R_j = 0 and E_j + h0 = 0: the whole qubit is in the kernel at this level.
    Degenerate,
    /// No kernel vector at this level.
    Out,
}

#[derive(Clone, Debug)]
pub struct Levels {
    pub kinds: Vec<LevelKind>,
    /// u_j = Pi_j psi_0 (exact), zero for levels outside the kernel.
    pub u: Vec<[GQ; 2]>,
    /// S = sum_j ||u_j||^2 (exact). ||Psi||^2 = S / N.
    pub s: Q,
    pub kernel_dim: usize,
    /// Psi exactly zero (AT1_CASE_V1 section 3 zero test), or kernel dimension 0.
    pub trivial: bool,
}

/// Unnormalized eigenvector of n.sigma for eigenvalue s|n| (AT1_CASE_V1 section 3), |n| > 0.
pub fn level_eigenvector(n: &[Q; 3], r: &Q, s: i32) -> [GQ; 2] {
    let sr = if s > 0 { r.clone() } else { r.neg() };
    let first = [GQ::new(sr.add(&n[2]), Q::zero()), GQ::new(n[0].clone(), n[1].clone())];
    if !(first[0].is_zero() && first[1].is_zero()) {
        return first;
    }
    [GQ::new(n[0].clone(), n[1].neg()), GQ::new(sr.sub(&n[2]), Q::zero())]
}

/// <f|psi> with the bra antilinear.
pub fn inner(f: &[GQ; 2], psi: &[GQ; 2]) -> GQ {
    f[0].conj().mul(&psi[0]).add(&f[1].conj().mul(&psi[1]))
}

pub fn levels(c: &Case) -> Levels {
    let mut kinds = Vec::with_capacity(c.clock_dim);
    let mut u = Vec::with_capacity(c.clock_dim);
    let mut kernel_dim = 0;
    for j in 0..c.clock_dim {
        let n = c.level_vector(j);
        let r = &c.level_norm[j];
        let shift = c.energies[j].add(&c.h0); // E_j + h0
        let kind = if r.is_zero() {
            if shift.is_zero() {
                LevelKind::Degenerate
            } else {
                LevelKind::Out
            }
        } else if shift.add(r).is_zero() {
            LevelKind::Pair { s: 1, f: level_eigenvector(&n, r, 1) }
        } else if shift.sub(r).is_zero() {
            LevelKind::Pair { s: -1, f: level_eigenvector(&n, r, -1) }
        } else {
            LevelKind::Out
        };
        let uj = match &kind {
            LevelKind::Pair { f, .. } => {
                kernel_dim += 1;
                let coef = inner(f, &c.psi);
                let f2 = f[0].norm2().add(&f[1].norm2());
                let scale = |x: &GQ| x.mul(&coef).scale(&Q::one().div(&f2));
                [scale(&f[0]), scale(&f[1])]
            }
            LevelKind::Degenerate => {
                kernel_dim += 2;
                c.psi.clone()
            }
            LevelKind::Out => [GQ::zero(), GQ::zero()],
        };
        kinds.push(kind);
        u.push(uj);
    }
    let s = u.iter().fold(Q::zero(), |acc, uj| acc.add(&uj[0].norm2()).add(&uj[1].norm2()));
    let trivial = kernel_dim == 0 || s.is_zero();
    Levels { kinds, u, s, kernel_dim, trivial }
}

/// exp(2 pi i x) for an exact rational turn count x, with the turn count reduced modulo one
/// exactly (AT1_SPEC 8.4 item 1). The reduced turn is split into the nearest multiple of
/// 1/8 (applied exactly for quarter turns, with the binary64 sqrt(1/2) for odd eighths)
/// and an exact remainder in [-1/16, 1/16] turned into binary64 once. Multiples of 1/4
/// come out exact.
pub fn cis_turns(x: &Q) -> C {
    let f = x.frac_part(); // [0, 1)
    let eighth = f.mul(&Q::int(8)).round_half_up_i64(); // 0..=8
    let rest = f.sub(&Q::frac(eighth, 8)); // [-1/16, 1/16)
    let base = match eighth.rem_euclid(8) {
        0 => C::new(1.0, 0.0),
        2 => C::new(0.0, 1.0),
        4 => C::new(-1.0, 0.0),
        6 => C::new(0.0, -1.0),
        e => {
            let h = std::f64::consts::FRAC_1_SQRT_2;
            match e {
                1 => C::new(h, h),
                3 => C::new(-h, h),
                5 => C::new(-h, -h),
                _ => C::new(h, -h),
            }
        }
    };
    if rest.is_zero() {
        return base;
    }
    let theta = 2.0 * std::f64::consts::PI * rest.to_f64();
    base * C::new(theta.cos(), theta.sin())
}

/// Convert an exact Gaussian rational to binary64 components (each correctly rounded).
pub fn gq_f64(z: &GQ) -> C {
    C::new(z.re.to_f64(), z.im.to_f64())
}
