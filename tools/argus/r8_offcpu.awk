# Main-thread off-CPU time by blocking site (speed2). Input: `perf script --show-switch-events
# -F tid,time,event,ip,sym` of `perf record --no-inherit -e context-switches -c 1 -g --switch-events`
# runs, concatenated with a blank line between. Vars: cfg (label), R (runs). Output: cfg, us/run, waits/run, key.
# Off-CPU time of one thread: context-switches sample (callchain) -> next PERF_RECORD_SWITCH IN.
# Key: the first two non-libc user frames. Output: per key total us / R runs, count / R.
function libc(s) { return s ~ /^(_|lll_|futex|pthread|\[unknown\]|syscall|nanosleep|clock_nanosleep|waitpid|wait4|posix_spawn|read|write|fread|fopen|fclose|open|close|__)/ }
/PERF_RECORD_SWITCH IN/ { t = $2 + 0; if (pend != "") { d[pend] += t - ts; c[pend]++; pend = "" } next }
/PERF_RECORD_SWITCH OUT/ { next }
/context-switches:/ { ts = $2 + 0; inb = 1; k = ""; nk = 0; next }
inb && /^\t/ { if ($1 !~ /^ffff/) { s = $2; if (!libc(s) && nk < 2) { k = k (nk ? " <- " : "") s; nk++ } } next }
inb && /^$/ { inb = 0; pend = (k == "" ? "?" : k) }
END { for (x in d) printf "%s\t%.1f\t%.1f\t%s\n", cfg, d[x] * 1e6 / R, c[x] / R, x }
