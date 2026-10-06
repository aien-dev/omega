#!/usr/bin/env bash
cd /home/drakestapleton/workspace/cand3-campaign/cand3
OM=f816473df391bc4fbcf99df722edd70ed26ebef1; AO=bbad5e4250e57f8cbd1be4cf1390109aa65ef92c; PH=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf; SC=80e071a3ef700dbe31b1b081a957eebdcbf5ffc3
quietlock check || { echo "quiet flag held" >> builds.done; exit 9; }
./cand3_build.sh A $PWD/rootA $OM $AO $PH $SC > buildA.out 2>&1; echo "A exit $?" >> builds.done
./cand3_build.sh B $PWD/rootB/deeper/path-two $OM $AO $PH $SC > buildB.out 2>&1; echo "B exit $?" >> builds.done
