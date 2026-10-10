//! Byte rules (AT1_CASE_V1 section 1) and the small token grammars shared by
//! case and result files.

/// Splits a file into lines after enforcing the byte rules. Err names the rule.
pub fn lines_checked(bytes: &[u8]) -> Result<Vec<String>, String> {
    if bytes.is_empty() {
        return Err("empty file".into());
    }
    for (i, &b) in bytes.iter().enumerate() {
        if b == b'\n' {
            continue;
        }
        if !(0x20..=0x7e).contains(&b) {
            return Err(format!("byte 0x{:02x} at offset {} not allowed", b, i));
        }
    }
    if *bytes.last().unwrap() != b'\n' {
        return Err("last line not LF-terminated".into());
    }
    let body = &bytes[..bytes.len() - 1];
    let mut out = Vec::new();
    for (n, raw) in body.split(|&b| b == b'\n').enumerate() {
        let s = String::from_utf8(raw.to_vec()).map_err(|_| "not ASCII".to_string())?;
        if s.is_empty() {
            return Err(format!("blank line {}", n + 1));
        }
        if s.starts_with(' ') || s.ends_with(' ') {
            return Err(format!("leading or trailing space on line {}", n + 1));
        }
        if s.contains("  ") {
            return Err(format!("double space on line {}", n + 1));
        }
        out.push(s);
    }
    Ok(out)
}

pub fn join_lf(lines: &[String]) -> Vec<u8> {
    let mut v = Vec::new();
    for l in lines {
        v.extend_from_slice(l.as_bytes());
        v.push(b'\n');
    }
    v
}

pub fn is_label(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty() && b.len() <= 32 && b[0].is_ascii_lowercase() && b.iter().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == b'_')
}

pub fn is_name(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty() && b.len() <= 64 && b[0].is_ascii_alphanumeric() && b.iter().all(|c| c.is_ascii_alphanumeric() || *c == b'_' || *c == b'.' || *c == b'-')
}

fn is_lower_hex(s: &str, n: usize) -> bool {
    s.len() == n && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
pub fn is_digest(s: &str) -> bool {
    is_lower_hex(s, 64)
}
pub fn is_hex40(s: &str) -> bool {
    is_lower_hex(s, 40)
}
/// <text>: 1..200 bytes of 0x20..0x7E, no leading or trailing space (byte rules already exclude other bytes).
pub fn is_text200(s: &str) -> bool {
    !s.is_empty() && s.len() <= 200 && !s.starts_with(' ') && !s.ends_with(' ')
}
pub fn is_path(s: &str) -> bool {
    !s.is_empty() && s.len() <= 200 && s.bytes().all(|c| (0x21..=0x7e).contains(&c))
}
pub fn is_repo(s: &str) -> bool {
    let ok = |p: &str| !p.is_empty() && p.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'.' || c == b'-');
    match s.split_once('/') {
        Some((a, b)) => ok(a) && ok(b),
        None => false,
    }
}
/// YYYY-MM-DDTHH:MM:SSZ with calendar ranges.
pub fn is_utc(s: &str) -> bool {
    let b = s.as_bytes();
    if b.len() != 20 || b[4] != b'-' || b[7] != b'-' || b[10] != b'T' || b[13] != b':' || b[16] != b':' || b[19] != b'Z' {
        return false;
    }
    let num = |a: usize, n: usize| -> Option<u32> {
        let t = &s[a..a + n];
        if t.bytes().all(|c| c.is_ascii_digit()) { t.parse().ok() } else { None }
    };
    match (num(0, 4), num(5, 2), num(8, 2), num(11, 2), num(14, 2), num(17, 2)) {
        (Some(_), Some(mo), Some(d), Some(h), Some(mi), Some(se)) => (1..=12).contains(&mo) && (1..=31).contains(&d) && h <= 23 && mi <= 59 && se <= 60,
        _ => false,
    }
}
pub fn is_code(s: &str) -> bool {
    let b = s.as_bytes();
    !b.is_empty() && b[0].is_ascii_uppercase() && b.iter().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit() || *c == b'_')
}

pub const FAILURE_CODES: [&str; 12] = [
    "BOUND_KIND_INSUFFICIENT",
    "CONDITIONAL_UNDEFINED",
    "CONSTRAINT_RESIDUAL_EXCEEDED",
    "INTERACTING_DEVIATION_EXCEEDED",
    "NONFINITE_VALUE",
    "ORACLE_DISAGREEMENT",
    "POVM_NORMALIZATION_EXCEEDED",
    "PRECISION_INSUFFICIENT",
    "PROBABILITY_OUT_OF_RANGE",
    "PROBABILITY_SUM_EXCEEDED",
    "SCHRODINGER_DEVIATION_EXCEEDED",
    "TRIVIAL_PHYSICAL_STATE",
];
pub const RUN_ERROR_CODES: [&str; 3] = ["ORACLE_UNAVAILABLE", "RESOURCE_LIMIT", "INTERNAL_ERROR"];

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn byte_rules() {
        assert!(lines_checked(b"a b\n").is_ok());
        assert!(lines_checked(b"a b").is_err());
        assert!(lines_checked(b"a  b\n").is_err());
        assert!(lines_checked(b"a b \n").is_err());
        assert!(lines_checked(b"a\n\nb\n").is_err());
        assert!(lines_checked(b"a\r\n").is_err());
        assert!(lines_checked(b"a\tb\n").is_err());
        assert!(lines_checked(b"").is_err());
        let mut s = FAILURE_CODES.to_vec();
        s.sort();
        assert_eq!(s, FAILURE_CODES.to_vec(), "closed set is listed sorted bytewise");
        assert!(is_utc("2026-10-10T02:00:00Z") && !is_utc("2026-13-10T02:00:00Z"));
        assert!(is_repo("aien-dev/omega") && !is_repo("aien dev/omega"));
    }
}
