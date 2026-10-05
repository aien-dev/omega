#!/usr/bin/env bash
# Clean reconstruction of CAND-1 from fresh GitHub clones and a fresh CARGO_HOME, compared with the frozen digests.
set -u
~/.local/bin/quietlock check || { echo "READY_TO_BUILD: quiet flag held"; exit 75; }
R=$HOME/workspace/cand2-campaign/recon; E=$HOME/workspace/evidence-out/CAND1-RECON-2eec75b; ORIG=$HOME/workspace/evidence-out/CAND1-BUILD-2eec75b/summary.txt
bash $R/cand1_recon_build.sh 2eec75b08aeb8f9d11f950070e13f6c2c046cf24; rc=$?
cp $R/cand1_recon_build.sh $R/run_recon.sh $E/ 2>/dev/null
: > $E/compare.txt
grep '^SAME ' "$ORIG" | while read _ name dig; do
  p=$(grep "^SAME $name " "$E/summary.txt" | cut -d' ' -f3)
  [ "$p" = "$dig" ] && echo "SAME $name $dig" >> $E/compare.txt || echo "DIFFERENT $name orig=$dig recon=${p:-missing}" >> $E/compare.txt
done
cat $E/compare.txt; echo "build rc=$rc"; echo "recon done $(date -u +%FT%TZ)" >> $E/compare.txt
