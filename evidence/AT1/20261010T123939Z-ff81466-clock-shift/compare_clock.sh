#!/bin/sh
# Compare qualification runs under different wall clocks: verdict_ids, values blocks, timestamps, evidence digests.
# usage: sh compare_clock.sh <baseline-out> <shifted-out> <label>
B=$1; S=$2; L=$3
vals() { awk '/^begin values$/{f=1} f{print} /^end values$/{f=0}' "$1"; }
for set in qualify-hidden qualify; do
  n=$(grep -vc '^#' "$B/$set/VERDICT_IDS.tsv"); 
  if diff <(sort "$B/$set/VERDICT_IDS.tsv") <(sort "$S/$set/VERDICT_IDS.tsv") >/dev/null; then echo "$L $set verdict_id identical $n/$n"; else echo "$L $set verdict_id DIFFER"; fi
  same=0; diffc=0; dg_same=0; ts_shift=0; total=0
  for r in "$B/$set/spliced/"*.result; do
    c=$(basename "$r"); s="$S/$set/spliced/$c"; total=$((total+1))
    if cmp -s <(vals "$r") <(vals "$s"); then same=$((same+1)); else diffc=$((diffc+1)); fi
    [ "$(sed -n 's/^evidence_digest //p' "$r")" = "$(sed -n 's/^evidence_digest //p' "$s")" ] && dg_same=$((dg_same+1))
    yb=$(sed -n 's/^run_started_utc \([0-9]*\).*/\1/p' "$r"); ys=$(sed -n 's/^run_started_utc \([0-9]*\).*/\1/p' "$s"); [ -n "$yb" ] && [ -n "$ys" ] && [ "$yb" != "$ys" ] && ts_shift=$((ts_shift+1))
  done
  echo "$L $set values_block identical $same/$total (differ $diffc); evidence_digest same $dg_same/$total; run_started year shifted $ts_shift/$total"
done
echo "$L oracle records: $(for r in "$B/qualify-hidden/oracle/"*; do c=$(basename "$r"); cmp -s <(grep -v -E '^(run_started_utc|run_finished_utc|evidence_digest) ' "$r") <(grep -v -E '^(run_started_utc|run_finished_utc|evidence_digest) ' "$S/qualify-hidden/oracle/$c") && printf s || printf d; done | tr -cd s | wc -c)/$(ls "$B/qualify-hidden/oracle" | wc -l) identical ignoring time lines"
