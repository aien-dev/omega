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
use super::complex::{matvec, outer, real_mat, trace, vnorm2, zeros, Mat, C};
use super::model::{cis_turns, gq_f64, CSum, LevelKind, Levels};

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
    // H_total = H_C (x) I + I (x) H_S + V, dense 2N x 2N. Each 2 x 2 diagonal block
    // (E_j + h0) I + n_j . sigma, n_j = h + v_j, is formed in exact rationals and converted to
    // binary64 once per entry, so a large h cancelling a large v_j costs nothing (review
    // finding 1). Off-diagonal blocks are exact zeros.
    let mut h_total = zeros(dim, dim);
    // largest operator norm of a block that carries part of Psi: a kernel block has
    // eigenvalues 0 and E_j + h0 - s R_j = -2 s R_j, so its norm is 2 R_j (0 when degenerate)
    let mut gmax: f64 = 0.0;
    for j in 0..n {
        let nj = c.level_vector(j);
        let d = c.energies[j].add(&c.h0);
        let blk = [
            [C::real(d.add(&nj[2]).to_f64()), C::new(nj[0].to_f64(), nj[1].neg().to_f64())],
            [C::new(nj[0].to_f64(), nj[1].to_f64()), C::real(d.sub(&nj[2]).to_f64())],
        ];
        for a in 0..2 {
            for b in 0..2 {
                h_total[2 * j + a][2 * j + b] = blk[a][b];
            }
        }
        if let LevelKind::Pair { .. } = lv.kinds[j] {
            gmax = gmax.max(2.0 * c.level_norm[j].to_f64());
        }
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
    // POVM residual: the explicit sum of the effects F_k = w |t_k><t_k| minus I. Entry (a, b)
    // of F_k is (w / N) exp(-2 pi i (E_a - E_b) k tau); its phase is reduced exactly as one
    // turn count (the product of the two clock-state entries, an exact identity), and the M
    // terms are summed with compensation, so the entry error is about
    // (CIS_ERR + 4) U w M / N independent of M (review finding 2).
    let wn = c.weight.div(&Q::int(n as i64));
    let wn_f = wn.to_f64();
    let mut fro2 = 0.0;
    for a in 0..n {
        for b in 0..n {
            let mut acc = CSum::new();
            let d = c.energies[a].sub(&c.energies[b]).mul(&c.tau).neg();
            for k in 0..m {
                acc.add(cis_turns(&d.mul(&Q::int(k as i64))).scale(wn_f));
            }
            let mut e = acc.value();
            if a == b {
                e = e - C::ONE;
            }
            fro2 += e.norm2();
        }
    }
    let povm_val = fro2.sqrt();
    // Frobenius norm over N^2 entries of size error (CIS_ERR + 4) U w M / N: at most
    // N (CIS_ERR + 4) U w M; then the rounding of the norm itself
    let povm_bound = SAFETY * ((CIS_ERR + 4.0) * U * w * m as f64 + 8.0 * U * povm_val);
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
    // the exact Psi has H Psi = 0 and lives on kernel blocks only (other blocks of Psi are
    // exact zeros, degenerate blocks of H are exact zeros), so the computed residual is
    // ||H dPsi|| / ||Psi|| <= gmax dPsi / ||Psi|| plus the rounding of the block entries (U
    // each) and of the two-term block rows ((sqrt(5) + 1) U), about 6.2 U gmax; written 8 U
    let constraint_bound = SAFETY * gmax * (dpsi / psi_norm + 8.0 * U);
    // relative error of ||Psi||^2: twice the relative error of Psi plus the 2N-term sum
    let rel_psi_n2 = 2.0 * dpsi / psi_norm + (2.0 * dim as f64 + 4.0) * U;
    let proj = pauli_projectors();
    let inv_sqrt_n = 1.0 / (n as f64).sqrt();
    let labels = clocks
        .iter()
        .map(|t| {
            let (mut p0s, mut p1s) = (CSum::new(), CSum::new());
            let mut mag = 0.0;
            for j in 0..n {
                let bra = t[j].conj();
                p0s.add(bra * psi[2 * j]);
                p1s.add(bra * psi[2 * j + 1]);
                mag += (psi[2 * j].norm2() + psi[2 * j + 1].norm2()).sqrt() * inv_sqrt_n;
            }
            let phi = [p0s.value(), p1s.value()];
            let nn = phi[0].norm2() + phi[1].norm2();
            let p = w * nn / psi_n2;
            // phi_a = sum_j conj(t_j) Psi_ja, compensated: per level |t_j| (||dPsi_j|| +
            // ((CIS_ERR + 3) + sqrt(5) + 2) U ||Psi_j||), triangle inequality over (|Psi_j0|, |Psi_j1|)
            let dphi = dpsi * inv_sqrt_n + (CIS_ERR + 8.0) * U * mag;
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
