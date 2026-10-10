//! Literal matrix route (the oracle record's own "engine" values, and an internal
//! cross-check of the closed form): build H_total = H_C (x) I + I (x) H_S + V as a dense
//! 2N x 2N matrix, the orthogonal projector onto its kernel from the exact kernel pairs
//! (AT1_SPEC 8.4 item 3: never from a numerical eigen-decomposition), Psi = P_0 (|t_r> (x)
//! psi_0), phi_k by applying the bra <t_k| literally, rho_k as a 2 x 2 density matrix and
//! every Pauli probability as Tr(rho_k Pi). The POVM residual is the Frobenius norm of the
//! explicit sum of the effects minus the identity.
//!
//! Shares with closed.rs only the exact level decisions (model.rs) and the exact phase
//! reduction; the linear algebra and the order of operations are different.

use super::big::Q;
use super::case::Case;
use super::closed::{pauli_bounds, CIS_ERR, SAFETY, U};
use super::complex::{kron, matvec, outer, real_mat, trace, vnorm2, zeros, Mat, C};
use super::model::{cis_turns, gq_f64, LevelKind, Levels};

#[derive(Clone, Debug)]
pub struct LabelVal {
    pub p: f64,
    pub p_bound: f64,
    pub pauli: Option<[[f64; 2]; 3]>,
    /// Per value, [axis][PLUS, MINUS]; zeros when `pauli` is None.
    pub pauli_bound: [[f64; 2]; 3],
}

#[derive(Clone, Debug)]
pub struct MatrixValues {
    pub kernel_dim: usize,
    pub trivial: bool,
    /// (value, bound); None on a trivial kernel.
    pub constraint: Option<(f64, f64)>,
    pub povm: (f64, f64),
    pub labels: Vec<LabelVal>,
}

fn pauli_projectors() -> [[Mat; 2]; 3] {
    let x = [[0.5, 0.5], [0.5, 0.5]];
    let xm = [[0.5, -0.5], [-0.5, 0.5]];
    let yp: Mat = vec![vec![C::new(0.5, 0.0), C::new(0.0, -0.5)], vec![C::new(0.0, 0.5), C::new(0.5, 0.0)]];
    let ym: Mat = vec![vec![C::new(0.5, 0.0), C::new(0.0, 0.5)], vec![C::new(0.0, -0.5), C::new(0.5, 0.0)]];
    [[real_mat(&x), real_mat(&xm)], [yp, ym], [real_mat(&[[1.0, 0.0], [0.0, 0.0]]), real_mat(&[[0.0, 0.0], [0.0, 1.0]])]]
}

fn sigma(v: &[f64; 3]) -> Mat {
    vec![vec![C::new(v[2], 0.0), C::new(v[0], -v[1])], vec![C::new(v[0], v[1]), C::new(-v[2], 0.0)]]
}

/// |t_k> = N^(-1/2) sum_j exp(-2 pi i E_j k tau) |E_j>, phase reduced exactly.
fn clock_state(c: &Case, k: usize) -> Vec<C> {
    let inv = 1.0 / (c.clock_dim as f64).sqrt();
    c.energies.iter().map(|e| cis_turns(&e.mul(&Q::int(k as i64)).mul(&c.tau).neg()).scale(inv)).collect()
}

pub fn evaluate(c: &Case, lv: &Levels) -> MatrixValues {
    let n = c.clock_dim;
    let m = c.labels.len();
    let dim = 2 * n;
    let w = c.weight.to_f64();
    // H_total
    let mut hc = zeros(n, n);
    for j in 0..n {
        hc[j][j] = C::real(c.energies[j].to_f64());
    }
    let hvec = [c.h[0].to_f64(), c.h[1].to_f64(), c.h[2].to_f64()];
    let mut hs = sigma(&hvec);
    let h0 = c.h0.to_f64();
    hs[0][0] = hs[0][0] + C::real(h0);
    hs[1][1] = hs[1][1] + C::real(h0);
    let i2 = real_mat(&[[1.0, 0.0], [0.0, 1.0]]);
    let mut eye_n = zeros(n, n);
    for (j, row) in eye_n.iter_mut().enumerate() {
        row[j] = C::ONE;
    }
    let mut h_total = kron(&hc, &i2);
    let ihs = kron(&eye_n, &hs);
    let mut hmax: f64 = 0.0;
    for j in 0..n {
        let vj = [c.v[j][0].to_f64(), c.v[j][1].to_f64(), c.v[j][2].to_f64()];
        let sv = sigma(&vj);
        for a in 0..2 {
            for b in 0..2 {
                h_total[2 * j + a][2 * j + b] = h_total[2 * j + a][2 * j + b] + ihs[2 * j + a][2 * j + b] + sv[a][b];
            }
        }
        let nj = c.level_vector(j);
        hmax = hmax.max(c.energies[j].to_f64().abs() + h0.abs() + nj.iter().map(|q| q.to_f64().abs()).sum::<f64>());
    }
    // kernel projector from the exact kernel pairs
    let mut p0 = zeros(dim, dim);
    for (j, kind) in lv.kinds.iter().enumerate() {
        let mut add = |sys: [C; 2]| {
            let mut b = vec![C::ZERO; dim];
            b[2 * j] = sys[0];
            b[2 * j + 1] = sys[1];
            let o = outer(&b, &b);
            for r in 0..dim {
                for s in 0..dim {
                    p0[r][s] = p0[r][s] + o[r][s];
                }
            }
        };
        match kind {
            LevelKind::Pair { f, .. } => {
                let (a, b) = (gq_f64(&f[0]), gq_f64(&f[1]));
                let nrm = (a.norm2() + b.norm2()).sqrt();
                add([a.scale(1.0 / nrm), b.scale(1.0 / nrm)]);
            }
            LevelKind::Degenerate => {
                add([C::ONE, C::ZERO]);
                add([C::ZERO, C::ONE]);
            }
            LevelKind::Out => {}
        }
    }
    let clocks: Vec<Vec<C>> = (0..m).map(|k| clock_state(c, k)).collect();
    // POVM residual
    let mut fsum = zeros(n, n);
    for t in &clocks {
        let o = outer(t, t);
        for a in 0..n {
            for b in 0..n {
                fsum[a][b] = fsum[a][b] + o[a][b].scale(w);
            }
        }
    }
    for (a, row) in fsum.iter_mut().enumerate() {
        row[a] = row[a] - C::ONE;
    }
    let povm_val = fsum.iter().flatten().map(|x| x.norm2()).sum::<f64>().sqrt();
    let povm_bound = SAFETY * ((16.0 + m as f64) * U * w * m as f64 + 8.0 * U * povm_val);
    if lv.trivial {
        return MatrixValues { kernel_dim: lv.kernel_dim, trivial: true, constraint: None, povm: (povm_val, povm_bound), labels: Vec::new() };
    }
    let psi0 = [gq_f64(&c.psi[0]), gq_f64(&c.psi[1])];
    let psi0_norm = (psi0[0].norm2() + psi0[1].norm2()).sqrt();
    let tr = &clocks[c.ref_index];
    let mut x = vec![C::ZERO; dim];
    for j in 0..n {
        x[2 * j] = tr[j] * psi0[0];
        x[2 * j + 1] = tr[j] * psi0[1];
    }
    let psi = matvec(&p0, &x);
    let psi_n2 = vnorm2(&psi);
    let psi_norm = psi_n2.sqrt();
    // Error of Psi (first order, standard model; NUMERICAL_ERROR.md): t_j carries
    // (CIS_ERR + 3) U (phase, 1/sqrt(N), scaling); x_j = t_j psi_0 adds U (conversion) and
    // sqrt(5) U (product), so ||dx_j|| <= (CIS_ERR + 7) U ||x_j||. Each kernel block of P_0
    // is b b^dagger with b a converted, normalized eigenvector (about 13.3 U per entry,
    // Frobenius norm 1) or the exact identity; the two-term block product adds
    // (sqrt(5) + 1) sqrt(2) U. Per block ||dPsi_j|| <= (CIS_ERR + 28) U ||x_j|| with
    // ||x_j|| = ||psi_0|| / sqrt(N), summed over the K kernel blocks (other blocks are exact
    // zeros).
    let k_levels = lv.kinds.iter().filter(|k| !matches!(k, LevelKind::Out)).count() as f64;
    let dpsi = (CIS_ERR + 28.0) * U * k_levels * psi0_norm / (n as f64).sqrt();
    let hpsi = matvec(&h_total, &psi);
    let constraint_val = (vnorm2(&hpsi) / psi_n2).sqrt();
    // the exact Psi has H Psi = 0, so the computed residual is about ||H dPsi|| / ||Psi||
    // plus the rounding of H (inputs and level sums) and of the block rows
    let constraint_bound = SAFETY * hmax * (dpsi / psi_norm + (16.0 + 4.0 * n as f64) * U);
    // relative error of ||Psi||^2: twice the relative error of Psi plus the 2N-term sum
    let rel_psi_n2 = 2.0 * dpsi / psi_norm + (2.0 * dim as f64 + 4.0) * U;
    let proj = pauli_projectors();
    let inv_sqrt_n = 1.0 / (n as f64).sqrt();
    let labels = clocks
        .iter()
        .map(|t| {
            let mut phi = [C::ZERO, C::ZERO];
            let mut mag = 0.0;
            for j in 0..n {
                let bra = t[j].conj();
                phi[0] = phi[0] + bra * psi[2 * j];
                phi[1] = phi[1] + bra * psi[2 * j + 1];
                mag += (psi[2 * j].norm2() + psi[2 * j + 1].norm2()).sqrt() * inv_sqrt_n;
            }
            let nn = phi[0].norm2() + phi[1].norm2();
            let p = w * nn / psi_n2;
            // phi_a = sum_j conj(t_j) Psi_ja: per level |t_j| (||dPsi_j|| + ((CIS_ERR + 3) +
            // sqrt(5) + (N - 1)) U ||Psi_j||), triangle inequality over (|Psi_j0|, |Psi_j1|)
            let dphi = dpsi * inv_sqrt_n + (CIS_ERR + 5.0 + n as f64) * U * mag;
            let norm = nn.sqrt();
            let p_bound = SAFETY * (w * (2.0 * norm * dphi + dphi * dphi) / psi_n2 + p * (rel_psi_n2 + 8.0 * U));
            let (pauli, pauli_bound) = if nn > 0.0 {
                let rho: Mat = outer(&phi, &phi).iter().map(|row| row.iter().map(|z| z.scale(1.0 / nn)).collect()).collect();
                let mut out = [[0.0; 2]; 3];
                for a in 0..3 {
                    for s in 0..2 {
                        out[a][s] = trace(&super::complex::matmul(&rho, &proj[a][s])).re;
                    }
                }
                // the trace route adds a few roundings over the pure-state formula; the 16 U
                // term of pauli_bounds covers them
                (Some(out), pauli_bounds(&out, dphi / norm))
            } else {
                (None, [[0.0; 2]; 3])
            };
            LabelVal { p, p_bound, pauli, pauli_bound }
        })
        .collect();
    MatrixValues { kernel_dim: lv.kernel_dim, trivial: false, constraint: Some((constraint_val, constraint_bound)), povm: (povm_val, povm_bound), labels }
}
