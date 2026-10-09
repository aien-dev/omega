//! Exact rationals on i128 for case validation and kernel tests.
//! Inputs are bounded by the contract (|n|, d <= 2^20), so every product here fits.

use std::cmp::Ordering;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Rat {
    pub n: i128,
    pub d: i128,
}

fn gcd(mut a: i128, mut b: i128) -> i128 {
    a = a.abs();
    b = b.abs();
    while b != 0 {
        let t = a % b;
        a = b;
        b = t;
    }
    if a == 0 {
        1
    } else {
        a
    }
}

pub fn isqrt(n: i128) -> i128 {
    if n < 2 {
        return n.max(0);
    }
    let mut x = (n as f64).sqrt() as i128;
    while x * x > n {
        x -= 1;
    }
    while (x + 1) * (x + 1) <= n {
        x += 1;
    }
    x
}

impl Rat {
    pub fn new(n: i128, d: i128) -> Rat {
        assert!(d != 0, "zero denominator");
        let g = gcd(n, d);
        let (mut n, mut d) = (n / g, d / g);
        if d < 0 {
            n = -n;
            d = -d;
        }
        Rat { n, d }
    }
    pub fn int(n: i128) -> Rat {
        Rat { n, d: 1 }
    }
    pub fn zero() -> Rat {
        Rat { n: 0, d: 1 }
    }
    pub fn is_zero(&self) -> bool {
        self.n == 0
    }
    pub fn add(&self, o: &Rat) -> Rat {
        Rat::new(self.n * o.d + o.n * self.d, self.d * o.d)
    }
    pub fn sub(&self, o: &Rat) -> Rat {
        Rat::new(self.n * o.d - o.n * self.d, self.d * o.d)
    }
    pub fn mul(&self, o: &Rat) -> Rat {
        Rat::new(self.n * o.n, self.d * o.d)
    }
    pub fn neg(&self) -> Rat {
        Rat { n: -self.n, d: self.d }
    }
    pub fn to_f64(&self) -> f64 {
        // n and d are below 2^53 so each converts exactly; one correctly rounded division.
        self.n as f64 / self.d as f64
    }
    pub fn text(&self) -> String {
        format!("{}/{}", self.n, self.d)
    }
    /// Exact square root if the (nonnegative) rational is a perfect square.
    pub fn sqrt_exact(&self) -> Option<Rat> {
        if self.n < 0 {
            return None;
        }
        let (rn, rd) = (isqrt(self.n), isqrt(self.d));
        if rn * rn == self.n && rd * rd == self.d {
            Some(Rat::new(rn, rd))
        } else {
            None
        }
    }
}

impl PartialOrd for Rat {
    fn partial_cmp(&self, o: &Rat) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

impl Ord for Rat {
    fn cmp(&self, o: &Rat) -> Ordering {
        (self.n * o.d).cmp(&(o.n * self.d))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reduces_and_orders() {
        assert_eq!(Rat::new(2, 4), Rat::new(1, 2));
        assert_eq!(Rat::new(1, -2), Rat::new(-1, 2));
        assert!(Rat::new(-3, 2) < Rat::new(-1, 2));
        assert_eq!(Rat::new(1, 2).add(&Rat::new(-1, 2)), Rat::zero());
        assert_eq!(Rat::new(9, 100).sqrt_exact(), Some(Rat::new(3, 10)));
        assert_eq!(Rat::new(2, 1).sqrt_exact(), None);
        assert_eq!(isqrt(1 << 40), 1 << 20);
    }
}
