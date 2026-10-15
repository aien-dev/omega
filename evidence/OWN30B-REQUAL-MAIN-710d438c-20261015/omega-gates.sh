#!/bin/bash
# omega chip gates at omega main (worktree ~/workspace/omega-requal-main), pinned physics. One quietlock hold (<=20 min)
# per gate, each under /tmp/aien-gb10.lock. Nothing is killed or timed out. Evidence outside the worktree (the gate
# scripts record `git status` dirtiness), copied into the omega evidence PR afterwards.
set -u
O=$HOME/workspace/omega-requal-main
P=$HOME/workspace/pkv514-pins/physics
E=$HOME/workspace/requal-0c63/omega-gates
mkdir -p "$E"
S=$E/summary.txt
echo "omega $(git -C $O rev-parse HEAD) dirty=$(git -C $O status --porcelain | wc -l) physics $(git -C $P rev-parse HEAD) dirty=$(git -C $P status --porcelain | wc -l) start $(date -u +%FT%TZ)" >> "$S"
g() { # g NAME CMD... (run from the omega worktree)
  local name=$1; shift
  local t0; t0=$(date -u +%FT%TZ)
  ( cd "$O" && quietlock hold --owner OWN30B --minutes 20 --reason "omega gate $name at omega main" -- flock /tmp/aien-gb10.lock "$@" ) > "$E/$name.out" 2>&1
  local rc=$?
  echo "$name rc=$rc start=$t0 end=$(date -u +%FT%TZ) verdict: $(grep -E 'PASS|FAIL|FAILED_AS_EXPECTED|UNEXPECTED' "$E/$name.out" | tail -1)" >> "$S"
}
g bf16_gemv    tools/run_gpu_bf16_gemv_chip.sh   "$E/bf16_gemv"   "$P"
g cmdlist      tools/run_gpu_cmdlist_chip.sh     "$E/cmdlist"     "$P"
g decode_attn  tools/run_gpu_decode_attn_chip.sh "$E/decode_attn" "$P"
g decode_graph tools/run_gpu_decode_graph_chip.sh "$E/decode_graph" main "$P"
g decode_graph_red tools/run_gpu_decode_graph_chip.sh "$E/decode_graph_red" red "$P"
g decode_graph_cached tools/run_gpu_decode_graph_chip.sh "$E/decode_graph_cached" cached "$P"
g fp8          tools/run_gpu_fp8_chip.sh         "$E/fp8"         "$P"
g fp8_v2       tools/run_gpu_fp8_v2_chip.sh      "$E/fp8_v2"      "$P"
g fp8gemm      tools/run_gpu_fp8gemm_chip.sh     "$E/fp8gemm"     "$P"
g l2d_barrier  tools/run_gpu_l2d_barrier_chip.sh "$E/l2d_barrier" "$P"
g l2d_prefetch tools/run_gpu_l2d_prefetch_chip.sh "$E/l2d_prefetch" "$P"
g matmul_api   tools/run_gpu_matmul_api_chip.sh  "$E/matmul_api"  "$P"
g prefill_attn tools/run_gpu_prefill_attn_chip.sh "$E/prefill_attn" "$P"
g router       tools/run_gpu_router_chip.sh      "$E/router"      "$P"
g tcgemm       tools/run_gpu_tcgemm_chip.sh      "$E/tcgemm"      "$P"
g attention    env PHYSICS_DIR="$P" tools/run_gpu_attention_chip.sh   "$E/attention"
g cut4b        env PHYSICS_DIR="$P" tools/run_gpu_cut4b_chip.sh       "$E/cut4b"
g elementwise  env PHYSICS_DIR="$P" tools/run_gpu_elementwise_chip.sh "$E/elementwise"
g reconv       env PHYSICS_DIR="$P" tools/run_gpu_reconv_chip.sh      "$E/reconv"
g l1g          env PHYS="$P" OUT_DIR="$HOME/workspace/requal-0c63/l1g-build" tools/run_l1g_chip.sh "$E/l1g" qmma pfgemm qgemm
echo "end $(date -u +%FT%TZ)" >> "$S"
