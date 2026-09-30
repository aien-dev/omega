# Turing calibration (EXP-001): the one VOID counter, shared by sealed generation, the frozen-tree tests and
# the evaluator (FAILURE_REPORTING.md section 2; CAL-0 review 2 Q1). Sourced by generate_sealed_data.sh and
# record_void.sh; turing-cal-eval implements the same rule in C (refuse_c).
#
# The counter is the number of void_receipt_<n>.json files in
#     $TC_EVAL_ROOT/<C_f>/run/bundle            (TC_EVAL_ROOT default ~/aien-data/turing-cal/eval)
# the directory the sealed evaluation writes its bundle to (BLINDING_PROTOCOL.md step 8). A void receipt takes the
# first free n (exclusive create, never overwritten). The receipt with n >= 3 also writes final_receipt.json
# (kind inconclusive_infra, verdict INCONCLUSIVE, reason INFRA): EXP-001 ends there. Nothing may start once
# final_receipt.json exists or three void receipts exist.

TC_EVAL_ROOT="${TC_EVAL_ROOT:-$HOME/aien-data/turing-cal/eval}"
TC_MAX_ATTEMPTS=3

tc_void_dir() { printf '%s/%s/run/bundle\n' "$TC_EVAL_ROOT" "$1"; }

# tc_void_count DIR: number of void receipts (1, 2, ... without gaps).
tc_void_count() {
    local n=0
    while [ -e "$1/void_receipt_$((n + 1)).json" ]; do n=$((n + 1)); done
    echo "$n"
}

# tc_void_ended DIR: true when EXP-001 has ended (a final receipt exists or the attempts are used up).
tc_void_ended() { [ -e "$1/final_receipt.json" ] || [ "$(tc_void_count "$1")" -ge "$TC_MAX_ATTEMPTS" ]; }

# JSON-safe text: quote, backslash and control characters become an apostrophe (as in turing-cal-eval).
tc_jclean() { printf '%s' "$1" | tr '"\\\000-\037' "'"; }

# tc_void_write C_F STAGE STEP CODE REASON COMMAND INPUTS PROFILE_DIGEST CANDIDATE_MANIFEST_SHA256
# Writes the next void receipt; prints its number. Writes the INCONCLUSIVE (INFRA) final receipt on the third.
tc_void_write() {
    local cf="$1" stage="$2" step="$3" code="$4" reason command inputs pd="$8" cm="$9" dir n t f
    reason="$(tc_jclean "$5")" command="$(tc_jclean "$6")" inputs="$(tc_jclean "$7")"
    dir="$(tc_void_dir "$cf")"
    mkdir -p "$dir"
    t="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    n=1
    while :; do
        f="$dir/void_receipt_$n.json"
        if (set -C; : >"$f") 2>/dev/null; then break; fi
        n=$((n + 1))
        [ "$n" -lt 1000 ] || return 1
    done
    cat >>"$f" <<EOF
{
  "schema": "turing.cal.void_receipt.v1",
  "experiment": "EXP-001",
  "kind": "void",
  "stage": "$stage",
  "dry_run": false,
  "created_utc": "$t",
  "attempt": $n,
  "step": "$step",
  "code": "$code",
  "reason": "$reason",
  "command": "$command",
  "inputs": "$inputs",
  "profile_digest": "$pd",
  "freeze_commit": "$cf",
  "candidate_manifest_sha256": "$cm"
}
EOF
    if [ "$n" -ge "$TC_MAX_ATTEMPTS" ] && (set -C; : >"$dir/final_receipt.json") 2>/dev/null; then
        cat >>"$dir/final_receipt.json" <<EOF
{
  "schema": "turing.cal.terminal_receipt.v1",
  "experiment": "EXP-001",
  "kind": "inconclusive_infra",
  "dry_run": false,
  "created_utc": "$t",
  "verdict": "INCONCLUSIVE",
  "reason": "INFRA",
  "void_attempts": $n,
  "last_step": "$step",
  "last_code": "$code",
  "profile_digest": "$pd",
  "freeze_commit": "$cf",
  "candidate_manifest_sha256": "$cm",
  "EXP_001_COMPRESSION_BRIDGE": "INCONCLUSIVE"
}
EOF
    fi
    echo "$n"
}

# tc_void_first_command DIR STAGE: the command of the first void receipt of STAGE (empty if none).
tc_void_first_command() {
    local k=1 f
    while f="$1/void_receipt_$k.json" && [ -e "$f" ]; do
        if grep -q "^  \"stage\": \"$2\",\$" "$f"; then sed -n 's/^  "command": "\(.*\)",$/\1/p' "$f"; return 0; fi
        k=$((k + 1))
    done
}
