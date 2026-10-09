//! Exact decisions on written tokens (AT0_RESULT_V1 section 4) without floats.
//!
//! Every binary64 value is m * 2^e with e >= -1074, every scaled decimal is
//! n / 10^k with k <= 40. Both are exact integer multiples of the unit
//! U = 2^-1074 * 10^-40, so each becomes a signed big integer count of U and
//! every comparison, sum and difference is exact integer arithmetic.

use std::cmp::Ordering;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Big(Vec<u32>); // little-endian limbs, no trailing zero limbs

impl Big {
    pub fn zero() -> Big {
        Big(Vec::new())
    }
    pub fn from_u128(mut v: u128) -> Big {
        let mut limbs = Vec::new();
        while v != 0 {
            limbs.push(v as u32);
            v >>= 32;
        }
        Big(limbs)
    }
    fn trim(mut self) -> Big {
        while self.0.last() == Some(&0) {
            self.0.pop();
        }
        self
    }
    pub fn is_zero(&self) -> bool {
        self.0.is_empty()
    }
    pub fn shl(&self, bits: u32) -> Big {
        if self.is_zero() {
            return Big::zero();
        }
        let (words, rem) = ((bits / 32) as usize, bits % 32);
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
        Big(out).trim()
    }
    pub fn mul_small(&self, m: u32) -> Big {
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
        Big(out).trim()
    }
    pub fn mul_pow10(&self, k: u32) -> Big {
        let mut out = self.clone();
        for _ in 0..k {
            out = out.mul_small(10);
        }
        out
    }
    pub fn add(&self, o: &Big) -> Big {
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
        Big(out).trim()
    }
    /// self - o, requires self >= o.
    pub fn sub(&self, o: &Big) -> Big {
        debug_assert!(self.cmp(o) != Ordering::Less);
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
        Big(out).trim()
    }
}

impl Ord for Big {
    fn cmp(&self, o: &Big) -> Ordering {
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

impl PartialOrd for Big {
    fn partial_cmp(&self, o: &Big) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

/// A signed exact number as a count of U = 2^-1074 * 10^-40.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Exact {
    neg: bool,
    mag: Big,
}

const BIN_SHIFT: u32 = 1074;
const DEC_PLACES: u32 = 40;

impl Exact {
    pub fn zero() -> Exact {
        Exact { neg: false, mag: Big::zero() }
    }
    /// Exact value of a finite double. None for NaN or infinity.
    pub fn from_f64(x: f64) -> Option<Exact> {
        if !x.is_finite() {
            return None;
        }
        let bits = x.to_bits();
        let neg = bits >> 63 == 1;
        let exp = ((bits >> 52) & 0x7ff) as i32;
        let frac = bits & ((1u64 << 52) - 1);
        let (m, e) = if exp == 0 { (frac, -1074) } else { (frac | (1u64 << 52), exp - 1075) };
        if m == 0 {
            return Some(Exact::zero());
        }
        let mag = Big::from_u128(m as u128).shl((e + BIN_SHIFT as i32) as u32).mul_pow10(DEC_PLACES);
        Some(Exact { neg, mag })
    }
    /// n / 10^k, k <= 40.
    pub fn from_scaled(n: u128, k: u32) -> Exact {
        assert!(k <= DEC_PLACES);
        Exact { neg: false, mag: Big::from_u128(n).mul_pow10(DEC_PLACES - k).shl(BIN_SHIFT) }
    }
    pub fn from_int(i: i64) -> Exact {
        Exact { neg: i < 0, mag: Big::from_u128(i.unsigned_abs() as u128).mul_pow10(DEC_PLACES).shl(BIN_SHIFT) }
    }
    pub fn abs(&self) -> Exact {
        Exact { neg: false, mag: self.mag.clone() }
    }
    pub fn neg(&self) -> Exact {
        Exact { neg: !self.neg && !self.mag.is_zero(), mag: self.mag.clone() }
    }
    pub fn add(&self, o: &Exact) -> Exact {
        if self.neg == o.neg {
            return Exact { neg: self.neg, mag: self.mag.add(&o.mag) };
        }
        match self.mag.cmp(&o.mag) {
            Ordering::Equal => Exact::zero(),
            Ordering::Greater => Exact { neg: self.neg, mag: self.mag.sub(&o.mag) },
            Ordering::Less => Exact { neg: o.neg, mag: o.mag.sub(&self.mag) },
        }
    }
    pub fn sub(&self, o: &Exact) -> Exact {
        self.add(&o.neg())
    }
    pub fn le(&self, o: &Exact) -> bool {
        self.cmp(o) != Ordering::Greater
    }
    pub fn gt(&self, o: &Exact) -> bool {
        self.cmp(o) == Ordering::Greater
    }
}

impl Ord for Exact {
    fn cmp(&self, o: &Exact) -> Ordering {
        match (self.neg, o.neg) {
            (false, true) => Ordering::Greater,
            (true, false) => Ordering::Less,
            (false, false) => self.mag.cmp(&o.mag),
            (true, true) => o.mag.cmp(&self.mag),
        }
    }
}

impl PartialOrd for Exact {
    fn partial_cmp(&self, o: &Exact) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn doubles_and_decimals_meet_exactly() {
        assert_eq!(Exact::from_f64(0.5).unwrap(), Exact::from_scaled(5, 1));
        assert_eq!(Exact::from_f64(0.25).unwrap(), Exact::from_scaled(25, 2));
        assert_eq!(Exact::from_f64(1.0).unwrap(), Exact::from_int(1));
        assert_eq!(Exact::from_f64(-0.0).unwrap(), Exact::zero());
        assert!(Exact::from_f64(0.1).unwrap().gt(&Exact::from_scaled(1, 1))); // 0.1 double is above 1/10
        assert!(Exact::from_f64(f64::NAN).is_none());
        let tiny = Exact::from_f64(5e-324).unwrap();
        assert!(tiny.gt(&Exact::zero()));
        assert!(tiny.le(&Exact::from_scaled(1, 40)));
        let a = Exact::from_f64(0.75).unwrap().sub(&Exact::from_int(1));
        assert_eq!(a.abs(), Exact::from_scaled(25, 2));
        assert_eq!(Exact::from_scaled(1, 12).add(&Exact::from_scaled(1, 12)), Exact::from_scaled(2, 12));
    }
}
