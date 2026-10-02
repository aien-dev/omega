#!/bin/bash
# C3b: build and run the GB10 unwritten-output trap. Forge queue only; the chip
# run is never killed and never timed out. No Python.
#   tools/run_unwritten_trap.sh [harness args]   (default: --chip --repeats 3000)
#   Exit: 0 = clean (no unwritten output), 1 = hits or device error, 2 = refused / NOT_RUN.
# Thin wrapper: build, refusals, evidence receipt and verdict live in tools/chip_run.sh,
# configured by tools/manifests/unwritten_trap.chiprun. PHYSICS env (default
# ~/workspace/physics) is honoured by the module.
set -u
HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec bash "$HERE/tools/chip_run.sh" "$HERE/tools/manifests/unwritten_trap.chiprun" -- "$@"
