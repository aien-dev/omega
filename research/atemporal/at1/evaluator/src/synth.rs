//! Synthetic writers built on the shadow: an honest engine, an honest oracle
//! record, the runner's splice, and every mutant of the receipt. These are the
//! evaluator's own controls; they are never presented as a candidate.

use crate::big::Int;
use crate::case::{case_bytes_from_body, BoundKind, Case};
use crate::judge::{label_from, placeholder_verdict, rederive};
use crate::model::{shadow, status_rule, Mods, Shadow, Status, Val};
use crate::rat::{Scaled, Q};
use crate::result::{zero_scaled, Prov, RVal, ResultFile, Six};
use crate::sha256::sha256_hex;

pub const SYNTH_TIME: &str = "2026-10-09T00:00:00Z";
pub const SYNTH_COMMIT: &str = "0000000000000000000000000000000000000000";

/// Engine-side defects (all false = the honest synthetic engine).
#[derive(Clone, Copy, Debug, Default)]
pub struct EngineMut {
    pub physics: Mods,
    /// M5: P(Y+) and P(Y-) exchanged
    pub y_sign: bool,
    /// M6: X and Y axes exchanged
    pub axis_swap: bool,
    /// clock marginal written as the ideal w/N instead of the interacting p(k)
    pub ideal_marginal: bool,
    /// label k reuses the status and Pauli values computed for label k-1 (stale cache)
    pub stale_label: bool,
    pub bound_kind_none_nonzero: bool,
}

/// Oracle-side defects (all false = the honest synthetic oracle).
#[derive(Clone, Copy, Debug, Default)]
pub struct OracleMut {
    pub physics: Mods,
    /// reference_ideal computed without H_S (no rotation)
    pub ideal_drop_h: bool,
}

pub fn bound_1e15() -> Scaled {
    Scaled { n: Int::from_u64(1), k: 15 }
}

fn rv(v: &Val, b: &Scaled) -> RVal {
    RVal::of(v.f64(), b)
}
fn undef6() -> Six {
    let u = RVal::undefined();
    [[u.clone(), u.clone()], [u.clone(), u.clone()], [u.clone(), u]]
}
fn six_of(s: &[[Val; 2]; 3], b: &Scaled) -> Six {
    [[rv(&s[0][0], b), rv(&s[0][1], b)], [rv(&s[1][0], b), rv(&s[1][1], b)], [rv(&s[2][0], b), rv(&s[2][1], b)]]
}

pub fn synth_prov(engine_tag: &str, oracle_tag: Option<&str>) -> Prov {
    let e = sha256_hex(engine_tag.as_bytes());
    let o = match oracle_tag {
        Some(t) => sha256_hex(t.as_bytes()),
        None => e.clone(),
    };
    Prov {
        source_repo: "aien-dev/omega".into(),
        source_commit: SYNTH_COMMIT.into(),
        source_tree_clean: "YES".into(),
        contract_commit: crate::verify::CONTRACT_COMMITS[0].into(),
        engine_sha256: e,
        oracle_repo: "aien-dev/omega".into(),
        oracle_commit: SYNTH_COMMIT.into(),
        oracle_sha256: o,
        build_cc: if oracle_tag.is_none() { format!("oracle {}", engine_tag) } else { engine_tag.to_string() },
        build_flags: "synthetic writer of the AT-1 Agent 4 evaluator (shadow model)".into(),
        host: "synthetic: values from the at1-eval shadow model, not a candidate".into(),
        run_started: SYNTH_TIME.into(),
        run_finished: SYNTH_TIME.into(),
        artifacts: Vec::new(),
    }
}

fn blank(c: &Case, prov: Prov) -> ResultFile {
    let m = c.m;
    ResultFile {
        case: c.clone(),
        case_file_sha256: sha256_hex(&case_bytes_from_body(&c.body_lines)),
        arithmetic: "BINARY64".into(),
        bound_kind: BoundKind::Estimated,
        kernel_dim: 0,
        constraint: RVal::undefined(),
        povm: RVal::undefined(),
        label: vec![Status::Undefined; m],
        clock: vec![RVal::undefined(); m],
        pauli: vec![undef6(); m],
        ref_label: vec![Status::Undefined; m],
        ref_clock: vec![RVal::undefined(); m],
        ref_ideal: vec![undef6(); m],
        ref_inter: vec![undef6(); m],
        verdict: placeholder_verdict("NOT_RUN", "none"),
        verdict_id: String::new(),
        prov,
        evidence_digest: String::new(),
    }
}

/// The interacting family written from a shadow: (kernel_dim, trivial, labels, clock, pauli).
fn interacting_lines(c: &Case, sh: &Shadow, b: &Scaled) -> (Vec<Status>, Vec<RVal>, Vec<Six>) {
    let tolz = Q::from_scaled(&c.tol_zero);
    let mut lab = vec![Status::Undefined; c.m];
    let mut clk = vec![RVal::undefined(); c.m];
    let mut pa = vec![undef6(); c.m];
    if sh.trivial {
        return (lab, clk, pa);
    }
    for k in 0..c.m {
        clk[k] = rv(&sh.p[k], b);
        lab[k] = label_from(&clk[k], &tolz);
        if lab[k] == Status::Defined {
            if let Some(s) = &sh.pauli[k] {
                pa[k] = six_of(s, b);
            }
        }
    }
    (lab, clk, pa)
}

/// Engine result before the splice: engine families written, reference families
/// undefined (the runner replaces them), verdict judged on the engine's own claim.
pub fn engine_result(c: &Case, em: &EngineMut, tag: &str) -> ResultFile {
    let b = bound_1e15();
    let sh = shadow(c, &em.physics, false);
    let mut r = blank(c, synth_prov(tag, Some("pending-splice")));
    r.kernel_dim = sh.kernel_dim as i64;
    r.povm = rv(&sh.povm_residual, &b);
    if !sh.trivial {
        r.constraint = RVal::of(0.0, &b);
        let (mut lab, mut clk, mut pa) = interacting_lines(c, &sh, &b);
        let tolz = Q::from_scaled(&c.tol_zero);
        if em.ideal_marginal {
            let wn = Val::exact(c.w.div(&Q::int(c.n as i64)));
            for k in 0..c.m {
                clk[k] = rv(&wn, &b);
                let st = status_rule(&clk[k].q().unwrap(), &clk[k].bq(), &tolz);
                if st != lab[k] {
                    // the label now claims what the ideal marginal implies
                    lab[k] = st;
                    if st == Status::Defined {
                        pa[k] = match &sh.pauli[k] { Some(s) => six_of(s, &b), None => six_of(&sh.ideal[k], &b) };
                    } else {
                        pa[k] = undef6();
                    }
                }
            }
        }
        for k in 0..c.m {
            if lab[k] != Status::Defined {
                continue;
            }
            if em.y_sign {
                pa[k][1].swap(0, 1);
            }
            if em.axis_swap {
                pa[k].swap(0, 1);
            }
        }
        if em.stale_label {
            for k in (1..c.m).rev() {
                lab[k] = lab[k - 1];
                pa[k] = pa[k - 1].clone();
            }
        }
        r.label = lab;
        r.clock = clk;
        r.pauli = pa;
    }
    if em.bound_kind_none_nonzero {
        r.bound_kind = BoundKind::None;
    }
    let nontrivial = !crate::verify::claims_trivial(&r);
    r.verdict = rederive(&r, nontrivial);
    r.seal();
    r
}

/// Oracle-written record: build_cc `oracle ...`, engine_sha256 == oracle_sha256.
/// Its engine families repeat its own interacting values.
pub fn oracle_record(c: &Case, om: &OracleMut, tag: &str) -> ResultFile {
    let b = bound_1e15();
    let sh = shadow(c, &om.physics, false);
    let mut r = blank(c, synth_prov(tag, None));
    r.kernel_dim = sh.kernel_dim as i64;
    r.povm = rv(&sh.povm_residual, &b);
    let ideal = if om.ideal_drop_h { crate::model::ideal_values(c, true) } else { sh.ideal.clone() };
    for k in 0..c.m {
        r.ref_ideal[k] = six_of(&ideal[k], &b);
    }
    if !sh.trivial {
        r.constraint = RVal::of(0.0, &b);
        let (lab, clk, pa) = interacting_lines(c, &sh, &b);
        r.label = lab.clone();
        r.clock = clk.clone();
        r.pauli = pa.clone();
        r.ref_label = lab;
        r.ref_clock = clk;
        r.ref_inter = pa;
    }
    let nontrivial = !crate::verify::claims_trivial(&r);
    r.verdict = rederive(&r, nontrivial);
    r.seal();
    r
}

/// The runner's splice (charter reading c): every reference_* family of the
/// oracle record replaces the candidate's, oracle provenance is filled from the
/// record, the verdict is judged again on the engine's own trivial claim, and
/// both identities are recomputed. An oracle record that did not complete
/// turns the candidate into ERROR ORACLE_UNAVAILABLE.
pub fn splice(engine: &ResultFile, oracle: &ResultFile) -> Result<ResultFile, String> {
    if engine.case.body_lines != oracle.case.body_lines || engine.case_file_sha256 != oracle.case_file_sha256 {
        return Err("engine result and oracle record are not for the same case".into());
    }
    if !(oracle.prov.build_cc.starts_with("oracle ") && oracle.prov.engine_sha256 == oracle.prov.oracle_sha256) {
        return Err("second input is not an oracle-marked record (build_cc `oracle ...`, engine_sha256 == oracle_sha256)".into());
    }
    if engine.prov.build_cc.starts_with("oracle ") {
        return Err("first input is an oracle record, never a candidate".into());
    }
    let mut r = engine.clone();
    let eo = engine.verdict.outcome.as_str();
    if eo == "ERROR" || eo == "NOT_RUN" {
        return Ok(r);
    }
    r.prov.oracle_repo = oracle.prov.source_repo.clone();
    r.prov.oracle_commit = oracle.prov.source_commit.clone();
    r.prov.oracle_sha256 = oracle.prov.engine_sha256.clone();
    let oo = oracle.verdict.outcome.as_str();
    if oo == "ERROR" || oo == "NOT_RUN" {
        let m = r.case.m;
        r.kernel_dim = 0;
        r.constraint = RVal::undefined();
        r.povm = RVal::undefined();
        r.label = vec![Status::Undefined; m];
        r.clock = vec![RVal::undefined(); m];
        r.pauli = vec![undef6(); m];
        r.ref_label = vec![Status::Undefined; m];
        r.ref_clock = vec![RVal::undefined(); m];
        r.ref_ideal = vec![undef6(); m];
        r.ref_inter = vec![undef6(); m];
        r.verdict = placeholder_verdict("ERROR", "ORACLE_UNAVAILABLE");
        r.seal();
        return Ok(r);
    }
    r.ref_label = oracle.ref_label.clone();
    r.ref_clock = oracle.ref_clock.clone();
    r.ref_ideal = oracle.ref_ideal.clone();
    r.ref_inter = oracle.ref_inter.clone();
    let nontrivial = !crate::verify::claims_trivial(&r);
    r.verdict = rederive(&r, nontrivial);
    r.seal();
    Ok(r)
}

pub fn none_bound() -> Scaled {
    zero_scaled()
}
