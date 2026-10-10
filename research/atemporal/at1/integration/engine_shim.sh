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
# Environment (set by run.sh):
#   AT1_MODEL_BIN AT1_ORACLE_BIN   executables
#   AT1_PROV_STATIC   file with the provenance lines source_repo .. host, in contract order
#   AT1_COMP_DIR      optional: keep the oracle record the engine consumed, as <class>_<name>.oracle
#   AT1_CC_NOTE       optional text appended to build_cc (marks a mutant control run)
set -u
case_file=${1:?usage: engine_shim.sh <case-file> <out-file>}
out_file=${2:?usage: engine_shim.sh <case-file> <out-file>}
: "${AT1_MODEL_BIN:?}" "${AT1_ORACLE_BIN:?}" "${AT1_PROV_STATIC:?}"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/at1es.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rec=none
if "$AT1_ORACLE_BIN" emit "$case_file" -o "$tmp/oracle" > /dev/null 2> "$tmp/oracle.err"; then rec="$tmp/oracle"; fi
finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
    if [ -n "${AT1_CC_NOTE:-}" ]; then
        sed "s|^build_cc \(.*\)\$|build_cc \1 ${AT1_CC_NOTE}|" "$AT1_PROV_STATIC"
    else cat "$AT1_PROV_STATIC"; fi
    echo "run_started_utc $started"
    echo "run_finished_utc $finished"
} > "$tmp/prov"
"$AT1_MODEL_BIN" result "$case_file" "$rec" "$tmp/prov" > "$tmp/result" 2> "$tmp/err"; rc=$?
if [ $rc -ne 0 ]; then cat "$tmp/err" >&2; [ $rc -eq 2 ] && exit 2; exit 1; fi
if [ -n "${AT1_COMP_DIR:-}" ] && [ "$rec" != none ]; then
    mkdir -p "$AT1_COMP_DIR"
    cp "$rec" "$AT1_COMP_DIR/$(basename "$(dirname "$case_file")")_$(basename "$case_file" .case).oracle"
fi
cp "$tmp/result" "$out_file"
