#!/bin/sh
# AT-0 Agent 5: run ONE case end to end and write one complete AT0_RESULT_V2 file to stdout.
#   sh candidate_run.sh <case-file>
# model -> engine component output; oracle -> record (only its `reference` lines are used);
# at0-assemble -> full result (exact judge, verdict_id, provenance, evidence_digest).
# Exit 0 result written; 2 case refused (AT0_CASE_REFUSED <code> on stderr, nothing on stdout);
# 1 any other failure. Environment (all set by run.sh):
#   AT0_MODEL_BIN AT0_ORACLE_BIN AT0_ASM_BIN   executables
#   AT0_PROV_STATIC   file with the fixed provenance lines (source_repo .. host)
#   AT0_COMP_DIR      optional: keep the component outputs here (<case>.engine, <case>.oracle)
#   AT0_CC_NOTE       optional text appended to build_cc (marks a mutant control run)
set -u
case_file=${1:?usage: candidate_run.sh <case-file>}
: "${AT0_MODEL_BIN:?}" "${AT0_ORACLE_BIN:?}" "${AT0_ASM_BIN:?}" "${AT0_PROV_STATIC:?}"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/at0cr.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
name=$(basename "$case_file" .case)
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
"$AT0_MODEL_BIN" "$case_file" > "$tmp/engine" 2> "$tmp/engine.err"; rc=$?
if [ $rc -eq 2 ]; then cat "$tmp/engine.err" >&2; exit 2; fi
if [ $rc -ne 0 ]; then echo "model failed rc=$rc: $(head -1 "$tmp/engine.err")" >&2; exit 1; fi
"$AT0_ORACLE_BIN" emit "$case_file" -o "$tmp/oracle" 2> "$tmp/oracle.err"; rc=$?
if [ $rc -ne 0 ]; then echo "oracle failed rc=$rc on a case the model accepted: $(head -1 "$tmp/oracle.err")" >&2; exit 1; fi
finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)
{
    if [ -n "${AT0_CC_NOTE:-}" ]; then
        sed "s|^build_cc \(.*\)\$|build_cc \1 ${AT0_CC_NOTE}|" "$AT0_PROV_STATIC"
    else cat "$AT0_PROV_STATIC"; fi
    echo "run_started_utc $started"
    echo "run_finished_utc $finished"
} > "$tmp/prov"
"$AT0_ASM_BIN" assemble "$case_file" "$tmp/engine" "$tmp/oracle" "$tmp/prov" > "$tmp/result" 2> "$tmp/asm.err"; rc=$?
cat "$tmp/asm.err" >&2
if [ $rc -ne 0 ]; then exit 1; fi
if [ -n "${AT0_COMP_DIR:-}" ]; then mkdir -p "$AT0_COMP_DIR"; cp "$tmp/engine" "$AT0_COMP_DIR/$name.engine"; cp "$tmp/oracle" "$AT0_COMP_DIR/$name.oracle"; fi
cat "$tmp/result"
