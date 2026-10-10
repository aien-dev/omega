//! Arbitrary-precision signed integers (std only). Little-endian u32 limbs,
//! normalised (no high zero limbs; zero is an empty vector with neg = false).
//! Division is Knuth algorithm D (Hacker's Delight divmnu form).

use std::cmp::Ordering;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Int {
    neg: bool,
    mag: Vec<u32>,
}

fn trim(v: &mut Vec<u32>) {
    while let Some(&0) = v.last() {
        v.pop();
    }
}

fn mag_cmp(a: &[u32], b: &[u32]) -> Ordering {
    if a.len() != b.len() {
        return a.len().cmp(&b.len());
    }
    for i in (0..a.len()).rev() {
        if a[i] != b[i] {
            return a[i].cmp(&b[i]);
        }
    }
    Ordering::Equal
}

fn mag_add(a: &[u32], b: &[u32]) -> Vec<u32> {
    let (a, b) = if a.len() >= b.len() { (a, b) } else { (b, a) };
    let mut r = Vec::with_capacity(a.len() + 1);
    let mut carry = 0u64;
    for i in 0..a.len() {
        let s = a[i] as u64 + if i < b.len() { b[i] as u64 } else { 0 } + carry;
        r.push(s as u32);
        carry = s >> 32;
    }
    if carry != 0 {
        r.push(carry as u32);
    }
    r
}

/// a - b, requires a >= b.
fn mag_sub(a: &[u32], b: &[u32]) -> Vec<u32> {
    let mut r = Vec::with_capacity(a.len());
    let mut borrow = 0i64;
    for i in 0..a.len() {
        let mut d = a[i] as i64 - borrow - if i < b.len() { b[i] as i64 } else { 0 };
        if d < 0 {
            d += 1 << 32;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r.push(d as u32);
    }
    assert!(borrow == 0, "mag_sub underflow");
    trim(&mut r);
    r
}

fn mag_mul(a: &[u32], b: &[u32]) -> Vec<u32> {
    if a.is_empty() || b.is_empty() {
        return Vec::new();
    }
    let mut r = vec![0u32; a.len() + b.len()];
    for i in 0..a.len() {
        let mut carry = 0u64;
        let ai = a[i] as u64;
        if ai == 0 {
            continue;
        }
        for j in 0..b.len() {
            let t = ai * b[j] as u64 + r[i + j] as u64 + carry;
            r[i + j] = t as u32;
            carry = t >> 32;
        }
        let mut k = i + b.len();
        while carry != 0 {
            let t = r[k] as u64 + carry;
            r[k] = t as u32;
            carry = t >> 32;
            k += 1;
        }
    }
    trim(&mut r);
    r
}

fn mag_divrem_small(a: &[u32], d: u32) -> (Vec<u32>, u32) {
    let mut q = vec![0u32; a.len()];
    let mut rem = 0u64;
    for i in (0..a.len()).rev() {
        let cur = (rem << 32) | a[i] as u64;
        q[i] = (cur / d as u64) as u32;
        rem = cur % d as u64;
    }
    trim(&mut q);
    (q, rem as u32)
}

/// Knuth D. Requires v nonzero.
fn mag_divrem(u: &[u32], v: &[u32]) -> (Vec<u32>, Vec<u32>) {
    assert!(!v.is_empty(), "division by zero");
    if mag_cmp(u, v) == Ordering::Less {
        return (Vec::new(), u.to_vec());
    }
    if v.len() == 1 {
        let (q, r) = mag_divrem_small(u, v[0]);
        let mut rv = vec![r];
        trim(&mut rv);
        return (q, rv);
    }
    let n = v.len();
    let m = u.len() - n;
    let s = v[n - 1].leading_zeros();
    let mut vn = vec![0u32; n];
    let mut un = vec![0u32; u.len() + 1];
    if s > 0 {
        for i in (1..n).rev() {
            vn[i] = (v[i] << s) | (v[i - 1] >> (32 - s));
        }
        vn[0] = v[0] << s;
        un[u.len()] = u[u.len() - 1] >> (32 - s);
        for i in (1..u.len()).rev() {
            un[i] = (u[i] << s) | (u[i - 1] >> (32 - s));
        }
        un[0] = u[0] << s;
    } else {
        vn.copy_from_slice(v);
        un[..u.len()].copy_from_slice(u);
        un[u.len()] = 0;
    }
    let b: u128 = 1u128 << 32;
    let mut q = vec![0u32; m + 1];
    for j in (0..=m).rev() {
        let num: u128 = ((un[j + n] as u128) << 32) | un[j + n - 1] as u128;
        let mut qhat = num / vn[n - 1] as u128;
        let mut rhat = num % vn[n - 1] as u128;
        loop {
            if qhat >= b || qhat * vn[n - 2] as u128 > ((rhat << 32) | un[j + n - 2] as u128) {
                qhat -= 1;
                rhat += vn[n - 1] as u128;
                if rhat < b {
                    continue;
                }
            }
            break;
        }
        let mut k: i128 = 0;
        let mut t: i128;
        for i in 0..n {
            let p: u128 = qhat * vn[i] as u128;
            t = un[i + j] as i128 - k - (p & 0xffff_ffff) as i128;
            un[i + j] = t as u32;
            k = (p >> 32) as i128 - (t >> 32);
        }
        t = un[j + n] as i128 - k;
        un[j + n] = t as u32;
        q[j] = qhat as u32;
        if t < 0 {
            q[j] = q[j].wrapping_sub(1);
            let mut c: u64 = 0;
            for i in 0..n {
                let s2 = un[i + j] as u64 + vn[i] as u64 + c;
                un[i + j] = s2 as u32;
                c = s2 >> 32;
            }
            un[j + n] = un[j + n].wrapping_add(c as u32);
        }
    }
    let mut r = vec![0u32; n];
    if s > 0 {
        for i in 0..n {
            r[i] = (un[i] >> s) | (((un[i + 1] as u64) << (32 - s)) as u32);
        }
    } else {
        r.copy_from_slice(&un[..n]);
    }
    trim(&mut q);
    trim(&mut r);
    (q, r)
}

impl Int {
    pub fn zero() -> Int {
        Int { neg: false, mag: Vec::new() }
    }
    pub fn from_u64(v: u64) -> Int {
        let mut mag = vec![v as u32, (v >> 32) as u32];
        trim(&mut mag);
        Int { neg: false, mag }
    }
    pub fn from_i64(v: i64) -> Int {
        let mut r = Int::from_u64(v.unsigned_abs());
        r.neg = v < 0 && !r.mag.is_empty();
        r
    }
    fn from_parts(neg: bool, mut mag: Vec<u32>) -> Int {
        trim(&mut mag);
        let neg = neg && !mag.is_empty();
        Int { neg, mag }
    }
    /// Parses a string of ASCII decimal digits (no sign). None if empty or not digits.
    pub fn from_dec_digits(s: &str) -> Option<Int> {
        if s.is_empty() || !s.bytes().all(|c| c.is_ascii_digit()) {
            return None;
        }
        let mut mag: Vec<u32> = Vec::new();
        for chunk in s.as_bytes().chunks(9) {
            let mut mul: u64 = 1;
            let mut add: u64 = 0;
            for &c in chunk {
                mul *= 10;
                add = add * 10 + (c - b'0') as u64;
            }
            let mut carry = add;
            for limb in mag.iter_mut() {
                let t = *limb as u64 * mul + carry;
                *limb = t as u32;
                carry = t >> 32;
            }
            while carry != 0 {
                mag.push(carry as u32);
                carry >>= 32;
            }
        }
        Some(Int::from_parts(false, mag))
    }
    pub fn to_dec(&self) -> String {
        if self.mag.is_empty() {
            return "0".to_string();
        }
        let mut parts = Vec::new();
        let mut cur = self.mag.clone();
        while !cur.is_empty() {
            let (q, r) = mag_divrem_small(&cur, 1_000_000_000);
            parts.push(r);
            cur = q;
        }
        let mut s = String::new();
        if self.neg {
            s.push('-');
        }
        s.push_str(&parts.last().unwrap().to_string());
        for p in parts.iter().rev().skip(1) {
            s.push_str(&format!("{:09}", p));
        }
        s
    }
    pub fn is_zero(&self) -> bool {
        self.mag.is_empty()
    }
    pub fn is_neg(&self) -> bool {
        self.neg
    }
    pub fn signum(&self) -> i32 {
        if self.mag.is_empty() {
            0
        } else if self.neg {
            -1
        } else {
            1
        }
    }
    pub fn neg(&self) -> Int {
        Int::from_parts(!self.neg, self.mag.clone())
    }
    pub fn abs(&self) -> Int {
        Int::from_parts(false, self.mag.clone())
    }
    pub fn add(&self, o: &Int) -> Int {
        if self.neg == o.neg {
            return Int::from_parts(self.neg, mag_add(&self.mag, &o.mag));
        }
        match mag_cmp(&self.mag, &o.mag) {
            Ordering::Equal => Int::zero(),
            Ordering::Greater => Int::from_parts(self.neg, mag_sub(&self.mag, &o.mag)),
            Ordering::Less => Int::from_parts(o.neg, mag_sub(&o.mag, &self.mag)),
        }
    }
    pub fn sub(&self, o: &Int) -> Int {
        self.add(&o.neg())
    }
    pub fn mul(&self, o: &Int) -> Int {
        Int::from_parts(self.neg != o.neg, mag_mul(&self.mag, &o.mag))
    }
    /// Truncated division (quotient rounds toward zero; remainder has the sign of self).
    pub fn divrem(&self, o: &Int) -> (Int, Int) {
        let (q, r) = mag_divrem(&self.mag, &o.mag);
        (Int::from_parts(self.neg != o.neg, q), Int::from_parts(self.neg, r))
    }
    /// Floor division and the matching non-negative remainder for a positive divisor.
    pub fn div_floor(&self, o: &Int) -> (Int, Int) {
        assert!(o.signum() > 0);
        let (q, r) = self.divrem(o);
        if r.is_neg() {
            (q.sub(&Int::from_u64(1)), r.add(o))
        } else {
            (q, r)
        }
    }
    pub fn cmp(&self, o: &Int) -> Ordering {
        match (self.neg, o.neg) {
            (false, true) => Ordering::Greater,
            (true, false) => Ordering::Less,
            (false, false) => mag_cmp(&self.mag, &o.mag),
            (true, true) => mag_cmp(&o.mag, &self.mag),
        }
    }
    pub fn gcd(a: &Int, b: &Int) -> Int {
        let mut x = a.abs();
        let mut y = b.abs();
        while !y.is_zero() {
            let (_, r) = x.divrem(&y);
            x = y;
            y = r;
        }
        x
    }
    pub fn shl(&self, bits: u32) -> Int {
        if self.mag.is_empty() {
            return self.clone();
        }
        let limbs = (bits / 32) as usize;
        let s = bits % 32;
        let mut r = vec![0u32; limbs];
        if s == 0 {
            r.extend_from_slice(&self.mag);
        } else {
            let mut carry = 0u32;
            for &l in &self.mag {
                r.push((l << s) | carry);
                carry = l >> (32 - s);
            }
            if carry != 0 {
                r.push(carry);
            }
        }
        Int::from_parts(self.neg, r)
    }
    pub fn bits(&self) -> u32 {
        match self.mag.last() {
            None => 0,
            Some(&top) => (self.mag.len() as u32 - 1) * 32 + (32 - top.leading_zeros()),
        }
    }
    pub fn pow10(k: u32) -> Int {
        let mut r = Int::from_u64(1);
        let ten = Int::from_u64(10);
        for _ in 0..k {
            r = r.mul(&ten);
        }
        r
    }
    pub fn pow2(k: u32) -> Int {
        Int::from_u64(1).shl(k)
    }
    /// floor(sqrt(self)) for self >= 0.
    pub fn isqrt(&self) -> Int {
        assert!(!self.neg);
        if self.is_zero() {
            return Int::zero();
        }
        let mut x = Int::pow2(self.bits() / 2 + 1);
        loop {
            let (q, _) = self.divrem(&x);
            let y = x.add(&q).shr1();
            if y.cmp(&x) != Ordering::Less {
                return x;
            }
            x = y;
        }
    }
    fn shr1(&self) -> Int {
        let mut r = self.mag.clone();
        let mut carry = 0u32;
        for i in (0..r.len()).rev() {
            let l = r[i];
            r[i] = (l >> 1) | (carry << 31);
            carry = l & 1;
        }
        Int::from_parts(self.neg, r)
    }
    pub fn is_even(&self) -> bool {
        self.mag.first().map_or(true, |l| l & 1 == 0)
    }
    /// The low 64 bits of the magnitude.
    pub fn low_u64(&self) -> u64 {
        let a = *self.mag.first().unwrap_or(&0) as u64;
        let b = *self.mag.get(1).unwrap_or(&0) as u64;
        a | (b << 32)
    }
    /// Top 64 bits of the magnitude, and the shift such that |self| ~ top * 2^shift (exact when shift <= 0).
    pub fn top_bits(&self, want: u32) -> (u64, i32) {
        let b = self.bits();
        if b <= want {
            return (self.low_u64(), 0);
        }
        let shift = b - want;
        let (q, _) = mag_divrem(&self.mag, &Int::pow2(shift).mag);
        let qi = Int::from_parts(false, q);
        (qi.low_u64(), shift as i32)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dec_roundtrip_and_division() {
        let a = Int::from_dec_digits("340282366920938463463374607431768211457").unwrap();
        assert_eq!(a.to_dec(), "340282366920938463463374607431768211457");
        let b = Int::from_dec_digits("18446744073709551629").unwrap();
        let (q, r) = a.divrem(&b);
        assert_eq!(q.mul(&b).add(&r), a);
        assert!(r.cmp(&b) == Ordering::Less);
        let c = a.mul(&a).add(&Int::from_u64(12345));
        assert_eq!(c.isqrt(), a);
        assert_eq!(Int::gcd(&Int::from_i64(-12), &Int::from_u64(18)), Int::from_u64(6));
        let (fq, fr) = Int::from_i64(-7).div_floor(&Int::from_u64(4));
        assert_eq!((fq, fr), (Int::from_i64(-2), Int::from_u64(1)));
    }
    #[test]
    fn knuth_d_random_like() {
        // deterministic pseudo-random operands exercising the add-back branch
        let mut x: u64 = 0x9e3779b97f4a7c15;
        for _ in 0..300 {
            let mut gen = |n: usize| {
                let mut v = Vec::new();
                for _ in 0..n {
                    x ^= x << 13;
                    x ^= x >> 7;
                    x ^= x << 17;
                    v.push(x as u32);
                }
                Int::from_parts(false, v)
            };
            let a = gen(9);
            let b = gen(4);
            if b.is_zero() {
                continue;
            }
            let (q, r) = a.divrem(&b);
            assert_eq!(q.mul(&b).add(&r), a);
            assert!(r.cmp(&b) == Ordering::Less);
        }
        let a = Int::from_parts(false, vec![0, 0, 0x8000_0000, 0x7fff_ffff]);
        let b = Int::from_parts(false, vec![1, 0, 0x8000_0000]);
        let (q, r) = a.divrem(&b);
        assert_eq!(q.mul(&b).add(&r), a);
    }
}
