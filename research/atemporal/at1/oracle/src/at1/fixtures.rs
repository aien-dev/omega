//! Calibration fixtures: every case of AT1_SPEC.md section 13 (classes P1 to P6, the N1
//! family, N2 to N6) built from exact parameters, with the hand-derived values of the
//! spec's tables as expectations. Values are copied from the spec at `SPEC_COMMIT`, written
//! here as exact fractions; nothing here is copied from a run of this oracle.

use super::big::{Int, Nat, Q, GQ};
use super::case::{parse_case, Spec};
use super::result::{compute, Computed};

pub fn q(n: i64, d: i64) -> Q {
    Q::frac(n, d)
}
pub fn qi(n: i64) -> Q {
    Q::int(n)
}
fn gz(re: Q, im: Q) -> GQ {
    GQ::new(re, im)
}
/// Exact fraction to binary64 (both parts below 2^53, so one correctly rounded division).
pub fn fr(n: i64, d: i64) -> f64 {
    n as f64 / d as f64
}

/// One label row of a hand table: p(k), P(X+), P(Y+), P(Z+); None for an UNDEFINED label.
pub type Row = Option<[f64; 4]>;

pub struct Fixture {
    pub name: &'static str,
    pub class: &'static str,
    pub spec: Spec,
    pub kernel_dim: usize,
    pub trivial: bool,
    /// Interacting table (empty for a trivial kernel).
    pub rows: Vec<Row>,
    /// Ideal reference P(X+), P(Y+), P(Z+) per label.
    pub ideal: Vec<[f64; 3]>,
    /// Exact povm_residual where the spec gives it.
    pub povm: Option<f64>,
}

fn tol() -> [String; 5] {
    ["1@12".into(), "1@12".into(), "1@12".into(), "1@9".into(), "1@12".into()]
}

fn zero3() -> [Q; 3] {
    [Q::zero(), Q::zero(), Q::zero()]
}
fn rho() -> [Q; 3] {
    [q(3, 10), Q::zero(), q(-1, 10)]
}

/// Base clock B of AT1_SPEC section 13 with the given couplings.
fn base(name: &str, v: Vec<[Q; 3]>) -> Spec {
    Spec {
        name: name.to_string(),
        energies: vec![q(-3, 2), q(-1, 2), q(1, 2), q(3, 2)],
        hpauli: [Q::zero(), Q::zero(), Q::zero(), q(1, 2)],
        v,
        ref_label: 0,
        psi: [gz(qi(1), Q::zero()), gz(qi(1), Q::zero())],
        tau: q(1, 4),
        weight: qi(1),
        labels: 4,
        control_kind: "POSITIVE".into(),
        target: "INTERACTING".into(),
        expected_outcome: "PASS".into(),
        expected_codes: vec![],
        min_bound_kind: "ESTIMATED".into(),
        tol: tol(),
    }
}

fn negative(mut s: Spec, name: &str, codes: &[&str]) -> Spec {
    s.name = name.to_string();
    s.control_kind = "NEGATIVE".into();
    s.expected_outcome = "FAIL".into();
    s.expected_codes = codes.iter().map(|c| c.to_string()).collect();
    s
}

fn ideal_target(mut s: Spec, name: &str) -> Spec {
    s.name = name.to_string();
    s.target = "IDEAL".into();
    s
}

/// Ideal table of P1 (|+x> rotated about z by pi k / 2).
fn p1_ideal() -> Vec<[f64; 3]> {
    vec![[1.0, 0.5, 0.5], [0.5, 1.0, 0.5], [0.0, 0.5, 0.5], [0.5, 0.0, 0.5]]
}

fn rows(t: &[[(i64, i64); 4]]) -> Vec<Row> {
    t.iter().map(|r| Some([fr(r[0].0, r[0].1), fr(r[1].0, r[1].1), fr(r[2].0, r[2].1), fr(r[3].0, r[3].1)])).collect()
}

fn p2_spec() -> Spec {
    base("at1-kat-rotated-level-n4", vec![zero3(), zero3(), rho(), zero3()])
}
fn p2_rows() -> Vec<Row> {
    rows(&[
        [(5, 28), (49, 50), (1, 2), (16, 25)],
        [(1, 4), (29, 70), (13, 14), (26, 35)],
        [(9, 28), (1, 10), (1, 2), (4, 5)],
        [(1, 4), (29, 70), (1, 14), (26, 35)],
    ])
}

fn p3_spec() -> Spec {
    let mut s = base("at1-p3-every-level-coupled", vec![rho(), [Q::zero(), q(3, 10), q(-1, 10)], [q(6, 5), Q::zero(), q(2, 5)]]);
    s.energies = vec![q(-1, 2), q(1, 2), q(3, 2)];
    s.weight = q(3, 4);
    s
}
fn p3b_spec() -> Spec {
    let mut s = base("at1-p3b-degenerate-level", vec![zero3(), [Q::zero(), Q::zero(), q(-1, 2)], rho()]);
    s.energies = vec![q(-1, 2), Q::zero(), q(1, 2)];
    s.psi = [gz(qi(1), Q::zero()), gz(Q::zero(), qi(1))];
    s.tau = q(1, 2);
    s.weight = q(3, 4);
    s
}
fn p4_spec() -> Spec {
    let mut s = p2_spec();
    s.name = "at1-p4-complex-state-ref-t1".into();
    s.psi = [gz(qi(1), Q::zero()), gz(q(3, 5), q(4, 5))];
    s.ref_label = 1;
    s
}
fn p5_spec() -> Spec {
    let mut s = base("at1-p5-large-rationals", vec![zero3(), [q(524175, 1048354), q(724, 524177), q(-1, 2)]]);
    s.energies = vec![q(-524289, 1048574), q(524285, 1048574)];
    s.hpauli = [q(1, 524287), Q::zero(), Q::zero(), q(1, 2)];
    s.weight = q(1, 2);
    s
}
fn p6_spec() -> Spec {
    base("at1-p6-near-miss-level", vec![[Q::zero(), Q::zero(), qi(1)], [Q::zero(), Q::zero(), q(1, 1048576)], rho(), zero3()])
}

pub fn all() -> Vec<Fixture> {
    let p1 = base("at1-p1a-zero-coupling", vec![zero3(), zero3(), zero3(), zero3()]);
    let p1_rows = rows(&[[(1, 4), (1, 1), (1, 2), (1, 2)], [(1, 4), (1, 2), (1, 1), (1, 2)], [(1, 4), (0, 1), (1, 2), (1, 2)], [(1, 4), (1, 2), (0, 1), (1, 2)]]);
    let p1c = ideal_target(base("at1-p1c-spectator-coupling", vec![[Q::zero(), Q::zero(), q(1, 2)], zero3(), zero3(), rho()]), "at1-p1c-spectator-coupling");
    let p3_rows = rows(&[
        [(107, 280), (98, 107), (65, 214), (65, 214)],
        [(53, 280), (37, 53), (101, 106), (61, 106)],
        [(5, 56), (8, 25), (37, 50), (9, 10)],
        [(19, 56), (37, 95), (17, 190), (29, 38)],
    ]);
    let p3b_rows = rows(&[
        [(41, 80), (29, 82), (40, 41), (45, 82)],
        [(19, 80), (1, 38), (10, 19), (25, 38)],
        [(1, 80), (1, 2), (0, 1), (1, 2)],
        [(19, 80), (37, 38), (10, 19), (13, 38)],
    ]);
    let p3b_ideal = vec![[0.5, 1.0, 0.5], [0.5, 0.0, 0.5], [0.5, 1.0, 0.5], [0.5, 0.0, 0.5]];
    let p4_rows = rows(&[
        [(29, 164), (277, 290), (17, 58), (73, 145)],
        [(37, 164), (197, 370), (73, 74), (113, 185)],
        [(53, 164), (37, 530), (65, 106), (193, 265)],
        [(45, 164), (13, 50), (1, 10), (17, 25)],
    ]);
    let p4_ideal = vec![[0.9, 0.2, 0.5], [0.8, 0.9, 0.5], [0.1, 0.8, 0.5], [0.2, 0.1, 0.5]];
    let p5_rows = rows(&[
        [(524181, 2096716), (274763624041, 549527248074), (274004612845, 549527248074), (524180, 524181)],
        [(522731, 2096716), (274761527333, 548007134774), (274004612845, 548007134774), (522730, 522731)],
        [(524177, 2096716), (274759430625, 549523054658), (275520532729, 549523054658), (524176, 524177)],
        [(525627, 2096716), (274761527333, 551043167958), (275520532729, 551043167958), (525626, 525627)],
    ]);
    let p6_rows = rows(&[
        [(5, 28), (49, 50), (1, 2), (16, 25)],
        [(9, 28), (1, 10), (1, 2), (4, 5)],
        [(5, 28), (49, 50), (1, 2), (16, 25)],
        [(9, 28), (1, 10), (1, 2), (4, 5)],
    ]);
    let sde = ["SCHRODINGER_DEVIATION_EXCEEDED"];
    let povm_sum = ["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"];
    let mut n4a = negative(p2_spec(), "at1-n4a-wrong-weight", &povm_sum);
    n4a.weight = q(1, 2);
    let n4a_rows: Vec<Row> = p2_rows().iter().map(|r| r.map(|v| [v[0] / 2.0, v[1], v[2], v[3]])).collect();
    let mut n4b = negative(p2_spec(), "at1-n4b-broken-clock-tau-1-3", &povm_sum);
    n4b.tau = q(1, 3);
    let r3 = 3.0f64.sqrt();
    let n4b_rows: Vec<Row> = vec![
        Some([fr(5, 28), fr(49, 50), 0.5, fr(16, 25)]),
        Some([fr(2, 7), fr(19, 80), 0.5 + 3.0 * r3 / 16.0, fr(31, 40)]),
        Some([fr(2, 7), fr(19, 80), 0.5 - 3.0 * r3 / 16.0, fr(31, 40)]),
        Some([fr(5, 28), fr(49, 50), 0.5, fr(16, 25)]),
    ];
    let third = |k: f64| {
        let th = 2.0 * std::f64::consts::PI * k / 3.0;
        [(1.0 + th.cos()) / 2.0, (1.0 + th.sin()) / 2.0, 0.5]
    };
    let n4b_ideal = vec![third(0.0), third(1.0), third(2.0), third(3.0)];
    let mut n5 = negative(p2_spec(), "at1-n5-rigorous-demand", &["BOUND_KIND_INSUFFICIENT"]);
    n5.min_bound_kind = "RIGOROUS".into();
    let mut n6 = negative(base("at1-n6-zero-projection", vec![zero3(), [Q::zero(), Q::zero(), q(1, 2)], rho(), zero3()]), "at1-n6-zero-projection", &["TRIVIAL_PHYSICAL_STATE"]);
    n6.psi = [gz(qi(3), Q::zero()), gz(qi(1), Q::zero())];
    let n6_ideal = vec![[0.8, 0.5, 0.9], [0.5, 0.8, 0.9], [0.2, 0.5, 0.9], [0.5, 0.2, 0.9]];
    let n3_rows: Vec<Row> = vec![Some([0.5, 0.5, 0.5, 1.0]), Some([0.25, 0.5, 0.5, 1.0]), None, Some([0.25, 0.5, 0.5, 1.0])];
    let fx = |name: &'static str, class: &'static str, spec: Spec, kernel_dim: usize, trivial: bool, rows: Vec<Row>, ideal: Vec<[f64; 3]>, povm: Option<f64>| Fixture { name, class, spec, kernel_dim, trivial, rows, ideal, povm };
    vec![
        fx("at1-p1a-zero-coupling", "P1", p1.clone(), 2, false, p1_rows.clone(), p1_ideal(), Some(0.0)),
        fx("at1-p1b-zero-coupling-ideal", "P1", ideal_target(p1, "at1-p1b-zero-coupling-ideal"), 2, false, p1_rows.clone(), p1_ideal(), Some(0.0)),
        fx("at1-p1c-spectator-coupling", "P1c", p1c, 2, false, p1_rows, p1_ideal(), Some(0.0)),
        fx("at1-kat-rotated-level-n4", "P2", p2_spec(), 2, false, p2_rows(), p1_ideal(), Some(0.0)),
        fx("at1-p3-every-level-coupled", "P3", p3_spec(), 3, false, p3_rows.clone(), p1_ideal(), None),
        fx("at1-p3b-degenerate-level", "P3b", p3b_spec(), 4, false, p3b_rows.clone(), p3b_ideal.clone(), None),
        fx("at1-p4-complex-state-ref-t1", "P4", p4_spec(), 2, false, p4_rows.clone(), p4_ideal.clone(), Some(0.0)),
        fx("at1-p5-large-rationals", "P5", p5_spec(), 2, false, p5_rows.clone(), p1_ideal(), None),
        fx("at1-p6-near-miss-level", "P6", p6_spec(), 2, false, p6_rows.clone(), p1_ideal(), Some(0.0)),
        fx("at1-n1-p2-ideal", "N1", negative(ideal_target(p2_spec(), ""), "at1-n1-p2-ideal", &sde), 2, false, p2_rows(), p1_ideal(), Some(0.0)),
        fx("at1-n1b-p3-ideal", "N1b", negative(ideal_target(p3_spec(), ""), "at1-n1b-p3-ideal", &sde), 3, false, p3_rows, p1_ideal(), None),
        fx("at1-n1c-p3b-ideal", "N1c", negative(ideal_target(p3b_spec(), ""), "at1-n1c-p3b-ideal", &sde), 4, false, p3b_rows, p3b_ideal, None),
        fx("at1-n1d-p4-ideal", "N1d", negative(ideal_target(p4_spec(), ""), "at1-n1d-p4-ideal", &sde), 2, false, p4_rows, p4_ideal, Some(0.0)),
        fx("at1-n1e-p5-ideal", "N1e", negative(ideal_target(p5_spec(), ""), "at1-n1e-p5-ideal", &sde), 2, false, p5_rows, p1_ideal(), None),
        fx("at1-n1f-p6-ideal", "N1f", negative(ideal_target(p6_spec(), ""), "at1-n1f-p6-ideal", &sde), 2, false, p6_rows, p1_ideal(), Some(0.0)),
        fx("at1-n2-all-levels-out", "N2", negative(base("", vec![zero3(), [Q::zero(), Q::zero(), q(1, 2)], [Q::zero(), Q::zero(), q(1, 2)], zero3()]), "at1-n2-all-levels-out", &["TRIVIAL_PHYSICAL_STATE"]), 0, true, vec![], p1_ideal(), Some(0.0)),
        fx("at1-n3-swapped-level-zero-label", "N3", negative(base("", vec![zero3(), zero3(), [Q::zero(), Q::zero(), qi(-1)], zero3()]), "at1-n3-swapped-level-zero-label", &["CONDITIONAL_UNDEFINED"]), 2, false, n3_rows, p1_ideal(), Some(0.0)),
        fx("at1-n4a-wrong-weight", "N4a", n4a, 2, false, n4a_rows, p1_ideal(), Some(1.0)),
        fx("at1-n4b-broken-clock-tau-1-3", "N4b", n4b, 2, false, n4b_rows, n4b_ideal, Some(42.0f64.sqrt() / 4.0)),
        fx("at1-n5-rigorous-demand", "N5", n5, 2, false, p2_rows(), p1_ideal(), Some(0.0)),
        fx("at1-n6-zero-projection", "N6", n6, 1, true, vec![], n6_ideal, Some(0.0)),
    ]
}

/// Largest absolute deviation between a computed table and the hand table, and the
/// smallest margin (bound minus actual error) over the written values, per route.
pub struct Calibration {
    pub name: &'static str,
    pub values_checked: usize,
    pub max_dev_matrix: f64,
    pub max_dev_closed: f64,
    pub max_dev_ideal: f64,
    pub bound_violations: usize,
    /// Largest bound written for any checked value (how close a bound comes to a tolerance).
    pub max_bound: f64,
}

pub fn calibrate(f: &Fixture, comp: &Computed) -> Calibration {
    let mut cal = Calibration { name: f.name, values_checked: 0, max_dev_matrix: 0.0, max_dev_closed: 0.0, max_dev_ideal: 0.0, bound_violations: 0, max_bound: 0.0 };
    let mut maxb: f64 = 0.0;
    let mut see = |dev: &mut f64, got: f64, want: f64, bound: f64, viol: &mut usize, n: &mut usize| {
        maxb = maxb.max(bound);
        let d = (got - want).abs();
        *dev = dev.max(d);
        *n += 1;
        if d > bound {
            *viol += 1;
        }
    };
    let mut viol = 0;
    let mut n = 0;
    for (k, idl) in comp.ideal.iter().enumerate() {
        for a in 0..3 {
            see(&mut cal.max_dev_ideal, idl.pauli[a][0], f.ideal[k][a], idl.bound, &mut viol, &mut n);
            see(&mut cal.max_dev_ideal, idl.pauli[a][1], 1.0 - f.ideal[k][a], idl.bound, &mut viol, &mut n);
        }
    }
    if !f.trivial {
        for (k, row) in f.rows.iter().enumerate() {
            let (ml, cl) = (&comp.matrix.labels[k], &comp.interacting[k]);
            match row {
                Some(r) => {
                    see(&mut cal.max_dev_matrix, ml.p, r[0], ml.p_bound, &mut viol, &mut n);
                    see(&mut cal.max_dev_closed, cl.p, r[0], cl.p_bound, &mut viol, &mut n);
                    for a in 0..3 {
                        let (mp, cp) = (ml.pauli.unwrap(), cl.pauli.unwrap());
                        see(&mut cal.max_dev_matrix, mp[a][0], r[a + 1], ml.pauli_bound[a][0], &mut viol, &mut n);
                        see(&mut cal.max_dev_matrix, mp[a][1], 1.0 - r[a + 1], ml.pauli_bound[a][1], &mut viol, &mut n);
                        see(&mut cal.max_dev_closed, cp[a][0], r[a + 1], cl.pauli_bound[a][0], &mut viol, &mut n);
                        see(&mut cal.max_dev_closed, cp[a][1], 1.0 - r[a + 1], cl.pauli_bound[a][1], &mut viol, &mut n);
                    }
                }
                None => {
                    // UNDEFINED label: exact p(k) = 0
                    see(&mut cal.max_dev_matrix, ml.p, 0.0, ml.p_bound, &mut viol, &mut n);
                    see(&mut cal.max_dev_closed, cl.p, 0.0, cl.p_bound, &mut viol, &mut n);
                }
            }
        }
    }
    if let Some(pv) = f.povm {
        // 1 - x is not exact for every x, so the hand value itself carries half an ulp
        see(&mut cal.max_dev_matrix, comp.matrix.povm.0, pv, comp.matrix.povm.1, &mut viol, &mut n);
    }
    cal.bound_violations = viol;
    cal.max_bound = maxb;
    cal.values_checked = n;
    cal
}

pub struct Rendered {
    pub files: Vec<(String, Vec<u8>)>,
    pub summary: Vec<String>,
}

/// Case files, values/verdict blocks and CALIBRATION.md; deterministic (no clock).
pub fn render() -> Rendered {
    let mut files = Vec::new();
    let mut summary = Vec::new();
    let mut md = String::from(
        "# AT-1 oracle calibration fixtures\n\nGenerated by `target/at1-oracle fixtures fixtures/` (deterministic, no wall clock). \
Each case is one row of AT1_SPEC.md section 13 (aien-architecture `81047f5`); expectations are the spec's exact hand tables. \
`matrix` is the literal matrix route (the record's own values), `closed` the closed-form route (`reference_clock_probability`, \
`reference_interacting`), `ideal` the Bloch-rotation route (`reference_ideal`). Deviations are absolute, against the exact value \
rounded once to binary64. `bound viol.` counts values whose actual error exceeds the written ESTIMATED bound (must be 0).\n\n\
| case | class | kernel dim | outcome | failure codes | expectation_met | values checked | max dev matrix | max dev closed | max dev ideal | bound viol. | max bound |\n\
|---|---|---|---|---|---|---|---|---|---|---|---|\n",
    );
    for f in all() {
        let bytes = f.spec.bytes();
        let c = parse_case(&bytes).expect("fixture case parses");
        let comp = compute(&c);
        let cal = calibrate(&f, &comp);
        let met = comp.verdict_lines.iter().find(|l| l.starts_with("expectation_met ")).cloned().unwrap_or_default();
        let codes = if comp.verdict.codes.is_empty() { "none".to_string() } else { comp.verdict.codes.join(",") };
        md.push_str(&format!(
            "| `{}` | {} | {} | {} | {} | {} | {} | {:.1e} | {:.1e} | {:.1e} | {} | {:.1e} |\n",
            f.name,
            f.class,
            comp.levels.kernel_dim,
            comp.verdict.outcome,
            codes,
            met.trim_start_matches("expectation_met "),
            cal.values_checked,
            cal.max_dev_matrix,
            cal.max_dev_closed,
            cal.max_dev_ideal,
            cal.bound_violations,
            cal.max_bound
        ));
        let mut vals = comp.values.join("\n");
        vals.push('\n');
        vals.push_str(&comp.verdict_lines.join("\n"));
        vals.push_str(&format!("\nverdict_id {}\n", comp.verdict_id));
        summary.push(format!("{} {} {} {}", f.name, comp.verdict.outcome, codes, met));
        files.push((format!("{}.case", f.name), bytes));
        files.push((format!("{}.values", f.name), vals.into_bytes()));
    }
    files.push(("CALIBRATION.md".to_string(), md.into_bytes()));
    Rendered { files, summary }
}

/// Unused-import guard (Int and Nat are used by callers of this module's helpers).
pub fn _int(n: i64) -> Int {
    Int::from_i64(n)
}
pub fn _nat(n: u64) -> Nat {
    Nat::from_u64(n)
}
