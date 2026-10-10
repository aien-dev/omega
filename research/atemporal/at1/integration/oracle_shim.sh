#!/bin/sh
# AT-1 Agent 5: oracle command for the evaluator's run.sh template '<oracle> {case} {out}'.
#   sh oracle_shim.sh <case-file> <out-file>   ->   at1-oracle emit <case-file> -o <out-file>
# Exit status and standard error are the oracle's own (2 and one `AT1_CASE_REFUSED <code>` line on a
# refusal); a usage or setup error here exits 1. Environment: AT1_ORACLE_BIN (set by run.sh).
set -u
if [ $# -ne 2 ]; then echo "usage: oracle_shim.sh <case-file> <out-file>" >&2; exit 1; fi
if [ -z "${AT1_ORACLE_BIN:-}" ]; then echo "oracle_shim.sh: AT1_ORACLE_BIN is not set" >&2; exit 1; fi
exec "$AT1_ORACLE_BIN" emit "$1" -o "$2"
