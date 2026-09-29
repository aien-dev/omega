#!/usr/bin/env bash
# Preliminary sensor validation, not an R15 qualification or PASS receipt.
set -euo pipefail
cd "$(dirname "$0")"
if [[ $# != 1 || -e "$1" ]]; then
    echo "usage: collect.sh NEW-run-directory" >&2
    exit 2
fi
run_dir="$1"
mkdir -p "$(dirname "$run_dir")"
mkdir "$run_dir"
run_dir="$(realpath "$run_dir")"
work_pid=
cleanup() { if [[ -n "$work_pid" ]]; then kill "$work_pid" 2>/dev/null || true; wait "$work_pid" 2>/dev/null || true; fi; }
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
make tools check >"$run_dir/build.log" 2>&1
uname -a >"$run_dir/uname.txt"
nvidia-smi -q >"$run_dir/nvidia-smi.txt"
mokutil --sb-state >"$run_dir/secure-boot.txt"
modinfo aien_spbm_readonly.ko >"$run_dir/module.txt"
cat /sys/module/aien_spbm_readonly/srcversion >"$run_dir/loaded-srcversion.txt"
[[ $(modinfo -F srcversion aien_spbm_readonly.ko) == $(cat "$run_dir/loaded-srcversion.txt") ]]
git rev-parse HEAD >"$run_dir/base-commit.txt"
git status --short >"$run_dir/worktree.txt"
ps -eo pid,comm,pcpu --sort=-pcpu >"$run_dir/processes-before.txt"
sudo -n cat /sys/firmware/acpi/tables/DSDT >"$run_dir/DSDT.dat"
cat /sys/devices/virtual/dmi/id/bios_version >"$run_dir/bios.txt"
cat /sys/devices/virtual/dmi/id/bios_date >>"$run_dir/bios.txt"
sha256sum *.c *.h Makefile collect.sh sample load reduce aien_spbm_readonly.ko >"$run_dir/source-binary.sha256"
for round in 1 2 3; do
    ./sample "$hwmon" 10 "$run_dir/idle-$round.jsonl"
    for group in p e; do
        ./load "$group" 14 >"$run_dir/load-$group-$round.jsonl" &
        work_pid=$!
        sleep 2
        ./sample "$hwmon" 10 "$run_dir/$group-$round.jsonl"
        wait "$work_pid"
        work_pid=
    done
done
./reduce "$run_dir" >"$run_dir/summary.json"
printf '%s\n' '{"collection_complete":true,"qualified":false,"R15_PASS":false}' >"$run_dir/status.json"
(cd "$run_dir" && sha256sum *.jsonl *.json *.txt *.log *.sha256 DSDT.dat >SHA256SUMS)
