//! Exact rationals, Gaussian rationals, the contract's token grammars
//! (rational, complex rational, scaled decimal, integer) and the exact value
//! of a binary64 bit pattern.

use crate::big::Int;
use std::cmp::Ordering;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Q {
    pub n: Int,
    pub d: Int, // > 0, gcd(|n|, d) = 1
}

impl Q {
    pub fn new(n: Int, d: Int) -> Q {
        assert!(!d.is_zero(), "zero denominator");
        let (n, d) = if d.is_neg() { (n.neg(), d.neg()) } else { (n, d) };
        let g = Int::gcd(&n, &d);
        if g.is_zero() || g == Int::from_u64(1) {
            if n.is_zero() {
                return Q { n, d: Int::from_u64(1) };
            }
            return Q { n, d };
        }
        Q { n: n.divrem(&g).0, d: d.divrem(&g).0 }
    }
    pub fn int(v: i64) -> Q {
        Q { n: Int::from_i64(v), d: Int::from_u64(1) }
    }
    pub fn frac(n: i64, d: i64) -> Q {
        Q::new(Int::from_i64(n), Int::from_i64(d))
    }
    pub fn zero() -> Q {
        Q::int(0)
    }
    pub fn one() -> Q {
        Q::int(1)
    }
    pub fn is_zero(&self) -> bool {
        self.n.is_zero()
    }
    pub fn add(&self, o: &Q) -> Q {
        if self.d == o.d {
            return Q::new(self.n.add(&o.n), self.d.clone());
        }
        Q::new(self.n.mul(&o.d).add(&o.n.mul(&self.d)), self.d.mul(&o.d))
    }
    pub fn sub(&self, o: &Q) -> Q {
        self.add(&o.neg())
    }
    pub fn mul(&self, o: &Q) -> Q {
        Q::new(self.n.mul(&o.n), self.d.mul(&o.d))
    }
    pub fn div(&self, o: &Q) -> Q {
        assert!(!o.is_zero(), "division by zero rational");
        Q::new(self.n.mul(&o.d), self.d.mul(&o.n))
    }
    pub fn neg(&self) -> Q {
        Q { n: self.n.neg(), d: self.d.clone() }
    }
    pub fn abs(&self) -> Q {
        Q { n: self.n.abs(), d: self.d.clone() }
    }
    pub fn cmp(&self, o: &Q) -> Ordering {
        self.n.mul(&o.d).cmp(&o.n.mul(&self.d))
    }
    pub fn signum(&self) -> i32 {
        self.n.signum()
    }
    pub fn is_int(&self) -> bool {
        self.d == Int::from_u64(1)
    }
    pub fn floor(&self) -> Int {
        self.n.div_floor(&self.d).0
    }
    /// self - floor(self), in [0, 1).
    pub fn mod1(&self) -> Q {
        let (_, r) = self.n.div_floor(&self.d);
        Q::new(r, self.d.clone())
    }
    /// Exact square root when self is the square of a rational.
    pub fn sqrt_exact(&self) -> Option<Q> {
        if self.n.is_neg() {
            return None;
        }
        let a = self.n.isqrt();
        let b = self.d.isqrt();
        if a.mul(&a) == self.n && b.mul(&b) == self.d {
            Some(Q { n: a, d: b })
        } else {
            None
        }
    }
    /// Exact value of a finite binary64 bit pattern.
    pub fn from_f64_bits(bits: u64) -> Q {
        let neg = bits >> 63 == 1;
        let e = ((bits >> 52) & 0x7ff) as i32;
        let f = bits & ((1u64 << 52) - 1);
        assert!(e != 0x7ff, "non-finite pattern");
        let (m, exp) = if e == 0 { (f, -1074) } else { (f | (1u64 << 52), e - 1075) };
        let mi = Int::from_u64(m);
        let mi = if neg { mi.neg() } else { mi };
        if exp >= 0 {
            Q::new(mi.shl(exp as u32), Int::from_u64(1))
        } else {
            Q::new(mi, Int::pow2((-exp) as u32))
        }
    }
    /// Scaled decimal N@k as an exact rational.
    pub fn from_scaled(s: &Scaled) -> Q {
        Q::new(s.n.clone(), Int::pow10(s.k))
    }
    /// Nearest-ish binary64 (for display and for seeding double-double only).
    pub fn to_f64(&self) -> f64 {
        let dd = crate::dd::Dd::from_q(self);
        dd.hi
    }
    pub fn text(&self) -> String {
        format!("{}/{}", self.n.to_dec(), self.d.to_dec())
    }
    /// Within the AT1_CASE_V1 section 2 token limits.
    pub fn within_limits(&self) -> bool {
        let lim = Int::from_u64(1_048_576);
        self.n.abs().cmp(&lim) != Ordering::Greater && self.d.cmp(&lim) != Ordering::Greater
    }
}

/// Gaussian rational.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Cq {
    pub re: Q,
    pub im: Q,
}

impl Cq {
    pub fn new(re: Q, im: Q) -> Cq {
        Cq { re, im }
    }
    pub fn zero() -> Cq {
        Cq::new(Q::zero(), Q::zero())
    }
    pub fn real(q: Q) -> Cq {
        Cq::new(q, Q::zero())
    }
    pub fn is_zero(&self) -> bool {
        self.re.is_zero() && self.im.is_zero()
    }
    pub fn add(&self, o: &Cq) -> Cq {
        Cq::new(self.re.add(&o.re), self.im.add(&o.im))
    }
    pub fn sub(&self, o: &Cq) -> Cq {
        Cq::new(self.re.sub(&o.re), self.im.sub(&o.im))
    }
    pub fn mul(&self, o: &Cq) -> Cq {
        Cq::new(
            self.re.mul(&o.re).sub(&self.im.mul(&o.im)),
            self.re.mul(&o.im).add(&self.im.mul(&o.re)),
        )
    }
    pub fn conj(&self) -> Cq {
        Cq::new(self.re.clone(), self.im.neg())
    }
    pub fn norm2(&self) -> Q {
        self.re.mul(&self.re).add(&self.im.mul(&self.im))
    }
    pub fn scale(&self, q: &Q) -> Cq {
        Cq::new(self.re.mul(q), self.im.mul(q))
    }
}

/// Scaled decimal N@k.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Scaled {
    pub n: Int,
    pub k: u32,
}

/// Outcome of parsing a token against its grammar.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tok {
    Ok,
    /// parsed, but not the canonical spelling (CASE_NONCANONICAL after the shape pass)
    NonCanonical,
}

/// Integer text: optional '-', digits. Returns the value and canonicality; None if not an integer at all.
pub fn parse_int(s: &str) -> Option<(Int, Tok)> {
    let (neg, digits) = match s.strip_prefix('-') {
        Some(rest) => (true, rest),
        None => (false, s),
    };
    let v = Int::from_dec_digits(digits)?;
    let mut t = Tok::Ok;
    if digits.len() > 1 && digits.starts_with('0') {
        t = Tok::NonCanonical;
    }
    if neg && v.is_zero() {
        t = Tok::NonCanonical;
    }
    Some((if neg { v.neg() } else { v }, t))
}

/// Rational n/d with d >= 1 (d < 1 is shape). Value is reduced; Tok says if the spelling was canonical.
pub fn parse_rat(s: &str) -> Option<(Q, Tok)> {
    let (a, b) = s.split_once('/')?;
    let (n, t1) = parse_int(a)?;
    if b.starts_with('-') {
        return None;
    }
    let (d, t2) = parse_int(b)?;
    if d.signum() < 1 {
        return None;
    }
    let mut t = if t1 == Tok::Ok && t2 == Tok::Ok { Tok::Ok } else { Tok::NonCanonical };
    let g = Int::gcd(&n, &d);
    if n.is_zero() {
        if d != Int::from_u64(1) {
            t = Tok::NonCanonical;
        }
    } else if g != Int::from_u64(1) {
        t = Tok::NonCanonical;
    }
    Some((Q::new(n, d), t))
}

/// Complex rational (re;im).
pub fn parse_crat(s: &str) -> Option<(Cq, Tok)> {
    let inner = s.strip_prefix('(')?.strip_suffix(')')?;
    let (a, b) = inner.split_once(';')?;
    let (re, t1) = parse_rat(a)?;
    let (im, t2) = parse_rat(b)?;
    let t = if t1 == Tok::Ok && t2 == Tok::Ok { Tok::Ok } else { Tok::NonCanonical };
    Some((Cq::new(re, im), t))
}

/// Scaled decimal N@k, 0 <= k <= 40.
pub fn parse_scaled(s: &str) -> Option<(Scaled, Tok)> {
    let (a, b) = s.split_once('@')?;
    let n = Int::from_dec_digits(a)?;
    let kv = Int::from_dec_digits(b)?;
    if b.len() > 2 {
        return None;
    }
    let k = kv.low_u64() as u32;
    if k > 40 {
        return None;
    }
    let mut t = Tok::Ok;
    if a.len() > 1 && a.starts_with('0') {
        t = Tok::NonCanonical;
    }
    if b.len() > 1 && b.starts_with('0') {
        t = Tok::NonCanonical;
    }
    if n.is_zero() && k != 0 {
        t = Tok::NonCanonical;
    }
    if !n.is_zero() && k != 0 && n.divrem(&Int::from_u64(10)).1.is_zero() {
        t = Tok::NonCanonical;
    }
    Some((Scaled { n, k }, t))
}

pub fn scaled_text(s: &Scaled) -> String {
    format!("{}@{}", s.n.to_dec(), s.k)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn grammars() {
        assert_eq!(parse_rat("2/4").unwrap().1, Tok::NonCanonical);
        assert_eq!(parse_rat("-0/1").unwrap().1, Tok::NonCanonical);
        assert_eq!(parse_rat("0/2").unwrap().1, Tok::NonCanonical);
        assert_eq!(parse_rat("3/10").unwrap(), (Q::frac(3, 10), Tok::Ok));
        assert!(parse_rat("1/0").is_none());
        assert!(parse_rat("1/-2").is_none());
        assert!(parse_rat("0.25").is_none());
        assert!(parse_rat("+1/2").is_none());
        assert_eq!(parse_int("04").unwrap().1, Tok::NonCanonical);
        assert_eq!(parse_scaled("10@1").unwrap().1, Tok::NonCanonical);
        assert_eq!(parse_scaled("0@3").unwrap().1, Tok::NonCanonical);
        assert_eq!(parse_scaled("1@12").unwrap().1, Tok::Ok);
        assert!(parse_scaled("1@41").is_none());
        assert_eq!(parse_crat("(1/1;0/1)").unwrap().1, Tok::Ok);
        assert!(parse_crat("(1/1,0/1)").is_none());
    }
    #[test]
    fn f64_exact() {
        assert_eq!(Q::from_f64_bits(0.25f64.to_bits()), Q::frac(1, 4));
        assert_eq!(Q::from_f64_bits((-3.0f64).to_bits()), Q::int(-3));
        assert_eq!(Q::from_f64_bits(1), Q::new(Int::from_u64(1), Int::pow2(1074)));
        assert_eq!(Q::frac(41, 100).sqrt_exact(), None);
        assert_eq!(Q::frac(1, 4).sqrt_exact(), Some(Q::frac(1, 2)));
        assert_eq!(Q::frac(-7, 4).mod1(), Q::frac(1, 4));
    }
}
