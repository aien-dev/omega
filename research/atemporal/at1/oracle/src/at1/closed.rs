//! Oracle reference quantities (AT1_RESULT_V1 section 3), each by its own code path:
//!
//! - `interacting`: the closed form of AT1_SPEC Theorem 3 from the exact level vectors,
//!   chi_k = sum_j exp(+2 pi i E_j (k - r) tau) u_j (sign frozen in AT1_CASE_V1 section 3),
//!   p(k) = w ||chi_k||^2 / (N S), Pauli probabilities from the pure state of chi_k.
//!   This gives `reference_clock_probability` and `reference_interacting`.
//! - `ideal`: Schrodinger evolution under H_S alone, as a right-handed Bloch rotation of
//!   psi_0 about h/|h| by 2|h|(t_k - t_r) (AT1_SPEC Definition 5.2). No V, no kernel.
//!   This gives `reference_ideal`.
//!
//! Bounds are ESTIMATED (stated estimates, not enclosures); see NUMERICAL_ERROR.md.

use super::big::{Q, GQ};
use super::case::Case;
use super::complex::C;
use super::model::{cis_turns, gq_f64, CSum, Levels};

/// Unit roundoff of binary64.
pub const U: f64 = 1.0 / 9007199254740992.0; // 2^-53

/// Error of one `cis_turns` value (model.rs), in units of U, absolute (the value has modulus
/// 1): remainder conversion and the product 2 pi r (3 U relative on an angle of at most
/// pi/8), `cos` and `sin` at most one ulp each, the binary64 sqrt(1/2) of odd eighths (U)
/// and the final complex product (sqrt(5) U); about 7 U, taken as 8.
pub const CIS_ERR: f64 = 8.0;

/// Factor applied to every first-order estimate below. The operation counts are worst-case
/// sums of absolute values in the standard rounding model, so the factor only has to cover
/// second-order terms and a libm that misses its ulp; see NUMERICAL_ERROR.md.
pub const SAFETY: f64 = 2.0;

/// Per-value Pauli bounds from a relative state error `rel = ||d phi|| / ||phi||`.
/// The ray of `phi + d phi` is within the angle `a = asin(rel)` of the ray of `phi`, and
/// `P = cos^2(beta)` has `|dP/dbeta| = 2 sqrt(P (1 - P))` and `|d2P/dbeta2| <= 2`, so
/// `|dP| <= 2 sqrt(P (1 - P)) a + a^2`. `sqrt(P (1 - P))` is evaluated at the computed `P`,
/// which moves it by at most `a` (it is `|sin 2 beta| / 2`), giving `2 s a + 3 a^2`; `16 U`
/// covers the final quadratic form and division. Exact zeros and ones (a state orthogonal
/// to an eigenvector) thereby get a second-order bound, which is what they have.
pub fn pauli_bounds(p: &[[f64; 2]; 3], rel: f64) -> [[f64; 2]; 3] {
    let a = if rel < 1.0 { rel.asin() } else { std::f64::consts::FRAC_PI_2 };
    let mut out = [[0.0; 2]; 3];
    for i in 0..3 {
        for s in 0..2 {
            let pv = p[i][s].clamp(0.0, 1.0);
            let sq = (pv * (1.0 - pv)).sqrt();
            out[i][s] = SAFETY * (2.0 * sq * a + 3.0 * a * a + 16.0 * U);
        }
    }
    out
}

#[derive(Clone, Debug)]
pub struct LabelRef {
    pub p: f64,
    pub p_bound: f64,
    /// [axis][PLUS, MINUS]; None when ||chi_k|| is exactly zero (no state to condition on).
    pub pauli: Option<[[f64; 2]; 3]>,
    /// Per value, [axis][PLUS, MINUS]; zeros when `pauli` is None.
    pub pauli_bound: [[f64; 2]; 3],
}

/// Six Pauli outcome probabilities of the pure state of a 2-vector, each computed directly
/// as |<e|phi>|^2 / ||phi||^2 (never as 1 - other).
pub fn pauli_of(c0: C, c1: C) -> Option<[[f64; 2]; 3]> {
    let nn = c0.norm2() + c1.norm2();
    if nn == 0.0 {
        return None;
    }
    let i = C::I;
    let x_p = (c0 + c1).norm2() / (2.0 * nn);
    let x_m = (c0 - c1).norm2() / (2.0 * nn);
    let y_p = (c0 - i * c1).norm2() / (2.0 * nn);
    let y_m = (c0 + i * c1).norm2() / (2.0 * nn);
    let z_p = c0.norm2() / nn;
    let z_m = c1.norm2() / nn;
    Some([[x_p, x_m], [y_p, y_m], [z_p, z_m]])
}

/// Interacting reference for every label (only meaningful when the kernel is nontrivial).
pub fn interacting(c: &Case, lv: &Levels) -> Vec<LabelRef> {
    let n = c.clock_dim as f64;
    let w = c.weight.to_f64();
    let s = lv.s.to_f64();
    let uf: Vec<[C; 2]> = lv.u.iter().map(|uj| [gq_f64(&uj[0]), gq_f64(&uj[1])]).collect();
    let live: Vec<usize> = (0..c.clock_dim).filter(|&j| !(lv.u[j][0].is_zero() && lv.u[j][1].is_zero())).collect();
    let mag: f64 = live.iter().map(|&j| (uf[j][0].norm2() + uf[j][1].norm2()).sqrt()).sum();
    let r = c.ref_index as i64;
    (0..c.labels.len())
        .map(|k| {
            let (mut c0, mut c1) = (CSum::new(), CSum::new());
            for &j in &live {
                let turns = c.energies[j].mul(&Q::int(k as i64 - r)).mul(&c.tau);
                let ph = cis_turns(&turns);
                c0.add(ph * uf[j][0]);
                c1.add(ph * uf[j][1]);
            }
            let chi = [c0.value(), c1.value()];
            let nn = chi[0].norm2() + chi[1].norm2();
            let p = w * nn / (n * s);
            // first-order, AT1_SPEC 8.4 item 4: per component |d chi_a| <= sum_j |u_ja| times
            // (U conversion of the exact u_j + CIS_ERR phase + sqrt(5) U product + 2 U
            // for the compensated sum), and by the triangle inequality on (|u_j0|, |u_j1|)
            // ||d chi|| <= (CIS_ERR + 5.3) U sum_j ||u_j||; written as (CIS_ERR + 6).
            let dchi = (CIS_ERR + 6.0) * U * mag;
            let norm = nn.sqrt();
            let p_bound = SAFETY * (w / (n * s) * (2.0 * norm * dchi + dchi * dchi) + 16.0 * U * p);
            let pauli = pauli_of(chi[0], chi[1]);
            let pauli_bound = match &pauli {
                Some(pv) => pauli_bounds(pv, dchi / norm),
                None => [[0.0; 2]; 3],
            };
            LabelRef { p, p_bound, pauli, pauli_bound }
        })
        .collect()
}

#[derive(Clone, Debug)]
pub struct IdealRef {
    pub pauli: [[f64; 2]; 3],
    pub bound: f64,
}

/// Ideal reference: Bloch vector of psi_0 rotated about h/|h| by the angle 2|h|(t_k - t_r).
pub fn ideal(c: &Case) -> Vec<IdealRef> {
    // exact Bloch vector of psi_0: (2 Re(conj a b), 2 Im(conj a b), |a|^2 - |b|^2) / ||psi||^2
    let (a, b) = (&c.psi[0], &c.psi[1]);
    let ab: GQ = a.conj().mul(b);
    let nn = a.norm2().add(&b.norm2());
    let two = Q::int(2);
    let bloch = [ab.re.mul(&two).div(&nn).to_f64(), ab.im.mul(&two).div(&nn).to_f64(), a.norm2().sub(&b.norm2()).div(&nn).to_f64()];
    let hh = c.h[0].mul(&c.h[0]).add(&c.h[1].mul(&c.h[1])).add(&c.h[2].mul(&c.h[2]));
    let h_exact = hh.sqrt_exact();
    let r = c.ref_index as i64;
    (0..c.labels.len())
        .map(|k| {
            let (b2, angle_err) = if hh.is_zero() {
                (bloch, 0.0)
            } else {
                // rotation by theta = 2 pi * T with T = 2 |h| (k - r) tau turns
                let (axis, cs, err) = match &h_exact {
                    Some(hn) => {
                        let t = hn.mul(&two).mul(&Q::int(k as i64 - r)).mul(&c.tau);
                        let z = cis_turns(&t);
                        let axis = [c.h[0].div(hn).to_f64(), c.h[1].div(hn).to_f64(), c.h[2].div(hn).to_f64()];
                        (axis, z, 0.0)
                    }
                    None => {
                        // |h| irrational (every v_j nonzero, AT1_SPEC 8.4 item 2): binary64 turn count
                        let hn = hh.to_f64().sqrt();
                        let t = 2.0 * hn * (k as f64 - r as f64) * c.tau.to_f64();
                        let tr = t - t.round();
                        let th = 2.0 * std::f64::consts::PI * tr;
                        let axis = [c.h[0].to_f64() / hn, c.h[1].to_f64() / hn, c.h[2].to_f64() / hn];
                        (axis, C::new(th.cos(), th.sin()), 2.0 * std::f64::consts::PI * 8.0 * U * t.abs())
                    }
                };
                let (co, si) = (cs.re, cs.im);
                let dot = axis[0] * bloch[0] + axis[1] * bloch[1] + axis[2] * bloch[2];
                let cross = [axis[1] * bloch[2] - axis[2] * bloch[1], axis[2] * bloch[0] - axis[0] * bloch[2], axis[0] * bloch[1] - axis[1] * bloch[0]];
                let mut out = [0.0; 3];
                for i in 0..3 {
                    out[i] = bloch[i] * co + cross[i] * si + axis[i] * dot * (1.0 - co);
                }
                (out, err)
            };
            let pauli = [[(1.0 + b2[0]) / 2.0, (1.0 - b2[0]) / 2.0], [(1.0 + b2[1]) / 2.0, (1.0 - b2[1]) / 2.0], [(1.0 + b2[2]) / 2.0, (1.0 - b2[2]) / 2.0]];
            // Rodrigues with unit inputs: a few tens of roundings, plus the angle error (a
            // rotation by d theta moves each Bloch component by at most d theta)
            let bound = SAFETY * (64.0 * U + angle_err);
            IdealRef { pauli, bound }
        })
        .collect()
}
