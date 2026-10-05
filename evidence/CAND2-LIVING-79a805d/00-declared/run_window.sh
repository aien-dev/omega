#!/bin/bash
# CAND-2 qualification window: every lane of DECLARED-ATTEMPTS.md once, in the declared order.
# Started by quietlock hold (QUIETLOCK_HOLD is inherited by every lane). Never kills, never retries.
set -u
H=c62f47b5ac178311d7d8f91c4f52a298a1754ec8
W=$HOME/workspace/cand3-campaign/cand2
OUT=$HOME/workspace/evidence-out/CAND2-LIVING-79a805d
mkdir -p "$OUT/00-declared"
cp -a "$W/DECLARED-ATTEMPTS.md" "$W/cand2_ladder.sh" "$W/run_window.sh" "$OUT/00-declared/"
sha256sum "$W/DECLARED-ATTEMPTS.md" "$W/cand2_ladder.sh" "$W/run_window.sh" > "$OUT/00-declared/sha256.txt"
echo "QUIETLOCK_HOLD=${QUIETLOCK_HOLD:-unset} start=$(date -u +%FT%TZ)" > "$OUT/00-declared/window.txt"
for lane in ladder r11 chipwait m18 r15; do
  "$W/cand2_ladder.sh" "$H" "$lane"
  echo "lane $lane exit=$? at $(date -u +%FT%TZ)" >> "$OUT/00-declared/window.txt"
done
echo "window done $(date -u +%FT%TZ)" >> "$OUT/00-declared/window.txt"
