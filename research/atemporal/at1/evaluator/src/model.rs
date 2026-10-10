//! Shadow model: the evaluator's own computation of the AT-1 physics from the
//! case text (AT1_CASE_V1 section 3, AT1_SPEC Theorems 1 to 5), written from the
//! contract and the spec only, never from the oracle's or the engine's code.
//!
//! Kernel pairs, degenerate levels, level vectors and the zero test are exact
//! (rational and Gaussian-rational). The conditional vector uses relative
//! phases (Corollary 3.2). When every phase is a quarter turn the whole
//! computation stays exact; otherwise it runs in double-double (dd.rs) with a
//! stated ESTIMATED bound. The ideal reference rotates the Bloch vector of psi_0
//! about h by 2|h|(t_k - t_r) (AT1_SPEC Definition 5.2), exact when |h| is
//! rational and the angle is a quarter turn.

use crate::big::Int;
use crate::case::Case;
use crate::dd::{cos_sin_turns, cos_sin_turns_q, Dd};
use crate::rat::{Cq, Q};

/// Claimed absolute error of every inexact shadow value (double-double, ~1e-30 actual).
pub fn dd_bound() -> Q {
    Q::new(Int::from_u64(1), Int::pow10(24))
}

#[derive(Clone, Debug)]
pub struct Val {
    pub q: Q,
    pub exact: bool,
}
impl Val {
    pub fn exact(q: Q) -> Val {
        Val { q, exact: true }
    }
    pub fn approx(d: Dd) -> Val {
        Val { q: d.to_q(), exact: false }
    }
    pub fn bound(&self) -> Q {
        if self.exact { Q::zero() } else { dd_bound() }
    }
    pub fn f64(&self) -> f64 {
        Dd::from_q(&self.q).hi
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Defined,
    Undefined,
    Indeterminate,
}
impl Status {
    pub fn text(self) -> &'static str {
        match self {
            Status::Defined => "DEFINED",
            Status::Undefined => "UNDEFINED",
            Status::Indeterminate => "INDETERMINATE",
        }
    }
    pub fn parse(s: &str) -> Option<Status> {
        match s {
            "DEFINED" => Some(Status::Defined),
            "UNDEFINED" => Some(Status::Undefined),
            "INDETERMINATE" => Some(Status::Indeterminate),
            _ => None,
        }
    }
}

/// The contract's status rule: UNDEFINED if p + b <= tol; DEFINED if p - b > tol; else INDETERMINATE.
pub fn status_rule(p: &Q, b: &Q, tol: &Q) -> Status {
    if p.add(b).cmp(tol) != std::cmp::Ordering::Greater {
        Status::Undefined
    } else if p.sub(b).cmp(tol) == std::cmp::Ordering::Greater {
        Status::Defined
    } else {
        Status::Indeterminate
    }
}

/// Physics-level defects a mutant engine or oracle can carry (all false = honest).
#[derive(Clone, Copy, Debug, Default)]
pub struct Mods {
    pub drop_v: bool,
    pub flip_v: bool,
    pub shift_level: bool,
    pub phase_minus: bool,
}

#[derive(Clone, Debug)]
pub struct Shadow {
    pub kernel_dim: usize,
    pub trivial: bool,
    pub interacting_exact: bool,
    pub p: Vec<Val>,
    pub status: Vec<Status>,
    /// [k][axis X,Y,Z][PLUS, MINUS]; None for a label that is not DEFINED
    pub pauli: Vec<Option<[[Val; 2]; 3]>>,
    pub ideal: Vec<[[Val; 2]; 3]>,
    pub ideal_exact: bool,
    pub povm_residual: Val,
}

trait F: Clone {
    fn from_q(q: &Q) -> Self;
    fn add(&self, o: &Self) -> Self;
    fn sub(&self, o: &Self) -> Self;
    fn mul(&self, o: &Self) -> Self;
    fn div(&self, o: &Self) -> Self;
    fn is_zero_exact(&self) -> bool;
    /// (cos, sin) of 2 pi turns, or None when this field cannot represent it.
    fn phase(turns: &Q) -> Option<(Self, Self)>;
    fn val(&self) -> Val;
}

impl F for Q {
    fn from_q(q: &Q) -> Q {
        q.clone()
    }
    fn add(&self, o: &Q) -> Q {
        Q::add(self, o)
    }
    fn sub(&self, o: &Q) -> Q {
        Q::sub(self, o)
    }
    fn mul(&self, o: &Q) -> Q {
        Q::mul(self, o)
    }
    fn div(&self, o: &Q) -> Q {
        Q::div(self, o)
    }
    fn is_zero_exact(&self) -> bool {
        self.is_zero()
    }
    fn phase(turns: &Q) -> Option<(Q, Q)> {
        let f4 = turns.mod1().mul(&Q::int(4));
        if !f4.is_int() {
            return None;
        }
        Some(match f4.n.low_u64() {
            0 => (Q::one(), Q::zero()),
            1 => (Q::zero(), Q::one()),
            2 => (Q::int(-1), Q::zero()),
            _ => (Q::zero(), Q::int(-1)),
        })
    }
    fn val(&self) -> Val {
        Val::exact(self.clone())
    }
}

impl F for Dd {
    fn from_q(q: &Q) -> Dd {
        Dd::from_q(q)
    }
    fn add(&self, o: &Dd) -> Dd {
        Dd::add(*self, *o)
    }
    fn sub(&self, o: &Dd) -> Dd {
        Dd::sub(*self, *o)
    }
    fn mul(&self, o: &Dd) -> Dd {
        Dd::mul(*self, *o)
    }
    fn div(&self, o: &Dd) -> Dd {
        Dd::div(*self, *o)
    }
    fn is_zero_exact(&self) -> bool {
        self.hi == 0.0 && self.lo == 0.0
    }
    fn phase(turns: &Q) -> Option<(Dd, Dd)> {
        Some(cos_sin_turns_q(turns))
    }
    fn val(&self) -> Val {
        Val::approx(*self)
    }
}

#[derive(Clone)]
struct C<T: F> {
    re: T,
    im: T,
}
impl<T: F> C<T> {
    fn from_cq(z: &Cq) -> C<T> {
        C { re: T::from_q(&z.re), im: T::from_q(&z.im) }
    }
    fn add(&self, o: &C<T>) -> C<T> {
        C { re: self.re.add(&o.re), im: self.im.add(&o.im) }
    }
    fn mul(&self, o: &C<T>) -> C<T> {
        C { re: self.re.mul(&o.re).sub(&self.im.mul(&o.im)), im: self.re.mul(&o.im).add(&self.im.mul(&o.re)) }
    }
    fn norm2(&self) -> T {
        self.re.mul(&self.re).add(&self.im.mul(&self.im))
    }
}

/// Pauli outcome probabilities of the pure state chi (unnormalised), each computed directly.
fn pauli_of<T: F>(c0: &C<T>, c1: &C<T>) -> [[T; 2]; 3] {
    let n0 = c0.norm2();
    let n1 = c1.norm2();
    let nn = n0.add(&n1);
    // conj(c0) c1
    let xr = c0.re.mul(&c1.re).add(&c0.im.mul(&c1.im));
    let xi = c0.re.mul(&c1.im).sub(&c0.im.mul(&c1.re));
    let half = T::from_q(&Q::frac(1, 2));
    let q = |a: &T| a.div(&nn);
    [
        [half.add(&q(&xr)), half.sub(&q(&xr))],
        [half.add(&q(&xi)), half.sub(&q(&xi))],
        [q(&n0), q(&n1)],
    ]
}

fn val3<T: F>(a: &[[T; 2]; 3]) -> [[Val; 2]; 3] {
    [[a[0][0].val(), a[0][1].val()], [a[1][0].val(), a[1][1].val()], [a[2][0].val(), a[2][1].val()]]
}

/// Level structure: per level, the exact level vector u_j (zero when the level is not in the kernel).
pub struct Levels {
    pub kernel_dim: usize,
    pub u: Vec<[Cq; 2]>,
    pub s: Q,
}

pub fn levels(c: &Case, m: &Mods) -> Levels {
    let n = c.n;
    let mut kernel_dim = 0;
    let mut u = Vec::new();
    for j in 0..n {
        let src = if m.shift_level { (j + n - 1) % n } else { j };
        let mut v = c.v[src].clone();
        if m.drop_v {
            v = [Q::zero(), Q::zero(), Q::zero()];
        }
        if m.flip_v {
            v = [v[0].neg(), v[1].neg(), v[2].neg()];
        }
        let nx = c.h[0].add(&v[0]);
        let ny = c.h[1].add(&v[1]);
        let nz = c.h[2].add(&v[2]);
        let r2 = nx.mul(&nx).add(&ny.mul(&ny)).add(&nz.mul(&nz));
        let base = c.e[j].add(&c.h0);
        let zero2 = [Cq::zero(), Cq::zero()];
        let r = match r2.sqrt_exact() {
            Some(r) => r,
            None => {
                // irrational level norm (only reachable by a mutant): never matches a rational energy
                u.push(zero2);
                continue;
            }
        };
        if r.is_zero() {
            if base.is_zero() {
                kernel_dim += 2;
                u.push(c.psi.clone());
            } else {
                u.push(zero2);
            }
            continue;
        }
        let mut found = None;
        for s in [1i64, -1] {
            let sr = r.mul(&Q::int(s));
            if base.add(&sr).is_zero() {
                found = Some(sr);
            }
        }
        match found {
            None => u.push(zero2),
            Some(sr) => {
                kernel_dim += 1;
                // contract eigenvector (s|n| + n_z, n_x + i n_y), else (n_x - i n_y, s|n| - n_z)
                let mut f = [Cq::real(sr.add(&nz)), Cq::new(nx.clone(), ny.clone())];
                if f[0].is_zero() && f[1].is_zero() {
                    f = [Cq::new(nx.clone(), ny.neg()), Cq::real(sr.sub(&nz))];
                }
                let ff = f[0].norm2().add(&f[1].norm2());
                let ip = f[0].conj().mul(&c.psi[0]).add(&f[1].conj().mul(&c.psi[1]));
                let coef = ip.scale(&Q::one().div(&ff));
                u.push([f[0].mul(&coef), f[1].mul(&coef)]);
            }
        }
    }
    let mut s = Q::zero();
    for uj in &u {
        s = s.add(&uj[0].norm2()).add(&uj[1].norm2());
    }
    Levels { kernel_dim, u, s }
}

struct Inter {
    p: Vec<Val>,
    pauli: Vec<Option<[[Val; 2]; 3]>>,
    zero: Vec<bool>,
}

fn interacting<T: F>(c: &Case, lv: &Levels, m: &Mods) -> Option<Inter> {
    let a = lv.u.iter().position(|x| !(x[0].is_zero() && x[1].is_zero()))?;
    let n_q = Q::int(c.n as i64);
    let ns = T::from_q(&n_q.mul(&lv.s));
    let w = T::from_q(&c.w);
    let mut out = Inter { p: Vec::new(), pauli: Vec::new(), zero: Vec::new() };
    for k in 0..c.m {
        let dk = Q::int(k as i64 - c.ref_index as i64);
        let mut chi0 = C { re: T::from_q(&Q::zero()), im: T::from_q(&Q::zero()) };
        let mut chi1 = chi0.clone();
        for (j, uj) in lv.u.iter().enumerate() {
            if uj[0].is_zero() && uj[1].is_zero() {
                continue;
            }
            let mut turns = c.e[j].sub(&c.e[a]).mul(&dk).mul(&c.tau);
            if m.phase_minus {
                turns = turns.neg();
            }
            let (cs, sn) = T::phase(&turns)?;
            let ph = C { re: cs, im: sn };
            chi0 = chi0.add(&ph.mul(&C::from_cq(&uj[0])));
            chi1 = chi1.add(&ph.mul(&C::from_cq(&uj[1])));
        }
        let nn = chi0.norm2().add(&chi1.norm2());
        let p = w.mul(&nn).div(&ns);
        out.zero.push(nn.is_zero_exact());
        out.p.push(p.val());
        if nn.is_zero_exact() {
            out.pauli.push(None);
        } else {
            out.pauli.push(Some(val3(&pauli_of(&chi0, &chi1))));
        }
    }
    Some(out)
}

fn bloch(psi: &[Cq; 2]) -> [Q; 3] {
    let n = psi[0].norm2().add(&psi[1].norm2());
    let x = psi[0].conj().mul(&psi[1]);
    let two = Q::int(2);
    [x.re.mul(&two).div(&n), x.im.mul(&two).div(&n), psi[0].norm2().sub(&psi[1].norm2()).div(&n)]
}

fn rotate<T: F>(b: &[T; 3], nh: &[T; 3], cs: &T, sn: &T) -> [T; 3] {
    let one = T::from_q(&Q::one());
    let dot = nh[0].mul(&b[0]).add(&nh[1].mul(&b[1])).add(&nh[2].mul(&b[2]));
    let cross = [
        nh[1].mul(&b[2]).sub(&nh[2].mul(&b[1])),
        nh[2].mul(&b[0]).sub(&nh[0].mul(&b[2])),
        nh[0].mul(&b[1]).sub(&nh[1].mul(&b[0])),
    ];
    let omc = one.sub(cs);
    let mut r = b.clone();
    for i in 0..3 {
        r[i] = b[i].mul(cs).add(&cross[i].mul(sn)).add(&nh[i].mul(&dot).mul(&omc));
    }
    r
}

fn probs_from_bloch<T: F>(b: &[T; 3]) -> [[Val; 2]; 3] {
    let half = T::from_q(&Q::frac(1, 2));
    let mut out: Vec<[Val; 2]> = Vec::new();
    for i in 0..3 {
        let hb = b[i].mul(&half);
        out.push([half.add(&hb).val(), half.sub(&hb).val()]);
    }
    [out[0].clone(), out[1].clone(), out[2].clone()]
}

/// Ideal reference for every label: (values, all exact?).
fn ideal(c: &Case, drop_h: bool) -> (Vec<[[Val; 2]; 3]>, bool) {
    let b = bloch(&c.psi);
    let h2 = c.h[0].mul(&c.h[0]).add(&c.h[1].mul(&c.h[1])).add(&c.h[2].mul(&c.h[2]));
    let mut out = Vec::new();
    let mut all_exact = true;
    for k in 0..c.m {
        if h2.is_zero() || drop_h {
            out.push(probs_from_bloch::<Q>(&b));
            continue;
        }
        let dk = Q::int(k as i64 - c.ref_index as i64);
        if let Some(hn) = h2.sqrt_exact() {
            let turns = Q::int(2).mul(&hn).mul(&dk).mul(&c.tau);
            let nh = [c.h[0].div(&hn), c.h[1].div(&hn), c.h[2].div(&hn)];
            if let Some((cs, sn)) = <Q as F>::phase(&turns) {
                out.push(probs_from_bloch::<Q>(&rotate::<Q>(&b, &nh, &cs, &sn)));
                continue;
            }
            all_exact = false;
            let (cs, sn) = cos_sin_turns_q(&turns);
            let bd = [Dd::from_q(&b[0]), Dd::from_q(&b[1]), Dd::from_q(&b[2])];
            let nd = [Dd::from_q(&nh[0]), Dd::from_q(&nh[1]), Dd::from_q(&nh[2])];
            out.push(probs_from_bloch::<Dd>(&rotate::<Dd>(&bd, &nd, &cs, &sn)));
        } else {
            all_exact = false;
            let hn = Dd::from_q(&h2).sqrt();
            let turns = hn.mul(Dd::from_q(&Q::int(2).mul(&dk).mul(&c.tau)));
            let (cs, sn) = cos_sin_turns(turns);
            let bd = [Dd::from_q(&b[0]), Dd::from_q(&b[1]), Dd::from_q(&b[2])];
            let nd = [Dd::from_q(&c.h[0]).div(hn), Dd::from_q(&c.h[1]).div(hn), Dd::from_q(&c.h[2]).div(hn)];
            out.push(probs_from_bloch::<Dd>(&rotate::<Dd>(&bd, &nd, &cs, &sn)));
        }
    }
    (out, all_exact)
}

/// || sum_k F_k - I ||_F (AT1_SPEC Theorem 2), exact when every gap sum is exact.
fn povm_residual(c: &Case) -> Val {
    let n = c.n;
    let mq = Q::int(c.m as i64);
    let wn = c.w.div(&Q::int(n as i64));
    // diagonal: (w M / N - 1)^2 per level
    let diag = wn.mul(&mq).sub(&Q::one());
    let mut exact_sum = diag.mul(&diag).mul(&Q::int(n as i64));
    let mut dd_extra = Dd::zero();
    let mut exact = true;
    for j in 0..n {
        for jp in (j + 1)..n {
            let dtau = c.e[j].sub(&c.e[jp]).mul(&c.tau);
            let s2: Option<Q> = if dtau.is_int() {
                Some(mq.mul(&mq))
            } else if dtau.mul(&mq).is_int() {
                Some(Q::zero())
            } else {
                // direct sum of exp(-2 pi i D k tau), k = 0..M-1
                let mut ex = Some((Q::zero(), Q::zero()));
                let mut re = Dd::zero();
                let mut im = Dd::zero();
                for k in 0..c.m {
                    let t = dtau.mul(&Q::int(k as i64)).neg();
                    if let Some((a, b)) = &mut ex {
                        match <Q as F>::phase(&t) {
                            Some((cs, sn)) => {
                                *a = a.add(&cs);
                                *b = b.add(&sn);
                            }
                            None => {}
                        }
                    }
                    if <Q as F>::phase(&t).is_none() {
                        ex = None;
                    }
                    let (cs, sn) = cos_sin_turns_q(&t);
                    re = re.add(cs);
                    im = im.add(sn);
                }
                match ex {
                    Some((a, b)) => Some(a.mul(&a).add(&b.mul(&b))),
                    None => {
                        exact = false;
                        dd_extra = dd_extra.add(re.mul(re).add(im.mul(im)).mul(Dd::from_q(&wn.mul(&wn))).mul_f(2.0));
                        None
                    }
                }
            };
            if let Some(s2) = s2 {
                exact_sum = exact_sum.add(&s2.mul(&wn).mul(&wn).mul(&Q::int(2)));
            }
        }
    }
    if exact {
        if let Some(r) = exact_sum.sqrt_exact() {
            return Val::exact(r);
        }
    }
    Val::approx(Dd::from_q(&exact_sum).add(dd_extra).sqrt())
}

pub fn shadow(c: &Case, m: &Mods, force_dd: bool) -> Shadow {
    let lv = levels(c, m);
    let trivial = lv.kernel_dim == 0 || lv.s.is_zero();
    let tolz = Q::from_scaled(&c.tol_zero);
    let (ideal_v, ideal_exact) = ideal(c, false);
    let povm = povm_residual(c);
    let mut sh = Shadow {
        kernel_dim: lv.kernel_dim,
        trivial,
        interacting_exact: true,
        p: Vec::new(),
        status: vec![Status::Undefined; c.m],
        pauli: vec![None; c.m],
        ideal: ideal_v,
        ideal_exact,
        povm_residual: povm,
    };
    if trivial {
        return sh;
    }
    let inter = if force_dd { None } else { interacting::<Q>(c, &lv, m) };
    let inter = match inter {
        Some(i) => i,
        None => {
            sh.interacting_exact = false;
            interacting::<Dd>(c, &lv, m).expect("double-double path always succeeds")
        }
    };
    for k in 0..c.m {
        let pv = &inter.p[k];
        let st = if inter.zero[k] && pv.exact { Status::Undefined } else { status_rule(&pv.q, &pv.bound(), &tolz) };
        sh.status[k] = st;
        if st == Status::Defined {
            sh.pauli[k] = inter.pauli[k].clone();
        }
    }
    sh.p = inter.p;
    sh
}

/// Shadow for the ideal model of the same case (V dropped): used by mutants that write the ideal marginal.
pub fn ideal_marginal(c: &Case) -> Q {
    c.w.div(&Q::int(c.n as i64))
}

/// Ideal reference values (Definition 5.2), optionally without H_S (oracle mutant).
pub fn ideal_values(c: &Case, drop_h: bool) -> Vec<[[Val; 2]; 3]> {
    ideal(c, drop_h).0
}

/// Display form: exact rational, or a decimal marked `~` for a double-double value.
pub fn show(v: &Val) -> String {
    if v.exact { v.q.text() } else { format!("~{:.17e}", v.q.to_f64()) }
}
