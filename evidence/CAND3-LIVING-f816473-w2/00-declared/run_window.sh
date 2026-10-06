#!/bin/bash
# CAND-3 qualification window: every lane of DECLARED-ATTEMPTS.md once, in the declared order. Copy of CAND-2's.
# R15 is NOT in the lane list (declared NOT ATTEMPTED tonight). The lane order keeps chipwait (make clean) after
# ladder and r11, and m18 builds its own omegatool after it.
# Started by quietlock hold (QUIETLOCK_HOLD is inherited by every lane). Never kills, never retries.
set -u
. "$(dirname "$0")/cand3_env.sh" || exit 2
H=$HARNESS_SHA
export WINDOW_TAG=${WINDOW_TAG:?WINDOW_TAG not set (W1 is sealed; each window has its own directory)}
OUT=$EVID/CAND3-LIVING-$(echo "$OMEGA_FINAL" | cut -c1-7)-$WINDOW_TAG
mkdir -p "$OUT/00-declared"
cp -a "$W/DECLARED-ATTEMPTS.md" "$W/DECLARED-ATTEMPTS-$WINDOW_TAG.md" "$W/cand3_ladder.sh" "$W/run_window.sh" "$W/cand3_env.sh" "$OUT/00-declared/"
sha256sum "$W/DECLARED-ATTEMPTS.md" "$W/DECLARED-ATTEMPTS-$WINDOW_TAG.md" "$W/cand3_ladder.sh" "$W/run_window.sh" "$W/cand3_env.sh" > "$OUT/00-declared/sha256.txt"
echo "QUIETLOCK_HOLD=${QUIETLOCK_HOLD:-unset} start=$(date -u +%FT%TZ)" > "$OUT/00-declared/window.txt"
for lane in ladder r11 chipwait m18; do
  "$W/cand3_ladder.sh" "$H" "$lane"
  echo "lane $lane exit=$? at $(date -u +%FT%TZ)" >> "$OUT/00-declared/window.txt"
done
echo "window done $(date -u +%FT%TZ)" >> "$OUT/00-declared/window.txt"
