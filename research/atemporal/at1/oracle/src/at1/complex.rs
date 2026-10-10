//! Explicit complex numbers and small dense matrices (Vec<Vec<C>>). Written
//! here; no numeric library, nothing shared with the C11 engine.

use std::ops::{Add, Mul, Neg, Sub};

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct C {
    pub re: f64,
    pub im: f64,
}

impl C {
    pub const ZERO: C = C { re: 0.0, im: 0.0 };
    pub const ONE: C = C { re: 1.0, im: 0.0 };
    pub const I: C = C { re: 0.0, im: 1.0 };
    pub fn new(re: f64, im: f64) -> C {
        C { re, im }
    }
    pub fn real(re: f64) -> C {
        C { re, im: 0.0 }
    }
    pub fn conj(self) -> C {
        C { re: self.re, im: -self.im }
    }
    pub fn norm2(self) -> f64 {
        self.re * self.re + self.im * self.im
    }
    pub fn scale(self, s: f64) -> C {
        C { re: self.re * s, im: self.im * s }
    }
    /// exp(i theta)
    pub fn cis(theta: f64) -> C {
        C { re: theta.cos(), im: theta.sin() }
    }
}

impl Add for C {
    type Output = C;
    fn add(self, o: C) -> C {
        C { re: self.re + o.re, im: self.im + o.im }
    }
}
impl Sub for C {
    type Output = C;
    fn sub(self, o: C) -> C {
        C { re: self.re - o.re, im: self.im - o.im }
    }
}
impl Mul for C {
    type Output = C;
    fn mul(self, o: C) -> C {
        C { re: self.re * o.re - self.im * o.im, im: self.re * o.im + self.im * o.re }
    }
}
impl Neg for C {
    type Output = C;
    fn neg(self) -> C {
        C { re: -self.re, im: -self.im }
    }
}

pub type Mat = Vec<Vec<C>>;

pub fn zeros(n: usize, m: usize) -> Mat {
    vec![vec![C::ZERO; m]; n]
}

pub fn identity(n: usize) -> Mat {
    let mut a = zeros(n, n);
    for (i, row) in a.iter_mut().enumerate() {
        row[i] = C::ONE;
    }
    a
}

pub fn matmul(a: &Mat, b: &Mat) -> Mat {
    let (n, k, m) = (a.len(), b.len(), b[0].len());
    let mut out = zeros(n, m);
    for i in 0..n {
        for j in 0..m {
            let mut s = C::ZERO;
            for l in 0..k {
                s = s + a[i][l] * b[l][j];
            }
            out[i][j] = s;
        }
    }
    out
}

pub fn matvec(a: &Mat, v: &[C]) -> Vec<C> {
    a.iter().map(|row| row.iter().zip(v).fold(C::ZERO, |s, (x, y)| s + *x * *y)).collect()
}

pub fn dagger(a: &Mat) -> Mat {
    let (n, m) = (a.len(), a[0].len());
    let mut out = zeros(m, n);
    for i in 0..n {
        for j in 0..m {
            out[j][i] = a[i][j].conj();
        }
    }
    out
}

pub fn kron(a: &Mat, b: &Mat) -> Mat {
    let (n, m, p, q) = (a.len(), a[0].len(), b.len(), b[0].len());
    let mut out = zeros(n * p, m * q);
    for i in 0..n {
        for j in 0..m {
            for k in 0..p {
                for l in 0..q {
                    out[i * p + k][j * q + l] = a[i][j] * b[k][l];
                }
            }
        }
    }
    out
}

/// a + s * b
pub fn add_scaled(a: &Mat, b: &Mat, s: f64) -> Mat {
    a.iter().zip(b).map(|(ra, rb)| ra.iter().zip(rb).map(|(x, y)| *x + y.scale(s)).collect()).collect()
}

pub fn scale(a: &Mat, s: f64) -> Mat {
    a.iter().map(|row| row.iter().map(|x| x.scale(s)).collect()).collect()
}

/// u v^dagger
pub fn outer(u: &[C], v: &[C]) -> Mat {
    u.iter().map(|x| v.iter().map(|y| *x * y.conj()).collect()).collect()
}

pub fn vnorm2(v: &[C]) -> f64 {
    v.iter().map(|x| x.norm2()).sum()
}

pub fn frobenius(a: &Mat) -> f64 {
    a.iter().flatten().map(|x| x.norm2()).sum::<f64>().sqrt()
}

pub fn trace(a: &Mat) -> C {
    (0..a.len()).fold(C::ZERO, |s, i| s + a[i][i])
}

pub fn real_mat(rows: &[[f64; 2]; 2]) -> Mat {
    rows.iter().map(|r| r.iter().map(|x| C::real(*x)).collect()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn algebra() {
        let x = real_mat(&[[0.0, 1.0], [1.0, 0.0]]);
        let y: Mat = vec![vec![C::ZERO, -C::I], vec![C::I, C::ZERO]];
        let z = real_mat(&[[1.0, 0.0], [0.0, -1.0]]);
        // X Y = i Z
        let xy = matmul(&x, &y);
        assert_eq!(xy, scale_c(&z, C::I));
        assert_eq!(trace(&matmul(&x, &x)), C::real(2.0));
        assert_eq!(kron(&identity(2), &identity(3)), identity(6));
        assert_eq!(dagger(&y), y);
        assert_eq!(frobenius(&identity(4)), 2.0);
    }

    fn scale_c(a: &Mat, c: C) -> Mat {
        a.iter().map(|row| row.iter().map(|x| *x * c).collect()).collect()
    }
}
