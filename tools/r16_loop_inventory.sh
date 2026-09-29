#!/bin/sh
# R16-G2 code-search gate, the entry point named in spec/r16-orchestrator-retirement.md.
# Builds the C tool (tools/r16_loop_inventory.c) if needed and runs it from the omega
# root. Arguments pass through (--map, --json, --skeleton, --patterns). Exit status is
# the tool's: 0 PASS, 1 FAIL, 2 usage/I-O error.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${OUT_DIR:-build}
make -s "$OUT/r16_loop_inventory" >/dev/null
exec "$OUT/r16_loop_inventory" "$@"
