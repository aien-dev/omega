#!/bin/sh
# AT-0 evaluator isolation gate (charter section 3, gate AT0-G2 method; adapted
# from omega tests/physics0/isolation.sh). Compute objects of the candidate
# (engine, oracle) must reference no clock, randomness, environment, file,
# process, socket or thread symbol. Works on .o files and on linked executables
# (C or Rust) via nm; undefined (U) and defined (T/W) references both count.
#   sh gates/isolation.sh <object-or-binary>...
# Prints AT0_ISOLATION: PASS|FAIL and one line per hit. Exit 0 on PASS.
fail=0
FORBIDDEN="clock_gettime clock_getres clock_nanosleep clock gettimeofday time timegm timelocal localtime localtime_r gmtime gmtime_r mktime strftime nanosleep sleep usleep rand rand_r srand random srandom drand48 lrand48 arc4random getrandom getentropy getenv secure_getenv setenv putenv fopen fopen64 freopen open open64 openat openat64 creat socket connect accept bind listen send recv sendto recvfrom getaddrinfo gethostbyname dlopen dlsym system popen fork vfork posix_spawn execv execve execvp execl pthread_create thrd_create clone"
for o in "$@"; do
    [ -f "$o" ] || { echo "missing $o"; fail=1; continue; }
    syms=$(nm -p "$o" 2>/dev/null | awk 'NF>=2 {print $NF}' | sed 's/@.*$//')
    for bad in $FORBIDDEN; do
        if echo "$syms" | grep -qx "_\{0,1\}${bad}" ; then echo "FAIL: $o references $bad"; fail=1; fi
    done
    # Rust std time/random/env/fs/net/process/thread paths in mangled names
    if nm -p "$o" 2>/dev/null | grep -qE 'std\.\.time|std\.\.env|std\.\.fs|std\.\.net|std\.\.process|std\.\.thread|rand\.\.|getrandom' ; then
        echo "FAIL: $o references a Rust std time/env/fs/net/process/thread or rand path"; fail=1
    fi
done
if [ $fail = 0 ]; then echo "AT0_ISOLATION: PASS ($# objects clean)"; else echo "AT0_ISOLATION: FAIL"; fi
exit $fail
