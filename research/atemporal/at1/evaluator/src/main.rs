//! at1-eval: the AT-1 Agent 4 adversarial evaluator (std-only Rust, no Cargo).
//! Build: rustc --edition 2021 -O src/main.rs -o build/at1-eval

mod big;
mod case;
mod corpus;
mod dd;
mod hidden;
mod judge;
mod model;
mod mutants;
mod qualify;
mod rat;
mod result;
mod scan;
mod sha256;
mod synth;
mod text;
mod verify;

use std::fs;
use std::process::exit;

fn usage() -> ! {
    eprintln!(
        "usage: at1-eval <command> ...
  case <file> [--why]                     validate an AT1_CASE_V1 file (refusal: one stderr line, exit 2)
  shadow <case>                           print the shadow model's values for a case
  verify [--case F] [--oracle-record] <result>   verify a result (exit 0 PASS, 1 FAIL, 3 INCONCLUSIVE)
  splice <engine-result> <oracle-record> <out>   runner role: splice the four reference families
  synth-engine <case> <out> [mutant]      honest synthetic engine (or a named mutant)
  synth-oracle <case> <out> [mutant]      honest synthetic oracle record (or a named oracle mutant)
  corpus-gen <dir>                        write the public corpus and MANIFEST.tsv
  corpus-check <dir>                      check the corpus on disk is byte-identical to the generator
  selftest                                KAT digests, hand tables, codec rows, honest controls
  mutants <out.tsv>                       build the mutant kill receipt
  qualify --cases D --engine CMD --oracle CMD [--out D] [--limit N]   run candidates on a corpus
  hidden-gen <seed-hex-64> <outdir>       generate the hidden set from a private seed
  hidden-commit <dir>                     print the manifest commitment of a case directory
  scan-clock <object>...                  instruction scan for counter reads and raw syscalls
  sha256 <file>...                        plain SHA-256 of files"
    );
    exit(64)
}

fn read(p: &str) -> Vec<u8> {
    match fs::read(p) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("at1-eval: cannot read {}: {}", p, e);
            exit(66)
        }
    }
}
fn write(p: &str, b: &[u8]) {
    if let Some(parent) = std::path::Path::new(p).parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Err(e) = fs::write(p, b) {
        eprintln!("at1-eval: cannot write {}: {}", p, e);
        exit(73)
    }
}

pub fn print_report(rep: &verify::Report) {
    for f in &rep.findings {
        println!("{} {} {}", if f.sev == verify::Sev::Fail { "FAIL" } else { "INCONCLUSIVE" }, f.code, f.detail);
    }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 2 {
        usage();
    }
    let args: Vec<&str> = a[2..].iter().map(|s| s.as_str()).collect();
    match a[1].as_str() {
        "case" => {
            if args.is_empty() {
                usage();
            }
            match case::validate_bytes(&read(args[0])) {
                Ok(c) => println!("AT1_CASE_OK {} {} {}", c.case_id, c.acceptance_id, c.file_sha256),
                Err((code, why)) => {
                    eprintln!("AT1_CASE_REFUSED {}", code.code());
                    if args.contains(&"--why") {
                        println!("{}", why);
                    }
                    exit(2)
                }
            }
        }
        "shadow" => {
            if args.is_empty() {
                usage();
            }
            let c = match case::validate_bytes(&read(args[0])) {
                Ok(c) => c,
                Err((code, _)) => {
                    eprintln!("AT1_CASE_REFUSED {}", code.code());
                    exit(2)
                }
            };
            let sh = model::shadow(&c, &model::Mods::default(), false);
            println!("kernel_dim {} trivial {} interacting_exact {} ideal_exact {}", sh.kernel_dim, sh.trivial, sh.interacting_exact, sh.ideal_exact);
            println!("povm_residual {} ({:.17e})", model::show(&sh.povm_residual), sh.povm_residual.q.to_f64());
            for k in 0..c.m {
                let ide = &sh.ideal[k];
                let p = if sh.trivial { "undefined".to_string() } else { model::show(&sh.p[k]) };
                let pa = match sh.pauli.get(k).and_then(|x| x.as_ref()) {
                    Some(s) => format!("{} {} {}", model::show(&s[0][0]), model::show(&s[1][0]), model::show(&s[2][0])),
                    None => "undefined".into(),
                };
                println!("k {} {} p {} status {} pauli+ {} ideal+ {} {} {}", k, c.labels[k], p, sh.status[k].text(), pa, model::show(&ide[0][0]), model::show(&ide[1][0]), model::show(&ide[2][0]));
            }
        }
        "verify" => {
            let mut case_file: Option<Vec<u8>> = None;
            let mut role = verify::Role::Candidate;
            let mut file: Option<&str> = None;
            let mut i = 0;
            while i < args.len() {
                match args[i] {
                    "--case" => {
                        i += 1;
                        case_file = Some(read(args.get(i).copied().unwrap_or_else(|| usage())));
                    }
                    "--oracle-record" => role = verify::Role::Oracle,
                    f => file = Some(f),
                }
                i += 1;
            }
            let rep = verify::verify(&read(file.unwrap_or_else(|| usage())), case_file.as_deref(), role);
            print_report(&rep);
            println!("AT1_VERIFY {} outcome {} codes {}", rep.status(), rep.derived_outcome, if rep.derived_codes.is_empty() { "none".into() } else { rep.derived_codes.join(",") });
            exit(match rep.status() {
                "PASS" => 0,
                "FAIL" => 1,
                _ => 3,
            })
        }
        "splice" => {
            if args.len() != 3 {
                usage();
            }
            let e = result::parse(&read(args[0])).unwrap_or_else(|m| {
                eprintln!("at1-eval: engine result does not parse: {}", m);
                exit(1)
            });
            let o = result::parse(&read(args[1])).unwrap_or_else(|m| {
                eprintln!("at1-eval: oracle record does not parse: {}", m);
                exit(1)
            });
            match synth::splice(&e, &o) {
                Ok(r) => write(args[2], &r.bytes()),
                Err(m) => {
                    eprintln!("at1-eval: splice refused: {}", m);
                    exit(1)
                }
            }
        }
        "synth-engine" | "synth-oracle" => {
            if args.len() < 2 {
                usage();
            }
            let c = case::validate_bytes(&read(args[0])).unwrap_or_else(|(code, _)| {
                eprintln!("AT1_CASE_REFUSED {}", code.code());
                exit(2)
            });
            let name = args.get(2).copied().unwrap_or("honest");
            let r = if a[1] == "synth-engine" {
                let em = mutants::engine_mut(name).unwrap_or_else(|| {
                    eprintln!("at1-eval: unknown engine mutant {}", name);
                    exit(64)
                });
                synth::engine_result(&c, &em, &format!("at1-eval synth-engine {}", name))
            } else {
                let om = mutants::oracle_mut(name).unwrap_or_else(|| {
                    eprintln!("at1-eval: unknown oracle mutant {}", name);
                    exit(64)
                });
                synth::oracle_record(&c, &om, &format!("at1-eval synth-oracle {}", name))
            };
            write(args[1], &r.bytes());
        }
        "corpus-gen" => {
            if args.len() != 1 {
                usage();
            }
            let dir = args[0];
            let mut man = String::from("# path\tclass\texpected tool response (every parser must give it; refusals: one stderr line, exit 2)\n");
            for r in corpus::public_rows() {
                write(&format!("{}/{}", dir, r.path), &r.bytes);
                man.push_str(&format!("{}\t{}\t{}\n", r.path, r.class, r.expect));
            }
            write(&format!("{}/MANIFEST.tsv", dir), man.as_bytes());
            println!("corpus: {} files written to {}", corpus::public_rows().len(), dir);
        }
        "corpus-check" => {
            if args.len() != 1 {
                usage();
            }
            let mut bad = 0;
            let rows = corpus::public_rows();
            for r in &rows {
                let p = format!("{}/{}", args[0], r.path);
                if fs::read(&p).ok().as_deref() != Some(r.bytes.as_slice()) {
                    println!("DIFFERS {}", r.path);
                    bad += 1;
                }
            }
            println!("corpus-check: {} files, {} differ", rows.len(), bad);
            exit(if bad == 0 { 0 } else { 1 })
        }
        "selftest" => exit(qualify::selftest()),
        "mutants" => {
            if args.len() != 1 {
                usage();
            }
            exit(mutants::receipt(args[0]))
        }
        "qualify" => exit(qualify::qualify(&args)),
        "hidden-gen" => {
            if args.len() != 2 {
                usage();
            }
            exit(hidden::generate(args[0], args[1]))
        }
        "hidden-commit" => {
            if args.len() != 1 {
                usage();
            }
            exit(hidden::commit(args[0]))
        }
        "scan-clock" => exit(scan::main(&args)),
        "sha256" => {
            for p in &args {
                println!("{}  {}", sha256::sha256_hex(&read(p)), p);
            }
        }
        _ => usage(),
    }
}
