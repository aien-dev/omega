#!/usr/bin/env bash
# R15 clarification C2 sensor validation: idle, CPU, GPU and combined load.
# Not an R15 qualification or PASS. Loads are deliberately bounded: 4 X925
# cores (load p 14 4) and the resident GB10 seat (r15_gpu_load claims 14),
# 14 s per load period; the sampler records 10 s inside each period.
# Usage: collect-c2.sh NEW-run-directory PATH-TO-r15_gpu_load
set -euo pipefail
cd "$(dirname "$0")"
if [[ $# != 2 || -e "$1" || ! -x "$2" ]]; then
    echo "usage: collect-c2.sh NEW-run-directory r15_gpu_load-binary" >&2
    exit 2
fi
run_dir="$1"
ko="${SPBM_KO:-$PWD/aien_spbm_readonly.ko}"   # the loaded module's file
gpu_load="$(realpath "$2")"
mkdir -p "$(dirname "$run_dir")"
mkdir "$run_dir"
run_dir="$(realpath "$run_dir")"
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
hwmon=
for h in /sys/class/hwmon/hwmon*; do
    if [[ $(cat "$h/name") == aien_spbm ]]; then
        if [[ -n "$hwmon" ]]; then echo "ambiguous SPBM device" >&2; exit 1; fi
        hwmon="$h"
    fi
done
[[ -n "$hwmon" ]]
uname -a >"$run_dir/uname.txt"
nvidia-smi -q >"$run_dir/nvidia-smi.txt"
mokutil --sb-state >"$run_dir/secure-boot.txt"
cat /sys/kernel/security/lockdown >"$run_dir/lockdown.txt"
cat /sys/module/aien_spbm_readonly/srcversion >"$run_dir/loaded-srcversion.txt"
modinfo "$ko" >"$run_dir/module.txt"
[[ $(modinfo -F srcversion "$ko") == $(cat "$run_dir/loaded-srcversion.txt") ]]
git rev-parse HEAD >"$run_dir/base-commit.txt"
git status --short >"$run_dir/worktree.txt"
ps -eo pid,comm,pcpu --sort=-pcpu | head -40 >"$run_dir/processes-before.txt"
sha256sum *.c *.h Makefile collect-c2.sh sample load reduce "$ko" "$gpu_load" \
    >"$run_dir/source-binary.sha256"
start_cpu() { ./load p 14 4 >"$run_dir/load-cpu-$1.jsonl" & pids+=($!); }
start_gpu() { "$gpu_load" claims 14 >"$run_dir/load-gpu-$1.jsonl" 2>"$run_dir/load-gpu-$1.err" & pids+=($!); }
finish() { for p in "${pids[@]}"; do wait "$p"; done; pids=(); }
for round in 1 2 3; do
    ./sample "$hwmon" 10 "$run_dir/idle-$round.jsonl"
    start_cpu "$round"; sleep 2; ./sample "$hwmon" 10 "$run_dir/cpu-$round.jsonl"; finish
    start_gpu "$round"; sleep 2; ./sample "$hwmon" 10 "$run_dir/gpu-$round.jsonl"; finish
    start_cpu "b$round"; start_gpu "b$round"; sleep 2
    ./sample "$hwmon" 10 "$run_dir/both-$round.jsonl"; finish
done
./reduce "$run_dir" idle cpu gpu both >"$run_dir/summary.json"
printf '%s\n' '{"collection_complete":true,"qualified":false,"R15_PASS":false}' >"$run_dir/status.json"
(cd "$run_dir" && sha256sum *.jsonl *.json *.txt *.sha256 *.err >SHA256SUMS)
