//! Direct-matrix path: Page-Wootters numerics by explicit complex linear algebra
//! on the full 2N-dimensional composite space. Produces `clock_probability` and
//! `pauli`. No closed-form Schrodinger formula appears here (see reference.rs).
//! Arithmetic binary64, bounds ESTIMATED (NUMERICAL_ERROR.md). No clock, file,
//! process, thread or random facility.

use super::case::{kernel_pairs, Case};
use super::complex::*;

pub const EPS: f64 = 2.220446049250313e-16; // 2^-52
pub const TWO_PI: f64 = 2.0 * std::f64::consts::PI;

pub fn pauli(axis: usize) -> Mat {
    match axis {
        0 => real_mat(&[[0.0, 1.0], [1.0, 0.0]]),
        1 => vec![vec![C::ZERO, -C::I], vec![C::I, C::ZERO]],
        _ => real_mat(&[[1.0, 0.0], [0.0, -1.0]]),
    }
}

pub fn clock_hamiltonian(c: &Case) -> Mat {
    let n = c.clock_dim;
    let mut h = zeros(n, n);
    for j in 0..n {
        h[j][j] = C::real(c.energies[j].to_f64());
    }
    h
}

pub fn system_hamiltonian(c: &Case) -> Mat {
    let mut hs = scale(&identity(2), c.pauli[0].to_f64());
    for (i, ax) in (1..4).zip(0..3) {
        hs = add_scaled(&hs, &pauli(ax), c.pauli[i].to_f64());
    }
    hs
}

pub fn total_hamiltonian(c: &Case) -> Mat {
    let hc = clock_hamiltonian(c);
    add_scaled(&kron(&hc, &identity(2)), &kron(&identity(c.clock_dim), &system_hamiltonian(c)), 1.0)
}

/// P_0, P_1 for eigenvalues h0 + |h| and h0 - |h| (computational split when |h| = 0).
pub fn system_eigenprojectors(c: &Case) -> [Mat; 2] {
    let hn = c.h_norm.to_f64();
    if hn == 0.0 {
        return [real_mat(&[[1.0, 0.0], [0.0, 0.0]]), real_mat(&[[0.0, 0.0], [0.0, 1.0]])];
    }
    let mut ns = zeros(2, 2);
    for (i, ax) in (1..4).zip(0..3) {
        ns = add_scaled(&ns, &pauli(ax), c.pauli[i].to_f64() / hn);
    }
    [scale(&add_scaled(&identity(2), &ns, 1.0), 0.5), scale(&add_scaled(&identity(2), &ns, -1.0), 0.5)]
}

fn clock_basis_projector(n: usize, j: usize) -> Mat {
    let mut e = zeros(n, n);
    e[j][j] = C::ONE;
    e
}

/// Orthogonal projector onto ker H_total from the exact rational pairs.
pub fn kernel_projector(c: &Case) -> Mat {
    let n = c.clock_dim;
    let p = system_eigenprojectors(c);
    let mut out = zeros(2 * n, 2 * n);
    for (j, s) in kernel_pairs(c) {
        out = add_scaled(&out, &kron(&clock_basis_projector(n, j), &p[s]), 1.0);
    }
    out
}

/// |t_k> = N^(-1/2) sum_j exp(-2 pi i E_j k tau) |E_j>
pub fn clock_state(c: &Case, k: usize) -> Vec<C> {
    let amp = 1.0 / (c.clock_dim as f64).sqrt();
    let tau = c.tau.to_f64();
    c.energies.iter().map(|e| C::cis(-TWO_PI * e.to_f64() * k as f64 * tau).scale(amp)).collect()
}

pub fn reference_system_vector(c: &Case) -> [C; 2] {
    [C::new(c.state[0].0.to_f64(), c.state[0].1.to_f64()), C::new(c.state[1].0.to_f64(), c.state[1].1.to_f64())]
}

/// |Psi> = P_0 (|t_r> (x) |psi_0>), unnormalized.
pub fn physical_state(c: &Case) -> Vec<C> {
    let tr = clock_state(c, c.ref_index);
    let psi0 = reference_system_vector(c);
    let mut v = Vec::with_capacity(2 * c.clock_dim);
    for t in &tr {
        v.push(*t * psi0[0]);
        v.push(*t * psi0[1]);
    }
    matvec(&kernel_projector(c), &v)
}

/// || H_total Psi_hat ||_2, None when Psi is numerically zero.
pub fn constraint_residual(c: &Case, psi: &[C]) -> Option<f64> {
    let n2 = vnorm2(psi);
    if n2 == 0.0 {
        return None;
    }
    Some((vnorm2(&matvec(&total_hamiltonian(c), psi)) / n2).sqrt())
}

/// || sum_k w |t_k><t_k| - I_C ||_F
pub fn povm_residual(c: &Case) -> f64 {
    let w = c.weight.to_f64();
    let mut s = scale(&identity(c.clock_dim), -1.0);
    for k in 0..c.labels.len() {
        let t = clock_state(c, k);
        s = add_scaled(&s, &outer(&t, &t), w);
    }
    frobenius(&s)
}

/// phi_k = (<t_k| (x) I_S) Psi
pub fn condition_vector(c: &Case, psi: &[C], k: usize) -> [C; 2] {
    let t = clock_state(c, k);
    let mut phi = [C::ZERO; 2];
    for (j, tj) in t.iter().enumerate() {
        for s in 0..2 {
            phi[s] = phi[s] + tj.conj() * psi[2 * j + s];
        }
    }
    phi
}

pub fn clock_probability(c: &Case, psi: &[C], k: usize) -> f64 {
    c.weight.to_f64() * vnorm2(&condition_vector(c, psi, k)) / vnorm2(psi)
}

/// rho = phi phi^dagger / ||phi||^2, None when phi is numerically zero.
pub fn conditional_density(phi: &[C; 2]) -> Option<Mat> {
    let n2 = vnorm2(phi);
    if n2 == 0.0 {
        return None;
    }
    Some(scale(&outer(phi, phi), 1.0 / n2))
}

/// [axis][sign] = Tr(rho (I +/- sigma)/2), computed directly, never as 1 - other.
pub fn pauli_probabilities(rho: &Mat) -> [[f64; 2]; 3] {
    let mut out = [[0.0; 2]; 3];
    for (ax, row) in out.iter_mut().enumerate() {
        for (si, sg) in [1.0, -1.0].iter().enumerate() {
            let proj = scale(&add_scaled(&identity(2), &pauli(ax), *sg), 0.5);
            row[si] = trace(&matmul(rho, &proj)).re;
        }
    }
    out
}

// ---- density-matrix route (independent of the vector route above) -------------

pub fn density_from_vector(psi: &[C]) -> Mat {
    outer(psi, psi)
}

/// sum over kernel pairs of Pi_js R Pi_js: removes every coherence between distinct
/// kernel branches |E_j>(x)|e_s>. Still annihilated by H_total: a stationary mixed
/// state, the negative control.
pub fn dephase_kernel_branches(c: &Case, r: &Mat) -> Mat {
    let n = c.clock_dim;
    let p = system_eigenprojectors(c);
    let mut out = zeros(2 * n, 2 * n);
    for (j, s) in kernel_pairs(c) {
        let pi = kron(&clock_basis_projector(n, j), &p[s]);
        out = add_scaled(&out, &matmul(&pi, &matmul(r, &pi)), 1.0);
    }
    out
}

/// (<t_k| (x) I) R (|t_k> (x) I) as a 2 x 2 matrix, and its trace.
pub fn condition_density(c: &Case, r: &Mat, k: usize) -> (Mat, f64) {
    let t = clock_state(c, k);
    let mut tm = zeros(2 * c.clock_dim, 2);
    for (j, tj) in t.iter().enumerate() {
        tm[2 * j][0] = *tj;
        tm[2 * j + 1][1] = *tj;
    }
    let red = matmul(&dagger(&tm), &matmul(r, &tm));
    let tr = trace(&red).re;
    (red, tr)
}

pub fn conditional_from_density(c: &Case, r: &Mat, k: usize) -> (f64, Option<Mat>) {
    let (red, tr) = condition_density(c, r, k);
    let p = c.weight.to_f64() * tr / trace(r).re;
    let rho = if tr != 0.0 { Some(scale(&red, 1.0 / tr)) } else { None };
    (p, rho)
}

pub fn bound_estimate(c: &Case) -> f64 {
    8.0 * (c.clock_dim + c.labels.len()) as f64 * EPS
}

pub struct MatrixValues {
    pub kernel_dim: usize,
    pub psi_norm2: f64,
    pub constraint_residual: Option<f64>,
    pub povm_residual: f64,
    pub clock_probability: Vec<f64>,
    pub rho: Vec<Option<Mat>>,
    pub pauli: Vec<Option<[[f64; 2]; 3]>>,
    pub bound: f64,
}

impl MatrixValues {
    pub fn live(&self) -> bool {
        self.kernel_dim >= 1 && self.psi_norm2 > 0.0
    }
}

/// All matrix-path quantities for a parsed case.
pub fn evaluate(c: &Case, dephased: bool) -> MatrixValues {
    let psi = physical_state(c);
    let mut out = MatrixValues {
        kernel_dim: kernel_pairs(c).len(),
        psi_norm2: vnorm2(&psi),
        constraint_residual: constraint_residual(c, &psi),
        povm_residual: povm_residual(c),
        clock_probability: Vec::new(),
        rho: Vec::new(),
        pauli: Vec::new(),
        bound: bound_estimate(c),
    };
    if !out.live() {
        return out;
    }
    let mut r = density_from_vector(&psi);
    if dephased {
        r = dephase_kernel_branches(c, &r);
    }
    for k in 0..c.labels.len() {
        let (p, rho) = if dephased {
            conditional_from_density(c, &r, k)
        } else {
            (clock_probability(c, &psi, k), conditional_density(&condition_vector(c, &psi, k)))
        };
        out.clock_probability.push(p);
        out.pauli.push(rho.as_ref().map(pauli_probabilities));
        out.rho.push(rho);
    }
    out
}

pub fn hermiticity_defect(rho: &Mat) -> f64 {
    frobenius(&add_scaled(rho, &dagger(rho), -1.0))
}

pub fn eigenvalues_2x2_hermitian(rho: &Mat) -> (f64, f64) {
    let tr = trace(rho).re;
    let det = (rho[0][0] * rho[1][1] - rho[0][1] * rho[1][0]).re;
    let r = (tr * tr - 4.0 * det).max(0.0).sqrt();
    ((tr - r) / 2.0, (tr + r) / 2.0)
}
