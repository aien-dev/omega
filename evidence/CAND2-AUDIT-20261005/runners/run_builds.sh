#!/usr/bin/env bash
cd /home/drakestapleton/workspace/cand3-campaign/cand2
./cand2_build.sh A /home/drakestapleton/workspace/cand3-campaign/cand2/rootA 79a805d162bfded8c5ce5a4c14f7c29e79025f39 bbad5e4250e57f8cbd1be4cf1390109aa65ef92c 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf 286fa9b7afbc71f06ed7e1dd29f68f36714fd92a > buildA.out 2>&1; echo "A exit $?" >> builds.done
./cand2_build.sh B /home/drakestapleton/workspace/cand3-campaign/cand2/rootB/deeper/path-two 79a805d162bfded8c5ce5a4c14f7c29e79025f39 bbad5e4250e57f8cbd1be4cf1390109aa65ef92c 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf 286fa9b7afbc71f06ed7e1dd29f68f36714fd92a > buildB.out 2>&1; echo "B exit $?" >> builds.done
