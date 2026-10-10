//! AT-1 reference oracle, outer layer: the only place that touches files, the process
//! table and the wall clock. Everything that computes lives in src/at1/.
//!
//! Usage:
//!   at1-oracle emit <case-file> [-o <out-file>]   write an AT1_RESULT_V1 oracle record
//!   at1-oracle fixtures <dir>                     write the calibration fixtures
//!   at1-oracle check <case-file>                  validate only; print the ids
//! A refused case prints exactly `AT1_CASE_REFUSED <code>` on stderr and exits 2.

mod at1;

use at1::case::{parse_case, CONTRACT_COMMIT};
use at1::result::{compute, result_bytes, PROVENANCE_KEYS};
use at1::sha256::sha256_hex;
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

fn run_text(cmd: &str, args: &[&str]) -> Option<String> {
    let o = Command::new(cmd).args(args).output().ok().filter(|o| o.status.success())?;
    let s = String::from_utf8_lossy(&o.stdout).split_whitespace().collect::<Vec<_>>().join(" ");
    if s.is_empty() { None } else { Some(s) }
}

/// `host` (AT1_RESULT_V1 section 7: free text that, for a replication run, names the
/// machine and the OS): host name from /etc/hostname (Linux) or `uname -n` (macOS has no
/// /etc/hostname), then the OS product line from `sw_vers` when it exists (macOS) and
/// `uname -srm`. Whitespace runs collapse to one space; anything outside 0x20..0x7E becomes
/// `_`; at most 200 bytes.
fn host() -> String {
    let name = std::fs::read_to_string("/etc/hostname").ok().map(|s| s.trim().to_string()).filter(|s| !s.is_empty()).or_else(|| run_text("uname", &["-n"])).unwrap_or_else(|| "unknown".to_string());
    let mut parts = vec![name];
    if let (Some(p), Some(v)) = (run_text("sw_vers", &["-productName"]), run_text("sw_vers", &["-productVersion"])) {
        parts.push(format!("{} {}", p, v));
    }
    if let Some(u) = run_text("uname", &["-srm"]) {
        parts.push(u);
    }
    let raw = parts.join(" ").split_whitespace().collect::<Vec<_>>().join(" ");
    let mut clean: String = raw.chars().map(|c| if (' '..='~').contains(&c) { c } else { '_' }).collect();
    clean.truncate(200);
    let clean = clean.trim().to_string();
    if clean.is_empty() { "unknown".to_string() } else { clean }
}

fn exe_sha256() -> String {
    std::env::current_exe().ok().and_then(|p| std::fs::read(p).ok()).map(|b| sha256_hex(&b)).unwrap_or_else(|| "0".repeat(64))
}

fn emit(args: &[String]) -> i32 {
    let out_path = args.iter().position(|a| a == "-o").and_then(|i| args.get(i + 1)).cloned();
    let inputs: Vec<&String> = args.iter().filter(|a| !a.starts_with('-') && Some(*a) != out_path.as_ref()).collect();
    if inputs.len() != 1 {
        eprintln!("usage: at1-oracle emit <case-file> [-o <out-file>]");
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
            eprintln!("AT1_CASE_REFUSED {}", r.code);
            return 2;
        }
    };
    let computed = compute(&case);
    // source provenance is the state the binary was BUILT from (AT0_RESULT_V2 section 7),
    // captured by build.sh; a binary built without build.sh falls back to the checkout it
    // runs from and never claims a clean tree
    let (commit, clean) = match (option_env!("AT1_SOURCE_COMMIT"), option_env!("AT1_SOURCE_CLEAN")) {
        (Some(c), Some(k)) if c.len() == 40 => (Some(c.to_string()), k == "YES"),
        // built by build.sh outside a git checkout: no commit to name (all zeros, NO)
        (Some(_), Some(_)) => (None, false),
        _ => (git(&["rev-parse", "HEAD"]), false),
    };
    let exe = exe_sha256();
    let rustc = option_env!("AT1_RUSTC_VERSION").unwrap_or("rustc unknown (built without build.sh)");
    let flags = option_env!("AT1_BUILD_FLAGS").unwrap_or("unknown");
    let values: Vec<String> = vec![
        "aien-dev/omega".to_string(),
        commit.clone().unwrap_or_else(|| "0".repeat(40)),
        if commit.is_some() && clean { "YES" } else { "NO" }.to_string(),
        CONTRACT_COMMIT.to_string(),
        exe.clone(),
        "aien-dev/omega".to_string(),
        commit.unwrap_or_else(|| "0".repeat(40)),
        exe,
        format!("oracle {} {}", rustc, option_env!("AT1_RUSTC_HOST").unwrap_or("unknown-host")),
        format!("rustc {} src/main.rs", flags),
        host(),
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
    let rendered = at1::fixtures::render();
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
            eprintln!("AT1_CASE_REFUSED {}", r.code);
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
            eprintln!("usage: at1-oracle emit <case> [-o out] | fixtures <dir> | check <case>");
            1
        }
    };
    std::process::exit(code);
}

#[cfg(test)]
mod tests;
