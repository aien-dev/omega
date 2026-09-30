#!/bin/sh
# R16-G3 authoritative path without legacy orchestrators
# (spec/r16-orchestrator-retirement.md §5 R16-G3).
#
# Usage: tools/r16_authpath.sh <mode> <r13-binary> <r14-binary> <outdir> <r13-sources...>
#   mode: host | silicon
#
# Checks, in order, and stops at the first failure:
#   1. sources: the living-system build lists only omega sources and the
#      pinned native AIENOS capability library; no omegatool, no SEQ reference
#      loop, no Rust, nothing from aien-sovereign-core or aegis-runtime.
#   2. link map: neither binary defines or references a legacy orchestrator
#      symbol (omegatool legacy_oracle / demonstration / gate runners, the SEQ
#      pulse loop, run_until_complete, Rust-mangled or aegis/sovereign names),
#      and neither needs a shared library beyond the C runtime.
#   3. no legacy program name is embedded in either binary as a string.
#   4. exec tracing: each binary runs under strace (execve only, seccomp
#      filter, so nothing else is slowed) with stub programs named after the
#      legacy binaries first on PATH. Every exec must be the test itself or a
#      helper on the allowlist (sh, rm, git: directory cleanup and the
#      receipt's commit lookup). Any other exec, or any stub being started,
#      fails.
#   5. the R13 living system and the R14 recovery run pass in that
#      configuration (host: HOST_PASS_NON_SILICON; silicon: PASS).
#
# The world-side hooks of the SEQ reference (rx_world_init_*sequential_
# reference, rx_world_seq_activate_*) are compiled into rx_world.c and are
# therefore present in the link map; they refuse a production world (R16-G4
# checks that). They are reported, not failed. The pulse loop that would drive
# them (rx_seq_pulse, rx_seq_run_until_complete) must be absent.
#
# Host mode never starts the graphics processor. Silicon mode is for the
# qualification run only: it must not be wrapped in `timeout` or killed
# (omega sessions rule 7), and this script never does either.
set -u

mode=${1:-}; r13=${2:-}; r14=${3:-}; out=${4:-}
if [ $# -lt 5 ] || { [ "$mode" != host ] && [ "$mode" != silicon ]; }; then
    echo "usage: $0 host|silicon <r13-binary> <r14-binary> <outdir> <r13-sources...>" >&2
    exit 2
fi
shift 4
mkdir -p "$out" || exit 2
fail() { echo "R16-G3 FAIL: $*"; echo "R16 gate: R16_G3_AUTHPATH=FAIL"; exit 1; }
note() { echo "R16-G3 $*"; }

# 1. sources -----------------------------------------------------------------
printf '%s\n' "$@" > "$out/sources.txt"
for s in "$@"; do
    case "$s" in
        tools/omegatool.c|*/omegatool.c) fail "source list links omegatool: $s" ;;
        *rx_seq_reference.c) fail "source list links the SEQ reference loop: $s" ;;
        *.rs|*sovereign*|*aegis-runtime*|*aegis_runtime*) fail "source list has a legacy Rust source: $s" ;;
        src/*.c|src/runtime/*.c|tests/runtime/*.c) ;;
        *libaienos_capability.a) ;;
        */m16/m16_native.c|*/nvrm/nvrm.c) [ "$mode" = silicon ] || fail "physical seat source in a host build: $s" ;;
        *) fail "source outside omega src/ and tests/runtime/: $s" ;;
    esac
done
note "sources: $# files, omega and the native AIENOS capability library only"

# 2. link map ----------------------------------------------------------------
legacy_sym='legacy_oracle|run_demonstration|omega_run_m[0-9]+_gates|omega_run_m4_gates|rx_seq_pulse|rx_seq_run_until_complete|run_until_complete|^_ZN|^_R[a-zA-Z0-9]|rust_|__rust|aegis_runtime|spark_aegis|sovereign|aien_runtime_spine|AienRuntimeSpine'
for b in "$r13" "$r14"; do
    [ -x "$b" ] || fail "binary missing: $b"
    n=$(basename "$b")
    nm "$b" > "$out/$n.nm" 2>&1 || fail "nm failed on $b"
    if awk '{print $NF}' "$out/$n.nm" | grep -Ei "$legacy_sym" > "$out/$n.legacy-symbols"; then
        fail "$n links legacy orchestrator symbols: $(tr '\n' ' ' < "$out/$n.legacy-symbols")"
    fi
    awk '{print $NF}' "$out/$n.nm" | grep -E 'sequential_reference|rx_world_seq_' \
        > "$out/$n.reference-hooks" || true
    ldd "$b" > "$out/$n.ldd" 2>&1 || fail "ldd failed on $b"
    if grep -vE 'linux-vdso|libc\.so|libm\.so|libpthread\.so|ld-linux|libdl\.so' "$out/$n.ldd" | grep -q .; then
        fail "$n needs a shared library outside the C runtime: $(grep -vE 'linux-vdso|libc\.so|libm\.so|libpthread\.so|ld-linux|libdl\.so' "$out/$n.ldd" | tr '\n' ' ')"
    fi
    note "link map $n: no legacy orchestrator symbol; C runtime libraries only; SEQ world hooks present (reference, refused on production worlds): $(wc -l < "$out/$n.reference-hooks")"
done

# 3. embedded program names ----------------------------------------------------
legacy_exe='spark-aegis|aegis-runtime|aien-cli|aien-runtime|aien-server|spark-dream|sovereign-core|omegatool'
for b in "$r13" "$r14"; do
    n=$(basename "$b")
    if strings -a "$b" | grep -E "$legacy_exe" > "$out/$n.legacy-strings"; then
        fail "$n embeds a legacy program name: $(head -3 "$out/$n.legacy-strings" | tr '\n' ' ')"
    fi
done
note "strings: no legacy program name embedded"

# 4 + 5. run under exec tracing, legacy programs stubbed ------------------------
stubs="$out/legacy-stubs"
mkdir -p "$stubs"
for p in spark-aegis aegis-runtime aien-cli aien-runtime aien-server spark-dream omegatool; do
    printf '#!/bin/sh\necho "$0 $*" >> "%s/legacy-exec.log"\nexit 97\n' "$out" > "$stubs/$p"
    chmod +x "$stubs/$p"
done
rm -f "$out/legacy-exec.log"

run_traced() {  # binary tag gate-regex
    b=$1; tag=$2; want=$3
    PATH="$stubs:$PATH" strace --seccomp-bpf -f -qq -e trace=execve,execveat \
        -o "$out/$tag.exec" "$b" > "$out/$tag.out" 2>&1
    rc=$?
    [ $rc -eq 0 ] || fail "$tag exited $rc (log $out/$tag.out)"
    grep -E "$want" "$out/$tag.out" > /dev/null || fail "$tag did not print its gate line ($want)"
    grep -oE 'execve(at)?\([^"]*"[^"]*"' "$out/$tag.exec" | sed 's/.*"\(.*\)"/\1/' \
        | sort | uniq -c > "$out/$tag.exec-summary"
    self=$(basename "$b")
    bad=$(awk '{print $2}' "$out/$tag.exec-summary" | while read -r e; do
        case "$(basename "$e")" in
            "$self"|sh|rm|git) ;;
            *) echo "$e" ;;
        esac
    done)
    [ -z "$bad" ] || fail "$tag exec'd programs outside the allowlist: $bad"
    [ ! -s "$out/legacy-exec.log" ] || fail "a legacy program stub was started: $(cat "$out/legacy-exec.log")"
    note "exec trace $tag: $(awk '{printf "%s x%s, ", $2, $1}' "$out/$tag.exec-summary")no legacy exec"
}

if [ "$mode" = host ]; then
    r13_want='R13 gate: R13_LIVING_SYSTEM=HOST_PASS_NON_SILICON'
    r14_want='R14 gate: R14_LIVING_RECOVERY=HOST_PASS_NON_SILICON'
else
    r13_want='R13 gate: R13_LIVING_SYSTEM=PASS'
    r14_want='R14 gate: R14_LIVING_RECOVERY=PASS'
fi
run_traced "$r13" r13 "$r13_want"
grep -E '^R13 (gate|costs|control E episode):' "$out/r13.out" | sed 's/^/R16-G3   /'
run_traced "$r14" r14 "$r14_want"
grep -E '^R14 [A-F]_[a-z_]+ +(PASS|FAIL)|^R14 gate:' "$out/r14.out" | sed 's/^/R16-G3   /'
if grep -E '^R14 [A-F]_[a-z_]+ +FAIL' "$out/r14.out" > /dev/null; then
    fail "an R14 recovery part failed"
fi

if [ "$mode" = host ]; then
    echo "R16 gate: R16_G3_AUTHPATH=HOST_PASS_NON_SILICON (host stand-in seat; the gate needs the silicon run)"
else
    echo "R16 gate: R16_G3_AUTHPATH=PASS"
fi
echo "R16-G3 evidence: $out"
