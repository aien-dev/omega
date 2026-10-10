#!/bin/sh
# AT-1 isolation gate (charter section 6, G2; adapted from the AT-0 Agent 4
# gate). Two independent scans of each compute object or executable:
#  1. symbol scan (nm): no clock, randomness, environment, file, network,
#     process, thread or raw-syscall entry point, C or Rust std;
#  2. instruction scan (at1-eval scan-clock): no inline counter read
#     (CNTVCT_EL0, CNTPCT_EL0, PMCCNTR_EL0, RDTSC) and no inline system call
#     (SVC, SYSCALL), which no symbol table shows.
# Portable POSIX sh: works with GNU nm and the llvm nm of Apple clang.
#   sh gates/isolation.sh <at1-eval> <object-or-binary>...    judge candidate objects
#   sh gates/isolation.sh --controls <at1-eval> [receipt]     prove the gate on its controls
# Exit 0 only on PASS.
set -u
FORBIDDEN="clock_gettime clock_getres clock_nanosleep clock gettimeofday time timegm timelocal localtime localtime_r gmtime gmtime_r mktime strftime nanosleep sleep usleep mach_absolute_time mach_continuous_time clock_gettime_nsec_np rand rand_r srand random srandom drand48 lrand48 arc4random arc4random_buf getrandom getentropy getenv secure_getenv setenv putenv fopen fopen64 freopen open open64 openat openat64 creat socket connect accept bind listen send recv sendto recvfrom getaddrinfo gethostbyname dlopen dlsym system popen fork vfork posix_spawn execv execve execvp execl pthread_create thrd_create clone syscall __syscall read write pread pwrite pread64 pwrite64 readv writev fread fwrite fgets fgetc getc getchar getline getdelim fscanf scanf mmap mmap64 ioctl"

scan_one() { # $1 = at1-eval, $2 = object; prints hits, returns 1 on any
    o=$2; bad=0
    if [ ! -f "$o" ]; then echo "FAIL: missing $o"; return 1; fi
    t=$(mktemp "${TMPDIR:-/tmp}/at1iso.XXXXXX") || return 1
    { nm -p "$o" 2>/dev/null; nm -D "$o" 2>/dev/null; } > "$t"
    if [ ! -s "$t" ] && ! nm "$o" >/dev/null 2>&1; then echo "FAIL: $o: nm cannot read symbols"; rm -f "$t"; return 1; fi
    # strip version suffixes and hardened-libc aliases (__fread_chk -> fread, __open_2 -> open)
    syms=$(awk 'NF>=2 {print $NF}' "$t" | sed -E 's/@.*$//; s/^_?__([a-z0-9_]+)_chk$/\1/; s/^_?__([a-z0-9_]+)_2$/\1/')
    for f in $FORBIDDEN; do
        if printf '%s\n' "$syms" | grep -qx "_\{0,1\}$f"; then echo "FAIL: $o references $f"; bad=1; fi
    done
    { nm -C -p "$o" 2>/dev/null; nm -C -D "$o" 2>/dev/null; } >> "$t"
    if grep -qE '3std4time|3std3env|3std2fs|3std3net|3std7process|3std6thread|3std2io5stdio|10SystemTime|7Instant|getrandom|std::(time|env|fs|net|process|thread|io::stdio)|SystemTime|Instant::|rand::' "$t"; then
        echo "FAIL: $o references a Rust std time/env/fs/net/process/thread or rand path"; bad=1
    fi
    rm -f "$t"
    if ! "$1" scan-clock "$o" | sed -n 's/^HIT /FAIL: instruction /p; s/^UNREADABLE /FAIL: unreadable /p' | grep . ; then :; else bad=1; fi
    return $bad
}

if [ "${1:-}" = "--controls" ]; then
    EVAL=${2:?usage: isolation.sh --controls <at1-eval> [receipt]}
    OUT=${3:-results/ISOLATION_RECEIPT.txt}
    D=$(mktemp -d "${TMPDIR:-/tmp}/at1gate.XXXXXX") || exit 1
    here=$(dirname "$0")
    CC=${CC:-cc}
    ok=1
    {
        echo "# AT-1 G2 gate controls: $(uname -s) $(uname -m), CC=$CC ($($CC --version 2>/dev/null | head -n 1))"
        for c in clean_control hidden_clock_mutant counter_mutant raw_syscall_mutant; do
            if ! $CC -c -O1 -o "$D/$c.o" "$here/$c.c" 2>"$D/$c.err"; then
                echo "$c	NOT_BUILT	$(head -n 1 "$D/$c.err")"; ok=0; continue
            fi
            hits=$(scan_one "$EVAL" "$D/$c.o")
            if [ -z "$hits" ]; then got=PASS; else got=FAIL; fi
            if [ "$c" = clean_control ]; then want=PASS; else want=FAIL; fi
            if [ "$got" = "$want" ]; then v=AS_INTENDED; else v=WRONG; ok=0; fi
            echo "$c	gate=$got	want=$want	$v	$(printf '%s' "$hits" | tr '\n' ';')"
        done
        # the symbol scan alone must be blind to the two inline mutants (that is why the instruction scan exists)
        for c in counter_mutant raw_syscall_mutant; do
            [ -f "$D/$c.o" ] || continue
            if scan_one true "$D/$c.o" >/dev/null 2>&1; then echo "$c	symbol-scan-only=PASS (blind, as expected)"; else echo "$c	symbol-scan-only=FAIL"; fi
        done
        if [ $ok = 1 ]; then echo "AT1_ISOLATION_CONTROLS: PASS"; else echo "AT1_ISOLATION_CONTROLS: FAIL"; fi
    } | sed "s|$D/||g" > "$D/receipt"
    mkdir -p "$(dirname "$OUT")"
    cp "$D/receipt" "$OUT"
    cat "$OUT"
    rm -rf "$D"
    # ok is set inside the piped block (a subshell), so the verdict is read back from the receipt
    grep -qx "AT1_ISOLATION_CONTROLS: PASS" "$OUT"
    exit $?
fi

EVAL=${1:?usage: isolation.sh <at1-eval> <object>...}
shift
fail=0
for o in "$@"; do scan_one "$EVAL" "$o" || fail=1; done
if [ $fail = 0 ]; then echo "AT1_ISOLATION: PASS ($# objects clean)"; else echo "AT1_ISOLATION: FAIL"; fi
exit $fail
