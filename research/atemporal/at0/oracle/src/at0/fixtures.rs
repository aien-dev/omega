//! Deterministic public calibration fixtures. Pure: returns file contents;
//! src/main.rs writes them. Expected tables are exact rationals derived by hand
//! (README.md, "How fixture expectations were derived").

use super::case::{build_case, parse_case, Acceptance};
use super::rational::Rat;
use super::result::{compute, Computed};

pub struct Fixture {
    pub name: &'static str,
    pub case: Vec<u8>,
    pub dephased: bool,
    /// per label: Some((P(X+), P(Y+), P(Z+), p(k))) where a closed form is rational
    pub expected: Option<Vec<Option<[Rat; 4]>>>,
}

fn r(n: i128, d: i128) -> Rat {
    Rat::new(n, d)
}

fn clock(n: usize) -> Vec<Rat> {
    (0..n).map(|j| r(2 * j as i128 - (n as i128 - 1), 2)).collect()
}

fn labels(m: usize) -> Vec<String> {
    (0..m).map(|k| format!("t{}", k)).collect()
}

const PLUS: [(Rat, Rat); 2] = [(Rat { n: 1, d: 1 }, Rat { n: 0, d: 1 }), (Rat { n: 1, d: 1 }, Rat { n: 0, d: 1 })];
const ZEROR: Rat = Rat { n: 0, d: 1 };

fn half() -> Rat {
    r(1, 2)
}

/// Ideal clock, H_S = Z/2 (even N) or Z (odd N), psi_0 = |+>: <X> = cos a, <Y> = sin a,
/// <Z> = 0 with a = 2|h| * 2 pi (k - ref)/N; exact only at quarter turns.
fn kat_table(n: usize, reference: usize) -> Vec<Option<[Rat; 4]>> {
    let h = if n % 2 == 0 { r(1, 2) } else { r(1, 1) };
    (0..n)
        .map(|k| {
            let turns = h.mul(&r(2, 1)).mul(&r(k as i128 - reference as i128, n as i128));
            let t = Rat::new(turns.n.rem_euclid(turns.d), turns.d);
            let cs = match (t.n, t.d) {
                (0, 1) => Some((1i128, 0i128)),
                (1, 4) => Some((0, 1)),
                (1, 2) => Some((-1, 0)),
                (3, 4) => Some((0, -1)),
                _ => None,
            };
            cs.map(|(c, s)| [r(1 + c, 2), r(1 + s, 2), half(), r(1, n as i128)])
        })
        .collect()
}

fn table(rows: &[(i128, i128, i128, i128)]) -> Vec<Option<[Rat; 4]>> {
    // rows as (2 P(X+), 2 P(Y+), 2 P(Z+), 1/p) integers for readability
    rows.iter().map(|(x, y, z, p)| Some([r(*x, 2), r(*y, 2), r(*z, 2), r(1, *p)])).collect()
}

pub fn catalogue() -> Vec<Fixture> {
    let pos = Acceptance::positive();
    let neg = |codes: &[&'static str]| Acceptance::negative(codes);
    let zhalf = [ZEROR, ZEROR, ZEROR, half()];
    let l4: Vec<&str> = vec!["t0", "t1", "t2", "t3"];
    let mut out = Vec::new();
    for n in [4usize, 6, 8, 3, 5] {
        let pauli = if n % 2 == 0 { zhalf } else { [ZEROR, ZEROR, ZEROR, r(1, 1)] };
        let name: &'static str = match n {
            4 => "at0-kat-ideal-qubit-n4",
            6 => "p1-ideal-n6",
            8 => "p1-ideal-n8",
            3 => "p1-ideal-n3",
            _ => "p1-ideal-n5",
        };
        let lb = labels(n);
        let lbs: Vec<&str> = lb.iter().map(String::as_str).collect();
        out.push(Fixture { name, case: build_case(name, &clock(n), pauli, "t0", PLUS, r(1, n as i128), r(1, 1), &lbs, &pos), dephased: false, expected: Some(kat_table(n, 0)) });
    }
    out.push(Fixture { name: "p1-offset-ref-t2", case: build_case("p1-offset-ref-t2", &clock(4), zhalf, "t2", PLUS, r(1, 4), r(1, 1), &l4, &pos), dephased: false, expected: Some(kat_table(4, 2)) });
    out.push(Fixture { name: "p1-global-phase-i", case: build_case("p1-global-phase-i", &clock(4), zhalf, "t0", [(ZEROR, r(1, 1)), (ZEROR, r(1, 1))], r(1, 4), r(1, 1), &l4, &pos), dephased: false, expected: Some(kat_table(4, 0)) });
    out.push(Fixture { name: "p1-unnormalized-2", case: build_case("p1-unnormalized-2", &clock(4), zhalf, "t0", [(r(2, 1), ZEROR), (r(2, 1), ZEROR)], r(1, 4), r(1, 1), &l4, &pos), dephased: false, expected: Some(kat_table(4, 0)) });
    out.push(Fixture { name: "p2-tilted-3-10-2-5", case: build_case("p2-tilted-3-10-2-5", &clock(4), [ZEROR, r(3, 10), ZEROR, r(2, 5)], "t0", PLUS, r(1, 4), r(1, 1), &l4, &pos), dephased: false, expected: None });
    out.push(Fixture { name: "p2-h0-shift", case: build_case("p2-h0-shift", &[r(-1, 1), ZEROR, r(1, 1), r(2, 1)], [half(), r(3, 10), ZEROR, r(2, 5)], "t0", PLUS, r(1, 4), r(1, 1), &l4, &pos), dephased: false, expected: None });
    out.push(Fixture { name: "n1-uncovered", case: build_case("n1-uncovered", &[r(1, 1), r(2, 1), r(3, 1), r(4, 1)], zhalf, "t0", PLUS, r(1, 4), r(1, 1), &l4, &neg(&["TRIVIAL_PHYSICAL_STATE"])), dephased: false, expected: None });
    out.push(Fixture { name: "n2-half-covered", case: build_case("n2-half-covered", &[r(1, 2), r(3, 2), r(5, 2), r(7, 2)], zhalf, "t0", PLUS, r(1, 4), r(1, 1), &l4, &neg(&["SCHRODINGER_DEVIATION_EXCEEDED"])), dephased: false, expected: None });
    out.push(Fixture { name: "n4-wrong-weight", case: build_case("n4-wrong-weight", &clock(4), zhalf, "t0", PLUS, r(1, 4), r(2, 1), &l4, &neg(&["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"])), dephased: false, expected: None });
    out.push(Fixture { name: "n3-broken-clock-tau-1-3", case: build_case("n3-broken-clock-tau-1-3", &clock(4), zhalf, "t0", PLUS, r(1, 3), r(1, 1), &l4, &neg(&["POVM_NORMALIZATION_EXCEEDED"])), dephased: false, expected: None });
    let mut rig = neg(&["BOUND_KIND_INSUFFICIENT"]);
    rig.min_bound_kind = "RIGOROUS";
    out.push(Fixture { name: "n5-rigorous-demand", case: build_case("n5-rigorous-demand", &clock(4), zhalf, "t0", PLUS, r(1, 4), r(1, 1), &l4, &rig), dephased: false, expected: None });
    // AT0_SPEC.md at SPEC_COMMIT (section 12 mapping, section 13.2 P1c): two-level clock, H_S = diag(0, 1), omega = 1, phi = 0.
    let spec_e = [r(-1, 1), ZEROR];
    let spec_h = [half(), ZEROR, ZEROR, r(-1, 2)];
    out.push(Fixture { name: "spec-ref-model-n4", case: build_case("spec-ref-model-n4", &spec_e, spec_h, "t0", PLUS, r(1, 4), half(), &l4, &pos), dephased: false, expected: Some(table(&[(2, 1, 1, 4), (1, 0, 1, 4), (0, 1, 1, 4), (1, 2, 1, 4)])) });
    out.push(Fixture { name: "spec-t4-eigenstate-00", case: build_case("spec-t4-eigenstate-00", &spec_e, spec_h, "t0", [(r(1, 1), ZEROR), (ZEROR, ZEROR)], r(1, 4), half(), &l4, &pos), dephased: false, expected: Some(table(&[(1, 1, 2, 4); 4])) });
    out.push(Fixture { name: "spec-t5-relational-phase", case: build_case("spec-t5-relational-phase", &spec_e, spec_h, "t0", [(r(1, 1), ZEROR), (ZEROR, r(1, 1))], r(1, 4), half(), &l4, &pos), dephased: false, expected: Some(table(&[(1, 2, 1, 4), (2, 1, 1, 4), (1, 0, 1, 4), (0, 1, 1, 4)])) });
    // AT0_SPEC.md section 13.2 P2: h0 = 1/10, h = (3/10, 0, 2/5), |h| = 1/2, clock E = -3/5, 2/5, psi_0 = |0>,
    // tau = 1/4, w = 1/2, M = 4. Bloch vector rotates about n = (3/5, 0, 4/5) by k pi / 2 (Rodrigues):
    // k=1: (12/25, -3/5, 16/25), k=2: (24/25, 0, 7/25), k=3: (12/25, 3/5, 16/25); P(A+) = (1 + r_A)/2.
    let p2x = |x: Rat, y: Rat, z: Rat| Some([x, y, z, r(1, 4)]);
    out.push(Fixture {
        name: "spec-p2-tilted-h0",
        case: build_case("spec-p2-tilted-h0", &[r(-3, 5), r(2, 5)], [r(1, 10), r(3, 10), ZEROR, r(2, 5)], "t0", [(r(1, 1), ZEROR), (ZEROR, ZEROR)], r(1, 4), half(), &l4, &pos),
        dephased: false,
        expected: Some(vec![p2x(half(), half(), r(1, 1)), p2x(r(37, 50), r(1, 5), r(41, 50)), p2x(r(49, 50), half(), r(16, 25)), p2x(r(37, 50), r(4, 5), r(41, 50))]),
    });
    out.push(Fixture { name: "spec-n3-wrong-weight", case: build_case("spec-n3-wrong-weight", &spec_e, spec_h, "t0", PLUS, r(1, 4), r(1, 4), &l4, &neg(&["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"])), dephased: false, expected: None });
    out.push(Fixture { name: "spec-n4-dropped-effect", case: build_case("spec-n4-dropped-effect", &spec_e, spec_h, "t0", PLUS, r(1, 4), half(), &l4[..3], &neg(&["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"])), dephased: false, expected: None });
    out.push(Fixture { name: "nc-dephased-control", case: build_case("nc-dephased-control", &clock(4), zhalf, "t0", PLUS, r(1, 4), r(1, 1), &l4, &neg(&["SCHRODINGER_DEVIATION_EXCEEDED"])), dephased: true, expected: Some(table(&[(1, 1, 1, 4); 4])) });
    out
}

pub struct Rendered {
    pub files: Vec<(String, Vec<u8>)>,
    pub calibration: String,
    pub summary: Vec<String>,
}

fn dev(a: f64, want: &Rat) -> f64 {
    (a - want.to_f64()).abs()
}

/// Largest deviation of the computed values from the hand table (matrix path, and
/// the reference path unless dephased), and the number of exact values compared.
pub fn calibrate(f: &Fixture, comp: &Computed) -> (usize, f64) {
    let (mut checked, mut worst) = (0usize, 0.0f64);
    if let Some(table) = &f.expected {
        for (k, row) in table.iter().enumerate() {
            let (row, pk) = match (row, comp.matrix.pauli.get(k).and_then(|p| *p)) {
                (Some(row), Some(pk)) => (row, pk),
                _ => continue,
            };
            for ax in 0..3 {
                worst = worst.max(dev(pk[ax][0], &row[ax])).max(dev(pk[ax][1], &Rat::int(1).sub(&row[ax])));
                if !f.dephased {
                    worst = worst.max(dev(comp.reference[k].schrodinger[ax][0], &row[ax]));
                }
                checked += 2;
            }
            worst = worst.max(dev(comp.matrix.clock_probability[k], &row[3]));
            checked += 1;
        }
    }
    (checked, worst)
}

pub fn render() -> Rendered {
    let mut files = Vec::new();
    let mut rows = Vec::new();
    let mut summary = Vec::new();
    for f in catalogue() {
        let c = parse_case(&f.case).expect("fixture case must be valid");
        let comp = compute(&c, f.dephased);
        files.push((format!("{}.case", f.name), f.case.clone()));
        let mut body = String::new();
        if f.dephased {
            body.push_str("note values computed from the DEPHASED kernel-branch mixture of this case (negative control)\n");
        }
        for l in comp.values.iter().chain(comp.verdict_lines.iter()) {
            body.push_str(l);
            body.push('\n');
        }
        body.push_str(&format!("verdict_id {}\n", comp.verdict_id));
        files.push((format!("{}.values", f.name), body.into_bytes()));
        let (checked, worst) = calibrate(&f, &comp);
        let met = comp.verdict_lines.iter().find(|l| l.starts_with("expectation_met")).map(|l| l[16..].to_string()).unwrap_or_default();
        let codes = if comp.verdict.codes.is_empty() { "none".to_string() } else { comp.verdict.codes.join(",") };
        rows.push(format!("| {} | {} | {} | {} | {} | {} | {} | {:.3e} | `{}` |", f.name, if f.dephased { "dephased" } else { "pure" }, comp.matrix.kernel_dim, comp.verdict.outcome, codes, met, checked, worst, comp.verdict_id));
        summary.push(format!("{:<26} {:<8} kernel={} {:<4} codes={:<62} met={} dev={:.2e}", f.name, if f.dephased { "dephased" } else { "pure" }, comp.matrix.kernel_dim, comp.verdict.outcome, codes, met, worst));
    }
    let mut cal = String::from("# AT-0 oracle calibration fixtures\n\nGenerated by `target/at0-oracle fixtures` (deterministic, no wall clock). Reproducing the known\nmathematics here is a calibration result, not a discovery. `exact values checked` counts\nhand-derived rational expectations compared; `max dev` is the largest deviation seen\nagainst them (matrix path and reference path), in absolute probability units.\n\n| fixture | state | kernel dim | outcome | failure codes | expectation_met | exact values checked | max dev | verdict_id |\n|---|---|---|---|---|---|---|---|---|\n");
    for row in &rows {
        cal.push_str(row);
        cal.push('\n');
    }
    cal.push_str("\nHand-derived tables: ideal clock, `H_S = Z/2` (even N) or `Z` (odd N), `psi_0 = |+>`, so\n`P(X+) = (1 + cos a)/2`, `P(Y+) = (1 + sin a)/2`, `P(Z+) = 1/2`, `p(k) = 1/N`, with\n`a = 2|h| * 2 pi (k - r) / N`; only labels whose angle is a multiple of a quarter turn\nhave rational expectations and are counted. Spec fixtures use `H_S = diag(0, 1)` (`hz = -1/2`),\nso `P(Y+) = (1 - sin theta)/2`. The dephased control expects `1/2` everywhere.\nValues blocks follow AT0_RESULT_V2 (f64: tokens, scaled-decimal bounds, bound_kind ESTIMATED).\n");
    files.push(("CALIBRATION.md".to_string(), cal.clone().into_bytes()));
    Rendered { files, calibration: cal, summary }
}
