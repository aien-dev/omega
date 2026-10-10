#!/bin/sh
# AT-1 Agent 5: oracle command for the evaluator's run.sh template '<oracle> {case} {out}'.
#   sh oracle_shim.sh <case-file> <out-file>   ->   at1-oracle emit <case-file> -o <out-file>
# Exit status and standard error are the oracle's own (2 and one `AT1_CASE_REFUSED <code>` line on a
# refusal). Environment: AT1_ORACLE_BIN (set by run.sh).
set -u
: "${AT1_ORACLE_BIN:?}"
exec "$AT1_ORACLE_BIN" emit "${1:?usage: oracle_shim.sh <case-file> <out-file>}" -o "${2:?usage: oracle_shim.sh <case-file> <out-file>}"
