//! `selftest` (the evaluator's own tests at run time) and `qualify` (run a
//! candidate engine and the oracle on a corpus, splice, verify, and print the
//! per-case table and gate verdicts of AT1 charter section 6).

use crate::case::{case_bytes_from_body, validate_bytes, BoundKind, Case};
use crate::corpus::{hand_tables, kat, public_rows, valid_specs};
use crate::judge::rederive;
use crate::model::{shadow, Mods, Status};
use crate::rat::{parse_rat, Q};
use crate::result::{parse, Ck, Kind, RVal, ResultFile};
use crate::synth::{engine_result, oracle_record, splice, EngineMut, OracleMut};
use crate::verify::{case_status, verify, Role, Sev};
use std::fs;
use std::process::Command;

pub const KAT_FILE_SHA: &str = "76e282fecf21025c9dd675378244e90ff192822a4c49d6cb812c9fe84b3f9ba4";
pub const KAT_CASE_ID: &str = "890980a43bd189494c2c922d2bf0049cbffb07f1090f578e04e1142aee7525f1";
pub const KAT_ACC_ID: &str = "d63246c391296c6cd381f1b654cc625be9f453c77c27c68d9eac240efd5b4d8f";

fn hq(s: &str) -> Q {
    if s.contains('/') {
        parse_rat(s).expect("hand table rational").0
    } else {
        Q::int(s.parse::<i64>().expect("hand table integer"))
    }
}

struct T {
    n: usize,
    bad: usize,
}
impl T {
    fn ok(&mut self, cond: bool, what: &str) {
        self.n += 1;
        if !cond {
            self.bad += 1;
            println!("SELFTEST FAIL: {}", what);
        }
    }
}

fn case_of(path_part: &str) -> (String, Case, Vec<u8>) {
    let (p, _, s) = valid_specs().into_iter().find(|(p, _, _)| p.contains(path_part)).expect("case");
    let b = s.bytes();
    (p, validate_bytes(&b).expect("valid"), b)
}
fn honest(c: &Case) -> ResultFile {
    splice(&engine_result(c, &EngineMut::default(), "at1-eval synth-engine honest"), &oracle_record(c, &OracleMut::default(), "at1-eval synth-oracle honest")).unwrap()
}
fn rejudge(r: &mut ResultFile) {
    let nt = !crate::verify::claims_trivial(r);
    r.verdict = rederive(r, nt);
    r.seal();
}
fn rv(x: f64, b: &str) -> RVal {
    RVal::of(x, &crate::rat::parse_scaled(b).unwrap().0)
}

/// Each check of section 4 fires on a crafted input, with the exact code.
fn check_firing(t: &mut T) {
    let (_, p2, _) = case_of("P2-kat");
    let base = honest(&p2);
    t.ok(base.verdict.outcome == "PASS" && base.verdict.checks[10] == Ck::NotEval, "honest P2 is PASS with check 11 NOT_EVALUATED");
    let fire = |t: &mut T, r: &ResultFile, i: usize, want: Ck, code: &str, what: &str| {
        t.ok(r.verdict.checks[i] == want && (code.is_empty() || r.verdict.failure_codes.iter().any(|x| x == code)), &format!("check {} {} -> {} {} (got {} {:?})", i + 1, what, want.text(), code, r.verdict.checks[i].text(), r.verdict.failure_codes));
    };
    let mut r = base.clone();
    r.bound_kind = BoundKind::None;
    rejudge(&mut r);
    fire(t, &r, 0, Ck::Fail, "BOUND_KIND_INSUFFICIENT", "bound_kind NONE vs min ESTIMATED");
    let mut r = base.clone();
    r.pauli[0][0][0] = RVal { kind: Kind::Nonfinite, bound: r.pauli[0][0][0].bound.clone() };
    rejudge(&mut r);
    fire(t, &r, 1, Ck::Fail, "NONFINITE_VALUE", "a nonfinite pauli value");
    let (_, n2, _) = case_of("N2-every");
    let r = honest(&n2);
    fire(t, &r, 2, Ck::Fail, "TRIVIAL_PHYSICAL_STATE", "N2 trivial kernel");
    t.ok([3, 5, 6, 7, 8, 9, 10, 11].iter().all(|&i| r.verdict.checks[i] == Ck::NotEval) && r.verdict.checks[4] == Ck::Pass, "trivial-kernel rule: 4, 6..12 NOT_EVALUATED, 5 evaluated");
    let mut r = base.clone();
    r.constraint = rv(1e-6, "1@15");
    rejudge(&mut r);
    fire(t, &r, 3, Ck::Fail, "CONSTRAINT_RESIDUAL_EXCEEDED", "constraint residual 1e-6");
    let (_, n4a, _) = case_of("N4a");
    let r = honest(&n4a);
    fire(t, &r, 4, Ck::Fail, "POVM_NORMALIZATION_EXCEEDED", "N4a povm");
    fire(t, &r, 5, Ck::Fail, "PROBABILITY_SUM_EXCEEDED", "N4a sum");
    let mut r = base.clone();
    r.pauli[1][2][0] = rv(1.25, "1@15");
    r.pauli[1][2][1] = rv(-0.25, "1@15");
    rejudge(&mut r);
    fire(t, &r, 6, Ck::Fail, "PROBABILITY_OUT_OF_RANGE", "P(Z+) 1.25 and P(Z-) -0.25");
    t.ok(r.verdict.checks[7] == Ck::Pass, "pair sum still 1 so check 8 PASS");
    let mut r = base.clone();
    r.pauli[2][0][0] = rv(0.2, "1@15");
    rejudge(&mut r);
    fire(t, &r, 7, Ck::Fail, "PROBABILITY_SUM_EXCEEDED", "P(X+)+P(X-) != 1");
    let (_, n3, _) = case_of("N3-zero");
    let r = honest(&n3);
    fire(t, &r, 8, Ck::Fail, "CONDITIONAL_UNDEFINED", "N3 zero label");
    t.ok(r.verdict.failure_codes == vec!["CONDITIONAL_UNDEFINED".to_string()], "N3 exact code set");
    let (_, n1, _) = case_of("N1-p2");
    let r = honest(&n1);
    fire(t, &r, 9, Ck::Fail, "SCHRODINGER_DEVIATION_EXCEEDED", "N1 target IDEAL");
    t.ok(r.verdict.checks[10] == Ck::Pass && r.verdict.checks[11] == Ck::Pass, "N1: check 11 and 12 PASS for a correct engine");
    let e = engine_result(&p2, &crate::mutants::engine_mut("M1-drop-v").unwrap(), "m1");
    let r = splice(&e, &oracle_record(&p2, &OracleMut::default(), "o")).unwrap();
    fire(t, &r, 9, Ck::Fail, "INTERACTING_DEVIATION_EXCEEDED", "drop-V engine, target INTERACTING");
    let e = engine_result(&n1.clone(), &crate::mutants::engine_mut("M1-drop-v").unwrap(), "m1");
    let r = splice(&e, &oracle_record(&n1, &OracleMut::default(), "o")).unwrap();
    fire(t, &r, 10, Ck::Fail, "ORACLE_DISAGREEMENT", "drop-V engine, target IDEAL");
    t.ok(r.verdict.checks[9] == Ck::Pass, "drop-V engine passes check 10 under IDEAL (AT1_SPEC 9.2 M1)");
    let mut r = base.clone();
    r.ref_label[3] = Status::Undefined;
    r.ref_clock[3] = rv(0.0, "0@0");
    r.ref_inter[3] = r.pauli[0].clone().map(|a| a.map(|_| RVal::undefined()));
    rejudge(&mut r);
    fire(t, &r, 11, Ck::Fail, "ORACLE_DISAGREEMENT", "engine DEFINED, oracle UNDEFINED");
    let mut r = base.clone();
    r.ref_clock[3] = rv(1e-9, "1@15");
    r.ref_label[3] = Status::Indeterminate;
    r.ref_inter[3] = r.pauli[0].clone().map(|a| a.map(|_| RVal::undefined()));
    rejudge(&mut r);
    fire(t, &r, 11, Ck::Indet, "PRECISION_INSUFFICIENT", "oracle label INDETERMINATE");
    let mut r = base.clone();
    r.clock[0] = RVal { kind: r.clock[0].kind.clone(), bound: crate::rat::parse_scaled("2@12").unwrap().0 };
    rejudge(&mut r);
    fire(t, &r, 5, Ck::Indet, "PRECISION_INSUFFICIENT", "sum bound straddles the tolerance");
    t.ok(r.verdict.outcome == "FAIL" && r.verdict.expectation == "NO", "PRECISION_INSUFFICIENT makes the outcome FAIL and expectation NO");
    // absolute and signed forms on exact boundaries
    let tol = Q::frac(1, 10);
    t.ok(crate::judge::tri_abs(&Q::frac(-1, 20), &Q::frac(1, 20), &tol) == Ck::Pass, "absolute form: |v|+b == tol is PASS");
    t.ok(crate::judge::tri_abs(&Q::frac(3, 20), &Q::frac(1, 20), &tol) == Ck::Indet, "absolute form: |v|-b == tol is INDETERMINATE");
    t.ok(crate::judge::tri_signed(&Q::frac(-1, 2), &Q::zero(), &tol) == Ck::Pass, "signed form: negative v is PASS");
}

pub fn selftest() -> i32 {
    let mut t = T { n: 0, bad: 0 };
    // 1. worked example digests
    let kb = kat().bytes();
    match validate_bytes(&kb) {
        Ok(c) => {
            t.ok(c.file_sha256 == KAT_FILE_SHA, "KAT case_file_sha256");
            t.ok(c.case_id == KAT_CASE_ID, "KAT case_id");
            t.ok(c.acceptance_id == KAT_ACC_ID, "KAT acceptance_id");
        }
        Err(e) => t.ok(false, &format!("KAT refused {:?}", e)),
    }
    // 2. codec on every public row, and canonical round trip of valid cases
    for r in public_rows() {
        let got = match validate_bytes(&r.bytes) {
            Ok(c) => {
                t.ok(case_bytes_from_body(&c.body_lines) == r.bytes, &format!("{} rebuilds byte-identically", r.path));
                "AT1_CASE_OK".to_string()
            }
            Err((code, _)) => format!("AT1_CASE_REFUSED {}", code.code()),
        };
        t.ok(got == r.expect, &format!("{}: codec gives {}, manifest {}", r.path, got, r.expect));
    }
    // 3. shadow against the AT1_SPEC 13.2 hand tables (exact)
    for (path, table) in hand_tables() {
        let (_, c, _) = case_of(path);
        let sh = shadow(&c, &Mods::default(), false);
        t.ok(sh.interacting_exact && sh.ideal_exact, &format!("{} is computed exactly", path));
        for (k, row) in table.iter().enumerate() {
            let pa = sh.pauli[k].as_ref();
            t.ok(sh.p[k].q == hq(row[0]), &format!("{} p({}) = {} (shadow {})", path, k, row[0], sh.p[k].q.text()));
            for a in 0..3 {
                t.ok(pa.map_or(false, |p| p[a][0].q == hq(row[1 + a])), &format!("{} k={} axis {} = {}", path, k, a, row[1 + a]));
                t.ok(sh.ideal[k][a][0].q == hq(row[4 + a]), &format!("{} k={} ideal axis {} = {}", path, k, a, row[4 + a]));
            }
        }
    }
    let kd = |p: &str| shadow(&case_of(p).1, &Mods::default(), false);
    t.ok(kd("P3-every").kernel_dim == 3 && kd("P3b").kernel_dim == 4 && kd("P2-kat").kernel_dim == 2 && kd("P6").kernel_dim == 2, "kernel dimensions 3, 4, 2, 2 (P3, P3b, P2, P6)");
    let n2 = kd("N2-every");
    t.ok(n2.kernel_dim == 0 && n2.trivial, "N2: dimension 0");
    let n6 = kd("N6-zero");
    t.ok(n6.kernel_dim == 1 && n6.trivial, "N6: dimension 1, Psi = 0 exactly");
    for (k, x) in ["4/5", "1/2", "1/5", "1/2"].iter().enumerate() {
        t.ok(n6.ideal[k][0][0].q == hq(x) && n6.ideal[k][2][0].q == hq("9/10"), &format!("N6 reference_ideal k={}", k));
    }
    let n3 = kd("N3-zero");
    t.ok(["1/2", "1/4", "0", "1/4"].iter().enumerate().all(|(k, x)| n3.p[k].q == hq(x)) && n3.status[2] == Status::Undefined, "N3 marginal 1/2, 1/4, 0, 1/4 with label 2 UNDEFINED");
    let n4a = kd("N4a");
    t.ok(["5/56", "1/8", "9/56", "1/8"].iter().enumerate().all(|(k, x)| n4a.p[k].q == hq(x)) && n4a.povm_residual.q == Q::one(), "N4a marginal and povm_residual 1");
    let n4b = kd("N4b");
    let r42 = (42.0f64).sqrt() / 4.0;
    t.ok(["5/28", "2/7", "2/7", "5/28"].iter().enumerate().all(|(k, x)| (n4b.p[k].q.sub(&hq(x))).abs().cmp(&Q::frac(1, 1_000_000_000_000)) == std::cmp::Ordering::Less) && (n4b.povm_residual.q.to_f64() - r42).abs() < 1e-15, "N4b marginal 5/28, 2/7, 2/7, 5/28 and povm sqrt(42)/4");
    let y1 = 0.5 + 3.0 * (3.0f64).sqrt() / 16.0;
    let near = |a: &Q, b: &Q| a.sub(b).abs().cmp(&Q::frac(1, 1_000_000_000_000_000_000)) == std::cmp::Ordering::Less;
    t.ok(n4b.pauli[1].as_ref().map_or(false, |p| near(&p[0][0].q, &hq("19/80")) && (p[1][0].q.to_f64() - y1).abs() < 1e-15 && near(&p[2][0].q, &hq("31/40"))), "N4b k=1 Pauli 19/80, 1/2 + 3 sqrt(3)/16, 31/40");
    // 4. AT1_SPEC 13.3 deviation maxima
    for (p, dp_max, dpp_max) in [("P2-kat", "3/10", "1/14"), ("P3-every", "2/5", "9/56"), ("P3b", "1", "21/80"), ("P4-complex-psi-ref-t1", "99/370", "3/41"), ("P5", "274763624033/549527248074", "362/524179"), ("P6", "49/50", "1/14"), ("P1a", "0", "0"), ("P1c", "0", "0")] {
        let (_, c, _) = case_of(p);
        let sh = shadow(&c, &Mods::default(), false);
        let wn = c.w.div(&Q::int(c.n as i64));
        let mut mdp = Q::zero();
        let mut mp = Q::zero();
        for k in 0..c.m {
            if let Some(pa) = &sh.pauli[k] {
                for a in 0..3 {
                    for g in 0..2 {
                        let d = pa[a][g].q.sub(&sh.ideal[k][a][g].q).abs();
                        if d.cmp(&mdp) == std::cmp::Ordering::Greater {
                            mdp = d;
                        }
                    }
                }
            }
            let d = sh.p[k].q.sub(&wn).abs();
            if d.cmp(&mp) == std::cmp::Ordering::Greater {
                mp = d;
            }
        }
        t.ok(mdp == hq(dp_max) && mp == hq(dpp_max), &format!("13.3 {}: max|dP| {} (got {}), max|dp| {} (got {})", p, dp_max, mdp.text(), dpp_max, mp.text()));
    }
    // 5. every honest synthetic run verifies PASS and meets its expectation; parser round trip
    for (path, _, s) in valid_specs() {
        let b = s.bytes();
        let c = validate_bytes(&b).unwrap();
        let r = honest(&c);
        let rb = r.bytes();
        t.ok(parse(&rb).map(|x| x.bytes() == rb).unwrap_or(false), &format!("{} result parse/emit round trip", path));
        let rep = verify(&rb, Some(&b), Role::Candidate);
        t.ok(rep.status() == "PASS", &format!("{} honest synthetic run verifies PASS ({:?})", path, rep.codes()));
        t.ok(r.verdict.expectation == "YES", &format!("{} honest run meets its expectation (codes {:?})", path, r.verdict.failure_codes));
        let ro = verify(&oracle_record(&c, &OracleMut::default(), "o").bytes(), Some(&b), Role::Oracle);
        t.ok(ro.status() == "PASS", &format!("{} honest oracle record verifies PASS ({:?})", path, ro.codes()));
    }
    // 6. the margin rule of AT1_SPEC 8.3 holds on every public valid case
    for (path, _, s) in valid_specs() {
        let c = validate_bytes(&s.bytes()).unwrap();
        let m = crate::hidden::margin_ok(&c);
        t.ok(m.is_ok(), &format!("{} margin rule: {:?}", path, m.err()));
    }
    // 7. checks firing on crafted inputs
    check_firing(&mut t);
    // 8. a result with a refused embedded case, and an ERROR placeholder, are handled
    let (_, p2, p2b) = case_of("P2-kat");
    let mut r = honest(&p2);
    r.verdict = crate::judge::placeholder_verdict("ERROR", "INTERNAL_ERROR");
    r.kernel_dim = 0;
    r.constraint = RVal::undefined();
    r.povm = RVal::undefined();
    for k in 0..p2.m {
        r.label[k] = Status::Undefined;
        r.ref_label[k] = Status::Undefined;
        r.clock[k] = RVal::undefined();
        r.ref_clock[k] = RVal::undefined();
        for a in 0..3 {
            for g in 0..2 {
                r.pauli[k][a][g] = RVal::undefined();
                r.ref_ideal[k][a][g] = RVal::undefined();
                r.ref_inter[k][a][g] = RVal::undefined();
            }
        }
    }
    r.seal();
    let rep = verify(&r.bytes(), Some(&p2b), Role::Candidate);
    t.ok(rep.codes() == vec!["E4-RUN-NOT-COMPLETED".to_string()], &format!("well-formed ERROR result gives only E4-RUN-NOT-COMPLETED ({:?})", rep.codes()));
    let rep = verify(b"OMEGA-AT1-RESULT v1\n", None, Role::Candidate);
    t.ok(rep.codes() == vec!["E4-PARSE".to_string()], "truncated result gives E4-PARSE");
    println!("SELFTEST: {} ({} assertions, {} failed)", if t.bad == 0 { "PASS" } else { "FAIL" }, t.n, t.bad);
    if t.bad == 0 { 0 } else { 1 }
}

// ---------------------------------------------------------------- qualify

fn sq(p: &str) -> String {
    format!("'{}'", p.replace('\'', "'\\''"))
}

struct Run {
    status: Option<i32>,
    stderr: String,
}

fn run_tool(tmpl: &str, case: &str, out: &str) -> Run {
    run_tool_env(tmpl, case, out, &[])
}

/// Runs a command template through `sh -c`; `envs` are set on the shell process itself, so
/// they reach every command of the template (`cd dir && ./engine ...` included).
fn run_tool_env(tmpl: &str, case: &str, out: &str, envs: &[(&str, &str)]) -> Run {
    let mut cmd = tmpl.replace("{case}", &sq(case));
    let capture = !tmpl.contains("{out}");
    cmd = cmd.replace("{out}", &sq(out));
    if capture {
        cmd = format!("{} > {}", cmd, sq(out));
    }
    match Command::new("sh").arg("-c").arg(&cmd).envs(envs.iter().copied()).output() {
        Ok(o) => Run { status: o.status.code(), stderr: String::from_utf8_lossy(&o.stderr).into_owned() },
        Err(e) => Run { status: None, stderr: format!("cannot start sh: {}", e) },
    }
}

fn refusal_ok(r: &Run, expect: &str) -> Result<(), String> {
    let want = format!("{}\n", expect);
    if r.status == Some(2) && r.stderr == want {
        Ok(())
    } else {
        Err(format!("exit {:?} stderr {:?} (want exit 2 and one line `{}`)", r.status, r.stderr.trim_end(), expect))
    }
}

pub fn qualify(args: &[&str]) -> i32 {
    let mut cases = "cases".to_string();
    let mut engine = String::new();
    let mut oracle = String::new();
    let mut out = "build/qualify".to_string();
    let mut limit = usize::MAX;
    let mut i = 0;
    while i < args.len() {
        let v = args.get(i + 1).map(|s| s.to_string()).unwrap_or_default();
        match args[i] {
            "--cases" => cases = v,
            "--engine" => engine = v,
            "--oracle" => oracle = v,
            "--out" => out = v,
            "--limit" => limit = v.parse().unwrap_or(usize::MAX),
            _ => {
                eprintln!("qualify: unknown option {}", args[i]);
                return 64;
            }
        }
        i += 2;
    }
    if engine.is_empty() || oracle.is_empty() {
        eprintln!("qualify: --engine and --oracle command templates are required ({{case}} and {{out}} are substituted)");
        return 64;
    }
    // A directory without MANIFEST.tsv (the revealed hidden set: only .case files, so the
    // commitment covers exactly what is run) is read as every .case file, sorted bytewise,
    // each expected to parse; the worked-example requirement then does not apply.
    let manifest_path = format!("{}/MANIFEST.tsv", cases);
    let has_manifest = std::path::Path::new(&manifest_path).exists();
    let man = if has_manifest {
        match fs::read_to_string(&manifest_path) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("qualify: cannot read {}: {}", manifest_path, e);
                return 66;
            }
        }
    } else {
        match crate::hidden::manifest(&cases) {
            Ok(m) if !m.is_empty() => m.lines().map(|l| format!("{}\thidden\tAT1_CASE_OK\n", &l[66..])).collect::<String>(),
            Ok(_) => {
                eprintln!("qualify: {} has neither MANIFEST.tsv nor .case files", cases);
                return 66;
            }
            Err(e) => {
                eprintln!("qualify: cannot read {}: {}", cases, e);
                return 66;
            }
        }
    };
    for d in ["engine", "oracle", "spliced"] {
        let _ = fs::create_dir_all(format!("{}/{}", out, d));
    }
    let mut table = String::from("# path\tclass\texpected\tcodec\tengine\toracle\tderived_outcome\tderived_codes\tcase_verdict\tfindings\n");
    let mut vids = String::new();
    let (mut g1, mut g3, mut g4, mut g5) = (true, true, true, true);
    let mut counts = std::collections::BTreeMap::<String, (usize, usize, usize)>::new();
    let mut kat_seen = false;
    for line in man.lines().filter(|l| !l.starts_with('#') && !l.is_empty()).take(limit) {
        let f: Vec<&str> = line.split('\t').collect();
        if f.len() != 3 {
            eprintln!("qualify: malformed manifest line (want 3 tab-separated fields): {:?}", line);
            return 66;
        }
        let (path, class, expect) = (f[0], f[1], f[2]);
        let cpath = format!("{}/{}", cases, path);
        let bytes = fs::read(&cpath).unwrap_or_default();
        let mine = match validate_bytes(&bytes) {
            Ok(_) => "AT1_CASE_OK".to_string(),
            Err((c, _)) => format!("AT1_CASE_REFUSED {}", c.code()),
        };
        let codec = if mine == expect { "ok" } else { g1 = false; "CODEC-DISAGREES" };
        let stem = path.replace('/', "_").trim_end_matches(".case").to_string();
        let eo = format!("{}/engine/{}.result", out, stem);
        let oo = format!("{}/oracle/{}.result", out, stem);
        let _ = fs::remove_file(&eo);
        let _ = fs::remove_file(&oo);
        let re = run_tool(&engine, &cpath, &eo);
        let ro = run_tool(&oracle, &cpath, &oo);
        if expect != "AT1_CASE_OK" {
            let e = refusal_ok(&re, expect);
            let o = refusal_ok(&ro, expect);
            let v = if e.is_ok() && o.is_ok() && codec == "ok" { "PASS" } else { g1 = false; "FAIL" };
            let ent = counts.entry("refusal".into()).or_default();
            if v == "PASS" { ent.0 += 1 } else { ent.1 += 1 }
            table.push_str(&format!("{}\t{}\t{}\t{}\t{}\t{}\t-\t-\t{}\t-\n", path, class, expect, codec, e.err().unwrap_or("refused-exactly".into()), o.err().unwrap_or("refused-exactly".into()), v));
            continue;
        }
        let eb = fs::read(&eo).unwrap_or_default();
        let ob = fs::read(&oo).unwrap_or_default();
        let er = parse(&eb);
        let or = parse(&ob);
        let es = match (&re.status, &er) {
            (Some(0), Ok(_)) => "ran".to_string(),
            _ => format!("RUN-FAILED exit {:?} {}", re.status, er.as_ref().err().cloned().unwrap_or_default()),
        };
        let os = match (&ro.status, &or) {
            (Some(0), Ok(_)) => "ran".to_string(),
            _ => format!("RUN-FAILED exit {:?} {}", ro.status, or.as_ref().err().cloned().unwrap_or_default()),
        };
        let neg = bytes.windows(21).any(|w| w == b"control_kind NEGATIVE");
        let mut verdict = "FAIL".to_string();
        let mut findings = String::new();
        let (mut dout, mut dcodes) = ("-".to_string(), "-".to_string());
        // oracle record (G3), judged whether or not the engine produced a parsable result
        let rep_o = match &or {
            Ok(_) => {
                let r = verify(&ob, Some(&bytes), Role::Oracle);
                if r.findings.iter().any(|f| f.sev == Sev::Fail) {
                    g3 = false;
                }
                Some(r)
            }
            Err(_) => {
                g3 = false;
                None
            }
        };
        // refusing a valid case is a codec disagreement (G1)
        for (who, r) in [("engine", &re), ("oracle", &ro)] {
            if r.status == Some(2) {
                g1 = false;
                findings.push_str(&format!("{}:refused-valid-case ", who));
            }
        }
        if let (Ok(e), Ok(o)) = (&er, &or) {
            // codec agreement on the identities (G1): both tools copied the case and its digest
            for (who, r) in [("engine", e), ("oracle", o)] {
                if case_bytes_from_body(&r.case.body_lines) != bytes {
                    g1 = false;
                    findings.push_str(&format!("{}:copied-case-differs ", who));
                }
            }
            if path.contains("P2-kat") {
                kat_seen = true;
                for r in [e, o] {
                    if r.case.case_id != KAT_CASE_ID || r.case.acceptance_id != KAT_ACC_ID || r.case_file_sha256 != KAT_FILE_SHA {
                        g1 = false;
                        findings.push_str("KAT-digests-differ ");
                    }
                }
            }
            match splice(e, o) {
                Ok(s) => {
                    let sb = s.bytes();
                    let _ = fs::write(format!("{}/spliced/{}.result", out, stem), &sb);
                    vids.push_str(&format!("{}\t{}\n", path, s.verdict_id));
                    let rep = verify(&sb, Some(&bytes), Role::Candidate);
                    verdict = case_status(&rep).to_string();
                    dout = rep.derived_outcome.clone();
                    dcodes = if rep.derived_codes.is_empty() { "none".into() } else { rep.derived_codes.join(",") };
                    findings.push_str(&rep.codes().join(","));
                    if e.bound_kind != o.bound_kind {
                        findings.push_str(" NOTE:bound_kind-mixed(engine-vs-oracle)");
                    }
                }
                Err(m) => findings.push_str(&format!("SPLICE-REFUSED {}", m)),
            }
        }
        if let Some(r) = &rep_o {
            if !r.codes().is_empty() {
                findings.push_str(&format!(" oracle:{}", r.codes().join(",")));
            }
        }
        if verdict != "PASS" {
            if neg { g5 = false } else { g4 = false }
        }
        let ent = counts.entry(if neg { "negative".into() } else { "positive".into() }).or_default();
        match verdict.as_str() {
            "PASS" => ent.0 += 1,
            "FAIL" => ent.1 += 1,
            _ => ent.2 += 1,
        }
        table.push_str(&format!("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", path, class, expect, codec, es, os, dout, dcodes, verdict, if findings.trim().is_empty() { "none".into() } else { findings.trim().to_string() }));
    }
    if !kat_seen && limit == usize::MAX && has_manifest {
        g1 = false;
    }
    // T9 wall-clock independence (AT1_SPEC 9.1): the worked example run again with a
    // different time zone and an extra environment variable must give the same values
    // block and verdict_id.
    let mut t9ok = true;
    let mut t9 = "not run (worked example absent)".to_string();
    if kat_seen {
        let kp = man.lines().find(|l| l.starts_with("positive/P2-kat")).and_then(|l| l.split('\t').next()).unwrap_or("");
        let cpath = format!("{}/{}", cases, kp);
        let e2 = format!("{}/engine/T9-second-run.result", out);
        let _ = run_tool_env(&engine, &cpath, &e2, &[("TZ", "Pacific/Kiritimati"), ("AT1_EVAL_T9_PROBE", "1")]);
        let first = fs::read(format!("{}/engine/{}.result", out, kp.replace('/', "_").trim_end_matches(".case"))).ok().and_then(|b| parse(&b).ok());
        let second = fs::read(&e2).ok().and_then(|b| parse(&b).ok());
        t9 = match (first, second) {
            (Some(a), Some(b)) if a.values_lines() == b.values_lines() && a.verdict_id == b.verdict_id && a.case.case_id == b.case.case_id && a.case.acceptance_id == b.case.acceptance_id && a.compute_evidence() == a.evidence_digest && b.compute_evidence() == b.evidence_digest => "PASS (case_id, acceptance_id, values block and verdict_id identical; each evidence_digest recomputes, so it differs whenever the covered bytes differ)".into(),
            (Some(_), Some(_)) => { t9ok = false; "FAIL (identities, values block or verdict_id differ between runs, or an evidence_digest does not recompute)".into() }
            _ => { t9ok = false; "FAIL (second run produced no parsable result)".into() }
        };
    }
    let gate = |b: bool| if b { "PASS" } else { "FAIL" };
    let mut summary = String::new();
    for (k, (p, f, ind)) in &counts {
        summary.push_str(&format!("# {}: PASS {} FAIL {} INDETERMINATE {}\n", k, p, f, ind));
    }
    summary.push_str(&format!("# AT1-G1 codec conformance (my codec, engine and oracle refusals exact; KAT digests copied): {}\n", gate(g1)));
    summary.push_str(&format!("# T9 wall-clock independence (engine run twice, TZ and environment changed): {}\n", t9));
    summary.push_str("# AT1-G2 isolation: not judged here; run gates/isolation.sh on the compute objects\n");
    summary.push_str(&format!("# AT1-G3 oracle calibration (every oracle record against the exact shadow, which reproduces the AT1_SPEC 13 hand tables in selftest): {}\n", gate(g3)));
    summary.push_str(&format!("# AT1-G4 positive arm (every POSITIVE case verifies PASS): {}\n", gate(g4)));
    summary.push_str(&format!("# AT1-G5 negative arm, candidate part (every NEGATIVE case FAILs with exactly its codes); mutant part: `at1-eval mutants`: {}\n", gate(g5)));
    table.push_str(&summary);
    let _ = fs::write(format!("{}/TABLE.tsv", out), table.as_bytes());
    let _ = fs::write(format!("{}/VERDICT_IDS.tsv", out), vids.as_bytes());
    print!("{}", table);
    if g1 && g3 && g4 && g5 && t9ok { 0 } else { 1 }
}

#[cfg(test)]
mod tests {
    #[test]
    fn selftest_passes() {
        assert_eq!(super::selftest(), 0);
    }
    #[test]
    fn hidden_generator_is_deterministic_and_valid() {
        // a throwaway seed (never the private one): same seed, same bytes; every case valid and within the margin rule
        let d1 = std::env::temp_dir().join("at1-eval-test-hidden-1");
        let d2 = std::env::temp_dir().join("at1-eval-test-hidden-2");
        let seed = "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";
        assert_eq!(crate::hidden::generate(seed, d1.to_str().unwrap()), 0);
        assert_eq!(crate::hidden::generate(seed, d2.to_str().unwrap()), 0);
        assert_eq!(crate::hidden::manifest(d1.to_str().unwrap()).unwrap(), crate::hidden::manifest(d2.to_str().unwrap()).unwrap());
        for e in std::fs::read_dir(&d1).unwrap() {
            let b = std::fs::read(e.unwrap().path()).unwrap();
            let c = crate::case::validate_bytes(&b).expect("hidden case valid");
            assert!(crate::hidden::margin_ok(&c).is_ok());
        }
        let _ = std::fs::remove_dir_all(&d1);
        let _ = std::fs::remove_dir_all(&d2);
    }
    #[test]
    fn scan_detects_patterns_in_a_crafted_elf() {
        // minimal ELF64 aarch64 relocatable with one executable section holding MRS CNTVCT_EL0 and SVC #0
        let mut b = vec![0u8; 64];
        b[0..4].copy_from_slice(b"\x7fELF");
        b[4] = 2;
        b[5] = 1;
        b[0x12] = 183;
        let text: [u8; 8] = [0x40, 0xe0, 0x3b, 0xd5, 0x01, 0x00, 0x00, 0xd4];
        let text_off = b.len();
        b.extend_from_slice(&text);
        let shoff = b.len();
        b[0x28..0x30].copy_from_slice(&(shoff as u64).to_le_bytes());
        b[0x3A] = 64;
        b[0x3C] = 2;
        b[0x3E] = 0;
        let mut sh0 = vec![0u8; 64]; // null section doubles as an empty string table
        sh0[24..32].copy_from_slice(&(0u64).to_le_bytes());
        let mut sh1 = vec![0u8; 64];
        sh1[4] = 1;
        sh1[8] = 0x6;
        sh1[24..32].copy_from_slice(&(text_off as u64).to_le_bytes());
        sh1[32..40].copy_from_slice(&(8u64).to_le_bytes());
        b.extend_from_slice(&sh0);
        b.extend_from_slice(&sh1);
        let hits = crate::scan::scan_bytes(&b).unwrap();
        assert_eq!(hits.len(), 2, "{:?}", hits);
        assert!(crate::scan::scan_bytes(b"not an object").is_err());
    }
}
