//! Mutant kill receipt. Every mutant is a deliberately broken engine, oracle or
//! result file. A cell is KILLED when the verifier raises the mutant's intended
//! finding and every other FAIL finding lies inside the mutant's explained set;
//! ACCIDENT when it is caught only by something else; SURVIVED when nothing
//! fires; BLIND when the mutant's written values equal the honest ones (then
//! the receipt states whether AT1_SPEC section 9.2 predicts that blindness).

use crate::case::{validate_bytes, Case};
use crate::corpus::valid_specs;
use crate::model::{shadow, Mods, Status};
use crate::rat::Q;
use crate::result::{Ck, Kind, RVal, ResultFile};
use crate::synth::{engine_result, oracle_record, splice, EngineMut, OracleMut};
use crate::verify::{verify, Role, Sev};

pub fn engine_mut(name: &str) -> Option<EngineMut> {
    let mut e = EngineMut::default();
    match name {
        "honest" => {}
        "M1-drop-v" => e.physics.drop_v = true,
        "M2-flip-v" => e.physics.flip_v = true,
        "M3-wrong-level" => e.physics.shift_level = true,
        "M4-phase-sign" => e.physics.phase_minus = true,
        "M5-y-sign" => e.y_sign = true,
        "M6-axis-swap" => e.axis_swap = true,
        "M8-ideal-marginal" => e.ideal_marginal = true,
        "M9-stale-label" => e.stale_label = true,
        "M10-bound-none-nonzero" => e.bound_kind_none_nonzero = true,
        _ => return None,
    }
    Some(e)
}

pub fn oracle_mut(name: &str) -> Option<OracleMut> {
    let mut o = OracleMut::default();
    match name {
        "honest" => {}
        "O1-ideal-without-h" => o.ideal_drop_h = true,
        "O2-oracle-drop-v" => o.physics = Mods { drop_v: true, ..Mods::default() },
        "O3-oracle-phase-sign" => o.physics = Mods { phase_minus: true, ..Mods::default() },
        _ => return None,
    }
    Some(o)
}

const PHYSICS: [&str; 7] = ["M1-drop-v", "M2-flip-v", "M3-wrong-level", "M4-phase-sign", "M5-y-sign", "M6-axis-swap", "M8-ideal-marginal"];
const ORACLE: [&str; 3] = ["O1-ideal-without-h", "O2-oracle-drop-v", "O3-oracle-phase-sign"];

/// AT1_SPEC 9.2 blindness for M1 to M4 on the section 13 positive cases (P-class
/// of the path). M5, M6 and M8 blindness follows from the honest values
/// (no label with P(Y+) != 1/2; P(X+) = P(Y+) on every label; p(k) = w/N).
fn spec_blind(m: &str, path: &str) -> Option<bool> {
    let cls = if path.contains("P1a") || path.contains("P1b") {
        "P1"
    } else if path.contains("P1c") {
        "P1c"
    } else if path.contains("P6") || path.contains("N1f") {
        "P6"
    } else {
        "other"
    };
    match m {
        "M1-drop-v" | "M2-flip-v" => Some(cls == "P1" || cls == "P1c"),
        "M3-wrong-level" => Some(cls == "P1"),
        "M4-phase-sign" => Some(cls == "P6"),
        _ => None,
    }
}

fn honest_engine_derived_blind(m: &str, c: &Case) -> bool {
    let sh = shadow(c, &Mods::default(), false);
    if sh.trivial {
        return true;
    }
    let half = Q::frac(1, 2);
    let wn = c.w.div(&Q::int(c.n as i64));
    match m {
        "M5-y-sign" => (0..c.m).all(|k| sh.pauli[k].as_ref().map_or(true, |p| p[1][0].q == half)),
        "M6-axis-swap" => (0..c.m).all(|k| sh.pauli[k].as_ref().map_or(true, |p| p[0][0].q == p[1][0].q)),
        "M8-ideal-marginal" => (0..c.m).all(|k| sh.p[k].q == wn),
        _ => false,
    }
}

fn engine_values_equal(a: &ResultFile, b: &ResultFile) -> bool {
    a.kernel_dim == b.kernel_dim && a.constraint == b.constraint && a.povm == b.povm && a.label == b.label && a.clock == b.clock && a.pauli == b.pauli && a.bound_kind == b.bound_kind
}
fn oracle_values_equal(a: &ResultFile, b: &ResultFile) -> bool {
    a.ref_label == b.ref_label && a.ref_clock == b.ref_clock && a.ref_ideal == b.ref_ideal && a.ref_inter == b.ref_inter
}

struct Cell {
    mutant: String,
    case: String,
    runner_codes: String,
    findings: Vec<String>,
    intended: String,
    verdict: String,
}

fn judge_cell(fails: &[String], flagged: &[String], intended: &[&str], allowed: &[&str]) -> &'static str {
    let ok = |f: &String| intended.iter().any(|i| f == i) || allowed.iter().any(|a| if a.ends_with('*') { f.starts_with(&a[..a.len() - 1]) } else { f == a });
    let hit = fails.iter().chain(flagged.iter()).any(|f| intended.iter().any(|i| f == i));
    if fails.is_empty() && flagged.is_empty() {
        "SURVIVED"
    } else if hit && fails.iter().all(ok) && flagged.iter().all(ok) {
        if fails.iter().any(|f| intended.iter().any(|i| f == i)) { "KILLED" } else { "FLAGGED" }
    } else {
        "ACCIDENT"
    }
}

fn codes_of(r: &ResultFile) -> String {
    if r.verdict.failure_codes.is_empty() { "none".into() } else { r.verdict.failure_codes.join(",") }
}

pub fn receipt(out: &str) -> i32 {
    let mut cells: Vec<Cell> = Vec::new();
    let specs = valid_specs();
    let cases: Vec<(String, Case, Vec<u8>)> = specs
        .iter()
        .map(|(p, _, s)| {
            let b = s.bytes();
            (p.clone(), validate_bytes(&b).expect("public case valid"), b)
        })
        .collect();
    let honest_o = |c: &Case| oracle_record(c, &OracleMut::default(), "at1-eval synth-oracle honest");
    let honest_e = |c: &Case| engine_result(c, &EngineMut::default(), "at1-eval synth-engine honest");
    // ---- physics mutants of the engine, on every positive case and the N1 family ----
    for (path, c, cb) in cases.iter().filter(|(p, _, _)| p.starts_with("positive/") || p.contains("/N1")) {
        let o = honest_o(c);
        let he = honest_e(c);
        for m in PHYSICS {
            let em = engine_mut(m).unwrap();
            let e = engine_result(c, &em, &format!("at1-eval synth-engine {}", m));
            let r = splice(&e, &o).expect("splice");
            let rep = verify(&r.bytes(), Some(cb), Role::Candidate);
            let fails: Vec<String> = rep.findings.iter().filter(|f| f.sev == Sev::Fail).map(|f| f.code.clone()).collect();
            let flagged: Vec<String> = rep.findings.iter().filter(|f| f.sev != Sev::Fail).map(|f| f.code.clone()).collect();
            let code_ok = if c.target_ideal {
                r.verdict.failure_codes.iter().any(|x| x == "ORACLE_DISAGREEMENT" || x == "TRIVIAL_PHYSICAL_STATE")
            } else {
                r.verdict.failure_codes.iter().any(|x| x == "INTERACTING_DEVIATION_EXCEEDED" || x == "TRIVIAL_PHYSICAL_STATE")
            };
            let intended = if c.target_ideal { "EXPECTATION-NOT-MET via check 11 ORACLE_DISAGREEMENT (or check 3)" } else { "EXPECTATION-NOT-MET via check 10 INTERACTING_DEVIATION_EXCEEDED (or check 3)" };
            let trivial_claim = rep.codes().iter().any(|x| x == "E4-SHADOW-TRIVIAL");
            let mut allowed: Vec<&str> = vec!["E4-SHADOW-*"];
            if trivial_claim {
                allowed.extend(["E4-CHECK-MISMATCH", "E4-OUTCOME-MISMATCH", "E4-CODES-MISMATCH", "E4-EXPECTATION-MISMATCH", "E4-UNDEFINED-DISCIPLINE"]);
            }
            let blind = engine_values_equal(&e, &he);
            let expected_blind = spec_blind(m, path).unwrap_or_else(|| honest_engine_derived_blind(m, c));
            let verdict = if blind {
                if expected_blind { "BLIND (predicted)".to_string() } else { "BLIND (NOT predicted)".to_string() }
            } else {
                let v = judge_cell(&fails, &flagged, &["E4-EXPECTATION-NOT-MET"], &allowed);
                let v = if v == "KILLED" && !code_ok { "ACCIDENT" } else { v };
                if expected_blind { format!("{} (spec predicted blind)", v) } else { v.to_string() }
            };
            cells.push(Cell { mutant: m.into(), case: path.clone(), runner_codes: codes_of(&r), findings: rep.codes(), intended: intended.into(), verdict });
        }
    }
    // ---- engine mutants with a single target case ----
    let find = |p: &str| cases.iter().find(|(x, _, _)| x.contains(p)).expect("case present");
    for (m, cpath, intended, allowed) in [
        ("M9-stale-label", "N3-zero-label", "E4-LABEL-RULE", vec!["E4-EXPECTATION-NOT-MET", "E4-SHADOW-*"]),
        ("M10-bound-none-nonzero", "P2-kat", "E4-BOUND-NONE-NONZERO", vec!["E4-EXPECTATION-NOT-MET"]),
    ] {
        let (path, c, cb) = find(cpath);
        let e = engine_result(c, &engine_mut(m).unwrap(), &format!("at1-eval synth-engine {}", m));
        let r = splice(&e, &honest_o(c)).unwrap();
        let rep = verify(&r.bytes(), Some(cb), Role::Candidate);
        let fails: Vec<String> = rep.findings.iter().filter(|f| f.sev == Sev::Fail).map(|f| f.code.clone()).collect();
        let flagged: Vec<String> = rep.findings.iter().filter(|f| f.sev != Sev::Fail).map(|f| f.code.clone()).collect();
        let v = judge_cell(&fails, &flagged, &[intended], &allowed);
        cells.push(Cell { mutant: m.into(), case: path.clone(), runner_codes: codes_of(&r), findings: rep.codes(), intended: intended.into(), verdict: v.into() });
    }
    // ---- oracle mutants: the oracle record itself (gate G3 role) ----
    for (path, c, cb) in cases.iter() {
        let ho = honest_o(c);
        for m in ORACLE {
            let o = oracle_record(c, &oracle_mut(m).unwrap(), &format!("at1-eval synth-oracle {}", m));
            let rep = verify(&o.bytes(), Some(cb), Role::Oracle);
            let fails: Vec<String> = rep.findings.iter().filter(|f| f.sev == Sev::Fail).map(|f| f.code.clone()).collect();
            let flagged: Vec<String> = rep.findings.iter().filter(|f| f.sev != Sev::Fail).map(|f| f.code.clone()).collect();
            let (intended, allowed): (&[&str], Vec<&str>) = match m {
                "O1-ideal-without-h" => (&["E4-ORACLE-IDEAL"], vec![]),
                _ => (&["E4-ORACLE-INTERACTING", "E4-ORACLE-CLOCK", "E4-ORACLE-LABEL", "E4-ORACLE-TRIVIAL"], vec!["E4-SHADOW-*", "E4-ORACLE-*", "E4-CHECK-MISMATCH", "E4-OUTCOME-MISMATCH", "E4-CODES-MISMATCH", "E4-EXPECTATION-MISMATCH", "E4-UNDEFINED-DISCIPLINE", "E4-TRIVIAL-SHAPE"]),
            };
            let blind = oracle_values_equal(&o, &ho);
            let verdict = if blind { "BLIND (values equal the honest oracle's)".to_string() } else { judge_cell(&fails, &flagged, intended, &allowed).to_string() };
            cells.push(Cell { mutant: m.into(), case: path.clone(), runner_codes: codes_of(&o), findings: rep.codes(), intended: intended.join("|"), verdict });
        }
    }
    // ---- result-file mutants on honest spliced results ----
    let honest_spliced = |p: &str| {
        let (path, c, cb) = find(p);
        (path.clone(), splice(&honest_e(c), &honest_o(c)).unwrap(), cb.clone())
    };
    let mut file_cells: Vec<(&str, &str, &str, Box<dyn Fn(&mut ResultFile) -> Option<Vec<u8>>>)> = Vec::new();
    file_cells.push(("F1-wrong-evidence-digest", "P2-kat", "E4-EVIDENCE-DIGEST", Box::new(|r: &mut ResultFile| {
        let mut b = r.evidence_digest.clone().into_bytes();
        b[0] = if b[0] == b'0' { b'1' } else { b'0' };
        r.evidence_digest = String::from_utf8(b).unwrap();
        Some(r.bytes())
    })));
    file_cells.push(("F2-wrong-verdict-id", "P2-kat", "E4-VERDICT-ID", Box::new(|r: &mut ResultFile| {
        let mut b = r.verdict_id.clone().into_bytes();
        b[5] = if b[5] == b'0' { b'1' } else { b'0' };
        r.verdict_id = String::from_utf8(b).unwrap();
        r.evidence_digest = r.compute_evidence();
        Some(r.bytes())
    })));
    file_cells.push(("F3-dropped-line", "P2-kat", "E4-PARSE", Box::new(|r: &mut ResultFile| {
        let mut l = r.lines();
        let i = l.iter().position(|x| x.starts_with("pauli 1 Y PLUS ")).unwrap();
        l.remove(i);
        Some(crate::text::join_lf(&l))
    })));
    file_cells.push(("F4-outcome-lie", "N1-p2", "E4-OUTCOME-MISMATCH", Box::new(|r: &mut ResultFile| {
        r.verdict.outcome = "PASS".into();
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F5-codes-lie", "N4a", "E4-CODES-MISMATCH", Box::new(|r: &mut ResultFile| {
        r.verdict.failure_codes.retain(|x| x != "PROBABILITY_SUM_EXCEEDED");
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F6-check-lie", "N1-p2", "E4-CHECK-MISMATCH", Box::new(|r: &mut ResultFile| {
        r.verdict.checks[9] = Ck::Pass;
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F7-expectation-lie", "P2-kat", "E4-EXPECTATION-MISMATCH", Box::new(|r: &mut ResultFile| {
        r.verdict.expectation = "NO".into();
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F8-oracle-is-engine", "P2-kat", "E4-PROV-ORACLE-IS-ENGINE", Box::new(|r: &mut ResultFile| {
        r.prov.oracle_sha256 = r.prov.engine_sha256.clone();
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F10-time-order", "P2-kat", "E4-PROV-TIME-ORDER", Box::new(|r: &mut ResultFile| {
        r.prov.run_started = "2026-10-09T00:00:01Z".into();
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F11-case-sha", "P2-kat", "E4-CASE-SHA", Box::new(|r: &mut ResultFile| {
        r.case_file_sha256 = crate::sha256::sha256_hex(b"not the case");
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F12-undefined-on-defined", "P2-kat", "E4-UNDEFINED-DISCIPLINE", Box::new(|r: &mut ResultFile| {
        r.pauli[1][2][0] = RVal::undefined();
        let nt = !crate::verify::claims_trivial(r);
        r.verdict = crate::judge::rederive(r, nt);
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F13-trivial-shape", "N2-every", "E4-TRIVIAL-SHAPE", Box::new(|r: &mut ResultFile| {
        r.povm = RVal::undefined();
        let nt = !crate::verify::claims_trivial(r);
        r.verdict = crate::judge::rederive(r, nt);
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F14-nonfinite-value", "P2-kat", "E4-EXPECTATION-NOT-MET", Box::new(|r: &mut ResultFile| {
        r.pauli[0][0][0] = RVal { kind: Kind::Nonfinite, bound: r.pauli[0][0][0].bound.clone() };
        let nt = !crate::verify::claims_trivial(r);
        r.verdict = crate::judge::rederive(r, nt);
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F15-dirty-tree", "P2-kat", "E4-PROV-DIRTY-TREE", Box::new(|r: &mut ResultFile| {
        r.prov.source_tree_clean = "NO".into();
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F16-unknown-contract", "P2-kat", "E4-PROV-CONTRACT-UNKNOWN", Box::new(|r: &mut ResultFile| {
        r.prov.contract_commit = "1111111111111111111111111111111111111111".into();
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F17-wrong-kernel-dim", "P2-kat", "E4-SHADOW-KERNEL-DIM", Box::new(|r: &mut ResultFile| {
        r.kernel_dim = 3;
        r.seal();
        Some(r.bytes())
    })));
    file_cells.push(("F18-label-status-lie", "N3-zero", "E4-LABEL-RULE", Box::new(|r: &mut ResultFile| {
        // label 2 (p = 0) claimed DEFINED with label 1's Pauli values
        r.label[2] = Status::Defined;
        r.pauli[2] = r.pauli[1].clone();
        let nt = !crate::verify::claims_trivial(r);
        r.verdict = crate::judge::rederive(r, nt);
        r.seal();
        Some(r.bytes())
    })));
    for (m, cp, intended, f) in file_cells.iter() {
        let (path, mut r, cb) = honest_spliced(cp);
        let b = f(&mut r).unwrap();
        let rep = verify(&b, Some(&cb), Role::Candidate);
        let fails: Vec<String> = rep.findings.iter().filter(|f| f.sev == Sev::Fail).map(|f| f.code.clone()).collect();
        let flagged: Vec<String> = rep.findings.iter().filter(|f| f.sev != Sev::Fail).map(|f| f.code.clone()).collect();
        let allowed: Vec<&str> = match *m {
            "F12-undefined-on-defined" | "F18-label-status-lie" => vec!["E4-EXPECTATION-NOT-MET", "E4-SHADOW-LABEL"],
            _ => vec![],
        };
        let v = judge_cell(&fails, &flagged, &[intended], &allowed);
        let rc = crate::result::parse(&b).map(|x| codes_of(&x)).unwrap_or_else(|_| "unparsed".into());
        cells.push(Cell { mutant: m.to_string(), case: path, runner_codes: rc, findings: rep.codes(), intended: intended.to_string(), verdict: v.into() });
    }
    // F9: an oracle record handed in as a candidate result
    {
        let (path, c, cb) = find("P2-kat");
        let o = honest_o(c);
        let rep = verify(&o.bytes(), Some(cb), Role::Candidate);
        let fails: Vec<String> = rep.findings.iter().filter(|f| f.sev == Sev::Fail).map(|f| f.code.clone()).collect();
        let flagged: Vec<String> = rep.findings.iter().filter(|f| f.sev != Sev::Fail).map(|f| f.code.clone()).collect();
        let v = judge_cell(&fails, &flagged, &["E4-PROV-ORACLE-RECORD"], &[]);
        cells.push(Cell { mutant: "F9-oracle-record-as-candidate".into(), case: path.clone(), runner_codes: codes_of(&o), findings: rep.codes(), intended: "E4-PROV-ORACLE-RECORD".into(), verdict: v.into() });
    }
    // ---- honest controls: every valid public case must verify PASS with no finding ----
    let mut honest_bad = 0;
    for (path, c, cb) in cases.iter() {
        let r = splice(&honest_e(c), &honest_o(c)).unwrap();
        let rep = verify(&r.bytes(), Some(cb), Role::Candidate);
        let ro = verify(&honest_o(c).bytes(), Some(cb), Role::Oracle);
        let v = if rep.status() == "PASS" && ro.status() == "PASS" { "PASS" } else { honest_bad += 1; "FAIL" };
        let mut f = rep.codes();
        f.extend(ro.codes().into_iter().map(|x| format!("oracle:{}", x)));
        cells.push(Cell { mutant: "honest".into(), case: path.clone(), runner_codes: codes_of(&r), findings: f, intended: "no finding".into(), verdict: v.into() });
    }
    // ---- write ----
    let mut s = String::from("# AT-1 Agent 4 mutant kill receipt (generated by `at1-eval mutants`; deterministic)\n# mutant\tcase\trunner_failure_codes\tverifier_findings\tintended\tverdict\n");
    let mut bad = honest_bad;
    let mut tally: std::collections::BTreeMap<String, (usize, usize, usize, usize)> = Default::default();
    for c in &cells {
        s.push_str(&format!("{}\t{}\t{}\t{}\t{}\t{}\n", c.mutant, c.case, c.runner_codes, if c.findings.is_empty() { "none".into() } else { c.findings.join(",") }, c.intended, c.verdict));
        let t = tally.entry(c.mutant.clone()).or_default();
        if c.mutant == "honest" {
            continue;
        }
        if c.verdict.starts_with("KILLED") || c.verdict.starts_with("FLAGGED") {
            t.0 += 1;
        } else if c.verdict == "BLIND (predicted)" || c.verdict.starts_with("BLIND (values") {
            t.1 += 1;
        } else {
            t.2 += 1;
            bad += 1;
        }
        t.3 += 1;
    }
    s.push_str("# summary: mutant\tkilled_or_flagged\tblind_explained\tunexpected\tcells\n");
    for (m, t) in &tally {
        if m == "honest" {
            continue;
        }
        s.push_str(&format!("# {}\t{}\t{}\t{}\t{}\n", m, t.0, t.1, t.2, t.3));
    }
    s.push_str(&format!("# honest controls failing: {}\n# MUTANT_RECEIPT: {}\n", honest_bad, if bad == 0 { "PASS" } else { "FAIL" }));
    if let Some(parent) = std::path::Path::new(out).parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    std::fs::write(out, s.as_bytes()).expect("write receipt");
    println!("MUTANT_RECEIPT: {} ({} cells, {} unexpected) -> {}", if bad == 0 { "PASS" } else { "FAIL" }, cells.len(), bad, out);
    if bad == 0 { 0 } else { 1 }
}
