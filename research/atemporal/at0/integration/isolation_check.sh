#!/bin/sh
# AT-0 Agent 5: symbol isolation check (charter section 6, gate AT0-G2), written independently of
# the evaluator's gates/isolation.sh and of the oracle's isolation.sh. Works on C objects and on
# the members of a Rust rlib: any undefined symbol that names a clock, timer, random source,
# environment, file, socket, process, thread or dynamic loader fails the object.
#   [ALLOW=<regex of exempt symbol names>] sh isolation_check.sh <object.o>...   prints one line per hit; exit 0 clean, 1 hit, 2 unreadable
BAN='clock_gettime|clock_getres|clock_nanosleep|gettimeofday|^time$|timespec_get|^times$|localtime|gmtime|mktime|nanosleep|usleep|^sleep$|^alarm$|setitimer|^rand$|^random$|srand|drand48|arc4random|getrandom|getentropy|getenv|setenv|putenv|fopen|freopen|^open$|open64|openat|^creat|^read$|^write$|^fread$|^fwrite$|socket|^connect$|^accept|^bind$|^listen$|^send|^recv|getaddrinfo|dlopen|dlsym|^system$|popen|fork|^exec|posix_spawn|pthread_|thrd_|^clone$|^syscall$|3std4time|3std3env|3std2fs|3std3net|3std7process|3std6thread|3std2io5stdio|SystemTime|Instant'
rc=0
ALLOW=${ALLOW:-^$}
for o in "$@"; do
    [ -f "$o" ] || { echo "UNREADABLE $o"; rc=2; continue; }
    syms=$(nm -u "$o" 2>/dev/null) || { echo "UNREADABLE $o (nm failed)"; rc=2; continue; }
    hits=$(printf '%s\n' "$syms" | awk 'NF{print $NF}' | sed 's/@.*$//; s/^__//; s/_chk$//' | grep -E "$BAN" | grep -vE "$ALLOW" | sort -u | tr '\n' ' ')
    if [ -n "$hits" ]; then echo "HIT $o: $hits"; [ $rc -eq 0 ] && rc=1; fi
done
exit $rc
