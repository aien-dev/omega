//! Arbitrary-precision integers and rationals, standard library only.
//!
//! AT1_CASE_V1 section 2: rules 3 and 4, the kernel test and the zero test are decided
//! exactly for every case within the token limits ("the sum of three squared rationals can
//! carry a denominator near 2^240"), with no implementation-defined refusal. Nothing here
//! has a fixed width: magnitudes are little-endian u32 limb vectors.
//!
//! Division is binary shift-and-subtract and the gcd is Stein's binary algorithm, chosen
//! for being short and easy to audit rather than fast; every number this oracle meets is a
//! few thousand bits at most.

use std::cmp::Ordering;

/// Unsigned magnitude, little-endian limbs, no trailing zero limb (zero is empty).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Nat(Vec<u32>);

impl Nat {
    pub fn zero() -> Nat {
        Nat(Vec::new())
    }
    pub fn one() -> Nat {
        Nat(vec![1])
    }
    pub fn from_u64(mut v: u64) -> Nat {
        let mut l = Vec::new();
        while v != 0 {
            l.push(v as u32);
            v >>= 32;
        }
        Nat(l)
    }
    fn trim(mut self) -> Nat {
        while self.0.last() == Some(&0) {
            self.0.pop();
        }
        self
    }
    pub fn is_zero(&self) -> bool {
        self.0.is_empty()
    }
    pub fn is_even(&self) -> bool {
        self.0.first().map(|l| l & 1 == 0).unwrap_or(true)
    }
    /// Decimal digit string (only ASCII digits; caller checked).
    pub fn from_decimal(s: &str) -> Nat {
        let mut n = Nat::zero();
        for b in s.bytes() {
            n = n.mul_small(10).add(&Nat::from_u64((b - b'0') as u64));
        }
        n
    }
    pub fn to_decimal(&self) -> String {
        if self.is_zero() {
            return "0".to_string();
        }
        let mut digits = Vec::new();
        let mut cur = self.clone();
        while !cur.is_zero() {
            let (q, r) = cur.divrem_small(1_000_000_000);
            digits.push(r);
            cur = q;
        }
        let mut s = format!("{}", digits.pop().unwrap());
        while let Some(d) = digits.pop() {
            s.push_str(&format!("{:09}", d));
        }
        s
    }
    /// Value if it fits in u64.
    pub fn to_u64(&self) -> Option<u64> {
        match self.0.len() {
            0 => Some(0),
            1 => Some(self.0[0] as u64),
            2 => Some(self.0[0] as u64 | (self.0[1] as u64) << 32),
            _ => None,
        }
    }
    pub fn bits(&self) -> u64 {
        match self.0.last() {
            None => 0,
            Some(&top) => (self.0.len() as u64 - 1) * 32 + (32 - top.leading_zeros() as u64),
        }
    }
    pub fn bit(&self, i: u64) -> bool {
        let (w, b) = ((i / 32) as usize, i % 32);
        self.0.get(w).map(|l| (l >> b) & 1 == 1).unwrap_or(false)
    }
    pub fn add(&self, o: &Nat) -> Nat {
        let n = self.0.len().max(o.0.len());
        let mut out = Vec::with_capacity(n + 1);
        let mut carry = 0u64;
        for i in 0..n {
            let s = *self.0.get(i).unwrap_or(&0) as u64 + *o.0.get(i).unwrap_or(&0) as u64 + carry;
            out.push(s as u32);
            carry = s >> 32;
        }
        if carry != 0 {
            out.push(carry as u32);
        }
        Nat(out).trim()
    }
    /// self - o; panics if o > self (a logic error, never input-driven).
    pub fn sub(&self, o: &Nat) -> Nat {
        assert!(self.cmp(o) != Ordering::Less, "Nat::sub underflow");
        let mut out = Vec::with_capacity(self.0.len());
        let mut borrow = 0i64;
        for i in 0..self.0.len() {
            let mut d = self.0[i] as i64 - *o.0.get(i).unwrap_or(&0) as i64 - borrow;
            if d < 0 {
                d += 1 << 32;
                borrow = 1;
            } else {
                borrow = 0;
            }
            out.push(d as u32);
        }
        Nat(out).trim()
    }
    pub fn mul_small(&self, m: u32) -> Nat {
        let mut out = Vec::with_capacity(self.0.len() + 1);
        let mut carry = 0u64;
        for &l in &self.0 {
            let p = l as u64 * m as u64 + carry;
            out.push(p as u32);
            carry = p >> 32;
        }
        if carry != 0 {
            out.push(carry as u32);
        }
        Nat(out).trim()
    }
    pub fn mul(&self, o: &Nat) -> Nat {
        if self.is_zero() || o.is_zero() {
            return Nat::zero();
        }
        let mut out = vec![0u32; self.0.len() + o.0.len()];
        for (i, &a) in self.0.iter().enumerate() {
            let mut carry = 0u64;
            for (j, &b) in o.0.iter().enumerate() {
                let t = out[i + j] as u64 + a as u64 * b as u64 + carry;
                out[i + j] = t as u32;
                carry = t >> 32;
            }
            let mut k = i + o.0.len();
            while carry != 0 {
                let t = out[k] as u64 + carry;
                out[k] = t as u32;
                carry = t >> 32;
                k += 1;
            }
        }
        Nat(out).trim()
    }
    pub fn shl(&self, bits: u64) -> Nat {
        if self.is_zero() {
            return Nat::zero();
        }
        let (words, rem) = ((bits / 32) as usize, (bits % 32) as u32);
        let mut out = vec![0u32; words];
        let mut carry = 0u32;
        for &l in &self.0 {
            if rem == 0 {
                out.push(l);
            } else {
                out.push((l << rem) | carry);
                carry = l >> (32 - rem);
            }
        }
        if carry != 0 {
            out.push(carry);
        }
        Nat(out).trim()
    }
    pub fn shr(&self, bits: u64) -> Nat {
        let (words, rem) = ((bits / 32) as usize, (bits % 32) as u32);
        if words >= self.0.len() {
            return Nat::zero();
        }
        let src = &self.0[words..];
        let mut out = Vec::with_capacity(src.len());
        for i in 0..src.len() {
            let lo = src[i] >> rem;
            let hi = if rem == 0 { 0 } else { src.get(i + 1).map(|h| h << (32 - rem)).unwrap_or(0) };
            out.push(lo | hi);
        }
        Nat(out).trim()
    }
    fn divrem_small(&self, d: u32) -> (Nat, u32) {
        let mut out = vec![0u32; self.0.len()];
        let mut rem = 0u64;
        for i in (0..self.0.len()).rev() {
            let cur = (rem << 32) | self.0[i] as u64;
            out[i] = (cur / d as u64) as u32;
            rem = cur % d as u64;
        }
        (Nat(out).trim(), rem as u32)
    }
    /// (floor(self / d), self mod d); d must be nonzero.
    pub fn divrem(&self, d: &Nat) -> (Nat, Nat) {
        assert!(!d.is_zero(), "division by zero");
        if self.cmp(d) == Ordering::Less {
            return (Nat::zero(), self.clone());
        }
        if d.0.len() == 1 {
            let (q, r) = self.divrem_small(d.0[0]);
            return (q, Nat::from_u64(r as u64));
        }
        let nb = self.bits();
        let mut q = vec![0u32; self.0.len()];
        let mut r = Nat::zero();
        for i in (0..nb).rev() {
            r = r.shl(1);
            if self.bit(i) {
                r = r.add(&Nat::one());
            }
            if r.cmp(d) != Ordering::Less {
                r = r.sub(d);
                q[(i / 32) as usize] |= 1 << (i % 32);
            }
        }
        (Nat(q).trim(), r)
    }
    /// Stein's binary gcd; gcd(0, x) = x.
    pub fn gcd(&self, o: &Nat) -> Nat {
        let (mut a, mut b) = (self.clone(), o.clone());
        if a.is_zero() {
            return b;
        }
        if b.is_zero() {
            return a;
        }
        let mut shift = 0u64;
        while a.is_even() && b.is_even() {
            a = a.shr(1);
            b = b.shr(1);
            shift += 1;
        }
        while a.is_even() {
            a = a.shr(1);
        }
        loop {
            while b.is_even() {
                b = b.shr(1);
            }
            if a.cmp(&b) == Ordering::Greater {
                std::mem::swap(&mut a, &mut b);
            }
            b = b.sub(&a);
            if b.is_zero() {
                break;
            }
        }
        a.shl(shift)
    }
    /// floor(sqrt(self)), digit-by-digit in base 4 (shifts, adds, compares only).
    pub fn isqrt(&self) -> Nat {
        if self.is_zero() {
            return Nat::zero();
        }
        let mut rem = self.clone();
        let mut res = Nat::zero();
        let mut bit_pos = (self.bits() - 1) & !1u64; // highest even bit position
        loop {
            let bit = Nat::one().shl(bit_pos);
            let trial = res.add(&bit);
            if rem.cmp(&trial) != Ordering::Less {
                rem = rem.sub(&trial);
                res = res.shr(1).add(&bit);
            } else {
                res = res.shr(1);
            }
            if bit_pos < 2 {
                break;
            }
            bit_pos -= 2;
        }
        res
    }
}

impl Ord for Nat {
    fn cmp(&self, o: &Nat) -> Ordering {
        if self.0.len() != o.0.len() {
            return self.0.len().cmp(&o.0.len());
        }
        for i in (0..self.0.len()).rev() {
            if self.0[i] != o.0[i] {
                return self.0[i].cmp(&o.0[i]);
            }
        }
        Ordering::Equal
    }
}
impl PartialOrd for Nat {
    fn partial_cmp(&self, o: &Nat) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

/// Signed integer: sign flag plus magnitude; zero is never negative.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Int {
    pub neg: bool,
    pub mag: Nat,
}

impl Int {
    pub fn zero() -> Int {
        Int { neg: false, mag: Nat::zero() }
    }
    pub fn from_i64(v: i64) -> Int {
        Int { neg: v < 0, mag: Nat::from_u64(v.unsigned_abs()) }
    }
    pub fn from_nat(neg: bool, mag: Nat) -> Int {
        let neg = neg && !mag.is_zero();
        Int { neg, mag }
    }
    pub fn is_zero(&self) -> bool {
        self.mag.is_zero()
    }
    pub fn neg(&self) -> Int {
        Int::from_nat(!self.neg, self.mag.clone())
    }
    pub fn add(&self, o: &Int) -> Int {
        if self.neg == o.neg {
            return Int::from_nat(self.neg, self.mag.add(&o.mag));
        }
        match self.mag.cmp(&o.mag) {
            Ordering::Equal => Int::zero(),
            Ordering::Greater => Int::from_nat(self.neg, self.mag.sub(&o.mag)),
            Ordering::Less => Int::from_nat(o.neg, o.mag.sub(&self.mag)),
        }
    }
    pub fn sub(&self, o: &Int) -> Int {
        self.add(&o.neg())
    }
    pub fn mul(&self, o: &Int) -> Int {
        Int::from_nat(self.neg != o.neg, self.mag.mul(&o.mag))
    }
    pub fn mul_nat(&self, o: &Nat) -> Int {
        Int::from_nat(self.neg, self.mag.mul(o))
    }
    pub fn to_decimal(&self) -> String {
        format!("{}{}", if self.neg { "-" } else { "" }, self.mag.to_decimal())
    }
}

impl Ord for Int {
    fn cmp(&self, o: &Int) -> Ordering {
        match (self.neg, o.neg) {
            (false, true) => Ordering::Greater,
            (true, false) => Ordering::Less,
            (false, false) => self.mag.cmp(&o.mag),
            (true, true) => o.mag.cmp(&self.mag),
        }
    }
}
impl PartialOrd for Int {
    fn partial_cmp(&self, o: &Int) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

/// Exact rational num/den with den > 0. `Q::new` reduces; the arithmetic helpers reduce
/// too (the judge's exact comparisons use `cmp`, which needs no reduction).
#[derive(Clone, Debug)]
pub struct Q {
    pub num: Int,
    pub den: Nat,
}

impl Q {
    pub fn new(num: Int, den: Nat) -> Q {
        assert!(!den.is_zero(), "zero denominator");
        if num.is_zero() {
            return Q { num: Int::zero(), den: Nat::one() };
        }
        let g = num.mag.gcd(&den);
        if g == Nat::one() {
            return Q { num, den };
        }
        Q { num: Int::from_nat(num.neg, num.mag.divrem(&g).0), den: den.divrem(&g).0 }
    }
    /// Unreduced constructor (exact value, no gcd).
    pub fn raw(num: Int, den: Nat) -> Q {
        assert!(!den.is_zero(), "zero denominator");
        Q { num, den }
    }
    pub fn zero() -> Q {
        Q { num: Int::zero(), den: Nat::one() }
    }
    pub fn one() -> Q {
        Q::int(1)
    }
    pub fn int(v: i64) -> Q {
        Q { num: Int::from_i64(v), den: Nat::one() }
    }
    pub fn frac(n: i64, d: i64) -> Q {
        assert!(d != 0);
        let (n, d) = if d < 0 { (-n, -d) } else { (n, d) };
        Q::new(Int::from_i64(n), Nat::from_u64(d as u64))
    }
    pub fn is_zero(&self) -> bool {
        self.num.is_zero()
    }
    pub fn is_neg(&self) -> bool {
        self.num.neg
    }
    pub fn neg(&self) -> Q {
        Q { num: self.num.neg(), den: self.den.clone() }
    }
    pub fn abs(&self) -> Q {
        Q { num: Int::from_nat(false, self.num.mag.clone()), den: self.den.clone() }
    }
    pub fn add(&self, o: &Q) -> Q {
        Q::new(self.num.mul_nat(&o.den).add(&o.num.mul_nat(&self.den)), self.den.mul(&o.den))
    }
    pub fn sub(&self, o: &Q) -> Q {
        self.add(&o.neg())
    }
    pub fn mul(&self, o: &Q) -> Q {
        Q::new(self.num.mul(&o.num), self.den.mul(&o.den))
    }
    /// self / o; o nonzero.
    pub fn div(&self, o: &Q) -> Q {
        assert!(!o.is_zero(), "division by zero rational");
        Q::new(Int::from_nat(self.num.neg != o.num.neg, self.num.mag.mul(&o.den)), self.den.mul(&o.num.mag))
    }
    /// Unreduced add, for the judge (exact, cheaper on 2^-1074 denominators).
    pub fn add_raw(&self, o: &Q) -> Q {
        if self.den == o.den {
            return Q { num: self.num.add(&o.num), den: self.den.clone() };
        }
        Q { num: self.num.mul_nat(&o.den).add(&o.num.mul_nat(&self.den)), den: self.den.mul(&o.den) }
    }
    pub fn sub_raw(&self, o: &Q) -> Q {
        self.add_raw(&o.neg())
    }
    /// Exact square root of a nonnegative rational, if it is the square of a rational.
    pub fn sqrt_exact(&self) -> Option<Q> {
        if self.num.neg {
            return None;
        }
        let r = Q::new(self.num.clone(), self.den.clone());
        let (a, b) = (r.num.mag.isqrt(), r.den.isqrt());
        if a.mul(&a) == r.num.mag && b.mul(&b) == r.den {
            Some(Q::new(Int::from_nat(false, a), b))
        } else {
            None
        }
    }
    /// Fractional part in [0, 1): self - floor(self).
    pub fn frac_part(&self) -> Q {
        let r = Q::new(self.num.clone(), self.den.clone());
        let (_, rem) = r.num.mag.divrem(&r.den);
        if rem.is_zero() {
            return Q::zero();
        }
        if r.num.neg {
            Q::new(Int::from_nat(false, r.den.sub(&rem)), r.den.clone())
        } else {
            Q::new(Int::from_nat(false, rem), r.den.clone())
        }
    }
    /// floor(self + 1/2) as i64 (callers use it on values of order one).
    pub fn round_half_up_i64(&self) -> i64 {
        let twice = Q::raw(self.num.mul_nat(&Nat::from_u64(2)).add(&Int::from_nat(false, self.den.clone())), self.den.mul_small(2));
        // floor of twice.num / twice.den
        let (q, r) = twice.num.mag.divrem(&twice.den);
        let q = q.to_u64().expect("round of a small rational") as i64;
        if twice.num.neg {
            if r.is_zero() {
                -q
            } else {
                -q - 1
            }
        } else {
            q
        }
    }
    /// Exact value of a finite binary64.
    pub fn from_f64(x: f64) -> Option<Q> {
        if !x.is_finite() {
            return None;
        }
        let bits = x.to_bits();
        let neg = bits >> 63 == 1;
        let exp = ((bits >> 52) & 0x7ff) as i64;
        let frac = bits & ((1u64 << 52) - 1);
        let (m, e) = if exp == 0 { (frac, -1074i64) } else { (frac | (1u64 << 52), exp - 1075) };
        if m == 0 {
            return Some(Q::zero());
        }
        let mag = Nat::from_u64(m);
        Some(if e >= 0 { Q::raw(Int::from_nat(neg, mag.shl(e as u64)), Nat::one()) } else { Q::raw(Int::from_nat(neg, mag), Nat::one().shl((-e) as u64)) })
    }
    /// n / 10^k.
    pub fn from_scaled(n: &Nat, k: u32) -> Q {
        let mut d = Nat::one();
        for _ in 0..k {
            d = d.mul_small(10);
        }
        Q::raw(Int::from_nat(false, n.clone()), d)
    }
    /// Correctly rounded (round half to even) binary64 value. Values met here are far from
    /// the overflow and subnormal ranges; both are still handled.
    pub fn to_f64(&self) -> f64 {
        if self.num.is_zero() {
            return 0.0;
        }
        let (a, b) = (&self.num.mag, &self.den);
        // choose s so that q = floor(a * 2^s / b) has 54 or 55 bits
        let s: i64 = 55 - (a.bits() as i64 - b.bits() as i64);
        let (num, den) = if s >= 0 { (a.shl(s as u64), b.clone()) } else { (a.clone(), b.shl((-s) as u64)) };
        let (q, r) = num.divrem(&den);
        let mut q = q.to_u64().expect("55-bit quotient");
        let mut e = -s; // value = (q + r/den) * 2^e
        let sticky = !r.is_zero();
        // normalise to exactly 53 significant bits plus guard bit and sticky
        let qb = 64 - q.leading_zeros() as i64;
        let mut drop = qb - 53;
        // subnormal handling: the lowest representable exponent for the integer significand is -1074
        if e + drop < -1074 {
            drop = -1074 - e;
        }
        let result_sig: u64;
        if drop > 0 {
            if drop >= 64 {
                return if self.num.neg { -0.0 } else { 0.0 };
            }
            let mask = (1u64 << drop) - 1;
            let low = q & mask;
            let half = 1u64 << (drop - 1);
            q >>= drop;
            e += drop;
            let round_up = low > half || (low == half && (sticky || q & 1 == 1));
            result_sig = q + if round_up { 1 } else { 0 };
        } else {
            // q already exact to 53 bits or fewer; the remainder only matters below one ulp,
            // which cannot happen because q has at least 54 bits (drop >= 1) for normal results
            result_sig = q;
        }
        // result_sig * 2^e, result_sig < 2^54 (rounding may carry to 2^53)
        let v = (result_sig as f64) * pow2(e);
        if self.num.neg {
            -v
        } else {
            v
        }
    }
}

/// 2^e as binary64 for e in the normal range, by repeated scaling (exact).
fn pow2(e: i64) -> f64 {
    let mut v = 1.0f64;
    let mut e = e;
    while e > 0 {
        let step = e.min(1000);
        v *= f64::from_bits(((step + 1023) as u64) << 52);
        e -= step;
    }
    while e < 0 {
        let step = (-e).min(1000);
        v *= f64::from_bits(((1023 - step) as u64) << 52);
        e += step;
    }
    v
}

/// Equality by value (consistent with `Ord`): an unreduced `Q::raw` equals its reduced form.
impl PartialEq for Q {
    fn eq(&self, o: &Q) -> bool {
        self.cmp(o) == Ordering::Equal
    }
}
impl Eq for Q {}

impl Ord for Q {
    fn cmp(&self, o: &Q) -> Ordering {
        self.num.mul_nat(&o.den).cmp(&o.num.mul_nat(&self.den))
    }
}
impl PartialOrd for Q {
    fn partial_cmp(&self, o: &Q) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

/// Gaussian rational re + i im.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GQ {
    pub re: Q,
    pub im: Q,
}

impl GQ {
    pub fn new(re: Q, im: Q) -> GQ {
        GQ { re, im }
    }
    pub fn zero() -> GQ {
        GQ { re: Q::zero(), im: Q::zero() }
    }
    pub fn is_zero(&self) -> bool {
        self.re.is_zero() && self.im.is_zero()
    }
    pub fn conj(&self) -> GQ {
        GQ { re: self.re.clone(), im: self.im.neg() }
    }
    pub fn add(&self, o: &GQ) -> GQ {
        GQ { re: self.re.add(&o.re), im: self.im.add(&o.im) }
    }
    pub fn sub(&self, o: &GQ) -> GQ {
        GQ { re: self.re.sub(&o.re), im: self.im.sub(&o.im) }
    }
    pub fn mul(&self, o: &GQ) -> GQ {
        GQ { re: self.re.mul(&o.re).sub(&self.im.mul(&o.im)), im: self.re.mul(&o.im).add(&self.im.mul(&o.re)) }
    }
    pub fn scale(&self, s: &Q) -> GQ {
        GQ { re: self.re.mul(s), im: self.im.mul(s) }
    }
    pub fn norm2(&self) -> Q {
        self.re.mul(&self.re).add(&self.im.mul(&self.im))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nat_int_q_basics() {
        let a = Nat::from_decimal("340282366920938463463374607431768211457");
        assert_eq!(a.to_decimal(), "340282366920938463463374607431768211457");
        let b = Nat::from_decimal("18446744073709551629");
        let (q, r) = a.divrem(&b);
        assert_eq!(q.mul(&b).add(&r), a);
        assert!(r < b);
        assert_eq!(Nat::from_u64(48).gcd(&Nat::from_u64(180)), Nat::from_u64(12));
        assert_eq!(Nat::from_u64(1 << 40).isqrt(), Nat::from_u64(1 << 20));
        assert_eq!(Nat::from_u64(99).isqrt(), Nat::from_u64(9));
        let big = Nat::from_decimal("274763624041").mul(&Nat::from_decimal("274763624041"));
        assert_eq!(big.isqrt(), Nat::from_decimal("274763624041"));
        assert_eq!(Q::frac(2, 4), Q::frac(1, 2));
        assert_eq!(Q::frac(9, 100).sqrt_exact(), Some(Q::frac(3, 10)));
        assert_eq!(Q::frac(41, 100).sqrt_exact(), None);
        assert!(Q::frac(-3, 2) < Q::frac(-1, 2));
        assert_eq!(Q::frac(-7, 4).frac_part(), Q::frac(1, 4));
        assert_eq!(Q::frac(7, 4).frac_part(), Q::frac(3, 4));
        assert_eq!(Q::frac(3, 8).round_half_up_i64(), 0);
        assert_eq!(Q::frac(1, 2).round_half_up_i64(), 1);
        assert_eq!(Q::frac(-1, 2).round_half_up_i64(), 0);
        assert_eq!(Q::frac(-5, 8).round_half_up_i64(), -1);
    }

    #[test]
    fn to_f64_is_correctly_rounded() {
        for (n, d) in [(1i64, 3i64), (2, 3), (1, 10), (-7, 9), (524181, 2096716), (274763624041, 549527248074), (1, 1 << 40), (5, 28)] {
            assert_eq!(Q::frac(n, d).to_f64(), n as f64 / d as f64, "{}/{}", n, d);
        }
        for x in [0.1f64, 1.0, -2.5, 1e-300, 5e-324, 1.7976931348623157e308, 0.30000000000000004] {
            assert_eq!(Q::from_f64(x).unwrap().to_f64(), x);
        }
        // halfway case rounds to even: (2^53 + 1) / 1 -> 2^53
        let h = Q::raw(Int::from_nat(false, Nat::from_u64((1u64 << 53) + 1)), Nat::one());
        assert_eq!(h.to_f64(), 9007199254740992.0);
        let h3 = Q::raw(Int::from_nat(false, Nat::from_u64((1u64 << 53) + 3)), Nat::one());
        assert_eq!(h3.to_f64(), 9007199254740996.0);
    }
}
