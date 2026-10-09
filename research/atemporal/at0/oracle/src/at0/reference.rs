//! Analytic reference path: the Schrodinger prediction without any clock matrix.
//!
//! 1. `schrodinger_probabilities`: exp(-i H_S t) = e^{-i h0 t} (cos(|h| t) I - i sin(|h| t) n.sigma)
//!    on psi_0 / ||psi_0||, then ||(I +/- sigma)/2 psi_t||^2. Two complex amplitudes.
//! 2. `bloch_probabilities`: Bloch vector of psi_0 rotated about n by 2 |h| t
//!    (Rodrigues), P(+1) = (1 + r_a)/2. Real arithmetic only.
//! t = t_k - t_r = 2 pi tau (k - r); the clock reading is declared data.

use super::case::Case;
use super::complex::C;
use super::matrix_path::{EPS, TWO_PI};

pub fn reading(c: &Case, k: usize) -> f64 {
    TWO_PI * k as f64 * c.tau.to_f64()
}

pub fn elapsed(c: &Case, k: usize) -> f64 {
    reading(c, k) - reading(c, c.ref_index)
}

fn normalized_psi0(c: &Case) -> (C, C) {
    let a = C::new(c.state[0].0.to_f64(), c.state[0].1.to_f64());
    let b = C::new(c.state[1].0.to_f64(), c.state[1].1.to_f64());
    let n = (a.norm2() + b.norm2()).sqrt();
    (a.scale(1.0 / n), b.scale(1.0 / n))
}

fn field(c: &Case) -> (f64, f64, f64, f64, f64) {
    (c.pauli[0].to_f64(), c.pauli[1].to_f64(), c.pauli[2].to_f64(), c.pauli[3].to_f64(), c.h_norm.to_f64())
}

/// psi_t = exp(-i H_S (t_k - t_r)) psi_0 / ||psi_0||
pub fn evolve(c: &Case, k: usize) -> (C, C) {
    let (a, b) = normalized_psi0(c);
    let (h0, hx, hy, hz, hn) = field(c);
    let t = elapsed(c, k);
    let ph = C::cis(-h0 * t);
    if hn == 0.0 {
        return (ph * a, ph * b);
    }
    let (nx, ny, nz) = (hx / hn, hy / hn, hz / hn);
    let (cs, sn) = ((hn * t).cos(), (hn * t).sin());
    // (cos I - i sin n.sigma)(a, b), n.sigma = [[nz, nx - i ny], [nx + i ny, -nz]]
    let ua = a.scale(cs) - C::I.scale(sn) * (a.scale(nz) + C::new(nx, -ny) * b);
    let ub = b.scale(cs) - C::I.scale(sn) * (C::new(nx, ny) * a - b.scale(nz));
    (ph * ua, ph * ub)
}

/// [axis][sign] probabilities on psi_t as squared norms of projections.
pub fn schrodinger_probabilities(c: &Case, k: usize) -> [[f64; 2]; 3] {
    let (a, b) = evolve(c, k);
    [
        [(a + b).norm2() / 2.0, (a - b).norm2() / 2.0],
        [(a - C::I * b).norm2() / 2.0, (a + C::I * b).norm2() / 2.0],
        [a.norm2(), b.norm2()],
    ]
}

pub fn bloch_vector(c: &Case) -> (f64, f64, f64) {
    let (a, b) = normalized_psi0(c);
    let ab = a.conj() * b;
    (2.0 * ab.re, 2.0 * ab.im, a.norm2() - b.norm2())
}

/// Rodrigues rotation of the Bloch vector about n by angle 2 |h| t.
pub fn bloch_probabilities(c: &Case, k: usize) -> [[f64; 2]; 3] {
    let (rx, ry, rz) = bloch_vector(c);
    let (_h0, hx, hy, hz, hn) = field(c);
    let t = elapsed(c, k);
    let (vx, vy, vz) = if hn == 0.0 {
        (rx, ry, rz)
    } else {
        let (nx, ny, nz) = (hx / hn, hy / hn, hz / hn);
        let th = 2.0 * hn * t;
        let (cs, sn) = (th.cos(), th.sin());
        let dot = nx * rx + ny * ry + nz * rz;
        let (cx, cy, cz) = (ny * rz - nz * ry, nz * rx - nx * rz, nx * ry - ny * rx);
        (
            rx * cs + cx * sn + nx * dot * (1.0 - cs),
            ry * cs + cy * sn + ny * dot * (1.0 - cs),
            rz * cs + cz * sn + nz * dot * (1.0 - cs),
        )
    };
    [[(1.0 + vx) / 2.0, (1.0 - vx) / 2.0], [(1.0 + vy) / 2.0, (1.0 - vy) / 2.0], [(1.0 + vz) / 2.0, (1.0 - vz) / 2.0]]
}

pub fn bound_estimate(c: &Case, k: usize) -> f64 {
    let (h0, _, _, _, hn) = field(c);
    64.0 * EPS * (1.0 + elapsed(c, k).abs() * (h0.abs() + hn))
}

pub struct RefValues {
    pub schrodinger: [[f64; 2]; 3],
    pub bloch: [[f64; 2]; 3],
    pub bound: f64,
}

pub fn evaluate(c: &Case) -> Vec<RefValues> {
    (0..c.labels.len())
        .map(|k| RefValues { schrodinger: schrodinger_probabilities(c, k), bloch: bloch_probabilities(c, k), bound: bound_estimate(c, k) })
        .collect()
}
