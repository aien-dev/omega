#!/bin/sh
# compare.sh -- R15 hwchar sustain/burst comparison table (POSIX sh + awk).
# Replaces compare.py (no Python in repos); output is byte-identical on the
# committed runs/ data.  Usage: research/r15-hwchar/compare.sh
LC_ALL=C; export LC_ALL
R="$(cd "$(dirname "$0")" && pwd)/runs"

# summ FILE: per (cpu,kind) summary of mode=sustain rows; silent if missing.
summ() {
  [ -f "$1" ] || return 0
  awk '
  function get(l, k,   s, i, v) {
    s = "\"" k "\":"; i = index(l, s); if (!i) return ""
    v = substr(l, i + length(s)); sub(/^[ \t]*/, "", v)
    if (substr(v, 1, 1) == "\"") { v = substr(v, 2); sub(/".*/, "", v); return v }
    sub(/[,}].*/, "", v); sub(/[ \t]+$/, "", v); return v
  }
  function keyless(a, b) {          # Python tuple order: (cpu int, kind str)
    if (kc[a] + 0 != kc[b] + 0) return kc[a] + 0 < kc[b] + 0
    return kk[a] < kk[b]
  }
  {
    l = $0; sub(/^[ \t\r\n\f\v]+/, "", l)
    if (substr(l, 1, 1) != "{") next
    if (get(l, "mode") != "sustain") next
    c = get(l, "cpu"); k = get(l, "kind"); key = c SUBSEP k
    if (!(key in seen)) { seen[key] = 1; nk++; keys[nk] = key; kc[key] = c; kk[key] = k }
    n = ++cnt[key]
    sec[key, n] = get(l, "second"); ghz[key, n] = get(l, "eff_ghz") + 0
    w = get(l, "pkg_w"); pw[key, n] = (w == "" || w == "null") ? 0 : w + 0
    mt[key, n] = get(l, "max_temp_mc") + 0
  }
  END {
    for (i = 2; i <= nk; i++) { t = keys[i]; for (j = i - 1; j >= 1 && keyless(t, keys[j]); j--) keys[j + 1] = keys[j]; keys[j + 1] = t }
    for (q = 1; q <= nk; q++) {
      key = keys[q]; n = cnt[key]
      for (i = 1; i <= n; i++) ord[i] = i
      for (i = 2; i <= n; i++) {     # stable insertion sort by second
        t = ord[i]
        for (j = i - 1; j >= 1 && sec[key, ord[j]] + 0 > sec[key, t] + 0; j--) ord[j + 1] = ord[j]
        ord[j + 1] = t
      }
      drop = "None"
      if (kc[key] + 0 >= 5) for (i = 1; i <= n; i++) if (ghz[key, ord[i]] < 3.7) { drop = sec[key, ord[i]]; break }
      se = 0; sw = 0; sl = 0; slw = 0; nl = 0; mn = ""; mx = ""
      for (i = 1; i <= n; i++) {
        g = ghz[key, ord[i]]; w = pw[key, ord[i]]; m = mt[key, ord[i]]
        if (i <= 10) { se += g; sw += w }
        if (i > 15) { sl += g; slw += w; nl++ }
        if (mn == "" || g < mn) mn = g
        if (mx == "" || m > mx) mx = m
      }
      ne = (n < 10 ? n : 10); if (ne < 1) ne = 1
      late = nl ? sl / nl : 0
      lg = late ? sprintf("%.3f", late) : "nan"
      lw = nl ? sprintf("%.1f", slw / nl) : "nan"
      printf "   cpu%2d %-5s secs=%3d GHz first10=%.3f after15=%s min=%.3f  drop_at_s=%s  W first10=%.1f after15=%s  maxT=%.1fC\n", kc[key], kk[key], n, se / ne, lg, mn, drop, sw / ne, lw, mx / 1000
    }
  }' "$1"
}

for p in "AFTERNOON hwchar-20260928" "POST-POWER-OFF hwchar-postboot-unloaded"; do
  tag=${p% *}; d="$R/${p#* }"
  for secs in 30 120; do
    echo "== $tag sustain-$secs"; summ "$d/sustain-$secs.jsonl"
  done
done
for p in "AFTERNOON burst|hwchar-burst-20260928" "POST burst unloaded|hwchar-burst-postboot-unloaded" "POST burst loaded|hwchar-burst-postboot-loaded"; do
  tag=${p%|*}; d="$R/${p#*|}"
  echo "== $tag"
  for f in "$d"/*.jsonl; do
    [ -f "$f" ] || continue
    echo "   $(basename "$f")"; summ "$f"
  done
done
