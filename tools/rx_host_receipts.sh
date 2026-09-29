#!/bin/bash
# rx_host_receipts.sh -- summarise and check the resident-reaction receipts in
# the "Emit qualification receipt" step of .github/workflows/rx-host.yml.
#
#   tools/rx_host_receipts.sh heartbeat  RECEIPT            (R1-R6 heartbeat)
#   tools/rx_host_receipts.sh native     RECEIPT CANDIDATE  (R7 native authority)
#   tools/rx_host_receipts.sh generation RECEIPT CANDIDATE  (R9 generation barrier)
#
# Prints the summary lines to stdout (Python-style True/False/None, as the
# retired inline python3 blocks did). native and generation then check the
# receipt is bound to CANDIDATE, came from a clean tree and has a non-empty,
# matching Linux/native differential; generation also needs >= 8 coherent
# crash recoveries covering lineages 1 and 2, each GENESIS or CANDIDATE.
# A failed check prints its reason on stderr and exits 1. Shell + jq +
# sha256sum; no Python.
set -u

# Python str() of a JSON value for the summary lines.
PY='def py: if . == true then "True" elif . == false then "False"
            elif . == null then "None" elif type == "string" then . else tojson end;
    def req($k): if type == "object" and has($k) then .[$k]
                 else error("receipt has no \"\($k)\"") end;
    def falsy: . == null or . == false or . == 0 or . == "" or . == [] or . == {};'

die() { echo "$1" >&2; exit 1; }

summary() { jq -r "$PY $1" "$2" || exit 1; }

# check PROGRAM FILE CANDIDATE -- PROGRAM yields "" or a failure reason.
check() {
    local msg
    msg=$(jq -r --arg candidate "$3" "$PY $1" "$2") || exit 1
    [ -z "$msg" ] || die "$msg"
}

mode=${1:?usage: rx_host_receipts.sh heartbeat|native|generation RECEIPT [CANDIDATE]}
path=${2:?receipt path required}
[ -r "$path" ] || die "cannot read $path"
jq empty "$path" || exit 1

case $mode in
heartbeat)
    summary '"test totals: tests=\(req("totals") | req("tests") | py) passed=\(.totals | req("tests_passed") | py) checks=\(.totals | req("checks") | py) failures=\(.totals | req("failures") | py)"' "$path"
    echo "receipt: $path"
    echo "receipt hash: $(sha256sum < "$path" | cut -d' ' -f1)"
    summary 'req("gates") as $g
        | ("R1_CANONICAL_SHARED_WORLD_PASS", "R2_CROSS_ENGINE_ABI_PASS",
           "R3_REACTION_CORE", "R4_CAUSAL_TRACE",
           "R5_RESOURCE_ARBITRATION", "R6_REACTION_STABILITY") as $n
        | "gate \($n): \(if ($g | has($n)) then ($g[$n] | py) else "missing" end)"' "$path"
    summary '"not-claimed gates: \(.gates.not_claimed // [] | join(", "))",
             "hardware scope: \(.hardware_scope | py)"' "$path"
    ;;
native)
    candidate=${3:?candidate commit required}
    echo "native receipt: $path"
    echo "native receipt hash: $(sha256sum < "$path" | cut -d' ' -f1)"
    summary '"native candidate: \(.candidate_commit | py)",
             "native candidate bound: \(.candidate_bound | py)",
             "native tree dirty: \(.tree_dirty | py)",
             "native aienos: \(.aienos_commit | py)",
             "native gate: \(req("gates") | .R7_NATIVE_AUTHORITY | py)",
             "native differential rows: \(.differential // [] | length)"' "$path"
    check '(.differential // []) as $d
        | if .candidate_commit != $candidate or .candidate_bound != true
          then "native receipt is not bound to this candidate"
          elif .tree_dirty != false
          then "native receipt was produced from a dirty tree"
          elif ($d | length) == 0 or any($d[]; .linux != .native)
          then "native differential is empty or mismatched"
          else "" end' "$path" "$candidate"
    ;;
generation)
    candidate=${3:?candidate commit required}
    echo "generation receipt: $path"
    summary '"generation candidate bound: \(.candidate_bound | py)",
             "generation gate: \(req("gates") | .R9_GENERATION_BARRIER | py)",
             "generation crash rows: \(.crashes // [] | length)"' "$path"
    check '(.differential // []) as $d | (.crashes // []) as $c
        | if .candidate_commit != $candidate or .candidate_bound != true
          then "generation receipt is not bound to this candidate"
          elif .tree_dirty != false or .failures != 0
          then "generation receipt was not a clean passing run"
          elif ($d | length) == 0 or any($d[]; .linux != .native)
          then "generation differential is empty or mismatched"
          elif ($c | length) < 8 or ([$c[] | .lineage] | unique) != [1, 2]
          then "generation crash recoveries did not cover both generations"
          elif any($c[]; .coherent | falsy)
          then "a crash recovery was not coherent"
          elif any($c[]; .evidence != "GENESIS" and .evidence != "CANDIDATE")
          then "a crash recovery mixed generations"
          else "" end' "$path" "$candidate"
    ;;
*) die "unknown receipt kind: $mode" ;;
esac
