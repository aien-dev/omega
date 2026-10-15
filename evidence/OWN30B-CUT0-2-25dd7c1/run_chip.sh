#!/bin/sh
# OWN-30B cuts 0 and 2 chip window. Run as: flock /tmp/aien-gb10.lock sh evidence/OWN30B-CUT0-2-25dd7c1/run_chip.sh
# One lock hold, one memory state: Cut 0 matmul baseline x3, attention hd128 battery (gqa8 = 32q/4kv), Cut 2 window gate x3.
set -u
E=evidence/OWN30B-CUT0-2-25dd7c1
SHAPES="1,2048,5120;1,4096,2048;1,2048,1536;1,768,2048;1,2048,12288;1,768,16384;1,2048,151936"
{
  echo "omega_sha=$(git rev-parse HEAD)"
  echo "omega_tree_dirty_lines=$(git status --porcelain | wc -l)"
  echo "physics_lock=$(cat physics.lock)"
  echo "physics_dir=$HOME/workspace/physics-own30b physics_sha=$(git -C $HOME/workspace/physics-own30b rev-parse HEAD)"
  for b in gpu_matmul_api_test gpu_attention_test gpu_moe_window_test libomega_gpu.a; do echo "binary_sha256 $b $(sha256sum build/$b | cut -d' ' -f1)"; done
  echo "settings spin_us=2000 cta_budget=1024 (the serving daemon defaults) shapes=$SHAPES"
  echo "start=$(date -u +%FT%TZ)"
} > $E/manifest.txt
{ nvidia-smi; uname -a; free -g; } > $E/device.txt 2>&1
for r in 1 2 3; do
  OMEGA_GPU_MATMUL_SHAPES="$SHAPES" ./build/gpu_matmul_api_test --timing --spin-us 2000 --cta-budget 1024 --out $E/matmul-baseline-r$r.json > $E/matmul-baseline-r$r.log 2>&1
  echo "matmul-baseline-r$r rc=$? end=$(date -u +%FT%TZ)" >> $E/manifest.txt
done
./build/gpu_attention_test --hd 128 --spin-us 2000 --out $E/attention-hd128.json > $E/attention-hd128.log 2>&1
echo "attention-hd128 rc=$? end=$(date -u +%FT%TZ)" >> $E/manifest.txt
for r in 1 2 3; do
  ./build/gpu_moe_window_test --spin-us 2000 --cta-budget 1024 --out $E/moe-window-r$r.json > $E/moe-window-r$r.log 2>&1
  echo "moe-window-r$r rc=$? end=$(date -u +%FT%TZ)" >> $E/manifest.txt
done
echo "end=$(date -u +%FT%TZ)" >> $E/manifest.txt
