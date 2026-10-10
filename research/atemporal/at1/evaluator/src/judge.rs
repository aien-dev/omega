//! Exact re-derivation of the AT1_RESULT_V1 section 4 checks and the section 5
//! outcome, codes and expectation from the written values. Every comparison is
//! on exact rationals (a binary64 value and a scaled decimal are both exact).

use crate::case::Case;
use crate::model::Status;
use crate::rat::Q;
use crate::result::{Ck, Kind, RVal, ResultFile, Six, Verdict};
use std::cmp::Ordering;

pub const CHECK_CODES: [&str; 12] = [
    "BOUND_KIND_INSUFFICIENT",
    "NONFINITE_VALUE",
    "TRIVIAL_PHYSICAL_STATE",
    "CONSTRAINT_RESIDUAL_EXCEEDED",
    "POVM_NORMALIZATION_EXCEEDED",
    "PROBABILITY_SUM_EXCEEDED",
    "PROBABILITY_OUT_OF_RANGE",
    "PROBABILITY_SUM_EXCEEDED",
    "CONDITIONAL_UNDEFINED",
    "", // check 10: depends on prediction_target
    "ORACLE_DISAGREEMENT",
    "ORACLE_DISAGREEMENT",
];

/// Absolute form: PASS if |v|+b <= tol; FAIL if |v|-b > tol; else INDETERMINATE.
pub fn tri_abs(v: &Q, b: &Q, tol: &Q) -> Ck {
    let a = v.abs();
    if a.add(b).cmp(tol) != Ordering::Greater {
        Ck::Pass
    } else if a.sub(b).cmp(tol) == Ordering::Greater {
        Ck::Fail
    } else {
        Ck::Indet
    }
}
/// Signed form (range limits): PASS if v+b <= tol; FAIL if v-b > tol; else INDETERMINATE.
pub fn tri_signed(v: &Q, b: &Q, tol: &Q) -> Ck {
    if v.add(b).cmp(tol) != Ordering::Greater {
        Ck::Pass
    } else if v.sub(b).cmp(tol) == Ordering::Greater {
        Ck::Fail
    } else {
        Ck::Indet
    }
}
/// FAIL > INDETERMINATE > PASS; NOT_EVALUATED is neutral.
pub fn worst(a: Ck, b: Ck) -> Ck {
    let r = |c: Ck| match c {
        Ck::NotEval => 0,
        Ck::Pass => 1,
        Ck::Indet => 2,
        Ck::Fail => 3,
    };
    if r(a) >= r(b) { a } else { b }
}

/// Value and bound of a written token, None for nonfinite or undefined.
fn qv(v: &RVal) -> Option<(Q, Q)> {
    v.q().map(|q| (q, v.bq()))
}

/// Status rule of section 3 applied to a written clock probability.
pub fn label_from(v: &RVal, tol_zero: &Q) -> Status {
    match qv(v) {
        None => Status::Undefined,
        Some((p, b)) => crate::model::status_rule(&p, &b, tol_zero),
    }
}

fn dev_six(a: &Six, b: &Six, tol: &Q, st: &mut Ck) {
    for ax in 0..3 {
        for g in 0..2 {
            if let (Some((x, bx)), Some((y, by))) = (qv(&a[ax][g]), qv(&b[ax][g])) {
                *st = worst(*st, tri_abs(&x.sub(&y), &bx.add(&by), tol));
            }
        }
    }
}

pub fn all_values(r: &ResultFile) -> Vec<&RVal> {
    let mut v: Vec<&RVal> = vec![&r.constraint, &r.povm];
    for k in 0..r.case.m {
        v.push(&r.clock[k]);
        v.push(&r.ref_clock[k]);
        for f in [&r.pauli[k], &r.ref_ideal[k], &r.ref_inter[k]] {
            for ax in 0..3 {
                for g in 0..2 {
                    v.push(&f[ax][g]);
                }
            }
        }
    }
    v
}

/// Re-derives the verdict block of a completed run. `nontrivial` is the exact
/// decision of check 3 (the caller supplies it: from the shadow when verifying,
/// from the engine's own claim when splicing as the runner).
pub fn rederive(r: &ResultFile, nontrivial: bool) -> Verdict {
    let c: &Case = &r.case;
    let tol_prob = Q::from_scaled(&c.tol_prob);
    let tol_schro = Q::from_scaled(&c.tol_schro);
    let one = Q::one();
    let mut ck = [Ck::NotEval; 12];
    ck[0] = if r.bound_kind >= c.min_bound_kind { Ck::Pass } else { Ck::Fail };
    ck[1] = if all_values(r).iter().any(|v| v.kind == Kind::Nonfinite) { Ck::Fail } else { Ck::Pass };
    ck[2] = if nontrivial { Ck::Pass } else { Ck::Fail };
    if let Some((v, b)) = qv(&r.povm) {
        ck[4] = tri_abs(&v, &b, &Q::from_scaled(&c.tol_povm));
    }
    if nontrivial {
        if let Some((v, b)) = qv(&r.constraint) {
            ck[3] = tri_abs(&v, &b, &Q::from_scaled(&c.tol_constraint));
        }
        // 6: sum of every clock probability minus 1, summed bounds
        let mut sum = Q::zero();
        let mut bs = Q::zero();
        let mut ok = true;
        for k in 0..c.m {
            match qv(&r.clock[k]) {
                Some((v, b)) => {
                    sum = sum.add(&v);
                    bs = bs.add(&b);
                }
                None => ok = false,
            }
        }
        if ok {
            ck[5] = tri_abs(&sum.sub(&one), &bs, &tol_prob);
        }
        // 7: every engine probability, signed form, both limits (reference lines excluded)
        let mut st = Ck::NotEval;
        for k in 0..c.m {
            let mut items: Vec<&RVal> = vec![&r.clock[k]];
            for ax in 0..3 {
                for g in 0..2 {
                    items.push(&r.pauli[k][ax][g]);
                }
            }
            for it in items {
                if let Some((v, b)) = qv(it) {
                    st = worst(st, tri_signed(&v.neg(), &b, &tol_prob));
                    st = worst(st, tri_signed(&v.sub(&one), &b, &tol_prob));
                }
            }
        }
        ck[6] = st;
        // 8: PLUS + MINUS - 1 per defined label and axis
        let mut st = Ck::NotEval;
        for k in 0..c.m {
            if r.label[k] != Status::Defined {
                continue;
            }
            for ax in 0..3 {
                if let (Some((p, bp)), Some((q, bq))) = (qv(&r.pauli[k][ax][0]), qv(&r.pauli[k][ax][1])) {
                    st = worst(st, tri_abs(&p.add(&q).sub(&one), &bp.add(&bq), &tol_prob));
                }
            }
        }
        ck[7] = st;
        // 9: no label UNDEFINED
        ck[8] = if r.label.iter().any(|s| *s == Status::Undefined) { Ck::Fail } else { Ck::Pass };
        // 10 and 11: labels DEFINED on both sides only
        let wn = c.w.div(&Q::int(c.n as i64));
        let mut st10 = Ck::NotEval;
        let mut st11 = Ck::NotEval;
        for k in 0..c.m {
            if r.label[k] != Status::Defined || r.ref_label[k] != Status::Defined {
                continue;
            }
            let target = if c.target_ideal { &r.ref_ideal[k] } else { &r.ref_inter[k] };
            dev_six(&r.pauli[k], target, &tol_schro, &mut st10);
            if let Some((p, bp)) = qv(&r.clock[k]) {
                if c.target_ideal {
                    st10 = worst(st10, tri_abs(&p.sub(&wn), &bp, &tol_prob));
                } else if let Some((m, bm)) = qv(&r.ref_clock[k]) {
                    st10 = worst(st10, tri_abs(&p.sub(&m), &bp.add(&bm), &tol_prob));
                }
            }
            if c.target_ideal {
                dev_six(&r.pauli[k], &r.ref_inter[k], &tol_schro, &mut st11);
                if let (Some((p, bp)), Some((m, bm))) = (qv(&r.clock[k]), qv(&r.ref_clock[k])) {
                    st11 = worst(st11, tri_abs(&p.sub(&m), &bp.add(&bm), &tol_prob));
                }
            }
        }
        ck[9] = st10;
        ck[10] = if c.target_ideal { st11 } else { Ck::NotEval };
        // 12: label status agreement
        let mut st = Ck::NotEval;
        for k in 0..c.m {
            let (a, b) = (r.label[k], r.ref_label[k]);
            let one = if a == Status::Indeterminate || b == Status::Indeterminate {
                Ck::Indet
            } else if a != b {
                Ck::Fail
            } else {
                Ck::Pass
            };
            st = worst(st, one);
        }
        ck[11] = st;
    }
    let mut codes: Vec<String> = Vec::new();
    let mut add = |s: &str| {
        if !codes.iter().any(|x| x == s) {
            codes.push(s.to_string());
        }
    };
    for i in 0..12 {
        if ck[i] == Ck::Fail {
            if i == 9 {
                add(if c.target_ideal { "SCHRODINGER_DEVIATION_EXCEEDED" } else { "INTERACTING_DEVIATION_EXCEEDED" });
            } else {
                add(CHECK_CODES[i]);
            }
        }
        if ck[i] == Ck::Indet {
            add("PRECISION_INSUFFICIENT");
        }
    }
    if r.label.iter().any(|s| *s == Status::Indeterminate) {
        add("PRECISION_INSUFFICIENT");
    }
    codes.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    let outcome = if codes.is_empty() { "PASS" } else { "FAIL" };
    let exp_outcome = if c.expected_pass { "PASS" } else { "FAIL" };
    let met = outcome == exp_outcome && codes == c.expected_codes;
    Verdict {
        checks: ck,
        outcome: outcome.into(),
        failure_codes: codes,
        error_code: "none".into(),
        expectation: if met { "YES".into() } else { "NO".into() },
    }
}

/// The ERROR / NOT_RUN verdict shape.
pub fn placeholder_verdict(outcome: &str, error_code: &str) -> Verdict {
    Verdict {
        checks: [Ck::NotEval; 12],
        outcome: outcome.into(),
        failure_codes: Vec::new(),
        error_code: error_code.into(),
        expectation: "NOT_APPLICABLE".into(),
    }
}
