fn step() {}
fn accept() -> Option<u8> { None }

pub fn run_until_complete(max_steps: usize) {
    for _ in 0..max_steps {
        step();
    }
}

fn serve() {
    loop {
        let c = accept();
        if c.is_none() { return; }
    }
}

// 'a lifetimes and char literals must not confuse the scanner
fn lt<'a>(x: &'a str) -> char { let _ = x; '}' }
