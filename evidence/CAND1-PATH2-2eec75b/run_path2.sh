#!/usr/bin/env bash
# Second-path build of CAND-1 (task B). Refuses to run while the spark quiet flag is held.
set -u
~/.local/bin/quietlock check || { echo "READY_TO_BUILD: quiet flag held"; exit 75; }
P=$HOME/workspace/cand2-campaign/path2; E=$HOME/workspace/evidence-out/CAND1-PATH2-2eec75b; ORIG=$HOME/workspace/evidence-out/CAND1-BUILD-2eec75b/summary.txt
bash $P/cand1_path2_build.sh 2eec75b08aeb8f9d11f950070e13f6c2c046cf24; rc=$?
cp $P/cand1_path2_build.sh $P/run_path2.sh $E/ 2>/dev/null
: > $E/compare.txt
grep '^SAME ' "$ORIG" | while read _ name dig; do
  p=$(grep "^SAME $name " "$E/summary.txt" | cut -d' ' -f3)
  [ "$p" = "$dig" ] && echo "SAME $name $dig" >> $E/compare.txt || echo "DIFFERENT $name orig=$dig path2=${p:-missing}" >> $E/compare.txt
done
cat $E/compare.txt; echo "build rc=$rc"
