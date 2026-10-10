//! AT-0 reference oracle, outer layer: the only place that touches files, the
//! process table and the wall clock. Everything that computes lives in src/at0/.
//!
//! Usage:
//!   at0-oracle emit <case-file> [--dephased] [-o <out-file>]   write an AT0_RESULT_V2 record
//!   at0-oracle fixtures <dir>                                   write the calibration fixtures
//!   at0-oracle check <case-file>                                validate only; print case_id
//! A refused case prints exactly `AT0_CASE_REFUSED <code>` on stderr and exits 2.

mod at0;

use at0::case::{parse_case, CONTRACT_COMMIT};
use at0::result::{compute, result_bytes, PROVENANCE_KEYS};
use at0::sha256::sha256_hex;
use std::io::Write;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

fn git(args: &[&str]) -> Option<String> {
    let dir = std::env::current_exe().ok()?.parent()?.parent()?.to_path_buf();
    let out = Command::new("git").args(args).current_dir(dir).output().ok()?;
    if out.status.success() {
        Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
    } else {
        None
    }
}

/// UTC timestamp at second resolution without any calendar library.
fn utc_now() -> String {
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    let (days, rem) = (secs.div_euclid(86400), secs.rem_euclid(86400));
    let (h, m, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    // civil from days (Howard Hinnant)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mo = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if mo <= 2 { y + 1 } else { y };
    format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, mo, d, h, m, s)
}

fn hostname() -> String {
    std::fs::read_to_string("/etc/hostname").map(|s| s.trim().to_string()).ok().filter(|s| !s.is_empty()).unwrap_or_else(|| "unknown".to_string())
}

fn exe_sha256() -> String {
    std::env::current_exe().ok().and_then(|p| std::fs::read(p).ok()).map(|b| sha256_hex(&b)).unwrap_or_else(|| "0".repeat(64))
}

fn emit(args: &[String]) -> i32 {
    let dephased = args.iter().any(|a| a == "--dephased");
    let out_path = args.iter().position(|a| a == "-o").and_then(|i| args.get(i + 1)).cloned();
    let inputs: Vec<&String> = args.iter().filter(|a| !a.starts_with('-') && Some(*a) != out_path.as_ref()).collect();
    if inputs.len() != 1 {
        eprintln!("usage: at0-oracle emit <case-file> [--dephased] [-o <out-file>]");
        return 1;
    }
    let started = utc_now();
    let data = match std::fs::read(inputs[0]) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("cannot read {}: {}", inputs[0], e);
            return 1;
        }
    };
    let case = match parse_case(&data) {
        Ok(c) => c,
        Err(r) => {
            eprintln!("AT0_CASE_REFUSED {}", r.code);
            return 2;
        }
    };
    let computed = compute(&case, dephased);
    let commit = git(&["rev-parse", "HEAD"]);
    let dirty = git(&["status", "--porcelain", "--", "."]).map(|s| !s.is_empty()).unwrap_or(true);
    let exe = exe_sha256();
    let rustc = option_env!("AT0_RUSTC_VERSION").unwrap_or("rustc unknown (built without build.sh)");
    let flags = option_env!("AT0_BUILD_FLAGS").unwrap_or("unknown");
    let values: Vec<String> = vec![
        "aien-dev/omega".to_string(),
        commit.clone().unwrap_or_else(|| "0".repeat(40)),
        if commit.is_some() && !dirty { "YES" } else { "NO" }.to_string(),
        CONTRACT_COMMIT.to_string(),
        exe.clone(),
        "aien-dev/omega".to_string(),
        commit.unwrap_or_else(|| "0".repeat(40)),
        exe,
        format!("oracle {} {}{}", rustc, option_env!("AT0_RUSTC_HOST").unwrap_or("unknown-host"), if dephased { " (dephased control)" } else { "" }),
        format!("rustc {} src/main.rs", flags),
        hostname(),
        started,
        utc_now(),
    ];
    let prov: Vec<(&str, String)> = PROVENANCE_KEYS.iter().cloned().zip(values).collect();
    let record = match result_bytes(&case, &computed, &prov) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("internal: {}", e);
            return 1;
        }
    };
    match out_path {
        Some(p) => std::fs::write(&p, &record).map(|_| 0).unwrap_or_else(|e| {
            eprintln!("cannot write {}: {}", p, e);
            1
        }),
        None => std::io::stdout().write_all(&record).map(|_| 0).unwrap_or(1),
    }
}

fn fixtures(dir: &str) -> i32 {
    if let Err(e) = std::fs::create_dir_all(dir) {
        eprintln!("cannot create {}: {}", dir, e);
        return 1;
    }
    let rendered = at0::fixtures::render();
    for (name, bytes) in &rendered.files {
        if let Err(e) = std::fs::write(format!("{}/{}", dir, name), bytes) {
            eprintln!("cannot write {}: {}", name, e);
            return 1;
        }
    }
    println!("wrote {} files to {}", rendered.files.len(), dir);
    for line in &rendered.summary {
        println!("{}", line);
    }
    0
}

fn check(path: &str) -> i32 {
    let data = match std::fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("cannot read {}: {}", path, e);
            return 1;
        }
    };
    match parse_case(&data) {
        Ok(c) => {
            println!("case_id {}\nacceptance_id {}\ncase_file_sha256 {}", c.case_id, c.acceptance_id, c.file_sha256);
            0
        }
        Err(r) => {
            eprintln!("AT0_CASE_REFUSED {}", r.code);
            2
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let code = match args.first().map(String::as_str) {
        Some("emit") => emit(&args[1..]),
        Some("fixtures") if args.len() == 2 => fixtures(&args[1]),
        Some("check") if args.len() == 2 => check(&args[1]),
        _ => {
            eprintln!("usage: at0-oracle emit <case> [--dephased] [-o out] | fixtures <dir> | check <case>");
            1
        }
    };
    std::process::exit(code);
}

#[cfg(test)]
mod tests {
    use super::at0::case::{build_case, kernel_pairs, parse_case, Acceptance, Scaled};
    use super::at0::complex::{add_scaled, frobenius, trace};
    use super::at0::matrix_path::{self as mp, EPS};
    use super::at0::rational::Rat;
    use super::at0::result::{self, compute, f64_token, parse_values, result_bytes, value_exact, PROVENANCE_KEYS};
    use super::at0::exact::Exact;

    const KAT_CASE_ID: &str = "3cf4ca4f882b5b9691ddcd905e65e15010855adcd2be200e19ebc3184655b44d";
    const KAT_ACC_ID: &str = "a13fb02dd674b8042ef8c0a197f03d58768709f782ba59cd26545eef6d41a228";
    const KAT_FILE: &str = "ed16c95c89bf312b0fbf95a4cd43bc352daa138954fabad78d8a170cd35b4e81";
    const TOL: f64 = 1e-12;

    fn r(n: i128, d: i128) -> Rat {
        Rat::new(n, d)
    }
    fn plus() -> [(Rat, Rat); 2] {
        [(r(1, 1), r(0, 1)), (r(1, 1), r(0, 1))]
    }
    fn clock(n: usize) -> Vec<Rat> {
        (0..n).map(|j| r(2 * j as i128 - (n as i128 - 1), 2)).collect()
    }
    fn zhalf() -> [Rat; 4] {
        [r(0, 1), r(0, 1), r(0, 1), r(1, 2)]
    }
    fn labels(m: usize) -> Vec<String> {
        (0..m).map(|k| format!("t{}", k)).collect()
    }
    fn ideal(n: usize, pauli: Option<[Rat; 4]>, acc: &Acceptance) -> Vec<u8> {
        let p = pauli.unwrap_or(if n % 2 == 0 { zhalf() } else { [r(0, 1), r(0, 1), r(0, 1), r(1, 1)] });
        let lb = labels(n);
        let lbs: Vec<&str> = lb.iter().map(String::as_str).collect();
        build_case(&format!("ideal-n{}", n), &clock(n), p, "t0", plus(), r(1, n as i128), r(1, 1), &lbs, acc)
    }
    fn kat() -> Vec<u8> {
        build_case("at0-kat-ideal-qubit-n4", &clock(4), zhalf(), "t0", plus(), r(1, 4), r(1, 1), &["t0", "t1", "t2", "t3"], &Acceptance::positive())
    }
    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() <= TOL
    }
    fn replace(data: &[u8], from: &str, to: &str) -> Vec<u8> {
        String::from_utf8(data.to_vec()).unwrap().replace(from, to).into_bytes()
    }
    fn refuses(data: &[u8], code: &str) {
        match parse_case(data) {
            Err(e) => assert_eq!(e.code, code, "{:?}", e),
            Ok(_) => panic!("expected refusal {}", code),
        }
    }
    /// Parses, computes, asserts PASS and three-way agreement of the paths.
    fn all_paths_agree(data: &[u8]) -> (super::at0::case::Case, result::Computed) {
        let c = parse_case(data).unwrap();
        let comp = compute(&c, false);
        assert_eq!(comp.verdict.outcome, "PASS", "{} {:?}", c.name, comp.verdict.codes);
        for k in 0..c.labels.len() {
            let p = comp.matrix.pauli[k].unwrap();
            for ax in 0..3 {
                for si in 0..2 {
                    assert!(close(p[ax][si], comp.reference[k].schrodinger[ax][si]), "{} k={} ax={} si={}", c.name, k, ax, si);
                    assert!(close(p[ax][si], comp.reference[k].bloch[ax][si]), "{} bloch k={} ax={}", c.name, k, ax);
                }
            }
        }
        (c, comp)
    }

    #[test]
    fn known_answer_digests_and_kernel() {
        let c = parse_case(&kat()).unwrap();
        assert_eq!(c.file_sha256, KAT_FILE);
        assert_eq!(c.case_id, KAT_CASE_ID);
        assert_eq!(c.acceptance_id, KAT_ACC_ID);
        assert_eq!(c.eigenvalues, [r(1, 2), r(-1, 2)]);
        assert_eq!(kernel_pairs(&c), vec![(1, 0), (2, 1)]);
    }

    #[test]
    fn refusals_in_contract_order() {
        refuses(&replace(&kat(), "OMEGA-AT0-CASE v1", "OMEGA-AT0-CASE v2"), "CASE_UNSUPPORTED_VERSION");
        refuses(&replace(&kat(), "domain omega.at0.case.v1", "domain omega.at0.case.v9"), "CASE_UNSUPPORTED_VERSION");
        let mut cut = kat();
        cut.pop();
        refuses(&cut, "CASE_PARSE_ERROR");
        refuses(&replace(&kat(), "povm_weight 1/1", "povm_weight  1/1"), "CASE_PARSE_ERROR");
        refuses(&replace(&kat(), "\n", "\r\n"), "CASE_PARSE_ERROR");
        refuses(&replace(&kat(), "povm_weight 1/1", "povm_weight 2/2"), "CASE_NONCANONICAL");
        refuses(&replace(&kat(), "povm_tau_turns 1/4", "povm_tau_turns 1/4\npovm_tau_turns 1/4"), "CASE_PARSE_ERROR");
        refuses(&replace(&kat(), "tol_probability 1@12", "tol_probability 10@13"), "CASE_NONCANONICAL");
        refuses(&replace(&kat(), "clock_energies -3/2", "clock_energies 3/2"), "CASE_INVALID_PARAMETER"); // duplicate energy: parameter rule precedes identity
        let l4 = ["t0", "t1", "t2", "t3"];
        refuses(&build_case("z", &clock(4), zhalf(), "t0", [(r(0, 1), r(0, 1)), (r(0, 1), r(0, 1))], r(1, 4), r(1, 1), &l4, &Acceptance::positive()), "CASE_INVALID_PARAMETER");
        refuses(&build_case("w", &clock(4), zhalf(), "t0", plus(), r(1, 4), r(0, 1), &l4, &Acceptance::positive()), "CASE_INVALID_PARAMETER");
        refuses(&build_case("e", &[r(1, 2), r(-1, 2)], zhalf(), "t0", plus(), r(1, 2), r(1, 1), &["t0", "t1"], &Acceptance::positive()), "CASE_INVALID_PARAMETER");
        refuses(&build_case("r", &clock(4), zhalf(), "t9", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::positive()), "CASE_INVALID_PARAMETER");
        refuses(&build_case("d", &clock(4), zhalf(), "t0", plus(), r(1, 4), r(1, 1), &["t0", "t0", "t2", "t3"], &Acceptance::positive()), "CASE_INVALID_PARAMETER");
        refuses(&build_case("irr", &clock(4), [r(0, 1), r(1, 1), r(0, 1), r(1, 1)], "t0", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::positive()), "CASE_IRRATIONAL_SPECTRUM");
        refuses(&replace(&kat(), "case_id 3cf4", "case_id 0cf4"), "CASE_ID_MISMATCH");
        refuses(&replace(&kat(), "expected_failure_codes none", "expected_failure_codes NOT_A_CODE"), "CASE_ID_MISMATCH");
        refuses(&ideal(4, None, &Acceptance { expected_codes: vec!["TRIVIAL_PHYSICAL_STATE"], ..Acceptance::positive() }), "CASE_INVALID_PARAMETER");
        refuses(&ideal(4, None, &Acceptance::negative(&["NOT_A_CODE"])), "CASE_INVALID_PARAMETER");
        refuses(&ideal(4, None, &Acceptance::negative(&["TRIVIAL_PHYSICAL_STATE", "NONFINITE_VALUE"])), "CASE_INVALID_PARAMETER");
        let big = build_case("big", &(0..65).map(|j| r(j, 1)).collect::<Vec<_>>(), zhalf(), "t0", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::positive());
        refuses(&big, "CASE_INVALID_PARAMETER");
    }

    #[test]
    fn case_name_outside_identity_and_tokens() {
        let a = parse_case(&kat()).unwrap();
        let b = parse_case(&replace(&kat(), "case_name at0-kat-ideal-qubit-n4", "case_name renamed")).unwrap();
        assert_eq!(a.case_id, b.case_id);
        assert_ne!(a.file_sha256, b.file_sha256);
        for tok in ["0@0", "1@12", "1421086@20", "5@0", "123@3"] {
            let s = super::at0::case::parse_scaled(tok).unwrap();
            assert_eq!(Scaled::canonical(s.n, s.k).text(), tok);
        }
        assert_eq!(f64_token(-0.0), "f64:0000000000000000");
        assert_eq!(f64_token(f64::INFINITY), "nonfinite");
        assert_eq!(value_exact(&f64_token(0.1)).unwrap().unwrap(), Exact::from_f64(0.1).unwrap());
        assert_eq!(result::bound_token(0.0), "0@0");
        assert!(super::at0::case::parse_scaled(&result::bound_token(1.4210854715202004e-14)).is_ok());
    }

    #[test]
    fn reference_model_hand_table() {
        let c = parse_case(&kat()).unwrap();
        let comp = compute(&c, false);
        let m = &comp.matrix;
        assert_eq!(m.kernel_dim, 2);
        assert!(m.constraint_residual.unwrap() <= TOL);
        assert!(close(super::at0::complex::vnorm2(&mp::physical_state(&c)), 0.5)); // ||psi_0||^2 / N
        assert!(m.povm_residual <= TOL);
        for p in &m.clock_probability {
            assert!(close(*p, 0.25));
        }
        assert!(close(m.clock_probability.iter().sum::<f64>(), 1.0));
        let table: [[f64; 3]; 4] = [[1.0, 0.5, 0.5], [0.5, 1.0, 0.5], [0.0, 0.5, 0.5], [0.5, 0.0, 0.5]];
        for (k, row) in table.iter().enumerate() {
            let pk = m.pauli[k].unwrap();
            for ax in 0..3 {
                assert!(close(pk[ax][0], row[ax]), "k={} ax={}", k, ax);
                assert!(close(pk[ax][1], 1.0 - row[ax]), "k={} ax={}", k, ax);
                assert!(close(comp.reference[k].schrodinger[ax][0], row[ax]));
                assert!(close(comp.reference[k].bloch[ax][0], row[ax]));
            }
        }
        assert_eq!(comp.verdict.outcome, "PASS");
        assert!(comp.verdict.codes.is_empty());
        assert!(comp.verdict.checks.iter().all(|(_, r)| *r == "PASS"));
        assert!(comp.verdict_lines.contains(&"expectation_met YES".to_string()));
    }

    #[test]
    fn density_matrix_properties_and_routes() {
        let c = parse_case(&kat()).unwrap();
        let comp = compute(&c, false);
        let psi = mp::physical_state(&c);
        let rr = mp::density_from_vector(&psi);
        for k in 0..4 {
            let rho = comp.matrix.rho[k].as_ref().unwrap();
            assert!(mp::hermiticity_defect(rho) <= TOL);
            assert!(close(trace(rho).re, 1.0));
            let (lo, hi) = mp::eigenvalues_2x2_hermitian(rho);
            assert!(lo >= -TOL && hi <= 1.0 + TOL);
            for v in comp.matrix.pauli[k].unwrap().iter().flatten() {
                assert!(*v >= -TOL && *v <= 1.0 + TOL);
            }
            let (p, rho2) = mp::conditional_from_density(&c, &rr, k);
            assert!(close(p, comp.matrix.clock_probability[k]));
            assert!(frobenius(&add_scaled(&rho2.unwrap(), rho, -1.0)) <= TOL);
        }
    }

    #[test]
    fn multiple_n_offsets_phases_tilts() {
        for n in [3usize, 4, 5, 6, 8, 12] {
            let (_, comp) = all_paths_agree(&ideal(n, None, &Acceptance::positive()));
            assert_eq!(comp.matrix.kernel_dim, 2);
            assert!(comp.matrix.povm_residual <= TOL);
            for p in &comp.matrix.clock_probability {
                assert!(close(*p, 1.0 / n as f64));
            }
        }
        let l4 = ["t0", "t1", "t2", "t3"];
        let base = compute(&parse_case(&kat()).unwrap(), false);
        for (shift, lb) in [(1usize, "t1"), (2, "t2"), (3, "t3")] {
            let (_, comp) = all_paths_agree(&build_case("offset", &clock(4), zhalf(), lb, plus(), r(1, 4), r(1, 1), &l4, &Acceptance::positive()));
            for k in 0..4 {
                let (a, b) = (comp.matrix.pauli[k].unwrap(), base.matrix.pauli[(k + 4 - shift) % 4].unwrap());
                for ax in 0..3 {
                    assert!(close(a[ax][0], b[ax][0]), "offset {} k={} ax={}", lb, k, ax);
                }
            }
        }
        all_paths_agree(&build_case("tilted", &clock(4), [r(0, 1), r(3, 10), r(0, 1), r(2, 5)], "t0", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::positive()));
        all_paths_agree(&build_case("tilted-y", &clock(4), [r(0, 1), r(3, 10), r(2, 5), r(0, 1)], "t0", [(r(1, 1), r(0, 1)), (r(0, 1), r(1, 1))], r(1, 4), r(1, 1), &l4, &Acceptance::positive()));
        let (c, comp) = all_paths_agree(&build_case("h0", &[r(-1, 1), r(0, 1), r(1, 1), r(2, 1)], [r(1, 2), r(3, 10), r(0, 1), r(2, 5)], "t0", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::positive()));
        assert_eq!(c.eigenvalues, [r(1, 1), r(0, 1)]);
        assert_eq!(comp.matrix.kernel_dim, 2);
        for st in [[(r(0, 1), r(1, 1)), (r(0, 1), r(1, 1))], [(r(-1, 1), r(0, 1)), (r(-1, 1), r(0, 1))], [(r(2, 1), r(0, 1)), (r(2, 1), r(0, 1))], [(r(0, 1), r(-3, 1)), (r(0, 1), r(-3, 1))]] {
            let (_, comp) = all_paths_agree(&build_case("phase", &clock(4), zhalf(), "t0", st, r(1, 4), r(1, 1), &l4, &Acceptance::positive()));
            for k in 0..4 {
                assert!(close(comp.matrix.clock_probability[k], 0.25));
                for ax in 0..3 {
                    assert!(close(comp.matrix.pauli[k].unwrap()[ax][0], base.matrix.pauli[k].unwrap()[ax][0]));
                }
            }
        }
    }

    #[test]
    fn negative_controls() {
        let l4 = ["t0", "t1", "t2", "t3"];
        // dephased stationary mixture: clock correlations without interference
        let c = parse_case(&kat()).unwrap();
        let d = compute(&c, true);
        assert_eq!(d.verdict.outcome, "FAIL");
        assert_eq!(d.verdict.codes, vec!["SCHRODINGER_DEVIATION_EXCEEDED"]);
        assert!(d.verdict.checks.contains(&("povm_normalization", "PASS")));
        assert!(d.verdict.checks.contains(&("conditional_defined", "PASS")));
        for k in 0..4 {
            assert!(close(d.matrix.clock_probability[k], 0.25));
            let p = d.matrix.pauli[k].unwrap();
            assert!(close(p[0][0], 0.5) && close(p[1][0], 0.5) && close(p[2][0], 0.5));
            assert!(mp::hermiticity_defect(d.matrix.rho[k].as_ref().unwrap()) <= TOL);
        }
        assert!(!close(compute(&c, false).matrix.pauli[0].unwrap()[0][0], 0.5));
        // N1 uncovered
        let n1 = compute(&parse_case(&build_case("n1", &[r(1, 1), r(2, 1), r(3, 1), r(4, 1)], zhalf(), "t0", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::negative(&["TRIVIAL_PHYSICAL_STATE"]))).unwrap(), false);
        assert_eq!(n1.verdict.codes, vec!["TRIVIAL_PHYSICAL_STATE"]);
        // V2 section 4 trivial-kernel table: check 3 FAIL, checks 4, 6, 7, 8, 9, 10 NOT_EVALUATED, 1, 2, 5 evaluated
        for (i, (name, res)) in n1.verdict.checks.iter().enumerate() {
            let want = match i + 1 { 3 => "FAIL", 4 | 6 | 7 | 8 | 9 | 10 => "NOT_EVALUATED", _ => "PASS" };
            assert_eq!(*res, want, "check {} {}", i + 1, name);
        }
        assert!(n1.verdict_lines.contains(&"expectation_met YES".to_string()));
        assert!(n1.values.iter().filter(|l| l.contains(" undefined 0@0")).count() == 1 + 4 + 24);
        // N2 half covered
        let n2 = compute(&parse_case(&build_case("n2", &[r(1, 2), r(3, 2), r(5, 2), r(7, 2)], zhalf(), "t0", plus(), r(1, 4), r(1, 1), &l4, &Acceptance::negative(&["SCHRODINGER_DEVIATION_EXCEEDED"]))).unwrap(), false);
        assert_eq!(n2.matrix.kernel_dim, 1);
        assert_eq!(n2.verdict.codes, vec!["SCHRODINGER_DEVIATION_EXCEEDED"]);
        for k in 0..4 {
            assert!(close(n2.matrix.pauli[k].unwrap()[2][0], 0.0));
        }
        // N4 wrong weight
        let n4 = compute(&parse_case(&build_case("n4", &clock(4), zhalf(), "t0", plus(), r(1, 4), r(2, 1), &l4, &Acceptance::negative(&["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"]))).unwrap(), false);
        assert_eq!(n4.verdict.codes, vec!["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"]);
        assert!(n4.verdict_lines.contains(&"expectation_met YES".to_string()));
        // N3 broken clock with the right weight: only the POVM check fails
        let n3 = compute(&parse_case(&build_case("n3", &clock(4), zhalf(), "t0", plus(), r(1, 3), r(1, 1), &l4, &Acceptance::positive())).unwrap(), false);
        assert_eq!(n3.verdict.codes, vec!["POVM_NORMALIZATION_EXCEEDED"]);
        assert!(n3.verdict_lines.contains(&"expectation_met NO".to_string()));
        // N5 precision demand
        let mut rig = Acceptance::positive();
        rig.min_bound_kind = "RIGOROUS";
        let n5 = compute(&parse_case(&build_case("n5", &clock(4), zhalf(), "t0", plus(), r(1, 4), r(1, 1), &l4, &rig)).unwrap(), false);
        assert_eq!(n5.verdict.codes, vec!["BOUND_KIND_INSUFFICIENT"]);
    }

    /// AT0_SPEC.md (aien-architecture#176, merged at SPEC_COMMIT), sections 1 and 13: two-level clock,
    /// H_S = diag(0, 1): P(X+) = (1 + cos th)/2, P(Y+) = (1 - sin th)/2, P(Z+) = 1/2, p_k = 1/4.
    #[test]
    fn spec_draft_reference_model() {
        let l4 = ["t0", "t1", "t2", "t3"];
        let spec = |state: [(Rat, Rat); 2], w: Rat, m: usize, acc: &Acceptance| build_case("spec", &[r(-1, 1), r(0, 1)], [r(1, 2), r(0, 1), r(0, 1), r(-1, 2)], "t0", state, r(1, 4), w, &l4[..m], acc);
        let (c, comp) = all_paths_agree(&spec(plus(), r(1, 2), 4, &Acceptance::positive()));
        assert_eq!(c.eigenvalues, [r(1, 1), r(0, 1)]);
        assert!(comp.matrix.constraint_residual.unwrap() <= 4.0 * EPS);
        assert!(comp.matrix.povm_residual <= 12.0 * EPS);
        let want = [[1.0, 0.5], [0.5, 0.0], [0.0, 0.5], [0.5, 1.0]];
        for (k, w) in want.iter().enumerate() {
            let p = comp.matrix.pauli[k].unwrap();
            assert!(close(p[0][0], w[0]) && close(p[1][0], w[1]) && close(p[2][0], 0.5), "k={}", k);
            assert!(close(comp.matrix.clock_probability[k], 0.25));
            let purity: f64 = (0..3).map(|ax| (2.0 * p[ax][0] - 1.0).powi(2)).sum();
            assert!(close(purity, 1.0)); // spec invariant I8
        }
        let (_, t4) = all_paths_agree(&spec([(r(1, 1), r(0, 1)), (r(0, 1), r(0, 1))], r(1, 2), 4, &Acceptance::positive()));
        for k in 0..4 {
            assert!(close(t4.matrix.pauli[k].unwrap()[2][0], 1.0) && close(t4.matrix.pauli[k].unwrap()[0][0], 0.5));
        }
        let (_, t5) = all_paths_agree(&spec([(r(1, 1), r(0, 1)), (r(0, 1), r(1, 1))], r(1, 2), 4, &Acceptance::positive()));
        for k in 0..4 {
            for ax in 0..3 {
                assert!(close(t5.matrix.pauli[k].unwrap()[ax][0], comp.matrix.pauli[(k + 3) % 4].unwrap()[ax][0]), "t5 k={}", k); // theta -> theta - pi/2
            }
        }
        let n3 = compute(&parse_case(&spec(plus(), r(1, 4), 4, &Acceptance::positive())).unwrap(), false);
        assert!(close(n3.matrix.povm_residual, 0.5f64.sqrt()));
        assert!(close(n3.matrix.clock_probability.iter().sum::<f64>(), 0.5));
        assert_eq!(n3.verdict.codes, vec!["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"]);
        let n4 = compute(&parse_case(&spec(plus(), r(1, 2), 3, &Acceptance::positive())).unwrap(), false);
        assert!(close(n4.matrix.povm_residual, 0.5));
        assert!(close(n4.matrix.clock_probability.iter().sum::<f64>(), 0.75));
        assert_eq!(n4.verdict.codes, vec!["POVM_NORMALIZATION_EXCEEDED", "PROBABILITY_SUM_EXCEEDED"]);
    }

    fn provenance(stamp: &str) -> Vec<(&'static str, String)> {
        let vals = ["aien-dev/omega", "0000000000000000000000000000000000000000", "NO", super::CONTRACT_COMMIT, &"1".repeat(64), "aien-dev/omega", "0000000000000000000000000000000000000000", &"1".repeat(64), "oracle rustc test", "std", "test", stamp, stamp];
        PROVENANCE_KEYS.iter().cloned().zip(vals.iter().map(|s| s.to_string())).collect()
    }

    #[test]
    fn record_shape_and_wall_clock_independence() {
        let c = parse_case(&kat()).unwrap();
        let comp = compute(&c, false);
        let a = result_bytes(&c, &comp, &provenance("2026-10-09T00:00:00Z")).unwrap();
        let b = result_bytes(&c, &comp, &provenance("2026-10-10T12:34:56Z")).unwrap();
        for data in [&a, &b] {
            let text = String::from_utf8(data.clone()).unwrap();
            let lines: Vec<&str> = text.split('\n').collect();
            assert_eq!(lines[0], "OMEGA-AT0-RESULT v2");
            assert_eq!(lines[1], "domain omega.at0.result.v2");
            assert_eq!(lines[2], "contract AT0_RESULT_V2");
            assert_eq!(lines[lines.len() - 1], "");
            assert_eq!(lines[lines.len() - 2], "end");
            let ed = lines[lines.len() - 3].strip_prefix("evidence_digest ").unwrap();
            let above: Vec<String> = lines[..lines.len() - 3].iter().map(|s| s.to_string()).collect();
            assert_eq!(result::evidence_digest(&above), ed);
            assert!(lines.contains(&"threads 1"));
            assert!(text.bytes().all(|b| b == b'\n' || (0x20..=0x7E).contains(&b)));
        }
        let pick = |d: &Vec<u8>, key: &str| String::from_utf8(d.clone()).unwrap().lines().find(|l| l.starts_with(key)).unwrap().to_string();
        // Agent 0 condition: records are marked as oracle output or not written at all
        let mut p = provenance("2026-10-09T00:00:00Z");
        p[8].1 = "rustc test".to_string();
        assert!(result_bytes(&c, &comp, &p).is_err());
        let mut p = provenance("2026-10-09T00:00:00Z");
        p[7].1 = "2".repeat(64);
        assert!(result_bytes(&c, &comp, &p).is_err());
        assert!(String::from_utf8(a.clone()).unwrap().contains("\nbuild_cc oracle "));
        assert_eq!(pick(&a, "verdict_id"), pick(&b, "verdict_id"));
        assert_ne!(pick(&a, "evidence_digest"), pick(&b, "evidence_digest"));
        assert_eq!(comp.verdict_id, result::verdict_id(&c, &comp.verdict_lines));
        let v = parse_values(&comp.values).unwrap();
        assert_eq!(v.kernel_dim, 2);
        assert_eq!(v.pauli[0].3.clone().unwrap(), Exact::from_int(1));
        let mut s = Exact::zero();
        for (_, p, _) in &v.clock_probability {
            s = s.add(p.as_ref().unwrap());
        }
        assert_eq!(s, Exact::from_int(1));
        // V2 rule: undefined outside a trivial kernel is refused by the parser
        let mut bad = comp.values.clone();
        bad[3] = "label 0 t0 UNDEFINED".to_string();
        let idx = bad.iter().position(|l| l.starts_with("pauli 0 X PLUS")).unwrap();
        bad[idx] = "pauli 0 X PLUS undefined 0@0".to_string();
        assert!(parse_values(&bad).is_err());
    }

    #[test]
    fn fixtures_render_and_meet_expectations() {
        let rendered = super::at0::fixtures::render();
        assert_eq!(rendered.files.len(), 22 * 2 + 1);
        for f in super::at0::fixtures::catalogue() {
            let c = parse_case(&f.case).unwrap();
            let comp = compute(&c, f.dephased);
            assert!(comp.verdict_lines.contains(&"expectation_met YES".to_string()), "{}", f.name);
            let (checked, worst) = super::at0::fixtures::calibrate(&f, &comp);
            if f.expected.is_some() {
                assert!(checked > 0 && worst <= 1e-15, "{} checked={} worst={:e}", f.name, checked, worst);
            }
        }
        let kat_file = rendered.files.iter().find(|(n, _)| n == "at0-kat-ideal-qubit-n4.case").unwrap();
        assert_eq!(super::at0::sha256::sha256_hex(&kat_file.1), KAT_FILE);
    }
}
