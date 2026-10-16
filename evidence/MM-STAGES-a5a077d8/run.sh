#!/bin/bash
EV=/home/drakestapleton/workspace/omega-evidence-e2/STAGES-a5a077d8; OM=$HOME/workspace/omega-mm-e2
{ echo "omega $(git -C $OM rev-parse HEAD) dirty=$(git -C $OM status --porcelain --untracked-files=no | wc -l)"; echo "red = HEAD tests + 42d30fac matmul_api.c (stub)"; sha256sum $EV/api_test.red $EV/api_test.green; echo "started $(date -u +%FT%TZ)"; } > $EV/ground.txt
ps aux --sort=-%cpu | head -8 | cut -c1-160 > $EV/ps-before.txt
timeout 300 $EV/api_test.red --out $EV/red-sweep.json > $EV/red-sweep.log 2>&1; echo "red sweep exit $?" >> $EV/ground.txt
timeout 300 $EV/api_test.green --out $EV/green-sweep.json > $EV/green-sweep.log 2>&1; echo "green sweep exit $?" >> $EV/ground.txt
timeout 300 $EV/api_test.green --stages --cta-budget 1024 --out $EV/stages-b1024.json > $EV/stages-b1024.log 2>&1; echo "stages 1024 exit $?" >> $EV/ground.txt
timeout 300 $EV/api_test.green --stages --cta-budget 64 --out $EV/stages-b64.json > $EV/stages-b64.log 2>&1; echo "stages 64 exit $?" >> $EV/ground.txt
echo "finished $(date -u +%FT%TZ)" >> $EV/ground.txt
