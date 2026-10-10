#!/bin/sh
# AT-0 evaluator isolation gate (charter section 3, gate AT0-G2 method; adapted
# from omega tests/physics0/isolation.sh). Compute objects of the candidate
# (engine, oracle) must reference no clock, randomness, environment, file,
# process, socket or thread symbol. Works on .o files and on linked executables
# (C or Rust) via nm static and dynamic tables; undefined and defined references both count.
# Limits: inline syscalls, raw timer reads (CNTVCT_EL0 / rdtsc) and symbols resolved at run time
# through dlsym-free tricks are NOT visible to a symbol scan (see ISOLATION_REPORT.md).
#   sh gates/isolation.sh <object-or-binary>...
# Prints AT0_ISOLATION: PASS|FAIL and one line per hit. Exit 0 on PASS.
fail=0
FORBIDDEN="clock_gettime clock_getres clock_nanosleep clock gettimeofday time timegm timelocal localtime localtime_r gmtime gmtime_r mktime strftime nanosleep sleep usleep rand rand_r srand random srandom drand48 lrand48 arc4random getrandom getentropy getenv secure_getenv setenv putenv fopen fopen64 freopen open open64 openat openat64 creat socket connect accept bind listen send recv sendto recvfrom getaddrinfo gethostbyname dlopen dlsym system popen fork vfork posix_spawn execv execve execvp execl pthread_create thrd_create clone syscall"
for o in "$@"; do
    [ -f "$o" ] || { echo "missing $o"; fail=1; continue; }
    # static (-p) and dynamic (-D) tables; a stripped executable keeps only its dynamic imports
    if ! nm -p "$o" >"$o.nm" 2>/dev/null && ! nm -D "$o" >"$o.nm" 2>/dev/null; then echo "FAIL: $o: nm cannot read symbols (not an ELF object?)"; fail=1; rm -f "$o.nm"; continue; fi
    { nm -p "$o" 2>/dev/null; nm -D "$o" 2>/dev/null; } > "$o.nm"
    syms=$(awk 'NF>=2 {print $NF}' "$o.nm" | sed 's/@.*$//')
    for bad in $FORBIDDEN; do
        if echo "$syms" | grep -qx "_\{0,1\}${bad}" ; then echo "FAIL: $o references $bad"; fail=1; fi
    done
    # Rust std time/random/env/fs/net/process/thread paths: legacy (_ZN3std4time..) and v0 (_RNv..3std4time..) mangling,
    # plus the demangled spelling (nm -C), so a toolchain change in mangling cannot blind the gate
    { nm -C -p "$o" 2>/dev/null; nm -C -D "$o" 2>/dev/null; } >> "$o.nm"
    if grep -qE '3std4time|3std3env|3std2fs|3std3net|3std7process|3std6thread|3std2io5stdio|10SystemTime|7Instant|4rand[0-9A-Za-z_]*|getrandom|std::(time|env|fs|net|process|thread|io::stdio)|SystemTime|Instant::|rand::' "$o.nm"; then
        echo "FAIL: $o references a Rust std time/env/fs/net/process/thread or rand path"; fail=1
    fi
    rm -f "$o.nm"
done
if [ $fail = 0 ]; then echo "AT0_ISOLATION: PASS ($# objects clean)"; else echo "AT0_ISOLATION: FAIL"; fi
exit $fail
