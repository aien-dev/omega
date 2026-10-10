#!/bin/sh
# AT-1 Agent 5: engine command for the evaluator's run.sh template '<engine> {case} {out}'.
#   sh engine_shim.sh <case-file> <out-file>
# Runs the oracle on the case, writes the provenance file (the engine reads no clock), then
#   at1-model result <case> <oracle-record|none> <provenance>
# which splices every reference_* line of the record in values-block order (charter reading (c))
# and writes the complete AT1_RESULT_V1 file to <out-file>.
# Exit 0 written; 2 case refused (the engine's own `AT1_CASE_REFUSED <code>` line on stderr, nothing
# written); 1 any other failure. The case is parsed by the engine before the record and the
# provenance are opened, so an oracle failure can never mask an engine refusal.
# An oracle failure other than a refusal (exit 2) is copied to stderr; the engine then runs with
# record "none" and its own verdict decides.
# Environment (set by run.sh):
#   AT1_MODEL_BIN AT1_ORACLE_BIN   executables
#   AT1_PROV_STATIC   file with the provenance lines source_repo .. host, in contract order
#   AT1_COMP_DIR      optional: keep the oracle record the engine consumed, as <class>_<name>.oracle
#                     (first write wins, so the evaluator's T9 rerun cannot replace it)
set -u
if [ $# -ne 2 ]; then echo "usage: engine_shim.sh <case-file> <out-file>" >&2; exit 1; fi
case_file=$1
out_file=$2
for v in AT1_MODEL_BIN AT1_ORACLE_BIN AT1_PROV_STATIC; do
    eval "val=\${$v:-}"
    if [ -z "$val" ]; then echo "engine_shim.sh: $v is not set" >&2; exit 1; fi
done
tmp=$(mktemp -d "${TMPDIR:-/tmp}/at1es.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rec=none
"$AT1_ORACLE_BIN" emit "$case_file" -o "$tmp/oracle" > /dev/null 2> "$tmp/oracle.err"; orc=$?
if [ $orc -eq 0 ]; then rec="$tmp/oracle"; fi
finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
    cat "$AT1_PROV_STATIC"
    echo "run_started_utc $started"
    echo "run_finished_utc $finished"
} > "$tmp/prov"
"$AT1_MODEL_BIN" result "$case_file" "$rec" "$tmp/prov" > "$tmp/result" 2> "$tmp/err"; rc=$?
if [ $rc -ne 0 ]; then cat "$tmp/err" >&2; [ $rc -eq 2 ] && exit 2; exit 1; fi
if [ $orc -ne 0 ] && [ $orc -ne 2 ]; then
    echo "engine_shim.sh: oracle exit $orc on $case_file (record none):" >&2
    cat "$tmp/oracle.err" >&2
fi
if [ -n "${AT1_COMP_DIR:-}" ] && [ "$rec" != none ]; then
    mkdir -p "$AT1_COMP_DIR" || exit 1
    comp="$AT1_COMP_DIR/$(basename "$(dirname "$case_file")")_$(basename "$case_file" .case).oracle"
    [ -e "$comp" ] || cp "$rec" "$comp" || exit 1
fi
cp "$tmp/result" "$out_file"
