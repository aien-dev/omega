#!/bin/sh
# Isolation gate for the Rust oracle, adapted from tests/physics0/isolation.sh (C, nm on objects).
# 1. Source rule: the compute crate src/at0/ names no std::time, std::fs, std::process,
#    std::thread, std::net, std::env, random or clock facility.
# 2. Object rule: the compute crate compiled alone (rlib) has no undefined reference to a
#    clock, file, process, socket, thread or random symbol (nm -u over its objects).
# Exit 0 PASS, 1 FAIL. Prints every offending line.
set -u
cd "$(dirname "$0")"
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
banned='clock_gettime|gettimeofday|^time$|nanosleep|getrandom|/dev/urandom|pthread_create|socket|connect|fopen|open64|openat|read64|write64|fork|execve|system|getenv'
if nm -u $objs 2>/dev/null | awk '{print $NF}' | sort -u | grep -E "$banned"; then
  echo "FAIL: banned undefined symbol in compute objects"; fail=1
else
  echo "ok: no clock, random, thread, socket, file, process or environment symbol referenced"
fi
echo "== negative control: a copy that reads the clock must be caught"
rm -rf target/mutant && mkdir -p target/mutant && cp -r src/at0 target/mutant/at0
printf '\npub fn hidden_clock() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }\n' >> target/mutant/at0/mod.rs
if grep -q -E 'std::(time|fs|process|thread|net|env)' target/mutant/at0/*.rs; then
  echo "ok: mutant caught by the source rule"
else
  echo "FAIL: mutant not caught"; fail=1
fi
[ $fail -eq 0 ] && echo "ISOLATION PASS" || echo "ISOLATION FAIL"
exit $fail
