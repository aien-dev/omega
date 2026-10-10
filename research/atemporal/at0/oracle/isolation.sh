#!/bin/sh
# Symbol-isolation gate for the AT-0 oracle compute crate (src/at0/), Rust adaptation of the
# charter's nm gate.
# 1. Source rule: no compute file names a std::time, std::fs, std::process, std::thread,
#    std::net, std::env, random or clock facility.
# 2. Object rule: the compute crate compiled alone (rlib) has no undefined reference to a
#    clock, file, process, thread, network, environment or random symbol (nm -u over its
#    objects). Two symbol families are banned: libc entry points (the extern "C" route) and
#    mangled Rust std paths (the std route: a std::time call leaves no libc symbol in the
#    rlib, only a mangled std path; both the v0 form _RNv..NtCs<hash>_3std4time.. and the
#    legacy form _ZN3std4time.. contain the substring 3std4time).
# 3. Negative control: a copy of the crate with a hidden std::time read must be caught by the
#    source rule AND by the object rule on its own.
# Exit 0 PASS, 1 FAIL. Prints every offending line.
set -u
cd "$(dirname "$0")"
FLAGS="--edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -C panic=abort"
fail=0

echo "== source rule (src/at0/)"
if grep -n -E 'std::(time|fs|process|thread|net|env|io::std(in|out|err))|SystemTime|Instant::|getrandom|rand::|libc::|extern "C"' src/at0/*.rs; then
  echo "FAIL: forbidden facility named in compute code"; fail=1
else
  echo "ok: no clock, file, process, thread, network, environment or random facility named"
fi

echo "== object rule (nm -u on the compute rlib)"
./build.sh lib >/dev/null || { echo "FAIL: compute crate did not build alone"; exit 1; }
rm -rf target/iso && mkdir -p target/iso && (cd target/iso && ar x ../libat0.rlib) || { echo "FAIL: cannot unpack rlib"; exit 1; }
objs=$(ls target/iso/*.o 2>/dev/null)
[ -n "$objs" ] || { echo "FAIL: no objects in rlib"; exit 1; }
libc_names='clock_gettime|gettimeofday|^time$|nanosleep|getrandom|/dev/urandom|pthread_create|socket|connect|fopen|open64|openat|read64|write64|fork|execve|system|getenv'
std_paths='3std4time|3std2fs|3std7process|3std6thread|3std3net|3std3env|3std2io5stdio|3std6random'
banned="$libc_names|$std_paths"
object_rule() {
  nm -u "$@" 2>/dev/null | awk '{print $NF}' | sort -u | grep -E "$banned"
}
if object_rule $objs; then
  echo "FAIL: banned undefined symbol in compute objects"; fail=1
else
  echo "ok: no clock, random, thread, socket, file, process or environment symbol referenced (libc names and mangled std paths)"
fi

echo "== negative control: a copy that reads the clock must be caught"
rm -rf target/mutant && mkdir -p target/mutant && cp -r src/at0 target/mutant/at0
printf '\npub fn hidden_clock() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }\n' >> target/mutant/at0/mod.rs
if grep -q -E 'std::(time|fs|process|thread|net|env)' target/mutant/at0/*.rs; then
  echo "ok: mutant caught by the source rule"
else
  echo "FAIL: mutant not caught by the source rule"; fail=1
fi
# Agent 5 finding D5: the object rule must catch the std::time mutant on its own.
if rustc $FLAGS --crate-type lib --crate-name at0_mutant target/mutant/at0/mod.rs -o target/mutant/libat0_mutant.rlib 2>/dev/null \
   && rm -rf target/mutant/iso && mkdir -p target/mutant/iso && (cd target/mutant/iso && ar x ../libat0_mutant.rlib) \
   && object_rule target/mutant/iso/*.o >/dev/null; then
  echo "ok: mutant caught by the object rule (nm -u, mangled std path)"
else
  echo "FAIL: mutant not caught by the object rule"; fail=1
fi

[ $fail -eq 0 ] && echo "ISOLATION PASS" || echo "ISOLATION FAIL"
exit $fail
