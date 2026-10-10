//! In-code instruction scan for the isolation gate (AT1 charter G2): a symbol
//! scan cannot see an inline counter read or an inline system call, so this
//! reads the executable sections of ELF64 and Mach-O 64 files (objects,
//! executables, fat binaries and ar archives of them) and looks for
//!   aarch64: MRS from the generic-timer block (CNTVCT_EL0, CNTPCT_EL0, their
//!            self-synchronised forms, CNTFRQ_EL0 ...; word & 0xFFFFF000 == 0xD53BE000),
//!            MRS PMCCNTR_EL0 (0xD53B9D00 | Rt), and SVC #imm (word & 0xFFE0001F == 0xD4000001);
//!   x86_64:  RDTSC (0F 31), RDTSCP (0F 01 F9), SYSCALL (0F 05), SYSENTER (0F 34), INT 80h (CD 80).
//! aarch64 words are 4-byte aligned and exact; the x86 scan is bytewise and can
//! report a pattern that is only data inside a longer instruction (a false
//! positive, never a false negative for these encodings).

fn u16le(b: &[u8], o: usize) -> Option<u64> {
    b.get(o..o + 2).map(|x| u16::from_le_bytes([x[0], x[1]]) as u64)
}
fn u32le(b: &[u8], o: usize) -> Option<u64> {
    b.get(o..o + 4).map(|x| u32::from_le_bytes([x[0], x[1], x[2], x[3]]) as u64)
}
fn u32be(b: &[u8], o: usize) -> Option<u64> {
    b.get(o..o + 4).map(|x| u32::from_be_bytes([x[0], x[1], x[2], x[3]]) as u64)
}
fn u64le(b: &[u8], o: usize) -> Option<u64> {
    b.get(o..o + 8).map(|x| u64::from_le_bytes([x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]]))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Arch {
    A64,
    X64,
    Other,
}

/// (arch, section name, bytes) for every executable section.
fn exec_sections<'a>(b: &'a [u8], out: &mut Vec<(Arch, String, &'a [u8])>) -> Result<(), String> {
    if b.starts_with(b"\x7fELF") {
        if b.get(4) != Some(&2) || b.get(5) != Some(&1) {
            return Err("only little-endian ELF64 is supported".into());
        }
        let arch = match u16le(b, 0x12) {
            Some(183) => Arch::A64,
            Some(62) => Arch::X64,
            _ => Arch::Other,
        };
        let shoff = u64le(b, 0x28).ok_or("truncated ELF header")? as usize;
        let shentsize = u16le(b, 0x3A).ok_or("truncated")? as usize;
        let shnum = u16le(b, 0x3C).ok_or("truncated")? as usize;
        let shstrndx = u16le(b, 0x3E).ok_or("truncated")? as usize;
        let sh = |i: usize| shoff + i * shentsize;
        let stroff = u64le(b, sh(shstrndx) + 24).unwrap_or(0) as usize;
        for i in 0..shnum {
            let h = sh(i);
            let typ = u32le(b, h + 4).ok_or("truncated section header")?;
            let flags = u64le(b, h + 8).ok_or("truncated section header")?;
            if flags & 0x4 == 0 || typ == 8 {
                continue;
            }
            let off = u64le(b, h + 24).unwrap() as usize;
            let size = u64le(b, h + 32).unwrap() as usize;
            let nm = u32le(b, h).unwrap() as usize;
            let name: String = b.get(stroff + nm..).map(|s| s.iter().take_while(|&&c| c != 0).map(|&c| c as char).collect()).unwrap_or_default();
            out.push((arch, name, b.get(off..off + size).ok_or("section outside file")?));
        }
        return Ok(());
    }
    if b.starts_with(&[0xCF, 0xFA, 0xED, 0xFE]) {
        let arch = match u32le(b, 4) {
            Some(0x0100000C) => Arch::A64,
            Some(0x01000007) => Arch::X64,
            _ => Arch::Other,
        };
        let ncmds = u32le(b, 16).ok_or("truncated Mach-O header")? as usize;
        let mut p = 32usize;
        for _ in 0..ncmds {
            let cmd = u32le(b, p).ok_or("truncated load command")?;
            let size = u32le(b, p + 4).ok_or("truncated load command")? as usize;
            if cmd == 0x19 {
                let nsects = u32le(b, p + 64).ok_or("truncated segment")? as usize;
                for s in 0..nsects {
                    let q = p + 72 + s * 80;
                    let name: String = b.get(q..q + 16).ok_or("truncated section")?.iter().take_while(|&&c| c != 0).map(|&c| c as char).collect();
                    let sz = u64le(b, q + 40).ok_or("truncated section")? as usize;
                    let off = u32le(b, q + 48).ok_or("truncated section")? as usize;
                    let flags = u32le(b, q + 64).ok_or("truncated section")?;
                    if flags & 0x8000_0400 == 0 || flags & 0xff == 1 || off == 0 {
                        continue;
                    }
                    out.push((arch, name, b.get(off..off + sz).ok_or("section outside file")?));
                }
            }
            if size == 0 {
                return Err("zero-size load command".into());
            }
            p += size;
        }
        return Ok(());
    }
    if b.starts_with(&[0xCA, 0xFE, 0xBA, 0xBE]) {
        let n = u32be(b, 4).ok_or("truncated fat header")? as usize;
        for i in 0..n {
            let q = 8 + i * 20;
            let off = u32be(b, q + 8).ok_or("truncated fat arch")? as usize;
            let size = u32be(b, q + 12).ok_or("truncated fat arch")? as usize;
            exec_sections(b.get(off..off + size).ok_or("fat slice outside file")?, out)?;
        }
        return Ok(());
    }
    if b.starts_with(b"!<arch>\n") {
        let mut p = 8usize;
        while p + 60 <= b.len() {
            let hdr = &b[p..p + 60];
            let name = String::from_utf8_lossy(&hdr[0..16]).trim().to_string();
            let size: usize = String::from_utf8_lossy(&hdr[48..58]).trim().parse().map_err(|_| "bad ar member size")?;
            let mut data = b.get(p + 60..p + 60 + size).ok_or("ar member outside file")?;
            if let Some(n) = name.strip_prefix("#1/") {
                let skip: usize = n.parse().unwrap_or(0);
                data = data.get(skip..).unwrap_or(&[]);
            }
            if data.starts_with(b"\x7fELF") || data.starts_with(&[0xCF, 0xFA, 0xED, 0xFE]) {
                exec_sections(data, out)?;
            }
            p += 60 + size + (size & 1);
        }
        return Ok(());
    }
    Err("not an ELF64, Mach-O 64, fat or ar file".into())
}

pub fn scan_bytes(b: &[u8]) -> Result<Vec<String>, String> {
    let mut secs = Vec::new();
    exec_sections(b, &mut secs)?;
    let mut hits = Vec::new();
    for (arch, name, d) in secs {
        match arch {
            Arch::A64 => {
                for (i, w) in d.chunks_exact(4).enumerate() {
                    let w = u32::from_le_bytes([w[0], w[1], w[2], w[3]]);
                    let what = if w & 0xFFFF_F000 == 0xD53B_E000 {
                        Some("MRS from the generic timer (CNT*_EL0)")
                    } else if w & 0xFFFF_FFE0 == 0xD53B_9D00 {
                        Some("MRS PMCCNTR_EL0 (cycle counter)")
                    } else if w & 0xFFE0_001F == 0xD400_0001 {
                        Some("SVC (raw system call)")
                    } else {
                        None
                    };
                    if let Some(x) = what {
                        hits.push(format!("{} +0x{:x}: {:08x} {}", name, i * 4, w, x));
                    }
                }
            }
            Arch::X64 => {
                for i in 0..d.len().saturating_sub(1) {
                    let what = match (d[i], d[i + 1], d.get(i + 2).copied()) {
                        (0x0F, 0x31, _) => Some("RDTSC"),
                        (0x0F, 0x01, Some(0xF9)) => Some("RDTSCP"),
                        (0x0F, 0x05, _) => Some("SYSCALL"),
                        (0x0F, 0x34, _) => Some("SYSENTER"),
                        (0xCD, 0x80, _) => Some("INT 80h"),
                        _ => None,
                    };
                    if let Some(x) = what {
                        hits.push(format!("{} +0x{:x}: {} (byte pattern)", name, i, x));
                    }
                }
            }
            Arch::Other => hits.push(format!("{}: unsupported architecture, not scanned", name)),
        }
    }
    Ok(hits)
}

pub fn main(args: &[&str]) -> i32 {
    if args.is_empty() {
        eprintln!("usage: at1-eval scan-clock <object-or-binary>...");
        return 64;
    }
    let mut fail = false;
    for p in args {
        match std::fs::read(p).map_err(|e| e.to_string()).and_then(|b| scan_bytes(&b)) {
            Ok(h) if h.is_empty() => println!("CLEAN {}", p),
            Ok(h) => {
                fail = true;
                for x in h {
                    println!("HIT {} {}", p, x);
                }
            }
            Err(e) => {
                fail = true;
                println!("UNREADABLE {} {}", p, e);
            }
        }
    }
    println!("AT1_CLOCK_SCAN: {}", if fail { "FAIL" } else { "PASS" });
    if fail { 1 } else { 0 }
}
