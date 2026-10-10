//! Double-double arithmetic (about 106 bits), deterministic on every IEEE-754
//! platform: only +, -, *, / and fused multiply-add (f64::mul_add, which is
//! correctly rounded by definition). Used by the shadow model when a phase is
//! not a quarter turn (then exact Gaussian-rational arithmetic is impossible).

use crate::big::Int;
use crate::rat::Q;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Dd {
    pub hi: f64,
    pub lo: f64,
}

fn two_sum(a: f64, b: f64) -> (f64, f64) {
    let s = a + b;
    let bb = s - a;
    (s, (a - (s - bb)) + (b - bb))
}
fn quick_two_sum(a: f64, b: f64) -> (f64, f64) {
    let s = a + b;
    (s, b - (s - a))
}
fn two_prod(a: f64, b: f64) -> (f64, f64) {
    let p = a * b;
    (p, a.mul_add(b, -p))
}

pub fn ldexp(mut x: f64, mut e: i32) -> f64 {
    while e > 1000 {
        x *= f64::from_bits(((1000 + 1023) as u64) << 52);
        e -= 1000;
    }
    while e < -1000 {
        x *= f64::from_bits(((-1000i32 + 1023) as u64) << 52);
        e += 1000;
    }
    x * f64::from_bits(((e + 1023) as u64) << 52)
}

pub const PI2: Dd = Dd { hi: 6.283185307179586232e+00, lo: 2.449293598294706414e-16 };

impl Dd {
    pub fn new(hi: f64) -> Dd {
        Dd { hi, lo: 0.0 }
    }
    pub fn zero() -> Dd {
        Dd::new(0.0)
    }
    pub fn one() -> Dd {
        Dd::new(1.0)
    }
    pub fn add(self, o: Dd) -> Dd {
        let (s, e) = two_sum(self.hi, o.hi);
        let (t, f) = two_sum(self.lo, o.lo);
        let e = e + t;
        let (s, e) = quick_two_sum(s, e);
        let e = e + f;
        let (hi, lo) = quick_two_sum(s, e);
        Dd { hi, lo }
    }
    pub fn neg(self) -> Dd {
        Dd { hi: -self.hi, lo: -self.lo }
    }
    pub fn sub(self, o: Dd) -> Dd {
        self.add(o.neg())
    }
    pub fn mul(self, o: Dd) -> Dd {
        let (p, e) = two_prod(self.hi, o.hi);
        let e = e + (self.hi * o.lo + self.lo * o.hi);
        let (hi, lo) = quick_two_sum(p, e);
        Dd { hi, lo }
    }
    pub fn mul_f(self, f: f64) -> Dd {
        self.mul(Dd::new(f))
    }
    pub fn div(self, o: Dd) -> Dd {
        let q1 = self.hi / o.hi;
        let r = self.sub(o.mul_f(q1));
        let q2 = r.hi / o.hi;
        let r = r.sub(o.mul_f(q2));
        let q3 = r.hi / o.hi;
        let (hi, lo) = quick_two_sum(q1, q2);
        Dd { hi, lo }.add(Dd::new(q3))
    }
    pub fn sqrt(self) -> Dd {
        if self.hi <= 0.0 {
            return Dd::zero();
        }
        let x = 1.0 / self.hi.sqrt();
        let ax = self.hi * x;
        let (p, e) = two_prod(ax, ax);
        let diff = self.sub(Dd { hi: p, lo: e });
        Dd::new(ax).add(Dd::new(diff.hi * (x * 0.5)))
    }
    pub fn abs(self) -> Dd {
        if self.hi < 0.0 || (self.hi == 0.0 && self.lo < 0.0) {
            self.neg()
        } else {
            self
        }
    }
    pub fn to_q(self) -> Q {
        Q::from_f64_bits(self.hi.to_bits()).add(&Q::from_f64_bits(self.lo.to_bits()))
    }
    /// Conversion of an exact rational, relative error about 2^-110.
    pub fn from_q(q: &Q) -> Dd {
        if q.is_zero() {
            return Dd::zero();
        }
        let neg = q.n.is_neg();
        let n = q.n.abs();
        let d = &q.d;
        let k: i64 = 120 + d.bits() as i64 - n.bits() as i64;
        let big = if k >= 0 { n.shl(k as u32).divrem(d).0 } else { n.divrem(&d.shl((-k) as u32)).0 };
        let (top, sh) = big.top_bits(53);
        let hi_int = Int::from_u64(top).shl(sh as u32);
        let rest = big.sub(&hi_int);
        let (rtop, rsh) = rest.top_bits(53);
        let hi = ldexp(top as f64, sh - k as i32);
        let lo = ldexp(rtop as f64, rsh - k as i32);
        let r = Dd::new(hi).add(Dd::new(lo));
        if neg {
            r.neg()
        } else {
            r
        }
    }
    /// floor of hi part as an exact integer adjustment for turn reduction.
    pub fn frac_turn(self) -> Dd {
        let f = self.hi.floor();
        let r = self.sub(Dd::new(f));
        if r.hi < 0.0 {
            r.add(Dd::one())
        } else if r.hi >= 1.0 {
            r.sub(Dd::one())
        } else {
            r
        }
    }
}

/// (cos, sin) of 2*pi*x for x in [0, 1/4] by Taylor series in double-double.
fn cos_sin_quarter(x: Dd) -> (Dd, Dd) {
    let t = PI2.mul(x);
    let t2 = t.mul(t);
    // sin
    let mut term = t;
    let mut s = t;
    let mut n = 1.0;
    for _ in 0..40 {
        term = term.mul(t2).div(Dd::new((n + 1.0) * (n + 2.0))).neg();
        n += 2.0;
        s = s.add(term);
        if term.hi.abs() < 1e-36 {
            break;
        }
    }
    let mut term = Dd::one();
    let mut c = Dd::one();
    let mut n = 0.0;
    for _ in 0..40 {
        term = term.mul(t2).div(Dd::new((n + 1.0) * (n + 2.0))).neg();
        n += 2.0;
        c = c.add(term);
        if term.hi.abs() < 1e-36 {
            break;
        }
    }
    (c, s)
}

/// (cos, sin) of 2*pi*turns for any turn count (reduced modulo 1 first).
pub fn cos_sin_turns(turns: Dd) -> (Dd, Dd) {
    let f = turns.frac_turn();
    let q4 = (f.hi * 4.0).floor().max(0.0).min(3.0);
    let r = f.sub(Dd::new(q4 * 0.25));
    let (c, s) = cos_sin_quarter(r);
    match q4 as i32 {
        0 => (c, s),
        1 => (s.neg(), c),
        2 => (c.neg(), s.neg()),
        _ => (s, c.neg()),
    }
}

/// Exact reduction of a rational turn count, then (cos, sin).
pub fn cos_sin_turns_q(turns: &Q) -> (Dd, Dd) {
    let f = turns.mod1();
    let f4 = f.mul(&Q::int(4));
    if f4.is_int() {
        let k = f4.n.low_u64();
        return match k {
            0 => (Dd::one(), Dd::zero()),
            1 => (Dd::zero(), Dd::one()),
            2 => (Dd::new(-1.0), Dd::zero()),
            _ => (Dd::zero(), Dd::new(-1.0)),
        };
    }
    cos_sin_turns(Dd::from_q(&f))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dd_basics() {
        let third = Dd::from_q(&Q::frac(1, 3));
        let back = third.mul_f(3.0).sub(Dd::one());
        assert!(back.hi.abs() < 1e-31);
        let s2 = Dd::new(2.0).sqrt();
        assert!(s2.mul(s2).sub(Dd::new(2.0)).hi.abs() < 1e-30);
        // cos(2 pi / 3) = -1/2, sin = sqrt(3)/2
        let (c, s) = cos_sin_turns_q(&Q::frac(1, 3));
        assert!(c.add(Dd::new(0.5)).hi.abs() < 1e-30);
        let s3 = Dd::new(3.0).sqrt().mul_f(0.5);
        assert!(s.sub(s3).hi.abs() < 1e-30);
        let (c, s) = cos_sin_turns_q(&Q::frac(1, 8));
        assert!(c.sub(s).hi.abs() < 1e-30);
        let (c, _) = cos_sin_turns_q(&Q::frac(-7, 3));
        assert!(c.add(Dd::new(0.5)).hi.abs() < 1e-30);
        let big = Dd::from_q(&Q::frac(524181, 2096716));
        assert!(big.to_q().sub(&Q::frac(524181, 2096716)).abs().cmp(&Q::frac(1, 1i64 << 62)) == std::cmp::Ordering::Less);
    }
}
