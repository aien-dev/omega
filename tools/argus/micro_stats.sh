#!/bin/bash
# Micro-op stats for bench_micro.sh output: per config median/mean ops/s, op p50/p99, and the
# paired per-round throughput change vs cfg 0 (mean, t-based 95% CI, as % of cfg 0's mean).
# Usage: micro_stats.sh out.jsonl
jq -r 'select(.res != null) | [.cfg, .round, .res.ops_per_s, .res.op_p50_ns, .res.op_p99_ns] | @tsv' "$1" | sort -k1,1 -k3,3n | awk -F'\t' '
function tq(df) { return df >= 120 ? 1.980 : df >= 60 ? 2.000 : df >= 40 ? 2.021 : df >= 29 ? 2.045 : df >= 20 ? 2.086 : 2.262 }
{ v[$1, ++n[$1]] = $3; w[$1, $2] = $3; p50[$1] = p50[$1] " " $4; p99[$1] = p99[$1] " " $5; s[$1] += $3; if ($2 > R) R = $2 }
END {
  printf "%-5s %4s %12s %12s %9s %22s %7s  %s\n", "cfg", "n", "median", "mean", "vs0_mean", "95% CI (paired)", "halfw", "p50/p99 ns seen"
  for (k in n) {
    m = n[k]; med = (m % 2) ? v[k, (m + 1) / 2] : (v[k, m / 2] + v[k, m / 2 + 1]) / 2; mean = s[k] / m
    split(p50[k], a, " "); split(p99[k], b, " "); delete u50; delete u99; l50 = ""; l99 = ""
    for (i in a) if (!(a[i] in u50)) { u50[a[i]] = 1; l50 = l50 (l50 ? "," : "") a[i] }
    for (i in b) if (!(b[i] in u99)) { u99[b[i]] = 1; l99 = l99 (l99 ? "," : "") b[i] }
    if (k == "0") { printf "%-5s %4d %11.2fM %11.2fM %9s %22s %7s  %s/%s\n", k, m, med / 1e6, mean / 1e6, "-", "-", "-", l50, l99; base = mean; continue }
    out[k] = sprintf("%-5s %4d %11.2fM %11.2fM", k, m, med / 1e6, mean / 1e6); l[k] = l50 "/" l99
  }
  for (k in out) {
    t = 0; tt = 0; c = 0
    for (r = 1; r <= R; r++) if (((k, r) in w) && (("0", r) in w)) { d = w[k, r] - w["0", r]; t += d; tt += d * d; c++ }
    md = t / c; sd = sqrt((tt - t * t / c) / (c - 1)); hw = tq(c - 1) * sd / sqrt(c)
    printf "%s %+8.2f%% [%+6.2f%%, %+6.2f%%] %6.2f%%  %s\n", out[k], 100 * md / base, 100 * (md - hw) / base, 100 * (md + hw) / base, 100 * hw / base, l[k]
  } }' | sort -k1,1
