#!/bin/bash
# 5 rounds; each round: rewrite/cached0 (baseline, negative control), rewrite/cached1, realloc/cached0, realloc/cached1. One process each.
cd "$(dirname "$0")/.."
for r in 1 2 3 4 5; do
  for m in rewrite realloc; do for c in 0 1; do
    f=runs/$m-c$c-r$r.log
    { echo "# $(date -u +%FT%TZ) ./l2_stale_test --mode $m --cached $c --reps 3"; ./l2_stale_test --mode $m --cached $c --reps 3; echo "# exit $?"; } > $f 2>&1
    grep -h '^STALE\|INVALID\|^FAIL' $f | sed "s|^|r$r |"
  done; done
done
