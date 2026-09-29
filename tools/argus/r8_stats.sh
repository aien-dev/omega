#!/bin/bash
# Paired statistics for r8_wall_ab.sh output: per config mean/sd; for each config vs BASE the
# mean of per-round differences as % of BASE's mean, with a t-based 95% CI (t from df).
# Usage: r8_stats.sh out.jsonl BASE
jq -r '[.cfg, .round, .wall_us, .passed] | @tsv' "$1" | awk -v base="$2" -F'\t' '
function tq(df) { return df >= 120 ? 1.980 : df >= 60 ? 2.000 : df >= 40 ? 2.021 : df >= 30 ? 2.042 : df >= 20 ? 2.086 : 2.262 }
{ w[$1, $2] = $3; c[$1] = 1; if ($2 > R) R = $2; if ($4 != "true") bad[$1]++ }
END {
  for (k in c) { s = 0; ss = 0; m = 0; for (r = 1; r <= R; r++) if ((k, r) in w) { s += w[k, r]; ss += w[k, r]^2; m++ }
    mean[k] = s / m; sd[k] = sqrt((ss - s * s / m) / (m - 1)); cnt[k] = m }
  printf "%-12s %6s %10s %8s %9s %22s %8s\n", "cfg", "n", "mean_ms", "sd_ms", "vs_base", "95% CI (paired)", "halfw"
  for (k in c) {
    if (k == base) { printf "%-12s %6d %10.3f %8.3f %9s %22s %8s\n", k, cnt[k], mean[k]/1000, sd[k]/1000, "-", "-", "-"; continue }
    s = 0; ss = 0; m = 0
    for (r = 1; r <= R; r++) if (((k, r) in w) && ((base, r) in w)) { d = w[k, r] - w[base, r]; s += d; ss += d * d; m++ }
    md = s / m; sdd = sqrt((ss - s * s / m) / (m - 1)); hw = tq(m - 1) * sdd / sqrt(m)
    printf "%-12s %6d %10.3f %8.3f %+8.2f%% [%+6.2f%%, %+6.2f%%] %7.2f%%%s\n", k, cnt[k], mean[k]/1000, sd[k]/1000,
      100 * md / mean[base], 100 * (md - hw) / mean[base], 100 * (md + hw) / mean[base], 100 * hw / mean[base],
      (bad[k] ? "  FAILED RUNS " bad[k] : "")
  } }' | sort -k1,1
