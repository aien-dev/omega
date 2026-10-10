// cmpvals.rs: AT-1 Agent 7 phase 2 value comparison. Rust std only.
// Compares the values block of each spliced AT1_RESULT_V1 file against the exact tables in the
// committed DETAILS.txt (hidden set 63b2edf0...). Usage: cmpvals <DETAILS.txt> <spliced-dir> <tol>
// For each case: kernel_dim, engine label and reference_label statuses (DEFINED iff exact p(k) > 0),
// clock_probability and reference_clock_probability against exact p(k), pauli PLUS/MINUS and
// reference_interacting against the exact conditional values, reference_ideal against the exact ideal.
use std::collections::BTreeMap;
use std::fs;

fn q(s: &str) -> f64 {
    let (a, b) = s.split_once('/').expect("rational");
    a.parse::<f64>().unwrap() / b.parse::<f64>().unwrap()
}
fn f64tok(s: &str) -> Option<f64> {
    let h = s.strip_prefix("f64:")?;
    Some(f64::from_bits(u64::from_str_radix(h, 16).ok()?))
}

struct Row { p: Option<f64>, plus: Option<[f64; 3]>, ideal: [f64; 3] }
struct Det { dim: usize, rows: Vec<Row> }

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let det = fs::read_to_string(&a[1]).unwrap();
    let dir = &a[2];
    let tol: f64 = a[3].parse().unwrap();
    let mut cases: BTreeMap<String, Det> = BTreeMap::new();
    let mut cur = String::new();
    for l in det.lines() {
        let t: Vec<&str> = l.split(' ').collect();
        if t[0] == "==" { cur = t[1].trim_end_matches(".case").to_string(); continue; }
        if t[0] == "kernel_dim" { cases.insert(cur.clone(), Det { dim: t[1].parse().unwrap(), rows: vec![] }); continue; }
        if t[0] == "k" {
            let p = if t[3] == "undefined" { None } else { Some(q(t[3])) };
            let pi = t.iter().position(|x| *x == "plusXYZ").unwrap();
            let ii = t.iter().position(|x| *x == "idealXYZ").unwrap();
            let plus = if t[pi + 1] == "undefined" { None } else { Some([q(t[pi + 1]), q(t[pi + 2]), q(t[pi + 3])]) };
            let ideal = [q(t[ii + 1]), q(t[ii + 2]), q(t[ii + 3])];
            cases.get_mut(&cur).unwrap().rows.push(Row { p, plus, ideal });
        }
    }
    let files: Vec<String> = fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
    let mut total_bad = 0;
    println!("# case\tkernel_dim\tvalues_compared\tmax_abs_dev_engine\tmax_abs_dev_oracle\tstatus_mismatches\tover_tol\tresult");
    for (name, d) in &cases {
        let f = match files.iter().find(|f| f.ends_with(&format!("_{}.result", name)) || **f == format!("{}.result", name)) {
            Some(f) => f, None => { println!("{}\t-\t0\t-\t-\t-\t-\tNO_RESULT_FILE", name); total_bad += 1; continue; }
        };
        let txt = fs::read_to_string(format!("{}/{}", dir, f)).unwrap();
        let (mut n, mut me, mut mo, mut stat_bad, mut over) = (0usize, 0f64, 0f64, 0usize, 0usize);
        let mut dim_ok = true;
        let mut cmp = |got: Option<f64>, want: f64, oracle: bool, n: &mut usize, over: &mut usize| {
            *n += 1;
            let dv = match got { Some(g) => (g - want).abs(), None => f64::INFINITY };
            if oracle { if dv > mo { mo = dv } } else if dv > me { me = dv }
            if !(dv <= tol) { *over += 1; }
        };
        let axis = |s: &str| match s { "X" => 0, "Y" => 1, _ => 2 };
        for l in txt.lines() {
            let t: Vec<&str> = l.split(' ').collect();
            match t[0] {
                "physical_state_kernel_dim" => { if t[1].parse::<usize>().unwrap() != d.dim { dim_ok = false; } }
                "label" | "reference_label" => {
                    let k: usize = t[1].parse().unwrap();
                    let want = if d.rows[k].p.map_or(false, |p| p > 0.0) { "DEFINED" } else { "UNDEFINED" };
                    if t[3] != want { stat_bad += 1; }
                }
                "clock_probability" | "reference_clock_probability" => {
                    let k: usize = t[1].parse().unwrap();
                    match d.rows[k].p { Some(p) => cmp(f64tok(t[2]), p, t[0].starts_with("reference"), &mut n, &mut over),
                        None => { if t[2] != "undefined" { stat_bad += 1; } } }
                }
                "pauli" | "reference_interacting" => {
                    let k: usize = t[1].parse().unwrap();
                    match d.rows[k].plus {
                        Some(pl) => { let x = pl[axis(t[2])]; let w = if t[3] == "PLUS" { x } else { 1.0 - x };
                                      cmp(f64tok(t[4]), w, t[0] == "reference_interacting", &mut n, &mut over); }
                        None => { if t[4] != "undefined" { stat_bad += 1; } }
                    }
                }
                "reference_ideal" => {
                    let k: usize = t[1].parse().unwrap();
                    let x = d.rows[k].ideal[axis(t[2])]; let w = if t[3] == "PLUS" { x } else { 1.0 - x };
                    cmp(f64tok(t[4]), w, true, &mut n, &mut over);
                }
                _ => {}
            }
        }
        if !dim_ok { stat_bad += 1; }
        let ok = stat_bad == 0 && over == 0;
        if !ok { total_bad += 1; }
        println!("{}\t{}\t{}\t{:.3e}\t{:.3e}\t{}\t{}\t{}", name, d.dim, n, me, mo, stat_bad, over, if ok { "AGREE" } else { "DISAGREE" });
    }
    println!("# cases {} disagreeing {} (tolerance {:e})", cases.len(), total_bad, tol);
    std::process::exit(if total_bad == 0 { 0 } else { 1 });
}
