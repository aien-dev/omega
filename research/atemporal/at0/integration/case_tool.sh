#!/bin/sh
# AT-0 Agent 5: codec-only front for the evaluator's AT0_CANDIDATE_CASE_TOOL hook.
# Prints `AT0_CASE_OK <case_id>` (exit 0) or `AT0_CASE_REFUSED <code>` on stderr (exit 2),
# using the candidate model's own parser. sh case_tool.sh <case-file>
set -u
: "${AT0_MODEL_BIN:?}"
out=$("$AT0_MODEL_BIN" "${1:?usage: case_tool.sh <case-file>}" 2>/tmp/at0ct.$$.err); rc=$?
if [ $rc -eq 0 ]; then echo "AT0_CASE_OK $(printf '%s\n' "$out" | sed -n 's/^case_id //p' | head -1)"; rm -f /tmp/at0ct.$$.err; exit 0; fi
cat /tmp/at0ct.$$.err >&2; rm -f /tmp/at0ct.$$.err; exit $rc
