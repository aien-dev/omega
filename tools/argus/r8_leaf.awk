# Main-thread samples by leaf and caller (speed2). Input: `perf script -F event,ip,sym` of
# `perf record --no-inherit -g -e instructions:u,cycles -c 50000` runs. Vars: cfg, R (runs).
# Output: cfg, event, M events/run (period 50000), "[k] " if the leaf is kernel, leaf <- caller.
# blocks: header line "event:" then frames "\taddr sym (dso)". Key = [k] if leaf is kernel, then first user sym <- its caller.
function flush() { if (ev == "") return; key = (kern ? "[k] " : "") u1 " <- " u2; n[ev "\t" key]++; t[ev]++; ev = "" }
/^ *armv8/ { flush(); ev = ($1 ~ /instr/) ? "instr" : "cycles"; first = 1; kern = 0; u1 = "?"; u2 = "?"; nu = 0; next }
/^\t/ { a = $1; s = $2; if (first) { kern = (a ~ /^ffff/); first = 0 }
        if (a !~ /^ffff/) { nu++; if (nu == 1) u1 = s; else if (nu == 2) u2 = s } next }
/^$/ { flush() }
END { flush(); for (k in n) { split(k, x, "\t"); printf "%s\t%s\t%.3f\t%s\n", cfg, x[1], n[k] * 50000 / R / 1e6, x[2] }
      for (e in t) printf "%s\t%s\t%.3f\tTOTAL\n", cfg, e, t[e] * 50000 / R / 1e6 }
