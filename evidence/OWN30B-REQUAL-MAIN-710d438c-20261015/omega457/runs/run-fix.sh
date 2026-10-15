#!/bin/bash
# variant=fix (omega-457-fix platform change only), same 4 cases x 5 rounds as run-all.sh
cd "$(dirname "$0")/.."
for r in 1 2 3 4 5; do for m in rewrite realloc; do for c in 0 1; do
  f=runs/fix-$m-c$c-r$r.log
  { echo "# $(date -u +%FT%TZ) ./l2_stale_test_fix --mode $m --cached $c --reps 3"; ./l2_stale_test_fix --mode $m --cached $c --reps 3; echo "# exit $?"; } > $f 2>&1
  grep -h '^STALE\|INVALID\|^FAIL' $f | sed "s|^|fix r$r |"
done; done; done
